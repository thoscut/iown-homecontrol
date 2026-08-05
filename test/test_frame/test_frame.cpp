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
#include "protocol/iohome_crypto.h"
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
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, frame.data_len);
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
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, frame.data_len);
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
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, parsed.data_len);

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
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, parsed.data_len);
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

    // Execute has no fixed length: captures show both 6 and 8 payload bytes.
    // Reporting an exact 6 made the 8-byte form miss the parser's precise test.
    TEST_ASSERT_EQUAL_INT(-1, iohome::frame::expected_payload_size(iohome::CMD_EXECUTE));
    TEST_ASSERT_EQUAL_INT(-1, iohome::frame::expected_payload_size(iohome::CMD_ACTIVATE_MODE));
    TEST_ASSERT_EQUAL_INT(6, iohome::frame::min_payload_size(iohome::CMD_EXECUTE));
    TEST_ASSERT_EQUAL_INT(6, iohome::frame::min_payload_size(iohome::CMD_ACTIVATE_MODE));

    TEST_ASSERT_EQUAL_INT(6, iohome::frame::expected_payload_size(iohome::CMD_CHALLENGE_REQUEST));
    TEST_ASSERT_EQUAL_INT(20, iohome::frame::expected_payload_size(iohome::CMD_SEND_1W_KEY));
    TEST_ASSERT_EQUAL_INT(16, iohome::frame::expected_payload_size(iohome::CMD_KEY_TRANSFER));
    TEST_ASSERT_EQUAL_INT(0, iohome::frame::expected_payload_size(iohome::CMD_DISCOVER));
    TEST_ASSERT_EQUAL_INT(-1, iohome::frame::expected_payload_size(0x20));

    // For fixed-length commands the minimum is the exact length.
    TEST_ASSERT_EQUAL_INT(6, iohome::frame::min_payload_size(iohome::CMD_CHALLENGE_REQUEST));
    TEST_ASSERT_EQUAL_INT(0, iohome::frame::min_payload_size(iohome::CMD_DISCOVER));
    TEST_ASSERT_EQUAL_INT(-1, iohome::frame::min_payload_size(0x20));
}

