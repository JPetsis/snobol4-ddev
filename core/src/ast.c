/**
 * @file ast.c
 * @brief AST node creation and memory management
 */

#include "snobol/ast.h"
#include "snobol/snobol_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Thread-local bump arena used for AST node storage during a single
 * compile.  NULL when the default heap allocator is active (e.g. external
 * callers of snobol_ast_free, or threads that have not bound an arena).
 */
static SNOBOL_THREAD_LOCAL snobol_arena_t *g_ast_arena = nullptr;

void snobol_ast_set_arena(snobol_arena_t *arena) {
  g_ast_arena = arena;
}

snobol_arena_t *snobol_ast_clear_arena(void) {
  snobol_arena_t *a = g_ast_arena;
  g_ast_arena = nullptr;
  return a;
}

/**
 * Allocate (and zero) an AST node.  Draws from the thread-local bump arena
 * when one is bound; on exhaustion (or when no arena is bound) falls back to
 * the heap.  The arena_allocated flag records the provenance so
 * snobol_ast_free() knows whether to call free().
 */
static ast_node_t *ast_node_alloc(void) {
  if (g_ast_arena) {
    ast_node_t *node = (ast_node_t *)snobol_arena_alloc(
        g_ast_arena, sizeof(ast_node_t), _Alignof(ast_node_t));
    if (node) {
      memset(node, 0, sizeof(*node));
      node->arena_allocated = true;
      return node;
    }
  }
  return (ast_node_t *)calloc(1, sizeof(ast_node_t));
}

/**
 * Internal helper: duplicate a string
 */
static char *str_dup(const char *s, size_t len) {
  if (!s) {
    return nullptr;
  }
  char *dup = (char *)malloc(len + 1);
  if (dup) {
    memcpy(dup, s, len);
    dup[len] = '\0';
  }
  return dup;
}

ast_node_t *snobol_ast_create_lit(const char *text, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_LITERAL;
  node->data.literal.text = str_dup(text, len);
  node->data.literal.len = len;
  return node;
}

ast_node_t *snobol_ast_create_concat(ast_node_t **parts, size_t count) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_CONCAT;
  node->data.concat.parts = parts;
  node->data.concat.count = count;
  return node;
}

ast_node_t *snobol_ast_create_alt(ast_node_t *left, ast_node_t *right) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_ALT;
  node->data.alt.left = left;
  node->data.alt.right = right;
  return node;
}

ast_node_t *snobol_ast_create_arbno(ast_node_t *sub) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_ARBNO;
  node->data.arbno.sub = sub;
  return node;
}

ast_node_t *snobol_ast_create_cap(int reg, ast_node_t *sub) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_CAP;
  node->data.cap.reg = reg;
  node->data.cap.sub = sub;
  return node;
}

ast_node_t *snobol_ast_create_span(const char *set, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_SPAN;
  node->data.charclass.set = str_dup(set, len);
  node->data.charclass.len = len;
  return node;
}

ast_node_t *snobol_ast_create_any(const char *set, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_ANY;
  if (set) {
    node->data.charclass.set = str_dup(set, len);
    node->data.charclass.len = len;
  } else {
    node->data.charclass.set = nullptr;
    node->data.charclass.len = 0;
  }
  return node;
}

ast_node_t *snobol_ast_create_repeat(ast_node_t *sub, int32_t min,
                                     int32_t max) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_REPETITION;
  node->data.repetition.sub = sub;
  node->data.repetition.min = min;
  node->data.repetition.max = max;
  return node;
}

ast_node_t *snobol_ast_create_goto(const char *label) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_GOTO;
  if (label) {
    size_t len = strlen(label);
    node->data.goto_stmt.label = (char *)malloc(len + 1);
    if (node->data.goto_stmt.label) {
      strcpy(node->data.goto_stmt.label, label);
    }
  } else {
    node->data.goto_stmt.label = nullptr;
  }
  return node;
}

ast_node_t *snobol_ast_create_label(char *name, ast_node_t *target) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }

  node->type = AST_LABEL;
  /* Make a copy of the name - caller may pass string literals */
  if (name) {
    size_t len = strlen(name);
    node->data.label.name = (char *)malloc(len + 1);
    if (node->data.label.name) {
      strcpy(node->data.label.name, name);
    }
    /* Note: We don't free 'name' - caller is responsible for it.
     * If caller passed a malloc'd string, they should free it.
     * If caller passed a string literal, we have our own copy. */
  } else {
    node->data.label.name = nullptr;
  }
  node->data.label.target = target;
  return node;
}

/**
 * Deep-copy an AST subtree.
 *
 * Recursively clones the node and all owned descendants (concat parts,
 * alternation children, capture/arbno/repetition bodies, literal and
 * charclass payloads, label names, and table keys/values), producing a
 * structurally identical tree that shares no memory with the source.
 * Nodes whose children were bound via snobol_ast_set_arena are cloned into
 * the same arena when one is active, otherwise into the heap.
 *
 * The caller owns the returned tree and must release it with
 * snobol_ast_free() when it is no longer needed.
 *
 * @param node AST subtree to clone, or NULL
 * @return Newly allocated clone, or NULL when @p node is NULL or the
 *         allocation fails
 */
ast_node_t *snobol_ast_clone(const ast_node_t *node) {
  if (!node) {
    return nullptr;
  }

  ast_node_t *clone = ast_node_alloc();
  if (!clone) {
    return nullptr;
  }

  clone->type = node->type;

  switch (node->type) {
    case AST_LITERAL:
      clone->data.literal.text =
          str_dup(node->data.literal.text, node->data.literal.len);
      clone->data.literal.len = node->data.literal.len;
      break;

    case AST_EMIT:
      clone->data.emit.text =
          str_dup(node->data.emit.text, node->data.emit.len);
      clone->data.emit.len = node->data.emit.len;
      clone->data.emit.reg = node->data.emit.reg;
      break;

    case AST_CONCAT: {
      clone->data.concat.count = node->data.concat.count;
      clone->data.concat.parts =
          (ast_node_t **)calloc(clone->data.concat.count, sizeof(ast_node_t *));
      if (!clone->data.concat.parts) {
        snobol_ast_free(clone);
        return nullptr;
      }
      for (size_t i = 0; i < clone->data.concat.count; i++) {
        clone->data.concat.parts[i] =
            snobol_ast_clone(node->data.concat.parts[i]);
      }
      break;
    }

    case AST_ALT:
      clone->data.alt.left = snobol_ast_clone(node->data.alt.left);
      clone->data.alt.right = snobol_ast_clone(node->data.alt.right);
      break;

    case AST_REPETITION:
    case AST_ARBNO:
      clone->data.repetition.sub = snobol_ast_clone(node->data.repetition.sub);
      clone->data.repetition.min = node->data.repetition.min;
      clone->data.repetition.max = node->data.repetition.max;
      break;

    case AST_CAP:
      clone->data.cap.reg = node->data.cap.reg;
      clone->data.cap.sub = snobol_ast_clone(node->data.cap.sub);
      break;

    case AST_SPAN:
    case AST_BREAK:
    case AST_ANY:
    case AST_NOTANY:
      clone->data.charclass.set =
          str_dup(node->data.charclass.set, node->data.charclass.len);
      clone->data.charclass.len = node->data.charclass.len;
      break;

    case AST_BREAKX:
      clone->data.breakx.set =
          str_dup(node->data.breakx.set, node->data.breakx.len);
      clone->data.breakx.len = node->data.breakx.len;
      break;

    case AST_LABEL:
      clone->data.label.name =
          str_dup(node->data.label.name, strlen(node->data.label.name));
      clone->data.label.target = snobol_ast_clone(node->data.label.target);
      break;

    case AST_GOTO:
      clone->data.goto_stmt.label = str_dup(node->data.goto_stmt.label,
                                            strlen(node->data.goto_stmt.label));
      break;

    case AST_TABLE_ACCESS:
      clone->data.table_access.table = str_dup(
          node->data.table_access.table, strlen(node->data.table_access.table));
      clone->data.table_access.key =
          snobol_ast_clone(node->data.table_access.key);
      break;

    case AST_TABLE_UPDATE:
      clone->data.table_update.table = str_dup(
          node->data.table_update.table, strlen(node->data.table_update.table));
      clone->data.table_update.key =
          snobol_ast_clone(node->data.table_update.key);
      clone->data.table_update.value =
          snobol_ast_clone(node->data.table_update.value);
      break;

    case AST_DYNAMIC_EVAL:
      clone->data.dynamic_eval.expr =
          snobol_ast_clone(node->data.dynamic_eval.expr);
      break;

    case AST_ASSIGN:
      clone->data.assign.var = node->data.assign.var;
      clone->data.assign.reg = node->data.assign.reg;
      break;

    case AST_LEN: clone->data.len.n = node->data.len.n; break;

    case AST_EVAL:
      clone->data.eval.fn = node->data.eval.fn;
      clone->data.eval.reg = node->data.eval.reg;
      break;

    case AST_ANCHOR: clone->data.anchor.atype = node->data.anchor.atype; break;

    case AST_BAL:
      clone->data.bal.open_cp = node->data.bal.open_cp;
      clone->data.bal.close_cp = node->data.bal.close_cp;
      break;

    case AST_RPOS:
    case AST_RTAB:
    case AST_POS:
    case AST_TAB: clone->data.rpos_rtab.n = node->data.rpos_rtab.n; break;

    case AST_FENCE:
    case AST_REM:
    case AST_ABORT:
    case AST_FAIL:
    case AST_SUCCEED: break;
  }

  return clone;
}

