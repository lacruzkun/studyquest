#include "app.h"
#include "core/theme.h"
#include "ui/ui.h"
#include "screens/screens.h"
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

static void app_media_root(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    snprintf(out, cap, "%s/.studyquest/media", home);
}

/* One-time safety copy of a pre-world save before the first v9 write, so a
 * player can always roll back to the previous version of the game. Never
 * overwrites an existing backup. Failure is non-fatal. */
static void backup_legacy_save(const char *path, int ver) {
    if (ver <= 0 || ver >= SAVE_VERSION) return;
    char dst[600];
    snprintf(dst, sizeof(dst), "%s.v%d.bak", path, ver);
    FILE *chk = fopen(dst, "rb");
    if (chk) { fclose(chk); return; }
    FILE *in = fopen(path, "rb");
    if (!in) return;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return; }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(dst);
    else TraceLog(LOG_INFO, "SAVE: backed up v%d save to %s", ver, dst);
}

App *app_create(void) {
    App *a = (App *)RL_CALLOC(1, sizeof(App));
    if (!a) {
        TraceLog(LOG_FATAL, "app_create: OOM (%zu bytes)", sizeof(App));
        return NULL;
    }
    save_default_path(a->save_path, sizeof(a->save_path));

    if (!save_load(&a->data, a->save_path)) {
        save_defaults(&a->data);
        a->screen = SCREEN_WELCOME;
    } else {
        backup_legacy_save(a->save_path, a->data.loaded_version);
        a->screen = SCREEN_DASHBOARD;
    }
    a->prev_screen = a->screen;
    a->screen_t = 0;
    a->transition_style = TRANS_FADE;

    app_refresh_fonts(a);

    char mroot[512];
    app_media_root(mroot, sizeof(mroot));
    assets_init(&a->assets, mroot);

    particles_reset(&a->particles);
    shake_reset(&a->shake);

    /* Apply persisted animation intensity */
    anim_set_intensity((AnimIntensity)a->data.player.anim_intensity);

    player_daily_check(&a->data.player);
    return a;
}

void app_refresh_fonts(App *a) {
    if (!a) return;
    int cp_count = 0;
    int *cp = fonts_collect_from_decks(&a->data.decks, &cp_count);
    fonts_free(&a->fonts);
    fonts_init(&a->fonts, cp, cp_count);
    free(cp);
    ui_set_fonts(&a->fonts);
}

void app_destroy(App *a) {
    app_save(a);
    fonts_free(&a->fonts);
    assets_free(&a->assets);
    free(a->session.queue);
    a->session.queue = NULL;
    decklist_free(&a->data.decks);
    RL_FREE(a);
}

void app_save(App *a) { save_write(&a->data, a->save_path); }
bool app_save_checked(App *a) { return save_write(&a->data, a->save_path); }

void app_goto(App *a, Screen s) {
    if (a->screen == s) return;
    a->prev_screen = a->screen;
    a->screen   = s;
    a->screen_t = 0;

    switch (s) {
        case SCREEN_STUDY:            a->transition_style = TRANS_SLIDE_RIGHT; break;
        case SCREEN_SESSION_COMPLETE: a->transition_style = TRANS_SCALE;       break;
        case SCREEN_DECKS:            a->transition_style = TRANS_SLIDE_LEFT;  break;
        default:                      a->transition_style = TRANS_FADE;        break;
    }
}

void app_toast(App *a, const char *msg, Color c) {
    snprintf(a->toast, sizeof(a->toast), "%s", msg);
    a->toast_t = 1.2f;
    a->toast_color = c;
}

void app_spawn_flight(App *a, Vector2 start, Vector2 end,
                      const char *text, Color color) {
    if (!anim_enabled()) return;
    for (int i = 0; i < MAX_REWARD_FLIGHTS; i++) {
        RewardFlight *f = &a->flights[i];
        if (f->active) continue;
        f->active = true;
        f->start = start;
        f->end = end;
        snprintf(f->text, sizeof(f->text), "%s", text);
        f->color = color;
        f->duration = 0.68f;
        f->elapsed = 0.f;
        f->delay = 0.f;
        return;
    }
}

void app_start_session(App *a, int deck_id) {
    Deck *d = decklist_find(&a->data.decks, deck_id);
    if (!d || d->card_count == 0) { app_toast(a, "Nothing to study.", TH.warning); return; }

    StudySession *s = &a->session;
    free(s->queue);
    memset(s, 0, sizeof(*s));
    s->deck_id = deck_id;

    /* Pre-allocate the queue for the worst case: every card is due. */
    int cap = d->card_count > 0 ? d->card_count : 1;
    s->queue = (int *)malloc(sizeof(int) * (size_t)cap);
    if (!s->queue) { app_toast(a, "Out of memory.", TH.danger); return; }
    s->queue_cap = cap;

    double now = (double)time(NULL);
    for (int i = 0; i < d->card_count; i++)
        if (scheduler_card_due(&d->cards[i], now))
            s->queue[s->queue_len++] = i;

    if (s->queue_len == 0) {
        app_toast(a, "All caught up! Come back later.", TH.success);
        return;
    }
    s->session_total = s->queue_len;
    s->current = 0;
    s->revealed = false;

    a->data.player.session_studied = 0;
    a->data.player.session_good = 0;

    study_reset_ui();
    app_goto(a, SCREEN_STUDY);
}

