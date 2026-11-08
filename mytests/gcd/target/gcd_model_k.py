#!/usr/bin/env python3
"""Directed inputs with k >= 1 for the GCD design v2 model. It lives in
mytests/gcd/target/ next to gcd_model.py, which analyze.py loads by default.

Run:  python3 mytests/gcd/target/gcd_model_k.py

Reuses run() from gcd_model.py (same directory) and prints the
same columns: za, zb, k, n, sum_t, za + zb + sum_t, and the asm-draft body
model (design section 7, without c0). Deterministic; checks math.gcd and
the proof's constraints.
"""
import math
import sys

import gcd_model

K_INPUTS = [
    (2, 4),
    (0x80000000, 0x80000000),
    (0xFFFFFFFE, 0xFFFFFFFC),
    (6, 0x60000000),
]


def main():
    ok = True
    print("%-24s %3s %3s %3s %3s %5s %7s %6s %s" %
          ("(a, b)", "za", "zb", "k", "n", "sum_t", "za+zb+", "model", "gcd"))
    for a, b in K_INPUTS:
        r = gcd_model.run(a, b)
        good = r["g"] == math.gcd(a, b)
        zsum = r["za"] + r["zb"] + r["sum_t"]
        if (not good or zsum > 63 or r["n"] > r["sum_t"]
                or r["k"] != min(r["za"], r["zb"]) or r["k"] < 1):
            ok = False
        print("(0x%08X, 0x%08X) %3d %3d %3d %3d %5d %7d %6d %s" %
              (a, b, r["za"], r["zb"], r["k"], r["n"], r["sum_t"], zsum,
               r["model"], "ok" if good else "MISMATCH"))
    print("all checks:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
