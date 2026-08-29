#include "packet_admission.h"

namespace esphome::lora_mesh {

PacketInspectionResult PacketAdmission::inspect(std::span<const uint8_t> packet) const {
  PacketInspectionResult result;
  if (packet.size() < MESH_HEADER_SIZE) {
    return result;
  }

  result.header = parse_packet_header(packet.data());
  if (result.header.fabric_id != *this->fabric_id_) {
    result.failure = AdmissionFailure::FABRIC_MISMATCH;
    return result;
  }
  result.failure = AdmissionFailure::NONE;
  return result;
}

PacketAdmissionResult PacketAdmission::authenticate(std::span<const uint8_t> packet, const PacketHeader &header) const {
  PacketAdmissionResult result;
  result.header = header;

  switch (header.packet_type) {
    case PacketType::HELLO:
      if (!this->validate_hello_(header, packet)) {
        result.failure = AdmissionFailure::INVALID_HELLO;
        return result;
      }
      break;
    case PacketType::DATA: {
      if (!PacketAdmission::validate_data_envelope_(header, packet)) {
        result.failure = AdmissionFailure::INVALID_DATA_ENVELOPE;
        return result;
      }
      uint8_t payload_size = packet[MESH_HEADER_SIZE];
      if (!mesh_decrypt_payload(this->fabric_key_, header.src_id, header.dst_id, header.frame_counter,
                                static_cast<uint8_t>(header.packet_type), header.flags, payload_size,
                                packet.data() + MESH_HEADER_SIZE + 1, result.plaintext.data(),
                                packet.data() + MESH_HEADER_SIZE + 1 + payload_size)) {
        result.failure = AdmissionFailure::DATA_AUTHENTICATION_FAILED;
        return result;
      }
      result.plaintext_size = payload_size;
      break;
    }
    default:
      result.failure = AdmissionFailure::UNSUPPORTED_TYPE;
      return result;
  }

  result.failure = AdmissionFailure::NONE;
  return result;
}

bool PacketAdmission::validate_hello_(const PacketHeader &header, std::span<const uint8_t> packet) const {
  constexpr size_t minimum_size = MESH_HEADER_SIZE + 3 + HELLO_AUTH_TAG_SIZE;
  if (packet.size() < minimum_size) {
    return false;
  }
  size_t authenticated_size = packet.size() - HELLO_AUTH_TAG_SIZE;
  size_t offset = MESH_HEADER_SIZE;
  if (packet[offset] != MESH_PROTO_VERSION) {
    return false;
  }
  if ((header.flags & ~FLAG_IS_GATEWAY) != 0 || header.dst_id != MESH_BROADCAST_ID || header.ttl == 0 ||
      header.hop_count != 0 || header.prev_hop != header.src_id || header.next_hop != MESH_BROADCAST_ID) {
    return false;
  }
  uint8_t name_len = packet[offset + 1];
  if (name_len > MESH_NODE_NAME_MAX_LEN) {
    return false;
  }
  size_t route_count_offset = offset + 2 + static_cast<size_t>(name_len);
  if (route_count_offset >= authenticated_size) {
    return false;
  }
  uint8_t route_count = packet[route_count_offset];
  size_t expected_size = route_count_offset + 1 + static_cast<size_t>(route_count) * ROUTE_ADV_SIZE;
  if (expected_size != authenticated_size) {
    return false;
  }
  for (size_t pos = route_count_offset + 1; pos < authenticated_size; pos += ROUTE_ADV_SIZE) {
    uint32_t advertised_destination = get_u32_le(packet.data() + pos + ROUTE_ADV_OFF_DEST_ID);
    uint8_t advertised_hops = packet[pos + ROUTE_ADV_OFF_HOP_COUNT];
    uint8_t route_flags = packet[pos + ROUTE_ADV_OFF_FLAGS];
    if (advertised_destination == MESH_BROADCAST_ID || advertised_destination == header.src_id ||
        advertised_hops < MESH_MIN_ADVERTISED_HOPS || advertised_hops > MESH_MAX_ADVERTISED_HOPS ||
        (route_flags & ~ROUTE_FLAG_IS_GATEWAY) != 0) {
      return false;
    }
  }
  return verify_hello_auth_tag(this->control_plane_key_, packet.data(), authenticated_size,
                               packet.data() + authenticated_size);
}

bool PacketAdmission::validate_data_envelope_(const PacketHeader &header, std::span<const uint8_t> packet) {
  if ((header.flags & ~(FLAG_IS_GATEWAY | FLAG_IS_BROADCAST)) != 0) {
    return false;
  }
  if (packet.size() < MESH_HEADER_SIZE + 1) {
    return false;
  }
  uint8_t payload_size = packet[MESH_HEADER_SIZE];
  size_t expected_size = MESH_HEADER_SIZE + 1 + payload_size + DATA_AUTH_TAG_SIZE;
  if (payload_size > MESH_MAX_DATA_PAYLOAD_SIZE || packet.size() != expected_size) {
    return false;
  }
  bool is_broadcast = header.dst_id == MESH_BROADCAST_ID;
  return is_broadcast == ((header.flags & FLAG_IS_BROADCAST) != 0);
}

}  // namespace esphome::lora_mesh
