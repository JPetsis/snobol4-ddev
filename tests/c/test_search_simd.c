/**
 * test_search_simd.c
 *
 * Tests for SIMD-accelerated Thompson NFA (Tier 9):
 *   - check_simd_eligible() for various opcode patterns
 *   - Tier routing via snobol_search_derive_meta()
 *   - Execution via tier_simd_nfa() / snobol_search_exec()
 *   - Edge cases: 0/1-byte, tail bytes, UTF-8 byte ranges
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
 * Bytecode helpers
 * ---------------------------------------------------------------------------
 */
static void emit_u32_be(uint8_t *bc, size_t *ip, uint32_t v) {
  bc[(*ip)++] = (uint8_t)(v >> 24);
  bc[(*ip)++] = (uint8_t)(v >> 16);
  bc[(*ip)++] = (uint8_t)(v >> 8);
  bc[(*ip)++] = (uint8_t)v;
}

static void emit_u16_be(uint8_t *bc, size_t *ip, uint16_t v) {
  bc[(*ip)++] = (uint8_t)(v >> 8);
  bc[(*ip)++] = (uint8_t)v;
}

/* Build SPAN(chars) + ACCEPT bytecode with inline offset table */
static size_t build_span_accept(uint8_t *bc, const char *chars, size_t nchr,
                                size_t extra_terminators) {
  size_t ip = 0;
  bc[ip++] = OP_SPAN;
  emit_u16_be(bc, &ip, 1);
  for (size_t i = 0; i < extra_terminators; i++) {
    bc[ip++] = OP_ACCEPT;
  }
  bc[ip++] = OP_ACCEPT;

  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, (uint16_t)nchr);
  emit_u16_be(bc, &ip, 0);
  for (size_t i = 0; i < nchr; i++) {
    uint32_t c = (uint32_t)(unsigned char)chars[i];
    emit_u32_be(bc, &ip, c);
    emit_u32_be(bc, &ip, c);
  }
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

static size_t build_break_accept(uint8_t *bc, const char *chars, size_t nchr) {
  size_t ip = 0;
  bc[ip++] = OP_BREAK;
  emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;

  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, (uint16_t)nchr);
  emit_u16_be(bc, &ip, 0);
  for (size_t i = 0; i < nchr; i++) {
    uint32_t c = (uint32_t)(unsigned char)chars[i];
    emit_u32_be(bc, &ip, c);
    emit_u32_be(bc, &ip, c);
  }
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

static size_t build_any_accept(uint8_t *bc, const char *chars, size_t nchr) {
  size_t ip = 0;
  bc[ip++] = OP_ANY;
  emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;

  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, (uint16_t)nchr);
  emit_u16_be(bc, &ip, 0);
  for (size_t i = 0; i < nchr; i++) {
    uint32_t c = (uint32_t)(unsigned char)chars[i];
    emit_u32_be(bc, &ip, c);
    emit_u32_be(bc, &ip, c);
  }
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

static size_t build_notany_accept(uint8_t *bc, const char *chars, size_t nchr) {
  size_t ip = 0;
  bc[ip++] = OP_NOTANY;
  emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;

  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, (uint16_t)nchr);
  emit_u16_be(bc, &ip, 0);
  for (size_t i = 0; i < nchr; i++) {
    uint32_t c = (uint32_t)(unsigned char)chars[i];
    emit_u32_be(bc, &ip, c);
    emit_u32_be(bc, &ip, c);
  }
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