void snobol_ast_free(ast_node_t *node) {
  if (!node) {
    return;
  }

  /* Free children based on type */
  switch (node->type) {
    case AST_CONCAT:
      for (size_t i = 0; i < node->data.concat.count; i++) {
        snobol_ast_free(node->data.concat.parts[i]);
      }
      free(node->data.concat.parts);
      break;

    case AST_ALT:
      snobol_ast_free(node->data.alt.left);
      snobol_ast_free(node->data.alt.right);
      break;

    case AST_REPETITION:
    case AST_ARBNO: snobol_ast_free(node->data.repetition.sub); break;

    case AST_CAP: snobol_ast_free(node->data.cap.sub); break;

    case AST_SPAN:
    case AST_BREAK:
    case AST_ANY:
    case AST_NOTANY: free(node->data.charclass.set); break;

    case AST_LITERAL:
    case AST_EMIT: free(node->data.literal.text); break;

    case AST_LABEL:
      free(node->data.label.name);
      snobol_ast_free(node->data.label.target);
      break;

    case AST_GOTO: free(node->data.goto_stmt.label); break;

    case AST_TABLE_ACCESS:
      free(node->data.table_access.table);
      snobol_ast_free(node->data.table_access.key);
      break;

    case AST_TABLE_UPDATE:
      free(node->data.table_update.table);
      snobol_ast_free(node->data.table_update.key);
      snobol_ast_free(node->data.table_update.value);
      break;

    case AST_DYNAMIC_EVAL: snobol_ast_free(node->data.dynamic_eval.expr); break;

    case AST_BREAKX: free(node->data.breakx.set); break;

    case AST_BAL:
    case AST_FENCE:
    case AST_REM:
    case AST_RPOS:
    case AST_RTAB:
    case AST_POS:
    case AST_TAB:
    case AST_ABORT:
    case AST_FAIL:
    case AST_SUCCEED:
      /* no owned pointers */
      break;

    default:
      /* No children to free */
      break;
  }

  /* Arena-allocated nodes are reclaimed by resetting the arena, not freed. */
  if (!node->arena_allocated) {
    free(node);
  }
}

