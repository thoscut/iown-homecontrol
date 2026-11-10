#!/usr/bin/env python3
"""Tests for tools/check_no_key_material.py.

The scanner is a security guard, but on the committed repo it always exits 0
(there is no real key material), so nothing would notice if its detection logic
regressed - and that logic has been rewritten several times. These fixtures make
a regression trip: a real (non-placeholder) key-transfer frame must be flagged, a
placeholder/synthetic vector must not, a text file with a lone NUL must still be
scanned, and a genuine binary must be skipped.

Run: python3 tools/test_check_no_key_material.py   (exit 1 on failure)
"""

import base64
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    "check_no_key_material", os.path.join(HERE, "check_no_key_material.py"))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)


def make_0x30(src, cmd=0x30, key=None):
    """Build a valid de-framed 1W/2W key-transfer frame from src, with a real CRC."""
    if key is None:
        key = list(range(16))
    # ctrl0 ctrl1 | dest[3] | src[3] | cmd | key[16] | mfr | reserved | crc[2]
    body = bytearray([0x00, 0x00, 0x00, 0x00, 0x00]
                     + list(src) + [cmd] + list(key) + [0x01, 0x00])
    body[0] = (len(body) + 2 - 3) & g.CTRL0_LENGTH_MASK  # size field -> frame_len
    crc = g.crc16_kermit(bytes(body))
    return bytes(body) + bytes([crc & 0xFF, (crc >> 8) & 0xFF])


def uart_frame(frame):
    """UART-frame bytes the way they go on air: start 0, 8 data bits LSB first,
    stop 1, packed MSB first (the inverse of the scanner's deframe())."""
    bits = []
    for b in frame:
        bits += [0] + [(b >> k) & 1 for k in range(8)] + [1]
    bits += [1] * (-len(bits) % 8)   # idle line
    return bytes(sum(bit << (7 - k) for k, bit in enumerate(bits[i:i + 8]))
                 for i in range(0, len(bits), 8))


def fires_on(text):
    # Same entry point main() uses per file, so every extractor is exercised.
    hits = []
    g.scan_text(text, "<t>", hits)
    return len(hits) > 0


def check(name, cond):
    if not cond:
        print(f"FAIL: {name}")
        raise SystemExit(1)
    print(f"ok: {name}")


