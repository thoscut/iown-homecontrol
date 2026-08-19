/**
 * Unit tests for IoHomeControl against the RadioLib mock.
 *
 * The focus is the receive policy - the code path that decides whether a frame
 * arriving off the air is allowed to do anything - plus the rolling-code
 * reservation and the shape of the frames that go out.
 */

#include <unity.h>

#include "IoHomeControl.h"

#include <string.h>
#include <string>
#include <utility>
#include <vector>

using iohome::IoHomeControl;
using iohome::RxReject;

namespace {

const uint8_t OWN_NODE[3] = {0x1A, 0x38, 0x0B};
const uint8_t PEER_NODE[3] = {0x70, 0x87, 0x58};
const uint8_t SYSTEM_KEY[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/// Build a frame as a peer would send it, so it can be fed to the receiver.
std::vector<uint8_t> make_frame(uint8_t command, uint16_t main_param, uint16_t sequence,
                                const uint8_t key[16], bool authenticated = true) {
  iohome::frame::IoFrame frame;
  iohome::frame::init_frame(&frame, true);
  iohome::frame::set_destination(&frame, OWN_NODE);
  iohome::frame::set_source(&frame, PEER_NODE);

  if (command == iohome::CMD_EXECUTE) {
    iohome::frame::set_execute_command(&frame, main_param);
  } else {
    iohome::frame::set_command(&frame, command, nullptr, 0);
  }

  if (authenticated) {
    iohome::frame::set_rolling_code(&frame, sequence);
    iohome::frame::finalize_frame(&frame, key);
  } else {
    iohome::frame::finalize_frame_plain(&frame);
  }

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&frame, buffer, sizeof(buffer));
  return std::vector<uint8_t>(buffer, buffer + len);
}

/// Counts the RollingCodeStore writes so flash wear can be asserted on.
class CountingStore : public iohome::RollingCodeStore {
 public:
  bool load(const uint8_t node_id[iohome::NODE_ID_SIZE], uint16_t& code) override {
    (void) node_id;
    code = stored;
    return has_value;
  }

  bool save(const uint8_t node_id[iohome::NODE_ID_SIZE], uint16_t code) override {
    (void) node_id;
    stored = code;
    has_value = true;
    saves++;
    return true;
  }

  uint16_t stored = 0;
  bool has_value = false;
  int saves = 0;
};

}  // namespace

// ---------------------------------------------------------------------------
// Initialization and radio configuration
// ---------------------------------------------------------------------------

void test_begin_rejects_nullptr(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);

  TEST_ASSERT_FALSE(controller.begin(nullptr, SYSTEM_KEY));
  TEST_ASSERT_FALSE(controller.begin(OWN_NODE, nullptr));
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));
}

void test_begin_without_radio_fails(void) {
  IoHomeControl controller(nullptr);
  TEST_ASSERT_FALSE(controller.begin(OWN_NODE, SYSTEM_KEY));
}

void test_configure_radio_programs_correct_parameters(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  TEST_ASSERT_EQUAL_INT(RADIOLIB_ERR_NONE, controller.configure_radio());

  TEST_ASSERT_FLOAT_WITHIN(0.01f, 868.95f, radio.frequency);

  // RadioLib takes kbps and kHz here, not bit/s and Hz.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 38.4f, radio.bit_rate);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 19.2f, radio.freq_dev);

  // The sync word must be the bit-reversed form the radio matches against.
  // Deriving it from the 0xFF33 OTA value by shifting yields 00 FF 33, which
  // no device answers.
  TEST_ASSERT_EQUAL_UINT(3, radio.sync_word.size());
  TEST_ASSERT_EQUAL_HEX8(0x57, radio.sync_word[0]);
  TEST_ASSERT_EQUAL_HEX8(0xFD, radio.sync_word[1]);
  TEST_ASSERT_EQUAL_HEX8(0x99, radio.sync_word[2]);

  // Preamble length is expressed in bits.
  TEST_ASSERT_EQUAL_UINT(512, radio.preamble_length);

  // The power loop must settle on a value the module accepts, not spin.
  TEST_ASSERT_EQUAL_INT(17, radio.output_power);
}

// ---------------------------------------------------------------------------
// Transmission
// ---------------------------------------------------------------------------

void test_execute_frame_shape(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
  TEST_ASSERT_EQUAL_UINT(1, radio.transmissions.size());

  const std::vector<uint8_t>& tx = radio.last_transmission;

  // header(9) + execute payload(6) + seq(2) + mac(6) + crc(2)
  TEST_ASSERT_EQUAL_UINT(25, tx.size());

  // Control Byte 0: 1W bit set, size = total - 3
  TEST_ASSERT_TRUE((tx[0] & iohome::CTRL0_ONE_WAY_MASK) != 0);
  TEST_ASSERT_EQUAL_UINT8(25 - 3, tx[0] & iohome::CTRL0_LENGTH_MASK);

  TEST_ASSERT_EQUAL_HEX8(0x70, tx[2]);  // destination
  TEST_ASSERT_EQUAL_HEX8(0x1A, tx[5]);  // source

  // Actuators are driven by command 0x00, not a dedicated per-action command.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, tx[8]);
  TEST_ASSERT_EQUAL_HEX8(0x01, tx[9]);   // Originator::USER
  TEST_ASSERT_EQUAL_HEX8(0x61, tx[10]);  // ACEI, bit 0 set
  TEST_ASSERT_TRUE(iohome::is_acei_valid(tx[10]));
  TEST_ASSERT_EQUAL_HEX8(0xC8, tx[11]);  // MP_CLOSE high byte
  TEST_ASSERT_EQUAL_HEX8(0x00, tx[12]);

  // The frame the receiver sees must validate.
  iohome::frame::IoFrame parsed;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(tx.data(), tx.size(), &parsed));
  TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed, SYSTEM_KEY));

  // What actually went on air must be UART-framed (ten bits per byte), not the
  // raw logical frame. Without this, a regression that transmitted the un-framed
  // buffer passed every field check above (the mock falls back to the raw bytes
  // when de-framing fails). A 25-byte frame is 32 wire bytes, and the framed
  // bytes must differ from the logical ones.
  TEST_ASSERT_EQUAL_UINT(iohome::phy::uart_wire_size(25), radio.last_wire.size());
  TEST_ASSERT_EQUAL_UINT(32, radio.last_wire.size());
  TEST_ASSERT_TRUE(radio.last_wire != tx);
}

void test_execute_with_extra_functional_params(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  // Four functional parameters, the shape seen in scripts/io-homecontrol.ksy.
  const uint8_t fps[4] = {0x80, 0xC8, 0x00, 0x00};
  TEST_ASSERT_TRUE(controller.send_execute_fp(PEER_NODE, 0xD400, fps, sizeof(fps)));

  const std::vector<uint8_t>& tx = radio.last_transmission;

  // header(9) + execute payload(8) + seq(2) + mac(6) + crc(2)
  TEST_ASSERT_EQUAL_UINT(27, tx.size());
  TEST_ASSERT_EQUAL_UINT8(27 - 3, tx[0] & iohome::CTRL0_LENGTH_MASK);

  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, tx[8]);
  TEST_ASSERT_EQUAL_HEX8(0xD4, tx[11]);  // main parameter high byte
  TEST_ASSERT_EQUAL_HEX8(0x00, tx[12]);
  TEST_ASSERT_EQUAL_HEX8(0x80, tx[13]);  // FP1
  TEST_ASSERT_EQUAL_HEX8(0xC8, tx[14]);  // FP2
  TEST_ASSERT_EQUAL_HEX8(0x00, tx[15]);  // FP3
  TEST_ASSERT_EQUAL_HEX8(0x00, tx[16]);  // FP4

  // The longer payload must still parse back and authenticate; the parser has
  // to tell eight parameter bytes from parameters plus a trailer.
  iohome::frame::IoFrame parsed;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(tx.data(), tx.size(), &parsed));
  TEST_ASSERT_TRUE(parsed.authenticated);
  TEST_ASSERT_EQUAL_UINT8(8, parsed.data_len);
  TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed, SYSTEM_KEY));

  // Argument checking.
  TEST_ASSERT_FALSE(controller.send_execute_fp(PEER_NODE, 0, nullptr, 2));
  TEST_ASSERT_FALSE(controller.send_execute_fp(PEER_NODE, 0, fps, 1));
  uint8_t too_many[iohome::EXECUTE_MAX_FUNCTIONAL_PARAMS + 1] = {0};
  TEST_ASSERT_FALSE(
      controller.send_execute_fp(PEER_NODE, 0, too_many, sizeof(too_many)));
}

