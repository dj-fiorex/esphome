#pragma once

// This file is conditionally included from the Python-generated code.
// Only one of the two adapter classes is compiled, depending on which radio
// component is configured in the user's YAML.

#include "lora_radio.h"
#include <vector>

#ifdef LORA_MESH_USE_SX126X
#include "esphome/components/sx126x/sx126x.h"

namespace esphome::lora_mesh {

/**
 * Thin adapter that bridges SX126x ↔ LoraMesh.
 *
 * Registers itself as an SX126xListener so it receives every packet from the
 * radio's loop()-level dispatcher, then forwards it to LoraMesh::on_radio_packet().
 */
class LoRaSX126xRadio : public LoRaRadio, public sx126x::SX126xListener {
 public:
  explicit LoRaSX126xRadio(sx126x::SX126x *radio) : radio_(radio) {}

  void transmit_packet(const Packet &data) override {
    // Convert the stack-allocated Packet to the vector the sx126x API expects.
    std::vector<uint8_t> vec(data.begin(), data.end());
    this->radio_->transmit_packet(vec);
  }

  size_t get_max_packet_size() override { return this->radio_->get_max_packet_size(); }

  void attach_listener(LoraMesh *mesh) override {
    this->mesh_ = mesh;
    this->radio_->register_listener(this);
  }

  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override {
    if (this->mesh_ != nullptr) {
      this->mesh_->on_radio_packet(packet.data(), packet.size(), rssi, snr);
    }
  }

 protected:
  sx126x::SX126x *radio_;
  LoraMesh *mesh_{nullptr};
};

}  // namespace esphome::lora_mesh
#endif  // LORA_MESH_USE_SX126X

// ─────────────────────────────────────────────────────────────────────────────

#ifdef LORA_MESH_USE_SX127X
#include "esphome/components/sx127x/sx127x.h"

namespace esphome::lora_mesh {

/**
 * Thin adapter that bridges SX127x ↔ LoraMesh.
 */
class LoRaSX127xRadio : public LoRaRadio, public sx127x::SX127xListener {
 public:
  explicit LoRaSX127xRadio(sx127x::SX127x *radio) : radio_(radio) {}

  void transmit_packet(const Packet &data) override {
    // Convert the stack-allocated Packet to the vector the sx127x API expects.
    std::vector<uint8_t> vec(data.begin(), data.end());
    this->radio_->transmit_packet(vec);
  }

  size_t get_max_packet_size() override { return this->radio_->get_max_packet_size(); }

  void attach_listener(LoraMesh *mesh) override {
    this->mesh_ = mesh;
    this->radio_->register_listener(this);
  }

  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override {
    if (this->mesh_ != nullptr) {
      this->mesh_->on_radio_packet(packet.data(), packet.size(), rssi, snr);
    }
  }

 protected:
  sx127x::SX127x *radio_;
  LoraMesh *mesh_{nullptr};
};

}  // namespace esphome::lora_mesh
#endif  // LORA_MESH_USE_SX127X
