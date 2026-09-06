# shellcheck shell=bash
# Where the installed dictionaries go, and where an uninstall looks for them. The engine is the authority on this, and metasequoia::data_directory() (vendor/MetasequoiaImeEngine/core/data_path.h) answers with METASEQUOIA_IME_DATA_DIR when that is absolute, then XDG_DATA_HOME/metasequoiaime when that is, and only then $HOME/.local/share/metasequoiaime. This mirrors that order instead of deriving a second answer: a user with the override set otherwise installs the dictionaries into one directory while the engine opens another, and sees an install that succeeded and an input method with no candidates. Relative values are ignored exactly as the engine ignores them, rather than being resolved against whatever directory the script was run from.
#
# Sourced by both scripts/install.sh and scripts/uninstall.sh rather than copied into each. The two were kept identical by hand before, and an uninstall that resolved this differently would sweep a directory the install never wrote and leave the real one, including the 67 MB Japanese model, in place. tests/DataDirResolution.sh pins the four branches.
resolve_data_dir() {
    if [[ ${METASEQUOIA_IME_DATA_DIR:-} == /* ]]; then
        printf '%s\n' "$METASEQUOIA_IME_DATA_DIR"
    elif [[ ${XDG_DATA_HOME:-} == /* ]]; then
        printf '%s\n' "$XDG_DATA_HOME/metasequoiaime"
    elif [[ ${HOME:-} == /* ]]; then
        printf '%s\n' "$HOME/.local/share/metasequoiaime"
    else
        return 1
    fi
}
