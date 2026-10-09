#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include <webp/decode.h>

#include "core/utf8.h"
#include "import/apkg_importer.h"
#include "import/anki_html.h"
#include "import/anki_parser.h"
#include "import/media_importer.h"
#include "import/anki_convert.h"
#include "import/import_manager.h"
#include "cards/cards.h"
#include "storage/save.h"
#include "player/player.h"

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        return 1; \
    } else { \
        printf("ok: %s\n", #cond); \
    } \
} while (0)

static int file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (int)st.st_size : -1;
}

/* Build the path to a sibling fixture next to the file in argv[1]. */
static void fixture_path(const char *argv1, const char *name,
                         char *out, size_t cap) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", argv1);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    else dir[0] = 0;
    snprintf(out, cap, "%s%s", dir, name);
}

static int test_package_variants(const char *argv1) {
    char path[1024];
    char tmp[] = "/tmp/sq-variant-XXXXXX";
    char *td = mkdtemp(tmp);
    CHECK(td != NULL);

    /* --- modern (zstd-compressed collection.anki21b) --------------- */
    fixture_path(argv1, "modern.apkg", path, sizeof(path));
    {
        ApkgHandle h;
        CHECK(apkg_open(path, td, &h));
        CHECK(h.variant == APKG_VARIANT_MODERN);
        CHECK(file_size(h.db_path) > 0);

        AnkiCollection col;
        CHECK(anki_parse(h.db_path, &col));
        CHECK(col.note_count == 3);
        CHECK(col.card_count == 3);
        CHECK(col.model_count == 1);

        bool found_konnichiwa = false, found_taberu = false, found_gakusei = false;
        for (int i = 0; i < col.note_count; i++) {
            if (col.notes[i].fields[0] &&
                strcmp(col.notes[i].fields[0], "こんにちは") == 0) found_konnichiwa = true;
            if (col.notes[i].fields[0] &&
                strcmp(col.notes[i].fields[0], "食べる") == 0) found_taberu = true;
            if (col.notes[i].fields[0] &&
                strcmp(col.notes[i].fields[0], "学生") == 0) found_gakusei = true;
        }
        CHECK(found_konnichiwa);
        CHECK(found_taberu);
        CHECK(found_gakusei);

        /* Deck ids must come from the JSON object keys, not the value
           nodes (regression: decks id was always 0). */
        bool deck1 = false, deck2 = false;
        for (int i = 0; i < col.deck_count; i++) {
            if (col.decks[i].id == 1 &&
                strcmp(col.decks[i].name, "Japanese::Vocabulary") == 0) deck1 = true;
            if (col.decks[i].id == 2 &&
                strcmp(col.decks[i].name, "Japanese::Kanji") == 0) deck2 = true;
        }
        CHECK(deck1);
        CHECK(deck2);
        CHECK(col.models[0].id == 1700000000000LL);

        anki_collection_free(&col);
        remove(h.db_path);
        remove(h.media_json_path);
    }

    /* --- legacy2 (collection.anki21 + dummy collection.anki2) ------ */
    fixture_path(argv1, "legacy2.apkg", path, sizeof(path));
    {
        ApkgHandle h;
        CHECK(apkg_open(path, td, &h));
        CHECK(h.variant == APKG_VARIANT_LEGACY2);

        AnkiCollection col;
        CHECK(anki_parse(h.db_path, &col));
        CHECK(col.note_count == 3);
        CHECK(col.card_count == 3);
        anki_collection_free(&col);
        remove(h.db_path);
        remove(h.media_json_path);
    }

    /* --- a ZIP that contains no collection file -------------------- */
    fixture_path(argv1, "invalid_no_collection.apkg", path, sizeof(path));
    {
        ApkgHandle h;
        CHECK(!apkg_open(path, td, &h));
    }

    /* --- a file that is not a ZIP at all --------------------------- */
    fixture_path(argv1, "invalid_corrupt.apkg", path, sizeof(path));
    {
        ApkgHandle h;
        CHECK(!apkg_open(path, td, &h));
    }

    rmdir(td);
    return 0;
}

static int test_scheduling(void) {
    AnkiCard ac = {0};
    ac.type = 2;   /* review */
    ac.ivl = 30;
    ac.factor = 2600;
    ac.reps = 7;
    ac.lapses = 2;
    ac.mod = 1700000000;

    Card c = {0};
    import_apply_scheduling(&ac, &c);
    CHECK(c.state == CARD_REVIEW);
    CHECK(c.interval_sec == 30.0 * 86400.0);
    CHECK(c.ease >= 2.599f && c.ease <= 2.601f);
    CHECK(c.reps == 7);
    CHECK(c.lapses == 2);
    CHECK(c.due <= (double)time(NULL));

    /* New card stays new. */
    AnkiCard nc = {0};
    nc.type = 0;
    nc.factor = 2500;
    Card ncard = {0};
    import_apply_scheduling(&nc, &ncard);
    CHECK(ncard.state == CARD_NEW);
    return 0;
}

static int test_duplicate_and_commit(const char *argv1) {
    char path[1024];
    char tmp[] = "/tmp/sq-commit-XXXXXX";
    char *td = mkdtemp(tmp);
    CHECK(td != NULL);

    fixture_path(argv1, "modern.apkg", path, sizeof(path));
    ApkgHandle h;
    CHECK(apkg_open(path, td, &h));
    CHECK(h.variant == APKG_VARIANT_MODERN);
    AnkiCollection col;
    CHECK(anki_parse(h.db_path, &col));

    DeckList app;
    decklist_init(&app);

    /* First import (NEW) into a scratch buffer, then commit. */
    {
        DeckList scratch;
        decklist_init(&scratch);
        ImportJob job;
        import_job_init(&job, &col, NULL, &scratch);
        while (!import_job_done(&job)) import_job_step(&job, 16);
        import_job_free(&job);

        ImportWarnings w;
        import_warnings_init(&w);
        CHECK(import_commit(&app, &scratch, IMPORT_MODE_NEW, &w));
        decklist_free(&scratch);
    }
    CHECK(app.count == 2);

    Deck *vocab = import_find_existing_deck(&app, "Japanese::Vocabulary");
    Deck *kanji = import_find_existing_deck(&app, "Japanese::Kanji");
    CHECK(vocab != NULL && kanji != NULL);
    CHECK(vocab->card_count == 2);
    CHECK(kanji->card_count == 1);
    CHECK(vocab->cards[0].anki_card_id != 0);   /* id preserved for dedup */

    /* Second import (UPDATE) must not duplicate any cards. */
    {
        DeckList scratch;
        decklist_init(&scratch);
        ImportJob job;
        import_job_init(&job, &col, NULL, &scratch);
        while (!import_job_done(&job)) import_job_step(&job, 16);
        import_job_free(&job);

        ImportWarnings w;
        import_warnings_init(&w);
        CHECK(import_commit(&app, &scratch, IMPORT_MODE_UPDATE, &w));
        decklist_free(&scratch);
    }
    CHECK(app.count == 2);
    CHECK(vocab->card_count == 2);   /* still 2, no duplicates */
    CHECK(kanji->card_count == 1);

    /* A failed conversion must leave the app untouched (atomicity): build a
       scratch, then simply discard it instead of committing. */
    {
        DeckList scratch;
        decklist_init(&scratch);
        ImportJob job;
        import_job_init(&job, &col, NULL, &scratch);
        while (!import_job_done(&job)) import_job_step(&job, 16);
        import_job_free(&job);
        decklist_free(&scratch);   /* discard — app must be unchanged */
    }
    CHECK(app.count == 2);
    CHECK(vocab->card_count == 2);

    decklist_free(&app);
    anki_collection_free(&col);
    remove(h.db_path);
    remove(h.media_json_path);
    rmdir(td);
    return 0;
}

