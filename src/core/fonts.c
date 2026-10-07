#include "fonts.h"
#include "core/utf8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

static bool cp_in_font(Font font, uint32_t cp) {
    for (int i = 0; i < font.glyphCount; i++) {
        if ((uint32_t)font.glyphs[i].value == cp) return true;
    }
    return false;
}

static int push_cp(int **cps, int *count, int *cap, int cp) {
    if (cp < 0) return 0;
    if (*count >= *cap) {
        int new_cap = (*cap < 1024) ? 1024 : (*cap * 2);
        int *p = (int *)realloc(*cps, sizeof(int) * (size_t)new_cap);
        if (!p) return -1;
        *cps = p;
        *cap = new_cap;
    }
    (*cps)[(*count)++] = cp;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Baseline codepoint set                                             */
/* ------------------------------------------------------------------ */

static int *build_baseline_codepoints(int *out_count) {
    int cap = 1024;
    int *cps = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!cps) { *out_count = 0; return NULL; }
    int n = 0;

    #define PUSH(c) do { if (n < cap) cps[n++] = (c); } while (0)

    for (int c = 0x20; c <= 0x7E; c++) PUSH(c);
    for (int c = 0xA0; c <= 0xFF; c++) PUSH(c);
    for (int c = 0x2010; c <= 0x2027; c++) PUSH(c);
    PUSH(0x2030); PUSH(0x2032); PUSH(0x2033); PUSH(0x20AC);

    for (int c = 0x3000; c <= 0x303F; c++) PUSH(c);
    for (int c = 0x3040; c <= 0x309F; c++) PUSH(c);
    for (int c = 0x30A0; c <= 0x30FF; c++) PUSH(c);
    for (int c = 0xFF00; c <= 0xFFEF; c++) PUSH(c);

    #undef PUSH

    qsort(cps, n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cps[i] != cps[m - 1]) cps[m++] = cps[i];

    *out_count = m;
    return cps;
}

static bool is_cjk_codepoint(uint32_t cp) {
    return (cp >= 0x3000 && cp <= 0x30FF) ||
           (cp >= 0x3400 && cp <= 0x4DBF) ||
           (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0x20000 && cp <= 0x2FA1F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF);
}

static int collect_cjk_from_string(const char *s, int **cps, int *n, int *cap) {
    if (!s) return 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = 0;
        size_t len = 0;
        if (!sq_utf8_decode(p, &cp, &len)) {
            p++;
            continue;
        }
        if (is_cjk_codepoint(cp)) {
            if (push_cp(cps, n, cap, (int)cp) != 0) return -1;
        }
        p += len;
    }
    return 0;
}

int *fonts_collect_from_decks(const DeckList *dl, int *out_count) {
    if (out_count) *out_count = 0;
    if (!dl) return NULL;

    int cap = 4096;
    int *cps = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!cps) return NULL;
    int n = 0;

    for (int i = 0; i < dl->count; i++) {
        const Deck *d = &dl->decks[i];
        for (int j = 0; j < d->card_count; j++) {
            const Card *c = &d->cards[j];
            if (collect_cjk_from_string(c->front, &cps, &n, &cap) != 0) goto oom;
            if (collect_cjk_from_string(c->back,  &cps, &n, &cap) != 0) goto oom;
            for (int k = 0; k < c->field_count; k++)
                if (collect_cjk_from_string(c->field_values[k], &cps, &n, &cap) != 0) goto oom;
        }
    }

    qsort(cps, n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cps[i] != cps[m - 1]) cps[m++] = cps[i];

    if (out_count) *out_count = m;
    return cps;

oom:
    free(cps);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Font file discovery                                                */
/* ------------------------------------------------------------------ */

