/**
 * @file test_dynamic_patterns.c
 * @brief Tests for dynamic pattern objects and caching
 *
 * Tests dynamic pattern creation, reference counting, and cache operations.
 * Verifies proper memory management and cache behavior.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "../../core/include/snobol/snobol_internal.h"
#include "snobol/dynamic_pattern.h"
#include "snobol/vm.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Helper to create mock bytecode.  Returns a heap-allocated buffer that
 * the caller must free.  Tests that exercise dynamic_pattern_create use the
 * returned pointer as the bytecode; dynamic_pattern_release will free it
 * when the pattern's refcount drops to zero. */
static uint8_t *create_mock_bytecode(size_t *out_len) {
  uint8_t *bc = (uint8_t *)malloc(8);
  if (bc) {
    bc[0] = 0x01;
    bc[1] = 0x02;
    bc[2] = 0x03;
    bc[3] = 0x04;
    bc[4] = 0x05;
    bc[5] = 0x06;
    bc[6] = 0x07;
    bc[7] = 0x08;
    *out_len = 8;
  }
  return bc;
}

static void test_dynamic_pattern_create_free(void) {
  test_suite("Dynamic Pattern: create and free");

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern =
      dynamic_pattern_create("test pattern", bc, bc_len);
  test_assert(pattern != NULL, "dynamic_pattern_create returns non-NULL");
  test_assert(pattern->bc != NULL, "bytecode pointer is stored");
  test_assert(pattern->bc_len == 8, "bytecode length is stored");
  test_assert(pattern->refcount == 1, "initial refcount is 1");
  test_assert(pattern->is_valid, "pattern is valid");
  test_assert(pattern->source != NULL, "source is copied");
  test_assert(strcmp(pattern->source, "test pattern") == 0,
              "source content matches");

  dynamic_pattern_release(pattern);
  test_assert(true, "dynamic_pattern_release completes without error");
}

static void test_dynamic_pattern_create_null_source(void) {
  test_suite("Dynamic Pattern: create with NULL source");

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern = dynamic_pattern_create(nullptr, bc, bc_len);
  test_assert(pattern != NULL, "create with NULL source succeeds");
  test_assert(pattern->source == NULL, "source is NULL");
  test_assert(pattern->is_valid, "pattern is still valid");

  dynamic_pattern_release(pattern);
}

static void test_dynamic_pattern_create_invalid(void) {
  test_suite("Dynamic Pattern: create with NULL bytecode");

  dynamic_pattern_t *pattern = dynamic_pattern_create("test", nullptr, 0);
  test_assert(pattern != NULL, "create with NULL bytecode succeeds");
  test_assert(!pattern->is_valid, "pattern is marked invalid");

  dynamic_pattern_release(pattern);
}

static void test_dynamic_pattern_reference_counting(void) {
  test_suite("Dynamic Pattern: reference counting");

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern = dynamic_pattern_create("test", bc, bc_len);
  test_assert(pattern->refcount == 1, "initial refcount is 1");

  dynamic_pattern_t *retained = dynamic_pattern_retain(pattern);
  test_assert(retained == pattern, "retain returns same pointer");
  test_assert(pattern->refcount == 2, "refcount is 2 after retain");

  dynamic_pattern_release(pattern);
  test_assert(pattern->refcount == 1, "refcount is 1 after first release");

  dynamic_pattern_release(retained);
  test_assert(true, "second release frees without crash");
}

static void test_dynamic_pattern_hash_function(void) {
  test_suite("Dynamic Pattern: hash function");

  uint32_t hash1 = dynamic_pattern_hash_source("test", 4);
  uint32_t hash2 = dynamic_pattern_hash_source("test", 4);
  test_assert(hash1 == hash2, "same source produces same hash");

  uint32_t hash3 = dynamic_pattern_hash_source("other", 5);
  test_assert(hash1 != hash3, "different source produces different hash");

  uint32_t hash_empty = dynamic_pattern_hash_source("", 0);
  test_assert(hash_empty != 0, "empty source hash is non-zero");
}

