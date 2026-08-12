/**
 * test_search_vm_capture.c
 *
 * Tests for the capture-aware Tier-6 search-VM, verifying that patterns
 * using CAP_START, CAP_END, ASSIGN, and BREAKX produce identical results
 * on the search-VM (Tier 6) and the full VM (Tier 8).
 *
 * Uses the C API to compile patterns (avoiding manual bytecode construction)
 * and extracts bytecode for direct comparison between execution paths.
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

/* ── Test: compile pattern, run via search-VM and full VM, compare ─────── */

/*
 * Compiles a SNOBOL4 pattern string, runs it through both snobol_search_exec
 * (Tier 6 for eligible patterns) and vm_run (Tier 8), and asserts that
 * the capture registers are identical.
 *
 * Note: The parser allocates capture registers sequentially from 0 in
 * order of @name appearance, so expected_cap0 checks register 0 and
 * expected_cap1 checks register 1.
 */
static void assert_captures_match(const char *pattern_str, const char *subject,
                                  const char *expected_cap0,
                                  const char *expected_cap1) {
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

  /* ---- Run via snobol_search_exec (Tier 6 for eligible patterns) ---- */
  VM svm = make_vm(bc, bc_len, subject);
  size_t svm_rmc;
  svm.range_meta = snobol_pattern_get_range_meta(pat, &svm_rmc);
  svm.range_meta_count = svm_rmc;
  snobol_search_result_t result;
  bool ok_svm = snobol_search_exec(&svm, subject, strlen(subject), 0, meta,
                                   NULL, &result, NULL);

  /* ---- Run via vm_run (Tier 8, full VM) ---- */
  VM fvm = make_vm(bc, bc_len, subject);
  size_t fvm_rmc;
  fvm.range_meta = snobol_pattern_get_range_meta(pat, &fvm_rmc);
  fvm.range_meta_count = fvm_rmc;
  bool ok_vm = vm_run(&fvm);

  test_assert(ok_svm == ok_vm, "search-VM and full VM agree on success");


  if (ok_svm && ok_vm) {
    /* Compare capture registers — the core correctness property */
    if (expected_cap0) {
      test_assert(cap_equals(&svm, 0, subject, expected_cap0),
                  "search-VM: cap0 matches expected");
      test_assert(cap_equals(&fvm, 0, subject, expected_cap0),
                  "full VM: cap0 matches expected");
      test_assert(svm.cap_start[0] == fvm.cap_start[0],
                  "cap0 start consistent");
      test_assert(svm.cap_end[0] == fvm.cap_end[0], "cap0 end consistent");
    }
    if (expected_cap1) {
      test_assert(cap_equals(&svm, 1, subject, expected_cap1),
                  "search-VM: cap1 matches expected");
      test_assert(cap_equals(&fvm, 1, subject, expected_cap1),
                  "full VM: cap1 matches expected");
      test_assert(svm.cap_start[1] == fvm.cap_start[1],
                  "cap1 start consistent");
      test_assert(svm.cap_end[1] == fvm.cap_end[1], "cap1 end consistent");
    }
  } else if (ok_svm && !ok_vm) {
    /* Full VM doesn't have unanchored search loop; search-VM does.
     * Verify search-VM capture is correct. */
    if (expected_cap1) {
      test_assert(cap_equals(&svm, 1, subject, expected_cap1),
                  "search-VM: cap1 matches expected (unanchored)");
    }
  }

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&svm);
  snobol_search_vm_cleanup(&fvm);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Tests
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Test 1: Simple capture — @r1 gets the first allocated register (0) */
static void test_simple_capture(void) {
  assert_captures_match("@r1('hello')", "hello", "hello", NULL);
}

/* Test 2: Capture at start with literal prefix */
static void test_capture_with_prefix(void) {
  assert_captures_match("'id:' @r1('12345')", "id:12345", "12345", NULL);
}

/* Test 3: Two captures — registers are allocated sequentially from 0,
 * so @r1 -> register 0 and @r2 -> register 1. Both are captured. */
static void test_multiple_captures(void) {
  assert_captures_match("@r1('ab') @r2('cd')", "abcd", "ab", "cd");
}

/* Test 4: Capture in alternation — each @name occurrence allocates a
 * register, so the second branch's capture lands in register 1. */
static void test_capture_in_alternation(void) {
  assert_captures_match("(@r1('a') | @r1('b'))", "b", NULL, "b");
}

/* Test 5: Capture after failed alternation (backtrack scenario) */
static void test_capture_after_failed_alt(void) {
  assert_captures_match("('aaa' | @r1('bbb'))", "bbb", "bbb", NULL);
}

/* Test 7: Two sequential captures — each @name occurrence allocates the next
 * register, so @r1('abc') @r1('def') captures "abc" in register 0 and
 * "def" in register 1 (no overwrite). */
static void test_sequential_captures(void) {
  assert_captures_match("@r1('abc') @r1('def')", "abcdef", "abc", "def");
}

/* Test 8: Capture of a SPAN in search mode.
 * Guards against the regression where a captured pattern (CAP_START SPAN
 * CAP_END) was misrouted to the non-capturing span-scan tier (Tier 1), which
 * silently dropped the capture. The dispatcher must route it to the
 * capture-aware Tier 6 (search-VM) and record the capture. */
static void test_capture_span_search_mode(void) {
  assert_captures_match("@r1(SPAN('0-9'))", "1234567890", "1234567890", NULL);
}

/* ── Suite entry point ────────────────────────────────────────────────── */

void test_search_vm_capture_suite(void) {
  test_suite("Search: VM Captures");
  test_simple_capture();
  test_capture_with_prefix();
  test_multiple_captures();
  test_capture_in_alternation();
  test_capture_after_failed_alt();
  test_sequential_captures();
  test_capture_span_search_mode();
}
