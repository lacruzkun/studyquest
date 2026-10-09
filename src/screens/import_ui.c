#include "screens/import_ui.h"
#include "core/theme.h"
#include "core/anim.h"
#include "ui/ui.h"
#include "import/apkg_importer.h"
#include "import/anki_parser.h"
#include "import/anki_convert.h"
#include "import/media_importer.h"
#include "import/import_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <inttypes.h>

/* ================================================================== */
/*  State                                                              */
/* ================================================================== */

#define IMP_MAX_ENTRIES 256

typedef enum {
    IMP_IDLE = 0,
    IMP_BROWSE,
    IMP_OPENING,
    IMP_PREVIEW,
    IMP_DUP,
    IMP_RUNNING,
    IMP_DONE,
    IMP_ERROR
} ImportPhase;

typedef enum {
    RUN_MEDIA = 0,       /* open the media map */
    RUN_MEDIA_COPY,      /* copy media files, a few per frame */
    RUN_CARDS,
    RUN_COMMIT
} RunSub;

typedef struct {
    char  name[256];
    bool  is_dir;
    bool  is_apkg;
} BrowseEntry;

#define TREE_MAX_ROWS 1024

/* One line of the deck-hierarchy preview. Parent levels have cards < 0. */
typedef struct {
    char label[96];
    int  depth;
    int  cards;
} TreeRow;

typedef struct {
    ImportPhase phase;
    float phase_t;

    /* Browser */
    char        browse_dir[512];
    BrowseEntry entries[IMP_MAX_ENTRIES];
    int         entry_count;
    int         scroll;
    int         selected;

    /* Selected package + parsed collection */
    char          apkg_path[1024];
    ApkgHandle    handle;
    AnkiCollection col;
    bool          have_handle;
    bool          have_col;
    char          tmp_dir[512];

    /* Preview */
    ImportDeckPlan plan;
    int            preview_models;
    int            preview_media;
    int            preview_reviewed;    /* cards with review history */
    bool           dup_any;
    TreeRow        tree[TREE_MAX_ROWS];
    int            tree_count;
    int            tree_scroll;
    int            open_frames;         /* frames drawn before the heavy open */

    /* Running */
    ImportMode       mode;
    DeckList         scratch;
    ImportJob        job;
    MediaImportResult media;
    MediaImportJob    mjob;
    bool              mjob_open;
    RunSub           run_sub;
    bool             run_started;

    /* Result */
    int             result_cards;
    int             result_notes;
    ImportWarnings  warnings;
    ImportCommitResult commit_res;
    float           warn_scroll;

    char error[1024];
} ImportUI;

static ImportUI I;

bool import_ui_active(void) { return I.phase != IMP_IDLE; }

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static void cleanup_temp(void) {
    if (!I.tmp_dir[0]) return;
    if (I.handle.db_path[0]) {
        const char *sfx[] = { "", "-wal", "-shm", "-journal" };
        for (int i = 0; i < 4; i++) {
            char p[700];
            snprintf(p, sizeof(p), "%s%s", I.handle.db_path, sfx[i]);
            remove(p);
        }
    }
    if (I.handle.media_json_path[0]) remove(I.handle.media_json_path);
    rmdir(I.tmp_dir);
    I.tmp_dir[0] = 0;
}

static void reset_state(void) {
    cleanup_temp();
    if (I.have_col) { anki_collection_free(&I.col); I.have_col = false; }
    if (I.have_handle) { apkg_close(&I.handle); I.have_handle = false; }
    if (I.mjob_open) { media_import_end(&I.mjob); I.mjob_open = false; }
    media_import_free(&I.media);
    import_job_free(&I.job);
    decklist_free(&I.scratch);
    memset(&I, 0, sizeof(I));
    I.phase = IMP_IDLE;
}

static void close_modal(void) { reset_state(); }

static void media_root(char *out, size_t cap) {
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(out, cap, "%s/.studyquest/media", home);
}

/* ================================================================== */
/*  File browser                                                       */
/* ================================================================== */

static bool has_apkg_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".apkg") == 0;
}

static int browse_cmp(const void *a, const void *b) {
    const BrowseEntry *ea = a, *eb = b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcasecmp(ea->name, eb->name);
}

