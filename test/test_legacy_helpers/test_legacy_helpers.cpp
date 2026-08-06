/**
 * The C helper layer under include/ and src/esp32_*.
 *
 * It predates the protocol library, nothing in IoHomeControl uses it, and it
 * had never been exercised - which is how it came to ship a CRC function whose
 * body was entirely commented out, returning 0xFFFF to every caller, and length
 * macros that contradicted the protocol layer.
 *
 * The CI job that compiles these headers as C proves they parse. This proves
 * the values are right.
 */

#include <unity.h>
#include <string.h>

extern "C" {
#include "iown.h"
#include "iown_node_types.h"
}

#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"

// ---------------------------------------------------------------------------
// Frame geometry
// ---------------------------------------------------------------------------

void test_size_field_bias(void) {
    // The field counts everything except Control Byte 0 and the CRC, so the
    // frame is three bytes longer than it says. The original code used 11.
    TEST_ASSERT_EQUAL_INT(3, IOWN_FRAME_SIZE_BIAS);

    // scripts/io-homecontrol.ksy: control byte 0xF8, size field 24, 27 bytes.
    TEST_ASSERT_EQUAL_INT(24, IOWN_FRAME_SIZE(0xF8));
    TEST_ASSERT_EQUAL_INT(27, IOWN_FRAME_LEN(0xF8));

    // docs/linklayer.md: 0xF6 -> 25 bytes, 0xC8 -> 11, 0xD1 -> 20.
    TEST_ASSERT_EQUAL_INT(25, IOWN_FRAME_LEN(0xF6));
    TEST_ASSERT_EQUAL_INT(11, IOWN_FRAME_LEN(0xC8));
    TEST_ASSERT_EQUAL_INT(20, IOWN_FRAME_LEN(0xD1));

    // The 5-bit field tops out at 34 bytes.
    TEST_ASSERT_EQUAL_INT(34, IOWN_FRAME_LEN_MAX);
    TEST_ASSERT_EQUAL_INT(34, IOWN_FRAME_LEN(0xFF));
}

void test_lengths_agree_with_the_protocol_layer(void) {
    // Two descriptions of one wire format. Where they disagreed, the C headers
    // were the ones that were wrong: a two-byte sync word, a 2W packet with no
    // HMAC, a flat one-byte parameter.
    TEST_ASSERT_EQUAL_INT(iohome::NODE_ID_SIZE, IOWN_LEN_NODEID);
    TEST_ASSERT_EQUAL_INT(iohome::SYNC_WORD_LEN, IOWN_LEN_SYNC_WORD);
    TEST_ASSERT_EQUAL_INT(iohome::CRC_SIZE, IOWN_LEN_CRC);
    TEST_ASSERT_EQUAL_INT(iohome::HMAC_SIZE, IOWN_LEN_HMAC);
    TEST_ASSERT_EQUAL_INT(iohome::ROLLING_CODE_SIZE, IOWN_LEN_ROLLING_CODE);
    TEST_ASSERT_EQUAL_INT(iohome::FRAME_HEADER_SIZE, IOWN_LEN_HEADER);
    TEST_ASSERT_EQUAL_INT(iohome::FRAME_SIZE_FIELD_BIAS, IOWN_FRAME_SIZE_BIAS);
    TEST_ASSERT_EQUAL_INT(iohome::FRAME_MAX_SIZE, IOWN_FRAME_LEN_MAX);
    TEST_ASSERT_EQUAL_INT(iohome::CTRL0_LENGTH_MASK, IOWN_FRAME_SIZE_MASK);

    // A 1W frame carries sequence + MAC, a 2W frame carries only the MAC.
    TEST_ASSERT_EQUAL_INT(iohome::AUTH_TRAILER_SIZE_1W,
                          IOWN_LEN_PACKET_1W(0) - IOWN_LEN_PACKET_PLAIN(0));
    TEST_ASSERT_EQUAL_INT(iohome::AUTH_TRAILER_SIZE_2W,
                          IOWN_LEN_PACKET_2W(0) - IOWN_LEN_PACKET_PLAIN(0));

    // The captured frames again, this time through the packet-length macros.
    TEST_ASSERT_EQUAL_INT(25, IOWN_LEN_FRAME(IOWN_MODE_1W, 6));
    TEST_ASSERT_EQUAL_INT(27, IOWN_LEN_FRAME(IOWN_MODE_1W, 8));
}

// ---------------------------------------------------------------------------
// Broadcast address
// ---------------------------------------------------------------------------

