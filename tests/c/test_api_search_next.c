/**
 * test_api_search_next.c - Tests for snobol_pattern_search_next() API
 *
 * Verify the lightweight unanchored literal search API produces correct
 * results without match-struct, capture, or output overhead.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_api_search_next_suite(void) {
  test_suite("API: snobol_pattern_search_next()");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  /* Single-byte literal: find 'a' at advancing offsets */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'a'", 3, &error);
    test_assert(pat != NULL, "literal 'a' compiled");

    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    test_assert(state != NULL, "state created");

    size_t pos;
    size_t len;
    bool ok = snobol_pattern_search_next(state, "ababa", 5, 0, &pos, &len);
    test_assert(ok, "next finds first 'a' at offset 0");
    test_assert(pos == 0, "first match at position 0");
    test_assert(len == 1, "first match length 1");

    ok = snobol_pattern_search_next(state, "ababa", 5, 1, &pos, &len);
    test_assert(ok, "next finds 'a' after offset 1");
    test_assert(pos == 2, "second match at position 2");
    test_assert(len == 1, "second match length 1");

    ok = snobol_pattern_search_next(state, "ababa", 5, 3, &pos, &len);
    test_assert(ok, "next finds 'a' after offset 3");
    test_assert(pos == 4, "third match at position 4");
    test_assert(len == 1, "third match length 1");

    ok = snobol_pattern_search_next(state, "ababa", 5, 5, &pos, &len);
    test_assert((!ok) != 0, "no match at or past subject end");

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
  }

  /* Multi-byte literal: find 'hello' */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'hello'", 7, &error);
    test_assert(pat != NULL, "literal 'hello' compiled");

    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    test_assert(state != NULL, "state created");

    size_t pos;
    size_t len;
    bool ok =
        snobol_pattern_search_next(state, "hi hello hey", 12, 0, &pos, &len);
    test_assert(ok, "next finds 'hello'");
    test_assert(pos == 3, "match at position 3");
    test_assert(len == 5, "match length 5");

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
  }

  /* Non-literal pattern returns false */
  {
    char *error = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'a' | 'b'", 9, &error);
    test_assert(pat != NULL, "alternation compiled");

    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    test_assert(state != NULL, "state created");

    size_t pos = 99;
    size_t len = 99;
    bool ok = snobol_pattern_search_next(state, "ab", 2, 0, &pos, &len);
    test_assert((!ok) != 0, "non-literal returns false");
    test_assert(pos == 99, "out params unchanged on false");
    test_assert(len == 99, "out params unchanged on false");

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
  }

  /* NULL guards */
  {
    size_t pos;
    size_t len;
    bool ok = snobol_pattern_search_next(nullptr, "x", 1, 0, &pos, &len);
    test_assert((!ok) != 0, "NULL state returns false");

    snobol_context_t *local_ctx = snobol_context_create();
    snobol_pattern_t *pat =
        snobol_pattern_compile(local_ctx, "'x'", 3, nullptr);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, snobol_pattern_get_bc_len(pat));
    ok = snobol_pattern_search_next(state, nullptr, 0, 0, &pos, &len);
    test_assert((!ok) != 0, "NULL subject returns false");

    ok = snobol_pattern_search_next(state, "x", 1, 0, nullptr, &len);
    test_assert((!ok) != 0, "NULL out_pos returns false");

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(local_ctx);
  }

  /* start_offset past end returns false */
  {
    snobol_context_t *local_ctx = snobol_context_create();
    snobol_pattern_t *pat =
        snobol_pattern_compile(local_ctx, "'x'", 3, nullptr);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, snobol_pattern_get_bc_len(pat));

    size_t pos;
    size_t len;
    bool ok = snobol_pattern_search_next(state, "x", 1, 5, &pos, &len);
    test_assert((!ok) != 0, "start_offset past end returns false");

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(local_ctx);
  }

  snobol_context_destroy(ctx);
}
