#!/usr/bin/env python3
"""Instruction model of the bf16 assembly, from the listing in src/bf16/bf16.S.

Every count is the number of retired instructions of the routine's body, ret
included, and excludes the caller constant c0. The pseudo-instructions expand
as follows: li of a 12-bit value, mv, neg, j, bnez, beqz, bgez, bltz and bgtu
are one instruction each; li of BF16_INF (0x7F80) and BF16_QNAN (0x7FC0) is
lui plus addi, two instructions.

The data-dependent loops (the fraction loop of mul, the two normalization
loops, the seven digits of sqrt) are simulated step by step on the register
values the assembly holds, not taken from a closed formula. The defaults are
the true costs; the keyword arguments exist so that a deliberately wrong model
can serve as a negative control.
"""
M32 = 0xFFFFFFFF

F32_SPECIAL_BODY = 5
F32_NORMAL_BODY = 11


def f32_body(x, normal=F32_NORMAL_BODY):
    """slli, lui, bgeu, then srli, ret (Inf, NaN) or six more first."""
    return F32_SPECIAL_BODY if ((x << 1) & M32) >= 0xFF000000 else normal


def sqrt_body(a, one_digit=2):
    """Return the body length of bf16_sqrt_asm(a)."""
    t0 = a >> 7
    cost = 4  # srli, addi, li, bgeu
    if 1 <= t0 <= 254:
        cost += 9  # andi, ori, andi, sll, slli, addi, lui, sub, lui
        m = (0x80 | (a & 0x7F)) << (1 if (t0 - 1) & 1 else 0)
        rem = 128 * m + 127 - (1 << 14)
        root, one = 1 << 14, 1 << 12
        while one:
            cost += 5  # add, bltu, srli, srli, bnez
            trial = root + one
            if rem >= trial:
                cost += one_digit  # sub, add
                rem -= trial
                root = trial + one
            root >>= 1
            one >>= 2
        return cost + 5  # addi, srli, slli, add, ret
    cost += 1  # beqz
    if t0 == 0:
        return cost + 2  # li a0, 0 and ret
    cost += 1  # beq
    if t0 == 255:
        return cost + 1  # ret
    cost += 2  # slli, beqz
    if (a & 0x7FFF) == 0:
        return cost + 2  # li a0, 0 and ret
    cost += 2  # lui, bgtu
    if (a & 0x7FFF) > 0x7F80:
        return cost + 1  # ret
    return cost + 3  # li (two) and ret


def _loop(t4, zero_step):
    """Fraction loop from .Lmul_test; t4 is nonzero. Returns its cost."""
    cost = 0
    while True:
        cost += 1  # bltz
        if not t4 >> 31:
            while True:
                cost += zero_step  # slli, srli, bgez
                t4 = (t4 << 1) & M32
                if t4 >> 31:
                    break
        cost += 4  # add, slli, srli, bnez
        t4 = (t4 << 1) & M32
        if t4 == 0:
            return cost


def _normalize(t, start_shift):
    """A normalization loop (slli, addi, bgez per step) on x << start_shift.
    Returns (steps, register value after the loop)."""
    t = (t << start_shift) & M32
    k = 0
    while True:
        t = (t << 1) & M32
        k += 1
        if t >> 31:
            return k, t


def _b_special(a, b, qnan_li):
    cost = 2  # andi, bnez
    if b & 0x7F:
        return cost + 2, "b_nan"  # mv, ret
    cost += 2  # slli, bnez
    if a & 0x7FFF:
        return cost + 3 + 4, "b_inf"  # xor, srli, slli; li (two), or, ret
    return cost + qnan_li + 1, "b_inf_zero"


