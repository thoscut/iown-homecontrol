---
title: iown-homecontrol - Radio Setup
description: Configuring an SX127x or SX126x transceiver for io-homecontrol
icon: material/radio-tower
---

# Radio Setup

RadioLib's FSK defaults do not match io-homecontrol. Most of the difference is
handled by `IoHomeControl::configure_radio()`, but two settings live on the
concrete radio class rather than on `PhysicalLayer`, so the application has to
make those calls itself.

This page lists everything the radio needs and gives the per-chip snippets.

---

## Parameters

| Parameter | Value | Set by |
| --------- | ----- | ------ |
| Frequency | 868.95 MHz (channel 2) | `configure_radio()` |
| Modulation | 2-FSK | `beginFSK()` |
| Bit rate | 38.4 kbps | `configure_radio()` |
| Frequency deviation | 19.2 kHz | `configure_radio()` |
| Encoding | NRZ | `configure_radio()` |
| Data shaping | none | `configure_radio()` |
| Sync word | `57 FD 99` | `configure_radio()` |
| Preamble | 512 bits | `configure_radio()` |
| Radio CRC | **off** | your code |
| Packet length mode | **fixed** | your code |

Three of these are easy to get wrong:

**Bit rate and deviation are in kbps and kHz.** RadioLib's `DataRate_t.fsk`
does not take bit/s and Hz. Passing `38400` puts the value outside the allowed
range and `setDataRate()` fails, taking the whole setup with it.

**The preamble length is in bits.** `setPreambleLength(512)`, not `512 / 8`.

**The sync word is bit-reversed.** io-homecontrol transmits least significant
bit first; RadioLib matches most significant bit first. The on-air sequence
`0xFF33` therefore has to be programmed as `57 FD 99`. Deriving the bytes from
`0xFF33` by shifting yields `00 FF 33`, which matches nothing. Use
`iohome::SYNC_WORD_BYTES`.

---

## Disabling the radio's CRC and length byte

An io-homecontrol frame carries its own length in Control Byte 0 and its own
CRC-16/KERMIT in the last two bytes. RadioLib's FSK defaults would prepend a
length byte and append a second CRC, corrupting every frame in both directions.

Because frames vary between 11 and 34 bytes, fixed-length mode has to be
narrowed to the exact length before each transmission and widened again for
reception. `IoHomeControl` does that through a hook so it does not need to know
which chip you have.

### SX1276 / SX1278 (SX127x family)

```cpp
SX1276 radio = new Module(CS, DIO0, RST, DIO1);

radio.beginFSK();
radio.setCRC(false);
radio.fixedPacketLengthMode(iohome::FRAME_MAX_SIZE);

controller.set_packet_length_callback(
    [](uint8_t len, void *ctx) -> int16_t {
      return static_cast<SX1276 *>(ctx)->fixedPacketLengthMode(len);
    },
    &radio);
```

### SX1262 / SX1268 (SX126x family)

`SX126x::setCRC()` takes the CRC length in bytes rather than a flag; `0`
disables it.

```cpp
SX1262 radio = new Module(CS, DIO1, RST, BUSY);

radio.beginFSK();
radio.setCRC(0);
radio.fixedPacketLengthMode(iohome::FRAME_MAX_SIZE);

controller.set_packet_length_callback(
    [](uint8_t len, void *ctx) -> int16_t {
      return static_cast<SX1262 *>(ctx)->fixedPacketLengthMode(len);
    },
    &radio);
```

### What happens without the hook

Fixed-length mode transmits exactly the programmed number of bytes. Leaving it
at `FRAME_MAX_SIZE` pads every frame out to 34 bytes with whatever follows the
buffer, and reading past the frame is exactly the kind of thing a receiver
should not have to tolerate. Receivers do use the length field in Control Byte
0, so the padding is ignored in practice - but the airtime is wasted and the
trailing bytes are uninitialised memory.

The ESPHome component holds the concrete radio pointer itself and does this
internally; nothing is required of the YAML.

---

## Reception

Reception is interrupt driven. `start_receive()` registers RadioLib's packet
callback and puts the radio into receive mode; `check_received()` picks the
frame up from `loop()`:

```cpp
controller.start_receive(on_frame);

void loop() {
  iohome::frame::IoFrame frame;
  if (controller.check_received(&frame)) {
    // accepted: CRC, MAC and replay checks all passed
  }
}
```

`scanChannel()` is *not* usable here: it is a LoRa channel-activity-detection
call and returns `RADIOLIB_ERR_WRONG_MODEM` in FSK.

If you drive the radio interrupt yourself, call
`IoHomeControl::notify_packet_received()` from your ISR instead.

---

## Frequency hopping (2W)

2W traffic hops across the three channels with a 2.7 ms dwell time. That is
below millisecond resolution, so `update_frequency_hopping()` reads `micros()`
internally - call it as often as you can from `loop()`:

```cpp
controller.enable_frequency_hopping(true);

void loop() {
  controller.update_frequency_hopping();
  // ...
}
```

Hopping this fast in software is tight. If frames are being missed, consider
pinning the loop to a core with nothing else on it, or driving the hop from a
hardware timer.

---

## Diagnosing a quiet link

`rx_stats()` reports why frames are being dropped, which usually points
straight at the cause:

| Counter rising | Likely cause |
| -------------- | ------------ |
| `crc_failures` | Wrong bit rate, deviation, or shaping; or the radio's own CRC is still enabled |
| `malformed` | Length byte still enabled, or the sync word is off by a bit |
| `mac_failures` | Wrong system key, or a 2W frame with no session nonce |
| `replays` | A retransmission, or someone replaying frames at you |
| `unauthenticated` | Peer is sending plain frames; it has not been paired |
| nothing at all | Sync word or frequency wrong, or the radio never entered receive mode |