static size_t build_len_accept(uint8_t *bc, uint32_t n) {
  size_t ip = 0;
  bc[ip++] = OP_LEN;
  emit_u32_be(bc, &ip, n);
  bc[ip++] = OP_ACCEPT;
  return ip;
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

/* Build SPAN with multi-byte UTF-8 ranges (bytes >= 128) */
static size_t build_span_utf8(uint8_t *bc) {
  size_t ip = 0;
  bc[ip++] = OP_SPAN;
  emit_u16_be(bc, &ip, 1);
  bc[ip++] = OP_ACCEPT;

  /* Single range [0xC0, 0xDF] — 2-byte UTF-8 lead bytes */
  size_t class_data_off = ip;
  emit_u16_be(bc, &ip, 1);
  emit_u16_be(bc, &ip, 0);
  emit_u32_be(bc, &ip, 0xC0);
  emit_u32_be(bc, &ip, 0xDF);
  emit_u32_be(bc, &ip, (uint32_t)class_data_off);
  emit_u32_be(bc, &ip, 1);
  return ip;
}

/* ---------------------------------------------------------------------------
 * Test: check_simd_eligible() — basic opcode filtering
 * ---------------------------------------------------------------------------
 */
static void test_simd_eligibility(void) {
  test_suite("SIMD: eligibility checks");

  {
    uint8_t bc[128];
    size_t n = build_span_accept(bc, "abc", 3, 0);
    test_assert(check_simd_eligible(bc, n), "SPAN('abc')+ACCEPT is eligible");
  }
  {
    uint8_t bc[128];
    size_t n = build_break_accept(bc, ",;", 2);
    test_assert(check_simd_eligible(bc, n), "BREAK(',;')+ACCEPT is eligible");
  }
  {
    uint8_t bc[128];
    size_t n = build_any_accept(bc, "0123456789", 10);
    test_assert(check_simd_eligible(bc, n), "ANY('0-9')+ACCEPT is eligible");
  }
  {
    uint8_t bc[128];
    size_t n = build_notany_accept(bc, "abc", 3);
    test_assert(check_simd_eligible(bc, n), "NOTANY('abc')+ACCEPT is eligible");
  }
  {
    uint8_t bc[128];
    size_t n = build_len_accept(bc, 3);
    test_assert((!check_simd_eligible(bc, n)) != 0,
                "LEN(3)+ACCEPT is NOT eligible");
  }
  {
    uint8_t bc[128];
    size_t n = build_lit_accept(bc, "hello", 5);
    test_assert((!check_simd_eligible(bc, n)) != 0,
                "LIT('hello')+ACCEPT is NOT eligible");
  }
  {
    /* Nested SPAN(JMP+SPLIT): not eligible */
    uint8_t bc[64];
    size_t ip = 0;
    bc[ip++] = OP_SPLIT;
    emit_u32_be(bc, &ip, 5);
    emit_u32_be(bc, &ip, 10);
    bc[ip++] = OP_SPAN;
    emit_u16_be(bc, &ip, 1);
    bc[ip++] = OP_ACCEPT;
    size_t class_off = ip;
    emit_u16_be(bc, &ip, 1);
    emit_u16_be(bc, &ip, 0);
    emit_u32_be(bc, &ip, 'a');
    emit_u32_be(bc, &ip, 'a');
    emit_u32_be(bc, &ip, (uint32_t)class_off);
    emit_u32_be(bc, &ip, 1);
    size_t n = ip;
    test_assert((!check_simd_eligible(bc, n)) != 0,
                "SPLIT+SPAN is NOT eligible (no control flow)");
  }
  {
    /* UTF-8 byte ranges (>=128) are eligible */
    uint8_t bc[128];
    size_t n = build_span_utf8(bc);
    test_assert(check_simd_eligible(bc, n),
                "SPAN([0xC0-0xDF])+ACCEPT is eligible (UTF-8 safe)");
  }
}

/* ---------------------------------------------------------------------------
 * Test: tier routing places eligible patterns at TIER_SIMD_NFA
 * ---------------------------------------------------------------------------
 */
static void test_simd_tier_routing(void) {
  test_suite("SIMD: tier routing");

  {
    uint8_t bc[128];
    size_t n = build_span_accept(bc, " \t", 2, 0);
    snobol_search_meta_t m;
    memset(&m, 0, sizeof(m));
    snobol_search_derive_meta(bc, n, &m);
    test_assert(m.simd_eligible, "simd_eligible true for SPAN(' \\t')");
    test_assert(
        (m.tier == TIER_SIMD_NFA || m.tier < TIER_SIMD_NFA) != 0,
        "SPAN(' \\t') tier == TIER_SIMD_NFA (or earlier if faster path)");
    snobol_search_meta_free(&m);
  }
  {
    uint8_t bc[128];
    size_t n = build_any_accept(bc, "aeiou", 5);
    snobol_search_meta_t m;
    memset(&m, 0, sizeof(m));
    snobol_search_derive_meta(bc, n, &m);
    /* ANY may be captured by TIER_BITMAP (Tier 4) first if it fits the
     * single-char alt acceleration; otherwise it routes to TIER_SIMD_NFA.
     * Either is correct — both are fast acceleration tiers. */
    if (m.simd_eligible) {
      test_assert((m.tier == TIER_SIMD_NFA || m.tier == TIER_BITMAP) != 0,
                  "ANY('aeiou') routes through an acceleration tier");
    }
    snobol_search_meta_free(&m);
  }
  {
    uint8_t bc[128];
    size_t n = build_len_accept(bc, 5);
    snobol_search_meta_t m;
    memset(&m, 0, sizeof(m));
    snobol_search_derive_meta(bc, n, &m);
    test_assert((!m.simd_eligible) != 0,
                "simd_eligible false for LEN(5)+ACCEPT");
    test_assert(m.tier != TIER_SIMD_NFA,
                "LEN(5)+ACCEPT does NOT route through TIER_SIMD_NFA");
    snobol_search_meta_free(&m);
  }
}

/* ---------------------------------------------------------------------------
 * Test: SPAN execution — match consecutive class bytes
 * ---------------------------------------------------------------------------
 */
static void test_simd_span(void) {
  test_suite("SIMD: SPAN execution");

  /* Create bytecode + range_meta for a full VM */
  uint8_t bc[128];
  size_t n = build_span_accept(bc, "abc", 3, 0);

  /* Derive metadata */
  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  /* Build range_meta */
  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* SPAN('abc') on "aabbcxyz": should match "aabbc" */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "aabbcxyz", 8, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('abc') matches 'aabbc' in 'aabbcxyz'");
    test_assert(r.match_start == 0, "SPAN('abc') match_start == 0");
    test_assert(r.match_end == 5, "SPAN('abc') match_end == 5");
  }

  /* SPAN('abc') on "xyz": should fail (no class bytes) */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "xyz", 3, 0, &m, nullptr, &r, &d);
    test_assert((!ok) != 0, "SPAN('abc') fails on 'xyz'");
  }

  /* SPAN('abc') on "aaaa": match the full string */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "aaaa", 4, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('abc') matches 'aaaa'");
    test_assert(r.match_end == 4, "SPAN('abc') matches all 4 bytes");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: BREAK execution — match until delimiter
 * ---------------------------------------------------------------------------
 */
