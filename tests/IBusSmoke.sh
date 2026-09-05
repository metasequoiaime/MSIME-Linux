#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build}
build_dir=$(cd "$build_dir" && pwd)
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
engine="$build_dir/metasequoia-ime-ibus"
component="$build_dir/metasequoiaime.xml"

if [[ ! -x "$engine" || ! -f "$component" ]]; then
    echo "IBus build outputs are missing in $build_dir." >&2
    exit 1
fi

smoke_root=$(mktemp -d)
daemon_pid=
cleanup() {
    if [[ -n "$daemon_pid" ]]; then
        kill "$daemon_pid" 2>/dev/null || true
    fi
    rm -rf "$smoke_root"
}
trap cleanup EXIT

export HOME="$smoke_root/home"
export XDG_CACHE_HOME="$smoke_root/cache"
export XDG_CONFIG_HOME="$smoke_root/config"
export XDG_RUNTIME_DIR="$smoke_root/run"
export DISPLAY=:99
export IBUS_USE_PORTAL=0
# gvfs mounts a fuse directory inside XDG_RUNTIME_DIR on newer desktops, and the
# cleanup rm cannot remove that mount point. The smoke run needs no gvfs at all.
export GIO_USE_VFS=local
export IBUS_COMPONENT_PATH="$smoke_root/components"
source_data_dir=${METASEQUOIA_IME_DATA_DIR:-"$project_root/vendor/MetasequoiaImeDict/out"}
source_helpcode_dir="$project_root/vendor/MetasequoiaImeEngine/helpcode/helpcodes"
if [[ -d "$source_data_dir/helpcodes" ]]; then
    source_helpcode_dir="$source_data_dir/helpcodes"
fi
export METASEQUOIA_IME_DATA_DIR="$smoke_root/data"

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR" "$IBUS_COMPONENT_PATH" \
    "$METASEQUOIA_IME_DATA_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cp --reflink=auto "$source_data_dir/msime.db" "$METASEQUOIA_IME_DATA_DIR/msime.db"
cp --reflink=auto "$source_data_dir/others.db" "$METASEQUOIA_IME_DATA_DIR/others.db"
cp --reflink=auto "$source_data_dir/english.db" "$METASEQUOIA_IME_DATA_DIR/english.db"
ln -s "$source_helpcode_dir" "$METASEQUOIA_IME_DATA_DIR/helpcodes"
sed "s|<exec>.*</exec>|<exec>$engine --ibus</exec>|" "$component" >"$IBUS_COMPONENT_PATH/metasequoiaime.xml"
mkdir -p "$XDG_CONFIG_HOME/metasequoiaime"
printf '%s\n' \
    '[input]' \
    'mode=direct' \
    'scheme=wubi' \
    'page-size=3' \
    'punctuation=english' \
    'full-width=true' \
    'comma-period-paging=true' \
    'show-quanpin-helpcode=false' \
    'show-shuangpin-helpcode=false' \
    'word-to-character=true' \
    'bracket-paging=false' \
    'smart-punctuation=true' \
    'smart-punctuation-repeat-to-chinese=true' \
    'paired-punctuation=true' \
    'preedit-style=hidden' \
    'quanpin-helpcode=true' \
    'quanpin-helpcode-schema=lantian' \
    'shuangpin-helpcode=true' \
    'shuangpin-helpcode-schema=xiaohe' \
    'frequency-adjustment=pin' \
    'frequency-trigger-count=1' \
    'frequency-linear-step=2' \
    'unicode-mode=true' \
    'super-jianpin-mode=true' \
    'temporary-english-mode=true' \
    'temporary-japanese-mode=true' \
    'mixed-english-candidates=true' \
    'mixed-english-minimum-prefix=2' \
    'mixed-emoji-candidates=true' \
    'mixed-kaomoji-candidates=true' \
    'future-option=preserve-me' \
    >"$XDG_CONFIG_HOME/metasequoiaime/config.ini"

dbus-run-session -- bash -s -- "$smoke_root" <<'SESSION'
set -euo pipefail
smoke_root=$1

ibus write-cache
ibus-daemon --verbose --single --panel disable --config disable --cache auto >"$smoke_root/ibus-daemon.log" 2>&1 &
daemon_pid=$!
echo "$daemon_pid" >"$smoke_root/ibus-daemon.pid"

for _ in {1..100}; do
    if ibus list-engine 2>/dev/null | grep -q 'metasequoiaime'; then
        break
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        cat "$smoke_root/ibus-daemon.log" >&2
        exit 1
    fi
    sleep 0.05
done
ibus list-engine | grep -q 'metasequoiaime'

if ! python3 - <<'PYTHON'
import gi
import os
from pathlib import Path
import sqlite3
import subprocess

gi.require_version("IBus", "1.0")
from gi.repository import Gio, GLib, IBus

IBus.init()
bus = IBus.Bus()
if not bus.is_connected():
    raise RuntimeError("The smoke client could not connect to IBus.")

context = bus.create_input_context("metasequoia-smoke")
expected = {
    "InputMode",
    "Punctuation",
    "CharacterWidth",
    "Scheme",
    "Scheme.Quanpin",
    "Scheme.Shuangpin",
    "Scheme.Wubi",
    "Scheme.Japanese",
}
payloads = []
property_snapshots = []
property_updates = []
preedit_updates = []
lookup_updates = []
lookup_candidate_updates = []
loop = GLib.MainLoop()


