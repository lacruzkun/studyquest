#include "screens/import_ui.h"
#include "core/theme.h"
#include "core/anim.h"
#include "ui/ui.h"
#include "import/apkg_importer.h"
#include "import/anki_parser.h"
#include "import/anki_convert.h"
#include "import/media_importer.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <inttypes.h>

/* ================================================================== */
/*  State                                                              */
/* ================================================================== */

typedef enum {
    IMP_IDLE = 0,
    IMP_ENTER_PATH,
    IMP_PREVIEW,
    IMP_RUNNING,
    IMP_DONE,
    IMP_ERROR
} ImportPhase;

typedef struct {
    ImportPhase phase;
    float phase_t;

    char path_input[MAX_TEXT];
    bool path_field_active;

    /* Package under inspection */
    ApkgHandle     handle;
    AnkiCollection col;
    bool have_handle;
    bool have_col;
    char tmp_dir[512];

    /* Preview */
    char preview_name[MAX_DECK_NAME];
    int  preview_notes;
    int  preview_cards;
    int  preview_models;
    int  preview_media;

    /* Running */
    int  run_card_index;     /* next card to process */
    int  run_deck_id;        /* StudyQuest deck we're adding to */
    bool run_started;

    /* Result */
    int  result_cards;
    char result_deck_name[MAX_DECK_NAME];
    int  result_deck_id;

    char error[1024];       /* was 512 */
} ImportUI;

static ImportUI I;

bool import_ui_active(void) { return I.phase != IMP_IDLE; }

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static void cleanup_temp(void) {
    if (!I.tmp_dir[0]) return;
    if (I.handle.db_path[0])         remove(I.handle.db_path);
    if (I.handle.media_json_path[0]) remove(I.handle.media_json_path);
    rmdir(I.tmp_dir);
    I.tmp_dir[0] = 0;
}

static void close_modal(void) {
    cleanup_temp();
    if (I.have_col) { anki_collection_free(&I.col); I.have_col = false; }
    if (I.have_handle) { apkg_close(&I.handle); I.have_handle = false; }
    I.phase = IMP_IDLE;
}

static void expand_tilde(const char *in, size_t in_cap, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (!in || in_cap == 0) { out[0] = 0; return; }

    if (in[0] == '~' && in_cap >= 2 && (in[1] == '/' || in[1] == 0)) {
        const char *home = getenv("HOME");
        if (!home || !*home) home = ".";

        size_t hlen = strnlen(home, cap - 1);
        size_t w = hlen < cap ? hlen : cap - 1;
        memcpy(out, home, w);

        size_t rest_max = in_cap - 1;
        size_t room     = cap - w - 1;
        size_t rlen     = strnlen(in + 1, rest_max < room ? rest_max : room);
        memcpy(out + w, in + 1, rlen);
        out[w + rlen] = 0;
    } else {
        size_t len = strnlen(in, in_cap - 1);
        if (len > cap - 1) len = cap - 1;
        memcpy(out, in, len);
        out[len] = 0;
    }
}

/* Path to the media directory (~/.studyquest/media). */
static void media_root(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(out, cap, "%s/.studyquest/media", home);
}

