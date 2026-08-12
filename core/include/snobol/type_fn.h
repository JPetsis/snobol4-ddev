/**
 * @file type_fn.h
 * @brief SNOBOL4 built-in comparison and type-checking functions
 *
 * Comparison functions use lexicographic (byte-level) ordering.
 * Type predicates check string format for numeric types.
 *
 * All predicates return true (succeed) or false (fail), matching SNOBOL4
 * semantics where a failing function causes the enclosing pattern to fail.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif


#include <stdbool.h>
#include "snobol/c23_compat.h"
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Numeric comparison helpers (for internal use in type_fn.c and PHP binding)
 * -------------------------------------------------------------------------- */
/**
 * Convert a length-bounded string to double, following SNOBOL4 numeric
 * conversion semantics (non-numeric strings yield 0.0).
 * @param s     Input string (UTF-8, not necessarily null-terminated)
 * @param len   Byte length of input
 * @return      Numeric value, or 0.0 if no numeric prefix is found
 */
double snobol_str_to_double(const char *s, size_t len);

/**
 * EQ: numeric equality – succeeds if a == b numerically.
 * Both strings are converted to double before comparison.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric values are equal
 */
bool snobol_eq(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * NE: numeric inequality – succeeds if a != b numerically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric values differ
 */
bool snobol_ne(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * LT: less-than – succeeds if a < b numerically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric value of a is less than b
 */
bool snobol_lt(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * GT: greater-than – succeeds if a > b numerically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric value of a is greater than b
 */
bool snobol_gt(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * LE: less-than-or-equal – succeeds if a <= b numerically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric value of a is <= b
 */
bool snobol_le(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * GE: greater-than-or-equal – succeeds if a >= b numerically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if numeric value of a is >= b
 */
bool snobol_ge(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * IDENT: identity predicate – succeeds if two strings are identical.
 * Equivalent to strcmp == 0 on strings.
 * @param a      First string (UTF-8)
 * @param a_len  Byte length of first string
 * @param b      Second string (UTF-8)
 * @param b_len  Byte length of second string
 * @return       true if strings are identical, false otherwise
 */
bool snobol_ident(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * DIFFER: difference predicate – succeeds if two strings differ.
 * Logical inverse of IDENT.
 * @param a      First string
 * @param a_len  Byte length of first string
 * @param b      Second string
 * @param b_len  Byte length of second string
 * @return       true if strings differ, false if identical
 */
bool snobol_differ(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * LEXEQ: lexical equality – succeeds if a == b lexicographically.
 * Uses memcmp with length comparison.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if a == b lexicographically
 */
bool snobol_lexeq(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * LEXLT: lexical less-than – succeeds if a < b lexicographically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if a < b lexicographically
 */
bool snobol_lexlt(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * LEXGT: lexical greater-than – succeeds if a > b lexicographically.
 * @param a      First string
 * @param a_len  Byte length
 * @param b      Second string
 * @param b_len  Byte length
 * @return       true if a > b lexicographically
 */
bool snobol_lexgt(const char *a, size_t a_len, const char *b, size_t b_len);

/**
 * INTEGER: type predicate – succeeds if the string represents an integer.
 * Valid format: optional +/- sign followed by one or more decimal digits.
 * @param str  Input string
 * @param len  Byte length
 * @return     true if string is a valid integer representation
 */
bool snobol_integer(const char *str, size_t len);

/**
 * REAL: type predicate – succeeds if the string represents a real number.
 * Valid format: optional sign, digits, optional decimal point + digits,
 * optional exponent (e/E with optional sign and digits).
 * Example: "12.34", "1.23e-4", "-0.5", "+1E10"
 * @param str  Input string
 * @param len  Byte length
 * @return     true if string is a valid real number representation
 */
bool snobol_real(const char *str, size_t len);

/**
 * NUMERIC: union type predicate – succeeds if the string is INTEGER or REAL.
 * Equivalent to: snobol_integer(str, len) || snobol_real(str, len)
 * @param str  Input string
 * @param len  Byte length
 * @return     true if string represents any numeric value
 */
bool snobol_numeric(const char *str, size_t len);

#ifdef __cplusplus
}
#endif