const char *snobol_ast_type_name(ast_type_t type) {
  switch (type) {
    case AST_LITERAL: return "LITERAL";
    case AST_CONCAT: return "CONCAT";
    case AST_ALT: return "ALT";
    case AST_REPETITION: return "REPETITION";
    case AST_SPAN: return "SPAN";
    case AST_BREAK: return "BREAK";
    case AST_ANY: return "ANY";
    case AST_NOTANY: return "NOTANY";
    case AST_ARBNO: return "ARBNO";
    case AST_CAP: return "CAP";
    case AST_ASSIGN: return "ASSIGN";
    case AST_LEN: return "LEN";
    case AST_EVAL: return "EVAL";
    case AST_DYNAMIC_EVAL: return "DYNAMIC_EVAL";
    case AST_ANCHOR: return "ANCHOR";
    case AST_EMIT: return "EMIT";
    case AST_LABEL: return "LABEL";
    case AST_GOTO: return "GOTO";
    case AST_TABLE_ACCESS: return "TABLE_ACCESS";
    case AST_TABLE_UPDATE: return "TABLE_UPDATE";
    case AST_BREAKX: return "BREAKX";
    case AST_BAL: return "BAL";
    case AST_FENCE: return "FENCE";
    case AST_REM: return "REM";
    case AST_RPOS: return "RPOS";
    case AST_RTAB: return "RTAB";
    case AST_POS: return "POS";
    case AST_TAB: return "TAB";
    case AST_ABORT: return "ABORT";
    case AST_FAIL: return "FAIL";
    case AST_SUCCEED: return "SUCCEED";
    default: return "UNKNOWN";
  }
}

void snobol_ast_dump(const ast_node_t *node, FILE *out, int indent) {
  if (!node) {
    fprintf(out, "%*s(NULL)\n", indent, "");
    return;
  }

  const char *type_name = snobol_ast_type_name(node->type);

  switch (node->type) {
    case AST_LITERAL:
      fprintf(out, "%*sLITERAL \"%.*s\"\n", indent, "",
              (int)node->data.literal.len, node->data.literal.text);
      break;

    case AST_CONCAT:
      fprintf(out, "%*sCONCAT[%zu]\n", indent, "", node->data.concat.count);
      for (size_t i = 0; i < node->data.concat.count; i++) {
        snobol_ast_dump(node->data.concat.parts[i], out, indent + 2);
      }
      break;

    case AST_ALT:
      fprintf(out, "%*sALT\n", indent, "");
      fprintf(out, "%*sLEFT:\n", indent + 2, "");
      snobol_ast_dump(node->data.alt.left, out, indent + 4);
      fprintf(out, "%*sRIGHT:\n", indent + 2, "");
      snobol_ast_dump(node->data.alt.right, out, indent + 4);
      break;

    case AST_ARBNO:
      fprintf(out, "%*sARBNO\n", indent, "");
      snobol_ast_dump(node->data.arbno.sub, out, indent + 2);
      break;

    case AST_CAP:
      fprintf(out, "%*sCAP(v%d)\n", indent, "", node->data.cap.reg);
      snobol_ast_dump(node->data.cap.sub, out, indent + 2);
      break;

    case AST_SPAN:
      fprintf(out, "%*sSPAN(\"%.*s\")\n", indent, "",
              (int)node->data.charclass.len, node->data.charclass.set);
      break;

    case AST_ANY:
      if (node->data.charclass.set) {
        fprintf(out, "%*sANY(\"%.*s\")\n", indent, "",
                (int)node->data.charclass.len, node->data.charclass.set);
      } else {
        fprintf(out, "%*sANY()\n", indent, "");
      }
      break;

    case AST_REPETITION:
      fprintf(out, "%*sREPEAT(%d,%d)\n", indent, "", node->data.repetition.min,
              node->data.repetition.max);
      snobol_ast_dump(node->data.repetition.sub, out, indent + 2);
      break;

    case AST_POS:
      fprintf(out, "%*sPOS(%d)\n", indent, "", node->data.rpos_rtab.n);
      break;

    case AST_TAB:
      fprintf(out, "%*sTAB(%d)\n", indent, "", node->data.rpos_rtab.n);
      break;

    default: fprintf(out, "%*s%s\n", indent, "", type_name); break;
  }
}

/**
 * Get AST version information
 */
snobol_ast_version_t snobol_ast_get_version(void) {
  snobol_ast_version_t version = {.major = SNOBOL_AST_VERSION_MAJOR,
                                  .minor = SNOBOL_AST_VERSION_MINOR,
                                  .patch = SNOBOL_AST_VERSION_PATCH,
                                  .string = SNOBOL_AST_VERSION_STRING};
  return version;
}