static void browse_scan(const char *dir) {
    snprintf(I.browse_dir, sizeof(I.browse_dir), "%s", dir);
    I.entry_count = 0;
    I.scroll = 0;
    I.selected = 0;

    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d)) != NULL && I.entry_count < IMP_MAX_ENTRIES) {
        if (strcmp(e->d_name, ".") == 0) continue;
        /* Hide dotfiles/dot-directories; ".." stays for navigating up. */
        if (e->d_name[0] == '.' && strcmp(e->d_name, "..") != 0) continue;
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;

        bool is_dir = S_ISDIR(st.st_mode);
        bool is_apkg = !is_dir && S_ISREG(st.st_mode) && has_apkg_ext(e->d_name);
        if (!is_dir && !is_apkg) continue;

        BrowseEntry *be = &I.entries[I.entry_count++];
        snprintf(be->name, sizeof(be->name), "%s", e->d_name);
        be->is_dir = is_dir;
        be->is_apkg = is_apkg;
    }
    closedir(d);
    qsort(I.entries, (size_t)I.entry_count, sizeof(BrowseEntry), browse_cmp);
}

static void browse_up(void) {
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", I.browse_dir);
    char *slash = strrchr(dir, '/');
    if (!slash) return;
    if (slash == dir) slash[1] = 0;   /* stay at root */
    else *slash = 0;
    browse_scan(dir);
}

static int plan_name_cmp(const void *a, const void *b) {
    const ImportDeckSummary *x = *(const ImportDeckSummary *const *)a;
    const ImportDeckSummary *y = *(const ImportDeckSummary *const *)b;
    return strcasecmp(x->name, y->name);
}

/* Turn the flat plan into an indented hierarchy: sort by full name, then for
   each deck print only the path levels that differ from the previous one. */
static void build_tree(void) {
    I.tree_count = 0;
    I.tree_scroll = 0;
    const ImportDeckSummary *order[IMPORT_MAX_DECKS];
    int n = I.plan.deck_count;
    for (int i = 0; i < n; i++) order[i] = &I.plan.decks[i];
    qsort(order, (size_t)n, sizeof(order[0]), plan_name_cmp);

    char prev[MAX_DECK_NAME] = "";
    for (int i = 0; i < n; i++) {
        const char *name = order[i]->name;
        /* walk both paths level by level */
        const char *a = name, *b = prev;
        int depth = 0;
        bool diverged = false;
        while (*a) {
            const char *ae = strstr(a, "::");
            size_t al = ae ? (size_t)(ae - a) : strlen(a);
            bool last = (ae == NULL);
            const char *be = *b ? strstr(b, "::") : NULL;
            size_t bl = *b ? (be ? (size_t)(be - b) : strlen(b)) : 0;
            bool same = !diverged && *b && al == bl && strncmp(a, b, al) == 0 && !last;
            if (!same) {
                diverged = true;
                if (I.tree_count < TREE_MAX_ROWS) {
                    TreeRow *r = &I.tree[I.tree_count++];
                    size_t cl = al < sizeof(r->label) - 1 ? al : sizeof(r->label) - 1;
                    memcpy(r->label, a, cl);
                    r->label[cl] = 0;
                    r->depth = depth;
                    r->cards = last ? order[i]->card_count : -1;
                }
            }
            if (last) break;
            a = ae + 2;
            if (*b) b = be ? be + 2 : b + bl;
            depth++;
        }
        snprintf(prev, sizeof(prev), "%s", name);
    }
}

