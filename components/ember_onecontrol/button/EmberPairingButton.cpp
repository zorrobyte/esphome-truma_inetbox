#include "EmberPairingButton.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_pairing_button";

void EmberPairingButton::dump_config() {
  LOG_BUTTON("", "Ember Pairing Button", this);
}

void EmberPairingButton::press_action() {
  ESP_LOGI(TAG, "Pairing button pressed — starting 60s discovery window");
  this->parent_->start_pairing();
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif
