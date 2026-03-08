#include "EmberBinarySensor.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_binary_sensor";

void EmberBinarySensor::setup() {
  auto *hub = this->parent_;
  this->function_name_ = this->get_function_name_for_type_();

  hub->register_hbridge_callback([this](DeviceAddress addr, int status, int position) {
    if (this->explicit_ids_) {
      DeviceAddress my_addr = (this->table_id_ << 8) | this->device_id_;
      if (addr != my_addr) return;
    } else if (this->function_name_ != 0) {
      DeviceAddress my_addr = this->parent_->resolve_address(this->function_name_);
      if (my_addr == 0 || addr != my_addr) return;
    } else {
      return;
    }
    // H-Bridge status: 0 = idle, non-zero = active/moving
    bool is_extended = (status != 0);
    this->publish_state(is_extended);
  });
}

void EmberBinarySensor::dump_config() {
  LOG_BINARY_SENSOR("", "Ember Binary Sensor", this);
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
  if (this->explicit_ids_) {
    ESP_LOGCONFIG(TAG, "  Table ID: 0x%02X, Device ID: 0x%02X", this->table_id_, this->device_id_);
  }
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
