/**
 * corpus.h – Embedded pattern corpus for the differential search oracle.
 *
 * Every pattern in this table is run through the equivalence harness in
 * test_search_oracle.c: the accelerated tier dispatch must produce exactly
 * the same outcome, position, length, and captures as a reference
 * vm_exec run on the same bytecode.
 *
 * The corpus doubles as documentation of supported shapes: common regex
 * use cases translated to SNOBOL, plus the uncommon shapes that previously
 * produced wrong answers (leading alternations, alternations above/below
 * the 2048-byte alt-literals walk bound, prefix-of-another literals,
 * empty literals, loops, BREAK/BREAKX).
 *
 * Grammar notes (snobol.ebnf): pattern primitives are literals, SPAN,
 * ANY, NOTANY, BREAK, BREAKX, LEN, EVAL; anchors are `^`/`$`; captures
 * are `@v1` prefixes; repetition uses `+`/`*`/`?` suffixes.
 */

#ifndef SNOBOL4_TEST_CORPUS_H
#define SNOBOL4_TEST_CORPUS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
  const char *name;    /* human-readable shape description */
  const char *pattern; /* SNOBOL pattern source */
  uint32_t flags;      /* compile flags (SNOBOL_FLAG_*) */
} oracle_corpus_entry_t;

/* ---------------------------------------------------------------------------
 * Static corpus: common regex use cases + uncommon shapes.
 * ---------------------------------------------------------------------------
 */
static const oracle_corpus_entry_t oracle_corpus[] = {
    /* ---- tokenization ---- */
    {"break-space", "BREAK(' ')", 0},
    {"span-space", "SPAN(' ')", 0},
    {"breakx-space", "BREAKX(' ')", 0},
    {"break-semicolon", "BREAK(';')", 0},
    {"breakx-pipe", "BREAKX('|')", 0},
    {"csv-field", "BREAK(',')", 0},
    /* ---- extraction ---- */
    {"extract-id", "'ID:' @v1 (ANY('a-z') | ANY('0-9'))*", 0},
    {"extract-between", "'name=' (ANY('a-z'))* ';'", 0},
    {"extract-email-ish", "(ANY('a-z'))* '@' (ANY('a-z'))*", 0},
    {"key-value", "SPAN('a-z') '=' (ANY('a-z'))*", 0},
    /* ---- validation / structure ---- */
    {"phone-number", "'(' SPAN('0-9') ')' SPAN('0-9') '-' SPAN('0-9')", 0},
    {"date", "SPAN('0-9') '/' SPAN('0-9') '/' SPAN('0-9')", 0},
    {"anchored-len5", "^ (ANY('a-z') | ANY('0-9'))* $", 0},
    {"digits-to-end", "SPAN('0-9') $", 0},
    /* ---- alternation ---- */
    {"single-char-alt", "'a' | 'b' | 'c'", 0},
    {"word-alt", "'cat' | 'dog' | 'bird'", 0},
    {"alt-with-capture", "@v1 ('cat' | 'dog')", 0},
    {"alt-eight-single-char", "'a'|'b'|'c'|'d'|'e'|'f'|'g'|'h'", 0},
    {"prefix-of-another", "'ab' | 'abc'", 0},
    {"lit-vs-prefix", "'abcd' | 'ab'", 0},
    {"mixed-lit-lengths", "'a' | 'bb' | 'ccc'", 0},
    {"empty-vs-lit", "'' | 'a'", 0},
    {"alt-with-empty-branch", "'a' | ''", 0},
    {"concat-alt-concat", "'x' ('a'|'b') 'y'", 0},
    /* ---- literals ---- */
    {"literal-needle", "'needle'", 0},
    {"literal-one-char", "'x'", 0},
    {"literal-digits", "'v1.0.2'", 0},
    {"literal-repeated", "'aaaa'", 0},
    {"empty-literal", "''", 0},
    {"empty-concat", "'' 'x'", 0},
    /* ---- quantifiers / loops ---- */
    {"loop-plus", "('ab')+", 0},
    {"loop-star", "('ab')*", 0},
    {"loop-body-alt", "('a' | 'b')*", 0},
    {"loop-nested", "('a' ('b' | 'c') 'd')+", 0},
    {"loop-greedy-tail", "('a')* 'b'", 0},
    {"loop-plus-tail", "('a'+)+ 'b'", 0},
    {"loop-double-star", "('a'*)* 'b'", 0},
    {"loop-alt-plus", "('a'|'b')+ '!'", 0},
    {"loop-empty-body", "('')*", 0},
    {"empty-loop-concat", "('')+ 'x'", 0},
    /* ---- anchors ---- */
    {"anchor-start", "^ 'abc'", 0},
    {"anchor-end", "'abc' $", 0},
    {"anchor-both", "^ 'a' 'b' $", 0},
    /* ---- character classes ---- */
    {"span-alpha", "SPAN('a-z')", 0},
    {"span-numeric", "SPAN('0-9')", 0},
    {"any-char", "ANY('abc')", 0},
    {"notany", "NOTANY('abc')", 0},
    {"concat-mixed", "'a' SPAN('0-9') 'b'", 0},
    {"any-three", "ANY('a-z') ANY('a-z') ANY('a-z')", 0},
    {"any-run", "(ANY('a-z'))*", 0},
    {"any-run-delim", "(ANY('a-z'))* '!'", 0},
    /* ---- captures ---- */
    {"cap-simple", "@v1 'abc'", 0},
    {"cap-span", "@v1 SPAN('0-9')", 0},
    {"cap-two", "@v1 @v2 'ab'", 0},
    {"cap-alt", "@v1 ('a' | 'b') 'c'", 0},
    /* ---- Unicode / case ---- */
    {"unicode-greek", "'κόσμε'", 0},
    {"unicode-japanese", "'日本語'", 0},
    {"unicode-bmp-latin1", "'café'", 0},
    {"unicode-umlaut", "'ü'", 0},
    {"case-insensitive-hello", "'hello'", SNOBOL_FLAG_CASE_INSENSITIVE},
    {"case-insensitive-utf8", "'ΚΌΣΜΕ'", SNOBOL_FLAG_CASE_INSENSITIVE},
    {"case-fold-digits", "'ID123'", SNOBOL_FLAG_CASE_INSENSITIVE},
};

