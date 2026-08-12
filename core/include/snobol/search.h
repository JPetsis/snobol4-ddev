#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include <stdio.h>

/**
 * @file search.h
 * @brief Core search runtime API for libsnobol4
 *
 * Provides search-from-offset, find-next-match, split-over-subject, and
 * replace-over-subject execution paths that operate within one native C
 * control loop, avoiding PHP-level offset iteration.
 *
 * The search runtime is language-agnostic: it depends only on the compiled
 * bytecode, the VM structure, and standard C library primitives.
 *
 * ## Tier Dispatch
 *
 * Search execution is dispatched through a function pointer table indexed
 * by meta->tier (snobol_search_tier_t). Each tier represents a matching
 * strategy with different performance characteristics:
 *
 *   Tier 0: BREAK/BREAKX ASCII bitmap scan
 *   Tier 1: SPAN ASCII bitmap scan
 *   Tier 2: Literal-only fast path (no VM)
 *   Tier 3: Literal prefix (memmem/memchr)
 *   Tier 4: Candidate-set bitmap for single-char alternation
 *   Tier 5: Alternation-of-literals (trie-based)
 *   Tier 6: Search-VM (lightweight backtracking NFA)
 *   Tier 7: DFA automaton (O(n) linear scan)
 *   Tier 8: General VM fallback (full SNOBOL4 VM)
 *   Tier 9: SIMD Thompson NFA (AVX2/NEON-accelerated byte-parallel NFA)
 *   Tier 10: Fused concat-pattern automaton (no VM)
 *
 * ## search_vm_t
 *
 * Lightweight VM state (~1.6 KB) for Tier 6-7 execution. Contains bytecode
 * pointer, subject pointer, instruction/position pointers, range metadata,
 * choice stack, loop counters, and (for Tier 6 capture-aware) capture and
 * variable registers. Lower tiers use only bc/ip/pos; the extra fields
 * are ignored.
 *
 * ## Metadata Bitfield Flags
 *
 * snobol_search_meta_t.flags packs 16 boolean flags into a uint32_t for
 * single-word access. Each flag corresponds to a META_* constant defined
 * below. The tier field (uint8_t) stores the pre-computed tier index for
 * single-dispatch routing via tier_table[meta->tier].
 */

#include "snobol/snobol_attrs.h"
#include "snobol/vm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct snobol_pattern; /* forward decl — only used as an opaque pointer */

/* ---------------------------------------------------------------------------
 * Search candidate metadata
 *
 * Derived once from compiled bytecode; stored alongside patterns for fast
 * retrieval on each search invocation.  Allows the search loop to skip
 * unpromising candidate positions before invoking the general VM matcher.
 * ---------------------------------------------------------------------------
 */

/** Maximum literal prefix bytes retained for candidate skipping. */
#define SNOBOL_SEARCH_MAX_PREFIX 16

/** Maximum single-char alternation bytes tracked. */
enum { SNOBOL_SEARCH_MAX_ALT = 32 };

/* ---------------------------------------------------------------------------
 * Tier constants for single-dispatch routing
 *
 * Each tier corresponds to a matching strategy in snobol_search_exec().
 * The tier is computed at compile time and stored in snobol_search_meta_t.tier.
 * ---------------------------------------------------------------------------
 */
typedef enum {
  TIER_BREAK_SCAN = 0, /**< BREAK/BREAKX with ASCII bitmap scan */
  TIER_SPAN_SCAN = 1,  /**< SPAN with ASCII bitmap scan */
  TIER_LITERAL = 2,    /**< Literal-only fast path (no VM) */
  TIER_PREFIX = 3,     /**< Literal prefix (memmem / memchr) */
  TIER_BITMAP = 4,     /**< Candidate-set bitmap for single-char alt */
  TIER_ALT_LIT = 5,    /**< Alternation-of-literals (trie-based) */
  TIER_SEARCH_VM = 6,  /**< Search-VM for eligible patterns */
  TIER_AUTOMATON = 7,  /**< DFA automaton for eligible patterns */
  TIER_GENERAL = 8,    /**< General VM fallback with start-byte bitmap */
  TIER_SIMD_NFA =
      9, /**< SIMD-accelerated Thompson NFA for charclass patterns */
  TIER_FUSED_AUTOMATON = 10, /**< Fused concat-pattern automaton (no VM) */
  TIER_COUNT = 11            /**< Number of tiers (sentinel) */
} snobol_search_tier_t;

