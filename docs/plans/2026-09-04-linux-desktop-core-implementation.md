# Linux Desktop Core Parity Implementation Plan

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Deliver the first complete Linux desktop input slice: runtime switching among Quanpin, Shuangpin, Wubi and Japanese, direct/IME mode switching, correct candidate cursor and paging, IBus properties, and persistent XDG settings.

**Architecture:** Extend the platform-neutral Engine `InputSession` only with scheme-control APIs. Add a Linux-only, IBus-free `InputController` that owns frontend state and keyboard semantics. Keep `IBusEngine.cpp` as a translation/presentation adapter, and persist settings through a small GLib key-file store.

**Tech Stack:** C++17, CMake/CTest, MetasequoiaImeEngine, GLib 2, IBus 1.5, SQLite, GitHub Actions on Ubuntu 24.04.

---

### Task 1: Expose safe scheme switching from the shared Engine

**Files:**
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.h`
- Modify: `vendor/MetasequoiaImeEngine/core/input_session.cpp`
- Test: `vendor/MetasequoiaImeEngine/tests/src/test_input_session.cpp`

**Step 1: Write the failing test**

Add a test that types a valid Quanpin composition, calls `switch_scheme(SchemeType::Wubi)`, and asserts that composition/candidates are cleared and `scheme()` returns Wubi. Repeat a no-composition switch to Japanese.

**Step 2: Run the focused test and verify RED**

Run:

```sh
cmake -S . -B build -DMETASEQUOIA_IME_BUILD_IBUS=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test_input_session --parallel
ctest --test-dir build -R '^input_session$' --output-on-failure
```

Expected: compilation fails because `InputSession::switch_scheme` and `scheme` do not exist.

**Step 3: Implement the minimal API**

Add:

```cpp
void InputSession::switch_scheme(SchemeType scheme) { engine_.switch_scheme(scheme); }
SchemeType InputSession::scheme() const { return engine_.current_scheme_type(); }
```

Document that switching clears the current composition and that frontends must commit first when required.

**Step 4: Run focused and full Engine tests**

Expected: the new test and existing Engine tests pass.

**Step 5: Commit and push the Engine fork branch**

Stage only the three explicit files and commit with `feat(core): expose input scheme switching`.

### Task 2: Add the platform-neutral Linux input controller

**Files:**
- Create: `src/InputController.h`
- Create: `src/InputController.cpp`
- Create: `tests/InputControllerTests.cpp`
- Modify: `CMakeLists.txt`

**Step 1: Write failing controller tests**

Cover these behaviors with a temporary SQLite dictionary:

- letters build composition only in IME mode;
- Up/Down move a bounded highlighted candidate;
- PageUp/PageDown move by page size without wrapping;
- Space commits the highlighted candidate;
- digits select relative to the visible page;
- Enter commits raw text and Escape cancels;
- switching scheme or entering direct mode commits the highlighted candidate first;
- a host shortcut commits composition but remains unhandled.

Use real `InputSession` and real SQLite rows; do not mock candidate behavior.

**Step 2: Verify RED**

Run `cmake --build build --target MetasequoiaImeLinuxControllerTests --parallel` and confirm the missing controller target/API is the failure.

**Step 3: Implement the controller**

Define compact value types:

```cpp
enum class FrontendKey { Character, Backspace, Enter, Escape, Space, Digit, Up, Down, PageUp, PageDown };
struct FrontendKeyEvent { FrontendKey key; char character{}; unsigned digit{}; bool host_shortcut{}; };
struct ControllerResult { bool handled{}; std::optional<std::string> commit; };
```

`InputController` owns an `InputSession`, `InputMode`, page size and highlighted absolute index. Clamp the index after every candidate refresh. Candidate commits always use the absolute index.

**Step 4: Verify GREEN and refactor**

Run the focused controller test, then full `ctest`. Keep the controller independent from GLib/IBus.

**Step 5: Commit**

Commit with `feat(core): add Linux input controller`.

### Task 3: Drive the controller from IBus

**Files:**
- Modify: `src/IBusEngine.cpp`
- Test: `tests/InputControllerTests.cpp`

**Step 1: Add failing translation-boundary tests**

Where practical, extend controller cases for IBus-visible key semantics: arrows, PageUp/PageDown, `-`/`=`, comma/period paging, keypad digits and modifier passthrough. Keep raw IBus types out of the test target.

**Step 2: Replace direct `InputSession` ownership**

Store `InputController *controller` in `MetasequoiaEngine`. Translate IBus keyvals into `FrontendKeyEvent`; publish preedit/candidates from the controller snapshot; set lookup cursor with `ibus_lookup_table_set_cursor_pos`.

**Step 3: Unify panel callbacks**

Implement `page_up`, `page_down`, `cursor_up`, `cursor_down`, and `candidate_clicked` through controller methods. Confirm mouse index handling uses the lookup table's absolute candidate position.

**Step 4: Build with strict warnings and run all tests**

Run the Ubuntu 24.04 workflow-equivalent configure/build/CTest command. Expected: the real IBus executable links and all tests pass.

**Step 5: Commit**

Commit with `feat(ibus): add candidate navigation and paging`.

### Task 4: Register IBus mode and scheme properties

**Files:**
- Modify: `src/IBusEngine.cpp`
- Modify: `resources/metasequoiaime.xml.in`
- Test: `tests/InputControllerTests.cpp`

**Step 1: Test controller switching transactions**

Assert that property-driven direct mode and scheme changes return any pending commit exactly once, then expose the new mode/scheme and an empty composition.

**Step 2: Create properties**

Register a CN/EN toggle and a radio-menu for Quanpin, Shuangpin, Wubi and Japanese in `focus_in`. Implement `property_activate`, update labels/states after changes, and apply returned commit before refreshing UI.

**Step 3: Add keyboard toggle semantics**

Implement the documented Linux toggle shortcut without stealing unrelated desktop shortcuts. Key-down/key-up state must prevent auto-repeat from toggling more than once.

**Step 4: Verify**

Run unit tests and an `ibus-daemon` D-Bus smoke test that creates the engine, focuses it, and observes registered properties.

**Step 5: Commit**

Commit with `feat(ibus): expose input mode and scheme properties`.

### Task 5: Persist settings in XDG config

**Files:**
- Create: `src/SettingsStore.h`
- Create: `src/SettingsStore.cpp`
- Create: `tests/SettingsStoreTests.cpp`
- Modify: `CMakeLists.txt`
- Modify: `src/IBusEngine.cpp`

**Step 1: Write failing filesystem tests**

Use a temporary `XDG_CONFIG_HOME`. Test missing-file defaults, round-trip of mode/scheme/page size, invalid-value fallback, atomic replacement and preservation of unknown keys.

**Step 2: Implement with GLib key-file APIs**

Store `config.ini` below `metasequoiaime/`. Validate page size to 3–9 and scheme names to the four supported values. Write a temporary sibling file and rename atomically.

**Step 3: Integrate lifecycle**

Load once per engine instance, initialize the controller, and save after a property change. A save error must not interrupt typing; expose a concise auxiliary warning without logging typed text.

**Step 4: Verify**

Run focused settings tests, full CTest, Ubuntu build, and a clean-home install smoke test.

**Step 5: Commit**

Commit with `feat(config): persist Linux input settings`.

### Task 6: Documentation and phase gate

**Files:**
- Modify: `README.md`
- Modify: `scripts/install.sh`
- Modify: `.github/workflows/ci.yml`
- Create: `tests/IBusSmoke.sh`

**Step 1: Add the failing CI smoke gate**

Add an IBus D-Bus smoke command to CI and confirm it fails until the helper and runtime setup exist.

**Step 2: Document and package the slice**

Document scheme/mode controls, XDG paths, reset procedure and supported keys. Ensure install creates no root-owned user files and uninstall instructions enumerate installed artifacts.

**Step 3: Run the complete gate**

Run dictionary generation, Debug build, full CTest, IBus smoke, install into a temporary prefix, and `git diff --check`.

**Step 4: Push and watch Actions**

Push the Linux commit, wait on the exact workflow run, and require success with zero relevant annotations.

**Step 5: Update the parity matrix**

Mark only evidence-backed desktop-core items complete; leave later design phases explicitly incomplete.
