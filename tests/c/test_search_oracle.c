/**
 * test_search_oracle.c – Differential search oracle.
 *
 * Every pattern in corpus.h (plus a seeded generator's output) is executed
 * through the accelerated tier dispatch AND through a reference run of the
 * full VM (vm_exec) at each subject offset.  Success, match position, match
 * length, and capture values must agree exactly — a disagreement means the
 * compile-time analysis (derive_meta) or a tier accelerator changed the
 * observable behavior of the pattern.
 *
 * Also asserts structural soundness of the search metadata: a required
 * literal must be present on every accepting bytecode path (conservative
 * must-analysis walk), leading alternations must derive no required
 * literal, and tier classification must be consistent with the eligibility
 * flags.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snobol/snobol.h"
#include "snobol/vm.h"

#include "corpus.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

static void test_must_analysis_walker(void);
static void test_prefilter_loop_soundness(void);
static void test_trie_pool_fallback(void);

/* ===========================================================================
 * Reference runner: per-offset vm_exec on the compiled bytecode.
 * =========================================================================== */

#define ORACLE_MAX_SUBJECT 256
#define ORACLE_MAX_VARS 64

typedef struct {
  bool success;
  size_t pos; /* subject-absolute match start */
  size_t end; /* subject-absolute match end */
  /* Capture values keyed by VM register index (1-based end-to-end:
   * the parser hardcodes @rN captures to register 1, so variable "1"
   * lives at var_start[1], NOT var_start[0]). */
  bool has_var[ORACLE_MAX_VARS];
  char vars[ORACLE_MAX_VARS][ORACLE_MAX_SUBJECT + 1];
  size_t var_lens[ORACLE_MAX_VARS];
} oracle_ref_t;

/* Run the reference: first offset at which a full vm_exec run succeeds.
 * The VM is reset per offset exactly like the search path does
 * (search_reset_vm semantics). */
static void oracle_ref_run(const uint8_t *bc, size_t bc_len,
                           const snobol_range_meta_t *range_meta,
                           size_t range_meta_count, const char *subject,
                           size_t sub_len, oracle_ref_t *out) {
  memset(out, 0, sizeof(*out));
  if (sub_len > ORACLE_MAX_SUBJECT) {
    sub_len = ORACLE_MAX_SUBJECT;
  }

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.range_meta = range_meta;
  vm.range_meta_count = range_meta_count;
  snobol_buf out_buf;
  snobol_buf_init(&out_buf);
  vm.out = &out_buf;

  for (size_t off = 0; off <= sub_len; off++) {
    vm.s = subject + off;
    vm.len = sub_len - off;
    vm.ip = 0;
    vm.pos = 0;
    vm.var_count = 0;
    vm.max_cap_used = 0;
    vm.max_counter_used = 0;
    vm.choices_top = 0;
    bool ok = vm_exec(&vm);
    if (ok) {
      out->success = true;
      out->pos = off;
      out->end = off + vm.pos;
      /* Record every variable register the VM populated.  Registers are
       * 1-based: variable "N" lives at var_start[N]. */
      for (size_t k = 1; k < ORACLE_MAX_VARS; k++) {
        if (k >= vm.var_count) {
          break;
        }
        size_t s = vm.var_start[k];
        size_t e = vm.var_end[k];
        if (e < s || e > vm.len) {
          e = s;
        }
        size_t clen = e - s;
        if (clen > ORACLE_MAX_SUBJECT) {
          clen = ORACLE_MAX_SUBJECT;
        }
        memcpy(out->vars[k], subject + off + s, clen);
        out->vars[k][clen] = '\0';
        out->var_lens[k] = clen;
        out->has_var[k] = true;
      }
      break;
    }
    if (vm.abort_flag) {
      break;
    }
  }
  snobol_buf_free(&out_buf);
}

/* Compare a search-side match result against the reference.  Returns the
 * number of mismatched fields (0 = equivalent). */
static int oracle_compare_match(snobol_match_t *m, const oracle_ref_t *ref) {
  int mismatches = 0;
  bool s = snobol_match_success(m);
  if (s != ref->success) {
    return mismatches + 1;
  }
  if (!s) {
    return 0;
  }
  if (snobol_match_get_position(m) != ref->pos) {
    mismatches++;
    printf("  position: search=%zu ref=%zu\n", snobol_match_get_position(m),
           ref->pos);
  }
  if (snobol_match_get_length(m) != ref->end - ref->pos) {
    mismatches++;
    printf("  length: search=%zu ref=%zu\n", snobol_match_get_length(m),
           ref->end - ref->pos);
  }
  /* Capture values: compare content for every variable register present on
   * either side.  Names are 1-based ("1", "2", ...) and map 1:1 to VM
   * registers (variable "N" lives at register index N). */
  for (size_t k = 1; k < ORACLE_MAX_VARS; k++) {
    char name[8];
    snprintf(name, sizeof(name), "%zu", k);
    size_t len = 0;
    const char *val = snobol_match_get_variable(m, name, &len);
    if (val != NULL) {
      if (!ref->has_var[k]) {
        mismatches++;
        printf("  capture %s: search has value, reference does not\n", name);
        break;
      }
      if (len != ref->var_lens[k] ||
          (len > 0 && memcmp(val, ref->vars[k], len) != 0)) {
        mismatches++;
        printf("  capture %s: search=\"%.*s\" ref=\"%.*s\"\n", name, (int)len,
               val, (int)ref->var_lens[k], ref->vars[k]);
        break;
      }
    } else if (ref->has_var[k] && ref->var_lens[k] > 0) {
      /* Search dropped a non-empty variable the reference recorded. */
      mismatches++;
      printf("  capture %s: search missing, reference has \"%.*s\"\n", name,
             (int)ref->var_lens[k], ref->vars[k]);
      break;
    }
  }
  return mismatches;
}

