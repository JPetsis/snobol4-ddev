# libsnobol4 C/C++ Manual

A high-performance C library implementing SNOBOL4-style string pattern
matching. This manual documents the C API (`core/include/snobol/snobol.h`)
and how to use it from C and C++.

For the PHP binding, see [php-manual.md](php-manual.md).

## Table of Contents

1. [Overview](#1-overview)
2. [Installation & Setup](#2-installation--setup)
3. [Architecture Overview](#3-architecture-overview)
4. [Core Concepts](#4-core-concepts)
5. [Pattern Compilation](#5-pattern-compilation)
6. [Pattern Matching](#6-pattern-matching)
7. [Search Operations](#7-search-operations)
8. [The Builder API](#8-the-builder-api)
9. [Batch Search](#9-batch-search)
10. [Match Results & Accessors](#10-match-results--accessors)
11. [Error Handling & Memory Management](#11-error-handling--memory-management)
12. [Tier Dispatch System](#12-tier-dispatch-system)
13. [Performance Characteristics](#13-performance-characteristics)
14. [Thread Safety](#14-thread-safety)
15. [C++ Interop](#15-c-interop)
16. [Version & ABI](#16-version--abi)
17. [Common Use Cases](#17-common-use-cases)
18. [Appendix: SNOBOL4 String Syntax](#18-appendix-snobol4-string-syntax)

---

## 1. Overview

libsnobol4 is a C23 library that implements the SNOBOL4 pattern-matching
language as a drop-in alternative to PCRE-style regex. Its differentiator is
the **pattern construction vocabulary**: patterns are composed from named,
maintainable pieces (`SPAN`, `BREAK`, `CAP`, `ARBNO`, `BAL`, …) rather than
cryptic operator soup.

The core is language-agnostic (a lexer → parser → compiler → VM pipeline in
`core/src/`); the same engine powers the PHP binding and any future binding.

### Key Differences from PCRE/Regex

| Aspect | PCRE / Regex | libsnobol4 |
|---|---|---|
| Pattern language | Symbol-heavy operators (`\d{2,4}`, `(?:...)`) | Named primitives (`SPAN('0-9')`, `ARBNO(...)`) |
| Captures | Backreferences by number | Named capture registers (`@r1(...)`) |
| Backtracking | Implicit, engine-chosen | Explicit choice points (`SPLIT`, `FENCE`, `BREAKX`) |
| Side effects | Impossible | Possible (`EVAL`, `ASSIGN`, table/array ops) |
| Match semantics | Anchored/unanchored modes | SNOBOL-style anchored cursor matching |

### When to Reach for libsnobol4

- You want a **purely programmatic pattern construction API** (the C Builder
  API) without string interpolation concerns.
- You need SNOBOL4 semantics (balanced strings, spans/breaks, backtracking
  control, capture registers).
- You want a C library whose PHP binding shares the exact same engine.

See [why-snobol-vs-pcre.md](why-snobol-vs-pcre.md) for a detailed comparison.

---

## 2. Installation & Setup

### Build from Source

```bash
cmake -B build
cmake --build build
cmake --install build        # installs libsnobol4.a, headers, and libsnobol4.pc
```

Options:

| Option | Default | Description |
|---|---|---|
| `BUILD_TESTS` | ON | Build the C test suite |
| `BUILD_PHP` | OFF | Build the PHP binding |
| `BUILD_SHARED_LIBS` | OFF | Build a shared library instead of static |
| `SNOBOL_PROFILE` | OFF | Enable VM profiling counters |
| `SNOBOL_SANITIZE` | OFF | AddressSanitizer + UBSan |
| `SNOBOL_LTO` | OFF | Link-time optimization (standalone builds) |

### Homebrew (macOS)

```bash
brew install JPetsis/homebrew-tap/libsnobol4
```

### Linking

```bash
# pkg-config
cc myprogram.c $(pkg-config --cflags --libs libsnobol4)

# CMake
find_package(libsnobol4 REQUIRED)
target_link_libraries(myapp PRIVATE snobol4)
```

Include the umbrella header:

```c
#include <snobol/snobol.h>
```

`snobol.h` re-exports the other public headers (`ast.h`, `compiler.h`,
`search.h`, `vm.h`, `parser.h`, …), so a single include is enough for the
common API surface.

---

## 3. Architecture Overview

```
SNOBOL4 source string
    → Lexer (tokenizer)
    → Parser (recursive-descent, builds AST)
    → Compiler (AST → bytecode, 42 opcodes)
    → Fusion pass (SPLIT+ANY/SPAN fusion)
    → Search metadata derivation (tier, candidate bitmaps, skip tables)
    → Pattern object ready for matching
```

Patterns can also be constructed directly via the **Builder API**
(`snobol_pattern_build_*`, AST nodes) and compiled with
`snobol_pattern_build_compile()`, bypassing the lexer/parser entirely.

At match time, `snobol_search_exec()` dispatches through a **tier table**
(see [Tier Dispatch System](#12-tier-dispatch-system)): a function-pointer
table indexed by the pre-computed `tier` field of the search metadata. Each
tier is a dedicated matching strategy — bitmap scans, literal accelerators,
a lightweight search VM, a DFA automaton, SIMD NFA, or the full
backtracking VM.

---

## 4. Core Concepts

| Type | Role | Ownership |
|---|---|---|
| `snobol_context_t` | Logical owner of patterns | `snobol_context_create()` / `snobol_context_destroy()` |
| `snobol_pattern_t` | Compiled, ready-to-match pattern | `snobol_pattern_compile[_ex]()` / `snobol_pattern_free()` |
| `snobol_match_t` | One match result (heap or reusable) | `snobol_pattern_match/search()` → `snobol_match_free()` |
| `ast_node_t` | AST node built by the Builder API | `snobol_ast_free()` (or consumed by compile) |
| `snobol_batch_result_t` | All matches of a batch search | Caller-allocated; `snobol_batch_result_free()` |

The pattern object is **opaque** — its struct is private to `api.c`. All
introspection goes through accessors (`snobol_pattern_get_bc()`,
`snobol_pattern_get_meta()`, …).

---

## 5. Pattern Compilation

### From Source String

```c
#include <snobol/snobol.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    snobol_context_t *ctx = snobol_context_create();

    char *err = NULL;
    snobol_pattern_t *pat = snobol_pattern_compile(
        ctx, "'id:' SPAN('0-9')", 17, &err);
    if (!pat) {
        fprintf(stderr, "compile failed: %s\n", err);
        free(err);
        return 1;
    }

    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
    return 0;
}
```

### With Options

```c
snobol_pattern_t *snobol_pattern_compile_ex(snobol_context_t *ctx,
                                            const char *source, size_t len,
                                            uint32_t flags, char **error);
```

`flags` is a bitmask of:

| Flag | Meaning |
|---|---|
| `SNOBOL_FLAG_CASE_INSENSITIVE` (`0x0001`) | Case-insensitive matching for ASCII + Latin-1 Supplement pairs; captured text preserves subject case |

Unknown flag bits are ignored (forward-compatible).

### From Builder AST

See [The Builder API](#8-the-builder-api) for the programmatic path; the
compile step is `snobol_pattern_build_compile()`.

---

## 6. Pattern Matching

### Anchored Match (`snobol_pattern_match`)

```c
snobol_match_t *snobol_pattern_match(snobol_pattern_t *pattern,
                                     const char *subject, size_t sub_len);
```

The match must start at offset 0 (SNOBOL-style anchored semantics). The
result is a heap-allocated `snobol_match_t`; free it with `snobol_match_free()`.
Always check `snobol_match_success()` before reading fields.

Anchored matches run the same required-byte prefilter as unanchored searches:
when the pattern provably requires a literal that is absent from the subject,
`snobol_search_exec_anchored` fails fast with `prefilter_skip` set (one O(n)
`memchr`/`memmem` scan, no tier/VM execution). The prefilter is a
necessary-condition check only — a subject containing the literal but failing
at the anchor falls through to the normal tier dispatch.

```c
snobol_match_t *m = snobol_pattern_match(pat, "id:1234 rest", 13);
if (m && snobol_match_success(m)) {
    printf("matched %zu bytes at %zu\n",
           snobol_match_get_length(m), snobol_match_get_position(m));
}
snobol_match_free(m);
```

### Unanchored Search (`snobol_pattern_search`)

```c
snobol_match_t *snobol_pattern_search(snobol_pattern_t *pattern,
                                      const char *subject, size_t sub_len);
```

Finds the first match anywhere in the subject. Positions in the result are
**subject-absolute** (captures included).

### Zero-Allocation Literal Matching (`snobol_pattern_match_literal`)

For patterns that are pure literals (e.g. `'hello'`), `match_literal` skips
the VM entirely:

```c
snobol_literal_match_t r = snobol_pattern_match_literal(pat, "hello world", 11);
if (r.success) {
    printf("literal at %zu, length %zu\n", r.position, r.length);
}
```

`snobol_literal_match_t` is `{ bool success; size_t position; size_t length; }`
returned by value — zero allocations. Non-literal patterns return
`{false, 0, 0}` immediately.

### Reusable Match Objects (`snobol_match_create` / `snobol_pattern_search_reuse`)

For hot loops, avoid per-call allocation:

```c
snobol_match_t *m = snobol_match_create();   // allocate once
for (size_t i = 0; i < n; i++) {
    bool ok = snobol_pattern_search_reuse(pat, subjects[i], lens[i], m);
    if (ok && m->success) { /* consume */ }
    snobol_match_reset(m);                   // clear between uses (optional)
}
snobol_match_free(m);
```

`snobol_match_reset()` frees owned strings (output, materialized captures)
and is safe to call before every reuse.

### One-Shot Convenience (`snobol_match`)

```c
snobol_match_result_t *r = snobol_match("'abc' ARB 'def'", 15,
                                        "xyz abc def xyz", 15, 0);
if (r && r->success) {
    printf("captures: %d\n", r->capture_count);
}
snobol_match_result_free(r);
```

Bundles compile + match; `snobol_match_result_t` carries `success`, `error`
(malloc'd on compile failure), `output`, `captures[]`, `capture_lens[]`, and
`capture_count`. Free with `snobol_match_result_free()`.

---

## 7. Search Operations

### Stateful Search (`snobol_pattern_search_state_t`)

The stateful `_ex` API reuses a persistent search state (VM, DFA, range
metadata, SIMD NFA caches) across calls — no per-call metadata rebuild:

```c
snobol_pattern_search_state_t *st = snobol_pattern_search_state_create(
    snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat));

snobol_match_t *m = snobol_pattern_search_ex(st, "xxab12yy", 8, 0);
m = snobol_pattern_search_ex(st, "xxab12yy", 8, 2);   // start_offset

// Anchored variant: match must start at offset 0
m = snobol_pattern_search_ex_anchored(st, "ab12", 4);

snobol_pattern_search_state_destroy(st);
```

State setters:

| Function | Purpose |
|---|---|
| `snobol_pattern_search_state_set_pattern(state, pattern)` | Bind a pattern (for automaton/trie caching) |
| `snobol_pattern_search_state_set_trie_cache(state, trie)` | Pre-built alt-literals trie |
| `snobol_pattern_search_state_set_eval_fn(state, fn, udata)` | EVAL callback + userdata |

### Lean Tokenize API (`snobol_pattern_search_next`)

Single-literal unanchored search (~8 ns/call), returning position and length
via out-parameters — no match struct, captures, or output buffer:

```c
size_t pos = 0, len = 0;
bool ok = snobol_pattern_search_next(st, "a,b,c", 5, 0, &pos, &len);
while (ok) {
    printf("token at %zu (len %zu)\n", pos, len);
    ok = snobol_pattern_search_next(st, "a,b,c", 5, pos + len, &pos, &len);
}
```

Returns `false` for non-literal patterns — callers fall back to
`snobol_pattern_search_ex()`.

---

## 8. The Builder API

Programmatic AST construction — compose patterns from named pieces without
writing pattern source strings.

```c
snobol_pattern_build_t *b = snobol_pattern_build_create();

ast_node_t *lit  = snobol_pattern_build_lit(b, "hello", 5);
ast_node_t *span = snobol_pattern_build_span(b, "0-9", 3);
ast_node_t *cap  = snobol_pattern_build_cap(b, 1, span);

ast_node_t **parts = malloc(2 * sizeof(ast_node_t *));
parts[0] = lit;
parts[1] = cap;
ast_node_t *root = snobol_pattern_build_concat(b, parts, 2);
root = snobol_pattern_build_emit(b, root);   // finalize: builder reusable

char *err = NULL;
snobol_pattern_t *pat = snobol_pattern_build_compile(ctx, root, 0, &err);
if (!pat) {
    fprintf(stderr, "compile failed: %s\n", err);
    free(err);
    return 1;
}
snobol_pattern_build_destroy(b);             // builder can be reused/destroyed

snobol_match_t *m = snobol_pattern_search(pat, "hello42", 7);
if (m && snobol_match_success(m))
    printf("matched\n");
snobol_match_free(m);
```

**Ownership contract:**

- Compound functions (`concat`, `alt`, `cap`, `arbno`, …) **take ownership**
  of their child nodes.
- The **parts array of `concat` is owned** — it must be heap-allocated
  (`malloc`), not a stack array.
- `snobol_pattern_build_emit()` transfers the root; the builder may be reused.
- `snobol_pattern_build_compile()` **consumes the AST tree** (freed on both
  success and failure) — do **not** call `snobol_ast_free()` on it afterwards.
- The returned pattern matches identically to the same pattern compiled from
  source, and frees with `snobol_pattern_free()`.

### Constructor Reference

| Function | Node |
|---|---|
| `snobol_pattern_build_lit(b, text, len)` | Literal string |
| `snobol_pattern_build_span(b, set, len)` | Match 1+ chars in set |
| `snobol_pattern_build_brk(b, set, len)` | Consume until char in set |
| `snobol_pattern_build_any(b, set, len)` | Match single char in set |
| `snobol_pattern_build_notany(b, set, len)` | Match single char NOT in set |
| `snobol_pattern_build_len(b, n)` | Match exactly n codepoints |
| `snobol_pattern_build_arbno(b, sub)` | Zero or more of sub |
| `snobol_pattern_build_cap(b, reg, sub)` | Capture sub into register `reg` |
| `snobol_pattern_build_assign(b, var, reg)` | Assign register to variable |
| `snobol_pattern_build_concat(b, parts, count)` | Sequence (owns parts array) |
| `snobol_pattern_build_alt(b, left, right)` | Alternation |
| `snobol_pattern_build_label(b, name, target)` | Label definition |
| `snobol_pattern_build_goto(b, label)` | Unconditional goto |
| `snobol_pattern_build_pos(b, n)` / `tab` / `rpos` / `rtab` | Position primitives |
| `snobol_pattern_build_breakx(b, set, len)` | BREAK with retry |
| `snobol_pattern_build_bal(b, open, close)` | Balanced string |
| `snobol_pattern_build_fence(b)` / `rem` / `abort` / `fail` / `succeed` | Control primitives |

For nodes without a builder constructor (e.g. `repeat(sub, min, max)` via
`snobol_ast_create_repeat()`, anchors via `snobol_ast_create_anchor()`), the
`snobol_ast_create_*()` family in `ast.h` can be mixed freely — the compiler
accepts any AST root.

---

## 9. Batch Search

Find **all** non-overlapping matches in one pass with flat C arrays:

```c
snobol_batch_result_t out;
memset(&out, 0, sizeof(out));

bool ok = snobol_pattern_search_batch(
    snobol_pattern_get_bc(pat), snobol_pattern_get_bc_len(pat),
    subject, subject_len, snobol_pattern_get_meta(pat), &out);

if (ok) {
    for (size_t i = 0; i < out.match_count; i++) {
        printf("match %zu at %zu, len %zu\n",
               i, out.positions[i], out.lengths[i]);
    }
} else if (out.eligible) {
    /* zero matches — DONE, do NOT fall back */
} else {
    /* pattern not batchable — fall back to per-call loop */
}
snobol_batch_result_free(&out);
```

**Critical contract:** a `false` return with `out->eligible == true` means
zero matches were found — treat it as DONE. Only `eligible == false` means
the pattern was not batch-eligible and the caller should fall back to a
per-call loop. Never blindly treat `false` as "try the per-call loop".

The stateful variant reuses caches across calls:

```c
ok = snobol_pattern_search_batch_ex(st, subject, len, &out);
```

Capture rows are `[var_count][match_count * 2]` offset pairs
(`[start0, len0, start1, len1, …]` per register); capture offsets are
subject-absolute. `outputs` concatenates per-match EMIT output with an
empty-string sentinel; `output_lens` gives each segment's length.

Patterns with side-effect ops (`EVAL`, `ASSIGN`, `DYNAMIC`) are not batchable
and return `false` with `eligible == false`.

---

## 10. Match Results & Accessors

| Accessor | Returns |
|---|---|
| `snobol_match_success(m)` | `bool` — whether the match succeeded |
| `snobol_match_get_position(m)` | Match start offset (subject-absolute) |
| `snobol_match_get_length(m)` | Matched byte length |
| `snobol_match_get_output(m, &len)` | EMIT output buffer (owned) |
| `snobol_match_get_variable(m, "1", &len)` | Capture value by register name |

### Captures

Capture registers are **1-based end-to-end**: `@r1(...)` in source and
`snobol_pattern_build_cap(b, 1, sub)` both store into register 1, and the
accessor maps the name `"1"` to that register:

```c
size_t clen = 0;
const char *cap = snobol_match_get_variable(m, "1", &clen);
if (cap) {
    printf("capture 1: %.*s\n", (int)clen, cap);
}
```

- `"1"`, `"2"`, … up to `SNOBOL_API_MAX_VARS` (64). The `"v1"` spelling is
  also accepted.
- Unset registers materialize as an empty string; invalid names
  (`""`, `"abc"`, negative, > 64) return `NULL`.
- Capture values are **subject-absolute** on every API path (match, search,
  reuse, `_ex`, batch) — no window-relative offsets.

---

## 11. Error Handling & Memory Management

### Error convention

Compile functions (`snobol_pattern_compile[_ex]`,
`snobol_pattern_build_compile`) set `*error` to a **malloc'd, NUL-terminated
error string** on failure (caller must `free()` it) and to `NULL` on success.
A `NULL` error out-param is supported.

```c
char *err = NULL;
snobol_pattern_t *pat = snobol_pattern_compile(ctx, "(", 1, &err);
if (!pat) {
    fprintf(stderr, "compile error: %s\n", err);
    free(err);
}
```

The one-shot `snobol_match()` reports failures inside the result struct:
`r->success == false` with `r->error` set (freed by `snobol_match_result_free()`).

### Ownership rules

| You created… | You must free with… |
|---|---|
| `snobol_context_t` | `snobol_context_destroy()` |
| `snobol_pattern_t` | `snobol_pattern_free()` |
| `snobol_match_t` | `snobol_match_free()` (or reuse) |
| `snobol_match_result_t` | `snobol_match_result_free()` |
| `ast_node_t` tree | `snobol_ast_free()` (unless consumed by compile) |
| `snobol_batch_result_t` arrays | `snobol_batch_result_free()` |
| `*error` strings | `free()` |
| bytecode buffers (`snobol_pattern_get_bc`) | not yours — read-only, freed by pattern |

All frees are NULL-safe.

---

## 12. Tier Dispatch System

`snobol_search_exec()` dispatches through a function-pointer table indexed by
the pre-computed `tier` field in `snobol_search_meta_t`. Each tier is a
matching strategy:

| Tier | Name | Description |
|---|---|---|
| 0 | `TIER_BREAK_SCAN` | BREAK/BREAKX ASCII bitmap scan |
| 1 | `TIER_SPAN_SCAN` | SPAN ASCII bitmap scan |
| 2 | `TIER_LITERAL` | Literal-only (no VM) |
| 3 | `TIER_PREFIX` | Literal prefix (memmem/memchr) |
| 4 | `TIER_BITMAP` | Candidate bitmap (single-char alt) |
| 5 | `TIER_ALT_LIT` | Alt-of-literals trie (bushy only) |
| 6 | `TIER_SEARCH_VM` | Lightweight backtracking NFA |
| 7 | `TIER_AUTOMATON` | DFA automaton (O(n) linear scan) |
| 8 | `TIER_GENERAL` | Full SNOBOL4 VM (backtracking, side effects) |
| 9 | `TIER_SIMD_NFA` | SIMD Thompson NFA (AVX2/NEON) |

A **single-literal short-circuit** runs before tier dispatch: 1-byte literal
patterns skip the prefilter and tier table entirely (~91 ns/call vs ~158 ns).

Patterns reach their best tier automatically — the tier is derived at compile
time and reused; you only need tiers if you profile (use
`snobol_pattern_get_meta(pat)->tier` for introspection).

---

## 13. Performance Characteristics

Reference numbers (Apple Silicon / AVX2, Release build, from
`bench/c/bench_probe.c`):

- Single-byte literal tokenize (`','`): **~8 ns/call** (`search_next`),
  ~91 ns via the full search path.
- Alt-of-literals trie (`'cat'|'dog'|'fox'`): **~210 ns/iter** vs ~1170 ns
  on the general VM.
- SIMD NFA charclass scans: O(n) bitmap-skip, one bit-test per byte.
- DFA automaton: O(n) single-pass for automaton-eligible patterns.
- General VM (Tier 8) is used only for irreducibly stateful patterns
  (`EVAL`, `DYNAMIC`, `GOTO`, `BAL`, …).

Overall, SNOBOL4-style matching is ~1.3×–9.5× slower than PCRE2 on synthetic
scenarios (closest on SIMD scans, widest on alternation-heavy patterns) —
the trade for the pattern vocabulary and engine simplicity.

---

## 14. Thread Safety

The core library is **partially thread-safe**:

| Operation | Thread-safe |
|---|---|
| Pattern compilation (`snobol_pattern_compile*`) | ✅ Reentrant (per-call stack state) |
| Matching/searching (`vm_run`, `snobol_search_exec`) | ✅ Reentrant |
| Public API (create/match/search/…, except compile) | ✅ No hidden global mutation |
| **Concurrent compilation** (charclass table in `compiler.c`) | ❌ Not thread-safe |

**Best practice:** create/compile patterns from one thread (or serialise
compiles with a mutex); matching and searching can then run from any thread
without locking. Each call uses its own VM/state — no shared mutable state
during execution.

---

## 15. C++ Interop

All public headers are wrapped in `extern "C"` guards, so the library works
from C++ with no shim:

```cpp
#include <snobol/snobol.h>

int main() {
    snobol_context_t* ctx = snobol_context_create();
    snobol_pattern_t* pat = snobol_pattern_compile(ctx, "'hello'", 7, nullptr);

    snobol_match_t* m = snobol_pattern_match(pat, "hello world", 11);
    if (m && snobol_match_success(m)) {
        // ...
    }
    snobol_match_free(m);
    snobol_pattern_free(pat);
    snobol_context_destroy(ctx);
}
```

Every public header also compiles standalone (each pulls in its own
dependencies); CI compiles the full header set as C++ with both `g++` and
`clang++`. Link against the static or shared library as usual.

---

## 16. Version & ABI

```c
int major, minor, patch;
snobol_version(&major, &minor, &patch);

uint32_t api = snobol_get_api_version();   // API version (major << 16)
uint32_t abi = snobol_get_abi_version();   // ABI version (major << 16)
```

Bindings compare `snobol_get_api_version() >> 16` against their compiled
expectation to detect incompatible headers at runtime. The version string is
generated into `<snobol/version.h>` from the top-level `CMakeLists.txt`
`project(libsnobol4 VERSION X.Y.Z)` declaration at configure time.

---

## 17. Common Use Cases

### Tokenization (split on delimiter)

`snobol_pattern_search_next()` is the lean path for literal delimiters
(~8 ns/call — position + length, no allocations):

```c
/* Pattern: ',' — the single-literal delimiter (search_next is literal-only) */
snobol_pattern_t *delim = snobol_pattern_compile(ctx, "','", 3, &err);
snobol_pattern_search_state_t *st = snobol_pattern_search_state_create(
    snobol_pattern_get_bc(delim), snobol_pattern_get_bc_len(delim));

size_t pos = 0, len = 0, start = 0;
while (snobol_pattern_search_next(st, "a,b,c,d", 7, start, &pos, &len)) {
    printf("delimiter at %zu\n", pos);
    start = pos + len;   /* next occurrence */
}
```

### Extraction with captures

```c
/* 'id:' @r1(SPAN('0-9')) — capture the id digits */
snobol_pattern_t *pat = snobol_pattern_compile(
    ctx, "'id:' @r1(SPAN('0-9'))", 22, &err);
snobol_match_t *m = snobol_pattern_search(pat, "user id:12345 active", 20);
if (m && snobol_match_success(m)) {
    size_t clen = 0;
    const char *id = snobol_match_get_variable(m, "1", &clen);
    printf("id = %.*s\n", (int)clen, id);
}
```

### Validation (anchored full-subject check)

```c
/* '^' 'a' 'b'* '$' — a followed by any number of b, full subject only */
snobol_pattern_t *pat = snobol_pattern_compile(ctx, "^ 'a' 'b'* $", 12, &err);
if (snobol_match_success(snobol_pattern_match(pat, "abbb", 4)))
    printf("valid\n");
```

### Programmatic construction (Builder)

See [The Builder API](#8-the-builder-api) — the end-to-end
create → build → emit → compile → match flow.

---

## 18. Appendix: SNOBOL4 String Syntax

The full SNOBOL4 pattern language (literals, alternation, concatenation,
repetition, captures, position primitives, tables, dynamic evaluation) is
documented in the **PHP manual's appendix** at
[php-manual.md#18-appendix-snobol4-string-syntax](php-manual.md), and the
authoritative grammar lives in [core/grammar/snobol.ebnf](../core/grammar/snobol.ebnf).

Quick reference:

| Syntax | Meaning |
|---|---|
| `'literal'` | Literal string |
| `'a' 'b'` | Concatenation |
| `'a' \| 'b'` | Alternation |
| `P+` / `P?` / `P*` | One-or-more / optional / zero-or-more |
| `@name` | Capture the following pattern into the next register |
| `SPAN('0-9')` | Match 1+ chars in set |
| `BREAK(',')` / `BREAKX(',')` | Consume until char (greedy / retry) |
| `LEN(n)`, `POS(n)`, `RPOS(n)`, `TAB(n)`, `RTAB(n)` | Length & position primitives (real integer arguments) |
| `ARB`, `BAL('(',')')`, `FENCE`, `REM`, `ABORT`, `FAIL`, `SUCCEED` | Extended primitives |
| `ARBNO(p)`, `repeat(p, min, max)` | Zero-or-more / bounded repetition |
| `@name` | Capture the following pattern into the next register (`v0`, `v1`, …) |
| `P . @name` / `P $ vN` | Match-naming: capture the match of `P` into a register (extension) |
| `EMIT('text')`, `EMIT(@vN)`, `T['k']`, `T['k'] = p`, `vN = <reg>` | Output, table, and assignment forms (extension) |
| `^` / `$` | Start / end anchor (extension) |

> **SNOBOL4 compatibility:** `match()` is anchored at offset 0 by default
> (classic SNOBOL4 scans unanchored; use the `search*` API family for that
> model), `BREAK` is deterministic-greedy (classic retry semantics live in
> `BREAKX`), and `RTAB(n)` clamps on overshoot instead of failing. The full
> faithful/divergence/extension/gap classification is the ledger:
> [docs/SNOBOL4_COMPATIBILITY.md](SNOBOL4_COMPATIBILITY.md).
