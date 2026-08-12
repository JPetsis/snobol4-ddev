#include "compiler_internal.h"
#include "snobol/snobol_internal.h"
#include <stddef.h>
#include <stdint.h>
#include "snobol/ast.h"
#include <stdlib.h>
#include <stdio.h>
#include "snobol/vm.h"
#include "snobol/unicode_fold.h"
#include <string.h>

/**
 * Initialize a bytecode output buffer.
 *
 * Allocates the initial 4096-byte buffer; the buffer grows geometrically
 * via cb_ensure() as instructions are emitted.  The caller owns the buffer
 * and must release it with cb_free().
 *
 * @param c CodeBuf to initialize (must not be NULL)
 */
void cb_init(CodeBuf *c) {
  c->cap = 4096;
  c->buf = snobol_malloc(c->cap);
  c->len = 0;
}
/**
 * Release a bytecode output buffer.
 *
 * Frees the buffer and zeroes all fields.  NULL-safe and idempotent.
 *
 * @param c CodeBuf to release (may be NULL)
 */
void cb_free(CodeBuf *c) {
  if (c->buf) {
    snobol_free(c->buf);
    c->buf = nullptr;
  }
  c->cap = c->len = 0;
}
void cb_ensure(CodeBuf *c, size_t need) {
  if (c->len + need <= c->cap) {
    return;
  }
  size_t newcap = c->cap ? c->cap * 2 : 4096;
  while (c->len + need > newcap) {
    newcap *= 2;
  }
  uint8_t *nb = snobol_realloc(c->buf, newcap);
  if (!nb) {
    return; /* OOM: keep the old buffer */
  }
  c->buf = nb;
  c->cap = newcap;
}
/**
 * Return the current write position of a bytecode buffer.
 *
 * The position equals the number of bytes emitted so far.
 *
 * @param c CodeBuf to query (must not be NULL)
 * @return Current length of the emitted bytecode
 */
size_t cb_pos(CodeBuf *c) {
  return c->len;
}
void cb_emit_u8(CodeBuf *c, uint8_t v) {
  cb_ensure(c, 1);
  c->buf[c->len++] = v;
}
void cb_emit_u16(CodeBuf *c, uint16_t v) {
  cb_ensure(c, 2);
  c->buf[c->len++] = (v >> 8) & 0xff;
  c->buf[c->len++] = v & 0xff;
}
void cb_emit_u32(CodeBuf *c, uint32_t v) {
  cb_ensure(c, 4);
  c->buf[c->len++] = (v >> 24) & 0xff;
  c->buf[c->len++] = (v >> 16) & 0xff;
  c->buf[c->len++] = (v >> 8) & 0xff;
  c->buf[c->len++] = v & 0xff;
}
void cb_emit_bytes(CodeBuf *c, const uint8_t *b, size_t n) {
  if (n == 0) {
    return;
  }
  cb_ensure(c, n);
  memcpy(c->buf + c->len, b, n);
  c->len += n;
}

/* Charclass table handling */
CCEntry *charclass_head = nullptr;
uint32_t charclass_count = 0;
uint8_t next_loop_id = 0;
bool compiler_case_insensitive = false;

/* ── Nullable (empty-matchable) analysis for zero-width-loop bounding (W2b) ──
 *
 * A sub-pattern is "nullable" if it can match the empty string. arbno/ARB over
 * a nullable sub-pattern can create zero-width closures; detecting this lets
 * the compiler warn (and the VM bound iterations) to avoid O(n^2)/exponential
 * choice-point blowup. */

/**
 * Determine whether an AST subtree can match the empty string.
 *
 * A subtree is nullable when it can complete a match without consuming
 * any subject bytes: literal nodes only when empty, concat only when every
 * part is nullable, alternation when either branch is nullable, and
 * zero-width structural nodes (ARBNO, repeat(0,…), SUCCEED, FAIL, FENCE,
 * REM, BREAKX, BAL) are always nullable.  The compiler uses this to warn
 * about (and bound) zero-width closures that could otherwise produce
 * exponential choice-point blowup.
 *
 * @param node AST subtree to analyse, or NULL
 * @return true if the subtree can match empty (NULL input → false)
 */
