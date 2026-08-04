/**
 * Unit tests for the io-homecontrol cryptographic primitives.
 */

#include <unity.h>
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_constants.h"
#include <string.h>

namespace crypto = iohome::crypto;

// ---------------------------------------------------------------------------
// CRC-16/KERMIT
// ---------------------------------------------------------------------------

void test_crc16_byte_initial(void) {
    TEST_ASSERT_EQUAL_UINT16(0x0000, crypto::compute_crc16_byte(0x00, 0x0000));
}

void test_crc16_byte_nonzero(void) {
    TEST_ASSERT_NOT_EQUAL(0x0000, crypto::compute_crc16_byte(0x01, 0x0000));
}

void test_crc16_known_vector(void) {
    // "123456789" -> CRC-16/KERMIT = 0x2189
    const uint8_t data[] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
    TEST_ASSERT_EQUAL_HEX16(0x2189, crypto::compute_crc16(data, 9));
}

void test_crc16_matches_captured_frame(void) {
    // docs/linklayer.md packet dump. The frame ends with the CRC bytes A9 37,
    // transmitted least significant byte first.
    const uint8_t covered[] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
                               0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
                               0x3B, 0xD2,
                               0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8};

    const uint16_t crc = crypto::compute_crc16(covered, sizeof(covered));
    TEST_ASSERT_EQUAL_HEX8(0xA9, crc & 0xFF);
    TEST_ASSERT_EQUAL_HEX8(0x37, (crc >> 8) & 0xFF);
}

void test_crc16_empty(void) {
    TEST_ASSERT_EQUAL_UINT16(0x0000, crypto::compute_crc16(nullptr, 0));
}

void test_crc16_incremental(void) {
    const uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    const uint16_t crc_once = crypto::compute_crc16(data, 4);

    uint16_t crc_inc = crypto::compute_crc16_byte(data[0], 0);
    crc_inc = crypto::compute_crc16_byte(data[1], crc_inc);
    crc_inc = crypto::compute_crc16_byte(data[2], crc_inc);
    crc_inc = crypto::compute_crc16_byte(data[3], crc_inc);

    TEST_ASSERT_EQUAL_HEX16(crc_once, crc_inc);
}

void test_verify_crc16_valid(void) {
    uint8_t frame[6] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x00};
    const uint16_t crc = crypto::compute_crc16(frame, 4);
    frame[4] = crc & 0xFF;
    frame[5] = (crc >> 8) & 0xFF;

    TEST_ASSERT_TRUE(crypto::verify_crc16(frame, 6));
}

void test_verify_crc16_invalid(void) {
    const uint8_t frame[6] = {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB};
    TEST_ASSERT_FALSE(crypto::verify_crc16(frame, 6));
}

void test_verify_crc16_rejects_degenerate_lengths(void) {
    const uint8_t frame[2] = {0x00, 0x00};

    // A buffer that is nothing but the CRC covers no data; accepting it would
    // let a 2-byte noise burst pass as a valid frame.
    TEST_ASSERT_FALSE(crypto::verify_crc16(frame, 2));
    TEST_ASSERT_FALSE(crypto::verify_crc16(frame, 1));
    TEST_ASSERT_FALSE(crypto::verify_crc16(frame, 0));
    TEST_ASSERT_FALSE(crypto::verify_crc16(nullptr, 8));
}

// ---------------------------------------------------------------------------
// AES-128
// ---------------------------------------------------------------------------

void test_aes128_fips197_vector(void) {
    // FIPS-197 Appendix C.1
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const uint8_t expected[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                  0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};

    uint8_t cipher[16];
    TEST_ASSERT_TRUE(crypto::aes128_encrypt(plain, key, cipher));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cipher, 16);

    uint8_t recovered[16];
    TEST_ASSERT_TRUE(crypto::aes128_decrypt(cipher, key, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(plain, recovered, 16);
}

void test_aes128_fips197_appendix_b(void) {
    const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                             0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const uint8_t plain[16] = {0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
                               0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34};
    const uint8_t expected[16] = {0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
                                  0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32};

    uint8_t cipher[16];
    TEST_ASSERT_TRUE(crypto::aes128_encrypt(plain, key, cipher));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cipher, 16);
}

void test_aes128_rejects_nullptr(void) {
    const uint8_t key[16] = {0};
    const uint8_t block[16] = {0};
    uint8_t out[16];

    TEST_ASSERT_FALSE(crypto::aes128_encrypt(nullptr, key, out));
    TEST_ASSERT_FALSE(crypto::aes128_encrypt(block, nullptr, out));
    TEST_ASSERT_FALSE(crypto::aes128_encrypt(block, key, nullptr));
    TEST_ASSERT_FALSE(crypto::aes128_decrypt(nullptr, key, out));
}

// ---------------------------------------------------------------------------
// IV construction
// ---------------------------------------------------------------------------