/* ================================================================ */

static void update_flights(App *a, float dt) {
    if (a->xp_pulse > 0.f) {
        a->xp_pulse -= dt * 2.5f;
        if (a->xp_pulse < 0.f) a->xp_pulse = 0.f;
    }
    for (int i = 0; i < MAX_REWARD_FLIGHTS; i++) {
        RewardFlight *f = &a->flights[i];
        if (!f->active) continue;
        if (f->delay > 0.f) { f->delay -= dt; continue; }
        f->elapsed += dt;
        if (f->elapsed >= f->duration) {
            /* On arrival: burst + pulse target */
            particles_burst_shaped(&a->particles, f->end, 16, f->color,
                                   90.f, 240.f, 0.6f,
                                   P_SHAPE_STAR, 3.f, 6.f, 0.f, 120.f);
            particles_burst_ring(&a->particles, f->end, 10, TH.accent,
                                 220.f, 0.45f, P_SHAPE_SPARK, 8.f);
            a->xp_pulse = 1.f;
            shake_add(&a->shake, 1.5f);
            f->active = false;
        }
    }
}

static void draw_flights(App *a) {
    for (int i = 0; i < MAX_REWARD_FLIGHTS; i++) {
        RewardFlight *f = &a->flights[i];
        if (!f->active || f->delay > 0.f) continue;
        float t = anim_clamp01(f->elapsed / f->duration);
        float eased = ease_in_out_cubic(t);
        float arc = sinf(PI * t) * 100.f;
        Vector2 p = {
            f->start.x + (f->end.x - f->start.x) * eased,
            f->start.y + (f->end.y - f->start.y) * eased - arc
        };
        float sc = 1.f - 0.3f * t;
        float alpha = 1.f;
        if (t < 0.12f) alpha = t / 0.12f;
        else if (t > 0.72f) alpha = 1.f - (t - 0.72f) / 0.28f;

        int fs = (int)(FONT_MD * sc);
        if (fs < 10) fs = 10;
        Color shadow = anim_color_alpha((Color){0,0,0,200}, alpha);
        Color col    = anim_color_alpha(f->color, alpha);
        ui_text(f->text, (int)p.x + 2, (int)p.y + 2, fs, shadow);
        ui_text(f->text, (int)p.x,     (int)p.y,     fs, col);
    }
}

static void draw_levelup(App *a, int W, int H) {
    if (a->levelup_t <= 0.f) return;
    float t = a->levelup_t;
    float elapsed = 2.6f - t;

    float dim = anim_clamp01(elapsed * 2.5f) * anim_clamp01(t * 2.f);
    DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, (unsigned char)(160 * dim) });

    float scl = ease_out_back(anim_clamp01(elapsed * 1.8f));
    float alpha = anim_clamp01(elapsed * 3.f) * anim_clamp01(t * 1.5f);

    int cx = W / 2, cy = H / 2;
    DrawCircle(cx, cy, 170 * scl, (Color){ 108,132,255, (unsigned char)(70 * alpha) });
    DrawCircle(cx, cy, 110 * scl, (Color){ 148,168,255, (unsigned char)(100 * alpha) });

    /* Continuous particle rain during the celebration */
    if (anim_enabled() && GetRandomValue(0, 100) < 45) {
        Vector2 spawn = {
            (float)cx + (float)GetRandomValue(-160, 160),
            (float)cy + (float)GetRandomValue(-120, 120)
        };
        Color pick = (GetRandomValue(0, 2) == 0) ? TH.accent
                   : (GetRandomValue(0, 1) == 0) ? TH.primary_hi : TH.success;
        particles_burst_shaped(&a->particles, spawn, 2, pick,
                               40.f, 140.f, 1.1f,
                               P_SHAPE_STAR, 4.f, 8.f, 0.f, 120.f);
    }

    Color ac = anim_color_alpha(TH.accent, alpha);
    Color tc = anim_color_alpha(TH.text, alpha);
    Color sc = anim_color_alpha(TH.success, alpha);

    ui_text_center("LEVEL UP",
        (Rectangle){ cx - 300, cy - 170, 600, 50 }, FONT_LG, ac);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", a->levelup_to);
    int num_size = (int)(FONT_XL * (0.6f + 0.7f * scl));
    ui_text_center(buf, (Rectangle){ cx - 300, cy - 60, 600, 140 }, num_size, tc);

    ui_text_center("NEW LEVEL",
        (Rectangle){ cx - 300, cy + 80, 600, 28 }, FONT_SM, sc);
    ui_text_center("+100 Coins",
        (Rectangle){ cx - 300, cy + 120, 600, 40 }, FONT_MD, sc);
}