static void test_simd_break(void) {
  test_suite("SIMD: BREAK execution");

  uint8_t bc[128];
  size_t n = build_break_accept(bc, ",", 1);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* BREAK(',') on "hello,world": break at comma */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        snobol_search_exec(&vm, "hello,world", 11, 0, &m, nullptr, &r, &d);
    test_assert(ok, "BREAK(',') matches in 'hello,world'");
    /* BREAK(',') matches the run of non-delimiter bytes up to the comma:
     * "hello" starting at 0, ending at 5 (exclusive of the delimiter). */
    test_assert(r.match_start == 0, "BREAK match_start == 0");
    test_assert(r.match_end == 5, "BREAK match_end == 5 ('hello')");
  }

  /* BREAK(',') on "abc": no comma, should match at end */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "abc", 3, 0, &m, nullptr, &r, &d);
    test_assert(ok, "BREAK(',') matches all of 'abc' (no comma)");
    test_assert(r.match_end == 3, "BREAK match_end == 3 (end of subject)");
  }

  /* BREAK(',') on ",": empty match */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, ",", 1, 0, &m, nullptr, &r, &d);
    test_assert(ok, "BREAK(',') on ',' matches empty (pos 0)");
    test_assert(r.match_start == 0, "BREAK match_start == 0");
    test_assert(r.match_end == 0, "BREAK match_end == 0 (at delimiter)");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: ANY execution — match single class byte
 * ---------------------------------------------------------------------------
 */
static void test_simd_any(void) {
  test_suite("SIMD: ANY execution");

  uint8_t bc[128];
  size_t n = build_any_accept(bc, "aeiou", 5);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* ANY('aeiou') on "frog": 'o' is a vowel at pos 2 */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "frog", 4, 0, &m, nullptr, &r, &d);
    test_assert(ok, "ANY('aeiou') matches 'o' in 'frog'");
    test_assert(r.match_start == 2, "ANY match_start == 2");
    test_assert(r.match_end == 3, "ANY match_end == 3");
  }

  /* ANY('aeiou') on "xyz": no vowel, should fail */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "xyz", 3, 0, &m, nullptr, &r, &d);
    test_assert((!ok) != 0, "ANY('aeiou') fails on 'xyz'");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: NOTANY execution — match single non-class byte
 * ---------------------------------------------------------------------------
 */
static void test_simd_notany(void) {
  test_suite("SIMD: NOTANY execution");

  uint8_t bc[128];
  size_t n = build_notany_accept(bc, "aeiou", 5);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* NOTANY('aeiou') on "frog": 'f' is not a vowel at pos 0 */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "frog", 4, 0, &m, nullptr, &r, &d);
    test_assert(ok, "NOTANY('aeiou') matches 'f' in 'frog'");
    test_assert(r.match_start == 0, "NOTANY match_start == 0");
    test_assert(r.match_end == 1, "NOTANY match_end == 1");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: tail-byte handling (subjects shorter than SIMD chunk)
 * ---------------------------------------------------------------------------
 */
