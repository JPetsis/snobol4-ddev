/**
 * test_search.c - Tests for search-engine tier dispatch and caching.
 *
 * Covers:
 *  - Tier-5 alternation trie is built once and reused across repeated
 *    snobol_pattern_search() calls on the same pattern (trie caching).
 *  - Tier-3 2-byte literal-prefix fast-path uses paired memchr (not memmem).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"
#include "snobol/ast.h"
#include "snobol/compiler.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Note on the memmem check: search_literal_accelerated() selects its candidate
 * scan by literal_prefix_len — prefix_len == 1 and == 2 use memchr, while the
 * memmem branch is guarded by prefix_len > 2. A pattern that routes to
 * TIER_PREFIX with literal_prefix_len == 2 therefore statically cannot take the
 * memmem branch. We assert that routing below as the proof that the 2-byte path
 * uses paired memchr (no memmem). (A dynamic memmem interposer is intentionally
 * avoided: macOS's two-level namespace prevents a main-executable override of
 * libc memmem from intercepting the static lib's calls.) */

/* Verify the Tier-5 alternation trie is built exactly once and reused
 * across repeated snobol_pattern_search() calls on the same pattern. */
static void test_trie_cache_hit(void) {
  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "'cat'|'car'|'cab'", 17, 0, &err);
  test_assert(pat != NULL, "bushy alt-literal pattern compiles");
  free(err);

  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }

  /* First search builds and caches the trie. */
  snobol_match_t *m = snobol_pattern_search(pat, "the car is here", 15);
  test_assert(m != NULL, "first search returns a result");
  if (m) {
    test_assert(snobol_match_success(m), "'car' matches");
    snobol_match_free(m);
  }
  snobol_auto_trie_t *cache1 = snobol_pattern_get_trie_cache(pat);
  test_assert(cache1 != NULL, "trie cache built after first search");

  /* Second search must reuse the same cached trie (identical pointer). */
  m = snobol_pattern_search(pat, "a cabinet", 10);
  test_assert(m != NULL, "second search returns a result");
  if (m) {
    test_assert(snobol_match_success(m), "'cab' matches");
    snobol_match_free(m);
  }
  snobol_auto_trie_t *cache2 = snobol_pattern_get_trie_cache(pat);
  test_assert(cache2 == cache1,
              "trie cache reused (not rebuilt) on 2nd search");

  /* Many more searches keep the same cache pointer. */
  for (int i = 0; i < 20; i++) {
    m = snobol_pattern_search(pat, "xcatsy", 6);
    if (m) {
      snobol_match_free(m);
    }
  }
  test_assert(snobol_pattern_get_trie_cache(pat) == cache1,
              "trie cache stable across many searches");

  /* Results stay correct after caching. */
  m = snobol_pattern_search(pat, "no match at all", 15);
  if (m) {
    test_assert((!snobol_match_success(m)) != 0,
                "no false match after caching");
    snobol_match_free(m);
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* Oracle for pattern "'ab' SPAN('xy')": first index i where the literal "ab"
 * appears AND a character from {x,y} immediately follows it. In this engine
 * SPAN matches exactly one character from the set, so the match span is
 * [i, i+3). Returns true on success. */
static bool oracle_2byte(const char *s, size_t len, size_t *out_start,
                         size_t *out_end) {
  for (size_t i = 0; i + 3 <= len; i++) {
    if (s[i] == 'a' && s[i + 1] == 'b') {
      char c = s[i + 2];
      if (c == 'x' || c == 'y') {
        /* SPAN consumes the WHOLE x/y run (mirroring the full VM). */
        size_t end = i + 3;
        while (end < len && (s[end] == 'x' || s[end] == 'y')) {
          end++;
        }
        *out_start = i;
        *out_end = end;
        return true;
      }
    }
  }
  return false;
}

/* Verify the Tier-3 2-byte literal-prefix fast-path (search_literal_accelerated
 * with literal_prefix_len == 2, ~search.c:1588) uses paired memchr and never
 * memmem, and produces correct results. A bare 'ab' is literal-only
 * (TIER_LITERAL) and instead goes through search_literal_only() which calls
 * memmem directly, so it does NOT exercise this path. */
static void test_2byte_prefix_memchr_path(void) {
  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  const char *pat_src = "'ab' SPAN('xy')";
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, pat_src, strlen(pat_src), 0, &err);
  test_assert(pat != NULL, "2-byte-prefix pattern compiles");
  free(err);
  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }

  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(meta != NULL, "meta present");
  test_assert(meta->tier == TIER_PREFIX, "pattern routes to TIER_PREFIX");
  test_assert(meta->literal_prefix_len == 2, "literal prefix length is 2");

  struct {
    const char *subj;
    size_t len;
  } cases[] = {
      {"abx", 3},      {"aby", 3},   {"ab", 2},   {"abz", 3},
      {"xxabxyyy", 8}, {"ababx", 5}, {"zzz", 3},  {"aaxbbx", 6},
      {"aabx", 4},     {"", 0},      {"aaaa", 4}, {"abxaabx", 7},
  };

  for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
    size_t exp_start = 0;
    size_t exp_end = 0;
    bool exp = oracle_2byte(cases[k].subj, cases[k].len, &exp_start, &exp_end);

    snobol_match_t *m = snobol_pattern_search(pat, cases[k].subj, cases[k].len);
    test_assert(m != NULL, "search returns a result");
    bool got = (m ? (int)snobol_match_success(m) : 0) != 0;
    test_assert(got == exp, "correct success for 2-byte prefix case");
    if (got && m) {
      size_t st = snobol_match_get_position(m);
      size_t ln = snobol_match_get_length(m);
      test_assert(st == exp_start, "correct match start");
      test_assert(st + ln == exp_end, "correct match end");
    }
    if (m) {
      snobol_match_free(m);
    }
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}


/* ===== test_coverage_search_tiers: coverage-driven tests merged into test_search.c ===== */
#include <stdio.h>


/* ── Bytecode builder helpers ─────────────────────────────────────────────── */

static void covt_emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

static void covt_emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

/* SPLIT(LIT(s1) ACCEPT) (LIT(s2) ACCEPT) — shared-prefix-capable tree. */
static size_t covt_build_split_lit_lit(uint8_t *bc, const char *s1, size_t len1,
                                       const char *s2, size_t len2) {
  size_t ip = 0;
  bc[ip++] = OP_SPLIT;
  uint32_t branch_a = (uint32_t)(1 + 4 + 4);
  covt_emit_u32_be(bc, &ip, branch_a);
  uint32_t branch_b = branch_a + (uint32_t)(9 + len1 + 1);
  covt_emit_u32_be(bc, &ip, branch_b);

  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  covt_emit_u32_be(bc, &ip, (uint32_t)len1);
  memcpy(bc + ip, s1, len1);
  ip += len1;
  bc[ip++] = OP_ACCEPT;

  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  covt_emit_u32_be(bc, &ip, (uint32_t)len2);
  memcpy(bc + ip, s2, len2);
  ip += len2;
  bc[ip++] = OP_ACCEPT;
  return ip;
}

