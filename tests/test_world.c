/* World progression tests: data model, rules, anti-farm, persistence and
 * migration from pre-world (v8) saves. No raylib window is needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "world/world_data.h"
#include "world/world_progress.h"
#include "storage/save.h"
#include "player/player.h"
#include "cards/cards.h"

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); exit(1); } \
    else printf("ok: %s\n", #cond); \
} while (0)

static const char *EXPECTED_RANKS[21] = {
    "Noob", "Rookie", "Novice", "Apprentice", "Fighter", "Veteran", "Elite",
    "Expert", "Master", "Grandmaster", "Champion", "Legend", "Mythic",
    "Ascendant", "Divine", "Demigod", "God", "Supreme God", "Cosmic",
    "Infinite", "Absolute"
};

/* One review through the same entry point study.c uses. */
static int rv(WorldProgress *wp, int deck, int card, bool was_new, bool was_due,
              int xp, int64_t day, WorldReward *out) {
    WorldReviewEvent e = { .deck_id = deck, .card_id = card, .rating = 2,
                           .was_new = was_new, .was_due = was_due, .xp = xp, .day = day };
    return world_on_review(wp, &e, out);
}

static bool same_world(const WorldProgress *a, const WorldProgress *b) {
    if (a->world_id != b->world_id || a->credit_total != b->credit_total ||
        a->levels_completed != b->levels_completed || a->shown_level != b->shown_level ||
        a->cosmetics != b->cosmetics || a->day != b->day ||
        a->credited_reviews_today != b->credited_reviews_today ||
        a->new_cards_today != b->new_cards_today ||
        a->credited_count != b->credited_count || a->event_count != b->event_count)
        return false;
    if (memcmp(a->claimed, b->claimed, sizeof(a->claimed)) != 0) return false;
    for (int i = 0; i < a->credited_count; i++)
        if (a->credited[i].deck_id != b->credited[i].deck_id ||
            a->credited[i].card_id != b->credited[i].card_id) return false;
    for (int i = 0; i < a->event_count; i++)
        if (a->events[i].type != b->events[i].type || a->events[i].level != b->events[i].level ||
            a->events[i].rank != b->events[i].rank) return false;
    return true;
}

/* ------------------------------------------------------------------ */
static void test_ranks(void) {
    CHECK(world_rank_count() == 21);
    for (int r = 0; r < 21; r++) {
        const RankDef *rk = world_rank(r);
        CHECK(rk != NULL);
        CHECK(strcmp(rk->name, EXPECTED_RANKS[r]) == 0);
        CHECK(rk->first_level == r * WORLD_LEVELS_PER_RANK + 1);
        CHECK(rk->level_count == WORLD_LEVELS_PER_RANK);
        const RegionDef *rg = world_region(r);
        CHECK(rg != NULL && rg->rank_id == r);
    }
    CHECK(world_rank(-1) == NULL);
    CHECK(world_rank(21) == NULL);
    CHECK(world_region(21) == NULL);
    /* 7 material families x 3 grades */
    CHECK(world_rank(0)->material == MAT_WOOD && world_rank(2)->material == MAT_WOOD);
    CHECK(world_rank(3)->material == MAT_BRONZE);
    CHECK(world_rank(20)->material == MAT_PRISMATIC && world_rank(20)->grade == 2);
}

