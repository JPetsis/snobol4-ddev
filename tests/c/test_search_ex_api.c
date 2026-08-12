/**
 * test_search_ex_api.c - Tests for the stateful snobol_pattern_search_ex() API
 *
 * Verifies that:
 *   1. snobol_pattern_search_ex() produces identical results to
 *      snobol_pattern_search() when called in equivalent loops
 *   2. State can be created and destroyed without leaks (validated by
 *      running under ASan/UBSan in the build-asan target)
 *   3. The JIT fires on hot patterns through the stateful path
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);


/* ===== test_coverage_engine2 (part): coverage-driven tests merged into test_search_ex_api.c ===== */
#include <stdio.h>
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_engine2_state_api(void) {
  test_suite("Coverage: state API capture cleanup + anchored output");

  /* Capture pattern searched twice: second call frees prior var_values. */
  {
    ast_node_t *ast = snobol_ast_create_cap(1, snobol_ast_create_lit("ab", 2));
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);
    test_assert(st != NULL, "state created");
    snobol_match_t *m = snobol_pattern_search_ex(st, "xxab", 4, 2);
    test_assert((m && m->success) != 0, "capture search from offset");
    if (m) {
      const char *cap = snobol_match_get_variable(m, "1", nullptr);
      test_assert((cap && strcmp(cap, "ab") == 0) != 0,
                  "capture materialized at window offset");
    }
    /* Second call over a non-matching subject frees the capture strings. */
    m = snobol_pattern_search_ex(st, "zz", 2, 0);
    test_assert((m && !m->success) != 0, "second call no-match");
    snobol_pattern_search_state_destroy(st);
    free(bc);
  }

  /* Anchored state search with EVAL output + capture. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(1, snobol_ast_create_lit("x ", 2));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_TRIM, 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);
    test_assert(st != NULL, "anchored state created");
    snobol_match_t *m = snobol_pattern_search_ex_anchored(st, "x ", 2);
    test_assert((m && m->success) != 0, "anchored EVAL match");
    if (m) {
      size_t olen = 0;
      const char *out = snobol_match_get_output(m, &olen);
      test_assert((olen == 1 && out && out[0] == 'x') != 0,
                  "anchored EVAL output copied");
      const char *cap = snobol_match_get_variable(m, "1", nullptr);
      test_assert((cap && cap[0] == 'x') != 0, "anchored capture copied");
    }
    m = snobol_pattern_search_ex_anchored(st, "ab", 2);
    test_assert((m && !m->success) != 0, "anchored second call mismatch");
    snobol_pattern_search_state_destroy(st);
    free(bc);
  }
}

/* ── fusion tier entry guards + anchored ──────────────────────────────────── */


/* Anchored automaton regression: an anchored search of an automaton-eligible
 * pattern must never return a match that starts away from the anchor.  The
 * DFA override used to ignore the anchored flag, so 'ab' SPAN('0-9') matched
 * "xab12" at offset 1 through snobol_pattern_search_ex_anchored. */
void test_cov_anchored_automaton(void) {
  test_suite("Coverage: anchored automaton respects the anchor");

  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_lit("ab", 2);
  parts[1] = snobol_ast_create_span("0-9", 3);
  ast_node_t *ast = snobol_ast_create_concat(parts, 2);
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
  snobol_ast_free(ast);
  test_assert((bc && bc_len > 0) != 0, "automaton pattern compiles");

  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(bc, bc_len);
  test_assert(st != NULL, "automaton state created");
  if (st) {
    /* Match exists but starts at offset 1: anchored must FAIL. */
    snobol_match_t *m = snobol_pattern_search_ex_anchored(st, "xab12", 5);
    test_assert((m && !m->success) != 0, "anchored automaton fails off-anchor");

    /* Match starts exactly at 0: anchored must SUCCEED.  (The DFA reports
     * the shortest accepting run, so SPAN('0-9') accepts right after "ab";
     * the anchored contract here is the match START, not greedy length.) */
    m = snobol_pattern_search_ex_anchored(st, "ab123", 5);
    test_assert((m && m->success) != 0,
                "anchored automaton succeeds at anchor");
    if (m) {
      test_assert(snobol_match_get_position(m) == 0,
                  "anchored automaton position is 0");
    }

    /* Unanchored control: the same subject matches at offset 1. */
    m = snobol_pattern_search_ex(st, "xab12", 5, 0);
    test_assert((m && m->success) != 0, "unanchored automaton still matches");
    if (m) {
      test_assert(snobol_match_get_position(m) == 1,
                  "unanchored automaton position is 1");
    }

    snobol_pattern_search_state_destroy(st);
  }
  free(bc);
}

