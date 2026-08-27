#include "lora_mesh.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::lora_mesh {

static const char *const TAG = "lora_mesh";

// NVS key prefixes for preference hashes.
static const char *const NVS_PREFIX_FRAME_COUNTER = "lora_mesh_fc_";

static bool is_preferred_gateway_route(const RouteEntry &candidate, const RouteEntry *current) {
  return current == nullptr || candidate.hop_count < current->hop_count ||
         (candidate.hop_count == current->hop_count &&
          (candidate.rssi > current->rssi || (candidate.rssi == current->rssi && candidate.dst_id < current->dst_id)));
}

// ─── Utility ──────────────────────────────────────────────────────────────────

void LoraMesh::id_to_hex(uint32_t id, char out[9]) { snprintf(out, 9, "%08" PRIX32, id); }

LoraMesh::LoraMesh(const std::string &fabric_key_hex) {
  if (fabric_key_hex.size() != FABRIC_KEY_SIZE * 2) {
    return;
  }
  for (size_t i = 0; i < FABRIC_KEY_SIZE; i++) {
    auto hex_value = [](char value) -> int {
      if (value >= '0' && value <= '9') {
        return value - '0';
      }
      if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
      }
      if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
      }
      return -1;
    };
    int high = hex_value(fabric_key_hex[i * 2]);
    int low = hex_value(fabric_key_hex[i * 2 + 1]);
    if (high < 0 || low < 0) {
      this->fabric_key_.fill(0);
      return;
    }
    this->fabric_key_[i] = static_cast<uint8_t>((high << 4) | low);
  }
  this->fabric_key_valid_ = true;
  this->fabric_id_ = derive_fabric_id(this->fabric_key_.data());
  derive_control_plane_key(this->fabric_key_.data(), this->control_plane_key_.data());
}

// ─── Component lifecycle ──────────────────────────────────────────────────────

void LoraMesh::setup() {
  if (!this->fabric_key_valid_) {
    ESP_LOGE(TAG, "Invalid Fabric Key — marking failed");
    this->mark_failed();
    return;
  }

  // Derive numeric IDs from strings.
  if (this->has_node_id_) {
    this->node_id_str_ = this->node_id_template_.value();
    this->node_id_ = fnv1a_str(this->node_id_str_);
  } else {
    uint8_t mac[6];
    get_mac_address_raw(mac);
    this->node_id_ = fnv1a_32(mac + 3, 3);
    char buf[7];
    snprintf(buf, sizeof(buf), "%06" PRIX32, this->node_id_ & 0xFFFFFFu);
    this->node_id_str_ = buf;
  }
  // Initialise arrays.
  for (auto &r : this->routes_) {
    r = RouteEntry{};
  }
  for (auto &s : this->seen_cache_) {
    s = SeenEntry{};
  }
  for (auto &n : this->name_map_) {
    n = NameEntry{};
  }
  for (auto &rc : this->replay_counters_) {
    rc = ReplayEntry{};
  }

  this->load_frame_counter_();

  if (this->radio_ == nullptr) {
    ESP_LOGE(TAG, "No radio configured — marking failed");
    this->mark_failed();
    return;
  }

  // Register as listener on the radio adapter.
  this->radio_->attach_listener(this);

  // Stagger the first HELLO by a random offset so devices booted simultaneously
  // do not collide on the channel.
  uint32_t jitter_ms = static_cast<uint32_t>(random_uint32() % (this->discovery_interval_ms_ / 5));
  this->last_hello_ = millis() - this->discovery_interval_ms_ + jitter_ms;
  this->setup_complete_ = true;

  ESP_LOGI(TAG, "LoraMesh setup: node_id=%s (0x%08" PRIX32 ") fabric_id=0x%08" PRIX32 " upstream=%s",
           this->node_id_str_.c_str(), this->node_id_, this->fabric_id_, this->upstream_connected_ ? "yes" : "no");
}

void LoraMesh::loop() {
  uint32_t now = millis();

  // Periodic and transition-triggered HELLOs share one coalesced, rate-limited path.
  if (now - this->last_hello_ >= this->discovery_interval_ms_) {
    this->hello_update_pending_ = true;
  }
  this->queue_pending_hello_(now);

  // Route and seen-cache expiry (every 10 s).
  if (now - this->last_expire_check_ >= 10000) {
    this->last_expire_check_ = now;
    this->expire_routes_();
    this->expire_seen_();
  }

  // Diagnostic sensor publishing (every 30 s).
  if (now - this->last_diag_publish_ >= 30000) {
    this->last_diag_publish_ = now;
    this->publish_diagnostics_();
  }

  // Transmit at most one queued packet per loop iteration.
  this->drain_tx_queue_(now);
}

