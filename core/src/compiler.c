#include "compiler_internal.h"

#ifndef STANDALONE_BUILD
/* Forward */
static int emit_node(zval *node, CodeBuf *c);

/* concat */
static int emit_concat(zval *parts, CodeBuf *c) {
  if (!parts || Z_TYPE_P(parts) != IS_ARRAY)
    return -1;
  zval *entry;
  int result = 0;
  ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(parts), entry) {
    if (!entry)
      continue;
    if (emit_node(entry, c) != 0) {
      result = -1;
      break;
    }
  }
  ZEND_HASH_FOREACH_END();
  return result;
}

/* alt */
static int emit_alt(zval *left, zval *right, CodeBuf *c) {
  size_t where_split = cb_pos(c);
  cb_emit_u8(c, OP_SPLIT);
  size_t where_a = cb_pos(c);
  cb_emit_u32(c, 0);
  size_t where_b = cb_pos(c);
  cb_emit_u32(c, 0);

  size_t left_start = cb_pos(c);
  if (emit_node(left, c) != 0)
    return -1;

  size_t jmp_where = cb_pos(c);
  cb_emit_u8(c, OP_JMP);
  size_t jmp_target_pos = cb_pos(c);
  cb_emit_u32(c, 0);

  size_t right_start = cb_pos(c);
  if (emit_node(right, c) != 0)
    return -1;

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

/* arbno (zero or more) */
static int emit_arbno(zval *sub, CodeBuf *c) {
  // Optimization: Unwrap nested ARBNOs.
  while (sub && Z_TYPE_P(sub) == IS_ARRAY) {
    zval *type_zv =
        zend_hash_str_find(Z_ARRVAL_P(sub), "type", sizeof("type") - 1);
    if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING)
      break;
    zend_string *type = Z_STR_P(type_zv);

    if (zend_string_equals_literal(type, "arbno")) {
      zval *inner =
          zend_hash_str_find(Z_ARRVAL_P(sub), "sub", sizeof("sub") - 1);
      if (inner) {
        sub = inner;
        continue;
      }
    } else if (zend_string_equals_literal(type, "repeat")) {
      // Check for repeat(x, 0, -1) which is ARBNO
      zval *min = zend_hash_str_find(Z_ARRVAL_P(sub), "min", sizeof("min") - 1);
      zval *max = zend_hash_str_find(Z_ARRVAL_P(sub), "max", sizeof("max") - 1);
      if (min && max && Z_TYPE_P(min) == IS_LONG && Z_TYPE_P(max) == IS_LONG) {
        if (Z_LVAL_P(min) == 0 && Z_LVAL_P(max) == -1) {
          zval *inner =
              zend_hash_str_find(Z_ARRVAL_P(sub), "sub", sizeof("sub") - 1);
          if (inner) {
            sub = inner;
            continue;
          }
        }
      }
    }
    break;
  }

  if (next_loop_id >= MAX_LOOPS)
    return -1;

  /* Zero-width-loop bounding (W2b): emit a diagnostic when the unbounded
   * arbno iterates over a nullable sub-pattern (the VM caps iterations). */
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
  cb_emit_u32(c, 0); // placeholder for skip_target

  size_t body_start = cb_pos(c);
  if (emit_node(sub, c) != 0)
    return -1;

  cb_emit_u8(c, OP_REPEAT_STEP);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, (uint32_t)body_start);

  size_t done_pos = cb_pos(c);
  // Fill skip_target
  uint32_t v_done = (uint32_t)done_pos;
  c->buf[skip_target_off + 0] = (v_done >> 24) & 0xff;
  c->buf[skip_target_off + 1] = (v_done >> 16) & 0xff;
  c->buf[skip_target_off + 2] = (v_done >> 8) & 0xff;
  c->buf[skip_target_off + 3] = v_done & 0xff;

  return 0;
}

/* span/break/any/notany */
static int emit_span(zval *set_str, CodeBuf *c) {
  if (!set_str || Z_TYPE_P(set_str) != IS_STRING)
    return -1;
  zend_string *zs = Z_STR_P(set_str);
  int setid = add_or_get_charclass(ZSTR_VAL(zs), ZSTR_LEN(zs));
  cb_emit_u8(c, OP_SPAN);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}
static int emit_break(zval *set_str, CodeBuf *c) {
  if (Z_TYPE_P(set_str) != IS_STRING)
    return -1;
  zend_string *zs = Z_STR_P(set_str);
  int setid = add_or_get_charclass(ZSTR_VAL(zs), ZSTR_LEN(zs));
  cb_emit_u8(c, OP_BREAK);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}
