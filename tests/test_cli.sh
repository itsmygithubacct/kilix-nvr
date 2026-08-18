#!/bin/sh
set -eu

command_path=${1:?kilix-nvr command required}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
export KILIX_NVR_HOME="$scratch/state"

expect_rejected_cap() {
    value=$1
    if "$command_path" prune --cap-mb "$value" >"$scratch/out" 2>"$scratch/err"; then
        printf 'accepted invalid --cap-mb value: %s\n' "$value" >&2
        exit 1
    fi
    grep -q -- '--cap-mb' "$scratch/err"
    if "$command_path" prune --dry-run --cap-mb "$value" \
        >"$scratch/out" 2>"$scratch/err"; then
        printf 'dry run accepted invalid --cap-mb value: %s\n' "$value" >&2
        exit 1
    fi
    grep -q -- '--cap-mb' "$scratch/err"
}

for invalid in 0 -1 +1 abc 1x 1.5 18446744073709551615 999999999999999999999999; do
    expect_rejected_cap "$invalid"
done

"$command_path" prune --dry-run --cap-mb 1 >"$scratch/dry"
grep -q '^would remove ' "$scratch/dry"
"$command_path" prune --cap-mb 1 >"$scratch/live"
grep -q '^removed ' "$scratch/live"

# Exercise the parsed limit through both store paths with real scratch media.
# The first prune calls above created the current schema for us.
media="$scratch/recording.bin"
dd if=/dev/zero of="$media" bs=1048576 count=2 status=none
python3 - "$scratch/state/state/events.db" "$media" <<'PY'
import sqlite3
import sys

database, media = sys.argv[1:]
connection = sqlite3.connect(database)
connection.execute(
    "INSERT INTO event (camera, started, ended) VALUES (?, ?, ?)",
    ("boundary-camera", 1, 2),
)
event = connection.execute("SELECT last_insert_rowid()").fetchone()[0]
connection.execute(
    "INSERT INTO media (event, kind, path, bytes) VALUES (?, ?, ?, ?)",
    (event, "clip", media, 2 * 1024 * 1024),
)
connection.commit()
connection.close()
PY
"$command_path" prune --dry-run --cap-mb 1 >"$scratch/dry-media"
grep -q '^would remove 1 event' "$scratch/dry-media"
test -f "$media"
"$command_path" prune --cap-mb 1 >"$scratch/live-media"
grep -q '^removed 1 event' "$scratch/live-media"
test ! -e "$media"

# floor(UINT64_MAX / 1 MiB) is the largest value whose conversion to bytes
# cannot wrap.  The next integer is covered by the hostile-value loop above.
"$command_path" prune --dry-run --cap-mb 17592186044415 \
    >"$scratch/boundary"
grep -q '^would remove ' "$scratch/boundary"
