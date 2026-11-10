#!/usr/bin/env bash
# Build and run the host reference check for the bf16 program: the structured
# and random sets under ASan and UBSan, the exhaustive sweeps (from_f32 and mul
# over all 2^32 inputs, sqrt over all 2^16) without sanitizers, then confirm
# that each mutant of the C source is caught.
#
# Usage (from anywhere):  bash mytests/bf16/run.sh <output-dir> [random-count]
#
# <output-dir> must not exist yet; it receives the binaries, the compiler
# commands, the program output and the mutated sources. Nothing is written
# inside the repository. src/bf16/bf16.c is compiled unmodified; only the
# copies under <output-dir>/mutants are changed, and its SHA-256 is compared
# before and after the run.
set -euo pipefail

die() {
    echo "run.sh: $*" >&2
    exit 1
}

[ $# -ge 1 ] || die "usage: bash mytests/bf16/run.sh <output-dir> [random-count]"
OUT=$1
COUNT=${2:-10000000}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
CC=${HOST_CC:-cc}

case "$(realpath -m "$OUT")/" in
"$ROOT"/*) die "output directory must be outside the repository" ;;
esac
mkdir "$OUT" || die "cannot create $OUT (it must not exist yet)"

COMMON_FLAGS=(-std=c11 -g -Wall -Wextra -Werror -Wvla
    -iquote "$ROOT" -iquote "$ROOT/src/bf16")
SAN_FLAGS=(-O1 -fsanitize=address,undefined -fno-sanitize-recover=all
    -fno-omit-frame-pointer)
FULL_FLAGS=(-O2)
REF=$ROOT/mytests/bf16/ref.c
IMPL=$ROOT/src/bf16/bf16.c
IMPL_SHA=$(sha256sum "$IMPL" | cut -d' ' -f1)

{
    echo "date: $(date -Iseconds)"
    echo "commit: $(cd "$ROOT" && git rev-parse HEAD)"
    echo "cc: $(command -v "$CC") $("$CC" --version | head -n 1)"
    sha256sum "$REF" "$IMPL"
    echo "+ $CC ${COMMON_FLAGS[*]} ${SAN_FLAGS[*]} $REF $IMPL -o $OUT/ref_san"
    echo "+ $CC ${COMMON_FLAGS[*]} ${FULL_FLAGS[*]} $REF $IMPL -o $OUT/ref_full"
} > "$OUT/build.txt"
"$CC" "${COMMON_FLAGS[@]}" "${SAN_FLAGS[@]}" "$REF" "$IMPL" -o "$OUT/ref_san" \
    >> "$OUT/build.txt" 2>&1 || die "sanitizer build failed (see $OUT/build.txt)"
"$CC" "${COMMON_FLAGS[@]}" "${FULL_FLAGS[@]}" "$REF" "$IMPL" -o "$OUT/ref_full" \
    >> "$OUT/build.txt" 2>&1 || die "build failed (see $OUT/build.txt)"

status=0

# run <label> <binary> <args...>: time one mode and keep its output.
run() {
    local label=$1 bin=$2 rc=0
    shift 2
    /usr/bin/time -f "real=%e user=%U sys=%S" -o "$OUT/$label.time" \
        "$bin" "$@" > "$OUT/$label.stdout" 2> "$OUT/$label.stderr" || rc=$?
    echo "$rc" > "$OUT/$label.rc"
    echo "== $label ($(basename "$bin") $*)"
    cat "$OUT/$label.stdout"
    echo "time: $(cat "$OUT/$label.time"); exit status $rc"
    [ "$rc" = 0 ] || status=1
}

run sets "$OUT/ref_san" sets "$COUNT"
run f32-all "$OUT/ref_full" f32-all
run mul-all "$OUT/ref_full" mul-all
run sqrt-all "$OUT/ref_full" sqrt-all

for label in f32-all mul-all; do
    grep -q "^total inputs 4294967296," "$OUT/$label.stdout" || {
        echo "$label: the run did not cover exactly 4294967296 inputs"
        status=1
    }
done
grep -q "^total inputs 65536," "$OUT/sqrt-all.stdout" || {
    echo "sqrt-all: the run did not cover exactly 65536 inputs"
    status=1
}

# Negative controls: name, then the sed expression applied to a copy. The
# names follow the mutant list of the design (section 7).
MUTANTS=(
    "denormal-keeps-hidden|s/(sign | (frac >> (1 - exp)))/(sign | ((frac | BF16_HIDDEN) >> (1 - exp)))/"
    "denormal-shift-minus-e|s/frac >> (1 - exp)/frac >> (-exp)/"
    "underflow-below-minus-4|s/if (exp < -6)/if (exp < -4)/"
    "b-examined-first|s/if (exp_a == 0xFF) {/if (exp_a == 0xFF \&\& exp_b != 0xFF) {/"
    "inf-times-zero-is-inf|/^uint16_t bf16_mul/,/^}/s/return BF16_QNAN;/return (uint16_t) (sign | BF16_INF);/"
    "zero-times-finite-is-plus-zero|/(!exp_a \&\& !mant_a) || (!exp_b \&\& !mant_b)/{n;s/return (uint16_t) sign;/return 0;/}"
    "denormal-field-as-zero|s/exp_\([ab]\) = 1;/exp_\1 = 0;/"
    "no-tie-to-even|s/f32bits += 0x7FFFu + ((f32bits >> 16) \& 1u);/f32bits += 0x7FFFu;/"
    "round-bias-0x8000|s/f32bits += 0x7FFFu +/f32bits += 0x8000u +/"
    "sqrt-compare-128m|s/if ((mid \* mid) >> 7 <= m) {/if (mid * mid <= 128 * m) {/"
    "sqrt-no-doubling|s/m <<= 1;/m <<= 0;/"
    "sqrt-truncating-exponent|s/uint32_t new_exp = (exp + 127u) >> 1;/uint32_t new_exp = (uint32_t) (((int) exp - 127) \/ 2 + 127);/"
)
mkdir "$OUT/mutants"
for entry in "${MUTANTS[@]}"; do
    name=${entry%%|*}
    expr=${entry#*|}
    src=$OUT/mutants/$name.c
    sed "$expr" "$IMPL" > "$src"
    cmp -s "$IMPL" "$src" && die "mutant $name did not change the source"
    diff "$IMPL" "$src" > "$OUT/mutants/$name.diff" || true
    "$CC" "${COMMON_FLAGS[@]}" "${SAN_FLAGS[@]}" "$REF" "$src" -o "$OUT/mutants/$name" \
        2> "$OUT/mutants/$name.build" || die "mutant $name does not build"
    rc=0
    "$OUT/mutants/$name" sets 1000000 > "$OUT/mutants/$name.stdout" 2>&1 || rc=$?
    if [ "$rc" = 1 ]; then
        echo "mutant $name: caught ($(tail -n 1 "$OUT/mutants/$name.stdout"))"
    else
        echo "mutant $name: NOT CAUGHT (exit $rc)"
        status=1
    fi
done

AFTER_SHA=$(sha256sum "$IMPL" | cut -d' ' -f1)
echo "src/bf16/bf16.c sha256 before $IMPL_SHA"
echo "src/bf16/bf16.c sha256 after  $AFTER_SHA"
[ "$IMPL_SHA" = "$AFTER_SHA" ] || {
    echo "src/bf16/bf16.c changed during the run"
    status=1
}

echo "exit status: $status (output in $OUT)"
exit "$status"
