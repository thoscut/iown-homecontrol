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
wire (de-framing it first, from every bit offset, so whatever sits ahead of the
frame - a preamble and sync word, digits of a log prefix that read as hex, or
the odd bits of a capture that did not start on a byte boundary - does not hide
it), scanning every command-byte position. It flags any
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
import os
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
MIN_KEY_FRAME = 27                # shortest key transfer worth flagging (bytes)
MAX_FRAME = 34                    # longest frame the size field can declare
# Bytes of UART wire the shortest key frame occupies (10 bits per byte).
MIN_KEY_FRAME_WIRE = (MIN_KEY_FRAME * 10 + 7) // 8
_KEY_CMD_RE = re.compile(b"[\\x30\\x32]")


def _byte_class(mask: int, value: int) -> bytes:
    return b"[" + b"".join(re.escape(bytes([b])) for b in range(256)
                           if b & mask == value) + b"]"


# Where UART wire could begin: four consecutive well-framed cells span exactly
# five bytes, with start bits (0) at bit 0, 10, 20, 30 and stop bits (1) at bit
# 9, 19, 29, 39. Matching that as five byte classes lets the regex engine
# reject 255 offsets in 256 before any Python runs; the lookahead makes the
# matches overlap, so every byte offset is tried.
_FRAMED_START_RE = re.compile(
    b"(?=" + _byte_class(0x80, 0x00) + _byte_class(0x60, 0x40)
    + _byte_class(0x18, 0x10) + _byte_class(0x06, 0x04)
    + _byte_class(0x01, 0x01) + b")", re.DOTALL)


def looks_binary(data: bytes) -> bool:
    """A file is treated as binary when a meaningful fraction of a leading sample
    is C0 control bytes (excluding tab/newline/CR). One stray byte or one embedded
    NUL in an otherwise-text file stays well under the threshold and is scanned."""
    sample = data[:65536]
    if not sample:
        return False
    text_controls = {0x09, 0x0A, 0x0D}
    control = sum(1 for b in sample if b < 0x20 and b not in text_controls)
    return control / len(sample) > 0.10


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


def deframe(wire: bytes, start: int = 0, max_cells: int = 0) -> bytes:
    """Strip UART framing: 10-bit cells, start 0, 8 data LSB-first, stop 1.
    Begins at byte `start` and stops at the first invalid cell (or after
    `max_cells` cells when given); bits are read on demand, so a long stream
    that is not framed costs only a few cells."""
    def bit(p):
        return (wire[p >> 3] >> (7 - (p & 7))) & 1
    out = bytearray()
    total = len(wire) * 8
    pos = start * 8
    while pos + 10 <= total and (max_cells == 0 or len(out) < max_cells):
        if bit(pos) != 0 or bit(pos + 9) != 1:
            break
        val = 0
        for k in range(8):
            val |= bit(pos + 1 + k) << k
        out.append(val)
        pos += 10
    return bytes(out)


def targeted_scan(stream: bytes, name: str, kind: str, hits: list) -> bool:
    """Linear scan: at every position whose command byte is a key transfer, take
    the frame the size field declares and flag it if the CRC validates and the
    source is not a placeholder. O(n) with a CRC check only at candidate starts."""
    n = len(stream)
    # Jump straight to command-byte candidates (C-speed search) rather than
    # testing every offset in Python.
    for m in _KEY_CMD_RE.finditer(stream, CMD_OFFSET):
        i = m.start() - CMD_OFFSET
        if i >= n - CMD_OFFSET - 3:
            break
        declared = (stream[i] & CTRL0_LENGTH_MASK) + FRAME_SIZE_FIELD_BIAS
        if declared < MIN_KEY_FRAME or i + declared > n:
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
    if len(df) >= MIN_KEY_FRAME and targeted_scan(df, name, kind + "/framed", hits):
        return
    # The wire form can also sit behind things that are not part of it: the
    # preamble and sync word of an SDR dump, or digits a log prefix contributes
    # to the hex region - the ESPHome component's own "RAW 54 bytes rssi=-71:
    # .." line reads as the capture with a 0x71 glued in front. And it need not
    # start on a byte boundary at all: a demodulator's bit dump begins wherever
    # it locked on, so a row that opens on a 1 of the 0x55 preamble ("AA AA ..")
    # has every frame byte straddling two dump bytes. So de-frame from every
    # bit offset: the stream is shifted left one bit at a time and searched for
    # places where framed cells could begin, each attempt capped at two frames'
    # worth of cells.
    n = len(seq)
    as_int = int.from_bytes(seq, "big")
    keep = (1 << (8 * n)) - 1
    for shift in range(8):
        stream = seq if shift == 0 else ((as_int << shift) & keep).to_bytes(n, "big")
        for m in _FRAMED_START_RE.finditer(stream, 0 if shift else 1):
            off = m.start()
            if off > n - MIN_KEY_FRAME_WIRE:
                break
            df = deframe(stream, off, 2 * MAX_FRAME)
            if len(df) >= MIN_KEY_FRAME and targeted_scan(
                    df, name, f"{kind}/framed+{off}.{shift}", hits):
                return


