/* pb_reader.h — minimal, bounds-checked protobuf wire-format reader.
 *
 * Modern Anki packages (2.1.50+) store note types, templates, deck kinds and
 * the media list as protobuf. We only need to pull a handful of fields out of
 * those messages, so a full protobuf runtime would be overkill. This reader
 * walks the wire format field by field and skips anything it does not know,
 * which also keeps it forward compatible with new Anki releases.
 *
 * All input is untrusted: every read is bounds-checked and a malformed
 * message simply makes pb_next() return false.
 */
#ifndef PB_READER_H
#define PB_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    bool           bad;     /* set when the message was malformed */
} PbReader;

typedef struct {
    uint32_t       field;   /* field number */
    int            wire;    /* 0=varint 1=64bit 2=len-delimited 5=32bit */
    uint64_t       varint;  /* wire 0 value, or 32/64-bit raw value */
    const uint8_t *data;    /* wire 2 payload */
    size_t         len;     /* wire 2 payload length */
} PbField;

static inline void pb_init(PbReader *r, const void *buf, size_t len) {
    r->p = (const uint8_t *)buf;
    r->end = r->p + len;
    r->bad = false;
}

static inline bool pb_read_varint(PbReader *r, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    while (r->p < r->end) {
        uint8_t b = *r->p++;
        if (shift >= 64) { r->bad = true; return false; }
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return true; }
        shift += 7;
    }
    r->bad = true;     /* ran off the end mid-varint */
    return false;
}

/* Read the next field. Returns false at a clean end of message or on error
   (check r->bad to tell them apart). */
static inline bool pb_next(PbReader *r, PbField *f) {
    if (r->p >= r->end) return false;

    uint64_t tag;
    if (!pb_read_varint(r, &tag)) return false;
    f->field  = (uint32_t)(tag >> 3);
    f->wire   = (int)(tag & 7);
    f->varint = 0;
    f->data   = NULL;
    f->len    = 0;
    if (f->field == 0) { r->bad = true; return false; }

    switch (f->wire) {
    case 0:
        return pb_read_varint(r, &f->varint);
    case 1:
        if ((size_t)(r->end - r->p) < 8) { r->bad = true; return false; }
        for (int i = 0; i < 8; i++) f->varint |= (uint64_t)r->p[i] << (8 * i);
        r->p += 8;
        return true;
    case 5:
        if ((size_t)(r->end - r->p) < 4) { r->bad = true; return false; }
        for (int i = 0; i < 4; i++) f->varint |= (uint64_t)r->p[i] << (8 * i);
        r->p += 4;
        return true;
    case 2: {
        uint64_t n;
        if (!pb_read_varint(r, &n)) return false;
        if (n > (uint64_t)(r->end - r->p)) { r->bad = true; return false; }
        f->data = r->p;
        f->len  = (size_t)n;
        r->p   += n;
        return true;
    }
    default:   /* groups (3/4) and anything else are not used by Anki */
        r->bad = true;
        return false;
    }
}

#endif