def collect_properties(prop_list, destination):
    index = 0
    while prop_list is not None:
        prop = prop_list.get(index)
        if prop is None:
            break
        destination[prop.get_key()] = (prop.get_label().get_text(), prop.get_state())
        collect_properties(prop.get_sub_props(), destination)
        index += 1


def registered_properties(_connection, _sender, _path, _interface, _signal, parameters):
    payloads.append(str(parameters))
    serialized = parameters.get_child_value(0).get_variant()
    prop_list = IBus.Serializable.deserialize_object(serialized)
    snapshot = {}
    collect_properties(prop_list, snapshot)
    property_snapshots.append(snapshot)
    if expected <= snapshot.keys():
        loop.quit()


def updated_property(_connection, _sender, _path, _interface, _signal, parameters):
    serialized = parameters.get_child_value(0).get_variant()
    prop = IBus.Serializable.deserialize_object(serialized)
    property_updates.append((prop.get_key(), prop.get_label().get_text(), prop.get_state()))


def preedit_updated(_connection, _sender, _path, _interface, _signal, parameters):
    serialized = parameters.get_child_value(0).get_variant()
    inline_text = IBus.Serializable.deserialize_object(serialized).get_text()
    visible = parameters.get_child_value(2).get_boolean()
    preedit_updates.append((inline_text, visible))


def lookup_updated(_connection, _sender, _path, _interface, _signal, parameters):
    serialized = parameters.get_child_value(0).get_variant()
    table = IBus.Serializable.deserialize_object(serialized)
    first_candidate = None
    candidate_count = table.get_number_of_candidates()
    if candidate_count > 0:
        first_candidate = table.get_candidate(0).get_text()
    candidates = tuple(table.get_candidate(index).get_text() for index in range(candidate_count))
    visible = parameters.get_child_value(1).get_boolean()
    lookup_updates.append((first_candidate, visible))
    lookup_candidate_updates.append((candidates, visible))


def latest_property(key):
    for updated_key, label, state in reversed(property_updates):
        if updated_key == key:
            return label, state
    return None


connection = bus.get_connection()
connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "RegisterProperties",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    registered_properties,
)
connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "UpdateProperty",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    updated_property,
)
connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "UpdatePreeditText",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    preedit_updated,
)
connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "UpdateLookupTable",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    lookup_updated,
)
context.set_capabilities(
    IBus.Capabilite.FOCUS
    | IBus.Capabilite.PREEDIT_TEXT
    | IBus.Capabilite.AUXILIARY_TEXT
    | IBus.Capabilite.LOOKUP_TABLE
    | IBus.Capabilite.PROPERTY
    | IBus.Capabilite.SURROUNDING_TEXT
)
context.focus_in()
context.set_engine("metasequoiaime")


def engine_is_active():
    active_engine = context.get_engine()
    return active_engine is not None and active_engine.get_name() == "metasequoiaime"


def activate_globally_if_needed():
    # ibus 1.5.34 no longer activates the engine from set_engine() alone: the
    # context stays on the dummy engine and no engine process is ever spawned.
    # Older releases do activate it per context, and setting the global engine
    # there breaks the property activation round-trip below, so this is only a
    # fallback for when the per-context path did not take.
    if not engine_is_active():
        bus.set_global_engine("metasequoiaime")
    return GLib.SOURCE_REMOVE


def refocus_when_active():
    if engine_is_active():
        context.focus_in()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


GLib.timeout_add(500, activate_globally_if_needed)
GLib.timeout_add(20, refocus_when_active)
GLib.timeout_add_seconds(5, loop.quit)
loop.run()

payload = "\n".join(payloads)
missing = sorted(key for key in expected if key not in payload)
if missing:
    active_engine = context.get_engine()
    active_name = None if active_engine is None else active_engine.get_name()
    processes = subprocess.run(["ps", "-ef"], check=True, capture_output=True, text=True).stdout
    engine_processes = [line for line in processes.splitlines() if "metasequoia-ime-ibus" in line]
    raise RuntimeError(
        f"IBus properties were not registered: {missing}; active engine: {active_name}; "
        f"processes: {engine_processes}; payload: {payload}"
    )

initial_properties = property_snapshots[-1]
if not (
    initial_properties["InputMode"] == ("英", IBus.PropState.UNCHECKED)
    and initial_properties["Scheme"][0] == "五笔"
    and initial_properties["Scheme.Wubi"][1] == IBus.PropState.CHECKED
    and initial_properties["Scheme.Quanpin"][1] == IBus.PropState.UNCHECKED
    and initial_properties["Punctuation"] == ("英标", IBus.PropState.UNCHECKED)
    and initial_properties["CharacterWidth"] == ("全", IBus.PropState.CHECKED)
):
    raise RuntimeError(f"Persisted input settings were not reflected in initial IBus properties: {initial_properties}")

context.property_activate("Scheme.Japanese", IBus.PropState.CHECKED)
context.property_activate("InputMode", IBus.PropState.CHECKED)
context.property_activate("Punctuation", IBus.PropState.CHECKED)
context.property_activate("CharacterWidth", IBus.PropState.UNCHECKED)
settings_path = Path(os.environ["XDG_CONFIG_HOME"]) / "metasequoiaime" / "config.ini"


