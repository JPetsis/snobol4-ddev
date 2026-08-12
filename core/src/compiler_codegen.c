#include "compiler_internal.h"

/* ---------------------------------------------------------------------------
 * Label tracking for C-AST compilation
 * ---------------------------------------------------------------------------
 * Labels are assigned sequential numeric IDs (0, 1, 2, ...).
 * The label offset table is appended to the end of the bytecode after the
 * charclass section:
 *   [label offsets: u32 * label_count] [label_count: u32]
 * vm_exec reads this table and pre-registers labels before running.
 * ---------------------------------------------------------------------------*/
typedef struct {
  char *name;      /* Owned: label name string */
  uint16_t id;     /* Numeric ID (== index in table) */
  uint32_t offset; /* Bytecode offset immediately after OP_LABEL+id */
  bool defined;    /* Was this label defined (not just referenced)? */
  bool referenced; /* Was this label referenced by a goto? */
} LabelEntry;

static LabelEntry *label_table_c = nullptr;
static uint16_t label_table_count_c = 0;
static uint16_t label_table_capacity_c = 0;
static char label_error_c[256];
static bool label_has_error_c = false;

static void free_label_table_c(void) {
  for (uint16_t i = 0; i < label_table_count_c; i++) {
    if (label_table_c[i].name)
      snobol_free(label_table_c[i].name);
  }
  if (label_table_c)
    snobol_free(label_table_c);
  label_table_c = nullptr;
  label_table_count_c = 0;
  label_table_capacity_c = 0;
  label_has_error_c = false;
  label_error_c[0] = '\0';
}

