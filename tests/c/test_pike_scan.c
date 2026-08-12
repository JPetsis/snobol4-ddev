/**
 * test_pike_scan.c — Pike single-pass scan tests (W1c)
 *
 * Tests the Pike scan function directly (not through tier dispatch).
 * Gated behind SNOBOL_PIKE_SCAN; compiled only when enabled.
 */
#ifdef SNOBOL_PIKE_SCAN

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snobol/search.h"
#include "snobol/vm.h"
#include "snobol/snobol.h"
#include "snobol/ast.h"
#include "snobol/compiler.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);
extern bool pike_scan(const uint8_t *bc, size_t bc_len, const char *subject,
                      size_t subject_len, const snobol_search_meta_t *meta,
                      const snobol_range_meta_t *range_meta,
                      size_t range_meta_count, VM *vm,
                      snobol_search_result_t *out_result);

static int pike_test_count = 0, pike_test_pass = 0;

static void pike_assert(bool cond, const char *name) {
  pike_test_count++;
  if (cond) {
    pike_test_pass++;
    return;
  }
  fprintf(stderr, "  PIKE FAIL: %s\n", name);
}

/* Mirror of the core pattern struct (core/src/api.c) so pike_scan can be
 * driven with AST-compiled bytecode + derived metadata + range metadata. */
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
} pike_pattern_layout;

static snobol_pattern_t *pike_make_pattern(ast_node_t *root) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  if (compile_ast_to_bytecode_c(root, false, &bc, &bc_len) != 0) {
    return nullptr;
  }
  pike_pattern_layout *p =
      (pike_pattern_layout *)calloc(1, sizeof(pike_pattern_layout));
  if (!p) {
    free(bc);
    return nullptr;
  }
  p->bc = bc;
  p->bc_len = bc_len;
  snobol_search_derive_meta(bc, bc_len, &p->meta);
  p->meta_initialized = true;
  snobol_build_range_meta(bc, bc_len, &p->range_meta, &p->range_meta_count);
  return (snobol_pattern_t *)p;
}

