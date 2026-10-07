#ifndef APP_H
#define APP_H

#include "raylib.h"
#include "storage/save.h"
#include "study/scheduler.h"
#include "core/fonts.h"
#include "core/anim.h"
#include "core/assets.h"

typedef enum {
    SCREEN_WELCOME,
    SCREEN_DASHBOARD,
    SCREEN_DECKS,
    SCREEN_STUDY,
    SCREEN_SESSION_COMPLETE,
    SCREEN_ACHIEVEMENTS,
    SCREEN_STATS,
    SCREEN_SETTINGS
} Screen;

typedef struct StudySession {
    int  deck_id;
    int  queue[MAX_CARDS];
    int  queue_len;
    int  current;
    bool revealed;
    int  session_total;
    int  session_done;
    int  xp_gained;
    int  coins_gained;
} StudySession;

/* A floating "+25 XP" or "+3 coins" text that flies from a start point
 * to a target and, on arrival, emits particles and pulses the target. */
typedef struct RewardFlight {
    bool    active;
    Vector2 start;
    Vector2 end;
    char    text[48];
    Color   color;
    float   duration;
    float   elapsed;
    float   delay;
} RewardFlight;

#define MAX_REWARD_FLIGHTS 12

typedef struct App {
    Screen screen;
    Screen prev_screen;
    float  screen_t;
    TransitionStyle transition_style;

    SaveData data;
    char save_path[512];

    FontSet        fonts;
    AssetCache     assets;      /* NEW */
    ParticleSystem particles;
    Shake          shake;
    StudySession   session;

    /* Reward flights and their target pulse */
    RewardFlight flights[MAX_REWARD_FLIGHTS];
    float        xp_pulse;      /* 1 → 0 after a flight lands */

    /* Toast */
    char  toast[128];
    float toast_t;
    Color toast_color;

    /* Level-up overlay */
    int   levelup_to;
    float levelup_t;

    /* Achievement popup */
    char  ach_popup[64];
    char  ach_popup_desc[128];
    float ach_popup_t;

    float dt;
} App;

App *app_create(void);
void app_destroy(App *a);
void app_update(App *a, float dt);
void app_draw(App *a);

void app_save(App *a);
void app_goto(App *a, Screen s);
void app_toast(App *a, const char *msg, Color c);
void app_start_session(App *a, int deck_id);
void app_refresh_fonts(App *a);

/* Spawn a reward flight. */
void app_spawn_flight(App *a, Vector2 start, Vector2 end,
                      const char *text, Color color);

#endif
