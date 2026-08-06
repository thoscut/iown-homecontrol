/**
 * @file iown_homecontrol.h
 * @brief ESPHome component for io-homecontrol protocol
 *
 * Hub component that initializes the radio module and provides
 * io-homecontrol frame reception, parsing, and transmission.
 *
 * Uses RadioLib for radio abstraction supporting SX1276/SX1262 modules.
 *
 * Protocol: io-homecontrol (iohc) - 868 MHz FSK
 * - 3 channels: 868.25, 868.95, 869.85 MHz
 * - 38.4 kbps, 19.2 kHz deviation, NRZ encoding
 * - Sync word: 57 FD 99 (bit-reversed form of the 0xFF33 OTA sequence)
 * - CRC-16/KERMIT, transmitted least significant byte first
 *
 * Frame layout (docs/linklayer.md):
 *
 *   | ctrl0 | ctrl1 | dest[3] | src[3] | cmd | data | [seq[2] mac[6]] | crc[2] |
 *
 * Control Byte 0: bits 7-6 Order, bit 5 isOneWay (1 = 1W), bits 4-0 Size,
 * where Size is the frame length excluding Control Byte 0 and the CRC.
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"

#include <RadioLib.h>
#include <SPI.h>
#include <vector>

// The protocol layer, copied verbatim from src/protocol/ by
// tools/sync_esphome_protocol.py. See PROTOCOL-COPY.md.
//
// The component used to carry its own CRC and MAC construction because it
// could not reach src/protocol/. It no longer has to: there is one
// implementation, and a copy of it that CI keeps identical.
#include "iohome_crypto.h"
#include "iohome_frame.h"
#include "iohome_phy_framing.h"
#include "iohome_replay_guard.h"
#include "iohome_2w.h"

namespace esphome {
namespace iown_homecontrol {

// io-homecontrol channel frequencies (MHz)
static const float IOHC_CHANNEL_1 = 868.25f;  // 2W only
static const float IOHC_CHANNEL_2 = 868.95f;  // 1W/2W (default)
static const float IOHC_CHANNEL_3 = 869.85f;  // 2W only

// Physical layer parameters.
// The sync word is the bit-reversed form of the on-air 0xFF33 sequence,
// because io-homecontrol transmits LSB first while RadioLib matches MSB first.
static const uint8_t IOHC_SYNC_WORD[] = {0x57, 0xFD, 0x99};
static const uint8_t IOHC_SYNC_WORD_LEN = 3;
static const uint16_t IOHC_PREAMBLE_BITS = 512;

// RadioLib takes the FSK bit rate in kbps and the deviation in kHz.
static const float IOHC_BITRATE_KBPS = 38.4f;
static const float IOHC_DEVIATION_KHZ = 19.2f;

// Control Byte 0 (bit 5 is `isOneWay`)
static const uint8_t IOHC_CTRL0_ONE_WAY = 0x20;
static const uint8_t IOHC_CTRL0_SIZE_MASK = 0x1F;
static const uint8_t IOHC_MODE_2W = 0x00;
static const uint8_t IOHC_MODE_1W = 0x20;

/// Size field bias: total frame length = Size + 3.
static const uint8_t IOHC_SIZE_BIAS = 3;

// Command IDs
static const uint8_t IOHC_CMD_EXECUTE = 0x00;
static const uint8_t IOHC_CMD_ACTIVATE_MODE = 0x01;

// Main parameter values for command 0x00
static const uint16_t IOHC_PARAM_OPEN = 0x0000;   // Min / fully open
static const uint16_t IOHC_PARAM_CLOSE = 0xC800;  // Max / fully closed
static const uint16_t IOHC_PARAM_STOP = 0xD200;   // Current position
static const uint16_t IOHC_PARAM_PERCENT_MAX = 0xC800;

/// Default ACEI byte: user priority level 2, IsValid set.
///
/// docs/commands.md: "LSB must be 1: The frame is not considered valid if this
/// bit is set to 0!". An ACEI of 0x00 is discarded by every actuator.
static const uint8_t IOHC_ACEI_DEFAULT = 0x61;
static const uint8_t IOHC_ACEI_VALID_MASK = 0x01;

/// Default command originator: user remote control.
static const uint8_t IOHC_ORIGINATOR_USER = 0x01;

/// Largest frame the 5-bit size field can describe: 31 + 3.
static const size_t IOHC_MAX_FRAME_SIZE = 34;

/// How many bytes fixed-length receive mode pulls off the air per packet.
///
/// A frame maxes out at 34 bytes, but each byte is sent as ten bits on the wire
/// - a start bit, the eight data bits, a stop bit (see iohome_phy_framing.h) -
/// so the radio delivers up to ceil(34 * 10 / 8) = 43 bytes for one frame,
/// before the trailing preamble. 64 leaves room for that plus the run-in the
/// receiver sees after the frame ends. The raw bytes are de-framed before
/// anything reads them as a frame.
///
/// This capture size is why a frame that looked like a "non-standard 48-byte
/// format" - and failed every CRC test - turned out to be an ordinary frame
/// wrapped in that UART framing. See docs/devices/velux/velux-frame-analysis.md.
static const size_t IOHC_RX_CAPTURE_SIZE = 64;

/// Minimum frame: ctrl0(1) + ctrl1(1) + dest(3) + src(3) + cmd(1) + crc(2).
static const size_t IOHC_MIN_FRAME_SIZE = 11;

/// Offsets inside the command 0x00 payload, counted from the command byte.
static const size_t IOHC_EXEC_OFFSET_ORIGINATOR = 1;
static const size_t IOHC_EXEC_OFFSET_ACEI = 2;
static const size_t IOHC_EXEC_OFFSET_MAIN_PARAM = 3;
static const size_t IOHC_EXEC_OFFSET_FP1 = 5;
static const size_t IOHC_EXEC_OFFSET_FP2 = 6;
static const size_t IOHC_EXEC_PAYLOAD_SIZE = 6;

enum RadioType {
  RADIO_SX1276 = 0,
  RADIO_SX1262 = 1,
};

// Forward declaration
class IOWNCover;

/**
 * @brief A received io-homecontrol frame, already validated.
 */
