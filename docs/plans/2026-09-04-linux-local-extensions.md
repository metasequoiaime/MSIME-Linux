# Linux Local Extensions Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the Linux IBus frontend to Windows-parity for local English, Emoji, kaomoji, date/time, Unicode, quick phrase, super-jianpin, and temporary English/Japanese input modes.

**Architecture:** Put deterministic queries and special-mode composition state in the shared `MetasequoiaImeEngine::InputSession`. Keep `InputController` responsible for cursor/page policy and settings, and keep `IBusEngine.cpp` limited to translating key events and publishing snapshots. Reuse the Windows Server algorithms and dictionary schemas, but replace Win32 time/path/thread APIs with portable C++17 and existing XDG data paths.

**Tech Stack:** C++17, CMake/CTest, SQLite, MetasequoiaImeEngine, GLib key files, IBus 1.5, Ubuntu 24.04 GitHub Actions.

---

### Task 1: Add the shared special-mode foundation and Unicode mode

**Files:**
- Create: `vendor/MetasequoiaImeEngine/local_modes/unicode_query.h`
- Create: `vendor/MetasequoiaImeEngine/local_modes/unicode_query.cpp`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.h`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `vendor/MetasequoiaImeEngine/CMakeLists.txt`
- Test: `vendor/MetasequoiaImeEngine/tests/src/test_input_session.cpp`

**Step 1: Write the failing shared tests**

Add tests proving that only `Shift+U` at the start of Quanpin/Shuangpin composition enters `LocalInputMode::Unicode`; the visible preedit retains `U` and optional `+`; bare digits are hex input; `U4e00` and `U+1f600` produce generated `一` and `😀` candidates; surrogate, overflow, overlong, and non-hex input never create candidates; Backspace at the prefix exits the mode; commit, cancel, scheme switch, and reset clear it.

**Step 2: Verify RED**

Run:

```sh
cmake --build /tmp/engine-build --target test_input_session --parallel
ctest --test-dir /tmp/engine-build -R '^input_session$' --output-on-failure
```

Expected: compilation fails because `LocalInputMode`, modifier-aware character input, and Unicode query support do not exist.

**Step 3: Implement the minimal shared state**

Expose:

```cpp
enum class LocalInputMode { None, Unicode, DateTime, QuickPhrase, Emoji, Kaomoji,
                            SuperJianpin, TemporaryEnglish, TemporaryJapanese };

struct LocalModeOptions {
    bool unicode = true;
    bool date_time = true;
    bool quick_phrase = true;
    bool emoji = true;
    bool kaomoji = true;
    bool super_jianpin = true;
    bool temporary_english = true;
    bool temporary_japanese = true;
};

KeyResult handle_character(char character, bool shift_only = false);
LocalInputMode local_input_mode() const;
```

Keep local preedit/candidates in `InputSession`; while a local mode is active, never send its prefix/payload through the normal pinyin engine. Port the Windows Unicode scalar parser exactly and emit `CandidateSource::Generated`.

**Step 4: Verify GREEN**

Run the focused Engine test and all Engine CTest targets.

**Step 5: Commit Engine**

```sh
git -C vendor/MetasequoiaImeEngine add CMakeLists.txt core/input_session.h core/input_session.cpp \
  local_modes/unicode_query.h local_modes/unicode_query.cpp tests/src/test_input_session.cpp
