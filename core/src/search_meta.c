/**
 * @file search_meta.c
 * @brief Compile-time search metadata derivation and tier selection.
 *
 * Split out of search.c (see oss-readiness T4). Implements:
 *  - snobol_search_derive_meta(): literal-prefix / first-byte / candidate
 *    bitmap / BREAK-SPAN classification / automaton & search-VM eligibility.
 *  - The cost-model tier selection (select_tier_by_cost) consumed by the
 *    dispatcher in search_tiers.c.
 *  - snobol_search_meta_free(), snobol_search_dump_cost_model().
 *
 * Runtime tier handlers and the search-VM/DFA machinery live in
 * search_tiers.c; SIMD acceleration lives in search_simd.c.
 */

#include "snobol/search.h"
#include "snobol/snobol.h"
#include "snobol/snobol_attrs.h"
#include "snobol/snobol_internal.h"
#include "snobol/simd.h"
#include "snobol/vm.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "search_internal.h"

/* ---------------------------------------------------------------------------
 * Bytecode-level alternation-of-literals shared-prefix detector.
 *
 * Walks the SPLIT tree collecting every LIT(..)ACC branch.  The trie is
 * "bushy" (and worth using) when at least two alternatives share a leading
 * byte — that forces a branch point in the trie.  When every alternative's
 * first byte is distinct the alternatives share no prefix, the trie is
 * "flat", and the general VM is a better choice.  Conservative: any
 * malformed / non-literal branch yields true so we keep the trie path.
 * ---------------------------------------------------------------------------
 */
static bool alt_literals_share_prefix(const uint8_t *bc, size_t bc_len,
                                      size_t ip) {
  uint8_t first_bytes[64];
  size_t nl = 0;

  size_t stack[64];
  int sp = 0;
  stack[sp++] = ip;

  while (sp > 0) {
    size_t p = stack[--sp];
    if (p + 2 > bc_len) {
      return true;
    }
    uint8_t op = bc[p];
    if (op == OP_LIT) {
      if (p + 10 > bc_len) {
        return true;
      }
      uint32_t off = search_read_u32(bc, p + 1);
      uint32_t len = search_read_u32(bc, p + 5);
      if (off >= bc_len || off + len > bc_len) {
        return true;
      }
      if (len == 0) {
        return true; /* empty literal: conservative bushy */
      }
      if (nl < 64) {
        first_bytes[nl++] = bc[off];
      }
    } else if (op == OP_SPLIT) {
      if (p + 9 > bc_len) {
        return true;
      }
      uint32_t a = search_read_u32(bc, p + 1);
      uint32_t b = search_read_u32(bc, p + 5);
      if (a >= bc_len || b >= bc_len) {
        return true;
      }
      if (sp + 2 > 64) {
        return true;
      }
      stack[sp++] = b;
      stack[sp++] = a;
    } else {
      return true;
    }
  }

  for (size_t i = 0; i < nl; i++) {
    for (size_t j = i + 1; j < nl; j++) {
      if (first_bytes[i] == first_bytes[j]) {
        return true; /* shared leading byte -> bushy trie */
      }
    }
  }
  return false;
}

/* ---------------------------------------------------------------------------
 * Alternation-of-literals shared-prefix extractor (P5).
 *
 * Walks the SPLIT/LIT tree (same shape check_alt_literals accepts) collecting
 * every leaf literal's byte string, then returns the longest common leading
 * prefix across all alternatives.  Caller uses this as a Boyer–Moore–Horspool
 * skip window: a match can only begin where the subject equals the shared
 * prefix, so failing positions advance by more than one byte.
 *
 * Returns the shared-prefix length (0 if none / not an alt-literal tree).
 * `prefix` must point to a buffer of at least SNOBOL_SEARCH_MAX_PREFIX bytes.
 * --------------------------------------------------------------------------- */
static size_t alt_literals_shared_prefix(const uint8_t *bc, size_t bc_len,
                                         uint8_t *prefix) {
  const uint8_t *lits[64];
  size_t lit_lens[64];
  size_t nl = 0;

  size_t stack[64];
  int sp = 0;
  stack[sp++] = 0;

  while (sp > 0) {
    size_t p = stack[--sp];
    if (p + 2 > bc_len) {
      return 0;
    }
    uint8_t op = bc[p];
    if (op == OP_LIT) {
      if (p + 10 > bc_len) {
        return 0;
      }
      uint32_t off = search_read_u32(bc, p + 1);
      uint32_t len = search_read_u32(bc, p + 5);
      if (off >= bc_len || off + len > bc_len) {
        return 0;
      }
      if (len == 0) {
        return 0; /* empty alternative: no useful prefix */
      }
      if (nl < 64) {
        lits[nl] = bc + off;
        lit_lens[nl] = len;
        nl++;
      }
    } else if (op == OP_SPLIT) {
      if (p + 9 > bc_len) {
        return 0;
      }
      uint32_t a = search_read_u32(bc, p + 1);
      uint32_t b = search_read_u32(bc, p + 5);
      if (a >= bc_len || b >= bc_len) {
        return 0;
      }
      if (sp + 2 > 64) {
        return 0;
      }
      stack[sp++] = b;
      stack[sp++] = a;
    } else {
      return 0;
    }
  }

  if (nl < 2) {
    return 0; /* need at least two alternatives to be an alternation */
  }

  /* Longest common prefix across all collected literals. */
  size_t max_len = lit_lens[0] < SNOBOL_SEARCH_MAX_PREFIX
                       ? lit_lens[0]
                       : SNOBOL_SEARCH_MAX_PREFIX;
  size_t shared = 0;
  for (size_t i = 0; i < max_len; i++) {
    uint8_t b = lits[0][i];
    bool all_same = true;
    for (size_t j = 1; j < nl; j++) {
      if (i >= lit_lens[j] || lits[j][i] != b) {
        all_same = false;
        break;
      }
    }
    if (!all_same) {
      break;
    }
    prefix[shared++] = b;
  }
  return shared;
}


/* ---------------------------------------------------------------------------
 * Start-byte bitmap helpers
 *
 * A 256-bit bitmap: bit i set → pattern CAN match starting with byte i.
 * Inspired by PCRE2's start_bits study data.
 * ---------------------------------------------------------------------------
 */

static inline void bitmap256_set(uint8_t bm[32], uint8_t b) {
  bm[b >> 3] |= (uint8_t)(1U << (b & 7));
}
static inline void bitmap256_set_all(uint8_t bm[32]) {
  memset(bm, 0xFF, 32);
}

/**
 * Compute start-byte bitmap for bytecode rooted at `ip`.
 *
 * Walks forward through sequential ops; for SPLIT (alternation) unions both
 * branches' start bytes via an explicit work-stack (see compute_start_bitmap
 * below).  Bounded by a step counter to prevent infinite loops from cycles.
 *
 * @param bc       Bytecode buffer
 * @param bc_len   Bytecode length
 * @param ip       Current bytecode offset (pointing to an opcode)
 * @param bm       Output bitmap (caller must zero before first call)
 * @param depth    Recursion depth guard (pass 0)
 * @param tmp_vm   Temporary VM for charclass resolution (zeroed by caller)
 */
/* Pop the next pending alternation branch, or finish the walk if none.
 * NOTE: this must NOT use `continue` inside the do/while wrapper -- `continue`
 * would bind to the (trivial) do/while loop, not the outer walk loop, and
 * control would silently fall through to the next switch case.  Use a label
 * jump (sbm_next) that re-enters the outer while loop instead. */
#define SBM_DONE()              \
  do {                          \
    if (sp > 0) {               \
      ip = (size_t)stack[--sp]; \
      goto sbm_next;            \
    }                           \
    return;                     \
  } while (0)