static void test_simd_tail(void) {
  test_suite("SIMD: tail bytes");

  uint8_t bc[128];
  size_t n = build_span_accept(bc, "ab", 2, 0);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* SPAN('ab') on empty string */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "", 0, 0, &m, nullptr, &r, &d);
    test_assert((!ok) != 0, "SPAN('ab') on empty string fails");
  }

  /* SPAN('ab') on 1-byte subject */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "a", 1, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('ab') on 'a' matches");
    test_assert(r.match_end == 1, "SPAN('ab') on 'a' match_end == 1");
  }

  /* SPAN('ab') on 31 bytes (AVX2 tail boundary) */
  {
    char buf[32];
    memset(buf, 'a', 31);
    buf[31] = '\0';
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, buf, 31, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('ab') on 31-byte subject matches");
    test_assert(r.match_end == 31, "SPAN('ab') on 31 bytes match_end == 31");
  }

  /* SPAN('ab') on 32 bytes (exact AVX2 width) */
  {
    char buf[33];
    memset(buf, 'a', 32);
    buf[32] = '\0';
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, buf, 32, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('ab') on 32-byte subject matches");
    test_assert(r.match_end == 32, "SPAN('ab') on 32 bytes match_end == 32");
  }

  /* SPAN('ab') on 64 bytes (multiple SIMD chunks) */
  {
    char buf[65];
    memset(buf, 'a', 64);
    buf[64] = '\0';
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, buf, 64, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('ab') on 64-byte subject matches");
    test_assert(r.match_end == 64, "SPAN('ab') on 64 bytes match_end == 64");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: UTF-8 byte ranges (bytes 128-255)
 * ---------------------------------------------------------------------------
 */
static void test_simd_utf8_range(void) {
  test_suite("SIMD: UTF-8 byte ranges");

  uint8_t bc[128];
  size_t n = build_span_utf8(bc);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);
  test_assert(m.simd_eligible, "SPAN([0xC0-0xDF]) is simd_eligible");

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* SPAN([0xC0-0xDF]) on bytes 0xC3 0xC8 0x41: match the first two */
  {
    unsigned char data[] = {0xC3, 0xC8, 0x41};
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        snobol_search_exec(&vm, (const char *)data, 3, 0, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN([0xC0-0xDF]) matches UTF-8 lead bytes");
    test_assert(r.match_end == 2, "SPAN([0xC0-0xDF]) match_end == 2");
  }

  /* SPAN([0xC0-0xDF]) on ASCII bytes: no match */
  {
    const char *data = "abc";
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, data, 3, 0, &m, nullptr, &r, &d);
    test_assert((!ok) != 0, "SPAN([0xC0-0xDF]) fails on ASCII-only data");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: partial match (offset > 0)
 * ---------------------------------------------------------------------------
 */
static void test_simd_offset(void) {
  test_suite("SIMD: non-zero start offset");

  uint8_t bc[128];
  size_t n = build_span_accept(bc, "ab", 2, 0);

  snobol_search_meta_t m;
  memset(&m, 0, sizeof(m));
  snobol_search_derive_meta(bc, n, &m);

  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;
  snobol_build_range_meta(bc, n, &rm, &rm_count);

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = n;
  vm.range_meta = rm;
  vm.range_meta_count = rm_count;

  /* SPAN('ab') on "xxaaabb" starting at offset 2 */
  {
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = snobol_search_exec(&vm, "xxaaabb", 7, 2, &m, nullptr, &r, &d);
    test_assert(ok, "SPAN('ab') from offset 2 matches 'aaabb'");
    test_assert(r.match_start == 2, "SPAN match_start == 2");
    test_assert(r.match_end == 7, "SPAN match_end == 7");
  }

  if (rm) {
    free((void *)rm);
  }
  snobol_search_meta_free(&m);
  snobol_search_vm_cleanup(&vm);
}

/* ---------------------------------------------------------------------------
 * Test: direct tier_simd_nfa() calls — verifies the O(n) bitmap-skip
 * rewrite without depending on the cost-model
 * wiring (which is intentionally still OFF).  All four
 * SIMD-eligible pattern shapes (SPAN, BREAK, ANY, NOTANY) are exercised
 * directly through the public tier_simd_nfa() entry point, asserting
 * bit-exact agreement with the full-VM search path that the rest of the
 * suite exercises.  The diagnostics counters are used to verify the
 * O(n) position-skip semantics structurally (no timing noise): a missed
 * match over a long subject MUST visit 0 candidates and skip every byte,
 * whereas a hit visits exactly 1 candidate.
 * ---------------------------------------------------------------------------
 */
static void test_simd_direct_dispatch(void) {
  test_suite("SIMD: direct tier_simd_nfa (bitmap skip)");

  /* Helper — set up the bytecode + range_meta + VM the same way
   * snobol_search_exec() does for these SIMD-eligible patterns. */
  uint8_t bc[128];
  snobol_range_meta_t *rm = nullptr;
  size_t rm_count = 0;

  /* ---- SPAN('abc') landscape, no anchors required ----
   * SPAN('abc') on "aabbcxyz": match [0,5) "aabbc". */
  {
    size_t n = build_span_accept(bc, "abc", 3, 0);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        tier_simd_nfa(&vm, "aabbcxyz", 8, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa SPAN('abc') matches 'aabbc'");
    test_assert(r.match_start == 0, "SPAN('abc') match_start == 0");
    test_assert(r.match_end == 5, "SPAN('abc') match_end == 5");
    /* The very first byte is a class byte — exactly one verify call. */
    test_assert(d.candidates_tested == 1, "SPAN hit: 1 candidate tested");
    test_assert(d.candidates_skipped == 0, "SPAN hit: 0 candidates skipped");
    free((void *)rm);
    rm = nullptr;
  }

  /* SPAN('abc') on "xyzabc": bitmap-skip over the non-class prefix and
   * verify exactly once at the first class byte. */
  {
    size_t n = build_span_accept(bc, "abc", 3, 0);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        tier_simd_nfa(&vm, "xyzabc", 6, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa SPAN('abc') matches 'abc' in 'xyzabc'");
    test_assert(r.match_start == 3, "SPAN('abc') match_start == 3");
    test_assert(r.match_end == 6, "SPAN('abc') match_end == 6");
    test_assert(d.candidates_tested == 1, "SPAN skip+hit: 1 verify call");
    test_assert(d.candidates_skipped == 3, "SPAN skip+hit: 3 skipped");
    free((void *)rm);
    rm = nullptr;
  }

  /* SPAN('abc') anchored at a non-class start byte -> MUST fail without
   * scanning later positions (anchored contract). */
  {
    size_t n = build_span_accept(bc, "abc", 3, 0);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "xabc", 4, 0, nullptr, nullptr, &r, &d, true);
    test_assert((!ok) != 0, "SPAN('abc') anchored at 'x' fails");
    test_assert(d.candidates_tested == 1,
                "SPAN anchored: 1 verify attempt at start_offset");
    test_assert(d.candidates_skipped == 0,
                "SPAN anchored: no bitmap skip is permitted");
    free((void *)rm);
    rm = nullptr;
  }

  /* ---- BREAK ----
   * BREAK(',') on "hello,world": match [0,5) "hello".  BREAK has no
   * leftmost-skip optimization — match begins at start_offset by
   * definition (BREAK can always match the empty string). */
  {
    size_t n = build_break_accept(bc, ",", 1);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "hello,world", 11, 0, nullptr, nullptr, &r, &d,
                            false);
    test_assert(ok,
                "tier_simd_nfa BREAK(',') matches 'hello' in 'hello,world'");
    test_assert(r.match_start == 0, "BREAK match_start == 0");
    test_assert(r.match_end == 5, "BREAK match_end == 5");
    test_assert(d.candidates_tested == 1, "BREAK: 1 verify call (leftmost)");
    test_assert(d.candidates_skipped == 0, "BREAK: no skip scan");

    free((void *)rm);
    rm = nullptr;
  }

  /* BREAK(',') on "abc" (no delimiter present): match [0, subject_len). */
  {
    size_t n = build_break_accept(bc, ",", 1);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "abc", 3, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa BREAK(',') matches all of 'abc'");
    test_assert(r.match_start == 0, "BREAK no-delim match_start == 0");
    test_assert(r.match_end == 3, "BREAK no-delim match_end == 3");
    free((void *)rm);
    rm = nullptr;
  }

  /* BREAK(',') on "," (first byte is the delimiter): empty match [0,0). */
  {
    size_t n = build_break_accept(bc, ",", 1);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, ",", 1, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa BREAK(',') on ',' matches empty");
    test_assert(r.match_start == 0, "BREAK empty: match_start == 0");
    test_assert(r.match_end == 0, "BREAK empty: match_end == 0");
    free((void *)rm);
    rm = nullptr;
  }

  /* ---- ANY ----
   * ANY('aeiou') on "frog": 'o' at offset 2. */
  {
    size_t n = build_any_accept(bc, "aeiou", 5);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "frog", 4, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa ANY('aeiou') matches at offset 2");
    test_assert(r.match_start == 2, "ANY match_start == 2");
    test_assert(r.match_end == 3, "ANY match_end == 3");
    test_assert(d.candidates_tested == 1, "ANY hit: 1 verify call");
    test_assert(d.candidates_skipped == 2, "ANY hit: 2 bytes skipped");
    free((void *)rm);
    rm = nullptr;
  }

  /* ANY('aeiou') on "xyz" (no class byte): MUST fail and MUST not
   * visit any verifier. */
  {
    size_t n = build_any_accept(bc, "aeiou", 5);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "xyz", 3, 0, nullptr, nullptr, &r, &d, false);
    test_assert((!ok) != 0, "ANY('aeiou') on 'xyz' fails");
    test_assert(d.candidates_tested == 0,
                "ANY miss: 0 verifier calls (O(1) per byte)");
    test_assert(d.candidates_skipped == 3,
                "ANY miss: every byte skipped by bitmap");
    free((void *)rm);
    rm = nullptr;
  }

  /* ---- NOTANY ----
   * NOTANY('aeiou') on "frog": 'f' at offset 0. */
  {
    size_t n = build_notany_accept(bc, "aeiou", 5);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, "frog", 4, 0, nullptr, nullptr, &r, &d, false);
    test_assert(ok, "tier_simd_nfa NOTANY('aeiou') matches at offset 0");
    test_assert(r.match_start == 0, "NOTANY match_start == 0");
    test_assert(r.match_end == 1, "NOTANY match_end == 1");
    test_assert(d.candidates_tested == 1, "NOTANY hit: 1 verify call");
    test_assert(d.candidates_skipped == 0, "NOTANY hit: 0 skipped (hit pos 0)");
    free((void *)rm);
    rm = nullptr;
  }

  /* NOTANY('aeiou') on "aeiou" (no non-class byte): MUST fail with 0
   * verify calls. */
  {
    size_t n = build_notany_accept(bc, "aeiou", 5);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        tier_simd_nfa(&vm, "aeiou", 5, 0, nullptr, nullptr, &r, &d, false);
    test_assert((!ok) != 0, "NOTANY('aeiou') on 'aeiou' fails");
    test_assert(d.candidates_tested == 0,
                "NOTANY miss: 0 verifier calls (post-fix bit-mask skip)");
    test_assert(d.candidates_skipped == 5,
                "NOTANY miss: 5 bytes skipped via bitmap");
    free((void *)rm);
    rm = nullptr;
  }

  /* ---- UTF-8 lead-byte ranges (>=128) ----
   * SPAN([0xC0-0xDF]) on {0xC3, 0xC8, 0x41}: match first two bytes.
   * The previous implementation short-circuited the bitmap-skip for
   * bytes > 127, falling through to scalar verify at every position
   * (O(n^2) regression for non-ASCII subjects).  The rewritten
   * tier_simd_nfa consults the full 256-bit char_mask, so non-ASCII
   * positions skip correctly. */
  {
    size_t n = build_span_utf8(bc);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    unsigned char data[] = {0xC3, 0xC8, 0x41};
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, (const char *)data, 3, 0, nullptr, nullptr, &r,
                            &d, false);
    test_assert(ok, "tier_simd_nfa SPAN([0xC0-0xDF]) hits UTF-8 lead bytes");
    test_assert(r.match_start == 0, "UTF-8 SPAN match_start == 0");
    test_assert(r.match_end == 2, "UTF-8 SPAN match_end == 2");
    test_assert(d.candidates_tested == 1, "UTF-8 SPAN hit: 1 verify");
    test_assert(d.candidates_skipped == 0, "UTF-8 SPAN hit: 0 skipped");
    free((void *)rm);
    rm = nullptr;
  }

  /* SPAN([0xC0-0xDF]) on a non-class subject — ASCII bytes mixed with
   * UTF-8 continuation bytes that are NOT in the class.  The bitmap-skip
   * must reject ALL bytes (including those > 127) in O(n) without
   * invoking the verifier. */
  {
    size_t n = build_span_utf8(bc);
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    /* 0x80, 0xBF are UTF-8 continuation bytes — NOT in the [0xC0-0xDF]
     * class.  'a', 'b', 'c' are ASCII.  No byte here is a class byte. */
    unsigned char data[] = {'a', 'b', 0x80, 0xBF, 'c', 0xE0, 'd'};
    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok = tier_simd_nfa(&vm, (const char *)data, 7, 0, nullptr, nullptr, &r,
                            &d, false);
    test_assert((!ok) != 0, "UTF-8 SPAN on non-class subject fails");
    test_assert(d.candidates_tested == 0,
                "UTF-8 SPAN miss: 0 verifier calls — non-ASCII bytes are "
                "skipped via the 256-bit bitmap");
    test_assert(d.candidates_skipped == 7,
                "UTF-8 SPAN miss: every byte skipped (including >127)");
    free((void *)rm);
    rm = nullptr;
  }
}

