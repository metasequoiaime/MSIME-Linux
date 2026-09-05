#!/usr/bin/env python3
"""Fetch the dictionaries the product lock names and verify them against its committed digests.

These used to be built here from the vendored MSIME-Dict sources. That reproduced part of that
repository's build in this one, and it meant the data lagged behind the submodule pin: the
placeholder replacement for the personal data in quick_phrases.txt sat unmerged upstream for two
days while this repository kept shipping the old values, and the pin had to be moved by hand.

MSIME-Dict now publishes msime.db, others.db, english.db and SHA256SUMS.txt as release assets, so
take them from there. Windows already does (MSIME-Server and MSIME-Windows), which means all three
platforms ship byte-identical dictionaries.

The release tag no longer lives here and cannot be overridden from the command line. product-lock.json names it and records the SHA256 of every asset, so a retagged release or a replaced database fails the build instead of shipping: rewriting the upstream SHA256SUMS.txt along with the data does not help, because that file is verified against a committed digest too. Move to a new release with `python3 scripts/product_lock.py refresh --dictionary-tag dict-YYYY.MM.DD` and review the resulting diff.

    python3 scripts/fetch_dictionary.py
"""

from __future__ import annotations

import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

import product_lock

ROOT = Path(__file__).resolve().parents[1]

# Unchanged from when these were built here, so CMakeLists.txt, scripts/install.sh and
# tests/IBusSmoke.sh keep working without edits.
OUTPUT_DIR = ROOT / "vendor" / "MetasequoiaImeDict" / "out"


def verify_contents(destination: Path) -> None:
    """The digests prove we got what was reviewed; these probes prove it is usable."""
    main_db = destination / "msime.db"
    others_db = destination / "others.db"
    english_db = destination / "english.db"

    with sqlite3.connect(main_db) as database:
        integrity = database.execute("PRAGMA integrity_check").fetchone()
        candidate = database.execute(
            "SELECT value FROM tbl_2_n WHERE key = ? ORDER BY weight DESC LIMIT 1", ("ni'hao",)
        ).fetchone()
        quick_phrase = database.execute(
            "SELECT value FROM quick_parases WHERE key = ? ORDER BY weight DESC,value LIMIT 1", ("yyds",)
        ).fetchone()
        wubi_candidate = database.execute(
            "SELECT value FROM wubi86 WHERE key = ? ORDER BY weight DESC LIMIT 1", ("aaaa",)
        ).fetchone()
    with sqlite3.connect(others_db) as database:
        others_integrity = database.execute("PRAGMA integrity_check").fetchone()
        emoji = database.execute(
            "SELECT emoji FROM emoji_pinyin WHERE key = ? ORDER BY sort_order LIMIT 1", ("xiaolian",)
        ).fetchone()
        kaomoji = database.execute(
            "SELECT kaomoji FROM kaomoji WHERE pinyin = ? ORDER BY sort_order LIMIT 1", ("haixiu",)
        ).fetchone()
    with sqlite3.connect(english_db) as database:
        english_integrity = database.execute("PRAGMA integrity_check").fetchone()
        english = database.execute(
            "SELECT display FROM english_words WHERE word = ? ORDER BY weight DESC LIMIT 1", ("hello",)
        ).fetchone()

    if (
        integrity != ("ok",)
        or candidate is None
        or quick_phrase != ("永远滴神",)
        or wubi_candidate is None
        or others_integrity != ("ok",)
        or emoji != ("😀",)
        or kaomoji != ("(*/ω＼*)",)
        or english_integrity != ("ok",)
        or english is None
        or english[0].lower() != "hello"
    ):
        raise SystemExit("Downloaded dictionary failed integrity or candidate verification.")

    print(
        f"{main_db.name} ({main_db.stat().st_size} bytes), "
        f"ni'hao -> {candidate[0]}, yyds -> {quick_phrase[0]}, aaaa -> {wubi_candidate[0]}"
    )
    print(
        f"{others_db.name} ({others_db.stat().st_size} bytes), "
        f"xiaolian -> {emoji[0]}, haixiu -> {kaomoji[0]}"
    )
    print(f"{english_db.name} ({english_db.stat().st_size} bytes), hello -> {english[0]}")


def main() -> None:
    data = product_lock.load()
    tag = data["dictionary"]["tag"]
    print(f"Fetching {tag} from {data['dictionary']['repository']}")

    # Everything is verified in a staging directory first. A failed or tampered download then leaves a previous usable checkout untouched instead of half replacing it. The staging directory sits inside the output directory because that path is gitignored and on the same filesystem, so it neither dirties the submodule checkout around it nor copies across devices.
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=OUTPUT_DIR) as temporary:
        incoming = Path(temporary)
        product_lock.download_assets(tag, incoming)
        product_lock.verify_assets(incoming, data)
        print(f"verified {len(data["dictionary"]["assets"])} assets against product-lock.json")
        verify_contents(incoming)
        for name in data["dictionary"]["assets"]:
            shutil.copyfile(incoming / name, OUTPUT_DIR / name)
    print(f"staged into {OUTPUT_DIR}")


if __name__ == "__main__":
    sys.exit(main())