/* ---------------------------------------------------------------------------
 * Metadata flag bit constants
 *
 * Packed into snobol_search_meta_t.flags for single-word access.
 * ---------------------------------------------------------------------------
 */
#define META_HAS_LITERAL_PREFIX (1u << 0)
#define META_HAS_FIRST_BYTE (1u << 1)
#define META_HAS_CANDIDATE_BITMAP (1u << 2)
#define META_MAY_MATCH_EMPTY (1u << 3)
#define META_ALWAYS_CONSUMES (1u << 4)
#define META_IS_SINGLE_CHAR_ALT (1u << 5)
#define META_IS_BREAK_FAMILY (1u << 6)
#define META_IS_SPAN_FAMILY (1u << 7)
#define META_IS_BREAKX (1u << 8)
#define META_ASCII_CLASS_ONLY (1u << 9)
#define META_HAS_START_BITMAP (1u << 10)
#define META_AUTOMATON_ELIGIBLE (1u << 11)
#define META_IS_ALT_LITERALS (1u << 12)
#define META_HAS_BMH_SKIP (1u << 13)
#define META_IS_LITERAL_ONLY (1u << 14)
#define META_SEARCH_VM_ELIGIBLE (1u << 15)
#define META_SIMD_ELIGIBLE (1u << 16)
#define META_IS_ALT_LITERALS_FLAT (1u << 17)
#define META_HAS_REQUIRED_LIT (1u << 18)
#define META_FUSION_ELIGIBLE (1u << 19)

/* ---------------------------------------------------------------------------
 * Automaton (DFA) types
 *
 * Constructed from search-VM-eligible bytecode via powerset construction.
 * Enables O(n) single-pass matching over the subject.
 * ---------------------------------------------------------------------------
 */

/** Sentinel: no transition / DEAD state. */
#define SNOBOL_DFA_DEAD UINT16_MAX

/** Maximum DFA states before we give up and fall back to the search-VM. */
enum { SNOBOL_DFA_MAX_STATES = 4096 };

/**
 * Compiled DFA for a single pattern.
 *
 * Transition table layout:  trans[state * 256 + byte] = next_state.
 * The DEAD state (SNOBOL_DFA_DEAD) means no valid continuation.
 */
typedef struct {
  uint16_t *trans;     /**< Flattened 256 × num_states transition table */
  uint32_t num_states; /**< Number of DFA states (excluding DEAD) */
  uint8_t *
      accepting; /**< Bitmap: byte (num_states+7)/8, bit i set => state i is accepting */
  uint16_t start_state; /**< Start DFA state index */
  uint32_t state_cap;   /**< Allocated capacity in states */
} snobol_dfa_t;

/**
 * @brief Opaque pre-allocated trie used by the Tier-5 alternation matcher.
 *
 * The full definition lives in search.c (it is a ~7 KB fixed pool).  Callers
 * (e.g. snobol_pattern_t) only ever hold a pointer, so the struct body is
 * intentionally not exposed here.
 */
typedef struct snobol_auto_trie_t snobol_auto_trie_t;

/** Free a cached trie previously built by the Tier-5 matcher. Safe on NULL. */
void snobol_auto_trie_free(snobol_auto_trie_t *trie);

/* ---------------------------------------------------------------------------
 * Pattern fusion types
 *
 * The fusion engine compiles concat chains of compatible ops (LIT, SPAN,
 * ANY, NOTANY, BREAK) into a flat segment list.  exec_fusion() walks the
 * segment list directly — no VM, no bytecode dispatch, no choice stack.
 * ---------------------------------------------------------------------------
 */

enum { FUSION_LIT = 0 };
enum { FUSION_RUN = 1 };
enum { FUSION_CHAR = 2 };
enum { FUSION_ALT = 3 };

enum { MAX_FUSION_SEGMENTS = 32 };
enum { MAX_FUSION_ALT = 8 };

