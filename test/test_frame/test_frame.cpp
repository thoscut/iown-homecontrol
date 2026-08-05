/**
 * Unit tests for the io-homecontrol frame layer.
 *
 * Several tests replay byte-for-byte captures taken from docs/linklayer.md and
 * docs/commands.md. Those are the ground truth for the wire format: the mode
 * bit, the size field and the placement of the authentication trailer.
 */

#include <unity.h>
#include "protocol/iohome_frame.h"
#include "protocol/iohome_constants.h"
#include <string.h>

using iohome::frame::AuthTrailer;
using iohome::frame::IoFrame;

// ---------------------------------------------------------------------------
// Control Byte 0 semantics
// ---------------------------------------------------------------------------

void test_ctrl0_one_way_bit(void) {
    // docs/linklayer.md: "isOneWay: 0 = 2W = Two Way, 1 = 1W = One Way"
    TEST_ASSERT_TRUE(iohome::frame::is_1w_mode(0x20));
    TEST_ASSERT_FALSE(iohome::frame::is_2w_mode(0x20));

    TEST_ASSERT_FALSE(iohome::frame::is_1w_mode(0x00));
    TEST_ASSERT_TRUE(iohome::frame::is_2w_mode(0x00));

    // 0xF6 is the control byte of the captured 1W frame.
    TEST_ASSERT_TRUE(iohome::frame::is_1w_mode(0xF6));
    // 0xC8 is the control byte of the captured 2W discover frame.
    TEST_ASSERT_TRUE(iohome::frame::is_2w_mode(0xC8));
}

void test_get_frame_length_matches_spec(void) {
    // Size = frame length excluding Control Byte 0 and the CRC => total = Size + 3
    TEST_ASSERT_EQUAL_UINT8(3, iohome::frame::get_frame_length(0x00));
    TEST_ASSERT_EQUAL_UINT8(11, iohome::frame::get_frame_length(0x08));   // capture: C8 -> 11 bytes
    TEST_ASSERT_EQUAL_UINT8(20, iohome::frame::get_frame_length(0xD1));   // capture: D1 -> 20 bytes
    TEST_ASSERT_EQUAL_UINT8(25, iohome::frame::get_frame_length(0xF6));   // capture: F6 -> 25 bytes
    TEST_ASSERT_EQUAL_UINT8(34, iohome::frame::get_frame_length(0x1F));   // largest encodable
}

void test_set_frame_length_roundtrip(void) {
    for (uint8_t total = iohome::FRAME_MIN_SIZE; total <= iohome::FRAME_MAX_SIZE; total++) {
        const uint8_t ctrl0 = iohome::frame::set_frame_length(0xE0, total);
        TEST_ASSERT_EQUAL_UINT8(total, iohome::frame::get_frame_length(ctrl0));
        // Bits outside the size field are preserved.
        TEST_ASSERT_EQUAL_UINT8(0xE0, ctrl0 & 0xE0);
    }
}

void test_order_field(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    iohome::frame::set_order(&frame, iohome::FrameOrder::GROUP_END);
    TEST_ASSERT_EQUAL(iohome::FrameOrder::GROUP_END, iohome::frame::get_order(frame.ctrl_byte_0));

    iohome::frame::set_order(&frame, iohome::FrameOrder::SINGLE);
    TEST_ASSERT_EQUAL(iohome::FrameOrder::SINGLE, iohome::frame::get_order(frame.ctrl_byte_0));

    // Changing the order must not disturb the mode bit or the size field.
    TEST_ASSERT_TRUE(iohome::frame::is_1w_mode(frame.ctrl_byte_0));
    TEST_ASSERT_EQUAL_UINT8(frame.frame_length, iohome::frame::get_frame_length(frame.ctrl_byte_0));
}

// ---------------------------------------------------------------------------
// Captured frames from the documentation
// ---------------------------------------------------------------------------

void test_parse_documented_2w_discover(void) {
    // docs/linklayer.md: "C8 00 00003B F00F00 28 1234"
    const uint8_t capture[] = {0xC8, 0x00, 0x00, 0x00, 0x3B, 0xF0, 0x0F, 0x00, 0x28, 0x12, 0x34};

    IoFrame frame;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &frame));

    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_FALSE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(11, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_DISCOVER, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(0, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x3B, frame.dest_node[2]);
    TEST_ASSERT_EQUAL_UINT8(0xF0, frame.src_node[0]);
}

