"""Generate REAL Anki packages with the official `anki` Python library.

These are the ground-truth fixtures for the importer tests: they are produced
by Anki's own exporter, so they have the exact on-disk layout real users'
packages have (schema-18 collection, zstd frames without a content size,
protobuf media list, zstd-compressed media files, ...).

    python3 -m venv /tmp/ankienv && /tmp/ankienv/bin/pip install anki
    /tmp/ankienv/bin/python scripts/make_real_fixtures.py [out_dir]

The committed fixtures were generated with anki 26.09.3. Regenerating them
changes ids/timestamps, so tests/test_anki_import.c would need the new values
(deck/card ids are not asserted; counts, names and field text are).
"""
import os, shutil, struct, zlib, time, sys
from anki.collection import Collection, ExportAnkiPackageOptions, DeckIdLimit

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "fixtures")
OUT = os.path.abspath(OUT)
os.makedirs(OUT, exist_ok=True)

def png(w=4, h=4, rgb=(200, 30, 30)):
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

def wav(secs=0.05, rate=8000):
    n = int(secs * rate)
    data = b"\x00\x00" * n
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data))
    return hdr + data

import tempfile
col_path = os.path.join(tempfile.mkdtemp(), "src.anki2")
for f in (col_path, col_path + "-wal"):
    if os.path.exists(f): os.remove(f)
col = Collection(col_path)

# ---- media
col.media.write_data("cat.png", png())
col.media.write_data("日本語.png", png(rgb=(10, 120, 200)))
col.media.write_data("hello.wav", wav())
col.media.write_data("evil name?.wav", wav(0.02))

# ---- decks (hierarchy)
d_vocab = col.decks.id("Japanese::Vocabulary")
d_kanji = col.decks.id("Japanese::Kanji")
d_gram  = col.decks.id("Japanese::Grammar::N5")

basic = col.models.by_name("Basic")
rev   = col.models.by_name("Basic (and reversed card)")
cloze = col.models.by_name("Cloze")

def add(model, did, fields, tags=""):
    n = col.new_note(model)
    for k, v in fields.items(): n[k] = v
    n.tags = tags.split() if tags else []
    col.add_note(n, did)
    return n

n1 = add(basic, d_vocab, {"Front": "こんにちは", "Back": "Hello <b>(greeting)</b>"}, "jlpt-n5 vocabulary")
n2 = add(basic, d_vocab, {"Front": "食べる", "Back": "to eat<br>たべる [sound:hello.wav]"}, "jlpt-n5 verb")
n3 = add(rev,   d_vocab, {"Front": "学生", "Back": "student <i>(noun)</i>"}, "vocabulary chapter-3")      # 2 cards
n4 = add(basic, d_kanji, {"Front": "日本語", "Back": '<img src="日本語.png"> Japanese language'}, "kanji")
n5 = add(basic, d_kanji, {"Front": "猫", "Back": '<img src="cat.png"><br>cat [sound:evil name?.wav]'}, "kanji animals")
n6 = add(cloze, d_gram,  {"Text": "日本語を{{c1::勉強}}しています。", "Back Extra": "I am studying Japanese."}, "grammar")
n7 = add(basic, d_gram,  {"Front": "ありがとう", "Back": "thank you<ul><li>polite</li><li>casual</li></ul>"}, "grammar")
n8 = add(basic, d_vocab, {"Front": "おはよう", "Back": "good morning"}, "")

# ---- schedule some cards as reviews
cids = col.find_cards("deck:Japanese*")
for cid in list(col.find_cards(f'nid:{n2.id}')) + list(col.find_cards(f'nid:{n1.id}')):
    c = col.get_card(cid)
    c.type = 2; c.queue = 2; c.ivl = 21; c.factor = 2350; c.reps = 7; c.lapses = 1
    c.due = col.sched.today + 5
    col.update_card(c)
c = col.get_card(col.find_cards(f'nid:{n7.id}')[0])
c.type = 1; c.queue = 1; c.reps = 1; c.due = int(time.time()) + 600
col.update_card(c)

# ---- a filtered deck pulling cards from Vocabulary
try:
    col.sched.add_or_update_filtered_deck  # presence check
    fd = col.sched.get_or_create_filtered_deck(deck_id=0)
    fd.name = "Filtered Study"
    fd.config.search_terms[0].search = f'nid:{n8.id}'
    fd.config.search_terms[0].limit = 10
    col.sched.add_or_update_filtered_deck(fd)
except Exception as e:
    print("filtered deck skipped:", e)

print("cards:", col.card_count(), "notes:", col.note_count(), "crt:", col.crt)

def export(name, legacy, with_sched=True, with_media=True, limit=None):
    opts = ExportAnkiPackageOptions(with_scheduling=with_sched, with_deck_configs=True, with_media=with_media, legacy=legacy)
    lim = limit or DeckIdLimit(deck_id=col.decks.id("Japanese"))
    p = os.path.join(OUT, name)
    n = col.export_anki_package(out_path=p, options=opts, limit=lim)
    print(name, n, os.path.getsize(p))

export("real_modern.apkg", legacy=False)
export("real_legacy.apkg", legacy=True)
export("real_modern_nosched.apkg", legacy=False, with_sched=False)
export("real_modern_nomedia.apkg", legacy=False, with_media=False)
col.close()
