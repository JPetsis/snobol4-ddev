/**
 * @file api.c
 * @brief High-level public C API for libsnobol4
 *
 * Implements snobol_context_t, snobol_pattern_t, and snobol_match_t along
 * with their lifecycle and query functions declared in snobol/snobol.h.
 *
 * The pipeline for pattern compilation is:
 *   source text → snobol_lexer → snobol_parser (AST) →
 * compile_ast_to_bytecode_c
 *
 * Pattern matching is done via vm_run().
 */

#include "snobol/ast.h"
#include "snobol/compiler.h"
#include "snobol/lexer.h"
#include "snobol/parser.h"
#include "snobol/search.h"
#include "snobol/snobol.h"
#include "snobol/snobol_internal.h"
#include "snobol/vm.h"
#include "search_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Opaque type definitions
 * ---------------------------------------------------------------------------
 */

struct snobol_context {
  int _reserved; /* Future: pattern registry, allocator config, etc. */
};

struct snobol_pattern {
  uint8_t *bc;
  size_t bc_len;
  bool case_insensitive;
  /* Cached search metadata derived from bytecode at compile time.
   * Reused by snobol_pattern_search() and snobol_pattern_search_ex()
   * to avoid re-walking the bytecode on every call. */
  snobol_search_meta_t meta;
  bool meta_initialized;
  /* Cached charclass range metadata — O(1) set_id lookup
   * for get_ranges_ptr().  Built once at compile time. */
  snobol_range_meta_t *range_meta;
  size_t range_meta_count;
  /* Cached DFA automaton (Tier 7). Constructed lazily on first
   * eligible search; freed in snobol_pattern_free(). */
  snobol_dfa_t *automaton;
  /* Cached Tier-5 alternation trie (snobol_auto_trie_t).  Built lazily on
   * the first Tier-5 search and reused for every subsequent search of this
   * pattern.  NULL until built; freed in snobol_pattern_free().  Only set
   * for bushy alternations (flat ones route to Tier 8 and never build one). */
  snobol_auto_trie_t *trie_cache;
  int trie_cache_refs; /* reserved for diagnostics; 0 when no cache present */
};

/* Maximum named variables returned from a match */
#define API_MAX_VARS SNOBOL_API_MAX_VARS

/* Forward declaration — defined just above the convenience match API. */
static void match_store_capture(snobol_match_t *m, const char *subject, int i,
                                size_t vs, size_t ve, size_t subject_len);

/* ---------------------------------------------------------------------------
 * Context lifecycle
 * ---------------------------------------------------------------------------
 */

snobol_context_t *snobol_context_create(void) {
  snobol_context_t *ctx =
      (snobol_context_t *)snobol_malloc(sizeof(snobol_context_t));
  if (ctx) {
    ctx->_reserved = 0;
  }
  return ctx;
}

void snobol_context_destroy(snobol_context_t *ctx) {
  if (ctx) {
    snobol_free(ctx);
  }
}

/* ---------------------------------------------------------------------------
 * Pattern lifecycle
 * ---------------------------------------------------------------------------
 */

/** Copy a static message into the caller's malloc'd error out-param. */
static void set_error_out(char **error, const char *msg) {
  if (!error) {
    return;
  }
  size_t mlen = strlen(msg) + 1;
  *error = (char *)malloc(mlen);
  if (*error) {
    memcpy(*error, msg, mlen);
  }
}

/**
 * Shared post-bytecode pattern initialization: derive search metadata and
 * range metadata, then allocate and initialize the pattern struct.  Takes
 * ownership of @p bc (freed on failure).  Single source of truth for the
 * tail of pattern compilation so the source-string and builder paths cannot
 * drift.
 */
static snobol_pattern_t *pattern_finalize(uint8_t *bc, size_t bc_len,
                                          bool case_insensitive, char **error) {
  snobol_pattern_t *pat =
      (snobol_pattern_t *)snobol_malloc(sizeof(snobol_pattern_t));
  if (!pat) {
    compiler_free(bc);
    set_error_out(error, "out of memory");
    return nullptr;
  }
  memset(pat, 0, sizeof(snobol_pattern_t));
  pat->bc = bc;
  pat->bc_len = bc_len;
  pat->case_insensitive = case_insensitive;
  /* Derive search metadata once at compile time so snobol_pattern_search()
   * and snobol_pattern_search_ex() don't re-walk the bytecode per call. */
  snobol_search_derive_meta(pat->bc, pat->bc_len, &pat->meta);
  pat->meta_initialized = true;
  snobol_build_range_meta(pat->bc, pat->bc_len, &pat->range_meta,
                          &pat->range_meta_count);
  return pat;
}

/**
 * Core compilation helper: lex → parse → compile → allocate pattern.
 */
static snobol_pattern_t *do_compile(const char *source, size_t len,
                                    bool case_insensitive, char **error) {
  if (error) {
    *error = nullptr;
  }

  snobol_lexer_t *lexer = snobol_lexer_create(source, len);
  snobol_parser_t *parser = snobol_parser_create();

  /* Bind a thread-local bump arena for AST node storage.  Node structs are
   * bump-allocated for the duration of this compile and reclaimed via a
   * single arena reset; owned sub-allocations (strings, concat arrays) still
   * use the heap.  Falls back to calloc when the arena is exhausted. */
  snobol_arena_t arena;
  void *arena_buf = snobol_malloc(SNOBOL_ARENA_DEFAULT_CAPACITY);
  if (arena_buf) {
    snobol_arena_init(&arena, arena_buf, SNOBOL_ARENA_DEFAULT_CAPACITY);
  } else {
    snobol_arena_init(&arena, NULL, 0);
  }
  snobol_ast_set_arena(&arena);

  ast_node_t *ast = snobol_parser_parse(parser, lexer);

  snobol_pattern_t *result = nullptr;

  if (!ast || snobol_parser_has_error(parser)) {
    const char *msg = snobol_parser_get_error(parser);
    if (!msg) {
      msg = "unknown parse error";
    }
    set_error_out(error, msg);
    snobol_ast_free(ast);
    goto cleanup;
  }

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(ast, case_insensitive, &bc, &bc_len);
  snobol_ast_free(ast);

  if (rc != 0) {
    set_error_out(error, "compilation failed");
    goto cleanup;
  }

  result = pattern_finalize(bc, bc_len, case_insensitive, error);

cleanup:
  snobol_parser_destroy(parser);
  snobol_lexer_destroy(lexer);
  /* Reclaim all bump-allocated AST node storage in one shot.  Owned strings
   * and concat part arrays were already freed by snobol_ast_free(). */
  snobol_ast_clear_arena();
  snobol_arena_reset(&arena);
  snobol_free(arena_buf);
  return result;
}

snobol_pattern_t *snobol_pattern_compile_ex(snobol_context_t *ctx,
                                            const char *source, size_t len,
                                            uint32_t flags, char **error) {
  (void)ctx; /* context owns the pattern conceptually; no registry yet */
  bool case_insensitive = (flags & SNOBOL_FLAG_CASE_INSENSITIVE) != 0;
  /* Unknown flag bits are intentionally ignored (forward-compatible). */
  return do_compile(source, len, case_insensitive, error);
}

snobol_pattern_t *snobol_pattern_compile(snobol_context_t *ctx,
                                         const char *source, size_t len,
                                         char **error) {
  return snobol_pattern_compile_ex(ctx, source, len, 0, error);
}

const uint8_t *snobol_pattern_get_bc(const snobol_pattern_t *pattern) {
  return pattern ? pattern->bc : nullptr;
}

size_t snobol_pattern_get_bc_len(const snobol_pattern_t *pattern) {
  return pattern ? pattern->bc_len : 0;
}

/* ---- Internal accessors (used by search.c) ---- */

snobol_dfa_t *snobol_pattern_get_automaton(const snobol_pattern_t *pattern) {
  return pattern ? pattern->automaton : nullptr;
}

