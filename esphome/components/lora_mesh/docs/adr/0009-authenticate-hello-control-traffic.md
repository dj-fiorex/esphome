# Authenticate HELLO and routing advertisements

All Nodes now share one mandatory Fabric Key, so protocol version 4 authenticates HELLO packets—including Node identity, Gateway status, and Route advertisements—before using them for discovery or routing. HELLO metadata remains cleartext, but an unauthenticated sender cannot create a fake Gateway or poison Routes; this replaces the earlier accepted unauthenticated-routing limitation that existed when forwarding Nodes might not hold the payload key.

The component derives a domain-separated control-plane key from the Fabric Key and appends an eight-byte truncated HMAC-SHA256 tag over the complete HELLO header and body. Receivers verify the tag before the packet affects the Seen-cache, Node-name cache, Routes, or Gateway selection.
