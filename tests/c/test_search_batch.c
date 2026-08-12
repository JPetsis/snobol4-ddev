#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Helper: run per-call loop and compare position/length against batch result. */
static void assert_batch_matches_percall(snobol_pattern_t *pat,
                                         const char *subject, size_t slen,
                                         const char *label) {
  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);

  /* Batch API */
  snobol_batch_result_t batch;
  memset(&batch, 0, sizeof(batch));
  bool batch_ok =
      snobol_pattern_search_batch(bc, bc_len, subject, slen, meta, &batch);

  /* Per-call loop reference */
  snobol_pattern_search_state_t *state =
      snobol_pattern_search_state_create(bc, bc_len);

  size_t ref_count = 0;
  size_t ref_pos[256];
  size_t ref_len[256];
  size_t offset = 0;

  while (state && offset <= slen && ref_count < 256) {
    snobol_match_t *m = snobol_pattern_search_ex(state, subject, slen, offset);
    if (!m || !snobol_match_success(m)) {
      break;
    }
    ref_pos[ref_count] = snobol_match_get_position(m);
    ref_len[ref_count] = snobol_match_get_length(m);
    ref_count++;
    size_t mlen = ref_len[ref_count - 1];
    offset = ref_pos[ref_count - 1] + (mlen > 0 ? mlen : 1);
  }

  /* Compare counts */
  char msg[120];
  snprintf(msg, sizeof(msg), "%s: count %zu vs %zu", label,
           batch_ok ? batch.match_count : 0, ref_count);
  if (batch_ok) {
    test_assert(batch.match_count == ref_count, msg);
  } else {
    test_assert(ref_count == 0, msg);
  }

  /* Compare positions and lengths */
  size_t n = (int)batch_ok ? batch.match_count : 0;
  if (n > ref_count) {
    n = ref_count;
  }
  for (size_t i = 0; i < n; i++) {
    snprintf(msg, sizeof(msg), "%s: pos[%zu] %zu vs %zu", label, i,
             batch.positions[i], ref_pos[i]);
    test_assert(batch.positions[i] == ref_pos[i], msg);
    snprintf(msg, sizeof(msg), "%s: len[%zu] %zu vs %zu", label, i,
             batch.lengths[i], ref_len[i]);
    test_assert(batch.lengths[i] == ref_len[i], msg);
  }

  snobol_batch_result_free(&batch);
  if (state) {
    snobol_pattern_search_state_destroy(state);
  }
}

void test_search_batch_suite(void) {
  test_suite("Search: batch API parity with per-call loop");

  /* 1. Literal — multiple matches */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'abc'", 5, &err);
    test_assert(pat != NULL, "compile 'abc' succeeds");
    if (pat) {
      assert_batch_matches_percall(pat, "abcabcabc", 9, "lit-triple");
      assert_batch_matches_percall(pat, "abc", 3, "lit-single");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 2. SPAN */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "SPAN('0-9')", 11, &err);
    test_assert(pat != NULL, "compile SPAN('0-9') succeeds");
    if (pat) {
      assert_batch_matches_percall(pat, "abc123def456ghi", 15, "span");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 3. Alternation-of-literals */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "'cat' | 'dog' | 'fox'", 21, &err);
    test_assert(pat != NULL, "compile alt-lit succeeds");
    if (pat) {
      assert_batch_matches_percall(
          pat, "the cat went dog walking fox jumped cat over dog near fox", 51,
          "altlit");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 4. BREAK + literal */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "BREAK(',') ','", 14, &err);
    test_assert(pat != NULL, "compile BREAK pattern succeeds");
    if (pat) {
      assert_batch_matches_percall(pat, "a,b,c,d,e", 9, "break");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 5. Zero-length matches */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "''", 2, &err);
    test_assert(pat != NULL, "compile '' succeeds");
    if (pat) {
      assert_batch_matches_percall(pat, "abc", 3, "zerolen");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 6. No match */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'xyz'", 5, &err);
    test_assert(pat != NULL, "compile 'xyz' succeeds");
    if (pat) {
      assert_batch_matches_percall(pat, "abc", 3, "nomatch");
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 7. Batch returns false for EVAL (ineligible) */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "'x' EVAL('SIZE')", 17, &err);
    test_assert(pat != NULL, "compile EVAL pattern succeeds");
    if (pat) {
      const uint8_t *bc = snobol_pattern_get_bc(pat);
      size_t bc_len = snobol_pattern_get_bc_len(pat);
      const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
      snobol_batch_result_t batch;
      memset(&batch, 0, sizeof(batch));
      bool ok = snobol_pattern_search_batch(bc, bc_len, "x", 1, meta, &batch);
      test_assert((!ok) != 0, "batch returns false for EVAL pattern");
      test_assert(batch.match_count == 0, "batch count 0 for EVAL");
      snobol_batch_result_free(&batch);
    }
    free(err);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
  }

  /* 8. NULL input safety */
  {
    snobol_batch_result_t batch;
    memset(&batch, 0, sizeof(batch));
    bool ok =
        snobol_pattern_search_batch(nullptr, 0, "abc", 3, nullptr, &batch);
    test_assert((!ok) != 0, "batch returns false for NULL inputs");
    test_assert(batch.match_count == 0, "count 0 for NULL");
    snobol_batch_result_free(&batch);
  }

  /* 9. snobol_batch_result_free(NULL) safety */
  {
    snobol_batch_result_free(nullptr);
    test_assert(true, "batch_free(NULL) is safe");
  }
}
