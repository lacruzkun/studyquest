#include "import/media_importer.h"
#include "import/zip_stream.h"
#include "import/pb_reader.h"

#include "miniz.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>

#define MEDIA_DEFAULT_MAX_FILE   (256ull * 1024 * 1024)
#define MEDIA_DEFAULT_MAX_TOTAL  (4ull * 1024 * 1024 * 1024)
#define MEDIA_MAX_MAP_BYTES      (32ull * 1024 * 1024)
#define MEDIA_MAX_ENTRIES        1000000

/* ================================================================== */
/*  Small filesystem helpers                                           */
/* ================================================================== */

static bool dir_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char *path) {
    if (!path || !*path) return false;
    char buf[1024];
    size_t n = strlen(path);
    if (n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);

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

static bool files_equal(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return false;
    if (sa.st_size != sb.st_size) return false;
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    bool eq = (fa && fb);
    unsigned char ba[65536], bb[65536];
    while (eq) {
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) eq = false;
        else if (na == 0) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return eq;
}

static uint64_t file_hash64(const char *path) {
    uint64_t h = 1469598103934665603ull;
    FILE *f = fopen(path, "rb");
    if (!f) return h;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        for (size_t i = 0; i < n; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    fclose(f);
    return h;
}

/* ================================================================== */
/*  Filename sanitization                                              */
/*                                                                     */
/*  Media filenames come from an untrusted archive. They may contain   */
/*  path separators, "..", leading slashes, NULs, or Windows drive     */
/*  letters. We reduce every name to a safe basename and reject it if  */
/*  nothing usable remains.                                            */
/* ================================================================== */

static bool sanitize_filename(const char *in, char *out, size_t cap) {
    if (!in || !*in) return false;
    if (cap < 2) return false;

    /* 1. Take the basename: strip anything up to the last '/' or '\\'. */
    const char *base = in;
    for (const char *p = in; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    if (*base == 0) return false;

    /* 2. Reject "." and "..". */
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return false;

    /* 3. Copy through, replacing anything that could cause trouble and
       rejecting control bytes. */
    size_t w = 0;
    const char *p = base;
    for (; *p && w + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;      /* control */
        if (c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            out[w++] = '_';
            continue;
        }
        out[w++] = (char)c;
    }
    /* Truncated in the middle of a UTF-8 sequence? If the first dropped byte
       is a continuation byte, the last character we copied is incomplete:
       remove it entirely (its continuation bytes, then its lead byte). */
    if (*p && ((unsigned char)*p & 0xC0) == 0x80) {
        while (w > 0 && ((unsigned char)out[w - 1] & 0xC0) == 0x80) w--;
        if (w > 0) w--;
    }
    out[w] = 0;
    if (w == 0) return false;

    /* 4. Refuse names that begin with a dot — hidden files inside a media
       dir are almost always malicious, and no legitimate Anki media does. */
    if (out[0] == '.') return false;

    return true;
}

static uint32_t fnv1a32(const char *s) {
    uint32_t h = 2166136261u;
    if (!s) return h;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/* ================================================================== */
/*  Media kind detection                                               */
/* ================================================================== */

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) return false;
    const char *p = s + (ls - lx);
    for (size_t i = 0; i < lx; i++) {
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

MediaKind media_kind_for(const char *filename) {
    static const char *images[] = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif", ".tiff",
        ".svg", ".ico", NULL
    };
    static const char *audio[] = {
        ".mp3", ".ogg", ".oga", ".wav", ".flac", ".m4a", ".opus", ".aac",
        ".wma", ".aiff", ".aif", NULL
    };
    for (int i = 0; images[i]; i++)
        if (ends_with_ci(filename, images[i])) return MEDIA_KIND_IMAGE;
    for (int i = 0; audio[i]; i++)
        if (ends_with_ci(filename, audio[i])) return MEDIA_KIND_AUDIO;
    return MEDIA_KIND_OTHER;
}

static const char *kind_subdir(MediaKind k) {
    switch (k) {
        case MEDIA_KIND_IMAGE: return "images";
        case MEDIA_KIND_AUDIO: return "audio";
        default:               return "other";
    }
}

/* Anki media entries are flat: "0", "1", "2", … Only a pure-digit name is
   accepted; anything else is either a differently-formatted archive or a
   hostile one. */
static bool is_numeric_entry(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

/* ================================================================== */
/*  Media map parsing (JSON, protobuf, optionally zstd-wrapped)        */
/* ================================================================== */

static void copy_trunc(char *dst, size_t cap, const char *src, size_t n) {
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Parses {"0":"cat.jpg", "1":"sub/dog.mp3", ...}. */
static bool parse_map_json(const char *text, MediaEntry **out_entries,
                           int *out_count, char *err, size_t err_cap) {
    cJSON *root = cJSON_Parse(text);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(err, err_cap, "Media map is not a JSON object.");
        return false;
    }

    int n = cJSON_GetArraySize(root);
    if (n > MEDIA_MAX_ENTRIES) {
        cJSON_Delete(root);
        snprintf(err, err_cap, "Media map lists too many files.");
        return false;
    }
    MediaEntry *arr = (MediaEntry *)calloc((size_t)(n > 0 ? n : 1), sizeof(MediaEntry));
    if (!arr) {
        cJSON_Delete(root);
        snprintf(err, err_cap, "Out of memory reading the media map.");
        return false;
    }

    int i = 0;
    cJSON *child = root->child;
    while (child && i < n) {
        /* The object key is the archive entry name ("0"); the value is the
           real filename ("cat.jpg"). */
        const char *key = child->string;
        if (!key || !cJSON_IsString(child) || !child->valuestring) {
            child = child->next;
            continue;
        }
        MediaEntry *e = &arr[i];
        copy_trunc(e->archive_entry, sizeof(e->archive_entry), key, strlen(key));
        copy_trunc(e->source_name, sizeof(e->source_name),
                   child->valuestring, strlen(child->valuestring));
        i++;
        child = child->next;
    }
    cJSON_Delete(root);
    *out_entries = arr;
    *out_count = i;
    return true;
}

/* Parses a protobuf MediaEntries message:
     message MediaEntries { repeated MediaEntry entries = 1; }
     message MediaEntry   { string name = 1; uint32 size = 2; bytes sha1 = 3;
                            optional uint32 legacy_zip_filename = 255; }
   The zip entry holding entry N is named by its position in the list (or by
   legacy_zip_filename when present). */
static bool parse_map_protobuf(const uint8_t *buf, size_t len,
                               MediaEntry **out_entries, int *out_count,
                               char *err, size_t err_cap) {
    /* First pass: count and validate. */
    int n = 0;
    PbReader r;
    PbField f;
    pb_init(&r, buf, len);
    while (pb_next(&r, &f)) {
        if (f.field != 1 || f.wire != 2) { r.bad = true; break; }
        if (++n > MEDIA_MAX_ENTRIES) {
            snprintf(err, err_cap, "Media map lists too many files.");
            return false;
        }
    }
    if (r.bad) {
        snprintf(err, err_cap,
                 "Media map is not valid JSON or a recognised Anki media list.");
        return false;
    }

    MediaEntry *arr = (MediaEntry *)calloc((size_t)(n > 0 ? n : 1), sizeof(MediaEntry));
    if (!arr) {
        snprintf(err, err_cap, "Out of memory reading the media map.");
        return false;
    }

    int idx = 0;
    pb_init(&r, buf, len);
    while (pb_next(&r, &f)) {
        PbReader er;
        PbField ef;
        pb_init(&er, f.data, f.len);
        const uint8_t *name = NULL;
        size_t name_len = 0;
        uint64_t legacy = 0;
        bool has_legacy = false;
        while (pb_next(&er, &ef)) {
            if (ef.field == 1 && ef.wire == 2) { name = ef.data; name_len = ef.len; }
            else if (ef.field == 255 && ef.wire == 0) { legacy = ef.varint; has_legacy = true; }
        }
        if (er.bad || !name || name_len == 0) {
            free(arr);
            snprintf(err, err_cap, "Media list contains a malformed entry.");
            return false;
        }
        MediaEntry *e = &arr[idx];
        copy_trunc(e->source_name, sizeof(e->source_name), (const char *)name, name_len);
        snprintf(e->archive_entry, sizeof(e->archive_entry), "%llu",
                 (unsigned long long)(has_legacy ? legacy : (uint64_t)idx));
        idx++;
    }
    *out_entries = arr;
    *out_count = idx;
    return true;
}

static bool parse_media_map(const char *path, MediaEntry **out_entries,
                            int *out_count, char *err, size_t err_cap) {
    *out_entries = NULL;
    *out_count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_cap, "Could not open media map: %s", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (unsigned long long)sz > MEDIA_MAX_MAP_BYTES) {
        fclose(f);
        snprintf(err, err_cap, "Media map has implausible size: %ld bytes", sz);
        return false;
    }
    if (sz == 0) { fclose(f); return true; }       /* empty map: no media */

    uint8_t *raw = (uint8_t *)malloc((size_t)sz + 1);
    if (!raw) { fclose(f); snprintf(err, err_cap, "Out of memory."); return false; }
    size_t got = fread(raw, 1, (size_t)sz, f);
    fclose(f);
    raw[got] = 0;

    uint8_t *data = raw;
    size_t   dlen = got;
    uint8_t *decoded = NULL;

    if (zs_looks_like_zstd(raw, got)) {
        size_t dl = 0;
        ZipStreamStatus st = zs_zstd_decode_mem(raw, got, MEDIA_MAX_MAP_BYTES,
                                                &decoded, &dl);
        if (st != ZS_OK) {
            free(raw);
            snprintf(err, err_cap, "Media map could not be decompressed (%s).",
                     zs_status_str(st));
            return false;
        }
        data = decoded;
        dlen = dl;
    }

    size_t k = 0;
    while (k < dlen && isspace(data[k])) k++;

    bool ok;
    if (k < dlen && data[k] == '{') {
        ok = parse_map_json((const char *)data, out_entries, out_count, err, err_cap);
    } else if (k >= dlen) {
        ok = true;                                   /* only whitespace */
    } else {
        ok = parse_map_protobuf(data, dlen, out_entries, out_count, err, err_cap);
    }

    free(decoded);
    free(raw);
    return ok;
}

/* ================================================================== */
/*  source_name -> entry index                                         */
/* ================================================================== */

static void index_build(MediaImportResult *r) {
    free(r->index);
    r->index = NULL;
    r->index_cap = 0;
    if (r->count <= 0) return;

    int cap = 16;
    while (cap < r->count * 2) cap <<= 1;
    int *slots = (int *)calloc((size_t)cap, sizeof(int));
    if (!slots) return;               /* resolve() falls back to a linear scan */

    for (int i = 0; i < r->count; i++) {
        uint32_t h = fnv1a32(r->entries[i].source_name);
        int pos = (int)(h & (uint32_t)(cap - 1));
        bool dup = false;
        while (slots[pos]) {
            if (strcmp(r->entries[slots[pos] - 1].source_name,
                       r->entries[i].source_name) == 0) { dup = true; break; }
            pos = (pos + 1) & (cap - 1);
        }
        if (!dup) slots[pos] = i + 1;   /* first occurrence wins */
    }
    r->index = slots;
    r->index_cap = cap;
}

bool media_import_resolve(const MediaImportResult *r, const char *source_name,
                          char *out_name, size_t out_cap) {
    if (!r || !source_name || !*source_name || !out_name || out_cap == 0) return false;

    const MediaEntry *hit = NULL;
    if (r->index && r->index_cap > 0) {
        uint32_t h = fnv1a32(source_name);
        int pos = (int)(h & (uint32_t)(r->index_cap - 1));
        while (r->index[pos]) {
            const MediaEntry *e = &r->entries[r->index[pos] - 1];
            if (strcmp(e->source_name, source_name) == 0) { hit = e; break; }
            pos = (pos + 1) & (r->index_cap - 1);
        }
    } else {
        for (int i = 0; i < r->count; i++) {
            if (strcmp(r->entries[i].source_name, source_name) == 0) {
                hit = &r->entries[i];
                break;
            }
        }
    }
    if (!hit || !hit->copied || !hit->dest_name[0]) return false;
    snprintf(out_name, out_cap, "%s", hit->dest_name);
    return true;
}

/* ================================================================== */
/*  Incremental import                                                 */
/* ================================================================== */

static void entry_path(const MediaImportResult *r, const MediaEntry *e,
                       char *out, size_t cap) {
    snprintf(out, cap, "%s/%s/%s", r->media_dir, kind_subdir(e->kind), e->dest_name);
}

bool media_import_begin(MediaImportJob *j, const char *apkg_path,
                        const char *media_map_path, const char *media_dir,
                        const MediaImportOptions *opt, MediaImportResult *res) {
    memset(j, 0, sizeof(*j));
    memset(res, 0, sizeof(*res));
    j->res = res;
    if (opt) j->opt = *opt;
    if (!j->opt.max_file_bytes)  j->opt.max_file_bytes  = MEDIA_DEFAULT_MAX_FILE;
    if (!j->opt.max_total_bytes) j->opt.max_total_bytes = MEDIA_DEFAULT_MAX_TOTAL;

    /* Absence of a media map is not an error. Some decks ship without
       media, and legacy packages occasionally skip the file entirely. */
    if (!media_map_path || !*media_map_path) return true;
    FILE *test = fopen(media_map_path, "rb");
    if (!test) return true;
    fclose(test);

    if (!media_dir || !*media_dir) {
        snprintf(res->error, sizeof(res->error), "No media directory was given.");
        return false;
    }
    snprintf(res->media_dir, sizeof(res->media_dir), "%s", media_dir);

    /* --- Parse the map ------------------------------------------- */
    MediaEntry *entries = NULL;
    int count = 0;
    if (!parse_media_map(media_map_path, &entries, &count,
                         res->error, sizeof(res->error))) {
        return false;
    }
    res->entries = entries;
    res->count   = count;
    index_build(res);
    if (count == 0) return true;

    /* --- Create the media directories ----------------------------- */
    if (!dir_exists(media_dir) && !mkdir_p(media_dir)) {
        snprintf(res->error, sizeof(res->error),
                 "Could not create media directory: %s", media_dir);
        return false;
    }
    static const char *subdirs[3] = { "images", "audio", "other" };
    for (int i = 0; i < 3; i++) {
        char sub[1100];
        snprintf(sub, sizeof(sub), "%s/%s", media_dir, subdirs[i]);
        if (!mkdir_p(sub)) {
            snprintf(res->error, sizeof(res->error),
                     "Could not create the '%s' media subdirectory.", subdirs[i]);
            return false;
        }
    }

    /* --- Open the archive ----------------------------------------- */
    mz_zip_archive *zip = (mz_zip_archive *)calloc(1, sizeof(*zip));
    if (!zip) {
        snprintf(res->error, sizeof(res->error), "Out of memory.");
        return false;
    }
    if (!mz_zip_reader_init_file(zip, apkg_path, 0)) {
        free(zip);
        snprintf(res->error, sizeof(res->error),
                 "Could not reopen the package for media extraction.");
        return false;
    }
    j->zip = zip;
    return true;
}

/* Move a fully extracted temp file into place, never overwriting a different
   file that is already there: identical content is reused, different content
   gets a content-hash suffix. */
static bool place_file(MediaImportResult *r, MediaEntry *e, const char *tmp_path,
                       const char *safe_name, uint64_t nbytes) {
    char dest[1200];
    snprintf(e->dest_name, sizeof(e->dest_name), "%s", safe_name);
    entry_path(r, e, dest, sizeof(dest));

    struct stat st;
    if (stat(dest, &st) != 0) {
        if (rename(tmp_path, dest) != 0) return false;
        e->created = true;
        r->bytes_written += nbytes;
        return true;
    }
    if (files_equal(tmp_path, dest)) {
        remove(tmp_path);
        e->created = false;
        r->reused++;
        return true;
    }

    /* Same name, different content: derive a collision-free name from the
       content itself so the same file always maps to the same name. */
    uint64_t h = file_hash64(tmp_path);
    const char *dot = strrchr(safe_name, '.');
    size_t stem = (dot && dot != safe_name) ? (size_t)(dot - safe_name)
                                            : strlen(safe_name);
    if (stem > 180) {                       /* keep names comfortably short */
        stem = 180;
        while (stem > 0 && ((unsigned char)safe_name[stem] & 0xC0) == 0x80) stem--;
    }
    snprintf(e->dest_name, sizeof(e->dest_name), "%.*s__%016llx%s",
             (int)stem, safe_name, (unsigned long long)h,
             (dot && dot != safe_name) ? dot : "");
    entry_path(r, e, dest, sizeof(dest));

    if (stat(dest, &st) != 0) {
        if (rename(tmp_path, dest) != 0) return false;
        e->created = true;
        r->bytes_written += nbytes;
        return true;
    }
    if (files_equal(tmp_path, dest)) {
        remove(tmp_path);
        e->created = false;
        r->reused++;
        return true;
    }
    return false;                           /* astronomically unlikely */
}

static void process_entry(MediaImportJob *j, int i) {
    MediaImportResult *r = j->res;
    MediaEntry *e = &r->entries[i];

    char safe[256];
    if (!sanitize_filename(e->source_name, safe, sizeof(safe)) ||
        !is_numeric_entry(e->archive_entry)) {
        r->skipped_unsafe++;
        return;
    }
    e->kind = media_kind_for(safe);

    if (r->bytes_written >= j->opt.max_total_bytes) { r->too_large++; return; }
    uint64_t cap = j->opt.max_file_bytes;
    uint64_t room = j->opt.max_total_bytes - r->bytes_written;
    if (cap > room) cap = room;

    char tmp_path[1200];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.import-%ld-%d.tmp",
             r->media_dir, (long)getpid(), j->tmp_counter++);
    FILE *fo = fopen(tmp_path, "wb");
    if (!fo) {
        j->failed = true;
        snprintf(r->error, sizeof(r->error),
                 "Could not write to the media directory (%s).", strerror(errno));
        return;
    }

    uint64_t nbytes = 0;
    ZipStreamStatus st = zs_extract_to_file((struct mz_zip_archive_tag *)j->zip,
                                            e->archive_entry, j->opt.zstd_entries,
                                            cap, fo, &nbytes);
    if (fclose(fo) != 0 && st == ZS_OK) st = ZS_IO;

    if (st != ZS_OK) {
        remove(tmp_path);
        switch (st) {
            case ZS_NOT_FOUND: r->missing++;   break;
            case ZS_TOO_BIG:   r->too_large++; break;
            case ZS_CORRUPT:   r->corrupt++;   break;
            default:
                j->failed = true;
                snprintf(r->error, sizeof(r->error),
                         "Writing media failed: %s.", zs_status_str(st));
                break;
        }
        return;
    }

    if (!place_file(r, e, tmp_path, safe, nbytes)) {
        remove(tmp_path);
        r->corrupt++;
        return;
    }
    e->copied = true;
    r->copied++;
}

int media_import_step(MediaImportJob *j, int max_entries) {
    if (!j || !j->res || !j->zip || j->failed) return 0;
    int done = 0;
    while (done < max_entries && j->next < j->res->count && !j->failed) {
        process_entry(j, j->next++);
        done++;
    }
    return done;
}

bool media_import_done(const MediaImportJob *j) {
    return !j || !j->res || !j->zip || j->failed || j->next >= j->res->count;
}

void media_import_end(MediaImportJob *j) {
    if (!j || !j->zip) return;
    mz_zip_reader_end((mz_zip_archive *)j->zip);
    free(j->zip);
    j->zip = NULL;
}

void media_import_rollback(MediaImportResult *r) {
    if (!r) return;
    for (int i = 0; i < r->count; i++) {
        MediaEntry *e = &r->entries[i];
        if (!e->created) continue;
        char path[1200];
        entry_path(r, e, path, sizeof(path));
        remove(path);
        e->created = false;
        e->copied = false;
    }
}

/* ================================================================== */
/*  One-shot wrappers                                                  */
/* ================================================================== */

bool media_import_all_ex(const char *apkg_path, const char *media_map_path,
                         const char *media_dir, const MediaImportOptions *opt,
                         MediaProgressFn on_progress, void *userdata,
                         MediaImportResult *out) {
    MediaImportJob job;
    if (!media_import_begin(&job, apkg_path, media_map_path, media_dir, opt, out)) {
        media_import_end(&job);
        return false;
    }
    while (!media_import_done(&job)) {
        media_import_step(&job, 1);
        if (on_progress) on_progress(job.next, out->count, userdata);
    }
    media_import_end(&job);
    return !job.failed;
}

bool media_import_all(const char *apkg_path, const char *media_json_path,
                      const char *media_dir, MediaProgressFn on_progress,
                      void *userdata, MediaImportResult *out) {
    return media_import_all_ex(apkg_path, media_json_path, media_dir, NULL,
                               on_progress, userdata, out);
}

void media_import_free(MediaImportResult *r) {
    if (!r) return;
    free(r->entries);
    free(r->index);
    memset(r, 0, sizeof(*r));
}
