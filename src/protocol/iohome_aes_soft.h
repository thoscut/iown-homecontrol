/**
 * @file iohome_aes_soft.h
 * @brief Minimal software AES-128 ECB (single block)
 * @author iown-homecontrol project
 *
 * Used when no hardware/mbedTLS AES is available - notably in the native unit
 * test build, so the protocol's MAC and key-masking logic can be verified
 * against published AES test vectors instead of being stubbed out.
 *
 * This is a straightforward, table-based FIPS-197 implementation. It is *not*
 * hardened against cache-timing side channels; on ESP32 the mbedTLS/hardware
 * path is used instead.
 */

#pragma once

#include <stdint.h>

namespace iohome {
namespace crypto {
namespace soft_aes {

/// AES-128 block and key size in bytes.
constexpr int BLOCK_SIZE = 16;
constexpr int KEY_SIZE = 16;

/**
 * @brief Encrypt one 16-byte block with AES-128 (ECB).
 *
 * `input` and `output` may alias.
 */
void encrypt_block(const uint8_t input[BLOCK_SIZE],
                   const uint8_t key[KEY_SIZE],
                   uint8_t output[BLOCK_SIZE]);

/**
 * @brief Decrypt one 16-byte block with AES-128 (ECB).
 *
 * `input` and `output` may alias.
 */
void decrypt_block(const uint8_t input[BLOCK_SIZE],
                   const uint8_t key[KEY_SIZE],
                   uint8_t output[BLOCK_SIZE]);

} // namespace soft_aes
} // namespace crypto
} // namespace iohome