void LoraMesh::dump_config() {
  ESP_LOGCONFIG(TAG, "LoraMesh:");
  ESP_LOGCONFIG(TAG, "  Node ID: %s (0x%08" PRIX32 ")", this->node_id_str_.c_str(), this->node_id_);
  ESP_LOGCONFIG(TAG, "  Upstream connectivity: %s", this->upstream_connected_ ? "connected" : "disconnected");
  ESP_LOGCONFIG(TAG, "  Max hops: %u", this->max_hops_);
  ESP_LOGCONFIG(TAG, "  Discovery interval: %" PRIu32 " ms", this->discovery_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Route TTL: %" PRIu32 " ms", this->route_ttl_ms_);
  ESP_LOGCONFIG(TAG, "  Forward messages: %s", this->forward_messages_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Max routes: %zu", this->routes_.size());
  ESP_LOGCONFIG(TAG, "  Seen cache size: %zu", this->seen_cache_.size());
  ESP_LOGCONFIG(TAG, "  TX queue size: %zu", this->tx_queue_.size());
  ESP_LOGCONFIG(TAG, "  TX jitter: %" PRIu32 " ms", this->tx_jitter_ms_);
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool LoraMesh::send_message(const std::string &destination, std::span<const uint8_t> payload) {
  uint32_t dst_id = fnv1a_str(destination);
  const RouteEntry *route = this->find_route_(dst_id);
  if (route == nullptr) {
    ESP_LOGW(TAG, "No route to %s", destination.c_str());
    return false;
  }
  auto pkt = this->build_data_packet_(dst_id, route->next_hop_id, payload);
  if (pkt.empty()) {
    return false;
  }
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA queued to %s (%zu bytes)", destination.c_str(), payload.size());
  return true;
}

bool LoraMesh::broadcast_message(std::span<const uint8_t> payload) {
  auto pkt = this->build_data_packet_(MESH_BROADCAST_ID, MESH_BROADCAST_ID, payload);
  if (pkt.empty()) {
    return false;
  }
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA broadcast queued (%zu bytes)", payload.size());
  return true;
}

bool LoraMesh::send_to_gateway(std::span<const uint8_t> payload) {
  const RouteEntry *gw = this->find_nearest_gateway_route_();
  if (gw == nullptr) {
    ESP_LOGW(TAG, "send_to_gateway: no gateway in routing table");
    return false;
  }
  auto pkt = this->build_data_packet_(gw->dst_id, gw->next_hop_id, payload);
  if (pkt.empty()) {
    return false;
  }
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA queued to gateway 0x%08" PRIX32 " (%zu bytes)", gw->dst_id, payload.size());
  return true;
}

void LoraMesh::set_upstream_connected(bool connected) {
  if (connected == this->upstream_connected_) {
    return;
  }
  this->upstream_connected_ = connected;
  ESP_LOGI(TAG, "Upstream connectivity changed → %s", connected ? "connected" : "disconnected");
  if (this->setup_complete_) {
    this->schedule_hello_update_();
  }
}

bool LoraMesh::has_route(const std::string &node_id) const { return this->find_route_(fnv1a_str(node_id)) != nullptr; }

bool LoraMesh::has_gateway() const { return this->find_nearest_gateway_route_() != nullptr; }

std::string LoraMesh::get_nearest_gateway() const {
  const RouteEntry *gw = this->find_nearest_gateway_route_();
  if (gw == nullptr) {
    return {};
  }
  char buf[9];
  LoraMesh::id_to_hex(gw->dst_id, buf);
  return buf;
}

void LoraMesh::clear_routes() {
  for (auto &r : this->routes_) {
    r.is_valid = false;
  }
  this->notify_route_changed_();
  ESP_LOGI(TAG, "Routing table cleared");
}

size_t LoraMesh::get_known_node_count() const {
  size_t count = 0;
  for (const auto &r : this->routes_) {
    if (r.is_valid) {
      ++count;
    }
  }
  return count;
}

std::string LoraMesh::get_routing_table_json() const {
  // Pre-reserve: each entry is ~90 bytes; +2 for '[' and ']'.
  size_t entry_count = this->get_known_node_count();
  std::string out;
  // Pre-reserve: '[' + ']' + entry_count * ~130 bytes per entry
  // (~96 base JSON + up to 32 chars for "name" field + commas and overhead).
  out.reserve(2 + entry_count * 130);
  out = '[';
  bool first = true;
  for (const auto &r : this->routes_) {
    if (!r.is_valid) {
      continue;
    }
    if (!first) {
      out += ',';
    }
    first = false;
    // Buffer: base JSON structure (~96 bytes) + "name" field (up to MESH_NODE_NAME_MAX_LEN=32) + safety margin.
    char buf[160];
    char dst_hex[9], nh_hex[9];
    LoraMesh::id_to_hex(r.dst_id, dst_hex);
    LoraMesh::id_to_hex(r.next_hop_id, nh_hex);
    const char *name = this->lookup_node_name_(r.dst_id);
    snprintf(buf, sizeof(buf), R"({"dst":"%s","name":"%s","nh":"%s","hops":%u,"gw":%s,"rssi":%.0f})", dst_hex,
             name != nullptr ? name : "", nh_hex, r.hop_count, r.is_gateway ? "true" : "false",
             static_cast<double>(r.rssi));
    out += buf;
  }
  out += ']';
  return out;
}

#ifdef LORA_MESH_LINK_SIM
// ─── Link simulator (debug/test only — see ADR-0004) ──────────────────────────

void LoraMesh::add_blocked_neighbor(const std::string &name) {
  if (name.empty()) {
    return;
  }
  uint32_t id = fnv1a_str(name);
  for (size_t i = 0; i < this->blocked_count_; i++) {
    if (this->blocked_neighbors_[i] == id) {
      return;  // already blocked
    }
  }
  if (this->blocked_count_ >= this->blocked_neighbors_.size()) {
    ESP_LOGW(TAG, "link-sim: blocklist full (%zu), ignoring '%s'", this->blocked_neighbors_.size(), name.c_str());
    return;
  }
  this->blocked_neighbors_[this->blocked_count_++] = id;
  ESP_LOGI(TAG, "link-sim: blocking neighbour '%s' (0x%08" PRIX32 ")", name.c_str(), id);
}

void LoraMesh::clear_blocked_neighbors() {
  this->blocked_count_ = 0;
  ESP_LOGI(TAG, "link-sim: blocklist cleared");
}

bool LoraMesh::is_link_blocked_(uint32_t prev_hop) const {
  for (size_t i = 0; i < this->blocked_count_; i++) {
    if (this->blocked_neighbors_[i] == prev_hop) {
      return true;
    }
  }
  return false;
}

std::string LoraMesh::get_blocked_neighbors_str() const {
  std::string out;
  for (size_t i = 0; i < this->blocked_count_; i++) {
    if (i != 0) {
      out += ", ";
    }
    const char *name = this->lookup_node_name_(this->blocked_neighbors_[i]);
    if (name != nullptr) {
      out += name;
    } else {
      char hex[9];
      LoraMesh::id_to_hex(this->blocked_neighbors_[i], hex);
      out += hex;
    }
  }
  return out;
}
#endif  // LORA_MESH_LINK_SIM

// ─── Radio packet dispatcher ──────────────────────────────────────────────────

void LoraMesh::on_radio_packet(const uint8_t *pkt, size_t pkt_len, float rssi, float snr) {
  if (pkt_len < MESH_HEADER_SIZE) {
    ESP_LOGW(TAG, "Packet too short (%zu bytes), dropped", pkt_len);
    return;
  }
  const uint8_t *h = pkt;

  // Fabric ID filter (Forwarding gate).
  uint32_t fabric_id = get_u32_le(h + MESH_OFF_FABRIC_ID);
  if (fabric_id != this->fabric_id_) {
    ESP_LOGV(TAG, "Fabric ID mismatch (0x%08" PRIX32 " vs 0x%08" PRIX32 "), dropped", fabric_id, this->fabric_id_);
    return;
  }

  auto pkt_type = static_cast<PacketType>(h[MESH_OFF_PKT_TYPE]);
  uint8_t flags = h[MESH_OFF_FLAGS];
  uint32_t src_id = get_u32_le(h + MESH_OFF_SRC_ID);
  uint32_t dst_id = get_u32_le(h + MESH_OFF_DST_ID);
  uint32_t frame_counter = get_u32_le(h + MESH_OFF_FRAME_COUNTER);
  uint8_t ttl = h[MESH_OFF_TTL];
  uint8_t hop_count = h[MESH_OFF_HOP_COUNT];
  uint32_t prev_hop = get_u32_le(h + MESH_OFF_PREV_HOP);
  uint32_t next_hop = get_u32_le(h + MESH_OFF_NEXT_HOP);
  uint8_t plaintext[MESH_MAX_DATA_PAYLOAD_SIZE];
  uint8_t payload_len = 0;

#ifdef LORA_MESH_LINK_SIM
  // Link simulator: pretend this packet was never received because its
  // immediate sender is "out of range". Dropped before duplicate/seen handling
  // so the blocked neighbour leaves no trace at all. See ADR-0004.
  if (this->is_link_blocked_(prev_hop)) {
    ESP_LOGD(TAG, "link-sim: dropped packet from prev_hop 0x%08" PRIX32 " (blocked)", prev_hop);
    return;
  }
#endif

  // Ignore our own transmissions echoed back.
  if (src_id == this->node_id_) {
    return;
  }

  // Reject unsupported or malformed packet types before duplicate suppression.
  // In particular, changing an authenticated HELLO's type byte must not let the
  // forged frame reserve its source/counter pair in the Seen-cache.
  switch (pkt_type) {
    case PacketType::HELLO:
      if (!this->validate_hello_packet_(pkt, pkt_len)) {
        ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " failed validation or authentication", src_id);
        return;
      }
      break;
    case PacketType::DATA:
      if (!this->validate_data_envelope_(pkt, pkt_len, dst_id, flags)) {
        return;
      }
      payload_len = pkt[MESH_HEADER_SIZE];
      if (!mesh_decrypt_payload(this->fabric_key_.data(), src_id, dst_id, frame_counter,
                                static_cast<uint8_t>(PacketType::DATA), flags, payload_len, pkt + MESH_HEADER_SIZE + 1,
                                plaintext, pkt + MESH_HEADER_SIZE + 1 + payload_len)) {
        ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " authentication failed, dropped", src_id);
        return;
      }
      break;
    default:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(pkt_type), src_id);
      return;
  }

  // Duplicate suppression.
  if (this->is_duplicate_(src_id, frame_counter)) {
    ESP_LOGV(TAG, "Duplicate from 0x%08" PRIX32 " frame=%" PRIu32 ", dropped", src_id, frame_counter);
    return;
  }
  this->mark_seen_(src_id, frame_counter);

  bool src_is_gw = (flags & FLAG_IS_GATEWAY) != 0;

  switch (pkt_type) {
    case PacketType::HELLO:
      this->process_hello_(pkt, pkt_len, MESH_HEADER_SIZE, src_id, src_is_gw, prev_hop, rssi, snr);
      break;
    case PacketType::DATA:
      this->process_data_(pkt, pkt_len, plaintext, payload_len, src_id, dst_id, frame_counter, ttl, hop_count, prev_hop,
                          next_hop, flags, rssi, snr);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(pkt_type), src_id);
      break;
  }
}