static void test_levels(void) {
    CHECK(world_level_count(WORLD_ID_FOUNDATIONS) == 105);
    CHECK(world_level_count(99) == 0);
    CHECK(world_level(WORLD_ID_FOUNDATIONS, 0) == NULL);
    CHECK(world_level(WORLD_ID_FOUNDATIONS, 106) == NULL);
    CHECK(world_level(99, 1) == NULL);

    int64_t prev = 0;
    for (int i = 1; i <= 105; i++) {
        const LevelDef *lv = world_level(WORLD_ID_FOUNDATIONS, i);
        CHECK(lv != NULL && lv->id == i);
        CHECK(lv->rank_id == (i - 1) / 5 && lv->region_id == lv->rank_id);
        CHECK(lv->credit_cost > 0);
        CHECK(lv->credit_cum == prev + lv->credit_cost);
        prev = lv->credit_cum;
        CHECK((lv->kind == NODE_RANK_GATE) == (i % 5 == 0));
        CHECK(lv->milestone == (i == 10 || i == 25 || i == 50 || i == 100));
    }
    /* independently computed (python) reference values */
    CHECK(world_level(1, 1)->credit_cost == 100);
    CHECK(world_level(1, 2)->credit_cost == 105);
    CHECK(world_level(1, 50)->credit_cost == 864);
    CHECK(world_level(1, 100)->credit_cost == 7808);
    CHECK(world_level(1, 105)->credit_cost == 9730);
    CHECK(world_credit_for_levels(1, 5) == 547);
    CHECK(world_credit_for_levels(1, 10) == 1229);
    CHECK(world_credit_for_levels(1, 25) == 4456);
    CHECK(world_credit_for_levels(1, 50) == 17852);
    CHECK(world_credit_for_levels(1, 100) == 179089);
    CHECK(world_credit_for_levels(1, 105) == 223725);
    CHECK(world_credit_for_levels(1, 0) == 0);
    CHECK(world_credit_for_levels(1, 999) == 223725);
    CHECK(world_rank_of_level(1, 5) == 0 && world_rank_of_level(1, 6) == 1);
    CHECK(world_rank_of_level(1, 105) == 20 && world_rank_of_level(1, 0) == -1);
}

static void test_credit_math(void) {
    CHECK(world_levels_from_credit(1, 0) == 0);
    CHECK(world_levels_from_credit(1, -5) == 0);
    CHECK(world_levels_from_credit(1, 99) == 0);
    CHECK(world_levels_from_credit(1, 100) == 1);
    CHECK(world_levels_from_credit(1, 204) == 1);
    CHECK(world_levels_from_credit(1, 205) == 2);
    CHECK(world_levels_from_credit(1, 546) == 4);
    CHECK(world_levels_from_credit(1, 547) == 5);
    CHECK(world_levels_from_credit(1, 223724) == 104);
    CHECK(world_levels_from_credit(1, 223725) == 105);
    CHECK(world_levels_from_credit(1, INT64_MAX / 8) == 105);
    CHECK(world_levels_from_credit(99, 1000000) == 0);
}

static void test_review_credit_and_completion(void) {
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    CHECK(world_current_level(&wp) == 1 && world_current_rank(&wp) == 0);
    CHECK(wp.shown_level == 1 && !world_is_complete(&wp));

    /* A card that was not due earns nothing. */
    CHECK(rv(&wp, 1, 1, false, false, 10, 100, &r) == 0);
    CHECK(wp.credit_total == 0);
    /* Zero-XP review earns nothing. */
    CHECK(rv(&wp, 1, 1, false, true, 0, 100, &r) == 0);

    /* Ten distinct 10-XP reviews complete level 1 (cost 100). */
    for (int c = 1; c <= 9; c++) CHECK(rv(&wp, 1, c, false, true, 10, 100, &r) == 10);
    CHECK(wp.levels_completed == 0 && r.levels_completed == 0);
    int64_t have, need;
    float f = world_level_progress(&wp, &have, &need);
    CHECK(have == 90 && need == 100 && f > 0.89f && f < 0.91f);
    CHECK(rv(&wp, 1, 10, false, true, 10, 100, &r) == 10);
    CHECK(wp.levels_completed == 1 && r.levels_completed == 1);
    CHECK(r.xp == 0 && r.coins == 10 && r.ranks_gained == 0);
    CHECK(world_current_level(&wp) == 2);
    CHECK(world_is_claimed(&wp, 1) && !world_is_claimed(&wp, 2));
    WorldEvent ev;
    CHECK(world_pop_event(&wp, &ev) && ev.type == WEV_LEVEL_COMPLETE && ev.level == 1);
    CHECK(!world_pop_event(&wp, &ev));

    /* The same card cannot earn world credit twice in a day. */
    int64_t before = wp.credit_total;
    CHECK(rv(&wp, 1, 3, false, true, 15, 100, &r) == 0);
    CHECK(wp.credit_total == before);
    /* Same card id in a different deck is a different card. */
    CHECK(rv(&wp, 2, 3, false, true, 15, 100, &r) == 15);
    /* Next day the same card may earn credit again (genuinely due again). */
    CHECK(rv(&wp, 1, 3, false, true, 15, 101, &r) == 15);
    CHECK(wp.credited_count == 1 && wp.credited_reviews_today == 1);
}

