# ESPHome truma_inetbox component

ESPHome component to remote control Truma CP Plus Heater by simulating a Truma iNet box.

See [1](https://github.com/danielfett/inetbox.py) and [2](https://github.com/mc0110/inetbox2mqtt) for great documentation about how to connect a CP Plus to an ESP32 or RP2040.

## Acknowledgements

This project is based on the work of the [WomoLIN project](https://github.com/muccc/WomoLIN) and [mc0110 inetbox.py](https://github.com/danielfett/inetbox.py), especially the initial protocol decoding and the inet box log files.

## Supported Hardware

Tested with:
- **Truma Combi 6 D E** with CP Plus controller
- **ESP32-S3** (ESP32S3_CAN_LIN_rev_B board) with LIN transceiver
- **Truma Aventa Eco** aircon (optional)

Should also work with Combi 4/4E/6/6E, VarioHeat, and Alde heaters (untested).

## Example Configuration

This example is just for connecting ESPHome to the CP Plus. See [truma-combi-6-d-e.yaml](/truma-combi-6-d-e.yaml) for a complete example with all entities, template switches, energy mix selects, timer control, clock sync, and aircon support.

```yaml
esphome:
  name: "esphome-truma"

external_components:
  - source: github://zorrobyte/esphome-truma_inetbox
    ref: feature/ember-onecontrol
    components: ["truma_inetbox"]

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: arduino

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

binary_sensor:
  - platform: truma_inetbox
    name: "CP Plus alive"
    type: CP_PLUS_CONNECTED

sensor:
  - platform: truma_inetbox
    name: "Current Room Temperature"
    type: CURRENT_ROOM_TEMPERATURE
  - platform: truma_inetbox
    name: "Current Water Temperature"
    type: CURRENT_WATER_TEMPERATURE
  - platform: truma_inetbox
    name: "Target Room Temperature"
    type: TARGET_ROOM_TEMPERATURE
  - platform: truma_inetbox
    name: "Target Water Temperature"
    type: TARGET_WATER_TEMPERATURE
```

## ESPHome components

This project contains the following ESPHome components:

- `truma_inetbox` has the following settings:
  - `cs_pin` (optional) if you connect the pin of your lin driver chip.
  - `fault_pin` (optional) if you connect the pin of your lin driver chip.
  - `on_heater_message` (optional) [ESPHome Trigger](https://esphome.io/guides/automations.html) when a message from CP Plus is recieved.

Requires ESP Home 2023.4 or higher.

### Binary sensor

Binary sensors are read-only.

```yaml
binary_sensor:
  - platform: truma_inetbox
    name: "CP Plus alive"
    type: CP_PLUS_CONNECTED
```

The following `type` values are available:

- `CP_PLUS_CONNECTED`
- `HEATER_ROOM`
- `HEATER_WATER`
- `HEATER_GAS`
- `HEATER_DIESEL`
- `HEATER_MIX_1`
- `HEATER_MIX_2`
- `HEATER_ELECTRICITY`
- `HEATER_HAS_ERROR`
- `TIMER_ACTIVE`
- `TIMER_ROOM`
- `TIMER_WATER`

### Climate

Climate components support read and write.

```yaml
climate:
  - platform: truma_inetbox
    name: "Truma Room"
    type: ROOM
  - platform: truma_inetbox
    name: "Truma Water"
    type: WATER
```

The following `type` values are available:

- `ROOM` — Fan modes map to heating modes: LOW=ECO, MEDIUM=HIGH, HIGH=BOOST
- `WATER` — Target temps: 40°C, 60°C, 80°C (step=20)
- `AIRCON` — For Truma Aventa aircon units

### Number

Number components support read and write.

```yaml
number:
  - platform: truma_inetbox
    name: "Target Room Temperature"
    type: TARGET_ROOM_TEMPERATURE
```

The following `type` values are available:

- `TARGET_ROOM_TEMPERATURE`
- `TARGET_WATER_TEMPERATURE`
- `ELECTRIC_POWER_LEVEL`
- `AIRCON_MANUAL_TEMPERATURE`

### Select

Select components support read and write.

```yaml
select:
  - platform: truma_inetbox
    name: "Fan Mode"
    type: HEATER_FAN_MODE_COMBI
```

The following `type` values are available:

- `HEATER_FAN_MODE_COMBI`
- `HEATER_FAN_MODE_VARIO_HEAT`
- `HEATER_ENERGY_MIX_GAS`
- `HEATER_ENERGY_MIX_DIESEL`
- `HEATER_ENERGY_MIX_PROPANE` — For Combi 6 D E: Propane, Mix 1, Mix 2, Electric 1, Electric 2
- `AIRCON_MODE` — Off, Ventilation, Cooling, Heating, Auto
- `AIRCON_VENT_MODE` — Low, Mid, High, Night, Auto

### Sensor

Sensors are read-only.

```yaml
sensor:
  - platform: truma_inetbox
    name: "Current Room Temperature"
    type: CURRENT_ROOM_TEMPERATURE
```

The following `type` values are available:

- `CURRENT_ROOM_TEMPERATURE`
- `CURRENT_WATER_TEMPERATURE`
- `TARGET_ROOM_TEMPERATURE`
- `TARGET_WATER_TEMPERATURE`
- `HEATING_MODE` — 0=OFF, 1=ECO, 10=HIGH, 11=BOOST
- `ELECTRIC_POWER_LEVEL` — 0, 900, or 1800 watts
- `ENERGY_MIX` — 0=NONE, 1=GAS/PROPANE, 2=ELECTRICITY, 3=MIX
- `OPERATING_STATUS` — 0=OFF, 1=WARNING, 4=START/COOLDOWN, 5-9=ON stages
- `HEATER_ERROR_CODE`
- `AIRCON_TARGET_TEMPERATURE`
- `AIRCON_CURRENT_TEMPERATURE`
- `AIRCON_MODE`
- `AIRCON_VENT_MODE`

### Actions

The following [ESP Home actions](https://esphome.io/guides/automations.html#actions) are available:

- `truma_inetbox.heater.set_target_room_temperature`
  - `temperature` - Temperature between 5C and 30C. Below 5C will disable the Heater.
  - `heating_mode` - Optional set heating mode: `"OFF"`, `ECO`, `HIGH`, `BOOST`.
- `truma_inetbox.heater.set_target_water_temperature`
  - `temperature` - Set water temp as number: `0`, `40`, `60`, `80`.
- `truma_inetbox.heater.set_target_water_temperature_enum`
  - `temperature` - Set water temp as text: `"OFF"`, `ECO`, `HIGH`, `BOOST`.
- `truma_inetbox.heater.set_electric_power_level`
  - `watt` - Set electricity level to `0`, `900`, `1800`.
- `truma_inetbox.heater.set_energy_mix`
  - `energy_mix` - Set energy mix to: `GAS`, `MIX`, `ELECTRICITY`.
  - `watt` - Optional: Set electricity level to `0`, `900`, `1800`
- `truma_inetbox.aircon.manual.set_target_temperature`
  - `temperature` - Temperature between 16C and 31C. Below 16C will disable the Aircon.
- `truma_inetbox.timer.disable` - Disable the timer configuration.
- `truma_inetbox.timer.activate` - Set a new timer configuration.
  - `start` - Start time.
  - `stop` - Stop time.
  - `room_temperature` - Temperature between 5C and 30C.
  - `heating_mode` - Optional: Set heating mode: `"OFF"`, `ECO`, `HIGH`, `BOOST`.
  - `water_temperature` - Optional: Set water temp as number: `0`, `40`, `60`, `80`.
  - `energy_mix` - Optional: Set energy mix to: `GAS`, `MIX`, `ELECTRICITY`.
  - `watt` - Optional: Set electricity level to `0`, `900`, `1800`.
- `truma_inetbox.clock.set` - Update CP Plus from ESP Home. You *must* have another [clock source](https://esphome.io/#time-components) configured like Home Assistant Time, GPS or DS1307 RTC.

## Testing

A comprehensive end-to-end test harness is included for validating firmware against a live device:

- **`test_harness.py`** — 13 test groups, 97+ test cases covering connectivity, sensors, climate control, number entities, energy mix, power level, switches, timers, aircon, and debounce stress testing
- **`quick_test.py`** — Targeted tests for specific scenarios (BOOST mode, energy mix, timer)

### Running Tests

```bash
# Install dependency
pip install aioesphomeapi

# Default tests (connectivity + control, tests 1-9, 13)
python test_harness.py --timeout 45

# Full suite including switches, aircon, and stress tests
python test_harness.py --full --stress --timeout 45

# Single test
python test_harness.py --test 5  # Room climate only

# Read-only (no commands sent)
python test_harness.py --no-control
```

Edit `DEVICE_HOST`, `DEVICE_PORT`, and `NOISE_PSK` at the top of `test_harness.py` to match your device.

### Test Results (Truma Combi 6 D E + Aventa Eco)

- **97 PASS, 0 FAIL, 2 WARN, 0 SKIP** across `--full --stress`
- BOOST WARN is expected: requires target-current room temp > 10°C (Truma hardware limitation)
- Water toggle WARN is expected: rapid ON/OFF/ON produces extra state changes due to stale-state suppression

## Known Limitations

- **BOOST mode**: CP Plus only activates BOOST when the delta between target and current room temperature exceeds 10°C (per Truma manual)
- **LIN bus latency**: Commands take 7-30 seconds to be confirmed by CP Plus
- **Stale-state suppression**: A 30-second window defers publishing status updates while commands are in-flight to prevent UI bounce

## TODO

- [ ] Testing of Combi 4E / Combi 6E and Alde devices
- [ ] VarioHeat testing
- [x] Combi 6 D E full end-to-end testing (97/97 pass)
- [x] Aircon (Aventa Eco) testing
- [x] Clock sync from Home Assistant
- [x] Stale-state suppression for UI stability
