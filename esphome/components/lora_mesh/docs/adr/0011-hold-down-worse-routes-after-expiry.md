---
status: accepted
---

# Hold down worse Routes after expiry

Protocol v4 HELLOs carry distance but no path or destination sequence, so after C disappears from A—B—C, B cannot distinguish A's stale C-via-B advertisement from an independent worse path. When a Route expires, the Node therefore holds down only candidates with a greater hop count than the lost Route for one Route lease plus the bounded expiry-scan interval. A direct HELLO or an equal-or-better alternative clears the hold-down immediately.

An expired Route remains in its existing fixed-capacity Route-table slot as a hold-down tombstone, so table pressure cannot evict active protection; when all slots are protected, new destinations wait rather than weakening loop safety. The state remains local, preserving protocol-v4 compatibility and bounded memory. Expiry runs before periodic HELLO construction, and a HELLO delayed in the TX queue is refreshed against current Route state at actual transmission, so no queued advertisement can grant one final stale lease. A genuinely longer alternative may be delayed by the hold-down, trading bounded recovery latency for freedom from two-node count-to-infinity loops without adding path vectors or sequence numbers to the wire format.
