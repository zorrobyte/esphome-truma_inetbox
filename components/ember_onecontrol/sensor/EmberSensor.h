#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

enum class EMBER_SENSOR_TYPE : uint8_t {
  BATTERY_VOLTAGE,
  TANK_FRESH,
  TANK_BLACK,
  TANK_GREY,
};

class EmberSensor : public Component, public sensor::Sensor, public Parented<EmberOneControl> {
 public:
  void setup() override;
  void dump_config() override;

  void set_type(EMBER_SENSOR_TYPE type) { this->type_ = type; }
  void set_table_id(uint8_t id) { this->table_id_ = id; this->explicit_ids_ = true; }
  void set_device_id(uint8_t id) { this->device_id_ = id; this->explicit_ids_ = true; }

 protected:
  EMBER_SENSOR_TYPE type_{EMBER_SENSOR_TYPE::BATTERY_VOLTAGE};
  uint8_t table_id_{0};
  uint8_t device_id_{0};
  bool explicit_ids_{false};
};

}  // namespace ember_onecontrol
}  // namespace esphome
