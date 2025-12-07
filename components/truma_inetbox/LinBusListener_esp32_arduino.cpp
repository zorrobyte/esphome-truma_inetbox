#ifdef USE_ESP32_FRAMEWORK_ARDUINO
#include "LinBusListener.h"
#include "esphome/core/log.h"
#ifdef CUSTOM_ESPHOME_UART
#include "esphome/components/uart/truma_uart_component_esp32_arduino.h"
#define ESPHOME_UART uart::truma_ESP32ArduinoUARTComponent
#else
#define ESPHOME_UART uart::ESP32ArduinoUARTComponent
#endif // CUSTOM_ESPHOME_UART
#include "esphome/components/uart/uart_component_esp32_arduino.h"

namespace esphome {
namespace truma_inetbox {

static const char *const TAG = "truma_inetbox.LinBusListener";

void LinBusListener::setup_framework() {
  ESP_LOGI(TAG, "Using Polling Mode for ESP32 (Arduino 3.0)");
  // Increase inter-byte timeout to 20ms to tolerate gaps at 9600 baud.
  // Standard calculation is ~5.7ms which causes partial data errors with slow devices.
  this->time_per_first_byte_ = 20000;
}

void LinBusListener::loop() {
  PollingComponent::loop();

  if (!this->check_for_lin_fault_()) {
    // Drain all available bytes first to avoid false timeouts when loop was blocked
    // Update timestamp before processing to ensure timeout checks use current time
    while (this->available()) {
      // Update timestamp before reading to prevent false timeout on stale timestamp
      // This handles the case where loop was blocked and data accumulated in buffer
      this->last_data_recieved_ = micros();
      
      // Debug mode: peek at byte and log before processing
      if (this->debug_mode_) {
        uint8_t debug_byte;
        if (this->peek_byte(&debug_byte)) {
          ESP_LOGD(TAG, "RX: %02X", debug_byte);
        }
      }
      
      this->read_lin_frame_();
    }
  }
  
  // Process the message queue in the loop since we removed the background task
  this->process_lin_msg_queue(0);
}

}  // namespace truma_inetbox
}  // namespace esphome

#undef ESPHOME_UART

#endif  // USE_ESP32_FRAMEWORK_ARDUINO
