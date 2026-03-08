#include "ember_onecontrol.h"

#ifdef USE_ESP32

namespace esphome {
namespace ember_onecontrol {

namespace espbt = esp32_ble_tracker;

static const char *TAG = "ember_onecontrol";

// Helper: convert a string UUID to esp_bt_uuid_t (128-bit)
static esp_bt_uuid_t uuid_from_string(const char *uuid_str) {
  auto uuid = espbt::ESPBTUUID::from_raw(std::string(uuid_str));
  return uuid.as_128bit().get_uuid();
}

// Helper: find a characteristic handle + properties by service + char UUID
struct CharInfo {
  uint16_t handle;
  uint8_t properties;
};

static CharInfo find_char_info(esp_gatt_if_t gattc_if, uint16_t conn_id,
                               const char *svc_uuid_str, const char *char_uuid_str) {
  esp_bt_uuid_t svc_uuid = uuid_from_string(svc_uuid_str);
  esp_bt_uuid_t char_uuid = uuid_from_string(char_uuid_str);

  // First find the service to get its handle range
  uint16_t svc_count = 1;
  esp_gattc_service_elem_t svc_result;
  memset(&svc_result, 0, sizeof(svc_result));
  esp_err_t status = esp_ble_gattc_get_service(gattc_if, conn_id, &svc_uuid, &svc_result, &svc_count, 0);
  if (status != ESP_OK || svc_count == 0) {
    ESP_LOGW(TAG, "Service not found for char lookup: %s", svc_uuid_str);
    return {0, 0};
  }

  // Now find the characteristic within that service's handle range
  uint16_t char_count = 1;
  esp_gattc_char_elem_t char_result;
  memset(&char_result, 0, sizeof(char_result));
  status = esp_ble_gattc_get_char_by_uuid(gattc_if, conn_id,
                                            svc_result.start_handle, svc_result.end_handle,
                                            char_uuid, &char_result, &char_count);
  if (status == ESP_OK && char_count > 0) {
    ESP_LOGI(TAG, "Char %s: handle=0x%04X properties=0x%02X (N=%d I=%d)",
             char_uuid_str, char_result.char_handle, char_result.properties,
             (char_result.properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) ? 1 : 0,
             (char_result.properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) ? 1 : 0);
    return {char_result.char_handle, (uint8_t) char_result.properties};
  }
  ESP_LOGW(TAG, "Characteristic not found: %s in service %s", char_uuid_str, svc_uuid_str);
  return {0, 0};
}

// Helper: find the CCCD descriptor handle for a characteristic
static uint16_t find_cccd_handle(esp_gatt_if_t gattc_if, uint16_t conn_id, uint16_t char_handle) {
  esp_bt_uuid_t cccd_uuid;
  cccd_uuid.len = ESP_UUID_LEN_16;
  cccd_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;  // 0x2902

  uint16_t count = 1;
  esp_gattc_descr_elem_t result;
  memset(&result, 0, sizeof(result));

  esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(
      gattc_if, conn_id, char_handle, cccd_uuid, &result, &count);

  if (status == ESP_GATT_OK && count > 0) {
    ESP_LOGD(TAG, "CCCD for char 0x%04X found at 0x%04X", char_handle, result.handle);
    return result.handle;
  }
  ESP_LOGW(TAG, "CCCD not found for char 0x%04X, using handle+1 (0x%04X)", char_handle, char_handle + 1);
  return char_handle + 1;  // Fallback: CCCD is usually the next handle
}

// Helper: write CCCD to enable notifications/indications on remote device
// Auto-detects based on characteristic properties
static void write_cccd_enable(esp_gatt_if_t gattc_if, uint16_t conn_id, uint16_t cccd_handle,
                               uint8_t char_properties) {
  uint8_t cccd_val[2];
  if (char_properties & ESP_GATT_CHAR_PROP_BIT_INDICATE) {
    // Prefer indications if available (some Ember characteristics only support indicate)
    cccd_val[0] = 0x02;
    cccd_val[1] = 0x00;
    ESP_LOGI(TAG, "Writing CCCD 0x0002 (INDICATE) to handle 0x%04X", cccd_handle);
  } else {
    // Fall back to notifications
    cccd_val[0] = 0x01;
    cccd_val[1] = 0x00;
    ESP_LOGI(TAG, "Writing CCCD 0x0001 (NOTIFY) to handle 0x%04X", cccd_handle);
  }
  esp_ble_gattc_write_char_descr(gattc_if, conn_id, cccd_handle,
                                  2, cccd_val, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void EmberOneControl::setup() {
  ESP_LOGI(TAG, "Setting up Ember OneControl BLE hub...");

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

  // Configure BLE security for bonding with Ember panel
  // PIN-based gateway (0x05C7) requires MITM + KeyboardDisplay capability.
  // The gateway PIN is used as the BLE passkey during pairing.
  esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
  esp_ble_io_cap_t io_cap = ESP_IO_CAP_KBDISP;  // KeyboardDisplay — matches Android/BlueZ agent
  uint8_t key_size = 16;
  uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
  esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io_cap, sizeof(io_cap));
  esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
  esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
  ESP_LOGI(TAG, "BLE security: SC+MITM+Bond (KeyboardDisplay, PIN-based)");

  // Log existing BLE bonds (no longer clearing them — MITM bonds should persist)
  int bond_num = esp_ble_get_bond_device_num();
  ESP_LOGI(TAG, "Stored BLE bonds: %d", bond_num);
  if (bond_num > 0) {
    esp_ble_bond_dev_t *bond_list = (esp_ble_bond_dev_t *) malloc(bond_num * sizeof(esp_ble_bond_dev_t));
    if (bond_list && esp_ble_get_bond_device_list(&bond_num, bond_list) == ESP_OK) {
      for (int i = 0; i < bond_num; i++) {
        ESP_LOGI(TAG, "  Bond[%d]: %02X:%02X:%02X:%02X:%02X:%02X", i,
                 bond_list[i].bd_addr[0], bond_list[i].bd_addr[1], bond_list[i].bd_addr[2],
                 bond_list[i].bd_addr[3], bond_list[i].bd_addr[4], bond_list[i].bd_addr[5]);
      }
    }
    free(bond_list);
  }

  // GATT app registration is deferred to loop() because ESP32BLE::ble_setup_()
  // (which registers the GATTC callback) runs in the first loop() iteration,
  // not in setup(). If we call app_register here, the REG_EVT would be lost.
}

void EmberOneControl::loop() {
  // Deferred GATTC app registration — must wait for ESP32BLE to be fully active
  if (!this->gattc_registered_ && !this->gattc_app_registered_) {
    esp_err_t ret = esp_ble_gattc_app_register(EMBER_GATTC_APP_ID);
    if (ret == ESP_OK) {
      this->gattc_app_registered_ = true;
      ESP_LOGI(TAG, "GATT client app_register OK (app_id=0x%02X)", EMBER_GATTC_APP_ID);
    } else {
      ESP_LOGW(TAG, "GATT client app_register not ready: %s (will retry)", esp_err_to_name(ret));
    }
  }

  // Check pairing timeout
  if (this->pairing_active_ && (millis() - this->pairing_start_time_ > PAIRING_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "Pairing timeout reached (%u ms), stopping pairing", PAIRING_TIMEOUT_MS);
    this->stop_pairing();
  }

  // Heartbeat: send GetDevices every 5 seconds while authenticated
  if (this->auth_state_ == AuthState::AUTHENTICATED && this->data_write_handle_ != 0) {
    uint32_t now = millis();
    if (now - this->last_heartbeat_time_ >= HEARTBEAT_INTERVAL_MS) {
      this->last_heartbeat_time_ = now;
      this->send_get_devices_();
    }
  }

  // Delayed re-read of UNLOCK_STATUS after key write (500ms delay)
  if (this->unlock_reread_pending_ && (millis() - this->unlock_reread_time_ >= 500)) {
    this->unlock_reread_pending_ = false;
    if (this->unlock_status_handle_ != 0 && this->gattc_if_ != 0 &&
        this->auth_state_ >= AuthState::CONNECTED) {
      ESP_LOGI(TAG, "Re-reading UNLOCK_STATUS after key write (conn_id=%d)...", this->conn_id_);
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
    // Debug: log all devices with manufacturer data during pairing
    for (auto &mfr_data : device.get_manufacturer_datas()) {
      ESP_LOGD(TAG, "BLE device %s mfr_id=%s data_len=%d name='%s'",
               device.address_str().c_str(),
               mfr_data.uuid.to_string().c_str(),
               (int)mfr_data.data.size(),
               device.get_name().c_str());
      if (!mfr_data.data.empty()) {
        ESP_LOGD(TAG, "  mfr_data[0]=0x%02X", mfr_data.data[0]);
      }
    }

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
// GATTcEventHandler interface — receives events from ESPHome's BLE stack
// ---------------------------------------------------------------------------

void EmberOneControl::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                           esp_ble_gattc_cb_param_t *param) {
  // Log ALL events for debugging (filter by our app only after REG)
  if (event == ESP_GATTC_REG_EVT) {
    ESP_LOGI(TAG, "[GATTC] REG_EVT: app_id=%d status=%d gattc_if=%d",
             param->reg.app_id, param->reg.status, gattc_if);
    if (param->reg.app_id != EMBER_GATTC_APP_ID) return;
  } else {
    // Log all events for our interface, with event number for unknowns
    if (gattc_if != this->gattc_if_) return;
    ESP_LOGD(TAG, "[GATTC] event=%d gattc_if=%d conn_id=%d", (int) event, (int) gattc_if, (int) this->conn_id_);
  }

  switch (event) {
    case ESP_GATTC_REG_EVT: {
      if (param->reg.status == ESP_GATT_OK) {
        this->gattc_if_ = gattc_if;
        this->gattc_registered_ = true;
        ESP_LOGI(TAG, "[GATTC] Client registered OK (if=%d)", gattc_if);
        if (this->has_saved_mac_) {
          this->connect_to_device_(this->panel_mac_);
        }
      } else {
        ESP_LOGE(TAG, "[GATTC] Client registration FAILED: %d", param->reg.status);
      }
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      ESP_LOGI(TAG, "[GATTC] OPEN_EVT: status=%d conn_id=%d mtu=%d",
               param->open.status, param->open.conn_id, param->open.mtu);
      if (param->open.status == ESP_GATT_OK) {
        this->conn_id_ = param->open.conn_id;
        this->set_auth_state_(AuthState::CONNECTED);
        // Request MITM-level encryption — required for PIN-based gateway
        // This triggers bonding with passkey exchange if no bond exists
        ESP_LOGI(TAG, "[GATTC] Requesting BLE encryption (MITM)...");
        esp_err_t ret = esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
        ESP_LOGI(TAG, "[GATTC] esp_ble_set_encryption returned: %s", esp_err_to_name(ret));
      } else {
        ESP_LOGW(TAG, "[GATTC] Connection FAILED: status=%d — cooldown 5s", param->open.status);
        this->reconnect_cooldown_until_ = millis() + 5000;
        this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
      }
      break;
    }

    case ESP_GATTC_CONNECT_EVT: {
      ESP_LOGI(TAG, "[GATTC] CONNECT_EVT: conn_id=%d link_role=%d remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
               param->connect.conn_id, param->connect.link_role,
               param->connect.remote_bda[0], param->connect.remote_bda[1],
               param->connect.remote_bda[2], param->connect.remote_bda[3],
               param->connect.remote_bda[4], param->connect.remote_bda[5]);
      break;
    }

    case ESP_GATTC_SEARCH_RES_EVT: {
      ESP_LOGI(TAG, "[GATTC] SEARCH_RES: start=0x%04X end=0x%04X uuid_len=%d",
               param->search_res.start_handle, param->search_res.end_handle,
               param->search_res.srvc_id.uuid.len);
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      ESP_LOGI(TAG, "[GATTC] SEARCH_CMPL: status=%d conn_id=%d searched_service_source=%d",
               param->search_cmpl.status, param->search_cmpl.conn_id,
               param->search_cmpl.searched_service_source);
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "[GATTC] Service search FAILED: %d", param->search_cmpl.status);
        this->disconnect_();
        break;
      }

      // Find Auth service characteristics (with properties for CCCD auto-detect)
      {
        auto unlock_info = find_char_info(this->gattc_if_, this->conn_id_,
                                           AUTH_SERVICE_UUID, UNLOCK_STATUS_CHAR_UUID);
        this->unlock_status_handle_ = unlock_info.handle;

        auto key_info = find_char_info(this->gattc_if_, this->conn_id_,
                                        AUTH_SERVICE_UUID, KEY_CHAR_UUID);
        this->key_handle_ = key_info.handle;

        auto seed_info = find_char_info(this->gattc_if_, this->conn_id_,
                                         AUTH_SERVICE_UUID, SEED_CHAR_UUID);
        this->seed_handle_ = seed_info.handle;
        this->seed_properties_ = seed_info.properties;

        auto dwrite_info = find_char_info(this->gattc_if_, this->conn_id_,
                                           DATA_SERVICE_UUID, DATA_WRITE_CHAR_UUID);
        this->data_write_handle_ = dwrite_info.handle;

        auto dread_info = find_char_info(this->gattc_if_, this->conn_id_,
                                          DATA_SERVICE_UUID, DATA_READ_CHAR_UUID);
        this->data_read_handle_ = dread_info.handle;
        this->data_read_properties_ = dread_info.properties;
      }

      ESP_LOGI(TAG, "[GATTC] Handles: unlock=0x%04X key=0x%04X seed=0x%04X(prop=0x%02X) dwrite=0x%04X dread=0x%04X(prop=0x%02X)",
               this->unlock_status_handle_, this->key_handle_,
               this->seed_handle_, this->seed_properties_,
               this->data_write_handle_,
               this->data_read_handle_, this->data_read_properties_);

      this->start_authentication_();
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      ESP_LOGI(TAG, "[GATTC] READ_CHAR: status=%d handle=0x%04X value_len=%d",
               param->read.status, param->read.handle, param->read.value_len);
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[GATTC] Read FAILED: status=%d (5=INSUF_AUTH, 14=INSUF_ENC)",
                 param->read.status);
        if (param->read.status == 5 || param->read.status == 14) {
          ESP_LOGE(TAG, "BLE encryption required — press CONNECT on Ember panel to pair!");
          this->reconnect_cooldown_until_ = millis() + 15000;
          this->disconnect_();
        }
        break;
      }
      // Log raw bytes
      if (param->read.value_len > 0 && param->read.value_len <= 32) {
        char hex[97] = {0};
        for (int i = 0; i < param->read.value_len && i < 32; i++) {
          snprintf(hex + i * 3, 4, "%02X ", param->read.value[i]);
        }
        ESP_LOGI(TAG, "[GATTC] Read data: %s", hex);
      }
      if (param->read.handle == this->unlock_status_handle_) {
        this->handle_unlock_status_read_(param->read.value, param->read.value_len);
      }
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      ESP_LOGI(TAG, "[GATTC] WRITE_CHAR: status=%d handle=0x%04X",
               param->write.status, param->write.handle);
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[GATTC] Write FAILED: status=%d", param->write.status);
        break;
      }
      if (param->write.handle == this->key_handle_) {
        if (this->auth_state_ == AuthState::UNLOCK_KEY_SENT) {
          ESP_LOGI(TAG, "[GATTC] Unlock KEY write OK — scheduling re-read in 500ms");
          this->unlock_reread_pending_ = true;
          this->unlock_reread_time_ = millis();
        } else if (this->auth_state_ == AuthState::AUTH_KEY_SENT ||
                   this->auth_state_ == AuthState::AUTHENTICATED) {
          ESP_LOGI(TAG, "[GATTC] Auth KEY write confirmed");
        }
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      ESP_LOGD(TAG, "[GATTC] NOTIFY: handle=0x%04X len=%d is_notify=%d",
               param->notify.handle, param->notify.value_len, param->notify.is_notify);
      if (param->notify.handle == this->seed_handle_) {
        this->handle_seed_notification_(param->notify.value, param->notify.value_len);
      } else if (param->notify.handle == this->data_read_handle_) {
        for (uint16_t i = 0; i < param->notify.value_len; i++) {
          if (this->cobs_decoder_.decode_byte(param->notify.value[i])) {
            this->process_decoded_frame_(this->cobs_decoder_.get_frame());
          }
        }
      } else {
        // Log first bytes of unknown notification
        if (param->notify.value_len > 0) {
          char hex[49] = {0};
          for (int i = 0; i < param->notify.value_len && i < 16; i++) {
            snprintf(hex + i * 3, 4, "%02X ", param->notify.value[i]);
          }
          ESP_LOGI(TAG, "[GATTC] Unknown notification handle=0x%04X: %s", param->notify.handle, hex);
        }
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      ESP_LOGI(TAG, "[GATTC] REG_FOR_NOTIFY: status=%d handle=0x%04X",
               param->reg_for_notify.status, param->reg_for_notify.handle);
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[GATTC] Register for notify FAILED: status=%d", param->reg_for_notify.status);
        break;
      }
      if (param->reg_for_notify.handle == this->data_read_handle_) {
        ESP_LOGI(TAG, "[GATTC] DATA_READ local registration OK — writing CCCD (properties=0x%02X)",
                 this->data_read_properties_);
        uint16_t cccd = find_cccd_handle(this->gattc_if_, this->conn_id_, this->data_read_handle_);
        write_cccd_enable(this->gattc_if_, this->conn_id_, cccd, this->data_read_properties_);
      }
      if (param->reg_for_notify.handle == this->seed_handle_) {
        ESP_LOGI(TAG, "[GATTC] SEED local registration OK — writing CCCD (properties=0x%02X)",
                 this->seed_properties_);
        uint16_t cccd = find_cccd_handle(this->gattc_if_, this->conn_id_, this->seed_handle_);
        write_cccd_enable(this->gattc_if_, this->conn_id_, cccd, this->seed_properties_);
      }
      break;
    }

    case ESP_GATTC_WRITE_DESCR_EVT: {
      ESP_LOGI(TAG, "[GATTC] WRITE_DESCR: status=%d handle=0x%04X",
               param->write.status, param->write.handle);
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "[GATTC] CCCD write FAILED: status=%d", param->write.status);
        break;
      }
      // Check if this is the DATA_READ CCCD write completion
      // Sequential flow: now subscribe to SEED after DATA_READ completes
      if (this->data_read_handle_ != 0 &&
          (param->write.handle == this->data_read_handle_ + 1 ||
           param->write.handle == this->data_read_handle_ + 2)) {
        ESP_LOGI(TAG, "[GATTC] DATA_READ CCCD enabled — now subscribing to SEED...");
        if (this->seed_handle_ != 0) {
          esp_ble_gattc_register_for_notify(this->gattc_if_, this->panel_mac_, this->seed_handle_);
        }
      }
      // Check if this is the SEED CCCD write completion
      if (this->seed_handle_ != 0 &&
          (param->write.handle == this->seed_handle_ + 1 ||
           param->write.handle == this->seed_handle_ + 2)) {
        ESP_LOGI(TAG, "[GATTC] SEED CCCD enabled on remote device — ready for Step 2");
      }
      break;
    }

