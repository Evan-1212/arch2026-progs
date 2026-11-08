#!/usr/bin/env bash
# Build and run the gcd target check on rv32emu, one ELF per optimization level.
#
# Usage:  bash mytests/gcd/target/run.sh <output-dir> [dir-of-gcd_model.py]
#
# Needs the objects from "make elfs". Only main.c is compiled here; it is linked
# with build/common/{start,perf,rt,print}.o, build/gcd/<O>/impl.o and
# build/gcd/impl_asm.o, so the code under test is what the harness runs.
# <output-dir> must not exist yet and must be outside the repository.
# The instruction model is the gcd_model.py next to this script by default.
set -euo pipefail

die() {
    echo "run.sh: $*" >&2
    exit 1
}

[ $# -eq 1 ] || [ $# -eq 2 ] ||
    die "usage: bash mytests/gcd/target/run.sh <output-dir> [dir-of-gcd_model.py]"
OUT=$1
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
MODEL_DIR=${2:-$ROOT/mytests/gcd/target}
CROSS_COMPILE=${CROSS_COMPILE:-riscv-none-elf-}
CC=${CROSS_COMPILE}gcc
LD=${CROSS_COMPILE}ld
EMU=$ROOT/third_party/rv32emu/build/rv32emu
BUILD=$ROOT/build
SRC=$ROOT/mytests/gcd/target/main.c

case "$(realpath -m "$OUT")/" in
"$ROOT"/*) die "output directory must be outside the repository" ;;
esac
[ -x "$EMU" ] || die "rv32emu not found: $EMU"
[ -f "$MODEL_DIR/gcd_model.py" ] || die "gcd_model.py not found in $MODEL_DIR"
mkdir "$OUT" || die "cannot create $OUT (it must not exist yet)"

COMMON=("$BUILD/common/start.o" "$BUILD/common/perf.o" "$BUILD/common/rt.o"
    "$BUILD/common/print.o")
BASE_CFLAGS=(-march=rv32i_zicsr -mabi=ilp32 -g -ffreestanding -fno-pic
    -fno-stack-protector -Wall -Wextra -Werror -Wvla
    -iquote "$ROOT" -I"$ROOT/common" -I"$ROOT/tests")

{
    echo "date: $(date -Iseconds)"
    echo "commit: $(cd "$ROOT" && git rev-parse HEAD)"
    echo "rv32emu: $(cd "$ROOT/third_party/rv32emu" && git rev-parse HEAD)"
    echo "cc: $(command -v "$CC") $("$CC" --version | head -n 1)"
    sha256sum "$SRC" "$ROOT/src/gcd/gcd.S" "$ROOT/src/gcd/gcd.c" \
        "$BUILD/gcd/impl_asm.o" "$BUILD"/gcd/O?/impl.o
} > "$OUT/env.txt"

status=0
for O in O1 O2 O3 Os; do
    "$CC" "${BASE_CFLAGS[@]}" "-$O" "-DOPT_NAME=\"$O\"" -c "$SRC" -o "$OUT/$O.main.o" \
        >> "$OUT/build.log" 2>&1 || die "compile failed at -$O (see $OUT/build.log)"
    "$LD" -T "$ROOT/common/linker.ld" --no-warn-rwx-segments -o "$OUT/$O.elf" \
        "${COMMON[@]}" "$BUILD/gcd/$O/impl.o" "$BUILD/gcd/impl_asm.o" "$OUT/$O.main.o" \
        >> "$OUT/build.log" 2>&1 || die "link failed at -$O (see $OUT/build.log)"
    rc=0
    /usr/bin/time -f "real=%e user=%U sys=%S" -o "$OUT/$O.time" \
        "$EMU" "$OUT/$O.elf" > "$OUT/$O.stdout" 2> "$OUT/$O.stderr" || rc=$?
    echo "$rc" > "$OUT/$O.rc"
    echo "-$O rc=$rc $(cat "$OUT/$O.time")"
    [ "$rc" = 0 ] || status=1
done

rc=0
python3 "$ROOT/mytests/gcd/target/analyze.py" "$OUT" "$MODEL_DIR" > "$OUT/analysis.txt" || rc=$?
cat "$OUT/analysis.txt"
[ "$rc" = 0 ] || status=1
echo "exit status: $status (output in $OUT)"
exit "$status"
