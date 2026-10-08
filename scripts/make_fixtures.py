#!/usr/bin/env python3
"""Generate Anki .apkg test fixtures for StudyQuest's import tests.

Produces (in tests/fixtures/):

  modern.apkg               collection.anki21b (zstd) + dummy collection.anki2
  legacy2.apkg              collection.anki21 (real) + dummy collection.anki2
  invalid_no_collection.apkg  a valid ZIP with no collection file
  invalid_corrupt.apkg        not a ZIP at all (plain text bytes)

Run from the repository root:

    python3 scripts/make_fixtures.py

Requires: python3 (stdlib sqlite3 + zipfile) and the `zstd` CLI on PATH.
"""
import json
import os
import sqlite3
import subprocess
import sys
import zipfile

FIXTURES = os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures")


def build_collection(path):
    """Build a minimal but valid Anki collection SQLite file at `path`."""
    models = {
        "1700000000000": {
            "id": 1700000000000,
            "name": "Basic (Japanese)",
            "type": 0,
            "flds": [
                {"name": "Expression", "ord": 0},
                {"name": "Meaning", "ord": 1},
            ],
            "tmpls": [
                {
                    "name": "Card 1",
                    "qfmt": "{{Expression}}",
                    "afmt": "{{FrontSide}}<hr id=answer>{{Meaning}}",
                }
            ],
        }
    }
    decks = {
        "1": {"id": 1, "name": "Japanese::Vocabulary", "dyn": 0},
        "2": {"id": 2, "name": "Japanese::Kanji", "dyn": 0},
    }
    notes = [
        # id, guid, mid, tags, flds (0x1f joined), sfld, did, ord
        (1700000000001, "gu1", 1700000000000, " jlpt-n5 vocabulary ",
         "こんにちは\x1fHello", "こんにちは", 1, 0),
        (1700000000002, "gu2", 1700000000000, " jlpt-n5 ",
         "食べる\x1fTo eat", "食べる", 1, 0),
        (1700000000003, "gu3", 1700000000000, " kanji ",
         "学生\x1fStudent", "学生", 2, 0),
    ]
    cards = []
    cid = 1700000001001
    for (nid, _g, mid, _tags, flds, _sfld, did, ord_) in notes:
        cards.append((cid, nid, did, ord_, 1700000000, 0, 0, 0, 0, 2500, 0, 0))
        cid += 1

    db = sqlite3.connect(path)
    db.execute(
        "CREATE TABLE col ("
        "id integer primary key, crt integer, mod integer, scm integer,"
        "ver integer, dty integer, usn integer, ls integer,"
        "conf text, models text, decks text, dconf text, tags text)"
    )
    db.execute(
        "INSERT INTO col (id, crt, mod, scm, ver, dty, usn, ls, conf, models, decks, dconf, tags)"
        " VALUES (1, 0, 0, 0, 11, 0, 0, 0, ?, ?, ?, ?, ?)",
        (json.dumps({}), json.dumps(models), json.dumps(decks), json.dumps({}), "{}"),
    )
    db.execute(
        "CREATE TABLE notes ("
        "id integer primary key, guid text, mid integer, mod integer, usn integer,"
        "tags text, flds text, sfld integer, csum integer, flags integer, data text)"
    )
    for (nid, guid, mid, tags, flds, sfld, _did, _ord) in notes:
        db.execute(
            "INSERT INTO notes (id, guid, mid, mod, usn, tags, flds, sfld, csum, flags, data)"
            " VALUES (?, ?, ?, 0, 0, ?, ?, ?, 0, 0, '')",
            (nid, guid, mid, tags, flds, sfld),
        )
    db.execute(
        "CREATE TABLE cards ("
        "id integer primary key, nid integer, did integer, ord integer, mod integer,"
        "usn integer, type integer, queue integer, due integer, ivl integer,"
        "factor integer, reps integer, lapses integer, left integer,"
        "odue integer, odid integer, flags integer, data text)"
    )
    for (cid, nid, did, ord_, mod, type_, queue, due, ivl, factor, reps, lapses) in cards:
        db.execute(
            "INSERT INTO cards (id, nid, did, ord, mod, usn, type, queue, due, ivl,"
            " factor, reps, lapses, left, odue, odid, flags, data)"
            " VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0, 0, '')",
            (cid, nid, did, ord_, mod, type_, queue, due, ivl, factor, reps, lapses),
        )
    db.commit()
    db.close()


def empty_schema_db(path):
    """Anki's compatibility `collection.anki2` is a schema-only dummy."""
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE col (id integer primary key)")
    db.commit()
    db.close()


def zstd_compress(src, dst):
    with open(dst, "wb") as out:
        subprocess.run(["zstd", "-q", "-c", src], stdout=out, check=True)


def make_zip(path, entries):
    """entries: list of (arcname, bytes) written with a fixed timestamp."""
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for arcname, data in entries:
            info = zipfile.ZipInfo(arcname, date_time=(2020, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)


def write_package(apkg_path, collection_arcname, collection_bytes,
                  include_dummy_anki2=True, media_map=None, media_files=None):
    entries = [(collection_arcname, collection_bytes)]
    if include_dummy_anki2:
        dummy = "dummy.anki2"
        empty_schema_db(dummy)
        try:
            with open(dummy, "rb") as f:
                entries.append(("collection.anki2", f.read()))
        finally:
            os.unlink(dummy)
    if media_map is not None:
        entries.append(("media", json.dumps(media_map).encode("utf-8")))
        for arcname, data in (media_files or []):
            entries.append((arcname, data))
    make_zip(apkg_path, entries)


def main():
    os.makedirs(FIXTURES, exist_ok=True)
    tmp = "/tmp/sq_make_fixtures_collection.anki21"
    for stale in (tmp, tmp + ".zst"):
        if os.path.exists(stale):
            os.unlink(stale)

    # --- modern: zstd-compressed collection.anki21b -----------------
    build_collection(tmp)
    zstd_compress(tmp, tmp + ".zst")
    with open(tmp + ".zst", "rb") as f:
        write_package(
            os.path.join(FIXTURES, "modern.apkg"),
            "collection.anki21b", f.read(),
            include_dummy_anki2=True,
            media_map={},
        )

    # --- legacy2: plain collection.anki21 + dummy anki2 -------------
    with open(tmp, "rb") as f:
        write_package(
            os.path.join(FIXTURES, "legacy2.apkg"),
            "collection.anki21", f.read(),
            include_dummy_anki2=True,
            media_map={},
        )

    # --- invalid: zip without any collection file -------------------
    make_zip(os.path.join(FIXTURES, "invalid_no_collection.apkg"),
             [("readme.txt", b"not an anki package")])

    # --- invalid: not a zip at all ----------------------------------
    with open(os.path.join(FIXTURES, "invalid_corrupt.apkg"), "wb") as f:
        f.write(b"this is definitely not a zip archive" * 10)

    os.unlink(tmp)
    os.unlink(tmp + ".zst")
    print("fixtures written to", os.path.abspath(FIXTURES))


if __name__ == "__main__":
    sys.exit(main())
