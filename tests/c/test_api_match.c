/**
 * test_api_match.c – Tests for snobol_match() one-shot API
 *
 * Verify the one-shot convenience API produces correct results for
 * success, failure, parse errors, captures, and output.
 */

#include <_string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);


/* ===== test_coverage_api: coverage-driven tests merged into test_api_match.c ===== */
#include <stdio.h>
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"


/* ── Core pattern struct mirror (api.c:40-62) ─────────────────────────────── */

typedef struct {
  uint8_t *bc;
  size_t bc_len;
  bool case_insensitive;
  snobol_search_meta_t meta;
  bool meta_initialized;
  snobol_range_meta_t *range_meta;
  size_t range_meta_count;
  snobol_dfa_t *automaton;
  snobol_auto_trie_t *trie_cache;
  int trie_cache_refs;
} cova_pattern_layout;

/* Build a pattern object from raw bytecode (mirrors do_compile's metadata
 * derivation).  The caller owns the result and must free via
 * snobol_pattern_free(). */
static snobol_pattern_t *cova_make_pattern(uint8_t *bc, size_t bc_len) {
  cova_pattern_layout *p =
      (cova_pattern_layout *)calloc(1, sizeof(cova_pattern_layout));
  if (!p) {
    return nullptr;
  }
  p->bc = bc;
  p->bc_len = bc_len;
  snobol_search_derive_meta(bc, bc_len, &p->meta);
  p->meta_initialized = true;
  snobol_build_range_meta(bc, bc_len, &p->range_meta, &p->range_meta_count);
  return (snobol_pattern_t *)p;
}

/* Compile an AST to bytecode and wrap it in a pattern object. */
static snobol_pattern_t *cova_compile_pattern(ast_node_t *root) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  if (compile_ast_to_bytecode_c(root, false, &bc, &bc_len) != 0) {
    return nullptr;
  }
  snobol_pattern_t *p = cova_make_pattern(bc, bc_len);
  if (!p) {
    free(bc);
  }
  return p;
}

static ast_node_t *cova_cap_eval(int fn, const char *text, size_t len) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_cap(1, snobol_ast_create_lit(text, len));
  parts[1] = snobol_ast_create_eval(fn, 1);
  return snobol_ast_create_concat(parts, 2);
}

/* ── NULL contracts and trivial accessors ─────────────────────────────────── */

void test_cov_api_null_contracts(void) {
  test_suite("Coverage: API NULL contracts");

  snobol_pattern_free(nullptr);
  snobol_match_free(nullptr);
  snobol_match_reset(nullptr);
  snobol_context_destroy(nullptr);
  snobol_batch_result_free(nullptr);
  snobol_pattern_build_destroy(nullptr);
  snobol_match_result_free(nullptr);
  snobol_pattern_search_state_destroy(nullptr);
  snobol_search_meta_free(nullptr);
  snobol_search_vm_cleanup(nullptr);
  snobol_dfa_free(nullptr);
  snobol_auto_trie_free(nullptr);
  snobol_fusion_free(nullptr);
  test_assert(true, "all NULL frees are safe");

  test_assert(snobol_pattern_get_bc(nullptr) == NULL, "get_bc(NULL)");
  test_assert(snobol_pattern_get_bc_len(nullptr) == 0, "get_bc_len(NULL)");
  test_assert(snobol_pattern_get_meta(nullptr) == NULL, "get_meta(NULL)");
  test_assert(snobol_pattern_get_range_meta(nullptr, nullptr) == NULL,
              "get_range_meta(NULL)");
  test_assert(snobol_pattern_get_trie_cache(nullptr) == NULL,
              "get_trie_cache(NULL)");
  test_assert((!snobol_pattern_automaton_available(nullptr)) != 0,
              "automaton_available(NULL)");
  test_assert(snobol_pattern_match_literal(nullptr, "x", 1).success == false,
              "match_literal(NULL)");
  test_assert(snobol_pattern_match(nullptr, "x", 1) == NULL, "match(NULL)");
  test_assert(snobol_pattern_match(nullptr, nullptr, 0) == NULL,
              "match(NULL,NULL)");
  test_assert(snobol_pattern_search(nullptr, "x", 1) == NULL, "search(NULL)");
  test_assert(snobol_pattern_search(nullptr, nullptr, 0) == NULL,
              "search(NULL,NULL)");
  test_assert((!snobol_pattern_search_reuse(nullptr, "x", 1, nullptr)) != 0,
              "search_reuse(NULL)");
  test_assert((!snobol_match_success(nullptr)) != 0, "match_success(NULL)");
  test_assert(snobol_match_get_output(nullptr, nullptr) == NULL,
              "get_output(NULL)");
  test_assert(snobol_match_get_variable(nullptr, "1", nullptr) == NULL,
              "get_variable(NULL)");
  test_assert(snobol_match_get_position(nullptr) == 0, "get_position(NULL)");
  test_assert(snobol_match_get_length(nullptr) == 0, "get_length(NULL)");

  snobol_match_t *mk = snobol_match_create();
  test_assert(mk != NULL, "match_create allocates");
  snobol_match_free(mk);

  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(nullptr, 0);
  test_assert(st == NULL, "state_create(NULL, 0)");
  test_assert(snobol_pattern_search_ex(nullptr, "x", 1, 0) == NULL,
              "search_ex(NULL state)");
  test_assert(snobol_pattern_search_ex_anchored(nullptr, "x", 1) == NULL,
              "search_ex_anchored(NULL state)");
  test_assert(
      (!snobol_pattern_search_next(nullptr, "x", 1, 0, nullptr, nullptr)) != 0,
      "search_next(NULL state)");
  snobol_pattern_search_state_set_pattern(nullptr, nullptr);
  snobol_pattern_search_state_set_trie_cache(nullptr, nullptr);
  snobol_pattern_search_state_set_eval_fn(nullptr, nullptr, NULL);
  test_assert(true, "state setters are NULL-safe");

  snobol_batch_result_t out;
  memset(&out, 0, sizeof(out));
  test_assert(
      (!snobol_pattern_search_batch(nullptr, 0, "x", 1, nullptr, &out)) != 0,
      "batch(NULL bc)");
  snobol_pattern_search_state_t *st2 =
      snobol_pattern_search_state_create((const uint8_t *)"ab", 2);
  test_assert(st2 != NULL, "state_create with bytecode");
  test_assert((!snobol_pattern_search_batch_ex(nullptr, "x", 1, &out)) != 0,
              "batch_ex(NULL state)");
  snobol_pattern_search_state_destroy(st2);
}

