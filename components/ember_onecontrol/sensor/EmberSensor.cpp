#include "EmberSensor.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_sensor";

void EmberSensor::setup() {
  auto *hub = this->parent_;
  this->function_name_ = this->get_function_name_for_type_();

  if (this->type_ == EMBER_SENSOR_TYPE::BATTERY_VOLTAGE) {
    hub->register_rv_status_callback([this](float voltage) {
      this->publish_state(voltage);
    });
  } else {
    // Tank sensors
    hub->register_tank_callback([this](DeviceAddress addr, int percent) {
      if (this->explicit_ids_) {
        DeviceAddress my_addr = (this->table_id_ << 8) | this->device_id_;
        if (addr != my_addr) return;
      } else if (this->function_name_ != 0) {
        DeviceAddress my_addr = this->parent_->resolve_address(this->function_name_);
        if (my_addr == 0 || addr != my_addr) return;
      } else {
        return;  // No way to identify this sensor
      }
      this->publish_state((float) percent);
    });
  }
}

void EmberSensor::dump_config() {
  LOG_SENSOR("", "Ember Sensor", this);
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
  if (this->explicit_ids_) {
    ESP_LOGCONFIG(TAG, "  Table ID: 0x%02X, Device ID: 0x%02X", this->table_id_, this->device_id_);
  }
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
