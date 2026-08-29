// Behavior tests for the lora_mesh wire format and single-path unicast forwarding.

#include "test_harness.h"

#include <limits>
#include <type_traits>

int g_failures = 0;

using namespace lmtest;

static_assert(!std::is_constructible_v<LoraMesh, const std::string &>);
static_assert(std::is_constructible_v<LoraMesh, const std::string &, esphome::lora_mesh::LoRaRadio *>);

static const uint32_t FABRIC = 0xDEFF3662;
static const uint32_t NODE_A = fnv1a_str("node-a");
static const uint32_t NODE_B = fnv1a_str("node-b");
static const uint32_t NODE_C = fnv1a_str("node-c");
static const uint32_t NODE_D = fnv1a_str("node-d");
static const uint32_t NODE_E = fnv1a_str("node-e");

// AES-128-CCM(TEST_FABRIC_KEY, nonce="LORA-FABRICID", empty plaintext,
// AAD="LORA-MESH-ID-v1", tag=8) starts with 62 36 ff de. The public Fabric ID
// uses those first four tag bytes as little-endian.

/// Advance the fake clock and run one loop() so periodic work (HELLO beacon,
/// route/seen-cache expiry) gets a chance to fire at the new time.
static void advance_and_loop(TestNode &n, uint32_t ms) {
  esphome::test_clock_advance(ms);
  n.mesh.loop();
}

static void expect_nearest_gateway(TestNode &node, uint32_t gateway_id) {
  char gateway_hex[9];
  snprintf(gateway_hex, sizeof(gateway_hex), "%08X", gateway_id);
  EXPECT_TRUE(node.mesh.get_nearest_gateway() == gateway_hex);
}

static uint8_t hello_route_count(const std::vector<uint8_t> &hello) {
  size_t route_count_offset = HDR + 2 + hello[HDR + 1];
  return hello[route_count_offset];
}

static RouteAdv hello_route_at(const std::vector<uint8_t> &hello, size_t index) {
  size_t route_count_offset = HDR + 2 + hello[HDR + 1];
  size_t route_offset = route_count_offset + 1 + index * esphome::lora_mesh::ROUTE_ADV_SIZE;
  return {
      get_u32_le(&hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_DEST_ID]),
      hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_HOP_COUNT],
      static_cast<int8_t>(hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_PATH_RSSI]),
      (hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_FLAGS] & esphome::lora_mesh::ROUTE_FLAG_IS_GATEWAY) != 0,
  };
}

static bool hello_advertises_gateway_state(const std::vector<uint8_t> &hello, uint32_t destination_id,
                                           bool is_gateway) {
  for (size_t index = 0; index < hello_route_count(hello); ++index) {
    RouteAdv route = hello_route_at(hello, index);
    if (route.dest_id == destination_id && route.is_gateway == is_gateway) {
      return true;
    }
  }
  return false;
}

static bool hello_advertises_destination(const std::vector<uint8_t> &hello, uint32_t destination_id) {
  for (size_t index = 0; index < hello_route_count(hello); ++index) {
    if (hello_route_at(hello, index).dest_id == destination_id) {
      return true;
    }
  }
  return false;
}

static bool route_uses_next_hop(const TestNode &node, uint32_t destination_id, uint32_t next_hop_id) {
  char destination[9];
  char next_hop[9];
  snprintf(destination, sizeof(destination), "%08X", destination_id);
  snprintf(next_hop, sizeof(next_hop), "%08X", next_hop_id);
  std::string routes = node.mesh.get_routing_table_json();
  size_t destination_pos = routes.find(std::string("\"dst\":\"") + destination + "\"");
  return destination_pos != std::string::npos &&
         routes.find(std::string("\"nh\":\"") + next_hop + "\"", destination_pos) != std::string::npos;
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

static void test_packet_header_named_fields_have_exact_v4_wire_order() {
  const esphome::lora_mesh::PacketHeader header{
      .fabric_id = 0x04030201,
      .packet_type = esphome::lora_mesh::PacketType::ACK,
      .flags = 0x06,
      .src_id = 0x0A090807,
      .dst_id = 0x0E0D0C0B,
      .frame_counter = 0x1211100F,
      .ttl = 0x13,
      .hop_count = 0x14,
      .prev_hop = 0x18171615,
      .next_hop = 0x1C1B1A19,
  };
  const std::array<uint8_t, HDR> expected{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E,
      0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
  };

  const auto wire_header = esphome::lora_mesh::serialize_packet_header(header);
  EXPECT_TRUE(std::equal(expected.begin(), expected.end(), wire_header.begin(), wire_header.end()));

  const auto parsed = esphome::lora_mesh::parse_packet_header(expected.data());
  EXPECT_EQ(parsed.fabric_id, header.fabric_id);
  EXPECT_EQ(static_cast<uint8_t>(parsed.packet_type), static_cast<uint8_t>(header.packet_type));
  EXPECT_EQ(parsed.flags, header.flags);
  EXPECT_EQ(parsed.src_id, header.src_id);
  EXPECT_EQ(parsed.dst_id, header.dst_id);
  EXPECT_EQ(parsed.frame_counter, header.frame_counter);
  EXPECT_EQ(parsed.ttl, header.ttl);
  EXPECT_EQ(parsed.hop_count, header.hop_count);
  EXPECT_EQ(parsed.prev_hop, header.prev_hop);
  EXPECT_EQ(parsed.next_hop, header.next_hop);
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
  bool ok = esphome::lora_mesh::mesh_decrypt_payload(TEST_FABRIC_KEY, NODE_A, BROADCAST, 2, PKT_DATA, FLAG_BROADCAST, 2,
                                                     &pkt[HDR + 1], plaintext, &pkt[HDR + 1 + 2]);
  EXPECT_TRUE(ok);
  EXPECT_EQ(plaintext[0], 'h');
  EXPECT_EQ(plaintext[1], 'i');

  static_assert(esphome::lora_mesh::MESH_PROTO_VERSION == 4, "proto version must be 4");
  static_assert(esphome::lora_mesh::MESH_HEADER_SIZE == 28, "header must be 28 bytes");
}

static void test_data_marks_origin_upstream_state() {
  TestNode gateway("node-a");
  gateway.mesh.set_upstream_connected(true);

  EXPECT_TRUE(gateway.mesh.broadcast_message("hi"));
  gateway.mesh.loop();

  EXPECT_EQ(gateway.radio.sent.size(), 1u);
  EXPECT_TRUE(gateway.radio.sent[0][5] & FLAG_GATEWAY);
  EXPECT_TRUE(gateway.radio.sent[0][5] & FLAG_BROADCAST);
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

static void test_binary_span_send_does_not_allocate_and_round_trips() {
  TestNode a("node-a");
  const std::array<uint8_t, 5> payload{{0x00, 0x41, 0xFF, 0x42, 0x00}};

  esphome::test_allocations_begin();
  bool accepted = a.mesh.broadcast_message(std::span<const uint8_t>(payload));
  size_t allocation_count = esphome::test_allocations_end();
  EXPECT_TRUE(accepted);
  EXPECT_EQ(allocation_count, 0u);

  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  const auto &packet = a.radio.sent[0];
  EXPECT_EQ(packet[HDR], payload.size());
  uint8_t plaintext[esphome::lora_mesh::MESH_MAX_DATA_PAYLOAD_SIZE]{};
  bool decrypted = esphome::lora_mesh::mesh_decrypt_payload(TEST_FABRIC_KEY, NODE_A, BROADCAST, get_u32_le(&packet[14]),
                                                            PKT_DATA, packet[5], packet[HDR], &packet[HDR + 1],
                                                            plaintext, &packet[HDR + 1 + packet[HDR]]);
  EXPECT_TRUE(decrypted);
  EXPECT_TRUE(memcmp(plaintext, payload.data(), payload.size()) == 0);
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
  EXPECT_TRUE(pkt.size() >= HDR + 3 + HELLO_TAG_SIZE);
  EXPECT_EQ(pkt[HDR], 4);      // proto_version
  EXPECT_EQ(pkt[HDR + 1], 6);  // name_len
  EXPECT_TRUE(std::string(pkt.begin() + HDR + 2, pkt.begin() + HDR + 8) == "node-a");
  EXPECT_EQ(pkt[HDR + 8], 0);  // route_count (no routes yet)

  std::vector<uint8_t> authenticated_bytes(pkt.begin(), pkt.end() - HELLO_TAG_SIZE);
  auto expected_tag = hello_auth_tag(authenticated_bytes);
  EXPECT_TRUE(std::equal(expected_tag.begin(), expected_tag.end(), pkt.end() - HELLO_TAG_SIZE));
  static const uint8_t EXPECTED_TAG[HELLO_TAG_SIZE] = {0xec, 0x1f, 0xe0, 0x89, 0x25, 0x55, 0xeb, 0xe6};
  EXPECT_TRUE(std::equal(EXPECTED_TAG, EXPECTED_TAG + HELLO_TAG_SIZE, pkt.end() - HELLO_TAG_SIZE));
}

static void test_hello_builds_direct_and_advertised_routes() {
  TestNode a("node-a");

  // HELLO from B advertising a 1-hop route to C.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}));

  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  EXPECT_FALSE(a.mesh.has_route("node-x"));
}

static void test_maximum_valid_route_is_advertised_without_poisoning_hello() {
  TestNode advertising_node("node-a");
  advertising_node.mesh.set_max_hops(UINT8_MAX);

  // A valid 253-hop advertisement extends to the maximum wire-valid Route
  // length. A 254-hop advertisement would extend to 255 and must not enter
  // the routing table even when a caller bypasses the Python schema.
  advertising_node.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 253, -70}, {NODE_E, 254, -80}}, 0, 50));
  advance_and_loop(advertising_node, 30000);

  EXPECT_EQ(advertising_node.radio.sent.size(), 1u);
  bool advertised_maximum_route = false;
  bool advertised_invalid_route = false;
  for (size_t index = 0; index < hello_route_count(advertising_node.radio.sent[0]); ++index) {
    RouteAdv route = hello_route_at(advertising_node.radio.sent[0], index);
    advertised_maximum_route |= route.dest_id == NODE_C && route.hop_count == 254;
    advertised_invalid_route |= route.hop_count == UINT8_MAX;
  }
  EXPECT_TRUE(advertised_maximum_route);
  EXPECT_FALSE(advertised_invalid_route);

  // The maximum valid advertisement must not make a receiving Node reject the entire
  // authenticated HELLO as malformed.
  TestNode receiving_node("node-d");
  receiving_node.receive(advertising_node.radio.sent[0]);
  EXPECT_TRUE(receiving_node.mesh.has_route("node-a"));
}

