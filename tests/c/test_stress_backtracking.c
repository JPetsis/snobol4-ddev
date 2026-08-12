#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/vm.h"

// Simple stress: deep alternation pushes many choices, then fails and
// backtracks. We only assert it doesn't crash/corrupt and returns false.
static bool run_deep_alt_failure(size_t depth) {
  // Bytecode:
  //   for i in 0..depth-1: SPLIT to literal_i and next
  // Each literal is 1 byte and will not match input (input is "z"; literals are
  // "a"). This will cause backtracking over all choices.

  // We'll encode as:
  //   SPLIT a b
  //   LIT(off,len,'a')
  //   FAIL
  //   ... repeated with updated targets ...
  //   FAIL

  // For simplicity, build a chain where each SPLIT's 'a' jumps to its LIT,
  // and 'b' jumps to next SPLIT (or final FAIL).

  // This is not a correctness test; just allocation/deep-stack stress.

  // Estimate BC size.
  size_t per =
      1 + 4 + 4 /*split*/ + (1 + 4 + 4 + 1) /*lit inline*/ + 1 /*fail*/;
  size_t bc_cap = (depth * per) + 1;

  uint8_t *bc = malloc(bc_cap);
  if (!bc) {
    return false;
  }

  size_t ip = 0;

  // We'll record the positions where we need to patch SPLIT targets.
  size_t *split_a_pos = malloc(depth * sizeof(size_t));
  size_t *split_b_pos = malloc(depth * sizeof(size_t));
  size_t *lit_ip = malloc(depth * sizeof(size_t));
  if (!split_a_pos || !split_b_pos || !lit_ip) {
    free(bc);
    free(split_a_pos);
    free(split_b_pos);
    free(lit_ip);
    return false;
  }

  for (size_t i = 0; i < depth; i++) {
    // SPLIT
    bc[ip++] = OP_SPLIT;
    split_a_pos[i] = ip;
    ip += 4;
    split_b_pos[i] = ip;
    ip += 4;

    // LIT "a" inline
    lit_ip[i] = ip;
    bc[ip++] = OP_LIT;
    // offset = after header
    uint32_t off = (uint32_t)(ip + 4 + 4);
    bc[ip++] = (off >> 24) & 0xff;
    bc[ip++] = (off >> 16) & 0xff;
    bc[ip++] = (off >> 8) & 0xff;
    bc[ip++] = off & 0xff;
    bc[ip++] = 0;
    bc[ip++] = 0;
    bc[ip++] = 0;
    bc[ip++] = 1;
    bc[ip++] = 'a';

    // FAIL (so we always backtrack)
    bc[ip++] = OP_FAIL;
  }

  // Final FAIL so the last split can jump somewhere.
  size_t final_fail_ip = ip;
  bc[ip++] = OP_FAIL;

  // Patch split targets.
  for (size_t i = 0; i < depth; i++) {
    uint32_t a = (uint32_t)lit_ip[i];
    uint32_t b = (uint32_t)((i + 1 < depth) ? (lit_ip[i + 1] - (1 + 4 + 4))
                                            : final_fail_ip);

    // b should jump to next SPLIT, which starts right before next lit_ip's
    // SPLIT. We stored lit_ip as start of LIT; SPLIT is 1+4+4 bytes before it.
    if (i + 1 < depth) {
      b = (uint32_t)(lit_ip[i + 1] - (1 + 4 + 4));
    }

    bc[split_a_pos[i] + 0] = (a >> 24) & 0xff;
    bc[split_a_pos[i] + 1] = (a >> 16) & 0xff;
    bc[split_a_pos[i] + 2] = (a >> 8) & 0xff;
    bc[split_a_pos[i] + 3] = a & 0xff;

    bc[split_b_pos[i] + 0] = (b >> 24) & 0xff;
    bc[split_b_pos[i] + 1] = (b >> 16) & 0xff;
    bc[split_b_pos[i] + 2] = (b >> 8) & 0xff;
    bc[split_b_pos[i] + 3] = b & 0xff;
  }

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = ip;
  vm.s = "z";
  vm.len = 1;

  bool ok = vm_exec(&vm);

  free(bc);
  free(split_a_pos);
  free(split_b_pos);
  free(lit_ip);
  free(vm.choices);

  return ok;
}

int test_stress_backtracking_main(void) {
  // Depth should exceed the old MAX_CHOICES=128 so this test would have been
  // useless before; now it should always run and simply return false.
  size_t depth = 5000;
  bool ok = run_deep_alt_failure(depth);
  if (ok) {
    fprintf(stderr, "Expected failure; got success\n");
    return 1;
  }
  printf("  ✓ Stress backtracking depth=%zu did not crash\n", depth);
  return 0;
}