static void pike_test_literal(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "'hello'", 7, &err);
  if (!p) {
    pike_assert(false, "literal compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "say hello world", 15, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "literal match succeeds");
  pike_assert(r.match_start == 4, "literal match_start == 4");
  pike_assert(r.match_end == 9, "literal match_end == 9");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

static void pike_test_span(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "SPAN('0-9')", 11, &err);
  if (!p) {
    pike_assert(false, "span compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "abc123def", 9, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "span match succeeds");
  pike_assert(r.match_start == 3, "span match_start == 3");
  pike_assert(r.match_end == 6, "span match_end == 6");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

static void pike_test_alt_capture(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p =
      snobol_pattern_compile(ctx, "(@r1('a') | @r1('b'))", 21, &err);
  if (!p) {
    pike_assert(false, "alt+cap compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "b", 1, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "alt+cap match succeeds");
  pike_assert(r.match_start == 0, "alt+cap match_start == 0");
  pike_assert(r.match_end == 1, "alt+cap match_end == 1");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

static void pike_test_notany(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p =
      snobol_pattern_compile(ctx, "NOTANY('aeiou')", 15, &err);
  if (!p) {
    pike_assert(false, "notany compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "frog", 4, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "notany match succeeds");
  pike_assert(r.match_start == 0, "notany match_start == 0");
  pike_assert(r.match_end == 1, "notany match_end == 1");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

/* Multi-capture alternation: @r1('a') | @r1('b') — verifies that pike_scan
 * carries capture registers across threads and writes them back correctly. */
static void pike_test_multi_capture_alt(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p =
      snobol_pattern_compile(ctx, "(@r1('foo') | @r1('bar'))", 25, &err);
  if (!p) {
    pike_assert(false, "multi-cap alt compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "bar is here", 11, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "multi-cap alt match succeeds");
  pike_assert(r.match_start == 0, "multi-cap alt match_start == 0");
  pike_assert(r.match_end == 3, "multi-cap alt match_end == 3");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

/* Unicode case-insensitive: pike_scan must check charclasses correctly,
 * so а (U+0430) matches А (U+0410) but NOT Б (U+0411). */
static void pike_test_ci_cyrillic(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  /* CI flag: SNOBOL_FLAG_CASE_INSENSITIVE = 1 */
  snobol_pattern_t *p =
      snobol_pattern_compile_ex(ctx, "'\xD0\xB0'", 3, 1, &err);
  if (!p) {
    pike_assert(false, "CI cyrillic compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);

  snobol_search_result_t r;
  /* а (U+0430) should match А (U+0410) */
  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "\xD0\x90", 2, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "CI: а matches А");

  /* а (U+0430) should NOT match Б (U+0411) */
  r.success = false;
  ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                 "\xD0\x91", 2, meta, rm, rc, nullptr, &r);
  pike_assert((!ok) != 0, "CI: а does NOT match Б");

  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

/* Overflow scenario: BREAKX(' ') over 1KB subject exercises overflow
 * fallback via tier_search_vm when pike_scan's thread buffer fills up.
 * Uses snobol_search_exec so the dispatch tier handles routing. */
static void pike_test_overflow_long(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "BREAKX(' ')", 11, &err);
  if (!p) {
    pike_assert(false, "overflow long compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  char subject[1024];
  memset(subject, 'x', 900);
  subject[900] = ' ';
  subject[901] = '\0';
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
  vm.bc_len = snobol_pattern_get_bc_len(p);
  snobol_search_result_t r;
  bool ok =
      snobol_search_exec(&vm, subject, 901, 0, meta, nullptr, &r, nullptr);
  pike_assert(ok, "overflow long: BREAKX finds space");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

/* Same pattern over short subject works normally */
static void pike_test_overflow_short(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "BREAKX(' ')", 11, &err);
  if (!p) {
    pike_assert(false, "overflow short compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  char subject[] = "hello world";
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
  vm.bc_len = snobol_pattern_get_bc_len(p);
  snobol_search_result_t r;
  bool ok = snobol_search_exec(&vm, subject, 11, 0, meta, nullptr, &r, nullptr);
  pike_assert(ok, "overflow short: BREAKX finds space");
  pike_assert(r.match_start == 0, "overflow short: match_start == 0");
  pike_assert(r.match_end == 5, "overflow short: match_end == 5");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

/* BREAKX retry branch: 'a' BREAKX(',') LEN(1) 'c' on "a,b,c" only matches
 * via the retry thread (re-execute BREAKX after the first delimiter, then
 * LEN(1) consumes the second delimiter and 'c' matches).  Regression for the
 * rt.ip = ip - 2 bug where the retry thread re-decoded an operand byte
 * (usually 0x00 = OP_ACCEPT) and reported a bogus short match. */
static void pike_test_breakx_retry(void) {
  ast_node_t **parts = (ast_node_t **)malloc(4 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_lit("a", 1);
  parts[1] = snobol_ast_create_breakx(",", 1);
  parts[2] = snobol_ast_create_len(1);
  parts[3] = snobol_ast_create_lit("c", 1);
  ast_node_t *root = snobol_ast_create_concat(parts, 4);
  snobol_pattern_t *p = pike_make_pattern(root);
  snobol_ast_free(root);
  if (!p) {
    pike_assert(false, "breakx retry compile");
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  size_t rc = 0;
  const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
  snobol_search_result_t r;

  bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                      "a,b,c", 5, meta, rm, rc, nullptr, &r);
  pike_assert(ok, "breakx retry match succeeds");
  pike_assert((r.match_start == 0 && r.match_end == 5) != 0,
              "breakx retry match_start == 0, match_end == 5");

  r.success = false;
  ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                 "a,b,x", 5, meta, rm, rc, nullptr, &r);
  pike_assert((!ok) != 0, "breakx retry no match on a,b,x");

  snobol_pattern_free(p);
}

/* Mid-pattern position constraints: pike must validate POS/RPOS/TAB/RTAB
 * instead of skipping them as no-ops.  'ab' moves the cursor to 2, so:
 *   POS(5)  → target 5  → fail (cursor != 5)
 *   TAB(5)  → target 5 ≥ len 3 → fail
 *   RPOS(0) → target 3  → fail (cursor != 3)
 *   RTAB(2) → target 1  → fail (cursor past target)
 * while POS(2) and RTAB(1) are satisfied and must still match. */
static void pike_test_position_ops(void) {
  const char *subject = "abc";
  snobol_search_result_t r;

  struct {
    ast_node_t *pos_op;
    const char *label;
  } cases[] = {
      {snobol_ast_create_pos(5), "POS(5)"},
      {snobol_ast_create_tab(5), "TAB(5)"},
      {snobol_ast_create_rpos(0), "RPOS(0)"},
      {snobol_ast_create_rtab(2), "RTAB(2)"},
      {snobol_ast_create_pos(2), "POS(2)"},
      {snobol_ast_create_rtab(1), "RTAB(1)"},
  };
  const bool expect[] = {false, false, false, false, true, true};

  for (size_t i = 0; i < 6; i++) {
    ast_node_t **pp = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    pp[0] = snobol_ast_create_lit("ab", 2);
    pp[1] = cases[i].pos_op;
    pp[2] = snobol_ast_create_lit("c", 1);
    ast_node_t *root = snobol_ast_create_concat(pp, 3);
    snobol_pattern_t *p = pike_make_pattern(root);
    snobol_ast_free(root);
    if (!p) {
      pike_assert(false, "position-op compile");
      continue;
    }
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
    size_t rc = 0;
    const snobol_range_meta_t *rm = snobol_pattern_get_range_meta(p, &rc);
    bool ok = pike_scan(snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p),
                        subject, 3, meta, rm, rc, nullptr, &r);
    if (expect[i]) {
      pike_assert(ok, "position-op positive control matches");
      pike_assert((r.match_start == 0 && r.match_end == 3) != 0,
                  "position-op positive control spans subject");
    } else {
      char msg[64];
      snprintf(msg, sizeof(msg), "pike enforces %s", cases[i].label);
      pike_assert((!ok) != 0, msg);
    }
    snobol_pattern_free(p);
  }
}

void test_pike_scan_suite(void) {
  test_suite("Search: Pike Scan");
  pike_test_count = 0;
  pike_test_pass = 0;
  pike_test_literal();
  pike_test_span();
  pike_test_alt_capture();
  pike_test_notany();
  pike_test_multi_capture_alt();
  pike_test_ci_cyrillic();
  pike_test_breakx_retry();
  pike_test_position_ops();
  pike_test_overflow_long();
  pike_test_overflow_short();
  test_assert(pike_test_pass == pike_test_count, "pike scan: all tests pass");
}

#endif /* SNOBOL_PIKE_SCAN */
