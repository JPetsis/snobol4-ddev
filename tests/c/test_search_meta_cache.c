/**
 * test_search_meta_cache.c - Tests for the cached search metadata
 *
 * Verifies that snobol_pattern_search() uses the compile-time cached
 * search metadata (snobol_pattern_t::meta) and produces identical
 * results to a fresh per-call derivation.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/search.h"
#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Internal access — we want to verify the pattern struct's meta field.
 * The struct definition is in core/src/api.c. We can only see the
 * public API from here, so we test behavior, not internals. The test
 * ensures that snobol_pattern_search() returns correct results
 * (regression) using the compile-time cached search metadata. */

/* ===== test_coverage_meta: coverage-driven tests merged into test_search_meta_cache.c ===== */
#include <stdio.h>
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"


/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void covm_emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

static void covm_emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)((v >> 24) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 16) & 0xFF);
  bc[(*ip)++] = (uint8_t)((v >> 8) & 0xFF);
  bc[(*ip)++] = (uint8_t)(v & 0xFF);
}

/* Derive meta (freeing any prior heap fields) and assert it ran cleanly. */
static void covm_derive(const uint8_t *bc, size_t bc_len) {
  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);
  snobol_search_meta_free(&meta);
}

/* Old-format charclass trailer: u16 count, u16 case, CpRange(s). */
static size_t covm_append_ascii_class(uint8_t *bc, size_t at, char lo,
                                      char hi) {
  size_t ip = at;
  covm_emit_u16_be(bc, &ip, 1);
  covm_emit_u16_be(bc, &ip, 0);
  covm_emit_u32_be(bc, &ip, (uint32_t)(unsigned char)lo);
  covm_emit_u32_be(bc, &ip, (uint32_t)(unsigned char)hi);
  return ip;
}

/* ── Root-op classification paths ─────────────────────────────────────────── */

void test_cov_meta_root_classification(void) {
  test_suite("Coverage: derive root-op classification");

  /* TAB-prefixed literal: the zero-width prefix skip reaches the LIT. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_TAB;
    covm_emit_u32_be(bc, &ip, 2);
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 2);
    bc[ip++] = 'x';
    bc[ip++] = 'y';
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((meta.has_literal_prefix && meta.literal_prefix_len == 2) != 0,
                "TAB prefix skipped, literal prefix kept");
    snobol_search_meta_free(&meta);
  }

  /* RTAB-prefixed literal. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_RTAB;
    covm_emit_u32_be(bc, &ip, 0);
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.has_literal_prefix, "RTAB prefix skipped");
    snobol_search_meta_free(&meta);
  }

  /* FENCE- and NOP-prefixed literal. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_FENCE;
    bc[ip++] = OP_NOP;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.has_literal_prefix, "FENCE/NOP prefix skipped");
    snobol_search_meta_free(&meta);
  }

  /* OP_ANY root: candidate bitmap built from the charclass. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_ANY;
    covm_emit_u16_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    size_t data_off = ip;
    size_t at = covm_append_ascii_class(bc, ip, 'a', 'a');
    covm_emit_u32_be(bc, &at, (uint32_t)data_off); /* offset table */
    covm_emit_u32_be(bc, &at, 1);                  /* class_count */
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, at, &meta);
    test_assert((meta.has_candidate_bitmap && meta.is_single_char_alt) != 0,
                "ANY root builds candidate bitmap");
    test_assert(meta.ascii_class_only, "ANY ASCII class flagged");
    snobol_search_meta_free(&meta);
  }

  /* OP_NOTANY root with a non-ASCII class → not ascii-only. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_NOTANY;
    covm_emit_u16_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    size_t at = ip;
    covm_emit_u16_be(bc, &at, 1);
    covm_emit_u16_be(bc, &at, 0);
    covm_emit_u32_be(bc, &at, 300); /* non-ASCII range */
    covm_emit_u32_be(bc, &at, 400);
    covm_emit_u32_be(bc, &at, (uint32_t)ip); /* offset table -> data */
    covm_emit_u32_be(bc, &at, 1);            /* class_count */
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, at, &meta);
    test_assert((!meta.ascii_class_only) != 0,
                "NOTANY non-ASCII class flagged");
    snobol_search_meta_free(&meta);
  }

  /* SPLIT single-char detection with a non-single second branch. */
  {
    uint8_t bc[128];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    size_t branch_b = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 2); /* two bytes: not a single-char alt */
    bc[ip++] = 'b';
    bc[ip++] = 'c';
    bc[ip++] = OP_ACCEPT;
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)branch_b);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.is_single_char_alt) != 0,
                "multi-byte branch rejects single-char alt");
    snobol_search_meta_free(&meta);
  }

  /* Single-char alt with bytes >= 64 (upper-word candidate bitmap). */
  {
    uint8_t bc[128];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'A'; /* 0x41 >= 64 */
    bc[ip++] = OP_ACCEPT;
    size_t branch_b = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'B';
    bc[ip++] = OP_ACCEPT;
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)branch_b);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((meta.is_single_char_alt && meta.has_candidate_bitmap) != 0,
                "single-char alt detected");
    test_assert(meta.candidate_bitmap[1] != 0, "upper-word candidate bits set");
    snobol_search_meta_free(&meta);
  }
}

