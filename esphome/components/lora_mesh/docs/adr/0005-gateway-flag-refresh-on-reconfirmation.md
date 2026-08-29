---
status: superseded
superseded_by: 0007-propagate-gateway-status-in-route-advertisements
---

# Refresh Gateway status independently of path metrics

The original direct-neighbour implementation established that Gateway status is independent of Route quality and
must refresh even when hop count, Next Hop, and RSSI do not improve. A status-only change is observable Route state.

ADR-0007 supersedes the direct-neighbour limitation: protocol-v4 Route advertisements now propagate Gateway status
across multiple hops, use Path RSSI, and trigger coalesced Gateway availability or Gateway Withdrawal HELLO updates.
