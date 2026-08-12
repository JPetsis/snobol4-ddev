/**
 * @file string_fn.c
 * @brief SNOBOL4 built-in string transformation function implementations
 *
 * All string operations are Unicode codepoint-aware.
 * ASCII fast path: if all bytes < 0x80, byte-level operations are used.
 *
 * Memory management: uses snobol_malloc/snobol_free/snobol_realloc from
 * snobol_internal.h so the same allocator is used as the rest of the library.
 */

#include "snobol/string_fn.h"
#include "snobol/snobol_internal.h"
#include "snobol/unicode_fold.h"
#include "snobol/vm.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Internal UTF-8 helpers
 * -------------------------------------------------------------------------- */

/**
 * Check if a byte-string is all-ASCII (fast path).
 */
static inline bool str_is_ascii(const char *str, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if ((unsigned char)str[i] >= 0x80) {
      return false;
    }
  }
  return true;
}

/**
 * Advance [pos] to the start of the next codepoint in [str].
 * Returns the byte length of the codepoint at [pos], or 0 if past end.
 */
static inline int utf8_cp_len(const char *str, size_t str_len, size_t pos) {
  if (pos >= str_len) {
    return 0;
  }
  unsigned char c = (unsigned char)str[pos];
  if (c < 0x80) {
    return 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return (pos + 1 < str_len) ? 2 : 0;
  }
  if ((c & 0xF0) == 0xE0) {
    return (pos + 2 < str_len) ? 3 : 0;
  }
  if ((c & 0xF8) == 0xF0) {
    return (pos + 3 < str_len) ? 4 : 0;
  }
  return 1; /* Invalid byte – treat as single byte */
}

/**
 * Encode a Unicode codepoint to UTF-8.
 * Returns the number of bytes written (1-4), or 0 on invalid codepoint.
 */
