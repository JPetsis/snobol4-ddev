/*
 * test_backtracking.c - VM backtracking correctness tests
 *
 * These tests build tiny bytecode programs by hand and run the VM in
 * STANDALONE_BUILD mode.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/vm.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void debug_dump_state(const char *label, const VM *vm) {
#ifdef DEBUG_BACKTRACK
  fprintf(stderr,
          "[%s] pos=%zu cap0=[%zu,%zu] cap1=[%zu,%zu] var_count=%zu "
          "var0=[%zu,%zu]\n",
          label, vm->pos, vm->cap_start[0], vm->cap_end[0], vm->cap_start[1],
          vm->cap_end[1], vm->var_count, vm->var_start[0], vm->var_end[0]);
#else
  (void)label;
  (void)vm;
#endif
}

static void emit_u8(uint8_t *bc, size_t *ip, uint8_t v) {
  bc[(*ip)++] = v;
}

static void emit_u16(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

static void emit_u32(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

static VM make_vm(const uint8_t *bc, size_t bc_len, const char *subject) {
  VM vm;
  memset(&vm, 0, sizeof(vm));

  vm.bc = bc;
  vm.bc_len = bc_len;

  vm.s = subject;
  vm.len = strlen(subject);

  vm.ip = 0;
  vm.pos = 0;

  // VM currently uses 0-initialization for "unset" capture/var slots.
  // Keep that invariant for standalone tests.
  vm.var_count = 0;

  return vm;
}

static bool cap_equals(const VM *vm, uint8_t reg, const char *subject,
                       const char *expected) {
  if (reg >= MAX_CAPS) {
    return false;
  }
  size_t a = vm->cap_start[reg];
  size_t b = vm->cap_end[reg];

  if (expected == NULL) {
    // Expect unset (defaults to [0,0])
    return (a == 0 && b == 0) != 0;
  }

  if (b < a) {
    return false;
  }
  size_t n = b - a;
  return (strlen(expected) == n && memcmp(subject + a, expected, n) == 0) != 0;
}

static bool var_equals(const VM *vm, uint16_t var, const char *subject,
                       const char *expected) {
  if (var >= MAX_VARS) {
    return false;
  }
  size_t a = vm->var_start[var];
  size_t b = vm->var_end[var];

  if (expected == NULL) {
    return (a == 0 && b == 0) != 0;
  }

  if (b < a) {
    return false;
  }
  size_t n = b - a;
  return (strlen(expected) == n && memcmp(subject + a, expected, n) == 0) != 0;
}

/*
 * Minimal bytecode builder that supports embedding literal bytes in a tail pool
 * and referencing them via OP_LIT offsets.
 */
typedef struct {
  uint8_t bc[512];
  size_t ip;
  size_t lit_pool;
} Bc;

static void bc_init(Bc *b) {
  memset(b, 0, sizeof(*b));
  b->ip = 0;
  // Leave space for code; literals are appended after this.
  b->lit_pool = 256;
}

static uint32_t bc_add_lit(Bc *b, const char *bytes, size_t len) {
  uint32_t off = (uint32_t)b->lit_pool;
  memcpy(b->bc + b->lit_pool, bytes, len);
  b->lit_pool += len;
  return off;
}

static void bc_emit_u8(Bc *b, uint8_t v) {
  emit_u8(b->bc, &b->ip, v);
}
static void bc_emit_u16(Bc *b, uint16_t v) {
  emit_u16(b->bc, &b->ip, v);
}
static void bc_emit_u32(Bc *b, uint32_t v) {
  emit_u32(b->bc, &b->ip, v);
}

static void bc_emit_lit1(Bc *b, char c) {
  uint32_t off = bc_add_lit(b, &c, 1);
  bc_emit_u8(b, OP_LIT);
  bc_emit_u32(b, off);
  bc_emit_u32(b, 1);
}

/*
 * Bytecode program for: ( CAP0( 'a' ) FAIL ) | ( CAP0( 'b' ) )
 * Subject: "b"
 * Expected: success, cap0 == "b".
 */
