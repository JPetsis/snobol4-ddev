#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include "snobol/snobol_attrs.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file table.h
 * @brief Runtime-owned associative table objects for SNOBOL4
 *
 * Ownership/Lifetime Rules:
 * - Tables are created with table_create() and must be freed with
 * table_release()
 * - Tables use reference counting for safe sharing across pattern executions
 * - When reference count reaches 0, the table and all entries are freed
 * - Table keys are strings (copied on insert)
 * - Table values are strings (copied on insert)
 * - NULL value indicates a deleted entry (tombstone)
 */

/* Initial hash table capacity */
#define TABLE_INITIAL_CAPACITY 16

/* Hash table entry */
typedef struct table_entry {
  char *key;     /* Owned: must be freed (NUL-safe: key_len bytes) */
  size_t key_len;   /* Byte length of key */
  char *value;   /* Owned: must be freed (NULL = tombstone) */
  size_t value_len; /* Byte length of value (0 when value is NULL) */
  uint32_t hash; /* Pre-computed hash of key */
  bool active;   /* Entry is active (not deleted) */
} table_entry_t;

/* Runtime table object */
typedef struct snobol_table {
  table_entry_t *entries; /* Hash table entries */
  size_t capacity;        /* Total slots in hash table */
  size_t size;            /* Number of active entries */
  size_t tombstones;      /* Number of deleted entries */
  uint32_t refcount;      /* Reference count for ownership */
  char *name;             /* Optional table name (for debugging) */
} snobol_table_t;

/**
 * @brief Create a new runtime table
 * @param name Optional name for debugging (can be NULL)
 * @return Pointer to new table, or NULL on failure
 *
 * Ownership: Caller owns the returned table and must call table_release()
 */
SNOBOL_NODISCARD snobol_table_t *table_create(const char *name);

/**
 * @brief Increment table reference count
 * @param table Table to retain
 * @return Same table pointer for convenience
 *
 * Use when sharing table ownership with another owner
 */
SNOBOL_NODISCARD snobol_table_t *table_retain(snobol_table_t *table);

/**
 * @brief Decrement table reference count and free if zero
 * @param table Table to release
 *
 * When refcount reaches 0, all entries and the table itself are freed
 */
void table_release(snobol_table_t *table);

/**
 * @brief Insert or update a key-value pair
 * @param table Table to modify
 * @param key String key (copied)
 * @param value String value (copied), or NULL to delete
 * @return true on success, false on allocation failure
 *
 * Ownership: Table takes ownership of copies of key and value
 */
SNOBOL_NODISCARD bool table_set(snobol_table_t *table, const char *key,
                                const char *value);

/**
 * @brief Look up a value by key
 * @param table Table to query
 * @param key Key to look up
 * @return Pointer to value string, or NULL if not found
 *
 * Ownership: Returned pointer is owned by the table, do not free
 * The pointer remains valid until the entry is modified or deleted
 */
const char *table_get(const snobol_table_t *table, const char *key);

/**
 * @brief Check if a key exists in the table
 * @param table Table to query
 * @param key Key to check
 * @return true if key exists with a non-NULL value
 */
bool table_has(const snobol_table_t *table, const char *key);

/**
 * @brief Delete a key from the table
 * @param table Table to modify
 * @param key Key to delete
 * @return true if key was found and deleted
 */
SNOBOL_NODISCARD bool table_delete(snobol_table_t *table, const char *key);

/**
 * @brief Insert or update a key-value pair (NUL-safe byte-exact variants)
 * @param table Table to modify
 * @param key String key (copied)
 * @param key_len Byte length of key (may contain NULs)
 * @param value String value (copied), or NULL to delete
 * @param value_len Byte length of value
 * @return true on success, false on allocation failure
 *
 * Ownership: Table takes ownership of copies of key and value
 */
SNOBOL_NODISCARD bool table_set_ex(snobol_table_t *table, const char *key,
                                   size_t key_len, const char *value,
                                   size_t value_len);

/**
 * @brief Look up a value by key (NUL-safe)
 * @param table Table to query
 * @param key Key to look up
 * @param key_len Byte length of key
 * @param out_len Set to the byte length of the returned value (may be NULL)
 * @return Pointer to value string, or NULL if not found
 *
 * Ownership: Returned pointer is owned by the table, do not free
 * The pointer remains valid until the entry is modified or deleted
 */
const char *table_get_ex(const snobol_table_t *table, const char *key,
                         size_t key_len, size_t *out_len);

/**
 * @brief Check if a key exists in the table (NUL-safe)
 * @param table Table to query
 * @param key Key to check
 * @param key_len Byte length of key
 * @return true if key exists with a non-NULL value
 */
bool table_has_ex(const snobol_table_t *table, const char *key, size_t key_len);

/**
 * @brief Delete a key from the table (NUL-safe)
 * @param table Table to modify
 * @param key Key to delete
 * @param key_len Byte length of key
 * @return true if key was found and deleted
 */
SNOBOL_NODISCARD bool table_delete_ex(snobol_table_t *table, const char *key,
                                      size_t key_len);

/**
 * @brief Hash function for strings (FNV-1a)
 * @param str String to hash
 * @return 32-bit hash value
 */
uint32_t table_hash_string(const char *str);

/**
 * @brief Hash function for byte-exact strings (FNV-1a, NUL-safe)
 * @param str Bytes to hash
 * @param len Byte length
 * @return 32-bit hash value
 */
uint32_t table_hash_bytes(const char *str, size_t len);

/**
 * @brief Clear all entries from the table
 * @param table Table to clear
 */
void table_clear(snobol_table_t *table);

/**
 * @brief Get the number of active entries
 * @param table Table to query
 * @return Number of active (non-deleted) entries
 */
size_t table_size(const snobol_table_t *table);

/**
 * @brief Get the table name (for debugging)
 * @param table Table to query
 * @return Table name or NULL if unnamed
 */
const char *table_name(const snobol_table_t *table);

#ifdef __cplusplus
}
#endif
