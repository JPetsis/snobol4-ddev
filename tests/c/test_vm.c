/*
 * test_vm.c - VM-level tests
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);


/* ===== test_coverage_vm_exec: coverage-driven tests merged into test_vm.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/array.h"
#include "../../core/include/snobol/snobol_internal.h"
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/dynamic_pattern.h"
#include "../../core/include/snobol/table.h"
#include "../../core/include/snobol/vm.h"


/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void covv_emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

static void covv_emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

/* Compile an AST and run it through the FULL VM (vm_exec → vm_run). */
static bool covv_run_ast(ast_node_t *root, const char *subject, size_t len,
                         int *out_pos, int *out_caps, VM *out_vm,
                         snobol_buf *out_buf) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  if (compile_ast_to_bytecode_c(root, false, &bc, &bc_len) != 0) {
    return false;
  }
  if (!bc || bc_len == 0) {
    free(bc);
    return false;
  }

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = len;
  snobol_buf ob;
  snobol_buf_init(&ob);
  vm.out = &ob;

  bool ok = vm_exec(&vm);
  if (out_pos) {
    *out_pos = (int)vm.pos;
  }
  if (out_caps) {
    *out_caps = (int)vm.var_count;
  }
  if (out_vm) {
    *out_vm = vm;
  } else {
    vm_free_labels(&vm);
  }
  if (out_buf) {
    *out_buf = ob;
  } else {
    snobol_buf_free(&ob);
  }
  free(bc);
  return ok;
}

static bool covv_run_bc(const uint8_t *bc, size_t bc_len, const char *subject,
                        size_t len, int *out_pos, VM *vm_out) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = len;
  snobol_buf ob;
  snobol_buf_init(&ob);
  vm.out = &ob;

  bool ok = vm_exec(&vm);
  if (out_pos) {
    *out_pos = (int)vm.pos;
  }
  if (vm_out) {
    *vm_out = vm;
  } else {
    vm_free_labels(&vm);
  }
  snobol_buf_free(&ob);
  return ok;
}

/* ── Non-ASCII / multi-byte charclass branches ────────────────────────────── */

void test_cov_vm_charclass_utf8(void) {
  test_suite("Coverage: full-VM UTF-8 charclass branches");

  /* SPAN('€') — non-ASCII class → op_span else-branch. */
  {
    const char *euro = "\xE2\x82\xAC";
    ast_node_t *ast = snobol_ast_create_span(euro, 3);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "SPAN('€') fails on ASCII subject");
    snobol_ast_free(ast);

    ast = snobol_ast_create_span(euro, 3);
    ok = covv_run_ast(ast, "\xE2\x82\xACx", 4, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 3) != 0,
                "SPAN('€') consumes the 3-byte codepoint");
    snobol_ast_free(ast);
  }

  /* ANY('€') — non-ASCII class → op_any else-branch (success + fail). */
  {
    const char *euro = "\xE2\x82\xAC";
    ast_node_t *ast = snobol_ast_create_any(euro, 3);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "x", 1, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "ANY('€') fails on 'x'");
    snobol_ast_free(ast);

    ast = snobol_ast_create_any(euro, 3);
    ok = covv_run_ast(ast, "\xE2\x82\xAC", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 3) != 0, "ANY('€') matches the codepoint");
    snobol_ast_free(ast);
  }

  /* NOTANY('€') — non-ASCII class (match + reject). */
  {
    const char *euro = "\xE2\x82\xAC";
    ast_node_t *ast = snobol_ast_create_notany(euro, 3);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "x", 1, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 1) != 0, "NOTANY('€') matches 'x'");
    snobol_ast_free(ast);

    ast = snobol_ast_create_notany(euro, 3);
    ok = covv_run_ast(ast, "\xE2\x82\xAC", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "NOTANY('€') rejects the codepoint");
    snobol_ast_free(ast);
  }

  /* BREAK('€') — non-ASCII class → op_break else-branch. */
  {
    const char *euro = "\xE2\x82\xAC";
    ast_node_t *ast = snobol_ast_create_break(euro, 3);
    int pos = 0;
    int caps = 0;
    bool ok =
        covv_run_ast(ast, "ab\xE2\x82\xACxy", 6, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 2) != 0, "BREAK('€') stops before the codepoint");
    snobol_ast_free(ast);

    ast = snobol_ast_create_break(euro, 3);
    ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 3) != 0,
                "BREAK('€') consumes to end when absent");
    snobol_ast_free(ast);
  }
}


void test_cov_vm_break_variants(void) {
  test_suite("Coverage: full-VM BREAK scan variants");

  /* BREAK with a multi-char ASCII class → bitmap loop (not single-byte). */
  ast_node_t *ast = snobol_ast_create_break("abc", 3);
  int pos = 0;
  int caps = 0;
  bool ok = covv_run_ast(ast, "zzzaxx", 6, &pos, &caps, nullptr, nullptr);
  test_assert((ok && pos == 3) != 0, "BREAK('abc') bitmap scan stops at 'a'");
  snobol_ast_free(ast);

  /* BREAK with single-byte class, delimiter absent → memchr NULL path. */
  ast = snobol_ast_create_break(",", 1);
  ok = covv_run_ast(ast, "aaaa", 4, &pos, &caps, nullptr, nullptr);
  test_assert((ok && pos == 4) != 0, "BREAK(',') memchr miss consumes to end");
  snobol_ast_free(ast);

  /* BREAK single-byte class with delimiter → memchr hit. */
  ast = snobol_ast_create_break(",", 1);
  ok = covv_run_ast(ast, "aaa,bbb", 7, &pos, &caps, nullptr, nullptr);
  test_assert((ok && pos == 3) != 0, "BREAK(',') memchr hit stops at comma");
  snobol_ast_free(ast);
}

/* ── Position ops ─────────────────────────────────────────────────────────── */


void test_cov_vm_position_ops(void) {
  test_suite("Coverage: full-VM REM/RPOS/RTAB/POS/TAB");

  /* REM: consume the remainder. */
  ast_node_t *ast = snobol_ast_create_rem();
  int pos = 0;
  int caps = 0;
  bool ok = covv_run_ast(ast, "hello", 5, &pos, &caps, nullptr, nullptr);
  test_assert((ok && pos == 5) != 0, "REM consumes the remainder");
  snobol_ast_free(ast);

  /* RPOS(0) after LEN(5) on "abcde" → at end, ok. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(5);
    parts[1] = snobol_ast_create_rpos(0);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abcde", 5, &pos, &caps, nullptr, nullptr);
    test_assert(ok, "RPOS(0) at end succeeds");
    snobol_ast_free(ast);

    /* RPOS(2) after LEN(2) on "abcde" → pos=2, 3 remain → fail. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(2);
    parts[1] = snobol_ast_create_rpos(2);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abcde", 5, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "RPOS(2) fails at pos 2");
    snobol_ast_free(ast);

    /* RPOS(1) on multibyte subject — continuation-byte walk. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(1);
    parts[1] = snobol_ast_create_rpos(1);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "a\xE2\x82\xAC", 4, &pos, &caps, nullptr, nullptr);
    test_assert(ok, "RPOS(1) skips UTF-8 continuation bytes");
    snobol_ast_free(ast);
  }

  /* RTAB(2) on "abcde" → pos=3; RTAB(9) fails. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_rtab(2);
    parts[1] = snobol_ast_create_rem();
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abcde", 5, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 5) != 0, "RTAB(2) advances to 2-from-end");
    snobol_ast_free(ast);

    /* RTAB(9) after LEN(1): target 0 < pos 1 → fail. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(1);
    parts[1] = snobol_ast_create_rtab(9);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abcde", 5, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "RTAB(9) fails when pos is past the target");
    snobol_ast_free(ast);
  }

  /* POS(0) succeeds at 0; POS(1) at pos 0 fails. */
  ast = snobol_ast_create_pos(0);
  ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
  test_assert(ok, "POS(0) at start succeeds");
  snobol_ast_free(ast);

  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_pos(1);
    parts[1] = snobol_ast_create_lit("a", 1);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "POS(1) at pos 0 fails");
    snobol_ast_free(ast);
  }

  /* TAB(2) then LEN(2) on "abcde" → pos=2 then 4. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_tab(2);
    parts[1] = snobol_ast_create_len(2);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abcde", 5, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 4) != 0, "TAB(2) advances to byte offset 2");
    snobol_ast_free(ast);

    /* TAB(9) beyond subject → fail. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_tab(9);
    parts[1] = snobol_ast_create_lit("a", 1);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "TAB(9) beyond subject fails");
    snobol_ast_free(ast);

    /* TAB(1) after LEN(2): pos already past target → fail. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(2);
    parts[1] = snobol_ast_create_tab(1);
    ast = snobol_ast_create_concat(parts, 2);
    ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "TAB(1) after pos 2 fails");
    snobol_ast_free(ast);
  }
}

/* ── Captures / assign / LEN / EVAL / ANCHOR ──────────────────────────────── */


