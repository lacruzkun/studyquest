#include "ui.h"
#include "core/theme.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
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
/*  Panels and shapes                                                  */
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

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

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
            char buf[4] = {0};
            int len = 1;
            unsigned char uc = (unsigned char)*q;
            if      ((uc & 0x80) == 0x00) len = 1;
            else if ((uc & 0xE0) == 0xC0) len = 2;
            else if ((uc & 0xF0) == 0xE0) len = 3;
            else if ((uc & 0xF8) == 0xF0) len = 4;
            for (int k = 0; k < len && q[k]; k++) buf[k] = q[k];
            float w = MeasureTextEx(f, buf, (float)size, 0.5f).x;
            if (line_w + w > r.width && line_w > 0) break;
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

/* ------------------------------------------------------------------ */
/*  Rich text                                                          */
/*                                                                     */
/*  Parses a small subset of inline markup into runs, then draws the   */
/*  runs character-by-character (UTF-8 aware). Returns the y coordinate */
/*  just below the last line, using the same font as the draw path so  */
/*  callers can stack content consistently.                            */
/* ------------------------------------------------------------------ */

typedef struct {
    char text[256];
    int  len;
    bool bold;
    bool underline;
    bool forced_break;
} RichRun;

#define MAX_RICH_RUNS 96

static int rich_parse(const char *in, RichRun *runs, int cap) {
    int n = 0;
    bool bold = false, under = false;
    RichRun cur;
    memset(&cur, 0, sizeof(cur));
    const char *p = in;

    #define FLUSH() do {                                  \
        if (cur.len > 0) {                                \
            if (n < cap) runs[n++] = cur;                 \
            memset(&cur, 0, sizeof(cur));                 \
        }                                                 \
    } while (0)

    #define FORCE_BREAK() do {                            \
        FLUSH();                                          \
        if (n < cap) {                                    \
            RichRun b; memset(&b, 0, sizeof(b));          \
            b.forced_break = true;                        \
            runs[n++] = b;                                \
        }                                                 \
    } while (0)

    while (*p && n < cap) {
        if (p[0] == '<') {
            const char *end = strchr(p, '>');
            if (!end) { p++; continue; }
            size_t tlen = (size_t)(end - (p + 1));
            char tag[32];
            if (tlen < sizeof(tag)) {
                memcpy(tag, p + 1, tlen);
                tag[tlen] = 0;
                bool closing = (tag[0] == '/');
                const char *name = closing ? tag + 1 : tag;

                size_t nl = strlen(name);
                if (nl > 0 && name[nl - 1] == '/') ((char*)name)[nl - 1] = 0;

                if (strncasecmp(name, "b", 1) == 0 &&
                    (name[1] == 0 || !isalnum((unsigned char)name[1]))) {
                    FLUSH(); bold = !closing;
                } else if (strncasecmp(name, "strong", 6) == 0) {
                    FLUSH(); bold = !closing;
                } else if (strncasecmp(name, "u", 1) == 0 &&
                           (name[1] == 0 || !isalnum((unsigned char)name[1]))) {
                    FLUSH(); under = !closing;
                } else if (strncasecmp(name, "br", 2) == 0) {
                    FORCE_BREAK();
                }
            }
            p = end + 1;
            continue;
        }

        if (p[0] == '\n') { FORCE_BREAK(); p++; continue; }

        int len = 1;
        unsigned char u = (unsigned char)*p;
        if      ((u & 0xE0) == 0xC0) len = 2;
        else if ((u & 0xF0) == 0xE0) len = 3;
        else if ((u & 0xF8) == 0xF0) len = 4;

        if (cur.len + len >= (int)sizeof(cur.text) - 1) {
            if (n < cap) runs[n++] = cur;
            memset(&cur, 0, sizeof(cur));
        }
        cur.bold = bold;
        cur.underline = under;
        for (int k = 0; k < len && p[k]; k++) cur.text[cur.len++] = p[k];
        cur.text[cur.len] = 0;
        p += len;
    }
    FLUSH();
    #undef FLUSH
    #undef FORCE_BREAK
    return n;
}

float ui_text_rich_ex(const char *t, Rectangle r, int size, Color c, int line_gap) {
    if (!t || !*t) return r.y;

    RichRun runs[MAX_RICH_RUNS];
    int n = rich_parse(t, runs, MAX_RICH_RUNS);
    if (n == 0) return r.y;

    Font f = pick(size);
    float line_h = size + line_gap;
    float x = r.x;
    float y = r.y;

    for (int i = 0; i < n; i++) {
        RichRun *run = &runs[i];
        if (run->forced_break) {
            x = r.x;
            y += line_h;
            continue;
        }

        const char *p = run->text;
        while (*p) {
            if (y + line_h > r.y + r.height + line_h) {
                /* Overflow — stop drawing but report the bottom. */
                return y + line_h;
            }

            int len = 1;
            unsigned char u = (unsigned char)*p;
            if      ((u & 0xE0) == 0xC0) len = 2;
            else if ((u & 0xF0) == 0xE0) len = 3;
            else if ((u & 0xF8) == 0xF0) len = 4;

            char ch[8] = {0};
            for (int k = 0; k < len && p[k]; k++) ch[k] = p[k];

            Vector2 m = MeasureTextEx(f, ch, (float)size, 0.5f);
            if (x + m.x > r.x + r.width && x > r.x) {
                x = r.x;
                y += line_h;
            }

            DrawTextEx(f, ch, (Vector2){ x, y }, (float)size, 0.5f, c);
            if (run->bold)
                DrawTextEx(f, ch, (Vector2){ x + 0.7f, y }, (float)size, 0.5f, c);
            if (run->underline)
                DrawLineEx((Vector2){ x, y + size + 1.f },
                           (Vector2){ x + m.x, y + size + 1.f },
                           1.5f, c);

            x += m.x;
            p += len;
        }
    }

    return y + line_h;
}

void ui_text_rich(const char *t, Rectangle r, int size, Color c, int line_gap) {
    (void)ui_text_rich_ex(t, r, size, c, line_gap);
}

/* ------------------------------------------------------------------ */
/*  Buttons                                                            */
/* ------------------------------------------------------------------ */

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
