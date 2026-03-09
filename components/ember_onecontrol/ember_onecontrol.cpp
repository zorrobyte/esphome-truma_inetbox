#include "ember_onecontrol.h"
#include "esphome/core/hal.h"

#ifdef USE_ESP32

#include "esp_bt.h"
#include "nvs_flash.h"

// Required for NimBLE bond storage
extern "C" void ble_store_config_init(void);

namespace esphome {
namespace ember_onecontrol {

static const char *TAG = "ember_onecontrol";

EmberOneControl *EmberOneControl::instance_ = nullptr;

// Pre-computed 128-bit UUIDs in little-endian byte order for NimBLE
static const ble_uuid128_t AUTH_SVC_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x10, 0x00, 0x00, 0x00);
static const ble_uuid128_t SEED_CHR_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x11, 0x00, 0x00, 0x00);
static const ble_uuid128_t UNLOCK_STATUS_CHR_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x12, 0x00, 0x00, 0x00);
static const ble_uuid128_t KEY_CHR_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x13, 0x00, 0x00, 0x00);
static const ble_uuid128_t DATA_SVC_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x30, 0x00, 0x00, 0x00);
static const ble_uuid128_t DATA_WRITE_CHR_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x33, 0x00, 0x00, 0x00);
static const ble_uuid128_t DATA_READ_CHR_UUID = BLE_UUID128_INIT(
    0x2c, 0xe6, 0x44, 0x80, 0xe2, 0xaf, 0x11, 0xe4,
    0x8e, 0xa5, 0x00, 0x02, 0x34, 0x00, 0x00, 0x00);

// ---------------------------------------------------------------------------
// C callback wrappers (NimBLE uses C function pointers)
// ---------------------------------------------------------------------------

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gap_event_(event);
  }
  return 0;
}

static int gatt_discsvc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_discsvc_cb_(conn_handle, error, service);
  }
  return 0;
}

static int gatt_discchr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_discchr_cb_(conn_handle, error, chr);
  }
  return 0;
}

static int gatt_discdsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_discdsc_cb_(conn_handle, error, chr_val_handle, dsc);
  }
  return 0;
}

static int gatt_read_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_read_cb_(conn_handle, error, attr);
  }
  return 0;
}

static int gatt_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_write_cb_(conn_handle, error, attr);
  }
  return 0;
}

static int gatt_notify_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct os_mbuf *om, void *arg) {
  if (EmberOneControl::instance_) {
    return EmberOneControl::instance_->handle_gatt_notify_cb_(conn_handle, attr_handle, om);
  }
  return 0;
}

static void nimble_host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void on_nimble_sync() {
  // Host has synced with controller — ready to scan/connect
  if (EmberOneControl::instance_) {
    EmberOneControl::instance_->handle_gap_event_(nullptr);  // signal sync via nullptr
  }
}

