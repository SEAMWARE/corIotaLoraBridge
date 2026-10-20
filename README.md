# corIotaLoraBridge

**Prototype - not released.** The FIWARE IoT Agent for LoRaWAN in the coraine NGSI-LD broker: a network
server's **uplinks** as **DataChannel records**, and a written attribute as a **downlink** - `iotalora.so`, a
bridge plugin loaded with `--bridges iotalora`. It speaks the network servers' MQTT integrations: The Things
Stack v3 (TTN) and ChirpStack v4.

## DataChannels

A DataChannel's endpoint is `<server>/<applicationId>` - every device of the application, its entity from
the record's `@deviceId` - or `<server>/<applicationId>/<deviceId>`, one device (it takes the device's
uplinks before its application's DataChannel does). `<server>` names a member of `servers`.

| Server | Uplink topic | `@deviceId` |
|---|---|---|
| TTN v3 (`"kind": "ttn"`) | `v3/<applicationId>/devices/<device_id>/up` | the device_id; `@devEui` its dev_eui |
| ChirpStack v4 (`"kind": "chirpstack"`) | `application/<applicationId>/device/<devEui>/event/up` | the DevEUI; `@deviceName` its name |

- **The record** is the frame's payload decoded, flat - an object's members each a column of their own,
  named by their path with dots (`gps_3.latitude`) - plus `@deviceId`, `@applicationId`, `@devEui` /
  `@deviceName`, `@fPort`.
- **Its time** (`observedAt`): TTN's `uplink_message.received_at` (else `received_at`), ChirpStack's `time`.
- **The data model**, per DataChannel (`channelInfo` `dataModel`), as iotagent-lorawan's:

| `dataModel` | The payload |
|---|---|
| `cayennelpp` (the default) | the frame (`frm_payload` / `data`, base64) as Cayenne LPP: `<type>_<channel>` - `digital_in`, `digital_out`, `analog_in`, `analog_out`, `luminosity`, `presence`, `temperature`, `relative_humidity`, `accelerometer` (`.x .y .z`), `barometric_pressure`, `gyrometer` (`.x .y .z`), `gps` (`.latitude .longitude .altitude`) |
| `cbor` | the frame as one CBOR item, a map; a byte string as base64 |
| `application_server` | decoded by the network server's payload formatter (codec): TTN's `decoded_payload`, ChirpStack's `object` |

- A frame that cannot be decoded - an LPP type that is not known, a frame that ends early, not CBOR - is
  logged and dropped whole. Luminosity and barometric pressure are read unsigned, as the LPP spec has them
  (iotagent-lorawan reads them signed).
- A Channel or an AssetChannel configured for this bridge is refused (dormant): it carries DataChannels only.

### Write-back - downlinks

With `"channelDirection": "both"` (coraine's DataChannel write-back, `"idEncoding": "percent"`), a client's
write to a mapped attribute is published to the plugin first, as a record fragment, and becomes a downlink:

| Server | Downlink topic | Message |
|---|---|---|
| TTN v3 | `v3/<applicationId>/devices/<device_id>/down/push` | `{"downlinks": [{"f_port", "frm_payload" \| "decoded_payload", "priority": "NORMAL"}]}` |
| ChirpStack v4 | `application/<applicationId>/device/<devEui>/command/down` | `{"devEui", "confirmed": false, "fPort", "data" \| "object"}` |

- **The device**: the fragment's `@deviceId` (an id template with `{@deviceId}`), else the endpoint's.
- **The port**: `channelInfo` `fPort` (1-223), 1 when not said.
- `cayennelpp`: the LPP actuators the fragment writes - `digital_out_<channel>` (0-255) and
  `analog_out_<channel>` (-327.68 to 327.67) - encoded as the frame; a write to anything else (a sensor's
  value) is refused, and the broker answers the client 400.
- `application_server`: the values, made whole again (`valve.open` as `{"valve": {"open": ...}}`), for the
  network server's formatter to encode.
- `cbor`: not built - refused.
- The downlink is queued to the network server (MQTT QoS 1) and the client answered; a server that is not
  connected refuses the write.

iotagent-lorawan has no downlinks.

## Configuration

```json
{
  "iotalora": {
    "servers": {
      "ttn": { "kind": "ttn", "uri": "mqtts://eu1.cloud.thethings.network:8883",
               "username": "farm@ttn", "passwordEnv": "TTN_API_KEY" },
      "cs":  { "kind": "chirpstack", "uri": "mqtt://chirpstack-mosquitto:1883" }
    },
    "ngsild": {
      "defaultEntity": false,
      "dataChannels": {
        "ttn/farm@ttn": { "channelDirection": "both", "idEncoding": "percent",
                          "entities": [ { "type": "Sensor", "id": "urn:ngsi-ld:Sensor:{@deviceId}",
                                          "attributes": { "temperature": { "column": "temperature_1" },
                                                          "location":    { "point": { "latitude": "gps_3.latitude", "longitude": "gps_3.longitude" } },
                                                          "relay":       { "column": "digital_out_4" } } } ] },
        "cs/app-1": { "channelInfo": [ { "key": "dataModel", "value": "application_server" } ],
                      "entities": [ { "type": "Meter", "id": "urn:ngsi-ld:Meter:{@deviceId}",
                                      "attributes": { "energy": { "column": "energy" } } } ] }
      }
    }
  }
}
```

| `servers` member | |
|---|---|
| `kind` | `ttn` (The Things Stack v3) or `chirpstack` (v4) |
| `uri` | `mqtt://host[:port]` (1883) or `mqtts://host[:port]` (8883) |
| `username`, `password` | the integration's credentials - TTN: `<applicationId>`, an API key |
| `passwordEnv` | in place of `password`: the environment variable that holds it |
| `caFile` | mqtts: the CA the server's certificate is verified against (default: the system's, `/etc/ssl/certs`) |

## Not built

The provisioning API (a DataChannel comes from the configuration file), CBOR downlinks, confirmed
downlinks, the network servers' other events (join, ack, status), ChirpStack v3's topics.

## Build and test

    make && make test          # libmosquitto-dev; the tests run mosquitto and python3-paho-mqtt

`test/funcTests/netserver.py` plays both network servers over a private mosquitto on port 11886: their
uplinks, a listener for the downlinks, Cayenne LPP frames written out by hand, CBOR.
