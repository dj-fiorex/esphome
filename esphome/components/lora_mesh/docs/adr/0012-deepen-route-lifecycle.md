# Deepen Route lifecycle behind the RouteTable interface

Issue #38 identifies three responsibilities that should leave `LoraMesh`. The first deepening slice is Route lifecycle
and convergence because the current implementation spreads one set of invariants across HELLO processing, periodic
expiry, send operations, Gateway propagation, and diagnostics.

**Decision:** introduce an in-process `RouteTable` module. Its interface accepts direct-neighbour observations and
normalized Route candidates and exposes current Route lookup, Nearest Gateway lookup, immutable iteration for HELLOs
and diagnostics, Gateway-update acknowledgement, expiry, and clearing. Its implementation owns fixed-capacity slot
allocation, Route choice and reconfirmation, Path RSSI refresh, Gateway-state tracking, lease renewal, Route Hold-down,
dependent Route invalidation, and deterministic Nearest Gateway ordering.

`LoraMesh` remains the public ESPHome/C++ interface. It owns radio packet parsing, turns authenticated HELLO bytes into
Route observations, schedules HELLOs, publishes diagnostics, and invokes callbacks in the same places as before. The
new seam is internal and in-process, so it needs no adapter. Focused host tests exercise convergence through the
`RouteTable` interface while the existing host suite continues to exercise behavior through the public `LoraMesh`
interface.

**Deletion test:** deleting `RouteTable` would redistribute its selection, lease, hold-down, invalidation, capacity,
Gateway propagation, and ordering rules across HELLO processing, `loop()`, the send methods, and diagnostics. The
module therefore concentrates real complexity rather than passing calls through.

**Consequence:** this slice intentionally preserves protocol v4, ESPHome configuration, public C++ behavior, callback
timing, fixed memory bounds, and radio behavior. Protocol admission and outbound airtime remain later deepening slices.
