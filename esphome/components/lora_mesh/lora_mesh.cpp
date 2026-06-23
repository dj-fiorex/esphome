#include "lora_mesh.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace esphome::lora_mesh {

static const char *const TAG = "lora_mesh";

// ─── Utility ──────────────────────────────────────────────────────────────────

void LoraMesh::id_to_hex(uint32_t id, char out[9]) { snprintf(out, 9, "%08" PRIX32, id); }

// ─── Component lifecycle ──────────────────────────────────────────────────────

void LoraMesh::setup() {
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
  this->fabric_id_ = fnv1a_str(this->mesh_secret_);

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

  // Load persisted group key and frame counter from NVS.
  this->load_group_key_();
  this->load_frame_counter_();

  if (this->radio_ == nullptr) {
    ESP_LOGE(TAG, "No radio configured — marking failed");
    this->mark_failed();
    return;
  }

  // Register as listener on the radio adapter.
  this->radio_->attach_listener(this);

  // Determine initial gateway state.
  this->acting_as_gateway_ = this->compute_gateway_state_();
  this->last_gateway_available_ = this->has_gateway();

  // Stagger the first HELLO by a random offset so devices booted simultaneously
  // do not collide on the channel.
  uint32_t jitter_ms = static_cast<uint32_t>(random_uint32() % (this->discovery_interval_ms_ / 5));
  this->last_hello_ = millis() - this->discovery_interval_ms_ + jitter_ms;

  ESP_LOGI(TAG, "LoraMesh setup: node_id=%s (0x%08" PRIX32 ") fabric_id=0x%08" PRIX32 " gw=%s provisioned=%s",
           this->node_id_str_.c_str(), this->node_id_, this->fabric_id_, this->acting_as_gateway_ ? "yes" : "no",
           this->has_group_key_ ? "yes" : "no");
}

void LoraMesh::loop() {
  uint32_t now = millis();

  // Update gateway state for AUTO mode.
  this->update_gateway_state_();

  // Periodic HELLO beacon.
  if (now - this->last_hello_ >= this->discovery_interval_ms_) {
    this->last_hello_ = now;
    auto pkt = this->build_hello_packet_();
    this->enqueue_tx_(pkt);
    ESP_LOGD(TAG, "HELLO queued (%zu bytes)", pkt.size());
  }

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
  ESP_LOGCONFIG(TAG, "  Gateway mode: %s",
                this->gateway_mode_ == GatewayMode::NORMAL    ? "normal"
                : this->gateway_mode_ == GatewayMode::GATEWAY ? "gateway"
                                                              : "auto");
  ESP_LOGCONFIG(TAG, "  Acting as gateway: %s", this->acting_as_gateway_ ? "yes" : "no");
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

bool LoraMesh::send_message(const std::string &destination, const std::string &payload) {
  uint32_t dst_id = fnv1a_str(destination);
  const RouteEntry *route = this->find_route_(dst_id);
  if (route == nullptr) {
    ESP_LOGW(TAG, "No route to %s", destination.c_str());
    return false;
  }
  auto pkt = this->build_data_packet_(dst_id, route->next_hop_id, payload);
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA queued to %s (%zu bytes)", destination.c_str(), payload.size());
  return true;
}

bool LoraMesh::broadcast_message(const std::string &payload) {
  auto pkt = this->build_data_packet_(MESH_BROADCAST_ID, MESH_BROADCAST_ID, payload);
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA broadcast queued (%zu bytes)", payload.size());
  return true;
}

bool LoraMesh::send_to_gateway(const std::string &payload) {
  const RouteEntry *gw = this->find_best_gateway_route_();
  if (gw == nullptr) {
    ESP_LOGW(TAG, "send_to_gateway: no gateway in routing table");
    return false;
  }
  auto pkt = this->build_data_packet_(gw->dst_id, gw->next_hop_id, payload);
  if (!this->enqueue_tx_(pkt)) {
    return false;
  }
  ESP_LOGD(TAG, "DATA queued to gateway 0x%08" PRIX32 " (%zu bytes)", gw->dst_id, payload.size());
  return true;
}

bool LoraMesh::has_route(const std::string &node_id) const { return this->find_route_(fnv1a_str(node_id)) != nullptr; }

bool LoraMesh::has_gateway() const { return this->find_best_gateway_route_() != nullptr; }

std::string LoraMesh::get_best_gateway() const {
  const RouteEntry *gw = this->find_best_gateway_route_();
  if (gw == nullptr) {
    return {};
  }
  char buf[9];
  id_to_hex(gw->dst_id, buf);
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
    id_to_hex(r.dst_id, dst_hex);
    id_to_hex(r.next_hop_id, nh_hex);
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
      id_to_hex(this->blocked_neighbors_[i], hex);
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

  // Fabric ID filter (relay gate).
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
      this->process_data_(pkt, pkt_len, MESH_HEADER_SIZE, src_id, dst_id, frame_counter, ttl, hop_count, prev_hop,
                          next_hop, flags, rssi, snr);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(pkt_type), src_id);
      break;
  }
}

