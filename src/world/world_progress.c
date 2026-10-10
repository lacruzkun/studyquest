#include "world_progress.h"
#include <string.h>

_Static_assert(WORLD_MAX_CREDITED >= WORLD_DAILY_REVIEW_CAP,
               "credited-card list must hold a full day of credited reviews");
_Static_assert(WORLD_LEVEL_COUNT <= WORLD_CLAIM_BYTES * 8,
               "claim bitset too small for the level count");

#define CREDIT_MAX (INT64_MAX / 4)

/* ------------------------------------------------------------------ */
/*  small helpers                                                      */
/* ------------------------------------------------------------------ */

static void claim_set(WorldProgress *wp, int level) {
    int i = level - 1;
    if (i < 0 || i >= WORLD_CLAIM_BYTES * 8) return;
    wp->claimed[i >> 3] |= (uint8_t)(1u << (i & 7));
}

bool world_is_claimed(const WorldProgress *wp, int level) {
    int i = level - 1;
    if (!wp || i < 0 || i >= WORLD_CLAIM_BYTES * 8) return false;
    return (wp->claimed[i >> 3] >> (i & 7)) & 1u;
}

static void push_event(WorldProgress *wp, int type, int level, int rank) {
    if (wp->event_count >= WORLD_MAX_EVENTS) {          /* drop the oldest */
        memmove(&wp->events[0], &wp->events[1],
                sizeof(WorldEvent) * (WORLD_MAX_EVENTS - 1));
        wp->event_count = WORLD_MAX_EVENTS - 1;
    }
    WorldEvent *e = &wp->events[wp->event_count++];
    e->type = type; e->level = level; e->rank = rank;
}

static void reset_daily(WorldProgress *wp, int64_t day) {
    wp->day = day;
    wp->credited_reviews_today = 0;
    wp->new_cards_today = 0;
    wp->credited_count = 0;
}

/* ------------------------------------------------------------------ */
/*  lifecycle                                                          */
/* ------------------------------------------------------------------ */

void world_init(WorldProgress *wp) {
    memset(wp, 0, sizeof(*wp));
    wp->world_id = WORLD_ID_FOUNDATIONS;
    wp->shown_level = 1;
}

void world_init_from_legacy(WorldProgress *wp,
                            int64_t total_easy, int64_t total_good,
                            int64_t total_reviews, int64_t total_correct) {
    world_init(wp);
    if (total_easy < 0) total_easy = 0;
    int64_t good_only = total_good - total_easy;   if (good_only < 0) good_only = 0;
    int64_t other     = total_reviews - total_correct; if (other < 0) other = 0;

    int64_t credit = 15 * total_easy + 10 * good_only + other;
    if (credit > CREDIT_MAX) credit = CREDIT_MAX;
    wp->credit_total = credit;

    int done = world_levels_from_credit(wp->world_id, credit);
    wp->levels_completed = done;
    for (int l = 1; l <= done; l++) {
        claim_set(wp, l);                                  /* no retroactive payout */
        const LevelDef *lv = world_level(wp->world_id, l);
        if (lv && lv->reward.cosmetic) wp->cosmetics |= (1ull << lv->reward.cosmetic);
    }
    wp->shown_level = world_current_level(wp);
}

/* ------------------------------------------------------------------ */
/*  queries                                                            */
/* ------------------------------------------------------------------ */

int world_current_level(const WorldProgress *wp) {
    int n = world_level_count(wp->world_id);
    int c = wp->levels_completed + 1;
    return c > n ? n : c;
}

int world_current_rank(const WorldProgress *wp) {
    int r = world_rank_of_level(wp->world_id, world_current_level(wp));
    return r < 0 ? 0 : r;
}

bool world_is_complete(const WorldProgress *wp) {
    return wp->levels_completed >= world_level_count(wp->world_id);
}

WorldLevelState world_level_state(const WorldProgress *wp, int level) {
    if (level <= wp->levels_completed) return WLS_COMPLETED;
    if (level == wp->levels_completed + 1) return WLS_CURRENT;
    if (level == wp->levels_completed + 2) return WLS_AVAILABLE;
    return WLS_LOCKED;
}

