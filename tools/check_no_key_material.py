#!/usr/bin/env python3
"""Fail if any real io-homecontrol key material is committed to the repo.

A 1W key transfer (command 0x30) carries the installation's system key masked
only with the *public* transfer key, so committing a real capture of one - in
either the on-air framed form or the de-framed frame - lets anyone recover that
installation's key (see docs/devices/velux/velux-frame-analysis.md). This has
now slipped through redaction twice, each time hiding in a hex representation an
earlier grep did not cover (spaced vs. contiguous, upper vs. lower case, framed
vs. de-framed). This check closes that off structurally.

It scans every tracked text file, pulls out every hex run, interprets it both as
a de-framed frame and as framed UART wire (de-framing it first), and flags any
that decode to a 0x30 key transfer. The ONLY 0x30 frames allowed are those
addressed from the documentation placeholder node `AB CD EF` - the address the
KLF 200 / linklayer.md worked example and this repo's synthetic test vector use.
A real capture has a real source address and so fails the check.

Run: python3 tools/check_no_key_material.py   (exit 1 on any finding)
"""

import re
import subprocess
import sys

# The documented example address (docs/linklayer.md 1W key-exchange worked
# example) and the synthetic test vector both use this. A real remote never
# would, so a 0x30 frame from AB CD EF is by definition not a real secret.
PLACEHOLDER_SRC = bytes([0xAB, 0xCD, 0xEF])

CMD_KEY_TRANSFER_1W = 0x30
CTRL0_ONE_WAY_MASK = 0x20


def deframe(wire: bytes) -> bytes:
    """Strip UART start/stop framing: 10-bit cells, start 0, 8 data LSB-first, stop 1."""
    bits = []
    for b in wire:
        for i in range(7, -1, -1):
            bits.append((b >> i) & 1)
    out = []
    pos = 0
    while pos + 10 <= len(bits):
        if bits[pos] != 0 or bits[pos + 9] != 1:
            break
        val = 0
        for k in range(8):
            val |= bits[pos + 1 + k] << k
        out.append(val)
        pos += 10
    return bytes(out)


def is_key_transfer(frame: bytes) -> bool:
    # ctrl0 has the 1W bit set; command byte (offset 8) is 0x30; long enough to
    # carry the 16-byte masked key.
    return (
        len(frame) >= 27
        and (frame[0] & CTRL0_ONE_WAY_MASK)
        and frame[8] == CMD_KEY_TRANSFER_1W
    )


def source_addr(frame: bytes) -> bytes:
    # ctrl0 ctrl1 | dest(3) | src(3) | cmd ...
    return frame[5:8]


def scan_text(name: str, text: str, hits: list) -> None:
    for m in re.finditer(r"(?:[0-9a-fA-F]{2}[\s:]?){20,}", text):
        raw = re.sub(r"[^0-9a-fA-F]", "", m.group(0))
        if len(raw) % 2:
            raw = raw[:-1]
        if len(raw) < 40:
            continue
        try:
            b = bytes.fromhex(raw)
        except ValueError:
            continue
        for label, frame in (("de-framed", b), ("framed", deframe(b))):
            if is_key_transfer(frame) and source_addr(frame) != PLACEHOLDER_SRC:
                hits.append((name, label, source_addr(frame).hex(), raw[:64]))


def main() -> int:
    files = subprocess.check_output(["git", "ls-files"]).decode().split()
    hits: list = []
    for f in files:
        try:
            text = open(f, "rb").read().decode("utf-8", "ignore")
        except OSError:
            continue
        scan_text(f, text, hits)

    if hits:
        print("FAIL: real 1W key-transfer (0x30) frame(s) committed to the repo:")
        for name, label, src, sample in hits:
            print(f"  {name}  [{label}, src {src}]  {sample}...")
        print()
        print("A 0x30 frame carries the installation key masked only with the public")
        print("transfer key. Redact it, or replace it with a synthetic frame addressed")
        print("from the placeholder AB CD EF (see test/test_phy_framing for how).")
        return 1

    print("OK: no real 0x30 key-transfer frames committed (placeholder AB CD EF only).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