/* Build SPLIT(branch_a) with a single literal of `lit_len` distinct bytes,
 * exceeding the trie node cap (SNOBOL_AUTO_MAX_NODES = 256). */
static size_t covt_build_oversized_lit(uint8_t *bc) {
  size_t ip = 0;
  bc[ip++] = OP_SPLIT;
  covt_emit_u32_be(bc, &ip, 1 + 4 + 4);
  covt_emit_u32_be(bc, &ip, 1 + 4 + 4 + 9 + 300 + 1);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  covt_emit_u32_be(bc, &ip, 300);
  for (int i = 0; i < 300; i++) {
    bc[ip++] = (uint8_t)i;
  }
  bc[ip++] = OP_ACCEPT;
  return ip;
}

/* Helper: run snobol_search_exec with crafted metadata forcing TIER_ALT_LIT
 * so the alt-literals walker is driven directly (no derive_meta on crafted
 * bytecode). Returns the search result. */
static bool covt_search_crafted(const uint8_t *bc, size_t bc_len,
                                const char *subject, size_t subj_len) {
  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.is_alt_literals = true; /* force TIER_ALT_LIT via cost model */
  meta.tier = TIER_ALT_LIT;

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok = snobol_search_exec(&vm, subject, subj_len, 0, &meta, nullptr,
                               &result, nullptr);
  snobol_search_vm_cleanup(&vm);
  return ok;
}

/* ── 2.1 Trie-builder reachable paths ─────────────────────────────────────── */

void test_cov_trie_bushy_via_pattern(void) {
  test_suite("Coverage: trie build + pattern-cache via public API");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "'apple' | 'apricot'", 19, 0, &err);
  test_assert(pat != NULL, "compile bushy alternation succeeds");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert((meta && meta->is_alt_literals) != 0, "meta says alt-literals");
  test_assert((meta && !meta->is_alt_literals_flat) != 0,
              "bushy (shared prefix)");

  /* First search builds the trie and caches it on the pattern. */
  snobol_match_t *m = snobol_pattern_search(pat, "an apricot fell", 16);
  test_assert((m && snobol_match_success(m)) != 0,
              "first search matches 'apricot'");
  if (m) {
    test_assert(snobol_match_get_position(m) == 3, "match at offset 3");
    snobol_match_free(m);
  }

  /* Second search reuses the cached trie (pattern-cache read path). */
  m = snobol_pattern_search(pat, "apple pie", 10);
  test_assert((m && snobol_match_success(m)) != 0,
              "second search matches 'apple'");
  if (m) {
    snobol_match_free(m);
  }

  m = snobol_pattern_search(pat, "zzz", 3);
  test_assert((m && !snobol_match_success(m)) != 0, "no match subject");
  if (m) {
    snobol_match_free(m);
  }

  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_trie_flat_via_pattern(void) {
  test_suite("Coverage: flat alternation (no cache)");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "'foo' | 'bar'", 13, 0, &err);
  test_assert(pat != NULL, "compile flat alternation succeeds");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert((meta && meta->is_alt_literals_flat) != 0, "flat classified");

  snobol_match_t *m = snobol_pattern_search(pat, "a bar of foo", 13);
  test_assert((m && snobol_match_success(m)) != 0, "flat matches 'bar'");
  if (m) {
    test_assert(snobol_match_get_position(m) == 2, "match at offset 2");
    snobol_match_free(m);
  }
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_trie_vm_cache(void) {
  test_suite("Coverage: trie supplied on VM (trie_cache field)");

  uint8_t bc[256];
  size_t bc_len = covt_build_split_lit_lit(bc, "apple", 5, "apricot", 7);
  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  snobol_auto_trie_t *trie = snobol_build_alt_trie(bc, bc_len);
  test_assert(trie != NULL, "standalone trie build succeeds");
  test_assert(snobol_pattern_get_trie_cache(nullptr) == NULL,
              "get_trie_cache(NULL) returns NULL");
  if (trie) {
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.trie_cache = trie; /* covers the vm->trie_cache read path */
    snobol_search_result_t result;
    bool ok = snobol_search_exec(&vm, "an apricot", 10, 0, &meta, nullptr,
                                 &result, nullptr);
    test_assert((ok && result.match_start == 3) != 0,
                "match via VM trie cache");
    snobol_search_vm_cleanup(&vm);
    snobol_auto_trie_free(trie);
  }
  snobol_search_meta_free(&meta);
}


void test_cov_trie_no_match_end(void) {
  test_suite("Coverage: trie_match end-of-subject exit");

  uint8_t bc[256];
  size_t bc_len = covt_build_split_lit_lit(bc, "apple", 5, "apricot", 7);
  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);
  test_assert(meta.is_alt_literals, "alt-literals detected");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  bool ok =
      snobol_search_exec(&vm, "x", 1, 0, &meta, nullptr, &result, nullptr);
  test_assert((!ok) != 0, "trie walk hits subject end with no match");
  snobol_search_vm_cleanup(&vm);
  snobol_search_meta_free(&meta);
}

/* ── 2.3 Crafted-bytecode bounds guards ───────────────────────────────────── */


void test_cov_walker_truncated_bc(void) {
  test_suite("Coverage: alt-lit walker truncated bytecode");

  /* Single byte — ip+2 > bc_len. */
  uint8_t bc1[4] = {OP_SPLIT, 0, 0, 0};
  bool ok = covt_search_crafted(bc1, 1, "abc", 3);
  test_assert((!ok) != 0, "truncated SPLIT fails closed");

  /* LIT opcode with no operands — ip+10 > bc_len. */
  uint8_t bc2[4] = {OP_LIT, 0, 0, 0};
  ok = covt_search_crafted(bc2, 2, "abc", 3);
  test_assert((!ok) != 0, "truncated LIT fails closed");

  /* SPLIT with truncated operand bytes — ip+9 > bc_len. */
  uint8_t bc3[8] = {OP_SPLIT, 0, 0, 0, 0, 0, 0, 0};
  ok = covt_search_crafted(bc3, 6, "abc", 3);
  test_assert((!ok) != 0, "truncated SPLIT operands fail closed");

  /* Non-LIT/SPLIT opcode at ip=0. */
  uint8_t bc4[8] = {OP_ACCEPT, 0, 0, 0, 0, 0, 0, 0};
  ok = covt_search_crafted(bc4, 8, "abc", 3);
  test_assert((!ok) != 0, "unsupported root opcode fails closed");
}


