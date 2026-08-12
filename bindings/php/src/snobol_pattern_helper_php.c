#include "php.h"
#include "php_snobol.h"
#include "zend_exceptions.h"
#include "Zend/zend_interfaces.h"
#include "Zend/zend_smart_str.h"
#include "snobol/snobol.h"

#define SNOBOL_LOG(fmt, ...) ((void)0)

/**
 * @file snobol_pattern_helper_php.c
 * @brief The static Snobol\PatternHelper facade.
 *
 * Convenience entry points over the Pattern class (matchOnce, matchAll,
 * split, replace, subst variants) plus a small slot cache that avoids
 * recompiling the same pattern string.
 */

extern zend_class_entry *snobol_pattern_ce;
zend_class_entry *snobol_pattern_helper_ce;

#include "snobol/table.h"
/* Forward declaration: extract C table pointer from a PHP Snobol\Table zval */
extern snobol_table_t *php_snobol_get_table_from_zval(zval *zv);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/** @brief Compile a pattern source string via Pattern::fromString.
 *  @return SUCCESS when @p ret holds a Pattern object. */
static int php_phelper_call_from_string(zval *pattern_str, zval *options,
                                        zval *ret) {
  zval args[2];
  ZVAL_COPY(&args[0], pattern_str);
  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    ZVAL_COPY(&args[1], options);
  } else {
    array_init(&args[1]);
  }
  zend_call_method(NULL, snobol_pattern_ce, NULL, "fromString",
                   sizeof("fromString") - 1, ret, 2, &args[0], &args[1]);
  zval_ptr_dtor(&args[0]);
  zval_ptr_dtor(&args[1]);
  return (Z_TYPE_P(ret) == IS_OBJECT) ? SUCCESS : FAILURE;
}

/** @brief Compile a Builder AST via Pattern::compileFromAst.
 *  @return SUCCESS when @p ret holds a Pattern object. */
static int php_phelper_call_from_ast(zval *ast, zval *options, zval *ret) {
  zval args[2];
  ZVAL_COPY(&args[0], ast);
  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    ZVAL_COPY(&args[1], options);
  } else {
    array_init(&args[1]);
  }
  zend_call_method(NULL, snobol_pattern_ce, NULL, "compileFromAst",
                   sizeof("compileFromAst") - 1, ret, 2, &args[0], &args[1]);
  zval_ptr_dtor(&args[0]);
  zval_ptr_dtor(&args[1]);
  return (Z_TYPE_P(ret) == IS_OBJECT) ? SUCCESS : FAILURE;
}

/* ------------------------------------------------------------------ */
/*  Simple internal pattern cache (no LRU, fixed-size ring buffer)     */
/* ------------------------------------------------------------------ */

#define PH_CACHE_SLOTS 128

/* Each slot holds a string key, the effective options hash, and a Pattern
 * object zval. */
typedef struct {
  zend_string *key;
  uint32_t options_hash;
  zval value;
  bool valid;
} php_phelper_cache_slot_t;

static php_phelper_cache_slot_t ph_cache[PH_CACHE_SLOTS];

/** @brief djb2-style hash over the string/long values of the options array
 *  (empty/absent options hash to the same value). */
static uint32_t php_phelper_options_hash(zval *options) {
  uint32_t h = 5381;
  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    zend_string *k;
    zval *v;
    ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(options), k, v) {
      if (k) {
        const char *ks = ZSTR_VAL(k);
        for (size_t i = 0; i < ZSTR_LEN(k); i++)
          h = ((h << 5) + h) + (unsigned char)ks[i];
      }
      h = ((h << 5) + h) + (unsigned char)Z_TYPE_P(v);
      if (Z_TYPE_P(v) == IS_STRING) {
        const char *vs = Z_STRVAL_P(v);
        for (size_t i = 0; i < Z_STRLEN_P(v); i++)
          h = ((h << 5) + h) + (unsigned char)vs[i];
      } else if (Z_TYPE_P(v) == IS_LONG) {
        h = ((h << 5) + h) + (unsigned char)(Z_LVAL_P(v) & 0xFF);
      }
    }
    ZEND_HASH_FOREACH_END();
  }
  return h;
}

