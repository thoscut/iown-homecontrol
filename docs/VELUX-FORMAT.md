# Reading the Velux format without a firmware dump

Several things this project needed to know about io-homecontrol were marked
"needs a capture" or "needs a dump". Most of them did not.

Velux publishes the full technical specification of the **KLF 200**, its own
io-homecontrol gateway, and three revisions of it have been sitting in
[`docs/devices/velux/KLF200/`](devices/velux/KLF200/) the whole time. The KLF 200
speaks a documented API on the LAN side and io-homecontrol on the RF side, so its
specification describes the actuator model in detail: what a command originator
is, what the priority levels mean, how a position value is encoded, what an
actuator type is and how it is packed. That is not the radio format, but it is
the semantics the radio format carries, written down by the manufacturer.

Cross-read against this repository's own `docs/commands.md`, which was
assembled from captures, it settles questions that otherwise wait for hardware -
and it exposes several places where the C++ had invented an answer.

Where the two sources agree, the fact is as solid as this project can make it
without a radio: one comes from observing real traffic, the other from the
manufacturer, and they were produced independently.

---

## What it confirmed

These were already right. They are worth recording as *verified* rather than
*assumed*, because "assumed correct" and "checked against the manufacturer's own
specification" are not the same claim.

| Thing | Source |
| ----- | ------ |
| Command Originator values - USER 1, RAIN 2, TIMER 3, UPS 5, SAAC 8, WIND 9, LOAD_SHEDDING 11, LOCAL_LIGHT 12, UNSPECIFIC_ENVIRONMENT_SENSOR 13, EMERGENCY 255 | Table 164 |
| Priority levels - 8 of them in three groups: Protection (0-1), User (2-3), Comfort (4-7) | Table 165 |
| Main Parameter encoding - Relative 0x0000-0xC800, Percent± 0xC900-0xD0D0, Target 0xD100, Current 0xD200, Default 0xD300, Ignore 0xD400 | Table 275 |
| 100 % is 51200 = 0xC800, not 0xFFFF | §13.1 |

### The direction of the Main Parameter

This one had been listed as an open question requiring hardware. It does not.

> The effect of the main parameter is adjusted so that it is possible to use a
> keyboard with up, down and stop, so that the up button always sends
> MP = 0x0000 and down button always sends 0xC800. Stop button sends
> MP = Current = 0xD200.
>
> — KLF 200 API §14.1

The accompanying table gives the meaning per actuator profile, and it is
consistent across all of them: **0x0000 is fully open / on / maximum, 0xC800 is
fully closed / off / minimum.**

| Actuator | MP = 0x0000 | MP = 0xC800 |
| -------- | ----------- | ----------- |
| Window opener | 100 % open | 0 % open |
| Roller shutter, blinds, awnings | 0 % down, light flows freely | 100 % down |
| Garage door, gate | 0 % closed - open | 100 % closed |
| Light | 100 % light output | 0 % |
| Door lock, window lock | Unlocked | Locked |
| On/off switch | On | Off |
| Ventilation point | Maximum ventilation | Minimum |

So `MP_OPEN = 0x0000` and `MP_CLOSE = 0xC800` are right, and the "Main Parameter
counts closure" framing holds for every profile.

What this does *not* settle is the direction of Functional Parameter 1 on a
tilting blind. The specification says FP1 is "orientation of the slats" and
stops there. That one still needs a capture.

### A second implementation sends the same values