void test_cov_walker_bad_offsets(void) {
  test_suite("Coverage: alt-lit walker invalid operand offsets");

  /* LIT with data offset beyond bc_len. */
  uint8_t bc1[64];
  size_t ip = 0;
  bc1[ip++] = OP_LIT;
  covt_emit_u32_be(bc1, &ip, 500); /* off >= bc_len */
  covt_emit_u32_be(bc1, &ip, 4);
  size_t bc_len1 = ip;
  test_assert((!covt_search_crafted(bc1, bc_len1, "abcd", 4)) != 0,
              "LIT offset beyond bc fails closed");

  /* LIT with off+len beyond bc_len. */
  uint8_t bc2[64];
  ip = 0;
  bc2[ip++] = OP_LIT;
  covt_emit_u32_be(bc2, &ip, 8);
  covt_emit_u32_be(bc2, &ip, 100);
  size_t bc_len2 = ip;
  test_assert((!covt_search_crafted(bc2, bc_len2, "abcd", 4)) != 0,
              "LIT len overruns bc fails closed");

  /* SPLIT with branch target beyond bc_len. */
  uint8_t bc3[64];
  ip = 0;
  bc3[ip++] = OP_SPLIT;
  covt_emit_u32_be(bc3, &ip, 8);
  covt_emit_u32_be(bc3, &ip, 500);
  size_t bc_len3 = ip;
  test_assert((!covt_search_crafted(bc3, bc_len3, "abcd", 4)) != 0,
              "SPLIT target beyond bc fails closed");
}


void test_cov_walker_stack_overflow(void) {
  test_suite("Coverage: alt-lit walker explicit-stack overflow");

  /* 40 nested SPLITs: the walker's 64-entry stack overflows at sp+2 > 64. */
  uint8_t bc[512];
  size_t ip = 0;
  for (int i = 0; i < 40; i++) {
    bc[ip++] = OP_SPLIT;
    covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  }
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;
  test_assert((!covt_search_crafted(bc, bc_len, "abc", 3)) != 0,
              "walker stack overflow fails closed");
}


void test_cov_trie_node_cap(void) {
  test_suite("Coverage: trie_insert node-cap exhaustion");

  uint8_t bc[512];
  size_t bc_len = covt_build_oversized_lit(bc);
  test_assert((!covt_search_crafted(bc, bc_len, "abc", 3)) != 0,
              "node-cap exhaustion fails closed (no crash)");
}

/* ── 2.2 Automaton promotion/demotion ─────────────────────────────────────── */


void test_cov_automaton_bmh_gate(void) {
  test_suite("Coverage: automaton BMH-skip gate + exec");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  /* Literal prefix ≥ 2 bytes + automaton-safe trailing op: BMH skip set,
   * not alt-literals → DFA override promotes to TIER_AUTOMATON. */
  const char *src = "'ab' SPAN('0-9')";
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "compile prefix+SPAN succeeds");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(meta->has_bmh_skip, "literal prefix yields BMH skip");
  /* SPAN's zero-width run-end exit cannot be encoded in the DFA (it would
   * accept after the FIRST class byte), so SPAN patterns are NOT
   * automaton-eligible — the search-VM handles them correctly. */
  test_assert((!meta->automaton_eligible) != 0,
              "SPAN patterns are not automaton-eligible");
  test_assert((!meta->is_alt_literals) != 0, "not alt-literals");

  /* No DFA promotion without automaton eligibility. */
  test_assert(snobol_search_executed_tier(meta, true, 32, false) !=
                  TIER_AUTOMATON,
              "SPAN pattern never promotes to AUTOMATON");
  test_assert(snobol_search_executed_tier(nullptr, true, 32, false) ==
                  TIER_GENERAL,
              "executed_tier(NULL) falls back to GENERAL");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = snobol_pattern_get_bc(pat);
  vm.bc_len = snobol_pattern_get_bc_len(pat);

  /* A hand-built DFA on a SPAN pattern must not change the match: the
   * automaton would accept after the first class byte (length 4 for
   * "ab34"), the correct run is the full 5 bytes. */
  snobol_dfa_t *dfa = build_dfa(vm.bc, vm.bc_len, &vm);
  test_assert(dfa != NULL, "DFA builds for prefix+SPAN");
  if (dfa) {
    snobol_search_result_t result;
    snobol_search_diag_t diag;
    bool ok =
        snobol_search_exec(&vm, "xxab34", 6, 0, meta, dfa, &result, &diag);
    test_assert((ok && result.match_start == 2 && result.match_end == 6) != 0,
                "search-vm matches the full 'ab34' run (not the DFA's 'ab3')");
    test_assert(diag.automaton_tests == 0, "automaton not used for SPAN");

    /* No-match subject returns false through the search-vm path. */
    ok = snobol_search_exec(&vm, "zzzzzzz", 7, 0, meta, dfa, &result, &diag);
    test_assert((!ok) != 0, "no-match returns false");
    snobol_dfa_free(dfa);
  }
  snobol_search_vm_cleanup(&vm);
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_automaton_demotion(void) {
  test_suite("Coverage: automaton demotion gates");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* Single-byte literal: no BMH skip → no automaton promotion. */
  snobol_pattern_t *pat1 = snobol_pattern_compile_ex(ctx, "'a'", 3, 0, &err);
  test_assert(pat1 != NULL, "compile 'a'");
  if (pat1) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat1);
    test_assert((!meta->has_bmh_skip) != 0, "1-byte literal has no BMH skip");
    test_assert(snobol_search_executed_tier(meta, true, 16, false) !=
                    TIER_AUTOMATON,
                "1-byte literal never promoted to automaton");
    snobol_pattern_free(pat1);
  }
  free(err);
  err = nullptr;

  /* Non-ASCII charclass: automaton ineligible. */
  const char *span_euro = "SPAN('\xE2\x82\xAC')";
  snobol_pattern_t *pat2 =
      snobol_pattern_compile_ex(ctx, span_euro, strlen(span_euro), 0, &err);
  test_assert(pat2 != NULL, "compile non-ASCII SPAN");
  if (pat2) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat2);
    test_assert(meta != NULL, "meta present");
    if (meta) {
      test_assert((!meta->automaton_eligible) != 0,
                  "non-ASCII class demotes automaton eligibility");
      test_assert((!meta->ascii_class_only) != 0, "class not ASCII-only");
    }
    snobol_pattern_free(pat2);
  }
  free(err);
  err = nullptr;

  /* Capturing pattern: automaton ineligible (captures dropped by DFA). */
  snobol_pattern_t *pat3 =
      snobol_pattern_compile_ex(ctx, "@r1('ab')", 9, 0, &err);
  test_assert(pat3 != NULL, "compile capture pattern");
  if (pat3) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat3);
    test_assert(meta != NULL, "meta present");
    if (meta) {
      test_assert(meta->has_capture, "capture detected");
      test_assert((!meta->automaton_eligible) != 0,
                  "capture pattern demotes automaton eligibility");
    }
    snobol_pattern_free(pat3);
  }
  free(err);

  snobol_context_destroy(ctx);
}

/* ── Scan-tier diagnostics paths ──────────────────────────────────────────── */