static int encode_utf8(uint32_t cp, char buf[4]) {
  if (cp > 0x10FFFF) {
    return 0;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return 0; /* Surrogate */
  }
  if (cp < 0x80) {
    buf[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  buf[0] = (char)(0xF0 | (cp >> 18));
  buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* --------------------------------------------------------------------------
 * SIZE
 * -------------------------------------------------------------------------- */

/**
 * SIZE: count Unicode codepoints.
 * ASCII fast path: if all bytes < 0x80, return len directly.
 */
size_t snobol_size(const char *str, size_t len) {
  if (!str) {
    return 0;
  }
  /* ASCII fast path */
  if (str_is_ascii(str, len)) {
    return len;
  }
  /* UTF-8 codepoint counting */
  size_t count = 0;
  size_t i = 0;
  while (i < len) {
    int cl = utf8_cp_len(str, len, i);
    if (cl <= 0) {
      cl = 1; /* Skip invalid bytes */
    }
    count++;
    i += (size_t)cl;
  }
  return count;
}

/* --------------------------------------------------------------------------
 * TRIM
 * -------------------------------------------------------------------------- */

/**
 * TRIM: remove trailing whitespace.
 * Whitespace: space (0x20), tab (0x09), CR (0x0D), LF (0x0A).
 * Unicode-aware: handles multi-byte trailing whitespace sequences.
 */
bool snobol_trim(const char *in, size_t in_len, snobol_buf *out) {
  if (!in || !out) {
    return false;
  }
  size_t end = in_len;
  while (end > 0) {
    unsigned char c = (unsigned char)in[end - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      end--;
    } else {
      break;
    }
  }
  snobol_buf_clear(out);
  snobol_buf_append(out, in, end);
  return true;
}

/* --------------------------------------------------------------------------
 * DUPL
 * -------------------------------------------------------------------------- */

/**
 * DUPL: duplicate string n times with pre-allocation.
 */
bool snobol_dupl(const char *str, size_t str_len, size_t n, snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || str_len == 0 || n == 0) {
    return true;
  }
  for (size_t i = 0; i < n; i++) {
    snobol_buf_append(out, str, str_len);
  }
  return true;
}

/* --------------------------------------------------------------------------
 * REVERSE
 * -------------------------------------------------------------------------- */

/**
 * REVERSE: reverse by codepoints (two-pass).
 * Pass 1: collect byte span for each codepoint.
 * Pass 2: append codepoints in reverse order.
 */
bool snobol_reverse(const char *str, size_t str_len, snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || str_len == 0) {
    return true;
  }

  /* ASCII fast path */
  if (str_is_ascii(str, str_len)) {
    char *buf = (char *)snobol_malloc(str_len);
    if (!buf) {
      return false;
    }
    for (size_t i = 0; i < str_len; i++) {
      buf[i] = str[str_len - 1 - i];
    }
    snobol_buf_append(out, buf, str_len);
    snobol_free(buf);
    return true;
  }

  /* Pass 1: count codepoints and record their byte spans */
  size_t max_cps = str_len; /* Upper bound: each byte could be a codepoint */
  size_t *cp_starts = (size_t *)snobol_malloc(max_cps * sizeof(size_t));
  size_t *cp_ends = (size_t *)snobol_malloc(max_cps * sizeof(size_t));
  if (!cp_starts || !cp_ends) {
    snobol_free(cp_starts);
    snobol_free(cp_ends);
    return false;
  }

  size_t count = 0;
  size_t pos = 0;
  while (pos < str_len) {
    int cl = utf8_cp_len(str, str_len, pos);
    if (cl <= 0) {
      cl = 1;
    }
    cp_starts[count] = pos;
    cp_ends[count] = pos + (size_t)cl;
    count++;
    pos += (size_t)cl;
  }

  /* Pass 2: append in reverse order */
  for (size_t i = count; i > 0; i--) {
    snobol_buf_append(out, str + cp_starts[i - 1],
                      cp_ends[i - 1] - cp_starts[i - 1]);
  }

  snobol_free(cp_starts);
  snobol_free(cp_ends);
  return true;
}

/* --------------------------------------------------------------------------
 * SUBSTR
 * -------------------------------------------------------------------------- */

/**
 * SUBSTR: extract substring by 1-based codepoint position.
 */
bool snobol_substr(const char *str, size_t str_len, size_t pos, size_t len,
                   snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || pos == 0) {
    return false; /* 1-based: pos=0 is invalid */
  }

  /* Walk forward to (pos-1) codepoints */
  size_t byte_pos = 0;
  for (size_t cp = 0; cp < pos - 1 && byte_pos < str_len; cp++) {
    int cl = utf8_cp_len(str, str_len, byte_pos);
    if (cl <= 0) {
      cl = 1;
    }
    byte_pos += (size_t)cl;
  }

  if (byte_pos >= str_len && pos > 1) {
    return false; /* pos out of range */
  }

  /* Collect [len] codepoints starting at byte_pos */
  size_t start_byte = byte_pos;
  size_t end_byte = byte_pos;
  for (size_t cp = 0; cp < len && end_byte < str_len; cp++) {
    int cl = utf8_cp_len(str, str_len, end_byte);
    if (cl <= 0) {
      cl = 1;
    }
    end_byte += (size_t)cl;
  }

  snobol_buf_append(out, str + start_byte, end_byte - start_byte);
  return true;
}

/* --------------------------------------------------------------------------
 * REPLACE
 * -------------------------------------------------------------------------- */

/**
 * REPLACE: replace all occurrences of [from] with [to].
 */
bool snobol_replace(const char *str, size_t str_len, const char *from,
                    size_t from_len, const char *to, size_t to_len,
                    snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str) {
    return true;
  }
  if (!from || from_len == 0) {
    /* Nothing to replace */
    snobol_buf_append(out, str, str_len);
    return true;
  }

  size_t i = 0;
  while (i + from_len <= str_len) {
    if (memcmp(str + i, from, from_len) == 0) {
      if (to && to_len > 0) {
        snobol_buf_append(out, to, to_len);
      }
      i += from_len;
    } else {
      snobol_buf_append(out, str + i, 1);
      i++;
    }
  }
  /* Append any remaining bytes */
  if (i < str_len) {
    snobol_buf_append(out, str + i, str_len - i);
  }
  return true;
}

