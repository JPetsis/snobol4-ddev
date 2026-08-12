/**
 * test_builder_compile.c – Tests for snobol_pattern_build_compile()
 *
 * Verify the C Builder API's one-call AST→pattern compilation: literal
 * patterns, composed patterns with byte-for-byte behavior parity against
 * source compilation (including captures), failure semantics (NULL +
 * malloc'd error string), and AST ownership transfer.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Search or match both patterns over the same subject and compare success,
 * position, match length, and capture register "1". */
static void compare_with_source(snobol_pattern_t *src, snobol_pattern_t *bld,
                                const char *subject, size_t slen, bool anchored,
                                const char *label) {
  snobol_match_t *ms = anchored ? snobol_pattern_match(src, subject, slen)
                                : snobol_pattern_search(src, subject, slen);
  snobol_match_t *mb = anchored ? snobol_pattern_match(bld, subject, slen)
                                : snobol_pattern_search(bld, subject, slen);
  bool sa = ms != NULL && snobol_match_success(ms);
  bool sb = mb != NULL && snobol_match_success(mb);
  char msg[160];
  snprintf(msg, sizeof(msg), "%s: success parity", label);
  test_assert(sa == sb, msg);
  if (sa && sb) {
    snprintf(msg, sizeof(msg), "%s: position parity", label);
    test_assert(snobol_match_get_position(ms) == snobol_match_get_position(mb),
                msg);
    snprintf(msg, sizeof(msg), "%s: length parity", label);
    test_assert(snobol_match_get_length(ms) == snobol_match_get_length(mb),
                msg);
    size_t ls = 0, lb = 0;
    /* Both construction paths use register 0: the parser allocates the first
     * @name capture to register 0 and the builder mirrors it explicitly. */
    const char *cs = snobol_match_get_variable(ms, "0", &ls);
    const char *cb = snobol_match_get_variable(mb, "0", &lb);
    snprintf(msg, sizeof(msg), "%s: capture parity", label);
    test_assert((cs == NULL) == (cb == NULL) && ls == lb &&
                    (cs == NULL || memcmp(cs, cb, ls) == 0),
                msg);
  }
  snobol_match_free(ms);
  snobol_match_free(mb);
}

/* 2.2: a built literal pattern matches and frees like any other pattern. */
static void test_builder_literal(void) {
  test_suite("Builder compile: literal pattern");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");
  if (!ctx)
    return;

  char *err = NULL;
  snobol_pattern_build_t *b = snobol_pattern_build_create();
  ast_node_t *lit = snobol_pattern_build_lit(b, "hello", 5);
  ast_node_t *root = snobol_pattern_build_emit(b, lit);
  test_assert(root == lit, "emit returns the root");
  snobol_pattern_t *pat = snobol_pattern_build_compile(ctx, root, 0, &err);
  test_assert(pat != NULL && err == NULL, "literal compiles");
  free(err);
  err = NULL;
  snobol_pattern_build_destroy(b);
  if (pat) {
    test_assert(snobol_pattern_get_meta(pat) != NULL,
                "search metadata derived");
    snobol_match_t *m = snobol_pattern_search(pat, "say hello world", 15);
    test_assert(m != NULL && snobol_match_success(m), "literal matches");
    if (m) {
      test_assert(snobol_match_get_position(m) == 4,
                  "literal found at offset 4");
      snobol_match_free(m);
    }
    m = snobol_pattern_search(pat, "nope", 4);
    test_assert(m != NULL && !snobol_match_success(m), "literal miss");
    if (m)
      snobol_match_free(m);
    snobol_pattern_free(pat);
  }

  /* The case-insensitive flag flows through the compile call. */
  b = snobol_pattern_build_create();
  lit = snobol_pattern_build_lit(b, "hello", 5);
  root = snobol_pattern_build_emit(b, lit);
  pat = snobol_pattern_build_compile(ctx, root, SNOBOL_FLAG_CASE_INSENSITIVE,
                                     &err);
  test_assert(pat != NULL && err == NULL, "case-insensitive literal compiles");
  free(err);
  err = NULL;
  snobol_pattern_build_destroy(b);
  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "HELLO there", 11);
    test_assert(m != NULL && snobol_match_success(m),
                "case-insensitive literal matches");
    if (m)
      snobol_match_free(m);
    snobol_pattern_free(pat);
  }

  snobol_context_destroy(ctx);
}