    case ESP_GATTC_CFG_MTU_EVT: {
      ESP_LOGI(TAG, "[GATTC] CFG_MTU: status=%d conn_id=%d mtu=%d",
               param->cfg_mtu.status, param->cfg_mtu.conn_id, param->cfg_mtu.mtu);
      // MTU negotiated — now start service discovery
      ESP_LOGI(TAG, "[GATTC] Starting service discovery after MTU negotiation...");
      if (this->gattc_if_ != 0) {
        esp_err_t ret = esp_ble_gattc_search_service(this->gattc_if_, this->conn_id_, nullptr);
        ESP_LOGI(TAG, "[GATTC] search_service returned: %s", esp_err_to_name(ret));
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "[GATTC] DISCONNECT: reason=0x%X conn_id=%d remote_bda=%02X:%02X:%02X:%02X:%02X:%02X",
               param->disconnect.reason, param->disconnect.conn_id,
               param->disconnect.remote_bda[0], param->disconnect.remote_bda[1],
               param->disconnect.remote_bda[2], param->disconnect.remote_bda[3],
               param->disconnect.remote_bda[4], param->disconnect.remote_bda[5]);
      this->conn_id_ = 0;
      this->unlock_status_handle_ = 0;
      this->key_handle_ = 0;
      this->seed_handle_ = 0;
      this->data_write_handle_ = 0;
      this->data_read_handle_ = 0;
      this->seed_properties_ = 0;
      this->data_read_properties_ = 0;
      this->unlock_reread_pending_ = false;
      this->last_heartbeat_time_ = 0;
      this->cobs_decoder_.reset();
      this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
      break;
    }

