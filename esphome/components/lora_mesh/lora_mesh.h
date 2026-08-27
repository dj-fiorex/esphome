#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "lora_mesh_crypto.h"
#include "lora_packet.h"
#include "lora_radio.h"
#include "route_table.h"

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
#include <span>
#include <string>

namespace esphome::lora_mesh {

// Array sizes are set at compile time via Python cg.add_define().
// These fallback values are only used for IDE/static-analysis builds.
#ifndef LORA_MESH_SEEN_CACHE_SIZE
#define LORA_MESH_SEEN_CACHE_SIZE 32  // NOLINT(cppcoreguidelines-macro-usage)
#endif
#ifndef LORA_MESH_TX_QUEUE_SIZE
#define LORA_MESH_TX_QUEUE_SIZE 8  // NOLINT(cppcoreguidelines-macro-usage)
#endif

class LoraMesh : public Component {
 public:
  explicit LoraMesh(const std::string &fabric_key_hex, LoRaRadio *radio);

  // ── Component lifecycle ──────────────────────────────────────────────────
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR - 1.0f; }

  // ── Configuration setters (called from Python codegen) ────────────────
  void set_node_id(const TemplatableValue<std::string> &node_id) {
    this->node_id_template_ = node_id;
    this->has_node_id_ = true;
  }
  void set_max_hops(uint8_t max_hops) {
    this->max_hops_ = max_hops;
    if (this->max_hops_ < MESH_MIN_ADVERTISED_HOPS) {
      this->max_hops_ = MESH_MIN_ADVERTISED_HOPS;
    } else if (this->max_hops_ > MESH_MAX_ADVERTISED_HOPS) {
      this->max_hops_ = MESH_MAX_ADVERTISED_HOPS;
    }
  }
  void set_discovery_interval(uint32_t ms) { this->discovery_interval_ms_ = ms; }
  void set_route_ttl(uint32_t ms) { this->routing_table_.set_route_ttl(ms); }
  void set_seen_cache_ttl(uint32_t ms) { this->seen_cache_ttl_ms_ = ms; }
  void set_forward_messages(bool forward) { this->forward_messages_ = forward; }
  void set_tx_jitter(uint32_t ms) { this->tx_jitter_ms_ = ms; }

  // ── Public API (callable from C++ lambdas / actions) ─────────────────
  bool send_message(const std::string &destination, std::span<const uint8_t> payload);
  bool broadcast_message(std::span<const uint8_t> payload);
  bool send_to_gateway(std::span<const uint8_t> payload);
  bool send_message(const std::string &destination, const std::string &payload) {
    return this->send_message(destination, LoraMesh::payload_as_bytes_(payload));
  }
  bool broadcast_message(const std::string &payload) {
    return this->broadcast_message(LoraMesh::payload_as_bytes_(payload));
  }
  bool send_to_gateway(const std::string &payload) {
    return this->send_to_gateway(LoraMesh::payload_as_bytes_(payload));
  }
  void set_upstream_connected(bool connected);
  bool has_route(const std::string &node_id) const;
  bool has_gateway() const;
  std::string get_nearest_gateway() const;
  std::string get_node_id() const { return this->node_id_str_; }
  bool is_upstream_connected() const { return this->upstream_connected_; }
  /** Returns the human-readable name for a node hash, or nullptr if unknown. */
  const char *get_node_name(uint32_t id) const;
  void clear_routes();
  std::string get_routing_table_json() const;
  size_t get_known_node_count() const;

#ifdef LORA_MESH_LINK_SIM
  // ── Link simulator (debug/test only — see ADR-0004) ──────────────────
  // Emulates "out of radio range" by dropping packets whose immediate sender
  // (prev_hop) is on a runtime-configurable blocklist. Lets a chain topology
  // (A↔B↔C with A and C unable to hear each other) be forced and reshaped
  // from the web UI without physically separating boards.
  /** Add a neighbour (by node_id string) whose direct transmissions are dropped. */
  void add_blocked_neighbor(const std::string &name);
  /** Clear the link-sim blocklist (restore full connectivity). */
  void clear_blocked_neighbors();
  /** Comma-separated list of currently blocked neighbours (name if known, else hex). */
  std::string get_blocked_neighbors_str() const;
#endif