static int test_media_edge_cases(const char *argv1) {
    char apkg[1024];
    fixture_path(argv1, "anki_media.apkg", apkg, sizeof(apkg));

    char tmp[] = "/tmp/sq-media-edge-XXXXXX";
    char *td = mkdtemp(tmp);
    CHECK(td != NULL);

    char media_json[1024], media_dir[1024];
    snprintf(media_json, sizeof(media_json), "%s/media", td);
    snprintf(media_dir,  sizeof(media_dir),  "%s/out", td);

    /* --- missing media entry: recoverable, not fatal --------------- */
    {
        FILE *f = fopen(media_json, "wb");
        CHECK(f != NULL);
        fputs("{\"0\": \"front.webp\", \"99\": \"missing.mp3\"}", f);
        fclose(f);

        MediaImportResult mr;
        CHECK(media_import_all(apkg, media_json, media_dir, NULL, NULL, &mr));
        CHECK(mr.copied == 1);
        CHECK(mr.missing == 1);
        CHECK(mr.skipped_unsafe == 0);
        media_import_free(&mr);
        remove(media_json);
    }

    /* --- path traversal: neutralized to basename, never escapes ----- */
    {
        FILE *f = fopen(media_json, "wb");
        CHECK(f != NULL);
        fputs("{\"0\": \"../../etc/passwd\"}", f);
        fclose(f);

        MediaImportResult mr;
        CHECK(media_import_all(apkg, media_json, media_dir, NULL, NULL, &mr));
        CHECK(mr.copied == 1);   /* sanitized to basename and imported */
        CHECK(mr.missing == 0);

        char outside[1024];
        snprintf(outside, sizeof(outside), "%s/etc/passwd", td);
        CHECK(file_size(outside) < 0);   /* must not have escaped */
        media_import_free(&mr);
        remove(media_json);
    }

    /* --- non-numeric archive entry key: skipped as unsafe ----------- */
    {
        FILE *f = fopen(media_json, "wb");
        CHECK(f != NULL);
        fputs("{\"evil/path\": \"cat.jpg\"}", f);
        fclose(f);

        MediaImportResult mr;
        CHECK(media_import_all(apkg, media_json, media_dir, NULL, NULL, &mr));
        CHECK(mr.skipped_unsafe == 1);
        CHECK(mr.copied == 0);
        media_import_free(&mr);
        remove(media_json);
    }

    /* --- malformed media map (not JSON): structural failure --------- */
    {
        FILE *f = fopen(media_json, "wb");
        CHECK(f != NULL);
        fputs("this is not json {", f);
        fclose(f);

        MediaImportResult mr;
        CHECK(!media_import_all(apkg, media_json, media_dir, NULL, NULL, &mr));
        CHECK(mr.error[0] != 0);
        media_import_free(&mr);
        remove(media_json);
    }

    /* --- import_warn_media folds counts into human warnings -------- */
    {
        ImportWarnings w;
        import_warnings_init(&w);
        MediaImportResult mr;
        memset(&mr, 0, sizeof(mr));
        mr.missing = 3;
        mr.skipped_unsafe = 2;
        import_warn_media(&w, &mr);
        CHECK(w.count == 2);
        CHECK(strstr(w.message, "missing") != NULL);
        CHECK(strstr(w.message, "not safe") != NULL);
    }

    rmdir(td);
    return 0;
}

static int test_import_manager(const char *argv1) {
    char path[1024];
    char tmp[] = "/tmp/sq-manager-XXXXXX";
    char *td = mkdtemp(tmp);
    CHECK(td != NULL);

    fixture_path(argv1, "modern.apkg", path, sizeof(path));
    ApkgHandle h;
    CHECK(apkg_open(path, td, &h));
    CHECK(h.variant == APKG_VARIANT_MODERN);

    AnkiCollection col;
    CHECK(anki_parse(h.db_path, &col));

    /* Deck plan: two distinct decks, full "::" names, correct counts. */
    ImportDeckPlan plan;
    CHECK(import_build_deck_plan(&col, &plan));
    CHECK(plan.deck_count == 2);

    const ImportDeckSummary *vocab = NULL, *kanji = NULL;
    for (int i = 0; i < plan.deck_count; i++) {
        if (strcmp(plan.decks[i].name, "Japanese::Vocabulary") == 0) vocab = &plan.decks[i];
        if (strcmp(plan.decks[i].name, "Japanese::Kanji") == 0) kanji = &plan.decks[i];
    }
    CHECK(vocab != NULL);
    CHECK(kanji != NULL);
    CHECK(vocab->card_count == 2);
    CHECK(vocab->note_count == 2);
    CHECK(kanji->card_count == 1);
    CHECK(kanji->note_count == 1);

    /* Run the job one card at a time into a fresh deck list. */
    DeckList dl;
    decklist_init(&dl);
    ImportJob job;
    import_job_init(&job, &col, NULL, &dl);
    while (!import_job_done(&job)) import_job_step(&job, 1);
    CHECK(job.cards_added == 3);
    CHECK(job.notes_added == 3);
    CHECK(dl.count == 2);

    Deck *d_vocab = NULL, *d_kanji = NULL;
    for (int i = 0; i < dl.count; i++) {
        if (strcmp(dl.decks[i].name, "Japanese::Vocabulary") == 0) d_vocab = &dl.decks[i];
        if (strcmp(dl.decks[i].name, "Japanese::Kanji") == 0) d_kanji = &dl.decks[i];
    }
    CHECK(d_vocab != NULL && d_kanji != NULL);
    CHECK(d_vocab->card_count == 2);
    CHECK(d_kanji->card_count == 1);

    /* Japanese text and tags survive into the StudyQuest cards. */
    bool konnichiwa = false, tags_ok = false;
    for (int i = 0; i < d_vocab->card_count; i++) {
        const char *v = card_field_value(&d_vocab->cards[i], 0);
        if (v && strcmp(v, "こんにちは") == 0) {
            konnichiwa = true;
            if (strcmp(d_vocab->cards[i].tags, "jlpt-n5,vocabulary") == 0) tags_ok = true;
        }
    }
    CHECK(konnichiwa);
    CHECK(tags_ok);
    CHECK(card_field_value(&d_vocab->cards[0], 1) != NULL); /* second field preserved */

    import_job_free(&job);
    decklist_free(&dl);
    anki_collection_free(&col);
    remove(h.db_path);
    remove(h.media_json_path);
    rmdir(td);
    return 0;
}

static int test_utf8(void) {
    const char *s = "食べる 髙橋 﨑 𠮷";
    size_t bad = 0;
    CHECK(sq_utf8_validate(s, &bad));

    uint32_t expected[] = { 0x98DF, 0x3079, 0x308B, 0x20, 0x9AD9,
                            0x6A4B, 0x20, 0xFA11, 0x20, 0x20BB7 };
    const char *p = s;
    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
        uint32_t cp = 0; size_t n = 0;
        CHECK(sq_utf8_decode(p, &cp, &n));
        CHECK(cp == expected[i]);
        p += n;
    }
    CHECK(*p == 0);
    CHECK(card_has_cjk("𠮷"));
    CHECK(sq_utf8_validate("食べる 髙橋 﨑 𠮷", &bad));
    CHECK(!sq_utf8_validate("\xE3\x81", &bad));
    CHECK(!sq_utf8_validate("\xC0\xAF", &bad));
    return 0;
}

static int test_html(void) {
    const char *html =
        "<p>食べる &#x9AD9; &#xFA11; &#x20BB7;</p>"
        "<img src='front.webp'>"
        " [sound:front.wav]"
        "<div>back</div>"
        "<img alt=back src=back.webp>"
        "[sound:back.wav] [sound:back2.wav]";
    char text[2048];
    char refs[16][512] = {{0}};
    int count = 0;

    anki_html_process(html, text, sizeof(text), refs, &count, 16);
    CHECK(strstr(text, "食べる") != NULL);
    CHECK(strstr(text, "髙") != NULL);
    CHECK(strstr(text, "﨑") != NULL);
    CHECK(strstr(text, "𠮷") != NULL);
    CHECK(strstr(text, "<img") == NULL);
    CHECK(strstr(text, "[sound:") == NULL);
    CHECK(count == 5);
    CHECK(strcmp(refs[0], "front.webp") == 0);
    CHECK(strcmp(refs[1], "front.wav") == 0);
    CHECK(strcmp(refs[2], "back.webp") == 0);
    CHECK(strcmp(refs[3], "back.wav") == 0);
    CHECK(strcmp(refs[4], "back2.wav") == 0);
    return 0;
}

