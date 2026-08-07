#!/usr/bin/env python3
"""Fail if any real io-homecontrol key material is committed to the repo.

A 1W key transfer (command 0x30) and a 2W key transfer (command 0x32) both carry
the installation's system key masked only with the *public* transfer key, so
committing a real capture of one - in docs, tests, captures, anywhere - lets
anyone recover that installation's key (see
docs/devices/velux/velux-frame-analysis.md). This has slipped through redaction
twice, each time hiding in a hex representation an earlier grep did not cover.

This check is structural and encoding-agnostic. For every tracked text file it
pulls out candidate byte sequences - hex written with any mix of separators
(spaces, newlines, commas, colons, hyphens, underscores, `0x`/`\\x` prefixes) and
base64 runs - and interprets each both as a de-framed frame and as framed UART
wire (de-framing it first), scanning every command-byte position. It flags any
frame that is a key transfer (command 0x30 or 0x32) **and validates its
CRC-16/KERMIT** - the CRC is what distinguishes a real frame from a coincidental
hex alignment (the AES S-box, ASCII text, MAC tails of ordinary commands, etc.
never validate). Non-text files (images, PDFs, vendor firmware) are out of scope:
a frame committed as data lives in a text form, and scanning arbitrary binaries
only yields coincidental CRC-valid windows.

The ONLY key-transfer frames allowed are those addressed from a documentation
placeholder node (AB CD EF for the 1W worked example / this repo's synthetic
vector; FE EF EE and F0 0F 00 for the 2W worked examples). A real capture has a
real source address and so fails the check.

Run: python3 tools/check_no_key_material.py   (exit 1 on any finding)
"""

import base64
import re
import subprocess
import sys

# Documentation / synthetic placeholder source addresses. A real remote never
# uses these, so a key-transfer frame from one is by definition not a secret.
PLACEHOLDER_SRC = {
    bytes([0xAB, 0xCD, 0xEF]),   # 1W worked example (linklayer.md) + synthetic test vector
    bytes([0xFE, 0xEF, 0xEE]),   # 2W worked example
    bytes([0xF0, 0x0F, 0x00]),   # 2W worked example (controller)
}

KEY_TRANSFER_CMDS = {0x30, 0x32}  # 1W send-key, 2W key-transfer
CTRL0_LENGTH_MASK = 0x1F
FRAME_SIZE_FIELD_BIAS = 3
CMD_OFFSET = 8                    # ctrl0 ctrl1 dest[3] src[3] cmd


def crc16_kermit(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if (crc & 1) else (crc >> 1)
    return crc


def frame_crc_ok(frame: bytes) -> bool:
    # Trailing two bytes are the CRC, little-endian (LSB first).
    return len(frame) >= 3 and crc16_kermit(frame[:-2]) == (frame[-2] | (frame[-1] << 8))


def deframe(wire: bytes) -> bytes:
    """Strip UART framing: 10-bit cells, start 0, 8 data LSB-first, stop 1."""
    bits = []
    for b in wire:
        for i in range(7, -1, -1):
            bits.append((b >> i) & 1)
    out = bytearray()
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


def targeted_scan(stream: bytes, name: str, kind: str, hits: list) -> bool:
    """Linear scan: at every position whose command byte is a key transfer, take
    the frame the size field declares and flag it if the CRC validates and the
    source is not a placeholder. O(n) with a CRC check only at candidate starts."""
    n = len(stream)
    for i in range(0, n - CMD_OFFSET - 3):
        if stream[i + CMD_OFFSET] not in KEY_TRANSFER_CMDS:
            continue
        declared = (stream[i] & CTRL0_LENGTH_MASK) + FRAME_SIZE_FIELD_BIAS
        if declared < 27 or i + declared > n:
            continue
        candidate = stream[i:i + declared]
        if not frame_crc_ok(candidate):
            continue
        src = candidate[5:8]
        if src not in PLACEHOLDER_SRC:
            hits.append((name, f"{kind}@{i}", src.hex(), candidate[:16].hex()))
            return True
    return False


def scan_bytes(seq: bytes, name: str, kind: str, hits: list) -> None:
    # A committed frame is either the de-framed frame itself, or the on-air UART
    # wire. Try both: de-framed anywhere in the stream, and the stream de-framed
    # from its start (a framed frame on its own line/token).
    if targeted_scan(seq, name, kind + "/de-framed", hits):
        return
    df = deframe(seq)
    if len(df) >= CMD_OFFSET + 4:
        targeted_scan(df, name, kind + "/framed", hits)


def hex_candidates(text: str):
    # Drop 0x / \x prefixes, then treat runs of hex digits + common separators as
    # one region; strip the separators to recover the byte stream.
    text = re.sub(r"0x|\\x", "", text, flags=re.IGNORECASE)
    for m in re.finditer(r"[0-9a-fA-F][0-9a-fA-F\s,:_\-]{38,}[0-9a-fA-F]", text):
        raw = re.sub(r"[^0-9a-fA-F]", "", m.group(0))
        if len(raw) % 2:
            raw = raw[:-1]
        if len(raw) >= 40:
            try:
                yield bytes.fromhex(raw)
            except ValueError:
                pass


def base64_candidates(text: str):
    for m in re.finditer(r"[A-Za-z0-9+/]{40,}={0,2}", text):
        s = m.group(0)
        try:
            yield base64.b64decode(s + "=" * (-len(s) % 4), validate=True)
        except Exception:  # noqa: BLE001 - best-effort decode, skip anything invalid
            pass


def main() -> int:
    files = subprocess.check_output(["git", "ls-files"]).decode().split()
    hits: list = []
    for f in files:
        try:
            text = open(f, "rb").read().decode("utf-8")
        except (OSError, UnicodeDecodeError):
            # Non-text file. A frame committed as data lives in a text form (hex or
            # base64); scanning arbitrary binaries (PDFs, images, vendor firmware)
            # only yields coincidental CRC-valid windows. Binary capture blobs are
            # out of scope by design - see the module docstring.
            continue
        # Per line: catches a framed frame on its own line (de-framed from the
        # line's start) and any single-line de-framed frame, in any separator
        # style. Whole text: catches a de-framed frame wrapped across lines.
        for line in text.splitlines():
            for seq in hex_candidates(line):
                scan_bytes(seq, f, "hex", hits)
        for seq in hex_candidates(text):
            scan_bytes(seq, f, "hex/multiline", hits)
        for seq in base64_candidates(text):
            scan_bytes(seq, f, "base64", hits)

    if hits:
        print("FAIL: real key-transfer (0x30/0x32) frame(s) committed to the repo:")
        for name, where, src, sample in hits:
            print(f"  {name}  [{where}, src {src}]  {sample}...")
        print()
        print("Such a frame carries the installation key masked only with the public")
        print("transfer key. Redact it, or replace it with a synthetic frame addressed")
        print("from a placeholder (AB CD EF etc.); see test/test_phy_framing for how.")
        return 1

    print("OK: no real 0x30/0x32 key-transfer frames committed (placeholder src only).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
