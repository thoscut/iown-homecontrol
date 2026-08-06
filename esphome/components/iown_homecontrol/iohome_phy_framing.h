/**
 * @file iohome_phy_framing.h
 * @brief UART-style start/stop bit framing for the io-homecontrol PHY
 * @author iown-homecontrol project
 *
 * On air, io-homecontrol does not send a frame's bytes directly. Each byte is
 * wrapped the way a UART wraps a character: a start bit (0), the eight data
 * bits **least-significant first**, then a stop bit (1). `docs/radio.md`:
 *
 *   > The data is transmitted as 8-bit bytes with 1 start bit (`0`) and a stop
 *   > bit (`1`). Bytes are transmitted in order, but bits of each byte are
 *   > swapped: the least significant bit is transmitted first.
 *
 * So every logical byte occupies ten bits on the wire, and a plain FSK radio -
 * an SX1276, an SX1262 - has no hardware that removes them. It hands the whole
 * bitstream up as bytes, start and stop bits included. A receiver that treats
 * those raw bytes as the frame sees something that is 10/8 too long, whose
 * "control byte" size field does not match its length, and whose CRC never
 * validates. That is exactly what a capture session with this component's
 * SX1262 produced, and it was read as a "non-standard 48-byte OEM format" -
 * see `docs/devices/velux/velux-frame-analysis.md`. It is not non-standard.
 * De-framed, all 173 captured frames validate a CRC-16/KERMIT and decode to
 * ordinary frames, one of them a 1W key transfer.
 *
 * This module is that missing step. `uart_decode_frame()` turns the radio's raw
 * bytes back into frame bytes on receive; `uart_encode()` wraps a frame for
 * transmit. Both are pure and host-tested against the real captures.
 *
 * The sync word (`57 FD 99`) is a raw pattern the radio matches and is **not**
 * framed - the radio consumes it, and the bitstream this module sees begins at
 * the control byte's start bit.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace iohome {
namespace phy {

/// Bits per byte on the wire: 1 start + 8 data + 1 stop.
constexpr size_t UART_BITS_PER_BYTE = 10;

/// Wire bytes needed to carry @p logical_len framed bytes (rounded up).
constexpr size_t uart_wire_size(size_t logical_len) {
  return (logical_len * UART_BITS_PER_BYTE + 7) / 8;
}

/**
 * @brief Wrap frame bytes in the on-air UART framing.
 *
 * Each input byte becomes ten bits - start(0), data LSB-first, stop(1) - and
 * the resulting bitstream is packed most-significant-bit-first into the output,
 * the same order the radio clocks bytes out. Any bits left over in the final
 * output byte are set to 1 (the idle/stop level), so a device that reads one
 * byte past the frame sees idle, not a spurious start bit.
 *
 * @param logical Frame bytes (control byte through CRC)
 * @param logical_len Number of frame bytes
 * @param wire_out Output buffer for the on-air bytes
 * @param wire_cap Capacity of @p wire_out
 * @return Number of wire bytes written, or 0 on bad arguments or overflow
 */
size_t uart_encode(const uint8_t* logical, size_t logical_len,
                   uint8_t* wire_out, size_t wire_cap);

/**
 * @brief Recover frame bytes from a raw on-air bitstream.
 *
 * Reads the bitstream most-significant-bit-first and, for each ten-bit cell,
 * takes the eight data bits (least-significant first) as one output byte. The
 * start and stop bits are checked: decoding stops at the first cell whose start
 * bit is not 0 or whose stop bit is not 1, which is how the trailing preamble
 * run (`55`/`AA`) marks the end of the frame.
 *
 * This does not know how long the frame is; it decodes as many whole cells as
 * the framing allows. Use uart_decode_frame() on receive, which stops at the
 * length the control byte declares.
 *
 * @param wire Raw bytes from the radio
 * @param wire_len Number of raw bytes
 * @param logical_out Output buffer for frame bytes
 * @param max_logical Capacity of @p logical_out
 * @return Number of frame bytes recovered
 */
size_t uart_decode(const uint8_t* wire, size_t wire_len,
                   uint8_t* logical_out, size_t max_logical);

/**
 * @brief De-frame exactly one complete frame from a raw radio buffer.
 *
 * Decodes the first cell to get the control byte, reads its 5-bit size field to
 * learn the frame length (Size + 3), and decodes that many bytes. The bytes
 * after the frame - trailing preamble, the next frame's lead-in, idle - are not
 * touched.
 *
 * @param wire Raw bytes from the radio
 * @param wire_len Number of raw bytes
 * @param frame_out Output buffer for the frame
 * @param frame_cap Capacity of @p frame_out
 * @return Frame length in bytes, or 0 if a whole frame could not be recovered
 *         (framing broke, the buffer was too short, or the declared length did
 *         not fit)
 */
size_t uart_decode_frame(const uint8_t* wire, size_t wire_len,
                         uint8_t* frame_out, size_t frame_cap);

} // namespace phy
} // namespace iohome
