#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_root=${METASEQUOIA_IME_BUILD_DIR:-"$project_root/build"}
user_prefix="$HOME/.local"
libexec_dir="$user_prefix/libexec"
bin_dir="$user_prefix/bin"
applications_dir="${XDG_DATA_HOME:-$user_prefix/share}/applications"
component_dir="${XDG_DATA_HOME:-$user_prefix/share}/ibus/component"
data_dir="${XDG_DATA_HOME:-$user_prefix/share}/metasequoiaime"
replay_executable="$build_root/metasequoia-ime-dictionary-replay"
settings_executable="$build_root/metasequoia-ime-settings"
tools_executable="$build_root/metasequoia-ime-tools"
voice_executable="$build_root/metasequoia-ime-voice"
toolbar_executable="$build_root/metasequoia-ime-toolbar"
helpcode_source_dir="$project_root/vendor/MetasequoiaImeHelpCode/helpcodes"
helpcode_files=(
    helpcode.txt
    zrm_helpcode_big_unique.txt
    shouyou2_0_helpcode.txt
    shouyouplus_helpcode.txt
    xiaohe_helpcode.txt
)
staged_main_database=""
staged_others_database=""
staged_english_database=""
backup_main_database=""
backup_others_database=""
backup_english_database=""
database_swap_started=false
database_swap_complete=false
had_live_main_database=false
had_live_others_database=false
had_live_english_database=false
cleanup() {
    if [[ "$database_swap_started" == true && "$database_swap_complete" != true ]]; then
        if [[ "$had_live_main_database" == true && -e "$backup_main_database" ]]; then
            mv -f -- "$backup_main_database" "$data_dir/msime.db" || true
            backup_main_database=""
        else
            rm -f -- "$data_dir/msime.db"
        fi
        if [[ "$had_live_others_database" == true && -e "$backup_others_database" ]]; then
            mv -f -- "$backup_others_database" "$data_dir/others.db" || true
            backup_others_database=""
        else
            rm -f -- "$data_dir/others.db"
        fi
        if [[ "$had_live_english_database" == true && -e "$backup_english_database" ]]; then
            mv -f -- "$backup_english_database" "$data_dir/english.db" || true
            backup_english_database=""
        else
            rm -f -- "$data_dir/english.db"
        fi
    fi
    if [[ -n "$staged_main_database" && -e "$staged_main_database" ]]; then
        rm -f -- "$staged_main_database"
    fi
    if [[ -n "$staged_others_database" && -e "$staged_others_database" ]]; then
        rm -f -- "$staged_others_database"
    fi
    if [[ -n "$staged_english_database" && -e "$staged_english_database" ]]; then
        rm -f -- "$staged_english_database"
    fi
    if [[ -n "$backup_main_database" && -e "$backup_main_database" ]]; then
        rm -f -- "$backup_main_database"
    fi
    if [[ -n "$backup_others_database" && -e "$backup_others_database" ]]; then
        rm -f -- "$backup_others_database"
    fi
    if [[ -n "$backup_english_database" && -e "$backup_english_database" ]]; then
        rm -f -- "$backup_english_database"
    fi
}
trap cleanup EXIT

if ! pkg-config --exists libsecret-1; then
    echo "libsecret-1 is required for secure online-provider credentials." >&2
    exit 1
fi

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
    echo "Metasequoia IME is running. Stop or restart IBus before installing." >&2
    exit 1
fi

cmake -S "$project_root" -B "$build_root" -DCMAKE_INSTALL_PREFIX="$user_prefix"

if [[ ! -x "$build_root/metasequoia-ime-ibus" ]]; then
    echo "Build output is missing. Run scripts/build.sh first." >&2
    exit 1
fi
if [[ ! -x "$replay_executable" ]]; then
    echo "Dictionary replay output is missing. Run scripts/build.sh first." >&2
    exit 1
fi
for utility_executable in "$settings_executable" "$tools_executable" "$voice_executable" "$toolbar_executable"; do
    if [[ ! -x "$utility_executable" ]]; then
        echo "Build output is missing: $utility_executable. Configure with METASEQUOIA_IME_BUILD_SETTINGS_UI=ON." >&2
        exit 1
    fi
done
if [[ ! -f "$project_root/vendor/MetasequoiaImeDict/out/msime.db" ]]; then
    echo "Dictionary is missing. Run scripts/fetch_dictionary.py first." >&2
    exit 1
fi
if [[ ! -f "$project_root/vendor/MetasequoiaImeDict/out/others.db" ]]; then
    echo "Expressive dictionary is missing. Run scripts/fetch_dictionary.py first." >&2
    exit 1
fi
if [[ ! -f "$project_root/vendor/MetasequoiaImeDict/out/english.db" ]]; then
    echo "English dictionary is missing. Run scripts/fetch_dictionary.py first." >&2
    exit 1
fi
for helpcode_file in "${helpcode_files[@]}"; do
    if [[ ! -f "$helpcode_source_dir/$helpcode_file" ]]; then
        echo "Helpcode data is missing. Run git submodule update --init --recursive first." >&2
        exit 1
    fi
done