static void on_nimble_reset(int reason) {
  ESP_LOGE(TAG, "NimBLE host reset: reason=%d", reason);
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void EmberOneControl::setup() {
  ESP_LOGI(TAG, "Setting up Ember OneControl (NimBLE)...");
  instance_ = this;

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
}

void EmberOneControl::loop() {
  // Start NimBLE on first loop iteration (after ESPHome has initialized everything)
  if (!this->nimble_started_) {
    this->start_nimble_();
    return;
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
    if (this->unlock_status_handle_ != 0 && this->connected_ &&
        this->auth_state_ >= AuthState::CONNECTED) {
      ESP_LOGD(TAG, "Re-reading UNLOCK_STATUS after key write");
      ble_gattc_read(this->conn_handle_, this->unlock_status_handle_, gatt_read_cb, nullptr);
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
  ESP_LOGCONFIG(TAG, "Ember OneControl BLE Hub (NimBLE):");
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
// NimBLE initialization
// ---------------------------------------------------------------------------

void EmberOneControl::start_nimble_() {
  ESP_LOGI(TAG, "Initializing NimBLE stack...");

  esp_err_t ret = nimble_port_init();
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init() failed: %s", esp_err_to_name(ret));
    return;
  }

  // Configure NimBLE host
  ble_hs_cfg.reset_cb = on_nimble_reset;
  ble_hs_cfg.sync_cb = on_nimble_sync;
  ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

  // Security configuration — bonding with SC+MITM, KeyboardDisplay IO
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_KEYBOARD_DISP;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  // Initialize NimBLE bond store (required for SM/pairing to work)
  ble_store_config_init();

  // Start NimBLE host task
  nimble_port_freertos_init(nimble_host_task);
  this->nimble_started_ = true;

  ESP_LOGI(TAG, "NimBLE stack started");
}

// ---------------------------------------------------------------------------
// Auth state management
// ---------------------------------------------------------------------------

void EmberOneControl::set_auth_state_(AuthState state) {
  if (this->auth_state_ == state) return;
  ESP_LOGD(TAG, "Auth state: %d -> %d", (int) this->auth_state_, (int) state);
  this->auth_state_ = state;
  for (auto &cb : this->auth_state_callbacks_) {
    cb(state);
  }
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

void EmberOneControl::start_scan_() {
  if (!this->nimble_synced_) return;

  struct ble_gap_disc_params disc_params = {};
  disc_params.passive = 0;     // Active scan to get scan responses
  disc_params.filter_duplicates = 0;
  disc_params.itvl = 0;        // Use defaults
  disc_params.window = 0;

  int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, gap_event_cb, nullptr);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
  }
}

// ---------------------------------------------------------------------------
// GAP event handler
// ---------------------------------------------------------------------------

int EmberOneControl::handle_gap_event_(struct ble_gap_event *event) {
  // nullptr event = sync callback
  if (event == nullptr) {
    ESP_LOGI(TAG, "NimBLE host synced — ready");
    this->nimble_synced_ = true;

    // Log bond count
    int bond_count = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bond_count);
    ESP_LOGI(TAG, "Stored bonds: %d", bond_count);

    if (this->has_saved_mac_ || this->pairing_active_) {
      this->start_scan_();
    }
    return 0;
  }

  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      // Advertisement received
      const struct ble_gap_disc_desc *disc = &event->disc;

      if (this->auth_state_ >= AuthState::CONNECTING) {
        return 0;  // Already connecting/connected
      }

      // Extract MAC (NimBLE stores in little-endian, we use big-endian)
      uint8_t mac[6];
      for (int i = 0; i < 6; i++) {
        mac[i] = disc->addr.val[5 - i];
      }

      // Reconnect to saved MAC
      if (this->has_saved_mac_ && !this->pairing_active_) {
        if (memcmp(mac, this->panel_mac_, 6) == 0) {
          if (this->reconnect_cooldown_until_ != 0 && millis() < this->reconnect_cooldown_until_) {
            return 0;
          }
          ESP_LOGD(TAG, "Found saved panel, connecting...");
          this->connect_to_device_(this->panel_mac_);
        }
        return 0;
      }

      // Pairing mode: look for Lippert manufacturer data in advertisement
      if (this->pairing_active_) {
        // Parse AD structures to find manufacturer-specific data (type 0xFF)
        const uint8_t *ad = disc->data;
        uint8_t ad_len = disc->length_data;
        uint8_t pos = 0;

        while (pos + 1 < ad_len) {
          uint8_t field_len = ad[pos];
          if (field_len == 0 || pos + field_len >= ad_len) break;

          uint8_t field_type = ad[pos + 1];
          if (field_type == 0xFF && field_len >= 3) {
            // Manufacturer Specific Data: [len][0xFF][mfr_id_lo][mfr_id_hi][data...]
            uint16_t mfr_id = ad[pos + 2] | (ad[pos + 3] << 8);
            if (mfr_id == LIPPERT_MANUFACTURER_ID && field_len >= 4) {
              uint8_t pairing_info = ad[pos + 4];
              ESP_LOGD(TAG, "Lippert device %02X:%02X:%02X:%02X:%02X:%02X pairing_info=0x%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], pairing_info);

              if (pairing_info & 0x02) {
                ESP_LOGI(TAG, "Found Lippert panel in pairing mode!");
                memcpy(this->panel_mac_, mac, 6);
                this->has_saved_mac_ = true;
                this->save_mac_to_nvs_(mac);
                this->pairing_active_ = false;

                ESP_LOGI(TAG, "Paired with panel: %02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

                // Stop scanning before connecting
                ble_gap_disc_cancel();
                this->connect_to_device_(this->panel_mac_);
                return 0;
              }
            }
          }
          pos += field_len + 1;
        }
      }
      return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
      if (event->connect.status == 0) {
        this->conn_handle_ = event->connect.conn_handle;
        this->connected_ = true;
        this->set_auth_state_(AuthState::CONNECTED);

        int bond_count = 0;
        ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &bond_count);
        ESP_LOGI(TAG, "Connected (conn_handle=%d, bonds=%d), initiating security...",
                 this->conn_handle_, bond_count);

        // Initiate security — NimBLE will use stored bond keys if available,
        // or start a new pairing if not.
        int rc = ble_gap_security_initiate(this->conn_handle_);
        if (rc != 0) {
          ESP_LOGW(TAG, "ble_gap_security_initiate failed: %d — requesting MTU and proceeding", rc);
          // Security initiation failed — proceed with MTU exchange and service discovery.
          // The peripheral may send a security request later (handled in ENC_CHANGE/PASSKEY).
          // Or characteristics may work without encryption initially.
          rc = ble_gattc_exchange_mtu(this->conn_handle_, [](uint16_t conn_handle,
              const struct ble_gatt_error *error, uint16_t mtu, void *arg) -> int {
            if (error->status == 0) {
              ESP_LOGD(TAG, "MTU exchanged: %d", mtu);
            }
            if (EmberOneControl::instance_) {
              EmberOneControl::instance_->start_service_discovery_();
            }
            return 0;
          }, nullptr);
          if (rc != 0) {
            this->start_service_discovery_();
          }
        }
      } else {
        ESP_LOGW(TAG, "Connection failed: status=%d — cooldown 5s", event->connect.status);
        this->connected_ = false;
        this->reconnect_cooldown_until_ = millis() + 5000;
        this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
        this->start_scan_();
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
      ESP_LOGW(TAG, "Disconnected: reason=0x%X", event->disconnect.reason);
      this->connected_ = false;
      this->conn_handle_ = 0;
      this->unlock_status_handle_ = 0;
      this->key_handle_ = 0;
      this->seed_handle_ = 0;
      this->data_write_handle_ = 0;
      this->data_read_handle_ = 0;
      this->data_read_cccd_handle_ = 0;
      this->seed_cccd_handle_ = 0;
      this->seed_properties_ = 0;
      this->data_read_properties_ = 0;
      this->auth_svc_start_ = 0;
      this->auth_svc_end_ = 0;
      this->data_svc_start_ = 0;
      this->data_svc_end_ = 0;
      this->svcs_discovered_ = 0;
      this->dscs_discovered_ = 0;
      this->unlock_reread_pending_ = false;
      this->last_heartbeat_time_ = 0;
      this->metadata_requested_ = false;
      this->cobs_decoder_.reset();
      this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
      this->start_scan_();
      return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
      if (event->enc_change.status == 0) {
        ESP_LOGI(TAG, "Encryption established successfully");
        // Request MTU exchange
        int rc = ble_att_set_preferred_mtu(185);
        if (rc != 0) {
          ESP_LOGW(TAG, "ble_att_set_preferred_mtu failed: %d", rc);
        }
        rc = ble_gattc_exchange_mtu(this->conn_handle_, [](uint16_t conn_handle,
            const struct ble_gatt_error *error, uint16_t mtu, void *arg) -> int {
          if (error->status == 0) {
            ESP_LOGD(TAG, "MTU exchanged: %d", mtu);
          }
          // Start service discovery after MTU exchange
          if (EmberOneControl::instance_) {
            EmberOneControl::instance_->start_service_discovery_();
          }
          return 0;
        }, nullptr);
        if (rc != 0) {
          ESP_LOGW(TAG, "ble_gattc_exchange_mtu failed: %d — discovering services anyway", rc);
          this->start_service_discovery_();
        }
      } else {
        ESP_LOGE(TAG, "Encryption FAILED: status=%d", event->enc_change.status);
        this->reconnect_cooldown_until_ = millis() + 10000;
        this->disconnect_();
      }
      return 0;
    }

    case BLE_GAP_EVENT_PASSKEY_ACTION: {
      if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
        uint32_t passkey = 0;
        for (size_t i = 0; i < this->pin_.length() && i < 6; i++) {
          passkey = passkey * 10 + (this->pin_[i] - '0');
        }
        ESP_LOGD(TAG, "Providing passkey for pairing");
        struct ble_sm_io pkey = {};
        pkey.action = BLE_SM_IOACT_INPUT;
        pkey.passkey = passkey;
        ble_sm_inject_io(event->passkey.conn_handle, &pkey);
      } else if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
        ESP_LOGD(TAG, "Passkey display action (ignoring)");
      } else if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
        ESP_LOGD(TAG, "Numeric comparison — confirming");
        struct ble_sm_io pkey = {};
        pkey.action = BLE_SM_IOACT_NUMCMP;
        pkey.numcmp_accept = 1;
        ble_sm_inject_io(event->passkey.conn_handle, &pkey);
      }
      return 0;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
      // Delete old bond and allow re-pairing
      ESP_LOGI(TAG, "Repeat pairing requested — deleting old bond");
      struct ble_gap_conn_desc desc;
      ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
      ble_store_util_delete_peer(&desc.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_MTU: {
      ESP_LOGD(TAG, "MTU updated: %d", event->mtu.value);
      return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX: {
      // Notification/indication received
      uint16_t attr_handle = event->notify_rx.attr_handle;
      struct os_mbuf *om = event->notify_rx.om;

      uint16_t data_len = OS_MBUF_PKTLEN(om);
      uint8_t data[256];
      if (data_len > sizeof(data)) data_len = sizeof(data);
      os_mbuf_copydata(om, 0, data_len, data);

      ESP_LOGD(TAG, "NOTIFY: handle=0x%04X len=%d", attr_handle, data_len);

      if (attr_handle == this->seed_handle_) {
        this->handle_seed_notification_(data, data_len);
      } else if (attr_handle == this->data_read_handle_) {
        for (uint16_t i = 0; i < data_len; i++) {
          if (this->cobs_decoder_.decode_byte(data[i])) {
            this->process_decoded_frame_(this->cobs_decoder_.get_frame());
          }
        }
      }
      return 0;
    }

    default:
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Connection management
// ---------------------------------------------------------------------------

void EmberOneControl::connect_to_device_(const uint8_t *mac) {
  if (this->reconnect_cooldown_until_ != 0 && millis() < this->reconnect_cooldown_until_) {
    return;
  }
  this->reconnect_cooldown_until_ = 0;

  // Stop scanning before connecting
  ble_gap_disc_cancel();

  this->set_auth_state_(AuthState::CONNECTING);
  ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  // NimBLE addresses are little-endian
  ble_addr_t addr = {};
  addr.type = BLE_ADDR_PUBLIC;
  for (int i = 0; i < 6; i++) {
    addr.val[5 - i] = mac[i];
  }

  int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, nullptr, gap_event_cb, nullptr);
  if (rc != 0) {
    ESP_LOGW(TAG, "ble_gap_connect failed: %d — cooldown 5s", rc);
    this->reconnect_cooldown_until_ = millis() + 5000;
    this->set_auth_state_(this->has_saved_mac_ ? AuthState::SCANNING : AuthState::DISCONNECTED);
    this->start_scan_();
  }
}

void EmberOneControl::disconnect_() {
  if (this->connected_) {
    ble_gap_terminate(this->conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
  }
  this->connected_ = false;
  this->unlock_status_handle_ = 0;
  this->key_handle_ = 0;
  this->seed_handle_ = 0;
  this->data_write_handle_ = 0;
  this->data_read_handle_ = 0;
  this->data_read_cccd_handle_ = 0;
  this->seed_cccd_handle_ = 0;
  this->seed_properties_ = 0;
  this->data_read_properties_ = 0;
  this->dscs_discovered_ = 0;
  this->unlock_reread_pending_ = false;
  this->last_heartbeat_time_ = 0;
  this->metadata_requested_ = false;
  this->cobs_decoder_.reset();
  this->set_auth_state_(AuthState::DISCONNECTED);
}

// ---------------------------------------------------------------------------
// Service / Characteristic / Descriptor discovery
// ---------------------------------------------------------------------------

void EmberOneControl::start_service_discovery_() {
  ESP_LOGD(TAG, "Starting service discovery...");
  this->svcs_discovered_ = 0;
  int rc = ble_gattc_disc_all_svcs(this->conn_handle_, gatt_discsvc_cb, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gattc_disc_all_svcs failed: %d", rc);
    this->disconnect_();
  }
}

int EmberOneControl::handle_gatt_discsvc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                              const struct ble_gatt_svc *service) {
  if (error->status == BLE_HS_EDONE) {
    ESP_LOGD(TAG, "Service discovery complete: auth_svc=%d-%d data_svc=%d-%d",
             this->auth_svc_start_, this->auth_svc_end_,
             this->data_svc_start_, this->data_svc_end_);

    // Now discover characteristics in both services
    if (this->auth_svc_start_ != 0) {
      ble_gattc_disc_all_chrs(this->conn_handle_,
                               this->auth_svc_start_, this->auth_svc_end_,
                               gatt_discchr_cb, nullptr);
    }
    return 0;
  }

  if (error->status != 0) {
    ESP_LOGE(TAG, "Service discovery error: %d", error->status);
    this->disconnect_();
    return 0;
  }

  // Check if this is one of our services
  if (service != nullptr) {
    if (ble_uuid_cmp(&service->uuid.u, &AUTH_SVC_UUID.u) == 0) {
      this->auth_svc_start_ = service->start_handle;
      this->auth_svc_end_ = service->end_handle;
      ESP_LOGD(TAG, "Found Auth service: %d-%d", this->auth_svc_start_, this->auth_svc_end_);
    } else if (ble_uuid_cmp(&service->uuid.u, &DATA_SVC_UUID.u) == 0) {
      this->data_svc_start_ = service->start_handle;
      this->data_svc_end_ = service->end_handle;
      ESP_LOGD(TAG, "Found Data service: %d-%d", this->data_svc_start_, this->data_svc_end_);
    }
  }

  return 0;
}

int EmberOneControl::handle_gatt_discchr_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                              const struct ble_gatt_chr *chr) {
  if (error->status == BLE_HS_EDONE) {
    this->svcs_discovered_++;

    if (this->svcs_discovered_ == 1 && this->data_svc_start_ != 0) {
      // Auth service chars done, now discover Data service chars
      ble_gattc_disc_all_chrs(this->conn_handle_,
                               this->data_svc_start_, this->data_svc_end_,
                               gatt_discchr_cb, nullptr);
    } else {
      // All chars discovered — now discover descriptors (CCCDs)
      ESP_LOGD(TAG, "Handles: unlock=0x%04X key=0x%04X seed=0x%04X dwrite=0x%04X dread=0x%04X",
               this->unlock_status_handle_, this->key_handle_,
               this->seed_handle_, this->data_write_handle_, this->data_read_handle_);

      // Discover descriptors for data_read and seed chars
      this->dscs_discovered_ = 0;
      if (this->data_read_handle_ != 0) {
        ble_gattc_disc_all_dscs(this->conn_handle_,
                                 this->data_read_handle_, this->data_svc_end_,
                                 gatt_discdsc_cb, nullptr);
      } else if (this->seed_handle_ != 0) {
        ble_gattc_disc_all_dscs(this->conn_handle_,
                                 this->seed_handle_, this->auth_svc_end_,
                                 gatt_discdsc_cb, nullptr);
      } else {
        this->start_authentication_();
      }
    }
    return 0;
  }

  if (error->status != 0) {
    ESP_LOGE(TAG, "Char discovery error: %d", error->status);
    return 0;
  }

  if (chr == nullptr) return 0;

  // Match characteristics by UUID
  if (ble_uuid_cmp(&chr->uuid.u, &UNLOCK_STATUS_CHR_UUID.u) == 0) {
    this->unlock_status_handle_ = chr->val_handle;
  } else if (ble_uuid_cmp(&chr->uuid.u, &KEY_CHR_UUID.u) == 0) {
    this->key_handle_ = chr->val_handle;
  } else if (ble_uuid_cmp(&chr->uuid.u, &SEED_CHR_UUID.u) == 0) {
    this->seed_handle_ = chr->val_handle;
    this->seed_properties_ = chr->properties;
  } else if (ble_uuid_cmp(&chr->uuid.u, &DATA_WRITE_CHR_UUID.u) == 0) {
    this->data_write_handle_ = chr->val_handle;
  } else if (ble_uuid_cmp(&chr->uuid.u, &DATA_READ_CHR_UUID.u) == 0) {
    this->data_read_handle_ = chr->val_handle;
    this->data_read_properties_ = chr->properties;
  }

  return 0;
}

int EmberOneControl::handle_gatt_discdsc_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                              uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc) {
  if (error->status == BLE_HS_EDONE) {
    this->dscs_discovered_++;

    if (this->dscs_discovered_ == 1 && this->seed_handle_ != 0 && this->seed_cccd_handle_ == 0) {
      // Data read descriptors done, now discover seed descriptors
      ble_gattc_disc_all_dscs(this->conn_handle_,
                               this->seed_handle_, this->auth_svc_end_,
                               gatt_discdsc_cb, nullptr);
    } else {
      // All descriptors discovered — start auth flow
      ESP_LOGD(TAG, "CCCDs: data_read=0x%04X seed=0x%04X",
               this->data_read_cccd_handle_, this->seed_cccd_handle_);
      this->start_authentication_();
    }
    return 0;
  }

  if (error->status != 0 || dsc == nullptr) return 0;

  // Check for CCCD (UUID 0x2902)
  ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);
  if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0) {
    // Figure out which characteristic this CCCD belongs to
    if (this->dscs_discovered_ == 0 && this->data_read_handle_ != 0) {
      this->data_read_cccd_handle_ = dsc->handle;
      ESP_LOGD(TAG, "Data Read CCCD: 0x%04X", dsc->handle);
    } else {
      this->seed_cccd_handle_ = dsc->handle;
      ESP_LOGD(TAG, "Seed CCCD: 0x%04X", dsc->handle);
    }
  }

  return 0;
}

