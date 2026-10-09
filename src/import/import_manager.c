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

/* Cut `s` to at most `max_bytes` bytes without splitting a UTF-8 sequence. */
static void utf8_clip(char *s, size_t max_bytes) {
    size_t n = strlen(s);
    if (n <= max_bytes) return;
    n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    s[n] = 0;
}

/* Fit a full "::" path into `cap` bytes. Japanese names use 3 bytes per
   character, so hierarchical names overflow MAX_DECK_NAME quickly. The leaf is
   the most informative part, so drop leading levels first ("...::Leaf") and
   only truncate the leaf itself as a last resort. Returns true if shortened. */
static bool fit_deck_name(const char *full, char *out, size_t cap) {
    if (strlen(full) < cap) { snprintf(out, cap, "%s", full); return false; }
    const char *p = full;
    while ((p = strstr(p, "::")) != NULL) {
        p += 2;
        size_t pl = strlen(p);
        if (pl + 5 < cap) { memcpy(out, "...::", 5); memcpy(out + 5, p, pl + 1); return true; }
    }
    size_t keep = cap - 4;                       /* room for "..." and NUL */
    size_t fl = strlen(full);
    if (keep > fl) keep = fl;
    memcpy(out, full, keep);
    out[keep] = 0;
    utf8_clip(out, keep);
    strcat(out, "...");
    return true;
}

static bool deck_name_from_anki(const AnkiCollection *col, int64_t did,
                                char *out, size_t cap) {
    for (int i = 0; i < col->deck_count; i++) {
        if (col->decks[i].id == did) return fit_deck_name(col->decks[i].name, out, cap);
    }
    /* Unknown deck id (missing from the deck list): stable fallback label. */
    snprintf(out, cap, "Deck %lld", (long long)did);
    return false;
}

typedef struct { int64_t did; int64_t nid; } DidNid;

