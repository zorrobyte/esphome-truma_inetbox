# Ember OneControl Seamless Pairing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rearchitect the ember_onecontrol ESPHome component to use standalone BLE scanning with push-to-pair auto-discovery instead of requiring a pre-configured MAC address.

**Architecture:** The hub drops `ble_client` dependency and implements `ESPBTDeviceListener` for BLE advertisement scanning + direct `esp_ble_gattc_*` GATT client. A pairing button entity in HA triggers a 60-second discovery window. The ESP32 connects to the first OneControl panel broadcasting "pairing active" (manufacturer ID 0x0499, bit 1 set). MAC is persisted to NVS flash for automatic reconnection on reboot.

**Tech Stack:** ESPHome C++ (ESP-IDF BLE APIs), Python (ESPHome codegen), ESP32 NVS preferences

**Repo:** `C:\Users\zorro\esphome-truma_inetbox` on branch `feature/ember-onecontrol`

**Reference:** Kotlin source at `C:\Users\zorro\AppData\Local\Temp\ble-plugin-bridge\app\src\main\java\com\blemqttbridge\plugins\onecontrol\`

---

### Task 1: Rearchitect hub — drop ble_client, add BLE scanner + GATT client

**Files:**
- Modify: `components/ember_onecontrol/__init__.py`
- Rewrite: `components/ember_onecontrol/ember_onecontrol.h`
- Rewrite: `components/ember_onecontrol/ember_onecontrol.cpp`

**Step 1: Rewrite `__init__.py` to remove ble_client dependency**

Replace ble_client schema with esp32_ble_tracker dependency. The hub no longer extends BLEClientNode.

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32_ble_tracker
from esphome.const import CONF_ID

DEPENDENCIES = ["esp32_ble_tracker"]
CODEOWNERS = ["@zorro"]

CONF_EMBER_ONECONTROL_ID = "ember_onecontrol_id"
CONF_PIN = "pin"

ember_onecontrol_ns = cg.esphome_ns.namespace("ember_onecontrol")
EmberOneControl = ember_onecontrol_ns.class_(
    "EmberOneControl",
    cg.PollingComponent,
    esp32_ble_tracker.ESPBTDeviceListener,
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EmberOneControl),
            cv.Optional(CONF_PIN, default="090336"): cv.string,
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await esp32_ble_tracker.register_ble_device(var, config)
    cg.add(var.set_pin(config[CONF_PIN]))
```

**Step 2: Rewrite `ember_onecontrol.h`**

The hub now:
- Extends `PollingComponent` + `esp32_ble_tracker::ESPBTDeviceListener`
- Has `parse_device()` for scanning advertisements
- Manages its own GATT interface via `esp_ble_gattc_*` APIs
- Stores MAC in ESPHome preferences (NVS)
- Has `start_pairing()` / `stop_pairing()` methods called by button entity

```cpp
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "ember_protocol.h"

#include <vector>
#include <functional>
#include <string>

#ifdef USE_ESP32
#include <esp_gattc_api.h>
#include <esp_bt_defs.h>
#include <esp_gap_ble_api.h>
#endif

namespace esphome {
namespace ember_onecontrol {

static const uint16_t LIPPERT_MANUFACTURER_ID = 0x0499;
static const uint32_t PAIRING_TIMEOUT_MS = 60000;

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

class EmberOneControl : public PollingComponent, public esp32_ble_tracker::ESPBTDeviceListener {
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

  AuthState get_auth_state() const { return this->auth_state_; }
  float get_battery_voltage() const { return this->battery_voltage_; }
  const std::string &get_battery_status() const { return this->battery_status_; }

 protected:
  void set_auth_state_(AuthState state);

  // GATT client management
  static void gattc_event_handler_static_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                           esp_ble_gattc_cb_param_t *param);
  void gattc_event_handler_(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                             esp_ble_gattc_cb_param_t *param);
  void connect_to_device_(const esp_bd_addr_t addr);
  void disconnect_();

  // Auth flow
  void start_authentication_();
  void handle_unlock_status_read_(const uint8_t *data, size_t len);
  void handle_seed_notification_(const uint8_t *data, size_t len);
  void enable_data_notifications_();
  void send_get_devices_metadata_();

  // Event processing
  void process_decoded_frame_(const std::vector<uint8_t> &frame);
  void handle_relay_status_(const uint8_t *data, size_t len);
  void handle_rv_status_(const uint8_t *data, size_t len);
  void handle_tank_status_(const uint8_t *data, size_t len);
  void handle_tank_status_v2_(const uint8_t *data, size_t len);
  void handle_hbridge_status_(const uint8_t *data, size_t len);
  void handle_gateway_info_(const uint8_t *data, size_t len);
  void send_command_(const std::vector<uint8_t> &raw_command);

  // NVS persistence
  void save_mac_to_nvs_(const uint8_t *mac);
  bool load_mac_from_nvs_(uint8_t *mac);

  std::string pin_{"090336"};
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

  // BLE handles
  uint16_t unlock_status_handle_{0};
  uint16_t key_handle_{0};
  uint16_t seed_handle_{0};
  uint16_t data_write_handle_{0};
  uint16_t data_read_handle_{0};

  // Protocol state
  CobsByteDecoder cobs_decoder_;
  uint16_t command_id_counter_{1};
  uint8_t device_table_id_{0};
  uint8_t auth_key_[16]{};

  // Cached values
  float battery_voltage_{0.0f};
  std::string battery_status_{"Unknown"};

  // NVS preferences
  ESPPreferenceObject pref_;

  // Callbacks
  std::vector<RelayCallback> relay_callbacks_;
  std::vector<RvStatusCallback> rv_status_callbacks_;
  std::vector<TankCallback> tank_callbacks_;
  std::vector<HBridgeCallback> hbridge_callbacks_;
  std::vector<AuthStateCallback> auth_state_callbacks_;

  // Static instance pointer for GATT callback routing
  static EmberOneControl *instance_;
};

}  // namespace ember_onecontrol
}  // namespace esphome
```

