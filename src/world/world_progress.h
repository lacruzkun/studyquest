#ifndef WORLD_PROGRESS_H
#define WORLD_PROGRESS_H

/* World progression state and rules. PURE LOGIC: no raylib, no I/O.
 *
 * World credit is the XP value that StudyQuest's existing reward table
 * already assigns to each review (1/5/10/15). It is an accumulator of that
 * same XP, not a second currency, and nothing can spend it. Level
 * completion is DERIVED from credit_total, so there is no way to advance
 * by pressing a button.
 *
 * Reward XP/coins are granted by the caller through the existing Player
 * API using the WorldReward returned here. Reward XP never feeds back into
 * world credit (credit only comes from world_on_review). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "world_data.h"

#define WORLD_CLAIM_BYTES   64     /* 512 claim bits: level id - 1 */
#define WORLD_MAX_CREDITED  512    /* must be >= WORLD_DAILY_REVIEW_CAP */
#define WORLD_MAX_EVENTS    16
#define WORLD_SERIAL_MAX    8192   /* upper bound of a serialized block */

typedef struct { int32_t deck_id; int32_t card_id; } WorldCardKey;

typedef enum {
    WEV_NONE = 0,
    WEV_LEVEL_COMPLETE = 1,
    WEV_RANK_UP = 2          /* player entered a new rank (event.rank) */
} WorldEventType;

typedef struct {
    int32_t type;
    int32_t level;           /* level that triggered the event */
    int32_t rank;            /* rank entered (WEV_RANK_UP) or rank of level */
} WorldEvent;

typedef struct WorldProgress {
    int32_t  world_id;
    int64_t  credit_total;
    int32_t  levels_completed;      /* 0..level_count */
    int32_t  shown_level;           /* level the character visibly stands on (1-based) */
    uint64_t cosmetics;             /* bit n set = cosmetic id n unlocked */
    uint8_t  claimed[WORLD_CLAIM_BYTES];

    /* Daily anti-farm bookkeeping */
    int64_t      day;
    int32_t      credited_reviews_today;
    int32_t      new_cards_today;
    int32_t      credited_count;
    WorldCardKey credited[WORLD_MAX_CREDITED];

    /* Unseen progression events (for animations / banners) */
    int32_t    event_count;
    WorldEvent events[WORLD_MAX_EVENTS];
} WorldProgress;

typedef struct {
    int     deck_id;
    int     card_id;
    int     rating;       /* 0..3, informational */
    bool    was_new;      /* card state was NEW before this review */
    bool    was_due;      /* card was due BEFORE scheduler_apply */
    int     xp;           /* review XP from the existing reward table */
    int64_t day;          /* current day number (Player.today_day) */
} WorldReviewEvent;

typedef struct {
    int      xp;
    int      coins;
    uint64_t cosmetics;       /* newly unlocked cosmetic bits */
    int      levels_completed; /* levels completed by this review */
    int      ranks_gained;
} WorldReward;

typedef enum {
    WLS_COMPLETED = 0,
    WLS_CURRENT,      /* character stands here; progress ring fills */
    WLS_AVAILABLE,    /* lit, next in line */
    WLS_LOCKED
} WorldLevelState;

/* ---- lifecycle ------------------------------------------------------ */
void world_init(WorldProgress *wp);

/* Migration from pre-world saves. Credit is reconstructed from lifetime
 * counters as 15*easy + 10*(good-only) + 1*(again/hard), a deliberate lower
 * bound (Again and Hard cannot be told apart in old saves). Everything the
 * result already completes is marked claimed (NO retroactive XP/coins);
 * cosmetics for completed milestones are unlocked. No events are queued
 * and shown_level is set to the current level. */
void world_init_from_legacy(WorldProgress *wp,
                            int64_t total_easy, int64_t total_good,
                            int64_t total_reviews, int64_t total_correct);

/* ---- the one mutation entry point ----------------------------------- */
/* Returns the credit awarded (0 when the review is ineligible). `out` may
 * be NULL. Caller grants out->xp / out->coins through the Player API. */
int world_on_review(WorldProgress *wp, const WorldReviewEvent *ev, WorldReward *out);

/* ---- queries -------------------------------------------------------- */
int   world_current_level(const WorldProgress *wp);   /* min(done+1, count) */
int   world_current_rank(const WorldProgress *wp);
bool  world_is_complete(const WorldProgress *wp);
WorldLevelState world_level_state(const WorldProgress *wp, int level);
/* Progress toward the current level. Returns fraction 0..1. */
float world_level_progress(const WorldProgress *wp, int64_t *have, int64_t *need);
bool  world_is_claimed(const WorldProgress *wp, int level);

/* ---- events / animation bookkeeping --------------------------------- */
bool  world_pop_event(WorldProgress *wp, WorldEvent *out);   /* oldest first */
void  world_set_shown_level(WorldProgress *wp, int level);   /* clamped to current */

/* ---- persistence helpers (little-endian, versioned, bounds-checked) -- */
size_t world_serialize(const WorldProgress *wp, uint8_t *buf, size_t cap);
bool   world_deserialize(WorldProgress *wp, const uint8_t *buf, size_t len);

#endif
