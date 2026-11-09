#!/usr/bin/env bash
# Build and run the host reference check for the fixed program under ASan and
# UBSan, cross-check the directed results with Python integers, then confirm
# that each mutant of the C source is caught.
#
# Usage (from anywhere):  bash mytests/fixed/run.sh <output-dir> [random-count]
#
# <output-dir> must not exist yet; it receives the binaries, the compiler
# command, the program output and the mutated sources. Nothing is written
# inside the repository. src/fixed/fixed.c is compiled unmodified; only the
# copies under <output-dir>/mutants are changed.
set -euo pipefail

die() {
    echo "run.sh: $*" >&2
    exit 1
}

[ $# -ge 1 ] || die "usage: bash mytests/fixed/run.sh <output-dir> [random-count]"
OUT=$1
COUNT=${2:-10000000}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${HOST_CC:-cc}

case "$(realpath -m "$OUT")/" in
"$ROOT"/*) die "output directory must be outside the repository" ;;
esac
mkdir "$OUT" || die "cannot create $OUT (it must not exist yet)"

CFLAGS=(-std=c11 -O1 -g -Wall -Wextra -Werror -Wvla
    -fsanitize=address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer -iquote "$ROOT" -iquote "$ROOT/src/fixed")
REF=$ROOT/mytests/fixed/ref.c
IMPL=$ROOT/src/fixed/fixed.c

{
    echo "date: $(date -Iseconds)"
    echo "commit: $(cd "$ROOT" && git rev-parse HEAD)"
    echo "cc: $(command -v "$CC") $("$CC" --version | head -n 1)"
    sha256sum "$REF" "$IMPL"
    echo "+ $CC ${CFLAGS[*]} $REF $IMPL -o $OUT/ref"
} > "$OUT/build.txt"
"$CC" "${CFLAGS[@]}" "$REF" "$IMPL" -o "$OUT/ref" >> "$OUT/build.txt" 2>&1 ||
    die "build failed (see $OUT/build.txt)"

status=0
rc=0
/usr/bin/time -f "real=%e user=%U sys=%S" -o "$OUT/time.txt" \
    "$OUT/ref" "$COUNT" "$OUT/directed.txt" > "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || rc=$?
echo "$rc" > "$OUT/rc.txt"
cat "$OUT/stdout.txt"
cat "$OUT/time.txt"
echo "reference exit status: $rc"
[ "$rc" = 0 ] || status=1

rc=0
python3 "$ROOT/mytests/fixed/check_directed.py" "$OUT/directed.txt" > "$OUT/python.txt" || rc=$?
cat "$OUT/python.txt"
[ "$rc" = 0 ] || status=1

# Negative controls: name, then the sed expression applied to a copy.
MUTANTS=(
    "no-lo-carry|s/hi += lo < half;/hi += 0;/"
    "no-rounding|s/half = 1u << (Q - 1)/half = 0u/"
    "no-accumulator-carry|s/hi += y_hi + (lo < y_lo);/hi += y_hi;/"
    "no-negate-borrow|s/hi = ~hi + (lo == 0);/hi = ~hi;/"
    "truncate-sign|s/if ((a < 0) != (b < 0))/if (a < 0)/"
    "pass-count-off|s/int passes = 0;/int passes = 1;/"
    "overflowing-average|s/return (a \& b) + ((a ^ b) >> 1);/return (a + b) >> 1;/"
)
mkdir "$OUT/mutants"
for entry in "${MUTANTS[@]}"; do
    name=${entry%%|*}
    expr=${entry#*|}
    src=$OUT/mutants/$name.c
    sed "$expr" "$IMPL" > "$src"
    cmp -s "$IMPL" "$src" && die "mutant $name did not change the source"
    "$CC" "${CFLAGS[@]}" "$REF" "$src" -o "$OUT/mutants/$name" \
        2> "$OUT/mutants/$name.build" || die "mutant $name does not build"
    rc=0
    "$OUT/mutants/$name" 200000 > "$OUT/mutants/$name.stdout" 2>&1 || rc=$?
    if [ "$rc" = 1 ]; then
        echo "mutant $name: caught ($(tail -n 1 "$OUT/mutants/$name.stdout"))"
    else
        echo "mutant $name: NOT CAUGHT (exit $rc)"
        status=1
    fi
done

echo "exit status: $status (output in $OUT)"
exit "$status"