// ─── HELLO processing ─────────────────────────────────────────────────────────

bool LoraMesh::validate_hello_packet_(const uint8_t *pkt, size_t pkt_len) const {
  constexpr size_t minimum_size = MESH_HEADER_SIZE + 3 + HELLO_AUTH_TAG_SIZE;
  if (pkt_len < minimum_size) {
    return false;
  }
  size_t authenticated_size = pkt_len - HELLO_AUTH_TAG_SIZE;
  size_t offset = MESH_HEADER_SIZE;
  if (pkt[offset] != MESH_PROTO_VERSION) {
    return false;
  }
  uint8_t flags = pkt[MESH_OFF_FLAGS];
  uint32_t src_id = get_u32_le(pkt + MESH_OFF_SRC_ID);
  if ((flags & ~FLAG_IS_GATEWAY) != 0 || get_u32_le(pkt + MESH_OFF_DST_ID) != MESH_BROADCAST_ID ||
      pkt[MESH_OFF_TTL] == 0 || pkt[MESH_OFF_HOP_COUNT] != 0 || get_u32_le(pkt + MESH_OFF_PREV_HOP) != src_id ||
      get_u32_le(pkt + MESH_OFF_NEXT_HOP) != MESH_BROADCAST_ID) {
    return false;
  }
  uint8_t name_len = pkt[offset + 1];
  if (name_len > MESH_NODE_NAME_MAX_LEN) {
    return false;
  }
  size_t route_count_offset = offset + 2 + static_cast<size_t>(name_len);
  if (route_count_offset >= authenticated_size) {
    return false;
  }
  uint8_t route_count = pkt[route_count_offset];
  size_t expected_size = route_count_offset + 1 + static_cast<size_t>(route_count) * ROUTE_ADV_SIZE;
  if (expected_size != authenticated_size) {
    return false;
  }
  for (size_t pos = route_count_offset + 1; pos < authenticated_size; pos += ROUTE_ADV_SIZE) {
    uint32_t advertised_destination = get_u32_le(pkt + pos + ROUTE_ADV_OFF_DEST_ID);
    uint8_t advertised_hops = pkt[pos + ROUTE_ADV_OFF_HOP_COUNT];
    uint8_t route_flags = pkt[pos + ROUTE_ADV_OFF_FLAGS];
    if (advertised_destination == MESH_BROADCAST_ID || advertised_destination == src_id || advertised_hops == 0 ||
        advertised_hops == UINT8_MAX || (route_flags & ~ROUTE_FLAG_IS_GATEWAY) != 0) {
      return false;
    }
  }
  return verify_hello_auth_tag(this->control_plane_key_.data(), pkt, authenticated_size, pkt + authenticated_size);
}

