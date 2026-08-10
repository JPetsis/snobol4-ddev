#ifndef PHP_SNOBOL_H
#define PHP_SNOBOL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "snobol/search.h"

/**
 * @brief Opaque forward declaration of the pattern AST node.
 *
 * The full definition lives in the core (ast.h); the binding only passes
 * pointers between the PHP-array compiler and the C compiler.
 */
typedef struct ast_node ast_node_t;

/**
 * @brief Opaque forward declaration of the persistent search state.
 *
 * The full definition lives in the core (snobol.h); the binding stores a
 * pointer on the PHP pattern struct and passes it to the search APIs.
 */
typedef struct snobol_pattern_search_state snobol_pattern_search_state_t;

/** @brief Module entry for the snobol extension (see php_snobol.c). */
extern zend_module_entry snobol_module_entry;
/** @brief Module-entry pointer used by the ZEND_GET_MODULE macro. */
#define phpext_snobol_ptr &snobol_module_entry

/** @brief Extension version string reported to phpinfo(). */
#define PHP_SNOBOL_VERSION "1.0.3"

/** @brief Module initialization entry point (php_snobol.c). */
PHP_MINIT_FUNCTION(snobol);

/**
 * @brief Compile a PHP-array AST (Builder output) to core bytecode.
 *
 * @param[in]  ast      PHP array in the Builder node format.
 * @param[in]  options  Optional options array (e.g. caseInsensitive);
 *                      may be NULL.
 * @param[out] out_bc   Receives a heap-allocated bytecode buffer on success
 *                      (caller frees with compiler_free()).
 * @param[out] out_len  Receives the bytecode length on success.
 * @return 0 on success, -1 on compilation failure (exception thrown).
 */
int compile_ast_to_bytecode(zval *ast, zval *options, uint8_t **out_bc,
                            size_t *out_len);

/**
 * @brief Compile a C AST node to core bytecode.
 *
 * Variant of compile_ast_to_bytecode() that consumes a C ast_node_t
 * instead of a PHP array.
 *
 * @param[in]  ast      C AST node to compile.
 * @param[in]  options  Optional options array; may be NULL.
 * @param[out] out_bc   Receives a heap-allocated bytecode buffer on success.
 * @param[out] out_len  Receives the bytecode length on success.
 * @return 0 on success, -1 on compilation failure (exception thrown).
 */
int compile_ast_to_bytecode_wrapper(ast_node_t *ast, zval *options,
                                    uint8_t **out_bc, size_t *out_len);

/** @brief Opaque core table type (snobol/table.h). */
typedef struct snobol_table snobol_table_t;

/**
 * @brief Store a value in an array with a refcount bump.
 *
 * PHP 8.5 removed the refcount increment from add_assoc_zval; this helper
 * restores the copy semantics for reference-counted zvals (arrays, objects).
 *
 * @param arr     Target array.
 * @param key     NUL-terminated key.
 * @param key_len Key length in bytes.
 * @param value   zval to store (copied, refcount bumped).
 */
static zend_always_inline void snobol_assoc_zval(zval *arr, const char *key,
                                                 size_t key_len, zval *value) {
  zval copy;
  ZVAL_COPY(&copy, value);
  zend_hash_str_update(Z_ARRVAL_P(arr), key, key_len, &copy);
}

/**
 * @brief Core object struct for the Snobol\Pattern class.
 *
 * Holds the compiled bytecode and all per-pattern caches (search metadata,
 * range metadata, DFA, alt-literals trie, persistent search state, eval
 * callbacks). The zend_object is stored LAST so the struct can be fetched
 * from a zend_object pointer with php_snobol_fetch().
 *
 * @note The layout deliberately differs from the core struct snobol_pattern:
 *       core fields that live on the search state (DFA, trie) must never be
 *       read from this struct at core offsets.
 */
typedef struct snobol_pattern {
  uint8_t *bc;
  size_t bc_len;
  /* Cached search metadata (derived once at compile time) */
  snobol_search_meta_t meta;
  bool meta_initialized;
  /* Cached charclass range metadata */
  snobol_range_meta_t *range_meta;
  size_t range_meta_count;
  /* Cached alt-literals trie for 'cat'|'dog'|'fox' patterns (Tier 5).
     * Built lazily on first searchAll/searchSplit call; freed in dtor. */
  snobol_auto_trie_t *trie_cache;
  /* Persistent search state reused across calls, avoiding per-call
     * state create/destroy and DFA rebuild.  Lazily created. */
  snobol_pattern_search_state_t *search_state;
  /* Cached eval callbacks registered via setEvalCallbacks().
     * Stored as a PHP array mapping fn_id => callable.
     * Initialized to ZVAL_UNDEF; freed in dtor. */
  zval eval_callbacks;
  zend_object std;
} snobol_pattern_t;

