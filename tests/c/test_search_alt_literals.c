/**
 * test_search_alt_literals.c - Tests for Tier 5 (alt-literals) robustness
 * and small-prefix acceleration.
 *
 * Covers:
 *  - Flat alternation-of-literals falls through to TIER_GENERAL (regression
 *    fix for the 125x Tier 5 regression).
 *  - Bushy alternation patterns still use the trie (TIER_ALT_LIT).
 *  - Start-byte bitmap filtering in the Tier 5 scan loop skips non-candidate
 *    positions.
 *  - Two-byte literal prefix uses the memchr fast-path.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* ── Bytecode builder helpers (mirrored from test_search_runtime.c) ─────── */

static void emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

/* Build: SPLIT(LIT(s1) ACCEPT) (LIT(s2) ACCEPT) */
static size_t build_split_lit_lit(uint8_t *bc, const char *s1, size_t len1,
                                  const char *s2, size_t len2) {
  size_t ip = 0;
  bc[ip++] = OP_SPLIT;

  uint32_t branch_a = (uint32_t)(1 + 4 + 4);
  emit_u32_be(bc, &ip, branch_a);

  uint32_t branch_b = branch_a + (uint32_t)(9 + len1 + 1);
  emit_u32_be(bc, &ip, branch_b);

  bc[ip++] = OP_LIT;
  uint32_t data_off_a = (uint32_t)(ip + 4 + 4);
  emit_u32_be(bc, &ip, data_off_a);
  emit_u32_be(bc, &ip, (uint32_t)len1);
  memcpy(bc + ip, s1, len1);
  ip += len1;
  bc[ip++] = OP_ACCEPT;

  bc[ip++] = OP_LIT;
  uint32_t data_off_b = (uint32_t)(ip + 4 + 4);
  emit_u32_be(bc, &ip, data_off_b);
  emit_u32_be(bc, &ip, (uint32_t)len2);
  memcpy(bc + ip, s2, len2);
  ip += len2;
  bc[ip++] = OP_ACCEPT;

  return ip;
}

/* Three-way nested split: (s1) | (s2) | (s3) */
static size_t build_split_3(uint8_t *bc, const char *s1, size_t len1,
                            const char *s2, size_t len2, const char *s3,
                            size_t len3) {
  /* Outer SPLIT: branch_a -> s1 ; branch_b -> inner SPLIT(s2|s3) */
  size_t ip = 0;
  bc[ip++] = OP_SPLIT;
  uint32_t outer_a = (uint32_t)(1 + 4 + 4);
  emit_u32_be(bc, &ip, outer_a);
  uint32_t outer_b = outer_a + (uint32_t)(9 + len1 + 1);
  emit_u32_be(bc, &ip, outer_b);

  /* branch a: LIT(s1) ACCEPT */
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  emit_u32_be(bc, &ip, (uint32_t)len1);
  memcpy(bc + ip, s1, len1);
  ip += len1;
  bc[ip++] = OP_ACCEPT;

  /* inner SPLIT: s2 | s3 */
  bc[ip++] = OP_SPLIT;
  uint32_t inner_a = (uint32_t)(ip + 4 + 4);
  emit_u32_be(bc, &ip, inner_a);
  uint32_t inner_b = inner_a + (uint32_t)(9 + len2 + 1);
  emit_u32_be(bc, &ip, inner_b);

  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  emit_u32_be(bc, &ip, (uint32_t)len2);
  memcpy(bc + ip, s2, len2);
  ip += len2;
  bc[ip++] = OP_ACCEPT;

  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(ip + 4 + 4));
  emit_u32_be(bc, &ip, (uint32_t)len3);
  memcpy(bc + ip, s3, len3);
  ip += len3;
  bc[ip++] = OP_ACCEPT;

  return ip;
}

/* Flat alternation-of-literals routes to the trie (TIER_ALT_LIT), which
 * handles flat tries as a set-membership test (no minlength acceleration
 * but correct matching). */
