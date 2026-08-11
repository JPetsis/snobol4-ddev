/*
 * bench_probe.c — Diagnostic probe for libsnobol4 C API
 *
 * Exercises representative patterns in tight loops and reports a per-scenario
 * table of timings + JIT stats deltas. Used to attribute per-iteration cost
 * between the interpreter and JIT-compiled paths.
 *
 * Uses only the PUBLIC C API — no core/ modifications, no internal headers.
 *
 * Build: cmake -B build -DBUILD_BENCH_C=ON && cmake --build build --target snobol4_probe
 * Run:   ./build/bench/c/snobol4_probe
 * Tune:  PROBE_ITERS=1000000 ./build/bench/c/snobol4_probe
 */

#include "bench_shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/* Per-scenario result */
typedef struct {
    const char *name;       /* scenario id */
    const char *unit;       /* what one iteration measures: match|pass|call */
    int64_t iters;          /* iterations executed */
    int64_t total_ns;       /* wall time for the loop */
    int64_t ns_per_iter;    /* total_ns / iters */
    int    tier;           /* structural meta->tier (pattern shape) */
    int    exec_tier;      /* executed dispatch tier (may differ from tier) */
    int64_t checksum;      /* observable work-consumed checksum (loop escape) */
} probe_result_t;

/* Capture both the structural tier and the executed dispatch tier for a
 * pattern over a subject of the given length. The structural tier is the
 * shape-derived tier (meta->tier); the executed tier is what
 * dispatch_search_impl actually selects (cost model + DFA override). They
 * differ whenever the cost model promotes a pattern to a faster tier (e.g.
 * a structurally SEARCH_VM pattern executed as AUTOMATON). */
static void capture_tiers(snobol_pattern_t *pat, size_t subject_len,
                          probe_result_t *r) {
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    if (!meta) {
        r->tier = -1;
        r->exec_tier = -1;
        return;
    }
    r->tier = (int)meta->tier;
    bool dfa = snobol_pattern_automaton_available(pat);
    r->exec_tier = (int)snobol_search_executed_tier(meta, dfa,
                                                     subject_len, false);
}

/* Comma subject (matches bench_alternation.c) */
static const char *SUBJECT_CSV =
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status,"
    "id,name,email,age,status,id,name,email,age,status";

/* 1KB subject with 'pqr' at offset 16 */
static const char *SUBJECT_WITH_PQR =
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
    "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";

/* Subject that STARTS with 'pqr' (anchored literal_ok succeeds at offset 0).
 * Total length matches SUBJECT_WITH_PQR (2080 bytes) so timing is comparable. */
static char SUBJECT_PQR_AT_0[2081];
static void init_literal_subjects(void) {
    memset(SUBJECT_PQR_AT_0, 'z', 2080);
    SUBJECT_PQR_AT_0[0] = 'p';
    SUBJECT_PQR_AT_0[1] = 'q';
    SUBJECT_PQR_AT_0[2] = 'r';
    SUBJECT_PQR_AT_0[2080] = '\0';
}

/* 1KB subject with NO 'pqr' */
static const char *SUBJECT_NO_PQR =
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz"
    "abcdefghijklmnorstuvwxyzabcdefghijklmnorstuvwxyz";

/* Whitespace-separated subject for tokenize (mimics bench/tokenize.php) */
static const char *SUBJECT_WHITESPACE =
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z "
    "a b c d e f g h i j k l m n o p q r s t u v w x y z ";

/* Mixed subject for alternation (a/b/c interleaved with other chars) */
static const char *SUBJECT_MIXED =
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c "
    "the a quick b brown c fox a jumps b over c the a lazy b dog c ";

/* Multi-word alternation subject: "cat", "dog", "fox" interleaved.
 * ~90 bytes per row × 12 rows = ~1KB */
static const char *SUBJECT_ALTLIT =
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox "
    "the cat went dog walking fox jumped cat over dog near fox ";

/* Canonical automaton subject for pattern SPAN('abc') 'd': "xyzabcd" x 32
 * = 224 bytes.  The first three bytes "xyz" are not in the span class, then
 * "abcd" matches SPAN('abc') with literal 'd'.  MUST stay byte-identical to
 * $SUBJECT_AUTOMATON in bindings/php/probe.php. */
static const char *SUBJECT_AUTOMATON =
    "xyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcd"
    "xyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcd"
    "xyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcd"
    "xyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcdxyzabcd";

/* 1KB subject: all digits except last byte → SPAN('0-9') scans 1023 bytes */
static char SUBJECT_SIMD_SPAN[1024];
static void init_simd_subjects(void) {
    memset(SUBJECT_SIMD_SPAN, '0', 1023);
    SUBJECT_SIMD_SPAN[1023] = 'x';  /* non-digit terminator */
}

/* Compile a pattern; abort on failure. */
static snobol_pattern_t *compile_or_die(snobol_context_t *ctx,
                                         const char *src, size_t len) {
    char *err = NULL;
    snobol_pattern_t *pat = snobol_pattern_compile(ctx, src, len, &err);
    if (!pat) {
        fprintf(stderr, "compile failed: %s\n", err ? err : "(no detail)");
        free(err);
        abort();
    }
    free(err);
    return pat;
}

/* ---------------------------------------------------------------------------
 * Scenario runners
 *
 * Each runner takes (iterations, subject, subject_len) and writes to *r.
 * --------------------------------------------------------------------------- */

/* Convenience-path baseline for P1.7: same tokenize loop as run_tokenize but
 * using the one-shot snobol_pattern_search (re-derives state per call) instead
 * of the reusable search state.  This is the "before" number the reuse API is
 * measured against.  Reports ns per FULL PASS (one complete split of the
 * subject) so it shares the unit of the PHP tokenize rows. */
