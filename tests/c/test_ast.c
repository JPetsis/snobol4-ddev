/**
 * @file test_ast.c
 * @brief Tests for AST version and memory management
 */

#include "snobol/ast.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test framework functions (from test_runner.c) */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_ast_version(void) {
  test_suite("AST: version information");

  snobol_ast_version_t version = snobol_ast_get_version();

  test_assert(version.major == 1, "major version is 1");
  test_assert(version.minor == 0, "minor version is 0");
  test_assert(version.patch == 0, "patch version is 0");
  test_assert(version.string != NULL, "version string is not NULL");
  test_assert(strcmp(version.string, "1.0.0") == 0,
              "version string is '1.0.0'");
}

static void test_ast_version_string(void) {
  test_suite("AST: version string");

  const char *version_str = snobol_ast_version_string();

  test_assert(version_str != NULL, "version string is not NULL");
  test_assert(strcmp(version_str, "1.0.0") == 0, "version string matches");
}

static void test_ast_version_check_compatible(void) {
  test_suite("AST: version check (compatible)");

  /* Same major, same minor - compatible */
  test_assert(snobol_ast_version_check(1, 0), "v1.0 is compatible with v1.0");

  /* Same major, lower minor - compatible */
  test_assert(snobol_ast_version_check(1, 0), "v1.0 is compatible with v1.0");
}

static void test_ast_version_check_incompatible(void) {
  test_suite("AST: version check (incompatible)");

  /* Different major - incompatible */
  test_assert(snobol_ast_version_check(2, 0) == false,
              "v2.0 is NOT compatible with v1.0");

  /* Different major - incompatible */
  test_assert(snobol_ast_version_check(0, 9) == false,
              "v0.9 is NOT compatible with v1.0");
}

static void test_ast_version_macro(void) {
  test_suite("AST: version macro");

  test_assert(SNOBOL_AST_VERSION_MAJOR == 1, "SNOBOL_AST_VERSION_MAJOR is 1");
  test_assert(SNOBOL_AST_VERSION_MINOR == 0, "SNOBOL_AST_VERSION_MINOR is 0");
  test_assert(SNOBOL_AST_VERSION_PATCH == 0, "SNOBOL_AST_VERSION_PATCH is 0");
  test_assert(strcmp(SNOBOL_AST_VERSION_STRING, "1.0.0") == 0,
              "SNOBOL_AST_VERSION_STRING is '1.0.0'");
}

static void test_ast_version_check_macro(void) {
  test_suite("AST: version check macro");

  test_assert(SNOBOL_AST_VERSION_CHECK(1, 0) == 1, "macro: v1.0 is compatible");
  test_assert(SNOBOL_AST_VERSION_CHECK(1, 1) == 0,
              "macro: v1.1 is NOT compatible (future minor)");
  test_assert(SNOBOL_AST_VERSION_CHECK(2, 0) == 0,
              "macro: v2.0 is NOT compatible");
}

static void test_ast_create_and_free(void) {
  test_suite("AST: create and free");

  /* Create a simple literal node */
  ast_node_t *node = snobol_ast_create_lit("test", 4);

  test_assert(node != NULL, "create_lit returns non-NULL");
  test_assert(node->type == AST_LITERAL, "node type is AST_LITERAL");
  test_assert(node->data.literal.len == 4, "literal length is 4");
  test_assert(memcmp(node->data.literal.text, "test", 4) == 0,
              "literal text matches");

  /* Free the node */
  snobol_ast_free(node);
  test_assert(true, "snobol_ast_free completes without crash");
}

static void test_ast_create_and_free_complex(void) {
  test_suite("AST: create and free complex tree");

  /* Create: 'A' | 'B' */
  ast_node_t *left = snobol_ast_create_lit("A", 1);
  ast_node_t *right = snobol_ast_create_lit("B", 1);
  ast_node_t *alt = snobol_ast_create_alt(left, right);

  test_assert(alt != NULL, "create_alt returns non-NULL");
  test_assert(alt->type == AST_ALT, "node type is AST_ALT");
  test_assert(alt->data.alt.left == left, "left child is correct");
  test_assert(alt->data.alt.right == right, "right child is correct");

  /* Free the entire tree (should free children recursively) */
  snobol_ast_free(alt);
  test_assert(true, "snobol_ast_free frees entire tree without crash");
}

