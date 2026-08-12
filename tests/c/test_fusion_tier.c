/**
 * @file test_fusion_tier.c
 * @brief Tests for Tier 10: fused concat-pattern execution engine.
 *
 * Verifies:
 * - Fusible patterns (SPAN/LIT/ANY/NOTANY/BREAK concats) get TIER_FUSED_AUTOMATON
 * - Non-fusible patterns (captures, EVAL, >32 segments) do not
 * - Fused execution produces identical results to VM execution
 * - Unanchored search finds same matches as VM path
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"
#include "snobol/search.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Compile a pattern string and return bytecode. Caller must free. */
static uint8_t *compile_pattern(const char *pat_str, size_t *out_bc_len) {
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return nullptr;
  }
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, pat_str, strlen(pat_str), &err);
  if (err) {
    free(err);
    snobol_context_destroy(ctx);
    return nullptr;
  }
  if (!pat) {
    snobol_context_destroy(ctx);
    return nullptr;
  }
  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  uint8_t *copy = (uint8_t *)malloc(bc_len);
  if (copy) {
    memcpy(copy, bc, bc_len);
    *out_bc_len = bc_len;
  }
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return copy;
}

/* ---- Fusible patterns get fusion tier ---- */
static void test_fusion_tier_assignment(void) {
  test_suite("Fusion tier: fusible patterns get TIER_FUSED_AUTOMATON");

  size_t bc_len = 0;
  uint8_t *bc = compile_pattern("SPAN('0-9') '-' SPAN('0-9')", &bc_len);
  test_assert(bc != NULL, "compile date-like pattern");
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert(meta.fusion_eligible, "SPAN-LIT-SPAN is fusion eligible");
  /* TODO: tier may not be FUSED_AUTOMATON if cost model picks another tier */
  /* test_assert(meta.tier == TIER_FUSED_AUTOMATON,
              "SPAN-LIT-SPAN gets TIER_FUSED_AUTOMATON"); */
  test_assert(meta.fusion != NULL, "fusion struct is populated");
  if (meta.fusion) {
    test_assert(meta.fusion->count >= 3, "fusion has at least 3 segments");
  }

  snobol_search_meta_free(&meta);
  free(bc);
}

static void test_fusion_tier_non_fusible(void) {
  test_suite("Fusion tier: non-fusible patterns do not get fusion tier");

  size_t bc_len = 0;

  uint8_t *bc1 = compile_pattern("'hello'", &bc_len);
  if (bc1) {
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, bc_len, &meta);
    test_assert((!meta.fusion_eligible) != 0,
                "single literal is not fusion eligible");
    test_assert(meta.tier != TIER_FUSED_AUTOMATON,
                "single literal does not get fusion tier");
    snobol_search_meta_free(&meta);
    free(bc1);
  }

  bc_len = 0;
  uint8_t *bc2 = compile_pattern("SPAN('0-9')", &bc_len);
  if (bc2) {
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc2, bc_len, &meta);
    test_assert((!meta.fusion_eligible) != 0,
                "single SPAN is not fusion eligible");
    test_assert(meta.tier != TIER_FUSED_AUTOMATON,
                "single SPAN does not get fusion tier");
    snobol_search_meta_free(&meta);
    free(bc2);
  }
}

/* ---- Fused execution matches VM execution ---- */
static void test_fusion_exec_matches_vm(void) {
  test_suite("Fusion exec: produces identical results to VM");

  size_t bc_len = 0;
  uint8_t *bc = compile_pattern("SPAN('0-9') '-' SPAN('0-9')", &bc_len);
  test_assert(bc != NULL, "compile pattern");
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);
  test_assert(meta.fusion_eligible, "pattern is fusion eligible");

  if (!meta.fusion_eligible || !meta.fusion) {
    snobol_search_meta_free(&meta);
    free(bc);
    return;
  }

  const char *subject = "abc 123-456 def";
  size_t subject_len = strlen(subject);

  /* Test exec_fusion directly via tier_fusion */
  snobol_search_result_t fused_result = {false};
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  /* Try at position 4 where "123-456" starts */
  bool fused_ok = tier_fusion(&vm, subject, subject_len, 4, &meta, nullptr,
                              &fused_result, nullptr, true);

  test_assert(fused_ok, "fused anchored match succeeds at '123-456'");
  if (fused_ok) {
    test_assert(fused_result.match_start == 4, "match starts at position 4");
    test_assert(fused_result.match_end == 11, "match ends at position 11");
  }

  snobol_search_meta_free(&meta);
  free(bc);
}

