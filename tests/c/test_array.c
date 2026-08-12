#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/snobol_internal.h"
#include "snobol/array.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_array_create_free(void) {
  test_suite("Array: create and free");

  snobol_array_t *array = snobol_array_create(10);
  test_assert(array != NULL, "snobol_array_create returns non-NULL");
  test_assert(snobol_array_size(array) == 0, "new array has size 0");

  snobol_array_release(array);
  test_assert(true, "snobol_array_release completes without error");
}

static void test_array_create_no_hint(void) {
  test_suite("Array: create without hint");

  snobol_array_t *array = snobol_array_create(0);
  test_assert(array != NULL, "snobol_array_create(0) returns non-NULL");

  snobol_array_release(array);
}

static void test_array_set_get(void) {
  test_suite("Array: set and get");

  snobol_array_t *array = snobol_array_create(0);

  bool result = snobol_array_set(array, 1, "hello");
  test_assert(result, "snobol_array_set returns true");
  test_assert(snobol_array_size(array) == 1, "size is 1 after insert");

  const char *value = snobol_array_get(array, 1);
  test_assert(value != NULL,
              "snobol_array_get returns non-NULL for existing key");
  test_assert(strcmp(value, "hello") == 0, "value matches inserted value");

  snobol_array_release(array);
}

static void test_array_get_unset(void) {
  test_suite("Array: get unset element");

  snobol_array_t *array = snobol_array_create(0);

  const char *value = snobol_array_get(array, 42);
  test_assert(value == NULL, "unset element returns NULL");

  snobol_array_release(array);
}

static void test_array_update(void) {
  test_suite("Array: update existing key");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_set(array, 5, "original");
  test_assert(snobol_array_size(array) == 1, "size is 1 after first insert");

  (void)snobol_array_set(array, 5, "updated");
  test_assert(snobol_array_size(array) == 1, "size is still 1 after update");

  const char *value = snobol_array_get(array, 5);
  test_assert(strcmp(value, "updated") == 0, "value is updated");

  snobol_array_release(array);
}

static void test_array_delete(void) {
  test_suite("Array: delete key");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_set(array, 1, "value1");
  (void)snobol_array_set(array, 2, "value2");
  test_assert(snobol_array_size(array) == 2, "size is 2");

  bool deleted = snobol_array_delete(array, 1);
  test_assert(deleted, "snobol_array_delete returns true for existing key");
  test_assert(snobol_array_size(array) == 1, "size is 1 after delete");

  const char *value = snobol_array_get(array, 1);
  test_assert(value == NULL, "snobol_array_get returns NULL for deleted key");

  snobol_array_release(array);
}

static void test_array_delete_nonexistent(void) {
  test_suite("Array: delete non-existent key");

  snobol_array_t *array = snobol_array_create(0);

  bool deleted = snobol_array_delete(array, 99);
  test_assert(!deleted, "delete non-existent returns false");

  snobol_array_release(array);
}

static void test_array_clear(void) {
  test_suite("Array: clear");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_set(array, 1, "a");
  (void)snobol_array_set(array, 2, "b");
  (void)snobol_array_set(array, 3, "c");
  test_assert(snobol_array_size(array) == 3, "size is 3");

  snobol_array_clear(array);
  test_assert(snobol_array_size(array) == 0, "size is 0 after clear");

  snobol_array_release(array);
}

static void test_array_sparse_access(void) {
  test_suite("Array: sparse access");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_set(array, 1, "first");
  (void)snobol_array_set(array, 100, "hundredth");
  (void)snobol_array_set(array, 1000, "thousandth");

  test_assert(snobol_array_size(array) == 3, "size is 3 for sparse entries");

  const char *v1 = snobol_array_get(array, 1);
  const char *v100 = snobol_array_get(array, 100);
  const char *v1000 = snobol_array_get(array, 1000);

  test_assert(strcmp(v1, "first") == 0, "index 1 has 'first'");
  test_assert(strcmp(v100, "hundredth") == 0, "index 100 has 'hundredth'");
  test_assert(strcmp(v1000, "thousandth") == 0, "index 1000 has 'thousandth'");

  test_assert(snobol_array_get(array, 50) == NULL,
              "unset index 50 returns NULL");

  snobol_array_release(array);
}

static void test_array_has(void) {
  test_suite("Array: has");

  snobol_array_t *array = snobol_array_create(0);
  test_assert(!snobol_array_has(array, 1), "unset key has returns false");

  (void)snobol_array_set(array, 1, "hello");
  test_assert(snobol_array_has(array, 1), "set key has returns true");

  snobol_array_release(array);
}

static void test_array_retain_release(void) {
  test_suite("Array: retain and release");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_retain(array);
  test_assert(true, "retain succeeds");

  snobol_array_release(array);
  snobol_array_release(array);
  test_assert(true, "double release succeeds (freed on second)");
}

