# libsnobol4 PHP Binding — Complete Reference

> **Version:** 1.0.2 · **API:** 0x010002 · **ABI:** 1 · **Namespaces:** `Snobol\`

---

## Table of Contents

1. [Overview](#1-overview)
2. [Installation & Setup](#2-installation--setup)
3. [Architecture Overview](#3-architecture-overview)
4. [Pattern Construction: The Builder API](#4-pattern-construction-the-builder-api)
5. [Opcode Reference (Complete)](#5-opcode-reference-complete)
   - 5.1 Core Matching Opcodes
   - 5.2 Positional Opcodes
   - 5.3 Character-Class Opcodes
   - 5.4 Capture & Assignment
   - 5.5 Control Flow
   - 5.6 Output & Template
   - 5.7 Repetition
   - 5.8 Extended Primitives
   - 5.9 Table/Array Operations
   - 5.10 Dynamic Patterns
6. [Pattern Compilation](#6-pattern-compilation)
7. [Pattern Matching](#7-pattern-matching)
8. [Search Operations](#8-search-operations)
9. [Template Substitution](#9-template-substitution)
10. [Tables & Arrays](#10-tables--arrays)
11. [Text Functions](#11-text-functions)
12. [Comparison & Type Functions](#12-comparison--type-functions)
13. [Caching](#13-caching)
14. [Tier Dispatch System](#14-tier-dispatch-system)
15. [Common Use Cases](#15-common-use-cases)
16. [Reusable Pattern Library](#16-reusable-pattern-library)
17. [Performance Characteristics](#17-performance-characteristics)
18. [Appendix: SNOBOL4 String Syntax](#18-appendix-snobol4-string-syntax)

---

## 1. Overview

libsnobol4 is a high-performance C library implementing **SNOBOL4-style pattern matching**. Unlike PCRE (Perl-Compatible Regular Expressions), SNOBOL4 uses a **pattern-as-values** model: patterns are first-class objects built from composable primitives (concatenation, alternation, repetition), not a string of special characters.

### Key Differences from PCRE/Regex

| Aspect             | PCRE/Regex                     | SNOBOL4 / libsnobol4                                        |
|--------------------|--------------------------------|-------------------------------------------------------------|
| Pattern model      | String with metacharacters     | Composable pattern objects                                  |
| Backtracking       | Implicit, greedy by default    | Explicit via choice points (`OP_SPLIT`)                     |
| Captures           | `$1`, `$2` by group position   | Named capture registers (`cap(0, sub)`, `cap(1, sub)`)      |
| Character classes  | `[a-z]` as syntax              | `SPAN`, `BREAK`, `ANY`, `NOTANY` as operators               |
| String functions   | Not part of regex              | Built-in: SIZE, TRIM, DUPL, REVERSE, SUBSTR, etc.           |
| Case-insensitivity | Flag `/i`                      | Compile-time `'caseInsensitive' => true` option             |
| Search vs match    | One model with `^`/`$` anchors | Explicit `match()` (anchored) vs `searchAll()` (unanchored) |
| Replace            | `preg_replace()`               | `searchReplace()` with template syntax `${v0.upper()}`      |
| Dynamic patterns   | `eval` inside regex (rare)     | First-class `EVAL` opcode for runtime pattern assembly      |

### When to Reach for libsnobol4

- You need **O(n) search as a lower bound** — the multi-tier engine auto-selects linear DFA automaton (Tier 7), SIMD Thompson NFA (Tier 9), or fast-path literal scans (Tier 2–3) for eligible patterns
- You want **zero-allocation literal matching** for pure-string patterns
- You need **template substitution** with `.upper()`, `.lower()`, `.lpad()`, `.length()` transforms
- You need **SNOBOL4-specific semantics** like `ARB`, `BAL` (balanced delimiters), `BREAKX` (tokenization with retry), or `FENCE` (cut)
- You want a **purely programmatic pattern construction API** (the Builder) without string interpolation concerns

---

## 2. Installation & Setup

### Build from Source

```bash
# Build C core + PHP extension
make build-php

# Or from the PHP binding directory
cd bindings/php/
cmake -B build && cmake --build build
```

### DDEV (recommended for development)

```bash
ddev start                        # First-time setup
ddev build-snobol-extension       # Rebuild PHP extension
ddev test                         # Run PHP test suite
```

### Autoload

```php
require 'vendor/autoload.php';
```

The extension registers all classes under `Snobol\` and global functions in the root namespace.

---

## 3. Architecture Overview

```
User Code (PHP)
    │
    ▼
┌─────────────────────────────┐
│   Snobol\Builder            │  ← AST construction (static methods)
│   Snobol\Pattern            │  ← Compiled pattern, matching, search
│   Snobol\PatternHelper      │  ← Convenience: matchOnce, matchAll, split, replace
│   Snobol\Table              │  ← String-keyed associative table
│   Snobol\Array_             │  ← Integer-keyed sparse array
│   Snobol\PatternCache       │  ← LRU cache for compiled patterns
│   Snobol\DynamicPatternCache│  ← LRU cache for dynamic/evaluated patterns
└──────────┬──────────────────┘
           │
           ▼ (C extension: php_snobol.c)
┌─────────────────────────────┐
│   C Core API                │
│   ┌──────────────────────┐  │
│   │ Lexer → Parser → AST │  │  ← Source string compilation
│   └──────────┬───────────┘  │
│              ▼              │
│   ┌──────────────────────┐  │
│   │ Compiler → Bytecode  │  │  ← 42 opcodes
│   └──────────┬───────────┘  │
│              ▼              │
│   ┌──────────────────────┐  │
│   │ Search Meta Derive   │  │  ← Tier classification
│   └──────────┬───────────┘  │
│              ▼              │
│   ┌──────────────────────┐  │
│   │ Tier 0-9 Dispatch    │  │  ← Auto-selected strategy
│   │  0: BREAK_SCAN       │  │
│   │  1: SPAN_SCAN        │  │
│   │  2: LITERAL (memcmp) │  │
│   │  3: PREFIX (memmem)  │  │
│   │  4: BITMAP           │  │
│   │  5: ALT_LIT (trie)   │  │
│   │  6: SEARCH_VM (NFA)  │  │
│   │  7: AUTOMATON (DFA)  │  │
│   │  8: GENERAL VM       │  │
│   │  9: SIMD_NFA (AVX2)  │  │
│   └──────────────────────┘  │
└─────────────────────────────┘
```
> **Tier 5 (`ALT_LIT`)** is used only for *bushy* alternations that share a prefix. Flat alternations with no shared prefix (e.g. `"abc"|"def"|"ghi"`) skip the trie and fall back to **tier 8 (general VM)**, which already applies the start-byte bitmap + BMH skip accelerators. Tier selection is cost-model-driven.

### Compilation Pipeline

```
SNOBOL4 source string
    → Lexer (tokenizer)
    → Parser (recursive-descent, builds AST)
    → Compiler (AST → bytecode, 42 opcodes)
    → Fusion pass (SPLIT+ANY/SPAN fusion)
    → Search metadata derivation (tier, candidate bitmaps, skip tables)
    → Pattern object ready for matching
```

Patterns can also be constructed directly via the **Builder API** (PHP arrays → AST → bytecode), bypassing the lexer/parser entirely.

---

## 4. Pattern Construction: The Builder API

`Snobol\Builder` is a **final class with all static methods**. Each method returns an **associative array** representing an AST node. Pass these arrays to `Pattern::compileFromAst()` or `PatternHelper::fromAst()`.

**C counterpart:** C users composing patterns programmatically build `ast_node_t`
trees with the `snobol_pattern_build_*()` functions (`core/include/snobol/snobol.h`)
and compile them in one call with `snobol_pattern_build_compile(ctx, root, flags,
error)` — the C equivalent of `compileFromAst()`. It takes ownership of the AST
root, runs the same post-AST pipeline as `snobol_pattern_compile_ex()`, and
returns a ready-to-match `snobol_pattern_t` (or `NULL` with a malloc'd error
string).

### Literal

```php
Builder::lit(string $s): array
```

Match literal text `$s`.

**PCRE equivalent:** `preg_quote($s)`

```php
$p = Pattern::compileFromAst(Builder::lit("hello"));
$p->match("hello world"); // matches "hello" at position 0
```

### Character Classes

```php
Builder::span(string $set): array      // Match 1+ chars IN set
Builder::brk(string $set): array       // Consume 0+ until char IN set
Builder::any(?string $set = null): array  // Match 1 char IN set (or any if null)
Builder::notany(string $set): array    // Match 1 char NOT IN set
Builder::breakx(string $set): array    // BREAK with retry on backtrack
```

**PCRE equivalents:**

| Snobol            | PCRE        | PHP                            |
|-------------------|-------------|--------------------------------|
| `span("a-z0-9")`  | `[a-z0-9]+` | `strspn()`                     |
| `brk(":")`        | `[^:]*`     | `strcspn()`                    |
| `any("aeiou")`    | `[aeiou]`   | `str_contains($set, $s[$pos])` |
| `notany("aeiou")` | `[^aeiou]`  | `!str_contains(...)`           |
| `breakx(",")`     | `(.*?),`    | tokenization loop              |
| `any()`           | `.`         | always true                    |

**Range syntax:** Strings like `"a-z"`, `"0-9"`, `"a-zA-Z"` are expanded into ranges. Hyphen at start/end is literal. Multi-byte Unicode characters can be included directly.

```php
// Digit span match
$p = Pattern::compileFromAst(Builder::span("0123456789"));
$p->match("12345abc");  // match_len = 5

// Range syntax
$p = Pattern::compileFromAst(Builder::span("a-z0-9"));
$p->match("abc123");    // match_len = 6

// BREAK until delimiter
$p = Pattern::compileFromAst(Builder::brk(":"));
$p->match("hello:world"); // match_len = 5 ("hello")
```

### Position Primitives

```php
Builder::pos(int $n): array     // Succeed only if cursor at codepoint n from start
Builder::rpos(int $n): array    // Succeed only if cursor at codepoint n from end
Builder::tab(int $n): array     // Advance cursor to codepoint n from start
Builder::rtab(int $n): array    // Advance cursor to codepoint n from end
```

**PCRE equivalents:**

| Snobol    | PCRE          | Semantics                    |
|-----------|---------------|------------------------------|
| `pos(5)`  | `(?<=^.{5})`  | Assert cursor at codepoint 5 |
| `rpos(0)` | `$`           | Assert at end of string      |
| `tab(5)`  | `.{5}`        | Skip 5 codepoints            |
| `rtab(3)` | `.*(?=.{3}$)` | Leave 3 codepoints remaining |

```php
// Assert cursor at position 3, then match "hello"
$ast = Builder::concat([Builder::pos(3), Builder::lit("hello")]);
$p = Pattern::compileFromAst($ast);
$p->match("xxhello");  // false (wrong position)
$p->match("   hello"); // true after 3 spaces

// Match remainder
$ast = Builder::concat([Builder::lit("foo"), Builder::rem()]);
$p = Pattern::compileFromAst($ast);
$p->match("foobar");   // match_len = 6
```

### Compound Patterns

```php
Builder::concat(array $parts): array       // Sequence of patterns (AND)
Builder::alt(mixed $left, mixed $right): array  // Alternation (OR)
Builder::arbno(array $sub): array          // Zero-or-more (greedy)
Builder::arb(): array                      // Shorthand for arbno(len(1))
```

**PCRE equivalents:**

| Snobol           | PCRE     | Notes                                                |
|------------------|----------|------------------------------------------------------|
| `concat([a, b])` | `ab`     | Sequence — same concept                              |
| `alt(a, b)`      | `a\|b`   | Alternation — same concept                           |
| `arbno(a)`       | `(?:a)*` | Greedy zero-or-more                                  |
| `arb()`          | `.*?`    | Match 0+ any (lazy), use with `FENCE` to avoid O(n²) |

```php
// Alternation: "foo" or "bar"
$ast = Builder::alt(Builder::lit("foo"), Builder::lit("bar"));
$p = Pattern::compileFromAst($ast);
$p->match("foo");      // true
$p->match("bar");      // true
$p->match("baz");      // false

// Concatenation: "hello world"
$ast = Builder::concat([
    Builder::lit("hello"),
    Builder::span(" "),
    Builder::lit("world")
]);

// ARB (arbitrary skip): skip to "world"
$ast = Builder::concat([Builder::arb(), Builder::lit("world")]);
$p = Pattern::compileFromAst($ast);
$p->match("hello world"); // true, matches at the right position
```

### Repetition

```php
Builder::repeat(array $sub, int $min, ?int $max = -1): array
```

Bounded repetition of pattern `$sub` for `$min` to `$max` times (inclusive). `$max = -1` means unbounded (≥min).

**PCRE equivalent:** `(?:<sub>){<min>,<max>}` or `(?:<sub>){<min>,}` for unbounded. See [§5.7](#57-repetition) for examples.

```php
// Exactly 3 a's
$ast = Builder::repeat(Builder::lit('a'), 3, 3);
$p = Pattern::compileFromAst($ast);
$p->match("aaaaa");   // match_len = 3