static void test_capture_restore_on_backtrack(void) {
  Bc b;
  bc_init(&b);

  // SPLIT a b
  size_t split_ip = b.ip;
  bc_emit_u8(&b, OP_SPLIT);
  bc_emit_u32(&b, 0); // a
  bc_emit_u32(&b, 0); // b

  // a:
  size_t a_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'a');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_FAIL);

  // b:
  size_t b_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ACCEPT);

  // Patch split targets
  b.bc[split_ip + 1] = (uint8_t)((a_ip >> 24) & 0xFF);
  b.bc[split_ip + 2] = (uint8_t)((a_ip >> 16) & 0xFF);
  b.bc[split_ip + 3] = (uint8_t)((a_ip >> 8) & 0xFF);
  b.bc[split_ip + 4] = (uint8_t)(a_ip & 0xFF);

  b.bc[split_ip + 5] = (uint8_t)((b_ip >> 24) & 0xFF);
  b.bc[split_ip + 6] = (uint8_t)((b_ip >> 16) & 0xFF);
  b.bc[split_ip + 7] = (uint8_t)((b_ip >> 8) & 0xFF);
  b.bc[split_ip + 8] = (uint8_t)(b_ip & 0xFF);

  VM vm = make_vm(b.bc, b.lit_pool, "b");
  bool ok = vm_run(&vm);

  debug_dump_state("after test_capture_restore_on_backtrack", &vm);

  test_assert(ok, "Capture backtrack: match should succeed");
  test_assert(cap_equals(&vm, 0, vm.s, "b"),
              "Capture backtrack: cap0 should be 'b'");
}

/*
 * Nested alternation:
 *   ( ( CAP0('x') CAP1('q') FAIL ) | ( CAP0('y') ) )
 * Subject: "y"
 * Expected: success, cap0=="y", cap1 unset.
 */
static void test_nested_captures_deep_alternation(void) {
  Bc b;
  bc_init(&b);

  // SPLIT a b
  size_t split_ip = b.ip;
  bc_emit_u8(&b, OP_SPLIT);
  bc_emit_u32(&b, 0); // a
  bc_emit_u32(&b, 0); // b

  // a:
  size_t a_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'x');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);

  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 1);
  bc_emit_lit1(&b, 'q');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 1);

  bc_emit_u8(&b, OP_FAIL);

  // b:
  size_t b_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'y');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ACCEPT);

  // Patch split targets
  b.bc[split_ip + 1] = (uint8_t)((a_ip >> 24) & 0xFF);
  b.bc[split_ip + 2] = (uint8_t)((a_ip >> 16) & 0xFF);
  b.bc[split_ip + 3] = (uint8_t)((a_ip >> 8) & 0xFF);
  b.bc[split_ip + 4] = (uint8_t)(a_ip & 0xFF);

  b.bc[split_ip + 5] = (uint8_t)((b_ip >> 24) & 0xFF);
  b.bc[split_ip + 6] = (uint8_t)((b_ip >> 16) & 0xFF);
  b.bc[split_ip + 7] = (uint8_t)((b_ip >> 8) & 0xFF);
  b.bc[split_ip + 8] = (uint8_t)(b_ip & 0xFF);

  VM vm = make_vm(b.bc, b.lit_pool, "y");
  bool ok = vm_run(&vm);

  debug_dump_state("after test_nested_captures_deep_alternation", &vm);

  test_assert(ok, "Nested capture backtrack: match should succeed");
  test_assert(cap_equals(&vm, 0, vm.s, "y"),
              "Nested capture backtrack: cap0 should be 'y'");
  test_assert(cap_equals(&vm, 1, vm.s, nullptr),
              "Nested capture backtrack: cap1 should be unset (not leaked from "
              "failed branch)");
}

/*
 * Assignment restore under backtracking:
 *   ( CAP0('a') ASSIGN v0=cap0 FAIL ) | ( CAP0('b') ASSIGN v0=cap0 )
 * Subject: "b"
 * Expected: success, var_count==1, v0=="b".
 */
