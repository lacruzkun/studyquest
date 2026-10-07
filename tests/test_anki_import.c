#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <webp/decode.h>

#include "core/utf8.h"
#include "import/apkg_importer.h"
#include "import/anki_html.h"
#include "import/anki_parser.h"
#include "import/media_importer.h"
#include "import/anki_convert.h"
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
    for (int i = 0; i < cc.media_ref_count; i++) {
        if (cc.media_sides[i] != 1) continue;
        if (cc.media_kinds[i] == 0 && strcmp(cc.media_refs[i], "back.webp") == 0)
            back_image = true;
        if (cc.media_kinds[i] == 1 && strcmp(cc.media_refs[i], "back.wav") == 0)
            back_audio1 = true;
        if (cc.media_kinds[i] == 1 && strcmp(cc.media_refs[i], "back2.wav") == 0)
            back_audio2 = true;
    }
    CHECK(back_image && back_audio1 && back_audio2);

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
        card_add_media_ref(c, resolved, cc.media_kinds[i], cc.media_sides[i]);
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
    CHECK(strcmp(loaded_c->media_refs[3].ref, "back.wav") == 0);
    CHECK(loaded_c->media_refs[3].kind == 1 && loaded_c->media_refs[3].side == 1);
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
