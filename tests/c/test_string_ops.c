/*
 * test_string_ops.c – Tests for SUBSTR and REPLACE
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/string_fn.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);


/* ===== test_coverage_misc (part): coverage-driven tests merged into test_string_ops.c ===== */
#include <stdint.h>
#include <stdlib.h>
#include "../../core/include/snobol/array.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/string_fn.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_misc_string_fn(void) {
  test_suite("Coverage: string function edge cases");

  snobol_buf out;
  snobol_buf_init(&out);

  /* DUPL: zero count, single, large. */
  test_assert((snobol_dupl("ab", 2, 0, &out) && out.len == 0) != 0, "dupl 0");
  test_assert((snobol_dupl("ab", 2, 3, &out) && out.len == 6) != 0, "dupl 3");
  test_assert((snobol_dupl(nullptr, 2, 3, &out) && out.len == 0) != 0,
              "dupl NULL");

  /* REVERSE: empty + non-empty. */
  test_assert((snobol_reverse("", 0, &out) && out.len == 0) != 0,
              "reverse empty");
  test_assert(
      (snobol_reverse("abc", 3, &out) && memcmp(out.data, "cba", 3) == 0) != 0,
      "reverse abc");

  /* SUBSTR: edge positions. */
  test_assert((snobol_substr("hello", 5, 2, 3, &out) && out.len == 3 &&
               memcmp(out.data, "ell", 3) == 0) != 0,
              "substr middle (1-based pos)");
  test_assert((snobol_substr("hello", 5, 5, 2, &out) && out.len == 1 &&
               out.data[0] == 'o') != 0,
              "substr clamps to remaining codepoints");
  test_assert((!snobol_substr("hello", 5, 0, 0, &out)) != 0,
              "substr pos 0 invalid");

  /* REPLACE. */
  test_assert((snobol_replace("aXbXc", 5, "X", 1, "Y", 1, &out) &&
               out.len == 5 && memcmp(out.data, "aYbYc", 5) == 0) != 0,
              "replace all");
  test_assert((snobol_replace("abc", 3, "z", 1, "q", 1, &out) && out.len == 3 &&
               memcmp(out.data, "abc", 3) == 0) != 0,
              "replace none");
  test_assert(
      (snobol_replace(nullptr, 0, "x", 1, "y", 1, &out) && out.len == 0) != 0,
      "replace NULL");

  /* REPLACE_CHAR. */
  test_assert((snobol_replace_char("abca", 4, "a", 1, "z", 1, &out) &&
               out.len == 4 && memcmp(out.data, "zbcz", 4) == 0) != 0,
              "replace_char all");

  /* LPAD / RPAD with multibyte pad and width. */
  test_assert((snobol_lpad("ab", 2, 5, '0', &out) && out.len == 5 &&
               memcmp(out.data, "000ab", 5) == 0) != 0,
              "lpad");
  test_assert((snobol_rpad("ab", 2, 5, '0', &out) && out.len == 5 &&
               memcmp(out.data, "ab000", 5) == 0) != 0,
              "rpad");
  test_assert((snobol_lpad("abcde", 5, 3, '0', &out) && out.len == 5) != 0,
              "lpad no-pad passthrough");
  test_assert((snobol_rpad("abcde", 5, 3, '0', &out) && out.len == 5) != 0,
              "rpad no-pad passthrough");
  test_assert(snobol_lpad(nullptr, 0, 5, '0', &out), "lpad NULL empty");

  /* CHAR / ORD incl. multibyte. */
  test_assert((snobol_char_fn(0x20AC, &out) && out.len == 3) != 0, "char euro");
  test_assert(snobol_char_fn(0x110000, &out) == false, "char out of range");
  test_assert(snobol_char_fn(0xD800, &out) == false, "char surrogate");
  uint32_t cp = 0;
  test_assert((snobol_ord("\xE2\x82\xAC", 3, &cp) && cp == 0x20AC) != 0,
              "ord euro codepoint");
  test_assert((!snobol_ord("", 0, &cp)) != 0, "ord empty");

  /* UPPER / LOWER with non-ASCII passthrough. */
  test_assert((snobol_upper("aBc", 3, &out) && out.len == 3 &&
               memcmp(out.data, "ABC", 3) == 0) != 0,
              "upper");
  test_assert((snobol_lower("aBc", 3, &out) && out.len == 3 &&
               memcmp(out.data, "abc", 3) == 0) != 0,
              "lower");
  test_assert((snobol_upper("a\xE4\xB8\xADz", 5, &out) && out.len == 5 &&
               memcmp(out.data, "A\xE4\xB8\xADZ", 5) == 0) != 0,
              "upper non-foldable codepoint passthrough");
  test_assert(snobol_size("abc", 3) == 3, "size");
  test_assert(snobol_size(nullptr, 0) == 0, "size NULL");

  snobol_buf_free(&out);
}

/* ── fusion executor ──────────────────────────────────────────────────────── */


