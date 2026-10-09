#ifndef MEDIA_IMPORTER_H
#define MEDIA_IMPORTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MEDIA_KIND_IMAGE = 0,
    MEDIA_KIND_AUDIO,
    MEDIA_KIND_OTHER
} MediaKind;

typedef struct {
    char      source_name[512];   /* name as it appears in cards */
    char      archive_entry[64];  /* zip entry name, e.g. "0", "1" */
    char      dest_name[512];     /* normalized name used by StudyQuest */
    MediaKind kind;
    bool      copied;             /* file is available under media_dir */
    bool      created;            /* this import created the file (rollback may delete it) */
} MediaEntry;

typedef struct {
    MediaEntry *entries;
    int         count;
    int         copied;           /* files now available (new + reused) */
    int         reused;           /* of those: identical file already present */
    int         missing;          /* listed in the map but absent from the archive */
    int         skipped_unsafe;   /* rejected by path/filename validation */
    int         too_large;        /* over the per-file or total size cap */
    int         corrupt;          /* present but undecodable */
    int         unresolved_refs;  /* card references that matched no media entry
                                     (filled in by the import job, not here) */
    uint64_t    bytes_written;
    char        media_dir[512];   /* root the files were written under */
    char        error[512];       /* filled when the whole operation fails */

    /* Internal: source_name -> entry index (open addressing). */
    int        *index;
    int         index_cap;
} MediaImportResult;

/* How the archive's media entries are encoded. */
typedef struct {
    bool     zstd_entries;      /* each archive entry is a zstd frame (package meta v3) */
    uint64_t max_file_bytes;    /* per-file cap; 0 = default (256 MiB) */
    uint64_t max_total_bytes;   /* whole-import cap; 0 = default (4 GiB) */
} MediaImportOptions;

/* Progress callback signature. Called from the same thread as the import.
   `done` and `total` are counts of processed entries. */
typedef void (*MediaProgressFn)(int done, int total, void *userdata);

/* ---------------- incremental interface (used by the UI) ------------- */

typedef struct {
    MediaImportResult *res;
    void              *zip;          /* opaque mz_zip_archive*, owned */
    MediaImportOptions opt;
    int                next;         /* next entry index to process */
    int                tmp_counter;
    bool               failed;       /* fatal error: res->error is set */
} MediaImportJob;

/*
 * Parse the media map and open the archive. The map may be:
 *   - a JSON object {"0":"cat.jpg","1":"dog.mp3"}        (legacy packages)
 *   - a protobuf MediaEntries message                     (Anki 2.1.50+)
 *   - either of the above wrapped in a zstd frame
 * `opt` may be NULL (raw entries, default caps).
 *
 * A missing/empty map is not an error: the job is simply already finished.
 * Returns false only for structural failures (res->error is set).
 */
bool media_import_begin(MediaImportJob *j, const char *apkg_path,
                        const char *media_map_path, const char *media_dir,
                        const MediaImportOptions *opt, MediaImportResult *res);

/* Process up to `max_entries` entries. Returns how many were processed (0
   when finished or failed). */
int  media_import_step(MediaImportJob *j, int max_entries);

bool media_import_done(const MediaImportJob *j);

/* Close the archive. Safe to call more than once. */
void media_import_end(MediaImportJob *j);

/* Delete every file this import newly created (files that already existed
   and were merely reused are left alone). Used when an import is cancelled
   or rolled back. */
void media_import_rollback(MediaImportResult *r);

/* ---------------- one-shot interface -------------------------------- */

/* Read the media map and copy each referenced archive entry from apkg_path
   into media_dir/<images|audio|other>/<sanitized_filename>.

   Missing archive entries are counted in `missing` and do NOT fail the
   import. Unsafe filenames are counted in `skipped_unsafe`. Only
   structural problems (unreadable map, unreadable archive) set `error`.

   The caller owns the result and must call media_import_free(). */
bool media_import_all(const char *apkg_path,
                      const char *media_json_path,
                      const char *media_dir,
                      MediaProgressFn on_progress,
                      void *userdata,
                      MediaImportResult *out);

bool media_import_all_ex(const char *apkg_path,
                         const char *media_map_path,
                         const char *media_dir,
                         const MediaImportOptions *opt,
                         MediaProgressFn on_progress,
                         void *userdata,
                         MediaImportResult *out);

void media_import_free(MediaImportResult *r);

/* Resolve an Anki media filename to the exact normalized filename written by
   the importer. Returns false when that source filename was not in the map
   (or its file could not be imported). */
bool media_import_resolve(const MediaImportResult *r, const char *source_name,
                          char *out_name, size_t out_cap);

/* Categorize a filename by extension. Used internally, exposed for tests. */
MediaKind media_kind_for(const char *filename);

#endif