def settings_saved():
    contents = settings_path.read_text(encoding="utf-8")
    saved = all(
        value in contents
        for value in (
            "mode=ime",
            "scheme=japanese",
            "page-size=3",
            "punctuation=chinese",
            "full-width=false",
            "comma-period-paging=true",
            "word-to-character=true",
            "bracket-paging=false",
            "smart-punctuation=true",
            "smart-punctuation-repeat-to-chinese=true",
            "paired-punctuation=true",
            "preedit-style=hidden",
            "quanpin-helpcode=true",
            "quanpin-helpcode-schema=lantian",
            "shuangpin-helpcode=true",
            "shuangpin-helpcode-schema=xiaohe",
            "frequency-adjustment=pin",
            "frequency-trigger-count=1",
            "frequency-linear-step=2",
            "unicode-mode=true",
            "super-jianpin-mode=true",
            "temporary-english-mode=true",
            "temporary-japanese-mode=true",
            "mixed-english-candidates=true",
            "mixed-english-minimum-prefix=2",
            "mixed-emoji-candidates=true",
            "mixed-kaomoji-candidates=true",
            "future-option=preserve-me",
        )
    )
    synchronized = (
        latest_property("Punctuation") == ("中标", IBus.PropState.CHECKED)
        and latest_property("CharacterWidth") == ("半", IBus.PropState.UNCHECKED)
    )
    if saved and synchronized:
        settings_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


settings_loop = GLib.MainLoop()
GLib.timeout_add(20, settings_saved)
GLib.timeout_add_seconds(5, settings_loop.quit)
settings_loop.run()
saved_contents = settings_path.read_text(encoding="utf-8")
if not all(
    value in saved_contents
    for value in (
        "mode=ime",
        "scheme=japanese",
        "page-size=3",
        "punctuation=chinese",
        "full-width=false",
        "comma-period-paging=true",
        "word-to-character=true",
        "bracket-paging=false",
        "smart-punctuation=true",
        "smart-punctuation-repeat-to-chinese=true",
        "paired-punctuation=true",
        "preedit-style=hidden",
        "quanpin-helpcode=true",
        "quanpin-helpcode-schema=lantian",
        "shuangpin-helpcode=true",
        "shuangpin-helpcode-schema=xiaohe",
        "frequency-adjustment=pin",
        "frequency-trigger-count=1",
        "frequency-linear-step=2",
        "unicode-mode=true",
        "super-jianpin-mode=true",
        "temporary-english-mode=true",
        "temporary-japanese-mode=true",
        "mixed-english-candidates=true",
        "mixed-english-minimum-prefix=2",
        "mixed-emoji-candidates=true",
        "mixed-kaomoji-candidates=true",
        "future-option=preserve-me",
    )
):
    raise RuntimeError("IBus property changes were not persisted through SettingsStore.")
if not (
    latest_property("Punctuation") == ("中标", IBus.PropState.CHECKED)
    and latest_property("CharacterWidth") == ("半", IBus.PropState.UNCHECKED)
):
    raise RuntimeError(f"IBus property activations were not synchronized: {property_updates}")

if not context.process_key_event(IBus.KEY_period, 0, IBus.ModifierType.CONTROL_MASK):
    raise RuntimeError("Ctrl+period was not handled by the active IBus engine.")
if not context.process_key_event(
    IBus.KEY_space,
    0,
    IBus.ModifierType.CONTROL_MASK | IBus.ModifierType.SHIFT_MASK,
):
    raise RuntimeError("Ctrl+Shift+Space was not handled by the active IBus engine.")


def hotkeys_are_saved_and_synchronized():
    contents = settings_path.read_text(encoding="utf-8")
    return (
        "punctuation=english" in contents
        and "full-width=true" in contents
        and latest_property("Punctuation") == ("英标", IBus.PropState.UNCHECKED)
        and latest_property("CharacterWidth") == ("全", IBus.PropState.CHECKED)
    )


def wait_for_hotkeys():
    if hotkeys_are_saved_and_synchronized():
        hotkey_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


hotkey_loop = GLib.MainLoop()
GLib.timeout_add(20, wait_for_hotkeys)
GLib.timeout_add_seconds(5, hotkey_loop.quit)
hotkey_loop.run()
if not hotkeys_are_saved_and_synchronized():
    raise RuntimeError(
        f"Parity hotkeys were not persisted and synchronized: {settings_path.read_text(encoding='utf-8')}; "
        f"updates: {property_updates}"
    )

committed_text = []


def text_committed(_connection, _sender, _path, _interface, _signal, parameters):
    serialized = parameters.get_child_value(0).get_variant()
    committed_text.append(IBus.Serializable.deserialize_object(serialized).get_text())


connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "CommitText",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    text_committed,
)
context.property_activate("Scheme.Quanpin", IBus.PropState.CHECKED)


def wait_for_commit(expected_text):
    commit_loop = GLib.MainLoop()

    def commit_received():
        if expected_text in committed_text:
            commit_loop.quit()
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    GLib.timeout_add(20, commit_received)
    GLib.timeout_add_seconds(5, commit_loop.quit)
    commit_loop.run()
    if expected_text not in committed_text:
        raise RuntimeError(f"Expected IBus commit {expected_text!r}, received: {committed_text}")