static void test_runtime_max_hops_clamps_to_minimum_advertisable_edge() {
  TestNode advertising_node("node-a");
  advertising_node.mesh.set_max_hops(0);
  advance_and_loop(advertising_node, 30000);

  EXPECT_EQ(advertising_node.radio.sent.size(), 1u);
  EXPECT_EQ(advertising_node.radio.sent[0][18], 1u);

  TestNode receiving_node("node-d");
  receiving_node.receive(advertising_node.radio.sent[0]);
  EXPECT_TRUE(receiving_node.mesh.has_route("node-a"));
}

static void test_protocol_v3_hello_is_rejected_without_route_state() {
  TestNode a("node-a");
  auto packet = make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}});
  packet.resize(packet.size() - HELLO_TAG_SIZE);
  packet[HDR] = 3;
  auto tag = hello_auth_tag(packet);
  packet.insert(packet.end(), tag.begin(), tag.end());

  a.receive(packet);

  EXPECT_FALSE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_route("node-c"));
}

static void test_forged_hello_does_not_poison_seen_cache_or_discovery_state() {
  TestNode a("node-a");
  int route_updates = 0;
  a.mesh.add_on_route_update_callback([&route_updates]() { ++route_updates; });
  auto forged = make_hello(FABRIC, NODE_B, "forged-name", {}, FLAG_GATEWAY, 41);
  forged.back() ^= 0x01;

  a.receive(forged);

  EXPECT_FALSE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_gateway());
  EXPECT_TRUE(a.mesh.get_node_name(NODE_B) == nullptr);
  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);
  EXPECT_EQ(route_updates, 0);

  // The authentic packet with the same source/counter must still be accepted,
  // proving the forged packet did not enter the Seen-cache.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 41));
  EXPECT_TRUE(a.mesh.has_route("node-b"));
  EXPECT_TRUE(a.mesh.has_gateway());
  EXPECT_TRUE(std::string(a.mesh.get_node_name(NODE_B)) == "node-b");
}

static void test_wrong_key_hello_cannot_create_gateway_or_route() {
  static const uint8_t WRONG_KEY[FABRIC_KEY_SIZE] = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA, 0xF9, 0xF8,
                                                     0xF7, 0xF6, 0xF5, 0xF4, 0xF3, 0xF2, 0xF1, 0xF0};
  TestNode a("node-a");

  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, FLAG_GATEWAY, 42, WRONG_KEY));

  EXPECT_FALSE(a.mesh.has_route("node-b"));
  EXPECT_FALSE(a.mesh.has_route("node-c"));
  EXPECT_FALSE(a.mesh.has_gateway());
  EXPECT_TRUE(a.mesh.get_node_name(NODE_B) == nullptr);
}

static void test_changed_authenticated_hello_byte_cannot_change_state() {
  TestNode a("node-a");
  auto changed_header = make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 43);
  changed_header[18] ^= 0x01;  // TTL is visible routing metadata, but authenticated.
  a.receive(changed_header);

  auto changed_body = make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 44);
  changed_body[HDR + 2] ^= 0x01;  // First Node-name byte.
  a.receive(changed_body);

  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);
  EXPECT_FALSE(a.mesh.has_gateway());
  EXPECT_TRUE(a.mesh.get_node_name(NODE_B) == nullptr);
  EXPECT_TRUE(a.mesh.get_node_name(NODE_C) == nullptr);
}

static void test_changed_hello_packet_type_cannot_poison_seen_cache() {
  TestNode a("node-a");

  auto changed_to_data = make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 45);
  changed_to_data[4] = PKT_DATA;
  a.receive(changed_to_data);
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 45));
  EXPECT_TRUE(a.mesh.has_route("node-b"));

  auto changed_to_reserved = make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 46);
  changed_to_reserved[4] = 6;  // Reserved ERROR type.
  a.receive(changed_to_reserved);
  a.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 46));
  EXPECT_TRUE(a.mesh.has_route("node-c"));
}

static void test_malformed_hello_cannot_change_state() {
  TestNode a("node-a");
  auto truncated = make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 45);
  truncated.pop_back();
  a.receive(truncated);

  auto trailing = make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 46);
  trailing.push_back(0x00);
  a.receive(trailing);

  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);
  EXPECT_FALSE(a.mesh.has_gateway());
}

static void test_authenticated_hello_with_invalid_header_cannot_change_state() {
  TestNode a("node-a");

  auto wrong_destination = make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 47);
  put_u32_le(&wrong_destination[10], NODE_A);
  resign_hello(wrong_destination);
  a.receive(wrong_destination);

  auto forwarded = make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 48);
  forwarded[19] = 1;
  resign_hello(forwarded);
  a.receive(forwarded);

  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);
  EXPECT_FALSE(a.mesh.has_gateway());
}

static void test_authenticated_hello_with_invalid_route_cannot_change_state() {
  TestNode a("node-a");
  auto invalid_route = make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 255, -70, true}}, FLAG_GATEWAY, 49);

  a.receive(invalid_route);

  EXPECT_EQ(a.mesh.get_known_node_count(), 0u);
  EXPECT_FALSE(a.mesh.has_gateway());
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

