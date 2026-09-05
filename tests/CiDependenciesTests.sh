#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
materialize_root=${1:-}
test_root=$(mktemp -d)
cleanup() {
    rm -rf "$test_root"
}
trap cleanup EXIT

# shellcheck disable=SC2016  # Match the workflow's literal shell variable.
grep -Fq 'run: tests/CiDependenciesTests.sh "$GITHUB_WORKSPACE"' \
    "$project_root/.github/workflows/ci.yml"
if grep -Fq 'GITHUB_TOKEN' \
    "$project_root/.github/workflows/ci.yml" \
    "$project_root/scripts/bootstrap_ci_dependencies.sh"; then
    echo "Dependency bootstrap must not receive the GitHub token." >&2
    exit 1
fi

checkout_root="$test_root/checkout"
git clone --quiet --no-checkout "$project_root" "$checkout_root"
git -C "$checkout_root" checkout --quiet "$(git -C "$project_root" rev-parse HEAD)"

if missing_command_output=$(PATH=/nonexistent /bin/bash \
    "$project_root/scripts/bootstrap_ci_dependencies.sh" "$checkout_root" 2>&1); then
    echo "Dependency bootstrap ignored a missing required command." >&2
    exit 1
fi
grep -Fq 'Required command is unavailable: git' <<<"$missing_command_output"

mkdir -p "$checkout_root/vendor/MetasequoiaImeEngine"
printf '%s\n' 'preserve me' >"$checkout_root/vendor/MetasequoiaImeEngine/local-change"
if "$project_root/scripts/bootstrap_ci_dependencies.sh" "$checkout_root" >/dev/null 2>&1; then
    echo "Dependency bootstrap overwrote a populated submodule directory." >&2
    exit 1
fi
test "$(cat "$checkout_root/vendor/MetasequoiaImeEngine/local-change")" = 'preserve me'

if [[ -n "$materialize_root" ]]; then
    checkout_root=$materialize_root
else
    rm -rf "$checkout_root"
    git clone --quiet --no-checkout "$project_root" "$checkout_root"
    git -C "$checkout_root" checkout --quiet "$(git -C "$project_root" rev-parse HEAD)"
fi

"$project_root/scripts/bootstrap_ci_dependencies.sh" "$checkout_root"

test -f "$checkout_root/vendor/MetasequoiaImeEngine/CMakeLists.txt"
test -f "$checkout_root/vendor/MetasequoiaImeEngine/googlepinyinime-rev/command/pinyinime_dictbuilder.cpp"
test -f "$checkout_root/vendor/MetasequoiaImeEngine/utfcpp/source/utf8.h"
test -f "$checkout_root/vendor/MetasequoiaImeEngine/helpcode/helpcodes/helpcode.txt"

engine_revision=$(git -C "$checkout_root" ls-tree HEAD -- vendor/MetasequoiaImeEngine | awk '{print $3}')
engine_url=$(git -C "$checkout_root" config -f .gitmodules \
    --get 'submodule.vendor/MetasequoiaImeEngine.url')
engine_repository=${engine_url#https://github.com/}
engine_repository=${engine_repository%.git}
if [[ "$engine_repository" == "$engine_url" || "$engine_repository" != */* ]]; then
    echo "Unsupported Engine submodule URL: $engine_url" >&2
    exit 1
fi
curl --fail --location --silent --show-error \
    --retry 3 --retry-all-errors \
    "https://raw.githubusercontent.com/$engine_repository/$engine_revision/core/input_session.cpp" \
    | cmp - "$checkout_root/vendor/MetasequoiaImeEngine/core/input_session.cpp"

engine_tree="$test_root/engine-tree.json"
curl --fail --location --silent --show-error \
    --retry 3 --retry-all-errors \
    "https://api.github.com/repos/$engine_repository/git/trees/$engine_revision?recursive=1" \
    --output "$engine_tree"
googlepinyin_revision=$(python3 - "$engine_tree" <<'PYTHON'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as tree_file:
    tree = json.load(tree_file)

for entry in tree.get("tree", []):
    if entry.get("path") == "googlepinyinime-rev" and entry.get("mode") == "160000":
        print(entry["sha"])
        break
else:
    raise SystemExit("Unable to resolve googlepinyinime-rev revision")
PYTHON
)
curl --fail --location --silent --show-error \
    --retry 3 --retry-all-errors \
    "https://raw.githubusercontent.com/metasequoiaime/googlepinyinime-rev/$googlepinyin_revision/src/share/pinyinime.cpp" \
    | cmp - "$checkout_root/vendor/MetasequoiaImeEngine/googlepinyinime-rev/src/share/pinyinime.cpp"
