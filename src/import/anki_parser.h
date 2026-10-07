/* anki_parser.h */
#ifndef ANKI_PARSER_H
#define ANKI_PARSER_H

#include "import/anki_types.h"
#include <stdbool.h>

/* Open the extracted SQLite file read-only and populate the collection.
   The caller must call anki_collection_free(). */
bool anki_parse(const char *db_path, AnkiCollection *out);

void anki_collection_free(AnkiCollection *c);

#endif