**Step 3: Rewrite `ember_onecontrol.cpp`**

Key changes from old ble_client version:
1. `setup()`: Register GATT client app, load MAC from NVS, auto-connect if saved
2. `parse_device()`: Check manufacturer ID 0x0499, pairing bit, trigger connect
3. `gattc_event_handler_()`: Same auth state machine but using direct ESP APIs
4. `loop()`: Check pairing timeout
5. NVS save/load for MAC persistence

The full implementation for this file is large. Key sections:

```cpp
// In setup():
//   - Register GATT client: esp_ble_gattc_app_register(0)
//   - Load MAC from NVS preferences
//   - If MAC exists, connect_to_device_()

// parse_device() — called for every BLE advertisement:
bool EmberOneControl::parse_device(const esp32_ble_tracker::ESPBTDevice &device) {
  // If already connected/authenticated, ignore all advertisements
  if (this->auth_state_ >= AuthState::CONNECTING) return false;

  // If we have a saved MAC and aren't pairing, try to reconnect to saved device
  if (this->has_saved_mac_ && !this->pairing_active_) {
    if (memcmp(device.address_uint64(), ...) == 0) {
      this->connect_to_device_(mac);
      return true;
    }
    return false;
  }

  // If not in pairing mode, ignore
  if (!this->pairing_active_) return false;

  // Check for Lippert manufacturer ID 0x0499
  for (auto &mfr_data : device.get_manufacturer_datas()) {
    if (mfr_data.uuid.get_uuid().uuid.uuid16 == LIPPERT_MANUFACTURER_ID) {
      if (mfr_data.data.size() >= 1) {
        uint8_t pairing_info = mfr_data.data[0];
        bool pairing_enabled = (pairing_info & 0x02) != 0;
        if (pairing_enabled) {
          ESP_LOGI(TAG, "Found pairing-active OneControl panel: %s",
                   device.address_str().c_str());
          // Save MAC, stop pairing, connect
          memcpy(this->panel_mac_, ...);
          this->save_mac_to_nvs_(this->panel_mac_);
          this->has_saved_mac_ = true;
          this->stop_pairing();
          this->connect_to_device_(this->panel_mac_);
          return true;
        }
      }
    }
  }
  return false;
}

// loop() — check pairing timeout:
void EmberOneControl::loop() {
  if (this->pairing_active_ && (millis() - this->pairing_start_time_ > PAIRING_TIMEOUT_MS)) {
    ESP_LOGW(TAG, "Pairing timeout (60s) — no panel found");
    this->stop_pairing();
  }
}

// GATT event handler — same auth state machine as before, but using direct APIs
// instead of ble_client's get_characteristic() wrappers. The flow is identical:
//   ESP_GATTC_REG_EVT → store gattc_if_
//   ESP_GATTC_OPEN_EVT → connected, request MTU
//   ESP_GATTC_SEARCH_CMPL_EVT → find handles, start auth
//   ESP_GATTC_READ_CHAR_EVT → handle_unlock_status_read_()
//   ESP_GATTC_NOTIFY_EVT → COBS decode / seed handling
//   ESP_GATTC_WRITE_CHAR_EVT → auth key verification
//   ESP_GATTC_DISCONNECT_EVT → reset state, schedule reconnect
```

