#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_helpers.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_pattern_abort_suite(void) {
  test_suite("Pattern: ABORT");

  /* ABORT after matching 'a' on "abc" should terminate with failure */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_abort();
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert((!ok) != 0,
                "ABORT: 'a' ABORT on 'abc' terminates with failure");
    snobol_ast_free(ast);
  }

  /* ABORT in alternation should prevent backtracking */
  {
    ast_node_t **left_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    left_parts[0] = snobol_ast_create_lit("a", 1);
    left_parts[1] = snobol_ast_create_abort();
    ast_node_t *left = snobol_ast_create_concat(left_parts, 2);
    ast_node_t *ast =
        snobol_ast_create_alt(left, snobol_ast_create_lit("b", 1));
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert((!ok) != 0, "ABORT: prevents backtracking to alt branch");
    snobol_ast_free(ast);
  }
}

void test_pattern_fail_suite(void) {
  test_suite("Pattern: FAIL");

  /* 'a' FAIL on "abc": matches 'a', then FAIL forces backtrack */
  {
    ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_fail();
    parts[2] = snobol_ast_create_lit("x", 1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 3);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert((!ok) != 0, "FAIL: 'a' FAIL on 'abc' eventually fails");
    snobol_ast_free(ast);
  }

  /* FAIL with a choice point should backtrack.
   * Both arms match the same literal at the same position;
   * arm-a matches first, then FAIL forces backtrack to arm-b. */
  {
    ast_node_t **left_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    left_parts[0] = snobol_ast_create_lit("a", 1);
    left_parts[1] = snobol_ast_create_fail();
    ast_node_t *left = snobol_ast_create_concat(left_parts, 2);
    ast_node_t *ast =
        snobol_ast_create_alt(left, snobol_ast_create_lit("a", 1));
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert(ok, "FAIL: backtracking to alt branch succeeds");
    test_assert(match_len == 1, "FAIL: alt branch matched 'a' at pos=1");
    snobol_ast_free(ast);
  }
}

void test_pattern_succeed_suite(void) {
  test_suite("Pattern: SUCCEED");

  /* 'a' SUCCEED on "abc": succeeds immediately at pos=1 */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_succeed();
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert(ok, "SUCCEED: skips remaining pattern and succeeds");
    test_assert(match_len == 1, "SUCCEED: position stays at 1");
    snobol_ast_free(ast);
  }

  /* SUCCEED at start of string succeeds immediately */
  {
    ast_node_t *ast = snobol_ast_create_succeed();
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert(ok, "SUCCEED: succeeds immediately at pos=0");
    test_assert(match_len == 0, "SUCCEED: position stays at 0");
    snobol_ast_free(ast);
  }
}
