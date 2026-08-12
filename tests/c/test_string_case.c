/*
 * test_string_case.c – Tests for UPPER and LOWER
 *
 * Covers:
 *   - ASCII (fast path)
 *   - Latin-1 accented characters (v2 Unicode folding)
 *   - German sharp-s expansion: UPPER("Straße") == "STRASSE"
 *   - Latin Extended-A: LOWER("ŠKODA") == "škoda"
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/string_fn.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_string_case_suite(void) {
  test_suite("String: UPPER / LOWER");

  snobol_buf b = {0};
  snobol_buf_init(&b);

  /* --- UPPER (ASCII fast path) --- */
  (void)snobol_upper("hello", 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "HELLO", 5) == 0) != 0,
              "UPPER: 'hello' → 'HELLO'");

  (void)snobol_upper("ALREADY", 7, &b);
  test_assert((b.len == 7 && memcmp(b.data, "ALREADY", 7) == 0) != 0,
              "UPPER: already uppercase unchanged");

  (void)snobol_upper("Hello World!", 12, &b);
  test_assert((b.len == 12 && memcmp(b.data, "HELLO WORLD!", 12) == 0) != 0,
              "UPPER: mixed case → all uppercase");

  (void)snobol_upper("", 0, &b);
  test_assert(b.len == 0, "UPPER: empty string");

  /* --- UPPER (Unicode v2) --- */

  /* UPPER("café") == "CAFÉ"
   * café = c a f U+00E9(é)  = 63 61 66 C3 A9  (5 bytes)
   * CAFÉ = C A F U+00C9(É)  = 43 41 46 C3 89  (5 bytes) */
  const char *cafe_lower = "caf\xC3\xA9"; /* café — 5 bytes */
  const char *cafe_upper = "CAF\xC3\x89"; /* CAFÉ — 5 bytes */
  (void)snobol_upper(cafe_lower, 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, cafe_upper, 5) == 0) != 0,
              "UPPER: \"café\" → \"CAFÉ\"");

  /* UPPER("Straße") == "STRASSE"
   * Straße: S t r a ß(U+00DF) e = 53 74 72 61 C3 9F 65 (7 bytes)
   * STRASSE: 7 ASCII bytes       = 53 54 52 41 53 53 45 (7 bytes) */
  const char strasse_lower_buf[] = {'S',        't',        'r', 'a',
                                    (char)0xC3, (char)0x9F, 'e', 0};
  const char *strasse_lower = strasse_lower_buf; /* Straße — 7 bytes */
  const char *strasse_upper = "STRASSE";         /* STRASSE — 7 bytes */
  (void)snobol_upper(strasse_lower, 7, &b);
  test_assert((b.len == 7 && memcmp(b.data, strasse_upper, 7) == 0) != 0,
              "UPPER: \"Straße\" → \"STRASSE\"");

  /* --- LOWER (ASCII fast path) --- */
  (void)snobol_lower("HELLO", 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, "hello", 5) == 0) != 0,
              "LOWER: 'HELLO' → 'hello'");

  (void)snobol_lower("already", 7, &b);
  test_assert((b.len == 7 && memcmp(b.data, "already", 7) == 0) != 0,
              "LOWER: already lowercase unchanged");

  (void)snobol_lower("Hello World!", 12, &b);
  test_assert((b.len == 12 && memcmp(b.data, "hello world!", 12) == 0) != 0,
              "LOWER: mixed case → all lowercase");

  (void)snobol_lower("", 0, &b);
  test_assert(b.len == 0, "LOWER: empty string");

  /* --- LOWER (Unicode v2) --- */

  /* LOWER("CAFÉ") == "café" */
  (void)snobol_lower(cafe_upper, 5, &b);
  test_assert((b.len == 5 && memcmp(b.data, cafe_lower, 5) == 0) != 0,
              "LOWER: \"CAFÉ\" → \"café\"");

  /* LOWER("ŠKODA") == "škoda"
   * ŠKODA: Š(U+0160) K O D A  = C5 A0 4B 4F 44 41  (6 bytes)
   * škoda: š(U+0161) k o d a  = C5 A1 6B 6F 64 61  (6 bytes) */
  const char *skoda_upper = "\xC5\xA0KODA"; /* ŠKODA — 6 bytes */
  const char *skoda_lower = "\xC5\xA1koda"; /* škoda — 6 bytes */
  (void)snobol_lower(skoda_upper, 6, &b);
  test_assert((b.len == 6 && memcmp(b.data, skoda_lower, 6) == 0) != 0,
              "LOWER: \"ŠKODA\" → \"škoda\"");

  /* Non-cased codepoints preserve identity */
  const char *greek = "ΑΒΓ"; /* U+0391 U+0392 U+0393, all out of coverage */
  size_t greek_len = strlen(greek);
  (void)snobol_upper(greek, greek_len, &b);
  test_assert((b.len == greek_len && memcmp(b.data, greek, greek_len) == 0) !=
                  0,
              "UPPER: out-of-coverage codepoints preserved unchanged");

  snobol_buf_free(&b);
}