void test_position_mapping(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  // 0 % open is fully closed, which is the maximum wire value.
  TEST_ASSERT_TRUE(controller.set_position(PEER_NODE, 0));
  TEST_ASSERT_EQUAL_HEX8(0xC8, radio.last_transmission[11]);

  // 100 % open is the minimum wire value.
  TEST_ASSERT_TRUE(controller.set_position(PEER_NODE, 100));
  TEST_ASSERT_EQUAL_HEX8(0x00, radio.last_transmission[11]);
  TEST_ASSERT_EQUAL_HEX8(0x00, radio.last_transmission[12]);

  // Half open sits in the middle of the 0x0000..0xC800 range.
  TEST_ASSERT_TRUE(controller.set_position(PEER_NODE, 50));
  const uint16_t main_param = static_cast<uint16_t>(
      (static_cast<uint16_t>(radio.last_transmission[11]) << 8) | radio.last_transmission[12]);
  TEST_ASSERT_EQUAL_HEX16(0x6400, main_param);
}

void test_ventilate_sends_secured_ventilation(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  // The window opener secured-ventilation alias, 0xD803. Confirmed against the
  // KLF 200 spec and rspaargaren/iohomecontrol's Vent button.
  TEST_ASSERT_TRUE(controller.ventilate(PEER_NODE));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, radio.last_transmission[8]);
  const uint16_t mp = static_cast<uint16_t>(
      (static_cast<uint16_t>(radio.last_transmission[11]) << 8) | radio.last_transmission[12]);
  TEST_ASSERT_EQUAL_HEX16(0xD803, mp);

  // Force preset: the observed 0x6400, wire-identical to a 50% position.
  TEST_ASSERT_TRUE(controller.force(PEER_NODE));
  const uint16_t fmp = static_cast<uint16_t>(
      (static_cast<uint16_t>(radio.last_transmission[11]) << 8) | radio.last_transmission[12]);
  TEST_ASSERT_EQUAL_HEX16(0x6400, fmp);
}

void test_rejects_invalid_acei(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  TEST_ASSERT_FALSE(controller.set_acei(0x60));
  TEST_ASSERT_TRUE(controller.set_acei(0x43));

  controller.close(PEER_NODE);
  TEST_ASSERT_EQUAL_HEX8(0x43, radio.last_transmission[10]);
}

void test_set_priority_selects_the_acei_level(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  // set_priority touches only the level bits (7-5), leaving IsValid set. From
  // the 0x61 default, selecting USER_LEVEL_1 (level 2, "High" - the level
  // rspaargaren/iohomecontrol's remote runs at) gives 0x41.
  controller.set_priority(iohome::PriorityLevel::USER_LEVEL_1);
  controller.close(PEER_NODE);
  TEST_ASSERT_EQUAL_HEX8(0x41, radio.last_transmission[10]);

  // Back to the default USER_LEVEL_2 (level 3) = 0x61.
  controller.set_priority(iohome::PriorityLevel::USER_LEVEL_2);
  controller.close(PEER_NODE);
  TEST_ASSERT_EQUAL_HEX8(0x61, radio.last_transmission[10]);

  // set_priority preserves the other ACEI bits: after set_acei(0x43) (which
  // also sets Extended Info = 1), selecting USER_LEVEL_2 keeps that bit and
  // only lifts the level, giving 0x63 rather than a reset-to-default 0x61.
  TEST_ASSERT_TRUE(controller.set_acei(0x43));
  controller.set_priority(iohome::PriorityLevel::USER_LEVEL_2);
  controller.close(PEER_NODE);
  TEST_ASSERT_EQUAL_HEX8(0x63, radio.last_transmission[10]);
}

void test_send_command_rejects_nullptr(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  TEST_ASSERT_FALSE(controller.send_command(nullptr, iohome::CMD_EXECUTE));
}

void test_2w_requires_handshake(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY, false));

  // Without an outstanding challenge there is nothing to bind the MAC to, so
  // the send must fail rather than silently authenticate with a null nonce.
  TEST_ASSERT_FALSE(controller.close(PEER_NODE));
  TEST_ASSERT_EQUAL_UINT(0, radio.transmissions.size());

  TEST_ASSERT_TRUE(controller.send_challenge_request(PEER_NODE));
  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
}

void test_2w_commands_work_after_handshake(void) {
  // Regression: completing the handshake used to invalidate the nonce, so the
  // commands it authorises were refused.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY, false));
  controller.start_receive();

  TEST_ASSERT_TRUE(controller.send_challenge_request(PEER_NODE));

  // Take the challenge the controller just sent and answer it as the peer.
  iohome::frame::IoFrame request;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(radio.last_transmission.data(),
                                              radio.last_transmission.size(), &request,
                                              iohome::frame::AuthTrailer::PRESENT));

  iohome::mode2w::AuthenticationManager peer;
  peer.begin(SYSTEM_KEY);

  iohome::frame::IoFrame response;
  TEST_ASSERT_TRUE(peer.create_challenge_response(&response, OWN_NODE, PEER_NODE, request.data));

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&response, buffer, sizeof(buffer));
  radio.deliver(buffer, len);

  iohome::frame::IoFrame received;
  TEST_ASSERT_TRUE(controller.check_received(&received));
  TEST_ASSERT_EQUAL(iohome::mode2w::ChallengeState::AUTHENTICATED, controller.get_auth_state());

  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
}

void test_accepts_peer_initiated_challenge(void) {
  // A peer opening a 2W handshake carries its own nonce in the payload. If we
  // insisted on verifying against a nonce of ours, we could never answer one.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY, false));
  controller.start_receive();

  iohome::mode2w::AuthenticationManager peer;
  peer.begin(SYSTEM_KEY);

  iohome::frame::IoFrame request;
  TEST_ASSERT_TRUE(peer.create_challenge_request(&request, OWN_NODE, PEER_NODE, 0));

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&request, buffer, sizeof(buffer));
  radio.deliver(buffer, len);

  iohome::frame::IoFrame received;
  TEST_ASSERT_TRUE(controller.check_received(&received));
  TEST_ASSERT_EQUAL(RxReject::NONE, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, received.command_id);

  // The nonce is available to answer with.
  TEST_ASSERT_EQUAL_UINT8(iohome::HMAC_SIZE, received.data_len);
  TEST_ASSERT_TRUE(controller.send_challenge_response(PEER_NODE, received.data));
}

void test_rejects_forged_peer_challenge(void) {
  // ...but it still has to be signed with the system key.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY, false));
  controller.start_receive();

  const uint8_t attacker_key[16] = {0xBA, 0xDD};
  iohome::mode2w::AuthenticationManager attacker;
  attacker.begin(attacker_key);

  iohome::frame::IoFrame request;
  TEST_ASSERT_TRUE(attacker.create_challenge_request(&request, OWN_NODE, PEER_NODE, 0));

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&request, buffer, sizeof(buffer));
  radio.deliver(buffer, len);

  iohome::frame::IoFrame received;
  TEST_ASSERT_FALSE(controller.check_received(&received));
  TEST_ASSERT_EQUAL(RxReject::MAC, controller.last_reject_reason());
}

void test_stray_challenge_response_does_not_break_the_session(void) {
  // A 0x3D from a node we never challenged must not disturb a handshake in
  // progress. Two things stop it here, and this pins the order of both: the
  // receive gate rejects the frame on its MAC before the authentication
  // manager is offered it at all, and the manager would ignore it anyway
  // because it is not from the peer we challenged.
  //
  // The ESPHome component had only the second of those. It routed 0x3C/0x3D
  // straight to the manager without waiting for the MAC verdict, so any second
  // io-homecontrol system in radio range cancelled its handshakes by accident,
  // and anyone with a radio could on purpose.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY, false));
  controller.start_receive();

  TEST_ASSERT_TRUE(controller.send_challenge_request(PEER_NODE));

  // Recover the nonce we just sent, so the peer can answer it properly later.
  iohome::frame::IoFrame sent;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(radio.last_transmission.data(),
                                              radio.last_transmission.size(), &sent));
  uint8_t nonce[iohome::HMAC_SIZE];
  memcpy(nonce, sent.data, iohome::HMAC_SIZE);

  // A stranger's 0x3D, properly signed - for its own conversation.
  const uint8_t STRANGER[3] = {0xEE, 0xEE, 0xEE};
  iohome::mode2w::AuthenticationManager other;
  other.begin(SYSTEM_KEY);
  const uint8_t other_nonce[iohome::HMAC_SIZE] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
  iohome::frame::IoFrame stray;
  TEST_ASSERT_TRUE(other.create_challenge_response(&stray, STRANGER, STRANGER, other_nonce));

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  size_t len = iohome::frame::serialize_frame(&stray, buffer, sizeof(buffer));
  radio.deliver(buffer, len);
  iohome::frame::IoFrame received;
  TEST_ASSERT_FALSE(controller.check_received(&received));
  TEST_ASSERT_EQUAL(RxReject::MAC, controller.last_reject_reason());

  // The genuine answer still completes the handshake.
  iohome::mode2w::AuthenticationManager peer;
  peer.begin(SYSTEM_KEY);
  iohome::frame::IoFrame response;
  TEST_ASSERT_TRUE(peer.create_challenge_response(&response, OWN_NODE, PEER_NODE, nonce));
  len = iohome::frame::serialize_frame(&response, buffer, sizeof(buffer));
  radio.deliver(buffer, len);
  TEST_ASSERT_TRUE(controller.check_received(&received));

  // Which is what a 2W command needs: without a live session it is refused.
  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
}

