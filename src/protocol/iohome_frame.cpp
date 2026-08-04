/**
 * @file iohome_frame.cpp
 * @brief io-homecontrol Frame Construction and Parsing Implementation
 * @author iown-homecontrol project
 */

#include "iohome_frame.h"
#include "iohome_crypto.h"
#include <string.h>
#include <stdio.h>

namespace iohome {
namespace frame {
namespace {

/**
 * @brief Recompute frame_length and the Control Byte 0 size field.
 * @return false if the frame does not fit into the 5-bit size field.
 */
bool refresh_length(IoFrame* frame) {
  size_t total = FRAME_HEADER_SIZE + frame->data_len + CRC_SIZE;
  if (frame->authenticated) {
    total += auth_trailer_size(frame->is_1w_mode);
  }

  if (total < FRAME_MIN_SIZE || total > FRAME_MAX_SIZE) {
    return false;
  }

  frame->frame_length = static_cast<uint8_t>(total);
  frame->ctrl_byte_0 = set_frame_length(frame->ctrl_byte_0, frame->frame_length);
  return true;
}

/// Copy command id + parameters into the buffer the MAC is computed over.
size_t build_mac_input(const IoFrame* frame, uint8_t out[1 + FRAME_MAX_DATA_SIZE]) {
  out[0] = frame->command_id;
  if (frame->data_len > 0) {
    memcpy(&out[1], frame->data, frame->data_len);
  }
  return 1u + frame->data_len;
}

} // namespace

// ============================================================================
// Frame Construction
// ============================================================================

void init_frame(IoFrame* frame, bool is_1w) {
  if (frame == nullptr) {
    return;
  }
  memset(frame, 0, sizeof(IoFrame));
  frame->is_1w_mode = is_1w;
  frame->authenticated = false;

  // Control Byte 0: order = SINGLE, isOneWay bit set for 1W, size filled in
  // once the payload is known.
  frame->ctrl_byte_0 = is_1w ? CTRL0_ONE_WAY_MASK : 0x00;
  frame->ctrl_byte_1 = 0x00;

  frame->frame_length = FRAME_MIN_SIZE;
  frame->ctrl_byte_0 = set_frame_length(frame->ctrl_byte_0, frame->frame_length);
}

void set_order(IoFrame* frame, FrameOrder order) {
  if (frame == nullptr) {
    return;
  }
  frame->ctrl_byte_0 = static_cast<uint8_t>(
    (frame->ctrl_byte_0 & ~CTRL0_ORDER_MASK) |
    ((static_cast<uint8_t>(order) << CTRL0_ORDER_SHIFT) & CTRL0_ORDER_MASK));
}

void set_destination(IoFrame* frame, const uint8_t node_id[NODE_ID_SIZE]) {
  if (frame == nullptr || node_id == nullptr) {
    return;
  }
  memcpy(frame->dest_node, node_id, NODE_ID_SIZE);
}

void set_source(IoFrame* frame, const uint8_t node_id[NODE_ID_SIZE]) {
  if (frame == nullptr || node_id == nullptr) {
    return;
  }
  memcpy(frame->src_node, node_id, NODE_ID_SIZE);
}

bool set_command(IoFrame* frame, uint8_t cmd_id, const uint8_t* params, size_t params_len) {
  if (frame == nullptr) {
    return false;
  }

  if (params_len > FRAME_MAX_DATA_SIZE) {
    return false; // Parameters too large for the frame structure
  }

  if (params == nullptr && params_len > 0) {
    return false;
  }

  const uint8_t previous_len = frame->data_len;

  frame->command_id = cmd_id;
  frame->data_len = static_cast<uint8_t>(params_len);

  if (params_len > 0) {
    memcpy(frame->data, params, params_len);
  }

  if (!refresh_length(frame)) {
    // Roll back so the frame stays consistent after a rejected update.
    frame->data_len = previous_len;
    refresh_length(frame);
    return false;
  }

  return true;
}

bool set_execute_command(IoFrame* frame,
                         uint16_t main_param,
                         Originator originator,
                         uint8_t acei,
                         uint8_t fp1,
                         uint8_t fp2) {
  if (frame == nullptr) {
    return false;
  }

  // Actuators discard frames whose ACEI "IsValid" bit is clear.
  if (!is_acei_valid(acei)) {
    return false;
  }

  const uint8_t params[EXECUTE_PAYLOAD_SIZE] = {
    static_cast<uint8_t>(originator),
    acei,
    static_cast<uint8_t>((main_param >> 8) & 0xFF),
    static_cast<uint8_t>(main_param & 0xFF),
    fp1,
    fp2
  };

  return set_command(frame, CMD_EXECUTE, params, sizeof(params));
}

void set_rolling_code(IoFrame* frame, uint16_t code) {
  if (frame == nullptr) {
    return;
  }
  frame->rolling_code[0] = code & 0xFF;         // LSB first
  frame->rolling_code[1] = (code >> 8) & 0xFF;
}

uint16_t get_rolling_code(const IoFrame* frame) {
  if (frame == nullptr) {
    return 0;
  }
  return static_cast<uint16_t>(frame->rolling_code[0] |
                               (static_cast<uint16_t>(frame->rolling_code[1]) << 8));
}

bool finalize_frame(IoFrame* frame, const uint8_t system_key[AES_KEY_SIZE], const uint8_t* challenge) {
  if (frame == nullptr || system_key == nullptr) {
    return false;
  }

  if (frame->data_len > FRAME_MAX_DATA_SIZE) {
    return false;
  }

  if (!frame->is_1w_mode && challenge == nullptr) {
    return false; // 2W MAC is bound to a challenge
  }

  const bool was_authenticated = frame->authenticated;
  frame->authenticated = true;
  if (!refresh_length(frame)) {
    frame->authenticated = was_authenticated;
    refresh_length(frame);
    return false;
  }

  // MAC is computed over command ID + parameters.
  uint8_t mac_input[1 + FRAME_MAX_DATA_SIZE];
  const size_t mac_input_len = build_mac_input(frame, mac_input);

  bool hmac_success;
  if (frame->is_1w_mode) {
    hmac_success = crypto::create_1w_hmac(mac_input, mac_input_len, frame->rolling_code,
                                          system_key, frame->hmac);
  } else {
    hmac_success = crypto::create_2w_hmac(mac_input, mac_input_len, challenge,
                                          system_key, frame->hmac);
  }

  if (!hmac_success) {
    return false;
  }

  // Serialize to a scratch buffer so the CRC covers the exact wire bytes.
  uint8_t temp_buffer[FRAME_MAX_SIZE];
  const size_t frame_len = serialize_frame(frame, temp_buffer, sizeof(temp_buffer));
  if (frame_len == 0) {
    return false;
  }

  const uint16_t crc_value = crypto::compute_crc16(temp_buffer, frame_len - CRC_SIZE);
  frame->crc[0] = crc_value & 0xFF;          // LSB first
  frame->crc[1] = (crc_value >> 8) & 0xFF;

  return true;
}

bool finalize_frame_plain(IoFrame* frame) {
  if (frame == nullptr) {
    return false;
  }

  if (frame->data_len > FRAME_MAX_DATA_SIZE) {
    return false;
  }

  const bool was_authenticated = frame->authenticated;
  frame->authenticated = false;
  if (!refresh_length(frame)) {
    frame->authenticated = was_authenticated;
    refresh_length(frame);
    return false;
  }

  memset(frame->hmac, 0, HMAC_SIZE);

  uint8_t temp_buffer[FRAME_MAX_SIZE];
  const size_t frame_len = serialize_frame(frame, temp_buffer, sizeof(temp_buffer));
  if (frame_len == 0) {
    return false;
  }

  const uint16_t crc_value = crypto::compute_crc16(temp_buffer, frame_len - CRC_SIZE);
  frame->crc[0] = crc_value & 0xFF;
  frame->crc[1] = (crc_value >> 8) & 0xFF;

  return true;
}

size_t serialize_frame(const IoFrame* frame, uint8_t* buffer, size_t buffer_size) {
  if (frame == nullptr || buffer == nullptr) {
    return 0;
  }

  if (frame->data_len > FRAME_MAX_DATA_SIZE) {
    return 0;
  }

  size_t required = FRAME_HEADER_SIZE + frame->data_len + CRC_SIZE;
  if (frame->authenticated) {
    required += auth_trailer_size(frame->is_1w_mode);
  }

  if (required != frame->frame_length) {
    return 0; // Frame metadata is inconsistent - refuse to emit garbage
  }

  if (buffer_size < required) {
    return 0; // Buffer too small
  }

  size_t offset = 0;

  buffer[offset++] = frame->ctrl_byte_0;
  buffer[offset++] = frame->ctrl_byte_1;

  memcpy(&buffer[offset], frame->dest_node, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;

  memcpy(&buffer[offset], frame->src_node, NODE_ID_SIZE);
  offset += NODE_ID_SIZE;

  buffer[offset++] = frame->command_id;

  if (frame->data_len > 0) {
    memcpy(&buffer[offset], frame->data, frame->data_len);
    offset += frame->data_len;
  }

  if (frame->authenticated) {
    if (frame->is_1w_mode) {
      memcpy(&buffer[offset], frame->rolling_code, ROLLING_CODE_SIZE);
      offset += ROLLING_CODE_SIZE;
    }
    memcpy(&buffer[offset], frame->hmac, HMAC_SIZE);
    offset += HMAC_SIZE;
  }

  memcpy(&buffer[offset], frame->crc, CRC_SIZE);
  offset += CRC_SIZE;

  return offset;
}

// ============================================================================
// Frame Parsing
// ============================================================================

bool parse_frame(const uint8_t* buffer, size_t buffer_len, IoFrame* frame, AuthTrailer trailer) {
  if (buffer == nullptr || frame == nullptr) {
    return false;
  }

  if (buffer_len < FRAME_MIN_SIZE) {
    return false; // Frame too short
  }

  memset(frame, 0, sizeof(IoFrame));

  frame->ctrl_byte_0 = buffer[OFFSET_CTRL_BYTE_0];
  frame->ctrl_byte_1 = buffer[OFFSET_CTRL_BYTE_1];
  frame->is_1w_mode = is_1w_mode(frame->ctrl_byte_0);
  frame->frame_length = get_frame_length(frame->ctrl_byte_0);

  if (frame->frame_length < FRAME_MIN_SIZE || frame->frame_length > FRAME_MAX_SIZE) {
    return false; // Length field cannot describe a legal frame
  }

  if (buffer_len < frame->frame_length) {
    return false; // Buffer doesn't contain the complete frame
  }

  size_t offset = CTRL_BYTE_SIZE;

  memcpy(frame->dest_node, &buffer[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;

  memcpy(frame->src_node, &buffer[offset], NODE_ID_SIZE);
  offset += NODE_ID_SIZE;

  frame->command_id = buffer[offset++];

  // Everything between the command byte and the CRC is payload plus an
  // optional authentication trailer.
  const size_t payload_len = frame->frame_length - FRAME_HEADER_SIZE - CRC_SIZE;
  const uint8_t trailer_len = auth_trailer_size(frame->is_1w_mode);

  switch (trailer) {
    case AuthTrailer::NONE:
      frame->authenticated = false;
      break;
    case AuthTrailer::PRESENT:
      if (payload_len < trailer_len) {
        return false; // Frame is too short to carry the trailer it claims
      }
      frame->authenticated = true;
      break;
    case AuthTrailer::AUTO:
    default:
      // Authenticated 1W frames append a sequence number and a MAC. 2W frames
      // seen in the wild (discovery, acks, execute) carry no MAC, so only
      // assume a trailer for 1W.
      frame->authenticated = frame->is_1w_mode && (payload_len >= trailer_len);
      break;
  }

  const size_t data_len = frame->authenticated ? (payload_len - trailer_len) : payload_len;

  if (data_len > FRAME_MAX_DATA_SIZE) {
    return false; // Payload larger than the structure can hold
  }

  frame->data_len = static_cast<uint8_t>(data_len);

  if (frame->data_len > 0) {
    memcpy(frame->data, &buffer[offset], frame->data_len);
    offset += frame->data_len;
  }

  if (frame->authenticated) {
    if (frame->is_1w_mode) {
      memcpy(frame->rolling_code, &buffer[offset], ROLLING_CODE_SIZE);
      offset += ROLLING_CODE_SIZE;
    }
    memcpy(frame->hmac, &buffer[offset], HMAC_SIZE);
    offset += HMAC_SIZE;
  }

  memcpy(frame->crc, &buffer[offset], CRC_SIZE);
  offset += CRC_SIZE;

  return offset == frame->frame_length;
}

bool validate_frame(const IoFrame* frame, const uint8_t* system_key, const uint8_t* challenge) {
  if (frame == nullptr) {
    return false;
  }

  // Serialize so the CRC is checked against the exact wire representation.
  uint8_t temp_buffer[FRAME_MAX_SIZE];
  const size_t frame_len = serialize_frame(frame, temp_buffer, sizeof(temp_buffer));
  if (frame_len == 0) {
    return false;
  }

  if (!crypto::verify_crc16(temp_buffer, frame_len)) {
    return false;
  }

  if (system_key == nullptr) {
    return true; // Caller opted out of MAC verification
  }

  if (!frame->authenticated) {
    // Nothing to verify: a plain frame carries no MAC. Report success so
    // callers can decide by policy whether plain frames are acceptable.
    return true;
  }

  const uint8_t* seq_or_challenge = frame->is_1w_mode ? frame->rolling_code : challenge;
  if (seq_or_challenge == nullptr) {
    return false; // 2W MAC cannot be checked without the challenge
  }

  uint8_t mac_input[1 + FRAME_MAX_DATA_SIZE];
  const size_t mac_input_len = build_mac_input(frame, mac_input);

  return crypto::verify_hmac(mac_input, mac_input_len, frame->hmac, seq_or_challenge,
                             system_key, !frame->is_1w_mode);
}

// ============================================================================
// Helper Functions
// ============================================================================

bool is_broadcast(const uint8_t node_id[NODE_ID_SIZE]) {
  if (node_id == nullptr) {
    return false;
  }
  return memcmp(node_id, ADDRESS_BROADCAST, NODE_ID_SIZE) == 0 ||
         memcmp(node_id, ADDRESS_BROADCAST_ALL, NODE_ID_SIZE) == 0 ||
         memcmp(node_id, ADDRESS_GROUP, NODE_ID_SIZE) == 0;
}

void print_frame(const IoFrame* frame, void (*print_func)(const char*)) {
  if (frame == nullptr || print_func == nullptr) {
    return;
  }

  char buf[128];

  snprintf(buf, sizeof(buf), "Frame: %s mode, %s, order %u",
           frame->is_1w_mode ? "1W" : "2W",
           frame->authenticated ? "authenticated" : "plain",
           static_cast<unsigned>(get_order(frame->ctrl_byte_0)));
  print_func(buf);

  snprintf(buf, sizeof(buf), "  Length: %u bytes", static_cast<unsigned>(frame->frame_length));
  print_func(buf);

  snprintf(buf, sizeof(buf), "  Dest: %02X %02X %02X",
           frame->dest_node[0], frame->dest_node[1], frame->dest_node[2]);
  print_func(buf);

  snprintf(buf, sizeof(buf), "  Src:  %02X %02X %02X",
           frame->src_node[0], frame->src_node[1], frame->src_node[2]);
  print_func(buf);

  snprintf(buf, sizeof(buf), "  Cmd:  0x%02X", frame->command_id);
  print_func(buf);

  if (frame->data_len > 0) {
    // 3 chars per byte plus terminator.
    char hex[3 * FRAME_MAX_DATA_SIZE + 1];
    size_t used = 0;
    for (size_t i = 0; i < frame->data_len && i < FRAME_MAX_DATA_SIZE; i++) {
      used += snprintf(hex + used, sizeof(hex) - used, "%02X ", frame->data[i]);
    }
    snprintf(buf, sizeof(buf), "  Data: %s", hex);
    print_func(buf);
  }

  if (frame->authenticated) {
    if (frame->is_1w_mode) {
      snprintf(buf, sizeof(buf), "  Sequence: %02X %02X (%u)",
               frame->rolling_code[0], frame->rolling_code[1],
               static_cast<unsigned>(get_rolling_code(frame)));
      print_func(buf);
    }

    char mac_str[3 * HMAC_SIZE + 1];
    size_t used = 0;
    for (uint8_t i = 0; i < HMAC_SIZE; i++) {
      used += snprintf(mac_str + used, sizeof(mac_str) - used, "%02X ", frame->hmac[i]);
    }
    snprintf(buf, sizeof(buf), "  MAC:  %s", mac_str);
    print_func(buf);
  }

  snprintf(buf, sizeof(buf), "  CRC:  %02X %02X", frame->crc[0], frame->crc[1]);
  print_func(buf);
}

} // namespace frame
} // namespace iohome