// ---------------------------------------------------------------------------
// GATT read/write callbacks
// ---------------------------------------------------------------------------

int EmberOneControl::handle_gatt_read_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                           struct ble_gatt_attr *attr) {
  if (error->status != 0) {
    ESP_LOGW(TAG, "Read failed: status=%d", error->status);
    if (error->status == BLE_HS_ATT_ERR(0x05) || error->status == BLE_HS_ATT_ERR(0x0F)) {
      ESP_LOGW(TAG, "Encryption required — press CONNECT on Ember panel to pair");
      this->reconnect_cooldown_until_ = millis() + 15000;
      this->disconnect_();
    }
    return 0;
  }

  if (attr == nullptr || attr->om == nullptr) return 0;

  uint16_t data_len = OS_MBUF_PKTLEN(attr->om);
  uint8_t data[64];
  if (data_len > sizeof(data)) data_len = sizeof(data);
  os_mbuf_copydata(attr->om, 0, data_len, data);

  if (attr->handle == this->unlock_status_handle_) {
    this->handle_unlock_status_read_(data, data_len);
  }

  return 0;
}

int EmberOneControl::handle_gatt_write_cb_(uint16_t conn_handle, const struct ble_gatt_error *error,
                                             struct ble_gatt_attr *attr) {
  if (error->status != 0) {
    ESP_LOGW(TAG, "Write failed: status=%d handle=0x%04X", error->status,
             attr ? attr->handle : 0);
    return 0;
  }

  // CCCD write for data_read completed — subscribe to seed next
  if (attr != nullptr && attr->handle == this->data_read_cccd_handle_) {
    if (this->seed_cccd_handle_ != 0) {
      uint8_t cccd_val[2];
      if (this->seed_properties_ & 0x20) {  // INDICATE
        cccd_val[0] = 0x02; cccd_val[1] = 0x00;
      } else {
        cccd_val[0] = 0x01; cccd_val[1] = 0x00;  // NOTIFY
      }
      ble_gattc_write_flat(this->conn_handle_, this->seed_cccd_handle_,
                            cccd_val, 2, gatt_write_cb, nullptr);
    }
  }

  // Key write completed — schedule re-read
  if (attr != nullptr && attr->handle == this->key_handle_ &&
      this->auth_state_ == AuthState::UNLOCK_KEY_SENT) {
    this->unlock_reread_pending_ = true;
    this->unlock_reread_time_ = millis();
  }

  return 0;
}

