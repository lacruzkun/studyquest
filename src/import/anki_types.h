#ifndef ANKI_TYPES_H
#define ANKI_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ANKI_MAX_FIELDS 32
#define ANKI_MAX_MEDIA_REFS 32

typedef struct {
    int64_t id;
    char    name[128];
    int     field_count;
    char    field_names[ANKI_MAX_FIELDS][64];

    /* Templates. Each has a qfmt (front) and afmt (back). */
    int     template_count;
    struct {
        char name[64];
        char qfmt[2048];
        char afmt[2048];
    } templates[16];

    bool    is_cloze;
} AnkiModel;

typedef struct {
    int64_t id;
    char    guid[64];
    int64_t mid;                 /* model id */
    char    tags[512];           /* space-separated, with leading/trailing space */
    char   *fields[ANKI_MAX_FIELDS]; /* heap-owned UTF-8 field values */
    int     field_count;
    int     sort_field_index;
} AnkiNote;

typedef struct {
    int64_t id;
    int64_t nid;                 /* note id */
    int64_t did;                 /* deck id */
    int     ord;                 /* template ordinal */
    int     type;                /* 0=new 1=learn 2=review 3=relearn */
    int     queue;
    int     due;
    int     ivl;                 /* interval in days */
    int     factor;              /* ease * 1000 */
    int     reps;
    int     lapses;
    int64_t mod;                 /* epoch seconds */
} AnkiCard;

typedef struct {
    int64_t id;
    char    name[256];           /* raw, e.g. "Japanese::Kanji" */
    int     usn;
    bool    is_filtered;
} AnkiDeck;

/* Aggregate collection parsed from a SQLite file. Defined here rather
   than in anki_parser.h so conversion modules can reference it without
   pulling in the SQLite include chain. */
typedef struct {
    AnkiModel *models; int model_count;
    AnkiDeck  *decks;  int deck_count;
    AnkiNote  *notes;  int note_count;
    AnkiCard  *cards;  int card_count;

    char *models_json;
    char *decks_json;
    char *dconf_json;
    char *conf_json;
    int   schema_version;
    int64_t crt;                 /* collection creation time (epoch secs); review
                                    due days are counted from here */
    void *note_keys;             /* sorted (id, index) pairs for O(log n) note lookup;
                                    built by anki_parse, optional for hand-built collections */
    bool  modern_schema;         /* models/decks came from schema-18 tables */

    char error[512];
} AnkiCollection;

/* Linear search by note id. Returns NULL if not found. */
const AnkiNote *anki_find_note(const AnkiCollection *c, int64_t nid);

#endif
