#include "save.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- low-level helpers ------------------------------------------- */

static bool w_u32(FILE *f, uint32_t v)  { return fwrite(&v, 4, 1, f) == 1; }
static bool w_i32(FILE *f, int32_t  v)  { return fwrite(&v, 4, 1, f) == 1; }
static bool w_i64(FILE *f, int64_t  v)  { return fwrite(&v, 8, 1, f) == 1; }
static bool w_f32(FILE *f, float    v)  { return fwrite(&v, 4, 1, f) == 1; }
static bool w_f64(FILE *f, double   v)  { return fwrite(&v, 8, 1, f) == 1; }
static bool w_bytes(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n; }
static bool w_str(FILE *f, const char *s, int max) {
    int len = (int)strlen(s);
    if (len > max - 1) len = max - 1;
    if (!w_i32(f, len)) return false;
    return w_bytes(f, s, (size_t)len);
}

static bool r_u32(FILE *f, uint32_t *v)  { return fread(v, 4, 1, f) == 1; }
static bool r_i32(FILE *f, int32_t  *v)  { return fread(v, 4, 1, f) == 1; }
static bool r_i64(FILE *f, int64_t  *v)  { return fread(v, 8, 1, f) == 1; }
static bool r_f32(FILE *f, float    *v)  { return fread(v, 4, 1, f) == 1; }
static bool r_f64(FILE *f, double   *v)  { return fread(v, 8, 1, f) == 1; }
static bool r_bytes(FILE *f, void *p, size_t n) { return fread(p, 1, n, f) == n; }
static bool r_str(FILE *f, char *s, int max) {
    int32_t len;
    if (!r_i32(f, &len)) return false;
    if (len < 0 || len >= max) return false;
    if (!r_bytes(f, s, (size_t)len)) return false;
    s[len] = 0;
    return true;
}

/* ---- deck / card / player serialization -------------------------- */

static bool write_card(FILE *f, const Card *c) {
    return w_i32(f, c->id)
        && w_str(f, c->front, MAX_TEXT)
        && w_str(f, c->back,  MAX_TEXT)
        && w_str(f, c->tags,  MAX_TAGS)
        && w_str(f, c->japanese, MAX_TEXT)
        && w_str(f, c->reading,  MAX_TEXT)
        && w_str(f, c->meaning,  MAX_TEXT)
        && w_str(f, c->example,  MAX_TEXT)
        && w_i32(f, (int32_t)c->state)
        && w_f64(f, c->interval_sec)
        && w_f32(f, c->ease)
        && w_i32(f, c->reps)
        && w_i32(f, c->lapses)
        && w_f64(f, c->due)
        && w_f64(f, c->last_review)
        && w_i32(f, c->last_rating);
}

static bool read_card(FILE *f, Card *c) {
    int32_t state, id, reps, lapses, lr;
    return r_i32(f, &id)
        && r_str(f, c->front, MAX_TEXT)
        && r_str(f, c->back,  MAX_TEXT)
        && r_str(f, c->tags,  MAX_TAGS)
        && r_str(f, c->japanese, MAX_TEXT)
        && r_str(f, c->reading,  MAX_TEXT)
        && r_str(f, c->meaning,  MAX_TEXT)
        && r_str(f, c->example,  MAX_TEXT)
        && r_i32(f, &state)
        && r_f64(f, &c->interval_sec)
        && r_f32(f, &c->ease)
        && r_i32(f, &reps)
        && r_i32(f, &lapses)
        && r_f64(f, &c->due)
        && r_f64(f, &c->last_review)
        && r_i32(f, &lr)
        && (c->id = id, c->state = (CardState)state, c->reps = reps,
            c->lapses = lapses, c->last_rating = lr, true);
}

static bool write_deck(FILE *f, const Deck *d) {
    uint32_t col = ((uint32_t)d->color.r) | ((uint32_t)d->color.g << 8)
                 | ((uint32_t)d->color.b << 16) | ((uint32_t)d->color.a << 24);
    if (!w_i32(f, d->id)) return false;
    if (!w_str(f, d->name, MAX_DECK_NAME)) return false;
    if (!w_u32(f, col)) return false;
    if (!w_i32(f, d->card_count)) return false;
    for (int i = 0; i < d->card_count; i++)
        if (!write_card(f, &d->cards[i])) return false;
    return true;
}

static bool read_deck(FILE *f, Deck *d) {
    memset(d, 0, sizeof(*d));
    int32_t id, cc; uint32_t col;
    if (!r_i32(f, &id)) return false;
    if (!r_str(f, d->name, MAX_DECK_NAME)) return false;
    if (!r_u32(f, &col)) return false;
    d->color.r = col & 0xFF;
    d->color.g = (col >> 8) & 0xFF;
    d->color.b = (col >> 16) & 0xFF;
    d->color.a = (col >> 24) & 0xFF;
    if (!r_i32(f, &cc)) return false;
    if (cc < 0 || cc > MAX_CARDS) return false;
    d->id = id;
    d->card_count = cc;
    for (int i = 0; i < cc; i++) if (!read_card(f, &d->cards[i])) return false;
    return true;
}

