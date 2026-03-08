#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace esphome {
namespace ember_onecontrol {

// BLE Service UUIDs
static const char *AUTH_SERVICE_UUID = "00000010-0200-a58e-e411-afe28044e62c";
static const char *SEED_CHAR_UUID = "00000011-0200-a58e-e411-afe28044e62c";
static const char *UNLOCK_STATUS_CHAR_UUID = "00000012-0200-a58e-e411-afe28044e62c";
static const char *KEY_CHAR_UUID = "00000013-0200-a58e-e411-afe28044e62c";

static const char *DATA_SERVICE_UUID = "00000030-0200-a58e-e411-afe28044e62c";
static const char *DATA_WRITE_CHAR_UUID = "00000033-0200-a58e-e411-afe28044e62c";
static const char *DATA_READ_CHAR_UUID = "00000034-0200-a58e-e411-afe28044e62c";

// TEA Encryption Constants
static const uint32_t TEA_DELTA = 0x9E3779B9UL;
static const uint32_t TEA_CONSTANT_1 = 0x436F7079UL;
static const uint32_t TEA_CONSTANT_2 = 0x72696768UL;
static const uint32_t TEA_CONSTANT_3 = 0x74204944UL;
static const uint32_t TEA_CONSTANT_4 = 0x53736E63UL;
static const uint32_t TEA_ROUNDS = 32;
static const uint32_t STEP1_CIPHER = 0x248431D5UL;  // For unlock challenge (big-endian)
static const uint32_t STEP2_CIPHER = 0x8100080DUL;  // For seed auth key (little-endian)

// MyRvLink Event Types
enum MyRvLinkEventType : uint8_t {
  EVENT_GATEWAY_INFORMATION = 0x01,
  EVENT_DEVICE_COMMAND = 0x02,
  EVENT_DEVICE_ONLINE_STATUS = 0x03,
  EVENT_DEVICE_LOCK_STATUS = 0x04,
  EVENT_RELAY_LATCHING_STATUS_TYPE1 = 0x05,
  EVENT_RELAY_LATCHING_STATUS_TYPE2 = 0x06,
  EVENT_RV_STATUS = 0x07,
  EVENT_DIMMABLE_LIGHT_STATUS = 0x08,
  EVENT_RGB_LIGHT_STATUS = 0x09,
  EVENT_GENERATOR_STATUS = 0x0A,
  EVENT_HVAC_STATUS = 0x0B,
  EVENT_TANK_SENSOR_STATUS = 0x0C,
  EVENT_HBRIDGE_STATUS_TYPE1 = 0x0D,
  EVENT_HBRIDGE_STATUS_TYPE2 = 0x0E,
  EVENT_TANK_SENSOR_STATUS_V2 = 0x1B,
};

// TEA Encryption
uint32_t tea_encrypt(uint32_t cypher, uint32_t seed);

// Build 16-byte auth key from seed notification
void build_auth_key(const uint8_t *seed_bytes, const std::string &pin, uint32_t cypher, uint8_t *out_key);

// Calculate unlock challenge response (big-endian, for UNLOCK_STATUS flow)
void calculate_unlock_response(const uint8_t *challenge, uint8_t *out_response);

// CRC8
uint8_t crc8_calculate(const uint8_t *data, size_t length);

// COBS byte-by-byte decoder (stateful)
class CobsByteDecoder {
 public:
  // Feed one byte. Returns true if a complete frame was decoded.
  bool decode_byte(uint8_t b);

  // Get the last decoded frame (valid only after decode_byte returns true)
  const std::vector<uint8_t> &get_frame() const { return result_; }

  void reset();

 private:
  static const int MAX_DATA_BYTES = 63;
  static const int FRAME_BYTE_COUNT_LSB = 64;
  int code_byte_ = 0;
  uint8_t output_buffer_[382];
  int dest_index_ = 0;
  std::vector<uint8_t> result_;
};

// COBS encoder with CRC8
std::vector<uint8_t> cobs_encode(const uint8_t *data, size_t length, bool prepend_start_frame = true);

// Command builders - return raw command bytes (before COBS encoding)
std::vector<uint8_t> build_get_devices(uint16_t command_id, uint8_t table_id,
                                        uint8_t start_device_id = 0, uint8_t max_count = 0xFF);
std::vector<uint8_t> build_get_devices_metadata(uint16_t command_id, uint8_t table_id,
                                                 uint8_t start_device_id = 0, uint8_t max_count = 0xFF);
std::vector<uint8_t> build_action_switch(uint16_t command_id, uint8_t table_id,
                                          bool turn_on, const std::vector<uint8_t> &device_ids);

}  // namespace ember_onecontrol
}  // namespace esphome
