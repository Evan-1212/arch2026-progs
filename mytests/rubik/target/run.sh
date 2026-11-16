#!/usr/bin/env bash
# rv32emu instruction counts of rubik_solve (C) at -O1 -O2 -O3 -Os for a list
# of states, using the harness objects build/rubik/<O>/impl.o (run make elfs
# first). The harness runtime (common/) is used read-only and built at -O2.
# Run from the repository root:
#   bash mytests/rubik/target/run.sh <output-dir> [state ...]
# Without states it measures the five states of largest search work of the
# host sweep (evidence 20261116-rubik-feasibility.md, section 6), the 11-move
# vector of tests/trubik.c, the distance-9 bench state and the solved cube.
set -euo pipefail
if [ $# -lt 1 ]; then
    echo "usage: bash mytests/rubik/target/run.sh <output-dir> [state ...]" >&2
    exit 2
fi
OUT="$1"
shift
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
EMU="$REPO/third_party/rv32emu/build/rv32emu"
if [ $# -eq 0 ]; then
    set -- 41532672313211 61352472313211 41625372313211 51342763312223 \
        16345271111111 21345671111111 24513763133333 12345671111111
fi
{
    echo "static const char *const states[] = {"
    for s in "$@"; do
        echo "    \"$s\","
    done
    echo "};"
} >"$OUT/probe_states.h"
P=riscv-none-elf-
FLAGS="-march=rv32i_zicsr -mabi=ilp32 -g -ffreestanding -fno-pic -fno-stack-protector -Wall -Wextra -Werror -Wvla"
"${P}gcc" --version | head -1
echo "rv32emu: $(cd "$REPO/third_party/rv32emu" && git rev-parse HEAD)"
"${P}as" -march=rv32i_zicsr -mabi=ilp32 -g "$REPO/common/start.S" -o "$OUT/start.o"
"${P}as" -march=rv32i_zicsr -mabi=ilp32 -g "$REPO/common/perf.S" -o "$OUT/perf.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -c "$REPO/common/rt.c" -o "$OUT/rt.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -c "$REPO/common/print.c" -o "$OUT/print.o"
"${P}gcc" $FLAGS -O2 -I"$REPO/common" -I"$OUT" -c "$HERE/probe.c" -o "$OUT/probe.o"
for O in O1 O2 O3 Os; do
    "${P}ld" -T "$REPO/common/linker.ld" --no-warn-rwx-segments \
        -o "$OUT/probe-$O.elf" "$OUT/start.o" "$OUT/perf.o" "$OUT/rt.o" \
        "$OUT/print.o" "$OUT/probe.o" "$REPO/build/rubik/$O/impl.o"
    echo "== $O ($(sha256sum "$REPO/build/rubik/$O/impl.o" | cut -c1-16))"
    "$EMU" "$OUT/probe-$O.elf"
done
