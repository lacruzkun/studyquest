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
