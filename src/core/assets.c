#include "assets.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <ctype.h>

/* SQ_HAVE_WEBP is set by CMake (see CMakeLists.txt). Default 0 so the
   file still compiles if someone builds it out of tree. */
#ifndef SQ_HAVE_WEBP
#  define SQ_HAVE_WEBP 0
#endif

#if SQ_HAVE_WEBP
#  include <webp/decode.h>
#endif

static bool ends_with_ci(const char *s, const char *suffix) {
    size_t ls = strlen(s), lx = strlen(suffix);
    if (lx > ls) return false;
    const char *p = s + (ls - lx);
    for (size_t i = 0; i < lx; i++)
        if (tolower((unsigned char)p[i]) != tolower((unsigned char)suffix[i]))
            return false;
    return true;
}

static double now_sec(void) { return (double)GetTime(); }

void assets_init(AssetCache *ac, const char *media_root) {
    memset(ac, 0, sizeof(*ac));
    if (media_root && *media_root) {
        snprintf(ac->media_root, sizeof(ac->media_root), "%s", media_root);
    }
    ac->audio_ready = IsAudioDeviceReady();
}

void assets_free(AssetCache *ac) {
    if (!ac) return;
    for (int i = 0; i < ASSET_MAX_TEXTURES; i++) {
        if (ac->textures[i].loaded) {
            UnloadTexture(ac->textures[i].tex);
            ac->textures[i].loaded = false;
        }
    }
    for (int i = 0; i < ASSET_MAX_SOUNDS; i++) {
        if (ac->sounds[i].loaded) {
            UnloadSound(ac->sounds[i].snd);
            ac->sounds[i].loaded = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Texture cache                                                      */
/* ------------------------------------------------------------------ */

static int find_texture_slot(AssetCache *ac) {
    /* Prefer an empty slot, then evict LRU. */
    for (int i = 0; i < ASSET_MAX_TEXTURES; i++)
        if (!ac->textures[i].loaded) return i;

    int oldest = 0;
    double best = 1e30;
    for (int i = 0; i < ASSET_MAX_TEXTURES; i++) {
        if (ac->textures[i].last_used < best) {
            best = ac->textures[i].last_used;
            oldest = i;
        }
    }
    UnloadTexture(ac->textures[oldest].tex);
    ac->textures[oldest].loaded = false;
    return oldest;
}

#if SQ_HAVE_WEBP
static Texture2D load_webp(const char *path) {
    Texture2D empty = {0};

    int file_size = 0;
    unsigned char *buf = LoadFileData(path, &file_size);
    if (!buf || file_size <= 0) {
        if (buf) UnloadFileData(buf);
        return empty;
    }

    int w = 0, h = 0;
    uint8_t *rgba = WebPDecodeRGBA(buf, (size_t)file_size, &w, &h);
    UnloadFileData(buf);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) WebPFree(rgba);
        return empty;
    }

    /* Copy into a raylib Image. raylib's LoadTextureFromImage does not
       take ownership of the pixel buffer, so we can free after. */
    Image img = {
        .data    = rgba,
        .width   = w,
        .height  = h,
        .mipmaps = 1,
        .format  = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    Texture2D tex = LoadTextureFromImage(img);
    WebPFree(rgba);

    /* Match the filtering used for fonts. Smooth scaling for images. */
    if (tex.id != 0) {
        SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
    }
    return tex;
}
#endif

Texture2D *assets_get_texture(AssetCache *ac, const char *filename) {
    if (!ac || !filename || !*filename) return NULL;

    for (int i = 0; i < ASSET_MAX_TEXTURES; i++) {
        if (!ac->textures[i].loaded && !ac->textures[i].failed) continue;
        if (strcmp(ac->textures[i].filename, filename) != 0) continue;
        if (ac->textures[i].failed) return NULL;   /* remembered failure */
        ac->textures[i].last_used = now_sec();
        return &ac->textures[i].tex;
    }

    /* Look for an empty slot first. Don't evict to make room for
       something that might fail to load — but we can't know that
       until we try, so just use find_texture_slot() as before. */
    int slot = find_texture_slot(ac);
    TexEntry *e = &ac->textures[slot];

    /* Clear the slot in case find_texture_slot() evicted something. */
    if (e->loaded) { UnloadTexture(e->tex); e->loaded = false; }
    e->failed = false;
    snprintf(e->filename, sizeof(e->filename), "%s", filename);
    e->last_used = now_sec();

    /* Media refs come from imported Anki data.  Use only the final
       filename component so stale/legacy refs cannot escape media_root. */
    const char *base = strrchr(filename, '/');
    if (!base) base = strrchr(filename, '\\');
    base = base ? base + 1 : filename;

    char path[1024];
    snprintf(path, sizeof(path), "%s/images/%s", ac->media_root, base);
    if (!FileExists(path)) {
        TraceLog(LOG_WARNING, "ASSETS: image file missing: ref='%s' path='%s'",
                 filename, path);
        e->failed = true;
        return NULL;
    }

    Texture2D t;
    if (ends_with_ci(filename, ".webp")) {
#if SQ_HAVE_WEBP
        t = load_webp(path);
#else
        /* WebP support not compiled in. Record the failure so we don't
           retry every frame, and log once. */
        TraceLog(LOG_WARNING,
            "WebP image %s cannot be loaded — libwebp not compiled in.",
            filename);
        t = (Texture2D){0};
#endif
    } else {
        t = LoadTexture(path);
    }
    if (t.id == 0) {
        TraceLog(LOG_WARNING, "ASSETS: image load failed: ref='%s' path='%s'",
                 filename, path);
        e->failed = true;
        return NULL;
    }

    e->tex = t;
    e->loaded = true;
    return &e->tex;
}

/* ------------------------------------------------------------------ */
/*  Sound cache                                                        */
/* ------------------------------------------------------------------ */

static int find_sound_slot(AssetCache *ac) {
    for (int i = 0; i < ASSET_MAX_SOUNDS; i++)
        if (!ac->sounds[i].loaded) return i;

    int oldest = 0;
    double best = 1e30;
    for (int i = 0; i < ASSET_MAX_SOUNDS; i++) {
        if (ac->sounds[i].last_used < best) {
            best = ac->sounds[i].last_used;
            oldest = i;
        }
    }
    UnloadSound(ac->sounds[oldest].snd);
    ac->sounds[oldest].loaded = false;
    return oldest;
}

Sound *assets_get_sound(AssetCache *ac, const char *filename) {
    if (!ac || !ac->audio_ready || !filename || !*filename) return NULL;

    for (int i = 0; i < ASSET_MAX_SOUNDS; i++) {
        if (!ac->sounds[i].loaded && !ac->sounds[i].failed) continue;
        if (strcmp(ac->sounds[i].filename, filename) != 0) continue;
        if (ac->sounds[i].failed) return NULL;
        ac->sounds[i].last_used = now_sec();
        return &ac->sounds[i].snd;
    }

    int slot = find_sound_slot(ac);
    SndEntry *e = &ac->sounds[slot];
    if (e->loaded) { UnloadSound(e->snd); e->loaded = false; }
    e->failed = false;
    snprintf(e->filename, sizeof(e->filename), "%s", filename);
    e->last_used = now_sec();

    const char *base = strrchr(filename, '/');
    if (!base) base = strrchr(filename, '\\');
    base = base ? base + 1 : filename;

    char path[1024];
    snprintf(path, sizeof(path), "%s/audio/%s", ac->media_root, base);
    if (!FileExists(path)) {
        TraceLog(LOG_WARNING, "ASSETS: audio file missing: ref='%s' path='%s'",
                 filename, path);
        e->failed = true;
        return NULL;
    }

    Sound s = LoadSound(path);
    if (s.frameCount == 0) {
        TraceLog(LOG_WARNING, "ASSETS: audio load failed: ref='%s' path='%s'",
                 filename, path);
        e->failed = true;
        return NULL;
    }

    e->snd = s;
    e->loaded = true;
    return &e->snd;
}

void assets_play_sound(AssetCache *ac, const char *filename) {
    Sound *s = assets_get_sound(ac, filename);
    if (s) PlaySound(*s);
}

void assets_stop_all(AssetCache *ac) {
    if (!ac) return;
    for (int i = 0; i < ASSET_MAX_SOUNDS; i++)
        if (ac->sounds[i].loaded) StopSound(ac->sounds[i].snd);
}
