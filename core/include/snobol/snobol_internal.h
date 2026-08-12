#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/* C23 keyword compatibility for pre-C23 compilers (MSVC, GCC/Clang < C23).
 * __STDC_VERSION__ == 202311L for C23; anything lower (or undefined) means the
 * compiler does not recognise 'nullptr' or 'constexpr' as keywords in C mode.
 */
#ifndef __cplusplus
#if !defined(__STDC_VERSION__) || (__STDC_VERSION__ < 202311L)
#ifndef nullptr
#define nullptr NULL
#endif
#ifndef constexpr
#define constexpr static const
#endif
#endif
#endif

/* C11 _Alignof: MSVC in default mode (without /std:c11+) does not recognise
 * _Alignof; use the MSVC-specific __alignof extension instead. */
#if defined(_MSC_VER) && !defined(_Alignof)
#define _Alignof __alignof
#endif

/* Default to standalone build unless PHP_BUILD is explicitly defined */
#ifndef PHP_BUILD
#define STANDALONE_BUILD 1
#endif

#ifdef STANDALONE_BUILD
#include <stdlib.h>
#include <string.h>
#else
#include "php.h"
#endif

/* Memory management macros to abstract away PHP's allocator */
#ifdef STANDALONE_BUILD
#define snobol_malloc malloc
#define snobol_free free
#define snobol_realloc realloc
#define snobol_calloc calloc
#else
#define snobol_malloc emalloc
#define snobol_free efree
#define snobol_realloc erealloc
#define snobol_calloc ecalloc
#endif

/**
 * snobol_check_alloc(ptr) — NULL-check a heap allocation.
 *
 * In STANDALONE builds this returns false and the pointer is null.
 * In PHP builds the allocator never returns NULL, so this is a no-op
 * that always returns true.
 *
 * Usage:
 *   char *buf = snobol_malloc(n);
 *   if (!snobol_check_alloc(buf)) return ERROR_NOMEM;
 */
#ifdef STANDALONE_BUILD
#define snobol_check_alloc(ptr) ((ptr) != NULL)
#else
#define snobol_check_alloc(ptr) ((void)(ptr), true)
#endif

/* Debug logging */
/* #define SNOBOL_TRACE 1 */
#ifdef SNOBOL_TRACE
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
static inline void snobol_log_impl(const char *file, int line, const char *fmt,
                                   ...) {
  FILE *f = fopen("/tmp/snobol_debug.log", "a");
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
#define SNOBOL_LOG(fmt, ...) \
  snobol_log_impl(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define SNOBOL_LOG(fmt, ...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