void test_command_ids_match_the_documented_table(void) {
    // Two independent sources agree on these: the "Command IDs" section of
    // docs/commands.md and the enum in scripts/io-homecontrol.ksy. Fabricated
    // IDs are the easiest way to build frames nothing answers - this library
    // once carried 0x60-0x63, which exist nowhere - so the table is pinned.
    TEST_ASSERT_EQUAL_HEX8(0x00, iohome::CMD_EXECUTE);
    TEST_ASSERT_EQUAL_HEX8(0x01, iohome::CMD_ACTIVATE_MODE);
    TEST_ASSERT_EQUAL_HEX8(0x02, iohome::CMD_MANUAL_ORDER);
    TEST_ASSERT_EQUAL_HEX8(0x04, iohome::CMD_PRIVATE_ANSWER);

    TEST_ASSERT_EQUAL_HEX8(0x28, iohome::CMD_DISCOVER);
    TEST_ASSERT_EQUAL_HEX8(0x29, iohome::CMD_DISCOVER_ANSWER);
    TEST_ASSERT_EQUAL_HEX8(0x2A, iohome::CMD_DISCOVER_REMOTE);
    TEST_ASSERT_EQUAL_HEX8(0x2B, iohome::CMD_DISCOVER_REMOTE_ANSWER);
    TEST_ASSERT_EQUAL_HEX8(0x2C, iohome::CMD_DISCOVER_CONFIRM);
    TEST_ASSERT_EQUAL_HEX8(0x2D, iohome::CMD_DISCOVER_CONFIRM_ACK);

    TEST_ASSERT_EQUAL_HEX8(0x30, iohome::CMD_SEND_1W_KEY);
    TEST_ASSERT_EQUAL_HEX8(0x31, iohome::CMD_ASK_CHALLENGE);
    TEST_ASSERT_EQUAL_HEX8(0x32, iohome::CMD_KEY_TRANSFER);
    TEST_ASSERT_EQUAL_HEX8(0x33, iohome::CMD_KEY_TRANSFER_ACK);
    TEST_ASSERT_EQUAL_HEX8(0x36, iohome::CMD_ADDRESS_REQUEST);
    TEST_ASSERT_EQUAL_HEX8(0x37, iohome::CMD_ADDRESS_ANSWER);
    TEST_ASSERT_EQUAL_HEX8(0x38, iohome::CMD_LAUNCH_KEY_TRANSFER);
    TEST_ASSERT_EQUAL_HEX8(0x39, iohome::CMD_REMOVE_1W_CONTROLLER);

    TEST_ASSERT_EQUAL_HEX8(0x3C, iohome::CMD_CHALLENGE_REQUEST);
    TEST_ASSERT_EQUAL_HEX8(0x3D, iohome::CMD_CHALLENGE_RESPONSE);

    TEST_ASSERT_EQUAL_HEX8(0x46, iohome::CMD_SCRIPT_UPLOAD);
    TEST_ASSERT_EQUAL_HEX8(0x47, iohome::CMD_DOWNLOAD_CONFIG);
    TEST_ASSERT_EQUAL_HEX8(0x4A, iohome::CMD_RENAME_FILE);

    TEST_ASSERT_EQUAL_HEX8(0x50, iohome::CMD_GET_NAME);
    TEST_ASSERT_EQUAL_HEX8(0x51, iohome::CMD_GET_NAME_ANSWER);
    TEST_ASSERT_EQUAL_HEX8(0x52, iohome::CMD_WRITE_NAME);
    TEST_ASSERT_EQUAL_HEX8(0x53, iohome::CMD_WRITE_NAME_ACK);
    TEST_ASSERT_EQUAL_HEX8(0x54, iohome::CMD_GET_INFO_1);
    TEST_ASSERT_EQUAL_HEX8(0x55, iohome::CMD_INFO_1_ANSWER);
    TEST_ASSERT_EQUAL_HEX8(0x56, iohome::CMD_GET_INFO_2);
    TEST_ASSERT_EQUAL_HEX8(0x57, iohome::CMD_INFO_2_ANSWER);

    TEST_ASSERT_EQUAL_HEX8(0xE0, iohome::CMD_BOOTLOADER_START);
    TEST_ASSERT_EQUAL_HEX8(0xE1, iohome::CMD_BOOTLOADER_DATA);

    // Reboot is 0xF2. The old CMD_SERVICE_RESET named a reset and addressed
    // 0xF1, which is "read groups" / service ACK.
    TEST_ASSERT_EQUAL_HEX8(0xF0, iohome::CMD_SEND_RAW_MESSAGE);
    TEST_ASSERT_EQUAL_HEX8(0xF1, iohome::CMD_READ_GROUPS);
    TEST_ASSERT_EQUAL_HEX8(0xF2, iohome::CMD_REBOOT);
    TEST_ASSERT_EQUAL_HEX8(0xF3, iohome::CMD_SERVICE_STATUS_ACK);
}

