#!/usr/bin/env python3
"""Host model of the GCD design v2. It lives in mytests/gcd/target/, and
analyze.py loads it by default.

Run:  python3 mytests/gcd/target/gcd_model.py

For each directed input it simulates the v2 algorithm step by step (k loop of
plan (i), while-form stripping of a and b, main loop with do-while stripping)
and reports:
  za, zb   trailing zeros of a and b
  k        shared power of two removed by the k loop (= min(za, zb))
  n        main-loop iterations (subtractions)
  sum_t    total bits removed by the main-loop strips
  bound    za + zb + sum_t (proof: <= 63), and n <= sum_t
  model    instruction count of the v2 asm DRAFT (design section B7), body
           only, without the caller constant c0. A design estimate, not a
           measurement.
The result is checked against math.gcd. No randomness; output is deterministic.
"""
import math
import sys

M32 = 0xFFFFFFFF

# tests/tgcd.c check_perf "hard" set (12 pairs), in source order.
HARD = [
    (0xFFFFFFFF, 1), (1, 0xFFFFFFFF),
    (0x80000000, 1), (0x80000000, 0x7FFFFFFF),
    (0xFFFFFFFF, 0xFFFFFFFE), (2971215073, 1836311903),
    (1836311903, 1134903170), (0xFFFFFFFF, 0xFFFF),
    (0xDEADBEEF, 0xCAFEBABE), (0x7FFFFFFF, 0x40000000),
    (0xAAAAAAAA, 0x55555555), (0x00010001, 0xFFFF0000),
]
EXTRA = [(0xFFFFFFFF, 0xFFFFFFFE), (0x55555FFF, 0xAAAAA001)]


def ctz(x):
    assert x != 0
    return (x & -x).bit_length() - 1


def run(a, b):
    """Simulate the v2 algorithm and the asm-draft cost model."""
    a0, b0 = a, b
    if a == 0:
        return dict(g=b, za=None, zb=None, k=0, n=0, sum_t=0, model=3)
    if b == 0:
        return dict(g=a, za=None, zb=None, k=0, n=0, sum_t=0, model=3)
    za, zb = ctz(a0), ctz(b0)
    cost = 6  # beqz, beqz, or, andi, li k, bnez
    # k loop, plan (i): needs (a | b) != 0, true because a, b != 0.
    t0, k = a | b, 0
    if t0 & 1 == 0:
        while t0 & 1 == 0:  # srli, addi, andi, beqz
            t0 >>= 1
            k += 1
            cost += 4
        a >>= k  # srl a0
        b >>= k  # srl a1
        cost += 2
    # while-form strips: need x != 0 (guaranteed by the zero branches).
    cost += 2  # andi, bnez for a
    while a & 1 == 0:
        a >>= 1
        cost += 3
    cost += 2  # andi, bnez for b
    while b & 1 == 0:
        b >>= 1
        cost += 3
    # entry dispatch: bltu a0,a1 (taken when a < b) ; beq a0,a1
    cost += 1 if a < b else 2
    n = sum_t = 0
    while a != b:
        path = 'A' if a > b else 'B'
        if path == 'A':
            a = a - b
        else:
            b = b - a
        cost += 1  # sub
        # do-while strip: needs x nonzero even (a != b, both odd).
        t = 0
        while True:
            if path == 'A':
                a >>= 1
                odd = a & 1
            else:
                b >>= 1
                odd = b & 1
            t += 1
            cost += 3  # srli, andi, beqz
            if odd:
                break
        n += 1
        sum_t += t
        nxt = 'A' if a > b else ('B' if a < b else 'done')
        cost += 1 if nxt == path else 2  # loop tail
    cost += 2  # sll, ret
    g = (a << k) & M32
    return dict(g=g, za=za, zb=zb, k=k, n=n, sum_t=sum_t, model=cost)


def main():
    ok = True
    print("%-24s %3s %3s %3s %3s %5s %7s %6s %s" %
          ("(a, b)", "za", "zb", "k", "n", "sum_t", "za+zb+", "model", "gcd"))
    rows = [("hard[%d]" % i, p) for i, p in enumerate(HARD)]
    rows += [("extra", p) for p in EXTRA]
    for tag, (a, b) in rows:
        r = run(a, b)
        want = math.gcd(a, b)
        if r["g"] != want:
            ok = False
        if r["za"] is None:
            zsum = "-"
        else:
            zsum = r["za"] + r["zb"] + r["sum_t"]
            if zsum > 63 or r["n"] > r["sum_t"] or r["k"] != min(r["za"], r["zb"]):
                ok = False
        print("%-8s (0x%08X, 0x%08X) %3s %3s %3d %3d %5d %7s %6d %s" %
              (tag, a, b, r["za"], r["zb"], r["k"], r["n"], r["sum_t"], zsum,
               r["model"], "ok" if r["g"] == want else "MISMATCH"))
    print("all checks:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