void test_iv_1w_layout(void) {
    const uint8_t data[] = {0x00, 0x01, 0x61, 0xD2, 0x00, 0x00, 0x00};
    const uint8_t seq[2] = {0x3B, 0xD2};

    uint8_t iv[16];
    crypto::construct_iv_1w(data, sizeof(data), seq, iv);

    // Bytes 0-6 mirror the frame data, byte 7 is padding (data is 7 bytes).
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, iv, 7);
    TEST_ASSERT_EQUAL_HEX8(iohome::IV_PADDING, iv[7]);

    // Bytes 10-11 carry the sequence number, 12-15 are padding.
    TEST_ASSERT_EQUAL_HEX8(0x3B, iv[10]);
    TEST_ASSERT_EQUAL_HEX8(0xD2, iv[11]);
    for (int i = 12; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8(iohome::IV_PADDING, iv[i]);
    }
}

void test_iv_1w_pads_short_data(void) {
    const uint8_t data[] = {0xAB};
    const uint8_t seq[2] = {0x00, 0x00};

    uint8_t iv[16];
    crypto::construct_iv_1w(data, sizeof(data), seq, iv);

    TEST_ASSERT_EQUAL_HEX8(0xAB, iv[0]);
    for (int i = 1; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(iohome::IV_PADDING, iv[i]);
    }
}

void test_iv_1w_truncates_long_data(void) {
    uint8_t data[20];
    for (int i = 0; i < 20; i++) {
        data[i] = static_cast<uint8_t>(i + 1);
    }
    const uint8_t seq[2] = {0x11, 0x22};

    uint8_t iv[16];
    crypto::construct_iv_1w(data, sizeof(data), seq, iv);

    // Only the first 8 data bytes land in the IV, but the checksum covers all.
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, iv, 8);
    TEST_ASSERT_EQUAL_HEX8(0x11, iv[10]);
}

void test_iv_2w_layout(void) {
    const uint8_t data[] = {0x3D, 0x01};
    const uint8_t challenge[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

    uint8_t iv[16];
    crypto::construct_iv_2w(data, sizeof(data), challenge, iv);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, iv, 2);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(challenge, &iv[10], 6);
}

void test_iv_construction_survives_nullptr(void) {
    uint8_t iv[16];

    // Must not crash and must produce a fully initialised IV.
    crypto::construct_iv_1w(nullptr, 0, nullptr, iv);
    crypto::construct_iv_2w(nullptr, 0, nullptr, iv);
    crypto::construct_iv_1w(nullptr, 0, nullptr, nullptr);

    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(iohome::IV_PADDING, iv[i]);
    }
}

void test_checksum_is_order_dependent(void) {
    uint8_t a1 = 0, a2 = 0, b1 = 0, b2 = 0;

    crypto::compute_checksum(0x01, a1, a2);
    crypto::compute_checksum(0x02, a1, a2);

    crypto::compute_checksum(0x02, b1, b2);
    crypto::compute_checksum(0x01, b1, b2);

    TEST_ASSERT_TRUE(a1 != b1 || a2 != b2);
}

// ---------------------------------------------------------------------------
// MAC generation
// ---------------------------------------------------------------------------

void test_1w_mac_is_deterministic(void) {
    const uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                             0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
    const uint8_t data[] = {0x00, 0x01, 0x61, 0xD2, 0x00, 0x00, 0x00};
    const uint8_t seq[2] = {0x01, 0x00};

    uint8_t mac_a[6], mac_b[6];
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq, key, mac_a));
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq, key, mac_b));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(mac_a, mac_b, 6);
}

void test_1w_mac_changes_with_sequence(void) {
    const uint8_t key[16] = {0x42};
    const uint8_t data[] = {0x00, 0x01, 0x61, 0xD2, 0x00, 0x00, 0x00};
    const uint8_t seq_a[2] = {0x01, 0x00};
    const uint8_t seq_b[2] = {0x02, 0x00};

    uint8_t mac_a[6], mac_b[6];
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq_a, key, mac_a));
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq_b, key, mac_b));

    TEST_ASSERT_FALSE(memcmp(mac_a, mac_b, 6) == 0);
}

void test_1w_mac_changes_with_key(void) {
    const uint8_t key_a[16] = {0x01};
    const uint8_t key_b[16] = {0x02};
    const uint8_t data[] = {0x00, 0x01};
    const uint8_t seq[2] = {0x00, 0x00};

    uint8_t mac_a[6], mac_b[6];
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq, key_a, mac_a));
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq, key_b, mac_b));

    TEST_ASSERT_FALSE(memcmp(mac_a, mac_b, 6) == 0);
}

