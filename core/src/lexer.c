/**
 * @file lexer.c
 * @brief Lexer implementation for SNOBOL pattern syntax
 *
 * Converts UTF-8 source text into a stream of tokens.
 * Handles character-by-character scanning with proper UTF-8 support.
 */

#include "snobol/lexer.h"
#include "snobol/snobol_internal.h"
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
  token_type_t prev_type; /* Type of the last returned token (context) */
  bool has_error;         /* Sticky lexical error (see set_lexer_error) */
  char error_msg[SNOBOL_LEXER_ERROR_MAX];
  size_t error_line;  /* Line of the offending character */
  size_t error_col;   /* Column of the offending character */
};

/* Forward declarations */
static token_t make_token(token_type_t type);
static token_t make_token_with_text(token_type_t type, const char *text,
                                    size_t len);
static void skip_whitespace(snobol_lexer_t *lexer);
static token_t scan_literal(snobol_lexer_t *lexer);
static token_t scan_ident(snobol_lexer_t *lexer);
static token_t scan_integer(snobol_lexer_t *lexer, bool negative);
static token_t scan_charclass(snobol_lexer_t *lexer);
static bool is_ident_start(char c);
static bool is_ident_continue(char c);
static bool bracket_is_table_key(const snobol_lexer_t *lexer);
static void set_lexer_error(snobol_lexer_t *lexer, char c);

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
  lexer->prev_type = TOKEN_EOF;
  lexer->has_error = false;
  lexer->error_msg[0] = '\0';

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

static token_t scan_integer(snobol_lexer_t *lexer, bool negative) {
  /* First digit already at lexer->pos (or a leading '-' was consumed) */
  int64_t value = 0;
  bool overflow = false;

  while (lexer->pos < lexer->len) {
    char c = lexer->source[lexer->pos];
    if (c < '0' || c > '9') {
      break;
    }
    if (value > (INT64_MAX - (int64_t)(c - '0')) / 10) {
      overflow = true;
    }
    value = value * 10 + (int64_t)(c - '0');
    lexer->pos++;
    lexer->column++;
  }

  if (negative && !overflow) {
    value = -value;
  }

  if (overflow) {
    /* Consume the rest of the digit run so the position is past it. */
    while (lexer->pos < lexer->len &&
           lexer->source[lexer->pos] >= '0' &&
           lexer->source[lexer->pos] <= '9') {
      lexer->pos++;
      lexer->column++;
    }
    if (!lexer->has_error) {
      lexer->has_error = true;
      lexer->error_line = lexer->line;
      lexer->error_col = lexer->column;
      snprintf(lexer->error_msg, sizeof(lexer->error_msg),
               "integer literal too large (64-bit overflow) at line %zu, "
               "column %zu",
               lexer->line, lexer->column);
    }
    return make_token(TOKEN_ERROR);
  }

  token_t token = make_token(TOKEN_INTEGER);
  token.data.integer.value = value;
  return token;
}

static void set_lexer_error(snobol_lexer_t *lexer, char c) {
  if (!lexer || lexer->has_error) {
    return; /* First error wins, and errors are sticky */
  }
  lexer->has_error = true;
  lexer->error_line = lexer->line;
  lexer->error_col = lexer->column;
  if (c >= 0x20 && c < 0x7f) {
    snprintf(lexer->error_msg, sizeof(lexer->error_msg),
             "unrecognized character '%c' at line %zu, column %zu", c,
             lexer->line, lexer->column);
  } else {
    snprintf(lexer->error_msg, sizeof(lexer->error_msg),
             "unrecognized character 0x%02X at line %zu, column %zu",
             (unsigned char)c, lexer->line, lexer->column);
  }
}

/**
 * Decide whether a '[' that directly follows an identifier is the start of
 * a table key (`IDENT['key']` / `IDENT[$vN]`) rather than a character class.
 * The lexer cannot know the parser's intent, so it uses the shape of the
 * bracketed span: a quoted literal or a `$vN` register reference reads as a
 * table key; anything else stays a character class.
 */
