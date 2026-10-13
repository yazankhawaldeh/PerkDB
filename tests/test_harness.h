// Minimal shared test harness for the PerkDB test suites.
//
// Every assertion counts as one check. A failed assertion prints its file,
// line, the source expression, and the expected vs actual values. Assertions
// are expressions that evaluate to 1 on pass and 0 on failure, so callers can
// print extra context: if (!ASSERT_TRUE(x)) printf("  context\n");
//
// Define HARNESS_MAX_REPORTED before including this header to cap how many
// failures are printed (0, the default, prints all of them). Every failure is
// still counted. Finish main() with: return harness_summary();

#ifndef PERKDB_TEST_HARNESS_H
#define PERKDB_TEST_HARNESS_H

#include <stdio.h>
#include <string.h>

#ifndef HARNESS_MAX_REPORTED
#define HARNESS_MAX_REPORTED 0
#endif

static int harness_checks = 0;
static int harness_failures = 0;

/**
 * Records one check and decides whether a failure should be printed.
 * @param passed nonzero if the check passed
 * @return 1 if the caller should print failure details, 0 otherwise
 */
static inline int harness_record(int passed) {
  harness_checks++;
  if (passed) return 0;
  harness_failures++;
  return HARNESS_MAX_REPORTED <= 0 || harness_failures <= HARNESS_MAX_REPORTED;
}

/**
 * Checks that a condition holds.
 * @param file source file of the assertion
 * @param line source line of the assertion
 * @param text the condition as written in the source
 * @param passed nonzero if the condition held
 * @return 1 on pass, 0 on failure
 */
static inline int harness_true(const char* file, int line, const char* text,
                               int passed) {
  if (harness_record(passed))
    printf("FAIL %s:%d: %s\n  expected: true\n  actual:   false\n", file, line,
           text);
  return passed;
}

/**
 * Checks that two integers are equal.
 * @param file source file of the assertion
 * @param line source line of the assertion
 * @param actual_text the actual expression as written in the source
 * @param expected_text the expected expression as written in the source
 * @param actual the value the code under test produced
 * @param expected the value the test expects
 * @return 1 on pass, 0 on failure
 */
static inline int harness_eq_int(const char* file, int line,
                                 const char* actual_text,
                                 const char* expected_text, long long actual,
                                 long long expected) {
  int passed = actual == expected;
  if (harness_record(passed))
    printf("FAIL %s:%d: %s == %s\n  expected: %lld\n  actual:   %lld\n", file,
           line, actual_text, expected_text, expected, actual);
  return passed;
}

/**
 * Checks that two C strings are equal; NULL only equals NULL.
 * @param file source file of the assertion
 * @param line source line of the assertion
 * @param actual_text the actual expression as written in the source
 * @param expected_text the expected expression as written in the source
 * @param actual the string the code under test produced (may be NULL)
 * @param expected the string the test expects (may be NULL)
 * @return 1 on pass, 0 on failure
 */
static inline int harness_eq_str(const char* file, int line,
                                 const char* actual_text,
                                 const char* expected_text, const char* actual,
                                 const char* expected) {
  int passed = (actual == NULL || expected == NULL)
                   ? actual == expected
                   : strcmp(actual, expected) == 0;
  if (harness_record(passed))
    printf("FAIL %s:%d: %s == %s\n  expected: %s%s%s\n  actual:   %s%s%s\n",
           file, line, actual_text, expected_text, expected ? "\"" : "",
           expected ? expected : "NULL", expected ? "\"" : "",
           actual ? "\"" : "", actual ? actual : "NULL", actual ? "\"" : "");
  return passed;
}

/**
 * Prints the pass count for the suite.
 * @return process exit status: 0 if every check passed, 1 otherwise
 */
static inline int harness_summary(void) {
  printf("%d/%d checks passed\n", harness_checks - harness_failures,
         harness_checks);
  return harness_failures != 0;
}

#define ASSERT_TRUE(cond) harness_true(__FILE__, __LINE__, #cond, (cond) != 0)

#define ASSERT_EQ_INT(actual, expected)                                       \
  harness_eq_int(__FILE__, __LINE__, #actual, #expected, (long long)(actual), \
                 (long long)(expected))

#define ASSERT_EQ_STR(actual, expected) \
  harness_eq_str(__FILE__, __LINE__, #actual, #expected, (actual), (expected))

#endif  // PERKDB_TEST_HARNESS_H