// ---------------------------------------------------------------------------
// 2W pairing: push a key from one controller to another, end to end
// ---------------------------------------------------------------------------

namespace {
struct KeyCapture {
  bool got = false;
  uint8_t key[16] = {0};
  uint8_t from[3] = {0};
};
void key_cb(const uint8_t key[16], const uint8_t from[3], void* ctx) {
  auto* c = static_cast<KeyCapture*>(ctx);
  c->got = true;
  memcpy(c->key, key, 16);
  memcpy(c->from, from, 3);
}

/// Carry whatever `from` just transmitted to `to`, and let `to` process it.
/// Returns the command ID that moved, or 0xFF if nothing was sent.
uint8_t relay(PhysicalLayer& from_radio, IoHomeControl& to, PhysicalLayer& to_radio) {
  if (from_radio.last_transmission.empty()) {
    return 0xFF;
  }
  std::vector<uint8_t> frame = from_radio.last_transmission;
  from_radio.last_transmission.clear();
  const uint8_t cmd = frame.size() > 8 ? frame[8] : 0xFF;
  to_radio.deliver(frame.data(), frame.size());
  iohome::frame::IoFrame received;
  to.check_received(&received);
  return cmd;
}

/// Read back which system key `node` currently holds: deliver a 0x3C signed with
/// `key` (a 0x3C validates under the receiver's own system key) and return
/// whether the node accepts it. `own` is the node's address, `peer` the sender.
bool speaks_key(IoHomeControl& node, PhysicalLayer& radio,
                const uint8_t own[3], const uint8_t peer[3], const uint8_t key[16]) {
  iohome::mode2w::AuthenticationManager signer;
  signer.begin(key);
  iohome::frame::IoFrame chal;
  if (!signer.create_challenge_request(&chal, own, peer, 1000UL)) {
    return false;
  }
  uint8_t buf[iohome::FRAME_MAX_SIZE];
  const size_t n = iohome::frame::serialize_frame(&chal, buf, sizeof(buf));
  radio.deliver(buf, n);
  iohome::frame::IoFrame got;
  return node.check_received(&got);
}
}  // namespace

void test_2w_push_pairing_transfers_the_key(void) {
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t NO_KEY[16] = {0};

  PhysicalLayer ctrl_radio, dev_radio;
  IoHomeControl controller(&ctrl_radio);
  IoHomeControl device(&dev_radio);

  TEST_ASSERT_TRUE(controller.begin(CTRL, STACK_KEY, /*is_1w=*/false));
  TEST_ASSERT_TRUE(device.begin(DEV, NO_KEY, /*is_1w=*/false));
  controller.start_receive();
  device.start_receive();

  KeyCapture cap;
  device.set_key_received_callback(key_cb, &cap);
  device.set_accept_pairing(true);

  // Step 1: the controller asks for a challenge. The very first frame it sends
  // must be 0x31 - not a 0x32 fired blind, which was the K9 bug.
  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, STACK_KEY));
  TEST_ASSERT_TRUE(controller.is_pairing());
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_ASK_CHALLENGE, ctrl_radio.last_transmission[8]);

  // 0x31 -> device answers 0x3C.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_ASK_CHALLENGE, relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, dev_radio.last_transmission[8]);

  // 0x3C -> controller sends the key transfer 0x32.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, relay(dev_radio, controller, ctrl_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, ctrl_radio.last_transmission[8]);

  // 0x32 -> device recovers the key, adopts it, and acknowledges with 0x33.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_TRUE(cap.got);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(STACK_KEY, cap.key, 16);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(CTRL, cap.from, 3);
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER_ACK, dev_radio.last_transmission[8]);

  // 0x33 -> controller finishes.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER_ACK, relay(dev_radio, controller, ctrl_radio));
  TEST_ASSERT_FALSE(controller.is_pairing());

  // The recovered key matching the pushed key byte for byte (asserted above via
  // the callback) is the proof the transfer worked: the device unmasked exactly
  // what the controller sent, using the challenge it had itself chosen.

  // ...but the callback only shows the device *recovered* the key; a
  // set_system_key() that dropped it on the floor would pass that check too.
  // Prove the device actually STORED it: an independent signer holding STACK_KEY
  // builds a challenge request, and the device validates its 0x3C MAC under its
  // own stored key - so it accepts this only if that key really is STACK_KEY.
  iohome::mode2w::AuthenticationManager signer;
  signer.begin(STACK_KEY);
  iohome::frame::IoFrame chal;
  TEST_ASSERT_TRUE(signer.create_challenge_request(&chal, DEV, CTRL, 1000UL));
  uint8_t buf[iohome::FRAME_MAX_SIZE];
  size_t n = iohome::frame::serialize_frame(&chal, buf, sizeof(buf));
  dev_radio.deliver(buf, n);
  iohome::frame::IoFrame got;
  TEST_ASSERT_TRUE(device.check_received(&got));  // accepted under the adopted key

  // Negative control: the same request signed under a different key is rejected,
  // so the accept above is the adopted key at work, not an open door.
  const uint8_t OTHER_KEY[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF,
                                 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF};
  iohome::mode2w::AuthenticationManager wrong;
  wrong.begin(OTHER_KEY);
  iohome::frame::IoFrame chal2;
  TEST_ASSERT_TRUE(wrong.create_challenge_request(&chal2, DEV, CTRL, 2000UL));
  n = iohome::frame::serialize_frame(&chal2, buf, sizeof(buf));
  dev_radio.deliver(buf, n);
  TEST_ASSERT_FALSE(device.check_received(&got));
  TEST_ASSERT_EQUAL(RxReject::MAC, device.last_reject_reason());
}

void test_2w_pairing_ignores_a_stranger_challenge(void) {
  // While a controller waits for its peer's 0x3C, a 0x3C from a DIFFERENT node
  // must not drive the exchange. The handler's `from_peer` guard is what stops
  // a stranger from injecting the challenge the controller masks its key
  // against; without it the controller would fire its 0x32 - the masked system
  // key - in response to any node's challenge.
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t STRANGER[3] = {0x11, 0x22, 0x33};
  const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

  PhysicalLayer ctrl_radio;
  IoHomeControl controller(&ctrl_radio);
  TEST_ASSERT_TRUE(controller.begin(CTRL, STACK_KEY, /*is_1w=*/false));
  controller.start_receive();

  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, STACK_KEY));
  TEST_ASSERT_TRUE(controller.is_pairing());
  ctrl_radio.last_transmission.clear();  // drop the 0x31 we just sent

  // A stranger's 0x3C, addressed to us. Its MAC is irrelevant - pairing frames
  // bypass the MAC gate, so only the from_peer check stands between it and our
  // key transfer.
  iohome::mode2w::AuthenticationManager stranger;
  stranger.begin(STACK_KEY);
  iohome::frame::IoFrame stray;
  TEST_ASSERT_TRUE(stranger.create_challenge_request(&stray, CTRL, STRANGER, 1000UL));
  uint8_t buf[iohome::FRAME_MAX_SIZE];
  const size_t n = iohome::frame::serialize_frame(&stray, buf, sizeof(buf));
  ctrl_radio.deliver(buf, n);
  iohome::frame::IoFrame got;
  controller.check_received(&got);

  // No key transfer went out, and we are still waiting for the real peer.
  TEST_ASSERT_TRUE(ctrl_radio.last_transmission.empty());
  TEST_ASSERT_TRUE(controller.is_pairing());
}

