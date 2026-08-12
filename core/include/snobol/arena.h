#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "snobol/snobol_attrs.h"

/* C11 _Alignof: MSVC in default mode does not recognise _Alignof. */
#if defined(_MSC_VER) && !defined(_Alignof)
#define _Alignof __alignof
#endif

/**
 * @file arena.h
 * @brief Bump-allocated arena pool for short-lived scratch allocations
 *        (e.g. AST nodes during compilation, scan-loop temporaries).
 *
 * The arena is owned by a single compile/search call: it is initialised once,
 * used for many small fixed-size allocations, and reset (or discarded) when
 * the call completes.  There is no per-object free — the whole arena is
 * released by the caller when it goes out of scope.  When the arena is
 * exhausted, callers fall back to snobol_malloc().
 */

#ifndef SNOBOL_ARENA_DEFAULT_CAPACITY
/** Default arena capacity: 64 KiB covers all but pathologically large ASTs. */
#define SNOBOL_ARENA_DEFAULT_CAPACITY (64u * 1024u)
#endif

typedef struct {
  uint8_t *base;   /**< Bump-pointer start (caller-allocated buffer) */
  uint8_t *ptr;    /**< Current bump position */
  size_t capacity; /**< Total size of the backing buffer */
  size_t peak;     /**< High-water mark for diagnostics */
} snobol_arena_t;

/**
 * Initialise an arena over a caller-provided buffer.
 *
 * @param arena     Arena to initialise (must be a valid pointer).
 * @param buffer    Backing storage (capacity bytes); may be NULL if capacity 0.
 * @param capacity  Size of @p buffer in bytes.
 */
static inline void snobol_arena_init(snobol_arena_t *arena, void *buffer,
                                     size_t capacity) {
  arena->base = (uint8_t *)buffer;
  arena->ptr = (uint8_t *)buffer;
  arena->capacity = capacity;
  arena->peak = 0;
}

/**
 * Bump-allocate @p size bytes aligned to @p align from the arena.
 *
 * @return Pointer into the arena, or NULL when the arena is exhausted.
 *         Callers must fall back to snobol_malloc() on NULL.
 */
static inline void *snobol_arena_alloc(snobol_arena_t *arena, size_t size,
                                       size_t align) {
  if (!arena->base) {
    return NULL;
  }
  uintptr_t raw = (uintptr_t)arena->ptr;
  uintptr_t aligned = (raw + (align > 0 ? align - 1 : 0)) &
                      ~((uintptr_t)(align > 0 ? align - 1 : 0));
  size_t used = (size_t)(aligned - (uintptr_t)arena->base);
  if (size > arena->capacity || used + size > arena->capacity) {
    return NULL;
  }
  void *p = (void *)aligned;
  arena->ptr = (uint8_t *)(aligned + size);
  if ((size_t)(arena->ptr - arena->base) > arena->peak) {
    arena->peak = (size_t)(arena->ptr - arena->base);
  }
  return p;
}

/**
 * Reset the arena to its initial (empty) state, retaining the backing buffer
 * for reuse by the next compile.
 */
static inline void snobol_arena_reset(snobol_arena_t *arena) {
  arena->ptr = arena->base;
}

/** Convenience: allocate an object of type @p t from arena @p a. */
#define ARENA_ALLOC(a, t) (t *)snobol_arena_alloc((a), sizeof(t), _Alignof(t))

#ifdef __cplusplus
}
#endif
