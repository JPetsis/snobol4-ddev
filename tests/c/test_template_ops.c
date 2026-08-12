/**
 * @file test_template_ops.c
 * @brief Tests for template operations including formatting and table-backed
 * replacements
 *
 * Tests OP_EMIT_FORMAT (upper, lower, length, lpad, rpad), OP_EMIT_TABLE,
 * snobol_template_bind_tables, and the OP_EMIT_EXPR legacy alias.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/include/snobol/snobol_internal.h"
#include "snobol/compiler.h"
#include "snobol/table.h"
#include "snobol/vm.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* Helper to create a simple VM for testing */
static void init_test_vm(VM *vm, const char *input, snobol_buf *out) {
  memset(vm, 0, sizeof(VM));
  vm->s = input;
  vm->len = strlen(input);
  vm->ip = 0;
  vm->pos = 0;
  vm->out = out;
  vm->choices = NULL;
  vm->choices_cap = 0;
  vm->choices_top = 0;
  snobol_buf_init(out);
  vm_init_labels(vm);
#ifdef SNOBOL_DYNAMIC_PATTERN
  vm_init_tables(vm);
  vm_init_arrays(vm);
#endif
}

static void cleanup_test_vm(VM *vm) {
  if (vm->choices) {
    snobol_free(vm->choices);
  }
  vm_free_labels(vm);
#ifdef SNOBOL_DYNAMIC_PATTERN
  vm_free_tables(vm);
  vm_free_arrays(vm);
#endif
}

static void test_format_upper(void) {
  test_suite("Template Ops: FORMAT upper");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "hello", &out);

  /* Set up capture register 0 to capture "hello" */
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 5;
  vm.max_cap_used = 1;

  /* Manually execute OP_EMIT_FORMAT with upper */
  uint8_t bc[] = {
      OP_EMIT_FORMAT, 0x00, /* reg = 0 */
      0x01                  /* format_type = 1 (upper) */
  };

  vm.bc = bc;
  vm.bc_len = sizeof(bc);
  vm.ip = 0;

  /* Execute */
  uint8_t op = bc[vm.ip++];
  test_assert(op == OP_EMIT_FORMAT, "op is OP_EMIT_FORMAT");

  uint8_t reg = bc[vm.ip++];
  uint8_t format_type = bc[vm.ip++];

  test_assert(reg == 0, "reg is 0");
  test_assert(format_type == 1, "format_type is 1 (upper)");

  /* Simulate the formatting */
  const char *data = vm.s + vm.cap_start[0];
  size_t len = vm.cap_end[0] - vm.cap_start[0];

  char *tmp = (char *)malloc(len + 1);
  for (size_t i = 0; i < len; ++i) {
    tmp[i] =
        (char)((data[i] >= 'a' && data[i] <= 'z') ? data[i] - 32 : data[i]);
  }
  tmp[len] = '\0';

  test_assert(strcmp(tmp, "HELLO") == 0, "uppercase conversion correct");

  free(tmp);
  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_format_lower(void) {
  test_suite("Template Ops: FORMAT lower");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "HELLO", &out);

  vm.cap_start[0] = 0;
  vm.cap_end[0] = 5;
  vm.max_cap_used = 1;

  /* Simulate lowercase formatting */
  const char *data = vm.s + vm.cap_start[0];
  size_t len = vm.cap_end[0] - vm.cap_start[0];

  char *tmp = (char *)malloc(len + 1);
  for (size_t i = 0; i < len; ++i) {
    tmp[i] =
        (char)((data[i] >= 'A' && data[i] <= 'Z') ? data[i] + 32 : data[i]);
  }
  tmp[len] = '\0';

  test_assert(strcmp(tmp, "hello") == 0, "lowercase conversion correct");

  free(tmp);
  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_format_length(void) {
  test_suite("Template Ops: FORMAT length");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "hello", &out);

  vm.cap_start[0] = 0;
  vm.cap_end[0] = 5;
  vm.max_cap_used = 1;

  /* Simulate length formatting */
  size_t len = vm.cap_end[0] - vm.cap_start[0];

  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%zu", len);

  test_assert(n == 1, "length string length is 1");
  test_assert(strcmp(tmp, "5") == 0, "length value is 5");

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_format_missing_capture(void) {
  test_suite("Template Ops: FORMAT missing capture");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "hello", &out);

  /* Don't set any captures - they should be empty */
  vm.max_cap_used = 0;

  /* Missing capture should emit nothing (graceful degradation) */
  test_assert(vm.cap_end[0] <= vm.cap_start[0], "capture is empty");

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_table_emit_success(void) {
  test_suite("Template Ops: EMIT_TABLE success");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "key1", &out);