bool ast_node_nullable(const ast_node_t *node) {
  if (!node) {
    return false;
  }
  switch (node->type) {
    case AST_ARBNO:
    case AST_REPETITION:
      /* arbno / repeat(0, …) can match zero times → nullable. */
      if (node->type == AST_ARBNO) {
        return true;
      }
      return node->data.repetition.min == 0;
    case AST_ALT:
      /* nullable if either branch can match empty. */
      return (ast_node_nullable(node->data.alt.left) ||
              ast_node_nullable(node->data.alt.right)) != 0;
    case AST_CONCAT:
      /* nullable only if EVERY part is nullable. */
      for (size_t i = 0; i < node->data.concat.count; i++) {
        if (!ast_node_nullable(node->data.concat.parts[i])) {
          return false;
        }
      }
      return node->data.concat.count == 0;
    case AST_CAP:
    case AST_EMIT: return ast_node_nullable(node->data.cap.sub);
    case AST_LITERAL:
      /* A literal only matches empty if it is the empty string. */
      return node->data.literal.len == 0;
    case AST_SUCCEED:
    case AST_FAIL:
    case AST_FENCE:
    case AST_REM:
    case AST_BREAKX:
    case AST_BAL:
      /* Zero-width structural nodes that match (or fail) without consuming. */
      return true;
    default:
      /* SPAN/ANY/NOTANY/LEN/POS/TAB/RPOS/RTAB/BAL/GOTO/LABEL/EVAL/etc. cannot
     * match empty (POS/TAB require a specific cursor position). */
      return false;
  }
}

/**
 * Emit a compiler diagnostic to stderr when enabled.
 *
 * Diagnostics are gated by the SNOBOL_DIAG environment variable (any
 * non-empty value enables them); the flag is probed once on first use.
 * Used by the compiler to surface warnings such as unbounded nullable
 * closures.
 *
 * @param msg Message to print (NULL prints an empty line)
 */
void snobol_diag(const char *msg) {
  static int diag_enabled = -1;
  if (diag_enabled < 0) {
    diag_enabled = (getenv("SNOBOL_DIAG") != nullptr) ? 1 : 0;
  }
  if (diag_enabled) {
    fprintf(stderr, "[snobol-diag] %s\n", msg ? msg : "");
  }
}

/**
 * Release the compiler's global character-class registry.
 *
 * Walks the singly-linked list of CCEntry nodes (see add_range /
 * get_cc_entry), freeing each entry and its range array, then resets the
 * list head, count, and the case-insensitivity flag.  Called at the end
 * of a compile so per-pattern character classes do not leak or leak
 * across compiles.
 */
void free_charclass_list(void) {
  CCEntry *e = charclass_head;
  while (e) {
    CCEntry *next = e->next;
    if (e->ranges) {
      snobol_free(e->ranges);
    }
    snobol_free(e);
    e = next;
  }
  charclass_head = nullptr;
  charclass_count = 0;
  compiler_case_insensitive = false;
}

void add_range(CCEntry *e, uint32_t start, uint32_t end) {
  if (e->range_count == e->range_cap) {
    e->range_cap = e->range_cap ? e->range_cap * 2 : 4;
    CpRange *nr = snobol_realloc(e->ranges, e->range_cap * sizeof(CpRange));
    if (!nr) {
      return; /* OOM: keep the old ranges */
    }
    e->ranges = nr;
  }
  e->ranges[e->range_count].start = start;
  e->ranges[e->range_count].end = end;
  e->range_count++;
}

int compare_ranges(const void *a, const void *b) {
  const CpRange *ra = (const CpRange *)a;
  const CpRange *rb = (const CpRange *)b;
  if (ra->start < rb->start) {
    return -1;
  }
  if (ra->start > rb->start) {
    return 1;
  }
  return 0;
}

