# Use the last uplink Gateway for downlink affinity

Each MQTT uplink forwarded by a Gateway identifies both the source Node and forwarding Gateway. The server records that Gateway as the source Node's current downlink affinity and publishes later commands to the Gateway's dedicated MQTT topic; the Gateway then uses named LoRa unicast to reach the destination Node.

This first-release strategy reuses the Node's own Nearest-Gateway choice and avoids asking every connected Gateway to transmit the same command, which would waste airtime and create collisions. Affinity is refreshed by subsequent uplinks rather than by exporting the mesh routing table to the server.

If the affinity Gateway disappears while a command is pending, the server retains the same command sequence and waits for the destination Node's next uplink to establish new affinity. It then republishes the pending command to the new Gateway; no all-Gateway fallback broadcast is used, and the Node's durable sequence makes late duplicate delivery harmless.