// 1 to 3 a's (greedy)
$ast = Builder::repeat(Builder::lit('a'), 1, 3);
$p = Pattern::compileFromAst($ast);
$p->match("aaaaa");   // match_len = 3

// At least 2 a's
$ast = Builder::repeat(Builder::lit('a'), 2);
$p = Pattern::compileFromAst($ast);
$p->match("aaa");     // match_len = 3
$p->match("a");       // false
```

### Capture & Assignment

```php
Builder::cap(int $reg, array $sub): array        // Capture sub-pattern into register
Builder::assign(int $var, int $reg): array       // Assign register to variable
```

Captures are stored in **numbered registers** (0–63). The matched text of a capture register appears in the result as `$result['v<reg>']`. Assigning to a variable makes it available for template substitution.

String patterns (`Pattern::fromString()`) support captures with the `@name` syntax: each `@name` occurrence allocates the next register in order of appearance (starting at `v0`). A zero-width or unset capture renders as `null` in string mode and as a `[position, 0]` pair in offsets mode.

```php
// Capture digits after "id:"
$ast = Builder::concat([
    Builder::lit("id:"),
    Builder::cap(0, Builder::span("0-9")),
    Builder::assign(0, 0)   // assign reg 0 → var 0
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("id:12345");
echo $res['v0'];  // "12345"
echo $res['_match_len']; // 8

// Capture with alternation
$ast = Builder::alt(
    Builder::cap(0, Builder::lit("hello")),
    Builder::cap(0, Builder::lit("goodbye"))
);
$p = Pattern::compileFromAst($ast);
$res = $p->match("goodbye");
echo $res['v0'];  // "goodbye" (only matched branch's capture)
```

### Anchors

```php
Builder::anchor(string $atype): array   // "start" or "end"
```

**PCRE equivalents:**

| Snobol            | PCRE |
|-------------------|------|
| `anchor('start')` | `^`  |
| `anchor('end')`   | `$`  |

```php
// Start anchor
$ast = Builder::concat([Builder::anchor('start'), Builder::lit('hello')]);
$p = Pattern::compileFromAst($ast);
$p->match("hello world");   // true
$p->match(" hello world");  // false

// End anchor
$ast = Builder::concat([Builder::lit('world'), Builder::anchor('end')]);
$p = Pattern::compileFromAst($ast);
$p->match("world");   // true
$p->match("world ");  // false
```

### Extended Primitives

```php
Builder::bal(?string $open = "(", ?string $close = ")"): array  // Balanced delimiter
Builder::fence(): array    // Cut — prevent backtracking past this point
Builder::rem(): array      // Match remainder of subject to end
Builder::abort(): array    // Terminate entire match immediately
Builder::fail(): array     // Force failure / trigger backtracking
Builder::succeed(): array  // Force immediate success at current position
```

**PCRE equivalents:**

| Snobol          | PCRE                     | Notes                                                |
|-----------------|--------------------------|------------------------------------------------------|
| `bal('(', ')')` | `\((?:[^()]++\|(?R))*\)` | Recursive balanced parens                            |
| `fence()`       | `(?>...)`                | Atomic group / cut                                   |
| `abort()`       | —                        | Kill entire match, all branches — no PCRE equivalent |
| `fail()`        | `(*FAIL)`                | Trigger backtrack                                    |
| `succeed()`     | empty alternative        | Force success at current position                    |

```php
// BAL: balanced parentheses
$ast = Builder::concat([
    Builder::cap(0, Builder::bal('(', ')')),
    Builder::assign(0, 0)
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("(hello (world))");
echo $res['v0'];  // "(hello (world))"

// FENCE: prevent backtracking
// Without FENCE: span("a") would backtrack to let lit("b") match
$ast = Builder::concat([Builder::span('a'), Builder::fence(), Builder::lit('b')]);
$p = Pattern::compileFromAst($ast);
$p->match("aaab");   // true (FENCE prevents span from giving up 'a's)

// REM: remainder
$ast = Builder::concat([Builder::lit('foo'), Builder::rem()]);
$p = Pattern::compileFromAst($ast);
$p->match("foobar");   // match_len = 6

// ABORT: kill entire match immediately
$ast = Builder::concat([Builder::lit('hello'), Builder::abort()]);
$p = Pattern::compileFromAst($ast);
$p->match("hello");  // false (ABORT terminates)

// FAIL: force backtrack
$ast = Builder::concat([Builder::lit('hello'), Builder::fail()]);
$p = Pattern::compileFromAst($ast);
$p->match("hello");  // false (FAIL forces backtrack, no alternatives remain)

// SUCCEED: force success
$ast = Builder::succeed();
$p = Pattern::compileFromAst($ast);
$p->match("anything");  // true (matches 0-length at position 0)
```

### Output & Emit

```php
Builder::emit(string $text): array      // Append literal to output
Builder::emitRef(int $reg): array       // Append capture register to output
```

The output buffer appears in the match result as `$result['_output']`.

```php
// Emit transformed text
$ast = Builder::concat([
    Builder::lit('h'),
    Builder::emit('H'),
    Builder::lit('e'),
    Builder::emit('E')
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("hello");
echo $res['_output'];  // "HE"

// Emit capture
$ast = Builder::concat([
    Builder::cap(1, Builder::lit('hel')),
    Builder::emitRef(1),
    Builder::lit('lo')
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("hello");
echo $res['_output'];  // "hel"
```

### Control Flow: Labels & Goto

```php
Builder::label(string $name, array $target): array   // Define label
Builder::goto(string $label): array                  // Jump to label
```

These enable looping and structured control flow in patterns.

```php
// Label + Goto for repetition
$ast = Builder::concat([
    Builder::label('loop', Builder::lit('a')),
    Builder::goto('loop'),
    Builder::lit('b')
]);
$p = Pattern::compileFromAst($ast);
// Warning: this creates an infinite loop unless bounded
```

### Dynamic Evaluation

```php
Builder::dynamicEval(array $expr): array   // Evaluate pattern at match time
```

The pattern inside `$expr` is compiled at match time rather than at pattern compile time. Useful when the pattern depends on runtime data.

```php
$pattern = Pattern::compileFromAst([
    'type' => 'dynamic_eval',
    'expr' => ['type' => 'lit', 'text' => 'A']
]);
$pattern->match('A'); // true
```

---

## 5. Opcode Reference (Complete)

All 42 VM opcodes with their PCRE/PHP equivalents and descriptions.

### 5.1 Core Matching

| Opcode            | Equivalent          | Description                                         |
|-------------------|---------------------|-----------------------------------------------------|
| `OP_ACCEPT` (0)   | —                   | Pattern succeeds at end of bytecode                 |
| `OP_FAIL` (1)     | `(*FAIL)`           | Force failure, triggers backtracking                |
| `OP_SUCCEED` (40) | `\|` (empty alt)    | Force success at current position, consumes nothing |
| `OP_LIT` (4)      | `str_starts_with()` | Match literal text at current position              |
| `OP_NOP` (41)     | —                   | No-op (fusion pass)                                 |

### 5.2 Positional

| Opcode           | Equivalent                   | Description                                                                |
|------------------|------------------------------|----------------------------------------------------------------------------|
| `OP_POS` (37)    | `$pos === $n` · `(?<=^.{n})` | Assert cursor at codepoint n from start. `pos(0)` = `^`. Builder: `pos(n)` |
| `OP_TAB` (38)    | `substr($s, $n)`             | Advance cursor to codepoint n from start. Builder: `tab(n)`                |
| `OP_RPOS` (35)   | `(?=.{n}$)`                  | Assert cursor at codepoint n from end. `rpos(0)` = `$`. Builder: `rpos(n)` |
| `OP_RTAB` (36)   | —                            | Advance cursor to leave n codepoints from end. Builder: `rtab(n)`          |
| `OP_REM` (34)    | `substr($s, $pos)` · `.*$`   | Match remainder of subject. Builder: `rem()`                               |
| `OP_ANCHOR` (14) | `^` / `$`                    | Assert subject start or end. Builder: `anchor('start'/'end')`              |

### 5.3 Character-Class

| Opcode           | Equivalent                     | Semantics                                             | Builder       |
|------------------|--------------------------------|-------------------------------------------------------|---------------|
| `OP_ANY` (5)     | `[aeiou]` · `str_contains()`   | Match **one** char in set                             | `any(set)`    |
| `OP_NOTANY` (6)  | `[^aeiou]` · `!str_contains()` | Match **one** char NOT in set                         | `notany(set)` |
| `OP_SPAN` (7)    | `[set]+` · `strspn()`          | **Greedy** — match **1+** consecutive chars in set    | `span(set)`   |
| `OP_BREAK` (8)   | `[^:]*` · `strcspn()`          | Consume **0+** until char in set (stops before it)    | `brk(set)`    |
| `OP_BREAKX` (31) | `(.*?)` with retry             | BREAK with backtrack choice point (O(n) tokenization) | `breakx(set)` |

**Character-class notes:**
- `OP_BREAKX` is like `OP_BREAK` but **creates a backtracking choice point**. On failure it retries with one fewer character consumed — this enables O(n) CSV-style tokenization.
- `OP_SPAN` is always greedy (consumes maximum). Use `OP_BREAK` / `OP_BREAKX` when you need to stop at a delimiter.
- Range syntax: strings like `"a-z"`, `"0-9"`, `"a-zA-Z"` expand into ranges. Hyphen at start/end is literal.

### 5.4 Capture & Assignment

| Opcode             | Equivalent        | Description                                                        |
|--------------------|-------------------|--------------------------------------------------------------------|
| `OP_CAP_START` (9) | `(`               | Begin capture group. Builder: embedded in `cap(reg, sub)`          |
| `OP_CAP_END` (10)  | `)`               | End capture group. Builder: embedded in `cap(reg, sub)`            |
| `OP_ASSIGN` (11)   | `$var = $capture` | Copy register `reg` to variable `var`. Builder: `assign(var, reg)` |

**Cap/Assign relationship:**
- `cap(reg, sub)` wraps `sub` in a capture group for register `reg`. The captured text appears in the match result as `$result['v<reg>']`.
- `assign(var, reg)` copies the captured text from register `reg` to variable `var`. This is primarily used for template substitution (`${v0}`).

Registers 0–63, variables 0–63. `var` and `reg` can be the same or different numbers.

```php
// Capture + assign for template use
$ast = Builder::concat([
    Builder::cap(0, Builder::span("0-9")),
    Builder::assign(0, 0)      // reg 0 → var 0
]);
// Template can now use ${v0}

// Multiple captures
$ast = Builder::concat([
    Builder::cap(0, Builder::span("a-z")),
    Builder::lit(" "),
    Builder::cap(1, Builder::span("a-z")),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("foo bar");
echo $res['v0']; // "foo"
echo $res['v1']; // "bar"
```

### 5.5 Control Flow

| Opcode            | Equivalent             | Description                                                                                                   |
|-------------------|------------------------|---------------------------------------------------------------------------------------------------------------|
| `OP_JMP` (2)      | —                      | Unconditional jump to bytecode offset                                                                         |
| `OP_SPLIT` (3)    | `\|` with backtracking | Non-deterministic branch. Push second target as choice, continue at first. Generated by `alt()` and `arbno()` |
| `OP_FENCE` (33)   | `(?>...)` atomic group | Cut choice stack — no backtracking past this point. Builder: `fence()`                                        |
| `OP_ABORT` (39)   | immediate termination  | Terminate entire match immediately. Builder: `abort()`                                                        |
| `OP_FAIL` (1)     | `(*FAIL)`              | Force failure, triggers backtracking. Builder: `fail()`                                                       |
| `OP_SUCCEED` (40) | empty alt              | Force success at current position. Builder: `succeed()`                                                       |
| `OP_LABEL` (22)   | —                      | Define a label target. Builder: `label()`                                                                     |
| `OP_GOTO` (23)    | —                      | Jump to label. Builder: `goto()`                                                                              |
| `OP_GOTO_F` (24)  | —                      | Jump to label if last match failed                                                                            |

**How `alt(left, right)` compiles:**
```
SPLIT right_target, left_target
(left bytecode...)
JMP after_alt
(right_target: right bytecode...)
(after_alt:)
```

**How `arbno(sub)` compiles:**
```
(loop:)
SPLIT after_arbno, sub_target
(sub_target: sub bytecode...)
JMP loop
(after_arbno:)
```

### 5.6 Output & Template

| Opcode                 | Builder                  | Description                       |
|------------------------|--------------------------|-----------------------------------|
| `OP_EMIT_LITERAL` (17) | `emit(text)`             | Append literal to output buffer   |
| `OP_EMIT_CAPTURE` (18) | `emitRef(reg)`           | Append capture register to output |
| `OP_EMIT_FORMAT` (21)  | `${v0.upper()}` template | Append formatted capture          |
| `OP_EMIT_EXPR` (19)    | —                        | Legacy expression emit            |
| `OP_EMIT_TABLE` (20)   | `$TABLE[key]` template   | Table-backed emit                 |

**Format types** (for `OP_EMIT_FORMAT` and `${vN.format()}` templates):

| Expression   | Value | PHP Equivalent                             | Description                        |
|--------------|-------|--------------------------------------------|------------------------------------|
| `.upper()`   | 1     | `strtoupper()`                             | ASCII/Unicode uppercase            |
| `.lower()`   | 2     | `strtolower()`                             | ASCII/Unicode lowercase            |
| `.length()`  | 3     | `strlen()` / `mb_strlen()`                 | Codepoint length as decimal string |
| `.lpad(n,c)` | 4     | `str_pad($s, $width, $pad, STR_PAD_LEFT)`  | Left-pad to width with fill char   |
| `.rpad(n,c)` | 5     | `str_pad($s, $width, $pad, STR_PAD_RIGHT)` | Right-pad to width with fill char  |

```php
// Template with format expressions
$pattern = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Builder::span("a-zA-Z0-9")),
        Builder::assign(0, 0),
    ])
);

$pattern->subst('hello', '${v0.upper()}');      // "HELLO"
$pattern->subst('HELLO', '${v0.lower()}');      // "hello"
$pattern->subst('hello', '${v0.length()}');      // "5"
$pattern->subst('42', '${v0.lpad(5,"0")}');      // "00042"
$pattern->subst('hi', '${v0.rpad(8,".")}');      // "hi......"
$pattern->subst('world', '${v0}');               // "world" (plain capture)

// Table-backed template
$table = new Table();
$table->set('sky', 'blue');
$pattern->subst('anything', "\$v0[colors['sky']]", ['colors' => $table]);
// → "blue"
```

### 5.7 Repetition

| Opcode                | Equivalent         | Description                                                     |
|-----------------------|--------------------|-----------------------------------------------------------------|
| `OP_REPEAT_INIT` (15) | `(?:sub){min,max}` | Initialize bounded repetition. Builder: `repeat(sub, min, max)` |
| `OP_REPEAT_STEP` (16) | —                  | Step loop iteration. Auto-generated.                            |

**Equivalent patterns:**

| SNOBOL4           | PCRE                          |
|-------------------|-------------------------------|
| `repeat(a, 3, 3)` | `(?:a){3}` (exactly 3)        |
| `repeat(a, 1, 3)` | `(?:a){1,3}` (1 to 3, greedy) |
| `repeat(a, 2)`    | `(?:a){2,}` (at least 2)      |
| `repeat(a, 0)`    | `(?:a)*` (zero or more)       |
| `arbno(a)`        | `(?:a)*` (zero or more)       |

### 5.8 Extended Primitives

| Opcode         | Equivalent               | Description                                                             |
|----------------|--------------------------|-------------------------------------------------------------------------|
| `OP_BAL` (32)  | `\((?:[^()]++\|(?R))*\)` | Match balanced delimiters (recursive). Builder: `bal(open, close)`      |
| `OP_LEN` (12)  | `.{n}`                   | Match exactly n codepoints. `mb_substr`-like. Builder: `len(n)` via AST |
| `OP_EVAL` (13) | `(?{code})`              | Evaluate PHP callable at match time                                     |

### 5.9 Table/Array Operations

| Opcode              | PHP Equivalent          | Description                                                 |
|---------------------|-------------------------|-------------------------------------------------------------|
| `OP_TABLE_GET` (25) | `$table[$key]`          | Lookup table entry. Builder: `tableAccess(name, key)`       |
| `OP_TABLE_SET` (26) | `$table[$key] = $value` | Set table entry. Builder: `tableUpdate(name, key, value)`   |
| `OP_ARRAY_GET` (27) | `$array[$key]`          | Lookup array entry. Builder: `arrayAccess(name, index)`     |
| `OP_ARRAY_SET` (28) | `$array[$key] = $value` | Set array entry. Builder: `arrayUpdate(name, index, value)` |

### 5.10 Dynamic Patterns

| Opcode                | Description                                                       |
|-----------------------|-------------------------------------------------------------------|
| `OP_DYNAMIC` (29)     | Evaluate dynamic pattern from pending definition                  |
| `OP_DYNAMIC_DEF` (30) | Define inline dynamic pattern block. Builder: `dynamicEval(expr)` |

---

## 6. Pattern Compilation

### From Source String (SNOBOL4 syntax)

```php
use Snobol\Pattern;

$p = Pattern::fromString("'hello'");                    // Literal
$p = Pattern::fromString("SPAN('0-9')");                // Digit span
$p = Pattern::fromString("'id:' SPAN('0-9')");          // Concatenation
$p = Pattern::fromString("'foo' | 'bar'");              // Alternation
$p = Pattern::fromString("BREAK(',')");                 // Consume until comma (Tier 0)
$p = Pattern::fromString("BREAKX(',')");                // BREAK with backtrack retry
```

See [Appendix: SNOBOL4 String Syntax](#18-appendix-snobol4-string-syntax) for the full SNOBOL4 pattern language.

### From Builder AST

```php
use Snobol\Builder;
use Snobol\Pattern;

$ast = Builder::concat([
    Builder::lit("id:"),
    Builder::cap(0, Builder::span("0-9")),
    Builder::assign(0, 0),
]);
$p = Pattern::compileFromAst($ast);
```

### With Options

```php
// Case-insensitive matching
$p = Pattern::fromString("'hello'", ['caseInsensitive' => true]);
$p->match("HELLO");   // true

// Builder with options
$p = Pattern::compileFromAst(
    Builder::span("abc"),
    ['caseInsensitive' => true]
);
$p->match("ABC");     // true
```

**Options:**

| Option            | Type   | Default | Description                                              |
|-------------------|--------|---------|----------------------------------------------------------|
| `caseInsensitive` | `bool` | `false` | Enable case-insensitive matching (ASCII + Latin-1 + BMP) |
| `cache`           | `bool` | `true`  | Enable internal pattern caching in PatternHelper         |
| `full`            | `bool` | `false` | Require full-subject match (used in `matchOnce`)         |

### Case-Insensitive Coverage

The `caseInsensitive` flag affects:
- **ASCII:** `a-z` ↔ `A-Z` (full bidirectional folding)
- **Latin-1 Supplement:** U+00C0–U+00FF pairs (e.g., `à` ↔ `À`)
- **Greek:** Full BMP Greek range (e.g., `α` ↔ `Α`)
- **Cyrillic:** Full BMP Cyrillic range (e.g., `а` ↔ `А`)
- **Latin Extended-A:** e.g., `ā` ↔ `Ā`

Caputred text **preserves original subject case** (not folded).

---

## 7. Pattern Matching

### `Pattern::match()` — Anchored Match

`match()` is the **anchored** entry point: it runs the *selected* tier once at offset 0 (via `snobol_search_exec_anchored`) rather than restarting the full VM from every position. Because of the tier dispatcher (§14), a recognizing-and-capturing pattern such as `lit("id:") + cap(span("0-9"))` is executed by the lightweight Tier-6 search-VM, not the full VM — so `match()` is typically far faster than an equivalent `^…$` PCRE match.

**Signature:**
```php
$p->match(string $subject, array $options = []): array|false
```

**Options:**

| Option     | Type      | Default     | Description                                     |
|------------|-----------|-------------|-------------------------------------------------|
| `metrics`  | `bool`    | `false`     | Include `_metrics` hash in result               |
| `captures` | `string`  | `'strings'` | `'strings'` = substring copies, `'offsets'` = `[start, len]` pairs |

```php
$p = Pattern::fromString("'hello'");
$res = $p->match("hello world");     // metrics absent
$res = $p->match("hello", ['metrics' => true]);  // includes _metrics
```

**Returns:** `array|false`

**Success result keys:**

| Key            | Type                     | Description                                        |
|----------------|--------------------------|----------------------------------------------------|
| `_match_len`   | `int`                    | Byte length of match                               |
| `_match_start` | `int`                    | Start position (always 0 for anchored match)       |
| `_output`      | `string`                 | Output buffer from `OP_EMIT_*`                     |
| `_metrics`     | `array` (opt-in)         | Choice-point push/peak depth/memory stats          |
| `v0`..`v63`    | `string\|null\|array`    | Capture values — string or `[start, len]` pair     |

When `captures => 'offsets'`, capture registers return `[start, length]` integer pairs instead of substring copies, avoiding string allocation:

```php
$p = Pattern::compileFromAst(Builder::cap(0, Builder::lit("hello")));
$res = $p->match("hello world", ['captures' => 'offsets']);
// $res['v0'] = [0, 5]   (start=0, length=5)
```

```php
$res = $p->match("hello world");
if ($res !== false) {
    echo $res['_match_len'];  // 5
    echo $res['v0'] ?? '';    // capture register 0 value
}
```

### `Pattern::matchLiteral()` — Zero-Allocation Literal Fast Path

For **pure-literal patterns only** (those that don't use span/break/etc.). Bypasses the VM entirely.

```php
$p = Pattern::fromString("'hello'");
$res = $p->matchLiteral("hello world");
// $res = ['success' => true, 'position' => 0, 'length' => 5]

$p2 = Pattern::fromString("SPAN('abc')");
$res2 = $p2->matchLiteral("abc");
// $res2 = ['success' => false] (non-literal pattern)
```

Use `matchLiteral()` when you know the pattern is a simple string — it avoids all VM setup (~50–100 ns faster). Falls through to VM for non-literal patterns.

### DFA Caching in `match()`

`match()` automatically detects automaton-eligible patterns (ASCII-only character classes, literals, and concat — no EVAL/ASSIGN side effects) and builds a **Deterministic Finite Automaton (DFA)** on the first call. The DFA is cached on the `Pattern` object and reused on subsequent calls, enabling **Tier 7 (AUTOMATON)** dispatch which runs in O(n) single-pass linear time over the subject.

This accelerates repeated matching of patterns like `SPAN('abc') 'd'` or `'hello' ANY('xyz')` — the second `match()` call reuses the cached DFA, avoiding VM setup and powerset construction.

Patterns with position constraints (POS, RPOS, ANCHOR) are excluded from automaton eligibility — they fall through to the search-VM or general VM which handle the constraints correctly.

### `Pattern::subst()` — Match + Replace Template

Run the pattern against subject and produce a string from the template.

```php
$p = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Builder::span("0-9")),
        Builder::assign(0, 0),
    ])
);
$p->subst('subject', 'template', ?$tables);
```

Parameters:
- `$subject`: The input string to match against
- `$template`: Template with `${v0}`, `${v0.upper()}`, etc.
- `$tables`: Optional array of `Snobol\Table` objects for table-backed templates

Returns `string`.

### `PatternHelper::matchOnce()` — Convenience Match

```php
use Snobol\PatternHelper;

// Three input forms:
$r = PatternHelper::matchOnce("'hello'", "hello world");         // string source
$r = PatternHelper::matchOnce(Builder::lit("hello"), "test");    // Builder AST
$r = PatternHelper::matchOnce($patternObject, "hello world");   // Pattern object

// With options:
$r = PatternHelper::matchOnce("'hello'", "hello", ['full' => true]);
```

Returns `array|false` — same shape as `Pattern::match()`.

### `PatternHelper::matchAll()` — Find All Occurrences

```php
$matches = PatternHelper::matchAll("'a'", "ababab");
// Returns array of match results (without _match_* metadata keys)
```

Returns `array` (empty if no matches). Use for **unanchored scanning** across subject.

### `PatternHelper::formattedSubst()`

```php
$result = PatternHelper::formattedSubst($patternOrAst, $template, $subject, $options);
```

Convenience wrapper for `Pattern::subst()`.

---

## 8. Search Operations

Search operations find matches **anywhere** in the subject (unanchored), not just at position 0.

All search methods use the **batch-search API** internally when the pattern is eligible
(no EVAL, ASSIGN, or DYNAMIC side effects).  For eligible patterns, the C engine
collects all match positions, lengths, captures, and output in a single pass —
eliminating per-match API boundary crossing.  Ineligible patterns fall back to the
per-call loop transparently.

The batch API now supports a **tri-state contract**:
- `true` (matches found) — normal.
- `false` with `eligible == true` — pattern was batch-eligible but found zero
  matches; the search is **DONE** and must NOT be re-run via the per-call loop.
- `false` with `eligible == false` — pattern was not batchable; caller should fall
  back to the per-call loop.

A **persistent search state** (`snobol_pattern_search_state_t`) is cached on each
`Pattern` object lazily and reused across all search calls (`searchAll`,
`searchSplit`, `searchSplitOffsets`, `searchSplitCuts`, `searchReplace`).  The
state caches range metadata (SPAN/BREAK bitmasks), the DFA (Tier 7), the
alt-literals trie (Tier 5), and the SIMD NFA (Tier 9).  These caches are built
once per `Pattern` lifetime and reused, eliminating the per-call metadata rebuild
that previously dominated small-subject searches (~220 µs → ~200 ns for
zero-match automaton-eligible patterns).

Both `result => 'flat'` and `captures => 'offsets'` result modes also route
through the batch fast path, producing identical result shapes.

All search methods accept an optional `$options` array:

| Option      | Type      | Default     | Description                                        |
|-------------|-----------|-------------|----------------------------------------------------|
| `metrics`   | `bool`    | `false`     | Include `_metrics` in per-match results            |
| `captures`  | `string`  | `'strings'` | `'strings'` or `'offsets'` (`[start, len]` pairs) |
| `result`    | `string`  | `'arrays'`  | `'arrays'` or `'flat'` (flat parallel arrays)      |

### `Pattern::searchAll()`

Find all non-overlapping matches.

```php
$p = Pattern::fromString("SPAN('0-9')");
$matches = $p->searchAll("abc 123 def 4567");
// Returns array of match-result arrays

// Flat result mode — parallel arrays, no per-match zval overhead:
$flat = $p->searchAll("abc 123 def 4567", ['result' => 'flat']);
// $flat = [
//   'match_start' => [4, 12],
//   'match_len'   => [3, 4],
//   'captures'    => ['v0' => ['123', '4567']],
//   '_output'     => ['', ''],
// ]
// Flat mode is ~1.1x faster than arrays-of-arrays (bulk of time
// is the C search loop; the batch API eliminates per-match overhead).

// Capture offsets:
$off = $p->searchAll("abc 123 def 4567", ['captures' => 'offsets']);
// Each capture register returns [start, len] instead of substring
```

### Trie Caching in Search Methods

When a pattern is a flat alternation of literals (e.g. `'cat' | 'car' | 'cab'`), the search engine uses a **Tier 5 trie-based** matcher. The trie (~7 KB) is built once from the SPLIT/LIT bytecode and cached on the `Pattern` object. On repeated `searchAll()`, `searchSplit()`, `searchSplitOffsets()`, or `searchReplace()` calls, the cached trie is passed to the search state via `vm->trie_cache`, avoiding per-call trie rebuild.

Alt-literals patterns with a shared prefix (bushy tries) benefit from the cache — the trie structure is reused across calls, eliminating the ~7 KB stack-local rebuild each time.

### `Pattern::searchAllGenerator()` — Lazy Iteration

Returns a `Snobol\SearchIterator` that lazily yields matches one at a time. The C search loop advances only when the caller iterates — callers that break after N matches pay zero cost for the remaining matches.

```php
$p = Pattern::fromString("'a'");
$it = $p->searchAllGenerator("abacad");
foreach ($it as $match) {
    echo $match['_match_start'];  // 0, then 2, then 4
    // break after first 2 matches — the 3rd is never computed
    if (++$count >= 2) break;
}
```

The SearchIterator is ideal for "find first N" patterns. For full iteration, the flat array mode of `searchAll()` is faster because it avoids per-match PHP dispatch overhead.

### `Pattern::searchSplitGenerator()` — Lazy Split Iteration

Returns a `Snobol\SplitIterator` that lazily yields segment strings one at a time. Each segment is materialized as a `zend_string` only when the caller's iteration reaches it — callers that break after N segments pay zero cost for the remaining delimiters. Uses `snobol_pattern_search_next()` internally for literal delimiters (~8 ns/call).

```php
$p = Pattern::fromString("','");
$it = $p->searchSplitGenerator("a,b,c");
foreach ($it as $i => $segment) {
    echo "$i: $segment\n";  // "0: a", "1: b", "2: c"
    // break after first 2 segments — the 3rd is never computed
    if ($i >= 1) break;
}
```

The SplitIterator is ideal for "find first N" split results. For full iteration that processes all segments, the eager `searchSplit()` is faster because it avoids per-segment PHP dispatch overhead.

### `Pattern::searchSplit()`

Split subject at matches, returning non-matching segments.

```php
$p = Pattern::fromString("SPAN('0-9')");
$segments = $p->searchSplit("abc123def456ghi");
// Returns ["abc", "def", "ghi"]

// Flat offset mode — alternating [start, len, start, len, ...]:
$flat = $p->searchSplit("abc123def456ghi", ['result' => 'flat']);
// Returns [0, 3, 6, 3, 9, 3]  →  seg0: [0,3]="abc", seg1: [6,3]="def", ...
```

**PHP equivalent:** `preg_split('/[0-9]+/', "abc123def456ghi")`

### `Pattern::searchSplitCuts()`

Return cut-point offsets — the cheapest possible split result. One flat array of longs, zero string allocation, zero sub-array allocation. Each segment `i` spans `subject[cuts[i-1]:cuts[i]]` (with `cuts[-1]` = 0).

```php
$p = Pattern::fromString("' '");
$cuts = $p->searchSplitCuts("a b c d");
// Returns [2, 4, 6]
// seg0 = subject[0:2] = "a "
// seg1 = subject[2:4] = "b "
// seg2 = subject[4:6] = "c "
// seg3 = subject[6:]  = "d"
// Note: cuts are delimiter-end positions; segments include delimiters.
```

### `Pattern::searchSplitOffsets()`

Return match positions as `[offset, length]` pairs instead of segment strings. Zero zend_string allocation.

```php
$p = Pattern::fromString("SPAN('0-9')");
$offsets = $p->searchSplitOffsets("abc123def456ghi");
// Returns [[3, 3], [9, 3]]
//           ^offset ^len  ^offset ^len

// Flat mode:
$flat = $p->searchSplitOffsets("abc123def456ghi", ['result' => 'flat']);
// Returns [3, 3, 9, 3]  —  alternating [start, len, start, len, ...]
```

**PHP equivalent:** `preg_match_all('/[0-9]+/', "abc123def456ghi", $m, PREG_OFFSET_CAPTURE)`

```php
// Empty subject
$p = Pattern::fromString("'cat' | 'dog'");
$offsets = $p->searchSplitOffsets("");
// Returns []

// No matches
$offsets = $p->searchSplitOffsets("hello");
// Returns []

// Zero-length match (empty pattern)
$p = Pattern::fromString("''");
$offsets = $p->searchSplitOffsets("abc");
// Returns [[0, 0], [1, 0], [2, 0], [3, 0]]
```

### `Pattern::searchReplace()`

Replace all matches.

```php
$p = Pattern::fromString("SPAN('0-9')");
$result = $p->searchReplace("abc123def456ghi", "[digits]");
// Returns "abc[digits]def[digits]ghi"

// Options are accepted but only metrics/captures/result affect
// replace output indirectly; the result is always a string.
$res = $p->searchReplace("foo bar", "X", ['metrics' => true]);
```

**PHP equivalent:** `preg_replace('/[0-9]+/', '[digits]', "abc123def456ghi")`

For subjects > 1 KB, `searchReplace()` runs a counting pass to pre-size the output buffer, avoiding reallocation during long replacement loops.

### `PatternHelper::split()`

```php
$p = Builder::lit(",");
$segments = PatternHelper::split($p, "a,b,c");
// Returns ["a", "b", "c"]
```

**PHP equivalent:** `explode(",", "a,b,c")`

### `PatternHelper::replace()`

```php
$p = Builder::lit("old");
$result = PatternHelper::replace($p, "new", "old text with old words");
// Returns "new text with new words"
```

### `Snobol\SearchIterator` — Lazy Match Iteration

`SearchIterator` implements PHP's `Iterator` interface for lazy iteration over search matches. Created implicitly by `Pattern::searchAllGenerator()`.

`SplitIterator` implements PHP's `Iterator` interface for lazy iteration over split segments. Created implicitly by `Pattern::searchSplitGenerator()`. Internally uses `snobol_pattern_search_next()` for literal delimiters.

```php
class SearchIterator implements Iterator {
    public static function fromPattern(Pattern $pattern, string $subject): SearchIterator;
    public function current(): mixed;
    public function key(): mixed;
    public function next(): void;
    public function valid(): bool;
    public function rewind(): void;
}
```

The iterator stores a `snobol_pattern_search_state_t` internally. Each `current()` call runs `snobol_pattern_search_ex()` once to find the next match. `foreach` works naturally:

```php
$it = SearchIterator::fromPattern($pattern, $subject);
foreach ($it as $i => $match) {
    // process match $i
    if ($i >= 10) break;  // only 10 matches computed
}
```

### Reusable search state (performance)

`Pattern::searchSplit`, `searchSplitOffsets`, `searchAll`, `searchSplitCuts`, and `searchReplace` **reuse a single persistent search state across all calls** on the same `Pattern` object. The state (VM, range_meta, DFA, trie, SIMD NFA) is lazily created on the first search call and kept alive until the `Pattern` object is destroyed. This eliminates per-call state create/destroy and DFA rebuild. For eligible patterns (no EVAL/ASSIGN/DYNAMIC side effects), the **batch-search API** uses the same persistent state via `snobol_pattern_search_batch_ex()`, so the DFA and other caches are built once and reused across calls. Ineligible and zero-match patterns also benefit: the tri-state `eligible` flag prevents redundant per-call fallback, and the persistent state avoids re-deriving metadata.

This makes repeated searching over a fixed subject materially cheaper than calling `Pattern::match` in a loop: in the diagnostic probe the reuse path runs **~50–60% faster** per search call than the one-shot convenience path (`tokenize_reuse` ≈ 88 ns vs `tokenize_conv` ≈ 136 ns). For literal-only delimiter patterns (e.g. `', '`), the per-call search cost drops further to **~8 ns/call** via `snobol_pattern_search_next()`, which bypasses the match struct and output buffer entirely. For tight tokenization loops, prefer the `searchSplit*` family over repeated `match` calls; the zero-length-allocation `searchSplitOffsets` and `searchSplitCuts` variants are the cheapest when you only need positions.

**PHP equivalent:** `str_replace("old", "new", "old text with old words")`

```php
// Advanced: replace with template
$pattern = Builder::cap(0, Builder::span("0-9"));
$result = PatternHelper::replace($pattern, "[\$v0]", "id:123 code:456");
// Returns "id:[123] code:[456]"
```

---

## 9. Template Substitution

Templates are used in `subst()`, `searchReplace()`, and `PatternHelper::replace()`.

### Template Syntax

| Syntax              | Description                 | Example                   |
|---------------------|-----------------------------|---------------------------|
| `literal text`      | Passed through as-is        | `"hello"` → `"hello"`     |
| `${v0}`             | Capture variable 0          | `"${v0}"` → captured text |
| `${v0.upper()}`     | Uppercase transform         | `"hello"` → `"HELLO"`     |
| `${v0.lower()}`     | Lowercase transform         | `"HELLO"` → `"hello"`     |
| `${v0.length()}`    | Codepoint length as string  | `"hello"` → `"5"`         |
| `${v0.lpad(5,'0')}` | Left-pad to width           | `"42"` → `"00042"`        |
| `${v0.rpad(8,'.')}` | Right-pad to width          | `"hi"` → `"hi......"`     |
| `$TABLE[key]`       | Table lookup by literal key | `$colors['sky']`          |
| `$TABLE[$v0]`       | Table lookup by capture key | `$STATE[$v0]`             |

The bracketed capture form `$v0[TABLE[key]]` / `$v0[TABLE[v1]]` is
equivalent and also supported. The table name must be a literal identifier
(≤ 255 bytes) followed by `[`; an identifier not followed by `[` is emitted
as literal text (e.g. `$version`).

### Template Transform Reference

| Expression   | C Format          | Semantics                             |
|--------------|-------------------|---------------------------------------|
| `.upper()`   | `SNBL_FMT_UPPER`  | ASCII + BMP uppercase                 |
| `.lower()`   | `SNBL_FMT_LOWER`  | ASCII + BMP lowercase                 |
| `.length()`  | `SNBL_FMT_LENGTH` | Codepoint count as decimal            |
| `.lpad(n,c)` | `SNBL_FMT_LPAD`   | Left-pad to width n with fill char c  |
| `.rpad(n,c)` | `SNBL_FMT_RPAD`   | Right-pad to width n with fill char c |

---

## 10. Tables & Arrays

### `Snobol\Table` — String-Keyed Associative Table

```php
use Snobol\Table;

$t = new Table('STATE');         // Optional name for debugging
$t->set('CA', 'California');
$t->set('NY', 'New York');

$t->get('CA');        // "California"
$t->get('XX');        // null (missing key)
$t->has('NY');        // true

$t->set('CA', null);  // Deletes the key
$t->delete('NY');     // Also deletes

$t->size();           // int
$t->clear();          // Remove all entries
```

### `Snobol\Array_` — Integer-Keyed Sparse Array

```php
use Snobol\Array_;

$a = new Array_(100);  // Optional size hint
$a->set(0, 'zero');
$a->set(42, 'forty-two');
$a->set(99, 'ninety-nine');

$a->get(42);           // "forty-two"
$a->has(0);            // true
$a->delete(42);        // removes entry

$a->size();            // int
$a->keys();            // [0, 99]
$a->values();          // ["zero", "ninety-nine"]
$a->clear();
```

**PHP equivalent:** `Snobol\Array_` is a sparse integer-keyed map where missing keys are distinct from empty values. `Snobol\Table` is a string-keyed hash table. Both use FNV-1a hashing.

### Table-Backed Templates

```php
// Pattern captures a state abbreviation
$pattern = Pattern::compileFromAst(
    Builder::cap(0, Builder::any("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))
);

// Table maps abbreviations to full names
$table = new Table('STATE');
$table->set('CA', 'California');
$table->set('NY', 'New York');

// Template uses capture-derived key
$result = $pattern->subst('CA', "\$v0[STATE['CA']]", ['STATE' => $table]);
// "California"
```

---

## 11. Text Functions

These are global functions wrapping the C core's built-in string operations.

### Size & Manipulation

| Function                                   | PHP Equivalent                             | Description                                    |
|--------------------------------------------|--------------------------------------------|------------------------------------------------|
| `snobol_text_size($s)`                     | `mb_strlen($s, 'UTF-8')`                   | Unicode codepoint count                        |
| `snobol_text_trim($s)`                     | `rtrim($s)`                                | Right-trim whitespace (SNOBOL4 TRIM)           |
| `snobol_text_dupl($s, $n)`                 | `str_repeat($s, $n)`                       | Repeat string N times                          |
| `snobol_text_reverse($s)`                  | `strrev(utf8_encode(...))`                 | Reverse codepoints (Unicode-safe)              |
| `snobol_text_substr($s, $pos, $len)`       | `mb_substr($s, $pos-1, $len, 'UTF-8')`     | Extract substring by codepoint (1-based!)      |
| `snobol_text_replace($s, $from, $to)`      | `str_replace($from, $to, $s)`              | Replace all occurrences                        |
| `snobol_text_replace_char($s, $from, $to)` | `strtr($s, $from, $to)`                    | Character-by-character translation (like `tr`) |
| `snobol_text_lpad($s, $width, $pad)`       | `str_pad($s, $width, $pad, STR_PAD_LEFT)`  | Left-pad to codepoint width                    |
| `snobol_text_rpad($s, $width, $pad)`       | `str_pad($s, $width, $pad, STR_PAD_RIGHT)` | Right-pad to codepoint width                   |
| `snobol_text_char($cp)`                    | `mb_chr($cp, 'UTF-8')`                     | Codepoint → UTF-8 character                    |
| `snobol_text_ord($s)`                      | `mb_ord($s, 'UTF-8')`                      | First codepoint of string                      |
| `snobol_text_upper($s)`                    | `mb_strtoupper($s, 'UTF-8')`               | BMP uppercase                                  |
| `snobol_text_lower($s)`                    | `mb_strtolower($s, 'UTF-8')`               | BMP lowercase                                  |

### SUBSTR Note

`snobol_text_substr` uses **1-based codepoint positions** (SNOBOL4 convention), unlike PHP's 0-based byte offsets.

```php
snobol_text_substr("hello world", 7, 5);  // "world" (position 7 codepoint, 5 codepoints)
snobol_text_substr("café", 3, 2);          // "fé"
snobol_text_substr("hello", 0, 3);         // false! (invalid: positions start at 1)
```

### Unicode Coverage

All text functions operate on UTF-8 strings and are Unicode-safe:
- `SIZE`, `REVERSE`, `SUBSTR` — codepoint-aware
- `UPPER`, `LOWER` — BMP case folding (ASCII, Latin-1, Greek, Cyrillic, Latin Extended-A)
- `CHAR`, `ORD` — full Unicode support (up to U+10FFFF)
- `TRIM`, `DUPL`, `REPLACE`, `REPLACE_CHAR`, `LPAD`, `RPAD` — byte-level (Unicode-safe by passing through)

---

## 12. Comparison & Type Functions

### Identity & Lexical

| Function                     | PHP Equivalent | Description           |
|------------------------------|----------------|-----------------------|
| `snobol_text_ident($a, $b)`  | `$a === $b`    | Exact string identity |
| `snobol_text_differ($a, $b)` | `$a !== $b`    | Strings differ        |
| `snobol_text_lexeq($a, $b)`  | `$a === $b`    | Lexical equality      |
| `snobol_text_lexlt($a, $b)`  | `$a < $b`      | Lexical less-than     |
| `snobol_text_lexgt($a, $b)`  | `$a > $b`      | Lexical greater-than  |

### Numeric Comparison (SNOBOL4 semantics)

SNOBOL4 comparisons **convert strings to numbers** before comparing. Non-numeric strings become `0.0`.

| Function                 | PHP Equivalent            | True Examples                                                       |
|--------------------------|---------------------------|---------------------------------------------------------------------|
| `snobol_text_eq($a, $b)` | `(float)$a === (float)$b` | `eq("5", "5.0")`, `eq("1e2", "100")`, `eq("abc", "xyz")` (both 0.0) |
| `snobol_text_ne($a, $b)` | `(float)$a !== (float)$b` | `ne("5", "6")`                                                      |
| `snobol_text_lt($a, $b)` | `(float)$a < (float)$b`   | `lt("3", "7")`                                                      |
| `snobol_text_gt($a, $b)` | `(float)$a > (float)$b`   | `gt("7", "3")`                                                      |
| `snobol_text_le($a, $b)` | `(float)$a <= (float)$b`  | `le("-5", "3")`                                                     |
| `snobol_text_ge($a, $b)` | `(float)$a >= (float)$b`  | `ge("7", "3")`                                                      |

**Important:** `snobol_text_eq("abc", "xyz")` returns `true` because both convert to `0.0`. This is the SNOBOL4 semantic. Use `IDENT` for string identity.

### Type Predicates

| Function                  | Description                     | True Examples                   |
|---------------------------|---------------------------------|---------------------------------|
| `snobol_text_integer($s)` | String represents an integer    | `"123"`, `"-456"`, `"+789"`     |
| `snobol_text_real($s)`    | String represents a real number | `"12.34"`, `"1.23e-4"`, `"123"` |
| `snobol_text_numeric($s)` | `INTEGER` OR `REAL`             | `"123"`, `"12.34"`              |

---

## 13. Caching

### `Snobol\PatternCache` — LRU Pattern Cache

Explicit LRU cache for compiled patterns. Uses **access-order** eviction.

```php
use Snobol\PatternCache;
use Snobol\PatternHelper;
use Snobol\Builder;

$cache = new PatternCache(128);  // Default capacity

// Get or create
$pattern = $cache->get('my-pattern', fn() =>
    PatternHelper::fromAst(Builder::lit("hello"))
);

$cache->has('my-pattern');   // bool
$cache->size();              // int
$cache->clear();             // Flush all
```

**Use case:** When compiling the same pattern string repeatedly (e.g., in a loop).

### `Snobol\DynamicPatternCache` — Dynamic Pattern Cache

LRU cache for dynamically-evaluated patterns (from `EVAL` opcode). Tracks compile and evaluate counts.

```php
use Snobol\DynamicPatternCache;

$cache = new DynamicPatternCache(128);

// Compile (returns compile status)
$r = $cache->compile("'A' | 'B'");
// $r = ['cached' => false, 'pattern' => "'A' | 'B'", 'compiled' => true]

// Compile again (cached)
$r = $cache->compile("'A' | 'B'");
// $r = ['cached' => true, ...]

// Evaluate (compile + match in one shot)
$r = $cache->evaluate("'A' | 'B'", "B");
// $r = ['cached' => false, 'evaluated' => true, 'matches' => [...]]

// Inspect
$cache->get("'A' | 'B'");    // ['found' => true, 'pattern' => Pattern]
$cache->stats();              // ['size', 'max_size', 'compile_count', 'evaluate_count']
$cache->clear();              // Flush
```

### Internal PatternHelper Cache

`PatternHelper::matchOnce()`, `matchAll()`, `split()`, and `replace()` maintain an internal **128-slot ring buffer cache** keyed by AST hash. Disable with `['cache' => false]`.

```php
// Bypass cache for this call
PatternHelper::matchOnce($ast, $subject, ['cache' => false]);

// Flush internal cache
PatternHelper::clearCache();
```

---

## 14. Tier Dispatch System

libsnobol4's search engine automatically selects the optimal matching strategy based on pattern analysis. This happens **once at compile time** during metadata derivation, and is used by both `searchAll()` (unanchored, scans every offset) and `match()` (anchored, runs the selected tier once at offset 0).

### Tier Summary

| Tier | Name         | Speed      | Eligible Patterns                                   | Engine                                            |
|------|--------------|------------|-----------------------------------------------------|---------------------------------------------------|
| 0    | `BREAK_SCAN` | ★★★★★ | BREAK/BREAKX at root                                | Direct bitmap scan                                |
| 1    | `SPAN_SCAN`  | ★★★★★ | SPAN at root                                        | Direct bitmap scan                                |
| 2    | `LITERAL`    | ★★★★★ | Pure literal only                                   | memcmp                                            |
| 3    | `PREFIX`     | ★★★★★ | Literal prefix pattern                              | memmem + BMH skip                                 |
| 4    | `BITMAP`     | ★★★★☆ | Single-char alternation                             | 128-bit candidate bitmap                          |
| 5    | `ALT_LIT`    | ★★★★☆ | Flat literal alternation                            | Trie-based matching                               |
| 6    | `SEARCH_VM`  | ★★★★☆ | Charclass + positional + captures + bounded repeat  | Lightweight VM (~424 bytes)                       |
| 7    | `AUTOMATON`  | ★★★★☆ | Tier 6 set, ASCII-only, no captures                 | DFA via powerset construction                     |
| 8    | `GENERAL`    | ★★☆☆☆ | Any pattern                                         | Full VM (~2.5 KB) + start-byte bitmap             |
| 9    | `SIMD_NFA`   | ★★★★☆ | Tier 6 set, ASCII-only, no captures, SIMD available | Vectorized NEON vqtbl1q_u8 / AVX2 vpshufb inner loop (cost-model selected) |
| 10   | `FUSED_AUTOMATON` | ★★★★★ | Concat of fusible ops (LIT/SPAN/ANY/NOTANY/BREAK), no captures | Flat segment list execution (no VM, no bytecode dispatch) |

### What Triggers Each Tier

- **Tier 0** — Pattern whose first effective opcode is `OP_BREAK` or `OP_BREAKX` with ASCII-only charclass
- **Tier 1** — Pattern whose first effective opcode is `OP_SPAN` with ASCII-only charclass
- **Tier 2** — Pattern that is only `OP_LIT` instructions (no SPAN/BREAK/ANY/etc.)
- **Tier 3** — Pattern with a literal prefix (starts with one or more `OP_LIT`)
- **Tier 4** — Pattern whose root is `OP_SPLIT` with all branches being single-character match and no captures
- **Tier 5** — Pattern whose root is flat alternation of literal strings
- **Pre-filter** (before any tier): A required-byte study identifies the rightmost literal before ACCEPT/SUCCEED. Both search entry points — `snobol_search_exec` (unanchored) and `snobol_search_exec_anchored` (anchored, used by `Pattern::match()`) — run `memchr`/`memmem` before any tier handler; if the required literal is absent, the call returns immediately with `prefilter_skip=true`, bypassing all tiers. No-op for patterns without a provably required literal.
- **Tier 6** — Patterns eligible for the lightweight search-VM: charclass ops (`SPAN`/`BREAK`/`BREAKX`/`ANY`/`NOTANY`), positional ops (`POS`/`TAB`/`RPOS`/`RTAB`/`REM`/`ANCHOR`/`FENCE`), **captures** (`CAP_START`/`CAP_END`), bounded repeats (`REPEAT_INIT`/`REPEAT_STEP`), and `ARB`/`ARBNO` — i.e. the NFA-compatible subset **with captures**, provided no stateful/side-effect op (`EVAL`, `DYNAMIC`, `TABLE_*`, `ARRAY_*`, `EMIT_*`, `GOTO`/`LABEL`, `BAL`) is present.  Execution uses the Pike/TDFA single-pass scan (pike_scan) by default for O(n) matching; falls back to the per-offset restart loop for anchored-only or non-zero start offsets. On thread-buffer overflow, pike_scan falls back to the restart loop (no silent false negatives).
- **Tier 7** — Tier 6-eligible pattern that is ASCII-only charclass and **capture-free**, and DFA powerset construction succeeds (< 4096 states). Only promoted when the pattern has `has_bmh_skip` (literal prefix ≥ 2 bytes) to avoid O(n²) per-position trial loop for 1-byte literals.
- **Tier 8** — Everything else (fallback through full VM)
- **Tier 9** — Single charclass op (SPAN/BREAK/ANY/NOTANY) + terminator, ASCII-only charclass, **capture-free**, and platform has AVX2/NEON.  Vectorized inner loop: NEON uses `vld1q_u8` + 256-byte membership table with `vqtbl1q_u8` lookup (16 bytes/cycle), AVX2 uses `vpshufb` + `vpmovmskb` (32 bytes/cycle). Tail bytes <16 (<32 for AVX2) fall through to scalar. Cached on the pattern search state (built once, reused across calls).
- **Tier 10** — Concat of fusible ops (LIT/SPAN/ANY/NOTANY/BREAK) with no captures, EVAL, or side effects, and ≤32 segments. The pattern is compiled into a flat segment list at compile time and executed directly without VM overhead. Single-character alternations (e.g., `('-' | '/')`) are optimized to FUSION_CHAR by the compiler. Multi-character alternations with different lengths fall back to the VM.

### Performance Impact

```
Pattern Type                     → Tier       → Speed (C, ns/iter)
──────────────────────────────────────────────────────────────────
Pure literal (match, success)      Tier 2       ~31 ns
Pure literal (match, fail)         Tier 2       ~55 ns (prefilter reject)
Pure literal at offset 16 (rej)    Tier 2       ~31 ns
SPAN(',') (match)                  Tier 0–1     ~85 ns
BREAK(',') (match)                 Tier 0       ~32 ns
Alternation (match)                Tier 4       ~56 ns
Alt-of-literals (match)            Tier 5       ~45 ns
Automaton pattern (match)          Tier 7       ~31 ns
Fused concat (match)               Tier 10      ~34 ns
Fused concat (unanchored)          Tier 10      ~150 ns
SPAN('0-9') on 1KB digits          Tier 9       ~501 ns
SPAN('0-9') on 1KB letters (miss)  Tier 9       ~566 ns
SIMD NOTANY miss                   Tier 9       ~566 ns
Tokenize per pass                  Tier 2–3     ~100 µs
Tokenize per call                  Tier 2–3     ~25 ns
Pike overflow (BREAKX, anchored)   Tier 0–6     ~2.6 µs
Required-byte miss (prefilter)     —            ~25 ns
Zero-progress guard (anchored)     Tier 6       ~25 ns

Batch all-matches (pass unit):
Alt-of-literals all-matches        Tier 5       ~3.2 µs
Residue repeat all (capture)       Tier 6       ~230 ns
Residue zero-width all              Tier 6       ~113 ns
Pike overflow all                  Tier 0–6     ~1.1 µs
Prefilter miss all                 Tier 3       ~129 ns
Zero-progress all                  Tier 6       ~111 ns

PHP binding overhead (pass unit):
Alt-of-literals all-matches        Tier 5       ~16 µs (5× C)
Prefilter miss all                 Tier 3       ~239 ns (1.9× C)
Zero-progress all                  Tier 6       ~225 ns (2× C)
General pattern          Tier 8       ~500-1500 ns
```

For the PHP binding, add ~50–300 ns overhead for the extension bridge, depending on call frequency.

---

## 15. Common Use Cases

### 1. Substring Extraction (like `preg_match`)

```php
// SNOBOL4: "id:" SPAN("0-9")
// PCRE:    /id:(\d+)/
$ast = Builder::concat([
    Builder::lit("id:"),
    Builder::cap(0, Builder::span("0-9")),
    Builder::assign(0, 0),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("id:12345");
echo $res['v0'];  // "12345"
```

### 2. Split by Delimiter (like `explode` / `preg_split`)

```php
// PCRE: preg_split('/\s+/', $subject)
$p = Pattern::fromString("SPAN(' ")");
$parts = $p->searchSplit("hello   world  foo");
// ["hello", "world", "foo"]

// Comma split: like explode(",")
$p = Pattern::compileFromAst(Builder::lit(","));
$parts = PatternHelper::split($p, "a,b,c");
// ["a", "b", "c"]
```

### 3. Validate Email Format (Simplified)

```php
// PCRE: /^[a-z0-9._%+-]+@[a-z0-9.-]+\.[a-z]{2,}$/i
$local = Builder::span("a-z0-9._%+-");
$domain = Builder::span("a-z0-9.-");
$tld = Builder::repeat(
    Builder::span("a-z"),
    2
);

$ast = Builder::concat([
    Builder::anchor('start'),
    $local,
    Builder::lit("@"),
    $domain,
    Builder::lit("."),
    $tld,
    Builder::anchor('end'),
]);

$p = Pattern::compileFromAst($ast, ['caseInsensitive' => true]);
$res = $p->match("user@example.com");  // true
```

### 4. Balanced Parentheses Matching

```php
// PCRE: /\( (?: [^()]++ | (?R) )* \)/x  (recursive, complex)
// SNOBOL4: BAL('(', ')')
$ast = Builder::concat([
    Builder::cap(0, Builder::bal('(', ')')),
    Builder::assign(0, 0),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("prefix (hello (nested)) suffix");
echo $res['v0'];  // "(hello (nested))"
```

### 5. CSV Tokenization (BREAKX)

```php
// PCRE: preg_match_all('/("(?:[^"]|"")*"|[^,]*),?/', ...)
// SNOBOL4: BREAKX(',') for each column
$ast = Builder::concat([
    Builder::cap(0, Builder::breakx(',')),
    Builder::assign(0, 0),
    Builder::lit(','),
    Builder::cap(1, Builder::breakx(',')),
    Builder::assign(1, 1),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("hello,world");
echo $res['v0'];  // "hello"
echo $res['v1'];  // "world"
```

### 6. Search and Replace with Transform

```php
// Pattern: capture word characters
$pattern = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Builder::span("a-zA-Z")),
        Builder::assign(0, 0),
    ])
);

// Replace each word with its uppercase version
$result = PatternHelper::replace(
    $pattern,
    "\${v0.upper()}",
    "hello world from snobol"
);
// "HELLO WORLD FROM SNOBOL"
```

### 7. Fixed-Width Field Parsing

```php
// PCRE: /^(.{5})(.{3})(.{4})/
// SNOBOL4: LEN(5) LEN(3) LEN(4)
$ast = Builder::concat([
    Builder::cap(0, Builder::len(5)),      // Remember: len(5) via AST type!
    Builder::cap(1, Builder::len(3)),
    Builder::cap(2, Builder::len(4)),
]);
```

Note: `LEN(n)` is available through AST construction but not directly through `Builder::len()` currently.

### 8. Date Format Parsing

```php
// PCRE: /^(\d{4})-(\d{2})-(\d{2})$/
$digits = Builder::span("0-9");

$ast = Builder::concat([
    Builder::anchor('start'),
    Builder::cap(0, Builder::repeat($digits, 4, 4)),
    Builder::lit("-"),
    Builder::cap(1, Builder::repeat($digits, 2, 2)),
    Builder::lit("-"),
    Builder::cap(2, Builder::repeat($digits, 2, 2)),
    Builder::anchor('end'),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("2024-01-15");
echo $res['v0'];  // "2024"
echo $res['v1'];  // "01"
echo $res['v2'];  // "15"
```

### 9. String Padding (like `str_pad`)

```php
// LPAD: zero-pad numbers
$result = \snobol_text_lpad("42", 5, "0");  // "00042"

// RPAD: right-pad with dots
$result = \snobol_text_rpad("hello", 10, ".");  // "hello....."

// Via template:
$pattern->subst("42", '${v0.lpad(5,"0")}');  // "00042"
```

### 10. Balanced Bracket Matching for JSON-like Structure

```php
// Deeply nested brackets
$ast = Builder::concat([
    Builder::cap(0, Builder::bal('[', ']')),
    Builder::assign(0, 0),
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("[outer [inner [deep]] tail]");
echo $res['v0'];  // "[outer [inner [deep]] tail]"
```

### 11. Replace Character Translation (like `strtr`)

```php
// Rot13 with REPLACE_CHAR
$result = \snobol_text_replace_char(
    "hello",
    "abcdefghijklmnopqrstuvwxyz",
    "nopqrstuvwxyzabcdefghijklm"
);  // "uryyb"
```

### 12. Word Tokenization with Position

```php
// Capture words using SPAN + enforce position
$ast = Builder::concat([
    Builder::pos(0),                       // Start at position 0
    Builder::cap(0, Builder::span("a-z")),  // First word
    Builder::span(" "),                     // Skip spaces
    Builder::cap(1, Builder::span("a-z")),  // Second word
]);
$p = Pattern::compileFromAst($ast);
$res = $p->match("hello world");
echo $res['v0'];  // "hello"
echo $res['v1'];  // "world"
```

### 13. Fast Contains Check (Literal Only)

```php
// PCRE: preg_match('/' . preg_quote($needle) . '/', $haystack)
// Fastest snobol path: matchLiteral (zero-allocation)
$p = Pattern::fromString("'needle'");
$res = $p->matchLiteral("haystack with needle inside");
if ($res['success']) {
    echo "Found at position {$res['position']}";
}
```

### 14. Numeric Validation

```php
// Integer check
$ast = Builder::concat([
    Builder::anchor('start'),
    Builder::any("+-"),
    Builder::span("0-9"),
    Builder::anchor('end'),
]);
$p = Pattern::compileFromAst($ast);
$p->match("+42");   // true
$p->match("12.5");  // false (decimal not in span set)
```

---

## 16. Reusable Pattern Library

For patterns used frequently across a project, define them once as static methods on a helper class. This avoids repeated compilation and centralises pattern logic.

### 16.1 Base Helper Class

```php
use Snobol\Builder;
use Snobol\Pattern;

class Patterns
{
    /**
     * Match an empty string (zero-length).
     */
    public static function empty(): Pattern
    {
        return Builder::len(0);
    }

    /**
     * Match any single character (identical to ARB with length 1).
     */
    public static function anyChar(): Pattern
    {
        return Builder::arb();
    }

    // -----------------------------------------------------------------
    // Character classes
    // -----------------------------------------------------------------

    /**
     * Match any single ASCII digit (0123456789).
     */
    public static function digit(): Pattern
    {
        return Builder::any('0-9');
    }

    /**
     * Match any single ASCII letter
     * (ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz).
     */
    public static function letter(): Pattern
    {
        return Builder::any('A-Za-z');
    }

    /**
     * Match any single whitespace character (space, tab, newline).
     */
    public static function whitespace(): Pattern
    {
        return Builder::any(" \t\n\r\f\v");
    }

    /**
     * Match any single hex digit (0123456789ABCDEFabcdef).
     */
    public static function hexDigit(): Pattern
    {
        return Builder::any('0-9A-Fa-f');
    }

    /**
     * Match any single alphanumeric character
     * (ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789).
     */
    public static function alnum(): Pattern
    {
        return Builder::any('A-Za-z0-9');
    }

    // -----------------------------------------------------------------
    // Negated character classes
    // -----------------------------------------------------------------

    /**
     * Match any character NOT in (0123456789).
     */
    public static function notDigit(): Pattern
    {
        return Builder::notany('0-9');
    }

    /**
     * Match any character NOT in
     * (ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz).
     */
    public static function notLetter(): Pattern
    {
        return Builder::notany('A-Za-z');
    }

    /**
     * Match any character that is NOT whitespace.
     */
    public static function notWhitespace(): Pattern
    {
        return Builder::notany(" \t\n\r\f\v");
    }

    // -----------------------------------------------------------------
    // Structural patterns
    // -----------------------------------------------------------------

    /**
     * One or more digits (greedy).
     */
    public static function integer(): Pattern
    {
        return Builder::arbno(static::digit());
    }

    /**
     * An unsigned integer: one or more digits.
     */
    public static function unsignedInt(): Pattern
    {
        return Builder::arbno(static::digit());
    }

    /**
     * A signed integer: optional [+/-] followed by one or more digits.
     */
    public static function signedInt(): Pattern
    {
        return Builder::alt(
            Builder::concat([
                Builder::any('+-'),
                Builder::arbno(static::digit()),
            ]),
            Builder::arbno(static::digit())
        );
    }

    /**
     * A floating-point number: optional sign, digits, optional [.digits].
     */
    public static function float(): Pattern
    {
        return Builder::alt(
            Builder::concat([
                Builder::any('+-'),
                static::unsignedInt(),
                Builder::alt(
                    Builder::concat([
                        Builder::lit('.'),
                        Builder::arbno(static::digit()),
                    ]),
                    Builder::lit('')  // option without fractional part
                ),
            ]),
            Builder::concat([
                Builder::arbno(static::digit()),
                Builder::alt(
                    Builder::concat([
                        Builder::lit('.'),
                        Builder::arbno(static::digit()),
                    ]),
                    Builder::lit('')
                ),
            ])
        );
    }

    /**
     * A simple word: one or more alphanumeric characters.
     */
    public static function word(): Pattern
    {
        return Builder::arbno(static::alnum());
    }

    /**
     * A quoted string (double quotes, no escapes).
     */
    public static function doubleQuotedString(): Pattern
    {
        return Builder::concat([
            Builder::lit('"'),
            Builder::arbno(Builder::notany('"')),
            Builder::lit('"'),
        ]);
    }

    /**
     * A quoted string (single quotes, no escapes).
     */
    public static function singleQuotedString(): Pattern
    {
        return Builder::concat([
            Builder::lit("'"),
            Builder::arbno(Builder::notany("'")),
            Builder::lit("'"),
        ]);
    }

    /**
     * A quoted string (either single or double quotes, no escapes).
     */
    public static function quotedString(): Pattern
    {
        return Builder::alt(
            static::doubleQuotedString(),
            static::singleQuotedString()
        );
    }

    /**
     * Parenthesised content: '(' ... ')' with no nesting.
     */
    public static function parenthesised(): Pattern
    {
        return Builder::concat([
            Builder::lit('('),
            Builder::arbno(Builder::notany(')')),
            Builder::lit(')'),
        ]);
    }

    /**
     * Comma-separated list (simple, no nesting):
     * tokens separated by optional-whitespace commas optional-whitespace.
     *
     * Tokens are non-empty sequences of non-comma, non-whitespace characters.
     * Returns the whole match (including commas). For individual tokens,
     * use searchSplit() with the token pattern.
     */
    public static function commaList(): Pattern
    {
        $token = Builder::arbno(Builder::notany(','));
        return Builder::arbno(
            Builder::concat([
                $token,
                Builder::span(','),
            ])
        );
    }

    /**
     * An email address (simplified; does not validate TLD length, RFC5321
     * quirks, etc.).
     */
    public static function emailSimple(): Pattern
    {
        $local  = Builder::arbno(Builder::any('A-Za-z0-9.!#$%&\'*+/=?^_`{|}~-'));
        $domain = Builder::arbno(Builder::any('A-Za-z0-9.-'));
        return Builder::concat([
            $local,
            Builder::lit('@'),
            $domain,
        ]);
    }

    // -----------------------------------------------------------------
    // URL / path patterns
    // -----------------------------------------------------------------

    /**
     * A file path segment (no slash, no null).
     */
    public static function pathSegment(): Pattern
    {
        return Builder::arbno(Builder::notany("/\0"));
    }

    /**
     * A POSIX-style absolute path: '/foo/bar/baz'.
     */
    public static function absolutePath(): Pattern
    {
        return Builder::concat([
            Builder::lit('/'),
            Builder::arbno(
                Builder::concat([
                    Builder::arbno(Builder::notany("/\0")),
                    Builder::lit('/'),
                ])
            ),
            Builder::arbno(Builder::notany("/\0")),
        ]);
    }

    // -----------------------------------------------------------------
    // Utility / production patterns
    // -----------------------------------------------------------------

    /**
     * Strip HTML/XML tags (including self-closing).
     *
     * Matches '<' + content up to '>' + '>'. Use with searchReplace()
     * to remove all tags from a string.
     *
     * ```php
     * $clean = Pattern::searchReplace($html, Patterns::stripTags(), '');
     * ```
     */
    public static function stripTags(): Pattern
    {
        return Builder::concat([
            Builder::lit('<'),
            Builder::brk('>'),
            Builder::lit('>'),
        ]);
    }

    /**
     * Match an HTTP(S) URL (simplified — no IP-literal, no userinfo).
     *
     * Captures: scheme, host, port, path, query, fragment as consecutive
     * captured groups when used with searchSplitOffsets().
     */
    public static function urlSimple(): Pattern
    {
        $scheme = Builder::concat([
            Builder::any('hH'),
            Builder::any('tT'),
            Builder::any('tT'),
            Builder::any('pP'),
            Builder::arbno(Builder::any('sS')),
            Builder::lit('://'),
        ]);
        $host   = Builder::arbno(Builder::any('A-Za-z0-9.-'));
        $port   = Builder::arbno(static::digit());
        $path   = Builder::arbno(Builder::notany("?#\0"));
        $query  = Builder::arbno(Builder::notany("#\0"));
        $frag   = Builder::arbno(Builder::any('A-Za-z0-9%-._~:/?#[]@!$&\'()*+,;='));
        return Builder::concat([
            $scheme,
            $host,
            Builder::alt(
                Builder::concat([Builder::lit(':'), $port]),
                Builder::lit('')
            ),
            $path,
            Builder::alt(
                Builder::concat([Builder::lit('?'), $query]),
                Builder::lit('')
            ),
            Builder::alt(
                Builder::concat([Builder::lit('#'), $frag]),
                Builder::lit('')
            ),
        ]);
    }

    /**
     * A single line of text (up to but not including a newline).
     */
    public static function line(): Pattern
    {
        return Builder::arbno(Builder::notany("\n\r"));
    }

    /**
     * A UUID (hex digits with dashes, e.g. 550e8400-e29b-41d4-a716-446655440000).
     */
    public static function uuid(): Pattern
    {
        return Builder::concat([
            Builder::len(8), static::hexDigit(),
            Builder::lit('-'),
            Builder::len(4), static::hexDigit(),
            Builder::lit('-'),
            Builder::len(4), static::hexDigit(),
            Builder::lit('-'),
            Builder::len(4), static::hexDigit(),
            Builder::lit('-'),
            Builder::len(12), static::hexDigit(),
        ]);
    }

    /**
     * A two-letter ISO 3166-1 alpha-2 country code (case-insensitive match).
     *
     * Matches any two ASCII letters without validating the specific code.
     */
    public static function countryCodeAlpha2(): Pattern
    {
        return Builder::len(2, static::letter());
    }

    /**
     * IPv4 address: four dot-separated decimal octets (simple form — does
     * not reject values > 255).
     */
    public static function ipv4Simple(): Pattern
    {
        $octet = Builder::concat([
            Builder::arbno(static::digit()),
            Builder::span(''),
        ]);
        return Builder::concat([
            $octet, Builder::lit('.'),
            $octet, Builder::lit('.'),
            $octet, Builder::lit('.'),
            $octet,
        ]);
    }
}
```

### 16.2 Usage Reference

The table below maps each `Patterns::*()` helper to the best API functions for common
tasks, with PHP/PCRE equivalents as a familiar reference point.

| Patterns helper | `match()` | `searchAll()` | `searchSplit()` | `searchReplace()` | `subst()` | `PatternHelper` | Good for | PCRE equivalent |
|---|---|---|---|---|---|---|---|---|
| **Literals & character classes** | | | | | | | | |
| `literal('abc')` | ★ | ★ | ★ | ★ | ★ | ★ | Exact match, find, split, replace | `abc` |
| `any('A-Z')` | ★ | ★ | ★ | | | ★ | Validate single char is in set | `[A-Z]` |
| `notAny('A-Z')` | ★ | ★ | ★ | | | ★ | Validate single char not in set | `[^A-Z]` |
| `span('0-9')` | | | ★ | | | ★ | Consume consecutive chars from set | `[0-9]+` (greedy) |
| `brk(',;')` | | | ★ | | | | Up-to-delimiter split | `[^,;]*` (in split context) |
| `len(5)` | ★ | | | | | ★ | Fixed-width field validation | `.{5}` |
| `arb()` | | ★ | | | | ★ | Match any single char (inside alternation) | `.` |
| `rem()` | ★ | | | | | | Remainder / end-of-string extraction | `\z` or `$` |
| **Repetition** | | | | | | | | |
| `arbno(any('0-9'))` | ★ | ★ | ★ | | | ★ | Repeat sub-pattern zero-or-more | `[0-9]*` |
| `repeat(any('0-9'),2,4)` | ★ | | | | | ★ | Bounded repetition (e.g. 2–4 digits) | `[0-9]{2,4}` |
| **Positions** | | | | | | | | |
| `pos(5)` / `rpos(3)` | ★ | | | | | ★ | Anchored position assertions | `(?<=...)/^` / `$` |
| `tab(5)` / `rtab(3)` | ★ | | | | | ★ | Cursor movement (skip N chars) | `(?<=.{5})` / `.{5}` (tricky) |
| **Flow control** | | | | | | | | |
| `fence()` | ★ | | | | | | Commit / prevent backtracking | `(?>...)` (atomic) |
| `fail()` / `abort()` | ★ | | | | | | Deliberate failure (in alternation) | `(*FAIL)` |
| `succeed()` | ★ | | | | | | Always-succeed placeholder | empty alternative |
| **Built-in composites** | | | | | | | | |
| `digit()` | ★ | ★ | ★ | | | ★ | Validate/find/extract digits | `[0-9]` |
| `integer()` | ★ | ★ | ★ | | | ★ | Extract or validate integers | `[0-9]+` |
| `unsignedInt()` | ★ | ★ | ★ | | | ★ | Unsigned integer token | `[0-9]+` |
| `signedInt()` | ★ | ★ | ★ | | | ★ | Optional-sign integer | `[-+]?[0-9]+` |
| `float()` | ★ | ★ | ★ | | | ★ | Floating-point numbers | `[-+]?[0-9]+(\.[0-9]+)?` |
| `hexDigit()` | ★ | ★ | ★ | | | ★ | Hex digit token | `[0-9A-Fa-f]` |
| `letter()` | ★ | ★ | ★ | | | ★ | ASCII letter token | `[A-Za-z]` |
| `word()` | ★ | ★ | ★ | | | ★ | Alphanumeric word token | `[A-Za-z0-9]+` |
| `alnum()` | ★ | ★ | ★ | | | ★ | Alphanumeric single char | `[A-Za-z0-9]` |
| `whitespace()` | ★ | ★ | ★ | | | ★ | Whitespace char | `[ \t\n\r\f\v]` |
| `notDigit()` / `notLetter()` / `notWhitespace()` | ★ | ★ | ★ | | | ★ | Negated character classes | `[^0-9]` / `[^A-Za-z]` / `\S` |
| **Structural** | | | | | | | | |
| `doubleQuotedString()` | ★ | ★ | ★ | | | ★ | Extract `"…"` tokens | `"[^"]*"` |
| `singleQuotedString()` | ★ | ★ | ★ | | | ★ | Extract `'…'` tokens | `'[^']*'` |
| `quotedString()` | ★ | ★ | ★ | | | ★ | Either quote style | `"[^"]*"|'[^']*'` |
| `parenthesised()` | ★ | ★ | ★ | | | ★ | Parenthesised (no nesting) | `\([^()]*\)` |
| `balanced('(',')')` | ★ | ★ | ★ | | | ★ | Nested delimiters | (recursive — PCRE `(?R)`) |
| `commaList()` | | | ★ | | | | Split by comma+space | `[^,]+` (in split context) |
| `stripTags()` | | | ★ | ★ | | | Remove HTML tags | `/<[^>]*>/` |
| `urlSimple()` | ★ | ★ | | | | ★ | Validate/find HTTP(S) URLs | `https?://[^\s/$.?#][^\s]*` |
| `emailSimple()` | ★ | ★ | | | | ★ | Validate/find email-like strings | `\S+@\S+\.\S+` |
| `uuid()` | ★ | ★ | ★ | | | ★ | Validate/find/extract UUIDs | `[0-9a-f]{8}-(?:…){4}-(?:…){12}` |
| `ipv4Simple()` | ★ | ★ | | | | ★ | Validate/find IPv4-like tokens | `\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}` |
| `countryCodeAlpha2()` | ★ | | | | | ★ | Validate two-letter country code | `[A-Z]{2}` |
| `pathSegment()` | ★ | ★ | ★ | | | ★ | POSIX path segment | `[^/]+` |
| `absolutePath()` | ★ | | | | | ★ | POSIX absolute path | `/([^/]+/?)*` |
| `line()` | ★ | ★ | ★ | | | ★ | Single-line token | `[^\n]*` |
| **Advanced** | | | | | | | | |
| `empty()` | ★ | | | | | ★ | Zero-length match (lookahead) | empty string |
| `anyChar()` | ★ | ★ | ★ | | | ★ | Match any single byte | `.` |
| `capture($n, ‘abc’)` | ★ | | | ★ | ★ | ★ | Named captures for substitution | `(?P<vN>…)` |
| `alt(lit('a'), lit('b'))` | ★ | ★ | ★ | | | ★ | Alternation | `a|b` |

**Legend**

| Column | Meaning |
|---|---|
| `match()` | Anchored full-string match (`Pattern::match`). Use for **validation** (is this input a UUID, email, integer, …?). Returns captures on success. |
| `searchAll()` | Find all non-overlapping matches (`Pattern::searchAll`). Use for **extraction** (find all numbers, all quoted strings, all tags, …). |
| `searchSplit()` | Split subject at pattern boundaries (`Pattern::searchSplit`). Use for **tokenization** (split on commas, whitespace, tags, …). |
| `searchReplace()` | Replace each match with a literal string (`Pattern::searchReplace`). Use for **stripping** (strip tags, remove punctuation, …). |
| `subst()` | Template substitution with capture references (`Pattern::subst`). Use for **reformatting** (reorder captured groups, interpolate captures into a template). |
| `PatternHelper` | `PatternHelper::matchOnce()`, `::matchAll()`, `::split()`, `::replace()`. Compiles from string/AST on the fly with an internal 128-slot ring cache. Use for **adhoc/interactive** usage where you don't hold a `Pattern` object. |

★ = primary / most idiomatic use. Blank entries are technically possible but
rarely useful (e.g. `pos()` with `searchAll()` — matches at every cursor
position).

**Which function should I use?**

| Task | Recommended |
|---|---|
| **Validate** that a whole string matches a pattern | `$pattern->match($subject)` |
| **Find all** occurrences in a string | `$pattern->searchAll($subject)` |
| **Tokenize** / split into segments | `$pattern->searchSplit($subject)` or `PatternHelper::split()` |
| **Strip** / delete matches | `$pattern->searchReplace($subject, '')` |
| **Replace** matches with fixed text | `$pattern->searchReplace($subject, $replacement)` |
| **Reformat** with capture-group references (`$v0`, `${v1}`) | `$pattern->subst($subject, $template)` |
| **One-off** match (no stored Pattern) | `PatternHelper::matchOnce('digit', $input)` |
| **Performance-sensitive** tokenization (avoid string alloc) | `$pattern->searchSplitOffsets($subject)` |
| **Pure literal** match (zero alloc, no VM) | `$pattern->matchLiteral($subject)` |

### 16.3 Usage Examples

All examples use the `Patterns` helper class from §16.1.

#### Validation (`match`)

```php
// UUID validation — anchored full-string match
$res = Patterns::uuid()->match('550e8400-e29b-41d4-a716-446655440000');
// $res['_match_len'] === 36

// Email validation
$res = Patterns::emailSimple()->match('user@example.org');       // true
$res = Patterns::emailSimple()->match('not an email');           // false

// Signed integer validation
$res = Patterns::signedInt()->match('-42');                      // true
$res = Patterns::signedInt()->match('+42');                      // true
$res = Patterns::signedInt()->match('12.5');                     // false

// Pattern::matchLiteral() — faster for pure-literal patterns (no VM)
$name = Pattern::compileFromAst(Builder::lit('admin'));
$res  = $name->matchLiteral('admin');  // ['success' => true, 'position' => 0, 'length' => 5]
```

#### Extraction (`searchAll`)

```php
// Extract all integers from a string
$ints = Patterns::integer()->searchAll('abc 123 def 4567 x');
// Returns match-result arrays for "123" and "4567"

// Find all quoted strings
$quoted = Patterns::quotedString()->searchAll('say "hello" then "goodbye"');
// Matches: '"hello"', '"goodbye"'

// Find all IPv4-like tokens
$ips = Patterns::ipv4Simple()->searchAll('Server: 192.168.1.1, GW: 10.0.0.1');
// Matches: '192.168.1.1', '10.0.0.1'

// Find all hex words
$hex = Patterns::hexDigit()->searchAll('color: #ff8800, bg: #eee');
// Matches: 'f', 'f', '8', '8', '0', '0', ... (individual chars — use arbno(hexDigit()) for words)
```

#### Tokenization (`searchSplit`)

```php
// Split on whitespace
$parts = Patterns::whitespace()->searchSplit("one two\tthree\nfour");
// ['one', 'two', 'three', 'four']

// Split CSV-like input
$parts = Patterns::commaList()->searchSplit('apple, banana, cherry');
// ['apple', 'banana', 'cherry']

// Split on digits (keep text segments)
$parts = Patterns::integer()->searchSplit('abc123def456ghi');
// ['abc', 'def', 'ghi']

// Zero-allocation tokenization (avoids intermediate strings)
$offsets = Patterns::integer()->searchSplitOffsets('abc123def456ghi');
// [[3, 3], [9, 3]] — [[offset, length], ...]
```

#### Stripping & Replacing (`searchReplace`)

```php
// Strip HTML tags
$clean = Patterns::stripTags()->searchReplace('<p>Hello <b>world</b></p>', '');
// 'Hello world'

// Remove all digits
$clean = Patterns::digit()->searchReplace('abc123def456', '');
// 'abcdef'

// Normalize whitespace — replace runs of whitespace with single space
$clean = Patterns::whitespace()->searchReplace("too  much\t space", ' ');
// 'too much space'

// Mask credit-card digits
$card = Pattern::compileFromAst(Builder::repeat(Patterns::digit(), 4, 4));
$masked = $card->searchReplace('Card: 1234 5678 9012 3456', '****');
// 'Card: **** **** **** ****'
```

#### Template Substitution (`subst`)

```php
// Extract and reformat a number
$pattern = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Patterns::integer()),
        Builder::assign(0, 0),
    ])
);
$pattern->subst('order: 42 items', '${v0}');        // '42'
$pattern->subst('order: 42 items', '${v0.upper()}'); // '42' (no-op; digits unchanged)
$pattern->subst('order: 42 items', '${v0.lpad(6,"0")}'); // '000042'

// Reformat a name capture — "last, first" → "first last"
$namePat = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Patterns::word()),     // last name
        Builder::lit(', '),
        Builder::cap(1, Patterns::word()),     // first name
        Builder::assign(0, 0),
        Builder::assign(1, 1),
    ])
);
$namePat->subst('Doe, John', '${v1} ${v0}');  // 'John Doe'
```

#### Pattern Combination & Chaining

Patterns compose via `Builder` calls — there is no implicit string concatenation
as in PCRE. Every combination is explicit:

```php
// --- Chain 1: concat of multiple helpers ---
$greeting = Pattern::compileFromAst(
    Builder::concat([
        Patterns::word(),                          // first word
        Patterns::whitespace(),                    // separator
        Patterns::word(),                          // second word
    ])
);
$greeting->match('hello world');  // true (match_len=11)

// --- Chain 2: cap + assign + subst for bidirectional transform ---
$amount = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Patterns::digit()),         // capture leading digit
        Builder::lit('.'),
        Builder::cap(1, Patterns::integer()),       // capture fractional part
        Builder::assign(0, 0),
        Builder::assign(1, 1),
    ])
);
$amount->subst('9.99', '${v0} dollars and ${v1} cents');
// '9 dollars and 99 cents'

// --- Chain 3: arbno(any(...)) for repetition ---
$digits = Pattern::compileFromAst(
    Builder::arbno(Patterns::digit())
);
$digits->match('12345abc');   // match_len=5  (greedy, stops at 'a')

// --- Chain 4: alt(..., ...) for alternatives ---
$bool = Pattern::compileFromAst(
    Builder::alt(
        Builder::lit('true'),
        Builder::lit('false')
    )
);
$bool->match('true');   // true
$bool->match('false');  // true
$bool->match('tru');    // false

// --- Chain 5: multi-capture extraction ---
$parts = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, Patterns::word()),           // capture 0: first
        Patterns::whitespace(),
        Builder::cap(1, Patterns::word()),           // capture 1: last
        Builder::assign(0, 0),
        Builder::assign(1, 1),
    ])
);
$res = $parts->match('John Doe');
echo $res['v0'];  // 'John'
echo $res['v1'];  // 'Doe'

// --- Chain 6: alt inside concat — optional port in URL ---
$hostPort = Pattern::compileFromAst(
    Builder::concat([
        Patterns::word(),
        Builder::alt(
            Builder::concat([Builder::lit(':'), Patterns::integer()]),
            Builder::lit('')           // empty = no port
        ),
    ])
);
$hostPort->match('example.com:8080');  // true includes port
$hostPort->match('example.com');       // true without port

// --- Chain 7: Patterns helper calling another Patterns helper ---
// signedInt() already chains any('+-') + digit() via alt:
//   Patterns::signedInt() ===
//     alt(concat(any('+-'), digit()), digit())
Patterns::signedInt()->match('+42');  // true

// --- Chain 8: repeat with shorthand counting ---
$threeDigits = Pattern::compileFromAst(
    Builder::repeat(Patterns::digit(), 3, 3)
);
$threeDigits->match('123');   // true
$threeDigits->match('12');    // false

$twoOrMore = Pattern::compileFromAst(
    Builder::repeat(Patterns::digit(), 2)
);
$twoOrMore->match('abc123');  // match_len=3 (at cursor)
```

#### PatternHelper Convenience (One-Off Usage)

`PatternHelper` accepts strings, Builder AST arrays, or Pattern objects — useful
when you don't need a named helper:

```php
use Snobol\PatternHelper;

// Match from source string
$res = PatternHelper::matchOnce("SPAN('0-9')", 'abc123');
// Uses internal 128-slot ring cache — recompilation avoided

// Split with inline AST
$parts = PatternHelper::split(
    Builder::span(','),   // or PatternHelper::fromAst(Builder::span(','))
    'a,b,c'
);
// ['a', 'b', 'c']

// Replace with template detection (auto-chooses searchReplace vs subst)
$result = PatternHelper::replace(
    Builder::cap(0, Patterns::integer()),
    '[\$v0]',
    'id:123 code:456'
);
// 'id:[123] code:[456]'

// matchAll convenience
$nums = PatternHelper::matchAll(
    Builder::arbno(Builder::any('0-9')),
    'ab42cd99ef'
);
```

#### Cross-Function Combo

Patterns defined once can be used across all API entry points:

```php
$intPat = Patterns::integer();

// 1) Validate
$intPat->match('42');                 // true

// 2) Extract all
$intPat->searchAll('a1b22c333');      // ['1', '22', '333']

// 3) Split
$intPat->searchSplit('x1y22z333');    // ['x', 'y', 'z']

// 4) Replace
$intPat->searchReplace('x1y22z333', 'N'); // 'xNyNzN'

// 5) Template subst
$pat = Pattern::compileFromAst(
    Builder::concat([
        Builder::cap(0, $intPat),
        Builder::assign(0, 0),
    ])
);
$pat->subst('42', 'value=${v0}');    // 'value=42'
```

### 16.4 Caching

Define patterns as class constants or use `PatternCache` to avoid repeated
compilation when the same helper is called many times:

```php
class Patterns
{
    private static ?Pattern $digit = null;

    public static function digit(): Pattern
    {
        if (self::$digit === null) {
            self::$digit = Builder::any('0-9');
        }
        return self::$digit;
    }
}
```

Or rely on `PatternCache` (see §13):

```php
PatternCache::remember('patterns.digit', fn() => Builder::any('0-9'));
```

---

## 17. Performance Characteristics

### Optimization Strategies

| Strategy                   | When to Use              | Pattern Requirements         |
|----------------------------|--------------------------|------------------------------|
| `matchLiteral()`           | Pure literal patterns    | No `SPAN`, `BREAK`, etc.     |
| `searchSplit()`            | Tokenization (segments)  | Any pattern (auto-tiered)    |
| `searchSplitOffsets()`     | Tokenization (positions) | Any pattern (auto-tiered)    |
| Builder API                | Compile once, match many | Avoids lexer/parse overhead  |
| `PatternCache`             | Repeated patterns        | Same AST/source recompiled   |
| `match()` vs `matchOnce()` | Direct call vs helper    | `match()` is slightly faster |

### Tier Selection Guidance

1. **Prefilter with `matchLiteral()`** — for simple string matching, this is the fastest path by far. Check `$res['success']` first; fall through to full match if needed.

2. **Use `SPAN` and `BREAK`** — these trigger Tiers 0–1 (direct bitmap scan). They're **10–50x faster** than PCRE for equivalent operations.

3. **Avoid unnecessary alternation** — `alt(lit("foo"), lit("bar"), lit("baz"))` triggers Tier 5 (trie) which is fast, but deep nested alternation triggers Tier 8 (VM fallback).

4. **Consider `FENCE`** for pruning — if backtracking is wasted on alternatives that can't succeed, `FENCE` cuts the choice stack and avoids exponential slowdowns.

5. **Use `BREAKX` for tokenization** — `BREAKX` creates backtracking choice points that are O(n) for tokenization, unlike `ARB` which can be O(n²).

### Benchmark Reference (C core, ns/iter)

| Scenario              | Snobol  | PCRE2    | Ratio |
|-----------------------|---------|----------|-------|
| Literal fail (short)  | ~550 ns | ~650 ns  | 0.85x |
| Literal match (short) | ~358 ns | ~700 ns  | 0.51x |
| SPAN comma            | ~336 ns | ~900 ns  | 0.37x |
| SPAN search           | ~445 ns | ~1010 ns | 0.44x |
| Alternation           | ~339 ns | ~810 ns  | 0.42x |
| Tokenization          | ~397 ns | ~870 ns  | 0.46x |

PHP overhead adds ~50–300 ns per call depending on the tier. The PHP/C coupling ratio is typically < 100x for most operations.

---

## 18. Appendix: SNOBOL4 String Syntax

The `Pattern::fromString()` method accepts SNOBOL4 pattern syntax:

| Syntax           | Builder Equivalent       | Description                    |
|------------------|--------------------------|--------------------------------|
| `'hello'`        | `Builder::lit("hello")`  | Literal string (single quotes) |
| `SPAN('abc')`    | `Builder::span("abc")`   | Span characters                |
| `BREAK('x')`     | `Builder::brk("x")`      | Break at character             |
| `ANY('abc')`     | `Builder::any("abc")`    | Any character in set           |
| `NOTANY('abc')`  | `Builder::notany("abc")` | Not any character in set       |
| `LEN(5)`         | `len(5)` via AST         | Match N characters             |
| `POS(0)`         | `Builder::pos(0)`        | Position assertion             |
| `RPOS(0)`        | `Builder::rpos(0)`       | Reverse position assertion     |
| `TAB(5)`         | `Builder::tab(5)`        | Tab to position                |
| `RTAB(5)`        | `Builder::rtab(5)`       | Reverse tab                    |
| `REM`            | `Builder::rem()`         | Remainder                      |
| `FENCE`          | `Builder::fence()`       | Cut                            |
| `ARB`            | `Builder::arb()`         | Arbitrary (0+ any)             |
| `ARBNO(pattern)` | `Builder::arbno(sub)`    | Zero or more of pattern        |
| `BAL('(', ')')`  | `Builder::bal('(', ')')` | Balanced                       |
| `A \| B`         | `Builder::alt(a, b)`     | Alternation                    |
| `$v0` / `${v0}`  | `${v0}` in templates     | Variable reference             |
| `@r0(pattern)`   | `Builder::cap(0, sub)`   | Capture into register 0        |
| `. v0`           | `Builder::assign(0, 0)`  | Assignment                     |

### Notes on SNOBOL4 Syntax Support

The string parser covers the most common patterns but may not support all SNOBOL4 features. For full access, use the Builder API, which covers all 29 AST node types and 42 opcodes.
