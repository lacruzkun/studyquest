/* anki_parser.h */
#ifndef ANKI_PARSER_H
#define ANKI_PARSER_H

#include "import/anki_types.h"
#include <stdbool.h>

/* Open the extracted SQLite file read-only and populate the collection.
   The caller must call anki_collection_free(). */
bool anki_parse(const char *db_path, AnkiCollection *out);

void anki_collection_free(AnkiCollection *c);

/* Build (or rebuild) the sorted note-id index that makes anki_find_note()
   O(log n). anki_parse() calls this; hand-built collections may too. */
void anki_collection_index_notes(AnkiCollection *c);

#endif
