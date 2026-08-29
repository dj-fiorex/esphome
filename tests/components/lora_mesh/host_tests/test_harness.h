#pragma once

// Tiny dependency-free test harness for lora_mesh host tests, plus the fake
// radio and wire-format helpers shared by all test cases.
//
// Tests exercise LoraMesh only through its public interface: configuration
// setters, setup()/loop(), the send APIs, callbacks, and on_radio_packet().
// Packets fed to the node are built here from the wire-format spec
// (docs/wire-format.md), independently of the component's own builders.

#include "esphome/components/lora_mesh/lora_mesh.h"
#include "esphome/components/lora_mesh/lora_mesh_crypto.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace esphome {
void test_clock_set(uint32_t ms);
void test_clock_advance(uint32_t ms);
void test_random_set(uint32_t v);
void test_preferences_clear();
void test_preferences_set_save_succeeds(bool succeeds);
void test_preferences_set_sync_succeeds(bool succeeds);
void test_preferences_simulate_power_loss();
void test_preferences_put_u32(uint32_t key, uint32_t value);
uint32_t test_preferences_get_u32(uint32_t key);
void test_log_clear();
bool test_log_contains(const char *text);
void test_allocations_begin();
size_t test_allocations_end();
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
    esphome::test_preferences_clear(); \
    fn(); \
    printf("%s %s\n", g_failures == before_ ? "PASS" : "FAIL", #fn); \
  } while (0)

// ── Fake radio ──────────────────────────────────────────────────────────────

namespace lmtest {

using esphome::lora_mesh::LoraMesh;
using esphome::lora_mesh::Packet;
using esphome::lora_mesh::TransmissionOutcome;
using esphome::lora_mesh::fnv1a_str;
using esphome::lora_mesh::get_u32_le;
using esphome::lora_mesh::put_u32_le;
using esphome::lora_mesh::DATA_AUTH_TAG_SIZE;
using esphome::lora_mesh::FABRIC_KEY_SIZE;

class FakeRadio : public esphome::lora_mesh::LoRaRadio {
 public:
  TransmissionOutcome transmit_packet(const Packet &data) override {
    this->attempted.emplace_back(data.begin(), data.end());
    if (this->transmit_outcome == TransmissionOutcome::SUCCESS) {
      this->sent.emplace_back(data.begin(), data.end());
    }
    return this->transmit_outcome;
  }
  size_t get_max_packet_size() override { return this->max_packet_size; }
  void attach_listener(LoraMesh *mesh) override { this->listener = mesh; }

  std::vector<std::vector<uint8_t>> attempted;
  std::vector<std::vector<uint8_t>> sent;
  LoraMesh *listener{nullptr};
  size_t max_packet_size{255};
  TransmissionOutcome transmit_outcome{TransmissionOutcome::SUCCESS};
};

static constexpr const char *TEST_FABRIC_KEY_HEX = "0102030405060708090a0b0c0d0e0f10";
static const uint8_t TEST_FABRIC_KEY[FABRIC_KEY_SIZE] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                                         0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

// ── Node fixture ────────────────────────────────────────────────────────────

struct TestNode {
  struct CapturedPayload {
    std::array<uint8_t, esphome::lora_mesh::MESH_MAX_DATA_PAYLOAD_SIZE> bytes{};
    size_t length{0};

    void assign(std::span<const uint8_t> payload) {
      this->length = payload.size();
      memcpy(this->bytes.data(), payload.data(), payload.size());
    }

    template<size_t N> bool operator==(const char (&text)[N]) const {
      return this->length == N - 1 && memcmp(this->bytes.data(), text, N - 1) == 0;
    }
  };

  struct CapturedMessage {
    char source[9]{};
    char source_name[esphome::lora_mesh::MESH_NODE_NAME_MAX_LEN + 1]{};
    char destination[9]{};
    char prev_hop[9]{};
    CapturedPayload payload;
    uint32_t frame_counter{0};
    uint8_t hop_count{0};
    uint8_t ttl{0};
    float rssi{0.0f};
    float snr{0.0f};
    bool is_broadcast{false};
    bool is_for_this_node{false};
  };

  FakeRadio radio;
  LoraMesh mesh;
  std::vector<CapturedMessage> received;

