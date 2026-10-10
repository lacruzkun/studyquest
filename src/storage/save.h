#ifndef SAVE_H
#define SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include "cards/cards.h"
#include "player/player.h"
#include "world/world_progress.h"

#define SAVE_MAGIC   0x53515631u   /* "SQV1" */
#define SAVE_VERSION 9

typedef struct SaveData {
    DeckList      decks;
    Player        player;
    WorldProgress world;          /* v9+; derived from legacy counters on older saves */
    int           loaded_version; /* version of the file that was loaded; 0 = defaults */
} SaveData;

/* Returns true on success. On failure, `out` is left in a safe state. */
bool save_load(SaveData *out, const char *path);
bool save_write(const SaveData *data, const char *path);

void save_defaults(SaveData *out);

/* Default path: $HOME/.studyquest.sav or ./studyquest.sav */
void save_default_path(char *out, size_t cap);

#endif
