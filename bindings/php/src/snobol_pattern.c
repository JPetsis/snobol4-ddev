#include "php.h"
#include "php_snobol.h"
#include "zend_exceptions.h"
#include "snobol/compiler.h"
#include "snobol/vm.h"
#include "snobol/lexer.h"
#include "snobol/parser.h"
#include "snobol/snobol.h"
#include "snobol/snobol_internal.h"
#include "snobol/search.h"
#include "snobol/table.h"

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

/* Forward declaration: extract C table pointer from a PHP Snobol\Table zval */
extern snobol_table_t *php_snobol_get_table_from_zval(zval *zv);

/* DEBUG LOGGING DISABLED
static inline void snobol_log_impl(const char *file, int line, const char *fmt, ...) {
    FILE *f = fopen("/var/www/html/snobol_debug.log", "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);
        fprintf(f, "[%s] [%s:%d] ", ts, file, line); 
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fprintf(f, "\n");
        fflush(f);
        fclose(f);
    }
}
*/
/* No-op macro to disable logging */
#define SNOBOL_LOG(fmt, ...) ((void)0)

extern zend_class_entry *snobol_pattern_ce;
static zend_object_handlers snobol_pattern_object_handlers;

/** @brief Free a Pattern object: releases bytecode, caches, search state and eval callbacks. */
static void php_snobol_pattern_dtor(zend_object *object) {
  snobol_pattern_t *intern = php_snobol_fetch(object);
  SNOBOL_LOG("php_snobol_pattern_dtor: intern=%p, bc=%p", (void *)intern,
             (void *)intern->bc);

  if (intern->bc) {
    compiler_free(intern->bc);
    intern->bc = NULL;
  }
  if (intern->meta.bmh_skip) {
    snobol_free(intern->meta.bmh_skip);
    intern->meta.bmh_skip = NULL;
  }
  if (intern->range_meta) {
    snobol_free(intern->range_meta);
    intern->range_meta = NULL;
  }
  if (intern->trie_cache) {
    snobol_auto_trie_free(intern->trie_cache);
    intern->trie_cache = NULL;
  }
  if (intern->search_state) {
    snobol_pattern_search_state_destroy(intern->search_state);
    intern->search_state = NULL;
  }
  if (Z_TYPE(intern->eval_callbacks) != IS_UNDEF) {
    zval_ptr_dtor(&intern->eval_callbacks);
    ZVAL_UNDEF(&intern->eval_callbacks);
  }

  zend_object_std_dtor(object);
  SNOBOL_LOG("php_snobol_pattern_dtor: done");
}

/** @brief Object factory for Snobol\Pattern: zeroes the internals and installs handlers. */
static zend_object *snobol_pattern_create(zend_class_entry *ce) {
  snobol_pattern_t *intern = zend_object_alloc(sizeof(snobol_pattern_t), ce);
  SNOBOL_LOG("snobol_pattern_create: intern=%p", (void *)intern);

  intern->bc = NULL;
  intern->bc_len = 0;
  intern->trie_cache = NULL;

  zend_object_std_init(&intern->std, ce);
  object_properties_init(&intern->std, ce);
  intern->std.handlers = &snobol_pattern_object_handlers;

  return &intern->std;
}

/* argument info */
ZEND_BEGIN_ARG_INFO_EX(ai_compileFromAst, 0, 0, 1)
ZEND_ARG_ARRAY_INFO(0, ast, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_fromString, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, source, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_match, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, input, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_subst, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, template, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, tables, IS_ARRAY, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_setEval, 0, 0, 1)
ZEND_ARG_ARRAY_INFO(0, callbacks, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_setJit, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, enabled, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_searchAll, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_matchLiteral, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, input, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_searchSplit, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_searchSplitOffsets, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_searchReplace, 0, 0, 2)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, replacement, IS_STRING, 0)
ZEND_ARG_ARRAY_INFO(0, options, 1)
ZEND_END_ARG_INFO()

/* Parse the $options array into a php_snobol_match_options_t.
 * Fields not present in the array keep their default (legacy) value. */
/** @brief Implementation of php_snobol_parse_match_options() (see php_snobol.h). */
void php_snobol_parse_match_options(zval *options_zv,
                                    php_snobol_match_options_t *opts) {
  opts->metrics = false;
  opts->captures = PHP_SNOBOL_CAPTURES_STRINGS;
  opts->result = PHP_SNOBOL_RESULT_ARRAYS;

  if (!options_zv || Z_TYPE_P(options_zv) != IS_ARRAY)
    return;

  zval *v;
  v = zend_hash_str_find(Z_ARRVAL_P(options_zv), "metrics",
                         sizeof("metrics") - 1);
  if (v)
    opts->metrics = zend_is_true(v);

  v = zend_hash_str_find(Z_ARRVAL_P(options_zv), "captures",
                         sizeof("captures") - 1);
  if (v && Z_TYPE_P(v) == IS_STRING) {
    if (strcmp(Z_STRVAL_P(v), "offsets") == 0)
      opts->captures = PHP_SNOBOL_CAPTURES_OFFSETS;
  }

  v = zend_hash_str_find(Z_ARRVAL_P(options_zv), "result",
                         sizeof("result") - 1);
  if (v && Z_TYPE_P(v) == IS_STRING) {
    if (strcmp(Z_STRVAL_P(v), "flat") == 0)
      opts->result = PHP_SNOBOL_RESULT_FLAT;
  }
}

/* PHP Methods */

/** @brief Pattern::compileFromAst(array $ast, ?array $options = null): Pattern
 *  Compiles a Builder-format PHP AST to bytecode and returns a Pattern. */
PHP_METHOD(Snobol_Pattern, compileFromAst) {
  zval *ast;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_ARRAY(ast)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  uint8_t *bc = NULL;
  size_t bc_len = 0;

  SNOBOL_LOG("Snobol_Pattern::compileFromAst: START");

  if (compile_ast_to_bytecode(ast, options, &bc, &bc_len) != 0) {
    SNOBOL_LOG("Snobol_Pattern::compileFromAst: compilation FAILED");
    zend_throw_exception(zend_ce_exception, "Failed to compile AST", 0);
    RETURN_NULL();
  }

  if (object_init_ex(return_value, snobol_pattern_ce) != SUCCESS) {
    SNOBOL_LOG("Snobol_Pattern::compileFromAst: object_init_ex FAILED");
    if (bc)
      compiler_free(bc);
    RETURN_NULL();
  }

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(return_value));
  intern->bc = bc;
  intern->bc_len = bc_len;
  /* Cache search metadata at compile time */
  snobol_search_derive_meta(bc, bc_len, &intern->meta);
  intern->meta_initialized = true;
  /* Build range metadata for SPAN/BREAK/BREAKX charclass resolution */
  snobol_build_range_meta(bc, bc_len, &intern->range_meta,
                          &intern->range_meta_count);

  SNOBOL_LOG(
      "Snobol_Pattern::compileFromAst: SUCCESS, intern=%p, bc=%p, len=%zu",
      (void *)intern, (void *)bc, bc_len);
}

/**
 * Pattern::fromString(string $source, ?array $options = null): Pattern
 * 
 * Parse and compile a pattern from source text using the C parser.
 * This is the new language-agnostic compilation path.
 */
PHP_METHOD(Snobol_Pattern, fromString) {
  zend_string *source;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STR(source)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  SNOBOL_LOG("Snobol_Pattern::fromString: START, source='%.*s'",
             (int)ZSTR_LEN(source), ZSTR_VAL(source));

  /* Create lexer and parser */
  snobol_lexer_t *lexer =
      snobol_lexer_create(ZSTR_VAL(source), ZSTR_LEN(source));
  if (!lexer) {
    zend_throw_exception(zend_ce_exception, "Failed to create lexer", 0);
    RETURN_NULL();
  }

  snobol_parser_t *parser = snobol_parser_create();
  if (!parser) {
    snobol_lexer_destroy(lexer);
    zend_throw_exception(zend_ce_exception, "Failed to create parser", 0);
    RETURN_NULL();
  }

  /* Parse the source */
  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  if (snobol_parser_has_error(parser)) {
    const char *error = snobol_parser_get_error(parser);
    size_t line, col;
    snobol_parser_get_error_location(parser, &line, &col);

    char msg[512];
    snprintf(msg, sizeof(msg), "Parse error at line %zu, column %zu: %s", line,
             col, error ? error : "unknown error");

    snobol_parser_destroy(parser);
    snobol_lexer_destroy(lexer);
    zend_throw_exception(zend_ce_exception, msg, 0);
    RETURN_NULL();
  }

  if (!ast) {
    snobol_parser_destroy(parser);
    snobol_lexer_destroy(lexer);
    zend_throw_exception(zend_ce_exception, "Parser returned NULL AST", 0);
    RETURN_NULL();
  }

  /* Extract caseInsensitive option */
  bool case_insensitive = false;
  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    zval *ci = zend_hash_str_find(Z_ARRVAL_P(options), "caseInsensitive",
                                  sizeof("caseInsensitive") - 1);
    if (ci && zend_is_true(ci)) {
      case_insensitive = true;
    }
  }

  /* Compile AST to bytecode */
  uint8_t *bc = NULL;
  size_t bc_len = 0;

  if (compile_ast_to_bytecode_c(ast, case_insensitive, &bc, &bc_len) != 0) {
    SNOBOL_LOG("Snobol_Pattern::fromString: compilation FAILED");
    snobol_ast_free(ast);
    snobol_parser_destroy(parser);
    snobol_lexer_destroy(lexer);
    zend_throw_exception(zend_ce_exception, "Failed to compile AST", 0);
    RETURN_NULL();
  }

  /* Free AST - bytecode is now compiled */
  snobol_ast_free(ast);
  snobol_parser_destroy(parser);
  snobol_lexer_destroy(lexer);

  /* Create Pattern object */
  if (object_init_ex(return_value, snobol_pattern_ce) != SUCCESS) {
    SNOBOL_LOG("Snobol_Pattern::fromString: object_init_ex FAILED");
    if (bc)
      compiler_free(bc);
    RETURN_NULL();
  }

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(return_value));
  intern->bc = bc;
  intern->bc_len = bc_len;
  /* Cache search metadata at compile time */
  snobol_search_derive_meta(bc, bc_len, &intern->meta);
  intern->meta_initialized = true;
  /* Build range metadata for SPAN/BREAK/BREAKX charclass resolution */
  snobol_build_range_meta(bc, bc_len, &intern->range_meta,
                          &intern->range_meta_count);

  SNOBOL_LOG("Snobol_Pattern::fromString: SUCCESS, intern=%p, bc=%p, len=%zu",
             (void *)intern, (void *)bc, bc_len);
}

/* ---------------------------------------------------------------------------
 * Eval callback dispatcher.
 *
 * Called by the VM when it encounters an OP_EVAL instruction.
 * Looks up fn_id in the pattern's cached eval_callbacks array,
 * calls the registered PHP callable with the matched substring,
 * and returns true/false accordingly.
 *
 * The userdata pointer carries the snobol_pattern_t internals so
 * we can access the cached callbacks without per-call allocation.
 * --------------------------------------------------------------------------- */