static void test_anti_farm_caps(void) {
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    int credited = 0;
    for (int c = 1; c <= 400; c++)
        if (rv(&wp, 1, c, false, true, 1, 7, &r)) credited++;
    CHECK(credited == WORLD_DAILY_REVIEW_CAP);
    CHECK(wp.credit_total == WORLD_DAILY_REVIEW_CAP);
    CHECK(wp.credited_count == WORLD_DAILY_REVIEW_CAP);
    CHECK(wp.credited_count <= WORLD_MAX_CREDITED);
    /* New day resets the cap. */
    CHECK(rv(&wp, 1, 1, false, true, 1, 8, &r) == 1);
    CHECK(wp.credited_reviews_today == 1);

    /* New-card cap: only 50 first-reviews per day earn credit. */
    WorldProgress w2; world_init(&w2);
    int nc = 0;
    for (int c = 1; c <= 80; c++)
        if (rv(&w2, 1, c, true, true, 10, 5, &r)) nc++;
    CHECK(nc == WORLD_DAILY_NEW_CAP);
    CHECK(w2.new_cards_today == WORLD_DAILY_NEW_CAP);
    /* Non-new cards are not limited by the new-card cap. */
    CHECK(rv(&w2, 1, 1000, false, true, 10, 5, &r) == 10);
    /* A farmed day cannot produce more than cap * max XP of credit. */
    WorldProgress w3; world_init(&w3);
    for (int c = 1; c <= 5000; c++) rv(&w3, 1, c, false, true, 15, 9, &r);
    CHECK(w3.credit_total == (int64_t)WORLD_DAILY_REVIEW_CAP * 15);
}

static void test_rank_up_and_rewards(void) {
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    /* One big review covering levels 1..5 (cum 547). */
    CHECK(rv(&wp, 1, 1, false, true, 547, 1, &r) == 547);
    CHECK(wp.levels_completed == 5 && r.levels_completed == 5 && r.ranks_gained == 1);
    CHECK(r.coins == 4 * 10 + 40);      /* four standard levels + rank-0 gate */
    CHECK(r.xp == 100);                 /* rank-0 gate */
    CHECK(world_current_rank(&wp) == 1 && world_current_level(&wp) == 6);
    CHECK(strcmp(world_rank(world_current_rank(&wp))->name, "Rookie") == 0);
    WorldEvent ev;
    for (int l = 1; l <= 5; l++) {
        CHECK(world_pop_event(&wp, &ev));
        CHECK(ev.type == WEV_LEVEL_COMPLETE && ev.level == l);
    }
    CHECK(world_pop_event(&wp, &ev) && ev.type == WEV_RANK_UP && ev.rank == 1);
    CHECK(!world_pop_event(&wp, &ev));

    /* Milestone: level 10 adds a cosmetic and bonus XP/coins. */
    WorldProgress w2; world_init(&w2);
    CHECK(rv(&w2, 1, 1, false, true, 1229, 1, &r) == 1229);
    CHECK(w2.levels_completed == 10 && r.ranks_gained == 2);
    CHECK(r.xp == 100 + (125 + 300));
    CHECK(r.coins == 8 * 10 + 40 + (50 + 100));
    CHECK((r.cosmetics & (1ull << 1)) != 0 && (w2.cosmetics & (1ull << 1)) != 0);

    /* Level 25 and 50 and 100 milestones give their own cosmetics. */
    WorldProgress w3; world_init(&w3);
    rv(&w3, 1, 1, false, true, 179089, 1, &r);
    CHECK(w3.levels_completed == 100);
    CHECK((w3.cosmetics & ((1ull<<1)|(1ull<<2)|(1ull<<3)|(1ull<<4))) ==
          ((1ull<<1)|(1ull<<2)|(1ull<<3)|(1ull<<4)));
}

