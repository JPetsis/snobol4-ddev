/*
 * test_source_parity.c - Source-vs-Builder bytecode parity harness
 *
 * Compiles every documented source form and its Builder twin and asserts
 * byte-for-byte snobol_pattern_get_bc equality, identical tier election,
 * and matched behavior pairs (success + length, plus capture variables)
 * on a shared corpus.  This enforces the "source == Builder" invariant of
 * the source-parser change: any drift between the two surfaces fails here.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"
#include "snobol/search.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Ambient builder handle used by the case constructors. */
static snobol_pattern_build_t *g_build;

typedef ast_node_t *(*case_build_fn)(void);

typedef struct {
  const char *name;
  const char *source;
  case_build_fn build; /* Builder twin (root AST node) */
  const char *subject;
  size_t subj_len;
  bool expect_ok;
  size_t expect_len;
  const char *var_name;     /* Optional variable check (NULL to skip) */
  const char *var_value;    /* Expected variable content */
  size_t var_value_len;
  bool expect_general_tier; /* EMIT/TABLE forms stay on TIER_GENERAL */
} parity_case_t;

/* ---- Builder-twin constructors ---- */

static ast_node_t *build_arb_readme(void) {
  ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "abc", 3);
  parts[1] = snobol_pattern_build_arbno(g_build, snobol_pattern_build_len(g_build, 1));
  parts[2] = snobol_pattern_build_lit(g_build, "def", 3);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 3));
}

static ast_node_t *build_arbno_a(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_arbno(g_build, snobol_pattern_build_lit(g_build, "a", 1)));
}

static ast_node_t *build_star_a(void) {
  return build_arbno_a();
}

static ast_node_t *build_plus_a(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "a", 1);
  parts[1] = snobol_pattern_build_arbno(g_build, snobol_pattern_build_lit(g_build, "a", 1));
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_opt_a(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 0, 1));
}

static ast_node_t *build_bal(void) {
  return snobol_pattern_build_emit(g_build, snobol_pattern_build_bal(g_build, '(', ')'));
}

static ast_node_t *build_fence(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "a", 1);
  parts[1] = snobol_pattern_build_fence(g_build);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_rem_after_ab(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "ab", 2);
  parts[1] = snobol_pattern_build_rem(g_build);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_rpos0(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "abc", 3);
  parts[1] = snobol_pattern_build_rpos(g_build, 0);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_rtab2_rem(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_rtab(g_build, 2);
  parts[1] = snobol_pattern_build_rem(g_build);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_len2(void) {
  return snobol_pattern_build_emit(g_build, snobol_pattern_build_len(g_build, 2));
}

static ast_node_t *build_pos2(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "ab", 2);
  parts[1] = snobol_pattern_build_pos(g_build, 2);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_tab2_c(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_tab(g_build, 2);
  parts[1] = snobol_pattern_build_lit(g_build, "c", 1);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_span_lower(void) {
  return snobol_pattern_build_emit(g_build, snobol_pattern_build_span(g_build, "a-z", 3));
}

static ast_node_t *build_break_space(void) {
  return snobol_pattern_build_emit(g_build, snobol_pattern_build_brk(g_build, " ", 1));
}

static ast_node_t *build_breakx_comma(void) {
  return snobol_pattern_build_emit(g_build, snobol_pattern_build_breakx(g_build, ",", 1));
}

static ast_node_t *build_emit_x(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "h", 1);
  parts[1] = snobol_ast_create_emit("X", 1, -1);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_emit_capture(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_cap(g_build, 0, snobol_pattern_build_lit(g_build, "ab", 2));
  parts[1] = snobol_ast_create_emit(NULL, 0, 0);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_dot_name(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_name(g_build, 0, snobol_pattern_build_lit(g_build, "a", 1)));
}

static ast_node_t *build_dollar_name(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "id:", 3);
  parts[1] = snobol_pattern_build_name(g_build, 1,
                                       snobol_pattern_build_span(g_build, "0-9", 3));
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_capture_prefix(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_cap(g_build, 0,
                                        snobol_pattern_build_lit(g_build, "a", 1)));
}

static ast_node_t *build_naming_then_lit(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_name(g_build, 0, snobol_pattern_build_lit(g_build, "a", 1));
  parts[1] = snobol_pattern_build_lit(g_build, "b", 1);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_table_access(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_ast_create_table_access("T", snobol_ast_create_lit("k", 1)));
}

static ast_node_t *build_table_update(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_ast_create_table_update("T", snobol_ast_create_lit("k", 1),
                                              snobol_ast_create_lit("v", 1)));
}

static ast_node_t *build_table_regref_key(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_cap(g_build, 0, snobol_pattern_build_lit(g_build, "z", 1));
  parts[1] = snobol_ast_create_table_access("T", snobol_ast_create_regref(0));
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_repeat_bounds(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 2, 3));
}

