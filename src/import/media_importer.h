#ifndef MEDIA_IMPORTER_H
#define MEDIA_IMPORTER_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MEDIA_KIND_IMAGE = 0,
    MEDIA_KIND_AUDIO,
    MEDIA_KIND_OTHER
} MediaKind;

typedef struct {
    char      source_name[512];   /* name as it appears in cards */
    char      archive_entry[64];  /* e.g. "0", "1" */
    char      dest_path[600];
    char      dest_name[512];   /* normalized name used by StudyQuest */     /* full path under media_dir */
    MediaKind kind;
    bool      copied;
} MediaEntry;

typedef struct {
    MediaEntry *entries;
    int         count;
    int         copied;
    int         missing;
    int         skipped_unsafe;   /* rejected by path validation */
    char        error[512];       /* filled when the whole operation fails */
} MediaImportResult;

/* Progress callback signature. Called from the same thread as the import.
   `done` and `total` are counts of processed entries. */
typedef void (*MediaProgressFn)(int done, int total, void *userdata);

/*
 * Read media_json_path (a JSON object like {"0":"cat.jpg","1":"dog.mp3"})
 * and copy each referenced archive entry from apkg_path into
 * media_dir/<images|audio|other>/<sanitized_filename>.
 *
 * Missing archive entries are counted in `missing` and do NOT fail the
 * import. Unsafe filenames are counted in `skipped_unsafe`. Only
 * structural problems (missing JSON, unreadable archive) set `error`.
 *
 * The caller owns the result and must call media_import_free().
 */
bool media_import_all(const char *apkg_path,
                      const char *media_json_path,
                      const char *media_dir,
                      MediaProgressFn on_progress,
                      void *userdata,
                      MediaImportResult *out);

void media_import_free(MediaImportResult *r);

/* Resolve an Anki media filename to the exact normalized filename written by
   the importer. Returns false when that source filename was not in the map. */
bool media_import_resolve(const MediaImportResult *r, const char *source_name,
                          char *out_name, size_t out_cap);

/* Categorize a filename by extension. Used internally, exposed for tests. */
MediaKind media_kind_for(const char *filename);

#endif