static void test_no_double_reward_and_final_level(void) {
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    rv(&wp, 1, 1, false, true, 547, 1, &r);
    CHECK(r.xp == 100);

    /* Simulate lost/lowered completion state (e.g. loaded block). The catch-up
     * must not pay the already-claimed levels again. */
    wp.levels_completed = 0;
    CHECK(rv(&wp, 1, 2, false, true, 1, 2, &r) == 1);
    CHECK(wp.levels_completed == 5);
    CHECK(r.xp == 0 && r.coins == 0 && r.cosmetics == 0);

    /* Completing everything: no rank-up past the last rank, state is complete. */
    WorldProgress w2; world_init(&w2);
    rv(&w2, 1, 1, false, true, 223725, 1, &r);
    CHECK(world_is_complete(&w2) && w2.levels_completed == 105);
    CHECK(r.ranks_gained == 20);
    CHECK(world_current_level(&w2) == 105 && world_current_rank(&w2) == 20);
    int64_t have, need;
    CHECK(world_level_progress(&w2, &have, &need) == 1.0f);
    CHECK(world_level_state(&w2, 105) == WLS_COMPLETED);
    /* Event queue is bounded and keeps the NEWEST events. */
    CHECK(w2.event_count == WORLD_MAX_EVENTS);
    WorldEvent ev, last = {0};
    int popped = 0;
    while (world_pop_event(&w2, &ev)) { last = ev; popped++; }
    CHECK(popped == WORLD_MAX_EVENTS);
    CHECK(last.type == WEV_LEVEL_COMPLETE && last.level == 105);
    /* Further credit after completion is harmless. */
    CHECK(rv(&w2, 1, 2, false, true, 10, 1, &r) == 10);
    CHECK(r.xp == 0 && r.coins == 0 && w2.levels_completed == 105);
}

static void test_states_and_shown_level(void) {
    WorldProgress wp; world_init(&wp);
    wp.credit_total = 547 - 1;                       /* not enough for level 5 */
    wp.levels_completed = 3;
    CHECK(world_level_state(&wp, 1) == WLS_COMPLETED);
    CHECK(world_level_state(&wp, 3) == WLS_COMPLETED);
    CHECK(world_level_state(&wp, 4) == WLS_CURRENT);
    CHECK(world_level_state(&wp, 5) == WLS_AVAILABLE);
    CHECK(world_level_state(&wp, 6) == WLS_LOCKED);
    CHECK(world_level_state(&wp, 105) == WLS_LOCKED);

    world_set_shown_level(&wp, 99);  CHECK(wp.shown_level == 4);
    world_set_shown_level(&wp, 0);   CHECK(wp.shown_level == 1);
    world_set_shown_level(&wp, 3);   CHECK(wp.shown_level == 3);

    WorldProgress w2; world_init(&w2);
    WorldReward r;
    rv(&w2, 1, 1, false, true, 150, 1, &r);        /* level 1 done, 50 into level 2 */
    int64_t have, need;
    float f = world_level_progress(&w2, &have, &need);
    CHECK(have == 50 && need == 105 && f > 0.47f && f < 0.48f);
    CHECK(w2.shown_level == 1);                    /* character has not travelled yet */
    CHECK(world_current_level(&w2) == 2);
}

