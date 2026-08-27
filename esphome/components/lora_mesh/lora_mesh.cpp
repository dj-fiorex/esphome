#include "lora_mesh.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <limits>

namespace esphome::lora_mesh {

static const char *const TAG = "lora_mesh";

// NVS key prefixes for preference hashes.
static const char *const NVS_PREFIX_FRAME_COUNTER = "lora_mesh_fc_";

// ─── Utility ──────────────────────────────────────────────────────────────────

void LoraMesh::id_to_hex(uint32_t id, char out[9]) { snprintf(out, 9, "%08" PRIX32, id); }

LoraMesh::LoraMesh(const std::string &fabric_key_hex, LoRaRadio *radio)
    : packet_admission_(this->fabric_id_, this->fabric_key_, this->control_plane_key_),
      radio_(radio),
      outbound_airtime_(radio, this, LoraMesh::refresh_queued_hello_, LoraMesh::queued_hello_attempted_) {
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
    char buf[7];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", mac[3], mac[4], mac[5]);
    this->node_id_str_ = buf;
    this->node_id_ = fnv1a_str(this->node_id_str_);
  }
  // Initialise arrays.
  this->routing_table_.clear();
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

  // Expire Routes before building this interval's HELLO. Advertising a Route
  // in the same loop iteration that invalidates it gives neighbours one final
  // stale lease and defeats bounded withdrawal.
  if (now - this->last_expire_check_ >= RouteTable::EXPIRY_SCAN_INTERVAL_MS) {
    this->last_expire_check_ = now;
    if (this->routing_table_.expire(now)) {
      this->notify_route_changed_();
    }
    this->expire_seen_();
  }

  // Periodic and transition-triggered HELLOs share one coalesced, rate-limited path.
  if (now - this->last_hello_ >= this->discovery_interval_ms_) {
    this->hello_update_pending_ = true;
  }
  this->queue_pending_hello_(now);

  // Diagnostic sensor publishing (every 30 s).
  if (now - this->last_diag_publish_ >= 30000) {
    this->last_diag_publish_ = now;
    this->publish_diagnostics_();
  }

  // Transmit at most one queued packet per loop iteration.
  this->outbound_airtime_.drain(now);
}