void test_parse_documented_2w_discover_answer(void) {
    // docs/linklayer.md: "D1 00 F00F00 FEEFEE 29 FFC0 FEEFEE 0C CC 0000 1234"
    const uint8_t capture[] = {0xD1, 0x00, 0xF0, 0x0F, 0x00, 0xFE, 0xEF, 0xEE, 0x29,
                               0xFF, 0xC0, 0xFE, 0xEF, 0xEE, 0x0C, 0xCC, 0x00, 0x00,
                               0x12, 0x34};

    IoFrame frame;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &frame));

    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_FALSE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(20, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_DISCOVER_ANSWER, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(9, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0xFF, frame.data[0]);
}

void test_parse_documented_1w_execute(void) {
    // docs/linklayer.md packet dump:
    //   F6 00 00003F 708758 00 01 61 D200 00 00 3BD2 E6B62CEF54C8 A937
    const uint8_t capture[] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
                               0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
                               0x3B, 0xD2,
                               0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8,
                               0xA9, 0x37};

    TEST_ASSERT_EQUAL_UINT(25, sizeof(capture));

    IoFrame frame;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &frame));

    TEST_ASSERT_TRUE(frame.is_1w_mode);
    TEST_ASSERT_TRUE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(25, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_EXECUTE, frame.command_id);

    // Payload is the 6-byte execute parameter block.
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_SIZE, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame.data[0]);   // Originator = User
    TEST_ASSERT_EQUAL_UINT8(0x61, frame.data[1]);   // ACEI
    TEST_ASSERT_TRUE(iohome::is_acei_valid(frame.data[1]));
    TEST_ASSERT_EQUAL_UINT8(0xD2, frame.data[2]);   // Main parameter MSB
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.data[3]);   // Main parameter LSB

    // Sequence number is transmitted before the MAC.
    TEST_ASSERT_EQUAL_UINT8(0x3B, frame.rolling_code[0]);
    TEST_ASSERT_EQUAL_UINT8(0xD2, frame.rolling_code[1]);

    const uint8_t expected_mac[] = {0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mac, frame.hmac, 6);

    TEST_ASSERT_EQUAL_UINT8(0xA9, frame.crc[0]);
    TEST_ASSERT_EQUAL_UINT8(0x37, frame.crc[1]);
}

void test_reserialize_documented_1w_execute(void) {
    const uint8_t capture[] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
                               0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
                               0x3B, 0xD2,
                               0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8,
                               0xA9, 0x37};

    IoFrame frame;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &frame));

    uint8_t out[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&frame, out, sizeof(out));

    TEST_ASSERT_EQUAL_UINT(sizeof(capture), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(capture, out, sizeof(capture));
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void test_init_frame_1w_sets_mode_bit(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    TEST_ASSERT_TRUE(frame.is_1w_mode);
    TEST_ASSERT_FALSE(frame.authenticated);
    TEST_ASSERT_EQUAL_UINT8(0, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.ctrl_byte_1);
    // Bit 5 set means 1W on the wire.
    TEST_ASSERT_TRUE((frame.ctrl_byte_0 & iohome::CTRL0_ONE_WAY_MASK) != 0);
}

void test_init_frame_2w_clears_mode_bit(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, false);

    TEST_ASSERT_FALSE(frame.is_1w_mode);
    TEST_ASSERT_TRUE((frame.ctrl_byte_0 & iohome::CTRL0_ONE_WAY_MASK) == 0);
}

void test_init_frame_nullptr(void) {
    iohome::frame::init_frame(nullptr, true);  // must not crash
}

void test_set_destination(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    const uint8_t dest[3] = {0xAA, 0xBB, 0xCC};
    iohome::frame::set_destination(&frame, dest);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(dest, frame.dest_node, 3);
}

void test_set_source(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    const uint8_t src[3] = {0x11, 0x22, 0x33};
    iohome::frame::set_source(&frame, src);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, frame.src_node, 3);
}