def mul_info(a, b, zero_step=3, qnan_li=2):
    """Return (body, info) for bf16_mul_asm(a, b). info describes the path:
    path, loop bits (the fraction bits of b the loop visits; 0 when it is
    skipped), k and k2 (normalization steps of a and of b) and the exit."""
    fa, fb = (a >> 7) & 0xFF, (b >> 7) & 0xFF
    info = {"path": None, "bits": None, "k": 0, "k2": 0, "exit": None}
    cost = 5  # srli, andi, addi, li, bgeu
    den_b = False
    if fa in (0, 255):
        cost += 1  # beq
        if fa == 255:
            cost += 2  # andi, bnez
            if a & 0x7F:
                info["path"] = "a_nan"
                return cost + 1, info
            cost += 2  # slli, beqz
            if (b & 0x7FFF) == 0:
                info["path"] = "a_inf_zero"
                return cost + qnan_li + 1, info
            info["path"] = "a_inf"
            return cost + 3 + 4, info
        cost += 4  # srli, andi, addi, beq
        if fb == 255:
            c, info["path"] = _b_special(a, b, qnan_li)
            return cost + c, info
        cost += 2  # andi, beqz
        if (a & 0x7F) == 0:
            info["path"] = "zero"
            return cost + 4, info  # xor, srli, slli, ret
        cost += 2  # slli, beqz
        if (b & 0x7FFF) == 0:
            info["path"] = "zero"
            return cost + 4, info
        cost += 2  # addi, slli
        k, t3 = _normalize(a, 24)
        cost += 3 * k
        info["k"] = k
        sa, ea1 = t3 >> 24, -k
        cost += 2  # srli, bltz
        if fb == 0:
            den_b = True
            info["path"] = "den_ab"
        else:
            cost += 2  # slli, j
            t4, eb1 = (b << 25) & M32, fb - 1
            info["path"] = "den_a"
    else:
        cost += 4  # srli, andi, addi, bgeu
        sa, ea1 = 0x80 | (a & 0x7F), fa - 1
        if fb in (0, 255):
            cost += 1  # beq
            if fb == 255:
                c, info["path"] = _b_special(a, b, qnan_li)
                return cost + c, info
            cost += 2  # slli, beqz
            if (b & 0x7FFF) == 0:
                info["path"] = "zero"
                return cost + 4, info
            cost += 2  # andi, ori
            den_b = True
            info["path"] = "den_b"
        else:
            cost += 3  # andi, ori, slli
            t4, eb1 = (b << 25) & M32, fb - 1
            info["path"] = "nn"
    if den_b:
        cost += 2  # addi, slli
        k2, t4 = _normalize(b, 24)
        cost += 3 * k2 + 2  # loop, slli (drop the hidden bit), j
        t4 = (t4 << 1) & M32
        info["k2"] = k2
        eb1 = -k2

    cost += 6  # xor, srli, slli, add, addi, beqz
    e1 = ea1 + eb1 - 126
    if t4 == 0:
        info["bits"] = 0
    else:
        bits = 7
        while not (t4 >> (32 - bits)) & 1:
            bits -= 1
        info["bits"] = bits
        cost += 2 + _loop(t4, zero_step) + 4
        sb = 0x80 | (t4 >> 25)
        carry = (sa * sb) >> 15
        e1 += carry

    cost += 1  # bgeu
    if 0 <= e1 <= 253:
        info["exit"] = "normal"
        return cost + 4, info  # slli, add, or, ret
    cost += 1  # bgez
    if e1 >= 254:
        info["exit"] = "inf"
        return cost + 4, info  # li (two), or, ret
    cost += 3  # neg, li, bgeu
    if -e1 >= 7:
        info["exit"] = "zero"
        return cost + 2, info  # mv, ret
    info["exit"] = "denormal"
    return cost + 4, info  # andi, srl, or, ret


def mul_body(a, b, **kw):
    return mul_info(a, b, **kw)[0]


# Static upper bounds of the design (section 5); the analysis checks that no
# measured body exceeds them.
F32_BOUND = 11
MUL_BOUND = 97
SQRT_BOUND = 67