/* Open the selected .apkg and parse the collection. */
static bool open_selected(void) {
    if (I.selected < 0 || I.selected >= I.entry_count) return false;
    BrowseEntry *be = &I.entries[I.selected];
    if (!be->is_apkg) return false;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", I.browse_dir, be->name);
    snprintf(I.apkg_path, sizeof(I.apkg_path), "%s", path);

    char tmpl[] = "/tmp/studyquest_XXXXXX";
    char *td = mkdtemp(tmpl);
    if (!td) {
        snprintf(I.error, sizeof(I.error), "Could not create a temporary directory.");
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

    if (I.col.card_count == 0) {
        snprintf(I.error, sizeof(I.error),
                 "The package contains no cards.");
        return false;
    }

    if (!import_build_deck_plan(&I.col, &I.plan)) {
        snprintf(I.error, sizeof(I.error),
                 "The package contains too many distinct decks.");
        return false;
    }

    I.preview_models = I.col.model_count;
    I.preview_media  = I.handle.media_entry_count;
    I.preview_reviewed = 0;
    for (int i = 0; i < I.col.card_count; i++)
        if (I.col.cards[i].type != 0) I.preview_reviewed++;
    build_tree();
    return true;
}

/* Detect whether any deck in the plan already exists in the app. */
static void detect_duplicates(const App *a) {
    I.dup_any = false;
    for (int i = 0; i < I.plan.deck_count; i++) {
        if (import_find_existing_deck(&a->data.decks, I.plan.decks[i].name)) {
            I.dup_any = true;
            break;
        }
    }
}

/* ================================================================== */
/*  Running                                                            */
/* ================================================================== */

static void run_begin(const App *a, ImportMode mode) {
    I.mode = mode;
    I.run_sub = RUN_MEDIA;
    I.run_started = true;
    decklist_init(&I.scratch);
    memset(&I.media, 0, sizeof(I.media));
    memset(&I.mjob, 0, sizeof(I.mjob));
    memset(&I.job, 0, sizeof(I.job));
    memset(&I.commit_res, 0, sizeof(I.commit_res));
    I.mjob_open = false;
    (void)a;
}

/* Abort the import: remove media this run created, drop everything staged
   in memory, and show the error screen. The user's decks were never touched
   (cards only reach them in the final all-or-nothing commit). */
static void run_fail(const char *msg) {
    if (I.mjob_open) { media_import_end(&I.mjob); I.mjob_open = false; }
    media_import_rollback(&I.media);
    media_import_free(&I.media);
    import_job_free(&I.job);
    decklist_free(&I.scratch);
    snprintf(I.error, sizeof(I.error), "%s", msg);
    I.phase = IMP_ERROR;
    I.phase_t = 0.f;
}

static void run_step(App *a) {
    if (I.run_sub == RUN_MEDIA) {
        I.run_sub = RUN_MEDIA_COPY;
        if (I.handle.media_json_path[0]) {
            char mroot[512];
            media_root(mroot, sizeof(mroot));
            MediaImportOptions opt = { .zstd_entries = I.handle.media_zstd };
            if (!media_import_begin(&I.mjob, I.apkg_path, I.handle.media_json_path,
                                    mroot, &opt, &I.media)) {
                char msg[600];
                snprintf(msg, sizeof(msg), "Media could not be imported: %s", I.media.error);
                run_fail(msg);
                return;
            }
            I.mjob_open = true;
        }
        return;
    }

    if (I.run_sub == RUN_MEDIA_COPY) {
        if (I.mjob_open && !media_import_done(&I.mjob)) {
            media_import_step(&I.mjob, 4);
            if (I.mjob.failed) {
                char msg[600];
                snprintf(msg, sizeof(msg), "Media could not be imported: %s", I.media.error);
                run_fail(msg);
            }
            return;
        }
        if (I.mjob_open) { media_import_end(&I.mjob); I.mjob_open = false; }
        /* job_init folds the media warnings into the job's warning list. */
        import_job_init(&I.job, &I.col, &I.media, &I.scratch);
        I.run_sub = RUN_CARDS;
        return;
    }

    if (I.run_sub == RUN_CARDS) {
        if (!import_job_done(&I.job)) {
            import_job_step(&I.job, 120);
            return;
        }
        I.warnings = I.job.warnings;
        I.run_sub = RUN_COMMIT;
        return;
    }

    /* RUN_COMMIT: the only step that touches the user's decks. */
    if (!import_commit_ex(&a->data.decks, &I.scratch, I.mode, &I.warnings, &I.commit_res)) {
        /* Use the last warning line as the explanation. */
        const char *msg = "The import could not be completed.";
        const char *last = NULL;
        for (const char *p = I.warnings.message; *p; ) {
            const char *nl = strchr(p, '\n');
            if (!nl) break;
            last = p;
            p = nl + 1;
        }
        char text[700];
        if (last) {
            const char *nl = strchr(last, '\n');
            snprintf(text, sizeof(text), "%.*s", (int)(nl - last), last);
            msg = text;
        }
        run_fail(msg);
        return;
    }
    I.result_cards = I.commit_res.cards_added;
    I.result_notes = I.job.notes_added;

    media_import_free(&I.media);
    import_job_free(&I.job);

    app_refresh_fonts(a);

    Player *p = &a->data.player;
    if (!p->achievements[ACH_FIRST_CARD].unlocked && I.result_cards > 0) {
        p->achievements[ACH_FIRST_CARD].unlocked = true;
        p->achievements[ACH_FIRST_CARD].unlocked_at = (double)time(NULL);
        p->coins += ACH_DEFS[ACH_FIRST_CARD].coin_reward;
        player_add_xp(p, ACH_DEFS[ACH_FIRST_CARD].xp_reward);
        snprintf(a->ach_popup, sizeof(a->ach_popup), "%s", ACH_DEFS[ACH_FIRST_CARD].name);
        snprintf(a->ach_popup_desc, sizeof(a->ach_popup_desc), "%s", ACH_DEFS[ACH_FIRST_CARD].desc);
        a->ach_popup_t = 3.f;
    }
    if (!app_save_checked(a)) {
        import_warnings_addf(&I.warnings,
            "The imported decks could not be written to disk (is the disk full?). "
            "They are available now but may be lost if you quit.");
    }

    I.phase = IMP_DONE;
    I.phase_t = 0.f;
}

/* ================================================================== */
/*  Update                                                             */
/* ================================================================== */

bool import_ui_update(App *a, float dt) {
    if (I.phase == IMP_IDLE) return false;
    I.phase_t += dt;

    int W = GetScreenWidth(), H = GetScreenHeight();

    /* ---- BROWSE ------------------------------------------------ */
    if (I.phase == IMP_BROWSE) {
        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }

        int rows = 12;

        if (IsKeyPressed(KEY_UP)) {
            I.selected--;
            if (I.selected < 0) I.selected = 0;
            if (I.selected < I.scroll) I.scroll = I.selected;
        }
        if (IsKeyPressed(KEY_DOWN)) {
            I.selected++;
            if (I.selected >= I.entry_count) I.selected = I.entry_count - 1;
            if (I.selected >= I.scroll + rows) I.scroll = I.selected - rows + 1;
        }
        int wheel = (int)GetMouseWheelMove();
        if (wheel != 0) {
            I.scroll -= wheel * 3;
            int max_scroll = I.entry_count > rows ? I.entry_count - rows : 0;
            if (I.scroll > max_scroll) I.scroll = max_scroll;
        }
        if (I.scroll < 0) I.scroll = 0;
        if (I.selected < 0 && I.entry_count > 0) I.selected = 0;

        /* Must match draw_browse(): panel top is H/2-200, list starts 120 below. */
        float list_y = H/2.f - 200 + 120;
        float row_h = 26;
        Rectangle list_rect = { W/2.f - 300, list_y, 600, rows * row_h };

        /* Mouse selection on entries. */
        for (int r = 0; r < rows; r++) {
            int idx = I.scroll + r;
            if (idx >= I.entry_count) break;
            Rectangle row = { list_rect.x + 8, list_rect.y + r * row_h,
                              list_rect.width - 16, row_h };
            if (ui_button_hover(row) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                I.selected = idx;
                if (I.entries[idx].is_apkg) {
                    I.phase = IMP_OPENING; I.phase_t = 0.f; I.open_frames = 0;
                    return true;
                }
                char nd[600];
                snprintf(nd, sizeof(nd), "%s/%s", I.browse_dir, I.entries[idx].name);
                browse_scan(nd);
                return true;
            }
        }

        bool do_open = false, do_up = false;
        if (IsKeyPressed(KEY_ENTER)) {
            if (I.selected >= 0 && I.selected < I.entry_count) {
                if (I.entries[I.selected].is_apkg) do_open = true;
                else {
                    char nd[600];
                    snprintf(nd, sizeof(nd), "%s/%s", I.browse_dir, I.entries[I.selected].name);
                    browse_scan(nd);
                    return true;
                }
            }
        }

        Rectangle btn_open   = { W/2.f - 280, H/2.f + 220, 180, 52 };
        Rectangle btn_up     = { W/2.f - 90,  H/2.f + 220, 180, 52 };
        Rectangle btn_cancel = { W/2.f + 100, H/2.f + 220, 180, 52 };

        if (ui_button_hover(btn_open) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) do_open = true;
        if (ui_button_hover(btn_up) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) do_up = true;
        if (ui_button_hover(btn_cancel) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { close_modal(); return true; }

        if (do_up) { browse_up(); return true; }
        if (do_open && I.selected >= 0 && I.selected < I.entry_count &&
            I.entries[I.selected].is_apkg) {
            I.phase = IMP_OPENING; I.phase_t = 0.f; I.open_frames = 0;
        }
        return true;
    }

    /* ---- OPENING ------------------------------------------------ */
    /* Unpacking a big package takes a moment. Draw "Reading package..." for
       a couple of frames first so the window never looks frozen on a stale
       screen, then do the work. */
    if (I.phase == IMP_OPENING) {
        if (++I.open_frames < 3) return true;
        if (open_selected()) { I.phase = IMP_PREVIEW; I.phase_t = 0.f; }
        else { I.phase = IMP_ERROR; I.phase_t = 0.f; }
        return true;
    }

    /* ---- PREVIEW ----------------------------------------------- */
    if (I.phase == IMP_PREVIEW) {
        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }
        I.tree_scroll -= (int)GetMouseWheelMove();
        if (IsKeyPressed(KEY_DOWN)) I.tree_scroll++;
        if (IsKeyPressed(KEY_UP))   I.tree_scroll--;
        if (I.tree_scroll < 0) I.tree_scroll = 0;
        if (I.tree_scroll > I.tree_count - 1) I.tree_scroll = I.tree_count > 0 ? I.tree_count - 1 : 0;

        Rectangle btn_import = { W/2.f - 280, H/2.f + 200, 180, 56 };
        Rectangle btn_back   = { W/2.f - 90,  H/2.f + 200, 180, 56 };
        Rectangle btn_cancel = { W/2.f + 100, H/2.f + 200, 180, 56 };

        if (ui_button_hover(btn_back) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            I.phase = IMP_BROWSE; I.phase_t = 0.f; return true;
        }
        if (ui_button_hover(btn_cancel) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            close_modal(); return true;
        }

        bool do_import = IsKeyPressed(KEY_ENTER);
        if (ui_button_hover(btn_import) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            do_import = true;

        if (do_import) {
            detect_duplicates(a);
            if (I.dup_any) { I.phase = IMP_DUP; I.phase_t = 0.f; return true; }
            run_begin(a, IMPORT_MODE_NEW);
            I.phase = IMP_RUNNING; I.phase_t = 0.f;
        }
        return true;
    }

    /* ---- DUP (duplicate handling) ------------------------------ */
    if (I.phase == IMP_DUP) {
        if (IsKeyPressed(KEY_ESCAPE)) { I.phase = IMP_PREVIEW; I.phase_t = 0.f; return true; }

        Rectangle btn_new    = { W/2.f - 300, H/2.f + 140, 180, 56 };
        Rectangle btn_update = { W/2.f - 100, H/2.f + 140, 180, 56 };
        Rectangle btn_cancel = { W/2.f + 100, H/2.f + 140, 180, 56 };

        if (ui_button_hover(btn_new) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            run_begin(a, IMPORT_MODE_NEW);
            I.phase = IMP_RUNNING; I.phase_t = 0.f; return true;
        }
        if (ui_button_hover(btn_update) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            run_begin(a, IMPORT_MODE_UPDATE);
            I.phase = IMP_RUNNING; I.phase_t = 0.f; return true;
        }
        if (ui_button_hover(btn_cancel) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            I.phase = IMP_PREVIEW; I.phase_t = 0.f; return true;
        }
        return true;
    }

    /* ---- RUNNING ----------------------------------------------- */
    if (I.phase == IMP_RUNNING) {
        /* Esc cancels at any point before the final commit and rolls back. */
        if (IsKeyPressed(KEY_ESCAPE) && I.run_sub != RUN_COMMIT) {
            run_fail("Import cancelled.");
            return true;
        }
        run_step(a);
        return true;
    }

    /* ---- DONE -------------------------------------------------- */
    if (I.phase == IMP_DONE) {
        /* Same geometry as draw_done(). */
        float by = H/2.f + (I.warnings.count > 0 ? 250 : 190);
        Rectangle btn_study = { W/2.f - 300, by, 220, 56 };
        Rectangle btn_close = { W/2.f - 70,  by, 260, 56 };

        if (IsKeyPressed(KEY_ESCAPE)) { close_modal(); return true; }
        if (ui_button_hover(btn_close) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            close_modal(); return true;
        }
        I.warn_scroll -= GetMouseWheelMove() * 24.f;
        if (I.warn_scroll < 0.f) I.warn_scroll = 0.f;

        if (ui_button_hover(btn_study) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            /* The deck the commit actually landed in (not a name lookup,
               which could hit an older same-named deck). */
            int deck_id = I.commit_res.first_deck_id;
            if (!decklist_find(&a->data.decks, deck_id)) deck_id = -1;
            close_modal();
            if (deck_id >= 0) app_start_session(a, deck_id);
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
    ui_text_center(title, (Rectangle){ m.x, m.y + 22, m.width, 34 }, FONT_MD, TH.text);
}

static void draw_browse(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 340, H/2.f - 200, 680, 480 };
    panel_header(m, "Import Anki Deck");

    ui_text_center("Select a .apkg file",
        (Rectangle){ m.x, m.y + 60, m.width, 22 }, FONT_SM, TH.text_dim);

    ui_text(I.browse_dir, (int)m.x + 40, (int)m.y + 92, FONT_XS, TH.text_muted);

    int rows = 12;
    float list_y = m.y + 120;
    float row_h = 26;
    Rectangle list_rect = { m.x + 40, list_y, m.width - 80, rows * row_h };
    ui_panel_border(list_rect, TH.bg2, TH.border, RADIUS_MD, 1.f);

    for (int r = 0; r < rows; r++) {
        int idx = I.scroll + r;
        if (idx >= I.entry_count) break;
        BrowseEntry *be = &I.entries[idx];
        Rectangle row = { list_rect.x + 6, list_rect.y + r * row_h,
                          list_rect.width - 12, row_h - 2 };
        if (idx == I.selected) ui_panel(row, TH.primary_lo, RADIUS_SM);

        char label[280];
        snprintf(label, sizeof(label), "%s%s", be->is_dir ? "DIR  " : "      ", be->name);
        Color c = be->is_dir ? TH.text_dim : TH.text;
        ui_text(label, (int)row.x + 10, (int)row.y + 5, FONT_SM, c);
    }

    ui_text_center("Up/Down to move   Enter to open   Esc to cancel",
        (Rectangle){ m.x, m.y + m.height - 120, m.width, 20 }, FONT_XS, TH.text_muted);

    Rectangle btn_open   = { W/2.f - 280, H/2.f + 220, 180, 52 };
    Rectangle btn_up     = { W/2.f - 90,  H/2.f + 220, 180, 52 };
    Rectangle btn_cancel = { W/2.f + 100, H/2.f + 220, 180, 52 };
    ui_button(btn_open, "OPEN", TH.primary, TH.text);
    ui_button(btn_up, "UP", TH.panel_hi, TH.text);
    ui_button(btn_cancel, "CANCEL", TH.panel_hi, TH.text);
}

static void draw_opening(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 260, H/2.f - 80, 520, 160 };
    panel_header(m, "Opening package...");
    ui_text_center("Reading the Anki package. Large decks can take a few seconds.",
        (Rectangle){ m.x + 20, m.y + 80, m.width - 40, 22 }, FONT_XS, TH.text_dim);
}

static void draw_preview(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 360, H/2.f - 260, 720, 580 };
    panel_header(m, "Import Preview");

    float y = m.y + 62;
    char buf[96];

    ui_text_center(apkg_variant_name(I.handle.variant),
        (Rectangle){ m.x, y - 4, m.width, 18 }, FONT_XS, TH.text_muted);
    y += 22;

    struct { const char *label; int n; } head[] = {
        { "Cards",      I.col.card_count },
        { "Notes",      I.col.note_count },
        { "Note types", I.preview_models },
        { "Media files", I.preview_media },
    };
    for (size_t i = 0; i < sizeof(head)/sizeof(head[0]); i++) {
        snprintf(buf, sizeof(buf), "%d", head[i].n);
        ui_text(head[i].label, (int)m.x + 60, (int)y, FONT_SM, TH.text_dim);
        int nw = ui_measure(buf, FONT_MD);
        ui_text(buf, (int)(m.x + m.width - 60 - nw), (int)y - 4, FONT_MD, TH.text);
        y += 28;
    }

    y += 6;
    ui_text("Decks", (int)m.x + 60, (int)y, FONT_SM, TH.text_dim);
    y += 24;

    /* Hierarchy tree, scrollable. */
    float list_bottom = m.y + m.height - 84;
    Rectangle view = { m.x + 56, y, m.width - 112, list_bottom - y };
    BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
    float ry = view.y;
    for (int i = I.tree_scroll; i < I.tree_count && ry < view.y + view.height; i++) {
        TreeRow *r = &I.tree[i];
        int ix = (int)view.x + 12 + r->depth * 22;
        bool leaf = r->cards >= 0;
        ui_text(r->label, ix, (int)ry, FONT_SM, leaf ? TH.text : TH.text_dim);
        if (leaf) {
            snprintf(buf, sizeof(buf), "%d cards", r->cards);
            int nw = ui_measure(buf, FONT_XS);
            ui_text(buf, (int)(view.x + view.width - 12 - nw), (int)ry + 3, FONT_XS, TH.text_muted);
        }
        ry += 22;
    }
    EndScissorMode();
    if (I.tree_count > 0 && I.tree_scroll + (int)(view.height / 22) < I.tree_count)
        ui_text_center("scroll for more", (Rectangle){ view.x, list_bottom - 2, view.width, 14 },
                       FONT_XS, TH.text_muted);

    if (I.preview_reviewed > 0)
        snprintf(buf, sizeof(buf), "%d cards have review history; their due dates are kept.",
                 I.preview_reviewed);
    else
        snprintf(buf, sizeof(buf), "All cards in this package are new.");
    ui_text_center(buf, (Rectangle){ m.x, m.y + m.height - 62, m.width, 18 }, FONT_XS, TH.text_muted);
    ui_text_center("Your existing decks are not changed unless the import completes.",
        (Rectangle){ m.x, m.y + m.height - 44, m.width, 18 }, FONT_XS, TH.text_muted);

    Rectangle btn_import = { W/2.f - 280, H/2.f + 200, 180, 56 };
    Rectangle btn_back   = { W/2.f - 90,  H/2.f + 200, 180, 56 };
    Rectangle btn_cancel = { W/2.f + 100, H/2.f + 200, 180, 56 };
    ui_button(btn_import, "IMPORT", TH.primary, TH.text);
    ui_button(btn_back, "BACK", TH.panel_hi, TH.text);
    ui_button(btn_cancel, "CANCEL", TH.panel_hi, TH.text);
}

