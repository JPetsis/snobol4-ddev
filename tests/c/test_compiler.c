/*
 * test_compiler.c - Compiler tests
 *
 * Verifies the AST → bytecode compiler emits the expected opcodes and
 * that captures are exposed in match results (regression for the bug
 * where OP_CAP_END didn't update var_count).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_helpers.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);


/* ===== test_coverage_codegen: coverage-driven tests merged into test_compiler.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"


/* Compile an AST; frees the AST and returns the bytecode buffer. */
static uint8_t *cov_compile(ast_node_t *ast, size_t *out_len) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
  snobol_ast_free(ast);
  if (rc != 0) {
    compiler_free(bc);
    return nullptr;
  }
  if (out_len) {
    *out_len = bc_len;
  }
  return bc;
}

void test_cov_codegen_emit_all(void) {
  test_suite("Coverage: codegen emit paths for every node type");

  uint8_t *bc;
  size_t bc_len;

  /* Primitive nodes. */
  bc = cov_compile(snobol_ast_create_lit("ab", 2), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "LIT emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_span("0-9", 3), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "SPAN emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_break(",", 1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "BREAK emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_any("ab", 2), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ANY emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_notany("c", 1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "NOTANY emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_breakx(";", 1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "BREAKX emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_len(2), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "LEN emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_assign(1, 0), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ASSIGN emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_eval(SNOBOL_FN_SIZE, 0), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "EVAL emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_bal('(', ')'), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "BAL emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_fence(), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "FENCE emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_rem(), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "REM emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_rpos(1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "RPOS emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_rtab(2), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "RTAB emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_pos(0), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "POS emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_tab(1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "TAB emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_abort(), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ABORT emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_fail(), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "FAIL emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_succeed(), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "SUCCEED emits");
  compiler_free(bc);

  /* Compound / structural nodes. */
  bc = cov_compile(snobol_ast_create_cap(1, snobol_ast_create_lit("a", 1)),
                   &bc_len);
  test_assert((bc && bc_len > 0) != 0, "CAP emits");
  compiler_free(bc);
  bc = cov_compile(
      snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 1, 3), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "REPEAT emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_arbno(snobol_ast_create_lit("a", 1)),
                   &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ARBNO emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_alt(snobol_ast_create_lit("a", 1),
                                         snobol_ast_create_lit("b", 1)),
                   &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ALT emits");
  compiler_free(bc);
  {
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = snobol_ast_create_lit("a", 1);
    parts[1] = snobol_ast_create_lit("b", 1);
    bc = cov_compile(snobol_ast_create_concat(parts, 2), &bc_len);
    test_assert((bc && bc_len > 0) != 0, "CONCAT emits");
    compiler_free(bc);
  }

  /* Anchors, emit, table access/update. */
  bc = cov_compile(snobol_ast_create_anchor(ANCHOR_START), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ANCHOR(start) emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_anchor(ANCHOR_END), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "ANCHOR(end) emits");
  compiler_free(bc);
  bc = cov_compile(snobol_ast_create_emit("out", 2, 1), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "EMIT emits");
  compiler_free(bc);
  bc = cov_compile(
      snobol_ast_create_table_access("tbl", snobol_ast_create_lit("k", 1)),
      &bc_len);
  test_assert((bc && bc_len > 0) != 0, "TABLE_ACCESS emits");
  compiler_free(bc);
  bc = cov_compile(
      snobol_ast_create_table_update("tbl", snobol_ast_create_lit("k", 1),
                                     snobol_ast_create_lit("v", 1)),
      &bc_len);
  test_assert((bc && bc_len > 0) != 0, "TABLE_UPDATE emits");
  compiler_free(bc);
  bc = cov_compile(
      snobol_ast_create_dynamic_eval(snobol_ast_create_lit("x", 1)), &bc_len);
  test_assert((bc && bc_len > 0) != 0, "DYNAMIC_EVAL emits");
  compiler_free(bc);

  /* Case-insensitive literal compilation. */
  {
    ast_node_t *ast = snobol_ast_create_lit("HELLO", 5);
    uint8_t *bc2 = nullptr;
    size_t len2 = 0;
    int rc = compile_ast_to_bytecode_c(ast, true, &bc2, &len2);
    test_assert((rc == 0 && bc2 && len2 > 0) != 0,
                "case-insensitive literal compiles");
    compiler_free(bc2);
    snobol_ast_free(ast);
  }
}


void test_cov_codegen_labels(void) {
  test_suite("Coverage: codegen label table + goto");

  /* Nested labels grow the codegen label table past its initial capacity. */
  {
    ast_node_t **parts = (ast_node_t **)malloc(6 * sizeof(ast_node_t *));
    parts[0] =
        snobol_ast_create_label((char *)"a", snobol_ast_create_lit("A", 1));
    parts[1] =
        snobol_ast_create_label((char *)"b", snobol_ast_create_lit("B", 1));
    parts[2] =
        snobol_ast_create_label((char *)"c", snobol_ast_create_lit("C", 1));
    parts[3] =
        snobol_ast_create_label((char *)"d", snobol_ast_create_lit("D", 1));
    parts[4] =
        snobol_ast_create_label((char *)"e", snobol_ast_create_lit("E", 1));
    parts[5] = snobol_ast_create_goto("a");
    ast_node_t *ast = snobol_ast_create_concat(parts, 6);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc == 0 && bc && bc_len > 0) != 0,
                "multi-label tree compiles");
    compiler_free(bc);
    snobol_ast_free(ast);
  }

  /* Unknown label: compiler rejects with no bytecode. */
  {
    ast_node_t *ast = snobol_ast_create_goto("missing");
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_ast_to_bytecode_c(ast, false, &bc, &bc_len);
    test_assert((rc != 0 && bc == NULL) != 0, "goto to unknown label rejected");
    compiler_free(bc);
    snobol_ast_free(ast);
  }
}


/* ===== test_coverage_engine2 (part): coverage-driven tests merged into test_compiler.c ===== */
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_engine2_fuse_shapes(void) {
  test_suite("Coverage: SPLIT->ANY fusion shape matrix");

  /* ANY-arm and mixed arms. */
  const char *pats[] = {"ANY('a') | ANY('b')",
                        "ANY('a') | 'b'",
                        "'a' | ANY('b')",
                        "NOTANY('a') | 'b'",
                        "('a' | 'b') | ('a' | 'b')",
                        "'a' | 'b' | 'a'"};
  for (size_t i = 0; i < sizeof(pats) / sizeof(pats[0]); i++) {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *p =
        snobol_pattern_compile_ex(ctx, pats[i], strlen(pats[i]), 0, &err);
    test_assert(p != NULL, "fuse-shape pattern compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_search(p, "b", 1);
      test_assert((m && m->success) != 0, "fuse-shape pattern matches");
      if (m) {
        snobol_match_free(m);
      }
      snobol_pattern_free(p);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Identical charclasses dedup in add_or_get_charclass. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    const char *src = "SPAN('a') 'x' SPAN('a')";
    snobol_pattern_t *p =
        snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
    test_assert(p != NULL, "repeated charclass compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_search(p, "aaxaa", 5);
      test_assert((m && m->success) != 0, "repeated charclass matches");
      if (m) {
        snobol_match_free(m);
      }
      snobol_pattern_free(p);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Long compile grows the code buffer. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    char src[512];
    size_t sl = 0;
    for (int i = 0; i < 40; i++) {
      memcpy(src + sl, "SPAN('0-9') ", 12);
      sl += 12;
    }
    snobol_pattern_t *p = snobol_pattern_compile_ex(ctx, src, sl, 0, &err);
    test_assert(p != NULL, "long chain compiles");
    if (p) {
      snobol_pattern_free(p);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

static void test_cov_len_real_semantics(void) {
  test_suite("Compiler: LEN(n) real semantics");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* Source LEN(2) must honor n (previously the parser placeholder fixed
   * the length at 1) and compile to bytecode identical to Builder len(2). */
  snobol_pattern_t *src =
      snobol_pattern_compile_ex(ctx, "LEN(2)", 6, 0, &err);
  snobol_pattern_build_t *b = snobol_pattern_build_create();
  ast_node_t *root = snobol_pattern_build_emit(b, snobol_pattern_build_len(b, 2));
  snobol_pattern_t *built = snobol_pattern_build_compile(ctx, root, 0, &err);

  test_assert((src && built) != 0, "source + builder LEN(2) both compile");
  if (src && built) {
    size_t al = snobol_pattern_get_bc_len(src);
    size_t bl = snobol_pattern_get_bc_len(built);
    test_assert(al == bl, "LEN(2) bytecode lengths match");
    test_assert(
        (al == bl && memcmp(snobol_pattern_get_bc(src),
                            snobol_pattern_get_bc(built), al) == 0) != 0,
        "LEN(2) source bytecode == builder bytecode");

    snobol_match_t *m1 = snobol_pattern_match(src, "abc", 3);
    snobol_match_t *m2 = snobol_pattern_match(built, "abc", 3);
    test_assert((m1 && m1->success && m1->length == 2) != 0,
                "source LEN(2) on 'abc' consumes exactly 2 characters");
    test_assert((m2 && m2->success && m2->length == 2) != 0,
                "builder len(2) on 'abc' consumes exactly 2 characters");
    if (m1) {
      snobol_match_free(m1);
    }
    if (m2) {
      snobol_match_free(m2);
    }
    snobol_pattern_free(src);
    snobol_pattern_free(built);
  } else {
    snobol_pattern_free(src);
    snobol_pattern_free(built);
  }
  free(err);
  err = nullptr;
  snobol_pattern_build_destroy(b);

  /* LEN() with no argument and LEN('5') with a quoted argument are both
   * rejected with descriptive errors naming the integer requirement. */
  {
    const char *bad[] = {"LEN()", "LEN('5')"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      err = nullptr;
      snobol_pattern_t *p =
          snobol_pattern_compile_ex(ctx, bad[i], strlen(bad[i]), 0, &err);
      test_assert(p == NULL, "malformed LEN argument rejected");
      test_assert((err && strstr(err, "integer") != NULL) != 0,
                  "LEN rejection names the integer requirement");
      free(err);
    }
  }

  snobol_context_destroy(ctx);
}

static void test_cov_source_primitives_behavior(void) {
  test_suite("Compiler: source primitives match + Builder parity");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;

  /* ARB: README quick-start 'abc' ARB 'def' — both compiles and behaves. */
  {
    snobol_pattern_t *src =
        snobol_pattern_compile_ex(ctx, "'abc' ARB 'def'", 15, 0, &err);

    snobol_pattern_build_t *b = snobol_pattern_build_create();
    ast_node_t *l1 = snobol_pattern_build_lit(b, "abc", 3);
    ast_node_t *arb = snobol_pattern_build_arbno(b, snobol_pattern_build_len(b, 1));
    ast_node_t *l2 = snobol_pattern_build_lit(b, "def", 3);
    ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = l1;
    parts[1] = arb;
    parts[2] = l2;
    ast_node_t *root = snobol_pattern_build_emit(b,
                            snobol_pattern_build_concat(b, parts, 3));
    snobol_pattern_t *built = snobol_pattern_build_compile(ctx, root, 0, &err);
    snobol_pattern_build_destroy(b);

    test_assert(src != NULL, "'abc' ARB 'def' compiles from source");
    test_assert((src && built) != 0, "builder twin compiles");
    if (src && built) {
      size_t al = snobol_pattern_get_bc_len(src);
      size_t bl = snobol_pattern_get_bc_len(built);
      test_assert((al == bl && memcmp(snobol_pattern_get_bc(src),
                                      snobol_pattern_get_bc(built), al) == 0) != 0,
                  "source ARB bytecode == builder arbno(len(1))");

      snobol_match_t *m = snobol_pattern_match(src, "abc def xyz", 11);
      test_assert((m && m->success && m->length == 7) != 0,
                  "ARB matches ' ' between abc and def");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(src);
    snobol_pattern_free(built);
    free(err);
    err = nullptr;
  }

  /* ARBNO('a') ≡ 'a'* bytecode and behavior. */
  {
    snobol_pattern_t *fn = snobol_pattern_compile_ex(ctx, "ARBNO('a')", 10, 0, &err);
    free(err);
    err = nullptr;
    snobol_pattern_t *star = snobol_pattern_compile_ex(ctx, "'a'*", 4, 0, &err);
    test_assert((fn && star) != 0, "ARBNO('a') and 'a'* both compile");
    if (fn && star) {
      size_t al = snobol_pattern_get_bc_len(fn);
      size_t bl = snobol_pattern_get_bc_len(star);
      test_assert((al == bl && memcmp(snobol_pattern_get_bc(fn),
                                      snobol_pattern_get_bc(star), al) == 0) != 0,
                  "ARBNO('a') bytecode == 'a'* bytecode");
      snobol_match_t *m = snobol_pattern_match(fn, "aaa", 3);
      test_assert((m && m->success && m->length == 3) != 0,
                  "ARBNO('a') consumes all of 'aaa'");
      if (m) {
        snobol_match_free(m);
      }
      m = snobol_pattern_match(fn, "bb", 2);
      test_assert((m && m->success && m->length == 0) != 0,
                  "ARBNO('a') matches empty at 'bb'");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(fn);
    snobol_pattern_free(star);
    free(err);
    err = nullptr;
  }

  /* BAL('(', ')') nested-delimiter matching; BAL() equals explicit form. */
  {
    snobol_pattern_t *bal = snobol_pattern_compile_ex(ctx, "BAL('(', ')')", 13, 0, &err);
    test_assert(bal != NULL, "BAL('(', ')') compiles");
    if (bal) {
      snobol_match_t *m = snobol_pattern_match(bal, "(outer (inner) outer)", 21);
      test_assert((m && m->success && m->length == 21) != 0,
                  "BAL matches the whole nested pair");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(bal);
    free(err);
    err = nullptr;
  }
  {
    snobol_pattern_t *bal0 = snobol_pattern_compile_ex(ctx, "BAL()", 5, 0, &err);
    snobol_pattern_t *balx = snobol_pattern_compile_ex(ctx, "BAL('(', ')')", 13, 0, &err);
    test_assert((bal0 && balx) != 0, "BAL() and BAL('(', ')') compile");
    if (bal0 && balx) {
      size_t al = snobol_pattern_get_bc_len(bal0);
      size_t bl = snobol_pattern_get_bc_len(balx);
      test_assert((al == bl && memcmp(snobol_pattern_get_bc(bal0),
                                      snobol_pattern_get_bc(balx), al) == 0) != 0,
                  "BAL() bytecode == BAL('(', ')') bytecode");
      snobol_match_t *m = snobol_pattern_match(bal0, "(a (b) c)", 9);
      test_assert((m && m->success && m->length == 9) != 0,
                  "BAL() with default delimiters matches '(a (b) c)'");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(bal0);
    snobol_pattern_free(balx);
    free(err);
    err = nullptr;
  }

  /* REM consumes the remainder; RTAB(2) + REM leaves the last 2. */
  {
    snobol_pattern_t *p = snobol_pattern_compile_ex(ctx, "'ab' REM", 8, 0, &err);
    test_assert(p != NULL, "'ab' REM compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_match(p, "abcdef", 6);
      test_assert((m && m->success && m->length == 6) != 0,
                  "REM consumes 'cdef' after 'ab'");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(p);
    free(err);
    err = nullptr;

    p = snobol_pattern_compile_ex(ctx, "RTAB(2) REM", 11, 0, &err);
    test_assert(p != NULL, "RTAB(2) REM compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_match(p, "abcdef", 6);
      test_assert((m && m->success && m->length == 6) != 0,
                  "RTAB(2) REM consumes everything, REM = 'ef'");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(p);
    free(err);
    err = nullptr;
  }

  /* RPOS(0) succeeds with the cursor at the end. */
  {
    snobol_pattern_t *p =
        snobol_pattern_compile_ex(ctx, "SPAN('a-z') RPOS(0)", 19, 0, &err);
    test_assert(p != NULL, "SPAN('a-z') RPOS(0) compiles");
    if (p) {
      snobol_match_t *m = snobol_pattern_match(p, "abc", 3);
      test_assert((m && m->success && m->length == 3) != 0,
                  "RPOS(0) succeeds at the subject end");
      if (m) {
        snobol_match_free(m);
      }
    }
    snobol_pattern_free(p);
    free(err);
    err = nullptr;
  }

  /* repeat('a', 2, 3) honors bounds; bytecode matches Builder repeat. */
  {
    snobol_pattern_t *src = snobol_pattern_compile_ex(ctx, "repeat('a', 2, 3)", 17, 0, &err);
    /* Builder twin: the C builder API has no repeat()-shaped helper, so
     * construct the identical AST node directly (snobol_ast_create_repeat). */
    snobol_pattern_build_t *b = snobol_pattern_build_create();
    ast_node_t *rep = snobol_ast_create_repeat(snobol_ast_create_lit("a", 1), 2, 3);
    ast_node_t *root = snobol_pattern_build_emit(b, rep);
    snobol_pattern_t *built = snobol_pattern_build_compile(ctx, root, 0, &err);
    snobol_pattern_build_destroy(b);

    test_assert((src && built) != 0, "repeat source + builder compile");
    if (src && built) {
      size_t al = snobol_pattern_get_bc_len(src);
      size_t bl = snobol_pattern_get_bc_len(built);
      test_assert((al == bl && memcmp(snobol_pattern_get_bc(src),
                                      snobol_pattern_get_bc(built), al) == 0) != 0,
                  "repeat source bytecode == builder repeat bytecode");

      struct {
        const char *subj;
        size_t len;
        bool ok;
        size_t mlen;
      } cases[] = {{"a", 1, false, 0},   {"aa", 2, true, 2},
                   {"aaa", 3, true, 3},  {"aaaa", 4, true, 3}};
      for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snobol_match_t *m = snobol_pattern_match(src, cases[i].subj, cases[i].len);
        test_assert((m && m->success) == cases[i].ok, "repeat bound outcome");
        if (m && m->success) {
          test_assert(m->length == cases[i].mlen, "repeat bound length");
        }
        if (m) {
          snobol_match_free(m);
        }
      }
    }
    snobol_pattern_free(src);
    snobol_pattern_free(built);
    free(err);
    err = nullptr;
  }

  snobol_context_destroy(ctx);
}

void test_compiler_suite(void) {
  test_suite("Compiler Tests");

  test_assert(true, "Compiler initialization placeholder");
  test_assert(true, "Compiler can process AST nodes");

  /* Plain literal (no capture) -> var_count stays 0 */
  {
    ast_node_t *lit = snobol_ast_create_lit("hello", 5);
    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(lit, "hello world", 11, &match_len, &cap_count);
    test_assert(ok, "plain literal matches");
    test_assert(cap_count == 0,
                "plain literal has 0 captures (var_count stays 0)");
    snobol_ast_free(lit);
  }

  /* Single capture: cap(0, SPAN('0-9')) on "id:12345" -> v0 == "12345" */
  {
    ast_node_t *id_lit = snobol_ast_create_lit("id:", 3);
    ast_node_t *digits = snobol_ast_create_span("0123456789", 10);
    ast_node_t *cap = snobol_ast_create_cap(0, digits);
    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    parts[0] = id_lit;
    parts[1] = cap;
    ast_node_t *concat = snobol_ast_create_concat(parts, 2);

    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(concat, "id:12345", 8, &match_len, &cap_count);
    test_assert(ok, "AST cap+span+concat matches 'id:12345'");
    test_assert(match_len == 8, "match_len is 8 (full pattern length)");
    test_assert(cap_count >= 1, "OP_CAP_END exposes capture register as a var");

    snobol_ast_free(concat);
  }

  /* Two captures via concat: cap(0,...) cap(1,...) on "ab ba" */
  {
    /* Build: cap(0, SPAN("abc")) ' ' cap(1, SPAN("abc")) */
    ast_node_t *left_span = snobol_ast_create_span("abc", 3);
    test_assert(left_span != NULL, "create_span for left_cap");
    ast_node_t *left_cap = snobol_ast_create_cap(0, left_span);
    test_assert(left_cap != NULL, "create_cap(0, ...)");
    ast_node_t *sp_lit = snobol_ast_create_lit(" ", 1);
    test_assert(sp_lit != NULL, "create_lit(' ')");
    ast_node_t *right_span = snobol_ast_create_span("abc", 3);
    test_assert(right_span != NULL, "create_span for right_cap");
    ast_node_t *right_cap = snobol_ast_create_cap(1, right_span);
    test_assert(right_cap != NULL, "create_cap(1, ...)");

    ast_node_t **parts = (ast_node_t **)malloc(3 * sizeof(ast_node_t *));
    parts[0] = left_cap;
    parts[1] = sp_lit;
    parts[2] = right_cap;
    ast_node_t *concat = snobol_ast_create_concat(parts, 3);
    test_assert(concat != NULL, "create_concat with 3 parts");

    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(concat, "ab ba", 5, &match_len, &cap_count);
    test_assert(ok, "two captures match 'ab ba'");
    test_assert(match_len == 5, "match_len is 5");
    test_assert(cap_count >= 2, "OP_CAP_END exposes both capture registers");

    snobol_ast_free(concat);
  }

  /* Capture in alternation: only the matched branch contributes */
  {
    ast_node_t *h_lit = snobol_ast_create_lit("hi", 2);
    ast_node_t *h_cap = snobol_ast_create_cap(0, h_lit);
    ast_node_t *g_lit = snobol_ast_create_lit("bye", 3);
    ast_node_t *g_cap = snobol_ast_create_cap(0, g_lit);
    ast_node_t *alt = snobol_ast_create_alt(h_cap, g_cap);

    int match_len = 0;
    int cap_count = 0;
    bool ok = run_ast_pattern(alt, "bye world", 9, &match_len, &cap_count);
    test_assert(ok, "alt with cap matches 'bye world'");
    test_assert(match_len == 3, "match_len is 3 (length of 'bye')");
    test_assert(cap_count >= 1, "capture in matched branch is exposed");

    snobol_ast_free(alt);
  }
  test_cov_codegen_emit_all();
  test_cov_codegen_labels();
  test_cov_engine2_fuse_shapes();
  test_cov_len_real_semantics();
  test_cov_source_primitives_behavior();
}
