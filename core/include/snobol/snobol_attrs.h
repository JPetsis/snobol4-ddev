#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @file snobol_attrs.h
 * @brief Portable function-attribute macros for SNOBOL4
 *
 * Provides SNOBOL_NODISCARD and SNOBOL_MAYBE_UNUSED that map to the
 * appropriate compiler-specific spelling:
 *
 *   Compiler / standard        SNOBOL_NODISCARD SNOBOL_MAYBE_UNUSED
 *   ─────────────────────────  ────────────────────────────────
 * ──────────────────────────────── C23 / C++17+               [[nodiscard]]
 * [[maybe_unused]] GCC / Clang (pre-C23) __attribute__((warn_unused_result))
 * __attribute__((unused)) MSVC / unknown             (empty — silently drop the
 * hint) (empty)
 */

/* ── SNOBOL_NODISCARD ──────────────────────────────────────────────────── */
#ifndef SNOBOL_NODISCARD
#if defined(__cplusplus) && __cplusplus >= 201703L
#define SNOBOL_NODISCARD [[nodiscard]]
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 202311L
#define SNOBOL_NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
#define SNOBOL_NODISCARD __attribute__((warn_unused_result))
#else
#define SNOBOL_NODISCARD
#endif
#endif

/* ── SNOBOL_MAYBE_UNUSED ───────────────────────────────────────────────── */
#ifndef SNOBOL_MAYBE_UNUSED
#if defined(__cplusplus) && __cplusplus >= 201703L
#define SNOBOL_MAYBE_UNUSED [[maybe_unused]]
#elif !defined(__cplusplus) && defined(__STDC_VERSION__) && \
    __STDC_VERSION__ >= 202311L
#define SNOBOL_MAYBE_UNUSED [[maybe_unused]]
#elif defined(__GNUC__) || defined(__clang__)
#define SNOBOL_MAYBE_UNUSED __attribute__((unused))
#else
#define SNOBOL_MAYBE_UNUSED
#endif
#endif

/* ── SNOBOL_ALWAYS_INLINE / SNOBOL_FORCE_INLINE ────────────────────────── */
#ifndef SNOBOL_ALWAYS_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_ALWAYS_INLINE __attribute__((always_inline))
#elifdef _MSC_VER
#define SNOBOL_ALWAYS_INLINE __forceinline
#else
#define SNOBOL_ALWAYS_INLINE inline
#endif
#endif

/* ── SNOBOL_HOT / SNOBOL_COLD ──────────────────────────────────────────── */
/* Place frequently executed code in the .text.hot section (hot) and
 * rarely taken paths (error handling, fallback branches) in .text.cold
 * (cold) to reduce iTLB pressure on the hot dispatch / scan loops. */
#ifndef SNOBOL_HOT
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_HOT __attribute__((hot))
#else
#define SNOBOL_HOT
#endif
#endif

#ifndef SNOBOL_COLD
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_COLD __attribute__((cold))
#else
#define SNOBOL_COLD
#endif
#endif

/* ── SNOBOL_PURE / SNOBOL_CONST ────────────────────────────────────────── */
/* pure: result depends only on arguments + visible memory; enables CSE/DCE.
 * const: result depends only on arguments (no memory reads at all). */
#ifndef SNOBOL_PURE
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_PURE __attribute__((pure))
#else
#define SNOBOL_PURE
#endif
#endif

#ifndef SNOBOL_CONST
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_CONST __attribute__((const))
#else
#define SNOBOL_CONST
#endif
#endif

/* ── SNOBOL_RESTRICT ───────────────────────────────────────────────────── */
/* Portable spelling of the C 'restrict' qualifier for pointer parameters of
 * hot functions whose pointers are provably non-aliasing. */
#ifndef SNOBOL_RESTRICT
#ifdef _MSC_VER
#define SNOBOL_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define SNOBOL_RESTRICT __restrict__
#else
#define SNOBOL_RESTRICT
#endif
#endif

/* ── likely() / unlikely() ─────────────────────────────────────────────── */
/* Branch prediction hints.  Wrapped so unknown compilers degrade to the plain
 * expression with no overhead. */
#ifndef likely
#if defined(__GNUC__) || defined(__clang__)
#define likely(x) __builtin_expect(!!(x), 1)
#else
#define likely(x) (x)
#endif
#endif

#ifndef unlikely
#if defined(__GNUC__) || defined(__clang__)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define unlikely(x) (x)
#endif
#endif

/* ── SNOBOL_ALIGNED ─────────────────────────────────────────────────────── */
/* Cache-line alignment hint for hot dispatch / scan-loop entry points so the
 * function prologue and the innermost loop body land in the same cache line.
 * GCC/Clang support per-function alignment via __attribute__((aligned(n)));
 * MSVC has no per-function alignment attribute (__declspec(align(n)) applies
 * only to variables, struct members and tag types), so it is a no-op there. */
#ifndef SNOBOL_ALIGNED
#if defined(__GNUC__) || defined(__clang__)
#define SNOBOL_ALIGNED(n) __attribute__((aligned(n)))
#elifdef _MSC_VER
#define SNOBOL_ALIGNED(n)
#else
#define SNOBOL_ALIGNED(n)
#endif
#endif

/* ── SNOBOL_THREAD_LOCAL ─────────────────────────────────────────────────── */
/* Thread-local storage qualifier.  MSVC (pre-C11 threads) uses __declspec;
 * C11+ (GCC/Clang, modern MSVC) uses _Thread_local. */
#ifndef SNOBOL_THREAD_LOCAL
#if defined(_MSC_VER) && !defined(__STDC_VERSION__)
#define SNOBOL_THREAD_LOCAL __declspec(thread)
#else
#define SNOBOL_THREAD_LOCAL _Thread_local
#endif
#endif

#ifdef __cplusplus
}
#endif
