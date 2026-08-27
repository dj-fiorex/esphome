#include "outbound_airtime.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::lora_mesh {

[[maybe_unused]] static const char *const TAG = "lora_mesh";

bool OutboundAirtime::enqueue(const Packet &packet, OutboundPacketKind kind) {
  if (this->count_ >= this->queue_.size()) {
    ESP_LOGW(TAG, "TX queue full (%zu), packet dropped", this->queue_.size());
    return false;
  }
  size_t tail = (this->head_ + this->count_) % this->queue_.size();
  this->queue_[tail].packet = packet;
  this->queue_[tail].kind = kind;
  ++this->count_;
  return true;
}

bool OutboundAirtime::has_queued_hello() const {
  for (size_t offset = 0; offset < this->count_; ++offset) {
    size_t index = (this->head_ + offset) % this->queue_.size();
    if (this->queue_[index].kind == OutboundPacketKind::HELLO) {
      return true;
    }
  }
  return false;
}

void OutboundAirtime::invalidate_queued_hello() {
  if (this->has_queued_hello()) {
    this->queued_hello_stale_ = true;
  }
}

void OutboundAirtime::drain(uint32_t now) {
  if (this->count_ == 0) {
    return;
  }
  if (!this->backoff_armed_) {
    uint32_t backoff = this->jitter_ms_ > 0 ? random_uint32() % (this->jitter_ms_ + 1) : 0;
    this->next_tx_at_ = now + backoff;
    this->backoff_armed_ = true;
  }
  if (static_cast<int32_t>(now - this->next_tx_at_) < 0) {
    return;
  }

  QueueEntry &entry = this->queue_[this->head_];
  Packet refreshed_hello;
  const Packet *packet_to_send = &entry.packet;
  if (entry.kind == OutboundPacketKind::HELLO && this->queued_hello_stale_ && this->refresh_hello_ != nullptr) {
    refreshed_hello = this->refresh_hello_(this->context_);
    packet_to_send = &refreshed_hello;
  }

  TransmissionOutcome outcome = TransmissionOutcome::SUCCESS;
  if (this->radio_ != nullptr && !packet_to_send->empty()) {
    outcome = this->radio_->transmit_packet(*packet_to_send);
  }
  if (outcome == TransmissionOutcome::TIMEOUT) {
    ESP_LOGE(TAG, "Radio transmission timed out; packet dropped");
  } else if (outcome == TransmissionOutcome::INVALID_PARAMETER) {
    ESP_LOGE(TAG, "Radio rejected transmission parameters; packet dropped");
  }

  if (entry.kind == OutboundPacketKind::HELLO) {
    if (this->hello_attempted_ != nullptr) {
      this->hello_attempted_(this->context_, *packet_to_send);
    }
    this->queued_hello_stale_ = false;
  }
  this->head_ = (this->head_ + 1) % this->queue_.size();
  --this->count_;
  this->backoff_armed_ = false;
}

}  // namespace esphome::lora_mesh
