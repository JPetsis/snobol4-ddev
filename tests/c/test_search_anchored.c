/**
 * test_search_anchored.c
 *
 * Tests for snobol_search_exec_anchored() — the anchored (SNOBOL-style) match
 * entry point used by Pattern::match().  Verifies that:
 *   - a match at offset 0 succeeds and reports match_start == 0,
 *   - a pattern that only matches later in the subject fails when anchored,
 *   - anchored results agree with the full VM (vm_run) on success and captures.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"
#include "snobol/vm.h"
#include "snobol/search.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* ── Helpers ───────────────────────────────────────────────────────────── */

static VM make_vm(const uint8_t *bc, size_t bc_len, const char *subject) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = strlen(subject);
  vm.ip = 0;
  vm.pos = 0;
  vm.var_count = 0;
  return vm;
}

static bool cap_equals(const VM *vm, uint8_t reg, const char *subject,
                       const char *expected) {
  if (reg >= MAX_CAPS)
    return false;
  size_t a = vm->cap_start[reg];
  size_t b = vm->cap_end[reg];
  if (expected == NULL)
    return a == 0 && b == 0;
  if (b < a)
    return false;
  size_t n = b - a;
  return strlen(expected) == n && memcmp(subject + a, expected, n) == 0;
}

/* Compile @p pattern_str and run it both anchored (snobol_search_exec_anchored)
 * and via the full VM (vm_run).  Assert agreement and anchored invariants.
 * expected_cap0 checks capture register 0 (the parser allocates the first
 * @name capture to register 0). */