/* --------------------------------------------------------------------------
 * REPLACE_CHAR
 * -------------------------------------------------------------------------- */

/**
 * REPLACE_CHAR: character translation via 256-byte lookup table.
 * Similar to POSIX tr or PHP strtr with character arrays.
 */
bool snobol_replace_char(const char *str, size_t str_len, const char *from,
                         size_t from_len, const char *to, size_t to_len,
                         snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || str_len == 0) {
    return true;
  }

  /* Build 256-entry translation table (identity by default) */
  uint8_t table[256];
  for (int i = 0; i < 256; i++) {
    table[i] = (uint8_t)i;
  }

  size_t map_len = from_len < to_len ? from_len : to_len;
  if (from) {
    for (size_t i = 0; i < map_len; i++) {
      table[(unsigned char)from[i]] =
          to ? (unsigned char)to[i] : (unsigned char)from[i];
    }
  }

  /* Single-pass translation */
  char *buf = (char *)snobol_malloc(str_len + 1);
  if (!buf) {
    return false;
  }
  for (size_t i = 0; i < str_len; i++) {
    buf[i] = (char)table[(unsigned char)str[i]];
  }
  snobol_buf_append(out, buf, str_len);
  snobol_free(buf);
  return true;
}

/* --------------------------------------------------------------------------
 * LPAD / RPAD
 * -------------------------------------------------------------------------- */

/**
 * LPAD: left-pad to [width] codepoints.
 */
bool snobol_lpad(const char *str, size_t str_len, size_t width, uint32_t pad_cp,
                 snobol_buf *out) {
  if (!out) {
    return false;
  }
  size_t cur_width = snobol_size(str ? str : "", str_len);
  snobol_buf_clear(out);

  if (cur_width >= width) {
    if (str) {
      snobol_buf_append(out, str, str_len);
    }
    return true;
  }

  char pad_buf[4];
  int pad_bytes = encode_utf8(pad_cp, pad_buf);
  if (pad_bytes <= 0) {
    return false;
  }

  size_t pad_count = width - cur_width;
  for (size_t i = 0; i < pad_count; i++) {
    snobol_buf_append(out, pad_buf, (size_t)pad_bytes);
  }
  if (str) {
    snobol_buf_append(out, str, str_len);
  }
  return true;
}

/**
 * RPAD: right-pad to [width] codepoints.
 */
bool snobol_rpad(const char *str, size_t str_len, size_t width, uint32_t pad_cp,
                 snobol_buf *out) {
  if (!out) {
    return false;
  }
  size_t cur_width = snobol_size(str ? str : "", str_len);
  snobol_buf_clear(out);
  if (str) {
    snobol_buf_append(out, str, str_len);
  }

  if (cur_width >= width) {
    return true;
  }

  char pad_buf[4];
  int pad_bytes = encode_utf8(pad_cp, pad_buf);
  if (pad_bytes <= 0) {
    return false;
  }

  size_t pad_count = width - cur_width;
  for (size_t i = 0; i < pad_count; i++) {
    snobol_buf_append(out, pad_buf, (size_t)pad_bytes);
  }
  return true;
}

/* --------------------------------------------------------------------------
 * CHAR / ORD
 * -------------------------------------------------------------------------- */

/**
 * CHAR: convert Unicode codepoint to UTF-8 string.
 */
bool snobol_char_fn(uint32_t cp, snobol_buf *out) {
  if (!out) {
    return false;
  }
  if (cp > 0x10FFFF) {
    return false;
  }
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return false; /* Surrogate range invalid */
  }

  char buf[4];
  int bytes = encode_utf8(cp, buf);
  if (bytes <= 0) {
    return false;
  }

  snobol_buf_clear(out);
  snobol_buf_append(out, buf, (size_t)bytes);
  return true;
}

/**
 * ORD: get codepoint value of first character.
 */