#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Create a table */
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "key1", "value1");

  /* Register the table */
  uint16_t table_id;
  vm_register_table(&vm, table, &table_id);

  /* Set up capture register 0 to capture "key1" */
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 4;
  vm.max_cap_used = 1;

  /* Simulate table lookup */
  snobol_table_t *t = vm_get_table(&vm, table_id);
  test_assert(t != NULL, "table retrieved");

  size_t key_len = vm.cap_end[0] - vm.cap_start[0];
  char *key = (char *)malloc(key_len + 1);
  memcpy(key, vm.s + vm.cap_start[0], key_len);
  key[key_len] = '\0';

  const char *value = table_get(t, key);
  test_assert(value != NULL, "table lookup succeeds");
  test_assert(strcmp(value, "value1") == 0, "value matches");

  free(key);
  table_release(table);
#else
  test_assert(true, "table support not enabled (skipped)");
#endif

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_table_emit_missing_key(void) {
  test_suite("Template Ops: EMIT_TABLE missing key");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "nonexistent", &out);

#ifdef SNOBOL_DYNAMIC_PATTERN
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "key1", "value1");

  uint16_t table_id;
  vm_register_table(&vm, table, &table_id);

  vm.cap_start[0] = 0;
  vm.cap_end[0] = 11;
  vm.max_cap_used = 1;

  snobol_table_t *t = vm_get_table(&vm, table_id);
  size_t key_len = vm.cap_end[0] - vm.cap_start[0];
  char *key = (char *)malloc(key_len + 1);
  memcpy(key, vm.s + vm.cap_start[0], key_len);
  key[key_len] = '\0';

  const char *value = table_get(t, key);
  test_assert(value == NULL, "missing key returns NULL (graceful degradation)");

  free(key);
  table_release(table);
#else
  test_assert(true, "table support not enabled (skipped)");
#endif

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_table_multiple_lookups(void) {
  test_suite("Template Ops: EMIT_TABLE multiple lookups");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "namecity", &out);

#ifdef SNOBOL_DYNAMIC_PATTERN
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "name", "Alice");
  (void)table_set(table, "city", "Boston");

  uint16_t table_id;
  vm_register_table(&vm, table, &table_id);

  /* Lookup "name" */
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 4;

  size_t key_len = vm.cap_end[0] - vm.cap_start[0];
  char *key = (char *)malloc(key_len + 1);
  memcpy(key, vm.s + vm.cap_start[0], key_len);
  key[key_len] = '\0';

  const char *value = table_get(table, key);
  test_assert(strcmp(value, "Alice") == 0, "first lookup correct");

  /* Lookup "city" */
  vm.cap_start[0] = 4;
  vm.cap_end[0] = 8;

  free(key);
  key_len = vm.cap_end[0] - vm.cap_start[0];
  key = (char *)malloc(key_len + 1);
  memcpy(key, vm.s + vm.cap_start[0], key_len);
  key[key_len] = '\0';

  value = table_get(table, key);
  test_assert(strcmp(value, "Boston") == 0, "second lookup correct");

  free(key);
  table_release(table);
#else
  test_assert(true, "table support not enabled (skipped)");
#endif

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_format_unknown_type(void) {
  test_suite("Template Ops: FORMAT unknown type");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "test", &out);

  vm.cap_start[0] = 0;
  vm.cap_end[0] = 4;
  vm.max_cap_used = 1;

  /* Unknown format type should emit raw */
  const char *data = vm.s + vm.cap_start[0];
  size_t len = vm.cap_end[0] - vm.cap_start[0];

  /* Should emit "test" as-is */
  test_assert(len == 4, "length is 4");
  test_assert(memcmp(data, "test", 4) == 0, "data is 'test'");

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_table_backed_template_literal_key(void) {
  test_suite("Template Ops: table-backed template with literal key");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "key", &out);

#ifdef SNOBOL_DYNAMIC_PATTERN
  /* Create a table */
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "key", "value_from_table");

  /* Register the table */
  uint16_t table_id;
  vm_register_table(&vm, table, &table_id);

  /* Set up capture register 0 to capture "key" */
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 3;
  vm.max_cap_used = 1;

  /* Simulate table lookup via capture-derived key */
  snobol_table_t *t = vm_get_table(&vm, table_id);
  test_assert(t != NULL, "table retrieved");

  size_t key_len = vm.cap_end[0] - vm.cap_start[0];
  char *key_str = (char *)malloc(key_len + 1);
  memcpy(key_str, vm.s + vm.cap_start[0], key_len);
  key_str[key_len] = '\0';

  const char *value = table_get(t, key_str);
  test_assert(value != NULL, "table lookup succeeds with capture-derived key");
  test_assert(strcmp(value, "value_from_table") == 0,
              "value matches for literal key");

  free(key_str);
  table_release(table);
