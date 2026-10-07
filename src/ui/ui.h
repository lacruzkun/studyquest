#ifndef UI_H
#define UI_H

#include "raylib.h"
#include "core/fonts.h"
#include <stdbool.h>

/* Install the FontSet used by every drawing helper. Call once after
   fonts_init() and before any draw call. */
void ui_set_fonts(const FontSet *fs);

void ui_panel(Rectangle r, Color fill, float radius);
void ui_panel_border(Rectangle r, Color fill, Color border, float radius, float thick);
void ui_panel_gradient(Rectangle r, Color top, Color bottom, float radius);
void ui_progress(Rectangle r, float t, Color fill, Color bg, float radius);

void ui_text(const char *t, int x, int y, int size, Color c);
void ui_text_ex(const char *t, Vector2 pos, int size, Color c, float spacing);
void ui_text_center(const char *t, Rectangle r, int size, Color c);
void ui_text_wrapped(const char *t, Rectangle r, int size, Color c, int line_gap);

/* Rounded-rect outline that works across raylib versions. */
void ui_outline(Rectangle r, float roundness, int segments, float thickness, Color c);

/* Rich text: understands <b>, <u>, <br>, <br/>, and strips any other
   HTML tag. UTF-8 is preserved byte-for-byte. Returns the y-coordinate
   just below the last line drawn, so callers can stack content below.
   ui_text_rich() is a thin wrapper that discards the return value. */
float ui_text_rich_ex(const char *t, Rectangle r, int size, Color c, int line_gap);
void  ui_text_rich(const char *t, Rectangle r, int size, Color c, int line_gap);

bool ui_button(Rectangle r, const char *label, Color bg, Color text_col);
bool ui_button_hover(Rectangle r);

/* Measure text at a nominal size using the correct loaded font. */
int ui_measure(const char *t, int size);

/* Easing helpers (kept for compatibility — see also core/anim.h). */
float ui_ease_out(float t);
float ui_ease_in_out(float t);
float ui_clamp01(float v);
float ui_lerp(float a, float b, float t);
Color ui_lerp_color(Color a, Color b, float t);

#endif