/* djb2-style hash for the raw pattern string (with or without options). */
/** @brief djb2-style hash over the pattern source and the options hash. */
static uint32_t php_phelper_cache_hash(zval *pattern, zval *options) {
  const char *p = Z_STRVAL_P(pattern);
  size_t plen = Z_STRLEN_P(pattern);
  uint32_t h = 5381;
  for (size_t i = 0; i < plen; i++) {
    h = ((h << 5) + h) + (unsigned char)p[i];
  }
  h = ((h << 5) + h) +
      (unsigned char)(php_phelper_options_hash(options) & 0xFF);
  return h;
}

/* Compare a cache slot against the given pattern+options. */
/** @brief Check a cache slot: the source string must match exactly and the
 *  effective options must hash identically (different options — e.g.
 *  case_insensitive — produce different compiled patterns and must not
 *  share a slot). */
static bool php_phelper_cache_match(php_phelper_cache_slot_t *slot,
                                    zval *pattern, zval *options) {
  if (!slot->valid)
    return false;
  if (!zend_string_equals(slot->key, Z_STR_P(pattern)))
    return false;
  if (slot->options_hash != php_phelper_options_hash(options))
    return false;
  return true;
}

/* Resolve a pattern spec to a Pattern object.
 * Returns SUCCESS and fills `out` (with refcount bumped) or FAILURE.
 * Uses an internal slot-based cache to avoid recompiling the same pattern. */
/** @brief Resolve a pattern spec (Pattern object, string, or AST array) to
 *  a Pattern object, consulting and filling the slot cache.
 *  @return SUCCESS with a refcounted Pattern in @p out, or FAILURE. */
static int php_phelper_resolve(zval *pattern_or_ast, zval *options, zval *out) {
  if (Z_TYPE_P(pattern_or_ast) == IS_OBJECT &&
      instanceof_function(Z_OBJCE_P(pattern_or_ast), snobol_pattern_ce)) {
    ZVAL_COPY(out, pattern_or_ast);
    return SUCCESS;
  }

  if (Z_TYPE_P(pattern_or_ast) == IS_STRING) {
    uint32_t idx =
        php_phelper_cache_hash(pattern_or_ast, options) % PH_CACHE_SLOTS;
    php_phelper_cache_slot_t *slot = &ph_cache[idx];

    if (php_phelper_cache_match(slot, pattern_or_ast, options)) {
      ZVAL_COPY(out, &slot->value);
      return SUCCESS;
    }

    if (php_phelper_call_from_string(pattern_or_ast, options, out) == SUCCESS &&
        Z_TYPE_P(out) == IS_OBJECT) {
      /* Replace slot */
      if (slot->valid) {
        zend_string_release(slot->key);
        zval_ptr_dtor(&slot->value);
      }
      slot->key = zend_string_copy(Z_STR_P(pattern_or_ast));
      slot->options_hash = php_phelper_options_hash(options);
      ZVAL_COPY(&slot->value, out);
      slot->valid = true;
      return SUCCESS;
    }
    return FAILURE;
  }

  if (Z_TYPE_P(pattern_or_ast) == IS_ARRAY) {
    if (php_phelper_call_from_ast(pattern_or_ast, options, out) == SUCCESS &&
        Z_TYPE_P(out) == IS_OBJECT) {
      return SUCCESS;
    }
    return FAILURE;
  }

  return FAILURE;
}

/* Clear the internal pattern cache */
/** @brief Drop every valid slot (used by PatternHelper::clearCache). */
static void php_phelper_cache_clear(void) {
  for (int i = 0; i < PH_CACHE_SLOTS; i++) {
    if (ph_cache[i].valid) {
      zend_string_release(ph_cache[i].key);
      zval_ptr_dtor(&ph_cache[i].value);
      ph_cache[i].valid = false;
    }
  }
}