static void draw_dup(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 320, H/2.f - 170, 640, 360 };
    panel_header(m, "Existing Deck Found");

    ui_text_center("A deck with this name already exists.",
        (Rectangle){ m.x, m.y + 66, m.width, 24 }, FONT_SM, TH.text);
    ui_text_center("Choose how to import:",
        (Rectangle){ m.x, m.y + 96, m.width, 24 }, FONT_SM, TH.text_dim);

    ui_text_center("Import as New — creates a separate deck (adds a suffix).",
        (Rectangle){ m.x, m.y + 140, m.width, 20 }, FONT_XS, TH.text_muted);
    ui_text_center("Update Existing — adds only cards not already present.",
        (Rectangle){ m.x, m.y + 162, m.width, 20 }, FONT_XS, TH.text_muted);

    Rectangle btn_new    = { W/2.f - 300, H/2.f + 140, 180, 56 };
    Rectangle btn_update = { W/2.f - 100, H/2.f + 140, 180, 56 };
    Rectangle btn_cancel = { W/2.f + 100, H/2.f + 140, 180, 56 };
    ui_button(btn_new, "IMPORT AS NEW", TH.primary, TH.text);
    ui_button(btn_update, "UPDATE", TH.panel_hi, TH.text);
    ui_button(btn_cancel, "CANCEL", TH.panel_hi, TH.text);
}