static int emit_any(zval *set_str, CodeBuf *c) {
  uint16_t setid = 0;
  if (set_str && Z_TYPE_P(set_str) == IS_STRING) {
    zend_string *zs = Z_STR_P(set_str);
    setid = (uint16_t)add_or_get_charclass(ZSTR_VAL(zs), ZSTR_LEN(zs));
  }
  cb_emit_u8(c, OP_ANY);
  cb_emit_u16(c, setid);
  return 0;
}
static int emit_notany(zval *set_str, CodeBuf *c) {
  if (Z_TYPE_P(set_str) != IS_STRING)
    return -1;
  zend_string *zs = Z_STR_P(set_str);
  int setid = add_or_get_charclass(ZSTR_VAL(zs), ZSTR_LEN(zs));
  cb_emit_u8(c, OP_NOTANY);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

/* New primitive emit functions */
static int emit_breakx(zval *set_str, CodeBuf *c) {
  if (!set_str || Z_TYPE_P(set_str) != IS_STRING)
    return -1;
  zend_string *zs = Z_STR_P(set_str);
  int setid = add_or_get_charclass(ZSTR_VAL(zs), ZSTR_LEN(zs));
  cb_emit_u8(c, OP_BREAKX);
  cb_emit_u16(c, (uint16_t)setid);
  return 0;
}

/* Decode first UTF-8 codepoint from a PHP string zval; return -1 on failure */
static int32_t first_codepoint(zval *str_zv) {
  if (!str_zv || Z_TYPE_P(str_zv) != IS_STRING)
    return -1;
  zend_string *zs = Z_STR_P(str_zv);
  if (ZSTR_LEN(zs) == 0)
    return -1;
  uint32_t cp = 0;
  int bytes = 0;
  if (!utf8_peek_next(ZSTR_VAL(zs), ZSTR_LEN(zs), 0, &cp, &bytes))
    return -1;
  return (int32_t)cp;
}

static int emit_bal(zval *open_zv, zval *close_zv, CodeBuf *c) {
  int32_t open_cp = first_codepoint(open_zv);
  int32_t close_cp = first_codepoint(close_zv);
  if (open_cp < 0 || close_cp < 0)
    return -1;
  cb_emit_u8(c, OP_BAL);
  cb_emit_u32(c, (uint32_t)open_cp);
  cb_emit_u32(c, (uint32_t)close_cp);
  return 0;
}

static int emit_fence(CodeBuf *c) {
  cb_emit_u8(c, OP_FENCE);
  return 0;
}

static int emit_rem(CodeBuf *c) {
  cb_emit_u8(c, OP_REM);
  return 0;
}

static int emit_rpos(zval *n_zv, CodeBuf *c) {
  if (!n_zv || Z_TYPE_P(n_zv) != IS_LONG)
    return -1;
  cb_emit_u8(c, OP_RPOS);
  cb_emit_u32(c, (uint32_t)(long)Z_LVAL_P(n_zv));
  return 0;
}

static int emit_rtab(zval *n_zv, CodeBuf *c) {
  if (!n_zv || Z_TYPE_P(n_zv) != IS_LONG)
    return -1;
  cb_emit_u8(c, OP_RTAB);
  cb_emit_u32(c, (uint32_t)(long)Z_LVAL_P(n_zv));
  return 0;
}

static int emit_pos(zval *n_zv, CodeBuf *c) {
  if (!n_zv || Z_TYPE_P(n_zv) != IS_LONG)
    return -1;
  cb_emit_u8(c, OP_POS);
  cb_emit_u32(c, (uint32_t)(long)Z_LVAL_P(n_zv));
  return 0;
}

static int emit_tab(zval *n_zv, CodeBuf *c) {
  if (!n_zv || Z_TYPE_P(n_zv) != IS_LONG)
    return -1;
  cb_emit_u8(c, OP_TAB);
  cb_emit_u32(c, (uint32_t)(long)Z_LVAL_P(n_zv));
  return 0;
}

static int emit_abort(CodeBuf *c) {
  cb_emit_u8(c, OP_ABORT);
  return 0;
}

static int emit_succeed(CodeBuf *c) {
  cb_emit_u8(c, OP_SUCCEED);
  return 0;
}

/* cap/assign/len/eval */
static int emit_cap(zval *reg_zv, zval *sub, CodeBuf *c) {
  if (!reg_zv || Z_TYPE_P(reg_zv) != IS_LONG)
    return -1;
  long reg = Z_LVAL_P(reg_zv);
  cb_emit_u8(c, OP_CAP_START);
  cb_emit_u8(c, (uint8_t)reg);
  if (emit_node(sub, c) != 0)
    return -1;
  cb_emit_u8(c, OP_CAP_END);
  cb_emit_u8(c, (uint8_t)reg);
  return 0;
}
static int emit_assign(zval *var_zv, zval *reg_zv, CodeBuf *c) {
  if (Z_TYPE_P(var_zv) != IS_LONG || Z_TYPE_P(reg_zv) != IS_LONG)
    return -1;
  long var = Z_LVAL_P(var_zv);
  long reg = Z_LVAL_P(reg_zv);
  cb_emit_u8(c, OP_ASSIGN);
  cb_emit_u16(c, (uint16_t)var);
  cb_emit_u8(c, (uint8_t)reg);
  return 0;
}
static int emit_len(zval *n_zv, CodeBuf *c) {
  if (Z_TYPE_P(n_zv) != IS_LONG)
    return -1;
  long n = Z_LVAL_P(n_zv);
  cb_emit_u8(c, OP_LEN);
  cb_emit_u32(c, (uint32_t)n);
  return 0;
}
static int emit_eval(zval *fn_zv, zval *reg_zv, CodeBuf *c) {
  if (Z_TYPE_P(fn_zv) != IS_LONG || Z_TYPE_P(reg_zv) != IS_LONG)
    return -1;
  long fn = Z_LVAL_P(fn_zv);
  long reg = Z_LVAL_P(reg_zv);
  cb_emit_u8(c, OP_EVAL);
  cb_emit_u16(c, (uint16_t)fn);
  cb_emit_u8(c, (uint8_t)reg);
  return 0;
}

static int emit_anchor(zval *type_zv, CodeBuf *c) {
  if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING)
    return -1;
  uint8_t t = 0;
  if (zend_string_equals_literal(Z_STR_P(type_zv), "start"))
    t = 0;
  else if (zend_string_equals_literal(Z_STR_P(type_zv), "end"))
    t = 1;
  else
    return -1;
  cb_emit_u8(c, OP_ANCHOR);
  cb_emit_u8(c, t);
  return 0;
}