/* Extract boolean option from options array */
/** @brief Read a boolean option: true for IS_TRUE or any non-zero long. */
static bool php_phelper_get_opt_bool(zval *options, const char *name) {
  if (!options || Z_TYPE_P(options) != IS_ARRAY)
    return false;
  zval *zv = zend_hash_str_find(Z_ARRVAL_P(options), name, strlen(name));
  if (!zv)
    return false;
  return (Z_TYPE_P(zv) == IS_TRUE ||
          (Z_TYPE_P(zv) == IS_LONG && Z_LVAL_P(zv) != 0));
}

/* Strip _match_len and _match_start keys from a match result array */
/** @brief Remove _match_len/_match_start keys from a match result array. */
static void php_phelper_strip_meta(zval *result) {
  if (Z_TYPE_P(result) != IS_ARRAY)
    return;
  zend_hash_str_del(Z_ARRVAL_P(result), "_match_len", sizeof("_match_len") - 1);
  zend_hash_str_del(Z_ARRVAL_P(result), "_match_start",
                    sizeof("_match_start") - 1);
}

/* ------------------------------------------------------------------ */
/*  Argument info                                                      */
/* ------------------------------------------------------------------ */

ZEND_BEGIN_ARG_INFO_EX(ai_ph_match_once, 0, 0, 2)
ZEND_ARG_INFO(0, patternOrAst)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_match_all, 0, 0, 2)
ZEND_ARG_INFO(0, patternOrAst)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_split, 0, 0, 2)
ZEND_ARG_INFO(0, patternOrAst)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_replace, 0, 0, 3)
ZEND_ARG_INFO(0, patternOrAst)
ZEND_ARG_TYPE_INFO(0, replacement, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_from_string, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, pattern, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_from_ast, 0, 0, 1)
ZEND_ARG_ARRAY_INFO(0, ast, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_eval_pattern, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, patternExpr, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_table_subst, 0, 0, 4)
ZEND_ARG_OBJ_INFO(0, table, Snobol\\Table, 0)
ZEND_ARG_TYPE_INFO(0, keyPattern, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, template, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_formatted_subst, 0, 0, 3)
ZEND_ARG_INFO(0, patternOrAst)
ZEND_ARG_TYPE_INFO(0, template, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_ph_clear_cache, 0, 0, 0)
ZEND_END_ARG_INFO()

/* ------------------------------------------------------------------ */
/*  PHP methods                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief PatternHelper::fromString(string $pattern, ?array $options = null): Pattern
 * @param pattern, options
 * @return Pattern object.
 */
