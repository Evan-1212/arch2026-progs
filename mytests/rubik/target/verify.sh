#!/usr/bin/env bash
# rv32emu check of rubik_solve_asm against rubik_solve and the host reference,
# at -O1 -O2 -O3 -Os, on the objects the harness builds (run make elfs first):
#   - every state of distance 11 (2,644), K fixed-seed random states (default
#     2,000), the tests/rubikvec.h states and, when given, the states of
#     <extra-states-file> (one per line, for example the states of largest
#     search work of a host sweep);
#   - invalid inputs of every class of the -1 rule;
#   - the cap rule on the tests/rubikvec.h states.
# verify.c records, judge.py and `ref replay` (mytests/rubik/ref.c) decide.
# The harness runtime (common/, tests/tperf.h, tests/rubikvec.h) is used
# read-only. Nothing is deleted or overwritten: the output directory must not
# exist yet.
# Run from the repository root:
#   bash mytests/rubik/target/verify.sh <new-output-dir> [extra-states-file] [K]
# Exit status 0 when every level passes.
set -euo pipefail
if [ $# -lt 1 ] || [ $# -gt 3 ]; then
    echo "usage: bash mytests/rubik/target/verify.sh <new-output-dir> [extra-states-file] [K]" >&2
    exit 2
fi
OUT="$1"
EXTRA="${2:-}"
K="${3:-2000}"
if [ -e "$OUT" ]; then
    echo "$OUT exists; give a new directory" >&2
    exit 2
fi
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
EMU="$REPO/third_party/rv32emu/build/rv32emu"
T_START=$(date +%s.%N)
date '+start %Y-%m-%d %H:%M:%S %Z'

# Host reference and inputs.
cc -std=c11 -g -Wall -Wextra -Werror -Wvla -iquote "$REPO" -O2 -o "$OUT/ref" \
    "$REPO/mytests/rubik/ref.c" "$REPO/src/rubik/rubik.c"
"$OUT/ref" inputs "$K" >"$OUT/ref-inputs.txt"
if [ -n "$EXTRA" ]; then
    python3 "$HERE/geninputs.py" "$OUT/ref-inputs.txt" "$OUT" "$EXTRA"
else
    python3 "$HERE/geninputs.py" "$OUT/ref-inputs.txt" "$OUT"
fi

# Target build: the runtime at -O2, as the harness builds it.
P=riscv-none-elf-
ARCH="-march=rv32i_zicsr -mabi=ilp32"
FLAGS="$ARCH -g -ffreestanding -fno-pic -fno-stack-protector -Wall -Wextra -Werror -Wvla"
"${P}gcc" --version | head -1
echo "rv32emu: $(cd "$REPO/third_party/rv32emu" && git rev-parse HEAD)"
"${P}as" $ARCH -g "$REPO/common/start.S" -o "$OUT/start.o"
"${P}as" $ARCH -g "$REPO/common/perf.S" -o "$OUT/perf.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -c "$REPO/common/rt.c" -o "$OUT/rt.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -c "$REPO/common/print.c" -o "$OUT/print.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -I"$REPO/tests" -I"$OUT" -c "$HERE/verify.c" -o "$OUT/verify.o"
sha256sum "$REPO/build/rubik/impl_asm.o" "$REPO/src/rubik/rubik.S" "$REPO/src/rubik/rubik_tables.inc" "$REPO/src/rubik/rubik.c"
for O in O1 O2 O3 Os; do
    "${P}ld" -T "$REPO/common/linker.ld" --no-warn-rwx-segments \
        -o "$OUT/verify-$O.elf" "$OUT/start.o" "$OUT/perf.o" "$OUT/rt.o" \
        "$OUT/print.o" "$OUT/verify.o" "$REPO/build/rubik/$O/impl.o" \
        "$REPO/build/rubik/impl_asm.o"
    sha256sum "$REPO/build/rubik/$O/impl.o"
done

# Run the four ELF files side by side; each records its own exit status.
run_one() {
    local O="$1" rc=0 t0 t1
    t0=$(date +%s.%N)
    "$EMU" "$OUT/verify-$O.elf" >"$OUT/run-$O.out" 2>"$OUT/run-$O.err" || rc=$?
    t1=$(date +%s.%N)
    echo "$rc $(python3 -c "print(round($t1 - $t0, 2))")" >"$OUT/run-$O.rc"
}
for O in O1 O2 O3 Os; do
    run_one "$O" &
done
wait

fail=0
for O in O1 O2 O3 Os; do
    read -r rc secs <"$OUT/run-$O.rc"
    echo "== -$O: rv32emu rc=$rc seconds=$secs"
    [ "$rc" -eq 0 ] || fail=1
    jrc=0
    python3 "$HERE/judge.py" "$OUT" "$O" "$OUT/ref" || jrc=$?
    echo "== -$O: judge rc=$jrc"
    [ "$jrc" -eq 0 ] || fail=1
done
T_END=$(date +%s.%N)
date '+end %Y-%m-%d %H:%M:%S %Z'
echo "total seconds=$(python3 -c "print(round($T_END - $T_START, 2))") result=$([ "$fail" -eq 0 ] && echo PASS || echo FAIL)"
exit "$fail"
