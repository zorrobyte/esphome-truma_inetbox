#include "ember_onecontrol.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

namespace espbt = esp32_ble_tracker;

static const char *TAG = "ember_onecontrol";

// Static instance pointer for GATT callback routing
EmberOneControl *EmberOneControl::instance_ = nullptr;

// Helper: convert a string UUID to esp_bt_uuid_t (128-bit)
static esp_bt_uuid_t uuid_from_string(const char *uuid_str) {
  auto uuid = espbt::ESPBTUUID::from_raw(std::string(uuid_str));
  return uuid.as_128bit().get_uuid();
}

// Helper: find a characteristic handle by service + char UUID
static uint16_t find_char_handle(esp_gatt_if_t gattc_if, uint16_t conn_id,
                                 const char *svc_uuid_str, const char *char_uuid_str) {
  esp_bt_uuid_t svc_uuid = uuid_from_string(svc_uuid_str);
  esp_bt_uuid_t char_uuid = uuid_from_string(char_uuid_str);
  uint16_t count = 1;
  esp_gattc_char_elem_t result;
  memset(&result, 0, sizeof(result));
  esp_err_t status = esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id, svc_uuid, char_uuid, &result, &count);
  if (status == ESP_OK && count > 0) {
    return result.char_handle;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void EmberOneControl::setup() {
  ESP_LOGI(TAG, "Setting up Ember OneControl BLE hub...");
  EmberOneControl::instance_ = this;

  // Try to load a previously paired MAC from NVS
  if (this->load_mac_from_nvs_(this->panel_mac_)) {
    this->has_saved_mac_ = true;
    ESP_LOGI(TAG, "Loaded saved panel MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             this->panel_mac_[0], this->panel_mac_[1], this->panel_mac_[2],
             this->panel_mac_[3], this->panel_mac_[4], this->panel_mac_[5]);
    this->set_auth_state_(AuthState::SCANNING);
  } else {
    ESP_LOGI(TAG, "No saved panel MAC — press Pair button to begin pairing");
    this->set_auth_state_(AuthState::DISCONNECTED);
  }

  // Register GATT callback BEFORE app_register (ESP-IDF requirement)
  esp_ble_gattc_register_callback(EmberOneControl::gattc_event_handler_static_);

  // Register GATT client application — callback will fire ESP_GATTC_REG_EVT
  esp_err_t ret = esp_ble_gattc_app_register(0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "GATT client app_register failed: %s", esp_err_to_name(ret));
  }
}

void EmberOneControl::loop() {
  // Check pairing timeout
  if (this->pairing_active_ && (millis() - this->pairing_start_time_ > PAIRING_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "Pairing timeout reached (%u ms), stopping pairing", PAIRING_TIMEOUT_MS);
    this->stop_pairing();
  }

  // Delayed re-read of UNLOCK_STATUS after key write (500ms delay)
  if (this->unlock_reread_pending_ && (millis() - this->unlock_reread_time_ >= 500)) {
    this->unlock_reread_pending_ = false;
    if (this->unlock_status_handle_ != 0 && this->gattc_if_ != 0 && this->conn_id_ != 0) {
      ESP_LOGI(TAG, "Re-reading UNLOCK_STATUS after key write...");
      esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, this->unlock_status_handle_,
                              ESP_GATT_AUTH_REQ_NONE);
    }
  }
}

void EmberOneControl::update() {
  if (this->auth_state_ == AuthState::AUTHENTICATED) {
    ESP_LOGD(TAG, "Status: authenticated, voltage=%.2fV", this->battery_voltage_);
  } else {
    ESP_LOGD(TAG, "Status: auth_state=%d, has_mac=%s, pairing=%s",
             (int) this->auth_state_, this->has_saved_mac_ ? "yes" : "no",
             this->pairing_active_ ? "yes" : "no");
  }
}

void EmberOneControl::dump_config() {
  ESP_LOGCONFIG(TAG, "Ember OneControl BLE Hub:");
  ESP_LOGCONFIG(TAG, "  PIN: %s****", this->pin_.substr(0, 2).c_str());
  ESP_LOGCONFIG(TAG, "  Auth State: %d", (int) this->auth_state_);
  if (this->has_saved_mac_) {
    ESP_LOGCONFIG(TAG, "  Saved MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                  this->panel_mac_[0], this->panel_mac_[1], this->panel_mac_[2],
                  this->panel_mac_[3], this->panel_mac_[4], this->panel_mac_[5]);
  } else {
    ESP_LOGCONFIG(TAG, "  Saved MAC: (none — not paired)");
  }
}

