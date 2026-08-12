/**
 * @file search_simd.c
 * @brief SIMD-accelerated Thompson NFA engine
 *
 * Implements Tier 9 of the search execution pipeline: a SIMD Thompson NFA
 * engine that processes 32 bytes (AVX2) or 16 bytes (NEON) simultaneously
 * for character-class patterns (SPAN, BREAK, ANY, NOTANY) with ASCII-only
 * ranges.
 *
 * The engine works by:
 *  1. Pre-building NFA state transition bitmasks from the pattern bytecode
 *  2. Loading 32/16 subject bytes into a SIMD register
 *  3. Broadcasting each byte and AND'ing with state masks to test character
 *     class membership
 *  4. Updating the active state set using SIMD blend/shift operations
 *
 * A scalar reference implementation is provided for platforms without SIMD
 * and for handling tail bytes that don't fill a full SIMD register.
 */

#include "snobol/search.h"
#include "snobol/simd.h"
#include "snobol/snobol_internal.h"
#include "snobol/vm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration for fallback when NFA build fails */
bool tier_general_fallback(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored);

/* ---------------------------------------------------------------------------
 * NFA state bitmask representation
 *
 * Each NFA state is represented as a bit in a bitmask. The state set tracks
 * which NFA states are active at the current subject position.
 * ---------------------------------------------------------------------------
 */

/** Maximum NFA states tracked by the SIMD engine. */
enum { SIMD_NFA_MAX_STATES = 64 };

/** Bitmask type for NFA state sets. */
typedef uint64_t simd_nfa_state_t;

/** Holds the pre-computed transition data for one NFA state. */
typedef struct {
  uint64_t char_mask
      [4]; /**< 256-bit byte-level bitmap: bytes that advance this state */
  uint16_t next_state; /**< Next NFA state index on match */
  bool is_accept;      /**< True if this state is an accepting state */
  bool is_split;       /**< True if this is a split/epsilon state */
} simd_nfa_trans_t;

/** NFA compiled state for SIMD execution. */
typedef struct simd_nfa {
  simd_nfa_trans_t states[SIMD_NFA_MAX_STATES];
  uint16_t num_states;
  uint16_t start_state;
  bool is_span;  /**< True for SPAN-style (match consecutive class bytes) */
  bool is_break; /**< True for BREAK-style (match up to first non-class byte) */
  bool is_any;   /**< True for ANY/NOTANY (match single class byte) */
} simd_nfa_t;

/* ---------------------------------------------------------------------------
 * NFA bytecode walker helpers (reuse from search.c)
 *
 * These duplicate the helpers in search.c to avoid exposing them in a header.
 * TODO: refactor into a shared internal header.
 * ---------------------------------------------------------------------------
 */

static inline uint16_t simd_read_u16(const uint8_t *bc, size_t ip) {
  return ((uint16_t)bc[ip] << 8) | (uint16_t)bc[ip + 1];
}

/* ---------------------------------------------------------------------------
 * 3.1 simd_nfa_state_t definition (bitmask) — used via typedef above
 * 3.2 simd_nfa_build_masks — convert NFA bytecode to state transition bitmasks
 * 3.4 SIMD eligibility check
 * ---------------------------------------------------------------------------
 */

/**
 * Check whether a pattern is eligible for SIMD NFA execution.
 *
 * A pattern is SIMD-eligible when:
 *  1. The bytecode starts with a single character-class op (SPAN, BREAK, ANY,
 *     NOTANY) followed only by terminators (ACCEPT, FAIL, ABORT, SUCCEED)
 *     and no-ops
 *  2. No side effects, captures, literal strings, or backtracking
 *  3. Character classes may cover the full byte range (0-255), making the
 *     SIMD path UTF-8 safe
 */
