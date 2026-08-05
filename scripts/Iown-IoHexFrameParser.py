#!/usr/bin/env python3
"""
Decode io-homecontrol frames given as hex.

  ./Iown-IoHexFrameParser.py f800 00007f 708758 00 ...
  ./Iown-IoHexFrameParser.py -f captures.txt
  ./Iown-IoHexFrameParser.py --self-test

Give the frame without the preamble and sync word, starting at Control Byte 0.
"""

import binascii
import importlib.util
import pathlib
import sys


def _load_sibling(name: str, filename: str):
  """
  Import a module that sits next to this file under a non-importable name.

  `from ioCrypto import ...` never resolved - the file is Iown-ioCrypto.py, and
  a hyphen cannot appear in a Python identifier - so this script failed at
  startup with ModuleNotFoundError. Loading by path also makes it work from any
  working directory rather than only from inside scripts/.
  """
  cached = sys.modules.get(name)
  if cached is not None:
    return cached
  path = pathlib.Path(__file__).resolve().with_name(filename)
  spec = importlib.util.spec_from_file_location(name, path)
  if spec is None or spec.loader is None:
    raise ImportError(f"cannot load {filename} from {path.parent}")
  module = importlib.util.module_from_spec(spec)
  sys.modules[name] = module
  spec.loader.exec_module(module)
  return module


compute_crc_8408 = _load_sibling("ioCrypto", "Iown-ioCrypto.py").compute_crc_8408

#### io-homecontrol definitions

# Size field bias: the field counts every byte except Control Byte 0 itself and
# the two CRC bytes, so the frame is three bytes longer than the field says.
SIZE_FIELD_BIAS = 3

# Bytes before the payload: two control bytes, destination, source, command.
HEADER_LEN = 9
CRC_LEN = 2
SEQ_LEN = 2
MAC_LEN = 6

# Order field, Control Byte 0 bits 7-6. These are not two independent
# "first frame"/"last frame" flags, which is how this script used to read them.
ORDER_NAMES = {
  0: "single",
  1: "next in series",
  2: "next in parallel",
  3: "command group end",
}

# Commands whose payload length is documented. Execute (0x00) and Activate Mode
# (0x01) are variable: they carry originator, ACEI, a 2-byte main parameter and
# at least two functional parameters, but more may follow.
FIXED_PAYLOAD_LEN = {
  0x28: 0,   # discover
  0x31: 0,   # ask challenge
  0x33: 0,   # key transfer ack
  0x39: 0,   # remove controller
  0x32: 16,  # key transfer
  0x30: 20,  # send 1W key
  0x3c: 6,   # challenge request
  0x3d: 6,   # challenge response
}
MIN_PAYLOAD_LEN = {0x00: 6, 0x01: 6}

# Bootstrap commands: the peers have no shared key yet, so these travel plain.
UNAUTHENTICATED_COMMANDS = {
  0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x30, 0x31, 0x32, 0x33,
  0x36, 0x37, 0x38, 0x39,
}


