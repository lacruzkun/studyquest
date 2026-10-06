#include "screens.h"
#include "core/theme.h"
#include "core/anim.h"
#include "ui/ui.h"
#include "study/scheduler.h"
#include "rlgl.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

typedef enum {
    PHASE_ENTER = 0,
    PHASE_IDLE,
    PHASE_REVEAL,
    PHASE_RATED
} StudyPhase;

typedef struct StudyUI {
    bool  show_exit;
    float exit_dialog_t;

    StudyPhase phase;
    float      phase_t;
    int        queued_rating;

    /* Card motion */
    float enter_x;         /* + = off-screen right */
    float enter_alpha;
    float exit_x, exit_y, exit_rot, exit_alpha;
    int   exit_dir_x, exit_dir_y;

    /* Mouse tilt */
    float tilt_x, tilt_y;

    /* Reveal stagger */
    float rev_word, rev_reading, rev_divider, rev_meaning, rev_example;

    /* Button springs */
    Spring btn_scale[4];
    float  btn_glow[4];

    /* Combo */
    int   combo;
    int   combo_best;
    float combo_pulse;
    Spring combo_scale;

    /* Progress number pulse */
    float prog_pulse;
    int   prog_last;
} StudyUI;

static StudyUI S;

void study_reset_ui(void) {
    memset(&S, 0, sizeof(S));
    for (int i = 0; i < 4; i++)
        spring_init(&S.btn_scale[i], 1.f, 280.f, 16.f);
    spring_init(&S.combo_scale, 1.f, 300.f, 14.f);
    S.phase = PHASE_ENTER;
    S.phase_t = 0.f;
    S.enter_x = 520.f;
    S.enter_alpha = 0.f;
}

/* ---------------- helpers ---------------- */

static void rating_reward(Rating r, int *xp, int *coins) {
    switch (r) {
        case RATING_AGAIN: *xp =  1; *coins = 0; break;
        case RATING_HARD:  *xp =  5; *coins = 1; break;
        case RATING_GOOD:  *xp = 10; *coins = 2; break;
        case RATING_EASY:  *xp = 15; *coins = 3; break;
    }
}

static void recompute_mature(App *a) {
    int n = 0;
    const double MATURE = 21.0 * 86400.0;
    for (int i = 0; i < a->data.decks.count; i++) {
        const Deck *d = &a->data.decks.decks[i];
        for (int j = 0; j < d->card_count; j++)
            if (d->cards[j].interval_sec >= MATURE) n++;
    }
    a->data.player.mature_cards = n;
}

/* Button row geometry shared by update and draw */
static void rating_button_rects(int W, int H, Rectangle out[4]) {
    float bw = 160.f, gap = 18.f;
    float total = bw * 4.f + gap * 3.f;
    float sx = (W - total) / 2.f;
    float by = H - 130.f;
    for (int i = 0; i < 4; i++)
        out[i] = (Rectangle){ sx + i * (bw + gap), by, bw, 64.f };
}

static Color rating_color(int rating) {
    switch (rating) {
        case 0: return TH.danger;
        case 1: return TH.warning;
        case 2: return TH.primary;
        case 3: return TH.success;
    }
    return TH.primary;
}

static const char *rating_label(int rating) {
    switch (rating) {
        case 0: return "AGAIN";
        case 1: return "HARD";
        case 2: return "GOOD";
        case 3: return "EASY";
    }
    return "?";
}

static void set_phase(App *a, StudyPhase p) {
    (void)a;
    S.phase = p;
    S.phase_t = 0.f;

    if (p == PHASE_ENTER) {
        S.enter_x = 520.f;
        S.enter_alpha = 0.f;
        /* Reset exit state — otherwise the new card is drawn off-screen
           at the previous card's final exit position. */
        S.exit_x = 0.f;
        S.exit_y = 0.f;
        S.exit_rot = 0.f;
        S.exit_alpha = 1.f;
    } else if (p == PHASE_REVEAL) {
        S.rev_word = S.rev_reading = S.rev_divider = 0.f;
        S.rev_meaning = S.rev_example = 0.f;
    } else if (p == PHASE_RATED) {
        switch (S.queued_rating) {
            case 0: S.exit_dir_x = 0; S.exit_dir_y = 1; break;
            case 1: S.exit_dir_x = 1; S.exit_dir_y = 0; break;
            case 2: S.exit_dir_x = 1; S.exit_dir_y = -1; break;
            case 3: S.exit_dir_x = 1; S.exit_dir_y = -2; break;
        }
    }
}

/* ---------------- advance / rating ---------------- */

static void advance(App *a) {
    StudySession *s = &a->session;
    s->current++;
    s->revealed = false;
    if (s->current >= s->queue_len) {
        a->data.player.total_sessions++;
        app_save(a);
        app_goto(a, SCREEN_SESSION_COMPLETE);
        return;
    }
    set_phase(a, PHASE_ENTER);
}

static void apply_rating_and_start_exit(App *a, int rating) {
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    if (!d) return;

    int card_idx = s->queue[s->current];
    Card *c = &d->cards[card_idx];
    double now = (double)time(NULL);
    bool was_new = (c->state == CARD_NEW);

    scheduler_apply(c, (Rating)rating, now);
    player_on_review(&a->data.player, rating, was_new);
    recompute_mature(a);

    int xp, coins;
    rating_reward((Rating)rating, &xp, &coins);
    s->xp_gained += xp;
    s->coins_gained += coins;
    s->session_done++;

    int lvl_before = a->data.player.level;
    player_add_xp(&a->data.player, xp);
    player_add_coins(&a->data.player, coins);
    if (a->data.player.level > lvl_before) {
        a->levelup_to = a->data.player.level;
        a->levelup_t = 2.6f;
        shake_add(&a->shake, 12.f);
        /* Celebration burst at screen centre */
        particles_burst_ring(&a->particles,
            (Vector2){ GetScreenWidth()/2.f, GetScreenHeight()/2.f },
            40, TH.accent, 420.f, 1.1f, P_SHAPE_STAR, 8.f);
    }

    /* Combo tracking */
    if (rating == RATING_AGAIN) {
        S.combo = 0;
    } else if (rating >= RATING_GOOD) {
        S.combo++;
        if (S.combo > S.combo_best) S.combo_best = S.combo;
        S.combo_pulse = 1.f;
        spring_set(&S.combo_scale, 1.35f);
    }

    /* Button position for particle + flight origin */
    Rectangle btns[4];
    rating_button_rects(GetScreenWidth(), GetScreenHeight(), btns);
    Vector2 emit = {
        btns[rating].x + btns[rating].width/2.f,
        btns[rating].y + btns[rating].height/2.f
    };

    /* Buttons respond */
    spring_snap(&S.btn_scale[rating], 0.86f);

    /* Particles from button */
    Color col = rating_color(rating);
    particles_burst_shaped(&a->particles, emit, 28, col,
                           140.f, 380.f, 0.75f,
                           P_SHAPE_CIRCLE, 3.f, 7.f, 0.f, 420.f);
    particles_burst_ring(&a->particles, emit, 14, col,
                         320.f, 0.5f, P_SHAPE_SPARK, 10.f);

    /* Reward flight — travels to the top-right session XP counter */
    int W = GetScreenWidth();
    Vector2 target = { W - 52.f, 46.f };
    char txt[32];
    snprintf(txt, sizeof(txt), "+%d XP", xp);
    app_spawn_flight(a, emit, target, txt, TH.success);

    /* Achievements */
    int newly[NUM_ACHIEVEMENTS];
    int n = player_check_achievements(&a->data.player, newly, NUM_ACHIEVEMENTS);
    if (n > 0) {
        int idx = newly[0];
        snprintf(a->ach_popup, sizeof(a->ach_popup), "%s", ACH_DEFS[idx].name);
        snprintf(a->ach_popup_desc, sizeof(a->ach_popup_desc), "%s", ACH_DEFS[idx].desc);
        a->ach_popup_t = 3.f;
    }

    app_save(a);

    /* Card reaction shake — small */
    shake_add(&a->shake, rating == 0 ? 5.f : 2.f);

    S.queued_rating = rating;
    set_phase(a, PHASE_RATED);
}

