/**
 * @file iohome_frame.h
 * @brief io-homecontrol Frame Construction and Parsing
 * @author iown-homecontrol project
 *
 * Functions for constructing and parsing io-homecontrol protocol frames.
 *
 * Wire layout (see docs/linklayer.md):
 *
 *   | ctrl0 | ctrl1 | dest[3] | src[3] | cmd | data[0..n] | [seq[2]] | [mac[6]] | crc[2] |
 *
 * The sequence number and MAC form the optional *authentication trailer*.
 * It is present in authenticated 1W frames (seq + MAC) and in authenticated
 * 2W frames (MAC only, the challenge takes the place of the sequence number).
 * Plain frames - discovery, key transfer, acks - carry neither.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iohome_constants.h"

namespace iohome {
namespace frame {

// ============================================================================
// Frame Structure
// ============================================================================

/**
 * @brief io-homecontrol Frame structure
 */
struct IoFrame {
  // Control Bytes
  uint8_t ctrl_byte_0;              // Order, Protocol Mode, Size
  uint8_t ctrl_byte_1;              // Beacon, Routed, Low Power, ACK, Version

  // Addresses
  uint8_t dest_node[NODE_ID_SIZE];  // Destination Node ID (3 bytes)
  uint8_t src_node[NODE_ID_SIZE];   // Source Node ID (3 bytes)

  // Command and Data
  uint8_t command_id;               // Command ID (1 byte)
  uint8_t data[FRAME_MAX_DATA_SIZE]; // Parameters (0-21 bytes)
  uint8_t data_len;                 // Actual length of data

  // Authentication trailer (only meaningful when `authenticated` is true)
  uint8_t rolling_code[ROLLING_CODE_SIZE]; // Sequence number (1W only)
  uint8_t hmac[HMAC_SIZE];          // Truncated AES MAC (6 bytes)

  // CRC
  uint8_t crc[CRC_SIZE];            // CRC-16/KERMIT (2 bytes, LSB first)

  // Metadata
  bool is_1w_mode;                  // true = 1W, false = 2W
  bool authenticated;               // true if seq/MAC trailer is present
  uint8_t frame_length;             // Total frame length in bytes
};

/**
 * @brief How parse_frame() should interpret the tail of the payload.
 */
enum class AuthTrailer : uint8_t {
  /**
   * Infer from the command ID and the payload length.
   *
   * Nothing on the wire says whether a frame carries an authentication
   * trailer, so it has to be deduced. Two facts make that possible:
   *
   *  - Some commands are defined as unauthenticated because the peers have not
   *    agreed on a key yet - discovery, key transfer and their acks.
   *  - Most commands have a fixed parameter length, so a payload that is
   *    exactly `parameters + trailer` long can only be the authenticated form.
   *
   * This resolves every capture in docs/ correctly. Use NONE or PRESENT when
   * the caller knows better.
   */
  AUTO,
  /// Payload is data only; no sequence number and no MAC.
  NONE,
  /// Force an authentication trailer appropriate for the frame's mode.
  PRESENT
};

/**
 * @brief Whether a command is defined to travel without a MAC
 *
 * These are the bootstrap commands: the peers have no shared key yet, so there
 * is nothing to authenticate with. Every other command should be authenticated.
 */
bool is_unauthenticated_command(uint8_t command_id);

/**
 * @brief Parameter length for commands whose payload has a fixed size
 *
 * @return Length in bytes, or -1 when the command's payload length varies
 */
int expected_payload_size(uint8_t command_id);

/**
 * @brief Smallest valid parameter length for a command
 *
 * Same as expected_payload_size() for fixed-length commands. For the
 * variable-length ones - Execute and Activate Mode, which may carry extra
 * functional parameters - this reports the minimum instead of -1, which is
 * what lets the parser tell a long parameter block apart from parameters
 * followed by an authentication trailer.
 *
 * @return Length in bytes, or -1 when the command is not documented
 */
int min_payload_size(uint8_t command_id);

// ============================================================================
// Frame Construction
// ============================================================================

/**
 * @brief Initialize a frame with default values
 *
 * @param frame Pointer to IoFrame structure
 * @param is_1w true for 1W mode, false for 2W mode
 */
void init_frame(IoFrame* frame, bool is_1w = true);

/**
 * @brief Set the command order relationship (Control Byte 0, bits 7-6)
 *
 * @param frame Pointer to IoFrame structure
 * @param order Order relationship
 */
void set_order(IoFrame* frame, FrameOrder order);