static void run_tokenize_convenience(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "' '", 3);
    size_t slen = strlen(SUBJECT_WHITESPACE);

    int64_t total_search_calls = 0;
    int64_t passes = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        size_t pos = 0;
        while (pos < slen) {
            /* Convenience API has no start offset: search the remaining suffix
             * of the subject. This is the "before" the reuse state avoids. */
            snobol_match_t *m =
                snobol_pattern_search(pat, SUBJECT_WHITESPACE + pos, slen - pos);
            total_search_calls++;
            if (!snobol_match_success(m)) {
                snobol_match_free(m);
                break;
            }
            pos += snobol_match_get_position(m) + snobol_match_get_length(m);
            snobol_match_free(m);
        }
        passes++;
    }
    int64_t end = bench_ns();

    r->iters = passes;
    r->total_ns = end - start;
    r->ns_per_iter = (passes > 0) ? (r->total_ns / passes) : 0;

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* Shared tokenize loop (mimics Pattern::searchSplit): loop
 * snobol_pattern_search_ex at advancing offsets, advance by match length.
 * Writes both units into *r depending on `per_pass`:
 *  - per_pass true  → ns per full split pass  (pairs with PHP tokenize rows)
 *  - per_pass false → ns per search call      (engine-level cost)
 */
static void run_tokenize_common(int64_t iters, probe_result_t *r,
                                bool per_pass) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "' '", 3);
    size_t slen = strlen(SUBJECT_WHITESPACE);

    /* Create search state for efficient repeated searches */
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state = snobol_pattern_search_state_create(bc, bc_len);

    int64_t total_search_calls = 0;
    int64_t passes = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        /* one full pass of the subject, splitting on ' ' */
        size_t pos = 0;
        while (pos <= slen) {
            snobol_match_t *m = snobol_pattern_search_ex(state, SUBJECT_WHITESPACE, slen, pos);
            total_search_calls++;
            if (!snobol_match_success(m)) {
                break;
            }
            /* advance past the match (single space → len 1) */
            pos = snobol_match_get_position(m) + snobol_match_get_length(m);
        }
        passes++;
    }
    int64_t end = bench_ns();

    int64_t units = per_pass ? passes : total_search_calls;
    r->iters = units;
    r->total_ns = end - start;
    r->ns_per_iter = (units > 0) ? (r->total_ns / units) : 0;

    capture_tiers(pat, slen, r);

    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* Primary tokenize row: ns per full pass (unit-compatible with PHP probe). */
static void run_tokenize(int64_t iters, probe_result_t *r) {
    run_tokenize_common(iters, r, true);
}

/* Secondary engine-level row: ns per individual search call. */
static void run_tokenize_call(int64_t iters, probe_result_t *r) {
    run_tokenize_common(iters, r, false);
}

/* Production API fast path: same tokenize loop as run_tokenize_common but
 * exercises the single-literal short-circuit in snobol_search_exec().
 * Reports as 'call' unit, matching tokenize_reuse_call and tokenize_memchr. */
static void run_tokenize_fastpath(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "' '", 3);
    size_t slen = strlen(SUBJECT_WHITESPACE);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    int64_t total_search_calls = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        size_t pos = 0;
        while (pos <= slen) {
            snobol_match_t *m = snobol_pattern_search_ex(
                state, SUBJECT_WHITESPACE, slen, pos);
            total_search_calls++;
            if (!snobol_match_success(m))
                break;
            pos = snobol_match_get_position(m) +
                  snobol_match_get_length(m);
        }
    }
    int64_t end = bench_ns();
    r->iters = total_search_calls;
    r->total_ns = end - start;
    r->ns_per_iter = (total_search_calls > 0)
                         ? (r->total_ns / total_search_calls)
                         : 0;
    capture_tiers(pat, slen, r);
    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* Lean tokenize API: same tokenize loop but uses snobol_pattern_search_next()
 * which avoids the snobol_pattern_search_ex entry/exit (~84 ns of match
 * struct, output buffer, and capture overhead).  Target: ~15 ns/call. */
static void run_tokenize_next(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "' '", 3);
    size_t slen = strlen(SUBJECT_WHITESPACE);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    int64_t total_search_calls = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        size_t pos = 0;
        size_t match_pos, match_len;
        while (snobol_pattern_search_next(state, SUBJECT_WHITESPACE,
                                          slen, pos, &match_pos, &match_len)) {
            total_search_calls++;
            pos = match_pos + match_len;
        }
    }
    int64_t end = bench_ns();
    r->iters = total_search_calls;
    r->total_ns = end - start;
    r->ns_per_iter = (total_search_calls > 0)
                         ? (r->total_ns / total_search_calls)
                         : 0;
    capture_tiers(pat, slen, r);
    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* Per-pass variant of tokenize_next: ns per full split pass using the lean
 * snobol_pattern_search_next() API.  Pairs with tokenize_reuse and
 * pcre2_tokenize for head-to-head comparison.
 *
 * The loop accumulates a checksum of the consumed positions that is printed
 * by print_table: the results are otherwise dead (nothing reads pos or
 * state afterwards), and Release builds default to SNOBOL_LTO=ON — LTO
 * provably eliminated the entire search loop, collapsing the row to
 * ~0 ns/iter.  The printed checksum keeps the loop observable. */