class IoFrame:

  Oneway = "One-Way"
  TwoWay = "Two-Way"

  def __init__(self, raw=None):
    self.raw = raw
    self.control_byte1 = 0
    self.control_byte2 = 0
    self.size_field = 0
    self.length = 0
    self.order = 0
    self.protocol_mode = IoFrame.Oneway
    self.from_addr = b""
    self.to_addr = b""
    self.use_beacon = False
    self.routed = False
    self.lpm = False
    self.ack = False
    self.protocol_version = 0
    self.commandid = 0
    self.commanddata = b""
    self.authenticated = False
    self.seqnum = b""
    self.mac = b""
    self.crc = b""
    if raw is not None:
      self.parse_raw(raw)

  def parse_raw(self, rframe):
    if len(rframe) < HEADER_LEN + CRC_LEN:
      raise ValueError(
        f"An io frame is at least {HEADER_LEN + CRC_LEN} bytes: "
        "control bytes, addresses, command ID and CRC")

    self.raw = rframe
    self.control_byte1 = rframe[0]
    self.control_byte2 = rframe[1]

    # The 5-bit field is not the frame length; the frame is three bytes longer.
    # Comparing the raw field against len(rframe) accepted truncated frames.
    self.size_field = self.control_byte1 & 0x1f
    self.length = self.size_field + SIZE_FIELD_BIAS
    if self.length != len(rframe):
      raise ValueError(
        f"Frame claims {self.length} bytes (size field {self.size_field} + "
        f"{SIZE_FIELD_BIAS}) but {len(rframe)} were given")

    self.order = (rframe[0] >> 6) & 0x03
    self.protocol_mode = IoFrame.Oneway if rframe[0] & 0x20 else IoFrame.TwoWay

    # Destination comes first on the wire, then the source. This script had the
    # two the wrong way round; scripts/io-homecontrol.ksy has the order right.
    self.to_addr = rframe[2:5]
    self.from_addr = rframe[5:8]

    self.use_beacon = rframe[1] & 0x80 != 0
    self.routed = rframe[1] & 0x40 != 0
    self.lpm = rframe[1] & 0x20 != 0
    self.ack = rframe[1] & 0x10 != 0
    self.protocol_version = rframe[1] & 0x03

    self.commandid = rframe[8]

    payload = rframe[HEADER_LEN:len(rframe) - CRC_LEN]
    self.crc = rframe[len(rframe) - CRC_LEN:]

    trailer_len = SEQ_LEN + MAC_LEN if self.protocol_mode == IoFrame.Oneway else MAC_LEN
    self.authenticated = self._looks_authenticated(len(payload), trailer_len)

    if self.authenticated:
      self.commanddata = payload[:len(payload) - trailer_len]
      if self.protocol_mode == IoFrame.Oneway:
        self.seqnum = payload[len(payload) - trailer_len:-MAC_LEN]
      self.mac = payload[-MAC_LEN:]
    else:
      self.commanddata = payload

  def _looks_authenticated(self, payload_len: int, trailer_len: int) -> bool:
    """
    Decide whether the payload ends in an authentication trailer.

    Nothing on the wire says so, so it has to be deduced from the command's
    documented payload length. Where that is ambiguous, assume a trailer: a
    plain frame read as authenticated fails its MAC check, whereas an
    authenticated frame read as plain silently hands parameters to the caller.
    """
    exact = FIXED_PAYLOAD_LEN.get(self.commandid)
    if exact is not None:
      if payload_len == exact + trailer_len:
        return True
      if payload_len == exact:
        return False

    minimum = MIN_PAYLOAD_LEN.get(self.commandid)
    if minimum is not None:
      if payload_len >= minimum + trailer_len:
        return True
      if payload_len >= minimum:
        return False

    return self.commandid not in UNAUTHENTICATED_COMMANDS and payload_len >= trailer_len

  def is_correct(self) -> bool:
    """Test whether the frame has a correct checksum"""
    # CRC-16/KERMIT over the frame including its own CRC comes out zero.
    return compute_crc_8408(self.raw) == 0

  def __str__(self) -> str:
    if not self.raw:
      return "None"

    hexs = lambda b: binascii.hexlify(b).decode() if b else "-"

    out = (
      f"io-homecontrol frame: length={self.length} "
      f"(size field {self.size_field} + {SIZE_FIELD_BIAS}), "
      f"from_addr={hexs(self.from_addr)}, to_addr={hexs(self.to_addr)}\n"
      f"  control byte 1=0x{self.control_byte1:02x} ("
      f"order={ORDER_NAMES.get(self.order, self.order)}, "
      f"protocol mode = {self.protocol_mode})\n"
      f"  control byte 2=0x{self.control_byte2:02x} ("
      f"use beacon={self.use_beacon}, routed={self.routed}, "
      f"low power mode={self.lpm}, ack={self.ack}, "
      f"protocol version={self.protocol_version})\n"
      f"  command ID=0x{self.commandid:02x}\n"
      f"  command data={hexs(self.commanddata)}\n"
    )
    if self.authenticated:
      if self.seqnum:
        out += f"  sequence={hexs(self.seqnum)}\n"
      out += f"  MAC={hexs(self.mac)}\n"
    else:
      out += "  no authentication trailer\n"
    out += f"  CRC={hexs(self.crc)} ({'correct' if self.is_correct() else 'incorrect'})\n"
    return out