def hex_candidates(text: str):
    # Drop 0x / \x prefixes, then treat runs of hex digits + common separators as
    # one region; strip the separators to recover the byte stream.
    text = re.sub(r"0x|\\x", "", text, flags=re.IGNORECASE)
    for m in re.finditer(r"[0-9a-fA-F][0-9a-fA-F\s,:_|.;/\-]{38,}[0-9a-fA-F]", text):
        raw = re.sub(r"[^0-9a-fA-F]", "", m.group(0))
        # A region can start half a byte early: a stray hex letter just before the
        # frame ("Frame: 1a 00 ..." yields "e1a00...") shifts every byte by a
        # nibble. Yield both nibble alignments so the frame is found either way.
        for start in (0, 1):
            r = raw[start:]
            if len(r) % 2:
                r = r[:-1]
            if len(r) >= 40:
                try:
                    yield bytes.fromhex(r)
                except ValueError:
                    pass


# A hex-dump line: an offset column (xxd "00000010: ", hexdump -C "00000010  ",
# od-style), then the hex bytes, then an ASCII column. The offset and ASCII
# columns would otherwise be read as frame bytes (or break the hex region), so a
# frame pasted as a dump slipped through.
_DUMP_OFFSET = re.compile(r"^\s*(?:0x)?[0-9a-fA-F]{4,16}:?\s+")
_DUMP_ASCII_PIPE = re.compile(r"\|[^|]*\|?\s*$")
_DUMP_HEX = re.compile(r"(?:[0-9a-fA-F]{2}){1,2}(?=\s|$)")


def strip_dump_columns(text: str) -> str:
    """Rewrite every hex-dump-looking line to just its hex bytes (offset and
    ASCII columns removed); other lines pass through unchanged."""
    out = []
    for line in text.splitlines():
        m = _DUMP_OFFSET.match(line)
        if not m:
            out.append(line)
            continue
        body = _DUMP_ASCII_PIPE.sub("", line[m.end():])
        groups = []
        pos = 0
        # Take consecutive 1- or 2-byte groups from the start; the first token
        # that is not one (the xxd ASCII column, typically) ends the hex area.
        while True:
            while pos < len(body) and body[pos] in " \t":
                pos += 1
            g = _DUMP_HEX.match(body, pos)
            if not g:
                break
            groups.append(g.group(0))
            pos = g.end()
        out.append(" ".join(groups) if groups else line)
    return "\n".join(out)