void test_alt_literals_flat_trie(void) {
  test_suite("Alt-literals: flat routes to trie");

  uint8_t bc[256];
  size_t bc_len = build_split_3(bc, "apple", 5, "orange", 6, "banana", 6);

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert(meta.is_alt_literals, "detected as alt-literals");
  test_assert(meta.is_alt_literals_flat, "classified flat (no shared prefix)");
  test_assert(meta.tier == TIER_ALT_LIT,
              "flat alt-literals dispatched to TIER_ALT_LIT (trie)");

  /* Behavioral correctness through the search entrypoint. */
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  bool ok = snobol_search_exec(&vm, "I ate a banana for lunch", 23, 0, &meta,
                               NULL, &result, NULL);
  test_assert(ok, "flat alt-literals matches 'banana' via trie");

  /* Verify no-match case. */
  result.success = false;
  ok = snobol_search_exec(&vm, "nothing here", 12, 0, &meta, NULL, &result,
                          NULL);
  test_assert(!ok, "flat alt-literals no match on non-matching subject");

  snobol_search_meta_free(&meta);
  snobol_search_vm_cleanup(&vm);
}

/* Sanity: bushy alternation (shared prefix) keeps the trie path. */
void test_alt_literals_bushy_trie(void) {
  test_suite("Alt-literals: bushy keeps TIER_ALT_LIT");

  uint8_t bc[256];
  size_t bc_len = build_split_lit_lit(bc, "apple", 5, "apricot", 7);

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert(meta.is_alt_literals, "detected as alt-literals");
  test_assert(!meta.is_alt_literals_flat, "classified bushy (shared prefix)");
  test_assert(meta.tier == TIER_ALT_LIT,
              "bushy alt-literals dispatched to TIER_ALT_LIT");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  bool ok = snobol_search_exec(&vm, "an apricot fell", 16, 0, &meta, NULL,
                               &result, NULL);
  test_assert(ok, "bushy alt-literals matches 'apricot' via trie");
  test_assert(result.match_start == 3, "match at offset 3");

  snobol_search_meta_free(&meta);
  snobol_search_vm_cleanup(&vm);
}

/* The Tier 5 scan loop applies the start-byte bitmap filter so
 * candidate positions whose byte cannot begin any alternative are skipped.
 * Pattern 'abc'|'axy' on subject "xabc": offset 0 ('x') must be filtered,
 * the match is found at offset 1. */
void test_tier5_start_bitmap_skip(void) {
  test_suite("Tier 5: start-byte bitmap filter skips non-candidates");

  uint8_t bc[256];
  size_t bc_len = build_split_lit_lit(bc, "abc", 3, "axy", 3);

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert(meta.is_alt_literals, "detected as alt-literals");
  test_assert(!meta.is_alt_literals_flat,
              "bushy (first bytes differ, shared?)");
  test_assert(meta.tier == TIER_ALT_LIT, "uses trie path");
  test_assert(meta.has_start_bitmap, "start-bitmap computed for alt-literals");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  bool ok = snobol_search_exec(&vm, "xabc", 4, 0, &meta, NULL, &result, NULL);
  test_assert(ok, "'abc'|'axy' matches in 'xabc'");
  test_assert(result.match_start == 1,
              "match found at offset 1 (offset 0 'x' filtered by bitmap)");
  test_assert(result.match_end == 4, "match length is 3");

  snobol_search_meta_free(&meta);
  snobol_search_vm_cleanup(&vm);
}

/* Two-byte literal prefix uses the paired-memchr fast-path and
 * produces correct results across subjects. */
