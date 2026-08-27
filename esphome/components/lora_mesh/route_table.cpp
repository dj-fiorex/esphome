#include "route_table.h"

#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome::lora_mesh {

[[maybe_unused]] static const char *const TAG = "lora_mesh";

static bool is_preferred_gateway_route(const RouteEntry &candidate, const RouteEntry *current) {
  return current == nullptr || candidate.hop_count < current->hop_count ||
         (candidate.hop_count == current->hop_count &&
          (candidate.rssi > current->rssi || (candidate.rssi == current->rssi && candidate.dst_id < current->dst_id)));
}

RouteUpdate RouteTable::observe_neighbor(uint32_t node_id, bool is_gateway, float rssi, float snr, uint32_t now) {
  this->clear_hold_down_(node_id);
  return this->update_({node_id, node_id, 1, is_gateway, rssi, snr}, now);
}

RouteUpdate RouteTable::consider(const RouteCandidate &candidate, uint32_t now) {
  if (candidate.hop_count < MESH_MIN_ADVERTISED_HOPS || candidate.hop_count > MESH_MAX_ADVERTISED_HOPS ||
      this->is_held_down_(candidate.destination_id, candidate.hop_count, now)) {
    return {};
  }

  RouteEntry *existing = this->find_(candidate.destination_id);
  bool better = existing == nullptr || candidate.hop_count < existing->hop_count ||
                (candidate.hop_count == existing->hop_count && candidate.path_rssi > existing->rssi);
  bool reconfirmed = existing != nullptr && existing->next_hop_id == candidate.next_hop_id &&
                     candidate.hop_count == existing->hop_count;
  if (better || reconfirmed) {
    return this->update_(candidate, now);
  }
  if (existing != nullptr && existing->is_gateway != candidate.is_gateway) {
    existing->is_gateway = candidate.is_gateway;
    existing->gateway_update_pending = true;
    return {.changed = true, .gateway_changed = true};
  }
  return {};
}

bool RouteTable::expire(uint32_t now) {
  bool changed = false;
  for (auto &route : this->entries_) {
    if (!route.is_valid || static_cast<int32_t>(route.expires_at - now) > 0) {
      continue;
    }
    ESP_LOGD(TAG, "Route to 0x%08" PRIX32 " expired", route.dst_id);
    uint32_t expired_destination = route.dst_id;
    bool direct_neighbor_expired = route.dst_id == route.next_hop_id;
    this->hold_down_(route, now);
    changed = true;
    if (direct_neighbor_expired) {
      this->invalidate_via_(expired_destination, now);
    }
  }
  return changed;
}

void RouteTable::clear() {
  for (auto &route : this->entries_) {
    route = RouteEntry{};
  }
}

const RouteEntry *RouteTable::find(uint32_t destination_id) const {
  for (const auto &route : this->entries_) {
    if (route.is_valid && route.dst_id == destination_id) {
      return &route;
    }
  }
  return nullptr;
}

const RouteEntry *RouteTable::nearest_gateway() const {
  const RouteEntry *nearest = nullptr;
  for (const auto &route : this->entries_) {
    if (route.is_valid && route.is_gateway && is_preferred_gateway_route(route, nearest)) {
      nearest = &route;
    }
  }
  return nearest;
}

size_t RouteTable::count() const {
  size_t count = 0;
  for (const auto &route : this->entries_) {
    if (route.is_valid) {
      ++count;
    }
  }
  return count;
}

void RouteTable::acknowledge_gateway_update(uint32_t destination_id) {
  RouteEntry *route = this->find_(destination_id);
  if (route != nullptr) {
    route->gateway_update_pending = false;
  }
}

bool RouteTable::has_pending_gateway_updates() const {
  for (const auto &route : this->entries_) {
    if (route.is_valid && route.gateway_update_pending) {
      return true;
    }
  }
  return false;
}

RouteEntry *RouteTable::find_(uint32_t destination_id) {
  for (auto &route : this->entries_) {
    if (route.is_valid && route.dst_id == destination_id) {
      return &route;
    }
  }
  return nullptr;
}

