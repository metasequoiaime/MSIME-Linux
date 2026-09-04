#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
user_prefix="${HOME}/.local"
python3 "$project_root/scripts/build_dictionary.py"
cmake -S "$project_root" -B "$project_root/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$user_prefix"
cmake --build "$project_root/build" --parallel
ctest --test-dir "$project_root/build" --output-on-failure --timeout 20
