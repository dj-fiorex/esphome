// Behavior tests for the lora_mesh wire format and single-path unicast forwarding.

#include "test_harness.h"

int g_failures = 0;

using namespace lmtest;

static const uint32_t FABRIC = 0xDEFF3662;
static const uint32_t NODE_A = fnv1a_str("node-a");
static const uint32_t NODE_B = fnv1a_str("node-b");
static const uint32_t NODE_C = fnv1a_str("node-c");
static const uint32_t NODE_D = fnv1a_str("node-d");

// AES-128-CCM(TEST_FABRIC_KEY, nonce="LORA-FABRICID", empty plaintext,
// AAD="LORA-MESH-ID-v1", tag=8) starts with 62 36 ff de. The public Fabric ID
// uses those first four tag bytes as little-endian.

/// Advance the fake clock and run one loop() so periodic work (HELLO beacon,
/// route/seen-cache expiry) gets a chance to fire at the new time.
static void advance_and_loop(TestNode &n, uint32_t ms) {
  esphome::test_clock_advance(ms);
  n.mesh.loop();
}

static void test_protocol_v4_derives_fabric_id_and_uses_eight_byte_data_tag() {
  TestNode a("node-a");

  EXPECT_TRUE(a.mesh.broadcast_message("hi"));
  a.mesh.loop();

  EXPECT_EQ(esphome::lora_mesh::MESH_PROTO_VERSION, 4);
  EXPECT_EQ(esphome::lora_mesh::DATA_AUTH_TAG_SIZE, 8u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][0]), FABRIC);
  EXPECT_EQ(a.radio.sent[0].size(), HDR + 1 + 2 + 8);
}

// ── Slice 1: protocol-v4 header on transmitted DATA ─────────────────────────

static void test_broadcast_data_has_v4_header() {
  TestNode a("node-a");

  EXPECT_TRUE(a.mesh.broadcast_message("hi"));
  a.mesh.loop();  // drain the TX queue

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  // 28-byte header + payload_len byte + 2 ciphertext bytes + 8 tag bytes
  EXPECT_EQ(pkt.size(), HDR + 1 + 2 + DATA_AUTH_TAG_SIZE);
  EXPECT_EQ(get_u32_le(&pkt[0]), FABRIC);      // fabric_id
  EXPECT_EQ(pkt[4], PKT_DATA);                 // pkt_type
  EXPECT_TRUE(pkt[5] & FLAG_BROADCAST);        // flags
  EXPECT_EQ(get_u32_le(&pkt[6]), NODE_A);      // src_id
  EXPECT_EQ(get_u32_le(&pkt[10]), BROADCAST);  // dst_id
  EXPECT_EQ(get_u32_le(&pkt[14]), 2u);         // frame_counter (frame 1 = initial HELLO)
  EXPECT_EQ(pkt[18], 8);                       // ttl = default max_hops
  EXPECT_EQ(pkt[19], 0);                       // hop_count
  EXPECT_EQ(get_u32_le(&pkt[20]), NODE_A);     // prev_hop = originator
  EXPECT_EQ(get_u32_le(&pkt[24]), BROADCAST);  // next_hop = any (flood)
  EXPECT_EQ(pkt[HDR], 2);                      // payload_len

  // Verify the ciphertext decrypts to "hi" with the test Fabric Key.
  uint8_t plaintext[2];
  bool ok = esphome::lora_mesh::mesh_decrypt_payload(TEST_FABRIC_KEY, NODE_A, BROADCAST, 2, PKT_DATA, 2, &pkt[HDR + 1],
                                                     plaintext, &pkt[HDR + 1 + 2]);
  EXPECT_TRUE(ok);
  EXPECT_EQ(plaintext[0], 'h');
  EXPECT_EQ(plaintext[1], 'i');

  static_assert(esphome::lora_mesh::MESH_PROTO_VERSION == 4, "proto version must be 4");
  static_assert(esphome::lora_mesh::MESH_HEADER_SIZE == 28, "header must be 28 bytes");
}

static void test_oversize_payload_truncated_consistently() {
  TestNode a("node-a");
  // 250 bytes does not fit: 255 radio limit - 28 header - 1 len byte - 8 tag bytes = 218 max.
  std::string big(250, 'x');

  EXPECT_TRUE(a.mesh.broadcast_message(big));
  a.mesh.loop();

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  EXPECT_TRUE(pkt.size() <= 255u);
  // payload_len + ciphertext + tag = pkt.size() - HDR - 1
  uint8_t payload_len = pkt[HDR];
  EXPECT_EQ(static_cast<size_t>(payload_len) + DATA_AUTH_TAG_SIZE, pkt.size() - HDR - 1);
  EXPECT_EQ(payload_len, 218u);
}

