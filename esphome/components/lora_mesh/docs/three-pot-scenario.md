# Three-pot protocol-v4 scenario

This scenario proves the complete ESPHome surface on three Heltec WiFi LoRa 32 V3 boards. All three Nodes use the
same protocol-v4 Fabric, while the application explicitly owns each Node's Upstream Connectivity state.

The runnable configuration is
[`tests/components/lora_mesh/three-pot.esp32-s3-idf.yaml`](../../../../tests/components/lora_mesh/three-pot.esp32-s3-idf.yaml).
Its adjacent `secrets.example.yaml` contains only an all-zero Fabric Key and dummy Wi-Fi values. Copy that example to
the ignored `secrets.yaml` so the scenario exercises ESPHome's `!secret` resolution. Never commit the copied file or
replace the tracked example with real credentials.

## Build and flash

Validate the configuration and compile the representative ESP-IDF firmware:

```bash
cp tests/components/lora_mesh/secrets.example.yaml tests/components/lora_mesh/secrets.yaml
esphome config tests/components/lora_mesh/three-pot.esp32-s3-idf.yaml
esphome compile tests/components/lora_mesh/three-pot.esp32-s3-idf.yaml
```

Flash the same firmware to all three boards. `name_add_mac_suffix` gives each board a distinct Node ID. Open each
board's web UI, note its **My node id** value, and assign the physical boards as A, B, and C.

## Force Node A → forwarding Node B → Gateway Node C

On a bench, A and C normally hear each other and choose a direct Route. The test-only `link_sim` controls force the
two-hop topology without changing transmit power or moving the boards:

1. On A, enter C's Node ID in **Block neighbor (name)**.
2. On C, enter A's Node ID in **Block neighbor (name)**.
3. Leave B's blocklist empty and wait for authenticated HELLO discovery.
4. Confirm A's **Mesh routing table** shows C at two hops. **Mesh nearest gateway** remains empty until a Gateway is
   promoted.

The filter acts on the immediate radio sender. A therefore rejects C's direct traffic but still accepts C traffic
forwarded by B, and C behaves symmetrically.

## Promote, deliver, and withdraw

Every board starts without Upstream Connectivity. On C, turn on **Upstream Connectivity**. The switch invokes the
templatable `lora_mesh.set_upstream_connected` action, C advertises Gateway availability, and B propagates it to A.
Confirm that A's **Mesh nearest gateway** becomes C's Node ID and **Mesh gateway available** becomes true.

Keep A offline and press **Send to gateway** on A, or leave **Auto traffic** enabled. A calls
`lora_mesh.send_to_gateway`; B is the single designated forwarding Node; C's **Last message** displays A as the source
with `h1`: the packet crossed two LoRa links and one forwarding Node. This proves offline delivery across B rather
than a direct A↔C exchange.

Turn off **Upstream Connectivity** on C. That caller-owned transition emits an authenticated Gateway Withdrawal,
which B propagates to A. A's Nearest Gateway diagnostic clears and a subsequent send returns `false` unless another
reachable Node has been promoted. Turning C back on demonstrates promotion again. Clearing the A/C link-sim blocks
lets the Routes reconverge directly; blocking B instead demonstrates passive expiry and healing.

## Application ownership

An online Jocondo publishes directly upstream through its application integration. It does not call
`send_to_gateway` for local delivery. An offline Jocondo owns buffering and retry around `send_to_gateway`: retain the
application record until the call is accepted and the application-level delivery policy is satisfied, and try again
on its normal schedule after `false`. The mesh retains neither the payload nor a selected Gateway.

For a LoRa uplink, the Gateway integration publishes both the source Node and the receiving Gateway. The server
records that Gateway as the Node's downlink affinity. A later server downlink is sent to that Gateway, which calls
named unicast with the destination Node ID. If affinity goes stale, the server waits for a later uplink to establish a
new Gateway before retrying the same idempotent application command.

This contract gives `lora_mesh` no MQTT knowledge: it transports bytes and Node identities, while the application
owns upstream publication, topics, persistence, acknowledgement, command sequencing, and retry.
