# ESPHome Truma iNetBox + Ember OneControl

ESPHome components for RV (recreational vehicle) control:

- **`truma_inetbox`** — Control Truma CP Plus heaters via LIN bus (simulates a Truma iNet box)
- **`ember_onecontrol`** — Control LCI/Lippert Ember OneControl panels via BLE (lights, tanks, slides, awnings)

Both components can run on the same ESP32, providing a single Home Assistant integration for your entire RV.

## Acknowledgements

This project is based on the work of the [WomoLIN project](https://github.com/muccc/WomoLIN) and [mc0110 inetbox.py](https://github.com/danielfett/inetbox.py), especially the initial protocol decoding and the inet box log files.

The Ember OneControl component is based on reverse engineering of the [ha-onecontrol](https://github.com/IAmTheMitchell/ha-onecontrol) Home Assistant integration and the LCI OneControl BLE protocol.

## Supported Hardware

### Truma
- **Truma Combi 4/4E/6/6E/6 D E** with CP Plus controller
- **Truma VarioHeat** (untested)
- **Truma Aventa Eco** aircon (optional)
- **Alde heaters** (untested)

### Ember OneControl
- **LCI/Lippert OneControl panels** with BLE (PIN-based pairing variant, manufacturer ID `0x05C7`)
- Lights (ceiling, accent, step, awning), tank sensors, water pump, slide, awning
- Battery voltage and status monitoring

### Board
- **[ESP32-S3 CAN/LIN Bus Board](https://copperhilltech.com/esp32s3-can-lin-bus-board/)** recommended (has built-in LIN transceiver for Truma)
- Any ESP32 with BLE works for Ember-only setups

## Quick Start

### Installation

```yaml
external_components:
  - source: github://zorrobyte/esphome-truma_inetbox
    components: ["truma_inetbox", "ember_onecontrol"]
```

### Minimal Truma Configuration

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

uart:
  - id: lin_uart_bus
    tx_pin: 10
    rx_pin: 3
    baud_rate: 9600
    data_bits: 8
    parity: NONE
    stop_bits: 2

truma_inetbox:
  uart_id: lin_uart_bus
  cs_pin: 46
  fault_pin: 9

sensor:
  - platform: truma_inetbox
    name: "Current Room Temperature"
    type: CURRENT_ROOM_TEMPERATURE

climate:
  - platform: truma_inetbox
    name: "Truma Room"
    type: ROOM
```

### Minimal Ember OneControl Configuration

```yaml
esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf

ember_onecontrol:
  pin: "357694"  # 6-digit PIN from your OneControl gateway sticker

button:
  - platform: ember_onecontrol
    name: "Ember Start Pairing"

light:
  - platform: ember_onecontrol
    name: "Ceiling Light"
    type: CEILING_LIGHT

sensor:
  - platform: ember_onecontrol
    name: "Fresh Tank"
    type: TANK_FRESH
  - platform: ember_onecontrol
    name: "Battery Voltage"
    type: BATTERY_VOLTAGE
```

### Combined Configuration

See [truma-combi-6-d-e.yaml](/truma-combi-6-d-e.yaml) for a complete example with both Truma and Ember entities.

## Ember OneControl

### Pairing

1. Add the `ember_onecontrol` component with your gateway's 6-digit PIN (found on the gateway sticker)
2. Add an `ember_onecontrol` pairing button entity
3. Flash the firmware
4. In Home Assistant, press the "Ember Start Pairing" button
5. Within 60 seconds, hold the **CONNECT** button on your Ember panel until the LED blinks
6. Pairing completes automatically — the LED goes solid

After initial pairing, the bond persists across reboots and OTA updates. You should never need to press CONNECT again.

### Entities

#### Lights

```yaml
light:
  - platform: ember_onecontrol
    name: "Ceiling Light"
    type: CEILING_LIGHT
```

Available types: `CEILING_LIGHT`, `ACCENT_LIGHT`, `STEP_LIGHT`, `AWNING_LIGHT`

Lights support on/off and brightness control.

#### Sensors

```yaml
sensor:
  - platform: ember_onecontrol
    name: "Battery Voltage"
    type: BATTERY_VOLTAGE
```

Available types:
- `BATTERY_VOLTAGE` — House battery voltage (volts)
- `TANK_FRESH` — Fresh water tank level (%)
- `TANK_BLACK` — Black water tank level (%)
- `TANK_GREY` — Grey water tank level (%)

#### Text Sensors

```yaml
text_sensor:
  - platform: ember_onecontrol
    name: "Battery Status"
    type: BATTERY_STATUS
  - platform: ember_onecontrol
    name: "Connection Status"
    type: CONNECTION_STATUS
```

Available types:
- `BATTERY_STATUS` — "Charged", "Good", "Fair", or "Low"
- `CONNECTION_STATUS` — BLE connection state

#### Switches

```yaml
switch:
  - platform: ember_onecontrol
    name: "Water Pump"
    type: WATER_PUMP
```

Available types: `WATER_PUMP`

#### Slide & Awning (H-Bridge)

The slide and awning are H-Bridge devices that require continuous button presses — hold to move, release to stop. They are not simple on/off switches.

```yaml
binary_sensor:
  - platform: ember_onecontrol
    name: "Slide State"
    type: SLIDE_STATE
  - platform: ember_onecontrol
    name: "Awning State"
    type: AWNING_STATE
```

Available types: `SLIDE_STATE`, `AWNING_STATE`

### How It Works

The component uses the NimBLE BLE stack directly (bypassing ESPHome's Bluedroid abstractions) to connect to the OneControl panel. It uses Secure Connections with MITM protection (passkey-based) and stores bond keys in NVS for persistence across reboots. After bonding, it performs a TEA-encrypted challenge-response unlock, then subscribes to data notifications. Device metadata is fetched automatically to map function names to device addresses — no hardcoded device IDs needed.

The protocol uses COBS-encoded frames over BLE GATT characteristics with a negotiated 247-byte MTU. A 5-second heartbeat polls for device state updates.

No `esp32_ble` or `esp32_ble_tracker` components are needed — the Ember component manages its own BLE stack.

## Truma iNetBox

### Components

#### `truma_inetbox`

```yaml
truma_inetbox:
  uart_id: lin_uart_bus
  cs_pin: 46       # LIN transceiver chip select (optional)
  fault_pin: 9     # LIN transceiver fault indicator (optional)
  on_heater_message:
    then:
      - logger.log: "Message from CP Plus received."
```

#### Binary Sensors

Available types: `CP_PLUS_CONNECTED`, `HEATER_ROOM`, `HEATER_WATER`, `HEATER_GAS`, `HEATER_DIESEL`, `HEATER_MIX_1`, `HEATER_MIX_2`, `HEATER_ELECTRICITY`, `HEATER_HAS_ERROR`, `TIMER_ACTIVE`, `TIMER_ROOM`, `TIMER_WATER`

#### Climate

Available types:
- `ROOM` — Fan modes map to heating modes: LOW=ECO, MEDIUM=HIGH, HIGH=BOOST
- `WATER` — Target temps: 40, 60, 80 (step=20)
- `AIRCON` — For Truma Aventa aircon units

#### Number

Available types: `TARGET_ROOM_TEMPERATURE`, `TARGET_WATER_TEMPERATURE`, `ELECTRIC_POWER_LEVEL`, `AIRCON_MANUAL_TEMPERATURE`

#### Select

Available types: `HEATER_FAN_MODE_COMBI`, `HEATER_FAN_MODE_VARIO_HEAT`, `HEATER_ENERGY_MIX_GAS`, `HEATER_ENERGY_MIX_DIESEL`, `HEATER_ENERGY_MIX_PROPANE`, `AIRCON_MODE`, `AIRCON_VENT_MODE`

#### Sensor

Available types: `CURRENT_ROOM_TEMPERATURE`, `CURRENT_WATER_TEMPERATURE`, `TARGET_ROOM_TEMPERATURE`, `TARGET_WATER_TEMPERATURE`, `HEATING_MODE`, `ELECTRIC_POWER_LEVEL`, `ENERGY_MIX`, `OPERATING_STATUS`, `HEATER_ERROR_CODE`, `AIRCON_TARGET_TEMPERATURE`, `AIRCON_CURRENT_TEMPERATURE`, `AIRCON_MODE`, `AIRCON_VENT_MODE`

#### Actions

- `truma_inetbox.heater.set_target_room_temperature` — temperature (5-30, below 5 disables), heating_mode (optional: OFF/ECO/HIGH/BOOST)
- `truma_inetbox.heater.set_target_water_temperature` — temperature (0/40/60/80)
- `truma_inetbox.heater.set_target_water_temperature_enum` — temperature (OFF/ECO/HIGH/BOOST)
- `truma_inetbox.heater.set_electric_power_level` — watt (0/900/1800)
- `truma_inetbox.heater.set_energy_mix` — energy_mix (GAS/MIX/ELECTRICITY), watt (optional)
- `truma_inetbox.aircon.manual.set_target_temperature` — temperature (16-31, below 16 disables)
- `truma_inetbox.timer.disable` — Disable timer
- `truma_inetbox.timer.activate` — start, stop, room_temperature, heating_mode, water_temperature, energy_mix, watt
- `truma_inetbox.clock.set` — Sync CP Plus clock from ESP (requires a [time source](https://esphome.io/#time-components))

## Testing

A comprehensive end-to-end test harness is included for Truma validation:

```bash
pip install aioesphomeapi

# Default tests (connectivity + control)
python test_harness.py --timeout 45

# Full suite including switches, aircon, and stress tests
python test_harness.py --full --stress --timeout 45
```

Edit `DEVICE_HOST`, `DEVICE_PORT`, and `NOISE_PSK` at the top of `test_harness.py` to match your device.

## Known Limitations

### Truma
- **BOOST mode**: CP Plus only activates BOOST when target-current delta > 10C
- **LIN bus latency**: Commands take 7-30 seconds to be confirmed
- **Stale-state suppression**: 30-second window defers status updates while commands are in-flight

### Ember OneControl
- **First pairing requires physical access** to press the CONNECT button on the panel
- **PIN-based gateways only** (manufacturer ID `0x05C7`) — other variants may use different auth
- **ESP-IDF framework required** — Arduino framework is not supported for Ember BLE bonding

## TODO

- [ ] Testing of Combi 4E / Combi 6E and Alde devices
- [ ] VarioHeat testing
- [ ] Ember: dimming support for compatible lights
- [ ] Ember: slide/awning H-Bridge cover controls (hold-to-move)
- [ ] Ember: additional device types (generators, leveling jacks)
- [x] Combi 6 D E full end-to-end testing (97/97 pass)
- [x] Aircon (Aventa Eco) testing
- [x] Clock sync from Home Assistant
- [x] Ember OneControl BLE integration with bond persistence
- [x] Ember: lights, tanks, water pump, slide, awning
