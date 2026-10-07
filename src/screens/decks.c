#include "screens.h"
#include "core/theme.h"
#include "screens/import_ui.h"
#include "ui/ui.h"
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Local UI state for the decks screen                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int  selected_deck;       /* index into decks[], or -1 */
    int  editing_card;        /* index into deck.cards[], or -1 */
    bool creating_card;

    char edit_front[MAX_TEXT];
    char edit_back[MAX_TEXT];
    char edit_tags[MAX_TAGS];
    int  edit_field;          /* 0=front, 1=back, 2=tags */

    char new_deck_name[MAX_DECK_NAME];
    bool creating_deck;

    bool confirm_delete;
    int  confirm_delete_deck; /* index, or -1 */
    int  confirm_delete_card; /* index, or -1 */
} DeckUI;

static DeckUI D;

/* ------------------------------------------------------------------ */
/*  Commit the in-progress card to the deck                            */
/* ------------------------------------------------------------------ */

static void commit_card(App *a) {
    if (a->data.decks.count == 0) return;
    if (D.selected_deck < 0 || D.selected_deck >= a->data.decks.count) return;

    Deck *d = &a->data.decks.decks[D.selected_deck];

    if (D.creating_card) {
        deck_add_card(d, D.edit_front, D.edit_back, D.edit_tags);

        /* Achievement: FIRST_CARD */
        Player *p = &a->data.player;
        if (!p->achievements[ACH_FIRST_CARD].unlocked) {
            p->achievements[ACH_FIRST_CARD].unlocked = true;
            p->achievements[ACH_FIRST_CARD].unlocked_at = (double)time(NULL);
            p->coins += ACH_DEFS[ACH_FIRST_CARD].coin_reward;
            player_add_xp(p, ACH_DEFS[ACH_FIRST_CARD].xp_reward);

            snprintf(a->ach_popup, sizeof(a->ach_popup), "%s",
                     ACH_DEFS[ACH_FIRST_CARD].name);
            snprintf(a->ach_popup_desc, sizeof(a->ach_popup_desc), "%s",
                     ACH_DEFS[ACH_FIRST_CARD].desc);
            a->ach_popup_t = 3.0f;
        }
    } else if (D.editing_card >= 0 && D.editing_card < d->card_count) {
        Card *c = &d->cards[D.editing_card];
        snprintf(c->front, MAX_TEXT, "%s", D.edit_front);
        snprintf(c->back,  MAX_TEXT, "%s", D.edit_back);
        snprintf(c->tags,  MAX_TAGS, "%s", D.edit_tags);
    }

    D.creating_card = false;
    D.editing_card = -1;
    app_save(a);
}

/* ------------------------------------------------------------------ */
/*  Update                                                             */
/* ------------------------------------------------------------------ */