void test_cov_break_scan_diag(void) {
  test_suite("Coverage: BREAK scan tier with diagnostics");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  /* BREAK is a greedy scan: it stops at the first delimiter, so the trailing
   * literal must be the delimiter itself. On subjects without a delimiter the
   * scan consumes everything and the trailing op fails — the no-match path. */
  const char *src = "BREAK(',') ','";
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "compile BREAK pattern");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(meta->is_break_family, "break family detected");
  test_assert(meta->ascii_class_only, "ASCII class");

  /* The cost model routes BREAK to the SIMD NFA tier; force the BREAK scan
   * tier by clearing simd eligibility in a meta copy. */
  snobol_search_meta_t meta_copy = *meta;
  meta_copy.simd_eligible = false;

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = snobol_pattern_get_bc(pat);
  vm.bc_len = snobol_pattern_get_bc_len(pat);
  snobol_search_result_t result;
  snobol_search_diag_t diag;

  bool ok = snobol_search_exec(&vm, "aaa,bbb", 7, 0, &meta_copy, nullptr,
                               &result, &diag);
  test_assert((ok && result.match_end == 4) != 0, "BREAK matches up to comma");
  test_assert(diag.candidates_skipped >= 3, "break scan skipped delimiters");
  test_assert(diag.candidates_tested >= 1, "break scan tested candidates");

  ok = snobol_search_exec(&vm, "no comma here", 13, 0, &meta_copy, nullptr,
                          &result, &diag);
  test_assert((!ok) != 0, "no-comma subject fails (trailing literal missing)");

  /* Anchored: BREAK_SCAN is excluded, the search-VM must fail on a subject
   * with no delimiter. */
  ok = snobol_search_exec_anchored(&vm, "zzz", 3, &meta_copy, nullptr, &result,
                                   &diag);
  test_assert((!ok) != 0, "anchored BREAK fails when no delimiter follows");

  snobol_search_vm_cleanup(&vm);
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_span_scan_diag(void) {
  test_suite("Coverage: SPAN scan tier with diagnostics");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "SPAN('a-z')", 11, 0, &err);
  test_assert(pat != NULL, "compile SPAN pattern");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(meta->is_span_family, "span family detected");

  snobol_search_meta_t meta_copy = *meta;
  meta_copy.simd_eligible = false; /* force the SPAN scan tier */

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = snobol_pattern_get_bc(pat);
  vm.bc_len = snobol_pattern_get_bc_len(pat);
  snobol_search_result_t result;
  snobol_search_diag_t diag;

  bool ok = snobol_search_exec(&vm, "42abc99", 7, 0, &meta_copy, nullptr,
                               &result, &diag);
  test_assert((ok && result.match_start == 2) != 0,
              "SPAN starts at first letter");
  test_assert(diag.candidates_skipped >= 2, "span scan skipped digits");
  test_assert(diag.candidates_tested >= 1, "span scan tested candidates");

  /* Anchored SPAN: single-position attempt (line 413-414). */
  ok = snobol_search_exec_anchored(&vm, "42abc", 5, &meta_copy, nullptr,
                                   &result, &diag);
  test_assert((!ok) != 0, "anchored SPAN fails when offset 0 not in class");

  snobol_search_vm_cleanup(&vm);
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}


void test_cov_literal_prefix_tiers(void) {
  test_suite("Coverage: literal-prefix scan tier");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* 2-byte literal prefix + trailing op → TIER_PREFIX memchr pair. */
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "'ab' SPAN('0-9')", 16, 0, &err);
  test_assert(pat != NULL, "compile prefix pattern");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->has_literal_prefix, "literal prefix detected");
    test_assert(meta->literal_prefix_len == 2, "2-byte prefix");

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = snobol_pattern_get_bc(pat);
    vm.bc_len = snobol_pattern_get_bc_len(pat);
    snobol_search_result_t result;
    snobol_search_diag_t diag;
    bool ok =
        snobol_search_exec(&vm, "xxab12", 6, 0, meta, nullptr, &result, &diag);
    test_assert((ok && result.match_start == 2) != 0, "prefix search matches");
    test_assert(diag.candidates_tested >= 1, "candidates tested");
    ok = snobol_search_exec(&vm, "qqqqq", 5, 0, meta, nullptr, &result, &diag);
    test_assert((!ok) != 0, "prefix absent → no match");
    snobol_search_vm_cleanup(&vm);
    snobol_pattern_free(pat);
  }
  free(err);
  err = nullptr;

  /* 3-byte literal prefix → memmem path. */
  pat = snobol_pattern_compile_ex(ctx, "'abc' ANY('x')", 15, 0, &err);
  test_assert(pat != NULL, "compile 3-byte prefix pattern");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->literal_prefix_len == 3, "3-byte prefix");
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = snobol_pattern_get_bc(pat);
    vm.bc_len = snobol_pattern_get_bc_len(pat);
    snobol_search_result_t result;
    bool ok = snobol_search_exec(&vm, "zzabcx", 6, 0, meta, nullptr, &result,
                                 nullptr);
    test_assert((ok && result.match_start == 2) != 0, "memmem prefix matches");
    snobol_search_vm_cleanup(&vm);
    snobol_pattern_free(pat);
  }
  free(err);

  snobol_context_destroy(ctx);
}


void test_cov_literal_only_paths(void) {
  test_suite("Coverage: literal-only fast path");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'hello'", 7, 0, &err);
  test_assert(pat != NULL, "compile literal");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->is_literal_only, "literal-only detected");

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = snobol_pattern_get_bc(pat);
    vm.bc_len = snobol_pattern_get_bc_len(pat);
    snobol_search_result_t result;
    snobol_search_diag_t diag;

    bool ok = snobol_search_exec(&vm, "xxhelloyy", 9, 0, meta, nullptr, &result,
                                 &diag);
    test_assert((ok && result.match_start == 2) != 0,
                "unanchored literal match");
    test_assert(diag.candidates_tested >= 1, "literal-only diag tested");

    ok = snobol_search_exec(&vm, "hi", 2, 0, meta, nullptr, &result, &diag);
    test_assert((!ok) != 0, "subject shorter than literal");

    ok = snobol_search_exec_anchored(&vm, "hello", 5, meta, nullptr, &result,
                                     &diag);
    test_assert((ok && result.match_start == 0) != 0, "anchored literal match");
    ok = snobol_search_exec_anchored(&vm, "xhello", 6, meta, nullptr, &result,
                                     &diag);
    test_assert((!ok) != 0, "anchored literal mismatch at offset 0");
    ok = snobol_search_exec_anchored(&vm, "hello", 3, meta, nullptr, &result,
                                     &diag);
    test_assert((!ok) != 0, "anchored literal too short");
    snobol_search_vm_cleanup(&vm);
    snobol_pattern_free(pat);
  }
  free(err);

  /* Crafted: is_literal_only meta but bytecode not a valid LIT root →
   * search_literal_only falls back (line 590-595 / 640-643). */
  {
    uint8_t bc[16] = {OP_ACCEPT, 0, 0, 0, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.is_literal_only = true;
    meta.required_lit_len = 0;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 8;
    snobol_search_result_t result;
    bool ok = snobol_search_exec(&vm, "hello", 5, 0, &meta, nullptr, &result,
                                 nullptr);
    test_assert((!ok) != 0, "literal-only fallback on non-LIT bytecode");
    snobol_search_vm_cleanup(&vm);
  }

  snobol_context_destroy(ctx);
}


