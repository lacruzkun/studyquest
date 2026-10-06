#include "player.h"
#include <string.h>
#include <time.h>
#include <stdio.h>

#define DAY_SEC 86400

static int64_t day_number(double unix_sec) {
    return (int64_t)(unix_sec / DAY_SEC);
}

int player_xp_for_level(int level) {
    /* 100, 150, 225, 337, ... */
    double base = 100.0;
    for (int i = 1; i < level; i++) base *= 1.5;
    return (int)base;
}

static void refresh_daily(Player *p) {
    int64_t today = day_number((double)time(NULL));
    if (p->today_day != today) {
        p->studied_today = 0;
        p->today_day = today;
    }
    if (p->quests_day != today) {
        p->quests_day = today;
        player_roll_daily_quests(p, (uint32_t)today);
    }
}

void player_daily_check(Player *p) {
    refresh_daily(p);

    /* Streak maintenance: if last_study_day is more than 1 day ago, break streak. */
    int64_t today = p->today_day;
    if (p->last_study_day > 0 && today - p->last_study_day > 1) {
        p->streak = 0;
    }
}

void player_init(Player *p) {
    memset(p, 0, sizeof(*p));
    p->level = 1;
    p->xp = 0;
    p->coins = 0;
    p->streak = 0;
    p->longest_streak = 0;
    p->daily_goal = 30;
    p->master_vol = 0.8f;
    p->sfx_vol = 0.9f;
    p->music_vol = 0.4f;
    p->fullscreen = false;
    p->today_day = day_number((double)time(NULL));
    p->quests_day = 0; /* force roll */
    refresh_daily(p);
}

int player_add_xp(Player *p, int xp) {
    if (xp <= 0) return 0;
    p->xp += xp;
    int gained = 0;
    while (p->xp >= player_xp_for_level(p->level)) {
        p->xp -= player_xp_for_level(p->level);
        p->level++;
        gained++;
    }
    return gained;
}

void player_add_coins(Player *p, int c) {
    if (c <= 0) return;
    p->coins += c;
}

void player_on_review(Player *p, int quality, bool was_new) {
    (void)was_new;

    refresh_daily(p);

    int64_t today = p->today_day;
    if (p->last_study_day != today) {
        /* Either first day or continuing from yesterday */
        if (p->last_study_day == today - 1) {
            p->streak++;
        } else if (p->last_study_day == 0) {
            p->streak = 1;
        } else {
            p->streak = 1;
        }
        if (p->streak > p->longest_streak) p->longest_streak = p->streak;
    }
    p->last_study_day = today;

    p->studied_today++;
    if (p->studied_today > p->daily_peak) p->daily_peak = p->studied_today;

    p->total_studied++;
    p->total_reviews++;
    p->session_studied++;
    if (quality >= 2) { p->total_correct++; p->session_good++; }
    if (quality == 2) p->total_good++;
    if (quality == 3) { p->total_good++; p->total_easy++; }

    /* Quest progress */
    player_tick_quest(p, QT_STUDY_CARDS, 1);
    if (quality == 2) player_tick_quest(p, QT_GOOD_RATINGS, 1);
    if (quality == 3) player_tick_quest(p, QT_EASY_RATINGS, 1);
}

void player_tick_quest(Player *p, QuestType t, int amount) {
    for (int i = 0; i < NUM_QUESTS; i++) {
        Quest *q = &p->quests[i];
        if (q->complete) continue;
        if (q->type != t) continue;
        q->progress += amount;
        if (q->progress >= q->target) {
            q->progress = q->target;
            q->complete = true;
            player_add_xp(p, q->xp_reward);
            player_add_coins(p, q->coin_reward);
        }
    }
}

void player_roll_daily_quests(Player *p, uint32_t seed) {
    /* Simple deterministic per-day roll */
    uint32_t s = seed * 2654435761u + 1013904223u;
    #define NEXT() (s = s * 1103515245u + 12345u, (s >> 16) & 0x7FFF)

    const QuestType pool[3] = { QT_STUDY_CARDS, QT_GOOD_RATINGS, QT_EASY_RATINGS };

    for (int i = 0; i < NUM_QUESTS; i++) {
        Quest *q = &p->quests[i];
        QuestType t = pool[(NEXT() + i) % 3];
        q->type = t;
        q->progress = 0;
        q->complete = false;
        switch (t) {
            case QT_STUDY_CARDS:
                q->target = 10 + (int)(NEXT() % 20);
                snprintf(q->desc, sizeof(q->desc), "Study %d cards", q->target);
                q->xp_reward = 80; q->coin_reward = 20;
                break;
            case QT_GOOD_RATINGS:
                q->target = 5 + (int)(NEXT() % 10);
                snprintf(q->desc, sizeof(q->desc), "Get %d Good ratings", q->target);
                q->xp_reward = 60; q->coin_reward = 15;
                break;
            case QT_EASY_RATINGS:
                q->target = 3 + (int)(NEXT() % 6);
                snprintf(q->desc, sizeof(q->desc), "Get %d Easy ratings", q->target);
                q->xp_reward = 50; q->coin_reward = 15;
                break;
            default: break;
        }
    }
    #undef NEXT
}

/* ------------------------------------------------------------------ */
/*  Achievement checks                                                */
/* ------------------------------------------------------------------ */

int player_check_achievements(Player *p, int *newly, int cap) {
    int n = 0;
    double now = (double)time(NULL);

    #define UNLOCK(idx) do {                                 \
        if (!p->achievements[(idx)].unlocked) {              \
            p->achievements[(idx)].unlocked = true;          \
            p->achievements[(idx)].unlocked_at = now;        \
            p->coins += ACH_DEFS[(idx)].coin_reward;         \
            player_add_xp(p, ACH_DEFS[(idx)].xp_reward);     \
            if (newly && n < cap) newly[n] = (idx);          \
            n++;                                             \
        }                                                    \
    } while (0)

    if (p->total_studied >= 1)   UNLOCK(ACH_FIRST_STUDY);
    if (p->total_studied >= 100) UNLOCK(ACH_STUDY_100);
    if (p->total_studied >= 1000)UNLOCK(ACH_STUDY_1000);
    if (p->streak >= 7)          UNLOCK(ACH_STREAK_7);
    if (p->streak >= 30)         UNLOCK(ACH_STREAK_30);
    if (p->studied_today >= 100) UNLOCK(ACH_STUDY_100_DAY);
    if (p->mature_cards >= 10)   UNLOCK(ACH_MASTER_10);
    if (p->level >= 10)          UNLOCK(ACH_LEVEL_10);
    if (p->session_studied >= 10 && p->session_good == p->session_studied)
        UNLOCK(ACH_PERFECT_SESSION);

    #undef UNLOCK
    return n;
}
