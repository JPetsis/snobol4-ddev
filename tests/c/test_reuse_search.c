/**
 * test_reuse_search.c — Differential tests for the reusable search API
 *
 * Asserts snobol_pattern_search_ex() results (position / length / captures)
 * equal snobol_pattern_search() across capture, alt, span, and prefix
 * patterns, and that N iterations terminate (no hang / pathological slowdown).
 *
 * The reuse path historically hung on automaton-eligible patterns because the
 * cached DFA was never supplied (dfa=NULL). This suite pins the fix.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Compare a single search result (position/length/var count/vars) against a
 * reference match object. */
static void assert_same_match(snobol_match_t *got, snobol_match_t *ref,
                              const char *label) {
  char msg[128];
  snprintf(msg, sizeof(msg), "%s: success flag matches", label);
  test_assert(snobol_match_success(got) == snobol_match_success(ref), msg);
  if (!snobol_match_success(got)) {
    return;
  }

  snprintf(msg, sizeof(msg), "%s: position matches", label);
  test_assert(snobol_match_get_position(got) == snobol_match_get_position(ref),
              msg);
  snprintf(msg, sizeof(msg), "%s: length matches", label);
  test_assert(snobol_match_get_length(got) == snobol_match_get_length(ref),
              msg);
  snprintf(msg, sizeof(msg), "%s: var count matches", label);
  test_assert(got->var_count == ref->var_count, msg);

  for (int i = 0; i < got->var_count; i++) {
    size_t gl = 0;
    size_t rl = 0;
    char name[8];
    /* Capture registers are 0-based; the accessor maps the bare number to
     * the register index directly. */
    snprintf(name, sizeof(name), "%d", i);
    const char *gv = snobol_match_get_variable(got, name, &gl);
    const char *rv = snobol_match_get_variable(ref, name, &rl);
    snprintf(msg, sizeof(msg), "%s: var %d length matches", label, i);
    test_assert(gl == rl, msg);
    snprintf(msg, sizeof(msg), "%s: var %d bytes match", label, i);
    if (gl == rl && gl > 0 && gv && rv) {
      test_assert(memcmp(gv, rv, gl) == 0, msg);
    }
  }
}

static void diff_patterns(const char *label, const char *pattern, size_t plen,
                          const char *subject, size_t slen) {
  (void)plen;
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, pattern, strlen(pattern), &err);
  char cmsg[256];
  snprintf(cmsg, sizeof(cmsg), "compile succeeds [%s]%s%s", label,
           err ? ": " : "", err ? err : "");
  test_assert(pat != NULL, cmsg);
  if (!pat) {
    free(err);
    snobol_context_destroy(ctx);
    return;
  }

  snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
      snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));
  snobol_pattern_search_state_set_pattern(state, pat);
  test_assert(state != NULL, "state create succeeds");

  /* Reference: non-stateful search */
  snobol_match_t *ref = snobol_pattern_search(pat, subject, slen);

  /* Reuse search across N iterations — must terminate and match each time. */
  const int N = 2000;
  for (int i = 0; i < N; i++) {
    snobol_match_t *m = snobol_pattern_search_ex(state, subject, slen, 0);
    test_assert(m != NULL, "reuse search returns non-NULL");
    if (m) {
      char itlabel[160];
      snprintf(itlabel, sizeof(itlabel), "%s iter %d", label, i);
      assert_same_match(m, ref, itlabel);
    }
  }

  if (ref) {
    snobol_match_free(ref);
  }
  snobol_pattern_search_state_destroy(state);
  snobol_pattern_free(pat);
  free(err);
  snobol_context_destroy(ctx);
}

void test_reuse_search_suite(void) {
  test_suite("Reuse Search: _ex parity with search()");

  /* Capture pattern: @r(SPAN('0-9')) */
  diff_patterns("capture", "@r(SPAN('0-9'))", 15, "id:12345,name:foo", 17);

  /* Alternation pattern */
  diff_patterns("alt", "'cat' | 'dog' | 'fox'", 20, "the fox jumps", 13);

  /* SPAN pattern */
  diff_patterns("span", "SPAN(',')", 9, "a,b,c,d,e", 9);

  /* Prefix pattern: literal prefix + SPAN */
  diff_patterns("prefix", "'id:' SPAN('0-9')", 18, "id:42 and id:7", 13);

  /* Automaton-eligible: alt-literal prefix + SPAN (the old hang case) */
  diff_patterns("alt_prefix", "('foo'|'fop') SPAN('z')", 22, "fopzzzfoo", 8);

  /* No-match subject must also agree */
  diff_patterns("nomatch", "@r(SPAN('0-9'))", 15, "no digits here", 14);

  /* Buffer hoist regression: 1000 searches on same state must all succeed */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *p =
        snobol_pattern_compile(ctx, "'hello' SPAN(' ')", 17, &err);
    if (p) {
      snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
          snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p));
      if (state) {
        bool all_ok = true;
        for (int i = 0; i < 1000; i++) {
          snobol_match_t *m =
              snobol_pattern_search_ex(state, "hello world", 11, 0);
          if (!m || !m->success) {
            all_ok = false;
          }
        }
        test_assert(all_ok, "buffer hoist: 1000 searches succeed");
        snobol_pattern_search_state_destroy(state);
      }
      snobol_pattern_free(p);
    }
    snobol_context_destroy(ctx);
  }
}