float world_level_progress(const WorldProgress *wp, int64_t *have, int64_t *need) {
    int n = world_level_count(wp->world_id);
    if (wp->levels_completed >= n) {
        if (have) *have = 1;
        if (need) *need = 1;
        return 1.f;
    }
    const LevelDef *lv = world_level(wp->world_id, wp->levels_completed + 1);
    int64_t base = world_credit_for_levels(wp->world_id, wp->levels_completed);
    int64_t h = wp->credit_total - base;
    int64_t nd = lv ? lv->credit_cost : 1;
    if (h < 0) h = 0;
    if (h > nd) h = nd;
    if (have) *have = h;
    if (need) *need = nd;
    return nd > 0 ? (float)((double)h / (double)nd) : 0.f;
}

/* ------------------------------------------------------------------ */
/*  mutation                                                           */
/* ------------------------------------------------------------------ */

/* Complete every level the current credit covers. Rewards are granted
 * only for levels whose claim bit is still clear, and the bit is set in
 * the same step, so a level can never pay out twice. */
static void advance(WorldProgress *wp, WorldReward *out) {
    int n = world_level_count(wp->world_id);
    while (wp->levels_completed < n) {
        const LevelDef *lv = world_level(wp->world_id, wp->levels_completed + 1);
        if (!lv || wp->credit_total < lv->credit_cum) break;

        wp->levels_completed++;
        if (out) out->levels_completed++;
        push_event(wp, WEV_LEVEL_COMPLETE, lv->id, lv->rank_id);

        if (!world_is_claimed(wp, lv->id)) {
            claim_set(wp, lv->id);
            if (out) {
                out->xp    += lv->reward.xp;
                out->coins += lv->reward.coins;
            }
            if (lv->reward.cosmetic) {
                uint64_t bit = 1ull << lv->reward.cosmetic;
                if (!(wp->cosmetics & bit)) {
                    wp->cosmetics |= bit;
                    if (out) out->cosmetics |= bit;
                }
            }
        }

        if (lv->kind == NODE_RANK_GATE && lv->rank_id + 1 < WORLD_RANK_COUNT) {
            push_event(wp, WEV_RANK_UP, lv->id, lv->rank_id + 1);
            if (out) out->ranks_gained++;
        }
    }
}

int world_on_review(WorldProgress *wp, const WorldReviewEvent *ev, WorldReward *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!wp || !ev) return 0;

    /* Day rollover (any change of day number resets the daily counters). */
    if (ev->day != wp->day) reset_daily(wp, ev->day);

    /* Only reviews of cards that were genuinely due earn credit. */
    if (!ev->was_due) return 0;
    if (ev->xp <= 0)  return 0;

    if (wp->credited_reviews_today >= WORLD_DAILY_REVIEW_CAP) return 0;
    if (ev->was_new && wp->new_cards_today >= WORLD_DAILY_NEW_CAP) return 0;

    /* A card earns world credit at most once per day. */
    for (int i = 0; i < wp->credited_count; i++) {
        if (wp->credited[i].deck_id == ev->deck_id &&
            wp->credited[i].card_id == ev->card_id)
            return 0;
    }
    if (wp->credited_count >= WORLD_MAX_CREDITED) return 0;

    wp->credited[wp->credited_count].deck_id = ev->deck_id;
    wp->credited[wp->credited_count].card_id = ev->card_id;
    wp->credited_count++;
    wp->credited_reviews_today++;
    if (ev->was_new) wp->new_cards_today++;

    wp->credit_total += ev->xp;
    if (wp->credit_total > CREDIT_MAX) wp->credit_total = CREDIT_MAX;

    advance(wp, out);
    return ev->xp;
}

bool world_pop_event(WorldProgress *wp, WorldEvent *out) {
    if (!wp || wp->event_count <= 0) return false;
    if (out) *out = wp->events[0];
    wp->event_count--;
    memmove(&wp->events[0], &wp->events[1], sizeof(WorldEvent) * (size_t)wp->event_count);
    return true;
}

void world_set_shown_level(WorldProgress *wp, int level) {
    int cur = world_current_level(wp);
    if (level < 1) level = 1;
    if (level > cur) level = cur;
    wp->shown_level = level;
}

/* ------------------------------------------------------------------ */
/*  serialization — explicit little-endian, versioned, bounds-checked  */
/* ------------------------------------------------------------------ */

#define WORLD_FMT 1

typedef struct { uint8_t *p; size_t cap, len; bool ok; } Wr;
typedef struct { const uint8_t *p; size_t len, pos; bool ok; } Rd;

static void w_u8(Wr *w, uint8_t v) {
    if (w->len + 1 > w->cap) { w->ok = false; return; }
    w->p[w->len++] = v;
}
static void w_i32(Wr *w, int32_t v) {
    for (int i = 0; i < 4; i++) w_u8(w, (uint8_t)(((uint32_t)v >> (8 * i)) & 0xFF));
}
static void w_i64(Wr *w, int64_t v) {
    for (int i = 0; i < 8; i++) w_u8(w, (uint8_t)(((uint64_t)v >> (8 * i)) & 0xFF));
}