static bool php_snobol_eval_cb(int fn_id, const char *s, size_t start,
                               size_t end, void *userdata) {
  snobol_pattern_t *intern = (snobol_pattern_t *)userdata;
  if (!intern || Z_TYPE(intern->eval_callbacks) == IS_UNDEF)
    return false;

  zval *cb =
      zend_hash_index_find(Z_ARRVAL(intern->eval_callbacks), (zend_ulong)fn_id);
  if (!cb || !zend_is_callable(cb, 0, NULL))
    return false;

  /* Extract the substring that matched the EVAL operand */
  size_t sub_len = (end > start) ? end - start : 0;
  zval retval;
  zval args[1];
  if (sub_len > 0 && start < end) {
    ZVAL_STRINGL(&args[0], s + start, sub_len);
  } else {
    ZVAL_EMPTY_STRING(&args[0]);
  }

  if (call_user_function(NULL, NULL, cb, &retval, 1, args) != SUCCESS) {
    zval_ptr_dtor(&args[0]);
    return false;
  }

  bool ok = zend_is_true(&retval);
  zval_ptr_dtor(&retval);
  zval_ptr_dtor(&args[0]);
  return ok;
}

/* ---------------------------------------------------------------------------
 * Shared capture emission.
 *
 * One code path for emitting a capture register into a PHP result array, used
 * by match(), searchAll (flat + array modes) and the batch builders, so the
 * v<reg> key convention, the bounds checks, and the empty-capture rendering
 * (null) cannot drift between the search methods.
 *
 * @p target  assoc mode: array receiving add_assoc_* entries under @p key;
 *            flat mode: per-register array receiving add_next_index_* entries.
 * @p coff    subject-absolute capture start (caller adds the window base).
 * @p clen    capture length.  A zero-length or out-of-bounds capture renders
 *            as null (string mode) or a [coff, 0] pair (offsets mode).
 * ------------------------------------------------------------------------- */
static void php_snobol_emit_capture(zval *target, bool assoc,
                                    const char *subject, size_t subject_len,
                                    size_t coff, size_t clen, const char *key,
                                    size_t key_len, int captures_mode) {
  if (captures_mode == PHP_SNOBOL_CAPTURES_OFFSETS) {
    zval pair;
    array_init(&pair);
    add_next_index_long(&pair, (zend_long)coff);
    add_next_index_long(&pair, (zend_long)clen);
    if (assoc) {
      snobol_assoc_zval(target, key, key_len, &pair);
      zval_ptr_dtor(&pair);
    } else {
      zend_hash_next_index_insert(Z_ARRVAL_P(target), &pair);
    }
  } else if (clen > 0 && coff + clen <= subject_len) {
    if (assoc)
      add_assoc_stringl(target, key, subject + coff, clen);
    else
      add_next_index_stringl(target, subject + coff, clen);
  } else {
    if (assoc)
      add_assoc_null(target, key);
    else
      add_next_index_null(target);
  }
}
/* ---------------------------------------------------------------------------
 * Anchored first-match via persistent search state.
 *
 * Routes Pattern::match() through the tier dispatch + prefilter path,
 * reusing the cached VM, DFA, and range_meta on intern->search_state.
 * Returns true and populates result on success.  On false the caller
 * should RETURN_FALSE (result is uninitialised).
 *
 * The literal fast path is handled separately by the caller; this
 * function is only called for non-literal patterns.
 * --------------------------------------------------------------------------- */
bool php_snobol_do_match(snobol_pattern_t *intern, const char *subject_val,
                         size_t subject_len, zval *result,
                         const php_snobol_match_options_t *opts) {
  /* Lazy-create persistent search state so caches (VM, DFA, range_meta)
     * are built once and reused across calls. */
  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state)
      return false;
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  /* Pre-build and cache the alt-literals trie so the tier dispatch
     * can use it without rebuilding per call. */
  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  /* Wire up cached eval callbacks on the persistent VM so the
     * callback function pointer persists across calls (no per-call
     * allocation).  The userdata is the PHP pattern struct itself,
     * which holds the cached callbacks array. */
  if (Z_TYPE(intern->eval_callbacks) != IS_UNDEF) {
    snobol_pattern_search_state_set_eval_fn(state, php_snobol_eval_cb, intern);
  }

  /* Anchored search via persistent state — reuses VM, DFA, range_meta,
     * output buffer, and caches.  Match must start at offset 0. */
  snobol_match_t *m =
      snobol_pattern_search_ex_anchored(state, subject_val, subject_len);
  if (!m || !snobol_match_success(m))
    return false;

  /* ---- Build result array ---- */
  array_init(result);

  /* Match length */
  size_t match_len = snobol_match_get_length(m);
  add_assoc_long(result, "_match_len", (zend_long)match_len);
  add_assoc_long(result, "_match_start",
                 (zend_long)snobol_match_get_position(m));

  /* Captures: register-indexed keys "v0", "v1", …
     * var_off[i]/var_len[i] are subject-absolute offsets for anchored match. */
  for (size_t i = 0; i < (size_t)m->var_count; ++i) {
    char key[32];
    size_t key_len = snprintf(key, sizeof(key), "v%u", (unsigned)i);
    php_snobol_emit_capture(result, true, subject_val, subject_len,
                            m->var_off[i], m->var_len[i], key, key_len,
                            opts->captures);
  }

  /* Output buffer (from OP_EMIT_* instructions) */
  size_t out_len = 0;
  const char *output = snobol_match_get_output(m, &out_len);
  if (output && out_len > 0) {
    add_assoc_stringl(result, "_output", output, out_len);
  } else {
    add_assoc_string(result, "_output", "");
  }

  /* Per-match metrics (opt-in via opts.metrics) */
  if (opts->metrics) {
    zval metrics;
    array_init(&metrics);
    add_assoc_long(&metrics, "choice_push_count", 0);
    add_assoc_long(&metrics, "choice_allocated", 0);
    add_assoc_long(&metrics, "choice_peak_depth", 0);
    add_assoc_long(&metrics, "choice_peak_memory", 0);
    snobol_assoc_zval(result, "_metrics", 8, &metrics);
    zval_ptr_dtor(&metrics);
  }

  return true;
}


/** @brief Pattern::match(string $subject, ?array $options = null): array|false
 *  Anchored first match. Literal-only patterns take a direct memcmp fast
 *  path; everything else routes through the persistent search state. */