void decks_update(App *a, float dt) {
    /* Import modal takes priority over everything on this screen. */
    if (import_ui_update(a, dt)) return;

    (void)dt;

    /* -------- Modal editor: text input + shortcuts -------- */
    if (D.creating_card || D.editing_card >= 0) {
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            char *field = D.edit_field == 0 ? D.edit_front :
                          D.edit_field == 1 ? D.edit_back  : D.edit_tags;
            int cap = D.edit_field == 0 ? MAX_TEXT :
                      D.edit_field == 1 ? MAX_TEXT : MAX_TAGS;
            size_t len = strlen(field);
            if (len + 1 < (size_t)cap && ch >= 32) {
                field[len]     = (char)ch;
                field[len + 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            char *field = D.edit_field == 0 ? D.edit_front :
                          D.edit_field == 1 ? D.edit_back  : D.edit_tags;
            size_t len = strlen(field);
            if (len > 0) field[len - 1] = 0;
        }
        if (IsKeyPressed(KEY_TAB)) {
            D.edit_field = (D.edit_field + 1) % 3;
        }
        if (IsKeyPressed(KEY_ENTER) &&
            (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL))) {
            commit_card(a);
            return;
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            D.creating_card = false;
            D.editing_card = -1;
            return;
        }
        return; /* swallow other input while editing */
    }

    /* -------- New-deck prompt -------- */
    if (D.creating_deck) {
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            size_t len = strlen(D.new_deck_name);
            if (len + 1 < MAX_DECK_NAME && ch >= 32) {
                D.new_deck_name[len]     = (char)ch;
                D.new_deck_name[len + 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            size_t len = strlen(D.new_deck_name);
            if (len > 0) D.new_deck_name[len - 1] = 0;
        }
        if (IsKeyPressed(KEY_ENTER) && D.new_deck_name[0]) {
            decklist_add(&a->data.decks, D.new_deck_name,
                (Color){
                    (unsigned char)(80 + GetRandomValue(0, 120)),
                    (unsigned char)(90 + GetRandomValue(0, 120)),
                    (unsigned char)(180 + GetRandomValue(0, 60)),
                    255
                });
            D.selected_deck = a->data.decks.count - 1;
            D.new_deck_name[0] = 0;
            D.creating_deck = false;
            app_save(a);
        }
        if (IsKeyPressed(KEY_ESCAPE)) D.creating_deck = false;
        return;
    }

    /* -------- Confirm-delete dialog -------- */
    if (D.confirm_delete) {
        if (IsKeyPressed(KEY_ESCAPE)) {
            D.confirm_delete = false;
            D.confirm_delete_card = -1;
            D.confirm_delete_deck = -1;
        }
        return;
    }

    /* -------- Plain ESC -------- */
    if (IsKeyPressed(KEY_ESCAPE)) {
        app_goto(a, SCREEN_DASHBOARD);
    }
}

/* ------------------------------------------------------------------ */
/*  Small static helper: draw one text field inside the modal          */
/* ------------------------------------------------------------------ */

static void draw_edit_field(int focused_idx, Rectangle r,
                            const char *label, const char *value, int idx) {
    ui_text(label, (int)r.x, (int)r.y - 20, FONT_XS, TH.text_muted);
    Color border = (focused_idx == idx) ? TH.primary : TH.border;
    ui_panel_border(r, TH.bg2, border, RADIUS_MD, focused_idx == idx ? 2.f : 1.f);
    ui_text_wrapped(value,
                    (Rectangle){ r.x + 10, r.y + 8, r.width - 20, r.height - 16 },
                    FONT_SM, TH.text, 2);
}

/* ------------------------------------------------------------------ */
/*  Card editor modal                                                  */
/* ------------------------------------------------------------------ */

static void draw_card_editor(App *a, int W, int H) {
    if (!D.creating_card && D.editing_card < 0) return;
    if (D.selected_deck < 0 || D.selected_deck >= a->data.decks.count) return;

    DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 180 });

    Rectangle m = { W / 2.f - 320, H / 2.f - 260, 640, 520 };
    ui_panel_border(m, TH.panel, TH.border_hi, RADIUS_LG, 2.f);

    const char *title = D.creating_card ? "New Card" : "Edit Card";
    ui_text(title, (int)m.x + 24, (int)m.y + 20, FONT_MD, TH.text);
    ui_text("Ctrl+Enter: save     Tab: next field     Esc: cancel",
            (int)m.x + 24, (int)m.y + 48, FONT_XS, TH.text_muted);

    draw_edit_field(D.edit_field,
                    (Rectangle){ m.x + 24, m.y + 90,  m.width - 48, 140 },
                    "FRONT / QUESTION", D.edit_front, 0);

    draw_edit_field(D.edit_field,
                    (Rectangle){ m.x + 24, m.y + 270, m.width - 48, 140 },
                    "BACK / ANSWER",    D.edit_back,  1);

    /* Tags field: single line */
    ui_text("TAGS (comma separated)",
            (int)m.x + 24, (int)m.y + 452 - 20, FONT_XS, TH.text_muted);
    Rectangle tr = { m.x + 24, m.y + 452, m.width - 48, 32 };
    ui_panel_border(tr, TH.bg2,
                    (D.edit_field == 2) ? TH.primary : TH.border,
                    RADIUS_SM, D.edit_field == 2 ? 2.f : 1.f);
    ui_text(D.edit_tags[0] ? D.edit_tags : "(optional)",
            (int)tr.x + 8, (int)tr.y + 8, FONT_SM,
            D.edit_tags[0] ? TH.text : TH.text_muted);

    /* Buttons */
    Rectangle b_save   = { m.x + m.width - 320, m.y + 500 - 46, 140, 40 };
    Rectangle b_cancel = { m.x + m.width - 170, m.y + 500 - 46, 140, 40 };

    if (ui_button(b_save, "SAVE", TH.primary, TH.text)) {
        commit_card(a);
        return;
    }
    if (ui_button(b_cancel, "CANCEL", TH.panel_hi, TH.text)) {
        D.creating_card = false;
        D.editing_card = -1;
    }
}