/* 2.3: concat/alt/cap/span/repeat/anchor behave identically to the same
 * pattern compiled from source, including capture values.  The builder tree
 * mirrors the parser's AST shape exactly ('+' = concat(clone, arbno),
 * '?' = repeat(sub, 0, 1), '^'/'$' = AST_ANCHOR) so the bytecode matches
 * too.  Both unanchored search and anchored match are compared. */
static void test_builder_composed(void) {
  test_suite("Builder compile: composed pattern vs source");

  /* ^ 'ab' @r1(SPAN('0-9')) ('x'|'y')+ 'q'? '!' $ */
  const char *src = "^ 'ab' @r1(SPAN('0-9')) ('x'|'y')+ 'q'? '!' $";

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");
  if (!ctx)
    return;

  char *err = NULL;
  snobol_pattern_t *from_source =
      snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(from_source != NULL && err == NULL, "source pattern compiles");
  free(err);
  err = NULL;

  snobol_pattern_build_t *b = snobol_pattern_build_create();
  ast_node_t **parts = (ast_node_t **)malloc(7 * sizeof(ast_node_t *));
  parts[0] = snobol_ast_create_anchor(ANCHOR_START);
  parts[1] = snobol_pattern_build_lit(b, "ab", 2);
  parts[2] =
      snobol_pattern_build_cap(b, 0, snobol_pattern_build_span(b, "0-9", 3));
  /* ('x'|'y')+ = concat(alt, arbno(alt)) — one alt node per child. */
  ast_node_t *alt_a =
      snobol_pattern_build_alt(b, snobol_pattern_build_lit(b, "x", 1),
                               snobol_pattern_build_lit(b, "y", 1));
  ast_node_t *alt_b =
      snobol_pattern_build_alt(b, snobol_pattern_build_lit(b, "x", 1),
                               snobol_pattern_build_lit(b, "y", 1));
  ast_node_t **plus_parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
  plus_parts[0] = alt_a;
  plus_parts[1] = snobol_ast_create_arbno(alt_b);
  parts[3] = snobol_ast_create_concat(plus_parts, 2);
  /* 'q'? = repeat(sub, 0, 1). */
  parts[4] =
      snobol_ast_create_repeat(snobol_pattern_build_lit(b, "q", 1), 0, 1);
  parts[5] = snobol_pattern_build_lit(b, "!", 1);
  parts[6] = snobol_ast_create_anchor(ANCHOR_END);
  ast_node_t *root =
      snobol_pattern_build_emit(b, snobol_pattern_build_concat(b, parts, 7));
  snobol_pattern_t *from_builder =
      snobol_pattern_build_compile(ctx, root, 0, &err);
  test_assert(from_builder != NULL && err == NULL, "builder pattern compiles");
  free(err);
  err = NULL;
  snobol_pattern_build_destroy(b);

  if (from_source && from_builder) {
    compare_with_source(from_source, from_builder, "ab123xyyyq!", 11, false,
                        "full match");
    compare_with_source(from_source, from_builder, "ab12xyy!", 8, false,
                        "optional skipped");
    compare_with_source(from_source, from_builder, "ab123xyz!", 9, false,
                        "no match");
    compare_with_source(from_source, from_builder, "xxab12xyy!", 10, false,
                        "off-start candidates");
    /* Anchored matching enforces the ^ anchor: both must miss off-start and
     * both must match at position 0 with identical captures. */
    compare_with_source(from_source, from_builder, "xxab12xyy!", 10, true,
                        "anchored: anchor blocks off-start");
    compare_with_source(from_source, from_builder, "ab123xyyyq!", 11, true,
                        "anchored: full match");
  }

  snobol_pattern_free(from_source);
  snobol_pattern_free(from_builder);
  snobol_context_destroy(ctx);
}