static void run_tokenize_next_pass(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "' '", 3);
    size_t slen = strlen(SUBJECT_WHITESPACE);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);
    snobol_pattern_search_state_t *state =
        snobol_pattern_search_state_create(bc, bc_len);
    int64_t passes = 0;
    uint64_t checksum = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && passes < (int64_t)1e9; i++) {
        size_t pos = 0;
        size_t match_pos, match_len;
        while (snobol_pattern_search_next(state, SUBJECT_WHITESPACE,
                                          slen, pos, &match_pos, &match_len)) {
            pos = match_pos + match_len;
            checksum += pos;
        }
        passes++;
    }
    int64_t end = bench_ns();
    r->iters = passes;
    r->total_ns = end - start;
    r->ns_per_iter = (passes > 0) ? (r->total_ns / passes) : 0;
    r->checksum = (int64_t)checksum;
    capture_tiers(pat, slen, r);
    snobol_pattern_search_state_destroy(state);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* Spike: bare memchr tokenize loop — no tier dispatch, no prefilter,
 * no search_evec.  Measures the irreducible cost of finding the next ' '
 * in the tokenize subject.  Gap vs tokenize_reuse_call (~160 ns) is the
 * snobol_search_exec dispatch overhead. */
static void run_tokenize_memchr(int64_t iters, probe_result_t *r) {
    size_t slen = strlen(SUBJECT_WHITESPACE);
    int64_t total_search_calls = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        size_t pos = 0;
        while (pos <= slen) {
            const char *found = (const char *)memchr(
                SUBJECT_WHITESPACE + pos, ' ', slen - pos);
            total_search_calls++;
            if (!found)
                break;
            pos = (found - SUBJECT_WHITESPACE) + 1;
        }
    }
    int64_t end = bench_ns();
    r->iters = total_search_calls;
    r->total_ns = end - start;
    r->ns_per_iter = (total_search_calls > 0)
                         ? (r->total_ns / total_search_calls)
                         : 0;
}

static void run_alt_literals(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    /* Multi-word alternation: hits Tier 3a (automaton/trie) */
    snobol_pattern_t *pat = compile_or_die(ctx, "'cat' | 'dog' | 'fox'", 20);
    size_t slen = strlen(SUBJECT_ALTLIT);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_match_t *m = snobol_pattern_match(pat, SUBJECT_ALTLIT, slen);
        snobol_match_free(m);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    capture_tiers(pat, slen, r);

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

static void run_alt_literals_search(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "'cat' | 'dog' | 'fox'", 20);
    size_t slen = strlen(SUBJECT_ALTLIT);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_match_t *m = snobol_pattern_search(pat, SUBJECT_ALTLIT, slen);
        snobol_match_free(m);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* ---------------------------------------------------------------------------
 * All-matches scenarios (*_all)
 *
 * Enumerate EVERY non-overlapping match per iteration via the batch API —
 * the same engine entry point Pattern::searchAll() uses for eligible
 * patterns.  One iteration == one full all-matches pass over the subject,
 * so these rows pair unit-for-unit with the PHP probe's searchAll rows.
 * --------------------------------------------------------------------------- */
static void run_search_all_scenario(const char *pattern_src, size_t pat_len,
                                    const char *subject, size_t subject_len,
                                    int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, pattern_src, pat_len);
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);
    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);

    /* Reuse one search state across iterations so the DFA/range_meta caches
     * are built once, mirroring the PHP binding's persistent-state fix. */
    snobol_pattern_search_state_t *st =
        snobol_pattern_search_state_create(bc, bc_len);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_batch_result_t batch;
        (void)snobol_pattern_search_batch_ex(st, subject, subject_len, &batch);
        snobol_batch_result_free(&batch);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    capture_tiers(pat, subject_len, r);

    snobol_pattern_search_state_destroy(st);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

static void run_alt_literals_search_all(int64_t iters, probe_result_t *r) {
    run_search_all_scenario("'cat' | 'dog' | 'fox'", 20, SUBJECT_ALTLIT,
                            strlen(SUBJECT_ALTLIT), iters, r);
}

/* ---------------------------------------------------------------------------
 * Anchored-match scenarios using snobol_search_exec_anchored() directly
 *
 * Mirrors PHP's Pattern::match(): no context create/destroy per call, no
 * search state — just a stack-allocated VM + tier dispatch.
 * --------------------------------------------------------------------------- */

/* Run anchored match with a minimal stack VM.  Compiles pattern once, then
 * in the loop sets s/len on a reused VM, calls snobol_vm_reset() between
 * iterations, and calls snobol_search_exec_anchored().  No context
 * create/destroy per iteration, no search state allocation. */
static void run_anchored_scenario(const char *pattern_src, size_t pat_len,
                                   const char *subject, size_t subject_len,
                                   int64_t iters, probe_result_t *r,
                                   bool capture_tier_info) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, pattern_src, pat_len);
    const snobol_search_meta_t *meta = snobol_pattern_get_meta(pat);

    const uint8_t *bc = snobol_pattern_get_bc(pat);
    size_t bc_len = snobol_pattern_get_bc_len(pat);

    size_t range_count = 0;
    const snobol_range_meta_t *range_meta =
        snobol_pattern_get_range_meta(pat, &range_count);

    VM vm;
    memset(&vm, 0, sizeof(vm));
    vm.bc = bc;
    vm.bc_len = bc_len;
    vm.range_meta = range_meta;
    vm.range_meta_count = range_count;

    snobol_search_result_t result;

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        vm.s = subject;
        vm.len = subject_len;
        snobol_vm_reset(&vm);

        memset(&result, 0, sizeof(result));

        (void)snobol_search_exec_anchored(&vm, subject, subject_len, meta, NULL,
                                           &result, NULL);
    }
    int64_t end = bench_ns();

    snobol_search_vm_cleanup(&vm);

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    if (capture_tier_info) {
        r->tier = (int)meta->tier;
        bool dfa = snobol_pattern_automaton_available(pat);
        r->exec_tier = (int)snobol_search_executed_tier(meta, dfa,
                                                         subject_len, true);
    }

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

