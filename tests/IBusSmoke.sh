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
export IBUS_COMPONENT_PATH="$smoke_root/components"
export METASEQUOIA_IME_DATA_DIR=${METASEQUOIA_IME_DATA_DIR:-"$project_root/vendor/MetasequoiaImeDict/out"}

mkdir -p "$HOME" "$XDG_CACHE_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR" "$IBUS_COMPONENT_PATH"
chmod 700 "$XDG_RUNTIME_DIR"
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
    'word-to-character=true' \
    'bracket-paging=false' \
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
context.set_capabilities(
    IBus.Capabilite.FOCUS
    | IBus.Capabilite.PREEDIT_TEXT
    | IBus.Capabilite.AUXILIARY_TEXT
    | IBus.Capabilite.LOOKUP_TABLE
    | IBus.Capabilite.PROPERTY
)
context.focus_in()
context.set_engine("metasequoiaime")


def refocus_when_active():
    active_engine = context.get_engine()
    if active_engine is not None and active_engine.get_name() == "metasequoiaime":
        context.focus_in()
        return GLib.SOURCE_REMOVE
    return GLib.SOURCE_CONTINUE


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


for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The Quanpin composition was not handled before first-Han selection.")
if not context.process_key_event(IBus.KEY_bracketleft, 0, 0):
    raise RuntimeError("Left bracket was not handled for first-Han selection.")
wait_for_commit("你")

committed_text.clear()
for keyval in (IBus.KEY_n, IBus.KEY_i, IBus.KEY_h, IBus.KEY_a, IBus.KEY_o):
    if not context.process_key_event(keyval, 0, 0):
        raise RuntimeError("The Quanpin composition was not handled before last-Han selection.")
if not context.process_key_event(IBus.KEY_bracketright, 0, 0):
    raise RuntimeError("Right bracket was not handled for last-Han selection.")
wait_for_commit("好")

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