bool check_simd_eligible(const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len < 2) {
    return false;
  }

  /* The SIMD NFA engine (`build_nfa_masks`) only supports patterns that start
   * with a single character-class op (SPAN, BREAK, ANY, NOTANY) followed by a
   * terminator (ACCEPT, FAIL, ABORT, SUCCEED).  Patterns with LEN, LIT, or
   * control flow (JMP, SPLIT, POS, RPOS) are not supported and must fall back
   * to the VM. */
  uint8_t op0 = bc[0];
  switch (op0) {
    case OP_SPAN:
    case OP_BREAK:
    case OP_ANY:
    case OP_NOTANY: break;
    default: return false;
  }

  /* Skip the first op and its arguments.
   * SPAN/BREAK/ANY/NOTANY: 1 (op) + 2 (set_id) = 3 bytes, but the range data
   * follows at variable offset referenced by set_id — we don't walk it here.
   * The remaining bytecode must be only terminators. */
  size_t ip = 0;
  uint8_t first_op = bc[ip++];
  (void)first_op;
  /* SPAN/BREAK: op + 2-byte set_id */
  ip += 2;

  /* The rest must be only terminators and no-ops.
   * Stop at the first terminator — any bytes after it are inline annotation
   * data (range entries, offset table), not bytecode to execute. */
  while (ip < bc_len) {
    uint8_t op = bc[ip++];
    switch (op) {
      case OP_ACCEPT:
      case OP_FAIL:
      case OP_ABORT:
      case OP_SUCCEED: return true; /* terminator found — pattern is valid */
      case OP_NOP: break;
      default: return false;
    }
  }
  return false; /* no terminator found */
}

/* ---------------------------------------------------------------------------
 * Build NFA transition masks from bytecode.
 *
 * Walks the bytecode and builds a state machine where each state has a
 * character-class bitmap (which bytes cause a transition) and a next-state
 * pointer.
 *
 * For SPAN patterns: the state loops on class bytes (self-transition) and
 * exits on non-class bytes (epsilon to next instruction).
 *
 * For BREAK patterns: the state advances through non-delimiter bytes and
 * exits on delimiter bytes.
 * ---------------------------------------------------------------------------
 */
bool build_nfa_masks(simd_nfa_t *nfa, const uint8_t *bc, size_t bc_len,
                     const VM *vm);

struct simd_nfa *build_nfa_masks_alloc(const uint8_t *bc, size_t bc_len,
                                       const VM *vm) {
  struct simd_nfa *nfa = (struct simd_nfa *)snobol_malloc(sizeof(simd_nfa_t));
  if (!nfa) {
    return nullptr;
  }
  if (!build_nfa_masks(nfa, bc, bc_len, vm)) {
    snobol_free(nfa);
    return nullptr;
  }
  return nfa;
}