/**
 * Sort and merge the range list of a character-class entry.
 *
 * Orders the entry's ranges by start offset and merges adjacent or
 * overlapping ranges (gap of one codepoint included) into a canonical,
 * disjoint, sorted sequence — the representation the rest of the
 * pipeline (bitmap caching, case folding, range lookups) relies on.
 *
 * @param e Character-class entry whose ranges are normalized in place
 */
void normalize_ranges(CCEntry *e) {
  if (e->range_count == 0) {
    return;
  }
  qsort(e->ranges, e->range_count, sizeof(CpRange), compare_ranges);

  size_t write = 0;
  for (size_t read = 1; read < e->range_count; ++read) {
    if (e->ranges[read].start <= e->ranges[write].end + 1) {
      if (e->ranges[read].end > e->ranges[write].end) {
        e->ranges[write].end = e->ranges[read].end;
      }
    } else {
      write++;
      e->ranges[write] = e->ranges[read];
    }
  }
  e->range_count = (uint16_t)(write + 1);
}

int add_or_get_charclass(const char *s, size_t len) {
  CCEntry *ne = snobol_malloc(sizeof(*ne));
  memset(ne, 0, sizeof(*ne));
  ne->case_insensitive = (int)compiler_case_insensitive ? 1 : 0;

  size_t pos = 0;
  while (pos < len) {
    uint32_t cp;
    int cp_bytes;
    if (!utf8_peek_next(s, len, pos, &cp, &cp_bytes)) {
      break;
    }

    /* Check for range notation: X-Y where X < Y */
    uint32_t dash_cp;
    int dash_bytes;
    uint32_t end_cp;
    int end_bytes;
    if (utf8_peek_next(s, len, pos + cp_bytes, &dash_cp, &dash_bytes) &&
        dash_cp == '-' &&
        utf8_peek_next(s, len, pos + cp_bytes + dash_bytes, &end_cp,
                       &end_bytes) &&
        end_cp > cp) {
      /* Expand X-Y range */
      add_range(ne, cp, end_cp);
      if (compiler_case_insensitive) {
        if (cp >= 'A' && end_cp <= 'Z') {
          add_range(ne, cp + 32, end_cp + 32);
        } else if (cp >= 'a' && end_cp <= 'z') {
          add_range(ne, cp - 32, end_cp - 32);
        } else if (end_cp > 0x7F) {
          for (uint32_t c = cp; c <= end_cp; c++) {
            uint32_t up[2];
            int ulen;
            snobol_to_upper_cp(c, up, &ulen);
            if (up[0] != c) {
              add_range(ne, up[0], up[0]);
              if (ulen > 1) {
                add_range(ne, up[1], up[1]);
              }
            }
            uint32_t lo = snobol_to_lower_cp(c);
            if (lo != c) {
              add_range(ne, lo, lo);
            }
          }
        }
      }
      pos += cp_bytes + dash_bytes + end_bytes;
      continue;
    }

    /* Single codepoint */
    add_range(ne, cp, cp);
    if (compiler_case_insensitive) {
      if (cp >= 'A' && cp <= 'Z') {
        add_range(ne, cp + 32, cp + 32);
      } else if (cp >= 'a' && cp <= 'z') {
        add_range(ne, cp - 32, cp - 32);
      } else if (cp > 0x7F) {
        uint32_t up[2];
        int ulen;
        snobol_to_upper_cp(cp, up, &ulen);
        if (up[0] != cp) {
          add_range(ne, up[0], up[0]);
          if (ulen > 1) {
            add_range(ne, up[1], up[1]);
          }
        }
        uint32_t lo = snobol_to_lower_cp(cp);
        if (lo != cp) {
          add_range(ne, lo, lo);
        }
      }
    }

    pos += cp_bytes;
  }
  normalize_ranges(ne);

  CCEntry *e = charclass_head;
  int id = 1;
  while (e) {
    if (e->range_count == ne->range_count &&
        e->case_insensitive == ne->case_insensitive &&
        memcmp(e->ranges, ne->ranges, e->range_count * sizeof(CpRange)) == 0) {
      if (ne->ranges) {
        snobol_free(ne->ranges);
      }
      snobol_free(ne);
      return id;
    }
    id++;
    e = e->next;
  }

  ne->next = nullptr;
  if (!charclass_head) {
    charclass_head = ne;
  } else {
    CCEntry *tail = charclass_head;
    while (tail->next) {
      tail = tail->next;
    }
    tail->next = ne;
  }
  charclass_count++;
  return (int)charclass_count;
}