lookup_candidate_updates.clear()
data_directory = Path(os.environ["METASEQUOIA_IME_DATA_DIR"])
with sqlite3.connect(data_directory / "english.db") as database:
    expected_mixed_english = database.execute(
        "SELECT display FROM english_words WHERE word>=? AND word<? "
        "ORDER BY CASE WHEN word=? THEN 0 ELSE 1 END,weight DESC,length(word),word,display LIMIT 1",
        ("ni", "ni{", "ni"),
    ).fetchone()[0]
with sqlite3.connect(data_directory / "others.db") as database:
    expected_mixed_emoji = database.execute(
        "SELECT emoji FROM emoji_pinyin WHERE key>=? AND key<? ORDER BY sort_order LIMIT 1",
        ("ni", "ni\x7f"),
    ).fetchone()[0]
    expected_mixed_kaomoji = database.execute(
        "SELECT kaomoji FROM kaomoji WHERE (pinyin>=? AND pinyin<?) OR (jianpin>=? AND jianpin<?) "
        "ORDER BY sort_order LIMIT 1",
        ("ni", "ni\x7f", "ni", "ni\x7f"),
    ).fetchone()[0]
expected_mixed_prefix = ("你", expected_mixed_english, expected_mixed_emoji, expected_mixed_kaomoji)
for keyval in (IBus.KEY_n, IBus.KEY_i):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Mixed-candidate pinyin input was not handled.")


def mixed_candidates_observed():
    if any(visible and candidates[:4] == expected_mixed_prefix
           for candidates, visible in lookup_candidate_updates):
        mixed_candidates_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


mixed_candidates_loop = GLib.MainLoop()
GLib.timeout_add(20, mixed_candidates_observed)
GLib.timeout_add_seconds(5, mixed_candidates_loop.quit)
mixed_candidates_loop.run()
if not any(visible and candidates[:4] == expected_mixed_prefix
           for candidates, visible in lookup_candidate_updates):
    raise RuntimeError(
        f"Mixed English/Emoji/kaomoji ordering was not published: "
        f"expected={expected_mixed_prefix!r}, updates={lookup_candidate_updates}"
    )
if not context.process_key_event(IBus.KEY_Escape, 0, 0):
    raise RuntimeError("Escape did not cancel the mixed-candidate composition.")


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_U, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+U did not enter Unicode mode.")
for keyval in (IBus.KEY_4, IBus.KEY_e, IBus.KEY_0, IBus.KEY_0):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Unicode hexadecimal input was not handled.")


def unicode_candidate_observed():
    if any(first_candidate == "一" and visible for first_candidate, visible in lookup_updates):
        unicode_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


unicode_loop = GLib.MainLoop()
GLib.timeout_add(20, unicode_candidate_observed)
GLib.timeout_add_seconds(5, unicode_loop.quit)
unicode_loop.run()
if not any(first_candidate == "一" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Unicode mode did not publish its generated candidate: {lookup_updates}")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the Unicode candidate.")
wait_for_commit("一")


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_T, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+T did not enter date/time mode.")
for keyval in (IBus.KEY_r, IBus.KEY_q):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Date keyword input was not handled.")


def date_candidate_observed():
    if any(first_candidate and visible for first_candidate, visible in lookup_updates):
        date_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


date_loop = GLib.MainLoop()
GLib.timeout_add(20, date_candidate_observed)
GLib.timeout_add_seconds(5, date_loop.quit)
date_loop.run()
date_candidates = [first_candidate for first_candidate, visible in lookup_updates if first_candidate and visible]
if not date_candidates:
    raise RuntimeError(f"Date/time mode did not publish a current-date candidate: {lookup_updates}")
expected_date = date_candidates[-1]
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the current-date candidate.")
wait_for_commit(expected_date)


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_K, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+K did not enter quick-phrase mode.")
for keyval in (IBus.KEY_y, IBus.KEY_y, IBus.KEY_d, IBus.KEY_s):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Quick-phrase code input was not handled.")


def quick_phrase_candidate_observed():
    if any(first_candidate == "永远滴神" and visible for first_candidate, visible in lookup_updates):
        quick_phrase_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


quick_phrase_loop = GLib.MainLoop()
GLib.timeout_add(20, quick_phrase_candidate_observed)
GLib.timeout_add_seconds(5, quick_phrase_loop.quit)
quick_phrase_loop.run()
if not any(first_candidate == "永远滴神" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Quick-phrase mode did not publish the shipped yyds candidate: {lookup_updates}")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the quick-phrase candidate.")
wait_for_commit("永远滴神")


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_E, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+E did not enter Emoji mode.")
for keyval in (
    IBus.KEY_x, IBus.KEY_i, IBus.KEY_a, IBus.KEY_o,
    IBus.KEY_l, IBus.KEY_i, IBus.KEY_a, IBus.KEY_n,
):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Emoji full-pinyin input was not handled.")


def emoji_candidate_observed():
    if any(first_candidate == "😀" and visible for first_candidate, visible in lookup_updates):
        emoji_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


emoji_loop = GLib.MainLoop()
GLib.timeout_add(20, emoji_candidate_observed)
GLib.timeout_add_seconds(5, emoji_loop.quit)
emoji_loop.run()
if not any(first_candidate == "😀" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Emoji mode did not publish the xiaolian candidate: {lookup_updates}")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the Emoji candidate.")
wait_for_commit("😀")


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_M, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+M did not enter kaomoji mode.")
for keyval in (
    IBus.KEY_h, IBus.KEY_a, IBus.KEY_i,
    IBus.KEY_x, IBus.KEY_i, IBus.KEY_u,
):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Kaomoji full-pinyin input was not handled.")