static void test_serialization(void) {
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    for (int c = 1; c <= 40; c++) rv(&wp, 1 + c % 3, c, c % 2 == 0, true, 10, 55, &r);
    rv(&wp, 1, 500, false, true, 600, 55, &r);
    wp.shown_level = 2;
    CHECK(wp.event_count > 0 && wp.credited_count > 0 && wp.cosmetics == 0);

    uint8_t buf[WORLD_SERIAL_MAX];
    size_t n = world_serialize(&wp, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    WorldProgress back; memset(&back, 0, sizeof(back));
    CHECK(world_deserialize(&back, buf, n));
    CHECK(same_world(&wp, &back));

    /* too-small buffer is rejected, not truncated */
    CHECK(world_serialize(&wp, buf, 10) == 0);
    /* every truncation fails */
    bool all_fail = true;
    for (size_t k = 0; k < n; k++) {
        WorldProgress t; if (world_deserialize(&t, buf, k)) { all_fail = false; break; }
    }
    CHECK(all_fail);
    /* trailing garbage fails */
    uint8_t big[WORLD_SERIAL_MAX]; memcpy(big, buf, n); big[n] = 0;
    WorldProgress t;
    CHECK(!world_deserialize(&t, big, n + 1));
    /* bad format byte */
    memcpy(big, buf, n); big[0] = 99;
    CHECK(!world_deserialize(&t, big, n));
    /* bad world id */
    memcpy(big, buf, n); big[1] = 9;
    CHECK(!world_deserialize(&t, big, n));
    /* negative credit */
    memcpy(big, buf, n); big[5 + 7] = 0x80;          /* sign bit of credit_total */
    CHECK(!world_deserialize(&t, big, n));

    /* completion count above what credit supports is clamped on load */
    WorldProgress lie = wp; lie.levels_completed = 90;
    n = world_serialize(&lie, buf, sizeof(buf));
    CHECK(world_deserialize(&t, buf, n));
    CHECK(t.levels_completed == world_levels_from_credit(1, wp.credit_total));
    CHECK(t.shown_level <= world_current_level(&t));
}

static void test_legacy_migration_math(void) {
    WorldProgress wp;
    world_init_from_legacy(&wp, 120, 300, 500, 380);
    CHECK(wp.credit_total == 15 * 120 + 10 * 180 + 120);   /* 3720 */
    CHECK(wp.levels_completed == 22);
    for (int l = 1; l <= 22; l++) CHECK(world_is_claimed(&wp, l));
    CHECK(!world_is_claimed(&wp, 23));
    CHECK(wp.event_count == 0);
    CHECK(wp.shown_level == world_current_level(&wp) && wp.shown_level == 23);
    CHECK((wp.cosmetics & (1ull << 1)) != 0);              /* level 10 reached */
    CHECK((wp.cosmetics & (1ull << 2)) == 0);              /* level 25 not reached */

    /* No retroactive payout: the very next completed level pays only itself. */
    WorldReward r;
    rv(&wp, 1, 1, false, true, 3893 - 3720, 1, &r);        /* exactly completes level 23 */
    CHECK(wp.levels_completed == 23 && r.coins == 10 && r.xp == 0);

    /* Defensive: nonsense counters never produce negative credit. */
    WorldProgress w2;
    world_init_from_legacy(&w2, -5, 3, 2, 9);
    CHECK(w2.credit_total >= 0);
    world_init_from_legacy(&w2, 0, 0, 0, 0);
    CHECK(w2.credit_total == 0 && w2.levels_completed == 0 && w2.shown_level == 1);
}

/* ------------------------------------------------------------------ */
static void make_tmp(char *path, size_t cap) {
    snprintf(path, cap, "/tmp/sq_world_test_XXXXXX");
    int fd = mkstemp(path);
    if (fd >= 0) close(fd);
}

static long file_size(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fclose(f); return n;
}

static bool copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"); if (!in) return false;
    FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return false; }
    char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, in)) > 0) fwrite(b, 1, n, out);
    fclose(in); fclose(out); return true;
}

