#ifndef ANKI_CONVERT_H
#define ANKI_CONVERT_H

#include "import/anki_types.h"
#include "cards/cards.h"
#include <stdbool.h>

typedef struct {
    /* Media refs found in note fields and card templates. */
    char media_refs[ANKI_MAX_MEDIA_REFS][512];
    unsigned char media_kinds[ANKI_MAX_MEDIA_REFS];
    unsigned char media_sides[ANKI_MAX_MEDIA_REFS];
    unsigned char media_fields[ANKI_MAX_MEDIA_REFS]; /* 0..MAX_FIELDS-1 for note fields; 255 for template media */
    int  media_ref_count;

    /* Every field of the note, cleaned and in order. Field 0 is the
       prompt; the rest are the answer. Values are heap-owned. */
    int  field_count;
    char field_names[MAX_FIELDS][MAX_FIELD_NAME];
    char *field_values[MAX_FIELDS];
} ConvertedCard;

void anki_converted_card_free(ConvertedCard *card);

bool anki_convert_card(const AnkiCollection *col,
                       const AnkiCard *card,
                       ConvertedCard *out);

#endif
