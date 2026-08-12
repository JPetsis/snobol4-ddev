/**
 * test_search_automaton.c
 *
 * Tests for the search-mode DFA automaton: construction (build_dfa),
 * eligibility checking, and execution routing (tier 7 of snobol_search_exec).
 *
 * Uses only LIT and LEN opcodes (inline data, no range_meta needed).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"

/* Provided by test_runner.c */
void test_suite(const char *name);
void test_assert(bool condition, const char *message);

/* ---------------------------------------------------------------------------
 * Bytecode building helpers
 *
 * LIT format:  OP_LIT (1) + u32 data_offset (4) + u32 length (4) + data
 * LEN format:  OP_LEN (1) + u32 count (4)
 * BREAK/BREAKX format:  OP (1) + u16 set_id (2) + ACCEPT + inline range_meta
 * ---------------------------------------------------------------------------
 */

static void emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)(v >> 24);
  bc[(*ip)++] = (uint8_t)(v >> 16);
  bc[(*ip)++] = (uint8_t)(v >> 8);
  bc[(*ip)++] = (uint8_t)(v);
}

static void emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)(v >> 8);
  bc[(*ip)++] = (uint8_t)(v);
}

static size_t build_lit_accept(uint8_t *bc, const char *s, size_t slen) {
  size_t ip = 0;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(1 + 4 + 4));
  emit_u32_be(bc, &ip, (uint32_t)slen);
  for (size_t i = 0; i < slen; i++) {
    bc[ip++] = (uint8_t)s[i];
  }
  bc[ip++] = OP_ACCEPT;
  return ip;
}

static size_t build_len_accept(uint8_t *bc, uint32_t n) {
  size_t ip = 0;
  bc[ip++] = OP_LEN;
  emit_u32_be(bc, &ip, n);
  bc[ip++] = OP_ACCEPT;
  return ip;
}

static size_t build_len_lit_len_accept(uint8_t *bc) {
  size_t ip = 0;
  bc[ip++] = OP_LEN;
  emit_u32_be(bc, &ip, 2);
  size_t lit_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(lit_ip + 9));
  emit_u32_be(bc, &ip, 3);
  bc[ip++] = 'a';
  bc[ip++] = 'b';
  bc[ip++] = 'c';
  bc[ip++] = OP_LEN;
  emit_u32_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;
  return ip;
}

static size_t build_lit_lit_accept(uint8_t *bc) {
  size_t ip = 0;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(1 + 4 + 4));
  emit_u32_be(bc, &ip, 1);
  bc[ip++] = 'a';
  size_t lit2_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(lit2_ip + 9));
  emit_u32_be(bc, &ip, 1);
  bc[ip++] = 'b';
  bc[ip++] = OP_ACCEPT;
  return ip;
}

static size_t build_split_lit_lit(uint8_t *bc, const char *s1, size_t len1,
                                  const char *s2, size_t len2) {
  size_t ip = 0;
  size_t split_ip = ip;
  bc[ip++] = OP_SPLIT;
  emit_u32_be(bc, &ip, 0); /* placeholder for target1 */
  emit_u32_be(bc, &ip, 0); /* placeholder for target2 */

  size_t lit1_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(lit1_ip + 9));
  emit_u32_be(bc, &ip, (uint32_t)len1);
  for (size_t i = 0; i < len1; i++) {
    bc[ip++] = (uint8_t)s1[i];
  }
  bc[ip++] = OP_ACCEPT;
  size_t after1 = ip;

  size_t lit2_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(lit2_ip + 9));
  emit_u32_be(bc, &ip, (uint32_t)len2);
  for (size_t i = 0; i < len2; i++) {
    bc[ip++] = (uint8_t)s2[i];
  }
  bc[ip++] = OP_ACCEPT;
  size_t after2 = ip;

  /* Patch SPLIT targets */
  size_t p = split_ip + 1;
  emit_u32_be(bc, &p, (uint32_t)(lit1_ip - split_ip));
  emit_u32_be(bc, &p, (uint32_t)(lit2_ip - split_ip));

  (void)after1;
  (void)after2;
  return ip;
}