bool LoraMesh::validate_data_envelope_(const uint8_t *pkt, size_t pkt_len, uint32_t dst_id, uint8_t flags) const {
  if (pkt_len < MESH_HEADER_SIZE + 1) {
    return false;
  }
  uint8_t payload_len = pkt[MESH_HEADER_SIZE];
  size_t expected_size = MESH_HEADER_SIZE + 1 + payload_len + DATA_AUTH_TAG_SIZE;
  if (payload_len > MESH_MAX_DATA_PAYLOAD_SIZE || pkt_len != expected_size) {
    return false;
  }
  bool is_broadcast = dst_id == MESH_BROADCAST_ID;
  return is_broadcast == ((flags & FLAG_IS_BROADCAST) != 0);
}

void LoraMesh::process_hello_(const uint8_t *pkt, size_t pkt_len, size_t offset, uint32_t src_id, bool src_is_gateway,
                              uint32_t prev_hop, float rssi, float snr) {
  if (pkt_len < offset + HELLO_FIXED_SIZE) {
    return;
  }

  // Reject packets from nodes running a different protocol version.
  if (pkt[offset] != MESH_PROTO_VERSION) {
    ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 ": proto version %u (expected %u), skipping body", src_id, pkt[offset],
             MESH_PROTO_VERSION);
    return;
  }

  // A packet from another protocol version must not change mesh state.
  this->update_route_(src_id, src_id, 1, src_is_gateway, rssi, snr);

  uint8_t name_len = pkt[offset + 1];

  // Ensure the packet contains name bytes and the route_count byte.
  if (pkt_len < offset + 2 + static_cast<size_t>(name_len) + 1) {
    return;
  }

  if (name_len > 0) {
    this->store_node_name_(src_id, reinterpret_cast<const char *>(pkt + offset + 2), name_len);
  }

  uint8_t route_count = pkt[offset + 2 + name_len];
  size_t pos = offset + 3 + name_len;
  size_t authenticated_size = pkt_len - HELLO_AUTH_TAG_SIZE;

  for (uint8_t i = 0; i < route_count && pos + ROUTE_ADV_SIZE <= authenticated_size; ++i, pos += ROUTE_ADV_SIZE) {
    uint32_t adv_dst = get_u32_le(pkt + pos + ROUTE_ADV_OFF_DEST_ID);
    uint8_t adv_hops = pkt[pos + ROUTE_ADV_OFF_HOP_COUNT];
    float advertised_rssi = static_cast<float>(static_cast<int8_t>(pkt[pos + ROUTE_ADV_OFF_PATH_RSSI]));
    bool advertised_gateway = (pkt[pos + ROUTE_ADV_OFF_FLAGS] & ROUTE_FLAG_IS_GATEWAY) != 0;

    if (adv_dst == this->node_id_ || adv_dst == src_id) {
      continue;
    }
    uint16_t new_hops = static_cast<uint16_t>(adv_hops) + 1;
    if (new_hops > this->max_hops_) {
      continue;
    }
    float path_rssi = std::min(advertised_rssi, rssi);
    const RouteEntry *existing = this->find_route_(adv_dst);
    // Accept a strictly better path from anyone; renew the lease when our
    // current next hop re-confirms the route at equal quality (ADR 0002) —
    // otherwise stable multi-hop routes expire and flap every route_ttl.
    bool better = existing == nullptr || new_hops < existing->hop_count ||
                  (new_hops == existing->hop_count && path_rssi > existing->rssi);
    bool reconfirmed = existing != nullptr && existing->next_hop_id == src_id && new_hops == existing->hop_count;
    if (better || reconfirmed) {
      this->update_route_(adv_dst, src_id, static_cast<uint8_t>(new_hops), advertised_gateway, path_rssi, snr);
    }
  }

  const char *src_name = this->lookup_node_name_(src_id);
  ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " name=%s rssi=%.0f snr=%.1f gw=%s routes=%u", src_id,
           src_name != nullptr ? src_name : "?", static_cast<double>(rssi), static_cast<double>(snr),
           src_is_gateway ? "yes" : "no", route_count);
}

// ─── DATA processing ──────────────────────────────────────────────────────────

