# Metasequoia IME for Linux

Linux frontend for Metasequoia IME. The first frontend targets IBus and reuses the shared C++ engine from [`MetasequoiaImeEngine`](https://github.com/houko/MetasequoiaImeEngine).

The engine submodule is currently pinned to the `houko` fork's `feat/linux-desktop-core` branch while the required native frontend API is being integrated upstream. Once the Engine pull request lands, the gitlink can move back to the upstream default branch.

The current desktop experience supports runtime switching among Quanpin, Shuangpin, Wubi and Japanese Romaji, a Chinese/direct-input toggle, mixed English/Emoji/kaomoji candidates, dedicated English candidates, live candidates from local SQLite dictionaries, keyboard and mouse candidate selection, paging, Chinese/English punctuation, half/full-width input, configurable inline preedit, Quanpin/Shuangpin helpcodes, and persistent per-user settings.

## Dependencies

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libibus-1.0-dev libboost-dev libfmt-dev libspdlog-dev libsqlite3-dev python3 python3-gi python3-pypinyin gir1.2-ibus-1.0 ibus dbus-x11 iso-codes
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

Stop any running Metasequoia IME engine before installing so its latest learned frequencies can be replayed safely. The installer refuses to continue while the current user's engine is running.

```sh
./scripts/install.sh
```

The installer puts the engine in `~/.local/libexec`, and installs the IBus component descriptor, three dictionaries, and five helpcode data files under `${XDG_DATA_HOME:-$HOME/.local/share}`. Restart IBus, then select “Metasequoia IME” in the desktop input-source settings.

To uninstall the current-user installation, remove the engine plus the component and dictionaries under `${XDG_DATA_HOME:-$HOME/.local/share}`, then restart IBus:

```sh
rm ~/.local/libexec/metasequoia-ime-ibus
data_home=${XDG_DATA_HOME:-$HOME/.local/share}
rm "$data_home/ibus/component/metasequoiaime.xml"
rm "$data_home/metasequoiaime/msime.db"
rm "$data_home/metasequoiaime/others.db"
rm "$data_home/metasequoiaime/english.db"
rm -r "$data_home/metasequoiaime/helpcodes"
```

## Controls and settings

- Tap either Shift key by itself to switch between Chinese conversion and direct input. Shift used with another key is left alone.
- Press `Ctrl+.` to switch between Chinese and English punctuation. Press `Ctrl+Shift+Space` to switch between half-width and full-width input. Both states are also available from the IBus language-bar menu.
- Use the IBus language-bar menu to select Quanpin, Shuangpin, Wubi or Japanese Romaji.
- Use Up/Down to move the candidate cursor. PageUp/PageDown, `-`/`=`, and Shift+Tab/Tab change pages. Comma/period paging is available as an opt-in setting and is disabled by default.
- Use `1`–`9` or keypad `1`–`9` to select from the visible page. Space commits the highlighted candidate, Return commits the raw input, and Escape cancels. Punctuation commits together with the highlighted candidate when a composition is active; apostrophe remains a pinyin separator.
- In Quanpin or Shuangpin, press `Shift+U` with no active composition to enter Unicode mode. Type a hexadecimal scalar such as `4e00` or `+1f600`; Space commits the highlighted character, and `Shift+1`–`Shift+9` select another visible candidate. Set `unicode-mode=false` to disable this mode.
- In Quanpin or Shuangpin, press `Shift+T` with no active composition for local date/time output. Use `rq`, `riqi`, or `date` for the current date; `sj`, `shijian`, or `time` for the current time; and `xq`, `xingqi`, or `week` for the current weekday.
- In Quanpin or Shuangpin, press `Shift+K` with no active composition and enter a lowercase letter code to query quick phrases from the local dictionary. For example, `yyds` selects its shipped phrase; user upserts and deletions survive staged dictionary upgrades through the XDG journal.
- In Quanpin or Shuangpin, press `Shift+E` for Emoji or `Shift+M` for kaomoji with no active composition. Search by full pinyin, abbreviated pinyin, supported Shuangpin spelling, or an English keyword; Space commits the highlighted result.
- Press `Ctrl+Shift+E` to enter or leave dedicated English mode. Type letters to query English-only prefix candidates; Space selects the highlighted candidate, while Return commits and learns raw alphabetic input without leaving the mode.
- Set `mixed-english-candidates=true`, `mixed-emoji-candidates=true`, or `mixed-kaomoji-candidates=true` to merge those local sources into normal Quanpin and Shuangpin candidates. The Windows-compatible priority is the leading Chinese candidate, then the first English, Emoji, and kaomoji matches, followed by the remaining local and source-group candidates with stable deduplication. Emoji and kaomoji start at two typed characters; `mixed-english-minimum-prefix` controls the English threshold and accepts 1–8. All three mixed sources are disabled by default, and the default English threshold is 2.
- Set `word-to-character=true` to make `[` commit the first Han character and `]` the last Han character of the highlighted candidate. If `bracket-paging=true`, bracket paging takes precedence and word-to-character selection is disabled for those keys.
- With `smart-punctuation=true`, comma, period and colon remain ASCII after an ASCII letter or digit. Repeating the same mark within two seconds replaces it with its Chinese form when `smart-punctuation-repeat-to-chinese=true`; unavailable surrounding text safely falls back to Chinese punctuation.
- With `paired-punctuation=true`, opening quotes, brackets, braces, book-title marks and parentheses insert both halves and leave the cursor between them.
- Set `preedit-style=raw`, `pinyin`, or `hidden` to show the typed keys, segmented pinyin, or no inline preedit. Hidden inline preedit does not hide the candidate lookup table.
- Quanpin and Shuangpin helpcodes are independently controlled by `quanpin-helpcode` and `shuangpin-helpcode`. Their schema keys accept `lantian`, `ziranma`, `shouyou2_0`, `shouyouplus`, or `xiaohe`; helpcodes activate only after a complete base spelling.
- Local candidate learning uses `frequency-adjustment=disabled|pin|halve|linear|promote`. `pin` moves a selected non-leading candidate to the top, `halve` halves its rank, `linear` advances by `frequency-linear-step`, and `promote` advances one slot or to slot five when it is farther back. `frequency-trigger-count` controls how many selections trigger an adjustment; both numeric settings accept 1–10.

Settings are stored in `$XDG_CONFIG_HOME/metasequoiaime/config.ini`, falling back to `~/.config/metasequoiaime/config.ini`. The `[input]` group stores `mode`, `scheme`, `page-size`, `punctuation`, `full-width`, `comma-period-paging`, `word-to-character`, `bracket-paging`, `smart-punctuation`, `smart-punctuation-repeat-to-chinese`, `paired-punctuation`, `preedit-style`, `quanpin-helpcode`, `quanpin-helpcode-schema`, `shuangpin-helpcode`, `shuangpin-helpcode-schema`, `frequency-adjustment`, `frequency-trigger-count`, `frequency-linear-step`, `unicode-mode`, `mixed-english-candidates`, `mixed-english-minimum-prefix`, `mixed-emoji-candidates`, and `mixed-kaomoji-candidates`. Defaults are Chinese input, Quanpin, page size 9, Chinese punctuation, half width, raw preedit, all three paging/selection switches and all mixed candidate sources disabled, all smart/paired punctuation, helpcode, and Unicode-mode switches enabled with the `lantian` helpcode schema, a mixed-English prefix threshold of 2, and `promote` learning after every non-leading selection. Edit or remove the file while the engine is not active; it will be written atomically after the next property or hotkey change. Learned weights and English raw entries are journaled in `${XDG_DATA_HOME:-$HOME/.local/share}/metasequoiaime/msime_user.db`; rerunning `scripts/install.sh` replays that journal into staged `msime.db`, `others.db`, and `english.db` files before replacing the live dictionary set as one unit.

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
| Raw/segmented/hidden preedit and helpcodes | Supported | Engine, controller, settings, installed-data and real D-Bus lookup smoke |
| Local candidate frequency learning | Supported | Five-mode Engine persistence tests plus controller, settings and real D-Bus/XDG journal smoke |
| Explicit Unicode scalar input | Supported | Engine scalar validation, controller/mapper/settings tests and real D-Bus commit smoke |
| Local date, time and weekday candidates | Supported | Deterministic Engine formatter tests plus controller and real D-Bus commit smoke |
| XDG quick phrases and upgrade replay | Supported | Engine query/failure tests, controller and real D-Bus commit smoke, transactional install smoke |
| Explicit Emoji and kaomoji modes | Supported | Engine prefix/ordering/failure tests, generated `others.db`, transactional install and real D-Bus commit smoke |
| Mixed English, Emoji and kaomoji candidates | Supported | Engine slot/deduplication/failure-isolation tests plus controller/settings and real D-Bus ordering smoke |
| Dedicated English input | Supported | Engine failure/learning tests, controller/mapper/settings tests, generated `english.db`, transactional install and real D-Bus commit smoke |
| Super-jianpin and temporary input modes | Planned | Local extensions phase |
| Cloud, AI, translation, voice and settings UI | Planned | Online and desktop-tools phases |

## Scope

This repository owns the Linux/IBus adapter, Linux installation, packaging, and CI. The macOS frontend remains in [`MetasequoiaImeMac`](https://github.com/houko/MetasequoiaImeMac), while the input engine and dictionary remain shared dependencies.
