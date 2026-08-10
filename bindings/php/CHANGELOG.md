# Changelog

All notable changes to the libsnobol4 **PHP binding** are documented
in this file.  The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and the
binding adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Changes to the **C core** live in the repository root's
[CHANGELOG.md](../CHANGELOG.md).

Git tags for the binding are `php/vX.Y.Z` (Packagist/PIE derive the
package version from the tag).

## [Unreleased]

### Fixed

- **Stale Packagist branch alias** (`composer.json`): `dev-main` resolved
  as `0.2.x-dev`; now `1.x-dev` so `dev-main` consumers get correct
  stability and semver range matching.

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
- **Removed vestigial PHP binding code** (`bindings/php/src/`):
  `php_snobol_init_vm_for_search`, `php_snobol_emit_cb` + `EmitBuf`, the
  never-assigned `intern->dfa` field, `php_snobol_get_all_tables` /
  `php_snobol_get_all_arrays`, and `php_phelper_use_cache` had no callers
  after the persistent-search-state migration and were removed.
## [1.0.1] - 2026-08-02

### Fixed

- **PIE release asset naming matched exactly** (`.github/workflows/release.yml`):
  the published asset name pattern now matches what PIE expects, and
  **PHP 8.5 builds** were added to the release matrix.
- **`snobol/version.h` generated at configure time when missing**
  (`bindings/php/config.m4`): the PHP binding's build no longer requires a
  pre-existing generated header, fixing out-of-tree configure runs.
## [1.0.0] - 2026-08-02

### Fixed

- **Windows CI builds the PHP extension via a direct dev-pack build**
  (`.github/workflows/`): the php-windows-builder action was replaced with
  the native Windows dev-pack flow, and the Windows `arch` input now uses
  `x64` (was `x86_64`).
- **PIE builder upgraded to `pie-ext-binary-builder@0.0.3`** with a
  `build-path` input so the extension binary lands at the expected path.
## [0.13.0] - 2026-07-28

### Lean Tokenize API

#### Added

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
### PHP Match Routing and Per-Call Optimization

#### Added

- **`php_snobol_do_match()`** (`bindings/php/src/snobol_pattern.c`, `bindings/php/src/php_snobol.h`): Routes `Pattern::match()` through the search-tier dispatch path (the same pipeline used by `searchAll`/`searchSplit`), adding the required-byte prefilter, search-VM accelerators, and automaton/Tier 7 offload to the PHP first-match path.
- **`eval_callbacks` cache on PHP `snobol_pattern_t`** (`bindings/php/src/php_snobol.h`, `bindings/php/src/snobol_pattern.c`): `setEvalCallbacks()` now stores the callbacks array on the pattern; the callback function pointer is passed to the persistent VM via the state setter, eliminating per-call allocation.
#### Changed

- **`Pattern::match()` now uses persistent search state** (`bindings/php/src/snobol_pattern.c`): The match method no longer creates a stack VM, EmitBuf, dyn_cache, or table binding per call. Instead it calls `php_snobol_do_match()` which reuses `intern->search_state` and the tier dispatch. Simple patterns (span, break, alternation, automaton) improved from ~100–1,000× PHP/C ratio to 1.6–3.2×. The existing `memcmp` literal fast path is preserved.
- **`setEvalCallbacks()` stores the callbacks array** (`bindings/php/src/snobol_pattern.c`): Previously a no-op; now saves the array on the pattern struct for use by the persistent VM's eval callback dispatcher.
### Probe Truth and Performance Fairness

#### Added

- **Persistent search state on PHP `snobol_pattern_t`** (`bindings/php/src/php_snobol.h`, `bindings/php/src/snobol_pattern.c`): `snobol_pattern_search_state_t *search_state` is lazily created on first search call, reused by all search methods (`searchAll`, `searchSplit`, `searchSplitOffsets`, `searchSplitCuts`, `searchReplace`), destroyed in the PHP dtor. Both the batch fast path and the per-call fallback use the same persistent state.
- **Aligned C/PHP probe scenarios** (`bench/c/bench_probe.c`, `bindings/php/probe.php`): All-matches scenarios (`*_all`, unit=pass), per-pass tokenize rows, canonicalized subjects, `unit` column in probe output tables.
#### Changed