static void assert_anchored(const char *pattern_str, const char *subject,
                            bool expect_success, const char *expected_cap0) {
  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  char *error = NULL;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, pattern_str, strlen(pattern_str), &error);
  test_assert(pat != NULL, "pattern compiled");
  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }

  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(bc != NULL && bc_len > 0, "bytecode available");
  test_assert(meta != NULL, "metadata available");

  /* Anchored entry */
  VM avm = make_vm(bc, bc_len, subject);
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok_a = snobol_search_exec_anchored(&avm, subject, strlen(subject), meta,
                                          NULL, &result, NULL);

  /* Full VM anchored (vm_run) — reference for agreement */
  VM fvm = make_vm(bc, bc_len, subject);
  bool ok_f = vm_run(&fvm);

  test_assert(ok_a == expect_success, "anchored success matches expectation");
  test_assert(ok_a == ok_f, "anchored and full-VM anchored agree on success");

  if (expect_success) {
    test_assert(result.match_start == 0, "anchored match starts at offset 0");
    test_assert(result.match_end > result.match_start,
                "anchored match has positive length");
    test_assert((result.match_end - result.match_start) == fvm.pos,
                "anchored match length equals full-VM consumed length");
    if (expected_cap0) {
      test_assert(cap_equals(&avm, 0, subject, expected_cap0),
                  "anchored cap0 matches expected");
      test_assert(cap_equals(&fvm, 0, subject, expected_cap0),
                  "full-VM cap0 matches expected");
      test_assert(avm.cap_start[0] == fvm.cap_start[0],
                  "cap0 start consistent across tiers");
      test_assert(avm.cap_end[0] == fvm.cap_end[0],
                  "cap0 end consistent across tiers");
    }
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Anchored literal at the start succeeds */
static void test_anchored_literal_at_start(void) {
  assert_anchored("'abc'", "abcdef", true, NULL);
}

/* Literal present but not at offset 0 must FAIL when anchored */
static void test_anchored_literal_not_at_start(void) {
  assert_anchored("'abc'", "xxabcdef", false, NULL);
}

/* Anchored capture at start succeeds and captures correctly */
static void test_anchored_capture_at_start(void) {
  assert_anchored("@r1('hello')", "hello world", true, "hello");
}

/* Capture only available later in subject must FAIL when anchored */
static void test_anchored_capture_not_at_start(void) {
  assert_anchored("@r1('hello')", "say hello there", false, NULL);
}

/* Alternation anchored at 0 */
static void test_anchored_alternation(void) {
  assert_anchored("('a' | 'b') 'c'", "bcd", true, NULL);
}

/* Capture inside a failed-then-taken alternation with trailing literal
 * (Tier 6 search-VM backtracking), anchored at 0 */
static void test_anchored_backtrack_alt(void) {
  assert_anchored("('foo' | @r1('bar')) 'baz'", "barbaz", true, "bar");
}

/* Prefix literal not at start fails when anchored */
static void test_anchored_prefix_not_at_start(void) {
  assert_anchored("'needle'", "hay needle stack", false, NULL);
}

/* Deep backtracking with nested captures (stresses the search-VM choice
 * stack — each SPLIT pushes a ~2 KiB search_choice_t). Mirrors the heap
 * overflow regression (search.c:search_vm_push_choice). */
static void test_anchored_deep_backtrack(void) {
  assert_anchored("('foo' | 'bar' | @r1('baz')) 'qux'", "bazqux", true, "baz");
}

/* ── Suite entry point ────────────────────────────────────────────────── */

void test_search_anchored_suite(void) {
  test_suite("Search: Anchored Entry");
  test_anchored_literal_at_start();
  test_anchored_literal_not_at_start();
  test_anchored_capture_at_start();
  test_anchored_capture_not_at_start();
  test_anchored_alternation();
  test_anchored_backtrack_alt();
  test_anchored_prefix_not_at_start();
  test_anchored_deep_backtrack();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Anchored literal-only early-exit: search_literal_only() must resolve an
 * anchored single-literal pattern with a single memcmp at start_offset, giving
 * results IDENTICAL to the full VM. These exercises cover the branch added by
 * the anchored-literal realignment fix.
 * ═════════════════════════════════════════════════════════════════════════ */

/* Compare the anchored literal-only path against the full VM (the correctness
 * reference; the pre-change path produced the same logical result, just slower).
 * `start_offset` is the anchor position (0 for a plain anchored match). */
static void assert_literal_only_anchored(const char *pattern_str,
                                         const char *subject,
                                         size_t start_offset,
                                         bool expect_success,
                                         size_t expect_len) {
  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");
  char *error = NULL;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, pattern_str, strlen(pattern_str), &error);
  test_assert(pat != NULL, "pattern compiled");
  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }
  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
  test_assert(bc != NULL && bc_len > 0, "bytecode available");
  test_assert(meta != NULL, "metadata available");

  VM avm = make_vm(bc, bc_len, subject);
  avm.pos = start_offset;
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok_a = snobol_search_exec_anchored(&avm, subject, strlen(subject), meta,
                                          NULL, &result, NULL);

  VM fvm = make_vm(bc, bc_len, subject);
  fvm.pos = start_offset;
  bool ok_f = vm_run(&fvm);

  test_assert(ok_a == expect_success,
              "anchored literal-only success expectation");
  test_assert(ok_a == ok_f, "anchored literal-only agrees with full VM");
  if (expect_success) {
    test_assert(result.match_start == start_offset,
                "anchored match starts at the anchor offset");
    test_assert((result.match_end - result.match_start) == expect_len,
                "anchored match length equals expected length");
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* Anchored literal at offset 0 succeeds (the fast memcmp path). */
static void test_literal_only_at_start(void) {
  assert_literal_only_anchored("'pqr'", "pqrzzzzzzzzzzzzzz", 0, true, 3);
}

/* Anchored literal present only at offset 16 must be rejected (single memcmp
 * at offset 0 fails immediately — no whole-subject scan). */
static void test_literal_only_late_reject(void) {
  char subj[64];
  memset(subj, 'z', sizeof(subj));
  subj[16] = 'p';
  subj[17] = 'q';
  subj[18] = 'r';
  subj[sizeof(subj) - 1] = '\0';
  assert_literal_only_anchored("'pqr'", subj, 0, false, 0);
}

/* Anchored literal absent from the subject is rejected. */
static void test_literal_only_absent(void) {
  assert_literal_only_anchored("'pqr'", "abcdefghijklmnop", 0, false, 0);
}

/* Empty literal anchored at 0 matches with zero length (memcmp of length 0). */
static void test_literal_only_empty(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *error = NULL;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, "''", 2, &error);
  test_assert(pat != NULL, "empty literal compiled");
  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);

  VM avm = make_vm(bc, bc_len, "abc");
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok_a =
      snobol_search_exec_anchored(&avm, "abc", 3, meta, NULL, &result, NULL);

  VM fvm = make_vm(bc, bc_len, "abc");
  bool ok_f = vm_run(&fvm);

  test_assert(ok_a, "empty literal anchored match succeeds");
  test_assert(ok_a == ok_f, "empty literal agrees with full VM");
  test_assert(result.match_start == 0, "empty literal starts at 0");
  test_assert(result.match_end == result.match_start,
              "empty literal has zero length");

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* POS/RPOS preludes are not parseable as source strings in this engine, so we
 * build the bytecode directly and derive meta. This still exercises the
 * prelude-skip + anchored memcmp branch in search_literal_only(). */
static void assert_literal_only_bc(const uint8_t *bc, size_t bc_len,
                                   const char *subject, size_t start_offset,
                                   bool expect_success, size_t expect_len) {
  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  VM avm = make_vm(bc, bc_len, subject);
  avm.pos = start_offset;
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok_a = snobol_search_exec_anchored(&avm, subject, strlen(subject), &meta,
                                          NULL, &result, NULL);

  VM fvm = make_vm(bc, bc_len, subject);
  fvm.pos = start_offset;
  bool ok_f = vm_run(&fvm);

  test_assert(ok_a == expect_success,
              "anchored literal-only success expectation");
  test_assert(ok_a == ok_f, "anchored literal-only agrees with full VM");
  if (expect_success) {
    test_assert(result.match_start == start_offset,
                "anchored match starts at the anchor offset");
    test_assert((result.match_end - result.match_start) == expect_len,
                "anchored match length equals expected length");
  }
  snobol_search_meta_free(&meta);
}

/* POS(0)-wrapped literal resolves identically to the bare literal (the prelude
 * skip leaves the anchored memcmp behavior unchanged). Bytecode:
 *   OP_POS(0) OP_LIT(off=14,len=3) "pqr" OP_ACCEPT */
static void test_literal_only_pos_wrapped(void) {
  uint8_t bc[] = {OP_POS, 0,   0,   0,        0,
                  OP_LIT, 0,   0,   0,        14, /* lit_off = 14 */
                  0,      0,   0,   3,            /* lit_len = 3 */
                  'p',    'q', 'r', OP_ACCEPT};
  assert_literal_only_bc(bc, sizeof(bc), "pqrzzzzzzzzzzzzzz", 0, true, 3);
}

/* RPOS(3)-wrapped literal at the end resolves identically to the full VM.
 * Bytecode: OP_RPOS(3) OP_LIT(off=14,len=3) "abc" OP_ACCEPT */
static void test_literal_only_rpos_wrapped(void) {
  uint8_t bc[] = {OP_RPOS, 0,   0,   0,        3,
                  OP_LIT,  0,   0,   0,        14, /* lit_off = 14 */
                  0,       0,   0,   3,            /* lit_len = 3 */
                  'a',     'b', 'c', OP_ACCEPT};
  assert_literal_only_bc(bc, sizeof(bc), "abc", 0, true, 3);
}

void test_search_literal_only_anchored_suite(void) {
  test_suite("Search: Anchored literal-only early-exit");
  test_literal_only_at_start();
  test_literal_only_late_reject();
  test_literal_only_absent();
  test_literal_only_empty();
  test_literal_only_pos_wrapped();
  test_literal_only_rpos_wrapped();
}