PHP_METHOD(Snobol_Pattern, match) {
  zend_string *input;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STR(input)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  php_snobol_match_options_t opts;
  php_snobol_parse_match_options(options, &opts);

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  SNOBOL_LOG("Snobol_Pattern::match: START, intern=%p, bc=%p, input_len=%zu",
             (void *)intern, (void *)intern->bc, ZSTR_LEN(input));

  if (!intern->bc || intern->bc_len == 0) {
    SNOBOL_LOG("Snobol_Pattern::match: ABORT, no bytecode");
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  /* Fast path: literal-only pattern — direct memcmp, no VM setup,
     * no tier dispatch, no prefilter scan.  The bytecode for a pure
     * literal is OP_LIT(off, len) followed by OP_ACCEPT (plus optional
     * NOP/FENCE/ANCHOR).  Extract the literal and compare at position 0.
     * This is the same approach as the original fast-path removed by P4
     * but simplified — we skip the POS/RPOS scan since is_literal_only
     * already guarantees there are no position constraints. */
  if (intern->meta.is_literal_only) {
    const uint8_t *bc = intern->bc;
    size_t bc_len = intern->bc_len;
    size_t ip = 0;
    while (ip < bc_len) {
      uint8_t op = bc[ip];
      if (op == OP_NOP || op == OP_FENCE || op == OP_ANCHOR ||
          op == OP_ACCEPT || op == OP_ABORT) {
        ip++;
        continue;
      }
      if ((op == OP_POS || op == OP_RPOS) && ip + 5 <= bc_len) {
        ip += 5;
        continue;
      }
      break;
    }
    if (ip + 9 <= bc_len && bc[ip] == OP_LIT) {
      uint32_t lit_off = ((uint32_t)bc[ip + 1] << 24) |
                         ((uint32_t)bc[ip + 2] << 16) |
                         ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
      uint32_t lit_len = ((uint32_t)bc[ip + 5] << 24) |
                         ((uint32_t)bc[ip + 6] << 16) |
                         ((uint32_t)bc[ip + 7] << 8) | (uint32_t)bc[ip + 8];
      /* Bounds-validate the literal operand before reading it; on a
         malformed layout fall through to the general search path. */
      if (lit_off + lit_len <= bc_len) {
        const char *lit = (const char *)(bc + lit_off);
        if (ZSTR_LEN(input) >= lit_len &&
            memcmp(ZSTR_VAL(input), lit, lit_len) == 0) {
          array_init(return_value);
          add_assoc_long(return_value, "_match_len", (zend_long)lit_len);
          add_assoc_long(return_value, "_match_start", 0);
          add_assoc_string(return_value, "_output", "");
          if (opts.metrics) {
            zval metrics;
            array_init(&metrics);
            add_assoc_long(&metrics, "choice_push_count", 0);
            add_assoc_long(&metrics, "choice_allocated", 0);
            add_assoc_long(&metrics, "choice_peak_depth", 0);
            add_assoc_long(&metrics, "choice_peak_memory", 0);
            snobol_assoc_zval(return_value, "_metrics", 8, &metrics);
            zval_ptr_dtor(&metrics);
          }
          return;
        }
        RETURN_FALSE;
      }
      /* Malformed operand region: fall through to the general path. */
    }
  }

  /* Non-literal patterns: route through the persistent-state tier
     * dispatch, reusing the cached VM, DFA, range_meta, and output
     * buffer for zero per-call allocation overhead. */
  if (php_snobol_do_match(intern, ZSTR_VAL(input), ZSTR_LEN(input),
                          return_value, &opts)) {
    return;
  }
  RETURN_FALSE;
}

/** @brief Pattern::subst(string $subject, string $template, ?array $tables = null): string
 *  Replaces every match with the compiled template (captures, format specs,
 *  optional table bindings) in a single pass. */
PHP_METHOD(Snobol_Pattern, subst) {
  zend_string *subject, *tpl_str;
  zval *tables_zval = NULL;
  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_STR(subject)
  Z_PARAM_STR(tpl_str)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_EX(tables_zval, 1, 0)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  uint8_t *tpl_bc = NULL;
  size_t tpl_bc_len = 0;
  if (compile_template_to_bytecode(ZSTR_VAL(tpl_str), ZSTR_LEN(tpl_str),
                                   &tpl_bc, &tpl_bc_len) != 0) {
    zend_throw_exception(zend_ce_exception, "Failed to compile template", 0);
    RETURN_FALSE;
  }

#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Collect tables from the optional third parameter and bind by name */
  snobol_table_t **php_tables = NULL;
  const char **tbl_names = NULL;
  uint16_t *tbl_ids = NULL;
  size_t tbl_count = 0;

  if (tables_zval && Z_TYPE_P(tables_zval) == IS_ARRAY) {
    HashTable *ht = Z_ARRVAL_P(tables_zval);
    size_t raw_count = zend_hash_num_elements(ht);
    if (raw_count > 0) {
      php_tables =
          (snobol_table_t **)emalloc(raw_count * sizeof(snobol_table_t *));
      tbl_names = (const char **)emalloc(raw_count * sizeof(const char *));
      tbl_ids = (uint16_t *)emalloc(raw_count * sizeof(uint16_t));
      zend_string *tbl_key;
      zval *entry;
      ZEND_HASH_FOREACH_STR_KEY_VAL(ht, tbl_key, entry) {
        if (!tbl_key)
          continue; /* skip integer-keyed entries */
        snobol_table_t *ct = php_snobol_get_table_from_zval(entry);
        if (ct) {
          php_tables[tbl_count] = ct;
          tbl_names[tbl_count] =
              ZSTR_VAL(tbl_key); /* use PHP array key as binding name */
          tbl_ids[tbl_count] = (uint16_t)tbl_count;
          tbl_count++;
        }
      }
      ZEND_HASH_FOREACH_END();
    }
  }

  /* Always bind (even with tbl_count==0) so unresolved table refs are detected */
  {
    int bind_rc = snobol_template_bind_tables(tpl_bc, tpl_bc_len, tbl_names,
                                              tbl_ids, tbl_count);
    if (bind_rc != 0) {
      if (php_tables) {
        efree(php_tables);
        php_tables = NULL;
      }
      if (tbl_names) {
        efree(tbl_names);
        tbl_names = NULL;
      }
      if (tbl_ids) {
        efree(tbl_ids);
        tbl_ids = NULL;
      }
      compiler_free(tpl_bc);
      zend_throw_exception(zend_ce_exception,
                           "Template references an unregistered table name", 0);
      RETURN_FALSE;
    }
  }
#endif /* SNOBOL_DYNAMIC_PATTERN */

  snobol_buf out;
  snobol_buf_init(&out);

  const char *subject_val = ZSTR_VAL(subject);
  size_t subject_len = ZSTR_LEN(subject);
  size_t last_match_end = 0;

  /* Lazy-create the persistent search state so the search phase reuses the
     cached VM, DFA, range_meta and eval wiring (mirrors match()/searchAll()
     — no per-iteration stack-VM memset). */
  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      zend_throw_exception(zend_ce_exception, "Out of memory", 0);
      snobol_buf_free(&out);
      RETURN_FALSE;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  /* Pre-build and cache the alt-literals trie so the tier dispatch can use
     it without rebuilding per call. */
  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  /* Wire up cached eval callbacks on the persistent VM. */
  if (Z_TYPE(intern->eval_callbacks) != IS_UNDEF) {
    snobol_pattern_search_state_set_eval_fn(state, php_snobol_eval_cb, intern);
  }

  /* Core search loop through the persistent state. */
  size_t search_offset = 0;
  while (search_offset <= subject_len) {
    snobol_match_t *m = snobol_pattern_search_ex(state, subject_val,
                                                 subject_len, search_offset);
    if (!m || !snobol_match_success(m))
      break;

    size_t match_start = snobol_match_get_position(m);
    size_t match_end = match_start + snobol_match_get_length(m);

    /* Append prefix */
    snobol_buf_append(&out, subject_val + last_match_end,
                      match_start - last_match_end);

    /* Run the template bytecode on a VM reconstructed from the match.
       Cap registers are window-relative, so anchor s at the match start
       (subject + match position), exactly like the search VM did. */
    VM tvm;
    memset(&tvm, 0, sizeof(tvm));
    tvm.s = subject_val + match_start;
    tvm.len = subject_len - match_start;
    tvm.bc = tpl_bc;
    tvm.bc_len = tpl_bc_len;
    tvm.ip = 0;
    tvm.out = &out;
    tvm.var_count = m->var_count;
    for (size_t ri = 0; ri < m->var_count && ri < MAX_CAPS; ri++) {
      tvm.cap_start[ri] = m->var_off[ri];
      tvm.cap_end[ri] = m->var_off[ri] + m->var_len[ri];
    }

#ifdef SNOBOL_DYNAMIC_PATTERN
    /* Register tables in the same sequential order as the bind step */
    if (tbl_count > 0) {
      vm_init_tables(&tvm);
      for (size_t k = 0; k < tbl_count; k++) {
        uint16_t assigned_id;
        vm_register_table(&tvm, php_tables[k], &assigned_id);
        (void)assigned_id;
      }
    }
    dynamic_pattern_cache_t dyn_cache;
    if (dynamic_pattern_cache_init(&dyn_cache, 64)) {
      tvm.dyn_cache = &dyn_cache;
    } else {
      tvm.dyn_cache = NULL;
    }
    tvm.dyn_pending_source = NULL;
    tvm.dyn_pending_source_len = 0;
    tvm.dyn_pending_bc = NULL;
    tvm.dyn_pending_bc_len = 0;
#endif

    vm_run(&tvm);

#ifdef SNOBOL_DYNAMIC_PATTERN
    if (tbl_count > 0)
      vm_free_tables(&tvm);
    if (tvm.array_count > 0)
      vm_free_arrays(&tvm);
    if (tvm.dyn_cache)
      dynamic_pattern_cache_destroy(tvm.dyn_cache);
    /* dyn_pending_source / dyn_pending_bc are owned and freed by the core
     * (snobol_free, and nulled once consumed) — never freed from here. */
#endif

    /* Advance past the match */
    size_t match_len = match_end - match_start;
    if (match_len == 0)
      match_len = 1;
    search_offset = match_start + match_len;
    last_match_end = search_offset;

    if (search_offset > subject_len)
      break;
  }

  /* Append remainder */
  if (last_match_end < subject_len) {
    snobol_buf_append(&out, subject_val + last_match_end,
                      subject_len - last_match_end);
  }

#ifdef SNOBOL_DYNAMIC_PATTERN
  if (php_tables)
    efree(php_tables);
  if (tbl_names)
    efree(tbl_names);
  if (tbl_ids)
    efree(tbl_ids);
#endif

  if (tpl_bc)
    compiler_free(tpl_bc);

  RETVAL_STRINGL(out.data, out.len);
  snobol_buf_free(&out);
}

/** @brief Pattern::setEvalCallbacks(array $callbacks): true
 *  Caches the fn_id => callable map on the pattern for OP_EVAL dispatch. */
PHP_METHOD(Snobol_Pattern, setEvalCallbacks) {
  zval *callbacks;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_ARRAY(callbacks)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));

  /* Free any previously cached callbacks */
  if (Z_TYPE(intern->eval_callbacks) != IS_UNDEF)
    zval_ptr_dtor(&intern->eval_callbacks);

  /* Store a copy — the array persists on the pattern struct and is
     * reused across match/search calls without re-allocation. */
  ZVAL_COPY(&intern->eval_callbacks, callbacks);

  SNOBOL_LOG("Snobol_Pattern::setEvalCallbacks: stored %d entries",
             (int)zend_hash_num_elements(Z_ARRVAL_P(callbacks)));
  RETURN_TRUE;
}

/** @brief Pattern::setJit(bool $enabled): true
 *  Accepted for API compatibility; JIT state is managed by the core. */
PHP_METHOD(Snobol_Pattern, setJit) {
  bool enabled;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_BOOL(enabled)
  ZEND_PARSE_PARAMETERS_END();

  RETURN_TRUE;
}

/* ---------------------------------------------------------------------------
 * Shared helper: try the batch-search API.  Returns true when the batch was
 * used and the PHP result has been fully built (return_value populated).
 * Returns false when the pattern is not batch-eligible; caller falls through
 * to the per-call loop.
 *
 * For flat mode (PHP_SNOBOL_RESULT_FLAT), builds parallel arrays directly
 * from the batch flat arrays.  For array mode, builds per-match zval arrays.
 * --------------------------------------------------------------------------- */
static bool php_snobol_try_batch(snobol_pattern_t *intern,
                                 const char *subject_val, size_t subject_len,
                                 zval *result,
                                 const php_snobol_match_options_t *opts) {
  const snobol_search_meta_t *meta = &intern->meta;

  /* Lazy-create persistent search state so caches (DFA, range_meta, trie)
     * are built once and reused across calls even for the batch fast path. */
  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      return false;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  /* Pre-build and cache the alt-literals trie so the batch path can use it. */
  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  snobol_batch_result_t batch;
  memset(&batch, 0, sizeof(batch));
  bool batch_ok =
      snobol_pattern_search_batch_ex(state, subject_val, subject_len, &batch);

  if (!batch_ok) {
    if (batch.eligible) {
      /* Zero matches, batch-eligible — DONE, no fallback.
             * Return the correct empty result shape for the requested mode. */
      snobol_batch_result_free(&batch);
      if (opts->result == PHP_SNOBOL_RESULT_FLAT) {
        zval ms, ml, ca, ob;
        array_init(&ms);
        array_init(&ml);
        array_init(&ca);
        array_init(&ob);
        array_init(result);
        snobol_assoc_zval(result, "match_start", 11, &ms);
        zval_ptr_dtor(&ms);
        snobol_assoc_zval(result, "match_len", 9, &ml);
        zval_ptr_dtor(&ml);
        snobol_assoc_zval(result, "captures", 8, &ca);
        zval_ptr_dtor(&ca);
        snobol_assoc_zval(result, "_output", 7, &ob);
        zval_ptr_dtor(&ob);
      } else {
        array_init(result);
      }
      return true;
    }
    snobol_batch_result_free(&batch);
    return false; /* ineligible — fall through */
  }

  size_t match_count = batch.match_count;

  if (opts->result == PHP_SNOBOL_RESULT_FLAT) {
    /* Flat result mode: parallel arrays.  match_count is known up front, so
     * pre-size every array (mirrors the searchSplit pre-sizing pattern). */
    zval match_starts, match_lengths, captures_arr, outputs_buf;
    array_init_size(&match_starts, match_count);
    array_init_size(&match_lengths, match_count);
    array_init_size(&captures_arr, match_count);
    array_init_size(&outputs_buf, match_count);

    /* out_pos: running cursor into the NUL-separated outputs blob, advanced
     * by the core-provided per-match lengths (O(1) per match). */
    size_t out_pos = 0;
    for (size_t mi = 0; mi < match_count; mi++) {
      size_t match_start = batch.positions[mi];
      size_t match_len = batch.lengths[mi];

      add_next_index_long(&match_starts, (zend_long)match_start);
      add_next_index_long(&match_lengths, (zend_long)match_len);
      /* Captures: flat per-register arrays */
      for (size_t ri = 0; ri < batch.var_count && ri < MAX_VARS; ri++) {
        char key[32];
        size_t key_len = snprintf(key, sizeof(key), "v%u", (unsigned)ri);

        size_t *cap_row = batch.captures ? batch.captures[ri] : NULL;
        if (!cap_row)
          continue;

        zval *reg_arr_zv =
            zend_hash_str_find(Z_ARRVAL_P(&captures_arr), key, key_len);
        if (!reg_arr_zv) {
          zval reg_arr;
          /* One entry per match in this register's flat array. */
          array_init_size(&reg_arr, match_count);
          zend_hash_str_add_new(Z_ARRVAL_P(&captures_arr), key, key_len,
                                &reg_arr);
          reg_arr_zv =
              zend_hash_str_find(Z_ARRVAL_P(&captures_arr), key, key_len);
        }

        php_snobol_emit_capture(reg_arr_zv, false, subject_val, subject_len,
                                cap_row[mi * 2], cap_row[mi * 2 + 1], key,
                                key_len, opts->captures);
      }

      /* Output: byte-exact via the core's per-match lengths (NUL-safe,
         O(1) per match — no strlen re-walk). */
      if (batch.outputs) {
        size_t olen = batch.output_lens[mi];
        add_next_index_stringl(&outputs_buf, batch.outputs + out_pos, olen);
        out_pos += olen + 1;
      } else {
        add_next_index_string(&outputs_buf, "");
      }
    }

    array_init(result);
    snobol_assoc_zval(result, "match_start", 11, &match_starts);
    zval_ptr_dtor(&match_starts);
    snobol_assoc_zval(result, "match_len", 9, &match_lengths);
    zval_ptr_dtor(&match_lengths);
    snobol_assoc_zval(result, "captures", 8, &captures_arr);
    zval_ptr_dtor(&captures_arr);
    snobol_assoc_zval(result, "_output", 7, &outputs_buf);
    zval_ptr_dtor(&outputs_buf);

    snobol_batch_result_free(&batch);
    return true;
  }

  /* Default array-of-arrays result mode: one sub-array per match. */
  array_init_size(result, match_count);

  /* out_pos: running cursor into the NUL-separated outputs blob. */
  size_t out_pos = 0;
  for (size_t mi = 0; mi < match_count; mi++) {
    size_t match_start = batch.positions[mi];
    size_t match_len = batch.lengths[mi];

    zval match_arr;
    array_init_size(&match_arr, batch.var_count + 3);

    for (size_t ri = 0; ri < batch.var_count && ri < MAX_VARS; ri++) {
      char key[32];
      size_t key_len = snprintf(key, sizeof(key), "v%u", (unsigned)ri);

      size_t *cap_row = batch.captures ? batch.captures[ri] : NULL;
      if (!cap_row)
        continue;

      php_snobol_emit_capture(&match_arr, true, subject_val, subject_len,
                              cap_row[mi * 2], cap_row[mi * 2 + 1], key,
                              key_len, opts->captures);
    }

    add_assoc_long(&match_arr, "_match_len", (zend_long)match_len);
    add_assoc_long(&match_arr, "_match_start", (zend_long)match_start);

    /* Output: byte-exact via the core's per-match lengths (NUL-safe,
       O(1) per match — no strlen re-walk). */
    if (batch.outputs) {
      size_t olen = batch.output_lens[mi];
      add_assoc_stringl(&match_arr, "_output", batch.outputs + out_pos, olen);
      out_pos += olen + 1;
    } else {
      add_assoc_string(&match_arr, "_output", "");
    }

    if (opts->metrics) {
      zval metrics;
      array_init(&metrics);
      add_assoc_long(&metrics, "choice_push_count", 0);
      add_assoc_long(&metrics, "choice_allocated", 0);
      add_assoc_long(&metrics, "choice_peak_depth", 0);
      add_assoc_long(&metrics, "choice_peak_memory", 0);
      snobol_assoc_zval(&match_arr, "_metrics", 8, &metrics);
      zval_ptr_dtor(&metrics);
    }

    add_next_index_zval(result, &match_arr);
  }

  snobol_batch_result_free(&batch);
  return true;
}