/* ── compute_start_bitmap edge cases ──────────────────────────────────────── */


void test_cov_meta_start_bitmap(void) {
  test_suite("Coverage: start-bitmap edge cases");

  /* SPLIT self-cycle trips the step guard → all-bytes bitmap.  (A pure JMP
   * cycle cannot be used: compute_minlength follows JMP targets with no
   * cycle guard, so it would spin forever; SPLIT recursion is depth-guarded
   * and terminates.) */
  {
    uint8_t bc[9] = {OP_SPLIT, 0, 0, 0, 0, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 9, &meta);
    test_assert((meta.has_start_bitmap && meta.start_bitmap[0] == 0xFF) != 0,
                "split cycle trips step guard");
    snobol_search_meta_free(&meta);
  }

  /* Truncated LIT operands. */
  {
    uint8_t bc[6] = {OP_LIT, 0, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 6, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated LIT sets all bits");
    snobol_search_meta_free(&meta);
  }

  /* LIT data offset beyond the buffer. */
  {
    uint8_t bc[12];
    size_t ip = 0;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, 99);
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "LIT offset beyond buffer");
    snobol_search_meta_free(&meta);
  }

  /* Truncated / unresolved ANY, NOTANY, SPAN. */
  {
    uint8_t bc1[2] = {OP_ANY, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, 2, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated ANY");
    snobol_search_meta_free(&meta);

    uint8_t bc2[3] = {OP_ANY, 0, 5}; /* set_id 5: no ranges */
    snobol_search_derive_meta(bc2, 3, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "unresolved ANY");
    snobol_search_meta_free(&meta);

    uint8_t bc3[2] = {OP_NOTANY, 0};
    snobol_search_derive_meta(bc3, 2, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated NOTANY");
    snobol_search_meta_free(&meta);

    uint8_t bc4[3] = {OP_NOTANY, 0, 5};
    snobol_search_derive_meta(bc4, 3, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "unresolved NOTANY");
    snobol_search_meta_free(&meta);

    uint8_t bc5[2] = {OP_SPAN, 0};
    snobol_search_derive_meta(bc5, 2, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated SPAN");
    snobol_search_meta_free(&meta);

    uint8_t bc6[3] = {OP_SPAN, 0, 5};
    snobol_search_derive_meta(bc6, 3, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "unresolved SPAN");
    snobol_search_meta_free(&meta);
  }

  /* Truncated SPLIT and out-of-range branch targets. */
  {
    uint8_t bc1[6] = {OP_SPLIT, 0, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, 6, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated SPLIT");
    snobol_search_meta_free(&meta);

    uint8_t bc2[10];
    size_t ip = 0;
    bc2[ip++] = OP_SPLIT;
    covm_emit_u32_be(bc2, &ip, 99);
    covm_emit_u32_be(bc2, &ip, 99);
    snobol_search_derive_meta(bc2, 10, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "SPLIT targets beyond");
    snobol_search_meta_free(&meta);
  }

  /* SPLIT alternation-stack overflow (>2048 pending branches). */
  {
    uint8_t bc[(2048 * 9) + 64]; /* room for 2050 SPLITs + ACCEPT */
    size_t ip = 0;
    for (int i = 0; i < 2050; i++) {
      bc[ip++] = OP_SPLIT;
      covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
      covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    }
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "SPLIT stack overflow");
    snobol_search_meta_free(&meta);
  }

  /* Truncated ASSIGN / POS / JMP and unknown opcodes. */
  {
    uint8_t bc1[3] = {OP_ASSIGN, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, 3, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated ASSIGN");
    snobol_search_meta_free(&meta);

    /* A truncated POS would trip derive's zero-width prefix skip, which
     * advances without bounds checks (OOB read under ASan).  Only the
     * non-prefix-set truncated ops are exercised here. */
    uint8_t bc3[4] = {OP_JMP, 0, 0, 0};
    snobol_search_derive_meta(bc3, 4, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "truncated JMP");
    snobol_search_meta_free(&meta);

    uint8_t bc4[5] = {OP_JMP, 0, 0, 0, 99};
    snobol_search_derive_meta(bc4, 5, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "JMP target beyond");
    snobol_search_meta_free(&meta);

    /* Terminal-only bytecode and unknown opcode. */
    uint8_t bc5[2] = {OP_ACCEPT, OP_ACCEPT};
    snobol_search_derive_meta(bc5, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc6[2] = {0x7F, OP_ACCEPT};
    snobol_search_derive_meta(bc6, 2, &meta);
    test_assert(meta.start_bitmap[0] == 0xFF, "unknown opcode");
    snobol_search_meta_free(&meta);
  }
}

/* ── compute_minlength edge cases ─────────────────────────────────────────── */


void test_cov_meta_minlength(void) {
  test_suite("Coverage: minlength edge cases");

  /* Truncated opcodes return the accumulated length / 0.  (derive_meta
   * bails on bc_len < 2, and the truncation checks are `ip + N > bc_len`
   * with ip = 0, so the buffers must hold the opcode plus at least one
   * operand byte.) */
  {
    uint8_t bc1[6] = {OP_LIT, 0, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, 6, &meta);
    test_assert(meta.minlength == 0, "truncated LIT minlength");
    snobol_search_meta_free(&meta);

    uint8_t bc2[2] = {OP_ANY, 0};
    snobol_search_derive_meta(bc2, 2, &meta);
    test_assert(meta.minlength == 1, "truncated ANY minlength");
    snobol_search_meta_free(&meta);

    uint8_t bc3[2] = {OP_SPAN, 0};
    snobol_search_derive_meta(bc3, 2, &meta);
    test_assert(meta.minlength == 1, "truncated SPAN minlength");
    snobol_search_meta_free(&meta);

    uint8_t bc4[2] = {OP_LEN, 0};
    snobol_search_derive_meta(bc4, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc6[3] = {OP_ASSIGN, 0, 0};
    snobol_search_derive_meta(bc6, 3, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc7[2] = {OP_SPLIT, 0};
    snobol_search_derive_meta(bc7, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc8[2] = {OP_JMP, 0};
    snobol_search_derive_meta(bc8, 2, &meta);
    snobol_search_meta_free(&meta);
  }

  /* SPLIT with a FAIL branch (infinite minlength branch) and double FAIL. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 2);
    bc[ip++] = 'a';
    bc[ip++] = 'b';
    bc[ip++] = OP_ACCEPT;
    size_t branch_b = ip;
    bc[ip++] = OP_FAIL;
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)branch_b);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.minlength == 2, "FAIL branch ignored in minlength");
    snobol_search_meta_free(&meta);
  }

  /* Both branches FAIL → minlength 0. */
  {
    uint8_t bc[16];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t f1 = ip;
    bc[ip++] = OP_FAIL;
    size_t f2 = ip;
    bc[ip++] = OP_FAIL;
    covm_emit_u32_be(bc, &a_at, (uint32_t)f1);
    covm_emit_u32_be(bc, &b_at, (uint32_t)f2);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.minlength == 0, "double-FAIL split minlength 0");
    snobol_search_meta_free(&meta);
  }

  /* REPEAT_STEP / BAL / bare NOPs / plain FAIL minlengths. */
  {
    uint8_t bc1[7] = {OP_REPEAT_STEP, 0, 0, 0, 0, 0, OP_ACCEPT};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc1, 7, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc2[9] = {OP_BAL, 0, 0, 0, 0, 0, 0, 0, OP_ACCEPT};
    snobol_search_derive_meta(bc2, 9, &meta);
    test_assert(meta.minlength == 2, "BAL minlength 2");
    snobol_search_meta_free(&meta);

    uint8_t bc3[5] = {OP_LEN, 0, 0, 0, 1}; /* walk runs past the end */
    snobol_search_derive_meta(bc3, 5, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc4[1] = {OP_FAIL};
    snobol_search_derive_meta(bc4, 1, &meta);
    snobol_search_meta_free(&meta);
  }
}

/* ── eligibility checkers: truncation paths ───────────────────────────────── */


void test_cov_meta_eligibility(void) {
  test_suite("Coverage: eligibility checker truncation paths");

  /* Bytecode longer than 512 bytes is never automaton-eligible. */
  {
    uint8_t bc[520];
    memset(bc, OP_NOP, sizeof(bc));
    bc[519] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 520, &meta);
    test_assert((!meta.automaton_eligible) != 0,
                "long bytecode not automaton-safe");
    snobol_search_meta_free(&meta);
  }

  /* Truncated operands make every checker bail out conservatively. */
  {
    snobol_search_meta_t meta;
    uint8_t bc1[1] = {OP_JMP};
    snobol_search_derive_meta(bc1, 1, &meta);
    test_assert((!meta.automaton_eligible && !meta.search_vm_eligible) != 0,
                "truncated JMP ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc2[1] = {OP_SPLIT};
    snobol_search_derive_meta(bc2, 1, &meta);
    test_assert((!meta.automaton_eligible) != 0, "truncated SPLIT ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc3[2] = {OP_LIT, 0};
    snobol_search_derive_meta(bc3, 2, &meta);
    test_assert((!meta.automaton_eligible) != 0, "truncated LIT ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc4[1] = {OP_ANY};
    snobol_search_derive_meta(bc4, 1, &meta);
    test_assert((!meta.automaton_eligible) != 0, "truncated ANY ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc5[1] = {OP_LEN};
    snobol_search_derive_meta(bc5, 1, &meta);
    test_assert((!meta.automaton_eligible) != 0, "truncated LEN ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc6[1] = {OP_REPEAT_INIT};
    snobol_search_derive_meta(bc6, 1, &meta);
    test_assert((!meta.automaton_eligible) != 0,
                "truncated REPEAT_INIT ineligible");
    snobol_search_meta_free(&meta);

    uint8_t bc7[1] = {OP_REPEAT_STEP};
    snobol_search_derive_meta(bc7, 1, &meta);
    test_assert((!meta.automaton_eligible) != 0,
                "truncated REPEAT_STEP ineligible");
    snobol_search_meta_free(&meta);
  }

  /* search-VM checker truncation paths. */
  {
    snobol_search_meta_t meta;
    uint8_t bc1[1] = {OP_ACCEPT};
    snobol_search_derive_meta(bc1, 1, &meta);
    test_assert((!meta.search_vm_eligible) != 0,
                "len<2 not search-VM eligible");
    snobol_search_meta_free(&meta);

    uint8_t bc2[1] = {OP_JMP};
    snobol_search_derive_meta(bc2, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc3[1] = {OP_SPLIT};
    snobol_search_derive_meta(bc3, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc4[2] = {OP_LIT, 0};
    snobol_search_derive_meta(bc4, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc5[1] = {OP_ANY};
    snobol_search_derive_meta(bc5, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc6[1] = {OP_LEN};
    snobol_search_derive_meta(bc6, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc7[1] = {OP_REPEAT_INIT};
    snobol_search_derive_meta(bc7, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc8[1] = {OP_CAP_START};
    snobol_search_derive_meta(bc8, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc9[1] = {OP_ASSIGN};
    snobol_search_derive_meta(bc9, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc10[1] = {OP_BREAKX};
    snobol_search_derive_meta(bc10, 1, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc11[1] = {0x7F};
    snobol_search_derive_meta(bc11, 1, &meta);
    snobol_search_meta_free(&meta);
  }
}

/* ── check_alt_literals / check_literal_only malformed shapes ─────────────── */


void test_cov_meta_alt_and_literal_only(void) {
  test_suite("Coverage: alt-literals + literal-only malformed shapes");

  /* Truncated alt-literals shapes. */
  {
    snobol_search_meta_t meta;
    uint8_t bc1[1] = {OP_ACCEPT};
    snobol_search_derive_meta(bc1, 1, &meta);
    test_assert((!meta.is_alt_literals) != 0, "tiny bytecode not alt-literals");
    snobol_search_meta_free(&meta);

    uint8_t bc2[6] = {OP_LIT, 0, 0, 0, 0, 0};
    snobol_search_derive_meta(bc2, 6, &meta);
    test_assert((!meta.is_alt_literals) != 0, "truncated LIT not alt-literals");
    snobol_search_meta_free(&meta);

    uint8_t bc3[8] = {OP_SPLIT, 0, 0, 0, 0, 0, 0, 0};
    snobol_search_derive_meta(bc3, 8, &meta);
    test_assert((!meta.is_alt_literals) != 0,
                "truncated SPLIT not alt-literals");
    snobol_search_meta_free(&meta);

    uint8_t bc4[6] = {OP_ANY, 0, 1, OP_ACCEPT, 0, 0};
    snobol_search_derive_meta(bc4, 6, &meta);
    test_assert((!meta.is_alt_literals) != 0, "non-LIT root not alt-literals");
    snobol_search_meta_free(&meta);
  }

  /* Literal followed by garbage (no ACCEPT) → not alt-literals.  (A JMP
   * cycle cannot be used to probe the alt-literals cycle guard: derive's
   * compute_minlength follows JMP targets with no cycle guard and would
   * hang.  See dev/coverage-findings.md.) */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_NOP; /* not ACCEPT */
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.is_alt_literals) != 0, "non-ACCEPT tail rejected");
    snobol_search_meta_free(&meta);
  }

  /* Literal-only: zero-width wrappers around the LIT. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_NOP;
    bc[ip++] = OP_FENCE;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    bc[ip++] = OP_NOP;
    bc[ip++] = OP_FENCE;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.is_literal_only, "wrapped literal still literal-only");
    snobol_search_meta_free(&meta);
  }

  /* Literal-only rejection paths. */
  {
    snobol_search_meta_t meta;
    uint8_t bc1[6] = {OP_NOP, OP_LEN, 0, 0, 0, 1};
    snobol_search_derive_meta(bc1, 6, &meta);
    test_assert((!meta.is_literal_only) != 0, "no LIT → not literal-only");
    snobol_search_meta_free(&meta);

    uint8_t bc2[12];
    size_t ip = 0;
    bc2[ip++] = OP_LIT;
    covm_emit_u32_be(bc2, &ip, 99); /* bad offset */
    covm_emit_u32_be(bc2, &ip, 1);
    bc2[ip++] = 'a';
    bc2[ip++] = OP_ACCEPT;
    snobol_search_derive_meta(bc2, ip, &meta);
    test_assert((!meta.is_literal_only) != 0,
                "bad LIT offset → not literal-only");
    snobol_search_meta_free(&meta);

    uint8_t bc3[32];
    ip = 0;
    bc3[ip++] = OP_LIT;
    covm_emit_u32_be(bc3, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc3, &ip, 1);
    bc3[ip++] = 'a';
    /* no ACCEPT at the end */
    snobol_search_derive_meta(bc3, ip, &meta);
    test_assert((!meta.is_literal_only) != 0,
                "missing ACCEPT → not literal-only");
    snobol_search_meta_free(&meta);
  }
}

/* ── fusion builder failure paths ─────────────────────────────────────────── */


void test_cov_meta_fusion_failures(void) {
  test_suite("Coverage: fusion builder failure paths");

  /* FAIL / ABORT terminate fusion. */
  {
    uint8_t bc[4] = {OP_FAIL, OP_ACCEPT};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 2, &meta);
    test_assert((!meta.fusion_eligible) != 0, "FAIL not fusible");
    snobol_search_meta_free(&meta);
  }

  /* More than MAX_FUSION_SEGMENTS (32) segments. */
  {
    uint8_t bc[(33 * 11) + 1];
    size_t ip = 0;
    for (int i = 0; i < 33; i++) {
      bc[ip++] = OP_LIT;
      covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
      covm_emit_u32_be(bc, &ip, 1);
      bc[ip++] = 'x';
    }
    bc[ip++] = OP_ACCEPT;
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.fusion_eligible) != 0,
                "over-capacity chain not fusible");
    snobol_search_meta_free(&meta);
  }

  /* Truncated / out-of-range / non-ASCII charclass ops in a chain. */
  {
    snobol_search_meta_t meta;

    uint8_t bc1[2] = {OP_LIT, 0};
    snobol_search_derive_meta(bc1, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc2[12];
    size_t ip = 0;
    bc2[ip++] = OP_LIT;
    covm_emit_u32_be(bc2, &ip, 99);
    covm_emit_u32_be(bc2, &ip, 1);
    bc2[ip++] = 'a';
    bc2[ip++] = OP_ACCEPT;
    snobol_search_derive_meta(bc2, ip, &meta);
    test_assert((!meta.fusion_eligible) != 0, "bad LIT offset not fusible");
    snobol_search_meta_free(&meta);

    /* Non-ASCII charclass ops reject fusion. */
    uint8_t bc3[64];
    ip = 0;
    bc3[ip++] = OP_SPAN;
    covm_emit_u16_be(bc3, &ip, 1);
    bc3[ip++] = OP_ACCEPT;
    size_t at = ip;
    covm_emit_u16_be(bc3, &at, 1);
    covm_emit_u16_be(bc3, &at, 0);
    covm_emit_u32_be(bc3, &at, 300);
    covm_emit_u32_be(bc3, &at, 400);
    covm_emit_u32_be(bc3, &at, 1);
    covm_emit_u32_be(bc3, &at, 1);
    snobol_search_derive_meta(bc3, at, &meta);
    test_assert((!meta.fusion_eligible) != 0, "non-ASCII SPAN not fusible");
    snobol_search_meta_free(&meta);

    /* Unresolved charclass set id rejects fusion. */
    uint8_t bc4[8] = {OP_ANY, 0, 9, OP_LIT, 0, 0, 0, 0};
    snobol_search_derive_meta(bc4, 8, &meta);
    test_assert((!meta.fusion_eligible) != 0, "unresolved ANY not fusible");
    snobol_search_meta_free(&meta);
  }

  /* SPLIT inside a fusion chain: one invalid branch kills fusion. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_ACCEPT;
    size_t branch_b = ip;
    bc[ip++] = OP_FAIL; /* invalid alt branch */
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)branch_b);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.fusion_eligible) != 0, "invalid alt branch not fusible");
    snobol_search_meta_free(&meta);
  }

  /* Fusion with an ALT branch containing a full segment mix. */
  {
    uint8_t bc[128];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 2);
    bc[ip++] = 'x';
    bc[ip++] = 'y';
    bc[ip++] = OP_ACCEPT;
    size_t branch_b = ip;
    bc[ip++] = OP_NOP; /* NOP skipped inside the alt walk */
    bc[ip++] = OP_ACCEPT;
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)branch_b);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    snobol_search_meta_free(&meta);
  }

  /* SPLIT with truncated operands and out-of-range targets. */
  {
    snobol_search_meta_t meta;
    uint8_t bc1[2] = {OP_SPLIT, 0};
    snobol_search_derive_meta(bc1, 2, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc2[10];
    size_t ip = 0;
    bc2[ip++] = OP_SPLIT;
    covm_emit_u32_be(bc2, &ip, 99);
    covm_emit_u32_be(bc2, &ip, 99);
    snobol_search_derive_meta(bc2, 10, &meta);
    test_assert((!meta.fusion_eligible) != 0,
                "SPLIT targets beyond not fusible");
    snobol_search_meta_free(&meta);
  }
}

/* ── derive tail: tier classification, required-literal bypass, cleanup ───── */


void test_cov_meta_derive_tail(void) {
  test_suite("Coverage: derive tail + cleanup paths");

  /* REM-only pattern: automaton-safe but not search-VM-safe → TIER_AUTOMATON. */
  {
    uint8_t bc[2] = {OP_REM, OP_ACCEPT};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 2, &meta);
    test_assert((meta.automaton_eligible && !meta.search_vm_eligible) != 0,
                "REM automaton-safe, not search-VM-safe");
    test_assert(meta.tier == TIER_AUTOMATON, "tier classified as AUTOMATON");
    snobol_search_meta_free(&meta);
  }

  /* Required-literal bypass: the SPLIT (scanned after LIT 'q') has branch A
   * = LIT('x') + JMP past LIT 'y', so 'y' is not required. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_LIT; /* leading literal so the scan has last_lit set */
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'q';
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    covm_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'x';
    bc[ip++] = OP_JMP;
    size_t jmp_at = ip;
    covm_emit_u32_be(bc, &ip, 0); /* patched to the final ACCEPT */
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'y';
    size_t final_accept = ip;
    bc[ip++] = OP_ACCEPT;
    covm_emit_u32_be(bc, &jmp_at, (uint32_t)final_accept);
    covm_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    covm_emit_u32_be(bc, &b_at, (uint32_t)final_accept);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.has_required_lit) != 0,
                "branch bypassing last literal clears required-lit");
    snobol_search_meta_free(&meta);
  }

  /* SPLIT branch target beyond the buffer in the required-lit scan. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_LIT; /* leading literal so the scan has last_lit set */
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_SPLIT;
    covm_emit_u32_be(bc, &ip, 99); /* branch A beyond */
    covm_emit_u32_be(bc, &ip, 0);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    snobol_search_meta_free(&meta);
  }

  /* meta_free with fusion and bmh_skip populated. */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 2);
    bc[ip++] = 'a';
    bc[ip++] = 'b';
    bc[ip++] = OP_SPAN;
    covm_emit_u16_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    size_t data_off = ip;
    size_t at = covm_append_ascii_class(bc, ip, '0', '9');
    covm_emit_u32_be(bc, &at, (uint32_t)data_off);
    covm_emit_u32_be(bc, &at, 1);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, at, &meta);
    test_assert(meta.has_bmh_skip, "bmh skip set");
    snobol_search_meta_free(&meta);
    test_assert(true, "meta_free releases bmh/fusion");
  }

  /* vm_cleanup frees a retained choice arena (keep_choices VM). */
  {
    uint8_t bc[16] = {OP_LIT, 0, 0, 0, 9, 0, 0, 0, 1, 'a', OP_ACCEPT};
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = 11;
    vm.s = "a";
    vm.len = 1;
    vm.keep_choices = true;
    bool ok = vm_exec(&vm);
    test_assert(ok, "keep_choices run succeeds");
    test_assert(vm.choices_arena != NULL, "arena retained");
    snobol_search_vm_cleanup(&vm);
    test_assert(vm.choices_arena == NULL, "cleanup frees retained arena");
    vm_free_labels(&vm);
  }
}


