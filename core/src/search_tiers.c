/**
 * @file search_tiers.c
 * @brief Search-execution tier handlers and dispatch for libsnobol4.
 *
 * Split out of search.c (see oss-readiness T4). Implements the runtime
 * matching strategies and the dispatcher:
 *  - Accelerated scanners (BREAK/SPAN/literal/bitmap/alt-literal trie).
 *  - The lightweight capture-aware search-VM (Tier 6).
 *  - NFA epsilon-closure + DFA construction/execution (Tier 7 automaton).
 *  - The tier dispatch table and snobol_search_exec(_anchored).
 *
 * Compile-time metadata derivation and tier selection live in search_meta.c;
 * SIMD acceleration lives in search_simd.c.
 */

#include "snobol/search.h"
#include "snobol/snobol.h"
#include "snobol/snobol_attrs.h"
#include "snobol/snobol_internal.h"
#include "snobol/simd.h"
#include "snobol/vm.h"

/* Tier 8 general-VM fallback, defined below; the alt-literals tier calls it
 * when the trie pool is exhausted (search_simd.c calls it too). */
bool tier_general_fallback(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored);

/* ---- Feature flags ---- */
/* Pike single-pass scan: replace per-offset restart loop with a single
 * left-to-right pass maintaining a set of NFA threads.  Default OFF;
 * enable with -DSNOBOL_PIKE_SCAN or cmake -DENABLE_PIKE_SCAN=ON. */
#ifndef SNOBOL_PIKE_SCAN
/* #define SNOBOL_PIKE_SCAN */
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "search_internal.h"

/* ---------------------------------------------------------------------------
 * Trie builder — inserts a single literal string into the trie.
 * `order` is the alternation branch order (0 = first branch): when a node
 * already terminates a pattern, the LOWEST order wins (ordered-alternation
 * semantics — the first matching branch decides the match).
 * Returns false when the node/edge pool is exhausted.
 * ---------------------------------------------------------------------------
 */
static bool trie_insert(snobol_auto_trie_t *t, const uint8_t *s, size_t len,
                        uint16_t order) {
  uint16_t n = 0; /* start at root */
  for (size_t i = 0; i < len; i++) {
    uint8_t b = s[i];
    /* Search for existing edge */
    uint16_t e = t->nodes[n].first_edge;
    uint16_t prev = SNOBOL_AUTO_NULL;
    while (e != SNOBOL_AUTO_NULL) {
      if (t->edges[e].byte == b) {
        break;
      }
      prev = e;
      e = t->edges[e].sibling;
    }
    if (e != SNOBOL_AUTO_NULL) {
      n = t->edges[e].next;
    } else {
      /* Create new edge + node */
      if (t->edge_count >= SNOBOL_AUTO_MAX_EDGES ||
          t->node_count >= SNOBOL_AUTO_MAX_NODES) {
        return false;
      }
      uint16_t ne = t->edge_count++;
      uint16_t nn = t->node_count++;
      t->edges[ne].byte = b;
      t->edges[ne].next = nn;
      t->edges[ne].sibling = SNOBOL_AUTO_NULL;
      t->nodes[nn].first_edge = SNOBOL_AUTO_NULL;
      t->nodes[nn].is_end = false;
      t->nodes[nn].end_order = 0;
      if (prev == SNOBOL_AUTO_NULL) {
        t->nodes[n].first_edge = ne;
      } else {
        t->edges[prev].sibling = ne;
      }
      n = nn;
    }
  }
  /* Mark the terminal.  The empty literal (len 0) marks the root itself;
   * it is a valid first alternative for ordered-alternation semantics. */
  if (!t->nodes[n].is_end || order < t->nodes[n].end_order) {
    t->nodes[n].is_end = true;
    t->nodes[n].end_order = order;
  }
  return true;
}

/* ---------------------------------------------------------------------------
 * Trie matcher — scans the subject from `offset`, returning true when the
 * trie matches at that position.  On match, `*match_len` receives the
 * number of bytes consumed.  Ordered-alternation semantics: when several
 * terminals match at the position (prefix overlap, or an empty branch), the
 * LOWEST branch order wins — the same first-branch preference as the full
 * VM's SPLIT evaluation.
 * ---------------------------------------------------------------------------
 */
static bool SNOBOL_HOT trie_match(const snobol_auto_trie_t *SNOBOL_RESTRICT t,
                                  const char *SNOBOL_RESTRICT subject,
                                  size_t subject_len, size_t offset,
                                  size_t *SNOBOL_RESTRICT match_len) {
  size_t pos = offset;
  uint16_t n = 0; /* start at root */
  uint16_t best_order = UINT16_MAX;
  size_t best_len = 0;
  if (t->nodes[0].is_end) {
    best_order = t->nodes[0].end_order;
    best_len = 0;
  }
  while (pos < subject_len) {
    uint8_t b = (uint8_t)subject[pos];
    uint16_t e = t->nodes[n].first_edge;
    while (e != SNOBOL_AUTO_NULL && t->edges[e].byte != b) {
      e = t->edges[e].sibling;
    }
    if (e == SNOBOL_AUTO_NULL) {
      break;
    }
    n = t->edges[e].next;
    pos++;
    if (t->nodes[n].is_end && t->nodes[n].end_order < best_order) {
      best_order = t->nodes[n].end_order;
      best_len = pos - offset;
    }
  }
  if (best_order != UINT16_MAX) {
    *match_len = best_len;
    return true;
  }
  return false;
}

/* ---------------------------------------------------------------------------
 * Trie-shape classifier.
 *
 * A trie is "flat" when no node below the root has more than one outgoing
 * edge, i.e. the alternatives share no common prefix.  Such a trie provides
 * no minlength acceleration (a bushy trie does), but is still a correct
 * set-membership test and avoids the full VM.
 * ---------------------------------------------------------------------------
 */
static bool SNOBOL_PURE trie_is_flat(const snobol_auto_trie_t *trie) {
  for (uint16_t n = 1; n < trie->node_count; n++) {
    uint16_t e = trie->nodes[n].first_edge;
    int outgoing = 0;
    while (e != SNOBOL_AUTO_NULL) {
      outgoing++;
      e = trie->edges[e].sibling;
    }
    if (outgoing > 1) {
      return false;
    }
  }
  return true;
}

void snobol_auto_trie_free(snobol_auto_trie_t *trie) {
  snobol_free(trie);
}

/* ---------------------------------------------------------------------------
 * Trie-based search for alternation-of-literals patterns.
 *
 * Walks the bytecode SPLIT tree to collect literals, builds a trie, then
 * scans the subject for matches.  Returns true and fills out_result on
 * the first match at or after start_offset.
 * ---------------------------------------------------------------------------
 */

/* 256-bit start-byte bitmap test (PCRE2-style start_bits acceleration). */
static inline bool SNOBOL_ALWAYS_INLINE bitmap256_test(const uint8_t bm[32],
                                                       uint8_t b) {
  return (bm[b >> 3] & (uint8_t)(1U << (b & 7))) != 0;
}

static bool SNOBOL_HOT search_alt_literals_try(
    VM *SNOBOL_RESTRICT vm, const char *SNOBOL_RESTRICT subject,
    size_t subject_len, size_t start_offset, const snobol_search_meta_t *meta,
    const snobol_dfa_t *dfa, snobol_search_diag_t *diag,
    snobol_search_result_t *out_result, bool anchored) {
  snobol_pattern_t *pat = vm->pattern;
  const snobol_auto_trie_t *trie = nullptr;
  snobol_auto_trie_t local; /* built only on a cache miss */

  /* Check for a pre-built trie on the VM first.  The PHP binding sets
   * vm->trie_cache directly to avoid the struct-offset problem: the PHP
   * snobol_pattern_t layout differs from the core struct, so calling
   * snobol_pattern_get_trie_cache() on a PHP pattern reads garbage. */
  if (vm->trie_cache) {
    trie = vm->trie_cache;
  } else if (pat) {
    trie = snobol_pattern_get_trie_cache(pat);
  }

  if (!trie) {
    /* Build the trie fresh from the SPLIT/LIT tree. */
    trie = &local;
    local.node_count = 1; /* root node (index 0) */
    local.edge_count = 0;
    local.nodes[0].first_edge = SNOBOL_AUTO_NULL;
    local.nodes[0].is_end = false;
    local.nodes[0].end_order = 0;

    bool all_ok = true;
    bool pool_overflow = false;
    uint16_t leaf_order = 0; /* alternation branch order (visit order) */
    size_t bc_len = vm->bc_len;
    const uint8_t *bc = vm->bc;

    size_t stack[64]; /* explicit stack to avoid recursion */
    int sp = 0;
    stack[sp++] = 0; /* start at ip=0 */

    while (sp > 0 && all_ok) {
      size_t ip = stack[--sp];
      if (ip + 2 > bc_len) {
        all_ok = false;
        break;
      }
      uint8_t op = bc[ip];
      if (op == OP_LIT) {
        /* Leaf: insert literal into trie */
        if (ip + 10 > bc_len) {
          all_ok = false;
          break;
        }
        uint32_t off = search_read_u32(bc, ip + 1);
        uint32_t len = search_read_u32(bc, ip + 5);
        if (off >= bc_len || off + len > bc_len) {
          all_ok = false;
          break;
        }
        if (!trie_insert(&local, bc + off, len, leaf_order++)) {
          all_ok = false;
          pool_overflow = true;
          break;
        }
      } else if (op == OP_SPLIT) {
        if (ip + 9 > bc_len) {
          all_ok = false;
          break;
        }
        uint32_t a = search_read_u32(bc, ip + 1);
        uint32_t b = search_read_u32(bc, ip + 5);
        if (a >= bc_len || b >= bc_len) {
          all_ok = false;
          break;
        }
        if (sp + 2 > 64) {
          all_ok = false;
          break;
        }
        stack[sp++] = b;
        stack[sp++] = a;
      } else {
        all_ok = false;
        break;
      }
    }

    if (!all_ok) {
      if (pool_overflow) {
        /* Trie pool exhausted on structurally valid bytecode (e.g. a stale
         * cache built before the derive_meta budget gate): fall back to the
         * general VM instead of reporting a false negative. */
        return tier_general_fallback(vm, subject, subject_len, start_offset,
                                     meta, dfa, out_result, diag, anchored);
      }
      /* Structural failure (truncated/invalid bytecode): fail closed. */
      return false;
    }

    /* Cache the freshly built trie on the owning pattern so subsequent
     * searches reuse it (by pointer — no per-call copy).  Skip flat tries
     * (no shared prefix); they give no benefit over the general VM and
     * route there instead. */
    if (pat && !trie_is_flat(&local)) {
      snobol_auto_trie_t *cache =
          (snobol_auto_trie_t *)snobol_malloc(sizeof(snobol_auto_trie_t));
      if (cache) {
        memcpy(cache, &local, sizeof(*cache));
        snobol_pattern_set_trie_cache(pat, cache);
      }
    }
  }

  /* ---- Scan subject from start_offset ---- */
  size_t offset = start_offset;
  size_t minlength = (meta && meta->minlength > 0) ? meta->minlength : 0;

  while (offset + minlength <= subject_len) {
    /* Anchored: only the single position start_offset may be tried; do not
     * scan or skip past it. */
    if (anchored && offset != start_offset) {
      break;
    }
    /* 1. Start-byte bitmap filter: skip candidate positions whose subject
     *    byte cannot possibly begin any alternative. */
    if (meta && meta->has_start_bitmap) {
      uint8_t b = (uint8_t)subject[offset];
      if (likely(!bitmap256_test(meta->start_bitmap, b))) {
        offset++;
        continue;
      }
    }

    /* 2. Trie match at this position */
    size_t match_len = 0;
    if (trie_match(trie, subject, subject_len, offset, &match_len)) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = offset + match_len;
      return true;
    }

    /* 3. BMH-style skip after a failed match (only when a skip table is
     *    available, e.g. for literal-leading alt patterns). */
    if (meta && meta->has_bmh_skip &&
        offset + meta->bmh_skip_len <= subject_len) {
      size_t adv = meta->bmh_skip[(
          unsigned char)subject[offset + meta->bmh_skip_len - 1]];
      offset += adv > 0 ? adv : 1;
    } else {
      offset++;
    }
  }
  return false;
}

/* ---------------------------------------------------------------------------
 * Internal: reset a VM for a fresh match attempt at the given position
 * within [subject, subject+subject_len).
 * ---------------------------------------------------------------------------
 */
static inline void search_reset_vm(VM *vm, const char *subject,
                                   size_t subject_len, size_t offset) {
  vm->s = subject + offset;
  vm->len = subject_len - offset;
  vm->ip = 0;
  vm->pos = 0;
  /* Clear capture state */
  vm->var_count = 0;
  vm->max_cap_used = 0;
  vm->max_counter_used = 0;
  /* Reset choice stack */
  vm->choices_top = 0;
}


/* ---------------------------------------------------------------------------
 * Tier 2: BREAK/BREAKX accelerated search
 *
 * For a BREAK-family pattern with a pure-ASCII character class bitmap, we
 * scan forward with the bitmap until we find the first delimiter byte, then
 * confirm with the VM.  This avoids invoking the VM at every non-delimiter
 * byte.
 * ---------------------------------------------------------------------------
 */
static bool search_break_accelerated(VM *vm, const char *subject,
                                     size_t subject_len, size_t start_offset,
                                     const snobol_search_meta_t *meta,
                                     snobol_search_result_t *out_result,
                                     snobol_search_diag_t *diag,
                                     bool anchored) {
  const uint64_t *bmap = meta->class_bitmap;
  size_t offset = start_offset;

  while (offset <= subject_len) {
    /* Advance until we find the first delimiter byte − or end of subject */
    size_t scan = offset;
    while (scan < subject_len) {
      uint8_t c = (uint8_t)subject[scan];
      if (c > 127 || bitmap_test(bmap, c)) {
        break;
      }
      scan++;
    }

    if (diag) {
      diag->candidates_skipped += (scan - offset);
    }

    /* Run the VM from `offset` (the start of the candidate field). BREAK
     * consumes the run of non-delimiter bytes up to `scan`, so match_end =
     * offset + (scan - offset) = scan — i.e. the field "up to, not including,
     * the delimiter". The previous code placed the cursor at `scan` (the
     * delimiter itself), which made BREAK match an empty string at the comma.
     * BREAKX needs the same start; its retry semantics are handled inside the
     * VM. */
    search_reset_vm(vm, subject, subject_len, offset);
    if (diag) {
      diag->candidates_tested++;
    }

    bool ok = vm_exec(vm);
    if (ok) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = offset + vm->pos;
      return true;
    }

    /* Advance past current position */
    if (scan >= subject_len) {
      break;
    }
    if (anchored) {
      break;
    }
    offset = scan + 1;
  }

  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * Tier 2: SPAN accelerated search
 *
 * For a SPAN pattern with a pure-ASCII class, we look for the next position
 * where the SPAN character set begins.  We use a simple forward scan with
 * the bitmap.
 * ---------------------------------------------------------------------------
 */
static bool search_span_accelerated(VM *vm, const char *subject,
                                    size_t subject_len, size_t start_offset,
                                    const snobol_search_meta_t *meta,
                                    snobol_search_result_t *out_result,
                                    snobol_search_diag_t *diag, bool anchored) {
  const uint64_t *bmap = meta->class_bitmap;
  size_t offset = start_offset;

  while (offset < subject_len) {
    /* Skip positions where subject[offset] is not in the SPAN set */
    uint8_t c = (uint8_t)subject[offset];
    if (c > 127 || !bitmap_test(bmap, c)) {
      if (diag) {
        diag->candidates_skipped++;
      }
      offset++;
      continue;
    }

    /* Likely start of a SPAN match — verify with VM */
    search_reset_vm(vm, subject, subject_len, offset);
    if (diag) {
      diag->candidates_tested++;
    }

    bool ok = vm_exec(vm);
    if (ok) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = offset + vm->pos;
      return true;
    }
    if (anchored) {
      break;
    }
    offset++;
  }

  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * Tier 1: Literal prefix accelerated search
 *
 * Uses memmem (or memchr for single-byte) to jump directly to the next
 * candidate position.  Falls back to the VM for confirmation (in case the
 * literal is only a prefix and the pattern has additional constraints).
 * ---------------------------------------------------------------------------
 */
