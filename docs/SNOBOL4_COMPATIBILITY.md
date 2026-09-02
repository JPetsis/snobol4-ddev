# SNOBOL4 Compatibility Ledger

This ledger is the source of truth for how libsnobol4 relates to classic
SNOBOL4 (Griswold et al., *The SNOBOL4 Programming Language*; cross-checked
against the CSNOBOL4 2.3.1 reference implementation). Every feature area is
classified as one of:

| Class | Meaning |
|---|---|
| **Faithful** | Implemented with classic semantics |
| **Deliberate divergence** | Implemented differently on purpose; rationale given |
| **Extension** | Not part of classic SNOBOL4; libsnobol4 addition |
| **Known gap** | Absent; nearest available mechanism named |

Deliberate divergences are also surfaced in the manuals (`docs/c-manual.md`,
`docs/php-manual.md`) wherever the affected feature is documented.

## 1. Pattern primitives

| Feature | Class | Notes |
|---|---|---|
| `'literal'` | Faithful | Single-quoted literal |
| `SPAN(set)`, `ANY(set)`, `NOTANY(set)` | Faithful | |
| `LEN(n)`, `POS(n)`, `TAB(n)`, `RPOS(n)`, `RTAB(n)`, `ARB`, `ARBNO(p)`, `BAL('('[,')'])`, `FENCE`, `REM`, `ABORT`, `FAIL`, `SUCCEED` | Faithful | Real integer arguments (int32, signed); `LEN(n)` matches exactly n characters; `ARB` is `ARBNO(LEN(1))` |
| `BREAK(set)` | **Deliberate divergence** | Deterministic and greedy: consumes up to the first set character and never gives characters back. Classic SNOBOL4 retries shorter prefixes on later failure. Retry semantics are available via `BREAKX(set)` (faithful). |
| `RTAB(n)` | **Deliberate divergence** | Clamps to the subject start when the tab distance overshoots (a zero-length advance) instead of failing the match as classic SNOBOL4 does. |
| `BREAKX(set)` | Faithful | Break with retry (the classic semantics of `BREAK`); uses the O(n) pre-scan optimisation |
| `EVAL(expr)` | Faithful | Dynamic pattern evaluation (runtime compile + cache) |
| `repeat(p, min, max)` | Extension | Bounded repetition as a source function; classic SNOBOL4 has no such primitive |
| `first-codepoint` delimiter decoding (`BAL('〈', '〉')`) | Deliberate divergence | Delimiters are decoded as the first UTF-8 codepoint, not one byte |

## 2. Anchoring and scanning mode

| Feature | Class | Notes |
|---|---|---|
| `match()` / `snobol_pattern_match` | **Deliberate divergence** | Anchored at offset 0 by default; classic SNOBOL4 scans unanchored (`&ANCHOR = 0` default). |
| `search*` API (`snobol_pattern_search`, `Pattern::searchAll`, …) | Faithful | Unanchored scanning mode. |
| `^` / `$` | Extension | Start/end anchors; classic SNOBOL4 has no anchor operators (`$` is the indirect-reference operator there). |

## 3. Alternation and backtracking model

| Feature | Class | Notes |
|---|---|---|
| Ordered alternation, leftmost match, backtracking | Faithful | Full SNOBOL4 semantics (choice stack, on-the-fly alternatives) |
| `FENCE` | Faithful | Cuts the choice stack (no backtracking past the fence) |
| `BREAKX` retry | Faithful | |
| Zero-progress loop guard, bounded repetition | Extension | Engine hardening (no infinite loops on empty bodies) |

## 4. Captures and naming

| Feature | Class | Notes |
|---|---|---|
| `@name` prefix capture | Deliberate divergence | Sequential 0-based register allocation per occurrence (`v0`, `v1`, …); classic SNOBOL4 assigns the captured value to the named *variable* and rebinds it at runtime |
| Capture registers `v0..v63` | Extension | Explicit register model shared with `Builder::cap`; match results expose `v<reg>` keys |
| `P . @name` / `P $ vN` / `P . vN` / `P . $vN` match-naming | Extension | Classic SNOBOL4 has no match-naming operators; the nearest classic mechanism is an `@`-capture followed by an assignment |
| Unary `$` indirect reference (`$X`) | Known gap | Rejected with "unary '$' indirect reference is not supported"; the nearest mechanisms are match-naming (`P $ vN`), table capture keys (`T[$v0]`) and template references (`$STATE[...]`) |
| Register assignment `vN = <reg>` / `name = <reg>` | Extension | Binds a capture register / registered capture name into another register |

## 5. Replacement and substitution

| Feature | Class | Notes |
|---|---|---|
| Template substitution (`$NAME[...]`, format operations) | Faithful | `snobol_template_*` + `PatternHelper::tableSubst()` / `formattedSubst()`; tables bound by name via `snobol_template_bind_tables` |
| Pattern `EMIT` (`EMIT('text')`, `EMIT(@vN)`, `EMIT(@name)`) | Extension | Emits into the match output buffer (classic SNOBOL4 has no EMIT; nearest classic mechanism is `OUTPUT` assignment) |
| Pattern-level table ops (`T['k']`, `T['k'] = p`, `T[$v0]`) | Known gap | Compile and run (parity with the Builder), but the table NAME is not bound to a runtime `Snobol\Table` — only template-level `$T[...]` values resolve through the runtime binding machinery. Planned: `snobol_pattern_bind_tables` (change `feat-pattern-table-binding`). Nearest working mechanism: template tables |

## 6. Data types

| Feature | Class | Notes |
|---|---|---|
| Strings, patterns, integers (arguments) | Faithful | |
| Runtime tables (`Snobol\Table`) | Faithful | |
| `CODE` data type | Known gap | Nearest mechanism: `DynamicPatternCache` / `EVAL` for compiled-at-runtime patterns |
| `NAME` data type | Known gap | Nearest mechanism: string references and the capture-name registry |

## 7. Statement-level programming

| Feature | Class | Notes |
|---|---|---|
| Labels (`NAME: pattern`) in pattern source | Partial | Supported inside a pattern expression |
| Goto transfers `:(LABEL)` | Partial | Supported inside a pattern expression |
| Full statement programs (variables, assignment statements, control flow between statements) | Known gap | Nearest mechanism: pattern-level labels/goto + host-language control flow |

## 8. I/O

| Feature | Class | Notes |
|---|---|---|
| `INPUT` / `OUTPUT` | Known gap | Nearest mechanism: host-language I/O; `EMIT` accumulates pattern output into the match result's `output` field |

## 9. User-defined functions

| Feature | Class | Notes |
|---|---|---|
| `DEFINE(...)` | Known gap | Nearest mechanism: host-language callbacks and `EVAL`-based dynamic patterns |
| Arithmetic operators | Known gap | Pattern arguments are integers; no arithmetic expression evaluation |

## Summary of deliberate divergences

1. `match()` is anchored at offset 0 (classic `&ANCHOR = 0` scans unanchored); unanchored behavior is available via the `search*` API family.
2. `BREAK` is deterministic-greedy; use `BREAKX` for classic retry semantics.
3. `RTAB(n)` clamps on overshoot instead of failing.
4. `BAL` delimiters decode the first codepoint (classic treats them as single characters).
5. `@name` captures allocate sequential registers instead of rebinding classic variables.