[`rspaargaren/iohomecontrol`](https://github.com/rspaargaren/iohomecontrol) drives
real Velux hardware by emulating a remote, and its button map is a third
independent source next to `docs/commands.md` and the KLF 200 specification. It
agrees where it overlaps:

| Button | Value it sends | Matches |
| ------ | -------------- | ------- |
| Vent | Main Parameter `0xD803` | `MP_SECURED_VENTILATION`, the §14.2.1 alias |
| ForceOpen | Main Parameter `0x6400` | `MP_FORCE` |

`0xD803` was already anchored to the specification; the emulated remote sending
exactly that closes the loop from the transmit side. `0x6400` is different: the
specification names no alias there, so it is recorded as **observed, not
spec-derived**. On the relative scale it is the halfway point (`0xC800 / 2`), so
on the wire a "force" preset is indistinguishable from a 50 % position - the
distinction is in the remote's intent, not the frame. That is why the library
exposes it as `force()` with `MP_FORCE`, kept separate from
`set_position(50)`, and marks the constant observed.

---

## What it contradicted

### The node type field is 16 bits, not 8

A node announces what it is with a 16-bit field: ten bits of type, six of
sub-type.

    type    = (field >> 6) & 0x3FF
    subtype =  field       & 0x3F

Both sources say so independently - `docs/commands.md` under *Discover Answer*
and Table 49 of the KLF 200 specification, which draws it as `AT9..AT0 |
ST5..ST0`. The values match too: window opener is type 4, so the field reads
0x0100; roller shutter is type 2, so 0x0080.

`DeviceType` was a byte with nineteen values that matched neither source -
`WINDOW_OPENER = 0x03` where the field says 0x0100, `ROLLER_SHUTTER = 0x00`
where it says 0x0080. It is now `NodeType`, carrying the real values.

### Every field of the Discover Answer was read at the wrong offset

The payload is:

| Offset | Field |
| ------ | ----- |
| 0-1 | Node type and sub-type, 16 bits, most significant byte first |
| 2-4 | Node address |
| 5 | Manufacturer (OEM) ID |
| 6 | Multi info byte |
| 7-8 | Timestamp |

The parser read `type = data[0]`, `manufacturer = data[1]`,
`protocol_version = data[2]`. That is the type truncated to its high byte, the
manufacturer read out of the type's low half, and a "protocol version" read from
the first byte of the node address - a field the answer does not contain.

Worked against the example in `docs/commands.md`, `29 FFC0 XXXXXX 0C CC 0000`: a
remote controller (type 1023) from Atlantic (0x0C) was reported as device type
0xFF from manufacturer 0xC0.

### There are no Velux-private command IDs at 0x58-0x5D

Six constants claimed there were: `GET_RAIN_SENSOR`, `SET_VENTILATION`,
`EMERGENCY_CLOSE`, `GET_WINDOW_STATUS`, `RESET_LIMITS`, `SET_LIMITS`. All six
were marked UNVERIFIED, and two structural facts give them away before any
capture is taken:

* 0x50-0x57 are four request/answer **pairs** - Get Name / Answer, Write Name /
  Ack, Get General Info 1 / Answer, Get General Info 2 / Answer. Even is the
  request, odd is the reply. The six sat in that block as unpaired singletons.
* That block is metadata. Four of the six claimed to be actuator *control*,
  which lives at command 0x00.

Every function they named exists. None of them is a command:

| Claimed command | What it actually is |
| --------------- | ------------------- |
| Get rain sensor | Not a query. A rain sensor is an input; when it acts, the frame it produces carries Command Originator 0x02 (RAIN). A window with one is node type 0x0101, *Window Opener with Integrated Rain Sensor*. |
| Set ventilation | Main Parameter **0xD803**, "Secured Ventilation" - "a position a window can be opened to for getting some ventilation and where the window is still locked" (§14.2.1). An ordinary Execute. |
| Emergency close | Command Originator 0xFF (EMERGENCY) plus a Protection priority level, on an ordinary Execute. Priority is what makes it override. |
| Get window status | The actuator reports its own state; a controller reads the current value with the Current access method (0xD200). |
| Set / reset limits | Real functionality - the KLF 200 exposes it as `GW_SET_LIMITATION_REQ` and documents the semantics in §10.5. Its **RF command ID is genuinely unknown**, and guessing 0x5C/0x5D did not make it known. |

### A node's product model is not on the wire

`detect_model()` mapped node type "window opener" to `VeluxModel::GGL_ELECTRIC`
and "venetian blind" to `FML` - specific product numbers derived from a field
that cannot distinguish them. A window opener is a window opener; whether it is a
GGL, a GGU or a GPL is a question the type field does not answer.

The KLF 200 gets that from a separate `ProductType` field ("Ex. KMG, KMX etc.")
which the Discover Answer does not carry. `Get General Info 1/2` (0x54-0x57) may
hold it, but `docs/commands.md` has no decoding for either.

It is now `detect_category()`, returning what the type field determines and
nothing more.

---

## The method, for the next question

1. **Read the two sources against each other.** `docs/commands.md` is captures;
   the KLF 200 specification is the manufacturer. Where they agree, stop
   worrying. Where only one speaks, note which.
2. **Check the shape before checking the value.** The six invented command IDs
   were detectable from the request/answer pairing alone, with no external
   source at all. A constant that breaks the pattern its neighbours follow is
   worth doubting.
3. **Ask what could possibly carry the information.** "Which model is this
   window" cannot be answered by a field with 1024 values covering every
   actuator in the ecosystem. When a function claims to derive something its
   inputs cannot determine, it is guessing.
4. **Say when something is still unknown.** The limitation command ID and the
   FP1 tilt direction are not resolved here. Writing a plausible constant and
   marking it UNVERIFIED is how this project got the six.

## Sources

* `docs/commands.md` - command list, node type table, manufacturer IDs
* `docs/devices/velux/KLF200/Velux-KLF200-Technical_Specification_API-318.pdf` -
  revision 3.18, and two earlier revisions alongside it
* `scripts/io-homecontrol.ksy` - the captured frame the length encoding is
  checked against
