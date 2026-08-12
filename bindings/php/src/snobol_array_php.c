#include "php.h"
#include "php_snobol.h"
#include "snobol/array.h"
#include "snobol/snobol_internal.h"
#include "zend_exceptions.h"

#include <stdio.h>
#include <string.h>

#define SNOBOL_LOG(fmt, ...) ((void)0)

/**
 * @file snobol_array_php.c
 * @brief The Snobol\Array_ internal class: marshals a core snobol_array_t
 *        to PHP and keeps a process-global registry of live arrays.
 */

/** @brief Object struct: core array pointer plus the zend_object tail. */
typedef struct {
  snobol_array_t *array;
  zend_object std;
} snobol_array_php_t;

extern zend_class_entry *snobol_array_ce;
static zend_object_handlers snobol_array_object_handlers;

/** @brief Recover the snobol_array_php_t from a zend_object pointer. */
static inline snobol_array_php_t *php_snobol_array_fetch(zend_object *obj) {
  return (
      snobol_array_php_t *)((char *)(obj)-XtOffsetOf(snobol_array_php_t, std));
}

static snobol_array_t **global_php_arrays = NULL;
static size_t global_php_array_count = 0;
static size_t global_php_array_cap = 0;

/** @brief Add an array to the process-global registry (grows the array). */
static void php_snobol_array_register(snobol_array_t *arr) {
  if (global_php_array_count == global_php_array_cap) {
    global_php_array_cap = global_php_array_cap ? global_php_array_cap * 2 : 16;
    global_php_arrays = realloc(
        global_php_arrays, global_php_array_cap * sizeof(snobol_array_t *));
  }
  global_php_arrays[global_php_array_count++] = arr;
}

/** @brief Remove an array from the registry (used by the dtor). */
static void php_snobol_array_unregister(snobol_array_t *arr) {
  for (size_t i = 0; i < global_php_array_count; i++) {
    if (global_php_arrays[i] == arr) {
      memmove(global_php_arrays + i, global_php_arrays + i + 1,
              (global_php_array_count - i - 1) * sizeof(snobol_array_t *));
      global_php_array_count--;
      break;
    }
  }
}

/** @brief Object dtor: unregisters and releases the core array. */
static void snobol_array_php_free(zend_object *object) {
  snobol_array_php_t *intern = php_snobol_array_fetch(object);
  SNOBOL_LOG("snobol_array_php_free: intern=%p array=%p", (void *)intern,
             (void *)intern->array);

  if (intern->array) {
    php_snobol_array_unregister(intern->array);
    snobol_array_release(intern->array);
    intern->array = NULL;
  }

  zend_object_std_dtor(object);
}

/** @brief Object factory for Snobol\Array_. */
static zend_object *snobol_array_php_create(zend_class_entry *ce) {
  snobol_array_php_t *intern =
      zend_object_alloc(sizeof(snobol_array_php_t), ce);
  intern->array = NULL;

  zend_object_std_init(&intern->std, ce);
  object_properties_init(&intern->std, ce);
  intern->std.handlers = &snobol_array_object_handlers;

  return &intern->std;
}

ZEND_BEGIN_ARG_INFO_EX(ai_array_construct, 0, 0, 0)
ZEND_ARG_TYPE_INFO(0, size, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_get, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_set, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_has, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_delete, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_clear, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_size, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_keys, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_array_values, 0, 0, 0)
ZEND_END_ARG_INFO()

/**
 * @brief Array_::__construct(int $size = 0)
 *  Creates the core array and registers it in the global registry.
 * @param size
 */
PHP_METHOD(Snobol_Array_, __construct) {
  zend_long size = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
  Z_PARAM_OPTIONAL
  Z_PARAM_LONG(size)
  ZEND_PARSE_PARAMETERS_END();

  if (size < 0 || size > 0x7FFFFFFF) {
    zend_throw_exception(zend_ce_value_error,
                         "Array size must be between 0 and 2147483647", 0);
    RETURN_NULL();
  }

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  intern->array = snobol_array_create((int32_t)size);

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Failed to create array", 0);
    RETURN_NULL();
  }

  php_snobol_array_register(intern->array);
}

/**
 * @brief Array_::get(int $key): ?string
 * @param key
 * @return Value at the key, or null when unset.
 */
static bool php_snobol_array_check_key(zend_long key) {
  if (key >= -2147483647 - 1 && key <= 0x7FFFFFFF)
    return true;
  zend_throw_exception(zend_ce_value_error,
                       "Array key must be within the int32 range", 0);
  return false;
}

PHP_METHOD(Snobol_Array_, get) {
  zend_long key;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_LONG(key)
  ZEND_PARSE_PARAMETERS_END();

  if (!php_snobol_array_check_key(key))
    RETURN_NULL();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_NULL();
  }

  size_t value_len = 0;
  const char *value =
      snobol_array_get_ex(intern->array, (int32_t)key, &value_len);

  if (!value) {
    RETURN_NULL();
  }

  RETVAL_STRINGL(value, value_len);
}

/**
 * @brief Array_::set(int $key, string $value): bool
 * @param key, value
 * @return true on success.
 */