void test_cov_vm_caps_assign(void) {
  test_suite("Coverage: full-VM capture/assign paths");

  /* CAP_END without CAP_START: max_cap_used update + var exposure. */
  {
    uint8_t bc[8] = {OP_CAP_END, 0, OP_ACCEPT};
    VM vm;
    int pos = 0;
    bool ok = covv_run_bc(bc, 3, "ab", 2, &pos, &vm);
    test_assert(ok, "CAP_END alone succeeds");
    test_assert(vm.max_cap_used == 1, "CAP_END bumps max_cap_used");
    test_assert(vm.var_count == 1, "CAP_END exposes variable register");
    vm_free_labels(&vm);
  }

  /* CAP + ASSIGN: var_count grows past existing registers. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("ab", 2));
    parts[1] = snobol_ast_create_assign(3, 0);
    parts[2] = snobol_ast_create_lit("c", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 3);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 3) != 0, "capture+assign pattern matches");
    test_assert(caps == 4, "assign to var 3 sets var_count to 4");
    snobol_ast_free(ast);
  }

  /* LEN beyond subject → fail path. */
  {
    ast_node_t *ast = snobol_ast_create_len(5);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "ab", 2, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "LEN(5) on 2-byte subject fails");
    snobol_ast_free(ast);
  }
}

static bool covv_eval_callback(int fn_id, const char *s, size_t start,
                               size_t end, void *udata) {
  (void)fn_id;
  (void)s;
  (void)start;
  (void)end;
  int *count = (int *)udata;
  (*count)++;
  return true;
}

static void covv_emit_cb(const char *data, size_t len, void *udata);


void test_cov_vm_eval(void) {
  test_suite("Coverage: full-VM EVAL dispatch paths");

  /* Builtin predicates (INTEGER/REAL/NUMERIC) + transforms with output. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("123", 3));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_INTEGER, 0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "123", 3, &pos, &caps, nullptr, nullptr);
    test_assert(ok, "EVAL(INTEGER) succeeds on '123'");
    snobol_ast_free(ast);
  }
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("abc", 3));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_NUMERIC, 0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "EVAL(NUMERIC) fails on 'abc'");
    snobol_ast_free(ast);
  }
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("1.5", 3));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_REAL, 0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "1.5", 3, &pos, &caps, nullptr, nullptr);
    test_assert(ok, "EVAL(REAL) succeeds on '1.5'");
    snobol_ast_free(ast);
  }

  /* TRIM with vm->out + emit_fn. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("x ", 2));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_TRIM, 0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    VM vm;
    snobol_buf out;
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc == 0 && bc) != 0, "EVAL TRIM compiles");
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "x ";
    vm.len = 2;
    snobol_buf_init(&out);
    vm.out = &out;
    snobol_buf cb_buf;
    snobol_buf_init(&cb_buf);
    vm.emit_fn = covv_emit_cb;
    vm.emit_udata = &cb_buf;
    bool ok = vm_exec(&vm);
    test_assert(ok, "EVAL(TRIM) succeeds");
    test_assert((out.len == 1 && out.data[0] == 'x') != 0,
                "TRIM output appended");
    test_assert((cb_buf.len == 1 && cb_buf.data[0] == 'x') != 0,
                "emit_fn invoked by EVAL");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    snobol_buf_free(&cb_buf);
    free(bc);
    snobol_ast_free(ast);
  }

  /* Multi-arg builtin (DUPL) falls back to the host eval_fn callback. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_cap(0, snobol_ast_create_lit("ab", 2));
    parts[1] = snobol_ast_create_eval(SNOBOL_FN_DUPL, 0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc == 0 && bc) != 0, "EVAL DUPL compiles");
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "ab";
    vm.len = 2;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    int cb_count = 0;
    vm.eval_fn = covv_eval_callback;
    vm.eval_udata = &cb_count;
    bool ok = vm_exec(&vm);
    test_assert((ok && cb_count == 1) != 0,
                "unknown builtin routed to eval_fn");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    free(bc);
    snobol_ast_free(ast);
  }

  /* fn == 0 → host callback path (not builtin dispatch). */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    covv_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covv_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EVAL;
    covv_emit_u16_be(bc, &ip, 0); /* fn = SNOBOL_FN_NONE */
    bc[ip++] = 0;                 /* reg */
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "a";
    vm.len = 1;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    int cb_count = 0;
    vm.eval_fn = covv_eval_callback;
    vm.eval_udata = &cb_count;
    bool ok = vm_exec(&vm);
    test_assert((ok && cb_count == 1) != 0, "EVAL(fn=0) uses host callback");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }
}


void test_cov_vm_anchor_fail(void) {
  test_suite("Coverage: full-VM ANCHOR fail paths");

  /* ANCHOR(0) after consuming → fail. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(1);
    parts[1] = snobol_ast_create_anchor(0);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "ab", 2, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "ANCHOR(start) at pos 1 fails");
    snobol_ast_free(ast);
  }

  /* ANCHOR(1) at pos 0 (not at end) → fail. */
  {
    ast_node_t *ast = snobol_ast_create_anchor(1);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "ab", 2, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "ANCHOR(end) at pos 0 fails");
    snobol_ast_free(ast);
  }

  /* ANCHOR(0) at pos 0 succeeds. */
  {
    ast_node_t *ast = snobol_ast_create_anchor(0);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "ab", 2, &pos, &caps, nullptr, nullptr);
    test_assert(ok, "ANCHOR(start) at pos 0 succeeds");
    snobol_ast_free(ast);
  }
}

/* ── REPEAT paths ─────────────────────────────────────────────────────────── */


void test_cov_vm_repeat(void) {
  test_suite("Coverage: full-VM REPEAT_INIT/STEP paths");

  /* min=2 max=2: count<min jump path, then max-bound fallthrough. */
  {
    ast_node_t *ast =
        snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 2, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "aa", 2, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 2) != 0, "repeat(2,2) matches 'aa'");
    snobol_ast_free(ast);
  }

  /* min=0: zero-iteration skip choice pushed (SPLIT backtracks). */
  {
    ast_node_t *ast =
        snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 0, 1);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "b", 1, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 0) != 0, "repeat(0,1) zero-iteration succeeds");
    snobol_ast_free(ast);
  }

  /* Unbounded repeat: zero-progress exit (''*) in O(1). */
  {
    ast_node_t *ast =
        snobol_ast_create_repeat(snobol_ast_create_lit("", 0), 1, (int32_t)-1);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 0) != 0,
                "empty-body repeat exits without looping");
    snobol_ast_free(ast);
  }

  /* Greedy-span optimisation: ARBNO(SPAN('a')) + 'b' style via 'a'* 'b'. */
  {
    ast_node_t *ast =
        snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 1, (int32_t)-1);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "aaa", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 3) != 0, "unbounded repeat consumes the run");
    snobol_ast_free(ast);
  }

  /* Repeat with a failing tail: choice pop restores then accept. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] =
        snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 1, (int32_t)-1);
    parts[1] = snobol_ast_create_lit("b", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "aaab", 4, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 4) != 0, "repeat+tail matches via backtracking");
    snobol_ast_free(ast);
  }
}

/* ── EMIT opcodes ─────────────────────────────────────────────────────────── */

