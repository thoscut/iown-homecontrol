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
  "-I${REPO_ROOT}/tools/unity_min"
  # RadioLib mock, so IoHomeControl can be built and tested on the host.
  "-I${REPO_ROOT}/test/mocks"
)

# Enable sanitizers unless explicitly disabled - they are the whole point of
# having a host build for a protocol parser.
if [[ "${IOHOME_NO_SANITIZERS:-0}" != "1" ]]; then
  CXXFLAGS+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
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
)

mkdir -p "${BUILD_DIR}"

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

failures=0
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
