#include "php.h"
#include "php_snobol.h"
#include "zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "snobol/snobol.h"
#include "snobol/search.h"

#define SNOBOL_LOG(fmt, ...) ((void)0)

/**
 * @file snobol_search_iterator_php.c
 * @brief The Snobol\SearchIterator class: lazy iteration over pattern
 *        matches (backing Pattern::searchAllGenerator()).
 *
 * The iterator holds its own persistent search state and fetches the next
 * match only when the caller advances, so early breaks pay nothing for the
 * remaining matches.
 */

zend_class_entry *snobol_search_iterator_ce;
extern zend_class_entry *snobol_pattern_ce;

static zend_object_handlers snobol_search_iterator_handlers;

/* Internal struct: stores state for the "lazy iterator" semantics */
typedef struct {
  zval pattern_ref; /* Owns a reference to the Pattern object (lifetime) */
  zval subject_ref; /* Owns a reference to the subject string (lifetime) */
  snobol_pattern_t *pattern;
  snobol_pattern_search_state_t *state;
  const char *subject;
  size_t subject_len;
  /* Current iteration state */
  zend_long key;
  bool started;
  bool valid;
  zval current_match; /* owned match array (or IS_UNDEF/NULL) */
  zend_object std;
} snobol_search_iterator_t;

/** @brief Recover the iterator struct from a zend_object pointer. */
static inline snobol_search_iterator_t *php_si_fetch(zend_object *obj) {
  return (snobol_search_iterator_t *)((char *)(obj)-XtOffsetOf(
      snobol_search_iterator_t, std));
}

/** @brief Object dtor: releases the referenced pattern/subject and state. */
static void si_dtor(zend_object *object) {
  snobol_search_iterator_t *iter = php_si_fetch(object);
  zval_ptr_dtor(&iter->current_match);
  if (iter->state) {
    snobol_pattern_search_state_destroy(iter->state);
    iter->state = NULL;
  }
  zval_ptr_dtor(&iter->pattern_ref);
  zval_ptr_dtor(&iter->subject_ref);
  zend_object_std_dtor(object);
}

/** @brief Object factory for Snobol\SearchIterator. */
static zend_object *si_create(zend_class_entry *ce) {
  snobol_search_iterator_t *iter =
      zend_object_alloc(sizeof(snobol_search_iterator_t), ce);
  zend_object_std_init(&iter->std, ce);
  object_properties_init(&iter->std, ce);
  iter->std.handlers = &snobol_search_iterator_handlers;
  ZVAL_UNDEF(&iter->current_match);
  ZVAL_UNDEF(&iter->pattern_ref);
  ZVAL_UNDEF(&iter->subject_ref);
  return &iter->std;
}

/* ---- Internal: fetch next match and store in iter->current_match ---- */
/** @brief Fetch the next match at or after @p search_offset into
 *  iter->current_match.
 *  @return true when a match was stored; false on exhaustion (the previous
 *          match is left in place). */
static bool si_fetch_next(snobol_search_iterator_t *iter,
                          size_t search_offset) {
  if (!iter->state || search_offset > iter->subject_len)
    return false;

  snobol_match_t *m = snobol_pattern_search_ex(
      iter->state, iter->subject, iter->subject_len, search_offset);
  if (!m || !snobol_match_success(m))
    return false;

  size_t ms = snobol_match_get_position(m);
  size_t me = ms + snobol_match_get_length(m);

  zval_ptr_dtor(&iter->current_match);
  array_init(&iter->current_match);
  for (size_t i = 0; i < m->var_count; ++i) {
    char key[32];
    snprintf(key, sizeof(key), "v%u", (unsigned)i);
    size_t vlen = 0;
    const char *vval = snobol_match_get_variable(m, key, &vlen);
    if (vval && vlen > 0) {
      add_assoc_stringl(&iter->current_match, key, vval, vlen);
    } else {
      add_assoc_null(&iter->current_match, key);
    }
  }
  add_assoc_long(&iter->current_match, "_match_len", (zend_long)(me - ms));
  add_assoc_long(&iter->current_match, "_match_start", (zend_long)ms);
  if (m->output && m->output_len > 0) {
    add_assoc_stringl(&iter->current_match, "_output", m->output,
                      m->output_len);
  } else {
    add_assoc_string(&iter->current_match, "_output", "");
  }
  return true;
}

/* ---- Iterator methods implemented on the class itself ---- */

/**
 * @brief SearchIterator::current(): mixed
 *  The current match array, or null before the first fetch / after
 *  exhaustion the last match is retained (undefined per Iterator).
 * @return Match array or null.
 */
PHP_METHOD(Snobol_SearchIterator, current) {
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(ZEND_THIS));
  if (Z_TYPE(iter->current_match) == IS_ARRAY) {
    RETURN_ZVAL(&iter->current_match, 1, 0);
  }
  RETURN_NULL();
}

/**
 * @brief SearchIterator::key(): int
 *  Zero-based match index.
 * @return Index.
 */
PHP_METHOD(Snobol_SearchIterator, key) {
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(ZEND_THIS));
  RETURN_LONG(iter->key);
}

/**
 * @brief SearchIterator::next(): void
 *  Advances to the next match; marks the iterator invalid at the end.
 */
PHP_METHOD(Snobol_SearchIterator, next) {
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(ZEND_THIS));
  iter->key++;

  if (!iter->started || !iter->valid) {
    iter->valid = false;
    return;
  }

  /* Find the end of the current match to advance past it */
  zend_long match_start = 0, match_len = 0;
  if (Z_TYPE(iter->current_match) == IS_ARRAY) {
    zval *ms =
        zend_hash_str_find(Z_ARRVAL(iter->current_match), "_match_start", 12);
    if (ms)
      match_start = Z_LVAL_P(ms);
    zval *ml =
        zend_hash_str_find(Z_ARRVAL(iter->current_match), "_match_len", 10);
    if (ml)
      match_len = Z_LVAL_P(ml);
  }

  size_t search_offset = (size_t)(match_start + (match_len ? match_len : 1));
  iter->valid = si_fetch_next(iter, search_offset);
}

