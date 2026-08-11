# Benchmark Suite — Interpretation Guide

## Running the benchmarks

### C microbenchmarks (native, no PHP needed)

```bash
make bench-c            # build (if needed) and run snobol4 vs PCRE2
```

This builds the C benchmark suite (`snobol4_bench`) and runs scenarios against
both libsnobol4 and PCRE2, printing a comparison table.

### PHP benchmarks (require the snobol extension loaded)

```bash
make bench              # run PHP benchmarks (tokenize, replace, dates, backtracking)
php bench/compare_pcre2.php   # snobol4 vs PCRE2 comparison via PHP API
```

## Diagnostic Probe

A standalone C tool (`bench/c/bench_probe.c`) that measures per-scenario
timing without modifying `core/`. Use it to attribute per-iteration cost
across the multi-tier search engine (Tiers 0-9).

### Building and running

```bash
cmake -B build -DBUILD_BENCH_C=ON
cmake --build build --target snobol4_probe
./build/bench/c/snobol4_probe
```

Override iteration count:

```bash
PROBE_ITERS=1000000 ./build/bench/c/snobol4_probe
```

### Scenarios

Each scenario reports a `unit` column (`match`|`pass`|`call`). Only rows with
matching name AND unit are comparable across the C and PHP probes. `pcre2_*`
rows are **unanchored-search context** — do not read them as head-to-head
comparisons against snobol anchored rows.

| Scenario                   | Pattern                        | Subject                    | API                                  | Unit  |
|----------------------------|--------------------------------|----------------------------|--------------------------------------|-------|
| `literal_fail`             | `'pqr'`                        | 2KB no `pqr`               | `snobol_pattern_match`               | match |
| `literal_ok`               | `'pqr'`                        | 2KB `pqr` at offset 0      | `snobol_pattern_match`               | match |
| `literal_late`             | `'pqr'`                        | 2KB `pqr` at offset 16    | `snobol_pattern_match`               | match |
| `span_comma`               | `SPAN(',')`                    | 1KB CSV                    | `snobol_pattern_match`               | match |
| `break_comma`              | `BREAK(',')`                   | 1KB CSV                    | `snobol_pattern_match`               | match |
| `alternation`              | `'a' \| 'b' \| 'c'`           | mixed                      | `snobol_pattern_match`               | match |
| `alt_literals`             | `'cat' \| 'dog' \| 'fox'`     | alt-lit stream             | `snobol_pattern_match`               | match |
| `automaton`                | `SPAN('abc') 'd'`              | 192-byte xy pattern        | `snobol_pattern_match`               | match |
| `alt_literals_search`      | `'cat' \| 'dog' \| 'fox'`     | alt-lit stream             | `snobol_pattern_search` (first)     | match |
| `alt_literals_search_all`  | `'cat' \| 'dog' \| 'fox'`     | alt-lit stream             | `snobol_pattern_search_batch_ex`     | pass  |
| `span_simd`                | `SPAN('0-9')`                  | 1KB digits                 | `snobol_pattern_search`              | match |
| `span_simd_miss`           | `SPAN('0-9')`                  | 1KB letters                | `snobol_pattern_search`              | match |
| `notany_simd_miss`         | `NOTANY('0')`                  | 1KB zeros                  | `snobol_pattern_search`              | match |
| `residue_repeat`           | `@r('a'*) 'b'`                 | 128 'a's, no 'b'           | `snobol_search_exec_anchored`        | match |
| `residue_repeat_all`       | `@r('a'*) 'b'`                 | 128 'a's, no 'b'           | `snobol_pattern_search_batch_ex`     | pass  |
| `residue_zero_width`       | `(''*) 'b'`                    | 128 'a's, no 'b'           | `snobol_search_exec_anchored`        | match |
| `residue_zero_width_all`   | `(''*) 'b'`                    | 128 'a's, no 'b'           | `snobol_pattern_search_batch_ex`     | pass  |
| `residue_catastrophic`     | `('a'+)+ 'b'`                  | 10 'a's, no 'b'            | `snobol_search_exec_anchored`        | match |
| `pike_overflow`            | `BREAKX(' ')`                  | 900 'x's + space           | `snobol_search_exec_anchored`        | match |
| `pike_overflow_all`        | `BREAKX(' ')`                  | 900 'x's + space           | `snobol_pattern_search_batch_ex`     | pass  |
| `prefilter_miss`           | `('a'+)+ 'b'`                  | 10 'a's, no 'b'            | `snobol_search_exec_anchored`        | match |
| `prefilter_miss_all`       | `('a'+)+ 'b'`                  | 10 'a's, no 'b'            | `snobol_pattern_search_batch_ex`     | pass  |
| `zero_progress`            | `('a'*) 'b'`                   | 64 'a's, no 'b'            | `snobol_search_exec_anchored`        | match |
| `zero_progress_all`        | `('a'*) 'b'`                   | 64 'a's, no 'b'            | `snobol_pattern_search_batch_ex`     | pass  |
| `tokenize_conv`            | `' '`                          | whitespace stream          | `snobol_pattern_match` loop         | pass  |
| `tokenize_reuse`           | `' '`                          | whitespace stream          | `snobol_pattern_search_ex` loop      | pass  |
| `tokenize_reuse_call`      | `' '`                          | whitespace stream          | `snobol_pattern_search` per call     | call  |
| `tokenize_fastpath`        | `' '`                          | whitespace stream          | `snobol_pattern_search_ex` (short)   | call  |
| `tokenize_next`            | `' '`                          | whitespace stream          | `snobol_pattern_search_next` loop    | call  |
| `tokenize_next_pass`       | `' '`                          | whitespace stream          | `snobol_pattern_search_next` pass    | pass  |
| `tokenize_memchr`          | `' '`                          | whitespace stream          | bare `memchr` loop                   | call  |