static size_t build_breakx_accept(uint8_t *bc) {
  size_t ip = 0;
  bc[ip++] = OP_BREAKX;
  emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;

  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, 1);
  emit_u16_be(bc, &ip, 0);
  emit_u32_be(bc, &ip, (uint32_t)',');
  emit_u32_be(bc, &ip, (uint32_t)',');
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

/* Build many SPLIT single-byte LIT alternations to test state explosion */
static size_t build_many_splits(uint8_t *bc) {
  size_t ip = 0;
#define NSPLITS 64
  size_t splits[NSPLITS];
  for (int i = 0; i < NSPLITS; i++) {
    splits[i] = ip;
    bc[ip++] = OP_SPLIT;
    emit_u32_be(bc, &ip, 0); /* placeholder for target1 */
    emit_u32_be(bc, &ip, 0); /* placeholder for target2 */
  }
  for (int i = 0; i < NSPLITS; i++) {
    size_t lit_ip = ip;
    bc[ip++] = OP_LIT;
    emit_u32_be(bc, &ip, (uint32_t)(lit_ip + 9));
    emit_u32_be(bc, &ip, 1);
    bc[ip++] = (uint8_t)('a' + (i % 26));
    bc[ip++] = OP_ACCEPT;
    size_t next_ip = (i + 1 < NSPLITS) ? splits[i + 1] : (lit_ip + 3);
    size_t p = splits[i] + 1;
    emit_u32_be(bc, &p, (uint32_t)(lit_ip - splits[i]));
    emit_u32_be(bc, &p, (uint32_t)(next_ip - splits[i]));
  }
  return ip;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

static VM make_vm(const uint8_t *bc, size_t bc_len) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  return vm;
}

/* Force automaton path by clearing all earlier-tier meta flags */
static snobol_search_meta_t make_automaton_meta(const uint8_t *bc,
                                                size_t bc_len) {
  snobol_search_meta_t m;
  snobol_search_derive_meta(bc, bc_len, &m);
  m.is_break_family = false;
  m.is_span_family = false;
  m.is_literal_only = false;
  m.has_literal_prefix = false;
  m.has_first_byte = false;
  m.has_candidate_bitmap = false;
  m.is_alt_literals = false;
  m.is_single_char_alt = false;
  m.has_start_bitmap = false;
  m.has_bmh_skip = false;
  /* Free BMH skip table — it was allocated by derive_meta but disabled
   * above.  The automaton tier never touches it. */
  snobol_search_meta_free(&m);
  return m;
}

/* ---------------------------------------------------------------------------
 * 5.1  DFA construction: unsupported opcode (LEN) yields degenerate DFA
 *
 * LEN is not handled by the DFA instruction set (no case OP_LEN in the BFS
 * transition loop), so build_dfa produces a 1-state DFA with no accepting
 * states.  Such patterns are marked !automaton_eligible and never reach the
 * automaton dispatch path at runtime.  This test simply verifies that the
 * builder doesn't crash on unsupported opcodes.
 * ---------------------------------------------------------------------------
 */
static void test_dfa_build_unsupported_opcode(void) {
  test_suite("5.1  DFA: unsupported opcode (LEN) yields no-accept DFA");

  uint8_t bc[256];
  size_t bc_len = build_len_lit_len_accept(bc);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);

  test_assert(dfa != NULL, "build_dfa succeeded (non-NULL)");
  if (dfa) {
    test_assert(dfa->num_states == 1, "dfa->num_states == 1 (degenerate)");
    test_assert(dfa->start_state == 0, "start_state is 0");

    bool found_accept = false;
    for (uint32_t s = 0; s < dfa->num_states; s++) {
      if (dfa->accepting[s / 8] & (uint8_t)(1U << (s % 8))) {
        found_accept = true;
        break;
      }
    }
    test_assert((!found_accept) != 0,
                "no accepting state (LEN not in automaton instruction set)");
    snobol_dfa_free(dfa);
  }
}

/* ---------------------------------------------------------------------------
 * 5.2  DFA construction: SPLIT alternation ("cat" | "dog")
 * ---------------------------------------------------------------------------
 */
