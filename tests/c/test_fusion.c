/**
 * @file test_fusion.c
 * @brief Tests for the SPLIT/ANY fusion pass (jit-split-tany-fusion change)
 *
 * Verifies that:
 * 5.1  'a' | 'b'         → bytecode contains OP_ANY, no OP_SPLIT
 * 5.2  fused OP_ANY matches 'a', 'b', rejects 'c'
 * 5.3  'ab' | 'c'        → bytecode retains OP_SPLIT (multi-char arm,
 * ineligible) 5.4  ANY(cc)|NOTANY(cc2) would retain OP_SPLIT; we test NOTANY
 * arm is not fused 5.5  'a' | 'b' | 'c'  → exactly one OP_ANY in bytecode (N=3)
 * 5.6  fused N=3 matches all three chars, rejects a fourth
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/ast.h"
#include "snobol/compiler.h"
#include "snobol/vm.h"

/* Test framework */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* ---- helpers ---- */

/* Count occurrences of a given opcode byte in the opcode portion of bytecode.
 * The opcode portion ends at (but does not include) the charclass table.
 * We detect the end by scanning for OP_ACCEPT; everything after is table data.
 * Simple heuristic: just count byte value in the first bc_len bytes. */
static int count_opcode_in_range(const uint8_t *bc, size_t bc_len, uint8_t op) {
  int count = 0;
  for (size_t i = 0; i < bc_len; i++) {
    if (bc[i] == op) {
      count++;
    }
  }
  return count;
}

/* Run a compiled bytecode pattern against subject s; return true if matches. */
static bool run_match(const uint8_t *bc, size_t bc_len, const char *s) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = s;
  vm.len = strlen(s);
  vm.pos = 0;
  vm.ip = 0;
  bool result = vm_run(&vm);
  if (vm.choices) {
    free(vm.choices);
    vm.choices = NULL;
  }
  return result;
}

/* ---- 5.1  'a' | 'b' → OP_ANY, no OP_SPLIT ---- */
static void test_fusion_ab_has_any_no_split(void) {
  test_suite("Fusion: 'a'|'b' → OP_ANY, no OP_SPLIT");

  ast_node_t *left = snobol_ast_create_lit("a", 1);
  ast_node_t *right = snobol_ast_create_lit("b", 1);
  ast_node_t *alt = snobol_ast_create_alt(left, right);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt, false, &bc, &bc_len);
  test_assert(rc == 0, "compile 'a'|'b' succeeds");
  snobol_ast_free(alt);

  if (rc == 0 && bc) {
    /* Count OP_SPLIT (value 3) and OP_ANY (value 5) bytes in bytecode */
    int split_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_SPLIT);
    int any_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_ANY);

    test_assert(split_count == 0, "fusion: no OP_SPLIT in 'a'|'b' bytecode");
    test_assert(any_count >= 1,
                "fusion: at least one OP_ANY in 'a'|'b' bytecode");
    free(bc);
  }
}

/* ---- 5.2  Fused 'a'|'b' matches 'a', 'b', rejects 'c' ---- */
static void test_fusion_ab_semantics(void) {
  test_suite("Fusion: 'a'|'b' semantics");

  ast_node_t *left = snobol_ast_create_lit("a", 1);
  ast_node_t *right = snobol_ast_create_lit("b", 1);
  ast_node_t *alt = snobol_ast_create_alt(left, right);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt, false, &bc, &bc_len);
  snobol_ast_free(alt);
  if (rc != 0 || !bc) {
    test_assert(false, "compile failed");
    return;
  }

  test_assert(run_match(bc, bc_len, "a"), "fused 'a'|'b' matches 'a'");
  test_assert(run_match(bc, bc_len, "b"), "fused 'a'|'b' matches 'b'");
  test_assert((!run_match(bc, bc_len, "c")) != 0, "fused 'a'|'b' rejects 'c'");
  free(bc);
}

/* ---- 5.3  'ab' | 'c' → retains OP_SPLIT (multi-char LIT is ineligible) ----
 */
