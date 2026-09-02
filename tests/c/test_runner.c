#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../../core/include/snobol/snobol_internal.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/types.h>
#endif

/* ---------------------------------------------------------------------------
 * Portable monotonic clock
 *
 * POSIX clock_gettime(CLOCK_MONOTONIC) is not available on Windows/MSVC.
 * Use QueryPerformanceCounter there; fall back to clock_gettime everywhere
 * else.
 * ---------------------------------------------------------------------------
 */
#ifdef _WIN32
#include <windows.h>
static void snobol_clock_gettime(struct timespec *ts) {
  LARGE_INTEGER freq, counter;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&counter);
  ts->tv_sec = (time_t)(counter.QuadPart / freq.QuadPart);
  ts->tv_nsec =
      (long)((counter.QuadPart % freq.QuadPart) * 1000000000LL / freq.QuadPart);
}
#define SNOBOL_CLOCK_GETTIME(ts) snobol_clock_gettime(ts)
#else
#define SNOBOL_CLOCK_GETTIME(ts) clock_gettime(CLOCK_MONOTONIC, ts)
#endif

/* ── Test framework ──────────────────────────────────────────────────────── */

typedef struct {
  int passed;
  int failed;
  const char *current_suite;
  int case_count; /* number of test_suite() calls (test cases) */
} TestContext;

enum { MAX_SUITES = 64 };
enum { NAME_WIDTH = 36 /* fixed width for suite name column          */ };
#define COL_SEP "  "      /* two-space column separator                  */
enum { RULE_WIDTH = 80 }; /* total width of separator lines */

typedef struct {
  const char *name;
  int passed;
  int failed;
  double time_ms;
} SuiteResult;

static TestContext test_ctx = {0};
static jmp_buf test_jump;
static SuiteResult suite_results[MAX_SUITES];
static int suite_count = 0;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static double elapsed_ms(struct timespec t0, struct timespec t1) {
  return ((double)(t1.tv_sec - t0.tv_sec) * 1000.0) +
         ((double)(t1.tv_nsec - t0.tv_nsec) / 1.0e6);
}

static void print_rule_w(char ch, int width) {
  for (int i = 0; i < width; i++) {
    putchar(ch);
  }
  putchar('\n');
}

static void print_rule(char ch) {
  print_rule_w(ch, RULE_WIDTH);
}

static void signal_handler(int sig) {
  const char *sig_name;
  if (sig == SIGILL) {
    sig_name = "SIGILL";
  } else if (sig == SIGSEGV) {
    sig_name = "SIGSEGV";
  } else {
    sig_name = "UNKNOWN";
  }
  printf("\n  [CRASH] signal %d (%s) in suite: %s\n", sig, sig_name,
         test_ctx.current_suite);
  test_ctx.failed++;
  longjmp(test_jump, 1);
}

/* ── Watchdog timer ────────────────────────────────────────────────────────
 *
 * Each test suite is wrapped with a watchdog that aborts the process if the
 * suite takes longer than SNOBOL_TEST_TIMEOUT_SECS (default 60).  The
 * watchdog uses a dedicated background thread that calls abort() when the
 * timer fires.  The aborted process produces a core file (where available)
 * and the parent CI job captures it as an artifact.
 * ---------------------------------------------------------------------------
 */
static int g_timeout_secs = 60;
static volatile int g_watchdog_active = 0;
#ifdef _WIN32
static HANDLE g_watchdog_thread = nullptr;
static DWORD g_watchdog_tid = 0;
static HANDLE g_watchdog_event = nullptr; /* signalled by watchdog_stop() */
#else
static pthread_t g_watchdog_thread;
#endif

static void watchdog_log(const char *fmt, ...) {
#ifdef _WIN32
  FILE *fp = nullptr;
  if (fopen_s(&fp, "watchdog.log", "a") != 0 || !fp)
    return;
#else
  int fd = open("watchdog.log", O_WRONLY | O_CREAT | O_APPEND, 0600);
  FILE *fp = fd >= 0 ? fdopen(fd, "a") : nullptr;
  if (!fp) {
    return;
  }
#endif
  va_list ap;
  va_start(ap, fmt);
  vfprintf(fp, fmt, ap);
  va_end(ap);
  fputc('\n', fp);
  fclose(fp);
}