typedef struct snobol_fusion_segment_t {
  uint8_t type;
  union {
    struct {
      const uint8_t *data;
      uint32_t len;
    } lit;
    struct {
      uint8_t bitmap[32];
      uint32_t min;
    } run;
    struct {
      uint8_t bitmap[32];
    } chr;
    struct {
      struct snobol_fusion_segment_t *alts[MAX_FUSION_ALT];
      uint32_t alt_lens[MAX_FUSION_ALT];
      uint32_t alt_count;
    } alt;
  };
} snobol_fusion_segment_t;

typedef struct {
  uint32_t count;
  snobol_fusion_segment_t segs[MAX_FUSION_SEGMENTS];
} snobol_fusion_t;

/** @brief Return the pattern's cached Tier-5 alternation trie, or NULL if
 *  it has not been built yet. Read-only accessor intended for diagnostics
 *  and tests; the cache is populated lazily on the first Tier-5 search. */
snobol_auto_trie_t *snobol_pattern_get_trie_cache(
    const struct snobol_pattern *pattern);

typedef struct {
  /* Packed flags and tier index ------------------------------------------ */
  uint32_t flags; /**< Bitfield of META_* flag constants */
  uint8_t tier;   /**< Tier index (snobol_search_tier_t) for dispatch */

  bool has_literal_prefix;
  uint8_t literal_prefix[SNOBOL_SEARCH_MAX_PREFIX];
  size_t literal_prefix_len;

  /* First-byte candidate ------------------------------------------------- */
  /* When has_first_byte is true, every match MUST start with first_byte.
   * Enables memchr-based position skipping when prefix_len >= 1. */
  bool has_first_byte;
  uint8_t first_byte;

  /* Candidate-set bitmap ------------------------------------------------- */
  /* Each set bit i means a match MAY start with byte i (0-127).
   * Used for alternation-of-single-chars and similar patterns. */
  bool has_candidate_bitmap;
  uint64_t candidate_bitmap[2]; /* 128-bit ASCII bitmap */

  /* Empty-match flag ----------------------------------------------------- */
  /* When true, the pattern can succeed while consuming zero bytes;
   * the search loop must advance unconditionally after an empty match. */
  bool may_match_empty;

  /* Always-consuming flag ------------------------------------------------ */
  /* When true, the pattern consumes at least one byte on success.
   * The search loop may advance by (match_end - match_start) safely. */
  bool always_consumes;

  /* Single-char alternation ---------------------------------------------- */
  /* True if the root pattern is a pure alternation of single-character
   * literals; these can be collapsed to a candidate-set for one-pass scan. */
  bool is_single_char_alt;
  uint8_t alt_bytes[SNOBOL_SEARCH_MAX_ALT];
  size_t alt_count;

  /* BREAK / SPAN / BREAKX classification --------------------------------- */
  /* True if the first effective opcode is BREAK, BREAKX, or SPAN. */
  bool is_break_family;     /**< OP_BREAK or OP_BREAKX as root op */
  bool is_span_family;      /**< OP_SPAN as root op */
  bool is_breakx;           /**< Specifically OP_BREAKX (extended retry) */
  bool ascii_class_only;    /**< Character class is entirely ASCII (<= 127) */
  uint64_t class_bitmap[2]; /**< ASCII bitmap for BREAK/SPAN/BREAKX root op */

  /* Start-byte bitmap ---------------------------------------------------- */
  /* 256-bit bitmap: bit i is set when the pattern can match starting with
   * byte value i.  Computed by derive_meta for ALL patterns.
   * Allows the search loop to reject candidate positions whose subject byte
   * is not in the bitmap (inspired by PCRE2's start_bits). */
  bool has_start_bitmap;
  uint8_t start_bitmap[32];

  /* Minimum match length (in bytes) -------------------------------------- */
  /* Lower bound on the number of subject bytes consumed by a successful
   * match.  Zero when unknown.  Inspired by PCRE2's find_minlength. */
  uint32_t minlength;

  /* Automaton eligibility ------------------------------------------------ */
  /* True if the pattern is eligible for the lightweight automaton path:
   * ASCII-only, side-effect free (no EVAL/ASSIGN/EMIT ops), and structurally
   * local (no DYNAMIC). */
  bool automaton_eligible;

  /* Alternation-of-literals detection ----------------------------------- */
  /* True when the pattern is a flat alternation of literal strings
   * (e.g. "abc" | "def" | "ghi").  Enables trie-based multi-string matching
   * (Tier 3a) which replaces the general VM loop for these patterns. */
  bool is_alt_literals;

  /* True when the alt-literals trie is "flat": the alternatives share no
   * common prefix, so the trie provides no benefit over the general VM.
   * Detected at derive_meta time; flat alt-literals are routed to
   * TIER_GENERAL instead of TIER_ALT_LIT (fixes the 125x regression). */
  bool is_alt_literals_flat;

  /* Boyer-Moore-Horspool skip table -------------------------------------- */
  /* When has_bmh_skip is true, bmh_skip[byte] gives the distance to advance
   * the search position after a VM failure at the current candidate.  The
   * table is computed from the literal prefix (bmh_skip_len) and allows the
   * search loop to skip more than one byte at a time for literal-leading
   * patterns that fall through to the general VM or automaton paths.
   *
   * The skip value is computed for the byte at subject[pos + bmh_skip_len - 1]
   * and is valid as long as pos + bmh_skip_len <= subject_len.
   * bmh_skip_len is identical to literal_prefix_len when has_bmh_skip is set. */
  bool has_bmh_skip;
  uint8_t *bmh_skip; /**< Heap-allocated BMH table (NULL if not computed) */
  size_t bmh_skip_len;

  /* Literal-only detection ------------------------------------------------ */
  /* True when the pattern is a single literal followed by ACCEPT (optionally
   * with zero-width assertions like POS/RPOS/ANCHOR between).  Enables a
   * fast memmem/memcmp path that bypasses the VM entirely. */
  bool is_literal_only;

  /* Search-VM eligibility ------------------------------------------------- */
  /* True when the pattern contains only opcodes from the safe set supported
   * by the lightweight search-VM (search_vm_exec).  The safe set includes
   * LIT, LEN, ANY, NOTANY, SPAN, BREAK, JMP, SPLIT, POS, RPOS, TAB, RTAB,
   * ANCHOR, FENCE, NOP, REPEAT_INIT, REPEAT_STEP, ACCEPT, FAIL, ABORT,
   * SUCCEED.  Patterns with EVAL, ASSIGN, CAP_START, CAP_END, EMIT_*,
   * DYNAMIC, TABLE_*, ARRAY_*, GOTO, GOTO_F, LABEL, BAL, REM, BREAKX are
   * NOT eligible. */
  bool search_vm_eligible;

  /* Required-byte pre-filter ----------------------------------------------- */
  /* The longest literal that MUST appear in the subject for a match to exist.
   * When required_lit_len > 0, dispatch_search_impl runs a memchr/memmem
   * scan before any tier and returns false (with prefilter_skip=true) when
   * the literal is absent.  Derived from the rightmost literal(s) before
   * ACCEPT/SUCCEED along all paths. */
  bool has_required_lit;
  uint8_t required_lit[SNOBOL_SEARCH_MAX_PREFIX];
  size_t required_lit_len;

  /* Capture presence ------------------------------------------------------- */
  /* True when the bytecode contains any OP_CAP_START / OP_CAP_END (or
   * OP_EMIT_CAPTURE).  Only TIER_SEARCH_VM (6) and TIER_GENERAL (8) can record
   * captures; all other tiers silently drop them.  derive_meta uses this to
   * gate the non-capturing eligibility flags off so captured patterns are
   * never routed to a tier that would lose the capture. */
  bool has_capture;

  /* SIMD NFA eligibility -------------------------------------------------- */
  /* True when the pattern is eligible for the SIMD-accelerated NFA path:
   * ASCII-only character-class operations (SPAN, BREAK, ANY, NOTANY) with
   * no side effects, captures, or complex control flow.  Patterns that pass
   * are routed to Tier 9 (TIER_SIMD_NFA). */
  bool simd_eligible;

  /* Fusion eligibility --------------------------------------------------- */
  /* True when the pattern is a concat of fusible ops (LIT, SPAN, ANY,
   * NOTANY, BREAK) with no captures, EVAL, or side effects.  When true,
   * `fusion` points to the compiled segment list and `tier` is set to
   * TIER_FUSED_AUTOMATON. */
  bool fusion_eligible;
  snobol_fusion_t *fusion; /**< Heap-allocated fusion segment list */
} snobol_search_meta_t;