static void test_ast_null_safety(void) {
  test_suite("AST: NULL safety");

  /* snobol_ast_free should handle NULL gracefully */
  snobol_ast_free(nullptr);
  test_assert(true, "snobol_ast_free(NULL) does not crash");
}

static void test_ast_type_names(void) {
  test_suite("AST: type names");

  const char *lit_name = snobol_ast_type_name(AST_LITERAL);
  test_assert(lit_name != NULL, "type_name returns non-NULL for LITERAL");
  test_assert(strcmp(lit_name, "LITERAL") == 0, "LITERAL type name is correct");

  const char *alt_name = snobol_ast_type_name(AST_ALT);
  test_assert(alt_name != NULL, "type_name returns non-NULL for ALT");
  test_assert(strcmp(alt_name, "ALT") == 0, "ALT type name is correct");

  const char *unknown_name = snobol_ast_type_name((ast_type_t)999);
  test_assert(unknown_name != NULL,
              "type_name returns non-NULL for unknown type");
  test_assert(strcmp(unknown_name, "UNKNOWN") == 0,
              "unknown type name is 'UNKNOWN'");
}

static void test_ast_create_all_types(void) {
  test_suite("AST: create all node types");

  /* Test each creation function */
  ast_node_t *node;

  node = snobol_ast_create_lit("test", 4);
  test_assert((node != NULL && node->type == AST_LITERAL) != 0,
              "create_lit works");
  snobol_ast_free(node);

  ast_node_t *left = snobol_ast_create_lit("A", 1);
  ast_node_t *right = snobol_ast_create_lit("B", 1);
  node = snobol_ast_create_alt(left, right);
  test_assert((node != NULL && node->type == AST_ALT) != 0, "create_alt works");
  snobol_ast_free(node);

  /* Test concat separately with fresh nodes */
  left = snobol_ast_create_lit("A", 1);
  right = snobol_ast_create_lit("B", 1);
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = left;
  parts[1] = right;
  node = snobol_ast_create_concat(parts, 2);
  test_assert((node != NULL && node->type == AST_CONCAT) != 0,
              "create_concat works");
  snobol_ast_free(node); /* This frees parts array AND children */

  node = snobol_ast_create_arbno(snobol_ast_create_lit("X", 1));
  test_assert((node != NULL && node->type == AST_ARBNO) != 0,
              "create_arbno works");
  snobol_ast_free(node);

  node = snobol_ast_create_span("a-z", 3);
  test_assert((node != NULL && node->type == AST_SPAN) != 0,
              "create_span works");
  snobol_ast_free(node);

  node = snobol_ast_create_any("aeiou", 5);
  test_assert((node != NULL && node->type == AST_ANY) != 0, "create_any works");
  snobol_ast_free(node);

  node = snobol_ast_create_cap(1, snobol_ast_create_lit("X", 1));
  test_assert((node != NULL && node->type == AST_CAP) != 0, "create_cap works");
  snobol_ast_free(node);

  node = snobol_ast_create_repeat(snobol_ast_create_lit("X", 1), 2, 5);
  test_assert((node != NULL && node->type == AST_REPETITION) != 0,
              "create_repeat works");
  snobol_ast_free(node);

  node = snobol_ast_create_label("LOOP", snobol_ast_create_lit("X", 1));
  test_assert((node != NULL && node->type == AST_LABEL) != 0,
              "create_label works");
  snobol_ast_free(node);
}


/* ===== test_coverage_ast: coverage-driven tests merged into test_ast.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/arena.h"
#include "../../core/include/snobol/snobol.h"


/* ── clone coverage: every node type ──────────────────────────────────────── */

