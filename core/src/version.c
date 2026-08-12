/**
 * @file version.c
 * @brief libsnobol4 version information
 */

#include "snobol/version.h"
#include "snobol/snobol.h"
#include <stdint.h>

void snobol_version(int *major, int *minor, int *patch) {
  if (major) {
    *major = SNOBOL_VERSION_MAJOR;
  }
  if (minor) {
    *minor = SNOBOL_VERSION_MINOR;
  }
  if (patch) {
    *patch = SNOBOL_VERSION_PATCH;
  }
}

uint32_t snobol_get_api_version(void) {
  return ((uint32_t)SNOBOL_VERSION_MAJOR << 16) |
         ((uint32_t)SNOBOL_VERSION_MINOR << 8) |
         ((uint32_t)SNOBOL_VERSION_PATCH);
}

uint32_t snobol_get_abi_version(void) {
  return SNOBOL_ABI_VERSION;
}