git -C vendor/MetasequoiaImeEngine commit -m "feat(core): add Unicode local input mode"
```

### Task 2: Route Unicode mode through Linux key handling and settings

**Files:**
- Modify: `src/IBusKeyMapper.cpp`
- Modify: `src/InputController.h`
- Modify: `src/InputController.cpp`
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Modify: `src/IBusEngine.cpp`
- Test: `tests/IBusKeyMapperTests.cpp`
- Test: `tests/InputControllerTests.cpp`
- Test: `tests/SettingsStoreTests.cpp`
- Test: `tests/IBusSmoke.sh`

**Step 1: Write failing mapper/controller/settings tests**

Require mapper output to retain exact Shift-only state. Verify bare digits and the optional plus feed Unicode composition, while `Shift+1..9` select candidates. Verify `unicode-mode=true|false` persistence and field-local invalid fallback. In a real IBus context, enter `Shift+U 4 e 0 0 Space`, observe preedit/candidate updates, and commit `一`.

**Step 2: Verify RED**

Run `ibus_key_mapper`, `input_controller`, `settings_store`, and `ibus_smoke`; expect the missing modifier/mode wiring to fail.

**Step 3: Implement modifier-preserving routing**

Add `bool shift_only` to `FrontendKeyEvent`. The controller first offers printable input to an active local mode; only unconsumed punctuation/digits continue through normal paging, selection, punctuation, and full-width rules. Load `LocalModeOptions` from `SettingsStore` and apply them before processing input.

**Step 4: Verify GREEN**

Run focused tests and the complete Linux CTest suite.

**Step 5: Commit Linux**

Commit explicit adapter/settings/test paths plus the Engine gitlink as `feat(ibus): expose Unicode input mode`.

### Task 3: Port deterministic date/time mode

**Files:**
- Create: `vendor/MetasequoiaImeEngine/local_modes/date_time_query.h`
- Create: `vendor/MetasequoiaImeEngine/local_modes/date_time_query.cpp`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `vendor/MetasequoiaImeEngine/CMakeLists.txt`
- Test: `vendor/MetasequoiaImeEngine/tests/src/test_local_modes.cpp`
- Test: `tests/InputControllerTests.cpp`
- Test: `tests/IBusSmoke.sh`

**Step 1: Write deterministic RED tests**

Use a portable `LocalDateTime` value fixed at 2026-08-09 14:30:00 Sunday. Cover the Windows aliases `rq|riqi|date`, `sj|shijian|time`, and `xq|xingqi|week`, exact candidate ordering, lunar date, limit handling, `Shift+T` entry, incomplete/unknown keywords, and reset/commit behavior.

**Step 2: Port the formatter**

Keep the Windows candidate strings and 1900–2100 lunar table. Replace `SYSTEMTIME`, `GetLocalTime`, and `SystemTimeToFileTime` with `LocalDateTime`, `std::chrono::system_clock`, thread-safe local-time conversion, and a civil-date day-count helper.

**Step 3: Integrate and verify**

Drive `Shift+T r q Space` through `InputSession`, `InputController`, and real IBus D-Bus smoke. Run all Engine/Linux tests.

**Step 4: Commit both layers**

Commit/push Engine first, update the Linux gitlink, then commit Linux tests and documentation.

### Task 4: Add XDG quick phrases and K mode

**Files:**
- Create: `vendor/MetasequoiaImeEngine/local_modes/quick_phrase_query.h`
- Create: `vendor/MetasequoiaImeEngine/local_modes/quick_phrase_query.cpp`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `scripts/install.sh`
- Modify: `src/DictionaryReplay.cpp`
- Test: Engine query/input-session tests, `tests/InstallSmoke.sh`, `tests/IBusSmoke.sh`

**Steps:**

1. RED-test `Shift+K` plus lowercase letter codes, prefix ordering, generated/quick-phrase source, commit/reset, missing/corrupt DB diagnostics, and XDG persistence.
2. Port the Windows `quick_parases(key,value,weight)` query and journal schema behavior; never mutate packaged source data directly.
3. Seed quick phrases from `vendor/MetasequoiaImeDict/mix/quick_phrases.txt`, replay user edits during staged install, and verify successful/failed upgrade paths.
4. Run full tests, commit Engine first, then Linux.

### Task 5: Port Emoji and kaomoji explicit modes

**Files:**
- Create: shared `local_modes/emoji_query.*` and `local_modes/kaomoji_query.*`
- Modify: dictionary build/install scripts to create and stage `others.db`
- Modify: shared `InputSession` mode dispatch
- Test: full-pinyin, jianpin, supported shuangpin, English keyword, deduplication, ordering, missing/corrupt DB, E/M mode D-Bus commit

**Steps:**

1. RED-test the Windows `others.db` schemas and exact prefix expansion rules.
2. Port query code without the Windows worker globals; synchronous local SQLite lookup stays below the existing latency budget.
3. Build/install `others.db` from pinned source text and add atomic upgrade smoke coverage.
4. Verify and commit both repositories.

### Task 6: Add mixed English and dedicated English mode

**Files:**
- Modify shared `InputSession` to merge `EnglishDictionary::query_prefix` results
- Modify Linux mapper/controller/settings for `Ctrl+Shift+E`
- Test Engine ordering, minimum-prefix threshold, dedicated mode raw fallback, learning/journal replay, settings and real D-Bus behavior

**Steps:**

1. RED-test mixed English insertion after local Chinese candidates and dedicated English-only candidates.
2. Add generation-free synchronous local queries; local Chinese candidates never wait on English failures.
3. Support `Ctrl+Shift+E`, candidate selection, raw Enter, automatic learning, and persisted enable/minimum-prefix settings.
4. Verify and commit.

### Task 7: Merge mixed Emoji/kaomoji candidates

**Files:** shared `InputSession`, Linux settings, Engine/controller/IBus tests

**Steps:**

1. RED-test Windows ordering: English candidates, then Emoji, then kaomoji, with local Chinese first and deduplication stable.
2. Reuse Task 5 queries and make each source independently disable/fail without affecting local input.
3. Persist feature toggles, verify real D-Bus ordering, and commit.

### Task 8: Port super-jianpin J mode

**Files:** shared `local_modes/jianpin_query.*`, `InputSession`, settings, tests

**Steps:**

1. RED-test Quanpin initials and each supported Shuangpin profile expansion against Windows fixtures, including distinct ambiguous inputs.
2. Port the query without Windows IPC/global state; retain canonical ranking keys and Task 8 frequency behavior.
3. Verify explicit J-mode paging/selection/reopen ordering and commit.

### Task 9: Add temporary English Y and Japanese R modes

**Files:** shared `InputSession`, Linux controller/settings, tests

**Steps:**

1. RED-test Y raw candidate plus English completions and R temporary Japanese session, including prefix Backspace, commit, cancel, scheme-switch, and return to the original Chinese scheme.
2. Reuse `EnglishDictionary` and the existing Japanese scheme; never create a second Linux-only conversion engine.
3. Verify real D-Bus commits and all exit paths, then commit.

### Task 10: Phase gate, install data, and documentation

**Files:** `README.md`, `.github/workflows/ci.yml`, install/build scripts, parity matrix

**Steps:**

1. Run clean Engine and Linux builds, all CTest targets, real D-Bus smoke, staged upgrade/replay smoke, actionlint, shellcheck, and diff checks.
2. Independently review every local-mode source and failure path; resolve all Critical/Important findings.
3. Push Engine fork before Linux gitlink, watch the exact GitHub Actions run, and only then mark the local-extension matrix supported.
4. Keep the full-version goal active for online capabilities and desktop tools/release work.