static void run_literal_fail_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("'pqr'", 5, SUBJECT_NO_PQR,
                          strlen(SUBJECT_NO_PQR), iters, r, false);
}

/* Anchored success: literal at offset 0 — the match succeeds every iteration. */
static void run_literal_ok_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("'pqr'", 5, SUBJECT_PQR_AT_0,
                          strlen(SUBJECT_PQR_AT_0), iters, r, false);
}

/* Anchored reject: literal present only at offset 16 — anchored match fails
 * every iteration.  Measures the anchored-rejection cost when the literal
 * exists later in the subject. */
static void run_literal_late_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("'pqr'", 5, SUBJECT_WITH_PQR,
                          strlen(SUBJECT_WITH_PQR), iters, r, false);
}

static void run_span_comma_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("SPAN(',')", 9, SUBJECT_CSV,
                          strlen(SUBJECT_CSV), iters, r, true);
}

static void run_break_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("BREAK(',') ','", 14, SUBJECT_CSV,
                          strlen(SUBJECT_CSV), iters, r, true);
}

static void run_alternation_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("'a' | 'b' | 'c'", 15, SUBJECT_MIXED,
                          strlen(SUBJECT_MIXED), iters, r, true);
}

static void run_alt_literals_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("'cat' | 'dog' | 'fox'", 20, SUBJECT_ALTLIT,
                          strlen(SUBJECT_ALTLIT), iters, r, true);
}

static void run_automaton_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("SPAN('abc') 'd'", 15, SUBJECT_AUTOMATON,
                           strlen(SUBJECT_AUTOMATON), iters, r, true);
}

/* Fused concat anchored: SPAN('0-9') '-' SPAN('0-9') on matching subject.
 * The fusion tier (Tier 10) executes this as a flat segment list — no VM,
 * no bytecode dispatch, no choice stack. */
static void run_fused_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("SPAN('0-9') '-' SPAN('0-9')", 27, "123-456",
                          7, iters, r, true);
}

/* Fused concat anchored (fail): SPAN('0-9') '-' SPAN('0-9') on non-matching. */
static void run_fused_fail_anchored(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("SPAN('0-9') '-' SPAN('0-9')", 27, "abc-def",
                          7, iters, r, true);
}

/* ---------------------------------------------------------------------------
 * Full-VM residue scenarios (Tier 8) — exercise the W2a–W2d choice-stack
 * optimizations: REPEAT/EMIT-heavy backtracking and zero-width closures.
 * --------------------------------------------------------------------------- */

/* Subject for residue scenarios: a long run of one repeated byte so that
 * backtracking / repetition has room to iterate without being trivially short.
 */
static const char *SUBJECT_RESIDUE =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

/* REPEAT/EMIT-heavy residue: repetition over a span produces many choice
 * points on backtracking. The trail/arena choice stack keeps each push O(1).
 * The capture forces the full-VM (Tier 8) residue path so the choice stack is
 * the dominant cost.  Anchored first-match, pairing with PHP's match(). */
static void run_residue_repeat(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("@r('a'*) 'b'", strlen("@r('a'*) 'b'"),
                          SUBJECT_RESIDUE, strlen(SUBJECT_RESIDUE), iters, r,
                          true);
}

/* Zero-width closure: repetition over a nullable body (here the empty literal)
 * would otherwise push a choice point per iteration without consuming input,
 * blowing up exponentially. W2b bounds iterations to subject_len + 1, so the
 * match completes in bounded (linear) time.  Anchored, pairing with PHP's
 * match(). */
static void run_residue_zero_width(int64_t iters, probe_result_t *r) {
    run_anchored_scenario("(''*) 'b'", strlen("(''*) 'b'"),
                          SUBJECT_RESIDUE, strlen(SUBJECT_RESIDUE), iters, r,
                          true);
}

/* All-matches counterparts of the residue scenarios (pair with the PHP
 * probe's searchAll-based rows). */
static void run_residue_repeat_all(int64_t iters, probe_result_t *r) {
    run_search_all_scenario("@r('a'*) 'b'", strlen("@r('a'*) 'b'"),
                            SUBJECT_RESIDUE, strlen(SUBJECT_RESIDUE), iters, r);
}

static void run_residue_zero_width_all(int64_t iters, probe_result_t *r) {
    run_search_all_scenario("(''*) 'b'", strlen("(''*) 'b'"),
                            SUBJECT_RESIDUE, strlen(SUBJECT_RESIDUE), iters, r);
}

/* ---------------------------------------------------------------------------
 * SIMD-focused scenarios — long subjects where SIMD scanning matters
 * --------------------------------------------------------------------------- */

/* SPAN('0-9') on 1KB of digits: SIMD scans 1023 bytes (Tier 9 on NEON/AVX2) */
static void run_span_simd(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "SPAN('0-9')", 11);

    capture_tiers(pat, 1024, r);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_match_t *m = snobol_pattern_search(pat, SUBJECT_SIMD_SPAN, 1024);
        snobol_match_free(m);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* SPAN('0-9') on 1KB of letters: SIMD quickly determines no match */
static void run_span_simd_miss(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "SPAN('0-9')", 11);

    capture_tiers(pat, 1024, r);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_match_t *m = snobol_pattern_search(pat, SUBJECT_NO_PQR, 1024);
        snobol_match_free(m);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* NOTANY('0') on 1KB of '0': every byte is in the NOTANY excluded class,
 * so the pattern must fail at each position → O(n) missing match path. */
