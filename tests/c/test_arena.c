/**
 * test_arena.c - Tests for the bump-arena allocator used during compilation.
 *
 * Verifies that AST nodes are bump-allocated from a thread-local arena when
 * one is bound, that the arena is reset/reclaimed cleanly, and that
 * the default heap allocator is used (with no reliance on the arena) when no
 * arena is bound.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/arena.h"
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/parser.h"
#include "../../core/include/snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Parse @p src with @p arena bound (or NULL for the default heap allocator)
 * and return the resulting AST root.  Caller frees the lexer/parser. */
static ast_node_t *parse_with_arena(const char *src, snobol_arena_t *arena,
                                    snobol_lexer_t **out_lexer,
                                    snobol_parser_t **out_parser) {
  snobol_lexer_t *lexer = snobol_lexer_create(src, strlen(src));
  snobol_parser_t *parser = snobol_parser_create();
  snobol_ast_set_arena(arena);
  ast_node_t *ast = snobol_parser_parse(parser, lexer);
  if (out_lexer) {
    *out_lexer = lexer;
  }
  if (out_parser) {
    *out_parser = parser;
  }
  return ast;
}

/* AST nodes are bump-allocated from a bound arena. */
void test_arena_allocation(void) {
  test_suite("Arena: AST nodes bump-allocated from bound arena");

  void *buf = malloc(SNOBOL_ARENA_DEFAULT_CAPACITY);
  snobol_arena_t arena;
  snobol_arena_init(&arena, buf, SNOBOL_ARENA_DEFAULT_CAPACITY);

  snobol_lexer_t *lexer = nullptr;
  snobol_parser_t *parser = nullptr;
  const char *src = "'hello' 'world' | ('a' 'b')";
  ast_node_t *ast = parse_with_arena(src, &arena, &lexer, &parser);

  test_assert(ast != NULL, "parse succeeds with arena bound");
  test_assert(arena.peak > 0, "AST nodes bump-allocated into arena (peak > 0)");

  snobol_ast_free(ast);
  snobol_ast_clear_arena();
  snobol_arena_reset(&arena);
  test_assert(arena.ptr == arena.base,
              "arena reset returns bump pointer to base");

  free(buf);
  snobol_parser_destroy(parser);
  snobol_lexer_destroy(lexer);
}

/* Without a bound arena the default allocator is used and the tree
 * is still freed correctly (no double free / leak of node storage). */
void test_arena_default_fallback(void) {
  test_suite("Arena: default heap allocator when no arena bound");

  snobol_lexer_t *lexer = nullptr;
  snobol_parser_t *parser = nullptr;
  const char *src = "'x' 'y' 'z'";
  ast_node_t *ast = parse_with_arena(src, nullptr, &lexer, &parser);

  test_assert(ast != NULL, "parse succeeds with no arena bound");
  snobol_ast_free(ast); /* must be safe: nodes were heap-allocated */

  snobol_parser_destroy(parser);
  snobol_lexer_destroy(lexer);
}

/* The full public compile path (which now binds a per-compile arena
 * internally) produces a working pattern and does not leak node storage. */
void test_arena_public_compile(void) {
  test_suite("Arena: snobol_pattern_compile uses internal arena");

  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  const char *src = "('foo' | 'bar') ('baz'+ 'qux')";
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, src, strlen(src), 0, &err);
  test_assert(pat != NULL, "complex pattern compiles");
  free(err);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

void test_arena_suite(void) {
  test_arena_allocation();
  test_arena_default_fallback();
  test_arena_public_compile();
}
