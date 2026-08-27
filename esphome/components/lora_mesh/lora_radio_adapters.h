#pragma once

// This file is conditionally included from the Python-generated code.
// Only one of the two adapter classes is compiled, depending on which radio
// component is configured in the user's YAML.

#include "lora_radio.h"
#include <vector>

namespace esphome::lora_mesh::detail {

template<typename RadioError> TransmissionOutcome to_transmission_outcome(RadioError error) {
  switch (error) {
    case RadioError::NONE:
      return TransmissionOutcome::SUCCESS;
    case RadioError::TIMEOUT:
      return TransmissionOutcome::TIMEOUT;
    case RadioError::INVALID_PARAMS:
      return TransmissionOutcome::INVALID_PARAMETER;
  }
  return TransmissionOutcome::INVALID_PARAMETER;
}

/** Shared implementation for the two concrete ESPHome radio adapters. */
template<typename Radio, typename Listener> class LoRaRadioAdapter : public LoRaRadio, public Listener {
 public:
  TransmissionOutcome transmit_packet(const Packet &data) override {
    // The mesh owns fixed-capacity packets; both ESPHome radio APIs currently accept vectors.
    this->transmit_buffer_.assign(data.begin(), data.end());
    return to_transmission_outcome(this->radio_->transmit_packet(this->transmit_buffer_));
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
  explicit LoRaRadioAdapter(Radio *radio) : radio_(radio) {
    // Allocate the driver's vector storage during construction, never on the packet hot path.
    this->transmit_buffer_.reserve(LORA_MAX_PACKET_SIZE);
  }

  Radio *radio_;
  LoraMesh *mesh_{nullptr};
  std::vector<uint8_t> transmit_buffer_;
};

}  // namespace esphome::lora_mesh::detail

#ifdef LORA_MESH_USE_SX126X
#include "esphome/components/sx126x/sx126x.h"

namespace esphome::lora_mesh {

/**
 * Thin adapter that bridges SX126x ↔ LoraMesh.
 *
 * Registers itself as an SX126xListener so it receives every packet from the
 * radio's loop()-level dispatcher, then forwards it to LoraMesh::on_radio_packet().
 */
class LoRaSX126xRadio final : public detail::LoRaRadioAdapter<sx126x::SX126x, sx126x::SX126xListener> {
 public:
  explicit LoRaSX126xRadio(sx126x::SX126x *radio) : LoRaRadioAdapter(radio) {}
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
class LoRaSX127xRadio final : public detail::LoRaRadioAdapter<sx127x::SX127x, sx127x::SX127xListener> {
 public:
  explicit LoRaSX127xRadio(sx127x::SX127x *radio) : LoRaRadioAdapter(radio) {}
};

}  // namespace esphome::lora_mesh
#endif  // LORA_MESH_USE_SX127X