static void compute_start_bitmap(const uint8_t *bc, size_t bc_len, size_t ip,
                                 uint8_t bm[32], int depth, VM *tmp_vm) {
  (void)depth;
  /* Explicit work-stack of pending branch targets.  OP_SPLIT's right branch
   * is walked iteratively (instead of recursing depth+1), so stack depth is
   * bounded by true nesting rather than the literal count.  This keeps the
   * start bitmap precise for large flat alternations, which the old depth>12
   * guard blanked to all-bits (defeating the Tier 8 bitmap skip). */
  uint32_t stack[2048];
  int sp = 0;
  size_t steps = 0;

  while (ip < bc_len) {
  sbm_next:
    if (++steps > (bc_len * 8) + 256) {
      bitmap256_set_all(bm);
      return;
    }
    uint8_t op = bc[ip]; /* ip points to opcode */
    switch (op) {

        /* ---- Terminal consuming ops: determine start bytes and stop ---- */
      case OP_LIT: {
        if (ip + 9 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint32_t lit_off = search_read_u32(bc, ip + 1);
        uint32_t lit_len = search_read_u32(bc, ip + 5);
        if (lit_off >= bc_len || lit_off + lit_len > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        if (lit_len == 0) {
          /* Empty literal: the pattern can start at ANY byte (the empty
           * branch contributes no start byte of its own). */
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        bitmap256_set(bm, bc[lit_off]);
        SBM_DONE();
      }

      case OP_ANY: {
        if (ip + 3 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *rng = get_ranges_ptr(tmp_vm, set_id, &count, &ci);
        if (rng) {
          uint64_t abm[2] = {0, 0};
          ranges_to_ascii_bitmap(rng, count, abm);
          for (int i = 0; i < 256; i++) {
            if (bitmap_test(abm, (unsigned)i)) {
              bitmap256_set(bm, (uint8_t)i);
            }
          }
        } else {
          bitmap256_set_all(bm);
        }
        SBM_DONE();
      }

      case OP_NOTANY: {
        if (ip + 3 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *rng = get_ranges_ptr(tmp_vm, set_id, &count, &ci);
        if (rng) {
          uint64_t abm[2] = {0, 0};
          ranges_to_ascii_bitmap(rng, count, abm);
          for (int i = 0; i < 256; i++) {
            if (!bitmap_test(abm, (unsigned)i)) {
              bitmap256_set(bm, (uint8_t)i);
            }
          }
        } else {
          bitmap256_set_all(bm);
        }
        SBM_DONE();
      }

      case OP_SPAN: {
        if (ip + 3 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *rng = get_ranges_ptr(tmp_vm, set_id, &count, &ci);
        if (rng) {
          uint64_t abm[2] = {0, 0};
          ranges_to_ascii_bitmap(rng, count, abm);
          for (int i = 0; i < 256; i++) {
            if (bitmap_test(abm, (unsigned)i)) {
              bitmap256_set(bm, (uint8_t)i);
            }
          }
        } else {
          bitmap256_set_all(bm);
        }
        SBM_DONE();
      }

      case OP_BREAK:
      case OP_BREAKX:
        /* BREAK can succeed at ANY position (zero-width at a delimiter or
       * at end-of-subject).  Conservatively allow all bytes. */
        bitmap256_set_all(bm);
        SBM_DONE();

      case OP_LEN:
      case OP_REM:
      case OP_BAL:
        /* LEN: any byte (consumes n arbitrary codepoints).
       * REM: any byte (consumes remainder).
       * BAL: any byte (balanced pair can start with any char). */
        bitmap256_set_all(bm);
        SBM_DONE();

      /* ---- Alternation: union of branch start bytes (iterate, don't recurse) ---- */
      case OP_SPLIT: {
        if (ip + 9 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint32_t a = search_read_u32(bc, ip + 1);
        uint32_t b = search_read_u32(bc, ip + 5);
        if (a >= bc_len || b >= bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        if (sp < 2048) {
          { stack[sp++] = a; /* defer left branch */ }
        } else {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        ip = (size_t)b; /* walk right branch iteratively */
        continue;
      }

      /* ---- Zero-width wrappers: skip to next instruction ---- */
      case OP_CAP_START:
      case OP_CAP_END:
        ip += 2; /* opcode + reg byte */
        continue;
      case OP_FENCE:
      case OP_NOP: ip += 1; continue;
      case OP_ANCHOR:
        ip += 2; /* opcode + type byte */
        continue;
      case OP_ASSIGN:
        if (ip + 4 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        ip += 4;
        continue;

      case OP_POS:
      case OP_RPOS:
      case OP_TAB:
      case OP_RTAB:
        if (ip + 5 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        ip += 5;
        continue;

      /* ---- Unconditional jump: follow target ---- */
      case OP_JMP: {
        if (ip + 5 > bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        uint32_t target = search_read_u32(bc, ip + 1);
        if (target >= bc_len) {
          bitmap256_set_all(bm);
          SBM_DONE();
        }
        ip = (size_t)target;
        continue;
      }

      /* ---- Unpredictable ops: set all bytes ---- */
      case OP_REPEAT_INIT:
      case OP_REPEAT_STEP:
      case OP_EVAL:
      case OP_DYNAMIC:
      case OP_DYNAMIC_DEF:
      case OP_TABLE_GET:
      case OP_TABLE_SET:
      case OP_ARRAY_GET:
      case OP_ARRAY_SET:
      case OP_EMIT_LITERAL:
      case OP_EMIT_CAPTURE:
      case OP_EMIT_EXPR:
      case OP_EMIT_TABLE:
      case OP_EMIT_FORMAT:
      case OP_GOTO:
      case OP_GOTO_F:
      case OP_LABEL: bitmap256_set_all(bm); SBM_DONE();

      /* ---- Terminals: no start bytes contributed ---- */
      case OP_ACCEPT:
      case OP_FAIL:
      case OP_ABORT:
      case OP_SUCCEED: SBM_DONE();

      default: bitmap256_set_all(bm); SBM_DONE();
    }
  }
}

/* ---------------------------------------------------------------------------
 * Minimum match length computation (PCRE2-style find_minlength)
 *
 * Walk bytecode from `ip` and compute a lower bound on subject bytes consumed
 * by a successful match.  Returns 0 when the bound is unknown (the skip loop
 * simply skips the minlength check).
 *
 * For alternation (SPLIT) we take the MIN of branches; FAIL branches are
 * treated as infinite (UINT32_MAX) so they don't affect the min.
 * Depth-limited for safety.
 * ---------------------------------------------------------------------------
 */

static uint32_t SNOBOL_PURE compute_minlength(const uint8_t *bc, size_t bc_len,
                                              size_t ip, int depth) {
  if (depth > 12) {
    return 0;
  }

  uint32_t len = 0;
  size_t steps = 0;

  while (ip < bc_len) {
    /* Cycle guard: a JMP loop (e.g. JMP(0) or LIT JMP(self)) must not spin
     * forever.  Mirrors compute_start_bitmap's step cap; linear bytecode
     * executes at most bc_len steps per invocation, so the cap only fires
     * on malformed cyclic input.  Bail conservatively (0) so the minlength
     * filter can never reject a real match. */
    if (++steps > (bc_len * 8) + 256) {
      return 0;
    }
    uint8_t op = bc[ip]; /* ip points to opcode */
    switch (op) {

      case OP_LIT: {
        if (ip + 9 > bc_len) {
          return 0;
        }
        uint32_t lit_len = search_read_u32(bc, ip + 5);
        len += lit_len;
        ip += 9 + lit_len;
        continue;
      }

      case OP_ANY:
      case OP_NOTANY:
        len += 1;
        if (ip + 3 > bc_len) {
          return len;
        }
        ip += 3;
        continue;

      case OP_SPAN:
        len += 1;
        if (ip + 3 > bc_len) {
          return len;
        }
        ip += 3;
        continue;

      case OP_BREAK:
      case OP_BREAKX:
        /* BREAK can consume 0 bytes (match at delimiter or end) */
        ip += 3;
        continue;

      case OP_LEN: {
        if (ip + 5 > bc_len) {
          return len;
        }
        uint32_t n = search_read_u32(bc, ip + 1);
        len += n;
        ip += 5;
        continue;
      }

      case OP_POS:
      case OP_RPOS:
      case OP_TAB:
      case OP_RTAB:
        if (ip + 5 > bc_len) {
          return len;
        }
        ip += 5;
        continue;

      case OP_ANCHOR: ip += 2; continue;

      case OP_CAP_START:
      case OP_CAP_END: ip += 2; continue;

      case OP_FENCE:
      case OP_NOP: ip += 1; continue;

      case OP_ASSIGN:
        if (ip + 4 > bc_len) {
          return len;
        }
        ip += 4;
        continue;

      case OP_SPLIT: {
        if (ip + 9 > bc_len) {
          return len;
        }
        uint32_t a = search_read_u32(bc, ip + 1);
        uint32_t b = search_read_u32(bc, ip + 5);
        if (a >= bc_len || b >= bc_len) {
          return len;
        }
        uint32_t ma = compute_minlength(bc, bc_len, (size_t)a, depth + 1);
        uint32_t mb = compute_minlength(bc, bc_len, (size_t)b, depth + 1);
        /* Treat FAIL (UINT32_MAX) branches as infinite */
        uint32_t branch_min;
        if (ma != UINT32_MAX && mb != UINT32_MAX) {
          branch_min = ma < mb ? ma : mb;
        } else if (ma != UINT32_MAX) {
          branch_min = ma;
        } else if (mb != UINT32_MAX) {
          branch_min = mb;
        } else {
          branch_min = 0;
        }
        len += branch_min;
        return len;
      }

      case OP_JMP: {
        if (ip + 5 > bc_len) {
          return len;
        }
        uint32_t target = search_read_u32(bc, ip + 1);
        if (target >= bc_len) {
          return len;
        }
        ip = (size_t)target;
        continue;
      }

      case OP_REPEAT_INIT:
        /* Lower bound = inner_min * min_rep.  We can't determine inner min
       * easily, so return 0 (conservative). */
        return 0;

      case OP_REPEAT_STEP: return len;

      case OP_BAL: return len + 2;


      case OP_REM: return len;

      case OP_ACCEPT:
      case OP_ABORT:
      case OP_SUCCEED: return len;

      case OP_FAIL:
        /* Dead path — return sentinel so SPLIT ignores this branch */
        return UINT32_MAX;

      case OP_EVAL:
      case OP_DYNAMIC:
      case OP_DYNAMIC_DEF:
      case OP_TABLE_GET:
      case OP_TABLE_SET:
      case OP_ARRAY_GET:
      case OP_ARRAY_SET:
      case OP_EMIT_LITERAL:
      case OP_EMIT_CAPTURE:
      case OP_EMIT_EXPR:
      case OP_EMIT_TABLE:
      case OP_EMIT_FORMAT:
      case OP_GOTO:
      case OP_GOTO_F:
      case OP_LABEL: return 0;

      default: return 0;
    }
  }

  return len;
}

/* ---------------------------------------------------------------------------
 * Automaton eligibility checker
 *
 * A pattern is automaton-eligible if it:
 *   1. Contains no EVAL, ASSIGN, EMIT_*, DYNAMIC, TABLE_GET/SET, GOTO*, LABEL
 * ops (side-effect-free during matching).
 *   2. Contains no BREAKX, BAL, or DYNAMIC_DEF (structurally local).
 *   3. All character classes reference only ASCII ranges (verified by the
 *      caller using the class bitmap).
 *   4. Total bytecode is short (< 512 bytes excluding charclass table).
 * ---------------------------------------------------------------------------
 */
static bool check_automaton_eligible(const uint8_t *bc, size_t bc_len) {
  if (bc_len > 512) {
    return false;
  }
  size_t ip = 0;
  while (ip < bc_len) {
    uint8_t op = bc[ip++];
    switch (op) {
      /* Side-effect ops — disqualify */
      case OP_EVAL:
      case OP_ASSIGN:
      case OP_EMIT_LITERAL:
      case OP_EMIT_CAPTURE:
      case OP_EMIT_EXPR:
      case OP_EMIT_TABLE:
      case OP_EMIT_FORMAT:
      case OP_DYNAMIC:
      case OP_DYNAMIC_DEF:
      case OP_TABLE_GET:
      case OP_TABLE_SET:
      case OP_GOTO:
      case OP_GOTO_F:
      case OP_LABEL:
      case OP_BREAKX: /* extended retry complicates backtracking-free execution */
      case OP_BAL: /* requires a stack */ return false;
      /* Simple opcode families — safe */
      case OP_ACCEPT:
      case OP_FAIL:
        return true; /* Short pattern: eligible if we hit terminal */
      case OP_JMP:
        if (ip + 4 > bc_len) {
          return false;
        }
        ip += 4;
        break;
      case OP_SPLIT:
        if (ip + 8 > bc_len) {
          return false;
        }
        ip += 8;
        break;
      case OP_LIT: {
        if (ip + 8 > bc_len) {
          return false;
        }
        /* off(u32) len(u32) */
        uint32_t lit_len = search_read_u32(bc, ip + 4);
        ip += 8 + lit_len;
        break;
      }
      case OP_ANY:
      case OP_NOTANY:
        if (ip + 2 > bc_len) {
          return false;
        }
        ip += 2;
        break;
      /* SPAN / BREAK: excluded from the DFA.  Their exit to the next
       * instruction is a zero-width CONDITIONAL (only when the class run
       * ends / a delimiter is found); the DFA builder encodes it as an
       * unconditional epsilon, which makes the automaton accept after the
       * FIRST class byte instead of the whole run (the same reason ANCHOR
       * and the position primitives are excluded). */
      case OP_SPAN:
      case OP_BREAK: return false;
      case OP_LEN:
        if (ip + 4 > bc_len) {
          return false;
        }
        ip += 4;
        break;
      /* Position-dependent zero-width ops: excluded from DFA because they
      * require runtime position constraint checking.  The search-VM
      * handles them correctly.  ANCHOR is included here because the DFA
      * builder treats it as an unconditional epsilon — the DFA cannot
      * enforce start/end position constraints. */
      case OP_POS:
      case OP_RPOS:
      case OP_TAB:
      case OP_RTAB:
      case OP_ANCHOR: return false;
      case OP_CAP_START:
      case OP_CAP_END:
      case OP_FENCE:
      case OP_REM:
      case OP_NOP:
      case OP_ABORT:
      case OP_SUCCEED:
        if (ip > bc_len) {
          return false;
        }
        break;
      case OP_REPEAT_INIT:
        if (ip + 13 > bc_len) {
          return false;
        }
        ip += 13;
        break;
      case OP_REPEAT_STEP:
        if (ip + 5 > bc_len) {
          return false;
        }
        ip += 5;
        break;
      default:
        /* Unknown opcode: conservative — not eligible */
        return false;
    }
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Alternation-of-literals checker
 *
 * Walks the SPLIT tree starting at `ip` and returns true when every leaf
 * branch is OP_LIT(…) followed by OP_ACCEPT — i.e. the pattern is a flat
 * alternation of literal strings (e.g. "abc" | "def" | "ghi").
 *
 * When true, the search runtime can use a trie-based multi-string matcher
 * (Tier 3a) instead of calling vm_exec at each candidate position.
 * ---------------------------------------------------------------------------
 */
/* True when the SPLIT/LIT alternation tree contains an empty (len 0)
 * literal branch. */
static bool SNOBOL_PURE alt_has_empty_branch(const uint8_t *bc, size_t bc_len) {
  size_t stack[64];
  int sp = 0;
  stack[sp++] = 0;
  while (sp > 0) {
    size_t ip = stack[--sp];
    if (ip + 2 > bc_len) {
      return false;
    }
    uint8_t op = bc[ip];
    if (op == OP_LIT) {
      if (ip + 10 > bc_len) {
        return false;
      }
      if (search_read_u32(bc, ip + 5) == 0) {
        return true;
      }
    } else if (op == OP_SPLIT) {
      if (ip + 9 > bc_len) {
        return false;
      }
      uint32_t a = search_read_u32(bc, ip + 1);
      uint32_t b = search_read_u32(bc, ip + 5);
      if (a >= bc_len || b >= bc_len) {
        return false;
      }
      stack[sp++] = b;
      stack[sp++] = a;
    } else {
      return false;
    }
  }
  return false;
}

static bool SNOBOL_PURE check_alt_literals(const uint8_t *bc, size_t bc_len,
                                           size_t ip, size_t *lit_bytes) {
  if (ip + 2 > bc_len) {
    return false;
  }
  uint8_t op = bc[ip];

  if (op == OP_LIT) {
    /* Leaf: LIT(off, len) whose literal data is emitted inline, followed
     * eventually by OP_ACCEPT.  The compiler emits the final alternative
     * as `LIT ... ACCEPT`, but every other alternative is `LIT ... JMP`
     * jumping to the shared ACCEPT.  Follow any trailing JMPs so both
     * shapes are accepted. */
    if (ip + 10 > bc_len) {
      return false;
    }
    uint32_t lit_len = search_read_u32(bc, ip + 5);
    if (ip + 9 + lit_len > bc_len) {
      return false;
    }
    /* Pool budget: the trie needs at most 1 + sum(len_i) nodes, so bound
     * the total literal bytes to keep every build inside the fixed pool.
     * Over-budget alternations fall through to the search-VM / general
     * tiers, which are correct. */
    *lit_bytes += lit_len;
    if (*lit_bytes > SNOBOL_AUTO_MAX_LIT_BYTES) {
      return false;
    }
    size_t cur = ip + 9 + lit_len;
    size_t guard = 0;
    while (cur < bc_len && bc[cur] == OP_JMP) {
      if (++guard > bc_len) {
        return false; /* cycle guard */
      }
      cur = (size_t)search_read_u32(bc, cur + 1);
    }
    if (cur >= bc_len || bc[cur] != OP_ACCEPT) {
      return false;
    }
    return true;
  }

  if (op == OP_SPLIT) {
    /* Branch node: recurse into both arms */
    if (ip + 9 > bc_len) {
      return false;
    }
    uint32_t a = search_read_u32(bc, ip + 1);
    uint32_t b = search_read_u32(bc, ip + 5);
    if (a >= bc_len || b >= bc_len) {
      return false;
    }
    return (check_alt_literals(bc, bc_len, a, lit_bytes) &&
            check_alt_literals(bc, bc_len, b, lit_bytes)) != 0;
  }

  return false;
}

/* ---------------------------------------------------------------------------
 * Literal-only checker
 *
 * Returns true when the bytecode represents a single literal match with no
 * side effects, captures, or output — i.e. the pattern is equivalent to
 * a plain substring search.  Zero-width assertions (NOP, FENCE, ANCHOR,
 * POS, RPOS) are allowed between the LIT and ACCEPT.
 *
 * When true, the search runtime can skip the VM entirely and use memmem()
 * (unanchored) or memcmp() (anchored) to find matches.
 * ---------------------------------------------------------------------------
 */
static bool check_literal_only(const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len < 2) {
    return false;
  }
  size_t ip = 0;

  /* Skip leading zero-width ops (NOP / FENCE only).  ANCHOR and the
   * position primitives (POS/RPOS/TAB/RTAB) are position-constrained and
   * the memmem/memcmp fast path cannot enforce them — a `^ 'abc'` pattern
   * must match only at position 0, not wherever the literal occurs. */
  while (ip < bc_len) {
    uint8_t op = bc[ip];
    if (op == OP_NOP || op == OP_FENCE) {
      ip++;
      continue;
    }
    break;
  }

  /* Must start with LIT */
  if (ip + 9 > bc_len) {
    return false;
  }
  if (bc[ip] != OP_LIT) {
    return false;
  }
  uint32_t lit_off = search_read_u32(bc, ip + 1);
  uint32_t lit_len = search_read_u32(bc, ip + 5);
  ip += 9;
  if (lit_off >= bc_len || lit_off + lit_len > bc_len || lit_len == 0) {
    return false;
  }
  ip += lit_len;

  /* Skip trailing zero-width ops (NOP / FENCE only — position-constrained
   * ops disqualify, see above). */
  while (ip < bc_len) {
    uint8_t op = bc[ip];
    if (op == OP_NOP || op == OP_FENCE) {
      ip++;
      continue;
    }
    break;
  }

  /* Must end with ACCEPT (possibly OP_ACCEPT = 0) */
  if (ip >= bc_len) {
    return false;
  }
  return bc[ip] == OP_ACCEPT;
}

/* ---------------------------------------------------------------------------
 * Search-VM eligibility checker
 *
 * Returns true when the pattern contains only opcodes from the safe set
 * supported by the lightweight search-VM.  This includes all pattern-match
 * primitives (LIT, LEN, ANY, NOTANY, SPAN, BREAK, POS, RPOS, TAB, RTAB,
 * ANCHOR, FENCE, NOP), control flow (JMP, SPLIT, REPEAT_INIT, REPEAT_STEP,
   * ACCEPT, FAIL, ABORT, SUCCEED), plus captures (OP_CAP_START / OP_CAP_END,
   * supported capture-aware since W1a).  It EXCLUDES side-effect ops (EVAL,
   * ASSIGN, EMIT_*), dynamic ops (DYNAMIC, DYNAMIC_DEF), table/array ops,
   * GOTO/GOTO_F/LABEL, BAL, REM, and BREAKX.
 *
 * Patterns that pass are eligible for the lightweight search-VM path
 * (Tier 6) which drops output buffer, capture tracking, and var tracking.
 * ---------------------------------------------------------------------------
 */
/* ---------------------------------------------------------------------------
 * bc_has_capture
 *
 * Walk the bytecode and report whether it contains any capture op
 * (OP_CAP_START / OP_CAP_END / OP_EMIT_CAPTURE).  Only TIER_SEARCH_VM (6) and
 * TIER_GENERAL (8) record captures, so this gates every non-capturing tier.
 *
 * Advances past each op's operands using fixed arity for the safe-set opcodes
 * (the only ones that can reach a fast tier).  Variable-length or non-safe ops
 * terminate the walk: such patterns are not fast-tier eligible and fall to
 * TIER_GENERAL (which already preserves captures), so a capture appearing past
 * an unknown op can never be misrouted to a capture-dropping tier.
 * ---------------------------------------------------------------------------
 */
static bool bc_has_capture(const uint8_t *bc, size_t bc_len) {
  size_t ip = 0;
  while (ip < bc_len) {
    uint8_t op = bc[ip];
    switch (op) {
      case OP_CAP_START:
      case OP_CAP_END:
      case OP_EMIT_CAPTURE: return true;
      case OP_ACCEPT:
      case OP_SUCCEED:
      case OP_ABORT:
        /* Program terminator — stop before any trailing class/label data so we
       * don't misinterpret tail bytes as capture opcodes.  Reaching a
       * terminator means no capture was seen on this path. */
        return false;
      case OP_FAIL:
      case OP_NOP:
      case OP_FENCE:
      case OP_REM:
      case OP_DYNAMIC: ip += 1; break;
      case OP_JMP:
      case OP_SPLIT:
      case OP_LIT:
      case OP_EMIT_LITERAL:
        ip += 9; /* op + u32 + u32 */
        break;
      case OP_ANY:
      case OP_NOTANY:
      case OP_SPAN:
      case OP_BREAK:
      case OP_BREAKX:
        ip += 3; /* op + u16 */
        break;
      case OP_ASSIGN:
      case OP_EVAL:
        ip += 4; /* op + u16 + u8 */
        break;
      case OP_LEN:
      case OP_POS:
      case OP_RPOS:
      case OP_TAB:
      case OP_RTAB:
        ip += 5; /* op + u32 */
        break;
      case OP_ANCHOR:
        ip += 2; /* op + u8 */
        break;
      case OP_REPEAT_INIT:
        ip += 14; /* op + u8 + u32 + u32 + u32 */
        break;
      case OP_REPEAT_STEP:
        ip += 6; /* op + u8 + u32 */
        break;
      default:
        /* Variable-length / non-safe op: stop. Pattern is not fast-tier
       * eligible, so TIER_GENERAL handles it (captures preserved). */
        return false;
    }
  }
  return false;
}

/* Conservative bytecode walk: report which register classes a pattern can
 * touch.  Mirrors bc_has_capture's arity table; unlike it, an op the walker
 * cannot classify marks EVERY class used (an unclassifiable op sits in a
 * region we must not assume register-free — under-classification would let
 * the restart loop zero too little and read stale stack state). */
uint8_t snobol_bc_register_classes(const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len < 2) {
    return SNBL_REG_CAPTURE | SNBL_REG_VARS | SNBL_REG_COUNTERS;
  }
  uint8_t classes = SNBL_REG_NONE;
  size_t ip = 0;
  while (ip < bc_len) {
    uint8_t op = bc[ip];
    switch (op) {
      case OP_CAP_START:
      case OP_CAP_END: classes |= SNBL_REG_CAPTURE; ip += 2; break;
      case OP_EMIT_CAPTURE: classes |= SNBL_REG_CAPTURE; ip += 2; break;
      case OP_ASSIGN: classes |= SNBL_REG_CAPTURE | SNBL_REG_VARS; ip += 4; break;
      case OP_REPEAT_INIT: classes |= SNBL_REG_COUNTERS; ip += 14; break;
      case OP_REPEAT_STEP: classes |= SNBL_REG_COUNTERS; ip += 6; break;
      case OP_ACCEPT:
      case OP_SUCCEED:
      case OP_ABORT:
        /* Program terminator — stop before trailing class/label data so we
         * don't misinterpret tail bytes as opcodes.  Reaching a terminator
         * means no register op was seen on this path. */
        return classes;
      case OP_FAIL:
      case OP_NOP:
      case OP_FENCE:
      case OP_REM:
      case OP_DYNAMIC: ip += 1; break;
      case OP_JMP:
        ip += 5; /* op + u32 */
        break;
      case OP_SPLIT:
        ip += 9; /* op + u32 + u32 */
        break;
      case OP_LIT:
      case OP_EMIT_LITERAL: {
        /* op + u32 offset + u32 len; when the offset points right after the
         * operands the literal bytes are inline and must be skipped over,
         * otherwise they sit in a shared pool elsewhere and the walk
         * continues at the next instruction. */
        if (ip + 9 > bc_len) {
          return SNBL_REG_CAPTURE | SNBL_REG_VARS | SNBL_REG_COUNTERS;
        }
        uint32_t off = ((uint32_t)bc[ip + 1] << 24) |
                       ((uint32_t)bc[ip + 2] << 16) |
                       ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
        uint32_t lit_len = ((uint32_t)bc[ip + 5] << 24) |
                           ((uint32_t)bc[ip + 6] << 16) |
                           ((uint32_t)bc[ip + 7] << 8) | (uint32_t)bc[ip + 8];
        ip += 9;
        if (off == ip) {
          ip += lit_len; /* skip inline literal bytes */
        }
        break;
      }
      case OP_ANY:
      case OP_NOTANY:
      case OP_SPAN:
      case OP_BREAK:
      case OP_BREAKX:
        ip += 3; /* op + u16 */
        break;
      case OP_EVAL:
        ip += 4; /* op + u16 + u8 */
        break;
      case OP_LEN:
      case OP_POS:
      case OP_RPOS:
      case OP_TAB:
      case OP_RTAB:
        ip += 5; /* op + u32 */
        break;
      case OP_ANCHOR:
        ip += 2; /* op + u8 */
        break;
      default:
        /* Variable-length / non-safe op: cannot advance or prove anything —
         * conservatively assume every register class may be touched. */
        return SNBL_REG_CAPTURE | SNBL_REG_VARS | SNBL_REG_COUNTERS;
    }
  }
  return classes;
}

static bool check_search_vm_eligible(const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len < 2) {
    return false;
  }
  size_t ip = 0;
  while (ip < bc_len) {
    uint8_t op = bc[ip++];
    switch (op) {
      /* Side-effect / complex ops — disqualify.  Note: OP_CAP_START / OP_CAP_END
   * ARE supported by the search-VM (capture-aware since W1a), so they are not
   * listed here.  OP_EMIT_CAPTURE is a side effect and is excluded. */
      case OP_EVAL:
      case OP_EMIT_LITERAL:
      case OP_EMIT_CAPTURE:
      case OP_EMIT_EXPR:
      case OP_EMIT_TABLE:
      case OP_EMIT_FORMAT:
      case OP_DYNAMIC:
      case OP_DYNAMIC_DEF:
      case OP_TABLE_GET:
      case OP_TABLE_SET:
      case OP_ARRAY_GET:
      case OP_ARRAY_SET:
      case OP_GOTO:
      case OP_GOTO_F:
      case OP_LABEL:
      case OP_BAL:
      case OP_REM: return false;
      /* Terminal — immediately eligible for this sub-pattern */
      case OP_ACCEPT:
      case OP_FAIL:
      case OP_ABORT:
      case OP_SUCCEED: return true;
      case OP_JMP:
        if (ip + 4 > bc_len) {
          return false;
        }
        ip += 4;
        break;
      case OP_SPLIT:
        if (ip + 8 > bc_len) {
          return false;
        }
        ip += 8;
        break;
      case OP_LIT: {
        if (ip + 8 > bc_len) {
          return false;
        }
        uint32_t lit_len = search_read_u32(bc, ip + 4);
        ip += 8 + lit_len;
        break;
      }
      case OP_ANY:
      case OP_NOTANY:
      case OP_SPAN:
      case OP_BREAK:
        if (ip + 2 > bc_len) {
          return false;
        }
        ip += 2;
        break;
      case OP_LEN:
      case OP_RPOS:
      case OP_RTAB:
      case OP_POS:
      case OP_TAB:
        if (ip + 4 > bc_len) {
          return false;
        }
        ip += 4;
        break;
      case OP_ANCHOR:
      case OP_FENCE:
      case OP_NOP:
        /* zero-width, no operands beyond opcode */
        break;
      case OP_REPEAT_INIT:
        if (ip + 13 > bc_len) {
          return false;
        }
        ip += 13;
        break;
      case OP_REPEAT_STEP:
        if (ip + 5 > bc_len) {
          return false;
        }
        ip += 5;
        break;
      /* Capture-aware ops now supported by search-VM */
      case OP_CAP_START:
      case OP_CAP_END:
        if (ip + 1 > bc_len) {
          return false;
        }
        ip += 1; /* uint8 register index */
        break;
      case OP_ASSIGN:
        if (ip + 3 > bc_len) {
          return false;
        }
        ip += 3; /* uint16 var index + uint8 cap register */
        break;
      case OP_BREAKX:
        if (ip + 2 > bc_len) {
          return false;
        }
        ip += 2; /* uint16 set_id */
        break;
      default: return false;
    }
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Fusion recognition
 *
 * Walks the bytecode and checks if the pattern is a concat of fusible ops
 * (LIT, SPAN, ANY, NOTANY, BREAK) with no captures, EVAL, or side effects.
 * If fusible, builds a segment list for the fusion executor.
 * ---------------------------------------------------------------------------
 */

static inline void fusion_bitmap_from_ascii(uint8_t bm[32],
                                            const uint64_t abm[2]) {
  memcpy(bm, abm, 16);
  memset(bm + 16, 0, 16);
}

static inline void fusion_bitmap_invert(uint8_t bm[32]) {
  for (int i = 0; i < 16; i++) {
    bm[i] = (uint8_t)~bm[i];
  }
  memset(bm + 16, 0xFF, 16);
}

/* ---------------------------------------------------------------------------
 * check_fusion_eligible: build a fused segment list for a pattern.
 *
 * Scans the pattern bytecode and records a sequence of adjacent fusible
 * operations — the side-effect-free, search-VM-eligible subset (LIT, SPAN,
 * BREAK, ANY, NOTANY, BREAKX, and bushy SPLIT-of-literals alternations) —
 * into a freshly allocated segment list.  When the whole pattern reduces to
 * such a run, the tier-10 fusion executor can match it with a single linear
 * pass over the subject instead of per-operation dispatch.
 *
 * Eligibility rules:
 *  - patterns with captures, or with any non-fusible opcode (ARB/BAL/EVAL/
 *    ASSIGN/position ops/jumps/…) are rejected with NULL
 *  - out-of-range or empty literal payloads are rejected
 *  - the segment count is capped at MAX_FUSION_SEGMENTS (alternation
 *    branches at MAX_FUSION_ALT)
 *
 * Every rejection path frees the partially-built list internally, so a
 * non-NULL return is the only resource the caller owns — release it with
 * snobol_fusion_free() when no longer needed.
 *
 * @param bc      Pattern bytecode
 * @param bc_len  Bytecode length
 * @param meta    Derived search metadata for the pattern
 * @return Newly allocated fusion segment list, or NULL when the pattern is
 *         not fusible (callers fall back to regular tier dispatch)
 * ---------------------------------------------------------------------------
 */
static snobol_fusion_t *check_fusion_eligible(
    const uint8_t *bc, size_t bc_len, const snobol_search_meta_t *meta) {
  if (!bc || bc_len < 2) {
    return nullptr;
  }
  if (meta->has_capture) {
    return nullptr;
  }

  snobol_fusion_t *fusion =
      (snobol_fusion_t *)snobol_calloc(1, sizeof(snobol_fusion_t));
  if (!fusion) {
    return nullptr;
  }
  fusion->count = 0;

  VM tmp_vm;
  memset(&tmp_vm, 0, sizeof(tmp_vm));
  tmp_vm.bc = bc;
  tmp_vm.bc_len = bc_len;

  size_t ip = 0;

  while (ip < bc_len) {
    uint8_t op = bc[ip];

    if (op == OP_NOP || op == OP_FENCE) {
      ip++;
      continue;
    }
    /* ANCHOR and the position primitives are position CONSTRAINTS that the
     * fusion engine cannot enforce (it walks fused segments only): a
     * pattern like `'a'$` must match only at the subject end, not wherever
     * 'a' occurs.  Exclude them — the search-VM enforces anchors. */
    if (op == OP_ANCHOR || op == OP_POS || op == OP_RPOS || op == OP_TAB ||
        op == OP_RTAB) {
      snobol_fusion_free(fusion);
      return nullptr;
    }

    if (op == OP_ACCEPT || op == OP_SUCCEED) {
      break;
    }

    if (op == OP_FAIL || op == OP_ABORT) {
      snobol_fusion_free(fusion);
      return nullptr;
    }

    if (fusion->count >= MAX_FUSION_SEGMENTS) {
      snobol_fusion_free(fusion);
      return nullptr;
    }

    snobol_fusion_segment_t *seg = &fusion->segs[fusion->count];

    switch (op) {
      case OP_LIT: {
        if (ip + 9 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint32_t lit_off = search_read_u32(bc, ip + 1);
        uint32_t lit_len = search_read_u32(bc, ip + 5);
        if (lit_off >= bc_len || lit_off + lit_len > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        if (lit_len == 0) {
          ip += 9;
          continue;
        }
        seg->type = FUSION_LIT;
        seg->lit.data = bc + lit_off;
        seg->lit.len = lit_len;
        ip += 9 + lit_len;
        fusion->count++;
        break;
      }

      case OP_SPAN: {
        if (ip + 3 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
        if (!ranges) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint64_t abm[2] = {0, 0};
        if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        seg->type = FUSION_RUN;
        fusion_bitmap_from_ascii(seg->run.bitmap, abm);
        seg->run.min = 1;
        ip += 3;
        fusion->count++;
        break;
      }

      case OP_ANY: {
        if (ip + 3 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
        if (!ranges) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint64_t abm[2] = {0, 0};
        if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        seg->type = FUSION_CHAR;
        fusion_bitmap_from_ascii(seg->chr.bitmap, abm);
        ip += 3;
        fusion->count++;
        break;
      }

      case OP_NOTANY: {
        if (ip + 3 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
        if (!ranges) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint64_t abm[2] = {0, 0};
        if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        seg->type = FUSION_CHAR;
        fusion_bitmap_from_ascii(seg->chr.bitmap, abm);
        fusion_bitmap_invert(seg->chr.bitmap);
        ip += 3;
        fusion->count++;
        break;
      }

      case OP_BREAK: {
        if (ip + 3 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint16_t set_id = search_read_u16(bc, ip + 1);
        uint16_t count = 0;
        uint16_t ci = 0;
        const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
        if (!ranges) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint64_t abm[2] = {0, 0};
        if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        seg->type = FUSION_RUN;
        fusion_bitmap_from_ascii(seg->run.bitmap, abm);
        fusion_bitmap_invert(seg->run.bitmap);
        seg->run.min = 0;
        ip += 3;
        fusion->count++;
        break;
      }

      case OP_SPLIT: {
        if (ip + 9 > bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }
        uint32_t branch_a = search_read_u32(bc, ip + 1);
        uint32_t branch_b = search_read_u32(bc, ip + 5);
        if (branch_a >= bc_len || branch_b >= bc_len) {
          snobol_fusion_free(fusion);
          return nullptr;
        }

        seg->type = FUSION_ALT;
        seg->alt.alt_count = 0;
        memset((void *)seg->alt.alts, 0, sizeof(seg->alt.alts));
        memset(seg->alt.alt_lens, 0, sizeof(seg->alt.alt_lens));
        fusion->count++;
        uint32_t seg_idx = fusion->count - 1;

        uint32_t branches[2] = {branch_a, branch_b};
        for (int b = 0; b < 2; b++) {
          uint32_t branch_ip = branches[b];
          snobol_fusion_segment_t *alt_segs =
              (snobol_fusion_segment_t *)snobol_calloc(
                  1, MAX_FUSION_SEGMENTS * sizeof(snobol_fusion_segment_t));
          if (!alt_segs) {
            snobol_fusion_free(fusion);
            return nullptr;
          }

          uint32_t alt_count = 0;
          size_t cur_ip = branch_ip;
          bool alt_valid = true;

          while (cur_ip < bc_len && alt_valid) {
            uint8_t alt_op = bc[cur_ip];

            if (alt_op == OP_NOP || alt_op == OP_FENCE) {
              cur_ip++;
              continue;
            }
            if (alt_op == OP_ACCEPT || alt_op == OP_SUCCEED ||
                alt_op == OP_JMP) {
              break;
            }
            if (alt_op == OP_FAIL || alt_op == OP_ABORT) {
              alt_valid = false;
              break;
            }
            if (alt_count >= MAX_FUSION_SEGMENTS) {
              alt_valid = false;
              break;
            }

            snobol_fusion_segment_t *alt_seg = &alt_segs[alt_count];

            switch (alt_op) {
              case OP_LIT: {
                if (cur_ip + 9 > bc_len) {
                  alt_valid = false;
                  break;
                }
                uint32_t lit_off = search_read_u32(bc, cur_ip + 1);
                uint32_t lit_len = search_read_u32(bc, cur_ip + 5);
                if (lit_off >= bc_len || lit_off + lit_len > bc_len) {
                  alt_valid = false;
                  break;
                }
                if (lit_len == 0) {
                  cur_ip += 9;
                  continue;
                }
                alt_seg->type = FUSION_LIT;
                alt_seg->lit.data = bc + lit_off;
                alt_seg->lit.len = lit_len;
                cur_ip += 9 + lit_len;
                alt_count++;
                break;
              }
              case OP_SPAN: {
                if (cur_ip + 3 > bc_len) {
                  alt_valid = false;
                  break;
                }
                uint16_t set_id = search_read_u16(bc, cur_ip + 1);
                uint16_t count = 0;
                uint16_t ci = 0;
                const uint8_t *ranges =
                    get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
                if (!ranges) {
                  alt_valid = false;
                  break;
                }
                uint64_t abm[2] = {0, 0};
                if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
                  alt_valid = false;
                  break;
                }
                alt_seg->type = FUSION_RUN;
                fusion_bitmap_from_ascii(alt_seg->run.bitmap, abm);
                alt_seg->run.min = 1;
                cur_ip += 3;
                alt_count++;
                break;
              }
              case OP_ANY: {
                if (cur_ip + 3 > bc_len) {
                  alt_valid = false;
                  break;
                }
                uint16_t set_id = search_read_u16(bc, cur_ip + 1);
                uint16_t count = 0;
                uint16_t ci = 0;
                const uint8_t *ranges =
                    get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
                if (!ranges) {
                  alt_valid = false;
                  break;
                }
                uint64_t abm[2] = {0, 0};
                if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
                  alt_valid = false;
                  break;
                }
                alt_seg->type = FUSION_CHAR;
                fusion_bitmap_from_ascii(alt_seg->chr.bitmap, abm);
                cur_ip += 3;
                alt_count++;
                break;
              }
              case OP_NOTANY: {
                if (cur_ip + 3 > bc_len) {
                  alt_valid = false;
                  break;
                }
                uint16_t set_id = search_read_u16(bc, cur_ip + 1);
                uint16_t count = 0;
                uint16_t ci = 0;
                const uint8_t *ranges =
                    get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
                if (!ranges) {
                  alt_valid = false;
                  break;
                }
                uint64_t abm[2] = {0, 0};
                if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
                  alt_valid = false;
                  break;
                }
                alt_seg->type = FUSION_CHAR;
                fusion_bitmap_from_ascii(alt_seg->chr.bitmap, abm);
                fusion_bitmap_invert(alt_seg->chr.bitmap);
                cur_ip += 3;
                alt_count++;
                break;
              }
              case OP_BREAK: {
                if (cur_ip + 3 > bc_len) {
                  alt_valid = false;
                  break;
                }
                uint16_t set_id = search_read_u16(bc, cur_ip + 1);
                uint16_t count = 0;
                uint16_t ci = 0;
                const uint8_t *ranges =
                    get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
                if (!ranges) {
                  alt_valid = false;
                  break;
                }
                uint64_t abm[2] = {0, 0};
                if (!ranges_to_ascii_bitmap(ranges, count, abm)) {
                  alt_valid = false;
                  break;
                }
                alt_seg->type = FUSION_RUN;
                fusion_bitmap_from_ascii(alt_seg->run.bitmap, abm);
                fusion_bitmap_invert(alt_seg->run.bitmap);
                alt_seg->run.min = 0;
                cur_ip += 3;
                alt_count++;
                break;
              }
              default: alt_valid = false; break;
            }
          }

          if (!alt_valid || alt_count == 0) {
            snobol_free(alt_segs);
            snobol_fusion_free(fusion);
            return nullptr;
          }

          if (seg->alt.alt_count < MAX_FUSION_ALT) {
            seg->alt.alts[seg->alt.alt_count] = alt_segs;
            seg->alt.alt_lens[seg->alt.alt_count] = alt_count;
            seg->alt.alt_count++;
          } else {
            snobol_free(alt_segs);
            snobol_fusion_free(fusion);
            return nullptr;
          }
        }

        if (seg->alt.alt_count < 2) {
          snobol_fusion_free(fusion);
          return nullptr;
        }

        ip += 9;
        fusion->count++;
        break;
      }

      default: snobol_fusion_free(fusion); return nullptr;
    }
  }

  if (fusion->count < 2) {
    snobol_fusion_free(fusion);
    return nullptr;
  }

  return fusion;
}

void snobol_fusion_free(snobol_fusion_t *fusion) {
  if (!fusion) {
    return;
  }
  for (uint32_t i = 0; i < fusion->count; i++) {
    if (fusion->segs[i].type == FUSION_ALT) {
      for (uint32_t j = 0; j < fusion->segs[i].alt.alt_count; j++) {
        if (fusion->segs[i].alt.alts[j]) {
          snobol_free(fusion->segs[i].alt.alts[j]);
        }
      }
    }
  }
  snobol_free(fusion);
}

/* ---------------------------------------------------------------------------
 * snobol_search_derive_meta
 *
 * Scans the compiled bytecode to extract search-acceleration hints:
 *  - Literal-prefix bytes for memmem/memchr candidate skipping
 *  - First-byte for memchr when prefix length is 1
 *  - ASCII candidate bitmap for alternation-of-single-chars
 *  - BREAK/SPAN/BREAKX root-op classification
 *  - Alternation-of-literals flag for trie matching
 *  - Automaton eligibility for the lightweight path
 *
 * The function is pure (read-only), fast, and language-agnostic.
 * ---------------------------------------------------------------------------
 */
void SNOBOL_HOT snobol_search_derive_meta(const uint8_t *bc, size_t bc_len,
                                          snobol_search_meta_t *out) {
  /* Zero everything */
  memset(out, 0, sizeof(*out));

  if (!bc || bc_len < 2) {
    return;
  }

  /* ---- Skip any leading cap/anchor ops that don't consume input ---- */
  size_t ip = 0;
  /* Walk past zero-width prefixes to find the first consuming opcode.
   * This allows patterns like ANCHOR(0) SPAN('0-9') or CAP_START(0) LIT("x")
   * to get BMH skip and literal-prefix classification. */
  while (ip < bc_len) {
    uint8_t peek = bc[ip];
    if (peek == OP_ANCHOR) {
      if (ip + 2 > bc_len) {
        break; /* truncated opcode: bail, leave meta unclassified */
      }
      ip += 2;
      continue;
    } /* op + type:u8 */
    if (peek == OP_POS || peek == OP_RPOS || peek == OP_TAB ||
        peek == OP_RTAB) {
      if (ip + 5 > bc_len) {
        break; /* truncated opcode: bail, leave meta unclassified */
      }
      ip += 5;
      continue;
    } /* op + target:u32 */
    if (peek == OP_NOP || peek == OP_FENCE) {
      ip++;
      continue;
    }
    break; /* first consuming opcode */
  }
  /* We peek at the first "real" consuming opcode to classify root behavior. */
  if (ip >= bc_len) {
    /* The whole pattern is zero-width (anchors, position primitives):
     * it matches empty at every position.  Classify it so the tier
     * selection below routes dispatch to a correct tier instead of
     * leaving the meta unclassified (tier 0 = break-scan would reject
     * every subject). */
    out->may_match_empty = true;
    out->always_consumes = false;
    ip = 0;
  }

  uint8_t op0 = bc[ip];

  /* OP_ACCEPT / OP_SUCCEED at the root (all-zero-width prefix, e.g. `^$`
   * or POS(0)): the pattern matches empty. */
  if (op0 == OP_ACCEPT || op0 == OP_SUCCEED) {
    out->may_match_empty = true;
    out->always_consumes = false;
  }

  /* OP_LIT: extract literal prefix bytes */
  if (op0 == OP_LIT && ip + 9 <= bc_len) {
    uint32_t lit_off = search_read_u32(bc, ip + 1);
    uint32_t lit_len = search_read_u32(bc, ip + 5);
    if (lit_off < bc_len && lit_off + lit_len <= bc_len && lit_len > 0) {
      size_t prefix_len = lit_len < SNOBOL_SEARCH_MAX_PREFIX
                              ? lit_len
                              : SNOBOL_SEARCH_MAX_PREFIX;
      memcpy(out->literal_prefix, bc + lit_off, prefix_len);
      out->literal_prefix_len = prefix_len;
      out->has_literal_prefix = true;
      out->has_first_byte = true;
      out->first_byte = bc[lit_off];
      out->always_consumes = (lit_len > 0);
      /* Not an empty-match pattern if there's a non-empty literal */
      out->may_match_empty = false;

      /* BMH skip table is allocated later (line ~1253) after all flags
       * are computed. Here we only record eligibility. */
      if (prefix_len >= 2) {
        out->has_bmh_skip = true;
        out->bmh_skip_len = prefix_len;
      }
    } else if (lit_off < bc_len && lit_off + lit_len <= bc_len &&
               lit_len == 0) {
      /* Empty literal at the root: matches empty at every position. */
      out->may_match_empty = true;
      out->always_consumes = false;
    }
  }

  /* OP_BREAK / OP_BREAKX: scan past delimiter set */
  else if ((op0 == OP_BREAK || op0 == OP_BREAKX) && ip + 3 <= bc_len) {
    uint16_t set_id = search_read_u16(bc, ip + 1);
    out->is_break_family = true;
    out->is_breakx = (op0 == OP_BREAKX);
    out->always_consumes = false; /* BREAK can match at end / consume 0 */
    out->may_match_empty = true;

    /* Extract ASCII bitmap for the character class */
    /* We need a minimal VM-like structure to call get_ranges_ptr */
    VM tmp_vm;
    memset(&tmp_vm, 0, sizeof(tmp_vm));
    tmp_vm.bc = bc;
    tmp_vm.bc_len = bc_len;
    uint16_t count = 0;
    uint16_t ci = 0;
    const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
    if (ranges) {
      out->ascii_class_only =
          ranges_to_ascii_bitmap(ranges, count, out->class_bitmap);
    }
  }

  /* OP_REPEAT_INIT: bounded repetition at the root */
  else if (op0 == OP_REPEAT_INIT && ip + 14 <= bc_len) {
    uint32_t min = search_read_u32(bc, ip + 2);
    uint32_t skip = search_read_u32(bc, ip + 10);
    if (min == 0 && skip < bc_len &&
        (bc[skip] == OP_ACCEPT || bc[skip] == OP_SUCCEED)) {
      /* Zero-iteration exit leads straight to ACCEPT: the pattern matches
       * empty (e.g. `($)*` — the anchor body is bypassed entirely). */
      out->may_match_empty = true;
      out->always_consumes = false;
    }
  }

  /* OP_SPAN: consume characters in set */
  else if (op0 == OP_SPAN && ip + 3 <= bc_len) {
    uint16_t set_id = search_read_u16(bc, ip + 1);
    out->is_span_family = true;
    out->always_consumes = true; /* SPAN requires at least 1 char */
    out->may_match_empty = false;

    VM tmp_vm;
    memset(&tmp_vm, 0, sizeof(tmp_vm));
    tmp_vm.bc = bc;
    tmp_vm.bc_len = bc_len;
    uint16_t count = 0;
    uint16_t ci = 0;
    const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
    if (ranges) {
      out->ascii_class_only =
          ranges_to_ascii_bitmap(ranges, count, out->class_bitmap);
    }
  }

  /* OP_ANY: fused single-char charclass (e.g. from SPLIT→ANY fusion pass).
   *
   * Build the candidate bitmap directly from the charclass ranges so that
   * snobol_search_exec() can route to the fast bitmap-accelerated Tier 3
   * path instead of calling vm_exec at every position (Tier 4/5).
   *
   * Also records `ascii_class_only` so the SIMD NFA tier (Tier 9) can decide
   * eligibility: ANY has codepoint semantics in the full VM but byte semantics
   * in the SIMD NFA, so non-ASCII charclasses (e.g. 'α' :i emitting an ANY
   * over UTF-8 byte sequences) must NOT route through the SIMD engine.
   */
  else if (op0 == OP_ANY && ip + 3 <= bc_len) {
    uint16_t set_id = search_read_u16(bc, ip + 1);
    VM tmp_vm;
    memset(&tmp_vm, 0, sizeof(tmp_vm));
    tmp_vm.bc = bc;
    tmp_vm.bc_len = bc_len;
    uint16_t count = 0;
    uint16_t ci = 0;
    const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
    if (ranges && count > 0) {
      /* Build 4×u64 ASCII bitmap from the charclass ranges */
      bool ascii_only =
          ranges_to_ascii_bitmap(ranges, count, out->candidate_bitmap);
      out->ascii_class_only = ascii_only;
      if (ascii_only) {
        out->has_candidate_bitmap = true;
        out->is_single_char_alt = true; /* route to bitmap_accelerated */
        out->always_consumes = true;
        out->may_match_empty = false;
      }
    }
  }

  /* OP_NOTANY: complement of OP_ANY.  The SIMD NFA inverts the class bitmap
   * in build_nfa_masks, so it shares the same ASCII-only eligibility gate:
   * for codepoint-wise NOTANY over non-ASCII charclasses (UTF-8 byte
   * sequences with codepoint > 255) we must keep the full VM path; for
   * ASCII classes the SIMD tier is correct.  Unlike OP_ANY we don't build
   * a candidate bitmap (TIER_BITMAP matches class bytes, not their
   * complement). */
  else if (op0 == OP_NOTANY && ip + 3 <= bc_len) {
    uint16_t set_id = search_read_u16(bc, ip + 1);
    VM tmp_vm;
    memset(&tmp_vm, 0, sizeof(tmp_vm));
    tmp_vm.bc = bc;
    tmp_vm.bc_len = bc_len;
    uint16_t count = 0;
    uint16_t ci = 0;
    const uint8_t *ranges = get_ranges_ptr(&tmp_vm, set_id, &count, &ci);
    if (ranges && count > 0) {
      uint64_t tmp[4];
      out->ascii_class_only = ranges_to_ascii_bitmap(ranges, count, tmp);
    }
    out->always_consumes = true;
    out->may_match_empty = false;
  }

  /* OP_SPLIT: try to detect single-char alternation
   *
   * Pattern structure for a two-alternative single-char alternation:
   *   SPLIT a b
   *   LIT off1 1 <byte1>    ← branch a
   *   ACCEPT
   *   LIT off2 1 <byte2>    ← branch b
   *   ACCEPT
   *
   * We walk the SPLIT tree up to SNOBOL_SEARCH_MAX_ALT branches, collecting
   * single-char literals.  If every branch is a 1-byte literal followed by
   * ACCEPT, we classify this as a single_char_alt and build a bitmap.
   */
  else if (op0 == OP_SPLIT && ip + 9 <= bc_len) {
    /* Simple two-branch case first */
    uint32_t branch_a = search_read_u32(bc, ip + 1);
    uint32_t branch_b = search_read_u32(bc, ip + 5);

    bool all_single = true;
    uint8_t alt_bytes[SNOBOL_SEARCH_MAX_ALT];
    size_t alt_count = 0;

/* Helper: check if bc[off..] is LIT(1-byte) ACCEPT */
#define CHECK_SINGLE_LIT(off)                        \
  do {                                               \
    if ((off) + 10 > bc_len) {                       \
      all_single = false;                            \
      break;                                         \
    }                                                \
    if (bc[(off)] != OP_LIT) {                       \
      all_single = false;                            \
      break;                                         \
    }                                                \
    uint32_t _off2 = search_read_u32(bc, (off) + 1); \
    uint32_t _len = search_read_u32(bc, (off) + 5);  \
    if (_len != 1) {                                 \
      all_single = false;                            \
      break;                                         \
    }                                                \
    if (_off2 >= bc_len) {                           \
      all_single = false;                            \
      break;                                         \
    }                                                \
    uint8_t _byte = bc[_off2];                       \
    if (bc[(off) + 9 + _len] != OP_ACCEPT) {         \
      all_single = false;                            \
      break;                                         \
    }                                                \
    if (alt_count < SNOBOL_SEARCH_MAX_ALT) {         \
      alt_bytes[alt_count++] = _byte;                \
    }                                                \
  } while (0)

    CHECK_SINGLE_LIT(branch_a);
    if (all_single) {
      CHECK_SINGLE_LIT(branch_b);
    }

#undef CHECK_SINGLE_LIT

    if (all_single && alt_count > 0) {
      out->is_single_char_alt = true;
      out->alt_count = alt_count;
      memcpy(out->alt_bytes, alt_bytes, alt_count);
      /* Build candidate bitmap */
      out->has_candidate_bitmap = true;
      memset(out->candidate_bitmap, 0, sizeof(out->candidate_bitmap));
      for (size_t i = 0; i < alt_count; i++) {
        uint8_t b = alt_bytes[i];
        if (b < 64) {
          out->candidate_bitmap[0] |= (1ULL << b);
        } else if (b < 128) {
          out->candidate_bitmap[1] |= (1ULL << (b - 64));
        }
      }
      out->has_first_byte = (alt_count == 1);
      if (out->has_first_byte) {
        out->first_byte = alt_bytes[0];
      }
      out->always_consumes = true;
      out->may_match_empty = false;
    }
  }

  /* ---- Alternation-of-literals detection ---- */
  /* Walk the SPLIT tree: every branch must be LIT(…) ACCEPT.  The walk is
   * bounded to the first 2048 bytes of bytecode — enough for alternations
   * with well over 100 literal branches while keeping derive_meta O(1)
   * relative to pathological bytecode sizes.  Patterns beyond the bound
   * fall through to the (correct) search-VM / general-VM tiers. */
  if (op0 == OP_SPLIT && bc_len >= 10) {
    size_t bc_remain = bc_len > 2048 ? 2048 : bc_len;
    size_t lit_bytes = 0;
    out->is_alt_literals = ((check_alt_literals(bc, bc_remain, 0, &lit_bytes) &&
                             lit_bytes <= SNOBOL_AUTO_MAX_LIT_BYTES) != 0);
    /* An empty branch makes the alternation match empty at any position. */
    if (out->is_alt_literals && alt_has_empty_branch(bc, bc_remain)) {
      out->may_match_empty = true;
    }
    /* Flat vs bushy: flat alternatives share no prefix and gain nothing
     * from the trie, so they are routed to TIER_GENERAL (which already has
     * start-bitmap + BMH + minlength acceleration).  This eliminates the
     * 125x regression on flat alternation patterns. */
    if (out->is_alt_literals) {
      out->is_alt_literals_flat =
          ((!alt_literals_share_prefix(bc, bc_remain, 0)) != 0);
    }

    /* P5: a shared leading prefix across the alternatives lets the per-offset
     * trial loop (TIER_GENERAL / TIER_AUTOMATON) skip failing positions by more
     * than one byte via a Boyer–Moore–Horspool window.  Populate the BMH
     * skip from the shared prefix.  We deliberately do NOT set
     * has_literal_prefix (which would misroute to TIER_PREFIX) — the shared
     * prefix is only used to build the skip table below. */
    if (out->is_alt_literals) {
      uint8_t sprefix[SNOBOL_SEARCH_MAX_PREFIX];
      size_t sp_len = alt_literals_shared_prefix(bc, bc_remain, sprefix);
      if (sp_len >= 1) {
        size_t plen = sp_len < SNOBOL_SEARCH_MAX_PREFIX
                          ? sp_len
                          : SNOBOL_SEARCH_MAX_PREFIX;
        memcpy(out->literal_prefix, sprefix, plen);
        out->literal_prefix_len = plen;
        out->has_bmh_skip = true;
        out->bmh_skip_len = plen;
      }
    }
  }

  /* ---- Automaton eligibility ---- */
  out->automaton_eligible = check_automaton_eligible(bc, bc_len);
  /* Further restrict: must also be ASCII-only for classes */
  if (out->automaton_eligible &&
      (out->is_break_family || out->is_span_family) && !out->ascii_class_only) {
    out->automaton_eligible = false;
  }

  /* ---- Literal-only detection ---- */
  out->is_literal_only = check_literal_only(bc, bc_len);

  /* ---- Search-VM eligibility ---- */
  out->search_vm_eligible = check_search_vm_eligible(bc, bc_len);

  /* ---- SIMD NFA eligibility ---- */
  out->simd_eligible = check_simd_eligible(bc, bc_len);

  /* ---- Fusion eligibility ---- */
  {
    snobol_fusion_t *f = check_fusion_eligible(bc, bc_len, out);
    if (f) {
      out->fusion_eligible = true;
      out->fusion = f;
    }
  }
  /* The SIMD NFA engine (search_simd.c) is byte-wise — its 256-bit char_mask
   * tests single bytes.  SPAN and BREAK are byte-wise in the full VM as well,
   * so the SIMD engine can run them for any byte-level charclass (including
   * non-ASCII byte ranges like [0xC0-0xDF]).  ANY and NOTANY are CODEPOINT-wise
   * in the full VM: a non-ASCII ANY('α') class stores UTF-8 codepoint ranges
   * (>255) and matches the whole multi-byte sequence; the SIMD engine would
   * match only the first byte.
   *
   * We DO NOT clamp simd_eligible here — the cost-model tier selector
   * (select_tier_by_cost) is the single source of truth for dispatch and
   * gates TIER_SIMD_NFA on (is_span_family || is_break_family ||
   * ascii_class_only).  Keeping simd_eligible structurally true preserves the
   * baseline automaton-override condition `(!meta->simd_eligible && ...)`
   * unchanged: with simd_eligible=true on a non-ASCII ANY pattern, the
   * DFA override stays off and the full VM (Tier 6/8) handles the codepoint
   * semantics — exactly the baseline behavior, which all 17 of the
   * Pattern: Case-Insensitive assertions exercise. */

  /* ---- Start-byte bitmap & minimum match length ---- */
  {
    VM bm_vm;
    memset(&bm_vm, 0, sizeof(bm_vm));
    bm_vm.bc = bc;
    bm_vm.bc_len = bc_len;
    memset(out->start_bitmap, 0, sizeof(out->start_bitmap));
    compute_start_bitmap(bc, bc_len, 0, out->start_bitmap, 0, &bm_vm);
    out->has_start_bitmap = true;
    /* A pattern that can match empty has no start byte — it can start at
     * ANY position, so the start-bitmap filter must not reject candidates
     * (e.g. an empty alternation branch matches subjects whose bytes are
     * not in any literal's start set). */
    if (out->may_match_empty) {
      bitmap256_set_all(out->start_bitmap);
    }
  }
  out->minlength = compute_minlength(bc, bc_len, 0, 0);

  /* ---- Pack boolean flags into flags bitfield ---- */
  out->flags = 0;
  if (out->has_literal_prefix) {
    out->flags |= META_HAS_LITERAL_PREFIX;
  }
  if (out->has_first_byte) {
    out->flags |= META_HAS_FIRST_BYTE;
  }
  if (out->has_candidate_bitmap) {
    out->flags |= META_HAS_CANDIDATE_BITMAP;
  }
  if (out->may_match_empty) {
    out->flags |= META_MAY_MATCH_EMPTY;
  }
  if (out->always_consumes) {
    out->flags |= META_ALWAYS_CONSUMES;
  }
  if (out->is_single_char_alt) {
    out->flags |= META_IS_SINGLE_CHAR_ALT;
  }
  if (out->is_break_family) {
    out->flags |= META_IS_BREAK_FAMILY;
  }
  if (out->is_span_family) {
    out->flags |= META_IS_SPAN_FAMILY;
  }
  if (out->is_breakx) {
    out->flags |= META_IS_BREAKX;
  }
  if (out->ascii_class_only) {
    out->flags |= META_ASCII_CLASS_ONLY;
  }
  if (out->has_start_bitmap) {
    out->flags |= META_HAS_START_BITMAP;
  }
  if (out->automaton_eligible) {
    out->flags |= META_AUTOMATON_ELIGIBLE;
  }
  if (out->is_alt_literals) {
    out->flags |= META_IS_ALT_LITERALS;
  }
  if (out->is_alt_literals_flat) {
    out->flags |= META_IS_ALT_LITERALS_FLAT;
  }
  if (out->has_bmh_skip) {
    out->flags |= META_HAS_BMH_SKIP;
  }
  if (out->is_literal_only) {
    out->flags |= META_IS_LITERAL_ONLY;
  }
  if (out->search_vm_eligible) {
    out->flags |= META_SEARCH_VM_ELIGIBLE;
  }
  if (out->simd_eligible) {
    out->flags |= META_SIMD_ELIGIBLE;
  }
  if (out->fusion_eligible) {
    out->flags |= META_FUSION_ELIGIBLE;
  }

  /* ---- Capture-aware tier gating ----
   * Only TIER_SEARCH_VM (6) and TIER_GENERAL (8) record captures.  Every other
   * tier (break/span scans, prefix, bitmap, alt-literal trie, automaton, SIMD
   * NFA) silently drops them, so a captured pattern must never be routed to
   * one.  check_simd_eligible already rejects captures (it inspects bc[0]),
   * but the structural family/alt/prefix/automaton flags below do not: without
   * this gate, e.g. CAP_START SPAN would be misclassified as a pure span scan
   * and its capture would be lost in search mode. */
  out->has_capture = bc_has_capture(bc, bc_len);
  /* D3: fine-grained register liveness for the restart loop's lazy init. */
  {
    uint8_t reg_classes = snobol_bc_register_classes(bc, bc_len);
    out->has_capture = out->has_capture ||
                       (reg_classes & SNBL_REG_CAPTURE) != 0;
    out->has_assign = (reg_classes & SNBL_REG_VARS) != 0;
    out->has_counter = (reg_classes & SNBL_REG_COUNTERS) != 0;
  }
  if (out->has_capture) {
    out->is_span_family = false;
    out->is_break_family = false;
    out->is_breakx = false;
    out->has_literal_prefix = false;
    out->has_first_byte = false;
    out->has_candidate_bitmap = false;
    out->is_single_char_alt = false;
    out->is_alt_literals = false;
    out->is_alt_literals_flat = false;
    out->automaton_eligible = false;
    out->simd_eligible = false;
    out->fusion_eligible = false;
  }

  /* ---- Compute tier index from flags ---- */
  if (out->is_break_family && out->ascii_class_only) {
    out->tier = TIER_BREAK_SCAN;
  } else if (out->is_span_family && out->ascii_class_only) {
    out->tier = TIER_SPAN_SCAN;
  } else if (out->is_literal_only) {
    out->tier = TIER_LITERAL;
  } else if (out->has_literal_prefix && out->literal_prefix_len > 0) {
    out->tier = TIER_PREFIX;
  } else if (out->has_candidate_bitmap && out->is_single_char_alt) {
    out->tier = TIER_BITMAP;
  } else if (out->is_alt_literals && !out->is_alt_literals_flat) {
    out->tier = TIER_ALT_LIT;
  } else if (
      out->is_alt_literals) { /* flat: trie (no minlength accel, but correct) */
    out->tier = TIER_ALT_LIT;
  } else if (out->fusion_eligible) {
    out->tier = TIER_FUSED_AUTOMATON;
  } else if (out->simd_eligible) {
    out->tier = TIER_SIMD_NFA;
  } else if (out->search_vm_eligible) {
    out->tier = TIER_SEARCH_VM;
  } else if (out->automaton_eligible) {
    out->tier = TIER_AUTOMATON;
  } else {
    out->tier = TIER_GENERAL;
  }

  /* ---- Required-byte pre-filter: find the last literal in the bytecode.
   * Forward scan: track the last OP_LIT encountered; if we see OP_SPLIT
   * we cancel (multiple alternative paths → no single required literal).
   * For ('a'+)+ 'b', the last literal is 'b' → pre-filter with memchr.
   * For SPAN/BREAK/ANY patterns there are no literals → no-op. ---- */
  {
    size_t scan = 0;
    uint8_t last_lit[SNOBOL_SEARCH_MAX_PREFIX];
    size_t last_lit_len = 0;
    size_t last_lit_off = 0;
    bool lit_bypassed = false;
    /* A SPLIT seen before ANY literal (entry alternation or loop-body
     * SPLIT) makes every later literal a possible branch — sticky. */
    bool split_seen_before_lit = false;
    /* A SPLIT branch that JMPs past the last literal makes it optional —
     * sticky, like split_seen_before_lit. */
    bool split_branch_bypass = false;
    /* Bytecode offset just past a min==0 REPEAT_INIT body: literals inside
     * [init, skip_target) can be bypassed by the zero-iteration skip edge.
     * Evaluated per literal, so a literal AFTER the loop is unaffected. */
    size_t skippable_loop_end = 0;
    while (scan < bc_len) {
      uint8_t op = bc[scan];
      /* The pattern's accepting point ends the linear scan: any LITs after
       * the first ACCEPT/SUCCEED are unreachable (e.g. charclass metadata
       * or other trailing data that can mimic literal instructions) and
       * must not contribute a "required" literal. */
      if (op == OP_ACCEPT || op == OP_SUCCEED || op == OP_ABORT) {
        break;
      }
      if (op == OP_LIT && scan + 9 <= bc_len) {
        last_lit_off = scan;
        uint32_t lit_off = search_read_u32(bc, scan + 1);
        uint32_t lit_len = search_read_u32(bc, scan + 5);
        if (lit_len > 0 && lit_off < bc_len && lit_off + lit_len <= bc_len) {
          last_lit_len = (size_t)lit_len < SNOBOL_SEARCH_MAX_PREFIX
                             ? (size_t)lit_len
                             : SNOBOL_SEARCH_MAX_PREFIX;
          memcpy(last_lit, bc + lit_off, last_lit_len);
          /* The newly recorded literal is bypassable when a SPLIT was seen
           * before any literal, when a SPLIT branch provably bypasses the
           * previous literal, or when it sits inside a min==0 loop body.
           * An EMPTY literal does not replace last_lit, so it must not
           * change the bypass state of the record it leaves in place. */
          lit_bypassed =
              ((split_seen_before_lit || split_branch_bypass ||
                (skippable_loop_end > 0 && scan < skippable_loop_end)) != 0);
        }
        scan += 9 + (size_t)lit_len;
        continue;
      }
      if (op == OP_SPLIT && scan + 9 <= bc_len) {
        /* A SPLIT seen before ANY literal (e.g. a leading alternation
         * 'a'|'b'|'c' or a loop body SPLIT) makes every later literal
         * optional: the literals the scan records afterwards may be branches
         * of this SPLIT, and the pattern can match without any of them.
         * Without this guard, the last branch's literal of a large leading
         * alternation was treated as REQUIRED and the prefilter wrongly
         * rejected subjects that matched any other branch. */
        if (last_lit_len == 0) {
          split_seen_before_lit = true;
          lit_bypassed = true;
          scan += 9;
          continue;
        }
        /* Check if any SPLIT branch bypasses the last literal.  Read both
         * branch targets.  If a branch's first instruction is LIT followed
         * by a JMP that jumps PAST the last literal, that branch skips it.
         * For 'a'|'b', branch A has LIT 'a' + JMP exit, and exit > last_lit
         * (LIT 'b'), so LIT 'b' is not required.  For ('a'+)+ 'b', the
         * SPLIT branches either go to the loop body (no JMP past 'b') or
         * to the exit (which IS 'b'), so 'b' is required. */
        uint32_t ta = search_read_u32(bc, scan + 1);
        uint32_t tb = search_read_u32(bc, scan + 5);
        if (last_lit_len > 0) {
          uint32_t check[2] = {ta, tb};
          for (int bi = 0; bi < 2; bi++) {
            uint32_t b_off = check[bi];
            /* Skip past the branch's first instruction (if LIT = 9+lit_len,
             * if JMP = 5, otherwise assume 1) */
            if (b_off >= bc_len) {
              continue;
            }
            uint8_t b_op = bc[b_off];
            size_t b_skip = 1;
            if (b_op == OP_LIT && b_off + 9 <= bc_len) {
              b_skip = 9 + (size_t)search_read_u32(bc, b_off + 5);
            } else if (b_op == OP_JMP) {
              b_skip = 5;
            }
            size_t jmp_off = b_off + b_skip;
            if (jmp_off + 5 <= bc_len && bc[jmp_off] == OP_JMP) {
              uint32_t jt = search_read_u32(bc, jmp_off + 1);
              if (jt > last_lit_off) {
                split_branch_bypass = true;
                lit_bypassed = true;
                break;
              }
            }
          }
        }
      }
      /* Advance by opcode size */
      size_t adv = 1;
      if (op == OP_SPLIT) {
        { adv = 9; }
      } else if (op == OP_REPEAT_INIT) {
        adv = 14;
        /* min==0 repetition: the skip edge (zero iterations) bypasses the
         * whole loop body, so literals inside [scan, skip_target) are not
         * required — the same reasoning as the SPLIT-before-literal guard.
         * Literals recorded after the loop's skip target are unaffected. */
        if (scan + 14 <= bc_len) {
          uint32_t min = search_read_u32(bc, scan + 2);
          uint32_t skip = search_read_u32(bc, scan + 10);
          if (min == 0 && skip < bc_len && skip > scan) {
            skippable_loop_end = skip;
          }
        }
      } else if (op == OP_REPEAT_STEP) {
        { adv = 6; }
      } else if (op == OP_JMP || op == OP_LEN || op == OP_POS ||
                 op == OP_RPOS || op == OP_TAB || op == OP_RTAB) {
        { adv = 5; }
      } else if (op == OP_ANCHOR || op == OP_CAP_START || op == OP_CAP_END) {
        { adv = 2; }
      } else if (op == OP_ANY || op == OP_NOTANY || op == OP_SPAN ||
                 op == OP_BREAK || op == OP_BREAKX) {
        { adv = 3; }
      } else if (op == OP_ASSIGN) {
        { adv = 4; }
      }
      scan += adv;
      if (adv == 0 || scan > bc_len) {
        break;
      }
    }
    /* Set required literal when it was found and is NOT bypassed by any
     * SPLIT branch (no alternative ending).  Also skip alt-literals
     * patterns (trie-based — the bytecode has no SPLIT). */
    if (!lit_bypassed && !out->is_alt_literals && last_lit_len > 0) {
      out->has_required_lit = true;
      snobol_meta_set_flag(out, META_HAS_REQUIRED_LIT);
      memcpy(out->required_lit, last_lit, last_lit_len);
      out->required_lit_len = last_lit_len;
    }
  }

  /* ---- Allocate BMH table separately (heap) ---- */
  if (out->has_bmh_skip && out->bmh_skip_len > 0) {
    out->bmh_skip = (uint8_t *)snobol_malloc(256);
    if (out->bmh_skip) {
      memset(out->bmh_skip, (int)out->bmh_skip_len, 256);
      /* Repopulate from literal prefix if available.  For a single literal
       * prefix this is gated on has_literal_prefix; for an alternation-of-
       * literals shared prefix (P5) has_literal_prefix stays false but the
       * shared prefix bytes were stored in literal_prefix for exactly this
       * purpose.  In both cases the prefix is valid BMH skip data. */
      if (out->has_literal_prefix ||
          (out->has_bmh_skip && out->literal_prefix_len > 0)) {
        size_t plen = out->literal_prefix_len < SNOBOL_SEARCH_MAX_PREFIX
                          ? out->literal_prefix_len
                          : SNOBOL_SEARCH_MAX_PREFIX;
        for (size_t i = 0; i < plen - 1; i++) {
          uint8_t b = out->literal_prefix[i];
          out->bmh_skip[b] = (uint8_t)(plen - 1 - i);
        }
      }
    }
  }
}

void snobol_search_meta_free(snobol_search_meta_t *meta) {
  if (!meta) {
    return;
  }
  if (meta->bmh_skip) {
    snobol_free(meta->bmh_skip);
    meta->bmh_skip = nullptr;
  }
  if (meta->fusion) {
    snobol_fusion_free(meta->fusion);
    meta->fusion = nullptr;
  }
}

void snobol_search_vm_cleanup(VM *vm) {
  if (!vm) {
    return;
  }
  if (vm->pike_thread_buf) {
    snobol_free(vm->pike_thread_buf);
    vm->pike_thread_buf = NULL;
  }
  if (vm->pike_defer_buf) {
    snobol_free(vm->pike_defer_buf);
    vm->pike_defer_buf = NULL;
  }
  if (vm->pike_cold_pool) {
    snobol_free(vm->pike_cold_pool);
    vm->pike_cold_pool = NULL;
  }
  if (vm->choices_arena) {
    vm_arena_destroy(vm->choices_arena);
    vm->choices_arena = nullptr;
  }
  vm_trail_free(vm);
  vm_write_log_free(vm);
}

/* ---------------------------------------------------------------------------
 * Cost-model tier selection (Priority 4)
 *
 * Per-invocation cost model: each eligible tier is scored as
 *     cost_ns = setup_ns + (subject_len / per_byte_div)
 * and the cheapest eligible tier is selected. The general VM (Tier 8) is
 * always a fallback candidate. Break/SPAN (0-1) keep their fixed tier, and the
 * automaton (Tier 7) is owned by the DFA override in snobol_search_exec(); this
 * model chooses among the remaining tiers (2-8).
 *
 * Coefficients live in this single table as the recalibration point.
 *
 * NOTE: the TIER_ALT_LIT setup cost was recalibrated from the design's 40 ns to
 * 12 ns AFTER the trie-cache optimization (Priority 2) removed the per-call
 * ~7 KB trie rebuild — the cached trie is now a pointer copy, so its per-call
 * cost is tiny. With the stale 40 ns, the model routed every bushy alt-literal
 * to the automaton (which is correct but defeats the cache). At 12 ns the trie
 * wins for typical (short/medium) subjects while the automaton still wins for
 * long subjects (>= ~87 bytes when a DFA is provided).
 * ------------------------------------------------------------------------- */
typedef struct {
  snobol_search_tier_t tier;
  int32_t setup_ns;
  int32_t per_byte_div; /* subject_len is divided by this */
} tier_cost_coeff_t;

static const tier_cost_coeff_t k_tier_cost[] = {
    {TIER_BREAK_SCAN, 20, 8},
    {TIER_SPAN_SCAN, 20, 8},
    {TIER_LITERAL, 10, 16},
    {TIER_PREFIX, 30, 32},
    {TIER_BITMAP, 25, 12},
    {TIER_ALT_LIT, 12, 20}, /* recalibrated post trie-cache (was 40) */
    {TIER_SEARCH_VM, 50, 10},
    {TIER_GENERAL, 100, 5},
    /* SIMD NFA: one bitmap-skip pass + O(1) scalar verify at candidates.
     * The coefficient is calibrated to win the cost race for simd_eligible
     * charclass patterns (SPAN/ANY/NOTANY) against the SEARCH_VM/GENERAL
     * tiers and the dedicated SPAN/BREAK bitmap scanners.  ascii_class_only
     * SPAN/BREAK still also match TIER_SPAN_SCAN/TIER_BREAK_SCAN; SIMD wins
     * because per-byte work is identical (one bit-test) and the NFA path
     * covers ANY/NOTANY too. */
    {TIER_SIMD_NFA, 15, 16},
    {TIER_FUSED_AUTOMATON, 20, 8},
};

/* Score every eligible tier and return the cheapest. `dfa_available` gates the
 * automaton candidate: when no DFA is supplied the caller cannot run Tier 7, so
 * it is excluded here (the DFA override in snobol_search_exec owns Tier 7). */
snobol_search_tier_t select_tier_by_cost(const snobol_search_meta_t *meta,
                                         size_t subject_len, bool dfa_available,
                                         bool anchored) {
  (void)
      dfa_available; /* automaton owned by the DFA override; not scored here */
  int best_tier = TIER_GENERAL;
  int32_t best_cost = 2147483647;

  for (size_t i = 0; i < sizeof(k_tier_cost) / sizeof(k_tier_cost[0]); i++) {
    const tier_cost_coeff_t *c = &k_tier_cost[i];
    bool eligible = false;
    switch (c->tier) {
      case TIER_BREAK_SCAN:
        /* BREAK/BREAKX relocate the match to the first break byte — only valid
       * for scanning; excluded when anchored (fixed offset). */
        eligible = ((!anchored && meta->is_break_family &&
                     meta->ascii_class_only) != 0);
        break;
      case TIER_SPAN_SCAN:
        eligible = ((!anchored && meta->is_span_family &&
                     meta->ascii_class_only) != 0);
        break;
      case TIER_LITERAL: eligible = meta->is_literal_only; break;
      case TIER_PREFIX:
        /* memmem/memchr prefix scan relocates the match — only valid for
       * scanning; excluded when anchored. */
        eligible = ((!anchored && meta->has_literal_prefix &&
                     meta->literal_prefix_len > 0) != 0);
        break;
      case TIER_BITMAP:
        eligible =
            ((meta->has_candidate_bitmap && meta->is_single_char_alt) != 0);
        break;
      case TIER_ALT_LIT:
        /* Both bushy and flat alt-literals use the trie. */
        eligible = meta->is_alt_literals;
        break;
      case TIER_SEARCH_VM:
        /* Alt-literals never use the search VM: flat and bushy -> ALT_LIT. */
        eligible = ((meta->search_vm_eligible && !meta->is_alt_literals) != 0);
        break;
      case TIER_SIMD_NFA:
        eligible = ((meta->simd_eligible && meta->ascii_class_only) != 0);
        break;
      case TIER_FUSED_AUTOMATON:
        eligible = ((meta->fusion_eligible && !meta->has_literal_prefix) != 0);
        break;
      case TIER_GENERAL:
        eligible = true; /* always available as fallback */
        break;
      default: eligible = false; break;
    }
    if (!eligible) {
      continue;
    }

    int32_t cost =
        c->setup_ns + (int32_t)(subject_len / (size_t)c->per_byte_div);
    if (cost < best_cost) {
      best_cost = cost;
      best_tier = (int)c->tier;
    }
  }
  return (snobol_search_tier_t)best_tier;
}

/* ---------------------------------------------------------------------------
 * Dump the authoritative cost-model coefficient table (k_tier_cost[]) to @p out
 * (or stdout when @p out is NULL).  Used by the benchmark probe to compare the
 * model's predicted setup cost against the measured per-scenario ns/iter and to
 * suggest recalibration of the coefficients.
 * ---------------------------------------------------------------------------
 */
void SNOBOL_COLD snobol_search_dump_cost_model(FILE *out) {
  FILE *f = out ? out : stdout;
  fprintf(f, "cost-model coefficients (cost_ns = setup_ns + subject_len / "
             "per_byte_div):\n");
  for (size_t i = 0; i < sizeof(k_tier_cost) / sizeof(k_tier_cost[0]); i++) {
    const tier_cost_coeff_t *c = &k_tier_cost[i];
    const char *name = "?";
    switch (c->tier) {
      case TIER_BREAK_SCAN: name = "BREAK_SCAN"; break;
      case TIER_SPAN_SCAN: name = "SPAN_SCAN"; break;
      case TIER_LITERAL: name = "LITERAL"; break;
      case TIER_PREFIX: name = "PREFIX"; break;
      case TIER_BITMAP: name = "BITMAP"; break;
      case TIER_ALT_LIT: name = "ALT_LIT"; break;
      case TIER_SEARCH_VM: name = "SEARCH_VM"; break;
      case TIER_SIMD_NFA: name = "SIMD_NFA"; break;
      case TIER_FUSED_AUTOMATON: name = "FUSED"; break;
      case TIER_GENERAL: name = "GENERAL"; break;
      default: break;
    }
    fprintf(f, "  tier=%-11s setup_ns=%-4d per_byte_div=%-4d\n", name,
            c->setup_ns, c->per_byte_div);
  }
  fprintf(f,
          "note: TIER_ALT_LIT setup recalibrated to %d ns post trie-cache.\n",
          k_tier_cost[5].setup_ns);
}