// ---------------------------------------------------------------------------
// Auth flow
// ---------------------------------------------------------------------------

void EmberOneControl::start_authentication_() {
  if (this->unlock_status_handle_ == 0) {
    ESP_LOGW(TAG, "UNLOCK_STATUS not found, trying direct notification enable");
    this->set_auth_state_(AuthState::AUTHENTICATED);
    this->enable_data_notifications_();
    return;
  }

  ESP_LOGD(TAG, "Reading UNLOCK_STATUS challenge...");
  this->set_auth_state_(AuthState::READING_UNLOCK_STATUS);
  ble_gattc_read(this->conn_handle_, this->unlock_status_handle_, gatt_read_cb, nullptr);
}

void EmberOneControl::handle_unlock_status_read_(const uint8_t *data, size_t len) {
  if (len >= 4) {
    std::string text((char *) data, len);
    std::string lower = text;
    for (auto &c : lower) c = tolower(c);
    if (lower.find("unlocked") != std::string::npos) {
      ESP_LOGD(TAG, "Gateway UNLOCKED");
      this->set_auth_state_(AuthState::WAITING_FOR_SEED);
      this->enable_data_notifications_();
      return;
    }
  }

  if (len == 4) {
    ESP_LOGD(TAG, "UNLOCK_STATUS: %02X%02X%02X%02X (state=%d)",
             data[0], data[1], data[2], data[3], (int) this->auth_state_);

    bool all_zeros = (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0);

    if (all_zeros) {
      ESP_LOGD(TAG, "UNLOCK_STATUS all zeros — not ready");
      return;
    }

    if (this->auth_state_ == AuthState::READING_UNLOCK_STATUS ||
        this->auth_state_ == AuthState::CONNECTED) {
      uint8_t response[4];
      calculate_unlock_response(data, response);
      ESP_LOGD(TAG, "Sending unlock response");

      this->set_auth_state_(AuthState::UNLOCK_KEY_SENT);
      ble_gattc_write_no_rsp_flat(this->conn_handle_, this->key_handle_, response, 4);
      this->unlock_reread_pending_ = true;
      this->unlock_reread_time_ = millis();
      return;
    }

    if (this->auth_state_ == AuthState::UNLOCK_KEY_SENT) {
      ESP_LOGW(TAG, "Step 1 FAILED — challenge not consumed (key rejected?)");
      this->reconnect_cooldown_until_ = millis() + 10000;
      this->disconnect_();
      return;
    }

    ESP_LOGD(TAG, "UNLOCK_STATUS in state %d — ignoring", (int) this->auth_state_);
  } else {
    ESP_LOGD(TAG, "UNLOCK_STATUS: %d bytes (unexpected)", (int) len);
  }
}