/**
 * @brief Recover the snobol_pattern_t from a zend_object pointer.
 *
 * @param obj zend_object embedded in the pattern struct.
 * @return Pointer to the containing snobol_pattern_t.
 */
static inline snobol_pattern_t *php_snobol_fetch(zend_object *obj) {
  return (snobol_pattern_t *)((char *)(obj)-XtOffsetOf(snobol_pattern_t, std));
}

/**
 * @brief Parsed match/search options (from the PHP $options array).
 *
 * Fields not present in the options array keep their legacy defaults
 * (no metrics, string captures, array-of-arrays result).
 */
typedef struct php_snobol_match_options {
  bool metrics; /* true to include _metrics hash */
  int captures; /* 0='strings', 1='offsets'      */
  int result;   /* 0='arrays', 1='flat'          */
} php_snobol_match_options_t;

/** @brief Capture materialization modes (the $options['captures'] value). */
enum {
  PHP_SNOBOL_CAPTURES_STRINGS = 0, /**< Captures as plain strings. */
  PHP_SNOBOL_CAPTURES_OFFSETS = 1, /**< Captures as [offset, length] pairs. */
  PHP_SNOBOL_RESULT_ARRAYS = 0,    /**< Result as array of match arrays. */
  PHP_SNOBOL_RESULT_FLAT = 1,      /**< Result as parallel flat arrays. */
};

/**
 * @brief Parse the $options array into a php_snobol_match_options_t.
 *
 * Recognized keys: "metrics" (bool), "captures" ("strings"/"offsets"),
 * "result" ("arrays"/"flat"). Unknown values fall back to the defaults.
 *
 * @param options_zv The options zval (may be NULL or non-array).
 * @param opts       Destination struct, always fully populated.
 */
void php_snobol_parse_match_options(zval *options_zv,
                                    php_snobol_match_options_t *opts);

/**
 * @brief Create a Snobol\SearchIterator for lazy match iteration.
 *
 * Used by Pattern::searchAllGenerator().
 *
 * @param return_value zval to populate with the iterator object.
 * @param pattern      Compiled pattern to search with.
 * @param subject      Subject string (borrowed; must outlive the iterator).
 * @param subject_len  Subject length in bytes.
 */
void php_snobol_create_search_iterator(zval *return_value,
                                       snobol_pattern_t *pattern,
                                       const char *subject, size_t subject_len);

/**
 * @brief Create a Snobol\SplitIterator for lazy split iteration.
 *
 * Used by Pattern::searchSplitGenerator().
 *
 * @param return_value zval to populate with the iterator object.
 * @param pattern      Compiled pattern to split with.
 * @param subject      Subject string (borrowed; must outlive the iterator).
 * @param subject_len  Subject length in bytes.
 */
void php_snobol_create_split_iterator(zval *return_value,
                                      snobol_pattern_t *pattern,
                                      const char *subject, size_t subject_len);

/** @brief Register the Snobol\SplitIterator class (MINIT). */
void snobol_split_iterator_minit(void);
/** @brief Class entry for Snobol\SplitIterator. */
extern zend_class_entry *snobol_split_iterator_ce;

/**
 * @brief Core search loop for Pattern::searchAll and PatternHelper::matchAll.
 *
 * Runs the batch fast path when the pattern is batch-eligible, otherwise a
 * per-call search loop, and fills @p result according to @p opts
 * (array-of-arrays or flat parallel arrays).
 *
 * @param intern      Pattern internals (bytecode, meta, caches).
 * @param subject_val Subject string.
 * @param subject_len Subject length in bytes.
 * @param result      zval to populate with the result array.
 * @param opts        Parsed match options.
 */
void php_snobol_do_search_all(snobol_pattern_t *intern, const char *subject_val,
                              size_t subject_len, zval *result,
                              const php_snobol_match_options_t *opts);

/**
 * @brief Anchored first-match via the persistent search state.
 *
 * Routes Pattern::match() through the tier dispatch + prefilter path,
 * reusing the cached VM, DFA, and range_meta on intern->search_state.
 * The literal fast path is handled separately by the caller.
 *
 * @param intern      Pattern internals.
 * @param subject_val Subject string.
 * @param subject_len Subject length in bytes.
 * @param result      zval to populate with the match array on success.
 * @param opts        Parsed match options (metrics, capture mode).
 * @return true and populates @p result on success; false on no match
 *         (@p result is uninitialised in that case).
 */
bool php_snobol_do_match(snobol_pattern_t *intern, const char *subject_val,
                         size_t subject_len, zval *result,
                         const php_snobol_match_options_t *opts);

#endif /* PHP_SNOBOL_H */
