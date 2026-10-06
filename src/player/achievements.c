#include "achievements.h"

const AchievementDef ACH_DEFS[NUM_ACHIEVEMENTS] = {
    { "FIRST_STUDY",       "First Steps",        "Review your first card.",                20,   5 },
    { "FIRST_CARD",        "Deck Builder",       "Create your first card.",                25,  10 },
    { "STUDY_100",         "Getting Serious",    "Study 100 cards.",                      100,  30 },
    { "STUDY_1000",        "Marathon Mind",      "Study 1000 cards.",                     500, 150 },
    { "STREAK_7",          "Week Warrior",       "Reach a 7-day study streak.",           200,  60 },
    { "STREAK_30",         "Iron Habit",         "Reach a 30-day study streak.",          800, 250 },
    { "PERFECT_SESSION",   "Flawless",           "Complete a session of 10+ cards with no Again ratings.", 150, 50 },
    { "MASTER_10",         "Mature Mind",        "Have 10 cards with intervals of 21+ days.", 300, 80 },
    { "STUDY_100_DAY",     "Century Day",        "Study 100 cards in one day.",           250,  75 },
    { "LEVEL_10",          "Double Digits",      "Reach level 10.",                       400, 120 },
};
