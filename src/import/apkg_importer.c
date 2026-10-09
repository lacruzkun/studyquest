/* apkg_importer.c */
#include "import/apkg_importer.h"
#include "import/zip_stream.h"
#include "import/pb_reader.h"
#include "miniz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <errno.h>

/* Untrusted-input guards. Anki packages are normally a few MB; these caps
   exist purely to stop hostile/corrupt archives from exhausting memory or
   disk. Everything is streamed, so a cap is a limit on *output* bytes. */
#define APKG_MAX_ARCHIVE_ENTRIES   1000000u
#define APKG_MAX_COLLECTION_BYTES  (1ull << 30)          /* decoded collection */
#define APKG_MAX_MEDIA_MAP         (32ull * 1024 * 1024)
#define APKG_MAX_META_BYTES        4096u

/* --- small helpers ---------------------------------------------- */

static bool file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char *path) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST) return false;
    return true;
}

/* Anki archive entries are flat (0, 1, collection.anki2, media, meta), so
   anything with a slash or .. is either malicious or unexpected. */
static bool safe_entry_name(const char *name) {
    if (!name || !*name) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (strstr(name, "..")) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    return true;
}

/* Stream one entry to `dest_path`. Removes the partial file on failure. */
static ZipStreamStatus extract_entry(mz_zip_archive *zip, const char *entry,
                                     bool zstd, uint64_t cap,
                                     const char *dest_path, uint64_t *bytes) {
    FILE *f = fopen(dest_path, "wb");
    if (!f) return ZS_IO;
    ZipStreamStatus st = zs_extract_to_file((struct mz_zip_archive_tag *)zip,
                                            entry, zstd, cap, f, bytes);
    if (fclose(f) != 0 && st == ZS_OK) st = ZS_IO;
    if (st != ZS_OK) remove(dest_path);
    return st;
}

/* The `meta` file is a tiny protobuf: PackageMetadata { Version version = 1 }
   with VERSION_LEGACY_1 = 1, VERSION_LEGACY_2 = 2, VERSION_LATEST = 3. */
static int read_meta_version(mz_zip_archive *zip) {
    uint8_t *buf = NULL;
    size_t len = 0;
    if (zs_extract_to_mem((struct mz_zip_archive_tag *)zip, "meta", false,
                          APKG_MAX_META_BYTES, &buf, &len) != ZS_OK)
        return 0;
    int version = 0;
    PbReader r;
    PbField f;
    pb_init(&r, buf, len);
    while (pb_next(&r, &f)) {
        if (f.field == 1 && f.wire == 0 && f.varint <= 1000) {
            version = (int)f.varint;
            break;
        }
    }
    free(buf);
    return version;
}

/* --- public API -------------------------------------------------- */