void test_setters_reject_nullptr(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    const uint8_t id[3] = {1, 2, 3};

    // None of these may dereference a null pointer.
    iohome::frame::set_destination(nullptr, id);
    iohome::frame::set_source(nullptr, id);
    iohome::frame::set_destination(&frame, nullptr);
    iohome::frame::set_source(&frame, nullptr);
    iohome::frame::set_rolling_code(nullptr, 0x1234);
    iohome::frame::set_order(nullptr, iohome::FrameOrder::SINGLE);

    TEST_ASSERT_FALSE(iohome::frame::set_command(nullptr, 0x00, nullptr, 0));
    TEST_ASSERT_EQUAL_UINT16(0, iohome::frame::get_rolling_code(nullptr));
}

void test_set_command_updates_length(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    const uint8_t params[] = {0x01, 0x02};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_EXECUTE, params, 2));

    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_EXECUTE, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(2, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, frame.data[1]);

    // header(9) + data(2) + crc(2), no auth trailer yet
    TEST_ASSERT_EQUAL_UINT8(13, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT8(13, iohome::frame::get_frame_length(frame.ctrl_byte_0));
}

void test_set_command_rejects_null_params_with_length(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    TEST_ASSERT_FALSE(iohome::frame::set_command(&frame, 0x00, nullptr, 4));
}

void test_set_command_too_large(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t params[22] = {0};
    TEST_ASSERT_FALSE(iohome::frame::set_command(&frame, 0x00, params, 22));
}

void test_set_command_rejects_unencodable_length(void) {
    // A 1W authenticated frame with 21 payload bytes would be 41 bytes, which
    // does not fit into the 5-bit size field. The frame must stay usable and
    // the failure must be reported, not silently produce a truncated frame.
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    uint8_t params[21] = {0};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, 0x00, params, 21));
    TEST_ASSERT_EQUAL_UINT8(32, frame.frame_length);   // 9 + 21 + 2, still plain

    // Adding the 1W authentication trailer pushes it over the limit.
    const uint8_t key[16] = {0};
    TEST_ASSERT_FALSE(iohome::frame::finalize_frame(&frame, key));

    // The rejected update must not have corrupted the frame.
    TEST_ASSERT_EQUAL_UINT8(32, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT8(21, frame.data_len);
}

void test_set_execute_command(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&frame, iohome::MP_STOP));

    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_EXECUTE, frame.command_id);
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_SIZE, frame.data_len);
    TEST_ASSERT_EQUAL_UINT8(0x01, frame.data[0]);           // Originator::USER
    TEST_ASSERT_EQUAL_HEX8(0x61, frame.data[1]);            // ACEI_DEFAULT
    TEST_ASSERT_EQUAL_UINT8(0xD2, frame.data[2]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.data[3]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.data[4]);
    TEST_ASSERT_EQUAL_UINT8(0x00, frame.data[5]);
}

void test_set_execute_command_rejects_invalid_acei(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    // docs/commands.md: "LSB must be 1: The frame is not considered valid if
    // this bit is set to 0".
    TEST_ASSERT_FALSE(iohome::frame::set_execute_command(
        &frame, iohome::MP_OPEN, iohome::Originator::USER, 0x60));
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(
        &frame, iohome::MP_OPEN, iohome::Originator::USER, 0x61));
}

void test_rolling_code_is_lsb_first(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    iohome::frame::set_rolling_code(&frame, 0x1234);

    TEST_ASSERT_EQUAL_UINT8(0x34, frame.rolling_code[0]);  // LSB
    TEST_ASSERT_EQUAL_UINT8(0x12, frame.rolling_code[1]);  // MSB
    TEST_ASSERT_EQUAL_UINT16(0x1234, iohome::frame::get_rolling_code(&frame));
}

void test_is_broadcast(void) {
    const uint8_t documented_broadcast[] = {0x00, 0x00, 0x3F};
    const uint8_t all_ones[] = {0xFF, 0xFF, 0xFF};
    const uint8_t group[] = {0x00, 0x00, 0x00};
    const uint8_t unicast[] = {0x1A, 0x38, 0x0B};

    TEST_ASSERT_TRUE(iohome::frame::is_broadcast(documented_broadcast));
    TEST_ASSERT_TRUE(iohome::frame::is_broadcast(all_ones));
    TEST_ASSERT_TRUE(iohome::frame::is_broadcast(group));
    TEST_ASSERT_FALSE(iohome::frame::is_broadcast(unicast));
    TEST_ASSERT_FALSE(iohome::frame::is_broadcast(nullptr));
}