void snobol_pattern_set_automaton(snobol_pattern_t *pattern,
                                  snobol_dfa_t *dfa) {
  if (pattern) {
    pattern->automaton = dfa;
  }
}

/* ---------------------------------------------------------------------------
 * Tier-5 alternation-trie cache accessors (mirror the DFA cache pattern).
 * The cache is built lazily on the first Tier-5 search of a bushy alternation
 * and reused for every subsequent search of that pattern; NULL for flat
 * alternations (which route to Tier 8) or patterns without an alternation.
 *
 * Centralising mutation here lets any future bytecode-rewrite site invalidate
 * the cache by calling snobol_pattern_set_trie_cache(pat, NULL).
 * ---------------------------------------------------------------------------
 */

/* Return the cached Tier-5 trie, or NULL when none has been built yet. */
snobol_auto_trie_t *snobol_pattern_get_trie_cache(
    const snobol_pattern_t *pattern) {
  return pattern ? pattern->trie_cache : nullptr;
}

/* Store (or clear, with @p trie == NULL) the cached Tier-5 trie and update the
 * reference count used by snobol_pattern_free() to decide whether to free it. */
void snobol_pattern_set_trie_cache(snobol_pattern_t *pattern,
                                   snobol_auto_trie_t *trie) {
  if (pattern) {
    pattern->trie_cache = trie;
    pattern->trie_cache_refs = trie ? 1 : 0;
  }
}

const snobol_search_meta_t *snobol_pattern_get_meta(
    const snobol_pattern_t *pattern) {
  return pattern ? &pattern->meta : nullptr;
}

const snobol_range_meta_t *snobol_pattern_get_range_meta(
    const snobol_pattern_t *pattern, size_t *count) {
  if (count) {
    *count = pattern ? pattern->range_meta_count : 0;
  }
  return pattern ? pattern->range_meta : nullptr;
}

bool snobol_pattern_automaton_available(const snobol_pattern_t *pattern) {
  return (pattern && pattern->automaton &&
          pattern->automaton->num_states < SNOBOL_DFA_MAX_STATES) != 0;
}

void snobol_pattern_free(snobol_pattern_t *pattern) {
  if (!pattern) {
    return;
  }
  compiler_free(pattern->bc);
  if (pattern->range_meta) {
    snobol_free(pattern->range_meta);
  }
  if (pattern->automaton) {
    snobol_dfa_free(pattern->automaton);
  }
  if (pattern->trie_cache) {
    snobol_auto_trie_free(pattern->trie_cache);
  }
  snobol_search_meta_free(&pattern->meta);
  snobol_free(pattern);
}

/* ---------------------------------------------------------------------------
 * Pattern matching
 * ---------------------------------------------------------------------------
 */

snobol_literal_match_t snobol_pattern_match_literal(snobol_pattern_t *pattern,
                                                    const char *subject,
                                                    size_t len) {
  snobol_literal_match_t result = {false, 0, 0};
  if (!pattern || !subject) {
    return result;
  }

  /* Derive meta on first use if not already initialized */
  if (!pattern->meta_initialized) {
    snobol_search_derive_meta(pattern->bc, pattern->bc_len, &pattern->meta);
    pattern->meta_initialized = true;
  }
  if (!pattern->meta.is_literal_only) {
    return result;
  }

  /* Extract the literal from bytecode: skip leading zero-width ops */
  const uint8_t *bc = pattern->bc;
  size_t bc_len = pattern->bc_len;
  size_t ip = 0;
  while (ip < bc_len) {
    uint8_t op = bc[ip];
    if (op == OP_NOP || op == OP_FENCE || op == OP_ANCHOR) {
      ip++;
      continue;
    }
    if ((op == OP_POS || op == OP_RPOS) && ip + 5 <= bc_len) {
      ip += 5;
      continue;
    }
    break;
  }
  if (ip + 9 > bc_len || bc[ip] != OP_LIT) {
    return result;
  }

  uint32_t lit_off = ((uint32_t)bc[ip + 1] << 24) |
                     ((uint32_t)bc[ip + 2] << 16) |
                     ((uint32_t)bc[ip + 3] << 8) | (uint32_t)bc[ip + 4];
  uint32_t lit_len = ((uint32_t)bc[ip + 5] << 24) |
                     ((uint32_t)bc[ip + 6] << 16) |
                     ((uint32_t)bc[ip + 7] << 8) | (uint32_t)bc[ip + 8];
  if (lit_off > bc_len || lit_off + lit_len > bc_len || lit_len == 0) {
    return result;
  }

  const char *lit = (const char *)(bc + lit_off);
  if (len >= lit_len && memcmp(subject, lit, lit_len) == 0) {
    result.success = true;
    result.position = 0;
    result.length = lit_len;
  }
  return result;
}

snobol_match_t *snobol_pattern_match(snobol_pattern_t *pattern,
                                     const char *subject, size_t len) {
  if (!pattern || !subject) {
    return nullptr;
  }

  snobol_match_t *m = (snobol_match_t *)snobol_malloc(sizeof(snobol_match_t));
  if (!m) {
    return nullptr;
  }
  memset(m, 0, sizeof(snobol_match_t));

  /* Set up output buffer */
  snobol_buf out_buf = {nullptr};
  snobol_buf_init(&out_buf);

  /* ---- Literal-only fast path: bypass VM entirely ---- */
  {
    snobol_literal_match_t lr =
        snobol_pattern_match_literal(pattern, subject, len);
    if (lr.success) {
      m->success = true;
      m->position = lr.position;
      m->length = lr.length;
      m->var_count = 0;
      snobol_buf_free(&out_buf);
      return m;
    }
    if (pattern && pattern->meta_initialized && pattern->meta.is_literal_only) {
      /* Literal-only but non-matching — skip VM entirely */
      m->success = false;
      snobol_buf_free(&out_buf);
      return m;
    }
  }

  /* Use cached search metadata from compile time (mirrors snobol_pattern_search). */
  snobol_search_meta_t meta;
  if (pattern->meta_initialized) {
    meta = pattern->meta;
  } else {
    snobol_search_derive_meta(pattern->bc, pattern->bc_len, &meta);
  }

  /* Initialise VM */
  VM vm;
  memset(&vm, 0, sizeof(VM));
  vm.bc = pattern->bc;
  vm.bc_len = pattern->bc_len;
  vm.pattern = pattern;
  vm.range_meta = pattern->range_meta;
  vm.range_meta_count = pattern->range_meta_count;
  vm.s = subject;
  vm.len = len;
  vm.out = &out_buf;

  /* Build and cache DFA for eligible patterns (lazy: reuse cached) */
  snobol_dfa_t *dfa = nullptr;
  if (meta.automaton_eligible) {
    dfa = snobol_pattern_get_automaton(pattern);
    if (!dfa) {
      dfa = build_dfa(pattern->bc, pattern->bc_len, &vm);
      if (dfa) {
        snobol_pattern_set_automaton(pattern, dfa);
      }
    }
  }

  /* Route through the cost-model tier dispatcher (anchored match). */
  snobol_search_result_t sr;
  bool ok =
      snobol_search_exec_anchored(&vm, subject, len, &meta, dfa, &sr, nullptr);
  m->success = ok;
  if (ok) {
    m->position = sr.match_start;
    m->length = sr.match_end - sr.match_start;
  }

  if (ok && out_buf.len > 0) {
    m->output = (char *)snobol_malloc(out_buf.len + 1);
    if (m->output) {
      memcpy(m->output, out_buf.data, out_buf.len);
      m->output[out_buf.len] = '\0';
      m->output_len = out_buf.len;
    }
  }

  /* Copy named variables (1-based: var_start[0] = variable "1").
   * The VM computes capture offsets relative to the match window (its
   * subject base is subject + match position); anchor var_subject to the
   * window base and bound against the window length so materialization
   * reads the correct absolute span for matches away from offset 0. */
  int n = (int)vm.var_count;
  if (n > API_MAX_VARS) {
    n = API_MAX_VARS;
  }
  m->var_count = n;
  /* Only touch sr.match_start when the search succeeded (it is unset on
   * failure); a failed match carries no captures anyway. */
  if (ok && n > 0) {
    const char *win_subject = subject + sr.match_start;
    size_t win_len = (sr.match_start <= len) ? len - sr.match_start : 0;
    for (int i = 0; i < n; i++) {
      size_t vs = vm.var_start[i];
      size_t ve = vm.var_end[i];
      match_store_capture(m, win_subject, i, vs, ve, win_len);
    }
  }

  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  snobol_search_vm_cleanup(&vm);
  return m;
}