bool apkg_open(const char *apkg_path, const char *tmp_dir, ApkgHandle *out) {
    memset(out, 0, sizeof(*out));
    out->variant = APKG_VARIANT_UNKNOWN;

    if (!file_exists(apkg_path)) {
        snprintf(out->error, sizeof(out->error),
                 "File not found: %s", apkg_path);
        return false;
    }
    if (!dir_exists(tmp_dir)) {
        if (!mkdir_p(tmp_dir)) {
            snprintf(out->error, sizeof(out->error),
                     "Could not create temp directory: %s", tmp_dir);
            return false;
        }
    }
    snprintf(out->tmp_dir, sizeof(out->tmp_dir), "%s", tmp_dir);

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, apkg_path, 0)) {
        snprintf(out->error, sizeof(out->error),
                 "Not a valid ZIP archive. The file may be corrupted "
                 "or may not be an Anki package.");
        return false;
    }

    /* Reject archives with an implausible number of entries up front. */
    if (mz_zip_reader_get_num_files(&zip) > APKG_MAX_ARCHIVE_ENTRIES) {
        snprintf(out->error, sizeof(out->error),
                 "The package contains too many files and will not be imported.");
        mz_zip_reader_end(&zip);
        return false;
    }

    out->meta_version = read_meta_version(&zip);

    /* Look for the three possible collection filenames. The order matters:
       we prefer the newest variant first so that a package containing both
       a real collection.anki21 and a dummy collection.anki2 is handled
       correctly. */
    const char *real_db_entry = NULL;
    bool real_db_is_zstd = false;

    if (mz_zip_reader_locate_file(&zip, "collection.anki21b", NULL, 0) >= 0) {
        real_db_entry = "collection.anki21b";
        real_db_is_zstd = true;
        out->variant = APKG_VARIANT_MODERN;
    } else if (mz_zip_reader_locate_file(&zip, "collection.anki21", NULL, 0) >= 0) {
        real_db_entry = "collection.anki21";
        out->variant = APKG_VARIANT_LEGACY2;
    } else if (mz_zip_reader_locate_file(&zip, "collection.anki2", NULL, 0) >= 0) {
        real_db_entry = "collection.anki2";
        out->variant = APKG_VARIANT_LEGACY1;
    } else {
        snprintf(out->error, sizeof(out->error),
                 "The package does not contain a recognised Anki "
                 "collection file. Expected collection.anki2, "
                 "collection.anki21, or collection.anki21b.");
        mz_zip_reader_end(&zip);
        return false;
    }

    /* In the latest package format (meta version 3) every media file is
       also a zstd frame. */
    out->media_zstd = (out->meta_version >= 3) ||
                      (out->variant == APKG_VARIANT_MODERN);

    /* Extract the real collection (streamed; zstd frames from Anki carry no
       decompressed size, so we cannot size a buffer up front). */
    snprintf(out->db_path, sizeof(out->db_path),
             "%s/%s", tmp_dir, real_db_entry);

    uint64_t n = 0;
    ZipStreamStatus st = extract_entry(&zip, real_db_entry, real_db_is_zstd,
                                       APKG_MAX_COLLECTION_BYTES, out->db_path, &n);
    if (st != ZS_OK || n == 0) {
        if (st == ZS_OK) remove(out->db_path);
        if (real_db_is_zstd) {
            snprintf(out->error, sizeof(out->error),
                     "Failed to decompress %s (%s).", real_db_entry,
                     st == ZS_OK ? "empty collection" : zs_status_str(st));
        } else {
            snprintf(out->error, sizeof(out->error),
                     "Failed to extract %s from the archive (%s).", real_db_entry,
                     st == ZS_OK ? "empty collection" : zs_status_str(st));
        }
        out->db_path[0] = 0;
        mz_zip_reader_end(&zip);
        return false;
    }

    /* Extract the media map, if present. Absence is not an error. */
    if (mz_zip_reader_locate_file(&zip, "media", NULL, 0) >= 0) {
        char mp[600];
        snprintf(mp, sizeof(mp), "%s/media", tmp_dir);
        if (extract_entry(&zip, "media", false, APKG_MAX_MEDIA_MAP, mp, NULL) == ZS_OK)
            snprintf(out->media_json_path, sizeof(out->media_json_path), "%s", mp);
    }

    /* Count media entries by iterating the archive. Media files are the
       ones whose names are purely numeric ("0", "1", ...). We do NOT
       extract them yet — that happens during import, driven by the
       parsed media map, so we don't waste disk on unreferenced files. */
    mz_uint total = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < total; i++) {
        mz_zip_archive_file_stat fs;
        if (!mz_zip_reader_file_stat(&zip, i, &fs)) continue;
        if (!safe_entry_name(fs.m_filename)) continue;
        bool numeric = true;
        for (const char *p = fs.m_filename; *p; p++) {
            if (*p < '0' || *p > '9') { numeric = false; break; }
        }
        if (numeric) out->media_entry_count++;
    }

    mz_zip_reader_end(&zip);
    return true;
}

void apkg_close(ApkgHandle *h) {
    /* Nothing heap-allocated inside ApkgHandle today; kept for symmetry
       with future caching. */
    (void)h;
}

const char *apkg_variant_name(ApkgVariant v) {
    switch (v) {
        case APKG_VARIANT_LEGACY1: return "Anki 2.0 (legacy)";
        case APKG_VARIANT_LEGACY2: return "Anki 2.1 (compatibility)";
        case APKG_VARIANT_MODERN:  return "Anki 2.1.50+ (modern, zstd)";
        default:                   return "unknown";
    }
}