  // ── Called by the radio adapter when a packet arrives ────────────────
  void on_radio_packet(const uint8_t *pkt, size_t pkt_len, float rssi, float snr);

  // ── Callback registrations (used by build_callback_automation) ───────

  /** Called for every DATA packet addressed to this node or broadcast. */
  template<typename F> void add_on_message_callback(F &&callback) {
    this->message_callback_.add(std::forward<F>(callback));
  }

  /** Called whenever the routing table changes. */
  template<typename F> void add_on_route_update_callback(F &&callback) {
    this->route_update_callback_.add(std::forward<F>(callback));
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
  void set_nearest_gateway_sensor(text_sensor::TextSensor *s) { this->nearest_gateway_sensor_ = s; }
#endif

 protected:
  // ── Packet building ────────────────────────────────────────────────────
  Packet build_hello_packet_();
  Packet build_data_packet_(uint32_t dst_id, uint32_t next_hop, std::span<const uint8_t> payload);
  bool validate_hello_packet_(const PacketHeader &header, std::span<const uint8_t> packet) const;
  bool validate_data_envelope_(const PacketHeader &header, std::span<const uint8_t> packet) const;

  // ── Outgoing TX queue (issue #12) ─────────────────────────────────────
  // All outbound packets are enqueued here and drained at most one per
  // loop() iteration; the radio does blind blocking TX, so this bounds the
  // loop stall to one packet's airtime and the pre-send jitter is a
  // poor-man's CSMA against simultaneous Forwarding Nodes. Each packet is
  // attempted once; radio failures are logged and dropped without retry.
  bool enqueue_tx_(const Packet &pkt);
  void drain_tx_queue_(uint32_t now);
  void schedule_hello_update_();
  void queue_pending_hello_(uint32_t now);
  void acknowledge_advertised_gateway_updates_(const Packet &hello);
  bool has_pending_gateway_updates_() const;

  // ── Packet processing ──────────────────────────────────────────────────
  void process_hello_(const PacketHeader &header, std::span<const uint8_t> packet, float rssi, float snr);
  void process_data_(const PacketHeader &header, std::span<const uint8_t> packet, std::span<const uint8_t> plaintext,
                     float rssi, float snr);

  // ── Route change orchestration ─────────────────────────────────────────
  void notify_route_changed_();

  // ── Duplicate suppression ──────────────────────────────────────────────
  bool is_duplicate_(uint32_t src_id, uint32_t frame_counter);
  void mark_seen_(uint32_t src_id, uint32_t frame_counter);
  void expire_seen_();

#ifdef LORA_MESH_LINK_SIM
  // ── Link simulator (debug/test only) ──────────────────────────────────
  /** True if prev_hop is on the blocklist (packet should be dropped as unheard). */
  bool is_link_blocked_(uint32_t prev_hop) const;
#endif

  // ── Node name cache ────────────────────────────────────────────────────
  void store_node_name_(uint32_t id, const char *name, uint8_t name_len);
  const char *lookup_node_name_(uint32_t id) const;

  // ── Diagnostics ───────────────────────────────────────────────────────
  void publish_diagnostics_();

  // ── Utility ───────────────────────────────────────────────────────────
  static std::span<const uint8_t> payload_as_bytes_(const std::string &payload) {
    return {reinterpret_cast<const uint8_t *>(payload.data()), payload.size()};
  }
  static void id_to_hex(uint32_t id, char out[9]);
  bool next_frame_counter_(uint32_t &frame_counter);

  // ── Frame counter persistence ─────────────────────────────────────────
  void persist_frame_counter_();
  void load_frame_counter_();

  // ── Replay protection ─────────────────────────────────────────────────
  enum class ReplayDecision : uint8_t { ACCEPT, REPLAY, TABLE_FULL };
  /** Atomically admit and record an authenticated counter, failing closed when an unknown source cannot be stored. */
  ReplayDecision admit_replay_counter_(uint32_t src_id, uint32_t frame_counter);

 private:
  // The Fabric identity is invariant-coupled: the ID must only ever be the
  // domain-separated derivation of this immutable, validated key.
  uint32_t fabric_id_{0};
  std::array<uint8_t, FABRIC_KEY_SIZE> fabric_key_{};
  std::array<uint8_t, CONTROL_PLANE_KEY_SIZE> control_plane_key_{};
  bool fabric_key_valid_{false};

  // Replay high-water state is safety-critical: only admit_replay_counter_()
  // may mutate it so accepted sources can never be evicted under pressure.
  struct ReplayEntry {
    uint32_t src_id{0};
    uint32_t high_water{0};
    bool is_valid{false};
  };
  std::array<ReplayEntry, LORA_MESH_MAX_ROUTES> replay_counters_{};

 protected:
  // ── State ──────────────────────────────────────────────────────────────
  LoRaRadio *radio_;
  uint32_t node_id_{0};
  std::string node_id_str_;
  TemplatableValue<std::string> node_id_template_;
  bool has_node_id_{false};
  bool upstream_connected_{false};
  bool setup_complete_{false};
  bool hello_update_pending_{false};
  enum class QueuedHelloState : uint8_t { NONE, CURRENT, STALE };
  QueuedHelloState queued_hello_state_{QueuedHelloState::NONE};
  uint8_t max_hops_{8};
  uint32_t discovery_interval_ms_{30000};
  uint32_t seen_cache_ttl_ms_{120000};
  bool forward_messages_{true};
  uint32_t frame_counter_{0};
  uint32_t last_hello_{0};
  uint32_t last_expire_check_{0};
  uint32_t last_diag_publish_{0};
  static constexpr uint32_t HELLO_UPDATE_MIN_INTERVAL_MS = 1000;

  // ── Frame counter persistence (batched NVS writes) ────────────────────
  // We persist frame_counter_ in batches: on boot we load the persisted value
  // (which was written ahead by FRAME_COUNTER_BATCH) and set our counter to
  // that value. On TX we only write to NVS when we exhaust the current batch.
  static constexpr uint32_t FRAME_COUNTER_BATCH = 1000;
  uint32_t frame_counter_persist_threshold_{0};  // write NVS when frame_counter_ >= this
  ESPPreferenceObject frame_counter_pref_{};

  // Fixed-size arrays (sizes set by LORA_MESH_MAX_ROUTES / LORA_MESH_SEEN_CACHE_SIZE defines)
  RouteTable routing_table_;
  std::array<SeenEntry, LORA_MESH_SEEN_CACHE_SIZE> seen_cache_{};
  size_t seen_cache_head_{0};

#ifdef LORA_MESH_LINK_SIM
  // Link-sim blocklist of immediate-sender (prev_hop) hashes; small fixed
  // capacity, zero heap allocation. Compiled out unless link_sim is enabled.
  std::array<uint32_t, 4> blocked_neighbors_{};
  size_t blocked_count_{0};
#endif

  // Node name cache — same capacity as the routing table, zero heap allocation.
  std::array<NameEntry, LORA_MESH_MAX_ROUTES> name_map_{};

  // Outgoing TX ring buffer — fixed capacity, zero heap allocation.
  std::array<Packet, LORA_MESH_TX_QUEUE_SIZE> tx_queue_{};
  size_t tx_queue_head_{0};
  size_t tx_queue_count_{0};
  uint32_t tx_jitter_ms_{100};    // upper bound for the random pre-send backoff
  uint32_t tx_next_tx_at_{0};     // millis() deadline of the armed backoff
  bool tx_backoff_armed_{false};  // backoff sampled for the current head packet

  // ── Callbacks ──────────────────────────────────────────────────────────
  // message_callback_ is likely always registered; use CallbackManager.
  CallbackManager<void(MeshMessage)> message_callback_;
  // Route callbacks are often unused; save 4 bytes.
  LazyCallbackManager<void()> route_update_callback_;

  // ── Diagnostic sensors ────────────────────────────────────────────────
#ifdef USE_SENSOR
  sensor::Sensor *node_count_sensor_{nullptr};
#endif
#ifdef USE_BINARY_SENSOR
  binary_sensor::BinarySensor *gateway_available_sensor_{nullptr};
#endif
#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *routing_table_sensor_{nullptr};
  text_sensor::TextSensor *nearest_gateway_sensor_{nullptr};
#endif
};

}  // namespace esphome::lora_mesh

// Included after LoraMesh is fully defined — lora_radio_adapters.h calls
// LoraMesh::on_radio_packet(), which requires the complete class definition.
#include "lora_radio_adapters.h"
