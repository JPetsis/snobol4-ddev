#include "php.h"
#include "php_snobol.h"
#include "zend_exceptions.h"
#include "snobol/snobol.h"

#define SNOBOL_LOG(fmt, ...) ((void)0)

/**
 * @file snobol_pattern_cache_php.c
 * @brief The Snobol\PatternCache class: an LRU cache with a per-key
 *        factory callback (get() computes and caches on miss).
 */

#define PCACHE_DEFAULT_CAPACITY 128

zend_class_entry *snobol_pattern_cache_ce;
static zend_object_handlers snobol_pattern_cache_object_handlers;

typedef struct {
  zval cache; /* PHP array: key => value */
  zval access_order; /* PHP indexed array: keys in access order, newest at end
                        (always densely reindexed 0..n-1 after deletions) */
  zend_long capacity;
  zend_object std;
} snobol_pattern_cache_php_t;

static inline snobol_pattern_cache_php_t *php_pcache_fetch(zend_object *obj) {
  return (snobol_pattern_cache_php_t *)((char *)(obj)-XtOffsetOf(
      snobol_pattern_cache_php_t, std));
}

static void php_pcache_free(zend_object *object) {
  snobol_pattern_cache_php_t *intern = php_pcache_fetch(object);
  zval_ptr_dtor(&intern->cache);
  zval_ptr_dtor(&intern->access_order);
  zend_object_std_dtor(object);
}

static zend_object *php_pcache_create(zend_class_entry *ce) {
  snobol_pattern_cache_php_t *intern =
      zend_object_alloc(sizeof(snobol_pattern_cache_php_t), ce);
  zend_object_std_init(&intern->std, ce);
  object_properties_init(&intern->std, ce);
  intern->std.handlers = &snobol_pattern_cache_object_handlers;
  array_init(&intern->cache);
  array_init(&intern->access_order);
  intern->capacity = PCACHE_DEFAULT_CAPACITY;
  return &intern->std;
}

/** @brief Rebuild the access-order array densely (keys 0..n-1). Deletions
 *  tombstone buckets and re-inserts can reuse the hole, so index-based LRU
 *  scans need a dense array; rebuilding after each mutation is O(capacity)
 *  and obviously correct. */
static void php_pcache_reindex_order(zval *order) {
  HashTable *src = Z_ARRVAL_P(order);
  zval fresh;
  array_init(&fresh);
  zval *entry;
  ZEND_HASH_FOREACH_VAL(src, entry) {
    zval copy;
    ZVAL_COPY(&copy, entry);
    zend_hash_next_index_insert(Z_ARRVAL_P(&fresh), &copy);
  }
  ZEND_HASH_FOREACH_END();
  zval_ptr_dtor(order);
  *order = fresh; /* move the struct */
}

/** @brief Evict the least-recently-used entry (oldest in access_order). */
static void php_pcache_evict_lru(snobol_pattern_cache_php_t *intern) {
  HashTable *ao = Z_ARRVAL_P(&intern->access_order);
  if (zend_hash_num_elements(ao) == 0)
    return;

  zval *lru_key_zv = zend_hash_index_find(ao, 0);
  if (!lru_key_zv)
    return;

  zend_hash_str_del(Z_ARRVAL_P(&intern->cache), Z_STRVAL_P(lru_key_zv),
                    Z_STRLEN_P(lru_key_zv));
  zend_hash_index_del(ao, 0);
  php_pcache_reindex_order(&intern->access_order);
}

/** @brief Move a key to the back of the LRU access order (no-op if absent). */
static void php_pcache_touch(snobol_pattern_cache_php_t *intern,
                             const char *key, size_t key_len) {
  HashTable *ao = Z_ARRVAL_P(&intern->access_order);
  uint32_t count = zend_hash_num_elements(ao);

  for (uint32_t i = 0; i < count; i++) {
    zval *zv = zend_hash_index_find(ao, i);
    if (zv && Z_STRLEN_P(zv) == key_len &&
        memcmp(Z_STRVAL_P(zv), key, key_len) == 0) {
      zval kv;
      ZVAL_STRINGL(&kv, key, key_len);
      zend_hash_index_del(ao, i);
      php_pcache_reindex_order(&intern->access_order);
      /* The insert moves kv's reference into the array (no dtor). */
      zend_hash_next_index_insert(Z_ARRVAL_P(&intern->access_order), &kv);
      return;
    }
  }
}