// ---------------------------------------------------------------------------
// Serialization / parsing
// ---------------------------------------------------------------------------

void test_serialize_plain_frame(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    const uint8_t dest[] = {0xAA, 0xBB, 0xCC};
    const uint8_t src[] = {0x11, 0x22, 0x33};
    iohome::frame::set_destination(&frame, dest);
    iohome::frame::set_source(&frame, src);

    const uint8_t params[] = {0x64, 0x00};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_EXECUTE, params, 2));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    TEST_ASSERT_EQUAL_UINT(frame.frame_length, len);
    TEST_ASSERT_EQUAL_UINT8(frame.ctrl_byte_0, buffer[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, buffer[2]);
    TEST_ASSERT_EQUAL_UINT8(0x11, buffer[5]);
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_EXECUTE, buffer[8]);
}

void test_serialize_rejects_small_buffer(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    uint8_t buffer[4];
    TEST_ASSERT_EQUAL_UINT(0, iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer)));
}

void test_serialize_nullptr(void) {
    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    TEST_ASSERT_EQUAL_UINT(0, iohome::frame::serialize_frame(nullptr, buffer, sizeof(buffer)));

    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_EQUAL_UINT(0, iohome::frame::serialize_frame(&frame, nullptr, sizeof(buffer)));
}

void test_parse_frame_too_short(void) {
    const uint8_t buffer[5] = {0};
    IoFrame frame;

    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, 5, &frame));
}

void test_parse_frame_rejects_truncated_buffer(void) {
    // Control byte claims 25 bytes but only 12 are present.
    uint8_t buffer[12] = {0xF6, 0x00};
    IoFrame frame;

    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, sizeof(buffer), &frame));
}

void test_parse_frame_rejects_undersized_length_field(void) {
    // Size field of 0 => 3 byte frame, shorter than the mandatory header.
    uint8_t buffer[iohome::FRAME_MIN_SIZE] = {0x20, 0x00};
    IoFrame frame;

    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, sizeof(buffer), &frame));
}

void test_parse_frame_nullptr(void) {
    IoFrame frame;
    TEST_ASSERT_FALSE(iohome::frame::parse_frame(nullptr, 10, &frame));

    const uint8_t buffer[iohome::FRAME_MAX_SIZE] = {0};
    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, sizeof(buffer), nullptr));
}

void test_parse_trailer_policy_none(void) {
    const uint8_t capture[] = {0xF6, 0x00, 0x00, 0x00, 0x3F, 0x70, 0x87, 0x58, 0x00,
                               0x01, 0x61, 0xD2, 0x00, 0x00, 0x00,
                               0x3B, 0xD2,
                               0xE6, 0xB6, 0x2C, 0xEF, 0x54, 0xC8,
                               0xA9, 0x37};

    IoFrame frame;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &frame,
                                                AuthTrailer::NONE));

    TEST_ASSERT_FALSE(frame.authenticated);
    // Forcing NONE folds the trailer back into the payload: 6 + 2 + 6 = 14.
    TEST_ASSERT_EQUAL_UINT8(14, frame.data_len);
}

void test_parse_trailer_policy_present_rejects_short_payload(void) {
    // 2W frame with a 2-byte payload cannot hold a 6-byte MAC.
    IoFrame source;
    iohome::frame::init_frame(&source, false);
    const uint8_t params[] = {0x01, 0x02};
    TEST_ASSERT_TRUE(iohome::frame::set_command(&source, 0x28, params, 2));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&source));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&source, buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, len);

    IoFrame parsed;
    TEST_ASSERT_FALSE(iohome::frame::parse_frame(buffer, len, &parsed, AuthTrailer::PRESENT));
}