// ---------------------------------------------------------------------------
// Auth state management
// ---------------------------------------------------------------------------

void EmberOneControl::set_auth_state_(AuthState state) {
  if (this->auth_state_ == state) return;
  ESP_LOGI(TAG, "Auth state: %d -> %d", (int) this->auth_state_, (int) state);
  this->auth_state_ = state;
  for (auto &cb : this->auth_state_callbacks_) {
    cb(state);
  }
}

// ---------------------------------------------------------------------------
// ESPBTDeviceListener — advertisement scanner
// ---------------------------------------------------------------------------

bool EmberOneControl::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  // If already connecting/connected/authenticated, ignore advertisements
  if (this->auth_state_ >= AuthState::CONNECTING) {
    return false;
  }

  // Reconnect to saved MAC
  if (this->has_saved_mac_ && !this->pairing_active_) {
    // Compare advertisement address to saved MAC
    auto addr = device.address_uint64();
    // Convert panel_mac_ (6 bytes big-endian) to uint64_t for comparison
    uint64_t saved = 0;
    for (int i = 0; i < 6; i++) {
      saved = (saved << 8) | this->panel_mac_[i];
    }
    if (addr == saved) {
      ESP_LOGI(TAG, "Found saved panel, connecting...");
      this->connect_to_device_(this->panel_mac_);
      return true;
    }
    return false;
  }

  // Pairing mode: look for Lippert manufacturer data
  if (this->pairing_active_) {
    for (auto &mfr_data : device.get_manufacturer_datas()) {
      if (mfr_data.uuid == espbt::ESPBTUUID::from_uint16(LIPPERT_MANUFACTURER_ID)) {
        if (mfr_data.data.empty()) continue;
        uint8_t pairing_info = mfr_data.data[0];
        // Check if pairing-active bit (bit 1) is set
        if (pairing_info & 0x02) {
          ESP_LOGI(TAG, "Found Lippert panel in pairing mode!");

          // Extract MAC from the advertisement
          auto raw_addr = device.address_uint64();
          // Convert uint64_t to esp_bd_addr_t (6 bytes, big-endian)
          uint8_t mac[6];
          for (int i = 5; i >= 0; i--) {
            mac[i] = raw_addr & 0xFF;
            raw_addr >>= 8;
          }

          // Save MAC
          memcpy(this->panel_mac_, mac, 6);
          this->has_saved_mac_ = true;
          this->save_mac_to_nvs_(mac);
          this->pairing_active_ = false;

          ESP_LOGI(TAG, "Paired with panel: %02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

          this->connect_to_device_(this->panel_mac_);
          return true;
        }
      }
    }
  }

  return false;
}

// ---------------------------------------------------------------------------
// Pairing control
// ---------------------------------------------------------------------------

void EmberOneControl::start_pairing() {
  ESP_LOGI(TAG, "Starting pairing mode (timeout=%u ms)...", PAIRING_TIMEOUT_MS);
  this->pairing_active_ = true;
  this->pairing_start_time_ = millis();
  this->set_auth_state_(AuthState::SCANNING);
}

void EmberOneControl::stop_pairing() {
  ESP_LOGI(TAG, "Stopping pairing mode");
  this->pairing_active_ = false;
  if (this->auth_state_ == AuthState::SCANNING && !this->has_saved_mac_) {
    this->set_auth_state_(AuthState::DISCONNECTED);
  }
}

void EmberOneControl::clear_paired_device() {
  ESP_LOGI(TAG, "Clearing paired device");
  this->disconnect_();
  this->has_saved_mac_ = false;
  memset(this->panel_mac_, 0, sizeof(this->panel_mac_));

  // Clear NVS
  SavedPanelConfig cfg{};
  cfg.valid = false;
  this->pref_.save(&cfg);
  global_preferences->sync();

  this->set_auth_state_(AuthState::DISCONNECTED);
}

// ---------------------------------------------------------------------------
// GATT client — static callback router
// ---------------------------------------------------------------------------

void EmberOneControl::gattc_event_handler_static_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                                    esp_ble_gattc_cb_param_t *param) {
  if (EmberOneControl::instance_ != nullptr) {
    EmberOneControl::instance_->gattc_event_handler_(event, gattc_if, param);
  }
}

// ---------------------------------------------------------------------------
// GATT client — event handler
// ---------------------------------------------------------------------------