static void test_mac_derived_default_node_id_is_stable_and_addressable() {
  std::string reported_node_id;

  {
    FakeRadio target_radio;
    esphome::lora_mesh::LoraMesh target(TEST_FABRIC_KEY_HEX, &target_radio);
    bool delivered = false;
    target.add_on_message_callback([&delivered](const esphome::lora_mesh::MeshMessage &message) {
      delivered = message.payload.size() == 8 && memcmp(message.payload.data(), "water me", 8) == 0;
    });
    target.setup();  // Deliberately omit set_node_id().
    target.loop();

    reported_node_id = target.get_node_id();
    EXPECT_TRUE(reported_node_id == "ABCDEF");
    EXPECT_EQ(target_radio.sent.size(), 1u);
    if (!target_radio.sent.empty()) {
      EXPECT_EQ(get_u32_le(&target_radio.sent[0][6]), fnv1a_str(reported_node_id));
    }

    TestNode sender("node-a");
    if (!target_radio.sent.empty()) {
      sender.receive(target_radio.sent[0]);
      EXPECT_TRUE(sender.mesh.send_message(reported_node_id, "water me"));
      sender.mesh.loop();
      EXPECT_EQ(sender.radio.sent.size(), 1u);
      if (!sender.radio.sent.empty()) {
        target.on_radio_packet(sender.radio.sent[0].data(), sender.radio.sent[0].size(), -60.0f, 8.0f);
      }
    }

    EXPECT_TRUE(delivered);
  }

  FakeRadio rebooted_radio;
  esphome::lora_mesh::LoraMesh rebooted(TEST_FABRIC_KEY_HEX, &rebooted_radio);
  rebooted.setup();  // Same fake MAC, still no configured node_id.
  EXPECT_TRUE(rebooted.get_node_id() == reported_node_id);
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

static void test_final_destination_accepts_directly_overheard_unicast() {
  TestNode c("node-c");

  // C can hear A directly even though A selected B as the Next Hop toward C.
  c.receive(make_data(FABRIC, NODE_A, NODE_C, NODE_B, "water me", 7, 8, 0, NODE_A));

  EXPECT_EQ(c.received.size(), 1u);
  EXPECT_TRUE(c.received[0].payload == "water me");
  EXPECT_TRUE(c.received[0].is_for_this_node);
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
  const auto original = make_data(FABRIC, NODE_A, NODE_C, NODE_B, "water me", 7, 8, 0, NODE_A);
  b.receive(original);
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
  EXPECT_TRUE(std::equal(original.begin(), original.begin() + esphome::lora_mesh::MESH_OFF_TTL, fwd.begin()));
  EXPECT_TRUE(std::equal(original.begin() + HDR, original.end(), fwd.begin() + HDR));
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

static void test_overheard_non_designated_unicast_does_not_suppress_later_designated_hop() {
  TestNode d("node-d");
  d.receive(make_hello(FABRIC, NODE_C, "node-c"));  // D reaches final destination C directly
  d.radio.sent.clear();

  // D overhears A's earlier transmission to designated Next Hop B.
  auto packet = make_data(FABRIC, NODE_A, NODE_C, NODE_B, "water me", 7, 8, 0, NODE_A);
  d.receive(packet);
  d.mesh.loop();
  EXPECT_EQ(d.radio.sent.size(), 0u);

  // B legitimately forwards the same authenticated DATA to D. Only mutable
  // link fields change, so this retains the same (source, frame counter).
  packet[18] = 7;
  packet[19] = 1;
  put_u32_le(&packet[20], NODE_B);
  put_u32_le(&packet[24], NODE_D);
  d.receive(packet);
  d.mesh.loop();

  EXPECT_EQ(d.received.size(), 0u);
  EXPECT_EQ(d.radio.sent.size(), 1u);
  if (!d.radio.sent.empty()) {
    EXPECT_EQ(get_u32_le(&d.radio.sent[0][20]), NODE_D);
    EXPECT_EQ(get_u32_le(&d.radio.sent[0][24]), NODE_C);
  }
}

static void test_forwarder_does_not_return_unicast_to_previous_hop() {
  TestNode b("node-b");

  // This is the transient cyclic state from A—B—C loss: B has accepted an
  // indirect Route to C from A, while A sends DATA expecting B to reach C.
  b.receive(make_hello(FABRIC, NODE_A, "node-a", {{NODE_C, 2, -70}}));
  b.radio.sent.clear();
  b.receive(make_data(FABRIC, NODE_D, NODE_C, NODE_B, "must not bounce", 7, 8, 1, NODE_A));
  b.mesh.loop();

  EXPECT_EQ(b.received.size(), 0u);
  EXPECT_EQ(b.radio.sent.size(), 0u);
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
  TestNode receiving_node("node-a");
  receiving_node.mesh.set_route_ttl(300000);
  int route_updates = 0;
  receiving_node.mesh.add_on_route_update_callback([&route_updates]() { ++route_updates; });

  // C via B, learned at t0 under a 300-second test lease.
  receiving_node.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 1));
  route_updates = 0;

  // At t0+250s B re-confirms the same route at equal quality (same next hop,
  // same hop count) — this must renew the lease, not just on improvement.
  advance_and_loop(receiving_node, 250000);
  receiving_node.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70}}, 0, 2));
  EXPECT_EQ(route_updates, 0);

  // t0+350s: past the original lease, within the renewed one.
  advance_and_loop(receiving_node, 100000);
  EXPECT_TRUE(receiving_node.mesh.has_route("node-b"));
  EXPECT_TRUE(receiving_node.mesh.has_route("node-c"));  // pre-fix: expired despite re-confirmation
}

static void test_direct_gateway_route_degradation_reselects_stronger_equal_hop_gateway() {
  TestNode a("node-a");
  int route_updates = 0;
  a.mesh.add_on_route_update_callback([&route_updates]() { ++route_updates; });

  // B initially wins as the stronger direct Gateway Route.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 1), -60.0f, 7.0f);
  a.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 1), -80.0f, 5.0f);
  expect_nearest_gateway(a, NODE_B);
  route_updates = 0;

  // Reconfirmation from the current Next Hop must replace its historical
  // high-water metric when the direct link degrades.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 2), -90.0f, -4.0f);
  expect_nearest_gateway(a, NODE_C);
  EXPECT_EQ(route_updates, 1);

  // An SNR-only change is observable once, proving the latest SNR replaces
  // the stored value while an identical confirmation only renews the lease.
  route_updates = 0;
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 3), -90.0f, -5.0f);
  EXPECT_EQ(route_updates, 1);
  route_updates = 0;
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 4), -90.0f, -5.0f);
  EXPECT_EQ(route_updates, 0);
}

static void test_multihop_route_reconfirmation_updates_degraded_path_rssi() {
  TestNode a("node-a");

  // C is reached through B. The first Path RSSI is limited by C's advertised
  // -60 dBm path because A's direct link to B is stronger.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -60, true}}, 0, 1), -50.0f, 6.0f);
  expect_nearest_gateway(a, NODE_C);
  advance_and_loop(a, 1000);  // consume C's prompt Gateway availability HELLO
  a.radio.sent.clear();

  // B later re-confirms the same Route after its path to C has degraded. A
  // must store the new weakest-link metric rather than retain -60 dBm.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -90, true}}, 0, 2), -50.0f, -3.0f);

  // The next periodic HELLO must propagate C's degraded Path RSSI.
  advance_and_loop(a, 30000);
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(a.radio.sent[0][4], PKT_HELLO);
  bool found_degraded_route = false;
  for (size_t index = 0; index < hello_route_count(a.radio.sent[0]); ++index) {
    RouteAdv route = hello_route_at(a.radio.sent[0], index);
    if (route.dest_id == NODE_C) {
      found_degraded_route = true;
      EXPECT_EQ(route.hop_count, 2u);
      EXPECT_EQ(route.rssi, -90);
      EXPECT_TRUE(route.is_gateway);
    }
  }
  EXPECT_TRUE(found_degraded_route);

  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_to_gateway("status"));
  a.mesh.loop();
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][esphome::lora_mesh::MESH_OFF_DST_ID]), NODE_C);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][esphome::lora_mesh::MESH_OFF_NEXT_HOP]), NODE_B);
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

static void test_distributed_neighbor_loss_converges_without_a_count_to_infinity_loop() {
  TestNode a("node-a"), b("node-b"), c("node-c"), d("node-d");
  std::array<TestNode *, 4> nodes{{&a, &b, &c, &d}};
  TestTopology<4> topology(nodes);
  topology.connect(0, 1);  // A—B
  topology.connect(1, 2);  // B—C

  for (TestNode *node : nodes) {
    node->mesh.set_discovery_interval(10000);
    node->mesh.set_route_ttl(30000);
    node->mesh.set_tx_jitter(0);
  }

  // Normal HELLO discovery reaches both ends of A—B—C.
  for (int round = 0; round < 3; ++round) {
    topology.advance(10000);
  }
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  EXPECT_TRUE(c.mesh.has_route("node-a"));
  EXPECT_TRUE(route_uses_next_hop(a, NODE_C, NODE_B));

  // C disappears from B. Advance HELLO and lease time far enough for both B's
  // direct Route and A's dependent Route to expire. At no round may A and B
  // install each other as Next Hop for C (the count-to-infinity failure).
  topology.disconnect(1, 2);
  for (int round = 0; round < 7; ++round) {
    topology.advance(10000);
    EXPECT_FALSE(route_uses_next_hop(a, NODE_C, NODE_B) && route_uses_next_hop(b, NODE_C, NODE_A));
  }
  EXPECT_FALSE(a.mesh.has_route("node-c"));
  EXPECT_FALSE(b.mesh.has_route("node-c"));
  EXPECT_FALSE(a.mesh.send_message("node-c", "must-not-loop"));

  // A newly available B—D—C path is learned after the bounded suppression
  // interval, and DATA crosses the healed single path exactly once per hop.
  topology.connect(1, 3);  // B—D
  topology.connect(3, 2);  // D—C
  for (int round = 0; round < 4; ++round) {
    topology.advance(10000);
  }
  EXPECT_TRUE(a.mesh.has_route("node-c"));
  EXPECT_TRUE(route_uses_next_hop(a, NODE_C, NODE_B));
  EXPECT_TRUE(route_uses_next_hop(b, NODE_C, NODE_D));

  EXPECT_TRUE(a.mesh.send_message("node-c", "healed"));
  size_t data_transmissions = 0;
  for (int round = 0; round < 4 && c.received.empty(); ++round) {
    data_transmissions += topology.run_round();
  }
  EXPECT_EQ(c.received.size(), 1u);
  EXPECT_TRUE(c.received[0].payload == "healed");
  EXPECT_EQ(data_transmissions, 3u);  // A, B, and D; no cyclic bounce.
}

