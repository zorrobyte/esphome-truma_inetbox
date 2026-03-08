#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

enum class EMBER_LIGHT_TYPE : uint8_t {
  ACCENT_LIGHT,
  STEP_LIGHT,
  PUMP_LIGHT,
  AWNING_LIGHT,
  CEILING_LIGHT,
};

class EmberLight : public Component, public light::LightOutput, public Parented<EmberOneControl> {
 public:
  void setup() override;
  void dump_config() override;

  light::LightTraits get_traits() override;
  void write_state(light::LightState *state) override;

  void set_type(EMBER_LIGHT_TYPE type) { this->type_ = type; }
  void set_table_id(uint8_t id) { this->table_id_ = id; this->explicit_ids_ = true; }
  void set_device_id(uint8_t id) { this->device_id_ = id; this->explicit_ids_ = true; }

 protected:
  EMBER_LIGHT_TYPE type_{EMBER_LIGHT_TYPE::CEILING_LIGHT};
  uint8_t table_id_{0};
  uint8_t device_id_{0};
  bool explicit_ids_{false};
  bool current_state_{false};
};

}  // namespace ember_onecontrol
}  // namespace esphome
