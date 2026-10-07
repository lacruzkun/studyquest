#ifndef FONTS_H
#define FONTS_H

#include "raylib.h"
#include "cards/cards.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct FontSet {
    Font body;
    Font head;
    Font display;
    Font fallback_body;
    Font fallback_head;
    Font fallback_display;
    bool is_default;
    bool fallback_loaded;
    char loaded_path[512];
    char fallback_path[512];
    int  glyph_count;
} FontSet;

bool fonts_init(FontSet *fs, const int *user_cps, int user_count);
void fonts_free(FontSet *fs);

Font fonts_body   (const FontSet *fs);
Font fonts_head   (const FontSet *fs);
Font fonts_display(const FontSet *fs);
Font fonts_pick   (const FontSet *fs, int nominal_size);
Font fonts_pick_fallback(const FontSet *fs, int nominal_size);
bool fonts_has_glyph(const FontSet *fs, int nominal_size, uint32_t codepoint);
bool fonts_has_fallback_glyph(const FontSet *fs, int nominal_size, uint32_t codepoint);

int *fonts_collect_from_decks(const DeckList *dl, int *out_count);

#endif