void EmberOneControl::handle_seed_notification_(const uint8_t *data, size_t len) {
  if (len < 4) {
    ESP_LOGW(TAG, "SEED notification too short: %d bytes", (int) len);
    return;
  }

  ESP_LOGD(TAG, "Received SEED notification");
  build_auth_key(data, this->pin_, STEP2_CIPHER, this->auth_key_);

  this->set_auth_state_(AuthState::AUTH_KEY_SENT);
  ble_gattc_write_no_rsp_flat(this->conn_handle_, this->key_handle_, this->auth_key_, 16);
  ESP_LOGI(TAG, "Authentication complete");
  this->set_auth_state_(AuthState::AUTHENTICATED);
}

void EmberOneControl::enable_data_notifications_() {
  // Subscribe to DATA_READ first
  if (this->data_read_cccd_handle_ != 0) {
    uint8_t cccd_val[2];
    if (this->data_read_properties_ & 0x20) {  // INDICATE
      cccd_val[0] = 0x02; cccd_val[1] = 0x00;
    } else {
      cccd_val[0] = 0x01; cccd_val[1] = 0x00;  // NOTIFY
    }
    ble_gattc_write_flat(this->conn_handle_, this->data_read_cccd_handle_,
                          cccd_val, 2, gatt_write_cb, nullptr);
    // SEED subscription is triggered in write_cb after DATA_READ CCCD completes
  } else if (this->seed_cccd_handle_ != 0) {
    uint8_t cccd_val[2] = {0x01, 0x00};
    ble_gattc_write_flat(this->conn_handle_, this->seed_cccd_handle_,
                          cccd_val, 2, gatt_write_cb, nullptr);
  }
}