static int didnid_cmp(const void *a, const void *b) {
    const DidNid *x = (const DidNid *)a, *y = (const DidNid *)b;
    if (x->did != y->did) return x->did < y->did ? -1 : 1;
    if (x->nid != y->nid) return x->nid < y->nid ? -1 : 1;
    return 0;
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
            s->name_shortened = deck_name_from_anki(col, did, s->name, sizeof(s->name));
        }
        s->card_count++;
    }

    /* Distinct notes per deck (a note can generate several cards in one
       deck; count it once). Sort (deck, note) pairs: O(n log n). */
    DidNid *pairs = col->card_count > 0
        ? (DidNid *)malloc(sizeof(DidNid) * (size_t)col->card_count) : NULL;
    if (!pairs) {
        for (int di = 0; di < out->deck_count; di++)
            out->decks[di].note_count = out->decks[di].card_count;   /* upper bound */
        return true;
    }
    for (int ci = 0; ci < col->card_count; ci++) {
        pairs[ci].did = col->cards[ci].did;
        pairs[ci].nid = col->cards[ci].nid;
    }
    qsort(pairs, (size_t)col->card_count, sizeof(DidNid), didnid_cmp);
    ImportDeckSummary *cur = NULL;
    for (int i = 0; i < col->card_count; i++) {
        if (i > 0 && pairs[i].did == pairs[i - 1].did && pairs[i].nid == pairs[i - 1].nid)
            continue;
        if (!cur || cur->anki_deck_id != pairs[i].did) cur = plan_find(out, pairs[i].did);
        if (cur) cur->note_count++;
    }
    free(pairs);
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
    snprintf(trimmed, sizeof(trimmed), "%s", base ? base : "");
    utf8_clip(trimmed, MAX_DECK_NAME - 8);      /* leave room for " (999)" */

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
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t used = strlen(w->message);
    size_t need = strlen(buf) + 1;                 /* text + '\n' */
    if (used + need + 1 > sizeof(w->message)) { w->dropped++; return; }
    snprintf(w->message + used, sizeof(w->message) - used, "%s\n", buf);
    w->count++;
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
    if (mr->too_large > 0) {
        import_warnings_addf(w, "%d media file(s) were skipped because they "
                                "are too large.", mr->too_large);
    }
    if (mr->corrupt > 0) {
        import_warnings_addf(w, "%d media file(s) could not be read (damaged "
                                "or undecodable).", mr->corrupt);
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

void import_job_init(ImportJob *j, const AnkiCollection *col,
                     const MediaImportResult *media, DeckList *target) {
    memset(j, 0, sizeof(*j));
    j->col = col;
    j->media = media;
    j->target = target;
    import_warnings_init(&j->warnings);
    /* Best effort: build the plan, ignoring failure (treated as no decks). */
    if (col) {
        import_build_deck_plan(col, &j->plan);
        if (col->note_count > 0)
            j->note_seen = (unsigned char *)calloc((size_t)col->note_count, 1);
    }
    /* Media problems found while extracting belong in the same list the
       result screen shows. */
    if (media) import_warn_media(&j->warnings, media);
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

/* Fold the per-category tallies into one warning each (once). */
static void job_summarize(ImportJob *j) {
    if (j->summarized) return;
    j->summarized = true;
    ImportWarnings *w = &j->warnings;
    if (j->skipped_unknown_deck > 0)
        import_warnings_addf(w, "%d card(s) belong to a deck that is not in the "
                                "package and were skipped.", j->skipped_unknown_deck);
    if (j->skipped_unconvertible > 0)
        import_warnings_addf(w, "%d card(s) could not be converted and were "
                                "skipped.", j->skipped_unconvertible);
    if (j->skipped_deck_full > 0)
        import_warnings_addf(w, "%d card(s) were skipped because a deck reached "
                                "its card limit.", j->skipped_deck_full);
    if (j->media && j->cards_with_missing_media > 0)
        import_warnings_addf(w, "%d card(s) refer to images or audio that are "
                                "not in the package.", j->cards_with_missing_media);
    if (j->suspended_cards > 0)
        import_warnings_addf(w, "%d suspended card(s) were imported as active "
                                "(StudyQuest has no suspend state).", j->suspended_cards);
    int shortened = 0;
    for (int i = 0; i < j->plan.deck_count; i++)
        if (j->plan.decks[i].name_shortened) shortened++;
    if (shortened > 0)
        import_warnings_addf(w, "%d deck name(s) were too long and were "
                                "abbreviated.", shortened);
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
            j->skipped_unknown_deck++;
            continue;
        }

        Deck *d = job_ensure_deck(j, gi);
        if (!d) continue;

        ConvertedCard cc;
        if (!anki_convert_card(j->col, ac, &cc)) {
            j->skipped_unconvertible++;
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
            j->skipped_deck_full++;
            anki_converted_card_free(&cc);
            continue;
        }

        /* Attach media refs, resolving Anki source names to the normalized
           filenames written by the media importer. */
        bool any_missing = false;
        for (int m = 0; m < cc.media_ref_count; m++) {
            char resolved[512];
            const char *ref = cc.media_refs[m];
            if (j->media) {
                if (media_import_resolve(j->media, cc.media_refs[m],
                                         resolved, sizeof(resolved))) ref = resolved;
                else any_missing = true;
            }
            card_add_media_ref(card, ref, cc.media_kinds[m],
                               cc.media_sides[m], cc.media_fields[m]);
        }

        if (any_missing) j->cards_with_missing_media++;

        j->cards_added++;
        if (note && j->note_seen) {
            long ni = (long)(note - j->col->notes);
            if (ni >= 0 && ni < j->col->note_count && !j->note_seen[ni]) {
                j->note_seen[ni] = 1;
                j->notes_added++;
            }
        }
        if (ac->queue == -1) j->suspended_cards++;

        /* Preserve the stable Anki identity and scheduling state. */
        card->anki_card_id = ac->id;
        import_apply_scheduling_ex(ac, j->col->crt, (double)time(NULL), card);

        anki_converted_card_free(&cc);
    }

    j->cards_done = j->next_card;
    if (j->next_card >= j->col->card_count) job_summarize(j);
    return attempted;
}

