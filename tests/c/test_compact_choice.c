/*
 * test_compact_choice.c - Compact choice stack and write-log mechanism tests
 *
 * Tests cover:
 *   - Write-log entry tracking and deduplication
 *   - Compact choice record size calculation
 *   - Mode consistency (legacy vs compact) for correctness
 *   - Memory footprint reduction for patterns with many choice points
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <stdlib.h> /* _putenv_s */
static int setenv(const char *name, const char *value, int overwrite) {
  (void)overwrite;
  return _putenv_s(name, value);
}
static int unsetenv(const char *name) {
  return _putenv_s(name, "");
}
#else
#include <unistd.h> /* setenv/unsetenv */
#endif

#include "snobol/vm.h"

/* External test framework functions */
extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

/* ── Minimal VM builder (mirrors test_backtracking.c) ────────────────────── */

static VM make_vm(const uint8_t *bc, size_t bc_len, const char *subject) {
  VM vm;
  memset(&vm, 0, sizeof(vm));

  vm.bc = bc;
  vm.bc_len = bc_len;

  vm.s = subject;
  vm.len = strlen(subject);

  vm.ip = 0;
  vm.pos = 0;
  vm.var_count = 0;

  return vm;
}

static bool cap_equals(const VM *vm, uint8_t reg, const char *subject,
                       const char *expected) {
  if (reg >= MAX_CAPS) {
    return false;
  }
  size_t a = vm->cap_start[reg];
  size_t b = vm->cap_end[reg];

  if (expected == NULL) {
    return (a == 0 && b == 0) != 0;
  }

  if (b < a) {
    return false;
  }
  size_t n = b - a;
  return (strlen(expected) == n && memcmp(subject + a, expected, n) == 0) != 0;
}

/* ── Bytecode builder helpers ───────────────────────────────────────────── */

typedef struct {
  uint8_t bc[1024];
  size_t ip;
  size_t lit_pool;
} Bc;

static void bc_init(Bc *b) {
  memset(b, 0, sizeof(*b));
  b->ip = 0;
  b->lit_pool = 256;
}

static uint32_t bc_add_lit(Bc *b, const char *bytes, size_t len) {
  uint32_t off = (uint32_t)b->lit_pool;
  memcpy(b->bc + b->lit_pool, bytes, len);
  b->lit_pool += len;
  return off;
}

static void bc_emit_u8(Bc *b, uint8_t v) {
  b->bc[b->ip++] = v;
}
static void bc_emit_u16(Bc *b, uint16_t v) {
  b->bc[b->ip++] = (uint8_t)((v >> 8) & 0xFF);
  b->bc[b->ip++] = (uint8_t)(v & 0xFF);
}
static void bc_emit_u32(Bc *b, uint32_t v) {
  b->bc[b->ip++] = (uint8_t)((v >> 24) & 0xFF);
  b->bc[b->ip++] = (uint8_t)((v >> 16) & 0xFF);
  b->bc[b->ip++] = (uint8_t)((v >> 8) & 0xFF);
  b->bc[b->ip++] = (uint8_t)(v & 0xFF);
}

static void bc_emit_lit1(Bc *b, char c) {
  uint32_t off = bc_add_lit(b, &c, 1);
  bc_emit_u8(b, OP_LIT);
  bc_emit_u32(b, off);
  bc_emit_u32(b, 1);
}

/* ── Write-log unit tests ───────────────────────────────────────────────── */

