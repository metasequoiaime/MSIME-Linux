# Metasequoia IME for Linux

Linux frontend for Metasequoia IME. The first frontend targets IBus and reuses the shared C++ engine from [`MSIME-Engine`](https://github.com/metasequoiaime/MSIME-Engine).

The engine submodule is pinned to the tested upstream revision that provides the native desktop frontend API.

The current desktop experience supports runtime switching among Quanpin, Shuangpin, Wubi and Japanese Romaji, a Chinese/direct-input toggle, mixed English/Emoji/kaomoji candidates, dedicated English candidates, live candidates from local SQLite dictionaries, keyboard and mouse candidate selection, paging, Chinese/English punctuation, half/full-width input, configurable inline preedit, Quanpin/Shuangpin helpcodes, and persistent per-user settings.

The Linux desktop tools include a GTK settings application, a clipboard-history store and panel, and a small screen-keyboard/handwriting workspace. Voice transcription is available through the standalone `metasequoia-ime-voice` command using the configured HTTPS provider; it accepts an existing WAV file or records from Linux audio with `--record SECONDS` when `arecord` or `pw-record` is installed.

## Dependencies

On Debian/Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libibus-1.0-dev libboost-dev libfmt-dev libspdlog-dev libsqlite3-dev libcurl4-openssl-dev libsecret-1-dev libgtk-3-dev gnome-keyring python3 python3-gi python3-pypinyin gir1.2-ibus-1.0 ibus dbus-x11 iso-codes
```

For local Chinese handwriting recognition, also install `tesseract-ocr` and `tesseract-ocr-chi-sim`. The desktop tool remains usable without them and reports the missing backend in its status line.

## Build and test

```sh
git clone --recursive https://github.com/metasequoiaime/MSIME-Linux.git
cd MSIME-Linux
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
- In Quanpin or Shuangpin, press `Shift+J` with no active composition to enter super-jianpin mode. Each following letter represents one initial; Shuangpin initial keys follow the active profile. Page and select candidates normally, or set `super-jianpin-mode=false` to disable this mode.
- In Quanpin or Shuangpin with no active composition, press `Shift+Y` for one temporary English composition (raw text first, followed by completions), or `Shift+R` for one temporary Japanese Romaji composition. Committing, cancelling, or deleting the bare prefix returns to the original Chinese scheme. Set `temporary-english-mode=false` or `temporary-japanese-mode=false` to disable either shortcut.
- Press `Ctrl+Shift+E` to enter or leave dedicated English mode. Type letters to query English-only prefix candidates; Space selects the highlighted candidate, while Return commits and learns raw alphabetic input without leaving the mode.
- Set `mixed-english-candidates=true`, `mixed-emoji-candidates=true`, or `mixed-kaomoji-candidates=true` to merge those local sources into normal Quanpin and Shuangpin candidates. The Windows-compatible priority is the leading Chinese candidate, then the first English, Emoji, and kaomoji matches, followed by the remaining local and source-group candidates with stable deduplication. Emoji and kaomoji start at two typed characters; `mixed-english-minimum-prefix` controls the English threshold and accepts 1–8. All three mixed sources are disabled by default, and the default English threshold is 2.
- Online candidates are enabled by default and are fetched asynchronously after a 500 ms idle period. Google cloud suggestions occupy the second candidate slot. Network failures, timeouts, reset, focus-out, and stale generations never block or alter local input. Set `cloud-enabled=false` in `[online]` to disable them.
- AI suggestions use an OpenAI-compatible provider (`deepseek`, `openai`, `siliconflow`, `groq`, or `custom`) and occupy the third slot. Configure non-secret values in `[ai]` (`enabled`, `provider`, `endpoint`, `model`, `prompt`, and `candidate-limit`); the API token is stored in the desktop Secret Service, never in `config.ini` or diagnostics. Only HTTPS endpoints are accepted at runtime.
- Candidate translations are shown as display metadata (`候选 · gloss`) and never change the committed candidate text. Local English/Chinese glosses are preferred; set `provider=deeplx` in `[translation]` to enable an HTTPS DeepLX-compatible fallback. Configure `target-language` and `endpoint` in `[translation]`; its Bearer token is stored in Secret Service. Translation errors clear only the gloss and leave candidate selection functional.
- Set `word-to-character=true` to make `[` commit the first Han character and `]` the last Han character of the highlighted candidate. If `bracket-paging=true`, bracket paging takes precedence and word-to-character selection is disabled for those keys.
- With `smart-punctuation=true`, comma, period and colon remain ASCII after an ASCII letter or digit. Repeating the same mark within two seconds replaces it with its Chinese form when `smart-punctuation-repeat-to-chinese=true`; unavailable surrounding text safely falls back to Chinese punctuation.
- With `paired-punctuation=true`, opening quotes, brackets, braces, book-title marks and parentheses insert both halves and leave the cursor between them.
- Set `preedit-style=raw`, `pinyin`, or `hidden` to show the typed keys, segmented pinyin, or no inline preedit. Hidden inline preedit does not hide the candidate lookup table.
- Quanpin and Shuangpin helpcodes are independently controlled by `quanpin-helpcode` and `shuangpin-helpcode`. Their schema keys accept `lantian`, `ziranma`, `shouyou2_0`, `shouyouplus`, or `xiaohe`; helpcodes activate only after a complete base spelling.
- Local candidate learning uses `frequency-adjustment=disabled|pin|halve|linear|promote`. `pin` moves a selected non-leading candidate to the top, `halve` halves its rank, `linear` advances by `frequency-linear-step`, and `promote` advances one slot or to slot five when it is farther back. `frequency-trigger-count` controls how many selections trigger an adjustment; both numeric settings accept 1–10.
- Launch `metasequoia-ime-settings` (also available from the desktop applications menu) to edit the same XDG settings without hand-editing `config.ini`. Secret Service credentials are intentionally omitted from the form. Launch `metasequoia-ime-tools` for clipboard history, a screen keyboard that builds text for the clipboard, and the handwriting workspace. Launch `metasequoia-ime-toolbar` for an always-on-top shortcut bar to these desktop tools.
- Set `voice.enabled=true` and configure the `[voice]` endpoint/model in the settings application, then run `metasequoia-ime-voice --file recording.wav` or `metasequoia-ime-voice --record 5`. Set `voice.polish-enabled=true` to send the transcript through the configured Chat Completions endpoint before printing it. The API token is stored in Secret Service under the voice provider and is never written to `config.ini`; failed transcription or optional polishing leaves the local input engine unaffected.

Settings are stored in `$XDG_CONFIG_HOME/metasequoiaime/config.ini`, falling back to `~/.config/metasequoiaime/config.ini`. The `[input]` group stores the local input settings listed above. Online non-secret values are stored in `[online]` (`cloud-enabled`, `connect-timeout-ms`, `total-timeout-ms`), `[ai]` (`enabled`, `provider`, `endpoint`, `model`, `prompt`, `candidate-limit`), and `[translation]` (`enabled`, `provider`, `target-language`, `endpoint`). Utility visibility is stored in `[utility]` (`clipboard-history`, `floating-toolbar`), and voice options are stored in `[voice]` (`enabled`, `provider`, `endpoint`, `model`, `language`, `polish-enabled`, `polish-endpoint`, `polish-model`, `polish-prompt`). AI, translation and voice tokens are stored in the desktop Secret Service under provider-isolated attributes and are never written to this file. Edit or remove the file while the engine is not active; it will be written atomically after the next property or hotkey change. Learned weights and English raw entries are journaled in `${XDG_DATA_HOME:-$HOME/.local/share}/metasequoiaime/msime_user.db`; rerunning `scripts/install.sh` replays that journal into staged `msime.db`, `others.db`, and `english.db` files before replacing the live dictionary set as one unit.

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
| Super-jianpin mode | Supported | Engine Quanpin/Shuangpin query and frequency tests plus controller/settings and real D-Bus paging/selection smoke |
| Dedicated English input | Supported | Engine failure/learning tests, controller/mapper/settings tests, generated `english.db`, transactional install and real D-Bus commit smoke |
| Temporary English and Japanese input modes | Supported | Engine lifecycle tests plus controller/settings and real D-Bus commit/restore smoke |
| Cloud candidate suggestions | Supported | Async provider/service tests, Controller generation tests, real IBus smoke, Ubuntu CI |
| OpenAI-compatible AI suggestions | Supported | Provider contract/cache tests, Controller generation tests, real IBus smoke, Ubuntu CI |
| Candidate translation display | Supported | Local/DeepLX parser, debounce/cancellation and UTF-8 tests, real IBus smoke, Ubuntu CI |
| GTK settings application | Supported | Settings model tests, headless `--check`, install smoke |
| Clipboard history | Supported | UTF-8/size/deduplication/atomic-store tests and GTK tools panel |
| Screen keyboard workspace | Supported | GTK desktop-tools executable and headless check |
| Voice transcription from recorded WAV | Supported | HTTPS multipart provider contract tests and standalone CLI |
| Microphone voice capture | Supported | Standalone CLI capture through `arecord` or `pw-record`; provider transcription remains optional |
| Handwriting recognition | Supported | GTK stroke canvas and Tesseract `chi_sim+eng` backend; clear install guidance when unavailable |
| Floating toolbar | Supported | Always-on-top GTK utility with desktop-tool launchers and install smoke |

## Scope

This repository owns the Linux/IBus adapter, Linux installation, packaging, and CI. The Apple frontend remains in [`MSIME-Apple`](https://github.com/metasequoiaime/MSIME-Apple), while the input engine and dictionary remain shared dependencies.
