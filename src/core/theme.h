#ifndef THEME_H
#define THEME_H

#include "raylib.h"

typedef struct Theme {
    Color bg, bg2, panel, panel_hi;
    Color border, border_hi;
    Color primary, primary_hi, primary_lo;
    Color accent, success, warning, danger;
    Color text, text_dim, text_muted;
} Theme;

extern const Theme TH;

#define RADIUS_LG 18.0f
#define RADIUS_MD 10.0f
#define RADIUS_SM 6.0f
#define PAD_LG 24
#define PAD_MD 16
#define PAD_SM 8

#define FONT_XL 56
#define FONT_LG 36
#define FONT_MD 22
#define FONT_SM 16
#define FONT_XS 13

#endif