void test_parameter_tables_match_the_documentation(void) {
    // docs/commands.md "Standard Values".
    TEST_ASSERT_EQUAL_HEX16(0x0000, iohome::MP_MIN);
    TEST_ASSERT_EQUAL_HEX16(0x0001, iohome::MP_1W_BUTTON_DOWN);
    TEST_ASSERT_EQUAL_HEX16(0x0002, iohome::MP_1W_BUTTON_STOP);
    TEST_ASSERT_EQUAL_HEX16(0x0003, iohome::MP_1W_BUTTON_PROG);
    TEST_ASSERT_EQUAL_HEX16(0x00FE, iohome::MP_BUTTON_RELEASED);
    TEST_ASSERT_EQUAL_HEX16(0x00FF, iohome::MP_BUTTON_STOP);
    TEST_ASSERT_EQUAL_HEX16(0xC800, iohome::MP_MAX);
    TEST_ASSERT_EQUAL_HEX16(0xD100, iohome::MP_TARGET);
    TEST_ASSERT_EQUAL_HEX16(0xD200, iohome::MP_CURRENT);
    TEST_ASSERT_EQUAL_HEX16(0xD300, iohome::MP_DEFAULT);
    TEST_ASSERT_EQUAL_HEX16(0xD400, iohome::MP_IGNORE);
    TEST_ASSERT_EQUAL_HEX16(0x6E00, iohome::MP_RUNNING);
    TEST_ASSERT_EQUAL_HEX16(0x7D00, iohome::MP_PLUS_MINUS_DEFAULT);
    TEST_ASSERT_EQUAL_HEX16(0xE000, iohome::MP_RETRY);
    TEST_ASSERT_EQUAL_HEX16(0xF7FF, iohome::MP_UNKNOWN_FEEDBACK);
    TEST_ASSERT_EQUAL_HEX16(0xC900, iohome::MP_SIGNED_PERCENT_MIN);
    TEST_ASSERT_EQUAL_HEX16(0xD0D0, iohome::MP_SIGNED_PERCENT_MAX);

    // "Open" is the minimum and "closed" the maximum: the wire value counts
    // closure, so the aliases must not be the other way round.
    TEST_ASSERT_EQUAL_HEX16(iohome::MP_MIN, iohome::MP_OPEN);
    TEST_ASSERT_EQUAL_HEX16(iohome::MP_MAX, iohome::MP_CLOSE);
    TEST_ASSERT_EQUAL_HEX16(iohome::MP_CURRENT, iohome::MP_STOP);

    // docs/commands.md "Command Originator".
    TEST_ASSERT_EQUAL_HEX8(0x00, static_cast<uint8_t>(iohome::Originator::LOCAL_USER));
    TEST_ASSERT_EQUAL_HEX8(0x01, static_cast<uint8_t>(iohome::Originator::USER));
    TEST_ASSERT_EQUAL_HEX8(0x02, static_cast<uint8_t>(iohome::Originator::SENSOR_RAIN));
    TEST_ASSERT_EQUAL_HEX8(0x08, static_cast<uint8_t>(iohome::Originator::SAAC));
    TEST_ASSERT_EQUAL_HEX8(0x09, static_cast<uint8_t>(iohome::Originator::SENSOR_WIND));
    TEST_ASSERT_EQUAL_HEX8(0x10, static_cast<uint8_t>(iohome::Originator::MYSELF));
    TEST_ASSERT_EQUAL_HEX8(0xFE, static_cast<uint8_t>(iohome::Originator::AUTOMATIC_CYCLE));
    TEST_ASSERT_EQUAL_HEX8(0xFF, static_cast<uint8_t>(iohome::Originator::EMERGENCY));

    // ACEI field layout: A(7-5) C(4-3) E(2-1) I(0).
    TEST_ASSERT_EQUAL_HEX8(0xE0, iohome::ACEI_LEVEL_MASK);
    TEST_ASSERT_EQUAL_HEX8(0x18, iohome::ACEI_SERVICE_MASK);
    TEST_ASSERT_EQUAL_HEX8(0x06, iohome::ACEI_EXTENDED_INFO_MASK);
    TEST_ASSERT_EQUAL_HEX8(0x01, iohome::ACEI_VALID_MASK);

    // Priority level 3 ("Level 2 - Default: Default for Remote Controllers")
    // with the IsValid bit gives 0x61 - the ACEI in both captured frames.
    TEST_ASSERT_EQUAL_HEX8(0x61, iohome::ACEI_DEFAULT);
    TEST_ASSERT_EQUAL_HEX8(0x61, iohome::make_acei(iohome::PriorityLevel::USER_LEVEL_2));
    TEST_ASSERT_EQUAL_HEX8(0x01, iohome::make_acei(iohome::PriorityLevel::HUMAN_PROTECTION));
    TEST_ASSERT_EQUAL_HEX8(0xE1, iohome::make_acei(iohome::PriorityLevel::COMFORT_LEVEL_4));

    // The IsValid bit is set whatever the caller asks for.
    for (uint8_t level = 0; level < 8; level++) {
        const uint8_t acei = iohome::make_acei(static_cast<iohome::PriorityLevel>(level), 3, 3);
        TEST_ASSERT_TRUE(iohome::is_acei_valid(acei));
        TEST_ASSERT_EQUAL_HEX8(level, (acei & iohome::ACEI_LEVEL_MASK) >> iohome::ACEI_LEVEL_SHIFT);
    }
}