/**
 * @brief Set destination node address
 *
 * @param frame Pointer to IoFrame structure
 * @param node_id Node ID (3 bytes)
 */
void set_destination(IoFrame* frame, const uint8_t node_id[NODE_ID_SIZE]);

/**
 * @brief Set source node address
 *
 * @param frame Pointer to IoFrame structure
 * @param node_id Node ID (3 bytes)
 */
void set_source(IoFrame* frame, const uint8_t node_id[NODE_ID_SIZE]);

/**
 * @brief Set command ID and parameters
 *
 * Recomputes the frame length and the Control Byte 0 size field. Fails if the
 * resulting frame would not fit into the 5-bit size field.
 *
 * @param frame Pointer to IoFrame structure
 * @param cmd_id Command ID
 * @param params Pointer to parameter data (can be nullptr)
 * @param params_len Length of parameters
 * @return true on success, false if params_len is too large
 */
bool set_command(IoFrame* frame, uint8_t cmd_id, const uint8_t* params = nullptr, size_t params_len = 0);

/**
 * @brief Build the payload of command 0x00 (Activate/Execute Function)
 *
 * Layout: originator(1) | acei(1) | main parameter(2, MSB first) | fp1(1) | fp2(1)
 *
 * @param frame Pointer to IoFrame structure
 * @param main_param Main parameter (e.g. MP_OPEN, MP_CLOSE, MP_STOP)
 * @param originator Command originator
 * @param acei ACEI byte; bit 0 must be set or actuators reject the frame
 * @param fp1 Functional parameter 1
 * @param fp2 Functional parameter 2
 * @return true on success, false on invalid arguments
 */
bool set_execute_command(IoFrame* frame,
                         uint16_t main_param,
                         Originator originator = Originator::USER,
                         uint8_t acei = ACEI_DEFAULT,
                         uint8_t fp1 = 0x00,
                         uint8_t fp2 = 0x00);

/**
 * @brief Build a command 0x00 payload with an arbitrary number of functional
 *        parameters
 *
 * Layout: originator(1) | acei(1) | main parameter(2, MSB first) | fp[0..n-1]
 *
 * set_execute_command() covers the two-parameter form that most actuators use.
 * Some carry more - the capture in scripts/io-homecontrol.ksy has four - and
 * actuator types that expose several channels need them.
 *
 * @param frame      Pointer to IoFrame structure
 * @param main_param Main parameter (e.g. MP_OPEN, MP_CLOSE, MP_STOP)
 * @param originator Command originator
 * @param acei       ACEI byte; bit 0 must be set or actuators reject the frame
 * @param fps        Functional parameters, or nullptr when @p fp_count is 0
 * @param fp_count   Number of functional parameters. At least 2 (the protocol
 *                   always carries FP1 and FP2) and at most
 *                   EXECUTE_MAX_FUNCTIONAL_PARAMS.
 * @return true on success; false on invalid arguments or if the resulting
 *         frame would not fit the 5-bit size field
 */
bool set_execute_command_fp(IoFrame* frame,
                            uint16_t main_param,
                            Originator originator,
                            uint8_t acei,
                            const uint8_t* fps,
                            size_t fp_count);

/**
 * @brief Set rolling code (1W mode only)
 *
 * @param frame Pointer to IoFrame structure
 * @param code Rolling code value (0-65535), stored LSB first
 */
void set_rolling_code(IoFrame* frame, uint16_t code);

/**
 * @brief Read the rolling code as a 16-bit value
 *
 * @param frame Pointer to IoFrame structure
 * @return Rolling code value, or 0 if `frame` is null
 */
uint16_t get_rolling_code(const IoFrame* frame);

/**
 * @brief Finalize an authenticated frame by calculating MAC and CRC
 *
 * Must be called after all frame fields are set. Marks the frame as
 * authenticated so serialization emits the sequence number and MAC.
 *
 * @param frame Pointer to IoFrame structure
 * @param system_key System key for MAC calculation (16 bytes)
 * @param challenge Challenge for 2W mode (must be non-null in 2W mode)
 * @return true on success, false on error
 */
bool finalize_frame(IoFrame* frame, const uint8_t system_key[AES_KEY_SIZE], const uint8_t* challenge = nullptr);

/**
 * @brief Finalize a plain (unauthenticated) frame by calculating only the CRC
 *
 * Used for discovery, key transfer and acknowledgement frames, which carry no
 * MAC because the peers have not agreed on a key yet.
 *
 * @param frame Pointer to IoFrame structure
 * @return true on success, false on error
 */
bool finalize_frame_plain(IoFrame* frame);

