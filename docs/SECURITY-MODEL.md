---
title: iown-homecontrol - Security Model
description: What io-homecontrol's authentication does and does not protect against
icon: material/shield-key
---

# Security Model

This page describes what the authentication in io-homecontrol actually buys
you, where it falls short, and what this implementation does about it. Read it
before pairing anything you care about.

The short version: io-homecontrol authenticates commands but does not encrypt
them, its key distribution is only obfuscated, and several parts of the design
predate current expectations. It is adequate for keeping a neighbour's remote
from opening your blinds. It is not a barrier against a motivated attacker with
a software-defined radio.

---

## What the protocol protects

### Message authentication

The MAC is AES-128 over a constructed IV, truncated to 48 bits. The IV mixes in
the command, its parameters, a proprietary checksum, and a freshness value. The
two modes carry it differently:

| Mode | Freshness input                         | Where the MAC travels                                 |
| ---- | --------------------------------------- | ----------------------------------------------------- |
| 1W   | 16-bit sequence number                  | Trailer on the command: sequence (2) + MAC (6)        |
| 2W   | 6-byte challenge chosen by the receiver | Payload of a separate 0x3D frame; no trailer anywhere |

2W follows `docs/linklayer.md` ("2W Authentication") and its captures: the
command goes out **plain**; the receiver answers with a plain 0x3C carrying a
fresh challenge; the sender answers with a plain 0x3D whose 6-byte payload is
`create_2w_hmac(command id + parameters of the original command, challenge,
system key)`; only then does the receiver act. With the documented push example
(stack key `0102…1516`, challenge `123456789ABC`, command 0x36) the answer is
`C7FDC0668818`, which `test_2w_auth_matches_documented_capture` pins.

Earlier versions of this library invented a different 2W scheme - a 0x3C with a
MAC trailer to open a "session", a 0x3D echoing the challenge, and a MAC trailer
on every 2W command - that no real device sends or accepts, and that dropped the
real, plain 0x3C/0x3D as unauthenticated. It also answered any signed 0x3C with
a MAC over the nonce it carried. Now:

- `IoHomeControl::send_command()` / the ESPHome component send a 2W command at
  once, plain, and remember it as the pending request.
- A 0x3C is answered **only** if it comes from the node that pending request
  went to, is addressed to us, and arrives within the response window (1 s by
  default, at most three answers). A nonce from anyone else is never signed, so
  there is no chosen-input MAC oracle; and signing our own request is no gift,
  because the actuator will pick its own fresh nonce for any replay of it.
- A plain command a peer sends *us* is held, challenged with a CSPRNG nonce, and
  delivered only after its 0x3D verifies (constant-time compare). A missing,
  short or wrong 0x3D consumes the nonce.

Without the 128-bit system key an attacker cannot produce a MAC that an
actuator will accept, so they cannot forge a command.

### Replay resistance

Because the freshness value feeds the MAC, an attacker cannot take a recorded
frame and change the command inside it. But the recorded frame *itself* stays
valid forever unless the receiver remembers what it has already seen — the MAC
is not bound to time.

That is what `iohome::ReplayGuard` is for. It tracks the highest sequence
number seen per source node and accepts a frame only if the counter moved
forward, within a bounded window:

```
delta = (received_sequence - last_seen_sequence) mod 2^16
accept if 0 < delta <= window          (window defaults to 1000)
```

The window exists because a transmitter counts every frame it sends, including
ones this receiver never heard — out of range, collision, a different channel.
Without a window those gaps would lock the receiver out; with one, a frame
recorded long ago lands outside it and is rejected.

`IoHomeControl` applies the guard automatically to every authenticated 1W frame
it accepts, and reports rejections through `rx_stats().replays`.

**The MAC does not cover the addresses.** A recorded 1W frame can be rewritten
to any source address and its CRC recomputed, and it still verifies. Keyed on
the source alone, the guard took every such rewrite for a new node's first
frame and accepted it - an unlimited replay. Two mechanisms close that:

- a **global ring of recently accepted MACs** (`GLOBAL_MAC_HISTORY` = 32, not
  keyed by source) rejects a MAC already accepted from *any* node, whatever
  address it now claims;
- **strict mode** (`ReplayGuard::set_strict()`,
  `IoHomeControl::set_strict_replay_protection()`) accepts only sources that
  were registered with `prime()` (persisted sequence) or `add_known_peer()`.
  The ESPHome component turns it on with `position_feedback`, registering the
  configured covers' target addresses - the only sources it takes reports from.

**2W frames carry no sequence number and no MAC.** Their freshness is the
receiver's fresh challenge: a recorded 0x3D answers a nonce that will never be
issued again, and a recorded command is re-challenged with a new one.


---

