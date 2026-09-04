#!/usr/bin/env bash
set -euo pipefail

build_dir=${1:-build}
build_dir=$(cd "$build_dir" && pwd)
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
smoke_root=$(mktemp -d)
engine_pid=""
cleanup() {
    if [[ -n "$engine_pid" ]] && kill -0 "$engine_pid" 2>/dev/null; then
        kill "$engine_pid"
        wait "$engine_pid" 2>/dev/null || true
    fi
    rm -rf "$smoke_root"
}
trap cleanup EXIT

export HOME="$smoke_root/home"
export XDG_DATA_HOME="$smoke_root/data"
export METASEQUOIA_IME_BUILD_DIR="$build_dir"

"$project_root/scripts/install.sh"

data_dir="$XDG_DATA_HOME/metasequoiaime"
test -x "$HOME/.local/libexec/metasequoia-ime-ibus"
test -x "$HOME/.local/libexec/metasequoia-ime-dictionary-replay"
test -f "$XDG_DATA_HOME/ibus/component/metasequoiaime.xml"
test -f "$data_dir/msime.db"
test -f "$data_dir/helpcodes/helpcode.txt"
test -f "$data_dir/helpcodes/zrm_helpcode_big_unique.txt"
test -f "$data_dir/helpcodes/shouyou2_0_helpcode.txt"
test -f "$data_dir/helpcodes/shouyouplus_helpcode.txt"
test -f "$data_dir/helpcodes/xiaohe_helpcode.txt"
grep -F "<exec>$HOME/.local/libexec/metasequoia-ime-ibus --ibus</exec>" \
    "$XDG_DATA_HOME/ibus/component/metasequoiaime.xml"

python3 - "$data_dir" <<'PYTHON'
import sqlite3
import sys
from pathlib import Path

data_dir = Path(sys.argv[1])
main_database = sqlite3.connect(data_dir / "msime.db")
original_weight = main_database.execute(
    "SELECT weight FROM tbl_1_n WHERE key='ni' AND value='你'"
).fetchone()[0]
learned_weight = original_weight + 12345
main_database.execute(
    "UPDATE tbl_1_n SET weight=? WHERE key='ni' AND value='你'",
    (learned_weight,),
)
main_database.commit()
main_database.close()

user_database = sqlite3.connect(data_dir / "msime_user.db")
user_database.execute(
    """
    CREATE TABLE user_dictionary_operations(
        dictionary TEXT NOT NULL,
        key TEXT NOT NULL,
        value TEXT NOT NULL,
        operation TEXT NOT NULL,
        weight INTEGER NOT NULL,
        display TEXT NOT NULL DEFAULT '',
        updated_at INTEGER NOT NULL DEFAULT(unixepoch()),
        PRIMARY KEY(dictionary,key,value)
    )
    """
)
user_database.execute(
    "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight) "
    "VALUES('pinyin','ni','你','upsert',?)",
    (learned_weight,),
)
user_database.commit()
user_database.close()
PYTHON

"$project_root/scripts/install.sh"

python3 - "$data_dir" <<'PYTHON'
import sqlite3
import sys
from pathlib import Path

data_dir = Path(sys.argv[1])
with sqlite3.connect(data_dir / "msime_user.db") as user_database:
    expected_weight = user_database.execute(
        "SELECT weight FROM user_dictionary_operations "
        "WHERE dictionary='pinyin' AND key='ni' AND value='你'"
    ).fetchone()[0]
with sqlite3.connect(data_dir / "msime.db") as database:
    actual_weight = database.execute(
        "SELECT weight FROM tbl_1_n WHERE key='ni' AND value='你'"
    ).fetchone()[0]
if actual_weight != expected_weight:
    raise SystemExit(
        f"journal replay lost learned weight: expected {expected_weight}, got {actual_weight}"
    )
PYTHON

bash -c 'exec -a metasequoia-ime-ibus sleep 30' &
engine_pid=$!
process_deadline=$((SECONDS + 5))
while [[ $(ps -p "$engine_pid" -o args= 2>/dev/null) != metasequoia-ime-ibus* ]]; do
    if ((SECONDS >= process_deadline)); then
        echo "Timed out waiting for the simulated running engine." >&2
        exit 1
    fi
    sleep 0.05
done
if "$project_root/scripts/install.sh"; then
    echo "Install unexpectedly succeeded while the IBus engine was running." >&2
    exit 1
fi
kill "$engine_pid"
wait "$engine_pid" 2>/dev/null || true
engine_pid=""

learned_weight_before_failure=$(python3 - "$data_dir" <<'PYTHON'
import sqlite3
import sys
from pathlib import Path

data_dir = Path(sys.argv[1])
with sqlite3.connect(data_dir / "msime.db") as database:
    print(database.execute(
        "SELECT weight FROM tbl_1_n WHERE key='ni' AND value='你'"
    ).fetchone()[0])
with sqlite3.connect(data_dir / "msime_user.db") as user_database:
    user_database.execute(
        "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight) "
        "VALUES('pinyin','!','invalid replay row','upsert',1)"
    )
PYTHON
)

if "$project_root/scripts/install.sh"; then
    echo "Install unexpectedly succeeded with an invalid replay journal." >&2
    exit 1
fi

python3 - "$data_dir" "$learned_weight_before_failure" <<'PYTHON'
import sqlite3
import sys
from pathlib import Path

data_dir = Path(sys.argv[1])
expected_weight = int(sys.argv[2])
with sqlite3.connect(data_dir / "msime.db") as database:
    actual_weight = database.execute(
        "SELECT weight FROM tbl_1_n WHERE key='ni' AND value='你'"
    ).fetchone()[0]
if actual_weight != expected_weight:
    raise SystemExit(
        "failed dictionary replay replaced the live database: "
        f"expected {expected_weight}, got {actual_weight}"
    )
PYTHON
