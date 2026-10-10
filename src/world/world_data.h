#ifndef WORLD_DATA_H
#define WORLD_DATA_H

/* Static world definition: ranks, regions and levels.
 *
 * Pure data + deterministic generation. No raylib dependency, so it links
 * into the logic test executables exactly like the scheduler and player.
 *
 * Levels are GENERATED from rank/region parameters (not stored as literal
 * structs) so the world can grow from 105 levels to many more without
 * touching rendering code. Everything is keyed by world_id so further
 * worlds (JLPT N5, N4, ...) can be added later. */

#include <stdbool.h>
#include <stdint.h>

#define WORLD_ID_FOUNDATIONS  1
#define WORLD_COUNT           1

#define WORLD_RANK_COUNT      21
#define WORLD_REGION_COUNT    21
#define WORLD_LEVELS_PER_RANK 5
#define WORLD_LEVEL_COUNT     (WORLD_RANK_COUNT * WORLD_LEVELS_PER_RANK)  /* 105 */

/* Level cost tuning: cost(L) = round(WORLD_COST_BASE * WORLD_COST_GROWTH^(L-1)) */
#define WORLD_COST_BASE       100.0
#define WORLD_COST_GROWTH     1.045

/* Anti-farming limits. They only gate WORLD credit; XP, streaks, quests
 * and the scheduler are never affected. */
#define WORLD_DAILY_REVIEW_CAP 300   /* credited reviews per day            */
#define WORLD_DAILY_NEW_CAP     50   /* credited first-reviews of new cards */

typedef enum {
    NODE_STANDARD = 0,
    NODE_RANK_GATE,    /* last level of a rank; completing it enters the next rank */
    NODE_CHALLENGE,    /* reserved: optional side node (Phase 7) */
    NODE_TREASURE,     /* reserved: optional side node (Phase 7) */
    NODE_BONUS         /* reserved: optional side node (Phase 7) */
} NodeKind;

typedef enum {
    MAT_WOOD = 0, MAT_BRONZE, MAT_SILVER, MAT_GOLD,
    MAT_PLATINUM, MAT_AETHER, MAT_PRISMATIC,
    MAT_COUNT
} RankMaterial;

typedef enum {
    THEME_VILLAGE = 0, THEME_GRASSLAND, THEME_FOREST, THEME_FOOTHILLS,
    THEME_RUINS, THEME_MOUNTAIN, THEME_SNOW_VOLCANO, THEME_FORTRESS,
    THEME_FLOATING_ISLANDS, THEME_TEMPLE, THEME_CELESTIAL_ARENA, THEME_SKY,
    THEME_MYSTICAL, THEME_HEAVENLY, THEME_DIVINE, THEME_CELESTIAL_LAND,
    THEME_PLANETARY, THEME_COSMIC, THEME_GALAXY, THEME_SURREAL,
    THEME_ULTIMATE
} RegionTheme;

typedef struct {
    int      xp;
    int      coins;
    uint32_t cosmetic;     /* cosmetic id 1..63, 0 = none */
} WorldRewardDef;

typedef struct {
    const char  *name;
    int          first_level;   /* global level id of the rank's first level */
    int          level_count;
    RankMaterial material;
    int          grade;         /* 0..2 ornament level within the material family */
} RankDef;

typedef struct {
    const char *name;
    int         rank_id;
    RegionTheme theme;
} RegionDef;

typedef struct {
    int            id;          /* global, 1-based, contiguous */
    int            world_id;
    int            rank_id;     /* 0-based index into the rank table */
    int            region_id;   /* 0-based index into the region table */
    NodeKind       kind;
    bool           milestone;   /* levels 10, 25, 50, 100 */
    int64_t        credit_cost; /* credit needed to finish this level */
    int64_t        credit_cum;  /* cumulative credit at which it completes */
    WorldRewardDef reward;      /* one-time completion reward */
} LevelDef;

/* ---- accessors (tables are built once, lazily, deterministically) ---- */
int              world_level_count(int world_id);          /* 0 if unknown world */
const LevelDef  *world_level(int world_id, int level_id);  /* NULL if out of range */
const RankDef   *world_rank(int rank_id);                  /* NULL if out of range */
const RegionDef *world_region(int region_id);              /* NULL if out of range */

int  world_rank_of_level(int world_id, int level_id);      /* -1 if invalid */
int  world_rank_count(void);

/* Number of levels whose cumulative threshold is <= credit (0..level_count). */
int  world_levels_from_credit(int world_id, int64_t credit);

/* Cumulative credit needed to have completed `levels` levels (0 -> 0). */
int64_t world_credit_for_levels(int world_id, int levels);

#endif
