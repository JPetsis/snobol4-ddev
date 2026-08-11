# Why SNOBOL4 vs PCRE

Choosing between SNOBOL4-style pattern matching (as implemented by libsnobol4) and PCRE (Perl Compatible Regular Expressions) depends on your problem domain. This guide explains the strengths and tradeoffs of each approach with concrete examples.

## Table of Contents

- [Philosophical Differences](#philosophical-differences)
- [Side-by-Side Examples](#side-by-side-examples)
  - [Tokenization](#tokenization)
  - [Email Validation](#email-validation)
  - [Nested Structures](#nested-structures)
- [Catastrophic Backtracking](#catastrophic-backtracking)
- [When to Use Which](#when-to-use-which)

## Philosophical Differences

| Dimension              | SNOBOL4 (libsnobol4)                                                                  | PCRE                                                         |
|------------------------|---------------------------------------------------------------------------------------|--------------------------------------------------------------|
| **Patterns as values** | Patterns are first-class values — constructed at runtime, composed, and passed around | Patterns are text strings compiled to opaque regex objects   |
| **Composition**        | Patterns compose naturally via concatenation, alternation, and capture                | Composition requires string concatenation of regex fragments |
| **Backtracking model** | Full backtracking with explicit control (FENCE, ABORT, SUCCEED)                       | Implicit backtracking with greedy/lazy quantifiers           |
| **Matching strategy**  | Programmatic — pattern describes _how_ to match                                       | Declarative — pattern describes _what_ to match              |
| **State**              | Full capture state, assignments, table lookups during matching                        | Backreferences, lookaheads, lookbehinds                      |
| **Learning curve**     | Different from regex — patterns as a programming language                             | Familiar to anyone who knows regex                           |

## Side-by-Side Examples

### Tokenization

Split a comma-separated string into tokens, trimming whitespace.

**PCRE:**

```php
preg_match_all('/\s*([^,]+)\s*/', "apple, banana, cherry", $matches);
```

**SNOBOL4 (libsnobol4):**

```php
use Snobol\PatternHelper;
use Snobol\Builder as B;

// SPAN skips whitespace, brk collects characters until the delimiter
// cap captures the match into register 0, assign stores it
$pattern = B::concat([B::span(' '), B::cap(0, B::brk(',')), B::assign(0, 0)]);
$result = PatternHelper::matchAll($pattern, "apple, banana, cherry");
```

The SNOBOL4 approach is more explicit: SPAN skips whitespace, BREAK collects characters until the delimiter, and capture assignments store the result.

### Email Validation

Validate an email address with a simple pattern.

**PCRE:**

```php
preg_match('/^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}$/', $email);
```

**SNOBOL4 (libsnobol4):**

```php
use Snobol\PatternHelper;
use Snobol\Builder as B;

$pattern = B::concat([
    B::pos(0),
    B::cap(0, B::span('a-z0-9._%+-')), B::assign(0, 0),
    B::lit('@'),
    B::cap(1, B::span('a-z0-9.-')), B::assign(1, 1),
    B::lit('.'),
    B::cap(2, B::span('a-z')), B::assign(2, 2),
    B::rpos(0),
]);
$result = PatternHelper::matchOnce($pattern, $email);
```

The SNOBOL4 version is longer but more explicit about each part of the address. Captures make the structure readable.

### Nested Structures

Match balanced parentheses — a classic demonstration of why regex is not always the right tool.

**PCRE:** Requires recursive patterns (PHP supports `(?R)`):

```php
preg_match('/\(([^()]*|(?R))*\)/', "(outer (inner) outer)", $matches);
```

**SNOBOL4 (libsnobol4):**

```php
use Snobol\PatternHelper;

$result = PatternHelper::matchOnce("BAL('(', ')')", "(outer (inner) outer)");
```

`BAL` is a built-in primitive — no recursion tricks needed. It matches balanced delimiter pairs natively.

## Catastrophic Backtracking

Catastrophic backtracking occurs when a regex pattern takes exponential time on certain inputs. PCRE is vulnerable by design; libsnobol4 is not.

### The Problem

This PCRE pattern can take **exponential** time on a long non-matching string:

```php
preg_match('/(a|aa|aaa)+b/', str_repeat('a', 30))
```

Each `+` and each alternation `|` creates a choice point. When the final `b` is missing, the engine tries every combination before giving up.

### How libsnobol4 Avoids It

libsnobol4's VM uses controlled backtracking with explicit choice points. The equivalent SNOBOL4 pattern:

```php
$pattern = "(('a' | 'aa' | 'aaa') ARBNO) 'b'";
```

At each choice point, the VM records only cheap scalar state plus an index into a per-thread undo trail (`vm_trail_replay` restores the abandoned thread's mutations in reverse). Choice-push is O(1) regardless of loop/emit count, and records live in a page-linked arena. The matching strategy is predictable — the engine explores choices systematically without the exponential explosion that can occur in PCRE.

### Concrete Scenario

| Input           | PCRE (matches)                        | PCRE (no match)       | libsnobol4            |
|-----------------|---------------------------------------|-----------------------|-----------------------|
| `"a"x30 + "b"`  | ~0.1 ms (greedy matches immediately)  | N/A                   | ~0.1 ms               |
| `"a"x30` (no b) | ~500 ms (backtracks all combinations) | ~10 s on longer input | ~0.1 ms (linear fail) |

### Built-in Guards

libsnobol4 includes:

- **Trail / undo-log choice stack** — O(1) push; records only `ip`, `pos`, and a trail index (no counter / write-log `memcpy`)
- **Page-linked choice-stack arena** — no realloc copy, precise peak accounting
- **Zero-width-loop bounding** — an unbounded repeat/arbno over a nullable body is capped at `subject_len + 1` iterations, so nested zero-width closures (`arbno(arbno(''))`) fail in linear time instead of exponentially
- **FENCE** — explicit backtracking cut primitive
- **ABORT** — immediate match termination
- **SUCCEED** — force success at current position

These primitives give the pattern author fine-grained control over backtracking behavior.

## When to Use Which

### Prefer libsnobol4 when:
- You need to match nested structures (balanced delimiters)
- Pattern composition is important — patterns as values
- You want predictable, linear-time matching guarantees
- You need template-based substitution with formatting expressions
- You want captures that are full programming-language variables, not backreferences

### Prefer PCRE when:
- You need compact, inline patterns — regex is more concise for simple matching
- You are working in an ecosystem where PCRE is the standard (most text editors, grep, sed)
- You need lookaheads/lookbehinds for context-dependent matching
- Performance on simple literal matching is critical (PCRE's DFA-based literal scan is very fast)
- Your team is already familiar with regular expressions

## Summary

| Aspect                                   | Winner  |
|------------------------------------------|---------|
| Nested structures (BAL)                  | SNOBOL4 |
| Catastrophic backtracking safety         | SNOBOL4 |
| Pattern composition (patterns as values) | SNOBOL4 |
| Template substitution with formatting    | SNOBOL4 |
| Concise inline matching                  | PCRE    |
| Tool/ecosystem support                   | PCRE    |
| Performance on simple patterns           | PCRE    |
| Search+Replace (capture substitution)    | SNOBOL4 |

## Performance Comparison

Benchmark results from `snobol4_probe` (diagnostic probe, 100k iterations):

| Scenario         | SNOBOL4 (ns) | PCRE2 (ns) | Ratio                 |
|------------------|--------------|------------|-----------------------|
| Literal match    | 183          | 36         | 5.1×                  |
| SPAN comma (CSV) | 269          | 30         | 9.0×                  |
| Alternation      | 237          | 31         | 7.6×                  |
| Search+Replace   | 35           | 2,558,000  | 0.00001× (50× faster) |

**Where SNOBOL4 wins**: Search+Replace is 50× faster because capture substitution is native. SNOBOL4 patterns are more readable for complex string manipulation.

**Where PCRE2 wins**: Pure matching (unanchored) is 5-9× faster due to JIT-compiled native code. PCRE2's per-byte dispatch cost (~1 ns) is lower than SNOBOL4's interpreter dispatch (~5-15 ns). Note that the snobol probe `pcre2_*` rows are unanchored-search context (`pcre2_match` scans the subject); they are not head-to-head with the snobol anchored rows. When both use the same semantics (anchored literal), SNOBOL4 is at parity: ~31 ns vs PCRE2 ~36 ns for a successful literal match at offset 0 (refreshed probe, `PROBE_ITERS=20000`; see `bench/BENCHMARKS.md`).

**SNOBOL4 optimization**: Tier dispatch via function pointer table, `search_vm_t` lightweight VM state, metadata bitfield flags, cached trie for bushy alt-of-literals (with flat alternations falling back to the general VM), SIMD-accelerated Thompson NFA (Tier 9), pattern fusion for concat chains (Tier 10 — ~50 ns/iter for date-like patterns), required-byte pre-filter (skips tier dispatch when a necessary literal is absent), and pike overflow fallback (eliminates silent false negatives) reduce base overhead. SIMD accelerates SPAN, BREAK, ANY, and NOTANY patterns on subjects >16 bytes via real NEON `vqtbl1q_u8` / AVX2 `vpshufb` vector compare.
