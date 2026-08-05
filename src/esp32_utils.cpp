/**
  * @file    esp32_utils.cpp
  * @author  iown-homecontrol
  * @brief   ESP32 helper functions
  */

// No Arduino dependency: this delegates to the protocol layer and nothing else,
// which is what lets test_legacy_helpers build it on the host.
#include "iown_frame.h"  // declares iown_crc_calc (and gives it C linkage)
#include "protocol/iohome_crypto.h"

#pragma region ESP32_CRC

/**
 * @brief CRC-16/CCITT (KERMIT) over an io-homecontrol packet.
 *
 * The body of this function was entirely commented out and it ended in
 * `return -1;` from a `uint16_t` return type, so every caller got 0xFFFF with
 * no way to tell that apart from a real checksum. Both parameters went unused.
 *
 * It now delegates to the implementation the protocol stack uses, which the
 * unit tests check against the captured frames in docs/linklayer.md.
 *
 * The ESP-IDF `esp_crc16_le()` the commented-out code reached for is not a
 * drop-in replacement: it seeds with ~init and reflects the result, so it does
 * not reproduce the captured checksums.
 *
 * @param iown_packet     frame bytes, excluding the CRC itself
 * @param iown_packet_len number of bytes at @p iown_packet
 * @return the CRC, or 0 for a null or empty input
 */
uint16_t iown_crc_calc(const uint8_t *iown_packet, uint8_t iown_packet_len) {
  if (iown_packet == nullptr || iown_packet_len == 0) {
    return 0;
  }
  return iohome::crypto::compute_crc16(iown_packet, iown_packet_len);
}

#pragma endregion ESP32_CRC