void EmberOneControl::gattc_event_handler_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                             esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_REG_EVT: {
      if (param->reg.status == ESP_GATT_OK) {
        this->gattc_if_ = gattc_if;
        this->gattc_registered_ = true;
        ESP_LOGI(TAG, "GATT client registered (if=%d)", gattc_if);
        // If we already have a saved MAC, connect now
        if (this->has_saved_mac_) {
          this->connect_to_device_(this->panel_mac_);
        }
      } else {
        ESP_LOGE(TAG, "GATT client registration failed: %d", param->reg.status);
      }
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        this->conn_id_ = param->open.conn_id;
        ESP_LOGI(TAG, "Connected (conn_id=%d), discovering services...", this->conn_id_);
        this->set_auth_state_(AuthState::CONNECTED);
        esp_ble_gattc_search_service(this->gattc_if_, this->conn_id_, nullptr);
      } else {
        ESP_LOGW(TAG, "Connection failed: status=%d", param->open.status);
        this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
      }
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Service search failed: %d", param->search_cmpl.status);
        this->disconnect_();
        break;
      }
      ESP_LOGI(TAG, "Service discovery complete, finding characteristics...");

      // Find Auth service characteristics
      this->unlock_status_handle_ = find_char_handle(this->gattc_if_, this->conn_id_,
                                                      AUTH_SERVICE_UUID, UNLOCK_STATUS_CHAR_UUID);
      this->key_handle_ = find_char_handle(this->gattc_if_, this->conn_id_,
                                            AUTH_SERVICE_UUID, KEY_CHAR_UUID);
      this->seed_handle_ = find_char_handle(this->gattc_if_, this->conn_id_,
                                             AUTH_SERVICE_UUID, SEED_CHAR_UUID);

      // Find Data service characteristics
      this->data_write_handle_ = find_char_handle(this->gattc_if_, this->conn_id_,
                                                   DATA_SERVICE_UUID, DATA_WRITE_CHAR_UUID);
      this->data_read_handle_ = find_char_handle(this->gattc_if_, this->conn_id_,
                                                  DATA_SERVICE_UUID, DATA_READ_CHAR_UUID);

      ESP_LOGI(TAG, "Handles: unlock_status=0x%04X, key=0x%04X, seed=0x%04X, data_write=0x%04X, data_read=0x%04X",
               this->unlock_status_handle_, this->key_handle_, this->seed_handle_,
               this->data_write_handle_, this->data_read_handle_);

      // Start authentication
      this->start_authentication_();
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Read characteristic failed: status=%d handle=0x%04X",
                 param->read.status, param->read.handle);
        break;
      }
      if (param->read.handle == this->unlock_status_handle_) {
        this->handle_unlock_status_read_(param->read.value, param->read.value_len);
      }
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Write characteristic failed: status=%d handle=0x%04X",
                 param->write.status, param->write.handle);
        break;
      }
      if (param->write.handle == this->key_handle_) {
        ESP_LOGI(TAG, "KEY write confirmed, scheduling UNLOCK_STATUS re-read in 500ms...");
        this->unlock_reread_pending_ = true;
        this->unlock_reread_time_ = millis();
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle == this->seed_handle_) {
        this->handle_seed_notification_(param->notify.value, param->notify.value_len);
      } else if (param->notify.handle == this->data_read_handle_) {
        // Feed bytes into COBS decoder
        for (uint16_t i = 0; i < param->notify.value_len; i++) {
          if (this->cobs_decoder_.decode_byte(param->notify.value[i])) {
            this->process_decoded_frame_(this->cobs_decoder_.get_frame());
          }
        }
      } else {
        ESP_LOGD(TAG, "Notification on unknown handle 0x%04X (%d bytes)",
                 param->notify.handle, param->notify.value_len);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Register for notify failed: status=%d handle=0x%04X",
                 param->reg_for_notify.status, param->reg_for_notify.handle);
        break;
      }
      ESP_LOGI(TAG, "Notifications registered for handle 0x%04X", param->reg_for_notify.handle);
      if (param->reg_for_notify.handle == this->data_read_handle_) {
        // Data notifications ready — request device metadata
        this->send_get_devices_metadata_();
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "Disconnected (reason=0x%X)", param->disconnect.reason);
      this->conn_id_ = 0;
      this->unlock_status_handle_ = 0;
      this->key_handle_ = 0;
      this->seed_handle_ = 0;
      this->data_write_handle_ = 0;
      this->data_read_handle_ = 0;
      this->unlock_reread_pending_ = false;
      this->cobs_decoder_.reset();
      this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
      break;
    }

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void EmberOneControl::connect_to_device_(const esp_bd_addr_t addr) {
  if (!this->gattc_registered_) {
    // Save MAC and wait for REG_EVT to connect
    memcpy(this->panel_mac_, addr, 6);
    ESP_LOGD(TAG, "GATT not yet registered, will connect after registration");
    return;
  }
  this->set_auth_state_(AuthState::CONNECTING);
  ESP_LOGI(TAG, "Opening GATT connection to %02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  esp_ble_gattc_open(this->gattc_if_, const_cast<uint8_t *>(addr), BLE_ADDR_TYPE_PUBLIC, true);
}

void EmberOneControl::disconnect_() {
  if (this->conn_id_ != 0 && this->gattc_if_ != 0) {
    esp_ble_gattc_close(this->gattc_if_, this->conn_id_);
  }
  this->conn_id_ = 0;
  this->unlock_status_handle_ = 0;
  this->key_handle_ = 0;
  this->seed_handle_ = 0;
  this->data_write_handle_ = 0;
  this->data_read_handle_ = 0;
  this->unlock_reread_pending_ = false;
  this->cobs_decoder_.reset();
  this->set_auth_state_(AuthState::DISCONNECTED);
}

// ---------------------------------------------------------------------------
// Auth flow
// ---------------------------------------------------------------------------

void EmberOneControl::start_authentication_() {
  if (this->unlock_status_handle_ == 0) {
    ESP_LOGW(TAG, "UNLOCK_STATUS characteristic not found, trying direct notification enable");
    this->set_auth_state_(AuthState::AUTHENTICATED);
    this->enable_data_notifications_();
    return;
  }

  ESP_LOGI(TAG, "Step 1: Reading UNLOCK_STATUS to get challenge...");
  this->set_auth_state_(AuthState::READING_UNLOCK_STATUS);
  esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, this->unlock_status_handle_,
                          ESP_GATT_AUTH_REQ_NONE);
}