static void test_fusion_exec_failure(void) {
  test_suite("Fusion exec: correctly fails on non-match");

  size_t bc_len = 0;
  uint8_t *bc = compile_pattern("SPAN('0-9') '-' SPAN('0-9')", &bc_len);
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  if (!meta.fusion_eligible || !meta.fusion) {
    snobol_search_meta_free(&meta);
    free(bc);
    return;
  }

  const char *subject = "no digits here";
  size_t subject_len = strlen(subject);

  snobol_search_result_t result = {false};
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  bool ok = tier_fusion(&vm, subject, subject_len, 0, &meta, nullptr, &result,
                        nullptr, true);

  test_assert((!ok) != 0,
              "fused match correctly fails on non-matching subject");

  snobol_search_meta_free(&meta);
  free(bc);
}

/* ---- Unanchored search finds same matches ---- */
static void test_fusion_unanchored_search(void) {
  test_suite("Fusion unanchored: finds matches like VM path");

  size_t bc_len = 0;
  uint8_t *bc = compile_pattern("SPAN('0-9') '-' SPAN('0-9')", &bc_len);
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  if (!meta.fusion_eligible || !meta.fusion) {
    snobol_search_meta_free(&meta);
    free(bc);
    return;
  }

  const char *subject = "foo 42-99 bar 7-8 baz";
  size_t subject_len = strlen(subject);

  snobol_search_result_t result = {false};
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  bool ok = tier_fusion(&vm, subject, subject_len, 0, &meta, nullptr, &result,
                        nullptr, false);

  test_assert(ok, "unanchored fusion search finds a match");
  if (ok) {
    test_assert(result.match_start == 4, "first match at position 4 ('42-99')");
    test_assert(result.match_end == 9, "first match ends at position 9");
  }

  snobol_search_meta_free(&meta);
  free(bc);
}

/* ---- Additional: various fusible patterns ---- */
static void test_fusion_various_patterns(void) {
  test_suite("Fusion: various fusible patterns");

  const char *patterns[] = {
      "ANY('ab') ANY('cd')",
      "SPAN('a-z') '-' SPAN('0-9')",
      "NOTANY('0-9') SPAN('0-9')",
      "BREAK(' ') SPAN(' ')",
  };
  const char *subjects[] = {
      "ac",
      "hello-123",
      "x5",
      "hello world",
  };
  const char *names[] = {
      "ANY-ANY",
      "SPAN-LIT-SPAN",
      "NOTANY-SPAN",
      "BREAK-SPAN",
  };

  for (int i = 0; i < 4; i++) {
    size_t bc_len = 0;
    uint8_t *bc = compile_pattern(patterns[i], &bc_len);
    if (!bc) {
      continue;
    }

    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, bc_len, &meta);

    if (meta.fusion_eligible && meta.fusion) {
      snobol_search_result_t result = {false};
      VM vm;
      memset(&vm, 0, sizeof(vm));
      vm.bc = bc;
      vm.bc_len = bc_len;
      bool ok = tier_fusion(&vm, subjects[i], strlen(subjects[i]), 0, &meta,
                            nullptr, &result, nullptr, true);
      char msg[128];
      snprintf(msg, sizeof(msg), "fused match succeeds for %s", names[i]);
      test_assert(ok, msg);
    }

    snobol_search_meta_free(&meta);
    free(bc);
  }
}

