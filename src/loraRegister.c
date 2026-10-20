//
// FILE            loraRegister.c
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
// The iotalora bridge - the SEAM: what the broker calls, and the one symbol it looks for (bridgeRegister).
// The contract is corBridge's BridgeDriver.h (broker -> plugin) and BridgeBroker.h (plugin -> broker).
//
// The IoT Agent LoRaWAN: network servers' uplinks as DataChannel records - a DataChannel's endpoint is
// <server>/<applicationId>[/<deviceId>] - and with "channelDirection": "both" a written attribute is a
// downlink (loraServer.c). A Channel or an AssetChannel is refused - this bridge carries DataChannels only.
//
#include <stdbool.h>                                  // bool
#include <stdio.h>                                    // fopen, fread, fseek, ftell, snprintf
#include <stdlib.h>                                   // malloc, free

#include "corLog/corLog.h"                            // COR_E, COR_W
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeDriver.h"                   // BridgeDriver, BridgeChannelKind, BridgeDirection
#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "loraServer.h"                               // loraServers*, loraEndpoint*, loraPublish, loraVersion



static const BridgeBroker* hostP = NULL;



// -----------------------------------------------------------------------------
//
// configLoad - the whole --bridgeConfig file, NUL-terminated; malloc'd. NULL: not readable
//
static char* configLoad(const char* path)
{
  FILE* fP = fopen(path, "r");
  if (fP == NULL)
    return NULL;

  char* text = NULL;
  long  size = (fseek(fP, 0, SEEK_END) == 0) ? ftell(fP) : -1;

  if ((size >= 0) && (fseek(fP, 0, SEEK_SET) == 0) && ((text = malloc(size + 1)) != NULL))
  {
    if (fread(text, 1, size, fP) == (size_t) size)
      text[size] = 0;
    else
    {
      free(text);
      text = NULL;
    }
  }

  fclose(fP);
  return text;
}



// -----------------------------------------------------------------------------
//
// iotaloraInit - the network servers, from the --bridgeConfig file's member of this plugin:
//
//   { "iotalora": { "servers": { "ttn": { "kind": "ttn", "uri": "mqtts://eu1.cloud.thethings.network:8883",
//                                         "username": "my-app@ttn", "passwordEnv": "TTN_API_KEY" } },
//                   "ngsild": { "dataChannels": { "ttn/my-app@ttn": { ... } } } } }
//
static int iotaloraInit(const char* configFile, const BridgeBroker* brokerP)
{
  CorAlloc  ka;
  CorJson   cj;
  char      buf[16 * 1024];
  CorNode*  serversP = NULL;
  char*     text     = (configFile != NULL) ? configLoad(configFile) : NULL;

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotalora config");
  corJsonCreate(&cj, &ka);

  if (text != NULL)
  {
    CorNode* rootP   = corJsonParse(&cj, text);       // valid JSON: the broker has parsed the same file
    CorNode* bridgeP = (rootP != NULL) ? corTreeLookup(rootP, "iotalora") : NULL;

    serversP = (bridgeP != NULL) ? corTreeLookup(bridgeP, "servers") : NULL;
  }

  hostP = brokerP;

  int r = loraServersOpen(serversP, brokerP);

  corAllocBufferReset(&ka, false);                    // nothing parsed here is kept: the servers have copies
  free(text);
  return r;
}



// -----------------------------------------------------------------------------
//
// iotaloraClose - must not return until no thread of the plugin can call the broker
//
static void iotaloraClose(void)
{
  loraServersClose();
}



// -----------------------------------------------------------------------------
//
// The DataChannels - each an application of a server, or one device of it. A broker of ABI 15 says which channel type an endpoint is
// (channelTypedAdd); an older one hands every channel over alike (channelAddInfo), and each is taken as a
// DataChannel.
//
static int iotaloraChannelAddInfo(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  (void) kind;
  (void) direction;                                   // in: the uplinks; both: also downlinks

  int r = loraEndpointAdd(endpoint, info);

  if (r == BRIDGE_BAD_INPUT)
    COR_W("iotalora: DataChannel '%s' refused - its endpoint is <server>/<applicationId>[/<deviceId>]", endpoint);

  return r;
}

#if BRIDGE_ABI_VERSION >= 15
static int iotaloraChannelTypedAdd(const char* endpoint, BridgeChannelType type, BridgeChannelKind kind, BridgeDirection direction, const char* info)
{
  if (type == BridgeChannelTypeDataChannel)
    return iotaloraChannelAddInfo(endpoint, kind, direction, info);

  const char* reason = "the iotalora bridge carries DataChannels only - an uplink is a record";

  if ((hostP != NULL) && (hostP->abiVersion >= 12) && (hostP->channelStatusIn != NULL))
    hostP->channelStatusIn("iotalora", endpoint, BridgeChannelDormant, reason);

  COR_W("iotalora: '%s' refused - %s", endpoint, reason);
  return BRIDGE_UNSUPPORTED;
}
#endif

static int iotaloraChannelAdd(const char* endpoint, BridgeChannelKind kind, BridgeDirection direction)
{
  return iotaloraChannelAddInfo(endpoint, kind, direction, NULL);
}

static int iotaloraChannelDel(const char* endpoint)
{
  return loraEndpointDel(endpoint);
}

static int iotaloraPublish(const char* endpoint, const char* json)
{
  return loraPublish(endpoint, json);
}



// -----------------------------------------------------------------------------
//
// iotaloraVersionInfo - one line for GET /version: the plugin, and what it links
//
static const char* iotaloraVersionInfo(void)
{
  static char info[128];

  snprintf(info, sizeof(info), "iotalora 0.1.0 (%s)", loraVersion());
  return info;
}



// -----------------------------------------------------------------------------
//
// bridgeRegister - the one symbol the broker looks for, and the one this .so exports
//
// A slot is filled only when the broker's abiVersion has it; abiVersion is then overwritten with this
// plugin's. Which channel type an endpoint is, ABI 15: before it, every channel is taken as a DataChannel.
//
__attribute__((visibility("default"))) void bridgeRegister(BridgeDriver* driverP)
{
  if (driverP->abiVersion < 1)
    driverP->abiVersion = 1;                          // a host from before the handshake

  driverP->alias       = "iotalora";                  // --bridges iotalora, and its member of --bridgeConfig
  driverP->version     = "0.1.0";
  driverP->args        = NULL;
  driverP->init        = iotaloraInit;
  driverP->close       = iotaloraClose;
  driverP->versionInfo = iotaloraVersionInfo;
  driverP->channelAdd  = iotaloraChannelAdd;
  driverP->channelDel  = iotaloraChannelDel;
  driverP->publish     = iotaloraPublish;

  if (driverP->abiVersion >= 9)
    driverP->channelAddInfo = iotaloraChannelAddInfo;

#if BRIDGE_ABI_VERSION >= 15
  if (driverP->abiVersion >= 15)
    driverP->channelTypedAdd = iotaloraChannelTypedAdd;
#endif

  driverP->abiVersion  = BRIDGE_ABI_VERSION;
}