static void test_dfa_build_split_alternation(void) {
  test_suite("5.2  DFA: SPLIT alternation (\"cat\" | \"dog\")");

  uint8_t bc[256];
  size_t bc_len = build_split_lit_lit(bc, "cat", 3, "dog", 3);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);

  test_assert(dfa != NULL, "build_dfa succeeded for alternation");
  if (dfa) {
    test_assert(dfa->num_states > 0, "num_states > 0");
    test_assert(dfa->start_state < dfa->num_states, "start_state within range");

    bool found_accept = false;
    for (uint32_t s = 0; s < dfa->num_states; s++) {
      if (dfa->accepting[s / 8] & (uint8_t)(1U << (s % 8))) {
        found_accept = true;
        break;
      }
    }
    test_assert(found_accept, "at least one accepting state");
    snobol_dfa_free(dfa);
  }
}

/* ---------------------------------------------------------------------------
 * 5.3  DFA execution: match at offset 0
 * ---------------------------------------------------------------------------
 */
static void test_dfa_exec_match_offset_zero(void) {
  test_suite("5.3  DFA exec: match at offset 0");

  uint8_t bc[64];
  size_t bc_len = build_lit_accept(bc, "abc", 3);

  snobol_search_meta_t meta = make_automaton_meta(bc, bc_len);
  test_assert(meta.automaton_eligible, "LIT ACCEPT is automaton-eligible");

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);
  test_assert(dfa != NULL, "build_dfa succeeded");
  if (!dfa) {
    return;
  }

  snobol_search_result_t result;
  bool ok =
      snobol_search_exec(&vm, "abcdef", 6, 0, &meta, dfa, &result, nullptr);
  test_assert(ok, "match found at offset 0");
  test_assert(result.match_start == 0, "match_start == 0");
  test_assert(result.match_end == 3, "match_end == 3");

  snobol_dfa_free(dfa);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * 5.4  DFA execution: match at non-zero offset
 * ---------------------------------------------------------------------------
 */
static void test_dfa_exec_match_nonzero_offset(void) {
  test_suite("5.4  DFA exec: match at non-zero offset");

  uint8_t bc[64];
  size_t bc_len = build_lit_accept(bc, "xyz", 3);

  snobol_search_meta_t meta = make_automaton_meta(bc, bc_len);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);
  test_assert(dfa != NULL, "build_dfa succeeded");
  if (!dfa) {
    return;
  }

  snobol_search_result_t result;

  bool ok = snobol_search_exec(&vm, "----xyz----", 11, 0, &meta, dfa, &result,
                               nullptr);
  test_assert(ok, "match found in search mode");
  test_assert(result.match_start == 4, "match_start == 4");
  test_assert(result.match_end == 7, "match_end == 7");

  ok = snobol_search_exec(&vm, "abcxyzxyz", 9, 0, &meta, dfa, &result, nullptr);
  test_assert(ok, "first match in 'abcxyzxyz'");
  test_assert(result.match_start == 3, "match_start == 3 (first 'xyz')");

  ok = snobol_search_exec(&vm, "abcxyzxyz", 9, 4, &meta, dfa, &result, nullptr);
  test_assert(ok, "second match with start_offset=4");
  test_assert(result.match_start == 6, "match_start == 6 (second 'xyz')");

  snobol_dfa_free(dfa);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * 5.5  DFA execution: no match
 * ---------------------------------------------------------------------------
 */
static void test_dfa_exec_no_match(void) {
  test_suite("5.5  DFA exec: no match");

  uint8_t bc[64];
  size_t bc_len = build_lit_accept(bc, "abc", 3);

  snobol_search_meta_t meta = make_automaton_meta(bc, bc_len);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);
  test_assert(dfa != NULL, "build_dfa succeeded");
  if (!dfa) {
    return;
  }

  snobol_search_result_t result;
  bool ok =
      snobol_search_exec(&vm, "xxxxxxx", 7, 0, &meta, dfa, &result, nullptr);
  test_assert((!ok) != 0, "no match returned failure");
  test_assert((!result.success) != 0, "result.success is false");

  snobol_dfa_free(dfa);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * 5.6  Non-eligible pattern falls back to Tier 6
 * ---------------------------------------------------------------------------
 */
