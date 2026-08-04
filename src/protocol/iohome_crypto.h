/**
 * @file iohome_crypto.h
 * @brief io-homecontrol Cryptographic Functions
 * @author iown-homecontrol project
 *
 * Cryptographic functions for io-homecontrol protocol including:
 * - CRC-16/KERMIT calculation
 * - AES-128 encryption/decryption
 * - IV (Initial Value) construction
 * - MAC generation for 1W and 2W modes
 * - Cryptographically secure random numbers
 *
 * On ESP32 the AES primitives are backed by mbedTLS. On other targets, and in
 * the native unit test build, a self-contained software AES-128 is used so the
 * protocol logic can be verified without a hardware crypto engine.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "iohome_constants.h"

namespace iohome {
namespace crypto {

// ============================================================================
// CRC-16/KERMIT Functions
// ============================================================================

/**
 * @brief Compute CRC-16/KERMIT for a single byte
 *
 * @param data The byte to process
 * @param crc Current CRC value (default: 0)
 * @return Updated CRC value
 */
uint16_t compute_crc16_byte(uint8_t data, uint16_t crc = CRC_INITIAL);

/**
 * @brief Compute CRC-16/KERMIT for a buffer
 *
 * @param data Pointer to data buffer (may be nullptr when length is 0)
 * @param length Number of bytes to process
 * @param crc Initial CRC value (default: 0)
 * @return Final CRC value
 */
uint16_t compute_crc16(const uint8_t* data, size_t length, uint16_t crc = CRC_INITIAL);

/**
 * @brief Verify CRC-16 of a frame (last 2 bytes are the CRC, LSB first)
 *
 * @param frame Pointer to complete frame including CRC
 * @param length Total frame length (including CRC)
 * @return true if CRC is valid, false otherwise
 */
bool verify_crc16(const uint8_t* frame, size_t length);

// ============================================================================
// Side-channel helpers
// ============================================================================

/**
 * @brief Compare two buffers in constant time
 *
 * Runtime depends only on `length`, never on the contents, so an attacker
 * cannot recover a secret byte-by-byte from timing differences.
 *
 * @return true if the buffers are equal
 */
bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t length);

/**
 * @brief Overwrite a buffer with zeroes in a way the compiler cannot elide
 *
 * Use this on stack copies of keys, IVs and MACs before they go out of scope.
 */
void secure_zero(void* buffer, size_t length);

// ============================================================================
// Random Number Generation
// ============================================================================

/**
 * @brief Fill a buffer with cryptographically secure random bytes
 *
 * Backed by the ESP32 hardware RNG (`esp_random`) on ESP32 targets and by
 * `std::random_device` on hosted builds.
 *
 * @param out Output buffer
 * @param length Number of bytes to generate
 * @return true on success; false if no secure source is available, in which
 *         case `out` is left zeroed and the caller must not proceed.
 */
bool random_bytes(uint8_t* out, size_t length);

/**
 * @brief Whether random_bytes() is backed by a cryptographically secure source
 *
 * Always check this before relying on challenges or generated keys.
 */
bool has_secure_random();

// ============================================================================
// Checksum Functions (for IV construction)
// ============================================================================

/**
 * @brief Compute custom checksum for IV construction
 *
 * This is a proprietary checksum algorithm used in the IV generation.
 *
 * @param frame_byte Input byte from frame data
 * @param chksum1 First checksum accumulator (in/out)
 * @param chksum2 Second checksum accumulator (in/out)
 */
void compute_checksum(uint8_t frame_byte, uint8_t& chksum1, uint8_t& chksum2);

// ============================================================================
// Initial Value (IV) Construction
// ============================================================================

/**
 * @brief Construct Initial Value for 1-Way mode MAC
 *
 * The IV is constructed from:
 * - Bytes 0-7: Frame data (or 0x55 padding)
 * - Bytes 8-9: Custom checksum
 * - Bytes 10-11: Sequence number (rolling code)
 * - Bytes 12-15: Padding (0x55)
 *
 * @param frame_data Pointer to frame data (command ID + parameters)
 * @param data_len Length of frame data
 * @param sequence_number Rolling code (2 bytes)
 * @param iv_out Output buffer for IV (16 bytes)
 */
void construct_iv_1w(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t sequence_number[ROLLING_CODE_SIZE],
  uint8_t iv_out[IV_SIZE]
);

/**
 * @brief Construct Initial Value for 2-Way mode MAC
 *
 * The IV is constructed from:
 * - Bytes 0-7: Frame data (or 0x55 padding)
 * - Bytes 8-9: Custom checksum
 * - Bytes 10-15: Challenge (6 bytes)
 *
 * @param frame_data Pointer to frame data (command ID + parameters)
 * @param data_len Length of frame data
 * @param challenge Challenge bytes (6 bytes)
 * @param iv_out Output buffer for IV (16 bytes)
 */
void construct_iv_2w(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t challenge[HMAC_SIZE],
  uint8_t iv_out[IV_SIZE]
);

// ============================================================================
// AES-128 Encryption/Decryption
// ============================================================================

