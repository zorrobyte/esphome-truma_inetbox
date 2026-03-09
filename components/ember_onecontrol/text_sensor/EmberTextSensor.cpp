#include "EmberTextSensor.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_text_sensor";

void EmberTextSensor::setup() {
  auto *hub = this->parent_;

  if (this->type_ == EMBER_TEXT_SENSOR_TYPE::BATTERY_STATUS) {
    hub->register_rv_status_callback([this](float voltage) {
      this->publish_state(this->parent_->get_battery_status());
    });
  } else if (this->type_ == EMBER_TEXT_SENSOR_TYPE::CONNECTION_STATUS) {
    hub->register_auth_state_callback([this](AuthState state) {
      switch (state) {
        case AuthState::DISCONNECTED:
          this->publish_state("Disconnected");
          break;
        case AuthState::SCANNING:
          this->publish_state("Scanning");
          break;
        case AuthState::CONNECTING:
          this->publish_state("Connecting");
          break;
        case AuthState::CONNECTED:
          this->publish_state("Connected");
          break;
        case AuthState::READING_UNLOCK_STATUS:
        case AuthState::UNLOCK_KEY_SENT:
        case AuthState::WAITING_FOR_SEED:
        case AuthState::AUTH_KEY_SENT:
          this->publish_state("Authenticating");
          break;
        case AuthState::AUTHENTICATED:
          this->publish_state("Authenticated");
          break;
      }
    });
  }
}

void EmberTextSensor::dump_config() {
  LOG_TEXT_SENSOR("", "Ember Text Sensor", this);
  ESP_LOGCONFIG(TAG, "  Type: %d", (int) this->type_);
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
