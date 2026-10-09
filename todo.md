Todo
[✓]
Phase 0.5: Convert Deck.cards to heap/growable array; raise MAX_DECKS; dynamic StudySession.queue
[✓]
Phase 0.5: Build and run tests; fix regressions
[✓]
Phase 1: Harden apkg_open (caps, zstd-bomb guard) + archive tests
[✓]
Phase 2: Fix anki_json deck-ID bug + parser tests
[✓]
Phase 3: import_manager orchestrator + per-deck hierarchy grouping
[✓]
Phase 4: HTML/template handling verification + warnings
[✓]
Phase 5: Media import progress + recoverable warnings surfaced
         (incremental MediaImportJob, UI progress bar, warnings folded into the job, content-safe collisions, rollback)
[✓]
Phase 6: Scheduling conversion (due dates preserved via col.crt), duplicate detection, transactional commit + rollback, atomic save
[✓]
Phase 7: Import UI (built-in file browser, hierarchy tree preview, progress + Esc cancel, scrollable warnings, correct "Start studying" target)
[✓]
Phase 8: Security/robustness hardening (streaming zip/zstd with caps, size limits, O(n log n) planning/lookup, hardened SQLite)
[✓]
Phase 9: Real-package integration fixtures + persistence-across-restart test
[✓]
Phase 10: REAL modern Anki support. Earlier phases were validated only on synthetic fixtures, so genuine
          Anki 2.1.50+ exports (zstd frame with no content size, schema-18 notetypes/fields/templates/decks
          tables with protobuf blobs, protobuf zstd media list, zstd media files, `unicase` collation) failed.
          Now covered by tests/fixtures/real_*.apkg (generated with the official `anki` library:
          scripts/make_real_fixtures.py).

[✓]
Phase 11: Study screen fixes found with the real Kaishi 1.5k deck
          - image-only fields (e.g. Kaishi's "Picture") were skipped because their text is empty; now laid out and drawn
          - card height is measured from real content (was estimated: 26px per field + 162px per image) and clamped
            between the header and the rating buttons; taller content scrolls (wheel, Up/Down, PgUp/PgDn) with a scrollbar
          - multi-line fields (\n / <br>) are word-wrapped and centred (ui_text_rich_layout in ui.c)

Known limitations / ideas (not done)
[ ] Kaishi's front template also shows the Sentence under the Word; StudyQuest's front shows field 0 only
[ ] Cloze deletions are imported as literal "{{c1::text}}" (warned in the result screen)
[ ] Multi-template note types (e.g. "Basic (and reversed card)") import every card as Front->Back; the reversed card does not swap sides (warned)
[ ] Media filename normalization: Anki compares NFC; a deck referencing NFD names (macOS-made) will not resolve
[ ] Suspended/buried Anki cards import as active (warned)

Notes for the next session
- Build the two test binaries and run:  ./build/studyquest_anki_import_tests tests/fixtures/anki_media.apkg
- Media root used by the UI is ~/.studyquest/media; never name a test media root "<tmpdir>/media" (apkg_open extracts the media *map* file there).
- The app locates assets/fonts relative to the working directory: run it from the repo root.
