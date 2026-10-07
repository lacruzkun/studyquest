#ifndef ASSETS_H
#define ASSETS_H

#include "raylib.h"
#include <stdbool.h>

#define ASSET_MAX_TEXTURES 24
#define ASSET_MAX_SOUNDS   12

typedef struct {
    Texture2D tex;
    char      filename[256];
    bool      loaded;
    bool      failed;      /* NEW: don't retry a load that already failed */
    double    last_used;
} TexEntry;

typedef struct {
    Sound  snd;
    char   filename[256];
    bool   loaded;
    bool   failed;         /* NEW */
    double last_used;
} SndEntry;

typedef struct AssetCache {
    char     media_root[512];
    TexEntry textures[ASSET_MAX_TEXTURES];
    SndEntry sounds[ASSET_MAX_SOUNDS];
    bool     audio_ready;
} AssetCache;

/* media_root should be the parent of images/ and audio/, e.g.
   ~/.studyquest/media. Pass NULL or empty to disable loading. */
void assets_init(AssetCache *ac, const char *media_root);
void assets_free(AssetCache *ac);

/* Return NULL if the file doesn't exist or fails to load. Cached on
   first use and evicted by LRU when the cache is full. */
Texture2D *assets_get_texture(AssetCache *ac, const char *filename);
Sound     *assets_get_sound  (AssetCache *ac, const char *filename);

/* Convenience: play immediately if resolvable. Safe with NULL/empty. */
void assets_play_sound(AssetCache *ac, const char *filename);

/* Stop every cached sound. Used when leaving the study screen. */
void assets_stop_all(AssetCache *ac);

#endif
