#include <unity.h>
#include "protocol/iohome_frame.h"
#include "protocol/iohome_constants.h"
#include <string.h>

void test_init_frame_1w(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    TEST_ASSERT_TRUE(frame.is_1w_mode);
    TEST_ASSERT_EQUAL_UINT8(0, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.ctrl_byte_1);
}

void test_init_frame_2w(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, false);

    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_TRUE((frame.ctrl_byte_0 & iohome::CTRL0_PROTOCOL_MASK) != 0);
}

void test_init_frame_nullptr(void) {
    // Should not crash
    iohome::frame::init_frame(nullptr, true);
}

void test_set_destination(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t dest[3] = {0xAA, 0xBB, 0xCC};
    iohome::frame::set_destination(&frame, dest);

    TEST_ASSERT_EQUAL_UINT8(0xAA, frame.dest_node[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBB, frame.dest_node[1]);
    TEST_ASSERT_EQUAL_UINT8(0xCC, frame.dest_node[2]);
}

void test_set_source(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t src[3] = {0x11, 0x22, 0x33};
    iohome::frame::set_source(&frame, src);

    TEST_ASSERT_EQUAL_UINT8(0x11, frame.src_node[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22, frame.src_node[1]);
    TEST_ASSERT_EQUAL_UINT8(0x33, frame.src_node[2]);
}

void test_set_command_basic(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t params[] = {0x01, 0x02};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, 0x60, params, 2));

    TEST_ASSERT_EQUAL_UINT8(0x60, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(2, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, frame.data[1]);
}

void test_set_command_no_params(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, 0x3C, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT8(0x3C, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(0, frame.data_len);
}

void test_set_command_too_large(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t params[22] = {0};
    TEST_ASSERT_FALSE(iohome::frame::set_command(&frame, 0x60, params, 22));
}

void test_set_command_max_size(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t params[21] = {0};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, 0x60, params, 21));
    TEST_ASSERT_EQUAL_UINT8(21, frame.data_len);
}

void test_set_rolling_code(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    iohome::frame::set_rolling_code(&frame, 0x1234);

    TEST_ASSERT_EQUAL_UINT8(0x34, frame.rolling_code[0]);  // LSB
    TEST_ASSERT_EQUAL_UINT8(0x12, frame.rolling_code[1]);  // MSB
}

void test_is_2w_mode(void) {
    TEST_ASSERT_FALSE(iohome::frame::is_2w_mode(0x00));
    TEST_ASSERT_TRUE(iohome::frame::is_2w_mode(0x20));
    TEST_ASSERT_TRUE(iohome::frame::is_2w_mode(0xFF));
    TEST_ASSERT_FALSE(iohome::frame::is_2w_mode(0xDF));
}

void test_get_frame_length(void) {
    TEST_ASSERT_EQUAL_UINT8(11, iohome::frame::get_frame_length(0x00));
    TEST_ASSERT_EQUAL_UINT8(12, iohome::frame::get_frame_length(0x01));
    TEST_ASSERT_EQUAL_UINT8(32, iohome::frame::get_frame_length(0x15));
}

void test_is_broadcast(void) {
    uint8_t bcast[] = {0x00, 0x00, 0x00};
    uint8_t unicast[] = {0x1A, 0x38, 0x0B};

    TEST_ASSERT_TRUE(iohome::frame::is_broadcast(bcast));
    TEST_ASSERT_FALSE(iohome::frame::is_broadcast(unicast));
}

void test_serialize_frame_1w(void) {
    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t dest[] = {0xAA, 0xBB, 0xCC};
    uint8_t src[] = {0x11, 0x22, 0x33};
    iohome::frame::set_destination(&frame, dest);
    iohome::frame::set_source(&frame, src);

    uint8_t params[] = {0x64, 0x00};
    iohome::frame::set_command(&frame, 0x60, params, 2);
    iohome::frame::set_rolling_code(&frame, 0x0001);

    uint8_t buffer[64];
    size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    TEST_ASSERT_GREATER_THAN(0, len);
    TEST_ASSERT_EQUAL(frame.frame_length, len);

    TEST_ASSERT_EQUAL_UINT8(frame.ctrl_byte_0, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(frame.ctrl_byte_1, buffer[1]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, buffer[2]);
    TEST_ASSERT_EQUAL_UINT8(0x11, buffer[5]);
    TEST_ASSERT_EQUAL_UINT8(0x60, buffer[8]);
}

void test_serialize_nullptr(void) {
    uint8_t buffer[64];
    TEST_ASSERT_EQUAL(0, iohome::frame::serialize_frame(nullptr, buffer, sizeof(buffer)));

    iohome::frame::IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_EQUAL(0, iohome::frame::serialize_frame(&frame, nullptr, sizeof(buffer)));
}

void test_parse_frame_too_short(void) {
    uint8_t buffer[5] = {0};
    iohome::frame::IoFrame frame;

    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, 5, &frame));
}

