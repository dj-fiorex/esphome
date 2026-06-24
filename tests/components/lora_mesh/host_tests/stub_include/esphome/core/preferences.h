#pragma once

// Stub preferences.h for host tests.
// Declares a minimal Preferences interface and global_preferences pointer
// that is implemented in stubs.cpp.

#include "esphome/core/preference_backend.h"

namespace esphome {

struct Preferences : public PreferencesMixin<Preferences> {
  using PreferencesMixin<Preferences>::make_preference;
  virtual ESPPreferenceObject make_preference(size_t size, uint32_t type, bool in_flash) = 0;
  virtual ESPPreferenceObject make_preference(size_t size, uint32_t type) = 0;
  virtual bool sync() = 0;
  virtual bool reset() = 0;
  virtual ~Preferences() = default;
};

using ESPPreferences = Preferences;
extern ESPPreferences *global_preferences;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome
