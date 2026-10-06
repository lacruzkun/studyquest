#include "screens.h"
#include "core/theme.h"
#include "core/anim.h"
#include "ui/ui.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* ==================================================================== */
/*  Welcome / first-run screen                                          */
/* ==================================================================== */

void welcome_update(App *a, float dt) { (void)a; (void)dt; }

void welcome_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    float t = a->screen_t;
    float t_sec = (float)GetTime();

    /* Ambient floating shapes behind everything */
    anim_draw_ambient_bg(W, H, t_sec);

    /* Soft vertical gradient wash on top of the ambient layer */
    for (int i = 0; i < 8; i++) {
        float g = (float)i / 7.f;
        Color c = ui_lerp_color(TH.bg, (Color){ 26, 30, 60, 255 }, g);
        c.a = 120;
        DrawRectangle(0, (H * i) / 8, W, H / 8 + 1, c);
    }

    /* Title — fades up + overshoots in */
    float p = anim_clamp01(t / 0.6f);
    float e = ease_out_back(p);
    int title_y = (int)(H/2 - 160 + (1.f - e) * 40.f);
    Color title_c = anim_color_alpha(TH.text, ease_out_cubic(anim_clamp01(t / 0.4f)));
    ui_text_center("STUDYQUEST",
        (Rectangle){ 0, (float)title_y, (float)W, 80 },
        72, title_c);

    /* Subtitle — delayed */
    float sp = anim_clamp01((t - 0.20f) / 0.5f);
    if (sp > 0.f) {
        Color sub = anim_color_alpha(TH.text_dim, sp);
        ui_text_center("Learn. Level up. Master anything.",
            (Rectangle){ 0, (float)(H/2 - 40), (float)W, 40 },
            FONT_MD, sub);
    }

    /* START button — pulsing glow when idle, spring on hover */
    float bp = anim_clamp01((t - 0.40f) / 0.4f);
    if (bp > 0.f) {
        float pulse = 0.5f + 0.5f * sinf(t_sec * 1.8f);
        Rectangle btn = { (float)(W/2 - 130), (float)(H/2 + 40), 260, 68 };
        Rectangle glow = btn; glow.x -= 10; glow.y -= 10;
        glow.width += 20; glow.height += 20;

        Color gc = TH.primary; gc.a = (unsigned char)(70 * bp * pulse);
        ui_panel(glow, gc, RADIUS_LG);

        bool hov = ui_button_hover(btn);
        Color fill = hov ? TH.primary_hi : TH.primary;
        fill = anim_color_alpha(fill, bp);

        float scale = 1.f + (hov ? 0.04f : 0.f) + pulse * 0.01f;
        float dw = btn.width * (1.f - scale) / 2.f;
        float dh = btn.height * (1.f - scale) / 2.f;
        Rectangle dr = { btn.x + dw, btn.y + dh, btn.width * scale, btn.height * scale };

        ui_panel_border(dr, fill,
            anim_color_alpha(TH.primary_hi, bp),
            RADIUS_MD, 1.5f);
        ui_text_center("START", dr, FONT_MD,
                       anim_color_alpha(TH.text, bp));

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app_goto(a, SCREEN_DASHBOARD);
            app_save(a);
        }
    }

    /* Footer hint */
    float fp = anim_clamp01((t - 0.8f) / 0.6f);
    if (fp > 0.f) {
        ui_text_center("Press Esc later to return here at any time.",
            (Rectangle){ 0, (float)H - 60, (float)W, 30 },
            FONT_XS, anim_color_alpha(TH.text_muted, fp));
    }
}

/* ==================================================================== */
/*  Dashboard                                                           */
/* ==================================================================== */

/* XP bar smooth follow. Static so it persists across frame boundaries. */
static float s_xp_bar_anim = 0.f;
static float s_xp_num_anim = 0.f;
static int   s_xp_num_last = -1;

void dashboard_update(App *a, float dt) {
    Player *p = &a->data.player;

    /* Smooth xp bar toward the real value */
    float target = (float)p->xp / (float)player_xp_for_level(p->level);
    s_xp_bar_anim += (target - s_xp_bar_anim) * anim_clamp01(dt * 7.f);

    /* XP number ticks up smoothly to the current value */
    if (s_xp_num_last != p->xp) {
        s_xp_num_last = p->xp;
    }
    s_xp_num_anim += ((float)p->xp - s_xp_num_anim) * anim_clamp01(dt * 9.f);
    if (fabsf(s_xp_num_anim - (float)p->xp) < 0.4f) s_xp_num_anim = (float)p->xp;

    if (IsKeyPressed(KEY_ESCAPE)) {
        app_goto(a, SCREEN_WELCOME);
    }
}

static void draw_stat_chip(Rectangle r, const char *label, const char *value, Color accent) {
    ui_panel_border(r, TH.panel, TH.border, RADIUS_MD, 1.0f);
    ui_text(label, (int)r.x + 14, (int)r.y + 10, FONT_XS, TH.text_muted);
    ui_text(value, (int)r.x + 14, (int)r.y + 28, FONT_LG, accent);
}

/* Small helper: staggered entrance transform for an element.
   Returns a rectangle offset slightly to the right while it enters,
   with alpha driven by the same progress. */
static void stagger_enter(float screen_t, int index, float delay_step,
                          float *out_dx, float *out_alpha) {
    float start = 0.10f + index * delay_step;
    float p = anim_clamp01((screen_t - start) / 0.42f);
    float e = ease_out_back(p);
    *out_dx    = (1.f - e) * 34.f;
    *out_alpha = ease_out_cubic(p);
}

void dashboard_draw(App *a) {
    Player *p = &a->data.player;
    int W = GetScreenWidth(), H = GetScreenHeight();
    float t_sec = (float)GetTime();

    anim_draw_ambient_bg(W, H, t_sec);

    /* --- header ------------------------------------------------ */
    float hdr_p = anim_clamp01(a->screen_t / 0.4f);
    float hdr_e = ease_out_cubic(hdr_p);
    float hdr_dy = (1.f - hdr_e) * -30.f;

    Rectangle header = { 24, 24 + hdr_dy, (float)W - 48, 120 };
    ui_panel_gradient(header,
                      anim_color_alpha(TH.panel_hi, hdr_e),
                      anim_color_alpha(TH.panel,    hdr_e),
                      RADIUS_LG);
    ui_outline(header, 0.20f, 8, 1.f, anim_color_alpha(TH.border_hi, hdr_e));

    /* Level badge */
    ui_text("LEVEL", (int)header.x + 24, (int)header.y + 18, FONT_XS,
            anim_color_alpha(TH.text_muted, hdr_e));
    char lvl[16]; snprintf(lvl, sizeof(lvl), "%d", p->level);
    ui_text(lvl, (int)header.x + 24, (int)header.y + 34, FONT_XL,
            anim_color_alpha(TH.text, hdr_e));

    /* XP bar */
    int xp_need = player_xp_for_level(p->level);
    Rectangle xp_r = { header.x + 180, header.y + 40, header.width - 340, 22 };
    ui_progress(xp_r, s_xp_bar_anim, TH.primary, TH.bg, RADIUS_MD);

    /* Subtle glow on top of the bar for polish */
    if (s_xp_bar_anim > 0.02f) {
        Rectangle fill = xp_r;
        fill.width *= s_xp_bar_anim;
        Color glow = TH.primary_hi;
        glow.a = (unsigned char)(60 * s_xp_bar_anim * hdr_e);
        Rectangle g = fill; g.y -= 2; g.height += 4;
        ui_panel(g, glow, RADIUS_MD);
    }

    char xpbuf[64];
    snprintf(xpbuf, sizeof(xpbuf), "%d / %d XP", (int)(s_xp_num_anim + 0.5f), xp_need);
    ui_text_center(xpbuf, xp_r, FONT_XS,
                   anim_color_alpha(TH.text, hdr_e));

    /* Streak + coins on the right */
    char streakbuf[32]; snprintf(streakbuf, sizeof(streakbuf), "STREAK  %d", p->streak);
    ui_text(streakbuf,
            (int)header.x + (int)header.width - 300,
            (int)header.y + 22, FONT_MD,
            anim_color_alpha(TH.accent, hdr_e));

    char coinbuf[32]; snprintf(coinbuf, sizeof(coinbuf), "COINS  %d", p->coins);
    ui_text(coinbuf,
            (int)header.x + (int)header.width - 300,
            (int)header.y + 56, FONT_MD,
            anim_color_alpha(TH.warning, hdr_e));

    /* --- Today panel ------------------------------------------- */
    float dx, al;
    stagger_enter(a->screen_t, 0, 0.08f, &dx, &al);

    Rectangle today = { 24 + dx, 168, 380, 260 };
    ui_panel_border(today,
                    anim_color_alpha(TH.panel, al),
                    anim_color_alpha(TH.border, al),
                    RADIUS_LG, 1.f);

    ui_text("TODAY", (int)today.x + 20, (int)today.y + 16, FONT_XS,
            anim_color_alpha(TH.text_muted, al));

    int due = 0;
    for (int i = 0; i < a->data.decks.count; i++)
        due += deck_due_count(&a->data.decks.decks[i], (double)time(NULL));

    char buf[64];
    snprintf(buf, sizeof(buf), "%d", due);
    ui_text(buf, (int)today.x + 20, (int)today.y + 36, FONT_XL,
            anim_color_alpha(TH.text, al));
    ui_text("cards due",
            (int)today.x + 20 + ui_measure(buf, FONT_XL) + 10,
            (int)today.y + 58, FONT_SM,
            anim_color_alpha(TH.text_dim, al));

    snprintf(buf, sizeof(buf), "%d", p->studied_today);
    ui_text(buf, (int)today.x + 20, (int)today.y + 100, FONT_LG,
            anim_color_alpha(TH.success, al));
    ui_text("completed",
            (int)today.x + 20 + ui_measure(buf, FONT_LG) + 10,
            (int)today.y + 112, FONT_SM,
            anim_color_alpha(TH.text_dim, al));

    /* Daily goal */
    ui_text("DAILY GOAL", (int)today.x + 20, (int)today.y + 160, FONT_XS,
            anim_color_alpha(TH.text_muted, al));
    Rectangle goal_r = { today.x + 20, today.y + 182, today.width - 40, 16 };
    float goal_pct = (float)p->studied_today /
                     (float)(p->daily_goal > 0 ? p->daily_goal : 1);
    ui_progress(goal_r, goal_pct,
                anim_color_alpha(TH.accent, al),
                anim_color_alpha(TH.bg, al),
                RADIUS_SM);
    snprintf(buf, sizeof(buf), "%d / %d", p->studied_today, p->daily_goal);
    ui_text_center(buf, goal_r, FONT_XS, anim_color_alpha(TH.text, al));

    /* STUDY NOW button — gentle pulse if there's something to study */
    Rectangle study_btn = { today.x + 20, today.y + 210, today.width - 40, 44 };
    bool has_due = (due > 0);
    bool hov_now = ui_button_hover(study_btn);
    Color sn_fill = hov_now ? TH.primary_hi : TH.primary;
    if (has_due) {
        float pulse = 0.5f + 0.5f * sinf(t_sec * 2.2f);
        sn_fill = ui_lerp_color(sn_fill, TH.primary_hi, 0.15f * pulse);
    }
    sn_fill = anim_color_alpha(sn_fill, al);
    ui_panel_border(study_btn, sn_fill,
                    anim_color_alpha(TH.primary_hi, al),
                    RADIUS_MD, 1.f);
    ui_text_center("STUDY NOW", study_btn, FONT_MD,
                   anim_color_alpha(TH.text, al));

    if (al >= 1.f && hov_now && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int pick = -1;
        for (int i = 0; i < a->data.decks.count; i++) {
            if (deck_due_count(&a->data.decks.decks[i], (double)time(NULL)) > 0) {
                pick = a->data.decks.decks[i].id;
                break;
            }
        }
        if (pick < 0 && a->data.decks.count > 0)
            pick = a->data.decks.decks[0].id;
        if (pick >= 0) app_start_session(a, pick);
        else           app_toast(a, "Create a deck first.", TH.warning);
    }

    /* --- Quests ------------------------------------------------ */
    stagger_enter(a->screen_t, 1, 0.08f, &dx, &al);

    Rectangle qr = { 424 + dx, 168, 380, 260 };
    ui_panel_border(qr,
                    anim_color_alpha(TH.panel, al),
                    anim_color_alpha(TH.border, al),
                    RADIUS_LG, 1.f);
    ui_text("DAILY QUESTS", (int)qr.x + 20, (int)qr.y + 16, FONT_XS,
            anim_color_alpha(TH.text_muted, al));

    for (int i = 0; i < NUM_QUESTS; i++) {
        Quest *q = &p->quests[i];

        /* Each quest line enters a touch later than the previous */
        float qp = anim_clamp01((a->screen_t - 0.30f - i * 0.08f) / 0.35f);
        float qdx = (1.f - ease_out_back(qp)) * 20.f;
        float qa = ease_out_cubic(qp) * al;

        float yy = qr.y + 44 + i * 68;
        Color title_c = q->complete ? TH.success : TH.text;
        ui_text(q->desc, (int)(qr.x + 20 + qdx), (int)yy, FONT_SM,
                anim_color_alpha(title_c, qa));

        char pr[32]; snprintf(pr, sizeof(pr), "%d / %d", q->progress, q->target);
        int tw = ui_measure(pr, FONT_XS);
        ui_text(pr, (int)(qr.x + qr.width - 20 - tw + qdx), (int)yy + 2, FONT_XS,
                anim_color_alpha(TH.text_dim, qa));

        Rectangle bar = { qr.x + 20 + qdx, yy + 22, qr.width - 40, 12 };
        ui_progress(bar, (float)q->progress / (float)q->target,
                    anim_color_alpha(q->complete ? TH.success : TH.primary_hi, qa),
                    anim_color_alpha(TH.bg, qa),
                    RADIUS_SM);

        char rew[64];
        snprintf(rew, sizeof(rew), "+%d XP   +%d coins", q->xp_reward, q->coin_reward);
        ui_text(rew, (int)(qr.x + 20 + qdx), (int)yy + 38, FONT_XS,
                anim_color_alpha(TH.text_muted, qa));
    }

    /* --- Decks (right column) ---------------------------------- */
    stagger_enter(a->screen_t, 2, 0.08f, &dx, &al);

    Rectangle dr = { 824 + dx, 168, (float)W - 848 - dx, 260 };
    if (dr.width < 100) dr.width = 100;
    ui_panel_border(dr,
                    anim_color_alpha(TH.panel, al),
                    anim_color_alpha(TH.border, al),
                    RADIUS_LG, 1.f);
    ui_text("DECKS", (int)dr.x + 20, (int)dr.y + 16, FONT_XS,
            anim_color_alpha(TH.text_muted, al));

    int max_show = (int)((dr.height - 50) / 48);
    for (int i = 0; i < a->data.decks.count && i < max_show; i++) {
        Deck *d = &a->data.decks.decks[i];

        /* Stagger each row further */
        float rp = anim_clamp01((a->screen_t - 0.34f - i * 0.07f) / 0.4f);
        float e = ease_out_back(rp);
        float rdx = (1.f - e) * 40.f;
        float ra = ease_out_cubic(rp) * al;

        float yy = dr.y + 44 + i * 48;
        Rectangle row = { dr.x + 12 + rdx, yy, dr.width - 24, 40 };

        bool hover = ui_button_hover(row);

        /* Hover state: fill + slight expand */
        if (hover) {
            Rectangle hov_r = row;
            hov_r.x -= 3; hov_r.width += 6;
            Color hv = TH.panel_hi;
            hv.a = (unsigned char)(255 * ra);
            ui_panel(hov_r, hv, RADIUS_MD);
        }

        Color swatch = d->color;
        swatch.a = (unsigned char)(255 * ra);
        DrawRectangleRounded(
            (Rectangle){ row.x + 8, row.y + 8, 24, 24 },
            0.4f, 6, swatch);

        ui_text(d->name,
                (int)row.x + 44, (int)row.y + 4, FONT_SM,
                anim_color_alpha(TH.text, ra));

        char sub[64];
        int due_c = deck_due_count(d, (double)time(NULL));
        snprintf(sub, sizeof(sub), "%d cards • %d due", d->card_count, due_c);
        ui_text(sub,
                (int)row.x + 44, (int)row.y + 22, FONT_XS,
                anim_color_alpha(TH.text_muted, ra));

        if (ra >= 1.f && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app_start_session(a, d->id);
        }
    }

    if (a->data.decks.count == 0) {
        ui_text("No decks yet.",
                (int)dr.x + 20, (int)dr.y + 50, FONT_SM,
                anim_color_alpha(TH.text_muted, al));
    }

    /* --- Footer / nav ----------------------------------------- */
    stagger_enter(a->screen_t, 3, 0.08f, &dx, &al);

    int nav_y = H - 60;
    Rectangle nav = { 24, (float)nav_y, (float)W - 48, 44 };

    struct { const char *label; Screen s; } tabs[] = {
        { "DASHBOARD",    SCREEN_DASHBOARD    },
        { "DECKS",        SCREEN_DECKS        },
        { "ACHIEVEMENTS", SCREEN_ACHIEVEMENTS },
        { "STATISTICS",   SCREEN_STATS        },
        { "SETTINGS",     SCREEN_SETTINGS     },
    };
    int n = sizeof(tabs) / sizeof(tabs[0]);
    float bw = nav.width / n;

    for (int i = 0; i < n; i++) {
        Rectangle b = { nav.x + bw * i + 4 + dx, nav.y, bw - 8, nav.height };
        bool active = (a->screen == tabs[i].s);
        bool hover = ui_button_hover(b);

        Color fill = active ? TH.primary_lo
                   : hover  ? TH.panel_hi
                            : TH.panel;
        fill = anim_color_alpha(fill, al);

        /* Slight lift on hover */
        float lift = hover ? -2.f : 0.f;
        Rectangle dr_b = b; dr_b.y += lift;

        ui_panel_border(dr_b, fill,
                        anim_color_alpha(active ? TH.primary : TH.border, al),
                        RADIUS_MD, active ? 1.5f : 1.f);
        ui_text_center(tabs[i].label, dr_b, FONT_SM,
                       anim_color_alpha(TH.text, al));

        if (al >= 1.f && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            app_goto(a, tabs[i].s);
        }
    }
}
