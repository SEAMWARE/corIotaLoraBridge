//
// FILE            loraServer.c
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
// LoRaWAN network servers over MQTT - see header.
//
// Threads: each server's libmosquitto thread takes its uplinks and hands them to the broker (sampleIn);
// publish() runs on a broker thread and only queues the downlink (mosquitto_publish does not block).
//
#include <pthread.h>                                  // pthread_mutex_*
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // int64_t
#include <stdio.h>                                    // snprintf, sscanf
#include <stdlib.h>                                   // malloc, calloc, free, atoi, getenv
#include <string.h>                                   // strcmp, strncmp, strchr, strdup, strlen
#include <time.h>                                     // struct tm, timegm

#include <mosquitto.h>                                // mosquitto_*

#include "corLog/corLog.h"                            // COR_E, COR_W, COR_I, COR_V
#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corAlloc/corAllocBufferInit.h"              // corAllocBufferInit
#include "corAlloc/corAllocBufferReset.h"             // corAllocBufferReset
#include "corJson/CorJson.h"                          // CorJson
#include "corJson/corJsonCreate.h"                    // corJsonCreate
#include "corJson/corJsonParse.h"                     // corJsonParse
#include "corJson/corJsonRender.h"                    // corJsonFastRender
#include "corJson/corJsonRenderSize.h"                // corJsonFastRenderSize
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeClone.h"                     // corTreeClone
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corBridge/BridgeBroker.h"                   // BridgeBroker, BRIDGE_*

#include "loraCodec.h"                                // loraBase64*, loraLpp*, loraCborDecode, loraFlatten
#include "loraServer.h"                               // Own interface



// -----------------------------------------------------------------------------
//
// ServerKind - which network server: their topics and their JSON differ
//
typedef enum ServerKind
{
  ServerTtn        = 0,                               // The Things Stack v3 - v3/<app>/devices/<device_id>/up
  ServerChirpstack = 1                                // ChirpStack v4 - application/<app>/device/<devEui>/event/up
} ServerKind;



// -----------------------------------------------------------------------------
//
// DataModel - what a frame's payload is
//
typedef enum DataModel
{
  ModelLpp     = 0,                                   // cayennelpp - iotagent-lorawan's default
  ModelCbor    = 1,                                   // cbor
  ModelServer  = 2                                    // application_server - the network server's codec decoded it
} DataModel;



// -----------------------------------------------------------------------------
//
// Server - one network server, its connection
//
typedef struct Server
{
  char               name[64];
  ServerKind         kind;
  char               desc[300];                       // host:port, for the log
  struct mosquitto*  mosq;
  bool               connected;
  struct Server*     next;
} Server;



// -----------------------------------------------------------------------------
//
// Endpoint - a DataChannel's: its server, application, device (NULL: every device of the application)
//
typedef struct Endpoint
{
  char*             endpoint;
  Server*           serverP;
  char*             app;
  char*             device;
  DataModel         model;
  int               fPort;
  struct Endpoint*  next;
} Endpoint;



static const BridgeBroker*  brokerP     = NULL;
static Server*              serverList  = NULL;
static pthread_mutex_t      mutex       = PTHREAD_MUTEX_INITIALIZER;
static Endpoint*            endpointList = NULL;



// -----------------------------------------------------------------------------
//
// topicOf - the uplink topic of an application, every device (+)
//
static void topicOf(Server* sP, const char* app, char* topic, int size)
{
  if (sP->kind == ServerTtn)
    snprintf(topic, size, "v3/%s/devices/+/up", app);
  else
    snprintf(topic, size, "application/%s/device/+/event/up", app);
}



// -----------------------------------------------------------------------------
//
// appCarried - whether an endpoint other than skipP has the server's application; under the mutex
//
static bool appCarried(Server* sP, const char* app, Endpoint* skipP)
{
  for (Endpoint* eP = endpointList; eP != NULL; eP = eP->next)
  {
    if ((eP != skipP) && (eP->serverP == sP) && (strcmp(eP->app, app) == 0))
      return true;
  }

  return false;
}



