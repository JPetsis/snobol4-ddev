/**
 * fuzz_oracle.c – Differential search-oracle libFuzzer target.
 *
 * Splits the fuzzer input into pattern bytes + subject bytes (same layout
 * as fuzz_vm.c), compiles the pattern, then runs BOTH the accelerated tier
 * dispatch and a reference per-offset vm_exec run on the same subject.
 *
 * Any disagreement in success, match position, or match length is a
 * finding: the target aborts (libFuzzer records it as a crash artifact
 * containing the reproducer input).  This turns the fuzzer from a
 * crash-only harness into a wrong-answer finder for search-engine
 * accelerations and compile-time metadata.
 *
 * To keep the reference cheap, the subject is capped at 256 bytes and
 * patterns containing group repetition (whose reference run is
 * exponential, e.g. `('ab')*`) are limited to 16-byte subjects.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Struct-layout parity: the core library is always compiled with
 * SNOBOL_PROFILE (core/CMakeLists.txt), which adds a `profile` field to
 * the VM struct.  Callers that allocate a VM on the stack MUST define
 * SNOBOL_PROFILE before including vm.h, or vm_exec's profile memset
 * writes past the end of the struct. */
#define SNOBOL_PROFILE 1
#include "../../core/include/snobol/snobol.h"
#include "../../core/include/snobol/vm.h"

#define FUZZ_MAX_SUBJECT 256

/* Repetition patterns are exponential in the per-offset vm_exec reference;
 * skip the reference (and the comparison) for them on long subjects. */
static bool fuzz_reference_bounded(const uint8_t *data, size_t len) {
  for (size_t i = 0; i + 1 < len; i++) {
    if ((data[i] == '*' || data[i] == '+') && data[i + 1] == ')')
      return true;
  }
  return false;
}

/* Reference: first offset at which a full vm_exec run succeeds. */
static bool fuzz_ref_run(const uint8_t *bc, size_t bc_len,
                         const snobol_range_meta_t *range_meta,
                         size_t range_meta_count, const char *subject,
                         size_t sub_len, size_t *out_pos, size_t *out_len) {
  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.range_meta = range_meta;
  vm.range_meta_count = range_meta_count;
  snobol_buf out_buf;
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  for (size_t off = 0; off <= sub_len; off++) {
    vm.s = subject + off;
    vm.len = sub_len - off;
    vm.ip = 0;
    vm.pos = 0;
    vm.var_count = 0;
    vm.max_cap_used = 0;
    vm.max_counter_used = 0;
    vm.choices_top = 0;
    bool ok = vm_exec(&vm);
    if (ok) {
      *out_pos = off;
      *out_len = vm.pos;
      snobol_buf_free(&out_buf);
      return true;
    }
    if (vm.abort_flag)
      break;
  }
  snobol_buf_free(&out_buf);
  return false;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (size < 4)
    return 0;

  /* Same pattern/subject split as fuzz_vm.c. */
  uint32_t split;
  if (size > 1024) {
    split = 256;
  } else {
    split = (uint32_t)(size / 2);
  }
  if (split == 0)
    split = 1;
  if (split >= size)
    split = (uint32_t)(size / 2);
  if (split == 0)
    return 0;

  size_t pat_len = split;
  size_t sub_len = size - split;
  if (sub_len > FUZZ_MAX_SUBJECT)
    sub_len = FUZZ_MAX_SUBJECT;

  /* Exponential-reference guard: repetition patterns on long subjects. */
  if (fuzz_reference_bounded(data, pat_len) && sub_len > 16)
    return 0;

  snobol_context_t *ctx = snobol_context_create();
  if (!ctx)
    return 0;

  char *error = NULL;
  snobol_pattern_t *pat =
      snobol_pattern_compile(ctx, (const char *)data, pat_len, &error);
  if (!pat) {
      free(error);
    snobol_context_destroy(ctx);
    return 0;
  }

  const uint8_t *bc = snobol_pattern_get_bc(pat);
  size_t bc_len = snobol_pattern_get_bc_len(pat);
  const snobol_range_meta_t *range = snobol_pattern_get_range_meta(pat, NULL);
  const char *subject = (const char *)(data + split);

  /* Tier dispatch. */
  snobol_match_t *m = snobol_pattern_search(pat, subject, sub_len);
  bool tier_ok = m && snobol_match_success(m);
  size_t tier_pos = tier_ok ? snobol_match_get_position(m) : 0;
  size_t tier_len = tier_ok ? snobol_match_get_length(m) : 0;

  /* Reference. */
  size_t ref_pos = 0, ref_len = 0;
  bool ref_ok = fuzz_ref_run(bc, bc_len, range, 0, subject, sub_len, &ref_pos,
                             &ref_len);

  bool disagree = (tier_ok != ref_ok) ||
                  (tier_ok && (tier_pos != ref_pos || tier_len != ref_len));
  if (disagree) {
    /* Finding: write the reproducer + a marker, then abort so the fuzzer
     * run fails loudly (CI captures the file as an artifact).  The target
     * writes its own artifact because libFuzzer's crash handler does not
     * always produce one on every platform. */
    static unsigned finding_no = 0;
    char path[64];
    snprintf(path, sizeof(path), "oracle-finding-%u.bin", finding_no++);
    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(data, 1, size, f);
      fclose(f);
    }
    fprintf(stderr,
            "ORACLE DISAGREEMENT: tier(%d,%zu,%zu) ref(%d,%zu,%zu) -> %s\n",
            (int)tier_ok, tier_pos, tier_len, (int)ref_ok, ref_pos, ref_len,
            path);
    abort();
  }

  snobol_match_free(m);
  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
  return 0;
}