void test_pairing_expires_after_timeout(void) {
  // A stalled push handshake must reset to IDLE after PAIRING_TIMEOUT_MS (5 s) so
  // it stops diverting unrelated 0x3C/0x32 frames from the MAC gate. The timeout
  // reads a clock a host cannot advance (clock()), so pin it via the test seam.
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(CTRL, STACK_KEY, /*is_1w=*/false));
  controller.start_receive();

  controller.set_pairing_clock_ms(1000);
  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, STACK_KEY));  // PUSH_WAIT_CHALLENGE
  TEST_ASSERT_TRUE(controller.is_pairing());
  radio.last_transmission.clear();  // drop the 0x31

  // Jump well past the 5 s timeout, then deliver the peer's genuine 0x3C. Because
  // the handshake has expired, check_received() resets it to IDLE *before*
  // routing, so this 0x3C is no longer the awaited challenge - it goes through
  // the normal gate and no key transfer is emitted. Were the timeout broken, the
  // controller would instead treat it as the challenge, send its 0x32, and stay
  // pairing - so is_pairing()==false here is exactly what the timeout buys.
  controller.set_pairing_clock_ms(1000 + 6000);

  iohome::mode2w::AuthenticationManager signer;
  signer.begin(STACK_KEY);
  iohome::frame::IoFrame chal;
  TEST_ASSERT_TRUE(signer.create_challenge_request(&chal, CTRL, DEV, 1000UL));
  uint8_t buf[iohome::FRAME_MAX_SIZE];
  const size_t n = iohome::frame::serialize_frame(&chal, buf, sizeof(buf));
  radio.deliver(buf, n);
  iohome::frame::IoFrame got;
  controller.check_received(&got);

  TEST_ASSERT_FALSE(controller.is_pairing());          // handshake abandoned
  TEST_ASSERT_TRUE(radio.last_transmission.empty());   // no 0x32 emitted
}

void test_pairing_rejects_truncated_frames(void) {
  // Pairing frames are CRC-checked but NOT MAC-checked, so their contents are
  // attacker-controlled. The data_len guards in handle_pairing_frame must reject
  // a CRC-valid frame with a truncated data field without emitting a key. The
  // guard is verified by the absence-of-transmission assertions below, not by a
  // memory error: IoFrame::data is a fixed-size array, so dropping the guard
  // would build a key transfer from stale buffer bytes rather than read out of
  // bounds - a silent wrong-frame, which the "nothing transmitted" checks catch.
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t STACK_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  uint8_t buf[iohome::FRAME_MAX_SIZE];

  // Controller waiting for its challenge: a 0x3C from the peer with < HMAC_SIZE
  // of data must be dropped, not turned into a key transfer.
  {
    PhysicalLayer ctrl_radio;
    IoHomeControl controller(&ctrl_radio);
    TEST_ASSERT_TRUE(controller.begin(CTRL, STACK_KEY, /*is_1w=*/false));
    controller.start_receive();
    TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, STACK_KEY));
    ctrl_radio.last_transmission.clear();

    iohome::frame::IoFrame chal;
    iohome::frame::init_frame(&chal, false);
    iohome::frame::set_destination(&chal, CTRL);
    iohome::frame::set_source(&chal, DEV);
    const uint8_t tiny[3] = {0x11, 0x22, 0x33};  // shorter than HMAC_SIZE
    TEST_ASSERT_TRUE(
        iohome::frame::set_command(&chal, iohome::CMD_CHALLENGE_REQUEST, tiny, sizeof(tiny)));
    iohome::frame::finalize_frame_plain(&chal);
    const size_t n = iohome::frame::serialize_frame(&chal, buf, sizeof(buf));
    ctrl_radio.deliver(buf, n);
    iohome::frame::IoFrame got;
    controller.check_received(&got);

    TEST_ASSERT_TRUE(ctrl_radio.last_transmission.empty());  // no 0x32 built from a short nonce
    TEST_ASSERT_TRUE(controller.is_pairing());               // state untouched
  }

  // Accept-pairing device: a 0x38 pull request with < HMAC_SIZE of data must not
  // produce a key transfer.
  {
    PhysicalLayer dev_radio;
    IoHomeControl device(&dev_radio);
    TEST_ASSERT_TRUE(device.begin(DEV, STACK_KEY, /*is_1w=*/false));
    device.set_accept_pairing(true);
    device.start_receive();

    iohome::frame::IoFrame launch;
    iohome::frame::init_frame(&launch, false);
    iohome::frame::set_destination(&launch, DEV);
    iohome::frame::set_source(&launch, CTRL);
    const uint8_t tiny[2] = {0x44, 0x55};  // shorter than HMAC_SIZE
    TEST_ASSERT_TRUE(
        iohome::frame::set_command(&launch, iohome::CMD_LAUNCH_KEY_TRANSFER, tiny, sizeof(tiny)));
    iohome::frame::finalize_frame_plain(&launch);
    const size_t n = iohome::frame::serialize_frame(&launch, buf, sizeof(buf));
    dev_radio.deliver(buf, n);
    iohome::frame::IoFrame got;
    device.check_received(&got);

    TEST_ASSERT_TRUE(dev_radio.last_transmission.empty());  // no key leaked
  }
}

void test_2w_push_adopts_new_key_only_after_ack(void) {
  // The controller must adopt the pushed key at the 0x33 ACK, not when it sends
  // the 0x32 - so a lost 0x32/0x33 leaves it on its old key and a retry recovers.
  // Push a key DISTINCT from the controller's initial key so the two are
  // distinguishable, and read back the controller's key at each step.
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t OLD_KEY[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                               0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
  const uint8_t NEW_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                               0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t NO_KEY[16] = {0};

  PhysicalLayer ctrl_radio, dev_radio;
  IoHomeControl controller(&ctrl_radio);
  IoHomeControl device(&dev_radio);
  TEST_ASSERT_TRUE(controller.begin(CTRL, OLD_KEY, /*is_1w=*/false));
  TEST_ASSERT_TRUE(device.begin(DEV, NO_KEY, /*is_1w=*/false));
  controller.start_receive();
  device.start_receive();
  device.set_accept_pairing(true);

  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, NEW_KEY));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_ASK_CHALLENGE, relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, relay(dev_radio, controller, ctrl_radio));

  // 0x32 is sent, 0x33 not yet received: the controller must STILL be on OLD_KEY.
  TEST_ASSERT_TRUE(controller.is_pairing());
  TEST_ASSERT_TRUE(speaks_key(controller, ctrl_radio, CTRL, DEV, OLD_KEY));
  TEST_ASSERT_FALSE(speaks_key(controller, ctrl_radio, CTRL, DEV, NEW_KEY));

  // Deliver the 0x32 to the device (it adopts NEW_KEY and acks), then the 0x33
  // to the controller - only now does the controller adopt NEW_KEY.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER_ACK, relay(dev_radio, controller, ctrl_radio));

  TEST_ASSERT_FALSE(controller.is_pairing());
  TEST_ASSERT_TRUE(speaks_key(controller, ctrl_radio, CTRL, DEV, NEW_KEY));
  TEST_ASSERT_FALSE(speaks_key(controller, ctrl_radio, CTRL, DEV, OLD_KEY));
}

void test_2w_push_transmit_failure_keeps_old_key(void) {
  // If the 0x32 key transfer cannot be sent, the controller must abandon the
  // handshake and stay on its old key (not switch to a key the device never got).
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t OLD_KEY[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                               0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
  const uint8_t NEW_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                               0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t NO_KEY[16] = {0};

  PhysicalLayer ctrl_radio, dev_radio;
  IoHomeControl controller(&ctrl_radio);
  IoHomeControl device(&dev_radio);
  TEST_ASSERT_TRUE(controller.begin(CTRL, OLD_KEY, /*is_1w=*/false));
  TEST_ASSERT_TRUE(device.begin(DEV, NO_KEY, /*is_1w=*/false));
  controller.start_receive();
  device.start_receive();
  device.set_accept_pairing(true);

  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, NEW_KEY));  // 0x31 sends fine
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_ASK_CHALLENGE, relay(ctrl_radio, device, dev_radio));

  // Make the controller's radio fail the next transmit - the 0x32.
  ctrl_radio.transmit_result = RADIOLIB_ERR_TX_TIMEOUT;
  relay(dev_radio, controller, ctrl_radio);  // 0x3C in; the 0x32 fails to send

  // The handshake is abandoned and the old key is retained.
  TEST_ASSERT_FALSE(controller.is_pairing());
  ctrl_radio.transmit_result = RADIOLIB_ERR_NONE;
  TEST_ASSERT_TRUE(speaks_key(controller, ctrl_radio, CTRL, DEV, OLD_KEY));
  TEST_ASSERT_FALSE(speaks_key(controller, ctrl_radio, CTRL, DEV, NEW_KEY));
}