static void draw_achievement_popup(App *a, int W) {
    if (a->ach_popup_t <= 0.f) return;
    float t = a->ach_popup_t;
    float slide = ease_out_back(anim_clamp01((1.f - t) * 4.f));
    float alpha = t < 0.15f ? t / 0.15f : 1.f;

    Rectangle r = {
        (float)W - 360 - (1.f - slide) * 380,
        24.f, 360.f, 96.f
    };
    Color c = TH.panel; c.a = (unsigned char)(240 * alpha);
    ui_panel_border(r, c, anim_color_alpha(TH.accent, alpha), RADIUS_MD, 2.f);

    ui_text("ACHIEVEMENT UNLOCKED", (int)r.x + 16, (int)r.y + 10, FONT_XS,
            anim_color_alpha(TH.accent, alpha));
    ui_text(a->ach_popup, (int)r.x + 16, (int)r.y + 30, FONT_MD,
            anim_color_alpha(TH.text, alpha));
    ui_text_wrapped(a->ach_popup_desc,
                    (Rectangle){ r.x+16, r.y+60, r.width-32, 30 },
                    FONT_XS, anim_color_alpha(TH.text_dim, alpha), 2);
}

static void draw_toast(App *a, int W, int H) {
    if (a->toast_t <= 0.f) return;
    float t = a->toast_t;
    float alpha = t > 0.9f ? (1.2f - t) / 0.3f
                : t < 0.2f ? t / 0.2f : 1.f;
    Color c = a->toast_color;
    c.a = (unsigned char)(255 * anim_clamp01(alpha));
    int w = ui_measure(a->toast, FONT_MD) + 44;
    Rectangle r = { (W - w) / 2.f, H - 96.f, (float)w, 46.f };
    ui_panel(r, (Color){ 0, 0, 0, (unsigned char)(180 * anim_clamp01(alpha)) }, RADIUS_MD);
    ui_text_center(a->toast, r, FONT_MD, c);
}

/* ================================================================ */

void app_update(App *a, float dt) {
    a->dt = dt;
    a->screen_t += dt;

    if (a->toast_t > 0.f)     a->toast_t    -= dt;
    if (a->levelup_t > 0.f)   a->levelup_t  -= dt;
    if (a->ach_popup_t > 0.f) a->ach_popup_t -= dt;

    particles_update(&a->particles, dt);
    shake_update(&a->shake, dt);
    update_flights(a, dt);
    player_daily_check(&a->data.player);

    switch (a->screen) {
        case SCREEN_WELCOME:          welcome_update(a, dt);          break;
        case SCREEN_DASHBOARD:        dashboard_update(a, dt);        break;
        case SCREEN_DECKS:            decks_update(a, dt);            break;
        case SCREEN_STUDY:            study_update(a, dt);            break;
        case SCREEN_SESSION_COMPLETE: session_complete_update(a, dt); break;
        case SCREEN_ACHIEVEMENTS:     achievements_update(a, dt);     break;
        case SCREEN_STATS:            stats_update(a, dt);            break;
        case SCREEN_SETTINGS:         settings_update(a, dt);         break;
    }
}

void app_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    ClearBackground(TH.bg);

    TransEffect tx = trans_evaluate(a->screen_t, TRANS_DEFAULT_DUR,
                                    a->transition_style, W, H);
    Vector2 shk = shake_offset(&a->shake);

    Camera2D cam = {0};
    cam.offset   = (Vector2){ W*0.5f + tx.offset.x + shk.x,
                              H*0.5f + tx.offset.y + shk.y };
    cam.target   = (Vector2){ W*0.5f, H*0.5f };
    cam.rotation = 0.f;
    cam.zoom     = tx.zoom;

    BeginMode2D(cam);
    switch (a->screen) {
        case SCREEN_WELCOME:          welcome_draw(a);          break;
        case SCREEN_DASHBOARD:        dashboard_draw(a);        break;
        case SCREEN_DECKS:            decks_draw(a);            break;
        case SCREEN_STUDY:            study_draw(a);            break;
        case SCREEN_SESSION_COMPLETE: session_complete_draw(a); break;
        case SCREEN_ACHIEVEMENTS:     achievements_draw(a);     break;
        case SCREEN_STATS:            stats_draw(a);            break;
        case SCREEN_SETTINGS:         settings_draw(a);         break;
    }
    particles_draw(&a->particles);
    EndMode2D();

    /* Screen fade overlay */
    if (tx.alpha > 0.001f) {
        DrawRectangle(0, 0, W, H,
            (Color){ TH.bg.r, TH.bg.g, TH.bg.b, (unsigned char)(255.f * tx.alpha) });
    }

    draw_flights(a);
    draw_levelup(a, W, H);
    draw_achievement_popup(a, W);
    draw_toast(a, W, H);
}