static int test_html_formatting(void) {
    char text[1024];

    /* Inline formatting tags are stripped, text preserved. */
    anki_html_to_text("<b>bold</b> <i>italic</i> <em>em</em> <u>under</u> <strong>strong</strong>",
                      text, sizeof(text));
    CHECK(strcmp(text, "bold italic em under strong") == 0);

    /* Block-level tags become line breaks. */
    anki_html_to_text("<p>para one</p><p>para two</p>", text, sizeof(text));
    CHECK(strcmp(text, "para one\npara two") == 0);

    anki_html_to_text("<div>a</div><div>b</div>", text, sizeof(text));
    CHECK(strcmp(text, "a\nb") == 0);

    anki_html_to_text("line1<br>line2", text, sizeof(text));
    CHECK(strcmp(text, "line1\nline2") == 0);

    anki_html_to_text("<ul><li>one</li><li>two</li></ul>", text, sizeof(text));
    CHECK(strcmp(text, "one\ntwo") == 0);

    /* No raw tags must leak. */
    anki_html_to_text("<b><i><u>formatted</u></i></b>", text, sizeof(text));
    CHECK(strstr(text, "<") == NULL);

    /* Japanese HTML: content and entities preserved, tags stripped. */
    anki_html_to_text("<p>日本語を<b>勉強</b>しています。&#x3002;</p>",
                      text, sizeof(text));
    CHECK(strstr(text, "日本語を勉強しています。") != NULL);
    CHECK(strstr(text, "<") == NULL);

    /* Trailing whitespace is trimmed. */
    anki_html_to_text("spaced   text  ", text, sizeof(text));
    CHECK(strcmp(text, "spaced   text") == 0);
    return 0;
}


static int test_conversion_and_persistence(const AnkiCollection *col,
                                           const AnkiCard *anki_card,
                                           const char *media_root,
                                           const MediaImportResult *mr) {
    ConvertedCard cc = {0};
    CHECK(anki_convert_card(col, anki_card, &cc));
    CHECK(cc.field_count == 4);
    CHECK(strncmp(cc.field_values[0], "食べる 髙橋 﨑 𠮷", strlen("食べる 髙橋 﨑 𠮷")) == 0);
    CHECK(strstr(cc.field_values[0], "<img") == NULL);
    CHECK(strstr(cc.field_values[0], "[sound:") == NULL);
    CHECK(cc.media_ref_count == 5);

    bool back_image = false, back_audio1 = false, back_audio2 = false;
    int back_image_field = -1, back_audio1_field = -1, back_audio2_field = -1;
    for (int i = 0; i < cc.media_ref_count; i++) {
        if (cc.media_sides[i] != 1) continue;
        if (cc.media_kinds[i] == 0 && strcmp(cc.media_refs[i], "back.webp") == 0) {
            back_image = true;
            back_image_field = cc.media_fields[i];
        }
        if (cc.media_kinds[i] == 1 && strcmp(cc.media_refs[i], "back.wav") == 0) {
            back_audio1 = true;
            back_audio1_field = cc.media_fields[i];
        }
        if (cc.media_kinds[i] == 1 && strcmp(cc.media_refs[i], "back2.wav") == 0) {
            back_audio2 = true;
            back_audio2_field = cc.media_fields[i];
        }
    }
    CHECK(back_image && back_audio1 && back_audio2);
    CHECK(back_image_field == 2);
    CHECK(back_audio1_field == 2 && back_audio2_field == 2);

    static DeckList decks;
    decklist_init(&decks);
    Deck *d = decklist_add(&decks, "Imported Japanese", (Color){1, 2, 3, 255});
    CHECK(d != NULL);
    Card *c = deck_add_card_fields(d, cc.field_names, (const char *const *)cc.field_values,
                                   cc.field_count, "test-tag");
    CHECK(c != NULL);
    for (int i = 0; i < cc.media_ref_count; i++) {
        char resolved[512];
        CHECK(media_import_resolve(mr, cc.media_refs[i], resolved, sizeof(resolved)));
        card_add_media_ref(c, resolved, cc.media_kinds[i], cc.media_sides[i], cc.media_fields[i]);
    }
    CHECK(c->media_ref_count == 5);
    CHECK(c->image_ref[0] != 0);
    CHECK(c->audio_ref[0] != 0);
    CHECK(card_field_value(c, 0) != NULL);
    CHECK(strncmp(card_field_value(c, 0), "食べる 髙橋 﨑 𠮷", strlen("食べる 髙橋 﨑 𠮷")) == 0);

    static char very_long[MAX_FIELD_VALUE * 8];
    const char *kanji = "難しい漢字テスト";
    size_t klen = strlen(kanji);
    size_t n = 0;
    while (n + klen < sizeof(very_long) - 1) {
        memcpy(very_long + n, kanji, klen);
        n += klen;
    }
    very_long[n] = 0;
    char names[1][MAX_FIELD_NAME] = {{0}};
    snprintf(names[0], sizeof(names[0]), "Long");
    const char *vals[1] = { very_long };
    Card *long_card = deck_add_card_fields(d, names, vals, 1, "");
    CHECK(long_card != NULL);
    CHECK(strlen(long_card->field_values[0]) == strlen(very_long));

    SaveData *data = calloc(1, sizeof(*data));
    CHECK(data != NULL);
    data->decks = decks;
    player_init(&data->player);
    char save_path[1024];
    snprintf(save_path, sizeof(save_path), "%s/studyquest-roundtrip.sav", media_root);
    CHECK(save_write(data, save_path));

    SaveData *loaded = calloc(1, sizeof(*loaded));
    CHECK(loaded != NULL);
    CHECK(save_load(loaded, save_path));
    CHECK(loaded->decks.count == 1);
    CHECK(loaded->decks.decks[0].card_count == 2);
    Card *loaded_c = &loaded->decks.decks[0].cards[0];
    CHECK(card_field_value(loaded_c, 0) != NULL);
    CHECK(strncmp(card_field_value(loaded_c, 0), "食べる 髙橋 﨑 𠮷", strlen("食べる 髙橋 﨑 𠮷")) == 0);
    CHECK(loaded_c->media_ref_count == 5);
    CHECK(strcmp(loaded_c->media_refs[2].ref, "back.webp") == 0);
    CHECK(loaded_c->media_refs[2].side == 1);
    CHECK(loaded_c->media_refs[2].field_index == 2);
    CHECK(strcmp(loaded_c->media_refs[3].ref, "back.wav") == 0);
    CHECK(loaded_c->media_refs[3].kind == 1 && loaded_c->media_refs[3].side == 1);
    CHECK(loaded_c->media_refs[3].field_index == 2);
    CHECK(strlen(loaded->decks.decks[0].cards[1].field_values[0]) == strlen(very_long));
    CHECK(strstr(loaded->decks.decks[0].cards[1].field_values[0], kanji) != NULL);

    remove(save_path);
    decklist_free(&loaded->decks);
    decklist_free(&data->decks);
    free(loaded);
    free(data);
    anki_converted_card_free(&cc);
    return 0;
}

/* ====================================================================== */
/*  Real Anki packages (generated by the official anki library)           */
/* ====================================================================== */

#include <math.h>
#include <zstd.h>
#include "miniz.h"
#include "import/zip_stream.h"

static double now_secs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static bool read_file(const char *path, unsigned char **buf, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    *buf = (unsigned char *)malloc((size_t)*len + 1);
    bool ok = *buf && fread(*buf, 1, (size_t)*len, f) == (size_t)*len;
    fclose(f);
    return ok;
}

static bool file_starts_with(const char *path, const void *magic, size_t n) {
    unsigned char b[16];
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t got = fread(b, 1, n, f);
    fclose(f);
    return got == n && memcmp(b, magic, n) == 0;
}

static const AnkiNote *note_with_field0(const AnkiCollection *c, const char *needle) {
    for (int i = 0; i < c->note_count; i++)
        if (c->notes[i].field_count > 0 && strstr(c->notes[i].fields[0], needle))
            return &c->notes[i];
    return NULL;
}

static int deck_index(const AnkiCollection *c, const char *name) {
    for (int i = 0; i < c->deck_count; i++)
        if (strcmp(c->decks[i].name, name) == 0) return i;
    return -1;
}

static const Card *find_card(const Deck *d, const char *front) {
    for (int i = 0; i < d->card_count; i++) {
        const char *v = card_field_value(&d->cards[i], 0);
        if (v && strstr(v, front)) return &d->cards[i];
    }
    return NULL;
}

