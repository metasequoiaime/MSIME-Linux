#!/usr/bin/env bash
set -euo pipefail

# Installs into a throwaway HOME, uninstalls, and checks that nothing the
# installer wrote is left behind. The README used to list the removal steps by
# hand and missed the executables in ~/.local/bin and their desktop entries, so
# the point of this test is that the two scripts stay symmetric.

build_dir=${1:-build}
build_dir=$(cd "$build_dir" && pwd)
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
smoke_root=$(mktemp -d)
cleanup() {
    rm -rf "$smoke_root"
}
trap cleanup EXIT

export HOME="$smoke_root/home"
export XDG_DATA_HOME="$smoke_root/data"
export XDG_CONFIG_HOME="$smoke_root/config"
export METASEQUOIA_IME_BUILD_DIR="$build_dir"
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

"$project_root/scripts/install.sh" >/dev/null

installed=(
    "$HOME/.local/libexec/metasequoia-ime-ibus"
    "$HOME/.local/libexec/metasequoia-ime-dictionary-replay"
    "$HOME/.local/bin/metasequoia-ime-settings"
    "$HOME/.local/bin/metasequoia-ime-tools"
    "$HOME/.local/bin/metasequoia-ime-voice"
    "$HOME/.local/bin/metasequoia-ime-toolbar"
    "$XDG_DATA_HOME/applications/metasequoia-ime-settings.desktop"
    "$XDG_DATA_HOME/applications/metasequoia-ime-tools.desktop"
    "$XDG_DATA_HOME/applications/metasequoia-ime-voice.desktop"
    "$XDG_DATA_HOME/applications/metasequoia-ime-toolbar.desktop"
    "$XDG_DATA_HOME/ibus/component/metasequoiaime.xml"
    "$XDG_DATA_HOME/metasequoiaime/msime.db"
    "$XDG_DATA_HOME/metasequoiaime/others.db"
    "$XDG_DATA_HOME/metasequoiaime/english.db"
    "$XDG_CONFIG_HOME/environment.d/10-metasequoiaime.conf"
)
for path in "${installed[@]}"; do
    if [[ ! -e "$path" ]]; then
        echo "The installer did not create $path; this test is out of date." >&2
        exit 1
    fi
done

# Learned data must survive a plain uninstall.
printf 'learned\n' >"$XDG_DATA_HOME/metasequoiaime/msime_user.db"

"$project_root/scripts/uninstall.sh" >/dev/null

for path in "${installed[@]}"; do
    if [[ -e "$path" ]]; then
        echo "Uninstall left $path behind." >&2
        exit 1
    fi
done
if [[ ! -f "$XDG_DATA_HOME/metasequoiaime/msime_user.db" ]]; then
    echo "Uninstall removed learned data without --purge." >&2
    exit 1
fi

"$project_root/scripts/uninstall.sh" --purge >/dev/null
if [[ -e "$XDG_DATA_HOME/metasequoiaime/msime_user.db" ]]; then
    echo "--purge kept learned data." >&2
    exit 1
fi
if [[ -e "$XDG_DATA_HOME/metasequoiaime" ]]; then
    echo "--purge left the data directory behind." >&2
    exit 1
fi

# Uninstalling a tree that was never installed must be a no-op, not an error.
"$project_root/scripts/uninstall.sh" >/dev/null
