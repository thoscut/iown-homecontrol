/**
 * Unit tests for the PHY UART framing codec.
 *
 * The vectors are real: raw bytes an SX1262 (Heltec V4.2) delivered while
 * sniffing a live Velux installation, paired with the frame each one decodes
 * to. They are the same captures that were once read as a "non-standard
 * 48-byte OEM format"; de-framed, every one is an ordinary io-homecontrol
 * frame with a valid CRC. See docs/devices/velux/velux-frame-analysis.md.
 */

#include <unity.h>
#include "protocol/iohome_phy_framing.h"
#include "protocol/iohome_constants.h"
#include "protocol/iohome_crypto.h"
#include <string.h>

using namespace iohome;

namespace {

struct Vector {
  const char* raw;    // bytes off the radio, one representative per command type
  const char* frame;  // the frame they decode to
  const char* label;
};

// One per command type seen in the capture: Execute, Private, Private Answer,
// a 1W group command, the 1W key transfer, Remove 1W controller, and a 2W
// challenge response.
const Vector VECTORS[] = {
  {"37c01004017e51d2f48f00501434270040100541344d3474473443d62c193ac55d",
   "f60000003f717ae2000161c800000005169671c41678a330ae", "0x00 execute"},
  {"6940155c0f564fd73cef60581004012e505550",
   "4b00d5e0357ee7ee030300003a41", "0x03 private"},
  {"34c013f5cf3bc43615dd105410040100401004010047f3b44d4040100519285e1f",
   "96007ee7ee844377040500000000000000fc6e64010000310a", "0x04 private answer"},
  {"47c01005017e51d2f48f3a4015043178c855d55901de717c5713",
   "f10000013f717ae22e0005188f425d35c0cff4d4", "0x2e"},
  {"1fc01005017e4694ad31065370f40b47db150d83165f71a5f5565bd60c3d5a501404412cd3958cfb",
   "fc0000013f2ca91930d978a0f11b858334df2c5f357b83782d0101049a398d", "0x30 key transfer"},
  {"47c01004017e4694ad314e4011053320d592549b5d4af29d6182",
   "f10000003f2ca91939000499823552b25deaca0d", "0x39 remove 1W controller"},
  {"3840155c0f564fd73cef5e58b48d6b1748973caf645540",
   "0e00d5e0357ee7ee3da389ad7422e7ea13", "0x3d challenge response"},
};

size_t unhex(const char* h, uint8_t* out, size_t cap) {
  size_t n = 0;
  while (h[0] && h[1] && n < cap) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return 0;
    };
    out[n++] = static_cast<uint8_t>((nib(h[0]) << 4) | nib(h[1]));
    h += 2;
  }
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Real captures decode to valid frames
// ---------------------------------------------------------------------------

void test_captures_decode_to_the_expected_frame(void) {
  for (const Vector& v : VECTORS) {
    uint8_t raw[64], want[FRAME_MAX_SIZE], got[FRAME_MAX_SIZE];
    const size_t raw_len = unhex(v.raw, raw, sizeof raw);
    const size_t want_len = unhex(v.frame, want, sizeof want);

    const size_t got_len = phy::uart_decode_frame(raw, raw_len, got, sizeof got);
    TEST_ASSERT_EQUAL_UINT(want_len, got_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, want_len);
  }
}

void test_decoded_captures_pass_crc(void) {
  // The point of the whole exercise: these are standard frames, so the standard
  // CRC-16/KERMIT that never validated on the raw bytes validates now.
  for (const Vector& v : VECTORS) {
    uint8_t raw[64], frame[FRAME_MAX_SIZE];
    const size_t raw_len = unhex(v.raw, raw, sizeof raw);
    const size_t len = phy::uart_decode_frame(raw, raw_len, frame, sizeof frame);
    TEST_ASSERT_TRUE(len > 0);
    TEST_ASSERT_TRUE(crypto::verify_crc16(frame, len));
  }
}

void test_size_field_matches_the_decoded_length(void) {
  for (const Vector& v : VECTORS) {
    uint8_t raw[64], frame[FRAME_MAX_SIZE];
    const size_t raw_len = unhex(v.raw, raw, sizeof raw);
    const size_t len = phy::uart_decode_frame(raw, raw_len, frame, sizeof frame);
    const size_t declared =
      static_cast<size_t>(frame[0] & CTRL0_LENGTH_MASK) + FRAME_SIZE_FIELD_BIAS;
    TEST_ASSERT_EQUAL_UINT(declared, len);
  }
}

// ---------------------------------------------------------------------------
// Round trip
// ---------------------------------------------------------------------------

void test_encode_is_the_inverse_of_decode(void) {
  for (const Vector& v : VECTORS) {
    uint8_t frame[FRAME_MAX_SIZE], wire[64], back[FRAME_MAX_SIZE];
    const size_t frame_len = unhex(v.frame, frame, sizeof frame);

    const size_t wire_len = phy::uart_encode(frame, frame_len, wire, sizeof wire);
    TEST_ASSERT_EQUAL_UINT(phy::uart_wire_size(frame_len), wire_len);

    const size_t back_len = phy::uart_decode_frame(wire, wire_len, back, sizeof back);
    TEST_ASSERT_EQUAL_UINT(frame_len, back_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, back, frame_len);
  }
}

