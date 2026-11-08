# gcd_u32_asm: binary GCD, hand-written RV32I.
#
# uint32_t gcd_u32_asm(uint32_t a, uint32_t b)   a in a0, b in a1, result in a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions. Tests: tests/tgcd.c.
#
# Algorithm (Stein): gcd(0, b) = b and gcd(a, 0) = a. Otherwise count the
# shared power of two k = ctz(a | b), remove it from both operands, strip the
# remaining twos so that both are odd, then repeat until a == b: subtract the
# smaller from the larger and strip the even, nonzero difference. The result
# is a << k. Neither operand is swapped: path A updates a0, path B updates a1,
# and each path ends with its own copy of the comparison.
#
# Registers:
#   a0   a; odd during the main loop; the result
#   a1   b; odd during the main loop
#   t0   a | b, shifted right while counting k
#   t1   scratch for the bit tests
#   t2   k
#
# Calling convention: ilp32 leaf function. It uses no stack, does not save ra,
# and touches only the caller-saved a0, a1 and t0-t2.
#
# Loop preconditions:
#   .Lk           t0 even and nonzero: both zero tests have been passed and
#                 the low bit was tested on entry.
#   .Lza, .Lzb    while form (tested on entry): the operand is nonzero, so a
#                 set bit exists; k <= ctz of each operand keeps them nonzero.
#   .La_s, .Lb_s  do-while form (shift before the first test): the difference
#                 is nonzero and even because the operands are odd and differ.
#                 With 1 or 0 here the value would reach 0 and never end.

    .text

    .globl  gcd_u32_asm
    .type   gcd_u32_asm, @function
    .align  2
gcd_u32_asm:                    # a0 = a, a1 = b
    beqz  a0, .Lret_b           # gcd(0, b) = b
    beqz  a1, .Lret             # gcd(a, 0) = a
    or    t0, a0, a1            # t0 != 0 (k-loop precondition)
    andi  t1, t0, 1
    li    t2, 0                 # k
    bnez  t1, .Lk_done
.Lk:                            # t0 even, nonzero
    srli  t0, t0, 1
    addi  t2, t2, 1
    andi  t1, t0, 1
    beqz  t1, .Lk
    srl   a0, a0, t2            # a, b stay nonzero: k <= za, zb
    srl   a1, a1, t2
.Lk_done:
    andi  t1, a0, 1             # strip a (while form, a != 0)
    bnez  t1, .La_odd
.Lza:
    srli  a0, a0, 1
    andi  t1, a0, 1
    beqz  t1, .Lza
.La_odd:
    andi  t1, a1, 1             # strip b (while form, b != 0)
    bnez  t1, .Lb_odd
.Lzb:
    srli  a1, a1, 1
    andi  t1, a1, 1
    beqz  t1, .Lzb
.Lb_odd:                        # a, b odd
    bltu  a0, a1, .Lb
    beq   a0, a1, .Ldone
.La:                            # a > b
    sub   a0, a0, a1            # nonzero even
.La_s:                          # do-while strip
    srli  a0, a0, 1
    andi  t1, a0, 1
    beqz  t1, .La_s
    bltu  a1, a0, .La           # a > b again
    beq   a0, a1, .Ldone        # else fall into B
.Lb:                            # b > a
    sub   a1, a1, a0
.Lb_s:
    srli  a1, a1, 1
    andi  t1, a1, 1
    beqz  t1, .Lb_s
    bltu  a0, a1, .Lb           # b > a again
    bne   a0, a1, .La           # else equal: fall into done
.Ldone:
    sll   a0, a0, t2
.Lret:
    ret
.Lret_b:
    mv    a0, a1
    ret
    .size   gcd_u32_asm, .-gcd_u32_asm