void test_2w_mac_changes_with_challenge(void) {
    const uint8_t key[16] = {0x33};
    const uint8_t data[] = {0x00, 0x01};
    const uint8_t challenge_a[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t challenge_b[6] = {1, 2, 3, 4, 5, 7};

    uint8_t mac_a[6], mac_b[6];
    TEST_ASSERT_TRUE(crypto::create_2w_hmac(data, sizeof(data), challenge_a, key, mac_a));
    TEST_ASSERT_TRUE(crypto::create_2w_hmac(data, sizeof(data), challenge_b, key, mac_b));

    TEST_ASSERT_FALSE(memcmp(mac_a, mac_b, 6) == 0);
}

void test_verify_hmac_accepts_and_rejects(void) {
    const uint8_t key[16] = {0x77};
    const uint8_t data[] = {0x00, 0x01, 0x61, 0xD2, 0x00, 0x00, 0x00};
    const uint8_t seq[2] = {0x05, 0x00};

    uint8_t mac[6];
    TEST_ASSERT_TRUE(crypto::create_1w_hmac(data, sizeof(data), seq, key, mac));
    TEST_ASSERT_TRUE(crypto::verify_hmac(data, sizeof(data), mac, seq, key, false));

    // Any single-bit change must be caught.
    for (int i = 0; i < 6; i++) {
        uint8_t tampered[6];
        memcpy(tampered, mac, 6);
        tampered[i] ^= 0x01;
        TEST_ASSERT_FALSE(crypto::verify_hmac(data, sizeof(data), tampered, seq, key, false));
    }
}

void test_mac_functions_reject_nullptr(void) {
    const uint8_t key[16] = {0};
    const uint8_t seq[2] = {0};
    uint8_t mac[6];

    TEST_ASSERT_FALSE(crypto::create_1w_hmac(nullptr, 0, nullptr, key, mac));
    TEST_ASSERT_FALSE(crypto::create_1w_hmac(nullptr, 0, seq, nullptr, mac));
    TEST_ASSERT_FALSE(crypto::create_1w_hmac(nullptr, 0, seq, key, nullptr));
    TEST_ASSERT_FALSE(crypto::create_2w_hmac(nullptr, 0, nullptr, key, mac));
    TEST_ASSERT_FALSE(crypto::verify_hmac(nullptr, 0, nullptr, seq, key, false));
    TEST_ASSERT_FALSE(crypto::verify_hmac(nullptr, 0, mac, nullptr, key, false));
}

// ---------------------------------------------------------------------------
// Key masking
// ---------------------------------------------------------------------------

void test_1w_key_masking_roundtrip(void) {
    const uint8_t system_key[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                    0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t node[3] = {0x1A, 0x38, 0x0B};

    uint8_t encrypted[16];
    TEST_ASSERT_TRUE(crypto::encrypt_1w_key(system_key, node, encrypted));
    TEST_ASSERT_FALSE(memcmp(system_key, encrypted, 16) == 0);

    uint8_t recovered[16];
    TEST_ASSERT_TRUE(crypto::decrypt_1w_key(encrypted, node, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(system_key, recovered, 16);
}

void test_1w_key_masking_is_node_specific(void) {
    const uint8_t system_key[16] = {0x5A};
    const uint8_t node_a[3] = {0x01, 0x02, 0x03};
    const uint8_t node_b[3] = {0x01, 0x02, 0x04};

    uint8_t out_a[16], out_b[16];
    TEST_ASSERT_TRUE(crypto::encrypt_1w_key(system_key, node_a, out_a));
    TEST_ASSERT_TRUE(crypto::encrypt_1w_key(system_key, node_b, out_b));

    TEST_ASSERT_FALSE(memcmp(out_a, out_b, 16) == 0);
}

void test_2w_key_masking_roundtrip(void) {
    const uint8_t system_key[16] = {0xDE, 0xAD, 0xBE, 0xEF};
    const uint8_t challenge[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};

    uint8_t encrypted[16];
    TEST_ASSERT_TRUE(crypto::encrypt_2w_key(system_key, challenge, encrypted));

    uint8_t recovered[16];
    TEST_ASSERT_TRUE(crypto::decrypt_2w_key(encrypted, challenge, recovered));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(system_key, recovered, 16);
}

void test_key_masking_rejects_nullptr(void) {
    const uint8_t key[16] = {0};
    const uint8_t node[3] = {0};
    const uint8_t challenge[6] = {0};
    uint8_t out[16];

    TEST_ASSERT_FALSE(crypto::encrypt_1w_key(nullptr, node, out));
    TEST_ASSERT_FALSE(crypto::encrypt_1w_key(key, nullptr, out));
    TEST_ASSERT_FALSE(crypto::encrypt_1w_key(key, node, nullptr));
    TEST_ASSERT_FALSE(crypto::encrypt_2w_key(nullptr, challenge, out));
    TEST_ASSERT_FALSE(crypto::encrypt_2w_key(key, nullptr, out));
}

// ---------------------------------------------------------------------------
// Side-channel helpers and RNG
// ---------------------------------------------------------------------------

void test_constant_time_equal(void) {
    const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t b[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t c[6] = {1, 2, 3, 4, 5, 7};

    TEST_ASSERT_TRUE(crypto::constant_time_equal(a, b, 6));
    TEST_ASSERT_FALSE(crypto::constant_time_equal(a, c, 6));
    TEST_ASSERT_TRUE(crypto::constant_time_equal(a, c, 5));   // prefix matches
    TEST_ASSERT_FALSE(crypto::constant_time_equal(nullptr, b, 6));
    TEST_ASSERT_FALSE(crypto::constant_time_equal(a, nullptr, 6));
}

void test_secure_zero(void) {
    uint8_t secret[16];
    memset(secret, 0xAB, sizeof(secret));

    crypto::secure_zero(secret, sizeof(secret));
    for (size_t i = 0; i < sizeof(secret); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x00, secret[i]);
    }

    crypto::secure_zero(nullptr, 16);  // must not crash
}

void test_random_bytes_are_available_and_varying(void) {
    TEST_ASSERT_TRUE(crypto::has_secure_random());

    uint8_t a[16], b[16];
    TEST_ASSERT_TRUE(crypto::random_bytes(a, sizeof(a)));
    TEST_ASSERT_TRUE(crypto::random_bytes(b, sizeof(b)));

    // Two 128-bit draws colliding would be a one-in-2^128 event.
    TEST_ASSERT_FALSE(memcmp(a, b, sizeof(a)) == 0);

    TEST_ASSERT_FALSE(crypto::random_bytes(nullptr, 4));
    TEST_ASSERT_TRUE(crypto::random_bytes(a, 0));
}

void test_random_bytes_respects_length(void) {
    // Regression guard for the word-at-a-time ESP32 path: a request that is not
    // a multiple of 4 must fill exactly `len` bytes and not one more.
    for (size_t len = 1; len <= 16; len++) {
        uint8_t buffer[24];
        memset(buffer, 0xCC, sizeof(buffer));

        TEST_ASSERT_TRUE(crypto::random_bytes(buffer, len));

        // Everything past the requested length must still hold the guard value.
        for (size_t i = len; i < sizeof(buffer); i++) {
            TEST_ASSERT_EQUAL_HEX8(0xCC, buffer[i]);
        }
    }
}

void test_generate_system_key(void) {
    uint8_t key_a[16], key_b[16];
    TEST_ASSERT_TRUE(crypto::generate_system_key(key_a));
    TEST_ASSERT_TRUE(crypto::generate_system_key(key_b));
    TEST_ASSERT_FALSE(memcmp(key_a, key_b, 16) == 0);
    TEST_ASSERT_FALSE(crypto::generate_system_key(nullptr));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_crc16_byte_initial);
    RUN_TEST(test_crc16_byte_nonzero);
    RUN_TEST(test_crc16_known_vector);
    RUN_TEST(test_crc16_matches_captured_frame);
    RUN_TEST(test_crc16_empty);
    RUN_TEST(test_crc16_incremental);
    RUN_TEST(test_verify_crc16_valid);
    RUN_TEST(test_verify_crc16_invalid);
    RUN_TEST(test_verify_crc16_rejects_degenerate_lengths);

    RUN_TEST(test_aes128_fips197_vector);
    RUN_TEST(test_aes128_fips197_appendix_b);
    RUN_TEST(test_aes128_rejects_nullptr);

    RUN_TEST(test_iv_1w_layout);
    RUN_TEST(test_iv_1w_pads_short_data);
    RUN_TEST(test_iv_1w_truncates_long_data);
    RUN_TEST(test_iv_2w_layout);
    RUN_TEST(test_iv_construction_survives_nullptr);
    RUN_TEST(test_checksum_is_order_dependent);

    RUN_TEST(test_1w_mac_is_deterministic);
    RUN_TEST(test_1w_mac_changes_with_sequence);
    RUN_TEST(test_1w_mac_changes_with_key);
    RUN_TEST(test_2w_mac_changes_with_challenge);
    RUN_TEST(test_verify_hmac_accepts_and_rejects);
    RUN_TEST(test_mac_functions_reject_nullptr);

    RUN_TEST(test_1w_key_masking_roundtrip);
    RUN_TEST(test_1w_key_masking_is_node_specific);
    RUN_TEST(test_2w_key_masking_roundtrip);
    RUN_TEST(test_key_masking_rejects_nullptr);

    RUN_TEST(test_constant_time_equal);
    RUN_TEST(test_secure_zero);
    RUN_TEST(test_random_bytes_are_available_and_varying);
    RUN_TEST(test_random_bytes_respects_length);
    RUN_TEST(test_generate_system_key);

    return UNITY_END();
}
