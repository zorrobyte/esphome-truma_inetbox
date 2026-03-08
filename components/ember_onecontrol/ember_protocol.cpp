#include "ember_protocol.h"

namespace esphome {
namespace ember_onecontrol {

// CRC8 lookup table (polynomial 0x07, initial value 0x55)
static const uint8_t CRC8_TABLE[256] = {
    0,   94,  188, 226, 97,  63,  221, 131, 194, 156, 126, 32,  163, 253, 31,  65,
    157, 195, 33,  127, 252, 164, 64,  30,  95,  1,   227, 189, 62,  96,  130, 220,
    35,  125, 159, 193, 66,  28,  254, 162, 225, 191, 93,  3,   128, 222, 60,  98,
    190, 224, 2,   92,  223, 129, 99,  61,  124, 34,  192, 158, 29,  67,  161, 255,
    70,  24,  250, 164, 39,  121, 155, 197, 132, 218, 56,  102, 229, 187, 89,  7,
    219, 133, 103, 57,  186, 228, 6,   88,  25,  71,  165, 251, 120, 38,  196, 154,
    101, 59,  217, 135, 4,   90,  184, 230, 167, 249, 27,  69,  198, 152, 122, 36,
    248, 166, 68,  26,  153, 199, 37,  123, 58,  100, 134, 216, 91,  5,   231, 185,
    140, 210, 48,  110, 237, 179, 81,  15,  78,  16,  242, 172, 47,  113, 147, 205,
    17,  79,  173, 243, 112, 46,  204, 146, 211, 141, 111, 49,  178, 236, 14,  80,
    175, 241, 19,  77,  206, 144, 114, 44,  109, 51,  209, 143, 12,  82,  176, 238,
    50,  108, 142, 208, 83,  13,  239, 177, 240, 174, 76,  18,  145, 207, 45,  115,
    202, 148, 118, 40,  171, 245, 23,  73,  8,   86,  180, 234, 105, 55,  213, 139,
    87,  9,   235, 181, 54,  104, 138, 212, 149, 203, 41,  119, 244, 170, 72,  22,
    233, 183, 85,  11,  136, 214, 52,  106, 43,  117, 151, 201, 74,  20,  246, 168,
    116, 42,  200, 150, 21,  75,  169, 247, 182, 232, 10,  84,  215, 137, 107, 53,
};

uint8_t crc8_calculate(const uint8_t *data, size_t length) {
  uint8_t crc = 85;  // 0x55 initial value
  for (size_t i = 0; i < length; i++) {
    crc = CRC8_TABLE[(crc ^ data[i]) & 0xFF];
  }
  return crc;
}

uint32_t tea_encrypt(uint32_t cypher, uint32_t seed) {
  uint32_t delta = TEA_DELTA;
  uint32_t c = cypher;
  uint32_t s = seed;

  for (uint32_t i = 0; i < TEA_ROUNDS; i++) {
    s += (((c << 4) + TEA_CONSTANT_1) ^ (c + delta) ^ ((c >> 5) + TEA_CONSTANT_2));
    c += (((s << 4) + TEA_CONSTANT_3) ^ (s + delta) ^ ((s >> 5) + TEA_CONSTANT_4));
    delta += TEA_DELTA;
  }

  return s;
}

void build_auth_key(const uint8_t *seed_bytes, const std::string &pin, uint32_t cypher, uint8_t *out_key) {
  // Parse seed as little-endian uint32
  uint32_t seed_value = seed_bytes[0] | (seed_bytes[1] << 8) | (seed_bytes[2] << 16) | (seed_bytes[3] << 24);

  uint32_t encrypted = tea_encrypt(cypher, seed_value);

  // Build 16-byte key: [encrypted LE 4 bytes][PIN ASCII 6 bytes][zeros 6 bytes]
  memset(out_key, 0, 16);
  out_key[0] = encrypted & 0xFF;
  out_key[1] = (encrypted >> 8) & 0xFF;
  out_key[2] = (encrypted >> 16) & 0xFF;
  out_key[3] = (encrypted >> 24) & 0xFF;

  size_t pin_len = pin.length();
  if (pin_len > 6) pin_len = 6;
  memcpy(out_key + 4, pin.c_str(), pin_len);
}

void calculate_unlock_response(const uint8_t *challenge, uint8_t *out_response) {
  // Parse challenge as big-endian uint32
  uint32_t seed = ((uint32_t) challenge[0] << 24) | ((uint32_t) challenge[1] << 16) |
                  ((uint32_t) challenge[2] << 8) | ((uint32_t) challenge[3]);

  // Compute TEA encrypt with Step 1 cipher (unlock challenge)
  uint32_t encrypted = tea_encrypt(STEP1_CIPHER, seed);

  // Output as big-endian
  out_response[0] = (encrypted >> 24) & 0xFF;
  out_response[1] = (encrypted >> 16) & 0xFF;
  out_response[2] = (encrypted >> 8) & 0xFF;
  out_response[3] = encrypted & 0xFF;
}

// COBS byte-by-byte decoder
bool CobsByteDecoder::decode_byte(uint8_t b) {
  if (b == 0x00) {
    // Frame terminator
    if (code_byte_ != 0 || dest_index_ <= 1) {
      reset();
      return false;
    }
    // Verify CRC
    uint8_t received_crc = output_buffer_[dest_index_ - 1];
    dest_index_--;
    uint8_t calculated_crc = crc8_calculate(output_buffer_, dest_index_);
    if (received_crc != calculated_crc) {
      reset();
      return false;
    }
    result_.assign(output_buffer_, output_buffer_ + dest_index_);
    reset();
    return true;
  }

  if (code_byte_ <= 0) {
    code_byte_ = b;
  } else {
    code_byte_--;
    if (dest_index_ < (int) sizeof(output_buffer_)) {
      output_buffer_[dest_index_++] = b;
    }
  }

  if ((code_byte_ & MAX_DATA_BYTES) == 0) {
    while (code_byte_ > 0) {
      if (dest_index_ < (int) sizeof(output_buffer_)) {
        output_buffer_[dest_index_++] = 0x00;
      }
      code_byte_ -= FRAME_BYTE_COUNT_LSB;
    }
  }

  return false;
}

void CobsByteDecoder::reset() {
  code_byte_ = 0;
  dest_index_ = 0;
}

// COBS encoder with CRC8
std::vector<uint8_t> cobs_encode(const uint8_t *data, size_t length, bool prepend_start_frame) {
  std::vector<uint8_t> output;
  output.reserve(length + 10);

  if (prepend_start_frame) {
    output.push_back(0x00);
  }

  if (length == 0) {
    return output;
  }

  // Calculate CRC of source data
  uint8_t crc_state = 85;  // 0x55
  for (size_t i = 0; i < length; i++) {
    crc_state = CRC8_TABLE[(crc_state ^ data[i]) & 0xFF];
  }

  size_t total_count = length + 1;  // +1 for CRC byte
  size_t src_index = 0;

  while (src_index < total_count) {
    size_t code_index = output.size();
    int code = 0;
    output.push_back(0xFF);  // Placeholder

    while (src_index < total_count) {
      uint8_t byte_val;
      if (src_index < length) {
        byte_val = data[src_index];
        if (byte_val == 0x00) break;
      } else {
        byte_val = crc_state;
        if (byte_val == 0x00) break;
      }
      src_index++;
      output.push_back(byte_val);
      code++;
      if (code >= 63) break;
    }

    while (src_index < total_count) {
      uint8_t byte_val = (src_index < length) ? data[src_index] : crc_state;
      if (byte_val != 0x00) break;
      src_index++;
      code += 64;
      if (code >= 192) break;
    }

    output[code_index] = (uint8_t) code;
  }

  output.push_back(0x00);  // Frame terminator
  return output;
}

std::vector<uint8_t> build_get_devices(uint16_t command_id, uint8_t table_id,
                                        uint8_t start_device_id, uint8_t max_count) {
  return {
      (uint8_t)(command_id & 0xFF),
      (uint8_t)((command_id >> 8) & 0xFF),
      0x01,  // GetDevices
      table_id,
      start_device_id,
      max_count,
  };
}

std::vector<uint8_t> build_get_devices_metadata(uint16_t command_id, uint8_t table_id,
                                                 uint8_t start_device_id, uint8_t max_count) {
  return {
      (uint8_t)(command_id & 0xFF),
      (uint8_t)((command_id >> 8) & 0xFF),
      0x02,  // GetDevicesMetadata
      table_id,
      start_device_id,
      max_count,
  };
}

std::vector<uint8_t> build_action_switch(uint16_t command_id, uint8_t table_id,
                                          bool turn_on, const std::vector<uint8_t> &device_ids) {
  std::vector<uint8_t> cmd;
  cmd.push_back(command_id & 0xFF);
  cmd.push_back((command_id >> 8) & 0xFF);
  cmd.push_back(0x40);  // ActionSwitch
  cmd.push_back(table_id);
  cmd.push_back(turn_on ? 0x01 : 0x00);
  for (uint8_t id : device_ids) {
    cmd.push_back(id);
  }
  return cmd;
}

}  // namespace ember_onecontrol
}  // namespace esphome