static void test_fusion_multichar_not_fused(void) {
  test_suite("Fusion: 'ab'|'c' retains OP_SPLIT");

  ast_node_t *left = snobol_ast_create_lit("ab", 2);
  ast_node_t *right = snobol_ast_create_lit("c", 1);
  ast_node_t *alt = snobol_ast_create_alt(left, right);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt, false, &bc, &bc_len);
  snobol_ast_free(alt);
  if (rc != 0 || !bc) {
    test_assert(false, "compile failed");
    return;
  }

  int split_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_SPLIT);
  test_assert(split_count >= 1, "ineligible 'ab'|'c' retains OP_SPLIT");
  free(bc);
}

/* ---- 5.4  NOTANY arm: test that NOTANY arm does NOT get fused with LIT ----
 */
static void test_fusion_notany_not_fused(void) {
  test_suite("Fusion: NOTANY arm not fused with LIT");

  /* Build: LIT('a') | NOTANY("b")  — arm types differ semantically.
   * Since our fusion only handles LIT and ANY (not NOTANY) arms, the
   * NOTANY arm must cause the SPLIT to be retained. */
  ast_node_t *left = snobol_ast_create_lit("a", 1);
  ast_node_t *right = snobol_ast_create_notany("b", 1);
  ast_node_t *alt = snobol_ast_create_alt(left, right);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt, false, &bc, &bc_len);
  snobol_ast_free(alt);
  if (rc != 0 || !bc) {
    test_assert(false, "compile failed");
    return;
  }

  int split_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_SPLIT);
  test_assert(split_count >= 1,
              "LIT|NOTANY retains OP_SPLIT (NOTANY arm not fused)");
  free(bc);
}

/* ---- 5.5  'a' | 'b' | 'c' → exactly one OP_ANY in bytecode (N=3) ---- */
static void test_fusion_three_way_one_any(void) {
  test_suite("Fusion: 'a'|'b'|'c' → one OP_ANY");

  /* Build right-associative: 'a' | ('b' | 'c') */
  ast_node_t *bc_node = snobol_ast_create_alt(snobol_ast_create_lit("b", 1),
                                              snobol_ast_create_lit("c", 1));
  ast_node_t *alt3 =
      snobol_ast_create_alt(snobol_ast_create_lit("a", 1), bc_node);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt3, false, &bc, &bc_len);
  snobol_ast_free(alt3);
  if (rc != 0 || !bc) {
    test_assert(false, "compile failed");
    return;
  }

  int split_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_SPLIT);
  int any_count = count_opcode_in_range(bc, bc_len, (uint8_t)OP_ANY);

  test_assert(split_count == 0, "fusion N=3: no OP_SPLIT");
  test_assert(any_count == 1, "fusion N=3: exactly one OP_ANY");
  free(bc);
}

/* ---- 5.6  Fused N=3 semantics ---- */
static void test_fusion_three_way_semantics(void) {
  test_suite("Fusion: 'a'|'b'|'c' semantics");

  ast_node_t *bc_node = snobol_ast_create_alt(snobol_ast_create_lit("b", 1),
                                              snobol_ast_create_lit("c", 1));
  ast_node_t *alt3 =
      snobol_ast_create_alt(snobol_ast_create_lit("a", 1), bc_node);

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(alt3, false, &bc, &bc_len);
  snobol_ast_free(alt3);
  if (rc != 0 || !bc) {
    test_assert(false, "compile failed");
    return;
  }

  test_assert(run_match(bc, bc_len, "a"), "fused N=3 matches 'a'");
  test_assert(run_match(bc, bc_len, "b"), "fused N=3 matches 'b'");
  test_assert(run_match(bc, bc_len, "c"), "fused N=3 matches 'c'");
  test_assert((!run_match(bc, bc_len, "d")) != 0, "fused N=3 rejects 'd'");
  free(bc);
}

void test_fusion_suite(void) {
  test_fusion_ab_has_any_no_split();
  test_fusion_ab_semantics();
  test_fusion_multichar_not_fused();
  test_fusion_notany_not_fused();
  test_fusion_three_way_one_any();
  test_fusion_three_way_semantics();
}
