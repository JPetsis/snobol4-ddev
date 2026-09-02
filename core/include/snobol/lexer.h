#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
#include "snobol/c23_compat.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file lexer.h
 * @brief Lexer for SNOBOL pattern syntax
 *
 * Converts UTF-8 source text into a stream of tokens.
 * Opaque handle pattern - implementation details hidden from callers.
 */

/**
 * Token types matching SNOBOL pattern syntax
 */
typedef enum {
  TOKEN_EOF,          /* End of input */
  TOKEN_LIT,          /* Literal string: 'text' */
  TOKEN_IDENT,        /* Identifier: name */
  TOKEN_CHARCLASS,    /* Character class: [abc] */
  TOKEN_INTEGER,      /* Integer literal: 5, -1 */
  TOKEN_PIPE,         /* Alternation: | */
  TOKEN_LPAREN,       /* Left paren: ( */
  TOKEN_RPAREN,       /* Right paren: ) */
  TOKEN_STAR,         /* Zero-or-more: * */
  TOKEN_PLUS,         /* One-or-more: + */
  TOKEN_QUESTION,     /* Optional: ? */
  TOKEN_ANCHOR_START, /* Start anchor: ^ */
  TOKEN_ANCHOR_END,   /* End anchor: $ */
  TOKEN_AT,           /* Capture: @ */
  TOKEN_COLON,        /* Label/goto marker: : */
  TOKEN_LBRACKET,     /* Left bracket: [ */
  TOKEN_RBRACKET,     /* Right bracket: ] */
  TOKEN_DOT,          /* Match naming: . */
  TOKEN_EQUALS,       /* Assignment: = */
  TOKEN_COMMA,        /* Comma: , */
  TOKEN_ERROR         /* Lexical error (see snobol_lexer_get_error) */
} token_type_t;

/**
 * Token value representation
 */
typedef struct {
  token_type_t type;

  union {
    /* TOKEN_LIT, TOKEN_IDENT, TOKEN_CHARCLASS */
    struct {
      const char *text; /* Pointer into source (not owned) */
      size_t len;       /* Length of token text */
    } string;

    /* TOKEN_INTEGER */
    struct {
      int64_t value; /* Parsed integer value */
    } integer;

    /* Single-character tokens use no additional data */
  } data;
} token_t;

/**
 * Opaque lexer state
 */
typedef struct snobol_lexer snobol_lexer_t;

/**
 * Create a new lexer for the given source
 * @param source UTF-8 source text (must remain valid until lexer is destroyed)
 * @param len Length of source (or -1 for strlen)
 * @return New lexer (caller owns, must free with snobol_lexer_destroy)
 */
snobol_lexer_t *snobol_lexer_create(const char *source, size_t len);

/**
 * Get the next token from the lexer
 * @param lexer Lexer instance
 * @return Next token (TOKEN_EOF when done)
 */
token_t snobol_lexer_next(snobol_lexer_t *lexer);

/**
 * Peek at the next token without consuming it
 * @param lexer Lexer instance
 * @return Next token (TOKEN_EOF when done)
 */
token_t snobol_lexer_peek(snobol_lexer_t *lexer);

/**
 * Get current position in source (for error reporting)
 * @param lexer Lexer instance
 * @return 0-based byte offset in source
 */
size_t snobol_lexer_get_pos(snobol_lexer_t *lexer);

/**
 * Get current line number (for error reporting)
 * @param lexer Lexer instance
 * @return 1-based line number
 */
size_t snobol_lexer_get_line(snobol_lexer_t *lexer);

/**
 * Lexer state for save/restore
 */
typedef struct {
  size_t pos;
  size_t line;
  size_t column;
  token_t peek_token;
  bool has_peek;
  token_type_t prev_type; /* Type of the last returned token */
} snobol_lexer_state_t;

/** Maximum length of a lexer error message. */
#define SNOBOL_LEXER_ERROR_MAX 160

/**
 * Check whether the lexer encountered an unrecognized character.
 * Once an error is set it is sticky: all subsequent tokens are TOKEN_ERROR.
 * @param lexer Lexer instance (NULL returns false)
 * @return true if a lexical error has been recorded
 */
bool snobol_lexer_has_error(const snobol_lexer_t *lexer);

/**
 * Get the lexer's error message (describes the offending character and
 * its position).  Valid only while snobol_lexer_has_error() is true.
 * @param lexer Lexer instance
 * @return Static message string (owned by the lexer, do not free)
 */
const char *snobol_lexer_get_error(const snobol_lexer_t *lexer);

/**
 * Save lexer state for later restoration
 * @param lexer Lexer instance
 * @return Saved state
 */
snobol_lexer_state_t snobol_lexer_save(snobol_lexer_t *lexer);

/**
 * Restore lexer state
 * @param lexer Lexer instance
 * @param state State to restore
 */
void snobol_lexer_restore(snobol_lexer_t *lexer, snobol_lexer_state_t state);

/**
 * Destroy lexer and free resources
 * @param lexer Lexer to destroy (NULL is safe)
 */
void snobol_lexer_destroy(snobol_lexer_t *lexer);

/**
 * Get human-readable token type name for error messages
 * @param type Token type
 * @return Static string (do not free)
 */
const char *snobol_token_name(token_type_t type);

#ifdef __cplusplus
}
#endif