static bool write_player(FILE *f, const Player *p) {
    #define WP(field) w_i32(f, p->field)
    #define WPI64(field) w_i64(f, p->field)
    if (!WP(level) || !WP(xp) || !WP(coins) || !WP(streak) || !WP(longest_streak)) return false;
    if (!WPI64(last_study_day) || !WP(daily_goal) || !WP(studied_today) || !WPI64(today_day)) return false;
    if (!WPI64(total_studied) || !WPI64(total_reviews) || !WPI64(total_sessions)) return false;
    if (!WPI64(total_correct) || !WPI64(total_good) || !WPI64(total_easy)) return false;
    if (!WP(daily_peak) || !WP(session_studied) || !WP(session_good) || !WP(mature_cards)) return false;
    if (!WPI64(quests_day)) return false;
    if (!w_f32(f, p->master_vol) || !w_f32(f, p->sfx_vol) || !w_f32(f, p->music_vol)) return false;
    if (!w_i32(f, p->fullscreen ? 1 : 0)) return false;

    for (int i = 0; i < NUM_ACHIEVEMENTS; i++) {
        if (!w_i32(f, p->achievements[i].unlocked ? 1 : 0)) return false;
        if (!w_f64(f, p->achievements[i].unlocked_at)) return false;
    }
    for (int i = 0; i < NUM_QUESTS; i++) {
        const Quest *q = &p->quests[i];
        if (!w_i32(f, q->type) || !w_str(f, q->desc, 96)) return false;
        if (!w_i32(f, q->target) || !w_i32(f, q->progress) || !w_i32(f, q->complete ? 1 : 0)) return false;
        if (!w_i32(f, q->xp_reward) || !w_i32(f, q->coin_reward)) return false;
    }
    if (!w_i32(f, p->anim_intensity)) return false;
    return true;
}

static bool read_player(FILE *f, Player *p) {
    memset(p, 0, sizeof(*p));
    int32_t tmp32;
    int64_t tmp64;

    #define RP32(field) do { if (!r_i32(f, &tmp32)) return false; p->field = tmp32; } while (0)
    #define RP64(field) do { if (!r_i64(f, &tmp64)) return false; p->field = tmp64; } while (0)
    RP32(level); RP32(xp); RP32(coins); RP32(streak); RP32(longest_streak);
    RP64(last_study_day); RP32(daily_goal); RP32(studied_today); RP64(today_day);
    RP64(total_studied); RP64(total_reviews); RP64(total_sessions);
    RP64(total_correct); RP64(total_good); RP64(total_easy);
    RP32(daily_peak); RP32(session_studied); RP32(session_good); RP32(mature_cards);
    RP64(quests_day);
    if (!r_f32(f, &p->master_vol)) return false;
    if (!r_f32(f, &p->sfx_vol)) return false;
    if (!r_f32(f, &p->music_vol)) return false;
    if (!r_i32(f, &tmp32)) return false;
    p->fullscreen = tmp32 != 0;

    for (int i = 0; i < NUM_ACHIEVEMENTS; i++) {
        if (!r_i32(f, &tmp32)) return false;
        p->achievements[i].unlocked = tmp32 != 0;
        if (!r_f64(f, &p->achievements[i].unlocked_at)) return false;
    }
    for (int i = 0; i < NUM_QUESTS; i++) {
        Quest *q = &p->quests[i];
        if (!r_i32(f, &tmp32)) return false; q->type = (QuestType)tmp32;
        if (!r_str(f, q->desc, 96)) return false;
        if (!r_i32(f, &tmp32)) return false; q->target = tmp32;
        if (!r_i32(f, &tmp32)) return false; q->progress = tmp32;
        if (!r_i32(f, &tmp32)) return false; q->complete = tmp32 != 0;
        if (!r_i32(f, &tmp32)) return false; q->xp_reward = tmp32;
        if (!r_i32(f, &tmp32)) return false; q->coin_reward = tmp32;
    }
        /* Backward-compatible read: older saves won't have this field. */
    {
        int32_t ai = 0;
        if (r_i32(f, &ai)) p->anim_intensity = ai;
        else               p->anim_intensity = 0;   /* FULL */
    }
    return true;
    #undef RP32
    #undef RP64
}

/* ---- public ------------------------------------------------------- */

bool save_write(const SaveData *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = true;
    ok &= w_u32(f, SAVE_MAGIC);
    ok &= w_u32(f, SAVE_VERSION);
    ok &= w_i32(f, d->decks.count);
    ok &= w_i32(f, d->decks.next_id);
    for (int i = 0; ok && i < d->decks.count; i++) ok &= write_deck(f, &d->decks.decks[i]);
    ok &= write_player(f, &d->player);
    fclose(f);
    return ok;
}

bool save_load(SaveData *out, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    uint32_t magic, ver;
    if (!r_u32(f, &magic) || magic != SAVE_MAGIC) { fclose(f); return false; }
    if (!r_u32(f, &ver) || ver != SAVE_VERSION)  { fclose(f); return false; }

    /* Deserialize straight into out->decks — no 2.5 MB intermediate copy. */
    int32_t count, next_id;
    if (!r_i32(f, &count) || count < 0 || count > MAX_DECKS) { fclose(f); return false; }
    if (!r_i32(f, &next_id)) { fclose(f); return false; }

    memset(&out->decks, 0, sizeof(out->decks));
    out->decks.count = count;
    out->decks.next_id = next_id;

    for (int i = 0; i < count; i++) {
        if (!read_deck(f, &out->decks.decks[i])) { fclose(f); return false; }
    }

    Player p;
    if (!read_player(f, &p)) { fclose(f); return false; }

    fclose(f);
    out->player = p;
    return true;
}

void save_defaults(SaveData *out) {
    decklist_init_sample(&out->decks);
    player_init(&out->player);
    player_roll_daily_quests(&out->player, 0xC0FFEE);
}

void save_default_path(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    snprintf(out, cap, "%s/.studyquest.sav", home);
}
