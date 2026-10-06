#include "screens.h"
#include "core/theme.h"
#include "ui/ui.h"
#include <stdio.h>
#include <string.h>

void stats_update(App *a, float dt) {
    (void)dt;
    if (IsKeyPressed(KEY_ESCAPE)) app_goto(a, SCREEN_DASHBOARD);
}

static void stat_row(const char *label, const char *value, Rectangle r) {
    ui_text(label, (int)r.x, (int)r.y, FONT_SM, TH.text_muted);
    int tw = MeasureText(value, FONT_MD);
    ui_text(value, (int)(r.x + r.width - tw), (int)r.y - 4, FONT_MD, TH.text);
}

void stats_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    ui_text("STATISTICS", 24, 24, FONT_LG, TH.text);

    Rectangle back = { W - 160.f, 26, 136, 40 };
    if (ui_button(back, "< BACK", TH.panel, TH.text)) app_goto(a, SCREEN_DASHBOARD);

    Player *p = &a->data.player;

    /* Left card: overall */
    Rectangle L = { 24, 90, 420, (float)H - 160 };
    ui_panel_border(L, TH.panel, TH.border, RADIUS_LG, 1.f);
    ui_text("OVERVIEW", (int)L.x + 20, (int)L.y + 16, FONT_XS, TH.text_muted);

    float y = L.y + 50;
    char buf[64];
    snprintf(buf, sizeof(buf), "%lld", (long long)p->total_studied);
    stat_row("Cards studied (all time)", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%lld", (long long)p->total_reviews);
    stat_row("Total reviews", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    float ret = p->total_reviews > 0 ? (float)p->total_correct / (float)p->total_reviews : 0;
    snprintf(buf, sizeof(buf), "%.1f%%", ret * 100.f);
    stat_row("Retention rate", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%d", p->streak);
    stat_row("Current streak", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%d", p->longest_streak);
    stat_row("Longest streak", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%d", p->level);
    stat_row("Level", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%lld", (long long)p->total_sessions);
    stat_row("Sessions", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    snprintf(buf, sizeof(buf), "%d", p->mature_cards);
    stat_row("Mature cards (21d+)", buf, (Rectangle){ L.x + 20, y, L.width - 40, 30 }); y += 40;

    /* Right card: bar chart of daily study (last 14 days) */
    Rectangle R = { 460, 90, (float)W - 484, (float)H - 160 };
    ui_panel_border(R, TH.panel, TH.border, RADIUS_LG, 1.f);
    ui_text("STUDY ACTIVITY", (int)R.x + 20, (int)R.y + 16, FONT_XS, TH.text_muted);

    /* We don't store history per day in this build — display a live snapshot bar:
       today's progress, alongside cumulative totals as horizontal bars. */
    ui_text("This is a live snapshot — history graphs are a planned extension.",
            (int)R.x + 20, (int)R.y + 44, FONT_XS, TH.text_muted);

    /* Today's activity bar */
    float bw = R.width - 40;
    Rectangle tbar = { R.x + 20, R.y + 90, bw, 24 };
    ui_text("Today", (int)tbar.x, (int)tbar.y - 20, FONT_XS, TH.text_dim);
    float day_pct = p->daily_goal > 0 ? (float)p->studied_today / p->daily_goal : 0;
    ui_progress(tbar, day_pct, TH.accent, TH.bg, RADIUS_SM);
    snprintf(buf, sizeof(buf), "%d / %d", p->studied_today, p->daily_goal);
    ui_text_center(buf, tbar, FONT_XS, TH.text);

    /* Weekly approximation via total / sessions (not truly day-bucketed) */
    Rectangle wbar = { R.x + 20, R.y + 160, bw, 24 };
    ui_text("Lifetime cards", (int)wbar.x, (int)wbar.y - 20, FONT_XS, TH.text_dim);
    float lifetime_pct = (float)((p->total_studied % 1000) / 1000.0);
    if (lifetime_pct < 0.02f) lifetime_pct = 0.02f;
    ui_progress(wbar, lifetime_pct, TH.primary, TH.bg, RADIUS_SM);
    snprintf(buf, sizeof(buf), "%lld", (long long)p->total_studied);
    ui_text_center(buf, wbar, FONT_XS, TH.text);

    /* Deck summary */
    ui_text("DECKS", (int)R.x + 20, (int)R.y + 220, FONT_XS, TH.text_muted);
    float dy = R.y + 250;
    for (int i = 0; i < a->data.decks.count && i < 8; i++) {
        Deck *d = &a->data.decks.decks[i];
        DrawRectangleRounded((Rectangle){ R.x + 20, dy, 20, 20 }, 0.4f, 6, d->color);
        ui_text(d->name, (int)R.x + 52, (int)dy + 2, FONT_SM, TH.text);
        char sub[64];
        snprintf(sub, sizeof(sub), "%d cards  •  %.0f%% retention",
                 d->card_count, deck_retention(d) * 100.f);
        int tw = MeasureText(sub, FONT_XS);
        ui_text(sub, (int)(R.x + R.width - 20 - tw), (int)dy + 4, FONT_XS, TH.text_muted);
        dy += 32;
    }
}
