# Single-path unicast forwarding with passive self-healing

The original implementation forwarded "unicast" DATA by re-transmitting on the shared LoRa medium with no next-hop in the packet, so every node with a route to the destination forwarded — i.e. directed flooding costing ~O(N) transmissions per delivered packet, the same as a broadcast, and self-colliding on a no-CSMA radio.

**Decision:** forward DATA along a single path. The packet carries the intended next-hop node hash; a node forwards a unicast packet only if it is that next hop. Broadcast remains flood-based (it is inherently a flood). Self-healing is passive: when a neighbour's direct route expires (we stop hearing its HELLOs), routes using it as next-hop are invalidated and re-learned from other neighbours' HELLOs. Equal-or-better alternatives recover on their next HELLO; worse alternatives observe the bounded Route Hold-down in ADR-0011 so a stale dependent Route cannot count to infinity. Reliability for the rare but critical actuator (valve/pump) commands is provided at the **application layer** via ACK + retry, not by the transport.

Also fixes a prerequisite bug: multi-hop routes must renew their lease when re-confirmed at equal quality (previously they only refreshed on strict improvement, so they expired and flapped every `route_ttl`).

**Why:** the deployment is uplink-dominated to a single gateway and must scale toward a large "unified mesh"; single-path is ~O(path length) instead of ~O(N) and avoids self-collision. Flooding's robustness is not worth paying on every packet when the only reliability-critical traffic is rare downlink commands that need explicit confirmation anyway. See [[lora-mesh-forwarding-model]].

**Consequence:** a wire-format change (new next-hop field → header grows / is repacked), breaking compatibility with any already-deployed nodes. A reactive ROUTE_REQUEST/REPLY mechanism (the reserved packet types) may be added later if passive recovery proves too slow for actuators.

For the bidirectional Jocondo gateway flow, application reliability uses a server-issued monotonic command sequence per destination pot. The pot durably records its highest executed sequence, executes a new sequence once, re-acknowledges duplicates without re-executing them, and returns the acknowledgement through a Gateway; the server retries the same sequence until acknowledged or expired. Locally executed schedules are outside this server-command sequence. This also closes the actuator replay gap across receiver reboots without persisting every transport frame counter.
