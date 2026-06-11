// Host-side stand-ins for the ESPHome runtime symbols lora_mesh links against.
// Only what the component actually calls is implemented; the fake clock is
// controllable from tests via test_clock_set()/test_clock_advance().

#include "esphome/core/component.h"

#include <cstdint>
#include <cstring>

namespace esphome {

namespace {
uint32_t g_fake_millis = 1000;
uint32_t g_fake_random = 0;
}  // namespace

void test_clock_set(uint32_t ms) { g_fake_millis = ms; }
void test_clock_advance(uint32_t ms) { g_fake_millis += ms; }
void test_random_set(uint32_t v) { g_fake_random = v; }

uint32_t millis() { return g_fake_millis; }

// Mirrors the implementation in esphome/core/helpers.cpp (which is not
// host-test friendly to compile wholesale).
void *callback_manager_grow(void *data, uint16_t size, uint16_t &capacity, size_t elem_size) {
  uint16_t new_cap = size + 1;
  auto *new_data = ::operator new(new_cap * elem_size);
  if (data != nullptr) {
    memcpy(new_data, data, size * elem_size);
    ::operator delete(data);
  }
  capacity = new_cap;
  return new_data;
}

// Deterministic: defaults to 0 (no HELLO stagger, no TX backoff); tests can
// override via test_random_set().
uint32_t random_uint32() { return g_fake_random; }

void get_mac_address_raw(uint8_t *mac) {
  static const uint8_t FAKE_MAC[6] = {0x02, 0x00, 0x00, 0xAB, 0xCD, 0xEF};
  memcpy(mac, FAKE_MAC, 6);
}

// Component methods normally defined in esphome/core/component.cpp (which
// drags in Application/Scheduler and is not host-test friendly).
void Component::setup() {}
void Component::loop() {}
void Component::dump_config() {}
float Component::get_setup_priority() const { return 0.0f; }
bool Component::can_proceed() { return true; }
void Component::call_setup() { this->setup(); }
void Component::mark_failed() {}

}  // namespace esphome
