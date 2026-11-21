#!/usr/bin/env bash
set -euo pipefail
# Output and measurement artifacts stay outside the repository.
if (( $# != 3 )); then
    printf 'usage: benchmark.sh BINARY DATA_DIR NEW_RESULT_DIR\n' >&2
    exit 2
fi
binary=$(realpath "$1")
data=$(realpath "$2")
result=$3
if [[ -e "$result" ]]; then
    printf 'result path already exists: %s\n' "$result" >&2
    exit 2
fi
mkdir -p "$result"
for threads in 1 2 4 8; do
    for repeat in 1 2 3; do
        out="$result/t${threads}-r${repeat}"
        mkdir "$out"
        NCPU=$threads /usr/bin/time -f 'wall_seconds=%e user_seconds=%U system_seconds=%S max_rss_kib=%M exit=%x' \
            "$binary" "$data" "$out" >"$out.stdout" 2>"$out.stderr"
        printf 'threads=%s repeat=%s\n' "$threads" "$repeat"
        grep -E '^(total |wall_seconds=)' "$out.stderr"
    done
done