// -----------------------------------------------------------------------------
//
// isoParse - ISO 8601 as epoch nanoseconds - YYYY-MM-DDThh:mm:ss[.frac](Z|+hh:mm|-hh:mm); 0: not that
//
static int64_t isoParse(const char* s)
{
  struct tm tm;
  int       n = 0;

  memset(&tm, 0, sizeof(tm));

  if ((sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &n) != 6) || (n != 19))
    return 0;

  const char* cP     = &s[n];
  int64_t     frac   = 0;
  int         digits = 0;

  if (*cP == '.')
  {
    for (cP++; (*cP >= '0') && (*cP <= '9'); cP++)
    {
      if (digits < 9)
      {
        frac = frac * 10 + (*cP - '0');
        digits++;
      }
    }

    for (; digits < 9; digits++)
      frac *= 10;
  }

  int offset = 0;

  if ((*cP == '+') || (*cP == '-'))
  {
    int hh, mm;

    if (sscanf(cP + 1, "%2d:%2d", &hh, &mm) != 2)
      return 0;

    offset = ((*cP == '+') ? 1 : -1) * (hh * 3600 + mm * 60);
  }
  else if (*cP != 'Z')
    return 0;

  tm.tm_year -= 1900;
  tm.tm_mon  -= 1;

  return ((int64_t) timegm(&tm) - offset) * 1000000000LL + frac;
}



// -----------------------------------------------------------------------------
//
// stringAt - a string member at a path of objects ("uplink_message", "received_at"), NULL if none
//
static const char* stringAt(CorNode* nodeP, const char* a, const char* b)
{
  if ((nodeP != NULL) && (a != NULL))
    nodeP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, a) : NULL;
  if ((nodeP != NULL) && (b != NULL))
    nodeP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, b) : NULL;

  return ((nodeP != NULL) && (nodeP->type == CorString)) ? nodeP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// nodeAt - a member at a path of objects, NULL if none
//
static CorNode* nodeAt(CorNode* nodeP, const char* a, const char* b)
{
  if ((nodeP != NULL) && (a != NULL))
    nodeP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, a) : NULL;
  if ((nodeP != NULL) && (b != NULL))
    nodeP = (nodeP->type == CorObject) ? corTreeLookup(nodeP, b) : NULL;

  return nodeP;
}



// -----------------------------------------------------------------------------
//
// Uplink - what an uplink says, whichever network server sent it
//
typedef struct Uplink
{
  const char*  app;
  const char*  deviceId;                              // TTN device_id, ChirpStack DevEUI (the topic's)
  const char*  devEui;
  const char*  deviceName;                            // ChirpStack's
  CorNode*     fPortP;
  const char*  payload;                               // base64: TTN frm_payload, ChirpStack data
  CorNode*     decodedP;                              // TTN decoded_payload, ChirpStack object
  int64_t      time;
} Uplink;



// -----------------------------------------------------------------------------
//
// uplinkRead - the topic's levels and the message's JSON as an Uplink; false: not an uplink
//
static bool uplinkRead(Server* sP, char** levelV, int levels, CorNode* msgP, Uplink* uP)
{
  memset(uP, 0, sizeof(Uplink));

  if (sP->kind == ServerTtn)
  {
    //
    // v3/<app>/devices/<device_id>/up: { end_device_ids: { device_id, dev_eui }, received_at,
    //   uplink_message: { f_port, frm_payload, decoded_payload, received_at } }
    //
    if ((levels != 5) || (strcmp(levelV[0], "v3") != 0) || (strcmp(levelV[2], "devices") != 0) || (strcmp(levelV[4], "up") != 0))
      return false;

    uP->app      = levelV[1];
    uP->deviceId = levelV[3];
    uP->devEui   = stringAt(msgP, "end_device_ids", "dev_eui");
    uP->fPortP   = nodeAt(msgP, "uplink_message", "f_port");
    uP->payload  = stringAt(msgP, "uplink_message", "frm_payload");
    uP->decodedP = nodeAt(msgP, "uplink_message", "decoded_payload");

    const char* t = stringAt(msgP, "uplink_message", "received_at");

    if (t == NULL)
      t = stringAt(msgP, "received_at", NULL);
    uP->time = (t != NULL) ? isoParse(t) : 0;
    return true;
  }

  //
  // application/<app>/device/<devEui>/event/up: { time, deviceInfo: { deviceName, devEui }, fPort, data, object }
  // - and ChirpStack v3's members, devEUI and deviceName at the top
  //
  if ((levels != 6) || (strcmp(levelV[0], "application") != 0) || (strcmp(levelV[2], "device") != 0) ||
      (strcmp(levelV[4], "event") != 0) || (strcmp(levelV[5], "up") != 0))
    return false;

  uP->app        = levelV[1];
  uP->deviceId   = levelV[3];
  uP->devEui     = levelV[3];
  uP->deviceName = stringAt(msgP, "deviceInfo", "deviceName");
  if (uP->deviceName == NULL)
    uP->deviceName = stringAt(msgP, "deviceName", NULL);
  uP->fPortP     = nodeAt(msgP, "fPort", NULL);
  uP->payload    = stringAt(msgP, "data", NULL);
  uP->decodedP   = nodeAt(msgP, "object", NULL);

  const char* t = stringAt(msgP, "time", NULL);

  uP->time = (t != NULL) ? isoParse(t) : 0;
  return true;
}