static int emit_repeat(zval *sub, zval *min_zv, zval *max_zv, CodeBuf *c) {
  if (next_loop_id >= MAX_LOOPS)
    return -1;
  uint8_t loop_id = next_loop_id++;
  uint32_t min = (uint32_t)Z_LVAL_P(min_zv);
  uint32_t max = (uint32_t)Z_LVAL_P(max_zv);

  // Flatten logic for repeat(0, -1) same as ARBNO
  if (min == 0 && max == (uint32_t)-1) {
    while (sub && Z_TYPE_P(sub) == IS_ARRAY) {
      zval *type_zv =
          zend_hash_str_find(Z_ARRVAL_P(sub), "type", sizeof("type") - 1);
      if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING)
        break;
      zend_string *type = Z_STR_P(type_zv);

      if (zend_string_equals_literal(type, "arbno")) {
        zval *inner =
            zend_hash_str_find(Z_ARRVAL_P(sub), "sub", sizeof("sub") - 1);
        if (inner) {
          sub = inner;
          continue;
        }
      } else if (zend_string_equals_literal(type, "repeat")) {
        zval *imin =
            zend_hash_str_find(Z_ARRVAL_P(sub), "min", sizeof("min") - 1);
        zval *imax =
            zend_hash_str_find(Z_ARRVAL_P(sub), "max", sizeof("max") - 1);
        if (imin && imax && Z_TYPE_P(imin) == IS_LONG &&
            Z_TYPE_P(imax) == IS_LONG) {
          if (Z_LVAL_P(imin) == 0 && Z_LVAL_P(imax) == -1) {
            zval *inner =
                zend_hash_str_find(Z_ARRVAL_P(sub), "sub", sizeof("sub") - 1);
            if (inner) {
              sub = inner;
              continue;
            }
          }
        }
      }
      break;
    }
  }

  size_t init_pos = cb_pos(c);
  cb_emit_u8(c, OP_REPEAT_INIT);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, min);
  cb_emit_u32(c, max);
  size_t skip_target_off = cb_pos(c);
  cb_emit_u32(c, 0); // placeholder for skip_target

  size_t body_start = cb_pos(c);
  if (emit_node(sub, c) != 0)
    return -1;

  cb_emit_u8(c, OP_REPEAT_STEP);
  cb_emit_u8(c, loop_id);
  cb_emit_u32(c, (uint32_t)body_start);

  size_t done_pos = cb_pos(c);
  // Fill skip_target
  uint32_t v_done = (uint32_t)done_pos;
  c->buf[skip_target_off + 0] = (v_done >> 24) & 0xff;
  c->buf[skip_target_off + 1] = (v_done >> 16) & 0xff;
  c->buf[skip_target_off + 2] = (v_done >> 8) & 0xff;
  c->buf[skip_target_off + 3] = v_done & 0xff;

  return 0;
}

static int emit_emit(zval *node, CodeBuf *c) {
  zval *text = zend_hash_str_find(Z_ARRVAL_P(node), "text", sizeof("text") - 1);
  zval *reg = zend_hash_str_find(Z_ARRVAL_P(node), "reg", sizeof("reg") - 1);
  if (text && Z_TYPE_P(text) == IS_STRING) {
    zend_string *zs = Z_STR_P(text);
    size_t off_of_payload = cb_pos(c) + 1 + 4 + 4;
    cb_emit_u8(c, OP_EMIT_LITERAL);
    cb_emit_u32(c, (uint32_t)off_of_payload);
    cb_emit_u32(c, (uint32_t)ZSTR_LEN(zs));
    cb_emit_bytes(c, (const uint8_t *)ZSTR_VAL(zs), ZSTR_LEN(zs));
    return 0;
  } else if (reg && Z_TYPE_P(reg) == IS_LONG) {
    cb_emit_u8(c, OP_EMIT_CAPTURE);
    cb_emit_u8(c, (uint8_t)Z_LVAL_P(reg));
    return 0;
  }
  return -1;
}

