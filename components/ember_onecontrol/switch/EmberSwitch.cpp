#include "EmberSwitch.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_switch";

void EmberSwitch::setup() {
  auto *hub = this->parent_;
  this->function_name_ = this->get_function_name_for_type_();

  if (this->is_hbridge_type_()) {
    // H-bridge devices (slide, awning) report status via hbridge callback
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
      // H-bridge: non-zero status means extended/deployed
      this->publish_state(status != 0);
    });
  } else {
    // Relay devices (water pump, etc.)
    hub->register_relay_callback([this](DeviceAddress addr, bool is_on) {
      if (this->explicit_ids_) {
        DeviceAddress my_addr = (this->table_id_ << 8) | this->device_id_;
        if (addr != my_addr) return;
      } else if (this->function_name_ != 0) {
        DeviceAddress my_addr = this->parent_->resolve_address(this->function_name_);
        if (my_addr == 0 || addr != my_addr) return;
      } else {
        return;
      }
      this->publish_state(is_on);
    });
  }
}

void EmberSwitch::dump_config() {
  LOG_SWITCH("", "Ember Switch", this);
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
  if (this->explicit_ids_) {
    ESP_LOGCONFIG(TAG, "  Table ID: 0x%02X, Device ID: 0x%02X", this->table_id_, this->device_id_);
  }
}

void EmberSwitch::write_state(bool state) {
  uint8_t tbl = this->table_id_;
  uint8_t dev = this->device_id_;

  if (!this->explicit_ids_) {
    if (this->function_name_ != 0) {
      DeviceAddress addr = this->parent_->resolve_address(this->function_name_);
      if (addr != 0) {
        tbl = (addr >> 8) & 0xFF;
        dev = addr & 0xFF;
      } else {
        ESP_LOGW(TAG, "Metadata not yet received - cannot send command");
        return;
      }
    } else {
      ESP_LOGW(TAG, "No table_id/device_id configured - cannot send command");
      return;
    }
  }
  this->parent_->send_switch_command(tbl, dev, state);
  this->publish_state(state);
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
