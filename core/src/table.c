/**
 * @file table.c
 * @brief Runtime-owned associative table implementation
 *
 * Implements string-keyed hash tables with reference counting for the SNOBOL4
 * runtime. Tables are owned by the C core and can be safely shared across
 * pattern executions.
 */

#include "snobol/table.h"
#include "snobol/snobol_internal.h"
#include <stdint.h>
#include <string.h>

/**
 * FNV-1a hash function for strings
 * Constants for 32-bit FNV-1a
 */
#ifndef SNOBOL_FNV_CONSTANTS_DEFINED
#define SNOBOL_FNV_CONSTANTS_DEFINED
#define FNV_OFFSET_BASIS 2166136261u
#define FNV_PRIME 16777619u
#endif

uint32_t table_hash_string(const char *str) {
  return table_hash_bytes(str, str ? strlen(str) : 0);
}

uint32_t table_hash_bytes(const char *str, size_t len) {
  uint32_t hash = FNV_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    hash ^= (uint8_t)str[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

/**
 * Internal: resize hash table when load factor exceeds threshold
 */
static bool table_resize(snobol_table_t *table, size_t new_capacity) {
  table_entry_t *old_entries = table->entries;
  size_t old_capacity = table->capacity;

  /* Allocate new entry array */
  table_entry_t *new_entries =
      (table_entry_t *)snobol_calloc(new_capacity, sizeof(table_entry_t));
  if (!new_entries) {
    return false;
  }

  /* Rehash all active entries */
  for (size_t i = 0; i < old_capacity; i++) {
    table_entry_t *entry = &old_entries[i];
    if (entry->active && entry->value != nullptr) {
      /* Find slot in new table */
      uint32_t mask = (uint32_t)(new_capacity - 1);
      uint32_t idx = entry->hash & mask;

      /* Linear probing */
      while (new_entries[idx].active) {
        idx = (idx + 1) & mask;
      }

      /* Copy entry */
      new_entries[idx] = *entry;
    }
  }

  /* Update table */
  table->entries = new_entries;
  table->capacity = new_capacity;
  table->tombstones = 0; /* Tombstones are removed during resize */

  /* Free old array (but not the strings, which were copied) */
  if (old_entries) {
    snobol_free(old_entries);
  }

  return true;
}

/**
 * Internal: check if resize is needed
 */
static bool table_maybe_resize(snobol_table_t *table) {
  /* Resize if load factor > 0.75 or too many tombstones */
  size_t threshold = (table->capacity * 3) / 4;
  if (table->size + table->tombstones > threshold) {
    return table_resize(table, table->capacity * 2);
  }
  return true;
}

snobol_table_t *table_create(const char *name) {
  snobol_table_t *table =
      (snobol_table_t *)snobol_calloc(1, sizeof(snobol_table_t));
  if (!table) {
    return nullptr;
  }

  table->entries = (table_entry_t *)snobol_calloc(TABLE_INITIAL_CAPACITY,
                                                  sizeof(table_entry_t));
  if (!table->entries) {
    snobol_free(table);
    return nullptr;
  }

  table->capacity = TABLE_INITIAL_CAPACITY;
  table->size = 0;
  table->tombstones = 0;
  table->refcount = 1;
  table->name = nullptr;

  if (name) {
    table->name = (char *)snobol_malloc(strlen(name) + 1);
    if (table->name) {
      strcpy(table->name, name);
    }
  }

  SNOBOL_LOG("table_create: table=%p name=%s", (void *)table,
             name ? name : "(unnamed)");
  return table;
}

snobol_table_t *table_retain(snobol_table_t *table) {
  if (!table) {
    return nullptr;
  }
  table->refcount++;
  SNOBOL_LOG("table_retain: table=%p refcount=%u", (void *)table,
             table->refcount);
  return table;
}

void table_release(snobol_table_t *table) {
  if (!table) {
    return;
  }

  table->refcount--;
  SNOBOL_LOG("table_release: table=%p refcount=%u", (void *)table,
             table->refcount);

  if (table->refcount > 0) {
    return;
  }

  /* Free all entries */
  for (size_t i = 0; i < table->capacity; i++) {
    table_entry_t *entry = &table->entries[i];
    if (entry->active) {
      if (entry->key)
        snobol_free(entry->key);
      if (entry->value)
        snobol_free(entry->value);
    }
  }

  /* Free entry array */
  if (table->entries) {
    snobol_free(table->entries);
  }

  /* Free name */
  if (table->name) {
    snobol_free(table->name);
  }

  SNOBOL_LOG("table_release: freeing table=%p", (void *)table);
  snobol_free(table);
}

bool table_set(snobol_table_t *table, const char *key, const char *value) {
  return table_set_ex(table, key, key ? strlen(key) : 0, value,
                      value ? strlen(value) : 0);
}

bool table_set_ex(snobol_table_t *table, const char *key, size_t key_len,
                  const char *value, size_t value_len) {
  if (!table || !key) {
    return false;
  }

  SNOBOL_LOG("table_set_ex: table=%p key='%.*s' value='%s'", (void *)table,
             (int)key_len, key, value ? value : "(null)");

  /* Resize if needed */
  if (!table_maybe_resize(table)) {
    return false;
  }

  uint32_t hash = table_hash_bytes(key, key_len);
  uint32_t mask = (uint32_t)(table->capacity - 1);
  uint32_t idx = hash & mask;

  /* Linear probing */
  size_t tombstone_idx = SIZE_MAX;
  while (table->entries[idx].active) {
    if (table->entries[idx].hash == hash && table->entries[idx].key &&
        table->entries[idx].key_len == key_len &&
        memcmp(table->entries[idx].key, key, key_len) == 0) {
      /* Key found - update or delete */
      if (value) {
        /* Update value */
        char *new_value = (char *)snobol_malloc(value_len + 1);
        if (!new_value) {
          return false;
        }
        if (table->entries[idx].value) {
          snobol_free(table->entries[idx].value);
        }
        memcpy(new_value, value, value_len);
        new_value[value_len] = '\0';
        table->entries[idx].value = new_value;
        table->entries[idx].value_len = value_len;
      } else {
        /* Delete entry (tombstone) */
        snobol_free(table->entries[idx].key);
        snobol_free(table->entries[idx].value);
        table->entries[idx].key = nullptr;
        table->entries[idx].key_len = 0;
        table->entries[idx].value = nullptr;
        table->entries[idx].value_len = 0;
        table->entries[idx].active = false;
        table->size--;
        table->tombstones++;
      }
      return true;
    }

    /* Remember first tombstone for insertion */
    if (!table->entries[idx].value && tombstone_idx == SIZE_MAX) {
      tombstone_idx = idx;
    }

    idx = (idx + 1) & mask;
  }

  /* Insert at empty slot or tombstone */
  size_t insert_idx = (tombstone_idx != SIZE_MAX) ? tombstone_idx : idx;

  /* Copy key */
  char *key_copy = (char *)snobol_malloc(key_len + 1);
  if (!key_copy) {
    return false;
  }
  memcpy(key_copy, key, key_len);
  key_copy[key_len] = '\0';

  /* Copy value if provided */
  char *value_copy = nullptr;
  if (value) {
    value_copy = (char *)snobol_malloc(value_len + 1);
    if (!value_copy) {
      snobol_free(key_copy);
      return false;
    }
    memcpy(value_copy, value, value_len);
    value_copy[value_len] = '\0';
  }

  /* Insert entry */
  table_entry_t *entry = &table->entries[insert_idx];
  entry->key = key_copy;
  entry->key_len = key_len;
  entry->value = value_copy;
  entry->value_len = value ? value_len : 0;
  entry->hash = hash;
  entry->active = true;

  if (tombstone_idx != SIZE_MAX) {
    table->tombstones--;
  }
  table->size++;

  return true;
}

const char *table_get(const snobol_table_t *table, const char *key) {
  return table_get_ex(table, key, key ? strlen(key) : 0, nullptr);
}

const char *table_get_ex(const snobol_table_t *table, const char *key,
                         size_t key_len, size_t *out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (!table || !key) {
    return nullptr;
  }

  uint32_t hash = table_hash_bytes(key, key_len);
  uint32_t mask = (uint32_t)(table->capacity - 1);
  uint32_t idx = hash & mask;

  /* Linear probing */
  while (table->entries[idx].active) {
    if (table->entries[idx].hash == hash && table->entries[idx].key &&
        table->entries[idx].key_len == key_len &&
        memcmp(table->entries[idx].key, key, key_len) == 0) {
      if (out_len) {
        *out_len = table->entries[idx].value_len;
      }
      return table->entries[idx].value;
    }
    idx = (idx + 1) & mask;
  }

  return nullptr;
}

bool table_has(const snobol_table_t *table, const char *key) {
  return table_has_ex(table, key, key ? strlen(key) : 0);
}

bool table_has_ex(const snobol_table_t *table, const char *key,
                  size_t key_len) {
  return table_get_ex(table, key, key_len, nullptr) != nullptr;
}

bool table_delete(snobol_table_t *table, const char *key) {
  return table_delete_ex(table, key, key ? strlen(key) : 0);
}

bool table_delete_ex(snobol_table_t *table, const char *key, size_t key_len) {
  if (!table || !key) {
    return false;
  }

  uint32_t hash = table_hash_bytes(key, key_len);
  uint32_t mask = (uint32_t)(table->capacity - 1);
  uint32_t idx = hash & mask;

  /* Linear probing */
  while (table->entries[idx].active) {
    if (table->entries[idx].hash == hash && table->entries[idx].key &&
        table->entries[idx].key_len == key_len &&
        memcmp(table->entries[idx].key, key, key_len) == 0) {
      /* Found - mark as tombstone */
      snobol_free(table->entries[idx].key);
      snobol_free(table->entries[idx].value);
      table->entries[idx].key = nullptr;
      table->entries[idx].key_len = 0;
      table->entries[idx].value = nullptr;
      table->entries[idx].value_len = 0;
      table->entries[idx].active = false;
      table->size--;
      table->tombstones++;
      return true;
    }
    idx = (idx + 1) & mask;
  }

  return false;
}

void table_clear(snobol_table_t *table) {
  if (!table) {
    return;
  }

  for (size_t i = 0; i < table->capacity; i++) {
    table_entry_t *entry = &table->entries[i];
    if (entry->active) {
      if (entry->key)
        snobol_free(entry->key);
      if (entry->value)
        snobol_free(entry->value);
      entry->key = nullptr;
      entry->value = nullptr;
      entry->active = false;
    }
  }

  table->size = 0;
  table->tombstones = 0;
}

size_t table_size(const snobol_table_t *table) {
  return table ? table->size : 0;
}

const char *table_name(const snobol_table_t *table) {
  return table ? table->name : nullptr;
}
