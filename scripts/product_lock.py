#!/usr/bin/env python3
"""Maintain and consume the reviewed Linux product dependency lock.

The Engine gitlink provides both the engine and helpcode data. Git already makes those immutable and shows every move in a pull request diff, so they are not copied into this lock: doing that would only give the submodule pin a second home to drift from, and it would break the dependabot bump that engine-bump-triage.yml merges on green CI.

The dictionary the packages actually *ship* is the one input git does not pin. It is a release asset behind a tag that upstream can retag, and it used to be verified against the SHA256SUMS.txt published beside it, which is exactly as mutable as the data. So product-lock.json holds the tag and the SHA256 of every asset, and the build verifies those committed digests instead.

The dictionary's *source* commit is locked alongside them rather than read from a gitlink. It used to be a gitlink, and that pin recorded the wrong answer: nothing moved it in lockstep with the release tag, so the manifest attested to whatever revision of MSIME-Dict happened to be vendored while the packages shipped bytes built from a different one. The commit the release tag resolves to is the only one that produced the data, so `refresh` resolves it at the moment the data is reviewed and commits it here.

`refresh` is the only command that reaches upstream. `manifest` records what a build consumed: the source commit, every gitlink, the locked dictionary and the digest of the lock itself.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import re
import subprocess
import tempfile
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import product_lock_shared as shared

ROOT = Path(__file__).resolve().parents[1]
_product_spec = importlib.util.spec_from_file_location("dictionary_product", Path(__file__).with_name("dictionary_product.py"))
_product = importlib.util.module_from_spec(_product_spec)
_product_spec.loader.exec_module(_product)
PRODUCT_MANIFEST = _product.MANIFEST_NAME
LEGACY_DICTIONARY_TAG = "dict-2026.09.05"


REPOSITORY = "metasequoiaime/MSIME-Linux"
DICTIONARY_REPOSITORY = "metasequoiaime/MSIME-Engine"
DICTIONARY_URL = f"https://github.com/{DICTIONARY_REPOSITORY}.git"

# Every gitlink this repository builds from. The manifest reads their commits here rather than from
# a second copy in the lock, so there is one source of truth for what was checked out.
SUBMODULES = {
    "engine": ("metasequoiaime/MSIME-Engine", "vendor/MetasequoiaImeEngine"),
}

# The databases CMakeLists.txt installs into the packages, plus the checksum file the release
# publishes beside them. The checksum file is locked too so a rewritten one is caught rather than
# trusted.
DATABASES = ("msime.db", "others.db", "english.db")
# The Japanese sentence model the Engine loads from the data directory beside msime.db, and the Mozc notice its licences require to be distributed with the model. Without the model the Japanese scheme this frontend advertises silently degrades to single-word candidates, because JapaneseSentenceDecoder::Load fails and the provider stores a null decoder instead of reporting anything.
JAPANESE_MODEL = ("dict_japanese.dat", "mozc_dictionary_oss_README.txt")
# Releases old enough to predate the dictionary product manifest publish neither the manifest nor the
# Japanese model, so the archived tag is held to the smaller set.
LEGACY_ASSETS = (*DATABASES, "SHA256SUMS.txt")
ASSETS = (*LEGACY_ASSETS, *JAPANESE_MODEL)

SHA = re.compile(r"[0-9a-f]{40}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
TAG = re.compile(r"dict-[A-Za-z0-9._-]+\Z")


def validate(data: dict) -> dict:
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported product lock schema_version")
    dictionary = data.get("dictionary", {})
    if dictionary.get("repository") != DICTIONARY_REPOSITORY and not (
        dictionary.get("repository") == "metasequoiaime/MSIME-Dict" and dictionary.get("tag") == LEGACY_DICTIONARY_TAG
    ):
        raise ValueError("Unexpected dictionary repository")
    if not TAG.fullmatch(dictionary.get("tag", "")):
        raise ValueError("Dictionary tag must be an explicit dict-* release, never latest")
    if not SHA.fullmatch(dictionary.get("source_commit", "")):
        raise ValueError("Dictionary source_commit must be the full commit the release tag resolves to")
    assets = dictionary.get("assets", {})
    expected_assets = set(LEGACY_ASSETS) if dictionary['tag'] == LEGACY_DICTIONARY_TAG else set(ASSETS) | {PRODUCT_MANIFEST}
    if set(assets) != expected_assets:
        raise ValueError("Dictionary lock must cover every shipped payload and the checksum file")
    for name, digest in assets.items():
        if not DIGEST.fullmatch(digest):
            raise ValueError(f"Invalid SHA256 for {name}")
    return data


def load(path: Path = ROOT / "product-lock.json") -> dict:
    return validate(json.loads(path.read_text(encoding="utf-8")))


def write_json(path: Path, data: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def sha256(path: Path) -> str:
    return shared.sha256(path)


def verify_assets(directory: Path, data: dict) -> None:
    """Fail on the committed digest, never on the checksum file that shipped with the data."""
    shared.verify_digests(directory, data["dictionary"]["assets"])

    if PRODUCT_MANIFEST in data["dictionary"]["assets"]:
        required_files = set(data["dictionary"]["assets"]) - {"SHA256SUMS.txt", PRODUCT_MANIFEST}
        shared.verify_manifest_provenance(directory, PRODUCT_MANIFEST, _product.verify_product, required_files,
                                          data["dictionary"]["repository"], data["dictionary"]["source_commit"])


def download_assets(tag: str, destination: Path, repository: str = DICTIONARY_REPOSITORY) -> None:
    """Plain HTTPS rather than the GitHub CLI or the API.

    The release assets of a public repository are served unauthenticated from a CDN, so this needs no credentials and no gh, which the bare Ubuntu containers in CI do not have. It also stays off the API, which this repository has already had rate limited to 403 from a single runner address (see scripts/bootstrap_ci_dependencies.sh).
    """
    if not TAG.fullmatch(tag):
        raise ValueError("Refusing to download from a tag that is not an explicit dict-* release")
    if repository not in (DICTIONARY_REPOSITORY, "metasequoiaime/MSIME-Dict"):
        raise ValueError("Unexpected dictionary repository")
    destination.mkdir(parents=True, exist_ok=True)
    names = LEGACY_ASSETS if tag == LEGACY_DICTIONARY_TAG else (*ASSETS, PRODUCT_MANIFEST)
    for name in names:
        url = f"https://github.com/{repository}/releases/download/{tag}/{name}"
        target = destination / name
        shared.download_with_retries(url, target)
        print(f"downloaded {name} ({target.stat().st_size} bytes)")


def resolve_tag_commit(tag: str) -> str:
    """Resolve a dict-* tag to the commit that built it, over the plain git protocol.

    Not the API: `refresh` is run from the same places `download_assets` is, and this repository has
    already had the API rate limited to 403 from a single runner address. ls-remote needs no
    credentials and no gh, and git is present anywhere the submodules can be checked out.
    """
    return shared.resolve_tag_commit(DICTIONARY_URL, tag)


def published_checksums(directory: Path) -> dict[str, str]:
    return shared.published_checksums(directory / "SHA256SUMS.txt")


def git(directory: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(directory), *args], text=True).strip()


def gitlinks(directory: Path) -> dict:
    submodules = {}
    for name, (repository, path) in SUBMODULES.items():
        fields = git(directory, "ls-tree", "HEAD", path).split()
        if len(fields) != 4 or fields[0] != "160000" or not SHA.fullmatch(fields[2]):
            raise ValueError(f"{path} is not a gitlink at an immutable commit in HEAD")
        submodules[name] = {"repository": repository, "path": path, "commit": fields[2]}
    return submodules


def manifest(directory: Path, commit: str, lock: Path, data: dict) -> dict:
    if not SHA.fullmatch(commit):
        raise ValueError("The source commit must be a full immutable SHA")
    return {
        "schema_version": 1,
        "source": {"repository": REPOSITORY, "commit": commit},
        "submodules": gitlinks(directory),
        "dictionary": data["dictionary"],
        "lock_sha256": sha256(lock),
    }


def refresh(tag: str) -> dict:
    """Resolve the digests of a dictionary release so a human can review the diff before it ships.

    Trusting the published SHA256SUMS.txt is appropriate here and only here: this is the one moment the data is looked at deliberately, and the digests it produces are what every later build is held to.
    """
    if not TAG.fullmatch(tag):
        raise ValueError("refresh requires an explicit dict-* release tag")
    with tempfile.TemporaryDirectory() as temporary:
        incoming = Path(temporary)
        download_assets(tag, incoming)
        published = published_checksums(incoming)
        assets = {}
        legacy = tag == LEGACY_DICTIONARY_TAG
        for name in (LEGACY_ASSETS if legacy else (*ASSETS, PRODUCT_MANIFEST)):
            digest = sha256(incoming / name)
            if name in published and published[name] != digest:
                raise ValueError(f"{name} does not match the checksums published with {tag}")
            assets[name] = digest
        for name in (DATABASES if legacy else (*DATABASES, *JAPANESE_MODEL)):
            if name not in published:
                raise ValueError(f"{name} has no entry in the SHA256SUMS.txt published with {tag}")
    source_commit = resolve_tag_commit(tag)
    print(f"locked {len(assets)} assets from {DICTIONARY_REPOSITORY} at {tag} ({source_commit})")
    return validate({
        "schema_version": 1,
        "dictionary": {
            "repository": DICTIONARY_REPOSITORY,
            "tag": tag,
            "source_commit": source_commit,
            "assets": assets,
        },
    })


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--lock", type=Path, default=ROOT / "product-lock.json")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate")
    commands.add_parser("tag")
    verify = commands.add_parser("verify-dictionaries")
    verify.add_argument("directory", type=Path)
    record = commands.add_parser("manifest")
    record.add_argument("--source-commit", required=True)
    record.add_argument("--repository", type=Path, default=ROOT)
    record.add_argument("--output", type=Path, required=True)
    update = commands.add_parser("refresh")
    update.add_argument("--dictionary-tag", required=True)
    args = parser.parse_args()

    if args.command == "refresh":
        write_json(args.lock, refresh(args.dictionary_tag))
        return

    data = load(args.lock)
    if args.command == "tag":
        print(data["dictionary"]["tag"])
    elif args.command == "verify-dictionaries":
        verify_assets(args.directory, data)
    elif args.command == "manifest":
        write_json(args.output, manifest(args.repository, args.source_commit, args.lock, data))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
