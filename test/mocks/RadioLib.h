/**
 * @file RadioLib.h
 * @brief Minimal RadioLib mock for the native test build
 *
 * `IoHomeControl` talks to the radio only through RadioLib's `PhysicalLayer`
 * interface. This header provides just enough of that interface to construct
 * the controller on the host, so the parts worth testing - the receive policy,
 * the rolling-code reservation, frame construction - can be exercised without
 * hardware.
 *
 * It is NOT on the normal include path: only `tools/run_native_tests.sh` adds
 * `-Itest/mocks`. Firmware builds use the real RadioLib.
 *
 * Keep the signatures in step with RadioLib; a mock that has drifted from the
 * real interface tests nothing useful.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <vector>

// The mock stands in for the *air*, not just the chip: a real io-homecontrol
// frame travels UART-framed (start/stop bits, LSB first), and the library now
// frames on transmit and de-frames on receive. So the mock frames what a test
// delivers and de-frames what the library transmits, presenting tests the
// logical frame either way. deliver_raw()/last_transmission still expose the
// on-air bytes for the tests that care about them.
#include "protocol/iohome_phy_framing.h"

// Error codes, copied verbatim from RadioLib 7.x src/TypeDef.h.
//
// These MUST match the real values. An earlier version of this mock invented
// RADIOLIB_ERR_INVALID_RADIO and got two other numbers wrong; the native build
// happily compiled against it while the firmware build failed, which is the
// whole failure mode a mock invites. tools/check_radiolib_mock.sh now compares
// every RADIOLIB_* constant below against the pinned upstream header and fails
// the test job on any drift. (It checks constants only - drift in the method
// signatures further down is caught by the PlatformIO build, which compiles
// IoHomeControl.cpp against the real PhysicalLayer.)
#define RADIOLIB_ERR_NONE (0)
#define RADIOLIB_ERR_UNKNOWN (-1)
#define RADIOLIB_ERR_CHIP_NOT_FOUND (-2)
#define RADIOLIB_ERR_PACKET_TOO_LONG (-4)
#define RADIOLIB_ERR_TX_TIMEOUT (-5)
#define RADIOLIB_ERR_RX_TIMEOUT (-6)
#define RADIOLIB_ERR_INVALID_OUTPUT_POWER (-13)
#define RADIOLIB_ERR_WRONG_MODEM (-20)

#define RADIOLIB_ENCODING_NRZ (0x00)
#define RADIOLIB_SHAPING_NONE (0x00)

#define RADIOLIB_ASSERT(STATEVAR) \
  do {                            \
    if ((STATEVAR) != RADIOLIB_ERR_NONE) { \
      return (STATEVAR);          \
    }                             \
  } while (0)

struct FSKRate_t {
  float bitRate;
  float freqDev;
};

union DataRate_t {
  FSKRate_t fsk;
};

/**
 * @brief Stand-in for RadioLib's PhysicalLayer.
 *
 * Records what the controller programmed and lets a test feed frames back in.
 */
class PhysicalLayer {
 public:
  virtual ~PhysicalLayer() = default;

  // --- Configuration -------------------------------------------------------

  virtual int16_t setFrequency(float freq) {
    frequency = freq;
    return frequency_result;
  }

  virtual int16_t setOutputPower(int8_t power) {
    // Model a module that rejects anything above 17 dBm, like a real one does.
    if (power > 17) {
      return RADIOLIB_ERR_INVALID_OUTPUT_POWER;
    }
    output_power = power;
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t setDataRate(DataRate_t dr) {
    bit_rate = dr.fsk.bitRate;
    freq_dev = dr.fsk.freqDev;
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t setEncoding(uint8_t enc) {
    encoding = enc;
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t setDataShaping(uint8_t sh) {
    shaping = sh;
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t setSyncWord(uint8_t* sync, size_t len) {
    sync_word.assign(sync, sync + len);
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t setPreambleLength(size_t len) {
    preamble_length = len;
    return RADIOLIB_ERR_NONE;
  }

  // --- Operation -----------------------------------------------------------

  virtual int16_t startReceive() {
    receive_started++;
    return receive_result;
  }

  virtual int16_t standby() {
    standby_calls++;
    return RADIOLIB_ERR_NONE;
  }

  virtual int16_t transmit(const uint8_t* data, size_t len, uint8_t addr = 0) {
    (void) addr;
    // The library transmits UART-framed wire bytes. Record both the raw wire
    // and the frame it decodes to, so field assertions can read the logical
    // frame while last_wire keeps what actually went out.
    last_wire.assign(data, data + len);
    uint8_t frame[64];
    const size_t frame_len = iohome::phy::uart_decode_frame(data, len, frame, sizeof frame);
    if (frame_len > 0) {
      last_transmission.assign(frame, frame + frame_len);
    } else {
      last_transmission.assign(data, data + len);  // not a frame; keep raw
    }
    transmissions.push_back(last_transmission);
    return transmit_result;
  }

  virtual int16_t readData(uint8_t* data, size_t len) {
    if (len > rx_buffer.size()) {
      len = rx_buffer.size();
    }
    memcpy(data, rx_buffer.data(), len);
    return read_result;
  }

  virtual size_t getPacketLength(bool update = true) {
    (void) update;
    return rx_buffer.size();
  }

  virtual int16_t getRSSI() { return rssi; }
  virtual float getSNR() { return snr; }

  virtual void setPacketReceivedAction(void (*func)(void)) { packet_action = func; }
  virtual void clearPacketReceivedAction() { packet_action = nullptr; }

  // --- Test helpers --------------------------------------------------------

  /// Deliver a logical frame: frame it as the air would and fire the interrupt.
  void deliver(const uint8_t* data, size_t len) {
    uint8_t wire[64];
    const size_t wire_len = iohome::phy::uart_encode(data, len, wire, sizeof wire);
    if (wire_len > 0) {
      deliver_raw(wire, wire_len);
    } else {
      deliver_raw(data, len);  // too long to frame; deliver as-is
    }
  }

  /// Deliver raw on-air bytes unchanged - for malformed or noise injection that
  /// is not a well-formed logical frame.
  void deliver_raw(const uint8_t* data, size_t len) {
    rx_buffer.assign(data, data + len);
    if (packet_action != nullptr) {
      packet_action();
    }
  }

  void reset() {
    transmissions.clear();
    last_transmission.clear();
    last_wire.clear();
    rx_buffer.clear();
    receive_started = 0;
    standby_calls = 0;
  }

  // Recorded configuration
  float frequency = 0.0f;
  int8_t output_power = 0;
  float bit_rate = 0.0f;
  float freq_dev = 0.0f;
  uint8_t encoding = 0xFF;
  uint8_t shaping = 0xFF;
  std::vector<uint8_t> sync_word;
  size_t preamble_length = 0;

  // Recorded traffic
  std::vector<std::vector<uint8_t>> transmissions;
  std::vector<uint8_t> last_transmission;  // de-framed logical frame
  std::vector<uint8_t> last_wire;          // raw UART-framed bytes as sent
  std::vector<uint8_t> rx_buffer;

  // Call counters
  int receive_started = 0;
  int standby_calls = 0;

  // Injectable results
  int16_t frequency_result = RADIOLIB_ERR_NONE;
  int16_t receive_result = RADIOLIB_ERR_NONE;
  int16_t transmit_result = RADIOLIB_ERR_NONE;
  int16_t read_result = RADIOLIB_ERR_NONE;

  int16_t rssi = -75;
  float snr = 9.5f;

  void (*packet_action)(void) = nullptr;
};