static int test_real_package_layouts(const char *argv1) {
    char path[1024], tmp[] = "/tmp/sq-real-XXXXXX";
    char *td = mkdtemp(tmp);
    CHECK(td != NULL);

    /* ---- modern: schema 18, zstd collection without a content size ---- */
    fixture_path(argv1, "real_modern.apkg", path, sizeof(path));
    ApkgHandle h;
    CHECK(apkg_open(path, td, &h));
    CHECK(h.variant == APKG_VARIANT_MODERN);
    CHECK(h.meta_version == 3);
    CHECK(h.media_zstd);
    CHECK(h.media_entry_count == 3);

    AnkiCollection col;
    CHECK(anki_parse(h.db_path, &col));
    CHECK(col.modern_schema);
    CHECK(col.schema_version == 18);
    CHECK(col.crt > 1000000000);
    CHECK(col.model_count == 3);
    CHECK(col.note_count == 7);
    CHECK(col.card_count == 8);

    bool saw_cloze = false, saw_two_templates = false;
    for (int i = 0; i < col.model_count; i++) {
        if (col.models[i].is_cloze) {
            saw_cloze = true;
            CHECK(strstr(col.models[i].templates[0].qfmt, "{{cloze:Text}}") != NULL);
        }
        if (col.models[i].template_count == 2) saw_two_templates = true;
        CHECK(col.models[i].field_count == 2);
        CHECK(col.models[i].templates[0].qfmt[0] != 0);
        CHECK(col.models[i].templates[0].afmt[0] != 0);
    }
    CHECK(saw_cloze && saw_two_templates);

    /* Hierarchy separators (0x1f in the DB) become "::". */
    CHECK(deck_index(&col, "Japanese::Vocabulary") >= 0);
    CHECK(deck_index(&col, "Japanese::Kanji") >= 0);
    CHECK(deck_index(&col, "Japanese::Grammar::N5") >= 0);

    const AnkiNote *n = note_with_field0(&col, "こんにちは");
    CHECK(n != NULL);
    CHECK(strstr(n->tags, "jlpt-n5") != NULL);
    CHECK(anki_find_note(&col, n->id) == n);        /* indexed lookup */
    CHECK(anki_find_note(&col, 42) == NULL);

    /* ---- legacy-compatibility export of the same collection ----------- */
    char td2_t[] = "/tmp/sq-real2-XXXXXX";
    char *td2 = mkdtemp(td2_t);
    fixture_path(argv1, "real_legacy.apkg", path, sizeof(path));
    ApkgHandle hl;
    CHECK(apkg_open(path, td2, &hl));
    CHECK(hl.variant == APKG_VARIANT_LEGACY2);
    CHECK(!hl.media_zstd);
    AnkiCollection cl;
    CHECK(anki_parse(hl.db_path, &cl));
    CHECK(!cl.modern_schema);
    CHECK(cl.note_count == col.note_count && cl.card_count == col.card_count);
    CHECK(deck_index(&cl, "Japanese::Grammar::N5") >= 0);
    CHECK(cl.crt == col.crt);

    anki_collection_free(&cl);
    anki_collection_free(&col);
    remove(h.db_path); remove(h.media_json_path); rmdir(td);
    remove(hl.db_path); remove(hl.media_json_path); rmdir(td2);
    return 0;
}

typedef struct { int calls; int last_done; int last_total; } ProgressLog;
static void progress_cb(int done, int total, void *u) {
    ProgressLog *p = (ProgressLog *)u;
    p->calls++; p->last_done = done; p->last_total = total;
}

static int test_real_media(const char *argv1) {
    char path[1024], tmp[] = "/tmp/sq-rmedia-XXXXXX";
    char *td = mkdtemp(tmp);
    fixture_path(argv1, "real_modern.apkg", path, sizeof(path));
    ApkgHandle h;
    CHECK(apkg_open(path, td, &h));

    char root[600];
    snprintf(root, sizeof(root), "%s/sq-media", td);
    MediaImportOptions opt = { .zstd_entries = h.media_zstd };

    /* Incremental import with progress, as the UI drives it. */
    MediaImportResult mr;
    MediaImportJob job;
    CHECK(media_import_begin(&job, path, h.media_json_path, root, &opt, &mr));
    CHECK(mr.count == 3);
    CHECK(!media_import_done(&job));
    int steps = 0;
    while (!media_import_done(&job)) { CHECK(media_import_step(&job, 1) == 1); steps++; }
    media_import_end(&job);
    CHECK(steps == 3);
    CHECK(mr.copied == 3 && mr.missing == 0 && mr.corrupt == 0 && mr.skipped_unsafe == 0);

    char img[800], snd[800], jp[800];
    snprintf(img, sizeof(img), "%s/images/cat.png", root);
    snprintf(jp,  sizeof(jp),  "%s/images/日本語.png", root);
    snprintf(snd, sizeof(snd), "%s/audio/hello.wav", root);
    /* The zip entries are zstd frames; the files on disk must be the decoded bytes. */
    CHECK(file_starts_with(img, "\x89PNG\r\n\x1a\n", 8));
    CHECK(file_starts_with(jp,  "\x89PNG\r\n\x1a\n", 8));
    CHECK(file_starts_with(snd, "RIFF", 4));
    char resolved[512];
    CHECK(media_import_resolve(&mr, "日本語.png", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "日本語.png") == 0);
    CHECK(!media_import_resolve(&mr, "evil name?.wav", resolved, sizeof(resolved)));

    /* Importing the same package again reuses identical files. */
    MediaImportResult mr2;
    CHECK(media_import_all_ex(path, h.media_json_path, root, &opt, NULL, NULL, &mr2));
    CHECK(mr2.copied == 3 && mr2.reused == 3);
    media_import_rollback(&mr2);                      /* nothing new: removes nothing */
    CHECK(file_size(img) > 0);
    media_import_free(&mr2);

    /* Rolling back the first import removes exactly the files it created. */
    media_import_rollback(&mr);
    CHECK(file_size(img) < 0 && file_size(snd) < 0 && file_size(jp) < 0);
    media_import_free(&mr);

    /* A different file with the same name is never overwritten. */
    char dir[800];
    snprintf(dir, sizeof(dir), "%s/images", root);
    mkdir(dir, 0755);
    FILE *f = fopen(img, "wb");
    CHECK(f != NULL);
    fputs("MY OWN CAT", f);
    fclose(f);
    MediaImportResult mr3;
    CHECK(media_import_all_ex(path, h.media_json_path, root, &opt, progress_cb,
                              &(ProgressLog){0}, &mr3));
    CHECK(mr3.copied == 3);
    CHECK(media_import_resolve(&mr3, "cat.png", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "cat.png") != 0);
    CHECK(strncmp(resolved, "cat__", 5) == 0);
    CHECK(strstr(resolved, ".png") != NULL);
    unsigned char *kept = NULL; long klen = 0;
    CHECK(read_file(img, &kept, &klen));
    CHECK(klen == 10 && memcmp(kept, "MY OWN CAT", 10) == 0);
    free(kept);
    media_import_rollback(&mr3);
    CHECK(file_size(img) == 10);                       /* user's file survives */
    media_import_free(&mr3);

    /* One-shot API reports progress for every entry. */
    ProgressLog pl = {0};
    MediaImportResult mr4;
    char root2[600];
    snprintf(root2, sizeof(root2), "%s/sq-media2", td);
    CHECK(media_import_all_ex(path, h.media_json_path, root2, &opt, progress_cb, &pl, &mr4));
    CHECK(pl.calls == 3 && pl.last_done == 3 && pl.last_total == 3);
    media_import_free(&mr4);
    return 0;
}