    default:
      ESP_LOGD(TAG, "[GATTC] Unhandled event: %d", (int) event);
      break;
  }
}

// ---------------------------------------------------------------------------
// GAPEventHandler interface — security/bonding events
// ---------------------------------------------------------------------------

void EmberOneControl::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  ESP_LOGI(TAG, "[GAP] event=%d", (int) event);

  switch (event) {
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      auto &auth = param->ble_security.auth_cmpl;
      ESP_LOGI(TAG, "[GAP] AUTH_CMPL: success=%d addr=%02X:%02X:%02X:%02X:%02X:%02X auth_mode=%d",
               auth.success, auth.bd_addr[0], auth.bd_addr[1], auth.bd_addr[2],
               auth.bd_addr[3], auth.bd_addr[4], auth.bd_addr[5], auth.auth_mode);
      if (auth.success) {
        ESP_LOGI(TAG, "[GAP] Bonding SUCCESS! key_present=%d key_type=%d", auth.key_present, auth.key_type);
        // Request MTU upgrade (reference uses 185, default is 23)
        ESP_LOGI(TAG, "[GAP] Requesting MTU=185...");
        esp_ble_gattc_send_mtu_req(this->gattc_if_, this->conn_id_);
        // Service discovery will be triggered in CFG_MTU_EVT handler
      } else {
        ESP_LOGE(TAG, "[GAP] Bonding FAILED: fail_reason=0x%X auth_mode=%d",
                 auth.fail_reason, auth.auth_mode);
        // Do NOT remove bond device here — stored keys may still be valid
        // on a subsequent attempt. Only clear bonds on explicit user action.
        // Try service discovery anyway — some panels allow GATT access without bonding
        if (this->gattc_if_ != 0 && this->auth_state_ >= AuthState::CONNECTED) {
          ESP_LOGW(TAG, "[GAP] Attempting service discovery without bonding...");
          esp_ble_gattc_search_service(this->gattc_if_, this->conn_id_, nullptr);
        } else {
          this->reconnect_cooldown_until_ = millis() + 10000;
          this->disconnect_();
        }
      }
      break;
    }
    case ESP_GAP_BLE_SEC_REQ_EVT: {
      ESP_LOGI(TAG, "[GAP] SEC_REQ: peer requesting security — accepting");
      esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
      break;
    }
    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT: {
      ESP_LOGI(TAG, "[GAP] PASSKEY_NOTIF: passkey=%06lu",
               (unsigned long) param->ble_security.key_notif.passkey);
      break;
    }
    case ESP_GAP_BLE_PASSKEY_REQ_EVT: {
      // Gateway is requesting the BLE passkey — provide the gateway PIN
      uint32_t passkey = 0;
      for (size_t i = 0; i < this->pin_.length() && i < 6; i++) {
        passkey = passkey * 10 + (this->pin_[i] - '0');
      }
      ESP_LOGI(TAG, "[GAP] PASSKEY_REQ: replying with gateway PIN (%06lu)", (unsigned long) passkey);
      esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true, passkey);
      break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT: {
      ESP_LOGI(TAG, "[GAP] NC_REQ: numeric comparison passkey=%06lu — accepting",
               (unsigned long) param->ble_security.key_notif.passkey);
      esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
      break;
    }
    default:
      ESP_LOGD(TAG, "[GAP] Unhandled GAP event: %d", (int) event);
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
  // Reconnect backoff: don't spam connections after failure
  if (this->reconnect_cooldown_until_ != 0 && millis() < this->reconnect_cooldown_until_) {
    return;
  }
  this->reconnect_cooldown_until_ = 0;
  this->set_auth_state_(AuthState::CONNECTING);
  ESP_LOGI(TAG, "Opening GATT connection to %02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  esp_ble_gattc_open(this->gattc_if_, const_cast<uint8_t *>(addr), BLE_ADDR_TYPE_PUBLIC, true);
}

