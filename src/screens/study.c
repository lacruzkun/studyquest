#include "screens.h"
#include "core/theme.h"
#include "core/anim.h"
#include "core/assets.h"
#include "ui/ui.h"
#include "study/scheduler.h"
#include "rlgl.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Local UI state                                                     */
/* ------------------------------------------------------------------ */

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
    int        audio_ref_index;

    /* Card motion */
    float enter_x;
    float enter_alpha;
    float exit_x, exit_y, exit_rot, exit_alpha;
    int   exit_dir_x, exit_dir_y;

    /* Mouse tilt */
    float tilt_x, tilt_y;

    /* Reveal stagger — one value per field, plus the divider. */
    float rev_field[MAX_FIELDS];
    float rev_divider;

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
    S.audio_ref_index = 0;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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

static int card_media_count(const Card *c, int kind, bool revealed) {
    if (!c) return 0;
    int n = 0;
    for (int i = 0; i < c->media_ref_count; i++) {
        if (c->media_refs[i].kind != (unsigned char)kind) continue;
        if (!revealed && c->media_refs[i].side != 0) continue;
        n++;
    }
    if (n == 0 && kind == 0 && c->image_ref[0] && (revealed || c->media_ref_count == 0)) return 1;
    if (n == 0 && kind == 1 && c->audio_ref[0] && (revealed || c->media_ref_count == 0)) return 1;
    return n;
}

static const char *card_media_at(const Card *c, int kind, bool revealed, int wanted_index) {
    if (!c || wanted_index < 0) return NULL;
    int n = 0;
    for (int i = 0; i < c->media_ref_count; i++) {
        if (c->media_refs[i].kind != (unsigned char)kind) continue;
        if (!revealed && c->media_refs[i].side != 0) continue;
        if (n++ == wanted_index) return c->media_refs[i].ref;
    }
    if (n == 0 && kind == 0 && c->image_ref[0] && (revealed || c->media_ref_count == 0) && wanted_index == 0)
        return c->image_ref;
    if (n == 0 && kind == 1 && c->audio_ref[0] && (revealed || c->media_ref_count == 0) && wanted_index == 0)
        return c->audio_ref;
    return NULL;
}

static const char *card_first_audio(const Card *c, bool revealed) {
    return card_media_at(c, 1, revealed, 0);
}