/* Find a deck name that doesn't collide with an existing StudyQuest deck. */
static bool unique_deck_name(App *a, const char *base,
                             char *out, size_t cap) {
    /* Ensure the base leaves room for a numeric suffix like " (999)". */
    char trimmed[MAX_DECK_NAME];
    snprintf(trimmed, sizeof(trimmed), "%.*s",
             MAX_DECK_NAME - 8, base ? base : "");
    base = trimmed;

    bool taken = false;
    for (int i = 0; i < a->data.decks.count; i++) {
        if (strcmp(a->data.decks.decks[i].name, base) == 0) {
            taken = true;
            break;
        }
    }
    if (!taken) {
        snprintf(out, cap, "%.*s", (int)(cap - 1), base);
        return true;
    }

    for (int n = 2; n < 1000; n++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s (%d)", base, n);
        bool found = false;
        for (int i = 0; i < a->data.decks.count; i++) {
            if (strcmp(a->data.decks.decks[i].name, buf) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            snprintf(out, cap, "%.*s", (int)(cap - 1), buf);
            return true;
        }
    }
    return false;
}

/* Compute the top-level name from a possibly nested Anki deck name. */
static void top_level_name(const char *anki_name, char *out, size_t cap) {
    const char *sep = strstr(anki_name, "::");
    size_t n = sep ? (size_t)(sep - anki_name) : strlen(anki_name);
    if (n >= cap) n = cap - 1;
    memcpy(out, anki_name, n);
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

/* Convert Anki's " jlpt-n5 vocabulary " to "jlpt-n5,vocabulary". */
static void anki_tags_to_csv(const char *anki_tags, char *out, size_t cap) {
    out[0] = 0;
    if (!anki_tags || !*anki_tags) return;
    size_t w = 0;
    const char *p = anki_tags;
    while (*p && w + 1 < cap) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - start);
        if (w > 0 && w + 1 < cap) out[w++] = ',';
        size_t n = len < (cap - w - 1) ? len : (cap - w - 1);
        memcpy(out + w, start, n);
        w += n;
    }
    out[w] = 0;
}

/* ================================================================== */
/*  Public entry points                                                */
/* ================================================================== */

void import_ui_start(App *a) {
    (void)a;
    memset(&I, 0, sizeof(I));
    I.phase = IMP_ENTER_PATH;
    I.phase_t = 0.f;
    I.path_field_active = true;
    const char *home = getenv("HOME");
    if (home) snprintf(I.path_input, sizeof(I.path_input), "%s/", home);
}

/* Open the package and parse the collection. Blocks for ~50 ms on
   typical decks (ZIP central directory + one file extraction + SQLite
   open). Fills I.error on failure. */
static bool open_and_parse(void) {
    char path[512];
    expand_tilde(I.path_input, sizeof(I.path_input), path, sizeof(path));

    /* Validate extension — we only accept .apkg here. */
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".apkg") != 0) {
        snprintf(I.error, sizeof(I.error),
                 "The file must have a .apkg extension.\n"
                 "Selected: %.450s", path);
        return false;
    }

    /* Temp directory via mkdtemp. */
    char tmpl[] = "/tmp/studyquest_XXXXXX";
    char *td = mkdtemp(tmpl);
    if (!td) {
        snprintf(I.error, sizeof(I.error),
                 "Could not create a temporary directory under /tmp.");
        return false;
    }
    snprintf(I.tmp_dir, sizeof(I.tmp_dir), "%s", td);

    if (!apkg_open(path, I.tmp_dir, &I.handle)) {
        snprintf(I.error, sizeof(I.error), "%s", I.handle.error);
        return false;
    }
    I.have_handle = true;

    if (!anki_parse(I.handle.db_path, &I.col)) {
        snprintf(I.error, sizeof(I.error), "%s", I.col.error);
        return false;
    }
    I.have_col = true;

    /* Preview stats. */
    I.preview_notes  = I.col.note_count;
    I.preview_cards  = I.col.card_count;
    I.preview_models = I.col.model_count;
    I.preview_media  = I.handle.media_entry_count;

    if (I.col.card_count == 0) {
        snprintf(I.error, sizeof(I.error),
                 "The package contains no cards.\n"
                 "If this is a Legacy 2 package, StudyQuest should have\n"
                 "found collection.anki21 automatically — please report.");
        return false;
    }

    /* Deck name: top-level of the first card's deck. */
    char raw[256] = "Imported Deck";
    if (I.col.card_count > 0) {
        int64_t did = I.col.cards[0].did;
        for (int i = 0; i < I.col.deck_count; i++) {
            if (I.col.decks[i].id == did) {
                snprintf(raw, sizeof(raw), "%s", I.col.decks[i].name);
                break;
            }
        }
    }
    top_level_name(raw, I.preview_name, sizeof(I.preview_name));
    if (!I.preview_name[0])
        snprintf(I.preview_name, sizeof(I.preview_name), "Imported Deck");

    return true;
}

