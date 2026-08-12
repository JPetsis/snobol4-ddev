/**
 * @file vm_exec.c
 * @brief Main execution loop and support for the full SNOBOL4 VM.
 *
 * Split out of vm.c (see oss-readiness T4). Implements charclass range
 * resolution (get_ranges_ptr / range meta / bitmap conversions), the output
 * buffer helpers, the dispatch loop (vm_run / vm_exec), and label/table/array
 * registration. The choice stack lives in vm_choice.c; the capture write-log
 * lives in vm_capture.c.
 */

#include "snobol/vm.h"
#include "snobol/array.h"
#include "snobol/dynamic_pattern.h"
#include "snobol/snobol_attrs.h"
#include "snobol/snobol_internal.h"
#include "snobol/string_fn.h"
#include "snobol/table.h"
#include "snobol/type_fn.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

/* Portable count-trailing-zeros for 64-bit. MSVC has no
 * __builtin_ctzll, so use _BitScanForward64 there. */
static inline int snobol_ctz64(uint64_t v) {
#if defined(_MSC_VER) && !defined(__clang__)
  unsigned long idx;
  _BitScanForward64(&idx, v);
  return (int)idx;
#else
  return __builtin_ctzll(v);
#endif
}

/* Return the single ASCII byte represented by a bitmap, or -1 if the
 * bitmap contains zero or more than one byte.  Only examines bytes 0-127. */
static inline int bitmap_single_ascii_byte(const uint64_t map[2]) {
  uint64_t lo = map[0];
  uint64_t hi = map[1];
  if (hi == 0 && lo != 0 && (lo & (lo - 1)) == 0) {
    return snobol_ctz64(lo);
  }
  if (lo == 0 && hi != 0 && (hi & (hi - 1)) == 0) {
    return 64 + snobol_ctz64(hi);
  }
  return -1;
}

/* Minimal code buffer for dynamic pattern compilation in VM */
typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
} VmCodeBuf;

static void vm_cb_ensure(VmCodeBuf *c, size_t need) {
  if (c->len + need <= c->cap) {
    return;
  }
  size_t newcap = c->cap ? c->cap * 2 : 4096;
  while (c->len + need > newcap) {
    newcap *= 2;
  }
  c->buf = snobol_realloc(c->buf, newcap);
  c->cap = newcap;
}
static void vm_cb_emit_u8(VmCodeBuf *c, uint8_t v) {
  vm_cb_ensure(c, 1);
  c->buf[c->len++] = v;
}
static void vm_cb_emit_u32(VmCodeBuf *c, uint32_t v) {
  vm_cb_ensure(c, 4);
  c->buf[c->len++] = (v >> 24) & 0xff;
  c->buf[c->len++] = (v >> 16) & 0xff;
  c->buf[c->len++] = (v >> 8) & 0xff;
  c->buf[c->len++] = v & 0xff;
}
static void vm_cb_emit_bytes(VmCodeBuf *c, const uint8_t *b, size_t n) {
  if (n == 0) {
    return;
  }
  vm_cb_ensure(c, n);
  memcpy(c->buf + c->len, b, n);
  c->len += n;
}

/* Magic that compiler appends after the label table (last 4 bytes of new-format
 * bc). Old-format bytecodes have charclass_count at bc_len-4 (typically <
 * 65536). 0x534E424C = "SNBL" — safely outside any realistic charclass_count
 * range. Guard prevents a redefinition error in the amalgam build where
 * compiler.c (included before vm.c) already declares this constant. */
#ifndef SNOBOL_LABEL_TABLE_MAGIC_DEFINED
#define SNOBOL_LABEL_TABLE_MAGIC_DEFINED
#define SNOBOL_LABEL_TABLE_MAGIC 0x534E424Cu
#endif

const uint8_t *get_ranges_ptr(const VM *vm, uint16_t set_id,
                              uint16_t *out_count, uint16_t *out_case) {
  if (set_id == 0) {
    return nullptr;
  }

  /* Fast path: use cached range metadata if available.
   * The table is indexed by (set_id - 1) and built at compile time. */
  if (vm->range_meta && (size_t)set_id <= vm->range_meta_count) {
    const snobol_range_meta_t *entry = &vm->range_meta[set_id - 1];
    if (entry->ranges_ptr) {
      *out_count = entry->count;
      *out_case = entry->case_insensitive;
      return entry->ranges_ptr;
    }
  }

  size_t tail_ip = vm->bc_len;
  if (tail_ip < 4) {
    return nullptr;
  }

  /* Detect bytecode format:
   *   NEW format (compiler-produced): last 4 bytes == SNOBOL_LABEL_TABLE_MAGIC
   *     layout (from end): MAGIC | label_count | label_offsets[] |
   * charclass_count | ... OLD format (hand-built tests): last 4 bytes ==
   * charclass_count
   *
   * For charclass lookup we only need to find charclass_count; in new format
   * we skip past the label table first. */
  uint32_t last4 = ((uint32_t)vm->bc[tail_ip - 4] << 24) |
                   ((uint32_t)vm->bc[tail_ip - 3] << 16) |
                   ((uint32_t)vm->bc[tail_ip - 2] << 8) |
                   (uint32_t)vm->bc[tail_ip - 1];

  size_t cc_tail; /* position just after charclass_count field */
  if (last4 == SNOBOL_LABEL_TABLE_MAGIC) {
    /* New format: read label_count from the u32 before the MAGIC */
    if (tail_ip < 8) {
      return nullptr;
    }
    uint32_t label_count = ((uint32_t)vm->bc[tail_ip - 8] << 24) |
                           ((uint32_t)vm->bc[tail_ip - 7] << 16) |
                           ((uint32_t)vm->bc[tail_ip - 6] << 8) |
                           (uint32_t)vm->bc[tail_ip - 5];
    /* charclass_count sits just before: [label_offsets…][label_count][MAGIC] */
    size_t skip =
        8 + ((size_t)label_count * 4); /* 4 (label_count) + 4 (MAGIC) + N*4 */
    if (tail_ip < skip + 4) {
      return nullptr;
    }
    cc_tail = tail_ip - skip;
  } else {
    /* Old format: charclass_count IS at bc_len-4 */
    cc_tail = tail_ip;
  }

  if (cc_tail < 4) {
    return nullptr;
  }
  uint32_t class_count = ((uint32_t)vm->bc[cc_tail - 4] << 24) |
                         ((uint32_t)vm->bc[cc_tail - 3] << 16) |
                         ((uint32_t)vm->bc[cc_tail - 2] << 8) |
                         (uint32_t)vm->bc[cc_tail - 1];
  if (set_id > class_count) {
    return nullptr;
  }
  size_t table_size = (size_t)class_count * 4;
  if (cc_tail < 4 + table_size) {
    return nullptr;
  }
  size_t table_start = cc_tail - 4 - table_size;
  size_t offset_pos = table_start + ((size_t)(set_id - 1) * 4);
  uint32_t offset = ((uint32_t)vm->bc[offset_pos + 0] << 24) |
                    ((uint32_t)vm->bc[offset_pos + 1] << 16) |
                    ((uint32_t)vm->bc[offset_pos + 2] << 8) |
                    (uint32_t)vm->bc[offset_pos + 3];
  if (offset >= vm->bc_len) {
    return nullptr;
  }
  size_t ip = (size_t)offset;
  *out_count = read_u16(vm->bc, vm->bc_len, &ip);
  *out_case = read_u16(vm->bc, vm->bc_len, &ip);
  return vm->bc + ip;
}

void snobol_build_range_meta(const uint8_t *bc, size_t bc_len,
                             snobol_range_meta_t **out_table,
                             size_t *out_count) {
  *out_table = nullptr;
  *out_count = 0;
  if (!bc || bc_len < 4) {
    return;
  }

  /* Detect charclass_count — same logic as get_ranges_ptr
   * (we cannot call get_ranges_ptr here to detect the count
   * because the cache doesn't exist yet). */
  size_t tail_ip = bc_len;
  uint32_t last4 = ((uint32_t)bc[tail_ip - 4] << 24) |
                   ((uint32_t)bc[tail_ip - 3] << 16) |
                   ((uint32_t)bc[tail_ip - 2] << 8) | (uint32_t)bc[tail_ip - 1];

  size_t cc_tail;
  if (last4 == SNOBOL_LABEL_TABLE_MAGIC) {
    if (tail_ip < 8) {
      return;
    }
    uint32_t label_count =
        ((uint32_t)bc[tail_ip - 8] << 24) | ((uint32_t)bc[tail_ip - 7] << 16) |
        ((uint32_t)bc[tail_ip - 6] << 8) | (uint32_t)bc[tail_ip - 5];
    size_t skip = 8 + ((size_t)label_count * 4);
    if (tail_ip < skip + 4) {
      return;
    }
    cc_tail = tail_ip - skip;
  } else {
    cc_tail = tail_ip;
  }

  if (cc_tail < 4) {
    return;
  }
  uint32_t class_count =
      ((uint32_t)bc[cc_tail - 4] << 24) | ((uint32_t)bc[cc_tail - 3] << 16) |
      ((uint32_t)bc[cc_tail - 2] << 8) | (uint32_t)bc[cc_tail - 1];

  if (class_count == 0 || class_count > 65535) {
    return;
  }

  snobol_range_meta_t *table =
      snobol_malloc((size_t)class_count * sizeof(snobol_range_meta_t));
  if (!table) {
    return;
  }

  /* Build a temporary VM — range_meta is NULL so get_ranges_ptr
   * will use the fallback re-parse path (exactly what we want:
   * we are building the cache for future use). */
  VM tmp_vm;
  memset(&tmp_vm, 0, sizeof(tmp_vm));
  tmp_vm.bc = bc;
  tmp_vm.bc_len = bc_len;

  for (uint32_t id = 1; id <= class_count; id++) {
    uint16_t count = 0;
    uint16_t ci = 0;
    const uint8_t *ptr = get_ranges_ptr(&tmp_vm, (uint16_t)id, &count, &ci);
    table[id - 1].ranges_ptr = ptr;
    table[id - 1].count = count;
    table[id - 1].case_insensitive = ci;
  }

  *out_table = table;
  *out_count = (size_t)class_count;
}

