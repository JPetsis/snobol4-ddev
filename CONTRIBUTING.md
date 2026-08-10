# Contributing to libsnobol4

Thank you for considering contributing to libsnobol4! This document provides guidelines and instructions for setting up
your development environment, building the project, and submitting changes.

## Project Structure

libsnobol4 uses a **monorepo** structure with a language-agnostic C core and language-specific bindings:

```
libsnobol4/
├── core/                    # Core C library (language-agnostic)
│   ├── include/snobol/      # Public API headers
│   ├── src/                 # Core implementation
│   └── grammar/             # SNOBOL pattern grammar
├── bindings/                # Language-specific bindings
│   └── php/                 # PHP binding
│       ├── src/             # PHP extension source
│       ├── php-src/         # PHP helper classes
│       └── tests/           # PHPUnit tests
├── tests/c/                 # Core C test suite
├── examples/c/              # C usage examples
└── README.md                # Project overview
```

## Development Environment

### Option 1: DDEV (Recommended for PHP Development)

[DDEV](https://ddev.com/) provides a consistent containerized environment:

```bash
# PHP binding development
cd bindings/php
ddev start
```

This will:

- Start a PHP 8.5 container
- Build the libsnobol4 extension from `core/`
- Enable the extension automatically

**Rebuilding:**
If you make changes to the C core or the PHP binding source, you can rebuild the extension without a full restart:

```bash
ddev build-snobol-extension
```

This is the canonical build path that handles amalgamation regeneration and artifact cleanup.

### Option 2: Native Build

For core library development without PHP:

```bash
# Install CMake and a C compiler
# macOS: brew install cmake
# Ubuntu: apt install cmake build-essential

# Configure and build
cmake -B build -DBUILD_TESTS=ON
cmake --build build

# Run tests
ctest --test-dir build
```

## Development Workflow

### Building

```bash
# Core library only
make build

# Debug build
make build-debug

# With PHP binding
make build-php

# Clean build
make clean
```

### Running Tests

```bash
# Core C tests
make test

# Verbose test output
make test-verbose

# PHP tests (requires PHP binding)
cd bindings/php
vendor/bin/phpunit tests/php

# Memory leak detection with Valgrind (delegates to CMake test-valgrind target)
make test-valgrind

# AddressSanitizer + UndefinedBehaviorSanitizer (GCC or Clang required)
# Step 1: configure and build with sanitizers enabled
make build-asan
# Step 2: run the test suite under ASan/UBSan
make test-asan

# Alternatively, use CMake directly:
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DSNOBOL_SANITIZE=ON
cmake --build build-asan --target test-asan
```

Fuzzing (crash and differential targets; requires Clang):

```bash
cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DSNOBOL_FUZZ=ON
cmake --build build-fuzz
./build-fuzz/tests/fuzz/fuzz_oracle -max_total_time=600
```

`fuzz_compiler`/`fuzz_vm` detect crashes and memory errors; `fuzz_oracle`
detects **wrong answers** — it runs the search-tier dispatch against the
reference VM on the same input and reports any disagreement in success,
position, or length. The deterministic equivalent (`test_search_oracle` with
the corpus in `tests/c/corpus.h`) runs as part of `make test`, so optimized
search paths must always behave identically to the reference VM.

### Regenerating Unicode Case-Folding Tables

The BMP case-folding tables in `core/src/unicode_fold_data.c` are auto-generated from
[Unicode CaseFolding.txt](https://www.unicode.org/Public/UCD/latest/ucd/CaseFolding.txt).

```bash
# Regenerate tables (requires C compiler; fetches UCD if not cached locally)
make gen-unicode-fold

# Rebuild after regeneration
make build
```

The generator tool is in `dev/gen_unicode_fold.c`. It parses the full-case fold
(staus C and S) entries from Unicode CaseFolding.txt and emits statically-initialized
C arrays with sorted pair tables for O(log n) binary search.

### Code Quality

```bash
# Build with strict warnings
make warnings

# Run clang-tidy (requires LLVM)
make lint

# Format code (requires clang-format)
make format
```

## Submitting Changes

### 1. Create a Branch

```bash
git checkout -b feature/your-feature-name
```

### 2. Make Changes

- Keep changes focused and minimal
- Follow existing code style
- Add tests for new functionality
- Update documentation as needed

### 3. Run Tests

Ensure all tests pass before submitting:

```bash
# Core tests (includes the differential oracle suite — accelerated search
# results must match the reference VM for the whole pattern corpus)
make test

# PHP tests (if applicable)
cd bindings/php && vendor/bin/phpunit tests/php
```

Changes to the search engine or metadata derivation should also be smoke-tested
with the oracle fuzzer (`./build-fuzz/tests/fuzz/fuzz_oracle -runs=10000`) —
it finds wrong answers, not just crashes.

### 4. Update the Changelog

**Every change must add an entry under `[Unreleased]`** in the changelog of
the affected component — [`CHANGELOG.md`](CHANGELOG.md) for the C core and
repository-level work, [`bindings/php/CHANGELOG.md`](bindings/php/CHANGELOG.md)
for the PHP binding — using
[Keep a Changelog](https://keepachangelog.com/en/1.0.0/) categorization
(Added / Changed / Fixed / Removed). Describe the change, the affected
tiers/surfaces, and any migration notes. PRs without a changelog entry will
not be merged.

### 5. Commit Changes

Write clear, descriptive commit messages:

```bash
git commit -m "feat: add bounded repetition support

- Implement repeat(P, min, max) pattern
- Add C tests for bounded repetition
- Update documentation"
```

### 6. Submit a Pull Request

- Push to your fork
- Open a PR against the `main` branch (the PR template lists the required gate)
- Include a description of changes
- Reference any related issues

## Coding Standards

### C Code (Core Library)

- **Standard**: C11/C17 (C17 on MSVC, C11 baseline)
- **Formatting**: Use `clang-format` (run `make format`)
- **Naming**:
  - Types: `snake_case_t` (e.g., `ast_node_t`)
  - Functions: `snake_case` (e.g., `snobol_ast_create`)
  - Macros: `SCREAMING_SNAKE_CASE` (e.g., `SNOBOL_AST_VERSION`)
- **Documentation**: Add file-level and function-level comments
- **C++ interop**: Public headers must stay C++-consumable. Wrap any new
  public header in `extern "C"` guards and keep it self-contained (it must
  compile standalone). The `header-cxx` CI job compiles the header set as C++
  with `g++` and `clang++` — a C-ism that breaks that build will fail CI.

### PHP Code (Bindings)

- **Standard**: PSR-12
- **Formatting**: Use `phpcbf` or configure your editor
- **Naming**:
  - Classes: `PascalCase` (e.g., `PatternHelper`)
  - Methods: `camelCase` (e.g., `matchOnce`)
  - Constants: `SCREAMING_SNAKE_CASE` (e.g., `VERSION`)

## Testing Guidelines

### C Tests

- Place tests in `tests/c/`
- Use the existing test framework in `test_runner.c`
- Test both success and failure cases
- Include stress tests for edge cases

### PHP Tests

- Place tests in `bindings/php/tests/php/`
- Use PHPUnit framework
- Test public API methods
- Include regression tests for bugs

## API Stability Policy

Starting with **v0.11.0**, libsnobol4 makes the following stability guarantees:

- **Public C API** — Functions declared in `core/include/snobol/snobol.h` with `snobol_` prefix follow [SemVer](https://semver.org/).
  - A **major** bump means breaking changes to the public API or ABI.
  - A **minor** bump adds functionality in a backward-compatible manner.
  - A **patch** bump contains only bug fixes.
- **ABI version** — `snobol_get_abi_version()` returns a monotonically-increasing integer. Load-time checks MUST compare this value at runtime (not at compile time) to detect incompatible dynamic libraries. A change in `SNOBOL_ABI_VERSION` always accompanies a major version bump.
- **Deprecation** — Public functions marked `SNOBOL_DEPRECATED` in the header remain available for one minor-version cycle before removal. Compiler warnings guide migration.
- **Internal headers** — Everything inside `core/include/snobol/` not in `snobol.h` or the public API section is subject to change without notice.
- **PHP binding** — Follows the major/minor/patch scheme independently. The PHP extension version `PHP_SNOBOL_VERSION` tracks the binding, not the core.

## Error Handling Convention

The C core libsnobol4 makes the following stability guarantees around allocation failures:

- **Allocation failures are recoverable.** Every public API function in `core/src/api.c` checks the result of every `snobol_malloc` / `snobol_calloc` and returns a sentinel (`NULL` for handle-returning functions, `0` for size-returning functions, or a `snobol_match_result_t{.success=false, .error="..."}` for `snobol_match()`) on failure. There is no `abort()` / `exit()` path triggered by OOM conditions out-of-the-box.
- **Cleanup is partial-state-safe.** When an allocation fails mid-construction, any earlier allocations in the same call are `snobol_free`d before the failure return. New code MUST follow this pattern — never return `NULL` from a partially-constructed object.
- **`snobol_check_alloc` helper.** A defensive macro `snobol_check_alloc(ptr)` is declared in `snobol/snobol_internal.h`. In standalone builds it is `((ptr) != NULL)`; in PHP builds it is a no-op (the Zend allocator does not return `NULL`). Use it for symmetry across build types when wrapping allocation sites.
- **Allocation failure tests.** The custom test runner (`tests/c/test_runner.c`) does not inject OOM. ASan/UBSan CI sanity-checks the standalone failure paths. Future work: add an OOM injection test framework (see roadmap, v0.12+).

## Release Process

### Versioning

libsnobol4 uses **one project-wide version number** and follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **Project version**: `v<major>.<minor>.<patch>` (e.g., `v1.0.2`)
- **Component tags**: `core/vX.Y.Z` and `php/vX.Y.Z` track the same
  release per component

The version has a **single source of truth**: the
`project(libsnobol4 VERSION X.Y.Z)` declaration in the top-level
`CMakeLists.txt`. The `SNOBOL_VERSION_*` macros in `<snobol/version.h>` are
generated from it at configure time via `core/cmake/version.h.in` — do **not**
hand-edit version literals in any header. `PHP_SNOBOL_VERSION` in
`bindings/php/src/php_snobol.h` mirrors it and is bumped with each release.

**Decoupling is per-component *content*, not per-component *numbers*:** the
per-component changelogs and tags record what changed in each component,
but the version number and the release vehicle (plain `vX.Y.Z` tag →
GitHub release + Packagist) are shared. A core-only patch still moves the
Packagist package version, because the PHP package embeds the core via the
amalgam — its shipped artifact changes. For the same reason the tag triplet
is always cut from the same commit (see "Creating a Release").

### Creating a Release

1. Bump `project(libsnobol4 VERSION X.Y.Z)` in the top-level `CMakeLists.txt`
   (and reconfigure); the version header regenerates automatically.
2. Move the `[Unreleased]` entries under a new version heading in the
   changelog of each released component (`CHANGELOG.md` for the C core,
   `bindings/php/CHANGELOG.md` for the PHP binding).
3. Create git tags:
   ```bash
   git tag v1.0.2
   git tag core/v1.0.2
   git tag php/v1.0.2
   git push origin --tags
   ```
   The plain `vX.Y.Z` tag drives the GitHub release workflow and Packagist;
   `core/vX.Y.Z` and `php/vX.Y.Z` track the components independently. **All
   three tags are cut from the same commit** — binding and core versions
   correspond 1:1, even when one component has no code changes (the PHP
   package still embeds the core via the amalgam).
4. Create GitHub release with changelog. Minor/major release notes are
   generated from merged PRs; **patch release notes are written manually**
   (cherry-picked hotfixes are commits, not PRs, so auto-generated notes
   would be empty).

### Branching Model

- **`main` is the trunk.** All OpenSpec changes merge directly to `main`;
  it always accumulates everything since the last release. A release is
  tagged whenever `main` is cohesive — no curation at merge time.
- **`release/v1.0.x`-style maintenance branches** exist only for a shipped
  line and only get bug fixes. They are created from the last release tag
  of that line when the first hotfix is needed, and are never merged back
  into `main`.
- **Long-lived `feature/*` branches** are reserved for work that must be
  *excluded* from a release (e.g. next-major-engine work). Rebase them onto
  `main` periodically.

#### Shipping a patch release (hotfix flow)

> **Never move a tag that Packagist has already seen.** Packagist stable
> versions are immutable: the version's git reference is pinned at first
> publication and a moved tag is a *blocked retag* (warning badge on the
> package page + maintainer notification). A tag can be moved back to its
> **original** commit to clear the badge — anything else requires shipping
> the fix as the **next** version number ("version numbers are cheap").
> Treat every pushed release tag as permanent.

1. Fix the bug on `main` first (trunk-first — `main` stays the canonical
   history). Add the changelog entry under `[Unreleased]` as usual.
2. Cherry-pick the fix (and its changelog entry, moved under the `[1.0.3]`
   heading — release-branch entries go under the version heading, not
   `[Unreleased]`) onto `release/v1.0.x`:
   ```bash
   git checkout release/v1.0.x
   git cherry-pick -x <sha>
   ```
3. Commit a version bump to `project(libsnobol4 VERSION 1.0.3)` on the
   release branch (do **not** cherry-pick the bump to `main`, which carries
   the next minor's version).
4. Push the release branch; CI (`release/**` triggers) gates it. Tag the
   release-branch commit with the tag triplet and run the release steps
   above.

## Getting Help

 - **Documentation**: See `README.md` and `bindings/php/README.md`
 - **Issues**: Open an issue on GitHub
 - **Discussions**: Use GitHub Discussions for questions

---

**Windows / cross-compiler quick start:**

```bash
# Visual Studio 2022
cmake --preset windows-msvc
cmake --build build-msvc --config Release
ctest --test-dir build-msvc -C Release --output-on-failure

# MinGW-w64
cmake --preset windows-mingw
cmake --build build-mingw
ctest --test-dir build-mingw --output-on-failure
```

`CMakePresets.json` at the project root includes `windows-msvc` and `windows-mingw` presets alongside `debug`, `release`, and `asan` presets for Unix.

Note: `SNOBOL_SANITIZE=ON` is not supported on MSVC — configure with GCC or Clang.

## Community Language Bindings

libsnobol4's **officially maintained** components are:
- **C engine** (`core/`) — language-agnostic pattern matching library
- **PHP binding** (`bindings/php/`) — PHP extension and helper classes

Additional language bindings (Python, Rust, Go, Java, etc.) are **community contributions** guided by the
principles below.

### Scope

Community bindings wrap the same language-agnostic C core and provide idiomatic surface APIs for their
respective languages. Examples include:
- A Python binding providing a `snobol.match(pattern, subject)` function
- A Rust crate exposing a `Pattern::compile()` builder API
- A Go module with `snobol.MatchString()`

### What We Provide

- A **reference prototype** at `examples/python-binding/` (Python) — not feature-complete, but demonstrates the C API
- Stabilized C headers under `core/include/snobol/`
- `snobol_match()` and `snobol_pattern_build_*()` convenience APIs (v0.11.0+) for one-shot usage
- A pkg-config / CMake target for linker integration
- PHP distribution via `pie install libsnobol4/snobol` (single command for the entire PHP binding)

### Reference Implementation

A **Python reference prototype** is available at `examples/python-binding/`. This is not
feature-complete but demonstrates the C API integration pattern — use it as a starting
point for your own binding.

### Minimal Binding Checklist

| Check | Requirement                                                                                                            |
|-------|------------------------------------------------------------------------------------------------------------------------|
| ☐     | **Link to C core** via `pkg-config --cflags --libs libsnobol4` or CMake `find_package(libsnobol4)`                     |
| ☐     | **Wrap `snobol_match()`** — the one-shot convenience API for simple use cases                                          |
| ☐     | **Wrap `snobol_pattern_compile()` / `snobol_pattern_match()`** — the multi-step API for repeated matching              |
| ☐     | **Wrap `snobol_match_result_free()`** — proper memory management for match results                                     |
| ☐     | **Provide idiomatic surface API** matching your language's conventions (e.g., a `Pattern` class, a `match()` function) |
| ☐     | **Permissive open-source license** — MIT, Apache 2.0, BSD-2, or similar                                                |
| ☐     | **Publish to standard distribution channel** — PyPI, crates.io, npm, etc.                                              |
| ☐     | **Host in your own repository** — community bindings live outside the libsnobol4 monorepo                              |
| ☐     | **Open a PR** to add your binding to the project README (listing at maintainers' discretion)                           |

### Maintainer Expectations

1. **Community bindings live in their own repositories**, not in this monorepo (except the Python reference).
2. Bindings must use a permissive open-source license (MIT, Apache 2.0, BSD-2, or similar).
3. Bindings follow their language's standard packaging and distribution channels (PyPI, crates.io, etc.).
4. The core maintainers will not break the C ABI without notice (guaranteed by SemVer).
5. The project README may list community binding repositories at the maintainers' discretion.

### Getting Started

1. Start from the **Python reference prototype** at `examples/python-binding/`.
2. Use `snobol_match()` for a one-shot convenience path or the full `snobol_pattern_compile()` / `snobol_pattern_match()`
   API for advanced usage.
3. Link via `pkg-config --cflags --libs libsnobol4` or CMake `find_package(libsnobol4)`.
4. Follow the [Minimal Binding Checklist](#minimal-binding-checklist) above.
5. Publish to your language's ecosystem and open a PR adding your binding to the project README.

## Code of Conduct

Please be respectful and constructive in all interactions. We welcome contributors of all backgrounds and experience
levels.