void LoraMesh::process_data_(const uint8_t *pkt, size_t pkt_len, const uint8_t *plaintext, uint8_t payload_len,
                             uint32_t src_id, uint32_t dst_id, uint32_t frame_counter, uint8_t ttl, uint8_t hop_count,
                             uint32_t prev_hop, uint32_t next_hop, uint8_t flags, float rssi, float snr) {
  bool is_broadcast = dst_id == MESH_BROADCAST_ID;
  bool is_for_us = is_broadcast || (dst_id == this->node_id_);

  if (is_for_us) {
    if (this->is_replay_(src_id, frame_counter)) {
      ESP_LOGW(TAG, "DATA from 0x%08" PRIX32 " frame=%" PRIu32 " replay rejected", src_id, frame_counter);
      return;
    }
    this->update_replay_counter_(src_id, frame_counter);

    MeshMessage msg;
    LoraMesh::id_to_hex(src_id, msg.source);
    const char *src_name = this->lookup_node_name_(src_id);
    if (src_name != nullptr) {
      snprintf(msg.source_name, sizeof(msg.source_name), "%s", src_name);
    }
    LoraMesh::id_to_hex(dst_id, msg.destination);
    LoraMesh::id_to_hex(prev_hop, msg.prev_hop);
    msg.payload.assign(plaintext, plaintext + payload_len);
    msg.frame_counter = frame_counter;
    msg.hop_count = hop_count;
    msg.ttl = ttl;
    msg.rssi = rssi;
    msg.snr = snr;
    msg.is_broadcast = is_broadcast;
    msg.is_for_this_node = !is_broadcast;

    ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " hops=%u len=%u", src_id, hop_count, payload_len);
    this->message_callback_(msg);
  }

  // Forward if TTL allows, forwarding is enabled, and packet is not unicast-only-to-us.
  if (!this->forward_messages_ || ttl <= 1 || (is_for_us && !is_broadcast)) {
    return;
  }

  // Single-path unicast (ADR 0002): only the designated next hop forwards.
  const RouteEntry *route = nullptr;
  if (!is_broadcast) {
    if (next_hop != this->node_id_) {
      return;
    }
    route = this->find_route_(dst_id);
    if (route == nullptr) {
      ESP_LOGW(TAG, "No route to forward 0x%08" PRIX32, dst_id);
      return;
    }
  }

  // Packet forwarding: copy the incoming packet into a stack-allocated Packet,
  // then patch the TTL, hop_count, prev_hop and next_hop fields before retransmitting.
  // The authenticated immutable header fields and encrypted body are Forwarded verbatim.
  Packet fwd(pkt, pkt + pkt_len);
  fwd[MESH_OFF_TTL] = ttl - 1;
  fwd[MESH_OFF_HOP_COUNT] = hop_count + 1;
  put_u32_le(&fwd[MESH_OFF_PREV_HOP], this->node_id_);

  if (is_broadcast) {
    put_u32_le(&fwd[MESH_OFF_NEXT_HOP], MESH_BROADCAST_ID);
    this->enqueue_tx_(fwd);
    ESP_LOGD(TAG, "Forward broadcast queued ttl=%u", ttl - 1);
    return;
  }

  put_u32_le(&fwd[MESH_OFF_NEXT_HOP], route->next_hop_id);
  this->enqueue_tx_(fwd);
  ESP_LOGD(TAG, "Forward unicast queued to 0x%08" PRIX32 " via 0x%08" PRIX32 " ttl=%u", dst_id, route->next_hop_id,
           ttl - 1);
}

// ─── Packet builders ──────────────────────────────────────────────────────────

Packet LoraMesh::build_header_(PacketType type, uint8_t flags, uint32_t dst_id, uint32_t frame_counter, uint8_t ttl,
                               uint8_t hop_count, uint32_t prev_hop, uint32_t next_hop) const {
  uint8_t buf[MESH_HEADER_SIZE]{};
  put_u32_le(buf + MESH_OFF_FABRIC_ID, this->fabric_id_);
  buf[MESH_OFF_PKT_TYPE] = static_cast<uint8_t>(type);
  buf[MESH_OFF_FLAGS] = flags;
  put_u32_le(buf + MESH_OFF_SRC_ID, this->node_id_);
  put_u32_le(buf + MESH_OFF_DST_ID, dst_id);
  put_u32_le(buf + MESH_OFF_FRAME_COUNTER, frame_counter);
  buf[MESH_OFF_TTL] = ttl;
  buf[MESH_OFF_HOP_COUNT] = hop_count;
  put_u32_le(buf + MESH_OFF_PREV_HOP, prev_hop);
  put_u32_le(buf + MESH_OFF_NEXT_HOP, next_hop);
  return Packet(buf, buf + MESH_HEADER_SIZE);
}

Packet LoraMesh::build_hello_packet_() {
  uint8_t flags = this->upstream_connected_ ? FLAG_IS_GATEWAY : 0;

  // Collect valid routes. Gateway state changes take bounded priority so an
  // immediate HELLO cannot omit the promotion or Withdrawal when every Route
  // does not fit; remaining changes stay pending for a later rate-limited HELLO.
  static_assert(LORA_MESH_MAX_ROUTES <= 255, "LORA_MESH_MAX_ROUTES must be <= 255");
  std::array<const RouteEntry *, LORA_MESH_MAX_ROUTES> ptrs{};
  size_t count = 0;
  for (const auto &r : this->routes_) {
    if (r.is_valid && count < ptrs.size()) {
      ptrs[count++] = &r;
    }
  }
  // Otherwise sort by hop_count ascending to prioritise close neighbours.
  std::sort(ptrs.begin(), ptrs.begin() + static_cast<ptrdiff_t>(count),
            [](const RouteEntry *left_route, const RouteEntry *right_route) {
              if (left_route->gateway_update_pending != right_route->gateway_update_pending) {
                return left_route->gateway_update_pending;
              }
              return left_route->hop_count < right_route->hop_count;
            });

  size_t name_len = std::min(this->node_id_str_.size(), MESH_NODE_NAME_MAX_LEN);
  // Overhead = proto_version(1) + name_len_field(1) + name(name_len) + route_count(1).
  size_t hello_overhead = 3 + name_len;
  size_t max_pkt = (this->radio_ != nullptr) ? this->radio_->get_max_packet_size() : 255;
  size_t fixed_size = MESH_HEADER_SIZE + hello_overhead + HELLO_AUTH_TAG_SIZE;
  size_t budget = max_pkt > fixed_size ? max_pkt - fixed_size : 0;
  size_t max_routes = std::min(budget / ROUTE_ADV_SIZE, count);
  if (max_routes > 255) {
    max_routes = 255;
  }

  // HELLO is single-hop: next_hop is broadcast but receivers never forward it.
  auto pkt = this->build_header_(PacketType::HELLO, flags, MESH_BROADCAST_ID, this->next_frame_counter_(),
                                 this->max_hops_, 0, this->node_id_, MESH_BROADCAST_ID);
  pkt.push_back(MESH_PROTO_VERSION);
  pkt.push_back(static_cast<uint8_t>(name_len));
  for (size_t i = 0; i < name_len; ++i) {
    pkt.push_back(static_cast<uint8_t>(this->node_id_str_[i]));
  }
  pkt.push_back(static_cast<uint8_t>(max_routes));

  for (size_t i = 0; i < max_routes; ++i) {
    const RouteEntry *r = ptrs[i];
    uint8_t entry[ROUTE_ADV_SIZE];
    put_u32_le(entry + ROUTE_ADV_OFF_DEST_ID, r->dst_id);
    entry[ROUTE_ADV_OFF_HOP_COUNT] = r->hop_count;
    int clamped = static_cast<int>(r->rssi);
    clamped = (clamped < -128) ? -128 : (clamped > 127) ? 127 : clamped;
    entry[ROUTE_ADV_OFF_PATH_RSSI] = static_cast<uint8_t>(static_cast<int8_t>(clamped));
    entry[ROUTE_ADV_OFF_FLAGS] = r->is_gateway ? ROUTE_FLAG_IS_GATEWAY : 0;
    for (uint8_t b : entry) {
      pkt.push_back(b);
    }
  }
  uint8_t tag[HELLO_AUTH_TAG_SIZE];
  compute_hello_auth_tag(this->control_plane_key_.data(), pkt.data(), pkt.size(), tag);
  for (uint8_t byte : tag) {
    pkt.push_back(byte);
  }
  return pkt;
}