/* ---------------------------------------------------------------------------
 * Metadata flag accessor macros
 *
 * Type-safe bitfield access for snobol_search_meta_t.flags.
 * Each macro tests exactly one bit; arguments are evaluated once.
 * ---------------------------------------------------------------------------
 */
#define snobol_meta_has_literal_prefix(m) \
  (!!((m)->flags & META_HAS_LITERAL_PREFIX))
#define snobol_meta_has_first_byte(m) (!!((m)->flags & META_HAS_FIRST_BYTE))
#define snobol_meta_has_candidate_bitmap(m) \
  (!!((m)->flags & META_HAS_CANDIDATE_BITMAP))
#define snobol_meta_may_match_empty(m) (!!((m)->flags & META_MAY_MATCH_EMPTY))
#define snobol_meta_always_consumes(m) (!!((m)->flags & META_ALWAYS_CONSUMES))
#define snobol_meta_is_single_char_alt(m) \
  (!!((m)->flags & META_IS_SINGLE_CHAR_ALT))
#define snobol_meta_is_break_family(m) (!!((m)->flags & META_IS_BREAK_FAMILY))
#define snobol_meta_is_span_family(m) (!!((m)->flags & META_IS_SPAN_FAMILY))
#define snobol_meta_is_breakx(m) (!!((m)->flags & META_IS_BREAKX))
#define snobol_meta_ascii_class_only(m) (!!((m)->flags & META_ASCII_CLASS_ONLY))
#define snobol_meta_has_start_bitmap(m) (!!((m)->flags & META_HAS_START_BITMAP))
#define snobol_meta_automaton_eligible(m) \
  (!!((m)->flags & META_AUTOMATON_ELIGIBLE))