/* Compile a pattern source; fills the out-params and returns true. */
static bool oracle_compile(const char *src, size_t len, uint32_t flags,
                           snobol_context_t *ctx, snobol_pattern_t **out_pat,
                           const uint8_t **out_bc, size_t *out_bc_len,
                           const snobol_search_meta_t **out_meta,
                           const snobol_range_meta_t **out_range,
                           size_t *out_range_count) {
  char *error = nullptr;
  snobol_pattern_t *pat =
      snobol_pattern_compile_ex(ctx, src, len, flags, &error);
  if (!pat) {
    printf("  compile failed: %s\n", error ? error : "unknown error");
    free(error);
    return false;
  }
  *out_pat = pat;
  *out_bc = snobol_pattern_get_bc(pat);
  *out_bc_len = snobol_pattern_get_bc_len(pat);
  *out_meta = snobol_pattern_get_meta(pat);
  *out_range = snobol_pattern_get_range_meta(pat, out_range_count);
  return true;
}

/* ===========================================================================
 * Equivalence harness: one pattern against one subject.
 * =========================================================================== */

/* Repetition patterns are exponential in the reference VM: a group
 * repetition like `('ab')*` or `('a'*)* 'b'` backtracks over every way to
 * partition the subject, which never terminates on long subjects (the
 * tier dispatch handles these via zero-progress guards, but the raw
 * per-offset vm_exec reference does not).  For such patterns the
 * equivalence check is limited to short subjects (<= 16 bytes) where the
 * reference terminates quickly. */
static bool oracle_reference_bounded(const char *src, size_t len) {
  for (size_t i = 0; i + 1 < len; i++) {
    if ((src[i] == '*' || src[i] == '+') && src[i + 1] == ')') {
      return true;
    }
  }
  return false;
}

static void oracle_check_pair(const char *name, const char *src, size_t src_len,
                              uint32_t flags, const char *subject,
                              size_t sub_len, int *checked, int *mismatch) {
  if (oracle_reference_bounded(src, src_len) && sub_len > 16) {
    return; /* reference would be exponential; skip the pair */
  }
  snobol_context_t *ctx = snobol_context_create();
  if (!ctx) {
    return;
  }
  snobol_pattern_t *pat = nullptr;
  const uint8_t *bc = nullptr;
  size_t bc_len = 0;
  const snobol_search_meta_t *meta = nullptr;
  const snobol_range_meta_t *range = nullptr;
  size_t range_count = 0;
  if (!oracle_compile(src, src_len, flags, ctx, &pat, &bc, &bc_len, &meta,
                      &range, &range_count)) {
    snobol_context_destroy(ctx);
    return;
  }

  oracle_ref_t ref;
  oracle_ref_run(bc, bc_len, range, range_count, subject, sub_len, &ref);

  /* --- snobol_pattern_search (tier dispatch) --- */
  snobol_match_t *m = snobol_pattern_search(pat, subject, sub_len);
  if (m) {
    (*checked)++;
    int mm = oracle_compare_match(m, &ref);
    if (mm > 0) {
      (*mismatch)++;
      printf("ORACLE MISMATCH search: %s subject=\"%.40s\" (%d fields)\n", name,
             subject, mm);
    }
    snobol_match_free(m);
  }

  /* --- stateful _ex + lean search_next --- */
  snobol_pattern_search_state_t *state =
      snobol_pattern_search_state_create(bc, bc_len);
  if (state) {
    snobol_pattern_search_state_set_pattern(state, pat);
    snobol_match_t *mx = snobol_pattern_search_ex(state, subject, sub_len, 0);
    if (mx) {
      (*checked)++;
      int mm = oracle_compare_match(mx, &ref);
      if (mm > 0) {
        (*mismatch)++;
        printf("ORACLE MISMATCH _ex: %s subject=\"%.40s\" (%d fields)\n", name,
               subject, mm);
      }
    }
    if (snobol_meta_is_literal_only(meta)) {
      size_t p = 0;
      size_t l = 0;
      bool ok = snobol_pattern_search_next(state, subject, sub_len, 0, &p, &l);
      (*checked)++;
      if (ok != ref.success) {
        (*mismatch)++;
        printf("ORACLE MISMATCH search_next success: %s\n", name);
      } else if (ok && (p != ref.pos || l != ref.end - ref.pos)) {
        (*mismatch)++;
        printf("ORACLE MISMATCH search_next pos/len: %s\n", name);
      }
    }
    snobol_pattern_search_state_destroy(state);
  }

  /* --- batch (compare only when batch-eligible) --- */
  snobol_batch_result_t br;
  memset(&br, 0, sizeof(br));
  bool bret =
      snobol_pattern_search_batch(bc, bc_len, subject, sub_len, meta, &br);
  if (br.eligible) {
    (*checked)++;
    if (ref.success) {
      if (!bret || br.match_count < 1 || br.positions[0] != ref.pos ||
          br.lengths[0] != ref.end - ref.pos) {
        (*mismatch)++;
        printf("ORACLE MISMATCH batch first match: %s\n", name);
      }
    } else if (bret || br.match_count != 0) {
      (*mismatch)++;
      printf("ORACLE MISMATCH batch zero-match: %s\n", name);
    }
  }
  snobol_batch_result_free(&br);

  snobol_pattern_free(pat);
  snobol_context_destroy(ctx);
}

/* ===========================================================================
 * Must-analysis: conservative forward dataflow over bytecode.
 *
 * Computes the intersection of literals matched on every entry→ACCEPT path.
 * Monotone fixpoint: must[offset] holds the set of literals guaranteed on
 * every analyzed path from entry to offset; joins are intersections, so a
 * literal only survives when every path through the join carries it.  The
 * result is a sound under-approximation, bounded by the bytecode size.
 * =========================================================================== */

#define MUST_MAX_LITS 256
#define MUST_TOP (~(uint64_t)0)

typedef struct {
  uint64_t bits[4]; /* 256-bit bitset */
} litset_t;

