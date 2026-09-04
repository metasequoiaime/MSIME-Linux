# Metasequoia IME for Linux

Linux frontend for Metasequoia IME. The first frontend targets IBus and reuses the shared C++ engine from [`MetasequoiaImeEngine`](https://github.com/houko/MetasequoiaImeEngine).

The engine submodule is currently pinned to the `houko` fork's `feat/linux-desktop-core` branch while the required native frontend API is being integrated upstream. Once the Engine pull request lands, the gitlink can move back to the upstream default branch.

The desktop-core slice supports runtime switching among Quanpin, Shuangpin, Wubi and Japanese Romaji, a Chinese/direct-input toggle, live candidates from `msime.db`, keyboard and mouse candidate selection, paging, and persistent per-user settings.

## Dependencies

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libibus-1.0-dev libboost-dev libfmt-dev libspdlog-dev libsqlite3-dev python3 python3-gi gir1.2-ibus-1.0 ibus dbus-x11 iso-codes
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

To uninstall the current-user installation, remove the engine plus the component and dictionary under `${XDG_DATA_HOME:-$HOME/.local/share}`, then restart IBus:

```sh
rm ~/.local/libexec/metasequoia-ime-ibus
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
rm "$data_home/ibus/component/metasequoiaime.xml"
rm "$data_home/metasequoiaime/msime.db"
```

## Controls and settings

- Tap either Shift key by itself to switch between Chinese conversion and direct input. Shift used with another key is left alone.
- Use the IBus language-bar menu to select Quanpin, Shuangpin, Wubi or Japanese Romaji.
- Use Up/Down to move the candidate cursor. PageUp/PageDown, `-`/`=`, `,`/`.`, and Shift+Tab/Tab change pages.
- Use `1`–`9` or keypad `1`–`9` to select from the visible page. Space commits the highlighted candidate, Return commits the raw input, and Escape cancels.

Settings are stored in `$XDG_CONFIG_HOME/metasequoiaime/config.ini`, falling back to `~/.config/metasequoiaime/config.ini`. To reset mode, scheme and page size, remove that file while the engine is not active; it will be recreated with defaults after the next property change.

## Desktop-core parity

| Capability | Status | Evidence |
| --- | --- | --- |
| Quanpin, Shuangpin, Wubi and Japanese switching | Supported | Controller tests and IBus property smoke |
| Chinese/direct mode and bare-Shift toggle | Supported | Controller and real-keyval mapper tests |
| Candidate cursor, paging, digits and mouse selection | Supported | Controller and IBus adapter tests |
| XDG configuration with atomic replacement | Supported | Filesystem and IBus lifecycle tests |
| IBus registration and current-user install | Supported | D-Bus and install CI smoke gates |
| Punctuation, full-width mode and user-frequency UX | Planned | Desktop experience phase |
| Emoji, kaomoji, phrases and extended input modes | Planned | Local extensions phase |
| Cloud, AI, translation, voice and settings UI | Planned | Online and desktop-tools phases |

## Scope

This repository owns the Linux/IBus adapter, Linux installation, packaging, and CI. The macOS frontend remains in [`MetasequoiaImeMac`](https://github.com/houko/MetasequoiaImeMac), while the input engine and dictionary remain shared dependencies.