#ifdef _WIN32
static DWORD WINAPI watchdog_proc(LPVOID arg) {
  (void)arg;
  int secs = g_timeout_secs;
  /* Wait for either the stop-event (suite finished) or the timeout. */
  DWORD result = WaitForSingleObject(g_watchdog_event, (DWORD)(secs * 1000));
  if (result == WAIT_TIMEOUT && g_watchdog_active) {
    watchdog_log("TIMEOUT after %d s in suite: %s", secs,
                 test_ctx.current_suite);
    fflush(stdout);
    abort();
  }
  return 0;
}
#else
static void *watchdog_proc(void *arg) {
  (void)arg;
  int secs = g_timeout_secs;
  /* Sleep in small slices so the active flag is checked frequently. */
  for (int i = 0; i < secs * 10 && g_watchdog_active; i++) {
    struct timespec ts = {0, 100L * 1000 * 1000}; /* 100 ms */
    nanosleep(&ts, nullptr);
  }
  if (g_watchdog_active) {
    watchdog_log("TIMEOUT after %d s in suite: %s", secs,
                 test_ctx.current_suite);
    fflush(stdout);
    /* abort() generates SIGABRT which is not blocked by the test's
     * SIGSEGV handler.  This terminates the process so the CI job
     * can capture the core dump. */
    abort();
  }
  return nullptr;
}
#endif

static void watchdog_start(void) {
  const char *env = getenv("SNOBOL_TEST_TIMEOUT_SECS");
  if (env && env[0]) {
    int v = (int)strtol(env, nullptr, 10);
    if (v > 0) {
      g_timeout_secs = v;
    }
  }
  g_watchdog_active = 1;
#ifdef _WIN32
  /* Create (or reset) the stop-event before spawning the thread. */
  if (!g_watchdog_event)
    g_watchdog_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  else
    ResetEvent(g_watchdog_event);
  g_watchdog_thread =
      CreateThread(nullptr, 0, watchdog_proc, nullptr, 0, &g_watchdog_tid);
#else
  pthread_create(&g_watchdog_thread, nullptr, watchdog_proc, nullptr);
#endif
}

static void watchdog_stop(void) {
  g_watchdog_active = 0;
#ifdef _WIN32
  /* Signal the event so the watchdog thread wakes immediately. */
  if (g_watchdog_event)
    SetEvent(g_watchdog_event);
  if (g_watchdog_thread) {
    WaitForSingleObject(g_watchdog_thread, INFINITE);
    CloseHandle(g_watchdog_thread);
    g_watchdog_thread = nullptr;
  }
#else
  pthread_join(g_watchdog_thread, nullptr);
#endif
}

/* ── Public API (called by individual test files) ────────────────────────── */

/* Called multiple times within a suite to label sub-groups */
void test_suite(const char *name) {
  test_ctx.current_suite = name;
  test_ctx.case_count++;
  printf("  -- %s\n", name);
}

void test_assert(bool condition, const char *message) {
  if (condition) {
    test_ctx.passed++;
    printf("     ✓  %s\n", message);
  } else {
    test_ctx.failed++;
    printf("     ✗  %s\n", message);
  }
  fflush(stdout);
}

/* ── RUN_SUITE macro ─────────────────────────────────────────────────────── */
/*
 * Usage: RUN_SUITE("Human Name", function_name);
 *
 * Prints:
 *   ▸ Human Name
 *     ·· sub-section (from test_suite() calls inside fn)
 *        ✓  assertion
 *   ↳ N passed · M failed · X.XX ms
 */
#define RUN_SUITE(display_name, fn)                                            \
  do {                                                                         \
    int _p0 = test_ctx.passed;                                                 \
    int _f0 = test_ctx.failed;                                                 \
    struct timespec _t0;                                                       \
    struct timespec _t1;                                                       \
    printf("\n▸ %s\n", (display_name));                                        \
    fflush(stdout);                                                            \
    watchdog_start();                                                          \
    SNOBOL_CLOCK_GETTIME(&_t0);                                                \
    if (setjmp(test_jump) == 0) {                                              \
      fn();                                                                    \
    }                                                                          \
    SNOBOL_CLOCK_GETTIME(&_t1);                                                \
    watchdog_stop();                                                           \
    int _sp = test_ctx.passed - _p0;                                           \
    int _sf = test_ctx.failed - _f0;                                           \
    double _ms = elapsed_ms(_t0, _t1);                                         \
    if (_sf > 0)                                                               \
      printf("  ↳  %d passed  |  \033[31m%d failed\033[0m  |  %.2f ms\n", _sp, \
             _sf, _ms);                                                        \
    else                                                                       \
      printf("  ↳  %d passed  |  0 failed  |  %.2f ms\n", _sp, _ms);           \
    fflush(stdout);                                                            \
    if (suite_count < MAX_SUITES)                                              \
      suite_results[suite_count++] =                                           \
          (SuiteResult){(display_name), _sp, _sf, _ms};                        \
  } while (0)