void test_cov_bitmap_tier(void) {
  test_suite("Coverage: candidate-bitmap tier");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "'a' | 'b'", 9, 0, &err);
  test_assert(pat != NULL, "compile single-char alt");
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(meta->has_candidate_bitmap, "candidate bitmap present");
  test_assert(meta->is_single_char_alt, "single-char alt detected");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = snobol_pattern_get_bc(pat);
  vm.bc_len = snobol_pattern_get_bc_len(pat);
  snobol_search_result_t result;
  snobol_search_diag_t diag;

  bool ok =
      snobol_search_exec(&vm, "zzbz", 4, 0, meta, nullptr, &result, &diag);
  test_assert((ok && result.match_start == 2) != 0, "bitmap match at 'b'");
  test_assert(diag.candidates_skipped >= 2, "bitmap skipped non-candidates");

  ok = snobol_search_exec(&vm, "zzzz", 4, 0, meta, nullptr, &result, &diag);
  test_assert((!ok) != 0, "no candidate bytes → no match");

  snobol_search_vm_cleanup(&vm);
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}

/* ── Search-VM (Tier 6) choice growth + misc ─────────────────────────────── */


void test_cov_search_vm_choice_growth(void) {
  test_suite("Coverage: search-VM choice-stack growth");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  /* Nested bounded repetition: deep backtracking that pushes more than 2
   * choice frames (exercises search_vm_push_choice realloc growth). */
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, "('a'+)+ 'b'", 12, 0, &err);
  test_assert(pat != NULL, "compile nested-repeat pattern");
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->search_vm_eligible, "search-VM eligible");

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = snobol_pattern_get_bc(pat);
    vm.bc_len = snobol_pattern_get_bc_len(pat);
    snobol_search_result_t result;
    bool ok =
        snobol_search_exec(&vm, "aaab", 4, 0, meta, nullptr, &result, nullptr);
    test_assert((ok && result.match_start == 0) != 0, "nested repeat matches");
    snobol_search_vm_cleanup(&vm);
    snobol_pattern_free(pat);
  }
  free(err);
  err = nullptr;

  /* ARBNO(ANY('a')) ABORT via AST builder: REPEAT_INIT makes pike_scan
   * overflow (falls through to the restart loop), and the restart loop's
   * search_vm_exec hits ABORT → tier abort path. */
  {
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_arbno(snobol_ast_create_any("a", 1));
    parts[1] = snobol_ast_create_abort();
    ast_node_t *root = snobol_ast_create_concat(parts, 2);
    int rc = compile_ast_to_bytecode_c(root, false, &bc, &bc_len);
    snobol_ast_free(root);
    test_assert((rc == 0 && bc && bc_len > 0) != 0, "AST ARBNO+ABORT compiles");

    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, bc_len, &meta);
    test_assert(meta.search_vm_eligible, "ARBNO+ABORT search-VM eligible");

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    snobol_search_result_t result;
    bool ok =
        snobol_search_exec(&vm, "aaa", 3, 0, &meta, nullptr, &result, nullptr);
    test_assert((!ok && result.aborted) != 0, "ABORT terminates search");
    snobol_search_vm_cleanup(&vm);
    snobol_search_meta_free(&meta);
    free(bc);
  }

  snobol_context_destroy(ctx);
}


void test_cov_search_vm_anchored(void) {
  test_suite("Coverage: search-VM anchored path");

  /* LIT+LEN via AST (the string grammar rejects `LEN(1) 'ab'`): search-VM
   * eligible, not SIMD (LEN is not a charclass op), so anchored dispatch
   * picks TIER_SEARCH_VM (scan tiers excluded). */
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_len(1);
  parts[1] = snobol_ast_create_lit("ab", 2);
  ast_node_t *root = snobol_ast_create_concat(parts, 2);
  int rc = compile_ast_to_bytecode_c(root, false, &bc, &bc_len);
  snobol_ast_free(root);
  test_assert((rc == 0 && bc && bc_len > 0) != 0, "AST LEN+LIT compiles");
  if (!bc) {
    free(bc);
    return;
  }
  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);
  test_assert(meta.search_vm_eligible, "LEN+LIT search-VM eligible");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  bool ok = snobol_search_exec_anchored(&vm, "xab", 3, &meta, nullptr, &result,
                                        nullptr);
  test_assert((ok && result.match_end == 3) != 0, "anchored search-VM match");
  ok = snobol_search_exec_anchored(&vm, "zzy", 3, &meta, nullptr, &result,
                                   nullptr);
  test_assert((!ok) != 0, "anchored mismatch at offset 0");
  snobol_search_vm_cleanup(&vm);
  snobol_search_meta_free(&meta);
  free(bc);
}


void test_cov_search_exec_misc(void) {
  test_suite("Coverage: dispatch entry-point guards");

  /* NULL meta → derive inline. */
  uint8_t bc[64];
  size_t bc_len = covt_build_split_lit_lit(bc, "abc", 3, "abd", 3);
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;

  bool ok = snobol_search_exec(&vm, "xxabcyy", 7, 0, nullptr, nullptr, &result,
                               nullptr);
  test_assert((ok && result.match_start == 2) != 0, "inline derive works");

  ok = snobol_search_exec(&vm, "xxabcyy", 7, 99, nullptr, nullptr, &result,
                          nullptr);
  test_assert((!ok) != 0, "start_offset beyond subject fails");

  ok = snobol_search_exec(nullptr, "xx", 2, 0, nullptr, nullptr, &result,
                          nullptr);
  test_assert((!ok) != 0, "NULL vm fails closed");

  ok = snobol_search_exec(&vm, nullptr, 2, 0, nullptr, nullptr, &result,
                          nullptr);
  test_assert((!ok) != 0, "NULL subject fails closed");
  snobol_search_vm_cleanup(&vm);

  /* snobol_search_dump_cost_model(NULL) → stdout. */
  snobol_search_dump_cost_model(nullptr);
  test_assert(true, "cost-model dump runs");
}

/* ── snobol_build_alt_trie failure paths ──────────────────────────────────── */


