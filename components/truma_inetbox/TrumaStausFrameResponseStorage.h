#pragma once

#include "TrumaStausFrameStorage.h"
#include "TrumaStructs.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace truma_inetbox {

class TrumaiNetBoxApp;

template<typename T, typename TResponse>
class TrumaStausFrameResponseStorage : public TrumaStausFrameStorage<T>, public Parented<TrumaiNetBoxApp> {
 public:
  void reset() override {
    TrumaStausFrameStorage<T>::reset();
    this->update_status_prepared_ = false;
    this->update_status_unsubmitted_ = false;
    this->update_status_stale_ = false;
  }
  virtual bool can_update() { return this->data_valid_; }
  virtual TResponse *update_prepare() = 0;
  void update_submit() { this->update_status_unsubmitted_ = true; }
  bool has_update() const { return this->update_status_unsubmitted_; }
  void set_status(T val) override {
    if (this->update_status_unsubmitted_) {
      // Command queued but not yet sent to CP Plus: suppress publishing entirely.
      this->data_ = val;
      this->data_valid_ = true;
      return;
    }
    if (this->update_status_stale_ && (millis() - this->update_submitted_at_ < STALE_TIMEOUT_MS)) {
      // Command sent to CP Plus but not yet confirmed: store data and schedule publish
      // so the confirmed state reaches the UI on the next update() cycle.
      this->data_ = val;
      this->data_valid_ = true;
      this->data_updated_ = true;
      this->update_status_stale_ = false;
      return;
    }
    this->update_status_stale_ = false;
    TrumaStausFrameStorage<T>::set_status(val);
  };
  virtual void create_update_data(StatusFrame *response, uint8_t *response_len, uint8_t command_counter) = 0;

 protected:
  inline void update_submitted() {
    this->update_status_prepared_ = false;
    this->update_status_unsubmitted_ = false;
    this->update_status_stale_ = true;
    this->update_submitted_at_ = millis();
  }

  static constexpr uint32_t STALE_TIMEOUT_MS = 30000;
  // Prepared means `update_status_` was copied from `data_`.
  bool update_status_prepared_ = false;
  // Unsubmitted means an update is already awaiting fetch from CP Plus.
  bool update_status_unsubmitted_ = false;
  // Submitted to CP Plus, but have not yet received an update with new heater values from CP Plus.
  bool update_status_stale_ = false;
  // Timestamp when the command was submitted, used to expire `update_status_stale_`.
  uint32_t update_submitted_at_ = 0;
  TResponse update_status_;
};

}  // namespace truma_inetbox
}  // namespace esphome