bool ranges_to_ascii_bitmap(const uint8_t *ranges_ptr, size_t count,
                            uint64_t map[2]) {
  map[0] = map[1] = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t offset = i * 8;
    uint32_t start = ((uint32_t)ranges_ptr[offset + 0] << 24) |
                     ((uint32_t)ranges_ptr[offset + 1] << 16) |
                     ((uint32_t)ranges_ptr[offset + 2] << 8) |
                     (uint32_t)ranges_ptr[offset + 3];
    uint32_t end = ((uint32_t)ranges_ptr[offset + 4] << 24) |
                   ((uint32_t)ranges_ptr[offset + 5] << 16) |
                   ((uint32_t)ranges_ptr[offset + 6] << 8) |
                   (uint32_t)ranges_ptr[offset + 7];
    if (start > 127 || end > 127) {
      return false;
    }
    for (uint32_t c = start; c <= end; ++c) {
      if (c < 64) {
        map[0] |= (1ULL << c);
      } else {
        map[1] |= (1ULL << (c - 64));
      }
    }
  }
  return true;
}

bool ranges_to_full_bitmap(const uint8_t *ranges_ptr, size_t count,
                           uint64_t map[4]) {
  map[0] = map[1] = map[2] = map[3] = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t offset = i * 8;
    uint32_t start = ((uint32_t)ranges_ptr[offset + 0] << 24) |
                     ((uint32_t)ranges_ptr[offset + 1] << 16) |
                     ((uint32_t)ranges_ptr[offset + 2] << 8) |
                     (uint32_t)ranges_ptr[offset + 3];
    uint32_t end = ((uint32_t)ranges_ptr[offset + 4] << 24) |
                   ((uint32_t)ranges_ptr[offset + 5] << 16) |
                   ((uint32_t)ranges_ptr[offset + 6] << 8) |
                   (uint32_t)ranges_ptr[offset + 7];
    if (start > 255 || end > 255) {
      return false;
    }
    for (uint32_t c = start; c <= end; ++c) {
      unsigned idx = (unsigned)c >> 6;
      map[idx] |= (1ULL << ((unsigned)c & 63));
    }
  }
  return true;
}

bool range_contains(const uint8_t *ranges_ptr, size_t count, uint32_t cp) {
  size_t lo = 0;
  size_t hi = count;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) / 2);
    size_t offset = mid * 8;
    uint32_t start = ((uint32_t)ranges_ptr[offset + 0] << 24) |
                     ((uint32_t)ranges_ptr[offset + 1] << 16) |
                     ((uint32_t)ranges_ptr[offset + 2] << 8) |
                     (uint32_t)ranges_ptr[offset + 3];
    uint32_t end = ((uint32_t)ranges_ptr[offset + 4] << 24) |
                   ((uint32_t)ranges_ptr[offset + 5] << 16) |
                   ((uint32_t)ranges_ptr[offset + 6] << 8) |
                   (uint32_t)ranges_ptr[offset + 7];
    if (cp < start) {
      hi = mid;
    } else if (cp > end) {
      lo = mid + 1;
    } else {
      return true;
    }
  }
  return false;
}

