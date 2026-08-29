# Keep gateway eligibility independent of upstream technology

`lora_mesh` is a transport, discovery, and routing layer and must not depend on Wi-Fi, MQTT, or any other upstream technology. The application reports whether a Node currently has Upstream Connectivity; the mesh uses that caller-owned state only to decide whether the Node advertises itself as a Gateway, allowing Jocondo to define connectivity using the health signal appropriate to the product.

There are no static, automatic, or manual Gateway modes. Every Node starts without Upstream Connectivity and becomes or ceases to be a Gateway when the application changes that single runtime state. Permanently connected applications set the state once at startup.

The state is exposed as the C++ method `set_upstream_connected(bool)` and the templatable ESPHome action `lora_mesh.set_upstream_connected`; both use the same state-transition implementation and schedule an immediate HELLO when Gateway eligibility changes.

A Jocondo with Upstream Connectivity sends application data upstream directly. `send_to_gateway` is reserved for a Node without Upstream Connectivity and only transports its payload over LoRa to the Nearest Gateway; it does not perform local delivery or know how the Gateway reaches the backend.
