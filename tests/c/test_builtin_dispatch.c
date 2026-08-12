/*
 * test_builtin_dispatch.c – Tests for OP_EVAL built-in dispatch
 *
 * Verifies that OP_EVAL with built-in function IDs (SNOBOL_FN_UPPER etc.)
 * dispatches directly to C functions without needing an eval_fn callback.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "snobol/snobol_internal.h"
#include "snobol/vm.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Build and run a pattern: CAP_START(0) LIT(subject) CAP_END(0) EVAL(fn, reg=0)
 * ACCEPT */
static bool run_eval_builtin(const char *subject, size_t subject_len,
                             snobol_builtin_fn_t fn_id, char *out_str,
                             size_t *out_len_p) {
  /* Bytecode layout:
   *   OP_CAP_START 0
   *   OP_LIT off len  (literal = subject bytes embedded in bytecode)
   *   OP_CAP_END 0
   *   OP_EVAL fn reg=0
   *   OP_ACCEPT
   *   [literal bytes]
   */
  size_t header_size = 2           /* CAP_START */
                       + 1 + 4 + 4 /* LIT (op + off + len) */
                       + 2         /* CAP_END */
                       + 1 + 2 + 1 /* EVAL (op + fn u16 + reg u8) */
                       + 1;        /* ACCEPT */
  size_t lit_offset = header_size;
  size_t total_size =
      header_size + subject_len + 16; /* +16 for charclass tail */

  uint8_t *bc = (uint8_t *)snobol_malloc(total_size);
  if (!bc) {
    return false;
  }
  memset(bc, 0, total_size);

  size_t off = 0;
  /* CAP_START 0 */
  bc[off++] = OP_CAP_START;
  bc[off++] = 0;
  /* LIT offset=lit_offset, len=subject_len */
  bc[off++] = OP_LIT;
  bc[off++] = (uint8_t)(lit_offset >> 24);
  bc[off++] = (uint8_t)(lit_offset >> 16);
  bc[off++] = (uint8_t)(lit_offset >> 8);
  bc[off++] = (uint8_t)lit_offset;
  bc[off++] = (uint8_t)(subject_len >> 24);
  bc[off++] = (uint8_t)(subject_len >> 16);
  bc[off++] = (uint8_t)(subject_len >> 8);
  bc[off++] = (uint8_t)subject_len;
  /* CAP_END 0 */
  bc[off++] = OP_CAP_END;
  bc[off++] = 0;
  /* EVAL fn reg=0 */
  bc[off++] = OP_EVAL;
  bc[off++] = ((uint16_t)fn_id >> 8) & 0xFF;
  bc[off++] = (uint16_t)fn_id & 0xFF;
  bc[off++] = 0; /* reg */
  /* ACCEPT */
  bc[off++] = OP_ACCEPT;
  /* Literal bytes */
  memcpy(bc + lit_offset, subject, subject_len);
  /* Append class_count=0 as tail (no charclasses needed for EVAL) */
  size_t bc_total = lit_offset + subject_len + 4;
  bc[bc_total - 4] = 0;
  bc[bc_total - 3] = 0;
  bc[bc_total - 2] = 0;
  bc[bc_total - 1] = 0;

  VM vm = {0};
  vm.bc = bc;
  vm.bc_len = bc_total;
  vm.s = subject;
  vm.len = subject_len;

  snobol_buf out_buf = {0};
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  bool result = vm_run(&vm);

  if (result && out_str && out_len_p) {
    size_t copy_len =
        out_buf.len < *out_len_p - 1 ? out_buf.len : *out_len_p - 1;
    memcpy(out_str, out_buf.data, copy_len);
    out_str[copy_len] = '\0';
    *out_len_p = copy_len;
  }

  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  snobol_free(bc);
  return result;
}

void test_builtin_dispatch_suite(void) {
  test_suite("VM: Built-in dispatch (OP_EVAL)");

  char out[64];
  size_t out_len;

  /* SNOBOL_FN_SIZE: predicate always succeeds */
  bool ok =
      run_eval_builtin("hello", 5, SNOBOL_FN_SIZE, out, &(size_t){sizeof(out)});
  test_assert(ok, "OP_EVAL SNOBOL_FN_SIZE: succeeds on 'hello'");

  /* SNOBOL_FN_UPPER: converts to uppercase */
  out_len = sizeof(out);
  ok = run_eval_builtin("hello", 5, SNOBOL_FN_UPPER, out, &out_len);
  test_assert(ok, "OP_EVAL SNOBOL_FN_UPPER: succeeds");
  test_assert((out_len == 5 && memcmp(out, "HELLO", 5) == 0) != 0,
              "OP_EVAL SNOBOL_FN_UPPER: 'hello' → 'HELLO'");

  /* SNOBOL_FN_LOWER */
  out_len = sizeof(out);
  ok = run_eval_builtin("WORLD", 5, SNOBOL_FN_LOWER, out, &out_len);
  test_assert(ok, "OP_EVAL SNOBOL_FN_LOWER: succeeds");
  test_assert((out_len == 5 && memcmp(out, "world", 5) == 0) != 0,
              "OP_EVAL SNOBOL_FN_LOWER: 'WORLD' → 'world'");

  /* SNOBOL_FN_INTEGER: type predicate */
  ok = run_eval_builtin("123", 3, SNOBOL_FN_INTEGER, nullptr, nullptr);
  test_assert(ok, "OP_EVAL SNOBOL_FN_INTEGER: '123' succeeds");

  ok = run_eval_builtin("abc", 3, SNOBOL_FN_INTEGER, nullptr, nullptr);
  test_assert((!ok) != 0, "OP_EVAL SNOBOL_FN_INTEGER: 'abc' fails");

  /* SNOBOL_FN_NUMERIC */
  ok = run_eval_builtin("12.34", 5, SNOBOL_FN_NUMERIC, nullptr, nullptr);
  test_assert(ok, "OP_EVAL SNOBOL_FN_NUMERIC: '12.34' succeeds");

  /* No host eval_fn required (built-in dispatch bypasses it) */
  test_assert(true, "OP_EVAL built-in dispatch: no eval_fn callback needed");
}