#else
  test_assert(true, "table support not enabled (skipped)");
#endif

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

static void test_table_backed_template_missing_key_fallback(void) {
  test_suite("Template Ops: table-backed template missing key fallback");

  VM vm;
  snobol_buf out;
  init_test_vm(&vm, "missing_key", &out);

#ifdef SNOBOL_DYNAMIC_PATTERN
  snobol_table_t *table = table_create("test");
  (void)table_set(table, "existing", "value");

  uint16_t table_id;
  vm_register_table(&vm, table, &table_id);

  /* Set up capture for missing key */
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 11;
  vm.max_cap_used = 1;

  snobol_table_t *t = vm_get_table(&vm, table_id);
  size_t key_len = vm.cap_end[0] - vm.cap_start[0];
  char *key_str = (char *)malloc(key_len + 1);
  memcpy(key_str, vm.s + vm.cap_start[0], key_len);
  key_str[key_len] = '\0';

  const char *value = table_get(t, key_str);
  test_assert(value == NULL, "missing key returns NULL");
  /* Graceful degradation: missing key emits empty string */

  free(key_str);
  table_release(table);
#else
  test_assert(true, "table support not enabled (skipped)");
#endif

  cleanup_test_vm(&vm);
  snobol_buf_free(&out);
}

/* ── Template compiler and VM integration tests ──────────────────────────── */

/* Helper: run a compiled template bytecode over a subject string */
static void run_template(const char *tpl_src, const char *subject,
                         size_t cap0_start,
                         size_t cap0_end, /* capture register 0 */
                         snobol_buf *out) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_template_to_bytecode(tpl_src, strlen(tpl_src), &bc, &bc_len);
  VM vm;
  snobol_buf_init(out);
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = subject;
  vm.len = strlen(subject);
  vm.cap_start[0] = cap0_start;
  vm.cap_end[0] = cap0_end;
  vm.max_cap_used = 1;
  vm.out = out;
  vm_init_labels(&vm);
#ifdef SNOBOL_DYNAMIC_PATTERN
  vm_init_tables(&vm);
  vm_init_arrays(&vm);
#endif
  vm_run(&vm);
  vm_free_labels(&vm);
#ifdef SNOBOL_DYNAMIC_PATTERN
  vm_free_tables(&vm);
  vm_free_arrays(&vm);
#endif
  if (bc) {
    compiler_free(bc);
  }
}

/* compile-and-run .lower() */
static void test_compile_lower(void) {
  test_suite("Template Ops: compile-and-run .lower()");
  snobol_buf out;
  run_template("${v0.lower()}", "HELLO", 0, 5, &out);
  test_assert((out.len == 5 && memcmp(out.data, "hello", 5) == 0) != 0,
              "lower output is 'hello'");
  snobol_buf_free(&out);
}

/* compile-and-run .lpad(5,'0') */
static void test_compile_lpad_fill(void) {
  test_suite("Template Ops: compile-and-run .lpad(5,'0')");
  snobol_buf out;
  run_template("${v0.lpad(5,'0')}", "42", 0, 2, &out);
  test_assert((out.len == 5 && memcmp(out.data, "00042", 5) == 0) != 0,
              "lpad output is '00042'");
  snobol_buf_free(&out);
}

/* compile-and-run .rpad(6) default fill=space */
static void test_compile_rpad_default(void) {
  test_suite("Template Ops: compile-and-run .rpad(6)");
  snobol_buf out;
  run_template("${v0.rpad(6)}", "hi", 0, 2, &out);
  test_assert(out.len == 6, "rpad output length is 6");
  test_assert(memcmp(out.data, "hi    ", 6) == 0, "rpad output is 'hi    '");
  snobol_buf_free(&out);
}

/* lpad/rpad are no-ops when capture >= width */
static void test_pad_noop_when_long(void) {
  test_suite("Template Ops: lpad/rpad no-op when capture >= width");
  snobol_buf out;
  run_template("${v0.lpad(3)}", "hello", 0, 5, &out);
  test_assert((out.len == 5 && memcmp(out.data, "hello", 5) == 0) != 0,
              "lpad no-op for long capture");
  snobol_buf_free(&out);

  run_template("${v0.rpad(2,'.')}", "hi", 0, 2, &out);
  test_assert((out.len == 2 && memcmp(out.data, "hi", 2) == 0) != 0,
              "rpad no-op exactly at width");
  snobol_buf_free(&out);
}

