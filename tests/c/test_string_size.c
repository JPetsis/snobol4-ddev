/*
 * test_string_size.c – Tests for snobol_size()
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "snobol/string_fn.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_string_size_suite(void) {
  test_suite("String: SIZE");

  /* ASCII fast path */
  test_assert(snobol_size("hello", 5) == 5, "SIZE: 'hello' = 5 codepoints");
  test_assert(snobol_size("", 0) == 0, "SIZE: empty string = 0");
  test_assert(snobol_size("a", 1) == 1, "SIZE: single char = 1");

  /* Two-byte UTF-8: é (U+00E9) = 2 bytes, 1 codepoint */
  /* café = c a f \xC3\xA9 (é encoded as 2 bytes) */
  const char cafe[] = {'c', 'a', 'f', '\xC3', '\xA9', '\0'};
  test_assert(snobol_size(cafe, 5) == 4,
              "SIZE: 'cafe-with-accent' = 4 codepoints (5 bytes)");

  /* Three-byte UTF-8: Chinese character 中 (U+4E2D) = 3 bytes */
  const char han[] = {'\xE4', '\xB8', '\xAD', '\0'};
  test_assert(snobol_size(han, 3) == 1,
              "SIZE: Chinese char = 1 codepoint (3 bytes)");

  /* Mixed ASCII + Unicode */
  const char mixed[] = {'a', '\xC3', '\xA9', 'b', '\0'};
  test_assert(snobol_size(mixed, 4) == 3,
              "SIZE: mixed ASCII+Unicode = 3 codepoints");

  /* Four-byte emoji: 😀 (U+1F600) = 4 bytes */
  const char emoji[] = {'\xF0', '\x9F', '\x98', '\x80', '\0'};
  test_assert(snobol_size(emoji, 4) == 1, "SIZE: 4-byte emoji = 1 codepoint");

  /* NULL safety */
  test_assert(snobol_size(nullptr, 0) == 0, "SIZE: NULL string = 0");
}