static uint8_t r_u8(Rd *r) {
    if (r->pos + 1 > r->len) { r->ok = false; return 0; }
    return r->p[r->pos++];
}
static int32_t r_i32(Rd *r) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)r_u8(r) << (8 * i);
    return (int32_t)v;
}
static int64_t r_i64(Rd *r) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r_u8(r) << (8 * i);
    return (int64_t)v;
}

size_t world_serialize(const WorldProgress *wp, uint8_t *buf, size_t cap) {
    Wr w = { buf, cap, 0, true };
    w_u8(&w, WORLD_FMT);
    w_i32(&w, wp->world_id);
    w_i64(&w, wp->credit_total);
    w_i32(&w, wp->levels_completed);
    w_i32(&w, wp->shown_level);
    w_i64(&w, (int64_t)wp->cosmetics);
    for (int i = 0; i < WORLD_CLAIM_BYTES; i++) w_u8(&w, wp->claimed[i]);
    w_i64(&w, wp->day);
    w_i32(&w, wp->credited_reviews_today);
    w_i32(&w, wp->new_cards_today);
    w_i32(&w, wp->credited_count);
    for (int i = 0; i < wp->credited_count; i++) {
        w_i32(&w, wp->credited[i].deck_id);
        w_i32(&w, wp->credited[i].card_id);
    }
    w_i32(&w, wp->event_count);
    for (int i = 0; i < wp->event_count; i++) {
        w_i32(&w, wp->events[i].type);
        w_i32(&w, wp->events[i].level);
        w_i32(&w, wp->events[i].rank);
    }
    return w.ok ? w.len : 0;
}

bool world_deserialize(WorldProgress *wp, const uint8_t *buf, size_t len) {
    if (!wp || !buf) return false;
    Rd r = { buf, len, 0, true };
    WorldProgress t;
    memset(&t, 0, sizeof(t));

    if (r_u8(&r) != WORLD_FMT) return false;
    t.world_id         = r_i32(&r);
    t.credit_total     = r_i64(&r);
    t.levels_completed = r_i32(&r);
    t.shown_level      = r_i32(&r);
    t.cosmetics        = (uint64_t)r_i64(&r);
    for (int i = 0; i < WORLD_CLAIM_BYTES; i++) t.claimed[i] = r_u8(&r);
    t.day                    = r_i64(&r);
    t.credited_reviews_today = r_i32(&r);
    t.new_cards_today        = r_i32(&r);
    t.credited_count         = r_i32(&r);
    if (!r.ok) return false;

    int n = world_level_count(t.world_id);
    if (n == 0) return false;
    if (t.credit_total < 0 || t.credit_total > CREDIT_MAX) return false;
    if (t.levels_completed < 0 || t.levels_completed > n) return false;
    if (t.shown_level < 1 || t.shown_level > n) return false;
    if (t.credited_count < 0 || t.credited_count > WORLD_MAX_CREDITED) return false;
    if (t.credited_reviews_today < 0 || t.new_cards_today < 0) return false;

    for (int i = 0; i < t.credited_count; i++) {
        t.credited[i].deck_id = r_i32(&r);
        t.credited[i].card_id = r_i32(&r);
    }
    t.event_count = r_i32(&r);
    if (!r.ok || t.event_count < 0 || t.event_count > WORLD_MAX_EVENTS) return false;
    for (int i = 0; i < t.event_count; i++) {
        t.events[i].type  = r_i32(&r);
        t.events[i].level = r_i32(&r);
        t.events[i].rank  = r_i32(&r);
        if (t.events[i].type < WEV_LEVEL_COMPLETE || t.events[i].type > WEV_RANK_UP)
            return false;
    }
    if (!r.ok || r.pos != r.len) return false;      /* no trailing garbage */

    /* Consistency: never trust a stored completion count beyond what the
     * credit supports. A lower stored value is fine; advance() catches up
     * (with proper claim handling) on the next review. */
    int derived = world_levels_from_credit(t.world_id, t.credit_total);
    if (t.levels_completed > derived) t.levels_completed = derived;
    {
        int cur = t.levels_completed + 1;
        if (cur > n) cur = n;
        if (t.shown_level > cur) t.shown_level = cur;
    }

    *wp = t;
    return true;
}
