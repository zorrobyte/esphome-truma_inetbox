# Ember RV OneControl BLE Component — Design

## Goal

ESPHome component that connects an ESP32 to an Ember RV OneControl panel over BLE, with seamless push-to-pair and full entity exposure in Home Assistant.

## Panel Entities (from physical panel)

| Panel Feature | HA Entity Type | Writable | Notes |
|---|---|---|---|
| Accent Light | `light` (binary) | Yes | Toggle ON/OFF via 0x40 ActionSwitch |
| Step Light | `light` (binary) | Yes | Toggle ON/OFF |
| Pump Light | `light` (binary) | Yes | Toggle ON/OFF |
| Awning Light | `light` (binary) | Yes | Toggle ON/OFF |
| Ceiling Light | `light` (binary) | Yes | Toggle ON/OFF |
| Water Pump | `switch` | Yes | Toggle ON/OFF via 0x40 ActionSwitch |
| Slide In/Out | `binary_sensor` | No | Read-only (safety) |
| Awning Ret/Ext | `binary_sensor` | No | Read-only (safety) |
| Battery Voltage | `sensor` (V) | No | From 0x07 RvStatus |
| Battery Status | `text_sensor` | No | Charged/Good/Fair/Low |
| Fresh Tank | `sensor` (%) | No | From 0x0C/0x1B tank events |
| Black Tank | `sensor` (%) | No | From 0x0C/0x1B tank events |
| Grey Tank | `sensor` (%) | No | From 0x0C/0x1B tank events |
| Connection Status | `text_sensor` | No | Disconnected/Scanning/Authenticating/Connected |

## Seamless Pairing Design

### No MAC address required in config

The component does NOT use `ble_client`. It implements its own BLE scanning and GATT client using `esp32_ble_tracker` listener API + direct `esp_ble_gattc_*` calls.

### Pairing flow

1. User presses **"Start Pairing"** button entity in HA
2. ESP32 enters discovery mode (60-second timeout)
3. ESP32 scans BLE advertisements for Lippert manufacturer ID `0x0499`
4. User holds CONNECT on Ember panel — panel sets bit 1 (pairing active) in advertisement
5. ESP32 detects pairing-active panel, saves MAC, initiates BLE connection
6. Auth state machine runs:
   - Read UNLOCK_STATUS → 4-byte challenge (big-endian)
   - TEA encrypt challenge → write 4-byte response to KEY (WRITE_NO_RESPONSE)
   - Re-read UNLOCK_STATUS → should say "Unlocked"
   - Enable notifications on SEED (0x0011) and DATA_READ (0x0034)
   - Receive SEED notification → build 16-byte auth key (TEA(seed) LE + PIN ASCII + zeros)
   - Write auth key to KEY characteristic
   - Authenticated — data flows
7. MAC persisted to ESP32 NVS flash via ESPHome preferences
8. Future reboots: auto-reconnect to saved MAC, no scanning needed

### Safety

- Scanner is idle by default — only active during 60-second window after button press
- Only connects to panels broadcasting pairing-active flag
- In RV parks, neighbor would need to hold THEIR button during YOUR 60-second window

## Minimal YAML Config

```yaml
esp32_ble_tracker:

ember_onecontrol:
  pin: "090336"  # Optional, default works

button:
  - platform: ember_onecontrol
    name: "Start Pairing"

light:
  - platform: ember_onecontrol
    name: "Ceiling Light"
    type: CEILING_LIGHT
# ... etc
```

## Protocol (ported from ble-plugin-bridge Kotlin)

- **TEA**: 32-round Tiny Encryption Algorithm, cypher 0x8100080D
- **COBS**: Consistent Overhead Byte Stuffing with CRC8 (initial 0x55, poly 0x07)
- **Commands**: COBS-encoded, written to DATA_WRITE (0x0033)
- **Events**: COBS byte-stream on DATA_READ (0x0034) notifications
- **Event dispatch**: 0x05/0x06→relays, 0x07→RV status, 0x0C/0x1B→tanks, 0x0D/0x0E→H-bridges

## Architecture

```
EmberOneControl (hub)
  ├── implements ESPBTDeviceListener (for scanning)
  ├── manages esp_ble_gattc_* directly (no ble_client)
  ├── CobsByteDecoder (stateful frame decoder)
  ├── auth state machine
  ├── event dispatch → callbacks
  └── NVS MAC persistence

EmberLight, EmberSwitch, EmberSensor, etc.
  └── register callbacks with hub, send commands through hub

EmberPairingButton
  └── triggers discovery mode on hub
```
