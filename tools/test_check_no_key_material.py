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

import importlib.util
import os
import sys

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


def fires_on(text):
    hits = []
    for line in text.splitlines():
        for seq in g.hex_candidates(line):
            g.scan_bytes(seq, "<t>", "hex", hits)
    for seq in g.hex_candidates(text):
        g.scan_bytes(seq, "<t>", "ml", hits)
    for seq in g.base64_candidates(text):
        g.scan_bytes(seq, "<t>", "b64", hits)
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
    check("live repo scan is clean", g.main() == 0)

    print("\nALL SCANNER TESTS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
