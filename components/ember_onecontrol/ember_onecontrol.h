#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/esp32_ble/ble.h"
#include "ember_protocol.h"

#include <vector>
#include <functional>
#include <string>
#include <map>

#ifdef USE_ESP32
#include <esp_gattc_api.h>
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>
#endif

namespace esphome {
namespace ember_onecontrol {

static const uint16_t LIPPERT_MANUFACTURER_ID = 0x05C7;  // LCI Industries (Lippert)
static const uint32_t PAIRING_TIMEOUT_MS = 60000;
static const uint8_t EMBER_GATTC_APP_ID = 0xFE;  // Unique app ID to avoid conflicts

enum class AuthState : uint8_t {
  DISCONNECTED,
  SCANNING,
  CONNECTING,
  CONNECTED,
  READING_UNLOCK_STATUS,
  UNLOCK_KEY_SENT,
  WAITING_FOR_SEED,
  AUTH_KEY_SENT,
  AUTHENTICATED,
};

using DeviceAddress = uint16_t;

using RelayCallback = std::function<void(DeviceAddress addr, bool is_on)>;
using RvStatusCallback = std::function<void(float voltage)>;
using TankCallback = std::function<void(DeviceAddress addr, int percent)>;
using HBridgeCallback = std::function<void(DeviceAddress addr, int status, int position)>;
using AuthStateCallback = std::function<void(AuthState state)>;

// NVS storage struct for saved MAC
struct SavedPanelConfig {
  uint8_t mac[6];
  bool valid;
} __attribute__((packed));

class EmberOneControl : public PollingComponent,
                        public esp32_ble_tracker::ESPBTDeviceListener,
                        public esp32_ble::GATTcEventHandler,
                        public esp32_ble::GAPEventHandler {
 public:
  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  // ESPBTDeviceListener — called for every BLE advertisement
  bool parse_device(const esp32_ble_tracker::ESPBTDevice &device) override;

  void set_pin(const std::string &pin) { this->pin_ = pin; }

  // Pairing control (called by button entity)
  void start_pairing();
  void stop_pairing();
  bool is_pairing() const { return this->pairing_active_; }
  void clear_paired_device();

  // Entity registration
  void register_relay_callback(RelayCallback cb) { this->relay_callbacks_.push_back(std::move(cb)); }
  void register_rv_status_callback(RvStatusCallback cb) { this->rv_status_callbacks_.push_back(std::move(cb)); }
  void register_tank_callback(TankCallback cb) { this->tank_callbacks_.push_back(std::move(cb)); }
  void register_hbridge_callback(HBridgeCallback cb) { this->hbridge_callbacks_.push_back(std::move(cb)); }
  void register_auth_state_callback(AuthStateCallback cb) { this->auth_state_callbacks_.push_back(std::move(cb)); }

  // Send a switch command
  void send_switch_command(uint8_t table_id, uint8_t device_id, bool turn_on);

  // Metadata resolution: returns DeviceAddress for a function_name, or 0 if unknown
  DeviceAddress resolve_address(uint16_t function_name) const;
  bool has_metadata() const { return this->metadata_received_; }

  AuthState get_auth_state() const { return this->auth_state_; }
  float get_battery_voltage() const { return this->battery_voltage_; }
  const std::string &get_battery_status() const { return this->battery_status_; }

  // GATTcEventHandler interface — receives GATTC events from ESPHome's BLE stack
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;

  // GAPEventHandler interface — receives GAP events (security/bonding)
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

 protected:
  void set_auth_state_(AuthState state);
  void connect_to_device_(const esp_bd_addr_t addr);
  void disconnect_();

  // Auth flow
  void start_authentication_();
  void handle_unlock_status_read_(const uint8_t *data, size_t len);
  void handle_seed_notification_(const uint8_t *data, size_t len);
  void enable_data_notifications_();
  void send_get_devices_();
  void send_get_devices_metadata_();

  // Event processing
  void process_decoded_frame_(const std::vector<uint8_t> &frame);
  void handle_relay_status_(const uint8_t *data, size_t len);
  void handle_rv_status_(const uint8_t *data, size_t len);
  void handle_tank_status_(const uint8_t *data, size_t len);
  void handle_tank_status_v2_(const uint8_t *data, size_t len);
  void handle_hbridge_status_(const uint8_t *data, size_t len);
  void handle_gateway_info_(const uint8_t *data, size_t len);
  void handle_command_response_(const uint8_t *data, size_t len);
  void send_command_(const std::vector<uint8_t> &raw_command);

  // NVS persistence
  void save_mac_to_nvs_(const uint8_t *mac);
  bool load_mac_from_nvs_(uint8_t *mac);

  std::string pin_{"357694"};
  AuthState auth_state_{AuthState::DISCONNECTED};

  // Pairing state
  bool pairing_active_{false};
  uint32_t pairing_start_time_{0};

  // Saved panel MAC
  esp_bd_addr_t panel_mac_{};
  bool has_saved_mac_{false};

  // GATT client state
  esp_gatt_if_t gattc_if_{0};
  uint16_t conn_id_{0};
  bool gattc_registered_{false};
  bool gattc_app_registered_{false};

  // BLE handles
  uint16_t unlock_status_handle_{0};
  uint16_t key_handle_{0};
  uint16_t seed_handle_{0};
  uint16_t data_write_handle_{0};
  uint16_t data_read_handle_{0};

  // Characteristic properties (for auto-detecting notify vs indicate)
  uint8_t seed_properties_{0};
  uint8_t data_read_properties_{0};

  // Delayed unlock re-read timer
  bool unlock_reread_pending_{false};
  uint32_t unlock_reread_time_{0};

  // Delayed NVS bond check (3s after bonding to verify persistence)

  // Reconnect backoff
  uint32_t reconnect_cooldown_until_{0};

  // Heartbeat timer (GetDevices every 5s to keep connection alive)
  uint32_t last_heartbeat_time_{0};
  static const uint32_t HEARTBEAT_INTERVAL_MS = 5000;

  // Protocol state
  CobsByteDecoder cobs_decoder_;
  uint16_t command_id_counter_{1};
  uint8_t device_table_id_{0};
  uint8_t auth_key_[16]{};

  // Metadata: function_name → DeviceAddress
  std::map<uint16_t, DeviceAddress> function_name_map_;
  bool metadata_received_{false};
  bool metadata_requested_{false};

  // Cached values
  float battery_voltage_{0.0f};
  std::string battery_status_{"Unknown"};

  // NVS preferences
  ESPPreferenceObject pref_;

  // GAP callback wrapper for KEY_EVT interception
  static EmberOneControl *instance_;
  static void gap_cb_wrapper_(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
  bool gap_wrapper_installed_{false};

  // Callbacks
  std::vector<RelayCallback> relay_callbacks_;
  std::vector<RvStatusCallback> rv_status_callbacks_;
  std::vector<TankCallback> tank_callbacks_;
  std::vector<HBridgeCallback> hbridge_callbacks_;
  std::vector<AuthStateCallback> auth_state_callbacks_;
};

}  // namespace ember_onecontrol
}  // namespace esphome
