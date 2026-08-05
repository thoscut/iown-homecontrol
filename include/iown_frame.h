/**
  * @file    iown_frame.h
  * @author  iown-homecontrol
  * @brief   Frame definitions and functions
  *
  * Frame (incl. CRC) F8 00 00003F 1A380B 00 01 61 0000 80D8 0500 02A6 24222E8BA351 5F52
  * FRAME = PREAMBLE
  *         SYNC_WORD (SFD = START_FRAME_DELIMITER)
  *         PACKET
  *         FCS (FRAME_CHECK_SEQUENCE)
  *         INTERFRAME_GAP
  *         PACKET = FRAME_CONTROL (CTRL_BYTE_1 + CTRL_BYTE_2)
  *         MAC (SENDER_NODE_ID + DESTINATION_NODE_ID)
  *         PAYLOAD (CMD + PARAMETER + [ROLLING_CODE + HMAC])
  *
  * These are the C-side notes on the on-air layout. The implementation the
  * firmware actually uses lives in src/protocol/iohome_constants.h and is
  * covered by unit tests against the captures in docs/linklayer.md; keep the
  * two in step. Where they disagreed, this file was the one that was wrong.
  */

#pragma once
/* Define to prevent recursive inclusion */
#ifdef __cplusplus
  extern "C" {
#endif

#include <stdint.h>

#include "iown_mac.h"

/*
 * Communication mode, as carried in bit 5 of Control Byte 0.
 *
 * These live here rather than in iown_defs.h because IOWN_LEN_PACKET() below
 * compares against them. iown_defs.h defined them *after* including this
 * header, so anyone including iown_frame.h on its own got an undefined
 * identifier out of that macro.
 */
#ifndef IOWN_MODE_1W
  #define IOWN_MODE_1W 1
#endif
#ifndef IOWN_MODE_2W
  #define IOWN_MODE_2W 0
#endif

#pragma region IOWN_LENGTH_DEFS /* ioHC Length Definitions */
// Packet Elements Length Definitions
//
// CTRL_BYTE_1 is Control Byte 0 in the protocol layer (order, 1W/2W flag and
// the 5-bit size field); CTRL_BYTE_2 is Control Byte 1.
#define IOWN_LEN_CTRL_BYTE_1  1
#define IOWN_LEN_CTRL_BYTE_2  1
/* IOWN_LEN_NODEID and IOWN_LEN_HEADER_MAC come from iown_mac.h */
#define IOWN_LEN_CMD          1
#define IOWN_LEN_ROLLING_CODE 2
#define IOWN_LEN_HMAC         6

// Header Length Definitions
#define IOWN_LEN_HEADER_FRAME_CONTROL (IOWN_LEN_CTRL_BYTE_1 + IOWN_LEN_CTRL_BYTE_2)
#define IOWN_LEN_HEADER (IOWN_LEN_HEADER_FRAME_CONTROL + IOWN_LEN_HEADER_MAC + IOWN_LEN_CMD)

// Packet Length Definitions
//
// The parameter block is command-dependent - Execute (0x00) alone carries at
// least six bytes - so there is no single IOWN_LEN_PARAMETER. It was defined
// as a flat 1, which made every length below wrong for every command that
// carries more than one byte. Callers pass the actual parameter length in.
//
// Authentication trailer, per docs/linklayer.md:
//   authenticated 1W: rolling code (2) + HMAC (6)
//   authenticated 2W: HMAC (6)          - the rolling code is not sent
//   unauthenticated:  neither
#define IOWN_LEN_PACKET_1W(PARAM_LEN) \
  (IOWN_LEN_HEADER + (PARAM_LEN) + IOWN_LEN_ROLLING_CODE + IOWN_LEN_HMAC)

// This omitted the HMAC entirely, describing an authenticated 2W packet as if
// it were a plain one.
#define IOWN_LEN_PACKET_2W(PARAM_LEN) \
  (IOWN_LEN_HEADER + (PARAM_LEN) + IOWN_LEN_HMAC)

#define IOWN_LEN_PACKET_PLAIN(PARAM_LEN) (IOWN_LEN_HEADER + (PARAM_LEN))

#define IOWN_LEN_PACKET(IOWN_MODE, PARAM_LEN) \
  (((IOWN_MODE) == IOWN_MODE_1W) ? IOWN_LEN_PACKET_1W(PARAM_LEN) \
                                 : IOWN_LEN_PACKET_2W(PARAM_LEN))

// Frame Length Definitions
//
// The sync word is three bytes on air (57 FD 99), not two.
#define IOWN_LEN_SYNC_WORD 3
#define IOWN_LEN_CRC       2

// Bytes on air after the sync word: the packet plus its CRC. That is what the
// size field describes, and what the CRC covers (minus the CRC itself). The
// old IOWN_LEN_FRAME counted the sync word as part of the frame, which no
// length in the protocol does.
#define IOWN_LEN_FRAME(IOWN_MODE, PARAM_LEN) \
  (IOWN_LEN_PACKET(IOWN_MODE, PARAM_LEN) + IOWN_LEN_CRC)
#pragma endregion IOWN_LENGTH_DEFS

#define IOWN_FRAME_SIZE_MASK       ((1 << 5) - 1) /* Get the last 5 bits */
#define IOWN_FRAME_SIZE(CTRLBYTE1) ((CTRLBYTE1) & IOWN_FRAME_SIZE_MASK)

/**
 * Total frame length, in bytes, encoded by the size field.
 *
 * The field counts everything except Control Byte 0 itself and the two CRC
 * bytes, so the frame is three bytes longer than the field says. Getting this
 * bias wrong is the easiest way to build frames no actuator ever answers.
 */
#define IOWN_FRAME_SIZE_BIAS       (IOWN_LEN_CTRL_BYTE_1 + IOWN_LEN_CRC)
#define IOWN_FRAME_LEN(CTRLBYTE1)  (IOWN_FRAME_SIZE(CTRLBYTE1) + IOWN_FRAME_SIZE_BIAS)

/** Largest frame the 5-bit size field can describe. */
#define IOWN_FRAME_LEN_MAX (IOWN_FRAME_SIZE_MASK + IOWN_FRAME_SIZE_BIAS)

/**
 * CRC-16/CCITT (KERMIT) over @p iown_packet_len bytes, excluding the CRC.
 *
 * Defined in src/esp32_utils.cpp. Returns 0 for a null or empty input.
 */
uint16_t iown_crc_calc(const uint8_t *iown_packet, uint8_t iown_packet_len);

#ifdef __cplusplus
}
#endif