void EmberOneControl::disconnect_() {
  if (this->auth_state_ >= AuthState::CONNECTED && this->gattc_if_ != 0) {
    esp_ble_gattc_close(this->gattc_if_, this->conn_id_);
  }
  this->unlock_status_handle_ = 0;
  this->key_handle_ = 0;
  this->seed_handle_ = 0;
  this->data_write_handle_ = 0;
  this->data_read_handle_ = 0;
  this->seed_properties_ = 0;
  this->data_read_properties_ = 0;
  this->unlock_reread_pending_ = false;
  this->last_heartbeat_time_ = 0;
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

  // Step 1: Read UNLOCK_STATUS to get the 4-byte challenge.
  // Notifications (SEED + DATA_READ) are enabled AFTER Step 1 succeeds,
  // matching the reference HA integration auth flow.
  ESP_LOGI(TAG, "Step 1: Reading UNLOCK_STATUS to get challenge...");
  this->set_auth_state_(AuthState::READING_UNLOCK_STATUS);
  esp_ble_gattc_read_char(this->gattc_if_, this->conn_id_, this->unlock_status_handle_,
                          ESP_GATT_AUTH_REQ_NONE);
}

void EmberOneControl::handle_unlock_status_read_(const uint8_t *data, size_t len) {
  // Check if response contains "unlocked" text (ASCII) — means gateway is already unlocked
  if (len >= 4) {
    std::string text((char *) data, len);
    // Case-insensitive check
    std::string lower = text;
    for (auto &c : lower) c = tolower(c);
    if (lower.find("unlocked") != std::string::npos) {
      ESP_LOGI(TAG, "Gateway reports UNLOCKED (text response)");
      if (this->auth_state_ == AuthState::UNLOCK_KEY_SENT) {
        // Step 1 complete! Now enable notifications for SEED + DATA_READ
        ESP_LOGI(TAG, "Step 1 SUCCESS — enabling notifications for Step 2...");
        this->set_auth_state_(AuthState::WAITING_FOR_SEED);
        this->enable_data_notifications_();
      } else {
        // Already unlocked from a previous session — enable notifications directly
        ESP_LOGI(TAG, "Already unlocked — enabling notifications...");
        this->set_auth_state_(AuthState::WAITING_FOR_SEED);
        this->enable_data_notifications_();
      }
      return;
    }
  }

  if (len == 4) {
    ESP_LOGI(TAG, "UNLOCK_STATUS 4-byte response: %02X %02X %02X %02X (auth_state=%d)",
             data[0], data[1], data[2], data[3], (int) this->auth_state_);

    bool all_zeros = (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0);

    if (all_zeros) {
      // All zeros = gateway not ready (per reference implementation)
      ESP_LOGW(TAG, "UNLOCK_STATUS all zeros — gateway not ready (state=%d)", (int) this->auth_state_);
      return;
    }

    if (this->auth_state_ == AuthState::READING_UNLOCK_STATUS ||
        this->auth_state_ == AuthState::CONNECTED) {
      // Non-zero challenge — compute TEA-encrypted response with STEP1_CIPHER
      uint8_t response[4];
      calculate_unlock_response(data, response);

      ESP_LOGI(TAG, "Step 1: Challenge=%02X%02X%02X%02X → Response=%02X%02X%02X%02X",
               data[0], data[1], data[2], data[3],
               response[0], response[1], response[2], response[3]);

      this->set_auth_state_(AuthState::UNLOCK_KEY_SENT);
      esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->key_handle_,
                               4, response, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
      // Schedule re-read after 500ms to verify unlock
      this->unlock_reread_pending_ = true;
      this->unlock_reread_time_ = millis();
      return;
    }

    // Non-zero in UNLOCK_KEY_SENT state means our key was rejected
    if (this->auth_state_ == AuthState::UNLOCK_KEY_SENT) {
      ESP_LOGW(TAG, "Step 1 FAILED — challenge not consumed (key rejected?)");
      this->reconnect_cooldown_until_ = millis() + 10000;
      this->disconnect_();
      return;
    }

    ESP_LOGI(TAG, "UNLOCK_STATUS response in state %d — ignoring", (int) this->auth_state_);
  } else {
    ESP_LOGI(TAG, "UNLOCK_STATUS response: %d bytes (unexpected size)", (int) len);
  }
}