static SaveData sd_a, sd_b;

static void test_save_roundtrip(void) {
    char path[128]; make_tmp(path, sizeof(path));
    save_defaults(&sd_a);
    CHECK(sd_a.loaded_version == 0);
    sd_a.player.total_easy = 11; sd_a.player.total_good = 30;
    WorldReward r;
    for (int c = 1; c <= 25; c++) rv(&sd_a.world, 1, c, false, true, 15, 3, &r);
    rv(&sd_a.world, 1, 99, false, true, 700, 3, &r);
    sd_a.world.shown_level = 3;
    int64_t credit = sd_a.world.credit_total;
    int done = sd_a.world.levels_completed;
    CHECK(done >= 5);

    CHECK(save_write(&sd_a, path));
    memset(&sd_b, 0, sizeof(sd_b));
    CHECK(save_load(&sd_b, path));
    CHECK(sd_b.loaded_version == SAVE_VERSION && SAVE_VERSION == 9);
    CHECK(same_world(&sd_a.world, &sd_b.world));
    CHECK(sd_b.world.credit_total == credit && sd_b.world.levels_completed == done);
    CHECK(sd_b.world.shown_level == 3);
    CHECK(sd_b.decks.count == sd_a.decks.count);
    CHECK(sd_b.player.total_easy == 11);

    /* Restart must not re-pay: same day, same cards earn nothing; claims survive. */
    WorldReward r2;
    CHECK(rv(&sd_b.world, 1, 1, false, true, 15, 3, &r2) == 0);
    CHECK(r2.xp == 0 && r2.coins == 0);
    for (int l = 1; l <= done; l++) CHECK(world_is_claimed(&sd_b.world, l));
    /* Queued events survived the restart exactly once. */
    CHECK(sd_b.world.event_count == sd_a.world.event_count);

    /* Re-saving is stable. */
    CHECK(save_write(&sd_b, path));
    memset(&sd_a, 0, sizeof(sd_a));
    CHECK(save_load(&sd_a, path));
    CHECK(same_world(&sd_a.world, &sd_b.world));
    remove(path);
}

static void test_v8_migration(const char *fixture) {
    char path[128]; make_tmp(path, sizeof(path));
    CHECK(copy_file(fixture, path));
    memset(&sd_a, 0, sizeof(sd_a));
    CHECK(save_load(&sd_a, path));
    CHECK(sd_a.loaded_version == 8);

    /* existing profile untouched */
    CHECK(sd_a.player.level == 3 && sd_a.player.xp == 40 && sd_a.player.coins == 77);
    CHECK(sd_a.player.streak == 4 && sd_a.player.longest_streak == 9);
    CHECK(sd_a.player.total_reviews == 500 && sd_a.player.total_easy == 120);
    CHECK(sd_a.decks.count == 2);
    CHECK(sd_a.decks.decks[0].cards[0].state == CARD_REVIEW);
    CHECK(sd_a.decks.decks[0].cards[2].reps == 4);

    /* world derived from lifetime counters, no payout, no animations */
    CHECK(sd_a.world.credit_total == 3720);
    CHECK(sd_a.world.levels_completed == 22);
    CHECK(sd_a.world.event_count == 0);
    CHECK(sd_a.world.shown_level == 23);
    for (int l = 1; l <= 22; l++) CHECK(world_is_claimed(&sd_a.world, l));

    /* Migration persists: after saving as v9 progress is read back, NOT
     * re-derived (prove it by adding credit before the save). */
    WorldReward r;
    CHECK(rv(&sd_a.world, 1, 1, false, true, 15, 10, &r) == 15);
    CHECK(save_write(&sd_a, path));
    memset(&sd_b, 0, sizeof(sd_b));
    CHECK(save_load(&sd_b, path));
    CHECK(sd_b.loaded_version == 9);
    CHECK(sd_b.world.credit_total == 3735);
    CHECK(same_world(&sd_a.world, &sd_b.world));
    CHECK(sd_b.player.level == 3 && sd_b.decks.count == 2);
    remove(path);
}

