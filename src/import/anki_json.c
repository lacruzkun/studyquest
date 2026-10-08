/* anki_json.c */
#include "import/anki_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

bool anki_json_parse_models(const char *json, AnkiModel **out, int *count, char *err) {
    *out = NULL; *count = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, 512, "Could not parse the note types JSON.");
        return false;
    }

    int n = cJSON_GetArraySize(root);
    AnkiModel *arr = n > 0 ? calloc((size_t)n, sizeof(AnkiModel)) : NULL;
    if (n > 0 && !arr) { cJSON_Delete(root); return false; }

    int i = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, root) {
        AnkiModel *am = &arr[i++];
        cJSON *id   = cJSON_GetObjectItem(m, "id");
        cJSON *name = cJSON_GetObjectItem(m, "name");
        cJSON *type = cJSON_GetObjectItem(m, "type");
        cJSON *flds = cJSON_GetObjectItem(m, "flds");
        cJSON *tmpl = cJSON_GetObjectItem(m, "tmpls");

        am->id = id ? (int64_t)id->valuedouble : 0;
        /* Fall back to the JSON object key (the canonical model id) when
           the "id" field is missing or zero. */
        if (am->id == 0 && m->string)
            am->id = (int64_t)strtoll(m->string, NULL, 10);
        copy_str(am->name, sizeof(am->name), name ? name->valuestring : "");
        am->is_cloze = (type && type->valueint == 1);

        if (flds) {
            int fc = cJSON_GetArraySize(flds);
            for (int k = 0; k < fc && k < ANKI_MAX_FIELDS; k++) {
                cJSON *f = cJSON_GetArrayItem(flds, k);
                cJSON *fn = cJSON_GetObjectItem(f, "name");
                copy_str(am->field_names[k], sizeof(am->field_names[k]),
                         fn ? fn->valuestring : "");
                am->field_count++;
            }
        }

        if (tmpl) {
            int tc = cJSON_GetArraySize(tmpl);
            for (int k = 0; k < tc && k < 16; k++) {
                cJSON *t = cJSON_GetArrayItem(tmpl, k);
                cJSON *tn = cJSON_GetObjectItem(t, "name");
                cJSON *qf = cJSON_GetObjectItem(t, "qfmt");
                cJSON *af = cJSON_GetObjectItem(t, "afmt");
                copy_str(am->templates[am->template_count].name, 64,
                         tn ? tn->valuestring : "");
                copy_str(am->templates[am->template_count].qfmt, 2048,
                         qf ? qf->valuestring : "");
                copy_str(am->templates[am->template_count].afmt, 2048,
                         af ? af->valuestring : "");
                am->template_count++;
            }
        }
    }

    cJSON_Delete(root);
    *out = arr;
    *count = n;
    return true;
}

bool anki_json_parse_decks(const char *json, AnkiDeck **out, int *count, char *err) {
    *out = NULL; *count = 0;

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        snprintf(err, 512, "Could not parse the decks JSON.");
        return false;
    }

    int n = cJSON_GetArraySize(root);
    AnkiDeck *arr = n > 0 ? calloc((size_t)n, sizeof(AnkiDeck)) : NULL;
    if (n > 0 && !arr) { cJSON_Delete(root); return false; }

    int i = 0;
    cJSON *d;
    cJSON_ArrayForEach(d, root) {
        AnkiDeck *ad = &arr[i++];
        cJSON *name = cJSON_GetObjectItem(d, "name");
        cJSON *dyn  = cJSON_GetObjectItem(d, "dyn");
        /* The deck id is the JSON object key ("1", "2", ...), not the
           value node. `d->string` holds that key during object iteration. */
        ad->id = d->string ? (int64_t)strtoll(d->string, NULL, 10) : 0;
        copy_str(ad->name, sizeof(ad->name), name ? name->valuestring : "");
        ad->is_filtered = dyn && dyn->valueint == 1;
    }

    cJSON_Delete(root);
    *out = arr;
    *count = n;
    return true;
}