static const char *find_font_file(void) {
    const char *env = getenv("STUDYQUEST_FONT");
    if (env && *env && FileExists(env)) return env;

    static const char *paths[] = {
        "assets/fonts/NotoSansJP-Regular.ttf",
        "assets/fonts/NotoSansJP-Regular.otf",
        "assets/fonts/NotoSansJP-Medium.ttf",
        "assets/fonts/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansJP-Regular.otf",
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/local/share/fonts/NotoSansJP-Regular.ttf",
        "/run/current-system/sw/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/run/current-system/sw/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        NULL
    };
    for (int i = 0; paths[i]; i++)
        if (FileExists(paths[i])) return paths[i];
    return NULL;
}

static const char *find_fallback_font_file(const char *primary) {
    const char *env = getenv("STUDYQUEST_FONT_FALLBACK");
    if (env && *env && FileExists(env) && (!primary || strcmp(env, primary) != 0))
        return env;

    static const char *paths[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/run/current-system/sw/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/run/current-system/sw/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
        "/run/current-system/sw/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
        NULL
    };
    for (int i = 0; paths[i]; i++)
        if (FileExists(paths[i]) && (!primary || strcmp(paths[i], primary) != 0)) return paths[i];
    return NULL;
}

static void set_defaults(FontSet *fs) {
    fs->body = fs->head = fs->display = GetFontDefault();
    fs->fallback_body = fs->fallback_head = fs->fallback_display = GetFontDefault();
    fs->is_default = true;
}

