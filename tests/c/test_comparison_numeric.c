#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/type_fn.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_comparison_numeric_suite(void) {
  test_suite("Comparison: EQ / NE / LT / GT / LE / GE (numeric)");

  /* --- EQ --- */
  test_assert(snobol_eq("42", 2, "42", 2), "EQ: equal integers");
  test_assert(snobol_eq("3.14", 4, "3.14", 4), "EQ: equal reals");
  test_assert(snobol_eq("7", 1, "7.0", 3), "EQ: int vs real (same value)");
  test_assert((!snobol_eq("42", 2, "43", 2)) != 0,
              "EQ: different integers fail");
  test_assert((!snobol_eq("3.14", 4, "2.72", 4)) != 0,
              "EQ: different reals fail");

  /* --- NE --- */
  test_assert(snobol_ne("42", 2, "43", 2), "NE: different integers");
  test_assert(snobol_ne("3.14", 4, "2.72", 4), "NE: different reals");
  test_assert((!snobol_ne("42", 2, "42", 2)) != 0, "NE: equal integers fail");
  test_assert((!snobol_ne("7.0", 3, "7", 1)) != 0,
              "NE: same numeric value fail");

  /* --- LT --- */
  test_assert(snobol_lt("3", 1, "7", 1), "LT: 3 < 7");
  test_assert(snobol_lt("-5", 2, "0", 1), "LT: -5 < 0");
  test_assert(snobol_lt("2.5", 3, "3.0", 3), "LT: 2.5 < 3.0");
  test_assert((!snobol_lt("7", 1, "3", 1)) != 0, "LT: 7 < 3 fails");
  test_assert((!snobol_lt("5", 1, "5", 1)) != 0, "LT: 5 < 5 fails (equal)");

  /* --- GT --- */
  test_assert(snobol_gt("7", 1, "3", 1), "GT: 7 > 3");
  test_assert(snobol_gt("0", 1, "-5", 2), "GT: 0 > -5");
  test_assert(snobol_gt("3.0", 3, "2.5", 3), "GT: 3.0 > 2.5");
  test_assert((!snobol_gt("3", 1, "7", 1)) != 0, "GT: 3 > 7 fails");
  test_assert((!snobol_gt("5", 1, "5", 1)) != 0, "GT: 5 > 5 fails (equal)");

  /* --- LE --- */
  test_assert(snobol_le("3", 1, "7", 1), "LE: 3 <= 7");
  test_assert(snobol_le("5", 1, "5", 1), "LE: 5 <= 5 (equal)");
  test_assert(snobol_le("-2", 2, "0", 1), "LE: -2 <= 0");
  test_assert((!snobol_le("7", 1, "3", 1)) != 0, "LE: 7 <= 3 fails");

  /* --- GE --- */
  test_assert(snobol_ge("7", 1, "3", 1), "GE: 7 >= 3");
  test_assert(snobol_ge("5", 1, "5", 1), "GE: 5 >= 5 (equal)");
  test_assert(snobol_ge("0", 1, "-2", 2), "GE: 0 >= -2");
  test_assert((!snobol_ge("3", 1, "7", 1)) != 0, "GE: 3 >= 7 fails");

  /* --- Edge cases --- */
  test_assert(snobol_eq("0", 1, "0.00", 4), "EQ: 0 == 0.00");
  test_assert(snobol_eq("-0", 2, "0", 1), "EQ: -0 == 0");
  test_assert(snobol_ne("abc", 3, "42", 2),
              "NE: non-numeric != number (both convert to 0 vs 42)");

  /* Large numbers (overflow to infinity) */
  {
    const char *big = "1e999";
    const char *big2 = "1e999";
    test_assert(snobol_eq(big, 5, big2, 5), "EQ: both overflow to +inf, equal");
  }
  {
    const char *big = "1e999";
    const char *neg_big = "-1e999";
    test_assert(snobol_lt(neg_big, 6, big, 5), "LT: -inf < +inf");
  }

  /* str_to_double helper */
  test_assert(snobol_str_to_double("42", 2) == 42.0, "str_to_double: 42");
  test_assert(snobol_str_to_double("3.14", 4) == 3.14, "str_to_double: 3.14");
  test_assert(snobol_str_to_double("", 0) == 0.0,
              "str_to_double: empty -> 0.0");
  test_assert(snobol_str_to_double("abc", 3) == 0.0,
              "str_to_double: non-numeric -> 0.0");
  test_assert(isinf(snobol_str_to_double("1e999", 5)),
              "str_to_double: 1e999 -> inf");

  /* Trailing garbage (SNOBOL4 convention: parse leading number) */
  test_assert(snobol_eq("42abc", 5, "42", 2), "EQ: trailing garbage ignored");
  test_assert(snobol_ne("42abc", 5, "43", 2),
              "NE: trailing garbage with different number");
}