/* ---------------- update ---------------- */

void study_update(App *a, float dt) {
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    if (!d || s->current >= s->queue_len) return;

    S.phase_t += dt;

    /* Progress number pulse decay */
    if (S.prog_pulse > 0.f) {
        S.prog_pulse -= dt * 3.f;
        if (S.prog_pulse < 0.f) S.prog_pulse = 0.f;
    }
    if (S.combo_pulse > 0.f) {
        S.combo_pulse -= dt * 2.f;
        if (S.combo_pulse < 0.f) S.combo_pulse = 0.f;
    }

    /* Update button springs */
    for (int i = 0; i < 4; i++) spring_update(&S.btn_scale[i], dt);
    spring_update(&S.combo_scale, dt);

    /* Mouse tilt */
    Vector2 m = GetMousePosition();
    float cx = GetScreenWidth()  / 2.f;
    float cy = GetScreenHeight() / 2.f - 60.f;
    float dist = sqrtf((m.x - cx)*(m.x - cx) + (m.y - cy)*(m.y - cy));
    float proximity = 1.f - fminf(1.f, dist / 520.f);
    proximity = proximity * proximity;
    float tx = (m.x - cx) / 380.f;
    float ty = (m.y - cy) / 260.f;
    if (tx > 1.f) tx = 1.f; if (tx < -1.f) tx = -1.f;
    if (ty > 1.f) ty = 1.f; if (ty < -1.f) ty = -1.f;
    S.tilt_x += (tx * proximity - S.tilt_x) * anim_clamp01(dt * 8.f);
    S.tilt_y += (ty * proximity - S.tilt_y) * anim_clamp01(dt * 8.f);

    /* Phase machine */
    switch (S.phase) {
        case PHASE_ENTER: {
            float p = anim_clamp01(S.phase_t / 0.42f);
            float e = ease_out_back(p);
            S.enter_x = (1.f - e) * 520.f;
            S.enter_alpha = ease_out_cubic(anim_clamp01(p * 1.4f));
            if (p >= 1.f) set_phase(a, PHASE_IDLE);
            break;
        }
        case PHASE_IDLE:
        case PHASE_REVEAL:
        case PHASE_RATED:
            break;
    }

    if (S.phase == PHASE_REVEAL) {
        float t = S.phase_t;
        S.rev_word    = ease_out_back(anim_clamp01((t - 0.02f) / 0.22f));
        S.rev_reading = ease_out_back(anim_clamp01((t - 0.10f) / 0.22f));
        S.rev_divider = ease_out_cubic(anim_clamp01((t - 0.14f) / 0.20f));
        S.rev_meaning = ease_out_back(anim_clamp01((t - 0.18f) / 0.24f));
        S.rev_example = ease_out_back(anim_clamp01((t - 0.30f) / 0.26f));
    }

    if (S.phase == PHASE_RATED) {
        float p = anim_clamp01(S.phase_t / 0.46f);
        const float ANTICIPATE = 0.22f;
        float ax, ay;
        if (p < ANTICIPATE) {
            float q = ease_out_quad(p / ANTICIPATE);
            ax = -S.exit_dir_x * 26.f * q;
            ay = -S.exit_dir_y * 26.f * q;
        } else {
            float q = ease_in_cubic((p - ANTICIPATE) / (1.f - ANTICIPATE));
            float sx = -S.exit_dir_x * 26.f;
            float sy = -S.exit_dir_y * 26.f;
            ax = sx * (1.f - q) + S.exit_dir_x * 1400.f * q;
            ay = sy * (1.f - q) + S.exit_dir_y * 1000.f * q;
        }
        S.exit_x = ax;
        S.exit_y = ay;
        S.exit_rot = ease_in_cubic(p) * S.exit_dir_x * 22.f;
        S.exit_alpha = p < 0.55f ? 1.f : 1.f - (p - 0.55f) / 0.45f;
        if (p >= 1.f) {
            advance(a);
            return;
        }
    }

    /* Exit dialog catches all input */
    if (S.show_exit) {
        S.exit_dialog_t += dt;
        if (IsKeyPressed(KEY_ESCAPE)) { S.show_exit = false; return; }
        return;
    }

    /* Back button + ESC */
    Rectangle back_btn = { 20, 20, 108, 36 };
    bool back_clicked = ui_button_hover(back_btn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (back_clicked || IsKeyPressed(KEY_ESCAPE)) {
        if (s->session_done > 0) S.show_exit = true;
        else                     app_goto(a, SCREEN_DASHBOARD);
        return;
    }

    if (S.phase == PHASE_ENTER || S.phase == PHASE_RATED) return;

    /* Reveal */
    if (S.phase == PHASE_IDLE) {
        bool rk = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);
        bool rm = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        if (rk || rm) {
            s->revealed = true;
            set_phase(a, PHASE_REVEAL);
            shake_add(&a->shake, 2.5f);
            /* Small compression burst at the card */
            int W = GetScreenWidth(), H = GetScreenHeight();
            particles_burst_ring(&a->particles,
                (Vector2){ W/2.f, H/2.f - 60.f },
                10, TH.primary_hi, 120.f, 0.4f, P_SHAPE_SPARK, 6.f);
        }
        return;
    }

    /* Rating */
    int rating = -1;
    if (IsKeyPressed(KEY_ONE))   rating = RATING_AGAIN;
    if (IsKeyPressed(KEY_TWO))   rating = RATING_HARD;
    if (IsKeyPressed(KEY_THREE)) rating = RATING_GOOD;
    if (IsKeyPressed(KEY_FOUR))  rating = RATING_EASY;

    Rectangle btns[4];
    rating_button_rects(GetScreenWidth(), GetScreenHeight(), btns);
    for (int i = 0; i < 4; i++) {
        bool hover = ui_button_hover(btns[i]);
        float target = hover ? 1.07f : 1.0f;
        spring_set(&S.btn_scale[i], target);
        S.btn_glow[i] += ((hover ? 1.f : 0.f) - S.btn_glow[i]) * anim_clamp01(dt * 7.f);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            rating = i;
            /* Kick the spring so it dips immediately */
            spring_snap(&S.btn_scale[i], 0.86f);
            spring_set(&S.btn_scale[i], 1.0f);
        }
    }

    if (rating >= 0) apply_rating_and_start_exit(a, rating);
}

