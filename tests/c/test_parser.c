/**
 * @file test_parser.c
 * @brief Tests for the SNOBOL C parser
 */

#include "snobol/ast.h"
#include "snobol/lexer.h"
#include "snobol/parser.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test framework functions (from test_runner.c) */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_parser_create_destroy(void) {
  test_suite("Parser: create and destroy");

  snobol_parser_t *parser = snobol_parser_create();
  test_assert(parser != NULL, "parser_create returns non-NULL");

  snobol_parser_destroy(parser);
  test_assert(true, "parser_destroy completes without crash");
}

static void test_parser_literal(void) {
  test_suite("Parser: literal pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("'hello'", 7);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_LITERAL, "AST node is LITERAL");
  test_assert(ast->data.literal.len == 5, "literal length is 5");
  test_assert(memcmp(ast->data.literal.text, "hello", 5) == 0,
              "literal text matches");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_alternation(void) {
  test_suite("Parser: alternation pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("'A' | 'B'", 9);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_ALT, "AST node is ALT");
  test_assert(ast->data.alt.left != NULL, "left child exists");
  test_assert(ast->data.alt.right != NULL, "right child exists");
  test_assert(ast->data.alt.left->type == AST_LITERAL, "left is LITERAL");
  test_assert(ast->data.alt.right->type == AST_LITERAL, "right is LITERAL");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_concatenation(void) {
  test_suite("Parser: concatenation pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("'hello' ' ' 'world'", 19);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_CONCAT, "AST node is CONCAT");
  test_assert(ast->data.concat.count == 3, "concat has 3 parts");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_arbno(void) {
  test_suite("Parser: arbno pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("'x'*", 4);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_ARBNO, "AST node is ARBNO");
  test_assert(ast->data.arbno.sub != NULL, "arbno has sub-pattern");
  test_assert(ast->data.arbno.sub->type == AST_LITERAL, "sub is LITERAL");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_span(void) {
  test_suite("Parser: SPAN function");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("SPAN('0-9')", 11);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_SPAN, "AST node is SPAN");
  if (ast->type == AST_SPAN) {
    test_assert(ast->data.charclass.len == 3,
                "charclass length is 3 (content without quotes)");
    test_assert(memcmp(ast->data.charclass.set, "0-9", 3) == 0,
                "charclass text matches");
  }

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_any(void) {
  test_suite("Parser: ANY function");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("ANY('aeiou')", 12);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_ANY, "AST node is ANY");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_capture(void) {
  test_suite("Parser: capture pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("@var 'hello'", 13);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_CAP, "AST node is CAP");
  test_assert(ast->data.cap.reg == 0, "first named capture gets register 0");
  test_assert(ast->data.cap.sub != NULL, "capture has sub-pattern");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_parenthesized(void) {
  test_suite("Parser: parenthesized pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("('A' | 'B')", 11);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_ALT, "AST node is ALT (parentheses removed)");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_anchors(void) {
  test_suite("Parser: anchors");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("^'start'", 8);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_CONCAT, "AST node is CONCAT (anchor + literal)");
  test_assert(ast->data.concat.count == 2, "concat has 2 parts");
  test_assert(ast->data.concat.parts[0]->type == AST_ANCHOR,
              "first part is ANCHOR");
  test_assert(ast->data.concat.parts[0]->data.anchor.atype == ANCHOR_START,
              "anchor is START");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_nested(void) {
  test_suite("Parser: nested pattern");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("('A' | 'B')*", 12);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  test_assert(ast != NULL, "parser returns AST");
  test_assert((!snobol_parser_has_error(parser)) != 0, "no parse error");
  test_assert(ast->type == AST_ARBNO, "AST node is ARBNO");
  test_assert(ast->data.arbno.sub != NULL, "arbno has sub-pattern");
  test_assert(ast->data.arbno.sub->type == AST_ALT, "sub is ALT");

  snobol_ast_free(ast);
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_error_unclosed_literal(void) {
  test_suite("Parser: error - unclosed literal");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("'unclosed", 8);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  /* Parser may return partial AST or NULL for syntax errors */
  /* The important thing is it doesn't crash */
  if (ast) {
    snobol_ast_free(ast);
  }
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);

  test_assert(true, "parser handles unclosed literal without crash");
}

static void test_parser_error_empty(void) {
  test_suite("Parser: error - empty input");

  snobol_parser_t *parser = snobol_parser_create();
  snobol_lexer_t *lexer = snobol_lexer_create("", 0);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  /* Empty input should produce an error */
  test_assert((ast == NULL || snobol_parser_has_error(parser)) != 0,
              "empty input produces error or NULL");

  if (ast) {
    snobol_ast_free(ast);
  }
  snobol_lexer_destroy(lexer);
  snobol_parser_destroy(parser);
}

static void test_parser_memory_cleanup(void) {
  test_suite("Parser: memory cleanup");

  /* Create and parse multiple patterns to check for memory leaks */
  for (int i = 0; i < 10; i++) {
    snobol_parser_t *parser = snobol_parser_create();
    snobol_lexer_t *lexer = snobol_lexer_create("'test' | 'pattern'", 17);

    ast_node_t *ast = snobol_parser_parse(parser, lexer);
    if (ast) {
      snobol_ast_free(ast);
    }

    snobol_lexer_destroy(lexer);
    snobol_parser_destroy(parser);
  }

  test_assert(true, "10 parse/free cycles completed without crash");
}


/* ===== test_coverage_parser: coverage-driven tests merged into test_parser.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/parser.h"


/* Parse a source string; returns the AST (caller frees) or NULL. */
static ast_node_t *covp_parse(snobol_parser_t *parser, const char *src,
                              bool *has_error) {
  snobol_lexer_t *lexer = snobol_lexer_create(src, strlen(src));
  ast_node_t *ast = snobol_parser_parse(parser, lexer);
  if (has_error) {
    *has_error = snobol_parser_has_error(parser);
  }
  snobol_lexer_destroy(lexer);
  return ast;
}

void test_cov_parser_labels_and_gotos(void) {
  test_suite("Coverage: parser labels + gotos");

  /* Label: `foo: 'a'` parses to AST_LABEL wrapping a literal. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "foo: 'a'", &err);
    test_assert((ast && !err && ast->type == AST_LABEL) != 0, "label parses");
    if (ast) {
      test_assert((ast->data.label.name &&
                   strcmp(ast->data.label.name, "foo") == 0) != 0,
                  "label name captured");
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Nested labels grow the seen-label table past its initial capacity. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "a: b: c: d: e: 'x'", &err);
    test_assert((ast && !err) != 0, "five nested labels parse");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Duplicate label is rejected. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "dup: dup: 'a'", &err);
    test_assert((ast == NULL && err) != 0, "duplicate label rejected");
    snobol_parser_destroy(parser);
  }

  /* Goto syntax: `'a' : (L)` produces concat(pattern, goto). */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' : (L)", &err);
    test_assert((ast && !err && ast->type == AST_CONCAT) != 0, "goto parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Goto failure modes. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' : (", &err);
    test_assert((ast == NULL && err) != 0, "goto without paren body rejected");
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' : (1)", &err);
    test_assert((ast == NULL && err) != 0,
                "goto with non-ident label rejected");
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' : (L", &err);
    test_assert((ast == NULL && err) != 0,
                "goto without closing paren rejected");
    snobol_parser_destroy(parser);
  }

  /* Trailing garbage after the pattern. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' 'b'", &err);
    test_assert((ast && !err) != 0, "concatenation parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
}


void test_cov_parser_repetition_and_primary(void) {
  test_suite("Coverage: parser repetition + primary errors");

  /* '?' and '+' postfix operators. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a'?", &err);
    test_assert((ast && !err && ast->type == AST_REPETITION) != 0,
                "question-mark parses to repetition");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a'+", &err);
    test_assert((ast && !err && ast->type == AST_CONCAT) != 0,
                "plus parses to concat(clone, arbno)");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Alternation with a missing right side. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' |", &err);
    test_assert((ast == NULL && err) != 0, "dangling pipe rejected");
    snobol_parser_destroy(parser);
  }

  /* Unclosed paren. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "('a'", &err);
    test_assert((ast == NULL && err) != 0, "unclosed paren rejected");
    snobol_parser_destroy(parser);
  }

  /* Anchors as primaries. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "^'a'", &err);
    test_assert((ast && !err && ast->type == AST_CONCAT) != 0,
                "start anchor parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "$ 'a'", &err);
    test_assert((ast && !err) != 0, "end anchor parses as primary");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Bad capture targets. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "@5 'a'", &err);
    test_assert((ast == NULL && err) != 0, "non-ident capture target rejected");
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "@r1", &err);
    test_assert((ast == NULL && err) != 0,
                "capture without sub-pattern rejected");
    snobol_parser_destroy(parser);
  }

  /* Bare identifier and unknown token. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "bareident", &err);
    test_assert((ast == NULL && err) != 0, "bare identifier rejected");
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "5", &err);
    test_assert((ast == NULL && err) != 0, "numeric token rejected");
    snobol_parser_destroy(parser);
  }
}


void test_cov_parser_functions(void) {
  test_suite("Coverage: parser function-call validation");

  /* ANY() with no argument is accepted. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "ANY()", &err);
    test_assert((ast && !err && ast->type == AST_ANY) != 0, "ANY() parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Argument-type errors for each builtin. */
  {
    /* Bare digits are skipped by the lexer, so these must use identifiers. */
    const char *bad[] = {"SPAN(foo)", "BREAK(foo)",  "BREAKX(foo)",
                         "ANY(foo)",  "NOTANY(foo)", "POS(foo)",
                         "TAB(foo)",  "FOO('x')"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      snobol_parser_t *parser = snobol_parser_create();
      bool err = false;
      ast_node_t *ast = covp_parse(parser, bad[i], &err);
      test_assert((ast == NULL && err) != 0, "bad builtin argument rejected");
      snobol_parser_destroy(parser);
    }
  }

  /* Missing closing paren after the argument. */
  {
    const char *bad[] = {"SPAN('a'",   "BREAK('a'", "BREAKX('a'", "ANY('a'",
                         "NOTANY('a'", "LEN('5'",   "POS('2'",    "TAB('3'",
                         "ABORT(x)",   "ABORT(",    "FAIL(",      "SUCCEED("};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      snobol_parser_t *parser = snobol_parser_create();
      bool err = false;
      ast_node_t *ast = covp_parse(parser, bad[i], &err);
      test_assert((ast == NULL && err) != 0, "unclosed builtin call rejected");
      snobol_parser_destroy(parser);
    }
  }

  /* Valid zero-arg and integer-arg builtins. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "ABORT()", &err);
    test_assert((ast && !err && ast->type == AST_ABORT) != 0, "ABORT() parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "FAIL()", &err);
    test_assert((ast && !err && ast->type == AST_FAIL) != 0, "FAIL() parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "SUCCEED()", &err);
    test_assert((ast && !err && ast->type == AST_SUCCEED) != 0,
                "SUCCEED() parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "POS('2')", &err);
    test_assert((ast && !err && ast->type == AST_POS) != 0, "POS('2') parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "TAB('3')", &err);
    test_assert((ast && !err && ast->type == AST_TAB) != 0, "TAB('3') parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "LEN('5')", &err);
    test_assert((ast && !err && ast->type == AST_LEN) != 0, "LEN('5') parses");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* NULL guards. */
  test_assert(snobol_parser_parse(nullptr, nullptr) == NULL,
              "parse(NULL,NULL)");
  {
    snobol_parser_t *parser = snobol_parser_create();
    test_assert(snobol_parser_parse(parser, nullptr) == NULL,
                "parse(parser,NULL)");
    test_assert((!snobol_parser_has_error(nullptr)) != 0, "has_error(NULL)");
    test_assert(snobol_parser_get_error(nullptr) == NULL, "get_error(NULL)");
    snobol_parser_destroy(parser);
  }
}


void test_cov_parser_round3(void) {
  test_suite("Coverage: parser trailing tokens + misc errors");

  /* Trailing token after the pattern. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' =", &err);
    test_assert((ast == NULL && err) != 0, "trailing token rejected");
    snobol_parser_destroy(parser);
  }

  /* ':' followed by EOF (no '(') — goto expect failure. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "'a' :", &err);
    test_assert((ast == NULL && err) != 0, "dangling colon rejected");
    snobol_parser_destroy(parser);
  }

  /* '@*' capture target (star fallback branch). */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    /* '@*' has no valid capture target: the parser must reject it with a
     * parse error (no star fallback branch). */
    ast_node_t *ast = covp_parse(parser, "@* 'a'", &err);
    test_assert((ast == NULL && err) != 0, "star capture target rejected");
    snobol_parser_destroy(parser);
  }

  /* EVAL with an empty expression. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "EVAL()", &err);
    test_assert((ast == NULL && err) != 0, "empty EVAL expression rejected");
    snobol_parser_destroy(parser);
  }

  /* EVAL without closing paren. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "EVAL('x'", &err);
    test_assert((ast == NULL && err) != 0, "unclosed EVAL rejected");
    snobol_parser_destroy(parser);
  }

  /* Re-parse frees the previous parse's labels. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "x: 'a'", &err);
    test_assert((ast && !err) != 0, "first labelled parse");
    if (ast) {
      snobol_ast_free(ast);
    }
    ast = covp_parse(parser, "y: 'b'", &err);
    test_assert((ast && !err) != 0, "second labelled parse reuses parser");
    if (ast) {
      snobol_ast_free(ast);
    }
    snobol_parser_destroy(parser);
  }

  /* Error-location and clear-error accessors. */
  {
    snobol_parser_t *parser = snobol_parser_create();
    size_t line = 0;
    size_t col = 0;
    bool err = false;
    ast_node_t *ast = covp_parse(parser, "5", &err);
    test_assert((ast == NULL && err) != 0, "numeric token errors");
    snobol_parser_get_error_location(parser, &line, &col);
    test_assert(line >= 1, "error location line");
    snobol_parser_clear_error(parser);
    test_assert((!snobol_parser_has_error(parser)) != 0, "clear error resets");
    snobol_parser_get_error_location(parser, nullptr, nullptr);
    snobol_parser_get_error_location(nullptr, &line, &col);
    snobol_parser_clear_error(nullptr);
    snobol_parser_destroy(parser);
  }
}

void test_parser_suite(void) {
  test_parser_create_destroy();
  test_parser_literal();
  test_parser_alternation();
  test_parser_concatenation();
  test_parser_arbno();
  test_parser_span();
  test_parser_any();
  test_parser_capture();
  test_parser_parenthesized();
  test_parser_anchors();
  test_parser_nested();
  test_parser_error_unclosed_literal();
  test_parser_error_empty();
  test_parser_memory_cleanup();
  test_cov_parser_labels_and_gotos();
  test_cov_parser_repetition_and_primary();
  test_cov_parser_functions();
  test_cov_parser_round3();
}
