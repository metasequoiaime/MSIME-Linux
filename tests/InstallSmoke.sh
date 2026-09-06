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
# The installer resolves the environment.d file it writes from XDG_CONFIG_HOME first and only falls back to HOME, so without this a caller who has that variable set gets the file written into their real config directory, naming a component directory inside the temporary tree this test deletes on exit.
export XDG_CONFIG_HOME="$smoke_root/config"
export METASEQUOIA_IME_BUILD_DIR="$build_dir"
mkdir -p "$HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

"$project_root/scripts/install.sh"

data_dir="$XDG_DATA_HOME/metasequoiaime"
test -x "$HOME/.local/libexec/metasequoia-ime-ibus"
test -x "$HOME/.local/libexec/metasequoia-ime-dictionary-replay"
test -x "$HOME/.local/bin/metasequoia-ime-settings"
test -x "$HOME/.local/bin/metasequoia-ime-tools"
test -x "$HOME/.local/bin/metasequoia-ime-voice"
test -x "$HOME/.local/bin/metasequoia-ime-toolbar"
test -f "$XDG_DATA_HOME/ibus/component/metasequoiaime.xml"
test -f "$XDG_DATA_HOME/applications/metasequoia-ime-settings.desktop"
test -f "$XDG_DATA_HOME/applications/metasequoia-ime-tools.desktop"
test -f "$XDG_DATA_HOME/applications/metasequoia-ime-voice.desktop"
test -f "$XDG_DATA_HOME/applications/metasequoia-ime-toolbar.desktop"
test -f "$data_dir/msime.db"
test -f "$data_dir/others.db"
test -f "$data_dir/english.db"
test -f "$data_dir/dict_japanese.dat"
test -f "$data_dir/mozc_dictionary_oss_README.txt"
test -f "$data_dir/helpcodes/helpcode.txt"
test -f "$data_dir/helpcodes/zrm_helpcode_big_unique.txt"
test -f "$data_dir/helpcodes/shouyou2_0_helpcode.txt"
test -f "$data_dir/helpcodes/shouyouplus_helpcode.txt"
test -f "$data_dir/helpcodes/xiaohe_helpcode.txt"
grep -F "<exec>$HOME/.local/libexec/metasequoia-ime-ibus --ibus</exec>" \
    "$XDG_DATA_HOME/ibus/component/metasequoiaime.xml"
# The Japanese scheme this frontend advertises degrades to single-word candidates without the model, and the decoder reports nothing when it fails to load one, so check the magic the Engine actually reads rather than only the file name.
test "$(head -c 7 "$data_dir/dict_japanese.dat")" = MSJPDT1
# IBUS_COMPONENT_PATH replaces the IBus search path rather than extending it, so the line has to name the system directory as well as this run's own component directory. Deriving the system directory the way the installer does also proves the file landed in the sandbox instead of the invoking user's real config directory.
system_component_dir=$(pkg-config --variable=pkgdatadir ibus-1.0 2>/dev/null || true)
grep -Fqx "IBUS_COMPONENT_PATH=${system_component_dir:-/usr/share/ibus}/component:$XDG_DATA_HOME/ibus/component" \
    "$XDG_CONFIG_HOME/environment.d/10-metasequoiaime.conf"

python3 - "$data_dir" <<'PYTHON'
import sqlite3
import sys
from pathlib import Path

data_dir = Path(sys.argv[1])
main_database = sqlite3.connect(data_dir / "msime.db")
seeded_quick_phrase = main_database.execute(
    "SELECT value,weight FROM quick_parases WHERE key='yyds'"
).fetchone()
if seeded_quick_phrase != ("永远滴神", 10):
    raise SystemExit(f"installed dictionary did not contain the shipped quick phrase: {seeded_quick_phrase}")
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

with sqlite3.connect(data_dir / "others.db") as others_database:
    emoji = others_database.execute(
        "SELECT emoji FROM emoji_pinyin WHERE key='xiaolian' ORDER BY sort_order LIMIT 1"
    ).fetchone()
    kaomoji = others_database.execute(
        "SELECT kaomoji FROM kaomoji WHERE pinyin='haixiu' ORDER BY sort_order LIMIT 1"
    ).fetchone()
if emoji != ("😀",) or kaomoji != ("(*/ω＼*)",):
    raise SystemExit(
        f"installed expressive dictionary was incomplete: emoji={emoji}, kaomoji={kaomoji}"
    )
