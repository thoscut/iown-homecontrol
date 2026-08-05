#!/usr/bin/env bash
#
# Verify that test/mocks/RadioLib.h agrees with the real RadioLib headers.
#
# The native test build compiles against a hand-written RadioLib mock. That is
# fine right up until the mock drifts: a constant that exists only in the mock
# compiles cleanly on the host and then breaks the firmware build. That is
# exactly what happened with RADIOLIB_ERR_INVALID_RADIO, which the mock had
# invented and two of whose neighbours it had numbered wrong.
#
# This script extracts every RADIOLIB_* constant the mock defines and compares
# each one against the upstream definition, so the drift is caught by the
# native test job instead of by the firmware job several minutes later.
#
# Usage:
#   tools/check_radiolib_mock.sh [--allow-missing]
#
#   --allow-missing   Exit 0 (with a warning) when no RadioLib checkout can be
#                     found. Without it, a missing upstream is an error - a
#                     check that silently passes when it cannot run is worse
#                     than no check at all.
#
# The upstream header is located, in order:
#   1. $RADIOLIB_DIR/src            (explicit override)
#   2. .pio/libdeps/*/RadioLib/src  (after any `pio run`)
#   3. ~/.platformio/lib/RadioLib*/src

set -euo pipefail

cd "$(dirname "$0")/.."

ALLOW_MISSING=0
for arg in "$@"; do
  case "$arg" in
    --allow-missing) ALLOW_MISSING=1 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

MOCK="test/mocks/RadioLib.h"
if [[ ! -f "$MOCK" ]]; then
  echo "FAIL: $MOCK not found" >&2
  exit 1
fi

# --- Locate the real RadioLib -----------------------------------------------

find_radiolib() {
  if [[ -n "${RADIOLIB_DIR:-}" && -d "$RADIOLIB_DIR/src" ]]; then
    echo "$RADIOLIB_DIR/src"
    return 0
  fi
  local candidate
  for candidate in .pio/libdeps/*/RadioLib/src "$HOME"/.platformio/lib/RadioLib*/src; do
    if [[ -d "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  return 1
}

if ! REAL_SRC="$(find_radiolib)"; then
  msg="no RadioLib checkout found (looked in \$RADIOLIB_DIR, .pio/libdeps/*/RadioLib, ~/.platformio/lib)"
  if [[ "$ALLOW_MISSING" -eq 1 ]]; then
    echo "SKIP: $msg"
    exit 0
  fi
  echo "FAIL: $msg" >&2
  echo "      Run 'pio run -e heltec_wifi_lora_32_V2' first, or pass --allow-missing." >&2
  exit 1
fi

echo "mock:     $MOCK"
echo "upstream: $REAL_SRC"
echo

# --- Compare every RADIOLIB_* object-like macro the mock defines -------------
#
# Values are normalised by stripping surrounding parentheses and whitespace so
# that "(-2)" and "-2" compare equal.

normalise() {
  local v="${1//[[:space:]]/}"
  while [[ "$v" == \(*\) ]]; do
    v="${v:1:${#v}-2}"
  done
  printf '%s' "$v"
}

failures=0
checked=0
missing=0

while IFS= read -r name; do
  mock_raw="$(grep -m1 -E "^#define[[:space:]]+${name}[[:space:]]" "$MOCK" \
              | sed -E "s/^#define[[:space:]]+${name}[[:space:]]+//")"

  real_raw="$(grep -rhm1 -E "^#define[[:space:]]+${name}[[:space:]]" "$REAL_SRC" \
              --include='*.h' 2>/dev/null \
              | sed -E "s/^#define[[:space:]]+${name}[[:space:]]+//" || true)"

  if [[ -z "$real_raw" ]]; then
    echo "MISSING  $name = $(normalise "$mock_raw")  (not defined upstream)"
    missing=$((missing + 1))
    failures=$((failures + 1))
    continue
  fi

  mock_val="$(normalise "$mock_raw")"
  real_val="$(normalise "$real_raw")"
  checked=$((checked + 1))

  if [[ "$mock_val" != "$real_val" ]]; then
    echo "MISMATCH $name: mock=$mock_val upstream=$real_val"
    failures=$((failures + 1))
  fi
done < <(grep -oE '^#define[[:space:]]+RADIOLIB_[A-Z0-9_]+[[:space:]]+\(' "$MOCK" \
         | sed -E 's/^#define[[:space:]]+//; s/[[:space:]]+\($//' \
         | sort -u)

if [[ "$checked" -eq 0 && "$missing" -eq 0 ]]; then
  echo "FAIL: no RADIOLIB_* constants found in the mock - is the extraction broken?" >&2
  exit 1
fi

if [[ "$failures" -ne 0 ]]; then
  echo
  echo "FAIL: $failures constant(s) disagree with upstream RadioLib." >&2
  echo "      Copy the upstream values into $MOCK; do not invent new ones." >&2
  exit 1
fi

echo "OK: $checked RADIOLIB_* constant(s) match upstream."
