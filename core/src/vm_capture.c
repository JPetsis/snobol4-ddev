/**
 * @file vm_capture.c
 * @brief Capture-register write-log for the full SNOBOL4 VM.
 *
 * Split out of vm.c (see oss-readiness T4). The write-log records prior
 * capture-register values so captures can be restored precisely on
 * backtrack. The choice stack lives in vm_choice.c; the main executor
 * lives in vm_exec.c.
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

/* ========== Write-log management for compact choice stack ========== */

void vm_write_log_init(VM *vm) {
  vm->write_log = nullptr;
  vm->write_log_cap = 0;
  vm->write_log_next = 0;
  vm->write_log_bitmap = 0;
  vm->write_log_compressed_count = 0;
  vm->write_log_dirty = false;
}

void vm_write_log_free(VM *vm) {
  if (vm->write_log) {
    snobol_free(vm->write_log);
    vm->write_log = nullptr;
  }
  vm->write_log_cap = 0;
  vm->write_log_next = 0;
  vm->write_log_bitmap = 0;
  vm->write_log_compressed_count = 0;
  vm->write_log_dirty = false;
}

void vm_write_log_clear(VM *vm) {
  vm->write_log_next = 0;
  vm->write_log_bitmap = 0;
  vm->write_log_dirty = false;
}

void vm_write_log_track_cap_start(VM *vm, uint8_t cap, size_t old_start) {
  if (!vm->use_compact_choice || !vm->write_log) {
    return;
  }
  /* Look for existing entry for this cap (most recent first) */
  size_t slot =
      (vm->write_log_next > 0) ? vm->write_log_next - 1 : vm->write_log_cap - 1;
  for (size_t i = 0; i < vm->write_log_cap; i++) {
    if (vm->write_log_bitmap & (1ULL << slot)) {
      if (vm->write_log[slot].cap_index == cap) {
        vm->write_log[slot].old_start = old_start;
        return;
      }
    }
    if (slot == 0) {
      slot = vm->write_log_cap - 1;
    } else {
      slot--;
    }
  }
  /* No existing entry: create new */
  slot = vm->write_log_next++;
  if (slot >= vm->write_log_cap) {
    vm->write_log_next = 0;
    slot = 0;
  }
  vm->write_log[slot].cap_index = cap;
  vm->write_log[slot].old_start = old_start;
  vm->write_log[slot].old_end = (size_t)-1; /* sentinel: end not changed */
  vm->write_log_bitmap |= (1ULL << slot);
  vm->write_log_dirty = true;
}

void vm_write_log_track_cap_end(VM *vm, uint8_t cap, size_t old_end) {
  if (!vm->use_compact_choice || !vm->write_log) {
    return;
  }
  /* Look for existing entry for this cap (most recent first) */
  size_t slot =
      (vm->write_log_next > 0) ? vm->write_log_next - 1 : vm->write_log_cap - 1;
  for (size_t i = 0; i < vm->write_log_cap; i++) {
    if (vm->write_log_bitmap & (1ULL << slot)) {
      if (vm->write_log[slot].cap_index == cap) {
        vm->write_log[slot].old_end = old_end;
        return;
      }
    }
    if (slot == 0) {
      slot = vm->write_log_cap - 1;
    } else {
      slot--;
    }
  }
  /* No existing entry: create new */
  slot = vm->write_log_next++;
  if (slot >= vm->write_log_cap) {
    vm->write_log_next = 0;
    slot = 0;
  }
  vm->write_log[slot].cap_index = cap;
  vm->write_log[slot].old_start = (size_t)-1; /* sentinel: start not changed */
  vm->write_log[slot].old_end = old_end;
  vm->write_log_bitmap |= (1ULL << slot);
  vm->write_log_dirty = true;
}

size_t vm_write_log_count_entries(const VM *vm) {
  size_t count = 0;
  for (size_t i = 0; i < vm->write_log_cap; i++) {
    if (vm->write_log_bitmap & (1ULL << i)) {
      const WriteLogEntry *e = &vm->write_log[i];
      if (e->old_start != (size_t)-1 || e->old_end != (size_t)-1) {
        count++;
      }
    }
  }
  return count;
}

