#pragma once

#include "lora_packet.h"
#include "lora_radio.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::lora_mesh {

#ifndef LORA_MESH_TX_QUEUE_SIZE
#define LORA_MESH_TX_QUEUE_SIZE 8  // NOLINT(cppcoreguidelines-macro-usage)
#endif

enum class OutboundPacketKind : uint8_t { DATA = 0, HELLO };

class OutboundAirtime {
 public:
  static constexpr uint32_t MAX_JITTER_MS = 0x7FFFFFFFu;
  using RefreshHello = Packet (*)(void *context);
  using HelloAttempted = void (*)(void *context, const Packet &packet);

  OutboundAirtime(LoRaRadio *radio, void *context, RefreshHello refresh_hello, HelloAttempted hello_attempted)
      : radio_(radio), context_(context), refresh_hello_(refresh_hello), hello_attempted_(hello_attempted) {}

  void set_jitter(uint32_t jitter_ms) {
    this->jitter_ms_ = jitter_ms > OutboundAirtime::MAX_JITTER_MS ? OutboundAirtime::MAX_JITTER_MS : jitter_ms;
  }
  uint32_t get_jitter() const { return this->jitter_ms_; }
  size_t capacity() const { return this->queue_.size(); }

  bool enqueue(const Packet &packet, OutboundPacketKind kind = OutboundPacketKind::DATA);
  bool has_queued_hello() const;
  void invalidate_queued_hello();
  void drain(uint32_t now);

 private:
  struct QueueEntry {
    Packet packet;
    OutboundPacketKind kind{OutboundPacketKind::DATA};
  };

  LoRaRadio *radio_;
  void *context_;
  RefreshHello refresh_hello_;
  HelloAttempted hello_attempted_;
  std::array<QueueEntry, LORA_MESH_TX_QUEUE_SIZE> queue_{};
  size_t head_{0};
  size_t count_{0};
  uint32_t jitter_ms_{100};
  uint32_t next_tx_at_{0};
  bool backoff_armed_{false};
  bool queued_hello_stale_{false};
};

}  // namespace esphome::lora_mesh
