/**
 * test_reusable_match.c - Tests for snobol_match_reset() and
 *                         snobol_pattern_search_reuse() APIs
 */
#include "snobol/snobol.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_reusable_match_suite(void) {
  test_suite("API: snobol_match_reset() / snobol_pattern_search_reuse()");

  char *error = nullptr;
  snobol_context_t *ctx = snobol_context_create();
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'hello'", 7, &error);
  assert(pat != NULL);
  assert(error == NULL);

  /* 7.5: double-reset safety */
  {
    snobol_match_t *m = snobol_match_create();
    assert(m != NULL);
    snobol_match_reset(m);
    test_assert(!m->success, "reset on fresh: success == false");
    test_assert(m->output == NULL, "reset on fresh: output == NULL");
    snobol_match_reset(m);
    test_assert(!m->success, "double-reset: success == false");
    snobol_match_free(m);
  }

  /* Reset frees allocated strings */
  {
    snobol_match_t *m = snobol_match_create();
    assert(m != NULL);
    m->output = (char *)malloc(6);
    memcpy(m->output, "hello", 6);
    m->output_len = 5;
    m->var_values[0] = (char *)malloc(4);
    memcpy(m->var_values[0], "abc", 4);
    m->var_lens[0] = 3;
    m->var_count = 1;
    snobol_match_reset(m);
    test_assert(m->output == NULL, "reset frees output");
    test_assert(m->var_values[0] == NULL, "reset frees var_values[0]");
    test_assert(m->var_count == 0, "reset clears var_count");
    snobol_match_free(m);
  }

  /* 7.6: reuse produces same results */
  {
    snobol_match_t *m = snobol_match_create();
    assert(m != NULL);
    bool ok = snobol_pattern_search_reuse(pat, "hello world", 11, m);
    test_assert(ok, "reuse finds 'hello'");
    test_assert(m->position == 0, "reuse: position == 0");
    test_assert(m->length == 5, "reuse: length == 5");

    snobol_match_t *m2 = snobol_pattern_search(pat, "hello world", 11);
    test_assert(m2 != NULL, "pattern_search returns non-NULL");
    test_assert(snobol_match_get_position(m2) == 0, "normal: position == 0");
    test_assert(snobol_match_get_length(m2) == 5, "normal: length == 5");

    ok = snobol_pattern_search_reuse(pat, "goodbye", 7, m);
    test_assert((!ok) != 0, "reuse no-match returns false");

    ok = snobol_pattern_search_reuse(pat, "say hello!", 10, m);
    test_assert(ok, "reuse finds 'hello' at offset 4");
    test_assert(m->position == 4, "reuse: position == 4");
    test_assert(m->length == 5, "reuse: length == 5");

    snobol_match_free(m2);
    snobol_match_free(m);
  }

  /* NULL safety */
  {
    snobol_match_reset(nullptr);
    test_assert(true, "reset(NULL) does not crash");
    bool ok = snobol_pattern_search_reuse(nullptr, "x", 1, nullptr);
    test_assert((!ok) != 0, "reuse(NULL,...) returns false");
    snobol_match_t *m = snobol_match_create();
    ok = snobol_pattern_search_reuse(pat, nullptr, 0, m);
    test_assert((!ok) != 0, "reuse(..., NULL, ...) returns false");
    snobol_match_free(m);
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}