void vm_write_log_copy_entries(const VM *vm, WriteLogEntry *dst,
                               size_t dst_cap) {
  size_t copied = 0;
  for (size_t i = 0; i < vm->write_log_cap; i++) {
    if (vm->write_log_bitmap & (1ULL << i)) {
      const WriteLogEntry *e = &vm->write_log[i];
      if (e->old_start != (size_t)-1 || e->old_end != (size_t)-1) {
        if (copied < dst_cap) {
          dst[copied] = *e;
          copied++;
        }
      }
    }
  }
}

/* ========== Undo trail for trail-based choice save (W2a) ========== */

void vm_trail_init(VM *vm) {
  vm->trail = nullptr;
  vm->trail_cap = 0;
  vm->trail_top = 0;
}

void vm_trail_free(VM *vm) {
  if (vm->trail) {
    snobol_free(vm->trail);
    vm->trail = nullptr;
  }
  vm->trail_cap = 0;
  vm->trail_top = 0;
}

void vm_trail_clear(VM *vm) {
  vm->trail_top = 0;
}

void vm_trail_push(VM *vm, UndoRecord rec) {
  if (vm->trail_top >= vm->trail_cap) {
    size_t new_cap = vm->trail_cap ? vm->trail_cap * 2 : 256;
    while (new_cap <= vm->trail_top) {
      new_cap *= 2;
    }
    UndoRecord *nt =
        (UndoRecord *)snobol_realloc(vm->trail, new_cap * sizeof(UndoRecord));
    if (!nt) {
      return;
    }
    vm->trail = nt;
    vm->trail_cap = new_cap;
  }
  vm->trail[vm->trail_top++] = rec;
}

void vm_trail_counter_inc(VM *vm, uint8_t loop_id, uint32_t prior_count,
                          size_t prior_last_pos) {
  if (!vm->use_compact_choice) {
    return;
  }
  UndoRecord r;
  memset(&r, 0, sizeof(r));
  r.kind = UNDO_COUNTER_DEC;
  r.index = loop_id;
  r.prior_u = prior_count;
  r.prior_lp = prior_last_pos;
  vm_trail_push(vm, r);
}

void vm_trail_cap_write(VM *vm, uint8_t cap, uint8_t sub, size_t prior_start,
                        size_t prior_end) {
  if (!vm->use_compact_choice) {
    return;
  }
  UndoRecord r;
  memset(&r, 0, sizeof(r));
  r.kind = UNDO_CAP_WRITE;
  r.index = cap;
  r.sub = sub;
  r.prior_a = prior_start;
  r.prior_b = prior_end;
  vm_trail_push(vm, r);
}

void vm_trail_var_write(VM *vm, uint8_t var, size_t prior_start,
                        size_t prior_end) {
  if (!vm->use_compact_choice) {
    return;
  }
  UndoRecord r;
  memset(&r, 0, sizeof(r));
  r.kind = UNDO_VAR_WRITE;
  r.index = var;
  r.prior_a = prior_start;
  r.prior_b = prior_end;
  vm_trail_push(vm, r);
}

void vm_trail_replay(VM *vm, size_t base) {
  /* Replay [base, top) in reverse so the most recent mutation is undone first.
   * For a single register, the reverse order naturally restores the original
   * value (later writes overwrite earlier ones; undoing the last write first
   * is required for correctness). */
  for (size_t i = vm->trail_top; i > base; i--) {
    const UndoRecord *e = &vm->trail[i - 1];
    switch (e->kind) {
      case UNDO_COUNTER_DEC:
        if (e->index < MAX_LOOPS) {
          vm->counters[e->index] = e->prior_u;
          vm->loop_last_pos[e->index] = e->prior_lp;
        }
        break;
      case UNDO_CAP_WRITE:
        if (e->index < MAX_CAPS) {
          if (e->sub == 0) {
            { vm->cap_start[e->index] = e->prior_a; }
          } else if (e->sub == 1) {
            { vm->cap_end[e->index] = e->prior_b; }
          } else {
            vm->cap_start[e->index] = e->prior_a;
            vm->cap_end[e->index] = e->prior_b;
          }
        }
        break;
      case UNDO_WL_POP:
        /* Retained for header compatibility; capture restoration now uses
       * UNDO_CAP_WRITE. No-op. */
        break;
      case UNDO_VAR_WRITE:
        if (e->index < MAX_VARS) {
          vm->var_start[e->index] = e->prior_a;
          vm->var_end[e->index] = e->prior_b;
        }
        break;
      default: break;
    }
  }
  vm->trail_top = base;
}

size_t vm_trail_depth(const VM *vm) {
  return vm->trail_top;
}
