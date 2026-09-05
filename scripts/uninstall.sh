#!/usr/bin/env bash
set -euo pipefail

# Removes everything scripts/install.sh writes for the current user. Learned
# data is kept unless --purge is given, because msime_user.db is the one thing
# here that cannot be recreated from the repository.

user_prefix="$HOME/.local"
libexec_dir="$user_prefix/libexec"
bin_dir="$user_prefix/bin"
applications_dir="${XDG_DATA_HOME:-$user_prefix/share}/applications"
component_dir="${XDG_DATA_HOME:-$user_prefix/share}/ibus/component"
data_dir="${XDG_DATA_HOME:-$user_prefix/share}/metasequoiaime"
environment_file="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d/10-metasequoiaime.conf"
config_file="${XDG_CONFIG_HOME:-$HOME/.config}/metasequoiaime/config.ini"

purge=false
case "${1:-}" in
    "") ;;
    --purge) purge=true ;;
    *)
        echo "Usage: ${BASH_SOURCE[0]} [--purge]" >&2
        echo "  --purge  also remove dictionaries, settings, clipboard history and learned data" >&2
        exit 2
        ;;
esac

# Removing the engine while IBus still has it loaded leaves the daemon holding a
# deleted binary, and the next restart fails in a way that looks unrelated.
engine_is_running() {
    local process_directory
    local process_argv0
    for process_directory in /proc/[0-9]*; do
        [[ -O "$process_directory" && -r "$process_directory/cmdline" ]] || continue
        process_argv0=""
        IFS= read -r -d '' process_argv0 <"$process_directory/cmdline" || true
        if [[ ${process_argv0##*/} == "metasequoia-ime-ibus" ]]; then
            return 0
        fi
    done
    return 1
}

if engine_is_running; then
    echo "Metasequoia IME is running. Stop or restart IBus before uninstalling." >&2
    exit 1
fi

removed=0
remove_path() {
    local path=$1
    if [[ -e "$path" || -L "$path" ]]; then
        rm -rf -- "$path"
        echo "removed $path"
        removed=$((removed + 1))
    fi
}

remove_path "$libexec_dir/metasequoia-ime-ibus"
remove_path "$libexec_dir/metasequoia-ime-dictionary-replay"
for tool in settings tools voice toolbar; do
    remove_path "$bin_dir/metasequoia-ime-$tool"
    # A stale desktop entry keeps the tool in the applications menu long after
    # the binary it points at is gone.
    remove_path "$applications_dir/metasequoia-ime-$tool.desktop"
done
remove_path "$component_dir/metasequoiaime.xml"
remove_path "$environment_file"
remove_path "$data_dir/msime.db"
remove_path "$data_dir/others.db"
remove_path "$data_dir/english.db"
remove_path "$data_dir/helpcodes"
# Interrupted installs can leave these behind, and they are large.
for leftover in "$data_dir"/.msime.db.* "$data_dir"/.others.db.* "$data_dir"/.english.db.* "$data_dir"/*.seeding; do
    [[ -e "$leftover" ]] && remove_path "$leftover"
done

if [[ "$purge" == true ]]; then
    remove_path "$data_dir/msime_user.db"
    remove_path "$data_dir/clipboard_history.json"
    remove_path "$config_file"
    remove_path "$data_dir"
elif [[ -f "$data_dir/msime_user.db" ]]; then
    echo "kept $data_dir/msime_user.db (learned words); pass --purge to remove it"
fi

rmdir "$data_dir" 2>/dev/null || true

echo "Removed $removed item(s) from $user_prefix and the XDG directories."
echo "Restart IBus so it stops offering Metasequoia IME."
