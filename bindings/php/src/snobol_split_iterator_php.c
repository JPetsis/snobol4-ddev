#include "php.h"
#include "php_snobol.h"
#include "zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "snobol/snobol.h"
#include "snobol/search.h"

#define SNOBOL_LOG(fmt, ...) ((void)0)

/**
 * @file snobol_split_iterator_php.c
 * @brief The Snobol\SplitIterator class: lazy split iteration (backing
 *        Pattern::searchSplitGenerator()).
 *
 * Segments are materialized one at a time via the lean tokenize API
 * (snobol_pattern_search_next), so early breaks pay nothing for the
 * remaining delimiters.
 */

zend_class_entry *snobol_split_iterator_ce;
extern zend_class_entry *snobol_pattern_ce;

static zend_object_handlers snobol_split_iterator_handlers;

typedef struct {
  zval pattern_ref; /* Owns a reference to the Pattern object (lifetime) */
  zval subject_ref; /* Owns a reference to the subject string (lifetime) */
  snobol_pattern_t *pattern;
  snobol_pattern_search_state_t *state;
  const char *subject;
  size_t subject_len;
  bool is_literal_only; /* lean snobol_pattern_search_next usable */
  zend_long key;
  bool started;
  bool valid;
  size_t last_end;
  bool has_trailing;
  zval current_segment;
  zend_object std;
} snobol_split_iterator_t;

snobol_split_iterator_t *php_si_split_fetch(zend_object *obj) {
  return (snobol_split_iterator_t *)((char *)(obj)-XtOffsetOf(
      snobol_split_iterator_t, std));
}

/** @brief Object dtor: releases the referenced pattern/subject and state. */
static void si_dtor(zend_object *object) {
  snobol_split_iterator_t *iter = php_si_split_fetch(object);
  zval_ptr_dtor(&iter->current_segment);
  if (iter->state) {
    snobol_pattern_search_state_destroy(iter->state);
    iter->state = NULL;
  }
  zval_ptr_dtor(&iter->pattern_ref);
  zval_ptr_dtor(&iter->subject_ref);
  zend_object_std_dtor(object);
}

/** @brief Object factory for Snobol\SplitIterator. */
static zend_object *si_create(zend_class_entry *ce) {
  snobol_split_iterator_t *iter =
      zend_object_alloc(sizeof(snobol_split_iterator_t), ce);
  zend_object_std_init(&iter->std, ce);
  object_properties_init(&iter->std, ce);
  iter->std.handlers = &snobol_split_iterator_handlers;
  ZVAL_UNDEF(&iter->current_segment);
  ZVAL_UNDEF(&iter->pattern_ref);
  ZVAL_UNDEF(&iter->subject_ref);
  return &iter->std;
}

/** @brief Materialize the next segment (delimiter match or trailing
 *  remainder) into iter->current_segment.
 *  @return true when a delimiter match was found; false at the end. */
static bool si_fetch_next(snobol_split_iterator_t *iter) {
  if (!iter->state)
    return false;

  size_t match_pos, match_len;
  bool found;
  if (iter->is_literal_only) {
    found = snobol_pattern_search_next(iter->state, iter->subject,
                                       iter->subject_len, iter->last_end,
                                       &match_pos, &match_len);
  } else {
    /* Non-literal delimiter (SPAN/alternation/…): the lean tokenize API is
       literal-only, so fall back to the general per-call search. */
    snobol_match_t *m = snobol_pattern_search_ex(
        iter->state, iter->subject, iter->subject_len, iter->last_end);
    if (m && snobol_match_success(m)) {
      match_pos = snobol_match_get_position(m);
      match_len = snobol_match_get_length(m);
      found = true;
    } else {
      found = false;
    }
  }

  zval_ptr_dtor(&iter->current_segment);

  if (found) {
    size_t seg_len = match_pos - iter->last_end;
    ZVAL_STRINGL(&iter->current_segment, iter->subject + iter->last_end,
                 seg_len);
    iter->last_end = match_pos + match_len;
    iter->has_trailing = false;
  } else {
    size_t seg_len = iter->subject_len - iter->last_end;
    if (seg_len > 0) {
      ZVAL_STRINGL(&iter->current_segment, iter->subject + iter->last_end,
                   seg_len);
    } else {
      ZVAL_EMPTY_STRING(&iter->current_segment);
    }
    iter->last_end = iter->subject_len;
    iter->has_trailing = false;
    /* If we just set the trailing segment, mark it available */
    if (seg_len > 0) {
      iter->has_trailing = true;
    }
  }
  return found;
}

/**
 * @brief SplitIterator::current(): mixed
 *  The current segment string, or null before the first fetch.
 * @return Segment or null.
 */
PHP_METHOD(Snobol_SplitIterator, current) {
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(ZEND_THIS));
  if (Z_TYPE(iter->current_segment) == IS_STRING) {
    RETURN_ZVAL(&iter->current_segment, 1, 0);
  }
  RETURN_NULL();
}

/**
 * @brief SplitIterator::key(): int
 *  Zero-based segment index.
 * @return Index.
 */
PHP_METHOD(Snobol_SplitIterator, key) {
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(ZEND_THIS));
  RETURN_LONG(iter->key);
}