static void test_delayed_queued_hello_is_refreshed_after_route_expiry() {
  TestNode b("node-b");
  b.mesh.set_discovery_interval(10000);
  b.mesh.set_route_ttl(30000);
  b.mesh.set_tx_jitter(50000);
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));

  // Queue a HELLO advertising C, but hold it behind a deterministic 40-second
  // TX backoff so C expires before the packet reaches the radio.
  esphome::test_random_set(40000);
  advance_and_loop(b, 10000);
  EXPECT_EQ(b.radio.sent.size(), 0u);

  advance_and_loop(b, 30000);
  EXPECT_FALSE(b.mesh.has_route("node-c"));
  EXPECT_EQ(b.radio.sent.size(), 0u);

  // The queued HELLO is rebuilt at transmission and must withdraw C instead
  // of granting a fresh stale lease to a neighbour.
  advance_and_loop(b, 10000);
  EXPECT_EQ(b.radio.sent.size(), 1u);
  EXPECT_EQ(b.radio.sent[0][esphome::lora_mesh::MESH_OFF_PKT_TYPE], PKT_HELLO);
  EXPECT_FALSE(hello_advertises_destination(b.radio.sent[0], NODE_C));
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
  EXPECT_TRUE(a.mesh.get_nearest_gateway() == gw_hex);

  // And a node sending to the gateway now finds the route and queues a packet.
  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_to_gateway("status"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][24]), NODE_B);  // next_hop = gateway B
}

static void test_upstream_connectivity_starts_false_and_only_real_transitions_schedule_hello() {
  TestNode a("node-a");

  EXPECT_FALSE(a.mesh.is_upstream_connected());

  // Repeating the boot state is a no-op.
  a.mesh.set_upstream_connected(false);
  advance_and_loop(a, 1000);
  EXPECT_EQ(a.radio.sent.size(), 0u);

  // A real transition is advertised promptly through HELLO.
  a.mesh.set_upstream_connected(true);
  advance_and_loop(a, 1000);
  EXPECT_TRUE(a.mesh.is_upstream_connected());
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(a.radio.sent[0][4], PKT_HELLO);
  EXPECT_TRUE(a.radio.sent[0][5] & FLAG_GATEWAY);

  // Repeating the current state must not create redundant transition traffic.
  a.radio.sent.clear();
  a.mesh.set_upstream_connected(true);
  advance_and_loop(a, 1000);
  EXPECT_EQ(a.radio.sent.size(), 0u);

  a.mesh.set_upstream_connected(false);
  advance_and_loop(a, 1000);
  EXPECT_FALSE(a.mesh.is_upstream_connected());
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_FALSE(a.radio.sent[0][5] & FLAG_GATEWAY);
}

static void test_route_advertisement_propagates_gateway_and_weakest_path_rssi() {
  TestNode a("node-a");

  // B advertises a Gateway C path measured at -70 dBm; A's B link is weaker
  // at -90 dBm, so the propagated Path RSSI must be the weakest link (-90).
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70, true}}, 0, 50), -90.0f);

  EXPECT_TRUE(a.mesh.has_gateway());
  char gateway_hex[9];
  snprintf(gateway_hex, sizeof(gateway_hex), "%08X", NODE_C);
  EXPECT_TRUE(a.mesh.get_nearest_gateway() == gateway_hex);
  std::string routes = a.mesh.get_routing_table_json();
  EXPECT_TRUE(routes.find("\"gw\":true") != std::string::npos);
  EXPECT_TRUE(routes.find("\"rssi\":-90") != std::string::npos);

  a.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_to_gateway("status"));
  a.mesh.loop();
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][10]), NODE_C);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][24]), NODE_B);
}

static void test_transmitted_route_advertisement_is_seven_bytes_with_gateway_flag() {
  TestNode b("node-b");

  b.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 62), -70.0f);
  advance_and_loop(b, 1000);

  EXPECT_EQ(b.radio.sent.size(), 1u);
  const auto &hello = b.radio.sent[0];
  size_t route_count_offset = HDR + 2 + hello[HDR + 1];
  EXPECT_EQ(hello[route_count_offset], 1u);
  EXPECT_EQ(hello.size(), route_count_offset + 1 + esphome::lora_mesh::ROUTE_ADV_SIZE + HELLO_TAG_SIZE);
  size_t route_offset = route_count_offset + 1;
  EXPECT_EQ(get_u32_le(&hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_DEST_ID]), NODE_C);
  EXPECT_EQ(hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_HOP_COUNT], 1u);
  EXPECT_EQ(static_cast<int8_t>(hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_PATH_RSSI]), -70);
  EXPECT_EQ(hello[route_offset + esphome::lora_mesh::ROUTE_ADV_OFF_FLAGS], esphome::lora_mesh::ROUTE_FLAG_IS_GATEWAY);
}

static void test_nearest_gateway_selection_is_deterministic() {
  TestNode a("node-a");

  // Fewer hops beats stronger Path RSSI.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 2, -40, true}}, 0, 51), -40.0f);
  a.receive(make_hello(FABRIC, NODE_D, "node-d", {}, FLAG_GATEWAY, 52), -100.0f);
  char node_d_hex[9];
  snprintf(node_d_hex, sizeof(node_d_hex), "%08X", NODE_D);
  EXPECT_TRUE(a.mesh.get_nearest_gateway() == node_d_hex);

  // At equal hops, stronger weakest-link Path RSSI wins.
  a.mesh.clear_routes();
  a.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 53), -80.0f);
  a.receive(make_hello(FABRIC, NODE_D, "node-d", {}, FLAG_GATEWAY, 54), -90.0f);
  char node_c_hex[9];
  snprintf(node_c_hex, sizeof(node_c_hex), "%08X", NODE_C);
  EXPECT_TRUE(a.mesh.get_nearest_gateway() == node_c_hex);

  // Exact metric ties use the lowest unsigned Node ID, regardless of learning order.
  TestNode first("node-a"), second("node-a");
  first.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 55), -70.0f);
  first.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 56), -70.0f);
  second.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 56), -70.0f);
  second.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 55), -70.0f);
  uint32_t lowest_id = std::min(NODE_B, NODE_C);
  char lowest_hex[9];
  snprintf(lowest_hex, sizeof(lowest_hex), "%08X", lowest_id);
  EXPECT_TRUE(first.mesh.get_nearest_gateway() == lowest_hex);
  EXPECT_TRUE(second.mesh.get_nearest_gateway() == lowest_hex);
}

static void test_nearest_gateway_uses_strongest_equal_hop_path() {
  TestNode a("node-a");

  // A first learns Gateway C through B with a weak Path RSSI, then learns the
  // same two-hop Gateway through D with a stronger Path RSSI.
  a.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -60, true}}, 0, 57), -90.0f);
  a.receive(make_hello(FABRIC, NODE_D, "node-d", {{NODE_C, 1, -70, true}}, 0, 58), -70.0f);

  EXPECT_TRUE(a.mesh.send_to_gateway("status"));
  a.mesh.loop();
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][esphome::lora_mesh::MESH_OFF_DST_ID]), NODE_C);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][esphome::lora_mesh::MESH_OFF_NEXT_HOP]), NODE_D);
}

static void test_send_to_gateway_reselects_current_nearest_gateway_on_every_call() {
  TestNode a("node-a");

  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 59), -80.0f);
  EXPECT_TRUE(a.mesh.send_to_gateway("first"));
  a.mesh.loop();
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][esphome::lora_mesh::MESH_OFF_DST_ID]), NODE_B);

  a.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 60), -60.0f);
  EXPECT_TRUE(a.mesh.send_to_gateway("second"));
  a.mesh.loop();
  EXPECT_EQ(get_u32_le(&a.radio.sent[1][esphome::lora_mesh::MESH_OFF_DST_ID]), NODE_C);
}

static void test_node_with_upstream_connectivity_has_no_local_send_to_gateway_delivery() {
  TestNode a("node-a");

  a.mesh.set_upstream_connected(true);

  EXPECT_FALSE(a.mesh.send_to_gateway("local"));
  EXPECT_EQ(a.received.size(), 0u);
}