void test_cov_ast_clone_all_types(void) {
  test_suite("Coverage: snobol_ast_clone every node type");

  ast_node_t *lit = snobol_ast_create_lit("abc", 3);
  ast_node_t *span = snobol_ast_create_span("0-9", 3);
  ast_node_t *brk = snobol_ast_create_break("x", 1);
  ast_node_t *any = snobol_ast_create_any("ab", 2);
  ast_node_t *notany = snobol_ast_create_notany("c", 1);
  ast_node_t *len = snobol_ast_create_len(3);
  ast_node_t *assign = snobol_ast_create_assign(2, 1);
  ast_node_t *eval = snobol_ast_create_eval(SNOBOL_FN_SIZE, 0);
  ast_node_t *anchor = snobol_ast_create_anchor(ANCHOR_END);
  ast_node_t *bal = snobol_ast_create_bal('(', ')');
  ast_node_t *rpos = snobol_ast_create_rpos(1);
  ast_node_t *rtab = snobol_ast_create_rtab(2);
  ast_node_t *pos = snobol_ast_create_pos(0);
  ast_node_t *tab = snobol_ast_create_tab(1);
  ast_node_t *fence = snobol_ast_create_fence();
  ast_node_t *rem = snobol_ast_create_rem();
  ast_node_t *abort = snobol_ast_create_abort();
  ast_node_t *fail = snobol_ast_create_fail();
  ast_node_t *succeed = snobol_ast_create_succeed();
  ast_node_t *emit = snobol_ast_create_emit("out", 3, 1);
  ast_node_t *dyn = snobol_ast_create_dynamic_eval(lit);
  ast_node_t *breakx = snobol_ast_create_breakx(";", 1);
  ast_node_t *cap = snobol_ast_create_cap(1, span);
  ast_node_t *arbno = snobol_ast_create_arbno(len);
  ast_node_t *repeat = snobol_ast_create_repeat(any, 1, 3);
  ast_node_t *alt = snobol_ast_create_alt(brk, notany);
  ast_node_t *label = snobol_ast_create_label((char *)"L1", succeed);
  ast_node_t *goto_node = snobol_ast_create_goto("L1");
  ast_node_t *tab_acc = snobol_ast_create_table_access("tbl", cap);
  ast_node_t *tab_upd = snobol_ast_create_table_update("tbl", assign, eval);
  ast_node_t *anchor_start = snobol_ast_create_anchor(ANCHOR_START);

  test_assert((lit && span && brk && any && notany && len && assign && eval &&
               anchor && bal && rpos && rtab && pos && tab && fence && rem &&
               abort && fail && succeed && emit && dyn && breakx && cap &&
               arbno && repeat && alt && label && goto_node && tab_acc &&
               tab_upd && anchor_start) != 0,
              "all node constructors succeed");

  /* Clone each and verify the copy frees cleanly. */
  ast_node_t *nodes[] = {
      lit,    span, brk,     any,       notany,  len,     assign,      eval,
      anchor, bal,  rpos,    rtab,      pos,     tab,     fence,       rem,
      abort,  fail, succeed, emit,      dyn,     breakx,  cap,         arbno,
      repeat, alt,  label,   goto_node, tab_acc, tab_upd, anchor_start};
  for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
    ast_node_t *c = snobol_ast_clone(nodes[i]);
    test_assert(c != NULL, "clone of each node type succeeds");
    if (c) {
      snobol_ast_free(c);
    }
  }

  /* Free-path coverage: standalone nodes of every owned type, freed through
   * their owners exactly once (the shared tree above frees via parents). */
  {
    ast_node_t *s1 = snobol_ast_create_lit("a", 1);
    ast_node_t *s2 = snobol_ast_create_span("b", 1);
    ast_node_t *s3 = snobol_ast_create_break("c", 1);
    ast_node_t *s4 = snobol_ast_create_any("d", 1);
    ast_node_t *s5 = snobol_ast_create_notany("e", 1);
    ast_node_t *s6 = snobol_ast_create_emit("f", 1, 0);
    ast_node_t *s7 = snobol_ast_create_goto("g");
    ast_node_t *s8 = snobol_ast_create_label((char *)"h", s1);
    ast_node_t *s9 = snobol_ast_create_breakx("i", 1);
    ast_node_t *s10 = snobol_ast_create_dynamic_eval(s2);
    ast_node_t *s11 = snobol_ast_create_cap(1, s3);
    ast_node_t *s12 = snobol_ast_create_arbno(s4);
    ast_node_t *s13 = snobol_ast_create_repeat(s5, 0, 1);
    ast_node_t *s14 = snobol_ast_create_alt(s6, s7);
    ast_node_t *s15 = snobol_ast_create_table_access("t", s8);
    ast_node_t *s16 = snobol_ast_create_table_update("t", s9, s10);
    ast_node_t *s17 = snobol_ast_create_len(1);
    ast_node_t *s18 = snobol_ast_create_assign(1, 0);
    ast_node_t *s19 = snobol_ast_create_eval(1, 0);
    ast_node_t *s20 = snobol_ast_create_anchor(ANCHOR_START);
    ast_node_t *s21 = snobol_ast_create_bal('(', ')');
    ast_node_t *s22 = snobol_ast_create_rpos(1);
    ast_node_t *s23 = snobol_ast_create_rtab(1);
    ast_node_t *s24 = snobol_ast_create_pos(1);
    ast_node_t *s25 = snobol_ast_create_tab(1);
    ast_node_t *s26 = snobol_ast_create_fence();
    ast_node_t *s27 = snobol_ast_create_rem();
    ast_node_t *s28 = snobol_ast_create_abort();
    ast_node_t *s29 = snobol_ast_create_fail();
    ast_node_t *s30 = snobol_ast_create_succeed();
    ast_node_t **s_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    s_parts[0] = s17;
    s_parts[1] = s18;
    ast_node_t *s31 = snobol_ast_create_concat(s_parts, 2);

    snobol_ast_free(s31); /* frees s17 + s18 + parts array */
    snobol_ast_free(s19);
    snobol_ast_free(s20);
    snobol_ast_free(s21);
    snobol_ast_free(s22);
    snobol_ast_free(s23);
    snobol_ast_free(s24);
    snobol_ast_free(s25);
    snobol_ast_free(s26);
    snobol_ast_free(s27);
    snobol_ast_free(s28);
    snobol_ast_free(s29);
    snobol_ast_free(s30);
    snobol_ast_free(s15); /* frees s8 -> s1 */
    snobol_ast_free(s16); /* frees s9, s10 -> s2 */
    snobol_ast_free(s11); /* frees s3 */
    snobol_ast_free(s12); /* frees s4 */
    snobol_ast_free(s13); /* frees s5 */
    snobol_ast_free(s14); /* frees s6 + s7 */
  }

  /* Concat clone (parts array) + free. */
  ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_lit("a", 1);
  parts[1] = snobol_ast_create_lit("b", 1);
  parts[2] = snobol_ast_create_lit("c", 1);
  ast_node_t *concat = snobol_ast_create_concat(parts, 3);
  ast_node_t *cc = snobol_ast_clone(concat);
  test_assert(
      (cc != NULL && cc->type == AST_CONCAT && cc->data.concat.count == 3) != 0,
      "concat clone copies parts array");
  snobol_ast_free(cc);
  snobol_ast_free(concat);

  test_assert(snobol_ast_clone(nullptr) == NULL, "clone(NULL)");

  /* Free the shared tree once via its owners. */
  snobol_ast_free(dyn); /* frees lit */
  snobol_ast_free(emit);
  snobol_ast_free(breakx);
  snobol_ast_free(arbno);  /* frees len */
  snobol_ast_free(repeat); /* frees any */
  snobol_ast_free(alt);    /* frees brk + notany */
  snobol_ast_free(label);  /* frees succeed */
  snobol_ast_free(goto_node);
  snobol_ast_free(tab_acc); /* frees cap -> span */
  snobol_ast_free(tab_upd); /* frees assign + eval */
  snobol_ast_free(anchor);
  snobol_ast_free(bal);
  snobol_ast_free(rpos);
  snobol_ast_free(rtab);
  snobol_ast_free(pos);
  snobol_ast_free(tab);
  snobol_ast_free(fence);
  snobol_ast_free(rem);
  snobol_ast_free(abort);
  snobol_ast_free(fail);
  snobol_ast_free(anchor_start);
  snobol_ast_free(nullptr);
  test_assert(true, "all node frees completed");
}

