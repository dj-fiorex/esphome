# `lora_mesh` architecture analysis

The component is a transport, authenticated-discovery, and routing module. It has no upstream-technology dependency:
the application reports Upstream Connectivity through `set_upstream_connected(bool)` or the templatable ESPHome
action. Gateway capability is derived only from that state.

Protocol version 4 uses one mandatory build-time Fabric Key. DATA uses AES-128-CCM with an eight-byte tag, while
HELLO uses a domain-separated control-plane key and an eight-byte truncated HMAC-SHA256 tag over its complete header
and body. HELLO validation precedes all Seen-cache, Node-name, Route, Gateway, diagnostic, and callback changes.

Route advertisements carry destination, hop count, weakest-link Path RSSI, and Gateway status. Nearest Gateway
selection is deterministic: fewest hops, strongest Path RSSI, then lowest unsigned Node ID. Gateway availability and
Gateway Withdrawal changes schedule coalesced HELLO updates; abrupt loss uses ordinary Route expiry.

The hot-path packet, Route, Seen-cache, replay, Node-name, and TX queue storage uses compile-time bounded containers.
Behavior is covered primarily by the host harness around real `LoraMesh` instances and fake radios, with ESPHome
configuration/code-generation tests as the secondary boundary.