/* Process up to `n` cards in the running phase. Called each frame. */
static bool run_chunk(App *a, int n) {
    Deck *d = decklist_find(&a->data.decks, I.run_deck_id);
    if (!d) {
        snprintf(I.error, sizeof(I.error),
                 "Internal error: deck disappeared during import.");
        return false;
    }

    int end = I.run_card_index + n;
    if (end > I.col.card_count) end = I.col.card_count;

    for (int i = I.run_card_index; i < end; i++) {
        const AnkiCard *ac = &I.col.cards[i];
        ConvertedCard cc;
        if (!anki_convert_card(&I.col, ac, &cc)) {
            TraceLog(LOG_WARNING, "ANKI: failed to convert card %d (nid=%" PRId64 ")", i, ac->nid);
            continue;
        }

        Card *card;
        if (cc.field_count > 0) {
            card = deck_add_card_fields(d, cc.field_names,
                                        (const char *const *)cc.field_values,
                                        cc.field_count, "");
        } else {
            card = deck_add_card(d, "", "", "");
        }
        if (!card) {
            anki_converted_card_free(&cc);
            continue;
        }

        /* Tags live on the note, not the card. */
        const AnkiNote *note = anki_find_note(&I.col, ac->nid);
        if (note && note->tags[0]) {
            char csv[MAX_TAGS];
            anki_tags_to_csv(note->tags, csv, sizeof(csv));
            if (csv[0]) snprintf(card->tags, MAX_TAGS, "%s", csv);
        }

        /* Preserve every discovered media reference and which side it
           belongs to. Resolution to the normalized imported filename is
           performed after the package media pass below. */
        for (int m = 0; m < cc.media_ref_count; m++) {
            card_add_media_ref(card, cc.media_refs[m],
                               cc.media_kinds[m], cc.media_sides[m]);
        }

        TraceLog(LOG_DEBUG,
                 "ANKI: card %d nid=%" PRId64 " fields=%d media=%d",
                 i, ac->nid, cc.field_count, cc.media_ref_count);
        anki_converted_card_free(&cc);
    }

    I.run_card_index = end;
    return true;
}

/* ================================================================== */
/*  Update                                                             */
/* ================================================================== */