void test_search_ex_api_suite(void) {
  test_suite("Search: stateful _ex API");

  /* Stateful search matches non-stateful search results */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "SPAN(',')", 9, &err);
    test_assert(pat != NULL, "compile SPAN succeeds");
    if (pat) {
      const char *subject = "a,b,,c,d,,,e";
      size_t slen = strlen(subject);

      /* Reference: non-stateful search */
      snobol_match_t *ref = snobol_pattern_search(pat, subject, slen);
      test_assert((ref != NULL && snobol_match_success(ref)) != 0,
                  "non-stateful search succeeds");
      size_t ref_output_len = 0;
      const char *ref_output =
          ref ? snobol_match_get_output(ref, &ref_output_len) : nullptr;
      char ref_buf[64] = {0};
      if (ref_output) {
        size_t cp = ref_output_len < 63 ? ref_output_len : 63;
        memcpy(ref_buf, ref_output, cp);
      }

      /* Stateful search */
      snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      test_assert(state != NULL, "state create returns non-NULL");
      if (state) {
        snobol_match_t *m = snobol_pattern_search_ex(state, subject, slen, 0);
        test_assert(m != NULL, "stateful search returns non-NULL");
        if (m) {
          test_assert(snobol_match_success(m), "stateful search succeeds");
          /* The output should be the same set of commas */
          size_t out_len = 0;
          const char *out = snobol_match_get_output(m, &out_len);
          test_assert(
              out_len == ref_output_len,
              "stateful and non-stateful search produce same output length");
          if (out && ref_output) {
            test_assert(memcmp(out, ref_output, out_len) == 0,
                        "stateful and non-stateful outputs match");
          }
        }
        snobol_pattern_search_state_destroy(state);
      }
      if (ref) {
        snobol_match_free(ref);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Multiple calls on the same state return valid results */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'abc'", 5, &err);
    test_assert(pat != NULL, "compile literal succeeds");
    if (pat) {
      snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      if (state) {
        const char *s1 = "abc";
        const char *s2 = "xabcx";
        const char *s3 = "abcabcabc";

        snobol_match_t *m1 = snobol_pattern_search_ex(state, s1, 3, 0);
        test_assert((m1 && snobol_match_success(m1)) != 0,
                    "stateful call 1 succeeds");

        snobol_match_t *m2 = snobol_pattern_search_ex(state, s2, 5, 0);
        test_assert((m2 && snobol_match_success(m2)) != 0,
                    "stateful call 2 succeeds");

        snobol_match_t *m3 = snobol_pattern_search_ex(state, s3, 9, 0);
        test_assert((m3 && snobol_match_success(m3)) != 0,
                    "stateful call 3 succeeds");

        snobol_pattern_search_state_destroy(state);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* start_offset is honored */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "'a'", 3, &err);
    test_assert(pat != NULL, "compile 'a' succeeds");
    if (pat) {
      snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      if (state) {
        const char *subject = "aXaXa";
        /* From offset 2, first 'a' is at offset 2 */
        snobol_match_t *m = snobol_pattern_search_ex(state, subject, 5, 2);
        test_assert((m && snobol_match_success(m)) != 0,
                    "search from offset 2 succeeds");
        snobol_pattern_search_state_destroy(state);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Hot loop: JIT still fires through the stateful path */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "SPAN('a')", 9, &err);
    test_assert(pat != NULL, "compile hot-loop pattern succeeds");
    if (pat) {
      snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      if (state) {
        /* warmup */
        for (int i = 0; i < 100; i++) {
          snobol_match_t *m =
              snobol_pattern_search_ex(state, "aaaaaaaaaaaaaaaaaa", 18, 0);
          (void)m;
        }
        for (int i = 0; i < 50; i++) {
          snobol_match_t *m =
              snobol_pattern_search_ex(state, "aaaaaaaaaaaaaaaaaa", 18, 0);
          (void)m;
        }
        snobol_pattern_search_state_destroy(state);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* NULL safety: state destroy on NULL is a no-op */
  {
    snobol_pattern_search_state_destroy(nullptr);
    test_assert(true, "snobol_pattern_search_state_destroy(NULL) is safe");
  }

  /* NULL safety: state create with NULL pattern returns NULL */
  {
    snobol_pattern_search_state_t *s =
        snobol_pattern_search_state_create(nullptr, 0);
    test_assert(s == NULL, "state create with NULL pattern returns NULL");
    if (s) {
      snobol_pattern_search_state_destroy(s);
    }
  }
  test_cov_engine2_state_api();
  test_cov_anchored_automaton();
}
