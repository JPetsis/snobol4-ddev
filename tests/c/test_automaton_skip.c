/**
 * test_automaton_skip.c — P5: DFA BMH-skip for alt-literal prefixes
 *
 * The search metadata derives a Boyer–Moore–Horspool skip table from the
 * shared literal prefix of an alternation-of-literals pattern (search_meta.c).
 * The automaton / alt-literal trial loop advances by more than one byte on a
 * failing position, eliminating the O(n^2) scan for patterns like
 * ('foo' | 'fop') over a long subject that never matches.
 *
 * Verifies:
 *   - matches are identical to the un-skipped expectation
 *   - a long failing subject completes in linear (bounded) time
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static int64_t now_ns(void) {
#ifdef _WIN32
  LARGE_INTEGER freq, cnt;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&cnt);
  return (int64_t)(cnt.QuadPart * 1000000000LL / freq.QuadPart);
#elif defined(__APPLE__)
  static mach_timebase_info_data_t info = {0};
  if (info.denom == 0) {
    mach_timebase_info(&info);
  }
  uint64_t raw = mach_absolute_time();
  return (int64_t)(raw * info.numer / info.denom);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000L + (int64_t)ts.tv_nsec;
#endif
}

static snobol_match_t *search(const char *pat, const char *subject) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, pat, strlen(pat), &err);
  free(err);
  snobol_match_t *m = nullptr;
  if (p) {
    m = snobol_pattern_search(p, subject, strlen(subject));
    snobol_pattern_free(p);
  }
  snobol_context_destroy(ctx);
  return m;
}

/* A subject of N 'a' bytes (never contains the pattern's first byte). */
static char *make_long(size_t n) {
  char *s = (char *)malloc(n + 1);
  if (!s) {
    return nullptr;
  }
  memset(s, 'a', n);
  s[n] = '\0';
  return s;
}

static void test_correctness(void) {
  test_suite("Automaton BMH: correctness vs expectation");

  /* Bushy alt-literal with shared prefix "fo". */
  snobol_match_t *m = search("('foo' | 'fop' | 'fox')", "xxfopxx");
  test_assert((m != NULL && snobol_match_success(m)) != 0, "match succeeds");
  if (m) {
    test_assert(snobol_match_get_position(m) == 2, "match at offset 2 ('fop')");
    test_assert(snobol_match_get_length(m) == 3, "match length 3");
    snobol_match_free(m);
  }

  /* First alternative wins when both could match. */
  snobol_match_t *m2 = search("('foo' | 'fop')", "zzfoo");
  test_assert((m2 != NULL && snobol_match_success(m2)) != 0, "match succeeds");
  if (m2) {
    test_assert(snobol_match_get_position(m2) == 2,
                "match at offset 2 ('foo')");
    snobol_match_free(m2);
  }

  /* No match when subject has no 'f'. */
  snobol_match_t *m3 = search("('foo' | 'fop')", "barbar");
  test_assert((m3 != NULL && !snobol_match_success(m3)) != 0,
              "no match on 'barbar'");
  snobol_match_free(m3);
}

static void test_linear_time(void) {
  test_suite("Automaton BMH: linear time on long failing subject");

  /* 2,000,000 'a' bytes — would be O(n^2) (~4e12 ops) without the
   * skip table, i.e. effectively a hang. With BMH skip it is O(n). */
  size_t n = 2000000;
  char *subj = make_long(n);
  test_assert(subj != NULL, "long subject allocated");
  if (!subj) {
    return;
  }

  int64_t t0 = now_ns();
  snobol_match_t *m = search("('foo' | 'fop' | 'fox')", subj);
  int64_t t1 = now_ns();

  double ms = (double)(t1 - t0) / 1.0e6;
  test_assert((m != NULL && !snobol_match_success(m)) != 0,
              "no match on long 'a' run");
  /* Linear scan of 2 MB should finish in well under a second. */
  test_assert(ms < 1000.0, "long failing subject scanned in < 1s (linear)");

  snobol_match_free(m);
  free(subj);
}

void test_automaton_skip_suite(void) {
  test_suite("Automaton: BMH-skip for alt-literal prefixes");
  test_correctness();
  test_linear_time();
}