void test_roundtrip_authenticated_1w(void) {
    const uint8_t key[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                             0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};

    IoFrame original;
    iohome::frame::init_frame(&original, true);

    const uint8_t dest[] = {0xAA, 0xBB, 0xCC};
    const uint8_t src[] = {0x11, 0x22, 0x33};
    iohome::frame::set_destination(&original, dest);
    iohome::frame::set_source(&original, src);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&original, iohome::MP_CLOSE));
    iohome::frame::set_rolling_code(&original, 0xABCD);

    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&original, key));
    TEST_ASSERT_TRUE(original.authenticated);

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&original, buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, len);

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));

    TEST_ASSERT_TRUE(parsed.is_1w_mode);
    TEST_ASSERT_TRUE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(original.command_id, parsed.command_id);
    TEST_ASSERT_EQUAL_UINT8(original.data_len, parsed.data_len);
    TEST_ASSERT_EQUAL_UINT16(0xABCD, iohome::frame::get_rolling_code(&parsed));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(original.hmac, parsed.hmac, 6);

    // CRC and MAC both check out.
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed, key));
}

void test_roundtrip_authenticated_2w(void) {
    const uint8_t key[16] = {0xAA};
    const uint8_t challenge[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};

    IoFrame original;
    iohome::frame::init_frame(&original, false);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&original, iohome::MP_OPEN));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&original, key, challenge));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&original, buffer, sizeof(buffer));
    TEST_ASSERT_GREATER_THAN(0, len);

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed, AuthTrailer::PRESENT));

    TEST_ASSERT_FALSE(parsed.is_1w_mode);
    TEST_ASSERT_TRUE(parsed.authenticated);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed, key, challenge));
}

void test_finalize_2w_requires_challenge(void) {
    const uint8_t key[16] = {0};

    IoFrame frame;
    iohome::frame::init_frame(&frame, false);
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_EXECUTE, nullptr, 0));

    TEST_ASSERT_FALSE(iohome::frame::finalize_frame(&frame, key, nullptr));
}

void test_validate_2w_without_challenge_is_rejected(void) {
    // Regression: validating an authenticated 2W frame without a challenge used
    // to dereference a null pointer inside the IV construction.
    const uint8_t key[16] = {0};
    const uint8_t challenge[6] = {1, 2, 3, 4, 5, 6};

    IoFrame frame;
    iohome::frame::init_frame(&frame, false);
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_EXECUTE, nullptr, 0));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&frame, key, challenge));

    TEST_ASSERT_FALSE(iohome::frame::validate_frame(&frame, key, nullptr));
}

void test_validate_detects_tampering(void) {
    const uint8_t key[16] = {0x0F};

    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&frame, iohome::MP_CLOSE));
    iohome::frame::set_rolling_code(&frame, 7);
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&frame, key));
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&frame, key));

    // Flipping a payload byte invalidates the CRC.
    frame.data[3] ^= 0x01;
    TEST_ASSERT_FALSE(iohome::frame::validate_frame(&frame, key));
}

void test_validate_detects_mac_forgery(void) {
    const uint8_t key[16] = {0x0F};
    const uint8_t wrong_key[16] = {0xF0};

    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&frame, iohome::MP_CLOSE));
    iohome::frame::set_rolling_code(&frame, 7);
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&frame, key));

    // CRC still fine, MAC computed under a different key must be rejected.
    TEST_ASSERT_FALSE(iohome::frame::validate_frame(&frame, wrong_key));
}

void test_max_payload_frame_roundtrips(void) {
    // Largest plain frame: the documented 21-byte payload maximum.
    IoFrame frame;
    iohome::frame::init_frame(&frame, false);

    uint8_t params[iohome::FRAME_MAX_DATA_SIZE];
    memset(params, 0x5A, sizeof(params));
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, 0x20, params, sizeof(params)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    // 9 header + 21 data + 2 CRC, comfortably inside the 34-byte encodable max.
    TEST_ASSERT_EQUAL_UINT8(32, frame.frame_length);
    TEST_ASSERT_LESS_OR_EQUAL(iohome::FRAME_MAX_SIZE, frame.frame_length);

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_UINT(32, len);

    // Command 0x20 has no documented parameter length, so AUTO cannot tell a
    // plain frame from an authenticated one and errs towards assuming a MAC -
    // which fails verification rather than passing unauthenticated data off as
    // authenticated. Tell the parser what this frame is.
    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed, AuthTrailer::NONE));
    TEST_ASSERT_EQUAL_UINT8(sizeof(params), parsed.data_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(params, parsed.data, sizeof(params));
}

