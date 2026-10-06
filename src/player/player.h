#ifndef PLAYER_H
#define PLAYER_H

#include <stdbool.h>
#include <stdint.h>
#include "achievements.h"

#define NUM_QUESTS 3

typedef struct {
    bool unlocked;
    double unlocked_at;
} AchievementState;

typedef enum {
    QT_STUDY_CARDS = 0,
    QT_GOOD_RATINGS,
    QT_EASY_RATINGS,
    QT_TYPE_COUNT
} QuestType;

typedef struct {
    QuestType type;
    char desc[96];
    int target;
    int progress;
    bool complete;
    int xp_reward;
    int coin_reward;
} Quest;

typedef struct Player {
    int level;
    int xp;
    int coins;
    int streak;
    int longest_streak;
    int64_t last_study_day;
    int daily_goal;
    int studied_today;
    int64_t today_day;

    int64_t total_studied;
    int64_t total_reviews;
    int64_t total_sessions;
    int64_t total_correct;
    int64_t total_good;
    int64_t total_easy;

    int daily_peak;
    int mature_cards;        /* <-- ADD THIS. Recomputed after each review. */

    /* Session scratch — reset when a session starts/ends */
    int session_studied;
    int session_good;

    AchievementState achievements[NUM_ACHIEVEMENTS];
    Quest quests[NUM_QUESTS];
    int64_t quests_day;      /* day number quests were rolled for */

    float master_vol;
    float sfx_vol;
    float music_vol;
    bool  fullscreen;
    int anim_intensity;
} Player;/* XP required to go from `level` -> `level + 1` */
int  player_xp_for_level(int level);

void player_init(Player *p);

/* Adds XP; returns number of levels gained (0 if none). */
int  player_add_xp(Player *p, int xp);
void player_add_coins(Player *p, int coins);

/* Call once per frame (cheap): updates day rollover and streak. */
void player_daily_check(Player *p);

/* Called when a review is completed. `quality` = Rating 0..3 */
void player_on_review(Player *p, int quality, bool was_new);

/* Quest tracking. */
void player_tick_quest(Player *p, QuestType t, int amount);
void player_roll_daily_quests(Player *p, uint32_t seed);

/* Returns bitmask / count of achievements newly unlocked. Fills `newly[10]`. */
int  player_check_achievements(Player *p, int *newly, int cap);
/* Longest interval in any of the player's cards, used by MASTER_10. */

#endif