bool import_job_done(const ImportJob *j) {
    return !j || !j->col || j->next_card >= j->col->card_count;
}

void import_job_free(ImportJob *j) {
    if (!j) return;
    free(j->note_seen);
    j->note_seen = NULL;
}

/* ================================================================== */
/*  Scheduling                                                         */
/* ================================================================== */

/* Anki `due` -> epoch seconds. Epoch-second values (learning cards) are far
   larger than any day number, which lets us tell the encodings apart. */
static double due_to_epoch(const AnkiCard *ac, int64_t crt, double now) {
    double fallback = now - 1.0;                 /* studyable immediately */
    if (ac->type == 0) return fallback;          /* new: `due` is a position */
    double v = (double)ac->due;
    double t;
    if (v > 1.0e9)       t = v;                              /* epoch seconds */
    else if (crt > 0 && v >= 0) t = (double)crt + v * 86400.0; /* day number */
    else return fallback;
    if (t > now + 20.0 * 365.0 * 86400.0) return fallback;   /* nonsense */
    return t;
}

void import_apply_scheduling_ex(const AnkiCard *ac, int64_t crt, double now,
                                Card *out) {
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

    out->due = due_to_epoch(ac, crt, now);
    out->last_review = (double)ac->mod;
}

void import_apply_scheduling(const AnkiCard *ac, Card *out) {
    import_apply_scheduling_ex(ac, 0, (double)time(NULL), out);
}

/* ================================================================== */
/*  Duplicates & commit                                                */
/* ================================================================== */

Deck *import_find_existing_deck(const DeckList *dl, const char *name) {
    if (!dl || !name) return NULL;
    for (int i = 0; i < dl->count; i++)
        if (strcmp(dl->decks[i].name, name) == 0) return (Deck *)&dl->decks[i];
    return NULL;
}

static int i64_cmp(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static bool id_in_sorted(const int64_t *ids, int n, int64_t id) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) return true;
        if (ids[mid] < id) lo = mid + 1; else hi = mid - 1;
    }
    return false;
}

