#pragma once

/* C23 keyword compatibility for pre-C23 compilers (MSVC, GCC/Clang < C23).
 * __STDC_VERSION__ == 202311L for C23; anything lower (or undefined) means the
 * compiler does not recognise 'nullptr' or 'constexpr' as keywords in C mode.
 *
 * Every self-contained public header includes this so standalone consumers
 * (and the test suite, which includes narrow headers directly) get the
 * fallbacks regardless of which header they pull in. */
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