bool build_nfa_masks(simd_nfa_t *nfa, const uint8_t *bc, size_t bc_len,
                     const VM *vm) {
  memset(nfa, 0, sizeof(*nfa));
  nfa->num_states = 0;
  nfa->start_state = 0;

  if (!bc || bc_len < 2) {
    return false;
  }

  size_t ip = 0;
  uint16_t state_id = 0;

  if (ip >= bc_len) {
    return false;
  }

  uint8_t op0 = bc[ip];
  uint16_t set_id;
  uint16_t count;
  uint16_t ci;
  const uint8_t *ranges;

  if (op0 == OP_SPAN && ip + 3 <= bc_len) {
    nfa->is_span = true;
    set_id = simd_read_u16(bc, ip + 1);
    ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    if (!ranges || count == 0) {
      return false;
    }
    /* State 0: on class byte → stay in state 0; on non-class → epsilon to
     * next instruction (state 1 = accept) */
    if (!ranges_to_full_bitmap(ranges, count, nfa->states[0].char_mask)) {
      return false;
    }
    nfa->states[0].next_state = 0; /* self-loop */
    nfa->states[0].is_split = false;
    nfa->states[0].is_accept = false;
    /* State 1: accept */
    nfa->states[1].is_accept = true;
    nfa->states[1].is_split = false;
    nfa->num_states = 2;

  } else if (op0 == OP_BREAK && ip + 3 <= bc_len) {
    nfa->is_break = true;
    set_id = simd_read_u16(bc, ip + 1);
    ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    if (!ranges || count == 0) {
      return false;
    }
    /* BREAK: advance on non-delimiter bytes.  Build the inverted bitmap. */
    uint64_t delim_mask[4];
    if (!ranges_to_full_bitmap(ranges, count, delim_mask)) {
      return false;
    }
    nfa->states[0].char_mask[0] = ~delim_mask[0];
    nfa->states[0].char_mask[1] = ~delim_mask[1];
    nfa->states[0].char_mask[2] = ~delim_mask[2];
    nfa->states[0].char_mask[3] = ~delim_mask[3];
    nfa->states[0].next_state = 0; /* self-loop on non-delimiter */
    nfa->states[0].is_accept = false;
    /* State 1: delimiter found → accept */
    nfa->states[1].is_accept = true;
    nfa->num_states = 2;

  } else if (op0 == OP_ANY && ip + 3 <= bc_len) {
    nfa->is_any = true;
    set_id = simd_read_u16(bc, ip + 1);
    ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    if (!ranges || count == 0) {
      return false;
    }
    if (!ranges_to_full_bitmap(ranges, count, nfa->states[0].char_mask)) {
      return false;
    }
    nfa->states[0].next_state = 1; /* advance to accept on match */
    nfa->states[0].is_accept = false;
    nfa->states[1].is_accept = true;
    nfa->num_states = 2;

  } else if (op0 == OP_NOTANY && ip + 3 <= bc_len) {
    nfa->is_any = true;
    set_id = simd_read_u16(bc, ip + 1);
    ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    if (!ranges || count == 0) {
      return false;
    }
    uint64_t class_bits[4];
    if (!ranges_to_full_bitmap(ranges, count, class_bits)) {
      return false;
    }
    nfa->states[0].char_mask[0] = ~class_bits[0];
    nfa->states[0].char_mask[1] = ~class_bits[1];
    nfa->states[0].char_mask[2] = ~class_bits[2];
    nfa->states[0].char_mask[3] = ~class_bits[3];
    nfa->states[0].next_state = 1;
    nfa->states[0].is_accept = false;
    nfa->states[1].is_accept = true;
    nfa->num_states = 2;

  } else {
    /* Alternation and other patterns — not directly supported yet */
    return false;
  }

  return true;
}

/* ---------------------------------------------------------------------------
 * 3.3 Scalar reference implementation
 *
 * Processes the subject 1 byte at a time, following NFA state transitions.
 * This is the reference implementation used for correctness verification
 * and as a fallback for tail bytes.
 * ---------------------------------------------------------------------------
 */
