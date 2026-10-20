//
// FILE            loraCodec.c
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
// A LoRaWAN frame's payload as values, and values as a frame - see header.
//
#include <math.h>                                     // ldexp, isfinite, lround
#include <stdbool.h>                                  // bool
#include <stdint.h>                                   // uint64_t
#include <stdio.h>                                    // snprintf
#include <stdlib.h>                                   // strtol
#include <string.h>                                   // strlen, strncmp, memcpy

#include "corAlloc/CorAlloc.h"                        // CorAlloc
#include "corAlloc/corAlloc.h"                        // corAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeObject, corTreeChildAdd, ...
#include "corTree/corTreeClone.h"                     // corTreeClone

#include "loraCodec.h"                                // Own interface



static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";



// -----------------------------------------------------------------------------
//
// b64Value - a base64 digit's value, -1: not one
//
static int b64Value(char c)
{
  if ((c >= 'A') && (c <= 'Z')) return c - 'A';
  if ((c >= 'a') && (c <= 'z')) return c - 'a' + 26;
  if ((c >= '0') && (c <= '9')) return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}



// -----------------------------------------------------------------------------
//
// loraBase64Decode -
//
unsigned char* loraBase64Decode(CorAlloc* kaP, const char* text, int* lenP)
{
  int            textLen = strlen(text);
  unsigned char* out     = (unsigned char*) corAlloc(kaP, textLen * 3 / 4 + 3);
  int            at      = 0;
  unsigned int   acc     = 0;
  int            bits    = 0;

  if (out == NULL)
    return NULL;

  for (int ix = 0; ix < textLen; ix++)
  {
    if (text[ix] == '=')
    {
      for (; ix < textLen; ix++)
      {
        if (text[ix] != '=')
          return NULL;
      }
      break;
    }

    int v = b64Value(text[ix]);

    if (v < 0)
      return NULL;

    acc   = (acc << 6) | v;
    bits += 6;

    if (bits >= 8)
    {
      bits -= 8;
      out[at++] = (unsigned char) ((acc >> bits) & 0xFF);
    }
  }

  *lenP = at;
  return out;
}