// ── Slice 2: HELLO at protocol v4 ───────────────────────────────────────────

static void test_own_hello_has_v4_body_at_offset_28() {
  TestNode a("node-a");

  // The initial HELLO is consumed by the fixture; trigger the next beacon.
  advance_and_loop(a, 30000);

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  EXPECT_EQ(pkt[4], PKT_HELLO);
  EXPECT_EQ(get_u32_le(&pkt[10]), BROADCAST);  // dst
  EXPECT_EQ(get_u32_le(&pkt[24]), BROADCAST);  // next_hop
  EXPECT_TRUE(pkt.size() >= HDR + 3);
  EXPECT_EQ(pkt[HDR], 4);      // proto_version
  EXPECT_EQ(pkt[HDR + 1], 6);  // name_len
  EXPECT_TRUE(std::string(pkt.begin() + HDR + 2, pkt.begin() + HDR + 8) == "node-a");
  EXPECT_EQ(pkt[HDR + 8], 0);  // route_count (no routes yet)
}

static void test_hello_builds_direct_and_advertised_routes() {
  TestNode a("node-a");

  // HELLO from B advertising a 1-hop route to C.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}));

  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  EXPECT_FALSE(a.mesh.has_route("node-x"));
}

static void test_protocol_v3_hello_is_rejected_without_route_state() {
  TestNode a("node-a");
  auto packet = make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}});
  packet[HDR] = 3;

  a.receive(packet);

  EXPECT_FALSE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_route("node-c"));
}

static void test_fabric_mismatch_drops_packet() {
  TestNode a("node-a");
  const uint32_t wrong_fabric = fnv1a_str("fabric-2");

  a.receive(make_hello(wrong_fabric, NODE_B, "node-b"));
  EXPECT_FALSE(a.mesh.has_route("node-b"));

  a.receive(make_data(wrong_fabric, NODE_B, NODE_A, NODE_A, "intruder"));
  EXPECT_EQ(a.received.size(), 0u);
}

// ── Slice 3: unicast TX carries the route's next hop ────────────────────────

static void test_unicast_send_sets_next_hop_from_routing_table() {
  TestNode a("node-a");
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}));

  EXPECT_TRUE(a.mesh.send_message("node-c", "water me"));
  a.mesh.loop();

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  EXPECT_EQ(pkt[4], PKT_DATA);
  EXPECT_FALSE(pkt[5] & FLAG_BROADCAST);
  EXPECT_EQ(get_u32_le(&pkt[6]), NODE_A);   // src
  EXPECT_EQ(get_u32_le(&pkt[10]), NODE_C);  // dst (end-to-end)
  EXPECT_EQ(get_u32_le(&pkt[20]), NODE_A);  // prev_hop = us
  EXPECT_EQ(get_u32_le(&pkt[24]), NODE_B);  // next_hop = B (toward C)
}

// ── Slice 4: delivery and (src_id, frame_counter) dedup ─────────────────────

static void test_unicast_delivered_to_destination() {
  TestNode c("node-c");

  c.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_C, "water me", 7, 6, 2, NODE_B));

  EXPECT_EQ(c.received.size(), 1u);
  EXPECT_TRUE(c.received[0].payload == "water me");
  EXPECT_EQ(c.received[0].frame_counter, 7u);
  EXPECT_EQ(c.received[0].hop_count, 2);
  EXPECT_TRUE(c.received[0].is_for_this_node);
  EXPECT_FALSE(c.received[0].is_broadcast);
  EXPECT_TRUE(std::string(c.received[0].prev_hop) != std::string(c.received[0].source));
}

static void test_duplicate_frame_counter_suppressed() {
  TestNode c("node-c");
  auto pkt = make_data(FABRIC, NODE_A, NODE_C, NODE_C, "water me", 7);

  c.receive(pkt);
  c.receive(pkt);  // same (src_id, frame_counter) → dropped
  EXPECT_EQ(c.received.size(), 1u);

  c.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_C, "water me", 8));
  EXPECT_EQ(c.received.size(), 2u);
}

// ── Slice 5: single-path unicast forwarding (ADR 0002) ──────────────────────
// 3-node line A—B—C: B is the designated next hop and forwards; another
// relay in range that also has a route but is not the next hop must not.

