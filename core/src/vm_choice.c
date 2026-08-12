/**
 * @file vm_choice.c
 * @brief Backtracking choice-stack management for the full SNOBOL4 VM.
 *
 * Split out of vm.c (see oss-readiness T4). Implements the compact choice
 * stack: push/pop of choice records, record-size accounting, stack depth /
 * memory statistics, and snobol_vm_reset(). The capture write-log lives in
 * vm_capture.c; the main executor lives in vm_exec.c.
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

size_t vm_compact_choice_record_size(const CompactChoiceHeader *hdr) {
  (void)hdr;
  /* Trail-based records are fixed-size: header + trailing size uint32. */
  size_t sz = sizeof(CompactChoiceHeader) + sizeof(uint32_t);
  return (sz + 7) & ~7;
}

/* ── Choice-stack arena (W2c) ───────────────────────────────────────────────
 * Each record is stored as [uint32 leading_size][payload][uint32 trailing_size]
 * within a page. Pop reads the trailing word of the current page's tail; depth
 * walking reads the leading word. Pages are chained and empty pages past the
 * head are freed on reset/pop. */

ChoiceArena *vm_arena_create(void) {
  ChoiceArena *a = (ChoiceArena *)snobol_malloc(sizeof(*a));
  if (!a) {
    return nullptr;
  }
  a->head = (ChoiceArenaPage *)snobol_malloc(sizeof(ChoiceArenaPage));
  if (!a->head) {
    snobol_free(a);
    return nullptr;
  }
  a->head->data = (uint8_t *)snobol_malloc(CHOICE_ARENA_PAGE_SIZE);
  if (!a->head->data) {
    snobol_free(a->head);
    snobol_free(a);
    return nullptr;
  }
  a->head->cap = CHOICE_ARENA_PAGE_SIZE;
  a->head->used = 0;
  a->head->prev = a->head->next = nullptr;
  a->cur = a->head;
  a->total_used = 0;
  a->peak_used = 0;
  a->last_rec_size = 0;
  return a;
}

void vm_arena_destroy(ChoiceArena *a) {
  if (!a) {
    return;
  }
  ChoiceArenaPage *p = a->head;
  while (p) {
    ChoiceArenaPage *nx = p->next;
    snobol_free(p->data);
    snobol_free(p);
    p = nx;
  }
  snobol_free(a);
}

void vm_arena_reset(ChoiceArena *a) {
  if (!a) {
    return;
  }
  ChoiceArenaPage *p = a->head->next;
  while (p) {
    ChoiceArenaPage *nx = p->next;
    snobol_free(p->data);
    snobol_free(p);
    p = nx;
  }
  a->head->next = nullptr;
  a->head->used = 0;
  a->cur = a->head;
  a->total_used = 0;
  a->peak_used = 0;
  a->last_rec_size = 0;
}

static ChoiceArenaPage *arena_new_page(size_t need) {
  ChoiceArenaPage *p = (ChoiceArenaPage *)snobol_malloc(sizeof(*p));
  if (!p) {
    return nullptr;
  }
  size_t cap = need > CHOICE_ARENA_PAGE_SIZE ? need : CHOICE_ARENA_PAGE_SIZE;
  p->data = (uint8_t *)snobol_malloc(cap);
  if (!p->data) {
    snobol_free(p);
    return nullptr;
  }
  p->cap = cap;
  p->used = 0;
  p->prev = p->next = nullptr;
  return p;
}

uint8_t *vm_arena_alloc(ChoiceArena *a, size_t payload_len) {
  if (!a) {
    return nullptr;
  }
  /* Layout: leading uint32 footprint | 4-byte align pad | aligned payload
   *         | 4-byte align pad | trailing uint32 footprint (last 4 bytes).
   * Payload is 8-aligned (CompactChoiceHeader / struct choice carry size_t
   * members that require 8-byte alignment — placing them at base+4 was UB
   * and triggered UBSan / Windows SEGV). Total footprint is a multiple of 8
   * so the next record's payload also lands at an 8-aligned offset. */
  size_t aligned_payload = (payload_len + 7) & ~(size_t)7;
  size_t footprint = aligned_payload + (2 * sizeof(uint32_t)) + 8;
  if (a->cur->used + footprint > a->cur->cap) {
    ChoiceArenaPage *np = arena_new_page(footprint);
    if (!np) {
      return nullptr;
    }
    np->prev = a->cur;
    a->cur->next = np;
    a->cur = np;
  }
  uint8_t *base = a->cur->data + a->cur->used;
  uint32_t *lw = (uint32_t *)base;
  uint32_t *tw = (uint32_t *)(base + footprint - sizeof(uint32_t));
  lw[0] = (uint32_t)footprint;
  tw[0] = (uint32_t)footprint;
  a->cur->used += footprint;
  a->total_used += footprint;
  if (a->total_used > a->peak_used) {
    a->peak_used = a->total_used;
  }
  a->last_rec_size = footprint;
  return base + 8; /* 8-byte aligned payload area */
}

