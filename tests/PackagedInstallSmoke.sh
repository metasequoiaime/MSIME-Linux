#!/usr/bin/env bash
set -euo pipefail

# Installs the built package and checks that the input method actually works
# from it. Everything the packaging fixes in #16, #18 and #27 addressed was a
# defect that only appeared after a package install: the component naming a
# path the package did not ship, two of three dictionaries missing, and the
# engine looking somewhere the package never wrote. Each of those produced a
# package that installed cleanly and then did nothing, and the checks that catch
# them today inspect the archive rather than running what is inside it.
#
# Needs root because it installs into /usr, so this runs from CI rather than
# from ctest.

build_dir=${1:-build}
build_dir=$(cd "$build_dir" && pwd)
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

if [[ $(id -u) -eq 0 ]]; then
    sudo=""
elif command -v sudo >/dev/null 2>&1; then
    sudo="sudo"
else
    echo "This smoke test installs a package and needs root." >&2
    exit 1
fi

smoke_root=$(mktemp -d)
installed=false
cleanup() {
    if [[ "$installed" == true ]]; then
        $sudo dpkg --purge metasequoia-ime-linux >/dev/null 2>&1 || true
    fi
    rm -rf "$smoke_root"
}
trap cleanup EXIT

# Package with the prefix the packages install into, because the current-user
# install smoke reconfigures this build directory with its own.
cmake -S "$project_root" -B "$build_dir" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=/usr >/dev/null
rm -rf "$build_dir/packagedsmoke"
cpack --config "$build_dir/CPackConfig.cmake" -G DEB -B "$build_dir/packagedsmoke" >/dev/null

shopt -s nullglob
packages=("$build_dir"/packagedsmoke/*.deb)
if [[ ${#packages[@]} -ne 1 ]]; then
    echo "Expected exactly one package, found: ${packages[*]}" >&2
    exit 1
fi

$sudo dpkg -i "${packages[0]}" >/dev/null
installed=true

# Nothing here may fall back to a source tree: the point is to exercise what a
# user who only installed the package would get.
test -x /usr/libexec/metasequoia-ime-ibus
test -f /usr/share/ibus/component/metasequoiaime.xml
for database in msime.db others.db english.db; do
    if [[ ! -s "/usr/share/metasequoiaime/$database" ]]; then
        echo "The package did not install $database." >&2
        exit 1
    fi
done

export HOME="$smoke_root/home"
export XDG_DATA_HOME="$smoke_root/data"
export XDG_CONFIG_HOME="$smoke_root/config"
export XDG_RUNTIME_DIR="$smoke_root/run"
export DISPLAY=:99
export IBUS_USE_PORTAL=0
export GIO_USE_VFS=local
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

cat > "$smoke_root/probe.py" <<'PYTHON'
import os
import pathlib
import sys

import gi

gi.require_version("IBus", "1.0")
from gi.repository import GLib, IBus

IBus.init()
bus = IBus.Bus()
if not bus.is_connected():
    raise SystemExit("The packaged smoke client could not connect to IBus.")

context = bus.create_input_context("metasequoia-packaged-smoke")
context.set_capabilities(
    IBus.Capabilite.FOCUS
    | IBus.Capabilite.PREEDIT_TEXT
    | IBus.Capabilite.LOOKUP_TABLE
    | IBus.Capabilite.PROPERTY
)
committed = []
context.connect("commit-text", lambda c, text: committed.append(text.get_text()))
context.focus_in()


def engine_is_active():
    engine = context.get_engine()
    return engine is not None and engine.get_name() == "metasequoiaime"


context.set_engine("metasequoiaime")


def activate_globally_if_needed():
    if not engine_is_active():
        bus.set_global_engine("metasequoiaime")
    return GLib.SOURCE_REMOVE


def settle(milliseconds):
    loop = GLib.MainLoop()
    GLib.timeout_add(milliseconds, lambda: (loop.quit(), False)[1])
    loop.run()


GLib.timeout_add(500, activate_globally_if_needed)
settle(4000)
if not engine_is_active():
    raise SystemExit("The packaged engine never became the active IBus engine.")
context.focus_in()
settle(500)

for character in "nihao":
    context.process_key_event(ord(character), 0, 0)
    settle(250)
context.process_key_event(0x20, 0, 0)
settle(1500)

if committed != ["你好"]:
    raise SystemExit(f"The packaged engine did not commit the expected text: {committed}")

# The dictionaries ship under /usr, where the engine does not look, so the
# engine seeds the per-user directory on first run. If that stopped working the
# commit above would already have failed, but name the file so a future failure
# points at the cause rather than at pinyin conversion.
seeded = pathlib.Path(os.environ["XDG_DATA_HOME"]) / "metasequoiaime" / "msime.db"
if not seeded.is_file() or seeded.stat().st_size == 0:
    raise SystemExit(f"The packaged engine did not seed {seeded}.")

print("Packaged install smoke passed.")
PYTHON

dbus-run-session -- bash -s -- "$smoke_root" <<'SESSION'
set -euo pipefail
smoke_root=$1

ibus write-cache >/dev/null 2>&1
ibus-daemon --single --panel disable --config disable --cache auto >"$smoke_root/ibus-daemon.log" 2>&1 &
daemon_pid=$!
for _ in {1..100}; do
    if ibus list-engine 2>/dev/null | grep -q metasequoiaime; then
        break
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
        cat "$smoke_root/ibus-daemon.log" >&2
        exit 1
    fi
    sleep 0.1
done
if ! ibus list-engine 2>/dev/null | grep -q metasequoiaime; then
    echo "The packaged component never registered with IBus." >&2
    cat "$smoke_root/ibus-daemon.log" >&2
    exit 1
fi

python3 "$smoke_root/probe.py"
kill "$daemon_pid" 2>/dev/null || true
SESSION