static ast_node_t *build_alt_ab(void) {
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_alt(g_build, snobol_pattern_build_lit(g_build, "a", 1),
                                        snobol_pattern_build_lit(g_build, "b", 1)));
}

static ast_node_t *build_anchor_start(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_anchor(ANCHOR_START);
  parts[1] = snobol_pattern_build_lit(g_build, "a", 1);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_anchor_end(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "a", 1);
  parts[1] = snobol_ast_create_anchor(ANCHOR_END);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_assign_name(void) {
  ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_cap(g_build, 0, snobol_pattern_build_lit(g_build, "ab", 2));
  parts[1] = snobol_pattern_build_assign(g_build, 1, 0);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 2));
}

static ast_node_t *build_arb_empty(void) {
  ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
  parts[0] = snobol_pattern_build_lit(g_build, "ab", 2);
  parts[1] = snobol_pattern_build_arbno(g_build, snobol_pattern_build_len(g_build, 1));
  parts[2] = snobol_pattern_build_lit(g_build, "cd", 2);
  return snobol_pattern_build_emit(
      g_build, snobol_pattern_build_concat(g_build, parts, 3));
}

/* ---- Corpus ---- */

static const parity_case_t g_cases[] = {
    {"ARB readme", "'abc' ARB 'def'", build_arb_readme, "abc def xyz", 11, true, 7,
     NULL, NULL, 0, false},
    {"ARBNO", "ARBNO('a')", build_arbno_a, "aaa", 3, true, 3, NULL, NULL, 0, false},
    {"postfix star", "'a'*", build_star_a, "bb", 2, true, 0, NULL, NULL, 0, false},
    {"postfix plus", "'a'+", build_plus_a, "aa", 2, true, 2, NULL, NULL, 0, false},
    {"postfix question", "'a'?", build_opt_a, "a", 1, true, 1, NULL, NULL, 0, false},
    {"BAL delimiters", "BAL('(', ')')", build_bal, "(a (b) c)", 9, true, 9, NULL, NULL,
     0, false},
    {"FENCE", "'a' FENCE", build_fence, "a", 1, true, 1, NULL, NULL, 0, false},
    {"REM", "'ab' REM", build_rem_after_ab, "abcdef", 6, true, 6, NULL, NULL, 0,
     false},
    {"RPOS", "'abc' RPOS(0)", build_rpos0, "abc", 3, true, 3, NULL, NULL, 0, false},
    {"RTAB+REM", "RTAB(2) REM", build_rtab2_rem, "abcdef", 6, true, 6, NULL, NULL, 0,
     false},
    {"LEN", "LEN(2)", build_len2, "abc", 3, true, 2, NULL, NULL, 0, false},
    {"POS", "'ab' POS(2)", build_pos2, "ab", 2, true, 2, NULL, NULL, 0, false},
    {"TAB", "TAB(2) 'c'", build_tab2_c, "abc", 3, true, 3, NULL, NULL, 0, false},
    {"SPAN", "SPAN('a-z')", build_span_lower, "abc", 3, true, 3, NULL, NULL, 0,
     false},
    {"BREAK", "BREAK(' ')", build_break_space, "abc xyz", 7, true, 3, NULL, NULL, 0,
     false},
    {"BREAKX", "BREAKX(',')", build_breakx_comma, "abc,def", 7, true, 3, NULL, NULL,
     0, false},
    {"EMIT literal", "'h' EMIT('X')", build_emit_x, "hi", 2, true, 1, NULL, NULL, 0,
     false},
    {"EMIT capture", "@name 'ab' EMIT(@name)", build_emit_capture, "ab", 2, true, 2,
     NULL, NULL, 0, true},
    {"dot naming", "'a' . @name", build_dot_name, "a", 1, true, 1, "v0", "a", 1,
     false},
    {"dollar naming", "'id:' SPAN('0-9') $v1", build_dollar_name, "id:42", 5, true,
     5, "v1", "42", 2, false},
    {"capture prefix", "@name 'a'", build_capture_prefix, "a", 1, true, 1, "v0", "a",
     1, false},
    {"naming tighter than concat", "'a' . @x 'b'", build_naming_then_lit, "ab", 2,
     true, 2, "v0", "a", 1, false},
    {"table access", "T['k']", build_table_access, "k", 1, false, 0, NULL, NULL, 0,
     true},
    {"table update", "T['k'] = 'v'", build_table_update, "kv", 2, false, 0, NULL,
     NULL, 0, true},
    {"table regref key", "@x 'z' T[$v0]", build_table_regref_key, "z", 1, false, 0,
     NULL, NULL, 0, true},
    {"repeat bounds", "repeat('a', 2, 3)", build_repeat_bounds, "aaa", 3, true, 3,
     NULL, NULL, 0, false},
    {"alternation", "'a'|'b'", build_alt_ab, "b", 1, true, 1, NULL, NULL, 0, false},
    {"anchor start", "^'a'", build_anchor_start, "a", 1, true, 1, NULL, NULL, 0,
     false},
    {"anchor end", "'a'$", build_anchor_end, "a", 1, true, 1, NULL, NULL, 0, false},
    {"charclass", "[a-z]", build_span_lower, "q", 1, true, 1, NULL, NULL, 0, false},
    {"assignment", "@x 'ab' v1 = 0", build_assign_name, "ab", 2, true, 2, "v1",
     "ab", 2, false},
    {"ARB empty span", "'ab' ARB 'cd'", build_arb_empty, "abcd", 4, true, 4, NULL,
     NULL, 0, false},
};

