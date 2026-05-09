#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome::lora_mesh {

class LoraMesh;

/**
 * Abstract radio interface used by LoraMesh.
 *
 * Concrete subclasses (LoRaSX126xRadio, LoRaSX127xRadio) implement this
 * interface by delegating to the actual sx126x / sx127x component.  This
 * keeps the mesh layer fully decoupled from the specific radio driver.
 */
class LoRaRadio {
 public:
  virtual ~LoRaRadio() = default;

  /** Transmit a raw byte buffer via the LoRa radio. */
  virtual void transmit_packet(const std::vector<uint8_t> &data) = 0;

  /** Return the maximum payload size supported by the underlying radio. */
  virtual size_t get_max_packet_size() = 0;

  /**
   * Called by the concrete adapter subclass once the radio component has
   * finished its own setup() so the mesh can register as a listener.
   * The default implementation does nothing; adapters override it to call
   * register_listener() on their parent radio.
   */
  virtual void attach_listener(LoraMesh *mesh) = 0;
};

}  // namespace esphome::lora_mesh
