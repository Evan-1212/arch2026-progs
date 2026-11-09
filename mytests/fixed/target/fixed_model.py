#!/usr/bin/env python3
"""Instruction model of the fixed assembly, from the listing in src/fixed/fixed.S.

Every count is the number of retired instructions of the routine's body,
ret included, and excludes the caller constant c0. The defaults are the
true costs; the keyword arguments exist so that a deliberately wrong model
can serve as a negative control.
"""
M32 = 0xFFFFFFFF
OP2_BODY = 2
OP3_BODY = 5
OP1_ZERO_BODY = 4
QMUL_ZERO_BODY = 11  # q_mul(0, 0)


def signed(x):
    return x - (1 << 32) if x & 0x80000000 else x


def op1_passes(a, b):
    n = 0
    while b:
        a, b = a ^ b, ((a & b) << 1) & M32
        n += 1
    return n


def op1_body(a, b, per_pass=5):
    if b == 0:
        return OP1_ZERO_BODY
    return per_pass * op1_passes(a, b) + 4


def qmul_body(a, b, carry_cost=1):
    a, b = signed(a), signed(b)
    y, x = abs(a), abs(b)
    neg = (a < 0) != (b < 0)
    # xor, two abs steps, bgeu, lui, bgez, the optional addi, li, li, bnez.
    cost = 1 + (1 + int(a < 0)) + (1 + int(b < 0)) + 1 + 2 + int(neg) + 2 + 1
    if y < x:
        y, x = x, y
        cost += 3
    if x == 0:
        return cost + 2  # li a0, 0 and ret
    lo = (1 << 19) - 1 if neg else 1 << 19
    hi, yhi, ylo = 0, 0, y
    first = True
    while x:
        if not first:
            top = ylo >> 31
            cost += 3 + top
            yhi = ((yhi << 1) | top) & M32
            ylo = (ylo << 1) & M32
        first = False
        cost += 2
        if x & 1:
            lo += ylo
            carry = lo >> 32
            lo &= M32
            hi = (hi + carry + yhi) & M32
            cost += 3 + carry * carry_cost
        x >>= 1
        cost += 2
    return cost + 5 + int(neg)