/**
 * Check if AST library version is compatible
 */
bool snobol_ast_version_check(uint16_t required_major,
                              uint16_t required_minor) {
  return SNOBOL_AST_VERSION_CHECK(required_major, required_minor);
}

/**
 * Get AST version as a string
 */
const char *snobol_ast_version_string(void) {
  return SNOBOL_AST_VERSION_STRING;
}

/* Additional AST creation functions for PHP binding */

ast_node_t *snobol_ast_create_literal(const char *text, size_t len) {
  return snobol_ast_create_lit(text, len);
}

ast_node_t *snobol_ast_create_break(const char *set, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_BREAK;
  node->data.charclass.set = str_dup(set, len);
  node->data.charclass.len = len;
  return node;
}

ast_node_t *snobol_ast_create_notany(const char *set, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_NOTANY;
  node->data.charclass.set = str_dup(set, len);
  node->data.charclass.len = len;
  return node;
}

ast_node_t *snobol_ast_create_assign(int var, int reg) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_ASSIGN;
  node->data.assign.var = var;
  node->data.assign.reg = reg;
  return node;
}

ast_node_t *snobol_ast_create_len(int32_t n) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_LEN;
  node->data.len.n = n;
  return node;
}

ast_node_t *snobol_ast_create_anchor(anchor_type_t atype) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_ANCHOR;
  node->data.anchor.atype = atype;
  return node;
}

ast_node_t *snobol_ast_create_emit(const char *text, size_t len, int reg) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_EMIT;
  node->data.emit.text = str_dup(text, len);
  node->data.emit.len = len;
  node->data.emit.reg = reg;
  return node;
}

ast_node_t *snobol_ast_create_dynamic_eval(ast_node_t *expr) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_DYNAMIC_EVAL;
  node->data.dynamic_eval.expr = expr;
  return node;
}

ast_node_t *snobol_ast_create_eval(int fn, int reg) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_EVAL;
  node->data.eval.fn = fn;
  node->data.eval.reg = reg;
  return node;
}

ast_node_t *snobol_ast_create_table_access(const char *table, ast_node_t *key) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_TABLE_ACCESS;
  node->data.table_access.table = str_dup(table, strlen(table));
  node->data.table_access.key = key;
  return node;
}

ast_node_t *snobol_ast_create_table_update(const char *table, ast_node_t *key,
                                           ast_node_t *value) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_TABLE_UPDATE;
  node->data.table_update.table = str_dup(table, strlen(table));
  node->data.table_update.key = key;
  node->data.table_update.value = value;
  return node;
}

ast_node_t *snobol_ast_create_breakx(const char *set, size_t len) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_BREAKX;
  node->data.breakx.set = str_dup(set, len);
  node->data.breakx.len = len;
  return node;
}

ast_node_t *snobol_ast_create_bal(uint32_t open_cp, uint32_t close_cp) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_BAL;
  node->data.bal.open_cp = open_cp;
  node->data.bal.close_cp = close_cp;
  return node;
}

ast_node_t *snobol_ast_create_fence(void) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_FENCE;
  return node;
}

ast_node_t *snobol_ast_create_rem(void) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_REM;
  return node;
}

ast_node_t *snobol_ast_create_rpos(int32_t n) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_RPOS;
  node->data.rpos_rtab.n = n;
  return node;
}

ast_node_t *snobol_ast_create_rtab(int32_t n) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_RTAB;
  node->data.rpos_rtab.n = n;
  return node;
}

ast_node_t *snobol_ast_create_pos(int32_t n) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_POS;
  node->data.rpos_rtab.n = n;
  return node;
}

ast_node_t *snobol_ast_create_tab(int32_t n) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_TAB;
  node->data.rpos_rtab.n = n;
  return node;
}

ast_node_t *snobol_ast_create_abort(void) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_ABORT;
  return node;
}

ast_node_t *snobol_ast_create_fail(void) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_FAIL;
  return node;
}

ast_node_t *snobol_ast_create_succeed(void) {
  ast_node_t *node = ast_node_alloc();
  if (!node) {
    return nullptr;
  }
  node->type = AST_SUCCEED;
  return node;
}