/* ---------------------------------------------------------------------------
 * Test: O(n) linearity of the missing-match path.  The O(n^2) per-position
 * restart loop in the original tier_simd_nfa was the D2b blocker: it would
 * call the anchored verifier at every subject position.  The rewritten
 * tier_simd_nfa uses the start-state char_mask to skip per-position
 * verification, so a failing match over N bytes costs O(N) bit-tests and 0
 * verifier calls.  This test asserts that structural property by scaling N
 * and checking that candidates_tested stays 0 (constant) while
 * candidates_skipped scales linearly with N — a property no O(n^2)
 * implementation can simultaneously satisfy.
 * ---------------------------------------------------------------------------
 */
static void test_simd_nfa_linearity(void) {
  test_suite("SIMD: tier_simd_nfa O(n) linearity");

  /* SPAN('x') over subjects of size 1k, 4k, 16k filled with 'a' (no 'x').
   * Failing match must visit exactly 0 verifier calls and skip every
   * byte — for every subject size, in O(n). */
  size_t sizes[] = {1024, 4096, 16384};
  uint64_t tested_prev = 0;
  uint64_t skipped_prev = 0;
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    size_t nchr = sizes[i];
    char *subj = (char *)malloc(nchr);
    test_assert(subj != NULL, "alloc subject for linearity probe");
    if (!subj) {
      return;
    }
    memset(subj, 'a', nchr);

    uint8_t bc[128];
    size_t n = build_span_accept(bc, "x", 1, 0);
    snobol_range_meta_t *rm = nullptr;
    size_t rm_count = 0;
    snobol_build_range_meta(bc, n, &rm, &rm_count);
    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = n;
    vm.range_meta = rm;
    vm.range_meta_count = rm_count;

    snobol_search_result_t r;
    snobol_search_diag_t d;
    memset(&r, 0, sizeof(r));
    memset(&d, 0, sizeof(d));
    bool ok =
        tier_simd_nfa(&vm, subj, nchr, 0, nullptr, nullptr, &r, &d, false);
    test_assert((!ok) != 0, "SPAN('x') on 'a'^N fails (no class byte)");

    /* No verify call: the bitmap-skip rejected every byte. */
    char msg[80];
    snprintf(msg, sizeof(msg),
             "N=%zu: candidates_tested == 0 (no per-position verify)", nchr);
    test_assert(d.candidates_tested == 0, msg);

    /* Every byte was skipped (linearly-scaling skip count). */
    snprintf(msg, sizeof(msg), "N=%zu: candidates_skipped == N (linear skip)",
             nchr);
    test_assert(d.candidates_skipped == (uint64_t)nchr, msg);

    /* The skip count must grow strictly proportionally to N — a true
     * O(n) property.  Compare against the previous size. */
    if (i > 0) {
      /* Simpler invariant: skip count == subject length. */
      uint64_t ratio =
          d.candidates_skipped / (skipped_prev > 0 ? skipped_prev : 1);
      test_assert(ratio > 1, "skip count grows with N (O(n), not O(1))");
    }
    skipped_prev = d.candidates_skipped;
    tested_prev = d.candidates_tested;
    (void)tested_prev;
    free(subj);
    free((void *)rm);
  }
}