static void draw_running(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 300, H/2.f - 100, 600, 220 };
    panel_header(m, "Importing...");

    const char *label = "Reading package...";
    float pct = 0.f;
    char sub[96] = "";

    if (I.run_sub == RUN_MEDIA || I.run_sub == RUN_MEDIA_COPY) {
        label = "Importing media...";
        if (I.mjob_open && I.media.count > 0) {
            pct = (float)I.mjob.next / (float)I.media.count;
            snprintf(sub, sizeof(sub), "%d / %d files", I.mjob.next, I.media.count);
        }
    } else if (I.run_sub == RUN_CARDS) {
        label = "Importing cards...";
        if (I.col.card_count > 0)
            pct = (float)I.job.next_card / (float)I.col.card_count;
        snprintf(sub, sizeof(sub), "%d / %d cards", I.job.next_card, I.col.card_count);
    } else {
        label = "Finishing up...";
        pct = 1.f;
    }
    if (pct < 0.f) pct = 0.f;
    if (pct > 1.f) pct = 1.f;

    ui_text_center(label, (Rectangle){ m.x, m.y + 62, m.width, 24 }, FONT_SM, TH.text_dim);

    Rectangle bar = { m.x + 40, m.y + 104, m.width - 80, 24 };
    ui_progress(bar, pct, TH.primary, TH.bg, RADIUS_MD);
    if (sub[0]) ui_text_center(sub, bar, FONT_SM, TH.text);

    ui_text_center("Press Esc to cancel. Nothing is added to your decks until the end.",
        (Rectangle){ m.x, m.y + 150, m.width, 22 }, FONT_XS, TH.text_muted);
}