void EmberOneControl::handle_unlock_status_read_(const uint8_t *data, size_t len) {
  // Check if response is "Unlocked" text
  if (len >= 8) {
    std::string text((char *) data, len);
    if (text.find("Unlocked") != std::string::npos || text.find("unlocked") != std::string::npos) {
      ESP_LOGI(TAG, "Gateway already unlocked!");
      this->set_auth_state_(AuthState::AUTHENTICATED);
      this->enable_data_notifications_();
      return;
    }
  }

  if (len == 4) {
    // 4-byte challenge
    ESP_LOGI(TAG, "Received challenge: %02X %02X %02X %02X", data[0], data[1], data[2], data[3]);

    if (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0) {
      ESP_LOGW(TAG, "Challenge is all zeros - gateway not ready");
      return;
    }

    // Calculate unlock response (big-endian TEA encrypt)
    uint8_t response[4];
    calculate_unlock_response(data, response);

    ESP_LOGI(TAG, "Step 2: Writing unlock response: %02X %02X %02X %02X",
             response[0], response[1], response[2], response[3]);

    this->set_auth_state_(AuthState::UNLOCK_KEY_SENT);
    esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->key_handle_,
                             4, response, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    // Schedule re-read via loop() timer (WRITE_TYPE_NO_RSP may not trigger WRITE_CHAR_EVT)
    this->unlock_reread_pending_ = true;
    this->unlock_reread_time_ = millis();
  } else if (this->auth_state_ == AuthState::UNLOCK_KEY_SENT || this->auth_state_ == AuthState::AUTH_KEY_SENT) {
    ESP_LOGI(TAG, "Unlock status after key write: %d bytes (waiting for SEED notification)", (int) len);
    this->set_auth_state_(AuthState::WAITING_FOR_SEED);
    // Register for SEED notifications
    if (this->seed_handle_ != 0) {
      esp_ble_gattc_register_for_notify(this->gattc_if_, this->panel_mac_, this->seed_handle_);
    }
    this->enable_data_notifications_();
  } else {
    ESP_LOGW(TAG, "Unexpected UNLOCK_STATUS response: %d bytes", (int) len);
    this->enable_data_notifications_();
  }
}