void test_initiator_ignores_stranger_pairing_requests(void) {
  // A node that is BOTH accept_pairing_ AND mid-handshake as an initiator must
  // not let a third node's plain 0x31/0x38 hijack pairing_peer_. The follower
  // section is gated on pairing_state_ == IDLE for exactly this reason; without
  // it, a stranger's 0x31 overwrites pairing_peer_ and stalls the real push.
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t STRANGER[3] = {0x11, 0x22, 0x33};
  const uint8_t OLD_KEY[16] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
                               0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F};
  const uint8_t NEW_KEY[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                               0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
  const uint8_t NO_KEY[16] = {0};

  PhysicalLayer ctrl_radio, dev_radio;
  IoHomeControl controller(&ctrl_radio);
  IoHomeControl device(&dev_radio);
  TEST_ASSERT_TRUE(controller.begin(CTRL, OLD_KEY, /*is_1w=*/false));
  TEST_ASSERT_TRUE(device.begin(DEV, NO_KEY, /*is_1w=*/false));
  controller.start_receive();
  device.start_receive();
  controller.set_accept_pairing(true);  // the initiator ALSO accepts pairing
  device.set_accept_pairing(true);

  TEST_ASSERT_TRUE(controller.pair_device_2w(DEV, NEW_KEY));   // PUSH_WAIT_CHALLENGE, peer=DEV
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_ASK_CHALLENGE, relay(ctrl_radio, device, dev_radio));

  // A stranger injects a plain ask-challenge while we are mid-push. It must be
  // ignored (follower section gated on IDLE), leaving pairing_peer_ == DEV.
  ctrl_radio.last_transmission.clear();
  iohome::frame::IoFrame ask;
  iohome::frame::init_frame(&ask, false);
  iohome::frame::set_destination(&ask, CTRL);
  iohome::frame::set_source(&ask, STRANGER);
  iohome::frame::set_command(&ask, iohome::CMD_ASK_CHALLENGE, nullptr, 0);
  iohome::frame::finalize_frame_plain(&ask);
  uint8_t buf[iohome::FRAME_MAX_SIZE];
  const size_t n = iohome::frame::serialize_frame(&ask, buf, sizeof(buf));
  ctrl_radio.deliver(buf, n);
  iohome::frame::IoFrame got;
  controller.check_received(&got);
  TEST_ASSERT_TRUE(ctrl_radio.last_transmission.empty());  // no challenge answered
  TEST_ASSERT_TRUE(controller.is_pairing());               // still waiting for DEV

  // The real DEV's 0x3C still completes the push - proof pairing_peer_ was intact.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_CHALLENGE_REQUEST, relay(dev_radio, controller, ctrl_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER_ACK, relay(dev_radio, controller, ctrl_radio));
  TEST_ASSERT_FALSE(controller.is_pairing());
  TEST_ASSERT_TRUE(speaks_key(controller, ctrl_radio, CTRL, DEV, NEW_KEY));
}

void test_2w_follower_rejects_unsolicited_key_transfer(void) {
  // SECURITY: a fresh accept-pairing 2W follower must NOT adopt a key from an
  // UNSOLICITED 0x32. Before the guards, an attacker could send one CRC-valid
  // 0x32 from src 00:00:00 - matching the zeroed default pairing_peer_ - masked
  // with the zero default challenge, and the follower would adopt an
  // attacker-chosen system key with no 0x31/0x3C handshake ever occurring.
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t GROUP[3] = {0x00, 0x00, 0x00};
  const uint8_t SYS[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                           0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};

  PhysicalLayer dev_radio;
  IoHomeControl device(&dev_radio);
  TEST_ASSERT_TRUE(device.begin(DEV, SYS, /*is_1w=*/false));
  device.set_accept_pairing(true);
  KeyCapture cap;
  device.set_key_received_callback(key_cb, &cap);
  device.start_receive();

  auto forged_key_transfer = [&](const uint8_t src[3]) {
    iohome::frame::IoFrame kt;
    iohome::frame::init_frame(&kt, false);
    iohome::frame::set_destination(&kt, DEV);
    iohome::frame::set_source(&kt, src);
    uint8_t payload[20];  // >= AES_KEY_SIZE; attacker-controlled bytes
    for (size_t i = 0; i < sizeof(payload); i++) payload[i] = 0xAB;
    iohome::frame::set_command(&kt, iohome::CMD_KEY_TRANSFER, payload, sizeof(payload));
    iohome::frame::finalize_frame_plain(&kt);
    uint8_t buf[iohome::FRAME_MAX_SIZE];
    const size_t n = iohome::frame::serialize_frame(&kt, buf, sizeof(buf));
    dev_radio.reset();
    dev_radio.deliver(buf, n);
    iohome::frame::IoFrame got;
    device.check_received(&got);
  };

  // From the group address (matches the zeroed pairing_peer_): must be rejected.
  forged_key_transfer(GROUP);
  TEST_ASSERT_TRUE(dev_radio.last_transmission.empty());  // no 0x33 ack -> not adopted
  TEST_ASSERT_FALSE(cap.got);                              // no key surfaced/adopted

  // From an arbitrary non-peer address (from_peer is false anyway): also rejected.
  forged_key_transfer(CTRL);
  TEST_ASSERT_TRUE(dev_radio.last_transmission.empty());
  TEST_ASSERT_FALSE(cap.got);

  // The device still authenticates under its ORIGINAL key - nothing was adopted.
  TEST_ASSERT_TRUE(speaks_key(device, dev_radio, DEV, CTRL, SYS));
}

void test_2w_pull_collects_the_device_key(void) {
  const uint8_t CTRL[3] = {0xF0, 0x0F, 0x00};
  const uint8_t DEV[3] = {0xFE, 0xEF, 0xEE};
  const uint8_t DEVICE_KEY[16] = {0xAB, 0xCD, 0xEF, 0x01, 0x02, 0x03, 0x04, 0x05,
                                  0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13};
  const uint8_t CTRL_KEY[16] = {0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22,
                                0x11, 0x00, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

  PhysicalLayer ctrl_radio, dev_radio;
  IoHomeControl controller(&ctrl_radio);
  IoHomeControl device(&dev_radio);

  TEST_ASSERT_TRUE(controller.begin(CTRL, CTRL_KEY, /*is_1w=*/false));
  TEST_ASSERT_TRUE(device.begin(DEV, DEVICE_KEY, /*is_1w=*/false));
  controller.start_receive();
  device.start_receive();

  KeyCapture cap;
  controller.set_key_received_callback(key_cb, &cap);
  device.set_accept_pairing(true);

  // The controller launches the pull; its first frame is 0x38, not 0x32.
  TEST_ASSERT_TRUE(controller.pull_device_key_2w(DEV));
  TEST_ASSERT_TRUE(controller.is_pairing());
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_LAUNCH_KEY_TRANSFER, ctrl_radio.last_transmission[8]);

  // 0x38 -> device answers with its key in a 0x32.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_LAUNCH_KEY_TRANSFER,
                         relay(ctrl_radio, device, dev_radio));
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, dev_radio.last_transmission[8]);

  // 0x32 -> controller recovers the device's key.
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_KEY_TRANSFER, relay(dev_radio, controller, ctrl_radio));
  TEST_ASSERT_TRUE(cap.got);
  // The surfaced key is the device's own, not the controller's - proof that
  // pull collected the device's key and did not adopt it.
  TEST_ASSERT_EQUAL_UINT8_ARRAY(DEVICE_KEY, cap.key, 16);
  TEST_ASSERT_TRUE(memcmp(cap.key, CTRL_KEY, 16) != 0);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(DEV, cap.from, 3);
  TEST_ASSERT_FALSE(controller.is_pairing());
}

void test_2w_pairing_is_off_by_default(void) {
  // A node that has not opted in must ignore an ask-challenge, so a stranger
  // cannot make it emit a challenge or accept a key.
  PhysicalLayer radio;
  IoHomeControl device(&radio);
  TEST_ASSERT_TRUE(device.begin(OWN_NODE, SYSTEM_KEY, /*is_1w=*/false));
  device.start_receive();

  iohome::frame::IoFrame ask;
  iohome::frame::init_frame(&ask, false);
  iohome::frame::set_destination(&ask, OWN_NODE);
  iohome::frame::set_source(&ask, PEER_NODE);
  iohome::frame::set_command(&ask, iohome::CMD_ASK_CHALLENGE, nullptr, 0);
  iohome::frame::finalize_frame_plain(&ask);

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&ask, buffer, sizeof(buffer));
  radio.reset();
  radio.deliver(buffer, len);

  iohome::frame::IoFrame received;
  device.check_received(&received);
  // Nothing sent back.
  TEST_ASSERT_TRUE(radio.last_transmission.empty());
}

