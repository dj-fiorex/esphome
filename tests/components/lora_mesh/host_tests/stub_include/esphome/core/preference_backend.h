#pragma once

// Stub preference_backend.h for host tests.
// Provides just enough interface for lora_mesh to compile and link.

#include <cstdint>
#include <cstddef>

namespace esphome {

struct PreferenceBackend {
  virtual bool save(const uint8_t *data, size_t len) = 0;
  virtual bool load(uint8_t *data, size_t len) = 0;
  virtual ~PreferenceBackend() = default;
};

using ESPPreferenceBackend = PreferenceBackend;

class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(PreferenceBackend *backend) : backend_(backend) {}

  template<typename T> bool save(const T *src) {
    if (this->backend_ == nullptr)
      return false;
    return this->backend_->save(reinterpret_cast<const uint8_t *>(src), sizeof(T));
  }

  template<typename T> bool load(T *dest) {
    if (this->backend_ == nullptr)
      return false;
    return this->backend_->load(reinterpret_cast<uint8_t *>(dest), sizeof(T));
  }

 protected:
  PreferenceBackend *backend_{nullptr};
};

/// CRTP mixin providing type-safe template make_preference<T>() helpers.
template<typename Derived> class PreferencesMixin {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t type, bool in_flash) {
    return static_cast<Derived *>(this)->make_preference(sizeof(T), type, in_flash);
  }

  template<typename T> ESPPreferenceObject make_preference(uint32_t type) {
    return static_cast<Derived *>(this)->make_preference(sizeof(T), type);
  }

 private:
  PreferencesMixin() = default;
  friend Derived;
};

}  // namespace esphome
