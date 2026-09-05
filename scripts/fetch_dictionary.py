#!/usr/bin/env python3
"""Fetch the shipping dictionaries from an MSIME-Dict release and verify them.

These used to be built here from the vendored MSIME-Dict sources. That reproduced part of that
repository's build in this one, and it meant the data lagged behind the submodule pin: the
placeholder replacement for the personal data in quick_phrases.txt sat unmerged upstream for two
days while this repository kept shipping the old values, and the pin had to be moved by hand.

MSIME-Dict now publishes msime.db, others.db, english.db and SHA256SUMS.txt as release assets, so
take them from there. Windows already does (MSIME-Server and MSIME-Windows), which means all three
platforms ship byte-identical dictionaries, and bumping DICTIONARY_RELEASE is a one-line reviewable
change rather than an invisible submodule pin.

    python3 scripts/fetch_dictionary.py
    python3 scripts/fetch_dictionary.py --tag dict-2026.09.05
"""

from __future__ import annotations

import argparse
import hashlib
import sqlite3
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Pinned so a rebuild of the same commit ships the same dictionaries. Bump deliberately, the same
# way a submodule pin used to be bumped, except the artifact is prebuilt and checksummed.
DICTIONARY_RELEASE = "dict-2026.09.05"
DICTIONARY_REPOSITORY = "metasequoiaime/MSIME-Dict"

# Unchanged from when these were built here, so CMakeLists.txt, scripts/install.sh and
# tests/IBusSmoke.sh keep working without edits.
OUTPUT_DIR = ROOT / "vendor" / "MetasequoiaImeDict" / "out"
DATABASES = ("msime.db", "others.db", "english.db")


def download(tag: str, destination: Path) -> None:
    """Plain HTTPS rather than the GitHub CLI or the API.

    The release assets of a public repository are served unauthenticated from a CDN, so this needs
    no credentials and no gh, which the bare Ubuntu containers in CI do not have. It also stays off
    the API, which this repository has already had rate limited to 403 from a single runner address
    (see scripts/bootstrap_ci_dependencies.sh).
    """
    destination.mkdir(parents=True, exist_ok=True)
    for name in (*DATABASES, "SHA256SUMS.txt"):
        url = f"https://github.com/{DICTIONARY_REPOSITORY}/releases/download/{tag}/{name}"
        target = destination / name
        last_error: Exception | None = None
        for attempt in range(1, 4):
            try:
                with urllib.request.urlopen(url, timeout=120) as response, target.open("wb") as out:
                    while chunk := response.read(1 << 20):
                        out.write(chunk)
                break
            except (urllib.error.URLError, TimeoutError, OSError) as error:
                last_error = error
                print(f"  attempt {attempt} for {name} failed: {error}")
        else:
            raise SystemExit(f"Could not download {url}: {last_error}")
        print(f"downloaded {name}")


def verify_checksums(destination: Path) -> None:
    """Fail here rather than letting a truncated download surface as an empty candidate list."""
    sums = destination / "SHA256SUMS.txt"
    if not sums.is_file():
        raise SystemExit(f"{sums} is missing; cannot verify the downloaded dictionaries.")

    published = {}
    for line in sums.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, _, name = line.partition("  ")
        published[name.strip()] = digest.strip()

    for name in DATABASES:
        path = destination / name
        if not path.is_file():
            raise SystemExit(f"{path} was not downloaded.")
        if name not in published:
            raise SystemExit(f"{name} has no entry in SHA256SUMS.txt.")
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual != published[name]:
            path.unlink(missing_ok=True)
            raise SystemExit(f"{name} checksum mismatch: expected {published[name]}, got {actual}")
        print(f"verified {name} ({path.stat().st_size} bytes)")


def verify_contents(destination: Path) -> None:
    """The checksums prove we got what was published; these probes prove it is usable."""
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
        for name in DATABASES:
            (destination / name).unlink(missing_ok=True)
        raise SystemExit("Downloaded dictionary failed integrity or candidate verification.")

    print(
        f"{main_db} ({main_db.stat().st_size} bytes), "
        f"ni'hao -> {candidate[0]}, yyds -> {quick_phrase[0]}, aaaa -> {wubi_candidate[0]}"
    )
    print(
        f"{others_db} ({others_db.stat().st_size} bytes), "
        f"xiaolian -> {emoji[0]}, haixiu -> {kaomoji[0]}"
    )
    print(f"{english_db} ({english_db.stat().st_size} bytes), hello -> {english[0]}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tag", default=DICTIONARY_RELEASE, help=f"MSIME-Dict release tag (default: {DICTIONARY_RELEASE})")
    args = parser.parse_args()

    print(f"Fetching {args.tag} from {DICTIONARY_REPOSITORY}")
    download(args.tag, OUTPUT_DIR)
    verify_checksums(OUTPUT_DIR)
    verify_contents(OUTPUT_DIR)


if __name__ == "__main__":
    sys.exit(main())