static int emit_node(zval *node, CodeBuf *c) {
  if (Z_TYPE_P(node) != IS_ARRAY)
    return -1;
  zval *type_zv =
      zend_hash_str_find(Z_ARRVAL_P(node), "type", sizeof("type") - 1);
  if (!type_zv || Z_TYPE_P(type_zv) != IS_STRING)
    return -1;
  zend_string *type = Z_STR_P(type_zv);

  if (zend_string_equals_literal(type, "lit")) {
    zval *text =
        zend_hash_str_find(Z_ARRVAL_P(node), "text", sizeof("text") - 1);
    if (!text || Z_TYPE_P(text) != IS_STRING)
      return -1;
    zend_string *zs = Z_STR_P(text);
    return emit_lit_bytes(c, ZSTR_VAL(zs), ZSTR_LEN(zs));
  }
  if (zend_string_equals_literal(type, "concat")) {
    zval *parts =
        zend_hash_str_find(Z_ARRVAL_P(node), "parts", sizeof("parts") - 1);
    return emit_concat(parts, c);
  }
  if (zend_string_equals_literal(type, "alt")) {
    zval *left =
        zend_hash_str_find(Z_ARRVAL_P(node), "left", sizeof("left") - 1);
    zval *right =
        zend_hash_str_find(Z_ARRVAL_P(node), "right", sizeof("right") - 1);
    if (!left || !right)
      return -1;
    return emit_alt(left, right, c);
  }
  if (zend_string_equals_literal(type, "span")) {
    zval *set = zend_hash_str_find(Z_ARRVAL_P(node), "set", sizeof("set") - 1);
    return emit_span(set, c);
  }
  if (zend_string_equals_literal(type, "break")) {
    zval *set = zend_hash_str_find(Z_ARRVAL_P(node), "set", sizeof("set") - 1);
    return emit_break(set, c);
  }
  if (zend_string_equals_literal(type, "any")) {
    zval *set = zend_hash_str_find(Z_ARRVAL_P(node), "set", sizeof("set") - 1);
    return emit_any(set, c);
  }
  if (zend_string_equals_literal(type, "notany")) {
    zval *set = zend_hash_str_find(Z_ARRVAL_P(node), "set", sizeof("set") - 1);
    return emit_notany(set, c);
  }
  if (zend_string_equals_literal(type, "arbno")) {
    zval *sub = zend_hash_str_find(Z_ARRVAL_P(node), "sub", sizeof("sub") - 1);
    return emit_arbno(sub, c);
  }
  if (zend_string_equals_literal(type, "cap")) {
    zval *reg = zend_hash_str_find(Z_ARRVAL_P(node), "reg", sizeof("reg") - 1);
    zval *sub = zend_hash_str_find(Z_ARRVAL_P(node), "sub", sizeof("sub") - 1);
    return emit_cap(reg, sub, c);
  }
  if (zend_string_equals_literal(type, "assign")) {
    zval *var = zend_hash_str_find(Z_ARRVAL_P(node), "var", sizeof("var") - 1);
    zval *reg = zend_hash_str_find(Z_ARRVAL_P(node), "reg", sizeof("reg") - 1);
    return emit_assign(var, reg, c);
  }
  if (zend_string_equals_literal(type, "len")) {
    zval *n = zend_hash_str_find(Z_ARRVAL_P(node), "n", sizeof("n") - 1);
    return emit_len(n, c);
  }
  if (zend_string_equals_literal(type, "eval")) {
    zval *fn = zend_hash_str_find(Z_ARRVAL_P(node), "fn", sizeof("fn") - 1);
    zval *reg = zend_hash_str_find(Z_ARRVAL_P(node), "reg", sizeof("reg") - 1);
    return emit_eval(fn, reg, c);
  }
  if (zend_string_equals_literal(type, "anchor")) {
    zval *atype =
        zend_hash_str_find(Z_ARRVAL_P(node), "atype", sizeof("atype") - 1);
    return emit_anchor(atype, c);
  }
  if (zend_string_equals_literal(type, "repeat")) {
    zval *sub = zend_hash_str_find(Z_ARRVAL_P(node), "sub", sizeof("sub") - 1);
    zval *min = zend_hash_str_find(Z_ARRVAL_P(node), "min", sizeof("min") - 1);
    zval *max = zend_hash_str_find(Z_ARRVAL_P(node), "max", sizeof("max") - 1);
    if (!sub || !min || !max)
      return -1;
    return emit_repeat(sub, min, max, c);
  }
  if (zend_string_equals_literal(type, "emit")) {
    return emit_emit(node, c);
  }
  /* Pattern primitives: breakx, bal, fence, rem, rpos, rtab */
  if (zend_string_equals_literal(type, "breakx")) {
    zval *set = zend_hash_str_find(Z_ARRVAL_P(node), "set", sizeof("set") - 1);
    return emit_breakx(set, c);
  }
  if (zend_string_equals_literal(type, "bal")) {
    zval *open =
        zend_hash_str_find(Z_ARRVAL_P(node), "open", sizeof("open") - 1);
    zval *close =
        zend_hash_str_find(Z_ARRVAL_P(node), "close", sizeof("close") - 1);
    return emit_bal(open, close, c);
  }
  if (zend_string_equals_literal(type, "fence")) {
    return emit_fence(c);
  }
  if (zend_string_equals_literal(type, "rem")) {
    return emit_rem(c);
  }
  if (zend_string_equals_literal(type, "rpos")) {
    zval *n = zend_hash_str_find(Z_ARRVAL_P(node), "n", sizeof("n") - 1);
    return emit_rpos(n, c);
  }
  if (zend_string_equals_literal(type, "rtab")) {
    zval *n = zend_hash_str_find(Z_ARRVAL_P(node), "n", sizeof("n") - 1);
    return emit_rtab(n, c);
  }
  if (zend_string_equals_literal(type, "pos")) {
    zval *n = zend_hash_str_find(Z_ARRVAL_P(node), "n", sizeof("n") - 1);
    return emit_pos(n, c);
  }
  if (zend_string_equals_literal(type, "tab")) {
    zval *n = zend_hash_str_find(Z_ARRVAL_P(node), "n", sizeof("n") - 1);
    return emit_tab(n, c);
  }
  if (zend_string_equals_literal(type, "abort")) {
    return emit_abort(c);
  }
  if (zend_string_equals_literal(type, "fail")) {
    cb_emit_u8(c, OP_FAIL);
    return 0;
  }
  if (zend_string_equals_literal(type, "succeed")) {
    return emit_succeed(c);
  }
  if (zend_string_equals_literal(type, "dynamic_eval")) {
    /* dynamic_eval: compile pattern for runtime caching and execution.
     * Canonical approach: Store both the compiled bytecode AND the source text.
     * - Source text: Used as cache key for reuse across repeated EVAL(...)
     * - Bytecode: Used for efficient execution in the VM
     */
    zval *expr =
        zend_hash_str_find(Z_ARRVAL_P(node), "expr", sizeof("expr") - 1);
    zval *source_text =
        zend_hash_str_find(Z_ARRVAL_P(node), "source", sizeof("source") - 1);

    if (!expr)
      return -1;

    /* Compile the expression AST to bytecode */
    CodeBuf dynamic_cb;
    cb_init(&dynamic_cb);

    if (emit_node(expr, &dynamic_cb) != 0) {
      cb_free(&dynamic_cb);
      return -1;
    }
    cb_emit_u8(&dynamic_cb, OP_ACCEPT);

    /* Emit the compiled bytecode with source text metadata
     * Format: OP_DYNAMIC_DEF + source_len(u32) + source_text + bc_len(u32) +
     * bytecode The VM will use source for cache keying and bytecode for
     * execution */
    cb_emit_u8(c, OP_DYNAMIC_DEF);

    /* Emit source length and source text (for cache keying) */
    if (source_text && Z_TYPE_P(source_text) == IS_STRING) {
      zend_string *source_zs = Z_STR_P(source_text);
      cb_emit_u32(c, (uint32_t)ZSTR_LEN(source_zs));
      cb_emit_bytes(c, (const uint8_t *)ZSTR_VAL(source_zs),
                    ZSTR_LEN(source_zs));
    } else {
      /* Fallback: use bytecode as source (no cache reuse) */
      cb_emit_u32(c, (uint32_t)dynamic_cb.len);
      cb_emit_bytes(c, dynamic_cb.buf, dynamic_cb.len);
    }

    /* Emit bytecode length and bytecode (for execution) */
    cb_emit_u32(c, (uint32_t)dynamic_cb.len);
    cb_emit_bytes(c, dynamic_cb.buf, dynamic_cb.len);

    /* Emit OP_DYNAMIC to trigger execution */
    cb_emit_u8(c, OP_DYNAMIC);

    cb_free(&dynamic_cb);
    return 0;
  }
  return -1;
}