Packet LoraMesh::build_data_packet_(uint32_t dst_id, uint32_t next_hop, std::span<const uint8_t> payload) {
  uint8_t flags = this->upstream_connected_ ? FLAG_IS_GATEWAY : 0;
  if (dst_id == MESH_BROADCAST_ID) {
    flags |= FLAG_IS_BROADCAST;
  }
  uint32_t fc = this->next_frame_counter_();
  auto pkt = this->build_header_(PacketType::DATA, flags, dst_id, fc, this->max_hops_, 0, this->node_id_, next_hop);
  // Budget = radio frame limit minus header, payload_len byte, and authentication tag.
  size_t max_pkt = (this->radio_ != nullptr) ? this->radio_->get_max_packet_size() : LORA_MAX_PACKET_SIZE;
  max_pkt = std::min(max_pkt, LORA_MAX_PACKET_SIZE);
  if (max_pkt < MESH_HEADER_SIZE + 1 + DATA_AUTH_TAG_SIZE) {
    ESP_LOGE(TAG, "Radio packet limit is too small for encrypted DATA");
    return {};
  }
  size_t len = std::min(payload.size(), max_pkt - MESH_HEADER_SIZE - 1 - DATA_AUTH_TAG_SIZE);
  pkt.push_back(static_cast<uint8_t>(len));

  uint8_t ciphertext[MESH_MAX_DATA_PAYLOAD_SIZE];
  uint8_t tag[DATA_AUTH_TAG_SIZE];
  bool encrypted =
      mesh_encrypt_payload(this->fabric_key_.data(), this->node_id_, dst_id, fc, static_cast<uint8_t>(PacketType::DATA),
                           flags, static_cast<uint8_t>(len), payload.data(), ciphertext, tag);
  if (!encrypted) {
    ESP_LOGE(TAG, "Failed to encrypt DATA packet");
    return {};
  }
  for (size_t i = 0; i < len; ++i) {
    pkt.push_back(ciphertext[i]);
  }
  for (size_t i = 0; i < DATA_AUTH_TAG_SIZE; ++i) {
    pkt.push_back(tag[i]);
  }
  return pkt;
}

// ─── TX queue ─────────────────────────────────────────────────────────────────

bool LoraMesh::enqueue_tx_(const Packet &pkt) {
  if (this->tx_queue_count_ >= this->tx_queue_.size()) {
    ESP_LOGW(TAG, "TX queue full (%zu), packet dropped", this->tx_queue_.size());
    return false;
  }
  size_t tail = (this->tx_queue_head_ + this->tx_queue_count_) % this->tx_queue_.size();
  this->tx_queue_[tail] = pkt;
  ++this->tx_queue_count_;
  return true;
}

void LoraMesh::drain_tx_queue_(uint32_t now) {
  if (this->tx_queue_count_ == 0) {
    return;
  }
  // Sample a fresh random backoff once per head packet (poor-man's CSMA:
  // de-syncs Forwarding Nodes that all queued the same packet at the same instant).
  if (!this->tx_backoff_armed_) {
    uint32_t backoff = this->tx_jitter_ms_ > 0 ? random_uint32() % (this->tx_jitter_ms_ + 1) : 0;
    this->tx_next_tx_at_ = now + backoff;
    this->tx_backoff_armed_ = true;
  }
  if (static_cast<int32_t>(now - this->tx_next_tx_at_) < 0) {
    return;
  }
  if (this->radio_ != nullptr) {
    this->radio_->transmit_packet(this->tx_queue_[this->tx_queue_head_]);
  }
  this->tx_queue_head_ = (this->tx_queue_head_ + 1) % this->tx_queue_.size();
  --this->tx_queue_count_;
  this->tx_backoff_armed_ = false;
}

void LoraMesh::schedule_hello_update_() { this->hello_update_pending_ = true; }

void LoraMesh::queue_pending_hello_(uint32_t now) {
  if (!this->hello_update_pending_ || now - this->last_hello_ < HELLO_UPDATE_MIN_INTERVAL_MS) {
    return;
  }
  auto pkt = this->build_hello_packet_();
  if (this->enqueue_tx_(pkt)) {
    this->acknowledge_advertised_gateway_updates_(pkt);
    this->hello_update_pending_ = this->has_pending_gateway_updates_();
    this->last_hello_ = now;
    ESP_LOGD(TAG, "HELLO queued (%zu bytes)", pkt.size());
  }
}

void LoraMesh::acknowledge_advertised_gateway_updates_(const Packet &hello) {
  size_t route_count_offset = MESH_HEADER_SIZE + 2 + hello[MESH_HEADER_SIZE + 1];
  uint8_t route_count = hello[route_count_offset];
  size_t route_offset = route_count_offset + 1;
  for (uint8_t index = 0; index < route_count; ++index, route_offset += ROUTE_ADV_SIZE) {
    RouteEntry *route = this->find_route_(get_u32_le(&hello[route_offset + ROUTE_ADV_OFF_DEST_ID]));
    if (route != nullptr) {
      route->gateway_update_pending = false;
    }
  }
}

