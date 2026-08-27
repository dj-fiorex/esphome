#pragma once

#include "esphome/core/helpers.h"
#include <cstdint>
#include <span>
#include <string>

namespace esphome::lora_mesh {

/** Maximum LoRa payload size (physical radio limit). */
static constexpr size_t LORA_MAX_PACKET_SIZE = 255;

/** Fixed-capacity packet buffer — fits any LoRa frame, zero heap allocation. */
using Packet = StaticVector<uint8_t, LORA_MAX_PACKET_SIZE>;

// ───────────────────────────────────────────────────────────────────────────
// Protocol constants
// ───────────────────────────────────────────────────────────────────────────

/** Wire protocol version carried in every HELLO body (bumped when the wire format changes). */
static constexpr uint8_t MESH_PROTO_VERSION = 4;

/** Maximum length of the human-readable node name carried in HELLO packets. */
static constexpr size_t MESH_NODE_NAME_MAX_LEN = 32;

/** Broadcast destination: every node in the mesh processes the packet. */
static constexpr uint32_t MESH_BROADCAST_ID = 0xFFFFFFFF;

/** Minimum packet size = fixed 28-byte header. */
static constexpr size_t MESH_HEADER_SIZE = 28;

/** Maximum application DATA payload: 255-byte frame - header - length - eight-byte tag. */
static constexpr size_t MESH_MAX_DATA_PAYLOAD_SIZE = 218;

// ───────────────────────────────────────────────────────────────────────────
// Header byte offsets (all fields are little-endian) — docs/wire-format.md §2
// ───────────────────────────────────────────────────────────────────────────
//
//  0  [4]  fabric_id      – public ID cryptographically derived from the Fabric Key
//  4  [1]  pkt_type       – see PacketType enum
//  5  [1]  flags          – see PacketFlags
//  6  [4]  src_id         – FNV-1a(node_id string) or FNV-1a(MAC bytes), immutable end-to-end
// 10  [4]  dst_id         – 0xFFFFFFFF = broadcast, immutable end-to-end
// 14  [4]  frame_counter  – per-source monotonic counter (wraps at 2^32)
// 18  [1]  ttl            – remaining forwarding hops, decremented on each hop
// 19  [1]  hop_count      – forwarding nodes already traversed; origin starts at zero
// 20  [4]  prev_hop       – node_id of the radio-level sender, rewritten on each forward
// 24  [4]  next_hop       – intended forwarder; 0xFFFFFFFF = any (broadcast/flood)
// ── total 28 bytes ──

static constexpr size_t MESH_OFF_FABRIC_ID = 0;
static constexpr size_t MESH_OFF_PKT_TYPE = 4;
static constexpr size_t MESH_OFF_FLAGS = 5;
static constexpr size_t MESH_OFF_SRC_ID = 6;
static constexpr size_t MESH_OFF_DST_ID = 10;
static constexpr size_t MESH_OFF_FRAME_COUNTER = 14;
static constexpr size_t MESH_OFF_TTL = 18;
static constexpr size_t MESH_OFF_HOP_COUNT = 19;
static constexpr size_t MESH_OFF_PREV_HOP = 20;
static constexpr size_t MESH_OFF_NEXT_HOP = 24;

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

static constexpr uint8_t FLAG_IS_GATEWAY = 0x01;    // Origin had upstream connectivity at send time
static constexpr uint8_t FLAG_IS_BROADCAST = 0x02;  // DATA destination is broadcast

// ───────────────────────────────────────────────────────────────────────────
// HELLO payload (after the 28-byte header)
//
//   [0]                   proto_version  (1 byte)  — must equal MESH_PROTO_VERSION
//   [1]                   name_len       (1 byte)  — byte length of node_name, 0 if absent
//   [2 .. 2+name_len-1]   node_name      (name_len bytes, NOT NUL-terminated on wire)
//   [2+name_len]          route_count    (1 byte)  — number of RouteAdvertisement entries
//   [3+name_len ..]       route entries  (7 bytes each)
//   [end-8 .. end-1]      auth_tag       (8 bytes) — truncated HMAC-SHA256 over header and body
//
// RouteAdvertisement (per entry):
//   [0..3]  dest_id      (uint32_t LE)
//   [4]     hop_count    (uint8_t)
//   [5]     rssi_scaled  (int8_t)  — RSSI dBm clamped to [-128, 127]
//   [6]     route_flags  (uint8_t) — bit 0: destination has Upstream Connectivity
//
// HELLO_FIXED_SIZE is the minimum number of HELLO-body bytes required to read
// proto_version and name_len (the two bytes needed before any further parsing).
// ───────────────────────────────────────────────────────────────────────────

static constexpr size_t HELLO_FIXED_SIZE = 2;
static constexpr size_t ROUTE_ADV_OFF_DEST_ID = 0;
static constexpr size_t ROUTE_ADV_OFF_HOP_COUNT = 4;
static constexpr size_t ROUTE_ADV_OFF_PATH_RSSI = 5;
static constexpr size_t ROUTE_ADV_OFF_FLAGS = 6;
static constexpr size_t ROUTE_ADV_SIZE = 7;
static constexpr uint8_t ROUTE_FLAG_IS_GATEWAY = 0x01;

// ───────────────────────────────────────────────────────────────────────────
// DATA payload (after the 28-byte header)
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
  bool gateway_update_pending : 1;

  RouteEntry() : is_gateway(false), is_valid(false), gateway_update_pending(false) {}
};

// ───────────────────────────────────────────────────────────────────────────
// Seen-packet cache entry (ring buffer for duplicate suppression)
// ───────────────────────────────────────────────────────────────────────────

struct SeenEntry {
  uint32_t src_id{0};
  uint32_t frame_counter{0};
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
  char source[9]{};                                // Hex of src_id hash (8 chars + NUL), always present
  char source_name[MESH_NODE_NAME_MAX_LEN + 1]{};  // Human-readable name, or empty string if unknown
  char destination[9]{};                           // Human-readable destination
  char prev_hop[9]{};                              // Node that sent this to us
  // Non-owning plaintext bytes. Valid only for the duration of the on_message
  // callback; copy at the application boundary if the payload must outlive it.
  std::span<const uint8_t> payload;
  uint32_t frame_counter{0};
  uint8_t hop_count{0};
  uint8_t ttl{0};
  float rssi{0.0f};
  float snr{0.0f};
  bool is_broadcast{false};
  bool is_for_this_node{false};
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
