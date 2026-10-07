#ifndef ANKI_CONVERT_H
#define ANKI_CONVERT_H

#include "import/anki_types.h"
#include "cards/cards.h"
#include <stdbool.h>

typedef struct {
    /* Media refs found in the note's rendered fields. */
    char media_refs[8][256];
    int  media_ref_count;

    /* Every field of the note, cleaned and in order. Field 0 is the
       prompt; the rest are the answer. */
    int  field_count;
    char field_names[MAX_FIELDS][MAX_FIELD_NAME];
    char field_values[MAX_FIELDS][MAX_FIELD_VALUE];
} ConvertedCard;

bool anki_convert_card(const AnkiCollection *col,
                       const AnkiCard *card,
                       ConvertedCard *out);

#endif