int compile_ast_to_bytecode(zval *ast, zval *options, uint8_t **out_bc,
                            size_t *out_len) {
  SNOBOL_LOG("compile_ast_to_bytecode START");
  free_charclass_list();
  next_loop_id = 0;
  compiler_case_insensitive = false;

  if (options && Z_TYPE_P(options) == IS_ARRAY) {
    zval *ci = zend_hash_str_find(Z_ARRVAL_P(options), "caseInsensitive",
                                  sizeof("caseInsensitive") - 1);
    if (ci && (Z_TYPE_P(ci) == IS_TRUE ||
               (Z_TYPE_P(ci) == IS_LONG && Z_LVAL_P(ci)))) {
      compiler_case_insensitive = true;
    }
  }

  CodeBuf cb;
  cb_init(&cb);
  if (emit_node(ast, &cb) != 0) {
    SNOBOL_LOG("compile_ast_to_bytecode FAILED at emit_node");
    cb_free(&cb);
    free_charclass_list();
    return -1;
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
    /* Emit offsets in reverse order so that set_id=1 maps to CC1's data.
     * The list was reversed before serialisation (last-added first), so
     * offsets[0] holds the position of CCN, offsets[N-1] holds CC1.
     * get_ranges_ptr uses (set_id-1) as the index, so we emit in
     * original-id order: CC1 at index 0, CC2 at index 1, … */
    for (int i = (int)charclass_count - 1; i >= 0; i--) {
      cb_emit_u32(&cb, (uint32_t)offsets[i]);
    }
    snobol_free(offsets);
  }

  cb_emit_u32(&cb, charclass_count);

  uint8_t *out = snobol_malloc(cb.len);
  if (!out) {
    SNOBOL_LOG("compile_ast_to_bytecode FAILED to allocate final bc");
    cb_free(&cb);
    return -1;
  }
  memcpy(out, cb.buf, cb.len);
  *out_bc = out;
  *out_len = cb.len;

  SNOBOL_LOG("compile_ast_to_bytecode SUCCESS, bc=%p, len=%zu", (void *)out,
             cb.len);

  cb_free(&cb);
  free_charclass_list();
  return 0;
}

#endif /* !STANDALONE_BUILD — PHP-specific AST compilation above only */

/* Emit OP_EMIT_TABLE for the key part following a table name.  Expects *ip
 * pointing at the opening '[' of the key and consumes: '[' then a quoted
 * literal key ("'sky'") or a capture-register key ("v0" / "$v0"), the
 * closing ']', and an optional outer ']'.  On malformed input, emits a
 * literal '$' (repositioning *ip to start_of_dollar + 1) and returns false.
 *
 * Format (literal key): opcode u8, table_id u16 (0xFFFF=unbound),
 *   key_type u8 (0=literal), name_len u8, name_bytes[name_len],
 *   key_len u16, key_bytes[key_len]
 * Format (capture key): opcode u8, table_id u16 (0xFFFF=unbound),
 *   key_type u8 (1=capture), name_len u8, name_bytes[name_len], key_reg u8
 */
