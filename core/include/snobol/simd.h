#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file simd.h
 * @brief SIMD capability detection and platform abstraction macros
 *
 * Provides compile-time detection of SIMD instruction set extensions
 * (AVX2 on x86-64, NEON on ARM64) and defines portable macros for
 * SIMD width and feature availability.
 *
 * The scalar fallback (SNOBOL_SIMD_WIDTH=1) is used when no SIMD
 * support is detected at compile time.
 */

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * SIMD capability detection
 * ---------------------------------------------------------------------------
 */

/** @def SNOBOL_HAS_AVX2
 *  Defined to 1 when AVX2 is available (x86-64 with -mavx2). */
#ifdef __AVX2__
#define SNOBOL_HAS_AVX2 1
#else
#define SNOBOL_HAS_AVX2 0
#endif

/** @def SNOBOL_HAS_NEON
 *  Defined to 1 when ARM NEON is available (ARM64/AArch64). */
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#define SNOBOL_HAS_NEON 1
#else
#define SNOBOL_HAS_NEON 0
#endif

/** @def SNOBOL_HAS_SIMD
 *  Defined to 1 when any SIMD extension is available. */
#define SNOBOL_HAS_SIMD (SNOBOL_HAS_AVX2 || SNOBOL_HAS_NEON)

/* ---------------------------------------------------------------------------
 * SIMD width constants
 * ---------------------------------------------------------------------------
 */

/** @def SNOBOL_SIMD_WIDTH
 *  Number of bytes processed per SIMD iteration:
 *   - 32 for AVX2 (256-bit registers)
 *   - 16 for NEON (128-bit registers)
 *   -  1 for scalar fallback */
#if SNOBOL_HAS_AVX2
enum { SNOBOL_SIMD_WIDTH = 32 };
#elif SNOBOL_HAS_NEON
enum { SNOBOL_SIMD_WIDTH = 16 };
#else
enum { SNOBOL_SIMD_WIDTH = 1 };
#endif

/* ---------------------------------------------------------------------------
 * SIMD NFA state type
 *
 * An NFA state set is represented as a bitmask where each bit corresponds
 * to one NFA state. The maximum number of states is limited by the SIMD
 * register width (256 for AVX2, 128 for NEON).
 * ---------------------------------------------------------------------------
 */

#if SNOBOL_HAS_AVX2
#include <immintrin.h>
typedef __m256i simd_reg_t;
enum { SNOBOL_SIMD_NFA_MAX_STATES = 256 };

#elif SNOBOL_HAS_NEON
#include <arm_neon.h>
typedef uint8x16_t simd_reg_t;
enum { SNOBOL_SIMD_NFA_MAX_STATES = 128 };

#else
/* Scalar fallback: use uint64_t[4] (256 bits) */
typedef uint64_t simd_reg_t[4];
enum { SNOBOL_SIMD_NFA_MAX_STATES = 256 };
#endif

#ifdef __cplusplus
}
#endif