/* ── Forward declarations ────────────────────────────────────────────────── */
void test_vm_suite(void);
void test_table_suite(void);
void test_array_suite(void);
void test_dynamic_pattern_suite(void);
void test_table_ops_suite(void);
void test_template_ops_suite(void);
void test_control_flow_suite(void);
void test_backtracking_suite(void);
void test_catastrophic_suite(void);
void test_search_meta_cache_suite(void);
void test_search_suite(void);
void test_search_ex_api_suite(void);
void test_search_vm_capture_suite(void);
void test_capture_registers_suite(void);
void test_search_anchored_suite(void);
void test_search_literal_only_anchored_suite(void);
void test_search_batch_ex_suite(void);
#ifdef SNOBOL_PIKE_SCAN
void test_pike_scan_suite(void);
#endif
void test_search_prefilter_suite(void);
void test_search_batch_suite(void);
void test_lexer_suite(void);
void test_parser_suite(void);
void test_ast_suite(void);
void test_compiler_suite(void);
int test_stress_backtracking_main(void);
/* Built-in function test suites */
void test_pattern_breakx_suite(void);
void test_pattern_bal_suite(void);
void test_pattern_fence_suite(void);
void test_pattern_pos_suite(void);
void test_pattern_tab_suite(void);
void test_pattern_abort_suite(void);
void test_pattern_fail_suite(void);
void test_pattern_succeed_suite(void);
void test_string_size_suite(void);
void test_string_transform_suite(void);
void test_string_ops_suite(void);
void test_string_char_suite(void);
void test_string_case_suite(void);
void test_comparison_suite(void);
void test_comparison_numeric_suite(void);
void test_builtin_dispatch_suite(void);
void test_search_runtime_suite(void);
void test_fusion_suite(void);
void test_fusion_tier_suite(void);
void test_compact_choice_suite(void);
void test_vm_trail_suite(void);
void test_choice_arena_suite(void);
void test_choice_shortcircuit_suite(void);
/* Unicode / API new suites */
void test_unicode_fold_suite(void);
void test_pattern_case_suite(void);
void test_api_version_suite(void);
void test_api_match_suite(void);
void test_api_literal_match_suite(void);
void test_api_search_next_suite(void);
void test_builder_compile_suite(void);
void test_reusable_match_suite(void);
void test_reuse_search_suite(void);
void test_break_grammar_suite(void);
void test_property_based_suite(void);
void test_source_parity_suite(void);
void test_search_automaton_suite(void);
void test_search_simd_suite(void);
void test_search_alt_literals_suite(void);
void test_automaton_skip_suite(void);
void test_arena_suite(void);
void test_search_oracle_suite(void);
void test_search_oracle_meta_suite(void);
void test_search_oracle_generator_suite(void);

/* ── main ────────────────────────────────────────────────────────────────── */

/* Width of the summary table rules + top/bottom banners, so all
 * divider lines share one width and match the data rows exactly.
 * Row formats (HEADER/DATA/TOTAL) are all exactly name_w + 31 glyphs:
 *   "  ✓ <name>  <Passed>  <Failed>  <Time>"
 *   = 2 + 1(✓) + 1 + name_w(name field) + 2 + 7 + 2 + 7 + 2 + 9
 * The ✓ is 3 bytes but 1 glyph, so the byte widths differ from the rule's
 * ASCII width by 2 bytes — invisible on screen.  Return name_w + 31 so the
 * rule matches the content rows glyph-for-glyph. */
static int summary_rule_w(void) {
  int name_w = 12; /* "TOTAL" + margin */
  for (int i = 0; i < suite_count; i++) {
    int len = (int)strlen(suite_results[i].name);
    if (len > name_w) {
      name_w = len;
    }
  }
  name_w += 2;
  return name_w + 31;
}