static int test_real_pipeline(const char *argv1) {
    char path[1024], tmp[] = "/tmp/sq-rpipe-XXXXXX";
    char *td = mkdtemp(tmp);
    fixture_path(argv1, "real_modern.apkg", path, sizeof(path));
    ApkgHandle h;
    CHECK(apkg_open(path, td, &h));
    AnkiCollection col;
    CHECK(anki_parse(h.db_path, &col));

    char root[600];
    snprintf(root, sizeof(root), "%s/sq-media", td);
    MediaImportOptions opt = { .zstd_entries = h.media_zstd };
    MediaImportResult mr;
    CHECK(media_import_all_ex(path, h.media_json_path, root, &opt, NULL, NULL, &mr));

    static DeckList scratch, app;
    decklist_init(&scratch);
    decklist_init(&app);
    ImportJob job;
    import_job_init(&job, &col, &mr, &scratch);
    CHECK(job.plan.deck_count == 3);
    while (!import_job_done(&job)) import_job_step(&job, 3);
    CHECK(job.cards_added == 8);
    CHECK(job.notes_added == 7);
    CHECK(job.cards_with_missing_media == 1);          /* [sound:evil name?.wav] */
    CHECK(strstr(job.warnings.message, "not in the package") != NULL);
    ImportWarnings warnings = job.warnings;
    import_job_free(&job);

    ImportCommitResult cr;
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_NEW, &warnings, &cr));
    CHECK(cr.decks_created == 3 && cr.decks_merged == 0 && cr.cards_added == 8);
    CHECK(app.count == 3 && scratch.count == 0);
    CHECK(decklist_find(&app, cr.first_deck_id) != NULL);
    CHECK(strcmp(decklist_find(&app, cr.first_deck_id)->name, cr.first_deck_name) == 0);

    Deck *vocab = import_find_existing_deck(&app, "Japanese::Vocabulary");
    Deck *kanji = import_find_existing_deck(&app, "Japanese::Kanji");
    Deck *n5    = import_find_existing_deck(&app, "Japanese::Grammar::N5");
    CHECK(vocab && kanji && n5);
    CHECK(vocab->card_count == 4 && kanji->card_count == 2 && n5->card_count == 2);

    /* Scheduling: review cards keep ease/interval/reps/lapses and their due
       date, counted in days from the collection's creation time. */
    const Card *rv = find_card(vocab, "こんにちは");
    CHECK(rv != NULL);
    CHECK(rv->state == CARD_REVIEW);
    CHECK(fabs(rv->interval_sec - 21 * 86400.0) < 1.0);
    CHECK(fabs(rv->ease - 2.35f) < 0.001f);
    CHECK(rv->reps == 7 && rv->lapses == 1);
    CHECK(fabs(rv->due - ((double)col.crt + 5 * 86400.0)) < 1.0);
    const Card *nw = find_card(vocab, "おはよう");
    CHECK(nw == NULL || nw->state == CARD_NEW);        /* may not be exported */
    const Card *lc = find_card(n5, "ありがとう");
    CHECK(lc != NULL && lc->state == CARD_LEARNING);
    CHECK(fabs(lc->due - 1791446320.0) < 1.0);         /* learning: epoch seconds */

    /* Media references point at files that exist on disk. */
    const Card *kc = find_card(kanji, "日本語");
    CHECK(kc != NULL && kc->media_ref_count >= 1);
    CHECK(file_size("/dev/null") == 0);                /* sanity of helper */
    char ip[800];
    snprintf(ip, sizeof(ip), "%s/images/日本語.png", root);
    CHECK(file_size(ip) > 0);

    /* Persistence across a save/load cycle. */
    static SaveData sd, loaded;
    memset(&sd, 0, sizeof(sd));
    save_defaults(&sd);
    decklist_free(&sd.decks);
    sd.decks = app;                                    /* shallow: same heap cards */
    char sp[600];
    snprintf(sp, sizeof(sp), "%s/test.sav", td);
    CHECK(save_write(&sd, sp));
    memset(&loaded, 0, sizeof(loaded));
    CHECK(save_load(&loaded, sp));
    CHECK(loaded.decks.count == 3);
    Deck *lv = import_find_existing_deck(&loaded.decks, "Japanese::Vocabulary");
    CHECK(lv && lv->card_count == 4);
    const Card *lrv = find_card(lv, "こんにちは");
    CHECK(lrv && lrv->state == CARD_REVIEW && fabs(lrv->due - rv->due) < 1.0);
    CHECK(lrv->reps == 7 && lrv->anki_card_id == rv->anki_card_id);
    Deck *lk = import_find_existing_deck(&loaded.decks, "Japanese::Kanji");
    const Card *lkc = find_card(lk, "日本語");
    CHECK(lkc && lkc->media_ref_count == kc->media_ref_count);
    decklist_free(&loaded.decks);

    /* Re-importing in UPDATE mode adds nothing; in NEW mode makes copies. */
    decklist_init(&scratch);
    import_job_init(&job, &col, &mr, &scratch);
    while (!import_job_done(&job)) import_job_step(&job, 50);
    import_job_free(&job);
    ImportWarnings w2; import_warnings_init(&w2);
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_UPDATE, &w2, &cr));
    CHECK(cr.cards_added == 0 && cr.cards_skipped_duplicate == 8 && cr.decks_merged == 3);
    CHECK(app.count == 3 && vocab->card_count == 4);

    decklist_init(&scratch);
    import_job_init(&job, &col, &mr, &scratch);
    while (!import_job_done(&job)) import_job_step(&job, 50);
    import_job_free(&job);
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_NEW, &w2, &cr));
    CHECK(cr.decks_created == 3 && app.count == 6);
    CHECK(import_find_existing_deck(&app, "Japanese::Vocabulary (2)") != NULL);

    decklist_free(&app);
    anki_collection_free(&col);
    media_import_free(&mr);
    return 0;
}

static int test_real_variants(const char *argv1) {
    char path[1024];
    static const char *names[] = { "real_modern_nosched.apkg", "real_modern_nomedia.apkg",
                                   "real_legacy.apkg" };
    for (int v = 0; v < 3; v++) {
        char tmp[] = "/tmp/sq-rvar-XXXXXX";
        char *td = mkdtemp(tmp);
        fixture_path(argv1, names[v], path, sizeof(path));
        ApkgHandle h;
        CHECK(apkg_open(path, td, &h));
        AnkiCollection col;
        CHECK(anki_parse(h.db_path, &col));
        CHECK(col.card_count == 8 && col.note_count == 7);

        char root[600];
        snprintf(root, sizeof(root), "%s/sq-m", td);
        MediaImportOptions opt = { .zstd_entries = h.media_zstd };
        MediaImportResult mr;
        CHECK(media_import_all_ex(path, h.media_json_path, root, &opt, NULL, NULL, &mr));
        if (v == 1) CHECK(mr.count == 0);              /* exported without media */
        else        CHECK(mr.count == 3 && mr.copied == 3);

        static DeckList scratch;
        decklist_init(&scratch);
        ImportJob job;
        import_job_init(&job, &col, &mr, &scratch);
        while (!import_job_done(&job)) import_job_step(&job, 100);
        CHECK(job.cards_added == 8);
        if (v == 0) {                                  /* no scheduling exported */
            for (int d = 0; d < scratch.count; d++)
                for (int c = 0; c < scratch.decks[d].card_count; c++)
                    CHECK(scratch.decks[d].cards[c].state == CARD_NEW);
        }
        if (v == 1) {                                  /* images/audio missing */
            CHECK(job.cards_with_missing_media >= 3);
            CHECK(strstr(job.warnings.message, "not in the package") != NULL);
        }
        import_job_free(&job);
        decklist_free(&scratch);
        anki_collection_free(&col);
        media_import_free(&mr);
        remove(h.db_path); remove(h.media_json_path);
    }
    return 0;
}

/* ====================================================================== */
/*  Hostile / corrupt input                                               */
/* ====================================================================== */

static int test_zip_stream_limits(void) {
    char tmp[] = "/tmp/sq-zs-XXXXXX";
    char *td = mkdtemp(tmp);
    char zp[600];
    snprintf(zp, sizeof(zp), "%s/t.zip", td);

    size_t big = 1 << 20;
    unsigned char *zeros = (unsigned char *)calloc(1, big);
    size_t cap = ZSTD_compressBound(big);
    unsigned char *comp = (unsigned char *)malloc(cap);
    size_t clen = ZSTD_compress(comp, cap, zeros, big, 3);
    CHECK(!ZSTD_isError(clen));
    CHECK(clen < 200);                                 /* a real "bomb": 1 MiB in <200 bytes */

    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "z", comp, clen, NULL, 0, MZ_NO_COMPRESSION));
    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "trunc", comp, clen / 2, NULL, 0, MZ_NO_COMPRESSION));
    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "raw", "hello world", 11, NULL, 0, MZ_NO_COMPRESSION));
    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "empty", "", 0, NULL, 0, MZ_NO_COMPRESSION));

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_reader_init_file(&zip, zp, 0));
    struct mz_zip_archive_tag *z = (struct mz_zip_archive_tag *)&zip;
    uint8_t *out = NULL; size_t len = 0;

    CHECK(zs_extract_to_mem(z, "z", true, 2u << 20, &out, &len) == ZS_OK);
    CHECK(len == big && out[0] == 0 && out[len - 1] == 0 && out[len] == 0);
    free(out);
    CHECK(zs_extract_to_mem(z, "z", true, 1000, &out, &len) == ZS_TOO_BIG);   /* bomb stopped at the cap */
    CHECK(out == NULL);
    CHECK(zs_extract_to_mem(z, "trunc", true, 2u << 20, &out, &len) == ZS_CORRUPT);
    CHECK(zs_extract_to_mem(z, "raw", true, 1000, &out, &len) == ZS_CORRUPT);
    CHECK(zs_extract_to_mem(z, "raw", false, 1000, &out, &len) == ZS_OK);
    CHECK(len == 11 && memcmp(out, "hello world", 11) == 0);
    free(out);
    CHECK(zs_extract_to_mem(z, "raw", false, 5, &out, &len) == ZS_TOO_BIG);
    CHECK(zs_extract_to_mem(z, "empty", false, 5, &out, &len) == ZS_OK && len == 0);
    free(out);
    CHECK(zs_extract_to_mem(z, "empty", true, 5, &out, &len) == ZS_CORRUPT);
    CHECK(zs_extract_to_mem(z, "nope", false, 5, &out, &len) == ZS_NOT_FOUND);
    mz_zip_reader_end(&zip);

    CHECK(zs_zstd_decode_mem(comp, clen, 2u << 20, &out, &len) == ZS_OK && len == big);
    free(out);
    CHECK(zs_zstd_decode_mem(comp, clen / 2, 2u << 20, &out, &len) == ZS_CORRUPT);
    free(zeros); free(comp);
    return 0;
}