**Step 4: Commit**

```bash
git add components/ember_onecontrol/__init__.py
git add components/ember_onecontrol/ember_onecontrol.h
git add components/ember_onecontrol/ember_onecontrol.cpp
git commit -m "refactor: drop ble_client, add standalone BLE scanner + GATT client

Hub now implements ESPBTDeviceListener for advertisement scanning
and manages its own GATT connection via esp_ble_gattc_* APIs.
Adds MAC persistence to NVS for auto-reconnect on reboot.
No more MAC address required in YAML config."
```

---

### Task 2: Add pairing button entity

**Files:**
- Create: `components/ember_onecontrol/button/__init__.py`
- Create: `components/ember_onecontrol/button/EmberPairingButton.h`
- Create: `components/ember_onecontrol/button/EmberPairingButton.cpp`

**Step 1: Create `button/__init__.py`**

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from esphome.const import CONF_ID
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl

DEPENDENCIES = ["ember_onecontrol"]

EmberPairingButton = ember_onecontrol_ns.class_(
    "EmberPairingButton", button.Button, cg.Component
)

CONFIG_SCHEMA = (
    button.button_schema(EmberPairingButton, icon="mdi:bluetooth-connect")
    .extend(
        {
            cv.GenerateID(CONF_EMBER_ONECONTROL_ID): cv.use_id(EmberOneControl),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)
    await cg.register_parented(var, config[CONF_EMBER_ONECONTROL_ID])
```

**Step 2: Create `EmberPairingButton.h`**

```cpp
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/button/button.h"
#include "esphome/components/ember_onecontrol/ember_onecontrol.h"

namespace esphome {
namespace ember_onecontrol {

class EmberPairingButton : public Component, public button::Button, public Parented<EmberOneControl> {
 public:
  void dump_config() override;

 protected:
  void press_action() override;
};

}  // namespace ember_onecontrol
}  // namespace esphome
```

**Step 3: Create `EmberPairingButton.cpp`**

```cpp
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
```

**Step 4: Commit**

```bash
git add components/ember_onecontrol/button/
git commit -m "feat: add pairing button entity for seamless push-to-pair"
```

---

### Task 3: Update all sub-component __init__.py files for new hub class

**Files:**
- Modify: `components/ember_onecontrol/switch/__init__.py`
- Modify: `components/ember_onecontrol/light/__init__.py`
- Modify: `components/ember_onecontrol/sensor/__init__.py`
- Modify: `components/ember_onecontrol/binary_sensor/__init__.py`
- Modify: `components/ember_onecontrol/text_sensor/__init__.py`

No C++ changes needed — the sub-components use `Parented<EmberOneControl>` which hasn't changed. Only Python imports need updating since the hub no longer comes from ble_client.

The `.. import` in each `__init__.py` already imports `EmberOneControl` from the parent — no change needed there. The main change is that none of them reference ble_client anymore (they didn't directly, so no changes needed).

**Step 1: Verify each sub-component __init__.py still works**

Check that each file's imports and schema reference the correct parent types. The key import is:
```python
from .. import ember_onecontrol_ns, CONF_EMBER_ONECONTROL_ID, EmberOneControl
```
This should still work since `__init__.py` still exports these symbols.

**Step 2: Update text_sensor connection status mapping**

The `EmberTextSensor.cpp` needs an update since AuthState now includes `SCANNING` and `CONNECTING`:

```cpp
// In EmberTextSensor.cpp setup(), update the auth state callback:
case AuthState::SCANNING:
  this->publish_state("Scanning");
  break;
case AuthState::CONNECTING:
  this->publish_state("Connecting");
  break;
```

**Step 3: Commit**

```bash
git add components/ember_onecontrol/text_sensor/EmberTextSensor.cpp
git commit -m "feat: update connection status text for scanning/connecting states"
```

---

### Task 4: Update example YAML for seamless pairing

**Files:**
- Rewrite: `examples/ember_onecontrol.yaml`

**Step 1: Rewrite example config**

Remove ble_client and MAC address. Add pairing button. Simplify.

```yaml
# Ember RV OneControl BLE Integration
#
# Setup:
# 1. Flash this config to an ESP32
# 2. In Home Assistant, press the "Start Pairing" button
# 3. Within 60 seconds, hold CONNECT on your Ember panel until it flashes
# 4. The ESP32 will auto-discover, pair, and authenticate
# 5. Entities appear in HA. Pairing is saved — survives reboots.
#
# To re-pair (e.g., different panel): press "Start Pairing" again.

esphome:
  name: ember-rv-controller
  friendly_name: "Ember RV Controller"

esp32:
  board: esp32dev
  framework:
    type: esp-idf

logger:
  level: DEBUG

api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Ember-RV-Fallback"
    password: !secret fallback_password

external_components:
  - source:
      type: local
      path: components

# BLE scanner (required for auto-discovery)
esp32_ble_tracker:

# OneControl hub — no MAC address needed!
ember_onecontrol:
  pin: "090336"

# Pairing button — press in HA to start 60s discovery window
button:
  - platform: ember_onecontrol
    name: "Start Pairing"

# Lights (toggle-only, no dimming)
light:
  - platform: ember_onecontrol
    name: "Ceiling Light"
    type: CEILING_LIGHT

  - platform: ember_onecontrol
    name: "Accent Light"
    type: ACCENT_LIGHT

  - platform: ember_onecontrol
    name: "Step Light"
    type: STEP_LIGHT

  - platform: ember_onecontrol
    name: "Pump Light"
    type: PUMP_LIGHT

  - platform: ember_onecontrol
    name: "Awning Light"
    type: AWNING_LIGHT

# Switches
switch:
  - platform: ember_onecontrol
    name: "Water Pump"
    type: WATER_PUMP

# Sensors
sensor:
  - platform: ember_onecontrol
    name: "Battery Voltage"
    type: BATTERY_VOLTAGE

  - platform: ember_onecontrol
    name: "Fresh Tank"
    type: TANK_FRESH

  - platform: ember_onecontrol
    name: "Black Tank"
    type: TANK_BLACK

  - platform: ember_onecontrol
    name: "Grey Tank"
    type: TANK_GREY

# Binary Sensors (read-only for safety)
binary_sensor:
  - platform: ember_onecontrol
    name: "Slide State"
    type: SLIDE_STATE

  - platform: ember_onecontrol
    name: "Awning State"
    type: AWNING_STATE

# Text Sensors
text_sensor:
  - platform: ember_onecontrol
    name: "Battery Status"
    type: BATTERY_STATUS

  - platform: ember_onecontrol
    name: "Connection Status"
    type: CONNECTION_STATUS
```

**Step 2: Commit**

```bash
git add examples/ember_onecontrol.yaml
git commit -m "docs: update example YAML for seamless pairing (no MAC needed)"
```

---

### Task 5: Write the full hub .cpp implementation

**Files:**
- Rewrite: `components/ember_onecontrol/ember_onecontrol.cpp`

This is the largest task. The implementation must handle:

1. **GATT client registration** — `esp_ble_gattc_app_register()` in `setup()`, store `gattc_if_` in `ESP_GATTC_REG_EVT`
2. **NVS persistence** — Use `ESPPreferenceObject` with `global_preferences->make_preference<SavedPanelConfig>(fnv1_hash("ember_oc"))`. Load in `setup()`, save when pairing succeeds.
3. **Advertisement scanning** — `parse_device()` checks manufacturer data for 0x0499 + pairing bit
4. **Connection** — `esp_ble_gattc_open(gattc_if_, addr, BLE_ADDR_TYPE_PUBLIC, true)`
5. **Service discovery** — After `ESP_GATTC_OPEN_EVT`, call `esp_ble_gattc_search_service()`. In `ESP_GATTC_SEARCH_RES_EVT` collect service handles. In `ESP_GATTC_SEARCH_CMPL_EVT` enumerate characteristics with `esp_ble_gattc_get_all_char()`.
6. **Auth state machine** — Same as current implementation but using `esp_ble_gattc_read_char()`, `esp_ble_gattc_write_char()`, `esp_ble_gattc_register_for_notify()` directly
7. **Reconnection** — On disconnect, if we have a saved MAC, set state to DISCONNECTED and let `parse_device()` catch the next advertisement to reconnect

**Key reference from Kotlin** (OneControlDevicePlugin.kt):
- Auth flow: lines 1230-1542 (UNLOCK_STATUS read → challenge → TEA → KEY write → verify → SEED notification → auth key)
- Data processing: lines 1720-1912 (COBS byte-by-byte decode → frame dispatch)
- Command sending: lines 3975-4016 (COBS encode → write to DATA_WRITE)

The static callback pattern for ESP GATTC:
```cpp
EmberOneControl *EmberOneControl::instance_ = nullptr;

void EmberOneControl::gattc_event_handler_static_(
    esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
    esp_ble_gattc_cb_param_t *param) {
  if (instance_ != nullptr) {
    instance_->gattc_event_handler_(event, gattc_if, param);
  }
}
```

**Step 1: Implement the full .cpp file** (details in code — this task produces the complete file)

**Step 2: Verify it compiles**

```bash
# From ESPHome dev environment:
esphome compile examples/ember_onecontrol.yaml
```

Expected: Compiles without errors (may have warnings about unused variables initially)

**Step 3: Commit**

```bash
git add components/ember_onecontrol/ember_onecontrol.cpp
git commit -m "feat: implement full BLE scanner + GATT client with push-to-pair

- GATT client registration and static callback routing
- Advertisement scanning for Lippert manufacturer ID 0x0499
- Pairing-active bit detection for seamless discovery
- MAC persistence to NVS via ESPPreferenceObject
- Complete auth state machine (UNLOCK_STATUS → TEA → SEED → KEY)
- COBS stream decode on DATA_READ notifications
- Auto-reconnect to saved MAC on reboot/disconnect"
```

---

### Task 6: Handle characteristic discovery via ESP GATT APIs

**Files:**
- Modify: `components/ember_onecontrol/ember_onecontrol.cpp` (the SEARCH_RES/SEARCH_CMPL handlers)

**Context:** Unlike `ble_client` which provides `get_characteristic()`, we need to manually enumerate services and characteristics using `esp_ble_gattc_get_attr_count()` and `esp_ble_gattc_get_char_by_uuid()`.

**Step 1: Implement service/characteristic discovery**

In the `ESP_GATTC_SEARCH_CMPL_EVT` handler, look up each characteristic by UUID:

```cpp
// Helper to find characteristic handle by service+char UUID
uint16_t find_char_handle_(uint16_t conn_id, espbt::ESPBTUUID service_uuid,
                            espbt::ESPBTUUID char_uuid) {
  // Use esp_ble_gattc_get_char_by_uuid()
  uint16_t count = 0;
  esp_gattc_char_elem_t result;
  auto svc = service_uuid.as_128bit().get_uuid();
  auto chr = char_uuid.as_128bit().get_uuid();

  esp_ble_gattc_get_char_by_uuid(
    this->gattc_if_, conn_id, svc, chr, &result, &count);

  if (count > 0) return result.char_handle;
  return 0;
}
```

**Step 2: Commit**

```bash
git add components/ember_onecontrol/ember_onecontrol.cpp
git commit -m "feat: implement characteristic discovery via ESP GATT APIs"
```

---

### Task 7: End-to-end integration test with example YAML

**Files:**
- Verify: `examples/ember_onecontrol.yaml` compiles
- Create: `tests/test_ember_onecontrol.py` (optional compile test)

**Step 1: Attempt compilation**

```bash
esphome compile examples/ember_onecontrol.yaml
```

Fix any compilation errors iteratively.

**Step 2: Fix any include path issues**

Sub-components include `esphome/components/ember_onecontrol/ember_onecontrol.h` — verify this resolves correctly now that the hub no longer includes ble_client headers.

**Step 3: Fix any API compatibility issues**

The `esp32_ble_tracker::ESPBTDevice` API for manufacturer data may vary by ESPHome version. Check:
- `device.get_manufacturer_datas()` returns vector of `ServiceData` with `.uuid` and `.data`
- Address access: `device.address_uint64()` or `device.address()`

**Step 4: Commit all fixes**

```bash
git add -A components/ember_onecontrol/
git commit -m "fix: compilation fixes for standalone BLE hub"
```

---

### Task 8: Final cleanup and design doc update

**Files:**
- Update: `docs/plans/2026-03-07-ember-onecontrol-design.md`
- Review: all files for consistency

**Step 1: Update design doc with final architecture**

**Step 2: Clean up any dead code from ble_client era**

Remove any remaining references to `ble_client::BLEClientNode`, `parent()->get_characteristic()`, `parent()->get_gattc_if()`, etc.

**Step 3: Final commit**

```bash
git add -A
git commit -m "chore: final cleanup, remove ble_client remnants, update design doc"
```

---

## Dependency Graph

```
Task 1 (hub rearchitect) ──┬──→ Task 2 (pairing button)
                            ├──→ Task 3 (sub-component updates)
                            ├──→ Task 5 (full .cpp implementation)
                            │         └──→ Task 6 (char discovery)
                            └──→ Task 4 (example YAML)

All tasks ──→ Task 7 (integration test) ──→ Task 8 (cleanup)
```

Tasks 2, 3, 4 can be done in parallel after Task 1. Task 5 is the heaviest. Task 6 is a refinement of Task 5. Tasks 7-8 are sequential at the end.