/* Find label by name. Returns index (==id), or -1 if not found. */
static int find_label_c(const char *name) {
  for (uint16_t i = 0; i < label_table_count_c; i++) {
    if (label_table_c[i].name && strcmp(label_table_c[i].name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* Get or create label entry. Returns index (==id), or -1 on error. */
static int get_or_create_label_c(const char *name) {
  int idx = find_label_c(name);
  if (idx >= 0)
    return idx;

  if (label_table_count_c >= label_table_capacity_c) {
    uint16_t new_cap =
        label_table_capacity_c ? (uint16_t)(label_table_capacity_c * 2) : 8;
    LabelEntry *new_tbl =
        snobol_realloc(label_table_c, new_cap * sizeof(LabelEntry));
    if (!new_tbl)
      return -1;
    label_table_c = new_tbl;
    label_table_capacity_c = new_cap;
  }
  uint16_t new_idx = label_table_count_c++;
  label_table_c[new_idx].name = snobol_malloc(strlen(name) + 1);
  if (!label_table_c[new_idx].name) {
    label_table_count_c--;
    return -1;
  }
  strcpy(label_table_c[new_idx].name, name);
  label_table_c[new_idx].id = new_idx;
  label_table_c[new_idx].offset = 0;
  label_table_c[new_idx].defined = false;
  label_table_c[new_idx].referenced = false;
  return (int)new_idx;
}

/* Emit label offset table at end of bytecode (after charclass section).
 * Format: [offset_0 u32] ... [offset_{N-1} u32] [label_count u32] [MAGIC u32]
 * The MAGIC at bc_len-4 lets readers distinguish this new format from the
 * old format where charclass_count was the last u32. */
static void emit_label_table(CodeBuf *c) {
  for (uint16_t i = 0; i < label_table_count_c; i++) {
    cb_emit_u32(c, label_table_c[i].defined ? label_table_c[i].offset : 0);
  }
  cb_emit_u32(c, (uint32_t)label_table_count_c);
  cb_emit_u32(c, SNOBOL_LABEL_TABLE_MAGIC);
}

/* Forward declaration */
static int emit_node_c(ast_node_t *node, CodeBuf *c);
static int emit_alt_c(ast_node_t *left, ast_node_t *right, CodeBuf *c);
static int emit_arbno_c(ast_node_t *sub, CodeBuf *c);
static int emit_cap_c(ast_node_t *reg_node, ast_node_t *sub, CodeBuf *c);
static int emit_repeat_c(ast_node_t *sub, ast_node_t *min_node,
                         ast_node_t *max_node, CodeBuf *c);

/**
 * Emit case-insensitive literal: each codepoint in [s, len) is matched as
 * OP_ANY with a charclass containing both its cases.  Used when
 * compiler_case_insensitive is true for AST_LITERAL nodes.
 */
static int emit_lit_case_insensitive_c(CodeBuf *c, const char *s, size_t len) {
  size_t pos = 0;
  while (pos < len) {
    uint32_t cp;
    int cp_bytes;
    if (!utf8_peek_next(s, len, pos, &cp, &cp_bytes)) {
      /* Invalid / truncated byte — emit as raw literal */
      if (emit_lit_bytes(c, s + pos, 1) != 0)
        return -1;
      pos++;
      continue;
    }
    /* add_or_get_charclass will expand the codepoint's case peer when
     * compiler_case_insensitive is set. */
    int setid = add_or_get_charclass(s + pos, (size_t)cp_bytes);
    cb_emit_u8(c, OP_ANY);
    cb_emit_u16(c, (uint16_t)setid);
    pos += (size_t)cp_bytes;
  }
  return 0;
}

static int emit_span_c(const char *set, size_t len, CodeBuf *c) {
  int setid = add_or_get_charclass(set, len);
  cb_emit_u8(c, OP_SPAN);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

static int emit_break_c(const char *set, size_t len, CodeBuf *c) {
  int setid = add_or_get_charclass(set, len);
  cb_emit_u8(c, OP_BREAK);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

static int emit_any_c(const char *set, size_t len, CodeBuf *c) {
  uint16_t setid = 0;
  if (set && len > 0) {
    setid = (uint16_t)add_or_get_charclass(set, len);
  }
  cb_emit_u8(c, OP_ANY);
  cb_emit_u16(c, setid);
  return 0;
}

static int emit_notany_c(const char *set, size_t len, CodeBuf *c) {
  int setid = add_or_get_charclass(set, len);
  cb_emit_u8(c, OP_NOTANY);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

static int emit_assign_c(int var, int reg, CodeBuf *c) {
  cb_emit_u8(c, OP_ASSIGN);
  cb_emit_u16(c, (uint16_t)var);
  cb_emit_u8(c, (uint8_t)reg);
  return 0;
}

static int emit_len_c(int n, CodeBuf *c) {
  cb_emit_u8(c, OP_LEN);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}

static int emit_eval_c(int fn, int reg, CodeBuf *c) {
  cb_emit_u8(c, OP_EVAL);
  cb_emit_u16(c, (uint16_t)fn);
  cb_emit_u8(c, (uint8_t)reg);
  return 0;
}

static int emit_table_access_c(const char *table, ast_node_t *key, CodeBuf *c) {
  /* To support table access in patterns:
   * 1. Capture key into reg 0
   * 2. Emit OP_TABLE_GET with unbound ID and the name
   */
  cb_emit_u8(c, OP_CAP_START);
  cb_emit_u8(c, 0);
  if (emit_node_c(key, c) != 0)
    return -1;
  cb_emit_u8(c, OP_CAP_END);
  cb_emit_u8(c, 0);

  cb_emit_u8(c, OP_TABLE_GET);
  cb_emit_u16(c, 0xFFFF); /* unbound */
  cb_emit_u8(c, 0);       /* kreg */
  cb_emit_u8(c, 0);       /* dreg */
  size_t nlen = strlen(table);
  cb_emit_u8(c, (uint8_t)nlen);
  cb_emit_bytes(c, (const uint8_t *)table, nlen);
  return 0;
}

static int emit_table_update_c(const char *table, ast_node_t *key,
                               ast_node_t *value, CodeBuf *c) {
  /* Capture key into reg 0 */
  cb_emit_u8(c, OP_CAP_START);
  cb_emit_u8(c, 0);
  if (emit_node_c(key, c) != 0)
    return -1;
  cb_emit_u8(c, OP_CAP_END);
  cb_emit_u8(c, 0);

  /* Capture value into reg 1 */
  cb_emit_u8(c, OP_CAP_START);
  cb_emit_u8(c, 1);
  if (emit_node_c(value, c) != 0)
    return -1;
  cb_emit_u8(c, OP_CAP_END);
  cb_emit_u8(c, 1);

  cb_emit_u8(c, OP_TABLE_SET);
  cb_emit_u16(c, 0xFFFF); /* unbound */
  cb_emit_u8(c, 0);       /* kreg */
  cb_emit_u8(c, 1);       /* vreg */
  size_t nlen = strlen(table);
  cb_emit_u8(c, (uint8_t)nlen);
  cb_emit_bytes(c, (const uint8_t *)table, nlen);
  return 0;
}

static int emit_anchor_c(const char *type, CodeBuf *c) {
  if (strcmp(type, "start") == 0) {
    cb_emit_u8(c, OP_ANCHOR);
    cb_emit_u8(c, 0); /* ANCHOR_START */
  } else if (strcmp(type, "end") == 0) {
    cb_emit_u8(c, OP_ANCHOR);
    cb_emit_u8(c, 1); /* ANCHOR_END */
  }
  return 0;
}
static int emit_emit_c(const char *text, size_t len, int reg, CodeBuf *c) {
  if (text != nullptr) {
    /* Emit literal text (NUL-safe: the AST carries the exact byte length) */
    size_t off_of_payload = cb_pos(c) + 1 + 4 + 4;
    cb_emit_u8(c, OP_EMIT_LITERAL);
    cb_emit_u32(c, (uint32_t)off_of_payload);
    cb_emit_u32(c, (uint32_t)len);
    cb_emit_bytes(c, (const uint8_t *)text, len);
  } else if (reg >= 0) {
    /* Emit capture reference */
    cb_emit_u8(c, OP_EMIT_CAPTURE);
    cb_emit_u8(c, (uint8_t)reg);
  }
  return 0;
}

/* Pattern primitive emit helpers (C-AST path) */
static int emit_breakx_c(const char *set, size_t len, CodeBuf *c) {
  int setid = add_or_get_charclass(set, len);
  cb_emit_u8(c, OP_BREAKX);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

static int emit_bal_c(uint32_t open_cp, uint32_t close_cp, CodeBuf *c) {
  cb_emit_u8(c, OP_BAL);
  cb_emit_u32(c, open_cp);
  cb_emit_u32(c, close_cp);
  return 0;
}

static int emit_fence_c(CodeBuf *c) {
  cb_emit_u8(c, OP_FENCE);
  return 0;
}

static int emit_rem_c(CodeBuf *c) {
  cb_emit_u8(c, OP_REM);
  return 0;
}

static int emit_rpos_c(int32_t n, CodeBuf *c) {
  cb_emit_u8(c, OP_RPOS);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}

static int emit_rtab_c(int32_t n, CodeBuf *c) {
  cb_emit_u8(c, OP_RTAB);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}

static int emit_pos_c(int32_t n, CodeBuf *c) {
  cb_emit_u8(c, OP_POS);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}

static int emit_tab_c(int32_t n, CodeBuf *c) {
  cb_emit_u8(c, OP_TAB);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}

static int emit_abort_c(CodeBuf *c) {
  cb_emit_u8(c, OP_ABORT);
  return 0;
}

static int emit_succeed_c(CodeBuf *c) {
  cb_emit_u8(c, OP_SUCCEED);
  return 0;
}

/**
 * Compile C AST to bytecode
 * @param ast Root AST node
 * @param case_insensitive Enable case-insensitive matching
 * @param out_bc Output: bytecode buffer
 * @param out_len Output: bytecode length
 * @return 0 on success, -1 on failure
 */
int compile_ast_to_bytecode_c(ast_node_t *ast, bool case_insensitive,
                              uint8_t **out_bc, size_t *out_len) {
  SNOBOL_LOG("compile_ast_to_bytecode_c START");
  free_charclass_list();
  free_label_table_c();
  next_loop_id = 0;
  compiler_case_insensitive = case_insensitive;

  CodeBuf cb;
  cb_init(&cb);

  if (emit_node_c(ast, &cb) != 0) {
    SNOBOL_LOG("compile_ast_to_bytecode_c FAILED at emit_node_c");
    cb_free(&cb);
    free_charclass_list();
    free_label_table_c();
    return -1;
  }

  /* Validate unknown labels - all referenced labels must be defined */
  for (uint16_t i = 0; i < label_table_count_c; i++) {
    if (label_table_c[i].referenced && !label_table_c[i].defined) {
      SNOBOL_LOG("compile_ast_to_bytecode_c FAILED: undefined label '%s'",
                 label_table_c[i].name);
      cb_free(&cb);
      free_charclass_list();
      free_label_table_c();
      return -1;
    }
  }

  cb_emit_u8(&cb, OP_ACCEPT);

  /* Fusion pass: fuse eligible SPLIT/LIT|ANY pairs into a single OP_ANY */
  snobol_bc_fuse_split_any(&cb);

  CCEntry *rev = nullptr;
  for (CCEntry *it = charclass_head; it != nullptr;) {
    CCEntry *next = it->next;
    it->next = rev;
    rev = it;
    it = next;
  }
  charclass_head = rev;

  size_t *offsets = charclass_count > 0
                        ? snobol_malloc(charclass_count * sizeof(size_t))
                        : nullptr;
  int idx = 0;
  for (CCEntry *it = charclass_head; it != nullptr; it = it->next) {
    if (offsets)
      offsets[idx++] = cb_pos(&cb);
    cb_emit_u16(&cb, it->range_count);
    cb_emit_u16(&cb, it->case_insensitive);
    for (size_t i = 0; i < it->range_count; ++i) {
      cb_emit_u32(&cb, it->ranges[i].start);
      cb_emit_u32(&cb, it->ranges[i].end);
    }
  }

  if (offsets) {
    /* Emit offsets in original-id order (CC1 at index 0, CC2 at index 1, …)
     * so get_ranges_ptr(vm, k) correctly resolves to CCk's data.
     * The list was reversed before serialisation so offsets[0]=CCN; we
     * emit in reverse to restore the original-id mapping. */
    for (int i = (int)charclass_count - 1; i >= 0; i--) {
      cb_emit_u32(&cb, (uint32_t)offsets[i]);
    }
    snobol_free(offsets);
  }

  cb_emit_u32(&cb, charclass_count);

  /* Emit label offset table (always, even if empty - label_count=0 is valid) */
  emit_label_table(&cb);

  uint8_t *out = snobol_malloc(cb.len);
  if (!out) {
    SNOBOL_LOG("compile_ast_to_bytecode_c FAILED to allocate final bc");
    cb_free(&cb);
    free_label_table_c();
    return -1;
  }
  memcpy(out, cb.buf, cb.len);
  *out_bc = out;
  *out_len = cb.len;

  cb_free(&cb);
  free_charclass_list();
  free_label_table_c();
  SNOBOL_LOG("compile_ast_to_bytecode_c SUCCESS, len=%zu", *out_len);
  return 0;
}

/* C AST emit helpers */
static int emit_alt_c(ast_node_t *left, ast_node_t *right, CodeBuf *c) {
  /* Emit SPLIT opcode to try both alternatives */
  size_t where_split = cb_pos(c);
  cb_emit_u8(c, OP_SPLIT);
  size_t where_a = cb_pos(c);
  cb_emit_u32(c, 0); /* placeholder */
  size_t where_b = cb_pos(c);
  cb_emit_u32(c, 0); /* placeholder */

  /* Left alternative */
  size_t left_start = cb_pos(c);
  if (emit_node_c(left, c) != 0)
    return -1;

  /* Jump past right alternative */
  size_t jmp_where = cb_pos(c);
  cb_emit_u8(c, OP_JMP);
  size_t jmp_target_pos = cb_pos(c);
  cb_emit_u32(c, 0);

  /* Right alternative */
  size_t right_start = cb_pos(c);
  if (emit_node_c(right, c) != 0)
    return -1;

  /* Fill in placeholders */
  size_t end_pos = cb_pos(c);
  uint32_t v1 = (uint32_t)left_start;
  c->buf[where_a + 0] = (v1 >> 24) & 0xff;
  c->buf[where_a + 1] = (v1 >> 16) & 0xff;
  c->buf[where_a + 2] = (v1 >> 8) & 0xff;
  c->buf[where_a + 3] = v1 & 0xff;

  uint32_t v2 = (uint32_t)right_start;
  c->buf[where_b + 0] = (v2 >> 24) & 0xff;
  c->buf[where_b + 1] = (v2 >> 16) & 0xff;
  c->buf[where_b + 2] = (v2 >> 8) & 0xff;
  c->buf[where_b + 3] = v2 & 0xff;

  uint32_t vj = (uint32_t)end_pos;
  c->buf[jmp_target_pos + 0] = (vj >> 24) & 0xff;
  c->buf[jmp_target_pos + 1] = (vj >> 16) & 0xff;
  c->buf[jmp_target_pos + 2] = (vj >> 8) & 0xff;
  c->buf[jmp_target_pos + 3] = vj & 0xff;

  return 0;
}

static int emit_arbno_c(ast_node_t *sub, CodeBuf *c) {
  /* Unwrap nested ARBNOs */
  while (sub && sub->type == AST_ARBNO) {
    sub = sub->data.arbno.sub;
  }

  /* Unwrap REPEAT(0, -1) which is equivalent to ARBNO */
  while (sub && sub->type == AST_REPETITION) {
    if (sub->data.repetition.min == 0 && sub->data.repetition.max == -1) {
      sub = sub->data.repetition.sub;
      continue;
    }
    break;
  }

  if (!sub)
    return -1;
  if (next_loop_id >= MAX_LOOPS)
    return -1;

  /* Zero-width-loop bounding (W2b): an unbounded arbno over a nullable
   * sub-pattern can create O(n^2)/exponential choice-point blowup. The VM now
   * caps iterations to subject_len (semantically identical), and we emit a
   * diagnostic so the caller is aware. */
  if (ast_node_nullable(sub)) {
    snobol_diag("arbno over a nullable sub-pattern: bounding zero-width "
                "iterations to subject length");
  }

  uint8_t loop_id = next_loop_id++;
  uint32_t min = 0;
  uint32_t max = (uint32_t)-1;

  size_t init_pos = cb_pos(c);
  cb_emit_u8(c, OP_REPEAT_INIT);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, min);
  cb_emit_u32(c, max);
  size_t skip_target_off = cb_pos(c);
  cb_emit_u32(c, 0); /* placeholder for skip_target */

  size_t body_start = cb_pos(c);
  if (emit_node_c(sub, c) != 0)
    return -1;

  cb_emit_u8(c, OP_REPEAT_STEP);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, (uint32_t)body_start);

  size_t done_pos = cb_pos(c);

  /* Fill skip_target placeholder */
  uint32_t v_done = (uint32_t)done_pos;
  c->buf[skip_target_off + 0] = (v_done >> 24) & 0xff;
  c->buf[skip_target_off + 1] = (v_done >> 16) & 0xff;
  c->buf[skip_target_off + 2] = (v_done >> 8) & 0xff;
  c->buf[skip_target_off + 3] = v_done & 0xff;

  return 0;
}

static int emit_cap_c(ast_node_t *reg_node, ast_node_t *sub, CodeBuf *c) {
  /* Emit capture start, sub-pattern, capture end */
  cb_emit_u8(c, OP_CAP_START);
  cb_emit_u8(c, (uint8_t)reg_node->data.len.n);

  if (emit_node_c(sub, c) != 0)
    return -1;

  cb_emit_u8(c, OP_CAP_END);
  cb_emit_u8(c, (uint8_t)reg_node->data.len.n);
  return 0;
}

static int emit_repeat_c(ast_node_t *sub, ast_node_t *min_node,
                         ast_node_t *max_node, CodeBuf *c) {
  if (!sub)
    return -1;
  if (next_loop_id >= MAX_LOOPS)
    return -1;

  uint8_t loop_id = next_loop_id++;
  uint32_t min = (uint32_t)min_node->data.len.n;
  uint32_t max = (uint32_t)max_node->data.len.n;

  /* Flatten repeat(0, -1) as ARBNO */
  if (min == 0 && max == (uint32_t)-1) {
    /* Unwrap nested */
    while (sub && sub->type == AST_REPETITION) {
      if (sub->data.repetition.min == 0 && sub->data.repetition.max == -1) {
        sub = sub->data.repetition.sub;
        continue;
      }
      break;
    }
    /* Use arbno logic */
    return emit_arbno_c(sub, c);
  }

  size_t init_pos = cb_pos(c);
  cb_emit_u8(c, OP_REPEAT_INIT);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, min);
  cb_emit_u32(c, max);
  size_t skip_target_off = cb_pos(c);
  cb_emit_u32(c, 0); /* placeholder */

  size_t body_start = cb_pos(c);
  if (emit_node_c(sub, c) != 0)
    return -1;

  cb_emit_u8(c, OP_REPEAT_STEP);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, (uint32_t)body_start);

  size_t done_pos = cb_pos(c);

  /* Fill skip_target */
  uint32_t v_done = (uint32_t)done_pos;
  c->buf[skip_target_off + 0] = (v_done >> 24) & 0xff;
  c->buf[skip_target_off + 1] = (v_done >> 16) & 0xff;
  c->buf[skip_target_off + 2] = (v_done >> 8) & 0xff;
  c->buf[skip_target_off + 3] = v_done & 0xff;

  return 0;
}

/**
 * Emit bytecode for a C AST node
 */
static int emit_node_c(ast_node_t *node, CodeBuf *c) {
  if (!node)
    return -1;

  switch (node->type) {
    case AST_LITERAL:
      if (compiler_case_insensitive) {
        return emit_lit_case_insensitive_c(c, node->data.literal.text,
                                           node->data.literal.len);
      }
      return emit_lit_bytes(c, node->data.literal.text, node->data.literal.len);

    case AST_CONCAT:
      for (size_t i = 0; i < node->data.concat.count; i++) {
        if (emit_node_c(node->data.concat.parts[i], c) != 0) {
          return -1;
        }
      }
      return 0;

    case AST_ALT:
      return emit_alt_c(node->data.alt.left, node->data.alt.right, c);

    case AST_REPETITION: {
      /* Create stub AST nodes for min/max values */
      ast_node_t min_node = {.type = AST_LEN,
                             .data.len.n = node->data.repetition.min};
      ast_node_t max_node = {.type = AST_LEN,
                             .data.len.n = node->data.repetition.max};
      return emit_repeat_c(node->data.repetition.sub, &min_node, &max_node, c);
    }

    case AST_SPAN:
      return emit_span_c(node->data.charclass.set, node->data.charclass.len, c);

    case AST_BREAK:
      return emit_break_c(node->data.charclass.set, node->data.charclass.len,
                          c);

    case AST_ANY:
      return emit_any_c(node->data.charclass.set, node->data.charclass.len, c);

    case AST_NOTANY:
      return emit_notany_c(node->data.charclass.set, node->data.charclass.len,
                           c);

    case AST_ARBNO: return emit_arbno_c(node->data.arbno.sub, c);

    case AST_CAP: {
      /* Emit capture start, sub-pattern, capture end */
      cb_emit_u8(c, OP_CAP_START);
      cb_emit_u8(c, (uint8_t)node->data.cap.reg);

      if (emit_node_c(node->data.cap.sub, c) != 0)
        return -1;

      cb_emit_u8(c, OP_CAP_END);
      cb_emit_u8(c, (uint8_t)node->data.cap.reg);
      return 0;
    }

    case AST_ASSIGN:
      return emit_assign_c(node->data.assign.var, node->data.assign.reg, c);

    case AST_LEN: return emit_len_c(node->data.len.n, c);

    case AST_EVAL:
      return emit_eval_c(node->data.eval.fn, node->data.eval.reg, c);

    case AST_DYNAMIC_EVAL:
      /* Dynamic eval: compile inner expression and emit as dynamic pattern */
      return emit_node_c(node->data.dynamic_eval.expr, c);

    case AST_ANCHOR:
      return emit_anchor_c(
          node->data.anchor.atype == ANCHOR_START ? "start" : "end", c);

    case AST_EMIT:
      return emit_emit_c(node->data.emit.text, node->data.emit.len,
                         node->data.emit.reg, c);

    case AST_BREAKX:
      return emit_breakx_c(node->data.breakx.set, node->data.breakx.len, c);

    case AST_BAL:
      return emit_bal_c(node->data.bal.open_cp, node->data.bal.close_cp, c);

    case AST_FENCE: return emit_fence_c(c);

    case AST_REM: return emit_rem_c(c);

    case AST_RPOS: return emit_rpos_c(node->data.rpos_rtab.n, c);

    case AST_RTAB: return emit_rtab_c(node->data.rpos_rtab.n, c);

    case AST_POS: return emit_pos_c(node->data.rpos_rtab.n, c);

    case AST_TAB: return emit_tab_c(node->data.rpos_rtab.n, c);

    case AST_ABORT: return emit_abort_c(c);

    case AST_FAIL: cb_emit_u8(c, OP_FAIL); return 0;

    case AST_SUCCEED: return emit_succeed_c(c);

    case AST_LABEL: {
      /* Emit OP_LABEL opcode, register offset, detect duplicates */
      const char *name = node->data.label.name;
      if (!name)
        return -1;

      /* Duplicate label check */
      int existing = find_label_c(name);
      if (existing >= 0 && label_table_c[existing].defined) {
        snprintf(label_error_c, sizeof(label_error_c), "Duplicate label: '%s'",
                 name);
        label_has_error_c = true;
        SNOBOL_LOG("emit_node_c: duplicate label '%s'", name);
        return -1;
      }

      int idx = get_or_create_label_c(name);
      if (idx < 0)
        return -1;

      /* Emit OP_LABEL with numeric label ID */
      cb_emit_u8(c, OP_LABEL);
      cb_emit_u16(c, (uint16_t)idx);

      /* Record the offset AFTER OP_LABEL+id - this is where OP_GOTO will transfer
     * to */
      label_table_c[idx].offset = (uint32_t)cb_pos(c);
      label_table_c[idx].defined = true;

      /* Compile the target pattern (code that runs at/after this label) */
      if (node->data.label.target) {
        return emit_node_c(node->data.label.target, c);
      }
      return 0;
    }

    case AST_GOTO: {
      /* Emit OP_GOTO for goto statement */
      const char *label_name = node->data.goto_stmt.label;
      if (!label_name)
        return -1;

      int idx = get_or_create_label_c(label_name);
      if (idx < 0)
        return -1;

      label_table_c[idx].referenced = true;

      cb_emit_u8(c, OP_GOTO);
      cb_emit_u16(c, (uint16_t)idx);
      return 0;
    }

    case AST_TABLE_ACCESS:
      return emit_table_access_c(node->data.table_access.table,
                                 node->data.table_access.key, c);

    case AST_TABLE_UPDATE:
      return emit_table_update_c(node->data.table_update.table,
                                 node->data.table_update.key,
                                 node->data.table_update.value, c);

    default:
      SNOBOL_LOG("emit_node_c: unknown node type %d", node->type);
      return -1;
  }
}
