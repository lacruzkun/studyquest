/* anki_parser.c */
#include "import/anki_parser.h"
#include "import/anki_json.h"
#include "import/pb_reader.h"
#include "core/utf8.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- row readers ------------------------------------------------- */

/* Anki's modern schema declares `COLLATE unicase` on some columns. Without a
   collation of that name SQLite refuses to touch those tables, so register a
   simple case-insensitive one. We only read, so exact Unicode folding does
   not matter — it just has to be a consistent ordering. */
static int unicase_cmp(void *unused, int na, const void *a, int nb, const void *b) {
    (void)unused;
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++) {
        int cx = x[i], cy = y[i];
        if (cx >= 'A' && cx <= 'Z') cx += 32;
        if (cy >= 'A' && cy <= 'Z') cy += 32;
        if (cx != cy) return cx < cy ? -1 : 1;
    }
    return na == nb ? 0 : (na < nb ? -1 : 1);
}

static bool read_col(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT ver, models, decks, dconf, conf, crt FROM col LIMIT 1;";
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
    out->crt = sqlite3_column_int64(st, 5);

    out->models_json = models ? strdup((const char *)models) : NULL;
    out->decks_json  = decks  ? strdup((const char *)decks)  : NULL;
    out->dconf_json  = dconf  ? strdup((const char *)dconf)  : NULL;
    out->conf_json   = conf   ? strdup((const char *)conf)   : NULL;

    sqlite3_finalize(st);

    /* Schema 18+ (Anki 2.1.50+) leaves these two columns empty and keeps the
       data in the notetypes/fields/templates/decks tables instead. */
    bool empty = (!out->models_json || !out->models_json[0]) &&
                 (!out->decks_json  || !out->decks_json[0]);
    out->modern_schema = empty;
    if (!empty && (!out->models_json || !out->decks_json)) {
        snprintf(out->error, sizeof(out->error),
                 "col row is missing models or decks JSON.");
        return false;
    }
    return true;
}

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        /* don't cut a UTF-8 sequence in half */
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Template message: { string q_format = 1; string a_format = 2; ... } */
static void parse_template_config(const void *blob, int len,
                                  char *qfmt, size_t qcap,
                                  char *afmt, size_t acap) {
    qfmt[0] = afmt[0] = 0;
    if (!blob || len <= 0) return;
    PbReader r;
    PbField f;
    pb_init(&r, blob, (size_t)len);
    while (pb_next(&r, &f)) {
        if (f.wire != 2) continue;
        if (f.field == 1) {
            size_t n = f.len < qcap - 1 ? f.len : qcap - 1;
            memcpy(qfmt, f.data, n); qfmt[n] = 0;
        } else if (f.field == 2) {
            size_t n = f.len < acap - 1 ? f.len : acap - 1;
            memcpy(afmt, f.data, n); afmt[n] = 0;
        }
    }
}

/* NotetypeConfig message: { Kind kind = 1; ... } with KIND_CLOZE = 1. */
static bool notetype_is_cloze(const void *blob, int len) {
    if (!blob || len <= 0) return false;
    PbReader r;
    PbField f;
    pb_init(&r, blob, (size_t)len);
    while (pb_next(&r, &f))
        if (f.field == 1 && f.wire == 0) return f.varint == 1;
    return false;
}