int main(void) {
  signal(SIGILL, signal_handler);
  signal(SIGSEGV, signal_handler);
#ifdef SIGBUS
  signal(SIGBUS, signal_handler);
#endif

  /* Core VM / data structures */
  RUN_SUITE("VM", test_vm_suite);
  RUN_SUITE("Tables", test_table_suite);
  RUN_SUITE("Arrays", test_array_suite);
  RUN_SUITE("Dynamic Patterns", test_dynamic_pattern_suite);
  RUN_SUITE("Table Ops", test_table_ops_suite);
  RUN_SUITE("Template Ops", test_template_ops_suite);

  /* Pattern matching engine */
  RUN_SUITE("Control Flow", test_control_flow_suite);
  RUN_SUITE("Backtracking", test_backtracking_suite);
  RUN_SUITE("Catastrophic Backtracking", test_catastrophic_suite);

  RUN_SUITE("Search: cached metadata", test_search_meta_cache_suite);
  RUN_SUITE("Search: tier caching", test_search_suite);
  RUN_SUITE("Search: stateful _ex API", test_search_ex_api_suite);
  RUN_SUITE("Search: VM captures", test_search_vm_capture_suite);
  RUN_SUITE("Captures: register-style", test_capture_registers_suite);
  RUN_SUITE("Search: Anchored Entry", test_search_anchored_suite);
  RUN_SUITE("Search: Anchored literal-only early-exit",
            test_search_literal_only_anchored_suite);
  RUN_SUITE("Search: stateful batch_ex + tri-state eligible",
            test_search_batch_ex_suite);
#ifdef SNOBOL_PIKE_SCAN
  RUN_SUITE("Search: Pike Scan", test_pike_scan_suite);
#endif
  RUN_SUITE("Search: Required-Byte Prefilter", test_search_prefilter_suite);
  RUN_SUITE("Search: batch API parity with per-call loop",
            test_search_batch_suite);

  /* Lexer / Parser / AST */
  RUN_SUITE("Lexer", test_lexer_suite);
  RUN_SUITE("Parser", test_parser_suite);
  RUN_SUITE("AST", test_ast_suite);
  RUN_SUITE("Compiler", test_compiler_suite);

  /* Built-in functions */
  RUN_SUITE("Pattern: BREAKX", test_pattern_breakx_suite);
  RUN_SUITE("Pattern: BAL", test_pattern_bal_suite);
  RUN_SUITE("Pattern: FENCE/REM/RPOS/RTAB", test_pattern_fence_suite);
  RUN_SUITE("Pattern: POS", test_pattern_pos_suite);
  RUN_SUITE("Pattern: TAB", test_pattern_tab_suite);
  RUN_SUITE("Pattern: ABORT", test_pattern_abort_suite);
  RUN_SUITE("Pattern: FAIL", test_pattern_fail_suite);
  RUN_SUITE("Pattern: SUCCEED", test_pattern_succeed_suite);
  RUN_SUITE("String: SIZE", test_string_size_suite);
  RUN_SUITE("String: TRIM/DUPL/REVERSE", test_string_transform_suite);
  RUN_SUITE("String: SUBSTR/REPLACE/PAD", test_string_ops_suite);
  RUN_SUITE("String: CHAR/ORD", test_string_char_suite);
  RUN_SUITE("String: UPPER/LOWER", test_string_case_suite);
  RUN_SUITE("Comparison predicates", test_comparison_suite);
  RUN_SUITE("Comparison: Numeric (EQ/NE/LT/GT/LE/GE)",
            test_comparison_numeric_suite);
  RUN_SUITE("Built-in dispatch (OP_EVAL)", test_builtin_dispatch_suite);

  /* Search runtime */
  RUN_SUITE("Search Runtime", test_search_runtime_suite);
  RUN_SUITE("Fusion: SPLIT/ANY", test_fusion_suite);
  RUN_SUITE("Fusion Tier: concat-pattern", test_fusion_tier_suite);
  RUN_SUITE("Compact Choice Stack", test_compact_choice_suite);
  RUN_SUITE("VM Trail Choice Save", test_vm_trail_suite);
  RUN_SUITE("Choice-Stack Arena", test_choice_arena_suite);
  RUN_SUITE("Choice Short-Circuit", test_choice_shortcircuit_suite);
  RUN_SUITE("Search: Automaton", test_search_automaton_suite);
  RUN_SUITE("Search: SIMD NFA", test_search_simd_suite);
  RUN_SUITE("Search: Alt-literals & small-prefix",
            test_search_alt_literals_suite);
  RUN_SUITE("Automaton: BMH-skip", test_automaton_skip_suite);
  RUN_SUITE("Arena Allocator", test_arena_suite);
  /* Unicode / API */
  RUN_SUITE("Unicode Fold Table", test_unicode_fold_suite);
  RUN_SUITE("Pattern: Case-Insensitive", test_pattern_case_suite);
  RUN_SUITE("API Version", test_api_version_suite);
  RUN_SUITE("API: snobol_match()", test_api_match_suite);
  RUN_SUITE("API: snobol_pattern_match_literal()",
            test_api_literal_match_suite);
  RUN_SUITE("API: snobol_pattern_search_next()", test_api_search_next_suite);
  RUN_SUITE("Builder: snobol_pattern_build_compile()",
            test_builder_compile_suite);
  RUN_SUITE("API: snobol_match_reset() / search_reuse()",
            test_reusable_match_suite);
  RUN_SUITE("Reuse Search: _ex parity with search()", test_reuse_search_suite);
  RUN_SUITE("Grammar: BREAK / BREAKX", test_break_grammar_suite);
  RUN_SUITE("Property-Based Tests", test_property_based_suite);

  /* Source-vs-Builder parity */
  RUN_SUITE("Source-vs-Builder Parity", test_source_parity_suite);

  /* Differential search oracle */
  RUN_SUITE("Search Oracle: corpus equivalence", test_search_oracle_suite);
  RUN_SUITE("Search Oracle: meta invariants", test_search_oracle_meta_suite);
  RUN_SUITE("Search Oracle: generated patterns",
            test_search_oracle_generator_suite);

  /* Stress test */
  {
    int _p0 = test_ctx.passed;
    int _f0 = test_ctx.failed;
    struct timespec _t0;
    struct timespec _t1;
    printf("\n▸ Stress: Backtracking\n");
    SNOBOL_CLOCK_GETTIME(&_t0);
    int rc = 0;
    if (setjmp(test_jump) == 0) {
      rc = test_stress_backtracking_main();
    }
    SNOBOL_CLOCK_GETTIME(&_t1);
    if (rc == 0) {
      test_ctx.passed++;
    } else {
      test_ctx.failed++;
    }
    int _sp = test_ctx.passed - _p0;
    int _sf = test_ctx.failed - _f0;
    double _ms = elapsed_ms(_t0, _t1);
    if (_sf > 0) {
      printf("  ↳  %d passed  |  \033[31m%d failed\033[0m  |  %.2f ms\n", _sp,
             _sf, _ms);
    } else {
      printf("  ↳  %d passed  |  0 failed  |  %.2f ms\n", _sp, _ms);
    }
    if (suite_count < MAX_SUITES) {
      suite_results[suite_count++] =
          (SuiteResult){"Stress: Backtracking", _sp, _sf, _ms};
    }
  }


  printf("\n");
  int rule_w = summary_rule_w();
  int name_w = rule_w - 31;
  print_rule_w('=', rule_w);
  printf("  SNOBOL4 C Core Test Suite\n");
  print_rule_w('=', rule_w);
  printf("\n");
  print_rule_w('=', rule_w);
  printf("  %-*s  %7s  %7s  %9s\n", name_w, "Suite", "Passed", "Failed",
         "Time(ms)");
  print_rule_w('-', rule_w);

  double total_ms = 0.0;
  for (int i = 0; i < suite_count; i++) {
    SuiteResult *r = &suite_results[i];
    if (r->failed > 0) {
      printf("  \033[31m✗\033[0m %-*s  %7d  \033[31m%7d\033[0m  %9.2f\n",
             name_w - 2, r->name, r->passed, r->failed, r->time_ms);
    } else {
      printf("  ✓ %-*s  %7d  %7d  %9.2f\n", name_w - 2, r->name, r->passed,
             r->failed, r->time_ms);
    }
    total_ms += r->time_ms;
  }

  print_rule_w('-', rule_w);
  printf("  %-*s  %7d  %7d  %9.2f\n", name_w, "TOTAL", test_ctx.passed,
         test_ctx.failed, total_ms);
  print_rule_w('=', rule_w);

  if (test_ctx.failed == 0) {
    printf("  \033[32m✓  All %d cases / %d assertions passed\033[0m\n",
           test_ctx.case_count, test_ctx.passed);
  } else {
    printf("  \033[31m✗  %d of %d assertions FAILED (%d cases)\033[0m\n",
           test_ctx.failed, test_ctx.passed + test_ctx.failed,
           test_ctx.case_count);
  }

  print_rule_w('=', rule_w);

  return test_ctx.failed > 0 ? 1 : 0;
}

// NOTE: In DEBUG_BACKTRACK mode we intentionally allow stderr output so we can
// see VM state dumps when diagnosing backtracking.