/* ------------------------------------------------------------------ */
/*  Draw                                                               */
/* ------------------------------------------------------------------ */

void decks_draw(App *a) {
    int W = GetScreenWidth(), H = GetScreenHeight();

    ui_text("DECKS", 24, 24, FONT_LG, TH.text);

    Rectangle back = { W - 160.f, 26, 136, 40 };
    if (ui_button(back, "< BACK", TH.panel, TH.text)) {
        app_goto(a, SCREEN_DASHBOARD);
    }

    /* ---------- Left: deck list ---------- */
    float list_w = 320;
    Rectangle list = { 24, 90, list_w, (float)H - 160 };
    ui_panel_border(list, TH.panel, TH.border, RADIUS_LG, 1.f);

    Rectangle new_btn = { list.x + 12, list.y + 12, list.width - 24, 40 };
    if (ui_button(new_btn, "+ NEW DECK", TH.primary, TH.text)) {
        D.creating_deck = true;
        D.new_deck_name[0] = 0;
    }

    float y = list.y + 64;
    for (int i = 0; i < a->data.decks.count; i++) {
        Deck *d = &a->data.decks.decks[i];
        Rectangle row = { list.x + 12, y, list.width - 24, 52 };
        if (y + row.height > list.y + list.height - 8) break;

        bool sel = (i == D.selected_deck);
        bool hov = ui_button_hover(row);

        if (sel)     ui_panel(row, TH.primary_lo, RADIUS_MD);
        else if (hov) ui_panel(row, TH.panel_hi,   RADIUS_MD);

        DrawRectangleRounded((Rectangle){ row.x + 8, row.y + 14, 24, 24 },
                             0.4f, 6, d->color);

        ui_text(d->name, (int)row.x + 44, (int)row.y + 8, FONT_SM, TH.text);

        char sub[64];
        snprintf(sub, sizeof(sub), "%d cards", d->card_count);
        ui_text(sub, (int)row.x + 44, (int)row.y + 28, FONT_XS, TH.text_muted);

        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            D.selected_deck = i;
            D.editing_card = -1;
            D.creating_card = false;
        }
        y += 58;
    }

    /* ---------- Right: deck detail ---------- */
    if (D.selected_deck >= 0 && D.selected_deck < a->data.decks.count) {
        Deck *d = &a->data.decks.decks[D.selected_deck];
        Rectangle pane = {
            list.x + list.width + 16,
            90,
            (float)W - list.width - 64,
            (float)H - 160
        };
        ui_panel_border(pane, TH.panel, TH.border, RADIUS_LG, 1.f);

        ui_text(d->name, (int)pane.x + 20, (int)pane.y + 18, FONT_MD, TH.text);

        /* Action buttons (top-right of the pane) */
        float bw = 130, bh = 36, gap = 8;
        Rectangle b_add = { pane.x + pane.width - 20 - bw*3 - gap*2, pane.y + 14, bw, bh };
        Rectangle b_imp = { pane.x + pane.width - 20 - bw*2 - gap*1, pane.y + 14, bw, bh };
        Rectangle b_exp = { pane.x + pane.width - 20 - bw*1,        pane.y + 14, bw, bh };

        if (ui_button(b_add, "+ CARD", TH.primary, TH.text)) {
            D.creating_card = true;
            D.edit_front[0] = D.edit_back[0] = D.edit_tags[0] = 0;
            D.edit_field = 0;
        }
        if (ui_button(b_imp, "IMPORT ANKI", TH.primary, TH.text)) {
            import_ui_start(a);
        }
        if (ui_button(b_exp, "EXPORT", TH.panel_hi, TH.text)) {
            char path[256];
            snprintf(path, sizeof(path), "export_%s.csv", d->name);
            for (char *p = path; *p; p++) if (*p == ' ') *p = '_';
            if (deck_export_csv(d, path)) {
                app_toast(a, TextFormat("Exported to %s", path), TH.success);
            } else {
                app_toast(a, "Export failed.", TH.danger);
            }
        }

        /* Card rows */
        float cy = pane.y + 64;
        float row_h = 50;
        for (int i = 0; i < d->card_count; i++) {
            if (cy + row_h > pane.y + pane.height - 60) break;

            Card *c = &d->cards[i];
            Rectangle row = { pane.x + 20, cy, pane.width - 40, 44 };
            bool hov = ui_button_hover(row);

            if (hov) ui_panel(row, TH.panel_hi, RADIUS_SM);

            /* Delete button */
            Rectangle del = { row.x + row.width - 36, row.y + 8, 28, 28 };
            bool del_hover = ui_button_hover(del);

            if (!del_hover) {
                ui_text(c->front, (int)row.x + 8, (int)row.y + 4, FONT_SM, TH.text);
                ui_text(c->back,  (int)row.x + 8, (int)row.y + 22, FONT_XS, TH.text_muted);
            }

            if (ui_button(del, "x", (Color){ 90, 40, 60, 255 }, TH.text)) {
                D.confirm_delete = true;
                D.confirm_delete_card = i;
                D.confirm_delete_deck = -1;
            } else if (hov && !del_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                D.editing_card = i;
                D.creating_card = false;
                snprintf(D.edit_front, MAX_TEXT, "%s", c->front);
                snprintf(D.edit_back,  MAX_TEXT, "%s", c->back);
                snprintf(D.edit_tags,  MAX_TAGS, "%s", c->tags);
                D.edit_field = 0;
            }
            cy += row_h;
        }

        /* Delete-deck button (bottom-right of pane) */
        Rectangle del_deck = {
            pane.x + pane.width - 180,
            pane.y + pane.height - 52,
            160, 36
        };
        if (ui_button(del_deck, "DELETE DECK", (Color){ 100, 40, 60, 255 }, TH.text)) {
            D.confirm_delete = true;
            D.confirm_delete_deck = D.selected_deck;
            D.confirm_delete_card = -1;
        }
    }

    /* ---------- Modals ---------- */
    draw_card_editor(a, W, H);

    /* New-deck prompt */
    if (D.creating_deck) {
        DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 160 });
        Rectangle m = { W / 2.f - 220, H / 2.f - 70, 440, 140 };
        ui_panel_border(m, TH.panel, TH.border_hi, RADIUS_LG, 2.f);

        ui_text_center("New Deck",
            (Rectangle){ m.x, m.y + 20, m.width, 30 }, FONT_MD, TH.text);

        Rectangle inp = { m.x + 20, m.y + 60, m.width - 40, 40 };
        ui_panel_border(inp, TH.bg2, TH.primary, RADIUS_MD, 1.f);
        ui_text(D.new_deck_name[0] ? D.new_deck_name : "Type a name...",
                (int)inp.x + 12, (int)inp.y + 10, FONT_MD,
                D.new_deck_name[0] ? TH.text : TH.text_muted);

        ui_text_center("Enter to confirm, Esc to cancel",
            (Rectangle){ m.x, m.y + 108, m.width, 20 }, FONT_XS, TH.text_muted);
    }

    /* Confirm-delete dialog */
    if (D.confirm_delete) {
        DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 160 });
        Rectangle m = { W / 2.f - 220, H / 2.f - 90, 440, 180 };
        ui_panel_border(m, TH.panel, TH.danger, RADIUS_LG, 2.f);

        ui_text_center("Delete?", (Rectangle){ m.x, m.y + 20, m.width, 30 },
                       FONT_MD, TH.danger);
        ui_text_center("This cannot be undone.",
                       (Rectangle){ m.x, m.y + 60, m.width, 20 },
                       FONT_SM, TH.text_dim);

        Rectangle by = { m.x + 40,  m.y + 110, 150, 44 };
        Rectangle bn = { m.x + 250, m.y + 110, 150, 44 };

        if (ui_button(by, "DELETE", TH.danger, TH.text)) {
            if (D.confirm_delete_deck >= 0 && D.confirm_delete_deck < a->data.decks.count) {
                decklist_remove(&a->data.decks, D.confirm_delete_deck);
                D.selected_deck = -1;
            } else if (D.confirm_delete_card >= 0 &&
                       D.selected_deck >= 0 &&
                       D.selected_deck < a->data.decks.count) {
                deck_remove_card(&a->data.decks.decks[D.selected_deck],
                                 D.confirm_delete_card);
            }
            D.confirm_delete = false;
            D.confirm_delete_card = -1;
            D.confirm_delete_deck = -1;
            app_save(a);
        }
        if (ui_button(bn, "CANCEL", TH.panel_hi, TH.text)) {
            D.confirm_delete = false;
            D.confirm_delete_card = -1;
            D.confirm_delete_deck = -1;
        }
    }

    import_ui_draw(a);
}