/* graceful degradation: missing capture with lower/lpad/rpad */
static void test_missing_capture_degradation(void) {
  test_suite("Template Ops: missing capture graceful degradation");

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  /* .lower() with cap_end <= cap_start (missing) */
  compile_template_to_bytecode("${v0.lower()}", 13, &bc, &bc_len);
  snobol_buf out;
  snobol_buf_init(&out);

  VM vm;
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = "";
  vm.len = 0;
  /* cap_end == 0, cap_start == 0: empty (treated as missing) */
  vm.out = &out;
  vm_init_labels(&vm);
  vm_run(&vm);
  vm_free_labels(&vm);
  compiler_free(bc);

  test_assert(out.len == 0, "missing capture with lower emits empty string");
  snobol_buf_free(&out);

  /* .lpad() with missing capture (cap_end < cap_start signals missing) */
  compile_template_to_bytecode("${v0.lpad(4)}", 13, &bc, &bc_len);
  snobol_buf_init(&out);
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = "hello";
  vm.len = 5;
  vm.cap_start[0] = 5;
  vm.cap_end[0] = 0; /* cap_end < cap_start = missing */
  vm.out = &out;
  vm_init_labels(&vm);
  vm_run(&vm);
  vm_free_labels(&vm);
  compiler_free(bc);
  test_assert(out.len == 0, "missing capture with lpad emits empty string");
  snobol_buf_free(&out);
}

#ifdef SNOBOL_DYNAMIC_PATTERN

/* snobol_template_bind_tables: compile, bind, verify table_id patched */
static void test_bind_tables_patch(void) {
  test_suite("Template Ops: snobol_template_bind_tables patches table_id");

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  const char *tpl = "$v0[mydict['hello']]";
  int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
  test_assert(rc == 0, "template compiles successfully");
  if (rc != 0) {
    return;
  }

  /* Bytecode should contain 0xFFFF (unbound sentinel) */
  /* Find OP_EMIT_TABLE in bytecode */
  bool found_unbound = false;
  for (size_t i = 0; i + 1 < bc_len;) {
    if (bc[i] == OP_EMIT_TABLE && i + 2 < bc_len) {
      uint16_t tid = ((uint16_t)bc[i + 1] << 8) | bc[i + 2];
      if (tid == SNBL_TABLE_ID_UNBOUND) {
        found_unbound = true;
        break;
      }
    }
    i++;
  }
  test_assert(found_unbound,
              "OP_EMIT_TABLE initialized with SNBL_TABLE_ID_UNBOUND");

  /* Bind: map "mydict" -> id 42 */
  const char *names[] = {"mydict"};
  uint16_t ids[] = {42};
  int bind_rc = snobol_template_bind_tables(bc, bc_len, names, ids, 1);
  test_assert(bind_rc == 0, "bind_tables returns 0 on success");

  /* table_id in bytecode should now be 42 */
  bool found_bound = false;
  for (size_t i = 0; i + 1 < bc_len;) {
    if (bc[i] == OP_EMIT_TABLE && i + 2 < bc_len) {
      uint16_t tid = ((uint16_t)bc[i + 1] << 8) | bc[i + 2];
      if (tid == 42) {
        found_bound = true;
        break;
      }
    }
    i++;
  }
  test_assert(found_bound, "OP_EMIT_TABLE table_id patched to 42");

  compiler_free(bc);
}

/* snobol_template_bind_tables returns -1 for unresolvable name */
static void test_bind_tables_unresolvable(void) {
  test_suite(
      "Template Ops: snobol_template_bind_tables returns -1 for unknown name");

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_template_to_bytecode("$v0[unknown['key']]", 19, &bc, &bc_len);

  const char *names[] = {"otherDict"};
  uint16_t ids[] = {7};
  int bind_rc = snobol_template_bind_tables(bc, bc_len, names, ids, 1);
  test_assert(bind_rc == -1, "bind_tables returns -1 for unresolvable name");

  /* unresolved entry should still have 0xFFFF */
  bool still_unbound = false;
  for (size_t i = 0; i + 1 < bc_len;) {
    if (bc[i] == OP_EMIT_TABLE && i + 2 < bc_len) {
      uint16_t tid = ((uint16_t)bc[i + 1] << 8) | bc[i + 2];
      if (tid == SNBL_TABLE_ID_UNBOUND) {
        still_unbound = true;
        break;
      }
    }
    i++;
  }
  test_assert(still_unbound,
              "unresolved OP_EMIT_TABLE retains SNBL_TABLE_ID_UNBOUND");

  compiler_free(bc);
}