static void set_phase(App *a, StudyPhase p) {
    (void)a;
    S.phase = p;
    S.phase_t = 0.f;

    if (p == PHASE_ENTER) {
        S.audio_ref_index = 0;
        S.enter_x = 520.f;
        S.enter_alpha = 0.f;
        S.exit_x = 0.f;
        S.exit_y = 0.f;
        S.exit_rot = 0.f;
        S.exit_alpha = 1.f;
    } else if (p == PHASE_REVEAL) {
        S.audio_ref_index = 0;
        for (int i = 0; i < MAX_FIELDS; i++) S.rev_field[i] = 0.f;
        S.rev_divider = 0.f;
    } else if (p == PHASE_RATED) {
        switch (S.queued_rating) {
            case 0: S.exit_dir_x = 0; S.exit_dir_y = 1; break;
            case 1: S.exit_dir_x = 1; S.exit_dir_y = 0; break;
            case 2: S.exit_dir_x = 1; S.exit_dir_y = -1; break;
            case 3: S.exit_dir_x = 1; S.exit_dir_y = -2; break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Advance / rating                                                   */
/* ------------------------------------------------------------------ */

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
        particles_burst_ring(&a->particles,
            (Vector2){ GetScreenWidth()/2.f, GetScreenHeight()/2.f },
            40, TH.accent, 420.f, 1.1f, P_SHAPE_STAR, 8.f);
    }

    if (rating == RATING_AGAIN) {
        S.combo = 0;
    } else if (rating >= RATING_GOOD) {
        S.combo++;
        if (S.combo > S.combo_best) S.combo_best = S.combo;
        S.combo_pulse = 1.f;
        spring_set(&S.combo_scale, 1.35f);
    }

    Rectangle btns[4];
    rating_button_rects(GetScreenWidth(), GetScreenHeight(), btns);
    Vector2 emit = {
        btns[rating].x + btns[rating].width/2.f,
        btns[rating].y + btns[rating].height/2.f
    };

    spring_snap(&S.btn_scale[rating], 0.86f);

    Color col = rating_color(rating);
    particles_burst_shaped(&a->particles, emit, 28, col,
                           140.f, 380.f, 0.75f,
                           P_SHAPE_CIRCLE, 3.f, 7.f, 0.f, 420.f);
    particles_burst_ring(&a->particles, emit, 14, col,
                         320.f, 0.5f, P_SHAPE_SPARK, 10.f);

    int W = GetScreenWidth();
    Vector2 target = { W - 52.f, 46.f };
    char txt[32];
    snprintf(txt, sizeof(txt), "+%d XP", xp);
    app_spawn_flight(a, emit, target, txt, TH.success);

    int newly[NUM_ACHIEVEMENTS];
    int n = player_check_achievements(&a->data.player, newly, NUM_ACHIEVEMENTS);
    if (n > 0) {
        int idx = newly[0];
        snprintf(a->ach_popup, sizeof(a->ach_popup), "%s", ACH_DEFS[idx].name);
        snprintf(a->ach_popup_desc, sizeof(a->ach_popup_desc), "%s", ACH_DEFS[idx].desc);
        a->ach_popup_t = 3.f;
    }

    app_save(a);
    shake_add(&a->shake, rating == 0 ? 5.f : 2.f);

    S.queued_rating = rating;
    set_phase(a, PHASE_RATED);
}

/* ------------------------------------------------------------------ */
/*  Update                                                             */
/* ------------------------------------------------------------------ */

void study_update(App *a, float dt) {
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    if (!d || s->current >= s->queue_len) return;

    S.phase_t += dt;

    if (S.prog_pulse > 0.f) {
        S.prog_pulse -= dt * 3.f;
        if (S.prog_pulse < 0.f) S.prog_pulse = 0.f;
    }
    if (S.combo_pulse > 0.f) {
        S.combo_pulse -= dt * 2.f;
        if (S.combo_pulse < 0.f) S.combo_pulse = 0.f;
    }

    for (int i = 0; i < 4; i++) spring_update(&S.btn_scale[i], dt);
    spring_update(&S.combo_scale, dt);

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
        for (int i = 1; i < MAX_FIELDS; i++) {
            float start = 0.04f + (i - 1) * 0.09f;
            S.rev_field[i] = ease_out_back(anim_clamp01((t - start) / 0.24f));
        }
        S.rev_field[0] = 1.f;
        S.rev_divider = ease_out_cubic(anim_clamp01((t - 0.14f) / 0.20f));
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

    if (S.show_exit) {
        S.exit_dialog_t += dt;
        if (IsKeyPressed(KEY_ESCAPE)) { S.show_exit = false; return; }
        return;
    }

    Rectangle back_btn = { 20, 20, 108, 36 };
    bool back_clicked = ui_button_hover(back_btn) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (back_clicked || IsKeyPressed(KEY_ESCAPE)) {
        if (s->session_done > 0) S.show_exit = true;
        else                     app_goto(a, SCREEN_DASHBOARD);
        return;
    }

    if (S.phase == PHASE_ENTER || S.phase == PHASE_RATED) return;

    if (S.phase == PHASE_IDLE) {
        bool rk = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER);
        bool rm = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
        if (rk || rm) {
            s->revealed = true;
            set_phase(a, PHASE_REVEAL);
            shake_add(&a->shake, 2.5f);
            int W = GetScreenWidth(), H = GetScreenHeight();
            particles_burst_ring(&a->particles,
                (Vector2){ W/2.f, H/2.f - 60.f },
                10, TH.primary_hi, 120.f, 0.4f, P_SHAPE_SPARK, 6.f);

            Card *cur = &d->cards[s->queue[s->current]];
            const char *audio = card_first_audio(cur, true);
            if (audio)
                assets_play_sound(&a->assets, audio);
        }
        return;
    }

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
            spring_snap(&S.btn_scale[i], 0.86f);
            spring_set(&S.btn_scale[i], 1.0f);
        }
    }

    if (rating >= 0) apply_rating_and_start_exit(a, rating);
}