/* ---------------- draw ---------------- */

static void draw_reveal_content(const Card *c, Rectangle card, float alpha) {
    bool jp = card_is_japanese(c);
    float W = card.width;

    if (jp) {
        /* Word has been drawn already in the front pass.
         * Reading, divider, meaning, example animate in sequence. */
        float reading_y = card.y + 40 + FONT_XL + 24;
        if (S.rev_reading > 0.f) {
            float yy = reading_y + (1.f - S.rev_reading) * 12.f;
            ui_text_center(c->reading[0] ? c->reading : "",
                (Rectangle){ card.x, yy, W, 30 },
                FONT_MD, anim_color_alpha(TH.text_dim, alpha * S.rev_reading));
        }

        float div_y = reading_y + 42.f;
        if (S.rev_divider > 0.f) {
            float dw = (W - 200.f) * S.rev_divider;
            float dx = card.x + W/2.f - dw/2.f;
            DrawLineEx((Vector2){ dx, div_y }, (Vector2){ dx + dw, div_y },
                       1.5f, anim_color_alpha((Color){ 80, 92, 128, 220 },
                                              alpha * S.rev_divider));
        }

        if (S.rev_meaning > 0.f) {
            float yy = div_y + 14.f + (1.f - S.rev_meaning) * 14.f;
            const char *mn = c->meaning[0] ? c->meaning : c->back;
            ui_text_center(mn,
                (Rectangle){ card.x, yy, W, 34 },
                FONT_MD, anim_color_alpha(TH.text, alpha * S.rev_meaning));
        }

        if (c->example[0] && S.rev_example > 0.f) {
            float yy = div_y + 60.f + (1.f - S.rev_example) * 12.f;
            ui_text_center(c->example,
                (Rectangle){ card.x, yy, W, 26 },
                FONT_SM, anim_color_alpha(TH.text_muted, alpha * S.rev_example));
        }
    } else {
        /* Non-Japanese: divider then wrapped answer */
        float div_y = card.y + card.height * 0.42f;
        if (S.rev_divider > 0.f) {
            float dw = (W - 100.f) * S.rev_divider;
            float dx = card.x + W/2.f - dw/2.f;
            DrawLineEx((Vector2){ dx, div_y }, (Vector2){ dx + dw, div_y },
                       1.f, anim_color_alpha((Color){ 80, 92, 128, 220 },
                                             alpha * S.rev_divider));
        }
        if (S.rev_meaning > 0.f) {
            ui_text_wrapped(c->back,
                (Rectangle){ card.x + 44, div_y + 20 + (1.f - S.rev_meaning) * 16.f,
                             W - 88, card.height - (div_y - card.y) - 40.f },
                FONT_MD, anim_color_alpha(TH.text_dim, alpha * S.rev_meaning), 8);
        }
    }
}

