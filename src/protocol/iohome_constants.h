/**
 * @file iohome_constants.h
 * @brief io-homecontrol Protocol Constants and Definitions
 * @author iown-homecontrol project
 *
 * Constants and definitions for the io-homecontrol protocol
 * including frame sizes, sync words, keys, and command IDs.
 *
 * All values are derived from the protocol documentation in `docs/`:
 * - `docs/linklayer.md` for the frame layout and control bytes
 * - `docs/commands.md` for the command IDs and their parameters
 */

#pragma once

#include <stdint.h>

namespace iohome {

// ============================================================================
// Physical Layer Constants
// ============================================================================

// Frequency Configuration
constexpr float FREQUENCY_CHANNEL_1 = 868.25f;  // MHz (2W only)
constexpr float FREQUENCY_CHANNEL_2 = 868.95f;  // MHz (1W/2W primary)
constexpr float FREQUENCY_CHANNEL_3 = 869.85f;  // MHz (2W only)

// Modulation Parameters
// NOTE: RadioLib's DataRate_t.fsk takes kbps / kHz, not bit/s / Hz.
constexpr float BIT_RATE = 38.4f;              // kbps
constexpr float FREQ_DEVIATION = 19.2f;        // kHz
constexpr uint16_t PREAMBLE_LENGTH = 512;      // bits

// Sync Word
// Raw OTA bit sequence ends with 0xFF33. Because io-homecontrol transmits
// LSB-first while RadioLib matches MSB-first, the sync word has to be given
// to the radio in its bit-reversed form: {0x57, 0xFD, 0x99}.
// SYNC_WORD keeps the documented OTA value; SYNC_WORD_BYTES is what is
// programmed into the transceiver. Never derive one from the other by shifting.
constexpr uint32_t SYNC_WORD = 0xFF33;
constexpr uint8_t SYNC_WORD_BYTES[] = {0x57, 0xFD, 0x99};
constexpr uint8_t SYNC_WORD_LEN = 3;

// Frequency Hopping
constexpr float CHANNEL_HOP_TIME_MS = 2.7f;    // milliseconds per channel

// ============================================================================
// Data Link Layer Constants
// ============================================================================

// Frame Field Sizes
constexpr uint8_t CTRL_BYTE_SIZE = 2;
constexpr uint8_t NODE_ID_SIZE = 3;
constexpr uint8_t COMMAND_ID_SIZE = 1;
constexpr uint8_t ROLLING_CODE_SIZE = 2;       // sequence number, 1W only
constexpr uint8_t HMAC_SIZE = 6;               // truncated AES MAC
constexpr uint8_t CRC_SIZE = 2;

// Header = ctrl0 + ctrl1 + dest + src + command
constexpr uint8_t FRAME_HEADER_SIZE =
  CTRL_BYTE_SIZE + (2 * NODE_ID_SIZE) + COMMAND_ID_SIZE;  // 9

// Control Byte 0 carries `Size` = frame length excluding Control Byte 0 and
// the CRC (see docs/linklayer.md "Control Byte 0 - Basic Frame Information").
// Therefore: total_frame_length = Size + FRAME_SIZE_FIELD_BIAS.
constexpr uint8_t FRAME_SIZE_FIELD_BIAS = 1 + CRC_SIZE;   // 3

// Smallest legal frame: header + CRC, no data, no authentication trailer.
constexpr uint8_t FRAME_MIN_SIZE = FRAME_HEADER_SIZE + CRC_SIZE;  // 11

// The size field is 5 bits wide, so the largest encodable frame is
// 31 + 3 = 34 bytes. Buffers must be sized accordingly.
constexpr uint8_t FRAME_SIZE_FIELD_MAX = 0x1F;
constexpr uint8_t FRAME_MAX_SIZE = FRAME_SIZE_FIELD_MAX + FRAME_SIZE_FIELD_BIAS;  // 34

// Maximum payload (command parameters) documented for io-homecontrol.
constexpr uint8_t FRAME_MAX_DATA_SIZE = 21;

// Overhead of the optional authentication trailer appended after the payload.
constexpr uint8_t AUTH_TRAILER_SIZE_1W = ROLLING_CODE_SIZE + HMAC_SIZE;  // 8
constexpr uint8_t AUTH_TRAILER_SIZE_2W = HMAC_SIZE;                      // 6

// Frame Field Offsets
constexpr uint8_t OFFSET_CTRL_BYTE_0 = 0;
constexpr uint8_t OFFSET_CTRL_BYTE_1 = 1;
constexpr uint8_t OFFSET_DEST_NODE = 2;
constexpr uint8_t OFFSET_SRC_NODE = 5;
constexpr uint8_t OFFSET_COMMAND_ID = 8;
constexpr uint8_t OFFSET_DATA = 9;

// Control Byte 0 Masks
// | BIT  | 7-6   | 5          | 4-0  |
// | NAME | Order | isOneWay   | Size |
constexpr uint8_t CTRL0_ORDER_MASK = 0xC0;      // bits 7-6
constexpr uint8_t CTRL0_ORDER_SHIFT = 6;
constexpr uint8_t CTRL0_ONE_WAY_MASK = 0x20;    // bit 5: 1 = 1W, 0 = 2W
constexpr uint8_t CTRL0_LENGTH_MASK = 0x1F;     // bits 4-0

/// @deprecated Kept for source compatibility; the bit means "isOneWay", not
/// "is 2W". Prefer CTRL0_ONE_WAY_MASK.
constexpr uint8_t CTRL0_PROTOCOL_MASK = CTRL0_ONE_WAY_MASK;

/**
 * @brief Command order relationship (Control Byte 0, bits 7-6)
 */
enum class FrameOrder : uint8_t {
  SINGLE = 0,            // Last/First session frame: No/No
  NEXT_IN_SERIES = 1,    // Last/First session frame: No/Yes
  NEXT_IN_PARALLEL = 2,  // Last/First session frame: Yes/No
  GROUP_END = 3          // Last/First session frame: Yes/Yes
};

// Control Byte 1 Masks
// | BIT  | 7          | 6      | 5   | 4   | 3   | 2   | 1-0              |
// | NAME | Use Beacon | Routed | LPM | Ack | ?   | ?   | Protocol Version |
constexpr uint8_t CTRL1_USE_BEACON = 0x80;      // bit 7
constexpr uint8_t CTRL1_ROUTED = 0x40;          // bit 6
constexpr uint8_t CTRL1_LOW_POWER = 0x20;       // bit 5
constexpr uint8_t CTRL1_ACK = 0x10;             // bit 4
constexpr uint8_t CTRL1_PROTOCOL_VERSION = 0x03; // bits 1-0

// Protocol Modes
constexpr uint8_t MODE_1W = 0x01;
constexpr uint8_t MODE_2W = 0x00;

// ============================================================================
// Cryptography Constants
// ============================================================================

// AES-128 Parameters
constexpr uint8_t AES_KEY_SIZE = 16;            // 128 bits
constexpr uint8_t AES_BLOCK_SIZE = 16;          // 128 bits
constexpr uint8_t IV_SIZE = 16;                 // 128 bits

// Transfer Key (hardcoded, used for key obfuscation during pairing).
// This is a protocol constant shared by every io-homecontrol device; it
// provides obfuscation, not confidentiality. See docs/SECURITY-MODEL.md.
constexpr uint8_t TRANSFER_KEY[AES_KEY_SIZE] = {
  0x34, 0xC3, 0x46, 0x6E, 0xD8, 0x8F, 0x4E, 0x8E,
  0x16, 0xAA, 0x47, 0x39, 0x49, 0x88, 0x43, 0x73
};

// IV Padding Value
constexpr uint8_t IV_PADDING = 0x55;

// CRC-16/KERMIT
constexpr uint16_t CRC_POLYNOMIAL = 0x8408;
constexpr uint16_t CRC_INITIAL = 0x0000;

// ============================================================================
// Command IDs (docs/commands.md - "Command IDs")
// ============================================================================

// Actuator Control
constexpr uint8_t CMD_EXECUTE = 0x00;           // Activate/Execute Function
constexpr uint8_t CMD_ACTIVATE_MODE = 0x01;     // Activate Mode
constexpr uint8_t CMD_MANUAL_ORDER = 0x02;      // Direct Command / Manual Order
constexpr uint8_t CMD_PRIVATE = 0x03;           // Private Command
constexpr uint8_t CMD_PRIVATE_ANSWER = 0x04;    // Private Command Answer

// Discovery Commands
constexpr uint8_t CMD_DISCOVER = 0x28;               // Discover
constexpr uint8_t CMD_DISCOVER_ANSWER = 0x29;        // Discover Answer
constexpr uint8_t CMD_DISCOVER_REMOTE = 0x2A;        // Discover Remote
constexpr uint8_t CMD_DISCOVER_REMOTE_ANSWER = 0x2B; // Discover Remote Answer
constexpr uint8_t CMD_DISCOVER_CONFIRM = 0x2C;       // Discover Actuator Confirmation
constexpr uint8_t CMD_DISCOVER_CONFIRM_ACK = 0x2D;   // Ack to 0x2C

// Key Exchange Commands
constexpr uint8_t CMD_SEND_1W_KEY = 0x30;       // Send encrypted 1W key
constexpr uint8_t CMD_ASK_CHALLENGE = 0x31;     // Ask challenge (answered with 0x3C)
constexpr uint8_t CMD_KEY_TRANSFER = 0x32;      // Encrypted 2W key transfer
constexpr uint8_t CMD_KEY_TRANSFER_ACK = 0x33;  // Ack to 0x32
constexpr uint8_t CMD_ADDRESS_REQUEST = 0x36;
constexpr uint8_t CMD_ADDRESS_ANSWER = 0x37;
constexpr uint8_t CMD_LAUNCH_KEY_TRANSFER = 0x38;
constexpr uint8_t CMD_REMOVE_1W_CONTROLLER = 0x39;

// Authentication Commands
constexpr uint8_t CMD_CHALLENGE_REQUEST = 0x3C;
constexpr uint8_t CMD_CHALLENGE_RESPONSE = 0x3D;

// File / Script Commands (docs/commands.md "46", "47", "4A")
constexpr uint8_t CMD_SCRIPT_UPLOAD = 0x46;
constexpr uint8_t CMD_DOWNLOAD_CONFIG = 0x47;
constexpr uint8_t CMD_RENAME_FILE = 0x4A;

// Naming / Info Commands
constexpr uint8_t CMD_GET_NAME = 0x50;
constexpr uint8_t CMD_GET_NAME_ANSWER = 0x51;
constexpr uint8_t CMD_WRITE_NAME = 0x52;
constexpr uint8_t CMD_WRITE_NAME_ACK = 0x53;
constexpr uint8_t CMD_GET_INFO_1 = 0x54;
constexpr uint8_t CMD_INFO_1_ANSWER = 0x55;
constexpr uint8_t CMD_GET_INFO_2 = 0x56;
constexpr uint8_t CMD_INFO_2_ANSWER = 0x57;

// Bootloader Commands
constexpr uint8_t CMD_BOOTLOADER_START = 0xE0;
constexpr uint8_t CMD_BOOTLOADER_DATA = 0xE1;

// Service Commands (docs/commands.md "Fx: Service Commands")
//
// CMD_SERVICE_RESET used to sit at 0xF1. Reboot is 0xF2; 0xF1 is "read groups"
// / service ACK, so the old constant named one command and addressed another.
// scripts/io-homecontrol.ksy agrees: 0xf0 send_raw_message, 0xf2 reboot,
// 0xf3 service_status_ack, and nothing at 0xf1.
constexpr uint8_t CMD_SEND_RAW_MESSAGE = 0xF0;   // "Find Hardware" / service
constexpr uint8_t CMD_READ_GROUPS = 0xF1;        // service ACK
constexpr uint8_t CMD_REBOOT = 0xF2;             // reboot / service status
constexpr uint8_t CMD_SERVICE_STATUS_ACK = 0xF3;

/// @deprecated Misleading name for 0xF0; use CMD_SEND_RAW_MESSAGE.
constexpr uint8_t CMD_SERVICE_PING = CMD_SEND_RAW_MESSAGE;

// ============================================================================
// Command 0x00 (Execute) payload
// ============================================================================
//
// | CMD | Originator | ACEI | Main Parameter | FP1 | FP2 | [FP3 ... FPn] |
// |  1  |     1      |  1   |       2        |  1  |  1  |    0 .. 14    |
//
// The payload following the command ID is *at least* 6 bytes. docs/commands.md
// documents up to 16 functional parameters, and captures show both lengths:
//
//   docs/commands.md, 1W example:  DATA(14) 01 67 d2 00 00 00 | SEQ | MAC
//                                           `- 6 payload bytes
//   scripts/io-homecontrol.ksy:    27-byte frame, 8 payload bytes
//                                  01 61 d4 00 80 c8 00 00 | 3b d5 | MAC
//
// Treating 6 as an exact length made the second frame fall through the frame
// parser's precise length test into its guess-a-trailer fallback.
constexpr uint8_t EXECUTE_PAYLOAD_MIN_SIZE = 6;

/// Bytes before the first functional parameter: originator, ACEI, main parameter.
constexpr uint8_t EXECUTE_PAYLOAD_PREFIX_SIZE = 4;

/// Largest number of functional parameters docs/commands.md describes.
constexpr uint8_t EXECUTE_MAX_FUNCTIONAL_PARAMS = 16;

/// Deprecated spelling of EXECUTE_PAYLOAD_MIN_SIZE, kept so existing callers
/// keep compiling. The value is a minimum, not an exact size.
constexpr uint8_t EXECUTE_PAYLOAD_SIZE = EXECUTE_PAYLOAD_MIN_SIZE;

/**
 * @brief Command originator - what or who fired the command
 */
enum class Originator : uint8_t {
  LOCAL_USER = 0x00,       // Button press on the actuator itself
  USER = 0x01,             // User remote control (default for controllers)
  SENSOR_RAIN = 0x02,
  SENSOR_TIMER = 0x03,
  SECURITY = 0x04,
  UPS = 0x05,
  SFC = 0x06,
  LSC = 0x07,
  SAAC = 0x08,             // Stand Alone Automatic Controls
  SENSOR_WIND = 0x09,
  UNKNOWN = 0x0A,
  LOAD_SHEDDING = 0x0B,
  SENSOR_LIGHT = 0x0C,
  SENSOR_ENVIRONMENT = 0x0D,
  MYSELF = 0x10,           // Actuator moved by itself
  AUTOMATIC_CYCLE = 0xFE,
  EMERGENCY = 0xFF
};

// ACEI byte: | Level 7-5 | Service 4-3 | Extended Info 2-1 | IsValid 0 |
constexpr uint8_t ACEI_VALID_MASK = 0x01;
constexpr uint8_t ACEI_EXTENDED_INFO_MASK = 0x06;
constexpr uint8_t ACEI_SERVICE_MASK = 0x18;
constexpr uint8_t ACEI_LEVEL_MASK = 0xE0;
constexpr uint8_t ACEI_LEVEL_SHIFT = 5;

/**
 * @brief ACEI priority level (bits 7-5)
 */
enum class PriorityLevel : uint8_t {
  HUMAN_PROTECTION = 0,      // Most secure level, disables all categories
  ENVIRONMENT_PROTECTION = 1,
  USER_LEVEL_1 = 2,          // Higher priority controller
  USER_LEVEL_2 = 3,          // Default for remote controllers
  COMFORT_LEVEL_1 = 4,
  COMFORT_LEVEL_2 = 5,
  COMFORT_LEVEL_3_SAAC = 6,
  COMFORT_LEVEL_4 = 7
};

/**
 * @brief Build an ACEI byte.
 *
 * Bit 0 (IsValid) is always set: "The frame is not considered valid if this
 * bit is set to 0" (docs/commands.md).
 *
 * @param level Priority level (bits 7-5)
 * @param service Priority service number 0-3 (bits 4-3)
 * @param extended_info Extended info 0-3 (bits 2-1)
 */
constexpr uint8_t make_acei(PriorityLevel level = PriorityLevel::USER_LEVEL_2,
                            uint8_t service = 0,
                            uint8_t extended_info = 0) {
  return static_cast<uint8_t>(
    ((static_cast<uint8_t>(level) << ACEI_LEVEL_SHIFT) & ACEI_LEVEL_MASK) |
    ((service << 3) & ACEI_SERVICE_MASK) |
    ((extended_info << 1) & ACEI_EXTENDED_INFO_MASK) |
    ACEI_VALID_MASK);
}

/// Default ACEI for a remote controller: user level 2, valid. (= 0x61)
constexpr uint8_t ACEI_DEFAULT = make_acei();

/// An ACEI byte is only accepted by actuators when bit 0 is set.
constexpr bool is_acei_valid(uint8_t acei) { return (acei & ACEI_VALID_MASK) != 0; }

// ============================================================================
// Main Parameter values (docs/commands.md - "Standard Values")
// ============================================================================

constexpr uint16_t MP_MIN = 0x0000;             // Min / On / 1W button up
constexpr uint16_t MP_OPEN = 0x0000;            // Alias: fully open
constexpr uint16_t MP_1W_BUTTON_DOWN = 0x0001;
constexpr uint16_t MP_1W_BUTTON_STOP = 0x0002;
constexpr uint16_t MP_1W_BUTTON_PROG = 0x0003;
constexpr uint16_t MP_BUTTON_RELEASED = 0x00FE;
constexpr uint16_t MP_BUTTON_STOP = 0x00FF;
constexpr uint16_t MP_MAX = 0xC800;             // Max / Off / Down
constexpr uint16_t MP_CLOSE = 0xC800;           // Alias: fully closed
constexpr uint16_t MP_TARGET = 0xD100;          // Execution parameter buffer: target
constexpr uint16_t MP_CURRENT = 0xD200;         // Execution parameter buffer: current
constexpr uint16_t MP_STOP = 0xD200;            // Alias: stop at current position
constexpr uint16_t MP_DEFAULT = 0xD300;         // Relative / target / current default

/// "MP: Ignore / FP: ReadOnly" - leave this parameter alone and act on the
/// others. This is the main parameter in the capture in
/// scripts/io-homecontrol.ksy, which sets functional parameters only.
constexpr uint16_t MP_IGNORE = 0xD400;

constexpr uint16_t MP_RUNNING = 0x6E00;
constexpr uint16_t MP_PLUS_MINUS_DEFAULT = 0x7D00;
constexpr uint16_t MP_RETRY = 0xE000;
constexpr uint16_t MP_UNKNOWN_FEEDBACK = 0xF7FF;

// Relative percentage range: 0x0000 (0 %) .. 0xC800 (100 %)
constexpr uint16_t MP_PERCENT_MAX_RAW = 0xC800;

// Signed percentage range: 0xC900 (-100 %) .. 0xD0D0 (+100 %)
constexpr uint16_t MP_SIGNED_PERCENT_MIN = 0xC900;
constexpr uint16_t MP_SIGNED_PERCENT_MAX = 0xD0D0;

/**
 * @brief Convert a percentage (0-100) into a Main Parameter value.
 *
 * The io-homecontrol percentage range is 0x0000..0xC800 where 0x0000 is
 * "open/min" and 0xC800 is "closed/max".
 *
 * @param percent_closed 0 = fully open, 100 = fully closed
 */
constexpr uint16_t mp_from_percent_closed(uint8_t percent_closed) {
  return static_cast<uint16_t>(
    (percent_closed >= 100) ? MP_PERCENT_MAX_RAW
                            : (static_cast<uint32_t>(percent_closed) * MP_PERCENT_MAX_RAW) / 100u);
}

/**
 * @brief Inverse of mp_from_percent_closed(). Returns 0-100, or 0xFF if the
 *        value is not inside the percentage range.
 */
constexpr uint8_t percent_closed_from_mp(uint16_t mp) {
  return (mp > MP_PERCENT_MAX_RAW)
           ? 0xFF
           : static_cast<uint8_t>((static_cast<uint32_t>(mp) * 100u + MP_PERCENT_MAX_RAW / 2u) /
                                  MP_PERCENT_MAX_RAW);
}

// ============================================================================
// Device Types (Actuator Subtypes)
// ============================================================================

enum class DeviceType : uint8_t {
  ROLLER_SHUTTER = 0x00,
  ADJUSTABLE_SLAT_SHUTTER = 0x01,
  SCREEN = 0x02,
  WINDOW_OPENER = 0x03,
  VENETIAN_BLIND = 0x04,
  EXTERIOR_BLIND = 0x05,
  DUAL_SHUTTER = 0x06,
  GARAGE_DOOR = 0x07,
  AWNING = 0x08,
  CURTAIN = 0x09,
  PERGOLA = 0x0A,
  HORIZONTAL_AWNING = 0x0B,
  EXTERIOR_SCREEN = 0x0C,
  LIGHT = 0x0D,
  LOCK = 0x0E,
  HEATING = 0x0F,
  GATE = 0x10,
  BEACON = 0x11,
  SENSOR = 0x12
};

// ============================================================================
// Well-known Addresses (docs/linklayer.md - "Addresses (NodeId)")
// ============================================================================

/// Broadcast address used by controllers when addressing every actuator.
constexpr uint8_t ADDRESS_BROADCAST[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};
/// Alternative all-ones broadcast address.
constexpr uint8_t ADDRESS_BROADCAST_ALL[NODE_ID_SIZE] = {0xFF, 0xFF, 0xFF};
/// Group address.
constexpr uint8_t ADDRESS_GROUP[NODE_ID_SIZE] = {0x00, 0x00, 0x00};
/// Source address used for point-to-point and broadcast frames.
constexpr uint8_t ADDRESS_P2P_SOURCE[NODE_ID_SIZE] = {0xFF, 0xFF, 0xFE};

/// @deprecated {0,0,0} is the *group* address; use ADDRESS_BROADCAST instead.
constexpr uint8_t BROADCAST_ADDRESS[NODE_ID_SIZE] = {0x00, 0x00, 0x3F};

} // namespace iohome