void test_ksy_capture_frame(void) {
    // The example frame from scripts/io-homecontrol.ksy, SFD stripped. It comes
    // from a different capture than docs/linklayer.md, so it is an independent
    // check on the size-field bias, the CRC and the payload layout - all three
    // of which the original code had wrong.
    //
    //  f8 00 | 00 00 7f | 70 87 58 | 00 | 01 61 d4 00 80 c8 00 00 | 3b d5
    //   ctrl   target     source    cmd   8 payload bytes           seq
    //  | 05 52 68 75 49 9c | 7e 72
    //    MAC                 CRC-16/KERMIT, LSB first
    static const uint8_t capture[] = {
        0xf8, 0x00,
        0x00, 0x00, 0x7f,
        0x70, 0x87, 0x58,
        0x00,
        0x01, 0x61, 0xd4, 0x00, 0x80, 0xc8, 0x00, 0x00,
        0x3b, 0xd5,
        0x05, 0x52, 0x68, 0x75, 0x49, 0x9c,
        0x7e, 0x72
    };

    // Size field 24 + 3 = 27 bytes. With the original bias of +11 this frame
    // would have been read as 35 bytes, past the end of the buffer.
    TEST_ASSERT_EQUAL_UINT(27, sizeof(capture));
    TEST_ASSERT_EQUAL_UINT8(24, capture[0] & 0x1F);

    // Our CRC reproduces the captured checksum exactly, LSB first.
    TEST_ASSERT_TRUE(iohome::crypto::verify_crc16(capture, sizeof(capture)));

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(capture, sizeof(capture), &parsed));

    TEST_ASSERT_EQUAL_UINT8(27, parsed.frame_length);
    TEST_ASSERT_TRUE(parsed.is_1w_mode);       // ctrl0 bit 5 set
    TEST_ASSERT_EQUAL_UINT8(iohome::CMD_EXECUTE, parsed.command_id);

    const uint8_t expected_src[] = {0x70, 0x87, 0x58};
    const uint8_t expected_dst[] = {0x00, 0x00, 0x7f};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_src, parsed.src_node, 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_dst, parsed.dest_node, 3);

    // Eight payload bytes: the six-byte minimum plus two extra functional
    // parameters. The trailer (2-byte sequence + 6-byte MAC) is split off.
    TEST_ASSERT_TRUE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(8, parsed.data_len);

    const uint8_t expected_payload[] = {0x01, 0x61, 0xd4, 0x00, 0x80, 0xc8, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_payload, parsed.data, sizeof(expected_payload));

    // ACEI "IsValid" is set, as it must be for an actuator to act on the frame.
    TEST_ASSERT_TRUE(iohome::is_acei_valid(parsed.data[1]));

    // Rolling code is stored LSB first, so 3b d5 on air reads back as 0xd53b.
    TEST_ASSERT_EQUAL_UINT16(0xd53b, iohome::frame::get_rolling_code(&parsed));

    const uint8_t expected_mac[] = {0x05, 0x52, 0x68, 0x75, 0x49, 0x9c};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected_mac, parsed.hmac, sizeof(expected_mac));

    // Round-trip: re-serializing the parsed frame reproduces the capture byte
    // for byte, CRC included.
    uint8_t out[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&parsed, out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(sizeof(capture), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(capture, out, sizeof(capture));
}

void test_execute_command_with_extra_functional_params(void) {
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);

    // Four functional parameters, as in the .ksy capture.
    const uint8_t fps[4] = {0x80, 0xc8, 0x00, 0x00};
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command_fp(
        &frame, 0xd400, iohome::Originator::USER, 0x61, fps, sizeof(fps)));

    TEST_ASSERT_EQUAL_UINT8(8, frame.data_len);
    const uint8_t expected[] = {0x01, 0x61, 0xd4, 0x00, 0x80, 0xc8, 0x00, 0x00};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame.data, sizeof(expected));

    // The two-parameter convenience wrapper produces the same bytes it always did.
    IoFrame plain_two;
    iohome::frame::init_frame(&plain_two, true);
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command(&plain_two, iohome::MP_CLOSE));
    TEST_ASSERT_EQUAL_UINT8(iohome::EXECUTE_PAYLOAD_MIN_SIZE, plain_two.data_len);

    // Rejections: fewer than FP1+FP2, more than the protocol allows, null array,
    // and an ACEI whose IsValid bit is clear.
    const uint8_t one_fp[1] = {0x00};
    TEST_ASSERT_FALSE(iohome::frame::set_execute_command_fp(
        &frame, 0, iohome::Originator::USER, 0x61, one_fp, 1));

    uint8_t too_many[iohome::EXECUTE_MAX_FUNCTIONAL_PARAMS + 1] = {0};
    TEST_ASSERT_FALSE(iohome::frame::set_execute_command_fp(
        &frame, 0, iohome::Originator::USER, 0x61, too_many, sizeof(too_many)));

    TEST_ASSERT_FALSE(iohome::frame::set_execute_command_fp(
        &frame, 0, iohome::Originator::USER, 0x61, nullptr, 2));

    TEST_ASSERT_FALSE(iohome::frame::set_execute_command_fp(
        &frame, 0, iohome::Originator::USER, 0x60, fps, sizeof(fps)));

    TEST_ASSERT_FALSE(iohome::frame::set_execute_command_fp(
        nullptr, 0, iohome::Originator::USER, 0x61, fps, sizeof(fps)));
}