/* Argument info */
ZEND_BEGIN_ARG_INFO_EX(ai_pcache_construct, 0, 0, 0)
ZEND_ARG_TYPE_INFO(0, capacity, IS_LONG, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_pcache_get, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_ARG_CALLABLE_INFO(0, factory, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_pcache_has, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_pcache_clear, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_pcache_size, 0, 0, 0)
ZEND_END_ARG_INFO()

/**
 * @brief PatternCache::__construct(?int $capacity = null)
 *  Capacities below 1 raise ValueError.
 * @param capacity
 */
PHP_METHOD(Snobol_PatternCache, __construct) {
  zend_long capacity = PCACHE_DEFAULT_CAPACITY;
  ZEND_PARSE_PARAMETERS_START(0, 1)
  Z_PARAM_OPTIONAL
  Z_PARAM_LONG(capacity)
  ZEND_PARSE_PARAMETERS_END();

  if (capacity < 1) {
    zend_throw_exception(zend_ce_value_error,
                         "Cache capacity must be at least 1", 0);
    RETURN_NULL();
  }

  snobol_pattern_cache_php_t *intern = php_pcache_fetch(Z_OBJ_P(getThis()));
  intern->capacity = capacity;
}

/**
 * @brief PatternCache::get(string $key, callable $factory): mixed
 *  Returns the cached value on hit (refreshing LRU order); otherwise
 *  calls the factory, caches and returns its result. Factory exceptions
 *  propagate and nothing is cached.
 * @param key, factory
 * @return Cached/factory value.
 */
PHP_METHOD(Snobol_PatternCache, get) {
  char *key;
  size_t key_len;
  zend_fcall_info fci;
  zend_fcall_info_cache fci_cache;

  ZEND_PARSE_PARAMETERS_START(2, 2)
  Z_PARAM_STRING(key, key_len)
  Z_PARAM_FUNC(fci, fci_cache)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_cache_php_t *intern = php_pcache_fetch(Z_OBJ_P(getThis()));

  zval *cached = zend_hash_str_find(Z_ARRVAL_P(&intern->cache), key, key_len);
  if (cached) {
    php_pcache_touch(intern, key, key_len);
    RETVAL_ZVAL(cached, 1, 0);
    return;
  }

  zval factory_ret;
  fci.retval = &factory_ret;
  if (zend_call_function(&fci, &fci_cache) == SUCCESS &&
      Z_TYPE(factory_ret) != IS_UNDEF) {

    ZVAL_COPY(return_value, &factory_ret);

    if (zend_hash_num_elements(Z_ARRVAL_P(&intern->cache)) >=
            (uint32_t)intern->capacity &&
        !zend_hash_str_exists(Z_ARRVAL_P(&intern->cache), key, key_len)) {
      php_pcache_evict_lru(intern);
    }

    zval copy;
    ZVAL_COPY(&copy, return_value);
    zend_hash_str_update(Z_ARRVAL_P(&intern->cache), key, key_len, &copy);

    zval kv;
    ZVAL_STRINGL(&kv, key, key_len);
    zend_hash_next_index_insert(Z_ARRVAL_P(&intern->access_order), &kv);

    zval_ptr_dtor(&factory_ret);
  } else {
    RETVAL_NULL();
  }
}

/**
 * @brief PatternCache::has(string $key): bool
 * @param key
 * @return true when the key is cached.
 */
PHP_METHOD(Snobol_PatternCache, has) {
  char *key;
  size_t key_len;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STRING(key, key_len)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_cache_php_t *intern = php_pcache_fetch(Z_OBJ_P(getThis()));
  RETURN_BOOL(zend_hash_str_exists(Z_ARRVAL_P(&intern->cache), key, key_len));
}

/**
 * @brief PatternCache::clear(): void
 *  Empties the cache and the LRU access order.
 */
PHP_METHOD(Snobol_PatternCache, clear) {
  ZEND_PARSE_PARAMETERS_NONE();
  snobol_pattern_cache_php_t *intern = php_pcache_fetch(Z_OBJ_P(getThis()));
  zend_hash_clean(Z_ARRVAL_P(&intern->cache));
  zend_hash_clean(Z_ARRVAL_P(&intern->access_order));
}

/**
 * @brief PatternCache::size(): int
 *  Number of cached entries.
 * @return Entry count.
 */
PHP_METHOD(Snobol_PatternCache, size) {
  ZEND_PARSE_PARAMETERS_NONE();
  snobol_pattern_cache_php_t *intern = php_pcache_fetch(Z_OBJ_P(getThis()));
  RETURN_LONG((zend_long)zend_hash_num_elements(Z_ARRVAL_P(&intern->cache)));
}

static const zend_function_entry snobol_pattern_cache_methods[] = {
    PHP_ME(Snobol_PatternCache, __construct, ai_pcache_construct,
           ZEND_ACC_PUBLIC) PHP_ME(Snobol_PatternCache, get, ai_pcache_get,
                                   ZEND_ACC_PUBLIC)
        PHP_ME(Snobol_PatternCache, has, ai_pcache_has, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_PatternCache, clear, ai_pcache_clear, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_PatternCache, size, ai_pcache_size,
                       ZEND_ACC_PUBLIC) PHP_FE_END};

/** @brief Register the Snobol\PatternCache class (MINIT). */
void snobol_pattern_cache_php_minit(void) {
  zend_class_entry ce;
  memcpy(&snobol_pattern_cache_object_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_pattern_cache_object_handlers.offset =
      XtOffsetOf(snobol_pattern_cache_php_t, std);
  snobol_pattern_cache_object_handlers.free_obj = php_pcache_free;
  INIT_CLASS_ENTRY(ce, "Snobol\\PatternCache", snobol_pattern_cache_methods);
  snobol_pattern_cache_ce = zend_register_internal_class(&ce);
  snobol_pattern_cache_ce->create_object = php_pcache_create;
}