void test_auto_trailer_uses_command_metadata(void) {
    // The wire format carries no "authenticated" flag, so parse_frame has to
    // deduce it. These are the cases that matter.
    IoFrame frame;

    // Fixed-length command, payload exactly the parameter length => plain.
    // This is the 2W execute frame from docs/commands.md: "CMD 0 DATA(6)".
    iohome::frame::init_frame(&frame, false);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&frame, iohome::MP_CLOSE));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));
    TEST_ASSERT_FALSE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_SIZE, parsed.data_len);

    // Same command, payload = parameters + MAC => authenticated. A 2W frame
    // carries a MAC too, so keying only on the protocol mode gets this wrong.
    const uint8_t key[16] = {0x5A};
    const uint8_t challenge[6] = {1, 2, 3, 4, 5, 6};
    iohome::frame::init_frame(&frame, false);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&frame, iohome::MP_CLOSE));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame(&frame, key, challenge));
    len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));
    TEST_ASSERT_TRUE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_SIZE, parsed.data_len);
    TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed, key, challenge));

    // A bootstrap command never carries a MAC, however long its payload is.
    iohome::frame::init_frame(&frame, true);
    uint8_t key_payload[iohome::AES_KEY_SIZE + 4];
    memset(key_payload, 0x11, sizeof(key_payload));
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_SEND_1W_KEY, key_payload,
                                                sizeof(key_payload)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));
    len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));
    TEST_ASSERT_FALSE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(sizeof(key_payload), parsed.data_len);
}

void test_command_metadata(void) {
    TEST_ASSERT_TRUE(iohome::frame::is_unauthenticated_command(iohome::CMD_DISCOVER));
    TEST_ASSERT_TRUE(iohome::frame::is_unauthenticated_command(iohome::CMD_SEND_1W_KEY));
    TEST_ASSERT_TRUE(iohome::frame::is_unauthenticated_command(iohome::CMD_KEY_TRANSFER));
    TEST_ASSERT_FALSE(iohome::frame::is_unauthenticated_command(iohome::CMD_EXECUTE));
    TEST_ASSERT_FALSE(iohome::frame::is_unauthenticated_command(iohome::CMD_CHALLENGE_RESPONSE));

    TEST_ASSERT_EQUAL_INT(6, iohome::frame::expected_payload_size(iohome::CMD_EXECUTE));
    TEST_ASSERT_EQUAL_INT(6, iohome::frame::expected_payload_size(iohome::CMD_CHALLENGE_REQUEST));
    TEST_ASSERT_EQUAL_INT(20, iohome::frame::expected_payload_size(iohome::CMD_SEND_1W_KEY));
    TEST_ASSERT_EQUAL_INT(16, iohome::frame::expected_payload_size(iohome::CMD_KEY_TRANSFER));
    TEST_ASSERT_EQUAL_INT(0, iohome::frame::expected_payload_size(iohome::CMD_DISCOVER));
    TEST_ASSERT_EQUAL_INT(-1, iohome::frame::expected_payload_size(0x20));
}

void test_key_transfer_sized_frames_fit(void) {
    // Regression: with the old FRAME_MAX_SIZE of 32 and the wrong size-field
    // formula, both pairing frames failed to serialize, so pairing could never
    // work. A 1W key transfer is 9 + 20 + 2 = 31 bytes, a 2W one 9 + 16 + 2 = 27.
    uint8_t payload_1w[iohome::AES_KEY_SIZE + 4];
    memset(payload_1w, 0xA5, sizeof(payload_1w));

    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_SEND_1W_KEY,
                                                payload_1w, sizeof(payload_1w)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));
    TEST_ASSERT_EQUAL_UINT8(31, frame.frame_length);

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    TEST_ASSERT_EQUAL_UINT(31, iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer)));

    uint8_t payload_2w[iohome::AES_KEY_SIZE];
    memset(payload_2w, 0x5A, sizeof(payload_2w));

    iohome::frame::init_frame(&frame, false);
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_KEY_TRANSFER,
                                                payload_2w, sizeof(payload_2w)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));
    TEST_ASSERT_EQUAL_UINT8(27, frame.frame_length);
    TEST_ASSERT_EQUAL_UINT(27, iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer)));
}