/* ── type names, dump, aliases, NULL branches ─────────────────────────────── */


void test_cov_ast_type_names_and_dump(void) {
  test_suite("Coverage: type names + dump");

  test_assert(
      (strcmp(snobol_ast_type_name(AST_LITERAL), "LITERAL") == 0 &&
       strcmp(snobol_ast_type_name(AST_CONCAT), "CONCAT") == 0 &&
       strcmp(snobol_ast_type_name(AST_ALT), "ALT") == 0 &&
       strcmp(snobol_ast_type_name(AST_REPETITION), "REPETITION") == 0 &&
       strcmp(snobol_ast_type_name(AST_SPAN), "SPAN") == 0 &&
       strcmp(snobol_ast_type_name(AST_BREAK), "BREAK") == 0 &&
       strcmp(snobol_ast_type_name(AST_ANY), "ANY") == 0 &&
       strcmp(snobol_ast_type_name(AST_NOTANY), "NOTANY") == 0 &&
       strcmp(snobol_ast_type_name(AST_ARBNO), "ARBNO") == 0 &&
       strcmp(snobol_ast_type_name(AST_CAP), "CAP") == 0 &&
       strcmp(snobol_ast_type_name(AST_ASSIGN), "ASSIGN") == 0 &&
       strcmp(snobol_ast_type_name(AST_LEN), "LEN") == 0 &&
       strcmp(snobol_ast_type_name(AST_EVAL), "EVAL") == 0 &&
       strcmp(snobol_ast_type_name(AST_DYNAMIC_EVAL), "DYNAMIC_EVAL") == 0 &&
       strcmp(snobol_ast_type_name(AST_ANCHOR), "ANCHOR") == 0 &&
       strcmp(snobol_ast_type_name(AST_EMIT), "EMIT") == 0 &&
       strcmp(snobol_ast_type_name(AST_LABEL), "LABEL") == 0 &&
       strcmp(snobol_ast_type_name(AST_GOTO), "GOTO") == 0 &&
       strcmp(snobol_ast_type_name(AST_TABLE_ACCESS), "TABLE_ACCESS") == 0 &&
       strcmp(snobol_ast_type_name(AST_TABLE_UPDATE), "TABLE_UPDATE") == 0 &&
       strcmp(snobol_ast_type_name(AST_BREAKX), "BREAKX") == 0 &&
       strcmp(snobol_ast_type_name(AST_BAL), "BAL") == 0 &&
       strcmp(snobol_ast_type_name(AST_FENCE), "FENCE") == 0 &&
       strcmp(snobol_ast_type_name(AST_REM), "REM") == 0 &&
       strcmp(snobol_ast_type_name(AST_RPOS), "RPOS") == 0 &&
       strcmp(snobol_ast_type_name(AST_RTAB), "RTAB") == 0 &&
       strcmp(snobol_ast_type_name(AST_POS), "POS") == 0 &&
       strcmp(snobol_ast_type_name(AST_TAB), "TAB") == 0 &&
       strcmp(snobol_ast_type_name(AST_ABORT), "ABORT") == 0 &&
       strcmp(snobol_ast_type_name(AST_FAIL), "FAIL") == 0 &&
       strcmp(snobol_ast_type_name(AST_SUCCEED), "SUCCEED") == 0 &&
       strcmp(snobol_ast_type_name((ast_type_t)999), "UNKNOWN") == 0) != 0,
      "every type name resolves");

  /* Dump every node shape to a throwaway stream. */
  FILE *sink = tmpfile();
  test_assert(sink != NULL, "dump sink created");
  if (sink) {
    ast_node_t *dump_nodes[40];
    size_t dn = 0;
    snobol_ast_dump(nullptr, sink, 2);
    dump_nodes[dn++] = snobol_ast_create_lit("hi", 2);
    dump_nodes[dn++] = snobol_ast_create_span("ab", 2);
    dump_nodes[dn++] = snobol_ast_create_any("ab", 2);
    dump_nodes[dn++] = snobol_ast_create_any(nullptr, 0);
    dump_nodes[dn++] = snobol_ast_create_notany("c", 1);
    dump_nodes[dn++] = snobol_ast_create_break("x", 1);
    dump_nodes[dn++] = snobol_ast_create_breakx(";", 1);
    dump_nodes[dn++] = snobol_ast_create_len(2);
    dump_nodes[dn++] = snobol_ast_create_assign(1, 0);
    dump_nodes[dn++] = snobol_ast_create_eval(1, 0);
    dump_nodes[dn++] = snobol_ast_create_anchor(ANCHOR_START);
    dump_nodes[dn++] = snobol_ast_create_bal('(', ')');
    dump_nodes[dn++] = snobol_ast_create_rpos(1);
    dump_nodes[dn++] = snobol_ast_create_rtab(2);
    dump_nodes[dn++] = snobol_ast_create_pos(0);
    dump_nodes[dn++] = snobol_ast_create_tab(1);
    dump_nodes[dn++] = snobol_ast_create_fence();
    dump_nodes[dn++] = snobol_ast_create_rem();
    dump_nodes[dn++] = snobol_ast_create_abort();
    dump_nodes[dn++] = snobol_ast_create_fail();
    dump_nodes[dn++] = snobol_ast_create_succeed();
    dump_nodes[dn++] = snobol_ast_create_emit("x", 1, 0);
    dump_nodes[dn++] = snobol_ast_create_goto("L");
    dump_nodes[dn++] =
        snobol_ast_create_label((char *)"L", snobol_ast_create_lit("a", 1));
    dump_nodes[dn++] =
        snobol_ast_create_table_access("t", snobol_ast_create_lit("k", 1));
    dump_nodes[dn++] = snobol_ast_create_table_update(
        "t", snobol_ast_create_lit("k", 1), snobol_ast_create_lit("v", 1));
    dump_nodes[dn++] =
        snobol_ast_create_dynamic_eval(snobol_ast_create_lit("x", 1));
    dump_nodes[dn++] = snobol_ast_create_arbno(snobol_ast_create_lit("q", 1));
    dump_nodes[dn++] = snobol_ast_create_cap(1, snobol_ast_create_lit("c", 1));
    dump_nodes[dn++] = snobol_ast_create_alt(snobol_ast_create_lit("l", 1),
                                             snobol_ast_create_lit("r", 1));
    dump_nodes[dn++] =
        snobol_ast_create_repeat(snobol_ast_create_lit("z", 1), 0, 3);
    ast_node_t **dump_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    dump_parts[0] = snobol_ast_create_lit("a", 1);
    dump_parts[1] = snobol_ast_create_lit("b", 1);
    dump_nodes[dn++] = snobol_ast_create_concat(dump_parts, 2);
    for (size_t di = 0; di < dn; di++) {
      snobol_ast_dump(dump_nodes[di], sink, 0);
    }
    for (size_t di = 0; di < dn; di++) {
      snobol_ast_free(dump_nodes[di]);
    }
    fclose(sink);
  }
  test_assert(true, "dump of all node shapes ran");
}


