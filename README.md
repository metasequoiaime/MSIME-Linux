# Metasequoia IME for Linux

Linux frontend for Metasequoia IME. The first frontend targets IBus and reuses the shared C++ engine from [`MetasequoiaImeEngine`](https://github.com/houko/MetasequoiaImeEngine).

The engine submodule is currently pinned to the `houko` fork's `feat/linux-desktop-core` branch while the required native frontend API is being integrated upstream. Once the Engine pull request lands, the gitlink can move back to the upstream default branch.

The current desktop experience supports runtime switching among Quanpin, Shuangpin, Wubi and Japanese Romaji, a Chinese/direct-input toggle, live candidates from `msime.db`, keyboard and mouse candidate selection, paging, Chinese/English punctuation, half/full-width input, and persistent per-user settings.

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

The installer puts the engine in `~/.local/libexec`, and installs the IBus component descriptor and dictionary under `${XDG_DATA_HOME:-$HOME/.local/share}`. Restart IBus, then select “Metasequoia IME” in the desktop input-source settings.

To uninstall the current-user installation, remove the engine plus the component and dictionary under `${XDG_DATA_HOME:-$HOME/.local/share}`, then restart IBus:

```sh
rm ~/.local/libexec/metasequoia-ime-ibus
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
rm "$data_home/ibus/component/metasequoiaime.xml"
rm "$data_home/metasequoiaime/msime.db"
```

## Controls and settings

- Tap either Shift key by itself to switch between Chinese conversion and direct input. Shift used with another key is left alone.
- Press `Ctrl+.` to switch between Chinese and English punctuation. Press `Ctrl+Shift+Space` to switch between half-width and full-width input. Both states are also available from the IBus language-bar menu.
- Use the IBus language-bar menu to select Quanpin, Shuangpin, Wubi or Japanese Romaji.
- Use Up/Down to move the candidate cursor. PageUp/PageDown, `-`/`=`, and Shift+Tab/Tab change pages. Comma/period paging is available as an opt-in setting and is disabled by default.
- Use `1`–`9` or keypad `1`–`9` to select from the visible page. Space commits the highlighted candidate, Return commits the raw input, and Escape cancels. Punctuation commits together with the highlighted candidate when a composition is active; apostrophe remains a pinyin separator.
- Set `word-to-character=true` to make `[` commit the first Han character and `]` the last Han character of the highlighted candidate. If `bracket-paging=true`, bracket paging takes precedence and word-to-character selection is disabled for those keys.
- With `smart-punctuation=true`, comma, period and colon remain ASCII after an ASCII letter or digit. Repeating the same mark within two seconds replaces it with its Chinese form when `smart-punctuation-repeat-to-chinese=true`; unavailable surrounding text safely falls back to Chinese punctuation.
- With `paired-punctuation=true`, opening quotes, brackets, braces, book-title marks and parentheses insert both halves and leave the cursor between them.

Settings are stored in `$XDG_CONFIG_HOME/metasequoiaime/config.ini`, falling back to `~/.config/metasequoiaime/config.ini`. The `[input]` group stores `mode`, `scheme`, `page-size`, `punctuation`, `full-width`, `comma-period-paging`, `word-to-character`, `bracket-paging`, `smart-punctuation`, `smart-punctuation-repeat-to-chinese`, and `paired-punctuation`. Defaults are Chinese input, Quanpin, page size 9, Chinese punctuation, half width, all three paging/selection switches disabled, and all three smart/paired punctuation switches enabled. Edit or remove the file while the engine is not active; it will be written atomically after the next property or hotkey change.

## Desktop-core parity

| Capability | Status | Evidence |
| --- | --- | --- |
| Quanpin, Shuangpin, Wubi and Japanese switching | Supported | Controller tests and IBus property smoke |
| Chinese/direct mode and bare-Shift toggle | Supported | Controller and real-keyval mapper tests |
| Candidate cursor, paging, digits and mouse selection | Supported | Controller and IBus adapter tests |
| XDG configuration with atomic replacement | Supported | Filesystem and IBus lifecycle tests |
| IBus registration and current-user install | Supported | D-Bus and install CI smoke gates |
| Chinese/English punctuation and full-width mode | Supported | Transform, controller, mapper, settings and real D-Bus tests |
| Smart and paired punctuation | Supported | Surrounding-text, replacement deletion and cursor-forwarding D-Bus smoke |
| Highlighted-candidate first/last Han selection | Supported | Engine Unicode tests, controller tests and real D-Bus commit smoke |
| User-frequency UX | Planned | Desktop experience phase |
| Emoji, kaomoji, phrases and extended input modes | Planned | Local extensions phase |
| Cloud, AI, translation, voice and settings UI | Planned | Online and desktop-tools phases |

## Scope

This repository owns the Linux/IBus adapter, Linux installation, packaging, and CI. The macOS frontend remains in [`MetasequoiaImeMac`](https://github.com/houko/MetasequoiaImeMac), while the input engine and dictionary remain shared dependencies.