static bool bracket_is_table_key(const snobol_lexer_t *lexer) {
  size_t i = lexer->pos + 1; /* Skip '[' */

  /* Skip whitespace inside the brackets */
  while (i < lexer->len &&
         (lexer->source[i] == ' ' || lexer->source[i] == '\t')) {
    i++;
  }
  if (i >= lexer->len) {
    return false;
  }

  char c = lexer->source[i];

  if (c == '\'') {
    /* Quoted key: '...' with a closing quote before ']' */
    i++;
    while (i < lexer->len && lexer->source[i] != ']') {
      if (lexer->source[i] == '\'') {
        return true;
      }
      i++;
    }
    return false;
  }

  if (c == '$') {
    /* Capture-derived key: $vN */
    return (i + 2 < lexer->len && lexer->source[i + 1] == 'v' &&
            lexer->source[i + 2] >= '0' && lexer->source[i + 2] <= '9') != 0;
  }

  return false;
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

  /* Lexical errors are sticky: keep reporting the error token */
  if (lexer->has_error) {
    return make_token(TOKEN_ERROR);
  }

  /* Return peeked token if available */
  if (lexer->has_peek) {
    lexer->has_peek = false;
    lexer->prev_type = lexer->peek_token.type;
    return lexer->peek_token;
  }

  skip_whitespace(lexer);

  /* A NUL byte is not a pattern character: callers may pass a length that
   * includes the string terminator, so treat it as end of input. */
  if (lexer->pos >= lexer->len || lexer->source[lexer->pos] == '\0') {
    return make_token(TOKEN_EOF);
  }

  char c = lexer->source[lexer->pos];

  /* Single-character tokens */
  switch (c) {
    case '|':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_PIPE;
      return make_token(TOKEN_PIPE);

    case '(':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_LPAREN;
      return make_token(TOKEN_LPAREN);

    case ')':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_RPAREN;
      return make_token(TOKEN_RPAREN);

    case '*':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_STAR;
      return make_token(TOKEN_STAR);

    case '+':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_PLUS;
      return make_token(TOKEN_PLUS);

    case '?':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_QUESTION;
      return make_token(TOKEN_QUESTION);

    case '^':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_ANCHOR_START;
      return make_token(TOKEN_ANCHOR_START);

    case '$':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_ANCHOR_END;
      return make_token(TOKEN_ANCHOR_END);

    case '.':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_DOT;
      return make_token(TOKEN_DOT);

    case '@':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_AT;
      return make_token(TOKEN_AT);

    case ':':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_COLON;
      return make_token(TOKEN_COLON);

    case '[':
      if (lexer->prev_type == TOKEN_IDENT && bracket_is_table_key(lexer)) {
        lexer->pos++;
        lexer->column++;
        lexer->prev_type = TOKEN_LBRACKET;
        return make_token(TOKEN_LBRACKET);
      }
      lexer->pos++;
      lexer->column++;
      {
        token_t tok = scan_charclass(lexer);
        lexer->prev_type = tok.type;
        return tok;
      }

    case ']':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_RBRACKET;
      return make_token(TOKEN_RBRACKET);

    case '=':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_EQUALS;
      return make_token(TOKEN_EQUALS);

    case ',':
      lexer->pos++;
      lexer->column++;
      lexer->prev_type = TOKEN_COMMA;
      return make_token(TOKEN_COMMA);

    case '\'':
      lexer->pos++; /* Skip opening quote */
      lexer->column++;
      {
        token_t tok = scan_literal(lexer);
        lexer->prev_type = tok.type;
        return tok;
      }

    default:
      /* Check for identifier */
      if (is_ident_start(c)) {
        token_t tok = scan_ident(lexer);
        lexer->prev_type = tok.type;
        return tok;
      }

      /* Check for integer literal */
      if (c >= '0' && c <= '9') {
        token_t tok = scan_integer(lexer, false);
        lexer->prev_type = tok.type;
        return tok;
      }

      /* Signed integer: '-' followed by a digit */
      if (c == '-' && lexer->pos + 1 < lexer->len &&
          lexer->source[lexer->pos + 1] >= '0' &&
          lexer->source[lexer->pos + 1] <= '9') {
        lexer->pos++;
        lexer->column++;
        token_t tok = scan_integer(lexer, true);
        lexer->prev_type = tok.type;
        return tok;
      }

      /* Unrecognized character: positioned error, no silent skipping */
      set_lexer_error(lexer, c);
      lexer->prev_type = TOKEN_ERROR;
      return make_token(TOKEN_ERROR);
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
  state.prev_type = lexer->prev_type;
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
  lexer->prev_type = state.prev_type;
}

bool snobol_lexer_has_error(const snobol_lexer_t *lexer) {
  if (!lexer) {
    return false;
  }
  return lexer->has_error;
}

const char *snobol_lexer_get_error(const snobol_lexer_t *lexer) {
  if (!lexer || !lexer->has_error) {
    return nullptr;
  }
  return lexer->error_msg;
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
    case TOKEN_INTEGER: return "INTEGER";
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
    case TOKEN_DOT: return "DOT";
    case TOKEN_EQUALS: return "EQUALS";
    case TOKEN_COMMA: return "COMMA";
    case TOKEN_ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}
