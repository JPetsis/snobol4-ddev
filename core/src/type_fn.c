/**
 * @file type_fn.c
 * @brief SNOBOL4 built-in comparison and type-checking function implementations
 *
 * Comparison functions use lexicographic byte ordering.
 * Type predicates check string format (integer or real number).
 */

#include "snobol/type_fn.h"
#include "snobol/snobol_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Numeric conversion helper
 * -------------------------------------------------------------------------- */

double snobol_str_to_double(const char *s, size_t len) {
  if (!s || len == 0) {
    return 0.0;
  }
  char *buf = (char *)snobol_malloc(len + 1);
  if (!buf) {
    return 0.0;
  }
  memcpy(buf, s, len);
  buf[len] = '\0';
  char *end = nullptr;
  double val = strtod(buf, &end);
  snobol_free(buf);
  return (end > buf) ? val : 0.0;
}

/* --------------------------------------------------------------------------
 * Numeric comparison functions
 * -------------------------------------------------------------------------- */

bool snobol_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) == snobol_str_to_double(b, b_len);
}

bool snobol_ne(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) != snobol_str_to_double(b, b_len);
}

bool snobol_lt(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) < snobol_str_to_double(b, b_len);
}

bool snobol_gt(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) > snobol_str_to_double(b, b_len);
}

bool snobol_le(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) <= snobol_str_to_double(b, b_len);
}

bool snobol_ge(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_str_to_double(a, a_len) >= snobol_str_to_double(b, b_len);
}

/* --------------------------------------------------------------------------
 * Internal comparison helper
 * -------------------------------------------------------------------------- */

/**
 * Three-way lexicographic comparison: returns <0, 0, or >0.
 */
static int snobol_lexcmp(const char *a, size_t a_len, const char *b,
                         size_t b_len) {
  size_t min_len = a_len < b_len ? a_len : b_len;
  if (min_len > 0) {
    int r = memcmp(a, b, min_len);
    if (r != 0) {
      return r;
    }
  }
  if (a_len < b_len) {
    return -1;
  }
  if (a_len > b_len) {
    return 1;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * IDENT
 * -------------------------------------------------------------------------- */

bool snobol_ident(const char *a, size_t a_len, const char *b, size_t b_len) {
  if (a_len != b_len) {
    return false;
  }
  if (a_len == 0) {
    return true;
  }
  return memcmp(a, b, a_len) == 0;
}

/* --------------------------------------------------------------------------
 * DIFFER
 * -------------------------------------------------------------------------- */

bool snobol_differ(const char *a, size_t a_len, const char *b, size_t b_len) {
  return (!snobol_ident(a, a_len, b, b_len)) != 0;
}

/* --------------------------------------------------------------------------
 * LEXEQ
 * -------------------------------------------------------------------------- */

bool snobol_lexeq(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_lexcmp(a, a_len, b, b_len) == 0;
}

/* --------------------------------------------------------------------------
 * LEXLT
 * -------------------------------------------------------------------------- */

bool snobol_lexlt(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_lexcmp(a, a_len, b, b_len) < 0;
}

/* --------------------------------------------------------------------------
 * LEXGT
 * -------------------------------------------------------------------------- */

bool snobol_lexgt(const char *a, size_t a_len, const char *b, size_t b_len) {
  return snobol_lexcmp(a, a_len, b, b_len) > 0;
}

/* --------------------------------------------------------------------------
 * INTEGER
 * -------------------------------------------------------------------------- */

/**
 * INTEGER: succeed if string represents an integer.
 * Pattern: [+-]?\d+
 */
bool snobol_integer(const char *str, size_t len) {
  if (!str || len == 0) {
    return false;
  }
  size_t i = 0;
  /* Optional sign */
  if (str[i] == '+' || str[i] == '-') {
    i++;
  }
  /* Must have at least one digit */
  if (i >= len) {
    return false;
  }
  size_t digit_start = i;
  while (i < len && str[i] >= '0' && str[i] <= '9') {
    i++;
  }
  /* Consumed all bytes and there was at least one digit */
  return ((i == len) && (i > digit_start)) != 0;
}

/* --------------------------------------------------------------------------
 * REAL
 * -------------------------------------------------------------------------- */

/**
 * REAL: succeed if string represents a real number.
 * Pattern: [+-]?\d+(.\d*)?([eE][+-]?\d+)?
 * The decimal point itself is not required to have digits on both sides
 * (e.g. "1." or ".5" would not be accepted—must have digits before decimal).
 */
bool snobol_real(const char *str, size_t len) {
  if (!str || len == 0) {
    return false;
  }
  size_t i = 0;
  /* Optional sign */
  if (i < len && (str[i] == '+' || str[i] == '-')) {
    i++;
  }
  /* Must have at least one integer digit */
  if (i >= len || str[i] < '0' || str[i] > '9') {
    return false;
  }
  while (i < len && str[i] >= '0' && str[i] <= '9') {
    i++;
  }
  /* Optional decimal part */
  if (i < len && str[i] == '.') {
    i++;
    while (i < len && str[i] >= '0' && str[i] <= '9') {
      i++;
    }
  }
  /* Optional exponent */
  if (i < len && (str[i] == 'e' || str[i] == 'E')) {
    i++;
    if (i < len && (str[i] == '+' || str[i] == '-')) {
      i++;
    }
    /* Must have at least one exponent digit */
    if (i >= len || str[i] < '0' || str[i] > '9') {
      return false;
    }
    while (i < len && str[i] >= '0' && str[i] <= '9') {
      i++;
    }
  }
  /* All bytes consumed and no decimal-only or sign-only strings */
  return i == len;
}

/* --------------------------------------------------------------------------
 * NUMERIC
 * -------------------------------------------------------------------------- */

bool snobol_numeric(const char *str, size_t len) {
  return (snobol_integer(str, len) || snobol_real(str, len)) != 0;
}
