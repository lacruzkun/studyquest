#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>
#include "cards/cards.h"

typedef enum {
    RATING_AGAIN = 0,
    RATING_HARD  = 1,
    RATING_GOOD  = 2,
    RATING_EASY  = 3
} Rating;

/* Returns true if the card is due at time `now` (unix epoch). */
bool   scheduler_card_due(const Card *c, double now);
double scheduler_card_due_time(const Card *c);

/* Returns the next interval (seconds) if the card were reviewed now. */
double scheduler_next_interval(const Card *c, Rating r, double now);

/* Applies the rating to the card — mutates SRS state and due time. */
void   scheduler_apply(Card *c, Rating r, double now);

/* Human-friendly label for an interval ("10m", "1d", "3mo"). */
void   scheduler_format_interval(double sec, char *out, size_t cap);

#endif