void EmberOneControl::handle_seed_notification_(const uint8_t *data, size_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "SEED notification too short: %d bytes", (int) len);
    return;
  }

  ESP_LOGI(TAG, "Received SEED: %02X %02X %02X %02X", data[0], data[1], data[2], data[3]);

  // Build 16-byte auth key from seed
  build_auth_key(data, this->pin_, TEA_CYPHER, this->auth_key_);

  ESP_LOGI(TAG, "Writing 16-byte auth key to KEY characteristic...");
  this->set_auth_state_(AuthState::AUTH_KEY_SENT);
  esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->key_handle_,
                           16, this->auth_key_, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  // Schedule re-read of UNLOCK_STATUS to verify auth
  this->unlock_reread_pending_ = true;
  this->unlock_reread_time_ = millis();
}

void EmberOneControl::enable_data_notifications_() {
  if (this->data_read_handle_ == 0) {
    ESP_LOGW(TAG, "DATA_READ characteristic not found");
    return;
  }

  ESP_LOGI(TAG, "Enabling DATA_READ notifications...");
  esp_ble_gattc_register_for_notify(this->gattc_if_, this->panel_mac_, this->data_read_handle_);
}

void EmberOneControl::send_get_devices_metadata_() {
  if (this->data_write_handle_ == 0) return;

  uint16_t cmd_id = this->command_id_counter_++;
  uint8_t table_id = (this->device_table_id_ != 0) ? this->device_table_id_ : 0x01;
  auto cmd = build_get_devices_metadata(cmd_id, table_id);
  this->send_command_(cmd);
  ESP_LOGI(TAG, "Sent GetDevicesMetadata (table=0x%02X, cmdId=0x%04X)", table_id, cmd_id);
}

void EmberOneControl::send_switch_command(uint8_t table_id, uint8_t device_id, bool turn_on) {
  if (this->auth_state_ != AuthState::AUTHENTICATED || this->data_write_handle_ == 0) {
    ESP_LOGW(TAG, "Cannot send command - not authenticated");
    return;
  }

  uint16_t cmd_id = this->command_id_counter_++;
  std::vector<uint8_t> device_ids = {device_id};
  auto cmd = build_action_switch(cmd_id, table_id, turn_on, device_ids);
  this->send_command_(cmd);
  ESP_LOGI(TAG, "Sent ActionSwitch (table=0x%02X, device=0x%02X, %s)", table_id, device_id,
           turn_on ? "ON" : "OFF");
}

void EmberOneControl::send_command_(const std::vector<uint8_t> &raw_command) {
  auto encoded = cobs_encode(raw_command.data(), raw_command.size());

  if (this->data_write_handle_ == 0 || this->gattc_if_ == 0 || this->conn_id_ == 0) {
    ESP_LOGW(TAG, "Cannot send command - not connected");
    return;
  }

  esp_err_t ret = esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->data_write_handle_,
                                            encoded.size(), encoded.data(),
                                            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "GATT write command failed: %s", esp_err_to_name(ret));
  }
}

// ---------------------------------------------------------------------------
// NVS persistence
// ---------------------------------------------------------------------------

