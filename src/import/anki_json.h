/* anki_json.h */
#ifndef ANKI_JSON_H
#define ANKI_JSON_H

#include "import/anki_types.h"
#include <stdbool.h>

bool anki_json_parse_models(const char *json, AnkiModel **out, int *count, char *err);
bool anki_json_parse_decks (const char *json, AnkiDeck  **out, int *count, char *err);

#endif
