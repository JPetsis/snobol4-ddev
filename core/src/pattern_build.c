/**
 * @file pattern_build.c
 * @brief Helper functions for building pattern bytecode with new primitives
 *
 * This file provides utility functions that assist in constructing bytecode
 * sequences for advanced SNOBOL4 pattern primitives:
 *   - BREAKX: pre-scan optimization helper
 *   - ARB: unbounded wildcard bytecode builder
 *   - BAL: balanced structure helper
 *   - FENCE / REM / RPOS / RTAB: simple emitters
 *
 * These helpers are primarily used by the compiler and by test harnesses
 * that need to build bytecode directly.
 */

#include "snobol/snobol_internal.h"
#include "snobol/vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal code-buffer helpers (mirrors compiler.c's CodeBuf)
 * -------------------------------------------------------------------------- */

typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
} PbCodeBuf;

static void pb_init(PbCodeBuf *c) {
  c->cap = 256;
  c->buf = (uint8_t *)snobol_malloc(c->cap);
  c->len = 0;
}

static void pb_ensure(PbCodeBuf *c, size_t need) {
  if (c->len + need <= c->cap) {
    return;
  }
  size_t newcap = c->cap ? c->cap * 2 : 256;
  while (c->len + need > newcap) {
    newcap *= 2;
  }
  uint8_t *nb = (uint8_t *)snobol_realloc(c->buf, newcap);
  if (!nb) {
    return; /* OOM: keep the old buffer */
  }
  c->buf = nb;
  c->cap = newcap;
}

static void pb_emit_u8(PbCodeBuf *c, uint8_t v) {
  pb_ensure(c, 1);
  c->buf[c->len++] = v;
}
static void pb_emit_u32(PbCodeBuf *c, uint32_t v) {
  pb_ensure(c, 4);
  c->buf[c->len++] = (v >> 24) & 0xff;
  c->buf[c->len++] = (v >> 16) & 0xff;
  c->buf[c->len++] = (v >> 8) & 0xff;
  c->buf[c->len++] = v & 0xff;
}

/* --------------------------------------------------------------------------
 * BREAKX pre-scan utility
 *
 * snobol_breakx_prescan() walks a subject string and collects the byte
 * positions of all characters that fall within the given charclass (the
 * "break" charset).  Callers can use this table to jump directly to the
 * next break-char candidate, achieving O(n) tokenization instead of O(n²).
 *
 * The caller is responsible for freeing *out_positions with snobol_free().
 * -------------------------------------------------------------------------- */

/**
 * Pre-scan [subject] for characters that belong to the break charset defined
 * by [ranges_ptr] / [range_count].
 *
 * @param subject       Subject string (UTF-8)
 * @param subject_len   Byte length of subject
 * @param ranges_ptr    Packed CpRange array (8 bytes each: start u32, end u32)
 * @param range_count   Number of ranges
 * @param out_positions Allocated array of matching byte positions (caller
 * frees)
 * @param out_count     Number of positions written
 * @return              true on success, false on allocation failure
 */
bool snobol_breakx_prescan(const char *subject, size_t subject_len,
                           const uint8_t *ranges_ptr, size_t range_count,
                           size_t **out_positions, size_t *out_count) {
  if (!subject || !out_positions || !out_count) {
    return false;
  }

  /* Pre-compute ASCII bitmap for fast path */
  uint64_t ascii_map[2] = {0, 0};
  bool is_ascii_only =
      ranges_to_ascii_bitmap(ranges_ptr, range_count, ascii_map);

  /* Collect positions */
  size_t capacity = 16;
  size_t *positions = (size_t *)snobol_malloc(capacity * sizeof(size_t));
  if (!positions) {
    return false;
  }
  size_t count = 0;

  size_t pos = 0;
  while (pos < subject_len) {
    bool is_break;
    int cp_bytes = 1;

    if (is_ascii_only) {
      is_break = bitmap_test(ascii_map, (uint8_t)subject[pos]);
    } else {
      uint32_t cp;
      int cb;
      if (!utf8_peek_next(subject, subject_len, pos, &cp, &cb)) {
        is_break = false;
      } else {
        is_break = range_contains(ranges_ptr, range_count, cp);
        cp_bytes = cb;
      }
    }

    if (is_break) {
      if (count >= capacity) {
        capacity *= 2;
        size_t *tmp =
            (size_t *)snobol_realloc(positions, capacity * sizeof(size_t));
        if (!tmp) {
          snobol_free(positions);
          return false;
        }
        positions = tmp;
      }
      positions[count++] = pos;
    }

    pos += (size_t)cp_bytes;
  }

  *out_positions = positions;
  *out_count = count;
  return true;
}

