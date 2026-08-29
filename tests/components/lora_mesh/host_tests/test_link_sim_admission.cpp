#include "test_harness.h"

int g_failures = 0;

using lmtest::BROADCAST;
using lmtest::TestNode;
using lmtest::fnv1a_str;
using lmtest::make_data;
using lmtest::make_hello;

static constexpr uint32_t FABRIC = 0xDEFF3662;

static void test_blocked_tampered_packet_is_dropped_before_authentication() {
  TestNode receiver("node-a");
  receiver.mesh.add_blocked_neighbor("node-b");
  auto packet = make_data(FABRIC, fnv1a_str("node-b"), BROADCAST, BROADCAST, "water", 7);
  packet.back() ^= 0x80;
  esphome::test_log_clear();

  receiver.receive(packet);

  EXPECT_TRUE(esphome::test_log_contains("link-sim: dropped packet"));
  EXPECT_FALSE(esphome::test_log_contains("authentication failed"));
}

static void test_recurring_blocklist_formatting_does_not_allocate() {
  TestNode receiver("node-a");
  const std::array<const char *, 4> names{{"node-000-abcdefghijklmnopqrstuvw", "node-001-abcdefghijklmnopqrstuvw",
                                           "node-002-abcdefghijklmnopqrstuvw", "node-003-abcdefghijklmnopqrstuvw"}};
  for (size_t index = 0; index < names.size(); ++index) {
    receiver.receive(
        make_hello(FABRIC, fnv1a_str(names[index]), names[index], {}, 0, static_cast<uint32_t>(index + 1)));
    receiver.mesh.add_blocked_neighbor(names[index]);
  }
  char blocked[esphome::lora_mesh::LoraMesh::BLOCKED_NEIGHBORS_TEXT_SIZE];

  esphome::test_allocations_begin();
  size_t length = receiver.mesh.write_blocked_neighbors(blocked, sizeof(blocked));
  size_t allocation_count = esphome::test_allocations_end();

  EXPECT_EQ(allocation_count, 0u);
  EXPECT_EQ(length, strlen(blocked));
  EXPECT_EQ(length, esphome::lora_mesh::LoraMesh::BLOCKED_NEIGHBORS_TEXT_SIZE - 1);
  std::string expected = receiver.mesh.get_blocked_neighbors_str();
  EXPECT_TRUE(expected == blocked);
}

int main() {
  RUN_TEST(test_blocked_tampered_packet_is_dropped_before_authentication);
  RUN_TEST(test_recurring_blocklist_formatting_does_not_allocate);
  printf("\n%s link-sim admission test (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
