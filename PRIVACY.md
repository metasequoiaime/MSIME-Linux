# Privacy

Metasequoia IME for Linux converts keystrokes, pre-edit text and candidates locally, using the shared C++ engine and the SQLite dictionaries installed under `${XDG_DATA_HOME:-$HOME/.local/share}/metasequoiaime/`. Local conversion never contacts the network. This document describes the optional online features that do, and exactly what each one sends.

## Network features

**Cloud candidates are enabled by default.** After a 500 ms idle period the typed spelling of the active composition is sent over HTTPS to Google's input-tools service (`https://inputtools.google.com/request`) to fetch one extra candidate. Google receives that spelling, the IP address and standard request metadata. No committed text, dictionary content, learned frequencies, settings or account data are sent. Set `cloud-enabled=false` in the `[online]` group of `config.ini`, or clear the matching checkbox in `metasequoia-ime-settings`, to disable it.

**AI suggestions are disabled by default.** When enabled, the typed spelling is sent to the OpenAI-compatible endpoint that the user configures (`deepseek`, `openai`, `siliconflow`, `groq` or a custom HTTPS endpoint), together with at most 32 characters of the text immediately preceding the caret in the focused application, which the prompt asks the model to rank candidates against. That preceding text is read only when AI suggestions are enabled, only from applications that expose surrounding text to the input method, and only when nothing is selected; the cloud-candidate path never sees more than the spelling. That third party's own privacy terms then apply to the request.

**Candidate translations are enabled by default and are local by default.** The default `local` provider resolves glosses from the installed dictionaries without any network access. Only when the provider is explicitly set to `deeplx` is the candidate text sent to the configured HTTPS DeepLX endpoint. Translations are display metadata and never change the text that is committed.

**Voice transcription is disabled by default** and runs only in the separate `metasequoia-ime-voice` command, never inside the input engine. When it is used, the selected WAV file or the audio recorded through `arecord`/`pw-record` is uploaded to the configured HTTPS transcription endpoint. If `polish-enabled=true`, the returned transcript is additionally sent to the configured Chat Completions endpoint.

Only HTTPS endpoints are accepted at runtime. Every online request is asynchronous: failures, timeouts, focus changes and stale results are discarded and never block or alter local input.

## Local data

Settings are stored in `${XDG_CONFIG_HOME:-$HOME/.config}/metasequoiaime/config.ini`. Learned word frequencies and English raw entries are journaled in `${XDG_DATA_HOME:-$HOME/.local/share}/metasequoiaime/msime_user.db` and replayed into the dictionaries when `scripts/install.sh` upgrades them. Clipboard history is disabled by default; when enabled, entries are stored locally under the same data directory, in a file readable only by the current user. Content that a password manager marks as secret is skipped rather than stored: the panel checks the clipboard for the `x-kde-passwordManagerHint` and `org.nspasteboard.ConcealedType` markers that KeePassXC, Bitwarden, 1Password and Chromium set, and leaves such entries alone. Handwriting recognition runs locally through Tesseract and sends no strokes or images anywhere.

All of these files live in the current user's home directory and are protected only by that account's permissions. Users should protect the account and its backups as they would other personal data.

API tokens for the AI, translation and voice providers are stored in the desktop Secret Service (GNOME Keyring or a compatible implementation) under provider-isolated attributes. They are never written to `config.ini`, log output or diagnostics.

## General

Metasequoia IME does not sell or share personal data, and installing or using it does not create an account. If a future release adds another network-backed feature, this notice must be updated before that feature ships.

Security or privacy concerns should be reported privately as described in [SECURITY.md](SECURITY.md).