- **PHP binding uses `snobol_pattern_search_batch_ex` with persistent state** (`bindings/php/src/snobol_pattern.c`): The batch fast path and fallback loop share one persistent `snobol_pattern_search_state_t` per PHP `Pattern` object. DFA, range_meta, and trie caches are built once per pattern lifetime. Heavy zero-match `searchAll` rows collapsed ~105 µs → 208 ns.
- **Flat result modes go through the batch fast path** (`bindings/php/src/snobol_pattern.c`): `result=>'flat'` and `result=>'offsets'` now route through the same stateful batch path as the default mode, with identical result shapes.
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
#### Changed

- **Literal fast-path restored** (`bindings/php/src/snobol_pattern.c`): The direct-`memcmp` literal fast-path in `match()` was restored (previously removed as "redundant" in P4). The C-core Tier 2 dispatcher is correct but adds ~300 ns due to `search_reset_vm` + prefilter overhead. The direct path returns in ~20 ns.
- **`Pattern::match()` signature** (`bindings/php/src/snobol_pattern.c`): Accepts optional `array $options = []` parameter.
#### Fixed

- **Literal fast-path bypass** (`bindings/php/src/snobol_pattern.c`): Literal fast-path now respects `$options['metrics' => true]` and emits the `_metrics` hash when requested.
### Core Batch-Search API

#### Added

- **Batch-search integration in PHP binding** (`bindings/php/src/snobol_pattern.c`): `searchAll()`, `searchSplit()`, `searchSplitOffsets()`, `searchSplitCuts()`, and `searchReplace()` try the batch API first; fall back to per-call loop for ineligible patterns. Eligible patterns complete in a single C pass with no per-match API overhead.
- **PHP test suite** (`bindings/php/tests/php/BatchSearchTest.php`): 17 tests covering searchAll, searchSplit, searchReplace, captures, and edge cases on both eligible and ineligible patterns.
### Search Engine Optimization

#### Performance

- PHP test suite: **319/319** tests, **621** assertions pass.
#### Fixed

- **PHP whole-file segfault (exit 139)** (`core/src/api.c`): `snobol_pattern_search_ex` cached the DFA on `state->pattern`, but the PHP-side `snobol_pattern_t` lacks the `automaton`/`trie_cache` fields that the core `struct snobol_pattern` carries. `snobol_pattern_get_automaton` then read uninitialized bytes at the wrong offset, yielding a garbage `dfa` pointer that crashed `search_automaton_exec`. Fixed by caching the DFA on the search *state* (`state->dfa`) instead of the pattern; freed in `snobol_pattern_search_state_destroy`. Both core and PHP patterns work via their respective accessors. (Root-caused and fixed as part of `search-perf-measured-wins`; verified by whole-file `ConvenienceApiTest.php` / `PatternTest.php` now passing, full PHP suite green.)
### OSS Readiness (library-grade hygiene)

#### Verified

- Core C suite: **2166 tests** pass (Release and ASan+UBSan); PHP binding suite: **315 PHPUnit tests** pass (incl. the C/PHP coupling probe). Both gated green after the TU modularization.
## [0.12.0] - 2026-07-08

### Added

- **`Pattern::searchSplitOffsets()`** (`bindings/php/src/snobol_pattern.c`): New PHP method returning `[[offset, length], ...]` pairs — zero zend_string allocation for token data. Single-pass offset recording with packed array construction. ~1.47× faster than pre-change `searchSplit` on delimiter-heavy scenarios.
### Added

- **PHP literal fast-path** (`bindings/php/src/snobol_pattern.c`): `Pattern::match()` now detects literal-only patterns via `snobol_search_derive_meta()` and bypasses VM setup entirely, calling `snobol_pattern_match_literal()` inline. Zero-allocation path for pure-literal patterns.
- **PHP `Pattern::matchLiteral()`** (`bindings/php/src/snobol_pattern.c`): new public method returning `{success, position, length}` associative array. Delegates directly to C `snobol_pattern_match_literal()`.
- **`ddev test-c-probe`** DDEV command (`.ddev/commands/web/test-c-probe`): runs only the `@group coupling-probe` filter. `CPhpCouplingTest` excluded from default `ddev test` (marked with `@group coupling-probe` and excluded in `phpunit.xml`).
### Changed

