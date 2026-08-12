/*
 * test_string_transform.c – Tests for TRIM, DUPL, REVERSE
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/string_fn.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_string_transform_suite(void) {
  test_suite("String: TRIM / DUPL / REVERSE");

  snobol_buf b = {};
  snobol_buf_init(&b);

  /* --- TRIM --- */
  (void)snobol_trim("hello  ", 7, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "TRIM: trailing spaces removed");

  (void)snobol_trim("hello\t\n", 7, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "TRIM: trailing tab+newline removed");

  (void)snobol_trim("hello", 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "TRIM: no trailing space unchanged");

  (void)snobol_trim("  ", 2, &b);
  test_assert(b.len == 0, "TRIM: all-whitespace becomes empty");

  (void)snobol_trim("", 0, &b);
  test_assert(b.len == 0, "TRIM: empty string stays empty");

  /* --- DUPL --- */
  (void)snobol_dupl("ab", 2, 3, &b);
  test_assert((b.len == 6 && memcmp(b.data, "ababab", 6) == 0) != 0,
              "DUPL: 'ab' x3 = 'ababab'");

  (void)snobol_dupl("x", 1, 0, &b);
  test_assert(b.len == 0, "DUPL: n=0 produces empty string");

  (void)snobol_dupl("", 0, 5, &b);
  test_assert(b.len == 0, "DUPL: empty string x5 = empty");

  /* DUPL with Unicode */
  const char *alpha_beta = "\xCE\xB1\xCE\xB2"; /* αβ */
  (void)snobol_dupl(alpha_beta, 4, 2, &b);
  test_assert((b.len == 8 &&
               memcmp(b.data, "\xCE\xB1\xCE\xB2\xCE\xB1\xCE\xB2", 8) == 0) != 0,
              "DUPL: 'αβ' x2 = correct UTF-8");

  /* --- REVERSE --- */
  (void)snobol_reverse("hello", 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "olleh", 5) == 0) != 0,
              "REVERSE: 'hello' → 'olleh'");

  (void)snobol_reverse("a", 1, &b);
  test_assert((b.len == 1 && b.data[0] == 'a') != 0,
              "REVERSE: single char unchanged");

  (void)snobol_reverse("", 0, &b);
  test_assert(b.len == 0, "REVERSE: empty string stays empty");

  /* REVERSE with Unicode: αβγ → γβα */
  const char *abc_greek = "\xCE\xB1\xCE\xB2\xCE\xB3"; /* αβγ */
  (void)snobol_reverse(abc_greek, 6, &b);
  test_assert(
      (b.len == 6 && memcmp(b.data, "\xCE\xB3\xCE\xB2\xCE\xB1", 6) == 0) != 0,
      "REVERSE: 'αβγ' → 'γβα' (codepoints, not bytes)");

  snobol_buf_free(&b);
}
