/**
 * @file lexer.c
 * @brief Lexer implementation for SNOBOL pattern syntax
 *
 * Converts UTF-8 source text into a stream of tokens.
 * Handles character-by-character scanning with proper UTF-8 support.
 */

#include "snobol/lexer.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Lexer state structure (opaque to callers)
 */
struct snobol_lexer {
  const char *source; /* Source text (not owned) */
  size_t len;         /* Length of source */
  size_t pos;         /* Current position */
  size_t line;        /* Current line (1-based) */
  size_t column;      /* Current column (1-based) */
  token_t peek_token; /* Peeked token (if any) */
  bool has_peek;      /* Whether peek_token is valid */
};

/* Forward declarations */
static token_t make_token(token_type_t type);
static token_t make_token_with_text(token_type_t type, const char *text,
                                    size_t len);
static void skip_whitespace(snobol_lexer_t *lexer);
static token_t scan_literal(snobol_lexer_t *lexer);
static token_t scan_ident(snobol_lexer_t *lexer);
static token_t scan_charclass(snobol_lexer_t *lexer);
static bool is_ident_start(char c);
static bool is_ident_continue(char c);

snobol_lexer_t *snobol_lexer_create(const char *source, size_t len) {
  if (!source) {
    return nullptr;
  }

  snobol_lexer_t *lexer = (snobol_lexer_t *)calloc(1, sizeof(snobol_lexer_t));
  if (!lexer) {
    return nullptr;
  }

  lexer->source = source;
  lexer->len = len;
  lexer->pos = 0;
  lexer->line = 1;
  lexer->column = 1;
  lexer->has_peek = false;

  return lexer;
}

static token_t make_token(token_type_t type) {
  token_t token;
  token.type = type;
  return token;
}

static token_t make_token_with_text(token_type_t type, const char *text,
                                    size_t len) {
  token_t token = make_token(type);
  token.data.string.text = text;
  token.data.string.len = len;
  return token;
}

static bool is_ident_start(char c) {
  return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') != 0;
}

static bool is_ident_continue(char c) {
  return (is_ident_start(c) || (c >= '0' && c <= '9')) != 0;
}

static void skip_whitespace(snobol_lexer_t *lexer) {
  while (lexer->pos < lexer->len) {
    char c = lexer->source[lexer->pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      if (c == '\n') {
        lexer->line++;
        lexer->column = 0;
      }
      lexer->pos++;
      lexer->column++;
    } else {
      break;
    }
  }
}

static token_t scan_literal(snobol_lexer_t *lexer) {
  /* Opening quote already consumed */
  size_t start = lexer->pos;

  while (lexer->pos < lexer->len) {
    char c = lexer->source[lexer->pos];
    if (c == '\'') {
      /* Closing quote found */
      token_t token = make_token_with_text(TOKEN_LIT, &lexer->source[start],
                                           lexer->pos - start);
      lexer->pos++; /* Skip closing quote */
      lexer->column += (lexer->pos - start) + 1;
      return token;
    }
    lexer->pos++;
    lexer->column++;
  }

  /* Unterminated literal - return what we have */
  return make_token_with_text(TOKEN_LIT, &lexer->source[start],
                              lexer->pos - start);
}

static token_t scan_ident(snobol_lexer_t *lexer) {
  size_t start = lexer->pos;

  while (lexer->pos < lexer->len &&
         is_ident_continue(lexer->source[lexer->pos])) {
    lexer->pos++;
    lexer->column++;
  }

  return make_token_with_text(TOKEN_IDENT, &lexer->source[start],
                              lexer->pos - start);
}

static token_t scan_charclass(snobol_lexer_t *lexer) {
  /* Opening [ already consumed */
  size_t start = lexer->pos;

  while (lexer->pos < lexer->len) {
    char c = lexer->source[lexer->pos];
    if (c == ']') {
      /* Closing bracket found */
      token_t token = make_token_with_text(
          TOKEN_CHARCLASS, &lexer->source[start], lexer->pos - start);
      lexer->pos++; /* Skip closing bracket */
      lexer->column += (lexer->pos - start) + 1;
      return token;
    }
    lexer->pos++;
    lexer->column++;
  }

  /* Unterminated charclass - return what we have */
  return make_token_with_text(TOKEN_CHARCLASS, &lexer->source[start],
                              lexer->pos - start);
}

