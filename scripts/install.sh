#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${METASEQUOIA_IME_BUILD_DIR:-"$project_root/build"}
user_prefix="$HOME/.local"
libexec_dir="$user_prefix/libexec"
component_dir="${XDG_DATA_HOME:-$user_prefix/share}/ibus/component"
data_dir="${XDG_DATA_HOME:-$user_prefix/share}/metasequoiaime"
helpcode_source_dir="$project_root/vendor/MetasequoiaImeHelpCode/helpcodes"
helpcode_files=(
    helpcode.txt
    zrm_helpcode_big_unique.txt
    shouyou2_0_helpcode.txt
    shouyouplus_helpcode.txt
    xiaohe_helpcode.txt
)

cmake -S "$project_root" -B "$build_root" -DCMAKE_INSTALL_PREFIX="$user_prefix"

if [[ ! -x "$build_root/metasequoia-ime-ibus" ]]; then
    echo "Build output is missing. Run scripts/build.sh first." >&2
    exit 1
fi
if [[ ! -f "$project_root/vendor/MetasequoiaImeDict/out/msime.db" ]]; then
    echo "Dictionary is missing. Run scripts/build_dictionary.py first." >&2
    exit 1
fi
for helpcode_file in "${helpcode_files[@]}"; do
    if [[ ! -f "$helpcode_source_dir/$helpcode_file" ]]; then
        echo "Helpcode data is missing. Run git submodule update --init --recursive first." >&2
        exit 1
    fi
done

mkdir -p "$libexec_dir" "$component_dir" "$data_dir/helpcodes"
install -m 0755 "$build_root/metasequoia-ime-ibus" "$libexec_dir/metasequoia-ime-ibus"
install -m 0644 "$build_root/metasequoiaime.xml" "$component_dir/metasequoiaime.xml"
install -m 0644 "$project_root/vendor/MetasequoiaImeDict/out/msime.db" "$data_dir/msime.db"
for helpcode_file in "${helpcode_files[@]}"; do
    install -m 0644 "$helpcode_source_dir/$helpcode_file" "$data_dir/helpcodes/$helpcode_file"
done

echo "Installed Metasequoia IME to $user_prefix"
echo "Restart IBus and select Metasequoia IME in your desktop input-source settings."
