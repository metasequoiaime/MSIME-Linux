# Linux Chinese Input Experience Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the Linux frontend to Windows-parity for punctuation, character width, word-to-character selection, preedit presentation, helpcodes, and local candidate frequency learning.

**Architecture:** Keep deterministic text conversion in a Linux IBus-free C++ utility and keep input transactions in `InputController`. `IBusEngine.cpp` remains a protocol adapter for surrounding text, cursor motion, properties, and persistence. Shared candidate and helpcode behavior is exposed through `MetasequoiaImeEngine::InputSession` on the user fork instead of being duplicated in the Linux frontend.

**Tech Stack:** C++17, CMake/CTest, MetasequoiaImeEngine, GLib 2, IBus 1.5, SQLite, GitHub Actions on Ubuntu 24.04.

---

### Task 1: Add deterministic punctuation and full-width transforms

**Files:**
- Create: `src/TextTransform.h`
- Create: `src/TextTransform.cpp`
- Create: `tests/TextTransformTests.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Write the failing transform tests**

Cover the Windows punctuation table for the commonly typed ASCII symbols, including multi-character `……` and `——`, and verify alternating Chinese single/double quotes. Verify that every printable ASCII code point maps to U+3000 or U+FF01–U+FF5E in full-width mode, while control and non-ASCII values are rejected.

Use the intended API:

```cpp
PunctuationFormatter formatter;
require(formatter.chinese(',') == "，", "Comma mapping changed.");
require(formatter.chinese('^') == "……", "Ellipsis mapping changed.");
require(formatter.chinese('"') == "“" && formatter.chinese('"') == "”",
        "Double quotes did not alternate.");
require(to_full_width('A') == "Ａ" && to_full_width(' ') == "　",
        "ASCII full-width conversion changed.");
```

**Step 2: Run the focused build and verify RED**

Run:

```sh
cmake -S . -B build -DMETASEQUOIA_IME_BUILD_IBUS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target MetasequoiaImeLinuxTextTransformTests --parallel
```

Expected: the target or `TextTransform` API is missing.

**Step 3: Implement the minimal utility**

Define `PunctuationMode`, `CharacterWidth`, `PunctuationFormatter::chinese(char)`, `is_punctuation(char)`, and `to_full_width(char)`. Use UTF-8 string literals and keep quote toggle state inside each formatter instance. Match the Windows punctuation table; do not depend on GLib or IBus.

**Step 4: Verify GREEN**

Run the focused test and full `ctest` with IBus disabled.

**Step 5: Commit**

```sh
git add CMakeLists.txt src/TextTransform.h src/TextTransform.cpp tests/TextTransformTests.cpp
git commit -m "feat(core): add punctuation and width transforms"
```

### Task 2: Add punctuation and width transactions to the controller

**Files:**
- Modify: `src/InputController.h`
- Modify: `src/InputController.cpp`
- Modify: `tests/InputControllerTests.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Write failing controller tests**

Add cases for:

- Chinese punctuation with and without an active composition;
- candidate plus punctuation committed exactly once;
- English punctuation passthrough through an explicit commit;
- apostrophe remaining a pinyin separator only after composition starts;
- `-`/`=` and optional comma/period paging retaining navigation semantics during composition;
- full-width ASCII in direct mode and for otherwise-unhandled IME input;
- punctuation taking precedence over full-width conversion;
- toggle/set accessors changing state without dropping composition.

Extend `FrontendKeyEvent` with a printable ASCII value and add `Punctuation`, `TogglePunctuation`, and `ToggleWidth` intents. Construct the controller with an `InputOptions` value so settings can be applied atomically.

**Step 2: Verify RED**

Run:

```sh
cmake --build build --target MetasequoiaImeLinuxControllerTests --parallel
ctest --test-dir build -R '^input_controller$' --output-on-failure
```

Expected: the new event/state APIs do not exist.

**Step 3: Implement the minimal state machine**

Order handling as follows:

1. host shortcuts commit an existing composition and remain unhandled;
2. toggle intents change controller state and remain handled;
3. IME letters and in-progress apostrophes feed `InputSession`;
4. active paging aliases navigate rather than emit punctuation;
5. punctuation commits the highlighted candidate, appends the selected ASCII/Chinese form, and resets once;
6. direct-mode or otherwise-unhandled printable ASCII uses full-width conversion when enabled;
7. all other keys pass through unchanged.