static void test_damaged_world_block_is_survivable(void) {
    char path[128]; make_tmp(path, sizeof(path));
    save_defaults(&sd_a);
    sd_a.player.level = 5; sd_a.player.coins = 321;
    sd_a.player.total_easy = 10; sd_a.player.total_good = 40;
    sd_a.player.total_reviews = 100; sd_a.player.total_correct = 70;
    WorldReward r;
    rv(&sd_a.world, 1, 1, false, true, 2000, 3, &r);     /* diverges from legacy math */
    CHECK(save_write(&sd_a, path));

    uint8_t buf[WORLD_SERIAL_MAX];
    size_t n = world_serialize(&sd_a.world, buf, sizeof(buf));
    long sz = file_size(path);
    CHECK(sz > (long)n + 4);

    /* (a) corrupt the world payload's format byte */
    char bad[128]; make_tmp(bad, sizeof(bad));
    CHECK(copy_file(path, bad));
    FILE *f = fopen(bad, "r+b");
    CHECK(f != NULL);
    fseek(f, sz - (long)n, SEEK_SET); fputc(0x7F, f); fclose(f);
    memset(&sd_b, 0, sizeof(sd_b));
    CHECK(save_load(&sd_b, bad));                         /* load must NOT fail */
    CHECK(sd_b.player.level == 5 && sd_b.player.coins == 321);
    CHECK(sd_b.decks.count == sd_a.decks.count);
    CHECK(sd_b.world.credit_total == 15 * 10 + 10 * 30 + 30);  /* legacy-derived */

    /* (b) file cut in the middle of the world block */
    CHECK(copy_file(path, bad));
    CHECK(truncate(bad, sz - 20) == 0);
    memset(&sd_b, 0, sizeof(sd_b));
    CHECK(save_load(&sd_b, bad));
    CHECK(sd_b.player.level == 5 && sd_b.decks.count == sd_a.decks.count);
    CHECK(sd_b.world.credit_total == 15 * 10 + 10 * 30 + 30);

    /* (c) absurd length prefix */
    CHECK(copy_file(path, bad));
    f = fopen(bad, "r+b");
    fseek(f, sz - (long)n - 4, SEEK_SET);
    uint32_t huge = 0x7FFFFFF0u; fwrite(&huge, 4, 1, f); fclose(f);
    memset(&sd_b, 0, sizeof(sd_b));
    CHECK(save_load(&sd_b, bad));
    CHECK(sd_b.player.coins == 321);
    remove(path); remove(bad);
}

static void test_reward_xp_never_feeds_credit(void) {
    /* Mirror of the study.c integration: reward XP goes to the Player only. */
    Player p; player_init(&p);
    WorldProgress wp; world_init(&wp);
    WorldReward r;
    int credit = rv(&wp, 1, 1, false, true, 547, 1, &r);
    CHECK(credit == 547 && r.xp == 100);
    int64_t before = wp.credit_total;
    player_add_xp(&p, r.xp);
    player_add_coins(&p, r.coins);
    CHECK(wp.credit_total == before);
    CHECK(p.coins >= r.coins);
}

int main(int argc, char **argv) {
    const char *fixture = argc > 1 ? argv[1] : "tests/fixtures/save_v8_legacy.sav";
    test_ranks();
    test_levels();
    test_credit_math();
    test_review_credit_and_completion();
    test_anti_farm_caps();
    test_rank_up_and_rewards();
    test_no_double_reward_and_final_level();
    test_states_and_shown_level();
    test_serialization();
    test_legacy_migration_math();
    test_save_roundtrip();
    test_v8_migration(fixture);
    test_damaged_world_block_is_survivable();
    test_reward_xp_never_feeds_credit();
    printf("\nAll world tests passed.\n");
    return 0;
}
