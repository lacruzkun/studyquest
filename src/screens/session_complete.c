#include "screens.h"
#include "core/theme.h"
#include "core/anim.h"
#include "ui/ui.h"
#include <stdio.h>
#include <math.h>

typedef struct SCUI {
    float t;
    float row[5];         /* 0..1 per row */
    int   fired[5];
    float xp_counter;     /* animated XP tick-up */
    float coins_counter;
    float score_pop;      /* success % reveal */
    float burst_done;
} SCUI;

static SCUI C;

static bool is_milestone_streak(int n) {
    return n == 3 || n == 7 || n == 14 || n == 30 || n == 60 || n == 100 || n == 365;
}

/* On first entry, reset the module state. The caller sets this by re-entering. */
static void sc_reset(float xp_target, float coins_target) {
    (void)xp_target; (void)coins_target;
    C.t = 0.f;
    for (int i = 0; i < 5; i++) { C.row[i] = 0.f; C.fired[i] = 0; }
    C.xp_counter = 0.f;
    C.coins_counter = 0.f;
    C.score_pop = 0.f;
    C.burst_done = 0;
}

void session_complete_update(App *a, float dt) {
    /* Detect entry: t == 0 means we've just arrived */
    if (C.t == 0.f) sc_reset((float)a->session.xp_gained, (float)a->session.coins_gained);

    C.t += dt;

    /* Stagger the rows. Order: header, big count, xp/coins, streak, continue */
    const float starts[5] = { 0.10f, 0.28f, 0.58f, 0.92f, 1.30f };
    for (int i = 0; i < 5; i++) {
        if (C.t > starts[i]) {
            C.row[i] += dt * 3.6f;
            if (C.row[i] > 1.f) C.row[i] = 1.f;

            /* Fire the burst when the big count finishes revealing */
            if (i == 1 && C.row[1] >= 1.f && !C.fired[1]) {
                C.fired[1] = 1;
                particles_burst_ring(&a->particles,
                    (Vector2){ GetScreenWidth()/2.f, GetScreenHeight()/2.f - 40.f },
                    28, TH.accent, 320.f, 0.9f, P_SHAPE_STAR, 8.f);
                particles_burst_shaped(&a->particles,
                    (Vector2){ GetScreenWidth()/2.f, GetScreenHeight()/2.f - 40.f },
                    20, TH.primary_hi, 100.f, 260.f, 0.9f,
                    P_SHAPE_CIRCLE, 3.f, 6.f, 0.f, 180.f);
                shake_add(&a->shake, 4.f);
            }
        }
    }

    /* XP/coin counters tick up */
    if (C.t > 0.58f) {
        float p = anim_clamp01((C.t - 0.58f) / 0.9f);
        C.xp_counter    = a->session.xp_gained    * ease_out_cubic(p);
        C.coins_counter = a->session.coins_gained * ease_out_cubic(p);
    }

    /* Success pop */
    if (C.t > 0.40f) C.score_pop = anim_clamp01((C.t - 0.40f) / 0.3f);

    /* Skip / continue */
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_SPACE)) {
        if (C.row[4] >= 1.f) app_goto(a, SCREEN_DASHBOARD);
        else { for (int i = 0; i < 5; i++) C.row[i] = 1.f; C.t = 1.5f; }
    }
}

static void reveal_text(const char *t, Rectangle r, int size, Color c, float reveal) {
    if (reveal <= 0.f) return;
    float e = ease_out_back(reveal);
    float sc = 0.82f + 0.18f * e;
    int s = (int)(size * sc);
    ui_text_center(t, r, s, anim_color_alpha(c, reveal));
}