snobol_match_t *snobol_pattern_search(snobol_pattern_t *pattern,
                                      const char *subject, size_t len) {
  if (!pattern || !subject) {
    return nullptr;
  }

  /* Required-byte prefilter: BEFORE any allocation or VM setup, reject
   * subjects that lack a provably required literal.  This avoids malloc,
   * buf_init, memset(VM), and DFA construction entirely. */
  if (pattern->meta_initialized) {
    const snobol_search_meta_t *meta = &pattern->meta;
    if (meta->has_required_lit && meta->required_lit_len > 0) {
      bool found;
      if (meta->required_lit_len == 1) {
        found = memchr(subject, meta->required_lit[0], len) != NULL;
      } else {
        found = memmem(subject, len, meta->required_lit,
                       meta->required_lit_len) != NULL;
      }
      if (!found) {
        snobol_match_t *m =
            (snobol_match_t *)snobol_malloc(sizeof(snobol_match_t));
        if (m) {
          memset(m, 0, sizeof(snobol_match_t));
        }
        return m;
      }
    }
  }

  snobol_match_t *m = (snobol_match_t *)snobol_malloc(sizeof(snobol_match_t));
  if (!m) {
    return nullptr;
  }
  memset(m, 0, sizeof(snobol_match_t));

  snobol_buf out_buf = {nullptr};
  snobol_buf_init(&out_buf);

  VM vm;
  memset(&vm, 0, sizeof(VM));
  vm.bc = pattern->bc;
  vm.bc_len = pattern->bc_len;
  vm.pattern = pattern;
  vm.range_meta = pattern->range_meta;
  vm.range_meta_count = pattern->range_meta_count;
  vm.s = subject;
  vm.len = len;
  vm.out = &out_buf;

  /* Use cached search metadata from compile time. Falls back to local
   * derivation only if the pattern was built without our compile path
   * (defensive — should not happen in practice). */
  snobol_search_meta_t meta;
  bool use_cached = pattern->meta_initialized;
  if (use_cached) {
    meta = pattern->meta;
  } else {
    snobol_search_derive_meta(pattern->bc, pattern->bc_len, &meta);
  }

  /* Build and cache DFA for eligible patterns (lazy: reuse cached) */
  snobol_dfa_t *dfa = nullptr;
  if (meta.automaton_eligible) {
    dfa = snobol_pattern_get_automaton(pattern);
    if (!dfa) {
      dfa = build_dfa(pattern->bc, pattern->bc_len, &vm);
      if (dfa) {
        snobol_pattern_set_automaton(pattern, dfa);
      }
    }
  }

  snobol_search_result_t sr;
  bool ok = snobol_search_exec(&vm, subject, len, 0, &meta, dfa, &sr, nullptr);
  m->success = ok;
  if (ok) {
    m->position = sr.match_start;
    m->length = sr.match_end - sr.match_start;
  }

  if (ok && out_buf.len > 0) {
    m->output = (char *)snobol_malloc(out_buf.len + 1);
    if (m->output) {
      memcpy(m->output, out_buf.data, out_buf.len);
      m->output[out_buf.len] = '\0';
      m->output_len = out_buf.len;
    }
  }

  int n = (int)vm.var_count;
  if (n > API_MAX_VARS) {
    n = API_MAX_VARS;
  }
  m->var_count = n;
  /* Capture offsets are relative to the match window; anchor var_subject
   * to the window base (subject + match position) so captures materialize
   * the correct absolute bytes for matches away from offset 0.  Only touch
   * sr.match_start when the search succeeded (it is unset on failure). */
  if (ok && n > 0) {
    const char *win_subject = subject + sr.match_start;
    size_t win_len = (sr.match_start <= len) ? len - sr.match_start : 0;
    for (int i = 0; i < n; i++) {
      size_t vs = vm.var_start[i];
      size_t ve = vm.var_end[i];
      match_store_capture(m, win_subject, i, vs, ve, win_len);
    }
  }

  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  snobol_search_vm_cleanup(&vm);
  return m;
}

void snobol_match_free(snobol_match_t *match) {
  if (!match) {
    return;
  }
  snobol_free(match->output);
  for (int i = 0; i < API_MAX_VARS; i++) {
    snobol_free(match->var_values[i]);
  }
  snobol_free(match);
}

snobol_match_t *snobol_match_create(void) {
  snobol_match_t *m = (snobol_match_t *)snobol_malloc(sizeof(snobol_match_t));
  if (m) {
    memset(m, 0, sizeof(snobol_match_t));
  }
  return m;
}

void snobol_match_reset(snobol_match_t *match) {
  if (!match) {
    return;
  }
  /* Free allocated strings before zeroing */
  snobol_free(match->output);
  for (int i = 0; i < API_MAX_VARS; i++) {
    snobol_free(match->var_values[i]);
  }
  /* Zero the entire struct */
  memset(match, 0, sizeof(snobol_match_t));
}

bool snobol_pattern_search_reuse(snobol_pattern_t *pattern, const char *subject,
                                 size_t len, snobol_match_t *match_out) {
  if (!pattern || !subject || !match_out) {
    return false;
  }

  /* Reset the match object for reuse */
  snobol_match_reset(match_out);

  snobol_buf out_buf = {nullptr};
  snobol_buf_init(&out_buf);

  VM vm;
  memset(&vm, 0, sizeof(VM));
  vm.bc = pattern->bc;
  vm.bc_len = pattern->bc_len;
  vm.range_meta = pattern->range_meta;
  vm.range_meta_count = pattern->range_meta_count;
  vm.s = subject;
  vm.len = len;
  vm.out = &out_buf;

  /* Use cached search metadata from compile time */
  const snobol_search_meta_t *meta = &pattern->meta;

  /* Build and cache DFA for eligible patterns */
  snobol_dfa_t *dfa = nullptr;
  if (meta->automaton_eligible) {
    dfa = snobol_pattern_get_automaton(pattern);
    if (!dfa) {
      dfa = build_dfa(pattern->bc, pattern->bc_len, &vm);
      if (dfa) {
        snobol_pattern_set_automaton(pattern, dfa);
      }
    }
  }

  snobol_search_result_t sr;
  bool ok = snobol_search_exec(&vm, subject, len, 0, meta, dfa, &sr, nullptr);
  match_out->success = ok;
  if (ok) {
    match_out->position = sr.match_start;
    match_out->length = sr.match_end - sr.match_start;
  }

  if (ok && out_buf.len > 0) {
    match_out->output = (char *)snobol_malloc(out_buf.len + 1);
    if (match_out->output) {
      memcpy(match_out->output, out_buf.data, out_buf.len);
      match_out->output[out_buf.len] = '\0';
      match_out->output_len = out_buf.len;
    }
  }

  int n = (int)vm.var_count;
  if (n > API_MAX_VARS) {
    n = API_MAX_VARS;
  }
  match_out->var_count = n;
  /* Capture offsets are relative to the match window; anchor var_subject
   * to the window base (subject + match position) so captures materialize
   * the correct absolute bytes for matches away from offset 0.  Only touch
   * sr.match_start when the search succeeded (it is unset on failure). */
  if (ok && n > 0) {
    const char *win_subject = subject + sr.match_start;
    size_t win_len = (sr.match_start <= len) ? len - sr.match_start : 0;
    for (int i = 0; i < n; i++) {
      size_t vs = vm.var_start[i];
      size_t ve = vm.var_end[i];
      match_store_capture(match_out, win_subject, i, vs, ve, win_len);
    }
  }

  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  snobol_search_vm_cleanup(&vm);
  return ok;
}

