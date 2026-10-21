#ifndef LORA_SERVER_H_
#define LORA_SERVER_H_

//
// FILE            loraServer.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// LoRaWAN network servers over MQTT - The Things Stack (TTN v3) and ChirpStack (v4) - their uplinks as
// DataChannel records, a written attribute as a downlink.
//
// A DataChannel's endpoint is <server>/<applicationId> (every device of the application, its entity from
// the record's @deviceId) or <server>/<applicationId>/<deviceId> (one device). <server> names a member of the
// configuration's "servers"; <deviceId> is the network server's: TTN's device_id, ChirpStack's DevEUI.
//
// The record: the frame's payload decoded - Cayenne LPP (the default), CBOR, or decoded by the network
// server already (application_server) - flat, plus @deviceId, @devEui, @applicationId and @fPort. Its time
// the network server's (TTN received_at, ChirpStack time).
//
#include <stdbool.h>                                  // bool

#include "corTree/CorNode.h"                          // CorNode
#include "corBridge/BridgeBroker.h"                   // BridgeBroker



// -----------------------------------------------------------------------------
//
// loraServersOpen - a connection to each of "servers" (the plugin's config member); BRIDGE_OK or BRIDGE_ERR
//
extern int loraServersOpen(CorNode* serversP, const BridgeBroker* brokerP);



// -----------------------------------------------------------------------------
//
// loraServersClose - no thread of the plugin calls the broker after it
//
extern void loraServersClose(void);



// -----------------------------------------------------------------------------
//
// loraEndpointAdd / Del - a DataChannel's endpoint; info: its channelInfo (JSON text, NULL: none) -
// "dataModel" (cayennelpp, cbor, application_server) and "fPort" (a downlink's, 1 when not said)
//
extern int loraEndpointAdd(const char* endpoint, const char* info);
extern int loraEndpointDel(const char* endpoint);



// -----------------------------------------------------------------------------
//
// loraPublish - a write-back fragment as a downlink, queued to the network server
//
extern int loraPublish(const char* endpoint, const char* json);



// -----------------------------------------------------------------------------
//
// loraVersion - "libmosquitto x.y.z"
//
extern const char* loraVersion(void);

#endif  // LORA_SERVER_H_
