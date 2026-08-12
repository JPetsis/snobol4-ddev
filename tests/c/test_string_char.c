/*
 * test_string_char.c – Tests for CHAR and ORD
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/string_fn.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_string_char_suite(void) {
  test_suite("String: CHAR / ORD");

  snobol_buf b = {nullptr};
  snobol_buf_init(&b);

  /* --- CHAR --- */
  /* ASCII: CHAR(65) = "A" */
  bool ok = snobol_char_fn(65, &b);
  test_assert((ok && b.len == 1 && b.data[0] == 'A') != 0,
              "CHAR: codepoint 65 → 'A'");

  /* Two-byte UTF-8: CHAR(0x03B1) = α (U+03B1) */
  ok = snobol_char_fn(0x03B1, &b);
  test_assert((ok && b.len == 2 && (unsigned char)b.data[0] == 0xCE &&
               (unsigned char)b.data[1] == 0xB1) != 0,
              "CHAR: 0x03B1 → 'α' (2-byte UTF-8)");

  /* Four-byte: CHAR(0x1F600) = 😀 */
  ok = snobol_char_fn(0x1F600, &b);
  test_assert((ok && b.len == 4) != 0, "CHAR: 0x1F600 → 4-byte emoji");

  /* Invalid codepoint: CHAR(0x110000) → fail */
  ok = snobol_char_fn(0x110000, &b);
  test_assert((!ok) != 0, "CHAR: invalid codepoint → false");

  /* Surrogate: CHAR(0xD800) → fail */
  ok = snobol_char_fn(0xD800, &b);
  test_assert((!ok) != 0, "CHAR: surrogate codepoint → false");

  /* Null codepoint is valid: CHAR(0) */
  ok = snobol_char_fn(0, &b);
  test_assert((ok && b.len == 1 && b.data[0] == '\0') != 0,
              "CHAR: codepoint 0 → null byte (valid)");

  /* --- ORD --- */
  uint32_t cp = 0;

  /* ASCII: ORD("A") = 65 */
  ok = snobol_ord("A", 1, &cp);
  test_assert((ok && cp == 65) != 0, "ORD: 'A' = 65");

  /* Unicode: ORD("α") = 0x03B1 */
  ok = snobol_ord("\xCE\xB1", 2, &cp);
  test_assert((ok && cp == 0x03B1) != 0, "ORD: 'α' = 0x03B1");

  /* Emoji: ORD("😀") = 0x1F600 */
  ok = snobol_ord("\xF0\x9F\x98\x80", 4, &cp);
  test_assert((ok && cp == 0x1F600) != 0, "ORD: emoji = 0x1F600");

  /* Empty string → fail */
  ok = snobol_ord("", 0, &cp);
  test_assert((!ok) != 0, "ORD: empty string → false");

  /* NULL → fail */
  ok = snobol_ord(nullptr, 0, &cp);
  test_assert((!ok) != 0, "ORD: NULL → false");

  snobol_buf_free(&b);
}
