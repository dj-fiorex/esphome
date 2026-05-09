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

std::string LoraMesh::id_to_hex(uint32_t id) {
  char buf[9];
  snprintf(buf, sizeof(buf), "%08" PRIX32, id);
  return {buf};
}

// ─── Component lifecycle ──────────────────────────────────────────────────────

void LoraMesh::setup() {
  // Derive numeric IDs from strings.
  if (this->node_id_func_) {
    this->node_id_str_ = this->node_id_func_();
    this->node_id_func_ = nullptr;  // Release the lambda capture after use.
  }
  if (this->node_id_str_.empty()) {
    uint8_t mac[6];
    get_mac_address_raw(mac);
    this->node_id_ = fnv1a_32(mac + 3, 3);
    char buf[7];
    snprintf(buf, sizeof(buf), "%06" PRIX32, this->node_id_ & 0xFFFFFFu);
    this->node_id_str_ = buf;
  } else {
    this->node_id_ = fnv1a_str(this->node_id_str_);
  }
  this->mesh_id_ = fnv1a_str(this->mesh_secret_);

  // Initialise arrays.
  for (auto &r : this->routes_) {
    r = RouteEntry{};
  }
  for (auto &s : this->seen_cache_) {
    s = SeenEntry{};
  }

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

  ESP_LOGI(TAG, "LoraMesh setup: node_id=%s (0x%08" PRIX32 ") mesh_id=0x%08" PRIX32 " gw=%s",
           this->node_id_str_.c_str(), this->node_id_, this->mesh_id_,
           this->acting_as_gateway_ ? "yes" : "no");
}

void LoraMesh::loop() {
  uint32_t now = millis();

  // Update gateway state for AUTO mode.
  this->update_gateway_state_();

  // Periodic HELLO beacon.
  if (now - this->last_hello_ >= this->discovery_interval_ms_) {
    this->last_hello_ = now;
    auto pkt = this->build_hello_packet_();
    this->transmit_(pkt);
    ESP_LOGD(TAG, "HELLO sent (%zu bytes)", pkt.size());
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
}

// ─── Public API ───────────────────────────────────────────────────────────────

bool LoraMesh::send_message(const std::string &destination, const std::string &payload) {
  uint32_t dst_id = fnv1a_str(destination);
  const RouteEntry *route = this->find_route_(dst_id);
  if (route == nullptr) {
    ESP_LOGW(TAG, "No route to %s", destination.c_str());
    return false;
  }
  auto pkt = this->build_data_packet_(dst_id, payload);
  this->transmit_(pkt);
  ESP_LOGD(TAG, "DATA sent to %s (%zu bytes)", destination.c_str(), payload.size());
  return true;
}

bool LoraMesh::broadcast_message(const std::string &payload) {
  auto pkt = this->build_data_packet_(MESH_BROADCAST_ID, payload);
  this->transmit_(pkt);
  ESP_LOGD(TAG, "DATA broadcast (%zu bytes)", payload.size());
  return true;
}

bool LoraMesh::send_to_gateway(const std::string &payload) {
  const RouteEntry *gw = this->find_best_gateway_route_();
  if (gw == nullptr) {
    ESP_LOGW(TAG, "send_to_gateway: no gateway in routing table");
    return false;
  }
  auto pkt = this->build_data_packet_(gw->dst_id, payload);
  this->transmit_(pkt);
  ESP_LOGD(TAG, "DATA sent to gateway 0x%08" PRIX32 " (%zu bytes)", gw->dst_id, payload.size());
  return true;
}

bool LoraMesh::has_route(const std::string &node_id) const { return this->find_route_(fnv1a_str(node_id)) != nullptr; }

bool LoraMesh::has_gateway() const { return this->find_best_gateway_route_() != nullptr; }

std::string LoraMesh::get_best_gateway() const {
  const RouteEntry *gw = this->find_best_gateway_route_();
  return (gw != nullptr) ? id_to_hex(gw->dst_id) : std::string{};
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
  out.reserve(2 + entry_count * 90);
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
    char buf[96];
    snprintf(buf, sizeof(buf), R"({"dst":"%s","nh":"%s","hops":%u,"gw":%s,"rssi":%.0f})",
             id_to_hex(r.dst_id).c_str(), id_to_hex(r.next_hop_id).c_str(), r.hop_count,
             r.is_gateway ? "true" : "false", static_cast<double>(r.rssi));
    out += buf;
  }
  out += ']';
  return out;
}

