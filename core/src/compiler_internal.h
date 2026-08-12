#pragma once

/*
 * compiler_internal.h — shared (non-public) infrastructure for the compiler
 * translation units.
 *
 * The compiler is split across several .c files:
 *   - compiler_analysis.c : CodeBuf, charclass table, SPLIT/ANY fusion pass
 *   - compiler_codegen.c  : C-AST → bytecode emission + label table
 *   - compiler.c          : PHP zval AST emission, template compilation,
 *                           table binding, public entry points
 *
 * Helpers that are needed by more than one of those files are declared here
 * with external linkage (promoted from their original file-local `static`
 * scope, per the TU-modularization design).  No public API signature changes.
 */

#include "snobol/compiler.h"
#include "snobol/snobol_internal.h"
#include "snobol/unicode_fold.h"
#include "snobol/vm.h" /* MUST come before snobol/compiler.h to get CHARCLASS_BITMAP_BYTES */
#include "snobol/ast.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Minimal dynamic code buffer (shared)
 * --------------------------------------------------------------------------- */
typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t len;
} CodeBuf;

void cb_init(CodeBuf *c);
void cb_free(CodeBuf *c);
void cb_ensure(CodeBuf *c, size_t need);
size_t cb_pos(CodeBuf *c);
void cb_emit_u8(CodeBuf *c, uint8_t v);
void cb_emit_u16(CodeBuf *c, uint16_t v);
void cb_emit_u32(CodeBuf *c, uint32_t v);
void cb_emit_bytes(CodeBuf *c, const uint8_t *b, size_t n);

/* ---------------------------------------------------------------------------
 * Charclass table handling (shared)
 * --------------------------------------------------------------------------- */
typedef struct cc_entry {
  CpRange *ranges;
  uint16_t range_count;
  uint16_t range_cap;
  uint16_t case_insensitive;
  struct cc_entry *next;
} CCEntry;

extern CCEntry *charclass_head;
extern uint32_t charclass_count;
extern uint8_t next_loop_id;
extern bool compiler_case_insensitive;

/* Nullable (empty-matchable) analysis for zero-width-loop bounding (W2b). */
bool ast_node_nullable(const ast_node_t *node);

/* Diagnostic sink (W2b): prints @p msg to stderr only when SNOBOL_DIAG is set
 * in the environment, so zero-width-loop bounding can be observed without
 * polluting normal output. */
void snobol_diag(const char *msg);

void free_charclass_list(void);
int add_or_get_charclass(const char *s, size_t len);
void add_range(CCEntry *e, uint32_t start, uint32_t end);
void normalize_ranges(CCEntry *e);
int compare_ranges(const void *a, const void *b);

/* ---------------------------------------------------------------------------
 * SPLIT/ANY fusion pass (shared)
 * --------------------------------------------------------------------------- */
uint32_t fuse_read_u32(const uint8_t *bc, size_t off);
uint16_t fuse_read_u16(const uint8_t *bc, size_t off);
CCEntry *get_cc_entry(uint32_t id);
int fuse_add_union_cc(CCEntry *ea, uint32_t cp_a, CCEntry *eb, uint32_t cp_b,
                      uint8_t ci);
void snobol_bc_fuse_split_any(CodeBuf *cb);

/* Emit literal inline: OP_LIT offset len bytes... (shared) */
int emit_lit_bytes(CodeBuf *c, const char *s, size_t len);

/* Magic sentinel marking the label-table bytecode extension (shared). */
#ifndef SNOBOL_LABEL_TABLE_MAGIC_DEFINED
#define SNOBOL_LABEL_TABLE_MAGIC_DEFINED
enum { SNOBOL_LABEL_TABLE_MAGIC = 0x534E424CU };
#endif
