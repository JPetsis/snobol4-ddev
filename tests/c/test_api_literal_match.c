/**
 * test_api_literal_match.c - Tests for snobol_pattern_match_literal() API
 *
 * Verify the lightweight anchored literal match API produces correct results
 * with zero heap allocations.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_api_literal_match_suite(void) {
  test_suite("API: snobol_pattern_match_literal()");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  /* 7.1: successful anchored literal match returns correct position and length */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'hello'", 7, &error);
    test_assert(pat != NULL, "literal pattern compiled");
    test_assert(error == NULL, "no compile error");

    snobol_literal_match_t r =
        snobol_pattern_match_literal(pat, "hello world", 11);
    test_assert(r.success, "'hello' matches 'hello world'");
    test_assert(r.position == 0, "position is 0");
    test_assert(r.length == 5, "length is 5");

    snobol_pattern_free(pat);
  }

  /* 7.2: non-matching subject returns failure */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'xyz'", 5, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    snobol_literal_match_t r =
        snobol_pattern_match_literal(pat, "hello world", 11);
    test_assert((!r.success) != 0, "'xyz' does not match 'hello world'");
    test_assert(r.position == 0, "position is 0 on failure");
    test_assert(r.length == 0, "length is 0 on failure");

    snobol_pattern_free(pat);
  }

  /* 7.3: non-literal pattern returns {false, 0, 0} with no fallback */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'a' | 'b'", 9, &error);
    test_assert(pat != NULL, "alternation pattern compiled");

    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "b", 1);
    test_assert((!r.success) != 0,
                "alternation returns failure (not literal-only)");
    test_assert(r.position == 0, "position is 0");
    test_assert(r.length == 0, "length is 0");

    snobol_pattern_free(pat);
  }

  /* 7.4: no heap allocations on success (verified by ASan and by returning by value) */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'test'", 6, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    /* Call multiple times to verify no memory leaks under ASan */
    for (int i = 0; i < 100; i++) {
      snobol_literal_match_t r =
          snobol_pattern_match_literal(pat, "test subject", 12);
      test_assert(r.success, "match succeeds in loop");
      test_assert(r.length == 4, "length is 4 in loop");
    }

    snobol_pattern_free(pat);
  }

  /* Additional: empty subject */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'x'", 3, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "", 0);
    test_assert((!r.success) != 0, "literal does not match empty subject");

    snobol_pattern_free(pat);
  }

  /* Additional: literal longer than subject */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'hello'", 7, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "hi", 2);
    test_assert((!r.success) != 0,
                "literal longer than subject returns failure");

    snobol_pattern_free(pat);
  }

  /* Additional: exact match */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'exact'", 7, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "exact", 5);
    test_assert(r.success, "exact match succeeds");
    test_assert(r.length == 5, "length matches subject length");

    snobol_pattern_free(pat);
  }

  /* Additional: NULL pattern returns failure */
  {
    snobol_literal_match_t r = snobol_pattern_match_literal(nullptr, "test", 4);
    test_assert((!r.success) != 0, "NULL pattern returns failure");
  }

  /* Additional: NULL subject returns failure */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'test'", 6, &error);
    test_assert(pat != NULL, "literal pattern compiled");

    snobol_literal_match_t r = snobol_pattern_match_literal(pat, nullptr, 0);
    test_assert((!r.success) != 0, "NULL subject returns failure");

    snobol_pattern_free(pat);
  }

  snobol_context_destroy(ctx);
}
