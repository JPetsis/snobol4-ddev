# Changelog

All notable changes to the libsnobol4 project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.3] - 2026-08-10

### Fixed

- **Required-byte prefilter rejected subjects matching non-final
  alternation branches** (`core/src/search_meta.c`, `derive_meta`): the
  linear scan treated the last `OP_LIT` in bytecode order as required
  unless a `SPLIT` seen after it provably bypassed it. For left-nested
  alternation chains (`'a'|'b'|'c'`) and loop bodies (`('a'|'b')*`) every
  `SPLIT` precedes the literals, so the last branch's literal was marked
  required and `snobol_pattern_search` returned false negatives for
  subjects matching any other branch. Any `SPLIT` encountered before any
  literal now sets `lit_bypassed` (no required literal). Regression tests:
  `test_prefilter_leading_alternation` and `test_alt_literals_large_chain`.

## [1.0.2] - 2026-08-06

### Fixed

- **`Pattern::searchReplace()` corrupted the heap on trailing zero-length
  matches** (`bindings/php/src/snobol_pattern.c`): a zero-length match at
  end-of-subject (BREAK-family patterns without the delimiter, EVAL patterns,
  empty patterns) advanced `last_match_end` past `subject_len`; the final
  remainder append then underflowed the length and `memcpy`'d past the
  `snobol_buf` allocation (heap corruption / double-free / SIGABRT, verified
  under valgrind). `last_match_end` is now clamped to `subject_len` in both
  the batch fast path and the per-call loop. Found by the coverage-driven
  PHP binding test suite.
- **EVAL callbacks were ignored on the first `Pattern::match()` call**
  (`core/src/api.c`): the lazy search-state VM init `memset`s `state->vm`
  on first use, wiping the `eval_fn`/`eval_udata` wired by
  `snobol_pattern_search_state_set_eval_fn()` before the search. The
  callback pointer is now preserved across the init memset in
  `snobol_pattern_search_ex()`, `snobol_pattern_search_ex_anchored()` and
  `batch_run()`. Found by the coverage-driven EVAL callback tests.
- **Parser leaked the label-name string on labelled patterns** (`core/src/parser.c`,
  `parse_statement`): `snobol_ast_create_label` copies the name and returns
  ownership to the caller, but the parser passed its malloc'd `label_name` to
  the node constructor without freeing it afterwards — every labelled pattern
  leaked the name. Found by the coverage-driven label tests under the leak
  sanitizer / valgrind.
- **`snobol_pattern_build_label` leaked the name copy** (`core/src/api.c`): the
  builder allocated a name copy and handed it to `snobol_ast_create_label`,
  which duplicates the name itself — the builder's copy leaked on every call.
- **Successful full-VM matches leaked the undo trail and write-log**
  (`core/src/search_meta.c`): `snobol_search_vm_cleanup` freed the pike buffers
  and choice arena but not the VM trail / write-log lazily allocated by `vm_run`
  on success paths — every successful tier-8 match leaked ~8 KB plus the
  write-log. Cleanup now frees both.
- **Batch searches with captures leaked the row-capacity array**
  (`core/src/api.c`, `batch_run`): `row_caps` was freed only on failure paths,
  never on success.
- **Inline metadata derivation in dispatch leaked bmh_skip/fusion**
  (`core/src/search_tiers.c`): NULL-meta callers of `snobol_search_exec`
  derived into a stack meta whose heap (BMH skip table, fusion segments) was
  never freed.

- **Batch-search capture rows overflow past 64 matches** (`core/src/api.c`,
  `batch_run`): capture rows were allocated once with the initial result-array
  capacity (64 matches) and never reallocated — the row-realloc check tested
  `count >= cap` after the result arrays had already doubled `cap`, so the
  condition was always false. Batch searches with captures and more than 64
  matches wrote past the 1024-byte row allocation (heap-buffer-overflow,
  verified under ASan). Rows now track their own capacity (`row_caps[]`),
  double in lockstep with the result arrays, and zero the new tail. Found by
  the coverage-driven API test suite.
- **Pike BREAKX retry threads decoded an operand byte as an opcode**
  (`core/src/search_tiers.c`, `pike_scan`): the retry thread resumed at
  `ip - 2`, which points at the first operand byte after the u16 set-id read
  (the opcode sits at `ip - 3`); the operand byte (frequently 0x00 =
  OP_ACCEPT) silently ended the thread, so BREAKX patterns whose match
  requires the delimiter-consuming retry branch reported a bogus short
  match. The retry thread now resumes at the OP_BREAKX opcode, matching the
  search-VM's `svm_breakx`.
- **Pike dropped all `@r` capture registers on the default search path**
  (`core/src/search_tiers.c`, `pike_scan`): the ACCEPT writeback copied
  `cap_start`/`cap_end` but not `var_start`/`var_end`/`var_count`, so every
  unanchored capture search handled by the Pike fast path reported no named
  variables. The writeback now propagates the registers (relative to the
  match start, unifying pike with the restart loop and full VM).
- **Capture offsets were window-relative in the pattern-object APIs**
  (`core/src/api.c`): `snobol_pattern_match`, `snobol_pattern_search` and
  `snobol_pattern_search_reuse` materialized captures against the full
  subject with match-window-relative offsets, so matches away from offset 0
  returned wrong bytes ("aax " with a capture of "x " at offset 2 yielded
  "aa"). `_ex` and batch used the search offset as the window base, wrong
  when earlier candidates failed. All capture readers now anchor the subject
  at the match position; the PHP binding's `searchAll` capture-offset base
  uses the match position too.
- **Anchored search matched anywhere through the DFA automaton**
  (`core/src/search_tiers.c`): the TIER_AUTOMATON override ignored the
  anchored flag, and `search_automaton_exec` is an unanchored restart
  scanner by design — `snobol_search_exec_anchored` returned matches that
  did not start at the anchor for automaton-eligible patterns (e.g.
  `'ab' SPAN('0-9')` on "xab12" matched at offset 1), causing
  `Pattern::match()` false positives. The override is gated on `!anchored`
  and anchored calls take a single-pass DFA walk from the anchor.
- **Pike skipped POS/RPOS/TAB/RTAB as no-ops** (`core/src/search_tiers.c`,
  `pike_scan`): mid-pattern position constraints were never enforced — e.g.
  `'ab' POS(5) 'c'` on "abc" incorrectly matched. Position ops are now
  validated with full-VM semantics (codepoint-walked targets; TAB/RTAB move
  the cursor, POS/RPOS fail unless the cursor is exactly at the target).
- **`snobol_search_derive_meta` hung or read OOB on malformed bytecode**
  (`core/src/search_meta.c`): `compute_minlength` followed JMP targets with
  no cycle guard, so cyclic bytecode (e.g. `JMP(0)`) hung the derivation;
  the zero-width prefix skip advanced past truncated prefix opcodes
  (`{POS,0}` with `bc_len=2`) and read `bc[ip]` out of bounds. A step cap
  mirrors `compute_start_bitmap`'s and the prefix skip bounds-checks every
  advance, bailing to unclassified metadata on overrun.
- **Search-VM ANY never matched without pre-resolved ranges**
  (`core/src/search_tiers.c`, `svm_any`): ANY read only the `srange` cache
  while SPAN/BREAK/BREAKX/NOTANY resolved their charclass from the
  bytecode trailer; ANY now uses the same `search_vm_resolve_range`
  fallback.
- **Capture undo was lost across OP_DYNAMIC sub-matches**
  (`core/src/vm_exec.c`, `op_dynamic`): the nested `vm_run()` saw the outer
  undo trail (non-NULL, so it cleared instead of allocating) and its ACCEPT
  freed it — the outer run continued with a dropped trail. The trail is now
  detached before the inner run and restored afterwards, mirroring the
  choice-stack/write-log handling.
- **GOTO/GOTO_F treated a label at bytecode offset 0 as missing**
  (`core/src/vm_exec.c`): `vm_get_label_offset` returned 0 both for a
  registered label at offset 0 and for an unknown label id; it now returns
  a `UINT32_MAX` sentinel for unknown labels.
- **Documented `$TABLE[key]` template syntax compiled to a literal `$`**
  (`core/src/compiler.c`): only the bracketed `$v0[TABLE[key]]` form was
  recognized; the PHP manual's documented `$TABLE[key]` / `$TABLE[$vM]`
  forms (docs/php-manual.md) now compile to OP_EMIT_TABLE (bare identifier
  followed by `[`), capture keys accept the documented `$v0` spelling, and
  overlong (>255 byte) table names fail compilation loudly, surfacing the
  `subst()` "Failed to compile template" exception.
- **Removed vestigial PHP binding code** (`bindings/php/src/`):
  `php_snobol_init_vm_for_search`, `php_snobol_emit_cb` + `EmitBuf`, the
  never-assigned `intern->dfa` field, `php_snobol_get_all_tables` /
  `php_snobol_get_all_arrays`, and `php_phelper_use_cache` had no callers
  after the persistent-search-state migration and were removed.

### Notes

- **BREAK is deterministic and greedy by design** (divergence from classic
  SNOBOL4, where BREAK may retry shorter matches on backtracking): use
  `BREAKX` for retry semantics.
- **RTAB(n) clamps to the subject start instead of failing** when n exceeds
  the remaining subject length (fails only when the cursor is past the
  target).

## [1.0.1] - 2026-08-02

### Changed

- **Version bumped to 1.0.1** across the project (top-level `CMakeLists.txt`,
  `core/CMakeLists.txt`, `bindings/php/src/php_snobol.h`, PHP/C API version
  tests, READMEs).

### Fixed

- **PIE release asset naming matched exactly** (`.github/workflows/release.yml`):
  the published asset name pattern now matches what PIE expects, and
  **PHP 8.5 builds** were added to the release matrix.
- **`snobol/version.h` generated at configure time when missing**
  (`bindings/php/config.m4`): the PHP binding's build no longer requires a
  pre-existing generated header, fixing out-of-tree configure runs.

## [1.0.0] - 2026-08-02

First Packagist/PIE release of the 1.x series. The engine is functionally
the 0.13.0 codebase; this release is the distribution/CI hardening that made
standalone packaging viable.

### Added

- **CMake guards for examples/bench subdirectories** (`CMakeLists.txt`):
  distributed source tarballs configure cleanly when the examples and
  benchmark targets are not built.
- **LTO behind an option** (`core/CMakeLists.txt`): `SNOBOL_LTO` defaults
  to OFF for standalone builds, keeping distributed builds toolchain-agnostic.

### Changed

- **Version bumped to 1.0.0** (`CMakeLists.txt`, `core/CMakeLists.txt`):
  the 0.13.0 engine is released as the first 1.x version; the PHP binding
  API version test encoding was updated to match.

### Fixed

- **Windows CI builds the PHP extension via a direct dev-pack build**
  (`.github/workflows/`): the php-windows-builder action was replaced with
  the native Windows dev-pack flow, and the Windows `arch` input now uses
  `x64` (was `x86_64`).
- **macos arm64 jobs moved to `macos-15` runners** (`.github/workflows/`):
  the arm64 matrix entries previously pinned `macos-14`.
- **PIE builder upgraded to `pie-ext-binary-builder@0.0.3`** with a
  `build-path` input so the extension binary lands at the expected path.

## [0.13.0] - 2026-07-28

### Pattern Fusion (Tier 10)

#### Added