Keep all outputs in `KeyResult::commit`; no IBus calls belong in the controller.

**Step 4: Verify GREEN and regressions**

Run the focused controller test and all non-IBus CTest targets.

**Step 5: Commit**

```sh
git add CMakeLists.txt src/InputController.h src/InputController.cpp tests/InputControllerTests.cpp
git commit -m "feat(core): handle punctuation and full-width input"
```

### Task 3: Translate Linux key events and parity hotkeys

**Files:**
- Modify: `src/IBusKeyMapper.h`
- Modify: `src/IBusKeyMapper.cpp`
- Modify: `tests/IBusKeyMapperTests.cpp`

**Step 1: Write failing mapper tests**

Verify ASCII punctuation is dispatched with its character value, releases remain ignored, `Ctrl+.` maps only to `TogglePunctuation`, and `Ctrl+Shift+Space` maps only to `ToggleWidth`. Other Ctrl/Alt/Super shortcuts must still forward. PageUp/PageDown and Tab aliases remain explicit navigation; printable `-`, `=`, comma, and period are left for the controller to resolve against composition and paging settings.

**Step 2: Verify RED**

Run:

```sh
cmake --build build --target MetasequoiaImeLinuxIBusKeyMapperTests --parallel
ctest --test-dir build -R '^ibus_key_mapper$' --output-on-failure
```

Expected: punctuation/hotkey translations do not exist.

**Step 3: Implement exact modifier matching**

Match the two parity hotkeys before the generic host-modifier forwarding branch. Reject extra Alt/Super/Meta modifiers. Dispatch printable punctuation as `FrontendKey::Punctuation` and preserve keypad decimal as ASCII `.` rather than Chinese punctuation.

**Step 4: Verify GREEN**

Run mapper, controller, and full CTest targets on Ubuntu.

**Step 5: Commit**

```sh
git add src/IBusKeyMapper.h src/IBusKeyMapper.cpp tests/IBusKeyMapperTests.cpp
git commit -m "feat(ibus): map punctuation and width controls"
```

### Task 4: Persist and expose punctuation and width state

**Files:**
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Modify: `src/IBusEngine.cpp`
- Modify: `tests/SettingsStoreTests.cpp`
- Modify: `tests/IBusSmoke.sh`
- Modify: `README.md`

**Step 1: Write failing settings and D-Bus tests**

Round-trip `punctuation`, `full-width`, and `comma-period-paging` keys, prove field-by-field invalid fallback and unknown-key preservation, then assert initial and updated IBus property values from a real input context.

**Step 2: Verify RED**

Run `settings_store` and `ibus_smoke`; expect missing fields/properties.

**Step 3: Implement persistence and properties**

Add `Punctuation` and `CharacterWidth` toggle properties. Initialize the controller from persisted `InputOptions`; save and synchronize properties after either hotkey or property activation. Defaults are Chinese punctuation, half-width, and comma/period paging disabled, matching the Windows defaults while keeping `-`/`=` and Tab paging available.

**Step 4: Verify GREEN**

Run full Ubuntu CTest, real IBus smoke, temporary-HOME install smoke, `actionlint`, shell syntax checks, and `git diff --check`.

**Step 5: Commit**

```sh
git add README.md src/SettingsStore.h src/SettingsStore.cpp src/IBusEngine.cpp tests/SettingsStoreTests.cpp tests/IBusSmoke.sh
git commit -m "feat(config): persist punctuation and width state"
```

### Task 5: Implement word-to-character selection through the Engine API