PHP_METHOD(Snobol_PatternHelper, fromString) {
  char *pattern;
  size_t pattern_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STRING(pattern, pattern_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_zv;
  ZVAL_STRINGL(&pattern_zv, pattern, pattern_len);
  php_phelper_call_from_string(&pattern_zv, options, return_value);
  zval_ptr_dtor(&pattern_zv);
}

/**
 * @brief PatternHelper::fromAst(array $ast, ?array $options = null): Pattern
 *  Throws ValueError for ASTs without a "type" field or when compilation fails.
 * @param ast, options
 * @return Pattern object.
 */
PHP_METHOD(Snobol_PatternHelper, fromAst) {
  zval *ast;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_ARRAY(ast)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval *type_zv =
      zend_hash_str_find(Z_ARRVAL_P(ast), "type", sizeof("type") - 1);
  if (!type_zv) {
    zend_throw_exception(zend_ce_value_error,
                         "Invalid AST: missing \"type\" field", 0);
    RETURN_NULL();
  }

  php_phelper_call_from_ast(ast, options, return_value);

  if (Z_TYPE_P(return_value) != IS_OBJECT ||
      !instanceof_function(Z_OBJCE_P(return_value), snobol_pattern_ce)) {
    /* compileFromAst throws with the compiler's real message on failure;
     * keep that exception and only fall back to a generic one when the
     * compilation failed silently. */
    if (!EG(exception)) {
      zend_throw_exception(zend_ce_value_error,
                           "Failed to compile pattern from AST", 0);
    }
    RETURN_NULL();
  }
}

/**
 * @brief PatternHelper::matchOnce($patternOrAst, string $subject, ?array $options = null): ?array
 *  Anchored first match; the "full" option restricts to whole-subject
 *  matches. Returns false on no match.
 * @param patternOrAst, subject, options
 * @return Match array or false.
 */
PHP_METHOD(Snobol_PatternHelper, matchOnce) {
  zval *pattern_or_ast;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_ZVAL(pattern_or_ast)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_obj;
  if (php_phelper_resolve(pattern_or_ast, options, &pattern_obj) != SUCCESS) {
    RETURN_FALSE;
  }

  zval args_match[1], match_ret;
  ZVAL_STRINGL(&args_match[0], subject, subject_len);
  zend_call_method_with_1_params(Z_OBJ_P(&pattern_obj), Z_OBJCE_P(&pattern_obj),
                                 NULL, "match", &match_ret, &args_match[0]);

  if (Z_TYPE(match_ret) == IS_FALSE || Z_TYPE(match_ret) == IS_NULL) {
    zval_ptr_dtor(&match_ret);
    zval_ptr_dtor(&args_match[0]);
    zval_ptr_dtor(&pattern_obj);
    RETURN_FALSE;
  }

  bool full = php_phelper_get_opt_bool(options, "full");
  if (full && Z_TYPE(match_ret) == IS_ARRAY) {
    zval *len_zv = zend_hash_str_find(Z_ARRVAL_P(&match_ret), "_match_len",
                                      sizeof("_match_len") - 1);
    if (len_zv && Z_TYPE_P(len_zv) == IS_LONG &&
        Z_LVAL_P(len_zv) == (zend_long)subject_len) {
      ZVAL_COPY(return_value, &match_ret);
    } else {
      RETURN_FALSE;
    }
  } else {
    ZVAL_COPY(return_value, &match_ret);
  }

  zval_ptr_dtor(&match_ret);
  zval_ptr_dtor(&args_match[0]);
  zval_ptr_dtor(&pattern_obj);
}

/**
 * @brief PatternHelper::matchAll($patternOrAst, string $subject, ?array $options = null): array
 *  All matches with _match_len/_match_start stripped.
 * @param patternOrAst, subject, options
 * @return Array of match arrays.
 */
PHP_METHOD(Snobol_PatternHelper, matchAll) {
  zval *pattern_or_ast;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_ZVAL(pattern_or_ast)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_obj;
  if (php_phelper_resolve(pattern_or_ast, options, &pattern_obj) != SUCCESS) {
    array_init(return_value);
    return;
  }

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(&pattern_obj));
  if (!intern->bc || intern->bc_len == 0) {
    array_init(return_value);
    zval_ptr_dtor(&pattern_obj);
    return;
  }

  php_snobol_match_options_t helper_opts;
  helper_opts.metrics = true;
  helper_opts.captures = 0; /* strings */
  helper_opts.result = 0;   /* arrays */
  zval search_ret;
  php_snobol_do_search_all(intern, subject, subject_len, &search_ret,
                           &helper_opts);

  array_init(return_value);
  zval *entry;
  ZEND_HASH_FOREACH_VAL(Z_ARRVAL(search_ret), entry) {
    if (Z_TYPE_P(entry) == IS_ARRAY) {
      zval clean_entry;
      ZVAL_COPY(&clean_entry, entry);
      php_phelper_strip_meta(&clean_entry);
      zend_hash_next_index_insert(Z_ARRVAL_P(return_value), &clean_entry);
    } else {
      /* Copy before moving: the source array still owns its bucket, so a
       * borrowed insert would leave return_value dangling after search_ret
       * is destroyed. */
      zval copy;
      ZVAL_COPY(&copy, entry);
      zend_hash_next_index_insert(Z_ARRVAL_P(return_value), &copy);
    }
  }
  ZEND_HASH_FOREACH_END();

  zval_ptr_dtor(&search_ret);
  zval_ptr_dtor(&pattern_obj);
}