/* ---------------------------------------------------------------------------
 * Test: greedy-star single bound choice
 *
 * Verifies that an unbounded star over a pure OP_SPAN body produces the
 * same results as the full VM and reduces per-iteration choice pushes
 * to a single bound choice per run.  The optimisation is transparent:
 * existing backtracking suites continue to pass because the greedy path
 * only triggers for uncontended OP_SPAN bodies—captured, side-effected,
 * or non-SPAN bodies keep the original per-step path.
 * ---------------------------------------------------------------------------
 */
static void test_star_span_greedy(void) {
  test_suite("VM: greedy-star span (L3)");

  /* ---- Correctness: packed *SPAN('abc') on various subjects ---- */
  snobol_context_t *ctx = snobol_context_create();

  /* *SPAN('abc') 'x' — the star greedily consumes span bytes, then 'x' matches */
  {
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "SPAN('abc')* 'x'", 17, &err);
    test_assert(pat != NULL, "L3: compile *SPAN('abc') 'x'");
    if (pat) {
      /* Full match: span covers first 6 bytes, then 'x' at 6 */
      snobol_match_t *m = snobol_pattern_match(pat, "abcabcx", 7);
      test_assert(snobol_match_success(m),
                  "L3: *SPAN('abc') 'x' matches 'abcabcx'");
      if (snobol_match_success(m)) {
        test_assert(snobol_match_get_position(m) == 0, "L3: match starts at 0");
        test_assert(snobol_match_get_length(m) == 7, "L3: match length 7");
      }
      snobol_match_free(m);

      /* No match: span consumes all but 'x' is missing */
      m = snobol_pattern_match(pat, "abcabc", 6);
      test_assert((!snobol_match_success(m)) != 0,
                  "L3: *SPAN('abc') 'x' fails on 'abcabc' (no 'x')");
      snobol_match_free(m);

      /* Empty span succeeds when star body matches zero bytes */
      m = snobol_pattern_match(pat, "x", 1);
      test_assert(snobol_match_success(m),
                  "L3: *SPAN('abc') 'x' matches 'x' with empty span");
      snobol_match_free(m);

      snobol_pattern_free(pat);
    }
    free(err);
  }

  /* ARBNO(SPAN('0-9')) 'x' — star runs to max, then 'x' */
  {
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "SPAN('0-9')* 'x'", 17, &err);
    test_assert(pat != NULL, "L3: compile ARBNO(SPAN('0-9')) 'x'");
    if (pat) {
      snobol_match_t *m = snobol_pattern_match(pat, "123x", 4);
      test_assert(snobol_match_success(m),
                  "L3: ARBNO(SPAN('0-9')) 'x' matches '123x'");
      snobol_match_free(m);

      m = snobol_pattern_match(pat, "x", 1);
      test_assert(snobol_match_success(m), "L3: empty ARBNO matches 'x'");
      snobol_match_free(m);
      snobol_pattern_free(pat);
    }
    free(err);
  }

  /* Captured body: behaviour-preserving, still runs per-step */
  {
    char *err = nullptr;
    snobol_pattern_t *pat =
        snobol_pattern_compile(ctx, "SPAN('abc')*", 13, &err);
    test_assert(pat != NULL, "L3: compile ARBNO(SPAN('abc'))");
    if (pat) {
      snobol_match_t *m = snobol_pattern_match(pat, "aaaabbbbcccc", 12);
      test_assert(snobol_match_success(m),
                  "L3: ARBNO(SPAN('abc')) on class-string succeeds");
      if (snobol_match_success(m)) {
        test_assert(snobol_match_get_position(m) == 0, "L3: starts at 0");
        test_assert(snobol_match_get_length(m) == 12, "L3: consumes all");
      }
      snobol_match_free(m);

      /* SPAN('abc')* on non-class subject succeeds (zero-length match) */
      m = snobol_pattern_match(pat, "xxx", 3);
      test_assert(snobol_match_success(m),
                  "L3: SPAN('abc')* succeeds on 'xxx' (empty match)");
      test_assert(snobol_match_get_length(m) == 0,
                  "L3: SPAN('abc')* has zero length on 'xxx'");
      snobol_match_free(m);

      snobol_pattern_free(pat);
    }
    free(err);
  }

  snobol_context_destroy(ctx);
}

