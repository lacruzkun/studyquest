#include "scheduler.h"
#include <stdio.h>
#include <math.h>

#define MIN_LEARN_SEC  60.0            /* 1 minute */
#define AGAIN_RELEARN_SEC (10 * 60.0)  /* 10 minutes */
#define DAY (24.0 * 60.0 * 60.0)

bool scheduler_card_due(const Card *c, double now) {
    return c->due <= now;
}

double scheduler_card_due_time(const Card *c) {
    return c->due;
}

double scheduler_next_interval(const Card *c, Rating r, double now) {
    (void)now;

    /* Learning step for a brand-new card */
    if (c->state == CARD_NEW) {
        switch (r) {
            case RATING_AGAIN: return MIN_LEARN_SEC;
            case RATING_HARD:  return 6.0 * 60.0;   /* 6 minutes */
            case RATING_GOOD:  return DAY;          /* 1 day */
            case RATING_EASY:  return 3.0 * DAY;    /* 3 days */
        }
    }

    double iv = c->interval_sec;
    if (iv < MIN_LEARN_SEC) iv = DAY;

    switch (r) {
        case RATING_AGAIN: {
            /* Relearning: fall back, don't reset to 0 */
            double next = iv * 0.4;
            if (next < AGAIN_RELEARN_SEC) next = AGAIN_RELEARN_SEC;
            return next;
        }
        case RATING_HARD: {
            double next = iv * 1.2;
            if (next < DAY) next = DAY;
            return next;
        }
        case RATING_GOOD: {
            double e = c->ease > 0.f ? c->ease : 2.5;
            double next = iv * e;
            if (next < DAY) next = DAY;
            return next;
        }
        case RATING_EASY: {
            double e = (c->ease > 0.f ? c->ease : 2.5) * 1.3;
            double next = iv * e;
            if (next < 3.0 * DAY) next = 3.0 * DAY;
            return next;
        }
    }
    return iv;
}

void scheduler_apply(Card *c, Rating r, double now) {
    double iv = scheduler_next_interval(c, r, now);

    c->interval_sec = iv;
    c->due = now + iv;
    c->last_review = now;
    c->last_rating = (int)r;
    c->reps++;

    /* Ease updates — SM-2 inspired */
    switch (r) {
        case RATING_AGAIN:
            c->ease -= 0.20f;
            c->lapses++;
            c->state = CARD_RELEARNING;
            break;
        case RATING_HARD:
            c->ease -= 0.15f;
            c->state = CARD_REVIEW;
            break;
        case RATING_GOOD:
            c->state = CARD_REVIEW;
            break;
        case RATING_EASY:
            c->ease += 0.15f;
            c->state = CARD_REVIEW;
            break;
    }
    if (c->ease < 1.3f) c->ease = 1.3f;
    if (c->ease > 3.0f) c->ease = 3.0f;
}

void scheduler_format_interval(double sec, char *out, size_t cap) {
    if (sec < 60)              snprintf(out, cap, "%ds", (int)sec);
    else if (sec < 3600)       snprintf(out, cap, "%dm", (int)(sec/60));
    else if (sec < DAY)        snprintf(out, cap, "%dh", (int)(sec/3600));
    else if (sec < 30*DAY)     snprintf(out, cap, "%dd", (int)(sec/DAY));
    else if (sec < 365*DAY)    snprintf(out, cap, "%dmo",(int)(sec/(30*DAY)));
    else                       snprintf(out, cap, "%.1fy", sec/(365*DAY));
}
