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

typedef enum {
    CARD_NEW = 0,
    CARD_LEARNING,
    CARD_REVIEW,
    CARD_RELEARNING
} CardState;

/*
 * Card — extends the original front/back/tags layout with four optional
 * Japanese fields. A "generic" card just leaves them empty. Japanese
 * cards populate them and the study screen renders the Japanese layout.
 *
 * Memory: ~2 KB per card. DeckList is ~6 MB — never put one on the
 * stack. Use App (heap-allocated) or `static`.
 */
typedef struct Card {
    int id;
    char front[MAX_TEXT];
    char back[MAX_TEXT];
    char tags[MAX_TAGS];

    /* Japanese fields — all optional, empty when not applicable. */
    char japanese[MAX_TEXT];
    char reading[MAX_TEXT];
    char meaning[MAX_TEXT];
    char example[MAX_TEXT];

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
void decklist_init_sample(DeckList *dl);   /* now seeds both C and JP decks */

Deck *decklist_find(DeckList *dl, int id);
Deck *decklist_add(DeckList *dl, const char *name, Color color);
void  decklist_remove(DeckList *dl, int index);

Card *deck_add_card(Deck *d, const char *front, const char *back, const char *tags);
Card *deck_add_card_jp(Deck *d,
                       const char *japanese, const char *reading,
                       const char *meaning,  const char *example,
                       const char *tags);
void  deck_remove_card(Deck *d, int idx);
int   deck_due_count(const Deck *d, double now);
float deck_retention(const Deck *d);

bool card_is_japanese(const Card *c);

bool deck_export_csv(const Deck *d, const char *path);
int  deck_import_csv(Deck *d, const char *path);

#endif