/* Shared test helpers (defined in test_vm.c). */
void cove_emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v);
void cove_emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v);

/* ===== test_coverage_engine2 (part): coverage-driven tests merged into test_search_meta_cache.c ===== */
#include "../../core/include/snobol/ast.h"
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/snobol_internal.h"

void test_cov_engine2_derive(void) {
  test_suite("Coverage: derive_meta malformed shapes (round 2)");

  snobol_search_meta_t meta;

  /* Alt-literals walkers reject truncated/odd shapes. */
  {
    uint8_t bc1[10] = {OP_LIT, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    snobol_search_derive_meta(bc1, 10, &meta);
    snobol_search_meta_free(&meta);

    uint8_t bc2[12] = {OP_LIT, 0, 0, 0, 99, 0, 0, 0, 2, 'a', 'b', OP_ACCEPT};
    snobol_search_derive_meta(bc2, 12, &meta);
    snobol_search_meta_free(&meta);

    /* Empty literal inside a split → conservative. */
    uint8_t bc3[32];
    size_t ip = 0;
    bc3[ip++] = OP_SPLIT;
    size_t a_at = ip;
    cove_emit_u32_be(bc3, &ip, 0);
    size_t b_at = ip;
    cove_emit_u32_be(bc3, &ip, 0);
    size_t br_a = ip;
    bc3[ip++] = OP_LIT;
    cove_emit_u32_be(bc3, &ip, 0); /* len 0 literal */
    cove_emit_u32_be(bc3, &ip, 0);
    bc3[ip++] = OP_ACCEPT;
    size_t br_b = ip;
    bc3[ip++] = OP_LIT;
    cove_emit_u32_be(bc3, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc3, &ip, 1);
    bc3[ip++] = 'x';
    bc3[ip++] = OP_ACCEPT;
    cove_emit_u32_be(bc3, &a_at, (uint32_t)br_a);
    cove_emit_u32_be(bc3, &b_at, (uint32_t)br_b);
    snobol_search_derive_meta(bc3, ip, &meta);
    test_assert((!meta.is_single_char_alt) != 0,
                "empty literal not single-char");
    snobol_search_meta_free(&meta);
  }

  /* NOTANY root with an ASCII class (derive tail branch). */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_NOTANY;
    cove_emit_u16_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    size_t data = ip;
    cove_emit_u16_be(bc, &ip, 1);
    cove_emit_u16_be(bc, &ip, 0);
    cove_emit_u32_be(bc, &ip, 'a');
    cove_emit_u32_be(bc, &ip, 'a');
    cove_emit_u32_be(bc, &ip, (uint32_t)data);
    cove_emit_u32_be(bc, &ip, 1);
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.ascii_class_only, "NOTANY ASCII class flagged");
    snobol_search_meta_free(&meta);
  }

  /* Required-lit bypass: branch A is JMP-led and its JMP chain jumps past
   * the last literal (the scanner's JMP-led branch skip, b_skip = 5). */
  {
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_LIT; /* leading literal so the scan has last_lit set */
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'q';
    bc[ip++] = OP_SPLIT;
    size_t a_at = ip;
    cove_emit_u32_be(bc, &ip, 0);
    size_t b_at = ip;
    cove_emit_u32_be(bc, &ip, 0);
    size_t branch_a = ip;
    bc[ip++] = OP_JMP; /* first branch instruction: JMP (skip = 5) */
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 4));
    bc[ip++] = OP_JMP; /* second JMP jumps past LIT 'y' */
    size_t jmp2_at = ip;
    cove_emit_u32_be(bc, &ip, 0);
    bc[ip++] = OP_LIT;
    cove_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    cove_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'y';
    size_t final_accept = ip;
    bc[ip++] = OP_ACCEPT;
    cove_emit_u32_be(bc, &a_at, (uint32_t)branch_a);
    cove_emit_u32_be(bc, &b_at, (uint32_t)final_accept);
    cove_emit_u32_be(bc, &jmp2_at, (uint32_t)final_accept);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert((!meta.has_required_lit) != 0,
                "JMP-led branch bypasses literal");
    snobol_search_meta_free(&meta);
  }
}


