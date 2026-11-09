#!/usr/bin/env python3
"""Cross-check the directed results of the host reference with big integers.

Usage:  python3 mytests/fixed/check_directed.py <directed-dump>

Every line of the dump written by mytests/fixed/ref.c is
"a b op1_sum op1_passes op2 op3 q_mul" (hex, except the decimal pass count).
Each value is recomputed from the contract with Python integers, which share
nothing with the C code. The exit status is 0 only when every line agrees.
"""
import sys

sys.dont_write_bytecode = True

M32 = (1 << 32) - 1


def signed(x):
    return x - (1 << 32) if x & (1 << 31) else x


def passes(a, b):
    n = 0
    while b:
        a, b = a ^ b, ((a & b) << 1) & M32
        n += 1
    return a, n


def main():
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    lines = bad = 0
    with open(sys.argv[1], encoding="ascii") as f:
        for line in f:
            field = line.split()
            a, b, s = int(field[0], 16), int(field[1], 16), int(field[2], 16)
            n = int(field[3])
            d, m, q = (int(x, 16) for x in field[4:7])
            lines += 1
            want_s, want_n = passes(a, b)
            want = (
                want_s == (a + b) & M32,
                s == want_s,
                n == want_n,
                d == (a - b) & M32,
                m == (a + b) // 2,
                q == ((signed(a) * signed(b) + (1 << 19)) >> 20) & M32,
            )
            if not all(want):
                bad += 1
                if bad <= 10:
                    print("MISMATCH", line.strip(), want)
    print("python cross-check: lines=%d mismatches=%d" % (lines, bad))
    return 0 if lines and not bad else 1


if __name__ == "__main__":
    sys.exit(main())