static SNOBOL_ALIGNED(64) bool SNOBOL_HOT
    search_literal_accelerated(VM *SNOBOL_RESTRICT vm,
                               const char *SNOBOL_RESTRICT subject,
                               size_t subject_len, size_t start_offset,
                               const snobol_search_meta_t *meta,
                               snobol_search_result_t *out_result,
                               snobol_search_diag_t *diag, bool anchored) {
  size_t offset = start_offset;

  while (offset <= subject_len) {
    const char *hay = subject + offset;
    size_t haylen = subject_len - offset;

    /* Find next candidate position */
    const char *found = nullptr;
    size_t skipped = 0;

    if (meta->literal_prefix_len == 1) {
      /* Single-byte prefix: use memchr */
      found = (const char *)memchr(hay, meta->literal_prefix[0], haylen);
    } else if (meta->literal_prefix_len == 2) {
      /* Two-byte prefix: paired memchr avoids memmem's setup overhead
       * (~30ns), which dominates for such short needles. */
      const char *p =
          (const char *)memchr(hay, meta->literal_prefix[0], haylen);
      while (p) {
        if (p + 1 < hay + haylen && (uint8_t)p[1] == meta->literal_prefix[1]) {
          break;
        }
        size_t remain = (size_t)(hay + haylen - (p + 1));
        if (remain == 0) {
          break;
        }
        const char *q =
            (const char *)memchr(p + 1, meta->literal_prefix[0], remain);
        p = q;
      }
      found = p;
    } else if (meta->literal_prefix_len > 2) {
      /* Multi-byte prefix: use memmem */
      found = (const char *)memmem(hay, haylen, meta->literal_prefix,
                                   meta->literal_prefix_len);
    } else {
      found = hay; /* No prefix hint — try every position */
    }

    if (!found) {
      /* No candidate in remaining subject */
      if (diag) {
        diag->candidates_skipped += haylen;
      }
      break;
    }

    size_t cand = (size_t)(found - subject);
    skipped = cand - offset;
    if (diag) {
      diag->candidates_skipped += skipped;
      diag->candidates_tested++;
    }

    /* Confirm with VM at candidate position */
    search_reset_vm(vm, subject, subject_len, cand);
    bool ok = vm_exec(vm);
    if (ok) {
      out_result->success = true;
      out_result->match_start = cand;
      out_result->match_end = cand + vm->pos;
      return true;
    }

    /* Not a real match — advance one byte past this candidate */
    if (anchored) {
      break;
    }
    offset = cand + 1;
  }

  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * Tier 1: Candidate-bitmap accelerated search
 *
 * Scans forward skipping bytes whose bit is not set in the candidate bitmap.
 * Invokes the VM only at positions where subject[pos] IS in the candidate set.
 * ---------------------------------------------------------------------------
 */
static bool search_bitmap_accelerated(VM *vm, const char *subject,
                                      size_t subject_len, size_t start_offset,
                                      const snobol_search_meta_t *meta,
                                      snobol_search_result_t *out_result,
                                      snobol_search_diag_t *diag,
                                      bool anchored) {
  const uint64_t *bmap = meta->candidate_bitmap;
  size_t offset = start_offset;

  while (offset < subject_len) {
    uint8_t c = (uint8_t)subject[offset];
    if (c > 127 || !bitmap_test(bmap, c)) {
      if (diag) {
        diag->candidates_skipped++;
      }
      offset++;
      continue;
    }

    if (diag) {
      diag->candidates_tested++;
    }
    search_reset_vm(vm, subject, subject_len, offset);
    bool ok = vm_exec(vm);
    if (ok) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = offset + vm->pos;
      return true;
    }
    if (anchored) {
      break;
    }
    offset++;
  }

  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * Tier 2: Literal-only fast path
 *
 * For patterns that are a single literal (optionally wrapped in zero-width
 * assertions) with no captures, output, or side effects, we bypass the VM
 * entirely and use memmem() (unanchored) to find the literal position.
 *
 * This is the fastest possible search path: a single libc call per candidate,
 * no bytecode dispatch, no choice stack, no capture tracking.
 * ---------------------------------------------------------------------------
 */
static bool search_literal_only(VM *vm, const char *subject, size_t subject_len,
                                size_t start_offset,
                                const snobol_search_meta_t *meta,
                                snobol_search_result_t *out_result,
                                snobol_search_diag_t *diag, bool anchored) {
  (void)vm;
  (void)meta;
  size_t offset = start_offset;

  /* Extract the literal from bytecode: skip any leading zero-width ops,
   * then read LIT(offset, len). */
  size_t ip = 0;
  const uint8_t *bc = vm->bc;
  size_t bc_len = vm->bc_len;

  while (ip < bc_len) {
    uint8_t op = bc[ip];
    if (op == OP_NOP || op == OP_FENCE || op == OP_ANCHOR) {
      ip++;
      continue;
    }
    if ((op == OP_POS || op == OP_RPOS) && ip + 5 <= bc_len) {
      ip += 5;
      continue;
    }
    break;
  }
  if (ip + 9 > bc_len || bc[ip] != OP_LIT) {
    goto fallback;
  }
  uint32_t lit_off = search_read_u32(bc, ip + 1);
  uint32_t lit_len = search_read_u32(bc, ip + 5);
  if (lit_off >= bc_len || lit_off + lit_len > bc_len || lit_len == 0) {
    goto fallback;
  }
  const uint8_t *lit = bc + lit_off;

  if (subject_len < lit_len) {
    goto nomatch;
  }

  if (anchored) {
    /* The entire pattern is this single literal, so an anchored match must
     * begin exactly at start_offset. Compare directly — no whole-subject scan
     * (the old memmem-then-reject path paid O(subject) for an O(literal) check). */
    if (subject_len - start_offset >= lit_len &&
        memcmp(subject + start_offset, lit, lit_len) == 0) {
      out_result->success = true;
      out_result->match_start = start_offset;
      out_result->match_end = start_offset + lit_len;
      return true;
    }
    goto nomatch;
  }

  while (offset + lit_len <= subject_len) {
    const char *hay = subject + offset;
    size_t haylen = subject_len - offset;

    const char *found = (const char *)memmem(hay, haylen, lit, lit_len);
    if (!found) {
      break;
    }

    size_t cand = (size_t)(found - subject);
    if (diag) {
      diag->candidates_skipped += (cand - offset);
      diag->candidates_tested++;
    }

    /* No VM confirmation needed — the ENTIRE pattern is this literal. */
    out_result->success = true;
    out_result->match_start = cand;
    out_result->match_end = cand + lit_len;
    return true;
  }

nomatch:
  out_result->success = false;
  return false;

fallback:
  /* Should never happen if derive_meta correctly set is_literal_only */
  out_result->success = false;
  return false;
}

/* ---------------------------------------------------------------------------
 * Tier 6: Search-VM dispatch
 *
 * Lightweight bytecode interpreter for search-mode-eligible patterns (no
 * side effects, no captures, no output).  Uses computed-goto dispatch with
 * a reduced opcode table supporting only the safe opcode set:
 *
 *   LIT, LEN, ANY, NOTANY, SPAN, BREAK, JMP, SPLIT, POS, RPOS, TAB, RTAB,
 *   ANCHOR, FENCE, NOP, REPEAT_INIT, REPEAT_STEP, ACCEPT, FAIL, ABORT,
 *   SUCCEED
 *
 * Key differences from vm_exec:
 *  - No output buffer (vm->out is unused)
 *  - No capture tracking (cap_start/cap_end arrays skipped)
 *  - No var tracking (var_start/var_end/var_count skipped)
 *  - Range pointers pre-resolved at entry from vm->range_meta
 *  - Backtracking through shared vm_push_choice/vm_pop_choice
 *
 * The caller must reset vm->ip and vm->pos before each call.
 * On success, vm->pos holds the match length (match_end - match_start).
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * Simplified choice stack for search_vm_t (Tier 6 search-VM)
 *
 * The capture-aware search-VM saves/restores capture registers, variable
 * registers, and max_cap_used alongside ip/pos so captures survive backtrack.
 * ---------------------------------------------------------------------------
 */
typedef struct {
  size_t ip;
  size_t pos;
  /* Capture registers (saved/restored on backtrack) */
  size_t cap_start[MAX_CAPS];
  size_t cap_end[MAX_CAPS];
  uint8_t max_cap_used;
  /* Variable registers (saved/restored on backtrack) */
  size_t var_start[MAX_VARS];
  size_t var_end[MAX_VARS];
  size_t var_count;
} search_choice_t;

static inline bool search_vm_push_choice(search_vm_t *vm, size_t ip,
                                         size_t pos) {
  if (!vm->choices) {
    /* Lazily allocate the choice stack on first push so linear (non-
     * backtracking) patterns never pay for it. Room for two frames so the
     * common shallow-backtracking case needs no immediate realloc. */
    size_t initial_cap = 2 * sizeof(search_choice_t);
    vm->choices = snobol_malloc(initial_cap);
    if (!vm->choices) {
      return false;
    }
    vm->choices_cap = initial_cap;
  }
  if (vm->choices_top + sizeof(search_choice_t) >= vm->choices_cap) {
    size_t new_cap =
        vm->choices_cap ? vm->choices_cap * 2 : 16 * sizeof(search_choice_t);
    while (vm->choices_top + sizeof(search_choice_t) >= new_cap) {
      new_cap *= 2;
    }
    void *new_choices = snobol_realloc(vm->choices, new_cap);
    if (!new_choices) {
      return false;
    }
    vm->choices = new_choices;
    vm->choices_cap = new_cap;
  }
  search_choice_t *c =
      (search_choice_t *)((uint8_t *)vm->choices + vm->choices_top);
  c->ip = ip;
  c->pos = pos;
  /* Save capture registers */
  memcpy(c->cap_start, vm->cap_start, sizeof(c->cap_start));
  memcpy(c->cap_end, vm->cap_end, sizeof(c->cap_end));
  c->max_cap_used = vm->max_cap_used;
  /* Save variable registers */
  memcpy(c->var_start, vm->var_start, sizeof(c->var_start));
  memcpy(c->var_end, vm->var_end, sizeof(c->var_end));
  c->var_count = vm->var_count;
  vm->choices_top += sizeof(search_choice_t);
  return true;
}

static inline bool search_vm_pop_choice(search_vm_t *vm) {
  if (!vm->choices || vm->choices_top == 0) {
    return false;
  }
  vm->choices_top -= sizeof(search_choice_t);
  search_choice_t *c =
      (search_choice_t *)((uint8_t *)vm->choices + vm->choices_top);
  vm->ip = c->ip;
  vm->pos = c->pos;
  /* Restore capture registers */
  memcpy(vm->cap_start, c->cap_start, sizeof(vm->cap_start));
  memcpy(vm->cap_end, c->cap_end, sizeof(vm->cap_end));
  vm->max_cap_used = c->max_cap_used;
  /* Restore variable registers */
  memcpy(vm->var_start, c->var_start, sizeof(vm->var_start));
  memcpy(vm->var_end, c->var_end, sizeof(vm->var_end));
  vm->var_count = c->var_count;
  return true;
}

/* ---------------------------------------------------------------------------
 * Initialize a search_vm_t from a full VM struct.
 * Copies only the fields needed by the search-VM (Tier 6).
 * ---------------------------------------------------------------------------
 */
static inline void search_vm_init_from_vm(search_vm_t *svm, const VM *vm) {
  svm->bc = vm->bc;
  svm->bc_len = vm->bc_len;
  svm->s = vm->s;
  svm->len = vm->len;
  svm->ip = vm->ip;
  svm->pos = vm->pos;
  svm->range_meta = vm->range_meta;
  svm->range_meta_count = vm->range_meta_count;
  svm->choices = vm->choices;
  svm->choices_cap = vm->choices_cap;
  svm->choices_top = vm->choices_top;
  svm->use_compact_choice = vm->use_compact_choice;
  memcpy(svm->counters, vm->counters, sizeof(svm->counters));
  memcpy(svm->loop_min, vm->loop_min, sizeof(svm->loop_min));
  memcpy(svm->loop_max, vm->loop_max, sizeof(svm->loop_max));
  memcpy(svm->loop_last_pos, vm->loop_last_pos, sizeof(svm->loop_last_pos));
  svm->max_counter_used = vm->max_counter_used;
  /* Seed capture and variable registers from full VM. Copy only the registers
   * currently in use; the remainder are zeroed so Valgrind cannot flag an
   * access to an uninitialized stack slot if the search-VM wrote a high
   * register without writing all lower ones. */
  {
    memset(svm->cap_start, 0, sizeof(svm->cap_start));
    memset(svm->cap_end, 0, sizeof(svm->cap_end));
    memset(svm->var_start, 0, sizeof(svm->var_start));
    memset(svm->var_end, 0, sizeof(svm->var_end));
    size_t cap_copy = vm->max_cap_used * sizeof(size_t);
    size_t var_copy = vm->var_count * sizeof(size_t);
    memcpy(svm->cap_start, vm->cap_start, cap_copy);
    memcpy(svm->cap_end, vm->cap_end, cap_copy);
    svm->max_cap_used = vm->max_cap_used;
    memcpy(svm->var_start, vm->var_start, var_copy);
    memcpy(svm->var_end, vm->var_end, var_copy);
    svm->var_count = vm->var_count;
  }
}

/* Resolve a charclass set_id to its range data, mirroring the full VM's
 * get_ranges_ptr().  This is the bytecode-embedded fallback used when the
 * caller did not populate vm->range_meta, so the search-VM never silently
 * fails to match a SPAN/BREAK pattern. */
static const uint8_t *search_vm_resolve_range(const search_vm_t *svm,
                                              uint16_t set_id,
                                              uint16_t *out_count,
                                              uint16_t *out_case) {
  VM tmp;
  memset(&tmp, 0, sizeof(tmp));
  tmp.bc = svm->bc;
  tmp.bc_len = svm->bc_len;
  tmp.range_meta = svm->range_meta;
  tmp.range_meta_count = svm->range_meta_count;
  return get_ranges_ptr(&tmp, set_id, out_count, out_case);
}

/* ---------------------------------------------------------------------------
 * Write back search_vm_t state to full VM struct.
 * Copies only the fields that may have changed during search-VM execution.
 * ---------------------------------------------------------------------------
 */
static inline void search_vm_writeback_to_vm(const search_vm_t *svm, VM *vm) {
  vm->ip = svm->ip;
  vm->pos = svm->pos;
  vm->choices = svm->choices;
  vm->choices_cap = svm->choices_cap;
  vm->choices_top = svm->choices_top;
  vm->use_compact_choice = svm->use_compact_choice;
  memcpy(vm->counters, svm->counters, sizeof(vm->counters));
  memcpy(vm->loop_last_pos, svm->loop_last_pos, sizeof(vm->loop_last_pos));
  vm->max_counter_used = svm->max_counter_used;
  /* Write back capture and variable registers to full VM. Copy only the
   * registers actually used during this search. */
  {
    size_t cap_copy = svm->max_cap_used * sizeof(size_t);
    size_t var_copy = svm->var_count * sizeof(size_t);
    memcpy(vm->cap_start, svm->cap_start, cap_copy);
    memcpy(vm->cap_end, svm->cap_end, cap_copy);
    vm->max_cap_used = svm->max_cap_used;
    memcpy(vm->var_start, svm->var_start, var_copy);
    memcpy(vm->var_end, svm->var_end, var_copy);
    vm->var_count = svm->var_count;
  }
}

static bool SNOBOL_HOT search_vm_exec(search_vm_t *SNOBOL_RESTRICT vm,
                                      const char *SNOBOL_RESTRICT subject,
                                      size_t subject_len, size_t offset,
                                      snobol_search_result_t *out_result) {
  (void)subject;
  (void)subject_len;
  const uint8_t *bc = vm->bc;
  size_t bc_len = vm->bc_len;
  size_t ip = vm->ip; /* set by search_reset_vm */
  size_t pos = vm->pos;
  const char *s = vm->s;
  size_t len = vm->len;

  /* ---- Pre-resolve range pointers ---- */
#define SEARCH_VM_MAX_RANGES 64
  struct {
    const uint8_t *ranges_ptr;
    uint16_t count;
    uint16_t case_flag;
  } srange[SEARCH_VM_MAX_RANGES];
  size_t srange_count = 0;

  if (vm->range_meta) {
    srange_count = vm->range_meta_count;
    if (srange_count > SEARCH_VM_MAX_RANGES) {
      srange_count = SEARCH_VM_MAX_RANGES;
    }
    for (size_t i = 0; i < srange_count; i++) {
      srange[i].ranges_ptr = vm->range_meta[i].ranges_ptr;
      srange[i].count = vm->range_meta[i].count;
      srange[i].case_flag = vm->range_meta[i].case_insensitive;
    }
  }

  /* ---- Choice stack is allocated lazily on the first push
   * (search_vm_push_choice). Linear (non-backtracking) patterns therefore pay
   * zero allocation, and the buffer is reused across candidates within a
   * single search — mirroring the full VM's vm_run() strategy. ---- */
  vm->use_compact_choice = true;
  vm->choices_top = 0;
  /* Don't touch compact-choice write log — we don't track captures */

  /* ---- Computed-goto dispatch table ---- */
#ifndef _MSC_VER
  static void *search_opcode_table[] = {
      [OP_ACCEPT] = &&svm_accept,
      [OP_FAIL] = &&svm_fail,
      [OP_JMP] = &&svm_jmp,
      [OP_SPLIT] = &&svm_split,
      [OP_LIT] = &&svm_lit,
      [OP_ANY] = &&svm_any,
      [OP_NOTANY] = &&svm_notany,
      [OP_SPAN] = &&svm_span,
      [OP_BREAK] = &&svm_break,
      [OP_BREAKX] = &&svm_breakx,
      [OP_LEN] = &&svm_len,
      [OP_ANCHOR] = &&svm_anchor,
      [OP_REPEAT_INIT] = &&svm_repeat_init,
      [OP_REPEAT_STEP] = &&svm_repeat_step,
      [OP_FENCE] = &&svm_fence,
      [OP_RPOS] = &&svm_rpos,
      [OP_RTAB] = &&svm_rtab,
      [OP_POS] = &&svm_pos,
      [OP_TAB] = &&svm_tab,
      [OP_ABORT] = &&svm_abort,
      [OP_SUCCEED] = &&svm_succeed,
      [OP_NOP] = &&svm_nop,
      [OP_CAP_START] = &&svm_cap_start,
      [OP_CAP_END] = &&svm_cap_end,
      [OP_ASSIGN] = &&svm_assign,
  };
#endif

  while (1) {
    if (ip >= bc_len) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    uint8_t op = bc[ip++];
#ifndef _MSC_VER
    goto *search_opcode_table[op];
#else
    switch (op) {
#endif

/* ---- Zero-width ops ---- */
#ifndef _MSC_VER
  svm_nop:
#endif
#ifdef _MSC_VER
  case OP_NOP:
#endif
    continue;

#ifndef _MSC_VER
  svm_fence:
#endif
#ifdef _MSC_VER
  case OP_FENCE:
#endif
    /* Cut: discard all prior choice points */
    vm->choices_top = 0;
    continue;

#ifndef _MSC_VER
  svm_anchor:
#endif
#ifdef _MSC_VER
  case OP_ANCHOR:
#endif
  {
    uint8_t anchor_type = bc[ip++];
    bool anchor_ok = ((anchor_type == 0) ? (pos == 0) : (pos == len)) != 0;
    if (!anchor_ok) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    continue;
  }

/* ---- Terminal opcodes ---- */
#ifndef _MSC_VER
  svm_accept:
#endif
#ifdef _MSC_VER
  case OP_ACCEPT:
#endif
    out_result->success = true;
    out_result->match_start = offset;
    out_result->match_end = offset + pos;
    return true;

#ifndef _MSC_VER
  svm_abort:
#endif
#ifdef _MSC_VER
  case OP_ABORT:
#endif
    /* ABORT: immediately terminate the entire match (SNOBOL semantics). */
    out_result->success = false;
    out_result->aborted = true;
    return false;

#ifndef _MSC_VER
  svm_succeed:
#endif
#ifdef _MSC_VER
  case OP_SUCCEED:
#endif
    /* Force success at current position, consuming nothing more */
    out_result->success = true;
    out_result->match_start = offset;
    out_result->match_end = offset + pos;
    return true;

/* ---- Capture-aware ops (Tier 6 search-VM) ---- */
#ifndef _MSC_VER
  svm_cap_start:
#endif
#ifdef _MSC_VER
  case OP_CAP_START:
#endif
  {
    uint8_t r = read_u8(bc, bc_len, &ip);
    if (r < MAX_CAPS) {
      vm->cap_start[r] = pos;
      if (r >= vm->max_cap_used) {
        vm->max_cap_used = r + 1;
      }
    }
    continue;
  }

#ifndef _MSC_VER
  svm_cap_end:
#endif
#ifdef _MSC_VER
  case OP_CAP_END:
#endif
  {
    uint8_t r = read_u8(bc, bc_len, &ip);
    if (r < MAX_CAPS) {
      vm->cap_end[r] = pos;
      if (r >= vm->max_cap_used) {
        vm->max_cap_used = r + 1;
      }
      /* Also expose capture register as variable v<r> */
      if (r < MAX_VARS) {
        vm->var_start[r] = vm->cap_start[r];
        vm->var_end[r] = vm->cap_end[r];
        if ((size_t)r + 1 > vm->var_count) {
          vm->var_count = (size_t)r + 1;
        }
      }
    }
    continue;
  }

#ifndef _MSC_VER
  svm_assign:
#endif
#ifdef _MSC_VER
  case OP_ASSIGN:
#endif
  {
    uint16_t var = read_u16(bc, bc_len, &ip);
    uint8_t r = read_u8(bc, bc_len, &ip);
    if (var < MAX_VARS && r < MAX_CAPS) {
      if (var >= vm->var_count) {
        vm->var_count = (size_t)var + 1;
      }
      vm->var_start[var] = vm->cap_start[r];
      vm->var_end[var] = vm->cap_end[r];
    }
    continue;
  }

#ifndef _MSC_VER
  svm_breakx:
#endif
#ifdef _MSC_VER
  case OP_BREAKX:
#endif
  {
    /* BREAKX: like BREAK but pushes a retry choice point.
       * When the choice is popped, pos advances past the break char. */
    size_t breakx_ip = ip - 1; /* points to OP_BREAKX opcode */
    uint16_t set_id = read_u16(bc, bc_len, &ip);
    const uint8_t *rp = nullptr;
    uint16_t cnt = 0;
    if (set_id > 0 && (size_t)(set_id - 1) < srange_count) {
      rp = srange[set_id - 1].ranges_ptr;
      cnt = srange[set_id - 1].count;
    } else if (set_id > 0 && vm->bc) {
      uint16_t cflag;
      rp = search_vm_resolve_range(vm, set_id, &cnt, &cflag);
    }
    /* Advance past non-break characters (like OP_BREAK) */
    while (pos < len) {
      bool in_class = false;
      if (rp) {
        uint8_t c = (uint8_t)s[pos];
        uint64_t map[2];
        if (c <= 127 && ranges_to_ascii_bitmap(rp, cnt, map) &&
            bitmap_test(map, c)) {
          in_class = true;
        } else {
          uint32_t cp;
          int bytes;
          if (utf8_peek_next(s, len, pos, &cp, &bytes) &&
              range_contains(rp, cnt, cp)) {
            in_class = true;
          }
        }
      }
      if (in_class) {
        break; /* found break char */
      }
      pos++;
    }
    /* If we stopped at a break char, push a retry choice that
       * re-executes BREAKX from the position AFTER the break char */
    if (pos < len) {
      uint32_t bx_cp2;
      int bx_skip = 1;
      if (utf8_peek_next(s, len, pos, &bx_cp2, &bx_skip)) {
        ; /* bx_skip now holds byte count of break char */
      }
      search_vm_push_choice(vm, breakx_ip, pos + (size_t)bx_skip);
    }
    continue;
  }

#ifndef _MSC_VER
  svm_fail:
#endif
#ifdef _MSC_VER
  case OP_FAIL:
#endif
  {
    bool had = search_vm_pop_choice(vm);
    if (!had) {
      goto svm_fail_ret;
    }
    ip = vm->ip;
    pos = vm->pos;
    continue;
  }

/* ---- LIT: inline literal match ---- */
#ifndef _MSC_VER
  svm_lit:
#endif
#ifdef _MSC_VER
  case OP_LIT:
#endif
  {
    uint32_t lit_off = read_u32(bc, bc_len, &ip);
    uint32_t lit_len = read_u32(bc, bc_len, &ip);
    if (lit_off == ip) {
      ip += lit_len;
    }
    if (lit_len <= len - pos && memcmp(s + pos, bc + lit_off, lit_len) == 0) {
      pos += lit_len;
    } else {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
    }
    continue;
  }

/* ---- LEN: advance by N codepoints ---- */
#ifndef _MSC_VER
  svm_len:
#endif
#ifdef _MSC_VER
  case OP_LEN:
#endif
  {
    uint32_t n = read_u32(bc, bc_len, &ip);
    size_t p = pos;
    uint32_t i;
    for (i = 0; i < n; i++) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
    }
    if (i < n) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    pos = p;
    continue;
  }

/* ---- ANY: match single char in class ---- */
#ifndef _MSC_VER
  svm_any:
#endif
#ifdef _MSC_VER
  case OP_ANY:
#endif
  {
    uint16_t set_id = read_u16(bc, bc_len, &ip);
    bool ok = false;
    const uint8_t *rp = nullptr;
    uint16_t cnt = 0;
    if (set_id > 0 && (size_t)(set_id - 1) < srange_count) {
      rp = srange[set_id - 1].ranges_ptr;
      cnt = srange[set_id - 1].count;
    } else if (set_id > 0 && vm->bc) {
      /* Range-resolve fallback from the bytecode trailer, like
       * SPAN/BREAK/BREAKX/NOTANY: callers passing raw bytecode without
       * range_meta must still get a working ANY. */
      uint16_t cflag;
      rp = search_vm_resolve_range(vm, set_id, &cnt, &cflag);
    }
    if (rp) {
      if (pos < len) {
        uint8_t c = (uint8_t)s[pos];
        uint64_t map[2];
        if (c <= 127 && ranges_to_ascii_bitmap(rp, cnt, map) &&
            bitmap_test(map, c)) {
          ok = true;
        } else {
          uint32_t cp;
          int bytes;
          if (utf8_peek_next(s, len, pos, &cp, &bytes) &&
              range_contains(rp, cnt, cp)) {
            ok = true;
          }
        }
      }
    }
    if (ok) {
      /* Advance by 1 byte (ASCII) or multi-byte (UTF-8) */
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, pos, &cp, &bytes)) {
        if (!search_vm_pop_choice(vm)) {
          goto svm_fail_ret;
        }
        ip = vm->ip;
        pos = vm->pos;
        continue;
      }
      pos += bytes;
    } else {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
    }
    continue;
  }

/* ---- NOTANY: match single char NOT in class ---- */
#ifndef _MSC_VER
  svm_notany:
#endif
#ifdef _MSC_VER
  case OP_NOTANY:
#endif
  {
    uint16_t set_id = read_u16(bc, bc_len, &ip);
    bool in_class = false;
    if (set_id > 0 && (size_t)(set_id - 1) < srange_count) {
      const uint8_t *rp = srange[set_id - 1].ranges_ptr;
      uint16_t cnt = srange[set_id - 1].count;
      if (rp && pos < len) {
        uint8_t c = (uint8_t)s[pos];
        uint64_t map[2];
        if (c <= 127 && ranges_to_ascii_bitmap(rp, cnt, map) &&
            bitmap_test(map, c)) {
          in_class = true;
        } else {
          uint32_t cp;
          int bytes;
          if (utf8_peek_next(s, len, pos, &cp, &bytes) &&
              range_contains(rp, cnt, cp)) {
            in_class = true;
          }
        }
      }
    }
    if (!in_class && pos < len) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, pos, &cp, &bytes)) {
        if (!search_vm_pop_choice(vm)) {
          goto svm_fail_ret;
        }
        ip = vm->ip;
        pos = vm->pos;
        continue;
      }
      pos += bytes;
    } else {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
    }
    continue;
  }