/* Core search loop shared by Pattern::searchAll and PatternHelper::matchAll.
 * Returns an array of match-result arrays (each with _match_len, _match_start, _output, _metrics).
 * The caller must pass a valid compiled pattern internals.
 *
 * P1 (search-perf-measured-wins): uses the stateful snobol_pattern_search_ex()
 * API so the VM, output buffer, and cached DFA are reused across iterations
 * instead of re-derived/re-allocated on every call. Captures are read lazily
 * via snobol_match_get_variable(); the per-call choice-stack metrics are no
 * longer available through the reuse path and are reported as 0.
 *
 * Batch-search fast path: for eligible patterns, snobol_pattern_search_batch()
 * collects all matches in a single pass, eliminating per-match API overhead. */
/** @brief Implementation of php_snobol_do_search_all() (see php_snobol.h). */
void php_snobol_do_search_all(snobol_pattern_t *intern, const char *subject_val,
                              size_t subject_len, zval *result,
                              const php_snobol_match_options_t *opts) {

  /* Batch-search fast path: single-pass for eligible patterns */
  if (php_snobol_try_batch(intern, subject_val, subject_len, result, opts))
    return;

  /* Lazy-create persistent search state (reused across calls on this pattern). */
  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      array_init(result);
      return;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  /* Pre-build and cache the alt-literals trie so it's reused across
     * search calls instead of being rebuilt every time by the tier
     * dispatch (which would build it on a stack-local buffer). */
  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  if (opts->result == PHP_SNOBOL_RESULT_FLAT) {
    /* Flat result mode: parallel arrays instead of array-of-arrays.
         * Pre-init flat arrays, append per-match, zero sub-array overhead. */
    zval match_starts, match_lengths, captures_arr, outputs_buf;
    array_init(&match_starts);
    array_init(&match_lengths);
    array_init(&captures_arr);
    array_init(&outputs_buf);

    size_t search_offset = 0;
    while (search_offset <= subject_len) {
      snobol_match_t *m = snobol_pattern_search_ex(state, subject_val,
                                                   subject_len, search_offset);
      if (!m || !snobol_match_success(m))
        break;

      size_t match_start = snobol_match_get_position(m);
      size_t match_end = match_start + snobol_match_get_length(m);

      add_next_index_long(&match_starts, (zend_long)match_start);
      add_next_index_long(&match_lengths, (zend_long)(match_end - match_start));

      /* Captures: flat per-register arrays.
             * var_off[i] is relative to the search window base
             * (subject + start_offset), NOT absolute.  The search API
             * (api.c:819-825) anchors var_subject at subject + start_offset
             * where start_offset is the offset passed to snobol_search_exec.
             * snobol_match_get_position(m) returns the absolute match
             * position, so use (match_start) as the window base for
             * capture offset correction: the capture starts at
             * window_base + var_off[i] = match_start + var_off[i]. */
      size_t cap_off_base = match_start;
      for (size_t i = 0; i < m->var_count; ++i) {
        char key[32];
        size_t key_len = snprintf(key, sizeof(key), "v%u", (unsigned)i);

        zval *reg_arr_zv =
            zend_hash_str_find(Z_ARRVAL_P(&captures_arr), key, key_len);
        if (!reg_arr_zv) {
          zval reg_arr;
          array_init(&reg_arr);
          zend_hash_str_add_new(Z_ARRVAL_P(&captures_arr), key, key_len,
                                &reg_arr);
          reg_arr_zv =
              zend_hash_str_find(Z_ARRVAL_P(&captures_arr), key, key_len);
        }

        php_snobol_emit_capture(reg_arr_zv, false, subject_val, subject_len,
                                cap_off_base + m->var_off[i], m->var_len[i],
                                key, key_len, opts->captures);
      }

      /* Output */
      if (m->output && m->output_len > 0) {
        add_next_index_stringl(&outputs_buf, m->output, m->output_len);
      } else {
        add_next_index_string(&outputs_buf, "");
      }

      size_t match_len = match_end - match_start;
      if (match_len == 0)
        match_len = 1;
      search_offset = match_start + match_len;
    }

    /* Assemble flat result */
    array_init(result);
    snobol_assoc_zval(result, "match_start", 11, &match_starts);
    zval_ptr_dtor(&match_starts);
    snobol_assoc_zval(result, "match_len", 9, &match_lengths);
    zval_ptr_dtor(&match_lengths);
    snobol_assoc_zval(result, "captures", 8, &captures_arr);
    zval_ptr_dtor(&captures_arr);
    snobol_assoc_zval(result, "_output", 7, &outputs_buf);
    zval_ptr_dtor(&outputs_buf);

    return;
  }

  /* Default array-of-arrays result mode */
  array_init(result);
  size_t search_offset = 0;

  while (search_offset <= subject_len) {
    snobol_match_t *m = snobol_pattern_search_ex(state, subject_val,
                                                 subject_len, search_offset);
    if (!m || !snobol_match_success(m)) {
      break;
    }

    size_t match_start = snobol_match_get_position(m);
    size_t match_end = match_start + snobol_match_get_length(m);

    zval match_arr;
    array_init(&match_arr);
    /* var_off[i] is relative to the match window (the position where the
     * match succeeded), not to the search offset: candidates before the
     * match may have failed.  Use match_start as the window base. */
    size_t cap_off_base = match_start;
    for (size_t i = 0; i < m->var_count; ++i) {
      char key[32];
      size_t key_len = snprintf(key, sizeof(key), "v%u", (unsigned)i);
      php_snobol_emit_capture(&match_arr, true, subject_val, subject_len,
                              cap_off_base + m->var_off[i], m->var_len[i], key,
                              key_len, opts->captures);
    }
    add_assoc_long(&match_arr, "_match_len",
                   (zend_long)(match_end - match_start));
    add_assoc_long(&match_arr, "_match_start", (zend_long)match_start);
    if (m->output && m->output_len > 0) {
      add_assoc_stringl(&match_arr, "_output", m->output, m->output_len);
    } else {
      add_assoc_string(&match_arr, "_output", "");
    }

    if (opts->metrics) {
      zval metrics;
      array_init(&metrics);
      add_assoc_long(&metrics, "choice_push_count", 0);
      add_assoc_long(&metrics, "choice_allocated", 0);
      add_assoc_long(&metrics, "choice_peak_depth", 0);
      add_assoc_long(&metrics, "choice_peak_memory", 0);
      snobol_assoc_zval(&match_arr, "_metrics", 8, &metrics);
      zval_ptr_dtor(&metrics);
    }

    add_next_index_zval(result, &match_arr);

    size_t match_len = match_end - match_start;
    if (match_len == 0)
      match_len = 1;
    search_offset = match_start + match_len;
  }
  /* Persistent state kept alive for the pattern's lifetime. */
}

/**
 * Pattern::searchAll(string $subject): array
 *
 * Find all non-overlapping matches using one native C search loop.
 * Returns an array of match-result arrays (same structure as match()).
 */
/** @brief Pattern::searchAll(string $subject, ?array $options = null): array
 *  All non-overlapping matches via the shared search loop. */
PHP_METHOD(Snobol_Pattern, searchAll) {
  zend_string *subject;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STR(subject)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  php_snobol_match_options_t opts;
  php_snobol_parse_match_options(options, &opts);

  php_snobol_do_search_all(intern, ZSTR_VAL(subject), ZSTR_LEN(subject),
                           return_value, &opts);
}

/**
 * Pattern::matchLiteral(string $input): array
 *
 * Lightweight anchored literal pattern match. Returns a simple associative
 * array with {success: bool, position: int, length: int} — no captures,
 * no output, no VM metrics. Only works for literal-only patterns; returns
 * {success: false, position: 0, length: 0} for non-literal patterns.
 */