void test_1w_node_ignores_pairing_frames_without_crashing(void) {
  // A node begun in 1W mode has no auth_manager_/discovery_manager_. If it also
  // has accept_pairing_ set, an unauthenticated ask-challenge (0x31) must NOT be
  // routed into the 2W pairing machine - doing so dereferenced a null manager
  // and crashed the node. A single such frame (broadcast or addressed) could
  // take down every 1W node. The frame must be a harmless no-op instead.
  PhysicalLayer radio;
  IoHomeControl device(&radio);
  TEST_ASSERT_TRUE(device.begin(OWN_NODE, SYSTEM_KEY, /*is_1w=*/true));
  device.set_accept_pairing(true);
  device.start_receive();

  iohome::frame::IoFrame ask;
  iohome::frame::init_frame(&ask, false);
  iohome::frame::set_destination(&ask, OWN_NODE);
  iohome::frame::set_source(&ask, PEER_NODE);
  iohome::frame::set_command(&ask, iohome::CMD_ASK_CHALLENGE, nullptr, 0);
  iohome::frame::finalize_frame_plain(&ask);

  uint8_t buffer[iohome::FRAME_MAX_SIZE];
  const size_t len = iohome::frame::serialize_frame(&ask, buffer, sizeof(buffer));
  radio.reset();
  radio.deliver(buffer, len);

  iohome::frame::IoFrame received;
  device.check_received(&received);  // must not crash

  TEST_ASSERT_FALSE(device.is_pairing());
  TEST_ASSERT_TRUE(radio.last_transmission.empty());  // no challenge emitted
}

// ---------------------------------------------------------------------------
// Rolling code persistence
// ---------------------------------------------------------------------------

void test_rolling_code_increments(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  TEST_ASSERT_EQUAL_UINT16(0, controller.get_rolling_code());
  controller.close(PEER_NODE);
  TEST_ASSERT_EQUAL_UINT16(1, controller.get_rolling_code());
  controller.open(PEER_NODE);
  TEST_ASSERT_EQUAL_UINT16(2, controller.get_rolling_code());
}

void test_rolling_code_writes_are_batched(void) {
  PhysicalLayer radio;
  CountingStore store;
  IoHomeControl controller(&radio);

  controller.set_rolling_code_store(&store, 8);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  // begin() reserves the first block.
  TEST_ASSERT_EQUAL_INT(1, store.saves);
  TEST_ASSERT_EQUAL_UINT16(8, store.stored);

  // Regression: the counter used to be written on every single command, which
  // wears out the NVS partition. Eight commands must fit in one block.
  for (int i = 0; i < 8; i++) {
    controller.close(PEER_NODE);
  }
  TEST_ASSERT_EQUAL_INT(2, store.saves);
  TEST_ASSERT_EQUAL_UINT16(16, store.stored);
}

void test_rolling_code_resumes_past_reservation(void) {
  PhysicalLayer radio;
  CountingStore store;

  {
    IoHomeControl controller(&radio);
    controller.set_rolling_code_store(&store, 8);
    controller.begin(OWN_NODE, SYSTEM_KEY);
    controller.close(PEER_NODE);  // uses code 0
  }

  // A reboot must resume at or past every code that could have been sent, so a
  // receiver never sees a sequence number it has already accepted.
  IoHomeControl restarted(&radio);
  restarted.set_rolling_code_store(&store, 8);
  restarted.begin(OWN_NODE, SYSTEM_KEY);

  TEST_ASSERT_GREATER_OR_EQUAL(1, restarted.get_rolling_code());
  TEST_ASSERT_EQUAL_UINT16(8, restarted.get_rolling_code());
}

// ---------------------------------------------------------------------------
// Receive policy
// ---------------------------------------------------------------------------

void test_accepts_valid_authenticated_frame(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  TEST_ASSERT_EQUAL_INT(RADIOLIB_ERR_NONE, controller.start_receive());

  const std::vector<uint8_t> tx = make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 1, SYSTEM_KEY);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  int16_t rssi = 0;
  TEST_ASSERT_TRUE(controller.check_received(&frame, &rssi));

  TEST_ASSERT_EQUAL(RxReject::NONE, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().accepted);
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_EXECUTE, frame.command_id);
  TEST_ASSERT_EQUAL_INT(-75, rssi);
}

void test_rejects_corrupted_frame(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  std::vector<uint8_t> tx = make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 1, SYSTEM_KEY);
  tx[11] ^= 0x01;  // flip a payload bit, leaving the CRC stale
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL(RxReject::CRC, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().crc_failures);
}

void test_rejects_frame_signed_with_wrong_key(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  const uint8_t attacker_key[16] = {0xDE, 0xAD};
  const std::vector<uint8_t> tx =
      make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 1, attacker_key);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL(RxReject::MAC, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().mac_failures);
}

void test_rejects_replayed_frame(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  const std::vector<uint8_t> tx = make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 5, SYSTEM_KEY);

  iohome::frame::IoFrame frame;

  radio.deliver(tx.data(), tx.size());
  TEST_ASSERT_TRUE(controller.check_received(&frame));

  // The MAC on a recorded frame stays valid forever. Only the rolling-code
  // check stops it from being replayed off the air.
  radio.deliver(tx.data(), tx.size());
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL(RxReject::REPLAY, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().replays);
}

void test_accepts_advancing_sequence(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  iohome::frame::IoFrame frame;
  for (uint16_t seq = 1; seq <= 4; seq++) {
    const std::vector<uint8_t> tx =
        make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, seq, SYSTEM_KEY);
    radio.deliver(tx.data(), tx.size());
    TEST_ASSERT_TRUE(controller.check_received(&frame));
  }

  TEST_ASSERT_EQUAL_UINT32(4, controller.rx_stats().accepted);
  TEST_ASSERT_EQUAL_UINT32(0, controller.rx_stats().replays);
}

void test_rejects_unauthenticated_command_frame(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  // A plain execute frame carries no MAC at all - accepting it would let
  // anyone drive the actuator.
  const std::vector<uint8_t> tx =
      make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 0, SYSTEM_KEY, false);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL(RxReject::UNAUTHENTICATED, controller.last_reject_reason());
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().unauthenticated);
}

void test_accepts_plain_bootstrap_command(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  // Discovery answers legitimately carry no MAC: no key has been agreed yet.
  const std::vector<uint8_t> tx =
      make_frame(iohome::CMD_DISCOVER_ANSWER, 0, 0, SYSTEM_KEY, false);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL(RxReject::NONE, controller.last_reject_reason());
}

void test_plain_frames_can_be_allowed_explicitly(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.set_accept_plain_frames(true);
  controller.start_receive();

  const std::vector<uint8_t> tx =
      make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 0, SYSTEM_KEY, false);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  TEST_ASSERT_TRUE(controller.check_received(&frame));
}

void test_rejects_garbage(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  const uint8_t noise[14] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                             0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  radio.deliver(noise, sizeof(noise));

  iohome::frame::IoFrame frame;
  TEST_ASSERT_FALSE(controller.check_received(&frame));
}

void test_check_received_needs_a_packet(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  iohome::frame::IoFrame frame;
  // No interrupt has fired, so there is nothing to read.
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_FALSE(controller.check_received(nullptr));
}

namespace {
struct SnifferCapture {
  std::vector<std::vector<uint8_t>> frames;
  int16_t last_rssi = 0;
  int calls = 0;
};

void sniffer_cb(const uint8_t *data, size_t len, int16_t rssi, float snr, void *ctx) {
  (void) snr;
  auto *capture = static_cast<SnifferCapture *>(ctx);
  capture->frames.emplace_back(data, data + len);
  capture->last_rssi = rssi;
  capture->calls++;
}
}  // namespace

namespace {
/// Stands in for a concrete RadioLib class: only fixedPacketLengthMode matters.
struct FakeRadio {
  int16_t fixedPacketLengthMode(uint8_t len) {
    lengths.push_back(len);
    return RADIOLIB_ERR_NONE;
  }
  std::vector<uint8_t> lengths;
};
}  // namespace

void test_packet_length_helper_programs_each_frame(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  // The hand-written form needs a lambda that casts void* back to the radio
  // type, naming it twice. This deduces it, so the cast cannot be wrong.
  FakeRadio fake;
  controller.use_radio_packet_length(fake);

  TEST_ASSERT_TRUE(controller.close(PEER_NODE));

  // Narrowed to this frame's *wire* length before sending, widened again after.
  // A 25-byte frame is 32 wire bytes (ten bits each), and reception widens back
  // to the full capture size - not the frame maximum, since framed frames are
  // longer than 34.
  // Assert the literal wire sizes, not uart_wire_size() on both sides of the
  // comparison (which would agree with itself even if that helper were wrong).
  TEST_ASSERT_EQUAL_UINT(2, fake.lengths.size());
  TEST_ASSERT_EQUAL_UINT8(32, fake.lengths[0]);  // == uart_wire_size(25)
  TEST_ASSERT_EQUAL_UINT8(64, fake.lengths[1]);

  // A longer frame programs a different length, not a constant.
  const uint8_t fps[4] = {0x80, 0xC8, 0x00, 0x00};
  TEST_ASSERT_TRUE(controller.send_execute_fp(PEER_NODE, 0xD400, fps, sizeof(fps)));
  TEST_ASSERT_EQUAL_UINT(4, fake.lengths.size());
  TEST_ASSERT_EQUAL_UINT8(34, fake.lengths[2]);  // == uart_wire_size(27)
  TEST_ASSERT_EQUAL_UINT8(64, fake.lengths[3]);
}

