#ifndef ACHIEVEMENTS_H
#define ACHIEVEMENTS_H

typedef struct {
    const char *id;
    const char *name;
    const char *desc;
    int xp_reward;
    int coin_reward;
} AchievementDef;

#define ACH_FIRST_STUDY        0
#define ACH_FIRST_CARD         1
#define ACH_STUDY_100          2
#define ACH_STUDY_1000         3
#define ACH_STREAK_7           4
#define ACH_STREAK_30          5
#define ACH_PERFECT_SESSION    6
#define ACH_MASTER_10          7
#define ACH_STUDY_100_DAY      8
#define ACH_LEVEL_10           9

#define NUM_ACHIEVEMENTS 10

extern const AchievementDef ACH_DEFS[NUM_ACHIEVEMENTS];

#endif