static void test_var_assignment_restore(void) {
  Bc b;
  bc_init(&b);

  size_t split_ip = b.ip;
  bc_emit_u8(&b, OP_SPLIT);
  bc_emit_u32(&b, 0);
  bc_emit_u32(&b, 0);

  size_t a_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'a');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ASSIGN);
  bc_emit_u16(&b, 0);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_FAIL);

  size_t b_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ASSIGN);
  bc_emit_u16(&b, 0);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ACCEPT);

  // Patch split targets
  b.bc[split_ip + 1] = (uint8_t)((a_ip >> 24) & 0xFF);
  b.bc[split_ip + 2] = (uint8_t)((a_ip >> 16) & 0xFF);
  b.bc[split_ip + 3] = (uint8_t)((a_ip >> 8) & 0xFF);
  b.bc[split_ip + 4] = (uint8_t)(a_ip & 0xFF);

  b.bc[split_ip + 5] = (uint8_t)((b_ip >> 24) & 0xFF);
  b.bc[split_ip + 6] = (uint8_t)((b_ip >> 16) & 0xFF);
  b.bc[split_ip + 7] = (uint8_t)((b_ip >> 8) & 0xFF);
  b.bc[split_ip + 8] = (uint8_t)(b_ip & 0xFF);

  VM vm = make_vm(b.bc, b.lit_pool, "b");
  bool ok = vm_run(&vm);

  debug_dump_state("after test_var_assignment_restore", &vm);

  test_assert(ok, "Var assignment backtrack: match should succeed");
  test_assert(vm.var_count == 1,
              "Var assignment backtrack: var_count should be 1");
  test_assert(var_equals(&vm, 0, vm.s, "b"),
              "Var assignment backtrack: var0 should be 'b'");
}

/*
 * Loop counter restore:
 *   REPEAT(min=0) body='a' then literal 'b'
 * Subject: "b"
 *
 * Greedy repeat will first try to match an 'a' (fails), then backtrack to skip.
 * This specifically exercises that the counter increment doesn't leak.
 */
static void test_loop_counter_restore(void) {
  Bc b;
  bc_init(&b);

  // REPEAT_INIT id=0 min=0 max=-1 skip=<skip_target>
  size_t init_ip = b.ip;
  bc_emit_u8(&b, OP_REPEAT_INIT);
  bc_emit_u8(&b, 0);
  bc_emit_u32(&b, 0);
  bc_emit_u32(&b, (uint32_t)-1);
  bc_emit_u32(&b, 0); // skip target placeholder

  // body target:
  size_t body_ip = b.ip;
  bc_emit_lit1(&b, 'a');

  // STEP id=0 target=body
  bc_emit_u8(&b, OP_REPEAT_STEP);
  bc_emit_u8(&b, 0);
  bc_emit_u32(&b, (uint32_t)body_ip);

  // skip target:
  size_t skip_ip = b.ip;
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_ACCEPT);

  // Patch skip target u32 at: init_ip + 1(op) + 1(loop_id) + 4(min) + 4(max)
  size_t patch = init_ip + 1 + 1 + 4 + 4;
  b.bc[patch + 0] = (uint8_t)((skip_ip >> 24) & 0xFF);
  b.bc[patch + 1] = (uint8_t)((skip_ip >> 16) & 0xFF);
  b.bc[patch + 2] = (uint8_t)((skip_ip >> 8) & 0xFF);
  b.bc[patch + 3] = (uint8_t)(skip_ip & 0xFF);

  VM vm = make_vm(b.bc, b.lit_pool, "b");
  bool ok = vm_run(&vm);

  test_assert(ok, "Loop counter backtrack: match should succeed");
  test_assert(vm.pos == 1, "Loop counter backtrack: should consume 'b'");
}

void test_backtracking_suite(void) {
  test_suite("Backtracking Correctness Tests");

  test_capture_restore_on_backtrack();
  test_nested_captures_deep_alternation();
  test_var_assignment_restore();
  test_loop_counter_restore();
}