PHP_METHOD(Snobol_Array_, set) {
  zend_long key;
  char *value;
  size_t value_len;

  ZEND_PARSE_PARAMETERS_START(2, 2)
  Z_PARAM_LONG(key)
  Z_PARAM_STRING(value, value_len)
  ZEND_PARSE_PARAMETERS_END();

  if (!php_snobol_array_check_key(key))
    RETURN_FALSE;

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_FALSE;
  }

  bool result =
      snobol_array_set_ex(intern->array, (int32_t)key, value, value_len);

  if (!result) {
    zend_throw_exception(zend_ce_exception, "Failed to set array value", 0);
    RETURN_FALSE;
  }

  RETURN_TRUE;
}

/**
 * @brief Array_::has(int $key): bool
 * @param key
 * @return true when the key is set.
 */
PHP_METHOD(Snobol_Array_, has) {
  zend_long key;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_LONG(key)
  ZEND_PARSE_PARAMETERS_END();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_FALSE;
  }

  bool result = snobol_array_has(intern->array, (int32_t)key);
  RETURN_BOOL(result);
}

/**
 * @brief Array_::delete(int $key): bool
 * @param key
 * @return true when the key existed and was removed.
 */
PHP_METHOD(Snobol_Array_, delete) {
  zend_long key;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_LONG(key)
  ZEND_PARSE_PARAMETERS_END();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_FALSE;
  }

  bool result = snobol_array_delete(intern->array, (int32_t)key);
  RETURN_BOOL(result);
}

/**
 * @brief Array_::clear(): void
 */
PHP_METHOD(Snobol_Array_, clear) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_NULL();
  }

  snobol_array_clear(intern->array);
}

/**
 * @brief Array_::size(): int
 * @return Number of set keys.
 */
PHP_METHOD(Snobol_Array_, size) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_LONG(0);
  }

  size_t size = snobol_array_size(intern->array);
  RETURN_LONG((zend_long)size);
}

/**
 * @brief Array_::keys(): array
 * @return Array of set keys as ints.
 */
PHP_METHOD(Snobol_Array_, keys) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_NULL();
  }

  size_t count;
  int32_t *keys = snobol_array_keys(intern->array, &count);

  array_init(return_value);
  for (size_t i = 0; i < count; i++) {
    add_next_index_long(return_value, (zend_long)keys[i]);
  }

  /* The core allocates with snobol_malloc (malloc in STANDALONE builds,
   * emalloc in PHP_BUILD builds); snobol_free matches either. */
  if (keys) {
    snobol_free(keys);
  }
}

/**
 * @brief Array_::values(): array
 * @return Array of values (NULL entries skipped).
 */
PHP_METHOD(Snobol_Array_, values) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_array_php_t *intern = php_snobol_array_fetch(Z_OBJ_P(getThis()));

  if (!intern->array) {
    zend_throw_exception(zend_ce_exception, "Array not initialized", 0);
    RETURN_NULL();
  }

  /* Read via keys + get_ex so values are byte-exact (embedded NULs are
   * preserved; snobol_array_values() cannot carry per-value lengths). */
  size_t count;
  int32_t *keys = snobol_array_keys(intern->array, &count);

  array_init(return_value);
  for (size_t i = 0; i < count; i++) {
    size_t vlen = 0;
    const char *v = snobol_array_get_ex(intern->array, keys[i], &vlen);
    if (v) {
      add_next_index_stringl(return_value, v, vlen);
    } else {
      add_next_index_null(return_value);
    }
  }

  if (keys) {
    snobol_free(keys);
  }
}

static const zend_function_entry snobol_array_methods[] = {
    PHP_ME(Snobol_Array_, __construct, ai_array_construct, ZEND_ACC_PUBLIC)
        PHP_ME(Snobol_Array_, get, ai_array_get, ZEND_ACC_PUBLIC) PHP_ME(
            Snobol_Array_, set, ai_array_set, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_Array_, has, ai_array_has, ZEND_ACC_PUBLIC) PHP_ME(
                Snobol_Array_, delete, ai_array_delete, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_Array_, clear, ai_array_clear, ZEND_ACC_PUBLIC)
                    PHP_ME(Snobol_Array_, size, ai_array_size, ZEND_ACC_PUBLIC)
                        PHP_ME(Snobol_Array_, keys, ai_array_keys,
                               ZEND_ACC_PUBLIC)
                            PHP_ME(Snobol_Array_, values, ai_array_values,
                                   ZEND_ACC_PUBLIC) PHP_FE_END};

zend_class_entry *snobol_array_ce;

/** @brief Register the Snobol\Array_ class (MINIT). */
void snobol_array_php_minit(void) {
  SNOBOL_LOG("snobol_array_php_minit: START");
  zend_class_entry ce;

  memcpy(&snobol_array_object_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_array_object_handlers.offset = XtOffsetOf(snobol_array_php_t, std);
  snobol_array_object_handlers.free_obj = snobol_array_php_free;

  INIT_CLASS_ENTRY(ce, "Snobol\\Array_", snobol_array_methods);
  snobol_array_ce = zend_register_internal_class(&ce);
  snobol_array_ce->create_object = snobol_array_php_create;

  SNOBOL_LOG("snobol_array_php_minit: DONE");
}
