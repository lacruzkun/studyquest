# StudyQuest

An Anki-style spaced-repetition flashcard game in C + raylib.

Learn → rate → gain XP → level up → maintain your streak → unlock achievements.

## Requirements
- C11 compiler
- CMake ≥ 3.16
- raylib ≥ 4.5 (found via `find_package(raylib)`; will be fetched automatically if missing)

## Build
```sh
mkdir build
cd build
cmake ..
cmake --build . -j
./studyquest

Test
sh

./studyquest_tests

Save data

~/.studyquest.sav (a compact binary format with a versioned header).
Controls

    Space / Enter — reveal answer

    1 / 2 / 3 / 4 — Again / Hard / Good / Easy

    Esc — go back

    Ctrl+Enter — save a card while editing

Import / export

Deck export writes <deckname>.csv with front,back,tags in the working
directory. To import, place import_<deckid>.csv next to the binary and click
Import in the Decks screen.
Architecture

See src/ — separated into cards, study, player, storage, ui,
core, screens. The scheduling algorithm lives entirely in
src/study/scheduler.c and is pure logic with no raylib dependency.
text


---

## Architecture summary

- **`cards/`** — pure data: `Card`, `Deck`, `DeckList` (fixed-size, no dynamic alloc). Includes CSV import/export and the sample deck.
- **`study/scheduler`** — pure logic, no raylib dependency. Implements an SM-2-inspired algorithm with `RATING_AGAIN / HARD / GOOD / EASY`, ease factors clamped to `[1.3, 3.0]`, and a "relearning" step. Everything UI touches is through `scheduler_apply(card, rating, now)` — the algorithm can be swapped without touching the UI.
- **`player/`** — pure logic. XP curve is `100 * 1.5^(level-1)` (100, 150, 225, 337, …). Streak uses day numbers `(unix / 86400)` stored as `int64_t`. Achievements are declared in `achievements.c` (definitions) with per-player unlock state. Quests are rolled deterministically per day from a seed.
- **`storage/save`** — explicit field-by-field binary serialization with a magic + version header, so the format can evolve safely.
- **`ui/`** — theme constants live in `core/theme.h`. Widget helpers (`ui_panel`, `ui_progress`, `ui_button`, `ui_text_wrapped`) keep rendering consistent. No magic numbers in screen code.
- **`screens/`** — one file per screen; each has `_update` and `_draw`. All screens go through `app_goto` which handles transitions.
- **`core/app`** — state machine + toast/level-up/achievement popups.

## Spaced repetition

Each card tracks `state`, `interval_sec`, `ease`, `reps`, `lapses`, `due`, `last_review`. On a rating:

| Rating | New card | Review card |
|---|---|---|
| Again  | 1 min, state=relearning | `interval * 0.4`, ease −= 0.20, `lapses++` |
| Hard   | 6 min | `interval * 1.2`, ease −= 0.15 |
| Good   | 1 day | `interval * ease` |
| Easy   | 3 days | `interval * ease * 1.3`, ease += 0.15 |

Ease is clamped to `[1.3, 3.0]`. The rating UI shows the resulting interval for each button before the user clicks.

## Game progression

- **XP** is earned per review (1 / 5 / 10 / 15 for Again / Hard / Good / Easy). This deliberately rewards genuine correct recall, not spamming Again.
- **Levels** use a compounding curve. Level-up pauses the screen with an animated overlay.
- **Coins** are earned per review plus achievements and quests.
- **Streak** is a day-based counter, safe against restarting the app (uses `last_study_day`).
- **Quests** roll over each day; three deterministic quests. Completing a quest grants XP + coins immediately.
- **Achievements** are checked after every review — cheap because they're simple integer compares.

## Build & run

```sh
mkdir build && cd build
cmake ..
cmake --build . -j
./studyquest

Test core logic (no raylib needed):
sh

./studyquest_tests

Files created

All files listed in the structure above. No pre-existing files were modified.
Dependencies

    raylib (5.x recommended) — windowing, rendering, input, timing.

    libc — stdio, stdlib, string, time, math.

    CMake — build system.

No SQLite (kept dependencies minimal; the binary save is versioned and complete).
