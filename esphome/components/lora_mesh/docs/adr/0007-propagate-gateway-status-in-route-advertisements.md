# Propagate Gateway status in route advertisements

`send_to_gateway` must reach the Nearest Gateway even when that Gateway is multiple LoRa hops away. Every HELLO Route advertisement therefore carries a route-flags byte containing the destination's Gateway status; receivers propagate that status with the Route. Gateway selection is ordered by fewest hops, strongest Path RSSI, then lowest numeric Gateway Node ID so an exact metric tie is deterministic. Path RSSI is the weakest link across the Route, calculated at each hop as the minimum of the advertised Path RSSI and the newly measured link RSSI.

All Nodes share one Fabric Key, so every reachable Gateway is eligible to serve every Node in the Fabric. Gateway advertisements therefore require no tenant, Group, or key-capability information.

This changes each Route advertisement from six to seven bytes and bumps the wire protocol from version 3 to version 4. No compatibility mode is required because protocol version 3 has not been deployed.

When a Node's Gateway status changes, it sends an immediate HELLO. Each Node that learns a Gateway-status change also schedules an immediate, rate-limited HELLO, producing a Gateway Withdrawal or availability wave across the Fabric without waiting one periodic HELLO interval per hop. Rate limiting prevents an unstable upstream connection from causing a HELLO storm.

`send_to_gateway` selects from the current routing table on every call and does not retain application payloads. If no Gateway is reachable, it returns `false` immediately; buffering, persistence, and retry policy belong to Jocondo rather than the mesh transport.

The public and internal API uses the canonical Nearest Gateway terminology: `get_nearest_gateway()`, `find_nearest_gateway_route_()`, and `nearest_gateway_sensor_id`. The former "best gateway" names are removed without aliases because no version has been deployed; `has_gateway()` and `send_to_gateway()` remain unchanged.

If a Gateway disappears abruptly and cannot send a Gateway Withdrawal, its ordinary Route lease is the failure detector; no separate Gateway heartbeat or timeout is added. Production configuration targets Route expiry after roughly three missed HELLOs, after which the next call selects the next Nearest Gateway.

The first-release API has no `on_gateway_changed` callback. It has no functional Jocondo consumer, and retry scheduling belongs to the application; Jocondo queries `has_gateway()` or handles `send_to_gateway()` returning `false` when its normal send schedule runs.
