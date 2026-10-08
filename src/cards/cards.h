#ifndef CARDS_H
#define CARDS_H

#include "raylib.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_DECKS       256
/* Hard sanity cap per deck (loads/imports). Decks hold a heap-grown card
   array, so this is only a guard against absurd/malicious input. */
#define MAX_CARDS       200000
#define MAX_TEXT        320
#define MAX_TAGS        96
#define MAX_DECK_NAME   64

/* Generic field model. Every card is a list of named fields. Field 0 is
   the prompt; fields 1..N-1 are revealed as the answer. Legacy CSV cards
   that only have front/back leave field_count = 0 and use the front/back
   strings directly. */
#define MAX_FIELDS      32
#define MAX_FIELD_NAME  64
#define MAX_FIELD_VALUE 384 /* retained for legacy CSV/sample stack buffers */
#define MAX_CARD_MEDIA_REFS 16

typedef enum {
    CARD_NEW = 0,
    CARD_LEARNING,
    CARD_REVIEW,
    CARD_RELEARNING
} CardState;

typedef struct Card {
    int  id;

    /* Stable identity of the source Anki card, used for duplicate
       detection on re-import. 0 means "not imported from Anki". */
    int64_t anki_card_id;

    /* Legacy / simple cards (CSV, manual editor). */
    char front[MAX_TEXT];
    char back[MAX_TEXT];
    char tags[MAX_TAGS];

    /* Ordered field list (Anki and other rich imports). */
    int  field_count;
    char field_names[MAX_FIELDS][MAX_FIELD_NAME];
    char *field_values[MAX_FIELDS];

    /* Media references. image_ref/audio_ref remain as legacy aliases to the
       first image/audio reference so existing UI/save code remains compatible. */
    char image_ref[512];
    char audio_ref[512];
    int  media_ref_count;
    struct {
        char ref[512];
        unsigned char kind; /* 0=image, 1=audio, 2=other */
        unsigned char side; /* 0=front, 1=back */
        unsigned char field_index; /* 0..MAX_FIELDS-1 for field media; 255=template/unknown */
    } media_refs[MAX_CARD_MEDIA_REFS];

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
    Card *cards;          /* heap-grown; capacity in card_capacity */
    int  card_count;
    int  card_capacity;
} Deck;

typedef struct DeckList {
    Deck decks[MAX_DECKS];
    int count;
    int next_id;
} DeckList;

void decklist_init(DeckList *dl);
void decklist_free(DeckList *dl);
void decklist_init_sample(DeckList *dl);

/* Free a deck's cards (and heap card array). The deck struct is left empty. */
void deck_free(Deck *d);

Deck *decklist_find(DeckList *dl, int id);
Deck *decklist_add(DeckList *dl, const char *name, Color color);
void  decklist_remove(DeckList *dl, int index);

/* Simple card (front/back only). */
Card *deck_add_card(Deck *d, const char *front, const char *back, const char *tags);

/* Field-based card. `names` and `values` are parallel arrays of length n. */
Card *deck_add_card_fields(Deck *d,
                           char (*names)[MAX_FIELD_NAME],
                           const char *const *values,
                           int n,
                           const char *tags);

void  deck_remove_card(Deck *d, int idx);
void  card_free(Card *c);
int   deck_due_count(const Deck *d, double now);
float deck_retention(const Deck *d);

/* Append a card by moving ownership of `src` (its heap field values) into
   `d`. Returns NULL if the deck is full. `src` is zeroed on success. */
Card *deck_add_card_move(Deck *d, Card *src);

/* True if any card in `d` carries the given Anki card id (id != 0). */
bool  deck_has_anki_card(const Deck *d, int64_t id);

/* Append `src` (by moving ownership) into `dl`, reassigning a fresh deck
   id. Returns NULL if the deck list is full. `src` is zeroed on success. */
Deck *decklist_append_deck(DeckList *dl, Deck *src);

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
void card_add_media_ref(Card *c, const char *filename, int kind, int side, int field_index);

bool deck_export_csv(const Deck *d, const char *path);
int  deck_import_csv(Deck *d, const char *path);

#endif