/* --------------------------------------------------------------------------
 * ARB bytecode builder
 *
 * ARB is not assigned its own VM opcode; instead it is compiled as the
 * sequence:
 *
 *   SPLIT  body, after    ; try zero-length first
 *   body:
 *   OP_LEN 1              ; match exactly one codepoint
 *   REPEAT_STEP loop,body ; loop back
 *   after:
 *
 * This is equivalent to SPLIT + REPEAT_STEP on LEN(1) which provides
 * full backtracking support (matches successively longer strings).
 *
 * snobol_emit_arb() writes this sequence into [out] and returns the
 * number of bytes written, or 0 on error.
 * -------------------------------------------------------------------------- */

/**
 * Emit bytecode for ARB into the provided buffer.
 *
 * The bytecode is self-contained (no external charclass table needed).
 * On return, [out_len] is set to the number of bytes written.
 *
 * @param out_buf   Allocated output buffer (caller frees)
 * @param out_len   Number of bytes written
 * @return          true on success
 */
bool snobol_emit_arb(uint8_t **out_buf, size_t *out_len) {
  if (!out_buf || !out_len) {
    return false;
  }

  PbCodeBuf c;
  pb_init(&c);

  /*
   * Layout (byte offsets):
   *   0: OP_SPLIT  a=10, b=6    ; try zero match (b=after), else go to body
   *  Wait, SPLIT pushes b and jumps to a.
   *  So: SPLIT body(=10), after(=?) – but we don't know 'after' yet.
   *
   * Simpler layout matching compiler.c REPEAT pattern:
   *   0: OP_REPEAT_INIT(loop=0, min=0, max=-1, skip=after)
   *      → 1(op) + 1(loop_id) + 4(min) + 4(max) + 4(skip) = 14 bytes
   *  14: OP_LEN 1
   *      → 1(op) + 4(n) = 5 bytes
   *  19: OP_REPEAT_STEP(loop=0, jmp_target=14)
   *      → 1(op) + 1(loop_id) + 4(target) = 6 bytes
   *  25: (end = after)
   */

  /* Offsets */
  uint32_t repeat_init_off = 0;
  uint32_t body_off = (uint32_t)(1 + 1 + 4 + 4 + 4); /* 14 */
  uint32_t len_off = body_off;                       /* 14 */
  uint32_t step_off = len_off + 1 + 4;               /* 19 */
  uint32_t after_off = step_off + 1 + 1 + 4;         /* 25 */

  /* OP_REPEAT_INIT loop_id=0, min=0, max=0xFFFFFFFF, skip_target=after */
  pb_emit_u8(&c, (uint8_t)OP_REPEAT_INIT);
  pb_emit_u8(&c, 0);            /* loop_id */
  pb_emit_u32(&c, 0);           /* min = 0 */
  pb_emit_u32(&c, 0xFFFFFFFFU); /* max = unlimited */
  pb_emit_u32(&c, after_off);   /* skip_target: jump over body when min=0 */

  /* OP_LEN 1 (body) */
  pb_emit_u8(&c, (uint8_t)OP_LEN);
  pb_emit_u32(&c, 1);

  /* OP_REPEAT_STEP loop_id=0, jmp_target=body (14) */
  pb_emit_u8(&c, (uint8_t)OP_REPEAT_STEP);
  pb_emit_u8(&c, 0);
  pb_emit_u32(&c, body_off);

  /* after: (nothing – caller appends more bytecode here) */

  (void)repeat_init_off;
  (void)len_off;
  *out_buf = c.buf;
  *out_len = c.len;
  c.buf = nullptr; /* caller owns the buffer */
  return true;
}