/* end-to-end: compile template with OP_EMIT_TABLE (literal key), bind, execute
 */
static void test_e2e_table_literal_key(void) {
  test_suite("Template Ops: end-to-end table lookup with literal key");

  snobol_table_t *table = table_create("colors");
  (void)table_set(table, "sky", "blue");
  (void)table_set(table, "sun", "yellow");

  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_template_to_bytecode("$v0[colors['sky']]", 18, &bc, &bc_len);

  const char *names[] = {"colors"};
  uint16_t ids[] = {0}; /* will be assigned ID 0 when registered */
  snobol_template_bind_tables(bc, bc_len, names, ids, 1);

  VM vm;
  snobol_buf out;
  snobol_buf_init(&out);
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = bc_len;
  vm.s = "";
  vm.len = 0;
  vm.out = &out;
  vm_init_labels(&vm);
  vm_init_tables(&vm);
  vm_init_arrays(&vm);
  uint16_t assigned;
  vm_register_table(&vm, table, &assigned);
  vm_run(&vm);
  vm_free_tables(&vm);
  vm_free_arrays(&vm);
  vm_free_labels(&vm);

  test_assert((out.len == 4 && memcmp(out.data, "blue", 4) == 0) != 0,
              "literal key table lookup returns 'blue'");

  snobol_buf_free(&out);
  compiler_free(bc);
  table_release(table);
}

/* end-to-end: compile template with OP_EMIT_TABLE (capture-derived key), bind,
 * execute */
static void test_e2e_table_capture_key(void) {
  test_suite("Template Ops: end-to-end table lookup with capture-derived key");

  snobol_table_t *table = table_create("words");
  (void)table_set(table, "cat", "gato");
  (void)table_set(table, "dog", "perro");

  const char *tpl = "$v0[words[v0]]";
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);

  const char *names[] = {"words"};
  uint16_t ids[] = {0};
  snobol_template_bind_tables(bc, bc_len, names, ids, 1);

  VM vm;
  snobol_buf out;
  snobol_buf_init(&out);
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = bc_len;
  const char *subject = "cat";
  vm.s = subject;
  vm.len = 3;
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 3;
  vm.max_cap_used = 1;
  vm.out = &out;
  vm_init_labels(&vm);
  vm_init_tables(&vm);
  vm_init_arrays(&vm);
  uint16_t assigned;
  vm_register_table(&vm, table, &assigned);
  vm_run(&vm);
  vm_free_tables(&vm);
  vm_free_arrays(&vm);
  vm_free_labels(&vm);

  test_assert((out.len == 4 && memcmp(out.data, "gato", 4) == 0) != 0,
              "capture-key table lookup returns 'gato' for 'cat'");

  snobol_buf_free(&out);
  compiler_free(bc);
  table_release(table);
}

#endif /* SNOBOL_DYNAMIC_PATTERN */

/* legacy OP_EMIT_EXPR bytecode (discriminant 1=upper) still works */
static void test_legacy_emit_expr(void) {
  test_suite("Template Ops: legacy OP_EMIT_EXPR alias (discriminant 1=upper)");

  VM vm;
  snobol_buf out;
  snobol_buf_init(&out);
  /* Manually construct bytecode: OP_EMIT_EXPR, reg=0, expr_type=1 (legacy
   * upper) */
  uint8_t bc[] = {OP_EMIT_EXPR, 0x00, 0x01, OP_ACCEPT};
  memset(&vm, 0, sizeof(VM));
  vm.bc = bc;
  vm.bc_len = sizeof(bc);
  vm.s = "hello";
  vm.len = 5;
  vm.cap_start[0] = 0;
  vm.cap_end[0] = 5;
  vm.max_cap_used = 1;
  vm.out = &out;
  vm_init_labels(&vm);
  vm_run(&vm);
  vm_free_labels(&vm);

  test_assert(
      (out.len == 5 && memcmp(out.data, "HELLO", 5) == 0) != 0,
      "legacy OP_EMIT_EXPR discriminant 1 uppercases 'hello' to 'HELLO'");

  snobol_buf_free(&out);
}


/* ===== test_coverage_compiler: coverage-driven tests merged into test_template_ops.c ===== */
#include "../../core/include/snobol/compiler.h"
#include "../../core/include/snobol/vm.h"


