#!/usr/bin/env python3
"""Split a cleaned LuaJIT daemon dump into one .lua file per embedded module.

The daemon stores its Lua modules as `name = "<escaped source>"` lines. This
walks those lines, unescapes each value and writes it out under the module
name, ready to be fed to the decompiler.

Usage:
    python3 luajit-convertNested.py IoHomecontrold.lua [output_dir]

Note: this assumes the LuaJIT daemon has already been decompiled *and* cleaned.
"""

import argparse
import codecs
import sys
from pathlib import Path

DECOMPILE_HINT = r"""
Use this PowerShell snippet afterwards to decompile the extracted files:

    gci -File -Filter "*.lua" |
        % { sleep -Milliseconds 250; .\luajit-decompiler.exe $_.FullName -s }
"""


def convert(source: Path, output_dir: Path) -> int:
    """Write one file per `name = "..."` line. Returns the number written."""
    output_dir.mkdir(parents=True, exist_ok=True)
    written = 0

    with source.open("r", encoding="utf-8") as handle:
        for lineno, readline in enumerate(handle, 1):
            if " = " not in readline:
                # Blank lines and stray text in a hand-cleaned dump are normal;
                # skipping them beats aborting halfway through the extraction.
                continue

            name, _, value = readline.partition(" = ")
            target = output_dir / (name.strip() + ".lua")

            data, _consumed = codecs.escape_decode(value)  # type: ignore[attr-defined]
            data = data[:-1]  # drop the trailing quote

            if target.exists():
                print(f"line {lineno}: {target} already exists, skipping", file=sys.stderr)
                continue

            target.write_bytes(data)
            written += 1

    return written


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path,
                        help="cleaned LuaJIT daemon dump, e.g. IoHomecontrold.lua")
    parser.add_argument("output_dir", type=Path, nargs="?", default=Path("."),
                        help="where to write the extracted modules (default: .)")
    args = parser.parse_args()

    if not args.source.is_file():
        print(f"error: {args.source} is not a file", file=sys.stderr)
        return 1

    written = convert(args.source, args.output_dir)
    print(f"Extracted {written} module(s) to {args.output_dir}")
    print(DECOMPILE_HINT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