def kaomoji_candidate_observed():
    if any(first_candidate == "(*/ω＼*)" and visible for first_candidate, visible in lookup_updates):
        kaomoji_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


kaomoji_loop = GLib.MainLoop()
GLib.timeout_add(20, kaomoji_candidate_observed)
GLib.timeout_add_seconds(5, kaomoji_loop.quit)
kaomoji_loop.run()
if not any(first_candidate == "(*/ω＼*)" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Kaomoji mode did not publish the haixiu candidate: {lookup_updates}")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the kaomoji candidate.")
wait_for_commit("(*/ω＼*)")


lookup_candidate_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_J, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+J did not enter super-jianpin mode.")
for keyval in (IBus.KEY_n, IBus.KEY_h):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Super-jianpin initials were not handled.")


def jianpin_candidates_observed():
    if any(visible and "你好" in candidates
           for candidates, visible in lookup_candidate_updates):
        jianpin_candidate_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


jianpin_candidate_loop = GLib.MainLoop()
GLib.timeout_add(20, jianpin_candidates_observed)
GLib.timeout_add_seconds(5, jianpin_candidate_loop.quit)
jianpin_candidate_loop.run()
jianpin_candidates = next(
    (candidates for candidates, visible in reversed(lookup_candidate_updates)
     if visible and "你好" in candidates),
    None,
)
if jianpin_candidates is None:
    raise RuntimeError(f"Super-jianpin did not publish nh candidates: {lookup_candidate_updates}")
data_directory = Path(os.environ["METASEQUOIA_IME_DATA_DIR"])
canonical_groups = {}
with sqlite3.connect(data_directory / "msime.db") as dictionary_connection:
    for candidate in jianpin_candidates:
        row = dictionary_connection.execute(
            "SELECT key FROM tbl_2_n WHERE jp='nh' AND value=? ORDER BY weight DESC LIMIT 1",
            (candidate,),
        ).fetchone()
        if row is not None:
            canonical_groups.setdefault(row[0], []).append(candidate)
jianpin_group = next((group for group in canonical_groups.values() if len(group) >= 2), None)
if jianpin_group is None:
    raise RuntimeError("Super-jianpin smoke data had no repeated canonical key for frequency learning.")
