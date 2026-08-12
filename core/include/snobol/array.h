#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file array.h
 * @brief Runtime-owned sparse array objects for SNOBOL4
 *
 * Provides an integer-keyed sparse array with get/set/delete/size operations.
 * Arrays use reference counting for safe sharing across pattern executions.
 * Keys are int32_t; values are strings (copied on insert).
 *
 * Ownership/Lifetime Rules:
 * - Arrays are created with snobol_array_create() and must be freed with
 * snobol_array_release()
 * - Arrays use reference counting for safe sharing
 * - When reference count reaches 0, the array and all entries are freed
 * - A NULL value indicates a deleted entry (tombstone)
 */

#include "snobol/snobol_attrs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Initial capacity for a new array. */
#define ARRAY_INITIAL_CAPACITY 16

/**
 * @brief A single entry in the sparse array.
 */
typedef struct array_entry {
  int32_t key;   /**< Integer key (1-based by convention). */
  char *value;   /**< Owned string value (NULL = tombstone). */
  size_t value_len; /**< Byte length of value (0 when value is NULL). */
  uint32_t hash; /**< Pre-computed hash of key. */
  bool active;   /**< Entry is active (not deleted). */
} array_entry_t;

/**
 * @brief Sparse array state.
 */
typedef struct snobol_array {
  array_entry_t *entries; /**< Hash table entries. */
  size_t capacity;        /**< Allocated capacity. */
  size_t size;            /**< Number of active entries. */
  size_t tombstones;      /**< Number of deleted (tombstone) entries. */
  uint32_t refcount;      /**< Reference count for safe sharing. */
  int32_t bounds_hint;    /**< Hint for expected key range (may be 0). */
} snobol_array_t;

/**
 * @brief Create a new sparse array.
 * @param bounds_hint Expected key range hint (may be 0).
 * @return New array, or NULL on allocation failure.
 */
SNOBOL_NODISCARD snobol_array_t *snobol_array_create(int32_t bounds_hint);

/**
 * @brief Retain (increment refcount) an array.
 * @param array Array to retain.
 * @return The same array pointer.
 */
SNOBOL_NODISCARD snobol_array_t *snobol_array_retain(snobol_array_t *array);

/**
 * @brief Release (decrement refcount) an array; frees when refcount reaches 0.
 * @param array Array to release (NULL is safe).
 */
void snobol_array_release(snobol_array_t *array);

/**
 * @brief Set a value at the given key.
 * @param array Target array.
 * @param key   Integer key.
 * @param value NUL-terminated string value (copied internally).
 * @return true on success, false on allocation failure.
 */
SNOBOL_NODISCARD bool snobol_array_set(snobol_array_t *array, int32_t key,
                                       const char *value);

/**
 * @brief Get the value at a given key.
 * @param array Target array.
 * @param key   Integer key.
 * @return Pointer to internal value string (owned by array, do not free),
 *         or NULL if the key is not set or is a tombstone.
 */
const char *snobol_array_get(const snobol_array_t *array, int32_t key);

/**
 * @brief Set a value at the given key (NUL-safe byte-exact variant)
 * @param array Target array.
 * @param key   Integer key.
 * @param value String value (copied internally), or NULL to delete.
 * @param value_len Byte length of value (may contain NULs).
 * @return true on success, false on allocation failure.
 */
SNOBOL_NODISCARD bool snobol_array_set_ex(snobol_array_t *array, int32_t key,
                                          const char *value, size_t value_len);

/**
 * @brief Get the value at a given key (NUL-safe)
 * @param array Target array.
 * @param key   Integer key.
 * @param out_len Set to the byte length of the returned value (may be NULL).
 * @return Pointer to internal value string (owned by array, do not free),
 *         or NULL if the key is not set or is a tombstone.
 */
const char *snobol_array_get_ex(const snobol_array_t *array, int32_t key,
                                size_t *out_len);

/**
 * @brief Check whether a key has an active entry.
 * @param array Target array.
 * @param key   Integer key.
 * @return true if the key exists and is not a tombstone.
 */
bool snobol_array_has(const snobol_array_t *array, int32_t key);

/**
 * @brief Delete the entry at a given key (marks as tombstone).
 * @param array Target array.
 * @param key   Integer key.
 * @return true on success, false if the key was not found.
 */
SNOBOL_NODISCARD bool snobol_array_delete(snobol_array_t *array, int32_t key);

/**
 * @brief Clear all entries from the array (resets to empty).
 * @param array Target array.
 */
void snobol_array_clear(snobol_array_t *array);

/**
 * @brief Return the number of active entries.
 * @param array Target array.
 * @return Active entry count.
 */
size_t snobol_array_size(const snobol_array_t *array);

/**
 * @brief Get all active keys.
 * @param  array     Target array.
 * @param  out_count Set to the number of keys returned.
 * @return Malloc'd array of keys (caller must free), or NULL on error.
 */
int32_t *snobol_array_keys(const snobol_array_t *array, size_t *out_count);

/**
 * @brief Get all active values.
 * @param  array     Target array.
 * @param  out_count Set to the number of values returned.
 * @return Malloc'd array of value strings (caller must free), or NULL on error.
 */
char **snobol_array_values(const snobol_array_t *array, size_t *out_count);

#ifdef __cplusplus
}
#endif