static void write_bytes(const char *path, const void *d, size_t n) {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(d, 1, n, f); fclose(f); }
}

static int test_media_hostile_inputs(void) {
    char tmp[] = "/tmp/sq-host-XXXXXX";
    char *td = mkdtemp(tmp);
    char zp[600], mp[600], root[600];
    snprintf(zp, sizeof(zp), "%s/p.apkg", td);
    snprintf(mp, sizeof(mp), "%s/media", td);
    snprintf(root, sizeof(root), "%s/out/media", td);

    unsigned char big[5000];
    memset(big, 'A', sizeof(big));
    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "0", big, sizeof(big), NULL, 0, MZ_NO_COMPRESSION));
    CHECK(mz_zip_add_mem_to_archive_file_in_place(zp, "1", "tiny", 4, NULL, 0, MZ_NO_COMPRESSION));

    /* Per-file size cap. */
    const char *json = "{\"0\":\"big.png\",\"1\":\"tiny.png\"}";
    write_bytes(mp, json, strlen(json));
    MediaImportOptions opt = { .max_file_bytes = 100 };
    MediaImportResult mr;
    CHECK(media_import_all_ex(zp, mp, root, &opt, NULL, NULL, &mr));
    CHECK(mr.too_large == 1 && mr.copied == 1);
    char resolved[512];
    CHECK(!media_import_resolve(&mr, "big.png", resolved, sizeof(resolved)));
    CHECK(media_import_resolve(&mr, "tiny.png", resolved, sizeof(resolved)));
    media_import_free(&mr);
    {
        ImportWarnings w; import_warnings_init(&w);
        MediaImportResult x; memset(&x, 0, sizeof(x));
        x.too_large = 2; x.corrupt = 1;
        import_warn_media(&w, &x);
        CHECK(w.count == 2 && strstr(w.message, "too large") && strstr(w.message, "damaged"));
    }

    /* Total size cap. */
    opt = (MediaImportOptions){ .max_total_bytes = 6000, .max_file_bytes = 100000 };
    const char *json2 = "{\"0\":\"a.png\",\"1\":\"b.png\"}";
    write_bytes(mp, json2, strlen(json2));
    char root_b[600];
    snprintf(root_b, sizeof(root_b), "%s/out/media-b", td);
    CHECK(media_import_all_ex(zp, mp, root_b, &opt, NULL, NULL, &mr));
    CHECK(mr.copied == 2);                             /* 5000 + 4 < 6000 */
    media_import_free(&mr);
    opt.max_total_bytes = 4000;
    char root_c[600];
    snprintf(root_c, sizeof(root_c), "%s/out/media-c", td);
    CHECK(media_import_all_ex(zp, mp, root_c, &opt, NULL, NULL, &mr));
    CHECK(mr.too_large == 1 && mr.copied == 1);
    media_import_free(&mr);

    /* Truncated / garbage protobuf media list: clean failure, no crash. */
    static const unsigned char bad1[] = { 0x0A, 0x21, 0x0A, 0x07, 0x63 };
    write_bytes(mp, bad1, sizeof(bad1));
    CHECK(!media_import_all_ex(zp, mp, root, NULL, NULL, NULL, &mr));
    CHECK(mr.error[0] != 0);
    media_import_free(&mr);
    static const unsigned char bad2[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    write_bytes(mp, bad2, sizeof(bad2));
    CHECK(!media_import_all_ex(zp, mp, root, NULL, NULL, NULL, &mr));
    media_import_free(&mr);
    static const unsigned char bad3[] = { 0x28, 0xB5, 0x2F, 0xFD, 0x00, 0x00 };   /* bogus zstd */
    write_bytes(mp, bad3, sizeof(bad3));
    CHECK(!media_import_all_ex(zp, mp, root, NULL, NULL, NULL, &mr));
    CHECK(strstr(mr.error, "decompress") != NULL);
    media_import_free(&mr);

    /* A valid protobuf list whose name tries to escape the media directory. */
    unsigned char pb[64];
    const char *evil = "../../escape.png";
    size_t el = strlen(evil), p = 0;
    pb[p++] = 0x0A; pb[p++] = (unsigned char)(2 + el);
    pb[p++] = 0x0A; pb[p++] = (unsigned char)el;
    memcpy(pb + p, evil, el); p += el;
    write_bytes(mp, pb, p);
    CHECK(media_import_all_ex(zp, mp, root, NULL, NULL, NULL, &mr));
    CHECK(mr.count == 1 && mr.copied == 1);
    CHECK(strcmp(mr.entries[0].dest_name, "escape.png") == 0);
    char esc[800];
    snprintf(esc, sizeof(esc), "%s/escape.png", td);
    CHECK(file_size(esc) < 0);
    snprintf(esc, sizeof(esc), "%s/out/escape.png", td);
    CHECK(file_size(esc) < 0);
    media_import_free(&mr);

    /* Names that are all dots/slashes are rejected, not turned into files. */
    const char *json3 = "{\"0\":\"..\",\"1\":\"/\"}";
    write_bytes(mp, json3, strlen(json3));
    CHECK(media_import_all_ex(zp, mp, root, NULL, NULL, NULL, &mr));
    CHECK(mr.skipped_unsafe == 2 && mr.copied == 0);
    media_import_free(&mr);
    return 0;
}

/* ====================================================================== */
/*  Scheduling, naming, commit rollback, atomic save, scale               */
/* ====================================================================== */

static int test_due_dates(void) {
    const double now = 1791440000.0;
    const int64_t crt = 1791432000;
    Card c;

    memset(&c, 0, sizeof(c));
    AnkiCard rv = { .type = 2, .queue = 2, .due = 5, .ivl = 21, .factor = 2500, .reps = 3 };
    import_apply_scheduling_ex(&rv, crt, now, &c);
    CHECK(c.state == CARD_REVIEW && fabs(c.due - (crt + 5 * 86400.0)) < 0.5);

    AnkiCard overdue = { .type = 2, .queue = 2, .due = 0, .ivl = 3, .factor = 2500 };
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&overdue, crt, now, &c);
    CHECK(fabs(c.due - (double)crt) < 0.5 && c.due < now);   /* already due */

    AnkiCard learn = { .type = 1, .queue = 1, .due = (int)(crt + 600) };
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&learn, crt, now, &c);
    CHECK(c.state == CARD_LEARNING && fabs(c.due - (crt + 600.0)) < 0.5);

    AnkiCard daylearn = { .type = 3, .queue = 3, .due = 2, .ivl = 1 };
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&daylearn, crt, now, &c);
    CHECK(c.state == CARD_RELEARNING && fabs(c.due - (crt + 2 * 86400.0)) < 0.5);

    AnkiCard fresh = { .type = 0, .queue = 0, .due = 12345 };   /* position, not a date */
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&fresh, crt, now, &c);
    CHECK(c.state == CARD_NEW && c.due < now);

    AnkiCard absurd = { .type = 2, .queue = 2, .due = 900000000 };
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&absurd, crt, now, &c);
    CHECK(c.due < now);                                       /* ignored, not year 4000 */

    AnkiCard unknown_crt = { .type = 2, .queue = 2, .due = 9 };
    memset(&c, 0, sizeof(c));
    import_apply_scheduling_ex(&unknown_crt, 0, now, &c);
    CHECK(c.due < now);                                       /* no crt: due now */
    return 0;
}