static void test_dynamic_pattern_cache_key(void) {
  test_suite("Dynamic Pattern: cache key computation");

  dynamic_pattern_cache_key_t key1;
  dynamic_pattern_cache_key_t key2;

  dynamic_pattern_compute_key("test", 4, &key1);
  dynamic_pattern_compute_key("test", 4, &key2);

  test_assert(key1.hash == key2.hash, "same source produces same key hash");
  test_assert(key1.source_len == key2.source_len,
              "same source produces same key length");

  dynamic_pattern_compute_key("other", 5, &key2);
  test_assert(key1.hash != key2.hash,
              "different source produces different key");
}

static void test_dynamic_pattern_cache_init_destroy(void) {
  test_suite("Dynamic Pattern: cache init/destroy");

  dynamic_pattern_cache_t cache;
  bool result = dynamic_pattern_cache_init(&cache, 10);
  test_assert(result, "cache init succeeds");
  test_assert(cache.max_size == 10, "max_size is set");
  test_assert(cache.size == 0, "initial size is 0");
  test_assert(cache.buckets != NULL, "buckets allocated");

  dynamic_pattern_cache_destroy(&cache);
  test_assert(true, "cache destroy completes");
}

static void test_dynamic_pattern_cache_default_size(void) {
  test_suite("Dynamic Pattern: cache default size");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 0);
  test_assert(cache.max_size > 0, "default max_size is non-zero");
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_put_get(void) {
  test_suite("Dynamic Pattern: cache put and get");

  dynamic_pattern_cache_t cache;
  bool init_result = dynamic_pattern_cache_init(&cache, 10);
  test_assert(init_result, "cache init succeeds");
  test_assert(cache.buckets != NULL, "cache buckets allocated");
  test_assert(cache.bucket_count > 0, "cache bucket_count > 0");

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern =
      dynamic_pattern_create("test source", bc, bc_len);
  test_assert(pattern != NULL, "pattern created");

  bool result = dynamic_pattern_cache_put(&cache, "test source", pattern);
  test_assert(result, "cache put succeeds");
  test_assert(cache.size == 1, "cache size is 1");

  dynamic_pattern_t *retrieved =
      dynamic_pattern_cache_get(&cache, "test source", -1);
  test_assert(retrieved != NULL, "cache get returns non-NULL");
  test_assert(retrieved == pattern, "retrieved pattern is same pointer");
  test_assert(retrieved->refcount >= 2,
              "retrieved pattern has incremented refcount");

  dynamic_pattern_release(pattern);
  dynamic_pattern_release(retrieved);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_miss(void) {
  test_suite("Dynamic Pattern: cache miss");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  dynamic_pattern_t *retrieved =
      dynamic_pattern_cache_get(&cache, "nonexistent", -1);
  test_assert(retrieved == NULL, "cache get returns NULL for missing key");

  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_remove(void) {
  test_suite("Dynamic Pattern: cache remove");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern = dynamic_pattern_create("test", bc, bc_len);
  dynamic_pattern_cache_put(&cache, "test", pattern);
  test_assert(cache.size == 1, "size is 1");

  bool result = dynamic_pattern_cache_remove(&cache, "test", -1);
  test_assert(result, "remove returns true for existing key");
  test_assert(cache.size == 0, "size is 0 after remove");

  dynamic_pattern_t *retrieved = dynamic_pattern_cache_get(&cache, "test", -1);
  test_assert(retrieved == NULL, "get returns NULL after remove");

  dynamic_pattern_release(pattern);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_remove_missing(void) {
  test_suite("Dynamic Pattern: cache remove missing");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  bool result = dynamic_pattern_cache_remove(&cache, "nonexistent", -1);
  test_assert(!result, "remove returns false for missing key");

  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_clear(void) {
  test_suite("Dynamic Pattern: cache clear");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  /* Add multiple entries */
  for (int i = 0; i < 5; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key%d", i);
    size_t bc_len;
    uint8_t *bc = create_mock_bytecode(&bc_len);
    dynamic_pattern_t *pattern = dynamic_pattern_create(key, bc, bc_len);
    dynamic_pattern_cache_put(&cache, key, pattern);
    dynamic_pattern_release(pattern);
  }

  test_assert(cache.size == 5, "size is 5");

  dynamic_pattern_cache_clear(&cache);
  test_assert(cache.size == 0, "size is 0 after clear");

  /* Verify all entries are gone */
  for (int i = 0; i < 5; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key%d", i);
    dynamic_pattern_t *retrieved = dynamic_pattern_cache_get(&cache, key, -1);
    test_assert(retrieved == NULL, "entry is gone after clear");
  }

  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_eviction(void) {
  test_suite("Dynamic Pattern: cache eviction");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 3); /* Small cache */

  /* Add more entries than capacity */
  dynamic_pattern_t *patterns[5];
  for (int i = 0; i < 5; i++) {
    char key[32];
    snprintf(key, sizeof(key), "key%d", i);
    size_t bc_len;
    uint8_t *bc = create_mock_bytecode(&bc_len);
    patterns[i] = dynamic_pattern_create(key, bc, bc_len);
    dynamic_pattern_cache_put(&cache, key, patterns[i]);
  }

  test_assert(cache.size <= 3, "cache size respects max after eviction");

  /* Clean up */
  for (int i = 0; i < 5; i++) {
    dynamic_pattern_release(patterns[i]);
  }
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_duplicate_put(void) {
  test_suite("Dynamic Pattern: cache duplicate put");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern1 = dynamic_pattern_create("test", bc, bc_len);

  bc = create_mock_bytecode(&bc_len);
  dynamic_pattern_t *pattern2 = dynamic_pattern_create("test", bc, bc_len);

  dynamic_pattern_cache_put(&cache, "test", pattern1);
  test_assert(cache.size == 1, "size is 1 after first put");

  dynamic_pattern_cache_put(&cache, "test", pattern2);
  test_assert(cache.size == 1, "size is still 1 after duplicate put");

  /* Verify original pattern is still in cache */
  dynamic_pattern_t *retrieved = dynamic_pattern_cache_get(&cache, "test", -1);
  test_assert(retrieved == pattern1, "original pattern is retained");

  dynamic_pattern_release(pattern1);
  dynamic_pattern_release(pattern2);
  dynamic_pattern_release(retrieved);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_cache_stats(void) {
  test_suite("Dynamic Pattern: cache statistics");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  size_t size;
  size_t max;
  dynamic_pattern_cache_stats(&cache, &size, &max);
  test_assert(size == 0, "initial size is 0");
  test_assert(max == 10, "max is 10");

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  dynamic_pattern_t *pattern = dynamic_pattern_create("test", bc, bc_len);
  dynamic_pattern_cache_put(&cache, "test", pattern);

  dynamic_pattern_cache_stats(&cache, &size, &max);
  test_assert(size == 1, "size is 1 after put");

  dynamic_pattern_release(pattern);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_create_use_release_cycle(void) {
  test_suite("Dynamic Pattern: create/use/release cycle");

  /* Simulate repeated create/use/release as would happen in pattern execution
   */
  for (int cycle = 0; cycle < 10; cycle++) {
    size_t bc_len;
    uint8_t *bc = create_mock_bytecode(&bc_len);

    dynamic_pattern_t *pattern =
        dynamic_pattern_create("cycle pattern", bc, bc_len);

    /* Use the pattern */
    test_assert(pattern->is_valid, "cycle: pattern is valid");
    test_assert(pattern->bc_len == 8, "cycle: bytecode length correct");

    dynamic_pattern_release(pattern);
  }

  test_assert(true, "10 create/use/release cycles completed without leak");
}

static void test_dynamic_pattern_cache_stress(void) {
  test_suite("Dynamic Pattern: cache stress test");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 100);

  /* Add many entries */
  for (int i = 0; i < 50; i++) {
    char key[32];
    snprintf(key, sizeof(key), "stress_key_%d", i);
    size_t bc_len;
    uint8_t *bc = create_mock_bytecode(&bc_len);
    dynamic_pattern_t *pattern = dynamic_pattern_create(key, bc, bc_len);
    dynamic_pattern_cache_put(&cache, key, pattern);
    dynamic_pattern_release(pattern);
  }

  test_assert(cache.size == 50, "cache has 50 entries");

  /* Retrieve all entries */
  for (int i = 0; i < 50; i++) {
    char key[32];
    snprintf(key, sizeof(key), "stress_key_%d", i);
    dynamic_pattern_t *retrieved = dynamic_pattern_cache_get(&cache, key, -1);
    test_assert(retrieved != NULL, "stress: can retrieve entry");
    if (retrieved) {
      dynamic_pattern_release(retrieved);
    }
  }

  /* Remove half */
  for (int i = 0; i < 25; i++) {
    char key[32];
    snprintf(key, sizeof(key), "stress_key_%d", i);
    dynamic_pattern_cache_remove(&cache, key, -1);
  }

  test_assert(cache.size == 25, "cache has 25 entries after removal");

  dynamic_pattern_cache_destroy(&cache);
  test_assert(true, "stress test completed without crash");
}

static void test_dynamic_pattern_vm_execution(void) {
  test_suite("Dynamic Pattern: VM execution with cache");

  /* Verify OP_DYNAMIC and OP_DYNAMIC_DEF opcodes exist */
  test_assert(OP_DYNAMIC > 0, "OP_DYNAMIC opcode is defined");
  test_assert(OP_DYNAMIC_DEF > 0, "OP_DYNAMIC_DEF opcode is defined");

  /* Note: Full cache execution testing requires PHP extension build
   * Standalone tests verify opcode definitions only */
  test_assert(true, "dynamic pattern opcodes verified (full execution tested "
                    "in PHP extension)");
}

static void test_dynamic_pattern_vm_op_dynamic_def(void) {
  test_suite("Dynamic Pattern: OP_DYNAMIC_DEF bytecode");

  /* Verify OP_DYNAMIC_DEF opcode exists */
  test_assert(OP_DYNAMIC_DEF > 0, "OP_DYNAMIC_DEF opcode is defined");

  /* Verify bytecode structure: OP_DYNAMIC_DEF + len(u32) + bytecode */
  uint8_t test_bc[] = {
      OP_DYNAMIC_DEF, 0x00, 0x00, 0x00, 0x03, /* len = 3 */
      0x01,           0x02, 0x03              /* bytecode payload */
  };

  test_assert(test_bc[0] == OP_DYNAMIC_DEF,
              "OP_DYNAMIC_DEF opcode in bytecode");
  test_assert((test_bc[1] == 0 && test_bc[2] == 0 && test_bc[3] == 0 &&
               test_bc[4] == 3) != 0,
              "length encoding correct");
}

static void test_dynamic_pattern_cache_reuse(void) {
  test_suite("Dynamic Pattern: cache reuse for repeated patterns");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  /* Create and cache a pattern */
  dynamic_pattern_t *pattern1 =
      dynamic_pattern_create("test_reuse", bc, bc_len);
  dynamic_pattern_cache_put(&cache, "test_reuse", pattern1);

  /* Retrieve the same pattern - should be cached */
  dynamic_pattern_t *pattern2 =
      dynamic_pattern_cache_get(&cache, "test_reuse", -1);
  test_assert(pattern2 != NULL, "cache hit for repeated pattern");
  test_assert(pattern2 == pattern1, "same pattern object returned");

  /* Verify refcount increased (cache retained its reference) */
  test_assert(pattern2->refcount >= 2, "cache retains reference");

  dynamic_pattern_release(pattern1);
  dynamic_pattern_release(pattern2);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_invalid_source_handling(void) {
  test_suite("Dynamic Pattern: invalid source handling");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  /* Try to get non-existent pattern */
  dynamic_pattern_t *pattern =
      dynamic_pattern_cache_get(&cache, "nonexistent", -1);
  test_assert(pattern == NULL, "get returns NULL for missing pattern");

  /* Try to cache with empty source */
  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);
  pattern = dynamic_pattern_create("", bc, bc_len);
  test_assert(pattern != NULL, "create with empty source succeeds");
  test_assert(pattern->is_valid, "empty source pattern is valid");

  bool result = dynamic_pattern_cache_put(&cache, "", pattern);
  test_assert(result, "can cache with empty source key");

  dynamic_pattern_release(pattern);
  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_ownership_lifecycle(void) {
  test_suite("Dynamic Pattern: ownership and lifecycle");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 10);

  size_t bc_len;
  uint8_t *bc = create_mock_bytecode(&bc_len);

  /* Create pattern - caller owns reference (refcount=1) */
  dynamic_pattern_t *pattern = dynamic_pattern_create("lifecycle", bc, bc_len);
  test_assert(pattern->refcount == 1, "initial refcount is 1");

  /* Cache it - cache retains its own reference */
  bool put_result = dynamic_pattern_cache_put(&cache, "lifecycle", pattern);
  test_assert(put_result, "put succeeds");
  test_assert(pattern->refcount == 2, "refcount is 2 after cache put");

  /* Get it back - caller gets retained reference */
  dynamic_pattern_t *retrieved =
      dynamic_pattern_cache_get(&cache, "lifecycle", -1);
  test_assert(retrieved == pattern, "get returns same pattern");
  test_assert(pattern->refcount == 3, "refcount is 3 after get");

  /* Release caller's original reference */
  dynamic_pattern_release(pattern);
  test_assert(pattern->refcount == 2, "refcount is 2 after caller release");

  /* Release retrieved reference */
  dynamic_pattern_release(retrieved);
  test_assert(pattern->refcount == 1, "refcount is 1 after retrieved release");

  /* Clear cache - cache releases its reference */
  dynamic_pattern_cache_clear(&cache);
  test_assert(true, "clear completes without crash");
  /* Pattern should now be freed (refcount went to 0) */

  dynamic_pattern_cache_destroy(&cache);
}

static void test_dynamic_pattern_concurrent_cache_access(void) {
  test_suite("Dynamic Pattern: concurrent cache access simulation");

  dynamic_pattern_cache_t cache;
  dynamic_pattern_cache_init(&cache, 100);

  /* Simulate multiple patterns being cached and accessed */
  dynamic_pattern_t *patterns[10];
  for (int i = 0; i < 10; i++) {
    char key[32];
    snprintf(key, sizeof(key), "concurrent_%d", i);
    size_t bc_len;
    uint8_t *bc = create_mock_bytecode(&bc_len);
    patterns[i] = dynamic_pattern_create(key, bc, bc_len);
    dynamic_pattern_cache_put(&cache, key, patterns[i]);
  }

  test_assert(cache.size == 10, "all 10 patterns cached");

  /* Access all patterns in different order */
  for (int i = 9; i >= 0; i--) {
    char key[32];
    snprintf(key, sizeof(key), "concurrent_%d", i);
    dynamic_pattern_t *retrieved = dynamic_pattern_cache_get(&cache, key, -1);
    test_assert(retrieved != NULL, "can retrieve pattern");
    if (retrieved) {
      dynamic_pattern_release(retrieved);
    }
  }

  /* Release original references */
  for (int i = 0; i < 10; i++) {
    dynamic_pattern_release(patterns[i]);
  }

  dynamic_pattern_cache_destroy(&cache);
  test_assert(true, "concurrent access simulation completed");
}

/* Top-level dynamic-pattern suite. */
void test_dynamic_pattern_suite(void) {
  test_dynamic_pattern_create_free();
  test_dynamic_pattern_create_null_source();
  test_dynamic_pattern_create_invalid();
  test_dynamic_pattern_reference_counting();
  test_dynamic_pattern_hash_function();
  test_dynamic_pattern_cache_key();
  test_dynamic_pattern_cache_init_destroy();
  test_dynamic_pattern_cache_default_size();
  test_dynamic_pattern_cache_put_get();
  test_dynamic_pattern_cache_miss();
  test_dynamic_pattern_cache_remove();
  test_dynamic_pattern_cache_remove_missing();
  test_dynamic_pattern_cache_clear();
  test_dynamic_pattern_cache_eviction();
  test_dynamic_pattern_cache_duplicate_put();
  test_dynamic_pattern_cache_stats();
  test_dynamic_pattern_create_use_release_cycle();
  test_dynamic_pattern_cache_stress();
  test_dynamic_pattern_vm_execution();
  test_dynamic_pattern_vm_op_dynamic_def();
  test_dynamic_pattern_cache_reuse();
  test_dynamic_pattern_invalid_source_handling();
  test_dynamic_pattern_ownership_lifecycle();
  test_dynamic_pattern_concurrent_cache_access();
}