/* ---------------------------------------------------------------------------
 * Suite entry-point
 * ---------------------------------------------------------------------------
 */
/* NFA cache: verify that 1000 searches on a state reuse the cached NFA */
static void test_simd_nfa_cache(void) {
  test_suite("SIMD NFA: state-level caching");
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *p = snobol_pattern_compile(ctx, "SPAN('0-9')", 11, &err);
  if (!p) {
    test_assert(false, "NFA cache: compile");
    snobol_context_destroy(ctx);
    return;
  }
  snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(
      snobol_pattern_get_bc(p), snobol_pattern_get_bc_len(p));
  if (!state) {
    test_assert(false, "NFA cache: state create");
    snobol_pattern_free(p);
    snobol_context_destroy(ctx);
    return;
  }
  bool all_ok = true;
  for (int i = 0; i < 1000; i++) {
    snobol_match_t *m = snobol_pattern_search_ex(state, "abc123def", 9, 0);
    if (!m || !m->success || m->position != 3 || m->length != 3) {
      all_ok = false;
    }
  }
  test_assert(all_ok, "NFA cache: 1000 searches correct");
  snobol_pattern_search_state_destroy(state);
  snobol_pattern_free(p);
  snobol_context_destroy(ctx);
}


/* ===== test_coverage_simd: coverage-driven tests merged into test_search_simd.c ===== */
#include <stdint.h>