void EmberOneControl::handle_seed_notification_(const uint8_t *data, size_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "SEED notification too short: %d bytes", (int) len);
    return;
  }

  ESP_LOGI(TAG, "Received SEED: %02X %02X %02X %02X", data[0], data[1], data[2], data[3]);

  // Build 16-byte auth key from seed using Step 2 cipher (little-endian)
  build_auth_key(data, this->pin_, STEP2_CIPHER, this->auth_key_);

  ESP_LOGI(TAG, "Step 2: Writing 16-byte auth key to KEY characteristic...");
  this->set_auth_state_(AuthState::AUTH_KEY_SENT);
  esp_ble_gattc_write_char(this->gattc_if_, this->conn_id_, this->key_handle_,
                           16, this->auth_key_, ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);

  // Auth key is accepted silently — notifications are already enabled from Step 1.
  // Data events should start flowing now.
  ESP_LOGI(TAG, "Step 2: Auth key sent — authentication complete!");
  this->set_auth_state_(AuthState::AUTHENTICATED);
  // Send initial GetDevicesMetadata to populate device info
  this->send_get_devices_metadata_();
}

void EmberOneControl::enable_data_notifications_() {
  // Sequential subscription: DATA_READ first, then SEED after DATA_READ CCCD completes.
  // This matches the reference implementation which awaits each start_notify() call.
  // SEED subscription is triggered by DATA_READ's WRITE_DESCR completion event.
  if (this->data_read_handle_ != 0) {
    ESP_LOGI(TAG, "Enabling DATA_READ notifications (SEED will follow after CCCD write)...");
    esp_ble_gattc_register_for_notify(this->gattc_if_, this->panel_mac_, this->data_read_handle_);
  } else {
    ESP_LOGW(TAG, "DATA_READ characteristic not found — subscribing to SEED directly");
    if (this->seed_handle_ != 0) {
      esp_ble_gattc_register_for_notify(this->gattc_if_, this->panel_mac_, this->seed_handle_);
    }
  }
}

void EmberOneControl::send_get_devices_() {
  if (this->data_write_handle_ == 0) return;
  uint16_t cmd_id = this->command_id_counter_++;
  uint8_t table_id = (this->device_table_id_ != 0) ? this->device_table_id_ : 0x01;
  auto cmd = build_get_devices(cmd_id, table_id);
  this->send_command_(cmd);
  ESP_LOGD(TAG, "Heartbeat: GetDevices (table=0x%02X, cmdId=0x%04X)", table_id, cmd_id);
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

  if (this->data_write_handle_ == 0 || this->gattc_if_ == 0 ||
      this->auth_state_ < AuthState::CONNECTED) {
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
