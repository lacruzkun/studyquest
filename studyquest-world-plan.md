# StudyQuest — 3D Adventure World: Phase 0 Plan

Status: **approved** with the default decisions below. Phase 1 is implemented (see the Phase 1 report in chat).
Corrections since the plan was first posted: in the scheduler, a *new* card rated Again returns after **60 s** (not 10 min). This does not change the design — the once-per-card-per-day credit rule makes short relearn intervals irrelevant to farming.

## Approved decisions
1. **Credit model:** world credit = accumulated review XP (the value `rating_reward` already awards: 1/5/10/15). Not derived from lifetime XP (quest/achievement/milestone XP would feed back).
2. **Which decks count:** all decks.
3. **Anti-farm caps (world credit only; XP/scheduler untouched):** once per card per day, 300 credited reviews/day, 50 credited new-card first reviews/day, card must have been due.
4. **Pacing:** 5 levels per rank, `cost(L) = round(100 · 1.045^(L−1))` → 105 levels, 223,725 total credit.
5. **Naming:** world = "Level"; the existing XP level is shown compactly as "Hero Lv".
6. **Migration:** v8 saves backfill credit as `15·easy + 10·(good−easy) + 1·(reviews−correct)` (lower bound); no retroactive XP/coins; cosmetics for reached milestones unlocked; no animations replay.

## 1. Existing architecture and findings
C11 / CMake / raylib 5.5, ~13k lines excluding vendored code. `App` holds an 8-screen state machine; `app_draw` wraps screens in `BeginMode2D` (Camera2D for transitions/shake). Binary versioned save (`SAVE_VERSION`, atomic tmp+rename). Scheduler is pure SM-2-style logic.

Findings that shape the design:
- Player level curve `100·1.5^(L−1)` cannot serve 100 levels (level 25 ≈ 3.4M cumulative XP; `int` overflow at L43). World levels therefore use their own thresholds; `player.level` is untouched.
- Single study hook: `apply_rating_and_start_exit()` in `study.c` (needs `was_due` captured before `scheduler_apply`).
- The dashboard is the app hub (nav to Decks/Achievements/Stats/Settings, deck picker, quests, Esc→Welcome). The world home must keep these reachable.
- `EndMode3D` resets the modelview matrix to identity (verified in raylib 5.5 source), so 3D must be drawn outside the `BeginMode2D` block.
- Pre-existing farming exposure: new cards are due immediately; editor-created cards and deck delete/re-import yield fresh XP-bearing cards.
- The app is not Japanese-only (sample decks include "C Programming").
- `save_load` failure falls back to defaults and later overwrites the old file — the world block must fail soft.
- Noticed, not touching: level-up overlay shows "+100 Coins" but grants none; CMake's vendored-zstd fallback path doesn't match the zip layout (system zstd works).

## 2. Reference images (`references/`, mapped by content)
- `…25` 3D forest gorge with ruins/bridge: environmental density, fog-layered depth, hazy distant landmark as goal.
- `…26` top-down painterly trail: path as dominant ribbon, asymmetrical foliage clusters, warm path vs green, pink canopy (sakura cue).
- `…27` mobile learning map: alternating chunky nodes on a vertical stone path, gold challenge coin + flag callout, grey padlock nodes, chests, silhouetted "?" landmarks, signboards, light HUD.
- `…28` rank ladder: emblem + ribbon per tier, material escalation wood→bronze→silver→gold→platinum→master→champion.

## 3. Visual direction
Stylized low-poly diorama (procedural faceted geometry, vertex-color gradients, soft directional light, fog). No external model/texture assets. Japanese motifs: torii, stone lanterns, shrines, pagodas, bamboo, sakura. HUD keeps the existing indigo/amber palette.

Rank emblems: 7 material families × 3 grades — Wood/clay (Noob–Novice), Bronze (Apprentice–Veteran), Silver (Elite–Master), Gold (Grandmaster–Legend), Platinum/crystal (Mythic–Divine), Violet/aether (Demigod–Supreme God), Prismatic/cosmic (Cosmic–Absolute). Grade adds ornament (ribbon, wings, crown, halo, flame).

