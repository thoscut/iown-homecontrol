#!/usr/bin/env bash
#
# Build and run the io-homecontrol protocol unit tests on the host.
#
# PlatformIO's `pio test -e native` needs the package registry to fetch Unity.
# This script builds the very same test sources against the bundled minimal
# Unity shim in tools/unity_min, so the tests run anywhere a C++17 compiler is
# available - locally, in CI, or offline.
#
# Usage:
#   ./tools/run_native_tests.sh              # build and run every suite
#   ./tools/run_native_tests.sh test_frame   # run a single suite
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/.build/native-tests"

CXX="${CXX:-g++}"
CC="${CC:-gcc}"
CXXFLAGS=(
  -std=c++17
  -O1
  -g
  -Wall
  -Wextra
  -Wshadow
  -Wno-unused-parameter
  -DUNIT_TEST
  -DIOHOME_FORCE_SOFTWARE_AES
  "-I${REPO_ROOT}/src"
  # The C helper layer under include/, which test_legacy_helpers checks.
  "-I${REPO_ROOT}/include"
  "-I${REPO_ROOT}/tools/unity_min"
  # RadioLib mock, so IoHomeControl can be built and tested on the host.
  "-I${REPO_ROOT}/test/mocks"
  # The ESPHome component keeps its own copy of the CRC and the MAC's initial
  # value, because an external_components directory has to be self-contained.
  # test_esphome_crypto builds that copy here and checks it against
  # src/protocol/, so the two cannot drift apart unnoticed.
  "-I${REPO_ROOT}/esphome/components/iown_homecontrol"
)

mkdir -p "${BUILD_DIR}"

# Enable sanitizers unless explicitly disabled - they are the whole point of
# having a host build for a protocol parser.
#
# Some toolchains ship the compiler without the sanitizer runtime, so probe
# before committing to the flags: failing to link is a worse outcome than
# running the tests uninstrumented.
if [[ "${IOHOME_NO_SANITIZERS:-0}" != "1" ]]; then
  SANITIZER_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
  probe_source="${BUILD_DIR}/sanitizer-probe.cpp"
  printf 'int main() { return 0; }\n' > "${probe_source}"

  if "${CXX}" "${SANITIZER_FLAGS[@]}" "${probe_source}" -o "${BUILD_DIR}/sanitizer-probe" \
      > /dev/null 2>&1; then
    CXXFLAGS+=("${SANITIZER_FLAGS[@]}")
  else
    echo "warning: ${CXX} cannot link the sanitizer runtime; running without it" >&2
  fi
fi

PROTOCOL_SOURCES=(
  "${REPO_ROOT}/src/protocol/iohome_crypto.cpp"
  "${REPO_ROOT}/src/protocol/iohome_aes_soft.cpp"
  "${REPO_ROOT}/src/protocol/iohome_frame.cpp"
  "${REPO_ROOT}/src/protocol/iohome_2w.cpp"
  "${REPO_ROOT}/src/protocol/iohome_rolling_code_store.cpp"
  "${REPO_ROOT}/src/protocol/iohome_replay_guard.cpp"
  "${REPO_ROOT}/src/velux/iohome_velux.cpp"
  "${REPO_ROOT}/src/IoHomeControl.cpp"
  # Legacy helpers: iown_crc_calc and the broadcast address.
  "${REPO_ROOT}/src/esp32_utils.cpp"
  "${REPO_ROOT}/src/iown_mac.cpp"
)

# Compile the Unity shim once.
"${CC}" -std=c11 -O1 -g -c "${REPO_ROOT}/tools/unity_min/unity.c" \
  -I"${REPO_ROOT}/tools/unity_min" -o "${BUILD_DIR}/unity.o"

if [[ $# -gt 0 ]]; then
  SUITES=("$@")
else
  SUITES=()
  for dir in "${REPO_ROOT}"/test/test_*; do
    [[ -d "${dir}" ]] && SUITES+=("$(basename "${dir}")")
  done
fi

# ---------------------------------------------------------------------------
# Randomness-across-restarts check
#
# A unit test cannot distinguish a CSPRNG from a well-seeded PRNG - both differ
# from themselves within one run. What matters is that two processes do not
# produce the same sequence, because a predictable 2W challenge lets an attacker
# precompute a valid response. That is only observable across restarts, so it
# lives here rather than in a suite.
# ---------------------------------------------------------------------------
run_random_restart_check() {
  local probe="${BUILD_DIR}/random_probe"

  "${CXX}" "${CXXFLAGS[@]}"     "${REPO_ROOT}/test/support/random_probe.cpp"     "${REPO_ROOT}/src/protocol/iohome_crypto.cpp"     "${REPO_ROOT}/src/protocol/iohome_aes_soft.cpp"     -o "${probe}"

  local first second
  if ! first="$("${probe}")" || ! second="$("${probe}")"; then
    echo "  FAIL  random source unavailable or failing"
    return 1
  fi

  if [[ "${first}" == "${second}" ]]; then
    echo "  FAIL  random_bytes() returned the same sequence in two processes:"
    echo "        ${first}"
    echo "        A constant-seeded generator makes 2W challenges predictable."
    return 1
  fi

  echo "  PASS  random_bytes differs across processes"
  return 0
}

failures=0

if [[ $# -eq 0 ]]; then
  echo
  echo "--- randomness across restarts ---"
  if ! run_random_restart_check; then
    failures=$((failures + 1))
  fi
fi

for suite in "${SUITES[@]}"; do
  suite_dir="${REPO_ROOT}/test/${suite}"
  if [[ ! -d "${suite_dir}" ]]; then
    echo "error: no such test suite: ${suite}" >&2
    exit 2
  fi

  binary="${BUILD_DIR}/${suite}"
  # shellcheck disable=SC2046
  "${CXX}" "${CXXFLAGS[@]}" \
    $(find "${suite_dir}" -name '*.cpp' -print) \
    "${PROTOCOL_SOURCES[@]}" \
    "${BUILD_DIR}/unity.o" \
    -o "${binary}"

  if ! "${binary}"; then
    failures=$((failures + 1))
  fi
done

echo
if [[ ${failures} -ne 0 ]]; then
  echo "RESULT: ${failures} suite(s) failed"
  exit 1
fi
echo "RESULT: all suites passed"