static int test_deck_name_fit(void) {
    static AnkiCollection col;
    memset(&col, 0, sizeof(col));
    col.decks = (AnkiDeck *)calloc(2, sizeof(AnkiDeck));
    col.cards = (AnkiCard *)calloc(2, sizeof(AnkiCard));
    col.deck_count = 2; col.card_count = 2;
    col.decks[0].id = 1;
    snprintf(col.decks[0].name, sizeof(col.decks[0].name), "%s",
             "日本語学習::文法::初級::第一課::助詞の使い方::例文集");
    col.decks[1].id = 2;
    snprintf(col.decks[1].name, sizeof(col.decks[1].name), "%s",
             "非常に長いデッキの名前が続きますこれはとても長い名前でありまして六十四バイトを超えます");
    col.cards[0].did = 1; col.cards[0].nid = 1;
    col.cards[1].did = 2; col.cards[1].nid = 2;

    ImportDeckPlan plan;
    CHECK(import_build_deck_plan(&col, &plan));
    CHECK(plan.deck_count == 2);
    for (int i = 0; i < 2; i++) {
        CHECK(strlen(plan.decks[i].name) < MAX_DECK_NAME);
        size_t bad = 0;
        CHECK(sq_utf8_validate(plan.decks[i].name, &bad));    /* never cut mid-character */
        CHECK(plan.decks[i].name_shortened);
    }
    CHECK(strstr(plan.decks[0].name, "例文集") != NULL);       /* leaf kept */
    CHECK(strncmp(plan.decks[0].name, "...::", 5) == 0);

    static DeckList dl;
    decklist_init(&dl);
    char out[MAX_DECK_NAME];
    CHECK(import_unique_deck_name(&dl, plan.decks[1].name, out, sizeof(out)));
    decklist_add(&dl, out, (Color){1, 2, 3, 255});
    char out2[MAX_DECK_NAME];
    CHECK(import_unique_deck_name(&dl, plan.decks[1].name, out2, sizeof(out2)));
    CHECK(strcmp(out, out2) != 0);
    size_t bad = 0;
    CHECK(sq_utf8_validate(out2, &bad) && strlen(out2) < MAX_DECK_NAME);
    decklist_free(&dl);
    anki_collection_free(&col);
    return 0;
}

static int test_commit_rollback(void) {
    static DeckList app, scratch;
    decklist_init(&app);
    decklist_init(&scratch);
    for (int i = 0; i < MAX_DECKS - 1; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "Existing %d", i);
        CHECK(decklist_add(&app, nm, (Color){1, 2, 3, 255}) != NULL);
    }
    /* Merging target that would receive new cards. */
    Deck *ex = import_find_existing_deck(&app, "Existing 0");
    CHECK(ex && deck_add_card(ex, "old", "old", "") != NULL);
    int before_cards = ex->card_count, before_decks = app.count, before_next = app.next_id;

    /* Scratch: one deck merging into "Existing 0", plus two brand-new decks
       (only one slot left) -> the whole import must be refused. */
    Deck *m = decklist_add(&scratch, "Existing 0", (Color){1, 2, 3, 255});
    Card *mc = deck_add_card(m, "q", "a", "");
    CHECK(mc != NULL); mc->anki_card_id = 77;
    Deck *n1 = decklist_add(&scratch, "New A", (Color){1, 2, 3, 255});
    Deck *n2 = decklist_add(&scratch, "New B", (Color){1, 2, 3, 255});
    CHECK(deck_add_card(n1, "x", "y", "") && deck_add_card(n2, "x", "y", ""));

    ImportWarnings w; import_warnings_init(&w);
    ImportCommitResult cr;
    CHECK(!import_commit_ex(&app, &scratch, IMPORT_MODE_UPDATE, &w, &cr));
    CHECK(strstr(w.message, "Not enough room") != NULL);
    CHECK(app.count == before_decks && app.next_id == before_next);
    CHECK(ex->card_count == before_cards);                    /* untouched */
    CHECK(cr.cards_added == 0);
    decklist_free(&scratch);

    /* A merge-only import needs no free slots and succeeds. */
    decklist_init(&scratch);
    m = decklist_add(&scratch, "Existing 0", (Color){1, 2, 3, 255});
    mc = deck_add_card(m, "q", "a", ""); mc->anki_card_id = 77;
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_UPDATE, &w, &cr));
    CHECK(cr.decks_merged == 1 && cr.cards_added == 1);
    CHECK(ex->card_count == before_cards + 1);
    CHECK(cr.first_deck_id == ex->id);
    decklist_free(&app);
    return 0;
}

static int test_atomic_save(void) {
    char tmp[] = "/tmp/sq-save-XXXXXX";
    char *td = mkdtemp(tmp);
    char sp[600], blocker[700];
    snprintf(sp, sizeof(sp), "%s/game.sav", td);

    static SaveData sd;
    memset(&sd, 0, sizeof(sd));
    save_defaults(&sd);
    CHECK(save_write(&sd, sp));
    int good = file_size(sp);
    CHECK(good > 0);

    /* Make the temp file impossible to create: the existing save must survive. */
    snprintf(blocker, sizeof(blocker), "%s.tmp", sp);
    CHECK(mkdir(blocker, 0755) == 0);
    CHECK(!save_write(&sd, sp));
    CHECK(file_size(sp) == good);
    static SaveData again;
    memset(&again, 0, sizeof(again));
    CHECK(save_load(&again, sp));
    rmdir(blocker);
    CHECK(save_write(&sd, sp));
    CHECK(file_size(blocker) < 0);                            /* no temp file left behind */
    decklist_free(&sd.decks);
    decklist_free(&again.decks);
    return 0;
}

static int test_large_import_scale(void) {
    const int N = 60000, DECKS = 10;
    static AnkiCollection col;
    memset(&col, 0, sizeof(col));
    col.models = (AnkiModel *)calloc(1, sizeof(AnkiModel));
    col.model_count = 1;
    col.models[0].id = 1;
    snprintf(col.models[0].name, sizeof(col.models[0].name), "Basic");
    col.models[0].field_count = 2;
    snprintf(col.models[0].field_names[0], 64, "Front");
    snprintf(col.models[0].field_names[1], 64, "Back");
    col.models[0].template_count = 1;
    snprintf(col.models[0].templates[0].qfmt, 2048, "{{Front}}");
    snprintf(col.models[0].templates[0].afmt, 2048, "{{Back}}");
    col.decks = (AnkiDeck *)calloc((size_t)DECKS, sizeof(AnkiDeck));
    col.deck_count = DECKS;
    for (int d = 0; d < DECKS; d++) {
        col.decks[d].id = 100 + d;
        snprintf(col.decks[d].name, sizeof(col.decks[d].name), "Big::Deck %d", d);
    }
    col.notes = (AnkiNote *)calloc((size_t)N, sizeof(AnkiNote));
    col.cards = (AnkiCard *)calloc((size_t)N, sizeof(AnkiCard));
    col.note_count = N; col.card_count = N;
    for (int i = 0; i < N; i++) {
        AnkiNote *nt = &col.notes[i];
        nt->id = 1700000000000LL + (int64_t)((i * 7919) % N);   /* scrambled order */
        nt->mid = 1;
        nt->field_count = 2;
        char b[64];
        snprintf(b, sizeof(b), "word %d", i); nt->fields[0] = strdup(b);
        snprintf(b, sizeof(b), "meaning %d", i); nt->fields[1] = strdup(b);
        snprintf(nt->tags, sizeof(nt->tags), " bulk ");
        AnkiCard *ac = &col.cards[i];
        ac->id = 1800000000000LL + i;
        ac->nid = nt->id;
        ac->did = 100 + (i % DECKS);
        ac->type = (i % 2) ? 2 : 0; ac->queue = ac->type; ac->due = i % 50; ac->ivl = 10; ac->factor = 2500;
    }
    col.crt = 1791432000;
    anki_collection_index_notes(&col);

    double t0 = now_secs();
    static DeckList scratch, app;
    decklist_init(&scratch);
    decklist_init(&app);
    ImportJob job;
    import_job_init(&job, &col, NULL, &scratch);
    CHECK(job.plan.deck_count == DECKS);
    CHECK(job.plan.decks[0].note_count == N / DECKS);
    while (!import_job_done(&job)) import_job_step(&job, 500);
    CHECK(job.cards_added == N && job.notes_added == N);
    import_job_free(&job);
    ImportWarnings w; import_warnings_init(&w);
    ImportCommitResult cr;
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_NEW, &w, &cr));
    /* Re-import into the same decks: every card is a duplicate. */
    decklist_init(&scratch);
    import_job_init(&job, &col, NULL, &scratch);
    while (!import_job_done(&job)) import_job_step(&job, 500);
    import_job_free(&job);
    CHECK(import_commit_ex(&app, &scratch, IMPORT_MODE_UPDATE, &w, &cr));
    CHECK(cr.cards_added == 0 && cr.cards_skipped_duplicate == N);
    double dt = now_secs() - t0;
    printf("   (60k cards imported twice in %.2fs)\n", dt);
    CHECK(dt < 15.0);                                          /* was minutes when O(n^2) */

    decklist_free(&app);
    anki_collection_free(&col);
    return 0;
}