void test_unexpected_length_on_a_fixed_command_still_assumes_a_trailer(void) {
    // A challenge request has a documented 6-byte payload. Give it 10 - neither
    // 6 nor 6 + trailer - and the parser must fall back to assuming a trailer,
    // not to "our table says 6, so 10 bytes cannot include one".
    //
    // The distinction matters because the table has been wrong before: Execute
    // was listed as exactly 6 while real frames carry 8. Treating a documented
    // length as a floor for every command would have turned that kind of gap
    // into silently rejected authenticated frames.
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    uint8_t params[10];
    memset(params, 0x5A, sizeof(params));
    TEST_ASSERT_TRUE(iohome::frame::set_command(&frame, iohome::CMD_CHALLENGE_REQUEST,
                                                params, sizeof(params)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));
    TEST_ASSERT_TRUE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(10 - 8, parsed.data_len);
}

void test_long_execute_payload_without_trailer_is_plain(void) {
    // An 8-byte Execute payload with no authentication trailer. The parser used
    // to call this authenticated because the payload was at least as long as a
    // trailer, and then read two functional parameters plus part of the main
    // parameter as a MAC.
    IoFrame frame;
    iohome::frame::init_frame(&frame, true);
    const uint8_t fps[4] = {0x11, 0x22, 0x33, 0x44};
    TEST_ASSERT_TRUE(iohome::frame::set_execute_command_fp(
        &frame, iohome::MP_CLOSE, iohome::Originator::USER, iohome::ACEI_DEFAULT,
        fps, sizeof(fps)));
    TEST_ASSERT_TRUE(iohome::frame::finalize_frame_plain(&frame));

    uint8_t buffer[iohome::FRAME_MAX_SIZE];
    const size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));

    IoFrame parsed;
    TEST_ASSERT_TRUE(iohome::frame::parse_frame(buffer, len, &parsed));
    TEST_ASSERT_FALSE(parsed.authenticated);
    TEST_ASSERT_EQUAL_UINT8(8, parsed.data_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame.data, parsed.data, 8);
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
    RUN_TEST(test_command_ids_match_the_documented_table);
    RUN_TEST(test_parameter_tables_match_the_documentation);
    RUN_TEST(test_ksy_capture_frame);
    RUN_TEST(test_execute_command_with_extra_functional_params);
    RUN_TEST(test_long_execute_payload_without_trailer_is_plain);
    RUN_TEST(test_unexpected_length_on_a_fixed_command_still_assumes_a_trailer);
    RUN_TEST(test_key_transfer_sized_frames_fit);
    RUN_TEST(test_parse_never_overflows_for_any_control_byte);
    RUN_TEST(test_print_frame_handles_nullptr);

    return UNITY_END();
}
