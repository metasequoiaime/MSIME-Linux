# Packaging Metasequoia IME for a distribution

This document is for distribution packagers — Debian, Fedora, Arch, openSUSE, Nix, Flathub. It is in English because that is the language of the packaging ecosystems; the rest of this repository's documentation is in Chinese.

Metasequoia IME is not in any distribution yet. If you are reading this because you want to change that, open an issue on this repository and say which distribution — questions get answered faster than they get guessed at.

## What this package is

An IBus input method engine for Chinese, plus desktop tools. The build produces:

| Binary | Installed to | Purpose |
| --- | --- | --- |
| `metasequoia-ime-ibus` | `libexecdir` | The IBus engine process, started by `ibus-daemon` |
| `metasequoia-ime-settings` | `bindir` | GTK settings application |
| `metasequoia-ime-voice` | `bindir` | Standalone speech-to-text, never invoked by the engine |
| `metasequoiaime.xml` | `datadir/ibus/component` | IBus component registration |

`ibus` is a **hard runtime requirement**. The component XML is useless without `ibus-daemon`, and the input source never appears in the desktop's list. `libibus-1.0.so.5` alone is not sufficient — on Fedora that is satisfied by `ibus-libs`, which does not include the daemon.

## Build dependencies

Debian / Ubuntu names:

```
build-essential cmake pkg-config
libibus-1.0-dev libboost-dev libboost-json-dev libcurl4-openssl-dev
nlohmann-json3-dev libfmt-dev libspdlog-dev libsqlite3-dev
libsecret-1-dev libgtk-3-dev python3
```

`libboost-json-dev` is required in addition to `libboost-dev`. Boost.JSON is a compiled component; the header metapackage does not provide the config file and library that `find_package(Boost REQUIRED COMPONENTS json)` needs, and CMake fails at configure time without it.

Optional at runtime, for local handwriting recognition: `tesseract-ocr` and the `chi_sim` language pack. Their absence is handled — the desktop tools report the missing backend in the status bar rather than failing.

API tokens for the optional online features are stored in the desktop Secret Service, so a keyring implementation is a runtime dependency of those features only.

## Getting the source

Use the release tarball, not the GitHub auto-generated one. **GitHub's auto-generated archive omits submodules**, so `vendor/MetasequoiaImeEngine` arrives empty and the build cannot work.

Each release attaches:

```
metasequoia-ime-linux-<version>.tar.gz
metasequoia-ime-linux-<version>.tar.gz.sha256
```

The tarball is self-contained: the shared engine and every other submodule are unpacked in place. Verify with `sha256sum -c`.

Note the release channels. Automatic per-merge builds are marked **Pre-release**; deliberately curated ones are ordinary releases. Package the latter.

## Dictionary data, and how to build offline

The dictionaries are not in this repository. They are release assets of [MSIME-Engine](https://github.com/metasequoiaime/MSIME-Engine), pinned by tag and by the SHA256 of every file in `product-lock.json`, so a retagged release or a replaced database fails the build instead of shipping.

`scripts/build.sh` fetches them over HTTPS, which a distribution build sandbox will not allow. Supply them from disk instead:

```sh
python3 scripts/fetch_dictionary.py --from-directory /path/to/assets
# or
MSIME_DICT_DIR=/path/to/assets python3 scripts/fetch_dictionary.py
```

The directory must contain every file named in `product-lock.json` (currently `msime.db`, `others.db`, `english.db`, `dict_japanese.dat`, `mozc_dictionary_oss_README.txt`, `dictionary-manifest.json`, `SHA256SUMS.txt`). **A supplied directory is verified exactly as a download is** — against the digests committed in `product-lock.json` — so this is not an escape hatch around the integrity check.

Download them once, outside the sandbox, from the engine release named by `product-lock.json`:

```
https://github.com/metasequoiaime/MSIME-Engine/releases/download/<dictionary tag>/<asset>
```

Roughly 180 MB total. Treating them as a separate source in your recipe, alongside the source tarball, is the intended shape.

### Licensing of the dictionary data

Read [`dictionary/NOTICE.md`](https://github.com/metasequoiaime/MSIME-Engine/blob/main/dictionary/NOTICE.md) in the engine repository before packaging. It records the upstream and licence of every input file, including several that had no redistribution grant. The engine's build now excludes those by default, but **check which dictionary release your pinned tag corresponds to**: assets published before that change may contain them. If you need certainty for a given tag, ask on the engine repository.

The code is GPL-3.0-only. The dictionary data has mixed provenance and is documented file by file in that notice.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr --strip
```

Standard GNU install directories are honoured through `GNUInstallDirs`, so `CMAKE_INSTALL_LIBEXECDIR` and friends work as expected.

Some tests need a session bus and a running IBus; they are named `ibus_smoke` and `*Smoke`. In a sandbox without those, exclude them:

```sh
ctest --test-dir build --output-on-failure -E '^(ibus_smoke|.*Smoke)$'
```

The remainder are in-process unit tests with no external requirements.

## Packages this repository builds itself

CPack produces `.deb`, `.rpm` and `.tar.gz` for amd64 and arm64 on every release. Those exist so users have something to install today; they are not a substitute for distribution packages and they do not integrate with a distribution's update path. If you package this properly, say so in an issue and the README will point at your package instead.

## Versioning

`version.txt` and the `project()` version in `CMakeLists.txt` are the version, kept in step by release-please from conventional commits. Tags are `vX.Y.Z`.

The project is pre-1.0 and says so: interfaces and configuration keys can still change between minor versions. A note in [`docs/product-release.md`](product-release.md) records why CalVer was tried and reverted on 2026-09-05 — `dpkg` and `rpm` both order `2026.9.1` above `0.7.0`, so users who had installed the CalVer build would never have been offered the upgrade back.

## Reporting problems

Packaging problems are ordinary issues on this repository. Suspected vulnerabilities go through the organization [SECURITY.md](https://github.com/metasequoiaime/.github/blob/main/SECURITY.md) instead, never a public issue.
