#include "fonts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* ------------------------------------------------------------------ */
/*  Baseline codepoint set                                             */
/*                                                                     */
/*  Everything the UI needs that is NOT derived from the user's        */
/*  cards: ASCII, punctuation, kana, fullwidth forms. The kanji the    */
/*  user actually studies come from fonts_collect_from_decks().        */
/* ------------------------------------------------------------------ */

static int *build_baseline_codepoints(int *out_count) {
    int cap = 1024;
    int *cps = (int *)malloc(sizeof(int) * cap);
    if (!cps) { *out_count = 0; return NULL; }
    int n = 0;

    #define PUSH(c) do { if (n < cap) cps[n++] = (c); } while (0)

    /* ASCII printable */
    for (int c = 0x20; c <= 0x7E; c++) PUSH(c);

    /* Latin-1 supplement (é, ü, ñ …) */
    for (int c = 0xA0; c <= 0xFF; c++) PUSH(c);

    /* Common general punctuation */
    for (int c = 0x2010; c <= 0x2027; c++) PUSH(c);
    PUSH(0x2030); PUSH(0x2032); PUSH(0x2033);
    PUSH(0x20AC);

    /* CJK Symbols and Punctuation — 。、「」etc. */
    for (int c = 0x3000; c <= 0x303F; c++) PUSH(c);

    /* Hiragana */
    for (int c = 0x3040; c <= 0x309F; c++) PUSH(c);

    /* Katakana */
    for (int c = 0x30A0; c <= 0x30FF; c++) PUSH(c);

    /* Halfwidth and Fullwidth Forms */
    for (int c = 0xFF00; c <= 0xFFEF; c++) PUSH(c);

    #undef PUSH

    qsort(cps, n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cps[i] != cps[m - 1]) cps[m++] = cps[i];

    *out_count = m;
    return cps;
}

/* ------------------------------------------------------------------ */
/*  CJK collection from user cards                                     */
/* ------------------------------------------------------------------ */

/* Include kana/fullwidth too so that if a user has a card with only
   kana in a field (e.g. reading), it's still covered even if they later
   edit the baseline set to drop it. Dedup handles duplicates. */
static bool is_cjk_codepoint(int cp) {
    return (cp >= 0x3000 && cp <= 0x30FF) ||   /* punct + hiragana + katakana */
           (cp >= 0x3400 && cp <= 0x4DBF) ||   /* CJK Ext A */
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   /* CJK Unified */
           (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK Compatibility */
           (cp >= 0xFF00 && cp <= 0xFFEF);     /* Halfwidth / Fullwidth */
}

/* Append every CJK codepoint found in `s` to cps[]. Returns new count. */
static int collect_cjk_from_string(const char *s, int *cps, int n, int cap) {
    if (!s) return n;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && n < cap) {
        int cp = 0;
        if (p[0] < 0x80) {
            cp = p[0]; p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x0F) << 12) |
                 ((p[1] & 0x3F) <<  6) |
                  (p[2] & 0x3F);          p += 3;
        } else if ((p[0] & 0xF8) == 0xF0 &&
                   (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x07) << 18) |
                 ((p[1] & 0x3F) << 12) |
                 ((p[2] & 0x3F) <<  6) |
                  (p[3] & 0x3F);          p += 4;
        } else {
            p++; continue;
        }
        if (is_cjk_codepoint(cp)) cps[n++] = cp;
    }
    return n;
}

int *fonts_collect_from_decks(const DeckList *dl, int *out_count) {
    if (out_count) *out_count = 0;
    if (!dl) return NULL;

    int cap = 4096;
    int *cps = (int *)malloc(sizeof(int) * cap);
    if (!cps) return NULL;
    int n = 0;

    for (int i = 0; i < dl->count && n < cap; i++) {
        const Deck *d = &dl->decks[i];
        for (int j = 0; j < d->card_count && n < cap; j++) {
            const Card *c = &d->cards[j];
            n = collect_cjk_from_string(c->front,    cps, n, cap);
            n = collect_cjk_from_string(c->back,     cps, n, cap);
            n = collect_cjk_from_string(c->japanese, cps, n, cap);
            n = collect_cjk_from_string(c->reading,  cps, n, cap);
            n = collect_cjk_from_string(c->meaning,  cps, n, cap);
            n = collect_cjk_from_string(c->example,  cps, n, cap);
        }
    }

    qsort(cps, n, sizeof(int), cmp_int);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (m == 0 || cps[i] != cps[m - 1]) cps[m++] = cps[i];

    if (out_count) *out_count = m;
    return cps;
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

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

bool fonts_init(FontSet *fs, const int *user_cps, int user_count) {
    memset(fs, 0, sizeof(*fs));

    int base_count = 0;
    int *base = build_baseline_codepoints(&base_count);

    /* Merge base + user, sort, dedup. */
    int total = base_count + (user_count > 0 ? user_count : 0);
    int *cps = (int *)malloc(sizeof(int) * total);
    if (!cps) {
        TraceLog(LOG_ERROR, "FONTS: OOM allocating codepoint list");
        free(base);
        fs->body = fs->head = fs->display = GetFontDefault();
        fs->is_default = true;
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
            "FONTS: no CJK font found. Japanese text will render as boxes.\n"
            "       Set STUDYQUEST_FONT or place NotoSansJP-Regular.ttf in assets/fonts/.");
        fs->body = fs->head = fs->display = GetFontDefault();
        fs->is_default = true;
        snprintf(fs->loaded_path, sizeof(fs->loaded_path), "(default)");
        free(cps);
        return false;
    }

    fs->body    = LoadFontEx(path, 22, cps, m);
    fs->head    = LoadFontEx(path, 36, cps, m);
    fs->display = LoadFontEx(path, 56, cps, m);

    if (fs->body.texture.id == 0 ||
        fs->head.texture.id == 0 ||
        fs->display.texture.id == 0) {
        TraceLog(LOG_ERROR, "FONTS: LoadFontEx failed for %s", path);
        if (fs->body.texture.id    != 0) UnloadFont(fs->body);
        if (fs->head.texture.id    != 0) UnloadFont(fs->head);
        if (fs->display.texture.id != 0) UnloadFont(fs->display);
        fs->body = fs->head = fs->display = GetFontDefault();
        fs->is_default = true;
        free(cps);
        return false;
    }

    SetTextureFilter(fs->body.texture,    TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fs->head.texture,    TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(fs->display.texture, TEXTURE_FILTER_BILINEAR);

    fs->is_default = false;
    fs->glyph_count = m;
    snprintf(fs->loaded_path, sizeof(fs->loaded_path), "%s", path);

    TraceLog(LOG_INFO, "FONTS: loaded %d glyphs (%d from cards) from %s",
             m, user_count, path);

    free(cps);
    return true;
}

void fonts_free(FontSet *fs) {
    if (!fs || fs->is_default) return;
    if (fs->body.texture.id    != 0) UnloadFont(fs->body);
    if (fs->head.texture.id    != 0) UnloadFont(fs->head);
    if (fs->display.texture.id != 0) UnloadFont(fs->display);
    fs->is_default = true;
}

Font fonts_body   (const FontSet *fs) { return fs->body; }
Font fonts_head   (const FontSet *fs) { return fs->head; }
Font fonts_display(const FontSet *fs) { return fs->display; }

Font fonts_pick(const FontSet *fs, int nominal_size) {
    if (!fs) return GetFontDefault();
    if (nominal_size >= 42) return fs->display;
    if (nominal_size >= 26) return fs->head;
    return fs->body;
}
