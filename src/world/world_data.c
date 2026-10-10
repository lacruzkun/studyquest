#include "world_data.h"
#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Ranks — exact order is part of the spec.                           */
/* ------------------------------------------------------------------ */

static const char *const RANK_NAMES[WORLD_RANK_COUNT] = {
    "Noob", "Rookie", "Novice", "Apprentice", "Fighter", "Veteran", "Elite",
    "Expert", "Master", "Grandmaster", "Champion", "Legend", "Mythic",
    "Ascendant", "Divine", "Demigod", "God", "Supreme God", "Cosmic",
    "Infinite", "Absolute"
};

/* Region names/themes follow the suggested table, one region per rank. */
static const char *const REGION_NAMES[WORLD_REGION_COUNT] = {
    "Village & Training Grounds", "Grasslands", "Whispering Forest",
    "Mountain Foothills", "Ancient Ruins", "Great Mountains",
    "Ash & Snow Peaks", "Ancient Fortress", "Floating Islands",
    "Grand Temple", "Celestial Arena", "Sky Realm", "Mystic World",
    "Heavenly Spires", "Divine Realm", "Celestial Expanse",
    "Planetary Reach", "Cosmic Sea", "Galactic Drift", "Endless Surreal",
    "The Absolute"
};

static RankDef   g_ranks[WORLD_RANK_COUNT];
static RegionDef g_regions[WORLD_REGION_COUNT];
static LevelDef  g_levels[WORLD_LEVEL_COUNT];
static bool      g_built = false;

static int64_t round_cost(int level) {
    double v = WORLD_COST_BASE * pow(WORLD_COST_GROWTH, (double)(level - 1));
    return (int64_t)(v + 0.5);
}

static bool is_milestone_level(int level) {
    return level == 10 || level == 25 || level == 50 || level == 100;
}

static void build(void) {
    if (g_built) return;

    for (int r = 0; r < WORLD_RANK_COUNT; r++) {
        RankDef *rk = &g_ranks[r];
        rk->name        = RANK_NAMES[r];
        rk->first_level = r * WORLD_LEVELS_PER_RANK + 1;
        rk->level_count = WORLD_LEVELS_PER_RANK;
        rk->material    = (RankMaterial)(r / 3);   /* 7 families x 3 grades */
        rk->grade       = r % 3;

        RegionDef *rg = &g_regions[r];
        rg->name    = REGION_NAMES[r];
        rg->rank_id = r;
        rg->theme   = (RegionTheme)r;
    }

    int64_t cum = 0;
    for (int i = 0; i < WORLD_LEVEL_COUNT; i++) {
        LevelDef *lv = &g_levels[i];
        int id = i + 1;
        int rank = i / WORLD_LEVELS_PER_RANK;
        bool gate = (id % WORLD_LEVELS_PER_RANK) == 0;

        lv->id        = id;
        lv->world_id  = WORLD_ID_FOUNDATIONS;
        lv->rank_id   = rank;
        lv->region_id = rank;
        lv->kind      = gate ? NODE_RANK_GATE : NODE_STANDARD;
        lv->milestone = is_milestone_level(id);
        lv->credit_cost = round_cost(id);
        cum += lv->credit_cost;
        lv->credit_cum = cum;

        /* Rewards: modest everywhere, meaningful at rank gates, large at
         * the four milestones. No reward on a level the player skips past
         * is ever re-granted: claiming is tracked in WorldProgress. */
        lv->reward.xp = 0;
        lv->reward.coins = 10;
        lv->reward.cosmetic = 0;
        if (gate) {
            lv->reward.xp    = 100 + 25 * rank;
            lv->reward.coins = 40 + 10 * rank;
        }
        if (lv->milestone) {
            switch (id) {
                case 10:  lv->reward.xp += 300;  lv->reward.coins += 100; lv->reward.cosmetic = 1; break;
                case 25:  lv->reward.xp += 600;  lv->reward.coins += 200; lv->reward.cosmetic = 2; break;
                case 50:  lv->reward.xp += 1200; lv->reward.coins += 400; lv->reward.cosmetic = 3; break;
                case 100: lv->reward.xp += 3000; lv->reward.coins += 800; lv->reward.cosmetic = 4; break;
            }
        }
    }
    g_built = true;
}

/* ------------------------------------------------------------------ */

int world_level_count(int world_id) {
    return world_id == WORLD_ID_FOUNDATIONS ? WORLD_LEVEL_COUNT : 0;
}

const LevelDef *world_level(int world_id, int level_id) {
    if (world_id != WORLD_ID_FOUNDATIONS) return NULL;
    if (level_id < 1 || level_id > WORLD_LEVEL_COUNT) return NULL;
    build();
    return &g_levels[level_id - 1];
}

const RankDef *world_rank(int rank_id) {
    if (rank_id < 0 || rank_id >= WORLD_RANK_COUNT) return NULL;
    build();
    return &g_ranks[rank_id];
}

const RegionDef *world_region(int region_id) {
    if (region_id < 0 || region_id >= WORLD_REGION_COUNT) return NULL;
    build();
    return &g_regions[region_id];
}

int world_rank_count(void) { return WORLD_RANK_COUNT; }

int world_rank_of_level(int world_id, int level_id) {
    const LevelDef *lv = world_level(world_id, level_id);
    return lv ? lv->rank_id : -1;
}

int64_t world_credit_for_levels(int world_id, int levels) {
    if (levels <= 0) return 0;
    int n = world_level_count(world_id);
    if (levels > n) levels = n;
    const LevelDef *lv = world_level(world_id, levels);
    return lv ? lv->credit_cum : 0;
}

int world_levels_from_credit(int world_id, int64_t credit) {
    int n = world_level_count(world_id);
    if (n == 0 || credit <= 0) return 0;
    build();
    /* binary search over monotonic cumulative thresholds */
    int lo = 0, hi = n;                 /* answer in [lo, hi] */
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (g_levels[mid - 1].credit_cum <= credit) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}
