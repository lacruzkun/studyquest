#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "study/scheduler.h"
#include "cards/cards.h"
#include "player/player.h"

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); exit(1); } \
    else printf("ok: %s\n", #cond); \
} while (0)

/* ------------------------------------------------------------------ */
/*  Scheduler                                                          */
/* ------------------------------------------------------------------ */

static void test_scheduler_new_card(void) {
    Card c = {0};
    c.state = CARD_NEW;
    c.ease = 2.5f;
    double now = 1000000.0;

    CHECK(scheduler_next_interval(&c, RATING_GOOD, now) == 86400.0);
    CHECK(scheduler_next_interval(&c, RATING_AGAIN, now) == 60.0);
    CHECK(scheduler_next_interval(&c, RATING_EASY, now) >= 3 * 86400.0);
}

static void test_scheduler_review_card(void) {
    Card c = {0};
    c.state = CARD_REVIEW;
    c.ease = 2.5f;
    c.interval_sec = 4 * 86400.0;
    double now = 1000000.0;

    double good  = scheduler_next_interval(&c, RATING_GOOD,  now);
    double easy  = scheduler_next_interval(&c, RATING_EASY,  now);
    double hard  = scheduler_next_interval(&c, RATING_HARD,  now);
    double again = scheduler_next_interval(&c, RATING_AGAIN, now);

    CHECK(good > hard);
    CHECK(easy > good);
    CHECK(again < hard);
    CHECK(again >= 10 * 60.0);
}

static void test_scheduler_apply(void) {
    Card c = {0};
    c.state = CARD_NEW;
    c.ease = 2.5f;

    double now = 1000.0;
    scheduler_apply(&c, RATING_GOOD, now);
    CHECK(c.reps == 1);
    CHECK(c.due == now + c.interval_sec);
    CHECK(c.state == CARD_REVIEW);

    scheduler_apply(&c, RATING_AGAIN, c.due);
    CHECK(c.lapses == 1);
    CHECK(c.ease < 2.5f);
    CHECK(c.state == CARD_RELEARNING);
}

/* ------------------------------------------------------------------ */
/*  Progression                                                        */
/* ------------------------------------------------------------------ */

static void test_xp_progression(void) {
    Player p; player_init(&p);

    int need1 = player_xp_for_level(1);
    CHECK(need1 == 100);

    player_add_xp(&p, need1);
    CHECK(p.level == 2);
    CHECK(p.xp == 0);

    int need2 = player_xp_for_level(2);
    CHECK(need2 > need1);
    int need3 = player_xp_for_level(3);
    CHECK(need3 > need2);

    /* Exactly enough to hit level 4: need2 + need3 + a little slack. */
    player_add_xp(&p, need2 + need3 + 25);
    CHECK(p.level == 4);
    CHECK(p.xp == 25);
}

/* ------------------------------------------------------------------ */
/*  Quests                                                             */
/* ------------------------------------------------------------------ */

static void test_quest_roll(void) {
    Player p; player_init(&p);
    player_roll_daily_quests(&p, 12345);
    CHECK(p.quests[0].target > 0);
    CHECK(!p.quests[0].complete);
}

/* ------------------------------------------------------------------ */
/*  Achievements                                                       */
/* ------------------------------------------------------------------ */

static void test_achievement_unlock(void) {
    Player p; player_init(&p);
    int newly[10];
    for (int i = 0; i < 50; i++) player_on_review(&p, 2, true);
    int n = player_check_achievements(&p, newly, 10);
    CHECK(n > 0);
    CHECK(p.achievements[ACH_FIRST_STUDY].unlocked);
    CHECK(p.achievements[ACH_STUDY_100].unlocked == false || p.total_studied >= 100);
}

/* ------------------------------------------------------------------ */
/*  Sample deck                                                        */
/* ------------------------------------------------------------------ */

static void test_deck_sample(void) {
    /* DeckList is large; keep it out of the stack frame. */
    static DeckList dl;
    decklist_init_sample(&dl);

    CHECK(dl.count == 2);

    Deck *cdeck = NULL;
    Deck *jdeck = NULL;
    for (int i = 0; i < dl.count; i++) {
        if (strcmp(dl.decks[i].name, "C Programming") == 0) cdeck = &dl.decks[i];
        if (strncmp(dl.decks[i].name, "Japanese", 8) == 0) jdeck = &dl.decks[i];
    }
    CHECK(cdeck != NULL);
    CHECK(jdeck != NULL);

    CHECK(cdeck->card_count >= 15);
    CHECK(jdeck->card_count >= 4);

    double now = (double)time(NULL);
    CHECK(deck_due_count(cdeck, now) == cdeck->card_count);
    CHECK(deck_due_count(jdeck, now) == jdeck->card_count);

    /* Every Japanese card should be identified as such and carry a
       Word field whose value contains CJK. */
    if (jdeck) {
        int jp_cards = 0;
        for (int i = 0; i < jdeck->card_count; i++) {
            if (card_is_japanese(&jdeck->cards[i])) jp_cards++;
        }
        CHECK(jp_cards == jdeck->card_count);

        int found_konnichiwa = 0;
        int found_taberu      = 0;
        int found_gakusei     = 0;
        for (int i = 0; i < jdeck->card_count; i++) {
            const Card *c = &jdeck->cards[i];
            const char *word = card_find_field(c, "Word");
            if (!word) continue;
            if (strcmp(word, "こんにちは") == 0) found_konnichiwa = 1;
            if (strcmp(word, "食べる")     == 0) found_taberu      = 1;
            if (strcmp(word, "学生")       == 0) found_gakusei     = 1;
        }
        CHECK(found_konnichiwa);
        CHECK(found_taberu);
        CHECK(found_gakusei);
    }
}

/* ------------------------------------------------------------------ */
/*  Streak                                                             */
/* ------------------------------------------------------------------ */

static void test_streak(void) {
    Player p; player_init(&p);

    /* refresh_daily() uses the real clock, so pin today_day to the real
       day to keep it from clobbering our simulated values. */
    int64_t today = (int64_t)(time(NULL) / 86400);
    p.today_day = today;

    /* First ever study. */
    p.last_study_day = 0;
    p.streak = 0;
    player_on_review(&p, 2, true);
    CHECK(p.streak == 1);

    /* Yesterday we studied, so today continues the streak. */
    p.last_study_day = today - 1;
    p.streak = 1;
    player_on_review(&p, 2, true);
    CHECK(p.streak == 2);

    /* Miss a day (today - 3): streak should reset to 1. */
    p.last_study_day = today - 3;
    p.streak = 2;
    player_on_review(&p, 2, true);
    CHECK(p.streak == 1);
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(void) {
    printf("== scheduler ==\n");
    test_scheduler_new_card();
    test_scheduler_review_card();
    test_scheduler_apply();

    printf("== progression ==\n");
    test_xp_progression();

    printf("== quests ==\n");
    test_quest_roll();

    printf("== achievements ==\n");
    test_achievement_unlock();

    printf("== sample deck ==\n");
    test_deck_sample();

    printf("== streak ==\n");
    test_streak();

    printf("\nAll tests passed.\n");
    return 0;
}