/**
 * @brief SplitIterator::next(): void
 *  Advances to the next segment; invalidates the iterator after the
 *  trailing segment is consumed.
 */
PHP_METHOD(Snobol_SplitIterator, next) {
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(ZEND_THIS));
  iter->key++;

  if (!iter->started || !iter->valid)
    return;

  /* If the trailing segment was already consumed, nothing left */
  if (iter->has_trailing) {
    iter->valid = false;
    iter->has_trailing = false;
    return;
  }

  bool has_more = si_fetch_next(iter);
  if (!has_more && !iter->has_trailing)
    iter->valid = false;
}

/**
 * @brief SplitIterator::valid(): bool
 * @return true while a segment is current.
 */
PHP_METHOD(Snobol_SplitIterator, valid) {
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(ZEND_THIS));
  RETURN_BOOL(iter->valid);
}

/**
 * @brief SplitIterator::rewind(): void
 *  Resets the segment cursor and recreates the search state.
 */
PHP_METHOD(Snobol_SplitIterator, rewind) {
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(ZEND_THIS));
  iter->key = 0;
  iter->started = true;
  iter->last_end = 0;
  iter->has_trailing = false;

  snobol_pattern_search_state_destroy(iter->state);
  iter->state = snobol_pattern_search_state_create(iter->pattern->bc,
                                                   iter->pattern->bc_len);
  if (!iter->state) {
    iter->valid = false;
    return;
  }

  bool has_more = si_fetch_next(iter);
  iter->valid = true;
  if (!has_more && !iter->has_trailing)
    iter->valid = false;
}

ZEND_BEGIN_ARG_INFO_EX(ai_split_fromPattern, 0, 0, 2)
ZEND_ARG_OBJ_INFO(0, pattern, Snobol\\Pattern, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_END_ARG_INFO()

/**
 * @brief SplitIterator::fromPattern(Pattern $pattern, string $subject): SplitIterator
 *  Static constructor; throws for un-compiled patterns.
 * @param pattern, subject
 * @return Iterator object.
 */
PHP_METHOD(Snobol_SplitIterator, fromPattern) {
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

  php_snobol_create_split_iterator(return_value, pattern_zv, subject);
}

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

static const zend_function_entry snobol_split_iterator_methods[] = {
    PHP_ME(Snobol_SplitIterator, fromPattern, ai_split_fromPattern,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
        PHP_ME(Snobol_SplitIterator, current, ai_si_current, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_SplitIterator, key, ai_si_key, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_SplitIterator, next, ai_si_next, ZEND_ACC_PUBLIC)
                    PHP_ME(Snobol_SplitIterator, valid, ai_si_valid,
                           ZEND_ACC_PUBLIC)
                        PHP_ME(Snobol_SplitIterator, rewind, ai_si_rewind,
                               ZEND_ACC_PUBLIC) PHP_FE_END};

/** @brief Implementation of php_snobol_create_split_iterator() (see php_snobol.h). */
void php_snobol_create_split_iterator(zval *return_value, zval *pattern_zv,
                                      zend_string *subject) {
  if (!snobol_split_iterator_ce) {
    zend_throw_exception(zend_ce_exception,
                         "SplitIterator class not registered", 0);
    return;
  }
  if (object_init_ex(return_value, snobol_split_iterator_ce) != SUCCESS) {
    zend_throw_exception(zend_ce_exception, "Failed to create SplitIterator",
                         0);
    return;
  }
  snobol_split_iterator_t *iter = php_si_split_fetch(Z_OBJ_P(return_value));
  /* Own references: the pattern object and subject string stay alive for
     the iterator's lifetime even if the caller drops its own. */
  ZVAL_COPY(&iter->pattern_ref, pattern_zv);
  ZVAL_STR_COPY(&iter->subject_ref, subject);
  iter->pattern = php_snobol_fetch(Z_OBJ_P(pattern_zv));
  iter->subject = ZSTR_VAL(subject);
  iter->subject_len = ZSTR_LEN(subject);
  iter->is_literal_only = iter->pattern->meta.is_literal_only;
  iter->state =
      snobol_pattern_search_state_create(iter->pattern->bc,
                                         iter->pattern->bc_len);
  iter->key = 0;
  iter->started = false;
  iter->valid = false;
  iter->last_end = 0;
  iter->has_trailing = false;
  ZVAL_UNDEF(&iter->current_segment);
  if (!iter->state) {
    zend_throw_exception(zend_ce_exception, "Failed to create search state", 0);
  }
}

/** @brief Register the Snobol\SplitIterator class as an Iterator (MINIT). */
void snobol_split_iterator_minit(void) {
  memcpy(&snobol_split_iterator_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_split_iterator_handlers.offset =
      XtOffsetOf(snobol_split_iterator_t, std);
  snobol_split_iterator_handlers.free_obj = si_dtor;

  zend_class_entry ce;
  INIT_CLASS_ENTRY(ce, "Snobol\\SplitIterator", snobol_split_iterator_methods);
  snobol_split_iterator_ce = zend_register_internal_class(&ce);
  snobol_split_iterator_ce->create_object = si_create;
  zend_class_implements(snobol_split_iterator_ce, 1, zend_ce_iterator);
}