bool LoraMesh::has_pending_gateway_updates_() const {
  for (const auto &route : this->routes_) {
    if (route.is_valid && route.gateway_update_pending) {
      return true;
    }
  }
  return false;
}

// ─── Routing table ────────────────────────────────────────────────────────────

RouteEntry *LoraMesh::find_route_(uint32_t dst_id) {
  for (auto &r : this->routes_) {
    if (r.is_valid && r.dst_id == dst_id) {
      return &r;
    }
  }
  return nullptr;
}

const RouteEntry *LoraMesh::find_route_(uint32_t dst_id) const {
  for (const auto &r : this->routes_) {
    if (r.is_valid && r.dst_id == dst_id) {
      return &r;
    }
  }
  return nullptr;
}

RouteEntry *LoraMesh::find_nearest_gateway_route_() {
  RouteEntry *nearest = nullptr;
  for (auto &r : this->routes_) {
    if (!r.is_valid || !r.is_gateway) {
      continue;
    }
    if (is_preferred_gateway_route(r, nearest)) {
      nearest = &r;
    }
  }
  return nearest;
}

const RouteEntry *LoraMesh::find_nearest_gateway_route_() const {
  const RouteEntry *nearest = nullptr;
  for (const auto &r : this->routes_) {
    if (!r.is_valid || !r.is_gateway) {
      continue;
    }
    if (is_preferred_gateway_route(r, nearest)) {
      nearest = &r;
    }
  }
  return nearest;
}

RouteEntry *LoraMesh::alloc_route_slot_() {
  for (auto &r : this->routes_) {
    if (!r.is_valid) {
      return &r;
    }
  }
  // Table full: evict the non-pending route with most hops, lowest RSSI. A
  // Gateway change remains protected until a HELLO has actually advertised it;
  // if every Route is pending, defer learning the new Route until a later HELLO.
  RouteEntry *worst = nullptr;
  for (auto &r : this->routes_) {
    if (r.gateway_update_pending) {
      continue;
    }
    if (worst == nullptr || r.hop_count > worst->hop_count ||
        (r.hop_count == worst->hop_count && r.rssi < worst->rssi)) {
      worst = &r;
    }
  }
  if (worst != nullptr) {
    ESP_LOGD(TAG, "Route table full, evicting 0x%08" PRIX32, worst->dst_id);
    worst->is_valid = false;
  } else {
    ESP_LOGD(TAG, "Route table full of pending Gateway updates, deferring new Route");
  }
  return worst;
}

void LoraMesh::update_route_(uint32_t dst_id, uint32_t next_hop, uint8_t hops, bool is_gw, float rssi, float snr) {
  uint32_t now = millis();
  RouteEntry *r = this->find_route_(dst_id);
  bool changed = false;
  bool gateway_changed = false;
  if (r == nullptr) {
    r = this->alloc_route_slot_();
    if (r == nullptr) {
      return;
    }
    *r = RouteEntry{};
    r->dst_id = dst_id;
    changed = true;
  }
  if (!r->is_valid || hops < r->hop_count || (hops == r->hop_count && rssi > r->rssi) || r->next_hop_id != next_hop) {
    r->next_hop_id = next_hop;
    r->hop_count = hops;
    r->rssi = rssi;
    r->snr = snr;
    changed = true;
  }
  // A node's gateway status can change independently of its path metric (e.g. it
  // becomes a gateway after we first learned it as a plain neighbour). Refresh the
  // flag on every confirmation, otherwise a stale is_gateway=false hides a gateway
  // that re-advertises at equal quality and send_to_gateway() finds no route.
  if (r->is_gateway != is_gw) {
    r->is_gateway = is_gw;
    r->gateway_update_pending = true;
    changed = true;
    gateway_changed = true;
  }
  r->is_valid = true;
  r->last_seen = now;
  r->expires_at = now + this->route_ttl_ms_;
  if (changed) {
    this->notify_route_changed_();
  }
  if (gateway_changed) {
    this->schedule_hello_update_();
  }
}

void LoraMesh::expire_routes_() {
  uint32_t now = millis();
  bool any = false;
  for (auto &r : this->routes_) {
    if (r.is_valid && static_cast<int32_t>(r.expires_at - now) <= 0) {
      ESP_LOGD(TAG, "Route to 0x%08" PRIX32 " expired", r.dst_id);
      r.is_valid = false;
      any = true;
      // Passive self-healing (ADR 0002): a direct neighbour going silent
      // invalidates every Route using it as Next Hop, so those destinations are
      // re-learned from other neighbours' HELLOs instead of black-holing.
      if (r.dst_id == r.next_hop_id) {
        this->invalidate_routes_via_(r.dst_id);
      }
    }
  }
  if (any) {
    this->notify_route_changed_();
  }
}

void LoraMesh::invalidate_routes_via_(uint32_t neighbor_id) {
  for (auto &r : this->routes_) {
    if (r.is_valid && r.next_hop_id == neighbor_id) {
      ESP_LOGD(TAG, "Route to 0x%08" PRIX32 " invalidated (next hop 0x%08" PRIX32 " lost)", r.dst_id, neighbor_id);
      r.is_valid = false;
    }
  }
}

void LoraMesh::notify_route_changed_() {
  this->route_update_callback_();
  this->publish_diagnostics_();
}

// ─── Node name cache ──────────────────────────────────────────────────────────

