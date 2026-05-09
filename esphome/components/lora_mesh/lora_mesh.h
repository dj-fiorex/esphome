#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "lora_packet.h"
#include "lora_radio.h"

#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::lora_mesh {

// Array sizes are set at compile time via Python cg.add_define().
// These fallback values are only used for IDE/static-analysis builds.
#ifndef LORA_MESH_MAX_ROUTES
#define LORA_MESH_MAX_ROUTES 16  // NOLINT(cppcoreguidelines-macro-usage)
#endif
#ifndef LORA_MESH_SEEN_CACHE_SIZE
#define LORA_MESH_SEEN_CACHE_SIZE 32  // NOLINT(cppcoreguidelines-macro-usage)
#endif

class LoraMesh : public Component {
 public:
  // ── Component lifecycle ──────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // ── Configuration setters (called from Python codegen) ────────────────
  void set_radio(LoRaRadio *radio) { this->radio_ = radio; }
  void set_node_id(TemplatableValue<std::string> node_id) {
    this->node_id_template_ = node_id;
    this->has_node_id_ = true;
  }
  void set_mesh_secret(const std::string &secret) { this->mesh_secret_ = secret; }
  void set_gateway_mode(GatewayMode mode) { this->gateway_mode_ = mode; }
  void set_max_hops(uint8_t max_hops) { this->max_hops_ = max_hops; }
  void set_discovery_interval(uint32_t ms) { this->discovery_interval_ms_ = ms; }
  void set_route_ttl(uint32_t ms) { this->route_ttl_ms_ = ms; }
  void set_seen_cache_ttl(uint32_t ms) { this->seen_cache_ttl_ms_ = ms; }
  void set_forward_messages(bool forward) { this->forward_messages_ = forward; }

  // ── Public API (callable from C++ lambdas / actions) ─────────────────
  bool send_message(const std::string &destination, const std::string &payload);
  bool broadcast_message(const std::string &payload);
  bool send_to_gateway(const std::string &payload);
  bool has_route(const std::string &node_id) const;
  bool has_gateway() const;
  std::string get_best_gateway() const;
  std::string get_node_id() const { return this->node_id_str_; }
  bool is_gateway() const { return this->acting_as_gateway_; }
  void clear_routes();
  std::string get_routing_table_json() const;
  size_t get_known_node_count() const;

  // ── Called by the radio adapter when a packet arrives ────────────────
  void on_radio_packet(const std::vector<uint8_t> &pkt, float rssi, float snr);

  // ── Callback registrations (used by build_callback_automation) ───────

  /** Called for every DATA packet addressed to this node or broadcast. */
  template<typename F> void add_on_message_callback(F &&callback) {
    this->message_callback_.add(std::forward<F>(callback));
  }

  /** Called whenever the routing table changes. */
  template<typename F> void add_on_route_update_callback(F &&callback) {
    this->route_update_callback_.add(std::forward<F>(callback));
  }

  /** Called when the gateway-available state changes. */
  template<typename F> void add_on_gateway_changed_callback(F &&callback) {
    this->gateway_changed_callback_.add(std::forward<F>(callback));
  }

  // ── Optional diagnostic sensors ──────────────────────────────────────
#ifdef USE_SENSOR
  void set_node_count_sensor(sensor::Sensor *s) { this->node_count_sensor_ = s; }
#endif
#ifdef USE_BINARY_SENSOR
  void set_gateway_available_sensor(binary_sensor::BinarySensor *s) { this->gateway_available_sensor_ = s; }
#endif
#ifdef USE_TEXT_SENSOR
  void set_routing_table_sensor(text_sensor::TextSensor *s) { this->routing_table_sensor_ = s; }
  void set_best_gateway_sensor(text_sensor::TextSensor *s) { this->best_gateway_sensor_ = s; }
#endif

