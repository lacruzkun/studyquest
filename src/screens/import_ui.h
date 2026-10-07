#ifndef IMPORT_UI_H
#define IMPORT_UI_H

#include "core/app.h"
#include <stdbool.h>

/* Opens the import modal. Call from decks_update when the user clicks
   the IMPORT button. */
void import_ui_start(App *a);

/* Called from decks_update at the very top. Returns true if the modal
   is open — the caller should then return immediately and skip its own
   input handling. */
bool import_ui_update(App *a, float dt);

/* Called from decks_draw at the very end, so the modal draws on top. */
void import_ui_draw(App *a);

/* True if the modal is currently visible. */
bool import_ui_active(void);

#endif