static void test_rejected_gateway_send_is_not_retried_after_queue_drains() {
  TestNode a("node-a");

  a.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 61));
  advance_and_loop(a, 1000);  // consume the Route-change HELLO
  a.radio.sent.clear();

  for (int i = 0; i < 8; ++i) {
    EXPECT_TRUE(a.mesh.broadcast_message("queued"));
  }
  EXPECT_FALSE(a.mesh.send_to_gateway("must-not-be-retained"));

  for (int i = 0; i < 20; ++i) {
    a.mesh.loop();
  }
  EXPECT_EQ(a.radio.sent.size(), 8u);
  for (const auto &packet : a.radio.sent) {
    EXPECT_EQ(get_u32_le(&packet[esphome::lora_mesh::MESH_OFF_DST_ID]), BROADCAST);
  }
}

static void test_three_node_gateway_discovery_and_delivery() {
  TestNode a("node-a"), b("node-b"), c("node-c");

  c.mesh.set_upstream_connected(true);
  advance_and_loop(c, 1000);
  EXPECT_EQ(c.radio.sent.size(), 1u);
  b.receive(c.radio.sent[0], -70.0f);

  // B promptly propagates C's Gateway availability in its next coalesced HELLO.
  advance_and_loop(b, 1000);
  EXPECT_EQ(b.radio.sent.size(), 1u);
  a.receive(b.radio.sent[0], -80.0f);
  EXPECT_TRUE(a.mesh.has_gateway());
  a.mesh.loop();  // consume A's own propagated HELLO before application DATA

  a.radio.sent.clear();
  b.radio.sent.clear();
  EXPECT_TRUE(a.mesh.send_to_gateway("soil=17"));
  a.mesh.loop();
  b.receive(a.radio.sent[0]);
  b.mesh.loop();
  c.receive(b.radio.sent[0]);
  EXPECT_EQ(c.received.size(), 1u);
  EXPECT_TRUE(c.received[0].payload == "soil=17");
}

// ── Gateway propagation and passive expiry (issue #22) ─────────────────

static void test_gateway_promotion_and_withdrawal_propagate_promptly_and_select_alternative() {
  TestNode offline_node("node-a"), forwarding_node("node-b"), primary_gateway("node-c"), alternative_gateway("node-d");

  primary_gateway.mesh.set_upstream_connected(true);
  advance_and_loop(primary_gateway, 1000);
  alternative_gateway.mesh.set_upstream_connected(true);
  advance_and_loop(alternative_gateway, 1000);

  // B hears both Gateways directly. C has the stronger path and is selected.
  forwarding_node.receive(primary_gateway.radio.sent[0], -50.0f);
  forwarding_node.receive(alternative_gateway.radio.sent[0], -90.0f);
  advance_and_loop(forwarding_node, 1000);

  // A cannot hear either Gateway directly; B's prompt HELLO makes both
  // selectable across the second LoRa hop without a periodic HELLO wait.
  offline_node.receive(forwarding_node.radio.sent[0], -60.0f);
  expect_nearest_gateway(offline_node, NODE_C);
  offline_node.mesh.loop();  // propagate availability onward before the Withdrawal wave

  primary_gateway.radio.sent.clear();
  forwarding_node.radio.sent.clear();
  offline_node.radio.sent.clear();

  // C gracefully withdraws. B and then A each emit one rate-limited HELLO,
  // clear C's stale eligibility, and keep D as the Nearest Gateway.
  primary_gateway.mesh.set_upstream_connected(false);
  primary_gateway.mesh.loop();
  EXPECT_EQ(primary_gateway.radio.sent.size(), 1u);
  forwarding_node.receive(primary_gateway.radio.sent[0], -50.0f);
  advance_and_loop(forwarding_node, 1000);
  EXPECT_EQ(forwarding_node.radio.sent.size(), 1u);
  offline_node.receive(forwarding_node.radio.sent[0], -60.0f);
  offline_node.mesh.loop();
  EXPECT_EQ(offline_node.radio.sent.size(), 1u);

  expect_nearest_gateway(offline_node, NODE_D);
}

static void test_gateway_withdrawal_from_worse_path_clears_stale_gateway_eligibility() {
  TestNode offline_node("node-a");

  // A learned C directly while D is its weaker alternative Gateway.
  offline_node.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 70), -50.0f);
  offline_node.receive(make_hello(FABRIC, NODE_D, "node-d", {}, FLAG_GATEWAY, 80), -90.0f);
  expect_nearest_gateway(offline_node, NODE_C);
  advance_and_loop(offline_node, 1000);
  offline_node.radio.sent.clear();

  // A misses C's direct Withdrawal but hears it propagated by B. Gateway
  // eligibility is destination state, so the worse two-hop path must still
  // clear C's stale status without replacing C's better Route through C.
  offline_node.receive(make_hello(FABRIC, NODE_B, "node-b", {{NODE_C, 1, -70, false}}, 0, 90), -60.0f);
  expect_nearest_gateway(offline_node, NODE_D);

  advance_and_loop(offline_node, 1000);
  EXPECT_EQ(offline_node.radio.sent.size(), 1u);
  if (offline_node.radio.sent.empty()) {
    return;
  }
  EXPECT_TRUE(hello_advertises_gateway_state(offline_node.radio.sent[0], NODE_C, false));
}

static void test_gateway_change_is_included_when_hello_cannot_fit_every_route() {
  TestNode forwarding_node("node-b");
  forwarding_node.radio.max_packet_size = 52;  // node-b HELLO overhead plus exactly one Route

  // The close non-Gateway route would normally win the hop-count ordering.
  // The farther Gateway change must instead occupy the sole Route entry in the HELLO.
  forwarding_node.receive(make_hello(FABRIC, NODE_C, "node-c", {{NODE_D, 2, -80, true}}, 0, 90), -60.0f);
  advance_and_loop(forwarding_node, 1000);

  const auto &availability_hello = forwarding_node.radio.sent[0];
  EXPECT_EQ(hello_route_count(availability_hello), 1u);
  EXPECT_TRUE(hello_advertises_gateway_state(availability_hello, NODE_D, true));

  // The same bounded priority is required for the Gateway Withdrawal.
  forwarding_node.radio.sent.clear();
  forwarding_node.receive(make_hello(FABRIC, NODE_C, "node-c", {{NODE_D, 2, -80, false}}, 0, 91), -60.0f);
  advance_and_loop(forwarding_node, 1000);

  const auto &withdrawal_hello = forwarding_node.radio.sent[0];
  EXPECT_EQ(hello_route_count(withdrawal_hello), 1u);
  EXPECT_TRUE(hello_advertises_gateway_state(withdrawal_hello, NODE_D, false));
}

static void test_gateway_changes_over_hello_capacity_continue_in_bounded_updates() {
  TestNode forwarding_node("node-b");
  forwarding_node.radio.max_packet_size = 52;  // node-b HELLO overhead plus exactly one Route

  forwarding_node.receive(
      make_hello(FABRIC, NODE_C, "node-c", {{NODE_D, 1, -70, true}, {NODE_A, 2, -80, true}}, 0, 94));
  advance_and_loop(forwarding_node, 1000);

  const auto &first_hello = forwarding_node.radio.sent[0];
  EXPECT_EQ(hello_route_count(first_hello), 1u);
  EXPECT_TRUE(hello_advertises_gateway_state(first_hello, NODE_D, true));

  // The unadvertised change remains pending for one later rate-limited HELLO.
  forwarding_node.radio.sent.clear();
  advance_and_loop(forwarding_node, 1000);
  const auto &second_hello = forwarding_node.radio.sent[0];
  EXPECT_EQ(hello_route_count(second_hello), 1u);
  EXPECT_TRUE(hello_advertises_gateway_state(second_hello, NODE_A, true));

  // Once every stable change has been advertised, no control burst continues.
  forwarding_node.radio.sent.clear();
  advance_and_loop(forwarding_node, 1000);
  EXPECT_EQ(forwarding_node.radio.sent.size(), 0u);
}

static void test_pending_gateway_withdrawal_survives_route_table_pressure() {
  TestNode forwarding_node("node-a");
  constexpr uint32_t withdrawing_gateway_id = 0x0000100F;
  std::vector<RouteAdv> advertised_routes;
  for (uint32_t destination_id = 0x00001000; destination_id < 0x0000100E; ++destination_id) {
    advertised_routes.push_back({destination_id, 1, -60, false});
  }
  advertised_routes.push_back({withdrawing_gateway_id, 7, -120, true});

  // B plus its fifteen advertisements fill the default sixteen-Route table.
  forwarding_node.receive(make_hello(FABRIC, NODE_B, "node-b", advertised_routes, 0, 95));
  advance_and_loop(forwarding_node, 1000);
  EXPECT_TRUE(hello_advertises_gateway_state(forwarding_node.radio.sent[0], withdrawing_gateway_id, true));

  // Learn the Withdrawal, then pressure the full table with a new direct Node
  // before the rate-limited HELLO can be built. The pending tombstone must not
  // be the Route evicted to make room.
  forwarding_node.radio.sent.clear();
  advertised_routes.back().is_gateway = false;
  forwarding_node.receive(make_hello(FABRIC, NODE_B, "node-b", advertised_routes, 0, 96));
  forwarding_node.receive(make_hello(FABRIC, NODE_C, "node-c", {}, 0, 97));
  advance_and_loop(forwarding_node, 1000);

  EXPECT_EQ(forwarding_node.radio.sent.size(), 1u);
  EXPECT_TRUE(hello_advertises_gateway_state(forwarding_node.radio.sent[0], withdrawing_gateway_id, false));
}

