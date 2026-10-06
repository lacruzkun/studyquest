#include "ui.h"
#include "core/theme.h"
#include <string.h>
#include <math.h>

static const FontSet *g_fonts = NULL;

void ui_set_fonts(const FontSet *fs) { g_fonts = fs; }

float ui_clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
float ui_lerp(float a, float b, float t) { return a + (b - a) * t; }
float ui_ease_out(float t) { t = ui_clamp01(t); return 1.f - powf(1.f - t, 3.f); }
float ui_ease_in_out(float t) { t = ui_clamp01(t); return t < 0.5f ? 2*t*t : 1 - powf(-2*t+2,2)/2; }

Color ui_lerp_color(Color a, Color b, float t) {
    t = ui_clamp01(t);
    return (Color){
        (unsigned char)ui_lerp(a.r, b.r, t),
        (unsigned char)ui_lerp(a.g, b.g, t),
        (unsigned char)ui_lerp(a.b, b.b, t),
        (unsigned char)ui_lerp(a.a, b.a, t),
    };
}

static Font pick(int nominal) {
    if (!g_fonts) return GetFontDefault();
    return fonts_pick(g_fonts, nominal);
}

int ui_measure(const char *t, int size) {
    if (!t) return 0;
    return (int)MeasureTextEx(pick(size), t, (float)size, 0.5f).x;
}

/* ------------------------------------------------------------------ */

void ui_panel(Rectangle r, Color fill, float radius) {
    float m = r.height < r.width ? r.height : r.width;
    float rr = m > 0 ? (radius / m) * 2.0f : 0.f;
    DrawRectangleRounded(r, rr, 8, fill);
}

void ui_outline(Rectangle r, float roundness, int segments, float thickness, Color c) {
#if defined(RAYLIB_VERSION_MAJOR) && RAYLIB_VERSION_MAJOR >= 5 && \
    defined(RAYLIB_VERSION_MINOR) && RAYLIB_VERSION_MINOR >= 5
    if (thickness > 0.f) DrawRectangleRoundedLinesEx(r, roundness, segments, thickness, c);
    else                 DrawRectangleRoundedLines(r, roundness, segments, c);
#else
    DrawRectangleRoundedLines(r, roundness, segments, thickness, c);
#endif
}

void ui_panel_border(Rectangle r, Color fill, Color border, float radius, float thick) {
    ui_panel(r, fill, radius);
    float m = r.height < r.width ? r.height : r.width;
    float rr = m > 0 ? (radius / m) * 2.0f : 0.f;
    ui_outline(r, rr, 8, thick, border);
}

void ui_panel_gradient(Rectangle r, Color top, Color bottom, float radius) {
    const int STRIPS = 24;
    for (int i = 0; i < STRIPS; i++) {
        float t = (float)i / (STRIPS - 1);
        Color c = ui_lerp_color(top, bottom, t);
        Rectangle s = { r.x, r.y + (r.height * i) / STRIPS,
                        r.width, r.height / STRIPS + 1 };
        DrawRectangleRounded(s, radius / (r.height < r.width ? r.height : r.width) * 2.f, 4, c);
    }
}

void ui_progress(Rectangle r, float t, Color fill, Color bg, float radius) {
    t = ui_clamp01(t);
    ui_panel(r, bg, radius);
    if (t <= 0) return;
    Rectangle f = { r.x, r.y, r.width * t, r.height };
    ui_panel(f, fill, radius);
}

void ui_text_ex(const char *t, Vector2 pos, int size, Color c, float spacing) {
    DrawTextEx(pick(size), t, pos, (float)size, spacing, c);
}

void ui_text(const char *t, int x, int y, int size, Color c) {
    ui_text_ex(t, (Vector2){ (float)x, (float)y }, size, c, 0.5f);
}

void ui_text_center(const char *t, Rectangle r, int size, Color c) {
    Vector2 m = MeasureTextEx(pick(size), t, (float)size, 0.5f);
    ui_text(t, (int)(r.x + (r.width - m.x) / 2),
               (int)(r.y + (r.height - m.y) / 2), size, c);
}

void ui_text_wrapped(const char *t, Rectangle r, int size, Color c, int line_gap) {
    Font f = pick(size);
    const char *p = t;
    float line_h = size + line_gap;
    float y = r.y;

    while (*p && y + line_h < r.y + r.height + line_h) {
        const char *line_start = p;
        const char *last_space = NULL;
        float line_w = 0.f;
        const char *q = p;
        while (*q && *q != '\n') {
                        char buf[8] = {0};
            int len = 1;
            unsigned char uc = (unsigned char)*q;
            if      ((uc & 0x80) == 0x00) len = 1;
            else if ((uc & 0xE0) == 0xC0) len = 2;
            else if ((uc & 0xF0) == 0xE0) len = 3;
            else if ((uc & 0xF8) == 0xF0) len = 4;
            if (len > 4) len = 4;               /* guard against malformed input */
            for (int k = 0; k < len && q[k]; k++) buf[k] = q[k];
            buf[len] = 0;                       /* explicit terminator */
            float w = MeasureTextEx(f, buf, (float)size, 0.5f).x;
            if (line_w + w > r.width && line_w > 0) break;
            /* Prefer to break on ASCII space, but for CJK we can break anywhere. */
            if (*q == ' ') last_space = q;
            line_w += w;
            q += len;
        }
        const char *line_end = q;
        if (*q && *q != '\n' && last_space && last_space > line_start) line_end = last_space;

        int line_len = (int)(line_end - line_start);
        if (line_len > 0) {
            char buf[1024];
            int n = line_len < (int)sizeof(buf) - 1 ? line_len : (int)sizeof(buf) - 1;
            memcpy(buf, line_start, n);
            buf[n] = 0;
            ui_text(buf, (int)r.x, (int)y, size, c);
        }
        y += line_h;
        p = line_end;
        while (*p == ' ') p++;
        if (*p == '\n') p++;
    }
}

bool ui_button_hover(Rectangle r) {
    return CheckCollisionPointRec(GetMousePosition(), r);
}

bool ui_button(Rectangle r, const char *label, Color bg, Color text_col) {
    bool hover = ui_button_hover(r);
    Color fill = hover ? ui_lerp_color(bg, (Color){255,255,255,255}, 0.08f) : bg;
    ui_panel_border(r, fill, ui_lerp_color(fill, (Color){255,255,255,255}, 0.14f), RADIUS_MD, 1.0f);
    ui_text_center(label, r, FONT_MD, text_col);
    return hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}
