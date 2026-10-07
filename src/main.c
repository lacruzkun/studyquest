#include "raylib.h"
#include "core/app.h"
#include "core/theme.h"

int main(void) {
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "StudyQuest");
    InitAudioDevice();
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); /* we handle Escape ourselves */
    SetRandomSeed((unsigned int)GetTime() * 1000);

    App *app = app_create();

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        app_update(app, dt);

        BeginDrawing();
        ClearBackground(TH.bg);
        app_draw(app);
        EndDrawing();
    }

    app_destroy(app);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
