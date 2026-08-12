/*
 * test_pattern_fence.c – Tests for OP_FENCE, OP_REM, OP_RPOS, OP_RTAB
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/ast.h"
#include "test_helpers.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_pattern_fence_suite(void) {
  test_suite("Pattern: FENCE / REM / RPOS / RTAB");

  /* FENCE test: after fence, no backtracking choice points remain */
  {
    /* Pattern: alt(fence(), fail()) - SPLIT creates choice, FENCE cuts it */
    ast_node_t *ast = snobol_ast_create_alt(snobol_ast_create_fence(),
                                            snobol_ast_create_fail());
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "test", 4, &match_len, &cap_count);
    test_assert(ok, "FENCE: match succeeds on SPLIT-then-FENCE path");
    snobol_ast_free(ast);
  }

  /* REM test: matches all remaining characters */
  {
    /* Pattern: cap(0, rem()) */
    ast_node_t *ast = snobol_ast_create_cap(0, snobol_ast_create_rem());
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "hello", 5, &match_len, &cap_count);
    test_assert(ok, "REM: matches entire subject");
    test_assert(match_len == 5, "REM: pos advanced to end");
    snobol_ast_free(ast);
  }

  /* RPOS test: succeed only at pos == len - n */
  {
    /* RPOS(3) on "hello world" (len=11): succeed only at pos=8 */
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(8);
    parts[1] = snobol_ast_create_rpos(3);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    const char *subj = "hello world";
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, subj, 11, &match_len, &cap_count);
    test_assert(ok, "RPOS(3): succeeds at pos 8 of 'hello world'");
    snobol_ast_free(ast);

    /* RPOS(3) with different pos should fail */
    parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(7);
    parts[1] = snobol_ast_create_rpos(3);
    ast = snobol_ast_create_concat(parts, 2);
    ok = run_ast_pattern(ast, subj, 11, &match_len, &cap_count);
    test_assert((!ok) != 0, "RPOS(3): fails when pos != 8");
    snobol_ast_free(ast);
  }

  /* RTAB test: advance to len-n position */
  {
    /* RTAB(2) on "hello world" (len=11): advance to pos=9 */
    ast_node_t *ast = snobol_ast_create_cap(0, snobol_ast_create_rtab(2));
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "hello world", 11, &match_len, &cap_count);
    test_assert(ok, "RTAB(2): succeeds on 'hello world'");
    test_assert(match_len == 9, "RTAB(2): pos advanced to 9 (11-2=9)");
    snobol_ast_free(ast);

    /* RTAB(0) = REM: advance to end */
    ast = snobol_ast_create_rtab(0);
    ok = run_ast_pattern(ast, "hello", 5, &match_len, &cap_count);
    test_assert(ok, "RTAB(0): succeeds (same as REM)");
    test_assert(match_len == 5, "RTAB(0): pos advanced to end");
    snobol_ast_free(ast);
  }
}