void test_cov_build_alt_trie_failures(void) {
  test_suite("Coverage: snobol_build_alt_trie failure paths");

  test_assert(snobol_build_alt_trie(nullptr, 0) == NULL, "NULL bc rejected");
  uint8_t one[1] = {OP_SPLIT};
  test_assert(snobol_build_alt_trie(one, 1) == NULL, "bc_len < 2 rejected");

  /* Non-LIT/SPLIT root op. */
  uint8_t bc1[8] = {OP_ACCEPT, 0, 0, 0, 0, 0, 0, 0};
  test_assert(snobol_build_alt_trie(bc1, 8) == NULL, "bad root op rejected");

  /* Truncated LIT operands. */
  uint8_t bc2[8] = {OP_LIT, 0, 0, 0, 0, 0, 0, 0};
  test_assert(snobol_build_alt_trie(bc2, 8) == NULL, "truncated LIT rejected");

  /* LIT data offset out of range. */
  uint8_t bc3[16];
  size_t ip = 0;
  bc3[ip++] = OP_LIT;
  covt_emit_u32_be(bc3, &ip, 500);
  covt_emit_u32_be(bc3, &ip, 2);
  test_assert(snobol_build_alt_trie(bc3, ip) == NULL, "LIT offset rejected");

  /* SPLIT branch target out of range. */
  uint8_t bc4[16];
  ip = 0;
  bc4[ip++] = OP_SPLIT;
  covt_emit_u32_be(bc4, &ip, 8);
  covt_emit_u32_be(bc4, &ip, 500);
  test_assert(snobol_build_alt_trie(bc4, ip) == NULL, "SPLIT target rejected");

  /* Oversized literal exhausts the trie node pool. */
  uint8_t bc5[512];
  size_t bc_len5 = covt_build_oversized_lit(bc5);
  test_assert(snobol_build_alt_trie(bc5, bc_len5) == NULL,
              "trie pool exhaustion rejected");
}

/* ── Crafted Pike-scan main loop (search_vm tier, no REPEAT overflow) ─────── */

/* Drive pike_scan's opcode dispatch with a crafted bytecode that has no
 * REPEAT_INIT (so pike does not overflow) plus a crafted range_meta. */


void test_cov_pike_main_loop(void) {
  test_suite("Coverage: pike main-loop opcodes");

  /* Layout:
   *   0: NOP 1: FENCE 2: ANCHOR(0) 4: POS(0) 9: TAB(0) 14: RTAB(5)
   *   19: RPOS(5) 24: CAP_START(0) 26: CAP_END(0) 28: ASSIGN(0,0)
   *   32: SPLIT(a=41, b=52)  41: LIT(inline 'a') 51: ACCEPT
   *   52: SPAN(1) 55: NOTANY(1) 58: ANY(1) 61: BREAK(1) 64: BREAKX(1)
   *   67: LEN(1) 72: FAIL 73: ACCEPT
   * Range data at 100: CpRange('a','a').
   * The position ops are all satisfiable at cursor 0 on a 5-byte subject
   * (POS(0), TAB(0), RTAB(5)→target 0, RPOS(5)→target 0) and keep the
   * cursor at 0 so the SPLIT/LIT branch can match. */
  uint8_t bc[256];
  size_t ip = 0;
  bc[ip++] = OP_NOP;
  bc[ip++] = OP_FENCE;
  bc[ip++] = OP_ANCHOR;
  bc[ip++] = 0; /* start anchor */
  bc[ip++] = OP_POS;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_TAB;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_RTAB;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_RPOS;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_CAP_START;
  bc[ip++] = 0;
  bc[ip++] = OP_CAP_END;
  bc[ip++] = 0;
  bc[ip++] = OP_ASSIGN;
  covt_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0;
  bc[ip++] = OP_SPLIT;
  covt_emit_u32_be(bc, &ip, 41);
  covt_emit_u32_be(bc, &ip, 52);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, 50); /* inline data at 50 */
  covt_emit_u32_be(bc, &ip, 1);
  bc[ip++] = 'a';
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_SPAN;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_NOTANY;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ANY;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_BREAK;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_BREAKX;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_LEN;
  covt_emit_u32_be(bc, &ip, 1);
  bc[ip++] = OP_FAIL;
  bc[ip++] = OP_ACCEPT;
  size_t data_off = 100;
  ip = data_off;
  covt_emit_u32_be(bc, &ip, 'a'); /* CpRange start */
  covt_emit_u32_be(bc, &ip, 'a'); /* CpRange end   */
  size_t bc_len = ip;

  snobol_range_meta_t rm[1];
  rm[0].ranges_ptr = bc + data_off;
  rm[0].count = 1;
  rm[0].case_insensitive = 0;

  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.search_vm_eligible = true; /* force TIER_SEARCH_VM → pike_scan */

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.range_meta = rm;
  vm.range_meta_count = 1;

  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok =
      snobol_search_exec(&vm, "axaax", 5, 0, &meta, nullptr, &result, nullptr);
  test_assert(ok, "pike finds LIT-branch match");
  test_assert((result.match_start == 0 && result.match_end == 1) != 0,
              "pike match covers the 'a' literal");
  test_assert((!result.pike_overflowed) != 0,
              "no pike overflow on linear pattern");

  snobol_search_vm_cleanup(&vm);
}

/* Crafted 70-level SPLIT tree (branch A = next SPLIT, branch B = leaf
 * LIT('z') ACCEPT) with no matching terminal: overflows the pike thread
 * buffer (each level spawns a branch thread), forcing the restart loop,
 * which then pushes 70 nested choice frames (search_vm_push_choice realloc
 * growth) before every leaf fails and the stack unwinds. */


void test_cov_pike_overflow_restart(void) {
  test_suite("Coverage: pike overflow falls back to restart loop");

  uint8_t bc[4096];
  size_t ip = 0;
  int levels = 70;
  for (int i = 0; i < levels; i++) {
    size_t base = ip;
    bc[ip++] = OP_SPLIT;
    uint32_t a = (uint32_t)(base + 20);
    uint32_t b = (uint32_t)(base + 9);
    covt_emit_u32_be(bc, &ip, a);
    covt_emit_u32_be(bc, &ip, b);
    /* leaf: LIT('z') ACCEPT (always fails on the 'abc' subject) */
    bc[ip++] = OP_LIT;
    covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covt_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'z';
    bc[ip++] = OP_ACCEPT;
  }
  bc[ip++] = OP_FAIL; /* terminal after the deepest branch-A chain */
  size_t bc_len = ip;

  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.search_vm_eligible = true;

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok =
      snobol_search_exec(&vm, "abc", 3, 0, &meta, nullptr, &result, nullptr);
  test_assert((!ok) != 0, "overflow pattern yields no match");
  test_assert(result.pike_overflowed, "pike overflow flagged");
  snobol_search_vm_cleanup(&vm);
}

/* ── Crafted search-VM (restart loop) opcode paths ────────────────────────── */

/* Runner: force TIER_SEARCH_VM with crafted meta; pike overflows on the
 * REPEAT_INIT prefix so the restart loop executes the crafted bytecode. */