static void test_designated_next_hop_forwards_and_rewrites_header() {
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));  // B hears C directly
  b.radio.sent.clear();

  // A → C unicast, with B as the designated next hop.
  b.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_B, "water me", 7, 8, 0, NODE_A));
  b.mesh.loop();

  EXPECT_EQ(b.received.size(), 0u);  // not for B, no delivery
  EXPECT_EQ(b.radio.sent.size(), 1u);
  const auto &fwd = b.radio.sent[0];
  EXPECT_EQ(get_u32_le(&fwd[6]), NODE_A);   // src unchanged
  EXPECT_EQ(get_u32_le(&fwd[10]), NODE_C);  // dst unchanged
  EXPECT_EQ(get_u32_le(&fwd[14]), 7u);      // frame_counter unchanged
  EXPECT_EQ(fwd[18], 7);                    // ttl decremented
  EXPECT_EQ(fwd[19], 1);                    // hop_count incremented
  EXPECT_EQ(get_u32_le(&fwd[20]), NODE_B);  // prev_hop rewritten to us
  EXPECT_EQ(get_u32_le(&fwd[24]), NODE_C);  // next_hop = next hop toward C
}

static void test_non_next_hop_relay_does_not_forward_unicast() {
  TestNode d("node-d");
  d.receive(make_hello(FABRIC, NODE_C, "node-c"));  // D also has a route to C
  d.radio.sent.clear();

  // Same A → C unicast overheard by D, but B is the designated next hop.
  d.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_B, "water me", 7, 8, 0, NODE_A));
  d.mesh.loop();

  EXPECT_EQ(d.received.size(), 0u);
  EXPECT_EQ(d.radio.sent.size(), 0u);  // single-path: only B forwards
}

// ── Slice 6: broadcast still floods ─────────────────────────────────────────

static void test_relay_rebroadcasts_broadcast_and_delivers_it() {
  TestNode b("node-b");

  b.receive(make_data(FABRIC, NODE_A, BROADCAST, BROADCAST, "hello all", 3, 8, 0, NODE_A));
  b.mesh.loop();

  // Delivered locally...
  EXPECT_EQ(b.received.size(), 1u);
  EXPECT_TRUE(b.received[0].is_broadcast);
  EXPECT_FALSE(b.received[0].is_for_this_node);
  // ...and re-flooded with patched per-hop fields.
  EXPECT_EQ(b.radio.sent.size(), 1u);
  const auto &fwd = b.radio.sent[0];
  EXPECT_EQ(get_u32_le(&fwd[10]), BROADCAST);  // dst unchanged
  EXPECT_EQ(fwd[18], 7);                       // ttl decremented
  EXPECT_EQ(fwd[19], 1);                       // hop_count incremented
  EXPECT_EQ(get_u32_le(&fwd[20]), NODE_B);     // prev_hop rewritten
  EXPECT_EQ(get_u32_le(&fwd[24]), BROADCAST);  // next_hop stays "any"
}

// ── End-to-end: 3-node line A—B—C through real node instances ──────────────

static void test_three_node_line_unicast_and_broadcast() {
  TestNode a("node-a"), b("node-b"), c("node-c");

  // Discovery: B hears C, then A hears B's HELLO advertising the route to C.
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}));
  a.radio.sent.clear();
  b.radio.sent.clear();

  // Unicast A → C travels the single path through B.
  EXPECT_TRUE(a.mesh.send_message("node-c", "open valve"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  b.receive(a.radio.sent[0]);  // B is the designated next hop
  b.mesh.loop();
  EXPECT_EQ(b.radio.sent.size(), 1u);
  c.receive(b.radio.sent[0]);
  EXPECT_EQ(c.received.size(), 1u);
  EXPECT_TRUE(c.received[0].payload == "open valve");
  EXPECT_EQ(c.received[0].hop_count, 1);

  a.radio.sent.clear();
  b.radio.sent.clear();

  // Broadcast from A reaches both B and C via flooding.
  EXPECT_TRUE(a.mesh.broadcast_message("hello all"));
  a.mesh.loop();
  b.receive(a.radio.sent[0]);
  b.mesh.loop();
  EXPECT_EQ(b.received.size(), 1u);
  EXPECT_EQ(b.radio.sent.size(), 1u);  // B re-floods
  c.receive(b.radio.sent[0]);
  EXPECT_EQ(c.received.size(), 2u);
  EXPECT_TRUE(c.received[1].payload == "hello all");
  EXPECT_TRUE(c.received[1].is_broadcast);
}

// ── Slice 7: route maintenance — refresh fix + passive healing (issue #11) ──

static void test_multihop_route_lease_renewed_on_reconfirmation() {
  TestNode a("node-a");

  // C via B, learned at t0. Default route_ttl is 300 s.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 1));

  // At t0+250s B re-confirms the same route at equal quality (same next hop,
  // same hop count) — this must renew the lease, not just on improvement.
  advance_and_loop(a, 250000);
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 2));

  // t0+350s: past the original lease, within the renewed one.
  advance_and_loop(a, 100000);
  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_TRUE(a.mesh.has_route("node-c"));  // pre-fix: expired despite re-confirmation
}