static void litset_clear(litset_t *s) {
  memset(s, 0, sizeof(*s));
}
static bool litset_empty(const litset_t *s) {
  return (s->bits[0] | s->bits[1] | s->bits[2] | s->bits[3]) == 0;
}
static void litset_set_all(litset_t *s) {
  s->bits[0] = s->bits[1] = s->bits[2] = s->bits[3] = MUST_TOP;
}
static void litset_add_id(litset_t *s, int id) {
  s->bits[id >> 6] |= 1ULL << (id & 63);
}
static void litset_intersect(litset_t *dst, const litset_t *a,
                             const litset_t *b) {
  for (int i = 0; i < 4; i++) {
    dst->bits[i] = a->bits[i] & b->bits[i];
  }
}

static uint32_t must_read_u32(const uint8_t *bc, size_t bc_len, size_t p) {
  if (p + 4 > bc_len) {
    return 0;
  }
  return ((uint32_t)bc[p] << 24) | ((uint32_t)bc[p + 1] << 16) |
         ((uint32_t)bc[p + 2] << 8) | (uint32_t)bc[p + 3];
}

typedef struct {
  int lit_count;
  size_t lit_off[MUST_MAX_LITS];
  size_t lit_len[MUST_MAX_LITS];
  bool gave_up;
} must_ctx_t;

static int must_lit_id(must_ctx_t *ctx, size_t off, size_t len) {
  for (int i = 0; i < ctx->lit_count; i++) {
    if (ctx->lit_off[i] == off && ctx->lit_len[i] == len) {
      return i;
    }
  }
  if (ctx->lit_count >= MUST_MAX_LITS) {
    return -1;
  }
  ctx->lit_off[ctx->lit_count] = off;
  ctx->lit_len[ctx->lit_count] = len;
  return ctx->lit_count++;
}

/* Look up a literal id by content (same bytes). */
static int must_find_lit(const must_ctx_t *ctx, const uint8_t *bc,
                         const uint8_t *lit, size_t lit_len) {
  for (int i = 0; i < ctx->lit_count; i++) {
    if (ctx->lit_len[i] == lit_len &&
        memcmp(bc + ctx->lit_off[i], lit, lit_len) == 0) {
      return i;
    }
  }
  return -1;
}

/* Edge kinds emitted by the transfer function. */
enum {
  MUST_EDGE_NONE = 0,   /* terminator: no outgoing edge */
  MUST_EDGE_LINEAR,     /* fall through: dst = p + advance */
  MUST_EDGE_JMP,        /* one explicit target */
  MUST_EDGE_SPLIT,      /* two explicit targets */
  MUST_EDGE_LINEAR_JMP, /* both the linear continuation and one explicit
                           target (repetition control flow) */
};

/* Apply the opcode at offset p to the incoming set.  Returns the outgoing
 * literal set and describes the control-flow edges. */