static const size_t oracle_corpus_count =
    sizeof(oracle_corpus) / sizeof(oracle_corpus[0]);

/* ---------------------------------------------------------------------------
 * Subjects: markers, ASCII, UTF-8, empty, long (each <= 256 bytes).
 * ---------------------------------------------------------------------------
 */
static const char *const oracle_subjects[] = {
    "",
    "a",
    "abc",
    "ab",
    "x",
    "x x x",
    "aaaa",
    "abababab",
    "aab",
    "y",
    "abbccc",
    "catdogbird",
    "needle in a haystack",
    "the quick brown fox jumps over the lazy dog",
    "abc,def;ghi jkl",
    "12345",
    "v1.0.2 v1.0.3 core/v1.0.2",
    "ID: abcdefgh",
    "name=foo;bar",
    "κόσμε",
    "日本語テキスト",
    "café au lait",
    "Hello WORLD hello",
    "hello HELLO HeLLo",
    "abcdefghijklmnopqrstuvwxyz0123456789",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    "a",
    "aaaaaaaaaaaaaaaaaaaaaaaaaa",
    "mixed 123 abc !!!",
    "(555) 123-4567",
    "01/02/2026",
    "κόσμε κόσμε κόσμε κόσμε κόσμε κόσμε κόσμε κόσμε κόσμε κόσμε",
    "ab cd abcd ab",
    "x y z",
};

static const size_t oracle_subject_count =
    sizeof(oracle_subjects) / sizeof(oracle_subjects[0]);

/* ---------------------------------------------------------------------------
 * Dynamically built shapes: large alternations.
 *
 * The alt-literals walk in derive_meta is bounded to the first 2048 bytes
 * of bytecode.  An N-branch chain costs roughly 25 bytes per branch
 * (SPLIT 9 + LIT 9+len + ACCEPT 1), so 82 markers of ~6 bytes exceed the
 * bound, while 30 branches of 20 bytes stay below it.
 * ---------------------------------------------------------------------------
 */
#define ORACLE_ALT_BUF_CAP 16384

/* Build "v1.0.0 | v1.0.1 | ... | v1.8.1": 82 changelog-style version
 * markers — the alternation shape that originally broke the prefilter.
 * Returns the pattern length. */
static size_t oracle_build_marker_alt(char *buf, size_t cap) {
  size_t off = 0;
  int n = 0;
  for (int maj = 1; maj <= 1 && n < 82; maj++) {
    for (int min = 0; min <= 8 && n < 82; min++) {
      for (int pat = 0; pat <= 9 && n < 82; pat++) {
        if (min == 8 && pat > 1) {
          break;
        }
        int w = snprintf(buf + off, cap - off, "%s'v%d.%d.%d'",
                         off ? " | " : "", maj, min, pat);
        if (w < 0 || (size_t)w >= cap - off) {
          break;
        }
        off += (size_t)w;
        n++;
      }
    }
  }
  buf[off] = '\0';
  return off;
}

/* Build a flat alternation of @p branches literals of @p lit_len bytes
 * (branch i uses byte ('a' + i % 26) repeated lit_len times).  Returns the
 * pattern length. */
static size_t oracle_build_big_alt(char *buf, size_t cap, int branches,
                                   size_t lit_len) {
  size_t off = 0;
  for (int i = 0; i < branches; i++) {
    size_t need = (off ? 3 : 1) + lit_len + 1; /* " | 'lit'" or "'lit'" */
    if (off + need >= cap) {
      break;
    }
    if (off) {
      buf[off++] = ' ';
      buf[off++] = '|';
      buf[off++] = ' ';
    }
    buf[off++] = '\'';
    char c = (char)('a' + (i % 26));
    for (size_t j = 0; j < lit_len; j++) {
      buf[off++] = c;
    }
    buf[off++] = '\'';
  }
  buf[off] = '\0';
  return off;
}

#endif /* SNOBOL4_TEST_CORPUS_H */