static bool simd_nfa_exec_scalar(const simd_nfa_t *nfa, const char *subject,
                                 size_t subject_len, size_t offset,
                                 snobol_search_result_t *out_result) {
  size_t start = offset;
  simd_nfa_state_t state = (simd_nfa_state_t)1 << nfa->start_state;

  while (offset < subject_len) {
    uint8_t byte = (uint8_t)subject[offset];

    /* Compute next state set */
    simd_nfa_state_t next = 0;
    for (uint16_t s = 0; s < nfa->num_states; s++) {
      if (!(state & ((simd_nfa_state_t)1 << s))) {
        continue;
      }
      if (nfa->states[s].is_accept) {
        next |= (simd_nfa_state_t)1 << s;
        continue;
      }
      if (nfa->states[s].is_split) {
        /* Epsilon transition: advance to next state */
        next |= (simd_nfa_state_t)1 << nfa->states[s].next_state;
        continue;
      }
      /* Check character class membership (256-bit bitmap) */
      unsigned idx = byte >> 6;
      bool in_class =
          ((nfa->states[s].char_mask[idx] >> (byte & 63)) & 1ULL) != 0U;

      if (in_class) {
        uint16_t ns = nfa->states[s].next_state;
        if (ns == s) {
          /* Self-loop: stay in this state */
          next |= (simd_nfa_state_t)1 << s;
        } else if (ns < nfa->num_states) {
          /* Transition to next state */
          next |= (simd_nfa_state_t)1 << ns;
        }
      }
    }

    state = next;
    offset++;

    /* Check if accepting state is reached */
    for (uint16_t s = 0; s < nfa->num_states; s++) {
      if ((state & ((simd_nfa_state_t)1 << s)) && nfa->states[s].is_accept) {
        out_result->success = true;
        out_result->match_start = start;
        out_result->match_end = offset;
        return true;
      }
    }

    /* If no states active, fail */
    if (state == 0) {
      break;
    }
  }

  /* Check if we can accept at end of subject (e.g., BREAK at position = len) */
  for (uint16_t s = 0; s < nfa->num_states; s++) {
    if ((state & ((simd_nfa_state_t)1 << s)) && nfa->states[s].is_accept) {
      out_result->success = true;
      out_result->match_start = start;
      out_result->match_end = offset;
      return true;
    }
  }

  /* SPAN matches at least one byte from the class.
   * When state == 0 a non-class byte was found and we've incremented
   * offset past it, so match_end = offset - 1 (position of the non-class
   * byte).  When state != 0 (end of all-class subject) match_end = offset.
   *
   * Require that at least one byte was actually processed (offset > start).
   * This prevents returning an empty match when the while loop never ran
   * (e.g. SPAN([0xC0-0xDF]) on ASCII-only data, or offset == subject_len). */
  if (nfa->is_span && offset > start && (state != 0 || offset > start + 1)) {
    size_t end = (state == 0) ? offset - 1 : offset;
    out_result->success = true;
    out_result->match_start = start;
    out_result->match_end = end;
    return true;
  }

  /* BREAK matches zero or more non-delimiter bytes ending at the first
   * delimiter byte (i.e. a byte NOT in the start state's char_mask, which
   * has been inverted to non-delimiters).  State 0 self-loops on non-
   * delimiter bytes; when a delimiter is read, the loop exits with
   * state == 0 and `offset` advanced one past the delimiter position.
   *
   * match_end:
   *   - delimiter hit:    offset - 1  (positions [start,delimiter))
   *   - end of subject:   offset      (positions [start,subject_len))
   *
   * BREAK may match the empty string at any position (when offset == start
   * after the loop exited without consuming a byte, e.g. the subject begins
   * with a delimiter, or the subject is empty).  Unlike SPAN, no minimum
   * match length is required. */
  if (nfa->is_break) {
    bool delim_hit =
        ((state == 0) && (offset != start) && (offset <= subject_len)) != 0;
    size_t end = (int)delim_hit ? offset - 1 : offset;
    out_result->success = true;
    out_result->match_start = start;
    out_result->match_end = end;
    return true;
  }

  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * 4/5. SIMD-accelerated implementations
 *
 * Per-candidate NFA verification using SIMD vector compare.  Each function
 * loads a SIMD-width window (32 bytes on AVX2, 16 on NEON), tests all bytes
 * in parallel against the NFA start-state's 256-bit char_mask via a 256-byte
 * membership look-up table, and finds the position where the class condition
 * changes (first non-class byte for SPAN; first delimiter byte for BREAK).
 * ANY and NOTARY delegate to the O(1) scalar check (single byte).
 *
 * The table is built once per invocation.  For a 256-byte table the build
 * cost is ~256 instruction cycles, amortised over the match-run length
 * (typically dozens to thousands of bytes).  When the run is shorter than
 * ~32 bytes the scalar fallback is used.
 * ---------------------------------------------------------------------------
 */


#if SNOBOL_HAS_AVX2
#include <immintrin.h>

/**
 * AVX2 implementation — process 32 bytes per window via vpcmpeqb + vpmovmskb.
 *
 * Builds a 256-byte class membership table from the NFA start state's
 * char_mask, then tests 32 bytes at a time: for each byte we compute
 * table[byte] where 1 = class member.  The first non-class byte for SPAN
 * (or first class byte for BREAK) is found via tzcnt on the inverted mask.
 * Tail bytes < 32 fall through to the scalar path.
 */
static bool simd_nfa_exec_avx2(const simd_nfa_t *nfa, const char *subject,
                               size_t subject_len, size_t offset,
                               snobol_search_result_t *out_result) {
  size_t start = offset;

  /* Build membership table from char_mask bitmap.  256 bytes for all
   * possible byte values (0x00-0xFF).  Each SPAN/BREAK iteration loads a
   * 16-byte sub-table and broadcasts it to both AVX2 lanes via
   * _mm256_broadcastsi128_si256. */
  const uint64_t *bmap = nfa->states[0].char_mask;
  alignas(32) uint8_t table[256];
  memset(table, 0, sizeof(table));
  for (int i = 0; i < 256; i++) {
    unsigned w = (unsigned)i >> 6;
    table[i] = (uint8_t)((bmap[w] >> ((unsigned)i & 63u)) & 1ull);
  }
  /* For ANY/NOTANY: single byte check, no SIMD benefit */
  if (nfa->is_any) {
    goto tail;
  }

  size_t i = offset;
  if (nfa->is_span) {
    /* SPAN: find first byte NOT in class */
    while (i + 32 <= subject_len) {
      __m256i data = _mm256_loadu_si256((const __m256i *)(subject + i));
      /* Compute table[byte] for each byte using vpshufb.
       * vpshufb(y, x) uses the low nibble of each x byte as an index into
       * y's 16 entries (bytes 0-15).  For bytes >= 16 it returns 0.
       * We split into high/low halves:
       *   lo_result = vpshufb(table_lo, data & 0x0F)
       *   hi_selector = (data >> 4) & 0x0F
       * Each hi_selector value selects a 16-byte block in the table.
       * For each of the 16 possible hi values we blend the lo result. */
      __m256i lo = _mm256_and_si256(data, _mm256_set1_epi8(0x0F));
      __m256i hi =
          _mm256_and_si256(_mm256_srli_epi32(data, 4), _mm256_set1_epi8(0x0F));
      /* Look up low nibble in each 16-byte sub-table and blend by hi value.
       * vpshufb operates on two independent 16-byte lanes, so we must
       * broadcast the 16-byte sub-table to both lanes — a plain 32-byte
       * load would put (g+1)'s entries in the high lane. */
      __m256i result = _mm256_setzero_si256();
      for (int g = 0; g < 16; g++) {
        __m256i sub = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)(table + g * 16)));
        __m256i tbl = _mm256_shuffle_epi8(sub, lo);
        __m256i mask = _mm256_cmpeq_epi8(hi, _mm256_set1_epi8((char)g));
        result = _mm256_blendv_epi8(result, tbl, mask);
      }
      /* result byte is 1 for class byte, 0 for non-class.
       * For SPAN we want the first 0: compute bitmask of inverted result. */
      int class_mask =
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(result, _mm256_set1_epi8(1)));
      int non_class_mask = (~class_mask) & 0xFFFFFFFF;
      if (non_class_mask) {
        unsigned first = (unsigned)__builtin_ctz(non_class_mask);
        i += first;
        goto found_span_match;
      }
      i += 32;
    }
  } else if (nfa->is_break) {
    /* BREAK: find first byte IN class (delimiter) */
    while (i + 32 <= subject_len) {
      __m256i data = _mm256_loadu_si256((const __m256i *)(subject + i));
      __m256i lo = _mm256_and_si256(data, _mm256_set1_epi8(0x0F));
      __m256i hi =
          _mm256_and_si256(_mm256_srli_epi32(data, 4), _mm256_set1_epi8(0x0F));
      __m256i result = _mm256_setzero_si256();
      for (int g = 0; g < 16; g++) {
        __m256i sub = _mm256_broadcastsi128_si256(
            _mm_loadu_si128((const __m128i *)(table + g * 16)));
        __m256i tbl = _mm256_shuffle_epi8(sub, lo);
        __m256i mask = _mm256_cmpeq_epi8(hi, _mm256_set1_epi8((char)g));
        result = _mm256_blendv_epi8(result, tbl, mask);
      }
      /* For BREAK, the table marks non-delimiter bytes as 1 — we want the
       * first 0 (delimiter byte).  Invert the movemask and find its lowest
       * set bit. */
      int non_delim =
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(result, _mm256_set1_epi8(1)));
      int delim_mask = (~non_delim) & 0xFFFFFFFF;
      if (delim_mask) {
        unsigned first = (unsigned)__builtin_ctz(delim_mask);
        i += first;
        goto found_break_match;
      }
      i += 32;
    }
  }

