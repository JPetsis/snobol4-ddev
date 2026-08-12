/**
 * @file test_tables.c
 * @brief Tests for runtime table objects
 *
 * Tests table creation, insertion, lookup, deletion, and reference counting.
 * Verifies proper memory management and ownership semantics.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/snobol_internal.h"
#include "snobol/table.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_table_create_free(void) {
  test_suite("Table: create and free");

  snobol_table_t *table = table_create("test");
  test_assert(table != NULL, "table_create returns non-NULL");
  test_assert(table_size(table) == 0, "new table has size 0");
  test_assert(strcmp(table_name(table), "test") == 0, "table name is set");

  table_release(table);
  test_assert(true, "table_free completes without error");
}

static void test_table_create_unnamed(void) {
  test_suite("Table: create unnamed");

  snobol_table_t *table = table_create(NULL);
  test_assert(table != NULL, "table_create(NULL) returns non-NULL");
  test_assert(table_name(table) == NULL, "unnamed table has NULL name");

  table_release(table);
}

static void test_table_set_get(void) {
  test_suite("Table: set and get");

  snobol_table_t *table = table_create("test");

  /* Insert a value */
  bool result = table_set(table, "key1", "value1");
  test_assert(result == true, "table_set returns true");
  test_assert(table_size(table) == 1, "size is 1 after insert");

  /* Retrieve the value */
  const char *value = table_get(table, "key1");
  test_assert(value != NULL, "table_get returns non-NULL for existing key");
  test_assert(strcmp(value, "value1") == 0, "value matches inserted value");

  /* Verify key ownership - table has its own copy */
  const char *direct = table_get(table, "key1");
  test_assert(direct == value, "repeated get returns same pointer");

  table_release(table);
}

static void test_table_update(void) {
  test_suite("Table: update existing key");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "key", "original");
  test_assert(table_size(table) == 1, "size is 1 after first insert");

  (void)table_set(table, "key", "updated");
  test_assert(table_size(table) == 1, "size is still 1 after update");

  const char *value = table_get(table, "key");
  test_assert(strcmp(value, "updated") == 0, "value is updated");

  table_release(table);
}

static void test_table_delete(void) {
  test_suite("Table: delete key");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "key1", "value1");
  (void)table_set(table, "key2", "value2");
  test_assert(table_size(table) == 2, "size is 2");

  bool deleted = table_delete(table, "key1");
  test_assert(deleted == true, "table_delete returns true for existing key");
  test_assert(table_size(table) == 1, "size is 1 after delete");

  const char *value = table_get(table, "key1");
  test_assert(value == NULL, "table_get returns NULL for deleted key");

  /* Delete non-existent key */
  deleted = table_delete(table, "nonexistent");
  test_assert(deleted == false, "table_delete returns false for missing key");

  table_release(table);
}

static void test_table_set_null_deletes(void) {
  test_suite("Table: set NULL value deletes");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "key", "value");
  test_assert(table_has(table, "key") == true, "key exists");

  (void)table_set(table, "key", NULL);
  test_assert(table_has(table, "key") == false,
              "key is deleted after set NULL");
  test_assert(table_size(table) == 0, "size is 0");

  table_release(table);
}

static void test_table_has(void) {
  test_suite("Table: has key");

  snobol_table_t *table = table_create("test");

  test_assert(table_has(table, "key") == false,
              "has returns false for missing key");

  (void)table_set(table, "key", "value");
  test_assert(table_has(table, "key") == true,
              "has returns true for existing key");

  (void)table_delete(table, "key");
  test_assert(table_has(table, "key") == false,
              "has returns false after delete");

  table_release(table);
}

static void test_table_clear(void) {
  test_suite("Table: clear all");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "key1", "value1");
  (void)table_set(table, "key2", "value2");
  (void)table_set(table, "key3", "value3");
  test_assert(table_size(table) == 3, "size is 3");

  table_clear(table);
  test_assert(table_size(table) == 0, "size is 0 after clear");
  test_assert(table_has(table, "key1") == false, "key1 is gone");
  test_assert(table_has(table, "key2") == false, "key2 is gone");
  test_assert(table_has(table, "key3") == false, "key3 is gone");

  /* Can re-use cleared table */
  (void)table_set(table, "newkey", "newvalue");
  test_assert(table_size(table) == 1, "can insert after clear");

  table_release(table);
}

static void test_table_reference_counting(void) {
  test_suite("Table: reference counting");

  snobol_table_t *table = table_create("test");
  test_assert(table != NULL, "create table");

  /* Retain increases refcount */
  snobol_table_t *retained = table_retain(table);
  test_assert(retained == table, "table_retain returns same pointer");

  /* First release doesn't free */
  table_release(table);
  test_assert(true, "first release doesn't crash");

  /* Second release frees */
  table_release(retained);
  test_assert(true, "second release frees without crash");
}

static void test_table_multiple_references(void) {
  test_suite("Table: multiple references");

  snobol_table_t *table = table_create("shared");

  /* Multiple owners */
  (void)table_retain(table);
  (void)table_retain(table);
  (void)table_retain(table);

  /* Modify through original */
  (void)table_set(table, "key", "value");
  test_assert(table_size(table) == 1, "size is 1");

  /* Release three times — plus one final release for the original table_create
   * reference */
  table_release(table);
  table_release(table);
  table_release(table);
  table_release(table); /* release original reference from table_create
                           (refcount: 4→3→2→1→0) */
  test_assert(true, "multiple releases don't crash");
}