void snobol_buf_init(snobol_buf *b) {
  b->cap = 1024;
  b->data = snobol_malloc(b->cap);
  b->len = 0;
}
void snobol_buf_append(snobol_buf *b, const char *data, size_t len) {
  if (len == 0) {
    return;
  }
  if (b->len + len >= b->cap) {
    size_t newcap = b->cap ? b->cap * 2 : 1024;
    while (b->len + len >= newcap) {
      newcap *= 2;
    }
    b->data = snobol_realloc(b->data, newcap);
    b->cap = newcap;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
}
void snobol_buf_clear(snobol_buf *b) {
  b->len = 0;
  if (b->data) {
    b->data[0] = '\0';
  }
}
void snobol_buf_free(snobol_buf *b) {
  if (b->data) {
    snobol_free(b->data);
    b->data = nullptr;
  }
  b->len = b->cap = 0;
}

/**
 * Resolve the SNOBOL_LEGACY_CHOICE env var ONCE per process and cache it.
 *
 * The legacy choice-mode toggle is a static deployment setting, not expected
 * to change mid-process. Reading it per match (the previous behaviour) put a
 * getenv()/__findenv_locked() call on the hot path of every anchored match,
 * which the search-perf investigation showed as a measurable cost. We read it
 * a single time and reuse the cached value thereafter.
 *
 * Thread-safety: the first call races are benign — all writers store the same
 * value derived from the (never-changing) environment at process start.
 */
static bool vm_legacy_choice_mode(void) {
  static int resolved = 0;
  static bool legacy = false;
  if (!resolved) {
    legacy = (getenv("SNOBOL_LEGACY_CHOICE") != nullptr);
    resolved = 1;
  }
  return legacy;
}

bool vm_run(VM *vm) {
  if (!vm->choices_arena) {
    vm->choices_arena = vm_arena_create();
    if (!vm->choices_arena) {
      return false;
    }
  } else {
    vm_arena_reset(vm->choices_arena);
  }
  vm->choices_top = 0;
  /* Cache the choice-mode flag once per process instead of getenv() per match. */
  vm->use_compact_choice = ((!vm_legacy_choice_mode()) != 0);
  if (vm->use_compact_choice) {
    if (!vm->trail) {
      vm_trail_init(vm);
      vm->trail_cap = 256;
      vm->trail = snobol_malloc(vm->trail_cap * sizeof(UndoRecord));
      if (!vm->trail) {
        if (!vm->keep_choices) {
          vm_arena_destroy(vm->choices_arena);
          vm->choices_arena = nullptr;
        }
        return false;
      }
    } else {
      vm_trail_clear(vm);
    }
    /* Write-log retained for API compatibility; not used by the trail. */
    if (!vm->write_log) {
      vm_write_log_init(vm);
      vm->write_log_cap = MAX_CAPS;
      vm->write_log = snobol_malloc(vm->write_log_cap * sizeof(WriteLogEntry));
    } else {
      vm_write_log_clear(vm);
    }
  }

#ifndef _MSC_VER
  static void *opcode_table[] = {
      [OP_ACCEPT] = &&op_accept,
      [OP_FAIL] = &&op_fail,
      [OP_JMP] = &&op_jmp,
      [OP_SPLIT] = &&op_split,
      [OP_LIT] = &&op_lit,
      [OP_ANY] = &&op_any,
      [OP_NOTANY] = &&op_notany,
      [OP_SPAN] = &&op_span,
      [OP_BREAK] = &&op_break,
      [OP_CAP_START] = &&op_cap_start,
      [OP_CAP_END] = &&op_cap_end,
      [OP_ASSIGN] = &&op_assign,
      [OP_LEN] = &&op_len,
      [OP_EVAL] = &&op_eval,
      [OP_ANCHOR] = &&op_anchor,
      [OP_REPEAT_INIT] = &&op_repeat_init,
      [OP_REPEAT_STEP] = &&op_repeat_step,
      [OP_EMIT_LITERAL] = &&op_emit_literal,
      [OP_EMIT_CAPTURE] = &&op_emit_capture,
      [OP_EMIT_EXPR] = &&op_emit_expr,
      [OP_EMIT_TABLE] = &&op_emit_table,
      [OP_EMIT_FORMAT] = &&op_emit_format,
      [OP_LABEL] = &&op_label,
      [OP_GOTO] = &&op_goto,
      [OP_GOTO_F] = &&op_goto_f,
#ifdef SNOBOL_DYNAMIC_PATTERN
      [OP_TABLE_GET] = &&op_table_get,
      [OP_TABLE_SET] = &&op_table_set,
      [OP_ARRAY_GET] = &&op_array_get,
      [OP_ARRAY_SET] = &&op_array_set,
      [OP_DYNAMIC] = &&op_dynamic,
      [OP_DYNAMIC_DEF] = &&op_dynamic_def,
#endif
      [OP_BREAKX] = &&op_breakx,
      [OP_BAL] = &&op_bal,
      [OP_FENCE] = &&op_fence,
      [OP_REM] = &&op_rem,
      [OP_RPOS] = &&op_rpos,
      [OP_RTAB] = &&op_rtab,
      [OP_POS] = &&op_pos,
      [OP_TAB] = &&op_tab,
      [OP_ABORT] = &&op_abort,
      [OP_SUCCEED] = &&op_succeed,
      [OP_NOP] = &&op_nop,
  };
#endif

  while (1) {
#ifdef SNOBOL_PROFILE
    vm->profile.dispatch_count++;
#endif
    if (vm->ip >= vm->bc_len) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      continue;
    }
    uint8_t op = vm->bc[vm->ip++];
#ifndef _MSC_VER
    goto *opcode_table[op];
#else
    switch (op) {
#endif
#ifndef _MSC_VER
  op_nop:
#endif
#ifdef _MSC_VER
  case OP_NOP:
#endif
    /* fusion filler — skip one byte */
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
#ifndef _MSC_VER
  op_accept:
#endif
#ifdef _MSC_VER
  case OP_ACCEPT:
#endif
    if (!vm->keep_choices) {
      vm_arena_destroy(vm->choices_arena);
      vm->choices_arena = nullptr;
      if (vm->use_compact_choice) {
        vm_write_log_free(vm);
        vm_trail_free(vm); /* free the undo trail allocated in vm_run */
      }
    }
    return true;
#ifndef _MSC_VER
  op_fail:
#endif
#ifdef _MSC_VER
  case OP_FAIL:
#endif
  {
    bool had = vm_pop_choice(vm);
    if (!had) {
      goto fail_ret;
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_jmp:
#endif
#ifdef _MSC_VER
  case OP_JMP:
#endif
  {
    uint32_t tgt = read_u32(vm->bc, vm->bc_len, &vm->ip);
    vm->ip = (size_t)tgt;
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_split:
#endif
#ifdef _MSC_VER
  case OP_SPLIT:
#endif
  {
    uint32_t a = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t b = read_u32(vm->bc, vm->bc_len, &vm->ip);
    vm_push_choice(vm, (size_t)b, vm->pos);
    vm->ip = (size_t)a;
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_lit:
#endif
#ifdef _MSC_VER
  case OP_LIT:
#endif
  {
    uint32_t off = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t len = read_u32(vm->bc, vm->bc_len, &vm->ip);
    if (off == vm->ip) {
      vm->ip += len;
    }
    if (len <= vm->len - vm->pos &&
        memcmp(vm->s + vm->pos, vm->bc + off, len) == 0) {
      vm->pos += len;
    } else if (!vm_pop_choice(vm)) {
      { goto fail_ret; }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_any:
#endif
#ifdef _MSC_VER
  case OP_ANY:
#endif
  {
    uint16_t set_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint16_t count;
    uint16_t ci;
    const uint8_t *ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    uint64_t map[2];
    bool is_ascii = (ranges && ranges_to_ascii_bitmap(ranges, count, map)) != 0;
    if (is_ascii) {
      if (vm->pos < vm->len && bitmap_test(map, (uint8_t)vm->s[vm->pos])) {
        vm->pos++;
      } else if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(vm->s, vm->len, vm->pos, &cp, &bytes)) {
        if (!vm_pop_choice(vm)) {
          goto fail_ret;
        }
      } else if (ranges && range_contains(ranges, count, cp)) {
        { vm->pos += bytes; }
      } else if (!vm_pop_choice(vm)) {
        { goto fail_ret; }
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_notany:
#endif
#ifdef _MSC_VER
  case OP_NOTANY:
#endif
  {
    uint16_t set_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint16_t count;
    uint16_t ci;
    const uint8_t *ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    uint64_t map[2];
    bool is_ascii = (ranges && ranges_to_ascii_bitmap(ranges, count, map)) != 0;
    if (is_ascii) {
      if (vm->pos < vm->len && !bitmap_test(map, (uint8_t)vm->s[vm->pos])) {
        vm->pos++;
      } else if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(vm->s, vm->len, vm->pos, &cp, &bytes)) {
        if (!vm_pop_choice(vm)) {
          goto fail_ret;
        }
      } else if (ranges && range_contains(ranges, count, cp)) {
        if (!vm_pop_choice(vm)) {
          goto fail_ret;
        }
      } else {
        { vm->pos += bytes; }
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_span:
#endif
#ifdef _MSC_VER
  case OP_SPAN:
#endif
  {
    uint16_t set_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint16_t count;
    uint16_t ci;
    const uint8_t *ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    uint64_t map[2];
    bool is_ascii = (ranges && ranges_to_ascii_bitmap(ranges, count, map)) != 0;
    if (is_ascii) {
      if (vm->pos < vm->len && bitmap_test(map, (uint8_t)vm->s[vm->pos])) {
        vm->pos++;
        while (vm->pos < vm->len && bitmap_test(map, (uint8_t)vm->s[vm->pos])) {
          vm->pos++;
        }
      } else if (!vm_pop_choice(vm)) {
        { goto fail_ret; }
      }
    } else {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(vm->s, vm->len, vm->pos, &cp, &bytes) || !ranges ||
          !range_contains(ranges, count, cp)) {
        if (!vm_pop_choice(vm)) {
          goto fail_ret;
        }
      } else {
        vm->pos += bytes;
        while (utf8_peek_next(vm->s, vm->len, vm->pos, &cp, &bytes) &&
               range_contains(ranges, count, cp)) {
          vm->pos += bytes;
        }
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_break:
#endif
#ifdef _MSC_VER
  case OP_BREAK:
#endif
  {
    uint16_t set_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint16_t count;
    uint16_t ci;
    const uint8_t *ranges = get_ranges_ptr(vm, set_id, &count, &ci);
    uint64_t map[2];
    bool is_ascii = (ranges && ranges_to_ascii_bitmap(ranges, count, map)) != 0;
    if (is_ascii) {
      int single = bitmap_single_ascii_byte(map);
      if (single >= 0 && vm->pos < vm->len) {
        const void *p =
            memchr(vm->s + vm->pos, (unsigned char)single, vm->len - vm->pos);
        if (p) {
          vm->pos = (size_t)((const uint8_t *)p - (const uint8_t *)vm->s);
        } else {
          vm->pos = vm->len;
        }
      } else {
        while (vm->pos < vm->len &&
               !bitmap_test(map, (uint8_t)vm->s[vm->pos])) {
          vm->pos++;
        }
      }
    } else {
      uint32_t cp;
      int bytes;
      while (utf8_peek_next(vm->s, vm->len, vm->pos, &cp, &bytes) &&
             (!ranges || !range_contains(ranges, count, cp))) {
        vm->pos += bytes;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_cap_start:
#endif
#ifdef _MSC_VER
  case OP_CAP_START:
#endif
  {
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    if (r < MAX_CAPS) {
      if (vm->use_compact_choice) {
        vm_trail_cap_write(vm, r, 0, vm->cap_start[r], vm->cap_end[r]);
      }
      vm->cap_start[r] = vm->pos;
      if (r >= vm->max_cap_used) {
        vm->max_cap_used = r + 1;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_cap_end:
#endif
#ifdef _MSC_VER
  case OP_CAP_END:
#endif
  {
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    if (r < MAX_CAPS) {
      if (vm->use_compact_choice) {
        vm_trail_cap_write(vm, r, 1, vm->cap_start[r], vm->cap_end[r]);
      }
      vm->cap_end[r] = vm->pos;
      if (r >= vm->max_cap_used) {
        vm->max_cap_used = r + 1;
      }
      /* Also expose the capture register as variable v<r> so
           it appears in the match result without an explicit
           OP_ASSIGN.  Cap registers and var indices are 1:1. */
      if (r < MAX_VARS) {
        if (vm->use_compact_choice) {
          vm_trail_var_write(vm, (uint8_t)r, vm->var_start[r], vm->var_end[r]);
        }
        vm->var_start[r] = vm->cap_start[r];
        vm->var_end[r] = vm->cap_end[r];
        if ((size_t)r + 1 > vm->var_count) {
          vm->var_count = (size_t)r + 1;
        }
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_assign:
#endif
#ifdef _MSC_VER
  case OP_ASSIGN:
#endif
  {
    uint16_t var = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    if (var < MAX_VARS && r < MAX_CAPS) {
      if (vm->use_compact_choice) {
        vm_trail_var_write(vm, (uint8_t)var, vm->var_start[var],
                           vm->var_end[var]);
      }
      if (var >= vm->var_count) {
        vm->var_count = (size_t)var + 1;
      }
      vm->var_start[var] = vm->cap_start[r];
      vm->var_end[var] = vm->cap_end[r];
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_len:
#endif
#ifdef _MSC_VER
  case OP_LEN:
#endif
  {
    uint32_t n = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t p = vm->pos;
    uint32_t i;
    for (i = 0; i < n; ++i) {
      uint32_t cp;
      int bytes;
      if (!utf8_peek_next(vm->s, vm->len, p, &cp, &bytes)) {
        break;
      }
      p += bytes;
    }
    if (i != n) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      { vm->pos = p; }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_eval:
#endif
#ifdef _MSC_VER
  case OP_EVAL:
#endif
  {
    /* Built-in dispatch table.
       * If fn_id is a known built-in (< SNOBOL_FN_MAX), dispatch
       * directly to the C function.  Otherwise fall back to the
       * host eval_fn callback to preserve backward compatibility. */
    uint16_t fn = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    if (r >= MAX_CAPS) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
#ifdef _MSC_VER
      break;
#else
          continue;
#endif
    }

    /* Validate capture bounds */
    bool cap_ok = ((vm->cap_end[r] >= vm->cap_start[r]) &&
                   (vm->cap_end[r] <= vm->len)) != 0;
    if (!cap_ok) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
#ifdef _MSC_VER
      break;
#else
          continue;
#endif
    }

    const char *cap_s = vm->s + vm->cap_start[r];
    size_t cap_l = vm->cap_end[r] - vm->cap_start[r];
    bool ok = true;

    SNOBOL_LOG("OP_EVAL: fn=%u reg=%u cap_len=%zu", fn, r, cap_l);

    if (fn > SNOBOL_FN_NONE && fn < SNOBOL_FN_MAX) {
      /* Direct C dispatch (no host callback) */
      snobol_buf tmp_out = {nullptr};
      snobol_buf_init(&tmp_out);

      switch ((snobol_builtin_fn_t)fn) {
        /* --- string transformations (output to vm->out if set) --- */
        case SNOBOL_FN_SIZE: {
          /* SIZE: always succeeds, useful as predicate */
          (void)snobol_size(cap_s, cap_l);
          ok = true;
          break;
        }
        case SNOBOL_FN_TRIM: {
          ok = snobol_trim(cap_s, cap_l, &tmp_out);
          if (ok && vm->out) {
            snobol_buf_append(vm->out, tmp_out.data, tmp_out.len);
          }
          if (ok && vm->emit_fn) {
            vm->emit_fn(tmp_out.data, tmp_out.len, vm->emit_udata);
          }
          break;
        }
        case SNOBOL_FN_REVERSE: {
          ok = snobol_reverse(cap_s, cap_l, &tmp_out);
          if (ok && vm->out) {
            snobol_buf_append(vm->out, tmp_out.data, tmp_out.len);
          }
          if (ok && vm->emit_fn) {
            vm->emit_fn(tmp_out.data, tmp_out.len, vm->emit_udata);
          }
          break;
        }
        case SNOBOL_FN_UPPER: {
          ok = snobol_upper(cap_s, cap_l, &tmp_out);
          if (ok && vm->out) {
            snobol_buf_append(vm->out, tmp_out.data, tmp_out.len);
          }
          if (ok && vm->emit_fn) {
            vm->emit_fn(tmp_out.data, tmp_out.len, vm->emit_udata);
          }
          break;
        }
        case SNOBOL_FN_LOWER: {
          ok = snobol_lower(cap_s, cap_l, &tmp_out);
          if (ok && vm->out) {
            snobol_buf_append(vm->out, tmp_out.data, tmp_out.len);
          }
          if (ok && vm->emit_fn) {
            vm->emit_fn(tmp_out.data, tmp_out.len, vm->emit_udata);
          }
          break;
        }
        /* --- predicates (succeed or fail, no output) --- */
        case SNOBOL_FN_INTEGER: ok = snobol_integer(cap_s, cap_l); break;
        case SNOBOL_FN_REAL: ok = snobol_real(cap_s, cap_l); break;
        case SNOBOL_FN_NUMERIC: ok = snobol_numeric(cap_s, cap_l); break;
        /* Multi-arg functions (IDENT, DIFFER, LEX*, REPLACE, DUPL,
         * SUBSTR, LPAD, RPAD, CHAR, ORD) require additional context
         * not available via single-register OP_EVAL.  Fall back to
         * host callback when fn_id reaches these cases. */
        default:
          /* Fallback to host eval_fn */
          ok = ((!vm->eval_fn || vm->eval_fn((int)fn, vm->s, vm->cap_start[r],
                                             vm->cap_end[r], vm->eval_udata)) !=
                0);
          break;
      }
      snobol_buf_free(&tmp_out);
    } else {
      /* Host callback fallback (fn == 0 or unknown >= SNOBOL_FN_MAX) */
      ok = ((!vm->eval_fn || vm->eval_fn((int)fn, vm->s, vm->cap_start[r],
                                         vm->cap_end[r], vm->eval_udata)) != 0);
    }
    if (!ok) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_anchor:
#endif
#ifdef _MSC_VER
  case OP_ANCHOR:
#endif
  {
    uint8_t type = read_u8(vm->bc, vm->bc_len, &vm->ip);
    bool ok = ((type == 0) ? (vm->pos == 0) : (vm->pos == vm->len)) != 0;
    if (!ok) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_repeat_init:
#endif
#ifdef _MSC_VER
  case OP_REPEAT_INIT:
#endif
  {
    uint8_t loop_id = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint32_t min = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t max = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t skip = read_u32(vm->bc, vm->bc_len, &vm->ip);
    if (loop_id < MAX_LOOPS) {
      vm->counters[loop_id] = 0;
      vm->loop_min[loop_id] = min;
      vm->loop_max[loop_id] = max;
      vm->loop_last_pos[loop_id] = vm->pos;
      if (loop_id + 1 > vm->max_counter_used) {
        vm->max_counter_used = loop_id + 1;
      }
      if (min == 0) {
        vm_push_choice(vm, (size_t)skip, vm->pos);
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_repeat_step:
#endif
#ifdef _MSC_VER
  case OP_REPEAT_STEP:
#endif
  {
    uint8_t loop_id = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint32_t target = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t step_ip = vm->ip; /* save past-step address for later */
    if (loop_id >= MAX_LOOPS) {
      goto step_done;
    }

    uint32_t prior_count = vm->counters[loop_id];
    size_t prior_last_pos = vm->loop_last_pos[loop_id];

    /* ---- Greedy-span optimisation (L3 / D3) --------------------------------
       * For an unbounded star over a pure OP_SPAN body (no captures / side
       * effects), avoid pushing one choice per consumed byte.  Instead scan
       * forward once to the end of the maximal run, push a single bound choice
       * at the post-run position, and on backtrack let the step instruction re-
       * execute to try one byte shorter — all without re-entering the body.
       * ----------------------------------------------------------------------- */
    if (vm->loop_max[loop_id] == (uint32_t)-1 && vm->bc[target] == OP_SPAN &&
        !vm->loop_span_greedy[loop_id]) {
      /* Detect on first entry and remember for this loop's lifetime. */
      vm->loop_span_greedy[loop_id] = true;
    }

    if (vm->loop_span_greedy[loop_id]) {
      /* The body (SPAN) just consumed one class byte and pos advanced by 1.
         * Normally we would push a choice and loop back; instead commit to the
         * maximal run.  On backtrack the choice below (ip = step instruction)
         * re-executes this handler, which pushes a new choice one byte shorter,
         * and so on — O(k) choices for k backtrack steps vs O(n) per byte. */
      uint16_t sc;
      (void)sc;
      size_t read_ip = target + 1;
      uint16_t sset = read_u16(vm->bc, vm->bc_len, &read_ip);
      uint16_t scnt;
      uint16_t sci;
      const uint8_t *rng = get_ranges_ptr(vm, sset, &scnt, &sci);
      if (rng && scnt > 0) {
        /* Build the 256-bit class bitmap once and find the run end. */
        uint64_t bmap[4];
        if (ranges_to_full_bitmap(rng, scnt, bmap)) {
          size_t run_end = (size_t)vm->pos;
          while (run_end < vm->len) {
            uint8_t c = (uint8_t)vm->s[run_end];
            unsigned w = (unsigned)c >> 6;
            if (bmap[w] & (1ULL << ((unsigned)c & 63U))) {
              run_end++;
            } else {
              break;
            }
          }
          size_t extra = run_end - (size_t)vm->pos;
          /* The body consumed 1 byte (advancing pos).  Increment the counter
             * by the full span length (1 body + extra) so the trail can undo
             * the entire greedy commit with a single UNDO_COUNTER_DEC. */
          vm->pos += (int32_t)extra;
          vm->counters[loop_id] += 1U + (uint32_t)extra;
          if (vm->use_compact_choice) {
            vm_trail_counter_inc(vm, loop_id, prior_count, prior_last_pos);
          }
        }
      }

      /* Push a single bound choice: ip points back to this step instruction so
         * popping retries the loop with one byte less from the span run. */
      if ((size_t)vm->pos > vm->loop_last_pos[loop_id]) {
        vm_push_choice(vm, step_ip - 6, vm->pos - 1);
      }
      vm->loop_last_pos[loop_id] = vm->pos;
      goto step_done;
    }

    /* Normal per-iteration loop body (original behaviour, L3
       * optimisation is a no-op for captured / side-effect bodies). */
    vm->counters[loop_id]++;
    {
      uint32_t count = vm->counters[loop_id];
      if (vm->use_compact_choice) {
        vm_trail_counter_inc(vm, loop_id, prior_count, prior_last_pos);
      }
      if (count < vm->loop_min[loop_id]) {
        vm->loop_last_pos[loop_id] = vm->pos;
        vm->ip = (size_t)target;
      } else if (vm->loop_max[loop_id] == (uint32_t)-1 ||
                 count < vm->loop_max[loop_id]) {
        if (vm->loop_max[loop_id] == (uint32_t)-1 &&
            vm->pos == vm->loop_last_pos[loop_id]) {
          /* Zero-progress: exit immediately (no choice push).  Checked
           * before the iteration-cap guard so empty-body loops like (''*)
           * exit in O(1) instead of O(subject_len). */
        } else if (vm->loop_max[loop_id] == (uint32_t)-1 &&
                   count > vm->len + 1) {
          /* fall through to the stop path (do not push) */
        } else {
          vm_push_choice(vm, vm->ip, vm->pos);
          vm->loop_last_pos[loop_id] = vm->pos;
          vm->ip = (size_t)target;
        }
      }
    }
  step_done:
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_emit_literal:
#endif
#ifdef _MSC_VER
  case OP_EMIT_LITERAL:
#endif
  {
    uint32_t off = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t len = read_u32(vm->bc, vm->bc_len, &vm->ip);
    if (off == vm->ip) {
      vm->ip += len;
    }
    if (vm->out) {
      snobol_buf_append(vm->out, (const char *)vm->bc + off, (size_t)len);
    }
    if (vm->emit_fn) {
      vm->emit_fn((const char *)vm->bc + off, (size_t)len, vm->emit_udata);
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_emit_capture:
#endif
#ifdef _MSC_VER
  case OP_EMIT_CAPTURE:
#endif
  {
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    if (r < MAX_CAPS && vm->cap_end[r] >= vm->cap_start[r] &&
        vm->cap_end[r] <= vm->len) {
      if (vm->out) {
        snobol_buf_append(vm->out, vm->s + vm->cap_start[r],
                          vm->cap_end[r] - vm->cap_start[r]);
      }
      if (vm->emit_fn) {
        vm->emit_fn(vm->s + vm->cap_start[r], vm->cap_end[r] - vm->cap_start[r],
                    vm->emit_udata);
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_emit_expr:
#endif
#ifdef _MSC_VER
  case OP_EMIT_EXPR:
#endif
  {
    /* LEGACY alias: old discriminants 1=upper, 2=length.
       * Map to SNBL_FMT_* and fall through to the same logic
       * as OP_EMIT_FORMAT. */
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t expr_type = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t mapped_type;
    if (expr_type == 1) {
      mapped_type = SNBL_FMT_UPPER;
    } else if (expr_type == 2) {
      mapped_type = SNBL_FMT_LENGTH;
    } else {
      mapped_type = 0; /* unknown → emit raw */
    }

    if (r < MAX_CAPS && vm->cap_end[r] >= vm->cap_start[r] &&
        vm->cap_end[r] <= vm->len) {
      const char *data = vm->s + vm->cap_start[r];
      size_t len = vm->cap_end[r] - vm->cap_start[r];
      if (mapped_type == SNBL_FMT_UPPER) {
        char *tmp = snobol_malloc(len + 1);
        for (size_t i = 0; i < len; ++i) {
          tmp[i] = (data[i] >= 'a' && data[i] <= 'z') ? data[i] - 32 : data[i];
        }
        if (vm->out) {
          snobol_buf_append(vm->out, tmp, len);
        }
        if (vm->emit_fn) {
          vm->emit_fn(tmp, len, vm->emit_udata);
        }
        snobol_free(tmp);
      } else if (mapped_type == SNBL_FMT_LENGTH) {
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%zu", len);
        if (vm->out) {
          snobol_buf_append(vm->out, tmp, (size_t)n);
        }
        if (vm->emit_fn) {
          vm->emit_fn(tmp, (size_t)n, vm->emit_udata);
        }
      } else {
        if (vm->out) {
          snobol_buf_append(vm->out, data, len);
        }
        if (vm->emit_fn) {
          vm->emit_fn(data, len, vm->emit_udata);
        }
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

  /* Table-backed and formatted replacement opcodes */
#ifndef _MSC_VER
  op_emit_table:
#endif
#ifdef _MSC_VER
  case OP_EMIT_TABLE:
#endif
  {
    /* Lookup table[key] and emit the value
       * Format: table_id u16 (SNBL_TABLE_ID_UNBOUND=unbound),
       *         key_type u8,
       *         name_len u8, name_bytes[name_len]  ← skip at runtime
       *   then key payload:
       *   - key_type=0 (literal): key_len u16, key_bytes[key_len]
       *   - key_type=1 (capture): key_reg u8
       * If key not found, emit empty string (graceful degradation) */
    uint16_t table_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t key_type = read_u8(vm->bc, vm->bc_len, &vm->ip);
    /* skip embedded table name (resolved at bind time) */
    uint8_t name_len = read_u8(vm->bc, vm->bc_len, &vm->ip);
    vm->ip += name_len;

#ifdef SNOBOL_DYNAMIC_PATTERN
    snobol_table_t *table = vm_get_table(vm, table_id);
    const char *value = nullptr;

    if (key_type == 0) {
      /* Literal key: read key_len and key_bytes from bytecode */
      uint16_t key_len = read_u16(vm->bc, vm->bc_len, &vm->ip);
      if (key_len > 0 && key_len < 1024) {
        char *key = (char *)snobol_malloc(key_len + 1);
        if (key) {
          memcpy(key, vm->bc + vm->ip, key_len);
          key[key_len] = '\0';
          vm->ip += key_len;

          /* Lookup and emit */
          if (table) {
            value = table_get(table, key);
          }
          snobol_free(key);
        }
      }
    } else if (key_type == 1) {
      /* Capture-derived key: read key_reg */
      uint8_t key_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);

      if (table && key_reg < MAX_CAPS &&
          vm->cap_end[key_reg] >= vm->cap_start[key_reg] &&
          vm->cap_end[key_reg] <= vm->len) {

        /* Extract key */
        size_t key_len = vm->cap_end[key_reg] - vm->cap_start[key_reg];
        char *key = (char *)snobol_malloc(key_len + 1);
        if (key) {
          memcpy(key, vm->s + vm->cap_start[key_reg], key_len);
          key[key_len] = '\0';

          /* Lookup */
          if (table) {
            value = table_get(table, key);
          }
          snobol_free(key);
        }
      }
    }

    /* Emit the value (or nothing if not found - graceful degradation) */
    if (value) {
      size_t val_len = strlen(value);
      if (vm->out) {
        snobol_buf_append(vm->out, value, val_len);
      }
      if (vm->emit_fn) {
        vm->emit_fn(value, val_len, vm->emit_udata);
      }
    }
#else
        (void)table_id;
        (void)key_type;
        (void)name_len; /* Table support not enabled */
        /* Still need to advance ip past key payload to avoid misalignment */
        if (key_type == 0) {
          uint16_t skip_len = read_u16(vm->bc, vm->bc_len, &vm->ip);
          vm->ip += skip_len;
        } else if (key_type == 1) {
          vm->ip += 1;
        }
#endif
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_emit_format:
#endif
#ifdef _MSC_VER
  case OP_EMIT_FORMAT:
#endif
  {
    /* Format capture with specified format type
       * Format: reg u8, format_type u8 (SNBL_FMT_*)
       * SNBL_FMT_LPAD / SNBL_FMT_RPAD also read: width u16, fill_char u8 */
    uint8_t r = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t format_type = read_u8(vm->bc, vm->bc_len, &vm->ip);

    if (r < MAX_CAPS && vm->cap_end[r] >= vm->cap_start[r] &&
        vm->cap_end[r] <= vm->len) {
      const char *data = vm->s + vm->cap_start[r];
      size_t len = vm->cap_end[r] - vm->cap_start[r];

      if (format_type == SNBL_FMT_UPPER) {
        /* Uppercase */
        char *tmp = (char *)snobol_malloc(len + 1);
        for (size_t i = 0; i < len; ++i) {
          tmp[i] = (data[i] >= 'a' && data[i] <= 'z') ? data[i] - 32 : data[i];
        }
        tmp[len] = '\0';
        if (vm->out) {
          snobol_buf_append(vm->out, tmp, len);
        }
        if (vm->emit_fn) {
          vm->emit_fn(tmp, len, vm->emit_udata);
        }
        snobol_free(tmp);
      } else if (format_type == SNBL_FMT_LOWER) {
        /* Lowercase */
        char *tmp = (char *)snobol_malloc(len + 1);
        for (size_t i = 0; i < len; ++i) {
          tmp[i] = (data[i] >= 'A' && data[i] <= 'Z') ? data[i] + 32 : data[i];
        }
        tmp[len] = '\0';
        if (vm->out) {
          snobol_buf_append(vm->out, tmp, len);
        }
        if (vm->emit_fn) {
          vm->emit_fn(tmp, len, vm->emit_udata);
        }
        snobol_free(tmp);
      } else if (format_type == SNBL_FMT_LENGTH) {
        /* Length as string */
        char tmp[32];
        int n = snprintf(tmp, sizeof(tmp), "%zu", len);
        if (vm->out) {
          snobol_buf_append(vm->out, tmp, (size_t)n);
        }
        if (vm->emit_fn) {
          vm->emit_fn(tmp, (size_t)n, vm->emit_udata);
        }
      } else if (format_type == SNBL_FMT_LPAD || format_type == SNBL_FMT_RPAD) {
        /* Padding: read width (u16, big-endian) and fill_char (u8) */
        uint16_t width = read_u16(vm->bc, vm->bc_len, &vm->ip);
        uint8_t fill = read_u8(vm->bc, vm->bc_len, &vm->ip);
        if (width > 1024) {
          width = 1024; /* cap */
        }
        if (len >= width) {
          /* No padding needed - emit as-is */
          if (vm->out) {
            snobol_buf_append(vm->out, data, len);
          }
          if (vm->emit_fn) {
            vm->emit_fn(data, len, vm->emit_udata);
          }
        } else {
          size_t pad = width - len;
          size_t total = (size_t)width;
          char *buf = (char *)snobol_malloc(total);
          if (buf) {
            if (format_type == SNBL_FMT_LPAD) {
              memset(buf, fill, pad);
              memcpy(buf + pad, data, len);
            } else {
              memcpy(buf, data, len);
              memset(buf + len, fill, pad);
            }
            if (vm->out) {
              snobol_buf_append(vm->out, buf, total);
            }
            if (vm->emit_fn) {
              vm->emit_fn(buf, total, vm->emit_udata);
            }
            snobol_free(buf);
          }
        }
      } else {
        /* Unknown format - emit raw */
        if (vm->out) {
          snobol_buf_append(vm->out, data, len);
        }
        if (vm->emit_fn) {
          vm->emit_fn(data, len, vm->emit_udata);
        }
      }
    } else {
      /* Missing capture or LPAD/RPAD: read and discard extra operands */
      if (format_type == SNBL_FMT_LPAD || format_type == SNBL_FMT_RPAD) {
        read_u16(vm->bc, vm->bc_len, &vm->ip); /* width */
        read_u8(vm->bc, vm->bc_len, &vm->ip);  /* fill */
      }
      /* emit empty (graceful degradation) */
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

  /* Control flow opcodes */
#ifndef _MSC_VER
  op_label:
#endif
#ifdef _MSC_VER
  case OP_LABEL:
#endif
  {
    /* Define a label target - just skip during execution
       * Labels are resolved at compile time, OP_LABEL is a no-op at runtime */
    uint16_t label_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    (void)label_id;
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_goto:
#endif
#ifdef _MSC_VER
  case OP_GOTO:
#endif
  {
    /* Unconditional transfer to label
       * CRITICAL: GOTO does NOT restore backtracking state
       * This distinguishes explicit control flow from ordinary backtracking */
    uint16_t label_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint32_t target = vm_get_label_offset(vm, label_id);

    if (target == UINT32_MAX) {
      /* Invalid label - fail without restoring backtracking state */
      vm->in_goto_fail = true;
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      /* Transfer control to label target
         * Note: We do NOT pop any choice here - GOTO is explicit flow,
         * not backtracking. The choice stack remains for future backtracking.
         */
      vm->ip = target;
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }
#ifndef _MSC_VER
  op_goto_f:
#endif
#ifdef _MSC_VER
  case OP_GOTO_F:
#endif
  {
    /* Transfer to label if last match failed
       * This is used for conditional control flow after a match attempt */
    uint16_t label_id = read_u16(vm->bc, vm->bc_len, &vm->ip);

    if (vm->in_goto_fail) {
      uint32_t target = vm_get_label_offset(vm, label_id);
      if (target == UINT32_MAX) {
        if (!vm_pop_choice(vm)) {
          goto fail_ret;
        }
      } else {
        vm->ip = target;
        vm->in_goto_fail = false;
      }
    } else {
      /* Continue normally - no failure occurred */
#ifdef _MSC_VER
      break;
#else
          continue;
#endif
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Table operations */
#ifndef _MSC_VER
  op_table_get:
#endif
#ifdef _MSC_VER
  case OP_TABLE_GET:
#endif
  {
    /* Lookup table[key] and store result in dest register
       * Format: table_id u16, key_reg u8, dest_reg u8
       * If key not found, fail (trigger backtracking) */
    uint16_t table_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t key_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t dest_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t name_len = read_u8(vm->bc, vm->bc_len, &vm->ip);
    vm->ip += name_len;

    snobol_table_t *table = vm_get_table(vm, table_id);
    if (!table) {
      /* Invalid table - fail */
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    /* Get key from capture register */
    if (key_reg >= MAX_CAPS || vm->cap_end[key_reg] <= vm->cap_start[key_reg]) {
      /* Invalid key register - fail */
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    /* Extract key string */
    size_t key_start = vm->cap_start[key_reg];
    size_t key_end = vm->cap_end[key_reg];
    size_t key_len = key_end - key_start;

    /* Allocate and copy key */
    char *key = (char *)snobol_malloc(key_len + 1);
    if (!key) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(key, vm->s + key_start, key_len);
    key[key_len] = '\0';

    /* Lookup in table */
    const char *value = table_get(table, key);
    snobol_free(key);

    if (!value) {
      /* Key not found - fail (trigger backtracking) */
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    /* Store result: update dest_reg to match the value
       * For now, we just note the lookup succeeded
       * Full implementation would store value in a temp buffer */
    (void)dest_reg; /* Result available via table */
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
#ifndef _MSC_VER
  op_table_set:
#endif
#ifdef _MSC_VER
  case OP_TABLE_SET:
#endif
  {
    /* Set table[key] = value
       * Format: table_id u16, key_reg u8, value_reg u8
       * Always succeeds (table operations don't fail) */
    uint16_t table_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t key_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t value_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t name_len = read_u8(vm->bc, vm->bc_len, &vm->ip);
    vm->ip += name_len;

    snobol_table_t *table = vm_get_table(vm, table_id);
    if (!table) {
      /* Invalid table - fail */
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    /* Get key from capture register */
    if (key_reg >= MAX_CAPS || vm->cap_end[key_reg] <= vm->cap_start[key_reg]) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    /* Get value from capture register */
    if (value_reg >= MAX_CAPS ||
        vm->cap_end[value_reg] <= vm->cap_start[value_reg]) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    size_t key_start = vm->cap_start[key_reg];
    size_t key_end = vm->cap_end[key_reg];
    size_t key_len = key_end - key_start;

    size_t val_start = vm->cap_start[value_reg];
    size_t val_end = vm->cap_end[value_reg];
    size_t val_len = val_end - val_start;

    /* Allocate and copy key */
    char *key = (char *)snobol_malloc(key_len + 1);
    if (!key) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(key, vm->s + key_start, key_len);
    key[key_len] = '\0';

    /* Allocate and copy value */
    char *value = (char *)snobol_malloc(val_len + 1);
    if (!value) {
      snobol_free(key);
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(value, vm->s + val_start, val_len);
    value[val_len] = '\0';

    /* Set in table */
    if (!table_set(table, key, value)) {
      snobol_free(key);
      snobol_free(value);
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    snobol_free(key);
    snobol_free(value);
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
  /* Array operations */
#ifndef _MSC_VER
  op_array_get:
#endif
#ifdef _MSC_VER
  case OP_ARRAY_GET:
#endif
  {
    uint16_t array_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t key_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t dest_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t name_len = read_u8(vm->bc, vm->bc_len, &vm->ip);
    vm->ip += name_len;

    snobol_array_t *array = vm_get_array(vm, array_id);
    if (!array) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    if (key_reg >= MAX_CAPS || vm->cap_end[key_reg] <= vm->cap_start[key_reg]) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    size_t key_start = vm->cap_start[key_reg];
    size_t key_end = vm->cap_end[key_reg];
    size_t key_len = key_end - key_start;

    char *key_str = (char *)snobol_malloc(key_len + 1);
    if (!key_str) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(key_str, vm->s + key_start, key_len);
    key_str[key_len] = '\0';

    int32_t key = (int32_t)strtol(key_str, nullptr, 10);
    snobol_free(key_str);

    const char *value = snobol_array_get(array, key);
    if (!value) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    (void)dest_reg;
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
#ifndef _MSC_VER
  op_array_set:
#endif
#ifdef _MSC_VER
  case OP_ARRAY_SET:
#endif
  {
    uint16_t array_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint8_t key_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t value_reg = read_u8(vm->bc, vm->bc_len, &vm->ip);
    uint8_t name_len = read_u8(vm->bc, vm->bc_len, &vm->ip);
    vm->ip += name_len;

    snobol_array_t *array = vm_get_array(vm, array_id);
    if (!array) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    if (key_reg >= MAX_CAPS || vm->cap_end[key_reg] <= vm->cap_start[key_reg]) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    if (value_reg >= MAX_CAPS ||
        vm->cap_end[value_reg] <= vm->cap_start[value_reg]) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    size_t key_start = vm->cap_start[key_reg];
    size_t key_end = vm->cap_end[key_reg];
    size_t key_len = key_end - key_start;

    size_t val_start = vm->cap_start[value_reg];
    size_t val_end = vm->cap_end[value_reg];
    size_t val_len = val_end - val_start;

    char *key_str = (char *)snobol_malloc(key_len + 1);
    if (!key_str) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(key_str, vm->s + key_start, key_len);
    key_str[key_len] = '\0';

    char *value_str = (char *)snobol_malloc(val_len + 1);
    if (!value_str) {
      snobol_free(key_str);
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }
    memcpy(value_str, vm->s + val_start, val_len);
    value_str[val_len] = '\0';

    int32_t key = (int32_t)strtol(key_str, nullptr, 10);
    snobol_free(key_str);

    if (!snobol_array_set(array, key, value_str)) {
      snobol_free(value_str);
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    snobol_free(value_str);
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
#ifndef _MSC_VER
  op_dynamic_def:
#endif
#ifdef _MSC_VER
  case OP_DYNAMIC_DEF:
#endif
  {
    /* Define dynamic pattern source and bytecode for runtime caching
       * Format: source_len(u32) + source_text + bc_len(u32) + bytecode
       *
       * Canonical approach: Store both source text and compiled bytecode.
       * - Source text: Used as cache key for reuse across repeated EVAL(...)
       * - Bytecode: Used for efficient execution in the VM
       */
    uint32_t source_len = read_u32(vm->bc, vm->bc_len, &vm->ip);

    /* Copy the source text for cache keying */
    char *source_copy = (char *)snobol_malloc(source_len + 1);
    if (source_copy) {
      memcpy(source_copy, vm->bc + vm->ip, source_len);
      source_copy[source_len] = '\0';
    }
    vm->ip += source_len;

    /* Copy the bytecode for execution */
    uint32_t bc_len = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint8_t *bc_copy = (uint8_t *)snobol_malloc(bc_len);
    if (bc_copy) {
      memcpy(bc_copy, vm->bc + vm->ip, bc_len);
    }
    vm->ip += bc_len;

    /* Store for OP_DYNAMIC to use */
#ifdef SNOBOL_DYNAMIC_PATTERN
    /* Free any previously pending data */
    if (vm->dyn_pending_source) {
      snobol_free(vm->dyn_pending_source);
    }
    if (vm->dyn_pending_bc) {
      snobol_free(vm->dyn_pending_bc);
    }

    vm->dyn_pending_source = source_copy;
    vm->dyn_pending_source_len = source_len;
    vm->dyn_pending_bc = bc_copy;
    vm->dyn_pending_bc_len = bc_len;
#else
      if (source_copy)
        snobol_free(source_copy);
      if (bc_copy)
        snobol_free(bc_copy);
#endif
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
#ifndef _MSC_VER
  op_dynamic:
#endif
#ifdef _MSC_VER
  case OP_DYNAMIC:
#endif
  {
    /* Evaluate dynamic pattern using cached compilation
       * Format: (no additional operands - uses data from OP_DYNAMIC_DEF)
       *
       * Canonical implementation:
       * 1. Look up pattern in dynamic pattern cache by source text
       * 2. On hit: use cached pattern (retains reference)
       * 3. On miss: create pattern from bytecode, cache it
       * 4. Execute the pattern with full VM semantics
       * 5. Handle success/failure with proper backtracking
       *
       * This enables:
       * - Cache reuse across repeated EVAL(...) with same source
       * - Full pattern semantics: alternation, backtracking, nested EVAL(...)
       * - Proper retain/release ownership through runtime cache
       */

#ifdef SNOBOL_DYNAMIC_PATTERN
    /* Verify we have both source and bytecode */
    if (!vm->dyn_pending_source || !vm->dyn_pending_bc) {
      SNOBOL_LOG("OP_DYNAMIC: missing source or bytecode");
      goto fail_ret;
    }

    SNOBOL_LOG("OP_DYNAMIC: looking up source '%.*s' (len=%zu) in cache",
               (int)vm->dyn_pending_source_len, vm->dyn_pending_source,
               vm->dyn_pending_source_len);

    /* Look up in dynamic pattern cache by source text */
    dynamic_pattern_t *pattern = dynamic_pattern_cache_get(
        vm->dyn_cache, vm->dyn_pending_source, (int)vm->dyn_pending_source_len);

    if (!pattern) {
      /* Cache miss - create pattern from pre-compiled bytecode */
      SNOBOL_LOG("OP_DYNAMIC: cache miss, creating pattern from bytecode "
                 "(bc_len=%zu)",
                 vm->dyn_pending_bc_len);

      /* Copy bytecode for the pattern object (pattern owns its copy) */
      uint8_t *bc_copy = (uint8_t *)snobol_malloc(vm->dyn_pending_bc_len);
      if (!bc_copy) {
        SNOBOL_LOG("OP_DYNAMIC: failed to allocate bytecode copy");
        goto fail_ret;
      }
      memcpy(bc_copy, vm->dyn_pending_bc, vm->dyn_pending_bc_len);

      /* Create pattern object with the bytecode */
      pattern = dynamic_pattern_create(vm->dyn_pending_source, bc_copy,
                                       vm->dyn_pending_bc_len);
      if (!pattern || !pattern->is_valid) {
        SNOBOL_LOG("OP_DYNAMIC: failed to create pattern");
        if (pattern) {
          dynamic_pattern_release(pattern);
        }
        if (bc_copy) {
          snobol_free(bc_copy);
        }
        goto fail_ret;
      }

      /* Cache the compiled pattern */
      if (!dynamic_pattern_cache_put(vm->dyn_cache, vm->dyn_pending_source,
                                     pattern)) {
        SNOBOL_LOG("OP_DYNAMIC: failed to cache pattern (continuing anyway)");
      }

      SNOBOL_LOG("OP_DYNAMIC: created and cached pattern");
    } else {
      SNOBOL_LOG("OP_DYNAMIC: cache hit");
    }

    /* The pattern has been built and cached; the pending buffers are
       * no longer needed.  Free them to avoid leaking across
       * vm_exec calls. */
    if (vm->dyn_pending_source) {
      snobol_free(vm->dyn_pending_source);
      vm->dyn_pending_source = nullptr;
      vm->dyn_pending_source_len = 0;
    }
    if (vm->dyn_pending_bc) {
      snobol_free(vm->dyn_pending_bc);
      vm->dyn_pending_bc = nullptr;
      vm->dyn_pending_bc_len = 0;
    }

    /* Execute the dynamic pattern with full VM semantics
       * Save/restore IP and position for proper backtracking */
    size_t saved_ip = vm->ip;
    size_t saved_pos = vm->pos;
    size_t saved_cap_start[MAX_CAPS];
    size_t saved_cap_end[MAX_CAPS];
    memcpy(saved_cap_start, vm->cap_start, sizeof(saved_cap_start));
    memcpy(saved_cap_end, vm->cap_end, sizeof(saved_cap_end));

    /* Save/restore the choice stack and write-log: vm_run() frees
       * vm->choices_arena and vm->write_log on both success and failure
       * paths, so the recursive call would destroy the outer call's
       * backtracking state, causing heap corruption on Windows. */
    ChoiceArena *saved_arena = vm->choices_arena;
    size_t saved_choices_top = vm->choices_top;
    bool saved_compact = vm->use_compact_choice;
    WriteLogEntry *saved_wlog = vm->write_log;
    size_t saved_wlog_cap = vm->write_log_cap;
    size_t saved_wlog_next = vm->write_log_next;
    uint64_t saved_wlog_bitmap = vm->write_log_bitmap;
    size_t saved_wlog_cc = vm->write_log_compressed_count;
    bool saved_wlog_dirty = vm->write_log_dirty;
    /* The undo trail has the same lifecycle: vm_run() frees it on exit, so
     * detach the outer trail before the inner run (it allocates its own) and
     * restore it afterwards, preserving capture undo across the boundary. */
    UndoRecord *saved_trail = vm->trail;
    size_t saved_trail_cap = vm->trail_cap;
    size_t saved_trail_top = vm->trail_top;
    /* Null out so vm_run() allocates fresh state for the inner run */
    vm->choices_arena = nullptr;
    vm->choices_top = 0;
    vm->write_log = nullptr;
    vm->write_log_cap = 0;
    vm->write_log_next = 0;
    vm->write_log_bitmap = 0;
    vm->write_log_compressed_count = 0;
    vm->write_log_dirty = false;
    vm->trail = nullptr;
    vm->trail_cap = 0;
    vm->trail_top = 0;

    /* Execute dynamic pattern bytecode through the VM */
    const uint8_t *saved_bc = vm->bc;
    size_t saved_bc_len = vm->bc_len;
    vm->bc = pattern->bc;
    vm->bc_len = pattern->bc_len;
    vm->ip = 0;
    vm->pos = saved_pos;

    /* Run the dynamic pattern bytecode through vm_run for full semantics
       * This enables alternation, backtracking, nested EVAL(...), etc. */
    int dynamic_result = (int)vm_run(vm);

    /* Restore VM state */
    vm->bc = saved_bc;
    vm->bc_len = saved_bc_len;
    vm->ip = saved_ip;
    /* Restore choice stack and write-log that vm_run() freed */
    vm->choices_arena = saved_arena;
    vm->choices_top = saved_choices_top;
    vm->use_compact_choice = saved_compact;
    vm->write_log = saved_wlog;
    vm->write_log_cap = saved_wlog_cap;
    vm->write_log_next = saved_wlog_next;
    vm->write_log_bitmap = saved_wlog_bitmap;
    vm->write_log_compressed_count = saved_wlog_cc;
    vm->write_log_dirty = saved_wlog_dirty;
    /* Restore the outer undo trail (the inner run freed its own) */
    vm->trail = saved_trail;
    vm->trail_cap = saved_trail_cap;
    vm->trail_top = saved_trail_top;

    if (!dynamic_result) {
      /* Dynamic pattern failed - restore captures and position */
      SNOBOL_LOG("OP_DYNAMIC: pattern failed, restoring state");
      vm->pos = saved_pos;
      memcpy(vm->cap_start, saved_cap_start, sizeof(saved_cap_start));
      memcpy(vm->cap_end, saved_cap_end, sizeof(saved_cap_end));
      dynamic_pattern_release(pattern);
      goto fail_ret;
    }

    /* Success - pattern matched */
    SNOBOL_LOG("OP_DYNAMIC: pattern matched, pos=%zu", vm->pos);

    /* Release our reference to the pattern (cache retains its own) */
    dynamic_pattern_release(pattern);
#else
      /* Dynamic pattern support not enabled */
      SNOBOL_LOG("OP_DYNAMIC: support not enabled");
#endif
#ifdef _MSC_VER
    break;
#else
      continue;
#endif
  }
#endif /* SNOBOL_DYNAMIC_PATTERN */

  /* ------------------------------------------------------------------
       * Pattern primitives
       * ------------------------------------------------------------------ */

#ifndef _MSC_VER
  op_breakx:
#endif
#ifdef _MSC_VER
  case OP_BREAKX:
#endif
  {
    /* BREAKX pre-scan optimization.
       *
       * Semantics: like BREAK (advance past non-break chars), but also
       * push a retry choice point.  When the choice is popped, pos
       * advances past the break char so matching resumes after it.
       * This is equivalent to BREAK + ARBNO(LEN(1) BREAK) but as a
       * single opcode with O(n) complexity (the pre-scan visits each
       * subject byte at most twice).
       */
    size_t breakx_ip = vm->ip - 1; /* points to OP_BREAKX opcode */
    uint16_t set_id = read_u16(vm->bc, vm->bc_len, &vm->ip);
    uint16_t bx_count;
    uint16_t bx_ci;
    const uint8_t *bx_ranges = get_ranges_ptr(vm, set_id, &bx_count, &bx_ci);
    uint64_t bx_map[2];
    bool bx_ascii =
        (bx_ranges && ranges_to_ascii_bitmap(bx_ranges, bx_count, bx_map)) != 0;

    /* Advance past non-break characters (like OP_BREAK) */
    if (bx_ascii) {
      int bx_single = bitmap_single_ascii_byte(bx_map);
      if (bx_single >= 0 && vm->pos < vm->len) {
        const void *bx_p = memchr(vm->s + vm->pos, (unsigned char)bx_single,
                                  vm->len - vm->pos);
        if (bx_p) {
          vm->pos = (size_t)((const uint8_t *)bx_p - (const uint8_t *)vm->s);
        } else {
          vm->pos = vm->len;
        }
      } else {
        while (vm->pos < vm->len &&
               !bitmap_test(bx_map, (uint8_t)vm->s[vm->pos])) {
          vm->pos++;
        }
      }
    } else {
      uint32_t bx_cp;
      int bx_bytes;
      while (utf8_peek_next(vm->s, vm->len, vm->pos, &bx_cp, &bx_bytes) &&
             (!bx_ranges || !range_contains(bx_ranges, bx_count, bx_cp))) {
        vm->pos += bx_bytes;
      }
    }

    /* If we stopped at a break char, push a retry choice that
       * re-executes BREAKX from the position AFTER the break char */
    if (vm->pos < vm->len) {
      uint32_t bx_cp2;
      int bx_skip = 1;
      if (utf8_peek_next(vm->s, vm->len, vm->pos, &bx_cp2, &bx_skip)) {
        ; /* bx_skip now holds byte count of break char */
      }
      vm_push_choice(vm, breakx_ip, vm->pos + (size_t)bx_skip);
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_bal:
#endif
#ifdef _MSC_VER
  case OP_BAL:
#endif
  {
    /* BAL: match balanced structure.
       * Operands: open_cp u32, close_cp u32
       * Fails if the subject at vm->pos does not start with open_cp,
       * or if the string ends before depth returns to zero.
       */
    uint32_t bal_open = read_u32(vm->bc, vm->bc_len, &vm->ip);
    uint32_t bal_close = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t bal_pos = vm->pos;
    int bal_depth = 0;
    bool bal_ok = false;

    /* Subject must start with the open delimiter */
    uint32_t bal_first;
    int bal_fb;
    if (!utf8_peek_next(vm->s, vm->len, bal_pos, &bal_first, &bal_fb) ||
        bal_first != bal_open) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
      break;
    }

    while (bal_pos < vm->len) {
      uint32_t bal_cp;
      int bal_cb;
      if (!utf8_peek_next(vm->s, vm->len, bal_pos, &bal_cp, &bal_cb)) {
        break;
      }
      if (bal_cp == bal_open) {
        bal_depth++;
      } else if (bal_cp == bal_close) {
        bal_depth--;
        if (bal_depth == 0) {
          bal_pos += (size_t)bal_cb;
          bal_ok = true;
          break;
        }
      }
      bal_pos += (size_t)bal_cb;
    }

    if (!bal_ok) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      { vm->pos = bal_pos; }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_fence:
#endif
#ifdef _MSC_VER
  case OP_FENCE:
#endif
  {
    /* FENCE: cut the choice stack.
       * Drops all choice points pushed since execution started,
       * preventing backtracking past this point.
       * This implements "possessive" / atomic behaviour.
       */
    if (vm->choices_arena) {
      vm_arena_reset(vm->choices_arena);
    }
    vm->choices_top = 0;
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_rem:
#endif
#ifdef _MSC_VER
  case OP_REM:
#endif
  {
    /* REM: match remainder of subject.
       * Advances pos to end of string unconditionally.
       */
    vm->pos = vm->len;
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_rpos:
#endif
#ifdef _MSC_VER
  case OP_RPOS:
#endif
  {
    /* REM: match remainder of subject.
       * codepoints from the end of the subject string.
       *
       * Operand: n u32
       * Algorithm: walk [n] codepoints backwards from vm->len to
       * determine the target byte offset; fail if vm->pos != target.
       */
    uint32_t rpos_n = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t rpos_target = vm->len;
    for (uint32_t rpos_i = 0; rpos_i < rpos_n && rpos_target > 0; rpos_i++) {
      rpos_target--;
      /* Skip past UTF-8 continuation bytes to find codepoint start */
      while (rpos_target > 0 &&
             ((unsigned char)vm->s[rpos_target] & 0xC0) == 0x80) {
        rpos_target--;
      }
    }
    if (vm->pos != rpos_target) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_rtab:
#endif
#ifdef _MSC_VER
  case OP_RTAB:
#endif
  {
    /* RTAB(n): advance cursor to the position that is
       * n codepoints from the end of the subject string.
       * RTAB(0) is equivalent to REM (advance to end).
       *
       * Operand: n u32
       * Fails if vm->pos is already past the target position.
       */
    uint32_t rtab_n = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t rtab_target = vm->len;
    for (uint32_t rtab_i = 0; rtab_i < rtab_n && rtab_target > 0; rtab_i++) {
      rtab_target--;
      while (rtab_target > 0 &&
             ((unsigned char)vm->s[rtab_target] & 0xC0) == 0x80) {
        rtab_target--;
      }
    }
    if (vm->pos > rtab_target) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      { vm->pos = rtab_target; }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_pos:
#endif
#ifdef _MSC_VER
  case OP_POS:
#endif
  {
    /* POS(n): succeed only when the current match position
       * is exactly n codepoints from the start of the subject.
       *
       * Operand: n u32
       * Calculates the byte offset of the nth codepoint from
       * the start (by walking forward n codepoints) and fails
       * if vm->pos != that offset.
       */
    uint32_t pos_n = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t pos_target = 0;
    for (uint32_t pos_i = 0; pos_i < pos_n && pos_target < vm->len; pos_i++) {
      pos_target++;
      while (pos_target < vm->len &&
             ((unsigned char)vm->s[pos_target] & 0xC0) == 0x80) {
        pos_target++;
      }
    }
    if (vm->pos != pos_target) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_tab:
#endif
#ifdef _MSC_VER
  case OP_TAB:
#endif
  {
    /* TAB(n): advance cursor to n codepoints from the
       * start of the subject string.
       *
       * Operand: n u32
       * Fails if vm->pos is already past the target or if
       * n is beyond the string length.
       */
    uint32_t tab_n = read_u32(vm->bc, vm->bc_len, &vm->ip);
    size_t tab_target = 0;
    for (uint32_t tab_i = 0; tab_i < tab_n && tab_target < vm->len; tab_i++) {
      tab_target++;
      while (tab_target < vm->len &&
             ((unsigned char)vm->s[tab_target] & 0xC0) == 0x80) {
        tab_target++;
      }
    }
    if (tab_target >= vm->len && tab_n > 0) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else if (vm->pos > tab_target) {
      if (!vm_pop_choice(vm)) {
        goto fail_ret;
      }
    } else {
      vm->pos = tab_target;
    }
#ifdef _MSC_VER
    break;
#else
        continue;
#endif
  }

#ifndef _MSC_VER
  op_abort:
#endif
#ifdef _MSC_VER
  case OP_ABORT:
#endif
  {
    /* ABORT: immediately terminate the entire match.
       * Sets a VM abort flag and unwinds all choice points,
       * returning failure with no further backtracking possible.
       * No operands.
       */
    vm->abort_flag = true;
    if (vm->choices_arena) {
      vm_arena_reset(vm->choices_arena);
    }
    vm->choices_top = 0;
    if (!vm->keep_choices) {
      vm_arena_destroy(vm->choices_arena);
      vm->choices_arena = nullptr;
      if (vm->use_compact_choice) {
        vm_write_log_free(vm);
        vm_trail_free(vm); /* free the undo trail allocated in vm_run */
      }
    }
    return false;
  }

#ifndef _MSC_VER
  op_succeed:
#endif
#ifdef _MSC_VER
  case OP_SUCCEED:
#endif
  {
    /* SUCCEED: force immediate match success at the
       * current position, skipping any remaining pattern.
       * No operands.
       */
    if (!vm->keep_choices) {
      vm_arena_destroy(vm->choices_arena);
      vm->choices_arena = nullptr;
      if (vm->use_compact_choice) {
        vm_write_log_free(vm);
        vm_trail_free(vm); /* free the undo trail allocated in vm_run */
      }
    }
    return true;
  }

  /* Note: ARB is not a separate opcode.
       * It is implemented at compile time as SPLIT + REPEAT
       * (equivalent to REPEAT_INIT(0,-1) + REPEAT_STEP on LEN(1)),
       * matching the spec description "split + LEN(1) loop".
       * Helper: snobol_emit_arb() in pattern_build.c builds the bytecode. */

#ifdef _MSC_VER
  default:
    if (!vm_pop_choice(vm))
      goto fail_ret;
    break;
  }
#endif
  /* Interpreter dispatch time tracking retired with tracing JIT */
}
if (!vm->keep_choices) {
  vm_arena_destroy(vm->choices_arena);
  vm->choices_arena = nullptr;
  if (vm->use_compact_choice) {
    vm_write_log_free(vm);
    vm_trail_free(vm); /* free the undo trail allocated in vm_run */
  }
}
return false;
fail_ret: if (!vm->keep_choices) {
  vm_arena_destroy(vm->choices_arena);
  vm->choices_arena = nullptr;
  if (vm->use_compact_choice) {
    vm_write_log_free(vm);
    vm_trail_free(vm); /* free the undo trail allocated in vm_run */
  }
}
return false;
}

bool SNOBOL_HOT vm_exec(VM *SNOBOL_RESTRICT vm) {
  vm->ip = 0;
  vm->pos = 0;
  vm->max_cap_used = 0;
  vm->max_counter_used = 0;
  vm->abort_flag = false;
  memset(vm->counters, 0, sizeof(vm->counters));

  /* Initialize control flow state if not already done */
  if (!vm->label_offsets) {
    vm_init_labels(vm);
  }

  /* Pre-register labels from bytecode tail (compiler-produced bytecodes only).
   * Detection: last 4 bytes == SNOBOL_LABEL_TABLE_MAGIC (0x534E424C = "SNBL").
   * Layout (from end): MAGIC | label_count | label_offsets[N] | ...charclass...
   * Hand-built test bytecodes have charclass_count there instead → no labels.
   */
  if (vm->bc_len >= 8) {
    uint32_t last4 = ((uint32_t)vm->bc[vm->bc_len - 4] << 24) |
                     ((uint32_t)vm->bc[vm->bc_len - 3] << 16) |
                     ((uint32_t)vm->bc[vm->bc_len - 2] << 8) |
                     (uint32_t)vm->bc[vm->bc_len - 1];
    if (last4 == SNOBOL_LABEL_TABLE_MAGIC) {
      uint32_t lc = ((uint32_t)vm->bc[vm->bc_len - 8] << 24) |
                    ((uint32_t)vm->bc[vm->bc_len - 7] << 16) |
                    ((uint32_t)vm->bc[vm->bc_len - 6] << 8) |
                    (uint32_t)vm->bc[vm->bc_len - 5];
      if (lc > 0 && vm->bc_len >= 8 + ((size_t)lc * 4)) {
        size_t table_base = vm->bc_len - 8 - ((size_t)lc * 4);
        for (uint32_t i = 0; i < lc; i++) {
          size_t op = table_base + ((size_t)i * 4);
          uint32_t offset =
              ((uint32_t)vm->bc[op] << 24) | ((uint32_t)vm->bc[op + 1] << 16) |
              ((uint32_t)vm->bc[op + 2] << 8) | (uint32_t)vm->bc[op + 3];
          vm_register_label(vm, (uint16_t)i, offset);
        }
      }
    }
  }

#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Initialize table and array registries if not already done */
  if (!vm->tables) {
    vm_init_tables(vm);
  }
  if (!vm->arrays) {
    vm_init_arrays(vm);
  }
#endif

#ifdef SNOBOL_PROFILE
  memset(&vm->profile, 0, sizeof(vm->profile));
#endif
  bool res = vm_run(vm);

  /* Cleanup - only labels registered from tail in this call */
  /* (If the caller pre-registered labels, they are responsible for cleanup) */
  /* For now, we keep the original behavior of vm_exec being self-contained,
   * but we allow pre-registered tables. Labels are usually per-bytecode. */
  vm_free_labels(vm);
#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Only free tables if they were NOT pre-registered?
   * Actually, let's just NOT free tables here and let the caller do it.
   * This is safer for persistent VM state. */
#endif

#ifdef SNOBOL_PROFILE
#endif
  return res;
}

/* Control flow initialization and cleanup */

void vm_init_labels(VM *vm) {
  vm->label_offsets = nullptr;
  vm->label_count = 0;
  vm->label_capacity = 0;
  vm->current_label = 0;
  vm->in_goto_fail = false;
  vm->abort_flag = false;
}

void vm_free_labels(VM *vm) {
  if (vm->label_offsets) {
    snobol_free(vm->label_offsets);
    vm->label_offsets = nullptr;
  }
  vm->label_count = 0;
  vm->label_capacity = 0;
}

bool vm_register_label(VM *vm, uint16_t label_id, uint32_t offset) {
  /* Ensure capacity */
  if (label_id >= vm->label_capacity) {
    size_t new_cap = (label_id + 1) * 2;
    uint16_t *new_offsets = (uint16_t *)snobol_realloc(
        vm->label_offsets, new_cap * sizeof(uint16_t));
    if (!new_offsets) {
      return false;
    }
    vm->label_offsets = new_offsets;
    vm->label_capacity = new_cap;
  }

  vm->label_offsets[label_id] = (uint16_t)offset;
  if (label_id >= vm->label_count) {
    vm->label_count = label_id + 1;
  }
  return true;
}

uint32_t vm_get_label_offset(VM *vm, uint16_t label_id) {
  if (label_id < vm->label_count && vm->label_offsets) {
    return vm->label_offsets[label_id];
  }
  return UINT32_MAX; /* Invalid label sentinel — offset 0 is a valid target */
}

#ifdef SNOBOL_DYNAMIC_PATTERN
/* Table registry functions */

void vm_init_tables(VM *vm) {
  vm->tables = nullptr;
  vm->table_count = 0;
  vm->table_capacity = 0;
}

void vm_free_tables(VM *vm) {
  if (vm->tables) {
    for (size_t i = 0; i < vm->table_count; i++) {
      if (vm->tables[i]) {
        table_release(vm->tables[i]);
      }
    }
    snobol_free(vm->tables);
    vm->tables = nullptr;
  }
  vm->table_count = 0;
  vm->table_capacity = 0;
}

bool vm_register_table(VM *vm, snobol_table_t *table, uint16_t *out_id) {
  if (vm->table_count >= vm->table_capacity) {
    size_t new_cap = (vm->table_capacity == 0) ? 16 : vm->table_capacity * 2;
    snobol_table_t **new_tables = (snobol_table_t **)snobol_realloc(
        vm->tables, new_cap * sizeof(snobol_table_t *));
    if (!new_tables) {
      return false;
    }
    vm->tables = new_tables;
    vm->table_capacity = new_cap;
  }

  *out_id = (uint16_t)vm->table_count;
  vm->tables[vm->table_count++] = table_retain(table);
  return true;
}

snobol_table_t *vm_get_table(VM *vm, uint16_t table_id) {
  if (table_id < vm->table_count && vm->tables) {
    return vm->tables[table_id];
  }
  return nullptr;
}

/* Array registry functions */

void vm_init_arrays(VM *vm) {
  vm->arrays = nullptr;
  vm->array_count = 0;
  vm->array_capacity = 0;
}

void vm_free_arrays(VM *vm) {
  if (vm->arrays) {
    for (size_t i = 0; i < vm->array_count; i++) {
      if (vm->arrays[i]) {
        snobol_array_release(vm->arrays[i]);
      }
    }
    snobol_free(vm->arrays);
    vm->arrays = nullptr;
  }
  vm->array_count = 0;
  vm->array_capacity = 0;
}

bool vm_register_array(VM *vm, snobol_array_t *array, uint16_t *out_id) {
  if (vm->array_count >= vm->array_capacity) {
    size_t new_cap = (vm->array_capacity == 0) ? 16 : vm->array_capacity * 2;
    snobol_array_t **new_arrays = (snobol_array_t **)snobol_realloc(
        vm->arrays, new_cap * sizeof(snobol_array_t *));
    if (!new_arrays) {
      return false;
    }
    vm->arrays = new_arrays;
    vm->array_capacity = new_cap;
  }

  *out_id = (uint16_t)vm->array_count;
  vm->arrays[vm->array_count++] = snobol_array_retain(array);
  return true;
}

snobol_array_t *vm_get_array(VM *vm, uint16_t array_id) {
  if (array_id < vm->array_count && vm->arrays) {
    return vm->arrays[array_id];
  }
  return nullptr;
}
#endif