bool snobol_ord(const char *str, size_t str_len, uint32_t *out_cp) {
  if (!str || str_len == 0 || !out_cp) {
    return false;
  }
  unsigned char c = (unsigned char)str[0];
  if (c < 0x80) {
    *out_cp = c;
    return true;
  }
  if ((c & 0xE0) == 0xC0) {
    if (str_len < 2) {
      return false;
    }
    *out_cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)str[1] & 0x3F);
    return true;
  }
  if ((c & 0xF0) == 0xE0) {
    if (str_len < 3) {
      return false;
    }
    *out_cp = ((uint32_t)(c & 0x0F) << 12) |
              ((uint32_t)((unsigned char)str[1] & 0x3F) << 6) |
              ((unsigned char)str[2] & 0x3F);
    return true;
  }
  if ((c & 0xF8) == 0xF0) {
    if (str_len < 4) {
      return false;
    }
    *out_cp = ((uint32_t)(c & 0x07) << 18) |
              ((uint32_t)((unsigned char)str[1] & 0x3F) << 12) |
              ((uint32_t)((unsigned char)str[2] & 0x3F) << 6) |
              ((unsigned char)str[3] & 0x3F);
    return true;
  }
  return false; /* Invalid UTF-8 lead byte */
}

/* --------------------------------------------------------------------------
 * UPPER / LOWER
 * -------------------------------------------------------------------------- */

/**
 * UPPER: Unicode-aware uppercase conversion.
 * v2: full Unicode case folding — Latin-1 Supplement (U+00C0–U+00FF) and
 *     Latin Extended-A (U+0100–U+017E), including multi-char expansion for
 *     the German sharp-s U+00DF ß → "SS".
 * ASCII fast path: direct arithmetic for a–z.
 * Codepoints beyond Latin Extended-A are passed through unchanged.
 */
bool snobol_upper(const char *str, size_t str_len, snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || str_len == 0) {
    return true;
  }

  size_t pos = 0;
  while (pos < str_len) {
    uint32_t cp = 0;
    int cp_bytes = 0;
    if (!utf8_peek_next(str, str_len, pos, &cp, &cp_bytes)) {
      /* Invalid or truncated byte — pass through as raw byte */
      snobol_buf_append(out, str + pos, 1);
      pos++;
      continue;
    }

    uint32_t upper_out[2];
    int upper_len = 0;
    snobol_to_upper_cp(cp, upper_out, &upper_len);

    char utf8_buf[4];
    for (int k = 0; k < upper_len; k++) {
      int nbytes = encode_utf8(upper_out[k], utf8_buf);
      if (nbytes > 0) {
        snobol_buf_append(out, utf8_buf, (size_t)nbytes);
      }
    }

    pos += (size_t)cp_bytes;
  }
  return true;
}

/**
 * LOWER: Unicode-aware lowercase conversion.
 * v2: full Unicode case folding — Latin-1 Supplement (U+00C0–U+00FF) and
 *     Latin Extended-A (U+0100–U+017E).
 * ASCII fast path: direct arithmetic for A–Z.
 * Codepoints beyond Latin Extended-A are passed through unchanged.
 */
bool snobol_lower(const char *str, size_t str_len, snobol_buf *out) {
  if (!out) {
    return false;
  }
  snobol_buf_clear(out);
  if (!str || str_len == 0) {
    return true;
  }

  size_t pos = 0;
  while (pos < str_len) {
    uint32_t cp = 0;
    int cp_bytes = 0;
    if (!utf8_peek_next(str, str_len, pos, &cp, &cp_bytes)) {
      snobol_buf_append(out, str + pos, 1);
      pos++;
      continue;
    }

    uint32_t lower_cp = snobol_to_lower_cp(cp);

    char utf8_buf[4];
    int nbytes = encode_utf8(lower_cp, utf8_buf);
    if (nbytes > 0) {
      snobol_buf_append(out, utf8_buf, (size_t)nbytes);
    }

    pos += (size_t)cp_bytes;
  }
  return true;
}
