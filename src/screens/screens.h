#ifndef SCREENS_H
#define SCREENS_H

#include "core/app.h"

void welcome_update(App *a, float dt);
void welcome_draw(App *a);

void dashboard_update(App *a, float dt);
void dashboard_draw(App *a);

void decks_update(App *a, float dt);
void decks_draw(App *a);

void study_update(App *a, float dt);
void study_draw(App *a);

void session_complete_update(App *a, float dt);
void session_complete_draw(App *a);

void achievements_update(App *a, float dt);
void achievements_draw(App *a);

void stats_update(App *a, float dt);
void stats_draw(App *a);

void settings_update(App *a, float dt);
void settings_draw(App *a);
void study_reset_ui(void);

/* helpers exposed to all screens */
void draw_top_nav(App *a, const char *title);

#endif