// ─── HELLO processing ─────────────────────────────────────────────────────────

void LoraMesh::process_hello_(const uint8_t *pkt, size_t pkt_len, size_t offset, uint32_t src_id, bool src_is_gateway,
                              uint32_t prev_hop, float rssi, float snr) {
  // Always create/update a direct route to the sender.
  this->update_route_(src_id, src_id, 1, src_is_gateway, rssi, snr);

  if (pkt_len < offset + HELLO_FIXED_SIZE) {
    return;
  }

  // Reject packets from nodes running a different protocol version.
  if (pkt[offset] != MESH_PROTO_VERSION) {
    ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 ": proto version %u (expected %u), skipping body", src_id, pkt[offset],
             MESH_PROTO_VERSION);
    return;
  }

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

  for (uint8_t i = 0; i < route_count && pos + ROUTE_ADV_SIZE <= pkt_len; ++i, pos += ROUTE_ADV_SIZE) {
    uint32_t adv_dst = get_u32_le(pkt + pos);
    uint8_t adv_hops = pkt[pos + 4];

    if (adv_dst == this->node_id_ || adv_dst == src_id) {
      continue;
    }
    uint8_t new_hops = static_cast<uint8_t>(adv_hops + 1);
    if (new_hops > this->max_hops_) {
      continue;
    }
    const RouteEntry *existing = this->find_route_(adv_dst);
    // Accept a strictly better path from anyone; renew the lease when our
    // current next hop re-confirms the route at equal quality (ADR 0002) —
    // otherwise stable multi-hop routes expire and flap every route_ttl.
    bool better = existing == nullptr || new_hops < existing->hop_count;
    bool reconfirmed = existing != nullptr && existing->next_hop_id == src_id && new_hops == existing->hop_count;
    if (better || reconfirmed) {
      this->update_route_(adv_dst, src_id, new_hops, false, rssi, snr);
    }
  }

  const char *src_name = this->lookup_node_name_(src_id);
  ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " name=%s rssi=%.0f snr=%.1f gw=%s routes=%u", src_id,
           src_name != nullptr ? src_name : "?", static_cast<double>(rssi), static_cast<double>(snr),
           src_is_gateway ? "yes" : "no", route_count);
}

// ─── DATA processing ──────────────────────────────────────────────────────────

