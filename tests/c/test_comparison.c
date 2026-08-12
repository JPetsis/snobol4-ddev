/*
 * test_comparison.c – Tests for IDENT, DIFFER, LEX*, INTEGER, REAL, NUMERIC
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/type_fn.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_comparison_suite(void) {
  test_suite("Comparison: IDENT / DIFFER / LEXEQ / LEXLT / LEXGT / INTEGER / "
             "REAL / NUMERIC");

  /* --- IDENT --- */
  test_assert(snobol_ident("hello", 5, "hello", 5),
              "IDENT: identical strings succeed");
  test_assert((!snobol_ident("hello", 5, "world", 5)) != 0,
              "IDENT: different strings fail");
  test_assert(snobol_ident("", 0, "", 0), "IDENT: empty strings are identical");
  test_assert((!snobol_ident("abc", 3, "abcd", 4)) != 0,
              "IDENT: different lengths fail");

  /* --- DIFFER --- */
  test_assert((!snobol_differ("hello", 5, "hello", 5)) != 0,
              "DIFFER: identical strings fail");
  test_assert(snobol_differ("hello", 5, "world", 5),
              "DIFFER: different strings succeed");

  /* --- LEXEQ --- */
  test_assert(snobol_lexeq("hello", 5, "hello", 5), "LEXEQ: equal");
  test_assert((!snobol_lexeq("hello", 5, "world", 5)) != 0, "LEXEQ: not equal");

  /* --- LEXLT --- */
  test_assert(snobol_lexlt("apple", 5, "banana", 6),
              "LEXLT: 'apple' < 'banana'");
  test_assert((!snobol_lexlt("banana", 6, "apple", 5)) != 0,
              "LEXLT: 'banana' not < 'apple'");
  test_assert((!snobol_lexlt("equal", 5, "equal", 5)) != 0,
              "LEXLT: equal strings not <");

  /* --- LEXGT --- */
  test_assert(snobol_lexgt("banana", 6, "apple", 5),
              "LEXGT: 'banana' > 'apple'");
  test_assert((!snobol_lexgt("apple", 5, "banana", 6)) != 0,
              "LEXGT: 'apple' not > 'banana'");

  /* --- INTEGER --- */
  test_assert(snobol_integer("123", 3), "INTEGER: '123'");
  test_assert(snobol_integer("-456", 4), "INTEGER: '-456'");
  test_assert(snobol_integer("+789", 4), "INTEGER: '+789'");
  test_assert(snobol_integer("0", 1), "INTEGER: '0'");
  test_assert((!snobol_integer("12.34", 5)) != 0,
              "INTEGER: '12.34' not integer");
  test_assert((!snobol_integer("12x", 3)) != 0, "INTEGER: '12x' not integer");
  test_assert((!snobol_integer("", 0)) != 0, "INTEGER: empty not integer");
  test_assert((!snobol_integer("-", 1)) != 0,
              "INTEGER: bare minus not integer");

  /* --- REAL --- */
  test_assert(snobol_real("12.34", 5), "REAL: '12.34'");
  test_assert(snobol_real("1.23e-4", 7), "REAL: '1.23e-4'");
  test_assert(snobol_real("1.23E+10", 8), "REAL: '1.23E+10'");
  test_assert(snobol_real("123", 3), "REAL: integer '123' is also real");
  test_assert((!snobol_real("hello", 5)) != 0, "REAL: 'hello' not real");
  test_assert((!snobol_real("", 0)) != 0, "REAL: empty not real");
  test_assert((!snobol_real("1e", 2)) != 0,
              "REAL: '1e' (no exponent digits) not real");

  /* --- NUMERIC --- */
  test_assert(snobol_numeric("123", 3), "NUMERIC: integer is numeric");
  test_assert(snobol_numeric("12.34", 5), "NUMERIC: real is numeric");
  test_assert((!snobol_numeric("hello", 5)) != 0,
              "NUMERIC: 'hello' not numeric");
  test_assert((!snobol_numeric("", 0)) != 0, "NUMERIC: empty not numeric");
}
