/**
 * @file unity.h
 * @brief Minimal Unity-compatible test harness for the native g++ build
 *
 * PlatformIO's `native` environment pulls in the real ThrowTheSwitch Unity
 * framework. That requires the PlatformIO package registry, which is not
 * always reachable (air-gapped CI, quick local checks). This header implements
 * the small subset of the Unity API the protocol tests use, so the same test
 * sources can also be compiled and run with nothing but a C++ compiler:
 *
 *     ./tools/run_native_tests.sh
 *
 * It is deliberately *not* on the default include path - only the standalone
 * runner adds `-Itools/unity_min`.
 */

#ifndef IOHOME_UNITY_MIN_H
#define IOHOME_UNITY_MIN_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Test state
 * -------------------------------------------------------------------------- */

extern int unity_tests_run;
extern int unity_tests_failed;
extern int unity_current_failed;
extern const char* unity_current_name;

void unity_begin(const char* file);
int unity_end(void);
void unity_run_test(void (*func)(void), const char* name);
void unity_fail(const char* file, int line, const char* message);

/* Optional fixtures - tests may define their own; these are weak defaults. */
void setUp(void);
void tearDown(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* --------------------------------------------------------------------------
 * Runner macros
 * -------------------------------------------------------------------------- */

#define UNITY_BEGIN() (unity_begin(__FILE__), 0)
#define UNITY_END() unity_end()
#define RUN_TEST(func) unity_run_test(func, #func)

/* --------------------------------------------------------------------------
 * Assertions
 *
 * Every assertion records a failure and returns from the current test function,
 * mirroring Unity's abort-on-first-failure behaviour.
 * -------------------------------------------------------------------------- */

#define UNITY_FAIL_AND_RETURN(msg)          \
  do {                                      \
    unity_fail(__FILE__, __LINE__, (msg));  \
    return;                                 \
  } while (0)

#define TEST_FAIL_MESSAGE(msg) UNITY_FAIL_AND_RETURN(msg)

#define TEST_ASSERT(cond) \
  do { if (!(cond)) UNITY_FAIL_AND_RETURN("Expected TRUE: " #cond); } while (0)

#define TEST_ASSERT_TRUE(cond) \
  do { if (!(cond)) UNITY_FAIL_AND_RETURN("Expected TRUE: " #cond); } while (0)

#define TEST_ASSERT_FALSE(cond) \
  do { if ((cond)) UNITY_FAIL_AND_RETURN("Expected FALSE: " #cond); } while (0)

#define TEST_ASSERT_NULL(ptr) \
  do { if ((ptr) != NULL) UNITY_FAIL_AND_RETURN("Expected NULL: " #ptr); } while (0)

#define TEST_ASSERT_NOT_NULL(ptr) \
  do { if ((ptr) == NULL) UNITY_FAIL_AND_RETURN("Expected non-NULL: " #ptr); } while (0)

#define UNITY_COMPARE_NUM(expected, actual, fmt, label)                              \
  do {                                                                               \
    long long unity_e_ = (long long)(expected);                                       \
    long long unity_a_ = (long long)(actual);                                         \
    if (unity_e_ != unity_a_) {                                                       \
      char unity_msg_[192];                                                           \
      snprintf(unity_msg_, sizeof(unity_msg_),                                        \
               label ": expected " fmt " but was " fmt, unity_e_, unity_a_);          \
      UNITY_FAIL_AND_RETURN(unity_msg_);                                              \
    }                                                                                 \
  } while (0)

#define TEST_ASSERT_EQUAL(expected, actual) \
  UNITY_COMPARE_NUM(expected, actual, "%lld", "Values not equal")
#define TEST_ASSERT_EQUAL_INT(expected, actual) \
  UNITY_COMPARE_NUM(expected, actual, "%lld", "Values not equal")
#define TEST_ASSERT_EQUAL_UINT(expected, actual) \
  UNITY_COMPARE_NUM(expected, actual, "%lld", "Values not equal")
#define TEST_ASSERT_EQUAL_UINT8(expected, actual) \
  UNITY_COMPARE_NUM((uint8_t)(expected), (uint8_t)(actual), "%lld", "uint8 not equal")
#define TEST_ASSERT_EQUAL_UINT16(expected, actual) \
  UNITY_COMPARE_NUM((uint16_t)(expected), (uint16_t)(actual), "%lld", "uint16 not equal")
#define TEST_ASSERT_EQUAL_UINT32(expected, actual) \
  UNITY_COMPARE_NUM((uint32_t)(expected), (uint32_t)(actual), "%lld", "uint32 not equal")
#define TEST_ASSERT_EQUAL_SIZE(expected, actual) \
  UNITY_COMPARE_NUM(expected, actual, "%lld", "size not equal")
#define TEST_ASSERT_EQUAL_HEX8(expected, actual) \
  UNITY_COMPARE_NUM((uint8_t)(expected), (uint8_t)(actual), "0x%llX", "hex8 not equal")
#define TEST_ASSERT_EQUAL_HEX16(expected, actual) \
  UNITY_COMPARE_NUM((uint16_t)(expected), (uint16_t)(actual), "0x%llX", "hex16 not equal")
#define TEST_ASSERT_EQUAL_HEX32(expected, actual) \
  UNITY_COMPARE_NUM((uint32_t)(expected), (uint32_t)(actual), "0x%llX", "hex32 not equal")

#define TEST_ASSERT_NOT_EQUAL(expected, actual)                                       \
  do {                                                                                \
    if ((long long)(expected) == (long long)(actual))                                  \
      UNITY_FAIL_AND_RETURN("Values should differ: " #expected " vs " #actual);        \
  } while (0)

#define TEST_ASSERT_GREATER_THAN(threshold, actual)                                   \
  do {                                                                                \
    if (!((long long)(actual) > (long long)(threshold)))                               \
      UNITY_FAIL_AND_RETURN(#actual " should be greater than " #threshold);            \
  } while (0)

#define TEST_ASSERT_LESS_THAN(threshold, actual)                                      \
  do {                                                                                \
    if (!((long long)(actual) < (long long)(threshold)))                               \
      UNITY_FAIL_AND_RETURN(#actual " should be less than " #threshold);               \
  } while (0)

#define TEST_ASSERT_GREATER_OR_EQUAL(threshold, actual)                               \
  do {                                                                                \
    if (!((long long)(actual) >= (long long)(threshold)))                              \
      UNITY_FAIL_AND_RETURN(#actual " should be >= " #threshold);                      \
  } while (0)

#define TEST_ASSERT_LESS_OR_EQUAL(threshold, actual)                                  \
  do {                                                                                \
    if (!((long long)(actual) <= (long long)(threshold)))                              \
      UNITY_FAIL_AND_RETURN(#actual " should be <= " #threshold);                      \
  } while (0)

#define TEST_ASSERT_EQUAL_MEMORY(expected, actual, len)                               \
  do {                                                                                \
    if (memcmp((expected), (actual), (size_t)(len)) != 0)                              \
      UNITY_FAIL_AND_RETURN("Memory contents differ: " #expected " vs " #actual);       \
  } while (0)

#define TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, actual, len)                          \
  do {                                                                                \
    const uint8_t* unity_e_ = (const uint8_t*)(expected);                              \
    const uint8_t* unity_a_ = (const uint8_t*)(actual);                                \
    for (size_t unity_i_ = 0; unity_i_ < (size_t)(len); unity_i_++) {                  \
      if (unity_e_[unity_i_] != unity_a_[unity_i_]) {                                  \
        char unity_msg_[192];                                                          \
        snprintf(unity_msg_, sizeof(unity_msg_),                                       \
                 "Array differs at index %zu: expected 0x%02X but was 0x%02X",         \
                 unity_i_, unity_e_[unity_i_], unity_a_[unity_i_]);                    \
        UNITY_FAIL_AND_RETURN(unity_msg_);                                             \
      }                                                                                \
    }                                                                                  \
  } while (0)

#define TEST_ASSERT_FLOAT_WITHIN(delta, expected, actual)                             \
  do {                                                                                \
    double unity_d_ = (double)(expected) - (double)(actual);                            \
    if (unity_d_ < 0) unity_d_ = -unity_d_;                                             \
    if (unity_d_ > (double)(delta)) {                                                   \
      char unity_msg_[192];                                                             \
      snprintf(unity_msg_, sizeof(unity_msg_),                                          \
               "Floats not within %g: expected %g but was %g",                          \
               (double)(delta), (double)(expected), (double)(actual));                  \
      UNITY_FAIL_AND_RETURN(unity_msg_);                                                \
    }                                                                                   \
  } while (0)

#define TEST_ASSERT_EQUAL_FLOAT(expected, actual) \
  TEST_ASSERT_FLOAT_WITHIN(0.00001f, expected, actual)

#endif /* IOHOME_UNITY_MIN_H */