static void must_transfer(must_ctx_t *ctx, const uint8_t *bc, size_t bc_len,
                          size_t p, const litset_t *in, litset_t *out,
                          int *edge_kind, uint32_t *e1, uint32_t *e2,
                          size_t *advance) {
  *out = *in;
  *edge_kind = MUST_EDGE_NONE;
  *e1 = *e2 = 0;
  *advance = 0;
  if (p >= bc_len) {
    ctx->gave_up = true;
    return;
  }
  uint8_t op = bc[p];
  switch (op) {
    case OP_LIT: {
      if (p + 9 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      uint32_t off = must_read_u32(bc, bc_len, p + 1);
      uint32_t len = must_read_u32(bc, bc_len, p + 5);
      size_t payload = (off == p + 9) ? p + 9 : (size_t)off;
      if (payload + len > bc_len) {
        ctx->gave_up = true;
        return;
      }
      int id = must_lit_id(ctx, payload, len);
      if (id >= 0) {
        litset_add_id(out, id);
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = (off == p + 9) ? 9 + len : 9;
      return;
    }
    case OP_ACCEPT:
    case OP_SUCCEED:
    case OP_FAIL:
    case OP_ABORT: *edge_kind = MUST_EDGE_NONE; return;
    case OP_JMP:
      if (p + 5 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_JMP;
      *e1 = must_read_u32(bc, bc_len, p + 1);
      return;
    case OP_SPLIT:
      if (p + 9 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_SPLIT;
      *e1 = must_read_u32(bc, bc_len, p + 1);
      *e2 = must_read_u32(bc, bc_len, p + 5);
      return;
    case OP_REPEAT_INIT:
      if (p + 14 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      /* min==0: both the linear entry into the body and the skip edge
     * (zero iterations, a real accepting path).  min>=1: the skip edge is
     * a failure path, so only the linear continuation counts. */
      if (must_read_u32(bc, bc_len, p + 2) == 0) {
        *edge_kind = MUST_EDGE_LINEAR_JMP;
        *e1 = must_read_u32(bc, bc_len, p + 10);
        *advance = 14;
      } else {
        *edge_kind = MUST_EDGE_LINEAR;
        *advance = 14;
      }
      return;
    case OP_REPEAT_STEP:
      if (p + 6 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      /* Both the linear loop exit and the back edge into the body. */
      *edge_kind = MUST_EDGE_LINEAR_JMP;
      *e1 = must_read_u32(bc, bc_len, p + 2);
      *advance = 6;
      return;
    case OP_ANY:
    case OP_NOTANY:
    case OP_SPAN:
    case OP_BREAK:
    case OP_BREAKX:
    case OP_ANCHOR:
    case OP_CAP_START:
    case OP_CAP_END:
    case OP_EMIT_CAPTURE:
      if (p + 2 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = (op == OP_ANY || op == OP_NOTANY || op == OP_SPAN ||
                  op == OP_BREAK || op == OP_BREAKX)
                     ? 3
                     : 2;
      return;
    case OP_LEN:
    case OP_POS:
    case OP_RPOS:
    case OP_TAB:
    case OP_RTAB:
      if (p + 5 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = 5;
      return;
    case OP_BAL:
      if (p + 9 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = 9;
      return;
    case OP_ASSIGN:
    case OP_EVAL:
      if (p + 4 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = 4;
      return;
    case OP_EMIT_LITERAL:
      if (p + 9 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = 9;
      return;
    case OP_NOP:
    case OP_FENCE:
    case OP_REM:
      if (p + 1 > bc_len) {
        ctx->gave_up = true;
        return;
      }
      *edge_kind = MUST_EDGE_LINEAR;
      *advance = 1;
      return;
    default: ctx->gave_up = true; return;
  }
}

/* Run the must-analysis.  On success fills *out_accept with the
 * intersection of must-sets over all accepting paths and returns true. */
static bool must_analyze(const uint8_t *bc, size_t bc_len, must_ctx_t *ctx,
                         litset_t *out_accept) {
  memset(ctx, 0, sizeof(*ctx));
  if (bc_len == 0 || bc_len > 65536) {
    ctx->gave_up = true;
    return false;
  }
  litset_t *must = (litset_t *)calloc(bc_len, sizeof(litset_t));
  if (!must) {
    return false;
  }
  for (size_t i = 1; i < bc_len; i++) {
    litset_set_all(&must[i]);
  }
  litset_clear(&must[0]); /* entry: nothing guaranteed yet */

  /* Only instruction-start offsets are visited (operand bytes are not
   * instructions).  Entry + edge targets + linear successors form the
   * reachable set. */
  uint8_t *reachable = (uint8_t *)calloc(bc_len, 1);
  if (!reachable) {
    free(must);
    return false;
  }
  reachable[0] = 1;

  litset_t accept_set;
  litset_clear(&accept_set);
  bool any_accept = false;

  bool changed = true;
  while (changed && !ctx->gave_up) {
    changed = false;
    for (size_t p = 0; p < bc_len; p++) {
      if (!reachable[p]) {
        continue;
      }
      litset_t out;
      int kind;
      uint32_t e1;
      uint32_t e2;
      size_t adv;
      must_transfer(ctx, bc, bc_len, p, &must[p], &out, &kind, &e1, &e2, &adv);
      if (ctx->gave_up) {
        break;
      }
      if (kind == MUST_EDGE_NONE) {
        if (bc[p] == OP_ACCEPT || bc[p] == OP_SUCCEED) {
          if (!any_accept) {
            accept_set = out;
            any_accept = true;
          } else {
            litset_intersect(&accept_set, &accept_set, &out);
          }
        }
        continue;
      }
      /* Propagate to each destination with an intersection join. */
      for (int e = 0; e < 3; e++) {
        size_t dst = 0;
        bool have = false;
        if ((kind == MUST_EDGE_LINEAR || kind == MUST_EDGE_LINEAR_JMP) &&
            e == 0) {
          dst = p + adv;
          have = dst < bc_len;
        } else if (kind == MUST_EDGE_JMP && e == 0) {
          dst = e1;
          have = dst < bc_len;
        } else if (kind == MUST_EDGE_LINEAR_JMP && e == 1) {
          dst = e1;
          have = dst < bc_len;
        } else if (kind == MUST_EDGE_SPLIT && e < 2) {
          dst = e == 0 ? e1 : e2;
          have = dst < bc_len;
        }
        if (!have) {
          continue;
        }
        litset_t joined;
        litset_intersect(&joined, &must[dst], &out);
        if (memcmp(&joined, &must[dst], sizeof(litset_t)) != 0) {
          must[dst] = joined;
          reachable[dst] = 1;
          changed = true;
        } else {
          reachable[dst] = 1;
        }
      }
    }
  }

  bool ok = (!ctx->gave_up && any_accept) != 0;
  if (ok) {
    *out_accept = accept_set;
  }
  free(reachable);
  free(must);
  return ok;
}

/* ===========================================================================
 * Meta invariants (task 2.2).
 * =========================================================================== */

static int oracle_check_meta_invariants(const char *name, const uint8_t *bc,
                                        size_t bc_len,
                                        const snobol_search_meta_t *meta) {
  int fails = 0;

  /* 1. has_required_lit ⇒ required_lit ∈ must-set */
  if (snobol_meta_has_required_lit(meta) && meta->required_lit_len > 0) {
    must_ctx_t ctx;
    litset_t accept;
    if (must_analyze(bc, bc_len, &ctx, &accept)) {
      int id =
          must_find_lit(&ctx, bc, meta->required_lit, meta->required_lit_len);
      if (id < 0 || !(accept.bits[id >> 6] & (1ULL << (id & 63)))) {
        fails++;
        printf("  meta invariant FAIL (required-lit not on all paths): %s\n",
               name);
      }
    }
  }

  /* 2. leading SPLIT ⇒ no required literal */
  if (bc_len >= 1 && bc[0] == OP_SPLIT && snobol_meta_has_required_lit(meta)) {
    fails++;
    printf("  meta invariant FAIL (leading SPLIT derives required lit): %s\n",
           name);
  }

  /* 3. tier/eligibility consistency */
  if (snobol_meta_is_alt_literals(meta) && meta->tier != TIER_ALT_LIT) {
    fails++;
    printf("  meta invariant FAIL (is_alt_literals but tier=%u): %s\n",
           (unsigned)meta->tier, name);
  }
  if (meta->tier == TIER_ALT_LIT && !snobol_meta_is_alt_literals(meta)) {
    fails++;
    printf("  meta invariant FAIL (tier ALT_LIT without flag): %s\n", name);
  }
  return fails;
}

/* ===========================================================================
 * Seeded generator (task 3.1/3.2): LCG reused from test_property_based.c.
 * =========================================================================== */

static size_t gen_state = 0x9E3779B97F4A7C15ULL;

static unsigned char gen_byte(void) {
  gen_state = (gen_state * 6364136223846793005ULL) + 1442695040888963407ULL;
  return (unsigned char)(gen_state >> 32);
}

static size_t gen_bounded(size_t n) {
  return gen_byte() % n;
}

#define GEN_BUF_CAP 4096

static size_t gen_literal(char *buf, size_t cap, size_t max_len) {
  size_t len = gen_bounded(max_len + 1);
  size_t off = 0;
  buf[off++] = '\'';
  for (size_t i = 0; i < len; i++) {
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyz0123456789 ,;.-+*/()=<>_";
    if (off + 1 >= cap) {
      break;
    }
    buf[off++] = alphabet[gen_bounded(sizeof(alphabet) - 1)];
  }
  buf[off++] = '\'';
  buf[off] = '\0';
  return off;
}

static size_t gen_pattern_fragment(char *buf, size_t cap, int kind, int depth) {
  if (depth <= 0) {
    kind = 0;
  }
  switch (kind) {
    case 0: return gen_literal(buf, cap, 6);
    case 1: {
      static const char *const classes[] = {"SPAN('a-z')", "SPAN('0-9')",
                                            "BREAK(' ,;')", "ANY('abc')"};
      const char *c = classes[gen_bounded(4)];
      size_t l = strlen(c);
      if (l >= cap) {
        l = cap - 1;
      }
      memcpy(buf, c, l);
      buf[l] = '\0';
      return l;
    }
    case 2: {
      int n = 2 + (int)gen_bounded(5);
      size_t off = 0;
      for (int i = 0; i < n; i++) {
        if (i) {
          off += (size_t)snprintf(buf + off, cap - off, " | ");
        }
        off += gen_literal(buf + off, cap - off, 4);
      }
      return off;
    }
    case 3: {
      size_t off = 0;
      if (off + 2 < cap) {
        buf[off++] = '(';
        off += gen_pattern_fragment(buf + off, cap - off, 0, depth - 1);
        buf[off++] = ')';
        buf[off++] = gen_byte() & 1 ? '+' : '*';
        buf[off] = '\0';
      }
      return off;
    }
    case 4: {
      /* capture: must lead the pattern (grammar: @IDENT prefix) */
      size_t off = (size_t)snprintf(buf, cap, "@v1 ");
      off += gen_pattern_fragment(buf + off, cap - off, 0, depth - 1);
      return off;
    }
    case 5: {
      /* anchor: "^ " prefix, none, or " $" suffix */
      int mode = (int)gen_bounded(3);
      size_t off = 0;
      if (mode == 0) {
        off = (size_t)snprintf(buf, cap, "^ ");
      }
      off += gen_pattern_fragment(buf + off, cap - off, 0, depth - 1);
      if (mode == 2) {
        if (off + 3 < cap) {
          snprintf(buf + off, cap - off, " $");
          off += 2;
        }
      }
      return off;
    }
    default: return gen_literal(buf, cap, 6);
  }
}

static size_t gen_pattern(char *buf, size_t cap) {
  int nparts = 1 + (int)gen_bounded(3);
  size_t off = 0;
  for (int i = 0; i < nparts; i++) {
    int kind = (int)gen_bounded(6);
    if (kind == 4 && i > 0) {
      kind = 0; /* captures must lead the pattern */
    }
    off += gen_pattern_fragment(buf + off, cap - off, kind, 3);
  }
  return off;
}

static size_t gen_subject(char *buf, size_t cap) {
  size_t len = 1 + gen_bounded(48);
  if (len >= cap) {
    len = cap - 1;
  }
  static const char *const tokens[] = {"a",  "ab", "abc",   "x",   "123",
                                       "  ", ",",  "κόσμε", "cat", "dog"};
  size_t off = 0;
  while (off < len) {
    const char *t = tokens[gen_bounded(10)];
    size_t tl = strlen(t);
    if (off + tl > len) {
      break;
    }
    memcpy(buf + off, t, tl);
    off += tl;
    if (off < len) {
      buf[off++] = ' ';
    }
  }
  buf[off] = '\0';
  return off;
}

/* ===========================================================================
 * Suites
 * =========================================================================== */

void test_search_oracle_suite(void) {
  test_suite("Search Oracle: corpus equivalence");

  char big_alt[ORACLE_ALT_BUF_CAP];
  char marker_alt[ORACLE_ALT_BUF_CAP];
  size_t marker_len = oracle_build_marker_alt(marker_alt, sizeof(marker_alt));
  size_t big30_len = oracle_build_big_alt(big_alt, sizeof(big_alt), 30, 20);
  size_t big70_len = oracle_build_big_alt(big_alt, sizeof(big_alt), 70, 20);
  test_assert((marker_len > 0 && big30_len > 0 && big70_len > 0) != 0,
              "corpus builders produce patterns");

  int checked = 0;
  int mismatch = 0;

  for (size_t i = 0; i < oracle_corpus_count; i++) {
    const oracle_corpus_entry_t *e = &oracle_corpus[i];
    for (size_t j = 0; j < oracle_subject_count; j++) {
      oracle_check_pair(e->name, e->pattern, strlen(e->pattern), e->flags,
                        oracle_subjects[j], strlen(oracle_subjects[j]),
                        &checked, &mismatch);
    }
  }

  /* Dynamic corpus entries (large alternations). */
  oracle_check_pair("marker-alt-82", marker_alt, marker_len, 0,
                    "v1.0.2 v1.0.3 core/v1.0.2", 24, &checked, &mismatch);
  oracle_check_pair("marker-alt-82", marker_alt, marker_len, 0,
                    "no markers here", 15, &checked, &mismatch);
  oracle_check_pair("marker-alt-82", marker_alt, marker_len, 0, "v1.8.1", 6,
                    &checked, &mismatch);
  oracle_check_pair("big-alt-30", big_alt, big30_len, 0, "mmmmmmmmmmmmmmmmmmmm",
                    20, &checked, &mismatch);
  oracle_check_pair("big-alt-30", big_alt, big30_len, 0, "jjjjjjjjjjjjjjjjjjjj",
                    20, &checked, &mismatch);
  oracle_check_pair("big-alt-70", big_alt, big70_len, 0, "mmmmmmmmmmmmmmmmmmmm",
                    20, &checked, &mismatch);
  oracle_check_pair("big-alt-70", big_alt, big70_len, 0, "no match here", 13,
                    &checked, &mismatch);

  test_assert(mismatch == 0, "no tier-dispatch vs reference disagreements");
  test_assert(checked > 1000, "corpus equivalence checks ran");
}

void test_search_oracle_meta_suite(void) {
  test_suite("Search Oracle: meta invariants");

  char big_alt[ORACLE_ALT_BUF_CAP];
  char marker_alt[ORACLE_ALT_BUF_CAP];
  size_t marker_len = oracle_build_marker_alt(marker_alt, sizeof(marker_alt));
  size_t big70_len = oracle_build_big_alt(big_alt, sizeof(big_alt), 70, 20);
  size_t big30_len = oracle_build_big_alt(big_alt, sizeof(big_alt), 30, 20);

  int fails = 0;
  int meta_checked = 0;

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  for (size_t i = 0; i < oracle_corpus_count; i++) {
    const oracle_corpus_entry_t *e = &oracle_corpus[i];
    snobol_pattern_t *pat = nullptr;
    const uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const snobol_search_meta_t *meta = nullptr;
    const snobol_range_meta_t *range = nullptr;
    size_t range_count = 0;
    if (oracle_compile(e->pattern, strlen(e->pattern), e->flags, ctx, &pat, &bc,
                       &bc_len, &meta, &range, &range_count)) {
      fails += oracle_check_meta_invariants(e->name, bc, bc_len, meta);
      meta_checked++;
      snobol_pattern_free(pat);
    }
  }

  {
    const struct {
      const char *name;
      const char *src;
      size_t len;
    } dyn[] = {
        {"marker-alt-82", marker_alt, marker_len},
        {"big-alt-30", big_alt, big30_len},
        {"big-alt-70", big_alt, big70_len},
    };
    for (size_t i = 0; i < sizeof(dyn) / sizeof(dyn[0]); i++) {
      snobol_pattern_t *pat = nullptr;
      const uint8_t *bc = nullptr;
      size_t bc_len = 0;
      const snobol_search_meta_t *meta = nullptr;
      const snobol_range_meta_t *range = nullptr;
      size_t range_count = 0;
      if (oracle_compile(dyn[i].src, dyn[i].len, 0, ctx, &pat, &bc, &bc_len,
                         &meta, &range, &range_count)) {
        fails += oracle_check_meta_invariants(dyn[i].name, bc, bc_len, meta);
        meta_checked++;
        /* The >2048-byte alternations must NOT be flagged alt-literals:
         * the derive_meta walk is bounded to the first 2048 bytes. */
        if ((strcmp(dyn[i].name, "marker-alt-82") == 0 ||
             strcmp(dyn[i].name, "big-alt-70") == 0) &&
            snobol_meta_is_alt_literals(meta)) {
          fails++;
          printf("  meta invariant FAIL (over-bound alt marked alt-lit): %s\n",
                 dyn[i].name);
        }
        snobol_pattern_free(pat);
      }
    }
  }

  snobol_context_destroy(ctx);
  test_assert(fails == 0, "search meta invariants hold for the corpus");
  test_assert(meta_checked >= 60, "meta invariants checked for >= 60 patterns");

  test_must_analysis_walker();
  test_prefilter_loop_soundness();
  test_trie_pool_fallback();
}

/* Unit assertions for the must-analysis walker (task 2.3): positive and
 * negative bytecode shapes, hand-built so the helper is block-covered. */
/* Direct test for the trie-pool-overflow fallback (task 4.1): a crafted
 * VALID alternation whose literal bytes exceed the pool, driven through
 * the forced TIER_ALT_LIT tier — the tier must fall back to the general
 * VM and still match. */
static void test_trie_pool_fallback(void) {
  test_suite("Search Oracle: trie pool fallback");

  /* SPLIT(a=9, b=327): a = LIT(300 bytes) ACCEPT, b = LIT('x') ACCEPT. */
  uint8_t bc[512];
  size_t ip = 0;
  bc[ip++] = OP_SPLIT;
  uint32_t tgt_a = 9;
  uint32_t tgt_b = (uint32_t)(9 + 9 + 300 + 1);
  bc[ip++] = (uint8_t)(tgt_a >> 24);
  bc[ip++] = (uint8_t)(tgt_a >> 16);
  bc[ip++] = (uint8_t)(tgt_a >> 8);
  bc[ip++] = (uint8_t)tgt_a;
  bc[ip++] = (uint8_t)(tgt_b >> 24);
  bc[ip++] = (uint8_t)(tgt_b >> 16);
  bc[ip++] = (uint8_t)(tgt_b >> 8);
  bc[ip++] = (uint8_t)tgt_b;
  bc[ip++] = OP_LIT;
  uint32_t off = (uint32_t)(ip + 8);
  bc[ip++] = (uint8_t)(off >> 24);
  bc[ip++] = (uint8_t)(off >> 16);
  bc[ip++] = (uint8_t)(off >> 8);
  bc[ip++] = (uint8_t)off;
  bc[ip++] = 0;
  bc[ip++] = 0;
  bc[ip++] = 1;
  bc[ip++] = 44; /* len = 300 */
  for (int i = 0; i < 300; i++) {
    bc[ip++] = (uint8_t)('a' + (i % 26));
  }
  bc[ip++] = OP_ACCEPT;
  bc[ip++] = OP_LIT;
  off = (uint32_t)(ip + 8);
  bc[ip++] = (uint8_t)(off >> 24);
  bc[ip++] = (uint8_t)(off >> 16);
  bc[ip++] = (uint8_t)(off >> 8);
  bc[ip++] = (uint8_t)off;
  bc[ip++] = 0;
  bc[ip++] = 0;
  bc[ip++] = 0;
  bc[ip++] = 1;
  bc[ip++] = 'x';
  bc[ip++] = OP_ACCEPT;
  size_t bc_len = ip;
  test_assert(bc_len > 300, "crafted over-budget alternation built");

  snobol_search_meta_t meta;
  memset(&meta, 0, sizeof(meta));
  meta.is_alt_literals = true; /* force the trie tier despite the budget */
  meta.tier = TIER_ALT_LIT;

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.bc = bc;
  vm.bc_len = bc_len;
  snobol_search_result_t result;
  memset(&result, 0, sizeof(result));
  bool ok =
      snobol_search_exec(&vm, "x", 1, 0, &meta, nullptr, &result, nullptr);
  snobol_search_vm_cleanup(&vm);
  test_assert((ok && result.match_start == 0 && result.match_end == 1) != 0,
              "pool-overflow tier falls back and matches");
}

/* Regression tests for the Class D prefilter fix: repetition control flow
 * must bypass the required literal the same way alternation SPLITs do. */
static void test_prefilter_loop_soundness(void) {
  test_suite("Search Oracle: prefilter loop soundness");

  snobol_context_t *ctx = snobol_context_create();
  test_assert(ctx != NULL, "context created");

  struct {
    const char *name;
    const char *pat;
    bool expect_required;
  } cases[] = {
      /* min==0 loops: the zero-iteration skip edge bypasses the body.
       * `+` compiles to a leading literal + a min==0 loop, so the loop
       * copy is bypassable and no required literal is derived. */
      {"loop-star", "('ab')*", false},
      {"loop-body-alt", "('a' | 'b')*", false},
      {"loop-empty-body", "('')*", false},
      {"loop-plus", "('ab')+", false},
      /* A literal AFTER the loop is still on every accepting path. */
      {"loop-greedy-tail", "('a')* 'b'", true},
      {"loop-double-star", "('a'*)* 'b'", true},
      {"loop-plus-tail", "('a'+)+ 'b'", true},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    snobol_pattern_t *pat = nullptr;
    const uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const snobol_search_meta_t *meta = nullptr;
    const snobol_range_meta_t *range = nullptr;
    size_t range_count = 0;
    if (!oracle_compile(cases[i].pat, strlen(cases[i].pat), 0, ctx, &pat, &bc,
                        &bc_len, &meta, &range, &range_count)) {
      continue;
    }
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: has_required_lit == %s", cases[i].name,
             cases[i].expect_required ? "true" : "false");
    test_assert(snobol_meta_has_required_lit(meta) ==
                    (int)cases[i].expect_required,
                msg);
    if (snobol_meta_has_required_lit(meta) && meta->required_lit_len > 0) {
      /* The derived required literal must be on every accepting path. */
      must_ctx_t mctx;
      litset_t accept;
      if (must_analyze(bc, bc_len, &mctx, &accept)) {
        int id = must_find_lit(&mctx, bc, meta->required_lit,
                               meta->required_lit_len);
        snprintf(msg, sizeof(msg), "%s: required lit in must-set",
                 cases[i].name);
        test_assert((id >= 0 && (accept.bits[id >> 6] & (1ULL << (id & 63)))) !=
                        0,
                    msg);
      }
    }
    snobol_pattern_free(pat);
  }

  snobol_context_destroy(ctx);
}

static void test_must_analysis_walker(void) {
  test_suite("Search Oracle: must-analysis walker");

  /* Positive: LIT("abc") LIT("def") ACCEPT — both literals must. */
  {
    uint8_t bc[256];
    size_t p = 0;
    uint32_t off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(off >> 24);
    bc[p++] = (uint8_t)(off >> 16);
    bc[p++] = (uint8_t)(off >> 8);
    bc[p++] = (uint8_t)off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "abc", 3);
    p += 3;
    off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(off >> 24);
    bc[p++] = (uint8_t)(off >> 16);
    bc[p++] = (uint8_t)(off >> 8);
    bc[p++] = (uint8_t)off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "def", 3);
    p += 3;
    bc[p++] = OP_ACCEPT;

    must_ctx_t ctx;
    litset_t accept;
    bool ok = must_analyze(bc, p, &ctx, &accept);
    test_assert(ok, "must-analysis completes on linear chain");
    int id_abc = must_find_lit(&ctx, bc, (const uint8_t *)"abc", 3);
    int id_def = must_find_lit(&ctx, bc, (const uint8_t *)"def", 3);
    test_assert((id_abc >= 0 &&
                 (accept.bits[id_abc >> 6] & (1ULL << (id_abc & 63)))) != 0,
                "abc is required on the linear chain");
    test_assert((id_def >= 0 &&
                 (accept.bits[id_def >> 6] & (1ULL << (id_def & 63)))) != 0,
                "def is required on the linear chain");
  }

  /* Negative: SPLIT → (LIT("abc") ACCEPT) | (LIT("xyz") ACCEPT) — the
   * entry is an alternation, so NO literal is on every accepting path. */
  {
    uint8_t bc[256];
    size_t p = 0;
    size_t split_at = p;
    bc[p++] = OP_SPLIT;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    uint32_t lit1_off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(lit1_off >> 24);
    bc[p++] = (uint8_t)(lit1_off >> 16);
    bc[p++] = (uint8_t)(lit1_off >> 8);
    bc[p++] = (uint8_t)lit1_off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "abc", 3);
    p += 3;
    size_t accept1 = p;
    bc[p++] = OP_ACCEPT;
    uint32_t lit2_off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(lit2_off >> 24);
    bc[p++] = (uint8_t)(lit2_off >> 16);
    bc[p++] = (uint8_t)(lit2_off >> 8);
    bc[p++] = (uint8_t)lit2_off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "xyz", 3);
    p += 3;
    bc[p++] = OP_ACCEPT;
    uint32_t tgt_a = (uint32_t)(split_at + 9);
    uint32_t tgt_b = (uint32_t)(accept1 + 1);
    bc[split_at + 1] = (uint8_t)(tgt_a >> 24);
    bc[split_at + 2] = (uint8_t)(tgt_a >> 16);
    bc[split_at + 3] = (uint8_t)(tgt_a >> 8);
    bc[split_at + 4] = (uint8_t)tgt_a;
    bc[split_at + 5] = (uint8_t)(tgt_b >> 24);
    bc[split_at + 6] = (uint8_t)(tgt_b >> 16);
    bc[split_at + 7] = (uint8_t)(tgt_b >> 8);
    bc[split_at + 8] = (uint8_t)tgt_b;

    must_ctx_t ctx;
    litset_t accept;
    bool ok = must_analyze(bc, p, &ctx, &accept);
    test_assert(ok, "must-analysis completes on alternation");
    test_assert(litset_empty(&accept),
                "no literal is required across an entry alternation");
  }

  /* Negative: LIT("abc") SPLIT → (ACCEPT) | (LIT("xyz") ACCEPT) — "abc"
   * is on every path, "xyz" is not. */
  {
    uint8_t bc[256];
    size_t p = 0;
    uint32_t lit1_off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(lit1_off >> 24);
    bc[p++] = (uint8_t)(lit1_off >> 16);
    bc[p++] = (uint8_t)(lit1_off >> 8);
    bc[p++] = (uint8_t)lit1_off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "abc", 3);
    p += 3;
    size_t split_at = p;
    bc[p++] = OP_SPLIT;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    size_t accept_a = p;
    bc[p++] = OP_ACCEPT;
    uint32_t lit2_off = (uint32_t)(p + 9);
    bc[p++] = OP_LIT;
    bc[p++] = (uint8_t)(lit2_off >> 24);
    bc[p++] = (uint8_t)(lit2_off >> 16);
    bc[p++] = (uint8_t)(lit2_off >> 8);
    bc[p++] = (uint8_t)lit2_off;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 0;
    bc[p++] = 3;
    memcpy(bc + p, "xyz", 3);
    p += 3;
    bc[p++] = OP_ACCEPT;
    uint32_t tgt_a = (uint32_t)accept_a;
    uint32_t tgt_b = (uint32_t)(accept_a + 1);
    bc[split_at + 1] = (uint8_t)(tgt_a >> 24);
    bc[split_at + 2] = (uint8_t)(tgt_a >> 16);
    bc[split_at + 3] = (uint8_t)(tgt_a >> 8);
    bc[split_at + 4] = (uint8_t)tgt_a;
    bc[split_at + 5] = (uint8_t)(tgt_b >> 24);
    bc[split_at + 6] = (uint8_t)(tgt_b >> 16);
    bc[split_at + 7] = (uint8_t)(tgt_b >> 8);
    bc[split_at + 8] = (uint8_t)tgt_b;

    must_ctx_t ctx;
    litset_t accept;
    bool ok = must_analyze(bc, p, &ctx, &accept);
    test_assert(ok, "must-analysis completes on post-literal split");
    int id_abc = must_find_lit(&ctx, bc, (const uint8_t *)"abc", 3);
    int id_xyz = must_find_lit(&ctx, bc, (const uint8_t *)"xyz", 3);
    test_assert((id_abc >= 0 &&
                 (accept.bits[id_abc >> 6] & (1ULL << (id_abc & 63)))) != 0,
                "abc survives the join (on every accepting path)");
    test_assert((id_xyz >= 0 &&
                 !(accept.bits[id_xyz >> 6] & (1ULL << (id_xyz & 63)))) != 0,
                "xyz does not survive the join");
  }
}

void test_search_oracle_generator_suite(void) {
  test_suite("Search Oracle: generated patterns");

  char big_alt[ORACLE_ALT_BUF_CAP];
  size_t big70_len = oracle_build_big_alt(big_alt, sizeof(big_alt), 70, 20);

  gen_state = 0x9E3779B97F4A7C15ULL; /* fixed seed — deterministic CI */

  char pat_buf[GEN_BUF_CAP];
  char sub_buf[512];
  int checked = 0;
  int mismatch = 0;
  int lits = 0;
  int classes = 0;
  int alts = 0;
  int repeats = 0;
  int captures = 0;
  int anchored = 0;

  /* Shape 0: force a >2048-byte leading alternation (the failing shape). */
  {
    const char *subjects[] = {"mmmmmmmmmmmmmmmmmmmm", "zzzzzzzzzzzzzzzzzzzz",
                              "qqqqqqqqqqqqqqqqqqqq"};
    for (size_t j = 0; j < 3; j++) {
      oracle_check_pair("gen-big-alt", big_alt, big70_len, 0, subjects[j],
                        strlen(subjects[j]), &checked, &mismatch);
    }
    snobol_context_t *ctx = snobol_context_create();
    snobol_pattern_t *pat = nullptr;
    const uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const snobol_search_meta_t *meta = nullptr;
    const snobol_range_meta_t *range = nullptr;
    size_t range_count = 0;
    if (oracle_compile(big_alt, big70_len, 0, ctx, &pat, &bc, &bc_len, &meta,
                       &range, &range_count)) {
      test_assert(bc_len > 2048, "big alternation bytecode exceeds 2048");
      test_assert((!snobol_meta_has_required_lit(meta)) != 0,
                  "over-bound leading alternation derives no required lit");
      snobol_pattern_free(pat);
    }
    snobol_context_destroy(ctx);
  }

  for (int trial = 0; trial < 60; trial++) {
    size_t plen = gen_pattern(pat_buf, sizeof(pat_buf));
    if (plen == 0) {
      continue;
    }
    size_t slen = gen_subject(sub_buf, sizeof(sub_buf));

    if (strchr(pat_buf, '\'')) {
      lits++;
    }
    if (strstr(pat_buf, "SPAN") || strstr(pat_buf, "BREAK") ||
        strstr(pat_buf, "ANY")) {
      classes++;
    }
    if (strstr(pat_buf, " | ")) {
      alts++;
    }
    if (strchr(pat_buf, '+') || strchr(pat_buf, '*')) {
      repeats++;
    }
    if (strstr(pat_buf, "@v1")) {
      captures++;
    }
    if (strchr(pat_buf, '^') || strchr(pat_buf, '$')) {
      anchored++;
    }

    oracle_check_pair("gen", pat_buf, plen, 0, sub_buf, slen, &checked,
                      &mismatch);
  }

  test_assert(mismatch == 0, "no disagreements on generated patterns");
  test_assert(checked > 100, "generated equivalence checks ran");
  test_assert((lits > 0 && classes > 0 && alts > 0 && repeats > 0 &&
               captures > 0 && anchored > 0) != 0,
              "generator covers every grammar production");
}