static void test_dead_next_hop_invalidates_dependent_routes() {
  TestNode a("node-a");
  int route_updates = 0;
  a.mesh.add_on_route_update_callback([&route_updates]() { ++route_updates; });

  // Diverge the leases so C-via-B outlives B's direct route: learn C via B
  // under a long TTL, then shorten the TTL and renew only B's direct route —
  // a HELLO without advertisements refreshes B without refreshing C.
  a.mesh.set_route_ttl(600000);
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 1));
  EXPECT_EQ(a.mesh.get_known_node_count(), 2u);

  a.mesh.set_route_ttl(10000);
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, 0, 2));

  // B goes silent. Its direct route expires while C-via-B still holds a
  // ~10-minute lease; healing must drop C with it instead of black-holing.
  route_updates = 0;
  advance_and_loop(a, 20000);
  EXPECT_FALSE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_route("node-c"));  // pre-fix: stale route via dead B
  EXPECT_TRUE(route_updates >= 1);
  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);

  // An alternate path at merely *equal* quality is now adopted from D's next
  // HELLO (pre-fix it could not replace the stale route: not strictly better).
  a.receive(make_hello(FABRIC, NODE_D, "node-d", {{NODE_C, 1, -70}}, 0, 1));
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_message("node-c", "x"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][24]), NODE_D);  // recovered via D
}

static void test_better_path_still_replaces_worse_route() {
  TestNode a("node-a");

  // C via B at 3 hops, repeatedly re-confirmed (lease renewal active)...
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 2, -70}}, 0, 1));
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 2, -70}}, 0, 2));

  // ...must not mask D's strictly better 2-hop path to C.
  a.receive(make_hello(FABRIC, NODE_D, "node-d", {{NODE_C, 1, -70}}, 0, 1));

  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_message("node-c", "x"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][24]), NODE_D);  // next_hop moved to D
}

// ── Gateway flag refresh on re-confirmation ─────────────────────────────────
// A node first heard as a plain neighbour that later becomes a gateway must be
// recognised as one, even when its gateway HELLO arrives over the same next hop
// at no-better link quality (so the path-metric branch of update_route_ is
// skipped). Field bug: send_to_gateway() reported "no gateway in routing table"
// while the neighbour's HELLO clearly advertised gw=yes.

static void test_gateway_flag_refreshed_when_neighbor_becomes_gateway() {
  TestNode a("node-a");

  // B first heard as a non-gateway at strong RSSI → direct route, is_gateway=false.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, /*flags=*/0, /*frame=*/1), /*rssi=*/-60.0f);
  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_gateway());

  // B becomes a gateway and re-advertises — same next hop, same hop count, and
  // weaker RSSI so the path metric does not improve. The flag must still update.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, /*flags=*/FLAG_GATEWAY, /*frame=*/2), /*rssi=*/-95.0f);
  EXPECT_TRUE(a.mesh.has_gateway());  // pre-fix: stale is_gateway=false hid the gateway
  char gw_hex[9];
  snprintf(gw_hex, sizeof(gw_hex), "%08X", NODE_B);
  EXPECT_TRUE(a.mesh.get_best_gateway() == gw_hex);

  // And a node sending to the gateway now finds the route and queues a packet.
  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_to_gateway("status"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][24]), NODE_B);  // next_hop = gateway B
}

// ── Slice 8: bounded TX queue with randomized jitter (issue #12) ────────────

static void test_app_send_is_queued_not_transmitted_inline() {
  TestNode a("node-a");

  EXPECT_TRUE(a.mesh.broadcast_message("hi"));
  EXPECT_EQ(a.radio.sent.size(), 0u);  // queued, nothing on air yet

  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);  // drained on the next loop
  EXPECT_EQ(a.radio.sent[0][4], PKT_DATA);
}

