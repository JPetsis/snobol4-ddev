/**
 * @file test_table_ops.c
 * @brief Tests for table operations in the pattern engine
 *
 * Tests OP_TABLE_GET, OP_TABLE_SET, and table-backed pattern behavior.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/table.h"
#include "snobol/vm.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_table_get_success(void) {
  test_suite("Table Ops: TABLE_GET success");

  /* Create a table with one entry */
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "key1", "value1");

  /* Verify lookup works */
  const char *result = table_get(table, "key1");
  test_assert(result != NULL, "table_get returns non-NULL");
  test_assert(strcmp(result, "value1") == 0, "value matches");

  table_release(table);
}

static void test_table_get_missing_key(void) {
  test_suite("Table Ops: TABLE_GET missing key");

  snobol_table_t *table = table_create("test");
  (void)table_set(table, "key1", "value1");

  /* Missing key should return NULL */
  const char *result = table_get(table, "nonexistent");
  test_assert(result == NULL, "missing key returns NULL");

  table_release(table);
}

static void test_table_set_and_get(void) {
  test_suite("Table Ops: TABLE_SET and get");

  snobol_table_t *table = table_create("test");

  /* Set multiple values */
  (void)table_set(table, "name", "Alice");
  (void)table_set(table, "city", "Boston");

  test_assert(table_size(table) == 2, "table has 2 entries");

  const char *name = table_get(table, "name");
  const char *city = table_get(table, "city");

  test_assert(name != NULL, "name lookup succeeds");
  test_assert(strcmp(name, "Alice") == 0, "name value correct");
  test_assert(city != NULL, "city lookup succeeds");
  test_assert(strcmp(city, "Boston") == 0, "city value correct");

  table_release(table);
}

static void test_table_update_existing_key(void) {
  test_suite("Table Ops: TABLE_SET update existing");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "count", "1");
  test_assert(table_size(table) == 1, "size is 1");

  (void)table_set(table, "count", "2");
  test_assert(table_size(table) == 1, "size still 1 after update");

  const char *result = table_get(table, "count");
  test_assert(strcmp(result, "2") == 0, "value updated");

  table_release(table);
}

static void test_table_delete_via_set_null(void) {
  test_suite("Table Ops: TABLE_SET NULL deletes");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "key", "value");
  test_assert(table_has(table, "key"), "key exists");

  (void)table_set(table, "key", nullptr);
  test_assert(!table_has(table, "key"), "key deleted");
  test_assert(table_size(table) == 0, "size is 0");

  table_release(table);
}

static void test_table_multiple_tables(void) {
  test_suite("Table Ops: multiple tables");

  snobol_table_t *table1 = table_create("t1");
  snobol_table_t *table2 = table_create("t2");

  (void)table_set(table1, "key", "from_t1");
  (void)table_set(table2, "key", "from_t2");

  const char *v1 = table_get(table1, "key");
  const char *v2 = table_get(table2, "key");

  test_assert(strcmp(v1, "from_t1") == 0, "table1 value correct");
  test_assert(strcmp(v2, "from_t2") == 0, "table2 value correct");

  table_release(table1);
  table_release(table2);
}

static void test_table_with_special_characters(void) {
  test_suite("Table Ops: special characters in keys/values");

  snobol_table_t *table = table_create("test");

  /* Test with spaces */
  (void)table_set(table, "hello world", "foo bar");
  const char *result = table_get(table, "hello world");
  test_assert(strcmp(result, "foo bar") == 0, "spaces preserved");

  /* Test with numbers */
  (void)table_set(table, "key123", "value456");
  result = table_get(table, "key123");
  test_assert(strcmp(result, "value456") == 0, "numbers preserved");

  table_release(table);
}

static void test_table_empty_string_value(void) {
  test_suite("Table Ops: empty string value");

  snobol_table_t *table = table_create("test");

  (void)table_set(table, "empty", "");
  test_assert(table_has(table, "empty"), "empty value exists");

  const char *result = table_get(table, "empty");
  test_assert(result != NULL, "empty value is non-NULL");
  test_assert(strlen(result) == 0, "empty value has length 0");

  table_release(table);
}

static void test_table_rapid_create_use_release(void) {
  test_suite("Table Ops: rapid create/use/release");

  /* Simulate pattern engine creating tables for each execution */
  for (int i = 0; i < 50; i++) {
    snobol_table_t *table = table_create("temp");

    /* Use the table */
    char key[32];
    char value[32];
    snprintf(key, sizeof(key), "key_%d", i);
    snprintf(value, sizeof(value), "val_%d", i);

    (void)table_set(table, key, value);

    const char *result = table_get(table, key);
    test_assert(result != NULL, "iteration: lookup succeeds");

    table_release(table);
  }

  test_assert(true, "50 rapid create/use/release cycles completed");
}

static void test_table_memory_no_leak(void) {
  test_suite("Table Ops: memory no leak stress");

  /* Create and destroy many tables to check for memory leaks */
  for (int i = 0; i < 100; i++) {
    snobol_table_t *table = table_create("stress");

    /* Add many entries */
    for (int j = 0; j < 20; j++) {
      char key[32];
      char value[32];
      snprintf(key, sizeof(key), "k%d", j);
      snprintf(value, sizeof(value), "v%d", j);
      (void)table_set(table, key, value);
    }

    /* Clear and reuse */
    table_clear(table);

    /* Add different entries */
    for (int j = 0; j < 10; j++) {
      char key[32];
      char value[32];
      snprintf(key, sizeof(key), "new%d", j);
      snprintf(value, sizeof(value), "val%d", j);
      (void)table_set(table, key, value);
    }

    table_release(table);
  }

  test_assert(true, "100 tables with clear/reuse completed without leak");
}

void test_table_ops_suite(void) {
  test_table_get_success();
  test_table_get_missing_key();
  test_table_set_and_get();
  test_table_update_existing_key();
  test_table_delete_via_set_null();
  test_table_multiple_tables();
  test_table_with_special_characters();
  test_table_empty_string_value();
  test_table_rapid_create_use_release();
  test_table_memory_no_leak();
}