/* ---- SPAN: consume 1+ characters in class ---- */
#ifndef _MSC_VER
  svm_span:
#endif
#ifdef _MSC_VER
  case OP_SPAN:
#endif
  {
    uint16_t set_id = read_u16(bc, bc_len, &ip);
    const uint8_t *rp = nullptr;
    uint16_t cnt = 0;
    if (set_id > 0 && (size_t)(set_id - 1) < srange_count) {
      rp = srange[set_id - 1].ranges_ptr;
      cnt = srange[set_id - 1].count;
    } else if (set_id > 0 && vm->bc) {
      /* Fallback to bytecode-embedded ranges (mirrors the full VM's
         * get_ranges_ptr).  Keeps the search-VM correct even when the
         * caller did not populate vm->range_meta. */
      uint16_t cflag;
      rp = search_vm_resolve_range(vm, set_id, &cnt, &cflag);
    }
    size_t start = pos;
    /* All-ASCII classes are matched BYTE-wise, mirroring the full VM:
     * any byte > 127 stops the run (its UTF-8 codepoint must not be
     * considered, as an invalid sequence could decode into a class byte).
     * Non-ASCII classes use codepoint-wise membership. */
    uint64_t amap[2];
    bool ascii_class = (rp && ranges_to_ascii_bitmap(rp, cnt, amap)) != 0;
    while (pos < len) {
      bool in_class = false;
      if (rp) {
        if (ascii_class) {
          uint8_t c = (uint8_t)s[pos];
          in_class = (((c <= 127) && bitmap_test(amap, c)) != 0);
        } else {
          uint32_t cp;
          int bytes;
          if (utf8_peek_next(s, len, pos, &cp, &bytes) &&
              range_contains(rp, cnt, cp)) {
            in_class = true;
          }
        }
      }
      if (!in_class) {
        break;
      }
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, pos, &cp, &bytes)) {
        break;
      }
      pos += bytes;
    }
    if (pos == start) {
      /* SPAN requires at least 1 char */
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
    }
    continue;
  }

/* ---- BREAK: consume 0+ characters until one in class ---- */
#ifndef _MSC_VER
  svm_break:
#endif
#ifdef _MSC_VER
  case OP_BREAK:
#endif
  {
    uint16_t set_id = read_u16(bc, bc_len, &ip);
    const uint8_t *rp = nullptr;
    uint16_t cnt = 0;
    if (set_id > 0 && (size_t)(set_id - 1) < srange_count) {
      rp = srange[set_id - 1].ranges_ptr;
      cnt = srange[set_id - 1].count;
    } else if (set_id > 0 && vm->bc) {
      uint16_t cflag;
      rp = search_vm_resolve_range(vm, set_id, &cnt, &cflag);
    }
    while (pos < len) {
      bool in_class = false;
      if (rp) {
        uint8_t c = (uint8_t)s[pos];
        uint64_t map[2];
        if (c <= 127 && ranges_to_ascii_bitmap(rp, cnt, map) &&
            bitmap_test(map, c)) {
          in_class = true;
        } else {
          uint32_t cp;
          int bytes;
          if (utf8_peek_next(s, len, pos, &cp, &bytes) &&
              range_contains(rp, cnt, cp)) {
            in_class = true;
          }
        }
      }
      if (in_class) {
        break;
      }
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, pos, &cp, &bytes)) {
        break;
      }
      pos += bytes;
    }
    continue;
  }

/* ---- POS: succeed at exact byte offset ---- */
#ifndef _MSC_VER
  svm_pos:
#endif
#ifdef _MSC_VER
  case OP_POS:
#endif
  {
    uint32_t n = read_u32(bc, bc_len, &ip);
    if (pos != (size_t)n) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    continue;
  }

/* ---- RPOS: succeed at N codepoints from end ---- */
#ifndef _MSC_VER
  svm_rpos:
#endif
#ifdef _MSC_VER
  case OP_RPOS:
#endif
  {
    uint32_t n = read_u32(bc, bc_len, &ip);
    /* Count remaining codepoints from pos to end */
    size_t p = pos;
    uint32_t remaining = 0;
    while (p < len) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
      remaining++;
    }
    if (remaining != n) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    continue;
  }

/* ---- TAB: advance to exact byte offset ---- */
#ifndef _MSC_VER
  svm_tab:
#endif
#ifdef _MSC_VER
  case OP_TAB:
#endif
  {
    uint32_t n = read_u32(bc, bc_len, &ip);
    /* Count codepoints from start to n, advance pos to that byte */
    size_t p = 0;
    uint32_t i;
    for (i = 0; i < n; i++) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
    }
    if (i < n || p > len) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    pos = p;
    continue;
  }

/* ---- RTAB: advance to N codepoints from end ---- */
#ifndef _MSC_VER
  svm_rtab:
#endif
#ifdef _MSC_VER
  case OP_RTAB:
#endif
  {
    uint32_t n = read_u32(bc, bc_len, &ip);
    /* Count total codepoints */
    size_t p = 0;
    uint32_t total_cp = 0;
    while (p < len) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
      total_cp++;
    }
    if (n > total_cp) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    /* Advance to (total_cp - n)th codepoint */
    uint32_t target = total_cp - n;
    p = 0;
    uint32_t i;
    for (i = 0; i < target; i++) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(s, len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
    }
    if (i < target) {
      if (!search_vm_pop_choice(vm)) {
        goto svm_fail_ret;
      }
      ip = vm->ip;
      pos = vm->pos;
      continue;
    }
    pos = p;
    continue;
  }

/* ---- JMP: unconditional jump ---- */
#ifndef _MSC_VER
  svm_jmp:
#endif
#ifdef _MSC_VER
  case OP_JMP:
#endif
  {
    uint32_t tgt = read_u32(bc, bc_len, &ip);
    ip = (size_t)tgt;
    continue;
  }

/* ---- SPLIT: non-deterministic branch ---- */
#ifndef _MSC_VER
  svm_split:
#endif
#ifdef _MSC_VER
  case OP_SPLIT:
#endif
  {
    uint32_t a = read_u32(bc, bc_len, &ip);
    uint32_t b = read_u32(bc, bc_len, &ip);
    search_vm_push_choice(vm, (size_t)b, pos);
    ip = (size_t)a;
    continue;
  }

/* ---- REPEAT_INIT: begin bounded repetition ---- */
#ifndef _MSC_VER
  svm_repeat_init:
#endif
#ifdef _MSC_VER
  case OP_REPEAT_INIT:
#endif
  {
    uint8_t loop_id = read_u8(bc, bc_len, &ip);
    uint32_t min = read_u32(bc, bc_len, &ip);
    uint32_t max = read_u32(bc, bc_len, &ip);
    uint32_t skip_target = read_u32(bc, bc_len, &ip);
    if (loop_id < MAX_LOOPS) {
      vm->counters[loop_id] = 0;
      vm->loop_min[loop_id] = min;
      vm->loop_max[loop_id] = max;
      vm->loop_last_pos[loop_id] = pos;
      if (loop_id + 1 > vm->max_counter_used) {
        vm->max_counter_used = loop_id + 1;
      }
      /* Allow 0-iteration exit only when min is 0 */
      if (min == 0) {
        search_vm_push_choice(vm, (size_t)skip_target, pos);
      }
    }
    continue;
  }

/* ---- REPEAT_STEP: step bounded repetition ---- */
#ifndef _MSC_VER
  svm_repeat_step:
#endif
#ifdef _MSC_VER
  case OP_REPEAT_STEP:
#endif
  {
    uint8_t loop_id = read_u8(bc, bc_len, &ip);
    uint32_t jmp_target = read_u32(bc, bc_len, &ip);
    if (loop_id < MAX_LOOPS) {
      vm->counters[loop_id]++;
      uint32_t count = vm->counters[loop_id];
      uint32_t min = vm->loop_min[loop_id];
      uint32_t max = vm->loop_max[loop_id];
      if (count < min) {
        /* Must satisfy the minimum: keep looping, no choice */
        vm->loop_last_pos[loop_id] = pos;
        ip = (size_t)jmp_target;
      } else if (max == (uint32_t)-1 || count < max) {
        if (max == (uint32_t)-1 && pos == vm->loop_last_pos[loop_id]) {
          /* Unbounded and no forward progress: exit without looping */
        } else {
          /* Push the EXIT choice (post-step instruction); fall through
             * to continue looping. If the body later fails, we backtrack
             * to this exit point. */
          search_vm_push_choice(vm, ip, pos);
          vm->loop_last_pos[loop_id] = pos;
          ip = (size_t)jmp_target;
        }
      }
      /* else count >= max: fall through to exit */
    }
    continue;
  }

#ifdef _MSC_VER
  }
#endif
}

svm_fail_ret: out_result->success = false;
return false;
}
#undef SEARCH_VM_MAX_RANGES

/* ---------------------------------------------------------------------------
 * Tier 8: Lightweight automaton path
 *
 * For automaton-eligible patterns (no side effects, no complex control flow,
 * ASCII-only), we use a simplified execution that avoids choice-stack overhead
 * for the common case.  Falls back to the general VM on any branching op
 * (SPLIT).
 *
 * The automaton runs from `offset` and returns true when the pattern matches.
 * Captures are populated in the VM state on success.
 * ---------------------------------------------------------------------------
 */
/* ---------------------------------------------------------------------------
 * Automaton (DFA) — NFA-to-DFA construction
 *
 * We treat the search-VM bytecode as an NFA and apply subset construction.
 * Each VM offset is an NFA state; epsilon transitions come from JMP, SPLIT,
 * and zero-width ops.  The DFA is a deterministic state machine with
 * 256-entry-per-state transition tables indexed by input byte.
 *
 * Because we only handle ascii_class_only patterns, every consuming op
 * operates on single bytes (no UTF-8 multi-byte sequences).
 * ---------------------------------------------------------------------------
 */

/* ---- NFA modelling helpers ---- */

/** Maximum NFA states (VM bytecode offsets) we track. */
enum { DFA_NFA_MAX = 512 };

/** A set of NFA states (VM offsets), stored as a sorted u16 array. */
typedef struct {
  uint16_t states[DFA_NFA_MAX];
  uint16_t count;
} nfa_set_t;

/** Reset a set to empty. */
static inline void nfa_set_clear(nfa_set_t *s) {
  s->count = 0;
}

/** Insert a value into a sorted set (no-op if already present). */
static inline void nfa_set_add(nfa_set_t *s, uint16_t v) {
  uint16_t i = 0;
  while (i < s->count && s->states[i] < v) {
    i++;
  }
  if (i < s->count && s->states[i] == v) {
    return;
  }
  if (s->count >= DFA_NFA_MAX) {
    return;
  }
  memmove(&s->states[i + 1], &s->states[i],
          (size_t)(s->count - i) * sizeof(uint16_t));
  s->states[i] = v;
  s->count++;
}

/** Return true if the set contains v. */
static inline bool nfa_set_contains(const nfa_set_t *s, uint16_t v) {
  for (uint16_t i = 0; i < s->count; i++) {
    if (s->states[i] == v) {
      return true;
    }
  }
  return false;
}

/** @brief Test bit b of a 256-bit byte class (uint64_t[4] view). */
static inline bool dfa_class_bit(const uint64_t bs[4], unsigned b) {
  if (b < 64) {
    return (bs[0] & (1ULL << b)) != 0;
  }
  if (b < 128) {
    return (bs[1] & (1ULL << (b - 64))) != 0;
  }
  if (b < 192) {
    return (bs[2] & (1ULL << (b - 128))) != 0;
  }
  return (bs[3] & (1ULL << (b - 192))) != 0;
}

/** Copy a set. */
static inline void nfa_set_copy(nfa_set_t *dst, const nfa_set_t *src) {
  dst->count = src->count;
  memcpy(dst->states, src->states, src->count * sizeof(uint16_t));
}

/** Compare two sets for equality. */
static inline bool nfa_set_eq(const nfa_set_t *a, const nfa_set_t *b) {
  if (a->count != b->count) {
    return false;
  }
  for (uint16_t i = 0; i < a->count; i++) {
    if (a->states[i] != b->states[i]) {
      return false;
    }
  }
  return true;
}

/*
 * Get the bytecode opcode at the given offset.  Returns OP_NOP for out-of-range.
 * The offset is treated as an absolute bytecode position.
 */

/**
 * Compute epsilon-closure: all NFA states reachable from `start` via
 * epsilon transitions (JMP, both SPLIT branches, zero-width ops).
 *
 * Iterates until a fixed point is reached.
 */
static void epsilon_closure(const uint8_t *bc, size_t bc_len, uint16_t start,
                            nfa_set_t *out) {
  nfa_set_clear(out);
  nfa_set_add(out, start);
  bool changed = true;
  while (changed) {
    changed = false;
    for (uint16_t si = 0; si < out->count; si++) {
      uint16_t off = out->states[si];
      if (off >= bc_len) {
        continue;
      }
      uint8_t op = bc[off];
      size_t ip = (size_t)off;

      switch (op) {

        case OP_JMP: {
          /* JMP target (u32) is an epsilon transition */
          if (ip + 5 <= bc_len) {
            uint32_t t = search_read_u32(bc, ip + 1);
            if (t < bc_len && !nfa_set_contains(out, (uint16_t)t)) {
              nfa_set_add(out, (uint16_t)t);
              changed = true;
            }
          }
          break;
        }

        case OP_SPLIT: {
          /* Both SPLIT targets are epsilon transitions */
          if (ip + 9 <= bc_len) {
            uint32_t a = search_read_u32(bc, ip + 1);
            uint32_t b = search_read_u32(bc, ip + 5);
            if (a < bc_len && !nfa_set_contains(out, (uint16_t)a)) {
              nfa_set_add(out, (uint16_t)a);
              changed = true;
            }
            if (b < bc_len && !nfa_set_contains(out, (uint16_t)b)) {
              nfa_set_add(out, (uint16_t)b);
              changed = true;
            }
          }
          break;
        }

        /* Zero-width ops — epsilon transition to next instruction */
        case OP_NOP:
          if (!nfa_set_contains(out, (uint16_t)(off + 1))) {
            nfa_set_add(out, (uint16_t)(off + 1));
            changed = true;
          }
          break;

        case OP_FENCE:
          if (!nfa_set_contains(out, (uint16_t)(off + 1))) {
            nfa_set_add(out, (uint16_t)(off + 1));
            changed = true;
          }
          break;

        case OP_LIT: {
          /* A zero-length literal matches the empty string, consuming no input,
         * so it is an epsilon transition to the instruction that follows it.
         * Without this, patterns such as '' never mark their start state as
         * accepting and the automaton reports no match. */
          if (ip + 9 <= bc_len) {
            uint32_t lit_len = search_read_u32(bc, ip + 5);
            if (lit_len == 0 && !nfa_set_contains(out, (uint16_t)(off + 9))) {
              nfa_set_add(out, (uint16_t)(off + 9));
              changed = true;
            }
          }
          break;
        }

        case OP_ANCHOR:
          if (ip + 2 <= bc_len && !nfa_set_contains(out, (uint16_t)(off + 2))) {
            nfa_set_add(out, (uint16_t)(off + 2));
            changed = true;
          }
          break;

        case OP_POS:
        case OP_RPOS:
        case OP_TAB:
        case OP_RTAB:
          if (ip + 5 <= bc_len && !nfa_set_contains(out, (uint16_t)(off + 5))) {
            nfa_set_add(out, (uint16_t)(off + 5));
            changed = true;
          }
          break;

        case OP_REPEAT_INIT:
          /* REPEAT_INIT: the skip_target is an epsilon transition to exit
         * the loop; fall-through to the loop body is also epsilon. */
          if (ip + 14 <= bc_len) {
            uint32_t skip =
                search_read_u32(bc, ip + 9); /* after loop_id, min, max */
            if (skip < bc_len && !nfa_set_contains(out, (uint16_t)skip)) {
              nfa_set_add(out, (uint16_t)skip);
              changed = true;
            }
            /* Also fall-through to the next instruction (the loop body) */
            if (!nfa_set_contains(out, (uint16_t)(off + 14))) {
              nfa_set_add(out, (uint16_t)(off + 14));
              changed = true;
            }
          }
          break;

        case OP_BREAK:
          /* BREAK has an epsilon transition to the next instruction (exit).
         * This means the DFA can exit BREAK without consuming a delimiter byte. */
          if (ip + 3 <= bc_len && !nfa_set_contains(out, (uint16_t)(off + 3))) {
            nfa_set_add(out, (uint16_t)(off + 3));
            changed = true;
          }
          break;

        case OP_REPEAT_STEP: {
          /* REPEAT_STEP: epsilon to jmp_target (next iteration) */
          if (ip + 6 <= bc_len) {
            uint32_t jmp_tgt = search_read_u32(bc, ip + 1); /* after loop_id */
            if (jmp_tgt < bc_len && !nfa_set_contains(out, (uint16_t)jmp_tgt)) {
              nfa_set_add(out, (uint16_t)jmp_tgt);
              changed = true;
            }
          }
          break;
        }

        default: break;
      }
    }
  }
}

/**
 * Check if an NFA state (VM offset) contains ACCEPT.
 */
/* True when the NFA offset is an OP_ACCEPT *instruction* (not a literal
 * data byte).  Literal payload bytes can equal 0x00 (OP_ACCEPT's opcode),
 * so the lit_next table — which maps literal-data offsets to their
 * continuation and is DEAD for instruction offsets — distinguishes them:
 * a NUL literal-data byte must not look like an accepting instruction. */
static inline bool nfa_is_accept(const uint8_t *bc, size_t bc_len, uint16_t off,
                                 const uint16_t *lit_next) {
  return (off < bc_len && bc[off] == OP_ACCEPT &&
          (lit_next == NULL || lit_next[off] == SNOBOL_DFA_DEAD)) != 0;
}

/**
 * For a consuming op at VM offset `off`, mark the bytes that trigger a
 * transition in `byte_set` (256-bit bitmap, as 4 uint64_ts).
 *
 * Returns true if there is a consuming transition; false for non-consuming ops.
 * For LEN and other ops that match any byte, all 256 bits are set.
 */
static bool get_op_bytes(const uint8_t *bc, size_t bc_len, uint16_t off,
                         const VM *dfa_vm, uint64_t byte_set[4]) {
  memset(byte_set, 0, 4 * sizeof(uint64_t));

  if (off >= bc_len) {
    return false;
  }
  uint8_t op = bc[off];
  size_t ip = (size_t)off;

  switch (op) {

    case OP_LIT: {
      /* LIT matches a specific sequence of bytes.  The first byte of the
     * literal determines the consuming transition from this offset. */
      if (ip + 9 > bc_len) {
        return false;
      }
      uint32_t lit_off = search_read_u32(bc, ip + 1);
      uint32_t lit_len = search_read_u32(bc, ip + 5);
      if (lit_off >= bc_len || lit_len == 0) {
        return false;
      }
      uint8_t b = bc[lit_off];
      if (b < 64) {
        byte_set[0] |= (1ULL << b);
      } else if (b < 128) {
        byte_set[1] |= (1ULL << (b - 64));
      } else if (b < 192) {
        byte_set[2] |= (1ULL << (b - 128));
      } else {
        byte_set[3] |= (1ULL << (b - 192));
      }
      return true;
    }

    case OP_ANY: {
      if (ip + 3 > bc_len) {
        return false;
      }
      uint16_t set_id = search_read_u16(bc, ip + 1);
      if (set_id == 0 || (size_t)(set_id - 1) >= dfa_vm->range_meta_count) {
        return false;
      }
      const uint8_t *rp = dfa_vm->range_meta[set_id - 1].ranges_ptr;
      uint16_t cnt = dfa_vm->range_meta[set_id - 1].count;
      if (!rp || cnt == 0) {
        memset(byte_set, 0xFF, 4 * sizeof(uint64_t));
        return true;
      }
      uint64_t abm[2] = {0, 0};
      ranges_to_ascii_bitmap(rp, cnt, abm);
      /* Copy 128-bit ASCII bitmap into 256-bit byte_set */
      byte_set[0] = abm[0];
      byte_set[1] = abm[1];
      return true;
    }

    case OP_NOTANY: {
      if (ip + 3 > bc_len) {
        return false;
      }
      uint16_t set_id = search_read_u16(bc, ip + 1);
      if (set_id == 0 || (size_t)(set_id - 1) >= dfa_vm->range_meta_count) {
        return false;
      }
      const uint8_t *rp = dfa_vm->range_meta[set_id - 1].ranges_ptr;
      uint16_t cnt = dfa_vm->range_meta[set_id - 1].count;
      if (!rp || cnt == 0) {
        memset(byte_set, 0xFF, 4 * sizeof(uint64_t));
        return true;
      }
      uint64_t abm[2] = {0, 0};
      ranges_to_ascii_bitmap(rp, cnt, abm);
      /* Invert: NOTANY matches bytes NOT in the class */
      byte_set[0] = ~abm[0];
      byte_set[1] = ~abm[1];
      byte_set[2] = ~0ULL;
      byte_set[3] = ~0ULL;
      return true;
    }

    case OP_SPAN: {
      /* SPAN first-byte transition: same as ANY */
      if (ip + 3 > bc_len) {
        return false;
      }
      uint16_t set_id = search_read_u16(bc, ip + 1);
      if (set_id == 0 || (size_t)(set_id - 1) >= dfa_vm->range_meta_count) {
        return false;
      }
      const uint8_t *rp = dfa_vm->range_meta[set_id - 1].ranges_ptr;
      uint16_t cnt = dfa_vm->range_meta[set_id - 1].count;
      if (!rp || cnt == 0) {
        memset(byte_set, 0xFF, 4 * sizeof(uint64_t));
        return true;
      }
      uint64_t abm[2] = {0, 0};
      ranges_to_ascii_bitmap(rp, cnt, abm);
      byte_set[0] = abm[0];
      byte_set[1] = abm[1];
      return true;
    }

    case OP_BREAK: {
      /* BREAK first-byte transition: bytes NOT in the class (matches the
     * delimiter, so the transition would be triggered by a delimiter byte).
     * Actually, BREAK consumes 0+ bytes until a delimiter.  The first
     * consuming step is: if current byte IS in the class, BREAK matches
     * 0 bytes (consuming nothing) and control passes to the next op.
     * If current byte is NOT in the class, BREAK consumes 1 byte.
     * Wait — BREAK matches 0+ bytes until a delimiter, so it consumes
     * everything up to the delimiter.  The transition from the BREAK op
     * itself is: "if not delimiter, consume one byte and stay". */
      if (ip + 3 > bc_len) {
        return false;
      }
      uint16_t set_id = search_read_u16(bc, ip + 1);
      if (set_id == 0 || (size_t)(set_id - 1) >= dfa_vm->range_meta_count) {
        return false;
      }
      const uint8_t *rp = dfa_vm->range_meta[set_id - 1].ranges_ptr;
      uint16_t cnt = dfa_vm->range_meta[set_id - 1].count;
      if (!rp || cnt == 0) {
        memset(byte_set, 0xFF, 4 * sizeof(uint64_t));
        return true;
      }
      uint64_t abm[2] = {0, 0};
      ranges_to_ascii_bitmap(rp, cnt, abm);
      /* BREAK loop: stay in BREAK while byte is NOT in the delimiter set.
     * The consuming transition (stay-in-loop) eats bytes outside the set. */
      byte_set[0] = ~abm[0];
      byte_set[1] = ~abm[1];
      byte_set[2] = ~0ULL;
      byte_set[3] = ~0ULL;
      return true;
    }

    case OP_LEN: {
      /* LEN(n): matches any byte (first byte of N). */
      memset(byte_set, 0xFF, 4 * sizeof(uint64_t));
      return true;
    }

    default: return false; /* Not a consuming op */
  }
}

/*
 * Fast epsilon-closure merge: adds all states from the pre-computed
 * epsilon closure of `off` into `out`.  Uses the table built by
 * build_dfa so we avoid re-computing the fixed point each time.
 */
static inline void ec_merge(const nfa_set_t *eps_closures, uint16_t off,
                            nfa_set_t *out) {
  const nfa_set_t *src = &eps_closures[off];
  for (uint16_t i = 0; i < src->count; i++) {
    nfa_set_add(out, src->states[i]);
  }
}

/* ---- DFA state hash table for deduplication ---- */

/* Simple modulo hash for a DFA state: hash the nfa_set_t contents. */
static uint32_t dfa_state_hash(const nfa_set_t *s) {
  uint32_t h = 0;
  for (uint16_t i = 0; i < s->count; i++) {
    h = (h * 31) + s->states[i];
  }
  return h;
}