static void test_at_most_one_transmit_per_loop() {
  TestNode a("node-a");

  EXPECT_TRUE(a.mesh.broadcast_message("one"));
  EXPECT_TRUE(a.mesh.broadcast_message("two"));
  EXPECT_TRUE(a.mesh.broadcast_message("three"));

  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 2u);
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 3u);

  // FIFO order preserved: check payload_len field (different lengths per message).
  EXPECT_EQ(a.radio.sent[0][HDR], 3u);  // "one" → 3 bytes
  EXPECT_EQ(a.radio.sent[1][HDR], 3u);  // "two" → 3 bytes
  EXPECT_EQ(a.radio.sent[2][HDR], 5u);  // "three" → 5 bytes
}

static void test_forwarding_is_queued_not_inline() {
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));
  b.radio.sent.clear();

  // Unicast forward: nothing may leave the radio from inside on_radio_packet.
  b.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_B, "x", 7, 8, 0, NODE_A));
  EXPECT_EQ(b.radio.sent.size(), 0u);
  b.mesh.loop();
  EXPECT_EQ(b.radio.sent.size(), 1u);

  // Broadcast re-flood is queued too.
  b.receive(make_data(FABRIC, NODE_A, BROADCAST, BROADCAST, "y", 8, 8, 0, NODE_A));
  EXPECT_EQ(b.radio.sent.size(), 1u);
  b.mesh.loop();
  EXPECT_EQ(b.radio.sent.size(), 2u);
}

static void test_randomized_backoff_delays_transmit() {
  TestNode a("node-a");
  // Default tx_jitter bound is 100 ms; rng=60 → backoff = 60 % 101 = 60 ms.
  esphome::test_random_set(60);

  EXPECT_TRUE(a.mesh.broadcast_message("hi"));
  a.mesh.loop();  // arms the backoff; deadline not reached yet
  EXPECT_EQ(a.radio.sent.size(), 0u);

  advance_and_loop(a, 59);  // 1 ms early
  EXPECT_EQ(a.radio.sent.size(), 0u);

  advance_and_loop(a, 1);  // deadline reached
  EXPECT_EQ(a.radio.sent.size(), 1u);
}

static void test_full_queue_drops_packet() {
  TestNode a("node-a");

  // Host tests build with the LORA_MESH_TX_QUEUE_SIZE fallback of 8.
  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(a.mesh.broadcast_message("p"));
  }
  EXPECT_FALSE(a.mesh.broadcast_message("overflow"));  // full → dropped

  // Drain completely: exactly the 8 accepted packets reach the air.
  for (int i = 0; i < 20; ++i) {
    a.mesh.loop();
  }
  EXPECT_EQ(a.radio.sent.size(), 8u);

  // Queue is usable again after draining.
  EXPECT_TRUE(a.mesh.broadcast_message("again"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 9u);
}

// ── Fabric Key DATA security / replay protection tests ──────────────────────

static void test_same_fabric_key_decrypts_payload() {
  // A and B share the same key; A sends to B, B's on_message fires with correct plaintext.
  TestNode a("node-a");
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_A, "node-a"));

  auto pkt = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "secret-msg", 10, 8, 0, NODE_A);
  b.receive(pkt);

  EXPECT_EQ(b.received.size(), 1u);
  EXPECT_TRUE(b.received[0].payload == "secret-msg");
}

static void test_wrong_key_drops_packet() {
  static const uint8_t WRONG_KEY[FABRIC_KEY_SIZE] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                                                     0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
  TestNode c("node-c");
  c.receive(make_hello(FABRIC, NODE_A, "node-a"));

  auto pkt = make_data(FABRIC, NODE_A, NODE_C, NODE_C, "for-c", 10, 8, 0, NODE_A, 0, WRONG_KEY);
  c.receive(pkt);

  EXPECT_EQ(c.received.size(), 0u);
}

static void test_modified_data_does_not_reach_application_callback() {
  TestNode b("node-b");
  auto pkt = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "authentic", 10, 8, 0, NODE_A);

  pkt[HDR + 1] ^= 0x01;
  b.receive(pkt);

  EXPECT_EQ(b.received.size(), 0u);
}

static void test_protocol_v3_four_byte_data_tag_is_rejected() {
  TestNode b("node-b");
  auto packet = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "legacy", 10, 8, 0, NODE_A);
  packet.resize(packet.size() - 4);

  b.receive(packet);

  EXPECT_EQ(b.received.size(), 0u);
}