PHP_METHOD(Snobol_Pattern, matchLiteral) {
  zend_string *input;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STR(input)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  const snobol_search_meta_t *meta = &intern->meta;

  array_init(return_value);
  if (!meta->is_literal_only) {
    add_assoc_bool(return_value, "success", 0);
    add_assoc_long(return_value, "position", 0);
    add_assoc_long(return_value, "length", 0);
    return;
  }

  /* Scan entire bytecode for any POS/RPOS op.
     * LIT bytecode layout: [0]:op [1..4]:lit_off(==9) [5..8]:lit_len [9..]:data */
  const uint8_t *bc = intern->bc;
  size_t bc_len = intern->bc_len;
  bool has_position_op = false;
  for (size_t i = 0; i < bc_len;) {
    uint8_t op = bc[i];
    if (op == OP_POS || op == OP_RPOS) {
      has_position_op = true;
      break;
    }
    if (op == OP_LIT && i + 9 <= bc_len) {
      uint32_t lit_len = ((uint32_t)bc[i + 5] << 24) |
                         ((uint32_t)bc[i + 6] << 16) |
                         ((uint32_t)bc[i + 7] << 8) | (uint32_t)bc[i + 8];
      i += 9 + lit_len;
      continue;
    }
    if (op == OP_NOP || op == OP_FENCE || op == OP_ANCHOR) {
      i++;
      continue;
    }
    if (op == OP_ACCEPT || op == OP_ABORT) {
      i++;
      continue;
    }
    break;
  }

  zend_long pos = 0, len = 0;
  bool matched = false;
  if (!has_position_op) {
    size_t ip = 0;
    while (ip < bc_len) {
      uint8_t op = bc[ip];
      if (op == OP_NOP || op == OP_FENCE || op == OP_ANCHOR ||
          op == OP_ACCEPT || op == OP_ABORT) {
        ip++;
        continue;
      }
      if ((op == OP_POS || op == OP_RPOS) && ip + 5 <= bc_len) {
        ip += 5;
        continue;
      }
      break;
    }
    if (ip + 9 <= bc_len && bc[ip] == OP_LIT) {
      uint32_t lit_off = ((uint32_t)bc[ip + 1] << 24) |
                         ((uint32_t)bc[ip + 2] << 16) |
                         ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
      uint32_t lit_len = ((uint32_t)bc[ip + 5] << 24) |
                         ((uint32_t)bc[ip + 6] << 16) |
                         ((uint32_t)bc[ip + 7] << 8) | (uint32_t)bc[ip + 8];
      /* Bounds-validate the literal operand before reading it; on a
         malformed layout fail cleanly (no out-of-bounds read). */
      if (lit_off + lit_len <= bc_len) {
        const char *lit = (const char *)(bc + lit_off);
        if (ZSTR_LEN(input) >= lit_len &&
            memcmp(ZSTR_VAL(input), lit, lit_len) == 0) {
          matched = true;
          len = (zend_long)lit_len;
        }
      }
    }
  }

  add_assoc_bool(return_value, "success", matched ? 1 : 0);
  add_assoc_long(return_value, "position", pos);
  add_assoc_long(return_value, "length", len);
}

/**
 * Pattern::searchSplit(string $subject): array
 *
 * Split a subject on non-overlapping pattern matches using one native C
 * search pass.  Records match offset pairs during the search, then
 * pre-allocates a packed zend_array of exact size and fills slots
 * directly (no zend_hash_next_index_insert).  Result contents are
 * byte-for-byte identical to the pre-change implementation.
 *
 * Pattern::searchSplitOffsets(string $subject): array
 *
 * Same search semantics but returns [[int $offset, int $length], ...]
 * pairs — zero zend_string allocation for token data.
 */

/** @brief One recorded match as a subject span [start, end). */
typedef struct {
  size_t start; /**< Absolute subject offset of the match start. */
  size_t end;   /**< Absolute subject offset of the match end. */
} snobol_match_record_t;

/** @brief SNOBOL zero-length-match advance: empty matches consume one byte. */
static inline size_t snobol_searchsplit_advance_len(size_t m) {
  return m == 0 ? 1 : m;
}

/* Shared offset-recording helper.  Runs a single search pass and records
 * {start, end} pairs for every match into a growable emalloc'd array.
 * Returns a heap-allocated array that the caller MUST efree;
 * sets *out_count to the number of matches (0 means no matches, returns
 * NULL). */
static snobol_match_record_t *php_snobol_searchsplit_record_offsets(
    snobol_pattern_search_state_t *state, const char *subject_val,
    size_t subject_len, size_t *out_count, const uint8_t *bc, size_t bc_len,
    const snobol_search_meta_t *meta) {
  /* Try batch API first: single-pass for eligible patterns.  Uses the
   * caller's persistent search state (batch_ex) so the cached
   * range_meta/DFA/trie are reused instead of rebuilt per call. */
  if (bc && meta && bc_len > 0) {
    snobol_batch_result_t batch;
    memset(&batch, 0, sizeof(batch));
    bool batch_ok = snobol_pattern_search_batch_ex(state, subject_val,
                                                    subject_len, &batch);

    if (batch_ok && batch.match_count > 0) {
      size_t n = batch.match_count;
      snobol_match_record_t *recs = emalloc(n * sizeof(snobol_match_record_t));
      for (size_t i = 0; i < n; i++) {
        recs[i].start = batch.positions[i];
        recs[i].end = batch.positions[i] + batch.lengths[i];
      }
      snobol_batch_result_free(&batch);
      *out_count = n;
      return recs;
    }
    snobol_batch_result_free(&batch);
    if (batch.eligible) {
      /* eligible — zero matches: DONE */
      *out_count = 0;
      return NULL;
    }
    /* not eligible — fall through to per-call loop */
  }

  size_t rec_cap = 16;
  snobol_match_record_t *recs =
      emalloc(rec_cap * sizeof(snobol_match_record_t));
  size_t rec_count = 0;
  size_t search_offset = 0;

  /* Literal-only fast path: use snobol_pattern_search_next() — no match
     * struct, no output buffer, no capture overhead. ~8 ns/call instead of
     * ~88 ns through snobol_pattern_search_ex. */
  if (meta && meta->is_literal_only) {
    size_t match_pos, match_len;
    while (snobol_pattern_search_next(state, subject_val, subject_len,
                                      search_offset, &match_pos, &match_len)) {
      if (rec_count == rec_cap) {
        rec_cap *= 2;
        recs = erealloc(recs, rec_cap * sizeof(snobol_match_record_t));
      }
      recs[rec_count].start = match_pos;
      recs[rec_count].end = match_pos + match_len;
      rec_count++;
      search_offset = match_pos + snobol_searchsplit_advance_len(match_len);
    }
  } else {
    while (search_offset <= subject_len) {
      snobol_match_t *m = snobol_pattern_search_ex(state, subject_val,
                                                   subject_len, search_offset);
      if (!m || !snobol_match_success(m)) {
        break;
      }

      size_t match_start = snobol_match_get_position(m);
      size_t match_len = snobol_match_get_length(m);

      if (rec_count == rec_cap) {
        rec_cap *= 2;
        recs = erealloc(recs, rec_cap * sizeof(snobol_match_record_t));
      }

      recs[rec_count].start = match_start;
      recs[rec_count].end = match_start + match_len;
      rec_count++;

      search_offset = match_start + snobol_searchsplit_advance_len(match_len);
    }
  }

  *out_count = rec_count;

  if (rec_count == 0) {
    efree(recs);
    return NULL;
  }
  return recs;
}

PHP_METHOD(Snobol_Pattern, searchSplit) {
  zend_string *subject;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STR(subject)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  php_snobol_match_options_t opts;
  php_snobol_parse_match_options(options, &opts);

  const char *subject_val = ZSTR_VAL(subject);
  size_t subject_len = ZSTR_LEN(subject);

  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      zend_throw_exception(zend_ce_exception, "Out of memory", 0);
      RETURN_FALSE;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  size_t match_count;
  snobol_match_record_t *recs = php_snobol_searchsplit_record_offsets(
      state, subject_val, subject_len, &match_count, intern->bc, intern->bc_len,
      &intern->meta);

  if (opts.result == PHP_SNOBOL_RESULT_FLAT) {
    /* Flat alternating [start, len, start, len, ...] result.
         * No per-segment zval overhead — single flat array of longs. */
    size_t seg_count = match_count + 1;
    array_init_size(return_value, seg_count * 2);

    size_t last_match_end = 0;
    for (size_t i = 0; i < match_count; i++) {
      size_t seg_len = recs[i].start - last_match_end;
      add_next_index_long(return_value, (zend_long)last_match_end);
      add_next_index_long(return_value, (zend_long)seg_len);
      last_match_end = recs[i].end;
    }
    /* Trailing segment */
    add_next_index_long(return_value, (zend_long)last_match_end);
    add_next_index_long(return_value,
                        (zend_long)(subject_len - last_match_end));

    if (recs)
      efree(recs);
    return;
  }

  /* Default: array of strings */
  array_init_size(return_value, match_count + 1);

  size_t last_match_end = 0;

  for (size_t i = 0; i < match_count; i++) {
    size_t seg_len = recs[i].start - last_match_end;
    add_next_index_stringl(return_value, subject_val + last_match_end, seg_len);
    last_match_end = recs[i].end;
  }

  if (recs)
    efree(recs);

  add_next_index_stringl(return_value, subject_val + last_match_end,
                         subject_len - last_match_end);
}

/** @brief Pattern::searchSplitOffsets(string $subject, ?array $options = null): array
 *  Split segments as [offset, length] pairs; no segment strings allocated. */
PHP_METHOD(Snobol_Pattern, searchSplitOffsets) {
  zend_string *subject;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(1, 2)
  Z_PARAM_STR(subject)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  php_snobol_match_options_t opts;
  php_snobol_parse_match_options(options, &opts);

  const char *subject_val = ZSTR_VAL(subject);
  size_t subject_len = ZSTR_LEN(subject);

  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      zend_throw_exception(zend_ce_exception, "Out of memory", 0);
      RETURN_FALSE;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  size_t match_count;
  snobol_match_record_t *recs = php_snobol_searchsplit_record_offsets(
      state, subject_val, subject_len, &match_count, intern->bc, intern->bc_len,
      &intern->meta);

  if (opts.result == PHP_SNOBOL_RESULT_FLAT) {
    /* Flat alternating [start, len, start, len, ...] — no sub-arrays */
    size_t seg_count = match_count + 1;
    array_init_size(return_value, seg_count * 2);

    size_t last_match_end = 0;
    for (size_t i = 0; i < match_count; i++) {
      size_t seg_len = recs[i].start - last_match_end;
      add_next_index_long(return_value, (zend_long)last_match_end);
      add_next_index_long(return_value, (zend_long)seg_len);
      last_match_end = recs[i].end;
    }
    add_next_index_long(return_value, (zend_long)last_match_end);
    add_next_index_long(return_value,
                        (zend_long)(subject_len - last_match_end));

    if (recs)
      efree(recs);
    return;
  }

  /* Default: array of [start, len] sub-arrays */
  array_init_size(return_value, match_count + 1);

  size_t last_match_end = 0;

  for (size_t i = 0; i < match_count; i++) {
    size_t seg_len = recs[i].start - last_match_end;

    zval pair;
    array_init(&pair);
    add_next_index_long(&pair, (zend_long)last_match_end);
    add_next_index_long(&pair, (zend_long)seg_len);
    add_next_index_zval(return_value, &pair);

    last_match_end = recs[i].end;
  }

  if (recs)
    efree(recs);

  /* Trailing segment */
  {
    size_t tail_len = subject_len - last_match_end;
    zval pair;
    array_init(&pair);
    add_next_index_long(&pair, (zend_long)last_match_end);
    add_next_index_long(&pair, (zend_long)tail_len);
    add_next_index_zval(return_value, &pair);
  }
}

