#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
#include "snobol/c23_compat.h"
#include <stddef.h>
#include <stdint.h>

/**
 * @file dynamic_pattern.h
 * @brief Dynamic pattern objects and canonical cache keys
 *
 * Ownership/Lifetime Rules:
 * - Dynamic patterns are created with dynamic_pattern_create() and freed with
 * dynamic_pattern_release()
 * - Dynamic patterns use reference counting for safe sharing
 * - The compiled bytecode is owned by the dynamic pattern object
 * - Cache entries hold references to dynamic patterns (via retain/release)
 * - Canonical keys are computed from pattern content for cache lookup
 *
 * Retain/Release Semantics:
 * - dynamic_pattern_create(): Returns pattern with refcount=1. Caller owns
 * reference.
 * - dynamic_pattern_retain(): Increments refcount. Returns same pointer for
 * convenience.
 * - dynamic_pattern_release(): Decrements refcount. Frees pattern when refcount
 * reaches 0.
 *
 * Cache Ownership Rules:
 * - dynamic_pattern_cache_get(): Returns pattern with retained reference.
 * Caller must release.
 * - dynamic_pattern_cache_put(): Cache retains its own reference. Caller still
 * owns their reference.
 * - dynamic_pattern_cache_remove(): Cache releases its reference. Caller still
 * owns their reference.
 * - dynamic_pattern_cache_clear(): Cache releases all references.
 * - dynamic_pattern_cache_destroy(): Cache releases all references and frees
 * memory.
 *
 * Execution Lifecycle (VM):
 * 1. OP_DYNAMIC_DEF: Stores source text and bytecode (both owned by VM)
 * 2. OP_DYNAMIC cache miss: Creates pattern, caches it (cache retains
 * reference)
 * 3. OP_DYNAMIC cache hit: Retrieves pattern (caller retains reference)
 * 4. Pattern execution: Uses pattern bytecode (pattern not modified)
 * 5. After execution: Caller releases their reference (cache retains its
 * reference)
 *
 * Success Path:
 * - Cache miss: create() → put() [cache retains] → execute → release()
 * [caller's ref]
 * - Cache hit: get() [retains for caller] → execute → release() [caller's ref]
 *
 * Failure Path:
 * - Cache miss with create failure: No pattern created, no cache entry
 * - Cache hit with execution failure: release() caller's reference, restore VM
 * state
 */

/* Forward declarations */
typedef struct snobol_table snobol_table_t;

/**
 * Dynamic pattern object
 *
 * Represents a pattern that was compiled at runtime from a string or AST.
 * The bytecode is owned by this object and freed when refcount reaches 0.
 */
typedef struct dynamic_pattern {
  uint8_t *bc;       /* Owned: compiled bytecode */
  size_t bc_len;     /* Length of bytecode */
  char *source;      /* Owned: original source string (for debugging) */
  uint32_t refcount; /* Reference count */
  uint32_t hash;     /* Pre-computed hash of source for caching */
  bool is_valid;     /* Pattern compiled successfully */
} dynamic_pattern_t;

/**
 * Canonical cache key for dynamic patterns
 *
 * Computed from pattern source content to enable cache lookup.
 * Two patterns with the same source will have the same cache key.
 */
typedef struct {
  uint32_t hash;     /* Hash of source content */
  size_t source_len; /* Length of source */
                     /* The actual source bytes follow this struct in memory */
} dynamic_pattern_cache_key_t;

/**
 * Dynamic pattern cache entry
 */
typedef struct dynamic_pattern_cache_entry {
  uint32_t hash;                            /* Hash for quick lookup */
  char *source;                             /* Owned: source string */
  dynamic_pattern_t *pattern;               /* Owned reference */
  struct dynamic_pattern_cache_entry *next; /* Next in bucket */
} dynamic_pattern_cache_entry_t;

/**
 * Dynamic pattern cache
 *
 * Thread-unsafe LRU-style cache for compiled dynamic patterns.
 * Uses open addressing with chaining for collision resolution.
 */