bool import_ui_update(App *a, float dt) {
    if (I.phase == IMP_IDLE) return false;
    I.phase_t += dt;

    int W = GetScreenWidth(), H = GetScreenHeight();

    /* ---- ENTER_PATH -------------------------------------------- */
    if (I.phase == IMP_ENTER_PATH) {
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            size_t len = strlen(I.path_input);
            if (len + 1 < sizeof(I.path_input) && ch >= 32) {
                I.path_input[len]     = (char)ch;
                I.path_input[len + 1] = 0;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
            size_t len = strlen(I.path_input);
            if (len > 0) I.path_input[len - 1] = 0;
        }
        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }

        Rectangle btn_open   = { W/2.f - 280, H/2.f + 80, 180, 52 };
        Rectangle btn_cancel = { W/2.f + 100, H/2.f + 80, 180, 52 };

        bool do_open = IsKeyPressed(KEY_ENTER);
        if (ui_button_hover(btn_open) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            do_open = true;
        if (ui_button_hover(btn_cancel) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            close_modal();

        if (do_open) {
            if (I.path_input[0] && open_and_parse()) {
                I.phase = IMP_PREVIEW;
                I.phase_t = 0.f;
            } else {
                if (!I.error[0])
                    snprintf(I.error, sizeof(I.error), "No path entered.");
                I.phase = IMP_ERROR;
                I.phase_t = 0.f;
            }
        }
        return true;
    }

    /* ---- PREVIEW ----------------------------------------------- */
    if (I.phase == IMP_PREVIEW) {
        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }

        Rectangle btn_import = { W/2.f - 280, H/2.f + 170, 180, 56 };
        Rectangle btn_cancel = { W/2.f + 100, H/2.f + 170, 180, 56 };

        if (ui_button_hover(btn_cancel) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            close_modal();
            return true;
        }

        bool do_import = IsKeyPressed(KEY_ENTER);
        if (ui_button_hover(btn_import) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            do_import = true;

        if (do_import) {
            /* Create the StudyQuest deck now, before the running phase. */
            char final_name[MAX_DECK_NAME];
            if (!unique_deck_name(a, I.preview_name, final_name, sizeof(final_name))) {
                snprintf(I.error, sizeof(I.error), "Too many decks — delete some first.");
                I.phase = IMP_ERROR;
                I.phase_t = 0.f;
                return true;
            }
            Color col = (Color){
                (unsigned char)(100 + GetRandomValue(0, 100)),
                (unsigned char)(120 + GetRandomValue(0, 100)),
                (unsigned char)(180 + GetRandomValue(0, 60)),
                255
            };
            Deck *d = decklist_add(&a->data.decks, final_name, col);
            if (!d) {
                snprintf(I.error, sizeof(I.error), "Could not create deck.");
                I.phase = IMP_ERROR;
                I.phase_t = 0.f;
                return true;
            }
            I.run_deck_id     = d->id;
            I.run_card_index  = 0;
            I.run_started     = true;
            I.result_deck_id  = d->id;
            snprintf(I.result_deck_name, sizeof(I.result_deck_name), "%s", final_name);
            I.phase = IMP_RUNNING;
            I.phase_t = 0.f;
        }
        return true;
    }

    /* ---- RUNNING ----------------------------------------------- */
    if (I.phase == IMP_RUNNING) {
        /* ~120 cards per frame keeps the loop responsive. */
        if (!run_chunk(a, 120)) {
            I.phase = IMP_ERROR;
            I.phase_t = 0.f;
            return true;
        }

        if (I.run_card_index >= I.col.card_count) {
            Deck *imported_deck = decklist_find(&a->data.decks, I.run_deck_id);
            if (!imported_deck) {
                snprintf(I.error, sizeof(I.error), "Imported deck disappeared before media resolution.");
                I.phase = IMP_ERROR;
                I.phase_t = 0.f;
                return true;
            }

            /* Media pass. Synchronous but bounded by file count. */
            if (I.handle.media_json_path[0]) {
                char mroot[512];
                media_root(mroot, sizeof(mroot));

                MediaImportResult mr = {0};
                char real_path[512];
                expand_tilde(I.path_input, sizeof(I.path_input), real_path, sizeof(real_path));
                if (!media_import_all(real_path,
                                      I.handle.media_json_path,
                                      mroot, NULL, NULL, &mr)) {
                    TraceLog(LOG_WARNING, "Media import failed: %s", mr.error);
                } else {
                    TraceLog(LOG_INFO,
                        "Media: copied=%d missing=%d unsafe=%d entries=%d",
                        mr.copied, mr.missing, mr.skipped_unsafe, mr.count);

                    /* Convert source names such as an Anki media-map value
                       to the exact normalized filename written on disk. */
                    for (int di = 0; di < imported_deck->card_count; di++) {
                        Card *card = &imported_deck->cards[di];
                        for (int mi = 0; mi < card->media_ref_count; mi++) {
                            char resolved[sizeof(card->media_refs[mi].ref)];
                            if (media_import_resolve(&mr, card->media_refs[mi].ref,
                                                     resolved, sizeof(resolved))) {
                                snprintf(card->media_refs[mi].ref,
                                         sizeof(card->media_refs[mi].ref), "%s", resolved);
                            }
                        }

                        /* Rebuild the legacy first-image/first-audio aliases
                           from the complete resolved list. */
                        card->image_ref[0] = 0;
                        card->audio_ref[0] = 0;
                        for (int mi = 0; mi < card->media_ref_count; mi++) {
                            if (card->media_refs[mi].kind == MEDIA_KIND_IMAGE && !card->image_ref[0])
                                snprintf(card->image_ref, sizeof(card->image_ref), "%s",
                                         card->media_refs[mi].ref);
                            if (card->media_refs[mi].kind == MEDIA_KIND_AUDIO && !card->audio_ref[0])
                                snprintf(card->audio_ref, sizeof(card->audio_ref), "%s",
                                         card->media_refs[mi].ref);
                        }
                    }
                }
                media_import_free(&mr);
            }

            I.result_cards = I.run_card_index;
            app_refresh_fonts(a);
            app_save(a);

            /* Achievement: FIRST_CARD fires when the user creates their
               first card; here we fire it for the whole import event so
               a first-import user gets the reward. */
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
                a->ach_popup_t = 3.f;
            }
            app_save(a);

            I.phase = IMP_DONE;
            I.phase_t = 0.f;
        }
        return true;
    }

    /* ---- DONE -------------------------------------------------- */
    if (I.phase == IMP_DONE) {
        Rectangle btn_study = { W/2.f - 280, H/2.f + 170, 240, 56 };
        Rectangle btn_close = { W/2.f + 40,  H/2.f + 170, 240, 56 };

        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }

        if (ui_button_hover(btn_close) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            close_modal();
            return true;
        }
        if (ui_button_hover(btn_study) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            int deck_id = I.result_deck_id;
            close_modal();
            app_start_session(a, deck_id);
            return true;
        }
        return true;
    }

    /* ---- ERROR ------------------------------------------------- */
    if (I.phase == IMP_ERROR) {
        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }

        Rectangle btn_ok = { W/2.f - 90, H/2.f + 140, 180, 52 };
        if (ui_button_hover(btn_ok) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            close_modal();
        }
        return true;
    }

    return true;
}