jianpin_anchor, jianpin_learned = jianpin_group[:2]
jianpin_target = jianpin_candidates.index(jianpin_learned)
for _ in range(jianpin_target // 3):
    if not context.process_key_event(IBus.KEY_Page_Down, 0, 0):
        raise RuntimeError("PageDown did not page through super-jianpin candidates.")
jianpin_digit = (IBus.KEY_1, IBus.KEY_2, IBus.KEY_3)[jianpin_target % 3]
if not context.process_key_event(jianpin_digit, 0, 0):
    raise RuntimeError("A page-relative digit did not select the super-jianpin candidate.")
wait_for_commit(jianpin_learned)

lookup_updates.clear()
lookup_candidate_updates.clear()
if not context.process_key_event(IBus.KEY_J, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Super-jianpin mode did not reopen after frequency learning.")
for keyval in (IBus.KEY_n, IBus.KEY_h):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Reopened super-jianpin initials were not handled.")


def learned_jianpin_order_observed():
    if any(visible and jianpin_learned in candidates and jianpin_anchor in candidates and
           candidates.index(jianpin_learned) < candidates.index(jianpin_anchor)
           for candidates, visible in lookup_candidate_updates):
        learned_jianpin_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


learned_jianpin_loop = GLib.MainLoop()
GLib.timeout_add(20, learned_jianpin_order_observed)
GLib.timeout_add_seconds(5, learned_jianpin_loop.quit)
learned_jianpin_loop.run()
if not any(visible and jianpin_learned in candidates and jianpin_anchor in candidates and
           candidates.index(jianpin_learned) < candidates.index(jianpin_anchor)
           for candidates, visible in lookup_candidate_updates):
    raise RuntimeError("Super-jianpin frequency learning did not persist within its canonical key.")
if not context.process_key_event(IBus.KEY_Escape, 0, 0):
    raise RuntimeError("Escape did not cancel the reopened super-jianpin composition.")


if not context.process_key_event(IBus.KEY_Y, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+Y did not enter temporary English mode.")
if not context.process_key_event(IBus.KEY_BackSpace, 0, 0):
    raise RuntimeError("Backspace did not exit a bare temporary English prefix.")
lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_Y, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary English mode could not be re-entered.")
for keyval in (IBus.KEY_h, IBus.KEY_e):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary English input was not handled.")


def temporary_english_candidate_observed():
    if any(first_candidate == "he" and visible for first_candidate, visible in lookup_updates):
        temporary_english_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


temporary_english_loop = GLib.MainLoop()
GLib.timeout_add(20, temporary_english_candidate_observed)
GLib.timeout_add_seconds(5, temporary_english_loop.quit)
temporary_english_loop.run()
if not any(first_candidate == "he" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError("Temporary English did not publish raw input as its leading candidate.")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select raw temporary English input.")
wait_for_commit("he")

committed_text.clear()
if not context.process_key_event(IBus.KEY_Y, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary English could not start for an Enter commit.")
for keyval in (IBus.KEY_h, IBus.KEY_i):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary English raw Enter input was not handled.")
if not context.process_key_event(IBus.KEY_Return, 0, 0):
    raise RuntimeError("Enter did not commit temporary English without its display prefix.")
wait_for_commit("hi")

committed_text.clear()
if not context.process_key_event(IBus.KEY_Y, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary English could not start for cancellation.")
for keyval in (IBus.KEY_h, IBus.KEY_e):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary English input before cancellation was not handled.")
if not context.process_key_event(IBus.KEY_Escape, 0, 0) or committed_text:
    raise RuntimeError("Escape did not cancel temporary English without committing it.")

if not context.process_key_event(IBus.KEY_R, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Shift+R did not enter temporary Japanese mode.")
if not context.process_key_event(IBus.KEY_BackSpace, 0, 0):
    raise RuntimeError("Backspace did not exit a bare temporary Japanese prefix.")
lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(IBus.KEY_R, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary Japanese mode could not be re-entered.")
for keyval in (IBus.KEY_k, IBus.KEY_a):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary Japanese romaji input was not handled.")


def temporary_japanese_candidate_observed():
    if any(first_candidate == "か" and visible for first_candidate, visible in lookup_updates):
        temporary_japanese_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


temporary_japanese_loop = GLib.MainLoop()
GLib.timeout_add(20, temporary_japanese_candidate_observed)
GLib.timeout_add_seconds(5, temporary_japanese_loop.quit)
temporary_japanese_loop.run()
if not any(first_candidate == "か" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError("Temporary Japanese did not publish the generated ka candidate.")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the temporary Japanese candidate.")
wait_for_commit("か")

lookup_updates.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Chinese input was not restored after temporary Japanese commit.")


def restored_chinese_candidate_observed():
    if any(first_candidate == "你" and visible for first_candidate, visible in lookup_updates):
        restored_chinese_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


restored_chinese_loop = GLib.MainLoop()
GLib.timeout_add(20, restored_chinese_candidate_observed)
GLib.timeout_add_seconds(5, restored_chinese_loop.quit)
restored_chinese_loop.run()
if not any(first_candidate == "你" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError("Temporary Japanese did not return the IBus engine to Quanpin.")
if not context.process_key_event(IBus.KEY_Escape, 0, 0):
    raise RuntimeError("Escape did not cancel the restored Quanpin composition.")

committed_text.clear()
if not context.process_key_event(IBus.KEY_R, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary Japanese could not start before a scheme-menu switch.")
for keyval in (IBus.KEY_k, IBus.KEY_a):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary Japanese input before a scheme-menu switch was not handled.")
context.property_activate("Scheme.Shuangpin", IBus.PropState.CHECKED)
wait_for_commit("か")


def shuangpin_scheme_observed():
    if latest_property("Scheme.Shuangpin")[1] == IBus.PropState.CHECKED:
        shuangpin_scheme_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


shuangpin_scheme_loop = GLib.MainLoop()
GLib.timeout_add(20, shuangpin_scheme_observed)
GLib.timeout_add_seconds(5, shuangpin_scheme_loop.quit)
shuangpin_scheme_loop.run()
if latest_property("Scheme.Shuangpin")[1] != IBus.PropState.CHECKED:
    raise RuntimeError("The scheme menu did not leave temporary Japanese in Shuangpin.")
context.property_activate("Scheme.Quanpin", IBus.PropState.CHECKED)

committed_text.clear()
if not context.process_key_event(IBus.KEY_R, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Temporary Japanese could not start for cancellation.")
for keyval in (IBus.KEY_k, IBus.KEY_a):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Temporary Japanese input before cancellation was not handled.")
if not context.process_key_event(IBus.KEY_Escape, 0, 0) or committed_text:
    raise RuntimeError("Escape did not cancel temporary Japanese without committing it.")


lookup_updates.clear()
committed_text.clear()
if not context.process_key_event(
    IBus.KEY_E,
    0,
    IBus.ModifierType.CONTROL_MASK | IBus.ModifierType.SHIFT_MASK,
):
    raise RuntimeError("Ctrl+Shift+E did not enter dedicated English mode.")
for keyval in (IBus.KEY_h, IBus.KEY_e, IBus.KEY_l, IBus.KEY_l, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("Dedicated English input was not handled.")


def english_candidate_observed():
    if any(first_candidate and first_candidate.lower() == "hello" and visible
           for first_candidate, visible in lookup_updates):
        english_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


english_loop = GLib.MainLoop()
GLib.timeout_add(20, english_candidate_observed)
GLib.timeout_add_seconds(5, english_loop.quit)
english_loop.run()
if not any(first_candidate and first_candidate.lower() == "hello" and visible
           for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Dedicated English mode did not publish the hello candidate: {lookup_updates}")
expected_english = [first_candidate for first_candidate, visible in lookup_updates
                    if first_candidate and first_candidate.lower() == "hello" and visible][-1]
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the dedicated English candidate.")
wait_for_commit(expected_english)


committed_text.clear()
learned_english = "Metasequoialinux"
for character in learned_english:
    keyval = getattr(IBus, "KEY_" + character)
    state = IBus.ModifierType.SHIFT_MASK if character.isupper() else 0
    if not context.process_key_event(keyval, 0, state):
        raise RuntimeError("Raw dedicated English input was not handled.")
if not context.process_key_event(IBus.KEY_Return, 0, 0):
    raise RuntimeError("Enter did not commit raw dedicated English input.")
wait_for_commit(learned_english)
data_directory = Path(os.environ["METASEQUOIA_IME_DATA_DIR"])
with sqlite3.connect(data_directory / "english.db") as english_connection:
    learned_rows = english_connection.execute(
        "SELECT COUNT(*) FROM english_words WHERE word=? AND display=?",
        (learned_english.lower(), learned_english),
    ).fetchone()[0]
with sqlite3.connect(data_directory / "msime_user.db") as user_connection:
    journaled_rows = user_connection.execute(
        "SELECT COUNT(*) FROM user_dictionary_operations "
        "WHERE dictionary='english' AND key=? AND value=? AND operation='upsert'",
        (learned_english.lower(), learned_english),
    ).fetchone()[0]
if learned_rows != 1 or journaled_rows != 1:
    raise RuntimeError(
        f"Raw dedicated English learning was not persisted: db={learned_rows}, journal={journaled_rows}"
    )
if not context.process_key_event(
    IBus.KEY_E,
    0,
    IBus.ModifierType.CONTROL_MASK | IBus.ModifierType.SHIFT_MASK,
):
    raise RuntimeError("Ctrl+Shift+E did not leave dedicated English mode.")


preedit_updates.clear()
lookup_updates.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o, IBus.KEY_F):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The Quanpin helpcode composition was not handled.")


def hidden_helpcode_ui_observed():
    hidden_inline = preedit_updates and all(
        inline_text == "" and not visible for inline_text, visible in preedit_updates
    )
    helpcode_lookup = any(first_candidate == "拟好" and visible for first_candidate, visible in lookup_updates)
    if hidden_inline and helpcode_lookup:
        helpcode_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


helpcode_loop = GLib.MainLoop()
GLib.timeout_add(20, hidden_helpcode_ui_observed)
GLib.timeout_add_seconds(5, helpcode_loop.quit)
helpcode_loop.run()
if not preedit_updates or any(inline_text != "" or visible for inline_text, visible in preedit_updates):
    raise RuntimeError(f"Hidden preedit published visible inline text: {preedit_updates}")
if not any(first_candidate == "拟好" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"Hidden preedit hid the helpcode lookup table or did not reorder candidates: {lookup_updates}")
if not context.process_key_event(IBus.KEY_Escape, 0, 0):
    raise RuntimeError("Escape did not cancel the helpcode smoke composition.")

committed_text.clear()
lookup_candidate_updates.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The frequency-learning composition was not handled.")


def frequency_candidate_observed():
    if any(visible and "拟好" in candidates for candidates, visible in lookup_candidate_updates):
        frequency_candidate_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


frequency_candidate_loop = GLib.MainLoop()
GLib.timeout_add(20, frequency_candidate_observed)
GLib.timeout_add_seconds(5, frequency_candidate_loop.quit)
frequency_candidate_loop.run()
frequency_candidates = next(
    (candidates for candidates, visible in reversed(lookup_candidate_updates)
     if visible and "拟好" in candidates),
    None,
)
if frequency_candidates is None:
    raise RuntimeError(f"The local candidate was absent from the mixed lookup table: {lookup_candidate_updates}")
for _ in range(frequency_candidates.index("拟好")):
    if not context.process_key_event(IBus.KEY_Down, 0, 0):
        raise RuntimeError("Down did not navigate to the local candidate through mixed candidates.")
if not context.process_key_event(IBus.KEY_space, 0, 0):
    raise RuntimeError("Space did not select the local candidate for frequency learning.")
wait_for_commit("拟好")

lookup_updates.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The learned candidate composition was not handled.")


def learned_order_observed():
    if any(first_candidate == "拟好" and visible for first_candidate, visible in lookup_updates):
        learned_order_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


learned_order_loop = GLib.MainLoop()
GLib.timeout_add(20, learned_order_observed)
GLib.timeout_add_seconds(5, learned_order_loop.quit)
learned_order_loop.run()
if not any(first_candidate == "拟好" and visible for first_candidate, visible in lookup_updates):
    raise RuntimeError(f"The persisted frequency adjustment did not change candidate order: {lookup_updates}")
user_database = Path(os.environ["METASEQUOIA_IME_DATA_DIR"]) / "msime_user.db"
with sqlite3.connect(user_database) as user_connection:
    learned_rows = user_connection.execute(
        "SELECT COUNT(*) FROM user_dictionary_operations "
        "WHERE dictionary='pinyin' AND key=? AND value=? AND operation='upsert'",
        ("ni'hao", "拟好"),
    ).fetchone()[0]
if learned_rows != 1:
    raise RuntimeError("The learned frequency was not journaled in XDG user data.")
if not context.process_key_event(IBus.KEY_Escape, 0, 0):
    raise RuntimeError("Escape did not cancel the learned-order smoke composition.")


for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The Quanpin composition was not handled before first-Han selection.")
if not context.process_key_event(IBus.KEY_bracketleft, 0, 0):
    raise RuntimeError("Left bracket was not handled for first-Han selection.")
wait_for_commit("拟")

committed_text.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The Quanpin composition was not handled before last-Han selection.")
if not context.process_key_event(IBus.KEY_bracketright, 0, 0):
    raise RuntimeError("Right bracket was not handled for last-Han selection.")
wait_for_commit("好")

deleted_ranges = []
forwarded_keys = []


def surrounding_deleted(_connection, _sender, _path, _interface, _signal, parameters):
    deleted_ranges.append(
        (
            parameters.get_child_value(0).get_int32(),
            parameters.get_child_value(1).get_uint32(),
        )
    )


def key_forwarded(_connection, _sender, _path, _interface, _signal, parameters):
    forwarded_keys.append(
        (
            parameters.get_child_value(0).get_uint32(),
            parameters.get_child_value(1).get_uint32(),
            parameters.get_child_value(2).get_uint32(),
        )
    )


connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "DeleteSurroundingText",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    surrounding_deleted,
)
connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "ForwardKeyEvent",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    key_forwarded,
)

context.property_activate("Punctuation", IBus.PropState.CHECKED)
context.property_activate("CharacterWidth", IBus.PropState.UNCHECKED)


def smart_punctuation_mode_restored():
    if (
        latest_property("Punctuation") == ("中标", IBus.PropState.CHECKED)
        and latest_property("CharacterWidth") == ("半", IBus.PropState.UNCHECKED)
    ):
        punctuation_mode_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


punctuation_mode_loop = GLib.MainLoop()
GLib.timeout_add(20, smart_punctuation_mode_restored)
GLib.timeout_add_seconds(5, punctuation_mode_loop.quit)
punctuation_mode_loop.run()
if not (
    latest_property("Punctuation") == ("中标", IBus.PropState.CHECKED)
    and latest_property("CharacterWidth") == ("半", IBus.PropState.UNCHECKED)
):
    raise RuntimeError(f"Could not restore smart-punctuation test mode: {property_updates}")

committed_text.clear()
context.set_surrounding_text(IBus.Text.new_from_string("A"), 1, 1)
if not context.process_key_event(IBus.KEY_comma, 0, 0):
    raise RuntimeError("Smart comma was not handled after an ASCII letter.")
wait_for_commit(",")

committed_text.clear()
context.set_surrounding_text(IBus.Text.new_from_string("A,"), 2, 2)
if not context.process_key_event(IBus.KEY_comma, 0, 0):
    raise RuntimeError("Repeated smart comma was not handled.")
wait_for_commit("，")


def replacement_observed():
    if (-1, 1) in deleted_ranges:
        replacement_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


replacement_loop = GLib.MainLoop()
GLib.timeout_add(20, replacement_observed)
GLib.timeout_add_seconds(5, replacement_loop.quit)
replacement_loop.run()
if (-1, 1) not in deleted_ranges:
    raise RuntimeError(f"Repeated smart punctuation did not delete the prior ASCII character: {deleted_ranges}")

committed_text.clear()
context.set_surrounding_text(IBus.Text.new_from_string(""), 0, 0)
if not context.process_key_event(IBus.KEY_parenleft, 0, IBus.ModifierType.SHIFT_MASK):
    raise RuntimeError("Paired opening parenthesis was not handled.")
wait_for_commit("（）")


def cursor_move_observed():
    if any(keyval == IBus.KEY_Left and state == 0 for keyval, _keycode, state in forwarded_keys):
        cursor_loop.quit()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


cursor_loop = GLib.MainLoop()
GLib.timeout_add(20, cursor_move_observed)
GLib.timeout_add_seconds(5, cursor_loop.quit)
cursor_loop.run()
if not any(keyval == IBus.KEY_Left and state == 0 for keyval, _keycode, state in forwarded_keys):
    raise RuntimeError(f"Paired punctuation did not forward a cursor-left event: {forwarded_keys}")

auxiliary_messages = []
warning_loop = GLib.MainLoop()


def auxiliary_updated(_connection, _sender, _path, _interface, _signal, parameters):
    serialized = parameters.get_child_value(0).get_variant()
    message = IBus.Serializable.deserialize_object(serialized).get_text()
    visible = parameters.get_child_value(1).get_boolean()
    auxiliary_messages.append((message, visible))
    if visible:
        warning_loop.quit()


connection.signal_subscribe(
    None,
    "org.freedesktop.IBus.InputContext",
    "UpdateAuxiliaryText",
    context.get_object_path(),
    None,
    Gio.DBusSignalFlags.NONE,
    auxiliary_updated,
)
settings_path.unlink()
settings_path.mkdir()
context.property_activate("Scheme.Wubi", IBus.PropState.CHECKED)
GLib.timeout_add_seconds(5, warning_loop.quit)
warning_loop.run()

expected_warning = "Unable to preserve the existing input settings."
if (expected_warning, True) not in auxiliary_messages:
    raise RuntimeError(f"A settings save failure did not publish the expected warning: {auxiliary_messages}")
if not context.process_key_event(IBus.KEY_n, 0, 0):
    raise RuntimeError("A settings save failure interrupted subsequent input.")
print("Registered IBus properties: " + ", ".join(sorted(expected)))
PYTHON
then
    cat "$smoke_root/ibus-daemon.log" >&2
    exit 1
fi
SESSION

daemon_pid=$(<"$smoke_root/ibus-daemon.pid")