void LoraMesh::dump_config() {
  ESP_LOGCONFIG(TAG, "LoraMesh:");
  ESP_LOGCONFIG(TAG, "  Node ID: %s (0x%08" PRIX32 ")", this->node_id_str_.c_str(), this->node_id_);
  ESP_LOGCONFIG(TAG, "  Upstream connectivity: %s", this->upstream_connected_ ? "connected" : "disconnected");
  ESP_LOGCONFIG(TAG, "  Max hops: %u", this->max_hops_);
  ESP_LOGCONFIG(TAG, "  Discovery interval: %" PRIu32 " ms", this->discovery_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Route TTL: %" PRIu32 " ms", this->routing_table_.get_route_ttl());
  ESP_LOGCONFIG(TAG, "  Forward messages: %s", this->forward_messages_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  Max routes: %zu", this->routing_table_.entries().size());
  ESP_LOGCONFIG(TAG, "  Seen cache size: %zu", this->seen_cache_.size());
  ESP_LOGCONFIG(TAG, "  TX queue size: %zu", this->outbound_airtime_.capacity());
  ESP_LOGCONFIG(TAG, "  TX jitter: %" PRIu32 " ms", this->outbound_airtime_.get_jitter());
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool LoraMesh::send_message(const std::string &destination, std::span<const uint8_t> payload) {
  uint32_t dst_id = fnv1a_str(destination);
  const RouteEntry *route = this->routing_table_.find(dst_id);
  if (route == nullptr) {
    ESP_LOGW(TAG, "No route to %s", destination.c_str());
    return false;
  }
  auto pkt = this->build_data_packet_(dst_id, route->next_hop_id, payload);
  if (pkt.empty()) {
    return false;
  }
  if (!this->outbound_airtime_.enqueue(pkt)) {
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
  if (!this->outbound_airtime_.enqueue(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA broadcast queued (%zu bytes)", payload.size());
  return true;
}

bool LoraMesh::send_to_gateway(std::span<const uint8_t> payload) {
  const RouteEntry *gw = this->routing_table_.nearest_gateway();
  if (gw == nullptr) {
    ESP_LOGW(TAG, "send_to_gateway: no gateway in routing table");
    return false;
  }
  auto pkt = this->build_data_packet_(gw->dst_id, gw->next_hop_id, payload);
  if (pkt.empty()) {
    return false;
  }
  if (!this->outbound_airtime_.enqueue(pkt)) {
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

bool LoraMesh::has_route(const std::string &node_id) const {
  return this->routing_table_.find(fnv1a_str(node_id)) != nullptr;
}

bool LoraMesh::has_gateway() const { return this->routing_table_.nearest_gateway() != nullptr; }

std::string LoraMesh::get_nearest_gateway() const {
  const RouteEntry *gw = this->routing_table_.nearest_gateway();
  if (gw == nullptr) {
    return {};
  }
  char buf[9];
  LoraMesh::id_to_hex(gw->dst_id, buf);
  return buf;
}

void LoraMesh::clear_routes() {
  this->routing_table_.clear();
  this->notify_route_changed_();
  ESP_LOGI(TAG, "Routing table cleared");
}

size_t LoraMesh::get_known_node_count() const { return this->routing_table_.count(); }

std::string LoraMesh::get_routing_table_json() const {
  // Pre-reserve: each entry is ~90 bytes; +2 for '[' and ']'.
  size_t entry_count = this->get_known_node_count();
  std::string out;
  // Pre-reserve: '[' + ']' + entry_count * ~130 bytes per entry
  // (~96 base JSON + up to 32 chars for "name" field + commas and overhead).
  out.reserve(2 + entry_count * 130);
  out = '[';
  bool first = true;
  for (const auto &r : this->routing_table_.entries()) {
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
  const std::span<const uint8_t> packet(pkt, pkt_len);
  PacketInspectionResult inspection = this->packet_admission_.inspect(packet);
  if (!inspection.accepted()) {
    this->log_admission_failure_(inspection.failure, inspection.header, pkt_len);
    return;
  }
  const PacketHeader &header = inspection.header;

#ifdef LORA_MESH_LINK_SIM
  // Link simulator: pretend this packet was never received because its
  // immediate sender is "out of range". Dropped before duplicate/seen handling
  // so the blocked neighbour leaves no trace at all. See ADR-0004.
  if (this->is_link_blocked_(header.prev_hop)) {
    ESP_LOGD(TAG, "link-sim: dropped packet from prev_hop 0x%08" PRIX32 " (blocked)", header.prev_hop);
    return;
  }
#endif

  // Ignore our own transmissions echoed back.
  if (header.src_id == this->node_id_) {
    return;
  }

  PacketAdmissionResult admission = this->packet_admission_.authenticate(packet, header);
  if (!admission.accepted()) {
    this->log_admission_failure_(admission.failure, header, pkt_len);
    return;
  }

  // Duplicate suppression.
  if (this->is_duplicate_(header.src_id, header.frame_counter)) {
    ESP_LOGV(TAG, "Duplicate from 0x%08" PRIX32 " frame=%" PRIu32 ", dropped", header.src_id, header.frame_counter);
    return;
  }
  this->mark_seen_(header.src_id, header.frame_counter);

  switch (header.packet_type) {
    case PacketType::HELLO:
      this->process_hello_(header, packet, rssi, snr);
      break;
    case PacketType::DATA:
      this->process_data_(header, packet, admission.plaintext_view(), rssi, snr);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(header.packet_type),
               header.src_id);
      break;
  }
}

void LoraMesh::log_admission_failure_(AdmissionFailure failure, const PacketHeader &header, size_t packet_size) const {
  switch (failure) {
    case AdmissionFailure::PACKET_TOO_SHORT:
      ESP_LOGW(TAG, "Packet too short (%zu bytes), dropped", packet_size);
      break;
    case AdmissionFailure::FABRIC_MISMATCH:
      ESP_LOGV(TAG, "Fabric ID mismatch (0x%08" PRIX32 " vs 0x%08" PRIX32 "), dropped", header.fabric_id,
               this->fabric_id_);
      break;
    case AdmissionFailure::UNSUPPORTED_TYPE:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(header.packet_type),
               header.src_id);
      break;
    case AdmissionFailure::INVALID_HELLO:
      ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " failed validation or authentication", header.src_id);
      break;
    case AdmissionFailure::DATA_AUTHENTICATION_FAILED:
      ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " authentication failed, dropped", header.src_id);
      break;
    case AdmissionFailure::INVALID_DATA_ENVELOPE:
    case AdmissionFailure::NONE:
      break;
  }
}

// ─── HELLO processing ─────────────────────────────────────────────────────────

void LoraMesh::process_hello_(const PacketHeader &header, std::span<const uint8_t> packet, float rssi, float snr) {
  constexpr size_t offset = MESH_HEADER_SIZE;
  uint32_t now = millis();
  RouteUpdate neighbor_update =
      this->routing_table_.observe_neighbor(header.src_id, (header.flags & FLAG_IS_GATEWAY) != 0, rssi, snr, now);
  if (neighbor_update.gateway_changed) {
    this->schedule_hello_update_();
  }
  if (neighbor_update.changed) {
    this->notify_route_changed_();
  }

  uint8_t name_len = packet[offset + 1];

  if (name_len > 0) {
    this->store_node_name_(header.src_id, reinterpret_cast<const char *>(packet.data() + offset + 2), name_len);
  }

  uint8_t route_count = packet[offset + 2 + name_len];
  size_t pos = offset + 3 + name_len;
  size_t authenticated_size = packet.size() - HELLO_AUTH_TAG_SIZE;

  for (uint8_t i = 0; i < route_count && pos + ROUTE_ADV_SIZE <= authenticated_size; ++i, pos += ROUTE_ADV_SIZE) {
    uint32_t adv_dst = get_u32_le(packet.data() + pos + ROUTE_ADV_OFF_DEST_ID);
    uint8_t adv_hops = packet[pos + ROUTE_ADV_OFF_HOP_COUNT];
    float advertised_rssi = static_cast<float>(static_cast<int8_t>(packet[pos + ROUTE_ADV_OFF_PATH_RSSI]));
    bool advertised_gateway = (packet[pos + ROUTE_ADV_OFF_FLAGS] & ROUTE_FLAG_IS_GATEWAY) != 0;

    if (adv_dst == this->node_id_ || adv_dst == header.src_id) {
      continue;
    }
    uint16_t new_hops = static_cast<uint16_t>(adv_hops) + 1;
    if (new_hops > this->max_hops_) {
      continue;
    }
    float path_rssi = std::min(advertised_rssi, rssi);
    RouteUpdate route_update = this->routing_table_.consider(
        {adv_dst, header.src_id, static_cast<uint8_t>(new_hops), advertised_gateway, path_rssi, snr}, now);
    if (route_update.gateway_changed) {
      this->schedule_hello_update_();
    }
    if (route_update.changed) {
      this->notify_route_changed_();
    }
  }

  const char *src_name = this->lookup_node_name_(header.src_id);
  ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " name=%s rssi=%.0f snr=%.1f gw=%s routes=%u", header.src_id,
           src_name != nullptr ? src_name : "?", static_cast<double>(rssi), static_cast<double>(snr),
           (header.flags & FLAG_IS_GATEWAY) != 0 ? "yes" : "no", route_count);
}

// ─── DATA processing ──────────────────────────────────────────────────────────

void LoraMesh::process_data_(const PacketHeader &header, std::span<const uint8_t> packet,
                             std::span<const uint8_t> plaintext, float rssi, float snr) {
  bool is_broadcast = header.dst_id == MESH_BROADCAST_ID;
  bool is_for_us = is_broadcast || (header.dst_id == this->node_id_);

  if (is_for_us) {
    ReplayDecision replay_decision = this->admit_replay_counter_(header.src_id, header.frame_counter);
    if (replay_decision == ReplayDecision::REPLAY) {
      ESP_LOGW(TAG, "DATA from 0x%08" PRIX32 " frame=%" PRIu32 " replay rejected", header.src_id, header.frame_counter);
      return;
    }
    if (replay_decision == ReplayDecision::TABLE_FULL) {
      ESP_LOGW(TAG, "Replay table full; DATA from unknown source 0x%08" PRIX32 " rejected fail-safe", header.src_id);
      return;
    }

    MeshMessage msg;
    LoraMesh::id_to_hex(header.src_id, msg.source);
    const char *src_name = this->lookup_node_name_(header.src_id);
    if (src_name != nullptr) {
      snprintf(msg.source_name, sizeof(msg.source_name), "%s", src_name);
    }
    LoraMesh::id_to_hex(header.dst_id, msg.destination);
    LoraMesh::id_to_hex(header.prev_hop, msg.prev_hop);
    msg.payload.assign(plaintext.begin(), plaintext.end());
    msg.frame_counter = header.frame_counter;
    msg.hop_count = header.hop_count;
    msg.ttl = header.ttl;
    msg.rssi = rssi;
    msg.snr = snr;
    msg.is_broadcast = is_broadcast;
    msg.is_for_this_node = !is_broadcast;

    ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " hops=%u len=%zu", header.src_id, header.hop_count, plaintext.size());
    this->message_callback_(msg);
  }

  // Forward if TTL allows, forwarding is enabled, and packet is not unicast-only-to-us.
  if (!this->forward_messages_ || header.ttl <= 1 || (is_for_us && !is_broadcast)) {
    return;
  }

  // Single-path unicast (ADR 0002): only the designated next hop forwards.
  const RouteEntry *route = nullptr;
  if (!is_broadcast) {
    if (header.next_hop != this->node_id_) {
      return;
    }
    route = this->routing_table_.find(header.dst_id);
    if (route == nullptr) {
      ESP_LOGW(TAG, "No route to forward 0x%08" PRIX32, header.dst_id);
      return;
    }
    if (route->next_hop_id == header.prev_hop) {
      ESP_LOGW(TAG, "Refusing to forward 0x%08" PRIX32 " back to previous hop 0x%08" PRIX32, header.dst_id,
               header.prev_hop);
      return;
    }
  }

  // Packet forwarding: copy the incoming packet into a stack-allocated Packet,
  // then patch the TTL, hop_count, prev_hop and next_hop fields before retransmitting.
  // The authenticated immutable header fields and encrypted body are Forwarded verbatim.
  Packet fwd(packet.begin(), packet.end());
  PacketHeader forwarded_header = header;
  forwarded_header.ttl--;
  forwarded_header.hop_count++;
  forwarded_header.prev_hop = this->node_id_;

  if (is_broadcast) {
    forwarded_header.next_hop = MESH_BROADCAST_ID;
    write_mutable_packet_header_fields(fwd.data(), forwarded_header);
    this->outbound_airtime_.enqueue(fwd);
    ESP_LOGD(TAG, "Forward broadcast queued ttl=%u", forwarded_header.ttl);
    return;
  }

  forwarded_header.next_hop = route->next_hop_id;
  write_mutable_packet_header_fields(fwd.data(), forwarded_header);
  this->outbound_airtime_.enqueue(fwd);
  ESP_LOGD(TAG, "Forward unicast queued to 0x%08" PRIX32 " via 0x%08" PRIX32 " ttl=%u", header.dst_id,
           route->next_hop_id, forwarded_header.ttl);
}

// ─── Packet builders ──────────────────────────────────────────────────────────

Packet LoraMesh::build_hello_packet_() {
  uint8_t flags = this->upstream_connected_ ? FLAG_IS_GATEWAY : 0;

  // Collect valid routes. Gateway state changes take bounded priority so an
  // immediate HELLO cannot omit the promotion or Withdrawal when every Route
  // does not fit; remaining changes stay pending for a later rate-limited HELLO.
  static_assert(LORA_MESH_MAX_ROUTES <= 255, "LORA_MESH_MAX_ROUTES must be <= 255");
  std::array<const RouteEntry *, LORA_MESH_MAX_ROUTES> ptrs{};
  size_t count = 0;
  for (const auto &r : this->routing_table_.entries()) {
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
  uint32_t frame_counter;
  if (!this->next_frame_counter_(frame_counter)) {
    return {};
  }
  const PacketHeader header{
      .fabric_id = this->fabric_id_,
      .packet_type = PacketType::HELLO,
      .flags = flags,
      .src_id = this->node_id_,
      .dst_id = MESH_BROADCAST_ID,
      .frame_counter = frame_counter,
      .ttl = this->max_hops_,
      .hop_count = 0,
      .prev_hop = this->node_id_,
      .next_hop = MESH_BROADCAST_ID,
  };
  auto pkt = serialize_packet_header(header);
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
  uint32_t frame_counter;
  if (!this->next_frame_counter_(frame_counter)) {
    return {};
  }
  const PacketHeader header{
      .fabric_id = this->fabric_id_,
      .packet_type = PacketType::DATA,
      .flags = flags,
      .src_id = this->node_id_,
      .dst_id = dst_id,
      .frame_counter = frame_counter,
      .ttl = this->max_hops_,
      .hop_count = 0,
      .prev_hop = this->node_id_,
      .next_hop = next_hop,
  };
  auto pkt = serialize_packet_header(header);
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
  bool encrypted = mesh_encrypt_payload(this->fabric_key_.data(), header.src_id, header.dst_id, header.frame_counter,
                                        static_cast<uint8_t>(header.packet_type), header.flags,
                                        static_cast<uint8_t>(len), payload.data(), ciphertext, tag);
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

void LoraMesh::schedule_hello_update_() {
  this->hello_update_pending_ = true;
  this->outbound_airtime_.invalidate_queued_hello();
}

void LoraMesh::queue_pending_hello_(uint32_t now) {
  if (!this->hello_update_pending_ || this->outbound_airtime_.has_queued_hello() ||
      now - this->last_hello_ < this->HELLO_UPDATE_MIN_INTERVAL_MS) {
    return;
  }
  auto pkt = this->build_hello_packet_();
  if (pkt.empty()) {
    return;
  }
  if (this->outbound_airtime_.enqueue(pkt, OutboundPacketKind::HELLO)) {
    this->last_hello_ = now;
    ESP_LOGD(TAG, "HELLO queued (%zu bytes)", pkt.size());
  }
}

void LoraMesh::acknowledge_advertised_gateway_updates_(const Packet &hello) {
  size_t route_count_offset = MESH_HEADER_SIZE + 2 + hello[MESH_HEADER_SIZE + 1];
  uint8_t route_count = hello[route_count_offset];
  size_t route_offset = route_count_offset + 1;
  for (uint8_t index = 0; index < route_count; ++index, route_offset += ROUTE_ADV_SIZE) {
    this->routing_table_.acknowledge_gateway_update(get_u32_le(&hello[route_offset + ROUTE_ADV_OFF_DEST_ID]));
  }
}

bool LoraMesh::has_pending_gateway_updates_() const { return this->routing_table_.has_pending_gateway_updates(); }

Packet LoraMesh::refresh_queued_hello_(void *context) {
  return static_cast<LoraMesh *>(context)->build_hello_packet_();
}

void LoraMesh::queued_hello_attempted_(void *context, const Packet &hello) {
  auto *mesh = static_cast<LoraMesh *>(context);
  if (!hello.empty()) {
    mesh->acknowledge_advertised_gateway_updates_(hello);
  }
  mesh->hello_update_pending_ = mesh->has_pending_gateway_updates_();
}

void LoraMesh::notify_route_changed_() {
  this->outbound_airtime_.invalidate_queued_hello();
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
    if (this->routing_table_.find(e.id) == nullptr) {
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

bool LoraMesh::next_frame_counter_(uint32_t &frame_counter) {
  if (this->frame_counter_ == std::numeric_limits<uint32_t>::max()) {
    ESP_LOGE(TAG, "Frame counter exhausted; refusing to originate another packet to prevent nonce reuse");
    return false;
  }
  ++this->frame_counter_;
  // Persist to NVS when we exceed the batch threshold.
  if (this->frame_counter_ >= this->frame_counter_persist_threshold_) {
    this->persist_frame_counter_();
  }
  frame_counter = this->frame_counter_;
  return true;
}

void LoraMesh::persist_frame_counter_() {
  // Write ahead by FRAME_COUNTER_BATCH so on next boot we resume from a safe value.
  uint32_t remaining = std::numeric_limits<uint32_t>::max() - this->frame_counter_;
  uint32_t persisted_val = this->frame_counter_ + std::min(FRAME_COUNTER_BATCH, remaining);
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
  // Persist immediately so if we crash before the first batch completes,
  // next boot still has a safe-ahead value. This also sets the next threshold.
  this->persist_frame_counter_();
}

// ─── Replay protection ────────────────────────────────────────────────────────

LoraMesh::ReplayDecision LoraMesh::admit_replay_counter_(uint32_t src_id, uint32_t frame_counter) {
  ReplayEntry *unused = nullptr;
  for (auto &entry : this->replay_counters_) {
    if (entry.is_valid && entry.src_id == src_id) {
      if (frame_counter <= entry.high_water) {
        return ReplayDecision::REPLAY;
      }
      entry.high_water = frame_counter;
      return ReplayDecision::ACCEPT;
    }
    if (!entry.is_valid && unused == nullptr) {
      unused = &entry;
    }
  }
  if (unused == nullptr) {
    return ReplayDecision::TABLE_FULL;
  }
  unused->src_id = src_id;
  unused->high_water = frame_counter;
  unused->is_valid = true;
  return ReplayDecision::ACCEPT;
}

}  // namespace esphome::lora_mesh
