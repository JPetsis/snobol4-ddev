#include "snobol/array.h"
#include "snobol/snobol_internal.h"
#include <stdint.h>
#include <string.h>

static uint32_t array_hash_int(int32_t key) {
  return (uint32_t)key * 2654435761u;
}

static bool array_resize(snobol_array_t *array, size_t new_capacity) {
  array_entry_t *old_entries = array->entries;
  size_t old_capacity = array->capacity;

  array_entry_t *new_entries =
      (array_entry_t *)snobol_calloc(new_capacity, sizeof(array_entry_t));
  if (!new_entries) {
    return false;
  }

  for (size_t i = 0; i < old_capacity; i++) {
    array_entry_t *entry = &old_entries[i];
    if (entry->active && entry->value != nullptr) {
      uint32_t mask = (uint32_t)(new_capacity - 1);
      uint32_t idx = entry->hash & mask;

      while (new_entries[idx].active) {
        idx = (idx + 1) & mask;
      }

      new_entries[idx] = *entry;
    }
  }

  array->entries = new_entries;
  array->capacity = new_capacity;
  array->tombstones = 0;

  if (old_entries) {
    snobol_free(old_entries);
  }

  return true;
}

static bool array_maybe_resize(snobol_array_t *array) {
  size_t threshold = (array->capacity * 3) / 4;
  if (array->size + array->tombstones > threshold) {
    return array_resize(array, array->capacity * 2);
  }
  return true;
}

snobol_array_t *snobol_array_create(int32_t bounds_hint) {
  snobol_array_t *array =
      (snobol_array_t *)snobol_calloc(1, sizeof(snobol_array_t));
  if (!array) {
    return nullptr;
  }

  size_t cap = ARRAY_INITIAL_CAPACITY;
  if (bounds_hint > 0 && (size_t)bounds_hint > cap) {
    cap = 1;
    while (cap < (size_t)bounds_hint) {
      cap *= 2;
    }
  }

  array->entries = (array_entry_t *)snobol_calloc(cap, sizeof(array_entry_t));
  if (!array->entries) {
    snobol_free(array);
    return nullptr;
  }

  array->capacity = cap;
  array->size = 0;
  array->tombstones = 0;
  array->refcount = 1;
  array->bounds_hint = bounds_hint > 0 ? bounds_hint : 0;

  SNOBOL_LOG("snobol_array_create: array=%p bounds_hint=%d", (void *)array,
             bounds_hint);
  return array;
}

snobol_array_t *snobol_array_retain(snobol_array_t *array) {
  if (!array) {
    return nullptr;
  }
  array->refcount++;
  SNOBOL_LOG("snobol_array_retain: array=%p refcount=%u", (void *)array,
             array->refcount);
  return array;
}

void snobol_array_release(snobol_array_t *array) {
  if (!array) {
    return;
  }

  array->refcount--;
  SNOBOL_LOG("snobol_array_release: array=%p refcount=%u", (void *)array,
             array->refcount);

  if (array->refcount > 0) {
    return;
  }

  for (size_t i = 0; i < array->capacity; i++) {
    array_entry_t *entry = &array->entries[i];
    if (entry->active && entry->value) {
      snobol_free(entry->value);
    }
  }

  if (array->entries) {
    snobol_free(array->entries);
  }

  SNOBOL_LOG("snobol_array_release: freeing array=%p", (void *)array);
  snobol_free(array);
}

bool snobol_array_set(snobol_array_t *array, int32_t key, const char *value) {
  return snobol_array_set_ex(array, key, value, value ? strlen(value) : 0);
}

bool snobol_array_set_ex(snobol_array_t *array, int32_t key, const char *value,
                         size_t value_len) {
  if (!array) {
    return false;
  }

  SNOBOL_LOG("snobol_array_set_ex: array=%p key=%d value='%.*s'", (void *)array,
             key, (int)value_len, value ? value : "(null)");

  if (!array_maybe_resize(array)) {
    return false;
  }

  uint32_t hash = array_hash_int(key);
  uint32_t mask = (uint32_t)(array->capacity - 1);
  uint32_t idx = hash & mask;

  size_t tombstone_idx = SIZE_MAX;
  while (array->entries[idx].active) {
    if (array->entries[idx].hash == hash && array->entries[idx].key == key) {
      if (value) {
        char *new_value = (char *)snobol_malloc(value_len + 1);
        if (!new_value) {
          return false;
        }
        if (array->entries[idx].value) {
          snobol_free(array->entries[idx].value);
        }
        memcpy(new_value, value, value_len);
        new_value[value_len] = '\0';
        array->entries[idx].value = new_value;
        array->entries[idx].value_len = value_len;
      } else {
        if (array->entries[idx].value) {
          snobol_free(array->entries[idx].value);
        }
        array->entries[idx].key = 0;
        array->entries[idx].value = nullptr;
        array->entries[idx].value_len = 0;
        array->entries[idx].active = false;
        array->size--;
        array->tombstones++;
      }
      return true;
    }

    if (!array->entries[idx].value && tombstone_idx == SIZE_MAX) {
      tombstone_idx = idx;
    }

    idx = (idx + 1) & mask;
  }

  size_t insert_idx = (tombstone_idx != SIZE_MAX) ? tombstone_idx : idx;

  char *value_copy = nullptr;
  if (value) {
    value_copy = (char *)snobol_malloc(value_len + 1);
    if (!value_copy) {
      return false;
    }
    memcpy(value_copy, value, value_len);
    value_copy[value_len] = '\0';
  }

  array_entry_t *entry = &array->entries[insert_idx];
  entry->key = key;
  entry->value = value_copy;
  entry->value_len = value ? value_len : 0;
  entry->hash = hash;
  entry->active = true;

  if (tombstone_idx != SIZE_MAX) {
    array->tombstones--;
  }
  array->size++;

  return true;
}

