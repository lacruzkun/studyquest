#include "ui.h"
#include "core/theme.h"
#include "core/utf8.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

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

static Font pick_cp(int nominal, uint32_t cp) {
    if (!g_fonts) return GetFontDefault();
    if (fonts_has_glyph(g_fonts, nominal, cp)) return fonts_pick(g_fonts, nominal);
    if (fonts_has_fallback_glyph(g_fonts, nominal, cp)) return fonts_pick_fallback(g_fonts, nominal);
    return fonts_pick(g_fonts, nominal);
}

static size_t next_utf8(const char *p, uint32_t *cp) {
    size_t n = 0;
    if (sq_utf8_decode(p, cp, &n)) return n;
    if (cp) *cp = (unsigned char)*p;
    return *p ? 1 : 0;
}

static float measure_utf8_with_fallback(const char *t, int size, float spacing) {
    if (!t || !*t) return 0.f;
    float w = 0.f;
    const char *p = t;
    while (*p) {
        uint32_t cp = 0;
        size_t len = next_utf8(p, &cp);
        if (!len) break;
        char ch[5] = {0};
        size_t copy = len < sizeof(ch) - 1 ? len : sizeof(ch) - 1;
        memcpy(ch, p, copy);
        Font f = pick_cp(size, cp);
        w += MeasureTextEx(f, ch, (float)size, spacing).x;
        p += len;
    }
    return w;
}

int ui_measure(const char *t, int size) {
    return (int)ceilf(measure_utf8_with_fallback(t, size, 0.5f));
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
    if (!t) return;
    float x = pos.x;
    const char *p = t;
    while (*p) {
        uint32_t cp = 0;
        size_t len = next_utf8(p, &cp);
        if (!len) break;
        if (cp == '\n') {
            x = pos.x;
            pos.y += (float)size;
            p += len;
            continue;
        }
        char ch[5] = {0};
        size_t copy = len < sizeof(ch) - 1 ? len : sizeof(ch) - 1;
        memcpy(ch, p, copy);
        Font f = pick_cp(size, cp);
        Vector2 m = MeasureTextEx(f, ch, (float)size, spacing);
        DrawTextEx(f, ch, pos, (float)size, spacing, c);
        x += m.x;
        pos.x = x;
        p += len;
    }
}

void ui_text(const char *t, int x, int y, int size, Color c) {
    ui_text_ex(t, (Vector2){ (float)x, (float)y }, size, c, 0.5f);
}

void ui_text_center(const char *t, Rectangle r, int size, Color c) {
    float w = measure_utf8_with_fallback(t, size, 0.5f);
    Font f = pick(size);
    float h = MeasureTextEx(f, "Ag", (float)size, 0.5f).y;
    ui_text(t, (int)(r.x + (r.width - w) / 2.f),
               (int)(r.y + (r.height - h) / 2.f), size, c);
}

void ui_text_wrapped(const char *t, Rectangle r, int size, Color c, int line_gap) {
    if (!t || !*t) return;
    float line_h = size + line_gap;
    float y = r.y;
    const char *p = t;

    while (*p && y + line_h < r.y + r.height + line_h) {
        const char *line_start = p;
        const char *last_space = NULL;
        float line_w = 0.f;
        const char *q = p;

        while (*q && *q != '\n') {
            uint32_t cp = 0;
            size_t len = next_utf8(q, &cp);
            if (!len) break;
            char ch[5] = {0};
            size_t copy = len < sizeof(ch) - 1 ? len : sizeof(ch) - 1;
            memcpy(ch, q, copy);
            float w = MeasureTextEx(pick_cp(size, cp), ch, (float)size, 0.5f).x;
            if (line_w + w > r.width && line_w > 0.f) break;
            if (cp == ' ') last_space = q;
            line_w += w;
            q += len;
        }

        const char *line_end = q;
        if (*q && *q != '\n' && last_space && last_space > line_start)
            line_end = last_space;

        const size_t line_len = (size_t)(line_end - line_start);
        if (line_len > 0) {
            char *buf = (char *)malloc(line_len + 1);
            if (!buf) return;
            memcpy(buf, line_start, line_len);
            buf[line_len] = 0;
            ui_text(buf, (int)r.x, (int)y, size, c);
            free(buf);
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

        uint32_t cp = 0;
        size_t decoded_len = next_utf8(p, &cp);
        if (!decoded_len) break;
        int len = (int)decoded_len;

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

            uint32_t cp = 0;
            size_t decoded_len = next_utf8(p, &cp);
            if (!decoded_len) break;
            int len = (int)decoded_len;

            char ch[5] = {0};
            size_t copy = decoded_len < sizeof(ch) - 1 ? decoded_len : sizeof(ch) - 1;
            memcpy(ch, p, copy);

            Font f = pick_cp(size, cp);
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
