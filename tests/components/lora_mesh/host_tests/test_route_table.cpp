#include "esphome/components/lora_mesh/route_table.h"

#include <cstdio>

using esphome::lora_mesh::RouteCandidate;
using esphome::lora_mesh::RouteTable;

static int failures = 0;

#define EXPECT_TRUE(condition) \
  do { \
    if (!(condition)) { \
      ++failures; \
      printf("  FAIL %s:%d: expected true: %s\n", __FILE__, __LINE__, #condition); \
    } \
  } while (0)

#define EXPECT_FALSE(condition) EXPECT_TRUE(!(condition))

#define EXPECT_EQ(left, right) EXPECT_TRUE((left) == (right))

static void test_current_path_reconfirmation_refreshes_metrics_and_lease() {
  RouteTable routes;
  routes.set_route_ttl(30000);

  EXPECT_TRUE(routes.consider({3, 2, 2, true, -60.0f, 7.0f}, 1000).changed);
  auto update = routes.consider({3, 2, 2, true, -90.0f, -4.0f}, 2000);

  EXPECT_TRUE(update.changed);
  const auto *route = routes.find(3);
  EXPECT_TRUE(route != nullptr);
  if (route != nullptr) {
    EXPECT_EQ(route->rssi, -90.0f);
    EXPECT_EQ(route->snr, -4.0f);
    EXPECT_EQ(route->expires_at, 32000u);
  }
}

static void test_expired_neighbor_holds_down_worse_dependent_feedback() {
  RouteTable routes;
  routes.set_route_ttl(10000);

  routes.observe_neighbor(2, false, -50.0f, 8.0f, 1000);
  routes.consider({3, 2, 2, false, -70.0f, 7.0f}, 1000);
  EXPECT_TRUE(routes.expire(11000));
  EXPECT_TRUE(routes.find(2) == nullptr);
  EXPECT_TRUE(routes.find(3) == nullptr);

  EXPECT_FALSE(routes.consider({3, 4, 3, false, -80.0f, 5.0f}, 11001).changed);
  EXPECT_TRUE(routes.find(3) == nullptr);

  EXPECT_TRUE(routes.consider({3, 4, 2, false, -75.0f, 5.0f}, 11002).changed);
  const auto *healed = routes.find(3);
  EXPECT_TRUE(healed != nullptr);
  if (healed != nullptr) {
    EXPECT_EQ(healed->next_hop_id, 4u);
  }
}

static void test_nearest_gateway_order_and_pending_state_are_owned_by_table() {
  RouteTable routes;

  auto first = routes.consider({30, 2, 2, true, -70.0f, 4.0f}, 1000);
  auto second = routes.consider({20, 3, 2, true, -70.0f, 4.0f}, 1000);
  EXPECT_TRUE(first.gateway_changed);
  EXPECT_TRUE(second.gateway_changed);
  EXPECT_EQ(routes.nearest_gateway()->dst_id, 20u);
  EXPECT_TRUE(routes.has_pending_gateway_updates());

  routes.acknowledge_gateway_update(20);
  EXPECT_TRUE(routes.has_pending_gateway_updates());
  routes.acknowledge_gateway_update(30);
  EXPECT_FALSE(routes.has_pending_gateway_updates());
}

static void test_full_table_rejects_worse_newcomer() {
  RouteTable routes;

  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.observe_neighbor(destination, false, -60.0f, 5.0f, 1000).changed);
  }

  EXPECT_FALSE(routes.consider({999, 2, 10, false, -100.0f, 1.0f}, 2000).changed);
  EXPECT_EQ(routes.count(), LORA_MESH_MAX_ROUTES);
  EXPECT_TRUE(routes.find(999) == nullptr);
  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.find(destination) != nullptr);
  }
}

static void test_full_table_replaces_worst_route_with_better_newcomer() {
  RouteTable routes;

  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    uint8_t hop_count = destination == 115 ? 8 : 2;
    EXPECT_TRUE(routes.consider({destination, 2, hop_count, false, -70.0f, 5.0f}, 1000).changed);
  }

  EXPECT_TRUE(routes.observe_neighbor(999, false, -80.0f, 3.0f, 2000).changed);
  EXPECT_EQ(routes.count(), LORA_MESH_MAX_ROUTES);
  EXPECT_TRUE(routes.find(999) != nullptr);
  EXPECT_TRUE(routes.find(115) == nullptr);
  for (uint32_t destination = 100; destination < 115; ++destination) {
    EXPECT_TRUE(routes.find(destination) != nullptr);
  }
}

static void test_full_table_uses_destination_id_to_break_exact_metric_ties() {
  RouteTable routes;

  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.consider({destination, 2, 2, false, -70.0f, 5.0f}, 1000).changed);
  }

  EXPECT_TRUE(routes.consider({50, 2, 2, false, -70.0f, 5.0f}, 2000).changed);
  EXPECT_TRUE(routes.find(50) != nullptr);
  EXPECT_TRUE(routes.find(115) == nullptr);
  EXPECT_FALSE(routes.consider({200, 2, 2, false, -70.0f, 5.0f}, 3000).changed);
  EXPECT_TRUE(routes.find(200) == nullptr);
}

static void test_full_table_preserves_pending_gateway_updates() {
  RouteTable routes;

  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.consider({destination, 2, 8, true, -100.0f, 1.0f}, 1000).changed);
  }

  EXPECT_FALSE(routes.observe_neighbor(999, false, -40.0f, 10.0f, 2000).changed);
  EXPECT_TRUE(routes.find(999) == nullptr);
  EXPECT_TRUE(routes.has_pending_gateway_updates());
  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.find(destination) != nullptr);
  }
}

static void test_full_table_preserves_hold_down_tombstones() {
  RouteTable routes;
  routes.set_route_ttl(10000);

  for (uint32_t destination = 100; destination < 100 + LORA_MESH_MAX_ROUTES; ++destination) {
    EXPECT_TRUE(routes.observe_neighbor(destination, false, -60.0f, 5.0f, 1000).changed);
  }
  EXPECT_TRUE(routes.expire(11000));

  EXPECT_FALSE(routes.observe_neighbor(999, false, -40.0f, 10.0f, 11001).changed);
  EXPECT_TRUE(routes.find(999) == nullptr);
  for (const auto &entry : routes.entries()) {
    EXPECT_FALSE(entry.is_valid);
    EXPECT_TRUE(entry.hold_down);
  }
}

int main() {
  test_current_path_reconfirmation_refreshes_metrics_and_lease();
  test_expired_neighbor_holds_down_worse_dependent_feedback();
  test_nearest_gateway_order_and_pending_state_are_owned_by_table();
  test_full_table_rejects_worse_newcomer();
  test_full_table_replaces_worst_route_with_better_newcomer();
  test_full_table_uses_destination_id_to_break_exact_metric_ties();
  test_full_table_preserves_pending_gateway_updates();
  test_full_table_preserves_hold_down_tombstones();
  printf("%s route_table focused tests (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
         failures == 1 ? "" : "s");
  return failures == 0 ? 0 : 1;
}
