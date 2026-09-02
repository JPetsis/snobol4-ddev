/**
 * @file parser.c
 * @brief Recursive descent parser for SNOBOL pattern syntax
 *
 * Consumes tokens from lexer and produces AST.
 * Implements the grammar defined in grammar/snobol.ebnf
 */

#include "snobol/parser.h"
#include "snobol/snobol_internal.h"
#include "snobol/vm.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Parser state structure (opaque to callers)
 */
struct snobol_parser {
  parser_error_t error;
  /* Duplicate label tracking */
  char **seen_labels;
  size_t seen_label_count;
  size_t seen_label_capacity;
  /* Sequential capture-register allocator: each @name capture receives the
   * next register (0-based), in order of appearance, matching the PHP
   * Builder::cap(reg, ...) convention. */
  int capture_reg_counter;
};

/* Forward declarations for recursive descent */
static ast_node_t *parse_pattern(snobol_parser_t *parser,
                                 snobol_lexer_t *lexer);
static ast_node_t *parse_statement(snobol_parser_t *parser,
                                   snobol_lexer_t *lexer);
static ast_node_t *parse_alternation(snobol_parser_t *parser,
                                     snobol_lexer_t *lexer);
static ast_node_t *parse_concatenation(snobol_parser_t *parser,
                                       snobol_lexer_t *lexer);
static ast_node_t *parse_repetition(snobol_parser_t *parser,
                                    snobol_lexer_t *lexer);
static ast_node_t *parse_primary(snobol_parser_t *parser,
                                 snobol_lexer_t *lexer);
static ast_node_t *parse_function_call(snobol_parser_t *parser,
                                       snobol_lexer_t *lexer);
static ast_node_t *parse_dynamic_eval(snobol_parser_t *parser,
                                      snobol_lexer_t *lexer);

/* Error handling helpers */
static void set_error(snobol_parser_t *parser, const char *msg, size_t line,
                      size_t col);
static token_t advance(snobol_lexer_t *lexer);
static token_t peek(snobol_lexer_t *lexer);
static bool expect(snobol_parser_t *parser, snobol_lexer_t *lexer,
                   token_type_t type);
static bool match(snobol_lexer_t *lexer, token_type_t type);

snobol_parser_t *snobol_parser_create(void) {
  snobol_parser_t *parser =
      (snobol_parser_t *)calloc(1, sizeof(snobol_parser_t));
  if (parser) {
    parser->error.has_error = false;
    parser->error.message[0] = '\0';
    parser->error.line = 0;
    parser->error.column = 0;
    parser->seen_labels = nullptr;
    parser->seen_label_count = 0;
    parser->seen_label_capacity = 0;
  }
  return parser;
}

static void set_error(snobol_parser_t *parser, const char *msg, size_t line,
                      size_t col) {
  if (!parser || !msg) {
    return;
  }

  parser->error.has_error = true;
  parser->error.line = line;
  parser->error.column = col;

  /* Truncate message if too long */
  size_t len = strlen(msg);
  if (len >= SNOBOL_PARSER_ERROR_MAX) {
    len = SNOBOL_PARSER_ERROR_MAX - 1;
  }
  memcpy(parser->error.message, msg, len);
  parser->error.message[len] = '\0';
}

static token_t advance(snobol_lexer_t *lexer) {
  return snobol_lexer_next(lexer);
}

static token_t peek(snobol_lexer_t *lexer) {
  return snobol_lexer_peek(lexer);
}

static bool match(snobol_lexer_t *lexer, token_type_t type) {
  token_t tok = peek(lexer);
  return tok.type == type;
}