/**
 * @brief PatternHelper::split($patternOrAst, string $subject, ?array $options = null): array
 *  Split segments on non-overlapping matches.
 * @param patternOrAst, subject, options
 * @return Array of segment strings.
 */
PHP_METHOD(Snobol_PatternHelper, split) {
  zval *pattern_or_ast;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_ZVAL(pattern_or_ast)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_obj;
  if (php_phelper_resolve(pattern_or_ast, options, &pattern_obj) != SUCCESS) {
    array_init(return_value);
    return;
  }

  zval args[1], split_ret;
  ZVAL_STRINGL(&args[0], subject, subject_len);
  zend_call_method_with_1_params(Z_OBJ_P(&pattern_obj), Z_OBJCE_P(&pattern_obj),
                                 NULL, "searchSplit", &split_ret, &args[0]);
  if (Z_TYPE(split_ret) == IS_ARRAY) {
    ZVAL_COPY(return_value, &split_ret);
  } else {
    array_init(return_value);
  }
  zval_ptr_dtor(&split_ret);

  zval_ptr_dtor(&args[0]);
  zval_ptr_dtor(&pattern_obj);
}

/**
 * @brief PatternHelper::replace($patternOrAst, string $replacement, string $subject, ?array $options = null): string
 *  Replacements containing '$' route to subst (template); others use
 *  searchReplace.
 * @param patternOrAst, replacement, subject, options
 * @return Replaced string.
 */
PHP_METHOD(Snobol_PatternHelper, replace) {
  zval *pattern_or_ast;
  char *replacement;
  size_t replacement_len;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(3, 4)
  Z_PARAM_ZVAL(pattern_or_ast)
  Z_PARAM_STRING(replacement, replacement_len)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_obj;
  if (php_phelper_resolve(pattern_or_ast, options, &pattern_obj) != SUCCESS) {
    ZVAL_STRINGL(return_value, subject, subject_len);
    return;
  }

  bool has_dollar = (memchr(replacement, '$', replacement_len) != NULL);
  const char *method = has_dollar ? "subst" : "searchReplace";

  zval args[2], method_ret;
  ZVAL_STRINGL(&args[0], subject, subject_len);
  ZVAL_STRINGL(&args[1], replacement, replacement_len);
  zend_call_method_with_2_params(Z_OBJ_P(&pattern_obj), Z_OBJCE_P(&pattern_obj),
                                 NULL, method, &method_ret, &args[0], &args[1]);
  if (Z_TYPE(method_ret) == IS_STRING) {
    ZVAL_COPY(return_value, &method_ret);
  } else {
    ZVAL_STRINGL(return_value, subject, subject_len);
  }
  zval_ptr_dtor(&method_ret);

  zval_ptr_dtor(&args[0]);
  zval_ptr_dtor(&args[1]);
  zval_ptr_dtor(&pattern_obj);
}

/**
 * @brief PatternHelper::clearCache(): void
 *  Empties the internal slot cache.
 */
PHP_METHOD(Snobol_PatternHelper, clearCache) {
  ZEND_PARSE_PARAMETERS_NONE();
  php_phelper_cache_clear();
}

/**
 * @brief PatternHelper::evalPattern(string $patternExpr, string $subject, ?array $options = null): mixed
 *  Compiles and matches in one step; parse failures propagate as
 *  exceptions.
 * @param patternExpr, subject, options
 * @return Match array or false.
 */