void EmberOneControl::save_mac_to_nvs_(const uint8_t *mac) {
  SavedPanelConfig cfg{};
  memcpy(cfg.mac, mac, 6);
  cfg.valid = true;
  this->pref_ = global_preferences->make_preference<SavedPanelConfig>(fnv1_hash("ember_oc_mac"));
  this->pref_.save(&cfg);
  global_preferences->sync();
  ESP_LOGI(TAG, "Saved panel MAC to NVS: %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool EmberOneControl::load_mac_from_nvs_(uint8_t *mac) {
  this->pref_ = global_preferences->make_preference<SavedPanelConfig>(fnv1_hash("ember_oc_mac"));
  SavedPanelConfig cfg{};
  if (!this->pref_.load(&cfg)) {
    ESP_LOGD(TAG, "No saved panel config in NVS");
    return false;
  }
  if (!cfg.valid) {
    ESP_LOGD(TAG, "Saved panel config marked invalid");
    return false;
  }
  memcpy(mac, cfg.mac, 6);
  return true;
}

// ---------------------------------------------------------------------------
// Event processing (working — carried over from previous implementation)
// ---------------------------------------------------------------------------

void EmberOneControl::process_decoded_frame_(const std::vector<uint8_t> &frame) {
  if (frame.empty()) return;

  uint8_t event_type = frame[0];
  const uint8_t *data = frame.data();
  size_t len = frame.size();

  ESP_LOGD(TAG, "Event type: 0x%02X (%d bytes)", event_type, len);

  switch (event_type) {
    case EVENT_GATEWAY_INFORMATION:
      this->handle_gateway_info_(data, len);
      // Receiving data proves auth is working
      if (this->auth_state_ != AuthState::AUTHENTICATED) {
        this->set_auth_state_(AuthState::AUTHENTICATED);
      }
      break;
    case EVENT_RELAY_LATCHING_STATUS_TYPE1:
    case EVENT_RELAY_LATCHING_STATUS_TYPE2:
      this->handle_relay_status_(data, len);
      break;
    case EVENT_RV_STATUS:
      this->handle_rv_status_(data, len);
      break;
    case EVENT_TANK_SENSOR_STATUS:
      this->handle_tank_status_(data, len);
      break;
    case EVENT_TANK_SENSOR_STATUS_V2:
      this->handle_tank_status_v2_(data, len);
      break;
    case EVENT_HBRIDGE_STATUS_TYPE1:
    case EVENT_HBRIDGE_STATUS_TYPE2:
      this->handle_hbridge_status_(data, len);
      break;
    default:
      ESP_LOGD(TAG, "Unhandled event: 0x%02X", event_type);
      break;
  }
}

void EmberOneControl::handle_gateway_info_(const uint8_t *data, size_t len) {
  if (len < 5) return;
  this->device_table_id_ = data[4];
  ESP_LOGI(TAG, "Gateway info: tableId=0x%02X, devices=%d", data[4], data[3]);
}

void EmberOneControl::handle_relay_status_(const uint8_t *data, size_t len) {
  if (len < 4) return;
  uint8_t table_id = data[1];
  uint8_t device_id = data[2];
  uint8_t status = data[3];
  bool is_on = (status & 0x0F) == 1;
  DeviceAddress addr = (table_id << 8) | device_id;

  ESP_LOGD(TAG, "Relay status: 0x%04X %s", addr, is_on ? "ON" : "OFF");
  for (auto &cb : this->relay_callbacks_) {
    cb(addr, is_on);
  }
}

void EmberOneControl::handle_rv_status_(const uint8_t *data, size_t len) {
  if (len < 6) return;

  // Voltage: unsigned 8.8 fixed point, big-endian
  uint16_t voltage_raw = (data[1] << 8) | data[2];
  if (voltage_raw != 0xFFFF) {
    this->battery_voltage_ = voltage_raw / 256.0f;

    // Determine battery status text
    if (this->battery_voltage_ >= 12.5f)
      this->battery_status_ = "Charged";
    else if (this->battery_voltage_ >= 12.2f)
      this->battery_status_ = "Good";
    else if (this->battery_voltage_ >= 11.8f)
      this->battery_status_ = "Fair";
    else
      this->battery_status_ = "Low";

    ESP_LOGD(TAG, "RV Status: voltage=%.2fV (%s)", this->battery_voltage_, this->battery_status_.c_str());

    for (auto &cb : this->rv_status_callbacks_) {
      cb(this->battery_voltage_);
    }
  }
}

void EmberOneControl::handle_tank_status_(const uint8_t *data, size_t len) {
  if (len < 4 || data[0] != EVENT_TANK_SENSOR_STATUS) return;
  uint8_t table_id = data[1];

  // Parse tank data: pairs of [DeviceId, Percent]
  for (size_t i = 2; i + 1 < len; i += 2) {
    uint8_t device_id = data[i];
    int percent = data[i + 1];
    DeviceAddress addr = (table_id << 8) | device_id;

    ESP_LOGD(TAG, "Tank status: 0x%04X = %d%%", addr, percent);
    for (auto &cb : this->tank_callbacks_) {
      cb(addr, percent);
    }
  }
}

void EmberOneControl::handle_tank_status_v2_(const uint8_t *data, size_t len) {
  if (len < 4 || data[0] != EVENT_TANK_SENSOR_STATUS_V2) return;
  uint8_t table_id = data[1];
  uint8_t device_id = data[2];
  int percent = data[3];
  DeviceAddress addr = (table_id << 8) | device_id;

  ESP_LOGD(TAG, "Tank V2 status: 0x%04X = %d%%", addr, percent);
  for (auto &cb : this->tank_callbacks_) {
    cb(addr, percent);
  }
}

void EmberOneControl::handle_hbridge_status_(const uint8_t *data, size_t len) {
  if (len < 5) return;
  uint8_t table_id = data[1];
  uint8_t device_id = data[2];
  int status = data[3];
  int position = data[4];
  DeviceAddress addr = (table_id << 8) | device_id;

  ESP_LOGD(TAG, "H-Bridge status: 0x%04X status=%d pos=%d", addr, status, position);
  for (auto &cb : this->hbridge_callbacks_) {
    cb(addr, status, position);
  }
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