static bool template_emit_table_lookup(CodeBuf *cb, const char *tpl, size_t len,
                                       size_t *ip, size_t start_of_dollar,
                                       const char *table_name,
                                       size_t table_name_len) {
  bool quoted = (*ip < len && tpl[*ip] == '\'') != 0;
  if (quoted) {
    (*ip)++; /* skip opening quote */
    size_t key_start = *ip;
    while (*ip < len && tpl[*ip] != '\'') {
      (*ip)++;
    }
    if (*ip >= len) {
      goto emit_literal; /* unclosed quote */
    }
    size_t key_len = *ip - key_start;
    (*ip)++; /* skip closing quote */
    if (*ip >= len || tpl[*ip] != ']') {
      goto emit_literal;
    }
    (*ip)++; /* skip ']' */
    if (*ip < len && tpl[*ip] == ']') {
      (*ip)++; /* consume outer ']' if present */
    }

    cb_emit_u8(cb, OP_EMIT_TABLE);
    cb_emit_u16(cb, (uint16_t)SNBL_TABLE_ID_UNBOUND);
    cb_emit_u8(cb, 0); /* key_type: 0 = literal key */
    cb_emit_u8(cb, (uint8_t)table_name_len);
    cb_emit_bytes(cb, (const uint8_t *)table_name, table_name_len);
    cb_emit_u16(cb, (uint16_t)key_len);
    cb_emit_bytes(cb, (const uint8_t *)(tpl + key_start), key_len);
    return true;
  }

  /* Capture-derived key: accept vN, $vN (documented $TABLE[$v0] form), or
   * bare digit(s). */
  if (*ip < len && tpl[*ip] == '$') {
    (*ip)++;
  }
  bool has_v_prefix = (*ip < len && tpl[*ip] == 'v') != 0;
  if (has_v_prefix) {
    (*ip)++; /* skip optional 'v' */
  }
  size_t key_reg_start = *ip;
  while (*ip < len && tpl[*ip] >= '0' && tpl[*ip] <= '9') {
    (*ip)++;
  }
  if (*ip == key_reg_start || *ip >= len || tpl[*ip] != ']') {
    goto emit_literal;
  }
  size_t key_reg = 0;
  for (size_t ki = key_reg_start; ki < *ip; ki++) {
    key_reg = (key_reg * 10) + (uint8_t)(tpl[ki] - '0');
  }
  (*ip)++; /* skip inner ']' */
  if (*ip < len && tpl[*ip] == ']') {
    (*ip)++; /* consume outer ']' if present */
  }

  cb_emit_u8(cb, OP_EMIT_TABLE);
  cb_emit_u16(cb, (uint16_t)SNBL_TABLE_ID_UNBOUND);
  cb_emit_u8(cb, 1); /* key_type: 1 = capture-derived key */
  cb_emit_u8(cb, (uint8_t)table_name_len);
  cb_emit_bytes(cb, (const uint8_t *)table_name, table_name_len);
  cb_emit_u8(cb, (uint8_t)key_reg);
  return true;

emit_literal:
  *ip = start_of_dollar + 1;
  cb_emit_u8(cb, OP_EMIT_LITERAL);
  size_t off = cb_pos(cb) + 4 + 4;
  cb_emit_u32(cb, (uint32_t)off);
  cb_emit_u32(cb, 1);
  cb_emit_u8(cb, '$');
  return false;
}