static void run_notany_simd_miss(int64_t iters, probe_result_t *r) {
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = compile_or_die(ctx, "NOTANY('0')", 11);

    capture_tiers(pat, 1024, r);

    /* All-digit 1KB subject */
    char subj[1025];
    memset(subj, '0', 1024);
    subj[1024] = '\0';

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        snobol_match_t *m = snobol_pattern_search(pat, subj, 1024);
        snobol_match_free(m);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}

/* ---------------------------------------------------------------------------
 * PCRE2 comparison scenarios (only when PCRE2 is available)
 * --------------------------------------------------------------------------- */

#ifdef HAVE_PCRE2

/* Helper: compile a PCRE2 pattern; abort on failure. */
static pcre2_code *pcre2_compile_or_die(const char *pattern, uint32_t options) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                    options, &errcode, &erroffset, NULL);
    if (!re) {
        fprintf(stderr, "PCRE2 compile failed for '%s'\n", pattern);
        abort();
    }
    return re;
}

static void run_pcre2_literal_fail(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("pqr", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_NO_PQR);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_NO_PQR, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_pcre2_literal_ok(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("pqr", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_WITH_PQR);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_WITH_PQR, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_pcre2_span_comma(int64_t iters, probe_result_t *r) {
    /* SPAN(',') matches one or more consecutive commas → PCRE2: ,+ */
    pcre2_code *re = pcre2_compile_or_die(",+", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_CSV);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_CSV, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_pcre2_alternation(int64_t iters, probe_result_t *r) {
    /* Single-char alt: 'a' | 'b' | 'c' → PCRE2: a|b|c or [abc] */
    pcre2_code *re = pcre2_compile_or_die("[abc]", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_MIXED);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_MIXED, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_pcre2_alt_literals(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("cat|dog|fox", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_ALTLIT);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_ALTLIT, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

/* PCRE2 tokenize: ns per FULL PASS (same unit as tokenize_reuse). */
static void run_pcre2_tokenize(int64_t iters, probe_result_t *r) {
    /* Splitting on space → PCRE2: \x20 (literal space) */
    pcre2_code *re = pcre2_compile_or_die("\\x20", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_WHITESPACE);

    int64_t total_search_calls = 0;
    int64_t passes = 0;
    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters && total_search_calls < (int64_t)1e9; i++) {
        size_t pos = 0;
        while (pos <= slen) {
            int rc = pcre2_match(re, (PCRE2_SPTR)SUBJECT_WHITESPACE, slen,
                                  pos, 0, md, NULL);
            total_search_calls++;
            if (rc < 0) break;
            /* Advance past the match */
            PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(md);
            pos = ovector[1];  /* end of match */
        }
        passes++;
    }
    int64_t end = bench_ns();

    r->iters = passes;
    r->total_ns = end - start;
    r->ns_per_iter = (passes > 0) ? (r->total_ns / passes) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

/* Catastrophic-backtracking contrast (W2b): PCRE2 (a+)+ over a short run of
 * 'a's with no trailing 'b' is the canonical exponential case. SNOBOL's
 * bounded repeat keeps the equivalent pattern linear. Uses a deliberately
 * short subject (20 'a's) and a single iteration so the PCRE2 side finishes
 * in a measurable (not astronomical) time. */
static const char *SUBJECT_CAT =
    "aaaaaaaaaa";

static void run_pcre2_catastrophic(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("(a+)+b", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
    size_t slen = strlen(SUBJECT_CAT);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_CAT, slen, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_residue_catastrophic(int64_t iters, probe_result_t *r) {
    /* Equivalent SNOBOL pattern to PCRE2 (a+)+b: one-or-more 'a', repeated,
     * then 'b'. 'b' is absent, so it fails — but bounded, not exponentially.
     * Uses SUBJECT_CAT (10 'a's) matching the PCRE2 catastrophic test.
     * Anchored, pairing with PHP's match(). */
    run_anchored_scenario("('a'+)+ 'b'", strlen("('a'+)+ 'b'"), SUBJECT_CAT,
                          strlen(SUBJECT_CAT), iters, r, true);
}

static void run_pcre2_span_simd(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("[0-9]+", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_SIMD_SPAN, 1024, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

static void run_pcre2_span_simd_miss(int64_t iters, probe_result_t *r) {
    pcre2_code *re = pcre2_compile_or_die("[0-9]+", 0);
    pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);

    int64_t start = bench_ns();
    for (int64_t i = 0; i < iters; i++) {
        pcre2_match(re, (PCRE2_SPTR)SUBJECT_NO_PQR, 1024, 0, 0, md, NULL);
    }
    int64_t end = bench_ns();

    r->iters = iters;
    r->total_ns = end - start;
    r->ns_per_iter = (iters > 0) ? (r->total_ns / iters) : 0;

    pcre2_match_data_free(md);
    pcre2_code_free(re);
}

#endif /* HAVE_PCRE2 */

/* ---------------------------------------------------------------------------
 * 9.x  New search-perf-levers scenarios
 * --------------------------------------------------------------------------- */

/* pike_overflow: BREAKX(' ') over 1KB subject with delimiter at position 900.
 * Forces pike_scan thread-buffer overflow + restart-loop fallback.  Anchored,
 * pairing with PHP's match(). */
static void run_pike_overflow(int64_t iters, probe_result_t *r) {
    char subj[1025];
    memset(subj, 'x', 900);
    subj[900] = ' ';
    subj[901] = '\0';
    run_anchored_scenario("BREAKX(' ')", 11, subj, 901, iters, r, true);
}

/* pike_overflow_all: same pattern/subject, all-matches per iteration. */
static void run_pike_overflow_all(int64_t iters, probe_result_t *r) {
    char subj[1025];
    memset(subj, 'x', 900);
    subj[900] = ' ';
    subj[901] = '\0';
    run_search_all_scenario("BREAKX(' ')", 11, subj, 901, iters, r);
}

/* prefilter_miss: ('a'+)+ 'b' on 10 'a's — required-byte prefilter memchr
 * rejects the subject without entering any tier.  Anchored, pairing with
 * PHP's match(). */
static void run_prefilter_miss(int64_t iters, probe_result_t *r) {
    char subj[11];
    memset(subj, 'a', 10);
    subj[10] = '\0';
    run_anchored_scenario("('a'+)+ 'b'", 12, subj, 10, iters, r, true);
}

/* prefilter_miss_all: same pattern/subject, all-matches per iteration. */
static void run_prefilter_miss_all(int64_t iters, probe_result_t *r) {
    char subj[11];
    memset(subj, 'a', 10);
    subj[10] = '\0';
    run_search_all_scenario("('a'+)+ 'b'", 12, subj, 10, iters, r);
}

/* zero_progress: ('a'*) 'b' on 64-byte subject of 'a's — zero-progress guard
 * should make the loop O(1) instead of O(n).  Anchored, pairing with PHP's
 * match(). */
static void run_zero_progress(int64_t iters, probe_result_t *r) {
    char subj[65];
    memset(subj, 'a', 64);
    subj[64] = '\0';
    run_anchored_scenario("('a'*) 'b'", 10, subj, 64, iters, r, true);
}

/* zero_progress_all: same pattern/subject, all-matches per iteration. */
static void run_zero_progress_all(int64_t iters, probe_result_t *r) {
    char subj[65];
    memset(subj, 'a', 64);
    subj[64] = '\0';
    run_search_all_scenario("('a'*) 'b'", 10, subj, 64, iters, r);
}

/* ---------------------------------------------------------------------------
 * Output
 * --------------------------------------------------------------------------- */

static void print_header(void) {
    printf("\n");
    printf("libsnobol4 diagnostic probe — per-scenario timing\n");
    printf("=================================================\n");
#ifdef HAVE_PCRE2
    printf("PCRE2 comparison: ENABLED (pcre2_* scenarios)\n");
#else
    printf("PCRE2 comparison: DISABLED (install pcre2 to enable)\n");
#endif
    printf("\n");
}

static void print_table(const probe_result_t *results, size_t n) {
    printf("%-24s %10s %8s %-5s %4s %4s %12s\n",
            "scenario", "ns/iter", "iters", "unit", "tier", "exec", "sum");
    printf("%-24s %10s %8s %-5s %4s %4s %12s\n",
            "-------", "-------", "-----", "----", "----", "----", "---");

    for (size_t i = 0; i < n; i++) {
        const probe_result_t *r = &results[i];
        printf("%-24s %10" PRId64 " %8" PRId64 " %-5s %4d %4d %12" PRId64 "\n",
                r->name,
                r->ns_per_iter,
                r->iters,
                r->unit ? r->unit : "-",
                r->tier,
                r->exec_tier,
                r->checksum);
    }
    printf("\n");
    printf("Legend:\n");
    printf("  ns/iter  : wall time per unit of work (lower = faster)\n");
    printf("  iters    : units of work executed in the scenario\n");
    printf("  unit     : what one iteration measures —\n");
    printf("             match = one match attempt (first match / anchored)\n");
    printf("             pass  = one full all-matches or split pass of the subject\n");
    printf("             call  = one individual search call inside a pass\n");
    printf("  tier     : structural tier (meta->tier, pattern shape)\n");
    printf("  exec     : executed dispatch tier (cost model + DFA override)\n");
    printf("  sum      : observable work-consumed checksum (loop escape guard —\n");
    printf("             nonzero for tokenize_next_pass; keeps LTO from removing\n");
    printf("             benchmark loops whose results are otherwise dead)\n");
    printf("\n");
    printf("Only rows with the same unit are comparable across the C and PHP\n");
    printf("probes.  pcre2_* rows are UNANCHORED-SEARCH CONTEXT: PCRE2's\n");
    printf("pcre2_match() scans the subject, while the snobol literal_* and\n");
    printf("anchored rows are anchored — do not read pcre2_* vs anchored rows\n");
    printf("as a head-to-head comparison.\n");
    printf("\n");
}

/* ---------------------------------------------------------------------------
 * Baseline regression guard
 *
 * Reads bench/results/search_perf_baseline.json and asserts each
 * scenario's ns_per_iter is within 10% of the baseline. Exits non-zero
 * on regression.
 * --------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Minimal JSON parser for our specific schema — finds a scenario
 * key in the c_probe object and extracts its ns_per_iter value. We
 * parse brace-delimited scopes to avoid matching ns_per_iter in
 * nested objects (e.g. c_api_extensions.comparison_to_before). */
typedef struct {
    char name[64];
    long long ns_per_iter;
    int  found;
} baseline_row_t;

static int parse_baseline_row(const char *json, const char *scenario, baseline_row_t *out) {
    /* The c_probe object is the authoritative source. Find it first. */
    const char *cprobe = strstr(json, "\"c_probe\"");
    if (!cprobe) return 0;
    const char *obj_start = strchr(cprobe, '{');
    if (!obj_start) return 0;
    /* Walk forward to find the matching '}' for the c_probe object. */
    int depth = 1;
    const char *q = obj_start + 1;
    while (*q && depth > 0) {
        if (*q == '{') depth++;
        else if (*q == '}') depth--;
        if (depth == 0) break;
        q++;
    }
    if (depth != 0) return 0;
    /* Now search for "<scenario>": { within c_probe only. */
    char key[128];
    snprintf(key, sizeof(key), "\"%s\"", scenario);
    const char *p = strstr(obj_start, key);
    if (!p || p > q) return 0;
    /* Find the scenario's object start. */
    const char *s_start = strchr(p, '{');
    if (!s_start || s_start > q) return 0;
    int sd = 1;
    const char *s_end = s_start + 1;
    while (s_end < q && sd > 0) {
        if (*s_end == '{') sd++;
        else if (*s_end == '}') sd--;
        if (sd == 0) break;
        s_end++;
    }
    if (sd != 0) return 0;
    /* Now find ns_per_iter within the scenario's brace-balanced object. */
    const char *nsp = strstr(s_start, "\"ns_per_iter\"");
    if (!nsp || nsp > s_end) return 0;
    const char *colon = strchr(nsp, ':');
    if (!colon) return 0;
    long long v = strtoll(colon + 1, NULL, 10);
    if (v <= 0) return 0;
    strncpy(out->name, scenario, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    out->ns_per_iter = v;
    out->found = 1;
    return 1;
}

static int read_file(const char *path, char **out) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = (char *)malloc((size_t)sz + 1);
    if (!*out) { fclose(f); return 0; }
    fread(*out, 1, (size_t)sz, f);
    (*out)[sz] = '\0';
    fclose(f);
    return 1;
}

static int assert_against_baseline(const probe_result_t *results, size_t n) {
    /* Try a few likely paths */
    const char *env_path = getenv("PROBE_BASELINE_PATH");
    const char *paths[5] = {0};
    int npaths = 0;
    if (env_path) paths[npaths++] = env_path;
    paths[npaths++] = "bench/results/search_perf_baseline.json";
    paths[npaths++] = "../bench/results/search_perf_baseline.json";
    paths[npaths++] = "../../bench/results/search_perf_baseline.json";
    paths[npaths] = NULL;
    char *json = NULL;
    int found_path = -1;
    for (int i = 0; paths[i]; i++) {
        if (read_file(paths[i], &json)) {
            found_path = i;
            break;
        }
    }
    if (!json) {
        fprintf(stderr, "PROBE_BASELINE=1 but no baseline file found\n");
        return 2;
    }
    printf("\n=== Baseline regression guard (PROBE_BASELINE=1) ===\n");
    printf("Baseline file: %s\n", paths[found_path]);
    printf("%-16s %12s %12s %12s\n",
           "scenario", "baseline", "observed", "delta%");
    printf("%-16s %12s %12s %12s\n",
           "-------", "--------", "--------", "------");

    int regressions = 0;
    int speedups = 0;
    for (size_t i = 0; i < n; i++) {
        baseline_row_t row;
        if (!parse_baseline_row(json, results[i].name, &row)) {
            /* No baseline entry — skip (not a regression) */
            continue;
        }
        long long base = row.ns_per_iter;
        long long obs  = results[i].ns_per_iter;
        double delta_pct = (base > 0) ? ((double)(obs - base) / base * 100.0) : 0.0;
        printf("%-16s %12lld %12lld %+11.1f%%",
               results[i].name, base, obs, delta_pct);
        if (delta_pct > 25.0) {
            printf("  REGRESSION\n");
            regressions++;
        } else if (delta_pct < -10.0) {
            printf("  speedup\n");
            speedups++;
        } else {
            printf("  ok\n");
        }
    }
    free(json);
    printf("\n%d regressions, %d speedups, %zu scenarios checked\n",
           regressions, speedups, n);
    if (regressions > 0) {
        printf("FAILED: %d scenarios regressed by more than 25%%\n", regressions);
        return 1;
    }
    printf("OK: no regressions exceeding 25%% threshold\n");
    return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * --------------------------------------------------------------------------- */

int main(void) {
    int64_t iters = 100000;
    const char *env_iters = getenv("PROBE_ITERS");
    if (env_iters && *env_iters) {
        long long v = atoll(env_iters);
        if (v > 0) iters = v;
    }
    /* Tokenize needs more outer iterations to make the timing meaningful;
     * scale them down by 10x so the probe stays fast. */
    int64_t tokenize_iters = iters / 10;
    if (tokenize_iters < 1) tokenize_iters = 1;

    init_simd_subjects();
    init_literal_subjects();

    print_header();
    printf("Iterations per scenario: %" PRId64 " (override with PROBE_ITERS)\n",
           iters);
    printf("Tokenize uses %" PRId64 " outer iters (one full pass each).\n\n",
           tokenize_iters);

    /* Total scenarios: 34 snobol + 9 PCRE2 (when available) = 43 */
    probe_result_t results[43];
    memset(results, 0, sizeof(results));

    /* Run each scenario */
    struct {
        const char *name;
        void (*run)(int64_t, probe_result_t *);
        int64_t iter_count;
        const char *unit;  /* match | pass | call */
    } scenarios[] = {
        /* Anchored scenarios (stack VM, no search state) */
        { "literal_fail",        run_literal_fail_anchored,        iters, "match" },
        { "literal_ok",          run_literal_ok_anchored,          iters, "match" },
        { "literal_late",        run_literal_late_anchored,        iters, "match" },
        { "span_comma",          run_span_comma_anchored,          iters, "match" },
        { "break_comma",         run_break_anchored,               iters, "match" },
        { "alternation",         run_alternation_anchored,         iters, "match" },
        { "alt_literals",        run_alt_literals_anchored,        iters, "match" },
        { "automaton",           run_automaton_anchored,           iters, "match" },
        /* Fused concat (Tier 10) */
        { "fused_match",         run_fused_anchored,               iters, "match" },
        { "fused_fail",          run_fused_fail_anchored,           iters, "match" },
        /* Convenience / search scenarios (first match per iteration) */
        { "alt_literals_conv",   run_alt_literals,                iters, "match" },
        { "alt_literals_search", run_alt_literals_search,         iters, "match" },
        { "span_simd",           run_span_simd,                   iters, "match" },
        { "span_simd_miss",      run_span_simd_miss,              iters, "match" },
        { "notany_simd_miss",    run_notany_simd_miss,            iters, "match" },
        /* All-matches scenarios (batch API; pair with PHP searchAll rows) */
        { "alt_literals_search_all", run_alt_literals_search_all, iters, "pass" },
        { "residue_repeat_all",      run_residue_repeat_all,      iters, "pass" },
        { "residue_zero_width_all",  run_residue_zero_width_all,  iters, "pass" },
        { "pike_overflow_all",       run_pike_overflow_all,       iters, "pass" },
        { "prefilter_miss_all",      run_prefilter_miss_all,      iters, "pass" },
        { "zero_progress_all",       run_zero_progress_all,       iters, "pass" },
        /* Tokenize: primary rows are per full pass; _call is per search call */
        { "tokenize_conv",        run_tokenize_convenience,       tokenize_iters, "pass" },
        { "tokenize_reuse",        run_tokenize,                   tokenize_iters, "pass" },
        { "tokenize_reuse_call",   run_tokenize_call,              tokenize_iters, "call" },
        { "tokenize_fastpath",     run_tokenize_fastpath,          tokenize_iters, "call" },
        { "tokenize_next",         run_tokenize_next,              tokenize_iters, "call" },
        { "tokenize_next_pass",    run_tokenize_next_pass,         tokenize_iters, "pass" },
        { "tokenize_memchr",       run_tokenize_memchr,            tokenize_iters, "call" },
        { "residue_repeat",        run_residue_repeat,             iters,  "match" },
        { "residue_zero_width",    run_residue_zero_width,         iters,  "match" },
        { "residue_catastrophic",  run_residue_catastrophic,       1000,   "match" },
        { "pike_overflow",         run_pike_overflow,              iters,  "match" },
        { "prefilter_miss",        run_prefilter_miss,             iters,  "match" },
        { "zero_progress",         run_zero_progress,              iters,  "match" },
#ifdef HAVE_PCRE2
        /* PCRE2 rows: UNANCHORED-SEARCH CONTEXT (see legend) */
        { "pcre2_literal_fail",  run_pcre2_literal_fail,  iters,           "match" },
        { "pcre2_literal_ok",    run_pcre2_literal_ok,    iters,           "match" },
        { "pcre2_span_comma",    run_pcre2_span_comma,    iters,           "match" },
        { "pcre2_alternation",   run_pcre2_alternation,   iters,           "match" },
        { "pcre2_alt_literals",  run_pcre2_alt_literals,  iters,           "match" },
        { "pcre2_span_simd",     run_pcre2_span_simd,     iters,           "match" },
        { "pcre2_span_simd_miss",run_pcre2_span_simd_miss, iters,          "match" },
        { "pcre2_tokenize",      run_pcre2_tokenize,      tokenize_iters,  "pass" },
        { "pcre2_catastrophic",  run_pcre2_catastrophic,  1,               "match" },
#endif
    };
    size_t n = sizeof(scenarios) / sizeof(scenarios[0]);

    for (size_t i = 0; i < n; i++) {
        results[i].name = scenarios[i].name;
        results[i].unit = scenarios[i].unit;
        scenarios[i].run(scenarios[i].iter_count, &results[i]);
    }

    print_table(results, n);

    /* Priority 4.3: dump the cost-model coefficients used by
     * select_tier_by_cost() and suggest recalibrations from the measured
     * short-subject timings. Keep per_byte_div (throughput) fixed; only
     * setup_ns is candidate for tuning. */
    snobol_search_dump_cost_model(stdout);
    printf("\nRecalibration suggestion (short-subject ns/iter -> candidate setup_ns):\n");
    for (size_t i = 0; i < n; i++) {
        int64_t ns = results[i].ns_per_iter;
        if (ns <= 0)
            continue;
        const char *tier = NULL;
        if (strcmp(results[i].name, "alt_literals") == 0 ||
            strcmp(results[i].name, "alt_literals_conv") == 0)
            tier = "ALT_LIT";
        else if (strcmp(results[i].name, "literal_ok") == 0 ||
                 strcmp(results[i].name, "literal_fail") == 0)
            tier = "LITERAL";
        else if (strcmp(results[i].name, "span_comma") == 0 ||
                 strcmp(results[i].name, "break_comma") == 0)
            tier = "SPAN_SCAN";
        else if (strcmp(results[i].name, "alternation") == 0)
            tier = "PREFIX/ALT_LIT";
        if (tier)
            printf("  %-16s measured=%" PRId64 " ns/iter -> suggest setup_ns ~= %d\n",
                   results[i].name, ns, (int)ns);
    }
    printf("(Apply by editing k_tier_cost in core/src/search_meta.c; the authoritative\n"
           " table is printed above. Keep per_byte_div for per-byte throughput.)\n");

    /* Optional baseline regression guard. If a baseline file exists
     * at bench/results/search_perf_baseline.json and PROBE_BASELINE=1,
     * assert each scenario's ns_per_iter is within 10% of the baseline. */
    if (getenv("PROBE_BASELINE") && atoi(getenv("PROBE_BASELINE")) == 1) {
        return assert_against_baseline(results, n);
    }

    return 0;
}