typedef struct {
  dynamic_pattern_cache_entry_t **buckets; /* Hash buckets */
  size_t bucket_count;                     /* Number of buckets */
  size_t size;                             /* Number of entries */
  size_t max_size;                         /* Maximum entries before eviction */
} dynamic_pattern_cache_t;

/**
 * @brief Create a new dynamic pattern object
 * @param source Source string (copied)
 * @param bc Compiled bytecode (ownership transferred)
 * @param bc_len Length of bytecode
 * @return Pointer to new dynamic pattern, or NULL on failure
 *
 * Ownership: Caller owns the returned pattern and must call
 * dynamic_pattern_release() The bytecode pointer is adopted by the pattern (do
 * not free separately)
 */
dynamic_pattern_t *dynamic_pattern_create(const char *source, uint8_t *bc,
                                          size_t bc_len);

/**
 * @brief Increment dynamic pattern reference count
 * @param pattern Pattern to retain
 * @return Same pattern pointer for convenience
 */
dynamic_pattern_t *dynamic_pattern_retain(dynamic_pattern_t *pattern);

/**
 * @brief Decrement reference count and free if zero
 * @param pattern Pattern to release
 */
void dynamic_pattern_release(dynamic_pattern_t *pattern);

/**
 * @brief Compute canonical cache key from source string
 * @param source Source string
 * @param source_len Length of source (or -1 for strlen)
 * @param out_key Output: cache key structure
 * @return Pointer to cache key (valid while source is valid)
 */
const dynamic_pattern_cache_key_t *dynamic_pattern_compute_key(
    const char *source, size_t source_len,
    dynamic_pattern_cache_key_t *out_key);

/**
 * @brief Initialize dynamic pattern cache
 * @param cache Cache to initialize
 * @param max_size Maximum entries before eviction (0 = default)
 * @return true on success
 */
bool dynamic_pattern_cache_init(dynamic_pattern_cache_t *cache,
                                size_t max_size);

/**
 * @brief Destroy cache and free all entries
 * @param cache Cache to destroy
 */
void dynamic_pattern_cache_destroy(dynamic_pattern_cache_t *cache);

/**
 * @brief Look up a pattern in the cache
 * @param cache Cache to query
 * @param source Source string
 * @param source_len Length of source (or -1 for strlen)
 * @return Cached pattern (reference retained), or NULL if not found
 *
 * Ownership: Caller must call dynamic_pattern_release() when done
 */
dynamic_pattern_t *dynamic_pattern_cache_get(dynamic_pattern_cache_t *cache,
                                             const char *source,
                                             int source_len);

/**
 * @brief Insert a pattern into the cache
 * @param cache Cache to modify
 * @param source Source string (copied)
 * @param pattern Pattern to cache (reference retained)
 * @return true on success, false on allocation failure or cache full
 *
 * Ownership: Cache retains its own reference to the pattern
 */
bool dynamic_pattern_cache_put(dynamic_pattern_cache_t *cache,
                               const char *source, dynamic_pattern_t *pattern);

/**
 * @brief Remove a pattern from the cache
 * @param cache Cache to modify
 * @param source Source string to remove
 * @param source_len Length of source (or -1 for strlen)
 * @return true if pattern was found and removed
 */
bool dynamic_pattern_cache_remove(dynamic_pattern_cache_t *cache,
                                  const char *source, int source_len);

/**
 * @brief Clear all entries from the cache
 * @param cache Cache to clear
 */
void dynamic_pattern_cache_clear(dynamic_pattern_cache_t *cache);

/**
 * @brief Get cache statistics
 * @param cache Cache to query
 * @param out_size Output: current number of entries
 * @param out_max Output: maximum entries
 */
void dynamic_pattern_cache_stats(const dynamic_pattern_cache_t *cache,
                                 size_t *out_size, size_t *out_max);

/**
 * @brief Hash function for pattern sources
 * @param source Source string
 * @param len Length of source
 * @return 32-bit hash value
 */
uint32_t dynamic_pattern_hash_source(const char *source, size_t len);

#ifdef __cplusplus
}
#endif
