/**
 * @file iohc_protocol.h
 * @brief Frame checksum and MAC input construction, free of any dependency
 *
 * The ESPHome component carries its own copy of the CRC and the MAC's initial
 * value because an `external_components` directory has to be self-contained -
 * ESPHome copies the component folder and nothing else, so it cannot reach
 * `src/protocol/`.
 *
 * Duplicated crypto is exactly the thing that drifts without anyone noticing,
 * and this project has already been bitten by it once: the 2W key transfer used
 * an initial value neither peer computed, and the round-trip test agreed with
 * itself for months. So the duplicate lives here, in a header that includes
 * nothing but <stdint.h> and <string.h>, and `test/test_esphome_crypto` builds
 * it on the host and asserts byte-for-byte agreement with `src/protocol/` and
 * with the captures in `docs/`.
 *
 * Keep this header dependency-free. The moment it needs ESPHome or mbedTLS,
 * the test cannot build it and the guarantee is gone.
 */

#pragma once

#include <stdint.h>
#include <string.h>

namespace esphome {
namespace iown_homecontrol {

/// Size of the AES initial value, in bytes.
static const size_t IOHC_IV_SIZE = 16;

/// Padding byte used wherever the initial value has no frame data to carry.
static const uint8_t IOHC_IV_PADDING = 0x55;

/**
 * @brief CRC-16/KERMIT over @p len bytes.
 *
 * Polynomial 0x8408, initial value 0, no final XOR. The two CRC bytes are sent
 * least significant first, so a frame including its own CRC checksums to zero.
 */
inline uint16_t iohc_compute_crc(const uint8_t *data, size_t len) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0x8408;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

/**
 * @brief The proprietary running checksum that occupies IV bytes 8 and 9.
 *
 * docs/linklayer.md, "Initial Vector (IV) and AES-128 Encryption". Both bytes
 * start at zero and are folded over every byte of the frame data.
 */
inline void iohc_checksum_step(uint8_t frame_byte, uint8_t *chksum1, uint8_t *chksum2) {
  const uint8_t tmpchksum = frame_byte ^ *chksum2;
  uint8_t next = ((*chksum1 & 0x7F) << 1) & 0xFF;

  if ((*chksum1 & 0x80) == 0) {
    if (tmpchksum >= 128) {
      next |= 1;
    }
    *chksum1 = next;
    *chksum2 = (tmpchksum << 1) & 0xFF;
  } else {
    if (tmpchksum >= 128) {
      next |= 1;
    }
    *chksum1 = next ^ 0x55;
    *chksum2 = ((tmpchksum << 1) ^ 0x5B) & 0xFF;
  }
}

/**
 * @brief Build the AES initial value for a 1W MAC.
 *
 * Layout, per docs/linklayer.md:
 *   bytes 0-7   first eight bytes of the frame data, 0x55-padded if shorter
 *   bytes 8-9   the running checksum over *all* of the frame data
 *   bytes 10-11 the sequence number (rolling code)
 *   bytes 12-15 0x55 padding
 *
 * @param frame_data   command ID followed by its parameters
 * @param data_len     length of @p frame_data
 * @param rolling_code sequence number, two bytes, as sent on the wire
 * @param iv_out       receives IOHC_IV_SIZE bytes
 */
inline void iohc_build_iv_1w(const uint8_t *frame_data, size_t data_len,
                             const uint8_t rolling_code[2], uint8_t iv_out[IOHC_IV_SIZE]) {
  memset(iv_out, IOHC_IV_PADDING, IOHC_IV_SIZE);

  uint8_t chksum1 = 0;
  uint8_t chksum2 = 0;
  for (size_t i = 0; i < data_len; i++) {
    iohc_checksum_step(frame_data[i], &chksum1, &chksum2);
    // Only the first eight bytes go into the IV; the checksum covers all of it.
    if (i < 8) {
      iv_out[i] = frame_data[i];
    }
  }

  iv_out[8] = chksum1;
  iv_out[9] = chksum2;
  iv_out[10] = rolling_code[0];
  iv_out[11] = rolling_code[1];
  // Bytes 12-15 keep the 0x55 padding from the memset above.
}

}  // namespace iown_homecontrol
}  // namespace esphome