/* ------------------------------------------------------------------ */
/*  Draw helpers                                                       */
/* ------------------------------------------------------------------ */

static void draw_card_content(const Card *c, Rectangle card, float alpha) {
    bool jp = card_is_japanese(c);
    float W = card.width;

    bool revealed = (S.phase == PHASE_REVEAL || S.phase == PHASE_RATED);

    const char *prompt = (c->field_count > 0) ? c->field_values[0] : c->front;
    if (!prompt) prompt = "";

    if (jp && c->field_count > 0) {
        float w_pop = (S.phase == PHASE_REVEAL)
            ? ease_out_back(anim_clamp01(S.phase_t / 0.28f))
            : 1.f;
        int ws = (int)(FONT_XL * (0.82f + 0.18f * w_pop));
        float wy = card.y + 40.f + (1.f - w_pop) * 14.f;
        ui_text_center(prompt,
            (Rectangle){ card.x, wy, W, (float)FONT_XL + 10 },
            ws, anim_color_alpha(TH.text, alpha));

        if (!revealed) return;

        float y = card.y + 40.f + FONT_XL + 24.f;
        bool divider_drawn = false;

        for (int i = 1; i < c->field_count; i++) {
            if (!c->field_values[i][0]) continue;
            float rev = S.rev_field[i];
            if (rev <= 0.f) continue;

            float ry = y + (1.f - rev) * 10.f;
            int  sz  = (i == 1) ? FONT_MD : FONT_SM;
            Color col = (i == 1) ? TH.text_dim : TH.text_muted;

            ui_text_center(c->field_values[i],
                (Rectangle){ card.x + 20, ry, W - 40, 32 },
                sz, anim_color_alpha(col, alpha * rev));
            y += 34.f;

            if (i == 1 && !divider_drawn && S.rev_divider > 0.f) {
                float dw = (W - 200.f) * S.rev_divider;
                float dx = card.x + W/2.f - dw/2.f;
                DrawLineEx((Vector2){ dx, y }, (Vector2){ dx + dw, y },
                           1.5f, anim_color_alpha((Color){ 80, 92, 128, 220 },
                                                  alpha * S.rev_divider));
                y += 14.f;
                divider_drawn = true;
            }
        }
    } else {
        float prompt_bottom = ui_text_rich_ex(prompt,
            (Rectangle){ card.x + 44, card.y + 40, W - 88, 200 },
            FONT_MD, anim_color_alpha(TH.text, alpha), 8);

        if (!revealed) return;

        float div_y = prompt_bottom + 16.f;
        float min_div = card.y + 200.f;
        if (div_y < min_div) div_y = min_div;

        if (S.rev_divider > 0.f) {
            float dw = (W - 100.f) * S.rev_divider;
            float dx = card.x + W/2.f - dw/2.f;
            DrawLineEx((Vector2){ dx, div_y }, (Vector2){ dx + dw, div_y },
                       1.f, anim_color_alpha((Color){ 80, 92, 128, 220 },
                                             alpha * S.rev_divider));
        }

        float y = div_y + 18.f;
        for (int i = 1; i < c->field_count; i++) {
            if (!c->field_values[i][0]) continue;
            float rev = S.rev_field[i];
            if (rev <= 0.f) continue;

            float ry = y + (1.f - rev) * 10.f;

            const char *fname = c->field_names[i];
            if (fname && fname[0] && strcasecmp(fname, "Back") != 0) {
                ui_text(fname,
                    (int)card.x + 44, (int)ry, FONT_XS,
                    anim_color_alpha(TH.text_muted, alpha * rev));
                ry += 20.f;
            }

            float bottom = ui_text_rich_ex(c->field_values[i],
                (Rectangle){ card.x + 44, ry, W - 88, 200 },
                FONT_SM, anim_color_alpha(TH.text_dim, alpha * rev), 6);

            y = bottom + 12.f;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */

void study_draw(App *a) {
    StudySession *s = &a->session;
    Deck *d = decklist_find(&a->data.decks, s->deck_id);
    if (!d) return;
    if (s->current >= s->queue_len) return;

    int W = GetScreenWidth(), H = GetScreenHeight();
    double now = (double)time(NULL);

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

    char prog[32];
    snprintf(prog, sizeof(prog), "%d / %d", s->current + 1, s->queue_len);
    int base_size = FONT_MD;
    int prog_size = base_size + (int)(10.f * S.prog_pulse);
    Color prog_col = anim_color_lerp(TH.text_dim, TH.text, S.prog_pulse);
    int tw = ui_measure(prog, prog_size);
    ui_text(prog, W - tw - 24, 26 - (prog_size - base_size)/2, prog_size, prog_col);

    float pulse = a->xp_pulse;
    int xp_size = FONT_SM + (int)(6.f * pulse);
    Color xp_col = anim_color_lerp(TH.success, TH.accent, pulse);
    char xpbuf[64];
    snprintf(xpbuf, sizeof(xpbuf), "Session XP  %d", s->xp_gained);
    int xpw = ui_measure(xpbuf, xp_size);
    ui_text(xpbuf, W - tw - 40 - xpw, 32 - (xp_size - FONT_SM)/2, xp_size, xp_col);

    /* -------- Card geometry -------- */
    Card *c = &d->cards[s->queue[s->current]];
    bool jp = card_is_japanese(c);
    bool revealed = s->revealed || S.phase == PHASE_REVEAL || S.phase == PHASE_RATED;
    const char *image_ref = card_media_at(c, 0, revealed, 0);
    bool has_image = image_ref != NULL;

    float cw = jp ? 780.f : 740.f;
    if (cw > W - 80) cw = W - 80;

    float image_slot_h = has_image ? 200.f : 0.f;

    float base_front_h = (jp ? 300.f : 260.f) + image_slot_h;
    float reveal_h = jp ? 260.f : 320.f;
    if (c->field_count > 3) reveal_h += (c->field_count - 3) * 26.f;
    float total_h = base_front_h + (s->revealed ? reveal_h : 0.f);

    float cx = W/2.f + S.enter_x + S.exit_x + S.tilt_x * 14.f;
    float cy = H/2.f - 90.f + S.exit_y + S.tilt_y * 12.f;

    float scale = 1.f;
    if (S.phase == PHASE_ENTER) {
        float p = anim_clamp01(S.phase_t / 0.42f);
        float e = ease_out_back(p);
        scale = 0.82f + 0.18f * e;
    } else if (S.phase == PHASE_REVEAL) {
        float p = anim_clamp01(S.phase_t / 0.35f);
        scale = 1.f - 0.05f * (1.f - ease_out_cubic(p));
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

    rlPushMatrix();
    rlTranslatef(card.x + card.width/2.f, card.y + card.height/2.f, 0.f);
    rlRotatef(rot, 0.f, 0.f, 1.f);
    rlTranslatef(-(card.x + card.width/2.f), -(card.y + card.height/2.f), 0.f);

    Rectangle sh = card; sh.x += 10.f; sh.y += 14.f;
    ui_panel(sh, (Color){ 0, 0, 0, (unsigned char)(110 * alpha) }, RADIUS_LG);

    ui_panel_gradient(card,
                      anim_color_alpha(TH.panel_hi, alpha),
                      anim_color_alpha(TH.panel,    alpha),
                      RADIUS_LG);

    Color border = TH.border_hi;
    if (S.rev_divider > 0.f)
        border = anim_color_lerp(border, TH.primary_hi, S.rev_divider * 0.6f);
    ui_outline(card, 0.14f, 12, 2.f, anim_color_alpha(border, alpha));

    if (has_image && alpha > 0.01f) {
        Texture2D *t = assets_get_texture(&a->assets, image_ref);
        if (t && t->id != 0) {
            float avail_w = card.width - 80.f;
            float avail_h = image_slot_h - 20.f;
            float aspect = (float)t->width / (float)t->height;
            float draw_w = avail_w;
            float draw_h = draw_w / aspect;
            if (draw_h > avail_h) {
                draw_h = avail_h;
                draw_w = draw_h * aspect;
            }
            float img_y = card.y + (jp ? (40.f + FONT_XL + 24.f + 40.f) : 40.f);
            float img_x = card.x + (card.width - draw_w) / 2.f;

            Rectangle shadow = { img_x + 4.f, img_y + 6.f, draw_w, draw_h };
            ui_panel(shadow, (Color){ 0, 0, 0, (unsigned char)(90 * alpha) },
                     RADIUS_SM);

            DrawTexturePro(*t,
                (Rectangle){ 0, 0, (float)t->width, (float)t->height },
                (Rectangle){ img_x, img_y, draw_w, draw_h },
                (Vector2){ 0, 0 }, 0.f,
                anim_color_alpha(WHITE, alpha));

            ui_outline((Rectangle){ img_x, img_y, draw_w, draw_h },
                       0.04f, 6, 1.f,
                       anim_color_alpha(TH.border_hi, alpha));
        }
    }

    draw_card_content(c, card, alpha);

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
        float pulse2 = 0.5f + 0.5f * sinf((float)GetTime() * 2.4f);
        Color fill = hov ? TH.primary_hi
                         : anim_color_lerp(TH.primary, TH.primary_hi, 0.15f * pulse2);
        ui_panel_border(rb, fill, ui_lerp_color(fill, (Color){255,255,255,255}, 0.2f),
                        RADIUS_MD, 1.f);
        ui_text_center("REVEAL  [Space]", rb, FONT_MD, TH.text);
    }

    /* -------- Rating buttons -------- */
    if (S.phase == PHASE_REVEAL) {
        Rectangle btns[4];
        rating_button_rects(W, H, btns);
        for (int i = 0; i < 4; i++) {
            float sc = S.btn_scale[i].value;
            float glow = S.btn_glow[i];
            Rectangle r = btns[i];
            float dw = r.width  * (1.f - sc) * 0.5f;
            float dh = r.height * (1.f - sc) * 0.5f;
            Rectangle dr = { r.x + dw, r.y + dh, r.width * sc, r.height * sc };

            Color base = rating_color(i);

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

    /* -------- Audio replay button (revealed cards only) -------- */
    const int audio_count = card_media_count(c, 1, true);
    const char *selected_audio = card_media_at(c, 1, true, S.audio_ref_index);
    if (s->revealed && audio_count > 0 && selected_audio) {
        Rectangle ab = { card.x + card.width - 52.f, card.y + 12.f, 40.f, 40.f };
        bool ah = ui_button_hover(ab);
        Color ac = ah ? TH.primary : TH.panel_hi;
        ui_panel_border(ab, ac, TH.border, RADIUS_MD, 1.f);

        Vector2 ctr = { ab.x + 20.f, ab.y + 20.f };
        DrawTriangle(
            (Vector2){ ctr.x - 6, ctr.y - 4 },
            (Vector2){ ctr.x - 6, ctr.y + 4 },
            (Vector2){ ctr.x - 1, ctr.y + 8 }, TH.text);
        DrawTriangle(
            (Vector2){ ctr.x - 6, ctr.y - 4 },
            (Vector2){ ctr.x - 1, ctr.y + 8 },
            (Vector2){ ctr.x - 1, ctr.y - 8 }, TH.text);
        DrawRectangle((int)(ctr.x - 1), (int)(ctr.y - 8), 4, 16, TH.text);
        for (int i = 0; i < 2; i++) {
            float r = 6.f + i * 4.f;
            DrawRing(ctr, r, r + 1.5f, -50.f, 50.f, 20, TH.text_dim);
        }

        if (ah && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            assets_play_sound(&a->assets, selected_audio);
            if (audio_count > 1) S.audio_ref_index = (S.audio_ref_index + 1) % audio_count;
            shake_add(&a->shake, 1.f);
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
