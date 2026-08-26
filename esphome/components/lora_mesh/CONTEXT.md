# lora_mesh

An ESPHome component that turns a set of LoRa radios (SX126x / SX127x) into a self-healing, multi-hop mesh. It is a transport + discovery + routing layer only: it delivers application payloads via `on_message` and the sending APIs, and deliberately knows nothing about MQTT, HTTP, or any upstream protocol — those live in user automations/lambdas.

## Language

**Node**:
A single device running the component. Identified on the wire by a 32-bit FNV-1a hash of its `node_id` string.
_Avoid_: device, peer, unit.

**Node ID**:
The human-readable string name of a node (e.g. `"sensor-01"`). Used as the address in `lora_mesh.send`. Hashed to 32 bits for transmission.

**Fabric ID**:
A public identifier carried in every header and derived internally from the Fabric Key. It lets Nodes reject unrelated mesh traffic before deeper processing; it is not separately configured and has no security role.
_Avoid_: mesh_secret, mesh_id (the old single field that conflated fabric + security).

**Fabric Key**:
The sole mandatory mesh configuration secret, shared by all Nodes in the Fabric. It authenticates and encrypts every DATA payload and deterministically derives the public Fabric ID; the first-release design has no unprovisioned or plaintext application-traffic state.
_Avoid_: Group Key, mesh secret, password.

**Gateway**:
A node that advertises itself as having Upstream Connectivity, so other nodes can reach the nearest Gateway without naming it. The mesh does not determine or provide that connectivity; the application owns the Gateway's upstream behaviour.
_Avoid_: bridge, relay (relay means something else here — see Forward).

**Nearest Gateway**:
The reachable Gateway selected by fewest LoRa hops, then strongest Path RSSI, then lowest numeric Node ID. The final tie-break makes selection independent of Route insertion order.
_Avoid_: best gateway, strongest gateway.

**Path RSSI**:
The weakest-link RSSI of a multi-hop Route. Each hop extends the Route by taking the minimum of the advertised Path RSSI and the newly measured link RSSI.
_Avoid_: neighbour RSSI, latest RSSI.

**Gateway Withdrawal**:
The immediate, rate-limited propagation of a Gateway becoming unavailable. It clears stale Gateway eligibility across multiple hops without waiting one periodic HELLO interval per hop.
_Avoid_: gateway timeout.

**Send to Gateway**:
Sending application data over LoRa from a Node without Upstream Connectivity to its Nearest Gateway. A Node with Upstream Connectivity sends through its application directly and does not use this mesh operation.
_Avoid_: upstream publish, local gateway delivery.

**Upstream Connectivity**:
An application-owned state indicating that a Node can currently deliver mesh traffic beyond the Fabric. The mesh receives this state from its caller and has no dependency on Wi-Fi, MQTT, or any particular upstream technology.
_Avoid_: Wi-Fi status, MQTT status.

**Forward**:
Re-transmitting another node's packet on behalf of the mesh to extend range (multi-hop). Distinct from being a Gateway. For **unicast** DATA only the designated **Next Hop** forwards (single-path); **broadcast** is forwarded by every node that hasn't seen it (flood).
_Avoid_: relay, repeat.

**Next Hop**:
The single neighbour a node sends a unicast packet to on the way to a destination. Stored per-Route and (post-redesign) carried in the packet so only that neighbour forwards.

**Self-healing**:
Automatic recovery of routing when a path breaks. Passive model: after roughly three missed HELLOs, a neighbour's direct Route expires, Routes that used it as Next Hop are invalidated, and alternatives are re-learned from other neighbours.

**HELLO**:
A periodic single-hop packet (never forwarded) carrying the sender's name, Gateway status, and a distance-vector digest of its known Routes. The sole mechanism by which Routes and Gateways are discovered. HELLO authentication is proposed in ADR-0009 but is not part of the current DATA-security contract.
_Avoid_: beacon, advertisement (used loosely; HELLO is the concrete packet).

**Route**:
A routing-table entry: destination hash, next-hop hash, hop count, RSSI/SNR, gateway flag, expiry. Best route = fewest hops, ties broken by highest RSSI.

**Seen-cache**:
A fixed ring buffer of `(src_id, frame_counter)` pairs used to suppress duplicate processing/forwarding of the same packet.
_Avoid_: dedup table.

**TX queue**:
The bounded outgoing packet queue. Every outbound packet (HELLO, app send, Forward) is enqueued, never transmitted inline; `loop()` drains at most one packet per iteration after a random **TX jitter** backoff (`0..tx_jitter`). Poor-man's CSMA for radios that do blind blocking TX; overflow drops the new packet with a log. Natural future home for CAD/listen-before-talk.
_Avoid_: send buffer, outbox.