token_t snobol_lexer_next(snobol_lexer_t *lexer) {
  if (!lexer) {
    return make_token(TOKEN_EOF);
  }

  /* Return peeked token if available */
  if (lexer->has_peek) {
    lexer->has_peek = false;
    return lexer->peek_token;
  }

  skip_whitespace(lexer);

  if (lexer->pos >= lexer->len) {
    return make_token(TOKEN_EOF);
  }

  char c = lexer->source[lexer->pos];

  /* Single-character tokens */
  switch (c) {
    case '|':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_PIPE);

    case '(':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_LPAREN);

    case ')':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_RPAREN);

    case '*':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_STAR);

    case '+':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_PLUS);

    case '?':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_QUESTION);

    case '^':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_ANCHOR_START);

    case '$':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_ANCHOR_END);

    case '@':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_AT);

    case ':':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_COLON);

    case '[':
      lexer->pos++;
      lexer->column++;
      return scan_charclass(lexer);

    case ']':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_RBRACKET);

    case '=':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_EQUALS);

    case ',':
      lexer->pos++;
      lexer->column++;
      return make_token(TOKEN_COMMA);

    case '\'':
      lexer->pos++; /* Skip opening quote */
      lexer->column++;
      return scan_literal(lexer);

    default:
      /* Check for identifier */
      if (is_ident_start(c)) {
        return scan_ident(lexer);
      }

      /* Unknown character - skip and try again */
      lexer->pos++;
      lexer->column++;
      return snobol_lexer_next(lexer);
  }
}

token_t snobol_lexer_peek(snobol_lexer_t *lexer) {
  if (!lexer) {
    return make_token(TOKEN_EOF);
  }

  if (!lexer->has_peek) {
    lexer->peek_token = snobol_lexer_next(lexer);
    lexer->has_peek = true;
  }

  return lexer->peek_token;
}

size_t snobol_lexer_get_pos(snobol_lexer_t *lexer) {
  if (!lexer) {
    return 0;
  }
  return lexer->pos;
}

size_t snobol_lexer_get_line(snobol_lexer_t *lexer) {
  if (!lexer) {
    return 1;
  }
  return lexer->line;
}

snobol_lexer_state_t snobol_lexer_save(snobol_lexer_t *lexer) {
  snobol_lexer_state_t state = {0};
  if (!lexer) {
    return state;
  }
  state.pos = lexer->pos;
  state.line = lexer->line;
  state.column = lexer->column;
  state.peek_token = lexer->peek_token;
  state.has_peek = lexer->has_peek;
  return state;
}

void snobol_lexer_restore(snobol_lexer_t *lexer, snobol_lexer_state_t state) {
  if (!lexer) {
    return;
  }
  lexer->pos = state.pos;
  lexer->line = state.line;
  lexer->column = state.column;
  lexer->peek_token = state.peek_token;
  lexer->has_peek = state.has_peek;
}

void snobol_lexer_destroy(snobol_lexer_t *lexer) {
  if (lexer) {
    free(lexer);
  }
}

const char *snobol_token_name(token_type_t type) {
  switch (type) {
    case TOKEN_EOF: return "EOF";
    case TOKEN_LIT: return "LITERAL";
    case TOKEN_IDENT: return "IDENT";
    case TOKEN_CHARCLASS: return "CHARCLASS";
    case TOKEN_PIPE: return "PIPE";
    case TOKEN_LPAREN: return "LPAREN";
    case TOKEN_RPAREN: return "RPAREN";
    case TOKEN_STAR: return "STAR";
    case TOKEN_PLUS: return "PLUS";
    case TOKEN_QUESTION: return "QUESTION";
    case TOKEN_ANCHOR_START: return "ANCHOR_START";
    case TOKEN_ANCHOR_END: return "ANCHOR_END";
    case TOKEN_AT: return "AT";
    case TOKEN_COLON: return "COLON";
    case TOKEN_LBRACKET: return "LBRACKET";
    case TOKEN_RBRACKET: return "RBRACKET";
    case TOKEN_EQUALS: return "EQUALS";
    case TOKEN_COMMA: return "COMMA";
    default: return "UNKNOWN";
  }
}
