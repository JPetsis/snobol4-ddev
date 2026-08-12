#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* phpize always builds shared extensions; ensure get_module() is compiled in.
   On macOS the autoconf-generated config.h sometimes omits COMPILE_DL_SNOBOL
   when --enable-snobol is passed without =shared.  This fallback is harmless
   for cmake builds because cmake defines it explicitly. */
#ifndef COMPILE_DL_SNOBOL
#define COMPILE_DL_SNOBOL 1
#endif

#include "php.h"
#include "php_snobol.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"

/* libsnobol4 core built-in function headers */
#include "snobol/string_fn.h"
#include "snobol/type_fn.h"
#include "snobol/vm.h"
#include "snobol/snobol.h"

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

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

/**
 * @file php_snobol.c
 * @brief Module entry point and global function exports for the snobol
 *        PHP extension.
 *
 * Registers the extension (MINIT: classes + version check) and the global
 * snobol_text_* functions that wrap the core string/type built-ins for the
 * Snobol\Text PHP helper class. Also exports the version and choice
 * statistics functions.
 */

/* Extern declaration from snobol_pattern.c */
void snobol_pattern_minit(void);

/* Extern declaration from snobol_table_php.c */
void snobol_table_php_minit(void);

/* Extern declaration from snobol_array_php.c */
void snobol_array_php_minit(void);

/* Extern declaration from snobol_dynamic_pattern_php.c */
void snobol_dynamic_pattern_cache_php_minit(void);

/* Extern declaration from snobol_builder_php.c */
void snobol_builder_php_minit(void);

/* Extern declaration from snobol_pattern_cache_php.c */
void snobol_pattern_cache_php_minit(void);

/* Extern declaration from snobol_pattern_helper_php.c */
void snobol_pattern_helper_php_minit(void);

/* Extern declaration from snobol_search_iterator_php.c */
void snobol_search_iterator_minit(void);
void snobol_split_iterator_minit(void);

PHP_MINFO_FUNCTION(snobol);

/**
 * @brief phpinfo() handler: reports the extension version and profiling
 *        status in a module table.
 */
PHP_MINFO_FUNCTION(snobol) {
  php_info_print_table_start();
  php_info_print_table_header(2, "snobol support", "enabled");
  php_info_print_table_row(2, "version", PHP_SNOBOL_VERSION);
#ifdef SNOBOL_PROFILE
  php_info_print_table_row(2, "profiling", "enabled");
#else
  php_info_print_table_row(2, "profiling", "disabled");
#endif
  php_info_print_table_end();
}

/**
 * @brief Module initialization: registers all internal classes.
 *
 * Verifies the linked core's major API version matches the compiled-in
 * SNOBOL_VERSION_MAJOR (a mismatch aborts module load), then registers
 * every class the extension exposes: Pattern, Table, Array_, Builder,
 * DynamicPatternCache, PatternCache, PatternHelper, SearchIterator and
 * SplitIterator.
 */
PHP_MINIT_FUNCTION(snobol) {
  SNOBOL_LOG("PHP_MINIT_FUNCTION(snobol): START");

  /* Verify that the linked libsnobol4 major version matches what this binding
     * was compiled against.  A major version mismatch means incompatible ABI. */
  uint32_t api_ver = snobol_get_api_version();
  uint32_t got_major = api_ver >> 16;
  if (got_major != (uint32_t)SNOBOL_VERSION_MAJOR) {
    zend_throw_exception_ex(
        zend_ce_exception, 0,
        "libsnobol4 API version mismatch: expected major %d, got %d",
        (int)SNOBOL_VERSION_MAJOR, (int)got_major);
    return FAILURE;
  }

  snobol_pattern_minit();
  snobol_table_php_minit();
  snobol_array_php_minit();
  snobol_dynamic_pattern_cache_php_minit();
  snobol_builder_php_minit();
  snobol_pattern_cache_php_minit();
  snobol_pattern_helper_php_minit();
  snobol_search_iterator_minit();
  snobol_split_iterator_minit();
  SNOBOL_LOG("PHP_MINIT_FUNCTION(snobol): DONE");
  return SUCCESS;
}

/* ============================================================
 * C function exports for libsnobol4 built-ins
 *
 * These PHP_FUNCTION implementations wrap the C built-in functions
 * from string_fn.h and type_fn.h, exposing them as global PHP
 * functions (snobol_text_*).  The Snobol\Text PHP class (Text.php)
 * uses these internally when the C extension is loaded.
 * ============================================================ */

