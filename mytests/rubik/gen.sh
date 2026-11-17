#!/usr/bin/env bash
# Build mytests/rubik/gen_tables.c on the host and write both outputs:
#   <output-dir>/rubik_tables.h   (copy to src/rubik/rubik_tables.h)
#   <output-dir>/rubik_tables.inc (copy to src/rubik/rubik_tables.inc; it is
#                                 included by rubik.S and must not end in .S,
#                                 or the Makefile assembles it a second time)
# Then check that clang-format-20 leaves the header unchanged.
# Run from the repository root: bash mytests/rubik/gen.sh <output-dir>
set -euo pipefail
if [ $# -ne 1 ]; then
    echo "usage: bash mytests/rubik/gen.sh <output-dir>" >&2
    exit 2
fi
OUT="$1"
mkdir -p "$OUT"
HERE="$(cd "$(dirname "$0")" && pwd)"
cc -std=c11 -O2 -Wall -Wextra -Werror -o "$OUT/gen_tables" "$HERE/gen_tables.c"
"$OUT/gen_tables" c >"$OUT/rubik_tables.h"
"$OUT/gen_tables" asm >"$OUT/rubik_tables.inc"
cp "$HERE/../../.clang-format" "$OUT/.clang-format"
clang-format-20 "$OUT/rubik_tables.h" >"$OUT/rubik_tables.fmt.h"
rc=0
cmp "$OUT/rubik_tables.h" "$OUT/rubik_tables.fmt.h" || rc=$?
echo "clang-format-20 cmp rc=$rc"
sha256sum "$OUT/rubik_tables.h" "$OUT/rubik_tables.inc"
exit "$rc"