/* ---------------------------------------------------------------------------
 * Stateful search API
 *
 * For hot loops (PHP Pattern::searchSplit iterates 1000+ times per call),
 * the per-call allocation cost of snobol_match_t, the output buffer, the
 * VM struct, and the search metadata derivation dominates.
 * snobol_pattern_search_ex() amortises all of that.
 *
 * The state holds:
 *   - A reference to the pattern (must outlive the state)
 *   - A reusable VM struct (allocated on first use, fields-only reset
 *     per call via search_reset_vm())
 *   - A reusable output buffer (snobol_buf)
 *   - A reusable snobol_match_t that the caller reads but does not free
 * ---------------------------------------------------------------------------
 */

struct snobol_pattern_search_state {
  const uint8_t *bc; /* borrowed, not owned */
  size_t bc_len;
  snobol_pattern_t *pattern; /* optional owning pattern (for trie cache) */
  snobol_buf out_buf;        /* reused across calls */
  VM vm;                     /* reused, fields-only reset per call */
  snobol_match_t match;      /* overwritten on each call */
  snobol_search_meta_t meta; /* derived once at create time */
  snobol_range_meta_t *range_meta; /* owned — derived once at create time */
  size_t range_meta_count;
  snobol_dfa_t *dfa;    /* cached automaton (Tier 7), built once per state */
  struct simd_nfa *nfa; /* cached SIMD NFA (Tier 9), built once per state */
  bool vm_inited;       /* true after first search call sets it up */
  bool buf_inited;      /* true after first out_buf_init */
};

snobol_pattern_search_state_t *snobol_pattern_search_state_create(
    const uint8_t *bc, size_t bc_len) {
  if (!bc || bc_len == 0) {
    return nullptr;
  }
  snobol_pattern_search_state_t *state =
      (snobol_pattern_search_state_t *)snobol_malloc(sizeof(*state));
  if (!state) {
    return nullptr;
  }
  memset(state, 0, sizeof(*state));
  state->bc = bc;
  state->bc_len = bc_len;
  /* Derive search metadata once — reused across all search calls */
  snobol_search_derive_meta(bc, bc_len, &state->meta);
  snobol_build_range_meta(bc, bc_len, &state->range_meta,
                          &state->range_meta_count);
  return state;
}

void snobol_pattern_search_state_set_pattern(
    snobol_pattern_search_state_t *state, snobol_pattern_t *pattern) {
  if (state) {
    state->pattern = pattern;
  }
}

void snobol_pattern_search_state_destroy(snobol_pattern_search_state_t *state) {
  if (!state) {
    return;
  }
  snobol_search_meta_free(&state->meta);
  if (state->dfa) {
    snobol_dfa_free(state->dfa);
  }
  if (state->nfa) {
    snobol_free(state->nfa);
    state->nfa = nullptr;
  }
  if (state->buf_inited) {
    snobol_buf_free(&state->out_buf);
  }
  vm_free_labels(&state->vm);
  snobol_search_vm_cleanup(&state->vm);
  if (state->range_meta) {
    snobol_free(state->range_meta);
  }
  if (state->match.output) {
    snobol_free(state->match.output);
    state->match.output = nullptr;
  }
  for (int i = 0; i < API_MAX_VARS; i++) {
    if (state->match.var_values[i]) {
      snobol_free(state->match.var_values[i]);
      state->match.var_values[i] = nullptr;
    }
  }
  snobol_free(state);
}

void snobol_pattern_search_state_set_trie_cache(
    snobol_pattern_search_state_t *state, snobol_auto_trie_t *trie) {
  if (state) {
    state->vm.trie_cache = trie;
  }
}

void snobol_pattern_search_state_set_eval_fn(
    snobol_pattern_search_state_t *state,
    bool (*eval_fn)(int fn_id, const char *s, size_t start, size_t end,
                    void *userdata),
    void *eval_udata) {
  if (state) {
    state->vm.eval_fn = eval_fn;
    state->vm.eval_udata = eval_udata;
  }
}

snobol_match_t *snobol_pattern_search_ex(snobol_pattern_search_state_t *state,
                                         const char *subject,
                                         size_t subject_len,
                                         size_t start_offset) {
  if (!state || !subject) {
    return nullptr;
  }

  /* Lazy init: output buffer on first call, JIT context on first call.
   * The VM struct is initialised fields-only (no memset) on every
   * call — search_reset_vm() handles the per-candidate reset. */
  if (!state->buf_inited) {
    snobol_buf_init(&state->out_buf);
    state->buf_inited = true;
  }

  if (!state->vm_inited) {
    /* Preserve a pre-wired EVAL callback across the init memset: callers
     * may set it via snobol_pattern_search_state_set_eval_fn() before the
     * first search call, and a wipe here silently disables host callbacks. */
    bool (*saved_eval_fn)(int fn_id, const char *s, size_t start, size_t end,
                          void *udata) = state->vm.eval_fn;
    void *saved_eval_udata = state->vm.eval_udata;
    memset(&state->vm, 0, sizeof(VM));
    state->vm.eval_fn = saved_eval_fn;
    state->vm.eval_udata = saved_eval_udata;
    state->vm.bc = (uint8_t *)state->bc;
    state->vm.bc_len = state->bc_len;
    state->vm.pattern = state->pattern;
    state->vm.range_meta = state->range_meta;
    state->vm.range_meta_count = state->range_meta_count;
    state->vm.out = &state->out_buf;
    state->vm_inited = true;
  }

  /* Free any output from the previous call */
  if (state->match.output) {
    snobol_free(state->match.output);
    state->match.output = nullptr;
    state->match.output_len = 0;
  }
  for (int i = 0; i < API_MAX_VARS; i++) {
    if (state->match.var_values[i]) {
      snobol_free(state->match.var_values[i]);
      state->match.var_values[i] = nullptr;
      state->match.var_lens[i] = 0;
    }
  }
  state->match.success = false;
  state->match.var_count = 0;

  /* Reset out_buf length (keeps capacity) */
  state->out_buf.len = 0;
  if (state->out_buf.cap > 0 && state->out_buf.data) {
    state->out_buf.data[0] = '\0';
  }

  /* Use the search metadata derived at state creation time.
   * Reuse the cached DFA (built once and attached to the owning pattern) so
   * automaton-eligible patterns route to the fast Tier 7 path on every call —
   * the previous dfa=NULL argument disabled automaton acceleration in the
   * reuse path, forcing the slower SEARCH_VM/GENERAL tiers and destroying the
   * reuse API's whole reason to exist. */
  snobol_dfa_t *dfa = nullptr;
  if (state->meta.automaton_eligible) {
    /* Cache the DFA on the search state (not on the pattern): the PHP-side
     * pattern struct layout differs from the core snobol_pattern_t and has no
     * automaton slot, so caching there read uninitialised memory.  The DFA is
     * derived solely from state->bc/state->bc_len, which are stable for the
     * state's lifetime, so caching on the state is both correct and safe. */
    dfa = state->dfa;
    if (!dfa) {
      dfa = build_dfa(state->bc, state->bc_len, &state->vm);
      if (dfa) {
        state->dfa = dfa;
      }
    }
  }

  /* Build and cache the SIMD NFA on the state (not in tier_simd_nfa, which
   * uses stack for stateless calls).  Mirrors the DFA caching pattern. */
  if (!state->nfa && state->meta.simd_eligible) {
    state->nfa = build_nfa_masks_alloc(state->bc, state->bc_len, &state->vm);
  }
  state->vm.simd_nfa = state->nfa;

  snobol_search_result_t sr;
  bool ok = snobol_search_exec(&state->vm, subject, subject_len, start_offset,
                               &state->meta, dfa, &sr, nullptr);
  state->match.success = ok;
  /* sr.match_start is already an absolute position in the subject
   * (not relative to start_offset). Do NOT add start_offset again.
   * Only read it when the search succeeded (unset on failure). */
  if (ok) {
    state->match.position = sr.match_start;
    state->match.length = sr.match_end - sr.match_start;
  }

  if (ok && state->out_buf.len > 0) {
    state->match.output = (char *)snobol_malloc(state->out_buf.len + 1);
    if (state->match.output) {
      memcpy(state->match.output, state->out_buf.data, state->out_buf.len);
      state->match.output[state->out_buf.len] = '\0';
      state->match.output_len = state->out_buf.len;
    }
  }

  int n = (int)state->vm.var_count;
  if (n > API_MAX_VARS) {
    n = API_MAX_VARS;
  }
  state->match.var_count = n;
  /* The VM computes capture offsets relative to the match window (its
   * subject base is subject + match position), not to start_offset:
   * candidates before the match may have failed.  Anchor var_subject to
   * the match position and bound against the remaining window length so
   * materialization reads the correct absolute span on every call. */
  if (ok && n > 0) {
    const char *win_subject = subject + sr.match_start;
    size_t win_len =
        (sr.match_start <= subject_len) ? subject_len - sr.match_start : 0;
    for (int i = 0; i < n; i++) {
      size_t vs = state->vm.var_start[i];
      size_t ve = state->vm.var_end[i];
      match_store_capture(&state->match, win_subject, i, vs, ve, win_len);
    }
  }

  return &state->match;
}