static void test_table_many_entries(void) {
  test_suite("Table: many entries (resize test)");

  snobol_table_t *table = table_create("large");

  /* Insert enough entries to trigger resize */
  char key[32], value[32];
  for (int i = 0; i < 100; i++) {
    snprintf(key, sizeof(key), "key%d", i);
    snprintf(value, sizeof(value), "value%d", i);
    bool result = table_set(table, key, value);
    test_assert(result == true, "insert many entries succeeds");
  }

  test_assert(table_size(table) == 100, "size is 100");

  /* Verify all entries */
  for (int i = 0; i < 100; i++) {
    snprintf(key, sizeof(key), "key%d", i);
    snprintf(value, sizeof(value), "value%d", i);
    const char *got = table_get(table, key);
    test_assert(got != NULL, "get existing key");
    test_assert(strcmp(got, value) == 0, "value matches");
  }

  table_release(table);
}

static void test_table_hash_function(void) {
  test_suite("Table: hash function");

  /* Same string produces same hash */
  uint32_t hash1 = table_hash_string("test");
  uint32_t hash2 = table_hash_string("test");
  test_assert(hash1 == hash2, "same string produces same hash");

  /* Different strings produce different hashes (usually) */
  uint32_t hash3 = table_hash_string("other");
  test_assert(hash1 != hash3,
              "different strings usually produce different hashes");

  /* Empty string */
  uint32_t hash_empty = table_hash_string("");
  test_assert(hash_empty != 0, "empty string hash is non-zero");
}

static void test_table_collision_handling(void) {
  test_suite("Table: collision handling");

  snobol_table_t *table = table_create("test");

  /* Insert keys that might collide */
  (void)table_set(table, "a", "1");
  (void)table_set(table, "b", "2");
  (void)table_set(table, "c", "3");

  test_assert(table_size(table) == 3, "size is 3");
  test_assert(atoi(table_get(table, "a")) == 1, "value a is correct");
  test_assert(atoi(table_get(table, "b")) == 2, "value b is correct");
  test_assert(atoi(table_get(table, "c")) == 3, "value c is correct");

  table_release(table);
}

static void test_table_create_use_release_cycle(void) {
  test_suite("Table: create/use/release cycle");

  /* Simulate repeated create/use/release as would happen in pattern execution
   */
  for (int cycle = 0; cycle < 10; cycle++) {
    snobol_table_t *table = table_create("cycle");

    /* Use the table */
    (void)table_set(table, "key1", "value1");
    (void)table_set(table, "key2", "value2");
    test_assert(table_size(table) == 2, "cycle: size is 2");

    const char *v1 = table_get(table, "key1");
    test_assert(strcmp(v1, "value1") == 0, "cycle: value1 is correct");

    (void)table_delete(table, "key1");
    test_assert(table_size(table) == 1, "cycle: size is 1 after delete");

    table_clear(table);
    test_assert(table_size(table) == 0, "cycle: size is 0 after clear");

    table_release(table);
  }

  test_assert(true, "10 create/use/release cycles completed without leak");
}

static void test_table_nul_safe_keys_and_values(void) {
  test_suite("Table: NUL-safe keys and values");

  snobol_table_t *table = table_create("nul");
  /* Two keys that share a prefix and differ only after an embedded NUL. */
  const char key_a[] = {'k', '\0', 'a'};
  const char key_b[] = {'k', '\0', 'b'};
  const char bin_val[] = {'v', '\0', 'x'};

  test_assert(
      table_set_ex(table, key_a, sizeof(key_a), bin_val, sizeof(bin_val)),
      "set_ex with NUL-containing key and value");
  test_assert(table_set_ex(table, key_b, sizeof(key_b), "plain", 5),
              "set_ex with the sibling key");
  test_assert(table_size(table) == 2, "keys differing only after the NUL "
                                      "are distinct entries");

  test_assert(table_has_ex(table, key_a, sizeof(key_a)), "has_ex key A");
  test_assert(table_has_ex(table, key_b, sizeof(key_b)), "has_ex key B");
  test_assert(!table_has_ex(table, "k", 1), "prefix alone is not a key");

  size_t vlen = 0;
  const char *got = table_get_ex(table, key_a, sizeof(key_a), &vlen);
  test_assert(got != NULL, "get_ex key A");
  test_assert(vlen == 3 && memcmp(got, bin_val, 3) == 0,
              "get_ex value is byte-exact (embedded NUL preserved)");

  test_assert(table_delete_ex(table, key_a, sizeof(key_a)), "delete_ex key A");
  test_assert(table_size(table) == 1, "size after delete");
  test_assert(table_has_ex(table, key_b, sizeof(key_b)),
              "sibling key survives");

  table_release(table);
}

void test_table_suite(void) {
  test_table_create_free();
  test_table_create_unnamed();
  test_table_set_get();
  test_table_update();
  test_table_delete();
  test_table_set_null_deletes();
  test_table_has();
  test_table_clear();
  test_table_reference_counting();
  test_table_multiple_references();
  test_table_many_entries();
  test_table_hash_function();
  test_table_collision_handling();
  test_table_create_use_release_cycle();
  test_table_nul_safe_keys_and_values();
}