/* A "Picture" field that holds only an <img> has empty text once the HTML is
   stripped, but its image must still be reported on the answer side with the
   field index, because the study screen draws it from that reference (Kaishi
   decks put every illustration in such a field). */
static int test_image_only_field(void) {
    static AnkiCollection col;
    memset(&col, 0, sizeof(col));
    col.models = (AnkiModel *)calloc(1, sizeof(AnkiModel));
    col.model_count = 1;
    col.models[0].id = 1;
    snprintf(col.models[0].name, sizeof(col.models[0].name), "Pic");
    col.models[0].field_count = 2;
    snprintf(col.models[0].field_names[0], 64, "Word");
    snprintf(col.models[0].field_names[1], 64, "Picture");
    col.models[0].template_count = 1;
    snprintf(col.models[0].templates[0].qfmt, 2048, "{{Word}}");
    snprintf(col.models[0].templates[0].afmt, 2048, "{{Word}}<br>{{Picture}}");
    col.notes = (AnkiNote *)calloc(1, sizeof(AnkiNote));
    col.cards = (AnkiCard *)calloc(1, sizeof(AnkiCard));
    col.note_count = 1; col.card_count = 1;
    col.notes[0].id = 10; col.notes[0].mid = 1; col.notes[0].field_count = 2;
    col.notes[0].fields[0] = strdup("私");
    col.notes[0].fields[1] = strdup("<img alt=\"x\" src=\"jikosyoukai_man-0f01.webp\">");
    col.cards[0].id = 20; col.cards[0].nid = 10; col.cards[0].did = 1;

    ConvertedCard cc;
    memset(&cc, 0, sizeof(cc));
    CHECK(anki_convert_card(&col, &col.cards[0], &cc));
    CHECK(cc.field_count == 2);
    CHECK(strcmp(cc.field_values[1], "") == 0);                /* text really is empty */
    int found = -1;
    for (int i = 0; i < cc.media_ref_count; i++)
        if (strcmp(cc.media_refs[i], "jikosyoukai_man-0f01.webp") == 0) found = i;
    CHECK(found >= 0);
    CHECK(cc.media_kinds[found] == 0);                          /* image */
    CHECK(cc.media_sides[found] == 1);                          /* answer side */
    CHECK(cc.media_fields[found] == 1);                         /* the Picture field */
    anki_converted_card_free(&cc);
    anki_collection_free(&col);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s tests/fixtures/anki_media.apkg\n", argv[0]);
        return 2;
    }

    printf("== strict UTF-8 ==\n");
    if (test_utf8() != 0) return 1;

    printf("== HTML/media reference extraction ==\n");
    if (test_html() != 0) return 1;
    if (test_html_formatting() != 0) return 1;

    printf("== package variants & safe failure ==\n");
    if (test_package_variants(argv[1]) != 0) return 1;

    printf("== import manager (deck grouping) ==\n");
    if (test_import_manager(argv[1]) != 0) return 1;

    printf("== media edge cases ==\n");
    if (test_media_edge_cases(argv[1]) != 0) return 1;

    printf("== scheduling, duplicates, commit ==\n");
    if (test_scheduling() != 0) return 1;
    if (test_duplicate_and_commit(argv[1]) != 0) return 1;

    printf("== real Anki packages (modern schema 18, zstd media) ==\n");
    if (test_real_package_layouts(argv[1]) != 0) return 1;
    if (test_real_media(argv[1]) != 0) return 1;
    if (test_real_pipeline(argv[1]) != 0) return 1;
    if (test_real_variants(argv[1]) != 0) return 1;

    printf("== hostile input, limits, rollback, scale ==\n");
    if (test_zip_stream_limits() != 0) return 1;
    if (test_media_hostile_inputs() != 0) return 1;
    if (test_due_dates() != 0) return 1;
    if (test_deck_name_fit() != 0) return 1;
    if (test_commit_rollback() != 0) return 1;
    if (test_atomic_save() != 0) return 1;
    if (test_large_import_scale() != 0) return 1;
    if (test_image_only_field() != 0) return 1;

    char tmp[] = "/tmp/studyquest-import-test-XXXXXX";
    char *tmpdir = mkdtemp(tmp);
    CHECK(tmpdir != NULL);

    ApkgHandle h;
    CHECK(apkg_open(argv[1], tmpdir, &h));
    CHECK(h.variant == APKG_VARIANT_LEGACY1);
    CHECK(h.media_entry_count == 5);

    AnkiCollection col;
    CHECK(anki_parse(h.db_path, &col));
    CHECK(col.note_count == 1);
    CHECK(col.card_count == 1);
    CHECK(col.notes[0].field_count == 4);
    CHECK(strstr(col.notes[0].fields[0], "食べる") != NULL);
    CHECK(strstr(col.notes[0].fields[0], "𠮷") != NULL);
    CHECK(strlen(col.notes[0].fields[0]) > 1000); /* no 8192/384 style truncation */
    CHECK(strstr(col.notes[0].fields[3], "#x9AD9") != NULL);

    char media_root[512];
    snprintf(media_root, sizeof(media_root), "%s/sq-media", tmpdir);
    MediaImportResult mr;
    CHECK(media_import_all(argv[1], h.media_json_path, media_root, NULL, NULL, &mr));
    CHECK(mr.count == 5);
    CHECK(mr.copied == 5);
    CHECK(mr.missing == 0);

    char resolved[512];
    CHECK(media_import_resolve(&mr, "back.webp", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "back.webp") == 0);
    CHECK(media_import_resolve(&mr, "back.wav", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "back.wav") == 0);
    CHECK(media_import_resolve(&mr, "back2.wav", resolved, sizeof(resolved)));
    CHECK(strcmp(resolved, "back2.wav") == 0);

    char path[1024];
    snprintf(path, sizeof(path), "%s/images/back.webp", media_root);
    CHECK(file_size(path) > 0);
    FILE *wf = fopen(path, "rb");
    CHECK(wf != NULL);
    unsigned char sig[12] = {0};
    CHECK(fread(sig, 1, sizeof(sig), wf) == sizeof(sig));
    fclose(wf);
    CHECK(memcmp(sig, "RIFF", 4) == 0);
    CHECK(memcmp(sig + 8, "WEBP", 4) == 0);

    int w = 0, hpx = 0;
    size_t webp_bytes = 0;
    unsigned char *wb = NULL;
    wf = fopen(path, "rb");
    CHECK(wf != NULL);
    fseek(wf, 0, SEEK_END);
    long sz = ftell(wf);
    rewind(wf);
    wb = malloc((size_t)sz);
    CHECK(wb != NULL);
    CHECK(fread(wb, 1, (size_t)sz, wf) == (size_t)sz);
    fclose(wf);
    uint8_t *rgba = WebPDecodeRGBA(wb, (size_t)sz, &w, &hpx);
    free(wb);
    CHECK(rgba != NULL);
    CHECK(w > 0 && hpx > 0);
    webp_bytes = (size_t)w * (size_t)hpx * 4u;
    CHECK(webp_bytes > 0);
    WebPFree(rgba);

    CHECK(test_conversion_and_persistence(&col, &col.cards[0], media_root, &mr) == 0);

    anki_collection_free(&col);
    media_import_free(&mr);
    remove(h.db_path);
    remove(h.media_json_path);
    rmdir(tmpdir);

    printf("\nAll Anki import component tests passed.\n");
    return 0;
}