Scenarios ending in `_all` run each iteration through `snobol_pattern_search_batch_ex`
with a persistent state, so the DFA/range_meta caches are built once per scenario
(reused across iterations). This mirrors the PHP binding's persistent-state fix.

### Output columns

| Column         | Source                                    | Meaning                                |
|----------------|-------------------------------------------|----------------------------------------|
| `ns/iter`      | `clock_gettime` (or `mach_absolute_time`) | Wall time per unit of work             |
| `iters`        | iteration counter                         | Units of work executed                 |
| `unit`         | hardcoded per scenario                    | `match`=one match attempt, `pass`=one full search/split pass, `call`=one search call |
| `tier`         | `meta->tier`                              | Structural tier (pattern shape)        |
| `exec`         | `snobol_search_executed_tier`             | Executed dispatch tier (cost model)    |
| `sum`          | per-scenario work-consumed checksum       | Loop-escape guard: nonzero for `tokenize_next_pass`, keeps LTO from removing benchmark loops whose results are otherwise dead |

### Interpreting results

- **`jit_attempts > 0`, `jit_ok > 0`**: pattern was compiled once and cached;
  subsequent calls run native code.
- **`jit_fb > 0`**: pattern contains non-compilable opcodes (SPAN, BREAK,
  SPLIT, ASSIGN, etc.) — runs via VM interpreter. Each call wastes a failed
  compilation attempt; non-compilable patterns are not cached.
- **High `ns/iter` + `jit_attempts == 0`** (match API): interpreter path,
  no attempt is made (method JIT only triggers in search-mode or PHP match()).

## PHP probe

`bench/php/probe.php` mirrors the C probe's scenarios through the public
PHP API. Use it to attribute cost between the C engine and the PHP
binding layer (`memset(VM,0)`, `add_next_index_stringl`, PHP↔C crossing).

```bash
# In ddev:
ddev build-c-probe           # one-time: builds the C probe
php /var/www/bench/php/probe.php
```

The PHP probe emits a JSON block on stderr that the coupling test parses.

## C/PHP coupling test

`bindings/php/tests/php/CPhpCouplingTest.php` runs both probes and
asserts they move together. A regression guard: if an engine change
improves the C path but the PHP path stays the same, this test fails.

```bash
# In ddev:
ddev test --filter CPhpCouplingTest
```

The test compares ns-per-iteration with per-scenario PHP/C ratio bounds
(≈10× for the anchored residue rows — `residue_repeat`, `residue_zero_width`,
`prefilter_miss`, `zero_progress` — and 50× for `alt_literals`) so it
doesn't fail on legitimate architectural differences. The goal is to catch
the case where an optimization is implemented in the C engine but the PHP
binding doesn't see it.

## Baseline regression guard

Both probes support a `PROBE_BASELINE=1` mode that compares every scenario's
ns-per-iteration against a captured baseline and fails on rows more than 25%
slower (rows more than 10% faster are reported as speedups).

The baseline (`bench/results/search_perf_baseline.json`) is captured from
the canonical Release build — `SNOBOL_LTO=ON`, the CMake default that
`make build` keeps. No-LTO codegen is systematically slower, so a no-LTO
probe build skips the guard with guidance instead of producing mass fake
"regressions"; `PROBE_BASELINE_PATH` overrides and forces the comparison.

```bash
# Capture/refresh the baseline (after an optimization lands):
PROBE_ITERS=20000 ./build/bench/c/snobol4_probe
# ...assemble the after-baseline JSON, then:
bash bench/results/update_baseline.sh after_baseline.json

# Run the guard:
PROBE_BASELINE=1 PROBE_ITERS=20000 ./build/bench/c/snobol4_probe
```

Note: single-iteration rows (e.g. `pcre2_catastrophic`) and sub-100 ns rows
are noisy by construction; a borderline failure on those is usually machine
load (background indexing, Docker), not a real regression — re-run before
investigating.