#define snobol_meta_is_alt_literals(m) (!!((m)->flags & META_IS_ALT_LITERALS))
#define snobol_meta_alt_literals_flat(m) \
  (!!((m)->flags & META_IS_ALT_LITERALS_FLAT))
#define snobol_meta_has_bmh_skip(m) (!!((m)->flags & META_HAS_BMH_SKIP))
#define snobol_meta_is_literal_only(m) (!!((m)->flags & META_IS_LITERAL_ONLY))
#define snobol_meta_search_vm_eligible(m) \
  (!!((m)->flags & META_SEARCH_VM_ELIGIBLE))
#define snobol_meta_simd_eligible(m) (!!((m)->flags & META_SIMD_ELIGIBLE))
#define snobol_meta_has_required_lit(m) (!!((m)->flags & META_HAS_REQUIRED_LIT))
#define snobol_meta_fusion_eligible(m) (!!((m)->flags & META_FUSION_ELIGIBLE))

/**
 * @brief Set a flag bit in metadata flags.
 *
 * Used during snobol_search_derive_meta() to populate the flags bitfield.
 *
 * @param m     Pointer to metadata struct.
 * @param flag  Flag constant (META_*).
 */
/* Verify tier field is within the first cache line (64 bytes)
 * so that dispatch decisions don't pull in the full metadata struct. */
static_assert(
    offsetof(snobol_search_meta_t, tier) < 64,
    "tier field must be within first 64 bytes of snobol_search_meta_t");

static inline void snobol_meta_set_flag(snobol_search_meta_t *m,
                                        uint32_t flag) {
  m->flags |= flag;
}

/* ---------------------------------------------------------------------------
 * Search execution result
 * ---------------------------------------------------------------------------
 */