with sqlite3.connect(data_dir / "english.db") as english_database:
    english = english_database.execute(
        "SELECT display FROM english_words WHERE word='hello' ORDER BY weight DESC LIMIT 1"
    ).fetchone()
if english is None or english[0].lower() != "hello":
    raise SystemExit(f"installed English dictionary was incomplete: {english}")

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
user_database.execute(
    "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight) "
    "VALUES('quick','mail','user@example.com','upsert',20)"
)
user_database.execute(
    "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight) "
    "VALUES('quick','yyds','永远滴神','delete',0)"
)
user_database.execute(
    "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight,display) "
    "VALUES('english','codexlinux','CodexLinux','upsert',25,'CodexLinux')"
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
    replayed_quick_phrase = database.execute(
        "SELECT weight FROM quick_parases WHERE key='mail' AND value='user@example.com'"
    ).fetchone()
    deleted_shipped_phrase = database.execute(
        "SELECT COUNT(*) FROM quick_parases WHERE key='yyds' AND value='永远滴神'"
    ).fetchone()[0]
with sqlite3.connect(data_dir / "english.db") as english_database:
    replayed_english = english_database.execute(
        "SELECT weight FROM english_words WHERE word='codexlinux' AND display='CodexLinux'"
    ).fetchone()
if actual_weight != expected_weight:
    raise SystemExit(
        f"journal replay lost learned weight: expected {expected_weight}, got {actual_weight}"
    )
if replayed_quick_phrase != (20,) or deleted_shipped_phrase != 0:
    raise SystemExit(
        "quick-phrase journal replay did not preserve user upserts and deletions: "
        f"upsert={replayed_quick_phrase}, deleted_count={deleted_shipped_phrase}"
    )
if replayed_english != (25,):
    raise SystemExit(f"English journal replay did not survive staged upgrade: {replayed_english}")
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
    quick_phrase = database.execute(
        "SELECT value,weight FROM quick_parases WHERE key='mail'"
    ).fetchone()
    if quick_phrase != ("user@example.com", 20):
        raise SystemExit(f"quick phrase was missing before failed-upgrade coverage: {quick_phrase}")
with sqlite3.connect(data_dir / "msime_user.db") as user_database:
    user_database.execute(
        "INSERT INTO user_dictionary_operations(dictionary,key,value,operation,weight) "
        "VALUES('pinyin','!','invalid replay row','upsert',1)"
    )
with sqlite3.connect(data_dir / "others.db") as others_database:
    others_database.execute("CREATE TABLE failed_upgrade_marker(value TEXT)")
    others_database.execute("INSERT INTO failed_upgrade_marker VALUES('preserve-live-others')")
with sqlite3.connect(data_dir / "english.db") as english_database:
    english_database.execute("CREATE TABLE failed_upgrade_marker(value TEXT)")
    english_database.execute("INSERT INTO failed_upgrade_marker VALUES('preserve-live-english')")
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
    quick_phrase = database.execute(
        "SELECT value,weight FROM quick_parases WHERE key='mail'"
    ).fetchone()
    deleted_shipped_phrase = database.execute(
        "SELECT COUNT(*) FROM quick_parases WHERE key='yyds' AND value='永远滴神'"
    ).fetchone()[0]
if actual_weight != expected_weight:
    raise SystemExit(
        "failed dictionary replay replaced the live database: "
        f"expected {expected_weight}, got {actual_weight}"
    )
if quick_phrase != ("user@example.com", 20) or deleted_shipped_phrase != 0:
    raise SystemExit(
        "failed dictionary replay replaced live quick-phrase state: "
        f"upsert={quick_phrase}, deleted_count={deleted_shipped_phrase}"
    )
with sqlite3.connect(data_dir / "others.db") as others_database:
    marker = others_database.execute("SELECT value FROM failed_upgrade_marker").fetchone()
if marker != ("preserve-live-others",):
    raise SystemExit(f"failed dictionary replay replaced live others.db: {marker}")
with sqlite3.connect(data_dir / "english.db") as english_database:
    marker = english_database.execute("SELECT value FROM failed_upgrade_marker").fetchone()
if marker != ("preserve-live-english",):
    raise SystemExit(f"failed dictionary replay replaced live english.db: {marker}")
PYTHON