tail:
  /* Fall through to scalar for tail/non-optimized paths */
  return simd_nfa_exec_scalar(nfa, subject, subject_len, start, out_result);

found_span_match: {
  /* SPAN matched up to position i (first non-class byte) */
  if (i > start) {
    out_result->success = true;
    out_result->match_start = start;
    out_result->match_end = i;
    return true;
  }
  return false;
}

found_break_match: {
  /* BREAK matched up to position i (first delimiter byte).
   * When the match is empty (i == start) this is a valid zero-length match
   * at a delimiter.  When the block had no delimiter the vector loop would
   * not reach here — the j-loop below only fires when the table reported a
   * delimiter. */
  out_result->success = true;
  out_result->match_start = start;
  out_result->match_end = i;
  return true;
}
}
#endif /* SNOBOL_HAS_AVX2 */

#if SNOBOL_HAS_NEON
#include <arm_neon.h>

/**
 * NEON implementation — process 16 bytes per window via vld1q_u8 + table
 * lookup. Builds a 256-byte membership table and uses vtbl1q_u8 to classify
 * all bytes in parallel.
 */
static bool simd_nfa_exec_neon(const simd_nfa_t *nfa, const char *subject,
                               size_t subject_len, size_t offset,
                               snobol_search_result_t *out_result) {
  size_t start = offset;

  /* Build 256-byte membership table from char_mask bitmap */
  const uint64_t *bmap = nfa->states[0].char_mask;
  uint8_t table[256];
  for (int i = 0; i < 256; i++) {
    unsigned w = (unsigned)i >> 6;
    table[i] = (uint8_t)((bmap[w] >> ((unsigned)i & 63U)) & 1ULL);
  }

  /* For ANY/NOTANY: single byte check, no SIMD benefit */
  if (nfa->is_any) {
    goto tail;
  }

  size_t i = offset;
  if (nfa->is_span) {
    /* SPAN: find first byte NOT in class */
    while (i + 16 <= subject_len) {
      uint8x16_t data = vld1q_u8((const uint8_t *)(subject + i));
      uint8x16_t lo = vandq_u8(data, vdupq_n_u8(0x0F));
      uint8x16_t hi = vshrq_n_u8(data, 4);
      /* Classify each byte: lookup table[byte] */
      uint8x16_t result = vdupq_n_u8(0);
      for (int g = 0; g < 16; g++) {
        uint8x16_t sub_tbl = vld1q_u8(table + ((size_t)g * 16));
        uint8x16_t tbl_res = vqtbl1q_u8(sub_tbl, lo);
        uint8x16_t mask = vceqq_u8(hi, vdupq_n_u8((uint8_t)g));
        result = vbslq_u8(mask, tbl_res, result);
      }
      /* Find first byte where result == 0 (non-class).  Use min to detect. */
      uint8_t min_val = vminvq_u8(result);
      if (min_val == 0) {
        /* At least one non-class byte — find its position */
        for (int j = 0; j < 16; j++) {
          uint8_t d = ((const uint8_t *)subject)[i + j];
          unsigned ww = (unsigned)d >> 6;
          if (!((bmap[ww] >> ((unsigned)d & 63U)) & 1ULL)) {
            i += j;
            goto found_span_match;
          }
        }
      }
      i += 16;
    }
  } else if (nfa->is_break) {
    /* BREAK: find first byte IN class (delimiter) */
    while (i + 16 <= subject_len) {
      uint8x16_t data = vld1q_u8((const uint8_t *)(subject + i));
      uint8x16_t lo = vandq_u8(data, vdupq_n_u8(0x0F));
      uint8x16_t hi = vshrq_n_u8(data, 4);
      uint8x16_t result = vdupq_n_u8(0);
      for (int g = 0; g < 16; g++) {
        uint8x16_t sub_tbl = vld1q_u8(table + ((size_t)g * 16));
        uint8x16_t tbl_res = vqtbl1q_u8(sub_tbl, lo);
        uint8x16_t mask = vceqq_u8(hi, vdupq_n_u8((uint8_t)g));
        result = vbslq_u8(mask, tbl_res, result);
      }
      /* The table marks non-delimiter bytes as 1 — find the first byte where
       * the table is 0 (the delimiter).  A block of only non-delimiters
       * yields min_val == 1 and falls through to the scalar tail. */
      uint8_t min_val = vminvq_u8(result);
      if (min_val == 0) {
        for (int j = 0; j < 16; j++) {
          uint8_t d = ((const uint8_t *)subject)[i + j];
          unsigned ww = (unsigned)d >> 6;
          if (!((bmap[ww] >> ((unsigned)d & 63U)) & 1ULL)) {
            i += j;
            goto found_break_match;
          }
        }
      }
      i += 16;
    }
  }

tail:
  return simd_nfa_exec_scalar(nfa, subject, subject_len, start, out_result);

found_span_match: {
  if (i > start) {
    out_result->success = true;
    out_result->match_start = start;
    out_result->match_end = i;
    return true;
  }
  return false;
}

found_break_match: {
  out_result->success = true;
  out_result->match_start = start;
  out_result->match_end = i;
  return true;
}
}
#endif /* SNOBOL_HAS_NEON */

