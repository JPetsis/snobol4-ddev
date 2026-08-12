/*
 * test_pattern_breakx.c – Tests for OP_BREAKX pattern primitive
 *
 * BREAKX pre-scan optimization: like BREAK but pushes retry choices so
 * that backtracking extends to the *next* occurrence of the break char.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/compiler.h"
#include "snobol/snobol_internal.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Helper: build minimal bytecode with OP_BREAKX + OP_CAP_START/END + OP_ACCEPT
 */
static bool run_breakx_match(const char *subject, size_t subj_len,
                             const char *break_chars, size_t bc_len_arg,
                             size_t *out_end_pos) {
  /* Compile a simple pattern: CAP_START(0) BREAKX(';') CAP_END(0) ACCEPT */
  /* We'll use snobol_pattern_compile via the public API */
  (void)bc_len_arg;
  (void)break_chars;
  (void)out_end_pos;

  /* Build bytecode manually for BREAKX test */
  /* This tests the VM directly */
  uint8_t bc_buf[64];
  size_t bc_off = 0;

  /* OP_CAP_START reg=0 */
  bc_buf[bc_off++] = OP_CAP_START;
  bc_buf[bc_off++] = 0;

  /* OP_BREAKX set_id=1 (set_id=1 → single range 0x3B-0x3B for ';') */
  bc_buf[bc_off++] = OP_BREAKX;
  bc_buf[bc_off++] = 0; /* set_id high byte */
  bc_buf[bc_off++] = 1; /* set_id = 1 */

  /* OP_CAP_END reg=0 */
  bc_buf[bc_off++] = OP_CAP_END;
  bc_buf[bc_off++] = 0;

  /* OP_ACCEPT */
  bc_buf[bc_off++] = OP_ACCEPT;

  /* Append charclass table at the end of bytecode.
   * Format: [range_data...][offset_table][class_count u32]
   * For set_id=1: one range [0x3B, 0x3B] (semicolon)
   * range_data: 8 bytes per range (start u32, end u32)
   * offset_table: 4 bytes per class pointing to range_data start
   * class_count: u32 = 1
   */
  size_t range_off = bc_off; /* offset where range data starts */
  /* Range: start=0x3B, end=0x3B */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0x3B; /* start = 59 */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0x3B; /* end = 59 */
  /* Offset table for class 1 → points to range_off */
  uint32_t off_val = (uint32_t)range_off;
  bc_buf[bc_off++] = (off_val >> 24) & 0xFF;
  bc_buf[bc_off++] = (off_val >> 16) & 0xFF;
  bc_buf[bc_off++] = (off_val >> 8) & 0xFF;
  bc_buf[bc_off++] = off_val & 0xFF;
  /* The range count field is AFTER the range itself; offset_table entry
   * points to: count u16, case u16, then ranges.  Rebuild to proper format. */
  /* Actually that format is more complex; let me use a simpler indicator:
   * the charclass format stores count+case as u16+u16 before range bytes.
   * So the range entry at range_off should be: count=1, case=0, then 8 bytes.
   */
  /* Rebuild: */
  bc_off = range_off;
  /* count=1 u16 */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 1;
  /* case=0 u16 */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  /* range: start=0x3B, end=0x3B (8 bytes) */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0x3B;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0x3B;
  /* offset table entry for class 1 (points to range_off) */
  bc_buf[bc_off++] = (range_off >> 24) & 0xFF;
  bc_buf[bc_off++] = (range_off >> 16) & 0xFF;
  bc_buf[bc_off++] = (range_off >> 8) & 0xFF;
  bc_buf[bc_off++] = range_off & 0xFF;
  /* class_count u32 = 1 */
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 0;
  bc_buf[bc_off++] = 1;

  VM vm = {};
  vm.bc = bc_buf;
  vm.bc_len = bc_off;
  vm.s = subject;
  vm.len = subj_len;

  snobol_buf out_buf = {};
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  bool result = vm_run(&vm);
  if (result && out_end_pos) {
    *out_end_pos = vm.cap_end[0];
  }
  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  return result;
}

void test_pattern_breakx_suite(void) {
  test_suite("Pattern: BREAKX");

  /* Basic: BREAKX on "token1;token2" should match "token1" first */
  size_t end_pos = 0;
  bool ok = run_breakx_match("token1;token2", 13, ";", 1, &end_pos);
  test_assert(ok, "BREAKX matches up to first semicolon");
  test_assert(end_pos == 6, "BREAKX capture end = 6 (before ';')");

  /* BREAKX on string with no break char: matches whole string */
  ok = run_breakx_match("nocolon", 7, ";", 1, &end_pos);
  test_assert(ok, "BREAKX matches whole string when no break char present");
  test_assert(end_pos == 7, "BREAKX capture end = 7 (whole string)");

  /* BREAKX on empty string: matches empty */
  ok = run_breakx_match("", 0, ";", 1, &end_pos);
  test_assert(ok, "BREAKX matches empty string");
}