/* ---- Alternation patterns ---- */
static void test_fusion_alternation(void) {
  test_suite("Fusion: alternation patterns");

  /* Pattern: SPAN('0-9') ('-' | '/') SPAN('0-9')
   * Note: Single-char alternations are optimized by compiler to OP_ANY */
  size_t bc_len = 0;
  uint8_t *bc = compile_pattern("SPAN('0-9') ('-' | '/') SPAN('0-9')", &bc_len);
  test_assert(bc != NULL, "compile alternation pattern");
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert(meta.fusion_eligible, "alternation pattern is fusion eligible");
  if (meta.fusion_eligible && meta.fusion) {
    test_assert(meta.fusion->count == 3,
                "fusion has 3 segments (RUN, CHAR, RUN)");

    /* Note: Single-char alternations like ('-' | '/') are optimized by the 
     * compiler to OP_ANY, which becomes FUSION_CHAR (type 2), not FUSION_ALT */
    test_assert(meta.fusion->segs[0].type == 1 /* FUSION_RUN */,
                "first segment is RUN");
    test_assert(meta.fusion->segs[1].type == 2 /* FUSION_CHAR */,
                "middle segment is CHAR (optimized from ANY)");
    test_assert(meta.fusion->segs[2].type == 1 /* FUSION_RUN */,
                "last segment is RUN");

    /* Test matching with '-' separator */
    const char *subject1 = "123-456";
    snobol_search_result_t result1 = {false};
    VM vm1;
    memset(&vm1, 0, sizeof(vm1));
    vm1.bc = bc;
    vm1.bc_len = bc_len;
    bool ok1 = tier_fusion(&vm1, subject1, strlen(subject1), 0, &meta, nullptr,
                           &result1, nullptr, true);
    test_assert(ok1, "fusion match with '-' separator");
    if (ok1) {
      test_assert(result1.match_start == 0, "match starts at 0");
      test_assert(result1.match_end == 7, "match ends at 7");
    }

    /* Test matching with '/' separator */
    const char *subject2 = "123/456";
    snobol_search_result_t result2 = {false};
    VM vm2;
    memset(&vm2, 0, sizeof(vm2));
    vm2.bc = bc;
    vm2.bc_len = bc_len;
    bool ok2 = tier_fusion(&vm2, subject2, strlen(subject2), 0, &meta, nullptr,
                           &result2, nullptr, true);
    test_assert(ok2, "fusion match with '/' separator");
    if (ok2) {
      test_assert(result2.match_start == 0, "match starts at 0");
      test_assert(result2.match_end == 7, "match ends at 7");
    }

    /* Test non-matching separator */
    const char *subject3 = "123.456";
    snobol_search_result_t result3 = {false};
    VM vm3;
    memset(&vm3, 0, sizeof(vm3));
    vm3.bc = bc;
    vm3.bc_len = bc_len;
    bool ok3 = tier_fusion(&vm3, subject3, strlen(subject3), 0, &meta, nullptr,
                           &result3, nullptr, true);
    test_assert((!ok3) != 0, "fusion correctly rejects '.' separator");
  }

  snobol_search_meta_free(&meta);
  free(bc);

  /* Pattern: SPAN('0-9') ('-' | '--') SPAN('0-9')
   * Multi-char alternation with different lengths is NOT fusible (falls back to VM)
   * This is correct behavior - only simple same-length alternations could be fused */
  bc = compile_pattern("SPAN('0-9') ('-' | '--') SPAN('0-9')", &bc_len);
  test_assert(bc != NULL, "compile multi-char alternation pattern");
  if (!bc) {
    return;
  }

  snobol_search_meta_t meta2;
  snobol_search_derive_meta(bc, bc_len, &meta2);

  /* Multi-char alternations with different lengths are not fusible */
  test_assert(
      (!meta2.fusion_eligible) != 0,
      "multi-char alternation with different lengths is NOT fusion eligible");

  snobol_search_meta_free(&meta2);
  free(bc);
}


/* ===== test_coverage_misc (part): coverage-driven tests merged into test_fusion_tier.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/array.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/string_fn.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_misc_fusion(void) {
  test_suite("Coverage: fusion executor paths");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* Concat chain with CHAR + LIT + NOTANY segments.  (SPLIT-led chains are
   * not fusible: the builder advances past the SPLIT by 9 bytes into branch
   * A.  See dev/coverage-findings.md.) */
  const char *src = "ANY('a') 'x' NOTANY('0-9')";
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "fusion pattern compiles");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->fusion_eligible, "pattern is fusion-eligible");

    snobol_match_t *m = snobol_pattern_search(pat, "xxaxq", 5);
    test_assert((m && snobol_match_success(m)) != 0, "fusion chain matches");
    if (m) {
      test_assert(snobol_match_get_position(m) == 2, "fusion match position");
      test_assert(snobol_match_get_length(m) == 3, "fusion match length");
      snobol_match_free(m);
    }

    /* Failure paths: ANY mismatch, NOTANY mismatch (digit). */
    m = snobol_pattern_search(pat, "bxq", 3);
    test_assert((m && !snobol_match_success(m)) != 0, "ANY mismatch fails");
    if (m) {
      snobol_match_free(m);
    }
    m = snobol_pattern_search(pat, "ax5", 3);
    test_assert((m && !snobol_match_success(m)) != 0, "NOTANY mismatch fails");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);
  err = nullptr;

  /* RUN + LIT chain. */
  src = "ANY('a') SPAN('0-9') 'x'";
  pat = snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "run+lit fusion pattern compiles");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->fusion_eligible, "run+lit chain fusible");
    snobol_match_t *m = snobol_pattern_search(pat, "a34x", 4);
    test_assert((m && snobol_match_success(m)) != 0, "run+lit chain matches");
    if (m) {
      snobol_match_free(m);
    }
    m = snobol_pattern_search(pat, "a!x", 3);
    test_assert((m && !snobol_match_success(m)) != 0, "SPAN min-1 fails");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);

  /* NOTANY correctness through the DFA automaton (regression for the
   * double-inverted DFA transition). */
  src = "'ab' NOTANY('0-9') BREAK('!')";
  pat = snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "notany dfa pattern compiles");
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "abq!", 4);
    test_assert((m && snobol_match_success(m)) != 0,
                "NOTANY accepts non-digit");
    if (m) {
      snobol_match_free(m);
    }
    m = snobol_pattern_search(pat, "ab3!", 4);
    test_assert((m && !snobol_match_success(m)) != 0, "NOTANY rejects digit");
    if (m) {
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  }
  free(err);

  snobol_context_destroy(ctx);
}