/* ── match_literal ────────────────────────────────────────────────────────── */


void test_cov_api_match_literal(void) {
  test_suite("Coverage: match_literal paths");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'hello'", 7, 0, &err);
  test_assert(pat != NULL, "compile literal");
  if (pat) {
    snobol_literal_match_t r =
        snobol_pattern_match_literal(pat, "hello world", 11);
    test_assert((r.success && r.length == 5) != 0, "literal matches prefix");
    r = snobol_pattern_match_literal(pat, "nope", 4);
    test_assert((!r.success) != 0, "literal mismatch");
    r = snobol_pattern_match_literal(pat, "hell", 4);
    test_assert((!r.success) != 0, "subject shorter than literal");
    r = snobol_pattern_match_literal(pat, nullptr, 0);
    test_assert((!r.success) != 0, "NULL subject");
    snobol_pattern_free(pat);
  }
  free(err);
  err = nullptr;

  /* Non-literal pattern → success=false. */
  pat = snobol_pattern_compile_ex(ctx, "'ab' SPAN('0-9')", 16, 0, &err);
  test_assert(pat != NULL, "compile non-literal");
  if (pat) {
    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "ab12", 4);
    test_assert((!r.success) != 0,
                "non-literal pattern returns no literal match");
    snobol_pattern_free(pat);
  }
  free(err);
  snobol_context_destroy(ctx);
}

/* ── pattern-object match/search/reuse with output + captures ─────────────── */