static bool read_modern_models(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    int count = 0;
    if (sqlite3_prepare_v2(db, "SELECT count(*) FROM notetypes;", -1, &st, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "This package uses an unrecognised collection layout (%s).",
                 sqlite3_errmsg(db));
        return false;
    }
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    if (count <= 0 || count > 100000) {
        snprintf(out->error, sizeof(out->error),
                 "The collection contains no usable note types.");
        return false;
    }

    out->models = (AnkiModel *)calloc((size_t)count, sizeof(AnkiModel));
    if (!out->models) {
        snprintf(out->error, sizeof(out->error), "Out of memory reading note types.");
        return false;
    }

    sqlite3_stmt *sm = NULL, *sf = NULL, *sg = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, name, config FROM notetypes;", -1, &sm, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT name FROM fields WHERE ntid = ?1 ORDER BY ord;", -1, &sf, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, "SELECT name, config FROM templates WHERE ntid = ?1 ORDER BY ord;", -1, &sg, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error), "note type query failed: %s", sqlite3_errmsg(db));
        sqlite3_finalize(sm); sqlite3_finalize(sf); sqlite3_finalize(sg);
        return false;
    }

    int rc, i = 0;
    while ((rc = sqlite3_step(sm)) == SQLITE_ROW && i < count) {
        AnkiModel *m = &out->models[i++];
        m->id = sqlite3_column_int64(sm, 0);
        copy_str(m->name, sizeof(m->name), (const char *)sqlite3_column_text(sm, 1));
        m->is_cloze = notetype_is_cloze(sqlite3_column_blob(sm, 2),
                                        sqlite3_column_bytes(sm, 2));

        sqlite3_reset(sf);
        sqlite3_bind_int64(sf, 1, m->id);
        while (sqlite3_step(sf) == SQLITE_ROW && m->field_count < ANKI_MAX_FIELDS) {
            copy_str(m->field_names[m->field_count], sizeof(m->field_names[0]),
                     (const char *)sqlite3_column_text(sf, 0));
            m->field_count++;
        }

        sqlite3_reset(sg);
        sqlite3_bind_int64(sg, 1, m->id);
        while (sqlite3_step(sg) == SQLITE_ROW && m->template_count < 16) {
            int k = m->template_count++;
            copy_str(m->templates[k].name, sizeof(m->templates[k].name),
                     (const char *)sqlite3_column_text(sg, 0));
            parse_template_config(sqlite3_column_blob(sg, 1), sqlite3_column_bytes(sg, 1),
                                  m->templates[k].qfmt, sizeof(m->templates[k].qfmt),
                                  m->templates[k].afmt, sizeof(m->templates[k].afmt));
        }
    }
    sqlite3_finalize(sm); sqlite3_finalize(sf); sqlite3_finalize(sg);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        snprintf(out->error, sizeof(out->error), "note type iteration failed: %s", sqlite3_errmsg(db));
        return false;
    }
    out->model_count = i;
    return true;
}

