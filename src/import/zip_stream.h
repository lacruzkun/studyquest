/* zip_stream.h — bounded, streaming extraction of ZIP entries.
 *
 * Anki packages are ZIP files. Modern ones (Anki 2.1.50+) additionally store
 * the collection database, the media list and every media file as zstd
 * frames *inside* the ZIP, and those frames do not carry their decompressed
 * size in the header. So the extractor must stream:
 *
 *     zip entry --(chunks)--> [zstd decoder] --> FILE* or growable buffer
 *
 * with a hard cap on the number of output bytes. Nothing is ever allocated
 * based on a size claimed by the (untrusted) archive, which makes zip bombs
 * and zstd bombs harmless: extraction simply stops at the cap.
 */
#ifndef ZIP_STREAM_H
#define ZIP_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct mz_zip_archive_tag;   /* miniz's mz_zip_archive */

typedef enum {
    ZS_OK = 0,
    ZS_NOT_FOUND,     /* entry is not in the archive */
    ZS_TOO_BIG,       /* output would exceed the caller's cap */
    ZS_CORRUPT,       /* bad zip data, bad zstd frame, or truncated frame */
    ZS_IO,            /* write failure on the destination */
    ZS_NOMEM
} ZipStreamStatus;

const char *zs_status_str(ZipStreamStatus s);

/* Magic bytes of a zstd frame (0xFD2FB528 little-endian). */
bool zs_looks_like_zstd(const void *data, size_t len);

/* Stream `entry` into `out`. If `zstd` is true the entry is decoded as one
   (or several concatenated) zstd frames, otherwise it is copied verbatim.
   At most `max_out` bytes are written; `*out_bytes` (optional) receives the
   number actually written. */
ZipStreamStatus zs_extract_to_file(struct mz_zip_archive_tag *zip,
                                   const char *entry, bool zstd,
                                   uint64_t max_out, FILE *out,
                                   uint64_t *out_bytes);

/* Same, but into a malloc'd, NUL-terminated buffer (caller frees). The
   terminator is not counted in *len. */
ZipStreamStatus zs_extract_to_mem(struct mz_zip_archive_tag *zip,
                                  const char *entry, bool zstd,
                                  uint64_t max_out,
                                  uint8_t **buf, size_t *len);

/* Decode an in-memory zstd stream (used for data already read elsewhere).
   Result is malloc'd and NUL-terminated (caller frees). */
ZipStreamStatus zs_zstd_decode_mem(const void *src, size_t src_len,
                                   uint64_t max_out,
                                   uint8_t **buf, size_t *len);

#endif
