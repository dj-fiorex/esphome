# LoRa Mesh

`lora_mesh` turns ESPHome SX126x and SX127x radios into a self-healing, multi-hop Fabric. It owns LoRa transport,
authenticated discovery, routing, Forwarding, and payload delivery. The application owns MQTT, HTTP, Wi-Fi, cellular,
or any other upstream technology.

Every Node shares one mandatory 128-bit Fabric Key. The key derives the public Fabric ID, encrypts and authenticates
DATA with AES-128-CCM, and derives a separate control-plane key that authenticates every HELLO with HMAC-SHA256.

## Core model

- Every Node may Forward packets; Forwarding is not a configured role.
- A Node is a Gateway only while its caller reports Upstream Connectivity.
- Every Node starts with Upstream Connectivity set to `false`.
- `lora_mesh` never imports or inspects Wi-Fi, MQTT, HTTP, Appwrite, or another upstream implementation.
- Gateway changes and learned Gateway Withdrawals schedule a coalesced, rate-limited HELLO.
- The Nearest Gateway is selected by fewest hops, strongest weakest-link Path RSSI, then lowest unsigned Node ID.

## Minimal configuration

Store the real key in `secrets.yaml`:

```yaml
lora_mesh_fabric_key: "00112233445566778899aabbccddeeff"
```

Reference it from the Node configuration:

```yaml
lora_mesh:
  id: mesh
  radio_id: lora_radio
  node_id: "pot-01"
  fabric_key: !secret lora_mesh_fabric_key

  on_message:
    then:
      - lambda: |-
          ESP_LOGI("mesh", "From %s: %.*s", x.source, static_cast<int>(x.payload.size()),
                   reinterpret_cast<const char *>(x.payload.data()));
```

### Configuration reference

| Option | Default | Description |
|---|---:|---|
| `radio_id` | required | SX126x or SX127x radio used by the mesh. |
| `fabric_key` | required | Exactly 32 hexadecimal characters (128 bits). Use `!secret` in production. |
| `node_id` | MAC-derived | Templatable human-readable Node ID. |
| `max_hops` | `8` | Maximum new-packet TTL and accepted Route length (`1` through `254`). |
| `discovery_interval` | `30s` | Periodic authenticated HELLO interval; minimum `5ms`. |
| `route_ttl` | `90s` | Route lease; the default detects abrupt loss after about three missed HELLOs. |
| `max_routes` | `16` | Compile-time Route-table capacity. |
| `seen_cache_size` | `32` | Compile-time duplicate Seen-cache capacity. |
| `seen_cache_ttl` | `2min` | Duplicate Seen-cache lifetime. |
| `forward_messages` | `true` | Whether eligible DATA is Forwarded. |
| `tx_jitter` | `100ms` | Random backoff before each queued radio transmission. |
| `tx_queue_size` | `8` | Compile-time bounded TX queue capacity. |
| `link_sim` | `false` | Debug-only neighbour blocklist used by the three-board test. |

When `node_id` is omitted, the Node ID is the last three raw MAC bytes formatted as six uppercase hexadecimal
characters (for example, MAC `02:00:00:AB:CD:EF` produces `ABCDEF`). This value is stable for a given MAC and is
hashed with 32-bit FNV-1a exactly like a configured Node ID to produce the wire identity.

There are no static, automatic, or manual Gateway modes. There is no `on_gateway_changed` trigger; applications query
`has_gateway()` or handle `send_to_gateway()` returning `false` on their normal send schedule.

## Reporting Upstream Connectivity

The application reports its own policy through C++:

```cpp
id(mesh).set_upstream_connected(backend_session_is_healthy);
```

Or through the templatable ESPHome action:

```yaml
- lora_mesh.set_upstream_connected:
    id: mesh
    connected: !lambda "return id(application_backend_ready);"
```

The action and C++ API share one transition path. Setting the existing value is a no-op. A real change schedules an
authenticated HELLO; rapid changes are coalesced so they cannot enqueue an unbounded burst.

## Gateway availability and failure detection

A Node that learns a Gateway promotion or Gateway Withdrawal schedules the same coalesced HELLO update. Each hop can
therefore propagate the final Gateway state after the one-second immediate-update rate limit, instead of waiting for
the next 30-second periodic HELLO. Repeated Upstream Connectivity flapping keeps only a pending update during the
rate-limit window, and the HELLO built when that window closes carries the final stable state. If one HELLO cannot fit
every Route, changed Gateway Routes take priority and any overflow remains pending for later rate-limited HELLOs.
Pending Gateway changes are not evicted to admit newly learned Routes before their update is advertised.

A Gateway that loses power cannot send a Gateway Withdrawal. That case deliberately uses ordinary self-healing:
neighbour and dependent Routes expire, then `send_to_gateway()` evaluates the remaining Routes and selects the current
alternative. A bounded Route Hold-down rejects only worse indirect candidates after expiry so neighbours cannot feed a
stale dependent Route back to each other; direct and equal-or-better alternatives remain immediately eligible. Active
hold-down tombstones retain their fixed Route-table slots, and delayed queued HELLOs are refreshed before transmission.
The default 30-second `discovery_interval` and 90-second `route_ttl` model roughly three missed HELLOs. There is no
Gateway-specific heartbeat, probe, timeout, Route discovery exchange, payload persistence, or mesh retry.