def main():
    # (0) Known-answer CRC vector - breaks the circularity of every other check.
    #     make_0x30 builds its fixtures with g.crc16_kermit and the scanner
    #     validates with the same function, so a bug in crc16_kermit would move
    #     both sides together and every "flagged / not flagged" assertion below
    #     would still pass against a broken CRC. This vector is computed offline
    #     from the CRC-16/KERMIT definition (poly 0x1021 reflected = 0x8408, init
    #     0, refin/refout, xorout 0), cross-checked with an independent non-
    #     reflected 0x1021 implementation, and hard-coded here. If crc16_kermit
    #     ever diverges from true CRC-16/KERMIT, this trips regardless of the
    #     fixture builder.
    #     (kat_body is the frame BODY without its CRC trailer - 27 bytes, too
    #     short for the size field it declares, so the scanner never mistakes
    #     this literal for a committed frame; the full CRC-valid frame is built
    #     only at runtime.)
    kat_body = bytes.fromhex(
        "1a0000000012345630000102030405060708090a0b0c0d0e0f0100")
    check("crc16_kermit known-answer", g.crc16_kermit(kat_body) == 0x57AC)
    # Tie the known answer to make_0x30: its body must equal kat_body, and it must
    # append exactly the CRC-16/KERMIT trailer (0x57AC -> ac 57, little-endian).
    built = make_0x30(bytes([0x12, 0x34, 0x56]))
    check("make_0x30 body matches the known-answer body", built[:-2] == kat_body)
    check("make_0x30 appends the known-answer CRC trailer",
          built[-2:] == bytes([0xAC, 0x57]))

    real = make_0x30(bytes([0x12, 0x34, 0x56]))            # non-placeholder src
    real_hex = real.hex()

    # (a) a real 0x30 from a non-placeholder address is flagged, in several forms.
    check("real 0x30 contiguous flagged", fires_on(real_hex))
    check("real 0x30 spaced flagged",
          fires_on(" ".join(real_hex[i:i + 2] for i in range(0, len(real_hex), 2))))
    check("real 0x30 comma/0x flagged",
          fires_on("{" + ",".join("0x" + real_hex[i:i + 2]
                                  for i in range(0, len(real_hex), 2)) + "}"))
    # The 2W key transfer (command 0x32) carries a key the same way and must also
    # be flagged. (The framed on-air form is covered by the C++ phy-framing suite.)
    check("real 2W 0x32 flagged",
          fires_on(make_0x30(bytes([0x12, 0x34, 0x56]), cmd=0x32).hex()))

    # (a1) the framed on-air form, which is what a raw capture or a sniffer log
    #      actually contains - on its own, and behind bytes that are not part of
    #      the frame.
    wire = uart_frame(real)
    check("uart_frame round-trips through deframe", g.deframe(wire) == real)
    wire_hex = " ".join(f"{b:02X}" for b in wire)
    check("framed real 0x30 flagged", fires_on(wire_hex))
    check("framed real 0x30 behind preamble and sync word flagged",
          fires_on("55 55 55 55 FF 33 " + wire_hex))
    #   - the ESPHome component's own VERBOSE line: the rssi digits join the hex
    #     region, so the capture arrives with a stray leading byte.
    check("framed real 0x30 in an ESPHome RAW log line flagged",
          fires_on(f"[V][iown_homecontrol:260]: RAW {len(wire)} bytes rssi=-71: {wire_hex}"))
    check("framed placeholder 0x30 not flagged",
          not fires_on("55 55 FF 33 " + " ".join(
              f"{b:02X}" for b in uart_frame(make_0x30(bytes([0xAB, 0xCD, 0xEF]))))))

    # (a2) encodings an earlier version missed (reviewer evasion probes).
    #   - base64 of the shortest key frames (27/29 bytes -> 36/40 chars incl. pad)
    for klen in (14, 16):   # 27- and 29-byte frames
        f = make_0x30(bytes([0x12, 0x34, 0x56]), key=list(range(klen)))
        b64 = base64.b64encode(f).decode()
        check(f"real {len(f)}-byte frame as base64 ({len(b64)} chars) flagged",
              fires_on(b64))
        check(f"real {len(f)}-byte frame as base64 glued to text flagged",
              fires_on("key" + b64))
    #   - a hex run starting half a byte early ("Frame: " ends in hex 'e')
    spaced = " ".join(f"{b:02x}" for b in real)
    check("hex run off by a nibble flagged", fires_on("Frame: " + spaced))
    #   - xxd and hexdump -C output (offset + ASCII columns)
    xxd = "\n".join(
        f"{i:08x}: " + " ".join(real[j:j + 2].hex() for j in range(i, min(i + 16, len(real)), 2))
        + "  ..4V0..........." for i in range(0, len(real), 16))
    check("xxd dump flagged", fires_on(xxd))
    hexdump = "\n".join(
        f"{i:08x}  " + " ".join(f"{b:02x}" for b in real[i:i + 8]) + "  "
        + " ".join(f"{b:02x}" for b in real[i + 8:i + 16]) + "  |..4V0...|"
        for i in range(0, len(real), 16))
    check("hexdump -C dump flagged", fires_on(hexdump))
    #   - C arrays with a comment on every row
    for open_c, close_c in (("// row", ""), ("# row", ""), ("/* row", " */")):
        rows = "\n".join(", ".join(f"0x{b:02x}" for b in real[i:i + 8])
                         + f",  {open_c} {i // 8}{close_c}"
                         for i in range(0, len(real), 8))
        check(f"C array with '{open_c.split()[0]}' row comments flagged",
              fires_on("{\n" + rows + "\n}"))
    #   - main() reads paths with spaces / non-ASCII (git ls-files -z) and fails
    #     on an unreadable tracked file instead of skipping it.
    with tempfile.TemporaryDirectory() as tmp:
        def git(*a):
            subprocess.run(["git", "-C", tmp, *a], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        git("init", "-q")
        path = os.path.join(tmp, "caps dir", "capture \u00e9.txt")
        os.makedirs(os.path.dirname(path))
        with open(path, "w") as fh:
            fh.write(real_hex + "\n")
        git("add", "-A")
        cwd = os.getcwd()
        os.chdir(tmp)
        try:
            check("path with space/non-ASCII is scanned (flagged)", g.main() == 1)
            with open(path, "w") as fh:
                fh.write("clean\n")
            check("clean temp repo passes", g.main() == 0)
            os.remove(path)   # tracked but unreadable now
            check("unreadable tracked file fails the check", g.main() == 1)
        finally:
            os.chdir(cwd)

    # (b) placeholder-source frames (docs worked example / synthetic vector) pass.
    check("placeholder ABCDEF 0x30 not flagged",
          not fires_on(make_0x30(bytes([0xAB, 0xCD, 0xEF])).hex()))
    check("placeholder FEEFEE 0x32 not flagged",
          not fires_on(make_0x30(bytes([0xFE, 0xEF, 0xEE]), cmd=0x32).hex()))

    # (c) a text file with one embedded NUL is NOT treated as binary, and its
    #     real frame is still found (the round-4 regression this guards against).
    nul_text = b"header\x00 notes about the capture\n" + real_hex.encode() + b"\n"
    check("lone-NUL text not binary", not g.looks_binary(nul_text))
    check("real 0x30 in lone-NUL text flagged",
          fires_on(nul_text.decode("utf-8", "replace")))

    # (d) a control-byte-heavy blob is treated as binary and skipped.
    check("control-heavy blob is binary", g.looks_binary(bytes(range(32)) * 100))
    check("mostly-ASCII text is not binary", not g.looks_binary(b"the quick brown fox " * 50))

    # The live repo must be clean (belt and suspenders on the real tree).
    os.chdir(os.path.dirname(HERE))
    check("live repo scan is clean", g.main() == 0)

    print("\nALL SCANNER TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
