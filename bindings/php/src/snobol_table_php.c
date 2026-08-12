/**
 * @file snobol_table_php.c
 * @brief PHP bindings for SNOBOL4 runtime tables
 * 
 * Provides thin PHP wrappers over the C core table implementation.
 * All semantics live in the C core - PHP is just a façade.
 */

#include "php.h"
#include "php_snobol.h"
#include "snobol/table.h"
#include "zend_exceptions.h"

#include <stdio.h>
#include <string.h>

/* Disable logging for now */
#define SNOBOL_LOG(fmt, ...) ((void)0)

/* Standard PHP Custom Object Pattern: zend_object at the END */
typedef struct {
  snobol_table_t *table; /* Owned: the C table object */
  zend_object std;
} snobol_table_php_t;

extern zend_class_entry *snobol_table_ce;
static zend_object_handlers snobol_table_object_handlers;

/** @brief Recover the snobol_table_php_t from a zend_object pointer. */
static inline snobol_table_php_t *php_snobol_table_fetch(zend_object *obj) {
  return (
      snobol_table_php_t *)((char *)(obj)-XtOffsetOf(snobol_table_php_t, std));
}

/* Table registry to support pattern binding */
static snobol_table_t **global_php_tables = NULL;
static size_t global_php_table_count = 0;
static size_t global_php_table_cap = 0;

/** @brief Add a table to the process-global registry (grows the array). */
static void php_snobol_table_register(snobol_table_t *tbl) {
  if (global_php_table_count == global_php_table_cap) {
    global_php_table_cap = global_php_table_cap ? global_php_table_cap * 2 : 16;
    global_php_tables = realloc(
        global_php_tables, global_php_table_cap * sizeof(snobol_table_t *));
  }
  global_php_tables[global_php_table_count++] = tbl;
}

/** @brief Remove a table from the registry (used by the dtor). */
static void php_snobol_table_unregister(snobol_table_t *tbl) {
  for (size_t i = 0; i < global_php_table_count; i++) {
    if (global_php_tables[i] == tbl) {
      memmove(global_php_tables + i, global_php_tables + i + 1,
              (global_php_table_count - i - 1) * sizeof(snobol_table_t *));
      global_php_table_count--;
      break;
    }
  }
}

/** @brief Object dtor: unregisters and releases the core table. */
static void snobol_table_php_free(zend_object *object) {
  snobol_table_php_t *intern = php_snobol_table_fetch(object);
  SNOBOL_LOG("snobol_table_php_free: intern=%p table=%p", (void *)intern,
             (void *)intern->table);

  if (intern->table) {
    php_snobol_table_unregister(intern->table);
    table_release(intern->table);
    intern->table = NULL;
  }

  zend_object_std_dtor(object);
  SNOBOL_LOG("snobol_table_php_free: done");
}

/** @brief Object factory for Snobol\Table. */
static zend_object *snobol_table_php_create(zend_class_entry *ce) {
  snobol_table_php_t *intern =
      zend_object_alloc(sizeof(snobol_table_php_t), ce);
  SNOBOL_LOG("snobol_table_php_create: intern=%p", (void *)intern);

  intern->table = NULL;

  zend_object_std_init(&intern->std, ce);
  object_properties_init(&intern->std, ce);
  intern->std.handlers = &snobol_table_object_handlers;

  return &intern->std;
}

/* Argument info */
ZEND_BEGIN_ARG_INFO_EX(ai_table_construct, 0, 0, 0)
ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_get, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_set, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_has, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_delete, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, key, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_clear, 0, 0, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_table_size, 0, 0, 0)
ZEND_END_ARG_INFO()

/* PHP Methods */

/**
 * @brief Table::__construct(?string $name = null)
 *  Creates the core table (optionally named) and registers it.
 * @param name
 */
PHP_METHOD(Snobol_Table, __construct) {
  char *name = NULL;
  size_t name_len = 0;

  ZEND_PARSE_PARAMETERS_START(0, 1)
  Z_PARAM_OPTIONAL
  Z_PARAM_STRING_OR_NULL(name, name_len)
  ZEND_PARSE_PARAMETERS_END();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  const char *table_name = name ? name : NULL;
  intern->table = table_create(table_name);

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Failed to create table", 0);
    RETURN_NULL();
  }

  php_snobol_table_register(intern->table);

  SNOBOL_LOG("Snobol_Table::__construct: table=%p name=%s",
             (void *)intern->table, table_name ? table_name : "(unnamed)");
}

/**
 * @brief Table::get(string $key): ?string
 * @param key
 * @return Value, or null for an unset key.
 */
PHP_METHOD(Snobol_Table, get) {
  char *key;
  size_t key_len;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STRING(key, key_len)
  ZEND_PARSE_PARAMETERS_END();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_NULL();
  }

  size_t value_len = 0;
  const char *value = table_get_ex(intern->table, key, key_len, &value_len);

  if (!value) {
    RETURN_NULL();
  }

  RETVAL_STRINGL(value, value_len);
}

