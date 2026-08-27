#include "test_harness.h"

#include "esphome/components/lora_mesh/packet_admission.h"

int g_failures = 0;

using esphome::lora_mesh::AdmissionFailure;
using esphome::lora_mesh::PacketAdmission;
using lmtest::fnv1a_str;
using lmtest::make_data;
using lmtest::make_hello;

static const uint32_t FABRIC = 0xDEFF3662;
static const uint32_t NODE_A = fnv1a_str("node-a");
static const uint32_t NODE_B = fnv1a_str("node-b");

static PacketAdmission make_admission() {
  static const std::array<uint8_t, esphome::lora_mesh::FABRIC_KEY_SIZE> fabric_key = {
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
  };
  static const std::array<uint8_t, esphome::lora_mesh::CONTROL_PLANE_KEY_SIZE> control_key = [] {
    std::array<uint8_t, esphome::lora_mesh::CONTROL_PLANE_KEY_SIZE> derived{};
    esphome::lora_mesh::derive_control_plane_key(fabric_key.data(), derived.data());
    return derived;
  }();
  return PacketAdmission(FABRIC, fabric_key, control_key);
}

static void test_authentic_data_is_admitted_with_plaintext() {
  PacketAdmission admission = make_admission();
  auto packet = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "water", 7);

  auto inspection = admission.inspect(packet);
  auto result = admission.authenticate(packet, inspection.header);

  EXPECT_TRUE(inspection.accepted());
  EXPECT_TRUE(result.accepted());
  EXPECT_EQ(result.header.src_id, NODE_A);
  EXPECT_EQ(result.header.dst_id, NODE_B);
  EXPECT_EQ(result.plaintext_size, 5u);
  EXPECT_TRUE(memcmp(result.plaintext.data(), "water", 5) == 0);
}

static void test_tampered_data_is_rejected_before_plaintext_admission() {
  PacketAdmission admission = make_admission();
  auto packet = make_data(FABRIC, NODE_A, NODE_B, NODE_B, "water", 7);
  packet.back() ^= 0x80;

  auto inspection = admission.inspect(packet);
  auto result = admission.authenticate(packet, inspection.header);

  EXPECT_TRUE(inspection.accepted());
  EXPECT_FALSE(result.accepted());
  EXPECT_EQ(result.failure, AdmissionFailure::DATA_AUTHENTICATION_FAILED);
  EXPECT_EQ(result.plaintext_size, 0u);
}

static void test_authenticated_hello_with_invalid_route_shape_is_rejected() {
  PacketAdmission admission = make_admission();
  auto packet = make_hello(FABRIC, NODE_A, "node-a", {{NODE_B, 255, -70, false}}, 0, 8);

  auto inspection = admission.inspect(packet);
  auto result = admission.authenticate(packet, inspection.header);

  EXPECT_TRUE(inspection.accepted());
  EXPECT_FALSE(result.accepted());
  EXPECT_EQ(result.failure, AdmissionFailure::INVALID_HELLO);
}

int main() {
  RUN_TEST(test_authentic_data_is_admitted_with_plaintext);
  RUN_TEST(test_tampered_data_is_rejected_before_plaintext_admission);
  RUN_TEST(test_authenticated_hello_with_invalid_route_shape_is_rejected);
  printf("\n%s packet_admission focused tests (%d failure%s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures,
         g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
