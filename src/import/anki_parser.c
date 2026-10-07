/* anki_parser.c */
#include "import/anki_parser.h"
#include "import/anki_json.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- row readers ------------------------------------------------- */

static bool read_col(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT ver, models, decks, dconf, conf FROM col LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "col table query failed: %s", sqlite3_errmsg(db));
        return false;
    }
    if (sqlite3_step(st) != SQLITE_ROW) {
        snprintf(out->error, sizeof(out->error),
                 "col table is empty — package has no collection data.");
        sqlite3_finalize(st);
        return false;
    }

    out->schema_version = sqlite3_column_int(st, 0);

    const unsigned char *models = sqlite3_column_text(st, 1);
    const unsigned char *decks  = sqlite3_column_text(st, 2);
    const unsigned char *dconf  = sqlite3_column_text(st, 3);
    const unsigned char *conf   = sqlite3_column_text(st, 4);

    out->models_json = models ? strdup((const char *)models) : NULL;
    out->decks_json  = decks  ? strdup((const char *)decks)  : NULL;
    out->dconf_json  = dconf  ? strdup((const char *)dconf)  : NULL;
    out->conf_json   = conf   ? strdup((const char *)conf)   : NULL;

    sqlite3_finalize(st);

    if (!out->models_json || !out->decks_json) {
        snprintf(out->error, sizeof(out->error),
                 "col row is missing models or decks JSON.");
        return false;
    }
    return true;
}

static bool read_notes(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, guid, mid, tags, flds, sfld FROM notes;",
            -1, &st, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "notes query failed: %s", sqlite3_errmsg(db));
        return false;
    }

    int cap = 256;
    out->notes = malloc(sizeof(AnkiNote) * cap);
    out->note_count = 0;

    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (out->note_count >= cap) {
            cap *= 2;
            AnkiNote *n = realloc(out->notes, sizeof(AnkiNote) * cap);
            if (!n) { sqlite3_finalize(st); return false; }
            out->notes = n;
        }
        AnkiNote *n = &out->notes[out->note_count];
        memset(n, 0, sizeof(*n));
        n->id  = sqlite3_column_int64(st, 0);
        n->mid = sqlite3_column_int64(st, 2);

        const unsigned char *guid = sqlite3_column_text(st, 1);
        const unsigned char *tags = sqlite3_column_text(st, 3);
        const unsigned char *flds = sqlite3_column_text(st, 4);
        n->sort_field_index = sqlite3_column_int(st, 5);

        if (guid) snprintf(n->guid, sizeof(n->guid), "%s", (const char *)guid);
        if (tags) snprintf(n->tags, sizeof(n->tags), "%s", (const char *)tags);

        /* Anki stores fields as a single string joined by 0x1f (unit
           separator). Split it here so downstream code sees plain fields. */
        if (flds) {
            const char *p = (const char *)flds;
            int fi = 0;
            while (*p && fi < ANKI_MAX_FIELDS) {
                const char *sep = strchr(p, '\x1f');
                size_t len = sep ? (size_t)(sep - p) : strlen(p);
                if (len >= ANKI_MAX_FIELD_LEN) len = ANKI_MAX_FIELD_LEN - 1;
                memcpy(n->fields[fi], p, len);
                n->fields[fi][len] = 0;
                fi++;
                if (!sep) break;
                p = sep + 1;
            }
            n->field_count = fi;
        }
        out->note_count++;
    }
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        snprintf(out->error, sizeof(out->error),
                 "notes iteration failed: %s", sqlite3_errmsg(db));
        return false;
    }
    return true;
}

static bool read_cards(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, nid, did, ord, mod, type, queue, due, ivl, "
            "       factor, reps, lapses FROM cards;",
            -1, &st, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "cards query failed: %s", sqlite3_errmsg(db));
        return false;
    }

    int cap = 256;
    out->cards = malloc(sizeof(AnkiCard) * cap);
    out->card_count = 0;

    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (out->card_count >= cap) {
            cap *= 2;
            AnkiCard *c = realloc(out->cards, sizeof(AnkiCard) * cap);
            if (!c) { sqlite3_finalize(st); return false; }
            out->cards = c;
        }
        AnkiCard *c = &out->cards[out->card_count++];
        c->id     = sqlite3_column_int64(st, 0);
        c->nid    = sqlite3_column_int64(st, 1);
        c->did    = sqlite3_column_int64(st, 2);
        c->ord    = sqlite3_column_int(st, 3);
        c->mod    = sqlite3_column_int64(st, 4);
        c->type   = sqlite3_column_int(st, 5);
        c->queue  = sqlite3_column_int(st, 6);
        c->due    = sqlite3_column_int(st, 7);
        c->ivl    = sqlite3_column_int(st, 8);
        c->factor = sqlite3_column_int(st, 9);
        c->reps   = sqlite3_column_int(st, 10);
        c->lapses = sqlite3_column_int(st, 11);
    }
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) {
        snprintf(out->error, sizeof(out->error),
                 "cards iteration failed: %s", sqlite3_errmsg(db));
        return false;
    }
    return true;
}

bool anki_parse(const char *db_path, AnkiCollection *out) {
    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_URI;
    if (sqlite3_open_v2(db_path, &db, flags, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "Could not open the collection database: %s",
                 sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }

    /* Integrity check — cheap and catches truncated/malicious files. */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *res = sqlite3_column_text(st, 0);
            if (res && strcmp((const char *)res, "ok") != 0) {
                snprintf(out->error, sizeof(out->error),
                         "The collection database failed its integrity check.");
                sqlite3_finalize(st);
                sqlite3_close(db);
                return false;
            }
        }
        sqlite3_finalize(st);
    }

    if (!read_col(db, out) ||
        !read_notes(db, out) ||
        !read_cards(db, out)) {
        sqlite3_close(db);
        return false;
    }

    /* Parse the JSON blobs into AnkiModel / AnkiDeck arrays. */
    if (!anki_json_parse_models(out->models_json,
                                &out->models, &out->model_count, out->error)) {
        sqlite3_close(db);
        return false;
    }
    if (!anki_json_parse_decks(out->decks_json,
                               &out->decks, &out->deck_count, out->error)) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
}

void anki_collection_free(AnkiCollection *c) {
    if (!c) return;
    free(c->models);
    free(c->decks);
    free(c->notes);
    free(c->cards);
    free(c->models_json);
    free(c->decks_json);
    free(c->dconf_json);
    free(c->conf_json);
    memset(c, 0, sizeof(*c));
}

const AnkiNote *anki_find_note(const AnkiCollection *c, int64_t nid) {
    if (!c) return NULL;
    for (int i = 0; i < c->note_count; i++)
        if (c->notes[i].id == nid) return &c->notes[i];
    return NULL;
}