bool import_commit_ex(DeckList *app, DeckList *scratch, ImportMode mode,
                      ImportWarnings *w, ImportCommitResult *res) {
    ImportCommitResult local;
    if (!res) res = &local;
    memset(res, 0, sizeof(*res));
    if (!app || !scratch) return false;

    int n = scratch->count;
    if (n == 0) return true;

    Deck   **existing = (Deck **)calloc((size_t)n, sizeof(Deck *));
    int64_t **idsets  = (int64_t **)calloc((size_t)n, sizeof(int64_t *));
    int     *idn      = (int *)calloc((size_t)n, sizeof(int));
    int     *orig     = (int *)malloc(sizeof(int) * (size_t)n);
    bool ok = existing && idsets && idn && orig;
    if (!ok) import_warnings_addf(w, "Out of memory while preparing the import.");
    for (int i = 0; ok && i < n; i++) orig[i] = -1;

    /* ---- Phase 1: validate, touching nothing ---------------------- */
    int new_decks = 0;
    for (int si = 0; ok && si < n; si++) {
        Deck *sd = &scratch->decks[si];
        Deck *ex = (mode == IMPORT_MODE_UPDATE) ? import_find_existing_deck(app, sd->name) : NULL;
        existing[si] = ex;
        if (!ex) { new_decks++; continue; }

        /* Sorted Anki ids already in the target deck -> O(log n) dedupe. */
        int64_t *ids = ex->card_count > 0
            ? (int64_t *)malloc(sizeof(int64_t) * (size_t)ex->card_count) : NULL;
        int k = 0;
        if (ids) {
            for (int c = 0; c < ex->card_count; c++)
                if (ex->cards[c].anki_card_id != 0) ids[k++] = ex->cards[c].anki_card_id;
            qsort(ids, (size_t)k, sizeof(int64_t), i64_cmp);
        } else if (ex->card_count > 0) {
            import_warnings_addf(w, "Out of memory while preparing the import.");
            ok = false;
            break;
        }
        idsets[si] = ids;
        idn[si] = k;

        long incoming = 0;
        for (int c = 0; c < sd->card_count; c++) {
            int64_t id = sd->cards[c].anki_card_id;
            if (id == 0 || !id_in_sorted(ids, k, id)) incoming++;
        }
        if ((long)ex->card_count + incoming > MAX_CARDS) {
            import_warnings_addf(w, "Deck '%s' would exceed the limit of %d cards. "
                                    "Nothing was imported.", ex->name, MAX_CARDS);
            ok = false;
        }
    }
    if (ok && app->count + new_decks > MAX_DECKS) {
        import_warnings_addf(w, "Not enough room for %d new deck(s) (limit %d). "
                                "Nothing was imported.", new_decks, MAX_DECKS);
        ok = false;
    }
    if (!ok) goto cleanup;

    /* ---- Phase 2: apply, with an undo path ------------------------- */
    {
        int decks_before = app->count;
        int next_id_before = app->next_id;
        bool failed = false;
        bool have_first = false;

        for (int si = 0; si < n && !failed; si++) {
            Deck *sd = &scratch->decks[si];
            Deck *ex = existing[si];
            Deck *landed = NULL;

            if (ex) {
                orig[si] = ex->card_count;
                for (int c = 0; c < sd->card_count; c++) {
                    Card *sc = &sd->cards[c];
                    if (sc->anki_card_id != 0 &&
                        id_in_sorted(idsets[si], idn[si], sc->anki_card_id)) {
                        res->cards_skipped_duplicate++;
                        continue;
                    }
                    if (!deck_add_card_move(ex, sc)) { failed = true; break; }
                    res->cards_added++;
                }
                if (failed) break;
                res->decks_merged++;
                deck_free(sd);
                landed = ex;
            } else {
                char final_name[MAX_DECK_NAME];
                int added = sd->card_count;
                if (!import_unique_deck_name(app, sd->name, final_name, sizeof(final_name))) {
                    import_warnings_addf(w, "Could not find a free name for deck '%s'.", sd->name);
                    failed = true;
                    break;
                }
                snprintf(sd->name, sizeof(sd->name), "%s", final_name);
                landed = decklist_append_deck(app, sd);
                if (!landed) { failed = true; break; }
                res->decks_created++;
                res->cards_added += added;
            }
            if (!have_first && landed) {
                have_first = true;
                res->first_deck_id = landed->id;
                snprintf(res->first_deck_name, sizeof(res->first_deck_name), "%s", landed->name);
            }
        }

        if (failed) {
            /* Undo: truncate merged decks, drop appended ones. */
            for (int si = 0; si < n; si++) {
                Deck *ex = existing[si];
                if (!ex || orig[si] < 0) continue;
                while (ex->card_count > orig[si]) {
                    card_free(&ex->cards[ex->card_count - 1]);
                    ex->card_count--;
                }
            }
            while (app->count > decks_before) {
                deck_free(&app->decks[app->count - 1]);
                memset(&app->decks[app->count - 1], 0, sizeof(Deck));
                app->count--;
            }
            app->next_id = next_id_before;
            memset(res, 0, sizeof(*res));
            import_warnings_addf(w, "The import failed part-way (out of memory or a "
                                    "size limit) and was rolled back. Your decks are unchanged.");
            ok = false;
        } else {
            scratch->count = 0;
        }
    }

cleanup:
    if (idsets) for (int i = 0; i < n; i++) free(idsets[i]);
    free(idsets); free(idn); free(existing); free(orig);
    return ok;
}

bool import_commit(DeckList *app, DeckList *scratch, ImportMode mode,
                   ImportWarnings *w) {
    return import_commit_ex(app, scratch, mode, w, NULL);
}
