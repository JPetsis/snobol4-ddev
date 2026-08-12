/**
 * test_pattern_case.c – Tests for case-insensitive pattern matching
 *
 * compile_ex with SNOBOL_FLAG_CASE_INSENSITIVE matches "HELLO" against
 *   pattern "hello"; flags=0 behaves identically to snobol_pattern_compile;
 *   unknown flag bits are tolerated.
 * Latin-1 case-insensitive tests.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* ---------------------------------------------------------------------------
 * Helper: compile + match a pattern against a subject, return match success.
 * Does NOT require an output or captures — just tests match/no-match.
 * ---------------------------------------------------------------------------
 */
static bool compile_and_match(const char *pattern_src, const char *subject,
                              uint32_t flags) {
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return false;
  }

  char *err = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile_ex(
      ctx, pattern_src, strlen(pattern_src), flags, &err);
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return false;
  }

  snobol_match_t *m = snobol_pattern_match(pat, subject, strlen(subject));
  bool ok = snobol_match_success(m);

  snobol_match_free(m);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return ok;
}

void test_pattern_case_suite(void) {
  test_suite("Pattern: case-insensitive (compile_ex)");

  /* --- ASCII case-insensitive matching --- */
  test_assert(
      compile_and_match("'hello'", "hello", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: 'hello' matches \"hello\"");

  test_assert(
      compile_and_match("'hello'", "HELLO", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: 'hello' matches \"HELLO\" (case-insensitive)");

  test_assert(
      compile_and_match("'hello'", "HeLLo", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: 'hello' matches \"HeLLo\" (case-insensitive)");

  /* --- flags=0 identical to snobol_pattern_compile (case-sensitive) --- */
  test_assert((!compile_and_match("'hello'", "HELLO", 0)) != 0,
              "CS: 'hello' does NOT match \"HELLO\" with flags=0");

  test_assert(compile_and_match("'hello'", "hello", 0),
              "CS: 'hello' matches \"hello\" with flags=0");

  /* --- Unknown flag bits are tolerated (shouldn't crash or fail) --- */
  /* Pattern is valid; unknown bits ignored; case-insensitive still active */
  test_assert(compile_and_match("'hello'", "HELLO",
                                0xFFFEU | SNOBOL_FLAG_CASE_INSENSITIVE),
              "CI: unknown flag bits tolerated (flags=0xFFFF)");

  test_suite("Pattern: snobol_pattern_compile (no flags)");

  /* snobol_pattern_compile should behave identically to compile_ex with flags=0
   */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'hello'", 7, &err);
    test_assert(pat != NULL, "snobol_pattern_compile returns non-NULL");
    if (pat) {
      snobol_match_t *m = snobol_pattern_match(pat, "hello", 5);
      test_assert(snobol_match_success(m),
                  "compile: 'hello' matches \"hello\"");
      snobol_match_free(m);

      m = snobol_pattern_match(pat, "HELLO", 5);
      test_assert((!snobol_match_success(m)) != 0,
                  "compile: 'hello' does NOT match \"HELLO\" (case-sensitive)");
      snobol_match_free(m);

      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  test_suite("Pattern: case-insensitive Latin-1");

  /* literal "café" matches "CAFÉ" case-insensitively */
  /* NB: pattern source uses single-quoted literals */
  /* café UTF-8: 63 61 66 C3 A9 (5 bytes) */
  /* CAFÉ UTF-8: 43 41 46 C3 89 (5 bytes) */
  test_assert(compile_and_match("'caf\xC3\xA9'", "CAF\xC3\x89",
                                SNOBOL_FLAG_CASE_INSENSITIVE),
              "CI: 'café' matches \"CAFÉ\" (Latin-1 case-insensitive)");

  /* Reverse: 'CAFÉ' matches "café" case-insensitively */
  test_assert(
      compile_and_match("'CAF\xC3\x89'", "caf\xC3\xA9",
                        SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: 'CAFÉ' matches \"café\" (Latin-1 case-insensitive, reverse)");

  test_suite("Pattern: case-insensitive BMP");

  /* Greek alpha α (U+03B1) matches Α (U+0391) case-insensitively */
  /* α UTF-8: CE B1; Α UTF-8: CE 91 */
  test_assert(
      compile_and_match("'\xCE\xB1'", "\xCE\x91", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: Greek α matches Α (case-insensitive)");

  /* Greek omega ω (U+03C9) matches Ω (U+03A9) */
  /* ω UTF-8: CF 89; Ω UTF-8: CE A9 */
  test_assert(
      compile_and_match("'\xCF\x89'", "\xCE\xA9", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: Greek ω matches Ω (case-insensitive)");

  /* Cyrillic а (U+0430) matches А (U+0410) */
  /* а UTF-8: D0 B0; А UTF-8: D0 90 */
  test_assert(
      compile_and_match("'\xD0\xB0'", "\xD0\x90", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: Cyrillic а matches А (case-insensitive)");

  /* Cyrillic я (U+044F) matches Я (U+042F) */
  /* я UTF-8: D1 8F; Я UTF-8: D0 AF */
  test_assert(
      compile_and_match("'\xD1\x8F'", "\xD0\xAF", SNOBOL_FLAG_CASE_INSENSITIVE),
      "CI: Cyrillic я matches Я (case-insensitive)");

  /* Cyrillic lowercase should NOT match different uppercase */
  /* а (U+0430) should NOT match Б (U+0411) */
  test_assert((!compile_and_match("'\xD0\xB0'", "\xD0\x91",
                                  SNOBOL_FLAG_CASE_INSENSITIVE)) != 0,
              "CI: Cyrillic а does NOT match Б (different letter)");

  /* Case-sensitive: α (U+03B1) should NOT match Α (U+0391) */
  test_assert((!compile_and_match("'\xCE\xB1'", "\xCE\x91", 0)) != 0,
              "CS: Greek α does NOT match Α (case-sensitive)");
}