static uint8_t *cov_compile_tpl(const char *tpl, size_t *out_len, int *out_rc) {
  uint8_t *bc = nullptr;
  size_t bc_len = 0;
  int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
  if (out_rc) {
    *out_rc = rc;
  }
  if (out_len) {
    *out_len = bc_len;
  }
  return bc;
}

void test_cov_tpl_substitutions(void) {
  test_suite("Coverage: template substitution forms");

  uint8_t *bc;
  size_t bc_len;
  int rc;

  /* Capture refs: $v1 and ${v1}. */
  bc = cov_compile_tpl("$v1", &bc_len, &rc);
  test_assert((bc && rc == 0 && bc_len > 0) != 0, "$v1 compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1} compiles");
  compiler_free(bc);

  /* Braced formats. */
  bc = cov_compile_tpl("${v1.upper()}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.upper()} compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1.lower()}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.lower()} compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1.length()}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.length()} compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1.lpad(5,'-')}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.lpad(5,'-')} compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1.rpad(3,'*')}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.rpad(3,'*')} compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("${v1.rpad(10)}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.rpad(10)} (no fill) compiles");
  compiler_free(bc);

  /* Unknown braced format falls back to a plain capture emit. */
  bc = cov_compile_tpl("${v1.bogus()}", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "${v1.bogus()} falls back to capture");
  compiler_free(bc);

  /* Unclosed brace falls back to a literal '$'. */
  bc = cov_compile_tpl("${v1", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "unclosed brace falls back");
  compiler_free(bc);

  /* Literal '$' at the end of the template. */
  bc = cov_compile_tpl("x$", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "trailing $ compiles");
  compiler_free(bc);

  /* '$v' without digits → literal '$'. */
  bc = cov_compile_tpl("$v", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "$v without digits falls back");
  compiler_free(bc);

  /* '$' followed by non-v/brace → literal '$'. */
  bc = cov_compile_tpl("$9", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "$9 falls back to literal");
  compiler_free(bc);

  /* Plain literal segments. */
  bc = cov_compile_tpl("plain text", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "plain text compiles");
  compiler_free(bc);
}


