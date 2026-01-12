#include "TrumaiNetBoxAppAirconManual.h"
#include "TrumaStatusFrameBuilder.h"
#include "esphome/core/log.h"
#include "helpers.h"
#include "TrumaiNetBoxApp.h"

namespace esphome {
namespace truma_inetbox {

static const char *const TAG = "truma_inetbox.TrumaiNetBoxAppAirconManual";

StatusFrameAirconManualResponse *TrumaiNetBoxAppAirconManual::update_prepare() {
  // An update is currently going on.
  if (this->update_status_prepared_ || this->update_status_stale_) {
    return &this->update_status_;
  }

  // prepare status response
  this->update_status_ = {};
  this->update_status_.mode = this->data_.mode;
  this->update_status_.vent_mode = this->data_.vent_mode;
  this->update_status_.aircon_on = 0x01;  // Must be 1 for commands to be accepted
  this->update_status_.target_temp_aircon = this->data_.target_temp_aircon;

  // Ensure a valid temperature is set (default to 22°C if not set)
  if (this->update_status_.target_temp_aircon == TargetTemp::TARGET_TEMP_OFF ||
      static_cast<uint16_t>(this->update_status_.target_temp_aircon) == 0) {
    this->update_status_.target_temp_aircon = TargetTemp::TARGET_TEMP_22C;
  }

  this->update_status_prepared_ = true;
  return &this->update_status_;
}

void TrumaiNetBoxAppAirconManual::create_update_data(StatusFrame *response, uint8_t *response_len,
                                                     uint8_t command_counter) {
  status_frame_create_empty(response, STATUS_FRAME_AIRCON_MANUAL_RESPONSE, sizeof(StatusFrameAirconManualResponse),
                            command_counter);

  response->airconManualResponse.mode = this->update_status_.mode;
  response->airconManualResponse.unknown_02 = 0x00;
  response->airconManualResponse.vent_mode = this->update_status_.vent_mode;
  response->airconManualResponse.aircon_on = 0x01;  // Must always be 1
  response->airconManualResponse.target_temp_aircon = this->update_status_.target_temp_aircon;
  // Zero out padding bytes
  memset(response->airconManualResponse.padding, 0x00, sizeof(response->airconManualResponse.padding));

  status_frame_calculate_checksum(response);
  (*response_len) = sizeof(StatusFrameHeader) + sizeof(StatusFrameAirconManualResponse);

  TrumaStausFrameResponseStorage<StatusFrameAirconManual, StatusFrameAirconManualResponse>::update_submitted();
}

void TrumaiNetBoxAppAirconManual::dump_data() const {}

bool TrumaiNetBoxAppAirconManual::can_update() {
  return TrumaStausFrameResponseStorage<StatusFrameAirconManual, StatusFrameAirconManualResponse>::can_update() &&
         this->parent_->get_aircon_device() != TRUMA_DEVICE::UNKNOWN;
}

bool TrumaiNetBoxAppAirconManual::action_set_temp(uint8_t temperature) {
  if (!this->can_update()) {
    ESP_LOGW(TAG, "Cannot update Truma aircon.");
    return false;
  }

  auto update_data = this->update_prepare();
  update_data->target_temp_aircon = decimal_to_aircon_manual_temp(temperature);

  this->update_submit();
  return true;
}

bool TrumaiNetBoxAppAirconManual::action_set_mode(AirconMode mode) {
  if (!this->can_update()) {
    ESP_LOGW(TAG, "Cannot update Truma aircon.");
    return false;
  }

  auto update_data = this->update_prepare();
  update_data->mode = mode;

  // When turning off, set vent mode to low as default
  if (mode == AirconMode::AIRCON_MODE_OFF) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_LOW;
  }
  // When setting auto mode, vent must also be auto
  if (mode == AirconMode::AIRCON_MODE_AUTO) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_AUTO;
  }
  // For vent/cool/hot modes, ensure vent mode is not auto (use low if currently auto)
  if ((mode == AirconMode::AIRCON_MODE_VENTILATION || mode == AirconMode::AIRCON_MODE_COOLING ||
       mode == AirconMode::AIRCON_MODE_HEATING) &&
      update_data->vent_mode == AirconVentMode::AIRCON_VENT_AUTO) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_LOW;
  }

  this->update_submit();
  return true;
}

bool TrumaiNetBoxAppAirconManual::action_set_vent_mode(AirconVentMode vent_mode) {
  if (!this->can_update()) {
    ESP_LOGW(TAG, "Cannot update Truma aircon.");
    return false;
  }

  auto update_data = this->update_prepare();

  // Vent auto mode is only valid when operating mode is auto
  if (vent_mode == AirconVentMode::AIRCON_VENT_AUTO) {
    update_data->mode = AirconMode::AIRCON_MODE_AUTO;
  }
  // For non-auto vent modes, ensure mode is not off
  if (vent_mode != AirconVentMode::AIRCON_VENT_AUTO &&
      update_data->mode == AirconMode::AIRCON_MODE_OFF) {
    update_data->mode = AirconMode::AIRCON_MODE_VENTILATION;
  }
  // For non-auto vent modes, ensure mode is not auto
  if (vent_mode != AirconVentMode::AIRCON_VENT_AUTO &&
      update_data->mode == AirconMode::AIRCON_MODE_AUTO) {
    update_data->mode = AirconMode::AIRCON_MODE_COOLING;
  }

  update_data->vent_mode = vent_mode;

  this->update_submit();
  return true;
}

bool TrumaiNetBoxAppAirconManual::action_aircon_manual(uint8_t temperature, AirconMode mode, AirconVentMode vent_mode) {
  if (!this->can_update()) {
    ESP_LOGW(TAG, "Cannot update Truma aircon.");
    return false;
  }

  auto update_data = this->update_prepare();

  update_data->mode = mode;
  update_data->vent_mode = vent_mode;
  update_data->target_temp_aircon = decimal_to_aircon_manual_temp(temperature);

  // Validate mode/vent_mode combinations
  if (mode == AirconMode::AIRCON_MODE_OFF) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_LOW;
  }
  if (mode == AirconMode::AIRCON_MODE_AUTO) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_AUTO;
  }
  if ((mode == AirconMode::AIRCON_MODE_VENTILATION || mode == AirconMode::AIRCON_MODE_COOLING ||
       mode == AirconMode::AIRCON_MODE_HEATING) &&
      update_data->vent_mode == AirconVentMode::AIRCON_VENT_AUTO) {
    update_data->vent_mode = AirconVentMode::AIRCON_VENT_LOW;
  }

  this->update_submit();
  return true;
}

}  // namespace truma_inetbox
}  // namespace esphome