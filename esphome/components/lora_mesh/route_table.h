#pragma once

#include "lora_packet.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::lora_mesh {

#ifndef LORA_MESH_MAX_ROUTES
#define LORA_MESH_MAX_ROUTES 16  // NOLINT(cppcoreguidelines-macro-usage)
#endif

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
  bool hold_down : 1;

  RouteEntry() : is_gateway(false), is_valid(false), gateway_update_pending(false), hold_down(false) {}
};

struct RouteCandidate {
  uint32_t destination_id{0};
  uint32_t next_hop_id{0};
  uint8_t hop_count{0};
  bool is_gateway{false};
  float path_rssi{0.0f};
  float snr{0.0f};
};

struct RouteUpdate {
  bool changed{false};
  bool gateway_changed{false};
};

class RouteTable {
 public:
  static constexpr uint32_t EXPIRY_SCAN_INTERVAL_MS = 10000;

  void set_route_ttl(uint32_t route_ttl_ms) { this->route_ttl_ms_ = route_ttl_ms; }
  uint32_t get_route_ttl() const { return this->route_ttl_ms_; }

  RouteUpdate observe_neighbor(uint32_t node_id, bool is_gateway, float rssi, float snr, uint32_t now);
  RouteUpdate consider(const RouteCandidate &candidate, uint32_t now);
  bool expire(uint32_t now);
  void clear();

  const RouteEntry *find(uint32_t destination_id) const;
  const RouteEntry *nearest_gateway() const;
  size_t count() const;
  const std::array<RouteEntry, LORA_MESH_MAX_ROUTES> &entries() const { return this->entries_; }

  void acknowledge_gateway_update(uint32_t destination_id);
  bool has_pending_gateway_updates() const;

 private:
  RouteEntry *find_(uint32_t destination_id);
  RouteEntry *allocate_(const RouteCandidate &candidate, uint32_t now);
  RouteUpdate update_(const RouteCandidate &candidate, uint32_t now);
  bool is_held_down_(uint32_t destination_id, uint8_t candidate_hops, uint32_t now);
  void clear_hold_down_(uint32_t destination_id);
  void hold_down_(RouteEntry &route, uint32_t now);
  void invalidate_via_(uint32_t neighbor_id, uint32_t now);

  std::array<RouteEntry, LORA_MESH_MAX_ROUTES> entries_{};
  uint32_t route_ttl_ms_{90000};
};

}  // namespace esphome::lora_mesh