// ---------------------------------------------------------------------------
// Pairing control
// ---------------------------------------------------------------------------

void EmberOneControl::start_pairing() {
  ESP_LOGI(TAG, "Starting pairing mode (timeout=%u ms)...", PAIRING_TIMEOUT_MS);
  this->pairing_active_ = true;
  this->pairing_start_time_ = millis();
  this->set_auth_state_(AuthState::SCANNING);
  if (this->nimble_synced_) {
    this->start_scan_();
  }
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

  SavedPanelConfig cfg{};
  cfg.valid = false;
  this->pref_.save(&cfg);
  global_preferences->sync();

  // Clear NimBLE bond store
  ble_store_clear();

  this->set_auth_state_(AuthState::DISCONNECTED);
}

// ---------------------------------------------------------------------------
// Command sending
// ---------------------------------------------------------------------------

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

  if (this->data_write_handle_ == 0 || !this->connected_ ||
      this->auth_state_ < AuthState::CONNECTED) {
    ESP_LOGW(TAG, "Cannot send command - not connected");
    return;
  }

  int rc = ble_gattc_write_no_rsp_flat(this->conn_handle_, this->data_write_handle_,
                                         encoded.data(), encoded.size());
  if (rc != 0) {
    ESP_LOGW(TAG, "GATT write command failed: %d", rc);
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
// Event processing (unchanged from Bluedroid version)
// ---------------------------------------------------------------------------

DeviceAddress EmberOneControl::resolve_address(uint16_t function_name) const {
  auto it = this->function_name_map_.find(function_name);
  if (it != this->function_name_map_.end()) {
    return it->second;
  }
  return 0;
}

void EmberOneControl::process_decoded_frame_(const std::vector<uint8_t> &frame) {
  if (frame.empty()) return;

  uint8_t event_type = frame[0];
  const uint8_t *data = frame.data();
  size_t len = frame.size();

  ESP_LOGD(TAG, "Event type: 0x%02X (%d bytes)", event_type, len);

  switch (event_type) {
    case EVENT_GATEWAY_INFORMATION:
      this->handle_gateway_info_(data, len);
      if (this->auth_state_ != AuthState::AUTHENTICATED) {
        this->set_auth_state_(AuthState::AUTHENTICATED);
      }
      if (!this->metadata_requested_) {
        this->metadata_requested_ = true;
        this->send_get_devices_metadata_();
      }
      break;
    case EVENT_DEVICE_COMMAND:
      this->handle_command_response_(data, len);
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
  ESP_LOGD(TAG, "Gateway info: tableId=0x%02X, devices=%d", data[4], data[3]);
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

  uint16_t voltage_raw = (data[1] << 8) | data[2];
  if (voltage_raw != 0xFFFF) {
    this->battery_voltage_ = voltage_raw / 256.0f;

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

void EmberOneControl::handle_command_response_(const uint8_t *data, size_t len) {
  if (len < 7) return;

  uint8_t resp_type = data[3];
  if (resp_type == 0x81) {
    ESP_LOGD(TAG, "Metadata complete (%d functions)", this->function_name_map_.size());
    return;
  }
  if (resp_type != 0x01) {
    ESP_LOGD(TAG, "Command response type: 0x%02X (not metadata data)", resp_type);
    return;
  }

  uint8_t table_id = data[4];
  uint8_t start_id = data[5];
  uint8_t count = data[6];

  ESP_LOGD(TAG, "Metadata: table=0x%02X start=%d count=%d", table_id, start_id, count);

  size_t offset = 7;
  int index = 0;

  while (index < count && offset + 2 < len) {
    uint8_t protocol = data[offset];
    uint8_t payload_size = data[offset + 1];
    size_t entry_size = payload_size + 2;

    if (offset + entry_size > len) {
      ESP_LOGW(TAG, "Metadata entry overflows (need %d, have %d)", offset + entry_size, len);
      break;
    }

    uint8_t device_id = (start_id + index) & 0xFF;
    DeviceAddress device_addr = (table_id << 8) | device_id;

    if ((protocol == 1 || protocol == 2) && payload_size == 17) {
      uint16_t func_name = (data[offset + 2] << 8) | data[offset + 3];
      this->function_name_map_[func_name] = device_addr;
      ESP_LOGD(TAG, "  [%d:0x%02X] func=%d → addr=0x%04X",
               table_id, device_id, func_name, device_addr);
    } else if (protocol == 1 && payload_size == 0) {
      this->function_name_map_[FUNC_GATEWAY_RVLINK] = device_addr;
      ESP_LOGD(TAG, "  [%d:0x%02X] Gateway → addr=0x%04X", table_id, device_id, device_addr);
    } else {
      ESP_LOGD(TAG, "  [%d:0x%02X] skipped: proto=%d payload=%d", table_id, device_id, protocol, payload_size);
    }

    offset += entry_size;
    index++;
  }

  if (!this->function_name_map_.empty()) {
    this->metadata_received_ = true;
    ESP_LOGD(TAG, "Metadata: %d functions resolved", this->function_name_map_.size());
  }
}

}  // namespace ember_onecontrol
}  // namespace esphome

#endif  // USE_ESP32