/* ---------------------------------------------------------------------------
 * SPLIT/ANY Fusion Pass
 *
 * After all bytecode ops are emitted (but before charclass data is appended),
 * scan for eligible OP_SPLIT patterns and fuse them into a single OP_ANY that
 * matches the union of both arm character sets.
 *
 * Eligible pattern:
 *   OP_SPLIT(a, b)
 *   @ a:  <single-char-op>  OP_JMP(merge)     [arm-a, followed by JMP]
 *   @ b:  <single-char-op>  [arm-b, falls through to merge]
 *   Both arms must be OP_LIT(len=1) or OP_ANY; not mixed with OP_NOTANY.
 *   NOPs (from a previous fusion) are skipped when searching for the JMP.
 *
 * Rewrite:
 *   bc[split_pos]   = OP_ANY
 *   bc[split_pos+1] = merged_cc >> 8
 *   bc[split_pos+2] = merged_cc & 0xff
 *   bc[split_pos+3 .. merge-1] = OP_NOP
 *
 * The pass runs right-to-left so nested SPLITs (N-arm chains) are fused
 * innermost-first, enabling each outer SPLIT to see an already-fused OP_ANY.
 * ---------------------------------------------------------------------------*/

uint32_t fuse_read_u32(const uint8_t *bc, size_t off) {
  return ((uint32_t)bc[off] << 24) | ((uint32_t)bc[off + 1] << 16) |
         ((uint32_t)bc[off + 2] << 8) | (uint32_t)bc[off + 3];
}
uint16_t fuse_read_u16(const uint8_t *bc, size_t off) {
  return (uint16_t)(((uint16_t)bc[off] << 8) | bc[off + 1]);
}

/* Get CCEntry for 1-based id (order of add_or_get_charclass calls). */
CCEntry *get_cc_entry(uint32_t id) {
  CCEntry *e = charclass_head;
  uint32_t i = 1;
  while (e && i < id) {
    e = e->next;
    i++;
  }
  return (e && i == id) ? e : nullptr;
}

/*
 * Register a new charclass that is the union of two CCEntry ranges.
 * ea / eb may be nullptr; in that case cp_a / cp_b is a single codepoint.
 */