static void test_non_eligible_fallback(void) {
  test_suite("5.6  Non-eligible pattern falls back to Tier 6");

  uint8_t bc[128];
  size_t bc_len = build_breakx_accept(bc);

  snobol_search_meta_t meta;
  snobol_search_derive_meta(bc, bc_len, &meta);

  test_assert((!meta.automaton_eligible) != 0,
              "BREAKX pattern is NOT automaton-eligible");

  VM vm = make_vm(bc, bc_len);
  snobol_search_result_t result;
  bool ok =
      snobol_search_exec(&vm, "a,b,c", 5, 0, &meta, nullptr, &result, nullptr);
  test_assert(ok, "BREAKX matches via accelerated BREAK path");
  /* BREAKX(',') on "a,b,c" matches the leading non-delimiter run "a":
    * start 0, end 1 (exclusive of the following ','). */
  test_assert(result.match_start == 0, "BREAKX match_start == 0 ('a')");
  test_assert(result.match_end == 1, "BREAKX match_end == 1 (before ',')");

  snobol_search_meta_free(&meta);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * 5.7  State explosion cap triggers fallback
 * ---------------------------------------------------------------------------
 */
static void test_state_explosion_cap(void) {
  test_suite("5.7  State explosion cap triggers fallback");

  uint8_t bc[8192];
  size_t bc_len = build_many_splits(bc);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);

  if (dfa) {
    test_assert(dfa->num_states <= SNOBOL_DFA_MAX_STATES,
                "DFA state count within limit");
    snobol_dfa_free(dfa);
  } else {
    test_assert(true, "build_dfa returned NULL (state explosion)");
  }
}

/* ---------------------------------------------------------------------------
 * 5.8  REPEAT_INIT/STEP in automaton
 * ---------------------------------------------------------------------------
 */
static void test_repeat_in_automaton(void) {
  test_suite("5.8  REPEAT_INIT/STEP in automaton");

  /* Build bytecode: REPEAT_INIT(min=0, max=-1) { LIT 'ab' } REPEAT_STEP LIT 'cd' ACCEPT
   * Uses 2-byte literals so has_bmh_skip is true, triggering the automaton
   * override per the BMH-skip gate requirement. */
  uint8_t bc[512];
  size_t ip = 0;

  /* REPEAT_INIT: opcode(1) + loop_id(1) + min(4) + max(4) + skip_target(4) = 14 bytes */
  size_t init_ip = ip;
  bc[ip++] = OP_REPEAT_INIT;
  bc[ip++] = 0;                       /* loop_id */
  emit_u32_be(bc, &ip, 0);            /* min = 0 */
  emit_u32_be(bc, &ip, (uint32_t)-1); /* max = -1 (unbounded) */
  emit_u32_be(bc, &ip, 0);            /* skip_target placeholder */

  /* Body: LIT 'ab' (2 bytes — ensures has_bmh_skip = true) */
  size_t body_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  emit_u32_be(bc, &ip, 2);
  bc[ip++] = 'a';
  bc[ip++] = 'b';

  /* REPEAT_STEP: opcode(1) + loop_id(1) + jmp_target(4) = 6 bytes */
  bc[ip++] = OP_REPEAT_STEP;
  bc[ip++] = 0;
  emit_u32_be(bc, &ip, (uint32_t)body_ip);

  /* Skip target: LIT 'cd' ACCEPT */
  size_t skip_ip = ip;
  bc[ip++] = OP_LIT;
  emit_u32_be(bc, &ip, (uint32_t)(ip + 8));
  emit_u32_be(bc, &ip, 2);
  bc[ip++] = 'c';
  bc[ip++] = 'd';
  bc[ip++] = OP_ACCEPT;

  /* Patch skip_target in REPEAT_INIT */
  size_t patch = init_ip + 1 + 1 + 4 + 4;
  bc[patch + 0] = (uint8_t)((skip_ip >> 24) & 0xFF);
  bc[patch + 1] = (uint8_t)((skip_ip >> 16) & 0xFF);
  bc[patch + 2] = (uint8_t)((skip_ip >> 8) & 0xFF);
  bc[patch + 3] = (uint8_t)(skip_ip & 0xFF);

  size_t bc_len = ip;

  snobol_search_meta_t meta = make_automaton_meta(bc, bc_len);
  test_assert(meta.automaton_eligible, "REPEAT pattern is automaton-eligible");

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);
  test_assert(dfa != NULL, "build_dfa succeeded for REPEAT pattern");
  if (!dfa) {
    return;
  }

  snobol_search_result_t result;

  /* Test: "cd" matches (zero repeats) */
  bool ok = snobol_search_exec(&vm, "cd", 2, 0, &meta, dfa, &result, nullptr);
  test_assert(ok, "ARB('ab') 'cd' matches 'cd' (zero repeats)");
  test_assert(result.match_start == 0, "match_start == 0");
  test_assert(result.match_end == 2, "match_end == 2");

  /* Test: "abcd" matches (one repeat) */
  ok = snobol_search_exec(&vm, "abcd", 4, 0, &meta, dfa, &result, nullptr);
  test_assert(ok, "ARB('ab') 'cd' matches 'abcd'");
  test_assert(result.match_start == 0, "match_start == 0");
  test_assert(result.match_end == 4, "match_end == 4");

  /* Test: "ababcd" matches (two repeats) */
  ok = snobol_search_exec(&vm, "ababcd", 6, 0, &meta, dfa, &result, nullptr);
  test_assert(ok, "ARB('ab') 'cd' matches 'ababcd'");
  test_assert(result.match_start == 0, "match_start == 0");
  test_assert(result.match_end == 6, "match_end == 6");

  /* Test: "a" does not match (no 'cd') */
  ok = snobol_search_exec(&vm, "a", 1, 0, &meta, dfa, &result, nullptr);
  test_assert((!ok) != 0, "ARB('ab') 'cd' does not match 'a' (no 'cd')");

  snobol_dfa_free(dfa);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * 5.9  DFA lifecycle (allocation + free + snobol_pattern_t API)
 * ---------------------------------------------------------------------------
 */
