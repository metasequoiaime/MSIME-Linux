#!/usr/bin/env bash
set -euo pipefail

test_executable=$1
fixture_root=$(mktemp -d)
cleanup() {
    rm -rf "$fixture_root"
}
trap cleanup EXIT

export HOME="$fixture_root/home"
export XDG_CONFIG_HOME="$fixture_root/config"
export XDG_DATA_HOME="$fixture_root/data"
export XDG_RUNTIME_DIR="$fixture_root/run"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

dbus-run-session -- bash -c '
set -euo pipefail
test_executable=$1
printf "\n" | gnome-keyring-daemon --unlock --components=secrets >/dev/null
exec "$test_executable" --integration
' bash "$test_executable"