static void test_write_log_tracking(void) {
  test_suite("Write-Log Tracking");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.use_compact_choice = true;
  vm_write_log_init(&vm);
  vm.write_log_cap = 64;
  vm.write_log = malloc(vm.write_log_cap * sizeof(WriteLogEntry));

  // Initially empty
  test_assert(vm_write_log_count_entries(&vm) == 0, "Write-log starts empty");

  // Track cap_start for cap 0
  vm_write_log_track_cap_start(&vm, 0, 100);
  test_assert(vm_write_log_count_entries(&vm) == 1,
              "One entry after cap_start");

  // Track cap_end for same cap 0 — should update existing entry, not add new
  vm_write_log_track_cap_end(&vm, 0, 200);
  test_assert(vm_write_log_count_entries(&vm) == 1,
              "Still one entry after cap_end on same cap");

  // Track cap_start for cap 1 — adds second entry
  vm_write_log_track_cap_start(&vm, 1, 300);
  test_assert(vm_write_log_count_entries(&vm) == 2,
              "Two entries after different cap");

  // Track cap_start for cap 0 again — should update existing entry (no new
  // entry)
  vm_write_log_track_cap_start(&vm, 0, 400);
  test_assert(vm_write_log_count_entries(&vm) == 2,
              "No new entry when updating existing cap");

  // Verify final values by copying
  size_t count = vm_write_log_count_entries(&vm);
  test_assert(count == 2, "Final entry count is 2");

  WriteLogEntry entries[4];
  vm_write_log_copy_entries(&vm, entries, 4);
  // Entries order: cap0 then cap1 (based on insertion order in circular buffer)
  bool found0 = false;
  bool found1 = false;
  for (size_t i = 0; i < count; i++) {
    if (entries[i].cap_index == 0) {
      found0 = true;
      test_assert(entries[i].old_start == 400, "cap0 old_start updated");
      test_assert(entries[i].old_end == 200, "cap0 old_end preserved");
    } else if (entries[i].cap_index == 1) {
      found1 = true;
      test_assert(entries[i].old_start == 300, "cap1 old_start");
      test_assert(entries[i].old_end == (size_t)-1, "cap1 old_end unset");
    }
  }
  test_assert((found0 && found1) != 0, "Both entries found in copy");

  vm_write_log_free(&vm);
}

/* ── Compact choice record size tests ───────────────────────────────────── */

static void test_compact_choice_size_calculation(void) {
  test_suite("Compact Choice Record Size");

  VM vm;
  memset(&vm, 0, sizeof(vm));
  vm.use_compact_choice = true;
  vm_trail_init(&vm);
  vm.trail_cap = 256;
  vm.trail = malloc(vm.trail_cap * sizeof(UndoRecord));
  vm.choices_arena = vm_arena_create();
  vm.choices_top = 0;

  // Simulate: many captures and counters mutated before the choice. With the
  // trail-based choice save the record size is INDEPENDENT of how many
  // captures/counters/emits happened — it is a fixed-size header + trailing u32.
  vm.max_cap_used = 10;
  vm.max_counter_used = 5;

  // Compute expected data size. The arena stores each record as
  // [leading size word][payload][trailing size word]; the compact payload is
  // CompactChoiceHeader + trailing uint32, so the footprint is that plus the
  // two size words.
  size_t expected_total =
      sizeof(CompactChoiceHeader) + sizeof(uint32_t) + (2 * sizeof(uint32_t));

  // Simulate push by calling vm_push_choice.
  uint8_t dummy_bc[1] = {OP_ACCEPT};
  vm.bc = dummy_bc;
  vm.bc_len = 1;
  vm.ip = 0;
  vm.pos = 0;

  vm_push_choice(&vm, 0, 0);

  size_t allocated = vm.choice_allocated;
  printf("  [info] allocated=%zu expected=%zu\n", allocated, expected_total);
  test_assert(
      allocated == expected_total,
      "Compact record size is fixed (trail-based, independent of state)");

  // Cleanup
  vm_arena_destroy(vm.choices_arena);
  vm_trail_free(&vm);
}

/* ── Mode consistency test (legacy vs compact) ──────────────────────────── */

static void test_mode_consistency_capture_restore(void) {
  test_suite("Mode Consistency: Capture Restore");

  Bc b;
  bc_init(&b);

  size_t split_ip = b.ip;
  bc_emit_u8(&b, OP_SPLIT);
  bc_emit_u32(&b, 0);
  bc_emit_u32(&b, 0);

  size_t a_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'a');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_FAIL);

  size_t b_ip = b.ip;
  bc_emit_u8(&b, OP_CAP_START);
  bc_emit_u8(&b, 0);
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_CAP_END);
  bc_emit_u8(&b, 0);
  bc_emit_u8(&b, OP_ACCEPT);

  b.bc[split_ip + 1] = (uint8_t)((a_ip >> 24) & 0xFF);
  b.bc[split_ip + 2] = (uint8_t)((a_ip >> 16) & 0xFF);
  b.bc[split_ip + 3] = (uint8_t)((a_ip >> 8) & 0xFF);
  b.bc[split_ip + 4] = (uint8_t)(a_ip & 0xFF);
  b.bc[split_ip + 5] = (uint8_t)((b_ip >> 24) & 0xFF);
  b.bc[split_ip + 6] = (uint8_t)((b_ip >> 16) & 0xFF);
  b.bc[split_ip + 7] = (uint8_t)((b_ip >> 8) & 0xFF);
  b.bc[split_ip + 8] = (uint8_t)(b_ip & 0xFF);

  const char *subject = "b";

  // Legacy mode
  setenv("SNOBOL_LEGACY_CHOICE", "1", 1);
  VM vm_legacy = make_vm(b.bc, b.lit_pool, subject);
  bool ok_legacy = vm_run(&vm_legacy);
  bool cap0_legacy = cap_equals(&vm_legacy, 0, subject, "b");

  // Compact mode
  unsetenv("SNOBOL_LEGACY_CHOICE");
  VM vm_compact = make_vm(b.bc, b.lit_pool, subject);
  bool ok_compact = vm_run(&vm_compact);
  bool cap0_compact = cap_equals(&vm_compact, 0, subject, "b");

  test_assert(ok_legacy == ok_compact, "Both modes agree on success");
  test_assert(cap0_legacy == cap0_compact, "Both modes produce same capture");
  test_assert(ok_compact, "Match should succeed in compact mode");
  test_assert(cap0_compact, "cap0 should be 'b' in compact mode");
}