- **PHP binding** (`bindings/php/src/`): Removed all JIT blocks from `php_snobol.c`, `snobol_pattern.c`, `php_snobol.h`. `config.m4` JIT defines removed. `build-snobol-extension` `deps/` copy made conditional.
- **`CPhpCouplingTest`**: Renamed from `JitCPhpCouplingTest`, JIT assertions removed, compares `alt_literals` ratio instead of `tokenize`. Marked `@group coupling-probe`, excluded from default `ddev test`.
### SLJIT Method JIT & Tracing-JIT Retirement — 2026-06-27 [0.11.0]

### Removed

- **Tracing-JIT PHP binding code**: all `jit_ctx`/`ip_counts`/`traces`/`ctx`/
  `search_mode` references removed from `snobol_pattern.c` and `php_snobol.h`.
### searchSplit Bulk-Result Buffer — 2026-06-20 [0.11.0]

### Added

- **`Pattern::searchSplit` two-pass bulk path** (`bindings/php/src/snobol_pattern.c`):
  for large subjects (`>= 1 MB`) the binding now runs a pre-pass to count matches,
  allocates one C buffer of `subject_len + 1` bytes, `memcpy`s every segment from
  the subject into the buffer at a running cursor, wraps the buffer in a single
  parent `zend_string`, and inserts N+1 child zend_strings via `zend_string_init`
  over sub-ranges. Extracted into `snobol_searchsplit_bulk_path` so the small-
  subject fast path's I-cache footprint is unchanged.
### Changed

- **Threshold-based dispatch** in `PHP_METHOD(Snobol_Pattern, searchSplit)`:
  the `PHP_METHOD` body is now a thin dispatcher (`if (subject_len < THRESHOLD)
  fast_path else bulk_path`), keeping the per-call hot path small.
- **Probe before/after numbers**:
  C `tokenize` 397 → 385 ns/iter (-3.0%), PHP `tokenize_php` 98,445 → 97,341
  ns/iter (-1.1%), `JitCPhpCouplingTest` unchanged (4 tests, 22 assertions).
### JIT Search Performance Baseline — 2026-06-20 [0.11.0]

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
### Diagnostic Probe — 2026-06-20 [0.11.0]

### Added

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
### Binding Performance & Range Syntax — 2026-06-20 [0.11.0]

### Added

- **PatternHelper pattern cache** (`bindings/php/src/snobol_pattern_helper_php.c`):
  fixed-size cache integrates `PatternCache` into
  `PatternHelper::matchOnce`/`matchAll` so string patterns are compiled
  once and reused. Eliminates per-call lex→parse→compile overhead.
- **VM reuse in `searchAll`** (`bindings/php/src/snobol_pattern.c`):
  refactored to reuse the VM struct, choice stack, and dynamic pattern
  cache across iterations instead of allocating/zeroing/freeing per match.
  Batch results are produced as C arrays and converted to PHP once.
- **Builder range notation** (`bindings/php/src/snobol_pattern_php.c`):
  `Builder::span()`, `Builder::brk()`, `Builder::any()`, `Builder::notany()`
  accept range notation (`A-Z`, `0-9`); ranges are expanded at the
  PHP-to-C AST conversion layer. All `docs/why-snobol-vs-pcre.md`
  examples updated to use range syntax where appropriate.
- **Range syntax tests** (`bindings/php/tests/php/BuilderTest.php`):
  edge cases (literal hyphens, uppercase ranges, mixed range+literal
  chars) covered.
### Core Primitives & Builtins — 2026-06-15 [0.11.0]

### Added

- **PHP Builder methods**: `Builder::pos(n)`, `Builder::tab(n)`, `Builder::abort()`,
  `Builder::fail()`, `Builder::succeed()` with `php_ast_to_c` conversion.
- **PHP `snobol_text_eq/ne/lt/gt/le/ge/integer()` functions** exposed as
  `Snobol\Text::*` PHP callables.
- **PHP tests**: `tests/php/PrimitivesTest.php` (90 tests), `tests/php/ComparisonsTest.php` (183 tests).
### Array Data Type — 2026-06-16 [0.11.0]

### Added