  explicit TestNode(const std::string &node_id, const std::string &fabric_key_hex = TEST_FABRIC_KEY_HEX)
      : mesh(fabric_key_hex, &this->radio) {
    this->mesh.set_node_id(esphome::TemplatableValue<std::string>(node_id));
    this->mesh.add_on_message_callback([this](const esphome::lora_mesh::MeshMessage &msg) {
      CapturedMessage captured;
      memcpy(captured.source, msg.source, sizeof(captured.source));
      memcpy(captured.source_name, msg.source_name, sizeof(captured.source_name));
      memcpy(captured.destination, msg.destination, sizeof(captured.destination));
      memcpy(captured.prev_hop, msg.prev_hop, sizeof(captured.prev_hop));
      captured.payload.assign(msg.payload);
      captured.frame_counter = msg.frame_counter;
      captured.hop_count = msg.hop_count;
      captured.ttl = msg.ttl;
      captured.rssi = msg.rssi;
      captured.snr = msg.snr;
      captured.is_broadcast = msg.is_broadcast;
      captured.is_for_this_node = msg.is_for_this_node;
      this->received.push_back(captured);
    });
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

// Advances real LoraMesh instances through the public radio boundary. Packets
// transmitted in one round are delivered to linked neighbours before the next
// round, making HELLO/lease progression and DATA forwarding deterministic.
template<size_t N> class TestTopology {
 public:
  explicit TestTopology(const std::array<TestNode *, N> &nodes) : nodes_(nodes) {}

  void connect(size_t left, size_t right) {
    this->links_[left][right] = true;
    this->links_[right][left] = true;
  }

  void disconnect(size_t left, size_t right) {
    this->links_[left][right] = false;
    this->links_[right][left] = false;
  }

  size_t advance(uint32_t ms) {
    esphome::test_clock_advance(ms);
    return this->run_round();
  }

  size_t run_round() {
    for (TestNode *node : this->nodes_) {
      node->mesh.loop();
    }

    size_t transmissions = 0;
    for (size_t sender = 0; sender < N; ++sender) {
      auto &packets = this->nodes_[sender]->radio.sent;
      while (this->delivered_[sender] < packets.size()) {
        const auto &packet = packets[this->delivered_[sender]++];
        ++transmissions;
        for (size_t receiver = 0; receiver < N; ++receiver) {
          if (this->links_[sender][receiver]) {
            this->nodes_[receiver]->receive(packet);
          }
        }
      }
    }
    return transmissions;
  }

 private:
  std::array<TestNode *, N> nodes_;
  std::array<std::array<bool, N>, N> links_{};
  std::array<size_t, N> delivered_{};
};

// ── Wire-format builders (spec §2, §4, §5 of docs/wire-format.md) ──────────

constexpr size_t HDR = 28;
constexpr uint32_t BROADCAST = 0xFFFFFFFF;
constexpr uint8_t PKT_HELLO = 1;
constexpr uint8_t PKT_DATA = 2;
constexpr uint8_t FLAG_GATEWAY = 0x01;
constexpr uint8_t FLAG_BROADCAST = 0x02;
constexpr size_t HELLO_TAG_SIZE = 8;

inline std::vector<uint8_t> hello_auth_tag(const std::vector<uint8_t> &packet_without_tag,
                                           const uint8_t *fabric_key = TEST_FABRIC_KEY) {
  uint8_t control_key[esphome::lora_mesh::CONTROL_PLANE_KEY_SIZE];
  uint8_t tag[HELLO_TAG_SIZE];
  esphome::lora_mesh::derive_control_plane_key(fabric_key, control_key);
  esphome::lora_mesh::compute_hello_auth_tag(control_key, packet_without_tag.data(), packet_without_tag.size(), tag);
  return std::vector<uint8_t>(tag, tag + HELLO_TAG_SIZE);
}

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
  bool is_gateway{false};
};

/// Single-hop HELLO from `src` (spec §5): protocol-v4 body at offset 28.
inline std::vector<uint8_t> make_hello(uint32_t fabric_id, uint32_t src_id, const std::string &name,
                                       const std::vector<RouteAdv> &routes = {}, uint8_t flags = 0,
                                       uint32_t frame_counter = 1, const uint8_t *fabric_key = TEST_FABRIC_KEY) {
  auto pkt = make_header(fabric_id, PKT_HELLO, flags, src_id, BROADCAST, frame_counter, 8, 0, src_id, BROADCAST);
  pkt.push_back(4);  // proto_version
  pkt.push_back(static_cast<uint8_t>(name.size()));
  pkt.insert(pkt.end(), name.begin(), name.end());
  pkt.push_back(static_cast<uint8_t>(routes.size()));
  for (const auto &r : routes) {
    uint8_t entry[7];
    put_u32_le(entry, r.dest_id);
    entry[4] = r.hop_count;
    entry[5] = static_cast<uint8_t>(r.rssi);
    entry[6] = r.is_gateway ? FLAG_GATEWAY : 0;
    pkt.insert(pkt.end(), entry, entry + 7);
  }
  auto tag = hello_auth_tag(pkt, fabric_key);
  pkt.insert(pkt.end(), tag.begin(), tag.end());
  return pkt;
}

inline void resign_hello(std::vector<uint8_t> &pkt, const uint8_t *fabric_key = TEST_FABRIC_KEY) {
  pkt.resize(pkt.size() - HELLO_TAG_SIZE);
  auto tag = hello_auth_tag(pkt, fabric_key);
  pkt.insert(pkt.end(), tag.begin(), tag.end());
}

/// Encrypted DATA packet (spec §4): payload encrypted with the Fabric Key and an eight-byte tag.
inline std::vector<uint8_t> make_data(uint32_t fabric_id, uint32_t src_id, uint32_t dst_id, uint32_t next_hop,
                                      const std::string &payload, uint32_t frame_counter = 1, uint8_t ttl = 8,
                                      uint8_t hop_count = 0, uint32_t prev_hop = 0, uint8_t extra_flags = 0,
                                      const uint8_t *fabric_key = TEST_FABRIC_KEY) {
  uint8_t flags = extra_flags;
  if (dst_id == BROADCAST) {
    flags |= FLAG_BROADCAST;
  }
  auto pkt = make_header(fabric_id, PKT_DATA, flags, src_id, dst_id, frame_counter, ttl, hop_count,
                         prev_hop != 0 ? prev_hop : src_id, next_hop);
  uint8_t payload_len = static_cast<uint8_t>(payload.size());
  pkt.push_back(payload_len);

  uint8_t ciphertext[esphome::lora_mesh::MESH_MAX_DATA_PAYLOAD_SIZE];
  uint8_t tag[DATA_AUTH_TAG_SIZE];
  esphome::lora_mesh::mesh_encrypt_payload(fabric_key, src_id, dst_id, frame_counter, PKT_DATA, flags, payload_len,
                                           reinterpret_cast<const uint8_t *>(payload.data()), ciphertext, tag);
  pkt.insert(pkt.end(), ciphertext, ciphertext + payload_len);
  pkt.insert(pkt.end(), tag, tag + DATA_AUTH_TAG_SIZE);
  return pkt;
}

}  // namespace lmtest
