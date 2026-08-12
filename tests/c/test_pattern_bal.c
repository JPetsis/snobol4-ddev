/*
 * test_pattern_bal.c – Tests for OP_BAL balanced pattern primitive
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Build and run a BAL pattern: CAP_START BAL(open,close) CAP_END ACCEPT */
static bool run_bal_match(const char *subject, size_t subj_len,
                          uint32_t open_cp, uint32_t close_cp,
                          size_t *out_cap_start, size_t *out_cap_end) {
  uint8_t bc_buf[32];
  size_t bc_off = 0;

  /* OP_CAP_START 0 */
  bc_buf[bc_off++] = OP_CAP_START;
  bc_buf[bc_off++] = 0;

  /* OP_BAL open_cp close_cp */
  bc_buf[bc_off++] = OP_BAL;
  bc_buf[bc_off++] = (open_cp >> 24) & 0xFF;
  bc_buf[bc_off++] = (open_cp >> 16) & 0xFF;
  bc_buf[bc_off++] = (open_cp >> 8) & 0xFF;
  bc_buf[bc_off++] = open_cp & 0xFF;
  bc_buf[bc_off++] = (close_cp >> 24) & 0xFF;
  bc_buf[bc_off++] = (close_cp >> 16) & 0xFF;
  bc_buf[bc_off++] = (close_cp >> 8) & 0xFF;
  bc_buf[bc_off++] = close_cp & 0xFF;

  /* OP_CAP_END 0 */
  bc_buf[bc_off++] = OP_CAP_END;
  bc_buf[bc_off++] = 0;

  /* OP_ACCEPT */
  bc_buf[bc_off++] = OP_ACCEPT;

  VM vm = {};
  vm.bc = bc_buf;
  vm.bc_len = bc_off;
  vm.s = subject;
  vm.len = subj_len;

  snobol_buf out_buf = {};
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  bool result = vm_run(&vm);
  if (result) {
    if (out_cap_start) {
      *out_cap_start = vm.cap_start[0];
    }
    if (out_cap_end) {
      *out_cap_end = vm.cap_end[0];
    }
  }
  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  return result;
}

void test_pattern_bal_suite(void) {
  test_suite("Pattern: BAL");

  size_t cs = 0;
  size_t ce = 0;

  /* Balanced parentheses */
  bool ok = run_bal_match("(a + b)", 7, '(', ')', &cs, &ce);
  test_assert(ok, "BAL matches '(a + b)'");
  test_assert(ce - cs == 7, "BAL capture covers full balanced expression");

  /* Nested balanced parentheses */
  ok = run_bal_match("(a + (b * c))", 13, '(', ')', &cs, &ce);
  test_assert(ok, "BAL matches nested '(a + (b * c))'");
  test_assert(ce - cs == 13, "BAL capture covers nested balanced expression");

  /* Unbalanced input: more opens than closes */
  ok = run_bal_match("(a + (b * c)", 12, '(', ')', &cs, &ce);
  test_assert((!ok) != 0, "BAL fails on unbalanced input (extra open)");

  /* Subject does not start with open delimiter */
  ok = run_bal_match("a + b", 5, '(', ')', &cs, &ce);
  test_assert((!ok) != 0,
              "BAL fails when subject does not start with open delimiter");

  /* Balanced brackets */
  ok = run_bal_match("[inner]", 7, '[', ']', &cs, &ce);
  test_assert(ok, "BAL matches balanced brackets");
}