/* ---------------------------------------------------------------------------
 * 3.5 Tier handler — top-level entry point for Tier 9 (SIMD NFA).
 *
 * Multi-position O(n) scan with O(1) per-candidate scalar verification.
 *
 * build_nfa_masks() compiles every SIMD-eligible pattern (SPAN, BREAK, ANY,
 * NOTANY followed by a terminator) into a 2-state NFA whose start state
 * carries a 256-bit char_mask of the bytes it accepts.  We use that mask as
 * a position-skip bitmap over the subject: at each position we ask "can a
 * match begin here?" via one 64-bit bit-test, and only run the (still
 * O(1)) scalar NFA verifier at candidate positions.  Failing matches
 * therefore scale linearly in subject length — there is no per-position
 * restart loop and no per-position VM invocation.
 *
 * Pattern-type notes:
 *   * SPAN: char_mask = class bytes; skip non-class; verify consumes the
 *     run of class bytes and accepts (>= 1 byte).
 *   * ANY:  char_mask = class bytes; skip non-class; verify accepts one
 *     class byte.
 *   * NOTANY: char_mask = inverted class (non-class bytes); skip class
 *     bytes; verify accepts one non-class byte.
 *   * BREAK: char_mask = inverted delimiter (non-delimiter bytes); BREAK
 *     matches at EVERY subject position (including a zero-length match
 *     at a delimiter), so there are no positions to skip — the leftmost
 *     possible match starts at start_offset and one scalar verify call
 *     decides it.  This keeps BREAK O(1) (anchored) or O(1) (unanchored
 *     from start_offset).
 *
 * The 256-bit char_mask is consulted for every subject byte (0..255),
 * including non-ASCII bytes (UTF-8 lead/continuation bytes); the SIMD NFA
 * is byte-oriented and makes no UTF-8 assumptions, so position skipping is
 * exact for ASCII and non-ASCII subjects alike.
 *
 * The cost-model tier selector (select_tier_by_cost) does NOT yet route
 * to TIER_SIMD_NFA — the dispatch table only reaches this handler when
 * callers force meta->tier = TIER_SIMD_NFA. _activating the tier
 * (select_tier_by_cost case + real AVX2/NEON vector compare) is the
 * remainder of group 3 and is intentionally not done
 * here.
 * ---------------------------------------------------------------------------
 */