mkdir -p "$libexec_dir" "$bin_dir" "$component_dir" "$applications_dir" "$data_dir/helpcodes"
install -m 0755 "$build_root/metasequoia-ime-ibus" "$libexec_dir/metasequoia-ime-ibus"
install -m 0755 "$replay_executable" "$libexec_dir/metasequoia-ime-dictionary-replay"
install -m 0755 "$settings_executable" "$bin_dir/metasequoia-ime-settings"
install -m 0755 "$tools_executable" "$bin_dir/metasequoia-ime-tools"
install -m 0755 "$voice_executable" "$bin_dir/metasequoia-ime-voice"
install -m 0755 "$toolbar_executable" "$bin_dir/metasequoia-ime-toolbar"
install -m 0644 "$build_root/metasequoiaime.xml" "$component_dir/metasequoiaime.xml"
install -m 0644 "$build_root/metasequoia-ime-settings.desktop" "$applications_dir/metasequoia-ime-settings.desktop"
install -m 0644 "$build_root/metasequoia-ime-tools.desktop" "$applications_dir/metasequoia-ime-tools.desktop"
install -m 0644 "$build_root/metasequoia-ime-voice.desktop" "$applications_dir/metasequoia-ime-voice.desktop"
install -m 0644 "$build_root/metasequoia-ime-toolbar.desktop" "$applications_dir/metasequoia-ime-toolbar.desktop"
staged_main_database=$(mktemp "$data_dir/.msime.db.install.XXXXXX")
staged_others_database=$(mktemp "$data_dir/.others.db.install.XXXXXX")
staged_english_database=$(mktemp "$data_dir/.english.db.install.XXXXXX")
install -m 0644 "$project_root/vendor/MetasequoiaImeDict/out/msime.db" "$staged_main_database"
install -m 0644 "$project_root/vendor/MetasequoiaImeDict/out/others.db" "$staged_others_database"
install -m 0644 "$project_root/vendor/MetasequoiaImeDict/out/english.db" "$staged_english_database"
for helpcode_file in "${helpcode_files[@]}"; do
    install -m 0644 "$helpcode_source_dir/$helpcode_file" "$data_dir/helpcodes/$helpcode_file"
done
if [[ -f "$data_dir/msime_user.db" ]]; then
    "$replay_executable" --data-dir "$data_dir" --main-db "$staged_main_database" \
        --english-db "$staged_english_database"
fi

if [[ -e "$data_dir/msime.db" ]]; then
    had_live_main_database=true
    backup_main_database=$(mktemp "$data_dir/.msime.db.backup.XXXXXX")
    rm -f -- "$backup_main_database"
    ln "$data_dir/msime.db" "$backup_main_database"
fi
if [[ -e "$data_dir/others.db" ]]; then
    had_live_others_database=true
    backup_others_database=$(mktemp "$data_dir/.others.db.backup.XXXXXX")
    rm -f -- "$backup_others_database"
    ln "$data_dir/others.db" "$backup_others_database"
fi
if [[ -e "$data_dir/english.db" ]]; then
    had_live_english_database=true
    backup_english_database=$(mktemp "$data_dir/.english.db.backup.XXXXXX")
    rm -f -- "$backup_english_database"
    ln "$data_dir/english.db" "$backup_english_database"
fi
database_swap_started=true
mv -f -- "$staged_others_database" "$data_dir/others.db"
staged_others_database=""
mv -f -- "$staged_english_database" "$data_dir/english.db"
staged_english_database=""
mv -f -- "$staged_main_database" "$data_dir/msime.db"
staged_main_database=""
database_swap_complete=true
if [[ -n "$backup_main_database" ]]; then
    rm -f -- "$backup_main_database"
    backup_main_database=""
fi
if [[ -n "$backup_others_database" ]]; then
    rm -f -- "$backup_others_database"
    backup_others_database=""
fi
if [[ -n "$backup_english_database" ]]; then
    rm -f -- "$backup_english_database"
    backup_english_database=""
fi

# IBus only scans its own package data directory and whatever IBUS_COMPONENT_PATH
# names; it does not look under XDG_DATA_HOME. Without this the component installed
# above is never registered, and the engine cannot be selected at all. The variable
# replaces the search path rather than extending it, so the system directory has to
# be listed explicitly alongside the user one.
system_component_dir=$(pkg-config --variable=pkgdatadir ibus-1.0 2>/dev/null || true)
if [[ -z "$system_component_dir" ]]; then
    system_component_dir=/usr/share/ibus
fi
system_component_dir="$system_component_dir/component"
environment_dir="${XDG_CONFIG_HOME:-$HOME/.config}/environment.d"
environment_file="$environment_dir/10-metasequoiaime.conf"
mkdir -p "$environment_dir"
printf '%s\n' \
    '# Written by MSIME-Linux scripts/install.sh. IBus does not scan XDG_DATA_HOME,' \
    '# so the current-user component directory has to be named explicitly.' \
    "IBUS_COMPONENT_PATH=$system_component_dir:$component_dir" \
    >"$environment_file"

echo "Installed Metasequoia IME to $user_prefix"
echo "Wrote $environment_file so IBus can find the current-user component."
echo "Log out and back in for that to take effect, then restart IBus and select"
echo "Metasequoia IME in your desktop input-source settings."
echo "For the current session only, you can instead run:"
echo "  IBUS_COMPONENT_PATH=$system_component_dir:$component_dir ibus-daemon -drx"
