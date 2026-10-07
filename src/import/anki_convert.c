#include "import/anki_convert.h"
#include "import/anki_html.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const AnkiModel *find_model(const AnkiCollection *col, int64_t mid) {
    for (int i = 0; i < col->model_count; i++)
        if (col->models[i].id == mid) return &col->models[i];
    return NULL;
}

static int media_kind_from_filename(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot) return 2;
    static const char *images[] = {
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".tif", ".tiff", ".svg", ".ico", NULL
    };
    static const char *audio[] = {
        ".mp3", ".ogg", ".oga", ".wav", ".flac", ".m4a", ".opus", ".aac", ".wma", ".aiff", ".aif", NULL
    };
    for (int i = 0; images[i]; i++)
        if (strcasecmp(dot, images[i]) == 0) return 0;
    for (int i = 0; audio[i]; i++)
        if (strcasecmp(dot, audio[i]) == 0) return 1;
    return 2;
}

static void media_add(ConvertedCard *out, const char *ref, int kind, int side) {
    if (!out || !ref || !*ref) return;
    for (int i = 0; i < out->media_ref_count; i++)
        if (out->media_kinds[i] == (unsigned char)kind &&
            out->media_sides[i] == (unsigned char)(side ? 1 : 0) &&
            strcmp(out->media_refs[i], ref) == 0)
            return;

    if (out->media_ref_count >= ANKI_MAX_MEDIA_REFS) {
        fprintf(stderr,
                "StudyQuest ANKI: media reference limit reached; additional ref '%s' ignored\n",
                ref);
        return;
    }

    size_t ref_len = strlen(ref);
    if (ref_len >= sizeof(out->media_refs[0])) {
        fprintf(stderr,
                "StudyQuest ANKI: media reference too long (%zu bytes); "
                "reference rejected: '%s'\n",
                ref_len, ref);
        return;
    }

    int i = out->media_ref_count++;
    memcpy(out->media_refs[i], ref, ref_len + 1);
    out->media_kinds[i] = (unsigned char)kind;
    out->media_sides[i] = (unsigned char)(side ? 1 : 0);
}

static void collect_media(ConvertedCard *out, const char *source, int side) {
    if (!source || !*source) return;
    char dummy_text[1] = {0};
    char refs[ANKI_MAX_MEDIA_REFS][512] = {{0}};
    int count = 0;
    anki_html_process(source, dummy_text, sizeof(dummy_text),
                      refs, &count, ANKI_MAX_MEDIA_REFS);
    for (int i = 0; i < count; i++)
        media_add(out, refs[i], media_kind_from_filename(refs[i]), side);
}

bool anki_convert_card(const AnkiCollection *col,
                       const AnkiCard *card,
                       ConvertedCard *out) {
    memset(out, 0, sizeof(*out));

    const AnkiNote *note = anki_find_note(col, card->nid);
    if (!note) return false;

    const AnkiModel *model = find_model(col, note->mid);
    if (!model) return false;

    int n = model->field_count;
    if (n > note->field_count) n = note->field_count;
    if (n > MAX_FIELDS) n = MAX_FIELDS;

    for (int i = 0; i < n; i++) {
        snprintf(out->field_names[i], MAX_FIELD_NAME, "%s", model->field_names[i]);

        const char *src = note->fields[i] ? note->fields[i] : "";
        size_t cap = strlen(src) + 1;
        out->field_values[i] = (char *)malloc(cap ? cap : 1);
        if (!out->field_values[i]) {
            anki_converted_card_free(out);
            return false;
        }
        anki_html_process(src, out->field_values[i], cap,
                          NULL, NULL, 0);
        collect_media(out, src, i == 0 ? 0 : 1);
    }
    out->field_count = n;

    /* Static media can live directly in the selected card template. */
    if (card->ord >= 0 && card->ord < model->template_count) {
        collect_media(out, model->templates[card->ord].qfmt, 0);
        collect_media(out, model->templates[card->ord].afmt, 1);
    }

    return true;
}

void anki_converted_card_free(ConvertedCard *card) {
    if (!card) return;
    for (int i = 0; i < MAX_FIELDS; i++) {
        free(card->field_values[i]);
        card->field_values[i] = NULL;
    }
    card->field_count = 0;
}