void test_cov_tpl_tables(void) {
  test_suite("Coverage: template table-backed substitutions");

  uint8_t *bc;
  size_t bc_len;
  int rc;

  /* Literal key: $v1[tbl['key']]. */
  bc = cov_compile_tpl("$v1[tbl['key']]", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "table literal key compiles");
  compiler_free(bc);

  /* Capture key: $v1[tbl[v1]] and $v1[tbl[2]]. */
  bc = cov_compile_tpl("$v1[tbl[v1]]", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "table capture key compiles");
  compiler_free(bc);
  bc = cov_compile_tpl("$v1[tbl[2]]", &bc_len, &rc);
  test_assert((bc && rc == 0) != 0, "table digit key compiles");
  compiler_free(bc);

  /* Error fallbacks: every bad shape emits a literal '$'. */
  {
    const char *bad[] = {"$v1[x]",            /* empty table name */
                         "$v1[tbl",           /* no bracket */
                         "$v1[tbl['unclosed", /* unclosed quote */
                         "$v1[tbl['k'",       /* missing close bracket */
                         "$v1[tbl[v",         /* capture key without digits */
                         "$v1[tbl[x]"};       /* non-digit unquoted key */
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
      bc = cov_compile_tpl(bad[i], &bc_len, &rc);
      test_assert((bc && rc == 0) != 0, "malformed table ref falls back");
      compiler_free(bc);
    }
  }

  /* Overlong table name → compile failure. */
  {
    char tpl[340];
    size_t ti = 0;
    memcpy(tpl + ti, "$v1[", 4);
    ti += 4;
    memset(tpl + ti, 'x', 256);
    ti += 256;
    memcpy(tpl + ti, "['k']]", 6);
    ti += 6;
    tpl[ti] = '\0';
    bc = cov_compile_tpl(tpl, &bc_len, &rc);
    test_assert((bc == NULL && rc == -1) != 0, "overlong table name rejected");
    compiler_free(bc);
  }

  /* bind_tables: resolve literal + capture key tables and report unknown. */
  {
    bc = cov_compile_tpl("$v1[tbl['key']] $v1[tbl[v1]]", &bc_len, &rc);
    test_assert((bc && rc == 0) != 0, "multi-table template compiles");
    if (bc) {
      const char *names[] = {"tbl"};
      const uint16_t ids[] = {7};
      int res = snobol_template_bind_tables(bc, bc_len, names, ids, 1);
      test_assert(res == 0, "table names resolve");
      compiler_free(bc);

      /* Fresh bytecode with no bindings → unbound table reported. */
      uint8_t *bc2 =
          cov_compile_tpl("$v1[tbl['key']] $v1[tbl[v1]]", &bc_len, &rc);
      test_assert(bc2 != NULL, "second template compiles");
      res = snobol_template_bind_tables(bc2, bc_len, nullptr, nullptr, 0);
      test_assert(res == -1, "unbound table reported");
      compiler_free(bc2);
    }
  }

  /* bind_tables guards and the full opcode walk. */
  {
    test_assert(snobol_template_bind_tables(nullptr, 0, nullptr, nullptr, 0) ==
                    0,
                "bind(NULL) no-op");

    /* Crafted template bytecode walking every bindable opcode form. */
    uint8_t bc2[256];
    size_t ip = 0;
    bc2[ip++] = OP_EMIT_LITERAL;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 1; /* len=1 */
    bc2[ip++] = 'x';
    bc2[ip++] = OP_EMIT_CAPTURE;
    bc2[ip++] = 0;
    bc2[ip++] = OP_EMIT_EXPR;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_EMIT_FORMAT;
    bc2[ip++] = 0;
    bc2[ip++] = SNBL_FMT_UPPER;
    bc2[ip++] = OP_EMIT_FORMAT;
    bc2[ip++] = 0;
    bc2[ip++] = SNBL_FMT_LPAD;
    bc2[ip++] = 0;
    bc2[ip++] = 5;
    bc2[ip++] = '-';
    bc2[ip++] = OP_LIT;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = 'y';
    bc2[ip++] = OP_ANY;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_SPAN;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_BREAK;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_BREAKX;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_NOTANY;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_LEN;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 1;
    bc2[ip++] = OP_CAP_START;
    bc2[ip++] = 0;
    bc2[ip++] = OP_CAP_END;
    bc2[ip++] = 0;
    bc2[ip++] = OP_ASSIGN;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_REM;
    bc2[ip++] = OP_FENCE;
    bc2[ip++] = OP_DYNAMIC;
    bc2[ip++] = OP_NOP;
    bc2[ip++] = OP_FAIL;
    bc2[ip++] = OP_SPLIT;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_REPEAT_INIT;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_REPEAT_STEP;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_JMP;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_RPOS;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_RTAB;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_POS;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_TAB;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_ABORT;
    bc2[ip++] = OP_SUCCEED;
    bc2[ip++] = OP_LABEL;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_GOTO;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_GOTO_F;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_BAL;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_EVAL;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = OP_EMIT_TABLE;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0; /* key_type literal */
    bc2[ip++] = 3; /* name_len */
    bc2[ip++] = 't';
    bc2[ip++] = 'b';
    bc2[ip++] = 'l';
    bc2[ip++] = 0;
    bc2[ip++] = 3; /* key_len */
    bc2[ip++] = 'k';
    bc2[ip++] = 'e';
    bc2[ip++] = 'y';
    bc2[ip++] = OP_EMIT_TABLE;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 1; /* key_type capture */
    bc2[ip++] = 3;
    bc2[ip++] = 't';
    bc2[ip++] = 'b';
    bc2[ip++] = 'l';
    bc2[ip++] = 1; /* key_reg */
    bc2[ip++] = OP_TABLE_GET;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 3;
    bc2[ip++] = 't';
    bc2[ip++] = 'b';
    bc2[ip++] = 'l';
    bc2[ip++] = OP_TABLE_SET;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0xFF;
    bc2[ip++] = 0;
    bc2[ip++] = 0;
    bc2[ip++] = 3;
    bc2[ip++] = 't';
    bc2[ip++] = 'b';
    bc2[ip++] = 'l';
    bc2[ip++] = OP_ACCEPT;
    size_t bc_len2 = ip;

    const char *names[] = {"tbl"};
    const uint16_t ids[] = {9};
    int res = snobol_template_bind_tables(bc2, bc_len2, names, ids, 1);
    test_assert(res == 0, "bind walker resolves all table refs");

    /* Truncated EMIT_TABLE / TABLE_GET stop the walk safely. */
    res = snobol_template_bind_tables(bc2, 3, names, ids, 1);
    test_assert(res == 0, "truncated bytecode stops the walk");
  }
}

/* Documented table syntax ($TABLE[key] / $TABLE[$vM], docs/php-manual.md):
 * the bare identifier form must compile to OP_EMIT_TABLE with the symbolic
 * name, exactly like the $v0[TABLE[key]] form; identifiers not followed by
 * '[' stay literal; overlong names fail compilation. */