/**
 * Pattern::searchSplitCuts(string $subject): array
 *
 * Returns a flat array of cut-point offsets delimiting segments.
 * Segment i spans subject[cuts[i-1]:cuts[i]], with the trailing
 * segment spanning subject[cuts[N-1]:].  The cheapest possible
 * split result: one flat array of longs, zero string allocation,
 * zero sub-array allocation.
 *
 * Example: subject "a b c", pattern "' '" (split on space)
 *   => [1, 3, 5]   (segments: "a", "b", "c")
 */
/** @brief Pattern::searchSplitCuts(string $subject): array
 *  Flat array of cut points: each match's end position. */
PHP_METHOD(Snobol_Pattern, searchSplitCuts) {
  zend_string *subject;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STR(subject)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  const char *subject_val = ZSTR_VAL(subject);
  size_t subject_len = ZSTR_LEN(subject);

  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      zend_throw_exception(zend_ce_exception, "Out of memory", 0);
      RETURN_FALSE;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  if (intern->meta.is_alt_literals && !intern->trie_cache) {
    intern->trie_cache = snobol_build_alt_trie(intern->bc, intern->bc_len);
  }
  if (intern->trie_cache) {
    snobol_pattern_search_state_set_trie_cache(state, intern->trie_cache);
  }

  size_t match_count;
  snobol_match_record_t *recs = php_snobol_searchsplit_record_offsets(
      state, subject_val, subject_len, &match_count, intern->bc, intern->bc_len,
      &intern->meta);

  /* Build flat cut-points array: each match's end position is a cut.
     * The trailing segment starts at the last cut (which is match N's end). */
  array_init_size(return_value, match_count);

  size_t last_match_end = 0;
  for (size_t i = 0; i < match_count; i++) {
    /* The cut is at the delimiter's end position */
    last_match_end = recs[i].end;
    add_next_index_long(return_value, (zend_long)last_match_end);
  }

  if (recs)
    efree(recs);
}

/**
 * Pattern::searchReplace(string $subject, string $replacement): string
 *
 * Replace non-overlapping pattern matches with a literal replacement using
 * one native C search loop.  For template-based substitution use subst().
 */
PHP_METHOD(Snobol_Pattern, searchReplace) {
  zend_string *subject, *replacement;
  zval *options = NULL;
  ZEND_PARSE_PARAMETERS_START(2, 3)
  Z_PARAM_STR(subject)
  Z_PARAM_STR(replacement)
  Z_PARAM_OPTIONAL
  Z_PARAM_ARRAY_OR_NULL(options)
  ZEND_PARSE_PARAMETERS_END();

  php_snobol_match_options_t opts;
  php_snobol_parse_match_options(options, &opts);
  (void)
      opts; /* metrics/captures/result are not used by replace (output is a string) */

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  const char *subject_val = ZSTR_VAL(subject);
  size_t subject_len = ZSTR_LEN(subject);
  const char *repl_val = ZSTR_VAL(replacement);
  size_t repl_len = ZSTR_LEN(replacement);

  snobol_buf out;
  snobol_buf_init(&out);

  /* Lazy-create the persistent search state; both the batch fast path and
     the per-call fallback reuse it (cached VM, DFA, range_meta). */
  if (!intern->search_state) {
    intern->search_state =
        snobol_pattern_search_state_create(intern->bc, intern->bc_len);
    if (!intern->search_state) {
      zend_throw_exception(zend_ce_exception, "Out of memory", 0);
      snobol_buf_free(&out);
      RETURN_FALSE;
    }
  }
  snobol_pattern_search_state_t *state = intern->search_state;

  /* Try batch API first via the persistent state: single-pass for eligible
     patterns.  Gets all match positions at once, then runs a single
     replacement pass. */
  {
    snobol_batch_result_t batch;
    memset(&batch, 0, sizeof(batch));
    bool batch_ok = snobol_pattern_search_batch_ex(state, subject_val,
                                                    subject_len, &batch);

    if (batch_ok) {
      /* Single replacement pass from batch positions */
      size_t last_match_end = 0;
      for (size_t mi = 0; mi < batch.match_count; mi++) {
        size_t match_start = batch.positions[mi];
        size_t match_end = match_start + batch.lengths[mi];

        snobol_buf_append(&out, subject_val + last_match_end,
                          match_start - last_match_end);
        snobol_buf_append(&out, repl_val, repl_len);

        size_t match_len = match_end - match_start;
        if (match_len == 0)
          match_len = 1;
        last_match_end = match_start + match_len;
      }
      /* A trailing zero-length match advances last_match_end past the
             * subject; clamp so the remainder append cannot underflow. */
      if (last_match_end > subject_len)
        last_match_end = subject_len;
      snobol_buf_append(&out, subject_val + last_match_end,
                        subject_len - last_match_end);

      snobol_batch_result_free(&batch);
      RETVAL_STRINGL(out.data, out.len);
      snobol_buf_free(&out);
      return;
    }
    snobol_batch_result_free(&batch);
    if (batch.eligible) {
      /* Zero matches — return subject unchanged */
      RETVAL_STRINGL(subject_val, subject_len);
      snobol_buf_free(&out);
      return;
    }
    /* Fall through to per-call loop (ineligible pattern) */
  }

  /* Literal-only heuristic pre-size: a bounded estimate of the output size
     that does NOT re-search the subject (no counting pass). */
  if (intern->meta.is_literal_only && intern->meta.required_lit_len > 0) {
    size_t lit_len = intern->meta.required_lit_len;
    size_t est_match_count = subject_len / lit_len + 1;
    size_t est =
        subject_len +
        est_match_count * (repl_len > lit_len ? repl_len - lit_len : 0);
    if (est > 0) {
      size_t chunk = est < 4096 ? est : 4096;
      char *zeros = calloc(1, chunk);
      if (zeros) {
        snobol_buf_append(&out, zeros, chunk);
        out.len = 0;
        free(zeros);
      }
    }
  }

  size_t search_offset = 0;
  size_t last_match_end = 0;

  while (search_offset <= subject_len) {
    snobol_match_t *m = snobol_pattern_search_ex(state, subject_val,
                                                 subject_len, search_offset);
    if (!m || !snobol_match_success(m)) {
      break;
    }

    size_t match_start = snobol_match_get_position(m);
    size_t match_end = match_start + snobol_match_get_length(m);

    snobol_buf_append(&out, subject_val + last_match_end,
                      match_start - last_match_end);
    snobol_buf_append(&out, repl_val, repl_len);

    size_t match_len = match_end - match_start;
    if (match_len == 0)
      match_len = 1;
    search_offset = match_start + match_len;
    last_match_end = search_offset;
  }

  /* A trailing zero-length match advances last_match_end past the subject;
     * clamp so the remainder append cannot underflow. */
  if (last_match_end > subject_len)
    last_match_end = subject_len;
  snobol_buf_append(&out, subject_val + last_match_end,
                    subject_len - last_match_end);

  RETVAL_STRINGL(out.data, out.len);
  snobol_buf_free(&out);
}

void php_snobol_create_search_iterator(zval *return_value, zval *pattern_zv,
                                       zend_string *subject);

/**
 * Pattern::searchSplitGenerator(string $subject): Iterator
 *
 * Returns a SplitIterator that lazily yields segment strings — the C
 * search loop advances only when the caller iterates.  Callers that
 * break after N segments pay zero cost for the remaining delimiters.
 * Uses snobol_pattern_search_next() internally for literal delimiters.
 */
PHP_METHOD(Snobol_Pattern, searchSplitGenerator) {
  zend_string *subject;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STR(subject)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  php_snobol_create_split_iterator(return_value, ZEND_THIS, subject);
}

/**
 * Pattern::searchAllGenerator(string $subject): Generator
 *
 * Returns a SearchIterator that lazily yields match results — the C
 * search loop advances only when the caller iterates.  Callers that
 * break after N matches pay zero cost for the remaining matches.
 */
PHP_METHOD(Snobol_Pattern, searchAllGenerator) {
  zend_string *subject;
  ZEND_PARSE_PARAMETERS_START(1, 1)
  Z_PARAM_STR(subject)
  ZEND_PARSE_PARAMETERS_END();

  snobol_pattern_t *intern = php_snobol_fetch(Z_OBJ_P(ZEND_THIS));
  if (!intern->bc || intern->bc_len == 0) {
    zend_throw_exception(zend_ce_exception, "Pattern not compiled", 0);
    RETURN_FALSE;
  }

  php_snobol_create_search_iterator(return_value, ZEND_THIS, subject);
}

