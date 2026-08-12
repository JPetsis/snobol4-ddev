/**
 * test_break_grammar.c — Grammar wiring for BREAK / BREAKX
 *
 * Verifies that:
 *   - BREAK(set) and BREAKX(set) parse and compile (previously the parser
 *     never produced AST_BREAK, leaving TIER_BREAK_SCAN unreachable).
 *   - An ASCII-class BREAK/BREAKX pattern's structural tier is TIER_BREAK_SCAN.
 *   - match()/search() return correct delimited-field results.
 *   - BREAK() with no argument is rejected (mirrors SPAN).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/search.h"
#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_break_grammar_suite(void) {
  test_suite("Grammar: BREAK / BREAKX");

  /* BREAK(',') compiles and routes to TIER_BREAK_SCAN */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "BREAK(',')", 10, &err);
    test_assert(pat != NULL, "BREAK(',') compiles");
    if (pat) {
      const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
      test_assert(meta != NULL, "meta available");
      if (meta) {
        test_assert(meta->tier == TIER_BREAK_SCAN,
                    "BREAK(',') structural tier is TIER_BREAK_SCAN");
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* BREAKX(',') compiles and routes to TIER_BREAK_SCAN */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "BREAKX(',')", 11, &err);
    test_assert(pat != NULL, "BREAKX(',') compiles");
    if (pat) {
      const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
      test_assert(meta != NULL, "meta available");
      if (meta) {
        test_assert(meta->tier == TIER_BREAK_SCAN,
                    "BREAKX(',') structural tier is TIER_BREAK_SCAN");
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* BREAK matches up to the delimiter (anchored): 'field1' from 'field1,field2' */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "BREAK(',')", 10, &err);
    test_assert(pat != NULL, "compile BREAK for match");
    if (pat) {
      const char *subject = "field1,field2";
      size_t slen = strlen(subject);
      snobol_match_t *m = snobol_pattern_match(pat, subject, slen);
      test_assert((m != NULL && snobol_match_success(m)) != 0,
                  "BREAK match succeeds on 'field1,field2'");
      if (snobol_match_success(m)) {
        test_assert(snobol_match_get_position(m) == 0,
                    "BREAK match starts at 0");
        test_assert(snobol_match_get_length(m) == 6,
                    "BREAK matches 'field1' (up to delimiter)");
      }
      snobol_match_free(m);
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* BREAK search finds the leading field token */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "BREAK(',')", 10, &err);
    test_assert(pat != NULL, "compile BREAK for search");
    if (pat) {
      const char *subject = "field1,field2";
      size_t slen = strlen(subject);
      snobol_match_t *m = snobol_pattern_search(pat, subject, slen);
      test_assert((m != NULL && snobol_match_success(m)) != 0,
                  "BREAK search succeeds");
      if (snobol_match_success(m)) {
        test_assert(snobol_match_get_position(m) == 0,
                    "BREAK search starts at 0");
        test_assert(snobol_match_get_length(m) == 6,
                    "BREAK search matches 'field1'");
      }
      snobol_match_free(m);
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* BREAK() with no argument is rejected (mirrors SPAN) */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "BREAK()", 7, &err);
    test_assert(pat == NULL, "BREAK() with no argument is rejected");
    free(err);
    snobol_context_destroy(ctx);
  }
}