void test_parse_never_overflows_for_any_control_byte(void) {
    // Fuzz-ish sweep: no combination of control bytes may make the parser read
    // past the buffer or report a frame it cannot re-serialize.
    for (int c0 = 0; c0 < 256; c0++) {
        for (int c1 = 0; c1 < 256; c1 += 37) {
            uint8_t buffer[iohome::FRAME_MAX_SIZE];
            memset(buffer, static_cast<uint8_t>(c1), sizeof(buffer));
            buffer[0] = static_cast<uint8_t>(c0);
            buffer[1] = static_cast<uint8_t>(c1);

            IoFrame frame;
            if (iohome::frame::parse_frame(buffer, sizeof(buffer), &frame)) {
                TEST_ASSERT_LESS_OR_EQUAL(iohome::FRAME_MAX_SIZE, frame.frame_length);
                TEST_ASSERT_GREATER_OR_EQUAL(iohome::FRAME_MIN_SIZE, frame.frame_length);
                TEST_ASSERT_LESS_OR_EQUAL(iohome::FRAME_MAX_DATA_SIZE, frame.data_len);

                uint8_t out[iohome::FRAME_MAX_SIZE];
                TEST_ASSERT_EQUAL_UINT(frame.frame_length,
                                       iohome::frame::serialize_frame(&frame, out, sizeof(out)));
            }
        }
    }
}

void test_print_frame_handles_nullptr(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    iohome::frame::print_frame(nullptr, nullptr);
    iohome::frame::print_frame(&frame, nullptr);
}

int main(int, char **) {
    UNITY_BEGIN();

    RUN_TEST(test_ctrl0_one_way_bit);
    RUN_TEST(test_get_frame_length_matches_spec);
    RUN_TEST(test_set_frame_length_roundtrip);
    RUN_TEST(test_order_field);

    RUN_TEST(test_parse_documented_2w_discover);
    RUN_TEST(test_parse_documented_2w_discover_answer);
    RUN_TEST(test_parse_documented_1w_execute);
    RUN_TEST(test_reserialize_documented_1w_execute);

    RUN_TEST(test_init_frame_1w_sets_mode_bit);
    RUN_TEST(test_init_frame_2w_clears_mode_bit);
    RUN_TEST(test_init_frame_nullptr);
    RUN_TEST(test_set_destination);
    RUN_TEST(test_set_source);
    RUN_TEST(test_setters_reject_nullptr);
    RUN_TEST(test_set_command_updates_length);
    RUN_TEST(test_set_command_rejects_null_params_with_length);
    RUN_TEST(test_set_command_too_large);
    RUN_TEST(test_set_command_rejects_unencodable_length);
    RUN_TEST(test_set_execute_command);
    RUN_TEST(test_set_execute_command_rejects_invalid_acei);
    RUN_TEST(test_rolling_code_is_lsb_first);
    RUN_TEST(test_is_broadcast);

    RUN_TEST(test_serialize_plain_frame);
    RUN_TEST(test_serialize_rejects_small_buffer);
    RUN_TEST(test_serialize_nullptr);
    RUN_TEST(test_parse_frame_too_short);
    RUN_TEST(test_parse_frame_rejects_truncated_buffer);
    RUN_TEST(test_parse_frame_rejects_undersized_length_field);
    RUN_TEST(test_parse_frame_nullptr);
    RUN_TEST(test_parse_trailer_policy_none);
    RUN_TEST(test_parse_trailer_policy_present_rejects_short_payload);

    RUN_TEST(test_roundtrip_authenticated_1w);
    RUN_TEST(test_roundtrip_authenticated_2w);
    RUN_TEST(test_finalize_2w_requires_challenge);
    RUN_TEST(test_validate_2w_without_challenge_is_rejected);
    RUN_TEST(test_validate_detects_tampering);
    RUN_TEST(test_validate_detects_mac_forgery);
    RUN_TEST(test_max_payload_frame_roundtrips);
    RUN_TEST(test_auto_trailer_uses_command_metadata);
    RUN_TEST(test_command_metadata);
    RUN_TEST(test_key_transfer_sized_frames_fit);
    RUN_TEST(test_parse_never_overflows_for_any_control_byte);
    RUN_TEST(test_print_frame_handles_nullptr);

    return UNITY_END();
}
