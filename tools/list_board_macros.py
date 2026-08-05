#!/usr/bin/env python3
"""
List the `-DARDUINO_*` macro each PlatformIO board defines.

`src/board_pins.h` selects its pin map on those macros, and they are not
guessable: the platform spells them `ARDUINO_TTGO_LoRa32_v21new`,
`ARDUINO_T_Beam` and `ARDUINO_heltec_wifi_lora_32_V3`, mixing case in three
different ways. Four entries in that table were keyed on the spelling a human
would expect rather than the one PlatformIO emits, so those boards silently
fell through to "unknown board".

Building every board environment is what actually catches that (see the
`boards` job in .github/workflows/platformio.yml). This script is for looking
up the right spelling in the first place.

Usage:
    tools/list_board_macros.py                 # every espressif32 board
    tools/list_board_macros.py heltec ttgo     # only boards matching a filter
    tools/list_board_macros.py --check         # compare against board_pins.h
"""

import argparse
import json
import os
import re
import sys
from pathlib import Path

PLATFORM_DIRS = [
    Path.home() / ".platformio" / "platforms" / "espressif32" / "boards",
    Path.home() / ".platformio" / "platforms" / "espressif32@src" / "boards",
]

REPO_ROOT = Path(__file__).resolve().parent.parent
BOARD_PINS = REPO_ROOT / "src" / "board_pins.h"


def find_boards_dir() -> Path:
    for candidate in PLATFORM_DIRS:
        if candidate.is_dir():
            return candidate
    print(
        "No espressif32 boards directory found. Install the platform first:\n"
        "    pio pkg install --global --platform espressif32",
        file=sys.stderr,
    )
    raise SystemExit(1)


def board_macros(boards_dir: Path):
    """Yield (board_id, mcu, [macros]) for every board manifest."""
    for path in sorted(boards_dir.glob("*.json")):
        try:
            manifest = json.loads(path.read_text())
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{path.name}: cannot read ({exc})", file=sys.stderr)
            continue

        build = manifest.get("build", {})
        flags = build.get("extra_flags", "")
        if isinstance(flags, list):
            flags = " ".join(flags)

        macros = [
            flag[2:]
            for flag in str(flags).split()
            if flag.startswith("-DARDUINO_")
            # These say nothing about which board it is.
            and not flag.startswith(("-DARDUINO_USB", "-DARDUINO_RUNNING", "-DARDUINO_EVENT"))
        ]
        yield path.stem, build.get("mcu", "?"), macros


def macros_used_by_board_pins() -> set:
    if not BOARD_PINS.is_file():
        print(f"{BOARD_PINS} not found", file=sys.stderr)
        raise SystemExit(1)
    text = BOARD_PINS.read_text()
    return set(re.findall(r"defined\((ARDUINO_[A-Za-z0-9_]+)\)", text))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("filters", nargs="*",
                        help="case-insensitive substrings; a board matches if any hits")
    parser.add_argument("--check", action="store_true",
                        help="report macros in board_pins.h that no board defines")
    args = parser.parse_args()

    boards_dir = find_boards_dir()
    rows = list(board_macros(boards_dir))

    defined = {macro for _, _, macros in rows for macro in macros}

    if args.check:
        used = macros_used_by_board_pins()
        unknown = sorted(used - defined)
        print(f"board_pins.h references {len(used)} macro(s); "
              f"the platform defines {len(defined)}")
        if unknown:
            print("\nReferenced but not defined by any installed board:")
            for macro in unknown:
                print(f"  {macro}")
            print("\nThis is not automatically a bug - an entry may exist for a board\n"
                  "definition that is not installed - but it is where the misspellings\n"
                  "hide. Check each one against the list above.")
        else:
            print("\nEvery macro board_pins.h references is defined by an installed board.")
        return 0

    for board, mcu, macros in rows:
        if args.filters and not any(f.lower() in board.lower() for f in args.filters):
            continue
        print(f"{board:34} {mcu:9} {' '.join(macros) if macros else '(none)'}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
