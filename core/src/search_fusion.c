/**
 * @file search_fusion.c
 * @brief Tier 10: Fused concat-pattern execution engine.
 *
 * Executes fusible concat patterns (LIT/SPAN/ANY/NOTANY/BREAK chains) via a
 * dedicated lightweight engine — no VM, no bytecode dispatch, no choice stack.
 *
 * The fusion pass in search_meta.c compiles the bytecode into a flat segment
 * list.  exec_fusion() walks the segment list directly, matching each segment
 * in sequence.  On success, returns the match start/end.  On failure, the
 * caller advances to the next candidate.
 */

#include "snobol/search.h"
#include "snobol/vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


/* ---------------------------------------------------------------------------
 * Bitmap helpers for 256-bit fusion bitmaps (32 bytes).
 * ---------------------------------------------------------------------------
 */

static inline bool fusion_bitmap_test(const uint8_t bm[32], uint8_t b) {
  return (bm[b >> 3] & (uint8_t)(1U << (b & 7))) != 0;
}

/* ---------------------------------------------------------------------------
 * exec_fusion: execute a fused pattern at a given position.
 *
 * Walks the segment list produced by check_fusion_eligible, matching each
 * segment against the subject in sequence, starting at @p pos and advancing
 * the cursor by the length of every matched segment:
 *
 *  - FUSION_LIT:  memcmp the literal at the cursor
 *  - FUSION_RUN:  consume a run of bytes accepted by the segment's bitmap
 *  - FUSION_CHAR: single-byte bitmap test
 *  - FUSION_ALT:  try each alternative branch (each itself a small segment
 *                 list) at the same cursor; the first branch that matches
 *                 advances the cursor; a branch that fails or exceeds the
 *                 subject leaves the cursor untouched
 *
 * Matching is strictly sequential and side-effect free: the first segment
 * that fails aborts the whole attempt and returns false.  On success
 * *out_match_end is set to the position after the last matched segment.
 *
 * @param fusion       Compiled fusion segment list (from
 *                     check_fusion_eligible / snobol_fusion_build)
 * @param subject      Subject string
 * @param subject_len  Subject length
 * @param pos          Starting position
 * @param out_match_end Output: position after last matched segment (only on
 *                      success)
 * @return true if all segments matched; false on failure
 * ---------------------------------------------------------------------------
 */
static bool exec_fusion(const snobol_fusion_t *fusion, const char *subject,
                        size_t subject_len, size_t pos, size_t *out_match_end) {
  size_t cur = pos;

  for (uint32_t i = 0; i < fusion->count; i++) {
    const snobol_fusion_segment_t *seg = &fusion->segs[i];

    switch (seg->type) {
      case FUSION_LIT: {
        if (cur + seg->lit.len > subject_len) {
          return false;
        }
        if (memcmp(subject + cur, seg->lit.data, seg->lit.len) != 0) {
          return false;
        }
        cur += seg->lit.len;
        break;
      }

      case FUSION_RUN: {
        size_t start = cur;
        while (cur < subject_len &&
               fusion_bitmap_test(seg->run.bitmap, (uint8_t)subject[cur])) {
          cur++;
        }
        if (cur - start < seg->run.min) {
          return false;
        }
        break;
      }

      case FUSION_CHAR: {
        if (cur >= subject_len) {
          return false;
        }
        if (!fusion_bitmap_test(seg->chr.bitmap, (uint8_t)subject[cur])) {
          return false;
        }
        cur++;
        break;
      }

      case FUSION_ALT: {
        bool matched = false;
        size_t save_cur = cur;
        for (uint32_t j = 0; j < seg->alt.alt_count; j++) {
          snobol_fusion_segment_t *alt_segs = seg->alt.alts[j];
          uint32_t alt_len = seg->alt.alt_lens[j];
          if (!alt_segs || alt_len == 0) {
            continue;
          }

          cur = save_cur;
          bool alt_matched = true;
          for (uint32_t k = 0; k < alt_len; k++) {
            const snobol_fusion_segment_t *alt_seg = &alt_segs[k];
            switch (alt_seg->type) {
              case FUSION_LIT: {
                if (cur + alt_seg->lit.len > subject_len) {
                  alt_matched = false;
                } else if (memcmp(subject + cur, alt_seg->lit.data,
                                  alt_seg->lit.len) != 0) {
                  alt_matched = false;
                } else {
                  cur += alt_seg->lit.len;
                }
                break;
              }
              case FUSION_RUN: {
                size_t start = cur;
                while (cur < subject_len &&
                       fusion_bitmap_test(alt_seg->run.bitmap,
                                          (uint8_t)subject[cur])) {
                  cur++;
                }
                if (cur - start < alt_seg->run.min) {
                  alt_matched = false;
                }
                break;
              }
              case FUSION_CHAR: {
                if (cur >= subject_len) {
                  alt_matched = false;
                } else if (!fusion_bitmap_test(alt_seg->chr.bitmap,
                                               (uint8_t)subject[cur])) {
                  alt_matched = false;
                } else {
                  cur++;
                }
                break;
              }
              default: alt_matched = false; break;
            }
            if (!alt_matched) {
              break;
            }
          }
          if (alt_matched) {
            matched = true;
            break;
          }
        }
        if (!matched) {
          return false;
        }
        break;
      }

      default: return false;
    }
  }

  *out_match_end = cur;
  return true;
}

/* ---------------------------------------------------------------------------
 * tier_fusion: Tier 10 dispatch entry point.
 *
 * For anchored matches: run exec_fusion once at start_offset.
 * For unanchored matches: use prefilter (memchr/memmem) to find candidate
 * positions, then verify each with exec_fusion.
 * ---------------------------------------------------------------------------
 */
bool tier_fusion(VM *vm, const char *subject, size_t subject_len,
                 size_t start_offset, const snobol_search_meta_t *meta,
                 const snobol_dfa_t *dfa, snobol_search_result_t *out_result,
                 snobol_search_diag_t *diag, bool anchored) {
  (void)vm;
  (void)dfa;

  if (!meta || !meta->fusion || !meta->fusion_eligible) {
    out_result->success = false;
    return false;
  }

  const snobol_fusion_t *fusion = meta->fusion;

  if (anchored) {
    size_t match_end = 0;
    if (exec_fusion(fusion, subject, subject_len, start_offset, &match_end)) {
      out_result->success = true;
      out_result->match_start = start_offset;
      out_result->match_end = match_end;
      return true;
    }
    out_result->success = false;
    return false;
  }

  size_t offset = start_offset;

  while (offset < subject_len) {
    if (diag) {
      diag->candidates_tested++;
    }

    size_t match_end = 0;
    if (exec_fusion(fusion, subject, subject_len, offset, &match_end)) {
      out_result->success = true;
      out_result->match_start = offset;
      out_result->match_end = match_end;
      return true;
    }

    if (offset >= subject_len) {
      break;
    }
    offset++;
  }

  out_result->success = false;
  return false;
}