## What the protocol does *not* protect

### Frames are not encrypted

The MAC authenticates; it does not conceal. Node addresses, command IDs and
parameters travel in the clear. Anyone with a 868 MHz receiver can see which
device moved, when, and to what position — which is a occupancy side channel
for a house.

There is nothing an implementation can do about this: it is how the protocol
works.

### The transfer key is public

Key distribution masks the system key with `AES-128(TRANSFER_KEY, IV)`, where
`TRANSFER_KEY` is a constant every io-homecontrol device shares. It is in this
repository, in `src/protocol/iohome_constants.h`, because it is in every device
on the market.

**A pairing frame therefore reveals the system key to anyone listening.**
Treat pairing as a plaintext key exchange:

- pair with the actuator physically close by, so the frame is weak at a
  distance;
- pair as rarely as you can;
- assume that anyone who was listening during pairing owns the installation.

The mask is `AES-128(TRANSFER_KEY, IV)` where the IV is built from the frame
that *requested* the transfer - command 0x38 (launch key transfer) with its
challenge, or command 0x31 (ask challenge) - together with the challenge
itself. Deriving it from the challenge alone produces a mask the peer does not
compute, so the key arrives as noise. `docs/linklayer.md` carries a captured
key transfer; `test_crypto` checks the implementation against it, because two
implementations that are wrong the same way round-trip perfectly.

### The follower only adopts a key it challenged for

`set_accept_pairing(true)` puts a 2W node in a window where it will accept a
pushed key. A pushed 0x32 is unmasked against the random challenge the node put
in its own 0x3C, so the node adopts a key only after it has issued that
challenge to the peer it is pairing with. Three checks enforce this, and matter
most on a *fresh* node whose `pairing_peer_` and `current_challenge_` are still
all-zero: the follower requires a live challenge (`has_active_challenge`) before
recovering any pushed key; `recover_2w_key()` refuses to unmask against an
all-zero challenge (a nonce the CSPRNG never emits); and any pairing frame whose
source is `00:00:00` is dropped so it can never match a zeroed peer. Without
these, an unsolicited 0x32 from source `00:00:00` would have unmasked against
the zero challenge and let a passer-by set the installation key with no
handshake — see V70.

What these checks do **not** remove is a **chosen-key injection** *during* an
open pairing window. The 2W key mask is `AES-128(TRANSFER_KEY, IV)`, and every
input to it is attacker-known: `TRANSFER_KEY` is the public constant above, the
ask-challenge request byte is a protocol constant, and the challenge is a nonce
the node **broadcasts in the clear** in its own 0x3C (frames are authenticated,
not encrypted). So an attacker who is active in the window can send its own 0x31,
read the challenge C off the node's 0x3C, compute the mask, and send a CRC-valid
0x32 (the 0x32 carries only a CRC, no MAC) whose payload is `K_chosen XOR mask`.
The node unmasks it back to `K_chosen` and adopts it — a key the **attacker
picked and therefore knows**. This is not a mere garbage-key corruption or DoS:
the attacker now holds the node's system key and can forge authenticated 2W
commands (open/close/ventilate/stop) and read state until the device is
re-paired. It is the same disclosure as a passive listener recovering the
operator's pushed key (see “A pairing frame reveals the system key” above), only
active — and it is inherent to io-homecontrol's plaintext key transfer, not
something these checks can close. The three checks above still matter: they stop
the *unsolicited*, no-handshake variant (a chosen key adopted with no 0x31/0x3C
at all — V70), which is reachable outside as well as inside the window. The
residual is bounded to the operator-opened window, so treat that window as a
key-disclosure risk: keep it as short as the guidance above says, pair with the
actuator physically close, and assume anyone active during pairing owns the
installation.

### The system key may be weak by origin

If the key came from an Overkiz/TaHoma box, it was generated by LuaJIT's
`math.random` seeded from the box serial number and the setup time. That is on
the order of 2^25 possibilities for an attacker who knows the serial — far
below the 2^128 the key size suggests. See `docs/linklayer.md` for the
generator.

Keys generated by this library use the platform CSPRNG
(`crypto::generate_system_key()`), which does not have that problem.

### 48-bit MAC

A 6-byte MAC leaves a 1-in-2^48 chance per blind forgery attempt. At the
protocol's 38.4 kbps that is not a practical online attack, but it is well
below what a modern design would choose, and it is fixed by the protocol.

### ESPHome position feedback is authenticated, but not confidential