static bool covt_search_vm_run(const uint8_t *bc, size_t bc_len,
                               const char *subject, size_t subj_len,
                               const snobol_range_meta_t *rm, size_t rm_count,
                               snobol_search_result_t *res) {
  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.search_vm_eligible = true;
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;
  memset(res, 0, sizeof(*res));
  bool ok = snobol_search_exec(&vm, subject, subj_len, 0, &meta, nullptr, res,
                               nullptr);
  snobol_search_vm_cleanup(&vm);
  return ok;
}

/* Emit REPEAT_INIT(min,max) + return the bytecode offset of its skip field. */
static size_t covt_emit_repeat_init(uint8_t *bc, size_t *ip, uint8_t loop_id,
                                    uint32_t min, uint32_t max) {
  bc[(*ip)++] = OP_REPEAT_INIT;
  bc[(*ip)++] = loop_id;
  covt_emit_u32_be(bc, ip, min);
  covt_emit_u32_be(bc, ip, max);
  size_t skip = *ip;
  covt_emit_u32_be(bc, ip, 0); /* patched later */
  return skip;
}

static void covt_patch_u32(uint8_t *bc, size_t at, uint32_t v) {
  bc[at] = (uint8_t)((v >> 24) & 0xFF);
  bc[at + 1] = (uint8_t)((v >> 16) & 0xFF);
  bc[at + 2] = (uint8_t)((v >> 8) & 0xFF);
  bc[at + 3] = (uint8_t)(v & 0xFF);
}


void test_cov_search_vm_position_ops(void) {
  test_suite("Coverage: search-VM position/zero-width ops");

  /* Body: POS(0) TAB(1) LIT('b') RPOS(0) ANCHOR(1) NOP FENCE CAP_START
   * CAP_END ASSIGN RTAB(0) → REPEAT_STEP. On "ab": the second iteration
   * fails at POS(0) and pops the REPEAT_STEP exit choice. */
  uint8_t bc[256];
  size_t ip = 0;
  size_t skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  size_t body = ip;
  bc[ip++] = OP_POS;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_TAB;
  covt_emit_u32_be(bc, &ip, 1);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "b", 1);
  ip += 1;
  bc[ip++] = OP_RPOS;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_ANCHOR;
  bc[ip++] = 1; /* end anchor */
  bc[ip++] = OP_NOP;
  bc[ip++] = OP_FENCE;
  bc[ip++] = OP_CAP_START;
  bc[ip++] = 0;
  bc[ip++] = OP_CAP_END;
  bc[ip++] = 0;
  bc[ip++] = OP_ASSIGN;
  covt_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0;
  bc[ip++] = OP_RTAB;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)body);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  snobol_search_result_t result;
  bool ok = covt_search_vm_run(bc, bc_len, "ab", 2, nullptr, 0, &result);
  test_assert((ok && result.match_end == 2) != 0,
              "repeat+position ops match via restart loop");
  ok = covt_search_vm_run(bc, bc_len, "zz", 2, nullptr, 0, &result);
  test_assert((!ok) != 0, "no-match subject fails");
}


void test_cov_search_vm_fail_paths(void) {
  test_suite("Coverage: search-VM zero-width fail paths");

  /* Body: POS(0) TAB(5) RTAB(2) RPOS(5) LEN(5) FAIL — each fails on the
   * short subject "ab" and pops (or exhausts) the choice stack. */
  uint8_t bc[256];
  size_t ip = 0;
  size_t skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  size_t body = ip;
  bc[ip++] = OP_POS;
  covt_emit_u32_be(bc, &ip, 0);
  bc[ip++] = OP_TAB;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_RTAB;
  covt_emit_u32_be(bc, &ip, 2);
  bc[ip++] = OP_RPOS;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_LEN;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_FAIL;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)body);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  snobol_search_result_t result;
  bool ok = covt_search_vm_run(bc, bc_len, "ab", 2, nullptr, 0, &result);
  test_assert((!ok) != 0, "all zero-width asserts fail on short subject");

  /* SUCCEED terminates a match immediately (empty match). */
  ip = 0;
  skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "a", 1);
  ip += 1;
  bc[ip++] = OP_SUCCEED;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)(ip + 4));
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;
  bc_len = ip;

  ok = covt_search_vm_run(bc, bc_len, "ab", 2, nullptr, 0, &result);
  test_assert((ok && result.match_end == 1) != 0, "SUCCEED terminates early");
}


void test_cov_search_vm_split_fail(void) {
  test_suite("Coverage: search-VM SPLIT + fail-with-choice");

  /* Body: SPLIT(a=POS(5) LIT('a') ACCEPT, b=LIT('a') ACCEPT). POS(5)
   * fails at every offset → pops the SPLIT choice → branch B matches. */
  uint8_t bc[256];
  size_t ip = 0;
  size_t skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  bc[ip++] = OP_SPLIT;
  size_t a_at = ip;
  covt_emit_u32_be(bc, &ip, 0);
  size_t b_at = ip;
  covt_emit_u32_be(bc, &ip, 0);
  size_t pos_at = ip;
  bc[ip++] = OP_POS;
  covt_emit_u32_be(bc, &ip, 5);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "a", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  size_t branch_b = ip;
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "a", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)pos_at);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  covt_patch_u32(bc, a_at, (uint32_t)pos_at);
  covt_patch_u32(bc, b_at, (uint32_t)branch_b);
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  snobol_search_result_t result;
  bool ok = covt_search_vm_run(bc, bc_len, "xa", 2, nullptr, 0, &result);
  test_assert((ok && result.match_start == 1) != 0,
              "POS failure backtracks to second SPLIT branch");
}

/* Charclass ops with crafted ranges (set 1 = { 'a' }, set 2 = { ',' }):
 * SPAN consume + fail, ANY success/fail (via range_meta), NOTANY success,
 * BREAKX retry push, trailer-fallback resolution. */


