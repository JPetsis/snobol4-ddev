#ifndef SNOBOL4_TEST_HELPERS_H
#define SNOBOL4_TEST_HELPERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "snobol/ast.h"
#include "snobol/compiler.h"
#include "snobol/vm.h"

static inline bool run_ast_pattern(ast_node_t *ast, const char *subject,
                                   size_t sub_len, int *out_match_len,
                                   int *out_cap_count) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  if (compile_ast_to_bytecode_c(ast, false, &bc, &bc_len) != 0) {
    return false;
  }
  if (!bc || bc_len == 0) {
    if (bc) {
      free(bc);
    }
    return false;
  }

  VM vm = {nullptr};
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = sub_len;

  snobol_buf out_buf = {nullptr};
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  bool ok = vm_exec(&vm);

  if (out_match_len) {
    *out_match_len = (int)vm.pos;
  }
  if (out_cap_count) {
    *out_cap_count = (int)vm.var_count;
  }

  snobol_buf_free(&out_buf);
  free(bc);
  return ok;
}

#endif /* SNOBOL4_TEST_HELPERS_H */
