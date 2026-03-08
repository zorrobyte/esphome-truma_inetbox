#include "EmberLight.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_light";

void EmberLight::setup() {
  auto *hub = this->parent_;

  hub->register_relay_callback([this](DeviceAddress addr, bool is_on) {
    if (this->explicit_ids_) {
      DeviceAddress my_addr = (this->table_id_ << 8) | this->device_id_;
      if (addr != my_addr) return;
    }
    this->current_state_ = is_on;
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

  if (!this->explicit_ids_) {
    ESP_LOGW(TAG, "No table_id/device_id configured - cannot send command");
    return;
  }

  this->parent_->send_switch_command(this->table_id_, this->device_id_, binary);
  this->current_state_ = binary;
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