/* A map from nfa_set_t -> DFA state index, using open addressing. */
enum { DFA_HASH_CAP = 8192 };

typedef struct {
  nfa_set_t key;
  uint16_t dfa_state; /**< DFA state index */
  bool occupied;
} dfa_hash_entry_t;

static void dfa_hash_init(dfa_hash_entry_t *ht, size_t cap) {
  memset(ht, 0, cap * sizeof(dfa_hash_entry_t));
}

static uint16_t dfa_hash_find_or_insert(dfa_hash_entry_t *ht, size_t cap,
                                        const nfa_set_t *s, uint16_t *next_id) {
  if (s->count == 0) {
    return SNOBOL_DFA_DEAD;
  }
  uint32_t h = dfa_state_hash(s);
  for (size_t i = 0; i < cap; i++) {
    size_t idx = (h + i) % cap;
    if (!ht[idx].occupied) {
      /* Insert */
      nfa_set_copy(&ht[idx].key, s);
      ht[idx].dfa_state = *next_id;
      ht[idx].occupied = true;
      uint16_t id = *next_id;
      (*next_id)++;
      return id;
    }
    if (nfa_set_eq(&ht[idx].key, s)) {
      return ht[idx].dfa_state;
    }
  }
  return SNOBOL_DFA_DEAD; /* Table full */
}

/* ---- Main DFA builder ---- */

/**
 * Build a DFA from search-VM-eligible bytecode.
 *
 * Returns the constructed DFA on success, or NULL on failure (state
 * explosion, allocation failure, etc.).  The caller owns the DFA and
 * must free it via snobol_dfa_free().
 *
 * @param bc       Bytecode buffer (search-VM-eligible subset)
 * @param bc_len   Bytecode length
 * @param dfa_vm   VM with range_meta populated for charclass resolution
 * @return Allocated DFA, or NULL on failure
 */
snobol_dfa_t *build_dfa(const uint8_t *bc, size_t bc_len, const VM *dfa_vm) {
  if (!bc || bc_len == 0) {
    return nullptr;
  }
  /* NFA state indices are uint16_t — reject oversized bytecode instead of
   * truncating offsets (callers fall back to the VM). */
  if (bc_len > UINT16_MAX) {
    return nullptr;
  }

  /* ---- Pre-scan bytecode for multi-byte LIT data positions ---- */
  /* Build a table mapping each literal data byte offset to its continuation:
   *   lit_next[pos] = next position after matching byte at pos
   * For the last byte of a LIT, this is the instruction after the LIT.
   * For intermediate bytes, it's the next byte of the literal data.
   * Positions not within literal data stay SNOBOL_DFA_DEAD.
   */
  nfa_set_t *eps_closures = nullptr;
  snobol_dfa_t *dfa = nullptr;
  dfa_hash_entry_t *ht = nullptr;
  uint16_t *queue = nullptr;
  uint16_t *lit_next = (uint16_t *)snobol_calloc(bc_len, sizeof(uint16_t));
  if (!lit_next) {
    goto fail;
  }
  for (size_t i = 0; i < bc_len; i++) {
    lit_next[i] = SNOBOL_DFA_DEAD;
  }
  {
    size_t ip = 0;
    while (ip < bc_len) {
      uint8_t op = bc[ip];
      size_t next_ip = ip + 1;
      if (op == OP_LIT && ip + 9 <= bc_len) {
        uint32_t lo = search_read_u32(bc, ip + 1);
        uint32_t ll = search_read_u32(bc, ip + 5);
        next_ip = ip + 9 + ll;
        if (ll > 1 && lo < bc_len && lo + ll <= bc_len) {
          for (uint32_t k = 0; k < ll; k++) {
            uint16_t pos = (uint16_t)(lo + k);
            if (k == ll - 1) {
              /* Last byte → next instruction after LIT */
              lit_next[pos] = (uint16_t)next_ip;
            } else {
              lit_next[pos] = (uint16_t)(lo + k + 1);
            }
          }
        }
      } else if (op == OP_LEN && ip + 5 <= bc_len &&
                 search_read_u32(bc, ip + 1) > 1) {
        /* For LEN(n > 1): we chain through virtual offsets beyond bc_len.
         * Handled during transition computation using a tracking counter. */
        next_ip = ip + 5;
      } else if (op == OP_ACCEPT || op == OP_ABORT || op == OP_SUCCEED) {
        /* Don't break — later code may be reachable via epsilon edges (SPLIT).
         * Just advance by 1 (these are 1-byte instructions). */
      } else if (op == OP_JMP && ip + 5 <= bc_len) {
        next_ip = search_read_u32(bc, ip + 1);
      } else if (op == OP_SPLIT && ip + 9 <= bc_len) {
        /* Don't scan past SPLIT; epsilon closure handles branching */
        ip++; /* scan byte-by-byte */
        continue;
      } else if (op == OP_NOP || op == OP_FENCE) {
        /* no-op: advance 1 */
      } else if (op == OP_ANCHOR) {
        next_ip = ip + 2;
      } else if (op == OP_POS || op == OP_RPOS || op == OP_TAB ||
                 op == OP_RTAB) {
        next_ip = ip + 5;
      } else if (op == OP_ANY || op == OP_NOTANY || op == OP_SPAN ||
                 op == OP_BREAK) {
        next_ip = ip + 3;
      } else if (op == OP_REPEAT_INIT) {
        next_ip = ip + 14;
      } else if (op == OP_REPEAT_STEP) {
        next_ip = ip + 6;
      } else if (op == OP_CAP_START || op == OP_CAP_END) {
        next_ip = ip + 2;
      } else {
        next_ip = ip + 1;
      }
      ip = next_ip > ip ? next_ip : ip + 1;
    }
  }

  /* Pre-compute epsilon closure for every VM offset up to bc_len */
  eps_closures = (nfa_set_t *)snobol_calloc(bc_len, sizeof(nfa_set_t));
  if (!eps_closures) {
    goto fail;
  }

  for (size_t off = 0; off < bc_len; off++) {
    uint8_t op = bc[off];
    if (op == OP_ACCEPT || op == OP_ABORT || op == OP_SUCCEED) {
      nfa_set_clear(&eps_closures[off]);
      nfa_set_add(&eps_closures[off], (uint16_t)off);
    } else {
      epsilon_closure(bc, bc_len, (uint16_t)off, &eps_closures[off]);
    }
  }

  /* Allocate DFA */
  dfa = (snobol_dfa_t *)snobol_calloc(1, sizeof(snobol_dfa_t));
  if (!dfa) {
    goto fail;
  }

  dfa->state_cap = 64;
  dfa->trans =
      (uint16_t *)snobol_calloc((size_t)dfa->state_cap * 256, sizeof(uint16_t));
  if (!dfa->trans) {
    goto fail;
  }
  dfa->accepting = (uint8_t *)snobol_calloc((dfa->state_cap + 7) / 8, 1);
  if (!dfa->accepting) {
    goto fail;
  }
  dfa->num_states = 0;
  dfa->start_state = SNOBOL_DFA_DEAD;

  /* Hash table for mapping NFA sets -> DFA states.
   * Must be heap-allocated (≈4.2 MB) to avoid stack overflow. */
  ht =
      (dfa_hash_entry_t *)snobol_calloc(DFA_HASH_CAP, sizeof(dfa_hash_entry_t));
  if (!ht) {
    goto fail;
  }
  dfa_hash_init(ht, DFA_HASH_CAP);

  uint16_t next_dfa_id = 0;

  /* Start state = epsilon closure of offset 0 */
  nfa_set_t start_set;
  epsilon_closure(bc, bc_len, 0, &start_set);
  if (start_set.count == 0) {
    goto fail;
  }

  dfa->start_state =
      dfa_hash_find_or_insert(ht, DFA_HASH_CAP, &start_set, &next_dfa_id);

  /* BFS queue of DFA states to process */
  size_t q_head = 0;
  size_t q_tail = 0;
  size_t q_cap = 512;
  queue = (uint16_t *)snobol_malloc(q_cap * sizeof(uint16_t));
  if (!queue) {
    goto fail;
  }
  queue[q_tail++] = dfa->start_state;
  dfa->num_states = next_dfa_id;

#define DFA_ENSURE(s)                                                      \
  do {                                                                     \
    if ((s) >= dfa->state_cap) {                                           \
      uint32_t new_cap = dfa->state_cap * 2;                               \
      if (new_cap > SNOBOL_DFA_MAX_STATES)                                 \
        new_cap = SNOBOL_DFA_MAX_STATES;                                   \
      if ((s) >= new_cap)                                                  \
        goto fail;                                                         \
      uint16_t *nt = (uint16_t *)snobol_realloc(                           \
          dfa->trans, (size_t)new_cap * 256 * sizeof(uint16_t));           \
      if (!nt)                                                             \
        goto fail;                                                         \
      memset(nt + ((size_t)dfa->state_cap * 256), 0,                       \
             (size_t)(new_cap - dfa->state_cap) * 256 * sizeof(uint16_t)); \
      dfa->trans = nt;                                                     \
      uint8_t *na =                                                        \
          (uint8_t *)snobol_realloc(dfa->accepting, (new_cap + 7) / 8);    \
      if (!na)                                                             \
        goto fail;                                                         \
      memset(na + (dfa->state_cap + 7) / 8, 0,                             \
             ((new_cap + 7) / 8) - ((dfa->state_cap + 7) / 8));            \
      dfa->accepting = na;                                                 \
      dfa->state_cap = new_cap;                                            \
    }                                                                      \
  } while (0)

  while (q_head < q_tail && dfa->num_states < SNOBOL_DFA_MAX_STATES) {
    uint16_t cur_dfa = queue[q_head++];
    DFA_ENSURE(cur_dfa);

    const nfa_set_t *cur_set = nullptr;
    for (size_t i = 0; i < DFA_HASH_CAP; i++) {
      if (ht[i].occupied && ht[i].dfa_state == cur_dfa) {
        cur_set = &ht[i].key;
        break;
      }
    }
    if (!cur_set) {
      continue;
    }

    bool is_accepting = false;
    for (uint16_t si = 0; si < cur_set->count && !is_accepting; si++) {
      if (nfa_is_accept(bc, bc_len, cur_set->states[si], lit_next)) {
        is_accepting = true;
      }
    }
    if (is_accepting) {
      dfa->accepting[cur_dfa / 8] |= (uint8_t)(1U << (cur_dfa % 8));
    }

    /* Compute byte-by-byte transitions (union across all NFA states) */
    for (int b = 0; b < 256; b++) {
      nfa_set_t next_set;
      nfa_set_clear(&next_set);

      for (uint16_t si = 0; si < cur_set->count; si++) {
        uint16_t off = cur_set->states[si];
        if (off >= bc_len) {
          continue;
        }

        /* LIT data byte positions — handle before opcode switch so data
         * bytes that happen to match opcode values aren't misinterpreted. */
        if (lit_next[off] != SNOBOL_DFA_DEAD) {
          if ((uint8_t)b == bc[off]) {
            uint16_t cont = lit_next[off];
            if (cont < bc_len && cont != off &&
                lit_next[cont] != SNOBOL_DFA_DEAD) {
              /* cont is another LIT data byte — just add it */
              nfa_set_add(&next_set, cont);
            } else {
              /* cont is a real instruction — merge its epsilon closure */
              ec_merge(eps_closures, cont, &next_set);
            }
          }
          continue;
        }

        uint8_t op = bc[off];

        switch (op) {

          /* ---- LIT ---- */
          case OP_LIT: {
            if ((size_t)off + 9 > bc_len) {
              break;
            }
            uint32_t lit_off = search_read_u32(bc, (size_t)off + 1);
            uint32_t lit_len = search_read_u32(bc, (size_t)off + 5);
            if (lit_off >= bc_len || lit_len == 0) {
              break;
            }
            if (bc[lit_off] != (uint8_t)b) {
              break;
            }

            if (lit_len == 1) {
              /* Single-byte literal: advance past LIT + data + 1 */
              ec_merge(eps_closures, (uint16_t)(off + 9 + 1), &next_set);
            } else {
              /* Multi-byte literal: transition to second byte */
              uint16_t second_byte = (uint16_t)(lit_off + 1);
              if (second_byte < bc_len &&
                  lit_next[second_byte] != SNOBOL_DFA_DEAD) {
                nfa_set_add(&next_set, second_byte);
              }
            }
            break;
          }

          /* ---- SPAN ---- */
          case OP_SPAN: {
            uint64_t bs[4];
            if (!get_op_bytes(bc, bc_len, off, dfa_vm, bs)) {
              break;
            }
            bool in_class = dfa_class_bit(bs, b);
            if (!in_class) {
              break;
            }
            /* On a class byte: both stay in SPAN (loop) and exit to next instruction */
            ec_merge(eps_closures, off, &next_set); /* stay in SPAN */
            ec_merge(eps_closures, (uint16_t)(off + 3),
                     &next_set); /* exit SPAN */
            break;
          }

          /* ---- ANY ---- */
          case OP_ANY: {
            uint64_t bs[4];
            if (!get_op_bytes(bc, bc_len, off, dfa_vm, bs)) {
              break;
            }
            bool in_class = dfa_class_bit(bs, b);
            if (!in_class) {
              break;
            }
            ec_merge(eps_closures, (uint16_t)(off + 3), &next_set);
            break;
          }

          /* ---- NOTANY ---- */
          case OP_NOTANY: {
            uint64_t bs[4];
            if (!get_op_bytes(bc, bc_len, off, dfa_vm, bs)) {
              break;
            }
            /* get_op_bytes for NOTANY already returns the class complement,
             * so a byte transitions when its bit IS set in bs[]. */
            bool in_class = dfa_class_bit(bs, b);
            if (!in_class) {
              break;
            }
            ec_merge(eps_closures, (uint16_t)(off + 3), &next_set);
            break;
          }

          /* ---- BREAK ---- */
          case OP_BREAK: {
            uint64_t bs[4];
            if (!get_op_bytes(bc, bc_len, off, dfa_vm, bs)) {
              break;
            }
            /* get_op_bytes for BREAK returns bytes NOT in delimiter set */
            bool is_delim = dfa_class_bit(bs, b) == false;
            if (is_delim) {
              /* Delimiter: BREAK doesn't consume it. The epsilon exit to off+3
             * (set in epsilon_closure) and the next instruction handle it. */
              break;
            }
            /* Non-delimiter: stay in BREAK loop */
            ec_merge(eps_closures, off, &next_set);
            break;
          }

          /* ---- LEN ---- */
          case OP_LEN: {
            if ((size_t)off + 5 > bc_len) {
              break;
            }
            uint32_t n = search_read_u32(bc, (size_t)off + 1);
            if (n <= 1) {
              ec_merge(eps_closures, (uint16_t)(off + 5), &next_set);
            } else {
              /* n > 1: stay at LEN (consume one byte, loop back) */
              nfa_set_add(&next_set, off);
            }
            break;
          }

          default: break;
        }
      }

      /* Commit the transition for byte b */
      if (next_set.count > 0) {
        uint16_t target =
            dfa_hash_find_or_insert(ht, DFA_HASH_CAP, &next_set, &next_dfa_id);
        if (target == SNOBOL_DFA_DEAD && next_dfa_id >= SNOBOL_DFA_MAX_STATES) {
          goto fail;
        }
        dfa->trans[((size_t)cur_dfa * 256) + b] = target;
        if (target != SNOBOL_DFA_DEAD && target >= dfa->num_states) {
          dfa->num_states = next_dfa_id;
          if (q_tail >= q_cap) {
            size_t new_qc = q_cap * 2;
            if (new_qc > 65536) {
              goto fail;
            }
            uint16_t *nq =
                (uint16_t *)snobol_realloc(queue, new_qc * sizeof(uint16_t));
            if (!nq) {
              goto fail;
            }
            queue = nq;
            q_cap = new_qc;
          }
          queue[q_tail++] = target;
        }
      } else {
        dfa->trans[((size_t)cur_dfa * 256) + b] = SNOBOL_DFA_DEAD;
      }
    }
  }

  if (dfa->num_states >= SNOBOL_DFA_MAX_STATES) {
    goto fail;
  }

  snobol_free(lit_next);
  snobol_free(eps_closures);
  snobol_free(queue);
  snobol_free(ht);

  uint16_t *nt = (uint16_t *)snobol_realloc(
      dfa->trans, (size_t)dfa->num_states * 256 * sizeof(uint16_t));
  if (!nt) {
    goto fail;
  }
  dfa->trans = nt;
  uint8_t *na =
      (uint8_t *)snobol_realloc(dfa->accepting, (dfa->num_states + 7) / 8);
  if (!na) {
    goto fail;
  }
  dfa->accepting = na;
  dfa->state_cap = dfa->num_states;

  return dfa;

fail:
  if (lit_next) {
    snobol_free(lit_next);
  }
  if (eps_closures) {
    snobol_free(eps_closures);
  }
  if (queue) {
    snobol_free(queue);
  }
  if (ht) {
    snobol_free(ht);
  }
  if (dfa) {
    if (dfa->trans) {
      snobol_free(dfa->trans);
    }
    if (dfa->accepting) {
      snobol_free(dfa->accepting);
    }
    snobol_free(dfa);
  }
  return nullptr;
}

/* ---- DFA execution engine ---- */

