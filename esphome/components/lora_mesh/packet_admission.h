#pragma once

#include "esphome/core/defines.h"
#include "lora_mesh_crypto.h"
#include "lora_packet.h"

#include <array>
#include <cstdint>
#include <span>

namespace esphome::lora_mesh {

enum class AdmissionFailure : uint8_t {
  NONE = 0,
  PACKET_TOO_SHORT,
  FABRIC_MISMATCH,
  UNSUPPORTED_TYPE,
  INVALID_HELLO,
  INVALID_DATA_ENVELOPE,
  DATA_AUTHENTICATION_FAILED,
};

struct PacketAdmissionResult {
  AdmissionFailure failure{AdmissionFailure::PACKET_TOO_SHORT};
  PacketHeader header{};
  std::array<uint8_t, MESH_MAX_DATA_PAYLOAD_SIZE> plaintext{};
  uint8_t plaintext_size{0};

  bool accepted() const { return this->failure == AdmissionFailure::NONE; }
  std::span<const uint8_t> plaintext_view() const { return {this->plaintext.data(), this->plaintext_size}; }
};

struct PacketInspectionResult {
  AdmissionFailure failure{AdmissionFailure::PACKET_TOO_SHORT};
  PacketHeader header{};

  bool accepted() const { return this->failure == AdmissionFailure::NONE; }
};

class PacketAdmission {
 public:
  PacketAdmission(const uint32_t &fabric_id, const std::array<uint8_t, FABRIC_KEY_SIZE> &fabric_key,
                  const std::array<uint8_t, CONTROL_PLANE_KEY_SIZE> &control_plane_key)
      : fabric_id_(&fabric_id), fabric_key_(fabric_key.data()), control_plane_key_(control_plane_key.data()) {}

  PacketInspectionResult inspect(std::span<const uint8_t> packet) const;
  PacketAdmissionResult authenticate(std::span<const uint8_t> packet, const PacketHeader &header) const;

 private:
  bool validate_hello_(const PacketHeader &header, std::span<const uint8_t> packet) const;
  static bool validate_data_envelope_(const PacketHeader &header, std::span<const uint8_t> packet);

  const uint32_t *fabric_id_;
  const uint8_t *fabric_key_;
  const uint8_t *control_plane_key_;
};

}  // namespace esphome::lora_mesh