PHP_METHOD(Snobol_PatternHelper, evalPattern) {
  char *pattern_expr;
  size_t pattern_expr_len;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_STRING(pattern_expr, pattern_expr_len)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_zv;
  ZVAL_STRINGL(&pattern_zv, pattern_expr, pattern_expr_len);

  zval pattern_obj;
  if (php_phelper_call_from_string(&pattern_zv, options, &pattern_obj) !=
          SUCCESS ||
      Z_TYPE(pattern_obj) != IS_OBJECT) {
    zval_ptr_dtor(&pattern_zv);
    RETURN_FALSE;
  }

  zval args_match[1], match_ret;
  ZVAL_STRINGL(&args_match[0], subject, subject_len);
  zend_call_method_with_1_params(Z_OBJ_P(&pattern_obj), Z_OBJCE_P(&pattern_obj),
                                 NULL, "match", &match_ret, &args_match[0]);
  if (Z_TYPE(match_ret) != IS_FALSE && Z_TYPE(match_ret) != IS_NULL) {
    ZVAL_COPY(return_value, &match_ret);
  } else {
    ZVAL_FALSE(return_value);
  }
  zval_ptr_dtor(&match_ret);

  zval_ptr_dtor(&args_match[0]);
  zval_ptr_dtor(&pattern_obj);
  zval_ptr_dtor(&pattern_zv);
}

/**
 * @brief PatternHelper::tableSubst(Table $table, string $keyPattern, string $template, string $subject): string
 *  Substitute matches of $keyPattern using $template (see Pattern::subst).
 * @param table, keyPattern, template, subject
 * @return Replaced string.
 */
PHP_METHOD(Snobol_PatternHelper, tableSubst) {
  zval *table_zv;
  char *key_pattern;
  size_t key_pattern_len;
  char *template_str;
  size_t template_str_len;
  char *subject;
  size_t subject_len;

  ZEND_PARSE_PARAMETERS_START(4, 4)
  Z_PARAM_OBJECT(table_zv)
  Z_PARAM_STRING(key_pattern, key_pattern_len)
  Z_PARAM_STRING(template_str, template_str_len)
  Z_PARAM_STRING(subject, subject_len)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_zv;
  ZVAL_STRINGL(&pattern_zv, key_pattern, key_pattern_len);

  zval pattern_obj;
  if (php_phelper_call_from_string(&pattern_zv, NULL, &pattern_obj) !=
          SUCCESS ||
      Z_TYPE(pattern_obj) != IS_OBJECT) {
    ZVAL_STRINGL(return_value, subject, subject_len);
    zval_ptr_dtor(&pattern_zv);
    return;
  }

  zval args_subst[3], subst_ret;
  ZVAL_STRINGL(&args_subst[0], subject, subject_len);
  ZVAL_STRINGL(&args_subst[1], template_str, template_str_len);
  /* Pass the table to subst so template lookups like $STATE[$v0] resolve
     against it. subst binds tables by name, so the array key must be the
     table's own name (this is what makes the helper table-backed). */
  array_init(&args_subst[2]);
  {
    snobol_table_t *ct = php_snobol_get_table_from_zval(table_zv);
    const char *tbl_name = ct ? table_name(ct) : NULL;
    if (ct && tbl_name) {
      zval copy;
      ZVAL_COPY(&copy, table_zv);
      add_assoc_zval(&args_subst[2], tbl_name, &copy);
    } else {
      Z_TRY_ADDREF_P(table_zv);
      add_next_index_zval(&args_subst[2], table_zv);
    }
  }
  ZVAL_UNDEF(&subst_ret);
  {
    zend_fcall_info fci;
    zend_fcall_info_cache fcc;
    zend_string *callable_name = NULL;
    /* The callable is [$pattern_obj, 'subst'] — a bare object is not
     * callable. */
    zval callable;
    array_init(&callable);
    zval obj_copy;
    ZVAL_COPY(&obj_copy, &pattern_obj);
    add_next_index_zval(&callable, &obj_copy);
    zval mname;
    ZVAL_STRINGL(&mname, "subst", sizeof("subst") - 1);
    add_next_index_zval(&callable, &mname);
    if (zend_fcall_info_init(&callable, 0, &fci, &fcc, &callable_name, NULL) ==
        SUCCESS) {
      fci.retval = &subst_ret;
      fci.params = args_subst;
      fci.param_count = 3;
      zend_call_function(&fci, &fcc);
    }
    zval_ptr_dtor(&callable);
    if (callable_name) {
      zend_string_release(callable_name);
    }
  }
  if (Z_TYPE(subst_ret) == IS_STRING) {
    ZVAL_COPY(return_value, &subst_ret);
  } else {
    ZVAL_STRINGL(return_value, subject, subject_len);
  }
  if (Z_TYPE(subst_ret) != IS_UNDEF) {
    zval_ptr_dtor(&subst_ret);
  }

  zval_ptr_dtor(&args_subst[0]);
  zval_ptr_dtor(&args_subst[1]);
  zval_ptr_dtor(&args_subst[2]);
  zval_ptr_dtor(&pattern_obj);
  zval_ptr_dtor(&pattern_zv);
}

