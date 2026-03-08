#include "EmberSwitch.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_switch";

void EmberSwitch::setup() {
  auto *hub = this->parent_;

  hub->register_relay_callback([this](DeviceAddress addr, bool is_on) {
    if (this->explicit_ids_) {
      DeviceAddress my_addr = (this->table_id_ << 8) | this->device_id_;
      if (addr != my_addr) return;
    }
    this->publish_state(is_on);
  });
}

void EmberSwitch::dump_config() {
  LOG_SWITCH("", "Ember Switch", this);
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
  if (this->explicit_ids_) {
    ESP_LOGCONFIG(TAG, "  Table ID: 0x%02X, Device ID: 0x%02X", this->table_id_, this->device_id_);
  }
}

void EmberSwitch::write_state(bool state) {
  if (!this->explicit_ids_) {
    ESP_LOGW(TAG, "No table_id/device_id configured - cannot send command");
    return;
  }
  this->parent_->send_switch_command(this->table_id_, this->device_id_, state);
  this->publish_state(state);
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