void test_raw_sniffer_sees_every_packet(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));

  SnifferCapture capture;
  controller.set_raw_frame_callback(sniffer_cb, &capture);
  TEST_ASSERT_EQUAL_INT(RADIOLIB_ERR_NONE, controller.start_receive());

  // The sniffer sees the raw on-air bytes, framing and all, so de-frame them
  // to compare against the logical frame the library sent.
  auto deframed = [](const std::vector<uint8_t>& wire) {
    uint8_t out[iohome::FRAME_MAX_SIZE];
    const size_t n = iohome::phy::uart_decode_frame(wire.data(), wire.size(), out, sizeof out);
    return std::vector<uint8_t>(out, out + n);
  };

  // A frame that passes every check.
  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
  const std::vector<uint8_t> good = radio.last_transmission;

  radio.deliver(good.data(), good.size());
  iohome::frame::IoFrame frame;
  controller.check_received(&frame);
  TEST_ASSERT_EQUAL_INT(1, capture.calls);
  // What the sniffer captured is framed; de-framed it is the frame we sent.
  const std::vector<uint8_t> seen0 = deframed(capture.frames[0]);
  TEST_ASSERT_EQUAL_UINT(good.size(), seen0.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(good.data(), seen0.data(), good.size());

  // A frame with a broken CRC never reaches the frame callback, but the
  // sniffer must still see it - that is the whole point of the hook.
  std::vector<uint8_t> corrupted = good;
  corrupted[corrupted.size() - 1] ^= 0xFF;
  radio.deliver(corrupted.data(), corrupted.size());
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL_INT(2, capture.calls);
  const std::vector<uint8_t> seen1 = deframed(capture.frames[1]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(corrupted.data(), seen1.data(), corrupted.size());

  // So must a frame too short to parse at all - injected as raw wire bytes,
  // since it is not a well-formed frame to be framed.
  const uint8_t runt[5] = {0xF8, 0x00, 0x11, 0x22, 0x33};
  radio.deliver_raw(runt, sizeof(runt));
  TEST_ASSERT_FALSE(controller.check_received(&frame));
  TEST_ASSERT_EQUAL_INT(3, capture.calls);
  TEST_ASSERT_EQUAL_UINT(sizeof(runt), capture.frames[2].size());

  // Every delivered packet is counted, whatever became of it.
  TEST_ASSERT_EQUAL_UINT32(3, controller.rx_stats().received);
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().accepted);

  // Detaching stops the callbacks without disturbing reception.
  controller.set_raw_frame_callback(nullptr);
  radio.deliver(good.data(), good.size());
  controller.check_received(&frame);
  TEST_ASSERT_EQUAL_INT(3, capture.calls);
  TEST_ASSERT_EQUAL_UINT32(4, controller.rx_stats().received);
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

namespace {
struct LogCapture {
  std::vector<std::pair<iohome::LogLevel, std::string>> messages;

  bool has(iohome::LogLevel level, const char* fragment) const {
    for (const auto& entry : messages) {
      if (entry.first == level && entry.second.find(fragment) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  size_t count(iohome::LogLevel level) const {
    size_t n = 0;
    for (const auto& entry : messages) {
      if (entry.first == level) {
        n++;
      }
    }
    return n;
  }
};

void log_cb(iohome::LogLevel level, const char* message, void* ctx) {
  static_cast<LogCapture*>(ctx)->messages.emplace_back(level, std::string(message));
}
}  // namespace

void test_log_sink_receives_messages_with_severity(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);

  LogCapture capture;
  controller.set_log_callback(log_cb, &capture);

  // A sink gets the messages whether or not the serial output is on: the
  // verbose flag governs the built-in printf, not the library's logging.
  TEST_ASSERT_FALSE(controller.begin(OWN_NODE, nullptr));
  TEST_ASSERT_TRUE(capture.has(iohome::LogLevel::ERROR, "Invalid parameters"));

  // Severity is a parameter now. It used to be spelled into the text as an
  // "Error:" prefix, which no caller could filter on.
  for (const auto& entry : capture.messages) {
    TEST_ASSERT_TRUE(entry.second.find("Error:") == std::string::npos);
    TEST_ASSERT_TRUE(entry.second.find("Warning:") == std::string::npos);
  }

  capture.messages.clear();
  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));
  TEST_ASSERT_TRUE(capture.has(iohome::LogLevel::INFO, "1W"));

  // The formatted arguments arrive, and no message carries a trailing newline
  // - the sink is documented to receive a bare line.
  TEST_ASSERT_TRUE(capture.has(iohome::LogLevel::INFO, "1A 38 0B"));
  for (const auto& entry : capture.messages) {
    TEST_ASSERT_TRUE(!entry.second.empty());
    TEST_ASSERT_NOT_EQUAL('\n', entry.second.back());
  }
}

void test_log_level_filters(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);

  LogCapture capture;
  controller.set_log_callback(log_cb, &capture, iohome::LogLevel::ERROR);
  controller.set_verbose(true);

  TEST_ASSERT_TRUE(controller.begin(OWN_NODE, SYSTEM_KEY));
  TEST_ASSERT_EQUAL_INT(RADIOLIB_ERR_NONE, controller.start_receive());

  // Only errors get through, and there are none on this path.
  TEST_ASSERT_EQUAL_UINT(0, capture.count(iohome::LogLevel::INFO));
  TEST_ASSERT_EQUAL_UINT(0, capture.count(iohome::LogLevel::DEBUG));

  // An error does.
  TEST_ASSERT_FALSE(controller.set_acei(0x60));
  TEST_ASSERT_EQUAL_UINT(1, capture.count(iohome::LogLevel::ERROR));

  // Widening the filter lets the rest through without reinstalling anything.
  controller.set_log_callback(log_cb, &capture, iohome::LogLevel::DEBUG);
  TEST_ASSERT_TRUE(controller.close(PEER_NODE));
  TEST_ASSERT_TRUE(capture.count(iohome::LogLevel::INFO) > 0);
  // The transmitted bytes are a DEBUG message, one per frame rather than one
  // per byte: a sink receives whole messages.
  TEST_ASSERT_TRUE(capture.has(iohome::LogLevel::DEBUG, "Transmitting 25 bytes"));

  // Detaching goes back to the built-in output. set_verbose(true) is still on,
  // so this deliberately prints one "[iohc ERROR] ..." line to stdout - that
  // line in the test output is the fallback working, not a stray printf.
  controller.set_log_callback(nullptr);
  const size_t before = capture.messages.size();
  TEST_ASSERT_FALSE(controller.set_acei(0x60));
  TEST_ASSERT_EQUAL_UINT(before, capture.messages.size());
}

namespace {
/// log() is protected; a subclass is how a derived class would reach it.
struct LoggingController : IoHomeControl {
  explicit LoggingController(PhysicalLayer* radio) : IoHomeControl(radio) {}
  using IoHomeControl::log;
};
}  // namespace

void test_log_passthrough_does_not_reformat(void) {
  PhysicalLayer radio;
  LoggingController controller(&radio);

  LogCapture capture;
  controller.set_log_callback(log_cb, &capture);

  // log() forwards the caller's string as an argument, not as the format. A
  // percent sign in it would otherwise read arguments that were never passed.
  controller.log("100% of %s frames");
  TEST_ASSERT_EQUAL_UINT(1, capture.messages.size());
  TEST_ASSERT_EQUAL_STRING("100% of %s frames", capture.messages[0].second.c_str());

  controller.log(nullptr);
  TEST_ASSERT_EQUAL_UINT(1, capture.messages.size());
}

void test_stats_reset(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  const std::vector<uint8_t> tx = make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 1, SYSTEM_KEY);
  radio.deliver(tx.data(), tx.size());

  iohome::frame::IoFrame frame;
  controller.check_received(&frame);
  TEST_ASSERT_EQUAL_UINT32(1, controller.rx_stats().accepted);

  controller.reset_rx_stats();
  TEST_ASSERT_EQUAL_UINT32(0, controller.rx_stats().accepted);
  TEST_ASSERT_EQUAL(RxReject::NONE, controller.last_reject_reason());
}

// ---------------------------------------------------------------------------
// Pairing and discovery
// ---------------------------------------------------------------------------