static void run_one_case(const parity_case_t *c) {
  char msg[192];
  char *err = nullptr;

  snobol_context_t *ctx = snobol_context_create();
  g_build = snobol_pattern_build_create();

  snobol_pattern_t *src =
      snobol_pattern_compile_ex(ctx, c->source, strlen(c->source), 0, &err);
  ast_node_t *root = c->build();
  snobol_pattern_t *built = snobol_pattern_build_compile(ctx, root, 0, &err);
  snobol_pattern_build_destroy(g_build);

  snprintf(msg, sizeof(msg), "%s: source compiles", c->name);
  test_assert(src != NULL, msg);
  snprintf(msg, sizeof(msg), "%s: builder twin compiles", c->name);
  test_assert(built != NULL, msg);

  if (src && built) {
    size_t al = snobol_pattern_get_bc_len(src);
    size_t bl = snobol_pattern_get_bc_len(built);
    snprintf(msg, sizeof(msg), "%s: bytecode length identical (%zu == %zu)",
             c->name, al, bl);
    test_assert(al == bl, msg);
    snprintf(msg, sizeof(msg), "%s: byte-for-byte identical bytecode", c->name);
    test_assert((al == bl &&
                 memcmp(snobol_pattern_get_bc(src), snobol_pattern_get_bc(built),
                        al) == 0) != 0,
                msg);

    const snobol_search_meta_t *msrc = snobol_pattern_get_meta(src);
    const snobol_search_meta_t *mbuilt = snobol_pattern_get_meta(built);
    snprintf(msg, sizeof(msg), "%s: identical tier election (%d == %d)",
             c->name, (int)msrc->tier, (int)mbuilt->tier);
    test_assert(msrc->tier == mbuilt->tier, msg);
    if (c->expect_general_tier) {
      snprintf(msg, sizeof(msg), "%s: stays on the general tier", c->name);
      test_assert(msrc->tier == TIER_GENERAL, msg);
    }

    /* Behavior pair on the shared corpus. */
    snobol_match_t *m1 = snobol_pattern_match(src, c->subject, c->subj_len);
    snobol_match_t *m2 = snobol_pattern_match(built, c->subject, c->subj_len);
    snprintf(msg, sizeof(msg), "%s: source match outcome", c->name);
    test_assert((m1 && m1->success) == c->expect_ok, msg);
    snprintf(msg, sizeof(msg), "%s: builder match outcome", c->name);
    test_assert((m2 && m2->success) == c->expect_ok, msg);
    if (m1 && m1->success) {
      snprintf(msg, sizeof(msg), "%s: source match length %zu", c->name,
               m1->length);
      test_assert(m1->length == c->expect_len, msg);
    }
    if (m2 && m2->success) {
      snprintf(msg, sizeof(msg), "%s: builder match length %zu", c->name,
               m2->length);
      test_assert(m2->length == c->expect_len, msg);
    }
    if (c->var_name) {
      size_t vlen1 = 0, vlen2 = 0;
      const char *v1 = m1 && m1->success
                           ? snobol_match_get_variable(m1, c->var_name, &vlen1)
                           : nullptr;
      const char *v2 = m2 && m2->success
                           ? snobol_match_get_variable(m2, c->var_name, &vlen2)
                           : nullptr;
      snprintf(msg, sizeof(msg), "%s: source binds %s", c->name, c->var_name);
      test_assert((v1 && vlen1 == c->var_value_len &&
                   memcmp(v1, c->var_value, vlen1) == 0) != 0,
                  msg);
      snprintf(msg, sizeof(msg), "%s: builder binds %s", c->name, c->var_name);
      test_assert((v2 && vlen2 == c->var_value_len &&
                   memcmp(v2, c->var_value, vlen2) == 0) != 0,
                  msg);
    }
    if (m1) {
      snobol_match_free(m1);
    }
    if (m2) {
      snobol_match_free(m2);
    }
  }

  snobol_pattern_free(src);
  snobol_pattern_free(built);
  free(err);
  snobol_context_destroy(ctx);
}

void test_source_parity_suite(void) {
  test_suite("Source-vs-Builder parity harness");

  for (size_t i = 0; i < sizeof(g_cases) / sizeof(g_cases[0]); i++) {
    run_one_case(&g_cases[i]);
  }
}