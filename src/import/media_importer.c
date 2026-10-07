#include "import/media_importer.h"

#include "miniz.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>

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

    /* 2. Reject control bytes, and reject "." and ".." outright. */
    if (strcmp(base, ".") == 0 || strcmp(base, "..") == 0) return false;

    /* 3. Copy through, replacing anything that could cause trouble. */
    size_t w = 0;
    for (const char *p = base; *p && w + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20)            return false;      /* control */
        if (c == ':' || c == '*'
            || c == '?' || c == '"'
            || c == '<' || c == '>'
            || c == '|') {
            out[w++] = '_';
            continue;
        }
        out[w++] = (char)c;
    }
    out[w] = 0;
    if (w == 0) return false;

    /* 4. Refuse to produce a name that begins with a dot — hidden files
       inside a media dir are almost always malicious. Allow dotfiles
       only if they're a plain extension like ".htaccess" — actually,
       just refuse entirely, no legitimate Anki media starts with '.'. */
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

static void make_dest_name(const char *source, const char *safe_base,
                           const MediaEntry *entries, int count,
                           char *out, size_t cap) {
    snprintf(out, cap, "%s", safe_base);
    for (int i = 0; i < count; i++) {
        if (!entries[i].copied || strcmp(entries[i].dest_name, out) != 0) continue;
        if (strcmp(entries[i].source_name, source) == 0) return;

        const char *dot = strrchr(safe_base, '.');
        uint32_t h = fnv1a32(source);
        if (dot && dot != safe_base) {
            size_t stem = (size_t)(dot - safe_base);
            if (stem > 400) stem = 400;
            char base[512];
            snprintf(base, sizeof(base), "%.*s__%08x%s",
                     (int)stem, safe_base, h, dot);
            snprintf(out, cap, "%s", base);
        } else {
            snprintf(out, cap, "%s__%08x", safe_base, h);
        }
        return;
    }
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

/* ================================================================== */
/*  Archive entry name validation                                      */
/*                                                                     */
/*  Anki media entries are flat: "0", "1", "2", … We only accept a     */
/*  pure-digit name here; anything else is either a differently-       */
/*  formatted archive or a hostile one.                                */
/* ================================================================== */

static bool is_numeric_entry(const char *name) {
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

/* ================================================================== */
/*  Media map parsing                                                  */
/* ================================================================== */

/* Parses {"0":"cat.jpg", "1":"sub/dog.mp3", ...}. Out-array is
   malloc'd; caller frees. Returns false only on structural failure. */
static bool parse_media_map(const char *json_path,
                            MediaEntry **out_entries, int *out_count,
                            char *err, size_t err_cap) {
    *out_entries = NULL;
    *out_count = 0;

    FILE *f = fopen(json_path, "rb");
    if (!f) {
        snprintf(err, err_cap, "Could not open media map: %s", json_path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 32 * 1024 * 1024) {
        fclose(f);
        snprintf(err, err_cap, "Media map has implausible size: %ld bytes", sz);
        return false;
    }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        snprintf(err, err_cap, "Media map is not a JSON object.");
        return false;
    }

    int n = cJSON_GetArraySize(root);
    MediaEntry *arr = (MediaEntry *)calloc((size_t)(n > 0 ? n : 1),
                                           sizeof(MediaEntry));
    if (!arr) { cJSON_Delete(root); return false; }

    int i = 0;
    cJSON *child = root->child;
    while (child && i < n) {
        /* In Anki's map, the object key is the archive entry name
           ("0") and the value is the real filename ("cat.jpg"). */
        const char *key = child->string;
        if (!key || !cJSON_IsString(child)) { child = child->next; continue; }

        MediaEntry *e = &arr[i];
        snprintf(e->archive_entry, sizeof(e->archive_entry), "%s", key);
        snprintf(e->source_name,   sizeof(e->source_name),
                 "%s", child->valuestring);
        i++;
        child = child->next;
    }

    cJSON_Delete(root);
    *out_entries = arr;
    *out_count   = i;
    return true;
}

/* ================================================================== */
/*  Public entry point                                                 */
/* ================================================================== */

bool media_import_all(const char *apkg_path,
                      const char *media_json_path,
                      const char *media_dir,
                      MediaProgressFn on_progress,
                      void *userdata,
                      MediaImportResult *out) {
    memset(out, 0, sizeof(*out));

    /* Absence of a media map is not an error. Some decks ship without
       media, and legacy packages occasionally skip the file entirely. */
    if (!media_json_path || !*media_json_path) {
        return true;
    }

    FILE *test = fopen(media_json_path, "rb");
    if (!test) return true;   /* no map → nothing to do */
    fclose(test);

    if (!dir_exists(media_dir) && !mkdir_p(media_dir)) {
        snprintf(out->error, sizeof(out->error),
                 "Could not create media directory: %s", media_dir);
        return false;
    }

    /* --- Parse the map ------------------------------------------- */
    MediaEntry *entries = NULL;
    int count = 0;
    if (!parse_media_map(media_json_path, &entries, &count,
                         out->error, sizeof(out->error))) {
        return false;
    }
    out->entries = entries;
    out->count   = count;

    if (count == 0) return true;

    /* --- Pre-create the three category subdirectories ------------- */
    char sub[400];
    const char *subdirs[3] = { "images", "audio", "other" };
    for (int i = 0; i < 3; i++) {
        snprintf(sub, sizeof(sub), "%s/%s", media_dir, subdirs[i]);
        if (!mkdir_p(sub)) {
            snprintf(out->error, sizeof(out->error),
                     "Could not create media subdirectory: %s", sub);
            return false;
        }
    }

    /* --- Open the archive ----------------------------------------- */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, apkg_path, 0)) {
        snprintf(out->error, sizeof(out->error),
                 "Could not reopen the package for media extraction.");
        return false;
    }

    /* --- Copy each referenced entry ------------------------------- */
    for (int i = 0; i < count; i++) {
        MediaEntry *e = &entries[i];

        /* Sanitize the destination filename. This is where we defeat
           "../../etc/passwd" and friends. */
        char safe_name[256];
        if (!sanitize_filename(e->source_name, safe_name, sizeof(safe_name))) {
            out->skipped_unsafe++;
            if (on_progress) on_progress(i + 1, count, userdata);
            continue;
        }

        /* The archive entry must be a plain numeric name. */
        if (!is_numeric_entry(e->archive_entry)) {
            out->skipped_unsafe++;
            if (on_progress) on_progress(i + 1, count, userdata);
            continue;
        }

        /* Resolve which subdir this file belongs in. */
        e->kind = media_kind_for(safe_name);
        make_dest_name(e->source_name, safe_name, entries, i,
                       e->dest_name, sizeof(e->dest_name));
        snprintf(e->dest_path, sizeof(e->dest_path),
                 "%s/%s/%s", media_dir, kind_subdir(e->kind), e->dest_name);

        /* Extract. A missing archive entry is a warning, not an error. */
        size_t size = 0;
        void *data = mz_zip_reader_extract_file_to_heap(
            &zip, e->archive_entry, &size, 0);
        if (!data) {
            out->missing++;
            if (on_progress) on_progress(i + 1, count, userdata);
            continue;
        }

        FILE *fo = fopen(e->dest_path, "wb");
        if (!fo) {
            mz_free(data);
            out->missing++;
            if (on_progress) on_progress(i + 1, count, userdata);
            continue;
        }
        size_t written = fwrite(data, 1, size, fo);
        fclose(fo);
        mz_free(data);

        if (written == size) {
            e->copied = true;
            out->copied++;
        } else {
            /* Partial write — remove the truncated file so we don't
               leave something half-imported on disk. */
            remove(e->dest_path);
            out->missing++;
        }

        if (on_progress) on_progress(i + 1, count, userdata);
    }

    mz_zip_reader_end(&zip);
    return true;
}

void media_import_free(MediaImportResult *r) {
    if (!r) return;
    free(r->entries);
    r->entries = NULL;
    r->count = 0;
}

bool media_import_resolve(const MediaImportResult *r, const char *source_name,
                          char *out_name, size_t out_cap) {
    if (!r || !source_name || !*source_name || !out_name || out_cap == 0) return false;
    for (int i = 0; i < r->count; i++) {
        const MediaEntry *e = &r->entries[i];
        if (strcmp(e->source_name, source_name) == 0) {
            if (!e->copied || !e->dest_name[0]) return false;
            snprintf(out_name, out_cap, "%s", e->dest_name);
            return true;
        }
    }
    return false;
}