/* ---------------------------------------------------------------------------
 * Stateful anchored search
 *
 * Identical to snobol_pattern_search_ex() but uses snobol_search_exec_anchored
 * so the match must start at offset 0 (SNOBOL-style anchored match).
 * Reuses the same persistent VM, DFA, range_meta, and output buffer.
 * ---------------------------------------------------------------------------
 */
snobol_match_t *snobol_pattern_search_ex_anchored(
    snobol_pattern_search_state_t *state, const char *subject,
    size_t subject_len) {
  if (!state || !subject) {
    return nullptr;
  }

  /* Lazy init: output buffer on first call */
  if (!state->buf_inited) {
    snobol_buf_init(&state->out_buf);
    state->buf_inited = true;
  }

  if (!state->vm_inited) {
    bool (*saved_eval_fn)(int fn_id, const char *s, size_t start, size_t end,
                          void *udata) = state->vm.eval_fn;
    void *saved_eval_udata = state->vm.eval_udata;
    memset(&state->vm, 0, sizeof(VM));
    state->vm.eval_fn = saved_eval_fn;
    state->vm.eval_udata = saved_eval_udata;
    state->vm.bc = (uint8_t *)state->bc;
    state->vm.bc_len = state->bc_len;
    state->vm.pattern = state->pattern;
    state->vm.range_meta = state->range_meta;
    state->vm.range_meta_count = state->range_meta_count;
    state->vm.out = &state->out_buf;
    state->vm_inited = true;
  }

  /* Free output/captures from previous call */
  if (state->match.output) {
    snobol_free(state->match.output);
    state->match.output = nullptr;
    state->match.output_len = 0;
  }
  for (int i = 0; i < API_MAX_VARS; i++) {
    if (state->match.var_values[i]) {
      snobol_free(state->match.var_values[i]);
      state->match.var_values[i] = nullptr;
      state->match.var_lens[i] = 0;
    }
  }
  state->match.success = false;
  state->match.var_count = 0;

  /* Reset out_buf length (keeps capacity) */
  state->out_buf.len = 0;
  if (state->out_buf.cap > 0 && state->out_buf.data) {
    state->out_buf.data[0] = '\0';
  }

  /* Build and cache DFA for automaton-eligible patterns */
  snobol_dfa_t *dfa = nullptr;
  if (state->meta.automaton_eligible) {
    dfa = state->dfa;
    if (!dfa) {
      dfa = build_dfa(state->bc, state->bc_len, &state->vm);
      if (dfa) {
        state->dfa = dfa;
      }
    }
  }

  /* Build and cache SIMD NFA */
  if (!state->nfa && state->meta.simd_eligible) {
    state->nfa = build_nfa_masks_alloc(state->bc, state->bc_len, &state->vm);
  }
  state->vm.simd_nfa = state->nfa;

  /* Anchored search — must start at offset 0 */
  snobol_search_result_t sr;
  bool ok = snobol_search_exec_anchored(&state->vm, subject, subject_len,
                                        &state->meta, dfa, &sr, nullptr);
  state->match.success = ok;
  if (ok) {
    state->match.position = sr.match_start;
    state->match.length = sr.match_end - sr.match_start;
  }

  if (ok && state->out_buf.len > 0) {
    state->match.output = (char *)snobol_malloc(state->out_buf.len + 1);
    if (state->match.output) {
      memcpy(state->match.output, state->out_buf.data, state->out_buf.len);
      state->match.output[state->out_buf.len] = '\0';
      state->match.output_len = state->out_buf.len;
    }
  }

  int n = (int)state->vm.var_count;
  if (n > API_MAX_VARS) {
    n = API_MAX_VARS;
  }
  state->match.var_count = n;
  /* Anchored: start_offset is always 0, so captures are subject-absolute */
  for (int i = 0; i < n; i++) {
    size_t vs = state->vm.var_start[i];
    size_t ve = state->vm.var_end[i];
    match_store_capture(&state->match, subject, i, vs, ve, subject_len);
  }

  return &state->match;
}

/* ---------------------------------------------------------------------------
 * Lightweight unanchored search for single-literal patterns.
 *
 * snobol_pattern_search_next() skips all match-struct, capture, and output
 * overhead.  It returns the position and length of the next occurrence of
 * the pattern's literal via out-parameters, using only meta->required_lit
 * and meta->required_lit_len (already derived at state creation time).
 * The per-call cost is ~15 ns vs ~91 ns for snobol_pattern_search_ex.
 *
 * For non-literal patterns (meta->is_literal_only == false) the function
 * returns false — the caller must fall back to snobol_pattern_search_ex().
 * ---------------------------------------------------------------------------
 */
bool snobol_pattern_search_next(snobol_pattern_search_state_t *state,
                                const char *subject, size_t subject_len,
                                size_t start_offset, size_t *out_pos,
                                size_t *out_len) {
  if (!state || !subject || !out_pos || !out_len) {
    return false;
  }
  if (!state->meta.is_literal_only || state->meta.required_lit_len == 0) {
    return false;
  }
  if (start_offset > subject_len) {
    return false;
  }

  size_t remain = subject_len - start_offset;
  const void *found;
  if (state->meta.required_lit_len == 1) {
    found = memchr(subject + start_offset, state->meta.required_lit[0], remain);
  } else {
    found = memmem(subject + start_offset, remain, state->meta.required_lit,
                   state->meta.required_lit_len);
  }
  if (!found) {
    return false;
  }

  *out_pos = (const char *)found - subject;
  *out_len = state->meta.required_lit_len;
  return true;
}

/* ---------------------------------------------------------------------------
 * Batch-search API
 *
 * Finds all non-overlapping matches in a single pass by inlining the search
 * loop — calls snobol_search_exec() directly (no wrapper), resets only VM
 * fields between matches, and collects positions/lengths/captures/outputs
 * into flat arrays allocated with snobol_malloc.
 *
 * Returns false for non-search-VM-eligible patterns (EVAL, ASSIGN, DYNAMIC)
 * so the caller can fall back to the per-call loop.
 * ---------------------------------------------------------------------------
 */

/* Core batch search loop, shared by the stateless and stateful batch entry
 * points. Reuses the caches already on `state` (range_meta, out_buf, and — for
 * automaton-eligible patterns — the DFA, trie, and SIMD NFA), building them
 * lazily on first use and reusing them on every subsequent call. Result arrays
 * are allocated fresh per call and returned in `out` (caller-owned, freed via
 * snobol_batch_result_free). The caller must zero `out` and set out->eligible
 * before calling; this function returns the match status but never touches
 * out->eligible. */