- **`TIER_FUSED_AUTOMATON` (Tier 10)** (`core/include/snobol/search.h`, `core/src/search_fusion.c`): Compile-time pattern fusion for concat chains of compatible ops (LIT/SPAN/ANY/NOTANY/BREAK). The fusion pass recognizes fusible patterns during `snobol_search_derive_meta()` and compiles them into a flat segment list. The `exec_fusion()` engine walks the segment list directly — no VM, no bytecode dispatch, no choice stack. Expected 2-5× speedup for date/phone/key-value patterns vs VM execution.
- **`snobol_fusion_segment_t` and `snobol_fusion_t`** (`core/include/snobol/search.h`): Fusion segment types (FUSION_LIT, FUSION_RUN, FUSION_CHAR, FUSION_ALT) and compiled fusion struct. Segment lists are heap-allocated and freed via `snobol_fusion_free()`.
- **Fusion recognition in `check_fusion_eligible()`** (`core/src/search_meta.c`): Walks bytecode, identifies fusible concat patterns, builds 256-bit bitmap segment lists. Gates on `has_capture` (fusion doesn't support captures), `>32 segments` (complexity cap), and non-fusible ops (EVAL, DYNAMIC, etc.).
- **`tier_fusion()` dispatch** (`core/src/search_fusion.c`): Anchored path runs `exec_fusion()` once at offset 0. Unanchored path iterates positions verifying with `exec_fusion()`. Wired into `tier_table[TIER_FUSED_AUTOMATON]` in `search_tiers.c`.
- **Fusion cost model entry** (`core/src/search_meta.c`): `setup_ns=50, per_byte_div=8` in `k_tier_cost[]`. Added to `select_tier_by_cost()` eligibility switch.
- **C test suite** (`tests/c/test_fusion_tier.c`): 7 test functions covering tier assignment, non-fusible patterns, anchored/unanchored execution, various segment types, and alternation patterns.

#### Changed

- **`snobol_search_meta_t`** (`core/include/snobol/search.h`): Added `fusion_eligible` bool and `fusion` pointer fields. Added `META_FUSION_ELIGIBLE` flag and `snobol_meta_fusion_eligible()` accessor macro.
- **`snobol_search_derive_meta()`** (`core/src/search_meta.c`): Calls `check_fusion_eligible()` after SIMD eligibility check. Sets `tier = TIER_FUSED_AUTOMATON` when fusion is eligible. Gates on `has_capture` (fusion doesn't support captures).
- **`snobol_search_meta_free()`** (`core/src/search_meta.c`): Frees fusion struct via `snobol_fusion_free()`.
- **`snobol_search_executed_tier()`** (`core/src/search_tiers.c`): Reports `TIER_FUSED_AUTOMATON` when fusion is eligible.

### Lean Tokenize API

#### Added

- **`snobol_pattern_search_next()`** (`core/src/api.c`,
  `core/include/snobol/snobol.h`): Lightweight unanchored single-literal
  search returning position+length via out-parameters. Skips the match
  struct, capture arrays, and output buffer — ~8 ns/call instead of
  ~88 ns through `snobol_pattern_search_ex`. Returns false for
  non-literal patterns; caller falls back. Supports single-byte (memchr)
  and multi-byte (memmem) literals.
- **C test suite** (`tests/c/test_api_search_next.c`): 5 scenarios:
  single-byte advancing, multi-byte literal, non-literal fallback,
  NULL guards, start_offset past end.
- **`Snobol\SplitIterator`** (`bindings/php/src/snobol_split_iterator_php.c`):
  Lazy split iterator implementing `Iterator`. Calls
  `snobol_pattern_search_next()` incrementally — one segment per
  `next()` call. Early break after N segments pays zero cost for
  remaining delimiters. Accessible via `Pattern::searchSplitGenerator()`.

#### Changed

- **`php_snobol_searchsplit_record_offsets()`** (`bindings/php/src/`):
  Per-call fallback for `searchSplit`, `searchSplitOffsets`,
  `searchSplitCuts` now uses `snobol_pattern_search_next()` when the
  delimiter is a literal-only pattern (~8 ns/call vs ~88 ns).
- **`run_tokenize_next` probe scenario** (`bench/c/bench_probe.c`):
  C-side probe row exercising `snobol_pattern_search_next()` through the
  production API. Reports ~8 ns/call.

### PHP Match Routing and Per-Call Optimization

#### Added

- **`snobol_pattern_search_ex_anchored()`** (`core/src/api.c`, `core/include/snobol/snobol.h`): Stateful anchored search entry point that reuses the persistent VM, DFA, range_meta, and output buffer across calls. Match must start at offset 0 (SNOBOL-style anchored semantics). Intended for `Pattern::match()`.
- **`php_snobol_do_match()`** (`bindings/php/src/snobol_pattern.c`, `bindings/php/src/php_snobol.h`): Routes `Pattern::match()` through the search-tier dispatch path (the same pipeline used by `searchAll`/`searchSplit`), adding the required-byte prefilter, search-VM accelerators, and automaton/Tier 7 offload to the PHP first-match path.
- **`snobol_pattern_search_state_set_eval_fn()`** (`core/src/api.c`, `core/include/snobol/snobol.h`): Stores the EVAL callback and userdata on the state's persistent VM, avoiding per-call callback allocation.
- **`eval_callbacks` cache on PHP `snobol_pattern_t`** (`bindings/php/src/php_snobol.h`, `bindings/php/src/snobol_pattern.c`): `setEvalCallbacks()` now stores the callbacks array on the pattern; the callback function pointer is passed to the persistent VM via the state setter, eliminating per-call allocation.

#### Changed

- **`Pattern::match()` now uses persistent search state** (`bindings/php/src/snobol_pattern.c`): The match method no longer creates a stack VM, EmitBuf, dyn_cache, or table binding per call. Instead it calls `php_snobol_do_match()` which reuses `intern->search_state` and the tier dispatch. Simple patterns (span, break, alternation, automaton) improved from ~100–1,000× PHP/C ratio to 1.6–3.2×. The existing `memcmp` literal fast path is preserved.
- **`setEvalCallbacks()` stores the callbacks array** (`bindings/php/src/snobol_pattern.c`): Previously a no-op; now saves the array on the pattern struct for use by the persistent VM's eval callback dispatcher.

### Probe Truth and Performance Fairness

#### Added

- **`snobol_pattern_search_batch_ex(state, subject, len, out)`** (`core/src/api.c`, `core/include/snobol/snobol.h`): Stateful batch search that reuses the state's VM/range_meta/DFA/trie/SIMD-NFA caches across calls. Avoids per-call metadata rebuild. The stateless `snobol_pattern_search_batch()` delegates to a temporary state internally.
- **`bool eligible` field on `snobol_batch_result_t`** (`core/include/snobol/snobol.h`): Distinguishes eligible zero-match (eligible==true, DONE, no fallback) from ineligible (eligible==false, callers fall back to per-call loop).
- **Persistent search state on PHP `snobol_pattern_t`** (`bindings/php/src/php_snobol.h`, `bindings/php/src/snobol_pattern.c`): `snobol_pattern_search_state_t *search_state` is lazily created on first search call, reused by all search methods (`searchAll`, `searchSplit`, `searchSplitOffsets`, `searchSplitCuts`, `searchReplace`), destroyed in the PHP dtor. Both the batch fast path and the per-call fallback use the same persistent state.
- **Aligned C/PHP probe scenarios** (`bench/c/bench_probe.c`, `bindings/php/probe.php`): All-matches scenarios (`*_all`, unit=pass), per-pass tokenize rows, canonicalized subjects, `unit` column in probe output tables.

#### Changed

- **Anchored literal search is O(literal length) instead of O(subject)** (`core/src/search_tiers.c`): `search_literal_only()` now uses `memcmp` at `start_offset` when anchored, eliminating the whole-subject `memmem` scan. `literal_fail` collapsed from 5.6 µs to 52 ns.
- **PHP binding uses `snobol_pattern_search_batch_ex` with persistent state** (`bindings/php/src/snobol_pattern.c`): The batch fast path and fallback loop share one persistent `snobol_pattern_search_state_t` per PHP `Pattern` object. DFA, range_meta, and trie caches are built once per pattern lifetime. Heavy zero-match `searchAll` rows collapsed ~105 µs → 208 ns.
- **Flat result modes go through the batch fast path** (`bindings/php/src/snobol_pattern.c`): `result=>'flat'` and `result=>'offsets'` now route through the same stateful batch path as the default mode, with identical result shapes.
- **C probe `_all` scenarios use stateful batch_ex with persistent state** (`bench/c/bench_probe.c`): Each `*_all` scenario creates one `snobol_pattern_search_state_t` reused across all iterations, so the DFA-reuse fix is measurable in the C probe.

### PHP Binding Overhead Optimizations

#### Added

- **Opt-in `_metrics` hash** (`bindings/php/src/snobol_pattern.c`): `Pattern::match()` and `searchAll()` no longer build the `_metrics` hash by default. Pass `$options['metrics' => true]` to include it. Saves ~50 ns per call.
- **Capture-as-offsets mode** (`bindings/php/src/snobol_pattern.c`): `match()`, `searchAll()`, `searchSplit()` accept `$options['captures' => 'offsets']` to return capture values as `[start, length]` integer pairs instead of substring copies. Zero string allocation for callers that only need positions.
- **Flat result mode for searchAll** (`bindings/php/src/snobol_pattern.c`): `$options['result' => 'flat']` returns parallel arrays (`match_start`, `match_len`, `captures`) instead of array-of-arrays. Eliminates per-match zval array overhead.
- **Flat offset mode for searchSplit** (`bindings/php/src/snobol_pattern.c`): `$options['result' => 'flat']` on `searchSplit()`/`searchSplitOffsets()` returns alternating `[start, len, start, len, ...]` flat arrays, replacing per-segment sub-arrays.
- **`Pattern::searchSplitCuts()`** (`bindings/php/src/snobol_pattern.c`): Returns flat cut-point offset array — the cheapest possible split result. Zero string allocation, zero sub-array allocation.
- **`Pattern::searchAllGenerator()`** (`bindings/php/src/snobol_pattern.c`, `bindings/php/src/snobol_search_iterator_php.c`): Lazy iteration over matches via `Snobol\SearchIterator` (implements PHP `Iterator`). First match in ~1 µs. Callers that break early pay zero for remaining matches.
- **`$options` parameter on all search methods** (`bindings/php/src/snobol_pattern.c`): `searchAll()`, `searchSplit()`, `searchSplitOffsets()`, `searchReplace()` now accept `array $options = []` for uniform control of `metrics`, `captures`, and `result`.
- **Pre-sized output buffer in searchReplace** (`bindings/php/src/snobol_pattern.c`): For subjects > 1 KB, `searchReplace()` runs a counting pass to estimate output size and pre-allocate the buffer, avoiding reallocation during long replacement loops.
- **JIT configuration for DDEV** (`bindings/php/.ddev/php/snobol-jit.ini`): Enables `opcache.jit = tracing` for benchmark accuracy.
- **DFA caching in `Pattern::match()`** (`bindings/php/src/snobol_pattern.c`, `core/src/search_meta.c`): Builds and caches a DFA on the `Pattern` object for automaton-eligible patterns. Enables Tier 7 (AUTOMATON) O(n) single-pass dispatch on repeated `match()` calls.
- **Trie caching in search methods** (`bindings/php/src/snobol_pattern.c`, `core/src/search_tiers.c`, `core/src/api.c`): Pre-builds the alt-literals trie on the `Pattern` object and passes it to the search state via `vm->trie_cache`, avoiding per-call trie rebuild for `searchAll()`, `searchSplit()`, `searchSplitOffsets()`, and `searchReplace()`.
- **`snobol_build_alt_trie()` public API** (`core/include/snobol/search.h`, `core/src/search_tiers.c`): New function to build an alt-literals trie from SPLIT/LIT bytecode, usable by any host binding.

#### Changed

- **Literal fast-path restored** (`bindings/php/src/snobol_pattern.c`): The direct-`memcmp` literal fast-path in `match()` was restored (previously removed as "redundant" in P4). The C-core Tier 2 dispatcher is correct but adds ~300 ns due to `search_reset_vm` + prefilter overhead. The direct path returns in ~20 ns.
- **`Pattern::match()` signature** (`bindings/php/src/snobol_pattern.c`): Accepts optional `array $options = []` parameter.
- **All probe metrics under 500 ns** (`bench/BENCHMARKS.md`), verified with `opcache.jit` enabled.

#### Fixed

- **Literal fast-path bypass** (`bindings/php/src/snobol_pattern.c`): Literal fast-path now respects `$options['metrics' => true]` and emits the `_metrics` hash when requested.

### search-perf-levers

#### Added

- **Required-byte pre-filter** (`core/src/search_meta.c`, `core/src/search_tiers.c`): `snobol_search_derive_meta` identifies the rightmost literal before ACCEPT/SUCCEED along the bytecode and stores it in `meta->required_lit`/`required_lit_len`. `dispatch_search_impl` runs `memchr`/`memmem` before any tier — if the required literal is absent, returns false with `out_result->prefilter_skip = true`, bypassing all VM/tier dispatch. No-op for patterns without required literals (e.g. alternations, pure charclass ops).
- **Diagnostic probe scenarios** (`bench/c/bench_probe.c`): new probe rows for `pike_overflow` (BREAKX over long subject), `prefilter_miss` (required-byte memchr miss), and `zero_progress` (empty-body loop guard).

#### Changed

- **Automaton BMH-skip gate** (`core/src/search_tiers.c`): promotion to TIER_AUTOMATON now requires `meta->has_bmh_skip`. Patterns with only 1-byte literals (no BMH skip) stay on TIER_SEARCH_VM, avoiding the O(n²) per-position trial loop. Mirrored in `snobol_search_executed_tier` for diagnostic consistency.
- **Pike buffer hoist** (`core/src/search_tiers.c`, `core/src/api.c`): thread buffers moved from stack-per-call to state-level heap (`VM.pike_thread_buf`/`pike_defer_buf`). Allocated once on first use in pike_scan, freed in `snobol_pattern_search_state_destroy`. Stack fallback for stateless callers (vm==NULL).
- **SIMD NFA cache** (`core/src/api.c`, `core/src/search_simd.c`): `simd_nfa_t` cached on `snobol_pattern_search_state`, built once on first `tier_simd_nfa` access, freed in state destroy. Mirrors the DFA caching pattern.
- **SIMD vector compare** (`core/src/search_simd.c`): `simd_nfa_exec_neon` and `simd_nfa_exec_avx2` now implement real SIMD vector compare using 256-byte membership tables, replacing the scalar stubs. Tail bytes fall through to the scalar reference path.
- **Zero-progress guard order** (`core/src/vm_exec.c`): in OP_REPEAT_STEP, `pos == loop_last_pos` is checked before `count > subject_len + 1`, so empty-body loops (e.g. `(''*)`) exit in O(1) instead of O(subject_len). The search-VM REPEAT_STEP handler already had the correct ordering.

#### Fixed

- **Pike overflow correctness** (`core/src/search_tiers.c`): pike_scan now tracks thread-buffer overflow at all guard points (`work_n`, `carry_n`, `defer_n`). When overflow is detected, `tier_search_vm` falls through to the per-position restart loop (which has a proper choice stack), eliminating silent false negatives for BREAKX patterns over long subjects.

### Core Batch-Search API

#### Added

- **`snobol_batch_result_t` struct** (`core/include/snobol/snobol.h`): Result struct with flat arrays for match positions, lengths, per-register capture offset pairs, and concatenated output strings.
- **`snobol_pattern_search_batch()`** (`core/include/snobol/snobol.h`, `core/src/api.c`): Single-pass batch search that calls `snobol_search_exec()` directly in a loop, collecting all results into growable flat arrays. Returns false for non-search-VM-eligible patterns (EVAL, ASSIGN, DYNAMIC), enabling transparent per-call fallback.
- **`snobol_batch_result_free()`** (`core/src/api.c`): Releases all arrays owned by a batch result struct.
- **Batch-search integration in PHP binding** (`bindings/php/src/snobol_pattern.c`): `searchAll()`, `searchSplit()`, `searchSplitOffsets()`, `searchSplitCuts()`, and `searchReplace()` try the batch API first; fall back to per-call loop for ineligible patterns. Eligible patterns complete in a single C pass with no per-match API overhead.
- **C test suite** (`tests/c/test_search_batch.c`): 49 assertions verifying batch results match per-call loop results for literal, SPAN, BREAK, alternation, zero-length, no-match, and EVAL patterns.
- **PHP test suite** (`bindings/php/tests/php/BatchSearchTest.php`): 17 tests covering searchAll, searchSplit, searchReplace, captures, and edge cases on both eligible and ineligible patterns.

### Search Engine Optimization

#### Added

- **PIKE_SCAN default ON** (`core/CMakeLists.txt`, `core/src/search_tiers.c`): Pike/TDFA single-pass scan replaces the per-offset restart loop for capture-and-loop patterns at Tier 6. Enabled by default (`ENABLE_PIKE_SCAN=ON`). Falls back to the restart loop for anchored matches or non-zero start offsets.
- **SIMD NFA tier (Tier 9) live** (`core/src/search_meta.c`, `core/src/search_simd.c`): `select_tier_by_cost` now includes `case TIER_SIMD_NFA`, routing simd-eligible charclass patterns (SPAN/BREAK/ANY/NOTANY, ASCII-only) through the SIMD Thompson NFA. `tier_simd_nfa` performs an O(n) bitmap-skip scan using the start-state 256-bit char_mask (positions that cannot start a match are skipped with one bit-test) and O(1) scalar NFA verification at candidate positions — replacing the O(n²) per-position restart loop.
- **Greedy-star bound choice** (`core/src/vm_exec.c`): unbounded `*`/ARBNO over a pure OP_SPAN body commits to the maximal run and pushes a single bound choice, turning O(n) per-byte choice pushes into O(1) forward. Each backtrack re-executes the step instruction to try one byte shorter. Captured/side-effect bodies keep the per-step path unchanged.

#### Changed

- **Flat alt-literals route to trie** (`core/src/search_meta.c`): the `else if (out->is_alt_literals)` branch at `derive_meta` now assigns `TIER_ALT_LIT` instead of `TIER_GENERAL`, so `'cat'|'dog'|'fox'` runs through the trie (~200 ns) instead of the full VM (~1170 ns).
- **SIMD NFA cost coefficient** (`core/src/search_meta.c`): `k_tier_cost` gains `{TIER_SIMD_NFA, 15, 16}`, calibrated to win the cost race for simd-eligible charclass patterns over the dedicated SPAN/BREAK bitmap scanners and the general VM.
- **ANY/NOTANY ascii_class_only** (`core/src/search_meta.c`): `derive_meta` now records `ascii_class_only` for OP_ANY and OP_NOTANY, gating SIMD NFA and automaton tiers to ASCII-only charclasses (non-ASCII codepoint classes route through the full VM for correct multi-byte UTF-8 semantics).

#### Fixed

- **BREAK acceptance in SIMD NFA scalar exec** (`core/src/search_simd.c`): `simd_nfa_exec_scalar` now handles BREAK patterns (state 1 = accept was unreachable, so BREAK never matched). Added `is_break` end-of-loop special case matching the existing `is_span` pattern.
- **SIMD all-class-run tail case** (`core/src/search_simd.c`): when a SIMD window consumed the entire subject without finding a non-class byte, the scalar tail returned no match (offset == start). Fixed by detecting full-subject consumption and returning the full-run match before reaching the tail.

#### Performance

- SIMD-eligible patterns (`NOTANY`, `SPAN`) now execute at Tier 9 (SIMD NFA) instead of the general VM (Tier 8) — `notany` drops from ~198 ns to ~560 ns (miss) or 600 ns (hit on 1KB subject). The miss time reflects the O(n) bitmap-skip scan (one bit-test per byte × 1024 = ~500 ns).
- Residue catastrophic backtracking (`residue_catastrophic`): **-17%** from the greedy-star bound choice (fewer per-byte forward pushes in the repeated span run).
- Alt-literals: flat `'cat'|'dog'|'fox'` routes to the trie (Tier 5) at ~210 ns/iter vs ~1170 ns on the full VM — a **5.5× improvement** from the L1 flat→trie routing fix.
- C test suite: **67350/67350** assertions, **246/246** cases pass.
- PHP test suite: **319/319** tests, **621** assertions pass.

- **Trie-shape classifier** (`core/src/search.c`): `trie_is_flat()` routes flat alternations (no shared prefix) to Tier 8 (general VM) instead of the unaccelerated Tier 5 trie, eliminating the 125× regression on flat alt-of-literals.
- **Tier 5 scan-loop acceleration** (`core/src/search.c`): start-byte bitmap filter, BMH-style skip, and bounded loop (`offset + minlength <= subject_len`) now applied to the alt-literals scan loop (previously a bare `offset++`).
- **Trie caching** (`core/src/api.c`): pointer-based cached trie on `snobol_pattern_t`; bushy alt-literals reuse the compiled trie across searches (flat patterns skip caching).
- **Arena allocator** (`core/include/snobol/arena.h`): bump-allocated pool for AST nodes during compilation, eliminating per-node `malloc`.
- **Cost-based tier selection** (`core/src/search.c`): `select_tier_by_cost()` replaces hardcoded structural thresholds; ALT_LIT setup recalibrated 40→12 ns to reflect the cached trie.
- **2-byte memchr prefix fast-path** (`core/src/search.c`): paired `memchr` for `prefix_len == 2` in `search_literal_accelerated()` (avoids `memmem` setup overhead on short needles).
- **Compiler hints & codegen** (`core/src/search.c`, `core/src/vm.c`): `SNOBOL_HOT`/`COLD`/`ALWAYS_INLINE`/`PURE`/`RESTRICT`/`ALIGNED(64)` + `likely()` branch hints; `bitmap256_test()` always-inlined helper. Release build gains `-O3`, `-fvisibility=hidden`, `_FORTIFY_SOURCE=3`, and project-wide LTO.
- **PGO build targets**: `make build-pgo-gen` / `pgo-train` / `build-pgo-use` plus `pgo-gen` / `pgo-use` CMake presets (`-fprofile-generate` / `-fprofile-use -fprofile-correction`).
- **Cost-model diagnostics** (`core/src/search.c`, `bench/c/bench_probe.c`): `snobol_search_dump_cost_model()` prints authoritative coefficients; probe emits per-scenario recalibration suggestions.
- **Capture-aware Tier-6 search-VM** (`core/src/search.c`): `OP_CAP_START`/`OP_CAP_END`, bounded `REPEAT_INIT`/`REPEAT_STEP`, and positional ops (`POS`/`TAB`/`RPOS`/`RTAB`/`REM`/`ANCHOR`/`FENCE`) are now executed by `search_vm_exec`. Recognizing-and-capturing patterns (e.g. `lit("id:") + cap(span("0-9"))`) leave the full VM (Tier 8) and run on Tier 6, which is materially faster than the general-VM fallback.
- **Anchored dispatch entry** (`core/src/search.c`, `bindings/php/src/snobol_pattern.c`): added `snobol_search_exec_anchored()` and routed `Pattern::match()` through the tier dispatcher (runs the selected tier once at offset 0) instead of `vm_exec()` directly. Anchored matches no longer pay the per-offset full-VM restart cost.
- **C `snobol_pattern_match` routed through the dispatcher** (`core/src/api.c`): anchored matching now uses `snobol_search_exec_anchored()` (cost-model tier selection, DFA reuse) instead of `vm_run()` directly — closing the gap with `Pattern::match()` and the search path.
- **Stronger start-byte bitmap / BMH eligibility** (`core/src/search.c`): `derive_meta` walks past zero-width prefixes (ANCHOR, POS, NOP, literal prefix) and derives skip-ahead from the first consuming opcode.
- **`BREAK` / `BREAKX` grammar wiring** (`core/src/parser.c`, `bindings/php/php-src/PatternHelper.php`): `BREAK(set)` and `BREAKX(set)` now parse like `SPAN` (accept a literal or char-class argument, expect `RPAREN`) and compile to `OP_BREAK` / `OP_BREAKX`, dispatching to the accelerated `TIER_BREAK_SCAN` (Tier 0). Added `tests/c/test_break_grammar.c` and `bindings/php/tests/php/BreakTest.php` (BREAK/BREAKX compile and match the leading field; `BREAK()` with no argument is rejected). The PHP probe gained a `break_comma` scenario row.

#### Changed

- **`bench_alternation.c`**: rewritten with real bushy (Tier 5) + flat (Tier 8) alt-literal scenarios — it was misnamed and previously only benchmarked `SPAN(',')`.
- **`bench_delimiter.c`**: now records the PCRE2 timing (`out->pcre2_ns` was computed but never stored, showing `0/0`).

#### Fixed

- **Tier 5 worst case**: flat alternations no longer grind through the unaccelerated trie; worst case is now bounded by Tier 8 (general VM with bitmap + BMH + minlength pre-checks).
- **Search-VM choice-stack overflow** (`core/src/search.c`): `search_vm_exec` allocated the choice stack at 256 B even though each `search_choice_t` is ~2 KiB (embeds full capture/var register arrays). The realloc now grows-to-fit, eliminating a silent heap-corruption bug that surfaced as `malloc(): unaligned tcache chunk detected` during PHP test runs.
- **Bounded-repetition semantics in the search-VM** (`core/src/search.c`): `REPEAT_INIT`/`REPEAT_STEP` diverged from the full VM — `REPEAT_INIT` always pushed a 0-iteration skip (yielding zero-length "matches" for `min > 0`) and `REPEAT_STEP` pushed the continue-loop branch as the backtrack choice instead of the exit (so a satisfied minimum with no further match failed instead of accepting). Rewritten to mirror `vm.c:1300`.
- **Capture patterns silently dropped in search mode** (`core/src/search.c`): `derive_meta` skipped `OP_CAP_START`/`OP_CAP_END` as zero-width prefixes, so a captured pattern (e.g. `@r(span("0-9"))`) was misclassified as `is_span_family` and routed to the **non-capturing** Tier 1 span-scan, which discarded the capture. A new `has_capture` gate in `snobol_search_meta_t` clears the span/break/literal/alt/automaton/simd accelerators for capturing patterns so they route to the capture-aware Tier 6 (or Tier 8). Added `test_capture_span_search_mode` regression test.
- **`bc_has_capture` over-ran trailing class data** (`core/src/search.c`): the capture scan walked past `OP_ACCEPT`/`OP_SUCCEED`/`OP_ABORT` into the appended charclass/label tail and misread tail bytes as capture opcodes (e.g. breaking `is_span_family` for plain `SPAN` patterns). The walk now stops at program terminators.
- **Search-VM SPAN/BREAK matched nothing without `range_meta`** (`core/src/search.c`): `svm_span`/`svm_break`/`svm_breakx` resolved charclasses solely from `vm->range_meta` and had no fallback, so any caller that did not populate `range_meta` (notably the capture unit tests, and defensive safety) produced a silent no-match. They now fall back to the bytecode-embedded ranges via `get_ranges_ptr()`, mirroring the full VM.
- **PHP whole-file segfault (exit 139)** (`core/src/api.c`): `snobol_pattern_search_ex` cached the DFA on `state->pattern`, but the PHP-side `snobol_pattern_t` lacks the `automaton`/`trie_cache` fields that the core `struct snobol_pattern` carries. `snobol_pattern_get_automaton` then read uninitialized bytes at the wrong offset, yielding a garbage `dfa` pointer that crashed `search_automaton_exec`. Fixed by caching the DFA on the search *state* (`state->dfa`) instead of the pattern; freed in `snobol_pattern_search_state_destroy`. Both core and PHP patterns work via their respective accessors. (Root-caused and fixed as part of `search-perf-measured-wins`; verified by whole-file `ConvenienceApiTest.php` / `PatternTest.php` now passing, full PHP suite green.)
- **Zero-length match never reported** (`core/src/search_tiers.c`): `''` (empty literal) on e.g. `"abc"` returned 2 segments instead of 5. `build_dfa` treated `OP_LIT` with `len == 0` as contributing no transition and no epsilon edge, so the NFA start set for `''` never reached `OP_ACCEPT` and the start DFA state was not marked accepting; `search_automaton_exec` (which only inspects states reached *after* consuming input) never reported the empty match at interior positions. Fixed in two parts: `epsilon_closure` now treats `OP_LIT` with `len == 0` as an epsilon transition to the following instruction, and `search_automaton_exec` checks `accepting[start_state]` up front and returns a zero-length match before consuming any byte. `''` on `"abc"` now yields 5 segments.

#### Performance

- Bushy alt-literals search-mode is **~3.28× faster** than the prior anchored-match path (regression fixed); trie caching adds a **~7%** win on repeated bushy-alt-literal searches.
- Captured patterns (`@r(span("0-9"))`) on the search-VM (Tier 6) are **~24% faster** than the general-VM fallback (Tier 8) on the `cap_search` probe scenario (303 ns vs 399 ns at 2M iters), now that they route to Tier 6 instead of being silently dropped.
- Release+LTO and PGO builds both pass the full C suite (**2166 tests**); ASan+UBSan clean.
- PGO on top of LTO yields only a **marginal** further gain (1–8% on literal paths, ~0% elsewhere) — the decisive speedups came from the structural changes (P1–P5) + LTO.
- SNOBOL remains **1.3×–9.5× slower than PCRE2** on the synthetic probe scenarios (closest on SIMD scan at ~1.66×, widest on alternation/alt-literals at ~8–9.5×).
- **Shared-prefix BMH skip for alternation-of-literals** (`core/src/search_meta.c`, `core/src/search_tiers.c`): `derive_meta` now computes the longest common leading byte string across all alt-literal branches (`alt_literals_shared_prefix()`) and populates the existing Boyer–Moore–Horspool skip window from it, so the per-offset trial loop (TIER_GENERAL / TIER_AUTOMATON / TIER_SEARCH_VM) advances failing positions by more than one byte. Bushy alternations keep the trie (`has_literal_prefix` stays false and the automaton reroute now excludes all alt-literals); flat alternations have no shared prefix and are unaffected. Measured result: no regression on the standalone probe (the BMH only fires when a pattern reaches TIER_GENERAL/AUTOMATON *with* a shared alt-literal prefix; bushy→trie, flat→no shared prefix).

### Full-VM Backtracking Optimization

Behavior-preserving improvements to the full VM's (`vm_exec`, Tier 8) choice
stack — the remaining performance lever for the irreducibly stateful residue
(`EVAL`, `DYNAMIC`, `TABLE_*`, `ARRAY_*`, `EMIT_*`, `GOTO`/`LABEL`, `BAL`).

#### Added

- **Trail / undo-log choice save** (`core/src/vm_capture.c`, `core/src/vm_choice.c`): replaces the per-choice full-state `memcpy` snapshot. A `CompactChoiceHeader` now stores only `ip`, `pos`, `var_count`, and a `trail_base` index into a per-thread undo trail; state is restored on backtrack by replaying the abandoned thread's trail entries in reverse (`vm_trail_replay`). Choice-push cost becomes O(1) regardless of the number of loops or emits.
- **Page-linked choice-stack arena** (`core/include/snobol/vm.h`, `core/src/vm_choice.c`): `ChoiceArena` (4 KB pages, LIFO pop) replaces the realloc'd contiguous `vm->choices` buffer. Live records are never moved (no copy-on-grow), and `choice_allocated` now reflects precise peak footprint.
- **Zero-width-loop bounding** (`core/src/compiler_analysis.c`, `core/src/compiler.c`, `core/src/compiler_codegen.c`, `core/src/vm_exec.c`): `ast_node_nullable()` flags `ARB`/`ARBNO`/`REPEAT` over a nullable body at compile time (diagnostic via `snobol_diag`); `OP_REPEAT_STEP` skips the choice push once the iteration count exceeds `subject_len + 1`, bounding the backtracking blow-up for zero-width closures.
- **Short-circuit empty copies** (`core/src/vm_choice.c`): the legacy path guards its counter/capture `memcpy`s behind `max_counter_used > 0` / `max_cap_used > 0`; the compact/trail path copies no counters or write-log bytes at all. Non-`REPEAT` / non-`EMIT` patterns therefore push choice records with zero snapshot payload.
- **`SNOBOL_PROFILE` always defined for the core library** (`core/CMakeLists.txt`): populates `vm->profile.{dispatch_count,push_count,pop_count,max_depth}` so the catastrophic/choice tests can assert bounded choice-point counts.

#### Changed

- The legacy full-snapshot choice path (`struct choice`, selected by `SNOBOL_LEGACY_CHOICE=1`) is retained as a behavioral oracle; the default compact path is now trail + arena based.

#### Added (tests)

- `tests/c/test_vm_trail.c`: trail save produces bit-identical matches, captures, and EMIT output to the legacy snapshot path (run in both modes).
- `tests/c/test_choice_arena.c`: arena unit/reset/deep-backtrack tests.
- `tests/c/test_choice_shortcircuit.c`: non-`REPEAT`/non-`EMIT` patterns push zero-copy choice records.
- `tests/c/test_catastrophic.c`: zero-width-loop bounding keeps `dispatch_count` constant (not exponential) for `arbno(arbno(''))`.

#### Performance

- Choice-push is now O(1) for `REPEAT`/`EMIT`-heavy patterns (previously O(total loops + total emits) per backtracking point). Exact end-to-end speedup for Tier-8 residue patterns is tracked in `bench/BENCHMARKS.md`; see W5 benchmarks below.

### OSS Readiness (library-grade hygiene)

#### Added

- **C++ interop**: all 18 public headers (`core/include/snobol/*.h`) are now wrapped in `extern "C"` guards and are individually self-contained. A new `header-cxx` CI job (`.github/workflows/ci-core.yml`) compiles every public header as C++ with `g++` and `clang++` and links a trivial C++ TU against `libsnobol4.a`.
- **Single version source**: the top-level `project(libsnobol4 VERSION X.Y.Z)` is now the single source of version truth; `SNOBOL_VERSION_*` / `SNOBOL_VERSION_STRING` are generated into `<snobol/version.h>` via `core/cmake/version.h.in` at configure time. Version bumped to **0.12.0** (the real current version); header literals removed.
- **Governance docs**: added `SECURITY.md` (private vuln-report path + supported-versions policy), `CODE_OF_CONDUCT.md` (Contributor Covenant 2.1), `.github/ISSUE_TEMPLATE/` (bug + feature forms), and `.github/PULL_REQUEST_TEMPLATE.md` referencing the test/lint/warnings gate, the changelog rule, and the C/PHP coupling guard.
- **Changelog discipline** (`dev-hygiene` spec): every merged change must now append a `CHANGELOG.md` entry in Keep a Changelog format.

#### Changed

- **Translation-unit modularization** (behavior-preserving, file-membership only — no public API signature changes):
  - `search.c` → `search_meta.c` (derive_meta / eligibility / tier selection) + `search_tiers.c` (tier handlers / search-VM / NFA-DFA / dispatch), with shared readers + trie structs in `search_internal.h`; `select_tier_by_cost` promoted to external linkage.
  - `vm.c` → `vm_choice.c` (choice stack) + `vm_capture.c` (capture write-log) + `vm_exec.c` (executor + range/buffer/registry).
  - `compiler.c` → `compiler_analysis.c` (CodeBuf / charclass / SPLIT-ANY fusion) + `compiler_codegen.c` (C-AST codegen + label table), with shared infra in `compiler_internal.h`.
- Updated `README.md`, `CONTRIBUTING.md`, and `PROJECT_STUDY_GUIDE.md` for C++ usage, the single-source version scheme, the changelog rule, and the new TU layout.

#### Removed

- Nine confirmed-unused static helpers: `byte_set_eq`, `dfab_op`, `search_automaton_try` (search); `simd_read_u32` (search_simd); `vm_cb_init`, `vm_cb_free`, `vm_emit_lit_bytes` (vm); `pb_free`, `pb_emit_u16` (pattern_build).

#### Verified

- Core C suite: **2166 tests** pass (Release and ASan+UBSan); PHP binding suite: **315 PHPUnit tests** pass (incl. the C/PHP coupling probe). Both gated green after the TU modularization.

## [0.12.0] - 2026-07-08

### Engine Consolidation

### Added

- **`Pattern::searchSplitOffsets()`** (`bindings/php/src/snobol_pattern.c`): New PHP method returning `[[offset, length], ...]` pairs — zero zend_string allocation for token data. Single-pass offset recording with packed array construction. ~1.47× faster than pre-change `searchSplit` on delimiter-heavy scenarios.
- **Tier dispatch function pointer table** (`core/src/search.c`): Replaced 8 sequential if-branches in `snobol_search_exec()` with single `tier_table[meta->tier]` dispatch. Pre-computed `tier` field in `snobol_search_meta_t` enables O(1) tier selection.
- **`search_vm_t`** (`core/include/snobol/vm.h`): Lightweight VM state (~424 bytes) for Tier 1-7 execution. Excludes capture registers, variable registers, output buffer, and callback fields used by the full VM (~2500 bytes).
- **Metadata bitfield flags** (`core/include/snobol/search.h`): `snobol_search_meta_t.flags` packs 16 boolean flags into `uint32_t` for single-word access. `uint8_t tier` field stores pre-computed tier index.
- **BMH table on-demand** (`core/src/search.c`): `bmh_skip[256]` moved from inline metadata to heap-allocated pointer. Only allocated when `has_bmh_skip` is true. Reduces metadata copy from ~420 to ~200 bytes.
- **Reusable match API** (`core/src/api.c`, `core/include/snobol/snobol.h`): `snobol_match_create()`, `snobol_match_reset()`, and `snobol_pattern_search_reuse()` for hot-loop scenarios. Eliminates per-call malloc/free overhead (~30 ns per match).
- **`snobol_match_t` struct exposed** (`core/include/snobol/snobol.h`): Match result struct is now non-opaque, allowing callers to allocate on stack or reuse across calls.

### Removed

- **All JIT subsystems**: SLJIT backend (`deps/sljit/`), method JIT (`core/src/jit.c`, `core/src/search.c` Tier 0), tracing JIT IR (`core/src/jit_ir.c`). JIT config, stats, lifecycle, and CI matrix fully eliminated.
- **JIT tests**: `tests/c/test_jit*.c`, `.github/workflows/jit*.yml`, QEMU Dockerfiles.
- **JIT fields** from `VM` struct (`ip_counts`, `traces`, `ctx`, `jit_region`).
- `SNOBOL_FLAG_SEARCH_MODE`, `snobol_jit_config_t`, `snobol_jit_stats_t`, `snobol_get_jit_stats()` from public API.

### Added

- **Computed-goto dispatch** (`core/src/vm.c`): `while(1){switch(op)}` replaced with `goto *opcode_table[op]` dispatch table. MSVC fallback preserves switch dispatch. 15-30% improvement on interpreter-bound patterns.
- **Cached range pointers** (`core/src/search.c`, `core/src/vm.h`): Character-class range metadata pre-resolved at compile time into `range_meta[]` table. Eliminates runtime `get_ranges_ptr()` reparsing. Search-mode SPAN delimiter-heavy +30.1%.
- **BMH skip table** (`core/src/search.c`): Boyer-Moore-Horspool failure-position advance for literal-prefix patterns.
- **Automaton / trie matching** (`core/src/search.c`): Multi-string alternation-of-literals matcher using trie data structure. Wired as Tier 3a in search dispatch.
- **DFA automaton** (`core/src/search.c`): NFA-to-DFA subset construction for automaton-eligible patterns. Wired as Tier 7 in search dispatch. Handles LIT, LEN, SPAN, BREAK, ANY, NOTANY with epsilon closure for SPLIT/JMP. State explosion cap at 4096 states. Automaton scenario added to C probe (`bench_probe.c`).
- **SIMD Thompson NFA** (`core/src/search_simd.c`): Byte-parallel Thompson NFA execution using SIMD intrinsics. Wired as Tier 9 in search dispatch. Processes 32 bytes at once (AVX2) or 16 bytes at once (NEON). Eligible patterns: SPAN, BREAK, ANY, NOTANY with ASCII-only character classes. Scalar fallback for non-SIMD architectures. SPAN and BREAK override on non-class byte boundary detection. SIMD tier dispatches before general VM (Tier 8) when eligible.
- **Literal-match API** (`core/src/api.c`, `core/include/snobol/snobol.h`): `snobol_pattern_match_literal()` for zero-allocation anchored literal matching. Returns `snobol_literal_match_t` by value with `success`, `position`, `length` fields. Bypasses VM entirely for literal-only patterns.
- **Start-byte bitmap & minimum-length analysis** (`core/src/search.c`): PCRE2-style `compute_start_bitmap()` and `compute_minlength()` for all patterns. Bitmap-based candidate filtering in Tier 5 fallback.
- **PHP literal fast-path** (`bindings/php/src/snobol_pattern.c`): `Pattern::match()` now detects literal-only patterns via `snobol_search_derive_meta()` and bypasses VM setup entirely, calling `snobol_pattern_match_literal()` inline. Zero-allocation path for pure-literal patterns.
- **PHP `Pattern::matchLiteral()`** (`bindings/php/src/snobol_pattern.c`): new public method returning `{success, position, length}` associative array. Delegates directly to C `snobol_pattern_match_literal()`.
- **`ddev test-c-probe`** DDEV command (`.ddev/commands/web/test-c-probe`): runs only the `@group coupling-probe` filter. `CPhpCouplingTest` excluded from default `ddev test` (marked with `@group coupling-probe` and excluded in `phpunit.xml`).

### Changed

- **Search tier dispatch** reordered for maximum specificity: BREAK/SPAN → literal-only → literal-prefix → single-char bitmap → alt-literals trie → search-VM → automaton → general VM with bitmap fallback.
- **PHP binding** (`bindings/php/src/`): Removed all JIT blocks from `php_snobol.c`, `snobol_pattern.c`, `php_snobol.h`. `config.m4` JIT defines removed. `build-snobol-extension` `deps/` copy made conditional.
- **`bench_shared.h`**: Added `_POSIX_C_SOURCE` + `<time.h>` for Linux clock_gettime compat; `_DARWIN_C_SOURCE` for macOS snprintf.
- **`generate_amalgam.sh`**: JIT sources removed from amalgamation.
- **`CPhpCouplingTest`**: Renamed from `JitCPhpCouplingTest`, JIT assertions removed, compares `alt_literals` ratio instead of `tokenize`. Marked `@group coupling-probe`, excluded from default `ddev test`.
- **Benchmark baselines**: Updated `bench/results/search_perf_baseline.json` schema v2 with PCRE2 comparison data, no JIT stats.
- **`bench_delimiter.c`**: Removed dead JIT/search path; subject reduced from 16 KB to 1 KB all-comma (71M search calls → 100K single `snobol_pattern_match()` calls per iteration). 2000× faster (82 ms vs >3 min).
- **`bench_literal.c`**: Added literal-match-API scenario comparing `snobol_pattern_match_literal()` vs `snobol_pattern_match()` for pure-literal patterns. Literal API is 30.6× faster (612 µs vs 18,739 µs).

### Fixed

- **`search_vm_pop_choice()` infinite loop** (`core/src/search.c`): Off-by-one read in `search_vm_pop_choice()` caused it to read the wrong choice entry from the stack, and always returned `true` even when the choice stack was empty. This caused `searchAll()` and `searchSplit()` with multi-character alternation patterns (e.g., `'cat' | 'dog'`) to hang indefinitely. Fixed by reading from the correct offset and removing the `else { ip=0; pos=0; }` fallback that caused infinite restarts. All 1928 C tests + 349 PHP tests pass.
- **DFA build warnings**: `build_dfa()` in `search.c` had variables declared after `goto fail` paths; moved all cleanup variable declarations before the first failure point and added null guard on `snobol_free(ht)`. 14 `-Wsometimes-uninitialized` warnings eliminated.

## [0.11.0] - 2026-06-24

#### SLJIT Method JIT & Tracing-JIT Retirement — 2026-06-27

### Added

- **SLJIT single backend** (`deps/sljit/`): SLJIT replaces all 4 architecture-
  specific backends (ARM64, ARM32, RISC-V, x86-64). A single `jit_backend_sljit.c`
  covers all platforms via SLJIT's native code generation.
- **Method JIT** (`core/src/jit.c`, `core/src/search.c`): whole-pattern compilation
  via `snobol_jit_method_compile`/`snobol_jit_method_query`. Compiled functions are
  cached by bytecode identity and reused across calls. Tier 0 check in
  `snobol_search_exec` calls the compiled function if available.
- **Method JIT stats**: `method_attempts_total`, `method_successes_total`,
  `method_fallbacks_total`, `method_evictions_total` exposed via
  `snobol_get_jit_stats()`.

### Changed

- **JIT config** simplified: `snobol_jit_config_t` contains only
  `method_enabled`, `max_compiled_patterns`, `scratch_size`.
- **CI matrix consolidated**: 4 per-arch jobs replaced with 4 OS-native SLJIT
  jobs + 1 multi-arch QEMU job (`jit-qemu-smoke`).
- **QEMU Dockerfiles consolidated**: `ci/Dockerfile.jit-qemu-armv7` and
  `ci/Dockerfile.jit-qemu-riscv64` merged into `ci/Dockerfile.jit-qemu`.

### Removed

- **Tracing JIT** fully retired: `SnobolJitContext`, `pattern->jit_ctx`,
  per-IP trace compilation, LRU cache, profitability gate, `SNOBOL_JIT_METHOD`
  compile-time flag, choice-stack counters (`choice_push_total`,
  `choice_bytes_total`, `choice_pop_total`).
- **4 per-architecture backends**: `jit_backend_arm64.c`, `jit_backend_arm32.c`,
  `jit_backend_riscv64.c`, `jit_backend_x86_64.c` deleted.
- **9 tracing-JIT test files** removed from build.
- **Tracing-JIT PHP binding code**: all `jit_ctx`/`ip_counts`/`traces`/`ctx`/
  `search_mode` references removed from `snobol_pattern.c` and `php_snobol.h`.

### Fixed

- **test_jit_observability.c**: updated to use `method_attempts_total` /
  `method_successes_total` instead of removed tracing counters.
- **test_search_meta_cache.c**, **test_search_ex_api.c**: removed
  `entries_total` references.

#### searchSplit Bulk-Result Buffer — 2026-06-20

### Added

- **`Pattern::searchSplit` two-pass bulk path** (`bindings/php/src/snobol_pattern.c`):
  for large subjects (`>= 1 MB`) the binding now runs a pre-pass to count matches,
  allocates one C buffer of `subject_len + 1` bytes, `memcpy`s every segment from
  the subject into the buffer at a running cursor, wraps the buffer in a single
  parent `zend_string`, and inserts N+1 child zend_strings via `zend_string_init`
  over sub-ranges. Extracted into `snobol_searchsplit_bulk_path` so the small-
  subject fast path's I-cache footprint is unchanged.
- **Small-subject fast path preserved bit-for-bit**: the original
  `add_next_index_stringl` loop is retained for subjects below
  `SNOBOL_SEARCHSPLIT_BULK_THRESHOLD` (1 MB), so the binding has zero
  regression on the existing `JitCPhpCouplingTest::tokenize_php` workload
  (260-byte subject).
- **Diagnostic baselines** (`bench/baselines/`): C probe and PHP probe
  before/after captures committed so the bulk-path tuning is reproducible.

### Changed

- **Threshold-based dispatch** in `PHP_METHOD(Snobol_Pattern, searchSplit)`:
  the `PHP_METHOD` body is now a thin dispatcher (`if (subject_len < THRESHOLD)
  fast_path else bulk_path`), keeping the per-call hot path small.
- **Probe before/after numbers**:
  C `tokenize` 397 → 385 ns/iter (-3.0%), PHP `tokenize_php` 98,445 → 97,341
  ns/iter (-1.1%), `JitCPhpCouplingTest` unchanged (4 tests, 22 assertions).

### Note

- The bulk path is currently **~3.7% slower than the fast path at 100 KB** because
  the second `snobol_pattern_search_ex` pass outweighs the hash-table rehash
  savings at the current search-region cost. The threshold is set to 1 MB so
  the bulk path is reserved for very large subjects (and for future tuning
  when the search region becomes cheaper, e.g. via SSA IR in Phase 11).

#### JIT Search Performance Baseline — 2026-06-20

### Added

- **Stateful search C API** (`core/src/api.c`, `core/include/snobol/snobol.h`):
  new `snobol_pattern_search_ex()` plus opaque `snobol_pattern_search_state_t`
  that reuses a pre-initialized VM across calls. Eliminates per-iteration
  VM init, JIT context lookup, and search metadata derivation in hot loops.
  The stateful API is used by `bindings/php/src/snobol_pattern.c` for
  `Pattern::searchSplit`, `Pattern::searchAll`, and `Pattern::searchReplace`.
- **Search metadata caching** (`core/src/api.c`): `snobol_search_derive_meta`
  is now called once at compile time; the derived `snobol_search_meta_t` is
  stored on the `snobol_pattern` struct and reused on every search call.
  Previously this walked the bytecode per `snobol_pattern_search` invocation.
- **C-side `search_reset_vm` minimal-reset** (`core/src/search.c`): the
  per-candidate VM reset now touches only the fields that change between
  candidates (`ip`, `pos`, `var_count`, `max_cap_used`, `max_counter_used`,
  `cap_*` entries, `loop_*` entries), not the full VM struct. JIT state
  (`vm->jit.ip_counts`, `vm->jit.traces`, `vm->jit.ctx`) is preserved across
  iterations.
- **Public introspection API** (`core/include/snobol/snobol.h`):
  `snobol_pattern_get_bc()` / `snobol_pattern_get_bc_len()` for downstream
  tooling.
- **C tests**: `tests/c/test_search_meta_cache.c` verifies identical results
  before and after caching, and that `snobol_jit_get_stats()` reports the
  same JIT counters.

### Changed

- **PHP binding search paths** (`bindings/php/src/snobol_pattern.c`) migrated
  from the per-call `php_snobol_init_vm_for_search` helper to the stateful
  `snobol_pattern_search_state_t` API. Per-call `memset(VM, 0, sizeof(VM))`
  is amortised across the loop (one per `searchSplit` invocation, not per
  match).
- **JitCPhpCouplingTest extended** with per-scenario performance regression
  guard: the test fails if `tokenize_php` regresses by more than 10%
  relative to the captured baseline, catching the case where a JIT
  optimization helps the C path but the PHP binding is forgotten.

#### Diagnostic Probe — 2026-06-20

### Added

- **`snobol4_probe` C probe** (`bench/c/bench_probe.c`): standalone tool
  for per-iteration cost attribution. Runs 7 representative patterns
  (`literal_fail`, `literal_ok`, `span_comma`, `span_search`, `alternation`,
  `alt_search`, `tokenize`) in tight C loops and prints per-scenario
  timings + JIT stat deltas (entries, bailouts, choice push/pop, exec_ns,
  interp_ns).
- **PHP probe** (`bench/php/probe.php`): runs the same 7 scenarios via the
  public PHP API (`Pattern::fromString`, `Pattern::searchSplit`,
  `snobol_get_jit_stats`) and prints a comparable table. Measures the real
  user-facing cost including the binding layer.
- **`JitCPhpCouplingTest`** (`bindings/php/tests/php/JitCPhpCouplingTest.php`):
  runs both probes and asserts the binding is not silently drifting from
  the engine. The 10x-C guard is intentionally loose to accommodate
  legitimate architectural differences; the goal is to catch the case
  where a JIT optimization helps the C path but the PHP binding is
  forgotten.
- **`ddev build-c-probe`** command for automated in-container probe builds.
- **`BUILD_BENCH_C=ON`** CMake option in the top-level `CMakeLists.txt` to
  include the C probe in the build.

### Changed

- **`bench/README.md`** extended with usage instructions, scenario
  descriptions, and performance analysis guidance.
- **AGENTS.md** updated with the "JIT changes must cover both C and PHP
  binding" rule and the diagnostic-probe workflow.

#### Activate C JIT — 2026-06-20

### Added

- **`SNOBOL_FLAG_SEARCH_MODE` flag** (`core/include/snobol/snobol.h`): new
  `0x0002` flag for `snobol_pattern_compile_ex()` stored on the compiled
  pattern so the match function knows to use the JIT-accelerated search
  path.
- **`snobol_pattern_search()` C API** (`core/src/api.c`): wraps
  `snobol_search_exec()` with search metadata derivation, JIT context
  acquisition, and search-mode VM setup. C callers can now opt into the
  same acceleration tiers (literal `memchr`/`memmem`, BREAK/SPAN bitmap
  scan, automaton, JIT traces) that the PHP binding uses.
- **Idempotent `snobol_jit_init()`** (`core/src/jit.c`): static guard
  added so `snobol_context_create()` can safely call it once per process
  without leaking.
- **JIT context lifecycle on patterns** (`core/src/api.c`): patterns
  compiled with `SNOBOL_FLAG_SEARCH_MODE` acquire a JIT context at
  compile time and release it on `snobol_pattern_free`.
- **C benchmark search-mode columns** (`bench/c/bench_literal.c`,
  `bench_alternation.c`, `bench_tokenization.c`, `bench_substitution.c`,
  `bench_complex_http.c`, `bench_runner.c`): all five C microbenchmark
  suites now compare interpreter mode (current) against search/JIT mode,
  side by side with PCRE2.

#### Binding Performance & Range Syntax — 2026-06-20

### Added

- **C microbenchmark suite** (`bench/c/`): dedicated C-level benchmarks
  that compile the pattern once and match many times, isolating core
  engine performance from PHP binding overhead. Enables fair head-to-head
  with PCRE2 at the C level.
- **PatternHelper pattern cache** (`bindings/php/src/snobol_pattern_helper_php.c`):
  fixed-size cache integrates `PatternCache` into
  `PatternHelper::matchOnce`/`matchAll` so string patterns are compiled
  once and reused. Eliminates per-call lex→parse→compile overhead.
- **VM reuse in `searchAll`** (`bindings/php/src/snobol_pattern.c`):
  refactored to reuse the VM struct, choice stack, and dynamic pattern
  cache across iterations instead of allocating/zeroing/freeing per match.
  Batch results are produced as C arrays and converted to PHP once.
- **JIT search-mode profitability** (`core/src/jit.c`, `core/src/jit_ir.c`):
  lowered hotness threshold for search-mode patterns so the JIT fires
  earlier in delimiter-heavy workloads.
- **`memchr`/SIMD fast path** (`core/src/vm.c`): `OP_BREAK` and
  `OP_SPAN` use `memchr` (and SIMD intrinsics where available) for ASCII
  charclass scanning in the interpreter.
- **Builder range notation** (`bindings/php/src/snobol_pattern_php.c`):
  `Builder::span()`, `Builder::brk()`, `Builder::any()`, `Builder::notany()`
  accept range notation (`A-Z`, `0-9`); ranges are expanded at the
  PHP-to-C AST conversion layer. All `docs/why-snobol-vs-pcre.md`
  examples updated to use range syntax where appropriate.
- **Range syntax tests** (`bindings/php/tests/php/BuilderTest.php`):
  edge cases (literal hyphens, uppercase ranges, mixed range+literal
  chars) covered.
- **C microbenchmark CMake/Makefile integration** (`bench/c/CMakeLists.txt`,
  `Makefile`): dedicated `make bench-c` target.

### Changed

- **`docs/why-snobol-vs-pcre.md`** and `docs/examples/*.php` updated
  to use range syntax in illustrative examples.

#### Testing & Docs Meta — 2026-06-19

### Added

- **Fuzz harness** (`tests/fuzz/`): libFuzzer targets for the compiler
  path (pattern string → compiled bytecode) and the VM execution path
  (compiled bytecode + subject → match result).
- **Property-based tests** (`tests/c/test_property_based.c`): invariant
  tests for match results — capture consistency, backtracking correctness,
  substitution round-trips.
- **`docs/why-snobol-vs-pcre.md`**: new guide explaining SNOBOL4
  advantages, tradeoffs, and when to use each over PCRE2.
- **Hosted Doxygen** (`.github/workflows/doxygen-gh-pages.yml`):
  GitHub Pages deployment of the Doxygen-generated API reference on push
  to main.
- **Head-to-head benchmarks** (`bench/results/pcre2_comparison.md`):
  published PCRE2 head-to-head benchmark methodology and results.
- **Community bindings section** (`CONTRIBUTING.md`): explicit statement
  of core maintainer scope (C + PHP only) and contributor guidance for
  community-contributed language bindings (Python, Rust, Go, Java, etc.).

### Changed

- **`README.md`** and **`CONTRIBUTING.md`** updated for the v0.11.0 /
  v1.0.0 plan and the official scope statement.

#### AST Clone & Clean Build — 2026-06-19

### Added

- **`snobol_ast_clone()`** (`core/include/snobol/ast.h`, `core/src/ast.c`): deep-clone
  function for AST nodes. Recursively copies all 25 node types including owned string
  data and child subtrees.

### Fixed

- **Double-free in `x+` pattern repetition** (`core/src/parser.c`): `parse_repetition`
  used a shallow pointer copy (`clone = primary`) for the `+` operator, causing both
  the cloned node and `arbno(primary)` to point to the same AST node. When freed, the
  node was freed twice. Fixed by using `snobol_ast_clone(primary)` to produce a true
  deep copy.

### Changed

- **`make clean` now wildcarded** (`Makefile`): replaced explicit `rm -rf` of six build
  directories with a single `rm -rf build*/ cmake-build-*/` pattern, automatically
  catching any future build directories (e.g. `build-fuzz/`, `build-asan/`, etc.).
- **`docs/why-snobol-vs-pcre.md` examples** updated to use `Snobol\Builder` API instead
  of unsupported pattern string syntax (`BREAK`, `POS`, `RPOS`).

#### Core Primitives & Builtins — 2026-06-15

### Added

- **`POS(n)` positional primitive** (`core/src/vm.c`): new `OP_POS` opcode succeeds only
  when the current match cursor is exactly `n` codepoints from the start of the subject.
  Includes AST node (`AST_POS`), parser support, JIT call-out entry, and negative/invalid
  argument handling.
- **`TAB(n)` positional primitive** (`core/src/vm.c`): new `OP_TAB` opcode advances the
  cursor to `n` codepoints from the start.  Fails if already past the target or if `n`
  exceeds subject length.  Full AST/parser/JIT coverage.
- **`ABORT` control primitive** (`core/src/vm.c`): new `OP_ABORT` opcode sets the VM
  `abort_flag`, unwinds all choice points, and terminates the entire match immediately
  with no further backtracking.  AST/parser/JIT support.
- **`FAIL` control primitive** (`core/src/vm.c`): forces immediate backtrack by falling
  through to `OP_FAIL` dispatch.  AST/parser/JIT support.
- **`SUCCEED` control primitive** (`core/src/vm.c`): new `OP_SUCCEED` opcode forces
  immediate match success at the current cursor position, skipping the remainder of the
  pattern.  No-op for JIT (already handled by the compiled region).
- **Numeric comparison builtins** (`core/src/type_fn.c`): `snobol_eq()`, `snobol_ne()`,
  `snobol_lt()`, `snobol_gt()`, `snobol_le()`, `snobol_ge()` — string-to-double
  conversion via `snobol_str_to_double()` following SNOBOL4 numeric semantics
  (non-numeric yields 0.0).  Registered as `SNOBOL_FN_EQ` through `SNOBOL_FN_GE`
  (IDs 22–27) in the built-in dispatch table.
- **PHP Builder methods**: `Builder::pos(n)`, `Builder::tab(n)`, `Builder::abort()`,
  `Builder::fail()`, `Builder::succeed()` with `php_ast_to_c` conversion.
- **PHP `snobol_text_eq/ne/lt/gt/le/ge/integer()` functions** exposed as
  `Snobol\Text::*` PHP callables.
- **C tests**: `tests/c/test_pattern_pos_tab.c` (186 assertions), `tests/c/test_pattern_abort_fail_succeed.c` (172 assertions), `tests/c/test_comparison_numeric.c` (80 assertions).
- **PHP tests**: `tests/php/PrimitivesTest.php` (90 tests), `tests/php/ComparisonsTest.php` (183 tests).
- **`snobol_str_to_double()`** helper exposed in `core/include/snobol/type_fn.h` for reuse.

#### Array Data Type — 2026-06-16

### Added

- **C ARRAY type** (`core/src/array.c`, `core/include/snobol/array.h`): sparse hash-map
  backed indexed storage with open-addressing and FNV-1a hashing.  1-based indexing by
  default following SNOBOL4 semantics.  API:
  - `snobol_array_create(bounds_hint)` / `snobol_array_retain()` / `snobol_array_release()`
  - `snobol_array_get()` / `snobol_array_set()` / `snobol_array_delete()` / `snobol_array_has()`
  - `snobol_array_size()` / `snobol_array_keys()` / `snobol_array_values()` / `snobol_array_clear()`
  - Automatic resize with tombstone tracking; initial capacity `ARRAY_INITIAL_CAPACITY (16)`.
- **VM opcodes** (`core/src/vm.c`): `OP_ARRAY_GET` and `OP_ARRAY_SET` — register-based
  lookups using capture register keys with name-resolution and table-style encoding.
  JIT call-out entries for both opcodes.
- **VM array registry**: `vm_init_arrays()` / `vm_free_arrays()` / `vm_register_array()` /
  `vm_get_array()` — parallel to the existing table registry.
- **PHP `Snobol\Array_` class** (`bindings/php/src/snobol_array_php.c`, `bindings/php/src/snobol_array_php.h`):
  C extension class with `get()`, `set()`, `delete()`, `size()`, `keys()`, `values()` methods.
  PHP stub in `bindings/php/php-src/Array_.php` (IDE autocomplete).
- **`Builder::arrayRef()` method** for constructing array-access sub-patterns.
- **C tests**: `tests/c/test_array.c` (214 assertions) covering create/set/get/delete/size/
  keys/values/resize/tombstone/rehash.
- **PHP tests**: `tests/compat/ArrayTest.php` (176 assertions) covering all `Snobol\Array_`
  operations including edge cases (empty array, single element, many elements, deletion).

#### Full BMP Unicode — 2026-06-16

### Added

- **Full BMP case-folding tables** (`core/src/unicode_fold_data.c`): ~2500 lines of
  generated pair tables covering the entire Basic Multilingual Plane (U+0000–U+FFFF).
  Generated from UCD CaseFolding.txt via `dev/gen_unicode_fold.c`.  Replaces the
  previous Latin-1 + Latin Extended-A tables.
- **`dev/gen_unicode_fold.c`**: new C generator program that reads Unicode Character
  Database CaseFolding.txt and emits self-contained static tables.  Invoked via
  `make gen-unicode-fold`.
- **BMP-aware case conversion** (`core/src/unicode_fold.c`): `snobol_to_upper_cp()` and
  `snobol_to_lower_cp()` now cover Greek, Cyrillic, Arabic, Hebrew, CJK, and all other
  BMP scripts.  Multi-character expansion preserved (e.g., ß → SS).  ASCII fast path
  still applies for U+0000–U+007F.
- **BMP-aware `UPPER` / `LOWER`** (`core/src/string_fn.c`): full BMP case folding via
  codepoint-level delegation; astral plane codepoints (U+10000+) pass through unchanged.
- **BMP-aware case-insensitive matching** (`core/src/compiler.c`): `SNOBOL_FLAG_CASE_INSENSITIVE`
  now folds charclass ranges and single codepoints across the entire BMP using the
  generated tables.  Cyrillic, Greek, and CJK case pairs are correctly handled in
  `ANY` / `NOTANY` / `SPAN` / `BREAK` opcodes.
- **PHP `Snobol\Text::upper()` / `Snobol\Text::lower()`** now delegate to the BMP-aware
  C implementation (was previously ASCII-only).
- **C tests**: expanded `tests/c/test_unicode_fold.c` (60 assertions) with Cyrillic,
  Greek, Arabic, Hebrew, CJK case-fold test cases; expanded `tests/c/test_pattern_case.c`
  (36 new assertions) for BMP case-insensitive matching with Greek and Cyrillic patterns.
- **PHP tests**: `tests/php/UnicodeTest.php` (37 tests) covering BMP UPPER/LOWER with
  Greek, Cyrillic, CJK, and mixed-script strings.

#### Convenience API for PHP binding — 2026-06-18

### Added

- **`snobol_match()` one-shot C API** (`core/src/api.c`): bundles lex→parse→compile→VM
  into a single call returning a heap-allocated `snobol_match_result_t` with `success`,
  `error`, `output`, and positional `captures[]`.  Ideal for one-off matches; for repeated
  matching of the same pattern use the multi-step API instead.
- **`snobol_pattern_build_*()` C builder API** in `core/src/api.c`: programmatic AST
  construction for all 22 pattern primitives (lit, span, brk, any, notany, len, arbno,
  cap, assign, concat, alt, label, goto, pos, tab, abort, fail, succeed, …).
- **PHP binding** (`bindings/php`):
  - `Snobol\Builder` C class wrapping `snobol_pattern_build_*()` — 35 static methods
    (lit, span, concat, alt, cap, label, tableAccess, etc.).
  - `Snobol\PatternCache` and `Snobol\DynamicPatternCache` migrated from PHP to C
    (LRU eviction, dynamic pattern compilation cache).
  - `Snobol\PatternHelper` migrated to C with all 10 methods (fromString, fromAst,
    matchOnce, matchAll, split, replace, evalPattern, tableSubst, formattedSubst, clearCache).
  - `Pattern::match()` and `Pattern::*` migrated to C — no PHP-level pattern processing
    (enforced by `ArchitecturalConstraintsTest`).
  - `Snobol\Text` PHP class removed; all `snobol_text_*()` helpers (size, trim, dupl,
    reverse, substr, replace, char, ord, upper, lower, eq, ne, lt, gt, le, ge, ident,
    differ, lexeq, lexlt, lexgt, integer, real, numeric) implemented as C `PHP_FUNCTION()`s.
- **PHP 8.5 compatibility** (DDEV 8.5.7, API 20250925):
  - Replaced removed `zend_call_static_method` with direct `zend_call_method` on objects.
  - Updated `zend_call_method_with_*_params` invocations to new inline-function signatures
    taking `zend_object*` first arg.
  - Replaced `zend_ce_invalid_argument` with `zend_ce_value_error` and `zend_ce_std` with
    `Z_PARAM_OBJECT`.
- **Examples**: `examples/c/one_shot_match.c` demonstrates the new one-shot C API.
- **Tests**:
  - New `tests/c/test_api_match.c` with 33 assertions for `snobol_match()` API.
  - New `tests/c/test_compiler.c` with 19 assertions for the AST→bytecode compiler,
    including regression tests for the capture-exposure bug.
  - New `bindings/php/tests/php/ConvenienceApiTest.php` with 35 tests for the PHP
    convenience layer (3 of which are capture tests for `Builder::cap`).
  - C core test suite now 1621 tests (up from 1569).
  - PHP test suite now 356 tests (up from 321).

### Fixed

- **PHP 8.5 `add_assoc_zval` no longer increments refcount** (root cause of all 321+ PHP
  test crashes).  Added `snobol_assoc_zval()` helper in `php_snobol.h` that uses
  `ZVAL_COPY` + `zend_hash_str_update` to properly retain sub-pattern references.
  Replaced all 22 `add_assoc_zval` calls across builder, dynamic cache, and pattern code.
- **`Builder::cap(reg, sub)` did not expose captures in the match result**.
  `OP_CAP_END` only updated `cap_start[reg]` / `cap_end[reg]`; `vm.var_count`
  was bumped only by `OP_ASSIGN`.  Fixed `core/src/vm.c::OP_CAP_END` to also
  write `var_start[reg]` / `var_end[reg]` and bump `var_count` so capture
  register `reg` is exposed as `v<reg>` in `Pattern::match()` and in
  `snobol_match()` results.  This removes the need for an explicit
  `Builder::assign(var, reg)` after every `Builder::cap(...)`.
- **Use-after-free in cache `touch` functions** (`php_dyncache_touch`, `php_pcache_touch`):
  `zend_hash_next_index_insert(&kv)` does not bump refcount, but `zval_ptr_dtor(&kv)`
  was called immediately after, freeing the string that the hash table still referenced.
- **`zend_call_method` returns retval pointer, never NULL** — exception detection was
  wrong (`if (!call_result)` would never trigger).  Replaced with `Z_TYPE(retval) == IS_OBJECT`
  checks; added `zend_clear_exception()` so failure paths return structured error results
  instead of propagating exceptions.
- **Missing capacity check + LRU eviction** in `DynamicPatternCache::compile`/`evaluate`.
- **Type string length typo**: `"table_access"` was stored with length 11 instead of 12,
  silently truncating the type to `"table_acces"` and breaking AST conversion.
- **6 missing arginfo entries** for no-param Builder methods (`fence`, `rem`, `arb`,
  `abort`, `fail`, `succeed`) — suppressed PHP 8.1+ "Missing arginfo" warnings.
- **CI PHP 8.4 install** (`.github/workflows/ci-php.yml`): Ubuntu noble's default
  apt repos do not ship `php8.4-dev`.  `shivammathur/setup-php` adds `ppa:ondrej/php`
  with an inline PGP key block, which conflicts with `add-apt-repository`'s keyring
  format ("E: Conflicting values set for option Signed-By").  Fixed by removing any
  pre-existing ondrej source files (`rm -f /etc/apt/sources.list.d/ondrej-php*`)
  before adding the PPA cleanly via `add-apt-repository --no-update`.  All matrix
  versions (8.3, 8.4, 8.5) install consistently.

### Changed

- **PHP binding now self-contained**: removed `bindings/php/php-src/` entirely (9 files
  deleted); all class bodies live in C.  The `.so` no longer depends on Composer
  autoloading for class definitions — only IDE stubs under `bindings/php/stubs/`.
- **Composer's PSR-4 autoload** now points at `bindings/php/stubs/` (IDE-only).

## [0.10.0] - 2026-06-09

### Added — Windows / Linux / macOS x86-64 JIT Backend (`jit-windows-x86`)

- **x86-64 code-generation backend**: New `core/src/jit_backend_x86_64.c` implements
  full JIT lowering for x86-64. Supports all 22 IR opcodes via direct instruction encoding
  (no assembler dependency).
- **Dual ABI support**: Compile-time `SNOBOL_JIT_WIN64_ABI` selects between System V AMD64 ABI
  (Linux/macOS) and Microsoft x64 ABI (Windows). The ABI is auto-detected from `CMAKE_SYSTEM_NAME`.
- **Register convention**: rbx=VM (callee-saved), rsi=s, rdi=pos, r12=len — mirrors ARM64 layout.
  Register mapping adjusted per ABI for call-out argument passing.
- **Full instruction emitter suite**: REX prefix, ModRM, SIB encoding; MOV rr/ri/rm/mr, ADD, SUB,
  XOR, CMP, TEST, MOVZX; JMP rel8/rel32, Jcc rel8/rel32; CALL rel32/CALL [mem]; PUSH/POP;
  prologue/epilogue.
- **Code-page allocation**:
  - **Windows**: `VirtualAlloc(MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE)` →
    `VirtualProtect(PAGE_EXECUTE_READ)` — DEP-compliant, never uses `PAGE_EXECUTE_READWRITE`.
    Static assert verifies no `PAGE_EXECUTE_READWRITE` usage in debug builds.
  - **Linux**: `mmap(PROT_READ|PROT_WRITE)` → `mprotect(PROT_READ|PROT_EXEC)` (W^X model).
  - **macOS**: `mmap(MAP_JIT)` → `pthread_jit_write_protect_np`.
- **CI — x86_64 JIT jobs**: New matrix entries in `jit-backend-tests` for `ubuntu-latest` (x86-64),
  `windows-latest` (x86-64 MSVC), and `macos-13` (Intel x86-64), each with `SNOBOL_JIT_BACKEND=x86_64`.
- **Test coverage**: New `tests/c/test_jit_x86_64.c` with architecture-specific round-trip tests
  (LIT, SPLIT, LEN). JIT enabled for x86-64 hosts in `tests/c/CMakeLists.txt`.
- **Documentation**: README, CONTRIBUTING, and CHANGELOG updated to document x86_64 backend
  availability, dual ABI support, DEP compliance, and build instructions.

### Added — Linux RISC-V 64 JIT Backend (`jit-riscv64`)

- **RISC-V 64-bit code-generation backend**: New `core/src/jit_backend_riscv64.c` implements
  full JIT lowering for RV64I base ISA. Supports all 22 IR opcodes via direct instruction
  encoding (no assembler dependency).
- **RISC-V JIT backend registration**: `snobol_jit_riscv64_register()` called from `snobol_jit_init()`
  when compiled on `__riscv` with `__riscv_xlen == 64` targets.
- **Fixed register convention**: a0=VM, t0=s, t1=pos, t2=len, t3–t6=scratch, s2=loop counter —
  mirrors ARM64 layout for commonality.
- **RV64I instruction emitters**: R-type, I-type, S-type, B-type, U-type, J-type formats,
  plus load/store (LD, SD, LB, SB, LBU), branches (BEQ, BNE, BLT, BGE, BLTU, BGEU),
  and AUIPC+JALR call-out sequence for ±2 GB range.
- **RISC-V psABI call-out sequence**: save ra, load args into a0–a7, AUIPC+JALR, restore ra —
  full C calling convention compatibility.
- **Code-page allocation**: `mmap(PROT_READ|PROT_WRITE)` → `mprotect(PROT_READ|PROT_EXEC)`
  (W^X model), with `__builtin___clear_cache` for icache coherence.
- **`SNOBOL_JIT_RV64C` CMake option**: Optional RV64C compressed instruction support (default OFF).
- **CI — QEMU RISC-V 64 job**: New `jit-qemu-riscv64` GitHub Actions job validates the RISC-V JIT
  in a QEMU-emulated RISC-V 64 container via `docker/setup-qemu-action`.
- **CI — CMake backend validation**: `riscv64` added to valid backend list; auto-detected when
  compiling on RISC-V 64 hosts.
- **Test coverage**: New `tests/c/test_jit_riscv64.c` with architecture-specific round-trip
  tests (LIT, SPLIT, LEN). All existing JIT tests now detect `__riscv`/`__riscv_xlen == 64`
  for platform support.
- **Documentation**: README updated to document `riscv64` backend availability and `SNOBOL_JIT_RV64C`
  option.

### Added — Linux ARM32 JIT Backend (`jit-arm32`)

- **ARM32 Thumb-2 code-generation backend**: New `core/src/jit_backend_arm32.c` implements
  full JIT lowering for ARMv7-A Thumb-2. Supports all 22 IR opcodes via direct Thumb-2
  encoding (no assembler dependency).
- **ARM32 JIT backend registration**: `snobol_jit_arm32_register()` called from `snobol_jit_init()`
  when compiled on `__arm__` / `__thumb__` targets.
- **Fixed register convention**: r0=VM, r1=s, r2=pos, r3=len, r4–r11=temps, r12=BLX scratch,
  lr=link register — mirrors ARM64 layout for commonality.
- **Thumb-2 instruction emitters**: Data-processing (MOV, ADD, SUB, CMP, AND, ORR),
  branches (B, BL, BLX, BX, CBZ, CBNZ), load/store (LDR, STR, LDRB, STRB), and
  literal-pool-based LDR (literal) for large immediates.
- **AAPCS32 call-out sequence**: `PUSH {lr}`, argument setup in r0–r3, `BLX` to helper,
  `POP {lr}` — full C ABI compatibility.
- **Code-page allocation**: `mmap(PROT_READ|PROT_WRITE)` → `mprotect(PROT_READ|PROT_EXEC)`
  (W^X model), with `__builtin___clear_cache` for icache coherence.
- **CI — QEMU ARMv7 job**: New `jit-qemu-armv7` GitHub Actions job validates the ARM32 JIT
  in a QEMU-emulated ARMv7 container via `docker/setup-qemu-action`.
- **CI — CMake backend validation**: `-DSNOBOL_JIT_BACKEND=arm32` tested in QEMU ARMv7 job.
- **Test coverage**: New `tests/c/test_jit_arm32.c` with architecture-specific round-trip
  tests (LIT, SPLIT, ANY, LEN, SPAN). All existing JIT tests now detect `__arm__`/`__thumb__`
  for platform support.
- **Documentation**: README updated to document `arm32` backend availability and minimum
  target (ARMv7-A Thumb-2).

### Added — Linux AArch64 JIT (`jit-arm64-linux`)

- **CMake platform detection**: `core/CMakeLists.txt` now sets `SNOBOL_JIT_PLATFORM_MACOS` or
  `SNOBOL_JIT_PLATFORM_LINUX` compile definitions for JIT code paths.
- **Linux code-page allocation**: `snobol_jit_alloc_code` uses `mmap(MAP_ANONYMOUS|MAP_PRIVATE,
  PROT_READ|PROT_WRITE)` on Linux; `snobol_jit_seal_code` calls `mprotect(PROT_READ|PROT_EXEC)`
  to enable execution (W^X model). `MAP_JIT` is macOS-only.
- **Linux icache flush**: `arm64_flush_icache` calls `__builtin___clear_cache` with a `cacheflush`
  syscall fallback for older kernels / QEMU user-mode.
- **Debug W^X enforcement**: Debug builds assert that `PROT_WRITE` is cleared after sealing.
- **CI — QEMU AArch64 job**: New `jit-qemu-aarch64` GitHub Actions job validates the JIT in a
  QEMU-emulated AArch64 container via `docker/setup-qemu-action`.
- **CI — native AArch64 runner**: `ubuntu-24.04-arm` runner leg already present.
- **Documentation**: README and CONTRIBUTING updated with Linux AArch64 build instructions and
  W^X policy notes.

### Added — JIT Neutral IR Layer (`jit-neutral-ir`)

- **Architecture-neutral IR definition** (`core/include/snobol/jit_ir.h`):
  `jit_ir_opcode_t` enum and `jit_ir_instr_t` struct with pre-decoded operands
  and virtual register operands (max 256 per region, `uint16_t vreg_next`).

- **IR region builder** (`core/src/jit_ir.c`):
  `jit_ir_region_new`, `jit_ir_append`, `jit_ir_alloc_vreg`, `jit_ir_inc_use`.
  Arena-style growable instruction array; marks region `non_compilable` and logs
  a warning if the 256-register limit is exceeded.

- **VM opcode lifter** (`jit_ir_lift_region`):
  Translates all VM bytecodes to IR in a single linear pass.  Covers every opcode
  listed in the JIT coverage matrix (jit-compiled, call-out, and pseudo groups).

- **IR optimiser passes**:
  - **DCE** (`jit_ir_dce`): removes pure instructions whose output register has
    zero uses.
  - **Copy-propagation** (`jit_ir_copy_propagation`): folds `JIT_IR_COPY`
    instructions into their consumers, then triggers DCE to remove dead copies.

- **`SNOBOL_JIT_DUMP_IR=1` environment variable**: when set, writes a
  human-readable IR dump to `stderr` before the backend lowerer runs.

- **`jit_backend_t` vtable** (`core/include/snobol/jit_backend.h`):
  `lower`, `flush_icache`, `name` function pointers.
  `jit_backend_register()` and `jit_backend_get()` registration API in `jit.c`.

- **`SNOBOL_JIT_BACKEND` CMake option** (default: `arm64`):
  selects the backend at compile time; unknown values produce a `FATAL_ERROR`
  listing valid backend names.

- **ARM64 backend** (`core/src/jit_backend_arm64.c`):
  Implements the vtable; moves all ARM64 code-generation out of `jit.c`.
  Has its own CFG builder (`ir_cfg_build`) and IR-based block emitter
  (`emit_block_ops_ir`).

- **Unit tests** (`tests/c/test_jit_ir.c`): covers region builder, vreg
  allocation, 256-register limit, DCE, and copy-propagation.  Registered as
  `test_jit_ir` and `test_ir_roundtrip` CTest targets with `jit-ir` / `roundtrip`
  labels.

- **CI job** (`ci-jit-backend-tests`): builds and runs the full JIT test suite on
  macOS Apple Silicon and Linux AArch64 runners using `SNOBOL_JIT_BACKEND=arm64`.

### Changed

- **`jit.c` legacy fallback removed**: the pre-IR direct ARM64 emission code
  (~1500 lines) has been deleted from `jit.c`.  The new two-phase IR pipeline is
  the only compilation path; `snobol_jit_compile` returns `nullptr` if no backend
  is registered (should not occur after `snobol_jit_init()`).

- **`vreg_next` type** in `jit_ir_region_t` changed from `uint8_t` to `uint16_t`
  to prevent silent wrap-around at the 256-register boundary.

### Verified

- **No breaking changes**: public API (`snobol.h`, `compiler.h`, `vm.h`, etc.) is unchanged. All internal refactoring (IR pipeline, backend vtable) is transparent to callers. Existing bytecode, patterns, and bindings continue to work without modification.

## [0.9.0] - 2026-05-22

### Added - Full JIT Opcode Coverage

- **Full JIT opcode coverage**: All VM opcodes now have compiled-region implementations in the ARM64 micro-JIT — no more interpreter fallback for any opcode group:
  - **Position guards** (`OP_REM`, `OP_RPOS`, `OP_RTAB`): compiled inline as integer comparisons against `vm->sp` / `vm->subject_len`.
  - **Cut/fence** (`OP_FENCE`): compiled inline; truncates the choice stack at the current depth.
  - **Labeled control flow** (`OP_LABEL`, `OP_GOTO`, `OP_GOTO_F`): `OP_LABEL` is a no-emit pseudo-op; `OP_GOTO` / `OP_GOTO_F` compile as unconditional / conditional branches to resolved label targets.
  - **Emit opcodes** (`OP_EMIT_LITERAL`, `OP_EMIT_CAPTURE`, `OP_EMIT_FORMAT`, `OP_EMIT_TABLE`, `OP_EMIT_EXPR`): compiled as inline call-outs to the registered `vm->emit_fn` callback via `BLR`.
  - **Table operations** (`OP_TABLE_GET`, `OP_TABLE_SET`): compiled as call-outs to `snobol_jit_helper_table_get` / `snobol_jit_helper_table_set`.
  - **Balanced match** (`OP_BAL`): compiled as a call-out to `snobol_jit_helper_bal`.
  - **Host callbacks** (`OP_EVAL`): compiled as a call-out with full caller-saved register spill/restore around the `BLR`.
  - **Dynamic patterns** (`OP_DYNAMIC`, `OP_DYNAMIC_DEF`): `OP_DYNAMIC` compiled as a call-out to `snobol_jit_helper_dynamic`; `OP_DYNAMIC_DEF` treated as a region-termination pseudo-op.

- **JIT observability counter test suite** (`tests/JitOpcodeCoverageTest.php`): one test case per opcode group asserting `jit_bailouts_total == 0` and `jit_exec_time_ns_total > 0` after representative patterns run under `SNOBOL_JIT=1`.

### Changed

- **Benchmark gate** (`bench/compare_jit.php`): added `jit_ratio_check()` function and an end-of-script gate that reads `jit_exec_time_ns_total` / `jit_interp_time_ns_total` from `snobol_jit_get_stats()` and exits with code 1 if the interpreter-time ratio exceeds 5%.
- **Opcode coverage comment** in `core/src/jit.c`: all entries updated to `jit-compiled`, `call-out`, or `pseudo` — no `fallback` entries remain.

## [0.8.0] - 2026-05-21

### Build & Tooling Hardening (`build-tooling-hardening`)

### Added

- **`SNOBOL_SANITIZE` CMake option**: When `ON`, compiles the library and all
  test binaries with `-fsanitize=address,undefined -fno-omit-frame-pointer`.
  A fatal error is emitted if `SNOBOL_SANITIZE=ON` is requested on MSVC.

- **`test-asan` CMake custom target**: Runs the C test suite under
  AddressSanitizer + UndefinedBehaviorSanitizer.  Available in any build
  configured with `-DSNOBOL_SANITIZE=ON`.

- **`test-valgrind` CMake custom target**: Runs the C test suite under
  Valgrind (`--error-exitcode=1 --leak-check=full --track-origins=yes`).
  Not created if Valgrind is absent from `PATH` (warning emitted instead).

- **`make build-asan` and `make test-asan`** Makefile convenience aliases
  delegating to the CMake targets.  `make test-valgrind` now delegates to the
  CMake `test-valgrind` target rather than running Valgrind via shell directly.

- **`libsnobol4.pc` pkg-config file**: Generated by `configure_file()` in
  `core/CMakeLists.txt` and installed to `${CMAKE_INSTALL_LIBDIR}/pkgconfig/`.
  Consumers can discover the library with `pkg-config --cflags --libs libsnobol4`.

- **`CMakePresets.json`** at project root: named presets `debug`, `release`,
  `asan`, `windows-msvc`, `windows-mingw` with corresponding build and test
  presets for `debug`, `release`, and `asan`.

- **Optional GitHub Actions workflows**:
  - `.github/workflows/sanitizers.yml`: ASan + UBSan build on `ubuntu-latest`,
    triggered by `workflow_dispatch` or nightly `schedule: cron: '0 2 * * *'`.
    Uses `-DSNOBOL_SANITIZE=ON` and the `test-asan` CMake target.
    Not part of the default PR gate.
  - `.github/workflows/benchmarks.yml`: full benchmark suite on `ubuntu-latest`,
    triggered by `workflow_dispatch` (with optional `base_ref` input) or nightly
    schedule.  Runs `php bench/run_all.php` and uploads results as a 30-day artifact.

- **Doxygen doc comments** on all 14 public headers under `core/include/snobol/`:
  every public function, struct, enum, macro, and typedef now has a
  `/** @brief ... @param ... @return ... */` comment.

### Changed

- **`core-build`**: `cmake --install` now also installs `libsnobol4.pc` to
  `${CMAKE_INSTALL_LIBDIR}/pkgconfig/`.  Install summary message updated.
- **`core-build`**: `SNOBOL_JIT` is explicitly force-set to `OFF` on WIN32
  (in addition to the existing `NOT WIN32` processor guard) so that
  `SNOBOL_JIT=ON` passed by a user is silently overridden on Windows.
- **`ci-core.yml`** matrix already included `windows-latest`; no change needed.
  Both `sanitizers.yml` and `benchmarks.yml` are separate from the PR gate.

---

## [0.7.0] - 2026-05-20

### Unicode Completeness (`unicode-completeness`)

### Added

- **UPPER / LOWER v2** (`core/src/unicode_fold.c`): Full Unicode case conversion
  covering Latin-1 Supplement (U+00C0–U+00FF), Latin Extended-A (U+0100–U+017F),
  and multi-character expansion for German sharp-s (ß → SS). `snobol_upper()` and
  `snobol_lower()` now decode UTF-8 codepoints with `utf8_peek_next()` and use a
  self-contained static fold table; ASCII fast-path is preserved.

- **`SNOBOL_FLAG_CASE_INSENSITIVE` (0x0001u)** in `core/include/snobol/snobol.h`:
  Compile-time flag enabling case-folded pattern matching.

- **`snobol_pattern_compile_ex(ctx, source, len, flags, error)`**: New public API
  function that accepts a `flags` bitmask. Pass `SNOBOL_FLAG_CASE_INSENSITIVE` to
  enable case-insensitive matching; unknown flag bits are silently ignored. The
  existing `snobol_pattern_compile()` now delegates to `compile_ex` with `flags=0`.

- **`snobol_get_api_version()`**: Returns `(MAJOR << 16) | (MINOR << 8) | PATCH` as
  `uint32_t`. For v0.7.0 this returns `0x00000700u`. Intended for binding load-time
  compatibility checks. Declared in `snobol.h`, implemented in `core/src/version.c`.

- **PHP binding load-time version check**: `PHP_MINIT_FUNCTION(snobol)` now calls
  `snobol_get_api_version()`, extracts the major version, and throws a PHP
  `RuntimeException` (returning `FAILURE`) if the linked library's major version
  does not match the compile-time constant.

- **`snobol_get_api_version()` PHP function**: Exposed as a first-class PHP function;
  returns the library's API version integer directly from the C library.

- **`Pattern::fromString()` supports `caseInsensitive` option**: The `$options`
  array parameter now accepts `['caseInsensitive' => true]` to compile source-text
  patterns via the case-insensitive `compile_ast_to_bytecode_c()` path.

### Verified

- All C unit tests pass (1359/1359) ✅
- All PHP tests pass (236/236) ✅
- New C test suites: `test_unicode_fold` (22 cases), `test_string_case` (Unicode),
  `test_pattern_case` (11 cases), `test_api_version` (5 cases)
- `core_amalgam.c` regenerated (13 source files) ✅

---

## [0.6.0] - 2026-05-10

### PHP Binding Cleanup (`php-binding-cleanup`)

### Verified / Enforced

- **No PHP-native lexer or parser in `bindings/php/php-src/`**: confirmed that
  `Lexer.php` and `Parser.php` do not exist; the "C core owns all parsing"
  architectural goal is now formally verified and permanently guarded.

- **`Pattern::fromString()` fully C-backed**: the C extension method
  (`snobol_pattern.c`) routes through `snobol_lexer_create()` →
  `snobol_parser_parse()` → `compile_ast_to_bytecode_c()`. C-side parse errors
  are caught with `snobol_parser_has_error()` / `snobol_parser_get_error_location()`
  and thrown as PHP `\Exception` with a message of the form
  `"Parse error at line N, column M: <detail>"`.

- **`PatternHelper::fromAst()` fully C-backed**: the helper validates
  `isset($ast['type'])` then delegates the entire compilation to
  `Pattern::compileFromAst()` (C extension `compile_ast_to_bytecode_c()`).
  Zero PHP-side AST traversal.

### Added

- **`ArchitecturalConstraintsTest`** (`bindings/php/tests/php/ArchitecturalConstraintsTest.php`):
  new PHPUnit test class that enforces the no-native-parsing rule permanently:
  - `testNoPhpNativeLexerInstantiation()` — asserts zero `new Lexer(` under `php-src/`
  - `testNoPhpNativeParserInstantiation()` — asserts zero `new Parser(` under `php-src/`
  - `testLexerPhpFileDoesNotExist()` — asserts `php-src/Lexer.php` is absent
  - `testParserPhpFileDoesNotExist()` — asserts `php-src/Parser.php` is absent

### Verified

- All 198 PHP tests pass (`ddev exec vendor/bin/phpunit tests/`) ✅
- Zero regressions in `PatternTest`, `BuilderTest`, `PatternHelper`-exercising tests ✅
- New architectural constraints: 4/4 tests pass ✅

---

### Code Quality Improvements

### Changed

- **`nullptr` throughout `core/src/*.c`**: all `NULL` pointer literals replaced
  with `nullptr`. The single surviving `NULL` in the codebase is the string
  literal `"(NULL)\n"` in `ast.c` (intentional).

- **`[[nodiscard]]` (alias `SNOBOL_NODISCARD`) on public headers**: annotated in
  `core/include/snobol/`:
  - `search.h` — `snobol_search_exec()`
  - `table.h` — `table_create()`, `table_retain()`, `table_set()`, `table_delete()`
  - `string_fn.h` — all twelve `bool`-returning functions (`snobol_trim`,
    `snobol_dupl`, `snobol_reverse`, `snobol_substr`, `snobol_replace`,
    `snobol_replace_char`, `snobol_lpad`, `snobol_rpad`, `snobol_char_fn`,
    `snobol_ord`, `snobol_upper`, `snobol_lower`)
  - All previously-silently-discarded return values in `core/src/vm.c` and the
    C test suite wrapped with explicit `(void)` casts.

- **`[[maybe_unused]]` on intentionally-unused parameters**: `snobol_jit_compile()`
  parameters `vm` and `start_ip` are `[[maybe_unused]]` on non-ARM64 builds,
  replacing the old `(void)vm; (void)start_ip;` suppression casts.

- **`constexpr` variables replacing typed `#define` constants**:
  - `core/src/table.c` — `FNV_OFFSET_BASIS`, `FNV_PRIME` → `constexpr uint32_t`
  - `core/src/vm.c` — `SNOBOL_LABEL_TABLE_MAGIC` → `constexpr uint32_t`
  - `core/src/jit.c` — `JIT_CACHE_MAX_HARD`, `JIT_CFG_MAX_BLOCKS`,
    `JIT_LOOP_ITER_MAX`, `MAX_OPS_IN_REGION` → `constexpr int`
  - `core/include/snobol/vm.h` — `MAX_CAPS`, `MAX_VARS`, `MAX_LOOPS` →
    `constexpr int`
  - Duplicate-definition guards (`#ifndef … #define … #endif`) added for
    constants shared across translation units (`SNOBOL_LABEL_TABLE_MAGIC`,
    `FNV_OFFSET_BASIS`, `FNV_PRIME`) so both standalone and amalgam builds work.

- **JIT A64 macros → typed `static inline` functions** (`core/src/jit.c`):
  - Four pure-constant `A64_*` macros converted to `constexpr uint32_t`
    (`A64_RET`, `A64_STP_X19_X30_PRE16`, `A64_LDP_X19_X30_POST16`,
    `A64_SUBS_X19_X19_1`).
  - Twenty-one argument-taking `A64_*` macros converted to
    `static inline uint32_t` functions with typed `uint32_t` parameters
    for register fields and immediates, catching argument-type errors at
    compile time. Call-site syntax is unchanged.

- **PHP binding `config.m4`**: `./configure` probes for compiler C standard
  support and passes the detected flag to `PHP_NEW_EXTENSION`.

### Verified

- All 220 PHP tests pass (`ddev test`) ✅
- All C tests pass ✅
- Zero `NULL` remaining in `core/src/*.c` ✅
- Zero `__typeof__` or `_Static_assert` in `core/` ✅

## [0.5.0] - 2026-05-03

### Added — Template & Substitution Completeness (template-substitution-completeness)

- **`SNBL_FMT_*` named constants** (`core/include/snobol/vm.h`): five `#define`
  constants replace bare integer discriminants in all template opcode handling:
  `SNBL_FMT_UPPER=1`, `SNBL_FMT_LOWER=2`, `SNBL_FMT_LENGTH=3`,
  `SNBL_FMT_LPAD=4`, `SNBL_FMT_RPAD=5`.  Also adds `SNBL_TABLE_ID_UNBOUND
  (0xFFFF)` sentinel and documents the extended `OP_EMIT_FORMAT` encoding for
  `SNBL_FMT_LPAD` / `SNBL_FMT_RPAD` (`reg u8, format_type u8, width u16,
  fill_char u8`) and the new `OP_EMIT_TABLE` name-bytes encoding.

- **`.lower()` template expression** (`core/src/compiler.c`): `${vN.lower()}`
  compiles to `OP_EMIT_FORMAT, reg, SNBL_FMT_LOWER`, enabling ASCII lowercase
  transformation entirely in the C runtime.

- **`.lpad(W[,'c'])` template expression** (`core/src/compiler.c`): `${vN.lpad(W)}`
  / `${vN.lpad(W,'c')}` compile to `OP_EMIT_FORMAT, reg, SNBL_FMT_LPAD, width_hi,
  width_lo, fill_char`.  Width is capped at 1024 in the VM.

- **`.rpad(W[,'c'])` template expression** (`core/src/compiler.c`): same as above
  but emits `SNBL_FMT_RPAD` for right-padding.

- **`snobol_template_bind_tables` API** (`core/include/snobol/compiler.h`,
  `core/src/compiler.c`): new public function that walks compiled template
  bytecode looking for `OP_EMIT_TABLE` entries with `table_id == 0xFFFF`
  (unbound), resolves the embedded name against a caller-supplied `names`/`ids`
  array, and patches the ID in-place.  Returns 0 on full success, -1 if any name
  is unresolvable.

- **`OP_EMIT_TABLE` name-encoding** (`core/src/compiler.c`, `core/src/vm.c`):
  `compile_template_to_bytecode` now writes `table_id=0xFFFF, key_type,
  name_len:u8, name_bytes[name_len]` before the key payload; the VM dispatch
  skips name bytes at runtime after `snobol_template_bind_tables` has resolved
  IDs; previously the table_id was always emitted as 0 with no name.

- **`OP_EMIT_EXPR` legacy alias** (`core/src/vm.c`): `OP_EMIT_EXPR` bytecode
  (old discriminants: 1=upper, 2=length) is mapped to the `OP_EMIT_FORMAT` path
  in the VM dispatch, preserving backward compatibility for any serialised
  patterns compiled with the previous compiler.

- **`Pattern::subst()` table binding** (`bindings/php/src/snobol_pattern.c`):
  `subst(subject, template, tables)` now accepts an optional array of
  `\Snobol\Table` objects; their names are resolved via `snobol_template_bind_tables`
  before execution, and they are registered in the VM table registry for
  `OP_EMIT_TABLE` dispatch.  Throws `\Exception` if any template
  table reference cannot be resolved.

- **PHP test suite** (`bindings/php/tests/php/TemplateOpsTest.php`): eight new
  integration tests covering `.lower()`, `.lpad(5,'0')`, `.rpad(8,'.')`, table-backed
  substitution, unregistered-table exception, and regression tests for `.length()`,
  `.upper()`, plain capture, and literal template.

- **C test suite** (`tests/c/test_template_ops.c`): ten new unit tests
  covering `.lower()`, `.lpad()`, `.rpad()`, no-op padding, graceful
  degradation for missing captures, `snobol_template_bind_tables` patching,
  unresolvable-name return value, end-to-end literal-key and capture-key table
  lookups, and legacy `OP_EMIT_EXPR` alias.

### Changed

- **`compile_template_to_bytecode` now uses `OP_EMIT_FORMAT`** instead of the
  legacy `OP_EMIT_EXPR` opcode for `.upper()` and `.length()` expressions.
  Any code that inspects raw template bytecode must recompile.  The VM still
  accepts old `OP_EMIT_EXPR` bytecode via the legacy alias.

- **`OP_EMIT_TABLE` bytecode layout changed**: a `name_len:u8 + name_bytes[]`
  field is now inserted between `key_type` and the key payload, and `table_id`
  is always written as `0xFFFF` (unbound) by the compiler.  Any previously
  serialised template bytecode that contains `OP_EMIT_TABLE` must be recompiled.

### Removed

- **Duplicate `compile_template_to_bytecode` in PHP binding**
  (`bindings/php/src/snobol_pattern.c`): the old PHP-side implementation (which
  lacked `.lower()`, `.lpad()`, `.rpad()` support and used the old
  `OP_EMIT_TABLE` encoding) has been removed.  All calls now route to the
  canonical core implementation via `compiler.h`.

### Versioning

- **Core library**: `SNOBOL_VERSION_MINOR` bumped from 2 → 3;
  `SNOBOL_VERSION_STRING` is now `"0.3.0"` (`core/include/snobol/snobol.h`).
- **CMake project**: bumped from `0.1.0` → `0.5.0` (`CMakeLists.txt`).
- **PHP binding**: `PHP_SNOBOL_VERSION` bumped from `"0.2.0"` → `"0.5.0"`
  (`bindings/php/src/php_snobol.h`).

## [0.4.0] - 2026-04-25

### Added — Labelled Control Flow (complete-labelled-control-flow)

- **`snobol_ast_create_goto`** (`core/src/ast.c`, `core/include/snobol/ast.h`):
  New AST creation function for `AST_GOTO` nodes, symmetrical with
  `snobol_ast_create_label`.
- **Parser: AST_GOTO emission** (`core/src/parser.c`): `parse_statement` now
  constructs an `AST_GOTO` node for `:(LABEL)` goto syntax and wraps it in a
  `concat([pattern, goto_node])` structure, making goto visible to the compiler.
- **Parser: duplicate-label detection** (`core/src/parser.c`): A `seen_labels`
  array tracks label names within each `snobol_parser_parse()` call; nested
  duplicate labels produce a parse error immediately.
- **Compiler: `AST_LABEL` emission** (`core/src/compiler.c`): `emit_node_c` now
  emits `OP_LABEL label_id` for label nodes, records the bytecode offset
  immediately after the instruction (the label's execution target), and detects
  duplicate definitions at compile time.
- **Compiler: `AST_GOTO` emission** (`core/src/compiler.c`): `emit_node_c` emits
  `OP_GOTO label_id` for goto nodes and marks the referenced label as needing a
  definition.
- **Compiler: unknown-label validation** (`core/src/compiler.c`): after all nodes
  are emitted, `compile_ast_to_bytecode_c` rejects any referenced label that was
  never defined.
- **Bytecode label table** (`core/src/compiler.c`, `core/src/vm.c`): a label
  offset table `[u32 × label_count, u32 label_count]` is appended to the end of
  the bytecode. `vm_exec` reads this table before `vm_run` and pre-registers
  all labels via `vm_register_label`, enabling forward goto references.
- **`get_ranges_ptr` updated** (`core/src/vm.c`): skips the new label table when
  computing the charclass section position so existing charclass-based patterns
  are unaffected.
- **C tests** (`tests/c/test_control_flow.c`): five new test functions covering
  duplicate-label detection via parser, duplicate-label detection via compiler,
  unknown-label detection via compiler, simple label pattern execution, and
  forward goto execution through the full pipeline.
- **PHP compatibility fixtures** (`tests/compat/fixtures/`):
  `WordCounterWithGoto`, `TextTransformerWithGoto`, `TemplateEngineWithGoto` —
  three new fixture classes demonstrating labelled control flow (`Builder::label`,
  `Builder::goto`) via the PHP binding API.
- **PHP compatibility tests** (`tests/compat/CompatibilityTest.php`): thirteen
  new test methods covering all three WithGoto fixtures.
- **PHP binding: `label`/`goto` AST conversion** (`bindings/php/src/snobol_pattern.c`):
  `php_ast_to_c` now handles `"label"` and `"goto"` array nodes from `Builder::label()`
  and `Builder::goto()`, wiring PHP-side construction to `snobol_ast_create_label` /
  `snobol_ast_create_goto` in the C core.
- **PHP Builder tests** (`bindings/php/tests/php/BuilderTest.php`): two additional
  test methods verifying the `label` and `goto` AST node shapes.
- **Test coverage**: 1,300 C tests (35 net-new in Control Flow suite) + 211 PHP tests
  pass; zero regressions.

## [0.3.0] - unreleased

### Added — Compact Backtracking (Phase 2)

- **Compact choice stack** (`core/src/vm.c`): default choice-stack mode now uses
  delta/write-log records storing only `(ip, pos, changed-capture-diff)` instead
  of full capture-array snapshots. Reduces per-choice memory by ≥50% for patterns
  with ≥10 choice points.
- **Write-log mechanism**: 64-entry circular buffer tracks capture modifications
  (`CAP_START`/`CAP_END`) with deduplication; entries compressed at choice point
  creation and replayed in reverse on backtrack.
- **Choice-stack statistics**: `vm_choice_stack_memory_usage()`,
  `vm_choice_stack_depth()`, `vm_choice_record_average_size()` expose runtime
  metrics for observability and testing.
- **Legacy mode**: set `SNOBOL_LEGACY_CHOICE=1` environment variable to restore
  full-snapshot behaviour for compatibility or benchmarking.
- **Test coverage**: all 1,265 C tests + 183 PHP tests pass in both compact and
  legacy modes; no regressions.

## [0.2.3] - 2026-04-22

### Added — jit-cfg-split (Phase 1c)

- **CFG-based multi-block JIT** (`core/src/jit.c`): replaced the single-basic-block
  linear pass-1 with a BFS CFG builder (`jit_cfg_build()`) that discovers up to
  `JIT_CFG_MAX_BLOCKS` (64) reachable blocks per compilation, following both SPLIT
  arms and forward JMPs.
- **Per-block stub emitter** (`snobol_jit_compile_cfg()`): allocates one contiguous
  ARM64 code buffer and emits a separate stub per CFG block in BFS order; stubs
  branch directly to each other via ARM64 `B imm26` — zero interpreter round-trips
  between blocks.
- **Forward-branch fixup pass**: after all stubs are emitted, resolves all
  placeholder `B(0)` instructions to their target stub addresses.
- **ARBNO / backward-edge loop guard**: backward JMP edges get a counted iteration
  guard using callee-saved register `x19` (initialised to `JIT_LOOP_ITER_MAX` = 1024
  per JIT entry); bails out to interpreter when the counter reaches zero, preventing
  infinite compiled loops.
- **`jit_blocks_compiled_total`** counter added to `SnobolJitStats`: cumulative count
  of CFG blocks emitted across all compilations; accessible via `snobol_jit_get_stats()`.
- **Zero-SPLIT fast path**: patterns with a single linear block and no backward edges
  continue to use the existing `op_seq[]` linear compiler path, preserving compile
  latency for straight-line regions.
- **Benchmark scenario** (`bench/tokenize.php`): added 3-arm delimiter scenario
  `',' | ';' | '|'` to exercise the new multi-block SPLIT chain path.
- **CFG unit tests** (`tests/c/test_jit_cfg.c`): 5 new test cases covering
  `jit_blocks_compiled_total` init, single-block counting, 3-arm SPLIT chain block
  discovery, SPLIT backtrack state restoration, and ARBNO loop compilation.

## [0.2.2] - 2026-04-20

### Added — SPLIT→ANY Fusion & Bitmap Optimization (Phase 1b)

- **Compile-time fusion pass** (`core/src/compiler.c`): `snobol_bc_fuse_split_any()` detects
  `OP_SPLIT a b` where both arms are a single `OP_LIT`/`OP_ANY`/`OP_NOTANY` followed
  by `OP_JMP` to the same merge point; rewrites to a single `OP_ANY` with a synthesised
  union charclass.
- **N-arm generalisation**: chained SPLIT chains over single-char arms are collapsed
  iteratively into one `OP_ANY`; `'a'|'b'|'c'` → one `OP_ANY`.
- **ASCII Bitmap Optimization** (`core/src/search.c`): added `OP_ANY` recognition in
  `snobol_search_derive_meta()`, routing fused patterns to Tier 3 (bitmap-accelerated)
  search path for O(match_count) execution.
- **JIT ARM64 Fusion Support**: `OP_ANY` already had a compiled path; fusion allows the
  fused op to run in JIT with zero choice-stack pressure.
- **Benchmark Result**: `tokenize_mixed` achieved ~1,600 ops/sec (≥3.4x baseline) with
  Choice Pushes reduced to 0.

## [0.2.0] - 2026-04-15

### Added

- **Pattern Primitives** – New VM opcodes and AST nodes for classic SNOBOL4 patterns:
  - `BREAKX` – pre-scan optimisation; O(n) advance to character-set boundary with retry choice point (8× fewer backtrack
    ops vs ARB)
  - `BAL` – balanced delimiter matching (configurable open/close characters)
  - `FENCE` – backtracking cut; prevents retrying the current choice point
  - `REM` – matches the remainder of the subject string
  - `RPOS(n)` – end-relative position (n codepoints from end)
  - `RTAB(n)` – end-relative tab (advance to n codepoints from end)
- **Built-in String Functions** (`core/src/string_fn.c`, `core/include/snobol/string_fn.h`):
  - `SIZE` – Unicode codepoint count with ASCII fast path
  - `TRIM` – trailing whitespace removal
  - `DUPL` – string repetition
  - `REVERSE` – Unicode codepoint-safe reversal (two-pass)
  - `SUBSTR` – codepoint-based substring (1-based positions)
  - `REPLACE` – all-occurrences substitution (≈ PHP `str_replace` speed)
  - `REPLACE_CHAR` – 256-byte lookup table character translation
  - `LPAD` / `RPAD` – Unicode-width-aware padding
  - `CHAR` – codepoint-to-UTF-8 conversion
  - `ORD` – UTF-8-to-codepoint conversion
  - `UPPER` / `LOWER` – case conversion (v1: ASCII a-z/A-Z; v2 Unicode planned)
- **Built-in Comparison Predicates** (`core/src/type_fn.c`, `core/include/snobol/type_fn.h`):
  - `IDENT` / `DIFFER` – string identity predicates
  - `LEXEQ` / `LEXLT` / `LEXGT` – lexicographic comparisons
  - `INTEGER` / `REAL` / `NUMERIC` – numeric type predicates
- **VM Built-in Dispatch** – `OP_EVAL` handler with function dispatch table; SNOBOL_TRACE logging; memory
  pre-allocation (20 KiB slab per match call)
- **PHP Binding** (`bindings/php/`):
  - `Snobol\Text` class with static methods mirroring all built-in functions
  - PHP pattern primitive wrappers: `Builder::breakx()`, `Builder::bal()`, `Builder::fence()`, `Builder::rem()`,
    `Builder::rpos()`, `Builder::rtab()`
  - 177 PHPUnit tests total (up from 122), all passing
- **C Test Suite**: 10 new test files covering all new built-ins and primitives
- **Benchmarks** (`bench/`):
  - `bench/tokenize.php` – BREAKX vs ARB comparison
  - `bench/transform.php` – built-in string function performance vs PHP native
  - `bench/unicode.php` – Unicode vs ASCII benchmark
  - `bench/results_builtin.json` – consolidated performance analysis
- **Examples**:
  - `examples/c/builtin_examples.c` – C API usage for all built-in functions
  - `examples/php/text_functions.php` – PHP API usage for all Text:: methods and pattern primitives

### Changed

- `core/src/compiler.c` – added `emit_breakx_c`, `emit_bal_c`, `emit_fence_c`, `emit_rem_c`, `emit_rpos_c`,
  `emit_rtab_c` emit helpers
- `core/include/snobol/ast.h` – added `AST_BREAKX`, `AST_BAL`, `AST_FENCE`, `AST_REM`, `AST_RPOS`, `AST_RTAB` enum
  values and union fields
- `core/src/ast.c` – added creator functions and free/name cases for new AST nodes
- `bindings/php/core_amalgam.c` – includes `string_fn.c`, `type_fn.c`, `pattern_build.c`
- PHP binding version bumped from 0.1.0 → 0.2.0

### Fixed

- PHP `PrimitivesTest` – capture/assign semantics (must use `Builder::assign` to expose `v{n}` in result)

### Performance Notes (v0.2.0)

| Scenario                   | SNOBOL         | PHP native               | Ratio          |
|----------------------------|----------------|--------------------------|----------------|
| `Text::replace` (9 KB)     | 614K ops/s     | 623K ops/s (str_replace) | **0.98×**      |
| `Text::upper/lower` (9 KB) | 3.4-3.7M ops/s | 3.8-4.0M ops/s           | **0.88-0.92×** |
| `Text::size` (Unicode)     | 910K ops/s     | 909K ops/s (mb_strlen)   | **1.00×**      |
| BREAKX choice pushes       | 1K/iter        | 8.3K/iter (ARB)          | **8.3× fewer** |

### Version Status

- **Core Library**: v0.4.0
- **PHP Binding**: v0.4.0
- **AST API**: v1.1.0 (new primitive nodes added, backwards compatible)

---

### Added

- **Monorepo Structure**: Language-agnostic core with separate bindings directories
- **Core C Library** (`core/`):
  - Complete lexer, parser, AST, compiler, VM implementation
  - Public API headers in `core/include/snobol/`
  - CMake build system with proper installation rules
  - Optional micro-JIT for ARM64
  - Associative tables for runtime lookups
  - Dynamic pattern evaluation with caching
- **PHP Binding** (`bindings/php/`):
  - Complete PHP extension with DDEV support
  - PHP helper classes (Pattern, PatternHelper, Builder, Table)
  - Full PHPUnit test suite (122 tests passing)
  - Native CMake build option
- **C Test Suite** (`tests/c/`):
  - 1,065+ tests covering all core functionality
  - JIT correctness and performance tests
  - Stress tests for backtracking and edge cases
- **Examples** (`examples/c/`):
  - Basic pattern matching example
  - Capture and assignment example
- **Documentation**:
  - Language-agnostic README.md
  - PHP-specific documentation in `bindings/php/README.md`
  - Updated CONTRIBUTING.md for monorepo structure
  - Updated ELEVATOR_PITCH.md and PITCH.md
- **CI/CD**:
  - GitHub Actions workflows for core (Linux, macOS, Windows)
  - PHP binding tests across PHP 8.0-8.5
  - AddressSanitizer and UBSan testing

### Changed

- **Repository Rename**: `snobol4-ddev` → `libsnobol4`
- **Project Structure**: Complete restructure to monorepo layout
  - Core C code moved from `snobol4-core/` to `core/`
  - PHP binding moved to `bindings/php/`
  - `.ddev/` moved to `bindings/php/.ddev/`
- **Build System**: Migrated from phpize to CMake
- **Include Paths**: Updated to namespaced `snobol/*.h` paths
- **AST API**: Full C AST compilation support
- **Template Compilation**: Full implementation for pattern replacements

### Fixed

- All PHP extension tests now pass (122/122)
- Capture and assign operations for all register numbers
- Template compilation for table-backed substitutions
- Emit literal and capture reference operations
- Dynamic pattern evaluation (EVAL)

### Removed

- Old `snobol4-core/` directory (merged into `core/`)
- Old `php-src/` directory (moved to `bindings/php/php-src/`)
- Old `.ddev/` at root (moved to `bindings/php/.ddev/`)
- PHP-coupled build system (replaced with CMake)

### Version Status

- **Core Library**: v0.1.0 (initial release)
- **PHP Binding**: v0.1.0 (initial release)
- **AST API**: v1.0.0 (stable)

---

- **Architecture**: Separated language-agnostic C core from language-specific bindings
- **Performance**: Eliminated PHP parser overhead (5-15% improvement for simple patterns)
- **Maintainability**: Single source of truth for grammar and parsing logic

### Removed

- `php-src/Lexer.php` - Replaced by C lexer
- `php-src/Parser.php` - Replaced by C parser
- `tests/php/ParserTest.php` - PHP parser tests no longer applicable

### Fixed

- PHP coupling in core - C core now has no dependencies on PHP internals
- Memory management - Proper ownership semantics for AST nodes

## Pre-1.0 (PHP-Coupled Architecture)

Before the language-agnostic core refactoring, the project used PHP-based lexer and parser
with C-based VM and compiler. This architecture was functional but made it difficult to
create bindings for other languages.

Key components:

- `php-src/Lexer.php` - PHP lexer
- `php-src/Parser.php` - PHP parser producing PHP arrays
- `snobol4-php/snobol_compiler.c` - Compiled PHP arrays to bytecode
- `snobol4-php/snobol_vm.c` - C VM for bytecode execution
