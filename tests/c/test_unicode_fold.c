/**
 * test_unicode_fold.c – Tests for the snobol_to_upper_cp / snobol_to_lower_cp
 * Unicode fold table functions.
 *
 * Verifies:
 *   - ASCII fast path
 *   - Latin-1 upper/lower round-trip
 *   - Latin Extended-A samples
 *   - German sharp-s multi-char expansion (U+00DF → "SS")
 *   - Greek multi-char expansion (U+0390 → {U+03B9, U+0308})
 *   - Cyrillic upper/lower round-trip
 *   - Non-case scripts identity (Arabic, Hebrew, CJK)
 *   - Non-case characters identity (multiplication, division)
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snobol/unicode_fold.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_unicode_fold_suite(void) {
  test_suite("Unicode fold table");

  uint32_t out[2];
  int out_len;

  /* --- ASCII fast path --- */
  snobol_to_upper_cp(0x61, out, &out_len); /* 'a' → 'A' */
  test_assert((out_len == 1 && out[0] == 0x41) != 0, "to_upper: 'a' → 'A'");

  snobol_to_upper_cp(0x7A, out, &out_len); /* 'z' → 'Z' */
  test_assert((out_len == 1 && out[0] == 0x5A) != 0, "to_upper: 'z' → 'Z'");

  test_assert(snobol_to_lower_cp(0x41) == 0x61, "to_lower: 'A' → 'a'");
  test_assert(snobol_to_lower_cp(0x5A) == 0x7A, "to_lower: 'Z' → 'z'");

  /* ASCII already-upper / already-lower: identity */
  snobol_to_upper_cp(0x41, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x41) != 0,
              "to_upper: 'A' → 'A' (identity)");
  test_assert(snobol_to_lower_cp(0x61) == 0x61,
              "to_lower: 'a' → 'a' (identity)");

  /* --- Latin-1 upper/lower round-trip --- */
  /* U+00E9 é → U+00C9 É */
  snobol_to_upper_cp(0x00E9, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x00C9) != 0,
              "to_upper: U+00E9 é → U+00C9 É");

  /* U+00C9 É → U+00E9 é */
  test_assert(snobol_to_lower_cp(0x00C9) == 0x00E9,
              "to_lower: U+00C9 É → U+00E9 é");

  /* U+00FC ü → U+00DC Ü */
  snobol_to_upper_cp(0x00FC, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x00DC) != 0,
              "to_upper: U+00FC ü → U+00DC Ü");

  /* U+00DC Ü → U+00FC ü */
  test_assert(snobol_to_lower_cp(0x00DC) == 0x00FC,
              "to_lower: U+00DC Ü → U+00FC ü");

  /* U+00FF ÿ → U+0178 Ÿ */
  snobol_to_upper_cp(0x00FF, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0178) != 0,
              "to_upper: U+00FF ÿ → U+0178 Ÿ");

  /* U+0178 Ÿ → U+00FF ÿ */
  test_assert(snobol_to_lower_cp(0x0178) == 0x00FF,
              "to_lower: U+0178 Ÿ → U+00FF ÿ");

  /* --- Sharp-s multi-char expansion --- */
  snobol_to_upper_cp(0x00DF, out, &out_len);
  test_assert((out_len == 2 && out[0] == 0x0053 && out[1] == 0x0053) != 0,
              "to_upper: U+00DF ß → {S, S} (two codepoints)");

  /* Sharp-s to_lower: identity (ß is already lowercase) */
  test_assert(snobol_to_lower_cp(0x00DF) == 0x00DF,
              "to_lower: U+00DF ß → ß (identity)");

  /* --- Latin Extended-A --- */
  /* U+0161 š → U+0160 Š */
  snobol_to_upper_cp(0x0161, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0160) != 0,
              "to_upper: U+0161 š → U+0160 Š");

  /* U+0160 Š → U+0161 š */
  test_assert(snobol_to_lower_cp(0x0160) == 0x0161,
              "to_lower: U+0160 Š → U+0161 š");

  /* U+017E ž → U+017D Ž */
  snobol_to_upper_cp(0x017E, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x017D) != 0,
              "to_upper: U+017E ž → U+017D Ž");

  /* U+0101 ā → U+0100 Ā */
  snobol_to_upper_cp(0x0101, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0100) != 0,
              "to_upper: U+0101 ā → U+0100 Ā");
  /* --- Cyrillic --- */
  /* U+0400 (IE with grave) uppercase is identity, lowercase → U+0450 */
  snobol_to_upper_cp(0x0400, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0400) != 0,
              "to_upper: U+0400 (Cyrillic IE with grave) → identity");
  test_assert(snobol_to_lower_cp(0x0400) == 0x0450,
              "to_lower: U+0400 → U+0450");

  /* U+0430 а (Cyrillic small a) → U+0410 А */
  snobol_to_upper_cp(0x0430, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0410) != 0,
              "to_upper: U+0430 а → U+0410 А");
  test_assert(snobol_to_lower_cp(0x0410) == 0x0430,
              "to_lower: U+0410 А → U+0430 а");

  /* U+044F я (Cyrillic small ya) → U+042F Я */
  snobol_to_upper_cp(0x044F, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x042F) != 0,
              "to_upper: U+044F я → U+042F Я");
  test_assert(snobol_to_lower_cp(0x042F) == 0x044F,
              "to_lower: U+042F Я → U+044F я");

  /* --- Greek --- */
  /* U+03B1 α (Greek small alpha) → U+0391 Α */
  snobol_to_upper_cp(0x03B1, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x0391) != 0,
              "to_upper: U+03B1 α → U+0391 Α");
  test_assert(snobol_to_lower_cp(0x0391) == 0x03B1,
              "to_lower: U+0391 Α → U+03B1 α");

  /* U+03C9 ω (Greek small omega) → U+03A9 Ω */
  snobol_to_upper_cp(0x03C9, out, &out_len);
  test_assert((out_len == 1 && out[0] == 0x03A9) != 0,
              "to_upper: U+03C9 ω → U+03A9 Ω");
  test_assert(snobol_to_lower_cp(0x03A9) == 0x03C9,
              "to_lower: U+03A9 Ω → U+03C9 ω");

  /* U+0390 (iota with dialytika+tonos) → { U+03B9, U+0308 } (multi-char) */
  snobol_to_upper_cp(0x0390, out, &out_len);
  test_assert((out_len == 2 && out[0] == 0x03B9 && out[1] == 0x0308) != 0,
              "to_upper: U+0390 → {U+03B9, U+0308} (multi-char)");
  test_assert(snobol_to_lower_cp(0x0390) == 0x0390,
              "to_lower: U+0390 → identity");

  /* --- Non-case scripts: identity --- */
  snobol_to_upper_cp(0x0627, out, &out_len); /* Arabic alef */
  test_assert((out_len == 1 && out[0] == 0x0627) != 0,
              "to_upper: U+0627 (Arabic) → identity");
  test_assert(snobol_to_lower_cp(0x0627) == 0x0627,
              "to_lower: U+0627 (Arabic) → identity");

  snobol_to_upper_cp(0x05D0, out, &out_len); /* Hebrew alef */
  test_assert((out_len == 1 && out[0] == 0x05D0) != 0,
              "to_upper: U+05D0 (Hebrew) → identity");
  test_assert(snobol_to_lower_cp(0x05D0) == 0x05D0,
              "to_lower: U+05D0 (Hebrew) → identity");

  snobol_to_upper_cp(0x4E2D, out, &out_len); /* CJK 中 */
  test_assert((out_len == 1 && out[0] == 0x4E2D) != 0,
              "to_upper: U+4E2D (CJK) → identity");
  test_assert(snobol_to_lower_cp(0x4E2D) == 0x4E2D,
              "to_lower: U+4E2D (CJK) → identity");

  /* --- Non-case characters: identity --- */
  snobol_to_upper_cp(0x00D7, out, &out_len); /* × multiplication sign */
  test_assert((out_len == 1 && out[0] == 0x00D7) != 0,
              "to_upper: U+00D7 × → identity");

  snobol_to_upper_cp(0x00F7, out, &out_len); /* ÷ division sign */
  test_assert((out_len == 1 && out[0] == 0x00F7) != 0,
              "to_upper: U+00F7 ÷ → identity");
}
