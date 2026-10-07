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

/* Substitute {{FieldName}} in a template. Also strips the common
   {{#Field}}..{{/Field}} conditional markers, keeping the inner text
   unconditionally. This is not a real template engine, but it gets
   front/back templates right on every deck I've tested. */
static void render_template(const char *tmpl, const AnkiNote *note,
                            const AnkiModel *model,
                            char *out, size_t cap) {
    size_t w = 0;
    const char *p = tmpl;
    while (*p && w + 1 < cap) {
        if (p[0] == '{' && p[1] == '{') {
            const char *end = strstr(p + 2, "}}");
            if (!end) break;
            char inner[128];
            size_t flen = (size_t)(end - (p + 2));
            if (flen >= sizeof(inner)) flen = sizeof(inner) - 1;
            memcpy(inner, p + 2, flen);
            inner[flen] = 0;

            /* Trim. */
            char *s = inner;
            while (*s == ' ') s++;
            char *e = s + strlen(s);
            while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;

            /* Conditional markers: {{#Field}} {{/Field}} {{^Field}} —
               just drop them, keeping surrounding text intact. */
            bool conditional = (s[0] == '#' || s[0] == '/' || s[0] == '^');
            if (conditional) { p = end + 2; continue; }

            /* {{type:Field}} — treat as plain field. */
            if (strncmp(s, "type:", 5) == 0) s += 5;

            /* {{FrontSide}} — emit nothing; we don't have it handy here. */
            if (strcasecmp(s, "FrontSide") == 0) {
                p = end + 2;
                continue;
            }

            int idx = -1;
            for (int k = 0; k < model->field_count; k++)
                if (strcasecmp(model->field_names[k], s) == 0) { idx = k; break; }

            if (idx >= 0 && idx < note->field_count) {
                const char *val = note->fields[idx];
                size_t vlen = strlen(val);
                if (w + vlen >= cap) vlen = cap - w - 1;
                memcpy(out + w, val, vlen);
                w += vlen;
            }
            p = end + 2;
        } else {
            out[w++] = *p++;
        }
    }
    out[w] = 0;
}

bool anki_convert_card(const AnkiCollection *col,
                       const AnkiCard *card,
                       ConvertedCard *out) {
    memset(out, 0, sizeof(*out));

    const AnkiNote *note = anki_find_note(col, card->nid);
    if (!note) return false;

    const AnkiModel *model = find_model(col, note->mid);
    if (!model) return false;

    /* We don't use the card template to decide what goes on the front —
       we just take the note's fields in order. Field 0 is the prompt;
       fields 1..N-1 are shown after reveal. This matches every Japanese
       deck I've tested (Kaishi, Core, JLPT) and works fine for math,
       chess, or anything else with a labelled field list. */
    int n = model->field_count;
    if (n > note->field_count) n = note->field_count;
    if (n > MAX_FIELDS) n = MAX_FIELDS;

    for (int i = 0; i < n; i++) {
        snprintf(out->field_names[i], MAX_FIELD_NAME, "%s", model->field_names[i]);
        anki_html_process(note->fields[i],
                          out->field_values[i], MAX_FIELD_VALUE,
                          out->media_refs, &out->media_ref_count, 8);
    }
    out->field_count = n;

    return true;
}
