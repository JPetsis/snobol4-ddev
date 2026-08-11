/**
 * test_api_version.c – Tests for snobol_get_api_version() and
 * snobol_get_abi_version()
 *
 * Assert version macros match encoded values at runtime.
 */

#include <stdbool.h>
#include <stdint.h>

#include "snobol/snobol.h"

extern void test_suite(const char *name);
extern void test_assert(bool condition, const char *message);

void test_api_version_suite(void) {
  test_suite("API version");

  uint32_t ver = snobol_get_api_version();

  /* For v1.0.4: (1 << 16) | (0 << 8) | 4 == 0x00010004 */
  test_assert(ver == 0x00010004u,
              "snobol_get_api_version() == 0x00010004u (v1.0.4)");

  /* Major version extraction */
  uint32_t major = ver >> 16;
  test_assert(major == SNOBOL_VERSION_MAJOR,
              "snobol_get_api_version() >> 16 == SNOBOL_VERSION_MAJOR");

  /* Minor version extraction */
  uint32_t minor = (ver >> 8) & 0xFF;
  test_assert(minor == SNOBOL_VERSION_MINOR,
              "snobol_get_api_version() minor field == SNOBOL_VERSION_MINOR");

  /* Patch version extraction */
  uint32_t patch = ver & 0xFF;
  test_assert(patch == SNOBOL_VERSION_PATCH,
              "snobol_get_api_version() patch field == SNOBOL_VERSION_PATCH");

  /* Consistency with snobol_version() */
  int vi_major, vi_minor, vi_patch;
  snobol_version(&vi_major, &vi_minor, &vi_patch);
  test_assert((int)major == vi_major && (int)minor == vi_minor &&
                  (int)patch == vi_patch,
              "snobol_get_api_version() consistent with snobol_version()");

  /* ABI version */
  uint32_t abi = snobol_get_abi_version();
  test_assert(abi == 1, "snobol_get_abi_version() == 1 (initial)");
  test_assert(abi == SNOBOL_ABI_VERSION,
              "snobol_get_abi_version() == SNOBOL_ABI_VERSION macro");
}