void LoraMesh::store_node_name_(uint32_t id, const char *name, uint8_t name_len) {
  size_t len = std::min(static_cast<size_t>(name_len), MESH_NODE_NAME_MAX_LEN);

  // Update the existing entry for this id if already known.
  for (auto &e : this->name_map_) {
    if (e.is_valid && e.id == id) {
      snprintf(e.name, sizeof(e.name), "%.*s", static_cast<int>(len), name);
      return;
    }
  }

  // Find an unused slot.
  for (auto &e : this->name_map_) {
    if (!e.is_valid) {
      e.id = id;
      e.is_valid = true;
      snprintf(e.name, sizeof(e.name), "%.*s", static_cast<int>(len), name);
      return;
    }
  }

  // Map is full: evict a stale entry whose node is no longer in the routing table.
  for (auto &e : this->name_map_) {
    if (this->find_route_(e.id) == nullptr) {
      e.id = id;
      snprintf(e.name, sizeof(e.name), "%.*s", static_cast<int>(len), name);
      // is_valid already true
      return;
    }
  }

  ESP_LOGD(TAG, "Name map full, cannot store name for 0x%08" PRIX32, id);
}

const char *LoraMesh::lookup_node_name_(uint32_t id) const {
  for (const auto &e : this->name_map_) {
    if (e.is_valid && e.id == id) {
      return e.name;
    }
  }
  return nullptr;
}

const char *LoraMesh::get_node_name(uint32_t id) const { return this->lookup_node_name_(id); }

// ─── Duplicate suppression ───────────────────────────────────────────────────

bool LoraMesh::is_duplicate_(uint32_t src_id, uint32_t frame_counter) {
  uint32_t now = millis();
  for (const auto &s : this->seen_cache_) {
    if (s.src_id == src_id && s.frame_counter == frame_counter && static_cast<int32_t>(s.expires_at - now) > 0) {
      return true;
    }
  }
  return false;
}

void LoraMesh::mark_seen_(uint32_t src_id, uint32_t frame_counter) {
  SeenEntry &slot = this->seen_cache_[this->seen_cache_head_];
  slot.src_id = src_id;
  slot.frame_counter = frame_counter;
  slot.expires_at = millis() + this->seen_cache_ttl_ms_;
  this->seen_cache_head_ = (this->seen_cache_head_ + 1) % this->seen_cache_.size();
}

void LoraMesh::expire_seen_() {
  uint32_t now = millis();
  for (auto &s : this->seen_cache_) {
    if (s.src_id != 0 && static_cast<int32_t>(s.expires_at - now) < 0) {
      s.src_id = 0;
      s.frame_counter = 0;
    }
  }
}

// ─── Diagnostics ──────────────────────────────────────────────────────────────

void LoraMesh::publish_diagnostics_() {
#ifdef USE_SENSOR
  if (this->node_count_sensor_ != nullptr) {
    this->node_count_sensor_->publish_state(static_cast<float>(this->get_known_node_count()));
  }
#endif
#ifdef USE_BINARY_SENSOR
  if (this->gateway_available_sensor_ != nullptr) {
    this->gateway_available_sensor_->publish_state(this->has_gateway() || this->upstream_connected_);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->routing_table_sensor_ != nullptr) {
    this->routing_table_sensor_->publish_state(this->get_routing_table_json());
  }
  if (this->nearest_gateway_sensor_ != nullptr) {
    this->nearest_gateway_sensor_->publish_state(this->get_nearest_gateway());
  }
#endif
}

// ─── Frame counter persistence ────────────────────────────────────────────────

uint32_t LoraMesh::next_frame_counter_() {
  ++this->frame_counter_;
  // Persist to NVS when we exceed the batch threshold.
  if (this->frame_counter_ >= this->frame_counter_persist_threshold_) {
    this->persist_frame_counter_();
  }
  return this->frame_counter_;
}

void LoraMesh::persist_frame_counter_() {
  // Write ahead by FRAME_COUNTER_BATCH so on next boot we resume from a safe value.
  uint32_t persisted_val = this->frame_counter_ + FRAME_COUNTER_BATCH;
  this->frame_counter_pref_.save(&persisted_val);
  this->frame_counter_persist_threshold_ = persisted_val;
  ESP_LOGD(TAG, "Frame counter persisted (next_boot=%" PRIu32 ")", persisted_val);
}

void LoraMesh::load_frame_counter_() {
  uint32_t hash = fnv1a_str(std::string(NVS_PREFIX_FRAME_COUNTER) + this->node_id_str_);
  this->frame_counter_pref_ = global_preferences->make_preference<uint32_t>(hash, true);

  uint32_t stored = 0;
  if (this->frame_counter_pref_.load(&stored) && stored > 0) {
    // Resume from the persisted-ahead value (guarantees no nonce reuse).
    this->frame_counter_ = stored;
    ESP_LOGI(TAG, "Frame counter loaded from NVS: %" PRIu32, stored);
  } else {
    this->frame_counter_ = 0;
  }
  // Set threshold so next persist happens after FRAME_COUNTER_BATCH more frames.
  this->frame_counter_persist_threshold_ = this->frame_counter_ + FRAME_COUNTER_BATCH;
  // Persist immediately so if we crash before the first batch completes,
  // next boot still has a safe-ahead value.
  this->persist_frame_counter_();
}

// ─── Replay protection ────────────────────────────────────────────────────────

bool LoraMesh::is_replay_(uint32_t src_id, uint32_t frame_counter) {
  for (const auto &entry : this->replay_counters_) {
    if (entry.is_valid && entry.src_id == src_id) {
      return frame_counter <= entry.high_water;
    }
  }
  return false;  // Unknown source — not a replay.
}

void LoraMesh::update_replay_counter_(uint32_t src_id, uint32_t frame_counter) {
  // Update existing entry.
  for (auto &entry : this->replay_counters_) {
    if (entry.is_valid && entry.src_id == src_id) {
      if (frame_counter > entry.high_water) {
        entry.high_water = frame_counter;
      }
      return;
    }
  }
  // Allocate new slot.
  for (auto &entry : this->replay_counters_) {
    if (!entry.is_valid) {
      entry.src_id = src_id;
      entry.high_water = frame_counter;
      entry.is_valid = true;
      return;
    }
  }
  // Table full: evict oldest (lowest high_water).
  ReplayEntry *oldest = &this->replay_counters_[0];
  for (auto &entry : this->replay_counters_) {
    if (entry.high_water < oldest->high_water) {
      oldest = &entry;
    }
  }
  oldest->src_id = src_id;
  oldest->high_water = frame_counter;
}

}  // namespace esphome::lora_mesh
