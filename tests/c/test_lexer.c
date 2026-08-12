/**
 * @file test_lexer.c
 * @brief Tests for the SNOBOL C lexer
 */

#include "snobol/lexer.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test framework functions (from test_runner.c) */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_lexer_create_destroy(void) {
  test_suite("Lexer: create and destroy");

  snobol_lexer_t *lexer = snobol_lexer_create("'hello'", 7);
  test_assert(lexer != NULL, "lexer_create returns non-NULL");

  snobol_lexer_destroy(lexer);
  test_assert(true, "lexer_destroy completes without crash");
}

static void test_lexer_create_null_source(void) {
  test_suite("Lexer: NULL source");

  snobol_lexer_t *lexer = snobol_lexer_create(nullptr, 0);
  test_assert(lexer == NULL, "lexer_create with NULL returns NULL");
}

static void test_lexer_literal(void) {
  test_suite("Lexer: literal token");

  snobol_lexer_t *lexer = snobol_lexer_create("'hello'", 7);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "first token is LITERAL");
  test_assert(tok.data.string.len == 5, "literal length is 5");
  test_assert(memcmp(tok.data.string.text, "hello", 5) == 0,
              "literal text matches");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "second token is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_alternation(void) {
  test_suite("Lexer: alternation operator");

  snobol_lexer_t *lexer = snobol_lexer_create("'A' | 'B'", 9);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "token 1 is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_PIPE, "token 2 is PIPE");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "token 3 is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "token 4 is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_whitespace(void) {
  test_suite("Lexer: whitespace handling");

  snobol_lexer_t *lexer = snobol_lexer_create("  'A'  |  'B'  ", 15);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "skips leading whitespace");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_PIPE, "skips whitespace before pipe");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "skips whitespace after pipe");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "skips trailing whitespace");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_charclass(void) {
  test_suite("Lexer: character class");

  snobol_lexer_t *lexer = snobol_lexer_create("[a-z]", 5);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_CHARCLASS, "token is CHARCLASS");
  test_assert(tok.data.string.len == 3, "charclass length is 3 (content only)");
  test_assert(memcmp(tok.data.string.text, "a-z", 3) == 0,
              "charclass text matches");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "next is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_anchors(void) {
  test_suite("Lexer: anchors");

  snobol_lexer_t *lexer = snobol_lexer_create("^'start' 'end'$", 15);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_ANCHOR_START, "first token is ANCHOR_START");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "second token is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "third token is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_ANCHOR_END, "fourth token is ANCHOR_END");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "fifth token is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_arbno(void) {
  test_suite("Lexer: arbno operator");

  snobol_lexer_t *lexer = snobol_lexer_create("'x'*", 4);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "first token is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_STAR, "second token is STAR");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_EOF, "third token is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_capture(void) {
  test_suite("Lexer: capture operator");

  snobol_lexer_t *lexer = snobol_lexer_create("@var 'hello'", 13);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_AT, "first token is AT");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_IDENT, "second token is IDENT");
  test_assert(tok.data.string.len == 3, "IDENT length is 3");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "third token is LITERAL");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_peek(void) {
  test_suite("Lexer: peek functionality");

  snobol_lexer_t *lexer = snobol_lexer_create("'A'", 3);
  test_assert(lexer != NULL, "lexer created");

  token_t tok1 = snobol_lexer_peek(lexer);
  test_assert(tok1.type == TOKEN_LIT, "peek returns LITERAL");

  token_t tok2 = snobol_lexer_peek(lexer);
  test_assert(tok2.type == TOKEN_LIT, "second peek returns same token");

  token_t tok3 = snobol_lexer_next(lexer);
  test_assert(tok3.type == TOKEN_LIT, "next returns the peeked token");

  token_t tok4 = snobol_lexer_next(lexer);
  test_assert(tok4.type == TOKEN_EOF, "next after is EOF");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_position_tracking(void) {
  test_suite("Lexer: position tracking");

  snobol_lexer_t *lexer = snobol_lexer_create("'A'\n'B'", 6);
  test_assert(lexer != NULL, "lexer created");

  test_assert(snobol_lexer_get_pos(lexer) == 0, "initial position is 0");
  test_assert(snobol_lexer_get_line(lexer) == 1, "initial line is 1");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "first token is LITERAL");

  tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "second token is LITERAL (after newline)");
  test_assert(snobol_lexer_get_line(lexer) == 2,
              "line incremented after newline");

  snobol_lexer_destroy(lexer);
}