static void test_array_keys_values(void) {
  test_suite("Array: keys and values");

  snobol_array_t *array = snobol_array_create(0);

  (void)snobol_array_set(array, 3, "three");
  (void)snobol_array_set(array, 1, "one");
  (void)snobol_array_set(array, 2, "two");

  size_t kcount;
  int32_t *keys = snobol_array_keys(array, &kcount);
  test_assert(kcount == 3, "keys count is 3");
  if (keys) {
    snobol_free(keys);
  }

  size_t vcount;
  char **values = snobol_array_values(array, &vcount);
  test_assert(vcount == 3, "values count is 3");
  if (values) {
    for (size_t i = 0; i < vcount; i++) {
      if (values[i]) {
        snobol_free(values[i]);
      }
    }
    snobol_free(values);
  }

  snobol_array_release(array);
}


/* ===== test_coverage_misc (part): coverage-driven tests merged into test_array.c ===== */
#include "../../core/include/snobol/array.h"
#include "../../core/include/snobol/lexer.h"
#include "../../core/include/snobol/search.h"
#include "../../core/include/snobol/string_fn.h"
#include "../../core/include/snobol/vm.h"
#include "../../core/include/snobol/snobol.h"

void test_cov_misc_array(void) {
  test_suite("Coverage: array resize + key edge cases");

  snobol_array_t *a = snobol_array_create(0);
  test_assert(a != NULL, "array created with 0 hint");

  /* Many inserts force resize chains. */
  for (int i = 0; i < 200; i++) {
    char key[16];
    snprintf(key, sizeof(key), "v%d", i);
    bool ok = snobol_array_set(a, i, key);
    test_assert(ok, "array insert grows capacity");
  }
  test_assert(snobol_array_get(a, 199) != NULL, "value retrievable");
  test_assert(snobol_array_get(a, 5000) == NULL, "missing key NULL");
  test_assert((!snobol_array_has(a, 5000)) != 0, "missing key absent");
  test_assert(snobol_array_has(a, 42), "present key found");

  /* Negative and zero keys. */
  test_assert(snobol_array_set(a, -5, "neg"), "negative key set");
  test_assert(strcmp(snobol_array_get(a, -5), "neg") == 0, "negative key get");
  test_assert(snobol_array_set(a, 0, "zero"), "zero key set");
  test_assert(strcmp(snobol_array_get(a, 0), "zero") == 0, "zero key get");

  /* Delete + clear. */
  test_assert(snobol_array_delete(a, 42), "delete present key");
  test_assert((!snobol_array_delete(a, 42)) != 0, "delete missing key");
  snobol_array_clear(a);
  test_assert((!snobol_array_has(a, 0)) != 0, "clear removes keys");

  /* values() snapshot. */
  for (int i = 0; i < 10; i++) {
    char key[16];
    snprintf(key, sizeof(key), "k%d", i);
    (void)snobol_array_set(a, i, key);
  }
  size_t count = 0;
  char **vals = snobol_array_values(a, &count);
  test_assert((vals != NULL && count == 10) != 0, "values snapshot");
  if (vals) {
    for (size_t vi = 0; vi < count; vi++) {
      snobol_free(vals[vi]);
    }
    snobol_free(vals);
  }

  /* Retain/release lifecycle. */
  snobol_array_t *r = snobol_array_retain(a);
  test_assert(r == a, "retain returns same object");
  snobol_array_release(r);
  snobol_array_release(a);
  test_assert(true, "array lifecycle complete");

  /* NULL guards. */
  test_assert(snobol_array_get(nullptr, 0) == NULL, "get(NULL)");
  test_assert((!snobol_array_has(nullptr, 0)) != 0, "has(NULL)");
  test_assert((!snobol_array_set(nullptr, 0, "x")) != 0, "set(NULL)");
  test_assert((!snobol_array_delete(nullptr, 0)) != 0, "delete(NULL)");
  snobol_array_clear(nullptr);
  snobol_array_release(nullptr);
  test_assert(snobol_array_values(nullptr, &count) == NULL, "values(NULL)");
  snobol_array_t *tmp = snobol_array_create(1);
  test_assert(tmp != NULL, "create with positive hint");
  snobol_array_release(tmp);
  tmp = snobol_array_create(-1);
  test_assert(tmp != NULL, "create with negative hint");
  snobol_array_release(tmp);
}

/* ── choice-stack arena + write-log + trail ───────────────────────────────── */