/**
 * Execute the automaton over the subject from `offset`, returning
 * the first match.
 *
 * The DFA walks the subject in a single linear scan:
 *   state = start_state
 *   for each position p in subject:
 *     state = trans[state][byte]
 *     if state == DEAD: restart from start_state at next position
 *     if accepting: record match
 *
 * On success, fills out_result with match start/end.
 */
static bool search_automaton_exec(const snobol_dfa_t *dfa, const char *subject,
                                  size_t subject_len, size_t offset,
                                  snobol_search_result_t *out_result) {
  if (!dfa || dfa->num_states == 0) {
    out_result->success = false;
    return false;
  }

  const uint16_t *trans = dfa->trans;
  uint16_t num_states = dfa->num_states;
  uint16_t start_state = dfa->start_state;
  const uint8_t *accepting = dfa->accepting;

  size_t pos = offset;

  while (pos <= subject_len) {
    uint16_t state = start_state;
    size_t match_start = pos;
    size_t start_pos = pos;

    /* Zero-length match: the start state itself is accepting, so the empty
     * string matches at this position before any byte is consumed.  This must
     * be checked up front — the byte-driven loop below only inspects states
     * reached after consuming input, so without this check patterns that can
     * match the empty string (e.g. '') would never report a match at interior
     * positions. */
    if (accepting[start_state / 8] & (uint8_t)(1U << (start_state % 8))) {
      out_result->success = true;
      out_result->match_start = match_start;
      out_result->match_end = match_start;
      return true;
    }

    while (pos < subject_len) {
      uint8_t byte = (uint8_t)subject[pos];
      uint16_t next = trans[((size_t)state * 256) + byte];

      if (next == SNOBOL_DFA_DEAD) {
        /* DEAD: restart at next subject position */
        pos = start_pos + 1;
        state = start_state;
        goto next_position;
      }

      state = next;

      /* Check if accepting */
      if (accepting[state / 8] & (uint8_t)(1U << (state % 8))) {
        /* Match found: position tracking.
         * If the automaton tracked the match start, we'd return it.
         * For the DFA, the match starts at match_start (the position
         * where we entered the start state) and ends at pos + 1
         * (current position after consuming this byte). */
        out_result->success = true;
        out_result->match_start = match_start;
        out_result->match_end = pos + 1;

        /* Handle empty match (accepting at start position) */
        if (pos + 1 == start_pos) {
          out_result->match_end = pos + 1;
        }

        return true;
      }

      pos++;
    }

    /* Reached end of subject without accepting */
    out_result->success = false;
    return false;

  next_position:
    continue;
  }

  out_result->success = false;
  return false;
}

/** Free a DFA allocated by build_dfa(). */
void snobol_dfa_free(snobol_dfa_t *dfa) {
  if (!dfa) {
    return;
  }
  if (dfa->trans) {
    snobol_free(dfa->trans);
  }
  if (dfa->accepting) {
    snobol_free(dfa->accepting);
  }
  snobol_free(dfa);
}

/**
 * Lazy-build or retrieve the automaton for a pattern.
 * Returns the DFA pointer if available, NULL if not.
 */
static snobol_dfa_t *get_or_build_automaton(snobol_pattern_t *pattern,
                                            const VM *dfa_vm) {
  snobol_dfa_t *cached = snobol_pattern_get_automaton(pattern);
  if (cached) {
    return cached;
  }

  const snobol_search_meta_t *meta = snobol_pattern_get_meta(pattern);
  if (!meta || !meta->automaton_eligible) {
    return nullptr;
  }

  snobol_dfa_t *dfa = build_dfa(snobol_pattern_get_bc(pattern),
                                snobol_pattern_get_bc_len(pattern), dfa_vm);
  if (dfa) {
    snobol_pattern_set_automaton(pattern, dfa);
  }
  return dfa;
}
/* ---------------------------------------------------------------------------
 * Unified search entrypoint.  Dispatches to the most specific tier available
 * using the pre-derived (or inline-derived) metadata, then falls back to the
 * general VM.
 *
 * Tier routing (highest priority first):
 *   1. BREAK/BREAKX with ASCII bitmap  -> break_accelerated
 *   2. SPAN with ASCII bitmap           -> span_accelerated
 *   3. Literal-only (no VM)             -> literal_only
 *   4. Literal prefix (>=1 byte)        -> literal_accelerated
 *   5. Single-char alt (candidate set)  -> bitmap_accelerated
 *   6. Alternation-of-literals (trie)   -> alt_literals_try
 *   7. Search-VM eligible               -> search_vm_exec
 *   8. Automaton-eligible               -> automaton path
 *   9. General VM fallback              -> vm_exec (start-byte accelerated)
 *
 * Semantics are identical to repeated anchored vm_exec calls in the caller:
 * the first non-overlapping match at or after start_offset is returned.
 *
 * Per-candidate VM reset policy (FIELDS-ONLY):
 * This function does NOT call memset(VM, 0, sizeof(VM)) per candidate. The
 * per-candidate reset is done by search_reset_vm() (above) which updates
 * only the fields that change between candidates:
 *   vm->s, vm->len, vm->ip, vm->pos, vm->var_count, vm->max_cap_used,
 *   vm->max_counter_used, vm->choices_top
 *
 * Callers that loop snobol_search_exec() (e.g. snobol_pattern_search_ex,
 * PHP Pattern::searchSplit) MUST initialise the VM struct once (setting
 * vm->bc, vm->bc_len, vm->out) and then call this function
 * repeatedly without re-memsetting the VM. snobol_pattern_search_ex()
 * in core/src/api.c is the reference implementation.
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * Tier handler function pointer type and wrappers
 *
 * All tier handlers share a uniform signature matching snobol_search_exec().
 * The tier_table[] array maps tier index to handler for single-dispatch
 * routing, replacing the 8 sequential if-branches.
 * ---------------------------------------------------------------------------
 */
typedef bool (*tier_fn)(VM *vm, const char *subject, size_t subject_len,
                        size_t start_offset, const snobol_search_meta_t *meta,
                        const snobol_dfa_t *dfa,
                        snobol_search_result_t *out_result,
                        snobol_search_diag_t *diag, bool anchored);

/* Tier 0: BREAK/BREAKX with ASCII bitmap scan */
static bool tier_break_scan(VM *vm, const char *subject, size_t subject_len,
                            size_t start_offset,
                            const snobol_search_meta_t *meta,
                            const snobol_dfa_t *dfa,
                            snobol_search_result_t *out_result,
                            snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = SNOBOL_SEARCH_SKIP_BREAK_SCAN;
  }
  return search_break_accelerated(vm, subject, subject_len, start_offset, meta,
                                  out_result, diag, anchored);
}

/* Tier 1: SPAN with ASCII bitmap scan */
static bool tier_span_scan(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = SNOBOL_SEARCH_SKIP_SPAN_SCAN;
  }
  return search_span_accelerated(vm, subject, subject_len, start_offset, meta,
                                 out_result, diag, anchored);
}

/* Tier 2: Literal-only fast path (no VM needed) */
static bool tier_literal_only(VM *vm, const char *subject, size_t subject_len,
                              size_t start_offset,
                              const snobol_search_meta_t *meta,
                              const snobol_dfa_t *dfa,
                              snobol_search_result_t *out_result,
                              snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = SNOBOL_SEARCH_SKIP_LITERAL;
  }
  return search_literal_only(vm, subject, subject_len, start_offset, meta,
                             out_result, diag, anchored);
}

/* Tier 3: Literal prefix (memmem / memchr) */
static bool tier_literal_prefix(VM *vm, const char *subject, size_t subject_len,
                                size_t start_offset,
                                const snobol_search_meta_t *meta,
                                const snobol_dfa_t *dfa,
                                snobol_search_result_t *out_result,
                                snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = (meta->literal_prefix_len == 1)
                                 ? SNOBOL_SEARCH_SKIP_FIRST_BYTE
                                 : SNOBOL_SEARCH_SKIP_LITERAL;
  }
  return search_literal_accelerated(vm, subject, subject_len, start_offset,
                                    meta, out_result, diag, anchored);
}

/* Tier 4: Candidate-set bitmap for single-char alternation */
static bool tier_bitmap(VM *vm, const char *subject, size_t subject_len,
                        size_t start_offset, const snobol_search_meta_t *meta,
                        const snobol_dfa_t *dfa,
                        snobol_search_result_t *out_result,
                        snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = SNOBOL_SEARCH_SKIP_BITMAP;
  }
  return search_bitmap_accelerated(vm, subject, subject_len, start_offset, meta,
                                   out_result, diag, anchored);
}

/* Tier 5: Alternation-of-literals (trie-based) */
static bool tier_alt_literals(VM *vm, const char *subject, size_t subject_len,
                              size_t start_offset,
                              const snobol_search_meta_t *meta,
                              const snobol_dfa_t *dfa,
                              snobol_search_result_t *out_result,
                              snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  if (diag) {
    diag->last_skip_reason = SNOBOL_SEARCH_SKIP_NONE;
  }
  return search_alt_literals_try(vm, subject, subject_len, start_offset, meta,
                                 dfa, diag, out_result, anchored);
}

/* Tier 6: Search-VM for eligible patterns */

/* ---------------------------------------------------------------------------
 * Pike single-pass scan (W1c)
 *
 * A single left-to-right pass over the subject that maintains a set of live
 * NFA threads.  On OP_SPLIT the driver spawns a new thread for branch B
 * (greedy: first branch is tried first) instead of pushing a choice point.
 * On OP_FAIL a thread dies instead of popping a choice.  This eliminates the
 * O(n) restart overhead of the per-offset loop and yields O(n) matching for
 * patterns without deep backtracking.
 *
 * Gated behind SNOBOL_PIKE_SCAN; falls back to the restart loop when the
 * thread buffer overflows or the feature is disabled.
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * Pike single-pass scan (W1c)
 *
 * A single left-to-right pass over the subject that maintains a set of live
 * NFA threads.  On OP_SPLIT the driver spawns a new thread for branch B
 * (greedy: first branch is tried first) instead of pushing a choice point.
 * On OP_FAIL a thread dies instead of popping a choice.  This eliminates the
 * O(n) restart overhead of the per-offset loop and yields O(n) matching for
 * patterns without deep backtracking.
 *
 * The key challenge is consuming opcodes (SPAN, BREAK, BREAKX) that advance
 * threads past the current position.  These threads are deferred into a queue
 * and merged into the buffer when the driver reaches their position.
 * ---------------------------------------------------------------------------
 */
/* ---------------------------------------------------------------------------
 * Pike single-pass scan (W1c)
 * ---------------------------------------------------------------------------
 */
#ifdef SNOBOL_PIKE_SCAN
#define PIKE_THREAD_BUF 64
#define PIKE_DEFER_BUF 64
typedef struct {
  size_t ip;
  size_t pos;
  size_t match_start;
  size_t cap_start[MAX_CAPS];
  size_t cap_end[MAX_CAPS];
  uint8_t max_cap_used;
  size_t var_start[MAX_VARS];
  size_t var_end[MAX_VARS];
  size_t var_count;
  uint32_t counters[MAX_LOOPS];
  size_t loop_last_pos[MAX_LOOPS];
  uint8_t max_counter_used;
} pike_thread_t;

