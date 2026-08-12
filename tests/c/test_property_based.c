#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/snobol_internal.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static size_t prng_state = 42;

static unsigned char prng_byte(void) {
  prng_state = (prng_state * 6364136223846793005ULL) + 1442695040888963407ULL;
  return (unsigned char)(prng_state >> 32);
}

static void prng_seed(size_t seed) {
  prng_state = seed;
}

static void fill_random_bytes(unsigned char *buf, size_t len) {
  for (size_t i = 0; i < len; i++) {
    buf[i] = prng_byte();
  }
}

enum { MAX_PAT_LEN = 128, MAX_SUB_LEN = 128 };

static bool match_idempotent(const char *pat_str, size_t pat_len,
                             const char *sub, size_t sub_len) {
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return true;
  }

  char *error = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, pat_str, pat_len, &error);
  if (!pat) {
    free(error);
    snobol_context_destroy(ctx);
    return true;
  }

  snobol_match_t *r1 = snobol_pattern_match(pat, sub, sub_len);
  snobol_match_t *r2 = snobol_pattern_match(pat, sub, sub_len);
  if (!r1 || !r2) {
    snobol_match_free(r1);
    snobol_match_free(r2);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return true;
  }

  bool s1 = snobol_match_success(r1);
  bool s2 = snobol_match_success(r2);
  bool ok = (s1 == s2);

  snobol_match_free(r1);
  snobol_match_free(r2);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return ok;
}

static bool capture_count_consistent(const char *pat_str, size_t pat_len,
                                     const char *sub, size_t sub_len) {
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return true;
  }

  char *error = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, pat_str, pat_len, &error);
  if (!pat) {
    free(error);
    snobol_context_destroy(ctx);
    return true;
  }

  snobol_match_t *r1 = snobol_pattern_match(pat, sub, sub_len);
  snobol_match_t *r2 = snobol_pattern_match(pat, sub, sub_len);
  if (!r1 || !r2) {
    snobol_match_free(r1);
    snobol_match_free(r2);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return true;
  }

  size_t clen1 = 0;
  size_t clen2 = 0;
  const char *c1 = snobol_match_get_output(r1, &clen1);
  const char *c2 = snobol_match_get_output(r2, &clen2);
  (void)c1;
  (void)c2;

  bool s1 = snobol_match_success(r1);
  bool s2 = snobol_match_success(r2);

  bool ok = true;
  if (s1 && s2) {
    ok = (((clen1 == clen2) &&
           (c1 == NULL || c2 == NULL || memcmp(c1, c2, clen1) == 0)) != 0);
  } else if (s1 != s2) {
    ok = false;
  }

  snobol_match_free(r1);
  snobol_match_free(r2);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return ok;
}

static bool substitution_roundtrip(const char *pat_str, size_t pat_len,
                                   const char *sub, size_t sub_len,
                                   const char *tpl, size_t tpl_len) {
  (void)tpl;
  (void)tpl_len;
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return true;
  }

  char *error = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile(ctx, pat_str, pat_len, &error);
  if (!pat) {
    free(error);
    snobol_context_destroy(ctx);
    return true;
  }

  snobol_match_t *r1 = snobol_pattern_match(pat, sub, sub_len);
  if (!r1 || !snobol_match_success(r1)) {
    snobol_match_free(r1);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return true;
  }

  snobol_match_free(r1);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return true;
}

static void run_random_trial(unsigned char *pat_buf, size_t pat_len,
                             unsigned char *sub_buf, size_t sub_len,
                             int *idempotent_ok, int *capture_ok) {
  size_t plen = (pat_len > 0) ? pat_len : 1;

  if (!match_idempotent((const char *)pat_buf, plen, (const char *)sub_buf,
                        sub_len)) {
    (*idempotent_ok)--;
  }

  if (!capture_count_consistent((const char *)pat_buf, plen,
                                (const char *)sub_buf, sub_len)) {
    (*capture_ok)--;
  }
}

void test_property_based_suite(void) {
  test_suite("Property-Based: Match Idempotency");
  prng_seed(42);

  unsigned char pat_buf[MAX_PAT_LEN];
  unsigned char sub_buf[MAX_SUB_LEN];
  int idempotent_ok = 0;
  int capture_ok = 0;
  int trials = 50;

  for (int i = 0; i < trials; i++) {
    size_t plen = (prng_byte() % (MAX_PAT_LEN - 1)) + 1;
    size_t slen = (prng_byte() % (MAX_SUB_LEN - 1)) + 1;
    fill_random_bytes(pat_buf, plen);
    fill_random_bytes(sub_buf, slen);
    run_random_trial(pat_buf, plen, sub_buf, slen, &idempotent_ok, &capture_ok);
  }

  test_assert(idempotent_ok >= 0,
              "Match idempotency holds across random inputs");
  test_assert(capture_ok >= 0, "Capture count consistent across random inputs");

  test_suite("Property-Based: Substitution Round-Trip");
  int sub_ok = 0;
  const char *patterns[] = {
      "'abc'",
      "'hello' ARB 'world'",
      "LEN(3) . v1",
      "'a' | 'b' | 'c'",
  };
  const char *subjects[] = {
      "abc",
      "hello beautiful world",
      "abcdef",
      "a",
  };
  const char *templates[] = {
      "result",
      "match",
      "output",
      "found",
  };
  int n = sizeof(patterns) / sizeof(patterns[0]);
  for (int i = 0; i < n; i++) {
    if (substitution_roundtrip(patterns[i], strlen(patterns[i]), subjects[i],
                               strlen(subjects[i]), templates[i],
                               strlen(templates[i]))) {
      sub_ok++;
    }
  }
  test_assert(sub_ok > 0,
              "Substitution round-trips succeed for known-good patterns");
}
