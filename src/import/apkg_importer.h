/* apkg_importer.h */
#ifndef APKG_IMPORTER_H
#define APKG_IMPORTER_H

#include "import/anki_types.h"
#include <stdbool.h>

typedef enum {
    APKG_VARIANT_UNKNOWN = 0,
    APKG_VARIANT_LEGACY1,     /* collection.anki2 is real */
    APKG_VARIANT_LEGACY2,     /* collection.anki21 is real, anki2 is dummy */
    APKG_VARIANT_MODERN       /* collection.anki21b, zstd-compressed */
} ApkgVariant;

typedef struct {
    ApkgVariant variant;
    int         meta_version;      /* from the package's `meta` file: 1, 2 or 3;
                                      0 when absent (very old packages) */
    bool        media_zstd;        /* media files inside the zip are zstd frames */
    char        tmp_dir[512];      /* caller-owned temp dir for this import */
    char        db_path[600];      /* path to extracted SQLite file */
    char        media_json_path[600];  /* path to extracted media map (may not exist);
                                          JSON for legacy packages, protobuf (usually
                                          zstd-wrapped) for modern ones. Pass it to
                                          media_import_*, which understands both. */
    int         media_entry_count; /* numeric (media) entries present in the zip */
    char        error[512];        /* human-readable error if the call failed */
} ApkgHandle;

/* Open the package, detect variant, extract the real collection file to
   `tmp_dir`, and (if present) extract the media map. Does NOT open SQLite.

   Extraction is streamed with hard size caps, so hostile archives cannot
   exhaust memory or disk. */
bool apkg_open(const char *apkg_path, const char *tmp_dir, ApkgHandle *out);

/* Free internal resources. The extracted files on disk are caller-managed. */
void apkg_close(ApkgHandle *h);

/* Human-readable variant name for the preview UI. */
const char *apkg_variant_name(ApkgVariant v);

#endif