/* --- ARGINFO for snobol_text_* functions ---
 * PHP 8.5 warns for any PHP_FE entry registered with NULL arginfo.
 * Each entry below declares the exact parameter count and types so
 * the engine can validate calls and suppress the warning.         */

/* string → int */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_size, 0, 1, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* string → string */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_str1, 0, 1, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* string, int → string */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_dupl, 0, 2, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, n, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* string, int, int → string|false */
ZEND_BEGIN_ARG_INFO_EX(ai_text_substr, 0, 0, 3)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, pos, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, len, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* string, string, string → string */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_str3, 0, 3, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, from, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, to, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* string, int [, string] → string  (lpad / rpad) */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_pad, 0, 2, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, width, IS_LONG, 0)
ZEND_ARG_TYPE_INFO(0, pad, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* int → string|false */
ZEND_BEGIN_ARG_INFO_EX(ai_text_char, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, cp, IS_LONG, 0)
ZEND_END_ARG_INFO()

/* string → int|false */
ZEND_BEGIN_ARG_INFO_EX(ai_text_ord, 0, 0, 1)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* string, string → bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_str2_bool, 0, 2, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, a, IS_STRING, 0)
ZEND_ARG_TYPE_INFO(0, b, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* string → bool */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_text_str1_bool, 0, 1, _IS_BOOL, 0)
ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* --- STRING FUNCTIONS --- */

/**
 * @brief Size of a string in characters.
 * @param s
 * @return Character count (zend_long).
 */
PHP_FUNCTION(snobol_text_size) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  RETURN_LONG((zend_long)snobol_size(s, slen));
}

/**
 * @brief Trim trailing whitespace from a string.
 * @param s
 * @return Trimmed string; the input when nothing to trim.
 */