The ESPHome component's `position_feedback` option updates a cover's reported
position from received frames only after the frame's MAC and rolling code
verify — the same `validate_frame()` + `ReplayGuard` gate every command goes
through. `dispatch_to_covers_()` drops any frame that is not `authenticated`, so
a spoofed or replayed position report cannot show a false position (this was the
V50/V58 fix). It is off by default because it needs the system key, and it
cannot cause an actuator to move. What it does *not* provide is confidentiality:
the frames are unencrypted, so an observer can still see which covers report and
when — io-homecontrol offers no confidentiality at all (the frames are plaintext
apart from the MAC).

---

## What this implementation does

| Area | Measure |
| ---- | ------- |
| Challenge generation | Platform CSPRNG (`esp_random` on ESP32, `std::random_device` on host). Refuses to emit a challenge rather than fall back to a predictable PRNG. |
| Challenge lifetime | Single use. Consumed on the first verification attempt from the challenged peer, success or failure, so an attacker cannot grind responses against a known nonce. Expires after 5 s. |
| Challenge answering | Only for our own pending 2W command, to the node it went to, within 1 s (`set_response_window_ms`), at most 3 times. |
| MAC comparison | Constant time (`crypto::constant_time_equal`), so a timing side channel cannot leak the expected MAC byte by byte. |
| Replay (1W) | `ReplayGuard`: sequence per source node (wraparound-safe, configurable window), plus a global recent-MAC ring that catches re-addressed replays, plus optional strict known-peers mode. |
| Replay (2W) | Fresh receiver-chosen challenge per command (0x3C/0x3D). |
| Pairing window | `set_accept_pairing(true)` closes itself after 120 s (configurable) and after one key has been adopted or handed out. |
| Rolling code persistence | Stored in NVS with block reservation, so a reboot resumes past every counter value that could have been transmitted. Without this, a receiver rejects everything until the counter catches up — and an attacker's recording becomes replayable again. |
| Key material | Zeroed with `crypto::secure_zero()` when it leaves scope. |
| Receive policy | 1W: CRC, then MAC, then replay. Frames without a MAC are rejected unless they carry one of the commands the protocol defines as unauthenticated (discovery, key transfer and their acks, 0x3C/0x3D). 2W: plain 0x3C/0x3D go to the authentication manager; any other command addressed to us is held and challenged. `validate_frame()` with a key rejects a frame that has no MAC. |
| Frame parsing | Bounds-checked against the declared length and the buffer, with the parser exercised under AddressSanitizer over every possible control-byte combination. |

### Things deliberately not done

- **No key storage encryption.** The system key sits in RAM in the clear, and
  in NVS in the clear if you put it there. On an ESP32 without flash
  encryption and secure boot, anyone with physical access and a USB cable
  reads it out. If that matters for your threat model, enable ESP32 flash
  encryption; the library cannot substitute for it.
- **No rate limiting on pairing.** Pairing is operator-initiated and rare.

---

## Threat model summary

| Threat | Protected? | By what |
| ------ | ---------- | ------- |
| Neighbour's remote operates your blinds | Yes | System key + MAC |
| Attacker forges a command without the key | Yes | 48-bit MAC |
| Attacker records and replays a 1W command | **Within a power cycle** | Receive-side `ReplayGuard` rejects it while the node stays up. Its state is in RAM only and is *not* primed on boot, so after a reboot a known peer's first frame is accepted at any sequence - one replay per *known* source per reboot, provided its MAC is not still in the ring. Harmless in this project — received 1W frames only update reported position, never actuator motion — but a receiver that *acts* on 1W commands must persist the last-accepted sequence per peer and restore it with `prime()`. |
| Attacker replays a 1W frame re-addressed to a new source | Yes | Global recent-MAC ring (32 deep); strict known-peers mode rejects unknown sources outright (on in ESPHome with `position_feedback`) |
| Attacker replays a 2W command or 0x3D | Yes | Receiver-chosen single-use challenge per command |
| Attacker gets our 2W key to sign a chosen nonce | Yes | 0x3C answered only for our own pending command to that peer |
| Attacker replays a command *we sent* after we power-cycle | Yes | Our transmit rolling code is persisted in NVS with block reservation, so we never reuse a sequence the actuator has already seen (distinct from the receive-side guard in the row above) |
| Attacker learns the MAC by timing your receiver | Yes | Constant-time comparison |
| Attacker predicts a 2W challenge | Yes | CSPRNG-backed challenges |
| Attacker observes which devices move and when | **No** | Frames are not encrypted |
| Attacker recording during pairing learns the key | **No** | Transfer key is a public constant |
| Attacker brute-forces a TaHoma-generated key | **No** | Weak upstream key generation |
| Attacker with physical access reads the key out of flash | **No** | Use ESP32 flash encryption |

---

## Reporting a security issue

Please follow the process in [`.github/SECURITY.md`](https://github.com/velocet/iown-homecontrol/blob/main/.github/SECURITY.md).
Do not open a public issue for a vulnerability in the authentication path.