void test_pair_device_1w_emits_two_frames(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  const uint8_t new_key[16] = {0x01, 0x02, 0x03};

  // docs/linklayer.md "1W Discovery": remove the old key (0x39), then send the
  // new one (0x30). Regression: this used to fail outright because the key
  // transfer frame did not fit the (wrongly sized) maximum frame.
  TEST_ASSERT_TRUE(controller.pair_device_1w(PEER_NODE, new_key, 0x02));
  TEST_ASSERT_EQUAL_UINT(2, radio.transmissions.size());

  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_REMOVE_1W_CONTROLLER, radio.transmissions[0][8]);
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_SEND_1W_KEY, radio.transmissions[1][8]);

  // The receiver must be able to recover the key we sent.
  iohome::frame::IoFrame parsed;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(radio.transmissions[1].data(),
                                              radio.transmissions[1].size(), &parsed,
                                              iohome::frame::AuthTrailer::NONE));
  TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed));

  // A real receiver unmasks with the frame's source - us, the sender - because
  // the key belongs to the sender. See create_key_transfer_1w.
  uint8_t recovered[16];
  TEST_ASSERT_TRUE(iohome::crypto::decrypt_1w_key(parsed.data, parsed.src_node, recovered));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(new_key, recovered, 16);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(OWN_NODE, parsed.src_node, 3);
}

void test_pair_rejects_nullptr(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);

  const uint8_t key[16] = {0};
  TEST_ASSERT_FALSE(controller.pair_device_1w(nullptr, key));
  TEST_ASSERT_FALSE(controller.pair_device_1w(PEER_NODE, nullptr));
  TEST_ASSERT_FALSE(controller.pair_device_2w(nullptr, key));
}

void test_discovery_transmits_request(void) {
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY, false);

  TEST_ASSERT_TRUE(controller.start_discovery(0xFF, 5000));
  TEST_ASSERT_EQUAL_UINT(1, radio.transmissions.size());
  TEST_ASSERT_EQUAL_HEX8(iohome::CMD_DISCOVER, radio.last_transmission[8]);

  // The request must carry a real CRC, not zeroes.
  iohome::frame::IoFrame parsed;
  TEST_ASSERT_TRUE(iohome::frame::parse_frame(radio.last_transmission.data(),
                                              radio.last_transmission.size(), &parsed));
  TEST_ASSERT_TRUE(iohome::frame::validate_frame(&parsed));
}

// ---------------------------------------------------------------------------
// Frequency hopping
// ---------------------------------------------------------------------------

void test_frequency_hopping_is_2w_only(void) {
  PhysicalLayer radio;
  IoHomeControl controller_1w(&radio);
  controller_1w.begin(OWN_NODE, SYSTEM_KEY, true);
  TEST_ASSERT_FALSE(controller_1w.enable_frequency_hopping(true));

  IoHomeControl controller_2w(&radio);
  controller_2w.begin(OWN_NODE, SYSTEM_KEY, false);
  TEST_ASSERT_TRUE(controller_2w.enable_frequency_hopping(true));
}

// ---------------------------------------------------------------------------
// Adversarial input
// ---------------------------------------------------------------------------

namespace {

/// Deterministic xorshift, so a failure is reproducible from the seed alone.
uint32_t next_random(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

}  // namespace

void test_survives_arbitrary_radio_input(void) {
  // Everything the receive path sees is attacker controlled: anyone can
  // transmit on 868 MHz. Feed it garbage of every length and make sure it
  // neither accepts a frame nor trips a sanitizer.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.start_receive();

  uint32_t state = 0x1A380B01;
  iohome::frame::IoFrame frame;

  for (int round = 0; round < 4000; round++) {
    // Raw wire bytes, up to the full capture size, so the de-framer is fuzzed
    // along with the parser, MAC and replay checks - the whole receive path as
    // it runs on real hardware.
    uint8_t buffer[64];
    const size_t len = 1 + (next_random(state) % sizeof(buffer));

    for (size_t i = 0; i < len; i++) {
      buffer[i] = static_cast<uint8_t>(next_random(state) & 0xFF);
    }

    radio.deliver_raw(buffer, len);
    // Random bytes should never pass CRC, MAC and the replay check. If one
    // ever did, that is a 1-in-2^64 fluke or a real hole.
    TEST_ASSERT_FALSE(controller.check_received(&frame));
  }

  TEST_ASSERT_EQUAL_UINT32(0, controller.rx_stats().accepted);
}

void test_survives_corrupted_valid_frames(void) {
  // Bit flips in an otherwise well-formed frame are the more interesting case:
  // they get much further into the parser than random noise does.
  PhysicalLayer radio;
  IoHomeControl controller(&radio);
  controller.begin(OWN_NODE, SYSTEM_KEY);
  controller.set_accept_plain_frames(true);  // exercise the widest path
  controller.start_receive();

  const std::vector<uint8_t> valid =
      make_frame(iohome::CMD_EXECUTE, iohome::MP_CLOSE, 1, SYSTEM_KEY);

  iohome::frame::IoFrame frame;
  uint32_t state = 0xC0FFEE01;

  for (int round = 0; round < 3000; round++) {
    std::vector<uint8_t> corrupted = valid;

    const int flips = 1 + static_cast<int>(next_random(state) % 4);
    for (int i = 0; i < flips; i++) {
      const size_t index = next_random(state) % corrupted.size();
      corrupted[index] ^= static_cast<uint8_t>(1u << (next_random(state) % 8));
    }

    radio.deliver(corrupted.data(), corrupted.size());
    // Whatever the verdict, it must not crash or read out of bounds.
    controller.check_received(&frame);
  }
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_begin_rejects_nullptr);
  RUN_TEST(test_begin_without_radio_fails);
  RUN_TEST(test_configure_radio_programs_correct_parameters);

  RUN_TEST(test_execute_frame_shape);
  RUN_TEST(test_execute_with_extra_functional_params);
  RUN_TEST(test_position_mapping);
  RUN_TEST(test_ventilate_sends_secured_ventilation);
  RUN_TEST(test_rejects_invalid_acei);
  RUN_TEST(test_set_priority_selects_the_acei_level);
  RUN_TEST(test_send_command_rejects_nullptr);
  RUN_TEST(test_2w_requires_handshake);
  RUN_TEST(test_2w_commands_work_after_handshake);
  RUN_TEST(test_accepts_peer_initiated_challenge);
  RUN_TEST(test_rejects_forged_peer_challenge);
  RUN_TEST(test_stray_challenge_response_does_not_break_the_session);

  RUN_TEST(test_rolling_code_increments);
  RUN_TEST(test_rolling_code_writes_are_batched);
  RUN_TEST(test_rolling_code_resumes_past_reservation);

  RUN_TEST(test_accepts_valid_authenticated_frame);
  RUN_TEST(test_rejects_corrupted_frame);
  RUN_TEST(test_rejects_frame_signed_with_wrong_key);
  RUN_TEST(test_rejects_replayed_frame);
  RUN_TEST(test_accepts_advancing_sequence);
  RUN_TEST(test_rejects_unauthenticated_command_frame);
  RUN_TEST(test_accepts_plain_bootstrap_command);
  RUN_TEST(test_plain_frames_can_be_allowed_explicitly);
  RUN_TEST(test_rejects_garbage);
  RUN_TEST(test_check_received_needs_a_packet);
  RUN_TEST(test_packet_length_helper_programs_each_frame);
  RUN_TEST(test_raw_sniffer_sees_every_packet);
  RUN_TEST(test_stats_reset);

  RUN_TEST(test_log_sink_receives_messages_with_severity);
  RUN_TEST(test_log_level_filters);
  RUN_TEST(test_log_passthrough_does_not_reformat);

  RUN_TEST(test_2w_push_pairing_transfers_the_key);
  RUN_TEST(test_2w_pairing_ignores_a_stranger_challenge);
  RUN_TEST(test_pairing_expires_after_timeout);
  RUN_TEST(test_pairing_rejects_truncated_frames);
  RUN_TEST(test_2w_push_adopts_new_key_only_after_ack);
  RUN_TEST(test_2w_push_transmit_failure_keeps_old_key);
  RUN_TEST(test_initiator_ignores_stranger_pairing_requests);
  RUN_TEST(test_2w_follower_rejects_unsolicited_key_transfer);
  RUN_TEST(test_2w_pull_collects_the_device_key);
  RUN_TEST(test_2w_pairing_is_off_by_default);
  RUN_TEST(test_1w_node_ignores_pairing_frames_without_crashing);
  RUN_TEST(test_pair_device_1w_emits_two_frames);
  RUN_TEST(test_pair_rejects_nullptr);
  RUN_TEST(test_discovery_transmits_request);

  RUN_TEST(test_frequency_hopping_is_2w_only);

  RUN_TEST(test_survives_arbitrary_radio_input);
  RUN_TEST(test_survives_corrupted_valid_frames);

  return UNITY_END();
}