int compile_template_to_bytecode(const char *tpl, size_t len, uint8_t **out_bc,
                                 size_t *out_len) {
  SNOBOL_LOG("compile_template_to_bytecode START: tpl='%.*s'", (int)len, tpl);
  CodeBuf cb;
  cb_init(&cb);

  size_t i = 0;
  while (i < len) {
    if (tpl[i] == '$') {
      size_t start_of_dollar = i;
      i++;
      if (i >= len) {
        cb_emit_u8(&cb, OP_EMIT_LITERAL);
        size_t off = cb_pos(&cb) + 4 + 4;
        cb_emit_u32(&cb, (uint32_t)off);
        cb_emit_u32(&cb, 1);
        cb_emit_u8(&cb, '$');
        break;
      }

      bool braced = (tpl[i] == '{');
      if (braced) {
        i++;
      }

      if (i < len && tpl[i] == 'v') {
        i++;
        uint8_t reg = 0;
        bool has_digits = false;
        while (i < len && tpl[i] >= '0' && tpl[i] <= '9') {
          reg = (reg * 10) + (tpl[i] - '0');
          i++;
          has_digits = true;
        }

        if (!has_digits) {
          i = start_of_dollar + 1;
          cb_emit_u8(&cb, OP_EMIT_LITERAL);
          size_t off = cb_pos(&cb) + 4 + 4;
          cb_emit_u32(&cb, (uint32_t)off);
          cb_emit_u32(&cb, 1);
          cb_emit_u8(&cb, '$');
          continue;
        }

        uint8_t fmt_type = 0;
        uint16_t fmt_width = 0;
        uint8_t fmt_fill = ' ';
        bool fmt_has_width = false;
        if (braced) {
          if (i < len && tpl[i] == '.') {
            i++;
            if (len - i >= 7 && memcmp(tpl + i, "upper()", 7) == 0) {
              fmt_type = SNBL_FMT_UPPER;
              i += 7;
            } else if (len - i >= 7 && memcmp(tpl + i, "lower()", 7) == 0) {
              fmt_type = SNBL_FMT_LOWER;
              i += 7;
            } else if (len - i >= 8 && memcmp(tpl + i, "length()", 8) == 0) {
              fmt_type = SNBL_FMT_LENGTH;
              i += 8;
            } else if (len - i >= 4 && (memcmp(tpl + i, "lpad", 4) == 0 ||
                                        memcmp(tpl + i, "rpad", 4) == 0)) {
              uint8_t pad_type =
                  (tpl[i] == 'l') ? SNBL_FMT_LPAD : SNBL_FMT_RPAD;
              i += 4;
              if (i < len && tpl[i] == '(') {
                i++;
                /* parse mandatory width integer */
                if (i < len && tpl[i] >= '1' && tpl[i] <= '9') {
                  uint16_t w = 0;
                  while (i < len && tpl[i] >= '0' && tpl[i] <= '9') {
                    w = (uint16_t)((w * 10) + (tpl[i] - '0'));
                    i++;
                  }
                  /* optional fill char: ,'c' */
                  if (i < len && tpl[i] == ',') {
                    i++;
                    if (i < len && tpl[i] == '\'') {
                      i++;
                      if (i < len) {
                        fmt_fill = (uint8_t)tpl[i];
                        i++;
                        if (i < len && tpl[i] == '\'') {
                          i++;
                        }
                      }
                    }
                  }
                  if (i < len && tpl[i] == ')') {
                    i++;
                    fmt_type = pad_type;
                    fmt_width = w;
                    fmt_has_width = true;
                  }
                }
              }
            }
          }
          if (i < len && tpl[i] == '}') {
            i++;
            if (fmt_type == 0) {
              cb_emit_u8(&cb, OP_EMIT_CAPTURE);
              cb_emit_u8(&cb, reg);
            } else {
              cb_emit_u8(&cb, OP_EMIT_FORMAT);
              cb_emit_u8(&cb, reg);
              cb_emit_u8(&cb, fmt_type);
              if (fmt_has_width) {
                cb_emit_u16(&cb, fmt_width); /* big-endian width */
                cb_emit_u8(&cb, fmt_fill);   /* fill char */
              }
            }
          } else {
            i = start_of_dollar + 1;
            cb_emit_u8(&cb, OP_EMIT_LITERAL);
            size_t off = cb_pos(&cb) + 4 + 4;
            cb_emit_u32(&cb, (uint32_t)off);
            cb_emit_u32(&cb, 1);
            cb_emit_u8(&cb, '$');
          }
        } else if (i < len && tpl[i] == '[') {
          /* Table-backed replacement: $v0[TABLE[key]]
           * Parse TABLE name and key, emit OP_EMIT_TABLE */
          i++; /* skip '[' */

          /* Parse table name (identifier until '.' or '[') */
          size_t table_name_start = i;
          while (i < len && tpl[i] != '.' && tpl[i] != '[' && tpl[i] != ']') {
            i++;
          }
          size_t table_name_len = i - table_name_start;

          if (table_name_len == 0 || i >= len || tpl[i] != '[') {
            /* Invalid syntax, emit as literal '$' */
            i = start_of_dollar + 1;
            cb_emit_u8(&cb, OP_EMIT_LITERAL);
            size_t off = cb_pos(&cb) + 4 + 4;
            cb_emit_u32(&cb, (uint32_t)off);
            cb_emit_u32(&cb, 1);
            cb_emit_u8(&cb, '$');
            continue;
          }

          /* For now, table name must be a literal identifier */
          /* Extract table name */
          const char *table_name = tpl + table_name_start;

          /* Reject overlong table names (name_len field is 1 byte) */
          if (table_name_len > 255) {
            cb_free(&cb);
            return -1;
          }

          /* Skip '[' and parse key */
          i++; /* skip '[' */
          template_emit_table_lookup(&cb, tpl, len, &i, start_of_dollar,
                                     table_name, table_name_len);
          continue;
        } else {
          cb_emit_u8(&cb, OP_EMIT_CAPTURE);
          cb_emit_u8(&cb, reg);
        }
      } else {
        /* Documented table syntax $TABLE[key] / $TABLE[$vM]: an identifier
         * directly followed by '[' is a table reference (the name is not
         * bracketed, unlike $v0[TABLE[key]]).  An identifier not followed
         * by '[' stays literal text (e.g. "$version"). */
        size_t table_name_start = i;
        while (i < len && tpl[i] != '.' && tpl[i] != '[' && tpl[i] != ']') {
          i++;
        }
        size_t table_name_len = i - table_name_start;
        if (table_name_len > 0 && i < len && tpl[i] == '[') {
          const char *table_name = tpl + table_name_start;
          /* Reject overlong table names (name_len field is 1 byte) */
          if (table_name_len > 255) {
            cb_free(&cb);
            return -1;
          }
          i++; /* skip '[' */
          template_emit_table_lookup(&cb, tpl, len, &i, start_of_dollar,
                                     table_name, table_name_len);
          continue;
        }
        i = start_of_dollar + 1;
        cb_emit_u8(&cb, OP_EMIT_LITERAL);
        size_t off = cb_pos(&cb) + 4 + 4;
        cb_emit_u32(&cb, (uint32_t)off);
        cb_emit_u32(&cb, 1);
        cb_emit_u8(&cb, '$');
      }
    } else {
      // scan literal segment
      size_t start = i;
      while (i < len && tpl[i] != '$') {
        i++;
      }
      size_t seglen = i - start;
      cb_emit_u8(&cb, OP_EMIT_LITERAL);
      size_t off = cb_pos(&cb) + 4 + 4;
      cb_emit_u32(&cb, (uint32_t)off);
      cb_emit_u32(&cb, (uint32_t)seglen);
      cb_emit_bytes(&cb, (const uint8_t *)tpl + start, seglen);
    }
  }

  cb_emit_u8(&cb, OP_ACCEPT);

  uint8_t *out = snobol_malloc(cb.len);
  if (!out) {
    cb_free(&cb);
    return -1;
  }
  memcpy(out, cb.buf, cb.len);
  *out_bc = out;
  *out_len = cb.len;

  cb_free(&cb);
  SNOBOL_LOG("compile_template_to_bytecode SUCCESS, len=%zu", *out_len);
  return 0;
}