void vm_arena_pop_last(ChoiceArena *a) {
  if (!a || a->total_used == 0) {
    return;
  }
  /* Trailing size word sits just before the page tail. */
  uint32_t footprint =
      *(uint32_t *)(a->cur->data + a->cur->used - sizeof(uint32_t));
  a->cur->used -= footprint;
  a->total_used -= footprint;
  if (a->cur->used == 0 && a->cur != a->head) {
    ChoiceArenaPage *prev = a->cur->prev;
    prev->next = nullptr;
    snobol_free(a->cur->data);
    snobol_free(a->cur);
    a->cur = prev;
  }
  if (a->cur->used > 0) {
    /* Recover the new last record's footprint from its trailing word. */
    a->last_rec_size =
        *(uint32_t *)(a->cur->data + a->cur->used - sizeof(uint32_t));
  } else {
    a->last_rec_size = 0;
  }
}

/* Reset VM between match attempts while preserving choice allocation */
void snobol_vm_reset(VM *vm) {
  memset(vm->cap_start, 0, sizeof(vm->cap_start));
  memset(vm->cap_end, 0, sizeof(vm->cap_end));
  vm->var_count = 0;
  memset(vm->var_start, 0, sizeof(vm->var_start));
  memset(vm->var_end, 0, sizeof(vm->var_end));
  memset(vm->counters, 0, sizeof(vm->counters));
  memset(vm->loop_min, 0, sizeof(vm->loop_min));
  memset(vm->loop_max, 0, sizeof(vm->loop_max));
  memset(vm->loop_last_pos, 0, sizeof(vm->loop_last_pos));
  memset(vm->loop_span_greedy, 0, sizeof(vm->loop_span_greedy));
  vm->max_cap_used = 0;
  vm->max_counter_used = 0;
  vm->ip = 0;
  vm->pos = 0;
  vm->abort_flag = false;
  vm->in_goto_fail = false;
  vm->current_label = 0;
  vm->choices_top = 0;
  if (vm->choices_arena) {
    vm_arena_reset(vm->choices_arena);
  }
  vm->choice_allocated = 0;
  vm->choice_push_count = 0;
  vm->choice_peak_depth = 0;
  vm->choice_peak_memory = 0;
  if (vm->write_log) {
    vm_write_log_clear(vm);
  }
}

/* Choice stack statistics */
size_t vm_choice_stack_memory_usage(VM *vm) {
  return vm->choices_top;
}

size_t vm_choice_stack_depth(VM *vm) {
  /* Depth = number of choice records on the arena stack. Each record carries
   * a leading size word, so we walk page-by-page counting records. */
  if (!vm->choices_arena) {
    return 0;
  }
  size_t depth = 0;
  for (ChoiceArenaPage *p = vm->choices_arena->head; p; p = p->next) {
    size_t pos = 0;
    while (pos + (2 * sizeof(uint32_t)) <= p->used) {
      uint32_t footprint = *(uint32_t *)(p->data + pos);
      pos += footprint;
      depth++;
    }
  }
  return depth;
}

size_t vm_choice_record_average_size(VM *vm) {
  if (vm->choice_push_count > 0) {
    return vm->choice_allocated / vm->choice_push_count;
  }
  return 0;
}