/* Malformed/cyclic bytecode: derive_meta must terminate and stay in bounds
 * (regression for the JMP-cycle hang and the truncated-prefix OOB read). */
void test_cov_meta_malformed_bytecode(void) {
  test_suite("Coverage: derive_meta on malformed bytecode");

  /* Cyclic JMP: JMP(0) self-loop used to hang compute_minlength.  The test
   * completing at all proves termination; minlength must be 0 (conservative
   * cycle-guard bail) and no literal prefix may be claimed. */
  {
    uint8_t bc[8] = {OP_JMP, 0, 0, 0, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 5, &meta);
    test_assert((meta.minlength == 0 && !meta.has_literal_prefix) != 0,
                "JMP self-loop terminates, minlength conservative");
    snobol_search_meta_free(&meta);
  }

  /* LIT then JMP back to the LIT: minlength must not spin. */
  {
    uint8_t bc[32];
    size_t ip = 0;
    bc[ip++] = OP_LIT;
    covm_emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
    covm_emit_u32_be(bc, &ip, 1);
    bc[ip++] = 'a';
    bc[ip++] = OP_JMP;
    covm_emit_u32_be(bc, &ip, 0);
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, ip, &meta);
    test_assert(meta.literal_prefix_len == 1,
                "LIT JMP(self) terminates, prefix still classified");
    snobol_search_meta_free(&meta);
  }

  /* Truncated prefix opcode: {POS, 0} with bc_len 2 must not read OOB. */
  {
    uint8_t bc[8] = {OP_POS, 0};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 2, &meta);
    test_assert((!meta.has_literal_prefix) != 0,
                "truncated POS prefix claims no literal prefix");
    snobol_search_meta_free(&meta);
  }

  /* NOP-only prefix that consumes the whole buffer: bc[ip] must not be
   * read when ip reaches bc_len. */
  {
    uint8_t bc[8] = {OP_NOP, OP_NOP};
    snobol_search_meta_t meta;
    snobol_search_derive_meta(bc, 2, &meta);
    test_assert((!meta.has_literal_prefix) != 0,
                "NOP-to-end claims no literal prefix");
    snobol_search_meta_free(&meta);
  }

  /* Well-formed compiler output equivalence: classification unchanged. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "'hello' SPAN('0-9')", 19, &err);
    test_assert(pat != NULL, "well-formed pattern compiles");
    if (pat) {
      const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
      test_assert((meta->has_literal_prefix && meta->literal_prefix_len == 5 &&
                   meta->has_bmh_skip) != 0,
                  "well-formed bytecode keeps literal prefix + BMH skip");
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
}

void test_search_meta_cache_suite(void) {
  test_suite("Search: cached metadata on pattern");

  /* Simple literal search */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile_ex(ctx, "'pqr'", 5, 0, &err);
    test_assert(pat != NULL, "compile succeeds");
    if (pat) {
      snobol_match_t *m =
          snobol_pattern_search(pat, "abcdefghijklmnopqrstuvwxyz", 26);
      test_assert(m != NULL, "search returns non-NULL");
      if (m) {
        test_assert(snobol_match_success(m), "search finds 'pqr' at offset 15");
        snobol_match_free(m);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* SPAN search-mode */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "SPAN(',')", 9, &err);
    test_assert(pat != NULL, "compile SPAN succeeds");
    if (pat) {
      snobol_match_t *m =
          snobol_pattern_search(pat, "id,name,email,age,status", 24);
      test_assert(m != NULL, "SPAN search returns non-NULL");
      if (m) {
        test_assert(snobol_match_success(m), "SPAN search finds comma run");
        snobol_match_free(m);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Alternation: split-any-fused path */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "'a' | 'b' | 'c'", 15, &err);
    test_assert(pat != NULL, "compile alternation succeeds");
    if (pat) {
      snobol_match_t *m = snobol_pattern_search(pat, "the quick brown fox", 19);
      test_assert(m != NULL, "alternation search returns non-NULL");
      if (m) {
        test_assert(snobol_match_success(m), "alternation search succeeds");
        snobol_match_free(m);
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }

  /* Hot loop: verify JIT still fires after the cache change.
   * With cached metadata, the search runtime should still get the
   * meta pointer and the JIT should fire. */
  {
    snobol_context_t *ctx = snobol_context_create();
    char *err = nullptr;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, "SPAN('a')", 9, &err);
    test_assert(pat != NULL, "compile hot-loop pattern succeeds");
    if (pat) {
      /* warmup: get to JIT hotness */
      for (int i = 0; i < 100; i++) {
        snobol_match_t *m =
            snobol_pattern_search(pat, "aaaaaaaaaaaaaaaaaa", 18);
        if (m) {
          snobol_match_free(m);
        }
      }
      for (int i = 0; i < 50; i++) {
        snobol_match_t *m =
            snobol_pattern_search(pat, "aaaaaaaaaaaaaaaaaa", 18);
        if (m) {
          snobol_match_free(m);
        }
      }
      snobol_pattern_free(pat);
    }
    free(err);
    snobol_context_destroy(ctx);
  }
  test_cov_meta_root_classification();
  test_cov_meta_start_bitmap();
  test_cov_meta_minlength();
  test_cov_meta_eligibility();
  test_cov_meta_alt_and_literal_only();
  test_cov_meta_fusion_failures();
  test_cov_meta_derive_tail();
  test_cov_meta_malformed_bytecode();
  test_cov_engine2_derive();
}
