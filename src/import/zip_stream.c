/* zip_stream.c — see zip_stream.h */
#include "import/zip_stream.h"

#include "miniz.h"
#include "zstd.h"

#include <stdlib.h>
#include <string.h>

#define ZS_OUT_CHUNK (128u * 1024u)

typedef struct {
    ZSTD_DCtx      *dctx;        /* NULL => raw copy */
    FILE           *file;        /* destination file, or NULL => memory */
    uint8_t        *mem;
    size_t          mem_len;
    size_t          mem_cap;
    uint8_t        *chunk;       /* zstd output scratch */
    uint64_t        max_out;
    uint64_t        total;
    bool            frame_complete;   /* last ZSTD call returned 0 */
    bool            saw_input;
    ZipStreamStatus status;
} Sink;

const char *zs_status_str(ZipStreamStatus s) {
    switch (s) {
        case ZS_OK:        return "ok";
        case ZS_NOT_FOUND: return "not found in archive";
        case ZS_TOO_BIG:   return "exceeds the size limit";
        case ZS_CORRUPT:   return "corrupt or truncated data";
        case ZS_IO:        return "write failed";
        case ZS_NOMEM:     return "out of memory";
    }
    return "error";
}

bool zs_looks_like_zstd(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    return len >= 4 && p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD;
}

/* ------------------------------------------------------------------ */

static bool sink_emit(Sink *s, const void *data, size_t n) {
    if (n == 0) return true;
    if (n > s->max_out || s->total > s->max_out - n) {
        s->status = ZS_TOO_BIG;
        return false;
    }
    if (s->file) {
        if (fwrite(data, 1, n, s->file) != n) { s->status = ZS_IO; return false; }
    } else {
        size_t need = s->mem_len + n + 1;     /* +1 for the NUL terminator */
        if (need > s->mem_cap) {
            size_t cap = s->mem_cap ? s->mem_cap : 4096;
            while (cap < need) cap *= 2;
            uint8_t *nb = (uint8_t *)realloc(s->mem, cap);
            if (!nb) { s->status = ZS_NOMEM; return false; }
            s->mem = nb;
            s->mem_cap = cap;
        }
        memcpy(s->mem + s->mem_len, data, n);
        s->mem_len += n;
    }
    s->total += n;
    return true;
}

/* Feed one chunk of (already ZIP-decoded) bytes through the pipeline. */
static bool sink_feed(Sink *s, const void *buf, size_t n) {
    if (s->status != ZS_OK) return false;
    if (n > 0) s->saw_input = true;
    if (!s->dctx) return sink_emit(s, buf, n);

    ZSTD_inBuffer in = { buf, n, 0 };
    while (in.pos < in.size) {
        size_t before = in.pos;
        ZSTD_outBuffer out = { s->chunk, ZS_OUT_CHUNK, 0 };
        size_t r = ZSTD_decompressStream(s->dctx, &out, &in);
        if (ZSTD_isError(r)) { s->status = ZS_CORRUPT; return false; }
        if (!sink_emit(s, s->chunk, out.pos)) return false;
        s->frame_complete = (r == 0);
        if (out.pos == 0 && in.pos == before) {   /* no progress: corrupt */
            s->status = ZS_CORRUPT;
            return false;
        }
    }
    return true;
}

static size_t sink_callback(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
    (void)ofs;
    return sink_feed((Sink *)opaque, buf, n) ? n : 0;
}

static bool sink_open(Sink *s, bool zstd, uint64_t max_out, FILE *f) {
    memset(s, 0, sizeof(*s));
    s->file = f;
    s->max_out = max_out;
    s->status = ZS_OK;
    if (zstd) {
        s->dctx = ZSTD_createDCtx();
        s->chunk = (uint8_t *)malloc(ZS_OUT_CHUNK);
        if (!s->dctx || !s->chunk) {
            if (s->dctx) ZSTD_freeDCtx(s->dctx);
            free(s->chunk);
            s->dctx = NULL; s->chunk = NULL;
            s->status = ZS_NOMEM;
            return false;
        }
    }
    return true;
}