int snobol_template_bind_tables(uint8_t *bc, size_t bc_len, const char **names,
                                const uint16_t *ids, size_t n) {
  if (!bc || bc_len == 0) {
    return 0;
  }
  /* Allow n==0: still scan so we can detect and report any unbound table IDs */

  int result = 0;
  size_t ip = 0;

  while (ip < bc_len) {
    uint8_t op = bc[ip++];

    switch (op) {
      case OP_ACCEPT: return result; /* end of template bytecode */

      case OP_EMIT_LITERAL: {
        /* off:u32(4) + len:u32(4) + data[len] */
        if (ip + 8 > bc_len) {
          return result;
        }
        uint32_t lit_len = ((uint32_t)bc[ip + 4] << 24) |
                           ((uint32_t)bc[ip + 5] << 16) |
                           ((uint32_t)bc[ip + 6] << 8) | (uint32_t)bc[ip + 7];
        ip += 8 + lit_len;
        break;
      }

      case OP_EMIT_CAPTURE:
        ip += 1; /* reg:u8 */
        break;

      case OP_EMIT_EXPR:
        ip += 2; /* reg:u8, expr_type:u8 (legacy) */
        break;

      case OP_EMIT_FORMAT: {
        /* reg:u8, format_type:u8 [+ width:u16, fill:u8 for LPAD/RPAD] */
        if (ip + 2 > bc_len) {
          return result;
        }
        uint8_t fmt = bc[ip + 1];
        ip += 2;
        if (fmt == SNBL_FMT_LPAD || fmt == SNBL_FMT_RPAD) {
          ip += 3; /* width:u16 + fill:u8 */
        }
        break;
      }

      case OP_LIT: {
        if (ip + 8 > bc_len) {
          return result;
        }
        uint32_t len = ((uint32_t)bc[ip + 4] << 24) |
                       ((uint32_t)bc[ip + 5] << 16) |
                       ((uint32_t)bc[ip + 6] << 8) | (uint32_t)bc[ip + 7];
        ip += 8 + len;
        break;
      }

      case OP_ANY:
      case OP_NOTANY:
      case OP_SPAN:
      case OP_BREAK:
      case OP_BREAKX: ip += 2; break;

      case OP_LEN: ip += 4; break;

      case OP_CAP_START:
      case OP_CAP_END: ip += 1; break;

      case OP_ASSIGN: ip += 3; break;

      case OP_REM:
      case OP_FENCE:
      case OP_DYNAMIC:
      case OP_NOP:
      case OP_FAIL:
        /* no operands */
        break;

      case OP_SPLIT:
      case OP_REPEAT_INIT: ip += 8; break;

      case OP_REPEAT_STEP:
      case OP_JMP: ip += 4; break;

      case OP_RPOS:
      case OP_RTAB:
      case OP_POS:
      case OP_TAB: ip += 4; break;

      case OP_ABORT:
      case OP_SUCCEED:
        /* no operands */
        break;

      case OP_LABEL:
      case OP_GOTO:
      case OP_GOTO_F: ip += 2; break;

      case OP_BAL: ip += 8; break;

      case OP_EVAL: ip += 3; break;

      case OP_EMIT_TABLE: {
        /* table_id:u16, key_type:u8, name_len:u8, name_bytes[name_len], <key
       * payload> */
        if (ip + 4 > bc_len) {
          return result;
        }
        uint16_t tid = ((uint16_t)bc[ip] << 8) | bc[ip + 1];
        uint8_t key_type = bc[ip + 2];
        uint8_t nm_len = bc[ip + 3];

        if (ip + 4 + nm_len > bc_len) {
          return result;
        }

        if (tid == (uint16_t)SNBL_TABLE_ID_UNBOUND) {
          const char *name_ptr = (const char *)bc + ip + 4;
          bool resolved = false;
          for (size_t k = 0; k < n; k++) {
            if (names[k] && strlen(names[k]) == nm_len &&
                memcmp(names[k], name_ptr, nm_len) == 0) {
              bc[ip] = (uint8_t)((ids[k] >> 8) & 0xFF);
              bc[ip + 1] = (uint8_t)(ids[k] & 0xFF);
              resolved = true;
              break;
            }
          }
          if (!resolved) {
            result = -1;
          }
        }

        ip += 2 + 1 + 1 +
              nm_len; /* table_id + key_type + name_len + name_bytes */

        /* skip key payload */
        if (key_type == 0) {
          /* literal key: key_len:u16, key_bytes[key_len] */
          if (ip + 2 > bc_len) {
            return result;
          }
          uint16_t key_len = ((uint16_t)bc[ip] << 8) | bc[ip + 1];
          ip += 2 + key_len;
        } else if (key_type == 1) {
          /* capture key: key_reg:u8 */
          ip += 1;
        }
        break;
      }

      case OP_TABLE_GET:
      case OP_TABLE_SET: {
        /* table_id:u16, reg:u8, reg:u8, name_len:u8, name_bytes[name_len] */
        if (ip + 4 > bc_len) {
          return result;
        }
        uint16_t tid = ((uint16_t)bc[ip] << 8) | bc[ip + 1];
        uint8_t nm_len = bc[ip + 4];

        if (ip + 5 + nm_len > bc_len) {
          return result;
        }

        if (tid == (uint16_t)SNBL_TABLE_ID_UNBOUND) {
          const char *name_ptr = (const char *)bc + ip + 5;
          bool resolved = false;
          for (size_t k = 0; k < n; k++) {
            if (names[k] && strlen(names[k]) == nm_len &&
                memcmp(names[k], name_ptr, nm_len) == 0) {
              bc[ip] = (uint8_t)((ids[k] >> 8) & 0xFF);
              bc[ip + 1] = (uint8_t)(ids[k] & 0xFF);
              resolved = true;
              break;
            }
          }
          SNOBOL_LOG("snobol_template_bind_tables: tid=0xFFFF nm_len=%d "
                     "name='%.*s' resolved=%d",
                     (int)nm_len, (int)nm_len, name_ptr, (int)resolved);
          if (!resolved) {
            result = -1;
          }
        }
        ip += 5 + nm_len;
        break;
      }

      default: return result; /* unknown op — stop walking safely */
    }
  }

  return result;
}

void compiler_free(uint8_t *bc) {
  if (bc) {
    SNOBOL_LOG("compiler_free bc=%p", (void *)bc);
    snobol_free(bc);
  }
}
