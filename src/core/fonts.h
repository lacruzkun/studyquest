#ifndef FONTS_H
#define FONTS_H

#include "raylib.h"
#include "cards/cards.h"
#include <stdbool.h>

/*
 * FontSet holds every font the app uses. Loaded once at startup and
 * freed in app_destroy. Never load fonts inside a frame.
 *
 * All three sizes load the same codepoint set — the baseline ASCII/kana/
 * punctuation ranges plus every CJK glyph found in the user's cards.
 * That means any kanji the user has actually written a card for will
 * render at every size.
 */
typedef struct FontSet {
    Font body;               /* 22pt — metadata, body text */
    Font head;               /* 36pt — section headers, deck names */
    Font display;            /* 56pt — Japanese front, big numbers */
    bool is_default;         /* true → caller must not UnloadFont */
    char loaded_path[512];
    int  glyph_count;
} FontSet;

/* `user_cps` may be NULL / 0 — baseline set is still loaded. */
bool fonts_init(FontSet *fs, const int *user_cps, int user_count);
void fonts_free(FontSet *fs);

Font fonts_body   (const FontSet *fs);
Font fonts_head   (const FontSet *fs);
Font fonts_display(const FontSet *fs);
Font fonts_pick   (const FontSet *fs, int nominal_size);

/*
 * Walk every card in every deck and return a malloc'd, sorted,
 * de-duplicated array of every CJK codepoint found. Caller frees.
 * Returned array may be NULL if allocation fails; *out_count is 0.
 */
int *fonts_collect_from_decks(const DeckList *dl, int *out_count);

#endif