typedef struct {
  bool success;
  bool aborted;         /**< Set true when an OP_ABORT terminated the match,
                          signalling the caller to stop searching entirely. */
  bool pike_overflowed; /**< Set true when pike_scan's thread buffer overflowed
                             and the restart-loop fallback was used. */
  bool pike_zero_progress; /**< Set true when pike_scan exited a repetition via
                                the zero-progress guard: pike's greedy match
                                position is then unreliable (the min==0 skip
                                path can match empty earlier), so the caller
                                must fall back to the restart loop. */
  bool prefilter_skip;     /**< Set true when the required-byte prefilter
                             rejected the subject without entering any tier. */
  size_t match_start;      /**< Byte offset of match start within the original
                          subject */
  size_t match_end; /**< Byte offset just past the match end in the original
                          subject */
} snobol_search_result_t;

/* ---------------------------------------------------------------------------
 * Search-mode skip/bailout reason codes
 *
 * Used in diagnostics to distinguish expected candidate rejection from
 * unsupported control flow or safety fallbacks.
 * ---------------------------------------------------------------------------
 */
typedef enum {
  SNOBOL_SEARCH_SKIP_NONE = 0, /**< Position not skipped */
  SNOBOL_SEARCH_SKIP_LITERAL =
      1, /**< Skipped by literal prefix scan (memmem) */
  SNOBOL_SEARCH_SKIP_FIRST_BYTE = 2, /**< Skipped by first-byte memchr */
  SNOBOL_SEARCH_SKIP_BITMAP = 3,     /**< Skipped by candidate bitmap scan */
  SNOBOL_SEARCH_SKIP_BREAK_SCAN = 4, /**< Skipped by BREAK/BREAKX bitmap scan */
  SNOBOL_SEARCH_SKIP_SPAN_SCAN = 5,  /**< Skipped by SPAN bitmap scan */
  SNOBOL_SEARCH_SKIP_JIT_COLD =
      6, /**< JIT skip: below search-mode hotness threshold */
  SNOBOL_SEARCH_SKIP_JIT_BUDGET = 7, /**< JIT skip: compile budget exhausted */
  SNOBOL_SEARCH_SKIP_JIT_EXIT_RATE = 8, /**< JIT skip: exit rate exceeded */
  SNOBOL_SEARCH_BAILOUT_UNSUPPORTED =
      9, /**< JIT bailout: unsupported control flow */
  SNOBOL_SEARCH_BAILOUT_SAFETY =
      10, /**< JIT bailout: safety fallback triggered */
  SNOBOL_SEARCH_BAILOUT_CANDIDATE =
      11, /**< JIT bailout: expected candidate rejection */
} snobol_search_skip_reason_t;

/* ---------------------------------------------------------------------------
 * Per-search-invocation diagnostics
 *
 * Populated by snobol_search_exec() when a non-NULL pointer is passed.
 * ---------------------------------------------------------------------------
 */
typedef struct {
  uint64_t candidates_tested;  /**< Positions where the VM was invoked */
  uint64_t candidates_skipped; /**< Positions skipped by candidate metadata */
  uint64_t automaton_tests;    /**< Positions tested by the automaton path */
  uint64_t search_vm_tests;    /**< Positions tested by the search-VM path */
  bool pike_overflow;          /**< Set true when pike_scan overflowed */
  snobol_search_skip_reason_t last_skip_reason;
} snobol_search_diag_t;

/* ---------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------------
 */

/**
 * Derive search candidate metadata from compiled pattern bytecode.
 *
 * Call once after compilation; store alongside the compiled pattern for fast
 * retrieval on each search invocation.  The function always writes to *out
 * (all fields default to false/0 when the pattern has no analyzable structure).
 *
 * @param bc      Compiled bytecode buffer (read-only)
 * @param bc_len  Bytecode length in bytes
 * @param out     Output metadata struct (caller-allocated)
 */
void snobol_search_derive_meta(const uint8_t *bc, size_t bc_len,
                               snobol_search_meta_t *out);

/**
 * Free heap-allocated fields in a metadata struct.
 *
 * Must be called when a metadata struct is no longer needed and was
 * populated by snobol_search_derive_meta(). Currently frees the
 * bmh_skip table if allocated.
 *
 * @param meta  Metadata struct to clean up (NULL is safe).
 */
void snobol_search_meta_free(snobol_search_meta_t *meta);

/**
 * Free heap-allocated fields in a VM used by the search runtime.
 *
 * Releases the Pike thread/defer buffers and choice arena that may have
 * been allocated during snobol_search_exec() or snobol_search_exec_anchored().
 * Safe to call on a zero-initialised or already-cleaned VM (fields are NULL).
 *
 * @param vm  VM struct to clean up (NULL is safe).
 */
