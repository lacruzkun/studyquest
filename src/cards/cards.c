#include "cards.h"
#include "study/scheduler.h"
#include <string.h>
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
    c->interval_sec = 0;
    c->due = (double)time(NULL) - 1.0;
    c->last_review = 0;
    c->reps = 0;
    c->lapses = 0;
    c->last_rating = -1;
    d->card_count++;
    return c;
}

Card *deck_add_card_jp(Deck *d,
                       const char *japanese, const char *reading,
                       const char *meaning,  const char *example,
                       const char *tags) {
    /* front/back are set to the JP text and meaning so the deck stays
       compatible with any code path that only knows front/back. */
    Card *c = deck_add_card(d, japanese ? japanese : "", meaning ? meaning : "", tags);
    if (!c) return NULL;
    safe_copy(c->japanese, MAX_TEXT, japanese ? japanese : "");
    safe_copy(c->reading,  MAX_TEXT, reading  ? reading  : "");
    safe_copy(c->meaning,  MAX_TEXT, meaning  ? meaning  : "");
    safe_copy(c->example,  MAX_TEXT, example  ? example  : "");
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

bool card_is_japanese(const Card *c) {
    return c && c->japanese[0] != 0;
}

/* ==================================================================== */
/*  Sample decks                                                        */
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

/* Japanese sample deck — the four the user asked for plus a few more. */
typedef struct { const char *jp, *rd, *mean, *ex, *tags; } SampleJP;

static const SampleJP SAMPLE_JP[] = {
 {"こんにちは", "こんにちは", "Hello",
  "こんにちは、田中さん。", "greetings,common"},
 {"食べる", "たべる", "To eat",
  "寿司を食べます。", "verbs,food"},
 {"学生", "がくせい", "Student",
  "私は大学の学生です。", "nouns,school"},
 {"日本語を勉強しています。", "にほんごをべんきょうしています。",
  "I am studying Japanese.",
  "毎日、日本語を勉強しています。", "phrases,study"},
 {"おはようございます", "おはようございます", "Good morning (polite)",
  "おはようございます、先生。", "greetings,common"},
 {"ありがとう", "ありがとう", "Thank you (casual)",
  "手伝ってくれてありがとう。", "greetings,common"},
 {"水", "みず", "Water",
  "水を一杯ください。", "nouns,food"},
 {"本", "ほん", "Book",
  "この本は面白いです。", "nouns,objects"},
 {"大きい", "おおきい", "Big",
  "大きい犬がいます。", "adjectives"},
 {"小さい", "ちいさい", "Small",
  "小さい猫が好きです。", "adjectives"},
};

void decklist_init_sample(DeckList *dl) {
    decklist_init(dl);

    /* ---- C Programming ---- */
    Deck *c = decklist_add(dl, "C Programming",
                           (Color){ 108, 132, 255, 255 });
    if (c) {
        int n = sizeof(SAMPLE_C) / sizeof(SAMPLE_C[0]);
        for (int i = 0; i < n; i++)
            deck_add_card(c, SAMPLE_C[i].q, SAMPLE_C[i].a, SAMPLE_C[i].tags);
    }

    /* ---- Japanese (N5 Starter) ---- */
    Deck *j = decklist_add(dl, "Japanese — N5 Starter",
                           (Color){ 240, 120, 160, 255 });
    if (j) {
        int n = sizeof(SAMPLE_JP) / sizeof(SAMPLE_JP[0]);
        for (int i = 0; i < n; i++)
            deck_add_card_jp(j,
                             SAMPLE_JP[i].jp,
                             SAMPLE_JP[i].rd,
                             SAMPLE_JP[i].mean,
                             SAMPLE_JP[i].ex,
                             SAMPLE_JP[i].tags);
    }
}

/* ==================================================================== */
/*  CSV import / export (unchanged CSV layout)                          */
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
    fprintf(f, "front,back,tags,japanese,reading,meaning,example\n");
    for (int i = 0; i < d->card_count; i++) {
        const Card *c = &d->cards[i];
        write_csv_field(f, c->front); fputc(',', f);
        write_csv_field(f, c->back);  fputc(',', f);
        write_csv_field(f, c->tags);  fputc(',', f);
        write_csv_field(f, c->japanese); fputc(',', f);
        write_csv_field(f, c->reading);  fputc(',', f);
        write_csv_field(f, c->meaning);  fputc(',', f);
        write_csv_field(f, c->example);
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
    if (!fgets(line, sizeof(line), f)) { fclose(f); return 0; }
    if (strncmp(line, "front,", 6) != 0) rewind(f);
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '\n' || line[0] == 0) continue;
        int pos = 0;
        char q[MAX_TEXT]={0}, a[MAX_TEXT]={0}, t[MAX_TAGS]={0};
        char jp[MAX_TEXT]={0}, rd[MAX_TEXT]={0}, mn[MAX_TEXT]={0}, ex[MAX_TEXT]={0};
        read_csv_field(line, &pos, q,  sizeof(q));  if (line[pos]==',') pos++;
        read_csv_field(line, &pos, a,  sizeof(a));  if (line[pos]==',') pos++;
        read_csv_field(line, &pos, t,  sizeof(t));  if (line[pos]==',') pos++;
        read_csv_field(line, &pos, jp, sizeof(jp)); if (line[pos]==',') pos++;
        read_csv_field(line, &pos, rd, sizeof(rd)); if (line[pos]==',') pos++;
        read_csv_field(line, &pos, mn, sizeof(mn)); if (line[pos]==',') pos++;
        read_csv_field(line, &pos, ex, sizeof(ex));
        if (jp[0]) {
            deck_add_card_jp(d, jp, rd, mn[0] ? mn : a, ex, t);
            added++;
        } else if (q[0]) {
            deck_add_card(d, q, a, t);
            added++;
        }
    }
    fclose(f);
    return added;
}
