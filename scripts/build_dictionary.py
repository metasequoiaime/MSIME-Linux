#!/usr/bin/env python3

import sqlite3
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DICTIONARY_ROOT = ROOT / "vendor" / "MetasequoiaImeDict"
OUTPUT = DICTIONARY_ROOT / "out" / "msime.db"
OTHERS_OUTPUT = DICTIONARY_ROOT / "out" / "others.db"
ENGLISH_OUTPUT = DICTIONARY_ROOT / "out" / "english.db"
BUILD_SCRIPTS = DICTIONARY_ROOT / "makecikudb" / "quanpindb" / "makedb" / "multi_table_has_jp"
MIX_BUILD_SCRIPTS = DICTIONARY_ROOT / "makecikudb" / "mixdb"
EMOJI_BUILD_SCRIPT = DICTIONARY_ROOT / "makecikudb" / "emojidb" / "build_emoji_db.py"
KAOMOJI_BUILD_SCRIPT = DICTIONARY_ROOT / "makecikudb" / "kaomoji" / "build_kaomoji_db.py"
ENGLISH_BUILD_SCRIPTS = DICTIONARY_ROOT / "makecikudb" / "englishdb" / "makedb"


def main() -> None:
    if (
        not BUILD_SCRIPTS.is_dir()
        or not MIX_BUILD_SCRIPTS.is_dir()
        or not EMOJI_BUILD_SCRIPT.is_file()
        or not KAOMOJI_BUILD_SCRIPT.is_file()
        or not ENGLISH_BUILD_SCRIPTS.is_dir()
    ):
        raise SystemExit("Dictionary sources are missing. Run git submodule update --init --recursive first.")

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.unlink(missing_ok=True)
    OTHERS_OUTPUT.unlink(missing_ok=True)
    ENGLISH_OUTPUT.unlink(missing_ok=True)
    for script in ("create_db_and_table.py", "insert_data.py", "create_index_for_db.py"):
        subprocess.run(["python3", str(BUILD_SCRIPTS / script)], cwd=DICTIONARY_ROOT, check=True)
    for script in ("01create_table.py", "03insert_data.py", "02create_index.py", "04verify_db.py"):
        subprocess.run(["python3", str(MIX_BUILD_SCRIPTS / script)], cwd=DICTIONARY_ROOT, check=True)
    subprocess.run(["python3", str(EMOJI_BUILD_SCRIPT)], cwd=DICTIONARY_ROOT, check=True)
    subprocess.run(["python3", str(KAOMOJI_BUILD_SCRIPT)], cwd=DICTIONARY_ROOT, check=True)
    for script in ("create_db_and_table.py", "insert_data.py", "verify_db.py"):
        subprocess.run(["python3", str(ENGLISH_BUILD_SCRIPTS / script)], cwd=DICTIONARY_ROOT, check=True)

    with sqlite3.connect(OUTPUT) as database:
        integrity = database.execute("PRAGMA integrity_check").fetchone()
        candidate = database.execute(
            "SELECT value FROM tbl_2_n WHERE key = ? ORDER BY weight DESC LIMIT 1", ("ni'hao",)
        ).fetchone()
        quick_phrase = database.execute(
            "SELECT value FROM quick_parases WHERE key = ? ORDER BY weight DESC,value LIMIT 1", ("yyds",)
        ).fetchone()
    with sqlite3.connect(OTHERS_OUTPUT) as database:
        others_integrity = database.execute("PRAGMA integrity_check").fetchone()
        emoji = database.execute(
            "SELECT emoji FROM emoji_pinyin WHERE key = ? ORDER BY sort_order LIMIT 1", ("xiaolian",)
        ).fetchone()
        kaomoji = database.execute(
            "SELECT kaomoji FROM kaomoji WHERE pinyin = ? ORDER BY sort_order LIMIT 1", ("haixiu",)
        ).fetchone()
    with sqlite3.connect(ENGLISH_OUTPUT) as database:
        english_integrity = database.execute("PRAGMA integrity_check").fetchone()
        english = database.execute(
            "SELECT display FROM english_words WHERE word = ? ORDER BY weight DESC LIMIT 1", ("hello",)
        ).fetchone()
    if (
        integrity != ("ok",)
        or candidate is None
        or quick_phrase != ("永远滴神",)
        or others_integrity != ("ok",)
        or emoji != ("😀",)
        or kaomoji != ("(*/ω＼*)",)
        or english_integrity != ("ok",)
        or english is None
        or english[0].lower() != "hello"
    ):
        OUTPUT.unlink(missing_ok=True)
        OTHERS_OUTPUT.unlink(missing_ok=True)
        ENGLISH_OUTPUT.unlink(missing_ok=True)
        raise SystemExit("Generated dictionary failed integrity or candidate verification.")
    print(
        f"Generated {OUTPUT} ({OUTPUT.stat().st_size} bytes), "
        f"ni'hao -> {candidate[0]}, yyds -> {quick_phrase[0]}"
    )
    print(
        f"Generated {OTHERS_OUTPUT} ({OTHERS_OUTPUT.stat().st_size} bytes), "
        f"xiaolian -> {emoji[0]}, haixiu -> {kaomoji[0]}"
    )
    print(
        f"Generated {ENGLISH_OUTPUT} ({ENGLISH_OUTPUT.stat().st_size} bytes), "
        f"hello -> {english[0]}"
    )


if __name__ == "__main__":
    main()
