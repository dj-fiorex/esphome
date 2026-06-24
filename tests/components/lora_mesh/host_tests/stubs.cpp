// Host-side stand-ins for the ESPHome runtime symbols lora_mesh links against.
// Only what the component actually calls is implemented; the fake clock is
// controllable from tests via test_clock_set()/test_clock_advance().

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

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

// ─── Fake preferences backend for host tests ─────────────────────────────────

// In-memory key-value store keyed by (hash, size) for the preferences system.
static std::unordered_map<uint32_t, std::vector<uint8_t>> g_pref_store;

struct FakePreferenceBackend : public PreferenceBackend {
  uint32_t key;
  size_t data_size;

  FakePreferenceBackend(uint32_t key, size_t data_size) : key(key), data_size(data_size) {}

  bool save(const uint8_t *data, size_t len) override {
    g_pref_store[this->key].assign(data, data + len);
    return true;
  }

  bool load(uint8_t *data, size_t len) override {
    auto it = g_pref_store.find(this->key);
    if (it == g_pref_store.end() || it->second.size() != len) {
      return false;
    }
    memcpy(data, it->second.data(), len);
    return true;
  }
};

// Keep backends alive for the lifetime of the process.
static std::vector<std::unique_ptr<FakePreferenceBackend>> g_backends;

struct FakePreferences : public Preferences {
  using PreferencesMixin<Preferences>::make_preference;

  ESPPreferenceObject make_preference(size_t size, uint32_t type, bool /*in_flash*/) override {
    return this->make_preference(size, type);
  }

  ESPPreferenceObject make_preference(size_t size, uint32_t type) override {
    auto backend = std::make_unique<FakePreferenceBackend>(type, size);
    auto *ptr = backend.get();
    g_backends.push_back(std::move(backend));
    return ESPPreferenceObject(ptr);
  }

  bool sync() override { return true; }
  bool reset() override {
    g_pref_store.clear();
    return true;
  }
};

static FakePreferences g_fake_prefs;
ESPPreferences *global_preferences = &g_fake_prefs;  // NOLINT

// Test helper: clear all persisted preferences (for test isolation).
void test_preferences_clear() {
  g_pref_store.clear();
}

}  // namespace esphome
