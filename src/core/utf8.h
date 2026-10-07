#ifndef SQ_UTF8_H
#define SQ_UTF8_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Strict UTF-8 decoder. Returns false for incomplete, overlong,
   surrogate, or out-of-range sequences. On success *bytes is 1..4. */
bool sq_utf8_decode(const char *s, uint32_t *codepoint, size_t *bytes);

/* Validate an entire NUL-terminated string. */
bool sq_utf8_validate(const char *s, size_t *bad_offset);

/* Copy at most cap-1 bytes without splitting a UTF-8 codepoint.
   The source is expected to be UTF-8. Invalid bytes are copied verbatim,
   but the returned length is always a complete-byte prefix. */
size_t sq_utf8_copy(char *dst, size_t cap, const char *src);

#endif