ZEND_BEGIN_ARG_INFO_EX(ai_searchSplitCuts, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(ai_searchAllGenerator, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, subject, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry snobol_pattern_methods[] = {
    PHP_ME(Snobol_Pattern, compileFromAst, ai_compileFromAst,
           ZEND_ACC_PUBLIC | ZEND_ACC_STATIC) PHP_ME(Snobol_Pattern, fromString,
                                                     ai_fromString,
                                                     ZEND_ACC_PUBLIC |
                                                         ZEND_ACC_STATIC)
        PHP_ME(Snobol_Pattern, match, ai_match, ZEND_ACC_PUBLIC) PHP_ME(
            Snobol_Pattern, subst, ai_subst,
            ZEND_ACC_PUBLIC) PHP_ME(Snobol_Pattern, setEvalCallbacks,
                                    ai_setEval, ZEND_ACC_PUBLIC)
            PHP_ME(Snobol_Pattern, setJit, ai_setJit, ZEND_ACC_PUBLIC) PHP_ME(
                Snobol_Pattern, searchAll, ai_searchAll, ZEND_ACC_PUBLIC)
                PHP_ME(Snobol_Pattern, matchLiteral, ai_matchLiteral,
                       ZEND_ACC_PUBLIC) PHP_ME(Snobol_Pattern, searchSplit,
                                               ai_searchSplit, ZEND_ACC_PUBLIC)
                    PHP_ME(Snobol_Pattern, searchSplitOffsets,
                           ai_searchSplitOffsets, ZEND_ACC_PUBLIC)
                        PHP_ME(Snobol_Pattern, searchSplitCuts,
                               ai_searchSplitCuts, ZEND_ACC_PUBLIC)
                            PHP_ME(Snobol_Pattern, searchReplace,
                                   ai_searchReplace, ZEND_ACC_PUBLIC)
                                PHP_ME(Snobol_Pattern, searchAllGenerator,
                                       ai_searchAllGenerator, ZEND_ACC_PUBLIC)
                                    PHP_ME(Snobol_Pattern, searchSplitGenerator,
                                           ai_searchAllGenerator,
                                           ZEND_ACC_PUBLIC) PHP_FE_END};

zend_class_entry *snobol_pattern_ce;

/** @brief Register the Snobol\Pattern class (MINIT). */
void snobol_pattern_minit(void) {
  SNOBOL_LOG("snobol_pattern_minit: START");
  zend_class_entry ce;

  memcpy(&snobol_pattern_object_handlers, zend_get_std_object_handlers(),
         sizeof(zend_object_handlers));
  snobol_pattern_object_handlers.offset = XtOffsetOf(snobol_pattern_t, std);
  snobol_pattern_object_handlers.free_obj = php_snobol_pattern_dtor;

  INIT_CLASS_ENTRY(ce, "Snobol\\Pattern", snobol_pattern_methods);
  snobol_pattern_ce = zend_register_internal_class(&ce);
  snobol_pattern_ce->create_object = snobol_pattern_create;
  SNOBOL_LOG("snobol_pattern_minit: DONE");
}

/* Forward declarations for PHP AST conversion */
static ast_node_t *php_ast_to_c(zval *php_ast);

/* Convert PHP AST array to C AST */
/** @brief Convert a PHP Builder-format AST array into a core C AST node.
 *  @return Heap-allocated node (or NULL for unknown/malformed nodes);
 *          the caller owns the result. */
static ast_node_t *php_ast_to_c(zval *php_ast) {
  if (Z_TYPE_P(php_ast) != IS_ARRAY) {
    return NULL;
  }

  zval *type_zv =
      zend_hash_str_find(Z_ARRVAL_P(php_ast), "type", sizeof("type") - 1);
  if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING) {
    return NULL;
  }

  const char *type = Z_STRVAL_P(type_zv);

  /* Convert based on type */
  if (strcmp(type, "lit") == 0) {
    zval *text_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "text", sizeof("text") - 1);
    if (text_zv && Z_TYPE_P(text_zv) == IS_STRING) {
      return snobol_ast_create_literal(Z_STRVAL_P(text_zv),
                                       Z_STRLEN_P(text_zv));
    }
  } else if (strcmp(type, "concat") == 0) {
    zval *parts_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "parts", sizeof("parts") - 1);
    if (parts_zv && Z_TYPE_P(parts_zv) == IS_ARRAY) {
      zend_array *parts = Z_ARRVAL_P(parts_zv);
      size_t count = zend_hash_num_elements(parts);
      ast_node_t **children = malloc(count * sizeof(ast_node_t *));
      if (!children)
        return NULL;

      zval *part;
      size_t i = 0;
      ZEND_HASH_FOREACH_VAL(parts, part) {
        children[i++] = php_ast_to_c(part);
      }
      ZEND_HASH_FOREACH_END();

      ast_node_t *node = snobol_ast_create_concat(children, count);
      /* Don't free children - concat node takes ownership */
      return node;
    }
  } else if (strcmp(type, "alt") == 0) {
    zval *left_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "left", sizeof("left") - 1);
    zval *right_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "right", sizeof("right") - 1);
    if (left_zv && right_zv) {
      ast_node_t *left = php_ast_to_c(left_zv);
      ast_node_t *right = php_ast_to_c(right_zv);
      if (left && right) {
        return snobol_ast_create_alt(left, right);
      }
    }
  } else if (strcmp(type, "span") == 0) {
    zval *set_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "set", sizeof("set") - 1);
    if (set_zv && Z_TYPE_P(set_zv) == IS_STRING) {
      return snobol_ast_create_span(Z_STRVAL_P(set_zv), Z_STRLEN_P(set_zv));
    }
  } else if (strcmp(type, "break") == 0) {
    zval *set_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "set", sizeof("set") - 1);
    if (set_zv && Z_TYPE_P(set_zv) == IS_STRING) {
      return snobol_ast_create_break(Z_STRVAL_P(set_zv), Z_STRLEN_P(set_zv));
    }
  } else if (strcmp(type, "any") == 0) {
    zval *set_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "set", sizeof("set") - 1);
    if (set_zv && Z_TYPE_P(set_zv) == IS_STRING) {
      return snobol_ast_create_any(Z_STRVAL_P(set_zv), Z_STRLEN_P(set_zv));
    }
    return snobol_ast_create_any(NULL, 0);
  } else if (strcmp(type, "notany") == 0) {
    zval *set_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "set", sizeof("set") - 1);
    if (set_zv && Z_TYPE_P(set_zv) == IS_STRING) {
      return snobol_ast_create_notany(Z_STRVAL_P(set_zv), Z_STRLEN_P(set_zv));
    }
  } else if (strcmp(type, "arbno") == 0) {
    zval *sub_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "sub", sizeof("sub") - 1);
    if (sub_zv) {
      ast_node_t *sub = php_ast_to_c(sub_zv);
      if (sub) {
        return snobol_ast_create_arbno(sub);
      }
    }
  } else if (strcmp(type, "repeat") == 0) {
    zval *sub_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "sub", sizeof("sub") - 1);
    zval *min_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "min", sizeof("min") - 1);
    zval *max_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "max", sizeof("max") - 1);
    if (sub_zv && min_zv && max_zv && Z_TYPE_P(min_zv) == IS_LONG &&
        Z_TYPE_P(max_zv) == IS_LONG) {
      ast_node_t *sub = php_ast_to_c(sub_zv);
      if (sub) {
        return snobol_ast_create_repeat(sub, Z_LVAL_P(min_zv),
                                        Z_LVAL_P(max_zv));
      }
    }
  } else if (strcmp(type, "cap") == 0) {
    zval *reg_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "reg", sizeof("reg") - 1);
    zval *sub_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "sub", sizeof("sub") - 1);
    if (reg_zv && sub_zv && Z_TYPE_P(reg_zv) == IS_LONG) {
      ast_node_t *sub = php_ast_to_c(sub_zv);
      if (sub) {
        return snobol_ast_create_cap(Z_LVAL_P(reg_zv), sub);
      }
    }
  } else if (strcmp(type, "assign") == 0) {
    zval *var_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "var", sizeof("var") - 1);
    zval *reg_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "reg", sizeof("reg") - 1);
    if (var_zv && reg_zv && Z_TYPE_P(var_zv) == IS_LONG &&
        Z_TYPE_P(reg_zv) == IS_LONG) {
      return snobol_ast_create_assign(Z_LVAL_P(var_zv), Z_LVAL_P(reg_zv));
    }
  } else if (strcmp(type, "len") == 0) {
    zval *n_zv = zend_hash_str_find(Z_ARRVAL_P(php_ast), "n", sizeof("n") - 1);
    if (n_zv && Z_TYPE_P(n_zv) == IS_LONG) {
      return snobol_ast_create_len(Z_LVAL_P(n_zv));
    }
  } else if (strcmp(type, "anchor") == 0) {
    zval *atype_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "atype", sizeof("atype") - 1);
    if (atype_zv && Z_TYPE_P(atype_zv) == IS_STRING) {
      const char *atype = Z_STRVAL_P(atype_zv);
      if (strcmp(atype, "start") == 0) {
        return snobol_ast_create_anchor(ANCHOR_START);
      } else if (strcmp(atype, "end") == 0) {
        return snobol_ast_create_anchor(ANCHOR_END);
      }
    }
  } else if (strcmp(type, "emit") == 0) {
    zval *text_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "text", sizeof("text") - 1);
    zval *reg_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "reg", sizeof("reg") - 1);
    if (text_zv && Z_TYPE_P(text_zv) == IS_STRING) {
      int reg = reg_zv && Z_TYPE_P(reg_zv) == IS_LONG ? Z_LVAL_P(reg_zv) : -1;
      return snobol_ast_create_emit(Z_STRVAL_P(text_zv), Z_STRLEN_P(text_zv),
                                    reg);
    } else if (reg_zv && Z_TYPE_P(reg_zv) == IS_LONG) {
      /* emitRef - only has reg, no text */
      return snobol_ast_create_emit(NULL, 0, Z_LVAL_P(reg_zv));
    }
  } else if (strcmp(type, "dynamic_eval") == 0) {
    zval *expr_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "expr", sizeof("expr") - 1);
    if (expr_zv) {
      ast_node_t *expr = php_ast_to_c(expr_zv);
      if (expr) {
        return snobol_ast_create_dynamic_eval(expr);
      }
    }
  } else if (strcmp(type, "eval") == 0) {
    zval *fn_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "fn", sizeof("fn") - 1);
    zval *reg_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "reg", sizeof("reg") - 1);
    if (fn_zv && reg_zv && Z_TYPE_P(fn_zv) == IS_LONG &&
        Z_TYPE_P(reg_zv) == IS_LONG) {
      return snobol_ast_create_eval((int)Z_LVAL_P(fn_zv),
                                    (int)Z_LVAL_P(reg_zv));
    }
  } else if (strcmp(type, "table_access") == 0) {
    zval *table_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "table", sizeof("table") - 1);
    zval *key_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "key", sizeof("key") - 1);
    if (table_zv && key_zv && Z_TYPE_P(table_zv) == IS_STRING) {
      ast_node_t *key = php_ast_to_c(key_zv);
      if (key) {
        return snobol_ast_create_table_access(Z_STRVAL_P(table_zv), key);
      }
    }
  } else if (strcmp(type, "table_update") == 0) {
    zval *table_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "table", sizeof("table") - 1);
    zval *key_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "key", sizeof("key") - 1);
    zval *val_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "value", sizeof("value") - 1);
    if (table_zv && key_zv && val_zv && Z_TYPE_P(table_zv) == IS_STRING) {
      ast_node_t *key = php_ast_to_c(key_zv);
      ast_node_t *val = php_ast_to_c(val_zv);
      if (key && val) {
        return snobol_ast_create_table_update(Z_STRVAL_P(table_zv), key, val);
      }
    }
  }
  /* ---- Pattern primitives ---- */
  else if (strcmp(type, "breakx") == 0) {
    zval *set_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "set", sizeof("set") - 1);
    if (set_zv && Z_TYPE_P(set_zv) == IS_STRING) {
      return snobol_ast_create_breakx(Z_STRVAL_P(set_zv), Z_STRLEN_P(set_zv));
    }
  } else if (strcmp(type, "bal") == 0) {
    zval *open_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "open", sizeof("open") - 1);
    zval *close_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "close", sizeof("close") - 1);
    if (open_zv && close_zv && Z_TYPE_P(open_zv) == IS_STRING &&
        Z_TYPE_P(close_zv) == IS_STRING) {
      /* Decode first codepoint of each delimiter string */
      uint32_t open_cp = 0, close_cp = 0;
      int bytes = 0;
      if (!utf8_peek_next(Z_STRVAL_P(open_zv), Z_STRLEN_P(open_zv), 0, &open_cp,
                          &bytes))
        return NULL;
      if (!utf8_peek_next(Z_STRVAL_P(close_zv), Z_STRLEN_P(close_zv), 0,
                          &close_cp, &bytes))
        return NULL;
      return snobol_ast_create_bal(open_cp, close_cp);
    }
  } else if (strcmp(type, "fence") == 0) {
    return snobol_ast_create_fence();
  } else if (strcmp(type, "rem") == 0) {
    return snobol_ast_create_rem();
  } else if (strcmp(type, "rpos") == 0) {
    zval *n_zv = zend_hash_str_find(Z_ARRVAL_P(php_ast), "n", sizeof("n") - 1);
    if (n_zv && Z_TYPE_P(n_zv) == IS_LONG) {
      return snobol_ast_create_rpos((int32_t)Z_LVAL_P(n_zv));
    }
  } else if (strcmp(type, "rtab") == 0) {
    zval *n_zv = zend_hash_str_find(Z_ARRVAL_P(php_ast), "n", sizeof("n") - 1);
    if (n_zv && Z_TYPE_P(n_zv) == IS_LONG) {
      return snobol_ast_create_rtab((int32_t)Z_LVAL_P(n_zv));
    }
  } else if (strcmp(type, "pos") == 0) {
    zval *n_zv = zend_hash_str_find(Z_ARRVAL_P(php_ast), "n", sizeof("n") - 1);
    if (n_zv && Z_TYPE_P(n_zv) == IS_LONG) {
      return snobol_ast_create_pos((int32_t)Z_LVAL_P(n_zv));
    }
  } else if (strcmp(type, "tab") == 0) {
    zval *n_zv = zend_hash_str_find(Z_ARRVAL_P(php_ast), "n", sizeof("n") - 1);
    if (n_zv && Z_TYPE_P(n_zv) == IS_LONG) {
      return snobol_ast_create_tab((int32_t)Z_LVAL_P(n_zv));
    }
  } else if (strcmp(type, "abort") == 0) {
    return snobol_ast_create_abort();
  } else if (strcmp(type, "fail") == 0) {
    return snobol_ast_create_fail();
  } else if (strcmp(type, "succeed") == 0) {
    return snobol_ast_create_succeed();
  }
  /* ---- Control flow ---- */
  else if (strcmp(type, "label") == 0) {
    zval *name_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "name", sizeof("name") - 1);
    zval *target_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "target", sizeof("target") - 1);
    if (name_zv && target_zv && Z_TYPE_P(name_zv) == IS_STRING) {
      ast_node_t *target = php_ast_to_c(target_zv);
      if (target) {
        return snobol_ast_create_label(Z_STRVAL_P(name_zv), target);
      }
    }
  } else if (strcmp(type, "goto") == 0) {
    zval *label_zv =
        zend_hash_str_find(Z_ARRVAL_P(php_ast), "label", sizeof("label") - 1);
    if (label_zv && Z_TYPE_P(label_zv) == IS_STRING) {
      return snobol_ast_create_goto(Z_STRVAL_P(label_zv));
    }
  }

  return NULL;
}

