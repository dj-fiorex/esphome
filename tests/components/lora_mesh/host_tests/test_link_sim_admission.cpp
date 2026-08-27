#include "test_harness.h"

int g_failures = 0;

using lmtest::BROADCAST;
using lmtest::TestNode;
using lmtest::fnv1a_str;
using lmtest::make_data;

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

int main() {
  RUN_TEST(test_blocked_tampered_packet_is_dropped_before_authentication);
  printf("\n%s link-sim admission test (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