/* ================================================================== */
/*  Draw                                                               */
/* ================================================================== */

static void dim_background(int W, int H) {
    DrawRectangle(0, 0, W, H, (Color){ 0, 0, 0, 180 });
}

static void panel_header(Rectangle m, const char *title) {
    ui_panel_border(m, TH.panel, TH.border_hi, RADIUS_LG, 2.f);
    ui_text_center(title,
        (Rectangle){ m.x, m.y + 22, m.width, 34 }, FONT_MD, TH.text);
}

static void draw_enter_path(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 320, H/2.f - 140, 640, 320 };
    panel_header(m, "Import Anki Deck");

    ui_text_center("Paste the path to a .apkg file.",
        (Rectangle){ m.x, m.y + 70, m.width, 22 },
        FONT_SM, TH.text_dim);
    ui_text_center("Example: ~/Downloads/Japanese Core 2k.apkg",
        (Rectangle){ m.x, m.y + 92, m.width, 22 },
        FONT_XS, TH.text_muted);

    Rectangle inp = { m.x + 40, m.y + 130, m.width - 80, 48 };
    ui_panel_border(inp, TH.bg2, TH.primary, RADIUS_MD, 1.5f);
    ui_text(I.path_input[0] ? I.path_input : "(empty)",
            (int)inp.x + 12, (int)inp.y + 14, FONT_SM,
            I.path_input[0] ? TH.text : TH.text_muted);

    if (((int)(GetTime() * 2.0f)) % 2 == 0) {
        int cx = (int)inp.x + 12 + ui_measure(I.path_input, FONT_SM) + 2;
        DrawRectangle(cx, (int)inp.y + 12, 2, 24, TH.primary);
    }

    Rectangle btn_open   = { W/2.f - 280, H/2.f + 80, 180, 52 };
    Rectangle btn_cancel = { W/2.f + 100, H/2.f + 80, 180, 52 };
    if (ui_button(btn_open,   "OPEN",   TH.primary, TH.text)) { /* handled */ }
    if (ui_button(btn_cancel, "CANCEL", TH.panel_hi, TH.text)) { /* handled */ }

    ui_text_center("Enter to open   •   Esc to cancel",
        (Rectangle){ m.x, m.y + m.height - 34, m.width, 22 },
        FONT_XS, TH.text_muted);
}

