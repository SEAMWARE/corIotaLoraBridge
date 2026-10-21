#ifndef LORA_CODEC_H_
#define LORA_CODEC_H_

//
// FILE            loraCodec.h
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// A LoRaWAN frame's payload as values, and values as a frame: base64 (the network servers' frm_payload /
// data), Cayenne LPP (iotagent-lorawan's default data model) and CBOR - and a record made flat.
//
#include <stdbool.h>                                  // bool

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corTree/CorNode.h"                          // CorNode



// -----------------------------------------------------------------------------
//
// loraBase64Decode - the bytes of base64 text, corAlloc'd, their number in *lenP; NULL: not base64
//
extern unsigned char* loraBase64Decode(CorAlloc* kaP, const char* text, int* lenP);



// -----------------------------------------------------------------------------
//
// loraBase64Encode - bytes as base64 text, corAlloc'd
//
extern char* loraBase64Encode(CorAlloc* kaP, const unsigned char* bytes, int len);



// -----------------------------------------------------------------------------
//
// loraLppDecode - a Cayenne LPP frame as members of objectP, named as iotagent-lorawan names them -
// <type>_<channel>: temperature_1, relative_humidity_2, gps_3 {latitude, longitude, altitude},
// accelerometer_4 {x, y, z}, ... NULL: ok, else why not (a type it does not know, a short frame)
//
extern const char* loraLppDecode(CorAlloc* kaP, const unsigned char* bytes, int len, CorNode* objectP);



// -----------------------------------------------------------------------------
//
// loraLppEncode - the actuators a fragment writes as a Cayenne LPP frame: digital_out_<channel> (0-255) and
// analog_out_<channel> (0.01 steps, -327.68 to 327.67), every other member skipped. The bytes in out (size),
// their number returned; -1: why not in *whyP
//
extern int loraLppEncode(CorNode* fragmentP, unsigned char* out, int size, const char** whyP);



// -----------------------------------------------------------------------------
//
// loraCborDecode - a CBOR item as a tree, corAlloc'd; NULL: why not in *whyP
//
extern CorNode* loraCborDecode(CorAlloc* kaP, const unsigned char* bytes, int len, const char** whyP);



// -----------------------------------------------------------------------------
//
// loraFlatten - the members of objectP into recordP, an object member's members each a member of its own
// named by its path with dots (gps_1.latitude); an array one member. false: out of memory
//
extern bool loraFlatten(CorAlloc* kaP, CorNode* recordP, CorNode* objectP, const char* prefix);

#endif  // LORA_CODEC_H_