static void covv_emit_cb(const char *data, size_t len, void *udata) {
  snobol_buf *b = (snobol_buf *)udata;
  snobol_buf_append(b, data, len);
}


void test_cov_vm_emit(void) {
  test_suite("Coverage: full-VM EMIT opcode handlers");

  /* EMIT_LITERAL with out + emit_fn. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_EMIT_LITERAL;
    covv_emit_u32_be(bc, &ip, 12);
    covv_emit_u32_be(bc, &ip, 3);
    bc[ip++] = OP_ACCEPT;
    bc[12] = 'X';
    bc[13] = 'Y';
    bc[14] = 'Z';
    size_t bc_len = 15;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "";
    vm.len = 0;
    snobol_buf out;
    snobol_buf cb_buf;
    snobol_buf_init(&out);
    snobol_buf_init(&cb_buf);
    vm.out = &out;
    vm.emit_fn = covv_emit_cb;
    vm.emit_udata = &cb_buf;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len == 3) != 0, "EMIT_LITERAL appends to out");
    test_assert(cb_buf.len == 3, "EMIT_LITERAL invokes emit_fn");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    snobol_buf_free(&cb_buf);
  }

  /* AST_EMIT literal with an embedded NUL byte must compile and run
   * byte-exact (regression: emit_emit_c used strlen() on the text, which
   * truncated at the NUL — the AST now carries the exact byte length). */
  {
    static const char nul_payload[] = "\x61\x00\x62"; /* 'a', NUL, 'b' */
    ast_node_t *emit = snobol_ast_create_emit(nul_payload, 3, -1);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    test_assert(compile_ast_to_bytecode_c(emit, false, &bc, &bc_len) == 0,
                "NUL emit compiles");
    snobol_ast_free(emit);

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "";
    vm.len = 0;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len == 3 && memcmp(out.data, nul_payload, 3) == 0) !=
                    0,
                "NUL emit output is byte-exact");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    free(bc);
  }

  /* EMIT_CAPTURE with populated capture. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    covv_emit_u32_be(bc, &ip, 20); /* data beyond the code */
    covv_emit_u32_be(bc, &ip, 2);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_CAPTURE;
    bc[ip++] = 0;
    bc[ip++] = OP_ACCEPT;
    bc[20] = 'a';
    bc[21] = 'b';
    size_t bc_len = 22;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "ab";
    vm.len = 2;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len == 2 && out.data[0] == 'a') != 0,
                "EMIT_CAPTURE appends capture");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EMIT_EXPR legacy: expr_type 1 (upper), 2 (length), 3 (raw). */
  for (int et = 1; et <= 3; et++) {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    covv_emit_u32_be(bc, &ip, 30);
    covv_emit_u32_be(bc, &ip, 3);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_EXPR;
    bc[ip++] = 0; /* reg */
    bc[ip++] = (uint8_t)et;
    bc[ip++] = OP_ACCEPT;
    bc[30] = 'a';
    bc[31] = 'b';
    bc[32] = 'c';
    size_t bc_len = 33;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "abc";
    vm.len = 3;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len > 0) != 0, "EMIT_EXPR emits");
    if (et == 1) {
      test_assert((out.len == 3 && out.data[0] == 'A') != 0,
                  "EMIT_EXPR upper-cases");
    } else if (et == 2) {
      test_assert(strncmp(out.data, "3", out.len) == 0,
                  "EMIT_EXPR emits length");
    } else {
      test_assert((out.len == 3 && out.data[0] == 'a') != 0, "EMIT_EXPR raw");
    }
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EMIT_FORMAT: upper, lower, length, lpad, rpad + width cap + missing cap. */
  {
    /* UPPER/LOWER/LENGTH */
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    covv_emit_u32_be(bc, &ip, 40);
    covv_emit_u32_be(bc, &ip, 3);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_UPPER;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_LOWER;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_LENGTH;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_LPAD;
    covv_emit_u16_be(bc, &ip, 5); /* width */
    bc[ip++] = '-';               /* fill */
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_RPAD;
    covv_emit_u16_be(bc, &ip, 5);
    bc[ip++] = '*';
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = 99; /* unknown → raw */
    bc[ip++] = OP_ACCEPT;
    bc[40] = 'a';
    bc[41] = 'b';
    bc[42] = 'c';
    size_t bc_len = 43;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "abc";
    vm.len = 3;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert(ok, "EMIT_FORMAT chain succeeds");
    /* ABC abc 3 --abc abc** abc (LPAD 5 = "--abc", RPAD 5 = "abc**") */
    const char *expected = "ABCabc3--abcabc**abc";
    test_assert((out.len == strlen(expected) &&
                 memcmp(out.data, expected, out.len) == 0) != 0,
                "EMIT_FORMAT outputs match");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EMIT_FORMAT LPAD width > 1024 → capped at 1024; reg >= MAX_CAPS is
   * a genuinely missing capture → nothing emitted. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0; /* empty-but-valid capture → padded to capped width */
    bc[ip++] = SNBL_FMT_LPAD;
    covv_emit_u16_be(bc, &ip, 0xFFFFU); /* width > 1024 */
    bc[ip++] = '0';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "";
    vm.len = 0;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert(ok, "EMIT_FORMAT padded capture succeeds");
    test_assert(out.len == 1024, "LPAD width capped at 1024");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 200; /* reg >= MAX_CAPS → missing capture */
    bc[ip++] = SNBL_FMT_LPAD;
    covv_emit_u16_be(bc, &ip, 5);
    bc[ip++] = '0';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "";
    vm.len = 0;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert(ok, "EMIT_FORMAT missing capture succeeds");
    test_assert(out.len == 0, "missing capture emits nothing");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }
}

/* ── TABLE / ARRAY ops via crafted bytecode ───────────────────────────────── */