static void test_new_gateway_in_reused_route_slot_still_schedules_hello() {
  TestNode forwarding_node("node-a");

  forwarding_node.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY, 92));
  advance_and_loop(forwarding_node, 1000);
  EXPECT_EQ(forwarding_node.radio.sent.size(), 1u);

  // Reusing B's invalid slot for another Gateway must reset B's stale flags;
  // otherwise C appears unchanged and no prompt availability HELLO is sent.
  forwarding_node.mesh.clear_routes();
  forwarding_node.radio.sent.clear();
  forwarding_node.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 93));
  advance_and_loop(forwarding_node, 1000);

  EXPECT_EQ(forwarding_node.radio.sent.size(), 1u);
  const auto &availability_hello = forwarding_node.radio.sent[0];
  EXPECT_TRUE(hello_advertises_gateway_state(availability_hello, NODE_C, true));
}

static void test_gateway_transition_hello_updates_are_coalesced() {
  TestNode a("node-a");

  for (int i = 0; i < 20; i++) {
    a.mesh.set_upstream_connected((i % 2) == 0);
  }
  advance_and_loop(a, 1000);

  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_FALSE(a.radio.sent[0][5] & FLAG_GATEWAY);  // final state wins
}

static void test_gateway_hello_rate_limit_preserves_final_stable_state() {
  TestNode gateway_node("node-a");

  gateway_node.mesh.set_upstream_connected(true);
  advance_and_loop(gateway_node, 1000);
  EXPECT_EQ(gateway_node.radio.sent.size(), 1u);
  EXPECT_TRUE(gateway_node.radio.sent[0][5] & FLAG_GATEWAY);
  gateway_node.radio.sent.clear();

  // Flap repeatedly inside the one-second immediate-HELLO window, ending
  // disconnected. No intermediate state may escape before the deadline.
  for (int i = 0; i < 20; ++i) {
    gateway_node.mesh.set_upstream_connected((i % 2) == 0);
  }
  gateway_node.mesh.loop();
  advance_and_loop(gateway_node, 999);
  EXPECT_EQ(gateway_node.radio.sent.size(), 0u);

  // The pending request survives coalescing and advertises the final stable
  // Gateway Withdrawal as soon as the rate-limit window closes.
  advance_and_loop(gateway_node, 1);
  EXPECT_EQ(gateway_node.radio.sent.size(), 1u);
  EXPECT_FALSE(gateway_node.radio.sent[0][5] & FLAG_GATEWAY);
}

static void test_silent_gateway_expires_after_three_missed_hellos_and_next_send_uses_alternative() {
  TestNode offline_node("node-a");

  // C starts as the Nearest Gateway. D is a weaker direct alternative that
  // continues sending HELLOs while C disappears without a Withdrawal.
  offline_node.receive(make_hello(FABRIC, NODE_C, "node-c", {}, FLAG_GATEWAY, 70), -50.0f);
  offline_node.receive(make_hello(FABRIC, NODE_D, "node-d", {}, FLAG_GATEWAY, 80), -90.0f);
  expect_nearest_gateway(offline_node, NODE_C);

  // The production defaults are a 30-second HELLO interval and a Route lease
  // of three missed HELLOs. Refresh only D during that failure-detection window.
  for (uint32_t missed = 1; missed <= 3; ++missed) {
    advance_and_loop(offline_node, 30000);
    offline_node.receive(make_hello(FABRIC, NODE_D, "node-d", {}, FLAG_GATEWAY, 80 + missed), -90.0f);
  }

  expect_nearest_gateway(offline_node, NODE_D);

  offline_node.radio.sent.clear();
  EXPECT_TRUE(offline_node.mesh.send_to_gateway("after-expiry"));
  offline_node.mesh.loop();
  EXPECT_EQ(offline_node.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&offline_node.radio.sent[0][esphome::lora_mesh::MESH_OFF_DST_ID]), NODE_D);
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

static void test_successful_radio_outcome_removes_packet_from_queue() {
  TestNode a("node-a");
  a.radio.attempted.clear();

  EXPECT_TRUE(a.mesh.broadcast_message("success"));
  a.mesh.loop();
  a.mesh.loop();

  EXPECT_EQ(a.radio.attempted.size(), 1u);
  EXPECT_EQ(a.radio.sent.size(), 1u);
}

static void test_radio_timeout_drops_failed_packet_and_continues_queue_without_retry() {
  TestNode a("node-a");
  a.radio.attempted.clear();
  a.radio.transmit_outcome = TransmissionOutcome::TIMEOUT;
  esphome::test_log_clear();

  EXPECT_TRUE(a.mesh.broadcast_message("first"));
  EXPECT_TRUE(a.mesh.broadcast_message("second"));
  a.mesh.loop();

  EXPECT_EQ(a.radio.attempted.size(), 1u);
  EXPECT_EQ(a.radio.sent.size(), 0u);
  EXPECT_TRUE(esphome::test_log_contains("Radio transmission timed out; packet dropped"));

  a.radio.transmit_outcome = TransmissionOutcome::SUCCESS;
  a.mesh.loop();
  a.mesh.loop();

  EXPECT_EQ(a.radio.attempted.size(), 2u);
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(a.radio.sent[0][HDR], 6u);  // The second packet was next; the failed first packet was not retried.
}

static void test_invalid_radio_parameters_drop_packet_without_retry() {
  TestNode a("node-a");
  a.radio.attempted.clear();
  a.radio.transmit_outcome = TransmissionOutcome::INVALID_PARAMETER;
  esphome::test_log_clear();

  EXPECT_TRUE(a.mesh.broadcast_message("invalid"));
  a.mesh.loop();

  EXPECT_EQ(a.radio.attempted.size(), 1u);
  EXPECT_EQ(a.radio.sent.size(), 0u);
  EXPECT_TRUE(esphome::test_log_contains("Radio rejected transmission parameters; packet dropped"));

  a.radio.transmit_outcome = TransmissionOutcome::SUCCESS;
  a.mesh.loop();

  EXPECT_EQ(a.radio.attempted.size(), 1u);
  EXPECT_EQ(a.radio.sent.size(), 0u);
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

static void test_receive_dispatch_does_not_allocate() {
  FakeRadio radio;
  LoraMesh mesh(TEST_FABRIC_KEY_HEX, &radio);
  mesh.set_node_id(esphome::TemplatableValue<std::string>("node-b"));

  std::array<uint8_t, esphome::lora_mesh::MESH_MAX_DATA_PAYLOAD_SIZE> delivered{};
  size_t delivered_size = 0;
  mesh.add_on_message_callback([&](const esphome::lora_mesh::MeshMessage &message) {
    delivered_size = message.payload.size();
    memcpy(delivered.data(), message.payload.data(), delivered_size);
  });
  mesh.setup();
  mesh.loop();

  const std::string payload(64, 'x');
  auto packet = make_data(FABRIC, NODE_A, NODE_B, NODE_B, payload, 10, 8, 0, NODE_A);

  esphome::test_allocations_begin();
  mesh.on_radio_packet(packet.data(), packet.size(), -60.0f, 8.0f);
  size_t allocation_count = esphome::test_allocations_end();

  EXPECT_EQ(allocation_count, 0u);
  EXPECT_EQ(delivered_size, payload.size());
  EXPECT_TRUE(memcmp(delivered.data(), payload.data(), payload.size()) == 0);
}

static void test_message_copy_owns_payload_for_deferred_automation() {
  FakeRadio radio;
  LoraMesh mesh(TEST_FABRIC_KEY_HEX, &radio);
  mesh.set_node_id(esphome::TemplatableValue<std::string>("node-b"));

  esphome::lora_mesh::MeshMessage retained;
  const uint8_t *callback_payload_data = nullptr;
  mesh.add_on_message_callback([&](const esphome::lora_mesh::MeshMessage &message) {
    callback_payload_data = message.payload.data();
    retained = message;
  });
  mesh.setup();
  mesh.loop();

  const std::string payload(64, 'x');
  auto packet = make_data(FABRIC, NODE_A, NODE_B, NODE_B, payload, 10, 8, 0, NODE_A);
  mesh.on_radio_packet(packet.data(), packet.size(), -60.0f, 8.0f);

  EXPECT_TRUE(retained.payload.data() != callback_payload_data);
  EXPECT_EQ(retained.payload.size(), payload.size());
  EXPECT_TRUE(memcmp(retained.payload.data(), payload.data(), payload.size()) == 0);
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

static void test_modified_broadcast_data_is_not_delivered_or_forwarded() {
  TestNode b("node-b");
  auto pkt = make_data(FABRIC, NODE_A, BROADCAST, BROADCAST, "authentic", 10, 8, 0, NODE_A);

  pkt[HDR + 1] ^= 0x01;
  b.receive(pkt);
  b.mesh.loop();

  EXPECT_EQ(b.received.size(), 0u);
  EXPECT_EQ(b.radio.sent.size(), 0u);
}

static void test_modified_transit_unicast_data_is_not_forwarded() {
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));
  b.radio.sent.clear();
  auto pkt = make_data(FABRIC, NODE_A, NODE_C, NODE_B, "authentic", 10, 8, 0, NODE_A);

  pkt[HDR + 1] ^= 0x01;
  b.receive(pkt);
  b.mesh.loop();

  EXPECT_EQ(b.radio.sent.size(), 0u);
}

