---
status: proposed
---

# Authenticate HELLO and routing advertisements

All Nodes now share one mandatory Fabric Key, so a future protocol revision can authenticate HELLO packets—including Node identity, Gateway status, and Route advertisements—before using them for discovery or routing. HELLO metadata would remain cleartext, but an unauthenticated sender could not create a fake Gateway or poison Routes; this would replace the currently accepted unauthenticated-routing limitation that existed when forwarding Nodes might not hold the payload key.

The proposed design derives a domain-separated control-plane key from the Fabric Key and appends an eight-byte truncated HMAC-SHA256 tag over the complete HELLO header and body. Receivers would verify the tag before the packet affects the Seen-cache, Node-name cache, Routes, or Gateway selection.