void test_cov_vm_table_array(void) {
  test_suite("Coverage: full-VM TABLE_GET/SET + ARRAY_GET/SET");

  /* TABLE_GET success + ARRAY ops via AST-built bytecode with registries. */
  uint8_t bc[256];
  size_t ip = 0;
  /* CAP_START(0) LIT('k') CAP_END(0) TABLE_GET(0, 0, 1, name_len=1,'t')
   * TABLE_SET(0, 0, 0, 1, 't') ACCEPT — table must contain "k". */
  bc[ip++] = OP_CAP_START;
  bc[ip++] = 0;
  bc[ip++] = OP_LIT;
  covv_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  covv_emit_u32_be(bc, &ip, 1);
  bc[ip++] = 'k';
  bc[ip++] = OP_CAP_END;
  bc[ip++] = 0;
  bc[ip++] = OP_TABLE_GET;
  covv_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0; /* key_reg */
  bc[ip++] = 1; /* dest_reg */
  bc[ip++] = 1; /* name_len */
  bc[ip++] = 't';
  bc[ip++] = OP_TABLE_SET;
  covv_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0; /* key_reg */
  bc[ip++] = 0; /* value_reg */
  bc[ip++] = 1; /* name_len */
  bc[ip++] = 't';
  bc[ip++] = OP_ARRAY_GET;
  covv_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0; /* key_reg */
  bc[ip++] = 1; /* dest_reg */
  bc[ip++] = 1; /* name_len */
  bc[ip++] = 'a';
  bc[ip++] = OP_ARRAY_SET;
  covv_emit_u16_be(bc, &ip, 0);
  bc[ip++] = 0; /* key_reg */
  bc[ip++] = 0; /* value_reg */
  bc[ip++] = 1; /* name_len */
  bc[ip++] = 'a';
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  snobol_table_t *table = table_create("t");
  snobol_array_t *array = snobol_array_create(16);
  test_assert((table && array) != 0, "table/array created");
  test_assert(table_set(table, "k", "v"), "table seed set");
  test_assert(snobol_array_set(array, 0, "v0"), "array seed set");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = "k";
  vm.len = 1;
  vm_init_tables(&vm);
  vm_init_arrays(&vm);
  uint16_t tid = 0;
  uint16_t aid = 0;
  test_assert((vm_register_table(&vm, table, &tid) && tid == 0) != 0,
              "table registered as id 0");
  test_assert((vm_register_array(&vm, array, &aid) && aid == 0) != 0,
              "array registered as id 0");
  bool ok = vm_exec(&vm);
  test_assert(ok, "TABLE/ARRAY GET+SET chain succeeds");
  test_assert(vm_get_table(&vm, 0) == table, "vm_get_table returns table");
  test_assert(vm_get_table(&vm, 99) == NULL, "vm_get_table bad id NULL");
  test_assert(vm_get_array(&vm, 0) == array, "vm_get_array returns array");
  test_assert(vm_get_array(&vm, 99) == NULL, "vm_get_array bad id NULL");
  vm_free_labels(&vm);
  vm_free_tables(&vm);
  vm_free_arrays(&vm);
  table_release(table);
  snobol_array_release(array);

  /* TABLE_GET with a missing key → backtrack-fail. */
  {
    uint8_t bc2[64];
    size_t ip2 = 0;
    bc2[ip2++] = OP_CAP_START;
    bc2[ip2++] = 0;
    bc2[ip2++] = OP_LIT;
    covv_emit_u32_be(bc2, &ip2, (uint32_t)(ip2 + 8));
    covv_emit_u32_be(bc2, &ip2, 1);
    bc2[ip2++] = 'z';
    bc2[ip2++] = OP_CAP_END;
    bc2[ip2++] = 0;
    bc2[ip2++] = OP_TABLE_GET;
    covv_emit_u16_be(bc2, &ip2, 0);
    bc2[ip2++] = 0;
    bc2[ip2++] = 1;
    bc2[ip2++] = 1;
    bc2[ip2++] = 't';
    bc2[ip2++] = OP_ACCEPT;
    size_t len2 = ip2;

    VM vm2;
    memset(&vm2, 0, sizeof(vm2));
    vm2.bc = bc2;
    vm2.bc_len = len2;
    vm2.s = "z";
    vm2.len = 1;
    vm_init_tables(&vm2);
    table = table_create("t");
    uint16_t t2 = 0;
    vm_register_table(&vm2, table, &t2);
    bool ok2 = vm_exec(&vm2);
    test_assert((!ok2) != 0, "TABLE_GET missing key fails");
    vm_free_labels(&vm2);
    vm_free_tables(&vm2);
    table_release(table);
  }

  /* TABLE_GET with invalid table id → fail. */
  {
    uint8_t bc3[64];
    size_t ip3 = 0;
    bc3[ip3++] = OP_CAP_START;
    bc3[ip3++] = 0;
    bc3[ip3++] = OP_LIT;
    covv_emit_u32_be(bc3, &ip3, (uint32_t)(ip3 + 8));
    covv_emit_u32_be(bc3, &ip3, 1);
    bc3[ip3++] = 'k';
    bc3[ip3++] = OP_CAP_END;
    bc3[ip3++] = 0;
    bc3[ip3++] = OP_TABLE_GET;
    covv_emit_u16_be(bc3, &ip3, 7); /* unregistered id */
    bc3[ip3++] = 0;
    bc3[ip3++] = 1;
    bc3[ip3++] = 1;
    bc3[ip3++] = 't';
    bc3[ip3++] = OP_ACCEPT;
    VM vm3;
    memset(&vm3, 0, sizeof(vm3));
    vm3.bc = bc3;
    vm3.bc_len = ip3;
    vm3.s = "k";
    vm3.len = 1;
    vm_init_tables(&vm3);
    bool ok3 = vm_exec(&vm3);
    test_assert((!ok3) != 0, "TABLE_GET invalid table fails");
    vm_free_labels(&vm3);
    vm_free_tables(&vm3);
  }

  /* ARRAY_GET with missing key → fail. */
  {
    uint8_t bc4[64];
    size_t ip4 = 0;
    bc4[ip4++] = OP_CAP_START;
    bc4[ip4++] = 0;
    bc4[ip4++] = OP_LIT;
    covv_emit_u32_be(bc4, &ip4, (uint32_t)(ip4 + 8));
    covv_emit_u32_be(bc4, &ip4, 1);
    bc4[ip4++] = '5'; /* key "5" → index 5 */
    bc4[ip4++] = OP_CAP_END;
    bc4[ip4++] = 0;
    bc4[ip4++] = OP_ARRAY_GET;
    covv_emit_u16_be(bc4, &ip4, 0);
    bc4[ip4++] = 0;
    bc4[ip4++] = 1;
    bc4[ip4++] = 1;
    bc4[ip4++] = 'a';
    bc4[ip4++] = OP_ACCEPT;
    VM vm4;
    memset(&vm4, 0, sizeof(vm4));
    vm4.bc = bc4;
    vm4.bc_len = ip4;
    vm4.s = "5";
    vm4.len = 1;
    vm_init_arrays(&vm4);
    array = snobol_array_create(4);
    uint16_t a4 = 0;
    vm_register_array(&vm4, array, &a4);
    bool ok4 = vm_exec(&vm4);
    test_assert((!ok4) != 0, "ARRAY_GET missing key fails");
    vm_free_labels(&vm4);
    vm_free_arrays(&vm4);
    snobol_array_release(array);
  }
}

/* ── DYNAMIC patterns ─────────────────────────────────────────────────────── */