void test_cov_misc_string_round2(void) {
  test_suite("Coverage: string function UTF-8 edge cases");

  snobol_buf out;
  snobol_buf_init(&out);

  /* Truncated UTF-8 sequences are treated as single bytes. */
  test_assert((snobol_reverse("\xE2\x82", 2, &out) && out.len == 2) != 0,
              "reverse truncated UTF-8");
  test_assert((snobol_upper("a\xE2", 2, &out) && out.len == 2) != 0,
              "upper truncated UTF-8");
  test_assert((snobol_lower("a\xE2", 2, &out) && out.len == 2) != 0,
              "lower truncated UTF-8");
  test_assert((snobol_substr("a\xC3\xA9"
                             "b",
                             4, 2, 1, &out) &&
               out.len == 2) != 0,
              "substr multibyte codepoint");
  test_assert((snobol_dupl("", 0, 5, &out) && out.len == 0) != 0, "dupl empty");

  /* CHAR boundary values. */
  test_assert((snobol_char_fn(0x7F, &out) && out.len == 1) != 0, "char 0x7F");
  test_assert((snobol_char_fn(0x80, &out) && out.len == 2) != 0, "char 0x80");
  test_assert((snobol_char_fn(0x800, &out) && out.len == 3) != 0, "char 0x800");
  test_assert((snobol_char_fn(0x10000, &out) && out.len == 4) != 0,
              "char 0x10000");

  /* ORD: invalid UTF-8 lead byte. */
  uint32_t cp = 0;
  test_assert((!snobol_ord("\xFF", 1, &cp)) != 0, "ord invalid lead byte");
  test_assert((!snobol_ord("\xE2", 1, &cp)) != 0, "ord truncated sequence");
  test_assert((snobol_ord("\xC3\xA9", 2, &cp) && cp == 0xE9) != 0,
              "ord 2-byte codepoint");

  /* LPAD/RPAD with a multibyte pad codepoint. */
  test_assert((snobol_lpad("x", 1, 3, 0x20AC, &out) && out.len == 7) != 0,
              "lpad multibyte pad");
  test_assert((snobol_rpad("x", 1, 3, 0x20AC, &out) && out.len == 7) != 0,
              "rpad multibyte pad");

  /* SUBSTR walk with 1-based multibyte positions. */
  test_assert(
      (snobol_substr("\xE2\x82\xACxy", 5, 1, 1, &out) && out.len == 3) != 0,
      "substr multibyte codepoint");

  snobol_buf_free(&out);
}


void test_cov_misc_round3_string(void) {
  test_suite("Coverage: string round 3");

  snobol_buf out;
  snobol_buf_init(&out);

  /* String-function NULL/edge guards. */
  test_assert((!snobol_trim(nullptr, 0, &out)) != 0, "trim(NULL)");
  test_assert((!snobol_dupl("ab", 2, 3, nullptr)) != 0, "dupl(NULL out)");
  test_assert((!snobol_reverse("ab", 2, nullptr)) != 0, "reverse(NULL out)");
  test_assert((!snobol_substr("abc", 3, 5, 1, &out)) != 0, "substr beyond end");
  test_assert((snobol_replace("abc", 3, "b", 1, "", 0, &out) && out.len == 2 &&
               memcmp(out.data, "ac", 2) == 0) != 0,
              "replace with empty replacement");
  test_assert((!snobol_char_fn(0x20AC, nullptr)) != 0, "char_fn(NULL out)");
  uint32_t cp = 0;
  test_assert((!snobol_ord(nullptr, 0, &cp)) != 0, "ord(NULL)");
  test_assert((!snobol_upper("ab", 2, nullptr)) != 0, "upper(NULL out)");
  test_assert((!snobol_lower("ab", 2, nullptr)) != 0, "lower(NULL out)");
  test_assert(snobol_size("\xFF"
                          "x",
                          2) == 2,
              "size treats invalid byte as one");
  test_assert((snobol_lpad("ab", 2, 2, '0', &out) && out.len == 2) != 0,
              "lpad width == len");
  test_assert((snobol_rpad("ab", 2, 2, '0', &out) && out.len == 2) != 0,
              "rpad width == len");
  test_assert((snobol_reverse("a\xC3\xA9", 3, &out) && out.len == 3) != 0,
              "reverse multibyte");
  test_assert(
      (snobol_replace_char("abc", 3, "a", 1, "", 0, &out) && out.len == 3) != 0,
      "replace_char empty replacement keeps text");
  snobol_buf_free(&out);
}