/**
 * @brief SearchIterator::valid(): bool
 * @return true while a match is current.
 */
PHP_METHOD(Snobol_SearchIterator, valid) {
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(ZEND_THIS));
  RETURN_BOOL(iter->valid);
}

/**
 * @brief SearchIterator::rewind(): void
 *  Resets the key and recreates the search state (no API reset exists).
 */
PHP_METHOD(Snobol_SearchIterator, rewind) {
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(ZEND_THIS));
  iter->key = 0;
  iter->started = true;

  if (!iter->state) {
    iter->valid = false;
    return;
  }

  /* Re-create search state (API has no reset). */
  snobol_pattern_search_state_destroy(iter->state);
  iter->state = snobol_pattern_search_state_create(iter->pattern->bc,
                                                   iter->pattern->bc_len);
  if (!iter->state) {
    iter->valid = false;
    return;
  }

  iter->valid = si_fetch_next(iter, 0);
  if (!iter->valid) {
    /* No match: drop the stale previous match so current() cannot return
     * a leftover from an earlier iteration. */
    zval_ptr_dtor(&iter->current_match);
    ZVAL_UNDEF(&iter->current_match);
  }
}

/* ---- Public API: create SearchIterator ---- */

/** @brief Implementation of php_snobol_create_search_iterator() (see php_snobol.h). */
void php_snobol_create_search_iterator(zval *return_value, zval *pattern_zv,
                                       zend_string *subject) {
  if (!snobol_search_iterator_ce) {
    zend_throw_exception(zend_ce_exception,
                         "SearchIterator class not registered", 0);
    return;
  }
  if (object_init_ex(return_value, snobol_search_iterator_ce) != SUCCESS) {
    zend_throw_exception(zend_ce_exception, "Failed to create SearchIterator",
                         0);
    return;
  }
  snobol_search_iterator_t *iter = php_si_fetch(Z_OBJ_P(return_value));
  /* Own references: the pattern object and subject string stay alive for
     the iterator's lifetime even if the caller drops its own. */
  ZVAL_COPY(&iter->pattern_ref, pattern_zv);
  ZVAL_STR_COPY(&iter->subject_ref, subject);
  iter->pattern = php_snobol_fetch(Z_OBJ_P(pattern_zv));
  iter->subject = ZSTR_VAL(subject);
  iter->subject_len = ZSTR_LEN(subject);
  iter->state =
      snobol_pattern_search_state_create(iter->pattern->bc,
                                         iter->pattern->bc_len);
  iter->key = 0;
  iter->started = false;
  iter->valid = false;
  ZVAL_UNDEF(&iter->current_match);
  if (!iter->state) {
    zend_throw_exception(zend_ce_exception, "Failed to create search state", 0);
  }
}

/**
 * @brief SearchIterator::fromPattern(Pattern $pattern, string $subject): SearchIterator
 *  Static constructor; throws for un-compiled patterns.
 * @param pattern, subject
 * @return Iterator object.
 */
PHP_METHOD(Snobol_SearchIterator, fromPattern) {
  zval *pattern_zv;
  zend_string *subject;
  ZEND_PARSE_PARAMETERS_START(2, 2)
  Z_PARAM_OBJECT_OF_CLASS(pattern_zv, snobol_pattern_ce)
  Z_PARAM_STR(subject)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *pat = php_snobol_fetch(Z_OBJ_P(pattern_zv));
  if (!pat->bc || pat->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_NULL();
  }
  php_snobol_create_search_iterator(return_value, pattern_zv, subject);
}

ZEND_BEGIN_ARG_INFO_EX(ai_si_fromPattern, 0, 0, 2)
ZEND_ARG_OBJ_INFO(0, pattern, Snobol\\Pattern, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_si_current, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_si_key, 0, 0, IS_MIXED, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_si_next, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_si_valid, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_si_rewind, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry snobol_search_iterator_methods[] = {
    PHP_ME(Snobol_SearchIterator, fromPattern, ai_si_fromPattern,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
        PHP_ME(Snobol_SearchIterator, current, ai_si_current, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_SearchIterator, key, ai_si_key, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_SearchIterator, next, ai_si_next, ZEND_ACC_PUBLIC)
                    PHP_ME(Snobol_SearchIterator, valid, ai_si_valid,
                           ZEND_ACC_PUBLIC)
                        PHP_ME(Snobol_SearchIterator, rewind, ai_si_rewind,
                               ZEND_ACC_PUBLIC) PHP_FE_END};

/** @brief Register the Snobol\SearchIterator class as an Iterator (MINIT). */
void snobol_search_iterator_minit(void) {
  memcpy(&snobol_search_iterator_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_search_iterator_handlers.offset =
      XtOffsetOf(snobol_search_iterator_t, std);
  snobol_search_iterator_handlers.free_obj = si_dtor;

  zend_class_entry ce;
  INIT_CLASS_ENTRY(ce, "Snobol\\SearchIterator",
                   snobol_search_iterator_methods);
  snobol_search_iterator_ce = zend_register_internal_class(&ce);
  snobol_search_iterator_ce->create_object = si_create;
  /* Implement Iterator interface — PHP will call the methods directly,
     * bypassing the buggy zend_object_iterator API. */
  zend_class_implements(snobol_search_iterator_ce, 1, zend_ce_iterator);
}
