#!/usr/bin/env bash
# Build and run the host reference check for gcd_u32 under ASan and UBSan.
#
# Usage (from anywhere):  bash mytests/gcd/run.sh <output-dir> [random-count]
#
# <output-dir> must not exist yet; it receives the binary, the compiler
# command, the program output and the timing. Nothing is written inside the
# repository. src/gcd/gcd.c is compiled unmodified.
set -euo pipefail

die() {
    echo "run.sh: $*" >&2
    exit 1
}

[ $# -ge 1 ] || die "usage: bash mytests/gcd/run.sh <output-dir> [random-count]"
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
    -fno-omit-frame-pointer -iquote "$ROOT")
SRCS=("$ROOT/mytests/gcd/ref.c" "$ROOT/src/gcd/gcd.c")

{
    echo "date: $(date -Iseconds)"
    echo "commit: $(cd "$ROOT" && git rev-parse HEAD)"
    echo "cc: $(command -v "$CC") $("$CC" --version | head -n 1)"
    sha256sum "${SRCS[@]}"
    echo "+ $CC ${CFLAGS[*]} ${SRCS[*]} -o $OUT/ref"
} > "$OUT/build.txt"
"$CC" "${CFLAGS[@]}" "${SRCS[@]}" -o "$OUT/ref" >> "$OUT/build.txt" 2>&1 ||
    die "build failed (see $OUT/build.txt)"

rc=0
/usr/bin/time -f "real=%e user=%U sys=%S" -o "$OUT/time.txt" \
    "$OUT/ref" "$COUNT" > "$OUT/stdout.txt" 2> "$OUT/stderr.txt" || rc=$?
echo "$rc" > "$OUT/rc.txt"

cat "$OUT/stdout.txt"
cat "$OUT/time.txt"
echo "exit status: $rc (output in $OUT)"
exit "$rc"