void test_2byte_prefix_memchr(void) {
  test_suite("Small-prefix: 2-byte memchr fast-path");

  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'ab'", 4, 0, &err);
  test_assert(pat != NULL, "compile 2-byte literal succeeds");

  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta != NULL, "meta available");
    if (meta)
      test_assert(meta->literal_prefix_len == 2,
                  "literal_prefix_len is 2 (memchr path)");

    snobol_match_t *m = snobol_pattern_search(pat, "xxabyy", 6);
    test_assert(m != NULL, "search returns a result");
    if (m) {
      test_assert(snobol_match_success(m), "'ab' matches in 'xxabyy'");
      test_assert(snobol_match_get_position(m) == 2, "match at offset 2");
      snobol_match_free(m);
    }

    m = snobol_pattern_search(pat, "abc", 3);
    if (m) {
      test_assert(snobol_match_success(m) && snobol_match_get_position(m) == 0,
                  "'ab' matches at offset 0 in 'abc'");
      snobol_match_free(m);
    }

    m = snobol_pattern_search(pat, "xxxx", 4);
    if (m) {
      test_assert(!snobol_match_success(m), "'ab' does not match 'xxxx'");
      snobol_match_free(m);
    }

    m = snobol_pattern_search(pat, "abab", 4);
    if (m) {
      test_assert(snobol_match_success(m) && snobol_match_get_position(m) == 0,
                  "'ab' first match at offset 0 in 'abab'");
      snobol_match_free(m);
    }

    snobol_pattern_free(pat);
  }

  free(err);
  snobol_context_destroy(ctx);
}

/* P5: shared-prefix alternation-of-literals enables BMH skip in the
 * per-offset trial loop (TIER_GENERAL / TIER_AUTOMATON).  The shared prefix
 * 'ab' of 'abc'|'abd' must populate the BMH skip window; flat alternatives
 * ('foo'|'bar') share no prefix and must NOT set has_bmh_skip. */
void test_alt_literals_bmh_skip(void) {
  test_suite("Alt-literals: P5 BMH skip from shared prefix");

  /* Bushy: 'abc' | 'abd'  (shared prefix "ab") */
  uint8_t bc_bushy[256];
  size_t len_bushy = build_split_lit_lit(bc_bushy, "abc", 3, "abd", 3);
  snobol_search_meta_t meta_b;
  snobol_search_derive_meta(bc_bushy, len_bushy, &meta_b);

  test_assert(meta_b.is_alt_literals, "detected as alt-literals");
  test_assert(!meta_b.is_alt_literals_flat, "classified bushy (shared prefix)");
  test_assert(meta_b.has_bmh_skip, "has_bmh_skip set for shared prefix");
  test_assert(meta_b.bmh_skip_len == 2, "bmh_skip_len == 2 (shared 'ab')");
  test_assert(meta_b.literal_prefix_len == 2, "literal_prefix_len == 2");
  test_assert(meta_b.literal_prefix[0] == 'a' &&
                  meta_b.literal_prefix[1] == 'b',
              "shared prefix bytes are 'ab'");
  test_assert(!meta_b.has_literal_prefix,
              "has_literal_prefix stays false (no TIER_PREFIX misroute)");
  /* BMH table: prefix[0]='a' gets skip 1 (plen-1-0), other bytes skip 2. */
  test_assert(meta_b.bmh_skip[(uint8_t)'a'] == 1,
              "BMH skip for prefix[0] is 1 (plen-1-0)");
  test_assert(meta_b.bmh_skip[(uint8_t)'z'] == 2,
              "BMH skip for non-prefix byte is 2");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc_bushy;
  vm.bc_len = len_bushy;
  snobol_search_result_t r;
  bool ok = snobol_search_exec(&vm, "xxabcyy", 7, 0, &meta_b, NULL, &r, NULL);
  test_assert(ok && r.match_start == 2, "'abc' matches at offset 2");
  ok = snobol_search_exec(&vm, "xxabdyy", 7, 0, &meta_b, NULL, &r, NULL);
  test_assert(ok && r.match_start == 2, "'abd' matches at offset 2");
  ok = snobol_search_exec(&vm, "zzzzzzz", 7, 0, &meta_b, NULL, &r, NULL);
  test_assert(!ok, "no match when no shared-prefix start");
  snobol_search_meta_free(&meta_b);

  /* Flat: 'foo' | 'bar'  (no shared prefix) */
  uint8_t bc_flat[256];
  size_t len_flat = build_split_lit_lit(bc_flat, "foo", 3, "bar", 3);
  snobol_search_meta_t meta_f;
  snobol_search_derive_meta(bc_flat, len_flat, &meta_f);

  test_assert(meta_f.is_alt_literals, "flat detected as alt-literals");
  test_assert(meta_f.is_alt_literals_flat, "classified flat");
  test_assert(!meta_f.has_bmh_skip,
              "has_bmh_skip NOT set for flat (no shared prefix)");
  snobol_search_meta_free(&meta_f);
  snobol_search_vm_cleanup(&vm);
}

