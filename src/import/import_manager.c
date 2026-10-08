#include "import/import_manager.h"
#include "import/anki_convert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>

/* ================================================================== */
/*  Deck plan                                                          */
/* ================================================================== */

static ImportDeckSummary *plan_find(ImportDeckPlan *p, int64_t did) {
    for (int i = 0; i < p->deck_count; i++)
        if (p->decks[i].anki_deck_id == did) return &p->decks[i];
    return NULL;
}

/* Full "::" path, truncated to MAX_DECK_NAME-1. */
static void deck_name_from_anki(const AnkiCollection *col, int64_t did,
                                char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < col->deck_count; i++) {
        if (col->decks[i].id == did) {
            snprintf(out, cap, "%.*s", (int)(cap - 1), col->decks[i].name);
            return;
        }
    }
    /* Unknown deck id (missing from decks JSON): fall back to a stable label. */
    snprintf(out, cap, "Deck %lld", (long long)did);
}

bool import_build_deck_plan(const AnkiCollection *col, ImportDeckPlan *out) {
    memset(out, 0, sizeof(*out));

    for (int ci = 0; ci < col->card_count; ci++) {
        int64_t did = col->cards[ci].did;
        ImportDeckSummary *s = plan_find(out, did);
        if (!s) {
            if (out->deck_count >= IMPORT_MAX_DECKS) return false;
            s = &out->decks[out->deck_count++];
            s->anki_deck_id = did;
            deck_name_from_anki(col, did, s->name, sizeof(s->name));
            s->card_count = 0;
            s->note_count = 0;
        }
        s->card_count++;
    }

    /* Distinct note count per deck (a note can generate several cards in
       the same deck; count it once). */
    for (int di = 0; di < out->deck_count; di++) {
        ImportDeckSummary *s = &out->decks[di];
        int distinct = 0;
        for (int ci = 0; ci < col->card_count; ci++) {
            if (col->cards[ci].did != s->anki_deck_id) continue;
            int64_t nid = col->cards[ci].nid;
            bool seen = false;
            for (int cj = 0; cj < ci; cj++) {
                if (col->cards[cj].did == s->anki_deck_id &&
                    col->cards[cj].nid == nid) { seen = true; break; }
            }
            if (!seen) distinct++;
        }
        s->note_count = distinct;
    }

    return true;
}

const ImportDeckSummary *import_deck_summary_for(const ImportDeckPlan *p,
                                                 int64_t did) {
    return plan_find((ImportDeckPlan *)p, did);
}

void import_tags_to_csv(const char *anki_tags, char *out, size_t cap) {
    out[0] = 0;
    if (!anki_tags || !*anki_tags) return;
    size_t w = 0;
    const char *p = anki_tags;
    while (*p && w + 1 < cap) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (w > 0 && w + 1 < cap) out[w++] = ',';
        size_t n = len < (cap - w - 1) ? len : (cap - w - 1);
        memcpy(out + w, start, n);
        w += n;
    }
    out[w] = 0;
}

Color import_deck_color(const char *name) {
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)(name ? name : ""); *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    /* Soft, StudyQuest-like blue/purple/pink range. */
    Color c;
    c.r = (unsigned char)(90 + (h % 110));
    c.g = (unsigned char)(100 + ((h >> 8) % 110));
    c.b = (unsigned char)(170 + ((h >> 16) % 60));
    c.a = 255;
    return c;
}

bool import_unique_deck_name(const DeckList *dl, const char *base,
                             char *out, size_t cap) {
    char trimmed[MAX_DECK_NAME];
    snprintf(trimmed, sizeof(trimmed), "%.*s", MAX_DECK_NAME - 8, base ? base : "");

    bool taken = false;
    for (int i = 0; i < dl->count; i++)
        if (strcmp(dl->decks[i].name, trimmed) == 0) { taken = true; break; }
    if (!taken) {
        snprintf(out, cap, "%.*s", (int)(cap - 1), trimmed);
        return true;
    }
    for (int n = 2; n < 1000; n++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s (%d)", trimmed, n);
        bool found = false;
        for (int i = 0; i < dl->count; i++)
            if (strcmp(dl->decks[i].name, buf) == 0) { found = true; break; }
        if (!found) { snprintf(out, cap, "%.*s", (int)(cap - 1), buf); return true; }
    }
    return false;
}

/* ================================================================== */
/*  Warnings                                                           */
/* ================================================================== */

void import_warnings_init(ImportWarnings *w) {
    memset(w, 0, sizeof(*w));
}

void import_warnings_addf(ImportWarnings *w, const char *fmt, ...) {
    if (!w) return;
    size_t used = strlen(w->message);
    if (used >= sizeof(w->message) - 2) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    int n = snprintf(w->message + used, sizeof(w->message) - used, "%s\n", buf);
    if (n > 0) w->count++;
}

