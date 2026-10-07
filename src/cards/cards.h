#ifndef CARDS_H
#define CARDS_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_DECKS       24
#define MAX_CARDS       128
#define MAX_TEXT        320
#define MAX_TAGS        96
#define MAX_DECK_NAME   64

/* Generic field model. Every card is a list of named fields. Field 0 is
   the prompt; fields 1..N-1 are revealed as the answer. Legacy CSV cards
   that only have front/back leave field_count = 0 and use the front/back
   strings directly. */
#define MAX_FIELDS      8
#define MAX_FIELD_NAME  48
#define MAX_FIELD_VALUE 384

typedef enum {
    CARD_NEW = 0,
    CARD_LEARNING,
    CARD_REVIEW,
    CARD_RELEARNING
} CardState;

typedef struct Card {
    int  id;

    /* Legacy / simple cards (CSV, manual editor). */
    char front[MAX_TEXT];
    char back[MAX_TEXT];
    char tags[MAX_TAGS];

    /* Ordered field list (Anki and other rich imports). */
    int  field_count;
    char field_names[MAX_FIELDS][MAX_FIELD_NAME];
    char field_values[MAX_FIELDS][MAX_FIELD_VALUE];

    /* Media references (filenames relative to ~/.studyquest/media). */
    char image_ref[256];
    char audio_ref[256];

    /* SRS state — unchanged. */
    CardState state;
    double interval_sec;
    float  ease;
    int    reps;
    int    lapses;
    double due;
    double last_review;
    int    last_rating;
} Card;

typedef struct Deck {
    int id;
    char name[MAX_DECK_NAME];
    Color color;
    Card cards[MAX_CARDS];
    int card_count;
} Deck;

typedef struct DeckList {
    Deck decks[MAX_DECKS];
    int count;
    int next_id;
} DeckList;

void decklist_init(DeckList *dl);
void decklist_init_sample(DeckList *dl);

Deck *decklist_find(DeckList *dl, int id);
Deck *decklist_add(DeckList *dl, const char *name, Color color);
void  decklist_remove(DeckList *dl, int index);

/* Simple card (front/back only). */
Card *deck_add_card(Deck *d, const char *front, const char *back, const char *tags);

/* Field-based card. `names` and `values` are parallel arrays of length n. */
Card *deck_add_card_fields(Deck *d,
                           const char (*names)[MAX_FIELD_NAME],
                           const char (*values)[MAX_FIELD_VALUE],
                           int n,
                           const char *tags);

void  deck_remove_card(Deck *d, int idx);
int   deck_due_count(const Deck *d, double now);
float deck_retention(const Deck *d);

/* Field helpers — safe with NULL and out-of-range indexes. */
void        card_add_field(Card *c, const char *name, const char *value);
const char *card_field_name (const Card *c, int i);   /* NULL if OOR */
const char *card_field_value(const Card *c, int i);   /* NULL if OOR */
const char *card_find_field (const Card *c, const char *name);

/* Rendering heuristic: true when the card has fields and field 0
   contains a CJK codepoint. Drives the big-centered layout. */
bool card_is_japanese(const Card *c);
bool card_has_cjk(const char *s);

/* Media. */
void card_set_image(Card *c, const char *filename);
void card_set_audio(Card *c, const char *filename);

bool deck_export_csv(const Deck *d, const char *path);
int  deck_import_csv(Deck *d, const char *path);

#endif