/* ── Memory reduction test ──────────────────────────────────────────────── */

static void test_memory_reduction_with_many_choices(void) {
  test_suite("Memory Reduction: ≥10 Choice Points");

  const int N = 10;

  // Legacy mode
  VM vm_legacy;
  memset(&vm_legacy, 0, sizeof(vm_legacy));
  vm_legacy.use_compact_choice = false;
  vm_legacy.max_cap_used = 10;
  vm_legacy.choices_arena = vm_arena_create();
  vm_legacy.choices_top = 0;
  vm_legacy.choice_allocated = 0;

  for (int i = 0; i < N; i++) {
    vm_push_choice(&vm_legacy, 0, 0);
  }
  size_t legacy_total = vm_legacy.choice_allocated;
  vm_arena_destroy(vm_legacy.choices_arena);

  // Compact mode
  VM vm_compact;
  memset(&vm_compact, 0, sizeof(vm_compact));
  vm_compact.use_compact_choice = true;
  vm_compact.max_cap_used = 10;
  vm_trail_init(&vm_compact);
  vm_compact.trail_cap = 256;
  vm_compact.trail = malloc(vm_compact.trail_cap * sizeof(UndoRecord));
  vm_compact.choices_arena = vm_arena_create();
  vm_compact.choices_top = 0;
  vm_compact.choice_allocated = 0;

  // With the trail-based choice save, pushing a choice does NOT copy capture or
  // counter state — every record is the same small fixed size regardless of how
  // much state was mutated.
  for (int i = 0; i < N; i++) {
    vm_push_choice(&vm_compact, 0, 0);
  }
  size_t compact_total = vm_compact.choice_allocated;

  vm_arena_destroy(vm_compact.choices_arena);
  vm_trail_free(&vm_compact);

  // Compute expected sizes. The arena stores each record as
  // [leading size word][payload][trailing size word], so the per-record
  // footprint = payload + 2*sizeof(uint32_t).
  // Legacy records: sizeof(struct choice) payload.
  size_t legacy_per = sizeof(struct choice) + (2 * sizeof(uint32_t));
  size_t legacy_expected = N * legacy_per;
  // Compact records: header + trailing uint32_t, plus 2 size words total.
  size_t compact_per =
      sizeof(CompactChoiceHeader) + sizeof(uint32_t) + (2 * sizeof(uint32_t));
  size_t compact_expected = N * compact_per;

  printf("  [info] legacy_total=%zu legacy_expected=%zu\n", legacy_total,
         legacy_expected);
  test_assert(legacy_total == legacy_expected,
              "Legacy total allocation matches expected");
  printf("  [info] compact_total=%zu compact_expected=%zu\n", compact_total,
         compact_expected);
  test_assert(compact_total == compact_expected,
              "Compact total allocation matches expected");

  double ratio = (double)compact_total / (double)legacy_total;
  printf("  [info] ratio=%.2f\n", ratio);
  test_assert(ratio <= 0.5, "Compact memory ≤ 50%% of legacy");
}

/* ── Statistics API test ─────────────────────────────────────────────────── */