static void test_modified_data_cannot_poison_seen_cache() {
  TestNode b("node-b");
  auto authentic = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "authentic", 10, 8, 0, NODE_A);
  auto modified = authentic;
  modified[HDR + 1] ^= 0x01;

  b.receive(modified);
  b.receive(authentic);

  EXPECT_EQ(b.received.size(), 1u);
  if (!b.received.empty()) {
    EXPECT_TRUE(b.received[0].payload == "authentic");
  }
}

static void test_modified_data_flags_do_not_reach_application_callback() {
  TestNode b("node-b");
  auto pkt = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "authentic", 10, 8, 0, NODE_A);

  pkt[5] ^= FLAG_GATEWAY;
  b.receive(pkt);

  EXPECT_EQ(b.received.size(), 0u);
}

static void test_data_with_trailing_bytes_does_not_reach_application_callback() {
  TestNode b("node-b");
  auto pkt = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "authentic", 10, 8, 0, NODE_A);

  pkt.push_back(0x00);
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

static void test_protocol_v3_four_byte_data_tag_is_not_forwarded() {
  TestNode b("node-b");
  b.receive(make_hello(FABRIC, NODE_C, "node-c"));
  auto packet = make_data(FABRIC, NODE_A, NODE_C, NODE_B, "legacy", 10, 8, 0, NODE_A);
  packet.resize(packet.size() - 4);

  b.receive(packet);
  b.mesh.loop();

  EXPECT_EQ(b.radio.sent.size(), 0u);
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

static void test_replay_table_pressure_fails_closed_without_forgetting_known_sources() {
  TestNode b("node-b");
  b.mesh.set_seen_cache_ttl(100);
  constexpr uint32_t first_source = 0xA0000000;

  auto first_packet = make_data(FABRIC, first_source, NODE_B, NODE_B, "original", 100, 8, 0, first_source);
  b.receive(first_packet);
  for (size_t i = 1; i < LORA_MESH_MAX_ROUTES; ++i) {
    uint32_t source = first_source + static_cast<uint32_t>(i);
    b.receive(make_data(FABRIC, source, NODE_B, NODE_B, "fill", 100, 8, 0, source));
  }
  EXPECT_EQ(b.received.size(), static_cast<size_t>(LORA_MESH_MAX_ROUTES));

  // Once every fixed replay slot belongs to a known source, a new source must
  // fail closed instead of evicting a high-water mark.
  uint32_t unknown_source = first_source + LORA_MESH_MAX_ROUTES;
  b.receive(make_data(FABRIC, unknown_source, NODE_B, NODE_B, "unknown", 100, 8, 0, unknown_source));
  EXPECT_EQ(b.received.size(), static_cast<size_t>(LORA_MESH_MAX_ROUTES));

  // Table pressure must not prevent a retained source from advancing.
  uint32_t retained_source = first_source + LORA_MESH_MAX_ROUTES - 1;
  b.receive(make_data(FABRIC, retained_source, NODE_B, NODE_B, "advance", 101, 8, 0, retained_source));
  EXPECT_EQ(b.received.size(), static_cast<size_t>(LORA_MESH_MAX_ROUTES + 1));

  // Exercise replay protection rather than duplicate suppression: expire the
  // Seen-cache, then resend the original authenticated DATA frame.
  esphome::test_clock_advance(10000);
  b.mesh.loop();
  b.receive(first_packet);
  EXPECT_EQ(b.received.size(), static_cast<size_t>(LORA_MESH_MAX_ROUTES + 1));
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

static void test_power_loss_after_data_send_does_not_reuse_frame_counter() {
  uint32_t first_counter = 0;
  {
    TestNode before_power_loss("node-a");
    EXPECT_TRUE(before_power_loss.mesh.broadcast_message("before-power-loss"));
    before_power_loss.mesh.loop();
    EXPECT_EQ(before_power_loss.radio.sent.size(), 1u);
    first_counter = get_u32_le(&before_power_loss.radio.sent[0][14]);
  }

  esphome::test_preferences_simulate_power_loss();

  TestNode after_power_loss("node-a");
  EXPECT_TRUE(after_power_loss.mesh.broadcast_message("after-power-loss"));
  after_power_loss.mesh.loop();
  EXPECT_EQ(after_power_loss.radio.sent.size(), 1u);
  EXPECT_TRUE(get_u32_le(&after_power_loss.radio.sent[0][14]) > first_counter);
}

static void test_setup_save_failure_permanently_refuses_origination() {
  esphome::test_preferences_set_save_succeeds(false);
  TestNode failed_setup("node-a");

  EXPECT_FALSE(failed_setup.mesh.broadcast_message("while-save-fails"));
  esphome::test_preferences_set_save_succeeds(true);
  EXPECT_FALSE(failed_setup.mesh.broadcast_message("after-save-recovers"));
  failed_setup.mesh.loop();
  EXPECT_EQ(failed_setup.radio.sent.size(), 0u);
}

static void test_setup_sync_failure_permanently_refuses_origination() {
  esphome::test_preferences_set_sync_succeeds(false);
  TestNode failed_setup("node-a");

  EXPECT_FALSE(failed_setup.mesh.broadcast_message("while-sync-fails"));
  esphome::test_preferences_set_sync_succeeds(true);
  EXPECT_FALSE(failed_setup.mesh.broadcast_message("after-sync-recovers"));
  failed_setup.mesh.loop();
  EXPECT_EQ(failed_setup.radio.sent.size(), 0u);
}

static void test_runtime_rollover_refuses_data_until_reservation_is_durable() {
  constexpr uint32_t max_counter = std::numeric_limits<uint32_t>::max();
  const uint32_t preference_key = fnv1a_str("lora_mesh_fc_node-a");
  esphome::test_preferences_put_u32(preference_key, max_counter - 2);
  TestNode a("node-a");

  esphome::test_preferences_set_sync_succeeds(false);
  EXPECT_FALSE(a.mesh.broadcast_message("sync-failed"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 0u);

  esphome::test_preferences_set_sync_succeeds(true);
  EXPECT_TRUE(a.mesh.broadcast_message("sync-recovered"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][14]), max_counter);
  EXPECT_FALSE(a.mesh.broadcast_message("exhausted"));
}

static void test_frame_counter_reservation_remains_batched() {
  const uint32_t preference_key = fnv1a_str("lora_mesh_fc_node-a");
  TestNode a("node-a");
  EXPECT_EQ(esphome::test_preferences_get_u32(preference_key), 1000u);

  EXPECT_TRUE(a.mesh.broadcast_message("one"));
  EXPECT_TRUE(a.mesh.broadcast_message("two"));
  EXPECT_TRUE(a.mesh.broadcast_message("three"));
  EXPECT_EQ(esphome::test_preferences_get_u32(preference_key), 1000u);
}

static void test_final_frame_counter_is_reserved_and_used_only_once() {
  constexpr uint32_t max_counter = std::numeric_limits<uint32_t>::max();
  const uint32_t preference_key = fnv1a_str("lora_mesh_fc_node-a");
  esphome::test_preferences_put_u32(preference_key, max_counter - 2);

  TestNode a("node-a");
  EXPECT_EQ(esphome::test_preferences_get_u32(preference_key), max_counter);

  EXPECT_TRUE(a.mesh.broadcast_message("final"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
  EXPECT_EQ(get_u32_le(&a.radio.sent[0][14]), max_counter);

  EXPECT_FALSE(a.mesh.broadcast_message("exhausted"));
  a.mesh.loop();
  EXPECT_EQ(a.radio.sent.size(), 1u);
}

static void test_reboot_at_frame_counter_exhaustion_refuses_all_data_sends() {
  constexpr uint32_t max_counter = std::numeric_limits<uint32_t>::max();
  const uint32_t preference_key = fnv1a_str("lora_mesh_fc_node-a");
  esphome::test_preferences_put_u32(preference_key, max_counter);

  TestNode rebooted("node-a");
  rebooted.receive(make_hello(FABRIC, NODE_B, "node-b", {}, FLAG_GATEWAY));
  esphome::test_log_clear();

  EXPECT_FALSE(rebooted.mesh.broadcast_message("broadcast"));
  EXPECT_FALSE(rebooted.mesh.send_message("node-b", "unicast"));
  EXPECT_FALSE(rebooted.mesh.send_to_gateway("gateway"));
  EXPECT_TRUE(esphome::test_log_contains("Frame counter exhausted"));
  EXPECT_TRUE(esphome::test_log_contains("prevent nonce reuse"));
  EXPECT_EQ(esphome::test_preferences_get_u32(preference_key), max_counter);

  rebooted.mesh.loop();
  EXPECT_EQ(rebooted.radio.sent.size(), 0u);
}

int main() {
  RUN_TEST(test_protocol_v4_derives_fabric_id_and_uses_eight_byte_data_tag);
  RUN_TEST(test_packet_header_named_fields_have_exact_v4_wire_order);
  RUN_TEST(test_broadcast_data_has_v4_header);
  RUN_TEST(test_data_marks_origin_upstream_state);
  RUN_TEST(test_oversize_payload_truncated_consistently);
  RUN_TEST(test_binary_span_send_does_not_allocate_and_round_trips);
  RUN_TEST(test_own_hello_has_v4_body_at_offset_28);
  RUN_TEST(test_hello_builds_direct_and_advertised_routes);
  RUN_TEST(test_maximum_valid_route_is_advertised_without_poisoning_hello);
  RUN_TEST(test_runtime_max_hops_clamps_to_minimum_advertisable_edge);
  RUN_TEST(test_protocol_v3_hello_is_rejected_without_route_state);
  RUN_TEST(test_forged_hello_does_not_poison_seen_cache_or_discovery_state);
  RUN_TEST(test_wrong_key_hello_cannot_create_gateway_or_route);
  RUN_TEST(test_changed_authenticated_hello_byte_cannot_change_state);
  RUN_TEST(test_changed_hello_packet_type_cannot_poison_seen_cache);
  RUN_TEST(test_malformed_hello_cannot_change_state);
  RUN_TEST(test_authenticated_hello_with_invalid_header_cannot_change_state);
  RUN_TEST(test_authenticated_hello_with_invalid_route_cannot_change_state);
  RUN_TEST(test_fabric_mismatch_drops_packet);
  RUN_TEST(test_unicast_send_sets_next_hop_from_routing_table);
  RUN_TEST(test_mac_derived_default_node_id_is_stable_and_addressable);
  RUN_TEST(test_unicast_delivered_to_destination);
  RUN_TEST(test_final_destination_accepts_directly_overheard_unicast);
  RUN_TEST(test_duplicate_frame_counter_suppressed);
  RUN_TEST(test_designated_next_hop_forwards_and_rewrites_header);
  RUN_TEST(test_non_next_hop_relay_does_not_forward_unicast);
  RUN_TEST(test_overheard_non_designated_unicast_does_not_suppress_later_designated_hop);
  RUN_TEST(test_forwarder_does_not_return_unicast_to_previous_hop);
  RUN_TEST(test_relay_rebroadcasts_broadcast_and_delivers_it);
  RUN_TEST(test_three_node_line_unicast_and_broadcast);
  RUN_TEST(test_multihop_route_lease_renewed_on_reconfirmation);
  RUN_TEST(test_direct_gateway_route_degradation_reselects_stronger_equal_hop_gateway);
  RUN_TEST(test_multihop_route_reconfirmation_updates_degraded_path_rssi);
  RUN_TEST(test_better_path_still_replaces_worse_route);
  RUN_TEST(test_dead_next_hop_invalidates_dependent_routes);
  RUN_TEST(test_distributed_neighbor_loss_converges_without_a_count_to_infinity_loop);
  RUN_TEST(test_delayed_queued_hello_is_refreshed_after_route_expiry);
  RUN_TEST(test_gateway_flag_refreshed_when_neighbor_becomes_gateway);
  RUN_TEST(test_upstream_connectivity_starts_false_and_only_real_transitions_schedule_hello);
  RUN_TEST(test_route_advertisement_propagates_gateway_and_weakest_path_rssi);
  RUN_TEST(test_transmitted_route_advertisement_is_seven_bytes_with_gateway_flag);
  RUN_TEST(test_nearest_gateway_selection_is_deterministic);
  RUN_TEST(test_nearest_gateway_uses_strongest_equal_hop_path);
  RUN_TEST(test_send_to_gateway_reselects_current_nearest_gateway_on_every_call);
  RUN_TEST(test_node_with_upstream_connectivity_has_no_local_send_to_gateway_delivery);
  RUN_TEST(test_rejected_gateway_send_is_not_retried_after_queue_drains);
  RUN_TEST(test_three_node_gateway_discovery_and_delivery);
  RUN_TEST(test_gateway_promotion_and_withdrawal_propagate_promptly_and_select_alternative);
  RUN_TEST(test_gateway_withdrawal_from_worse_path_clears_stale_gateway_eligibility);
  RUN_TEST(test_gateway_change_is_included_when_hello_cannot_fit_every_route);
  RUN_TEST(test_gateway_changes_over_hello_capacity_continue_in_bounded_updates);
  RUN_TEST(test_pending_gateway_withdrawal_survives_route_table_pressure);
  RUN_TEST(test_new_gateway_in_reused_route_slot_still_schedules_hello);
  RUN_TEST(test_gateway_transition_hello_updates_are_coalesced);
  RUN_TEST(test_gateway_hello_rate_limit_preserves_final_stable_state);
  RUN_TEST(test_silent_gateway_expires_after_three_missed_hellos_and_next_send_uses_alternative);
  RUN_TEST(test_app_send_is_queued_not_transmitted_inline);
  RUN_TEST(test_successful_radio_outcome_removes_packet_from_queue);
  RUN_TEST(test_radio_timeout_drops_failed_packet_and_continues_queue_without_retry);
  RUN_TEST(test_invalid_radio_parameters_drop_packet_without_retry);
  RUN_TEST(test_at_most_one_transmit_per_loop);
  RUN_TEST(test_forwarding_is_queued_not_inline);
  RUN_TEST(test_randomized_backoff_delays_transmit);
  RUN_TEST(test_full_queue_drops_packet);
  // Fabric Key DATA security / replay tests
  RUN_TEST(test_same_fabric_key_decrypts_payload);
  RUN_TEST(test_receive_dispatch_does_not_allocate);
  RUN_TEST(test_message_copy_owns_payload_for_deferred_automation);
  RUN_TEST(test_wrong_key_drops_packet);
  RUN_TEST(test_modified_data_does_not_reach_application_callback);
  RUN_TEST(test_modified_broadcast_data_is_not_delivered_or_forwarded);
  RUN_TEST(test_modified_transit_unicast_data_is_not_forwarded);
  RUN_TEST(test_modified_data_cannot_poison_seen_cache);
  RUN_TEST(test_modified_data_flags_do_not_reach_application_callback);
  RUN_TEST(test_data_with_trailing_bytes_does_not_reach_application_callback);
  RUN_TEST(test_protocol_v3_four_byte_data_tag_is_rejected);
  RUN_TEST(test_protocol_v3_four_byte_data_tag_is_not_forwarded);
  RUN_TEST(test_replay_rejected);
  RUN_TEST(test_replay_table_pressure_fails_closed_without_forgetting_known_sources);
  RUN_TEST(test_frame_counter_persists_across_reboot);
  RUN_TEST(test_power_loss_after_data_send_does_not_reuse_frame_counter);
  RUN_TEST(test_setup_save_failure_permanently_refuses_origination);
  RUN_TEST(test_setup_sync_failure_permanently_refuses_origination);
  RUN_TEST(test_runtime_rollover_refuses_data_until_reservation_is_durable);
  RUN_TEST(test_frame_counter_reservation_remains_batched);
  RUN_TEST(test_final_frame_counter_is_reserved_and_used_only_once);
  RUN_TEST(test_reboot_at_frame_counter_exhaustion_refuses_all_data_sends);
  printf("\n%s (%d failure%s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