bool tier_simd_nfa(VM *vm, const char *subject, size_t subject_len,
                   size_t start_offset, const snobol_search_meta_t *meta,
                   const snobol_dfa_t *dfa, snobol_search_result_t *out_result,
                   snobol_search_diag_t *diag, bool anchored) {
  (void)diag;

  /* Use cached NFA if available (vm->simd_nfa is set by the state API).
   * For stateless one-shot calls, build on stack — no malloc, no leak. */
  simd_nfa_t nfa_stack;
  simd_nfa_t *nfa;
  if (vm->simd_nfa) {
    nfa = vm->simd_nfa;
  } else {
    nfa = &nfa_stack;
    if (!build_nfa_masks(nfa, vm->bc, vm->bc_len, vm)) {
      return tier_general_fallback(vm, subject, subject_len, start_offset, meta,
                                   dfa, out_result, diag, anchored);
    }
  }

  /* Select the platform-specific NFA verifier at compile time. */
#if SNOBOL_HAS_NEON
#define SIMD_NFA_VERIFY(nfa_p, subj, slen, off, tmp_p) \
  simd_nfa_exec_neon((nfa_p), (subj), (slen), (off), (tmp_p))
#elif SNOBOL_HAS_AVX2
#define SIMD_NFA_VERIFY(nfa_p, subj, slen, off, tmp_p) \
  simd_nfa_exec_avx2((nfa_p), (subj), (slen), (off), (tmp_p))
#else
#define SIMD_NFA_VERIFY(nfa_p, subj, slen, off, tmp_p) \
  simd_nfa_exec_scalar((nfa_p), (subj), (slen), (off), (tmp_p))
#endif

  /* Anchored: the only legal match position is start_offset — try the
   * verifier once.  No bitmap skip is permitted because the verifier may
   * legitimately fail at start_offset (e.g. SPAN over a non-class byte),
   * but skipping to later positions would violate the anchored contract. */
  if (anchored) {
    if (diag) {
      diag->candidates_tested++;
    }
    snobol_search_result_t tmp;
    if (SIMD_NFA_VERIFY(nfa, subject, subject_len, start_offset, &tmp)) {
      out_result->success = true;
      out_result->match_start = tmp.match_start;
      out_result->match_end = tmp.match_end;
      return true;
    }
    out_result->success = false;
    return false;
  }

  /* BREAK matches at every subject position (the empty match is always
   * available), so the leftmost match, anchored-or-not, starts at
   * start_offset.  Verify once and return; no scan loop is needed. */
  if (nfa->is_break) {
    if (diag) {
      diag->candidates_tested++;
    }
    snobol_search_result_t tmp;
    if (SIMD_NFA_VERIFY(nfa, subject, subject_len, start_offset, &tmp)) {
      out_result->success = true;
      out_result->match_start = tmp.match_start;
      out_result->match_end = tmp.match_end;
      return true;
    }
    out_result->success = false;
    return false;
  }

  /* SPAN / ANY / NOTANY: walk the subject at most once.  The start
   * state's char_mask is a 256-bit bitmap of the bytes that can begin a
   * match; we advance past every other byte with one bit-test (no
   * verifier call) and only invoke the O(1) (SIMD) NFA verifier at
   * candidate positions.  Total work per byte is O(1) -> overall O(n)
   * regardless of whether the pattern ultimately matches. */
  const uint64_t *bmap = nfa->states[0].char_mask;
  size_t offset = start_offset;
  while (offset < subject_len) {
    uint8_t c = (uint8_t)subject[offset];
    unsigned word = (unsigned)c >> 6; /* 0..3 -> index into uint64_t[4] */
    if (!((bmap[word] >> ((unsigned)c & 63U)) & 1ULL)) {
      /* This byte cannot start a match — advance without invoking the
       * NFA verifier. */
      if (diag) {
        diag->candidates_skipped++;
      }
      offset++;
      continue;
    }

    /* Candidate: verify at this position with the platform SIMD verifier. */
    if (diag) {
      diag->candidates_tested++;
    }
    snobol_search_result_t tmp;
    if (SIMD_NFA_VERIFY(nfa, subject, subject_len, offset, &tmp)) {
      out_result->success = true;
      out_result->match_start = tmp.match_start;
      out_result->match_end = tmp.match_end;
      return true;
    }

    /* Defensive: bitmap classified this byte as a candidate but the
     * verifier disagreed (only reachable for unexpected NFA shapes —
     * the 2-state masks we build never trigger this).  Step one byte
     * forward and continue scanning. */
    offset++;
  }

  out_result->success = false;
  return false;
#undef SIMD_NFA_VERIFY
}
