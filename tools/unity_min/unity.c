/**
 * @file unity.c
 * @brief Minimal Unity-compatible test harness - runtime
 */

#include "unity.h"

#include <stdlib.h>

int unity_tests_run = 0;
int unity_tests_failed = 0;
int unity_current_failed = 0;
const char* unity_current_name = "";

static const char* unity_suite_file = "";

/* Weak defaults so test files that do not define fixtures still link. */
#if defined(__GNUC__)
__attribute__((weak)) void setUp(void) {}
__attribute__((weak)) void tearDown(void) {}
#endif

void unity_begin(const char* file) {
  unity_suite_file = file;
  unity_tests_run = 0;
  unity_tests_failed = 0;
  printf("\n--- %s ---\n", file);
}

void unity_run_test(void (*func)(void), const char* name) {
  unity_current_name = name;
  unity_current_failed = 0;
  unity_tests_run++;

  setUp();
  func();
  tearDown();

  if (unity_current_failed) {
    unity_tests_failed++;
  } else {
    printf("  PASS  %s\n", name);
  }
}

void unity_fail(const char* file, int line, const char* message) {
  unity_current_failed = 1;
  printf("  FAIL  %s\n        %s:%d: %s\n", unity_current_name, file, line, message);
}

int unity_end(void) {
  printf("--- %s: %d test(s), %d failure(s) ---\n",
         unity_suite_file, unity_tests_run, unity_tests_failed);
  return unity_tests_failed;
}