void LoraMesh::process_data_(const uint8_t *pkt, size_t pkt_len, size_t offset, uint32_t src_id, uint32_t dst_id,
                             uint32_t frame_counter, uint8_t ttl, uint8_t hop_count, uint32_t prev_hop,
                             uint32_t next_hop, uint8_t flags, float rssi, float snr) {
  bool is_broadcast = (dst_id == MESH_BROADCAST_ID) || ((flags & FLAG_IS_BROADCAST) != 0);
  bool is_for_us = is_broadcast || (dst_id == this->node_id_);

  if (is_for_us) {
    // Only attempt decryption if we hold a group key (provisioned).
    if (!this->has_group_key_) {
      ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " dropped (unprovisioned)", src_id);
      // Fall through to forwarding below (unprovisioned relays still forward).
    } else {
      if (pkt_len < offset + 1) {
        return;
      }
      uint8_t payload_len = pkt[offset];
      size_t payload_start = offset + 1;
      // Encrypted DATA body: payload_len bytes of ciphertext + 4-byte MIC.
      if (pkt_len < payload_start + payload_len + MIC_SIZE) {
        return;
      }

      const uint8_t *ciphertext = pkt + payload_start;
      const uint8_t *mic = pkt + payload_start + payload_len;

      // Decrypt and verify MIC.
      uint8_t plaintext[226];  // max payload
      bool mic_ok = mesh_decrypt_payload(this->group_key_, src_id, dst_id, frame_counter,
                                         static_cast<uint8_t>(PacketType::DATA), payload_len, ciphertext, plaintext,
                                         mic);
      if (!mic_ok) {
        ESP_LOGD(TAG, "DATA from 0x%08" PRIX32 " MIC failed (not our group), dropped", src_id);
        // MIC fail → not our group. Still forward if applicable.
      } else {
        // MIC passed. Now check replay (verify MIC first, then reject replay).
        if (this->is_replay_(src_id, frame_counter)) {
          ESP_LOGW(TAG, "DATA from 0x%08" PRIX32 " frame=%" PRIu32 " replay rejected", src_id, frame_counter);
          // Replay → do not deliver, do not forward.
          return;
        }
        this->update_replay_counter_(src_id, frame_counter);

        MeshMessage msg;
        id_to_hex(src_id, msg.source);
        const char *src_name = this->lookup_node_name_(src_id);
        if (src_name != nullptr) {
          snprintf(msg.source_name, sizeof(msg.source_name), "%s", src_name);
        }
        id_to_hex(dst_id, msg.destination);
        id_to_hex(prev_hop, msg.prev_hop);
        msg.payload.assign(reinterpret_cast<const char *>(plaintext), payload_len);
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
    }
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
  // Header + encrypted body are forwarded verbatim (relay does not need the key).
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
  uint8_t flags = this->acting_as_gateway_ ? FLAG_IS_GATEWAY : 0;

  // Collect valid routes, sorted by hop_count ascending.
  static_assert(LORA_MESH_MAX_ROUTES <= 255, "LORA_MESH_MAX_ROUTES must be <= 255");
  std::array<const RouteEntry *, LORA_MESH_MAX_ROUTES> ptrs{};
  size_t count = 0;
  for (const auto &r : this->routes_) {
    if (r.is_valid && count < ptrs.size()) {
      ptrs[count++] = &r;
    }
  }
  // Sort valid route pointers by hop_count ascending to prioritise close neighbours.
  std::sort(ptrs.begin(), ptrs.begin() + static_cast<ptrdiff_t>(count),
            [](const RouteEntry *a, const RouteEntry *b) { return a->hop_count < b->hop_count; });

  size_t name_len = std::min(this->node_id_str_.size(), MESH_NODE_NAME_MAX_LEN);
  // Overhead = proto_version(1) + name_len_field(1) + name(name_len) + route_count(1).
  size_t hello_overhead = 3 + name_len;
  size_t max_pkt = (this->radio_ != nullptr) ? this->radio_->get_max_packet_size() : 255;
  size_t budget = (max_pkt > MESH_HEADER_SIZE + hello_overhead) ? max_pkt - MESH_HEADER_SIZE - hello_overhead : 0;
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
    put_u32_le(entry, r->dst_id);
    entry[4] = r->hop_count;
    int clamped = static_cast<int>(r->rssi);
    clamped = (clamped < -128) ? -128 : (clamped > 127) ? 127 : clamped;
    entry[5] = static_cast<uint8_t>(static_cast<int8_t>(clamped));
    for (uint8_t b : entry) {
      pkt.push_back(b);
    }
  }
  return pkt;
}

