#include "EmberLight.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_light";

void EmberLight::setup() {
  auto *hub = this->parent_;
  this->function_name_ = this->get_function_name_for_type_();

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
    if (this->current_state_ != is_on) {
      this->current_state_ = is_on;
      if (this->light_state_ != nullptr) {
        auto call = this->light_state_->make_call();
        call.set_state(is_on);
        call.perform();
      }
    }
  });
}

void EmberLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Ember Light:");
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
  if (this->explicit_ids_) {
    ESP_LOGCONFIG(TAG, "  Table ID: 0x%02X, Device ID: 0x%02X", this->table_id_, this->device_id_);
  }
}

light::LightTraits EmberLight::get_traits() {
  auto traits = light::LightTraits();
  traits.set_supported_color_modes({light::ColorMode::ON_OFF});
  return traits;
}

void EmberLight::write_state(light::LightState *state) {
  bool binary;
  state->current_values_as_binary(&binary);

  // Skip if state already matches (callback-driven update, no need to re-send)
  if (binary == this->current_state_) return;

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

  this->parent_->send_switch_command(tbl, dev, binary);
  this->current_state_ = binary;
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
