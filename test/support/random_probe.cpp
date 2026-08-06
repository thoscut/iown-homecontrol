/**
 * @file random_probe.cpp
 * @brief Prints one draw from crypto::random_bytes(), for the restart check.
 *
 * A unit test cannot tell a CSPRNG from a well-seeded PRNG: both produce
 * values that differ from each other within a single run. The property that
 * actually matters - that two devices, or one device across a reboot, do not
 * emit the same challenge sequence - is only observable across processes.
 *
 * tools/run_native_tests.sh runs this twice and compares the output. A
 * constant-seeded generator prints the same line both times and fails the
 * check; that is exactly the regression this guards against, because a
 * predictable 2W challenge lets an attacker precompute a valid response.
 */

#include "protocol/iohome_crypto.h"

#include <cstdio>

int main() {
  if (!iohome::crypto::has_secure_random()) {
    std::fprintf(stderr, "no secure random source on this platform\n");
    return 2;
  }

  uint8_t buffer[32];
  if (!iohome::crypto::random_bytes(buffer, sizeof(buffer))) {
    std::fprintf(stderr, "random_bytes() failed\n");
    return 2;
  }

  for (const uint8_t byte : buffer) {
    std::printf("%02X", byte);
  }
  std::printf("\n");
  return 0;
}