 protected:
  // ── Packet building ────────────────────────────────────────────────────
  std::vector<uint8_t> build_header_(PacketType type, uint8_t flags, uint32_t dst_id, uint32_t msg_id, uint8_t ttl,
                                     uint8_t hop_count, uint32_t prev_hop) const;
  std::vector<uint8_t> build_hello_packet_();
  std::vector<uint8_t> build_data_packet_(uint32_t dst_id, const std::string &payload);
  void transmit_(const std::vector<uint8_t> &pkt);

  // ── Packet processing ──────────────────────────────────────────────────
  void process_hello_(const std::vector<uint8_t> &pkt, size_t offset, uint32_t src_id, bool src_is_gateway,
                      uint32_t prev_hop, float rssi, float snr);
  void process_data_(const std::vector<uint8_t> &pkt, size_t offset, uint32_t src_id, uint32_t dst_id,
                     uint32_t msg_id, uint8_t ttl, uint8_t hop_count, uint32_t prev_hop, uint8_t flags, float rssi,
                     float snr);

  // ── Routing helpers ────────────────────────────────────────────────────
  RouteEntry *find_route_(uint32_t dst_id);
  const RouteEntry *find_route_(uint32_t dst_id) const;
  RouteEntry *find_best_gateway_route_();
  const RouteEntry *find_best_gateway_route_() const;
  void update_route_(uint32_t dst_id, uint32_t next_hop, uint8_t hops, bool is_gw, float rssi, float snr);
  RouteEntry *alloc_route_slot_();
  void expire_routes_();
  void notify_route_changed_();

  // ── Duplicate suppression ──────────────────────────────────────────────
  bool is_duplicate_(uint32_t src_id, uint32_t msg_id);
  void mark_seen_(uint32_t src_id, uint32_t msg_id);
  void expire_seen_();

  // ── Gateway mode ──────────────────────────────────────────────────────
  bool compute_gateway_state_() const;
  void update_gateway_state_();

  // ── Diagnostics ───────────────────────────────────────────────────────
  void publish_diagnostics_();

  // ── Utility ───────────────────────────────────────────────────────────
  static std::string id_to_hex(uint32_t id);
  uint32_t next_msg_id_() { return ++this->seq_counter_; }

  // ── State ──────────────────────────────────────────────────────────────
  LoRaRadio *radio_{nullptr};
  uint32_t node_id_{0};
  uint32_t mesh_id_{0};
  std::string node_id_str_;
  bool has_node_id_{false};
  TemplatableValue<std::string> node_id_template_;
  std::string mesh_secret_;
  GatewayMode gateway_mode_{GatewayMode::NORMAL};
  bool acting_as_gateway_{false};
  bool last_gateway_available_{false};
  uint8_t max_hops_{8};
  uint32_t discovery_interval_ms_{30000};
  uint32_t route_ttl_ms_{300000};
  uint32_t seen_cache_ttl_ms_{120000};
  bool forward_messages_{true};
  uint32_t seq_counter_{0};
  uint32_t last_hello_{0};
  uint32_t last_expire_check_{0};
  uint32_t last_diag_publish_{0};

  // Fixed-size arrays (sizes set by LORA_MESH_MAX_ROUTES / LORA_MESH_SEEN_CACHE_SIZE defines)
  std::array<RouteEntry, LORA_MESH_MAX_ROUTES> routes_{};
  std::array<SeenEntry, LORA_MESH_SEEN_CACHE_SIZE> seen_cache_{};
  size_t seen_cache_head_{0};

  // ── Callbacks ──────────────────────────────────────────────────────────
  // message_callback_ is likely always registered; use CallbackManager.
  CallbackManager<void(MeshMessage)> message_callback_;
  // Route and gateway callbacks are often unused; save 4 bytes each.
  LazyCallbackManager<void()> route_update_callback_;
  LazyCallbackManager<void()> gateway_changed_callback_;

  // ── Diagnostic sensors ────────────────────────────────────────────────
#ifdef USE_SENSOR
  sensor::Sensor *node_count_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *gateway_available_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *routing_table_sensor_{nullptr};
  text_sensor::TextSensor *best_gateway_sensor_{nullptr};
#endif
};

}  // namespace esphome::lora_mesh
