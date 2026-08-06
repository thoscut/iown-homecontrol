/**
 * @file iohome_phy_framing.cpp
 * @brief UART-style start/stop bit framing for the io-homecontrol PHY
 * @author iown-homecontrol project
 */

#include "iohome_phy_framing.h"
#include "iohome_constants.h"

namespace iohome {
namespace phy {

namespace {

/// One bit of a byte, most-significant first (bit 7 is index 0).
inline uint8_t bit_msb_first(uint8_t byte, size_t index) {
  return static_cast<uint8_t>((byte >> (7 - index)) & 1);
}

} // namespace

size_t uart_encode(const uint8_t* logical, size_t logical_len,
                   uint8_t* wire_out, size_t wire_cap) {
  if (logical == nullptr || wire_out == nullptr) {
    return 0;
  }

  const size_t wire_len = uart_wire_size(logical_len);
  if (wire_len > wire_cap) {
    return 0;
  }

  // Start every output byte at the idle level so the unused tail of the final
  // byte reads as stop/idle (all ones) rather than a stray start bit.
  for (size_t i = 0; i < wire_len; i++) {
    wire_out[i] = 0xFF;
  }

  size_t bit = 0;  // running bit position in the output, MSB-first
  const auto put = [&](uint8_t value) {
    const size_t byte = bit >> 3;
    const size_t within = bit & 7;         // 0 = MSB
    const uint8_t mask = static_cast<uint8_t>(0x80 >> within);
    if (value) {
      wire_out[byte] = static_cast<uint8_t>(wire_out[byte] | mask);
    } else {
      wire_out[byte] = static_cast<uint8_t>(wire_out[byte] & ~mask);
    }
    bit++;
  };

  for (size_t i = 0; i < logical_len; i++) {
    const uint8_t b = logical[i];
    put(0);                                  // start bit
    for (size_t d = 0; d < 8; d++) {
      put(static_cast<uint8_t>((b >> d) & 1));  // data, least-significant first
    }
    put(1);                                  // stop bit
  }

  return wire_len;
}

size_t uart_decode(const uint8_t* wire, size_t wire_len,
                   uint8_t* logical_out, size_t max_logical) {
  if (wire == nullptr || logical_out == nullptr) {
    return 0;
  }

  const size_t total_bits = wire_len * 8;
  size_t out = 0;
  size_t bit = 0;

  const auto get = [&](size_t position) -> uint8_t {
    return bit_msb_first(wire[position >> 3], position & 7);
  };

  while (out < max_logical && bit + UART_BITS_PER_BYTE <= total_bits) {
    if (get(bit) != 0) {
      break;  // start bit must be 0; a 1 here is the trailing preamble
    }
    if (get(bit + 9) != 1) {
      break;  // stop bit must be 1
    }
    uint8_t value = 0;
    for (size_t d = 0; d < 8; d++) {
      // Data is least-significant first, so bit (bit+1+d) has weight 1<<d.
      value = static_cast<uint8_t>(value | (get(bit + 1 + d) << d));
    }
    logical_out[out++] = value;
    bit += UART_BITS_PER_BYTE;
  }

  return out;
}

size_t uart_decode_frame(const uint8_t* wire, size_t wire_len,
                         uint8_t* frame_out, size_t frame_cap) {
  if (wire == nullptr || frame_out == nullptr) {
    return 0;
  }

  // Decode the control byte alone first: its size field says how long the whole
  // frame is, and there is no point decoding past that into the preamble.
  uint8_t ctrl0 = 0;
  if (uart_decode(wire, wire_len, &ctrl0, 1) != 1) {
    return 0;
  }

  const size_t frame_len =
    static_cast<size_t>(ctrl0 & CTRL0_LENGTH_MASK) + FRAME_SIZE_FIELD_BIAS;

  if (frame_len < FRAME_MIN_SIZE || frame_len > FRAME_MAX_SIZE ||
      frame_len > frame_cap) {
    return 0;
  }

  const size_t got = uart_decode(wire, wire_len, frame_out, frame_len);
  return (got == frame_len) ? frame_len : 0;
}

} // namespace phy
} // namespace iohome
