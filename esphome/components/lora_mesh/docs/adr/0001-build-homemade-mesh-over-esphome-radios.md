# Build a homemade mesh layer instead of adopting an existing LoRa mesh library

We need a multi-hop LoRa mesh for the VasoSmart smart-pot product (ESP32-S3 + SX126x). We evaluated adopting LoRaMesher (MIT, distance-vector), MeshCore (MIT, hybrid routing), and Meshtastic (GPL-3.0, full firmware) instead of maintaining our own ~700-line component.

**Decision:** continue with the homemade ESPHome-native component, and use LoRaMesher's MIT source only as a reference for proven algorithms.

**Why:** the component's core value is reusing ESPHome's existing `sx126x`/`sx127x` drivers and fitting ESPHome's cooperative `loop()` model. Every library alternative is built on **RadioLib** and FreeRTOS tasks, so adopting one means abandoning the ESPHome radio components (the whole premise) or performing major surgery to rehost their routing core — more work than hardening the code we have. Meshtastic is additionally a GPL-3.0 *application* (not a library), which is a poor fit for a one-chip commercial product. Our traffic profile (periodic sensor uplink + rare actuator commands) is modest and does not need the libraries' sophistication. The known problems (route-refresh flap, flooding-vs-unicast model, blocking TX, `std::string` on the hot path) are a bounded hardening backlog, not a rewrite.

**Consequence:** we own routing correctness and all its edge cases indefinitely. If the team cannot sustain that, the fallback is porting LoRaMesher's engine (same distance-vector family, least impedance) at the cost of ESPHome radio reuse.
