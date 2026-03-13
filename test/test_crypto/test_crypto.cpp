#include <unity.h>
#include "protocol/iohome_crypto.h"
#include "protocol/iohome_constants.h"
#include <string.h>

void test_crc16_byte_initial(void) {
    uint16_t crc = iohome::crypto::compute_crc16_byte(0x00, 0x0000);
    TEST_ASSERT_EQUAL_UINT16(0x0000, crc);
}

void test_crc16_byte_nonzero(void) {
    uint16_t crc = iohome::crypto::compute_crc16_byte(0x01, 0x0000);
    TEST_ASSERT_NOT_EQUAL(0x0000, crc);
}

void test_crc16_known_vector(void) {
    // "123456789" should give CRC-16/KERMIT = 0x2189
    const uint8_t data[] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39};
    uint16_t crc = iohome::crypto::compute_crc16(data, 9);
    TEST_ASSERT_EQUAL_HEX16(0x2189, crc);
}

void test_crc16_empty(void) {
    uint16_t crc = iohome::crypto::compute_crc16(nullptr, 0);
    TEST_ASSERT_EQUAL_UINT16(0x0000, crc);
}

void test_crc16_incremental(void) {
    const uint8_t data[] = {0xAA, 0xBB, 0xCC, 0xDD};
    uint16_t crc_once = iohome::crypto::compute_crc16(data, 4);

    uint16_t crc_inc = iohome::crypto::compute_crc16_byte(data[0], 0);
    crc_inc = iohome::crypto::compute_crc16_byte(data[1], crc_inc);
    crc_inc = iohome::crypto::compute_crc16_byte(data[2], crc_inc);
    crc_inc = iohome::crypto::compute_crc16_byte(data[3], crc_inc);

    TEST_ASSERT_EQUAL_HEX16(crc_once, crc_inc);
}

void test_verify_crc16_valid(void) {
    uint8_t frame[6] = {0x11, 0x22, 0x33, 0x44, 0x00, 0x00};
    uint16_t crc = iohome::crypto::compute_crc16(frame, 4);
    frame[4] = crc & 0xFF;
    frame[5] = (crc >> 8) & 0xFF;

    TEST_ASSERT_TRUE(iohome::crypto::verify_crc16(frame, 6));
}

void test_verify_crc16_invalid(void) {
    uint8_t frame[6] = {0x11, 0x22, 0x33, 0x44, 0xAA, 0xBB};
    TEST_ASSERT_FALSE(iohome::crypto::verify_crc16(frame, 6));
}

void test_verify_crc16_too_short(void) {
    uint8_t frame[1] = {0x11};
    TEST_ASSERT_FALSE(iohome::crypto::verify_crc16(frame, 1));
}

void test_checksum_deterministic(void) {
    uint8_t chk1a = 0, chk2a = 0;
    uint8_t chk1b = 0, chk2b = 0;

    iohome::crypto::compute_checksum(0xAA, chk1a, chk2a);
    iohome::crypto::compute_checksum(0xAA, chk1b, chk2b);

    TEST_ASSERT_EQUAL_UINT8(chk1a, chk1b);
    TEST_ASSERT_EQUAL_UINT8(chk2a, chk2b);
}

void test_checksum_varies_with_input(void) {
    uint8_t chk1a = 0, chk2a = 0;
    uint8_t chk1b = 0, chk2b = 0;

    iohome::crypto::compute_checksum(0x00, chk1a, chk2a);
    iohome::crypto::compute_checksum(0xFF, chk1b, chk2b);

    TEST_ASSERT_TRUE(chk1a != chk1b || chk2a != chk2b);
}

void test_construct_iv_1w_padding(void) {
    uint8_t data[] = {0x01, 0x02};
    uint8_t seq[2] = {0x10, 0x20};
    uint8_t iv[16];

    iohome::crypto::construct_iv_1w(data, 2, seq, iv);

    TEST_ASSERT_EQUAL_UINT8(0x01, iv[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, iv[1]);

    // Bytes 2-7 should be 0x55 padding
    for (int i = 2; i < 8; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x55, iv[i]);
    }

    // Bytes 10-11 should be sequence number
    TEST_ASSERT_EQUAL_UINT8(0x10, iv[10]);
    TEST_ASSERT_EQUAL_UINT8(0x20, iv[11]);

    // Bytes 12-15 should be 0x55 padding
    for (int i = 12; i < 16; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x55, iv[i]);
    }
}

void test_construct_iv_2w_challenge(void) {
    uint8_t data[] = {0x3C, 0xAA, 0xBB};
    uint8_t challenge[6] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6};
    uint8_t iv[16];

    iohome::crypto::construct_iv_2w(data, 3, challenge, iv);

    TEST_ASSERT_EQUAL_UINT8(0x3C, iv[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, iv[1]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, iv[2]);

    // Bytes 10-15 should be challenge
    TEST_ASSERT_EQUAL_UINT8(0xC1, iv[10]);
    TEST_ASSERT_EQUAL_UINT8(0xC2, iv[11]);
    TEST_ASSERT_EQUAL_UINT8(0xC3, iv[12]);
    TEST_ASSERT_EQUAL_UINT8(0xC4, iv[13]);
    TEST_ASSERT_EQUAL_UINT8(0xC5, iv[14]);
    TEST_ASSERT_EQUAL_UINT8(0xC6, iv[15]);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_crc16_byte_initial);
    RUN_TEST(test_crc16_byte_nonzero);
    RUN_TEST(test_crc16_known_vector);
    RUN_TEST(test_crc16_empty);
    RUN_TEST(test_crc16_incremental);
    RUN_TEST(test_verify_crc16_valid);
    RUN_TEST(test_verify_crc16_invalid);
    RUN_TEST(test_verify_crc16_too_short);
    RUN_TEST(test_checksum_deterministic);
    RUN_TEST(test_checksum_varies_with_input);
    RUN_TEST(test_construct_iv_1w_padding);
    RUN_TEST(test_construct_iv_2w_challenge);

    return UNITY_END();
}
