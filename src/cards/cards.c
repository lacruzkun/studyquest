#include "cards.h"
#include "study/scheduler.h"
#include "core/utf8.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void safe_copy(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static char *dup_string(const char *src) {
    if (!src) src = "";
    size_t n = strlen(src);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, src, n + 1);
    return p;
}

void card_free(Card *c) {
    if (!c) return;
    for (int i = 0; i < MAX_FIELDS; i++) {
        free(c->field_values[i]);
        c->field_values[i] = NULL;
    }
    c->field_count = 0;
}

static void card_move(Card *dst, Card *src) {
    if (!dst || !src || dst == src) return;
    card_free(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

static void deck_free(Deck *d) {
    if (!d) return;
    for (int i = 0; i < d->card_count; i++) card_free(&d->cards[i]);
    d->card_count = 0;
}

static void deck_move(Deck *dst, Deck *src) {
    if (!dst || !src || dst == src) return;
    deck_free(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

void decklist_init(DeckList *dl) {
    memset(dl, 0, sizeof(*dl));
    dl->next_id = 1;
}

void decklist_free(DeckList *dl) {
    if (!dl) return;
    for (int i = 0; i < dl->count; i++) deck_free(&dl->decks[i]);
    memset(dl, 0, sizeof(*dl));
}

Deck *decklist_add(DeckList *dl, const char *name, Color color) {
    if (dl->count >= MAX_DECKS) return NULL;
    Deck *d = &dl->decks[dl->count++];
    memset(d, 0, sizeof(*d));
    d->id = dl->next_id++;
    safe_copy(d->name, MAX_DECK_NAME, name);
    d->color = color;
    return d;
}

Deck *decklist_find(DeckList *dl, int id) {
    for (int i = 0; i < dl->count; i++)
        if (dl->decks[i].id == id) return &dl->decks[i];
    return NULL;
}

void decklist_remove(DeckList *dl, int idx) {
    if (idx < 0 || idx >= dl->count) return;
    deck_free(&dl->decks[idx]);
    for (int i = idx; i < dl->count - 1; i++)
        deck_move(&dl->decks[i], &dl->decks[i + 1]);
    memset(&dl->decks[dl->count - 1], 0, sizeof(dl->decks[dl->count - 1]));
    dl->count--;
}

/* ------------------------------------------------------------------ */
/*  Card creation                                                     */
/* ------------------------------------------------------------------ */

Card *deck_add_card(Deck *d, const char *front, const char *back, const char *tags) {
    if (d->card_count >= MAX_CARDS) return NULL;
    Card *c = &d->cards[d->card_count];
    memset(c, 0, sizeof(*c));
    c->id = d->card_count + 1;
    safe_copy(c->front, MAX_TEXT, front);
    safe_copy(c->back,  MAX_TEXT, back);
    safe_copy(c->tags,  MAX_TAGS, tags ? tags : "");
    c->state = CARD_NEW;
    c->ease = 2.5f;
    c->due = (double)time(NULL) - 1.0;
    c->last_rating = -1;
    d->card_count++;
    return c;
}

Card *deck_add_card_fields(Deck *d,
                           char (*names)[MAX_FIELD_NAME],
                           const char *const *values,
                           int n,
                           const char *tags) {
    if (n <= 0) return NULL;
    if (n > MAX_FIELDS) n = MAX_FIELDS;
    if (!values) return NULL;

    Card *c = deck_add_card(d, values[0], n > 1 ? values[1] : "", tags);
    if (!c) return NULL;

    for (int i = 0; i < n; i++) {
        safe_copy(c->field_names[i], MAX_FIELD_NAME, names ? names[i] : "");
        c->field_values[i] = dup_string(values[i] ? values[i] : "");
        if (!c->field_values[i]) {
            card_free(c);
            d->card_count--;
            return NULL;
        }
    }
    c->field_count = n;
    return c;
}

void deck_remove_card(Deck *d, int idx) {
    if (idx < 0 || idx >= d->card_count) return;
    card_free(&d->cards[idx]);
    for (int i = idx; i < d->card_count - 1; i++)
        card_move(&d->cards[i], &d->cards[i + 1]);
    memset(&d->cards[d->card_count - 1], 0, sizeof(d->cards[d->card_count - 1]));
    d->card_count--;
    for (int i = 0; i < d->card_count; i++) d->cards[i].id = i + 1;
}

/* ------------------------------------------------------------------ */
/*  Field helpers                                                     */
/* ------------------------------------------------------------------ */

void card_add_field(Card *c, const char *name, const char *value) {
    if (!c || c->field_count >= MAX_FIELDS) return;
    int i = c->field_count;
    char *copy = dup_string(value ? value : "");
    if (!copy) return;
    safe_copy(c->field_names[i], MAX_FIELD_NAME, name ? name : "");
    c->field_values[i] = copy;
    c->field_count++;
}

const char *card_field_name(const Card *c, int i) {
    if (!c || i < 0 || i >= c->field_count) return NULL;
    return c->field_names[i];
}

const char *card_field_value(const Card *c, int i) {
    if (!c || i < 0 || i >= c->field_count) return NULL;
    return c->field_values[i];
}

const char *card_find_field(const Card *c, const char *name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->field_count; i++)
        if (strcasecmp(c->field_names[i], name) == 0)
            return c->field_values[i];
    return NULL;
}

bool card_has_cjk(const char *s) {
    if (!s) return false;
    const char *p = s;
    while (*p) {
        uint32_t cp = 0;
        size_t n = 0;
        if (!sq_utf8_decode(p, &cp, &n)) {
            p++;
            continue;
        }
        if ((cp >= 0x3040 && cp <= 0x30FF) ||
            (cp >= 0x3400 && cp <= 0x4DBF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0x20000 && cp <= 0x2FA1F))
            return true;
        p += n;
    }
    return false;
}

bool card_is_japanese(const Card *c) {
    if (!c || c->field_count == 0) return false;
    return card_has_cjk(c->field_values[0]);
}

void card_add_media_ref(Card *c, const char *filename, int kind, int side, int field_index) {
    if (!c || !filename || !*filename) return;

    unsigned char stored_field = (unsigned char)(field_index < 0 ? 255 : field_index);
    for (int i = 0; i < c->media_ref_count; i++) {
        if (c->media_refs[i].kind == (unsigned char)kind &&
            c->media_refs[i].side == (unsigned char)(side ? 1 : 0) &&
            c->media_refs[i].field_index == stored_field &&
            strcmp(c->media_refs[i].ref, filename) == 0) {
            return;
        }
    }

    /* Keep the legacy aliases useful while retaining every discovered ref. */
    if (kind == 0 && !c->image_ref[0]) safe_copy(c->image_ref, sizeof(c->image_ref), filename);
    if (kind == 1 && !c->audio_ref[0]) safe_copy(c->audio_ref, sizeof(c->audio_ref), filename);

    if (c->media_ref_count >= MAX_CARD_MEDIA_REFS) {
        TraceLog(LOG_WARNING,
                 "CARD: media reference limit reached; ignoring additional %s reference '%s'",
                 kind == 0 ? "image" : kind == 1 ? "audio" : "other", filename);
        return;
    }

    int i = c->media_ref_count++;
    safe_copy(c->media_refs[i].ref, sizeof(c->media_refs[i].ref), filename);
    c->media_refs[i].kind = (unsigned char)kind;
    c->media_refs[i].side = (unsigned char)(side ? 1 : 0);
    c->media_refs[i].field_index = stored_field;
}

void card_set_image(Card *c, const char *filename) {
    if (!c) return;
    safe_copy(c->image_ref, sizeof(c->image_ref), filename ? filename : "");
    if (filename && *filename) card_add_media_ref(c, filename, 0, 0, 0);
}

void card_set_audio(Card *c, const char *filename) {
    if (!c) return;
    safe_copy(c->audio_ref, sizeof(c->audio_ref), filename ? filename : "");
    if (filename && *filename) card_add_media_ref(c, filename, 1, 0, 0);
}

int deck_due_count(const Deck *d, double now) {
    int n = 0;
    for (int i = 0; i < d->card_count; i++)
        if (scheduler_card_due(&d->cards[i], now)) n++;
    return n;
}

float deck_retention(const Deck *d) {
    int reviewed = 0, correct = 0;
    for (int i = 0; i < d->card_count; i++) {
        const Card *c = &d->cards[i];
        if (c->reps > 0) {
            reviewed += c->reps;
            correct  += c->reps - c->lapses;
        }
    }
    if (reviewed == 0) return 0.f;
    return (float)correct / (float)reviewed;
}

/* ==================================================================== */
/*  Sample decks                                                       */
/* ==================================================================== */

typedef struct { const char *q, *a, *tags; } SampleCard;

static const SampleCard SAMPLE_C[] = {
 {"What does malloc() return?",
  "A void* pointer to allocated heap memory, or NULL if allocation fails.",
  "c,memory,heap"},
 {"What does free() do?",
  "Releases heap memory previously allocated by malloc/calloc/realloc.",
  "c,memory,heap"},
 {"Difference between stack and heap allocation?",
  "Stack: automatic, LIFO, freed on scope exit. Heap: manual via malloc/free.",
  "c,memory"},
 {"What is a pointer?",
  "A variable that stores the memory address of another object.",
  "c,pointers"},
 {"What does the `const` qualifier mean?",
  "The object cannot be modified through that name.",
  "c,const"},
 {"What is a struct?",
  "A user-defined aggregate type that groups named fields together.",
  "c,structs"},
 {"What does `typedef` do?",
  "Creates an alias for a type. Example: `typedef unsigned long u64;`.",
  "c,typedef"},
 {"What does `static` do at file scope?",
  "Gives the symbol internal linkage: visible only in that translation unit.",
  "c,static"},
 {"What does `extern` mean?",
  "Declares a symbol without defining it — tells the compiler it's elsewhere.",
  "c,extern"},
 {"What does the preprocessor do?",
  "Performs textual substitution before compilation: #include, #define, #if.",
  "c,preprocessor"},
 {"What is undefined behavior?",
  "Behavior the C standard does not specify. Anything can happen.",
  "c,ub"},
 {"How do you declare a function pointer?",
  "`int (*fp)(int, int);` — fp points to a function taking two ints.",
  "c,pointers"},
 {"What is the difference between #include <x.h> and #include \"x.h\"?",
  "Angle brackets search system paths; quotes search the local dir first.",
  "c,preprocessor"},
 {"Why use header guards?",
  "Prevent a header from being included more than once per TU.",
  "c,headers"},
 {"What is the return type of sizeof?",
  "size_t (an unsigned integer type).",
  "c,types"},
 {"What is a string literal in C?",
  "A NUL-terminated array of char stored in read-only memory.",
  "c,strings"},
 {"What is the difference between array and pointer in C?",
  "The array name decays to a pointer in most expressions, but sizeof differs.",
  "c,arrays,pointers"},
 {"What does `volatile` mean?",
  "The object may change at any time; the compiler must not cache reads.",
  "c,volatile"},
};

/* Japanese sample deck — field-based. */
typedef struct { const char *jp, *rd, *mean, *ex, *tags; } SampleJP;

static const SampleJP SAMPLE_JP[] = {
 {"こんにちは", "こんにちは", "Hello", "こんにちは、田中さん。", "greetings,common"},
 {"食べる", "たべる", "To eat", "寿司を食べます。", "verbs,food"},
 {"学生", "がくせい", "Student", "私は大学の学生です。", "nouns,school"},
 {"日本語を勉強しています。", "にほんごをべんきょうしています。",
  "I am studying Japanese.",
  "毎日、日本語を勉強しています。", "phrases,study"},
 {"おはようございます", "おはようございます", "Good morning (polite)",
  "おはようございます、先生。", "greetings,common"},
 {"ありがとう", "ありがとう", "Thank you (casual)",
  "手伝ってくれてありがとう。", "greetings,common"},
 {"水", "みず", "Water", "水を一杯ください。", "nouns,food"},
 {"本", "ほん", "Book", "この本は面白いです。", "nouns,objects"},
 {"大きい", "おおきい", "Big", "大きい犬がいます。", "adjectives"},
 {"小さい", "ちいさい", "Small", "小さい猫が好きです。", "adjectives"},
};

void decklist_init_sample(DeckList *dl) {
    decklist_init(dl);

    Deck *c = decklist_add(dl, "C Programming", (Color){ 108, 132, 255, 255 });
    if (c) {
        int n = sizeof(SAMPLE_C) / sizeof(SAMPLE_C[0]);
        for (int i = 0; i < n; i++)
            deck_add_card(c, SAMPLE_C[i].q, SAMPLE_C[i].a, SAMPLE_C[i].tags);
    }

    Deck *j = decklist_add(dl, "Japanese — N5 Starter", (Color){ 240, 120, 160, 255 });
    if (j) {
        int n = sizeof(SAMPLE_JP) / sizeof(SAMPLE_JP[0]);
        for (int i = 0; i < n; i++) {
            char names[MAX_FIELDS][MAX_FIELD_NAME];
            char values[MAX_FIELDS][MAX_FIELD_VALUE];
            snprintf(names[0], MAX_FIELD_NAME,  "Word");
            snprintf(values[0], MAX_FIELD_VALUE, "%s", SAMPLE_JP[i].jp);
            snprintf(names[1], MAX_FIELD_NAME,  "Reading");
            snprintf(values[1], MAX_FIELD_VALUE, "%s", SAMPLE_JP[i].rd);
            snprintf(names[2], MAX_FIELD_NAME,  "Meaning");
            snprintf(values[2], MAX_FIELD_VALUE, "%s", SAMPLE_JP[i].mean);
            snprintf(names[3], MAX_FIELD_NAME,  "Example");
            snprintf(values[3], MAX_FIELD_VALUE, "%s", SAMPLE_JP[i].ex);
            const char *value_ptrs[MAX_FIELDS];
            for (int k = 0; k < 4; k++) value_ptrs[k] = values[k];
            deck_add_card_fields(j, names, value_ptrs, 4, SAMPLE_JP[i].tags);
        }
    }
}

/* ==================================================================== */
/*  CSV import / export                                                */
/* ==================================================================== */

static void write_csv_field(FILE *f, const char *s) {
    bool q = false;
    for (const char *p = s; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') { q = true; break; }
    if (!q) { fputs(s, f); return; }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

bool deck_export_csv(const Deck *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Write a header with the union of field names used by this deck.
       Cards with different field counts still export fine — missing
       columns come out empty. */
    char names[MAX_FIELDS][MAX_FIELD_NAME];
    int  name_count = 0;
    for (int i = 0; i < d->card_count; i++) {
        const Card *c = &d->cards[i];
        for (int j = 0; j < c->field_count; j++) {
            bool found = false;
            for (int k = 0; k < name_count; k++)
                if (strcasecmp(names[k], c->field_names[j]) == 0) { found = true; break; }
            if (!found && name_count < MAX_FIELDS) {
                safe_copy(names[name_count++], MAX_FIELD_NAME, c->field_names[j]);
            }
        }
    }

    fprintf(f, "tags");
    for (int i = 0; i < name_count; i++) { fputc(',', f); write_csv_field(f, names[i]); }
    fputc('\n', f);

    for (int i = 0; i < d->card_count; i++) {
        const Card *c = &d->cards[i];
        write_csv_field(f, c->tags);
        for (int k = 0; k < name_count; k++) {
            fputc(',', f);
            const char *v = card_find_field(c, names[k]);
            if (!v) v = "";
            write_csv_field(f, v);
        }
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

static int read_csv_field(const char *line, int *pos, char *out, size_t cap) {
    size_t w = 0;
    const char *p = line + *pos;
    if (*p == '"') {
        p++;
        while (*p) {
            if (*p == '"') {
                if (p[1] == '"') { if (w + 1 < cap) out[w++] = '"'; p += 2; continue; }
                p++; break;
            }
            if (w + 1 < cap) out[w++] = *p;
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '\r' && *p != '\n') {
            if (w + 1 < cap) out[w++] = *p;
            p++;
        }
    }
    out[w] = 0;
    if (*p == ',') p++;
    *pos = (int)(p - line);
    return (int)w;
}

int deck_import_csv(Deck *d, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char line[4096];
    int added = 0;

    /* Read header to learn the field names. */
    char header[MAX_FIELDS + 2][MAX_FIELD_NAME];
    int header_count = 0;
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    {
        int pos = 0;
        while (header_count < MAX_FIELDS + 1) {
            char tmp[MAX_FIELD_NAME];
            int n = read_csv_field(line, &pos, tmp, sizeof(tmp));
            if (n < 0) break;
            snprintf(header[header_count], MAX_FIELD_NAME, "%s", tmp);
            header_count++;
            if (line[pos - 1] != ',') break;
        }
    }

    /* If the header didn't look like "tags,Field,Field", treat it as data
       and use front/back as default field names. */
    bool has_tags = (header_count > 0 && strcasecmp(header[0], "tags") == 0);

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == 0) continue;
        int pos = 0;

        char tags[MAX_TAGS] = {0};
        if (has_tags) {
            read_csv_field(line, &pos, tags, sizeof(tags));
            if (line[pos - 1] == ',') {} /* already advanced */
        }

        char names[MAX_FIELDS][MAX_FIELD_NAME];
        char values[MAX_FIELDS][MAX_FIELD_VALUE];
        int  n = 0;
        while (n < MAX_FIELDS) {
            char tmp[MAX_FIELD_VALUE];
            int len = read_csv_field(line, &pos, tmp, sizeof(tmp));
            if (len == 0 && line[pos] == 0) break;
            if (has_tags) {
                const char *src = header[n + 1 < header_count ? n + 1 : 0];
                snprintf(names[n], MAX_FIELD_NAME, "%.47s", src);
            } else {
                snprintf(names[n], MAX_FIELD_NAME, "%s",
                         (n == 0) ? "Front" : (n == 1) ? "Back" : "Field");
            }
            snprintf(values[n], MAX_FIELD_VALUE, "%s", tmp);
            n++;
            if (line[pos] == 0) break;
        }

        if (n > 0 && values[0][0]) {
            const char *value_ptrs[MAX_FIELDS];
            for (int k = 0; k < n; k++) value_ptrs[k] = values[k];
            deck_add_card_fields(d, names, value_ptrs, n, tags);
            added++;
        }
    }
    fclose(f);
    return added;
}
