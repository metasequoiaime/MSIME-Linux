#!/usr/bin/env bash
set -euo pipefail

# GitHub blocks git transport for these forks while their parent repositories are
# disabled. The archive and Git tree APIs remain readable and still let CI honor
# every pinned gitlink revision exactly.
project_root=${1:-"$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"}
for required_command in git curl tar python3 awk find mktemp; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "Required command is unavailable: $required_command" >&2
        exit 1
    fi
done

temporary_root=$(mktemp -d)
github_api_headers=(
    --header 'Accept: application/vnd.github+json'
    --header 'X-GitHub-Api-Version: 2022-11-28'
)
cleanup() {
    rm -rf "$temporary_root"
}
trap cleanup EXIT

github_repository() {
    local url=$1
    local repository=${url#https://github.com/}
    repository=${repository%.git}
    if [[ "$repository" == "$url" || "$repository" != */* ]]; then
        echo "Unsupported GitHub submodule URL: $url" >&2
        return 1
    fi
    printf '%s\n' "$repository"
}

download_archive() {
    local repository=$1
    local revision=$2
    local destination=$3
    local archive="$temporary_root/${repository//\//-}-$revision.tar.gz"

    if [[ -e "$destination" && ! -d "$destination" ]]; then
        echo "Refusing to replace non-directory dependency path: $destination" >&2
        return 1
    fi
    if [[ -d "$destination" ]] && \
        [[ -n "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
        echo "Refusing to overwrite populated dependency directory: $destination" >&2
        return 1
    fi

    curl --fail --location --silent --show-error \
        --retry 3 --retry-all-errors \
        "https://codeload.github.com/$repository/tar.gz/$revision" \
        --output "$archive"
    mkdir -p "$destination"
    tar -xzf "$archive" --strip-components=1 -C "$destination"
}

materialize_nested_submodules() {
    local repository=$1
    local revision=$2
    local checkout_root=$3
    local modules_file="$checkout_root/.gitmodules"
    local tree_file="$temporary_root/${repository//\//-}-$revision-tree.json"

    if [[ ! -f "$modules_file" ]]; then
        return
    fi

    curl --fail --location --silent --show-error \
        --retry 3 --retry-all-errors \
        "${github_api_headers[@]}" \
        "https://api.github.com/repos/$repository/git/trees/$revision?recursive=1" \
        --output "$tree_file"

    while read -r path_key nested_path; do
        local nested_name=${path_key#submodule.}
        nested_name=${nested_name%.path}
        local nested_url
        nested_url=$(git config -f "$modules_file" --get "submodule.$nested_name.url")
        local nested_repository
        nested_repository=$(github_repository "$nested_url")
        local nested_revision
        nested_revision=$(python3 - "$tree_file" "$nested_path" <<'PYTHON'
import json
import sys

tree_path, submodule_path = sys.argv[1:]
with open(tree_path, encoding="utf-8") as tree_file:
    tree = json.load(tree_file)

for entry in tree.get("tree", []):
    if entry.get("path") == submodule_path and entry.get("mode") == "160000":
        print(entry["sha"])
        break
else:
    raise SystemExit(f"Unable to resolve nested submodule revision: {submodule_path}")
PYTHON
        )
        download_archive "$nested_repository" "$nested_revision" "$checkout_root/$nested_path"
        materialize_nested_submodules \
            "$nested_repository" \
            "$nested_revision" \
            "$checkout_root/$nested_path"
    done < <(git config -f "$modules_file" --get-regexp '^submodule\..*\.path$')
}

while read -r path_key submodule_path; do
    submodule_name=${path_key#submodule.}
    submodule_name=${submodule_name%.path}
    submodule_url=$(git -C "$project_root" config -f .gitmodules --get "submodule.$submodule_name.url")
    submodule_repository=$(github_repository "$submodule_url")
    submodule_revision=$(git -C "$project_root" ls-tree HEAD -- "$submodule_path" | awk '{print $3}')
    if [[ ! "$submodule_revision" =~ ^[0-9a-f]{40}$ ]]; then
        echo "Unable to resolve submodule revision: $submodule_path" >&2
        exit 1
    fi

    download_archive "$submodule_repository" "$submodule_revision" "$project_root/$submodule_path"
    materialize_nested_submodules \
        "$submodule_repository" \
        "$submodule_revision" \
        "$project_root/$submodule_path"
done < <(git -C "$project_root" config -f .gitmodules --get-regexp '^submodule\..*\.path$')
