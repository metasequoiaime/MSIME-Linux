#!/usr/bin/env bash
set -euo pipefail

# Pins where scripts/install.sh writes the dictionaries and where scripts/uninstall.sh looks for them. Both scripts source scripts/lib/data_dir.sh, and this exercises that resolver directly rather than by running an install: the smoke tests that drive those scripts copy roughly 185 MB of dictionaries per run, and a second install under METASEQUOIA_IME_DATA_DIR would about double that for one path assertion.
#
# The order mirrors metasequoia::data_directory() in the engine, and the whole point of the resolver is that the two answers cannot drift, so all four branches are pinned: the absolute override, XDG_DATA_HOME, HOME, and the rejection of a relative override.

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
# shellcheck source=scripts/lib/data_dir.sh
source "$project_root/scripts/lib/data_dir.sh"

failures=0

# Runs the resolver with exactly the three variables set to the given values, so a value inherited from the caller's environment cannot decide a branch. An empty argument means the variable is unset rather than empty, which is the case a script run from a login shell with no HOME actually produces.
resolve_with() {
    local override=$1 xdg=$2 home=$3
    (
        if [[ -n "$override" ]]; then export METASEQUOIA_IME_DATA_DIR="$override"; else unset METASEQUOIA_IME_DATA_DIR; fi
        if [[ -n "$xdg" ]]; then export XDG_DATA_HOME="$xdg"; else unset XDG_DATA_HOME; fi
        if [[ -n "$home" ]]; then export HOME="$home"; else unset HOME; fi
        resolve_data_dir
    )
}

expect_resolves() {
    local description=$1 override=$2 xdg=$3 home=$4 expected=$5
    local actual
    if ! actual=$(resolve_with "$override" "$xdg" "$home"); then
        echo "$description: the resolver failed where it should have answered $expected" >&2
        failures=$((failures + 1))
        return
    fi
    if [[ "$actual" != "$expected" ]]; then
        echo "$description: resolved to $actual, expected $expected" >&2
        failures=$((failures + 1))
    fi
}

expect_fails() {
    local description=$1 override=$2 xdg=$3 home=$4
    local actual
    if actual=$(resolve_with "$override" "$xdg" "$home"); then
        echo "$description: the resolver answered $actual where it should have failed" >&2
        failures=$((failures + 1))
    fi
}

expect_resolves "an absolute override is used verbatim" \
    /srv/metasequoia /var/lib/xdg /var/lib/home /srv/metasequoia
# An override names the directory itself. Appending the project name to it would install into a directory the engine never opens, which is the whole defect the shared resolver exists to prevent.
expect_resolves "an absolute override outranks XDG_DATA_HOME and HOME" \
    /srv/other-name /var/lib/xdg /var/lib/home /srv/other-name
expect_resolves "XDG_DATA_HOME is used when there is no override" \
    "" /var/lib/xdg /var/lib/home /var/lib/xdg/metasequoiaime
expect_resolves "HOME is the last resort" \
    "" "" /var/lib/home /var/lib/home/.local/share/metasequoiaime
# The engine ignores a relative override rather than resolving it against the current directory, so the scripts have to skip that branch too instead of installing into ./metasequoiaime under whatever directory the user happened to run them from.
expect_resolves "a relative override is ignored, not resolved" \
    relative/data /var/lib/xdg /var/lib/home /var/lib/xdg/metasequoiaime
expect_resolves "a relative XDG_DATA_HOME is ignored too" \
    "" relative/xdg /var/lib/home /var/lib/home/.local/share/metasequoiaime
expect_fails "nothing absolute leaves no directory to answer with" \
    relative/data relative/xdg relative/home

# The resolver is only worth pinning if it is the one the scripts actually run.
for script in install.sh uninstall.sh; do
    if ! grep -q 'scripts/lib/data_dir.sh' "$project_root/scripts/$script"; then
        echo "scripts/$script no longer sources scripts/lib/data_dir.sh" >&2
        failures=$((failures + 1))
    fi
done

if [[ "$failures" -ne 0 ]]; then
    echo "$failures data directory resolution check(s) failed." >&2
    exit 1
fi