static void sink_close(Sink *s) {
    if (s->dctx) ZSTD_freeDCtx(s->dctx);
    free(s->chunk);
    s->dctx = NULL;
    s->chunk = NULL;
}

/* Called after the last input byte: a zstd stream must end on a frame
   boundary, otherwise it was truncated. */
static ZipStreamStatus sink_finish(Sink *s) {
    if (s->status != ZS_OK) return s->status;
    if (s->dctx && s->saw_input && !s->frame_complete) return ZS_CORRUPT;
    if (s->dctx && !s->saw_input) return ZS_CORRUPT;   /* empty "zstd" stream */
    return ZS_OK;
}

/* ------------------------------------------------------------------ */

ZipStreamStatus zs_extract_to_file(struct mz_zip_archive_tag *zip,
                                   const char *entry, bool zstd,
                                   uint64_t max_out, FILE *out,
                                   uint64_t *out_bytes) {
    if (out_bytes) *out_bytes = 0;
    if (!zip || !entry || !out) return ZS_IO;
    if (mz_zip_reader_locate_file((mz_zip_archive *)zip, entry, NULL, 0) < 0) return ZS_NOT_FOUND;

    Sink s;
    if (!sink_open(&s, zstd, max_out, out)) return s.status;

    mz_bool ok = mz_zip_reader_extract_file_to_callback((mz_zip_archive *)zip, entry,
                                                        sink_callback, &s, 0);
    ZipStreamStatus st = s.status;
    if (st == ZS_OK && !ok) st = ZS_CORRUPT;
    if (st == ZS_OK) st = sink_finish(&s);
    if (out_bytes) *out_bytes = s.total;
    sink_close(&s);
    return st;
}

ZipStreamStatus zs_extract_to_mem(struct mz_zip_archive_tag *zip,
                                  const char *entry, bool zstd,
                                  uint64_t max_out,
                                  uint8_t **buf, size_t *len) {
    if (buf) *buf = NULL;
    if (len) *len = 0;
    if (!zip || !entry || !buf) return ZS_IO;
    if (mz_zip_reader_locate_file((mz_zip_archive *)zip, entry, NULL, 0) < 0) return ZS_NOT_FOUND;

    Sink s;
    if (!sink_open(&s, zstd, max_out, NULL)) return s.status;

    mz_bool ok = mz_zip_reader_extract_file_to_callback((mz_zip_archive *)zip, entry,
                                                        sink_callback, &s, 0);
    ZipStreamStatus st = s.status;
    if (st == ZS_OK && !ok) st = ZS_CORRUPT;
    if (st == ZS_OK) st = sink_finish(&s);

    if (st != ZS_OK) {
        free(s.mem);
        sink_close(&s);
        return st;
    }
    if (!s.mem) {                       /* empty entry: still hand back "" */
        s.mem = (uint8_t *)calloc(1, 1);
        if (!s.mem) { sink_close(&s); return ZS_NOMEM; }
    }
    s.mem[s.mem_len] = 0;
    *buf = s.mem;
    if (len) *len = s.mem_len;
    sink_close(&s);
    return ZS_OK;
}

ZipStreamStatus zs_zstd_decode_mem(const void *src, size_t src_len,
                                   uint64_t max_out,
                                   uint8_t **buf, size_t *len) {
    if (buf) *buf = NULL;
    if (len) *len = 0;
    if (!src || !buf) return ZS_IO;

    Sink s;
    if (!sink_open(&s, true, max_out, NULL)) return s.status;

    bool ok = sink_feed(&s, src, src_len);
    ZipStreamStatus st = s.status;
    if (st == ZS_OK && !ok) st = ZS_CORRUPT;
    if (st == ZS_OK) st = sink_finish(&s);
    if (st != ZS_OK) {
        free(s.mem);
        sink_close(&s);
        return st;
    }
    if (!s.mem) {
        s.mem = (uint8_t *)calloc(1, 1);
        if (!s.mem) { sink_close(&s); return ZS_NOMEM; }
    }
    s.mem[s.mem_len] = 0;
    *buf = s.mem;
    if (len) *len = s.mem_len;
    sink_close(&s);
    return ZS_OK;
}