// -----------------------------------------------------------------------------
//
// endpointOf - the DataChannel an uplink goes to: its device's, else its application's; its name and model
// copied out (the list may change once the mutex is let go). false: none
//
static bool endpointOf(Server* sP, const char* app, const char* deviceId, char* endpoint, int size, DataModel* modelP)
{
  Endpoint* foundP = NULL;

  pthread_mutex_lock(&mutex);

  for (Endpoint* eP = endpointList; (eP != NULL) && (foundP == NULL); eP = eP->next)
  {
    if ((eP->serverP == sP) && (strcmp(eP->app, app) == 0) && (eP->device != NULL) && (strcmp(eP->device, deviceId) == 0))
      foundP = eP;
  }

  for (Endpoint* eP = endpointList; (eP != NULL) && (foundP == NULL); eP = eP->next)
  {
    if ((eP->serverP == sP) && (strcmp(eP->app, app) == 0) && (eP->device == NULL))
      foundP = eP;
  }

  if (foundP != NULL)
  {
    snprintf(endpoint, size, "%s", foundP->endpoint);
    *modelP = foundP->model;
  }

  pthread_mutex_unlock(&mutex);
  return (foundP != NULL);
}



// -----------------------------------------------------------------------------
//
// uplinkIn - one uplink: decoded by its DataChannel's model, made flat, handed over as a record
//
static void uplinkIn(Server* sP, const char* topic, const char* payload, int len)
{
  char  topicCopy[512];
  char* levelV[8];
  int   levels = 0;

  snprintf(topicCopy, sizeof(topicCopy), "%s", topic);

  for (char* s = topicCopy; (s != NULL) && (levels < 8); )
  {
    levelV[levels++] = s;
    s = strchr(s, '/');
    if (s != NULL)
      *s++ = 0;
  }

  CorAlloc ka;
  CorJson  cj;
  char     buf[8 * 1024];

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotalora uplink");
  corJsonCreate(&cj, &ka);

  char*    text = corAlloc(&ka, len + 1);
  CorNode* msgP = NULL;

  if (text != NULL)
  {
    memcpy(text, payload, len);
    text[len] = 0;
    msgP = corJsonParse(&cj, text);
  }

  Uplink    up;
  char      endpoint[512];
  DataModel model;

  if ((msgP == NULL) || (msgP->type != CorObject) || (uplinkRead(sP, levelV, levels, msgP, &up) == false))
    COR_W("iotalora: %s: '%s' is not an uplink - dropped", sP->name, topic);
  else if (endpointOf(sP, up.app, up.deviceId, endpoint, sizeof(endpoint), &model) == false)
    COR_V("iotalora: %s: an uplink of %s/%s - no DataChannel has it", sP->name, up.app, up.deviceId);
  else
  {
    //
    // The decoded payload: the network server's, else the frame decoded here
    //
    CorNode*    decodedP = NULL;
    const char* why      = NULL;

    if (model == ModelServer)
    {
      decodedP = up.decodedP;
      if (decodedP == NULL)
        why = "no decoded payload - the network server has no payload formatter (codec) for it";
    }
    else if (up.payload == NULL)
      why = "no frame payload";
    else
    {
      int            frameLen;
      unsigned char* frame = loraBase64Decode(&ka, up.payload, &frameLen);

      if (frame == NULL)
        why = "the frame payload is not base64";
      else if (model == ModelLpp)
      {
        decodedP = corTreeObject(&ka, NULL);
        why      = loraLppDecode(&ka, frame, frameLen, decodedP);
      }
      else
        decodedP = loraCborDecode(&ka, frame, frameLen, &why);
    }

    if ((why == NULL) && ((decodedP == NULL) || (decodedP->type != CorObject)))
      why = "the decoded payload is not an object";

    CorNode* recordP = (why == NULL) ? corTreeObject(&ka, NULL) : NULL;

    if ((recordP != NULL) && (loraFlatten(&ka, recordP, decodedP, NULL) == true))
    {
      corTreeChildAdd(recordP, corTreeString(&ka, "@deviceId", up.deviceId));
      corTreeChildAdd(recordP, corTreeString(&ka, "@applicationId", up.app));
      if (up.devEui != NULL)
        corTreeChildAdd(recordP, corTreeString(&ka, "@devEui", up.devEui));
      if (up.deviceName != NULL)
        corTreeChildAdd(recordP, corTreeString(&ka, "@deviceName", up.deviceName));
      if ((up.fPortP != NULL) && (up.fPortP->type == CorInt))
        corTreeChildAdd(recordP, corTreeInteger(&ka, "@fPort", up.fPortP->value.i));

      int   size = corJsonFastRenderSize(recordP);
      char* json = malloc(size + 1);

      if (json != NULL)
      {
        corJsonFastRender(recordP, json);

        int r = brokerP->sampleIn("iotalora", endpoint, json, up.time);

        COR_V("iotalora: record on %s (%d): %s", endpoint, r, json);
        free(json);
      }
    }
    else if (why != NULL)
      COR_W("iotalora: %s: an uplink of %s/%s dropped - %s", sP->name, up.app, up.deviceId, why);
  }

  corAllocBufferReset(&ka, false);
}



