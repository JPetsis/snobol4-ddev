# libsnobol4

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![Core CI](https://github.com/JPetsis/libsnobol4/actions/workflows/ci-core.yml/badge.svg)](https://github.com/JPetsis/libsnobol4/actions/workflows/ci-core.yml)
[![PHP CI](https://github.com/JPetsis/libsnobol4/actions/workflows/ci-php.yml/badge.svg)](https://github.com/JPetsis/libsnobol4/actions/workflows/ci-php.yml)
[![Sanitizers](https://github.com/JPetsis/libsnobol4/actions/workflows/sanitizers.yml/badge.svg)](https://github.com/JPetsis/libsnobol4/actions/workflows/sanitizers.yml)
[![Valgrind](https://github.com/JPetsis/libsnobol4/actions/workflows/valgrind.yml/badge.svg)](https://github.com/JPetsis/libsnobol4/actions/workflows/valgrind.yml)
[![Fuzz](https://github.com/JPetsis/libsnobol4/actions/workflows/fuzz.yml/badge.svg)](https://github.com/JPetsis/libsnobol4/actions/workflows/fuzz.yml)
[![Coverage C](https://img.shields.io/codecov/c/github/JPetsis/libsnobol4?flag=c-core&label=coverage%20%28C%29)](https://codecov.io/gh/JPetsis/libsnobol4)
[![Coverage PHP](https://img.shields.io/codecov/c/github/JPetsis/libsnobol4?flag=php-binding&label=coverage%20%28PHP%29)](https://codecov.io/gh/JPetsis/libsnobol4)
[![Docs](https://github.com/JPetsis/libsnobol4/actions/workflows/docs.yml/badge.svg)](https://JPetsis.github.io/libsnobol4/)
[![Packagist](https://img.shields.io/packagist/v/libsnobol4/snobol.svg)](https://packagist.org/packages/libsnobol4/snobol)
[![Packagist Downloads](https://img.shields.io/packagist/dt/libsnobol4/snobol.svg)](https://packagist.org/packages/libsnobol4/snobol)
[![GitHub Release](https://img.shields.io/github/v/release/JPetsis/libsnobol4.svg)](https://github.com/JPetsis/libsnobol4/releases)

A high-performance C library implementing [SNOBOL4](https://en.wikipedia.org/wiki/SNOBOL)-style string pattern
matching and manipulation — a **PCRE alternative** for complex string processing.

libsnobol4 provides a powerful, expressive alternative to PCRE (Perl Compatible Regular Expressions) for complex string
manipulation tasks. The core library is language-agnostic, with officially maintained bindings for **C** and **PHP**.
Additional language bindings (Python, Rust, Go, etc.) are community contributions — see
[CONTRIBUTING.md](CONTRIBUTING.md) for a minimal binding checklist and the Python reference prototype.

> **Official scope:** C engine + PHP binding only. See [CONTRIBUTING.md](CONTRIBUTING.md) for community binding guidance.

## Quick Install

The fastest way to use libsnobol4 from PHP:

```bash
pie install libsnobol4/snobol
```

This installs the native `snobol` PHP extension — PIE downloads a pre-built
binary for your platform, or falls back to building from source automatically.

| What you need                  | Command                                                                                |
|--------------------------------|----------------------------------------------------------------------------------------|
| **PHP extension** (recommended) | `pie install libsnobol4/snobol` — single command, binary or source                     |
| **C library on macOS**         | `brew install JPetsis/homebrew-tap/libsnobol4`                                         |
| **C library, any platform**    | `cmake -B build && cmake --build build && cmake --install build`                       |

See [Distribution](#distribution) for details and alternative channels.

## Features

### Core Library (C)

* **Language-Agnostic Core**: Pure C implementation with no external dependencies
* **Robust Backtracking Engine**:
  * **Catastrophic Backtracking Protection**: Detects and prevents infinite loops in nested zero-width matches
  * **Deep Recursion**: Dynamically growable choice stack handles deeply nested patterns without crashing
  * **Compact Choice Stack**: Trail / undo-log choice save (default) stores only `ip`, `pos`, and a trail index per choice point — O(1) push regardless of loop/emit count; records live in a page-linked arena. Legacy full-snapshot mode remains available via `SNOBOL_LEGACY_CHOICE=1`
* **Rich Pattern Primitives**:
  * **Literals**: Exact string matching
  * **Concatenation & Alternation**: Sequential and alternative pattern composition
  * **Span & Break**: Character class matching
  * **Any & NotAny**: Single character matching
   * **Unicode Support**: Full UTF-8 with BMP case folding (U+0000–U+FFFF)
  * **Arbno & Bounded Repetition**: Variable-length pattern repetition
  * **Anchors**: Start/end of string anchors
  * **BREAKX**: Character-set break with O(n) pre-scan optimisation (8 fewer backtracks vs ARB)
  * **BAL**: Balanced delimiter matching (e.g. nested parentheses)
  * **FENCE**: Backtracking cut (prevents retrying a choice point)
  * **REM**: Match remainder of subject string
  * **RPOS / RTAB**: End-relative position and tab patterns
  * **Labelled Control Flow** (v0.4.0): Named labels and `goto` transfers within a pattern.
    Labels are compile-time names that resolve to bytecode offsets; `goto` is explicit control
    flow that does **not** pop the backtracking choice stack (distinguishing it from backtracking).
    Duplicate-label and unknown-label references are detected at compile time.
* **Template Substitution** (v0.5.0): `Pattern::subst(subject, template[, tables])` replaces
  every match in `subject` using a template string where capture references and formatting
  expressions are compiled entirely to C VM instructions.  Supported expressions inside `${vN...}`:
  * `${vN}` — raw capture
  * `${vN.upper()}` — Unicode uppercase (full BMP; German ß→SS)
  * `${vN.lower()}` — Unicode lowercase (full BMP)
  * `${vN.length()}` — decimal codepoint count
  * `${vN.lpad(W[,'c'])}` — left-pad to width W (fill char defaults to space)
  * `${vN.rpad(W[,'c'])}` — right-pad to width W (fill char defaults to space)
  * `$TABLE['key']` / `$TABLE[vN]` — table-backed lookup (fully compiled; no PHP post-processing)
* **Captures & Assignments**: Register-based capture and variable assignment
* **Associative Tables**: Runtime-owned hash tables for key-value storage
* **Dynamic Pattern Evaluation**: Runtime pattern compilation with caching (`EVAL(...)`)
* **Built-in String Functions** (v0.2.0): C-native string manipulation on UTF-8 strings:
  * **SIZE** – Unicode codepoint count (ASCII fast path)
  * **TRIM** – Remove trailing whitespace
  * **DUPL** – Repeat a string N times
  * **REVERSE** – Reverse by codepoints (multi-byte safe)
  * **SUBSTR** – Codepoint-based substring extraction (1-based)
*   **REPLACE** – Replace all occurrences (≈ PHP `str_replace` speed)
   *   **REPLACE_CHAR** – Character translation table (like POSIX `tr`)
   *   **LPAD / RPAD** – Pad strings to a Unicode codepoint width
   *   **CHAR / ORD** – Codepoint ↔ UTF-8 character conversions
   *   **UPPER / LOWER** – Full BMP Unicode case conversion (Cyrillic, Greek, Arabic, CJK, etc.; German sharp-s ß→SS)
*   **Case-Insensitive Pattern Matching** (v0.11.0): `SNOBOL_FLAG_CASE_INSENSITIVE` covers the full Basic Multilingual Plane (BMP, U+0000–U+FFFF).
*   **Numeric Comparison Functions** (v0.11.0): **EQ**, **NE**, **LT**, **GT**, **LE**, **GE** — alongside existing IDENT/DIFFER/LEXEQ/LEXLT/LEXGT.
*   **POS / TAB** (v0.11.0): Start-relative positional primitives.
*   **ABORT / FAIL / SUCCEED** (v0.11.0): Pattern-level control verbs.
*   **ARRAY Data Type** (v0.11.0): Indexed sparse array with integer keys.
*   **ABI Stability Policy** (v0.11.0): `snobol_get_abi_version()` returns the monotonically-increasing ABI version (initial value `1`). Load-time compatibility checks MUST compare this at runtime. Deprecated functions are marked with `SNOBOL_DEPRECATED` and remain available for one minor-version cycle.
*   **Thread Safety** (v0.11.0): VMs and pattern compilation are fully reentrant. See "Thread Safety" section below for per-component guarantees.
*   **API Version Function** (v0.7.0): `snobol_get_api_version()` returns `(MAJOR << 16) | (MINOR << 8) | PATCH` for binding/library compatibility checks.
*   **Built-in Comparison Predicates** (v0.2.0): Boolean predicates matching SNOBOL4 semantics:
   *   **IDENT / DIFFER** – String identity / difference
   *   **LEXEQ / LEXLT / LEXGT** – Lexicographic comparisons
   *   **INTEGER / REAL / NUMERIC** – Numeric type predicates
* **Modern C Code Quality** (v0.6.0): Core adopts `nullptr`, `SNOBOL_NODISCARD`, `[[maybe_unused]]`, and `constexpr` with MSVC-compatible fallbacks throughout.
* **DFA Automaton** (v0.11.0): NFA-to-DFA subset construction for automaton-eligible patterns. Tier 7 in search dispatch. Handles LIT, LEN, SPAN, BREAK, ANY, NOTANY with epsilon closure for SPLIT/JMP. State explosion cap at 4096 states.
* **Literal-Match API** (v0.11.0): `snobol_pattern_match_literal()` for zero-allocation anchored literal matching. Returns lightweight struct by value.
* **Profiling Support**: Built-in execution profiling for performance analysis

### Available Bindings

| Binding                       | Status   | Version  |
|-------------------------------|----------|----------|
| [PHP](bindings/php/README.md) | ✅ Stable | v1.0.2  |

## Project Structure

```
libsnobol4/
├── core/                    # Language-agnostic C core library
│   ├── CMakeLists.txt       # Core build configuration
│   ├── include/snobol/      # Public API headers
│   ├── src/                 # Core implementation
│   └── grammar/             # SNOBOL pattern grammar (EBNF)
├── bindings/                # Language-specific bindings
│   └── php/                 # PHP binding
│       ├── CMakeLists.txt   # PHP binding build
│       ├── src/             # PHP extension source
│       ├── php-src/         # PHP helper classes
│       └── tests/           # PHPUnit tests
├── tests/c/                 # Core C test suite
├── examples/c/              # C usage examples
├── CMakeLists.txt           # Root build configuration
└── README.md                # This file
```

## Quick Start

### Building the Core Library

```bash
# Configure
cmake -B build

# Build
cmake --build build

# Install (optional)
cmake --install build
```

### Using the Core Library (C)

#### One-shot convenience API (v0.11.0+)

```c
#include <stdio.h>
#include <snobol/snobol.h>

int main(void) {
    snobol_match_result_t *r = snobol_match(
        "'abc' ARB 'def'", 15,
        "xyz abc def xyz",  15,
        0);
    if (r && r->success) {
        printf("Match succeeded\n");
        for (int i = 0; i < r->capture_count; i++) {
            if (r->captures[i]) {
                printf("  capture %d: %s\n", i, r->captures[i]);
            }
        }
        if (r->output) {
            printf("  output: %s\n", r->output);
        }
    }
    snobol_match_result_free(r);
    return 0;
}
```

The function returns a heap-allocated result.  Always check `r->success` (true = match,
false = no match) and `r->error` (NULL on success, otherwise an error message string).  Captures
are 0-indexed; `r->captures[i]` is the value bound to positional capture `i` from the pattern
(where `i = 0` is the first capture).  Free the result with `snobol_match_result_free()`.

#### Literal-match API (zero-allocation, v0.13.0+)

For patterns that are pure literals (e.g., `'abc'`), use `snobol_pattern_match_literal()` for
zero-allocation anchored matching. Returns a lightweight struct by value with `success`,
`position`, and `length` fields. No VM setup, no heap allocations.

```c
#include <snobol/snobol.h>

int main(void) {
    snobol_context_t* ctx = snobol_context_create();
    snobol_pattern_t* pat = snobol_pattern_compile(ctx, "'hello'", 7, NULL);
    
    snobol_literal_match_t r = snobol_pattern_match_literal(pat, "hello world", 11);
    if (r.success) {
        printf("Matched at position %zu, length %zu\n", r.position, r.length);
    }
    
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return 0;
}
```

For non-literal patterns, `snobol_pattern_match_literal()` returns `{false, 0, 0}` immediately.

#### Multi-step API (fine-grained control)

```c
#include <snobol/snobol.h>

int main(void) {
    snobol_context_t* ctx = snobol_context_create();
    snobol_pattern_t* pattern = snobol_pattern_compile(ctx, "'hello'", 7, NULL);
    snobol_match_result_t res;
    snobol_match(pattern, "hello world", 11, &res);
    if (res.status == SNOBOL_MATCH_SUCCESS) {
        printf("Match found!\n");
    }
    snobol_match_result_free(&res);
    snobol_pattern_free(pattern);
    snobol_context_destroy(ctx);
    return 0;
}
```

#### Builder API (programmatic pattern construction)

For patterns composed from named, maintainable pieces instead of source
strings, build an AST with `snobol_pattern_build_*()` and compile it in one
call with `snobol_pattern_build_compile()`:

```c
#include <snobol/snobol.h>

int main(void) {
    snobol_context_t* ctx = snobol_context_create();
    snobol_pattern_build_t* b = snobol_pattern_build_create();

    ast_node_t* lit = snobol_pattern_build_lit(b, "hello", 5);
    ast_node_t* root = snobol_pattern_build_emit(b, lit);

    char* err = NULL;
    snobol_pattern_t* pat = snobol_pattern_build_compile(ctx, root, 0, &err);
    if (!pat) {
        fprintf(stderr, "compile failed: %s\n", err ? err : "unknown");
        free(err);
        return 1;
    }
    // root is consumed by compile; the builder can be reused or destroyed.
    snobol_pattern_build_destroy(b);

    snobol_match_t* m = snobol_pattern_search(pat, "say hello world", 15);
    if (m && snobol_match_success(m)) {
        printf("matched at offset %zu\n", snobol_match_get_position(m));
    }
    snobol_match_free(m);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return 0;
}
```

The AST root's ownership transfers to `snobol_pattern_build_compile()` (it is
freed on both success and failure); the returned pattern matches identically
to the same pattern compiled from source and frees with
`snobol_pattern_free()`.

#### Using from C++

All public headers are wrapped in `extern "C"` guards, so libsnobol4 can be
consumed directly from C++ with no shim. Include `<snobol/snobol.h>` and link
against the static or shared library as usual:

```cpp
#include <snobol/snobol.h>

int main() {
    snobol_context_t* ctx = snobol_context_create();
    snobol_pattern_t* pat = snobol_pattern_compile(ctx, "'hello'", 7, nullptr);
    // ... use the C API from C++ ...
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}
```

Every public header also compiles standalone (each pulls in its own
dependencies), and a CI job compiles the full header set as C++ with both
`g++` and `clang++` to keep the interop guarantee green.

### Using the PHP Binding

```bash
cd bindings/php
ddev start
```

```php
<?php
use Snobol\PatternHelper;

// Match pattern
$result = PatternHelper::matchOnce("'hello'", "hello world");

// Using Builder API
use Snobol\Builder;
$pattern = Builder::lit("hello");
$result = PatternHelper::matchOnce($pattern, "hello world");
```

See [bindings/php/README.md](bindings/php/README.md) for detailed PHP documentation.

## Build Options

| Option               | Default | Description                                                                                              |
|----------------------|---------|----------------------------------------------------------------------------------------------------------|
| `BUILD_TESTS`        | ON      | Build C test suite                                                                                       |
| `BUILD_PHP`          | OFF     | Build PHP binding                                                                                        |
| `BUILD_SHARED_LIBS`  | OFF     | Build shared library                                                                                     |
| `SNOBOL_PROFILE`     | OFF     | Enable VM profiling                                                                                      |
| `SNOBOL_SANITIZE`    | OFF     | AddressSanitizer + UBSan (GCC/Clang)                                                                     |

### Example Configurations

```bash
# Debug build with tests
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON

# Build with PHP binding
cmake -B build -DBUILD_PHP=ON

# Release build with profiling
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSNOBOL_PROFILE=ON

# ASan + UBSan build (GCC or Clang required)
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSNOBOL_SANITIZE=ON
cmake --build build-asan --target test-asan
```

### Windows

```bash
# Visual Studio 2022
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# MinGW-w64
cmake -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure
```

CMake presets are available via `CMakePresets.json`:

```bash
cmake --preset asan   # ASan + UBSan
cmake --preset debug  # Debug build
cmake --preset release
```

## Testing

### Core C Tests

```bash
# Run all tests
ctest --test-dir build

# Run with verbose output
ctest --test-dir build --verbose

# Run specific test
ctest --test-dir build -R test_lexer

# Run with AddressSanitizer + UBSan
make build-asan
make test-asan

# Run under Valgrind (memcheck)
make test-valgrind
```

### PHP Tests

```bash
cd bindings/php
composer install
vendor/bin/phpunit tests/php
```

### Fuzzing (libFuzzer)

Crash-detection and differential targets (build with `-DSNOBOL_FUZZ=ON`,
requires Clang):

```bash
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DSNOBOL_FUZZ=ON
cmake --build build-fuzz
./build-fuzz/tests/fuzz/fuzz_compiler -max_total_time=600
./build-fuzz/tests/fuzz/fuzz_vm -max_total_time=600
./build-fuzz/tests/fuzz/fuzz_oracle -max_total_time=600
```

- `fuzz_compiler`, `fuzz_vm` — crash/leak/OOB detection under ASan
- `fuzz_oracle` — **differential**: runs the search-tier dispatch and the
  reference VM on the same pattern+subject and reports any disagreement in
  success, position, or length — turning the fuzzer from a crash finder into
  a wrong-answer finder for search-engine optimizations

The C suite's deterministic equivalent (`test_search_oracle.c`, pattern
corpus in `tests/c/corpus.h`) checks the same accelerated-vs-reference
equivalence on every `make test` run, so regressions are caught in CI without
fuzzing.

## Performance

libsnobol4 is designed for high-performance string processing:

- **Streaming Substitution**: Native C implementation minimizes data copying
- **Pattern Caching**: Compiled patterns are cached for efficient reuse
- **Multi-Tier Search Engine**: Cost-model-driven auto-selection of the optimal strategy (literal memcmp, BMH skip, cached trie for bushy alt-of-literals, DFA automaton, SIMD NFA). Required-byte pre-filter eliminates tier dispatch entirely when a necessary literal is absent. Thread-buffer overflow fallback prevents silent false negatives in pike_scan. Zero-progress guard exits empty-body loops in O(1). Flat alternations fall back to the general VM, avoiding a 125× regression on unshared-prefix alternations.
- **Built-in C functions**: `Text::replace` matches PHP `str_replace` within 2%; `Text::upper/lower/trim` within 4-15%
  of native PHP built-ins
- **BREAKX optimisation**: 8.3× fewer backtrack operations vs `ARB+NOTANY` for key extraction
- **Unicode parity**: `Text::size` on Unicode input equals `mb_strlen` performance

See `bench/` directory for benchmark scripts and `bench/results_builtin.json` for detailed results.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Language Bindings                    │
│  ┌────────────┐    ┌──────────────┐    ┌─────────────┐  │
│  │    PHP     │    │   Python *   │    │   Rust **   │  │
│  │  Binding   │    │  (Reference) │    │ (Community) │  │
│  └─────┬──────┘    └──────┬───────┘    └────────┬────┘  │
│        │                  │                     │       │
│        └──────────────────┼─────────────────────┘       │
│                           │                             │
│                   C Extension API                       │
│ * examples/python-binding/ — starter for community      │
│ ** not yet started — see CONTRIBUTING.md                │
└──────────────────────────┼──────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────┐
│              Language-Agnostic Core (C)                 │
│                          │                              │
│  ┌──────────────┐  ┌─────▼──────┐  ┌──────────────┐     │
│  │ Lexer (C)    │─▶│ Parser (C) │─▶│  Compiler    │     │
│  │ UTF-8 aware  │  │ Recursive  │  │  Bytecode    │     │
│  └──────────────┘  │  Descent   │  └──────┬───────┘     │
│                    └────────────┘         │             │
│                          │                │             │
│                    EBNF Grammar      ┌────▼───────┐     │
│                    (snobol.ebnf)     │  Runtime   │     │
│                                      │   Cache    │     │
│                                      └────┬───────┘     │
│                                           │             │
│  ┌──────────────┐  ┌──────────────┐  ┌────▼───────┐     │
│  │      VM      │◀─│   Bytecode   │◀─│   Tables   │     │
│  │  Backtracking│  │   Execution  │  │  (Assoc)   │     │
│  └──────────────┘  └──────────────┘  └────────────┘     │
└─────────────────────────────────────────────────────────┘
```

## Documentation

- **[Why SNOBOL4 vs PCRE](docs/why-snobol-vs-pcre.md)** — comparison guide with side-by-side examples
- **[C/C++ Manual](docs/c-manual.md)** — full C API reference: compile, match, search, builder, batch, tiers, C++ interop
- **[PHP Manual](docs/php-manual.md)** — full PHP binding reference
- **[Language Compatibility](docs/LANGUAGE_COMPATIBILITY.md)** — which classic SNOBOL4 language features are implemented (tables, labelled control flow, EVAL, templates) with bytecode and fixture details
- **Hosted Doxygen**: [JPetsis.github.io/libsnobol4](https://JPetsis.github.io/libsnobol4/) — auto-deployed on push to main
- **Core API**: Headers in `core/include/snobol/`
- **PHP Binding**: [bindings/php/README.md](bindings/php/README.md)
- **Grammar**: [core/grammar/snobol.ebnf](core/grammar/snobol.ebnf)

## Distribution

### C Library

| Channel               | Command                                                          | Notes                                    |
|-----------------------|------------------------------------------------------------------|------------------------------------------|
| **Build from source** | `cmake -B build && cmake --build build && cmake --install build` | Full control, all platforms              |
| **Homebrew (macOS)**  | `brew install JPetsis/homebrew-tap/libsnobol4`                   | Builds from source via CMake (no bottles yet) |

### PHP Extension

| Channel               | Command                         | Notes                                                             |
|-----------------------|---------------------------------|-------------------------------------------------------------------|
| **PIE** (recommended) | `pie install libsnobol4/snobol` | Single command — downloads pre-built binary or builds from source |

### GitHub Releases

Each release includes pre-built `snobol.so` binaries for 5 platform variants (named per PIE convention):
- Linux x86_64, Linux aarch64
- macOS x86_64, macOS arm64
- Windows x86_64

## Thread Safety

The libsnobol4 core library is **partially thread-safe**. Key guarantees:

| Component                                                                          | Thread-Safe       | Notes                                                                                                                           |
|------------------------------------------------------------------------------------|-------------------|---------------------------------------------------------------------------------------------------------------------------------|
| Pattern compilation (`snobol_pattern_compile*`)                                    | ✅ Fully reentrant | Per-call stack state, no shared globals                                                                                         |
| Pattern matching (`vm_run`, `snobol_search_exec`)                                  | ✅ Fully reentrant | VM state is stack-allocated per call                                                                                            |
| Public API (`snobol_context_create`, `snobol_pattern_match`, `snobol_match`, etc.) | ✅ Reentrant       | No hidden global mutation                                                                                                       |
| Character-class compilation (`compiler.c` global list)                             | ❌ Not thread-safe | Each `snobol_pattern_compile*` uses a file-scope static linked list; do not compile patterns concurrently from multiple threads |

**Best practice:** Create and use patterns from a single thread, or serialise calls to `snobol_pattern_compile*` with an external mutex. Matching and searching can then be called from any thread without additional locking.

In PHP, the Zend Engine serialises extension calls per request, so the PHP binding is inherently single-threaded per request.

## CI / Contributing

The default pull-request CI gate (`.github/workflows/ci-core.yml`) runs on `ubuntu-latest`, `macos-latest`, and `windows-latest`.
**ASan + UBSan** is now included as a standard CI job (no longer optional).

Additional workflows:
- **Benchmarks** (`.github/workflows/benchmarks.yml`): full benchmark suite with artifact upload.
- **Valgrind** (`.github/workflows/valgrind.yml`): memory leak detection on Linux.

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution guidelines.

## License

[Apache License, Version 2.0](LICENSE)

## Versioning

libsnobol4 uses **one project-wide version number**; per-component
changelogs and tags (`core/v*`, `php/v*`) record what changed in each
component:

| Component              | Current | Next        | Status               | Install                               |
|------------------------|---------|-------------|----------------------|---------------------------------------|
| **Core**               | v1.0.4 | v1.0.4     | ✅ v1.0.4 shipped    | `brew install JPetsis/homebrew-tap/libsnobol4` |
| **PHP Binding**        | v1.0.4 | v1.0.4     | ✅ Stable (graduated) | `pie install libsnobol4/snobol`       |
| **Python (reference)** | —       | —           | Prototype only       | `examples/python-binding/`            |

A core-only release still moves the shared version number (and the
Packagist package), because the PHP package embeds the core via the
amalgam; the per-component changelogs and tags track what actually changed.

The project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
The **single source of version truth** is the top-level
`project(libsnobol4 VERSION X.Y.Z)` declaration in `CMakeLists.txt`; the
`SNOBOL_VERSION_*` / `SNOBOL_VERSION_STRING` macros in `<snobol/version.h>`
are generated from it at configure time — never edit the version literals in a
header directly.

Every merged change **must** add an entry to
[`CHANGELOG.md`](CHANGELOG.md) under `[Unreleased]`, using
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/) categorization
(Added / Changed / Fixed / Removed). See [CONTRIBUTING.md](CONTRIBUTING.md),
[SECURITY.md](SECURITY.md), and [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) for
the full contribution, security-reporting, and conduct policies.

### Platform Support

| Platform             | CI Status       |
|----------------------|-----------------|
| macOS ARM64          | ✅ Native runner |
| Linux AArch64        | ✅ Native runner |
| Linux ARMv7-A        | ✅ Native runner |
| Linux RISC-V 64      | ✅ Native runner |
| Linux x86-64         | ✅ Native runner |
| macOS x86-64 (Intel) | ✅ Native runner |
| Windows x86-64       | ✅ Native runner |