void test_parse_frame_nullptr(void) {
    iohome::frame::IoFrame frame;
    TEST_ASSERT_FALSE(iohome::frame::parse_frame(nullptr, 10, &frame));

    uint8_t buffer[32] = {0};
    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, 32, nullptr));
}

void test_serialize_parse_roundtrip_1w(void) {
    iohome::frame::IoFrame original;
    iohome::frame::init_frame(&original, true);

    uint8_t dest[] = {0xAA, 0xBB, 0xCC};
    uint8_t src[] = {0x11, 0x22, 0x33};
    iohome::frame::set_destination(&original, dest);
    iohome::frame::set_source(&original, src);

    uint8_t params[] = {0x64, 0x00};
    iohome::frame::set_command(&original, 0x60, params, 2);
    iohome::frame::set_rolling_code(&original, 0xABCD);

    memset(original.hmac, 0x42, 6);
    original.crc[0] = 0x00;
    original.crc[1] = 0x00;

    uint8_t buffer[64];
    size_t len = iohome::frame::serialize_frame(&original, buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, len);

    iohome::frame::IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));

    TEST_ASSERT_TRUE(parsed.is_1w_mode);
    TEST_ASSERT_EQUAL_UINT8(original.command_id, parsed.command_id);
    TEST_ASSERT_EQUAL_UINT8(original.data_len, parsed.data_len);
    TEST_ASSERT_EQUAL_UINT8(0xAA, parsed.dest_node[0]);
    TEST_ASSERT_EQUAL_UINT8(0x11, parsed.src_node[0]);
    TEST_ASSERT_EQUAL_UINT8(0xCD, parsed.rolling_code[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAB, parsed.rolling_code[1]);
}

void test_serialize_parse_roundtrip_2w(void) {
    iohome::frame::IoFrame original;
    iohome::frame::init_frame(&original, false);

    uint8_t dest[] = {0xDE, 0xAD, 0x01};
    uint8_t src[] = {0xBE, 0xEF, 0x02};
    iohome::frame::set_destination(&original, dest);
    iohome::frame::set_source(&original, src);

    uint8_t params[] = {0x01};
    iohome::frame::set_command(&original, 0x3C, params, 1);

    memset(original.hmac, 0x55, 6);
    original.crc[0] = 0x00;
    original.crc[1] = 0x00;

    uint8_t buffer[64];
    size_t len = iohome::frame::serialize_frame(&original, buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, len);

    iohome::frame::IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));

    TEST_ASSERT_FALSE(parsed.is_1w_mode);
    TEST_ASSERT_EQUAL_UINT8(0x3C, parsed.command_id);
    TEST_ASSERT_EQUAL_UINT8(1, parsed.data_len);
    TEST_ASSERT_EQUAL_UINT8(0xDE, parsed.dest_node[0]);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_init_frame_1w);
    RUN_TEST(test_init_frame_2w);
    RUN_TEST(test_init_frame_nullptr);
    RUN_TEST(test_set_destination);
    RUN_TEST(test_set_source);
    RUN_TEST(test_set_command_basic);
    RUN_TEST(test_set_command_no_params);
    RUN_TEST(test_set_command_too_large);
    RUN_TEST(test_set_command_max_size);
    RUN_TEST(test_set_rolling_code);
    RUN_TEST(test_is_2w_mode);
    RUN_TEST(test_get_frame_length);
    RUN_TEST(test_is_broadcast);
    RUN_TEST(test_serialize_frame_1w);
    RUN_TEST(test_serialize_nullptr);
    RUN_TEST(test_parse_frame_too_short);
    RUN_TEST(test_parse_frame_nullptr);
    RUN_TEST(test_serialize_parse_roundtrip_1w);
    RUN_TEST(test_serialize_parse_roundtrip_2w);

    return UNITY_END();
}
