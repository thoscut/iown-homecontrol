/**
 * @file iohome_crypto.cpp
 * @brief io-homecontrol Cryptographic Functions Implementation
 * @author iown-homecontrol project
 */

#include "iohome_crypto.h"
#include <string.h>

// ---------------------------------------------------------------------------
// AES backend selection
// ---------------------------------------------------------------------------
// ESP32 builds use mbedTLS (hardware accelerated). Everything else - including
// the native unit test build - uses the bundled software AES so the protocol
// logic is exercised for real instead of being stubbed out.
//
// ESP_PLATFORM covers plain ESP-IDF builds, where mbedTLS is present but
// ARDUINO is not defined.
#if defined(IOHOME_FORCE_SOFTWARE_AES) || defined(UNIT_TEST)
  #define IOHOME_USE_SOFTWARE_AES 1
#elif defined(ARDUINO) || defined(ESP_PLATFORM)
  #define IOHOME_USE_SOFTWARE_AES 0
#else
  #define IOHOME_USE_SOFTWARE_AES 1
#endif

#if IOHOME_USE_SOFTWARE_AES
  #include "iohome_aes_soft.h"
#else
  #include <mbedtls/aes.h>
#endif

// ---------------------------------------------------------------------------
// Random backend selection
// ---------------------------------------------------------------------------
#if defined(ESP32) || defined(ESP_PLATFORM) || defined(ARDUINO_ARCH_ESP32)
  // esp_random() moved from esp_system.h to esp_random.h in ESP-IDF 5, and
  // both spellings are in the field. Include whichever exists.
  #if defined(__has_include)
    #if __has_include(<esp_random.h>)
      #include <esp_random.h>
    #endif
  #endif
  #include <esp_system.h>
  #define IOHOME_HAS_ESP_RANDOM 1
#else
  #define IOHOME_HAS_ESP_RANDOM 0
#endif

#if !IOHOME_HAS_ESP_RANDOM && !defined(ARDUINO)
  #include <random>
  #define IOHOME_HAS_STD_RANDOM 1
#else
  #define IOHOME_HAS_STD_RANDOM 0
#endif

