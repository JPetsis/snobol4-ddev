#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/ast.h"
#include "test_helpers.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_pattern_pos_suite(void) {
  test_suite("Pattern: POS");

  /* POS(3) on "abcde": match at position 3 */
  {
    int match_len = 0;
    int cap_count = 0;
    ast_node_t *ast = snobol_ast_create_pos(3);
    bool ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert((!ok) != 0, "POS(3): fails at position 0");
    snobol_ast_free(ast);

    /* With LEN(1) first, then POS(3) should fail at pos=1 */
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(1);
    parts[1] = snobol_ast_create_pos(3);
    ast = snobol_ast_create_concat(parts, 2);
    ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert((!ok) != 0, "POS(3): fails at position 1");
    snobol_ast_free(ast);

    /* LEN(3) POS(3) LEN(1) succeeds at pos=4 */
    parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(3);
    parts[1] = snobol_ast_create_pos(3);
    parts[2] = snobol_ast_create_len(1);
    ast = snobol_ast_create_concat(parts, 3);
    ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert(ok, "POS(3): succeeds after LEN(3) at pos=3");
    test_assert(match_len == 4, "POS(3): pos=4 after matching 'd'");
    snobol_ast_free(ast);
  }

  /* POS beyond string length should never match */
  {
    int match_len = 0;
    int cap_count = 0;
    ast_node_t *ast = snobol_ast_create_pos(10);
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert((!ok) != 0, "POS(10): fails on 'abc' (beyond length)");
    snobol_ast_free(ast);
  }

  /* POS(0) should match at position 0 */
  {
    int match_len = 0;
    int cap_count = 0;
    ast_node_t *ast = snobol_ast_create_pos(0);
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert(ok, "POS(0): succeeds at position 0");
    snobol_ast_free(ast);
  }
}

void test_pattern_tab_suite(void) {
  test_suite("Pattern: TAB");

  /* TAB(2) LEN(2) on "abcde": advance to pos=2, match "cd" */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_tab(2);
    parts[1] = snobol_ast_create_len(2);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert(ok, "TAB(2): advances cursor and succeeds");
    test_assert(match_len == 4, "TAB(2): pos=4 after LEN(2)");
    snobol_ast_free(ast);
  }

  /* TAB(0) should keep pos at 0 (beginning) */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_tab(0);
    parts[1] = snobol_ast_create_len(2);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert(ok, "TAB(0): keeps pos at beginning");
    test_assert(match_len == 2, "TAB(0): pos=2 after LEN(2)");
    snobol_ast_free(ast);
  }

  /* TAB(10) on "abc" should fail (beyond string length) */
  {
    ast_node_t *ast = snobol_ast_create_tab(10);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abc", 3, &match_len, &cap_count);
    test_assert((!ok) != 0, "TAB(10): fails on 'abc' (beyond length)");
    snobol_ast_free(ast);
  }

  /* TAB from beyond target position should fail */
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_len(3);
    parts[1] = snobol_ast_create_tab(1);
    ast_node_t *ast = snobol_ast_create_concat(parts, 2);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(ast, "abcde", 5, &match_len, &cap_count);
    test_assert((!ok) != 0, "TAB(1): fails when pos(3) > target(1)");
    snobol_ast_free(ast);
  }
}
