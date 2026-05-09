#pragma once

#include "esphome/core/helpers.h"
#include <cstdint>
#include <string>

namespace esphome::lora_mesh {

/** Maximum LoRa payload size (physical radio limit). */
static constexpr size_t LORA_MAX_PACKET_SIZE = 255;

/** Fixed-capacity packet buffer — fits any LoRa frame, zero heap allocation. */
using Packet = StaticVector<uint8_t, LORA_MAX_PACKET_SIZE>;

// ───────────────────────────────────────────────────────────────────────────
// Protocol constants
// ───────────────────────────────────────────────────────────────────────────

/** Wire protocol version carried in every HELLO body (bumped when body layout changes). */
static constexpr uint8_t MESH_PROTO_VERSION = 2;

/** Maximum length of the human-readable node name carried in HELLO packets. */
static constexpr size_t MESH_NODE_NAME_MAX_LEN = 32;

/** Broadcast destination: every node in the mesh processes the packet. */
static constexpr uint32_t MESH_BROADCAST_ID = 0xFFFFFFFF;

/** Minimum packet size = fixed 24-byte header. */
static constexpr size_t MESH_HEADER_SIZE = 24;

// ───────────────────────────────────────────────────────────────────────────
// Header byte offsets (all fields are little-endian)
// ───────────────────────────────────────────────────────────────────────────
//
//  0  [4]  mesh_id     – FNV-1a(mesh_secret), never the raw secret
//  4  [1]  pkt_type    – see PacketType enum
//  5  [1]  flags       – see PacketFlags
//  6  [4]  src_id      – FNV-1a(node_id string) or FNV-1a(MAC bytes)
// 10  [4]  dst_id      – 0xFFFFFFFF = broadcast
// 14  [4]  msg_id      – per-source monotonic counter (wraps at 2^32)
// 18  [1]  ttl         – remaining forwarding hops, decremented on each hop
// 19  [1]  hop_count   – hops already traversed
// 20  [4]  prev_hop    – node_id of the radio-level sender
// ── total 24 bytes ──

// ───────────────────────────────────────────────────────────────────────────
// Packet types
// ───────────────────────────────────────────────────────────────────────────

enum class PacketType : uint8_t {
  HELLO = 1,          // Periodic beacon / neighbour discovery
  DATA = 2,           // Application payload
  ROUTE_REQUEST = 3,  // Ask for a route (reserved for future use)
  ROUTE_REPLY = 4,    // Route reply (reserved for future use)
  ACK = 5,            // Acknowledgement (reserved for future use)
  ERROR = 6,          // Route error / unreachable
};

// ───────────────────────────────────────────────────────────────────────────
// Flags bitfield (byte 5)
// ───────────────────────────────────────────────────────────────────────────

static constexpr uint8_t FLAG_IS_GATEWAY = 0x01;    // Sender is a gateway
static constexpr uint8_t FLAG_IS_BROADCAST = 0x02;  // Packet is a broadcast
static constexpr uint8_t FLAG_ACK_REQUESTED = 0x04;
static constexpr uint8_t FLAG_IS_FORWARD = 0x08;  // This is a forwarded packet

// ───────────────────────────────────────────────────────────────────────────
// HELLO payload (after the 24-byte header)
//
//   [0]                   proto_version  (1 byte)  — must equal MESH_PROTO_VERSION
//   [1]                   name_len       (1 byte)  — byte length of node_name, 0 if absent
//   [2 .. 2+name_len-1]   node_name      (name_len bytes, NOT NUL-terminated on wire)
//   [2+name_len]          route_count    (1 byte)  — number of RouteAdvertisement entries
//   [3+name_len ..]       route entries  (6 bytes each)
//
// RouteAdvertisement (per entry):
//   [0..3]  dest_id      (uint32_t LE)
//   [4]     hop_count    (uint8_t)
//   [5]     rssi_scaled  (int8_t)  — RSSI dBm clamped to [-128, 127]
//
// HELLO_FIXED_SIZE is the minimum number of HELLO-body bytes required to read
// proto_version and name_len (the two bytes needed before any further parsing).
// ───────────────────────────────────────────────────────────────────────────

static constexpr size_t HELLO_FIXED_SIZE = 2;
static constexpr size_t ROUTE_ADV_SIZE = 6;

// ───────────────────────────────────────────────────────────────────────────
// DATA payload (after the 24-byte header)
//
//   [0]     payload_len  (uint8_t)
//   [1+]    raw payload bytes (up to payload_len)
//
// ───────────────────────────────────────────────────────────────────────────

// ───────────────────────────────────────────────────────────────────────────
// Routing table entry (stored in-memory, not transmitted verbatim)
// ───────────────────────────────────────────────────────────────────────────

struct RouteEntry {
  uint32_t dst_id{0};       // Destination node hash
  uint32_t next_hop_id{0};  // Next hop node hash
  uint32_t expires_at{0};   // millis() deadline
  uint32_t last_seen{0};    // millis() of last update
  float rssi{0.0f};
  float snr{0.0f};
  uint8_t hop_count{0};
  bool is_gateway : 1;
  bool is_valid : 1;

