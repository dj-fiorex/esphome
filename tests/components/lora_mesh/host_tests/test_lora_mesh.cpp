// Behavior tests for the lora_mesh proto-v3 wire format and single-path
// unicast forwarding (GitHub issue #10, docs/wire-format.md, ADR 0002).

#include "test_harness.h"

int g_failures = 0;

using namespace lmtest;

static const uint32_t FABRIC = fnv1a_str("fabric-1");
static const uint32_t NODE_A = fnv1a_str("node-a");
static const uint32_t NODE_B = fnv1a_str("node-b");
static const uint32_t NODE_C = fnv1a_str("node-c");
static const uint32_t NODE_D = fnv1a_str("node-d");

/// Advance the fake clock and run one loop() so periodic work (HELLO beacon,
/// route/seen-cache expiry) gets a chance to fire at the new time.
static void advance_and_loop(TestNode &n, uint32_t ms) {
  esphome::test_clock_advance(ms);
  n.mesh.loop();
}

// ── Slice 1: proto-v3 header on transmitted DATA ────────────────────────────

static void test_broadcast_data_has_v3_header() {
  TestNode a("node-a");

  EXPECT_TRUE(a.mesh.broadcast_message("hi"));
  a.mesh.loop();  // drain the TX queue

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  // 28-byte header + payload_len byte + 2 payload bytes
  EXPECT_EQ(pkt.size(), HDR + 1 + 2);
  EXPECT_EQ(get_u32_le(&pkt[0]), FABRIC);       // fabric_id
  EXPECT_EQ(pkt[4], PKT_DATA);                  // pkt_type
  EXPECT_TRUE(pkt[5] & FLAG_BROADCAST);         // flags
  EXPECT_EQ(get_u32_le(&pkt[6]), NODE_A);       // src_id
  EXPECT_EQ(get_u32_le(&pkt[10]), BROADCAST);   // dst_id
  EXPECT_EQ(get_u32_le(&pkt[14]), 2u);          // frame_counter (frame 1 = initial HELLO)
  EXPECT_EQ(pkt[18], 8);                        // ttl = default max_hops
  EXPECT_EQ(pkt[19], 0);                        // hop_count
  EXPECT_EQ(get_u32_le(&pkt[20]), NODE_A);      // prev_hop = originator
  EXPECT_EQ(get_u32_le(&pkt[24]), BROADCAST);   // next_hop = any (flood)
  EXPECT_EQ(pkt[HDR], 2);                       // payload_len
  EXPECT_EQ(pkt[HDR + 1], 'h');
  EXPECT_EQ(pkt[HDR + 2], 'i');

  static_assert(esphome::lora_mesh::MESH_PROTO_VERSION == 3, "proto version must be 3");
  static_assert(esphome::lora_mesh::MESH_HEADER_SIZE == 28, "header must be 28 bytes");
}

static void test_oversize_payload_truncated_consistently() {
  TestNode a("node-a");
  // 250 bytes does not fit: 255 radio limit - 28 header - 1 len byte = 226 max.
  std::string big(250, 'x');

  EXPECT_TRUE(a.mesh.broadcast_message(big));
  a.mesh.loop();

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  EXPECT_TRUE(pkt.size() <= 255u);
  EXPECT_EQ(pkt[HDR], pkt.size() - HDR - 1);  // payload_len matches bytes on wire
}

// ── Slice 2: HELLO at proto v3 ──────────────────────────────────────────────

static void test_own_hello_has_v3_body_at_offset_28() {
  TestNode a("node-a");

  // The initial HELLO is consumed by the fixture; trigger the next beacon.
  advance_and_loop(a, 30000);

  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &pkt = a.radio.sent[0];
  EXPECT_EQ(pkt[4], PKT_HELLO);
  EXPECT_EQ(get_u32_le(&pkt[10]), BROADCAST);  // dst
  EXPECT_EQ(get_u32_le(&pkt[24]), BROADCAST);  // next_hop
  EXPECT_TRUE(pkt.size() >= HDR + 3);
  EXPECT_EQ(pkt[HDR], 3);                                   // proto_version
  EXPECT_EQ(pkt[HDR + 1], 6);                               // name_len
  EXPECT_TRUE(std::string(pkt.begin() + HDR + 2, pkt.begin() + HDR + 8) == "node-a");
  EXPECT_EQ(pkt[HDR + 8], 0);                               // route_count (no routes yet)
}

static void test_hello_builds_direct_and_advertised_routes() {
  TestNode a("node-a");

  // HELLO from B advertising a 1-hop route to C.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}));

  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  EXPECT_FALSE(a.mesh.has_route("node-x"));
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
  // a HELLO from a different proto version proves B is alive (direct route
  // refreshes) but its body, including the route digest, is skipped.
  a.mesh.set_route_ttl(600000);
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 1));
  EXPECT_EQ(a.mesh.get_known_node_count(), 2u);

  a.mesh.set_route_ttl(10000);
  auto alien_hello = make_hello(FABRIC, NODE_B, "node-b", {}, 0, 2);
  alien_hello[HDR] = 2;  // proto_version mismatch
  a.receive(alien_hello);

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

  // FIFO order preserved.
  EXPECT_EQ(a.radio.sent[0][HDR + 1], 'o');
  EXPECT_EQ(a.radio.sent[1][HDR + 1], 't');
  EXPECT_EQ(a.radio.sent[2][HDR + 1], 't');
  EXPECT_EQ(a.radio.sent[2][HDR + 3], 'r');
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

int main() {
  RUN_TEST(test_broadcast_data_has_v3_header);
  RUN_TEST(test_oversize_payload_truncated_consistently);
  RUN_TEST(test_own_hello_has_v3_body_at_offset_28);
  RUN_TEST(test_hello_builds_direct_and_advertised_routes);
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
  RUN_TEST(test_app_send_is_queued_not_transmitted_inline);
  RUN_TEST(test_at_most_one_transmit_per_loop);
  RUN_TEST(test_forwarding_is_queued_not_inline);
  RUN_TEST(test_randomized_backoff_delays_transmit);
  RUN_TEST(test_full_queue_drops_packet);
  printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