static bool batch_run(snobol_pattern_search_state_t *state, const char *subject,
                      size_t len, snobol_batch_result_t *out) {
  VM *vm = &state->vm;
  const snobol_search_meta_t *meta = &state->meta;

  if (!state->vm_inited) {
    bool (*saved_eval_fn)(int fn_id, const char *s, size_t start, size_t end,
                          void *udata) = state->vm.eval_fn;
    void *saved_eval_udata = state->vm.eval_udata;
    memset(vm, 0, sizeof(VM));
    vm->eval_fn = saved_eval_fn;
    vm->eval_udata = saved_eval_udata;
    vm->bc = (uint8_t *)state->bc;
    vm->bc_len = state->bc_len;
    vm->pattern = state->pattern;
    vm->range_meta = state->range_meta;
    vm->range_meta_count = state->range_meta_count;
    state->vm_inited = true;
  }
  if (!state->buf_inited) {
    snobol_buf_init(&state->out_buf);
    state->buf_inited = true;
  }
  snobol_buf *out_buf = &state->out_buf;
  vm->out = out_buf;

  /* Lazily build DFA for automaton-eligible patterns (once per state). */
  snobol_dfa_t *dfa = nullptr;
  if (meta->automaton_eligible) {
    if (!state->dfa) {
      state->dfa = build_dfa(state->bc, state->bc_len, vm);
    }
    dfa = state->dfa;
  }

  /* ---- Allocate result arrays (caller-owned, fresh per call) ---- */
  size_t cap = 64;
  size_t *positions = (size_t *)snobol_malloc(cap * sizeof(size_t));
  size_t *lengths = (size_t *)snobol_malloc(cap * sizeof(size_t));
  size_t *output_lens = (size_t *)snobol_malloc(cap * sizeof(size_t));
  size_t outbuf_cap = 1024;
  char *outbuf_data = (char *)snobol_malloc(outbuf_cap);

  if (!positions || !lengths || !output_lens || !outbuf_data) {
    snobol_free(positions);
    snobol_free(lengths);
    snobol_free(output_lens);
    snobol_free(outbuf_data);
    return false;
  }

  /* Capture arrays: allocate rows for MAX_VARS lazily.  Each row's capacity
   * (in match pairs) is tracked separately in row_caps because the result
   * arrays' `cap` doubles in the main loop — a realloc condition against the
   * live `cap` would never fire for rows (they would overflow past 64
   * matches). */
  bool has_caps = meta->has_capture;
  size_t **captures = nullptr;
  size_t *row_caps = nullptr;
  if (has_caps) {
    captures = (size_t **)snobol_calloc((size_t)MAX_VARS, sizeof(size_t *));
    row_caps = (size_t *)snobol_calloc((size_t)MAX_VARS, sizeof(size_t));
    if (!captures || !row_caps) {
      snobol_free(positions);
      snobol_free(lengths);
      snobol_free(output_lens);
      snobol_free(outbuf_data);
      if (captures) {
        snobol_free(captures);
      }
      if (row_caps) {
        snobol_free(row_caps);
      }
      return false;
    }
  }

  /* ---- Main search loop ---- */
  size_t count = 0;
  size_t offset = 0;
  size_t out_pos = 0; /* write cursor into outbuf_data */
  size_t max_var_count = 0;

  while (offset <= len) {
    /* Set subject pointers before each call */
    vm->s = subject;
    vm->len = len;

    snobol_search_result_t sr;
    bool ok =
        snobol_search_exec(vm, subject, len, offset, meta, dfa, &sr, nullptr);
    if (!ok || sr.aborted) {
      break;
    }

    /* Grow position/length/output_len arrays if needed */
    if (count >= cap) {
      size_t new_cap = cap * 2;
      size_t *np =
          (size_t *)snobol_realloc(positions, new_cap * sizeof(size_t));
      size_t *nl = (size_t *)snobol_realloc(lengths, new_cap * sizeof(size_t));
      size_t *no =
          (size_t *)snobol_realloc(output_lens, new_cap * sizeof(size_t));
      if (!np || !nl || !no) {
        snobol_free(np ? np : positions);
        snobol_free(nl ? nl : lengths);
        snobol_free(no ? no : output_lens);
        positions = lengths = nullptr;
        output_lens = nullptr;
        count = 0; /* signal error to cleanup below */
        break;
      }
      positions = np;
      lengths = nl;
      output_lens = no;
      cap = new_cap;
    }

    size_t mstart = sr.match_start;
    size_t mlen = sr.match_end - sr.match_start;

    positions[count] = mstart;
    lengths[count] = mlen;
    output_lens[count] = 0;

    /* Collect captures. The VM stores offsets relative to the match window
     * (the candidate where the match succeeded — candidates before it may
     * have failed), so add sr.match_start (the match position) to get
     * absolute subject positions. */
    if (has_caps && captures) {
      int nv = (int)vm->var_count;
      if (nv > (int)max_var_count) {
        max_var_count = (size_t)nv;
      }
      if (nv > MAX_VARS) {
        nv = MAX_VARS;
      }
      for (int ri = 0; ri < nv; ri++) {
        if (!captures[ri]) {
          captures[ri] = (size_t *)snobol_calloc(cap, 2 * sizeof(size_t));
          if (!captures[ri]) {
            continue;
          }
          row_caps[ri] = cap;
        } else if (count >= row_caps[ri]) {
          /* Row capacity is in match pairs; double it (catching up to the
           * grown result arrays) and zero the new tail. */
          size_t new_row_cap = row_caps[ri] * 2;
          while (new_row_cap <= count) {
            new_row_cap *= 2;
          }
          size_t *new_row = (size_t *)snobol_realloc(
              captures[ri], new_row_cap * 2 * sizeof(size_t));
          if (!new_row) {
            continue;
          }
          captures[ri] = new_row;
          memset(captures[ri] + row_caps[ri] * 2, 0,
                 (new_row_cap - row_caps[ri]) * 2 * sizeof(size_t));
          row_caps[ri] = new_row_cap;
        }
        size_t vs = vm->var_start[ri];
        size_t ve = vm->var_end[ri];
        captures[ri][count * 2] = sr.match_start + vs;
        captures[ri][(count * 2) + 1] = (ve > vs) ? (ve - vs) : 0;
      }
    }

    /* Collect output (EMIT ops). Always store a NUL-terminated entry per
     * match — including empty-string entries for matches without output —
     * so that PHP iteration can index into the concatenated buffer directly. */
    size_t out_len = out_buf->len > 0 ? out_buf->len : 0;
    size_t needed = out_pos + out_len + 1; /* data + NUL */
    if (needed > outbuf_cap) {
      while (outbuf_cap < needed) {
        outbuf_cap *= 2;
      }
      char *new_data = (char *)snobol_realloc(outbuf_data, outbuf_cap);
      if (new_data) {
        outbuf_data = new_data;
      }
    }
    if (outbuf_data) {
      if (out_len > 0) {
        memcpy(outbuf_data + out_pos, out_buf->data, out_len);
      }
      outbuf_data[out_pos + out_len] = '\0';
      out_pos += out_len + 1;
      output_lens[count] = out_len;
    } else {
      output_lens[count] = 0;
    }
    /* Clear output buffer for next match (keep capacity) */
    out_buf->len = 0;
    if (out_buf->cap > 0 && out_buf->data) {
      out_buf->data[0] = '\0';
    }

    count++;

    /* Advance past this match.  For zero-length matches, advance by 1 byte
     * to avoid infinite loop (SNOBOL4 semantics). */
    offset = mstart + (mlen > 0 ? mlen : 1);
  }

  /* Reset reusable VM working state. State-owned caches (range_meta, out_buf,
   * dfa, trie, simd nfa) are intentionally preserved for the next call. */
  out_buf->len = 0;
  vm_free_labels(vm);
  snobol_search_vm_cleanup(vm);

  /* ---- Populate output struct ---- */
  if (count == 0 || !positions || !lengths) {
    snobol_free(positions);
    snobol_free(lengths);
    snobol_free(output_lens);
    snobol_free(outbuf_data);
    if (captures) {
      for (int i = 0; i < MAX_VARS; i++) {
        snobol_free(captures[i]);
      }
      snobol_free(captures);
    }
    if (row_caps) {
      snobol_free(row_caps);
    }
    return false;
  }

  out->match_count = count;
  out->positions = positions;
  out->lengths = lengths;
  out->var_count = max_var_count;
  out->captures = captures; /* may be NULL when no captures */
  out->outputs = (out_pos > 0) ? outbuf_data : nullptr;
  out->output_lens = output_lens;

  if (row_caps) {
    snobol_free(row_caps);
  }

  return true;
}