void vm_push_choice(VM *vm, size_t ip, size_t pos) {
  if (!vm->choices_arena) {
    return;
  }
#ifdef SNOBOL_PROFILE
  vm->profile.push_count++;
#endif
  if (vm->use_compact_choice) {
    /* Trail-based choice save (W2a): the record stores only cheap scalar
     * fields and the trail-base index. State is restored by replaying the
     * abandoned thread's undo records in reverse (see vm_pop_choice), so no
     * counter / write-log memcpy is needed. Choice-push cost is now O(1)
     * regardless of the number of loops or emits. Allocation goes through the
     * page-linked arena (W2c). */
    size_t rec_payload = sizeof(CompactChoiceHeader) + sizeof(uint32_t);
    CompactChoiceHeader *h =
        (CompactChoiceHeader *)vm_arena_alloc(vm->choices_arena, rec_payload);
    if (!h) {
      return;
    }
    h->total_size = (uint32_t)rec_payload;
    h->ip = ip;
    h->pos = pos;
    h->var_count = vm->var_count;
    h->max_cap_used = vm->max_cap_used;
    h->max_counter_used = vm->max_counter_used;
    h->trail_base = (uint32_t)vm->trail_top;
    h->pad = 0;
    uint32_t *size_ptr =
        (uint32_t *)((uint8_t *)h + rec_payload - sizeof(uint32_t));
    *size_ptr = (uint32_t)rec_payload;
    vm->choices_top = vm->choices_arena->total_used;
    vm->choice_allocated += rec_payload + (2 * sizeof(uint32_t));
    vm->choice_push_count++;
    vm->choice_live_depth++;
    if (vm->choices_top > vm->choice_peak_memory) {
      vm->choice_peak_memory = vm->choices_top;
    }
    if (vm->choice_live_depth > vm->choice_peak_depth) {
      vm->choice_peak_depth = vm->choice_live_depth;
    }
  } else {
    struct choice *c = (struct choice *)vm_arena_alloc(vm->choices_arena,
                                                       sizeof(struct choice));
    if (!c) {
      return;
    }
    c->ip = ip;
    c->pos = pos;
    c->var_count_snapshot = vm->var_count;
    c->max_cap_used_snapshot = vm->max_cap_used;
    c->max_counter_used_snapshot = vm->max_counter_used;
    if (vm->max_cap_used > 0) {
      size_t cap_size = vm->max_cap_used * sizeof(size_t);
      memcpy(c->cap_start_snapshot, vm->cap_start, cap_size);
      memcpy(c->cap_end_snapshot, vm->cap_end, cap_size);
    }
    if (vm->max_counter_used > 0) {
      memcpy(c->counters_snapshot, vm->counters,
             vm->max_counter_used * sizeof(uint32_t));
      memcpy(c->loop_last_pos_snapshot, vm->loop_last_pos,
             vm->max_counter_used * sizeof(size_t));
    }
    vm->choices_top = vm->choices_arena->total_used;
    vm->choice_allocated += sizeof(struct choice) + (2 * sizeof(uint32_t));
    vm->choice_push_count++;
    vm->choice_live_depth++;
    if (vm->choices_top > vm->choice_peak_memory) {
      vm->choice_peak_memory = vm->choices_top;
    }
    if (vm->choice_live_depth > vm->choice_peak_depth) {
      vm->choice_peak_depth = vm->choice_live_depth;
    }
  }
}

bool vm_pop_choice(VM *vm) {
  if (!vm->choices_arena || vm->choices_arena->total_used == 0) {
    return false;
  }
#ifdef SNOBOL_PROFILE
  vm->profile.pop_count++;
#endif
  if (vm->use_compact_choice) {
    ChoiceArenaPage *pg = vm->choices_arena->cur;
    uint32_t footprint = *(uint32_t *)(pg->data + pg->used - sizeof(uint32_t));
    uint8_t *rec_base = pg->data + pg->used - footprint;
    CompactChoiceHeader *h =
        (CompactChoiceHeader *)(rec_base + 8); /* 8-aligned payload */
    vm->ip = h->ip;
    vm->pos = h->pos;
    vm->var_count = h->var_count;
    vm->max_cap_used = h->max_cap_used;
    vm->max_counter_used = h->max_counter_used;
    /* Restore counters / captures / vars by replaying the abandoned thread's
     * trail entries in reverse. This is bit-exact with the prior snapshot
     * path because every mutation since the choice was pushed is undone in
     * the opposite order it was applied. */
    vm_trail_replay(vm, h->trail_base);
  } else {
    ChoiceArenaPage *pg = vm->choices_arena->cur;
    uint32_t footprint = *(uint32_t *)(pg->data + pg->used - sizeof(uint32_t));
    uint8_t *rec_base = pg->data + pg->used - footprint;
    struct choice *c = (struct choice *)(rec_base + 8); /* 8-aligned payload */
    vm->ip = c->ip;
    vm->pos = c->pos;
    vm->var_count = c->var_count_snapshot;
    vm->max_cap_used = c->max_cap_used_snapshot;
    vm->max_counter_used = c->max_counter_used_snapshot;
    if (vm->max_cap_used > 0) {
      size_t cap_bytes = vm->max_cap_used * sizeof(size_t);
      memcpy(vm->cap_start, c->cap_start_snapshot, cap_bytes);
      memcpy(vm->cap_end, c->cap_end_snapshot, cap_bytes);
    }
    if (vm->max_counter_used > 0) {
      memcpy(vm->counters, c->counters_snapshot,
             vm->max_counter_used * sizeof(uint32_t));
      memcpy(vm->loop_last_pos, c->loop_last_pos_snapshot,
             vm->max_counter_used * sizeof(size_t));
    }
  }
  vm_arena_pop_last(vm->choices_arena);
  vm->choices_top = vm->choices_arena->total_used;
  if (vm->choice_live_depth > 0) {
    vm->choice_live_depth--;
  }
  return true;
}
