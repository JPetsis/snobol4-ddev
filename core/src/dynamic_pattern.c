/**
 * @file dynamic_pattern.c
 * @brief Dynamic pattern objects and caching implementation
 *
 * Implements runtime-compiled pattern objects with reference counting
 * and a cache for reusing previously compiled patterns.
 */

#include "snobol/dynamic_pattern.h"
#include "snobol/snobol_internal.h"
#include <stdint.h>
#include <string.h>

/* Default cache size */
enum { CACHE_DEFAULT_MAX_SIZE = 64 };
enum { CACHE_INITIAL_BUCKETS = 32 };

/**
 * FNV-1a hash for pattern sources
 */
/* Guard keeps the amalgam (single-TU) build clean: table.c (included before
 * this file) already defines these as constexpr uint32_t. */
#ifndef SNOBOL_FNV_CONSTANTS_DEFINED
#define SNOBOL_FNV_CONSTANTS_DEFINED
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME 16777619u
#endif

uint32_t dynamic_pattern_hash_source(const char *source, size_t len) {
  uint32_t hash = FNV_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)source[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

dynamic_pattern_t *dynamic_pattern_create(const char *source, uint8_t *bc,
                                          size_t bc_len) {
  dynamic_pattern_t *pattern =
      (dynamic_pattern_t *)snobol_calloc(1, sizeof(dynamic_pattern_t));
  if (!pattern) {
    return nullptr;
  }

  pattern->bc = bc;
  pattern->bc_len = bc_len;
  pattern->refcount = 1;
  pattern->is_valid = ((bc != nullptr && bc_len > 0) != 0);
  pattern->source = nullptr;
  pattern->hash = 0;

  if (source) {
    size_t source_len = strlen(source);
    pattern->source = (char *)snobol_malloc(source_len + 1);
    if (pattern->source) {
      strcpy(pattern->source, source);
      pattern->hash = dynamic_pattern_hash_source(source, source_len);
    }
  }

  SNOBOL_LOG(
      "dynamic_pattern_create: pattern=%p source='%s' bc_len=%zu valid=%d",
      (void *)pattern, source ? source : "(null)", bc_len, pattern->is_valid);

  return pattern;
}

dynamic_pattern_t *dynamic_pattern_retain(dynamic_pattern_t *pattern) {
  if (!pattern) {
    return nullptr;
  }
  pattern->refcount++;
  SNOBOL_LOG("dynamic_pattern_retain: pattern=%p refcount=%u", (void *)pattern,
             pattern->refcount);
  return pattern;
}

void dynamic_pattern_release(dynamic_pattern_t *pattern) {
  if (!pattern) {
    return;
  }

  pattern->refcount--;
  SNOBOL_LOG("dynamic_pattern_release: pattern=%p refcount=%u", (void *)pattern,
             pattern->refcount);

  if (pattern->refcount > 0) {
    return;
  }

  SNOBOL_LOG("dynamic_pattern_release: freeing pattern=%p bc=%p",
             (void *)pattern, (void *)pattern->bc);

  /* Free bytecode */
  if (pattern->bc) {
/* Use compiler_free if available, otherwise snobol_free */
#ifdef STANDALONE_BUILD
    snobol_free(pattern->bc);
#else
    /* In PHP build, use the compiler's free function */
    extern void compiler_free(uint8_t *bc);
    compiler_free(pattern->bc);
#endif
  }

  /* Free source */
  if (pattern->source) {
    snobol_free(pattern->source);
  }

  snobol_free(pattern);
}

const dynamic_pattern_cache_key_t *dynamic_pattern_compute_key(
    const char *source, size_t source_len,
    dynamic_pattern_cache_key_t *out_key) {
  if (!source || !out_key) {
    return nullptr;
  }

  size_t len = (source_len < 0) ? strlen(source) : source_len;
  out_key->hash = dynamic_pattern_hash_source(source, len);
  out_key->source_len = len;

  return out_key;
}

bool dynamic_pattern_cache_init(dynamic_pattern_cache_t *cache,
                                size_t max_size) {
  if (!cache) {
    return false;
  }

  cache->max_size = (max_size > 0) ? max_size : CACHE_DEFAULT_MAX_SIZE;
  cache->bucket_count = CACHE_INITIAL_BUCKETS;
  cache->size = 0;

  cache->buckets = (dynamic_pattern_cache_entry_t **)snobol_calloc(
      cache->bucket_count, sizeof(dynamic_pattern_cache_entry_t *));

  if (!cache->buckets) {
    return false;
  }

  SNOBOL_LOG("dynamic_pattern_cache_init: cache=%p max_size=%zu buckets=%zu",
             (void *)cache, cache->max_size, cache->bucket_count);

  return true;
}

void dynamic_pattern_cache_destroy(dynamic_pattern_cache_t *cache) {
  if (!cache || !cache->buckets) {
    return;
  }

  SNOBOL_LOG("dynamic_pattern_cache_destroy: cache=%p size=%zu", (void *)cache,
             cache->size);

  /* Free all entries */
  for (size_t i = 0; i < cache->bucket_count; i++) {
    dynamic_pattern_cache_entry_t *entry = cache->buckets[i];
    while (entry) {
      dynamic_pattern_cache_entry_t *next = entry->next;

      if (entry->source) {
        snobol_free(entry->source);
      }
      if (entry->pattern) {
        dynamic_pattern_release(entry->pattern);
      }
      snobol_free(entry);

      entry = next;
    }
  }

  snobol_free((void *)cache->buckets);
  cache->buckets = nullptr;
  cache->size = 0;
  cache->bucket_count = 0;
}

static dynamic_pattern_cache_entry_t *cache_find_entry(
    dynamic_pattern_cache_t *cache, uint32_t hash, const char *source,
    size_t source_len) {
  size_t bucket = hash % cache->bucket_count;
  dynamic_pattern_cache_entry_t *entry = cache->buckets[bucket];

  while (entry) {
    if (entry->hash == hash && entry->source &&
        strlen(entry->source) == source_len &&
        memcmp(entry->source, source, source_len) == 0) {
      return entry;
    }
    entry = entry->next;
  }

  return nullptr;
}

dynamic_pattern_t *dynamic_pattern_cache_get(dynamic_pattern_cache_t *cache,
                                             const char *source,
                                             int source_len) {
  if (!cache || !source) {
    return nullptr;
  }

  size_t len = (source_len < 0) ? strlen(source) : source_len;
  uint32_t hash = dynamic_pattern_hash_source(source, len);

  dynamic_pattern_cache_entry_t *entry =
      cache_find_entry(cache, hash, source, len);

  if (entry) {
    SNOBOL_LOG("dynamic_pattern_cache_get: cache=%p source='%s' HIT",
               (void *)cache, source);
    return dynamic_pattern_retain(entry->pattern);
  }

  SNOBOL_LOG("dynamic_pattern_cache_get: cache=%p source='%s' MISS",
             (void *)cache, source);
  return nullptr;
}

static bool cache_evict_one(dynamic_pattern_cache_t *cache) {
  /* Simple eviction: remove first entry from first non-empty bucket */
  for (size_t i = 0; i < cache->bucket_count; i++) {
    if (cache->buckets[i]) {
      dynamic_pattern_cache_entry_t *entry = cache->buckets[i];
      cache->buckets[i] = entry->next;

      SNOBOL_LOG("dynamic_pattern_cache_evict: removing source='%s'",
                 entry->source ? entry->source : "(null)");

      if (entry->source) {
        snobol_free(entry->source);
      }
      if (entry->pattern) {
        dynamic_pattern_release(entry->pattern);
      }
      snobol_free(entry);

      cache->size--;
      return true;
    }
  }

  return false;
}

bool dynamic_pattern_cache_put(dynamic_pattern_cache_t *cache,
                               const char *source, dynamic_pattern_t *pattern) {
  if (!cache || !source || !pattern) {
    return false;
  }

  size_t source_len = strlen(source);
  uint32_t hash = dynamic_pattern_hash_source(source, source_len);

  /* Check if already in cache */
  if (cache_find_entry(cache, hash, source, source_len)) {
    SNOBOL_LOG("dynamic_pattern_cache_put: already cached, skipping");
    return true;
  }

  /* Evict if at capacity */
  while (cache->size >= cache->max_size) {
    if (!cache_evict_one(cache)) {
      break;
    }
  }

  /* Create new entry */
  dynamic_pattern_cache_entry_t *entry =
      (dynamic_pattern_cache_entry_t *)snobol_malloc(
          sizeof(dynamic_pattern_cache_entry_t));
  if (!entry) {
    return false;
  }

  entry->source = (char *)snobol_malloc(source_len + 1);
  if (!entry->source) {
    snobol_free(entry);
    return false;
  }
  strcpy(entry->source, source);

  entry->hash = hash;
  entry->pattern = dynamic_pattern_retain(pattern);
  entry->next = nullptr;

  /* Insert into bucket */
  size_t bucket = hash % cache->bucket_count;
  entry->next = cache->buckets[bucket];
  cache->buckets[bucket] = entry;
  cache->size++;

  SNOBOL_LOG("dynamic_pattern_cache_put: cache=%p source='%s' size=%zu",
             (void *)cache, source, cache->size);

  return true;
}

bool dynamic_pattern_cache_remove(dynamic_pattern_cache_t *cache,
                                  const char *source, int source_len) {
  if (!cache || !source) {
    return false;
  }

  size_t len = (source_len < 0) ? strlen(source) : source_len;
  uint32_t hash = dynamic_pattern_hash_source(source, len);

  size_t bucket = hash % cache->bucket_count;
  dynamic_pattern_cache_entry_t *entry = cache->buckets[bucket];
  dynamic_pattern_cache_entry_t *prev = nullptr;

  while (entry) {
    if (entry->hash == hash && entry->source && strlen(entry->source) == len &&
        memcmp(entry->source, source, len) == 0) {

      /* Found - remove from chain */
      if (prev) {
        prev->next = entry->next;
      } else {
        cache->buckets[bucket] = entry->next;
      }

      SNOBOL_LOG("dynamic_pattern_cache_remove: removed source='%s'", source);

      /* Free entry */
      if (entry->source) {
        snobol_free(entry->source);
      }
      if (entry->pattern) {
        dynamic_pattern_release(entry->pattern);
      }
      snobol_free(entry);

      cache->size--;
      return true;
    }
    prev = entry;
    entry = entry->next;
  }

  return false;
}

void dynamic_pattern_cache_clear(dynamic_pattern_cache_t *cache) {
  if (!cache) {
    return;
  }

  SNOBOL_LOG("dynamic_pattern_cache_clear: cache=%p size=%zu", (void *)cache,
             cache->size);

  /* Free all entries */
  for (size_t i = 0; i < cache->bucket_count; i++) {
    dynamic_pattern_cache_entry_t *entry = cache->buckets[i];
    while (entry) {
      dynamic_pattern_cache_entry_t *next = entry->next;

      if (entry->source) {
        snobol_free(entry->source);
      }
      if (entry->pattern) {
        dynamic_pattern_release(entry->pattern);
      }
      snobol_free(entry);

      entry = next;
    }
    cache->buckets[i] = nullptr;
  }

  cache->size = 0;
}

void dynamic_pattern_cache_stats(const dynamic_pattern_cache_t *cache,
                                 size_t *out_size, size_t *out_max) {
  if (cache) {
    if (out_size) {
      *out_size = cache->size;
    }
    if (out_max) {
      *out_max = cache->max_size;
    }
  } else {
    if (out_size) {
      *out_size = 0;
    }
    if (out_max) {
      *out_max = 0;
    }
  }
}