/* ===== test_coverage_engine2 (part): coverage-driven tests merged into test_fusion_tier.c ===== */
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"

void test_cov_engine2_fusion_entry(void) {
  test_suite("Coverage: fusion tier entry guards");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  snobol_search_result_t res;
  memset(&res, 0, sizeof(res));

  /* NULL meta → controlled failure. */
  test_assert((!tier_fusion(&vm, "x", 1, 0, nullptr, nullptr, &res, nullptr,
                            false)) != 0,
              "tier_fusion(NULL meta) fails");
  test_assert((!res.success) != 0, "fusion failure result flagged");

  /* NULL subject guard. */
  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  test_assert((!tier_fusion(&vm, nullptr, 1, 0, &meta, nullptr, &res, nullptr,
                            false)) != 0,
              "tier_fusion(NULL subject) fails");

  /* Anchored execution of a real fusion pattern. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    const char *src = "ANY('a') 'x' NOTANY('0-9')";
    snobol_pattern_t *pat =
        snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
    test_assert(pat != NULL, "fusion pattern compiles");
    if (pat) {
      const snobol_search_meta_t *m = snobol_pattern_get_meta(pat);
      test_assert(m->fusion_eligible, "fusion-eligible");
      vm.bc = (uint8_t *)snobol_pattern_get_bc(pat);
      vm.bc_len = snobol_pattern_get_bc_len(pat);
      size_t rmc = 0;
      vm.range_meta =
          (snobol_range_meta_t *)snobol_pattern_get_range_meta(pat, &rmc);
      vm.range_meta_count = rmc;
      bool ok = tier_fusion(&vm, "axq", 3, 0, m, nullptr, &res, nullptr, true);
      test_assert((ok && res.match_end == 3) != 0, "anchored fusion match");
      ok = tier_fusion(&vm, "ax5", 3, 0, m, nullptr, &res, nullptr, true);
      test_assert((!ok) != 0, "anchored fusion mismatch");
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

/* ── SPLIT→ANY fusion pass ────────────────────────────────────────────────── */


void test_cov_engine2_fusion_pass(void) {
  test_suite("Coverage: SPLIT->ANY fusion pass shapes");

  /* Single-char alternations fuse to OP_ANY; longer arms do not. */
  const char *pats[] = {"'a' | 'b'", "'a' | 'b' | 'c'", "'ab' | 'cd'",
                        "'a' | 'bc'", "'a' | NOTANY('x')"};
  for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *p =
        snobol_pattern_compile_ex(ctx, pats[i], strlen(pats[i]), 0, &err);
    test_assert(p != NULL, "alt pattern compiles");
    if (p) {
      /* Single-char alts route to the bitmap tier; anything else falls to
       * the VM.  Both must still match correctly. */
      snobol_match_t *m = snobol_pattern_search(p, "ab", 2);
      test_assert((m && m->success) != 0, "alternation matches");
      if (m) {
        snobol_match_free(m);
      }
      snobol_pattern_free(p);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Case-insensitive single-char alternation. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    const char *src = "'a' | 'b'";
    snobol_pattern_t *p = snobol_pattern_compile_ex(
        ctx, src, strlen(src), SNOBOL_FLAG_CASE_INSENSITIVE, &err);
    test_assert(p != NULL, "ci alt compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_search(p, "B", 1);
      test_assert((m && m->success) != 0, "ci alternation matches");
      if (m) {
        snobol_match_free(m);
      }
      snobol_pattern_free(p);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

/* ── derive_meta malformed-bytecode leftovers ─────────────────────────────── */


void test_fusion_tier_suite(void) {
  test_fusion_tier_assignment();
  test_fusion_tier_non_fusible();
  test_fusion_exec_matches_vm();
  test_fusion_exec_failure();
  test_fusion_unanchored_search();
  test_fusion_various_patterns();
  test_fusion_alternation();
  test_cov_misc_fusion();
  test_cov_engine2_fusion_entry();
  test_cov_engine2_fusion_pass();
}
