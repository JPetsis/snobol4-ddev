#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snobol/search.h"
#include "snobol/vm.h"
#include "snobol/snobol.h"

void test_suite(const char *name);
void test_assert(bool condition, const char *message);

static void test_prefilter_miss(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  /* ('a'+)+ 'b' — required lit is 'b' */
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "('a'+)+ 'b'", 12, &err);
  if (!p) {
    test_assert(false, "prefilter miss: compile");
    snobol_context_destroy(ctx);
    return;
  }
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
  vm.bc_len = snobol_pattern_get_bc_len(p);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  fprintf(stderr,
          "  prefilter miss: has_required_lit=%d required_lit_len=%zu "
          "has_literal_prefix=%d tier=%d\n",
          meta->has_required_lit, meta->required_lit_len,
          meta->has_literal_prefix, meta->tier);
  snobol_search_result_t r;
  bool ok = snobol_search_exec(&vm, "aaaaaaaaaa", 10, 0, meta, NULL, &r, NULL);
  test_assert(!ok, "prefilter miss: no match on a-only");
  test_assert(r.prefilter_skip, "prefilter miss: prefilter_skip set");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

static void test_prefilter_hit(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "('a'+)+ 'b'", 12, &err);
  if (!p) {
    test_assert(false, "prefilter hit: compile");
    snobol_context_destroy(ctx);
    return;
  }
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
  vm.bc_len = snobol_pattern_get_bc_len(p);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  snobol_search_result_t r;
  bool ok = snobol_search_exec(&vm, "aaaaabaaaa", 10, 0, meta, NULL, &r, NULL);
  test_assert(ok, "prefilter hit: match found");
  test_assert(!r.prefilter_skip, "prefilter hit: prefilter_skip not set");
  test_assert(r.match_start <= 5 && r.match_end > 5,
              "prefilter hit: match contains 'b'");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

static void test_prefilter_noop(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  /* 'a' | 'b' — no single required lit (SPLIT in bytecode prevents it) */
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "'a' | 'b'", 9, &err);
  if (!p) {
    test_assert(false, "prefilter noop: compile");
    snobol_context_destroy(ctx);
    return;
  }
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
  vm.bc_len = snobol_pattern_get_bc_len(p);
  snobol_search_result_t r;
  bool ok = snobol_search_exec(&vm, "c", 1, 0, meta, NULL, &r, NULL);
  test_assert(!ok, "prefilter noop: no match");
  /* prefilter_skip should be false — the pre-filter was skipped */
  test_assert(!r.prefilter_skip, "prefilter noop: no prefilter_skip");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

static void test_prefilter_leading_alternation(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  /* A SPLIT encountered before any literal (loop body or leading
   * alternation) makes every later literal optional — no required literal
   * may be derived, and subjects without any branch literal must still
   * match (zero-length).  Regression: ('a'|'b')* derived required='b' and
   * wrongly rejected "xyz" in the prefilter. */
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "('a'|'b')*", 10, &err);
  if (!p) {
    test_assert(false, "prefilter leading alt: compile");
    snobol_context_destroy(ctx);
    return;
  }
  free(err);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  test_assert(!meta->has_required_lit,
              "prefilter leading alt: no required literal derived");
  snobol_match_t *m = snobol_pattern_search(p, "xyz", 3);
  test_assert(m && snobol_match_success(m) && snobol_match_get_length(m) == 0,
              "prefilter leading alt: zero-length match on 'xyz'");
  snobol_match_free(m);
  m = snobol_pattern_search(p, "ab", 2);
  test_assert(m && snobol_match_success(m) && snobol_match_get_length(m) == 2,
              "prefilter leading alt: 'ab' fully consumed");
  snobol_match_free(m);
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}

/* Full VM with the subject preloaded — the reference for anchored agreement. */
static VM make_vm(const uint8_t *bc, size_t bc_len, const char *subject) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = strlen(subject);
  vm.ip = 0;
  vm.pos = 0;
  vm.var_count = 0;
  return vm;
}

/* The anchored entry (snobol_search_exec_anchored) must run the same
 * required-byte prefilter as the unanchored one: a subject lacking the
 * required literal fails with prefilter_skip = true before any tier runs. */
static void test_anchored_prefilter_miss(void) {
  /* The last case uses a multi-byte required literal ("pqr") to exercise
   * the memmem prefilter path on the anchored entry. */
  const char *patterns[] = {"('a'*) 'b'", "('a'+)+ 'b'", "@r('a'*) 'b'",
                            "('a'+) 'pqr'"};
  const char *subject = "aaaaaaaaaa";
  for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++) {
    snobol_context_t *ctx = snobol_context_create();
    char *err = NULL;
    snobol_pattern_t *p = snobol_pattern_compile(ctx, patterns[i],
                                                 strlen(patterns[i]), &err);
    if (!p) {
      test_assert(false, "anchored prefilter miss: compile");
      snobol_context_destroy(ctx);
      return;
    }
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = (uint8_t *)snobol_pattern_get_bc(p);
    vm.bc_len = snobol_pattern_get_bc_len(p);
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
    snobol_search_result_t r;
    bool ok = snobol_search_exec_anchored(&vm, subject, strlen(subject), meta,
                                          NULL, &r, NULL);
    test_assert(!ok, "anchored prefilter miss: no match on a-only subject");
    test_assert(r.prefilter_skip,
                "anchored prefilter miss: prefilter_skip set");
    snobol_pattern_free(p);
    snobol_context_destroy(ctx);
    snobol_search_vm_cleanup(&vm);
  }
}

/* The prefilter is a necessary-condition check: when the required literal is
 * present but the pattern still fails at the anchor, the prefilter must NOT
 * short-circuit and the failure must agree with the full VM. */
static void test_anchored_prefilter_no_shortcircuit(void) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = NULL;
  /* ('a'+) needs an 'a' at the anchor; "bbbbbb" has the required 'b' but no
   * anchored match. */
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "('a'+) 'b'", 10, &err);
  if (!p) {
    test_assert(false, "anchored prefilter no-shortcircuit: compile");
    snobol_context_destroy(ctx);
    return;
  }
  const uint8_t *bc = snobol_pattern_get_bc(p);
  size_t bc_len = snobol_pattern_get_bc_len(p);
  const snobol_search_meta_t *meta = snobol_pattern_get_meta(p);
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = (uint8_t *)bc;
  vm.bc_len = bc_len;
  snobol_search_result_t r;
  bool ok = snobol_search_exec_anchored(&vm, "bbbbbb", 6, meta, NULL, &r, NULL);
  test_assert(!ok, "anchored prefilter no-shortcircuit: no anchored match");
  test_assert(!r.prefilter_skip,
              "anchored prefilter no-shortcircuit: prefilter did not reject");
  VM fvm = make_vm(bc, bc_len, "bbbbbb");
  test_assert(ok == vm_run(&fvm),
              "anchored prefilter no-shortcircuit: agrees with full VM");
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
  snobol_search_vm_cleanup(&vm);
}

void test_search_prefilter_suite(void) {
  test_suite("Search: Required-Byte Prefilter");
  test_prefilter_miss();
  test_prefilter_hit();
  test_prefilter_noop();
  test_prefilter_leading_alternation();
  test_anchored_prefilter_miss();
  test_anchored_prefilter_no_shortcircuit();
}
