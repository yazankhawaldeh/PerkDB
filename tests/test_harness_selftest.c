// Self-test for tests/test_harness.h: verifies the assert macros return 1 on
// pass and 0 on failure, count correctly, handle NULL strings, and print the
// file, line, and expected vs actual. Intentional failures are captured into a
// temp file (stdout is redirected) and the counters are reset afterwards, so
// this suite exits 0 when the macros behave correctly.

#include <stdlib.h>
#include <unistd.h>

#include "test_harness.h"

static int saved_stdout = -1;
static FILE* capture = NULL;

/** Starts redirecting stdout into an anonymous temp file. */
static void capture_begin(void) {
  fflush(stdout);
  capture = tmpfile();
  saved_stdout = dup(fileno(stdout));
  dup2(fileno(capture), fileno(stdout));
}

/**
 * Stops redirecting stdout and returns what was captured.
 * @param buf output buffer
 * @param cap size of buf
 */
static void capture_end(char* buf, size_t cap) {
  fflush(stdout);
  dup2(saved_stdout, fileno(stdout));
  close(saved_stdout);
  rewind(capture);
  size_t n = fread(buf, 1, cap - 1, capture);
  buf[n] = '\0';
  fclose(capture);
}

int main(void) {
  int ok = 1;
  char out[2048];
  int r_true, r_int, r_str, r_null1, r_null2, r_null3;
  int line_true, line_int, line_str;

  // Passing asserts return 1 and print nothing.
  capture_begin();
  int p1 = ASSERT_TRUE(1 == 1);
  int p2 = ASSERT_EQ_INT(7, 7);
  int p3 = ASSERT_EQ_STR("abc", "abc");
  int p4 = ASSERT_EQ_STR(NULL, NULL);
  capture_end(out, sizeof out);
  ok &= p1 == 1 && p2 == 1 && p3 == 1 && p4 == 1;
  ok &= out[0] == '\0';
  ok &= harness_checks == 4 && harness_failures == 0;

  // Failing asserts return 0, are counted, and report details.
  capture_begin();
  line_true = __LINE__ + 1;
  r_true = ASSERT_TRUE(1 == 2);
  line_int = __LINE__ + 1;
  r_int = ASSERT_EQ_INT(3, 4);
  line_str = __LINE__ + 1;
  r_str = ASSERT_EQ_STR("abc", "abd");
  r_null1 = ASSERT_EQ_STR(NULL, "x");
  r_null2 = ASSERT_EQ_STR("x", NULL);
  r_null3 = ASSERT_EQ_INT(-1, 1);
  capture_end(out, sizeof out);
  ok &= r_true == 0 && r_int == 0 && r_str == 0 && r_null1 == 0 &&
        r_null2 == 0 && r_null3 == 0;
  ok &= harness_checks == 10 && harness_failures == 6;
  ok &= (harness_failures != 0);

  char want[128];
  ok &= strstr(out, __FILE__) != NULL;
  snprintf(want, sizeof want, ":%d:", line_true);
  ok &= strstr(out, want) != NULL;
  snprintf(want, sizeof want, ":%d:", line_int);
  ok &= strstr(out, want) != NULL;
  snprintf(want, sizeof want, ":%d:", line_str);
  ok &= strstr(out, want) != NULL;
  ok &= strstr(out, "expected: 4") != NULL && strstr(out, "actual:   3") != NULL;
  ok &= strstr(out, "expected: \"abd\"") != NULL &&
        strstr(out, "actual:   \"abc\"") != NULL;
  ok &= strstr(out, "NULL") != NULL;

  // Reset counters so the summary reflects only the self-test result.
  harness_checks = 0;
  harness_failures = 0;
  fflush(stdout);
  ASSERT_TRUE(ok);
  return harness_summary();
}