void test_cov_misc_round4_string(void) {
  test_suite("Coverage: string final edge-case sweep");

  snobol_buf out;
  snobol_buf_init(&out);

  /* size/reverse/substr/replace/ord UTF-8 and boundary paths. */
  test_assert(snobol_size("\xE2\x82\xAC", 3) == 1, "size counts codepoints");
  test_assert((snobol_reverse("\xE2\x82\xAC"
                              "x",
                              4, &out) &&
               out.len == 4) != 0,
              "reverse multibyte subject");
  test_assert((!snobol_substr("abc", 3, 4, 1, &out)) != 0,
              "substr pos out of range");
  test_assert((snobol_replace("aXbXc", 5, "X", 1, "YY", 2, &out) &&
               out.len == 7 && memcmp(out.data, "aYYbYYc", 7) == 0) != 0,
              "replace growing replacement");
  test_assert(
      (snobol_replace_char("abc", 3, "", 0, "", 0, &out) && out.len == 3) != 0,
      "replace_char empty maps identity");
  test_assert((snobol_replace_char("abc", 3, "ab", 2, "z", 1, &out) &&
               out.len == 3 && memcmp(out.data, "zbc", 3) == 0) != 0,
              "replace_char truncated map");
  test_assert((snobol_lpad("x", 1, 2, 0x80, &out) && out.len == 3) != 0,
              "lpad 2-byte pad codepoint");
  test_assert((snobol_rpad("x", 1, 2, 0x80, &out) && out.len == 3) != 0,
              "rpad 2-byte pad codepoint");
  test_assert((snobol_char_fn(0x7FF, &out) && out.len == 2) != 0, "char 0x7FF");
  uint32_t cp = 0;
  test_assert((snobol_ord("\xE4\xB8\xAD", 3, &cp) && cp == 0x4E2D) != 0,
              "ord 3-byte codepoint");
  test_assert((snobol_ord("\xF0\x9F\x98\x80", 4, &cp) && cp == 0x1F600) != 0,
              "ord 4-byte codepoint");
  snobol_buf_free(&out);
}

void test_string_ops_suite(void) {
  test_suite("String: SUBSTR / REPLACE / REPLACE_CHAR / LPAD / RPAD");

  snobol_buf b = {};
  snobol_buf_init(&b);

  /* --- SUBSTR --- */
  /* "hello world", pos=7, len=5 → "world" */
  (void)snobol_substr("hello world", 11, 7, 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "world", 5) == 0) != 0,
              "SUBSTR: 'hello world'[7,5] = 'world'");

  /* pos=1, len=5 → "hello" */
  (void)snobol_substr("hello world", 11, 1, 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "SUBSTR: 'hello world'[1,5] = 'hello'");

  /* Unicode SUBSTR: "αβγδε" pos=2, len=3 → "βγδ" */
  const char *greek = "\xCE\xB1\xCE\xB2\xCE\xB3\xCE\xB4\xCE\xB5"; /* αβγδε */
  (void)snobol_substr(greek, 10, 2, 3, &b);
  test_assert(
      (b.len == 6 && memcmp(b.data, "\xCE\xB2\xCE\xB3\xCE\xB4", 6) == 0) != 0,
      "SUBSTR: 'αβγδε'[2,3] = 'βγδ' (codepoint positions)");

  /* pos=0 is invalid (1-based) */
  bool ok = snobol_substr("hello", 5, 0, 3, &b);
  test_assert((!ok) != 0, "SUBSTR: pos=0 returns false (1-based indexing)");

  /* --- REPLACE --- */
  (void)snobol_replace("hello hello", 11, "ll", 2, "xx", 2, &b);
  test_assert((b.len == 11 && memcmp(b.data, "hexxo hexxo", 11) == 0) != 0,
              "REPLACE: 'll'→'xx' in 'hello hello'");

  /* No occurrences: output equals input */
  (void)snobol_replace("hello", 5, "xyz", 3, "abc", 3, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "REPLACE: no match → input unchanged");

  /* Empty from: no replacement */
  (void)snobol_replace("hello", 5, "", 0, "x", 1, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "REPLACE: empty from → no replacement");

  /* --- REPLACE_CHAR --- */
  (void)snobol_replace_char("hello", 5, "el", 2, "xy", 2, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hxyyo", 5) == 0) != 0,
              "REPLACE_CHAR: e→x, l→y in 'hello'");

  /* Rot13 */
  (void)snobol_replace_char("hello", 5, "abcdefghijklmnopqrstuvwxyz", 26,
                            "nopqrstuvwxyzabcdefghijklm", 26, &b);
  test_assert((b.len == 5 && memcmp(b.data, "uryyb", 5) == 0) != 0,
              "REPLACE_CHAR: rot13 of 'hello' = 'uryyb'");

  /* --- LPAD --- */
  (void)snobol_lpad("5", 1, 3, '0', &b);
  test_assert((b.len == 3 && memcmp(b.data, "005", 3) == 0) != 0,
              "LPAD: '5' padded to width 3 with '0' = '005'");

  /* Already wide enough */
  (void)snobol_lpad("hello", 5, 3, ' ', &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "LPAD: already at width → unchanged");

  /* --- RPAD --- */
  (void)snobol_rpad("hi", 2, 5, ' ', &b);
  test_assert((b.len == 5 && memcmp(b.data, "hi   ", 5) == 0) != 0,
              "RPAD: 'hi' padded to width 5 with space");

  snobol_buf_free(&b);
  test_cov_misc_string_fn();
  test_cov_misc_string_round2();
  test_cov_misc_round3_string();
  test_cov_misc_round4_string();
}