const char *snobol_array_get(const snobol_array_t *array, int32_t key) {
  return snobol_array_get_ex(array, key, nullptr);
}

const char *snobol_array_get_ex(const snobol_array_t *array, int32_t key,
                                size_t *out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (!array) {
    return nullptr;
  }

  uint32_t hash = array_hash_int(key);
  uint32_t mask = (uint32_t)(array->capacity - 1);
  uint32_t idx = hash & mask;

  while (array->entries[idx].active) {
    if (array->entries[idx].hash == hash && array->entries[idx].key == key) {
      if (out_len) {
        *out_len = array->entries[idx].value_len;
      }
      return array->entries[idx].value;
    }
    idx = (idx + 1) & mask;
  }

  return nullptr;
}

bool snobol_array_has(const snobol_array_t *array, int32_t key) {
  return snobol_array_get(array, key) != nullptr;
}

bool snobol_array_delete(snobol_array_t *array, int32_t key) {
  if (!array) {
    return false;
  }

  uint32_t hash = array_hash_int(key);
  uint32_t mask = (uint32_t)(array->capacity - 1);
  uint32_t idx = hash & mask;

  while (array->entries[idx].active) {
    if (array->entries[idx].hash == hash && array->entries[idx].key == key) {
      if (array->entries[idx].value) {
        snobol_free(array->entries[idx].value);
      }
      array->entries[idx].key = 0;
      array->entries[idx].value = nullptr;
      array->entries[idx].value_len = 0;
      array->entries[idx].active = false;
      array->size--;
      array->tombstones++;
      return true;
    }
    idx = (idx + 1) & mask;
  }

  return false;
}

void snobol_array_clear(snobol_array_t *array) {
  if (!array) {
    return;
  }

  for (size_t i = 0; i < array->capacity; i++) {
    array_entry_t *entry = &array->entries[i];
    if (entry->active) {
      if (entry->value) {
        snobol_free(entry->value);
      }
      entry->key = 0;
      entry->value = nullptr;
      entry->value_len = 0;
      entry->active = false;
    }
  }

  array->size = 0;
  array->tombstones = 0;
}

size_t snobol_array_size(const snobol_array_t *array) {
  return array ? array->size : 0;
}

int32_t *snobol_array_keys(const snobol_array_t *array, size_t *out_count) {
  if (!array || !out_count) {
    if (out_count)
      *out_count = 0;
    return nullptr;
  }

  int32_t *keys = (int32_t *)snobol_malloc(array->size * sizeof(int32_t));
  if (!keys) {
    *out_count = 0;
    return nullptr;
  }

  size_t count = 0;
  for (size_t i = 0; i < array->capacity; i++) {
    if (array->entries[i].active && array->entries[i].value) {
      keys[count++] = array->entries[i].key;
    }
  }

  *out_count = count;
  return keys;
}

char **snobol_array_values(const snobol_array_t *array, size_t *out_count) {
  if (!array || !out_count) {
    if (out_count)
      *out_count = 0;
    return nullptr;
  }

  char **values = (char **)snobol_malloc(array->size * sizeof(char *));
  if (!values) {
    *out_count = 0;
    return nullptr;
  }

  size_t count = 0;
  for (size_t i = 0; i < array->capacity; i++) {
    if (array->entries[i].active && array->entries[i].value) {
      values[count] = (char *)snobol_malloc(array->entries[i].value_len + 1);
      if (values[count]) {
        memcpy(values[count], array->entries[i].value,
               array->entries[i].value_len);
        values[count][array->entries[i].value_len] = '\0';
      }
      count++;
    }
  }

  *out_count = count;
  return values;
}