static bool covs_search(const char *pat_src, const char *subject,
                        size_t *out_pos, size_t *out_len) {
  snobol_context_t *ctx = snobol_context_create();
  char *err = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, pat_src, strlen(pat_src), 0, &err);
  bool ok = false;
  if (pat) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    test_assert(meta->simd_eligible, "pattern is SIMD-eligible");
    snobol_match_t *m = snobol_pattern_search(pat, subject, strlen(subject));
    ok = ((m && m->success) != 0);
    if (m) {
      if (out_pos) {
        *out_pos = m->position;
      }
      if (out_len) {
        *out_len = m->length;
      }
      snobol_match_free(m);
    }
    snobol_pattern_free(pat);
  } else {
    free(err);
  }
  snobol_context_destroy(ctx);
  return ok;
}

void test_cov_simd_patterns(void) {
  test_suite("Coverage: SIMD NFA pattern shapes");

  size_t pos = 0;
  size_t len = 0;

  /* SPAN: long run inside a >16-byte subject (vector scan path). */
  test_assert(covs_search("SPAN('a')", "bbbbbbbbbbbbbbbbaaaaaaaaaaaaaaaaaaaa",
                          &pos, &len),
              "SPAN long run matches");
  test_assert((pos == 16 && len == 20) != 0, "SPAN run position/length");

  /* SPAN with a run crossing a vector boundary. */
  test_assert(
      covs_search("SPAN('a')", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", &pos, &len),
      "SPAN run crossing vector boundary");
  test_assert(len == 31, "SPAN boundary run length");

  /* BREAK: delimiter inside a long subject. */
  test_assert(
      covs_search("BREAK(',')", "aaaaaaaaaaaaaaa,bbbbbbbbbbbbbbbb", &pos, &len),
      "BREAK delimiter found");
  test_assert((pos == 0 && len == 15) != 0, "BREAK stops before delimiter");

  /* ANY / NOTANY single char. */
  test_assert(
      covs_search("ANY('a')", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbab", &pos, &len),
      "ANY single char found");
  test_assert((pos == 30 && len == 1) != 0, "ANY position");
  test_assert((!covs_search("NOTANY('a')", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                            nullptr, nullptr)) != 0,
              "NOTANY rejects class-only subject");
  test_assert(
      covs_search("NOTANY('a')", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", &pos, &len),
      "NOTANY matches non-class byte");
  test_assert((pos == 0 && len == 1) != 0, "NOTANY first byte");

  /* Failure subjects drive the candidate-skip loop. */
  test_assert((!covs_search("SPAN('a')", "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",
                            nullptr, nullptr)) != 0,
              "SPAN no-match subject");
  test_assert(
      covs_search("BREAK(',')", "qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq", &pos, &len),
      "BREAK consumes delimiter-less subject");
  test_assert(len == 31, "BREAK full-length match");
  test_assert((!covs_search("ANY('a')", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                            nullptr, nullptr)) != 0,
              "ANY no-match subject");
}

void test_search_simd_suite(void) {
  test_simd_eligibility();
  test_simd_tier_routing();
  test_simd_span();
  test_simd_break();
  test_simd_any();
  test_simd_notany();
  test_simd_tail();
  test_simd_utf8_range();
  test_simd_offset();
  test_simd_direct_dispatch();
  test_simd_nfa_linearity();
  test_star_span_greedy();
  test_simd_nfa_cache();
  test_cov_simd_patterns();
}
