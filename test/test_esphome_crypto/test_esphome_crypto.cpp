/**
 * The ESPHome component carries its own CRC and MAC-input construction, because
 * an `external_components` directory has to be self-contained. This suite is
 * what keeps that copy honest: it builds the component's dependency-free header
 * on the host and checks it byte for byte against `src/protocol/` and against
 * the captures in `docs/`.
 *
 * Without this, the duplicate is only as correct as the last person to change
 * one side and remember the other. The 2W key transfer showed what that costs:
 * two implementations wrong in the same way agree with each other perfectly.
 */

#include <unity.h>
#include <string.h>

#include "iohc_protocol.h"
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_constants.h"

using esphome::iown_homecontrol::iohc_build_iv_1w;
using esphome::iown_homecontrol::iohc_compute_crc;

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

void test_crc_matches_the_library(void) {
    // A spread of lengths and byte values, including the empty input.
    uint8_t buffer[64];
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = static_cast<uint8_t>((i * 37) ^ 0xA5);
    }

    for (size_t len = 0; len <= sizeof(buffer); len++) {
        TEST_ASSERT_EQUAL_HEX16(iohome::crypto::compute_crc16(buffer, len),
                                iohc_compute_crc(buffer, len));
    }
}

void test_crc_matches_the_captured_frames(void) {
    // scripts/io-homecontrol.ksy, SFD stripped: CRC 7E 72 on the wire.
    static const uint8_t ksy[] = {
        0xf8, 0x00, 0x00, 0x00, 0x7f, 0x70, 0x87, 0x58, 0x00,
        0x01, 0x61, 0xd4, 0x00, 0x80, 0xc8, 0x00, 0x00,
        0x3b, 0xd5,
        0x05, 0x52, 0x68, 0x75, 0x49, 0x9c,
        0x7e, 0x72};
    TEST_ASSERT_EQUAL_HEX16(0x727E, iohc_compute_crc(ksy, sizeof(ksy) - 2));

    // docs/linklayer.md 1W execute: CRC A9 37.
    static const uint8_t doc[] = {
        0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
        0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
        0x3B, 0xD2,
        0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8,
        0xA9, 0x37};
    TEST_ASSERT_EQUAL_HEX16(0x37A9, iohc_compute_crc(doc, sizeof(doc) - 2));

    // A frame including its own CRC checksums to zero.
    TEST_ASSERT_EQUAL_HEX16(0x0000, iohc_compute_crc(ksy, sizeof(ksy)));
    TEST_ASSERT_EQUAL_HEX16(0x0000, iohc_compute_crc(doc, sizeof(doc)));
}

// ---------------------------------------------------------------------------
// MAC initial value
// ---------------------------------------------------------------------------

void test_iv_matches_the_library(void) {
    const uint8_t sequence[2] = {0x12, 0x34};

    uint8_t frame_data[32];
    for (size_t i = 0; i < sizeof(frame_data); i++) {
        frame_data[i] = static_cast<uint8_t>((i * 91) ^ 0x3C);
    }

    // Lengths on both sides of the eight-byte prefix, so the 0x55 padding rule
    // and the "checksum covers everything" rule are both exercised.
    for (size_t len = 1; len <= sizeof(frame_data); len++) {
        uint8_t esphome_iv[16];
        uint8_t library_iv[16];
        iohc_build_iv_1w(frame_data, len, sequence, esphome_iv);
        iohome::crypto::construct_iv_1w(frame_data, len, sequence, library_iv);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(library_iv, esphome_iv, 16);
    }
}

void test_iv_layout(void) {
    // Four bytes of frame data: the rest of the first eight must be padding.
    const uint8_t frame_data[4] = {0x00, 0x01, 0x61, 0xD2};
    const uint8_t sequence[2] = {0xAB, 0xCD};

    uint8_t iv[16];
    iohc_build_iv_1w(frame_data, sizeof(frame_data), sequence, iv);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame_data, iv, sizeof(frame_data));
    for (size_t i = sizeof(frame_data); i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x55, iv[i]);
    }

    // Sequence number at 10-11, padding from 12 on.
    TEST_ASSERT_EQUAL_HEX8(0xAB, iv[10]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, iv[11]);
    for (size_t i = 12; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x55, iv[i]);
    }
}

void test_mac_matches_the_documented_vector(void) {
    // scripts/Iown-ioCrypto.py, 1W key push: node ABCDEF, controller key
    // 01020304050607080910111213141516, sequence 0x1234 -> MAC 19E81EC43D5E.
    const uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                             0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    const uint8_t node[3] = {0xAB, 0xCD, 0xEF};
    const uint8_t sequence[2] = {0x12, 0x34};

    uint8_t encrypted_key[16];
    TEST_ASSERT_TRUE(iohome::crypto::encrypt_1w_key(key, node, encrypted_key));

    uint8_t frame_data[17];
    frame_data[0] = iohome::CMD_SEND_1W_KEY;
    memcpy(&frame_data[1], encrypted_key, 16);

    // Take the component's IV through the same AES the library uses, so this
    // checks the component's construction rather than the library's.
    uint8_t iv[16];
    iohc_build_iv_1w(frame_data, sizeof(frame_data), sequence, iv);

    uint8_t block[16];
    TEST_ASSERT_TRUE(iohome::crypto::aes128_encrypt(iv, key, block));

    const uint8_t expected[6] = {0x19, 0xE8, 0x1E, 0xC4, 0x3D, 0x5E};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, block, 6);
}

void test_constants_match_the_library(void) {
    // A divergence here would put the component's frames off by a byte.
    TEST_ASSERT_EQUAL_UINT(iohome::IV_SIZE, esphome::iown_homecontrol::IOHC_IV_SIZE);
    TEST_ASSERT_EQUAL_HEX8(iohome::IV_PADDING, esphome::iown_homecontrol::IOHC_IV_PADDING);
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_crc_matches_the_library);
    RUN_TEST(test_crc_matches_the_captured_frames);
    RUN_TEST(test_iv_matches_the_library);
    RUN_TEST(test_iv_layout);
    RUN_TEST(test_mac_matches_the_documented_vector);
    RUN_TEST(test_constants_match_the_library);

    return UNITY_END();
}