RouteEntry *RouteTable::allocate_(uint32_t now) {
  for (auto &route : this->entries_) {
    if (!route.is_valid) {
      if (route.hold_down && static_cast<int32_t>(route.expires_at - now) > 0) {
        continue;
      }
      route.hold_down = false;
      return &route;
    }
  }

  RouteEntry *worst = nullptr;
  for (auto &route : this->entries_) {
    if (!route.is_valid || route.gateway_update_pending) {
      continue;
    }
    if (worst == nullptr || route.hop_count > worst->hop_count ||
        (route.hop_count == worst->hop_count && route.rssi < worst->rssi)) {
      worst = &route;
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

RouteUpdate RouteTable::update_(const RouteCandidate &candidate, uint32_t now) {
  if (candidate.hop_count < MESH_MIN_ADVERTISED_HOPS || candidate.hop_count > MESH_MAX_ADVERTISED_HOPS) {
    return {};
  }

  RouteEntry *route = this->find_(candidate.destination_id);
  bool changed = false;
  if (route == nullptr) {
    route = this->allocate_(now);
    if (route == nullptr) {
      return {};
    }
    *route = RouteEntry{};
    route->dst_id = candidate.destination_id;
    changed = true;
  }

  bool current_route_reconfirmed =
      route->is_valid && candidate.hop_count == route->hop_count && candidate.next_hop_id == route->next_hop_id;
  bool update_path = !route->is_valid || current_route_reconfirmed || candidate.hop_count < route->hop_count ||
                     (candidate.hop_count == route->hop_count && candidate.path_rssi > route->rssi) ||
                     candidate.next_hop_id != route->next_hop_id;
  if (update_path) {
    changed = changed || !route->is_valid || route->next_hop_id != candidate.next_hop_id ||
              route->hop_count != candidate.hop_count || route->rssi != candidate.path_rssi ||
              route->snr != candidate.snr;
    route->next_hop_id = candidate.next_hop_id;
    route->hop_count = candidate.hop_count;
    route->rssi = candidate.path_rssi;
    route->snr = candidate.snr;
  }

  bool gateway_changed = route->is_gateway != candidate.is_gateway;
  if (gateway_changed) {
    route->is_gateway = candidate.is_gateway;
    route->gateway_update_pending = true;
    changed = true;
  }
  route->is_valid = true;
  route->last_seen = now;
  route->expires_at = now + this->route_ttl_ms_;
  return {.changed = changed, .gateway_changed = gateway_changed};
}

bool RouteTable::is_held_down_(uint32_t destination_id, uint8_t candidate_hops, uint32_t now) {
  for (auto &route : this->entries_) {
    if (!route.hold_down || route.dst_id != destination_id) {
      continue;
    }
    if (static_cast<int32_t>(route.expires_at - now) <= 0 || candidate_hops <= route.hop_count) {
      route.hold_down = false;
      return false;
    }
    return true;
  }
  return false;
}

void RouteTable::clear_hold_down_(uint32_t destination_id) {
  for (auto &route : this->entries_) {
    if (route.hold_down && route.dst_id == destination_id) {
      route.hold_down = false;
      return;
    }
  }
}

void RouteTable::hold_down_(RouteEntry &route, uint32_t now) {
  route.is_valid = false;
  route.hold_down = true;
  route.expires_at = now + this->route_ttl_ms_ + RouteTable::EXPIRY_SCAN_INTERVAL_MS;
}

void RouteTable::invalidate_via_(uint32_t neighbor_id, uint32_t now) {
  for (auto &route : this->entries_) {
    if (route.is_valid && route.next_hop_id == neighbor_id) {
      ESP_LOGD(TAG, "Route to 0x%08" PRIX32 " invalidated (next hop 0x%08" PRIX32 " lost)", route.dst_id, neighbor_id);
      this->hold_down_(route, now);
    }
  }
}

}  // namespace esphome::lora_mesh