void test_cov_misc_array_round2(void) {
  test_suite("Coverage: array update/tombstone paths");

  snobol_array_t *a = snobol_array_create(1024);
  test_assert(a != NULL, "large-hint array created");

  /* Update-in-place replaces the stored value. */
  test_assert(snobol_array_set(a, 1, "one"), "set key 1");
  test_assert(snobol_array_set(a, 1, "ONE"), "update key 1");
  test_assert(strcmp(snobol_array_get(a, 1), "ONE") == 0, "update visible");

  /* NULL value deletes the entry (tombstone). */
  test_assert(snobol_array_set(a, 1, nullptr), "NULL-value deletes key");
  test_assert((!snobol_array_has(a, 1)) != 0, "deleted key absent");

  /* Tombstone slot is reused by the next insert. */
  test_assert(snobol_array_set(a, 1, "again"), "insert into tombstone");
  test_assert(strcmp(snobol_array_get(a, 1), "again") == 0,
              "tombstone reuse visible");

  /* Probes walk past tombstones: delete a cluster, insert more, get works. */
  for (int i = 10; i < 40; i++) {
    snobol_array_set(a, i, "fill");
  }
  for (int i = 10; i < 25; i++) {
    snobol_array_set(a, i, nullptr); /* tombstones */
  }
  for (int i = 25; i < 60; i++) {
    snobol_array_set(a, i, "fill2");
  }
  test_assert(strcmp(snobol_array_get(a, 30), "fill2") == 0,
              "get past tombstones");

  /* values() on a tombstones-laden array. */
  size_t count = 0;
  char **vals = snobol_array_values(a, &count);
  test_assert((vals != NULL && count > 0) != 0, "values with tombstones");
  if (vals) {
    for (size_t vi = 0; vi < count; vi++) {
      snobol_free(vals[vi]);
    }
    snobol_free(vals);
  }

  /* Clear frees everything. */
  snobol_array_clear(a);
  size_t c2 = 99;
  char **empty_vals = snobol_array_values(a, &c2);
  test_assert((empty_vals != NULL && c2 == 0) != 0, "values on empty array");
  snobol_free(empty_vals);
  snobol_array_release(a);
}


void test_cov_misc_round3_array(void) {
  /* Array negative-key hashing + tombstone-heavy ops. */
  snobol_array_t *a = snobol_array_create(4);
  test_assert(snobol_array_set(a, -7, "neg7"), "negative key set");
  test_assert(snobol_array_set(a, -7, "neg7b"), "negative key update");
  test_assert(strcmp(snobol_array_get(a, -7), "neg7b") == 0,
              "negative key read");
  test_assert(snobol_array_delete(a, -7), "negative key delete");
  test_assert((!snobol_array_has(a, -7)) != 0, "negative key gone");
  for (int i = 0; i < 100; i++) {
    snobol_array_set(a, i, "v");
  }
  for (int i = 0; i < 100; i += 2) {
    snobol_array_set(a, i, nullptr); /* tombstones */
  }
  for (int i = 0; i < 100; i += 2) {
    snobol_array_set(a, i, "v2"); /* reuse tombstones */
  }
  test_assert(strcmp(snobol_array_get(a, 98), "v2") == 0,
              "tombstone reuse works");
  size_t cnt = 0;
  char **vals = snobol_array_values(a, &cnt);
  test_assert((vals != NULL && cnt == 100) != 0,
              "values after tombstone churn");
  if (vals) {
    for (size_t vi = 0; vi < cnt; vi++) {
      snobol_free(vals[vi]);
    }
    snobol_free(vals);
  }
  snobol_array_release(a);
}


void test_cov_misc_round4_array(void) {
  /* Array retain(NULL) and tombstone-resize interplay. */
  test_assert(snobol_array_retain(nullptr) == NULL, "retain(NULL)");
  snobol_array_t *a = snobol_array_create(8);
  for (int i = 0; i < 50; i++) {
    snobol_array_set(a, i, "v");
  }
  for (int i = 0; i < 50; i += 2) {
    snobol_array_set(a, i, nullptr); /* tombstones beyond threshold */
  }
  test_assert(snobol_array_has(a, 49), "resize preserves live entries");
  snobol_array_release(a);
}

static void test_array_nul_safe_values(void) {
  test_suite("Array: NUL-safe values (byte-exact)");

  snobol_array_t *array = snobol_array_create(0);
  const char bin_val[] = {'a', '\0', 'b', 'c'};
  test_assert(snobol_array_set_ex(array, 7, bin_val, sizeof(bin_val)),
              "set_ex stores a NUL-containing value");

  size_t vlen = 0;
  const char *got = snobol_array_get_ex(array, 7, &vlen);
  test_assert(got != NULL, "get_ex finds the key");
  test_assert((vlen == 4 && memcmp(got, bin_val, 4) == 0) != 0,
              "get_ex returns all bytes (incl. embedded NUL)");

  /* values() must copy byte-exact too. */
  size_t cnt = 0;
  char **vals = snobol_array_values(array, &cnt);
  test_assert((vals && cnt == 1) != 0, "values() returns the entry");
  if (vals && cnt == 1) {
    test_assert(memcmp(vals[0], bin_val, 4) == 0,
                "values() copy is byte-exact");
    snobol_free(vals[0]);
    snobol_free(vals);
  }
  snobol_array_release(array);
}

void test_array_suite(void) {
  test_array_create_free();
  test_array_create_no_hint();
  test_array_set_get();
  test_array_get_unset();
  test_array_update();
  test_array_delete();
  test_array_delete_nonexistent();
  test_array_clear();
  test_array_sparse_access();
  test_array_has();
  test_array_retain_release();
  test_array_keys_values();
  test_array_nul_safe_values();
  test_cov_misc_array();
  test_cov_misc_array_round2();
  test_cov_misc_round3_array();
  test_cov_misc_round4_array();
}