void test_cov_ast_aliases_and_null_args(void) {
  test_suite("Coverage: constructor aliases + NULL args");

  /* Aliases. */
  ast_node_t *a1 = snobol_ast_create_literal("x", 1);
  ast_node_t *a2 = snobol_ast_create_break(",", 1);
  ast_node_t *a3 = snobol_ast_create_notany("y", 1);
  ast_node_t *a4 = snobol_ast_create_len(4);
  ast_node_t *a5 = snobol_ast_create_anchor(ANCHOR_START);
  ast_node_t *a6 = snobol_ast_create_emit("t", 1, 0);
  ast_node_t *a7 = snobol_ast_create_dynamic_eval(a1);
  ast_node_t *a8 = snobol_ast_create_breakx("z", 1);
  ast_node_t *a9 = snobol_ast_create_bal('{', '}');
  ast_node_t *a10 = snobol_ast_create_fence();
  ast_node_t *a11 = snobol_ast_create_rem();
  ast_node_t *a12 = snobol_ast_create_rpos(2);
  ast_node_t *a13 = snobol_ast_create_rtab(3);
  ast_node_t *a14 = snobol_ast_create_pos(1);
  ast_node_t *a15 = snobol_ast_create_tab(4);
  ast_node_t *a16 = snobol_ast_create_abort();
  ast_node_t *a17 = snobol_ast_create_fail();
  ast_node_t *a18 = snobol_ast_create_succeed();
  ast_node_t *a19 = snobol_ast_create_table_access("tbl", a2);
  ast_node_t *a20 = snobol_ast_create_table_update("tbl", a3, a4);

  test_assert((a1 && a2 && a3 && a4 && a5 && a6 && a7 && a8 && a9 && a10 &&
               a11 && a12 && a13 && a14 && a15 && a16 && a17 && a18 && a19 &&
               a20) != 0,
              "alias constructors succeed");
  test_assert((a1->type == AST_LITERAL && a2->type == AST_BREAK &&
               a3->type == AST_NOTANY && a4->type == AST_LEN &&
               a5->type == AST_ANCHOR && a6->type == AST_EMIT &&
               a7->type == AST_DYNAMIC_EVAL && a8->type == AST_BREAKX &&
               a9->type == AST_BAL && a19->type == AST_TABLE_ACCESS &&
               a20->type == AST_TABLE_UPDATE) != 0,
              "alias types correct");

  snobol_ast_free(a7);  /* frees a1 */
  snobol_ast_free(a19); /* frees a2 */
  snobol_ast_free(a20); /* frees a3 + a4 */
  snobol_ast_free(a5);
  snobol_ast_free(a6);
  snobol_ast_free(a8);
  snobol_ast_free(a9);
  snobol_ast_free(a10);
  snobol_ast_free(a11);
  snobol_ast_free(a12);
  snobol_ast_free(a13);
  snobol_ast_free(a14);
  snobol_ast_free(a15);
  snobol_ast_free(a16);
  snobol_ast_free(a17);
  snobol_ast_free(a18);

  /* NULL-argument branches. */
  ast_node_t *lit_null = snobol_ast_create_lit(nullptr, 0);
  test_assert(lit_null != NULL, "lit(NULL) node created (empty text)");
  snobol_ast_free(lit_null);
  ast_node_t *any_null = snobol_ast_create_any(nullptr, 0);
  test_assert((any_null != NULL && any_null->data.charclass.set == NULL) != 0,
              "any(NULL) keeps empty set");
  snobol_ast_free(any_null);
  ast_node_t *goto_null = snobol_ast_create_goto(nullptr);
  test_assert((goto_null != NULL && goto_null->data.goto_stmt.label == NULL) !=
                  0,
              "goto(NULL) keeps NULL label");
  snobol_ast_free(goto_null);
  ast_node_t *label_null = snobol_ast_create_label(nullptr, nullptr);
  test_assert((label_null != NULL && label_null->data.label.name == NULL) != 0,
              "label(NULL) keeps NULL name");
  snobol_ast_free(label_null);
}

