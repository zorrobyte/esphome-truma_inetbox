#pragma once

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

enum class EMBER_SWITCH_TYPE : uint8_t {
  WATER_PUMP,
};

class EmberSwitch : public Component, public switch_::Switch, public Parented<EmberOneControl> {
 public:
  void setup() override;
  void dump_config() override;

  void set_type(EMBER_SWITCH_TYPE type) { this->type_ = type; }
  void set_table_id(uint8_t id) { this->table_id_ = id; this->explicit_ids_ = true; }
  void set_device_id(uint8_t id) { this->device_id_ = id; this->explicit_ids_ = true; }

 protected:
  void write_state(bool state) override;

  EMBER_SWITCH_TYPE type_{EMBER_SWITCH_TYPE::WATER_PUMP};
  uint8_t table_id_{0};
  uint8_t device_id_{0};
  bool explicit_ids_{false};
};

}  // namespace ember_onecontrol
}  // namespace esphome