bool pike_scan(const uint8_t *bc, size_t bc_len, const char *subject,
               size_t subject_len, const snobol_search_meta_t *meta,
               const snobol_range_meta_t *range_meta, size_t range_meta_count,
               VM *vm, snobol_search_result_t *out_result) {
  (void)meta;
  /* Use heap-allocated buffers when a VM is provided (reuse across calls);
   * fall back to stack allocation for stateless/direct pike_scan callers. */
  pike_thread_t stack_threads[PIKE_THREAD_BUF];
  pike_thread_t stack_defer[PIKE_DEFER_BUF];
  pike_thread_t *threads = stack_threads;
  pike_thread_t *defer = stack_defer;
  if (vm) {
    if (!vm->pike_thread_buf) {
      vm->pike_thread_buf =
          snobol_malloc(PIKE_THREAD_BUF * sizeof(pike_thread_t));
    }
    if (!vm->pike_defer_buf) {
      vm->pike_defer_buf =
          snobol_malloc(PIKE_DEFER_BUF * sizeof(pike_thread_t));
    }
    threads = (pike_thread_t *)vm->pike_thread_buf;
    defer = (pike_thread_t *)vm->pike_defer_buf;
  }
  size_t thread_n = 0;
  size_t defer_n = 0;
  bool found = false;
  bool overflowed = false;
  size_t m_start = 0;
  size_t m_end = 0;
  if (out_result) {
    out_result->pike_zero_progress = false;
  }

  /* Don't seed upfront — spawn dynamically as we scan */
  thread_n = 0;

  for (size_t pos = 0; pos <= subject_len; ++pos) {
    defer_n = 0;

    /* Partition the incoming thread set: threads whose scan position equals
     * `pos` are worked on this pass; threads still in the future are carried
     * forward unchanged.  (Threads cannot have pos < pos.) */
    pike_thread_t carry[PIKE_THREAD_BUF];
    size_t carry_n = 0;
    pike_thread_t work[PIKE_THREAD_BUF];
    size_t work_n = 0;
    for (size_t t = 0; t < thread_n; ++t) {
      if (threads[t].pos == pos) {
        if (work_n < PIKE_THREAD_BUF) {
          work[work_n++] = threads[t];
        } else {
          overflowed = true;
        }
      } else if (threads[t].pos > pos) {
        if (carry_n < PIKE_THREAD_BUF) {
          carry[carry_n++] = threads[t];
        } else {
          overflowed = true;
        }
      }
    }
    /* Spawn a fresh thread at this start position. */
    if (work_n < PIKE_THREAD_BUF) {
      pike_thread_t fr;
      memset(&fr, 0, sizeof(fr));
      fr.pos = pos;
      fr.match_start = pos;
      work[work_n++] = fr;
    }

    for (size_t wt = 0; wt < work_n; ++wt) {
      pike_thread_t th = work[wt];
    pike_retry_thread:
      size_t ip = th.ip;
      size_t tp = pos;

      /* zero-width / control ops inline — all of these resume the SAME thread
       * at the next instruction, so their `continue` re-enters this loop. */
      while (ip < bc_len) {
        uint8_t op = bc[ip];
        if (op == OP_NOP || op == OP_FENCE) {
          ip++;
          continue;
        }
        if (op == OP_ANCHOR) {
          uint8_t at = bc[ip + 1];
          /* Window-relative semantics like the full VM: the start anchor
           * succeeds at the thread's match start (not just subject byte 0),
           * the end anchor at the subject end. */
          if ((at == 0 && tp != th.match_start) ||
              (at == 1 && tp != subject_len)) {
            goto pike_die;
          }
          ip += 2;
          continue;
        }
        if (op == OP_POS || op == OP_RPOS || op == OP_TAB || op == OP_RTAB) {
          /* Position constraints must be validated, mirroring the full VM:
           * POS/RPOS fail unless the cursor is exactly at the codepoint
           * target; TAB fails if the target is beyond the subject or the
           * cursor is past it (then moves the cursor); RTAB fails if the
           * cursor is past the target (then moves the cursor). */
          uint32_t n = ((uint32_t)bc[ip + 1] << 24) |
                       ((uint32_t)bc[ip + 2] << 16) |
                       ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
          size_t target;
          if (op == OP_POS || op == OP_TAB) {
            target = 0;
            for (uint32_t i = 0; i < n && target < subject_len; i++) {
              uint32_t cp;
              int by = 1;
              if (!utf8_peek_next(subject, subject_len, target, &cp, &by)) {
                break;
              }
              target += (size_t)by;
            }
          } else {
            target = subject_len;
            for (uint32_t i = 0; i < n && target > 0; i++) {
              target--;
              while (target > 0 &&
                     ((unsigned char)subject[target] & 0xC0) == 0x80) {
                target--;
              }
            }
          }
          if (op == OP_POS || op == OP_RPOS) {
            if (tp != target) {
              goto pike_die;
            }
          } else if (op == OP_TAB) {
            if ((target >= subject_len && n > 0) || tp > target) {
              goto pike_die;
            }
            tp = target;
          } else { /* OP_RTAB */
            if (tp > target) {
              goto pike_die;
            }
            tp = target;
          }
          ip += 5;
          continue;
        }
        if (op == OP_CAP_START) {
          uint8_t r = bc[ip + 1];
          if (r < MAX_CAPS) {
            th.cap_start[r] = tp;
            if (r >= th.max_cap_used) {
              th.max_cap_used = r + 1;
            }
          }
          ip += 2;
          continue;
        }
        if (op == OP_CAP_END) {
          uint8_t r = bc[ip + 1];
          if (r < MAX_CAPS) {
            th.cap_end[r] = tp;
            if (r >= th.max_cap_used) {
              th.max_cap_used = r + 1;
            }
            if (r < MAX_VARS) {
              th.var_start[r] = th.cap_start[r];
              th.var_end[r] = th.cap_end[r];
              if ((size_t)r + 1 > th.var_count) {
                th.var_count = (size_t)r + 1;
              }
            }
          }
          ip += 2;
          continue;
        }
        if (op == OP_ASSIGN) {
          uint16_t v = (uint16_t)((bc[ip + 1] << 8) | bc[ip + 2]);
          uint8_t r = bc[ip + 3];
          if (v < MAX_VARS && r < MAX_CAPS) {
            if (v >= th.var_count) {
              th.var_count = (size_t)v + 1;
            }
            th.var_start[v] = th.cap_start[r];
            th.var_end[v] = th.cap_end[r];
          }
          ip += 4;
          continue;
        }
        if (op == OP_JMP) {
          uint32_t tgt = ((uint32_t)bc[ip + 1] << 24) |
                         ((uint32_t)bc[ip + 2] << 16) |
                         ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
          ip = (size_t)tgt;
          continue;
        }
        if (op == OP_LEN) {
          uint32_t n = ((uint32_t)bc[ip + 1] << 24) |
                       ((uint32_t)bc[ip + 2] << 16) |
                       ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
          ip += 5;
          for (uint32_t i = 0; i < n; i++) {
            if (tp >= subject_len) {
              goto pike_die;
            }
            uint32_t cp;
            int by;
            if (!utf8_peek_next(subject, subject_len, tp, &cp, &by)) {
              goto pike_die;
            }
            tp += by;
          }
          continue;
        }
        if (op == OP_SPLIT) {
          uint32_t aa = ((uint32_t)bc[ip + 1] << 24) |
                        ((uint32_t)bc[ip + 2] << 16) |
                        ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
          uint32_t bb = ((uint32_t)bc[ip + 5] << 24) |
                        ((uint32_t)bc[ip + 6] << 16) |
                        ((uint32_t)bc[ip + 7] << 8) | (uint32_t)bc[ip + 8];
          ip = (size_t)aa;
          /* Branch B runs at the same scan position (SPLIT is zero-width);
           * append it to the work queue so it is tried after branch A. */
          if (work_n < PIKE_THREAD_BUF) {
            pike_thread_t nt = th;
            nt.ip = (size_t)bb;
            nt.pos = tp;
            work[work_n++] = nt;
          } else {
            overflowed = true;
          }
          continue;
        }
        if (op == OP_REPEAT_INIT) {
          overflowed = true; /* pike_scan cannot backtrack REPEAT_STEP; signal
                              * fallback so tier_search_vm tries the restart
                              * loop which has a proper choice stack. */
          uint8_t lid = bc[ip + 1];
          if (lid < MAX_LOOPS) {
            th.counters[lid] = 0;
            th.loop_last_pos[lid] = 0;
          }
          ip += 14;
          continue;
        }
        if (op == OP_REPEAT_STEP) {
          uint8_t lid = bc[ip + 1];
          uint32_t tgt = ((uint32_t)bc[ip + 2] << 24) |
                         ((uint32_t)bc[ip + 3] << 16) |
                         ((uint32_t)bc[ip + 4] << 8) | (uint32_t)bc[ip + 5];
          if (lid < MAX_LOOPS) {
            th.counters[lid]++;
            /* Zero-progress guard (mirrors the full VM): an iteration that
             * consumed nothing cannot repeat — exit the loop.  Without this,
             * empty-body loops like `('')*` spin forever.
             * A zero-progress exit also makes pike's match POSITION
             * unreliable (the min==0 skip path can match empty at an
             * earlier position), so signal overflow to force the restart
             * loop — the same fallback as REPEAT_INIT. */
            if (th.pos == th.loop_last_pos[lid]) {
              overflowed = true;
              out_result->pike_zero_progress = true;
              ip += 6;
            } else {
              th.loop_last_pos[lid] = tp;
              ip = (size_t)tgt;
            }
          } else {
            ip += 6;
          }
          continue;
        }
        break;
      }
      if (ip >= bc_len) {
        goto pike_die;
      }
      uint8_t op = bc[ip++];

      if (op == OP_ACCEPT || op == OP_SUCCEED) {
        if (!found || th.match_start < m_start ||
            (th.match_start == m_start && tp > m_end)) {
          found = true;
          m_start = th.match_start;
          m_end = tp;
          /* Write captures back to the VM so unanchored capture search stays
           * consistent with the full VM (mirrors search_vm_writeback_to_vm).
           * The named-variable registers are included: CAP_END/ASSIGN populate
           * the thread's var registers, and they must reach the caller.
           * Thread positions are absolute in the subject; store them
           * relative to the match start like the restart loop and full VM do
           * (the match-window convention used by every capture reader). */
          if (vm) {
            size_t base = th.match_start;
            for (size_t ci = 0; ci < MAX_CAPS; ci++) {
              if (ci < th.max_cap_used) {
                vm->cap_start[ci] = th.cap_start[ci] - base;
                vm->cap_end[ci] = th.cap_end[ci] - base;
              }
            }
            vm->max_cap_used = th.max_cap_used;
            for (size_t vi = 0; vi < MAX_VARS; vi++) {
              if (vi < th.var_count) {
                vm->var_start[vi] = th.var_start[vi] - base;
                vm->var_end[vi] = th.var_end[vi] - base;
              }
            }
            vm->var_count = th.var_count;
          }
        }
        goto pike_die;
      }
      if (op == OP_FAIL || op == OP_ABORT) {
        goto pike_die;
      }

      /* NOTE: no blanket `tp >= subject_len` guard here — zero-length ops
       * (LIT len 0, BREAK at the end) are legal at the subject end; each
       * consuming op applies its own bounds. */
      if (op == OP_LIT) {
        uint32_t off = ((uint32_t)bc[ip] << 24) | ((uint32_t)bc[ip + 1] << 16) |
                       ((uint32_t)bc[ip + 2] << 8) | (uint32_t)bc[ip + 3];
        uint32_t alen = ((uint32_t)bc[ip + 4] << 24) |
                        ((uint32_t)bc[ip + 5] << 16) |
                        ((uint32_t)bc[ip + 6] << 8) | (uint32_t)bc[ip + 7];
        ip += 8;
        if (off == ip) {
          ip += alen; /* literal data stored inline; skip past it */
        }
        if (tp + alen > subject_len ||
            memcmp(subject + tp, bc + off, alen) != 0) {
          goto pike_die;
        }
        tp += alen;
        th.ip = ip;
        th.pos = tp;
        if (tp > pos) {
          if (defer_n < PIKE_DEFER_BUF) {
            defer[defer_n++] = th;
          } else {
            overflowed = true;
          }
        } else {
          /* Zero progress (empty literal): resume the same thread at this
           * position instead of dropping it. */
          work[wt] = th;
          goto pike_retry_thread;
        }
        goto pike_next;
      }
      if (op == OP_ANY) {
        uint16_t sid = (uint16_t)((bc[ip] << 8) | bc[ip + 1]);
        ip += 2;
        uint16_t cnt = 0;
        const uint8_t *rp = (sid > 0 && (size_t)(sid - 1) < range_meta_count)
                                ? range_meta[sid - 1].ranges_ptr
                                : nullptr;
        cnt = rp ? range_meta[sid - 1].count : 0;
        uint32_t cp;
        int by;
        if (!utf8_peek_next(subject, subject_len, tp, &cp, &by)) {
          goto pike_die;
        }
        if (!rp || !range_contains(rp, cnt, cp)) {
          goto pike_die;
        }
        tp += by;
        th.ip = ip;
        th.pos = tp;
        if (tp > pos) {
          if (defer_n < PIKE_DEFER_BUF) {
            defer[defer_n++] = th;
          } else {
            overflowed = true;
          }
        }
        goto pike_next;
      }
      if (op == OP_NOTANY) {
        uint16_t sid = (uint16_t)((bc[ip] << 8) | bc[ip + 1]);
        ip += 2;
        if (tp >= subject_len) {
          goto pike_die; /* NOTANY needs a byte; the full VM fails at the end */
        }
        uint16_t cnt = 0;
        const uint8_t *rp = (sid > 0 && (size_t)(sid - 1) < range_meta_count)
                                ? range_meta[sid - 1].ranges_ptr
                                : nullptr;
        cnt = rp ? range_meta[sid - 1].count : 0;
        if (rp) {
          uint32_t cp;
          int by;
          if (utf8_peek_next(subject, subject_len, tp, &cp, &by) &&
              range_contains(rp, cnt, cp)) {
            goto pike_die;
          }
          tp += by;
        } else {
          tp++;
        }
        th.ip = ip;
        th.pos = tp;
        /* NOTANY always consumes >= 1 byte: only the defer path is
         * reachable (no zero-progress re-queue needed). */
        if (defer_n < PIKE_DEFER_BUF) {
          defer[defer_n++] = th;
        } else {
          overflowed = true;
        }
        goto pike_next;
      }
      if (op == OP_SPAN) {
        uint16_t sid = (uint16_t)((bc[ip] << 8) | bc[ip + 1]);
        ip += 2;
        uint16_t cnt = 0;
        const uint8_t *rp = (sid > 0 && (size_t)(sid - 1) < range_meta_count)
                                ? range_meta[sid - 1].ranges_ptr
                                : nullptr;
        cnt = rp ? range_meta[sid - 1].count : 0;
        /* All-ASCII classes are matched BYTE-wise, mirroring the full VM:
         * any byte > 127 stops the run (its UTF-8 codepoint must not be
         * considered, as an invalid sequence could decode into a class
         * byte).  Non-ASCII classes use codepoint-wise membership. */
        uint64_t amap[2];
        bool ascii_class = (rp && ranges_to_ascii_bitmap(rp, cnt, amap)) != 0;
        size_t sp = tp;
        if (rp) {
          while (sp < subject_len) {
            if (ascii_class) {
              uint8_t c = (uint8_t)subject[sp];
              if (c > 127 || !bitmap_test(amap, c)) {
                break;
              }
              sp++;
              continue;
            }
            uint32_t cp;
            int by;
            if (!utf8_peek_next(subject, subject_len, sp, &cp, &by)) {
              break;
            }
            if (!range_contains(rp, cnt, cp)) {
              break;
            }
            sp += by;
          }
        } else {
          sp = subject_len;
        } /* NULL ranges: match any (full VM compat) */
        if (sp == tp) {
          goto pike_die;
        }
        th.ip = ip;
        th.pos = sp;
        if (defer_n < PIKE_DEFER_BUF) {
          defer[defer_n++] = th;
        } else {
          overflowed = true;
        }
        goto pike_next;
      }
      if (op == OP_BREAK || op == OP_BREAKX) {
        uint16_t sid = (uint16_t)((bc[ip] << 8) | bc[ip + 1]);
        ip += 2;
        uint16_t cnt = 0;
        const uint8_t *rp = (sid > 0 && (size_t)(sid - 1) < range_meta_count)
                                ? range_meta[sid - 1].ranges_ptr
                                : nullptr;
        cnt = rp ? range_meta[sid - 1].count : 0;
        size_t sp = tp;
        if (rp) {
          while (sp < subject_len) {
            uint32_t cp;
            int by;
            if (!utf8_peek_next(subject, subject_len, sp, &cp, &by)) {
              break;
            }
            if (range_contains(rp, cnt, cp)) {
              break;
            }
            sp += by;
          }
        } else {
          sp = subject_len;
        }
        if (op == OP_BREAKX) {
          uint32_t bx_cp;
          int bx_by = 1;
          utf8_peek_next(subject, subject_len, sp, &bx_cp, &bx_by);
          if (defer_n < PIKE_DEFER_BUF) {
            pike_thread_t rt = th;
            rt.ip = ip - 3; /* re-execute the OP_BREAKX opcode (opcode sits at
                             * ip-3 after the u16 operand read) */
            rt.pos = sp + (size_t)bx_by;
            defer[defer_n++] = rt;
          } else {
            overflowed = true;
          }
        }
        th.ip = ip;
        th.pos = sp;
        if (sp > pos) {
          if (defer_n < PIKE_DEFER_BUF) {
            defer[defer_n++] = th;
          } else {
            overflowed = true;
          }
        } else {
          /* Zero progress (delimiter at the current position, or at the
           * subject end): BREAK consumes 0 bytes and must continue at the
           * same position. */
          work[wt] = th;
          goto pike_retry_thread;
        }
        goto pike_next;
      }
      goto pike_die;
    pike_die:
      /* Thread failed/ended: discard it. */
      continue;
    pike_next:
        /* Thread advanced and was stored in defer[] above. */
        ;
    }

    /* Rebuild the thread set: carried threads (future positions) first, then
     * threads that advanced past `pos` during this pass. */
    thread_n = 0;
    for (size_t c = 0; c < carry_n; ++c) {
      if (thread_n < PIKE_THREAD_BUF) {
        threads[thread_n++] = carry[c];
      } else {
        overflowed = true;
      }
    }
    for (size_t d = 0; d < defer_n; ++d) {
      if (thread_n < PIKE_THREAD_BUF) {
        threads[thread_n++] = defer[d];
      } else {
        overflowed = true;
      }
    }
  }
  out_result->pike_overflowed = overflowed;
  out_result->success = found;
  if (found) {
    out_result->match_start = m_start;
    out_result->match_end = m_end;
  }
  return found;
}
#endif /* SNOBOL_PIKE_SCAN */
/* Tier 6: Search-VM for eligible patterns */
static bool tier_search_vm(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
#ifdef SNOBOL_PIKE_SCAN
  /* pike_scan does not support anchored mode — it scans the full subject.
   * When anchored, fall through to the restart loop which respects the
   * anchor constraint (single position). */
  if (start_offset == 0 && !anchored) {
    if (diag) {
      diag->search_vm_tests++;
    }
    bool pike_ok =
        pike_scan(vm->bc, vm->bc_len, subject, subject_len, meta,
                  vm->range_meta, vm->range_meta_count, vm, out_result);
    if (!out_result->pike_zero_progress &&
        (pike_ok || !out_result->pike_overflowed)) {
      return pike_ok;
    }
    /* Overflow (thread buffer, REPEAT handling) or a zero-progress loop
     * exit: pike's greedy result is unreliable — fall through to the
     * restart loop which tries each position with a proper choice stack.
     * Set the result flag so callers can detect overflow. */
    out_result->pike_overflowed = true;
    if (diag) {
      diag->pike_overflow = true;
    }
  }
#endif
  size_t offset = start_offset;
  out_result->aborted = false;
  search_vm_t svm;
  while (offset <= subject_len) {
    if (diag) {
      diag->candidates_tested++;
      diag->search_vm_tests++;
    }
    search_reset_vm(vm, subject, subject_len, offset);
    search_vm_init_from_vm(&svm, vm);
    svm.choices_top = 0;
    out_result->aborted = false;
    bool ok = search_vm_exec(&svm, subject, subject_len, offset, out_result);
    search_vm_writeback_to_vm(&svm, vm);
    if (ok) {
      /* search_vm_exec lazily allocated the choice stack into svm.choices
       * (and wrote the pointer back to vm->choices).  Own it here and free
       * it so direct snobol_search_exec callers don't leak it. */
      if (svm.choices) {
        snobol_free(svm.choices);
      }
      svm.choices = NULL;
      vm->choices = NULL;
      return true;
    }
    if (out_result->aborted) {
      if (svm.choices) {
        snobol_free(svm.choices);
      }
      svm.choices = NULL;
      vm->choices = NULL;
      out_result->success = false;
      return false;
    }
    if (offset >= subject_len) {
      break;
    }
    if (anchored) {
      break;
    }
    if (meta->has_bmh_skip && offset + meta->bmh_skip_len <= subject_len) {
      size_t adv = meta->bmh_skip[(
          unsigned char)subject[offset + meta->bmh_skip_len - 1]];
      offset += adv > 0 ? adv : 1;
    } else {
      offset++;
    }
  }
  if (svm.choices) {
    snobol_free(svm.choices);
  }
  svm.choices = NULL;
  vm->choices = NULL;
  out_result->success = false;
  return false;
}

/* Tier 8: General VM fallback with start-byte bitmap acceleration */
bool tier_general_fallback(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored) {
  (void)dfa;
  size_t offset = start_offset;
  out_result->aborted = false;

  while (offset <= subject_len) {
    /* Start-byte bitmap filter: skip subject bytes that can never start
     * the pattern.  On average this eliminates 255/256 ≈ 99.6% of
     * candidate positions for constrained patterns. */
    if (meta->has_start_bitmap && offset < subject_len) {
      uint8_t b = (uint8_t)subject[offset];
      if (likely(!bitmap256_test(meta->start_bitmap, b))) {
        if (diag) {
          diag->candidates_skipped++;
        }
        offset++;
        continue;
      }
    }

    /* Minimum-length filter: if too few remaining bytes, give up */
    if (meta->minlength > 0 && offset + meta->minlength > subject_len) {
      out_result->success = false;
      return false;
    }

    if (diag) {
      diag->candidates_tested++;
    }
    search_reset_vm(vm, subject, subject_len, offset);
    bool ok = vm_exec(vm);
    if (ok) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = offset + vm->pos;
      return true;
    }
    if (vm->abort_flag) {
      out_result->aborted = true;
      out_result->success = false;
      return false;
    }
    if (offset >= subject_len) {
      break;
    }
    if (anchored) {
      break;
    }
    if (meta->has_bmh_skip && offset + meta->bmh_skip_len <= subject_len) {
      size_t adv = meta->bmh_skip[(
          unsigned char)subject[offset + meta->bmh_skip_len - 1]];
      offset += adv > 0 ? adv : 1;
    } else {
      offset++;
    }
  }

  out_result->success = false;
  return false;
}

/* Anchored automaton pass: walk the DFA once from `offset` with no
 * DEAD-restart and no BMH shift.  Success only when the accepting run
 * begins exactly at the anchor.  Mirrors search_automaton_exec minus the
 * per-position restart loop. */
