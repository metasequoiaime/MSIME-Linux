#!/usr/bin/env bash
set -euo pipefail

# Formatting has to be reproducible across contributor machines and CI, so the
# formatter version is pinned here and installed from PyPI instead of being
# taken from whatever the host toolchain happens to ship. Apple, Debian and
# LLVM upstream all disagree about clang-format defaults between releases.
clang_format_version=18.1.8

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
venv_root=${METASEQUOIA_CLANG_FORMAT_VENV:-"${TMPDIR:-/tmp}/metasequoia-clang-format-$clang_format_version"}
clang_format="$venv_root/bin/clang-format"

if [[ ! -x "$clang_format" ]]; then
    python3 -m venv "$venv_root"
    "$venv_root/bin/pip" install --quiet "clang-format==$clang_format_version"
fi

mode=${1:---write}
case "$mode" in
    --check) clang_format_arguments=(--dry-run --Werror) ;;
    --write) clang_format_arguments=(-i) ;;
    *)
        echo "Usage: ${BASH_SOURCE[0]} [--check|--write]" >&2
        exit 2
        ;;
esac

cd "$project_root"
git ls-files 'src/*.cpp' 'src/*.h' 'tests/*.cpp' \
    | xargs "$clang_format" "${clang_format_arguments[@]}" --style=file
