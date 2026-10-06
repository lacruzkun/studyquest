#include "screens.h"
#include "core/theme.h"
#include "ui/ui.h"
#include <stdio.h>
#include <time.h>

void achievements_update(App *a, float dt) {
    (void)dt;
    if (IsKeyPressed(KEY_ESCAPE)) app_goto(a, SCREEN_DASHBOARD);
}

/* Draw a small medallion at `c`. `unlocked` switches between accent gold
   and a dull grey. We draw the check / lock glyph with primitive shapes
   so we don't depend on any particular font glyph being available. */
static void draw_medallion(Vector2 c, bool unlocked) {
    Color outer = unlocked ? TH.accent    : TH.border;
    Color inner = unlocked
        ? ui_lerp_color(TH.accent, TH.text, 0.35f)
        : TH.bg2;

    DrawCircleV(c, 28, outer);
    DrawCircleV(c, 22, inner);

    if (unlocked) {
        /* Checkmark */
        DrawLineEx(
            (Vector2){ c.x - 9, c.y },
            (Vector2){ c.x - 2, c.y + 7 },
            3.5f, TH.bg);
        DrawLineEx(
            (Vector2){ c.x - 2, c.y + 7 },
            (Vector2){ c.x + 10, c.y - 7 },
            3.5f, TH.bg);
    } else {
        /* Small dot to suggest a locked slot */
        DrawCircleV(c, 4, TH.text_muted);
    }
}

void achievements_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    ui_text("ACHIEVEMENTS", 24, 24, FONT_LG, TH.text);

    Rectangle back = { W - 160.f, 26, 136, 40 };
    if (ui_button(back, "< BACK", TH.panel, TH.text)) {
        app_goto(a, SCREEN_DASHBOARD);
    }

    Player *p = &a->data.player;

    /* Two-column grid. Each row is 118 tall. */
    float top    = 90.f;
    float left   = 24.f;
    float right  = 24.f;
    float gutter = 16.f;
    float cw     = (W - left - right - gutter) / 2.f;
    float ch     = 118.f;

    /* Optional scrolling: if there are too many, we could add a scroll.
       With 10 achievements in 2 cols = 5 rows = 590 + gaps, fits fine on
       720p and up. */

    for (int i = 0; i < NUM_ACHIEVEMENTS; i++) {
        const AchievementDef *def = &ACH_DEFS[i];
        bool unlocked = p->achievements[i].unlocked;

        int col = i % 2;
        int row = i / 2;

        float x = left + col * (cw + gutter);
        float y = top  + row * (ch + gutter);

        if (y + ch > H - 24.f) break;

        Rectangle r = { x, y, cw, ch };

        ui_panel_border(
            r,
            unlocked ? TH.panel : TH.bg2,
            unlocked ? TH.accent : TH.border,
            RADIUS_LG,
            unlocked ? 2.f : 1.f);

        /* Medallion */
        Vector2 ic = { r.x + 54.f, r.y + ch / 2.f };
        draw_medallion(ic, unlocked);

        /* Name + description + reward */
        Color name_col   = unlocked ? TH.text       : TH.text_muted;
        Color desc_col   = unlocked ? TH.text_dim   : TH.text_muted;
        Color reward_col = unlocked ? TH.success    : TH.text_muted;

        float text_x = r.x + 104.f;
        float text_w = r.width - 120.f;

        ui_text(def->name, (int)text_x, (int)r.y + 22, FONT_MD, name_col);

        ui_text_wrapped(
            def->desc,
            (Rectangle){ text_x, r.y + 52, text_w, 40 },
            FONT_XS, desc_col, 2);

        char rew[64];
        snprintf(rew, sizeof(rew), "+%d XP   +%d coins",
                 def->xp_reward, def->coin_reward);
        ui_text(rew, (int)text_x, (int)(r.y + ch - 24), FONT_XS, reward_col);

        /* Unlocked timestamp on the right of the reward row */
        if (unlocked && p->achievements[i].unlocked_at > 0.0) {
            time_t t = (time_t)p->achievements[i].unlocked_at;
            struct tm tmv;
            localtime_r(&t, &tmv);
            char stamp[32];
            snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
            int sw = MeasureText(stamp, FONT_XS);
            ui_text(stamp, (int)(r.x + r.width - 16 - sw),
                    (int)(r.y + ch - 24), FONT_XS, TH.text_muted);
        }
    }

    /* Footer summary */
    int unlocked_count = 0;
    for (int i = 0; i < NUM_ACHIEVEMENTS; i++)
        if (p->achievements[i].unlocked) unlocked_count++;

    char buf[64];
    snprintf(buf, sizeof(buf), "%d / %d unlocked", unlocked_count, NUM_ACHIEVEMENTS);
    ui_text_center(buf, (Rectangle){ 0, (float)H - 40, (float)W, 24 }, FONT_SM, TH.text_dim);
}