static bool search_automaton_exec_anchored(const snobol_dfa_t *dfa,
                                           const char *subject,
                                           size_t subject_len, size_t offset,
                                           snobol_search_result_t *out_result) {
  if (!dfa || dfa->num_states == 0) {
    out_result->success = false;
    return false;
  }

  const uint16_t *trans = dfa->trans;
  uint16_t start_state = dfa->start_state;
  const uint8_t *accepting = dfa->accepting;

  /* Zero-length match at the anchor: the start state itself is accepting. */
  if (accepting[start_state / 8] & (uint8_t)(1U << (start_state % 8))) {
    out_result->success = true;
    out_result->match_start = offset;
    out_result->match_end = offset;
    return true;
  }

  uint16_t state = start_state;
  size_t pos = offset;
  while (pos < subject_len) {
    uint8_t byte = (uint8_t)subject[pos];
    uint16_t next = trans[((size_t)state * 256) + byte];
    if (next == SNOBOL_DFA_DEAD) {
      out_result->success = false;
      return false;
    }
    state = next;
    pos++;
    if (accepting[state / 8] & (uint8_t)(1U << (state % 8))) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = pos;
      return true;
    }
  }

  out_result->success = false;
  return false;
}

/* Tier 7: Automaton path for eligible patterns */
static bool tier_automaton(VM *vm, const char *subject, size_t subject_len,
                           size_t start_offset,
                           const snobol_search_meta_t *meta,
                           const snobol_dfa_t *dfa,
                           snobol_search_result_t *out_result,
                           snobol_search_diag_t *diag, bool anchored) {
  snobol_dfa_t *owned = nullptr;
  if (!dfa) {
    owned = build_dfa(vm->bc, vm->bc_len, vm);
    dfa = owned;
  }
  if (dfa) {
    size_t offset = start_offset;
    while (offset <= subject_len) {
      if (diag) {
        diag->candidates_tested++;
        diag->automaton_tests++;
      }
      bool ok =
          search_automaton_exec(dfa, subject, subject_len, offset, out_result);
      if (ok) {
        if (owned) {
          snobol_dfa_free(owned);
        }
        return true;
      }
      if (offset >= subject_len) {
        break;
      }
      if (anchored) {
        break;
      }
      if (meta->has_bmh_skip && offset + meta->bmh_skip_len <= subject_len) {
        size_t adv = meta->bmh_skip[(
            unsigned char)subject[offset + meta->bmh_skip_len - 1]];
        offset += adv > 0 ? adv : 1;
      } else {
        offset++;
      }
    }
  }
  if (owned) {
    snobol_dfa_free(owned);
  }
  /* Fall through to general VM */
  return tier_general_fallback(vm, subject, subject_len, start_offset, meta,
                               nullptr, out_result, diag, anchored);
}

/**
 * Tier dispatch table: index by meta->tier for single-dispatch routing.
 * Replaces the 8 sequential if-branches in the old snobol_search_exec().
 *
 * Each handler has uniform signature: tier_fn(VM*, subject, len, offset,
 * meta, dfa, result, diag). Handlers for Tiers 1-5 bypass the VM entirely;
 * Tier 6 uses the lightweight search_vm_t; Tier 7 uses the DFA automaton;
 * Tier 8 uses the full SNOBOL4 VM.
 */
static const tier_fn tier_table[TIER_COUNT] = {
    [TIER_BREAK_SCAN] = tier_break_scan, /* BREAK/BREAKX ASCII bitmap scan */
    [TIER_SPAN_SCAN] = tier_span_scan,   /* SPAN ASCII bitmap scan */
    [TIER_LITERAL] = tier_literal_only,  /* Literal-only (no VM) */
    [TIER_PREFIX] = tier_literal_prefix, /* Literal prefix (memmem/memchr) */
    [TIER_BITMAP] = tier_bitmap,        /* Candidate bitmap (single-char alt) */
    [TIER_ALT_LIT] = tier_alt_literals, /* Alt-of-literals (trie) */
    [TIER_SEARCH_VM] = tier_search_vm,  /* Search-VM (backtracking NFA) */
    [TIER_AUTOMATON] = tier_automaton,  /* DFA automaton (O(n) scan) */
    [TIER_GENERAL] = tier_general_fallback, /* General VM fallback */
    [TIER_SIMD_NFA] = tier_simd_nfa,        /* SIMD-accelerated Thompson NFA */
    [TIER_FUSED_AUTOMATON] = tier_fusion,   /* Fused concat-pattern (no VM) */
};

SNOBOL_ALIGNED(64)
static bool SNOBOL_HOT dispatch_search_impl(
    VM *SNOBOL_RESTRICT vm, const char *SNOBOL_RESTRICT subject,
    size_t subject_len, size_t start_offset, const snobol_search_meta_t *meta,
    const snobol_dfa_t *dfa, snobol_search_result_t *out_result,
    snobol_search_diag_t *diag, bool anchored) {
  if (diag) {
    memset(diag, 0, sizeof(*diag));
  }
  if (out_result) {
    out_result->success = false;
    out_result->pike_overflowed = false;
    out_result->prefilter_skip = false;
  }
  if (!vm || !subject || !out_result) {
    return false;
  }
  out_result->aborted = false;
  if (start_offset > subject_len) {
    return false;
  }

  /* Derive metadata inline if not provided */
  snobol_search_meta_t local_meta;
  bool meta_derived_inline = false;
  if (!meta) {
    snobol_search_derive_meta(vm->bc, vm->bc_len, &local_meta);
    meta = &local_meta;
    meta_derived_inline = true;
  }

  /* Cost-model tier selection (Priority 4): refine the structural tier from
   * derive_meta using subject length and per-tier cost. Break/SPAN keep their
   * fixed tier; the automaton is handled by the DFA override below. When
   * anchored, scanning tiers (BREAK/SPAN/PREFIX) are excluded by
   * select_tier_by_cost. */
  snobol_search_tier_t dispatch_tier =
      select_tier_by_cost(meta, subject_len, dfa != NULL, anchored);

  /* If caller provided a DFA, consider automaton acceleration.
    * The automaton's per-offset trial loop is O(n) per position — harmless
    * when BMH skip is available (advances several bytes per try), but O(n^2)
    * when every byte is tried individually for patterns with NO literal
    * structure (pure SPAN/BREAK on failing subjects).
    *
    * Enable the automaton when:
    *  - The pattern has BMH skip (literal prefix ≥ 2 bytes makes the
    *    per-position trial O(n) overall instead of O(n²)), or
    *  - The pattern is NOT a pure SPAN/BREAK (simd_eligible=false) AND
    *    would need the full VM anyway (tier ≥ TIER_SEARCH_VM).
    *
    * Pure SPAN/BREAK patterns (simd_eligible=true, any tier) skip the
    * automaton: the O(n) bitmap or SIMD NFA scan is faster and avoids
    * the O(n²) per-position automaton trial loop. */
  /* P5 note: has_bmh_skip is now also set for bushy alternation-of-literals
    * (shared prefix).  Those must stay on the trie path (TIER_ALT_LIT), so the
    * automaton reroute excludes all alt-literals; only flat alternations
    * (which have no shared prefix and thus no BMH skip) can reach the DFA.
    * Anchored calls skip the override: search_automaton_exec is an unanchored
    * restart scanner by design, so anchored matching takes the single-pass
    * automaton walk below instead. */
  if (dfa && meta->automaton_eligible && !meta->is_alt_literals &&
      meta->has_bmh_skip && !anchored) {
    dispatch_tier = TIER_AUTOMATON;
  }

  /* Anchored automaton pass: the DFA is a linear walk, so an anchored call
   * can still use it as a single pass from the anchor — no DEAD-restart, no
   * BMH shift, success only when the accepting run begins at start_offset.
   * On failure (e.g. the DFA cannot express the pattern, as with captures)
   * fall through to the tier dispatch below, which bounds its attempt to
   * start_offset when anchored — mirroring tier_automaton's fallback. */
  if (anchored && dfa && meta->automaton_eligible && !meta->is_alt_literals &&
      meta->has_bmh_skip) {
    bool anchored_ok = search_automaton_exec_anchored(dfa, subject, subject_len,
                                                      start_offset, out_result);
    if (anchored_ok) {
      if (meta_derived_inline) {
        snobol_search_meta_free(&local_meta);
      }
      return true;
    }
    out_result->success = false;
  }

  /* Tier 9 (SIMD NFA) is now selected by select_tier_by_cost for every
    * simd_eligible charclass pattern (SPAN/BREAK/ANY/NOTANY with no side
    * effects).  tier_simd_nfa performs an O(n) bitmap-skip scan over the
    * subject with O(1) scalar NFA verification at candidate positions (see
    * search_simd.c).  No additional override is needed here — the cost
    * model handles dispatch directly. */

  /* Lazy VM initialization: zero only the fields needed by the selected tier.
   * Tiers 0-5 bypass the VM entirely. Tiers 6-7 use the lightweight
   * search_vm_t and need minimal setup. Tier 8+ uses the full VM and relies on
   * caller-provided bc/bc_len. Initializing from dispatch_tier (not the
   * structural meta->tier) is essential: the cost model may pick a VM tier
   * whose structural tier differs. */
  if (dispatch_tier <= TIER_ALT_LIT) {
    /* Tiers 0-5: no VM fields needed */
  } else if (dispatch_tier <= TIER_AUTOMATON) {
    /* Tiers 6-7: minimal search_vm_t fields */
    vm->ip = 0;
    vm->pos = 0;
    vm->var_count = 0;
    vm->max_cap_used = 0;
    vm->max_counter_used = 0;
    vm->choices_top = 0;
  }

  /* Direct tier dispatch for the chosen tier.  For anchored matching each
   * tier handler bounds its attempt to a single position (start_offset), so a
   * successful match always begins at the anchor — no post-filter needed. */
  bool dispatched =
      tier_table[dispatch_tier](vm, subject, subject_len, start_offset, meta,
                                dfa, out_result, diag, anchored);
  if (meta_derived_inline) {
    snobol_search_meta_free(&local_meta);
  }
  return dispatched;
}

/* Required-byte pre-filter: before entering any tier or DFA setup, check
 * whether a literal that MUST appear in the subject is present.  When the
 * literal is absent, mark out_result with prefilter_skip and return true —
 * the caller returns false immediately, with no VM/tier/DFA invocation.
 * Hoisted out of dispatch_search_impl so callers like snobol_pattern_search
 * can skip DFA building when the prefilter rejects, and shared by the
 * anchored entry point so anchored matches get the same fail-fast. */
static bool SNOBOL_HOT
search_prefilter_miss(const snobol_search_meta_t *meta,
                      const char *SNOBOL_RESTRICT subject, size_t subject_len,
                      size_t start_offset, snobol_search_result_t *out_result) {
  if (meta && meta->has_required_lit && meta->required_lit_len > 0) {
    bool absent;
    if (meta->required_lit_len == 1) {
      absent = ((!memchr(subject + start_offset, meta->required_lit[0],
                         subject_len - start_offset)) != 0);
    } else {
      absent = ((!memmem(subject + start_offset, subject_len - start_offset,
                         meta->required_lit, meta->required_lit_len)) != 0);
    }
    if (absent) {
      out_result->success = false;
      out_result->prefilter_skip = true;
      return true;
    }
  }
  return false;
}

SNOBOL_ALIGNED(64)
bool SNOBOL_HOT snobol_search_exec(VM *SNOBOL_RESTRICT vm,
                                   const char *SNOBOL_RESTRICT subject,
                                   size_t subject_len, size_t start_offset,
                                   const snobol_search_meta_t *meta,
                                   const snobol_dfa_t *dfa,
                                   snobol_search_result_t *out_result,
                                   snobol_search_diag_t *diag) {
  /* Single-literal short-circuit: when the entire pattern is a single byte,
   * skip the prefilter, tier dispatch, and search_literal_only entirely.
   * Just memchr from start_offset — ~6 ns/call vs ~158 ns through the full
   * dispatch chain. The tier dispatch is still used for multi-byte literals,
   * non-literal patterns, anchored matches (which go through
   * snobol_search_exec_anchored → dispatch_search_impl directly), and when
   * a diagnostics struct is requested (the short-circuit doesn't populate it). */
  if (meta && meta->is_literal_only && meta->required_lit_len == 1 && !diag) {
    out_result->success = false;
    out_result->pike_overflowed = false;
    out_result->prefilter_skip = false;
    out_result->aborted = false;
    if (start_offset > subject_len) {
      return false;
    }
    const unsigned char *p = (const unsigned char *)memchr(
        subject + start_offset, meta->required_lit[0],
        subject_len - start_offset);
    if (p) {
      out_result->success = true;
      out_result->match_start = (size_t)(p - (const unsigned char *)subject);
      out_result->match_end = out_result->match_start + meta->required_lit_len;
    }
    return out_result->success;
  }

  /* Required-byte pre-filter: before entering any tier or DFA setup, check
   * whether a literal that MUST appear in the subject is present.  When the
   * literal is absent, return false immediately — no VM/tier/DFA invocation.
   * This is hoisted here (not inside dispatch_search_impl) so callers like
   * snobol_pattern_search can skip DFA building when the prefilter rejects. */
  if (search_prefilter_miss(meta, subject, subject_len, start_offset,
                            out_result)) {
    return false;
  }
  return dispatch_search_impl(vm, subject, subject_len, start_offset, meta, dfa,
                              out_result, diag, false);
}

snobol_search_tier_t snobol_search_executed_tier(
    const snobol_search_meta_t *meta, bool dfa_available, size_t subject_len,
    bool anchored) {
  if (!meta) {
    return TIER_GENERAL;
  }

  /* Mirror the tier-selection logic in dispatch_search_impl(): cost-model
   * selection, then the DFA override. */
  snobol_search_tier_t dispatch_tier =
      select_tier_by_cost(meta, subject_len, dfa_available, anchored);

  if (dfa_available && meta->automaton_eligible &&
      !meta->is_alt_literals_flat && meta->has_bmh_skip) {
    dispatch_tier = TIER_AUTOMATON;
  }

  return dispatch_tier;
}

SNOBOL_ALIGNED(64)
bool SNOBOL_HOT snobol_search_exec_anchored(VM *SNOBOL_RESTRICT vm,
                                            const char *SNOBOL_RESTRICT subject,
                                            size_t subject_len,
                                            const snobol_search_meta_t *meta,
                                            const snobol_dfa_t *dfa,
                                            snobol_search_result_t *out_result,
                                            snobol_search_diag_t *diag) {
  /* Required-byte pre-filter: an anchored match cannot succeed when a
   * literal required on every accepting path is absent from the subject, so
   * the same fail-fast as snobol_search_exec applies.  Necessary-condition
   * only: subjects containing the literal fall through to dispatch. */
  if (search_prefilter_miss(meta, subject, subject_len, 0, out_result)) {
    return false;
  }
  return dispatch_search_impl(vm, subject, subject_len, 0, meta, dfa,
                              out_result, diag, true);
}

/* ---------------------------------------------------------------------------
 * Public API: build an alt-literals trie from SPLIT/LIT bytecode.
 *
 * Walks the bytecode SPLIT tree, collects OP_LIT leaves, and builds a
 * trie.  Returns NULL on allocation failure or when the bytecode is not
 * a valid alt-literals SPLIT tree.  The caller owns the returned trie.
 * --------------------------------------------------------------------------- */
snobol_auto_trie_t *snobol_build_alt_trie(const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len < 2) {
    return nullptr;
  }
  snobol_auto_trie_t *trie =
      (snobol_auto_trie_t *)snobol_malloc(sizeof(snobol_auto_trie_t));
  if (!trie) {
    return nullptr;
  }
  trie->node_count = 1; /* root node (index 0) */
  trie->edge_count = 0;
  trie->nodes[0].first_edge = SNOBOL_AUTO_NULL;
  trie->nodes[0].is_end = false;
  trie->nodes[0].end_order = 0;

  bool all_ok = true;
  uint16_t leaf_order = 0; /* alternation branch order (visit order) */
  size_t stack[64];
  int sp = 0;
  stack[sp++] = 0;

  while (sp > 0 && all_ok) {
    size_t ip = stack[--sp];
    if (ip + 2 > bc_len) {
      all_ok = false;
      break;
    }
    uint8_t op = bc[ip];
    if (op == OP_LIT) {
      if (ip + 10 > bc_len) {
        all_ok = false;
        break;
      }
      uint32_t off = search_read_u32(bc, ip + 1);
      uint32_t len = search_read_u32(bc, ip + 5);
      if (off >= bc_len || off + len > bc_len) {
        all_ok = false;
        break;
      }
      if (!trie_insert(trie, bc + off, len, leaf_order++)) {
        all_ok = false;
        break;
      }
    } else if (op == OP_SPLIT) {
      if (ip + 9 > bc_len) {
        all_ok = false;
        break;
      }
      uint32_t a = search_read_u32(bc, ip + 1);
      uint32_t b = search_read_u32(bc, ip + 5);
      if (a >= bc_len || b >= bc_len) {
        all_ok = false;
        break;
      }
      if (sp + 2 > 64) {
        all_ok = false;
        break;
      }
      stack[sp++] = b;
      stack[sp++] = a;
    } else {
      all_ok = false;
      break;
    }
  }

  if (!all_ok) {
    snobol_free(trie);
    return nullptr;
  }
  return trie;
}