/**
 * @brief Table::set(string $key, ?string $value): bool
 *  Setting null deletes the key (SNOBOL semantics). Non-string, non-null
 *  values are rejected with an Exception.
 * @param key, value
 * @return true on success.
 */
PHP_METHOD(Snobol_Table, set) {
  char *key;
  size_t key_len;
  zval *value_zv;

  ZEND_PARSE_PARAMETERS_START(2, 2)
  Z_PARAM_STRING(key, key_len)
  Z_PARAM_ZVAL(value_zv)
  ZEND_PARSE_PARAMETERS_END();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_NULL();
  }

  const char *val_to_set = NULL;
  size_t val_len = 0;
  if (Z_TYPE_P(value_zv) == IS_NULL) {
    val_to_set = NULL;
  } else if (Z_TYPE_P(value_zv) == IS_STRING) {
    val_to_set = Z_STRVAL_P(value_zv);
    val_len = Z_STRLEN_P(value_zv);
  } else {
    zend_throw_exception(zend_ce_exception, "Value must be string or null", 0);
    RETURN_FALSE;
  }

  bool result = table_set_ex(intern->table, key, key_len, val_to_set, val_len);

  if (!result) {
    zend_throw_exception(zend_ce_exception, "Failed to set table value", 0);
    RETURN_FALSE;
  }

  RETURN_TRUE;
}

/**
 * @brief Table::has(string $key): bool
 * @param key
 * @return true when the key is set.
 */
PHP_METHOD(Snobol_Table, has) {
  char *key;
  size_t key_len;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STRING(key, key_len)
  ZEND_PARSE_PARAMETERS_END();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_FALSE;
  }

  bool result = table_has_ex(intern->table, key, key_len);
  RETURN_BOOL(result);
}

/**
 * @brief Table::delete(string $key): bool
 * @param key
 * @return true when the key existed and was removed.
 */
PHP_METHOD(Snobol_Table, delete) {
  char *key;
  size_t key_len;

  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STRING(key, key_len)
  ZEND_PARSE_PARAMETERS_END();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_FALSE;
  }

  bool result = table_delete_ex(intern->table, key, key_len);
  RETURN_BOOL(result);
}

/**
 * @brief Table::clear(): void
 */
PHP_METHOD(Snobol_Table, clear) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_NULL();
  }

  table_clear(intern->table);
}

/**
 * @brief Table::size(): int
 * @return Number of set keys.
 */
PHP_METHOD(Snobol_Table, size) {
  ZEND_PARSE_PARAMETERS_NONE();

  snobol_table_php_t *intern = php_snobol_table_fetch(Z_OBJ_P(getThis()));

  if (!intern->table) {
    zend_throw_exception(zend_ce_exception, "Table not initialized", 0);
    RETURN_LONG(0);
  }

  size_t size = table_size(intern->table);
  RETURN_LONG((zend_long)size);
}

/* Method table */
static const zend_function_entry snobol_table_methods[] = {
    PHP_ME(Snobol_Table, __construct, ai_table_construct, ZEND_ACC_PUBLIC)
        PHP_ME(Snobol_Table, get, ai_table_get, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_Table, set, ai_table_set, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_Table, has, ai_table_has, ZEND_ACC_PUBLIC) PHP_ME(
                    Snobol_Table, delete, ai_table_delete, ZEND_ACC_PUBLIC)
                    PHP_ME(Snobol_Table, clear, ai_table_clear, ZEND_ACC_PUBLIC)
                        PHP_ME(Snobol_Table, size, ai_table_size,
                               ZEND_ACC_PUBLIC) PHP_FE_END};

zend_class_entry *snobol_table_ce;

/**
 * Public helper: extract the underlying C snobol_table_t from a PHP zval
 * that holds a \\Snobol\\Table object.  Returns NULL if the zval is not a
 * Table object or if the internal pointer has not been initialised.
 */
snobol_table_t *php_snobol_get_table_from_zval(zval *zv) {
  if (!zv || Z_TYPE_P(zv) != IS_OBJECT)
    return NULL;
  zend_object *obj = Z_OBJ_P(zv);
  if (!obj || obj->ce != snobol_table_ce)
    return NULL;
  snobol_table_php_t *intern = php_snobol_table_fetch(obj);
  return intern ? intern->table : NULL;
}

/** @brief Register the Snobol\Table class (MINIT). */
void snobol_table_php_minit(void) {
  SNOBOL_LOG("snobol_table_php_minit: START");
  zend_class_entry ce;

  memcpy(&snobol_table_object_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_table_object_handlers.offset = XtOffsetOf(snobol_table_php_t, std);
  snobol_table_object_handlers.free_obj = snobol_table_php_free;

  INIT_CLASS_ENTRY(ce, "Snobol\\Table", snobol_table_methods);
  snobol_table_ce = zend_register_internal_class(&ce);
  snobol_table_ce->create_object = snobol_table_php_create;

  SNOBOL_LOG("snobol_table_php_minit: DONE");
}
