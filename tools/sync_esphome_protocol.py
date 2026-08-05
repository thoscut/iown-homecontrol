#!/usr/bin/env python3
"""
Copy the protocol layer into the ESPHome component, or check that the copy is
current.

ESPHome's `external_components` mechanism takes a component *directory* and
compiles the files it finds *directly* inside it - subdirectories are skipped
unless the manifest opts in, which only core components can do. So a component
cannot reach `src/protocol/` at the repository root, and cannot tuck the copy
away in a subfolder either. That is why this one grew its own implementation of
the CRC and the MAC's initial value.

Two implementations of one wire format drift, and this project has already paid
for that: the 2W key transfer used an initial value neither peer computed, and
the round-trip test agreed with itself for months because both directions were
wrong the same way.

So the component gets a copy - but a copy this script makes, byte for byte, and
that CI re-checks on every push. `--check` is what runs there.

Usage:
    tools/sync_esphome_protocol.py           # refresh the copy
    tools/sync_esphome_protocol.py --check   # fail if it is out of date

`iohome_nvs_store.*` is deliberately left out: it includes Arduino's
<Preferences.h>, and the component persists its rolling code through ESPHome's
own preference API instead.
"""

import argparse
import filecmp
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIR = REPO_ROOT / "src" / "protocol"
TARGET_DIR = REPO_ROOT / "esphome" / "components" / "iown_homecontrol"

# Everything the component needs, and nothing that drags in Arduino.
FILES = [
    "iohome_constants.h",
    "iohome_crypto.h",
    "iohome_crypto.cpp",
    "iohome_aes_soft.h",
    "iohome_aes_soft.cpp",
    "iohome_frame.h",
    "iohome_frame.cpp",
    "iohome_2w.h",
    "iohome_2w.cpp",
    "iohome_replay_guard.h",
    "iohome_replay_guard.cpp",
    "iohome_rolling_code_store.h",
    "iohome_rolling_code_store.cpp",
]

README = """# The `iohome_*.{h,cpp}` files here are copies

They are copied verbatim from `src/protocol/` by
`tools/sync_esphome_protocol.py`. **Do not edit them here.** Change the
originals and re-run the script; CI fails the build if the two ever differ.

ESPHome's `external_components` takes a component directory and compiles the
files directly inside it. Subdirectories are skipped, and the component cannot
reach the repository root, so the copies have to sit alongside the component's
own sources rather than in a tidy subfolder.

The alternative was a second implementation of the same wire format, which this
project has already been bitten by: the 2W key transfer used an initial value
neither peer computed, and the round-trip test agreed with itself for months
because both directions were wrong in the same way. There is now one
implementation and a copy of it, not two implementations.

`iohome_nvs_store.*` is not copied - it includes Arduino's `<Preferences.h>`,
and the component uses ESPHome's preference API instead.

**After adding or removing a file here, run `esphome clean` before compiling.**
PlatformIO does not notice new sources in an existing build tree, and the link
fails with undefined references to functions whose `.cpp` is sitting right
there uncompiled.
"""


def check() -> int:
    missing = [name for name in FILES if not (TARGET_DIR / name).is_file()]
    stale = [
        name
        for name in FILES
        if (TARGET_DIR / name).is_file()
        and not filecmp.cmp(SOURCE_DIR / name, TARGET_DIR / name, shallow=False)
    ]

    # Only files this script owns; the component's own sources live here too.
    extra = sorted(
        path.name
        for path in TARGET_DIR.glob("iohome_*")
        if path.is_file() and path.name not in FILES
    )

    if not missing and not stale and not extra:
        print(f"OK: {len(FILES)} file(s) in {TARGET_DIR.relative_to(REPO_ROOT)} "
              "match src/protocol/")
        return 0

    for name in missing:
        print(f"MISSING  {name}")
    for name in stale:
        print(f"STALE    {name}")
    for name in extra:
        print(f"UNEXPECTED {name} - not copied from src/protocol/")

    print()
    print("The ESPHome component's copy of the protocol layer is out of date.")
    print("Run tools/sync_esphome_protocol.py and commit the result.")
    return 1


def sync() -> int:
    TARGET_DIR.mkdir(parents=True, exist_ok=True)

    copied = 0
    for name in FILES:
        source = SOURCE_DIR / name
        if not source.is_file():
            print(f"error: {source} does not exist", file=sys.stderr)
            return 1
        target = TARGET_DIR / name
        if target.is_file() and filecmp.cmp(source, target, shallow=False):
            continue
        target.write_bytes(source.read_bytes())
        print(f"updated {target.relative_to(REPO_ROOT)}")
        copied += 1

    # A file dropped from FILES would otherwise stay behind and keep compiling.
    for path in sorted(TARGET_DIR.glob("iohome_*")):
        if path.is_file() and path.name not in FILES:
            path.unlink()
            print(f"removed {path.relative_to(REPO_ROOT)}")

    (TARGET_DIR / "PROTOCOL-COPY.md").write_text(README)

    print(f"{copied} file(s) updated; {len(FILES)} in sync")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true",
                        help="report whether the copy is current; do not write")
    args = parser.parse_args()
    return check() if args.check else sync()


if __name__ == "__main__":
    sys.exit(main())