void study_draw(App *a) {
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    if (!d) return;
    if (s->current >= s->queue_len) return;

    int W = GetScreenWidth(), H = GetScreenHeight();
    double now = (double)time(NULL);

    /* Ambient background */
    anim_draw_ambient_bg(W, H, (float)GetTime());

    /* -------- Back / Exit button -------- */
    Rectangle back_btn = { 20, 20, 108, 36 };
    bool back_hover = ui_button_hover(back_btn) && !S.show_exit;
    Color bb = back_hover ? TH.panel_hi : TH.panel;
    if (back_hover) {
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 6.f);
        bb = ui_lerp_color(bb, TH.primary_lo, 0.25f * pulse);
    }
    ui_panel_border(back_btn, bb, TH.border, RADIUS_MD, 1.f);
    ui_text_center("< EXIT", back_btn, FONT_XS, TH.text_dim);

    /* -------- Header -------- */
    ui_text(d->name, 148, 26, FONT_MD, TH.text);

    /* Progress counter — pulse when the pulse value is high */
    char prog[32];
    snprintf(prog, sizeof(prog), "%d / %d", s->current + 1, s->queue_len);
    int base_size = FONT_MD;
    int prog_size = base_size + (int)(10.f * S.prog_pulse);
    Color prog_col = anim_color_lerp(TH.text_dim, TH.text, S.prog_pulse);
    int tw = ui_measure(prog, prog_size);
    ui_text(prog, W - tw - 24, 26 - (prog_size - base_size)/2, prog_size, prog_col);

    /* Session XP counter (flight target) — pulses when a flight arrives */
    float pulse = a->xp_pulse;
    int xp_size = FONT_SM + (int)(6.f * pulse);
    Color xp_col = anim_color_lerp(TH.success, TH.accent, pulse);
    char xpbuf[64];
    snprintf(xpbuf, sizeof(xpbuf), "Session XP  %d", s->xp_gained);
    int xpw = ui_measure(xpbuf, xp_size);
    ui_text(xpbuf, W - tw - 40 - xpw, 32 - (xp_size - FONT_SM)/2, xp_size, xp_col);

    /* -------- Card -------- */
    Card *c = &d->cards[s->queue[s->current]];
    bool jp = card_is_japanese(c);

    float cw = jp ? 780.f : 740.f;
    if (cw > W - 80) cw = W - 80;

    /* Card height depends on phase */
    float base_front_h = jp ? 300.f : 260.f;
    float reveal_h = jp ? 260.f : 320.f;
    float total_h = base_front_h + (s->revealed ? reveal_h : 0.f);

    /* Center of the card */
    float cx = W/2.f + S.enter_x + S.exit_x + S.tilt_x * 14.f;
    float cy = H/2.f - 90.f + S.exit_y + S.tilt_y * 12.f;

    /* Scale by phase */
    float scale = 1.f;
    if (S.phase == PHASE_ENTER) {
        float p = anim_clamp01(S.phase_t / 0.42f);
        float e = ease_out_back(p);
        scale = 0.82f + 0.18f * e;
    } else if (S.phase == PHASE_REVEAL) {
        /* Compression on reveal, then settle */
        float p = anim_clamp01(S.phase_t / 0.35f);
        scale = 1.f - 0.05f * (1.f - ease_out_cubic(p));
        /* Add a small overshoot bump */
        scale += 0.015f * sinf(p * PI * 2.f) * (1.f - p);
    } else if (S.phase == PHASE_RATED) {
        float p = anim_clamp01(S.phase_t / 0.46f);
        if (p < 0.3f) scale = 1.f + 0.055f * ease_out_back(p / 0.3f);
        else          scale = 1.f + 0.055f * (1.f - ease_in_cubic((p - 0.3f) / 0.7f));
    }

    float rot = S.exit_rot + S.tilt_x * 4.f;
    float alpha = S.phase == PHASE_ENTER ? S.enter_alpha
                 : S.phase == PHASE_RATED ? S.exit_alpha : 1.f;

    float cw_s = cw * scale;
    float ch_s = total_h * scale;
    Rectangle card = { cx - cw_s/2.f, cy - ch_s/2.f, cw_s, ch_s };

    /* Draw rotated via rlgl. Text drawn inside inherits the transform. */
    rlPushMatrix();
    rlTranslatef(card.x + card.width/2.f, card.y + card.height/2.f, 0.f);
    rlRotatef(rot, 0.f, 0.f, 1.f);
    rlTranslatef(-(card.x + card.width/2.f), -(card.y + card.height/2.f), 0.f);

    /* Shadow */
    Rectangle sh = card; sh.x += 10.f; sh.y += 14.f;
    ui_panel(sh, (Color){ 0, 0, 0, (unsigned char)(110 * alpha) }, RADIUS_LG);

    /* Body */
    ui_panel_gradient(card,
                      anim_color_alpha(TH.panel_hi, alpha),
                      anim_color_alpha(TH.panel,    alpha),
                      RADIUS_LG);

    /* Border glow as reveal progresses */
    Color border = TH.border_hi;
    if (S.rev_word > 0.f)
        border = anim_color_lerp(border, TH.primary_hi, S.rev_word * 0.6f);
    ui_outline(card, 0.14f, 12, 2.f, anim_color_alpha(border, alpha));

    /* --- Card content --- */
    if (jp) {
        /* Japanese word — dominant, springs in */
        float w_pop = S.phase == PHASE_ENTER ? 1.f
                    : S.phase == PHASE_REVEAL ? ease_out_back(anim_clamp01(S.phase_t / 0.28f))
                    : 1.f;
        int ws = (int)(FONT_XL * (0.82f + 0.18f * w_pop));
        const char *word = c->japanese[0] ? c->japanese : c->front;
        float wy = card.y + 40.f + (1.f - w_pop) * 14.f;
        ui_text_center(word,
            (Rectangle){ card.x, wy, card.width, (float)FONT_XL + 10 },
            ws, anim_color_alpha(TH.text, alpha));
    } else {
        ui_text_wrapped(c->front,
            (Rectangle){ card.x + 44, card.y + 40, card.width - 88, 140 },
            FONT_MD, anim_color_alpha(TH.text, alpha), 8);
    }

    if (s->revealed) {
        draw_reveal_content(c, card, alpha);
    }

    /* Combo indicator — centered above the card, scales on each increment */
    if (S.combo >= 2 && S.phase != PHASE_RATED) {
        float cs = S.combo_scale.value;
        int combo_size = (int)(FONT_MD * cs);
        Color cc = S.combo >= 5 ? TH.accent :
                   S.combo >= 3 ? TH.primary_hi : TH.text_dim;
        cc = anim_color_alpha(cc, alpha);
        char cb[32];
        snprintf(cb, sizeof(cb), "COMBO x%d", S.combo);
        int cw2 = ui_measure(cb, combo_size);
        ui_text(cb, (int)(cx - cw2/2), (int)(card.y - 44), combo_size, cc);
    }

    rlPopMatrix();

    /* -------- Reveal button (IDLE) -------- */
    if (S.phase == PHASE_IDLE) {
        Rectangle rb = { (W - 220) / 2.f, card.y + card.height + 30.f, 220.f, 60.f };
        bool hov = ui_button_hover(rb);
        float pulse = 0.5f + 0.5f * sinf((float)GetTime() * 2.4f);
        Color fill = hov ? TH.primary_hi
                         : anim_color_lerp(TH.primary, TH.primary_hi, 0.15f * pulse);
        ui_panel_border(rb, fill, ui_lerp_color(fill, (Color){255,255,255,255}, 0.2f),
                        RADIUS_MD, 1.f);
        ui_text_center("REVEAL  [Space]", rb, FONT_MD, TH.text);
    }

    /* -------- Rating buttons -------- */
    if (S.phase == PHASE_REVEAL) {
        Rectangle btns[4];
        rating_button_rects(W, H, btns);
        for (int i = 0; i < 4; i++) {
            float s = S.btn_scale[i].value;
            float glow = S.btn_glow[i];
            Rectangle r = btns[i];
            float dw = r.width  * (1.f - s) * 0.5f;
            float dh = r.height * (1.f - s) * 0.5f;
            Rectangle dr = { r.x + dw, r.y + dh, r.width * s, r.height * s };

            Color base = rating_color(i);

            /* Hover glow behind */
            if (glow > 0.01f) {
                Color gc = base;
                gc.a = (unsigned char)(100 * glow);
                Rectangle gr = dr;
                gr.x -= 6.f; gr.y -= 6.f;
                gr.width += 12.f; gr.height += 12.f;
                ui_panel(gr, gc, RADIUS_MD + 6.f);
            }

            Color fill = base;
            if (glow > 0.f) fill = anim_color_lerp(fill, (Color){255,255,255,255}, 0.12f * glow);
            ui_panel_border(dr, fill,
                            ui_lerp_color(base, (Color){255,255,255,255}, 0.24f),
                            RADIUS_MD, 1.2f);

            double iv = scheduler_next_interval(c, (Rating)i, now);
            char ivbuf[16];
            scheduler_format_interval(iv, ivbuf, sizeof(ivbuf));
            ui_text_center(rating_label(i),
                (Rectangle){ dr.x, dr.y + 8, dr.width, 22 }, FONT_SM, TH.text);
            ui_text_center(ivbuf,
                (Rectangle){ dr.x, dr.y + 34, dr.width, 22 }, FONT_XS, TH.text_muted);
        }
    }

    /* -------- Exit confirmation -------- */
    if (S.show_exit) {
        float dim = ease_out_cubic(anim_clamp01(S.exit_dialog_t / 0.15f));
        DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, (unsigned char)(180 * dim) });

        Rectangle m = { W/2.f - 260, H/2.f - 130, 520, 260 };
        ui_panel_border(m, TH.panel, TH.border_hi, RADIUS_LG, 2.f);

        ui_text_center("Leave this study session?",
            (Rectangle){ m.x, m.y + 28, m.width, 32 }, FONT_MD, TH.text);
        ui_text_center("Your completed reviews will be saved.",
            (Rectangle){ m.x, m.y + 74, m.width, 24 }, FONT_SM, TH.text_dim);

        Rectangle btn_cont = { m.x + 36,  m.y + 160, 210, 56 };
        Rectangle btn_exit = { m.x + 274, m.y + 160, 210, 56 };

        if (ui_button(btn_cont, "CONTINUE STUDYING", TH.primary, TH.text))
            S.show_exit = false;
        if (ui_button(btn_exit, "EXIT SESSION", (Color){ 100, 40, 60, 255 }, TH.text)) {
            S.show_exit = false;
            app_goto(a, SCREEN_DASHBOARD);
        }

        if (IsKeyPressed(KEY_ENTER)) S.show_exit = false;
    }
}
