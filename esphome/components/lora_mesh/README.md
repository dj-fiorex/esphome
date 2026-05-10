# `lora_mesh` — LoRa Multi-Hop Mesh Networking Component

`lora_mesh` is an ESPHome component that builds a self-healing, multi-hop mesh network over a LoRa radio. Nodes discover each other automatically, forward packets on behalf of others, and can optionally act as gateways to upstream infrastructure (such as Wi-Fi/internet).

---

## Table of Contents

1. [Purpose and Features](#purpose-and-features)
2. [Architecture](#architecture)
3. [Dependencies and Supported Platforms](#dependencies-and-supported-platforms)
4. [Setup Steps](#setup-steps)
5. [Configuration Reference](#configuration-reference)
   - [Core Options](#core-options)
   - [Routing Options](#routing-options)
   - [Diagnostic Sensors](#diagnostic-sensors)
   - [Automation Triggers](#automation-triggers)
6. [Actions](#actions)
7. [YAML Examples](#yaml-examples)
   - [Minimal Node](#minimal-node)
   - [Always-on Gateway](#always-on-gateway)
   - [Manual Gateway (upstream-driven)](#manual-gateway-upstream-driven)
   - [Full-featured Node with Diagnostics](#full-featured-node-with-diagnostics)
8. [MeshMessage Fields](#meshmessage-fields)
9. [Public C++ API](#public-c-api)
10. [Protocol Overview](#protocol-overview)
11. [Routing Algorithm](#routing-algorithm)
12. [Gateway Modes](#gateway-modes)
13. [Limitations](#limitations)
14. [Troubleshooting](#troubleshooting)
15. [Developer Notes](#developer-notes)

---

## Purpose and Features

- **Multi-hop mesh**: packets are automatically relayed through intermediate nodes, extending range far beyond a single radio link.
- **Automatic neighbor discovery**: periodic HELLO beacons let nodes find each other without any manual topology configuration.
- **Distance-vector routing**: each HELLO beacon advertises known routes, so remote nodes learn paths through neighbors.
- **Duplicate suppression**: a ring-buffer seen-cache prevents the same packet being processed or forwarded more than once.
- **Gateway abstraction**: any node can act as a gateway (bridge to another network). Sensor nodes can send data to the "best" gateway without knowing which one it is.
- **Three sending modes**: unicast to a named node, broadcast to all nodes, or unicast to the best available gateway.
- **Automation-friendly**: `on_message`, `on_route_update`, and `on_gateway_changed` triggers integrate natively with ESPHome automations.
- **Optional diagnostic sensors**: expose node count, gateway availability, routing table (JSON), and best gateway name to ESPHome sensors.
- **Radio-agnostic**: works with both the `sx126x` (SX1261/SX1262/SX1268) and `sx127x` (SX1276/SX1278/…) radio components via thin adapter classes.
- **Embedded-friendly**: fixed-size arrays (no `std::vector` growth after `setup()`), no heap allocation in the fast path.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  ESPHome YAML configuration                              │
└────────────────────────┬─────────────────────────────────┘
                         │ Python code-generation (__init__.py)
                         ▼
┌──────────────────────────────────────────────────────────┐
│  LoraMesh  (lora_mesh.h / lora_mesh.cpp)                 │
│  ┌────────────────────────────────────────────────────┐  │
│  │  Routing table   (std::array<RouteEntry, N>)       │  │
│  │  Seen-packet cache (std::array<SeenEntry, M>)      │  │
│  │  Callback managers (on_message / on_route_update / │  │
│  │                     on_gateway_changed)            │  │
│  └────────────────────────┬───────────────────────────┘  │
│                           │  LoRaRadio* (abstract)        │
└───────────────────────────┼──────────────────────────────┘
                            │
            ┌───────────────┴────────────────┐
            │                                │
 ┌──────────┴─────────┐           ┌──────────┴────────┐
 │ LoRaSX126xRadio    │           │ LoRaSX127xRadio   │
 │ (adapter)          │           │ (adapter)         │
 └──────────┬─────────┘           └──────────┬────────┘
            │                                │
 ┌──────────┴─────────┐           ┌──────────┴────────┐
 │ sx126x component   │           │ sx127x component  │
 └────────────────────┘           └───────────────────┘
```

**Key classes and files:**

| File | Description |
|------|-------------|
| `__init__.py` | Python configuration schema and C++ code generation |
| `lora_mesh.h` / `lora_mesh.cpp` | Main `LoraMesh` component — lifecycle, routing, packet processing |
| `lora_packet.h` | Protocol constants, structs (`RouteEntry`, `SeenEntry`, `MeshMessage`), FNV-1a hash |
| `lora_radio.h` | Abstract `LoRaRadio` interface |
| `lora_radio_adapters.h` | Concrete adapters for `sx126x` and `sx127x` |
| `automation.h` | `SendMessageAction`, `BroadcastMessageAction`, `SendToGatewayAction` |

### Component Lifecycle

1. **`setup()`** — Derives `node_id` (from config or MAC), computes `mesh_id` from `mesh_secret`, initialises routing and seen-cache arrays, registers with the radio adapter. Sends the first HELLO beacon after a random jitter to avoid channel collision when many devices boot simultaneously.
2. **`loop()`** — Periodically sends HELLO beacons, expires stale routes and seen-cache entries, and publishes diagnostic sensors.
3. **`on_radio_packet()`** — Called by the radio adapter whenever a packet arrives. Validates mesh ID, suppresses duplicates, then dispatches to `process_hello_()` or `process_data_()`.

---

## Dependencies and Supported Platforms

| Requirement | Details |
|-------------|---------|
| **Radio component** | Either `sx126x` or `sx127x` must be configured in the same YAML |
| **Platform** | ESP32 (Arduino and ESP-IDF frameworks) |

**Note:** The component does not support ESP8266 or RP2040 due to its dependency on the `sx126x`/`sx127x` radio components.

---

## Setup Steps

1. **Wire the LoRa radio** to the ESP32 via SPI (CS, RST, BUSY (sx126x) or DIO0 (sx127x), and DIO1 pins).
2. **Configure the radio component** (`sx126x` or `sx127x`) in your YAML, giving it an `id`.
3. **Add `lora_mesh:`** to your YAML, referencing the radio via `radio_id`.
4. **Choose a `mesh_secret`** — all nodes in the same mesh must share the same secret. Packets from a different secret are silently dropped.
5. **Optionally set `node_id`** — a human-readable name used to address this node. If omitted, a short hex string derived from the last 3 bytes of the MAC address is used.
6. **Flash all nodes** and observe logs for `HELLO sent` and `HELLO from …` messages.

---

## Configuration Reference

### Core Options

```yaml
lora_mesh:
  id: mesh                      # ESPHome object ID (optional, auto-generated)
  radio_id: my_radio            # (Required) ID of the sx126x or sx127x component
  node_id: "sensor-01"          # (Optional) Human-readable node name
  mesh_secret: "my_secret"      # (Required) Shared passphrase — all nodes must match
  gateway: normal               # (Optional) Gateway mode: normal | gateway | auto
```

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `radio_id` | component ID | — | **Required.** Must reference an `sx126x` or `sx127x` component. The type is detected automatically at code-generation time. |
| `node_id` | templatable string | MAC-derived | Human-readable name for this node. Used as the address when other nodes call `lora_mesh.send`. Supports `!lambda`. If omitted, a 6-character hex string derived from the last 3 bytes of the Wi-Fi MAC is used. |
| `mesh_secret` | string | — | **Required.** Shared secret for the mesh. FNV-1a hashed to a 32-bit `mesh_id` embedded in every packet. Nodes with a different secret ignore each other's traffic. |
| `gateway` | enum | `normal` | Controls whether this node announces itself as a gateway. See [Gateway Modes](#gateway-modes). Values: `normal`, `gateway`, `manual`. |

### Routing Options

```yaml
lora_mesh:
  # ... core options above ...
  max_hops: 8                   # Maximum forwarding hops (1–255)
  discovery_interval: 30s       # How often to send HELLO beacons
  route_ttl: 5min               # How long a route stays valid without refresh
  max_routes: 16                # Compile-time routing table capacity (4–255)
  seen_cache_size: 32           # Compile-time duplicate-suppression cache size (8–255)
  seen_cache_ttl: 2min          # How long a seen-packet entry is remembered
  forward_messages: true        # Whether to relay packets on behalf of other nodes
```

| Key | Type | Default | Range | Description |
|-----|------|---------|-------|-------------|
| `max_hops` | int | `8` | 1–255 | Maximum TTL for a new packet. Also the upper bound on routes accepted from HELLO advertisements. |
| `discovery_interval` | time | `30s` | > 0 | Interval between HELLO beacon transmissions. Shorter = faster route discovery, more airtime. |
| `route_ttl` | time | `5min` | > 0 | A route that has not been refreshed within this period is removed. Set longer if nodes beacon infrequently. |
| `max_routes` | int | `16` | 4–255 | **Compile-time constant** — sets the size of the routing table array. Changing this value requires recompilation. When the table is full, the route with the most hops (or lowest RSSI on tie) is evicted. |
| `seen_cache_size` | int | `32` | 8–255 | **Compile-time constant** — size of the circular ring buffer used for duplicate suppression. Larger = fewer false duplicates at the cost of RAM. |
| `seen_cache_ttl` | time | `2min` | > 0 | How long a `(src_id, msg_id)` pair is remembered. Should be at least as long as the longest expected propagation delay across the mesh. |
| `forward_messages` | bool | `true` | — | When `false`, this node will not relay packets for other nodes. Useful for leaf nodes that should not consume airtime forwarding. |

> **Memory note:** Each `RouteEntry` is 24 bytes; each `SeenEntry` is 12 bytes. A default configuration uses `16 × 24 + 32 × 12 = 768 bytes` of static RAM. Adjust `max_routes` and `seen_cache_size` according to your available memory.

### Diagnostic Sensors

All diagnostic sensors are optional. Declare them as separate sensor components and reference them by ID.

```yaml
sensor:
  - platform: template
    id: mesh_node_count
    name: "Mesh Node Count"

binary_sensor:
  - platform: template
    id: mesh_gateway_available
    name: "Gateway Available"

text_sensor:
  - platform: template
    id: mesh_routing_table
    name: "Routing Table"

  - platform: template
    id: mesh_best_gateway
    name: "Best Gateway"

lora_mesh:
  # ... other options ...
  node_count_sensor_id: mesh_node_count
  gateway_available_sensor_id: mesh_gateway_available
  routing_table_sensor_id: mesh_routing_table
  best_gateway_sensor_id: mesh_best_gateway
```

| Key | Sensor type | Description |
|-----|-------------|-------------|
| `node_count_sensor_id` | `sensor` (numeric) | Number of currently known nodes in the routing table. Updated every 30 s and on every routing table change. |
| `gateway_available_sensor_id` | `binary_sensor` | `true` if at least one gateway is reachable (or if this node itself is a gateway). |
| `routing_table_sensor_id` | `text_sensor` | Current routing table as a compact JSON array. Each entry: `{"dst":"XXXXXXXX","nh":"XXXXXXXX","hops":N,"gw":true/false,"rssi":-NN}`. |
| `best_gateway_sensor_id` | `text_sensor` | 8-character hex node-ID of the gateway with the fewest hops (ties broken by highest RSSI), or empty string if none known. |

### Automation Triggers

#### `on_message`

Fires for every DATA packet addressed to this node or a broadcast. The trigger variable `x` is a `MeshMessage` struct (see [MeshMessage Fields](#meshmessage-fields)).

```yaml
lora_mesh:
  on_message:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "From %s: %s (hops=%u rssi=%.0f)",
                   x.source.c_str(), x.payload.c_str(),
                   x.hop_count, x.rssi);
```

#### `on_route_update`

Fires whenever the routing table changes (new route added, route updated, or route expired). No trigger variables.

```yaml
lora_mesh:
  on_route_update:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "Routing table changed, known nodes: %zu",
                   id(mesh).get_known_node_count());
```

#### `on_gateway_changed`

Fires when this node's own gateway-available state transitions (a gateway appeared or disappeared). No trigger variables. Use `id(mesh).has_gateway()` or `id(mesh).is_gateway()` inside the lambda to query the new state.

```yaml
lora_mesh:
  on_gateway_changed:
    then:
      - lambda: |-
          if (id(mesh).has_gateway()) {
            ESP_LOGI("mesh", "Gateway now reachable: %s",
                     id(mesh).get_best_gateway().c_str());
          } else {
            ESP_LOGW("mesh", "No gateway reachable");
          }
```

---

## Actions

### `lora_mesh.send`

Send a unicast message to a named node. Returns `false` (and logs a warning) if no route to the destination is currently known.

```yaml
- lora_mesh.send:
    id: mesh
    destination: "sensor-02"     # Must match the target node's node_id
    payload: "hello"
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `destination` | templatable string | Node ID string of the target. Must match the `node_id` configured on the destination device. |
| `payload` | templatable string | Application data. Maximum 255 bytes. |

### `lora_mesh.broadcast`

Broadcast a message to all nodes in the mesh. All nodes that receive it (directly or via forwarding) will fire their `on_message` trigger.

```yaml
- lora_mesh.broadcast:
    id: mesh
    payload: "alert: high temperature"
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `payload` | templatable string | Application data. Maximum 255 bytes. |

### `lora_mesh.send_to_gateway`

Send a unicast message to the best known gateway (fewest hops, then highest RSSI). Returns `false` if no gateway is currently in the routing table.

```yaml
- lora_mesh.send_to_gateway:
    id: mesh
    # Note: avoid std::to_string() in production; use snprintf into a char buffer instead.
    payload: !lambda |-
      char buf[32];
      snprintf(buf, sizeof(buf), "temp=%.1f", id(temperature).state);
      return std::string(buf);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `payload` | templatable string | Application data. Maximum 255 bytes. |

### `lora_mesh.set_connected`

Notify the mesh that the upstream connection is available or has been lost.  Only has effect when `gateway: manual` is configured; the node will announce itself as a gateway while connected and revert to a normal node when disconnected.  Call this from any automation — `wifi.on_connect`, `mqtt.on_connect`, or a custom health-check.

```yaml
wifi:
  on_connect:
    - lora_mesh.set_connected:
        id: mesh
        connected: true
  on_disconnect:
    - lora_mesh.set_connected:
        id: mesh
        connected: false
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `connected` | templatable bool | `true` = upstream is up (node becomes gateway); `false` = upstream is down (node reverts to normal). |

---

## YAML Examples

### Minimal Node

```yaml
sx126x:
  id: lora_radio
  spi_id: spi_bus
  cs_pin: GPIO12
  rst_pin: GPIO13
  busy_pin: GPIO25
  dio1_pin: GPIO26
  frequency: 433920000
  bandwidth: 125_0kHz
  spreading_factor: 7
  coding_rate: CR_4_6
  modulation: LORA
  rx_start: true

lora_mesh:
  id: mesh
  radio_id: lora_radio
  node_id: "node-01"
  mesh_secret: "my_secret_key"

  on_message:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "Message from %s: %s", x.source.c_str(), x.payload.c_str());
```

### Always-on Gateway

A node that is always a gateway (e.g. connected to MQTT or a serial bridge):

```yaml
lora_mesh:
  id: mesh
  radio_id: lora_radio
  node_id: "gateway-01"
  mesh_secret: "my_secret_key"
  gateway: gateway              # Always announces itself as a gateway

  on_message:
    then:
      - lambda: |-
          // Forward payload to MQTT or serial
          id(mqtt_client).publish("lora/rx", x.payload);
```

### Manual Gateway (upstream-driven)

This node acts as a gateway only while the upstream connection is available.
Use `lora_mesh.set_connected` from any automation (Wi-Fi, MQTT, or a custom health-check) to drive the state:

```yaml
wifi:
  ssid: "MyNetwork"
  password: "MyPassword"
  on_connect:
    - lora_mesh.set_connected:
        id: mesh
        connected: true
  on_disconnect:
    - lora_mesh.set_connected:
        id: mesh
        connected: false

lora_mesh:
  id: mesh
  radio_id: lora_radio
  node_id: "edge-01"
  mesh_secret: "my_secret_key"
  gateway: manual               # Gateway when set_upstream_connected(true) is called

  on_gateway_changed:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "Acting as gateway: %s",
                   id(mesh).is_gateway() ? "yes" : "no");

  on_message:
    then:
      - lambda: |-
          if (id(mesh).is_gateway()) {
            // Only the gateway forwards to the cloud
            id(mqtt_client).publish("lora/rx", x.payload);
          }
```

### Full-featured Node with Diagnostics

```yaml
sensor:
  - platform: template
    id: mesh_node_count
    name: "Mesh Node Count"
    unit_of_measurement: "nodes"

binary_sensor:
  - platform: template
    id: mesh_gateway_ok
    name: "Mesh Gateway Available"

text_sensor:
  - platform: template
    id: mesh_routing_table
    name: "Mesh Routing Table"

  - platform: template
    id: mesh_best_gw
    name: "Mesh Best Gateway"

lora_mesh:
  id: mesh
  radio_id: lora_radio
  node_id: "sensor-barn"
  mesh_secret: "farm_mesh_2025"
  gateway: normal
  max_hops: 6
  discovery_interval: 60s
  route_ttl: 10min
  max_routes: 24
  seen_cache_size: 48
  seen_cache_ttl: 3min
  forward_messages: true

  node_count_sensor_id: mesh_node_count
  gateway_available_sensor_id: mesh_gateway_ok
  routing_table_sensor_id: mesh_routing_table
  best_gateway_sensor_id: mesh_best_gw

  on_message:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "[%u hops] %s -> %s: %s",
                   x.hop_count, x.source.c_str(),
                   x.is_broadcast ? "ALL" : x.destination.c_str(),
                   x.payload.c_str());

  on_route_update:
    then:
      - lambda: |-
          ESP_LOGD("mesh", "Routes: %zu", id(mesh).get_known_node_count());

  on_gateway_changed:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "Gateway available: %s",
                   id(mesh).has_gateway() ? id(mesh).get_best_gateway().c_str() : "none");

# Periodically send sensor data to gateway
interval:
  - interval: 5min
    then:
      - lora_mesh.send_to_gateway:
          id: mesh
          payload: !lambda |-
            char buf[64];
            snprintf(buf, sizeof(buf), "temp=%.1f,hum=%.1f",
                     id(temperature).state, id(humidity).state);
            return std::string(buf);
```

---

## MeshMessage Fields

The `x` variable in `on_message` automations is a `MeshMessage` struct:

| Field | C++ type | Description |
|-------|----------|-------------|
| `x.source` | `std::string` | Hex string of the originating node's ID hash (e.g. `"A1B2C3D4"`). |
| `x.destination` | `std::string` | Hex string of the destination (`"FFFFFFFF"` for broadcasts). |
| `x.prev_hop` | `std::string` | Hex string of the node that immediately forwarded this packet to us. |
| `x.payload` | `std::string` | Application payload (raw bytes as a string). |
| `x.msg_id` | `uint32_t` | Monotonically increasing sequence number from the source node. |
| `x.hop_count` | `uint8_t` | Number of hops already traversed when received. `1` = direct neighbour. |
| `x.ttl` | `uint8_t` | Remaining TTL when received. |
| `x.rssi` | `float` | RSSI (dBm) of the received signal at this node. |
| `x.snr` | `float` | SNR (dB) of the received signal at this node. |
| `x.is_broadcast` | `bool` | `true` if this was a broadcast packet. |
| `x.is_for_this_node` | `bool` | `true` if the destination is this node (unicast). |

---

## Public C++ API

Use these methods inside `!lambda` expressions or custom C++ components:

```cpp
// Sending
bool send_message(const std::string &destination, const std::string &payload);
bool broadcast_message(const std::string &payload);
bool send_to_gateway(const std::string &payload);

// Route queries
bool has_route(const std::string &node_id) const;
bool has_gateway() const;
std::string get_best_gateway() const;   // returns hex node ID, or "" if none
size_t get_known_node_count() const;
std::string get_routing_table_json() const;

// Node identity
std::string get_node_id() const;        // returns human-readable name
bool is_gateway() const;               // true if this node is currently a gateway

// Gateway control (only effective with gateway: manual)
void set_upstream_connected(bool connected);  // drive gateway state from user code

// Maintenance
void clear_routes();                   // wipe routing table and fire on_route_update
```

**Example — conditional send from lambda:**

```yaml
on_press:
  - lambda: |-
      if (id(mesh).has_gateway()) {
        id(mesh).send_to_gateway("button_pressed");
      } else if (id(mesh).has_route("hub-01")) {
        id(mesh).send_message("hub-01", "button_pressed");
      } else {
        id(mesh).broadcast_message("button_pressed");
      }
```

---

## Protocol Overview

### Packet Header (24 bytes, little-endian)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 4 | `mesh_id` | FNV-1a hash of `mesh_secret`. Packets with a mismatched mesh ID are silently dropped. |
| 4 | 1 | `pkt_type` | `1`=HELLO, `2`=DATA (others reserved for future use) |
| 5 | 1 | `flags` | Bitmask: `0x01`=IS_GATEWAY, `0x02`=IS_BROADCAST, `0x04`=ACK_REQUESTED, `0x08`=IS_FORWARD |
| 6 | 4 | `src_id` | FNV-1a hash of the originating node's ID string |
| 10 | 4 | `dst_id` | FNV-1a hash of destination, or `0xFFFFFFFF` for broadcast |
| 14 | 4 | `msg_id` | Per-source monotonic counter for duplicate detection |
| 18 | 1 | `ttl` | Remaining forwarding hops |
| 19 | 1 | `hop_count` | Hops already taken |
| 20 | 4 | `prev_hop` | `src_id` of the radio-level sender (updated on each hop) |

### HELLO Payload (after 24-byte header)

```
[0]  proto_version  (1 byte) — always 1
[1]  route_count    (1 byte) — number of route advertisements that follow
[2+] route entries  (6 bytes each):
     [0..3]  dest_id    (uint32_t LE)
     [4]     hop_count  (uint8_t)
     [5]     rssi_scaled (int8_t, clamped to [-128, 127])
```

Routes are sorted by hop count ascending. The number of routes advertised is limited by the radio's maximum packet size.

### DATA Payload (after 24-byte header)

```
[0]     payload_len  (uint8_t, 0–255)
[1+]    raw payload bytes
```

---

## Routing Algorithm

`lora_mesh` uses a simple **distance-vector** algorithm:

1. **Direct routes** — When a HELLO beacon is received, a direct route to the sender is created with `hop_count = 1`.
2. **Indirect routes** — Routes advertised inside a HELLO are accepted if the new path (`advertised_hops + 1`) has fewer hops than the existing route, or the same hop count but strictly higher RSSI (any improvement, no minimum threshold). Because RSSI measurements fluctuate with RF conditions, this can cause minor route oscillation in borderline-signal scenarios — tuning `route_ttl` longer reduces churn.
3. **Best path selection** — Fewest hops wins. Ties are broken by highest RSSI.
4. **Table full eviction** — When `max_routes` is reached, the entry with the most hops (or lowest RSSI on tie) is evicted to make room.
5. **TTL expiry** — Routes not refreshed within `route_ttl` are removed. Expiry checks run every 10 seconds.
6. **Gateway selection** — The gateway with fewest hops is preferred; ties broken by RSSI.

---

## Gateway Modes

| Mode | Behaviour |
|------|-----------|
| `normal` (default) | Node never announces itself as a gateway. Cannot be a target for `lora_mesh.send_to_gateway`. |
| `gateway` | Node always announces itself as a gateway in every HELLO and DATA packet. |
| `manual` | Node is a gateway **only while** `lora_mesh.set_connected: true` has been called (and `false` has not been called since). What "connected" means is entirely up to the user — it can be Wi-Fi up, MQTT broker reachable, a custom health endpoint, or any other condition. Transitions trigger an immediate HELLO so neighbours learn the new state quickly. |

---

## Limitations

- **No acknowledgements or retransmission** — packet delivery is best-effort. Implement application-level acknowledgements if reliability is required.
- **No encryption** — `mesh_secret` provides network isolation (foreign packets are dropped), but it is **not** encrypted. Payloads are transmitted in plaintext. Add encryption at the application layer if needed.
- **255-byte payload limit** — a single DATA packet can carry at most 255 bytes of payload. For larger data, fragment at the application level.
- **No source routing or on-demand route discovery** — routes must be learnt via HELLO beacons before unicast can succeed. If no route exists, `lora_mesh.send` fails immediately.
- **No loop prevention beyond TTL** — in a poorly configured mesh with very short `route_ttl` or very long `discovery_interval`, routing loops could temporarily consume airtime. Proper parameter tuning avoids this.
- **`max_routes` and `seen_cache_size` are compile-time constants** — changing them requires recompilation and reflashing.
- **Single instance** — `MULTI_CONF = False`; only one `lora_mesh` block is allowed per device.
- **ESP32 only** — depends on `sx126x`/`sx127x` components which are only validated on ESP32 (Arduino and IDF).

---

## Troubleshooting

### No HELLO messages received from neighbors

- Verify all nodes share exactly the same `mesh_secret`.
- Confirm the `sx126x`/`sx127x` frequency, bandwidth, spreading factor, and coding rate are identical across all devices.
- Check that `rx_start: true` is set on the radio component so it starts listening immediately.
- Increase log level to `DEBUG` and watch for `Mesh ID mismatch` messages.

### `send_message` always returns false / "No route to …" in logs

- The destination node must have been heard via a HELLO beacon before a route exists. Wait at least one `discovery_interval` after all nodes boot.
- Verify `node_id` on the destination matches the string used in `destination:`.
- Use `id(mesh).has_route("target-node")` in a lambda to check before sending.

### `send_to_gateway` always returns false / "no gateway in routing table"

- Ensure at least one node in the mesh is configured with `gateway: gateway` or `gateway: manual` (with `lora_mesh.set_connected: true` called).
- Wait for HELLO beacons to propagate. Gateway status is learned from HELLO packets.
- Check `gateway_available_sensor_id` if configured.

### Routing table fills up quickly

- Increase `max_routes` (requires recompile).
- Reduce `route_ttl` so stale routes expire faster.
- Verify devices are not generating spurious routes (e.g. spurious packet sources).

### Duplicate messages received by `on_message`

- The seen-cache size or TTL may be too small. Increase `seen_cache_size` or `seen_cache_ttl`.
- This can also happen if `seen_cache_ttl` expires before a forwarded copy arrives, which is expected in extreme multi-path scenarios.

### High airtime / channel congestion

- Increase `discovery_interval` (e.g. `60s` or `120s`).
- Reduce `max_hops` to limit how far HELLOs propagate.
- Disable `forward_messages` on nodes that do not need to relay traffic.

---

## Developer Notes

### Adding a new radio backend

1. Create a new adapter class in `lora_radio_adapters.h` (or a new file) that extends both `LoRaRadio` and the radio component's listener interface.
2. Implement `transmit_packet()`, `get_max_packet_size()`, and `attach_listener()`.
3. In `__init__.py`, detect the new radio type inside `to_code()` and emit the appropriate `cg.add_define()` and adapter instantiation — following the pattern used for `sx126x` and `sx127x`.

### Extending the protocol

- Add new `PacketType` values in `lora_packet.h`.
- Add a corresponding `process_xxx_()` method in `lora_mesh.h`/`lora_mesh.cpp`.
- Dispatch from `on_radio_packet()` in the `switch` statement.
- Keep the 24-byte header layout unchanged to remain backward compatible with existing nodes.

### Debugging packet flow

Enable verbose (`VERBOSE`) log level to see every received packet:

```yaml
logger:
  level: VERBOSE
```

Key log tags:
- `lora_mesh` — all component logs
- Watch for: `HELLO from`, `DATA from`, `Duplicate from`, `Mesh ID mismatch`, `No route to`, `Forwarded unicast`, `Forwarded broadcast`.

### Inspecting the routing table at runtime

```yaml
on_route_update:
  then:
    - lambda: |-
        ESP_LOGI("mesh", "Routing table: %s",
                 id(mesh).get_routing_table_json().c_str());
```

### Unit IDs are hashes, not strings

Node IDs in logs and `MeshMessage` fields are printed as 8-character hex strings (e.g. `"A1B2C3D4"`). These are FNV-1a 32-bit hashes of the node's `node_id` string, not the string itself. This is intentional — it limits the packet header to a fixed 4 bytes regardless of the name length.