static bool expect(snobol_parser_t *parser, snobol_lexer_t *lexer,
                   token_type_t type) {
  token_t tok = peek(lexer);
  if (tok.type == type) {
    advance(lexer);
    return true;
  }

  /* Surface a lexical error verbatim instead of a misleading expectation */
  if (tok.type == TOKEN_ERROR && snobol_lexer_has_error(lexer)) {
    set_error(parser, snobol_lexer_get_error(lexer),
              snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
    return false;
  }

  /* Error: unexpected token */
  char msg[128];
  snprintf(msg, sizeof(msg), "Expected %s, got %s", snobol_token_name(type),
           snobol_token_name(tok.type));
  set_error(parser, msg, snobol_lexer_get_line(lexer),
            snobol_lexer_get_pos(lexer));
  return false;
}

/**
 * Parse a mandatory integer argument for a builtin function.
 * Emits a descriptive error naming the function when the argument is
 * missing, not an integer, or outside the int32 range the AST accepts.
 */
static bool parse_integer_arg(snobol_parser_t *parser, snobol_lexer_t *lexer,
                              const char *func, int32_t *out) {
  token_t tok = peek(lexer);

  if (tok.type == TOKEN_ERROR && snobol_lexer_has_error(lexer)) {
    set_error(parser, snobol_lexer_get_error(lexer),
              snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
    return false;
  }

  if (tok.type != TOKEN_INTEGER) {
    char msg[192];
    const char *token_desc;
    char lit_buf[48];
    if (tok.type == TOKEN_LIT) {
      size_t n = tok.data.string.len;
      if (n > sizeof(lit_buf) - 3) {
        n = sizeof(lit_buf) - 3;
      }
      lit_buf[0] = '\'';
      memcpy(lit_buf + 1, tok.data.string.text, n);
      lit_buf[n + 1] = '\'';
      lit_buf[n + 2] = '\0';
      token_desc = lit_buf;
    } else if (tok.type == TOKEN_IDENT) {
      size_t n = tok.data.string.len;
      if (n > sizeof(lit_buf) - 1) {
        n = sizeof(lit_buf) - 1;
      }
      memcpy(lit_buf, tok.data.string.text, n);
      lit_buf[n] = '\0';
      token_desc = lit_buf;
    } else {
      token_desc = snobol_token_name(tok.type);
    }
    snprintf(msg, sizeof(msg), "%s expects an integer argument, got %s", func,
             token_desc);
    set_error(parser, msg, snobol_lexer_get_line(lexer),
              snobol_lexer_get_pos(lexer));
    return false;
  }

  int64_t value = tok.data.integer.value;
  if (value < INT32_MIN || value > INT32_MAX) {
    char msg[192];
    snprintf(msg, sizeof(msg),
             "%s integer argument %lld is out of range (int32)", func,
             (long long)value);
    set_error(parser, msg, snobol_lexer_get_line(lexer),
              snobol_lexer_get_pos(lexer));
    return false;
  }

  advance(lexer);
  *out = (int32_t)value;
  return true;
}

ast_node_t *snobol_parser_parse(snobol_parser_t *parser,
                                snobol_lexer_t *lexer) {
  if (!parser || !lexer) {
    return nullptr;
  }

  /* Clear any previous error */
  parser->error.has_error = false;

  /* Clear seen labels from previous parse */
  for (size_t i = 0; i < parser->seen_label_count; i++) {
    free(parser->seen_labels[i]);
  }
  parser->seen_label_count = 0;

  /* Parse the pattern */
  ast_node_t *ast = parse_pattern(parser, lexer);

  /* Check for trailing tokens */
  if (!parser->error.has_error) {
    token_t tok = peek(lexer);
    if (tok.type == TOKEN_ERROR && snobol_lexer_has_error(lexer)) {
      set_error(parser, snobol_lexer_get_error(lexer),
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      snobol_ast_free(ast);
      return nullptr;
    }
    if (tok.type != TOKEN_EOF) {
      set_error(parser, "Unexpected token after pattern",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      snobol_ast_free(ast);
      return nullptr;
    }
  }

  return ast;
}

static ast_node_t *parse_pattern(snobol_parser_t *parser,
                                 snobol_lexer_t *lexer) {
  return parse_statement(parser, lexer);
}

static ast_node_t *parse_statement(snobol_parser_t *parser,
                                   snobol_lexer_t *lexer) {
  /* Check for label: IDENT ':' */
  token_t tok = peek(lexer);
  if (tok.type == TOKEN_IDENT) {
    /* Look ahead to check for ':' */
    snobol_lexer_state_t saved_state = snobol_lexer_save(lexer);
    advance(lexer);
    token_t next = peek(lexer);

    if (next.type == TOKEN_COLON) {
      /* This is a label */
      advance(lexer); /* Consume ':' */

      /* Create label name */
      char *label_name = (char *)malloc(tok.data.string.len + 1);
      if (label_name) {
        memcpy(label_name, tok.data.string.text, tok.data.string.len);
        label_name[tok.data.string.len] = '\0';
      }

      /* Duplicate label detection */
      for (size_t i = 0; i < parser->seen_label_count; i++) {
        if (parser->seen_labels[i] &&
            strcmp(parser->seen_labels[i], label_name) == 0) {
          char msg[256];
          snprintf(msg, sizeof(msg), "Duplicate label: '%s'", label_name);
          set_error(parser, msg, snobol_lexer_get_line(lexer),
                    snobol_lexer_get_pos(lexer));
          free(label_name);
          return nullptr;
        }
      }

      /* Record label name */
      if (parser->seen_label_count >= parser->seen_label_capacity) {
        size_t new_cap =
            parser->seen_label_capacity ? parser->seen_label_capacity * 2 : 4;
        char **new_labels = (char **)realloc((void *)parser->seen_labels,
                                             new_cap * sizeof(char *));
        if (new_labels) {
          parser->seen_labels = new_labels;
          parser->seen_label_capacity = new_cap;
        }
      }
      if (parser->seen_label_count < parser->seen_label_capacity) {
        char *copy = (char *)malloc(strlen(label_name) + 1);
        if (copy) {
          strcpy(copy, label_name);
        }
        parser->seen_labels[parser->seen_label_count++] = copy;
      }

      /* Parse the target pattern */
      ast_node_t *target = parse_statement(parser, lexer);
      if (!target) {
        free(label_name);
        return nullptr;
      }

      ast_node_t *label = snobol_ast_create_label(label_name, target);
      free(label_name);
      return label;
    }
    /* Not a label, restore position */
    snobol_lexer_restore(lexer, saved_state);
  }

  /* Parse the pattern expression */
  ast_node_t *pattern = parse_alternation(parser, lexer);
  if (!pattern) {
    return nullptr;
  }

  /* Check for goto: ':' '(' IDENT ')' */
  tok = peek(lexer);
  if (tok.type == TOKEN_COLON) {
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_LPAREN)) {
      snobol_ast_free(pattern);
      return nullptr;
    }

    tok = peek(lexer);
    if (tok.type != TOKEN_IDENT) {
      set_error(parser, "Expected label name after ':('",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      snobol_ast_free(pattern);
      return nullptr;
    }

    /* Capture label name for goto */
    char *goto_label = (char *)malloc(tok.data.string.len + 1);
    if (goto_label) {
      memcpy(goto_label, tok.data.string.text, tok.data.string.len);
      goto_label[tok.data.string.len] = '\0';
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      free(goto_label);
      snobol_ast_free(pattern);
      return nullptr;
    }

    /* Create a concat of the pattern and the goto node */
    ast_node_t *goto_node = snobol_ast_create_goto(goto_label);
    free(goto_label);
    if (!goto_node) {
      snobol_ast_free(pattern);
      return nullptr;
    }

    ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
    if (!parts) {
      snobol_ast_free(pattern);
      snobol_ast_free(goto_node);
      return nullptr;
    }
    parts[0] = pattern;
    parts[1] = goto_node;
    return snobol_ast_create_concat(parts, 2);
  }

  return pattern;
}

static ast_node_t *parse_alternation(snobol_parser_t *parser,
                                     snobol_lexer_t *lexer) {
  ast_node_t *left = parse_concatenation(parser, lexer);
  if (!left) {
    return nullptr;
  }

  while (match(lexer, TOKEN_PIPE)) {
    advance(lexer); /* Consume '|' */

    ast_node_t *right = parse_concatenation(parser, lexer);
    if (!right) {
      snobol_ast_free(left);
      return nullptr;
    }

    left = snobol_ast_create_alt(left, right);
  }

  return left;
}

static ast_node_t *parse_concatenation(snobol_parser_t *parser,
                                       snobol_lexer_t *lexer) {
  /* Collect all concatenated parts */
  ast_node_t **parts = nullptr;
  size_t count = 0;
  size_t capacity = 0;

  while (true) {
    token_t tok = peek(lexer);

    /* Check if this token starts a primary pattern */
    bool is_primary =
        (tok.type == TOKEN_LIT || tok.type == TOKEN_CHARCLASS ||
         tok.type == TOKEN_LPAREN || tok.type == TOKEN_ANCHOR_START ||
         tok.type == TOKEN_ANCHOR_END || tok.type == TOKEN_AT ||
         tok.type == TOKEN_IDENT) != 0;

    /* Check for function calls, table accesses, assignments, or bare
     * primitives (ARB, FENCE, REM) */
    if (tok.type == TOKEN_IDENT) {
      /* Look ahead for '(' / '[' / '=' or a bare primitive name */
      snobol_lexer_state_t saved_state = snobol_lexer_save(lexer);
      advance(lexer);             /* Consume IDENT */
      token_t next = peek(lexer); /* Peek at next token */
      if (next.type == TOKEN_LPAREN || next.type == TOKEN_LBRACKET ||
          next.type == TOKEN_EQUALS) {
        is_primary = true;
      } else if ((tok.data.string.len == 3 &&
                  strncmp(tok.data.string.text, "ARB", 3) == 0) ||
                 (tok.data.string.len == 5 &&
                  strncmp(tok.data.string.text, "FENCE", 5) == 0) ||
                 (tok.data.string.len == 3 &&
                  strncmp(tok.data.string.text, "REM", 3) == 0)) {
        is_primary = true;
      }
      /* Restore lexer position */
      snobol_lexer_restore(lexer, saved_state);
    }

    if (!is_primary) {
      break;
    }

    ast_node_t *part = parse_repetition(parser, lexer);
    if (!part) {
      /* Free collected parts */
      for (size_t i = 0; i < count; i++) {
        snobol_ast_free(parts[i]);
      }
      free((void *)parts);
      return nullptr;
    }

    /* Add to parts array */
    if (count >= capacity) {
      capacity = (capacity == 0) ? 4 : capacity * 2;
      ast_node_t **new_parts = (ast_node_t **)realloc(
          (void *)parts, capacity * sizeof(ast_node_t *));
      if (!new_parts) {
        snobol_ast_free(part);
        for (size_t i = 0; i < count; i++) {
          snobol_ast_free(parts[i]);
        }
        free((void *)parts);
        return nullptr;
      }
      parts = new_parts;
    }

    parts[count++] = part;
  }

  if (count == 0) {
    free((void *)parts);
    set_error(parser, "Expected pattern element", snobol_lexer_get_line(lexer),
              snobol_lexer_get_pos(lexer));
    return nullptr;
  }

  if (count == 1) {
    ast_node_t *result = parts[0];
    free((void *)parts);
    return result;
  }

  return snobol_ast_create_concat(parts, count);
}

static ast_node_t *parse_repetition(snobol_parser_t *parser,
                                    snobol_lexer_t *lexer) {
  ast_node_t *primary = parse_primary(parser, lexer);
  if (!primary) {
    return nullptr;
  }

  token_t tok = peek(lexer);

  switch (tok.type) {
    case TOKEN_STAR: advance(lexer); return snobol_ast_create_arbno(primary);

    case TOKEN_PLUS:
      advance(lexer);
      /* x+ = x x* = concat(x, arbno(x)) */
      {
        ast_node_t *clone = snobol_ast_clone(primary);
        ast_node_t *arbno = snobol_ast_create_arbno(primary);
        ast_node_t **parts = (ast_node_t **)malloc(2 * sizeof(ast_node_t *));
        if (!parts) {
          snobol_ast_free(arbno);
          snobol_ast_free(clone);
          return nullptr;
        }
        if (!clone) {
          snobol_ast_free(arbno);
          free((void *)parts);
          return nullptr;
        }
        parts[0] = clone;
        parts[1] = arbno;
        return snobol_ast_create_concat(parts, 2);
      }

    case TOKEN_QUESTION:
      advance(lexer);
      /* x? = alt(x, empty) - simplified as repetition with min=0, max=1 */
      return snobol_ast_create_repeat(primary, 0, 1);

    default: return primary;
  }
}

static ast_node_t *parse_primary(snobol_parser_t *parser,
                                 snobol_lexer_t *lexer) {
  token_t tok = peek(lexer);

  switch (tok.type) {
    case TOKEN_LIT:
      advance(lexer);
      return snobol_ast_create_lit(tok.data.string.text, tok.data.string.len);

    case TOKEN_CHARCLASS:
      advance(lexer);
      return snobol_ast_create_span(tok.data.string.text, tok.data.string.len);

    case TOKEN_LPAREN:
      advance(lexer);
      {
        ast_node_t *inner = parse_alternation(parser, lexer);
        if (!inner) {
          return nullptr;
        }

        if (!expect(parser, lexer, TOKEN_RPAREN)) {
          snobol_ast_free(inner);
          return nullptr;
        }

        return inner;
      }

    case TOKEN_ANCHOR_START:
      advance(lexer);
      {
        ast_node_t *node = (ast_node_t *)calloc(1, sizeof(ast_node_t));
        if (node) {
          node->type = AST_ANCHOR;
          node->data.anchor.atype = ANCHOR_START;
        }
        return node;
      }

    case TOKEN_ANCHOR_END:
      advance(lexer);
      {
        ast_node_t *node = (ast_node_t *)calloc(1, sizeof(ast_node_t));
        if (node) {
          node->type = AST_ANCHOR;
          node->data.anchor.atype = ANCHOR_END;
        }
        return node;
      }

    case TOKEN_AT:
      advance(lexer);
      {
        /* Capture: @IDENT (register allocated sequentially from 0) */
        tok = peek(lexer);

        if (tok.type == TOKEN_IDENT) {
          /* Named captures are positional: each gets the next register */
          advance(lexer);
        } else {
          set_error(parser, "Expected capture target",
                    snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
          return nullptr;
        }

        if (parser->capture_reg_counter >= MAX_VARS) {
          set_error(parser, "Too many captures (max 64)",
                    snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
          return nullptr;
        }
        int reg = parser->capture_reg_counter++;

        ast_node_t *sub = parse_primary(parser, lexer);
        if (!sub) {
          return nullptr;
        }

        return snobol_ast_create_cap(reg, sub);
      }

    case TOKEN_IDENT:
      /* Could be function call or bare identifier */
      return parse_function_call(parser, lexer);

    default:
      if (tok.type == TOKEN_ERROR && snobol_lexer_has_error(lexer)) {
        set_error(parser, snobol_lexer_get_error(lexer),
                  snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
        return nullptr;
      }
      if (tok.type == TOKEN_INTEGER) {
        set_error(parser,
                  "Integer literal is not valid here (digits only appear "
                  "inside builtin argument lists)",
                  snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
        return nullptr;
      }
      set_error(parser, "Unexpected token in pattern",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
  }
}

static ast_node_t *parse_function_call(snobol_parser_t *parser,
                                       snobol_lexer_t *lexer) {
  token_t name_tok = peek(lexer);

  if (name_tok.type != TOKEN_IDENT) {
    set_error(parser, "Expected function name", snobol_lexer_get_line(lexer),
              snobol_lexer_get_pos(lexer));
    return nullptr;
  }

  /* Check function name */
  const char *name = name_tok.data.string.text;
  size_t name_len = name_tok.data.string.len;

  /* Look ahead for '(' */
  advance(lexer);
  token_t next = peek(lexer);

  if (next.type != TOKEN_LPAREN) {
    /* Not a function call: bare pattern primitives ARB / FENCE / REM. */
    if (name_len == 3 && strncmp(name, "ARB", 3) == 0) {
      /* ARB = arbitrary substring; PHP Builder::arb() is arbno(len(1)). */
      return snobol_ast_create_arbno(snobol_ast_create_len(1));
    }
    if (name_len == 5 && strncmp(name, "FENCE", 5) == 0) {
      return snobol_ast_create_fence();
    }
    if (name_len == 3 && strncmp(name, "REM", 3) == 0) {
      return snobol_ast_create_rem();
    }
    /* For now, return error - identifiers alone aren't valid patterns */
    set_error(parser, "Bare identifier is not a valid pattern",
              snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
    return nullptr;
  }

  advance(lexer); /* Consume '(' */

  /* Parse arguments based on function name */
  if (strncmp(name, "SPAN", name_len) == 0) {
    token_t arg = peek(lexer);
    if (arg.type != TOKEN_LIT && arg.type != TOKEN_CHARCLASS) {
      set_error(parser, "SPAN expects string argument",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_span(arg.data.string.text, arg.data.string.len);
  }

  if (strncmp(name, "BREAK", name_len) == 0) {
    token_t arg = peek(lexer);
    if (arg.type != TOKEN_LIT && arg.type != TOKEN_CHARCLASS) {
      set_error(parser, "BREAK expects string argument",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_break(arg.data.string.text, arg.data.string.len);
  }

  if (strncmp(name, "BREAKX", name_len) == 0) {
    token_t arg = peek(lexer);
    if (arg.type != TOKEN_LIT && arg.type != TOKEN_CHARCLASS) {
      set_error(parser, "BREAKX expects string argument",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_breakx(arg.data.string.text, arg.data.string.len);
  }

  if (strncmp(name, "ANY", name_len) == 0) {
    token_t arg = peek(lexer);
    if (arg.type == TOKEN_RPAREN) {
      /* ANY() with no args */
      advance(lexer);
      return snobol_ast_create_any(nullptr, 0);
    }

    if (arg.type != TOKEN_LIT && arg.type != TOKEN_CHARCLASS) {
      set_error(parser, "ANY expects string argument",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_any(arg.data.string.text, arg.data.string.len);
  }

  if (strncmp(name, "NOTANY", name_len) == 0) {
    token_t arg = peek(lexer);
    if (arg.type != TOKEN_LIT && arg.type != TOKEN_CHARCLASS) {
      set_error(parser, "NOTANY expects string argument",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_notany(arg.data.string.text, arg.data.string.len);
  }

  if (strncmp(name, "LEN", name_len) == 0) {
    int32_t n = 1;
    if (!parse_integer_arg(parser, lexer, "LEN", &n)) {
      return nullptr;
    }

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_len(n);
  }

  if (strncmp(name, "EVAL", name_len) == 0) {
    return parse_dynamic_eval(parser, lexer);
  }

  if (strncmp(name, "POS", name_len) == 0) {
    int32_t n = 0;
    if (!parse_integer_arg(parser, lexer, "POS", &n)) {
      return nullptr;
    }

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_pos(n);
  }

  if (strncmp(name, "TAB", name_len) == 0) {
    int32_t n = 0;
    if (!parse_integer_arg(parser, lexer, "TAB", &n)) {
      return nullptr;
    }

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }

    return snobol_ast_create_tab(n);
  }

  /* --- Source-syntax primitive parity (see source-primitive-parity spec) --- */

  if (strncmp(name, "ARB", name_len) == 0) {
    /* ARB() — zero-argument function form of the primitive */
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_arbno(snobol_ast_create_len(1));
  }

  if (strncmp(name, "ARBNO", name_len) == 0) {
    if (match(lexer, TOKEN_RPAREN)) {
      set_error(parser, "ARBNO expects one pattern argument: ARBNO(pattern)",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    ast_node_t *sub = parse_repetition(parser, lexer);
    if (!sub) {
      return nullptr;
    }
    if (match(lexer, TOKEN_COMMA)) {
      set_error(parser, "ARBNO expects one pattern argument: ARBNO(pattern)",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      snobol_ast_free(sub);
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    return snobol_ast_create_arbno(sub);
  }

  if (strncmp(name, "BAL", name_len) == 0) {
    uint32_t open_cp = '(';
    uint32_t close_cp = ')';
    token_t arg = peek(lexer);
    if (arg.type == TOKEN_RPAREN) {
      /* BAL() — default parentheses delimiters */
      advance(lexer);
      return snobol_ast_create_bal(open_cp, close_cp);
    }

    /* First delimiter: BAL('(') / BAL('(', ')') */
    if (arg.type != TOKEN_LIT) {
      set_error(parser,
                "BAL expects string delimiters: BAL() or BAL('(', ')')",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);
    int bytes = 0;
    if (!utf8_peek_next(arg.data.string.text, arg.data.string.len, 0,
                        &open_cp, &bytes)) {
      set_error(parser, "BAL delimiter is not valid UTF-8",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }

    if (match(lexer, TOKEN_COMMA)) {
      advance(lexer);
      arg = peek(lexer);
      if (arg.type == TOKEN_RPAREN) {
        set_error(parser,
                  "BAL expects a closing delimiter after the comma: "
                  "BAL('(', ')')",
                  snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
        return nullptr;
      }
      if (arg.type != TOKEN_LIT) {
        set_error(parser,
                  "BAL expects string delimiters: BAL() or BAL('(', ')')",
                  snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
        return nullptr;
      }
      advance(lexer);
      if (!utf8_peek_next(arg.data.string.text, arg.data.string.len, 0,
                          &close_cp, &bytes)) {
        set_error(parser, "BAL delimiter is not valid UTF-8",
                  snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
        return nullptr;
      }
    }

    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_bal(open_cp, close_cp);
  }

  if (strncmp(name, "FENCE", name_len) == 0) {
    if (!match(lexer, TOKEN_RPAREN)) {
      set_error(parser, "FENCE expects no arguments",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);
    return snobol_ast_create_fence();
  }

  if (strncmp(name, "REM", name_len) == 0) {
    if (!match(lexer, TOKEN_RPAREN)) {
      set_error(parser, "REM expects no arguments",
                snobol_lexer_get_line(lexer), snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    advance(lexer);
    return snobol_ast_create_rem();
  }

  if (strncmp(name, "RPOS", name_len) == 0) {
    int32_t n = 0;
    if (!parse_integer_arg(parser, lexer, "RPOS", &n)) {
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_rpos(n);
  }

  if (strncmp(name, "RTAB", name_len) == 0) {
    int32_t n = 0;
    if (!parse_integer_arg(parser, lexer, "RTAB", &n)) {
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_rtab(n);
  }

  if (strncmp(name, "repeat", name_len) == 0) {
    const char *sig = "repeat expects three arguments: repeat(pattern, min, max)";
    if (match(lexer, TOKEN_RPAREN)) {
      set_error(parser, sig, snobol_lexer_get_line(lexer),
                snobol_lexer_get_pos(lexer));
      return nullptr;
    }
    ast_node_t *sub = parse_repetition(parser, lexer);
    if (!sub) {
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_COMMA)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    int32_t min = 0;
    if (!parse_integer_arg(parser, lexer, "repeat", &min)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    if (match(lexer, TOKEN_RPAREN)) {
      set_error(parser, sig, snobol_lexer_get_line(lexer),
                snobol_lexer_get_pos(lexer));
      snobol_ast_free(sub);
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_COMMA)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    int32_t max = 0;
    if (!parse_integer_arg(parser, lexer, "repeat", &max)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      snobol_ast_free(sub);
      return nullptr;
    }
    if (min < 0) {
      char msg[160];
      snprintf(msg, sizeof(msg),
               "repeat: min must be non-negative (got %d)", min);
      set_error(parser, msg, snobol_lexer_get_line(lexer),
                snobol_lexer_get_pos(lexer));
      snobol_ast_free(sub);
      return nullptr;
    }
    if (max < min) {
      char msg[160];
      snprintf(msg, sizeof(msg), "repeat: max (%d) must be >= min (%d)", max,
               min);
      set_error(parser, msg, snobol_lexer_get_line(lexer),
                snobol_lexer_get_pos(lexer));
      snobol_ast_free(sub);
      return nullptr;
    }
    return snobol_ast_create_repeat(sub, min, max);
  }

  if (strncmp(name, "ABORT", name_len) == 0) {
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_abort();
  }

  if (strncmp(name, "FAIL", name_len) == 0) {
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_fail();
  }

  if (strncmp(name, "SUCCEED", name_len) == 0) {
    if (!expect(parser, lexer, TOKEN_RPAREN)) {
      return nullptr;
    }
    return snobol_ast_create_succeed();
  }

  /* Unknown function */
  set_error(parser, "Unknown function", snobol_lexer_get_line(lexer),
            snobol_lexer_get_pos(lexer));
  return nullptr;
}

static ast_node_t *parse_dynamic_eval(snobol_parser_t *parser,
                                      snobol_lexer_t *lexer) {
  /* EVAL already consumed, '(' already consumed */

  /* Parse the inner pattern expression */
  ast_node_t *expr = parse_alternation(parser, lexer);
  if (!expr) {
    return nullptr;
  }

  if (!expect(parser, lexer, TOKEN_RPAREN)) {
    snobol_ast_free(expr);
    return nullptr;
  }

  /* Create dynamic eval node */
  ast_node_t *node = (ast_node_t *)calloc(1, sizeof(ast_node_t));
  if (node) {
    node->type = AST_DYNAMIC_EVAL;
    node->data.dynamic_eval.expr = expr;
  }

  return node;
}

bool snobol_parser_has_error(snobol_parser_t *parser) {
  if (!parser) {
    return false;
  }
  return parser->error.has_error;
}

const char *snobol_parser_get_error(snobol_parser_t *parser) {
  if (!parser || !parser->error.has_error) {
    return nullptr;
  }
  return parser->error.message;
}

void snobol_parser_get_error_location(snobol_parser_t *parser, size_t *line,
                                      size_t *column) {
  if (!parser) {
    return;
  }
  if (line) {
    *line = parser->error.line;
  }
  if (column) {
    *column = parser->error.column;
  }
}

void snobol_parser_clear_error(snobol_parser_t *parser) {
  if (!parser) {
    return;
  }
  parser->error.has_error = false;
  parser->error.message[0] = '\0';
  parser->error.line = 0;
  parser->error.column = 0;
}

void snobol_parser_destroy(snobol_parser_t *parser) {
  if (parser) {
    for (size_t i = 0; i < parser->seen_label_count; i++) {
      free(parser->seen_labels[i]);
    }
    free((void *)parser->seen_labels);
    free(parser);
  }
}