static void draw_preview(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 360, H/2.f - 220, 720, 500 };
    panel_header(m, "Import Preview");

    /* Deck name */
    ui_text_center(I.preview_name,
        (Rectangle){ m.x, m.y + 62, m.width, 34 },
        FONT_LG, TH.text);

    /* Stats rows */
    float y = m.y + 110;
    char buf[128];
    struct { const char *label; int n; } rows[] = {
        { "Cards",      I.preview_cards },
        { "Notes",      I.preview_notes },
        { "Note types", I.preview_models },
        { "Media files", I.preview_media },
    };
    for (size_t i = 0; i < sizeof(rows)/sizeof(rows[0]); i++) {
        snprintf(buf, sizeof(buf), "%d", rows[i].n);
        ui_text(rows[i].label, (int)m.x + 60, (int)y, FONT_SM, TH.text_dim);
        int nw = ui_measure(buf, FONT_MD);
        ui_text(buf, (int)(m.x + m.width - 60 - nw), (int)y - 4, FONT_MD, TH.text);
        y += 36;
    }

    /* Note about scheduling */
    ui_text_center("All imported cards will be treated as new.",
        (Rectangle){ m.x, (float)(m.y + m.height - 130), m.width, 22 },
        FONT_XS, TH.text_muted);
    ui_text_center("StudyQuest's scheduler takes over from here.",
        (Rectangle){ m.x, (float)(m.y + m.height - 108), m.width, 22 },
        FONT_XS, TH.text_muted);

    Rectangle btn_import = { W/2.f - 280, H/2.f + 170, 180, 56 };
    Rectangle btn_cancel = { W/2.f + 100, H/2.f + 170, 180, 56 };
    ui_button(btn_import, "IMPORT", TH.primary, TH.text);
    ui_button(btn_cancel, "CANCEL", TH.panel_hi, TH.text);
}

static void draw_running(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 300, H/2.f - 90, 600, 200 };
    panel_header(m, "Importing...");

    float pct = I.col.card_count > 0
        ? (float)I.run_card_index / (float)I.col.card_count
        : 0.f;

    Rectangle bar = { m.x + 40, m.y + 100, m.width - 80, 24 };
    ui_progress(bar, pct, TH.primary, TH.bg, RADIUS_MD);

    char buf[64];
    snprintf(buf, sizeof(buf), "%d / %d cards", I.run_card_index, I.col.card_count);
    ui_text_center(buf, bar, FONT_SM, TH.text);

    ui_text_center("Please wait — do not close the window.",
        (Rectangle){ m.x, m.y + 148, m.width, 22 },
        FONT_XS, TH.text_muted);
}

static void draw_done(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 320, H/2.f - 180, 640, 400 };
    panel_header(m, "Import Complete");

    /* Big count */
    char buf[64];
    snprintf(buf, sizeof(buf), "%d", I.result_cards);
    ui_text_center(buf,
        (Rectangle){ m.x, m.y + 80, m.width, 90 },
        FONT_XL, TH.success);
    ui_text_center("cards imported",
        (Rectangle){ m.x, m.y + 172, m.width, 24 },
        FONT_SM, TH.text_dim);

    ui_text_center(I.result_deck_name,
        (Rectangle){ m.x, m.y + 212, m.width, 34 },
        FONT_MD, TH.text);

    Rectangle btn_study = { W/2.f - 280, H/2.f + 170, 240, 56 };
    Rectangle btn_close = { W/2.f + 40,  H/2.f + 170, 240, 56 };
    ui_button(btn_study, "START STUDYING", TH.primary, TH.text);
    ui_button(btn_close, "BACK TO DECKS",  TH.panel_hi, TH.text);
}

static void draw_error(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 320, H/2.f - 160, 640, 340 };
    panel_header(m, "Import Failed");

    ui_text_wrapped(I.error,
        (Rectangle){ m.x + 40, m.y + 70, m.width - 80, 140 },
        FONT_SM, TH.danger, 4);

    ui_text_center("No changes were made to your existing decks.",
        (Rectangle){ m.x, m.y + 230, m.width, 22 },
        FONT_XS, TH.text_muted);

    Rectangle btn_ok = { W/2.f - 90, H/2.f + 140, 180, 52 };
    ui_button(btn_ok, "OK", TH.panel_hi, TH.text);
}

void import_ui_draw(App *a) {
    (void)a;
    if (I.phase == IMP_IDLE) return;
    int W = GetScreenWidth(), H = GetScreenHeight();
    switch (I.phase) {
        case IMP_ENTER_PATH: draw_enter_path(W, H); break;
        case IMP_PREVIEW:    draw_preview(W, H);    break;
        case IMP_RUNNING:    draw_running(W, H);    break;
        case IMP_DONE:       draw_done(W, H);       break;
        case IMP_ERROR:      draw_error(W, H);      break;
        default: break;
    }
}