/* 2.4: compile failure returns NULL with a malloc'd error string. */
static void test_builder_compile_failure(void) {
  test_suite("Builder compile: failure returns NULL + error");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");
  if (!ctx)
    return;

  char *err = NULL;

  /* Undefined label reference fails the label-table validation. */
  snobol_pattern_build_t *b = snobol_pattern_build_create();
  ast_node_t *g = snobol_pattern_build_goto(b, "no_such_label");
  ast_node_t *root = snobol_pattern_build_emit(b, g);
  snobol_pattern_t *pat = snobol_pattern_build_compile(ctx, root, 0, &err);
  test_assert(pat == NULL, "undefined label returns NULL");
  test_assert(err != NULL && strlen(err) > 0, "error string provided");
  free(err);
  err = NULL;
  snobol_pattern_build_destroy(b);

  /* A NULL root fails cleanly (no dereference, no leak). */
  pat = snobol_pattern_build_compile(ctx, NULL, 0, &err);
  test_assert(pat == NULL, "NULL root returns NULL");
  test_assert(err != NULL && strlen(err) > 0, "NULL root error string");
  free(err);

  snobol_context_destroy(ctx);
}

/* 2.5: compile consumes the AST tree.  The caller must not free the tree
 * afterwards — double-frees and leaks are caught by the ASan/Valgrind run
 * (task 3.3).  The builder stays reusable after a compile. */
static void test_builder_ast_consumed(void) {
  test_suite("Builder compile: AST ownership transfer");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");
  if (!ctx)
    return;

  char *err = NULL;
  snobol_pattern_build_t *b = snobol_pattern_build_create();

  ast_node_t *lit = snobol_pattern_build_lit(b, "ab", 2);
  ast_node_t *root = snobol_pattern_build_emit(b, lit);
  snobol_pattern_t *pat = snobol_pattern_build_compile(ctx, root, 0, &err);
  test_assert(pat != NULL && err == NULL, "first tree compiles");
  free(err);
  err = NULL;

  /* Builder reuse after compile: build and compile a second tree. */
  ast_node_t *span = snobol_pattern_build_span(b, "0-9", 3);
  root = snobol_pattern_build_emit(b, span);
  snobol_pattern_t *pat2 = snobol_pattern_build_compile(ctx, root, 0, &err);
  test_assert(pat2 != NULL && err == NULL, "second tree compiles");
  free(err);
  err = NULL;

  /* A NULL error out-param is supported (set_error no-op path); the tree is
   * still consumed on the failure path. */
  ast_node_t *bad = snobol_pattern_build_goto(b, "no_such_label");
  root = snobol_pattern_build_emit(b, bad);
  snobol_pattern_t *pat3 = snobol_pattern_build_compile(ctx, root, 0, NULL);
  test_assert(pat3 == NULL, "NULL error param on failure is safe");

  /* NULL error out-param on the success path too. */
  ast_node_t *suc = snobol_pattern_build_succeed(b);
  root = snobol_pattern_build_emit(b, suc);
  snobol_pattern_t *pat4 = snobol_pattern_build_compile(ctx, root, 0, NULL);
  test_assert(pat4 != NULL, "NULL error param on success is safe");
  snobol_pattern_free(pat4);
  snobol_pattern_build_destroy(b);

  if (pat) {
    snobol_match_t *m = snobol_pattern_search(pat, "xab", 3);
    test_assert(m != NULL && snobol_match_success(m),
                "first pattern still matches");
    if (m)
      snobol_match_free(m);
    snobol_pattern_free(pat);
  }
  if (pat2) {
    snobol_match_t *m = snobol_pattern_search(pat2, "z42", 3);
    test_assert(m != NULL && snobol_match_success(m), "second pattern matches");
    if (m)
      snobol_match_free(m);
    snobol_pattern_free(pat2);
  }

  snobol_context_destroy(ctx);
}

void test_builder_compile_suite(void) {
  test_suite("Builder compile: snobol_pattern_build_compile()");
  test_builder_literal();
  test_builder_composed();
  test_builder_compile_failure();
  test_builder_ast_consumed();
}