void test_broadcast_address(void) {
    // The macro referenced a symbol that never existed, and the address was
    // sized with IOWN_LEN_HEADER_MAC - two node IDs - so the comparison read
    // past the caller's buffer.
    const uint8_t broadcast[IOWN_LEN_NODEID] = {0x00, 0x00, 0x3F};
    const uint8_t other[IOWN_LEN_NODEID] = {0x70, 0x87, 0x58};

    TEST_ASSERT_TRUE(IOWN_IS_BROADCAST_ADDR(broadcast));
    TEST_ASSERT_FALSE(IOWN_IS_BROADCAST_ADDR(other));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(iohome::ADDRESS_BROADCAST, IOWN_NODEID_BROADCAST,
                                  IOWN_LEN_NODEID);

    // Only the first three bytes may be read. Put a sentinel right after a
    // three-byte address and check it is never consulted.
    struct { uint8_t addr[3]; uint8_t sentinel[3]; } packed = {
        {0x00, 0x00, 0x3F}, {0xFF, 0xFF, 0xFF}};
    TEST_ASSERT_TRUE(IOWN_IS_BROADCAST_ADDR(packed.addr));
}

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

void test_legacy_crc_matches_the_captures(void) {
    // This function used to return -1 from a uint16_t - 0xFFFF - for every
    // input, with its entire body commented out.
    static const uint8_t ksy[] = {
        0xf8, 0x00, 0x00, 0x00, 0x7f, 0x70, 0x87, 0x58, 0x00,
        0x01, 0x61, 0xd4, 0x00, 0x80, 0xc8, 0x00, 0x00,
        0x3b, 0xd5,
        0x05, 0x52, 0x68, 0x75, 0x49, 0x9c,
        0x7e, 0x72};
    TEST_ASSERT_EQUAL_HEX16(0x727E, iown_crc_calc(ksy, sizeof(ksy) - 2));

    static const uint8_t doc[] = {
        0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
        0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
        0x3B, 0xD2,
        0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8,
        0xA9, 0x37};
    TEST_ASSERT_EQUAL_HEX16(0x37A9, iown_crc_calc(doc, sizeof(doc) - 2));

    // Over the whole frame, CRC included, the result is zero.
    TEST_ASSERT_EQUAL_HEX16(0x0000, iown_crc_calc(ksy, sizeof(ksy)));
}

void test_legacy_crc_matches_the_protocol_layer(void) {
    uint8_t buffer[64];
    for (size_t i = 0; i < sizeof(buffer); i++) {
        buffer[i] = static_cast<uint8_t>((i * 53) ^ 0x9C);
    }
    for (uint8_t len = 1; len < sizeof(buffer); len++) {
        TEST_ASSERT_EQUAL_HEX16(iohome::crypto::compute_crc16(buffer, len),
                                iown_crc_calc(buffer, len));
    }
}

void test_legacy_crc_rejects_bad_input(void) {
    const uint8_t data[2] = {0x00, 0x01};
    // 0xFFFF was the old return for everything; 0 is a value a caller can act
    // on, and neither input has a meaningful checksum.
    TEST_ASSERT_EQUAL_HEX16(0, iown_crc_calc(nullptr, 4));
    TEST_ASSERT_EQUAL_HEX16(0, iown_crc_calc(data, 0));
}

// ---------------------------------------------------------------------------
// Node types
// ---------------------------------------------------------------------------

void test_node_type_values(void) {
    // docs/commands.md, actuator type list. These are read off the wire, so the
    // numbers matter even though the enum is only ever used for display.
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(iown_node_actuator_types::Unknown));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(iown_node_actuator_types::VenetianBlind));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(iown_node_actuator_types::RollingShutter));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(iown_node_actuator_types::WindowOpener));
    TEST_ASSERT_EQUAL_INT(9, static_cast<int>(iown_node_actuator_types::MotorizedBolt));
    TEST_ASSERT_EQUAL_INT(17, static_cast<int>(iown_node_actuator_types::ExternalVenetianBlind));
    TEST_ASSERT_EQUAL_INT(24, static_cast<int>(iown_node_actuator_types::SwingingShutter));
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_size_field_bias);
    RUN_TEST(test_lengths_agree_with_the_protocol_layer);
    RUN_TEST(test_broadcast_address);
    RUN_TEST(test_legacy_crc_matches_the_captures);
    RUN_TEST(test_legacy_crc_matches_the_protocol_layer);
    RUN_TEST(test_legacy_crc_rejects_bad_input);
    RUN_TEST(test_node_type_values);

    return UNITY_END();
}
