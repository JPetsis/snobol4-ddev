/*
 * test_choice_shortcircuit.c - Short-circuit empty counter/capture copies (W2d)
 *
 * A capture + alternation pattern with NO REPEAT / EMIT must behave bit-for-bit
 * identically to the legacy snapshot path, and the compact/trail choice path
 * must not copy any counter or write-log bytes (there are none). This guards
 * against regressions where the short-circuit guards (max_counter_used > 0 /
 * max_cap_used > 0) are removed or bypassed.
 */

#include "snobol/vm.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void emit_u8(uint8_t *bc, size_t *ip, uint8_t v) {
  bc[(*ip)++] = v;
}
static void emit_u32(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (v >> 24) & 0xFF;
  bc[(*ip)++] = (v >> 16) & 0xFF;
  bc[(*ip)++] = (v >> 8) & 0xFF;
  bc[(*ip)++] = v & 0xFF;
}

/* Emit an opcode byte, then reserve a 4-byte big-endian target slot and return
 * its offset. The VM decodes jump targets big-endian via read_u32, so patches
 * must use emit_u32 (NOT a native memcpy of a uint32_t). */
static size_t emit_jump_op(uint8_t *bc, size_t *ip, uint8_t op) {
  emit_u8(bc, ip, op);
  size_t t = *ip;
  emit_u32(bc, ip, 0);
  return t;
}

static void test_short_circuit_empty_copies(void) {
  /* Pattern: @1 ('a' | 'b') 'c'  -- a capture + alternation but NO REPEAT /
   * EMIT, so the choice records carry no counter/capture snapshot bytes. The
   * compact/trail path never copies them; the legacy path guards the memcpy
   * behind max_counter_used / max_cap_used > 0. */
  uint8_t bc[256];
  memset(bc, 0, sizeof(bc));
  size_t ip = 0;
  uint32_t lit_a_off = 240;
  uint32_t lit_b_off = 241;
  uint32_t lit_c_off = 242;
  bc[lit_a_off] = 'a';
  bc[lit_b_off] = 'b';
  bc[lit_c_off] = 'c';

  emit_u8(bc, &ip, OP_CAP_START);
  emit_u8(bc, &ip, 1);
  size_t a_tgt_ref = emit_jump_op(bc, &ip, OP_SPLIT);
  size_t b_tgt_ref = emit_jump_op(bc, &ip, OP_SPLIT);
  size_t a_ip = ip;
  emit_u8(bc, &ip, OP_LIT);
  emit_u32(bc, &ip, lit_a_off);
  emit_u32(bc, &ip, 1);
  size_t a_jmp_ref = emit_jump_op(bc, &ip, OP_JMP);
  size_t b_ip = ip;
  emit_u8(bc, &ip, OP_LIT);
  emit_u32(bc, &ip, lit_b_off);
  emit_u32(bc, &ip, 1);
  size_t b_jmp_ref = emit_jump_op(bc, &ip, OP_JMP);
  size_t join_ip = ip;
  uint32_t va = (uint32_t)a_ip;
  uint32_t vb = (uint32_t)b_ip;
  uint32_t vj = (uint32_t)join_ip;
  emit_u32(bc, &a_tgt_ref, va);
  emit_u32(bc, &b_tgt_ref, vb);
  emit_u32(bc, &a_jmp_ref, vj);
  emit_u32(bc, &b_jmp_ref, vj);
  emit_u8(bc, &ip, OP_LIT);
  emit_u32(bc, &ip, lit_c_off);
  emit_u32(bc, &ip, 1);
  emit_u8(bc, &ip, OP_CAP_END);
  emit_u8(bc, &ip, 1);
  emit_u8(bc, &ip, OP_ACCEPT);

  /* 'ac' should match; 'xc' should not. */
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = sizeof(bc);
  vm.use_compact_choice = true;
  vm.choices_arena = vm_arena_create();
  vm_trail_init(&vm);
  vm.trail_cap = 256;
  vm.trail = malloc(vm.trail_cap * sizeof(UndoRecord));
  vm.s = "ac";
  vm.len = 2;
  bool ok = vm_exec(&vm);
  test_assert(ok, "('a'|'b')'c' matches 'ac'");
  test_assert(vm.max_counter_used == 0,
              "non-REPEAT pattern has no loop counters (short-circuited)");
  test_assert(vm.choice_push_count >= 1,
              "alternation pushed at least one choice");
  test_assert(vm.cap_end[1] > vm.cap_start[1], "capture @1 was recorded");
  free(vm.trail);
  vm_arena_destroy(vm.choices_arena);

  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = sizeof(bc);
  vm.use_compact_choice = true;
  vm.choices_arena = vm_arena_create();
  vm_trail_init(&vm);
  vm.trail_cap = 256;
  vm.trail = malloc(vm.trail_cap * sizeof(UndoRecord));
  vm.s = "xc";
  vm.len = 2;
  ok = vm_exec(&vm);
  test_assert(ok == false, "('a'|'b')'c' rejects 'xc'");
  free(vm.trail);
  vm_arena_destroy(vm.choices_arena);
}

void test_choice_shortcircuit_suite(void) {
  test_suite("Choice Short-Circuit (W2d)");
  test_short_circuit_empty_copies();
}
