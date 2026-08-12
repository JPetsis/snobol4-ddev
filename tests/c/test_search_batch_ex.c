/**
 * test_search_batch_ex.c - Tests for the stateful batch API
 * (snobol_pattern_search_batch_ex) and the tri-state `eligible` flag.
 *
 * Covers: batch_ex results identical to the stateless batch; eligible-vs-
 * ineligible distinction; DFA/range_meta caches reused across repeated
 * batch_ex calls on one state; interleaved search_ex/batch_ex on one state.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Run both stateless batch and stateful batch_ex on the same pattern/subject
 * and assert identical match counts, positions, lengths, and eligible flag. */
static void assert_batch_ex_matches_batch(const char *src, const char *subject,
                                          size_t slen, const char *label) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  (void)label;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, src, strlen(src), &err);
  test_assert(pat != NULL, "compile succeeds");
  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }
  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);

  snobol_batch_result_t b1;
  memset(&b1, 0, sizeof(b1));
  bool ok1 = snobol_pattern_search_batch(bc, bc_len, subject, slen, meta, &b1);

  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(bc, bc_len);
  snobol_batch_result_t b2;
  memset(&b2, 0, sizeof(b2));
  bool ok2 = snobol_pattern_search_batch_ex(st, subject, slen, &b2);

  test_assert(ok1 == ok2, "batch and batch_ex agree on success");
  test_assert(b1.eligible == b2.eligible, "eligible flag agrees");
  test_assert(b1.match_count == b2.match_count, "match counts agree");
  size_t n = b1.match_count;
  for (size_t i = 0; i < n; i++) {
    test_assert(b1.positions[i] == b2.positions[i], "positions agree");
    test_assert(b1.lengths[i] == b2.lengths[i], "lengths agree");
  }

  snobol_batch_result_free(&b1);
  snobol_batch_result_free(&b2);
  snobol_pattern_search_state_destroy(st);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* Eligible pattern, zero matches: batch and batch_ex return false but keep
 * eligible == true (so callers must NOT re-run the search). */
static void test_batch_ex_eligible_zero_match(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, "SPAN('abc') 'd'", 14, &err);
  test_assert(pat != NULL, "compile eligible pattern");
  if (pat) {
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);

    snobol_batch_result_t b;
    memset(&b, 0, sizeof(b));
    bool ok = snobol_pattern_search_batch(bc, bc_len, "aaaa", 4, meta, &b);
    test_assert((!ok) != 0, "zero-match eligible returns false");
    test_assert(b.eligible, "eligible true on zero-match (stateless)");
    test_assert(b.match_count == 0, "match_count 0");
    snobol_batch_result_free(&b);

    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);
    snobol_batch_result_t be;
    memset(&be, 0, sizeof(be));
    bool oke = snobol_pattern_search_batch_ex(st, "aaaa", 4, &be);
    test_assert((!oke) != 0, "batch_ex zero-match returns false");
    test_assert(be.eligible, "eligible true on zero-match (batch_ex)");
    snobol_batch_result_free(&be);
    snobol_pattern_search_state_destroy(st);
  }
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* Ineligible bytecode (OP_EVAL): both entries return false with eligible ==
 * false (caller should fall back to the per-call loop).  We use a minimal
 * raw bytecode containing the excluded opcode rather than relying on the
 * parser to produce one, because the parser may not generate OP_EVAL for
 * any given source string (it may leave patterns eligible by default). */
static void test_batch_ex_ineligible_flag(void) {
  uint8_t bc[] = {OP_EVAL, 0, 0, 0, OP_ACCEPT};
  size_t bc_len = sizeof(bc);
  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  snobol_search_derive_meta(bc, bc_len, &meta);

  snobol_batch_result_t b;
  memset(&b, 0, sizeof(b));
  bool ok = snobol_pattern_search_batch(bc, bc_len, "x", 1, &meta, &b);
  test_assert((!ok) != 0, "ineligible returns false");
  test_assert((!b.eligible) != 0, "ineligible flag false (stateless)");
  snobol_batch_result_free(&b);

  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(bc, bc_len);
  snobol_batch_result_t be;
  memset(&be, 0, sizeof(be));
  bool oke = snobol_pattern_search_batch_ex(st, "x", 1, &be);
  test_assert((!oke) != 0, "batch_ex ineligible returns false");
  test_assert((!be.eligible) != 0, "ineligible flag false (batch_ex)");
  snobol_batch_result_free(&be);
  snobol_pattern_search_state_destroy(st);
  snobol_search_meta_free(&meta);
}

/* Automaton-eligible pattern: many batch_ex calls on one state must all yield
 * identical correct results — a proxy that the DFA/range_meta caches are built
 * once and reused without corruption. ASan covers the no-leak part. */
static void test_batch_ex_dfa_reused(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, "SPAN('abc') 'd'", 14, &err);
  test_assert(pat != NULL, "compile automaton pattern");
  if (pat) {
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);
    const char *subj = "abcabcabcdabcabcabc";
    size_t slen = strlen(subj);
    for (int i = 0; i < 50; i++) {
      snobol_batch_result_t b;
      memset(&b, 0, sizeof(b));
      bool ok = snobol_pattern_search_batch_ex(st, subj, slen, &b);
      test_assert(ok, "batch_ex succeeds on repeated call");
      test_assert(b.match_count == 1, "batch_ex finds the single 'abcd'");
      snobol_batch_result_free(&b);
    }
    snobol_pattern_search_state_destroy(st);
  }
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* Interleaving search_ex (first match) and batch_ex (all matches) on one state
 * must stay correct and leak-free. */
static void test_batch_ex_interleaved_with_search_ex(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'abc'", 5, &err);
  test_assert(pat != NULL, "compile 'abc'");
  if (pat) {
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);
    const char *subj = "abcabcabc";
    size_t slen = strlen(subj);

    for (int i = 0; i < 20; i++) {
      snobol_match_t *m = snobol_pattern_search_ex(st, subj, slen, 0);
      test_assert((m && snobol_match_success(m)) != 0, "search_ex first match");
      if (m) {
        test_assert(snobol_match_get_position(m) == 0, "search_ex pos 0");
        /* NOT freed — search_ex returns an internal pointer owned by the
         * state (the caller must NOT free it). */
      }
      snobol_batch_result_t b;
      memset(&b, 0, sizeof(b));
      bool ok = snobol_pattern_search_batch_ex(st, subj, slen, &b);
      test_assert((ok && b.match_count == 3) != 0, "batch_ex finds 3 abc");
      snobol_batch_result_free(&b);
    }
    snobol_pattern_search_state_destroy(st);
  }
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

void test_search_batch_ex_suite(void) {
  test_suite("Search: stateful batch_ex + tri-state eligible");
  assert_batch_ex_matches_batch("'abc'", "abcabcabc", 9, "lit");
  assert_batch_ex_matches_batch("'cat' | 'dog' | 'fox'", "cat dog fox", 11,
                                "altlit");
  assert_batch_ex_matches_batch("SPAN('0-9')", "a1b22c333", 8, "span");
  test_batch_ex_eligible_zero_match();
  test_batch_ex_ineligible_flag();
  test_batch_ex_dfa_reused();
  test_batch_ex_interleaved_with_search_ex();
}
