#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file unicode_fold.h
 * @brief Unicode case-folding lookup tables for libsnobol4
 *
 * Provides codepoint-level case conversion with no external ICU dependency.
 * All data is compiled-in static C arrays.
 *
 * Coverage:
 *   - U+0000–U+007F: ASCII fast path (arithmetic: ±32)
 *   - U+00C0–U+00FF: Latin-1 Supplement (UPPER_MAP[256] table)
 *   - U+0100–U+FFFF: Full Basic Multilingual Plane (generated pair tables,
 *                     binary search)
 *
 * Codepoints outside the BMP (U+10000+) are returned unchanged (identity
 * mapping).  Some lowercase codepoints expand to multiple uppercase
 * codepoints; the best-known case is U+00DF (ß) → "SS" (U+0053 U+0053).
 */

#include <stdint.h>
#include "snobol/c23_compat.h"

/**
 * Convert a Unicode codepoint to its uppercase equivalent(s).
 *
 * @param cp      Input codepoint.
 * @param out     Caller-provided output buffer of at least 2 uint32_t elements.
 *                On return, holds the resulting uppercase codepoint(s).
 *                The buffer is written by the function; ownership stays with
 *                the caller.  Do NOT free the contents — they are scalar
 * values.
 * @param out_len Set to the number of codepoints written to @p out (1 or 2).
 */
void snobol_to_upper_cp(uint32_t cp, uint32_t *out, int *out_len);

/**
 * Convert a Unicode codepoint to its lowercase equivalent.
 *
 * @param cp  Input codepoint.
 * @return    Lowercase codepoint, or cp itself if no mapping is found.
 */
uint32_t snobol_to_lower_cp(uint32_t cp);

#ifdef __cplusplus
}
#endif