Packet LoraMesh::build_data_packet_(uint32_t dst_id, uint32_t next_hop, const std::string &payload) {
  uint8_t flags = this->acting_as_gateway_ ? FLAG_IS_GATEWAY : 0;
  if (dst_id == MESH_BROADCAST_ID) {
    flags |= FLAG_IS_BROADCAST;
  }
  uint32_t fc = this->next_frame_counter_();
  auto pkt = this->build_header_(PacketType::DATA, flags, dst_id, fc, this->max_hops_, 0,
                                 this->node_id_, next_hop);
  // Budget = radio frame limit minus header, payload_len byte, and MIC.
  size_t max_pkt = (this->radio_ != nullptr) ? this->radio_->get_max_packet_size() : LORA_MAX_PACKET_SIZE;
  max_pkt = std::min(max_pkt, LORA_MAX_PACKET_SIZE);
  size_t len = std::min(payload.size(), max_pkt - MESH_HEADER_SIZE - 1 - MIC_SIZE);
  pkt.push_back(static_cast<uint8_t>(len));

  if (this->has_group_key_) {
    // Encrypt payload and append ciphertext + MIC.
    uint8_t ciphertext[226];
    uint8_t mic[MIC_SIZE];
    mesh_encrypt_payload(this->group_key_, this->node_id_, dst_id, fc, static_cast<uint8_t>(PacketType::DATA),
                         static_cast<uint8_t>(len), reinterpret_cast<const uint8_t *>(payload.data()), ciphertext, mic);
    for (size_t i = 0; i < len; ++i) {
      pkt.push_back(ciphertext[i]);
    }
    for (size_t i = 0; i < MIC_SIZE; ++i) {
      pkt.push_back(mic[i]);
    }
  } else {
    // Unprovisioned: send plaintext (no security — for bootstrapping/debug only).
    for (size_t i = 0; i < len; ++i) {
      pkt.push_back(static_cast<uint8_t>(payload[i]));
    }
    // Still append a zero MIC so the wire format is consistent.
    for (size_t i = 0; i < MIC_SIZE; ++i) {
      pkt.push_back(0);
    }
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
  // de-syncs relays that all queued the same packet at the same instant).
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

RouteEntry *LoraMesh::find_best_gateway_route_() {
  RouteEntry *best = nullptr;
  for (auto &r : this->routes_) {
    if (!r.is_valid || !r.is_gateway) {
      continue;
    }
    if (best == nullptr || r.hop_count < best->hop_count || (r.hop_count == best->hop_count && r.rssi > best->rssi)) {
      best = &r;
    }
  }
  return best;
}

const RouteEntry *LoraMesh::find_best_gateway_route_() const {
  const RouteEntry *best = nullptr;
  for (const auto &r : this->routes_) {
    if (!r.is_valid || !r.is_gateway) {
      continue;
    }
    if (best == nullptr || r.hop_count < best->hop_count || (r.hop_count == best->hop_count && r.rssi > best->rssi)) {
      best = &r;
    }
  }
  return best;
}

RouteEntry *LoraMesh::alloc_route_slot_() {
  for (auto &r : this->routes_) {
    if (!r.is_valid) {
      return &r;
    }
  }
  // Table full: evict route with most hops, lowest RSSI.
  RouteEntry *worst = nullptr;
  for (auto &r : this->routes_) {
    if (worst == nullptr || r.hop_count > worst->hop_count ||
        (r.hop_count == worst->hop_count && r.rssi < worst->rssi)) {
      worst = &r;
    }
  }
  if (worst != nullptr) {
    ESP_LOGD(TAG, "Route table full, evicting 0x%08" PRIX32, worst->dst_id);
    worst->is_valid = false;
  }
  return worst;
}

void LoraMesh::update_route_(uint32_t dst_id, uint32_t next_hop, uint8_t hops, bool is_gw, float rssi, float snr) {
  uint32_t now = millis();
  RouteEntry *r = this->find_route_(dst_id);
  bool changed = false;
  if (r == nullptr) {
    r = this->alloc_route_slot_();
    if (r == nullptr) {
      return;
    }
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
    changed = true;
  }
  r->is_valid = true;
  r->last_seen = now;
  r->expires_at = now + this->route_ttl_ms_;
  if (changed) {
    this->notify_route_changed_();
  }
}

void LoraMesh::expire_routes_() {
  uint32_t now = millis();
  bool any = false;
  for (auto &r : this->routes_) {
    if (r.is_valid && static_cast<int32_t>(r.expires_at - now) < 0) {
      ESP_LOGD(TAG, "Route to 0x%08" PRIX32 " expired", r.dst_id);
      r.is_valid = false;
      any = true;
      // Passive self-healing (ADR 0002): a direct neighbour going silent
      // invalidates every route relayed through it, so those destinations are
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

// ─── Gateway mode ─────────────────────────────────────────────────────────────

bool LoraMesh::compute_gateway_state_() const {
  switch (this->gateway_mode_) {
    case GatewayMode::NORMAL:
      return false;
    case GatewayMode::GATEWAY:
      return true;
    case GatewayMode::AUTO:
#ifdef USE_WIFI
      return wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected();
#else
      return false;
#endif
    default:
      return false;
  }
}

void LoraMesh::update_gateway_state_() {
  bool new_state = this->compute_gateway_state_();
  if (new_state == this->acting_as_gateway_) {
    return;
  }
  this->acting_as_gateway_ = new_state;
  ESP_LOGI(TAG, "Gateway state changed → %s", new_state ? "GATEWAY" : "node");
  this->gateway_changed_callback_();
  // Queue a HELLO right away so neighbours learn the new state quickly.
  auto pkt = this->build_hello_packet_();
  this->enqueue_tx_(pkt);
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
    this->gateway_available_sensor_->publish_state(this->has_gateway() || this->acting_as_gateway_);
  }
#endif
#ifdef USE_TEXT_SENSOR
  if (this->routing_table_sensor_ != nullptr) {
    this->routing_table_sensor_->publish_state(this->get_routing_table_json());
  }
  if (this->best_gateway_sensor_ != nullptr) {
    this->best_gateway_sensor_->publish_state(this->get_best_gateway());
  }
#endif
}

// ─── Group key management ─────────────────────────────────────────────────────

void LoraMesh::set_group_key(const uint8_t *key, size_t len) {
  if (key == nullptr || len == 0) {
    this->has_group_key_ = false;
    memset(this->group_key_, 0, GROUP_KEY_SIZE);
    ESP_LOGI(TAG, "Group key cleared (unprovisioned)");
  } else {
    memcpy(this->group_key_, key, std::min(len, GROUP_KEY_SIZE));
    if (len < GROUP_KEY_SIZE) {
      memset(this->group_key_ + len, 0, GROUP_KEY_SIZE - len);
    }
    this->has_group_key_ = true;
    ESP_LOGI(TAG, "Group key set (provisioned)");
  }
  this->persist_group_key_();
}

void LoraMesh::set_group_key_hex(const std::string &hex_key) {
  if (hex_key.empty()) {
    this->set_group_key(nullptr, 0);
    return;
  }
  if (hex_key.size() != GROUP_KEY_SIZE * 2) {
    ESP_LOGW(TAG, "set_group_key_hex: expected 32 hex chars, got %zu", hex_key.size());
    return;
  }
  uint8_t key[GROUP_KEY_SIZE];
  for (size_t i = 0; i < GROUP_KEY_SIZE; i++) {
    char hi = hex_key[i * 2];
    char lo = hex_key[i * 2 + 1];
    auto hex_val = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
      if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
      return 0;
    };
    key[i] = static_cast<uint8_t>((hex_val(hi) << 4) | hex_val(lo));
  }
  this->set_group_key(key, GROUP_KEY_SIZE);
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
  uint32_t hash = fnv1a_str("lora_mesh_fc_" + this->node_id_str_);
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

void LoraMesh::persist_group_key_() {
  uint32_t hash = fnv1a_str("lora_mesh_gk_" + this->node_id_str_);
  this->group_key_pref_ = global_preferences->make_preference<uint8_t[GROUP_KEY_SIZE + 1]>(hash, true);

  // Store: 1-byte flag (has_key) + 16-byte key.
  uint8_t buf[GROUP_KEY_SIZE + 1];
  buf[0] = this->has_group_key_ ? 1 : 0;
  memcpy(buf + 1, this->group_key_, GROUP_KEY_SIZE);
  this->group_key_pref_.save(&buf);
}

void LoraMesh::load_group_key_() {
  uint32_t hash = fnv1a_str("lora_mesh_gk_" + this->node_id_str_);
  this->group_key_pref_ = global_preferences->make_preference<uint8_t[GROUP_KEY_SIZE + 1]>(hash, true);

  uint8_t buf[GROUP_KEY_SIZE + 1];
  if (this->group_key_pref_.load(&buf) && buf[0] == 1) {
    memcpy(this->group_key_, buf + 1, GROUP_KEY_SIZE);
    this->has_group_key_ = true;
    ESP_LOGI(TAG, "Group key loaded from NVS (provisioned)");
  } else {
    this->has_group_key_ = false;
    memset(this->group_key_, 0, GROUP_KEY_SIZE);
  }
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