void test_cov_vm_dynamic(void) {
  test_suite("Coverage: full-VM DYNAMIC_DEF/DYNAMIC");

  /* DYNAMIC_DEF(source, inner_bc: LIT('a') ACCEPT) DYNAMIC ACCEPT.
   * First run: cache miss; second run on the same VM: cache hit. */
  uint8_t bc[256];
  size_t ip = 0;
  const char *src = "'a'";
  bc[ip++] = OP_DYNAMIC_DEF;
  covv_emit_u32_be(bc, &ip, (uint32_t)strlen(src));
  memcpy(bc + ip, src, strlen(src));
  ip += strlen(src);
  covv_emit_u32_be(bc, &ip, 11); /* inner bc_len */
  /* Inner bytecode lives in its own copied buffer starting here: LIT at
   * inner offset 0 → data offset 9. */
  bc[ip++] = OP_LIT;
  covv_emit_u32_be(bc, &ip, 9);
  covv_emit_u32_be(bc, &ip, 1);
  bc[ip++] = 'a';
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_DYNAMIC;
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;

  dynamic_pattern_cache_t cache;
  test_assert(dynamic_pattern_cache_init(&cache, 0), "dyn cache init");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = "a";
  vm.len = 1;
  vm.dyn_cache = &cache;
  snobol_buf out;
  snobol_buf_init(&out);
  vm.out = &out;

  bool ok = vm_exec(&vm);
  test_assert(ok, "DYNAMIC cache-miss match succeeds");
  ok = vm_exec(&vm);
  test_assert(ok, "DYNAMIC cache-hit match succeeds");

  /* Capture undo across a DYNAMIC sub-match: a choice pushed BEFORE a
   * capture set must undo the capture when backtracking through the
   * dynamic boundary.  The buggy op_dynamic let the inner run clear and
   * free the outer trail, so the pop had nothing to undo and the capture
   * stuck at the branch-A value (cap_end[0] == 1 instead of 0). */
  {
    uint8_t bc4[256];
    size_t ip4 = 0;
    const char *src4 = "'a'";
    bc4[ip4++] = OP_SPLIT;
    size_t split_a_off = ip4;
    covv_emit_u32_be(bc4, &ip4, 0); /* branch A target: patch below */
    size_t split_b_off = ip4;
    covv_emit_u32_be(bc4, &ip4, 0); /* branch B target: patch below */
    size_t branch_a = ip4;
    bc4[ip4++] = OP_CAP_START;
    bc4[ip4++] = 0;
    bc4[ip4++] = OP_LIT;
    covv_emit_u32_be(bc4, &ip4, (uint32_t)(ip4 + 8));
    covv_emit_u32_be(bc4, &ip4, 1);
    bc4[ip4++] = 'a';
    bc4[ip4++] = OP_CAP_END;
    bc4[ip4++] = 0;
    bc4[ip4++] = OP_DYNAMIC_DEF;
    covv_emit_u32_be(bc4, &ip4, (uint32_t)strlen(src4));
    memcpy(bc4 + ip4, src4, strlen(src4));
    ip4 += strlen(src4);
    covv_emit_u32_be(bc4, &ip4, 11); /* inner bc_len */
    /* Inner bytecode lives in its own copied buffer: LIT at offset 0,
     * data at offset 9 ('a'), ACCEPT at offset 10. */
    bc4[ip4++] = OP_LIT;
    covv_emit_u32_be(bc4, &ip4, 9);
    covv_emit_u32_be(bc4, &ip4, 1);
    bc4[ip4++] = 'a';
    bc4[ip4++] = OP_ACCEPT;
    bc4[ip4++] = OP_DYNAMIC;
    bc4[ip4++] = OP_FAIL;
    size_t branch_b = ip4;
    bc4[ip4++] = OP_ACCEPT;
    size_t bc4_len = ip4;
    /* Patch SPLIT branch targets (big-endian u32). */
    bc4[split_a_off] = (uint8_t)(branch_a >> 24);
    bc4[split_a_off + 1] = (uint8_t)(branch_a >> 16);
    bc4[split_a_off + 2] = (uint8_t)(branch_a >> 8);
    bc4[split_a_off + 3] = (uint8_t)branch_a;
    bc4[split_b_off] = (uint8_t)(branch_b >> 24);
    bc4[split_b_off + 1] = (uint8_t)(branch_b >> 16);
    bc4[split_b_off + 2] = (uint8_t)(branch_b >> 8);
    bc4[split_b_off + 3] = (uint8_t)branch_b;

    VM vm4;
    memset(&vm4, 0, sizeof(vm4));
    vm4.bc = bc4;
    vm4.bc_len = bc4_len;
    vm4.s = "aa";
    vm4.len = 2;
    dynamic_pattern_cache_t c4;
    dynamic_pattern_cache_init(&c4, 0);
    vm4.dyn_cache = &c4;
    snobol_buf ob4;
    snobol_buf_init(&ob4);
    vm4.out = &ob4;
    bool ok4 = vm_exec(&vm4);
    test_assert(ok4, "DYNAMIC backtrack + branch-B accept succeeds");
    test_assert(vm4.cap_end[0] == 0,
                "capture undone across the DYNAMIC boundary");
    vm_free_labels(&vm4);
    snobol_buf_free(&ob4);
    dynamic_pattern_cache_destroy(&c4);
  }
  vm_free_labels(&vm);
  snobol_buf_free(&out);
  dynamic_pattern_cache_destroy(&cache);

  /* DYNAMIC with no pending definition → fail. */
  {
    uint8_t bc2[8] = {OP_DYNAMIC, OP_ACCEPT};
    VM vm2;
    memset(&vm2, 0, sizeof(vm2));
    vm2.bc = bc2;
    vm2.bc_len = 2;
    vm2.s = "a";
    vm2.len = 1;
    dynamic_pattern_cache_t c2;
    dynamic_pattern_cache_init(&c2, 0);
    vm2.dyn_cache = &c2;
    snobol_buf ob;
    snobol_buf_init(&ob);
    vm2.out = &ob;
    bool ok2 = vm_exec(&vm2);
    test_assert((!ok2) != 0, "DYNAMIC without DEF fails");
    vm_free_labels(&vm2);
    snobol_buf_free(&ob);
    dynamic_pattern_cache_destroy(&c2);
  }

  /* DYNAMIC with a failing inner pattern → restore + fail. */
  {
    uint8_t bc3[64];
    size_t ip3 = 0;
    const char *src3 = "'z'";
    bc3[ip3++] = OP_DYNAMIC_DEF;
    covv_emit_u32_be(bc3, &ip3, (uint32_t)strlen(src3));
    memcpy(bc3 + ip3, src3, strlen(src3));
    ip3 += strlen(src3);
    covv_emit_u32_be(bc3, &ip3, 2); /* inner: OP_FAIL OP_ACCEPT */
    bc3[ip3++] = OP_FAIL;
    bc3[ip3++] = OP_ACCEPT;
    bc3[ip3++] = OP_DYNAMIC;
    bc3[ip3++] = OP_ACCEPT;
    VM vm3;
    memset(&vm3, 0, sizeof(vm3));
    vm3.bc = bc3;
    vm3.bc_len = ip3;
    vm3.s = "a";
    vm3.len = 1;
    dynamic_pattern_cache_t c3;
    dynamic_pattern_cache_init(&c3, 0);
    vm3.dyn_cache = &c3;
    snobol_buf ob3;
    snobol_buf_init(&ob3);
    vm3.out = &ob3;
    bool ok3 = vm_exec(&vm3);
    test_assert((!ok3) != 0, "DYNAMIC failing inner pattern fails");
    vm_free_labels(&vm3);
    snobol_buf_free(&ob3);
    dynamic_pattern_cache_destroy(&c3);
  }
}

/* ── Primitives: BREAKX/BAL/FENCE/ABORT/SUCCEED/FAIL/NOP/GOTO ─────────────── */


void test_cov_vm_primitives(void) {
  test_suite("Coverage: full-VM primitive opcodes");

  /* BREAKX single-byte memchr + retry push. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_breakx(",", 1);
    parts[1] = snobol_ast_create_lit(",", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "a,b", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 2) != 0, "BREAKX+literal matches at the comma");
    snobol_ast_free(ast);
  }

  /* BREAKX multi-char class → bitmap loop branch. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_breakx("ab", 2);
    parts[1] = snobol_ast_create_lit("a", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "zzza", 4, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 4) != 0, "BREAKX bitmap scan matches");
    snobol_ast_free(ast);
  }

  /* BREAKX non-ASCII class → UTF-8 walk over the leading ASCII byte. */
  {
    const char *euro = "\xE2\x82\xAC";
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_breakx(euro, 3);
    parts[1] = snobol_ast_create_lit(euro, 3);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok =
        covv_run_ast(ast, "x\xE2\x82\xAC", 4, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 4) != 0, "BREAKX UTF-8 walk matches");
    snobol_ast_free(ast);
  }

  /* BREAKX with no break char: no retry push, trailing op fails. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_breakx(",", 1);
    parts[1] = snobol_ast_create_lit("x", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "BREAKX without delimiter cannot match tail");
    snobol_ast_free(ast);
  }

  /* BAL: balanced delimiters. */
  {
    ast_node_t *ast = snobol_ast_create_bal('(', ')');
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "(a(b)c)", 7, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 7) != 0, "BAL matches balanced parens");
    snobol_ast_free(ast);

    ast = snobol_ast_create_bal('(', ')');
    ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "BAL fails without open delimiter");
    snobol_ast_free(ast);

    ast = snobol_ast_create_bal('(', ')');
    ok = covv_run_ast(ast, "(a", 2, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "BAL fails on unbalanced subject");
    snobol_ast_free(ast);
  }

  /* FENCE cuts backtracking. */
  {
    /* 'a' | 'ab' FENCE 'c' on "abc": first branch 'a' fails at 'c' after
     * FENCE cut → no backtrack to 'ab'. */
    ast_node_t *left = snobol_ast_create_lit("a", 1);
    ast_node_t *right = snobol_ast_create_lit("ab", 2);
    ast_node_t *alt = snobol_ast_create_alt(left, right);
    ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = alt;
    parts[1] = snobol_ast_create_fence();
    parts[2] = snobol_ast_create_lit("c", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 3);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "FENCE blocks backtracking into alternative");
    snobol_ast_free(ast);
  }

  /* FAIL with a live choice: backtracks to the alternative. */
  {
    ast_node_t **left_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    left_parts[0] = snobol_ast_create_lit("x", 1);
    left_parts[1] = snobol_ast_create_fail();
    ast_node_t *left = snobol_ast_create_concat(left_parts, 2);
    ast_node_t *ast =
        snobol_ast_create_alt(left, snobol_ast_create_lit("y", 1));
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "y", 1, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 1) != 0, "FAIL backtracks to the alternative");
    snobol_ast_free(ast);
  }

  /* ABORT terminates the match. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_abort();
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((!ok) != 0, "ABORT terminates with failure");
    snobol_ast_free(ast);
  }

  /* SUCCEED forces success. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_succeed();
    parts[1] = snobol_ast_create_lit("z", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int pos = 0;
    int caps = 0;
    bool ok = covv_run_ast(ast, "abc", 3, &pos, &caps, nullptr, nullptr);
    test_assert((ok && pos == 0) != 0, "SUCCEED short-circuits the tail");
    snobol_ast_free(ast);
  }

  /* NOP via crafted bytecode. */
  {
    uint8_t bc[32] = {OP_NOP, OP_LIT, 0, 0, 0, 1, 'a', OP_ACCEPT};
    size_t ip = 2;
    /* Rebuild: NOP LIT(off=10,len=1) data ACCEPT */
    bc[0] = OP_NOP;
    bc[1] = OP_LIT;
    ip = 2;
    covv_emit_u32_be(bc, &ip, 10);
    covv_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "a";
    vm.len = 1;
    bool ok = vm_exec(&vm);
    test_assert(ok, "NOP is skipped");
    vm_free_labels(&vm);
  }
}