def self_test() -> int:
  """Check the parser against the captures the repository documents."""
  failures = 0

  def check(label, got, want):
    nonlocal failures
    if got != want:
      print(f"FAIL {label}: got {got!r}, want {want!r}")
      failures += 1

  # scripts/io-homecontrol.ksy, SFD stripped. Execute with 8 payload bytes.
  ksy = bytes.fromhex(
    "f800" "00007f" "708758" "00"
    "0161d40080c80000" "3bd5" "05526875499c" "7e72")
  frame = IoFrame(ksy)
  check("ksy length", frame.length, 27)
  check("ksy mode", frame.protocol_mode, IoFrame.Oneway)
  check("ksy to_addr", frame.to_addr.hex(), "00007f")
  check("ksy from_addr", frame.from_addr.hex(), "708758")
  check("ksy command", frame.commandid, 0x00)
  check("ksy payload", frame.commanddata.hex(), "0161d40080c80000")
  check("ksy sequence", frame.seqnum.hex(), "3bd5")
  check("ksy mac", frame.mac.hex(), "05526875499c")
  check("ksy crc", frame.is_correct(), True)

  # docs/linklayer.md, 1W execute with the documented 6-byte payload.
  doc = bytes.fromhex(
    "f600" "00003f" "708758" "00"
    "0161d2000000" "3bd2" "e6b62cef54c8" "a937")
  frame = IoFrame(doc)
  check("doc length", frame.length, 25)
  check("doc to_addr", frame.to_addr.hex(), "00003f")
  check("doc from_addr", frame.from_addr.hex(), "708758")
  check("doc payload", frame.commanddata.hex(), "0161d2000000")
  check("doc sequence", frame.seqnum.hex(), "3bd2")
  check("doc crc", frame.is_correct(), True)

  # docs/linklayer.md, plain 2W discover: no trailer at all.
  disc = bytes.fromhex("c800" "00003b" "f00f00" "28" "1234")
  frame = IoFrame(disc)
  check("discover length", frame.length, 11)
  check("discover mode", frame.protocol_mode, IoFrame.TwoWay)
  check("discover authenticated", frame.authenticated, False)
  check("discover payload", frame.commanddata.hex(), "")

  # A frame whose size field disagrees with the bytes given must be rejected.
  try:
    IoFrame(ksy[:-1])
  except ValueError:
    pass
  else:
    print("FAIL truncated frame was accepted")
    failures += 1

  print("self-test: " + ("OK" if failures == 0 else f"{failures} failure(s)"))
  return 1 if failures else 0


def main(args):
  if len(args) < 2 or (len(args) == 2 and args[1] == "-f"):
    print(f"Usage: {args[0]} <hex encoded frame | -f file | --self-test>")
    return 1

  if args[1] == "--self-test":
    return self_test()

  raw_frames = []
  if args[1] == "-f":
    with open(args[2], "rt") as f:
      for line in f:
        line = line.split("#", 1)[0].strip()
        if line:                      # blank and comment lines are not frames
          raw_frames.append(line)
  else:
    raw_frames.append("".join(args[1:]))

  status = 0
  for rframe in raw_frames:
    try:
      print(IoFrame(bytes.fromhex(rframe)))
    except ValueError as exc:
      # One bad line in a capture file should not abandon the rest of it.
      print(f"Cannot parse {rframe!r}: {exc}")
      status = 1
  return status


if __name__ == "__main__":
  sys.exit(main(sys.argv))