## Sending

```yaml
- lora_mesh.send:
    id: mesh
    destination: "pot-02"
    payload: "open-valve"

- lora_mesh.broadcast:
    id: mesh
    payload: "configuration-changed"

- lora_mesh.send_to_gateway:
    id: mesh
    payload: !lambda 'return id(encoded_status);'
```

`send_to_gateway()` evaluates current Routes on every call. It does not retain the selected Gateway or payload. It
returns `false` when no Gateway is reachable or the TX queue cannot accept the packet; persistence and retry policy
belong to the application.

### Application integration contract

An online Jocondo publishes directly through its own upstream integration and does not call `send_to_gateway()` for
local delivery. An offline Jocondo owns buffering and retry around `send_to_gateway()`: it retains the application
record and retries on its normal schedule when the mesh returns `false`.

When a Gateway publishes a LoRa uplink, the application includes both the source Node and the receiving Gateway. The
server records that Gateway as the Node's current downlink affinity. For a later downlink, the server targets that
Gateway and the Gateway calls named `send_message(destination, payload)` unicast. MQTT topics, upstream sessions,
application acknowledgements, durable command sequencing, and retries remain outside `lora_mesh`.

## Automations and diagnostics

`on_message` receives a `MeshMessage` with source, optional source name, destination, previous hop, payload, counter,
hop count, TTL, RSSI, SNR, and broadcast/destination flags. `MeshMessage.payload` is an owning, fixed-capacity
`StaticVector<uint8_t, MESH_MAX_DATA_PAYLOAD_SIZE>`. It stays valid when ESPHome copies the message into a delayed
automation action, and the payload copy itself does not allocate. ESPHome actions and the scheduler can have their own
allocation behavior outside the component's packet dispatch. This is a breaking change from the former owning
`std::string`; text consumers should use `payload.data()` with `payload.size()` as shown above. `on_route_update` fires
when observable Route state changes.

Optional diagnostics are connected by ID:

```yaml
lora_mesh:
  # ...
  node_count_sensor_id: mesh_node_count
  gateway_available_sensor_id: mesh_gateway_available
  routing_table_sensor_id: mesh_routing_table
  nearest_gateway_sensor_id: mesh_nearest_gateway
```

`nearest_gateway_sensor_id` publishes the eight-character hexadecimal Node ID selected by hops, Path RSSI, and Node
ID tie-break. `gateway_available_sensor_id` is true when this Node has Upstream Connectivity or can reach a Gateway.

## Public C++ interface

```cpp
bool send_message(const std::string &destination, std::span<const uint8_t> payload);
bool broadcast_message(std::span<const uint8_t> payload);
bool send_to_gateway(std::span<const uint8_t> payload);
// Convenience overloads accepting const std::string & remain available.
void set_upstream_connected(bool connected);
bool is_upstream_connected() const;
bool has_route(const std::string &node_id) const;
bool has_gateway() const;
std::string get_nearest_gateway() const;
std::string get_node_id() const;
const char *get_node_name(uint32_t id) const;
void clear_routes();
std::string get_routing_table_json() const;
size_t get_known_node_count() const;
```

The byte-span overloads and fixed-capacity packet buffers do not allocate in the send path. Receive decryption uses a
fixed stack buffer and copies plaintext into the message's inline fixed-capacity payload without allocating. The
owning buffer, rather than a non-owning view, preserves payload lifetime when ESPHome stores automation arguments for
later actions. Diagnostic string-returning helpers (`get_routing_table_json()`, `get_nearest_gateway()`, and the
link-simulator blocklist formatter) retain owning strings because they run only on explicit diagnostic/configuration
paths, not per-packet builders or dispatch.

## Protocol v4 security

Routing metadata remains visible. A HELLO is:

```text
28-byte header || version || name length || name || Route count || 7-byte Routes || 8-byte HELLO tag
```

Each Route carries destination ID, hop count, Path RSSI, and Gateway status. The HELLO tag is the first eight bytes of
HMAC-SHA256 over the complete header and body under a domain-separated control-plane key. Receivers validate version,
shape, and tag before changing the Seen-cache, Node-name cache, direct or advertised Routes, Gateway visibility,
diagnostics, or callbacks.

DATA uses AES-128-CCM with an eight-byte tag and a maximum application payload of 218 bytes. A persistent sender frame
counter prevents nonce reuse. Runtime replay high-water marks reject repeated authenticated DATA during one boot.

See [wire-format.md](docs/wire-format.md) for exact byte layouts and derivations.

## Tests and hardware example

Run deterministic host behavior tests:

```bash
tests/components/lora_mesh/host_tests/run.sh
```

The YAML/code-generation fixtures under `tests/components/lora_mesh/` cover validation, the templatable Upstream
Connectivity action, diagnostics, and ESP32 Arduino/ESP-IDF builds. `test-three-pot.esp32-s3-idf.yaml` is the compiled
fixture for the runnable `three-pot.esp32-s3-idf.yaml` hardware scenario. Its Upstream Connectivity switch demonstrates
Gateway promotion and withdrawal without coupling the mesh to Wi-Fi. See
[three-pot-scenario.md](docs/three-pot-scenario.md) for the A→B→C setup and Jocondo integration contract.
