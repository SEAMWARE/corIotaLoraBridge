#!/usr/bin/env python3
#
# FILE            netserver.py
#
# Copyright 2026 Seamware
# SPDX-License-Identifier: Apache-2.0
#
# netserver.py - a LoRaWAN network server's MQTT integration, as much of it as the tests need:
#
#   netserver.py pub <port> <topic> <json>   publish an uplink, as TTN or ChirpStack publishes it
#   netserver.py listen <port> <logFile>     the downlinks the plugin publishes, one line each: <topic> <payload>;
#                                            READY in the log once subscribed
#   netserver.py b64 <hex>                   a frame's bytes (hex) as base64 - Cayenne LPP, written out by hand
#   netserver.py cbor <json>                 a JSON value as a CBOR item, base64
#
import base64
import json
import struct
import sys

import paho.mqtt.client as mqtt
import paho.mqtt.publish as publish


def cbor(v):
    def head(major, n):
        if n < 24:
            return bytes([major << 5 | n])
        for info, fmt in ((24, ">B"), (25, ">H"), (26, ">I"), (27, ">Q")):
            if n < 1 << (8 * struct.calcsize(fmt)):
                return bytes([major << 5 | info]) + struct.pack(fmt, n)
    if v is False:
        return b"\xf4"
    if v is True:
        return b"\xf5"
    if v is None:
        return b"\xf6"
    if isinstance(v, int):
        return head(0, v) if v >= 0 else head(1, -1 - v)
    if isinstance(v, float):
        return b"\xfb" + struct.pack(">d", v)
    if isinstance(v, str):
        b = v.encode()
        return head(3, len(b)) + b
    if isinstance(v, list):
        return head(4, len(v)) + b"".join(cbor(x) for x in v)
    if isinstance(v, dict):
        return head(5, len(v)) + b"".join(cbor(k) + cbor(x) for k, x in v.items())
    raise ValueError(v)


def listen(port, logFile):
    def onConnect(client, userdata, flags, rc, properties=None):
        client.subscribe("v3/+/devices/+/down/push", qos=1)
        client.subscribe("application/+/device/+/command/down", qos=1)

    def onSubscribe(client, userdata, mid, rc, properties=None):
        with open(logFile, "a") as f:
            f.write("READY\n")

    def onMessage(client, userdata, msg):
        with open(logFile, "a") as f:
            f.write("%s %s\n" % (msg.topic, msg.payload.decode()))

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = onConnect
    client.on_subscribe = onSubscribe
    client.on_message = onMessage
    client.connect("localhost", port)
    client.loop_forever()


verb = sys.argv[1]

if verb == "pub":
    publish.single(sys.argv[3], sys.argv[4], qos=1, hostname="localhost", port=int(sys.argv[2]))
elif verb == "listen":
    listen(int(sys.argv[2]), sys.argv[3])
elif verb == "b64":
    print(base64.b64encode(bytes.fromhex(sys.argv[2])).decode())
elif verb == "cbor":
    print(base64.b64encode(cbor(json.loads(sys.argv[2]))).decode())