int fuse_add_union_cc(CCEntry *ea, uint32_t cp_a, CCEntry *eb, uint32_t cp_b,
                      uint8_t ci) {
  uint16_t na = ea ? ea->range_count : 1;
  uint16_t nb = eb ? eb->range_count : 1;
  CCEntry *ne = snobol_malloc(sizeof(*ne));
  memset(ne, 0, sizeof(*ne));
  ne->case_insensitive = ci;
  ne->range_cap = (uint16_t)(na + nb);
  ne->ranges = snobol_malloc(ne->range_cap * sizeof(CpRange));
  if (ea) {
    memcpy(ne->ranges, ea->ranges, na * sizeof(CpRange));
    ne->range_count = na;
  } else {
    ne->ranges[0].start = cp_a;
    ne->ranges[0].end = cp_a;
    ne->range_count = 1;
  }
  if (eb) {
    memcpy(ne->ranges + ne->range_count, eb->ranges, nb * sizeof(CpRange));
    ne->range_count = (uint16_t)(ne->range_count + nb);
  } else {
    ne->ranges[ne->range_count].start = cp_b;
    ne->ranges[ne->range_count].end = cp_b;
    ne->range_count++;
  }
  normalize_ranges(ne);

  /* Dedup: check for existing identical entry */
  CCEntry *e = charclass_head;
  int id = 1;
  while (e) {
    if (e->range_count == ne->range_count &&
        e->case_insensitive == ne->case_insensitive &&
        memcmp(e->ranges, ne->ranges, ne->range_count * sizeof(CpRange)) == 0) {
      if (ne->ranges) {
        snobol_free(ne->ranges);
      }
      snobol_free(ne);
      return id;
    }
    id++;
    e = e->next;
  }
  ne->next = nullptr;
  if (!charclass_head) {
    charclass_head = ne;
  } else {
    CCEntry *tail = charclass_head;
    while (tail->next) {
      tail = tail->next;
    }
    tail->next = ne;
  }
  charclass_count++;
  return (int)charclass_count;
}

/*
 * Describe a single-char arm.
 * type: 1=LIT, 2=ANY; 0=ineligible
 */
typedef struct {
  int type;       /* 1=LIT, 2=ANY */
  uint32_t cp;    /* codepoint (for LIT) */
  uint16_t cc_id; /* charclass id (for ANY) */
  size_t merge;   /* merge point (first byte after this arm's contribution) */
} ArmInfo;

/* Parse arm-a: single-char-op followed (after optional NOPs) by OP_JMP */
static ArmInfo parse_arm_a(const uint8_t *bc, size_t bc_len, size_t a) {
  ArmInfo r = {0};
  if (a >= bc_len) {
    return r;
  }
  uint8_t op = bc[a];
  size_t after_op;
  if (op == OP_LIT) {
    if (a + 9 > bc_len) {
      return r;
    }
    uint32_t len = fuse_read_u32(bc, a + 5);
    if (len != 1) {
      return r; /* multi-char LIT: not eligible */
    }
    if (a + 9 >= bc_len) {
      return r;
    }
    r.cp = (uint32_t)bc[a + 9]; /* single ASCII byte */
    after_op = a + 10;
    r.type = 1;
  } else if (op == OP_ANY) {
    if (a + 3 > bc_len) {
      return r;
    }
    r.cc_id = fuse_read_u16(bc, a + 1);
    after_op = a + 3;
    r.type = 2;
  } else {
    return r; /* ineligible opcode */
  }
  /* Skip NOPs between op and the required JMP */
  while (after_op < bc_len && bc[after_op] == OP_NOP) {
    after_op++;
  }
  if (after_op + 5 > bc_len) {
    r.type = 0;
    return r;
  }
  if (bc[after_op] != OP_JMP) {
    r.type = 0;
    return r;
  }
  r.merge = (size_t)fuse_read_u32(bc, after_op + 1);
  return r;
}

/* Parse arm-b: single-char-op that falls through to merge (no trailing JMP) */
static ArmInfo parse_arm_b(const uint8_t *bc, size_t bc_len, size_t b) {
  ArmInfo r = {0};
  if (b >= bc_len) {
    return r;
  }
  uint8_t op = bc[b];
  size_t after_op;
  if (op == OP_LIT) {
    if (b + 9 > bc_len) {
      return r;
    }
    uint32_t len = fuse_read_u32(bc, b + 5);
    if (len != 1) {
      return r;
    }
    if (b + 9 >= bc_len) {
      return r;
    }
    r.cp = (uint32_t)bc[b + 9];
    after_op = b + 10;
    r.type = 1;
  } else if (op == OP_ANY) {
    if (b + 3 > bc_len) {
      return r;
    }
    r.cc_id = fuse_read_u16(bc, b + 1);
    after_op = b + 3;
    r.type = 2;
  } else {
    return r;
  }
  /* Skip NOPs after the arm-b op to find the merge point */
  while (after_op < bc_len && bc[after_op] == OP_NOP) {
    after_op++;
  }
  r.merge = after_op;
  return r;
}