/* Build a source alternation of n literal branches and return it malloc'd. */
static char *build_alt_source(size_t n, const char *prefix) {
  size_t cap = n * (strlen(prefix) + 12) + 8;
  char *src = (char *)malloc(cap);
  if (!src)
    return NULL;
  char *p = src;
  *p++ = '\'';
  for (size_t i = 0; i < n; i++) {
    if (i)
      p += sprintf(p, "' | '");
    p += sprintf(p, "%s%zu", prefix, i);
  }
  *p++ = '\'';
  *p = '\0';
  return src;
}

/* Alternations whose bytecode exceeds the historical 512-byte alt-literals
 * walk bound used to fall out of the trie tier AND be misrejected by the
 * required-literal prefilter (which picked the last branch's literal).
 * Regression for both: (a) the walk bound raised to 2048 so moderately large
 * alternations stay on TIER_ALT_LIT; (b) the prefilter no longer derives a
 * required literal from patterns whose SPLITs precede every literal, so even
 * >2048-byte alternations match any branch. */
void test_alt_literals_large_chain(void) {
  test_suite("Alt-literals: large chains stay on the trie");

  snobol_context_t *ctx = snobol_context_create();

  /* ~40 branches with distinct lengths — bytecode > 512 bytes, < 2048. */
  char *src = build_alt_source(40, "lit");
  snobol_pattern_t *p =
      snobol_pattern_compile(ctx, src, strlen(src), NULL);
  test_assert(p != NULL, "large alternation compiles");
  if (p) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
    test_assert(meta->is_alt_literals, "large alternation detected");
    test_assert(meta->tier == TIER_ALT_LIT,
                "large alternation stays on TIER_ALT_LIT");
    snobol_match_t *m = snobol_pattern_search(p, "xx lit17 yy", 11);
    test_assert(m && snobol_match_success(m),
                "middle branch matches via the trie");
    snobol_match_free(m);
    m = snobol_pattern_search(p, "lit39 end", 9);
    test_assert(m && snobol_match_success(m), "last branch matches");
    snobol_match_free(m);
    m = snobol_pattern_search(p, "none", 4);
    test_assert(m && !snobol_match_success(m), "no branch -> miss");
    snobol_match_free(m);
    snobol_pattern_free(p);
  }
  free(src);

  /* ~260 branches — bytecode > 2048 bytes: falls off the trie walk bound
   * but must still match any branch (prefilter regression). */
  src = build_alt_source(260, "longlit");
  p = snobol_pattern_compile(ctx, src, strlen(src), NULL);
  test_assert(p != NULL, "over-bound alternation compiles");
  if (p) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
    test_assert(!meta->has_required_lit,
                "over-bound alternation derives no required literal");
    snobol_match_t *m = snobol_pattern_search(p, "zz longlit130 zz", 16);
    test_assert(m && snobol_match_success(m),
                "over-bound alternation matches a middle branch");
    snobol_match_free(m);
    snobol_pattern_free(p);
  }
  free(src);

  snobol_context_destroy(ctx);
}

void test_search_alt_literals_suite(void) {
  test_alt_literals_flat_trie();
  test_alt_literals_bushy_trie();
  test_tier5_start_bitmap_skip();
  test_2byte_prefix_memchr();
  test_alt_literals_bmh_skip();
  test_alt_literals_large_chain();
}
