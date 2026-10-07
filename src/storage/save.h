#ifndef SAVE_H
#define SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include "cards/cards.h"
#include "player/player.h"

#define SAVE_MAGIC   0x53515631u   /* "SQV1" */
#define SAVE_VERSION 6

typedef struct SaveData {
    DeckList decks;
    Player   player;
} SaveData;

/* Returns true on success. On failure, `out` is left in a safe state. */
bool save_load(SaveData *out, const char *path);
bool save_write(const SaveData *data, const char *path);

void save_defaults(SaveData *out);

/* Default path: $HOME/.studyquest.sav or ./studyquest.sav */
void save_default_path(char *out, size_t cap);

#endif
