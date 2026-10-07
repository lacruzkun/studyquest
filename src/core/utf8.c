#include "core/utf8.h"
#include <string.h>

static bool continuation(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

bool sq_utf8_decode(const char *s, uint32_t *codepoint, size_t *bytes) {
    if (!s || !*s) return false;

    const unsigned char *p = (const unsigned char *)s;
    uint32_t cp;
    size_t n;

    if (p[0] < 0x80u) {
        cp = p[0];
        n = 1;
    } else if (p[0] >= 0xC2u && p[0] <= 0xDFu) {
        if (!p[1] || !continuation(p[1])) return false;
        cp = ((uint32_t)(p[0] & 0x1Fu) << 6) |
             (uint32_t)(p[1] & 0x3Fu);
        n = 2;
    } else if (p[0] >= 0xE0u && p[0] <= 0xEFu) {
        if (!p[1] || !p[2] || !continuation(p[1]) || !continuation(p[2])) return false;
        if (p[0] == 0xE0u && p[1] < 0xA0u) return false; /* overlong */
        if (p[0] == 0xEDu && p[1] >= 0xA0u) return false; /* surrogate */
        cp = ((uint32_t)(p[0] & 0x0Fu) << 12) |
             ((uint32_t)(p[1] & 0x3Fu) << 6) |
             (uint32_t)(p[2] & 0x3Fu);
        n = 3;
    } else if (p[0] >= 0xF0u && p[0] <= 0xF4u) {
        if (!p[1] || !p[2] || !p[3] ||
            !continuation(p[1]) || !continuation(p[2]) || !continuation(p[3])) return false;
        if (p[0] == 0xF0u && p[1] < 0x90u) return false; /* overlong */
        if (p[0] == 0xF4u && p[1] >= 0x90u) return false; /* > U+10FFFF */
        cp = ((uint32_t)(p[0] & 0x07u) << 18) |
             ((uint32_t)(p[1] & 0x3Fu) << 12) |
             ((uint32_t)(p[2] & 0x3Fu) << 6) |
             (uint32_t)(p[3] & 0x3Fu);
        n = 4;
    } else {
        return false;
    }

    if (codepoint) *codepoint = cp;
    if (bytes) *bytes = n;
    return true;
}

bool sq_utf8_validate(const char *s, size_t *bad_offset) {
    if (bad_offset) *bad_offset = 0;
    if (!s) return true;

    const char *p = s;
    while (*p) {
        uint32_t cp = 0;
        size_t n = 0;
        if (!sq_utf8_decode(p, &cp, &n)) {
            if (bad_offset) *bad_offset = (size_t)(p - s);
            return false;
        }
        p += n;
    }
    return true;
}

size_t sq_utf8_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return 0;
    dst[0] = 0;
    if (!src) return 0;

    size_t w = 0;
    const char *p = src;
    while (*p && w + 1 < cap) {
        uint32_t cp = 0;
        size_t n = 0;
        if (!sq_utf8_decode(p, &cp, &n)) {
            /* Preserve the byte rather than inventing/replacing a glyph. */
            n = 1;
        }
        if (w + n >= cap) break;
        memcpy(dst + w, p, n);
        w += n;
        p += n;
    }
    dst[w] = 0;
    return w;
}
