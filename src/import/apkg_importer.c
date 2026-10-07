/* apkg_importer.c */
#include "import/apkg_importer.h"
#include "miniz.h"
#include "zstd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

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
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

/* Reject any archive entry whose name contains a path traversal.
   Anki archive entries are flat (0, 1, collection.anki2, media, meta),
   so anything with a slash or .. is either malicious or unexpected. */
static bool safe_entry_name(const char *name) {
    if (!name || !*name) return false;
    if (name[0] == '/' || name[0] == '\\') return false;
    if (strstr(name, "..")) return false;
    if (strchr(name, '/') || strchr(name, '\\')) return false;
    return true;
}

/* Extract one archive entry to disk. Returns the number of bytes written,
   or -1 on failure. */
static long extract_entry_to_file(mz_zip_archive *zip, const char *entry_name,
                                  const char *dest_path) {
    size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(zip, entry_name, &size, 0);
    if (!data) return -1;

    FILE *f = fopen(dest_path, "wb");
    if (!f) { mz_free(data); return -1; }
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    mz_free(data);

    if (written != size) return -1;
    return (long)size;
}

/* Extract one entry, zstd-decompress it, write the result. */
static long extract_entry_zstd_to_file(mz_zip_archive *zip, const char *entry_name,
                                       const char *dest_path) {
    size_t size = 0;
    void *data = mz_zip_reader_extract_file_to_heap(zip, entry_name, &size, 0);
    if (!data) return -1;

    unsigned long long decompressed_size = ZSTD_getFrameContentSize(data, size);
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR ||
        decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        mz_free(data);
        return -1;
    }

    void *out = malloc((size_t)decompressed_size);
    if (!out) { mz_free(data); return -1; }

    size_t got = ZSTD_decompress(out, (size_t)decompressed_size, data, size);
    mz_free(data);

    if (ZSTD_isError(got)) { free(out); return -1; }

    FILE *f = fopen(dest_path, "wb");
    if (!f) { free(out); return -1; }
    size_t written = fwrite(out, 1, got, f);
    fclose(f);
    free(out);

    return written == got ? (long)got : -1;
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
        real_db_is_zstd = false;
        out->variant = APKG_VARIANT_LEGACY2;
    } else if (mz_zip_reader_locate_file(&zip, "collection.anki2", NULL, 0) >= 0) {
        real_db_entry = "collection.anki2";
        real_db_is_zstd = false;
        out->variant = APKG_VARIANT_LEGACY1;
    } else {
        snprintf(out->error, sizeof(out->error),
                 "The package does not contain a recognised Anki "
                 "collection file. Expected collection.anki2, "
                 "collection.anki21, or collection.anki21b.");
        mz_zip_reader_end(&zip);
        return false;
    }

    /* Extract the real collection. */
    snprintf(out->db_path, sizeof(out->db_path),
             "%s/%s", tmp_dir, real_db_entry);

    long n;
    if (real_db_is_zstd) {
        n = extract_entry_zstd_to_file(&zip, real_db_entry, out->db_path);
        if (n < 0) {
            snprintf(out->error, sizeof(out->error),
                     "Failed to decompress %s. The zstd stream may be "
                     "corrupt.", real_db_entry);
            mz_zip_reader_end(&zip);
            return false;
        }
    } else {
        n = extract_entry_to_file(&zip, real_db_entry, out->db_path);
        if (n < 0) {
            snprintf(out->error, sizeof(out->error),
                     "Failed to extract %s from the archive.", real_db_entry);
            mz_zip_reader_end(&zip);
            return false;
        }
    }

    /* Extract the media map, if present. Absence is not an error. */
    if (mz_zip_reader_locate_file(&zip, "media", NULL, 0) >= 0) {
        snprintf(out->media_json_path, sizeof(out->media_json_path),
                 "%s/media", tmp_dir);
        if (extract_entry_to_file(&zip, "media", out->media_json_path) < 0) {
            out->media_json_path[0] = 0;
        }
    }

    /* Count media entries by iterating the archive. Media files are the
       ones whose names are purely numeric ("0", "1", ...). We do NOT
       extract them yet — that happens during import, driven by the
       parsed media map, so we don't waste disk on unreferenced files. */
    mz_uint total = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < total; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        if (!safe_entry_name(st.m_filename)) continue;
        bool numeric = true;
        for (const char *p = st.m_filename; *p; p++) {
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