static void test_replay_rejected() {
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_A, "node-a"));

  // First packet: frame_counter=10, delivers.
  auto pkt1 = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "msg1", 10, 8, 0, NODE_A);
  b.receive(pkt1);
  EXPECT_EQ(b.received.size(), 1u);

  // Replay with same frame_counter=10 — duplicate suppression in seen-cache catches this,
  // but even if seen-cache expired, replay protection would catch it.
  // Use a different frame_counter that's <= high_water but NOT in seen cache.
  // Frame_counter=5 (lower than 10) is a replay.
  auto pkt2 = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "replay", 5, 8, 0, NODE_A);
  b.receive(pkt2);
  EXPECT_EQ(b.received.size(), 1u);  // replay rejected, still only 1 message

  // Forward progression: frame_counter=11 should be accepted.
  auto pkt3 = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "msg2", 11, 8, 0, NODE_A);
  b.receive(pkt3);
  EXPECT_EQ(b.received.size(), 2u);
}

static void test_frame_counter_persists_across_reboot() {
  // Simulate: node A sends messages, then "reboots" (new TestNode with same identity).
  // The frame counter should resume from the NVS-persisted value, never reusing.
  {
    TestNode a("node-a");
    // After setup + loop, frame_counter is at 1 (HELLO).
    // Send 3 messages: frame_counters 2, 3, 4.
    a.mesh.broadcast_message("m1");
    a.mesh.broadcast_message("m2");
    a.mesh.broadcast_message("m3");
    a.mesh.loop();
    a.mesh.loop();
    a.mesh.loop();
  }
  // "Reboot" — new TestNode with same node_id should load persisted counter.
  {
    TestNode a2("node-a");
    // After setup, frame_counter should be >= persisted-ahead value (1000+).
    a2.mesh.broadcast_message("after-reboot");
    a2.mesh.loop();
    EXPECT_EQ(a2.radio.sent.size(), 1u);
    // The frame counter should be well above 4 (the last used value).
    uint32_t fc = get_u32_le(&a2.radio.sent[0][14]);
    EXPECT_TRUE(fc > 1000u);  // Persisted-ahead batch
  }
}

int main() {
  RUN_TEST(test_protocol_v4_derives_fabric_id_and_uses_eight_byte_data_tag);
  RUN_TEST(test_broadcast_data_has_v4_header);
  RUN_TEST(test_oversize_payload_truncated_consistently);
  RUN_TEST(test_own_hello_has_v4_body_at_offset_28);
  RUN_TEST(test_hello_builds_direct_and_advertised_routes);
  RUN_TEST(test_protocol_v3_hello_is_rejected_without_route_state);
  RUN_TEST(test_fabric_mismatch_drops_packet);
  RUN_TEST(test_unicast_send_sets_next_hop_from_routing_table);
  RUN_TEST(test_unicast_delivered_to_destination);
  RUN_TEST(test_duplicate_frame_counter_suppressed);
  RUN_TEST(test_designated_next_hop_forwards_and_rewrites_header);
  RUN_TEST(test_non_next_hop_relay_does_not_forward_unicast);
  RUN_TEST(test_relay_rebroadcasts_broadcast_and_delivers_it);
  RUN_TEST(test_three_node_line_unicast_and_broadcast);
  RUN_TEST(test_multihop_route_lease_renewed_on_reconfirmation);
  RUN_TEST(test_better_path_still_replaces_worse_route);
  RUN_TEST(test_dead_next_hop_invalidates_dependent_routes);
  RUN_TEST(test_gateway_flag_refreshed_when_neighbor_becomes_gateway);
  RUN_TEST(test_app_send_is_queued_not_transmitted_inline);
  RUN_TEST(test_at_most_one_transmit_per_loop);
  RUN_TEST(test_forwarding_is_queued_not_inline);
  RUN_TEST(test_randomized_backoff_delays_transmit);
  RUN_TEST(test_full_queue_drops_packet);
  // Fabric Key DATA security / replay tests
  RUN_TEST(test_same_fabric_key_decrypts_payload);
  RUN_TEST(test_wrong_key_drops_packet);
  RUN_TEST(test_modified_data_does_not_reach_application_callback);
  RUN_TEST(test_protocol_v3_four_byte_data_tag_is_rejected);
  RUN_TEST(test_replay_rejected);
  RUN_TEST(test_frame_counter_persists_across_reboot);
  printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