PHP_FUNCTION(snobol_text_trim) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_trim(s, slen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Duplicate a string n times.
 * @param s
 * @param n
 * @return Duplicated string (empty when n <= 0).
 */
PHP_FUNCTION(snobol_text_dupl) {
  char *s;
  size_t slen;
  zend_long n;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sl", &s, &slen, &n) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (n > 0 && snobol_dupl(s, slen, (size_t)n, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL("", 0);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Reverse a string.
 * @param s
 * @return Reversed string.
 */
PHP_FUNCTION(snobol_text_reverse) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_reverse(s, slen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Extract a substring starting at 1-based position.
 * @param s
 * @param pos
 * @param len
 * @return Substring; false when pos < 1 or the request fails.
 */
PHP_FUNCTION(snobol_text_substr) {
  char *s;
  size_t slen;
  zend_long pos, len;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sll", &s, &slen, &pos, &len) ==
      FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (pos >= 1 && snobol_substr(s, slen, (size_t)pos, (size_t)len, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_FALSE;
  }
  snobol_buf_free(&b);
}

/**
 * @brief Replace all occurrences of one substring with another.
 * @param s
 * @param from
 * @param to
 * @return Replaced string.
 */
PHP_FUNCTION(snobol_text_replace) {
  char *s, *f, *t;
  size_t slen, flen, tlen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss", &s, &slen, &f, &flen, &t,
                            &tlen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_replace(s, slen, f, flen, t, tlen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Replace all occurrences of one character with another.
 * @param s
 * @param from
 * @param to
 * @return Replaced string.
 */
PHP_FUNCTION(snobol_text_replace_char) {
  char *s, *f, *t;
  size_t slen, flen, tlen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sss", &s, &slen, &f, &flen, &t,
                            &tlen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_replace_char(s, slen, f, flen, t, tlen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Left-pad a string to a width with a fill character.
 * @param s
 * @param width
 * @param pad
 * @return Padded string.
 */
PHP_FUNCTION(snobol_text_lpad) {
  char *s, *pad = " ";
  size_t slen, padlen = 1;
  zend_long width;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sl|s", &s, &slen, &width, &pad,
                            &padlen) == FAILURE)
    return;
  /* Decode the pad string as a full UTF-8 codepoint (a multi-byte pad such
     as '€' must not be truncated to its first byte). */
  uint32_t pad_cp = (uint32_t)' ';
  if (padlen > 0 && !snobol_ord(pad, padlen, &pad_cp))
    pad_cp = (uint32_t)' ';
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_lpad(s, slen, (size_t)width, pad_cp, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Right-pad a string to a width with a fill character.
 * @param s
 * @param width
 * @param pad
 * @return Padded string.
 */
PHP_FUNCTION(snobol_text_rpad) {
  char *s, *pad = " ";
  size_t slen, padlen = 1;
  zend_long width;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "sl|s", &s, &slen, &width, &pad,
                            &padlen) == FAILURE)
    return;
  /* Decode the pad string as a full UTF-8 codepoint (see lpad). */
  uint32_t pad_cp = (uint32_t)' ';
  if (padlen > 0 && !snobol_ord(pad, padlen, &pad_cp))
    pad_cp = (uint32_t)' ';
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_rpad(s, slen, (size_t)width, pad_cp, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Convert a codepoint to its UTF-8 encoding.
 * @param cp
 * @return UTF-8 string; false for invalid codepoints.
 */
PHP_FUNCTION(snobol_text_char) {
  zend_long cp;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "l", &cp) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (cp >= 0 && snobol_char_fn((uint32_t)cp, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_FALSE;
  }
  snobol_buf_free(&b);
}

/**
 * @brief Codepoint of the first character of a string.
 * @param s
 * @return Codepoint; false for empty input.
 */
PHP_FUNCTION(snobol_text_ord) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  uint32_t cp = 0;
  if (snobol_ord(s, slen, &cp)) {
    RETURN_LONG((zend_long)cp);
  } else {
    RETURN_FALSE;
  }
}

/**
 * @brief Unicode-aware uppercase conversion.
 * @param s
 * @return Uppercased string.
 */
PHP_FUNCTION(snobol_text_upper) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_upper(s, slen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/**
 * @brief Unicode-aware lowercase conversion.
 * @param s
 * @return Lowercased string.
 */
PHP_FUNCTION(snobol_text_lower) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  snobol_buf b = {0};
  snobol_buf_init(&b);
  if (snobol_lower(s, slen, &b)) {
    RETVAL_STRINGL(b.data, b.len);
  } else {
    RETVAL_STRINGL(s, slen);
  }
  snobol_buf_free(&b);
}

/* --- COMPARISON / TYPE-CHECK FUNCTIONS --- */

/**
 * @brief SNOBOL IDENT: true when both strings are identical.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_ident) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_ident(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL DIFFER: true when the strings differ.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_differ) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_differ(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL LEXEQ: lexicographic equality.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_lexeq) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_lexeq(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL LEXLT: lexicographic less-than.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_lexlt) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_lexlt(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL LEXGT: lexicographic greater-than.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_lexgt) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_lexgt(a, alen, b_s, blen));
}

/* Numeric comparison functions */
/**
 * @brief SNOBOL EQ: numeric equality.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_eq) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_eq(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL NE: numeric inequality.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_ne) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_ne(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL LT: numeric less-than.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_lt) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_lt(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL GT: numeric greater-than.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_gt) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_gt(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL LE: numeric less-or-equal.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_le) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_le(a, alen, b_s, blen));
}

/**
 * @brief SNOBOL GE: numeric greater-or-equal.
 * @param a
 * @param b
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_ge) {
  char *a, *b_s;
  size_t alen, blen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "ss", &a, &alen, &b_s, &blen) ==
      FAILURE)
    return;
  RETURN_BOOL(snobol_ge(a, alen, b_s, blen));
}

/**
 * @brief True when the string parses as an integer.
 * @param s
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_integer) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  RETURN_BOOL(snobol_integer(s, slen));
}

/**
 * @brief True when the string parses as a real number.
 * @param s
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_real) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  RETURN_BOOL(snobol_real(s, slen));
}

/**
 * @brief True when the string is numeric (integer or real).
 * @param s
 * @return Boolean.
 */
PHP_FUNCTION(snobol_text_numeric) {
  char *s;
  size_t slen;
  if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &s, &slen) == FAILURE)
    return;
  RETURN_BOOL(snobol_numeric(s, slen));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_snobol_get_api_version, 0, 0,
                                        IS_LONG, 0)
ZEND_END_ARG_INFO()

/**
 * @brief Encoded core API version (MAJOR<<16 | MINOR<<8 | PATCH).
 * @return zend_long.
 */
PHP_FUNCTION(snobol_get_api_version) {
  if (zend_parse_parameters_none() == FAILURE) {
    return;
  }
  RETURN_LONG((zend_long)snobol_get_api_version());
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_snobol_get_abi_version, 0, 0,
                                        IS_LONG, 0)
ZEND_END_ARG_INFO()

/**
 * @brief Core ABI version.
 * @return zend_long.
 */
PHP_FUNCTION(snobol_get_abi_version) {
  if (zend_parse_parameters_none() == FAILURE) {
    return;
  }
  RETURN_LONG((zend_long)snobol_get_abi_version());
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(ai_snobol_get_choice_stats, 0, 0,
                                        IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/**
 * @brief Global choice-point statistics (all zeros; per-match metrics are precise).
 * @return Associative array of counters.
 */
PHP_FUNCTION(snobol_get_choice_stats) {
  if (zend_parse_parameters_none() == FAILURE) {
    return;
  }

  array_init(return_value);
  /* Choice stats are not global but per-VM run. For global observability, we would
     * need a global collector. Since Choice Points are transient, only per-match
     * observability via `_metrics` is currently precise. This function returns 0s
     * for now until a global accumulator is added to the core. */
  add_assoc_long(return_value, "choice_push_count", (zend_long)0);
  add_assoc_long(return_value, "choice_allocated", (zend_long)0);
  add_assoc_long(return_value, "choice_stack_depth", (zend_long)0);
  add_assoc_long(return_value, "choice_stack_memory_usage", (zend_long)0);
}

static const zend_function_entry snobol_functions[] = {
    /* C function exports for string/comparison built-ins */
    PHP_FE(snobol_text_size, ai_text_size) PHP_FE(
        snobol_text_trim,
        ai_text_str1) PHP_FE(snobol_text_dupl,
                             ai_text_dupl) PHP_FE(snobol_text_reverse,
                                                  ai_text_str1)
        PHP_FE(snobol_text_substr, ai_text_substr) PHP_FE(
            snobol_text_replace,
            ai_text_str3) PHP_FE(snobol_text_replace_char,
                                 ai_text_str3) PHP_FE(snobol_text_lpad,
                                                      ai_text_pad)
            PHP_FE(snobol_text_rpad, ai_text_pad) PHP_FE(
                snobol_text_char,
                ai_text_char) PHP_FE(snobol_text_ord,
                                     ai_text_ord) PHP_FE(snobol_text_upper,
                                                         ai_text_str1)
                PHP_FE(snobol_text_lower, ai_text_str1) PHP_FE(
                    snobol_text_ident,
                    ai_text_str2_bool) PHP_FE(snobol_text_differ,
                                              ai_text_str2_bool)
                    PHP_FE(snobol_text_lexeq, ai_text_str2_bool) PHP_FE(
                        snobol_text_lexlt,
                        ai_text_str2_bool) PHP_FE(snobol_text_lexgt,
                                                  ai_text_str2_bool)
                        PHP_FE(snobol_text_eq, ai_text_str2_bool) PHP_FE(
                            snobol_text_ne,
                            ai_text_str2_bool) PHP_FE(snobol_text_lt,
                                                      ai_text_str2_bool)
                            PHP_FE(snobol_text_gt, ai_text_str2_bool) PHP_FE(
                                snobol_text_le,
                                ai_text_str2_bool) PHP_FE(snobol_text_ge,
                                                          ai_text_str2_bool)
                                PHP_FE(
                                    snobol_text_integer,
                                    ai_text_str1_bool) PHP_FE(snobol_text_real,
                                                              ai_text_str1_bool)
                                    PHP_FE(snobol_text_numeric,
                                           ai_text_str1_bool)
                                        PHP_FE(snobol_get_api_version,
                                               ai_snobol_get_api_version)
                                            PHP_FE(snobol_get_abi_version,
                                                   ai_snobol_get_abi_version)
                                                PHP_FE(
                                                    snobol_get_choice_stats,
                                                    ai_snobol_get_choice_stats)
                                                    PHP_FE_END};

zend_module_entry snobol_module_entry = {STANDARD_MODULE_HEADER,
                                         "snobol",
                                         snobol_functions,
                                         PHP_MINIT(snobol),
                                         NULL,
                                         NULL,
                                         NULL,
                                         PHP_MINFO(snobol),
                                         PHP_SNOBOL_VERSION,
                                         STANDARD_MODULE_PROPERTIES};

#ifdef COMPILE_DL_SNOBOL
ZEND_GET_MODULE(snobol)
#endif