// -----------------------------------------------------------------------------
//
// loraBase64Encode -
//
char* loraBase64Encode(CorAlloc* kaP, const unsigned char* bytes, int len)
{
  char* out = corAlloc(kaP, ((len + 2) / 3) * 4 + 1);
  int   at  = 0;

  if (out == NULL)
    return NULL;

  for (int ix = 0; ix < len; ix += 3)
  {
    unsigned int v = bytes[ix] << 16;

    if (ix + 1 < len) v |= bytes[ix + 1] << 8;
    if (ix + 2 < len) v |= bytes[ix + 2];

    out[at++] = b64[(v >> 18) & 0x3F];
    out[at++] = b64[(v >> 12) & 0x3F];
    out[at++] = (ix + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
    out[at++] = (ix + 2 < len) ? b64[v & 0x3F]        : '=';
  }

  out[at] = 0;
  return out;
}



// -----------------------------------------------------------------------------
//
// be - a big-endian integer of n bytes, signed or not
//
static long be(const unsigned char* p, int n, bool isSigned)
{
  unsigned long v = 0;

  for (int ix = 0; ix < n; ix++)
    v = (v << 8) | p[ix];

  if ((isSigned == true) && ((p[0] & 0x80) != 0))
    return (long) v - (1L << (n * 8));

  return (long) v;
}



// -----------------------------------------------------------------------------
//
// LppType - a Cayenne LPP data type: its number, name, size; how each of its values is read
//
typedef struct LppType
{
  int          type;
  const char*  name;
  int          size;
  int          parts;                                 // 1, or 3 (x/y/z, latitude/longitude/altitude)
  bool         isSigned;
  double       divisor;                               // 0: an integer
} LppType;

//
// The types iotagent-lorawan reads, as the Cayenne LPP spec has them. Luminosity and barometric pressure
// are unsigned (iotagent-lorawan reads them signed: a value over 32767 comes out negative there)
//
static const LppType lppTypes[] =
{
  {   0, "digital_in",          1, 1, false, 0      },
  {   1, "digital_out",         1, 1, false, 0      },
  {   2, "analog_in",           2, 1, true,  100    },
  {   3, "analog_out",          2, 1, true,  100    },
  { 101, "luminosity",          2, 1, false, 0      },
  { 102, "presence",            1, 1, false, 0      },
  { 103, "temperature",         2, 1, true,  10     },
  { 104, "relative_humidity",   1, 1, false, 2      },
  { 113, "accelerometer",       6, 3, true,  1000   },
  { 115, "barometric_pressure", 2, 1, false, 10     },
  { 134, "gyrometer",           6, 3, true,  100    },
  { 136, "gps",                 9, 3, true,  0      }      // 3 bytes each: 0.0001 deg, 0.0001 deg, 0.01 m
};



// -----------------------------------------------------------------------------
//
// loraLppDecode -
//
const char* loraLppDecode(CorAlloc* kaP, const unsigned char* bytes, int len, CorNode* objectP)
{
  if (len < 3)
    return "a Cayenne LPP frame has 3 bytes at least";

  int cursor = 0;

  while (cursor < len)
  {
    if (cursor + 2 > len)
      return "a Cayenne LPP frame ends inside a value's channel and type";

    int            channel = bytes[cursor];
    int            type    = bytes[cursor + 1];
    const LppType* tP      = NULL;

    cursor += 2;

    for (unsigned int ix = 0; ix < sizeof(lppTypes) / sizeof(lppTypes[0]); ix++)
    {
      if (lppTypes[ix].type == type)
        tP = &lppTypes[ix];
    }

    if (tP == NULL)
      return "a Cayenne LPP data type that is not known - the rest of the frame cannot be read";

    if (cursor + tP->size > len)
      return "a Cayenne LPP frame ends inside a value";

    char name[64];

    snprintf(name, sizeof(name), "%s_%d", tP->name, channel);

    char* nameCopy = corAlloc(kaP, strlen(name) + 1);

    if (nameCopy == NULL)
      return "out of memory";
    strcpy(nameCopy, name);

    CorNode* valueP;

    if (tP->parts == 1)
    {
      long raw = be(&bytes[cursor], tP->size, tP->isSigned);

      valueP = (tP->divisor == 0) ? corTreeInteger(kaP, nameCopy, raw) : corTreeFloat(kaP, nameCopy, raw / tP->divisor);
    }
    else
    {
      static const char* xyz[] = { "x", "y", "z" };
      static const char* gps[] = { "latitude", "longitude", "altitude" };
      int                each  = tP->size / 3;

      valueP = corTreeObject(kaP, nameCopy);

      for (int part = 0; (part < 3) && (valueP != NULL); part++)
      {
        long   raw     = be(&bytes[cursor + part * each], each, true);
        double divisor = (tP->type == 136) ? ((part < 2) ? 10000 : 100) : tP->divisor;

        corTreeChildAdd(valueP, corTreeFloat(kaP, (tP->type == 136) ? gps[part] : xyz[part], raw / divisor));
      }
    }

    if (valueP == NULL)
      return "out of memory";

    corTreeChildAdd(objectP, valueP);
    cursor += tP->size;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// loraLppEncode -
//
int loraLppEncode(CorNode* fragmentP, unsigned char* out, int size, const char** whyP)
{
  int at = 0;

  for (CorNode* memberP = fragmentP->value.head; memberP != NULL; memberP = memberP->next)
  {
    bool digital = (strncmp(memberP->name, "digital_out_", 12) == 0);
    bool analog  = (strncmp(memberP->name, "analog_out_",  11) == 0);

    if ((digital == false) && (analog == false))
      continue;

    char* endP;
    long  channel = strtol(&memberP->name[digital ? 12 : 11], &endP, 10);

    if ((*endP != 0) || (channel < 0) || (channel > 255))
    {
      *whyP = "an LPP actuator's name ends in its channel, 0-255 (digital_out_3)";
      return -1;
    }

    double v;

    if (memberP->type == CorInt)
      v = (double) memberP->value.i;
    else if (memberP->type == CorFloat)
      v = memberP->value.f;
    else if (memberP->type == CorBoolean)
      v = (memberP->value.b == true) ? 1 : 0;
    else
    {
      *whyP = "an LPP actuator's value is a number";
      return -1;
    }

    if (at + ((digital == true) ? 3 : 4) > size)
    {
      *whyP = "the frame is too long";
      return -1;
    }

    out[at++] = (unsigned char) channel;

    if (digital == true)
    {
      if ((v < 0) || (v > 255) || (v != (long) v))
      {
        *whyP = "a digital_out value is an integer, 0-255";
        return -1;
      }

      out[at++] = 1;
      out[at++] = (unsigned char) v;
    }
    else
    {
      long raw = lround(v * 100);

      if ((raw < -32768) || (raw > 32767))
      {
        *whyP = "an analog_out value is -327.68 to 327.67";
        return -1;
      }

      out[at++] = 3;
      out[at++] = (unsigned char) ((raw >> 8) & 0xFF);
      out[at++] = (unsigned char) (raw & 0xFF);
    }
  }

  if (at == 0)
    *whyP = "nothing an LPP frame can write - a Cayenne LPP actuator is digital_out_<channel> or analog_out_<channel>";

  return (at == 0) ? -1 : at;
}



// -----------------------------------------------------------------------------
//
// Cbor - the decoder's position
//
typedef struct Cbor
{
  CorAlloc*             kaP;
  const unsigned char*  bytes;
  int                   len;
  int                   at;
  const char*           why;
} Cbor;



// -----------------------------------------------------------------------------
//
// cborArg - the argument of the item's initial byte (its length, its value); false: past the end, or
// indefinite (*indefiniteP)
//
static bool cborArg(Cbor* cP, int info, uint64_t* argP, bool* indefiniteP)
{
  *indefiniteP = false;

  if (info < 24)
  {
    *argP = info;
    return true;
  }

  if (info == 31)
  {
    *indefiniteP = true;
    return true;
  }

  int n = (info == 24) ? 1 : (info == 25) ? 2 : (info == 26) ? 4 : (info == 27) ? 8 : 0;

  if ((n == 0) || (cP->at + n > cP->len))
  {
    cP->why = (n == 0) ? "a CBOR item with a reserved length" : "a CBOR item ends early";
    return false;
  }

  uint64_t v = 0;

  for (int ix = 0; ix < n; ix++)
    v = (v << 8) | cP->bytes[cP->at++];

  *argP = v;
  return true;
}



// -----------------------------------------------------------------------------
//
// halfFloat - an IEEE 754 half-precision float
//
static double halfFloat(unsigned int h)
{
  int    exp  = (h >> 10) & 0x1F;
  int    mant = h & 0x3FF;
  double v;

  if (exp == 0)
    v = ldexp(mant, -24);
  else if (exp != 31)
    v = ldexp(mant + 1024, exp - 25);
  else
    v = (mant == 0) ? INFINITY : NAN;

  return ((h & 0x8000) != 0) ? -v : v;
}



// -----------------------------------------------------------------------------
//
// cborItem - one item, named name; NULL: why in cP->why
//
static CorNode* cborItem(Cbor* cP, const char* name, int depth)
{
  if (depth > 16)
  {
    cP->why = "CBOR nested deeper than 16";
    return NULL;
  }

  if (cP->at >= cP->len)
  {
    cP->why = "a CBOR item ends early";
    return NULL;
  }

  int      initial = cP->bytes[cP->at++];
  int      major   = initial >> 5;
  int      info    = initial & 0x1F;
  uint64_t arg;
  bool     indefinite;

  if ((major == 7) && (info >= 25) && (info <= 27))
  {
    if (cborArg(cP, info, &arg, &indefinite) == false)
      return NULL;

    double v;

    if (info == 25)
      v = halfFloat((unsigned int) arg);
    else if (info == 26)
    {
      uint32_t u = (uint32_t) arg;
      float    f;

      memcpy(&f, &u, sizeof(f));
      v = f;
    }
    else
      memcpy(&v, &arg, sizeof(v));

    if (isfinite(v) == 0)
      return corTreeNull(cP->kaP, name);              // JSON has no NaN nor Infinity

    return corTreeFloat(cP->kaP, name, v);
  }

  if (cborArg(cP, info, &arg, &indefinite) == false)
    return NULL;

  switch (major)
  {
  case 0:
    if (arg > (uint64_t) INT64_MAX)
      return corTreeFloat(cP->kaP, name, (double) arg);
    return corTreeInteger(cP->kaP, name, (long long) arg);

  case 1:
    if (arg > (uint64_t) INT64_MAX)
      return corTreeFloat(cP->kaP, name, -1.0 - (double) arg);
    return corTreeInteger(cP->kaP, name, -1 - (long long) arg);

  case 2:
  case 3:
  {
    //
    // A string - a byte string as base64 - in one piece, or (indefinite) in chunks of its own major type
    //
    int   total = 0;
    int   start = cP->at;
    char* text;

    if (indefinite == false)
    {
      if (arg > (uint64_t) (cP->len - cP->at))
      {
        cP->why = "a CBOR string ends early";
        return NULL;
      }

      total = (int) arg;
      text  = corAlloc(cP->kaP, total + 1);
      if (text == NULL)
        return NULL;
      memcpy(text, &cP->bytes[start], total);
      cP->at += total;
    }
    else
    {
      text = corAlloc(cP->kaP, cP->len - cP->at + 1);
      if (text == NULL)
        return NULL;

      for (;;)
      {
        if (cP->at >= cP->len)
        {
          cP->why = "a CBOR string ends early";
          return NULL;
        }

        if (cP->bytes[cP->at] == 0xFF)
        {
          cP->at++;
          break;
        }

        int      chunkInitial = cP->bytes[cP->at++];
        uint64_t chunkLen;
        bool     chunkIndefinite;

        if (((chunkInitial >> 5) != major) || (cborArg(cP, chunkInitial & 0x1F, &chunkLen, &chunkIndefinite) == false) ||
            (chunkIndefinite == true) || (chunkLen > (uint64_t) (cP->len - cP->at)))
        {
          cP->why = "a CBOR string's chunk is not one";
          return NULL;
        }

        memcpy(&text[total], &cP->bytes[cP->at], (int) chunkLen);
        total  += (int) chunkLen;
        cP->at += (int) chunkLen;
      }
    }

    text[total] = 0;

    if (major == 2)
      return corTreeString(cP->kaP, name, loraBase64Encode(cP->kaP, (unsigned char*) text, total));

    return corTreeString(cP->kaP, name, text);
  }

  case 4:
  case 5:
  {
    CorNode* containerP = (major == 4) ? corTreeArray(cP->kaP, name) : corTreeObject(cP->kaP, name);

    if (containerP == NULL)
      return NULL;

    for (uint64_t ix = 0; (indefinite == true) || (ix < arg); ix++)
    {
      if ((indefinite == true) && (cP->at < cP->len) && (cP->bytes[cP->at] == 0xFF))
      {
        cP->at++;
        break;
      }

      const char* memberName = "";

      if (major == 5)
      {
        //
        // A map's key - a text, or another item as its JSON text (an integer key: "1")
        //
        CorNode* keyP = cborItem(cP, NULL, depth + 1);

        if (keyP == NULL)
          return NULL;

        if (keyP->type == CorString)
          memberName = keyP->value.s;
        else
        {
          char* keyText = corAlloc(cP->kaP, 64);

          if (keyText == NULL)
            return NULL;

          if (keyP->type == CorInt)
            snprintf(keyText, 64, "%lld", keyP->value.i);
          else if (keyP->type == CorFloat)
            snprintf(keyText, 64, "%g", keyP->value.f);
          else if (keyP->type == CorBoolean)
            snprintf(keyText, 64, "%s", (keyP->value.b == true) ? "true" : "false");
          else
            snprintf(keyText, 64, "null");

          memberName = keyText;
        }
      }

      CorNode* itemP = cborItem(cP, memberName, depth + 1);

      if (itemP == NULL)
        return NULL;

      corTreeChildAdd(containerP, itemP);
    }

    return containerP;
  }

  case 6:
    return cborItem(cP, name, depth + 1);             // a tag: its content, the tag not kept

  default:                                            // 7, the simple values
    if (arg == 20) return corTreeBoolean(cP->kaP, name, false);
    if (arg == 21) return corTreeBoolean(cP->kaP, name, true);
    if ((arg == 22) || (arg == 23)) return corTreeNull(cP->kaP, name);

    cP->why = "a CBOR simple value that is not false, true, null nor undefined";
    return NULL;
  }
}



// -----------------------------------------------------------------------------
//
// loraCborDecode -
//
CorNode* loraCborDecode(CorAlloc* kaP, const unsigned char* bytes, int len, const char** whyP)
{
  Cbor cbor = { kaP, bytes, len, 0, "out of memory" };

  CorNode* itemP = cborItem(&cbor, NULL, 0);

  if ((itemP != NULL) && (cbor.at != len))
  {
    *whyP = "bytes after the CBOR item";
    return NULL;
  }

  if (itemP == NULL)
    *whyP = cbor.why;

  return itemP;
}



// -----------------------------------------------------------------------------
//
// loraFlatten -
//
bool loraFlatten(CorAlloc* kaP, CorNode* recordP, CorNode* objectP, const char* prefix)
{
  for (CorNode* memberP = objectP->value.head; memberP != NULL; memberP = memberP->next)
  {
    char* name = memberP->name;

    if (prefix != NULL)
    {
      int len = strlen(prefix) + 1 + strlen(memberP->name) + 1;

      if ((name = corAlloc(kaP, len)) == NULL)
        return false;
      snprintf(name, len, "%s.%s", prefix, memberP->name);
    }

    if (memberP->type == CorObject)
    {
      if (loraFlatten(kaP, recordP, memberP, name) == false)
        return false;
      continue;
    }

    CorNode* copyP = corTreeClone(kaP, memberP);

    if (copyP == NULL)
      return false;

    copyP->name = name;
    corTreeChildAdd(recordP, copyP);
  }

  return true;
}