namespace iohome {
namespace crypto {

// ============================================================================
// CRC-16/KERMIT Implementation
// ============================================================================

uint16_t compute_crc16_byte(uint8_t data, uint16_t crc) {
  crc ^= data;
  for (int i = 0; i < 8; i++) {
    const uint16_t remainder = (crc & 1) ? CRC_POLYNOMIAL : 0;
    crc = (crc >> 1) ^ remainder;
  }
  return crc;
}

uint16_t compute_crc16(const uint8_t* data, size_t length, uint16_t crc) {
  if (data == nullptr) {
    return crc;
  }
  for (size_t i = 0; i < length; i++) {
    crc = compute_crc16_byte(data[i], crc);
  }
  return crc;
}

bool verify_crc16(const uint8_t* frame, size_t length) {
  // A frame must carry at least one covered byte plus the two CRC bytes;
  // `length == CRC_SIZE` would otherwise "verify" an empty message.
  if (frame == nullptr || length <= CRC_SIZE) {
    return false;
  }

  const uint16_t calculated_crc = compute_crc16(frame, length - CRC_SIZE);
  const uint16_t received_crc =
    static_cast<uint16_t>(frame[length - 2] | (static_cast<uint16_t>(frame[length - 1]) << 8));

  return calculated_crc == received_crc;
}

// ============================================================================
// Side-channel helpers
// ============================================================================

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t length) {
  if (a == nullptr || b == nullptr) {
    return false;
  }

  uint8_t diff = 0;
  for (size_t i = 0; i < length; i++) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

void secure_zero(void* buffer, size_t length) {
  if (buffer == nullptr) {
    return;
  }
  // The volatile pointer keeps the compiler from optimising the writes away
  // as a dead store on a soon-to-be-destroyed object.
  volatile uint8_t* p = static_cast<volatile uint8_t*>(buffer);
  while (length-- > 0) {
    *p++ = 0;
  }
}

// ============================================================================
// Random Number Generation
// ============================================================================

bool has_secure_random() {
#if IOHOME_HAS_ESP_RANDOM || IOHOME_HAS_STD_RANDOM
  return true;
#else
  return false;
#endif
}

bool random_bytes(uint8_t* out, size_t length) {
  if (out == nullptr) {
    return false;
  }

  if (length == 0) {
    return true;
  }

#if IOHOME_HAS_ESP_RANDOM
  // esp_random() draws from the hardware RNG. It is only a true RNG while the
  // RF subsystem is active; ESP-IDF still seeds it from hardware noise
  // otherwise, which is strictly better than the Arduino PRNG.
  size_t offset = 0;
  while (offset < length) {
    const uint32_t word = esp_random();
    const size_t chunk = (length - offset) < sizeof(word) ? (length - offset) : sizeof(word);
    memcpy(out + offset, &word, chunk);
    offset += chunk;
  }
  return true;
#elif IOHOME_HAS_STD_RANDOM
  static std::random_device device;
  std::uniform_int_distribution<unsigned int> dist(0, 255);
  for (size_t i = 0; i < length; i++) {
    out[i] = static_cast<uint8_t>(dist(device));
  }
  return true;
#else
  // No cryptographically secure source on this platform. Refuse rather than
  // silently handing back predictable "randomness".
  memset(out, 0, length);
  return false;
#endif
}

// ============================================================================
// Checksum Functions (for IV construction)
// ============================================================================

void compute_checksum(uint8_t frame_byte, uint8_t& chksum1, uint8_t& chksum2) {
  const uint8_t tmpchksum = frame_byte ^ chksum2;
  chksum2 = ((chksum1 & 0x7F) << 1) & 0xFF;

  if ((chksum1 & 0x80) == 0) {
    if (tmpchksum >= 128) {
      chksum2 |= 1;
    }
    chksum1 = chksum2;
    chksum2 = (tmpchksum << 1) & 0xFF;
    return;
  }

  if (tmpchksum >= 128) {
    chksum2 |= 1;
  }

  chksum1 = chksum2 ^ 0x55;
  chksum2 = ((tmpchksum << 1) ^ 0x5B) & 0xFF;
}

// ============================================================================
// Initial Value (IV) Construction
// ============================================================================

namespace {

/// Shared prefix of the 1W and 2W IVs: data bytes, padding and checksums.
void construct_iv_prefix(const uint8_t* frame_data, size_t data_len, uint8_t iv_out[IV_SIZE]) {
  memset(iv_out, 0, IV_SIZE);

  uint8_t chksum1 = 0;
  uint8_t chksum2 = 0;

  if (frame_data != nullptr) {
    for (size_t i = 0; i < data_len; i++) {
      compute_checksum(frame_data[i], chksum1, chksum2);
      if (i < 8) {
        iv_out[i] = frame_data[i];
      }
    }
  } else {
    data_len = 0;
  }

  // Pad bytes 0-7 with 0x55 when the data is shorter than 8 bytes.
  for (size_t i = data_len; i < 8; i++) {
    iv_out[i] = IV_PADDING;
  }

  iv_out[8] = chksum1;
  iv_out[9] = chksum2;
}

} // namespace

void construct_iv_1w(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t sequence_number[ROLLING_CODE_SIZE],
  uint8_t iv_out[IV_SIZE]
) {
  if (iv_out == nullptr) {
    return;
  }

  construct_iv_prefix(frame_data, data_len, iv_out);

  // Bytes 10-11: sequence number
  iv_out[10] = (sequence_number != nullptr) ? sequence_number[0] : IV_PADDING;
  iv_out[11] = (sequence_number != nullptr) ? sequence_number[1] : IV_PADDING;

  // Bytes 12-15: padding
  for (int i = 12; i < IV_SIZE; i++) {
    iv_out[i] = IV_PADDING;
  }
}

void construct_iv_2w(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t challenge[HMAC_SIZE],
  uint8_t iv_out[IV_SIZE]
) {
  if (iv_out == nullptr) {
    return;
  }

  construct_iv_prefix(frame_data, data_len, iv_out);

  // Bytes 10-15: challenge
  if (challenge != nullptr) {
    memcpy(&iv_out[10], challenge, HMAC_SIZE);
  } else {
    memset(&iv_out[10], IV_PADDING, HMAC_SIZE);
  }
}

// ============================================================================
// AES-128 Encryption/Decryption
// ============================================================================

bool aes128_encrypt(
  const uint8_t input[AES_BLOCK_SIZE],
  const uint8_t key[AES_KEY_SIZE],
  uint8_t output[AES_BLOCK_SIZE]
) {
  if (input == nullptr || key == nullptr || output == nullptr) {
    return false;
  }

#if IOHOME_USE_SOFTWARE_AES
  soft_aes::encrypt_block(input, key, output);
  return true;
#else
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
  if (ret != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, input, output);

  mbedtls_aes_free(&aes);
  return (ret == 0);
#endif
}

bool aes128_decrypt(
  const uint8_t input[AES_BLOCK_SIZE],
  const uint8_t key[AES_KEY_SIZE],
  uint8_t output[AES_BLOCK_SIZE]
) {
  if (input == nullptr || key == nullptr || output == nullptr) {
    return false;
  }

#if IOHOME_USE_SOFTWARE_AES
  soft_aes::decrypt_block(input, key, output);
  return true;
#else
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
  if (ret != 0) {
    mbedtls_aes_free(&aes);
    return false;
  }

  ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input, output);

  mbedtls_aes_free(&aes);
  return (ret == 0);
#endif
}

// ============================================================================
// Key Encryption (for pairing)
// ============================================================================

namespace {

/// Build the AES mask both 1W key transfer directions share.
bool key_mask_1w(const uint8_t node_address[NODE_ID_SIZE], uint8_t mask_out[AES_BLOCK_SIZE]) {
  // IV repeats the node address: AB CD EF AB CD EF ... AB
  uint8_t iv[IV_SIZE];
  for (int i = 0; i < IV_SIZE; i++) {
    iv[i] = node_address[i % NODE_ID_SIZE];
  }

  const bool ok = aes128_encrypt(iv, TRANSFER_KEY, mask_out);
  secure_zero(iv, sizeof(iv));
  return ok;
}

/// Build the AES mask both 2W key transfer directions share.
///
/// The IV is the ordinary 2W one, built from the frame that asked for the key
/// transfer - command 0x38 (launch key transfer) or 0x31 (ask challenge) - and
/// the challenge it carried. docs/linklayer.md: "The initial value is always
/// created using data from the requesting command".
///
/// This used to fill bytes 0-9 with 0x55 padding and use only the challenge,
/// dropping the requesting frame's payload and its checksum. The resulting key
/// mask was not the one the peer computes, so a device paired this way ended up
/// with a system key neither side could use - and nothing caught it, because
/// both directions of our own code made the same mistake and so agreed with
/// each other.
bool key_mask_2w(const uint8_t* frame_data,
                 size_t data_len,
                 const uint8_t challenge[HMAC_SIZE],
                 uint8_t mask_out[AES_BLOCK_SIZE]) {
  uint8_t iv[IV_SIZE];
  construct_iv_2w(frame_data, data_len, challenge, iv);

  const bool ok = aes128_encrypt(iv, TRANSFER_KEY, mask_out);
  secure_zero(iv, sizeof(iv));
  return ok;
}

bool xor_with_mask(const uint8_t* in, const uint8_t mask[AES_BLOCK_SIZE], uint8_t* out) {
  for (int i = 0; i < AES_KEY_SIZE; i++) {
    out[i] = in[i] ^ mask[i];
  }
  return true;
}

} // namespace

bool encrypt_1w_key(
  const uint8_t system_key[AES_KEY_SIZE],
  const uint8_t node_address[NODE_ID_SIZE],
  uint8_t encrypted_out[AES_KEY_SIZE]
) {
  if (system_key == nullptr || node_address == nullptr || encrypted_out == nullptr) {
    return false;
  }

  uint8_t mask[AES_BLOCK_SIZE];
  if (!key_mask_1w(node_address, mask)) {
    return false;
  }

  xor_with_mask(system_key, mask, encrypted_out);
  secure_zero(mask, sizeof(mask));
  return true;
}

bool decrypt_1w_key(
  const uint8_t encrypted[AES_KEY_SIZE],
  const uint8_t node_address[NODE_ID_SIZE],
  uint8_t system_key_out[AES_KEY_SIZE]
) {
  // XOR masking is an involution, so decryption is the same operation.
  return encrypt_1w_key(encrypted, node_address, system_key_out);
}

bool encrypt_2w_key(
  const uint8_t system_key[AES_KEY_SIZE],
  const uint8_t* request_frame_data,
  size_t request_data_len,
  const uint8_t challenge[HMAC_SIZE],
  uint8_t encrypted_out[AES_KEY_SIZE]
) {
  if (system_key == nullptr || challenge == nullptr || encrypted_out == nullptr) {
    return false;
  }
  if (request_frame_data == nullptr || request_data_len == 0) {
    return false;
  }

  uint8_t mask[AES_BLOCK_SIZE];
  if (!key_mask_2w(request_frame_data, request_data_len, challenge, mask)) {
    return false;
  }

  xor_with_mask(system_key, mask, encrypted_out);
  secure_zero(mask, sizeof(mask));
  return true;
}

bool decrypt_2w_key(
  const uint8_t encrypted[AES_KEY_SIZE],
  const uint8_t* request_frame_data,
  size_t request_data_len,
  const uint8_t challenge[HMAC_SIZE],
  uint8_t system_key_out[AES_KEY_SIZE]
) {
  // XOR masking is an involution, so decryption is the same operation - but
  // only when both sides derive the mask from the same requesting frame.
  return encrypt_2w_key(encrypted, request_frame_data, request_data_len,
                        challenge, system_key_out);
}

bool generate_system_key(uint8_t key_out[AES_KEY_SIZE]) {
  if (key_out == nullptr) {
    return false;
  }
  return random_bytes(key_out, AES_KEY_SIZE);
}

// ============================================================================
// MAC Generation
// ============================================================================

bool create_1w_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t sequence_number[ROLLING_CODE_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  uint8_t hmac_out[HMAC_SIZE]
) {
  if (sequence_number == nullptr || system_key == nullptr || hmac_out == nullptr) {
    return false;
  }

  uint8_t iv[IV_SIZE];
  construct_iv_1w(frame_data, data_len, sequence_number, iv);

  uint8_t encrypted_iv[AES_BLOCK_SIZE];
  const bool ok = aes128_encrypt(iv, system_key, encrypted_iv);
  secure_zero(iv, sizeof(iv));

  if (!ok) {
    return false;
  }

  // Truncate the AES output to 6 bytes.
  memcpy(hmac_out, encrypted_iv, HMAC_SIZE);
  secure_zero(encrypted_iv, sizeof(encrypted_iv));
  return true;
}

bool create_2w_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t challenge[HMAC_SIZE],
  const uint8_t system_key[AES_KEY_SIZE],
  uint8_t hmac_out[HMAC_SIZE]
) {
  if (challenge == nullptr || system_key == nullptr || hmac_out == nullptr) {
    return false;
  }

  uint8_t iv[IV_SIZE];
  construct_iv_2w(frame_data, data_len, challenge, iv);

  uint8_t encrypted_iv[AES_BLOCK_SIZE];
  const bool ok = aes128_encrypt(iv, system_key, encrypted_iv);
  secure_zero(iv, sizeof(iv));

  if (!ok) {
    return false;
  }

  memcpy(hmac_out, encrypted_iv, HMAC_SIZE);
  secure_zero(encrypted_iv, sizeof(encrypted_iv));
  return true;
}

bool verify_hmac(
  const uint8_t* frame_data,
  size_t data_len,
  const uint8_t received_hmac[HMAC_SIZE],
  const uint8_t* sequence_or_challenge,
  const uint8_t system_key[AES_KEY_SIZE],
  bool is_2w
) {
  if (received_hmac == nullptr || sequence_or_challenge == nullptr || system_key == nullptr) {
    return false;
  }

  uint8_t calculated_hmac[HMAC_SIZE];
  const bool success =
    is_2w ? create_2w_hmac(frame_data, data_len, sequence_or_challenge, system_key, calculated_hmac)
          : create_1w_hmac(frame_data, data_len, sequence_or_challenge, system_key, calculated_hmac);

  if (!success) {
    return false;
  }

  const bool equal = constant_time_equal(calculated_hmac, received_hmac, HMAC_SIZE);
  secure_zero(calculated_hmac, sizeof(calculated_hmac));
  return equal;
}

} // namespace crypto
} // namespace iohome