void snobol_search_vm_cleanup(VM *vm);

/**
 * @brief Print the cost-model tier coefficients used by select_tier_by_cost().
 *
 * Diagnostic aid for recalibrating the per-tier setup/throughput costs from
 * bench_probe output. The authoritative values live in core/src/search.c
 * (k_tier_cost); this prints them so the probe and other diagnostics share a
 * single source of truth.
 *
 * @param out  Stream to write to (NULL is treated as stdout).
 */
void snobol_search_dump_cost_model(FILE *out);

/**
 * Execute a single search over [subject, subject+subject_len) from
 * start_offset, returning the first non-overlapping match using one native C
 * control loop.
 *
 * The caller must populate vm->bc and vm->bc_len with the match pattern
 * bytecode and may set vm->eval_fn, vm->emit_fn, and related callback fields
 * before calling.  The function manages vm->s, vm->len, vm->pos, and vm->ip
 * internally for each candidate position.
 *
 * After a successful return, vm->var_start, vm->var_end, and vm->var_count
 * reflect the captures from the winning match.
 *
 * @param vm            Partially-initialised VM (bc/bc_len + callbacks set)
 * @param subject       Full subject string pointer
 * @param subject_len   Subject length in bytes
 * @param start_offset  Byte offset to start searching from (0 = beginning)
 * @param meta          Optional pre-derived metadata for acceleration (NULL =
 * derive inline)
 * @param dfa           Optional pre-built DFA for the automaton tier (NULL =
 * none)
 * @param out_result    Output: match position (always written; success=false on
 * no match)
 * @param diag          Optional diagnostics output (may be NULL)
 * @return true if a match was found; false if no match in [start_offset,
 * subject_len]
 */
SNOBOL_NODISCARD bool snobol_search_exec(
    VM *vm, const char *subject, size_t subject_len, size_t start_offset,
    const snobol_search_meta_t *meta, const snobol_dfa_t *dfa,
    snobol_search_result_t *out_result, snobol_search_diag_t *diag);

/**
 * Anchored (SNOBOL-style) match entry point used by Pattern::match().
 *
 * Equivalent to snobol_search_exec() with start_offset = 0 and the anchored
 * flag set: the match must begin exactly at offset 0.  The cost-model tier
 * selection excludes scanning tiers (BREAK/SPAN/PREFIX) and any match that
 * does not start at the anchor is rejected.  This routes anchored matching
 * through the cheaper cost-model tiers instead of forcing the full VM.
 *
 * @param vm      Partially-initialised VM (bc/bc_len + callbacks set)
 * @param subject Full subject string pointer
 * @param subject_len Subject length in bytes
 * @param meta    Optional pre-derived metadata (NULL = derive inline)
 * @param dfa     Optional DFA for automaton acceleration (NULL = none)
 * @param out_result Output: match position (always written; success=false on
 * no match)
 * @param diag    Optional diagnostics output (may be NULL)
 * @return true if a match was found starting at offset 0; false otherwise
 */
SNOBOL_NODISCARD bool snobol_search_exec_anchored(
    VM *vm, const char *subject, size_t subject_len,
    const snobol_search_meta_t *meta, const snobol_dfa_t *dfa,
    snobol_search_result_t *out_result, snobol_search_diag_t *diag);

/**
 * Diagnostic readout: return the *executed* dispatch tier that
 * dispatch_search_impl() would select for the given metadata and subject,
 * distinct from the structural tier reported by snobol_pattern_get_meta() ->
 * meta.tier.
 *
 * The cost model (select_tier_by_cost) may promote a structurally SEARCH_VM
 * pattern to the AUTOMATON tier when a DFA is available, so benchmarks that
 * label scenarios by meta->tier mislabel the real path. This function lets the
 * probe (and external callers) measure the tier actually executed.
 *
 * @param meta           Pre-derived search metadata (e.g. from
 *                       snobol_pattern_get_meta()).
 * @param dfa_available True when the caller would supply a DFA (enables the
 *                       Tier-7 automaton override); false otherwise.
 * @param subject_len    Subject length in bytes.
 * @param anchored      True for anchored (Pattern::match) matching.
 * @return The snobol_search_tier_t that would be dispatched.
 */