/* PHP AST compilation - converts PHP Builder AST to C AST then compiles */
/** @brief Implementation of compile_ast_to_bytecode() (see php_snobol.h). */
int compile_ast_to_bytecode(zval *ast, zval *options, uint8_t **out_bc,
                            size_t *out_len) {
  /* Convert PHP AST to C AST */
  ast_node_t *c_ast = php_ast_to_c(ast);
  if (!c_ast) {
    zend_throw_exception(zend_ce_exception,
                         "Failed to convert PHP AST to C AST", 0);
    return -1;
  }

  /* Extract case_insensitive option if present */
  bool case_insensitive = false;
  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    zval *ci = zend_hash_str_find(Z_ARRVAL_P(options), "caseInsensitive",
                                  sizeof("caseInsensitive") - 1);
    if (ci && (Z_TYPE_P(ci) == IS_TRUE ||
               (Z_TYPE_P(ci) == IS_LONG && Z_LVAL_P(ci)))) {
      case_insensitive = true;
    }
  }

  /* Compile C AST to bytecode */
  int result =
      compile_ast_to_bytecode_c(c_ast, case_insensitive, out_bc, out_len);

  /* Free C AST */
  snobol_ast_free(c_ast);

  return result;
}

/* compile_template_to_bytecode is provided by the core (compiler.c / core_amalgam.c).
 * The PHP-side duplicate has been removed; template compilation is
 * handled entirely by the core. */
#if 0  /* REMOVED: duplicate template compiler – delegate to core */
/** @brief Placeholder for a removed template compiler (see core/compiler.c). */
int compile_template_to_bytecode_REMOVED(const char *tpl, size_t len, uint8_t **out_bc, size_t *out_len) {
    SNOBOL_LOG("compile_template_to_bytecode START: tpl='%.*s'", (int)len, tpl);
    CodeBuf cb;
    cb_init(&cb);

    size_t i = 0;
    while (i < len) {
        if (tpl[i] == '$') {
            size_t start_of_dollar = i;
            i++;
            if (i >= len) {
                cb_emit_u8(&cb, OP_EMIT_LITERAL);
                size_t off = cb_pos(&cb) + 4 + 4;
                cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                break;
            }
            
            bool braced = (tpl[i] == '{');
            if (braced) i++;

            if (i < len && tpl[i] == 'v') {
                i++;
                uint8_t reg = 0;
                bool has_digits = false;
                while (i < len && tpl[i] >= '0' && tpl[i] <= '9') {
                    reg = reg * 10 + (tpl[i] - '0');
                    i++;
                    has_digits = true;
                }
                
                if (!has_digits) {
                    i = start_of_dollar + 1;
                    cb_emit_u8(&cb, OP_EMIT_LITERAL);
                    size_t off = cb_pos(&cb) + 4 + 4;
                    cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                    continue;
                }

                uint8_t expr_type = 0;
                if (braced) {
                    if (i < len && tpl[i] == '.') {
                        i++;
                        if (len - i >= 7 && memcmp(tpl + i, "upper()", 7) == 0) {
                            expr_type = 1; i += 7;
                        } else if (len - i >= 8 && memcmp(tpl + i, "length()", 8) == 0) {
                            expr_type = 2; i += 8;
                        }
                    }
                    if (i < len && tpl[i] == '}') {
                        i++;
                        if (expr_type == 0) {
                            cb_emit_u8(&cb, OP_EMIT_CAPTURE); cb_emit_u8(&cb, reg);
                        } else {
                            cb_emit_u8(&cb, OP_EMIT_EXPR); cb_emit_u8(&cb, reg); cb_emit_u8(&cb, expr_type);
                        }
                    } else {
                        i = start_of_dollar + 1;
                        cb_emit_u8(&cb, OP_EMIT_LITERAL);
                        size_t off = cb_pos(&cb) + 4 + 4;
                        cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                    }
                } else if (i < len && tpl[i] == '[') {
                    /* Table-backed replacement: $TABLE[key]
                     * Parse TABLE name and key, emit OP_EMIT_TABLE */
                    i++; /* skip '[' */
                    
                    /* Parse table name (identifier until '.' or '[') */
                    size_t table_name_start = i;
                    while (i < len && tpl[i] != '.' && tpl[i] != '[' && tpl[i] != ']') {
                        i++;
                    }
                    size_t table_name_len = i - table_name_start;
                    
                    if (table_name_len == 0 || i >= len || tpl[i] != '[') {
                        /* Invalid syntax, emit as literal '$' */
                        i = start_of_dollar + 1;
                        cb_emit_u8(&cb, OP_EMIT_LITERAL);
                        size_t off = cb_pos(&cb) + 4 + 4;
                        cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                        continue;
                    }
                    
                    /* For now, table name must be a literal identifier */
                    /* Extract table name */
                    const char *table_name = tpl + table_name_start;
                    
                    /* Skip '[' and parse key */
                    i++; /* skip '[' */
                    size_t key_start = i;
                    
                    /* Key can be: quoted literal or identifier */
                    bool quoted = (i < len && tpl[i] == '\'');
                    if (quoted) {
                        i++; /* skip opening quote */
                        key_start = i;
                        while (i < len && tpl[i] != '\'') {
                            i++;
                        }
                        if (i >= len) {
                            /* Unclosed quote, emit as literal '$' */
                            i = start_of_dollar + 1;
                            cb_emit_u8(&cb, OP_EMIT_LITERAL);
                            size_t off = cb_pos(&cb) + 4 + 4;
                            cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                            continue;
                        }
                        /* Key is from key_start to i (exclusive of closing quote) */
                        size_t key_len = i - key_start;
                        i++; /* skip closing quote */

                        /* Check for closing ']' */
                        if (i >= len || tpl[i] != ']') {
                            i = start_of_dollar + 1;
                            cb_emit_u8(&cb, OP_EMIT_LITERAL);
                            size_t off = cb_pos(&cb) + 4 + 4;
                            cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                            continue;
                        }
                        i++; /* skip ']' */

                        /* Emit OP_EMIT_TABLE with literal key stored in bytecode
                         * Format: opcode u8, table_id u16, key_type u8 (0=literal), key_len u16, key_bytes...
                         * Table ID 0 means "resolve by name at runtime" - for now use placeholder */
                        cb_emit_u8(&cb, OP_EMIT_TABLE);
                        cb_emit_u16(&cb, 0); /* table_id placeholder - resolved at runtime */
                        cb_emit_u8(&cb, 0);  /* key_type: 0 = literal key */
                        cb_emit_u16(&cb, (uint16_t)key_len); /* literal key length */
                        cb_emit_bytes(&cb, (const uint8_t*)(tpl + key_start), key_len);
                    } else {
                        /* Identifier key (capture-derived) */
                        while (i < len && tpl[i] >= '0' && tpl[i] <= '9') {
                            i++;
                        }
                        if (i >= len || tpl[i] != ']') {
                            i = start_of_dollar + 1;
                            cb_emit_u8(&cb, OP_EMIT_LITERAL);
                            size_t off = cb_pos(&cb) + 4 + 4;
                            cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
                            continue;
                        }
                        size_t key_reg = 0;
                        /* Parse register number from identifier like v0, v1, etc. */
                        if (key_start > 0 && tpl[key_start - 1] == 'v') {
                            const char *reg_start = tpl + key_start;
                            key_reg = (uint8_t)(reg_start[0] - '0');
                        }
                        i++; /* skip ']' */

                        /* Emit OP_EMIT_TABLE with capture-derived key
                         * Format: opcode u8, table_id u16, key_type u8 (1=capture), key_reg u8 */
                        cb_emit_u8(&cb, OP_EMIT_TABLE);
                        cb_emit_u16(&cb, 0); /* table_id placeholder - needs resolution */
                        cb_emit_u8(&cb, 1);  /* key_type: 1 = capture-derived key */
                        cb_emit_u8(&cb, (uint8_t)key_reg);
                    }
                } else {
                    cb_emit_u8(&cb, OP_EMIT_CAPTURE); cb_emit_u8(&cb, reg);
                }
            } else {
                i = start_of_dollar + 1;
                cb_emit_u8(&cb, OP_EMIT_LITERAL);
                size_t off = cb_pos(&cb) + 4 + 4;
                cb_emit_u32(&cb, (uint32_t)off); cb_emit_u32(&cb, 1); cb_emit_u8(&cb, '$');
            }
        } else {
            // scan literal segment
            size_t start = i;
            while (i < len && tpl[i] != '$') i++;
            size_t seglen = i - start;
            cb_emit_u8(&cb, OP_EMIT_LITERAL);
            size_t off = cb_pos(&cb) + 4 + 4;
            cb_emit_u32(&cb, (uint32_t)off);
            cb_emit_u32(&cb, (uint32_t)seglen);
            cb_emit_bytes(&cb, (const uint8_t*)tpl + start, seglen);
        }
    }

    cb_emit_u8(&cb, OP_ACCEPT);

    uint8_t *out = snobol_malloc(cb.len);
    if (!out) { cb_free(&cb); return -1; }
    memcpy(out, cb.buf, cb.len);
    *out_bc = out;
    *out_len = cb.len;

    cb_free(&cb);
    SNOBOL_LOG("compile_template_to_bytecode SUCCESS, len=%zu", *out_len);
    return 0;
}
#endif /* REMOVED */