// -----------------------------------------------------------------------------
//
// onMessage / onConnect / onDisconnect - a server's libmosquitto thread; it reconnects by itself
//
static void onMessage(struct mosquitto* m, void* userdata, const struct mosquitto_message* msgP)
{
  (void) m;
  uplinkIn((Server*) userdata, msgP->topic, (const char*) msgP->payload, msgP->payloadlen);
}

static void onConnect(struct mosquitto* m, void* userdata, int rc)
{
  Server* sP = (Server*) userdata;

  if (rc != 0)
  {
    COR_W("iotalora: %s (%s) refused the connection: %s", sP->name, sP->desc, mosquitto_connack_string(rc));
    return;
  }

  sP->connected = true;

  //
  // Every application a DataChannel has - again after a reconnect: a clean session forgets them
  //
  pthread_mutex_lock(&mutex);
  for (Endpoint* eP = endpointList; eP != NULL; eP = eP->next)
  {
    bool earlier = false;                             // an application once, however many DataChannels have it

    for (Endpoint* pP = endpointList; pP != eP; pP = pP->next)
    {
      if ((pP->serverP == sP) && (strcmp(pP->app, eP->app) == 0))
        earlier = true;
    }

    if ((eP->serverP == sP) && (earlier == false))
    {
      char topic[512];

      topicOf(sP, eP->app, topic, sizeof(topic));
      mosquitto_subscribe(m, NULL, topic, 1);
    }
  }
  pthread_mutex_unlock(&mutex);

  COR_I("iotalora: connected to %s (%s)", sP->name, sP->desc);
}

static void onDisconnect(struct mosquitto* m, void* userdata, int rc)
{
  (void) m;

  Server* sP = (Server*) userdata;

  sP->connected = false;

  if (rc != 0)
    COR_W("iotalora: lost %s (%s) - reconnecting", sP->name, sP->desc);
}



