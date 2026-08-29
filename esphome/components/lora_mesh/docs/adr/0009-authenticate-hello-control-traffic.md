---
status: accepted
---

# Authenticate HELLO and routing advertisements

All Nodes share one mandatory Fabric Key, so protocol v4 authenticates HELLO packets—including Node identity, Gateway status, and Route advertisements—before using them for discovery or routing. HELLO metadata remains cleartext, but an unauthenticated sender cannot create a fake Gateway or poison Routes.

The control-plane key is `HMAC-SHA256(Fabric Key, "LORA-MESH-CONTROL-v1")`. Every HELLO appends the first eight bytes of HMAC-SHA256 over its complete header and body under that key. Receivers validate the version, exact packet shape, and tag before the packet affects the Seen-cache, Node-name cache, direct or advertised Routes, Gateway selection, diagnostics, or callbacks.