/**
 * @brief PatternHelper::formattedSubst($patternOrAst, string $template, string $subject, ?array $options = null): string
 *  Template substitution (captures/format specs) over all matches.
 * @param patternOrAst, template, subject, options
 * @return Replaced string.
 */
PHP_METHOD(Snobol_PatternHelper, formattedSubst) {
  zval *pattern_or_ast;
  char *template_str;
  size_t template_str_len;
  char *subject;
  size_t subject_len;
  zval *options = NULL;

  ZEND_PARSE_PARAMETERS_START(3, 4)
  Z_PARAM_ZVAL(pattern_or_ast)
  Z_PARAM_STRING(template_str, template_str_len)
  Z_PARAM_STRING(subject, subject_len)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY(options)
  ZEND_PARSE_PARAMETERS_END();

  zval pattern_obj;
  if (php_phelper_resolve(pattern_or_ast, options, &pattern_obj) != SUCCESS) {
    ZVAL_STRINGL(return_value, subject, subject_len);
    return;
  }

  zval args_subst[2], subst_ret;
  ZVAL_STRINGL(&args_subst[0], subject, subject_len);
  ZVAL_STRINGL(&args_subst[1], template_str, template_str_len);
  zend_call_method_with_2_params(Z_OBJ_P(&pattern_obj), Z_OBJCE_P(&pattern_obj),
                                 NULL, "subst", &subst_ret, &args_subst[0],
                                 &args_subst[1]);
  if (Z_TYPE(subst_ret) == IS_STRING) {
    ZVAL_COPY(return_value, &subst_ret);
  } else {
    ZVAL_STRINGL(return_value, subject, subject_len);
  }
  zval_ptr_dtor(&subst_ret);

  zval_ptr_dtor(&args_subst[0]);
  zval_ptr_dtor(&args_subst[1]);
  zval_ptr_dtor(&pattern_obj);
}

static const zend_function_entry snobol_pattern_helper_methods[] = {
    PHP_ME(Snobol_PatternHelper, fromString, ai_ph_from_string,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(Snobol_PatternHelper,
                                                     fromAst, ai_ph_from_ast,
                                                     ZEND_ACC_PUBLIC |
                                                         ZEND_ACC_STATIC)
        PHP_ME(Snobol_PatternHelper, matchOnce, ai_ph_match_once,
               ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
            PHP_ME(Snobol_PatternHelper, matchAll, ai_ph_match_all,
                   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                PHP_ME(Snobol_PatternHelper, split, ai_ph_split,
                       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                    PHP_ME(Snobol_PatternHelper, replace, ai_ph_replace,
                           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                        PHP_ME(Snobol_PatternHelper, clearCache,
                               ai_ph_clear_cache,
                               ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                            PHP_ME(Snobol_PatternHelper, evalPattern,
                                   ai_ph_eval_pattern,
                                   ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                                PHP_ME(Snobol_PatternHelper, tableSubst,
                                       ai_ph_table_subst,
                                       ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                                    PHP_ME(Snobol_PatternHelper, formattedSubst,
                                           ai_ph_formatted_subst,
                                           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
                                        PHP_FE_END};

/** @brief Register the Snobol\PatternHelper class (MINIT). */
void snobol_pattern_helper_php_minit(void) {
  zend_class_entry ce;
  INIT_CLASS_ENTRY(ce, "Snobol\\PatternHelper", snobol_pattern_helper_methods);
  snobol_pattern_helper_ce = zend_register_internal_class(&ce);
}