  RouteEntry() : is_gateway(false), is_valid(false) {}
};

// ───────────────────────────────────────────────────────────────────────────
// Seen-packet cache entry (ring buffer for duplicate suppression)
// ───────────────────────────────────────────────────────────────────────────

struct SeenEntry {
  uint32_t src_id{0};
  uint32_t msg_id{0};
  uint32_t expires_at{0};  // millis() deadline
};

// ───────────────────────────────────────────────────────────────────────────
// Node name cache entry (stored in-memory, not transmitted verbatim)
// Maps a 32-bit node hash to the human-readable name learned from HELLO packets.
// ───────────────────────────────────────────────────────────────────────────

struct NameEntry {
  uint32_t id{0};
  bool is_valid{false};
  char name[MESH_NODE_NAME_MAX_LEN + 1]{};
};

// ───────────────────────────────────────────────────────────────────────────
// MeshMessage — payload delivered to on_message automation trigger
// ───────────────────────────────────────────────────────────────────────────

struct MeshMessage {
  char source[9]{};                                   // Hex of src_id hash (8 chars + NUL), always present
  char source_name[MESH_NODE_NAME_MAX_LEN + 1]{};     // Human-readable name, or empty string if unknown
  char destination[9]{};                              // Human-readable destination
  char prev_hop[9]{};                                 // Node that sent this to us
  std::string payload;                         // Application data
  uint32_t msg_id{0};
  uint8_t hop_count{0};
  uint8_t ttl{0};
  float rssi{0.0f};
  float snr{0.0f};
  bool is_broadcast{false};
  bool is_for_this_node{false};
};

// ───────────────────────────────────────────────────────────────────────────
// Gateway mode (set via YAML `gateway:` option)
// ───────────────────────────────────────────────────────────────────────────

enum class GatewayMode : uint8_t {
  NORMAL = 0,   // Never acts as gateway
  GATEWAY = 1,  // Always acts as gateway
  AUTO = 2,     // Gateway when Wi-Fi/internet is available
};

// ───────────────────────────────────────────────────────────────────────────
// FNV-1a 32-bit hash — constexpr, no heap allocation
// ───────────────────────────────────────────────────────────────────────────

inline constexpr uint32_t fnv1a_32(const uint8_t *data, size_t len) {
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 0x01000193u;
  }
  return h;
}

inline uint32_t fnv1a_str(const std::string &s) {
  return fnv1a_32(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}

// ───────────────────────────────────────────────────────────────────────────
// Little-endian serialisation helpers
// ───────────────────────────────────────────────────────────────────────────

inline void put_u32_le(uint8_t *buf, uint32_t v) {
  buf[0] = static_cast<uint8_t>(v);
  buf[1] = static_cast<uint8_t>(v >> 8);
  buf[2] = static_cast<uint8_t>(v >> 16);
  buf[3] = static_cast<uint8_t>(v >> 24);
}

inline uint32_t get_u32_le(const uint8_t *buf) {
  return static_cast<uint32_t>(buf[0]) | (static_cast<uint32_t>(buf[1]) << 8) | (static_cast<uint32_t>(buf[2]) << 16) |
         (static_cast<uint32_t>(buf[3]) << 24);
}

}  // namespace esphome::lora_mesh