## 4. Rendering and camera
- Draw the world *before* `BeginMode2D` when the home is active (keeps 4× MSAA); HUD stays in the 2D pass with existing fonts. Phase 2 starts with a spike to prove this, `RenderTexture` as fallback.
- Perspective camera tilted ~50–55° looking along −Z; path is a spline that also rises with progress.
- Terrain: heightfield chunks per region, built once into static meshes with baked vertex colors (height/slope/AO); nearby regions full detail, distant regions simplified silhouettes.
- Props via `DrawMeshInstanced`, seeded per region, ≈ one draw call per prop type per visible chunk.
- One custom GLSL shader: directional Lambert + hemispheric ambient + height/distance fog into a sky gradient. Shadows: baked AO + blob shadows; shadow map is a stretch goal only if it holds up.
- Rail camera along the path with look-ahead; wheel/drag/PgUp/PgDn/arrows; clamped; `focus(level)` using the existing `Spring`; auto-focus on open.
- Labels via `GetWorldToScreen` (existing fonts → Japanese rendering unaffected); node picking by ray vs bounding spheres of visible nodes.
- Procedural low-poly character with idle and walk cycles; cosmetic variants as rewards.

## 5. Architecture (`src/world/`)
`world_data` (static tables, level generation) · `world_progress` (pure logic, no raylib) · `world_layout` (path + node positions) · `world_render` · `world_camera` · `world_anim` · `world_ui`. Serialization lives in `storage/save.c` (existing convention). `dashboard_update/draw` keep their names so `screens.h`/`app.c` change little.

## 6. Data model
```
RankDef   { name, first_level, level_count, material, grade }
RegionDef { name, rank_id, theme }
LevelDef  { id, world_id, rank_id, region_id, kind, milestone, credit_cost, credit_cum, reward{xp,coins,cosmetic} }
kind: STANDARD | RANK_GATE | CHALLENGE | TREASURE | BONUS   (last three reserved for Phase 7)
```
Levels are generated from parameters; everything is keyed by `world_id` for future worlds. Pacing checkpoints (cumulative credit): L10 1,229 · L25 4,456 · L50 17,852 · L100 179,089 · L105 223,725.

## 7. Progression rules
- Level completion is derived from `credit_total`; no button can advance it.
- Rank-up = completing a rank's gate level (every 5th level).
- Node states: Completed (behind), Current (character stands here, progress ring), Available (lit next node + unlocked optional side nodes), Locked.
- One-time rewards are guarded by claim bits set in the same step as the grant; one `save_write` persists grant + claim atomically.
- Reward XP never feeds world credit.
- Challenges are optional and read-only against review data; no alternative scheduler.

## 8. Persistence
`SAVE_VERSION 9`; world block (length-prefixed, explicit little-endian, bounds-checked) after the player block. Holds credit, completed count, `shown_level`, claim bits, cosmetics, daily anti-farm state, pending events. `shown_level` advances only when a travel animation finishes, so a seen move never replays and an unseen one still plays. Old saves are copied once to `.studyquest.sav.v8.bak` before the first v9 write. A damaged world block falls back to legacy-derived progress instead of failing the load.

## 9. Performance budget
~150 draw calls, ~300k visible triangles, frustum culling by chunk, meshes built once, no per-frame allocation, ambient animation honors `anim_enabled()`. A debug overlay (frame time, draw calls, triangles) is for the user's GPU; sandbox numbers (llvmpipe) are relative only.

## 10. Testing strategy
New `studyquest_world_tests` (rules, caps, persistence, migration, corruption); existing suites after every phase; Xvfb + llvmpipe screenshots for visual review (click automation needs the button held ≥1 frame); an end-to-end study session compared against the unmodified build.

## 11. Risks
Save-format change (gated, fail-soft, backup) · `app.c` draw-path change (spike first, `RenderTexture` fallback) · `study.c` hook (one call site, `was_due` before scheduler) · explicit test source lists in CMake · no real-GPU FPS in sandbox · losing dashboard functions (nav strip, deck picker in Continue panel, quests drawer) · procedural art quality (screenshot review each phase).

## 12. Implementation order
1. Baseline commit + decisions. 2. **Phase 1** data/progress/save v9/migration/hook/tests. 3. Phase 2 render-pass spike, camera, terrain/props for 2–3 regions, shader, debug overlay. 4. Phase 3 path/layout, nodes, character, all 21 regions. 5. Phase 4 finish study wiring, rewards/event UX, tests. 6. Phase 5 home swap, rank HUD, panels, Continue, nav strip. 7. Phase 6 travel/unlock/rank-up animation, ambient motion. 8. Phase 7 milestone/treasure/challenge nodes, multi-world shape. 9. Phase 8 regression, performance, visual review.