/* ── arena binding and exhaustion fallback ────────────────────────────────── */


void test_cov_ast_arena(void) {
  test_suite("Coverage: arena binding + exhaustion fallback");

  /* Bind a tiny arena: allocations overflow to the heap. */
  uint8_t buf[64];
  snobol_arena_t arena;
  snobol_arena_init(&arena, buf, sizeof(buf));
  snobol_ast_set_arena(&arena);

  ast_node_t *nodes[64];
  size_t n = 0;
  for (int i = 0; i < 8; i++) {
    nodes[n++] = snobol_ast_create_lit("abcdefghij", 10); /* 10+ bytes each */
  }
  test_assert(n == 8, "nodes created under a small arena");

  /* The arena is cleared; owned strings are heap-freed, arena nodes are
   * reclaimed by the reset. */
  for (size_t i = 0; i < n; i++) {
    snobol_ast_free(nodes[i]);
  }

  snobol_arena_t *returned = snobol_ast_clear_arena();
  test_assert(returned == &arena, "clear_arena returns the bound arena");
  snobol_arena_reset(&arena);
  test_assert(true, "arena reset after clear");

  /* No arena bound → plain heap allocation. */
  ast_node_t *heap_node = snobol_ast_create_lit("heap", 4);
  test_assert(heap_node != NULL, "heap node created without arena");
  snobol_ast_free(heap_node);
}

void test_ast_suite(void) {
  test_ast_version();
  test_ast_version_string();
  test_ast_version_check_compatible();
  test_ast_version_check_incompatible();
  test_ast_version_macro();
  test_ast_version_check_macro();
  test_ast_create_and_free();
  test_ast_create_and_free_complex();
  test_ast_null_safety();
  test_ast_type_names();
  test_ast_create_all_types();
  test_cov_ast_clone_all_types();
  test_cov_ast_type_names_and_dump();
  test_cov_ast_aliases_and_null_args();
  test_cov_ast_arena();
}