static void unload_font_if_owned(Font f, bool owned) {
    if (owned && f.texture.id != 0) UnloadFont(f);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool fonts_init(FontSet *fs, const int *user_cps, int user_count) {
    if (!fs) return false;
    memset(fs, 0, sizeof(*fs));
    set_defaults(fs);

    int base_count = 0;
    int *base = build_baseline_codepoints(&base_count);
    if (!base) return false;

    int total = base_count + (user_count > 0 ? user_count : 0);
    int *cps = (int *)malloc(sizeof(int) * (size_t)(total > 0 ? total : 1));
    if (!cps) {
        free(base);
        TraceLog(LOG_ERROR, "FONTS: OOM allocating codepoint list");
        return false;
    }
    int n = 0;
    for (int i = 0; i < base_count; i++) cps[n++] = base[i];
    for (int i = 0; i < user_count; i++) cps[n++] = user_cps[i];
    free(base);

    qsort(cps, n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cps[i] != cps[m - 1]) cps[m++] = cps[i];

    const char *path = find_font_file();
    if (!path) {
        TraceLog(LOG_WARNING,
            "FONTS: no CJK font found. Set STUDYQUEST_FONT or install a Japanese-capable font.");
        free(cps);
        snprintf(fs->loaded_path, sizeof(fs->loaded_path), "(default)");
        return false;
    }

    fs->body = LoadFontEx(path, 22, cps, m);
    fs->head = LoadFontEx(path, 36, cps, m);
    fs->display = LoadFontEx(path, 56, cps, m);

    if (fs->body.texture.id == 0 || fs->head.texture.id == 0 || fs->display.texture.id == 0) {
        TraceLog(LOG_ERROR, "FONTS: LoadFontEx failed for primary font %s", path);
        unload_font_if_owned(fs->body, fs->body.texture.id != 0);
        unload_font_if_owned(fs->head, fs->head.texture.id != 0);
        unload_font_if_owned(fs->display, fs->display.texture.id != 0);
        set_defaults(fs);
        snprintf(fs->loaded_path, sizeof(fs->loaded_path), "(default)");
        free(cps);
        return false;
    }

    SetTextureFilter(fs->body.texture,    TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fs->head.texture,    TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fs->display.texture, TEXTURE_FILTER_BILINEAR);

    fs->is_default = false;
    fs->glyph_count = m;
    snprintf(fs->loaded_path, sizeof(fs->loaded_path), "%s", path);

    const char *fallback = find_fallback_font_file(path);
    int missing = 0;
    for (int i = 0; i < m; i++) {
        uint32_t cp = (uint32_t)cps[i];
        if (!cp_in_font(fs->body, cp) || !cp_in_font(fs->head, cp) || !cp_in_font(fs->display, cp))
            missing++;
    }

    if (missing > 0 && fallback) {
        fs->fallback_body = LoadFontEx(fallback, 22, cps, m);
        fs->fallback_head = LoadFontEx(fallback, 36, cps, m);
        fs->fallback_display = LoadFontEx(fallback, 56, cps, m);
        if (fs->fallback_body.texture.id != 0 &&
            fs->fallback_head.texture.id != 0 &&
            fs->fallback_display.texture.id != 0) {
            fs->fallback_loaded = true;
            SetTextureFilter(fs->fallback_body.texture,    TEXTURE_FILTER_BILINEAR);
            SetTextureFilter(fs->fallback_head.texture,    TEXTURE_FILTER_BILINEAR);
            SetTextureFilter(fs->fallback_display.texture, TEXTURE_FILTER_BILINEAR);
            snprintf(fs->fallback_path, sizeof(fs->fallback_path), "%s", fallback);
        } else {
            if (fs->fallback_body.texture.id) UnloadFont(fs->fallback_body);
            if (fs->fallback_head.texture.id) UnloadFont(fs->fallback_head);
            if (fs->fallback_display.texture.id) UnloadFont(fs->fallback_display);
            fs->fallback_body = fs->fallback_head = fs->fallback_display = GetFontDefault();
        }
    }

    int unresolved = 0;
    for (int i = 0; i < m; i++) {
        uint32_t cp = (uint32_t)cps[i];
        bool ok = cp_in_font(fs->body, cp) || (fs->fallback_loaded && cp_in_font(fs->fallback_body, cp));
        if (!ok && is_cjk_codepoint(cp)) {
            unresolved++;
            TraceLog(LOG_WARNING, "FONTS: no glyph coverage for U+%04X", (unsigned)cp);
        }
    }

    TraceLog(LOG_INFO, "FONTS: primary=%s glyphs=%d card_cjk=%d missing_primary=%d fallback=%s unresolved_cjk=%d",
             fs->loaded_path, m, user_count, missing,
             fs->fallback_loaded ? fs->fallback_path : "none", unresolved);

    free(cps);
    return true;
}

void fonts_free(FontSet *fs) {
    if (!fs) return;
    if (!fs->is_default) {
        if (fs->body.texture.id) UnloadFont(fs->body);
        if (fs->head.texture.id) UnloadFont(fs->head);
        if (fs->display.texture.id) UnloadFont(fs->display);
    }
    if (fs->fallback_loaded) {
        if (fs->fallback_body.texture.id) UnloadFont(fs->fallback_body);
        if (fs->fallback_head.texture.id) UnloadFont(fs->fallback_head);
        if (fs->fallback_display.texture.id) UnloadFont(fs->fallback_display);
    }
    memset(fs, 0, sizeof(*fs));
    set_defaults(fs);
}

Font fonts_body   (const FontSet *fs) { return fs->body; }
Font fonts_head   (const FontSet *fs) { return fs->head; }
Font fonts_display(const FontSet *fs) { return fs->display; }
Font fonts_pick   (const FontSet *fs, int nominal_size) {
    if (nominal_size >= 48) return fs->display;
    if (nominal_size >= 32) return fs->head;
    return fs->body;
}
Font fonts_pick_fallback(const FontSet *fs, int nominal_size) {
    if (!fs || !fs->fallback_loaded) return GetFontDefault();
    if (nominal_size >= 48) return fs->fallback_display;
    if (nominal_size >= 32) return fs->fallback_head;
    return fs->fallback_body;
}
bool fonts_has_glyph(const FontSet *fs, int nominal_size, uint32_t codepoint) {
    if (!fs) return false;
    return cp_in_font(fonts_pick(fs, nominal_size), codepoint);
}
bool fonts_has_fallback_glyph(const FontSet *fs, int nominal_size, uint32_t codepoint) {
    if (!fs || !fs->fallback_loaded) return false;
    return cp_in_font(fonts_pick_fallback(fs, nominal_size), codepoint);
}