def strip_comments(text: str) -> str:
    """Drop C block comments and //, # line comments: a C array with a comment on
    every row ("0x1a, 0x00, ...  // header") otherwise breaks the multi-line hex
    region at the comment text."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return "\n".join(re.sub(r"(//|#).*$", "", line) for line in text.splitlines())


def base64_candidates(text: str):
    # 36 chars is the shortest encoding of the shortest key frame (27 bytes ->
    # 36 chars, padding excluded). Try every 4-char alignment, since a run can be
    # glued to preceding base64-alphabet text ("key" + base64...).
    for m in re.finditer(r"[A-Za-z0-9+/]{36,}={0,2}", text):
        s = m.group(0)
        for start in range(4):
            t = s[start:]
            if len(t.rstrip("=")) < 36:
                break
            try:
                yield base64.b64decode(t + "=" * (-len(t) % 4), validate=True)
            except Exception:  # noqa: BLE001 - best-effort decode, skip anything invalid
                pass


def scan_text(text: str, name: str, hits: list) -> None:
    """Run every candidate extractor over one file's text."""
    already = len(hits)
    # Per line: catches a framed frame on its own line (de-framed from the
    # line's start) and any single-line de-framed frame, in any separator
    # style. Whole text: catches a de-framed frame wrapped across lines.
    for line in text.splitlines():
        for seq in hex_candidates(line):
            scan_bytes(seq, name, "hex", hits)
    for seq in hex_candidates(text):
        scan_bytes(seq, name, "hex/multiline", hits)
    # Derived views: hex dumps with their offset/ASCII columns removed, and the
    # text with code comments removed. Only rescanned when they differ, and
    # only when the raw passes found nothing (one report per frame is enough).
    if len(hits) > already:
        return
    for kind, derived in (("hexdump", strip_dump_columns(text)),
                          ("uncommented", strip_comments(text))):
        if derived != text:
            for seq in hex_candidates(derived):
                scan_bytes(seq, name, kind, hits)
    for seq in base64_candidates(text):
        scan_bytes(seq, name, "base64", hits)


def main() -> int:
    # -z: NUL-separated, unquoted paths. Splitting the default output on
    # whitespace silently dropped paths with spaces, and git C-quotes non-ASCII
    # paths, which then failed to open and were skipped.
    out = subprocess.check_output(["git", "ls-files", "-z"])
    files = [os.fsdecode(p) for p in out.split(b"\0") if p]
    hits: list = []
    errors: list = []
    for f in files:
        try:
            with open(f, "rb") as fh:
                data = fh.read()
        except OSError as e:
            # An unreadable tracked file is unscanned, so it cannot be passed.
            # (A tracked file deleted in the working tree is reported too.)
            errors.append((f, e))
            continue
        # Skip genuinely binary files by CONTENT RATIO, not by the mere presence of
        # a NUL: a real binary (PDF, image, firmware) is mostly control bytes, while
        # a text file with one stray non-UTF-8 byte or one embedded NUL is not - and
        # that text file must still be scanned (skipping it on the NUL alone was a
        # coverage regression). This also keeps the scan fast: without it, decoding
        # megabyte binaries to U+FFFD-filled text and scanning them is slow for no
        # benefit (their raw-byte frames do not survive the decode anyway).
        if looks_binary(data):
            continue
        # Decode leniently rather than skipping any file. errors="replace" turns
        # each invalid byte into U+FFFD, which is neither a hex digit nor a
        # separator, so a genuine binary (PDF, image, firmware) has its raw-byte
        # frames broken up and yields no candidates - while a text file that merely
        # carries a stray non-UTF-8 byte or an embedded NUL (both of which used to
        # get the whole file skipped) is still fully scanned. A NUL is valid UTF-8
        # (U+0000) and left in place. Skipping on either condition let a real frame
        # ride through while CI printed OK - the pattern this guard exists to stop.
        text = data.decode("utf-8", errors="replace")
        scan_text(text, f, hits)

    if errors:
        print("FAIL: could not read tracked file(s); they were not scanned:")
        for name, e in errors:
            print(f"  {name}: {e}")
    if hits:
        print("FAIL: real key-transfer (0x30/0x32) frame(s) committed to the repo:")
        for name, where, src, sample in hits:
            print(f"  {name}  [{where}, src {src}]  {sample}...")
        print()
        print("Such a frame carries the installation key masked only with the public")
        print("transfer key. Redact it, or replace it with a synthetic frame addressed")
        print("from a placeholder (AB CD EF etc.); see test/test_phy_framing for how.")
        return 1
    if errors:
        return 1

    print("OK: no real 0x30/0x32 key-transfer frames committed (placeholder src only).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