static bool read_modern_decks(sqlite3 *db, AnkiCollection *out) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT id, name, kind FROM decks;", -1, &st, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error), "deck query failed: %s", sqlite3_errmsg(db));
        return false;
    }
    int cap = 16, n = 0;
    AnkiDeck *arr = (AnkiDeck *)calloc((size_t)cap, sizeof(AnkiDeck));
    if (!arr) { sqlite3_finalize(st); return false; }

    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n >= cap) {
            cap *= 2;
            AnkiDeck *g = (AnkiDeck *)realloc(arr, sizeof(AnkiDeck) * (size_t)cap);
            if (!g) { free(arr); sqlite3_finalize(st); return false; }
            arr = g;
            memset(arr + n, 0, sizeof(AnkiDeck) * (size_t)(cap - n));
        }
        AnkiDeck *d = &arr[n++];
        d->id = sqlite3_column_int64(st, 0);

        /* Schema 18 separates hierarchy levels with 0x1f; the UI (and legacy
           packages) use "::". */
        const unsigned char *nm = sqlite3_column_text(st, 1);
        size_t w = 0;
        for (const unsigned char *p = nm; p && *p && w + 2 < sizeof(d->name); p++) {
            if (*p == 0x1f) { d->name[w++] = ':'; d->name[w++] = ':'; }
            else d->name[w++] = (char)*p;
        }
        d->name[w] = 0;

        /* Deck.KindContainer: field 1 = normal deck, field 2 = filtered. */
        const unsigned char *kind = (const unsigned char *)sqlite3_column_blob(st, 2);
        int kl = sqlite3_column_bytes(st, 2);
        d->is_filtered = (kind && kl > 0 && kind[0] == 0x12);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(arr);
        snprintf(out->error, sizeof(out->error), "deck iteration failed: %s", sqlite3_errmsg(db));
        return false;
    }
    out->decks = arr;
    out->deck_count = n;
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

        /* Anki stores fields as one UTF-8 string joined by 0x1f (unit
           separator). Read the SQLite byte length instead of treating the
           field as a fixed-size C buffer, then split without truncating the
           database value. */
        if (flds) {
            int total_bytes = sqlite3_column_bytes(st, 4);
            const char *p = (const char *)flds;
            const char *end_all = p + total_bytes;
            int fi = 0;
            while (p <= end_all && fi < ANKI_MAX_FIELDS) {
                const char *sep = memchr(p, '\x1f', (size_t)(end_all - p));
                const char *end = sep ? sep : end_all;
                size_t len = (size_t)(end - p);
                n->fields[fi] = malloc(len + 1);
                if (!n->fields[fi]) {
                    for (int k = 0; k < fi; k++) free(n->fields[k]);
                    sqlite3_finalize(st);
                    snprintf(out->error, sizeof(out->error),
                             "Out of memory while copying note fields.");
                    return false;
                }
                memcpy(n->fields[fi], p, len);
                n->fields[fi][len] = 0;

                size_t bad = 0;
                if (!sq_utf8_validate(n->fields[fi], &bad)) {
                    fprintf(stderr,
                            "StudyQuest ANKI: note %lld field %d contains invalid UTF-8 at byte %zu\n",
                            (long long)n->id, fi, bad);
                }
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
            "       factor, reps, lapses, odid FROM cards;",
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
        /* Cards sitting in a filtered (custom study) deck keep their real
           deck in odid; file them there, not under the temporary deck. */
        int64_t odid = sqlite3_column_int64(st, 12);
        if (odid != 0) c->did = odid;
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
    int flags = SQLITE_OPEN_READONLY;
    if (sqlite3_open_v2(db_path, &db, flags, NULL) != SQLITE_OK) {
        snprintf(out->error, sizeof(out->error),
                 "Could not open the collection database: %s",
                 sqlite3_errmsg(db));
        sqlite3_close(db);
        return false;
    }

    /* The file is untrusted: disable anything that could run code embedded
       in the schema (views/triggers calling functions) and register the
       collation modern collections rely on. */
    sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sqlite3_exec(db, "PRAGMA trusted_schema=OFF;", NULL, NULL, NULL);
    sqlite3_create_collation(db, "unicase", SQLITE_UTF8, NULL, unicase_cmp);

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

    if (out->modern_schema) {
        if (!read_modern_models(db, out) || !read_modern_decks(db, out)) {
            sqlite3_close(db);
            return false;
        }
    } else {
        /* Legacy schema: models/decks are JSON blobs in the col row. */
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
    }

    sqlite3_close(db);
    anki_collection_index_notes(out);
    return true;
}

void anki_collection_free(AnkiCollection *c) {
    if (!c) return;
    free(c->models);
    free(c->decks);
    if (c->notes) {
        for (int i = 0; i < c->note_count; i++)
            for (int k = 0; k < c->notes[i].field_count; k++)
                free(c->notes[i].fields[k]);
    }
    free(c->notes);
    free(c->cards);
    free(c->note_keys);
    free(c->models_json);
    free(c->decks_json);
    free(c->dconf_json);
    free(c->conf_json);
    memset(c, 0, sizeof(*c));
}

typedef struct { int64_t id; int idx; } NoteKey;

static int notekey_cmp(const void *a, const void *b) {
    const NoteKey *x = (const NoteKey *)a, *y = (const NoteKey *)b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return x->idx - y->idx;           /* stable: lowest index wins on duplicates */
}

void anki_collection_index_notes(AnkiCollection *c) {
    if (!c) return;
    free(c->note_keys);
    c->note_keys = NULL;
    if (c->note_count <= 0) return;
    NoteKey *k = (NoteKey *)malloc(sizeof(NoteKey) * (size_t)c->note_count);
    if (!k) return;                   /* lookups fall back to a linear scan */
    for (int i = 0; i < c->note_count; i++) { k[i].id = c->notes[i].id; k[i].idx = i; }
    qsort(k, (size_t)c->note_count, sizeof(NoteKey), notekey_cmp);
    c->note_keys = k;
}

const AnkiNote *anki_find_note(const AnkiCollection *c, int64_t nid) {
    if (!c) return NULL;
    if (c->note_keys) {
        const NoteKey *k = (const NoteKey *)c->note_keys;
        int lo = 0, hi = c->note_count - 1;
        while (lo < hi) {                       /* first key >= nid */
            int mid = lo + (hi - lo) / 2;
            if (k[mid].id < nid) lo = mid + 1; else hi = mid;
        }
        if (lo >= 0 && lo < c->note_count && k[lo].id == nid) return &c->notes[k[lo].idx];
        return NULL;
    }
    for (int i = 0; i < c->note_count; i++)
        if (c->notes[i].id == nid) return &c->notes[i];
    return NULL;
}