bool snobol_pattern_search_batch(const uint8_t *bc, size_t bc_len,
                                 const char *subject, size_t len,
                                 const snobol_search_meta_t *meta,
                                 snobol_batch_result_t *out) {
  /* Zero the output struct so partial failure cleanup is safe */
  memset(out, 0, sizeof(*out));

  if (!bc || !subject || !meta) {
    return false;
  }

  /* Non-search-VM-eligible patterns (EVAL, ASSIGN, DYNAMIC) must fall back
   * to per-call loop — each match's side effects affect the next. */
  if (!snobol_meta_search_vm_eligible(meta)) {
    return false;
  }

  /* Eligible patterns keep eligible == true even on zero matches, so callers
   * can distinguish "done, no matches" from "not batchable, fall back". */
  out->eligible = true;

  /* Delegate to a temporary state: metadata, range_meta and (for
   * automaton-eligible patterns) the DFA are derived/built once for this call.
   * The stateful snobol_pattern_search_batch_ex() reuses a persistent state to
   * amortise that cost across calls. */
  snobol_pattern_search_state_t *st =
      snobol_pattern_search_state_create(bc, bc_len);
  if (!st) {
    return false;
  }
  bool ok = batch_run(st, subject, len, out);
  snobol_pattern_search_state_destroy(st);
  return ok;
}

bool snobol_pattern_search_batch_ex(snobol_pattern_search_state_t *state,
                                    const char *subject, size_t len,
                                    snobol_batch_result_t *out) {
  /* Zero the output struct so partial failure cleanup is safe */
  memset(out, 0, sizeof(*out));

  if (!state || !subject) {
    return false;
  }

  /* Non-search-VM-eligible patterns must fall back to the per-call loop. */
  if (!snobol_meta_search_vm_eligible(&state->meta)) {
    return false;
  }

  out->eligible = true;
  return batch_run(state, subject, len, out);
}

void snobol_batch_result_free(snobol_batch_result_t *out) {
  if (!out) {
    return;
  }
  snobol_free(out->positions);
  snobol_free(out->lengths);
  snobol_free(out->output_lens);
  snobol_free(out->outputs);
  if (out->captures) {
    for (size_t i = 0; i < MAX_VARS; i++) {
      snobol_free(out->captures[i]);
    }
    snobol_free(out->captures);
  }
  memset(out, 0, sizeof(*out));
}

/* ---------------------------------------------------------------------------
 * Match result access
 * ---------------------------------------------------------------------------
 */

bool snobol_match_success(snobol_match_t *match) {
  return (match && match->success) != 0;
}

const char *snobol_match_get_output(snobol_match_t *match, size_t *len) {
  if (!match || !match->success) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  if (len) {
    *len = match->output_len;
  }
  return match->output ? match->output : "";
}

size_t snobol_match_get_position(const snobol_match_t *match) {
  if (!match || !match->success) {
    return 0;
  }
  return match->position;
}

size_t snobol_match_get_length(const snobol_match_t *match) {
  if (!match || !match->success) {
    return 0;
  }
  return match->length;
}

const char *snobol_match_get_variable(snobol_match_t *match, const char *name,
                                      size_t *len) {
  if (!match || !match->success || !name) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  /* Variable name is the capture register number, either as a bare decimal
   * integer ("0", "1", …) or with a "v" prefix ("v0", "v1", …) to match the
   * PHP binding's capture-array keys.  The engine stores capture r at
   * var_start[r], so the (optionally v-prefixed) number maps directly to the
   * array index. */
  const char *p = name;
  if (p[0] == 'v') {
    p++;
  }
  char *end;
  long idx = strtol(p, &end, 10);
  if (end == p || idx < 0 || idx > API_MAX_VARS) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  int i = (int)idx;
  if (i >= match->var_count) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  /* Materialize lazily on first access: copy the (offset, length) register
   * into an owned NUL-terminated string.  An unmaterialized register has
   * var_values[i] == NULL; the (offset, length) is still valid for both
   * non-empty and zero-width (empty) captures, so materialize either way. */
  if (!match->var_values[i] && match->var_subject) {
    size_t vlen = match->var_len[i];
    char *buf = (char *)snobol_malloc(vlen + 1);
    if (buf) {
      if (vlen > 0) {
        memcpy(buf, match->var_subject + match->var_off[i], vlen);
      }
      buf[vlen] = '\0';
      match->var_values[i] = buf;
      match->var_lens[i] = vlen;
    }
  }
  if (!match->var_values[i]) {
    if (len) {
      *len = 0;
    }
    return nullptr;
  }
  if (len) {
    *len = match->var_lens[i];
  }
  return match->var_values[i];
}

/* ---------------------------------------------------------------------------
 * One-shot convenience match API
 * ---------------------------------------------------------------------------
 */

/* Store capture `i` register-style: keep (subject, offset, length) and defer
 * the owned-byte copy until snobol_match_get_variable() is called.  A NULL
 * var_values[i] with var_len[i] > 0 signals "present, not yet materialized". */
static void match_store_capture(snobol_match_t *m, const char *subject, int i,
                                size_t vs, size_t ve, size_t subject_len) {
  if (ve <= vs || ve > subject_len) {
    return;
  }
  m->var_subject = subject;
  m->var_off[i] = vs;
  m->var_len[i] = ve - vs;
  m->var_values[i] = nullptr; /* materialize on demand */
  m->var_lens[i] = 0;
}

#define MATCH_MAX_CAPTURES 64

snobol_match_result_t *snobol_match(const char *pattern, size_t pat_len,
                                    const char *subject, size_t sub_len,
                                    uint32_t flags) {
  snobol_match_result_t *result =
      (snobol_match_result_t *)snobol_malloc(sizeof(snobol_match_result_t));
  if (!result) {
    return nullptr;
  }
  memset(result, 0, sizeof(snobol_match_result_t));

  /* Compile pattern */
  bool case_insensitive = (flags & SNOBOL_FLAG_CASE_INSENSITIVE) != 0;
  snobol_context_t *ctx =
      nullptr; /* not needed for compile, but required by API */
  char *compile_error = nullptr;
  snobol_pattern_t *pat =
      do_compile(pattern, pat_len, case_insensitive, &compile_error);

  if (!pat) {
    if (compile_error) {
      result->error = compile_error; /* transfer ownership */
    } else {
      result->error = snobol_malloc(16);
      if (result->error) {
        memcpy(result->error, "unknown error", 14);
      }
    }
    return result;
  }

  /* Set up output buffer */
  snobol_buf out_buf = {nullptr};
  snobol_buf_init(&out_buf);

  /* Initialise VM */
  VM vm;
  memset(&vm, 0, sizeof(VM));
  vm.bc = pat->bc;
  vm.bc_len = pat->bc_len;
  vm.range_meta = pat->range_meta;
  vm.range_meta_count = pat->range_meta_count;
  vm.s = subject;
  vm.len = sub_len;
  vm.out = &out_buf;

  bool ok = vm_run(&vm);
  result->success = ok;

  if (ok && out_buf.len > 0) {
    result->output = (char *)snobol_malloc(out_buf.len + 1);
    if (result->output) {
      memcpy(result->output, out_buf.data, out_buf.len);
      result->output[out_buf.len] = '\0';
      result->output_len = out_buf.len;
    }
  }

  /* Copy named variables */
  int n = (int)vm.var_count;
  if (n > MATCH_MAX_CAPTURES) {
    n = MATCH_MAX_CAPTURES;
  }
  result->capture_count = n;

  if (n > 0) {
    result->captures = (char **)snobol_calloc((size_t)n, sizeof(char *));
    result->capture_lens = (size_t *)snobol_calloc((size_t)n, sizeof(size_t));
    if (result->captures && result->capture_lens) {
      for (int i = 0; i < n; i++) {
        size_t vs = vm.var_start[i];
        size_t ve = vm.var_end[i];
        if (ve > vs && ve <= sub_len) {
          size_t vlen = ve - vs;
          result->captures[i] = (char *)snobol_malloc(vlen + 1);
          if (result->captures[i]) {
            memcpy(result->captures[i], subject + vs, vlen);
            result->captures[i][vlen] = '\0';
            result->capture_lens[i] = vlen;
          }
        }
      }
    }
  }

  snobol_buf_free(&out_buf);
  vm_free_labels(&vm);
  snobol_pattern_free(pat);
  snobol_free(compile_error);

  return result;
}