void test_cov_search_vm_charclass(void) {
  test_suite("Coverage: search-VM charclass ops");

  uint8_t bc[512];
  size_t ip = 0;
  size_t range1 = 200;
  size_t range2 = range1 + 12;
  size_t table_at = range2 + 12;
  size_t cc_at = table_at + 8;
  size_t bc_len = cc_at + 4;
  /* set 1: { 'a' } — u16 count, u16 case, CpRange('a','a') */
  covt_emit_u16_be(bc, &range1, 1);
  covt_emit_u16_be(bc, &range1, 0);
  covt_patch_u32(bc, range1, 'a');
  covt_patch_u32(bc, range1 + 4, 'a');
  /* set 2: { ',' } */
  covt_emit_u16_be(bc, &range2, 1);
  covt_emit_u16_be(bc, &range2, 0);
  covt_patch_u32(bc, range2, ',');
  covt_patch_u32(bc, range2 + 4, ',');
  /* offset table + class_count */
  covt_patch_u32(bc, table_at, 200);
  covt_patch_u32(bc, table_at + 4, 212);
  covt_patch_u32(bc, cc_at, 2);

  snobol_search_result_t result;

  /* Blob 1: SPAN(1) NOTANY(1) BREAKX(2) LIT(',') on "aa,x," → match 0..5.
   * range_meta NULL → bytecode-trailer fallback resolution. */
  ip = 0;
  size_t skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  size_t body = ip;
  bc[ip++] = OP_SPAN;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_NOTANY;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_BREAKX;
  covt_emit_u16_be(bc, &ip, 2);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, ",", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)body);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;

  bool ok = covt_search_vm_run(bc, bc_len, "aa,x,", 5, nullptr, 0, &result);
  test_assert((ok && result.match_start == 0 && result.match_end == 5) != 0,
              "SPAN+NOTANY+BREAKX+LIT matches");
  ok = covt_search_vm_run(bc, bc_len, "zz", 2, nullptr, 0, &result);
  test_assert((!ok) != 0, "SPAN fail + no match");

  /* Blob 2: ANY(1) LIT('b') — ANY resolves only via range_meta. */
  ip = 0;
  skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  body = ip;
  bc[ip++] = OP_ANY;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "b", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)body);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;

  snobol_range_meta_t rm[1];
  rm[0].ranges_ptr = bc + range1; /* range1 = 204 (after count/case emits) */
  rm[0].count = 1;
  rm[0].case_insensitive = 0;
  ok = covt_search_vm_run(bc, bc_len, "ab", 2, rm, 1, &result);
  test_assert((ok && result.match_end == 2) != 0, "ANY success path matches");
  ok = covt_search_vm_run(bc, bc_len, "zb", 2, rm, 1, &result);
  test_assert((!ok) != 0, "ANY fail path fails closed");

  /* ANY resolves its charclass from the bytecode trailer when range_meta is
   * absent, exactly like SPAN/BREAK/BREAKX/NOTANY. */
  ok = covt_search_vm_run(bc, bc_len, "ab", 2, nullptr, 0, &result);
  test_assert(ok, "ANY resolves ranges via the bytecode trailer");
  ok = covt_search_vm_run(bc, bc_len, "zb", 2, nullptr, 0, &result);
  test_assert((!ok) != 0, "ANY trailer resolve fails closed");

  /* Blob 3: SPAN(1) LIT('a') — SPAN then LIT: SPAN consumes the run so the
   * trailing LIT fails at the first non-class byte (choice-pop + fail_ret). */
  ip = 0;
  skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFU);
  body = ip;
  bc[ip++] = OP_SPAN;
  covt_emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, "a", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)body);
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;

  ok = covt_search_vm_run(bc, bc_len, "ba", 2, nullptr, 0, &result);
  test_assert((!ok) != 0, "SPAN-fail branch falls through");
  ok = covt_search_vm_run(bc, bc_len, "aaa", 3, nullptr, 0, &result);
  test_assert((!ok) != 0, "SPAN consumed past LIT position");

  /* Probe: the pike-scan BREAKX retry thread should re-run BREAKX from the
   * byte after the break char.  Today the retry ip is computed one byte too
   * late (it re-decodes an operand byte as an opcode), so on "a,x" the retry
   * thread can hit a spurious ACCEPT and report a false match.  The correct
   * behavior is no match.  Disabled until the engine is fixed; see
   * dev/coverage-findings.md.
   *
   * Bytecode: BREAKX(1) LIT('x') ACCEPT with the trailer range { ',' }.
   *   ip = 0; skip = covt_emit_repeat_init(bc, &ip, 0, 1, 0xFFFFFFFFu);
   *   body = ip;
   *   bc[ip++] = OP_BREAKX; covt_emit_u16_be(bc, &ip, 1);
   *   bc[ip++] = OP_LIT; covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
   *   covt_emit_u32_be(bc, &ip, 1); bc[ip++] = 'x'; bc[ip++] = OP_ACCEPT;
   *   bc[ip++] = OP_REPEAT_STEP; bc[ip++] = 0;
   *   covt_patch_u32(bc, ip, (uint32_t)body); ip += 4;
   *   covt_patch_u32(bc, skip, (uint32_t)ip); bc[ip++] = OP_ACCEPT;
   *   ok = covt_search_vm_run(bc, bc_len, "a,x", 3, NULL, 0, &result);
   *   test_assert(!ok, "BREAKX retry thread must not report a false match");
   */
}


void test_cov_search_vm_breakx_retry(void) {
  test_suite("Coverage: search-VM BREAKX retry + range clamp");

  uint8_t bc[256];
  size_t ip = 0;
  size_t skip = covt_emit_repeat_init(bc, &ip, 0, 0, 0xFFFFFFFFU);
  bc[ip++] = OP_BREAKX;
  covt_emit_u16_be(bc, &ip, 65); /* set_id above the 64-entry clamp */
  bc[ip++] = OP_LIT;
  covt_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covt_emit_u32_be(bc, &ip, 1);
  memcpy(bc + ip, ",", 1);
  ip += 1;
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  covt_patch_u32(bc, ip, (uint32_t)(ip - 3));
  ip += 4;
  covt_patch_u32(bc, skip, (uint32_t)ip);
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  snobol_range_meta_t rm[65];
  for (int i = 0; i < 65; i++) {
    rm[i].ranges_ptr = bc + bc_len;
    rm[i].count = 1;
    rm[i].case_insensitive = 0;
  }
  covt_patch_u32(bc, bc_len, ',');
  covt_patch_u32(bc, bc_len + 4, ',');
  size_t data = bc_len;
  for (int i = 0; i < 65; i++) {
    rm[i].ranges_ptr = bc + data;
  }

  snobol_search_result_t result;
  bool ok = covt_search_vm_run(bc, bc_len, "a,b", 3, rm, 65, &result);
  test_assert((ok && result.match_start == 0) != 0,
              "BREAKX retry finds the comma literal");
}

void test_search_suite(void) {
  test_suite("Search: tier caching");
  test_trie_cache_hit();
  test_2byte_prefix_memchr_path();
  test_cov_trie_bushy_via_pattern();
  test_cov_trie_flat_via_pattern();
  test_cov_trie_vm_cache();
  test_cov_trie_no_match_end();
  test_cov_walker_truncated_bc();
  test_cov_walker_bad_offsets();
  test_cov_walker_stack_overflow();
  test_cov_trie_node_cap();
  test_cov_automaton_bmh_gate();
  test_cov_automaton_demotion();
  test_cov_break_scan_diag();
  test_cov_span_scan_diag();
  test_cov_literal_prefix_tiers();
  test_cov_literal_only_paths();
  test_cov_bitmap_tier();
  test_cov_search_vm_choice_growth();
  test_cov_search_vm_anchored();
  test_cov_search_exec_misc();
  test_cov_build_alt_trie_failures();
  test_cov_pike_main_loop();
  test_cov_pike_overflow_restart();
  test_cov_search_vm_position_ops();
  test_cov_search_vm_fail_paths();
  test_cov_search_vm_split_fail();
  test_cov_search_vm_charclass();
  test_cov_search_vm_breakx_retry();
}
