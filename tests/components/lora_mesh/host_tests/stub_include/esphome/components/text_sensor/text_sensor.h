#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace esphome::text_sensor {

class TextSensor {
 public:
  void publish_state(const std::string &state) { this->publish_state(state.data(), state.size()); }
  void publish_state(const char *state) { this->publish_state(state, strlen(state)); }
  void publish_state(const char *state, size_t len) {
    if (len != this->state.size() || memcmp(state, this->state.data(), len) != 0) {
      this->state.assign(state, len);
    }
  }

  std::string state;
};

}  // namespace esphome::text_sensor