// ─── Radio packet dispatcher ──────────────────────────────────────────────────

void LoraMesh::on_radio_packet(const std::vector<uint8_t> &pkt, float rssi, float snr) {
  if (pkt.size() < MESH_HEADER_SIZE) {
    ESP_LOGW(TAG, "Packet too short (%zu bytes), dropped", pkt.size());
    return;
  }
  const uint8_t *h = pkt.data();

  // Mesh ID filter.
  uint32_t mesh_id = get_u32_le(h + 0);
  if (mesh_id != this->mesh_id_) {
    ESP_LOGV(TAG, "Mesh ID mismatch (0x%08" PRIX32 " vs 0x%08" PRIX32 "), dropped", mesh_id, this->mesh_id_);
    return;
  }

  auto pkt_type = static_cast<PacketType>(h[4]);
  uint8_t flags = h[5];
  uint32_t src_id = get_u32_le(h + 6);
  uint32_t dst_id = get_u32_le(h + 10);
  uint32_t msg_id = get_u32_le(h + 14);
  uint8_t ttl = h[18];
  uint8_t hop_count = h[19];
  uint32_t prev_hop = get_u32_le(h + 20);

  // Ignore our own transmissions echoed back.
  if (src_id == this->node_id_) {
    return;
  }

  // Duplicate suppression.
  if (this->is_duplicate_(src_id, msg_id)) {
    ESP_LOGV(TAG, "Duplicate from 0x%08" PRIX32 " msg=%" PRIu32 ", dropped", src_id, msg_id);
    return;
  }
  this->mark_seen_(src_id, msg_id);

  bool src_is_gw = (flags & FLAG_IS_GATEWAY) != 0;

  switch (pkt_type) {
    case PacketType::HELLO:
      this->process_hello_(pkt, MESH_HEADER_SIZE, src_id, src_is_gw, prev_hop, rssi, snr);
      break;
    case PacketType::DATA:
      this->process_data_(pkt, MESH_HEADER_SIZE, src_id, dst_id, msg_id, ttl, hop_count, prev_hop, flags, rssi, snr);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled packet type %u from 0x%08" PRIX32, static_cast<uint8_t>(pkt_type), src_id);
      break;
  }
}

// ─── HELLO processing ─────────────────────────────────────────────────────────

void LoraMesh::process_hello_(const std::vector<uint8_t> &pkt, size_t offset, uint32_t src_id, bool src_is_gateway,
                               uint32_t prev_hop, float rssi, float snr) {
  // Always create/update a direct route to the sender.
  this->update_route_(src_id, src_id, 1, src_is_gateway, rssi, snr);

  if (pkt.size() < offset + HELLO_FIXED_SIZE) {
    return;
  }
  uint8_t route_count = pkt[offset + 1];
  size_t pos = offset + HELLO_FIXED_SIZE;

  for (uint8_t i = 0; i < route_count && pos + ROUTE_ADV_SIZE <= pkt.size(); ++i, pos += ROUTE_ADV_SIZE) {
    uint32_t adv_dst = get_u32_le(pkt.data() + pos);
    uint8_t adv_hops = pkt[pos + 4];

    if (adv_dst == this->node_id_ || adv_dst == src_id) {
      continue;
    }
    uint8_t new_hops = static_cast<uint8_t>(adv_hops + 1);
    if (new_hops > this->max_hops_) {
      continue;
    }
    const RouteEntry *existing = this->find_route_(adv_dst);
    if (existing == nullptr || new_hops < existing->hop_count) {
      this->update_route_(adv_dst, src_id, new_hops, false, rssi, snr);
    }
  }

  ESP_LOGD(TAG, "HELLO from 0x%08" PRIX32 " rssi=%.0f snr=%.1f gw=%s routes=%u", src_id, static_cast<double>(rssi),
           static_cast<double>(snr), src_is_gateway ? "yes" : "no", route_count);
}

// ─── DATA processing ──────────────────────────────────────────────────────────

void LoraMesh::process_data_(const std::vector<uint8_t> &pkt, size_t offset, uint32_t src_id, uint32_t dst_id,
                              uint32_t msg_id, uint8_t ttl, uint8_t hop_count, uint32_t prev_hop, uint8_t flags,
                              float rssi, float snr) {
  bool is_broadcast = (dst_id == MESH_BROADCAST_ID) || ((flags & FLAG_IS_BROADCAST) != 0);
  bool is_for_us = is_broadcast || (dst_id == this->node_id_);

  if (is_for_us) {
    if (pkt.size() < offset + 1) {
      return;
    }
    uint8_t payload_len = pkt[offset];
    size_t payload_start = offset + 1;
    if (pkt.size() < payload_start + payload_len) {
      return;
    }

    MeshMessage msg;
    msg.source = id_to_hex(src_id);
    msg.destination = id_to_hex(dst_id);
    msg.prev_hop = id_to_hex(prev_hop);
    msg.payload.assign(reinterpret_cast<const char *>(pkt.data() + payload_start), payload_len);
    msg.msg_id = msg_id;
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

  // Packet forwarding: we must mutate the TTL, hop_count and prev_hop fields
  // before retransmitting, so we copy the packet.  The input is const& and
  // the three modified bytes are in the fixed 24-byte header.
  std::vector<uint8_t> fwd(pkt);
  fwd[18] = ttl - 1;
  fwd[19] = hop_count + 1;
  put_u32_le(&fwd[20], this->node_id_);

  if (is_broadcast) {
    this->radio_->transmit_packet(fwd);
    ESP_LOGD(TAG, "Forwarded broadcast ttl=%u", ttl - 1);
    return;
  }

  const RouteEntry *route = this->find_route_(dst_id);
  if (route == nullptr) {
    ESP_LOGW(TAG, "No route to forward 0x%08" PRIX32, dst_id);
    return;
  }
  this->radio_->transmit_packet(fwd);
  ESP_LOGD(TAG, "Forwarded unicast to 0x%08" PRIX32 " ttl=%u", dst_id, ttl - 1);
}

// ─── Packet builders ──────────────────────────────────────────────────────────

std::vector<uint8_t> LoraMesh::build_header_(PacketType type, uint8_t flags, uint32_t dst_id, uint32_t msg_id,
                                             uint8_t ttl, uint8_t hop_count, uint32_t prev_hop) const {
  std::vector<uint8_t> h(MESH_HEADER_SIZE);
  put_u32_le(h.data() + 0, this->mesh_id_);
  h[4] = static_cast<uint8_t>(type);
  h[5] = flags;
  put_u32_le(h.data() + 6, this->node_id_);
  put_u32_le(h.data() + 10, dst_id);
  put_u32_le(h.data() + 14, msg_id);
  h[18] = ttl;
  h[19] = hop_count;
  put_u32_le(h.data() + 20, prev_hop);
  return h;
}

std::vector<uint8_t> LoraMesh::build_hello_packet_() {
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

  size_t max_pkt = (this->radio_ != nullptr) ? this->radio_->get_max_packet_size() : 255;
  size_t budget = (max_pkt > MESH_HEADER_SIZE + HELLO_FIXED_SIZE) ? max_pkt - MESH_HEADER_SIZE - HELLO_FIXED_SIZE : 0;
  size_t max_routes = std::min(budget / ROUTE_ADV_SIZE, count);
  if (max_routes > 255) {
    max_routes = 255;
  }

  auto pkt = this->build_header_(PacketType::HELLO, flags, MESH_BROADCAST_ID, this->next_msg_id_(), this->max_hops_,
                                 0, this->node_id_);
  pkt.reserve(pkt.size() + HELLO_FIXED_SIZE + max_routes * ROUTE_ADV_SIZE);
  pkt.push_back(MESH_PROTO_VERSION);
  pkt.push_back(static_cast<uint8_t>(max_routes));

  for (size_t i = 0; i < max_routes; ++i) {
    const RouteEntry *r = ptrs[i];
    uint8_t entry[ROUTE_ADV_SIZE];
    put_u32_le(entry, r->dst_id);
    entry[4] = r->hop_count;
    int clamped = static_cast<int>(r->rssi);
    clamped = (clamped < -128) ? -128 : (clamped > 127) ? 127 : clamped;
    entry[5] = static_cast<uint8_t>(static_cast<int8_t>(clamped));
    pkt.insert(pkt.end(), entry, entry + ROUTE_ADV_SIZE);
  }
  return pkt;
}

std::vector<uint8_t> LoraMesh::build_data_packet_(uint32_t dst_id, const std::string &payload) {
  uint8_t flags = this->acting_as_gateway_ ? FLAG_IS_GATEWAY : 0;
  if (dst_id == MESH_BROADCAST_ID) {
    flags |= FLAG_IS_BROADCAST;
  }
  auto pkt = this->build_header_(PacketType::DATA, flags, dst_id, this->next_msg_id_(), this->max_hops_, 0,
                                 this->node_id_);
  size_t len = std::min(payload.size(), static_cast<size_t>(255));
  pkt.push_back(static_cast<uint8_t>(len));
  pkt.insert(pkt.end(), payload.begin(), payload.begin() + static_cast<ptrdiff_t>(len));
  return pkt;
}

void LoraMesh::transmit_(const std::vector<uint8_t> &pkt) {
  if (this->radio_ != nullptr) {
    this->radio_->transmit_packet(pkt);
  }
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
    if (best == nullptr || r.hop_count < best->hop_count ||
        (r.hop_count == best->hop_count && r.rssi > best->rssi)) {
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
    if (best == nullptr || r.hop_count < best->hop_count ||
        (r.hop_count == best->hop_count && r.rssi > best->rssi)) {
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
  if (!r->is_valid || hops < r->hop_count || (hops == r->hop_count && rssi > r->rssi) ||
      r->next_hop_id != next_hop) {
    r->next_hop_id = next_hop;
    r->hop_count = hops;
    r->is_gateway = is_gw;
    r->rssi = rssi;
    r->snr = snr;
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
    }
  }
  if (any) {
    this->notify_route_changed_();
  }
}

void LoraMesh::notify_route_changed_() {
  this->route_update_callback_();
  this->publish_diagnostics_();
}

// ─── Duplicate suppression ───────────────────────────────────────────────────

bool LoraMesh::is_duplicate_(uint32_t src_id, uint32_t msg_id) {
  uint32_t now = millis();
  for (const auto &s : this->seen_cache_) {
    if (s.src_id == src_id && s.msg_id == msg_id && static_cast<int32_t>(s.expires_at - now) > 0) {
      return true;
    }
  }
  return false;
}

void LoraMesh::mark_seen_(uint32_t src_id, uint32_t msg_id) {
  SeenEntry &slot = this->seen_cache_[this->seen_cache_head_];
  slot.src_id = src_id;
  slot.msg_id = msg_id;
  slot.expires_at = millis() + this->seen_cache_ttl_ms_;
  this->seen_cache_head_ = (this->seen_cache_head_ + 1) % this->seen_cache_.size();
}

void LoraMesh::expire_seen_() {
  uint32_t now = millis();
  for (auto &s : this->seen_cache_) {
    if (s.src_id != 0 && static_cast<int32_t>(s.expires_at - now) < 0) {
      s.src_id = 0;
      s.msg_id = 0;
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
  // Immediately send HELLO so neighbours learn the new state quickly.
  auto pkt = this->build_hello_packet_();
  this->transmit_(pkt);
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

}  // namespace esphome::lora_mesh