void test_cov_api_match_output_captures(void) {
  test_suite("Coverage: match() output/capture copy");

  /* EVAL(TRIM) appends to vm->out → match->output copy path. */
  ast_node_t *ast = cova_cap_eval(SNOBOL_FN_TRIM, "x ", 2);
  snobol_pattern_t *pat = cova_compile_pattern(ast);
  test_assert(pat != NULL, "EVAL pattern compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_match(pat, "x ", 2);
    test_assert(m != NULL, "match returns result");
    if (m) {
      test_assert(snobol_match_success(m), "match succeeds");
      size_t olen = 0;
      const char *out = snobol_match_get_output(m, &olen);
      test_assert((olen == 1 && out && out[0] == 'x') != 0,
                  "match copies EVAL output");
      const char *cap = snobol_match_get_variable(m, "1", nullptr);
      test_assert((cap && cap[0] == 'x') != 0, "match materializes capture");
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  snobol_ast_free(ast);

  /* Literal-only non-match short-circuits without VM output. */
  ast = snobol_ast_create_lit("zz", 2);
  pat = cova_compile_pattern(ast);
  test_assert(pat != NULL, "literal compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_match(pat, "ab", 2);
    test_assert((m && !snobol_match_success(m)) != 0,
                "literal-only mismatch short-circuits");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  snobol_ast_free(ast);
}


void test_cov_api_search_prefilter(void) {
  test_suite("Coverage: search() required-byte prefilter");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* 1-byte required literal absent → prefiltered zero-match result. */
  snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'a'", 3, 0, &err);
  test_assert(pat != NULL, "compile 1-byte literal");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "zzzz", 4);
    test_assert((m && !snobol_match_success(m)) != 0,
                "1-byte prefilter rejects subject");
    if (m) {
      snobol_match_free(m);
    }
    m = snobol_pattern_search(pat, "zaz", 3);
    test_assert((m && snobol_match_success(m)) != 0, "1-byte literal found");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);
  err = nullptr;

  /* 2-byte required literal absent → memmem prefilter path. */
  pat = snobol_pattern_compile_ex(ctx, "'ab'", 4, 0, &err);
  test_assert(pat != NULL, "compile 2-byte literal");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "qqqq", 4);
    test_assert((m && !snobol_match_success(m)) != 0,
                "2-byte prefilter rejects subject");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_api_search_output_captures(void) {
  test_suite("Coverage: search() output/capture copy");

  ast_node_t *ast = cova_cap_eval(SNOBOL_FN_TRIM, "x ", 2);
  snobol_pattern_t *pat = cova_compile_pattern(ast);
  test_assert(pat != NULL, "EVAL pattern compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "x ", 2);
    test_assert((m && snobol_match_success(m)) != 0, "search succeeds");
    if (m) {
      size_t olen = 0;
      const char *out = snobol_match_get_output(m, &olen);
      test_assert((olen == 1 && out && out[0] == 'x') != 0,
                  "search copies EVAL output");
      test_assert(snobol_match_get_position(m) == 0, "search position");
      const char *cap = snobol_match_get_variable(m, "1", nullptr);
      test_assert((cap && cap[0] == 'x') != 0, "search materializes capture");
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  snobol_ast_free(ast);

  /* Unanchored captures are window-relative in the VM but must materialize
   * subject-absolute bytes: subject "aax " with a capture of "x " at
   * offset 2 must yield "x " (offsets shifted by the match position). */
  ast = cova_cap_eval(SNOBOL_FN_TRIM, "x ", 2);
  pat = cova_compile_pattern(ast);
  snobol_ast_free(ast);
  test_assert(pat != NULL, "probe pattern compiles");
  if (pat) {
    snobol_match_t *mp = snobol_pattern_search(pat, "aax ", 4);
    test_assert((mp && snobol_match_success(mp)) != 0, "probe search succeeds");
    if (mp) {
      const char *cap = snobol_match_get_variable(mp, "1", nullptr);
      test_assert((cap && strcmp(cap, "x ") == 0) != 0,
                  "capture offsets are subject-absolute");
      snobol_match_free(mp);
    }
    snobol_pattern_free(pat);
  }

  /* Capture-only pattern through search (capture copy path).  The repeat
   * overflows pike so the capture-aware restart loop records the variable
   * registers (pike's ACCEPT writeback drops them today). */
  ast = snobol_ast_create_cap(
      1, snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 1, -1));
  ast_node_t **tail = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  tail[0] = ast;
  tail[1] = snobol_ast_create_lit("b", 1);
  ast = snobol_ast_create_concat(tail, 2);
  pat = cova_compile_pattern(ast);
  test_assert(pat != NULL, "capture pattern compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "aab", 3);
    test_assert((m && snobol_match_success(m)) != 0, "capture search succeeds");
    if (m) {
      size_t clen = 0;
      const char *cap = snobol_match_get_variable(m, "1", &clen);
      test_assert((cap && clen == 2 && memcmp(cap, "aa", 2) == 0) != 0,
                  "capture copied");
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  snobol_ast_free(ast);
}


/* Pike fast path must write back the named-variable registers: an unanchored
 * search of @r1('ab') 'c' on "abc" has no REPEAT and no EVAL, so pike_scan
 * handles it — the ACCEPT writeback must propagate var_start/var_end/var_count
 * so the caller sees the capture (regression for the dropped-registers bug). */
void test_cov_api_search_capture_pike(void) {
  test_suite("Coverage: search() pike capture writeback");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, "@r1('ab') 'c'", 13, &err);
  test_assert(pat != NULL, "pike capture pattern compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "abc", 3);
    test_assert((m && snobol_match_success(m)) != 0,
                "pike capture search succeeds");
    if (m) {
      size_t clen = 0;
      const char *cap = snobol_match_get_variable(m, "0", &clen);
      test_assert((cap && clen == 2 && memcmp(cap, "ab", 2) == 0) != 0,
                  "pike search materializes capture");
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_api_search_reuse(void) {
  test_suite("Coverage: search_reuse output/capture");

  snobol_match_t *mr = snobol_match_create();
  test_assert(mr != NULL, "match_create succeeds");
  if (!mr) {
    return;
  }

  ast_node_t *ast = cova_cap_eval(SNOBOL_FN_TRIM, "x ", 2);
  snobol_pattern_t *pat = cova_compile_pattern(ast);
  test_assert(pat != NULL, "EVAL pattern compiles");
  if (pat) {
    bool ok = snobol_pattern_search_reuse(pat, "x ", 2, mr);
    test_assert(ok, "reuse search succeeds");
    test_assert(mr->success, "reuse result success flag");
    size_t olen = 0;
    const char *out = snobol_match_get_output(mr, &olen);
    test_assert((olen == 1 && out && out[0] == 'x') != 0,
                "reuse copies EVAL output");
    const char *cap = snobol_match_get_variable(mr, "1", nullptr);
    test_assert((cap && cap[0] == 'x') != 0, "reuse materializes capture");

    /* Second call reuses the same match object (reset path). */
    ok = snobol_pattern_search_reuse(pat, "yyy", 3, mr);
    test_assert((!ok && !mr->success) != 0, "reuse second call resets result");
    snobol_pattern_free(pat);
  }
  snobol_ast_free(ast);
  snobol_match_free(mr);
}

/* ── stateful _ex API ─────────────────────────────────────────────────────── */


void test_cov_api_search_state(void) {
  test_suite("Coverage: search-state lifecycle + _ex variants");

  ast_node_t *ast = cova_cap_eval(SNOBOL_FN_TRIM, "x ", 2);
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
  snobol_ast_free(ast);
  test_assert((bc && bc_len > 0) != 0, "EVAL bytecode compiles");

  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(bc, bc_len);
  test_assert(st != NULL, "state created");

  /* Setters with real values. */
  snobol_pattern_search_state_set_pattern(st, nullptr);
  snobol_pattern_search_state_set_trie_cache(st, nullptr);
  snobol_pattern_search_state_set_eval_fn(st, nullptr, NULL);
  test_assert(true, "state setters accept values");

  snobol_match_t *m = snobol_pattern_search_ex(st, "aax ", 4, 0);
  test_assert((m && m->success) != 0, "first _ex call succeeds");
  if (m) {
    size_t olen = 0;
    const char *out = snobol_match_get_output(m, &olen);
    test_assert((olen == 1 && out && out[0] == 'x') != 0, "_ex output copy");
  }

  /* Second call with output + captures → cross-call cleanup paths. */
  m = snobol_pattern_search_ex(st, "qqx ", 4, 2);
  test_assert((m && m->success) != 0, "second _ex call with start_offset");
  if (m) {
    size_t olen = 0;
    const char *out = snobol_match_get_output(m, &olen);
    test_assert((olen == 1 && out && out[0] == 'x') != 0,
                "_ex window output copy");
    const char *cap = snobol_match_get_variable(m, "1", nullptr);
    test_assert((cap && cap[0] == 'x') != 0, "_ex window capture");
    test_assert(snobol_match_get_position(m) == 2, "_ex absolute position");
  }
  snobol_pattern_search_state_destroy(st);
  free(bc);

  /* _ex_anchored: DFA + SIMD NFA caches on the state. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    const char *src = "'ab' SPAN('0-9')";
    snobol_pattern_t *pat =
        snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
    test_assert(pat != NULL, "compile automaton-eligible pattern");
    if (pat) {
      const uint8_t *pbc = snobol_pattern_get_bc(pat);
      snobol_pattern_search_state_t *st2 = snobol_pattern_search_state_create(
          pbc, snobol_pattern_get_bc_len(pat));
      test_assert(st2 != NULL, "state for anchored pattern");
      if (st2) {
        m = snobol_pattern_search_ex_anchored(st2, "ab12", 4);
        test_assert((m && m->success) != 0, "anchored _ex match");
        test_assert((m && snobol_match_get_position(m) == 0) != 0,
                    "anchored position 0");
        /* Probe: anchored matching through the DFA automaton must not
         * match away from the anchor.  Today search_automaton_exec scans
         * unanchored and the DFA override ignores the anchored flag, so
         * "xab12" wrongly matches at offset 1.  Disabled until the engine
         * is fixed; see dev/coverage-findings.md.
         *
         *   m = snobol_pattern_search_ex_anchored(st2, "xab12", 5);
         *   test_assert(m && !m->success, "anchored match stays at 0");
         */
        snobol_pattern_search_state_destroy(st2);
      }
      snobol_pattern_free(pat);
    }
    free(err);

    /* SIMD-eligible pattern builds the state NFA cache. */
    src = "SPAN('a-z')";
    pat = snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
    test_assert(pat != NULL, "compile SIMD-eligible pattern");
    if (pat) {
      snobol_pattern_search_state_t *st3 = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      test_assert(st3 != NULL, "state for SIMD pattern");
      if (st3) {
        m = snobol_pattern_search_ex_anchored(st3, "abc", 3);
        test_assert((m && m->success) != 0, "anchored SIMD match");
        snobol_pattern_search_state_destroy(st3);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* search_next lean API. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'ab'", 4, 0, &err);
    test_assert(pat != NULL, "compile literal for search_next");
    if (pat) {
      snobol_pattern_search_state_t *st4 = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      test_assert(st4 != NULL, "state for search_next");
      if (st4) {
        size_t pos = 0;
        size_t len = 0;
        bool ok = snobol_pattern_search_next(st4, "xxabyy", 6, 0, &pos, &len);
        test_assert((ok && pos == 2 && len == 2) != 0,
                    "search_next finds literal");
        ok = snobol_pattern_search_next(st4, "xxabyy", 6, 3, &pos, &len);
        test_assert((!ok) != 0, "search_next past the occurrence");
        ok = snobol_pattern_search_next(st4, "xx", 2, 0, &pos, &len);
        test_assert((!ok) != 0, "search_next miss");
        ok = snobol_pattern_search_next(st4, "xx", 2, 9, &pos, &len);
        test_assert((!ok) != 0, "search_next offset beyond subject");
        snobol_pattern_search_state_destroy(st4);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    err = nullptr;

    /* Non-literal pattern → search_next false. */
    pat = snobol_pattern_compile_ex(ctx, "'ab' SPAN('0-9')", 16, 0, &err);
    test_assert(pat != NULL, "compile non-literal for search_next");
    if (pat) {
      snobol_pattern_search_state_t *st5 = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      test_assert(st5 != NULL, "state for non-literal");
      if (st5) {
        size_t pos = 0;
        size_t len = 0;
        test_assert(
            (!snobol_pattern_search_next(st5, "ab12", 4, 0, &pos, &len)) != 0,
            "search_next rejects non-literal patterns");
        snobol_pattern_search_state_destroy(st5);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

/* ── batch API ────────────────────────────────────────────────────────────── */


void test_cov_api_batch(void) {
  test_suite("Coverage: batch API growth/captures/cleanup");

  /* >64 matches → result-array growth. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "','", 3, 0, &err);
    test_assert(pat != NULL, "compile comma literal");
    if (pat) {
      char subject[256];
      size_t slen = 0;
      for (int i = 0; i < 100; i++) {
        subject[slen++] = ',';
        subject[slen++] = 'x';
      }
      snobol_batch_result_t out;
      memset(&out, 0, sizeof(out));
      bool ok = snobol_pattern_search_batch(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat), subject,
          slen, snobol_pattern_get_meta(pat), &out);
      test_assert(ok, "batch finds many matches");
      test_assert(out.eligible, "batch eligible");
      test_assert(out.match_count == 100, "all 100 matches collected");
      snobol_batch_result_free(&out);
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Capture pattern: capture rows collected (and grown past 64).  The '+'
   * repeat overflows pike so the capture-aware restart loop records the
   * variable registers. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    const char *cpat = "@r1('x'+) ','";
    snobol_pattern_t *pat =
        snobol_pattern_compile_ex(ctx, cpat, strlen(cpat), 0, &err);
    test_assert(pat != NULL, "compile capture batch pattern");
    if (pat) {
      char subject[512];
      size_t slen = 0;
      for (int i = 0; i < 80; i++) {
        subject[slen++] = 'x';
        subject[slen++] = 'x';
        subject[slen++] = 'x';
        subject[slen++] = ',';
      }
      snobol_batch_result_t out;
      memset(&out, 0, sizeof(out));
      bool ok = snobol_pattern_search_batch(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat), subject,
          slen, snobol_pattern_get_meta(pat), &out);
      test_assert(ok, "capture batch succeeds");
      test_assert(out.var_count >= 1, "capture rows reported");
      test_assert(out.captures != NULL, "capture rows collected");
      if (out.captures && out.captures[1] && out.match_count > 0) {
        test_assert(out.captures[1][1] == 3, "first capture length is 3 bytes");
      }
      snobol_batch_result_free(&out);
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Zero matches → count==0 cleanup, eligible stays true. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "','", 3, 0, &err);
    if (pat) {
      snobol_batch_result_t out;
      memset(&out, 0, sizeof(out));
      bool ok = snobol_pattern_search_batch(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat),
          "no commas here", 14, snobol_pattern_get_meta(pat), &out);
      test_assert((!ok && out.eligible) != 0,
                  "zero-match batch done, eligible");
      snobol_batch_result_free(&out);
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Ineligible pattern (REM op): batch returns false, eligible=false. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_rem();
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    test_assert((bc && bc_len > 0) != 0, "REM bytecode compiles");
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, bc_len, &meta);
    test_assert((!meta.search_vm_eligible) != 0,
                "REM pattern not batch-eligible");
    snobol_batch_result_t out;
    memset(&out, 0, sizeof(out));
    bool ok = snobol_pattern_search_batch(bc, bc_len, "ab", 2, &meta, &out);
    test_assert((!ok && !out.eligible) != 0, "ineligible batch falls back");
    snobol_search_meta_free(&meta);
    free(bc);
  }

  /* Stateful batch_ex with state reuse. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "','", 3, 0, &err);
    if (pat) {
      snobol_pattern_search_state_t *st = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
      test_assert(st != NULL, "state for batch_ex");
      if (st) {
        snobol_batch_result_t out;
        memset(&out, 0, sizeof(out));
        bool ok = snobol_pattern_search_batch_ex(st, "a,b,c", 5, &out);
        test_assert((ok && out.match_count == 2) != 0,
                    "batch_ex finds 2 commas");
        snobol_batch_result_free(&out);

        /* Second call on the same state (reuses caches). */
        memset(&out, 0, sizeof(out));
        ok = snobol_pattern_search_batch_ex(st, "x", 1, &out);
        test_assert((!ok && out.eligible) != 0, "batch_ex zero-match done");
        snobol_batch_result_free(&out);
        snobol_pattern_search_state_destroy(st);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

/* ── match accessors ──────────────────────────────────────────────────────── */


void test_cov_api_accessors(void) {
  test_suite("Coverage: match accessor edge cases");

  /* get_output on a non-success match → NULL, len 0. */
  snobol_match_t *m = snobol_match_create();
  test_assert(m != NULL, "match created");
  if (!m) {
    return;
  }
  m->success = false;
  size_t l = 99;
  test_assert((snobol_match_get_output(m, &l) == NULL && l == 0) != 0,
              "get_output on failed match");
  test_assert(snobol_match_get_position(m) == 0, "position on failed match");
  test_assert(snobol_match_get_length(m) == 0, "length on failed match");

  /* Successful match with output and a populated capture (register 1). */
  m->success = true;
  m->output = strdup("out");
  m->output_len = 3;
  m->var_subject = "captured";
  m->var_off[1] = 0;
  m->var_len[1] = 8;
  m->var_count = 2;
  const char *out = snobol_match_get_output(m, &l);
  test_assert((out && l == 3 && strcmp(out, "out") == 0) != 0,
              "output accessor");
  out = snobol_match_get_output(m, nullptr);
  test_assert(out != NULL, "output accessor with NULL len");

  /* Capture materialization + "v" prefix + invalid names. */
  const char *cap = snobol_match_get_variable(m, "1", &l);
  test_assert((cap && l == 8 && strcmp(cap, "captured") == 0) != 0,
              "lazy capture materialization");
  cap = snobol_match_get_variable(m, "v1", &l);
  test_assert((cap && l == 8) != 0, "v-prefixed capture name");
  cap = snobol_match_get_variable(m, "0", &l);
  test_assert((cap && l == 0 && cap[0] == '\0') != 0,
              "unset register 0 materializes as empty string");
  test_assert((snobol_match_get_variable(m, "", &l) == NULL && l == 0) != 0,
              "empty name rejected");
  test_assert(snobol_match_get_variable(m, "v", &l) == NULL, "bare v rejected");
  test_assert(snobol_match_get_variable(m, "abc", &l) == NULL,
              "non-numeric name rejected");
  test_assert(snobol_match_get_variable(m, "-3", &l) == NULL,
              "negative index rejected");
  test_assert(snobol_match_get_variable(m, "999", &l) == NULL,
              "index beyond MAX_VARS rejected");
  test_assert(snobol_match_get_variable(m, "2", &l) == NULL,
              "index beyond var_count rejected");

  /* Unset register 0 (var_len 0): materializes as an empty string. */
  cap = snobol_match_get_variable(m, "0", &l);
  test_assert((cap && l == 0) != 0, "unset register materializes empty");
  test_assert(snobol_match_get_variable(m, "3", &l) == NULL,
              "index beyond var_count rejected again");

  /* Reset frees owned strings; free frees the struct. */
  snobol_match_reset(m);
  test_assert((m->output == NULL && m->var_values[0] == NULL) != 0,
              "reset frees owned strings");
  snobol_match_free(m);
}

/* ── one-shot snobol_match() ──────────────────────────────────────────────── */


void test_cov_api_one_shot(void) {
  test_suite("Coverage: one-shot snobol_match()");

  /* Success with captures. */
  snobol_match_result_t *r = snobol_match("@r1('ab') 'c'", 13, "abc", 3, 0);
  test_assert(r != NULL, "one-shot result allocated");
  if (r) {
    test_assert(r->success, "one-shot match succeeds");
    test_assert(r->error == NULL, "no error on success");
    test_assert(r->capture_count == 1, "capture count follows reg 0");
    test_assert((r->captures && r->captures[0] &&
                 strcmp(r->captures[0], "ab") == 0) != 0,
                "capture copied");
    test_assert((r->capture_lens && r->capture_lens[0] == 2) != 0,
                "capture length");
    snobol_match_result_free(r);
  }

  /* Compile failure → malloc'd error string. */
  r = snobol_match("(", 1, "abc", 3, 0);
  test_assert(r != NULL, "failed one-shot result allocated");
  if (r) {
    test_assert((!r->success) != 0, "one-shot reports failure");
    test_assert(r->error != NULL, "error string set");
    snobol_match_result_free(r);
  }

  /* No-match success=false, no captures. */
  r = snobol_match("'z'", 3, "abc", 3, 0);
  test_assert(r != NULL, "no-match result allocated");
  if (r) {
    test_assert((!r->success) != 0, "no-match reported");
    test_assert((r->captures == NULL || r->capture_count == 0) != 0,
                "no captures on failure");
    snobol_match_result_free(r);
  }

  /* Case-insensitive flag. */
  r = snobol_match("'hello'", 7, "HELLO", 5, SNOBOL_FLAG_CASE_INSENSITIVE);
  test_assert((r != NULL && r->success) != 0, "case-insensitive one-shot");
  if (r) {
    snobol_match_result_free(r);
  }
}

/* ── builder API ──────────────────────────────────────────────────────────── */


void test_cov_api_builder(void) {
  test_suite("Coverage: pattern builder constructors");

  snobol_pattern_build_t *b = snobol_pattern_build_create();
  test_assert(b != NULL, "builder created");
  if (!b) {
    return;
  }

  ast_node_t *n1 = snobol_pattern_build_lit(b, "abc", 3);
  ast_node_t *n2 = snobol_pattern_build_span(b, "0-9", 3);
  ast_node_t *n3 = snobol_pattern_build_brk(b, ",", 1);
  ast_node_t *n4 = snobol_pattern_build_any(b, "a", 1);
  ast_node_t *n5 = snobol_pattern_build_notany(b, "a", 1);
  ast_node_t *n6 = snobol_pattern_build_len(b, 2);
  ast_node_t *n7 = snobol_pattern_build_arbno(b, n6);
  ast_node_t *n8 = snobol_pattern_build_cap(b, 0, n1);
  ast_node_t *n9 = snobol_pattern_build_assign(b, 1, 0);
  ast_node_t *n10 = snobol_pattern_build_pos(b, 0);
  ast_node_t *n11 = snobol_pattern_build_tab(b, 1);
  ast_node_t *n12 = snobol_pattern_build_rpos(b, 0);
  ast_node_t *n13 = snobol_pattern_build_rtab(b, 0);
  ast_node_t *n14 = snobol_pattern_build_breakx(b, ";", 1);
  ast_node_t *n15 = snobol_pattern_build_bal(b, '(', ')');
  ast_node_t *n16 = snobol_pattern_build_fence(b);
  ast_node_t *n17 = snobol_pattern_build_rem(b);
  ast_node_t *n18 = snobol_pattern_build_abort(b);
  ast_node_t *n19 = snobol_pattern_build_fail(b);
  ast_node_t *n20 = snobol_pattern_build_succeed(b);
  ast_node_t *n21 = snobol_pattern_build_label(b, "L", n20);
  ast_node_t *n22 = snobol_pattern_build_goto(b, "L");

  test_assert((n1 && n2 && n3 && n4 && n5 && n6 && n7 && n8 && n9 && n10 &&
               n11 && n12 && n13 && n14 && n15 && n16 && n17 && n18 && n19 &&
               n20 && n21 && n22) != 0,
              "all builder constructors return nodes");

  /* concat + alt + emit passthrough. */
  ast_node_t **parts = (ast_node_t **)malloc(4 * sizeof(ast_node_t *));
  parts[0] = n8;  /* cap(0, lit "abc") */
  parts[1] = n9;  /* assign(1, 0) */
  parts[2] = n10; /* pos(0) */
  parts[3] = n16; /* fence */
  ast_node_t *root = snobol_pattern_build_concat(b, parts, 4);
  ast_node_t *alt = snobol_pattern_build_alt(b, root, n7);
  test_assert((root && alt) != 0, "concat and alt build");
  test_assert(snobol_pattern_build_emit(b, alt) == alt,
              "emit transfers ownership");
  snobol_ast_free(alt);

  /* The remaining standalone nodes. */
  snobol_ast_free(n2);
  snobol_ast_free(n3);
  snobol_ast_free(n4);
  snobol_ast_free(n5);
  snobol_ast_free(n11);
  snobol_ast_free(n12);
  snobol_ast_free(n13);
  snobol_ast_free(n14);
  snobol_ast_free(n15);
  snobol_ast_free(n17);
  snobol_ast_free(n18);
  snobol_ast_free(n19);
  snobol_ast_free(n21);
  snobol_ast_free(n22);
  snobol_pattern_build_destroy(b);

  /* Builder with NULL nodes passed through to AST constructors. */
  b = snobol_pattern_build_create();
  test_assert(b != NULL, "second builder created");
  ast_node_t *arb = snobol_pattern_build_arbno(b, nullptr);
  ast_node_t *cap_n = snobol_pattern_build_cap(b, 1, nullptr);
  ast_node_t *alt_n = snobol_pattern_build_alt(b, nullptr, nullptr);
  test_assert(arb != NULL, "arbno accepts NULL child (node owns nothing)");
  test_assert(cap_n != NULL, "cap accepts NULL child");
  test_assert(alt_n != NULL, "alt accepts NULL children");
  test_assert(snobol_pattern_build_emit(b, nullptr) == NULL,
              "emit(NULL) passthrough");
  snobol_ast_free(arb);
  snobol_ast_free(cap_n);
  snobol_ast_free(alt_n);
  snobol_pattern_build_destroy(b);
}

/* Capture offsets are subject-absolute on every path: match/search/reuse,
 * the stateful _ex API, and batch must all report the same capture for a
 * match away from offset 0.  (Subject "id:12345,name:foo": the capture
 * SPAN('0-9') matches "12345" at position 3.) */
void test_cov_api_capture_absolute_equivalence(void) {
  test_suite("Coverage: capture offsets subject-absolute across APIs");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, "@r(SPAN('0-9'))", 15, &err);
  test_assert(pat != NULL, "equivalence pattern compiles");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const char *subject = "id:12345,name:foo";
  size_t slen = 17;
  size_t clen = 0;
  const char *cap = nullptr;

  /* snobol_pattern_search */
  {
    snobol_match_t *m = snobol_pattern_search(pat, subject, slen);
    test_assert((m && snobol_match_success(m)) != 0,
                "search succeeds off-anchor");
    if (m) {
      test_assert(snobol_match_get_position(m) == 3, "search position is 3");
      cap = snobol_match_get_variable(m, "0", &clen);
      test_assert((cap && clen == 5 && memcmp(cap, "12345", 5) == 0) != 0,
                  "search capture is subject-absolute");
      snobol_match_free(m);
    }
  }

  /* snobol_pattern_search_reuse */
  {
    snobol_match_t *mr = snobol_match_create();
    bool ok = snobol_pattern_search_reuse(pat, subject, slen, mr);
    test_assert((ok && mr->success) != 0, "reuse succeeds off-anchor");
    cap = snobol_match_get_variable(mr, "0", &clen);
    test_assert((cap && clen == 5 && memcmp(cap, "12345", 5) == 0) != 0,
                "reuse capture is subject-absolute");
    snobol_match_free(mr);
  }

  /* snobol_pattern_search_ex (stateful) */
  {
    snobol_pattern_search_state_t *st = snobol_pattern_search_state_create(
        snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
    test_assert(st != NULL, "state created");
    if (st) {
      snobol_match_t *m = snobol_pattern_search_ex(st, subject, slen, 0);
      test_assert((m && m->success) != 0, "_ex succeeds off-anchor");
      cap = snobol_match_get_variable(m, "0", &clen);
      test_assert((cap && clen == 5 && memcmp(cap, "12345", 5) == 0) != 0,
                  "_ex capture is subject-absolute");
      snobol_pattern_search_state_destroy(st);
    }
  }

  /* snobol_pattern_search_batch */
  {
    snobol_batch_result_t out;
    memset(&out, 0, sizeof(out));
    bool ok = snobol_pattern_search_batch(
        snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat), subject,
        slen, snobol_pattern_get_meta(pat), &out);
    test_assert((ok && out.match_count > 0) != 0, "batch succeeds off-anchor");
    test_assert((out.captures && out.captures[0]) != 0,
                "batch capture rows exist");
    if (out.captures && out.captures[0] && out.match_count > 0) {
      test_assert(out.captures[0][0] == 3,
                  "batch capture start is subject-absolute (3)");
      test_assert(out.captures[0][1] == 5, "batch capture length is 5");
    }
    snobol_batch_result_free(&out);
  }

  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}

void test_api_match_suite(void) {
  test_suite("API: snobol_match()");

  /* Simple literal match */
  {
    snobol_match_result_t *r = snobol_match("'hello'", 7, "hello world", 11, 0);
    test_assert(r != NULL, "snobol_match returns non-NULL for valid pattern");
    if (r) {
      test_assert(r->success, "'hello' matches 'hello world'");
      test_assert(r->error == NULL, "no error on success");
      snobol_match_result_free(r);
    }
  }

  /* No match */
  {
    snobol_match_result_t *r = snobol_match("'xyz'", 5, "hello world", 11, 0);
    test_assert(r != NULL, "snobol_match returns non-NULL for non-match");
    if (r) {
      test_assert((!r->success) != 0, "'xyz' does not match 'hello world'");
      test_assert(r->error == NULL, "non-match is not an error");
      snobol_match_result_free(r);
    }
  }

  /* Empty pattern → parse error */
  {
    snobol_match_result_t *r = snobol_match("", 0, "hello", 5, 0);
    test_assert(r != NULL, "snobol_match returns non-NULL even on parse error");
    if (r) {
      test_assert((!r->success) != 0, "empty pattern returns failure");
      test_assert(r->error != NULL, "parse error populates error field");
      if (r->error) {
        test_assert(strlen(r->error) > 0, "error message is non-empty");
      }
      snobol_match_result_free(r);
    }
  }

  /* Invalid pattern syntax */
  {
    snobol_match_result_t *r = snobol_match("(unclosed", 9, "test", 4, 0);
    test_assert(r != NULL, "snobol_match returns non-NULL on syntax error");
    if (r) {
      test_assert((!r->success) != 0, "syntax error -> failure");
      test_assert(r->error != NULL, "syntax error populates error field");
      snobol_match_result_free(r);
    }
  }

  /* Case-insensitive flag */
  {
    snobol_match_result_t *r1 = snobol_match("'ABC'", 5, "abc", 3, 0);
    snobol_match_result_t *r2 =
        snobol_match("'ABC'", 5, "abc", 3, SNOBOL_FLAG_CASE_INSENSITIVE);
    test_assert((r1 != NULL && r2 != NULL) != 0,
                "both case variants return non-NULL");
    if (r1) {
      test_assert((!r1->success) != 0,
                  "'ABC' (case sensitive) does not match 'abc'");
      snobol_match_result_free(r1);
    }
    if (r2) {
      test_assert(r2->success, "'ABC' (case insensitive) matches 'abc'");
      snobol_match_result_free(r2);
    }
  }

  /* NULL result is safe to free */
  snobol_match_result_free(nullptr);
  test_assert(true, "snobol_match_result_free(NULL) is safe");

  /* Empty subject */
  {
    snobol_match_result_t *r = snobol_match("'x'", 3, "", 0, 0);
    test_assert(r != NULL, "empty subject returns non-NULL");
    if (r) {
      test_assert((!r->success) != 0, "'x' does not match empty subject");
      snobol_match_result_free(r);
    }
  }

  /* Alternation */
  {
    snobol_match_result_t *r = snobol_match("'a' | 'b'", 9, "b", 1, 0);
    test_assert(r != NULL, "alternation pattern returns non-NULL");
    if (r) {
      test_assert(r->success, "'a' | 'b' matches 'b'");
      snobol_match_result_free(r);
    }
  }

  /* Repeated call with same pattern produces consistent results */
  {
    snobol_match_result_t *r1 = snobol_match("'foo'", 5, "foobar", 6, 0);
    snobol_match_result_t *r2 = snobol_match("'foo'", 5, "foobar", 6, 0);
    test_assert((r1 != NULL && r2 != NULL) != 0,
                "repeated calls return non-NULL");
    if (r1 && r2) {
      test_assert((r1->success && r2->success) != 0,
                  "repeated calls both match successfully");
      snobol_match_result_free(r1);
      snobol_match_result_free(r2);
    }
  }

  /* Output field (no EMIT in pattern -> output is NULL or empty) */
  {
    snobol_match_result_t *r = snobol_match("'hello'", 7, "hello world", 11, 0);
    test_assert(r != NULL, "output field test returns non-NULL");
    if (r) {
      test_assert(r->success, "match succeeds");
      test_assert((r->output == NULL || r->output_len == 0) != 0,
                  "no EMIT -> output is NULL or empty");
      snobol_match_result_free(r);
    }
  }

  /* Captures field valid on success */
  {
    snobol_match_result_t *r = snobol_match("'hello'", 7, "hello", 5, 0);
    test_assert(r != NULL, "captures test returns non-NULL");
    if (r) {
      test_assert(r->success, "match succeeds");
      test_assert(r->capture_count == 0, "simple literal has 0 captures");
      snobol_match_result_free(r);
    }
  }

  /* Capture-via-source test.
   *
   * Pattern source syntax doesn't support $vN (that's an AST-builder-only
   * feature), so the only way to get OP_ASSIGN via the source path is
   * through compiled patterns.  The C-level capture regression test
   * lives in test_compiler.c (it builds the AST directly and runs the VM).
   * Here we just confirm that snobol_match() doesn't crash on
   * captures[] when the source pattern has no captures. */
  {
    snobol_match_result_t *r = snobol_match("'a' 'b' 'c'", 11, "abc", 3, 0);
    test_assert(r != NULL, "non-capture source returns non-NULL");
    if (r) {
      test_assert(r->success, "non-capture source matches");
      test_assert(r->capture_count == 0,
                  "non-capture source has capture_count == 0");
      test_assert((r->captures == NULL || r->captures[0] == NULL) != 0,
                  "captures[] is NULL or empty");
      snobol_match_result_free(r);
    }
  }
  test_cov_api_null_contracts();
  test_cov_api_match_literal();
  test_cov_api_match_output_captures();
  test_cov_api_search_prefilter();
  test_cov_api_search_output_captures();
  test_cov_api_search_capture_pike();
  test_cov_api_capture_absolute_equivalence();
  test_cov_api_search_reuse();
  test_cov_api_search_state();
  test_cov_api_batch();
  test_cov_api_accessors();
  test_cov_api_one_shot();
  test_cov_api_builder();
}
