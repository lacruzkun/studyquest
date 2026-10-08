#ifndef IMPORT_MANAGER_H
#define IMPORT_MANAGER_H

#include "import/anki_types.h"
#include "import/media_importer.h"
#include "cards/cards.h"
#include <stdbool.h>
#include <stdarg.h>

#define IMPORT_MAX_DECKS   256
#define IMPORT_WARNING_CAP 4096

/* One StudyQuest deck to be produced for a distinct Anki deck id. The name
   keeps the full "::" path (hierarchy is flattened into a single deck list,
   but the nesting is preserved in the name). */
typedef struct {
    int64_t anki_deck_id;
    char    name[MAX_DECK_NAME];
    int     card_count;
    int     note_count;
} ImportDeckSummary;

typedef struct {
    ImportDeckSummary decks[IMPORT_MAX_DECKS];
    int               deck_count;
} ImportDeckPlan;

/* Accumulates recoverable, non-fatal warnings (missing media, unsupported
   template features, etc.) shown on the import result screen. */
typedef struct {
    char message[IMPORT_WARNING_CAP];
    int  count;
} ImportWarnings;

/* Group the cards in `col` by deck id, computing a per-deck name (full "::"
   path) and card/note counts. Only decks that actually contain cards are
   listed. Returns false if there are more distinct decks than supported. */
bool import_build_deck_plan(const AnkiCollection *col, ImportDeckPlan *out);

/* Find the summary for a deck id, or NULL. */
const ImportDeckSummary *import_deck_summary_for(const ImportDeckPlan *p,
                                                 int64_t did);

/* Convert Anki's space-separated tag string (" a b c ") to StudyQuest's
   comma-separated form ("a,b,c"). */
void import_tags_to_csv(const char *anki_tags, char *out, size_t cap);

/* Generate a stable deck color from a name (no raylib randomness, so the
   same deck always gets the same hue). */
Color import_deck_color(const char *name);

/* Find a deck name that does not collide with `dl`; appends " (2)", " (3)"
   etc. if needed. Returns false if no unique name could be formed. */
bool import_unique_deck_name(const DeckList *dl, const char *base,
                             char *out, size_t cap);

/* ----------------------- warnings -------------------------------- */

void import_warnings_init(ImportWarnings *w);
void import_warnings_addf(ImportWarnings *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Fold a media import result into warnings (missing files and unsafe names
   are recoverable, not fatal). */
void import_warn_media(ImportWarnings *w, const MediaImportResult *mr);

/* ----------------------- scheduling ------------------------------ */

/* Convert an Anki card's scheduling state into StudyQuest SRS fields.
   Preserves ease (factor), reps, lapses, interval (ivl) and state (type).
   `due` is reset to "now" (immediately studyable) because Anki's due values
   use a different day-number scheme; maturity is preserved so StudyQuest's
   scheduler continues from the correct interval/ease. */
void import_apply_scheduling(const AnkiCard *ac, Card *out);

/* ----------------------- duplicates / commit --------------------- */

typedef enum {
    IMPORT_MODE_NEW = 0,      /* always create new decks (unique names) */
    IMPORT_MODE_UPDATE = 1    /* merge into same-named decks, dedupe by id */
} ImportMode;

/* Find a deck in `dl` by exact name, or NULL. */
Deck *import_find_existing_deck(DeckList *dl, const char *name);

/* Move the decks built into `scratch` into `app`. In UPDATE mode, cards
   whose Anki id already exists in a same-named deck are skipped (not
   duplicated). `scratch` is emptied either way; `app` is left consistent.
   Always returns true (failures are recorded as warnings). */
bool import_commit(DeckList *app, DeckList *scratch, ImportMode mode,
                   ImportWarnings *w);

/* ----------------------- conversion job -------------------------- */

/* Stateful, incremental conversion. The UI calls import_job_step() once or
   more per frame so large decks never block the render loop. */
typedef struct {
    const AnkiCollection     *col;
    const MediaImportResult  *media;   /* may be NULL (no media pass) */
    DeckList                 *target;  /* where decks/cards are written */
    ImportDeckPlan            plan;
    ImportWarnings            warnings;

    /* Map plan.decks[i] -> StudyQuest deck id in `target`. */
    int                       sq_deck_id[IMPORT_MAX_DECKS];
    bool                      deck_created[IMPORT_MAX_DECKS];

    int                       next_card;     /* index into col->cards */
    int                       cards_done;
    int                       cards_added;
    int                       notes_added;   /* distinct note ids */
    int64_t                  *seen_notes;
    int                       seen_note_count;
    int                       seen_note_cap;

    /* Model ids we've already emitted a template/cloze warning about. */
    int64_t                   warned_models[32];
    int                       warned_model_count;
} ImportJob;

/* Reset and bind the job. `target` is where decks/cards are written. */
void import_job_init(ImportJob *j, const AnkiCollection *col,
                     const MediaImportResult *media, DeckList *target);

/* Process up to `max_cards` cards. Returns how many were attempted this
   call (0 once the job is finished). Decomposes to import_job_done(). */
int  import_job_step(ImportJob *j, int max_cards);

bool import_job_done(const ImportJob *j);

/* Release transient allocations (does NOT free col/media/target). */
void import_job_free(ImportJob *j);

#endif