void snobol_match_result_free(snobol_match_result_t *result) {
  if (!result) {
    return;
  }
  snobol_free(result->error);
  snobol_free(result->output);
  if (result->captures) {
    for (int i = 0; i < result->capture_count; i++) {
      snobol_free(result->captures[i]);
    }
    snobol_free(result->captures);
  }
  snobol_free(result->capture_lens);
  snobol_free(result);
}

/* ---------------------------------------------------------------------------
 * Builder API
 * ---------------------------------------------------------------------------
 */

struct snobol_pattern_build {
  int _reserved;
};

snobol_pattern_build_t *snobol_pattern_build_create(void) {
  snobol_pattern_build_t *b =
      (snobol_pattern_build_t *)snobol_malloc(sizeof(snobol_pattern_build_t));
  if (b) {
    b->_reserved = 0;
  }
  return b;
}

void snobol_pattern_build_destroy(snobol_pattern_build_t *build) {
  if (build) {
    snobol_free(build);
  }
}

ast_node_t *snobol_pattern_build_lit(snobol_pattern_build_t *build,
                                     const char *text, size_t len) {
  (void)build;
  return snobol_ast_create_lit(text, len);
}

ast_node_t *snobol_pattern_build_span(snobol_pattern_build_t *build,
                                      const char *set, size_t len) {
  (void)build;
  return snobol_ast_create_span(set, len);
}

ast_node_t *snobol_pattern_build_brk(snobol_pattern_build_t *build,
                                     const char *set, size_t len) {
  (void)build;
  return snobol_ast_create_break(set, len);
}

ast_node_t *snobol_pattern_build_any(snobol_pattern_build_t *build,
                                     const char *set, size_t len) {
  (void)build;
  return snobol_ast_create_any(set, len);
}

ast_node_t *snobol_pattern_build_notany(snobol_pattern_build_t *build,
                                        const char *set, size_t len) {
  (void)build;
  return snobol_ast_create_notany(set, len);
}

ast_node_t *snobol_pattern_build_len(snobol_pattern_build_t *build, int32_t n) {
  (void)build;
  return snobol_ast_create_len(n);
}

ast_node_t *snobol_pattern_build_arbno(snobol_pattern_build_t *build,
                                       ast_node_t *sub) {
  (void)build;
  return snobol_ast_create_arbno(sub);
}

ast_node_t *snobol_pattern_build_cap(snobol_pattern_build_t *build, int reg,
                                     ast_node_t *sub) {
  (void)build;
  return snobol_ast_create_cap(reg, sub);
}

ast_node_t *snobol_pattern_build_assign(snobol_pattern_build_t *build, int var,
                                        int reg) {
  (void)build;
  return snobol_ast_create_assign(var, reg);
}

ast_node_t *snobol_pattern_build_concat(snobol_pattern_build_t *build,
                                        ast_node_t **parts, size_t count) {
  (void)build;
  return snobol_ast_create_concat(parts, count);
}

ast_node_t *snobol_pattern_build_alt(snobol_pattern_build_t *build,
                                     ast_node_t *left, ast_node_t *right) {
  (void)build;
  return snobol_ast_create_alt(left, right);
}

ast_node_t *snobol_pattern_build_label(snobol_pattern_build_t *build,
                                       const char *name, ast_node_t *target) {
  (void)build;
  /* snobol_ast_create_label copies the name; free our copy here. */
  char *name_copy = (char *)snobol_malloc(strlen(name) + 1);
  if (name_copy) {
    strcpy(name_copy, name);
  }
  ast_node_t *node = snobol_ast_create_label(name_copy, target);
  free(name_copy);
  return node;
}

ast_node_t *snobol_pattern_build_goto(snobol_pattern_build_t *build,
                                      const char *label) {
  (void)build;
  return snobol_ast_create_goto(label);
}

ast_node_t *snobol_pattern_build_pos(snobol_pattern_build_t *build, int32_t n) {
  (void)build;
  return snobol_ast_create_pos(n);
}

ast_node_t *snobol_pattern_build_tab(snobol_pattern_build_t *build, int32_t n) {
  (void)build;
  return snobol_ast_create_tab(n);
}

ast_node_t *snobol_pattern_build_rpos(snobol_pattern_build_t *build,
                                      int32_t n) {
  (void)build;
  return snobol_ast_create_rpos(n);
}

ast_node_t *snobol_pattern_build_rtab(snobol_pattern_build_t *build,
                                      int32_t n) {
  (void)build;
  return snobol_ast_create_rtab(n);
}

ast_node_t *snobol_pattern_build_breakx(snobol_pattern_build_t *build,
                                        const char *set, size_t len) {
  (void)build;
  return snobol_ast_create_breakx(set, len);
}

ast_node_t *snobol_pattern_build_bal(snobol_pattern_build_t *build,
                                     uint32_t open_cp, uint32_t close_cp) {
  (void)build;
  return snobol_ast_create_bal(open_cp, close_cp);
}

ast_node_t *snobol_pattern_build_fence(snobol_pattern_build_t *build) {
  (void)build;
  return snobol_ast_create_fence();
}

ast_node_t *snobol_pattern_build_rem(snobol_pattern_build_t *build) {
  (void)build;
  return snobol_ast_create_rem();
}

ast_node_t *snobol_pattern_build_abort(snobol_pattern_build_t *build) {
  (void)build;
  return snobol_ast_create_abort();
}

ast_node_t *snobol_pattern_build_fail(snobol_pattern_build_t *build) {
  (void)build;
  return snobol_ast_create_fail();
}

ast_node_t *snobol_pattern_build_succeed(snobol_pattern_build_t *build) {
  (void)build;
  return snobol_ast_create_succeed();
}

ast_node_t *snobol_pattern_build_emit(snobol_pattern_build_t *build,
                                      ast_node_t *root) {
  (void)build;
  /* Ownership is transferred to the caller; just return the root. */
  return root;
}

snobol_pattern_t *snobol_pattern_build_compile(snobol_context_t *ctx,
                                               ast_node_t *root, uint32_t flags,
                                               char **error) {
  (void)ctx; /* context owns the pattern conceptually; no registry yet */
  if (error) {
    *error = nullptr;
  }

  bool case_insensitive = (flags & SNOBOL_FLAG_CASE_INSENSITIVE) != 0;
  /* Unknown flag bits are intentionally ignored (forward-compatible). */
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_ast_to_bytecode_c(root, case_insensitive, &bc, &bc_len);
  snobol_ast_free(root); /* AST ownership consumed on both outcomes */

  if (rc != 0) {
    set_error_out(error, "compilation failed");
    return nullptr;
  }

  return pattern_finalize(bc, bc_len, case_insensitive, error);
}
