#include "screens.h"
#include "core/theme.h"
#include "ui/ui.h"
#include <stdio.h>
#include <string.h>

static int s_confirm = 0; /* 0=none, 1=reset */

void settings_update(App *a, float dt) {
    (void)dt;
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (s_confirm) s_confirm = 0;
        else app_goto(a, SCREEN_DASHBOARD);
    }
    /* Confirm reset with Y */
    if (s_confirm == 1 && IsKeyPressed(KEY_Y)) {
        save_defaults(&a->data);
        app_save(a);
        s_confirm = 0;
        app_toast(a, "Progress reset.", TH.success);
    }
}

static void slider_row(Player *p, Rectangle r, const char *label, float *val) {
    ui_text(label, (int)r.x, (int)r.y - 4, FONT_SM, TH.text_dim);

    Rectangle track = { r.x + 200, r.y, r.width - 260, 20 };
    ui_progress(track, *val, TH.primary, TH.bg, RADIUS_SM);

    Vector2 m = GetMousePosition();
    if (ui_button_hover(track) && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        *val = ui_clamp01((m.x - track.x) / track.width);
    }
    Rectangle knob = { track.x + track.width * (*val) - 8, track.y - 4, 16, track.height + 8 };
    ui_panel(knob, TH.text, 6);

    char buf[16]; snprintf(buf, sizeof(buf), "%d%%", (int)(*val * 100));
    ui_text(buf, (int)(track.x + track.width + 10), (int)track.y, FONT_XS, TH.text_dim);
    (void)p;
}

void settings_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();
    ui_text("SETTINGS", 24, 24, FONT_LG, TH.text);

    Rectangle back = { W - 160.f, 26, 136, 40 };
    if (ui_button(back, "< BACK", TH.panel, TH.text)) app_goto(a, SCREEN_DASHBOARD);

    Player *p = &a->data.player;

    Rectangle panel = { 24, 90, (float)W - 48, (float)H - 160 };
    ui_panel_border(panel, TH.panel, TH.border, RADIUS_LG, 1.f);

    ui_text("AUDIO", (int)panel.x + 24, (int)panel.y + 20, FONT_XS, TH.text_muted);

    float y = panel.y + 60;
    slider_row(p, (Rectangle){ panel.x + 24, y, panel.width - 48, 22 }, "Master volume", &p->master_vol); y += 50;
    slider_row(p, (Rectangle){ panel.x + 24, y, panel.width - 48, 22 }, "Sound effects", &p->sfx_vol);    y += 50;
    slider_row(p, (Rectangle){ panel.x + 24, y, panel.width - 48, 22 }, "Music",         &p->music_vol);  y += 60;

    ui_text("DISPLAY", (int)panel.x + 24, (int)y, FONT_XS, TH.text_muted); y += 30;

    Rectangle fs = { panel.x + 24, y, 180, 40 };
    if (ui_button(fs, p->fullscreen ? "Fullscreen: ON" : "Fullscreen: OFF", TH.panel_hi, TH.text)) {
        p->fullscreen = !p->fullscreen;
        ToggleFullscreen();
        app_save(a);
    } y += 60;

    ui_text("ANIMATION", (int)panel.x + 24, (int)y, FONT_XS, TH.text_muted); y += 30;

    const char *labels[3] = { "FULL", "REDUCED", "OFF" };
    float aw = 140.f;
    for (int i = 0; i < 3; i++) {
        Rectangle ab = { panel.x + 24 + i * (aw + 12), y, aw, 40 };
        bool active = (p->anim_intensity == i);
        Color fill = active ? TH.primary : TH.panel_hi;
        if (ui_button(ab, labels[i], fill, TH.text)) {
            p->anim_intensity = i;
            anim_set_intensity((AnimIntensity)i);
            app_save(a);
        }
    }
    y += 60;
    ui_text("KEYBOARD SHORTCUTS", (int)panel.x + 24, (int)y, FONT_XS, TH.text_muted); y += 26;

    const char *shortcuts[] = {
        "Space / Enter — reveal answer",
        "1 / 2 / 3 / 4 — Again / Hard / Good / Easy",
        "Esc — back / cancel",
        "Ctrl+Enter — save card (in editor)",
        "Tab — next field (in editor)",
    };
    for (int i = 0; i < 5; i++) {
        ui_text(shortcuts[i], (int)panel.x + 24, (int)y, FONT_SM, TH.text_dim);
        y += 22;
    }

    y += 20;
    Rectangle reset = { panel.x + 24, y, 240, 44 };
    if (ui_button(reset, "RESET PROGRESS", TH.danger, TH.text)) {
        s_confirm = 1;
    }

    if (s_confirm) {
        DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 160 });
        Rectangle m = { W/2.f - 240, H/2.f - 90, 480, 180 };
        ui_panel_border(m, TH.panel, TH.danger, RADIUS_LG, 2.f);
        ui_text_center("Reset all progress?", (Rectangle){ m.x, m.y + 30, m.width, 30 }, FONT_MD, TH.danger);
        ui_text_center("This deletes your decks, XP and achievements.",
                       (Rectangle){ m.x, m.y + 70, m.width, 20 }, FONT_SM, TH.text_dim);
        ui_text_center("Press Y to confirm, Esc to cancel.",
                       (Rectangle){ m.x, m.y + 110, m.width, 20 }, FONT_XS, TH.text_muted);
    }
}
