# Deepen Route lifecycle, protocol admission, and outbound airtime

Issue #38 identifies three responsibilities that should leave `LoraMesh`. The first deepening slice is Route lifecycle
and convergence because the current implementation spreads one set of invariants across HELLO processing, periodic
expiry, send operations, Gateway propagation, and diagnostics.

**Decision:** introduce an in-process `RouteTable` module. Its interface accepts direct-neighbour observations and
normalized Route candidates and exposes current Route lookup, Nearest Gateway lookup, immutable iteration for HELLOs
and diagnostics, Gateway-update acknowledgement, expiry, and clearing. Its implementation owns fixed-capacity slot
allocation, Route choice and reconfirmation, Path RSSI refresh, Gateway-state tracking, lease renewal, Route Hold-down,
dependent Route invalidation, and deterministic Nearest Gateway ordering.

`LoraMesh` remains the public ESPHome/C++ interface. It orchestrates admitted packets, turns authenticated HELLO bytes
into Route observations, schedules HELLOs, publishes diagnostics, and invokes callbacks in the same places as before. The
new seam is internal and in-process, so it needs no adapter. Focused host tests exercise convergence through the
`RouteTable` interface while the existing host suite continues to exercise behavior through the public `LoraMesh`
interface.

**Deletion test:** deleting `RouteTable` would redistribute its selection, lease, hold-down, invalidation, capacity,
Gateway propagation, and ordering rules across HELLO processing, `loop()`, the send methods, and diagnostics. The
module therefore concentrates real complexity rather than passing calls through.

After the Route slice established the pattern, two more in-process modules complete issue #38:

- `PacketAdmission` first inspects raw bytes through the header/Fabric gate, then authenticates the inspected frame and
  returns either one named failure or decrypted DATA plaintext. The staged interface preserves the pre-existing
  link-simulation and self-echo checkpoints without parsing the header twice. It owns header parsing, exact HELLO/DATA
  shape validation, HELLO HMAC verification, and DATA CCM authentication, so admission ordering cannot diverge among
  local delivery, broadcast, and Forwarding. Deleting it would redistribute security-critical parsing and
  authentication into the dispatcher.
- `OutboundAirtime` accepts completed packets and owns the fixed-capacity FIFO, per-head random jitter, at-most-one
  attempt per loop, radio outcome handling, failure drop policy, and just-in-time refresh of stale queued HELLOs. Its
  fixed function hooks let `LoraMesh` build and acknowledge HELLO content without adding a second radio adapter or
  heap-backed callback. Deleting it would redistribute airtime policy across application sends, Forwarding, HELLO
  scheduling, and the component loop.

**Consequence:** these slices preserve protocol v4, ESPHome configuration, public C++ behavior, callback timing, fixed
memory bounds, and radio behavior. `LoraMesh` orchestrates the three deep modules but no longer implements their policy.
