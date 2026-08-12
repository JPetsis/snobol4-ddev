/**
 * test_capture_registers.c
 *
 * Tests for register-style captures (P3): captures are stored as
 * (subject, offset, length) registers in snobol_match_t and materialized
 * lazily into owned byte copies only when snobol_match_get_variable() is
 * called.  These tests exercise the lazy-materialization contract directly
 * (the engine populates the registers; the accessor does the on-demand copy)
 * and verify:
 *   - an unmaterialized register reads back the correct source span
 *   - unread registers stay NULL (zero per-match allocation)
 *   - reading materializes exactly one owned copy
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Populate a match's register `i` (1-based, matching the engine's
 * variable numbering) with (subject, off, len) and leave the
 * materialized copy NULL, mirroring what the search API does per match. */
static void set_register(snobol_match_t *m, int i, const char *subject,
                         size_t off, size_t len) {
  m->var_subject = subject;
  m->var_off[i] = off;
  m->var_len[i] = len;
  m->var_values[i] = NULL;
  m->var_lens[i] = 0;
}

/* ── Lazy materialization produces correct bytes ────────────────────── */
static void test_lazy_correct(void) {
  test_suite("Captures: lazy materialization returns correct span");

  snobol_match_t *m = snobol_match_create();
  test_assert(m != NULL, "match allocated");
  if (!m)
    return;
  m->success = true;
  m->var_count = 3;

  const char *subject = "hello world";
  set_register(m, 1, subject, 0, 5); /* "hello" -> variable "1" */
  set_register(m, 2, subject, 6, 5); /* "world" -> variable "2" */

  /* Both unmaterialized before any read. */
  test_assert(m->var_values[1] == NULL && m->var_values[2] == NULL,
              "registers unmaterialized before read");

  size_t len = 0;
  const char *v1 = snobol_match_get_variable(m, "1", &len);
  test_assert(v1 != NULL && len == 5 && memcmp(v1, "hello", 5) == 0,
              "variable '1' materializes to 'hello'");
  test_assert(m->var_values[1] != NULL, "register 1 now materialized");

  const char *v2 = snobol_match_get_variable(m, "2", &len);
  test_assert(v2 != NULL && len == 5 && memcmp(v2, "world", 5) == 0,
              "variable '2' materializes to 'world'");

  /* Re-read returns the same cached owned copy. */
  const char *v1b = snobol_match_get_variable(m, "1", &len);
  test_assert(v1b == v1, "re-read returns cached copy (no re-alloc)");

  snobol_match_free(m);
}

/* ── Unread register stays NULL (zero per-match alloc) ───────────── */
static void test_unread_untouched(void) {
  test_suite("Captures: unread register stays unmaterialized");

  snobol_match_t *m = snobol_match_create();
  test_assert(m != NULL, "match allocated");
  if (!m)
    return;
  m->success = true;
  m->var_count = 3;

  const char *subject = "abc123";
  set_register(m, 1, subject, 0, 3); /* "abc"  -> variable "1" */
  set_register(m, 2, subject, 3, 3); /* "123"  -> variable "2" */

  /* Read only register 1. */
  size_t len = 0;
  const char *v1 = snobol_match_get_variable(m, "1", &len);
  test_assert(v1 != NULL && len == 3 && memcmp(v1, "abc", 3) == 0,
              "register 1 reads correctly");

  test_assert(m->var_values[1] != NULL, "register 1 materialized");
  test_assert(m->var_values[2] == NULL,
              "register 2 still unmaterialized after sibling read");

  snobol_match_free(m);
}

/* ── Empty (zero-width) register materializes to empty string ─────── */
static void test_empty(void) {
  test_suite("Captures: zero-width register materializes to empty");

  snobol_match_t *m = snobol_match_create();
  test_assert(m != NULL, "match allocated");
  if (!m)
    return;
  m->success = true;
  m->var_count = 2;

  const char *subject = "xyz";
  set_register(m, 1, subject, 1, 0); /* empty span at offset 1 */

  size_t len = 0;
  const char *v = snobol_match_get_variable(m, "1", &len);
  test_assert(v != NULL && len == 0 && v[0] == '\0',
              "zero-width register reads as empty string");

  snobol_match_free(m);
}

/* ── Absent variable returns NULL without allocating ──────────────── */
static void test_absent(void) {
  test_suite("Captures: absent variable returns NULL");

  snobol_match_t *m = snobol_match_create();
  test_assert(m != NULL, "match allocated");
  if (!m)
    return;
  m->success = true;
  m->var_count = 0;

  size_t len = 0;
  const char *v = snobol_match_get_variable(m, "1", &len);
  test_assert(v == NULL && len == 0, "absent variable is NULL");

  snobol_match_free(m);
}

/* ── Engine-driven capture: @r1 through real match path ──────────── */
static void test_engine_capture(void) {
  test_suite("Captures: @r1 resolves via engine registers");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context allocated");
  if (!ctx)
    return;

  char *err = NULL;
  /* "@r1" captures whatever the pattern matched (here "world"). */
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, "'hello ' @r1 'world'", 20, &err);
  test_assert(pat != NULL && err == NULL, "pattern compiled");
  if (!pat) {
    snobol_context_destroy(ctx);
    return;
  }

  snobol_match_t *m = snobol_pattern_match(pat, "hello world", 11);
  test_assert(m != NULL && m->success, "match succeeded");
  if (!m || !m->success) {
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return;
  }

  size_t len = 0;
  /* @r1 allocates the first register (0) under sequential allocation. */
  const char *v = snobol_match_get_variable(m, "0", &len);
  test_assert(v != NULL && len == 5 && memcmp(v, "world", 5) == 0,
              "variable '0' reads engine capture 'world'");

  snobol_match_free(m);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

void test_capture_registers_suite(void) {
  test_suite("Captures: register-style lazy materialization");
  test_lazy_correct();
  test_unread_untouched();
  test_empty();
  test_absent();
  test_engine_capture();
}