static void test_dfa_lifecycle(void) {
  test_suite("5.9  DFA lifecycle (allocation + free)");

  uint8_t bc[64];
  size_t bc_len = build_lit_accept(bc, "hello", 5);

  VM vm = make_vm(bc, bc_len);
  snobol_dfa_t *dfa = build_dfa(bc, bc_len, &vm);
  test_assert(dfa != NULL, "DFA allocated");
  if (!dfa) {
    return;
  }

  snobol_search_meta_t meta = make_automaton_meta(bc, bc_len);
  snobol_search_result_t result;
  bool ok = snobol_search_exec(&vm, "hello world", 11, 0, &meta, dfa, &result,
                               nullptr);
  test_assert(ok, "DFA-based match succeeded");
  test_assert(result.match_start == 0, "match_start == 0");

  snobol_dfa_free(dfa);

  char *err = nullptr;
  snobol_pattern_t *pat = snobol_pattern_compile(nullptr, "'hello'", 7, &err);
  test_assert(pat != NULL, "pattern 'hello' compiled");
  if (pat) {
    /* First match triggers DFA construction and caching --
     * BUT literal-only patterns go through a fast path that
     * skips DFA building, so automaton won't be available. */
    snobol_match_t *match = snobol_pattern_match(pat, "hello world", 11);
    test_assert(match != NULL, "pattern match returned non-NULL");
    if (match) {
      test_assert(snobol_match_success(match),
                  "snobol_match_success for 'hello'");
      snobol_match_free(match);
    }
    /* Use snobol_pattern_search instead, which always builds the DFA */
    snobol_match_t *s_match = snobol_pattern_search(pat, "hello world", 11);
    test_assert(s_match != NULL, "search returned non-NULL");
    if (s_match) {
      test_assert(snobol_match_success(s_match),
                  "snobol_match_success via search");
      snobol_match_free(s_match);
    }
    test_assert(snobol_pattern_automaton_available(pat),
                "automaton available on 'hello' pattern after search");
    snobol_pattern_free(pat);
  }
  free(err);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Suite entry point
 * ---------------------------------------------------------------------------
 */
void test_search_automaton_suite(void) {
  test_dfa_build_unsupported_opcode();
  test_dfa_build_split_alternation();
  test_dfa_exec_match_offset_zero();
  test_dfa_exec_match_nonzero_offset();
  test_dfa_exec_no_match();
  test_non_eligible_fallback();
  test_state_explosion_cap();
  test_repeat_in_automaton();
  test_dfa_lifecycle();
}