static void test_lexer_single_quotes(void) {
  test_suite("Lexer: single quote literals");

  snobol_lexer_t *lexer = snobol_lexer_create("'it''s'", 7);
  test_assert(lexer != NULL, "lexer created");

  token_t tok = snobol_lexer_next(lexer);
  test_assert(tok.type == TOKEN_LIT, "token is LITERAL");

  snobol_lexer_destroy(lexer);
}


/* ===== test_coverage_misc (part): coverage-driven tests merged into test_lexer.c ===== */
#include <stdint.h>
#include "../../core/include/snobol/array.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/string_fn.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_misc_lexer(void) {
  test_suite("Coverage: lexer NULL guards + tokens");

  test_assert(snobol_lexer_create(nullptr, 0) == NULL, "lexer_create(NULL)");
  test_assert(snobol_lexer_next(nullptr).type == TOKEN_EOF, "next(NULL)");
  test_assert(snobol_lexer_peek(nullptr).type == TOKEN_EOF, "peek(NULL)");
  test_assert(snobol_lexer_get_pos(nullptr) == 0, "get_pos(NULL)");
  test_assert(snobol_lexer_get_line(nullptr) == 1, "get_line(NULL)");
  snobol_lexer_state_t st = snobol_lexer_save(nullptr);
  test_assert(st.pos == 0, "save(NULL) zero state");
  snobol_lexer_restore(nullptr, st);
  snobol_lexer_destroy(nullptr);
  test_assert(true, "lexer NULL guards");

  /* Token names for every single-char operator. */
  test_assert(
      (strcmp(snobol_token_name(TOKEN_STAR), "STAR") == 0 &&
       strcmp(snobol_token_name(TOKEN_PLUS), "PLUS") == 0 &&
       strcmp(snobol_token_name(TOKEN_QUESTION), "QUESTION") == 0 &&
       strcmp(snobol_token_name(TOKEN_ANCHOR_START), "ANCHOR_START") == 0 &&
       strcmp(snobol_token_name(TOKEN_ANCHOR_END), "ANCHOR_END") == 0 &&
       strcmp(snobol_token_name(TOKEN_AT), "AT") == 0 &&
       strcmp(snobol_token_name(TOKEN_COLON), "COLON") == 0 &&
       strcmp(snobol_token_name(TOKEN_LBRACKET), "LBRACKET") == 0 &&
       strcmp(snobol_token_name(TOKEN_RBRACKET), "RBRACKET") == 0 &&
       strcmp(snobol_token_name(TOKEN_EQUALS), "EQUALS") == 0 &&
       strcmp(snobol_token_name(TOKEN_COMMA), "COMMA") == 0 &&
       strcmp(snobol_token_name(TOKEN_EOF), "EOF") == 0 &&
       strcmp(snobol_token_name((token_type_t)99), "UNKNOWN") == 0) != 0,
      "operator token names resolve");

  /* Unterminated charclass still yields a CHARCLASS token. */
  {
    snobol_lexer_t *l = snobol_lexer_create("[abc", 4);
    token_t t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_CHARCLASS, "unterminated charclass token");
    snobol_lexer_destroy(l);
  }

  /* Single-char operator tokens. */
  {
    snobol_lexer_t *l = snobol_lexer_create("^$@:*+?", 7);
    token_t t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_ANCHOR_START, "^ token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_ANCHOR_END, "$ token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_AT, "@ token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_COLON, ": token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_STAR, "* token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_PLUS, "+ token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_QUESTION, "? token");
    t = snobol_lexer_next(l);
    test_assert(t.type == TOKEN_EOF, "EOF reached");
    snobol_lexer_destroy(l);
  }
}

/* ── array edge cases ─────────────────────────────────────────────────────── */


void test_lexer_suite(void) {
  test_lexer_create_destroy();
  test_lexer_create_null_source();
  test_lexer_literal();
  test_lexer_alternation();
  test_lexer_whitespace();
  test_lexer_charclass();
  test_lexer_anchors();
  test_lexer_arbno();
  test_lexer_capture();
  test_lexer_peek();
  test_lexer_position_tracking();
  test_lexer_single_quotes();
  test_cov_misc_lexer();
}
