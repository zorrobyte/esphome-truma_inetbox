#pragma once

#include "esphome/core/component.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

enum class EMBER_TEXT_SENSOR_TYPE : uint8_t {
  BATTERY_STATUS,
  CONNECTION_STATUS,
};

class EmberTextSensor : public Component, public text_sensor::TextSensor, public Parented<EmberOneControl> {
 public:
  void setup() override;
  void dump_config() override;

  void set_type(EMBER_TEXT_SENSOR_TYPE type) { this->type_ = type; }

 protected:
  EMBER_TEXT_SENSOR_TYPE type_{EMBER_TEXT_SENSOR_TYPE::BATTERY_STATUS};
};

}  // namespace ember_onecontrol
}  // namespace esphome