- **PHP `Snobol\Array_` class** (`bindings/php/src/snobol_array_php.c`, `bindings/php/src/snobol_array_php.h`):
  C extension class with `get()`, `set()`, `delete()`, `size()`, `keys()`, `values()` methods.
  PHP stub in `bindings/php/php-src/Array_.php` (IDE autocomplete).
- **`Builder::arrayRef()` method** for constructing array-access sub-patterns.
- **PHP tests**: `tests/compat/ArrayTest.php` (176 assertions) covering all `Snobol\Array_`
  operations including edge cases (empty array, single element, many elements, deletion).
### Full BMP Unicode — 2026-06-16 [0.11.0]

### Added

- **PHP `Snobol\Text::upper()` / `Snobol\Text::lower()`** now delegate to the BMP-aware
  C implementation (was previously ASCII-only).
- **PHP tests**: `tests/php/UnicodeTest.php` (37 tests) covering BMP UPPER/LOWER with
  Greek, Cyrillic, CJK, and mixed-script strings.
### Convenience API for PHP binding — 2026-06-18 [0.11.0]

### Added

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
## [0.7.0] - 2026-05-20

### Added

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

- All PHP tests pass (236/236) ✅
## [0.6.0] - 2026-05-10

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
### Changed

- **PHP binding `config.m4`**: `./configure` probes for compiler C standard
  support and passes the detected flag to `PHP_NEW_EXTENSION`.
### Verified

- All 220 PHP tests pass (`ddev test`) ✅
## [0.5.0] - 2026-05-03

### Added — Template & Substitution Completeness (template-substitution-completeness)

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
### Removed

- **Duplicate `compile_template_to_bytecode` in PHP binding**
  (`bindings/php/src/snobol_pattern.c`): the old PHP-side implementation (which
  lacked `.lower()`, `.lpad()`, `.rpad()` support and used the old
  `OP_EMIT_TABLE` encoding) has been removed.  All calls now route to the
  canonical core implementation via `compiler.h`.
### Versioning

- **PHP binding**: `PHP_SNOBOL_VERSION` bumped from `"0.2.0"` → `"0.5.0"`
  (`bindings/php/src/php_snobol.h`).
## [0.4.0] - 2026-04-25

### Added — Labelled Control Flow (complete-labelled-control-flow)

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

- **Test coverage**: all 1,265 C tests + 183 PHP tests pass in both compact and
  legacy modes; no regressions.
## [0.2.0] - 2026-04-15

### Added

- **PHP Binding** (`bindings/php/`):
  - `Snobol\Text` class with static methods mirroring all built-in functions
  - PHP pattern primitive wrappers: `Builder::breakx()`, `Builder::bal()`, `Builder::fence()`, `Builder::rem()`,
    `Builder::rpos()`, `Builder::rtab()`
  - 177 PHPUnit tests total (up from 122), all passing
### Changed

- `bindings/php/core_amalgam.c` – includes `string_fn.c`, `type_fn.c`, `pattern_build.c`
- PHP binding version bumped from 0.1.0 → 0.2.0
### Fixed

- PHP `PrimitivesTest` – capture/assign semantics (must use `Builder::assign` to expose `v{n}` in result)
### Version Status

- **PHP Binding**: v0.4.0
### Added

- **PHP Binding** (`bindings/php/`):
  - Complete PHP extension with DDEV support
  - PHP helper classes (Pattern, PatternHelper, Builder, Table)
  - Full PHPUnit test suite (122 tests passing)
  - Native CMake build option
### Changed

- **Repository Rename**: `snobol4-ddev` → `libsnobol4`
- **Build System**: Migrated from phpize to CMake
### Fixed

- All PHP extension tests now pass (122/122)
### Removed

- Old `php-src/` directory (moved to `bindings/php/php-src/`)
- Old `.ddev/` at root (moved to `bindings/php/.ddev/`)
### Version Status

- **PHP Binding**: v0.1.0 (initial release)
- **Performance**: Eliminated PHP parser overhead (5-15% improvement for simple patterns)
### Removed

- `php-src/Lexer.php` - Replaced by C lexer
- `php-src/Parser.php` - Replaced by C parser
- `tests/php/ParserTest.php` - PHP parser tests no longer applicable
### Fixed

- PHP coupling in core - C core now has no dependencies on PHP internals
- `php-src/Lexer.php` - PHP lexer
- `php-src/Parser.php` - PHP parser producing PHP arrays
