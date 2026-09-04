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
    "Scheme",
    "Scheme.Quanpin",
    "Scheme.Shuangpin",
    "Scheme.Wubi",
    "Scheme.Japanese",
}
payloads = []
loop = GLib.MainLoop()


def registered_properties(_connection, _sender, _path, _interface, _signal, parameters):
    payloads.append(str(parameters))
    payload = "\n".join(payloads)
    if all(key in payload for key in expected):
        loop.quit()


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
context.set_capabilities(
    IBus.Capabilite.FOCUS
    | IBus.Capabilite.PREEDIT_TEXT
    | IBus.Capabilite.LOOKUP_TABLE
    | IBus.Capabilite.PROPERTY
)
context.focus_in()
context.set_engine("metasequoiaime")


def refocus_when_active():
    active_engine = context.get_engine()
    if active_engine is not None and active_engine.get_name() == "metasequoiaime":
        context.focus_in()
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

context.property_activate("Scheme.Japanese", IBus.PropState.CHECKED)
context.property_activate("InputMode", IBus.PropState.CHECKED)
settings_path = Path(os.environ["XDG_CONFIG_HOME"]) / "metasequoiaime" / "config.ini"


def settings_saved():
    contents = settings_path.read_text(encoding="utf-8")
    if all(
        value in contents
        for value in (
            "mode=ime",
            "scheme=japanese",
            "page-size=3",
            "future-option=preserve-me",
        )
    ):
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
    for value in ("mode=ime", "scheme=japanese", "page-size=3", "future-option=preserve-me")
):
    raise RuntimeError("IBus property changes were not persisted through SettingsStore.")
print("Registered IBus properties: " + ", ".join(sorted(expected)))
PYTHON
then
    cat "$smoke_root/ibus-daemon.log" >&2
    exit 1
fi
SESSION

daemon_pid=$(<"$smoke_root/ibus-daemon.pid")
