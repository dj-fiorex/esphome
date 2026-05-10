#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "lora_mesh.h"

namespace esphome::lora_mesh {

// ─── Actions ──────────────────────────────────────────────────────────────────

/**
 * lora_mesh.send — send a unicast message to a named node.
 *
 * YAML:
 *   - lora_mesh.send:
 *       id: mesh
 *       destination: "node-abc"
 *       payload: "hello"
 */
template<typename... Ts> class SendMessageAction : public Action<Ts...>, public Parented<LoraMesh> {
 public:
  TEMPLATABLE_VALUE(std::string, destination)
  TEMPLATABLE_VALUE(std::string, payload)

  void play(const Ts &...x) override {
    this->parent_->send_message(this->destination_.value(x...), this->payload_.value(x...));
  }
};

/**
 * lora_mesh.broadcast — broadcast a message to all nodes in the mesh.
 *
 * YAML:
 *   - lora_mesh.broadcast:
 *       id: mesh
 *       payload: "hello all"
 */
template<typename... Ts> class BroadcastMessageAction : public Action<Ts...>, public Parented<LoraMesh> {
 public:
  TEMPLATABLE_VALUE(std::string, payload)

  void play(const Ts &...x) override { this->parent_->broadcast_message(this->payload_.value(x...)); }
};

/**
 * lora_mesh.send_to_gateway — send a message to the best known gateway node.
 *
 * YAML:
 *   - lora_mesh.send_to_gateway:
 *       id: mesh
 *       payload: !lambda 'return "data";'
 */
template<typename... Ts> class SendToGatewayAction : public Action<Ts...>, public Parented<LoraMesh> {
 public:
  TEMPLATABLE_VALUE(std::string, payload)

  void play(const Ts &...x) override { this->parent_->send_to_gateway(this->payload_.value(x...)); }
};

/**
 * lora_mesh.set_connected — notify the mesh whether the upstream connection is up.
 *
 * Only effective when `gateway: manual` is configured.  While connected the
 * node announces itself as a gateway; on disconnect it reverts to a normal node.
 *
 * YAML:
 *   - lora_mesh.set_connected:
 *       id: mesh
 *       connected: true
 */
template<typename... Ts> class SetConnectedAction : public Action<Ts...>, public Parented<LoraMesh> {
 public:
  TEMPLATABLE_VALUE(bool, connected)

  void play(const Ts &...x) override { this->parent_->set_upstream_connected(this->connected_.value(x...)); }
};

}  // namespace esphome::lora_mesh