void test_encode_every_length_round_trips(void) {
  // Not just the captured lengths: every frame size, every byte value, so a
  // packing bug at some offset cannot hide.
  for (size_t len = FRAME_MIN_SIZE; len <= FRAME_MAX_SIZE; len++) {
    uint8_t frame[FRAME_MAX_SIZE], wire[64], back[FRAME_MAX_SIZE];
    for (size_t i = 0; i < len; i++) {
      frame[i] = static_cast<uint8_t>((i * 37 + len * 11) & 0xFF);
    }
    // Make the size field agree with the length so uart_decode_frame accepts it.
    frame[0] = static_cast<uint8_t>((frame[0] & ~CTRL0_LENGTH_MASK) |
                                    ((len - FRAME_SIZE_FIELD_BIAS) & CTRL0_LENGTH_MASK));

    const size_t wire_len = phy::uart_encode(frame, len, wire, sizeof wire);
    TEST_ASSERT_TRUE(wire_len > 0);
    const size_t back_len = phy::uart_decode_frame(wire, wire_len, back, sizeof back);
    TEST_ASSERT_EQUAL_UINT(len, back_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, back, len);
  }
}

// ---------------------------------------------------------------------------
// Framing details and edge cases
// ---------------------------------------------------------------------------

void test_start_and_stop_bits_are_checked(void) {
  // A byte with its start bit set to 1 is not a valid cell; decoding must stop
  // there rather than invent a byte.
  uint8_t frame[FRAME_MIN_SIZE];
  for (size_t i = 0; i < sizeof frame; i++) frame[i] = 0xA5;
  frame[0] = static_cast<uint8_t>((sizeof frame - FRAME_SIZE_FIELD_BIAS));

  uint8_t wire[64];
  const size_t wire_len = phy::uart_encode(frame, sizeof frame, wire, sizeof wire);

  // Corrupt the very first bit (the first start bit) to 1.
  wire[0] |= 0x80;
  uint8_t out[FRAME_MAX_SIZE];
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_decode_frame(wire, wire_len, out, sizeof out));
}

void test_trailing_preamble_ends_the_frame(void) {
  // Real captures are followed by a run of 0x55/0xAA preamble. uart_decode_frame
  // must return exactly the frame and ignore what follows.
  uint8_t raw[64], frame[FRAME_MAX_SIZE];
  const size_t raw_len = unhex(VECTORS[0].raw, raw, sizeof raw);
  // Append preamble bytes past what the codec needs.
  size_t n = raw_len;
  while (n < sizeof raw) { raw[n] = (n & 1) ? 0x55 : 0xAA; n++; }

  const size_t len = phy::uart_decode_frame(raw, n, frame, sizeof frame);
  uint8_t want[FRAME_MAX_SIZE];
  const size_t want_len = unhex(VECTORS[0].frame, want, sizeof want);
  TEST_ASSERT_EQUAL_UINT(want_len, len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, frame, want_len);
}

void test_short_buffer_yields_no_frame(void) {
  uint8_t raw[64], frame[FRAME_MAX_SIZE];
  const size_t raw_len = unhex(VECTORS[4].raw, raw, sizeof raw);  // 31-byte frame
  // Only the first few wire bytes: not enough for a whole frame.
  uint8_t out[FRAME_MAX_SIZE];
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_decode_frame(raw, 5, out, sizeof out));
  (void) frame;
  (void) raw_len;
}

void test_rejects_bad_arguments(void) {
  uint8_t buf[8] = {0};
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_encode(nullptr, 4, buf, sizeof buf));
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_encode(buf, 4, nullptr, sizeof buf));
  // Output too small: 4 frame bytes need 5 wire bytes.
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_encode(buf, 4, buf, 4));
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_decode(nullptr, 4, buf, sizeof buf));
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_decode_frame(buf, sizeof buf, nullptr, 4));
}

void test_wire_size_is_ten_bits_per_byte(void) {
  TEST_ASSERT_EQUAL_UINT(0, phy::uart_wire_size(0));
  TEST_ASSERT_EQUAL_UINT(13, phy::uart_wire_size(10));   // 100 bits -> 13 bytes
  TEST_ASSERT_EQUAL_UINT(32, phy::uart_wire_size(25));   // 250 bits -> 32 bytes
  TEST_ASSERT_EQUAL_UINT(43, phy::uart_wire_size(34));   // the maximum frame
}

int main(void) {
  UNITY_BEGIN();

  RUN_TEST(test_captures_decode_to_the_expected_frame);
  RUN_TEST(test_decoded_captures_pass_crc);
  RUN_TEST(test_size_field_matches_the_decoded_length);

  RUN_TEST(test_encode_is_the_inverse_of_decode);
  RUN_TEST(test_encode_every_length_round_trips);

  RUN_TEST(test_start_and_stop_bits_are_checked);
  RUN_TEST(test_trailing_preamble_ends_the_frame);
  RUN_TEST(test_short_buffer_yields_no_frame);
  RUN_TEST(test_rejects_bad_arguments);
  RUN_TEST(test_wire_size_is_ten_bits_per_byte);

  return UNITY_END();
}
