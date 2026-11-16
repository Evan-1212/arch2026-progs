#!/usr/bin/env bash
# Build the independent reference mytests/rubik/ref.c with src/rubik/rubik.c
# (unmodified) twice, -O2 and -O1 with ASan/UBSan, and run:
#   -O2:       ref all, ref edge
#   sanitizer: ref sample K (default 1000), ref edge, ref all
# Each run's output goes to <output-dir>/<build>-<mode>.log; the exit status of
# each run is printed and the script fails if any run fails.
# Run from the repository root: bash mytests/rubik/run.sh <output-dir> [K]
set -euo pipefail
if [ $# -lt 1 ]; then
    echo "usage: bash mytests/rubik/run.sh <output-dir> [K]" >&2
    exit 2
fi
OUT="$1"
K="${2:-1000}"
mkdir -p "$OUT"
CFLAGS="-std=c11 -g -Wall -Wextra -Werror -Wvla -iquote ."
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
cc $CFLAGS -O2 -o "$OUT/ref-O2" mytests/rubik/ref.c src/rubik/rubik.c
cc $CFLAGS -O1 $SAN -o "$OUT/ref-san" mytests/rubik/ref.c src/rubik/rubik.c
fail=0
run() {
    local name="$1"
    shift
    local rc=0 t0 t1
    t0=$(date +%s.%N)
    "$@" >"$OUT/$name.log" 2>&1 || rc=$?
    t1=$(date +%s.%N)
    echo "$name rc=$rc seconds=$(python3 -c "print(round($t1 - $t0, 2))")"
    tail -n 3 "$OUT/$name.log"
    [ "$rc" -eq 0 ] || fail=1
}
run O2-all "$OUT/ref-O2" all
run O2-edge "$OUT/ref-O2" edge
run san-sample "$OUT/ref-san" sample "$K"
run san-edge "$OUT/ref-san" edge
run san-all "$OUT/ref-san" all
exit "$fail"
