#pragma once

#include "esphome/core/component.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

enum class EMBER_BINARY_SENSOR_TYPE : uint8_t {
  SLIDE_STATE,
  AWNING_STATE,
};

class EmberBinarySensor : public Component, public binary_sensor::BinarySensor, public Parented<EmberOneControl> {
 public:
  void setup() override;
  void dump_config() override;

  void set_type(EMBER_BINARY_SENSOR_TYPE type) { this->type_ = type; }
  void set_table_id(uint8_t id) { this->table_id_ = id; this->explicit_ids_ = true; }
  void set_device_id(uint8_t id) { this->device_id_ = id; this->explicit_ids_ = true; }

 protected:
  EMBER_BINARY_SENSOR_TYPE type_{EMBER_BINARY_SENSOR_TYPE::SLIDE_STATE};
  uint8_t table_id_{0};
  uint8_t device_id_{0};
  bool explicit_ids_{false};
};

}  // namespace ember_onecontrol
}  // namespace esphome