/**
 * @brief Encrypt a 16-byte block using AES-128 ECB
 *
 * @param input Input data (16 bytes)
 * @param key AES key (16 bytes)
 * @param output Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool aes128_encrypt(
  const uint8_t input[AES_BLOCK_SIZE],
  const uint8_t key[AES_KEY_SIZE],
  uint8_t output[AES_BLOCK_SIZE]
);

/**
 * @brief Decrypt a 16-byte block using AES-128 ECB
 *
 * @param input Input data (16 bytes)
 * @param key AES key (16 bytes)
 * @param output Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool aes128_decrypt(
  const uint8_t input[AES_BLOCK_SIZE],
  const uint8_t key[AES_KEY_SIZE],
  uint8_t output[AES_BLOCK_SIZE]
);

// ============================================================================
// Key Encryption (for pairing)
// ============================================================================

/**
 * @brief Encrypt system key for 1-Way mode transfer
 *
 * The key is masked with AES-128(TRANSFER_KEY, IV) where the IV repeats the
 * node address. Because TRANSFER_KEY is a public protocol constant this
 * provides obfuscation only - anyone who records the pairing frame recovers
 * the key. See docs/SECURITY-MODEL.md.
 *
 * @param system_key System key to encrypt (16 bytes)
 * @param node_address Node address (3 bytes)
 * @param encrypted_out Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool encrypt_1w_key(
  const uint8_t system_key[AES_KEY_SIZE],
  const uint8_t node_address[NODE_ID_SIZE],
  uint8_t encrypted_out[AES_KEY_SIZE]
);

/**
 * @brief Recover a system key from a 1-Way key transfer frame
 *
 * Inverse of encrypt_1w_key(); the masking operation is its own inverse.
 *
 * @param encrypted Encrypted key from the frame (16 bytes)
 * @param node_address Node address the key was addressed to (3 bytes)
 * @param system_key_out Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool decrypt_1w_key(
  const uint8_t encrypted[AES_KEY_SIZE],
  const uint8_t node_address[NODE_ID_SIZE],
  uint8_t system_key_out[AES_KEY_SIZE]
);

/**
 * @brief Encrypt system key for 2-Way mode transfer
 *
 * @param system_key System key to encrypt (16 bytes)
 * @param challenge Challenge bytes (6 bytes)
 * @param encrypted_out Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool encrypt_2w_key(
  const uint8_t system_key[AES_KEY_SIZE],
  const uint8_t challenge[HMAC_SIZE],
  uint8_t encrypted_out[AES_KEY_SIZE]
);

/**
 * @brief Recover a system key from a 2-Way key transfer frame
 *
 * @param encrypted Encrypted key from the frame (16 bytes)
 * @param challenge Challenge used during the transfer (6 bytes)
 * @param system_key_out Output buffer (16 bytes)
 * @return true on success, false on error
 */
bool decrypt_2w_key(
  const uint8_t encrypted[AES_KEY_SIZE],
  const uint8_t challenge[HMAC_SIZE],
  uint8_t system_key_out[AES_KEY_SIZE]
);

/**
 * @brief Generate a fresh random system key
 *
 * @param key_out Output buffer (16 bytes)
 * @return true on success; false when no secure random source is available
 */
bool generate_system_key(uint8_t key_out[AES_KEY_SIZE]);

// ============================================================================
// MAC Generation
// ============================================================================

/**
 * @brief Generate MAC for 1-Way mode
 *
 * The MAC is a 6-byte truncated AES output used for authentication.
 *
 * @param frame_data Complete frame data (command ID + parameters)
 * @param data_len Length of frame data
 * @param sequence_number Rolling code (2 bytes)
 * @param system_key System key (16 bytes)
 * @param hmac_out Output buffer (6 bytes)
 * @return true on success, false on error
 */
bool create_1w_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t sequence_number[ROLLING_CODE_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  uint8_t hmac_out[HMAC_SIZE]
);

/**
 * @brief Generate MAC for 2-Way mode
 *
 * @param frame_data Complete frame data (command ID + parameters)
 * @param data_len Length of frame data
 * @param challenge Challenge bytes (6 bytes)
 * @param system_key System key (16 bytes)
 * @param hmac_out Output buffer (6 bytes)
 * @return true on success, false on error
 */
bool create_2w_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t challenge[HMAC_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  uint8_t hmac_out[HMAC_SIZE]
);

/**
 * @brief Verify the MAC of a received frame (constant-time comparison)
 *
 * @param frame_data Complete frame data
 * @param data_len Length of frame data
 * @param received_hmac MAC from frame (6 bytes)
 * @param sequence_or_challenge Sequence number (1W) or challenge (2W)
 * @param system_key System key (16 bytes)
 * @param is_2w true for 2W mode, false for 1W mode
 * @return true if the MAC is valid, false otherwise
 */
bool verify_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t received_hmac[HMAC_SIZE],
  const uint8_t* sequence_or_challenge,
  const uint8_t system_key[AES_KEY_SIZE],
  bool is_2w
);

} // namespace crypto
} // namespace iohome