**Files:**
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.h`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `vendor/MetasequoiaImeEngine/tests/src/test_input_session.cpp`
- Modify: `src/InputController.h`
- Modify: `src/InputController.cpp`
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Modify: `src/IBusKeyMapper.cpp`
- Test: `tests/InputControllerTests.cpp`

**Step 1: Write failing Engine and controller tests**

Add `CandidateEdge::{FirstHan,LastHan}` selection tests with BMP and supplementary-plane Han characters. Add controller tests proving `[`/`]` commit the highlighted candidate edge only when enabled and only when bracket paging is disabled.

**Step 2: Verify RED**

Run Engine `input_session` and Linux `input_controller`; expect missing edge-selection APIs.

**Step 3: Implement on the Engine fork first**

Reuse the shared UTF-8/Han helpers rather than byte slicing. Selection must reset the composition exactly like a normal candidate commit. Push the Engine fork commit, update the Linux submodule pointer, then add controller/settings integration.

**Step 4: Verify GREEN**

Run all Engine and Linux tests, including a real D-Bus commit signal assertion for first/last selection.

**Step 5: Commit**

Commit and push the Engine fork explicitly, then commit the Linux submodule pointer and adapter changes.

### Task 6: Add smart and paired punctuation without blocking local input

**Files:**
- Modify: `src/TextTransform.h`
- Modify: `src/TextTransform.cpp`
- Modify: `src/InputController.h`
- Modify: `src/InputController.cpp`
- Modify: `src/IBusEngine.cpp`
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Test: `tests/TextTransformTests.cpp`
- Test: `tests/InputControllerTests.cpp`
- Test: `tests/IBusSmoke.sh`

**Step 1: Write failing deterministic tests**

Cover ASCII comma/period/colon after ASCII letters or digits, Chinese fallback without surrounding text, two-second repeat-to-Chinese replacement, paired quotes/brackets, and the requested cursor-left count. Inject time into the punctuation state machine so tests never sleep.

**Step 2: Extend the controller result**

Introduce a Linux `ControllerResult` carrying `handled`, optional commit text, delete-before count, and cursor-left count. Adapt existing controller and IBus result application without changing the shared Engine `KeyResult`.

**Step 3: Integrate IBus surrounding text conservatively**

Request surrounding-text capability, read only the immediately preceding Unicode scalar, and fall back to Chinese punctuation if unavailable. Apply replacement/cursor movement only through normal IBus commit/forward events. Invalidate repeat state on focus/reset/navigation so stale edits cannot delete unrelated application text.

**Step 4: Verify GREEN**

Run deterministic unit tests and observe commit/forward-key D-Bus signals in `ibus_smoke`.

**Step 5: Commit**

Commit as `feat(ibus): add smart and paired punctuation`.

### Task 7: Expose preedit styles and helpcode controls

**Files:**
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.h`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `vendor/MetasequoiaImeEngine/tests/src/test_input_session.cpp`
- Modify: `src/InputController.h`
- Modify: `src/InputController.cpp`
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Modify: `src/IBusEngine.cpp`
- Test: `tests/InputControllerTests.cpp`
- Test: `tests/SettingsStoreTests.cpp`
- Test: `tests/IBusSmoke.sh`

**Step 1: Write failing shared API tests**

Expose raw and normalized segmentation from `InputSession`, plus safe Quanpin/Shuangpin helpcode enablement and supported helpcode schema selection. Prove helpcodes change candidates only after a complete base spelling.

**Step 2: Add Linux presentation settings**

Support inline preedit `raw`, `pinyin`, or `hidden`; keep the IBus lookup table visible even when inline preedit is hidden. Persist independent helpcode toggles and schema with validation.

**Step 3: Verify on the Engine fork and Linux**

Run shared Engine tests before pushing its fork commit, update the submodule, then run Linux unit/D-Bus tests.

**Step 4: Commit**

Commit Engine and Linux changes separately with explicit paths.

### Task 8: Learn local candidate frequency safely

**Files:**
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.h`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Modify: `vendor/MetasequoiaImeEngine/user_dictionary/*` as required by the selected algorithm
- Modify: `src/SettingsStore.h`
- Modify: `src/SettingsStore.cpp`
- Test: `vendor/MetasequoiaImeEngine/tests/src/test_input_session.cpp`
- Test: `tests/InputControllerTests.cpp`

**Step 1: Audit current Windows algorithms and storage**

Trace off/top/half/linear/advance-once behavior and trigger frequency to the authoritative Engine mutation. Confirm writes go to an XDG user-data database, never the packaged system dictionary.

**Step 2: Write failing persistence and ordering tests**

For each algorithm, select a non-first local candidate enough times to cross the configured trigger, reopen a session, and assert the documented new ordering. Add write-failure tests showing candidate commit still succeeds and emits a non-sensitive diagnostic.

**Step 3: Implement the minimal shared learning API**

Keep ranking and mutation in the Engine. The Linux controller only reports a successful selection and configuration; it must not issue SQL or know dictionary tables.

**Step 4: Verify and document**

Run clean XDG-data tests, the full Engine/Linux matrix, and update README parity evidence only for behavior proven by tests.

**Step 5: Commit and phase gate**

Push both fork branches, watch the exact Actions run, request final code review, and leave only evidence-backed parity items marked supported.
