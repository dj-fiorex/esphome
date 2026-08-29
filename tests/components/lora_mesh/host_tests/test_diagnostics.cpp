#include "test_harness.h"

int g_failures = 0;

using namespace lmtest;

static constexpr uint32_t FABRIC = 0xDEFF3662;

static void test_route_change_diagnostic_publication_does_not_allocate() {
  FakeRadio radio;
  LoraMesh mesh(TEST_FABRIC_KEY_HEX, &radio);
  esphome::text_sensor::TextSensor routing_table_sensor;
  mesh.set_node_id(esphome::TemplatableValue<std::string>("node-a"));
  mesh.set_routing_table_sensor(&routing_table_sensor);
  mesh.setup();
  mesh.loop();

  auto hello = make_hello(FABRIC, fnv1a_str("node-b"), "node-b");
  esphome::test_allocations_begin();
  mesh.on_radio_packet(hello.data(), hello.size(), -60.0f, 8.0f);
  size_t allocation_count = esphome::test_allocations_end();

  EXPECT_EQ(allocation_count, 0u);
  EXPECT_TRUE(routing_table_sensor.state.find("\"dst\":\"") != std::string::npos);
}

static void test_periodic_diagnostic_publication_does_not_allocate() {
  FakeRadio radio;
  LoraMesh mesh(TEST_FABRIC_KEY_HEX, &radio);
  esphome::text_sensor::TextSensor routing_table_sensor;
  mesh.set_node_id(esphome::TemplatableValue<std::string>("node-a"));
  mesh.set_discovery_interval(60000);
  mesh.set_routing_table_sensor(&routing_table_sensor);
  mesh.setup();
  mesh.loop();
  auto hello = make_hello(FABRIC, fnv1a_str("node-b"), "node-b");
  mesh.on_radio_packet(hello.data(), hello.size(), -60.0f, 8.0f);

  esphome::test_clock_advance(29000);
  esphome::test_allocations_begin();
  mesh.loop();
  size_t allocation_count = esphome::test_allocations_end();

  EXPECT_EQ(allocation_count, 0u);
  EXPECT_TRUE(routing_table_sensor.state.find("\"name\":\"node-b\"") != std::string::npos);
}

static void test_maximum_route_table_growth_diagnostics_do_not_allocate() {
  static_assert(LORA_MESH_MAX_ROUTES == 255);
  FakeRadio radio;
  LoraMesh mesh(TEST_FABRIC_KEY_HEX, &radio);
  esphome::text_sensor::TextSensor routing_table_sensor;
  mesh.set_node_id(esphome::TemplatableValue<std::string>("node-a"));
  mesh.set_routing_table_sensor(&routing_table_sensor);
  mesh.setup();
  mesh.loop();

  std::array<std::vector<uint8_t>, LORA_MESH_MAX_ROUTES> hellos;
  for (size_t index = 0; index < hellos.size(); ++index) {
    char name[esphome::lora_mesh::MESH_NODE_NAME_MAX_LEN + 1];
    snprintf(name, sizeof(name), "node-%03zu-abcdefghijklmnopqrstuvw", index);
    hellos[index] = make_hello(FABRIC, fnv1a_str(name), name, {}, 0, static_cast<uint32_t>(index + 1));
  }

  esphome::test_allocations_begin();
  for (const auto &hello : hellos) {
    mesh.on_radio_packet(hello.data(), hello.size(), -60.0f, 8.0f);
  }
  size_t allocation_count = esphome::test_allocations_end();

  EXPECT_EQ(allocation_count, 0u);
  EXPECT_EQ(mesh.get_known_node_count(), static_cast<size_t>(LORA_MESH_MAX_ROUTES));
  EXPECT_TRUE(routing_table_sensor.state.size() < LoraMesh::ROUTING_TABLE_JSON_BUFFER_SIZE);
  EXPECT_TRUE(routing_table_sensor.state.front() == '[');
  EXPECT_TRUE(routing_table_sensor.state.back() == ']');
}

int main() {
  RUN_TEST(test_route_change_diagnostic_publication_does_not_allocate);
  RUN_TEST(test_periodic_diagnostic_publication_does_not_allocate);
  RUN_TEST(test_maximum_route_table_growth_diagnostics_do_not_allocate);
  printf("\n%s diagnostics test (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