/**
 * Fuse SPLIT-of-ANY (and SPLIT-of-LIT) alternations into a single ANY.
 *
 * Scans the emitted bytecode right-to-left (so inner SPLITs fuse before
 * outer ones) and, for each SPLIT whose two arms are strictly-forward
 * LIT or ANY operations that merge at the same point, rewrites the SPLIT
 * site into a single OP_ANY whose charclass is the union of the two arm
 * classes, padding the gap to the merge point with OP_NOPs.  NOTANY arms
 * are never fused.  This shrinks the pattern and speeds up search-mode
 * execution by replacing two-arm branches with a single character-class
 * scan.
 *
 * @param cb Bytecode buffer holding the pattern to fuse (modified in
 *           place)
 */
void snobol_bc_fuse_split_any(CodeBuf *cb) {
  uint8_t *bc = cb->buf;
  size_t bc_len = cb->len;

  /* Right-to-left scan so inner SPLITs are fused before outer ones */
  size_t pos = bc_len;
  while (pos > 0) {
    pos--;
    if (bc[pos] != OP_SPLIT) {
      continue;
    }
    if (pos + 9 > bc_len) {
      continue;
    }

    size_t a = (size_t)fuse_read_u32(bc, pos + 1);
    size_t b = (size_t)fuse_read_u32(bc, pos + 5);

    /* Both arms must be strictly forward */
    if (a <= pos || b <= pos || a >= bc_len || b >= bc_len) {
      continue;
    }

    ArmInfo arm_a = parse_arm_a(bc, bc_len, a);
    ArmInfo arm_b = parse_arm_b(bc, bc_len, b);

    if (arm_a.type == 0 || arm_b.type == 0) {
      continue;
    }
    /* Don't mix NOTANY — only LIT and ANY are fused */
    /* arm_b and arm_a are type 1 or 2: compatible */
    if (arm_a.merge != arm_b.merge) {
      continue;
    }

    size_t merge = arm_a.merge;

    /* Determine union charclass */
    CCEntry *ea = (arm_a.type == 2) ? get_cc_entry(arm_a.cc_id) : nullptr;
    CCEntry *eb = (arm_b.type == 2) ? get_cc_entry(arm_b.cc_id) : nullptr;
    uint32_t cp_a = (arm_a.type == 1) ? arm_a.cp : 0;
    uint32_t cp_b = (arm_b.type == 1) ? arm_b.cp : 0;
    uint8_t ci =
        (ea && ea->case_insensitive) || (eb && eb->case_insensitive) ? 1 : 0;

    int merged_id = fuse_add_union_cc(ea, cp_a, eb, cp_b, ci);
    if (merged_id <= 0 || merged_id > 0xFFFF) {
      continue;
    }

    /* Rewrite: OP_ANY merged_id at pos, NOPs for pos+3 .. merge-1 */
    bc[pos] = (uint8_t)OP_ANY;
    bc[pos + 1] = (uint8_t)(((uint16_t)merged_id >> 8) & 0xFF);
    bc[pos + 2] = (uint8_t)((uint16_t)merged_id & 0xFF);
    for (size_t i = pos + 3; i < merge; i++) {
      bc[i] = (uint8_t)OP_NOP;
    }
  }
}

/* Emit literal inline: OP_LIT offset len bytes... (offset points to payload
 * location) */
int emit_lit_bytes(CodeBuf *c, const char *s, size_t len) {
  size_t off_of_payload = cb_pos(c) + 1 + 4 + 4;
  cb_emit_u8(c, OP_LIT);
  cb_emit_u32(c, (uint32_t)off_of_payload);
  cb_emit_u32(c, (uint32_t)len);
  cb_emit_bytes(c, (const uint8_t *)s, len);
  return 0;
}