struct ReceivedFrame {
  /// True when the frame carried a MAC this component verified against the
  /// system key. Anything that acts on a frame must check this first.
  bool authenticated;
  uint8_t ctrl0;
  uint8_t ctrl1;
  uint32_t dest_address;
  uint32_t src_address;
  uint8_t command;
  const uint8_t *payload;   // Points into the receive buffer
  size_t payload_len;
  bool is_1w;
  int16_t rssi;
};

/**
 * @class IOWNHomeControlComponent
 * @brief Main hub component for io-homecontrol radio communication.
 *
 * Initializes the radio module, configures io-homecontrol physical layer
 * parameters, receives and parses incoming frames, and provides methods
 * for transmitting cover commands.
 */
class IOWNHomeControlComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  void set_cs_pin(int pin) { this->cs_pin_ = pin; }
  void set_rst_pin(int pin) { this->rst_pin_ = pin; }
  void set_dio0_pin(int pin) { this->dio0_pin_ = pin; }
  void set_dio1_pin(int pin) { this->dio1_pin_ = pin; }
  /// SX126x only: the BUSY line. SX127x modules do not have one.
  void set_busy_pin(int pin) { this->busy_pin_ = pin; }
  void set_frequency(float freq) { this->frequency_ = freq; }
  void set_radio_type(RadioType type) { this->radio_type_ = type; }
  void set_source_address(uint32_t addr) { this->source_address_ = addr; }
  void set_sck_pin(int pin) { this->sck_pin_ = pin; }
  void set_mosi_pin(int pin) { this->mosi_pin_ = pin; }
  void set_miso_pin(int pin) { this->miso_pin_ = pin; }
  void set_system_key(const std::vector<uint8_t> &key);
  void set_encryption_enabled(bool enabled) { this->encryption_enabled_ = enabled; }
  void set_acei(uint8_t acei) { this->acei_ = acei; }
  void set_originator(uint8_t originator) { this->originator_ = originator; }
  void set_position_feedback(bool enabled) { this->position_feedback_ = enabled; }
  /// Select 2W (challenge-response authenticated) instead of 1W.
  void set_two_way(bool enabled) { this->two_way_ = enabled; }

  /// Read the system key out of a 1W key transfer (command 0x30) and log it.
  ///
  /// A 1W controller sends its key masked with the public TRANSFER_KEY, so a
  /// receiver in range recovers it - that is the protocol's own pairing path,
  /// not a weakness being exploited, but it does mean anyone listening while
  /// you pair gets the key too. Off by default; turn it on only for the pairing
  /// itself and off again afterwards.
  void set_key_capture(bool enabled) { this->key_capture_ = enabled; }

  /// SX126x only: the voltage the radio supplies on DIO3 to an external TCXO.
  ///
  /// RadioLib defaults this to 1.6 V, which is not what every board wants -
  /// the Heltec V3 and V4 specify 1.8 V. Running the oscillator below its
  /// rated supply is not an obvious failure: SX126x::config() falls back to
  /// plain XTAL when the oscillator reports a start error, leaving a radio
  /// that initialises cleanly and sits on the wrong frequency. 0 selects a
  /// crystal and skips DIO3 entirely.
  void set_tcxo_voltage(float volts) { this->tcxo_voltage_ = volts; }

  /// Board power rail feeding the radio front end - Heltec calls it VEXT and
  /// drives it active low. Must be on before the radio is talked to.
  void set_vext_pin(int pin, bool active_high) {
    this->vext_pin_ = pin;
    this->vext_active_high_ = active_high;
  }

  /// External PA/LNA front end (Heltec V4.2: GC1109).
  ///
  /// @param power_pin  Enables the LDO feeding the front end (VFEM_Ctrl).
  /// @param enable_pin Chip enable, active high (GC1109 CSD).
  /// @param tx_pin     TX/RX path select (GC1109 CPS): high selects the PA,
  ///                   low the receive bypass. Handed to RadioLib so it
  ///                   follows the radio's own TX/RX transitions - holding it
  ///                   high would route reception through the PA.
  void set_rf_frontend(int power_pin, int enable_pin, int tx_pin) {
    this->fem_power_pin_ = power_pin;
    this->fem_enable_pin_ = enable_pin;
    this->fem_tx_pin_ = tx_pin;
  }

  void register_cover(IOWNCover *cover) { this->covers_.push_back(cover); }

  /** Send a raw frame over the radio. */
  bool send_frame(const uint8_t *data, size_t len);

  /** Send a cover control command (command 0x00 with a main parameter). */
  bool send_cover_command(uint32_t target_address, uint8_t command, uint16_t main_param,
                          uint8_t fp1 = 0x00, uint8_t fp2 = 0x00);

  /** Convert a percentage of closure (0-100) to a main parameter value. */
  static uint16_t main_param_from_percent_closed(uint8_t percent_closed);

  /** Inverse of main_param_from_percent_closed(); 0xFF if out of range. */
  static uint8_t percent_closed_from_main_param(uint16_t main_param);

  /** Compute CRC-16/KERMIT over the given data. */
  static uint16_t compute_crc(const uint8_t *data, size_t len);

  /** RSSI of the most recently received frame, in dBm. */
  int16_t last_rssi() const { return this->last_rssi_; }

  /** Number of frames accepted since boot. */
  uint32_t frames_received() const { return this->frames_received_; }

  /** Number of frames dropped because the CRC did not match. */
  uint32_t crc_errors() const { return this->crc_errors_; }
  /// Frames whose MAC did not verify against the configured system key.
  uint32_t mac_errors() const { return this->mac_errors_; }
  /// Authenticated frames whose rolling code had already been seen.
  uint32_t replay_errors() const { return this->replay_errors_; }
  /// Whether a 2W session is currently authenticated. Always false in 1W mode.
  bool is_2w_authenticated();

  /** Rolling code that will be used for the next authenticated transmission. */
  uint16_t rolling_code() const { return this->rolling_code_; }

 protected:
  int cs_pin_{-1};
  int rst_pin_{-1};
  int dio0_pin_{-1};
  int dio1_pin_{-1};
  int busy_pin_{-1};

  /// Rejects a replayed rolling code from a node we have already heard from.
  /// Position reports are authenticated but not fresh without this: a recorded
  /// "fully closed" frame replays perfectly.
  iohome::ReplayGuard replay_guard_;

  /// 2W challenge-response state. One session at a time, which is the same
  /// simplification IoHomeControl makes: the challenge is a property of the
  /// exchange, and a controller talks to one actuator at a time anyway.
  iohome::mode2w::AuthenticationManager auth_;
  bool two_way_{false};

  /// Build a 3-byte node ID from the 24-bit address the configuration uses.
  static void address_to_node(uint32_t address, uint8_t node[iohome::NODE_ID_SIZE]) {
    node[0] = static_cast<uint8_t>((address >> 16) & 0xFF);
    node[1] = static_cast<uint8_t>((address >> 8) & 0xFF);
    node[2] = static_cast<uint8_t>(address & 0xFF);
  }

  /// Whether an authenticated frame is also *fresh*, by whichever mechanism
  /// its mode carries. A valid MAC proves authorship, never freshness.
  bool frame_is_fresh_(const iohome::frame::IoFrame &frame);

  /// Serialize and transmit a frame the protocol layer built.
  bool send_protocol_frame_(const iohome::frame::IoFrame *frame);

  /// Make sure a 2W session is authenticated, starting a handshake if not.
  /// Returns false while the handshake is still in flight.
  bool ensure_2w_session_(uint32_t target_address);

  /// Send a command 0x00 over an authenticated 2W session.
  bool send_2w_command_(uint32_t target_address, uint16_t main_param, uint8_t fp1, uint8_t fp2);

  /// Handle 0x3C / 0x3D during reception.
  void handle_challenge_frame_(const iohome::frame::IoFrame *frame);

  /// Bring the board's power rail and any external front end up, before the
  /// radio is reset or addressed over SPI.
  void power_up_frontend_();

  int sck_pin_{-1};
  int mosi_pin_{-1};
  int miso_pin_{-1};
  float frequency_{IOHC_CHANNEL_2};
  RadioType radio_type_{RADIO_SX1276};
  uint32_t source_address_{0x1A380B};

  /// Matches RadioLib's own default, so leaving this unset changes nothing.
  float tcxo_voltage_{1.6f};
  int vext_pin_{-1};
  bool vext_active_high_{false};
  int fem_power_pin_{-1};
  int fem_enable_pin_{-1};
  int fem_tx_pin_{-1};

  Module *radio_module_{nullptr};
  PhysicalLayer *phy_{nullptr};
  SX1276 *sx1276_{nullptr};
  SX1262 *sx1262_{nullptr};

  std::vector<IOWNCover *> covers_;

  uint8_t system_key_[16] = {0};
  bool system_key_set_{false};
  bool encryption_enabled_{false};
  bool position_feedback_{false};
  bool key_capture_{false};
  uint8_t acei_{IOHC_ACEI_DEFAULT};
  uint8_t originator_{IOHC_ORIGINATOR_USER};

  // Rolling code, persisted in NVS.
  //
  // A receiver rejects a sequence number it has already seen, so the counter
  // must not restart at 0 after a reboot. Writes are batched: a block of
  // counter values is reserved and only the end of the block is stored.
  static const uint16_t ROLLING_CODE_RESERVE_BLOCK = 64;
  uint16_t rolling_code_{0};
  uint16_t rolling_code_reserved_until_{0};
  ESPPreferenceObject rolling_code_pref_;

  int16_t last_rssi_{0};
  uint32_t frames_received_{0};
  uint32_t crc_errors_{0};
  uint32_t mac_errors_{0};
  uint32_t replay_errors_{0};

  // Packet-ready flag, set from the radio interrupt and consumed in loop().
  //
  // A plain `volatile bool` rather than `std::atomic<bool>`: a single aligned
  // byte is atomic in hardware on Xtensa, the ISR only ever sets the flag and
  // the main loop only ever clears it, and `volatile` already stops the
  // compiler from caching or reordering the access.
  //
  // std::atomic would be the tidier spelling, but its store expands to a
  // barrier sequence that needs a literal-pool load. Emitting that inside an
  // IRAM_ATTR function produces
  //   "dangerous relocation: l32r: literal placed after use"
  // at link time on ESP32, so the ISR is defined out of line in the .cpp and
  // kept as small as possible.
  static volatile bool packet_flag_;

  /** Radio interrupt handler; runs from IRAM. */
  static void packet_isr_();

  /** Read and clear the packet flag. */
  static bool take_packet_flag_();

  /** Clear the packet flag without acting on it. */
  static void clear_packet_flag_();

  /** Configure the radio physical layer for io-homecontrol. */
  int16_t configure_phy_layer_();

  /** Disable the radio's own CRC and length byte, which io-homecontrol does not use. */
  int16_t configure_packet_format_();

  /** Program the radio's fixed payload length before a transmission. */
  int16_t set_packet_length_(uint8_t len);

  /** Check for and process received frames. */
  void receive_frame_();

  /** Parse and dispatch a received io-homecontrol frame. */
  void parse_frame_(const uint8_t *data, size_t len, int16_t rssi);

  /** Feed a decoded execute frame to any cover that owns the source address. */
  void dispatch_to_covers_(const ReceivedFrame &frame);

  /** Recover the system key from a 1W key transfer and verify it via its MAC. */
  void handle_1w_key_transfer_(const uint8_t *data, size_t frame_len, uint32_t src_addr);

  /** Compute the 6-byte MAC for 1W mode using AES-128-ECB. */
  bool compute_hmac_(const uint8_t *frame_data, size_t data_len,
                     const uint8_t rolling_code[2], uint8_t hmac_out[6]);

  /** Take the next rolling code, extending the persisted reservation. */
  uint16_t consume_rolling_code_();

  /** Restore the rolling code from NVS and reserve a fresh block. */
  void restore_rolling_code_();
};

}  // namespace iown_homecontrol
}  // namespace esphome