/* Rough wrapped height of the warning text, for scroll limits. */
static float warnings_height(float width, int size, int gap) {
    float h = 0.f;
    const char *p = I.warnings.message;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[IMPORT_WARNING_CAP > 600 ? 600 : IMPORT_WARNING_CAP];
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        int lines = (int)(ui_measure(line, size) / width) + 1;
        h += (float)lines * (size + gap);
        if (!nl) break;
        p = nl + 1;
    }
    return h;
}

static void draw_done(int W, int H) {
    dim_background(W, H);
    bool has_warnings = I.warnings.count > 0;
    Rectangle m = { W/2.f - 340, H/2.f - 200, 680, has_warnings ? 530 : 470 };
    panel_header(m, "Import Complete");

    char buf[128];
    snprintf(buf, sizeof(buf), "%d", I.result_cards);
    ui_text_center(buf, (Rectangle){ m.x, m.y + 70, m.width, 80 }, FONT_XL, TH.success);
    ui_text_center("cards imported",
        (Rectangle){ m.x, m.y + 150, m.width, 22 }, FONT_SM, TH.text_dim);

    int decks = I.commit_res.decks_created + I.commit_res.decks_merged;
    if (I.commit_res.cards_skipped_duplicate > 0)
        snprintf(buf, sizeof(buf), "%d notes • %d decks • %d already present",
                 I.result_notes, decks, I.commit_res.cards_skipped_duplicate);
    else
        snprintf(buf, sizeof(buf), "%d notes • %d decks", I.result_notes, decks);
    ui_text_center(buf, (Rectangle){ m.x, m.y + 180, m.width, 24 }, FONT_SM, TH.text_dim);

    if (has_warnings) {
        snprintf(buf, sizeof(buf), "Warnings (%d%s)", I.warnings.count,
                 I.warnings.dropped > 0 ? "+" : "");
        ui_text_center(buf, (Rectangle){ m.x, m.y + 216, m.width, 22 }, FONT_XS, TH.warning);

        Rectangle view = { m.x + 40, m.y + 240, m.width - 80, 120 };
        float total = warnings_height(view.width, FONT_XS, 2);
        float max_scroll = total > view.height ? total - view.height : 0.f;
        if (I.warn_scroll > max_scroll) I.warn_scroll = max_scroll;
        BeginScissorMode((int)view.x, (int)view.y, (int)view.width, (int)view.height);
        ui_text_wrapped(I.warnings.message,
            (Rectangle){ view.x, view.y - I.warn_scroll, view.width, total + 40 },
            FONT_XS, TH.text_muted, 2);
        EndScissorMode();
        if (max_scroll > 0.f)
            ui_text_center("scroll for more",
                (Rectangle){ view.x, view.y + view.height + 2, view.width, 14 }, FONT_XS, TH.text_muted);
    }

    Rectangle btn_study = { W/2.f - 300, H/2.f + (has_warnings ? 250 : 190), 220, 56 };
    Rectangle btn_close = { W/2.f - 70,  H/2.f + (has_warnings ? 250 : 190), 260, 56 };
    ui_button(btn_study, "START STUDYING", TH.primary, TH.text);
    ui_button(btn_close, "BACK TO DECKS", TH.panel_hi, TH.text);
}