// -----------------------------------------------------------------------------
//
// serverOpen - one member of "servers":
//
//   "ttn": { "kind": "ttn", "uri": "mqtts://eu1.cloud.thethings.network:8883", "username": "my-app@ttn",
//            "password": "NNSXS...", "caFile": "/etc/ssl/certs/ca-certificates.crt" }
//
// "passwordEnv" in place of "password": the environment variable that holds it - the key not in the file.
// mqtt:// (1883) or mqtts:// (8883; caFile: the CA the server's certificate is verified against).
//
static int serverOpen(CorNode* configP)
{
  const char* kind     = stringAt(configP, "kind", NULL);
  const char* uri      = stringAt(configP, "uri", NULL);
  const char* username = stringAt(configP, "username", NULL);
  const char* password = stringAt(configP, "password", NULL);
  const char* pwEnv    = stringAt(configP, "passwordEnv", NULL);
  const char* caFile   = stringAt(configP, "caFile", NULL);
  Server*     sP       = calloc(1, sizeof(Server));

  if (sP == NULL)
    return BRIDGE_ERR;

  snprintf(sP->name, sizeof(sP->name), "%s", configP->name);

  if ((kind != NULL) && (strcmp(kind, "ttn") == 0))
    sP->kind = ServerTtn;
  else if ((kind != NULL) && (strcmp(kind, "chirpstack") == 0))
    sP->kind = ServerChirpstack;
  else
  {
    COR_E("iotalora: servers.%s.kind - \"ttn\" (The Things Stack v3) or \"chirpstack\" (v4)", sP->name);
    free(sP);
    return BRIDGE_ERR;
  }

  if ((password == NULL) && (pwEnv != NULL))
    password = getenv(pwEnv);

  bool tls  = ((uri != NULL) && (strncmp(uri, "mqtts://", 8) == 0));
  int  skip = (tls == true) ? 8 : 7;

  if ((uri == NULL) || ((tls == false) && (strncmp(uri, "mqtt://", 7) != 0)))
  {
    COR_E("iotalora: servers.%s.uri - mqtt://host[:port] or mqtts://host[:port]", sP->name);
    free(sP);
    return BRIDGE_ERR;
  }

  char        host[256];
  const char* s     = &uri[skip];
  const char* colon = strchr(s, ':');
  const char* slash = strchr(s, '/');
  int         len   = (colon != NULL) ? (int) (colon - s) : (slash != NULL) ? (int) (slash - s) : (int) strlen(s);
  int         port  = (colon != NULL) ? atoi(&colon[1]) : ((tls == true) ? 8883 : 1883);

  if ((len <= 0) || (len >= (int) sizeof(host)) || (port <= 0) || (port > 65535))
  {
    COR_E("iotalora: servers.%s.uri '%s' - no host, or a port that is no TCP port", sP->name, uri);
    free(sP);
    return BRIDGE_ERR;
  }

  memcpy(host, s, len);
  host[len] = 0;
  snprintf(sP->desc, sizeof(sP->desc), "%s:%d", host, port);

  sP->mosq = mosquitto_new(NULL, true, sP);

  if (sP->mosq == NULL)
  {
    free(sP);
    return BRIDGE_ERR;
  }

  if ((username != NULL) && (mosquitto_username_pw_set(sP->mosq, username, password) != MOSQ_ERR_SUCCESS))
    COR_W("iotalora: servers.%s - the username and password not set", sP->name);

  if ((tls == true) && (mosquitto_tls_set(sP->mosq, caFile, (caFile == NULL) ? "/etc/ssl/certs" : NULL, NULL, NULL, NULL) != MOSQ_ERR_SUCCESS))
  {
    COR_E("iotalora: servers.%s - TLS: the caFile '%s' cannot be used", sP->name, (caFile != NULL) ? caFile : "(the system's, /etc/ssl/certs)");
    mosquitto_destroy(sP->mosq);
    free(sP);
    return BRIDGE_ERR;
  }

  mosquitto_connect_callback_set(sP->mosq, onConnect);
  mosquitto_disconnect_callback_set(sP->mosq, onDisconnect);
  mosquitto_message_callback_set(sP->mosq, onMessage);
  mosquitto_reconnect_delay_set(sP->mosq, 1, 30, true);

  int rc = mosquitto_connect_async(sP->mosq, host, port, 60);

  if ((rc != MOSQ_ERR_SUCCESS) && (rc != MOSQ_ERR_ERRNO))   // ERRNO: not up yet - the loop retries
  {
    COR_E("iotalora: cannot connect to %s (%s): %s", sP->name, sP->desc, mosquitto_strerror(rc));
    mosquitto_destroy(sP->mosq);
    free(sP);
    return BRIDGE_ERR;
  }

  if (mosquitto_loop_start(sP->mosq) != MOSQ_ERR_SUCCESS)
  {
    COR_E("iotalora: cannot start libmosquitto's thread for %s", sP->name);
    mosquitto_destroy(sP->mosq);
    free(sP);
    return BRIDGE_ERR;
  }

  sP->next   = serverList;
  serverList = sP;
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loraServersOpen -
//
int loraServersOpen(CorNode* serversP, const BridgeBroker* _brokerP)
{
  brokerP = _brokerP;

  if ((serversP == NULL) || (serversP->type != CorObject) || (serversP->value.head == NULL))
  {
    COR_E("iotalora: no \"servers\" - { \"<name>\": { \"kind\": \"ttn\" | \"chirpstack\", \"uri\": \"mqtt[s]://...\" } }");
    return BRIDGE_ERR;
  }

  mosquitto_lib_init();

  for (CorNode* configP = serversP->value.head; configP != NULL; configP = configP->next)
  {
    if ((configP->type != CorObject) || (serverOpen(configP) != BRIDGE_OK))
    {
      if (configP->type != CorObject)
        COR_E("iotalora: servers.%s is not an object", configP->name);
      return BRIDGE_ERR;
    }
  }

  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loraServersClose -
//
void loraServersClose(void)
{
  while (serverList != NULL)
  {
    Server* sP = serverList;

    serverList = sP->next;
    mosquitto_disconnect(sP->mosq);
    mosquitto_loop_stop(sP->mosq, true);
    mosquitto_destroy(sP->mosq);
    free(sP);
  }

  pthread_mutex_lock(&mutex);
  while (endpointList != NULL)
  {
    Endpoint* eP = endpointList;

    endpointList = eP->next;
    free(eP->endpoint);
    free(eP->app);
    free(eP->device);
    free(eP);
  }
  pthread_mutex_unlock(&mutex);

  mosquitto_lib_cleanup();
  brokerP = NULL;
}



// -----------------------------------------------------------------------------
//
// infoValue - a channelInfo key's value ([ { "key": ..., "value": ... } ]) into buf; false: not there
//
static bool infoValue(const char* info, const char* key, char* buf, int size)
{
  if (info == NULL)
    return false;

  CorAlloc ka;
  CorJson  cj;
  char     kaBuf[4 * 1024];
  char*    copy  = strdup(info);
  bool     found = false;

  if (copy == NULL)
    return false;

  corAllocBufferInit(&ka, kaBuf, sizeof(kaBuf), 16 * 1024, NULL, "iotalora info");
  corJsonCreate(&cj, &ka);

  CorNode* arrayP = corJsonParse(&cj, copy);

  for (CorNode* itemP = ((arrayP != NULL) && (arrayP->type == CorArray)) ? arrayP->value.head : NULL; itemP != NULL; itemP = itemP->next)
  {
    const char* k = stringAt(itemP, "key", NULL);
    const char* v = stringAt(itemP, "value", NULL);

    if ((k != NULL) && (v != NULL) && (strcmp(k, key) == 0))
    {
      snprintf(buf, size, "%s", v);
      found = true;
    }
  }

  corAllocBufferReset(&ka, false);
  free(copy);
  return found;
}



// -----------------------------------------------------------------------------
//
// loraEndpointAdd -
//
int loraEndpointAdd(const char* endpoint, const char* info)
{
  char  copy[512];
  char* serverName = copy;
  char* app;
  char* device;

  snprintf(copy, sizeof(copy), "%s", endpoint);

  if (((app = strchr(serverName, '/')) == NULL) || (app == serverName) || (app[1] == 0))
    return BRIDGE_BAD_INPUT;
  *app++ = 0;

  if ((device = strchr(app, '/')) != NULL)
  {
    *device++ = 0;
    if ((device[0] == 0) || (strchr(device, '/') != NULL) || (strchr(device, '+') != NULL) || (strchr(device, '#') != NULL))
      return BRIDGE_BAD_INPUT;
  }

  if ((strchr(app, '+') != NULL) || (strchr(app, '#') != NULL))
    return BRIDGE_BAD_INPUT;

  Server* sP;

  for (sP = serverList; (sP != NULL) && (strcmp(sP->name, serverName) != 0); sP = sP->next)
    ;

  if (sP == NULL)
  {
    COR_W("iotalora: DataChannel '%s' - no server '%s' in \"servers\"", endpoint, serverName);
    return BRIDGE_BAD_INPUT;
  }

  char      model[64] = "cayennelpp";
  char      fPort[16] = "1";
  DataModel m;

  infoValue(info, "dataModel", model, sizeof(model));
  infoValue(info, "fPort", fPort, sizeof(fPort));

  if (strcmp(model, "cayennelpp") == 0)               m = ModelLpp;
  else if (strcmp(model, "cbor") == 0)                m = ModelCbor;
  else if (strcmp(model, "application_server") == 0)  m = ModelServer;
  else
  {
    COR_W("iotalora: DataChannel '%s' - dataModel '%s' is cayennelpp, cbor or application_server", endpoint, model);
    return BRIDGE_BAD_INPUT;
  }

  int port = atoi(fPort);

  if ((port < 1) || (port > 223))
  {
    COR_W("iotalora: DataChannel '%s' - fPort '%s' is a LoRaWAN application port, 1-223", endpoint, fPort);
    return BRIDGE_BAD_INPUT;
  }

  Endpoint* eP = calloc(1, sizeof(Endpoint));

  if (eP == NULL)
    return BRIDGE_ERR;

  eP->endpoint = strdup(endpoint);
  eP->serverP  = sP;
  eP->app      = strdup(app);
  eP->device   = (device != NULL) ? strdup(device) : NULL;
  eP->model    = m;
  eP->fPort    = port;

  pthread_mutex_lock(&mutex);

  bool subscribed = appCarried(sP, app, NULL);

  eP->next     = endpointList;
  endpointList = eP;

  if ((subscribed == false) && (sP->connected == true))
  {
    char topic[512];

    topicOf(sP, app, topic, sizeof(topic));
    mosquitto_subscribe(sP->mosq, NULL, topic, 1);
  }

  pthread_mutex_unlock(&mutex);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// loraEndpointDel -
//
int loraEndpointDel(const char* endpoint)
{
  Endpoint* foundP = NULL;

  pthread_mutex_lock(&mutex);

  for (Endpoint** epP = &endpointList; *epP != NULL; epP = &(*epP)->next)
  {
    if (strcmp((*epP)->endpoint, endpoint) == 0)
    {
      foundP = *epP;
      *epP   = foundP->next;
      break;
    }
  }

  if ((foundP != NULL) && (appCarried(foundP->serverP, foundP->app, NULL) == false) && (foundP->serverP->connected == true))
  {
    char topic[512];

    topicOf(foundP->serverP, foundP->app, topic, sizeof(topic));
    mosquitto_unsubscribe(foundP->serverP->mosq, NULL, topic);
  }

  pthread_mutex_unlock(&mutex);

  if (foundP == NULL)
    return BRIDGE_NOT_FOUND;

  free(foundP->endpoint);
  free(foundP->app);
  free(foundP->device);
  free(foundP);
  return BRIDGE_OK;
}



// -----------------------------------------------------------------------------
//
// unflatten - a fragment's columns as the object they were flat - gps_1.latitude in gps_1 - the @-columns
// left out; NULL: why in *whyP
//
static CorNode* unflatten(CorAlloc* kaP, CorNode* fragmentP, const char** whyP)
{
  CorNode* objectP = corTreeObject(kaP, NULL);

  for (CorNode* memberP = fragmentP->value.head; (memberP != NULL) && (objectP != NULL); memberP = memberP->next)
  {
    if (memberP->name[0] == '@')
      continue;

    char* path = corAlloc(kaP, strlen(memberP->name) + 1);

    if (path == NULL)
      return NULL;
    strcpy(path, memberP->name);

    CorNode* containerP = objectP;
    char*    segment    = path;
    char*    dotP;

    while ((dotP = strchr(segment, '.')) != NULL)
    {
      *dotP = 0;

      CorNode* childP = corTreeLookup(containerP, segment);

      if (childP == NULL)
      {
        if ((childP = corTreeObject(kaP, segment)) == NULL)
          return NULL;
        corTreeChildAdd(containerP, childP);
      }
      else if (childP->type != CorObject)
      {
        *whyP = "two columns name the same member, one as an object";
        return NULL;
      }

      containerP = childP;
      segment    = dotP + 1;
    }

    CorNode* copyP = corTreeClone(kaP, memberP);

    if (copyP == NULL)
      return NULL;

    copyP->name = segment;
    corTreeChildAdd(containerP, copyP);
  }

  return objectP;
}



// -----------------------------------------------------------------------------
//
// loraPublish - the fragment as a downlink:
//
//   TTN         v3/<app>/devices/<device_id>/down/push   { "downlinks": [ { "f_port", "frm_payload" | "decoded_payload", "priority" } ] }
//   ChirpStack  application/<app>/device/<devEui>/command/down   { "devEui", "confirmed": false, "fPort", "data" | "object" }
//
// The device: the fragment's @deviceId (the id template's), else the endpoint's own. Cayenne LPP: the
// actuators it writes (digital_out_<n>, analog_out_<n>) encoded here; application_server: the values,
// for the network server's codec to encode; CBOR is not encoded.
//
int loraPublish(const char* endpoint, const char* json)
{
  Endpoint  e;
  Server*   sP = NULL;
  char      app[256];
  char      device[256] = "";

  pthread_mutex_lock(&mutex);

  for (Endpoint* eP = endpointList; eP != NULL; eP = eP->next)
  {
    if (strcmp(eP->endpoint, endpoint) == 0)
    {
      e  = *eP;
      sP = eP->serverP;
      snprintf(app, sizeof(app), "%s", eP->app);
      if (eP->device != NULL)
        snprintf(device, sizeof(device), "%s", eP->device);
      break;
    }
  }

  pthread_mutex_unlock(&mutex);

  if (sP == NULL)
    return BRIDGE_NOT_FOUND;

  if (sP->connected == false)
  {
    COR_W("iotalora: a downlink on '%s' refused - %s (%s) is not connected", endpoint, sP->name, sP->desc);
    return BRIDGE_ERR;
  }

  if (e.model == ModelCbor)
  {
    COR_W("iotalora: a downlink on '%s' refused - a CBOR downlink is not built (dataModel cayennelpp or application_server)", endpoint);
    return BRIDGE_UNSUPPORTED;
  }

  CorAlloc ka;
  CorJson  cj;
  char     buf[4 * 1024];

  corAllocBufferInit(&ka, buf, sizeof(buf), 16 * 1024, NULL, "iotalora downlink");
  corJsonCreate(&cj, &ka);

  int         len       = strlen(json);
  char*       copy      = corAlloc(&ka, len + 1);
  CorNode*    fragmentP = NULL;
  const char* why       = "out of memory";
  int         r         = BRIDGE_BAD_INPUT;

  if (copy != NULL)
  {
    memcpy(copy, json, len + 1);
    fragmentP = corJsonParse(&cj, copy);
    why       = "not a JSON object";
  }

  if ((fragmentP != NULL) && (fragmentP->type == CorObject))
  {
    const char* devFromId = stringAt(fragmentP, "@deviceId", NULL);

    if (devFromId != NULL)
      snprintf(device, sizeof(device), "%s", devFromId);

    CorNode* payloadP = NULL;
    char*    frame    = NULL;

    why = NULL;

    if (device[0] == 0)
      why = "no device - the id template has {@deviceId}, or the endpoint names the device";
    else if (e.model == ModelLpp)
    {
      unsigned char bytes[242];                       // a LoRaWAN frame's payload, at the most
      int           n = loraLppEncode(fragmentP, bytes, sizeof(bytes), &why);

      if (n > 0)
        frame = loraBase64Encode(&ka, bytes, n);
    }
    else
      payloadP = unflatten(&ka, fragmentP, &why);

    char  topic[600];
    char* message = NULL;

    if ((why == NULL) && ((frame != NULL) || (payloadP != NULL)))
    {
      CorNode* msgP = corTreeObject(&ka, NULL);

      if (sP->kind == ServerTtn)
      {
        CorNode* downP  = corTreeObject(&ka, NULL);
        CorNode* listP  = corTreeArray(&ka, "downlinks");

        corTreeChildAdd(downP, corTreeInteger(&ka, "f_port", e.fPort));
        if (frame != NULL)
          corTreeChildAdd(downP, corTreeString(&ka, "frm_payload", frame));
        else
        {
          payloadP->name = "decoded_payload";
          corTreeChildAdd(downP, payloadP);
        }
        corTreeChildAdd(downP, corTreeString(&ka, "priority", "NORMAL"));
        corTreeChildAdd(listP, downP);
        corTreeChildAdd(msgP, listP);

        snprintf(topic, sizeof(topic), "v3/%s/devices/%s/down/push", app, device);
      }
      else
      {
        corTreeChildAdd(msgP, corTreeString(&ka, "devEui", device));
        corTreeChildAdd(msgP, corTreeBoolean(&ka, "confirmed", false));
        corTreeChildAdd(msgP, corTreeInteger(&ka, "fPort", e.fPort));
        if (frame != NULL)
          corTreeChildAdd(msgP, corTreeString(&ka, "data", frame));
        else
        {
          payloadP->name = "object";
          corTreeChildAdd(msgP, payloadP);
        }

        snprintf(topic, sizeof(topic), "application/%s/device/%s/command/down", app, device);
      }

      int size = corJsonFastRenderSize(msgP);

      if ((message = malloc(size + 1)) != NULL)
        corJsonFastRender(msgP, message);
    }

    if (message != NULL)
    {
      int rc = mosquitto_publish(sP->mosq, NULL, topic, strlen(message), message, 1, false);

      if (rc == MOSQ_ERR_SUCCESS)
      {
        COR_V("iotalora: downlink on %s: %s", topic, message);
        r = BRIDGE_OK;
      }
      else
      {
        COR_W("iotalora: a downlink on %s: %s", topic, mosquitto_strerror(rc));
        r = BRIDGE_ERR;
      }

      free(message);
    }
    else if (why == NULL)
      why = "out of memory";
  }

  if (why != NULL)
    COR_W("iotalora: a downlink on '%s' refused - %s: %s", endpoint, why, json);

  corAllocBufferReset(&ka, false);
  return r;
}



// -----------------------------------------------------------------------------
//
// loraVersion -
//
const char* loraVersion(void)
{
  static char version[64];
  int         major, minor, revision;

  mosquitto_lib_version(&major, &minor, &revision);
  snprintf(version, sizeof(version), "libmosquitto %d.%d.%d", major, minor, revision);
  return version;
}
