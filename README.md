# Metasequoia IME for Linux

Linux frontend for Metasequoia IME. The first frontend targets IBus and reuses the shared C++ engine from [`MetasequoiaImeEngine`](https://github.com/houko/MetasequoiaImeEngine).

The engine submodule is pinned to its `feat/shared-input-session` branch while the platform-neutral `InputSession` API is being integrated upstream. Once that API lands on the default branch, update the submodule pin.

The initial implementation supports full-pinyin composition, live candidates from `msime.db`, candidate selection, Space to commit the leading candidate, Return to commit raw input, Backspace, Escape, and composition commit on focus changes.

## Dependencies

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libibus-1.0-dev libboost-dev libfmt-dev libspdlog-dev libsqlite3-dev python3
```

## Build and test

```sh
git clone --recursive https://github.com/houko/MetasequoiaImeLinux.git
cd MetasequoiaImeLinux
python3 scripts/build_dictionary.py
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To build the tested core without IBus headers, use `-DMETASEQUOIA_IME_BUILD_IBUS=OFF`.

## Install for the current user

```sh
./scripts/install.sh
```

The installer puts the engine in `~/.local/libexec`, the IBus component descriptor in `~/.local/share/ibus/component`, and the dictionary in `~/.local/share/metasequoiaime`. Restart IBus, then select “Metasequoia IME” in the desktop input-source settings.

## Scope

This repository owns the Linux/IBus adapter, Linux installation, packaging, and CI. The macOS frontend remains in [`MetasequoiaImeMac`](https://github.com/houko/MetasequoiaImeMac), while the input engine and dictionary remain shared dependencies.
