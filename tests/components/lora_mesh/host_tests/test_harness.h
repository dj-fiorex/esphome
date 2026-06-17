#pragma once

// Tiny dependency-free test harness for lora_mesh host tests, plus the fake
// radio and wire-format helpers shared by all test cases.
//
// Tests exercise LoraMesh only through its public interface: configuration
// setters, setup()/loop(), the send APIs, callbacks, and on_radio_packet().
// Packets fed to the node are built here from the wire-format spec
// (docs/wire-format.md), independently of the component's own builders.

#include "esphome/components/lora_mesh/lora_mesh.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace esphome {
void test_clock_set(uint32_t ms);
void test_clock_advance(uint32_t ms);
void test_random_set(uint32_t v);
}  // namespace esphome

// ── Assertions ──────────────────────────────────────────────────────────────

extern int g_failures;

#define EXPECT_TRUE(cond) \
  do { \
    if (!(cond)) { \
      ++g_failures; \
      printf("  FAIL %s:%d: expected true: %s\n", __FILE__, __LINE__, #cond); \
    } \
  } while (0)

#define EXPECT_FALSE(cond) EXPECT_TRUE(!(cond))

#define EXPECT_EQ(a, b) \
  do { \
    auto va_ = (a); \
    auto vb_ = (b); \
    if (!(va_ == vb_)) { \
      ++g_failures; \
      printf("  FAIL %s:%d: %s == %s: %llu vs %llu\n", __FILE__, __LINE__, #a, #b, \
             static_cast<unsigned long long>(va_), static_cast<unsigned long long>(vb_)); \
    } \
  } while (0)

#define RUN_TEST(fn) \
  do { \
    int before_ = g_failures; \
    esphome::test_clock_set(1000); \
    esphome::test_random_set(0); \
    fn(); \
    printf("%s %s\n", g_failures == before_ ? "PASS" : "FAIL", #fn); \
  } while (0)

// ── Fake radio ──────────────────────────────────────────────────────────────

namespace lmtest {

using esphome::lora_mesh::LoraMesh;
using esphome::lora_mesh::Packet;
using esphome::lora_mesh::fnv1a_str;
using esphome::lora_mesh::get_u32_le;
using esphome::lora_mesh::put_u32_le;

class FakeRadio : public esphome::lora_mesh::LoRaRadio {
 public:
  void transmit_packet(const Packet &data) override { this->sent.emplace_back(data.begin(), data.end()); }
  size_t get_max_packet_size() override { return 255; }
  void attach_listener(LoraMesh *mesh) override { this->listener = mesh; }

  std::vector<std::vector<uint8_t>> sent;
  LoraMesh *listener{nullptr};
};

// ── Node fixture ────────────────────────────────────────────────────────────

struct TestNode {
  FakeRadio radio;
  LoraMesh mesh;
  std::vector<esphome::lora_mesh::MeshMessage> received;

  explicit TestNode(const std::string &node_id, const std::string &fabric = "fabric-1") {
    this->mesh.set_radio(&this->radio);
    this->mesh.set_node_id(esphome::TemplatableValue<std::string>(node_id));
    this->mesh.set_mesh_secret(fabric);
    this->mesh.add_on_message_callback(
        [this](const esphome::lora_mesh::MeshMessage &msg) { this->received.push_back(msg); });
    this->mesh.setup();
    // The first loop() emits the initial HELLO (frame_counter 1, jitter
    // stubbed to 0); consume it so tests start with an empty TX queue.
    this->mesh.loop();
    this->radio.sent.clear();
  }

  void receive(const std::vector<uint8_t> &pkt, float rssi = -60.0f, float snr = 8.0f) {
    this->mesh.on_radio_packet(pkt.data(), pkt.size(), rssi, snr);
  }
};

// ── Wire-format builders (spec §2, §4, §5 of docs/wire-format.md) ──────────

constexpr size_t HDR = 28;
constexpr uint32_t BROADCAST = 0xFFFFFFFF;
constexpr uint8_t PKT_HELLO = 1;
constexpr uint8_t PKT_DATA = 2;
constexpr uint8_t FLAG_GATEWAY = 0x01;
constexpr uint8_t FLAG_BROADCAST = 0x02;

inline std::vector<uint8_t> make_header(uint32_t fabric_id, uint8_t pkt_type, uint8_t flags, uint32_t src_id,
                                        uint32_t dst_id, uint32_t frame_counter, uint8_t ttl, uint8_t hop_count,
                                        uint32_t prev_hop, uint32_t next_hop) {
  std::vector<uint8_t> h(HDR, 0);
  put_u32_le(&h[0], fabric_id);
  h[4] = pkt_type;
  h[5] = flags;
  put_u32_le(&h[6], src_id);
  put_u32_le(&h[10], dst_id);
  put_u32_le(&h[14], frame_counter);
  h[18] = ttl;
  h[19] = hop_count;
  put_u32_le(&h[20], prev_hop);
  put_u32_le(&h[24], next_hop);
  return h;
}

struct RouteAdv {
  uint32_t dest_id;
  uint8_t hop_count;
  int8_t rssi;
};

/// Single-hop HELLO from `src` (spec §5): proto_version 3 body at offset 28.
inline std::vector<uint8_t> make_hello(uint32_t fabric_id, uint32_t src_id, const std::string &name,
                                       const std::vector<RouteAdv> &routes = {}, uint8_t flags = 0,
                                       uint32_t frame_counter = 1) {
  auto pkt = make_header(fabric_id, PKT_HELLO, flags, src_id, BROADCAST, frame_counter, 8, 0, src_id, BROADCAST);
  pkt.push_back(3);  // proto_version
  pkt.push_back(static_cast<uint8_t>(name.size()));
  pkt.insert(pkt.end(), name.begin(), name.end());
  pkt.push_back(static_cast<uint8_t>(routes.size()));
  for (const auto &r : routes) {
    uint8_t entry[6];
    put_u32_le(entry, r.dest_id);
    entry[4] = r.hop_count;
    entry[5] = static_cast<uint8_t>(r.rssi);
    pkt.insert(pkt.end(), entry, entry + 6);
  }
  return pkt;
}

/// DATA packet (spec §4, plaintext payload for this slice — crypto is a later issue).
inline std::vector<uint8_t> make_data(uint32_t fabric_id, uint32_t src_id, uint32_t dst_id, uint32_t next_hop,
                                      const std::string &payload, uint32_t frame_counter = 1, uint8_t ttl = 8,
                                      uint8_t hop_count = 0, uint32_t prev_hop = 0, uint8_t extra_flags = 0) {
  uint8_t flags = extra_flags;
  if (dst_id == BROADCAST) {
    flags |= FLAG_BROADCAST;
  }
  auto pkt = make_header(fabric_id, PKT_DATA, flags, src_id, dst_id, frame_counter, ttl, hop_count,
                         prev_hop != 0 ? prev_hop : src_id, next_hop);
  pkt.push_back(static_cast<uint8_t>(payload.size()));
  pkt.insert(pkt.end(), payload.begin(), payload.end());
  return pkt;
}

}  // namespace lmtest