static void test_choice_statistics_api(void) {
  test_suite("Choice Statistics API");

  Bc b;
  bc_init(&b);
  size_t split_ip = b.ip;
  bc_emit_u8(&b, OP_SPLIT);
  bc_emit_u32(&b, 0);
  bc_emit_u32(&b, 0);
  size_t a_ip = b.ip;
  bc_emit_lit1(&b, 'a');
  bc_emit_u8(&b, OP_FAIL);
  size_t b_ip = b.ip;
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_ACCEPT);
  b.bc[split_ip + 1] = (uint8_t)((a_ip >> 24) & 0xFF);
  b.bc[split_ip + 2] = (uint8_t)((a_ip >> 16) & 0xFF);
  b.bc[split_ip + 3] = (uint8_t)((a_ip >> 8) & 0xFF);
  b.bc[split_ip + 4] = (uint8_t)(a_ip & 0xFF);
  b.bc[split_ip + 5] = (uint8_t)((b_ip >> 24) & 0xFF);
  b.bc[split_ip + 6] = (uint8_t)((b_ip >> 16) & 0xFF);
  b.bc[split_ip + 7] = (uint8_t)((b_ip >> 8) & 0xFF);
  b.bc[split_ip + 8] = (uint8_t)(b_ip & 0xFF);

  VM vm = make_vm(b.bc, b.lit_pool, "b");
  bool ok = vm_run(&vm);
  test_assert(ok, "Pattern should match");

  /* After vm_run completes, choices_top is reset to 0 (choices consumed).
   * Use cumulative stats: choice_push_count and choice_allocated. */
  test_assert(vm.choice_push_count == 1,
              "One choice was pushed during execution");

  test_assert(vm.choice_allocated > 0,
              "Choice bytes were allocated during execution");

  size_t avg_size = vm_choice_record_average_size(&vm);
  test_assert(avg_size > 0, "Average record size should be > 0");
  size_t expected_compact =
      sizeof(CompactChoiceHeader) + sizeof(uint32_t) + (2 * sizeof(uint32_t));
  printf("  [info] avg_size=%zu expected≈%zu\n", avg_size, expected_compact);
  test_assert((avg_size >= expected_compact - 1 &&
               avg_size <= expected_compact + 1) != 0,
              "Average size matches compact header-only record");
}

/* ── Env-var caching regression (P2.1): getenv is NOT re-read per match ──
 *
 * The SNOBOL_LEGACY_CHOICE toggle is resolved once per process and cached on
 * the VM. This guards that a later change to the environment does NOT flip the
 * choice mode for subsequently created VMs — i.e. the per-match getenv() hot
 * path was removed. */

static void test_env_var_cached_once(void) {
  test_suite("Env-var choice mode cached at first VM init (P2.1)");

  Bc b;
  bc_init(&b);
  bc_emit_lit1(&b, 'b');
  bc_emit_u8(&b, OP_ACCEPT);

  /* The SNOBOL_LEGACY_CHOICE toggle is resolved ONCE per process (static
   * cache in vm_legacy_choice_mode). The first VM created anywhere in this
   * process fixes the mode for all later VMs. We therefore cannot assert an
   * absolute mode here (an earlier suite may have already resolved the cache);
   * what we CAN and MUST guard is that the mode is NOT re-read from the
   * environment on subsequent VM creation — i.e. the per-match getenv() hot
   * path was removed (P2.1). */

  /* Observe the currently-resolved mode via the first VM. */
  VM vm_first = make_vm(b.bc, b.lit_pool, "b");
  bool ok_first = vm_run(&vm_first);
  bool first_compact = vm_first.use_compact_choice;
  test_assert(ok_first, "first VM match succeeds");

  /* Flip the environment. A cached implementation keeps the mode from first
   * init; the old per-match getenv would flip it to the new value here. */
  if (first_compact) {
    setenv("SNOBOL_LEGACY_CHOICE", "1", 1);
  } else {
    unsetenv("SNOBOL_LEGACY_CHOICE");
  }
  VM vm_second = make_vm(b.bc, b.lit_pool, "b");
  bool ok_second = vm_run(&vm_second);
  bool second_compact = vm_second.use_compact_choice;
  test_assert(ok_second, "second VM match succeeds");
  test_assert(second_compact == first_compact,
              "mode is NOT re-read from env after first init (P2.1 cache)");

  /* Leave the env clean so later suites default to compact mode. */
  unsetenv("SNOBOL_LEGACY_CHOICE");
}

/* ── Suite entry point ───────────────────────────────────────────────────── */

void test_compact_choice_suite(void) {
  test_suite("Compact Choice Stack");

  test_write_log_tracking();
  test_compact_choice_size_calculation();
  test_mode_consistency_capture_restore();
  test_memory_reduction_with_many_choices();
  test_choice_statistics_api();
  test_env_var_cached_once();
}
