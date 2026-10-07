#include "cards.h"
#include "study/scheduler.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void safe_copy(char *dst, size_t cap, const char *src) {
    if (!src) { dst[0] = 0; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

void decklist_init(DeckList *dl) {
    memset(dl, 0, sizeof(*dl));
    dl->next_id = 1;
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
    for (int i = idx; i < dl->count - 1; i++) dl->decks[i] = dl->decks[i + 1];
    dl->count--;
}

/* ------------------------------------------------------------------ */
/*  Card creation                                                      */
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
                           const char (*names)[MAX_FIELD_NAME],
                           const char (*values)[MAX_FIELD_VALUE],
                           int n,
                           const char *tags) {
    if (n <= 0) return NULL;
    if (n > MAX_FIELDS) n = MAX_FIELDS;

    Card *c = deck_add_card(d, values[0], n > 1 ? values[1] : "", tags);
    if (!c) return NULL;

    for (int i = 0; i < n; i++) {
        safe_copy(c->field_names[i],  MAX_FIELD_NAME,  names[i]);
        safe_copy(c->field_values[i], MAX_FIELD_VALUE, values[i]);
    }
    c->field_count = n;
    return c;
}

void deck_remove_card(Deck *d, int idx) {
    if (idx < 0 || idx >= d->card_count) return;
    for (int i = idx; i < d->card_count - 1; i++) d->cards[i] = d->cards[i + 1];
    d->card_count--;
    for (int i = 0; i < d->card_count; i++) d->cards[i].id = i + 1;
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

/* ------------------------------------------------------------------ */
/*  Field helpers                                                      */
/* ------------------------------------------------------------------ */

void card_add_field(Card *c, const char *name, const char *value) {
    if (!c) return;
    if (c->field_count >= MAX_FIELDS) return;
    int i = c->field_count;
    safe_copy(c->field_names[i],  MAX_FIELD_NAME,  name ? name : "");
    safe_copy(c->field_values[i], MAX_FIELD_VALUE, value ? value : "");
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
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        int cp = 0, len = 1;
        if      (p[0] < 0x80)              { cp = p[0]; len = 1; }
        else if ((p[0] & 0xE0) == 0xC0) { cp = ((p[0]&0x1F)<<6)|(p[1]&0x3F); len = 2; }
        else if ((p[0] & 0xF0) == 0xE0) { cp = ((p[0]&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F); len = 3; }
        else if ((p[0] & 0xF8) == 0xF0) { cp = ((p[0]&0x07)<<18)|((p[1]&0x3F)<<12)|((p[2]&0x3F)<<6)|(p[3]&0x3F); len = 4; }
        else { p++; continue; }

        if ((cp >= 0x3040 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) ||
            (cp >= 0x3400 && cp <= 0x4DBF))
            return true;
        p += len;
    }
    return false;
}

bool card_is_japanese(const Card *c) {
    if (!c || c->field_count == 0) return false;
    return card_has_cjk(c->field_values[0]);
}

void card_set_image(Card *c, const char *filename) {
    if (!c) return;
    safe_copy(c->image_ref, sizeof(c->image_ref), filename ? filename : "");
}

void card_set_audio(Card *c, const char *filename) {
    if (!c) return;
    safe_copy(c->audio_ref, sizeof(c->audio_ref), filename ? filename : "");
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
            deck_add_card_fields(j, names, values, 4, SAMPLE_JP[i].tags);
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
            deck_add_card_fields(d, names, values, n, tags);
            added++;
        }
    }
    fclose(f);
    return added;
}
