#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root="$project_root/build"
user_prefix="$HOME/.local"
libexec_dir="$user_prefix/libexec"
component_dir="${XDG_DATA_HOME:-$user_prefix/share}/ibus/component"
data_dir="${XDG_DATA_HOME:-$user_prefix/share}/metasequoiaime"

if [[ ! -x "$build_root/metasequoia-ime-ibus" ]]; then
    echo "Build output is missing. Run scripts/build.sh first." >&2
    exit 1
fi
if [[ ! -f "$project_root/vendor/MetasequoiaImeDict/out/msime.db" ]]; then
    echo "Dictionary is missing. Run scripts/build_dictionary.py first." >&2
    exit 1
fi

mkdir -p "$libexec_dir" "$component_dir" "$data_dir"
install -m 0755 "$build_root/metasequoia-ime-ibus" "$libexec_dir/metasequoia-ime-ibus"
install -m 0644 "$build_root/metasequoiaime.xml" "$component_dir/metasequoiaime.xml"
install -m 0644 "$project_root/vendor/MetasequoiaImeDict/out/msime.db" "$data_dir/msime.db"

echo "Installed Metasequoia IME to $user_prefix"
echo "Restart IBus and select Metasequoia IME in your desktop input-source settings."