void session_complete_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    Player *p = &a->data.player;

    anim_draw_ambient_bg(W, H, (float)GetTime());

    Rectangle panel = { W/2.f - 360, H/2.f - 280, 720, 560 };
    ui_panel_gradient(panel, TH.panel_hi, TH.panel, RADIUS_LG);
    ui_outline(panel, 0.14f, 12, 2.f, TH.border_hi);

    /* Header */
    reveal_text("SESSION COMPLETE",
        (Rectangle){ panel.x, panel.y + 30, panel.width, 32 },
        FONT_MD, TH.accent, C.row[0]);
    if (d)
        ui_text_center(d->name,
            (Rectangle){ panel.x, panel.y + 66, panel.width, 26 },
            FONT_SM, anim_color_alpha(TH.text_dim, C.row[0]));

    /* Big card count */
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", s->session_done);
    reveal_text(buf,
        (Rectangle){ panel.x, panel.y + 108, panel.width, 96 },
        FONT_XL, TH.text, C.row[1]);
    reveal_text("CARDS REVIEWED",
        (Rectangle){ panel.x, panel.y + 206, panel.width, 22 },
        FONT_XS, TH.text_muted, C.row[1]);

    /* Success rate */
    if (C.score_pop > 0.f && s->session_done > 0) {
        int good = p->session_good;
        int pct  = (good * 100) / s->session_done;
        snprintf(buf, sizeof(buf), "%d%% SUCCESS", pct);
        Color sc = pct >= 80 ? TH.success : pct >= 50 ? TH.primary_hi : TH.warning;
        reveal_text(buf,
            (Rectangle){ panel.x, panel.y + 240, panel.width, 30 },
            FONT_MD, sc, C.score_pop);
    }

    /* XP and coins — animated counters */
    if (C.row[2] > 0.f) {
        snprintf(buf, sizeof(buf), "+%d XP", (int)C.xp_counter);
        reveal_text(buf,
            (Rectangle){ panel.x, panel.y + 290, panel.width/2.f, 50 },
            FONT_LG, TH.success, C.row[2]);

        snprintf(buf, sizeof(buf), "+%d COINS", (int)C.coins_counter);
        reveal_text(buf,
            (Rectangle){ panel.x + panel.width/2.f, panel.y + 290,
                         panel.width/2.f, 50 },
            FONT_LG, TH.warning, C.row[2]);
    }

    /* Streak — milestone pops harder */
    if (C.row[3] > 0.f) {
        bool milestone = is_milestone_streak(p->streak);
        float pop = 0.f;
        if (milestone) {
            float s_t = ease_out_elastic(ui_clamp01(C.row[3]));
            pop = (1.f - s_t) * 12.f;
        }
        Color stc = milestone ? TH.accent : TH.text;
        int st_size = milestone ? (int)(FONT_LG + pop) : FONT_MD;
        snprintf(buf, sizeof(buf), "%d DAY STREAK", p->streak);
        reveal_text(buf,
            (Rectangle){ panel.x, panel.y + 362, panel.width, 50 },
            st_size, stc, C.row[3]);

        if (milestone && !C.fired[3]) {
            C.fired[3] = 1;
            particles_burst_ring(&a->particles,
                (Vector2){ W/2.f, panel.y + 380.f },
                30, TH.accent, 360.f, 1.0f, P_SHAPE_STAR, 9.f);
        }
    }

    /* Continue button */
    if (C.row[4] > 0.f) {
        Rectangle btn = { panel.x + 140, panel.y + 460, panel.width - 280, 62 };
        float e = ease_out_back(C.row[4]);
        float sc = 0.9f + 0.1f * e;
        Rectangle dr = {
            btn.x + btn.width * (1.f - sc) / 2.f,
            btn.y + btn.height * (1.f - sc) / 2.f,
            btn.width * sc, btn.height * sc
        };
        Color fill = anim_color_alpha(TH.primary, C.row[4]);
        ui_panel_border(dr, fill,
                        anim_color_alpha(TH.primary_hi, C.row[4]),
                        RADIUS_MD, 1.5f);
        ui_text_center("CONTINUE  [Enter]", dr, FONT_MD,
                       anim_color_alpha(TH.text, C.row[4]));
        if (ui_button_hover(dr) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            app_goto(a, SCREEN_DASHBOARD);
    }
}
