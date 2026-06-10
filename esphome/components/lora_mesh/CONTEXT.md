# lora_mesh

An ESPHome component that turns a set of LoRa radios (SX126x / SX127x) into a self-healing, multi-hop mesh. It is a transport + discovery + routing layer only: it delivers application payloads via `on_message` and the sending APIs, and deliberately knows nothing about MQTT, HTTP, or any upstream protocol — those live in user automations/lambdas.

## Language

**Node**:
A single device running the component. Identified on the wire by a 32-bit FNV-1a hash of its `node_id` string.
_Avoid_: device, peer, unit.

**Node ID**:
The human-readable string name of a node (e.g. `"sensor-01"`). Used as the address in `lora_mesh.send`. Hashed to 32 bits for transmission.

**Fabric ID**:
A *shared, non-secret* identifier (cleartext in every header) that makes all VasoSmart pots relay for each other and ignore unrelated LoRa traffic — the "unified mesh". Gates forwarding/membership only; has no security role.
_Avoid_: mesh_secret, mesh_id (the old single field that conflated fabric + security).

**Group** (a.k.a. Channel):
A logical tenant partition (one condo, one museum) inside the one unified Fabric. Membership is defined by holding the Group Key. Relays forward all Groups' packets; only members can read/act on a Group's payloads — this is the "divided in software".
_Avoid_: tenant, network (use Group).

**Group Key**:
The per-Group secret, **provisioned at runtime over BLE** by the mobile app and persisted in NVS. Used to authenticate-encrypt DATA payloads (AES-128-CCM + MIC + replay counter). A node with no Group Key is *unprovisioned* and inert.
_Avoid_: mesh_secret, password.

**Gateway**:
A node that advertises itself (via a header flag) as having upstream connectivity, so other nodes can reach "the best gateway" without naming it. The component does NOT bridge to MQTT/internet itself — a gateway is just a node whose `on_message`/lambdas happen to forward traffic upstream. Modes: `normal`, `gateway` (always), `auto` (while Wi-Fi connected).
_Avoid_: bridge, relay (relay means something else here — see Forward).

**Forward**:
Re-transmitting another node's packet on behalf of the mesh to extend range (multi-hop). Distinct from being a Gateway. For **unicast** DATA only the designated **Next Hop** forwards (single-path); **broadcast** is forwarded by every node that hasn't seen it (flood).
_Avoid_: relay, repeat.

**Next Hop**:
The single neighbour a node sends a unicast packet to on the way to a destination. Stored per-Route and (post-redesign) carried in the packet so only that neighbour forwards.

**Self-healing**:
Automatic recovery of routing when a path breaks. Passive model: when a neighbour's direct Route expires (its HELLOs stop), Routes that used it as Next Hop are invalidated and re-learned from other neighbours.

**HELLO**:
A periodic single-hop beacon (never forwarded) carrying the sender's name and a distance-vector digest of its known routes. The sole mechanism by which routes are discovered.
_Avoid_: beacon, advertisement (used loosely; HELLO is the concrete packet).

**Route**:
A routing-table entry: destination hash, next-hop hash, hop count, RSSI/SNR, gateway flag, expiry. Best route = fewest hops, ties broken by highest RSSI.

**Seen-cache**:
A fixed ring buffer of `(src_id, msg_id)` pairs used to suppress duplicate processing/forwarding of the same packet.
_Avoid_: dedup table.