/**
 * @brief Serialize frame to byte buffer
 *
 * @param frame Pointer to IoFrame structure
 * @param buffer Output buffer
 * @param buffer_size Size of output buffer
 * @return Number of bytes written, or 0 on error
 */
size_t serialize_frame(const IoFrame* frame, uint8_t* buffer, size_t buffer_size);

// ============================================================================
// Frame Parsing
// ============================================================================

/**
 * @brief Parse a received frame from a byte buffer
 *
 * @param buffer Input buffer
 * @param buffer_len Length of input buffer
 * @param frame Output IoFrame structure
 * @param trailer How to interpret the tail of the payload
 * @return true on success, false on parse error
 */
bool parse_frame(const uint8_t* buffer, size_t buffer_len, IoFrame* frame,
                 AuthTrailer trailer = AuthTrailer::AUTO);

/**
 * @brief Validate frame (CRC and, for authenticated frames, the MAC)
 *
 * @param frame Pointer to IoFrame structure
 * @param system_key System key for MAC verification (nullptr to skip MAC check)
 * @param challenge Challenge for 2W mode (required to verify a 2W MAC)
 * @return true if frame is valid, false otherwise
 */
bool validate_frame(const IoFrame* frame, const uint8_t* system_key = nullptr, const uint8_t* challenge = nullptr);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Get protocol mode from control byte
 *
 * Control Byte 0 bit 5 is `isOneWay`: 1 = 1W, 0 = 2W.
 *
 * @param ctrl_byte_0 Control byte 0
 * @return true if 1W mode, false if 2W mode
 */
inline bool is_1w_mode(uint8_t ctrl_byte_0) {
  return (ctrl_byte_0 & CTRL0_ONE_WAY_MASK) != 0;
}

/**
 * @brief Get protocol mode from control byte
 *
 * @param ctrl_byte_0 Control byte 0
 * @return true if 2W mode, false if 1W mode
 */
inline bool is_2w_mode(uint8_t ctrl_byte_0) {
  return (ctrl_byte_0 & CTRL0_ONE_WAY_MASK) == 0;
}

/**
 * @brief Get the command order relationship from control byte 0
 */
inline FrameOrder get_order(uint8_t ctrl_byte_0) {
  return static_cast<FrameOrder>((ctrl_byte_0 & CTRL0_ORDER_MASK) >> CTRL0_ORDER_SHIFT);
}

/**
 * @brief Get total frame length from control byte 0
 *
 * The `Size` field holds the frame length excluding Control Byte 0 and the
 * CRC, so the total length is `Size + 3`.
 *
 * @param ctrl_byte_0 Control byte 0
 * @return Total frame length in bytes (3-34)
 */
inline uint8_t get_frame_length(uint8_t ctrl_byte_0) {
  return static_cast<uint8_t>((ctrl_byte_0 & CTRL0_LENGTH_MASK) + FRAME_SIZE_FIELD_BIAS);
}

/**
 * @brief Encode a total frame length into the Control Byte 0 size field
 *
 * @param ctrl_byte_0 Current control byte 0 (other bits are preserved)
 * @param total_length Total frame length in bytes
 * @return Updated control byte 0
 */
inline uint8_t set_frame_length(uint8_t ctrl_byte_0, uint8_t total_length) {
  const uint8_t size_field =
    static_cast<uint8_t>((total_length - FRAME_SIZE_FIELD_BIAS) & CTRL0_LENGTH_MASK);
  return static_cast<uint8_t>((ctrl_byte_0 & ~CTRL0_LENGTH_MASK) | size_field);
}

/**
 * @brief Size of the authentication trailer for a given mode
 *
 * @param is_1w true for 1W (sequence number + MAC), false for 2W (MAC only)
 */
inline uint8_t auth_trailer_size(bool is_1w) {
  return is_1w ? AUTH_TRAILER_SIZE_1W : AUTH_TRAILER_SIZE_2W;
}

/**
 * @brief Check whether an address is a broadcast or group address
 *
 * @param node_id Node ID (3 bytes)
 * @return true for 00:00:3F, FF:FF:FF and 00:00:00
 */
bool is_broadcast(const uint8_t node_id[NODE_ID_SIZE]);

/**
 * @brief Print frame in human-readable format (for debugging)
 *
 * @param frame Pointer to IoFrame structure
 * @param print_func Function pointer for printing (e.g., Serial.println)
 */
void print_frame(const IoFrame* frame, void (*print_func)(const char*));

} // namespace frame
} // namespace iohome