void import_warn_media(ImportWarnings *w, const MediaImportResult *mr) {
    if (!w || !mr) return;
    if (mr->missing > 0) {
        import_warnings_addf(w, "%d media file(s) referenced by cards were "
                                "missing from the package.", mr->missing);
    }
    if (mr->skipped_unsafe > 0) {
        import_warnings_addf(w, "%d media file(s) were skipped because their "
                                "filename was not safe.", mr->skipped_unsafe);
    }
}

/* ================================================================== */
/*  Conversion job                                                     */
/* ================================================================== */

static int job_group_index(const ImportJob *j, int64_t did) {
    for (int i = 0; i < j->plan.deck_count; i++)
        if (j->plan.decks[i].anki_deck_id == did) return i;
    return -1;
}

static const AnkiModel *job_find_model(const AnkiCollection *col, int64_t mid) {
    for (int i = 0; i < col->model_count; i++)
        if (col->models[i].id == mid) return &col->models[i];
    return NULL;
}

static void job_warn_model(ImportJob *j, const AnkiModel *m) {
    if (!m) return;
    for (int i = 0; i < j->warned_model_count; i++)
        if (j->warned_models[i] == m->id) return;
    if (j->warned_model_count < 32)
        j->warned_models[j->warned_model_count++] = m->id;

    if (m->is_cloze) {
        import_warnings_addf(&j->warnings,
            "Note type '%s' is a cloze type; cloze deletions are not "
            "reproduced and cards are simplified.", m->name);
    }
    if (m->template_count > 1) {
        import_warnings_addf(&j->warnings,
            "Note type '%s' has %d card templates; StudyQuest imports one "
            "front/back card per Anki card.", m->name, m->template_count);
    }
}

static bool job_note_seen(ImportJob *j, int64_t nid) {
    for (int i = 0; i < j->seen_note_count; i++)
        if (j->seen_notes[i] == nid) return true;
    if (j->seen_note_count >= j->seen_note_cap) {
        int cap = j->seen_note_cap ? j->seen_note_cap * 2 : 64;
        int64_t *n = (int64_t *)realloc(j->seen_notes, sizeof(int64_t) * (size_t)cap);
        if (!n) return true;  /* OOM: treat as seen to avoid double count */
        j->seen_notes = n;
        j->seen_note_cap = cap;
    }
    j->seen_notes[j->seen_note_count++] = nid;
    return false;
}

void import_job_init(ImportJob *j, const AnkiCollection *col,
                     const MediaImportResult *media, DeckList *target) {
    memset(j, 0, sizeof(*j));
    j->col = col;
    j->media = media;
    j->target = target;
    import_warnings_init(&j->warnings);
    /* Best effort: build the plan, ignoring failure (treated as no decks). */
    if (col) import_build_deck_plan(col, &j->plan);
}

static Deck *job_ensure_deck(ImportJob *j, int group_idx) {
    if (j->deck_created[group_idx]) {
        return decklist_find(j->target, j->sq_deck_id[group_idx]);
    }
    char final_name[MAX_DECK_NAME];
    if (!import_unique_deck_name(j->target, j->plan.decks[group_idx].name,
                                 final_name, sizeof(final_name))) {
        import_warnings_addf(&j->warnings,
                             "Too many decks — '%s' skipped.",
                             j->plan.decks[group_idx].name);
        j->deck_created[group_idx] = true; /* mark done to avoid retry */
        return NULL;
    }
    Deck *d = decklist_add(j->target, final_name,
                           import_deck_color(final_name));
    if (!d) {
        import_warnings_addf(&j->warnings,
                             "Could not create deck '%s' (deck limit).", final_name);
        j->deck_created[group_idx] = true;
        return NULL;
    }
    j->sq_deck_id[group_idx] = d->id;
    j->deck_created[group_idx] = true;
    return d;
}