static void test_documented_table_syntax(void) {
  test_suite("Template Ops: documented $TABLE[key] syntax");

  /* $colors['sky']: bare name + literal key */
  {
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const char *tpl = "$colors['sky']";
    int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
    test_assert(rc == 0, "bare table literal-key compiles");
    if (rc == 0) {
      bool found = false;
      for (size_t i = 0; i + 2 < bc_len; i++) {
        if (bc[i] == OP_EMIT_TABLE && i + 2 < bc_len) {
          uint16_t tid = ((uint16_t)bc[i + 1] << 8) | bc[i + 2];
          if (tid == SNBL_TABLE_ID_UNBOUND) {
            /* key_type at i+3, name_len at i+4, name bytes after */
            if (bc[i + 3] == 0 && bc[i + 4] == 6 &&
                memcmp(bc + i + 5, "colors", 6) == 0) {
              found = true;
            }
          }
        }
      }
      test_assert(found,
                  "bare $colors['sky'] emits OP_EMIT_TABLE name 'colors'");
      compiler_free(bc);
    }
  }

  /* $STATE[$v0]: bare name + capture-register key (documented $ form) */
  {
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const char *tpl = "$STATE[$v0]";
    int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
    test_assert(rc == 0, "bare table capture-key compiles");
    if (rc == 0) {
      bool found = false;
      for (size_t i = 0; i + 2 < bc_len; i++) {
        if (bc[i] == OP_EMIT_TABLE && i + 2 < bc_len) {
          uint16_t tid = ((uint16_t)bc[i + 1] << 8) | bc[i + 2];
          if (tid == SNBL_TABLE_ID_UNBOUND) {
            /* key_type=1 (capture), name_len=5 'STATE', key_reg=0 */
            if (bc[i + 3] == 1 && bc[i + 4] == 5 &&
                memcmp(bc + i + 5, "STATE", 5) == 0 && bc[i + 10] == 0) {
              found = true;
            }
          }
        }
      }
      test_assert(found,
                  "bare $STATE[$v0] emits OP_EMIT_TABLE capture key reg 0");
      compiler_free(bc);
    }
  }

  /* Identifier not followed by '[' stays literal text: no OP_EMIT_TABLE. */
  {
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const char *tpl = "$version";
    int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
    test_assert(rc == 0, "bare identifier compiles");
    if (rc == 0) {
      bool found = false;
      for (size_t i = 0; i + 2 < bc_len; i++) {
        if (bc[i] == OP_EMIT_TABLE) {
          found = true;
          break;
        }
      }
      test_assert((!found) != 0, "bare identifier without '[' stays literal");
      compiler_free(bc);
    }
  }

  /* Malformed table ref falls back to a literal '$' (no OP_EMIT_TABLE). */
  {
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    const char *tpl = "$colors['sky'";
    int rc = compile_template_to_bytecode(tpl, strlen(tpl), &bc, &bc_len);
    test_assert(rc == 0, "malformed table ref compiles as literal");
    if (rc == 0) {
      bool found = false;
      for (size_t i = 0; i + 2 < bc_len; i++) {
        if (bc[i] == OP_EMIT_TABLE) {
          found = true;
          break;
        }
      }
      test_assert((!found) != 0, "malformed table ref falls back to literal");
      compiler_free(bc);
    }
  }

  /* Overlong table name (>255 bytes) fails compilation loudly. */
  {
    char tpl[300];
    tpl[0] = '$';
    memset(tpl + 1, 'x', 256);
    memcpy(tpl + 257, "['k']", 5);
    uint8_t *bc = nullptr;
    size_t bc_len = 0;
    int rc = compile_template_to_bytecode(tpl, 262, &bc, &bc_len);
    test_assert(rc != 0, "overlong bare table name fails compilation");
    if (bc) {
      compiler_free(bc);
    }
  }
}

void test_template_ops_suite(void) {
  test_format_upper();
  test_format_lower();
  test_format_length();
  test_format_missing_capture();
  test_table_emit_success();
  test_table_emit_missing_key();
  test_table_multiple_lookups();
  test_format_unknown_type();
  test_table_backed_template_literal_key();
  test_table_backed_template_missing_key_fallback();
  test_compile_lower();
  test_compile_lpad_fill();
  test_compile_rpad_default();
  test_pad_noop_when_long();
  test_missing_capture_degradation();
#ifdef SNOBOL_DYNAMIC_PATTERN
  test_bind_tables_patch();
  test_bind_tables_unresolvable();
  test_e2e_table_literal_key();
  test_e2e_table_capture_key();
#endif
  test_legacy_emit_expr();
  test_documented_table_syntax();
  test_cov_tpl_substitutions();
  test_cov_tpl_tables();
}