void test_cov_vm_goto(void) {
  test_suite("Coverage: full-VM GOTO/GOTO_F paths");

  /* GOTO to a registered label → success. */
  {
    uint8_t bc[8] = {OP_GOTO, 0, 1, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 4;
    vm.s = "a";
    vm.len = 1;
    test_assert(vm_register_label(&vm, 1, 3), "label 1 registered at 3");
    bool ok = vm_exec(&vm);
    test_assert(ok, "GOTO to valid label succeeds");
    vm_free_labels(&vm);
  }

  /* GOTO to an unknown label → in_goto_fail + fail. */
  {
    uint8_t bc[8] = {OP_GOTO, 0, 5, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 3;
    vm.s = "a";
    vm.len = 1;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "GOTO to unknown label fails");
    vm_free_labels(&vm);
  }

  /* GOTO_F without failure → continue (no jump). */
  {
    uint8_t bc[8] = {OP_GOTO_F, 0, 1, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 4;
    vm.s = "a";
    vm.len = 1;
    bool ok = vm_exec(&vm);
    test_assert(ok, "GOTO_F without failure continues");
    vm_free_labels(&vm);
  }

  /* GOTO_F with in_goto_fail and a valid target → jumps and clears flag. */
  {
    uint8_t bc[8] = {OP_GOTO_F, 0, 1, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 4;
    vm.s = "a";
    vm.len = 1;
    vm_register_label(&vm, 1, 3);
    vm.in_goto_fail = true;
    bool ok = vm_exec(&vm);
    test_assert(ok, "GOTO_F with failure jumps to label");
    vm_free_labels(&vm);
  }

  /* GOTO_F targeting a label genuinely emitted at bytecode offset 0: the
   * jump must succeed (offset 0 is a valid target, distinct from "missing").
   * The second pass through GOTO_F sees in_goto_fail cleared, so it falls
   * through to ACCEPT — no loop.  Regression for the 0-vs-missing
   * ambiguity in vm_get_label_offset. */
  {
    uint8_t bc[8] = {OP_LABEL, 0, 1, OP_GOTO_F, 0, 1, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 7;
    vm.s = "a";
    vm.len = 1;
    vm_register_label(&vm, 1, 0);
    vm.in_goto_fail = true;
    bool ok = vm_exec(&vm);
    test_assert(ok, "GOTO_F to label at offset 0 succeeds");
    vm_free_labels(&vm);
  }

  /* GOTO_F with in_goto_fail and unknown label → fail. */
  {
    uint8_t bc[8] = {OP_GOTO_F, 0, 9, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 3;
    vm.s = "a";
    vm.len = 1;
    vm.in_goto_fail = true;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "GOTO_F with unknown label fails");
    vm_free_labels(&vm);
  }
}

/* ── Format detection / range meta / misc helpers ─────────────────────────── */


void test_cov_vm_helpers(void) {
  test_suite("Coverage: range meta + bitmap helpers");

  /* get_ranges_ptr with a compiler-produced bytecode (SNBL trailer) and no
   * range_meta → NEW-format detection. */
  {
    ast_node_t *ast = snobol_ast_create_span("a", 1);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc == 0 && bc && bc_len >= 8) != 0, "span pattern compiles");
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    uint16_t cnt = 0;
    uint16_t ci = 0;
    const uint8_t *rp = get_ranges_ptr(&vm, 1, &cnt, &ci);
    test_assert((rp != NULL && cnt >= 1) != 0, "SNBL-trailer range resolution");
    test_assert(get_ranges_ptr(&vm, 0, &cnt, &ci) == NULL,
                "set_id 0 returns NULL");
    test_assert(get_ranges_ptr(&vm, 99, &cnt, &ci) == NULL,
                "set_id beyond count returns NULL");
    compiler_free(bc);
    snobol_ast_free(ast);
  }

  /* snobol_build_range_meta: new-format + old-format + tiny buffer. */
  {
    ast_node_t *ast = snobol_ast_create_span("a", 1);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_range_meta_t *tab = nullptr;
    size_t tab_count = 0;
    snobol_build_range_meta(bc, bc_len, &tab, &tab_count);
    test_assert((tab_count >= 1 && tab != NULL) != 0,
                "range meta built from SNBL-trailer bytecode");
    snobol_free(tab);
    compiler_free(bc);
    snobol_ast_free(ast);

    uint8_t one[2] = {0, 0};
    snobol_build_range_meta(one, 2, &tab, &tab_count);
    test_assert((tab == NULL && tab_count == 0) != 0,
                "range meta rejects tiny bytecode");
    snobol_build_range_meta(nullptr, 0, &tab, &tab_count);
    test_assert(tab == NULL, "range meta rejects NULL bytecode");
  }

  /* ranges_to_full_bitmap: range beyond 255 → false; valid → true. */
  {
    uint8_t rng[8];
    size_t at = 0;
    covv_emit_u32_be(rng, &at, 0);
    covv_emit_u32_be(rng, &at, 300);
    uint64_t map[4];
    test_assert((!ranges_to_full_bitmap(rng, 1, map)) != 0,
                "range > 255 rejected by full bitmap");
    at = 0;
    covv_emit_u32_be(rng, &at, 0);
    covv_emit_u32_be(rng, &at, 127);
    test_assert(ranges_to_full_bitmap(rng, 1, map),
                "range <= 255 accepted by full bitmap");
    test_assert((bitmap_test_256(map, 0) && !bitmap_test_256(map, 200)) != 0,
                "full bitmap bits set");
  }

  /* snobol_buf growth. */
  {
    snobol_buf b;
    snobol_buf_init(&b);
    for (int i = 0; i < 3000; i++) {
      snobol_buf_append(&b, "x", 1);
    }
    test_assert((b.len == 3000 && b.cap >= 3000) != 0,
                "buffer grows past 1 KB");
    snobol_buf_clear(&b);
    test_assert(b.len == 0, "buffer clear resets length");
    snobol_buf_free(&b);
    test_assert((b.data == NULL && b.cap == 0) != 0,
                "buffer free resets state");
  }

  /* VM state reuse: second vm_run reuses trail/write-log (clear paths). */
  {
    uint8_t bc[16] = {OP_LIT, 0, 0, 0, 9, 0, 0, 0, 1, 'a', OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 11;
    vm.s = "a";
    vm.len = 1;
    bool ok = vm_exec(&vm);
    test_assert(ok, "first VM run succeeds");
    ok = vm_exec(&vm);
    test_assert(ok, "second VM run reuses trail/write-log");
    vm_free_labels(&vm);
  }

  /* get_ranges_ptr on a 3-byte buffer → NULL (tail < 4). */
  {
    uint8_t tiny[3] = {0, 0, 0};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = tiny;
    vm.bc_len = 3;
    uint16_t cnt = 0;
    uint16_t ci = 0;
    test_assert(get_ranges_ptr(&vm, 1, &cnt, &ci) == NULL,
                "tiny bytecode range lookup returns NULL");
  }
}


/* ===== test_coverage_engine2 (part): coverage-driven tests merged into test_vm.c ===== */
#include "../../core/include/snobol/snobol.h"


void cove_emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

void cove_emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

void cove_emit_cb(const char *data, size_t len, void *udata) {
  snobol_buf *b = (snobol_buf *)udata;
  snobol_buf_append(b, data, len);
}

/* ── vm_exec crafted paths ────────────────────────────────────────────────── */

void test_cov_engine2_vm_exec(void) {
  test_suite("Coverage: vm_exec crafted paths (round 2)");

  /* EVAL with an invalid register (>= MAX_CAPS) fails. */
  {
    uint8_t bc[16] = {OP_EVAL, 0, 0, 99, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 4;
    vm.s = "x";
    vm.len = 1;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "EVAL invalid register fails");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EVAL with invalid capture bounds (cap_end < cap_start). */
  {
    uint8_t bc[16] = {OP_CAP_START, 0, OP_LIT,  0, 0, 0, 8,        1, 'x',
                      OP_CAP_END,   0, OP_EVAL, 0, 1, 0, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 16;
    vm.s = "xx";
    vm.len = 2;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    vm.cap_end[0] = 0; /* end before start */
    vm.cap_start[0] = 1;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "EVAL invalid capture bounds fail");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EVAL REVERSE/UPPER/LOWER with out + emit_fn. */
  {
    const int fns[] = {SNOBOL_FN_REVERSE, SNOBOL_FN_UPPER, SNOBOL_FN_LOWER};
    for (size_t fi = 0; fi < sizeof(fns) / sizeof(fns[0]); fi++) {
      int fn = fns[fi];
      uint8_t bc[32];
      size_t ip = 0;
      bc[ip++] = OP_CAP_START;
      bc[ip++] = 0;
      bc[ip++] = OP_LIT;
      cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
      cove_emit_u32_be(bc, &ip, 2);
      bc[ip++] = 'a';
      bc[ip++] = 'b';
      bc[ip++] = OP_CAP_END;
      bc[ip++] = 0;
      bc[ip++] = OP_EVAL;
      cove_emit_u16_be(bc, &ip, (uint16_t)fn);
      bc[ip++] = 0;
      bc[ip++] = OP_ACCEPT;
      VM vm;
      memset(&vm, 0, sizeof(vm));
      vm.bc = bc;
      vm.bc_len = ip;
      vm.s = "ab";
      vm.len = 2;
      snobol_buf out;
      snobol_buf cb;
      snobol_buf_init(&out);
      snobol_buf_init(&cb);
      vm.out = &out;
      vm.emit_fn = cove_emit_cb;
      vm.emit_udata = &cb;
      bool ok = vm_exec(&vm);
      test_assert(ok, "EVAL transform builtin succeeds");
      test_assert((out.len > 0 && cb.len == out.len) != 0,
                  "EVAL builtin writes out + emit_fn");
      vm_free_labels(&vm);
      snobol_buf_free(&out);
      snobol_buf_free(&cb);
    }
  }

  /* REPEAT_STEP with an out-of-range loop id → step_done. */
  {
    uint8_t bc[8] = {OP_REPEAT_STEP, 99, 0, 0, 0, 0, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 7;
    vm.s = "";
    vm.len = 0;
    bool ok = vm_exec(&vm);
    test_assert(ok, "REPEAT_STEP bad loop id falls through to ACCEPT");
    vm_free_labels(&vm);
  }

  /* Greedy-span repeat: unbounded repeat over a pure SPAN body. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_repeat(snobol_ast_create_span("a", 1), 1, -1);
    parts[1] = snobol_ast_create_lit("b", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc == 0 && bc) != 0, "greedy-span pattern compiles");
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "aaaab";
    vm.len = 5;
    bool ok = vm_exec(&vm);
    test_assert((ok && vm.pos == 5) != 0, "greedy-span repeat matches run");
    vm_free_labels(&vm);
    compiler_free(bc);
    snobol_ast_free(ast);
  }

  /* EMIT_FORMAT variants with emit_fn. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, 40);
    cove_emit_u32_be(bc, &ip, 3);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_UPPER;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_LOWER;
    bc[ip++] = OP_EMIT_FORMAT;
    bc[ip++] = 0;
    bc[ip++] = SNBL_FMT_LENGTH;
    bc[ip++] = OP_ACCEPT;
    bc[40] = 'a';
    bc[41] = 'b';
    bc[42] = 'c';
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 43;
    vm.s = "abc";
    vm.len = 3;
    snobol_buf out;
    snobol_buf cb;
    snobol_buf_init(&out);
    snobol_buf_init(&cb);
    vm.out = &out;
    vm.emit_fn = cove_emit_cb;
    vm.emit_udata = &cb;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len == 7 && cb.len == out.len) != 0,
                "EMIT_FORMAT chain with emit_fn");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    snobol_buf_free(&cb);
  }

  /* GOTO_F failure path with a live choice (target missing). */
  {
    uint8_t bc[16] = {OP_SPLIT, 0, 0,         0, 4, 0,        0,
                      0,        4, OP_GOTO_F, 0, 9, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 13;
    vm.s = "";
    vm.len = 0;
    vm.in_goto_fail = true;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "GOTO_F unknown label fails");
    vm_free_labels(&vm);
  }

  /* TABLE_GET with an invalid key register. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'k';
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_TABLE_GET;
    cove_emit_u16_be(bc, &ip, 0);
    bc[ip++] = 99; /* key_reg >= MAX_CAPS */
    bc[ip++] = 1;
    bc[ip++] = 1;
    bc[ip++] = 't';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "k";
    vm.len = 1;
    vm_init_tables(&vm);
    snobol_table_t *tbl = table_create("t");
    uint16_t tid = 0;
    vm_register_table(&vm, tbl, &tid);
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "TABLE_GET invalid key register fails");
    vm_free_labels(&vm);
    vm_free_tables(&vm);
    table_release(tbl);
  }

  /* TABLE_SET with a missing value register. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'k';
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_TABLE_SET;
    cove_emit_u16_be(bc, &ip, 0);
    bc[ip++] = 0;  /* key_reg */
    bc[ip++] = 99; /* value_reg >= MAX_CAPS */
    bc[ip++] = 1;
    bc[ip++] = 't';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "k";
    vm.len = 1;
    vm_init_tables(&vm);
    snobol_table_t *tbl = table_create("t");
    uint16_t tid = 0;
    vm_register_table(&vm, tbl, &tid);
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "TABLE_SET invalid value register fails");
    vm_free_labels(&vm);
    vm_free_tables(&vm);
    table_release(tbl);
  }

  /* Position ops with UTF-8 subjects (continuation-byte walks + fails). */
  {
    /* RPOS fail: LEN(1) RPOS(2) on "ab" (2 remain after pos 1). */
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(1);
    parts[1] = snobol_ast_create_rpos(2);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "ab";
    vm.len = 2;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "RPOS fail path");
    vm_free_labels(&vm);
    compiler_free(bc);

    /* POS UTF-8: POS(2) after LEN(1) on "a\xC3\xA9b" → byte offset 3. */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(2);
    parts[1] = snobol_ast_create_pos(2);
    ast = snobol_ast_create_concat(parts, 2);
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "a\xC3\xA9"
           "b";
    vm.len = 4;
    ok = vm_exec(&vm);
    test_assert(ok, "POS(2) at byte offset 3 on multibyte subject");
    vm_free_labels(&vm);
    compiler_free(bc);

    /* TAB UTF-8 + fail: TAB(3) on a 2-codepoint subject fails. */
    parts = (ast_node_t **)malloc(1 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_tab(3);
    ast = snobol_ast_create_concat(parts, 1);
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "a\xC3\xA9"
           "b";
    vm.len = 4;
    ok = vm_exec(&vm);
    test_assert((!ok) != 0, "TAB beyond subject fails");
    vm_free_labels(&vm);
    compiler_free(bc);
  }

  /* DYNAMIC_DEF twice frees the previous pending buffers. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    const char *s1 = "'a'";
    bc[ip++] = OP_DYNAMIC_DEF;
    cove_emit_u32_be(bc, &ip, (uint32_t)strlen(s1));
    memcpy(bc + ip, s1, strlen(s1));
    ip += strlen(s1);
    cove_emit_u32_be(bc, &ip, 6); /* LEN(1) ACCEPT */
    bc[ip++] = OP_LEN;
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    const char *s2 = "'b'";
    bc[ip++] = OP_DYNAMIC_DEF;
    cove_emit_u32_be(bc, &ip, (uint32_t)strlen(s2));
    memcpy(bc + ip, s2, strlen(s2));
    ip += strlen(s2);
    cove_emit_u32_be(bc, &ip, 6);
    bc[ip++] = OP_LEN;
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    bc[ip++] = OP_DYNAMIC;
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "a";
    vm.len = 1;
    dynamic_pattern_cache_t cache;
    dynamic_pattern_cache_init(&cache, 0);
    vm.dyn_cache = &cache;
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert(ok, "double DYNAMIC_DEF runs");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    dynamic_pattern_cache_destroy(&cache);
  }
}

void test_cov_engine2_vm_exec_round5(void) {
  test_suite("Coverage: vm_exec final opcode paths");

  /* EVAL cap_ok failure (cap_end > len). */
  {
    uint8_t bc[16] = {OP_CAP_START, 0, OP_LIT,  0, 0, 0, 8,        1, 'x',
                      OP_CAP_END,   0, OP_EVAL, 0, 1, 0, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 16;
    vm.s = "x";
    vm.len = 1;
    vm.cap_start[0] = 0;
    vm.cap_end[0] = 5; /* beyond subject len 1 */
    snobol_buf out;
    snobol_buf_init(&out);
    vm.out = &out;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "EVAL capture bounds beyond subject fail");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
  }

  /* EMIT_LITERAL with inline data + emit_fn. */
  {
    uint8_t bc[16] = {OP_EMIT_LITERAL, 0, 0, 0, 9, 0, 0, 0, 1, 'Z', OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 11;
    vm.s = "";
    vm.len = 0;
    snobol_buf out;
    snobol_buf cb;
    snobol_buf_init(&out);
    snobol_buf_init(&cb);
    vm.out = &out;
    vm.emit_fn = cove_emit_cb;
    vm.emit_udata = &cb;
    bool ok = vm_exec(&vm);
    test_assert((ok && out.len == 1 && cb.len == 1) != 0,
                "EMIT_LITERAL inline");
    vm_free_labels(&vm);
    snobol_buf_free(&out);
    snobol_buf_free(&cb);
  }

  /* EMIT_CAPTURE with emit_fn. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, 20);
    cove_emit_u32_be(bc, &ip, 2);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_CAPTURE;
    bc[ip++] = 0;
    bc[ip++] = OP_ACCEPT;
    bc[20] = 'a';
    bc[21] = 'b';
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 22;
    vm.s = "ab";
    vm.len = 2;
    snobol_buf cb;
    snobol_buf_init(&cb);
    vm.emit_fn = cove_emit_cb;
    vm.emit_udata = &cb;
    bool ok = vm_exec(&vm);
    test_assert((ok && cb.len == 2) != 0, "EMIT_CAPTURE emit_fn");
    vm_free_labels(&vm);
    snobol_buf_free(&cb);
  }

  /* EMIT_EXPR with emit_fn. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, 20);
    cove_emit_u32_be(bc, &ip, 2);
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_EMIT_EXPR;
    bc[ip++] = 0;
    bc[ip++] = 1; /* upper */
    bc[ip++] = OP_ACCEPT;
    bc[20] = 'a';
    bc[21] = 'b';
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 22;
    vm.s = "ab";
    vm.len = 2;
    snobol_buf cb;
    snobol_buf_init(&cb);
    vm.emit_fn = cove_emit_cb;
    vm.emit_udata = &cb;
    bool ok = vm_exec(&vm);
    test_assert((ok && cb.len == 2 && cb.data[0] == 'A') != 0,
                "EMIT_EXPR upper emit_fn");
    vm_free_labels(&vm);
    snobol_buf_free(&cb);
  }

  /* GOTO invalid target with a live choice (pop restore).  Label 5 is
   * unregistered, so vm_get_label_offset returns the invalid sentinel. */
  {
    uint8_t bc[16] = {OP_SPLIT, 0, 0,       0, 9, 0,        0,
                      0,        9, OP_GOTO, 0, 5, OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 13;
    vm.s = "";
    vm.len = 0;
    /* label 5 intentionally NOT registered → invalid path */
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "GOTO invalid target fails");
    vm_free_labels(&vm);
  }

  /* ARRAY_GET with an invalid array id. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = '1';
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_ARRAY_GET;
    cove_emit_u16_be(bc, &ip, 7); /* unregistered */
    bc[ip++] = 0;
    bc[ip++] = 1;
    bc[ip++] = 1;
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "1";
    vm.len = 1;
    vm_init_arrays(&vm);
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "ARRAY_GET invalid array fails");
    vm_free_labels(&vm);
    vm_free_arrays(&vm);
  }

  /* ARRAY_SET with an invalid value register. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_CAP_START;
    bc[ip++] = 0;
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = '1';
    bc[ip++] = OP_CAP_END;
    bc[ip++] = 0;
    bc[ip++] = OP_ARRAY_SET;
    cove_emit_u16_be(bc, &ip, 0);
    bc[ip++] = 0;
    bc[ip++] = 99;
    bc[ip++] = 1;
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = ip;
    vm.s = "1";
    vm.len = 1;
    vm_init_arrays(&vm);
    snobol_array_t *arr = snobol_array_create(4);
    uint16_t aid = 0;
    vm_register_array(&vm, arr, &aid);
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "ARRAY_SET invalid value register fails");
    vm_free_labels(&vm);
    vm_free_arrays(&vm);
    snobol_array_release(arr);
  }

  /* BAL on truncated UTF-8 subject. */
  {
    ast_node_t *ast = snobol_ast_create_bal('(', ')');
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    snobol_ast_free(ast);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.s = "(\xE2\x82"; /* truncated UTF-8 inside parens */
    vm.len = 3;
    bool ok = vm_exec(&vm);
    test_assert((!ok) != 0, "BAL truncated UTF-8 fails");
    vm_free_labels(&vm);
    compiler_free(bc);
  }
}


/* ── state API capture cleanup + anchored output ──────────────────────────── */


void test_vm_suite(void) {
  test_suite("VM Tests");

  /* Basic VM initialization test */
  test_assert(true, "VM initialization placeholder");

  /* Add more VM-specific tests here as the VM API stabilizes */
  test_assert(true, "VM can be instantiated");
  test_cov_vm_charclass_utf8();
  test_cov_vm_break_variants();
  test_cov_vm_position_ops();
  test_cov_vm_caps_assign();
  test_cov_vm_eval();
  test_cov_vm_anchor_fail();
  test_cov_vm_repeat();
  test_cov_vm_emit();
  test_cov_vm_table_array();
  test_cov_vm_dynamic();
  test_cov_vm_primitives();
  test_cov_vm_goto();
  test_cov_vm_helpers();
  test_cov_engine2_vm_exec();
  test_cov_engine2_vm_exec_round5();
}