int import_job_step(ImportJob *j, int max_cards) {
    if (!j || !j->col || !j->target) return 0;
    if (j->next_card >= j->col->card_count) return 0;

    int attempted = 0;
    while (attempted < max_cards && j->next_card < j->col->card_count) {
        int idx = j->next_card++;
        attempted++;
        const AnkiCard *ac = &j->col->cards[idx];

        int gi = job_group_index(j, ac->did);
        if (gi < 0) {
            import_warnings_addf(&j->warnings,
                                 "Card %d references unknown deck id; skipped.", idx);
            continue;
        }

        Deck *d = job_ensure_deck(j, gi);
        if (!d) continue;

        ConvertedCard cc;
        if (!anki_convert_card(j->col, ac, &cc)) {
            import_warnings_addf(&j->warnings,
                                 "Card %d could not be converted; skipped.", idx);
            continue;
        }

        /* Tags come from the note, not the card. */
        char tags[MAX_TAGS] = {0};
        const AnkiNote *note = anki_find_note(j->col, ac->nid);
        if (note && note->tags[0]) import_tags_to_csv(note->tags, tags, sizeof(tags));

        /* Warn (once per note type) about template features StudyQuest
           cannot reproduce faithfully. */
        if (note) job_warn_model(j, job_find_model(j->col, note->mid));

        Card *card;
        if (cc.field_count > 0) {
            card = deck_add_card_fields(d, cc.field_names,
                                        (const char *const *)cc.field_values,
                                        cc.field_count, tags);
        } else {
            card = deck_add_card(d, "", "", tags);
        }

        if (!card) {
            import_warnings_addf(&j->warnings,
                                 "Deck '%s' is full; card %d skipped.", d->name, idx);
            anki_converted_card_free(&cc);
            continue;
        }

        /* Attach media refs, resolving Anki source names to the normalized
           filenames written by the media importer. */
        for (int m = 0; m < cc.media_ref_count; m++) {
            char resolved[512];
            const char *ref = cc.media_refs[m];
            if (j->media &&
                media_import_resolve(j->media, cc.media_refs[m],
                                     resolved, sizeof(resolved))) {
                ref = resolved;
            }
            card_add_media_ref(card, ref, cc.media_kinds[m],
                               cc.media_sides[m], cc.media_fields[m]);
        }

        j->cards_added++;
        if (!job_note_seen(j, ac->nid)) j->notes_added++;

        /* Preserve the stable Anki identity and scheduling state. */
        card->anki_card_id = ac->id;
        import_apply_scheduling(ac, card);

        anki_converted_card_free(&cc);
    }

    j->cards_done = j->next_card;
    return attempted;
}

bool import_job_done(const ImportJob *j) {
    return !j || !j->col || j->next_card >= j->col->card_count;
}

void import_job_free(ImportJob *j) {
    if (!j) return;
    free(j->seen_notes);
    j->seen_notes = NULL;
    j->seen_note_count = 0;
    j->seen_note_cap = 0;
}

/* ================================================================== */
/*  Scheduling                                                         */
/* ================================================================== */

void import_apply_scheduling(const AnkiCard *ac, Card *out) {
    if (!ac || !out) return;

    if (ac->factor > 0) {
        float e = (float)ac->factor / 1000.0f;
        if (e < 1.3f) e = 1.3f;
        if (e > 3.0f) e = 3.0f;
        out->ease = e;
    }
    out->reps   = ac->reps > 0 ? ac->reps : 0;
    out->lapses = ac->lapses > 0 ? ac->lapses : 0;

    if (ac->ivl > 0)       out->interval_sec = (double)ac->ivl * 86400.0;
    else if (ac->ivl < 0)  out->interval_sec = -(double)ac->ivl; /* learning secs */

    switch (ac->type) {
        case 1: out->state = CARD_LEARNING;   break;
        case 2: out->state = CARD_REVIEW;     break;
        case 3: out->state = CARD_RELEARNING; break;
        default: out->state = CARD_NEW;       break;
    }

    /* Due is reset to "now": Anki's due values use a different day-number
       scheme, so they can't be mapped reliably. Maturity is preserved via
       interval/ease so StudyQuest's scheduler continues correctly. */
    out->due = (double)time(NULL) - 1.0;
    out->last_review = (double)ac->mod;
}

/* ================================================================== */
/*  Duplicates & commit                                                */
/* ================================================================== */

Deck *import_find_existing_deck(DeckList *dl, const char *name) {
    if (!dl || !name) return NULL;
    for (int i = 0; i < dl->count; i++)
        if (strcmp(dl->decks[i].name, name) == 0) return &dl->decks[i];
    return NULL;
}

bool import_commit(DeckList *app, DeckList *scratch, ImportMode mode,
                   ImportWarnings *w) {
    if (!app || !scratch) return false;

    for (int si = 0; si < scratch->count; si++) {
        Deck *sd = &scratch->decks[si];
        Deck *existing =
            (mode == IMPORT_MODE_UPDATE) ? import_find_existing_deck(app, sd->name)
                                         : NULL;

        if (existing) {
            /* Merge into the existing deck, skipping duplicate Anki ids. */
            for (int ci = 0; ci < sd->card_count; ci++) {
                Card *sc = &sd->cards[ci];
                if (sc->anki_card_id != 0 && deck_has_anki_card(existing, sc->anki_card_id))
                    continue;   /* duplicate: freed with sd below */
                deck_add_card_move(existing, sc);
            }
            deck_free(sd);
        } else {
            char final_name[MAX_DECK_NAME];
            if (!import_unique_deck_name(app, sd->name, final_name, sizeof(final_name))) {
                import_warnings_addf(w, "Too many decks — '%s' skipped.", sd->name);
                deck_free(sd);
                continue;
            }
            snprintf(sd->name, sizeof(sd->name), "%s", final_name);
            if (!decklist_append_deck(app, sd)) {
                import_warnings_addf(w, "Deck list full — '%s' skipped.", sd->name);
                deck_free(sd);
                continue;
            }
        }
    }

    scratch->count = 0;
    return true;
}