snobol_search_tier_t snobol_search_executed_tier(
    const snobol_search_meta_t *meta, bool dfa_available, size_t subject_len,
    bool anchored);

/**
 * Build a DFA from search-VM-eligible bytecode via powerset construction.
 * Returns NULL on allocation failure or state explosion (>4096 states).
 * The caller owns the returned DFA (free with snobol_dfa_free()).
 */
snobol_dfa_t *build_dfa(const uint8_t *bc, size_t bc_len, const VM *vm);

/* Forward declaration — complete type in snobol/snobol.h */
struct snobol_pattern;

/**
 * Check whether a pattern is eligible for SIMD NFA execution.
 *
 * Returns true when the bytecode contains only character-class ops
 * (SPAN, BREAK, ANY, NOTANY) with no side effects, captures, or
 * complex control flow.  All character classes must be ASCII-only.
 *
 * @param bc      Compiled bytecode buffer
 * @param bc_len  Bytecode length
 * @return true if the pattern is SIMD-eligible
 */
bool check_simd_eligible(const uint8_t *bc, size_t bc_len);

/**
 * Tier 9: SIMD-accelerated Thompson NFA entry point.
 *
 * Processes eligible character-class patterns (SPAN, BREAK, ANY, NOTANY with
 * ASCII-only ranges) using SIMD instructions when available, falling back to
 * a scalar reference implementation for short subjects and non-SIMD platforms.
 *
 * @param vm           VM with bc/bc_len set for the pattern
 * @param subject      Subject string
 * @param subject_len  Subject length
 * @param start_offset Search start offset
 * @param meta         Pre-derived metadata (unused, must be non-NULL)
 * @param dfa          Unused (must be NULL)
 * @param out_result   Match result
 * @param diag         Diagnostics (may be NULL)
 * @param anchored     Whether the match must begin exactly at start_offset
 * @return true when a match is found
 */
bool tier_simd_nfa(VM *vm, const char *subject, size_t subject_len,
                   size_t start_offset, const snobol_search_meta_t *meta,
                   const snobol_dfa_t *dfa, snobol_search_result_t *out_result,
                   snobol_search_diag_t *diag, bool anchored);

/**
 * Build an alt-literals trie from SPLIT/LIT bytecode.
 *
 * Walks the bytecode starting at offset 0, following SPLIT branches and
 * collecting OP_LIT leaves into a trie (stack-allocated ~7 KB pool).
 * Returns NULL on allocation failure or if the bytecode is not a valid
 * alt-literals SPLIT tree.  The caller owns the returned trie and must
 * free it with snobol_auto_trie_free().
 */
snobol_auto_trie_t *snobol_build_alt_trie(const uint8_t *bc, size_t bc_len);

/**
 * Free a DFA allocated by build_dfa().
 * Called from snobol_pattern_free() in api.c.
 */
void snobol_dfa_free(snobol_dfa_t *dfa);

/**
 * Check whether a pattern has a usable DFA automaton.
 * Returns true when the DFA is constructed and its state count is under the
 * SNOBOL_DFA_MAX_STATES cap (i.e. construction did not abort).
 */
bool snobol_pattern_automaton_available(const struct snobol_pattern *pattern);

/**
 * Free heap-allocated fusion struct and its ALT sub-lists.
 * Safe on NULL.
 */
void snobol_fusion_free(snobol_fusion_t *fusion);

/**
 * Tier 10: Fused concat-pattern execution entry point.
 *
 * Executes fusible concat patterns (LIT/SPAN/ANY/NOTANY/BREAK chains)
 * via a dedicated lightweight engine — no VM, no bytecode dispatch,
 * no choice stack.
 */
bool tier_fusion(VM *vm, const char *subject, size_t subject_len,
                 size_t start_offset, const snobol_search_meta_t *meta,
                 const snobol_dfa_t *dfa, snobol_search_result_t *out_result,
                 snobol_search_diag_t *diag, bool anchored);

#ifdef __cplusplus
}
#endif