static void draw_error(int W, int H) {
    dim_background(W, H);
    Rectangle m = { W/2.f - 320, H/2.f - 160, 640, 340 };
    panel_header(m, "Import Failed");

    ui_text_wrapped(I.error,
        (Rectangle){ m.x + 40, m.y + 70, m.width - 80, 140 },
        FONT_SM, TH.danger, 4);

    ui_text_center("No changes were made to your existing decks.",
        (Rectangle){ m.x, m.y + 230, m.width, 22 }, FONT_XS, TH.text_muted);

    Rectangle btn_ok = { W/2.f - 90, H/2.f + 140, 180, 52 };
    ui_button(btn_ok, "OK", TH.panel_hi, TH.text);
}

void import_ui_draw(App *a) {
    (void)a;
    if (I.phase == IMP_IDLE) return;
    int W = GetScreenWidth(), H = GetScreenHeight();
    switch (I.phase) {
        case IMP_BROWSE:  draw_browse(W, H);  break;
        case IMP_OPENING: draw_opening(W, H); break;
        case IMP_PREVIEW: draw_preview(W, H); break;
        case IMP_DUP:     draw_dup(W, H);     break;
        case IMP_RUNNING: draw_running(W, H); break;
        case IMP_DONE:    draw_done(W, H);    break;
        case IMP_ERROR:   draw_error(W, H);   break;
        default: break;
    }
}

/* ================================================================== */
/*  Entry                                                              */
/* ================================================================== */

void import_ui_start(App *a) {
    (void)a;
    reset_state();
    I.phase = IMP_BROWSE;
    I.phase_t = 0.f;

    const char *home = getenv("HOME");
    if (!home || !*home) home = ".";
    browse_scan(home);
}
