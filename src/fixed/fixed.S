# op1, op2, op3 and q_mul: hand-written RV32I.
#
# uint32_t op1_asm(uint32_t a, uint32_t b, int *n)   a0, a1, a2 -> a0
# uint32_t op2_asm(uint32_t a, uint32_t b)           a0, a1 -> a0
# uint32_t op3_asm(uint32_t a, uint32_t b)           a0, a1 -> a0
# int32_t  q_mul_asm(int32_t a, int32_t b)           a0, a1 -> a0
#
# RV32I and Zicsr only: no mul, div, rem, no floating point, no compressed
# instructions. q_mul forms a 64-bit product in a register pair. Tests:
# tests/tfixed.c.
#
# All four are leaf functions: they use no stack, change only caller-saved
# registers and leave ra alone.

    .text

# op1: the bitwise adder of the contract, one pass per loop iteration.
#   a0  running sum (returned)
#   a1  carry word
#   a2  int *n, written once on every path
#   t0  pass count
#   t1  a & b
# Cost in instructions, ret included: 4 when b = 0, else 5 * passes + 4, which
# is at most 164 because the carry word is zero after 32 passes.
    .globl  op1_asm
    .type   op1_asm, @function
    .align  2
op1_asm:
    li    t0, 0
    beqz  a1, .Lop1_done
.Lop1_loop:
    and   t1, a0, a1
    xor   a0, a0, a1
    slli  a1, t1, 1
    addi  t0, t0, 1
    bnez  a1, .Lop1_loop
.Lop1_done:
    sw    t0, 0(a2)
    ret
    .size   op1_asm, .-op1_asm

# op2: subtraction modulo 2^32. Two instructions on every path.
    .globl  op2_asm
    .type   op2_asm, @function
    .align  2
op2_asm:
    sub   a0, a0, a1
    ret
    .size   op2_asm, .-op2_asm

# op3: floor((a + b) / 2), from a + b = (a ^ b) + 2 * (a & b). No intermediate
# value exceeds the result. Five instructions on every path.
#   t0  a & b
    .globl  op3_asm
    .type   op3_asm, @function
    .align  2
op3_asm:
    and   t0, a0, a1
    xor   a0, a0, a1
    srli  a0, a0, 1
    add   a0, a0, t0
    ret
    .size   op3_asm, .-op3_asm

# q_mul: shift-and-add on the magnitudes M = |a| * |b|, scanning the smaller
# magnitude so that the loop ends after its top set bit.
#   a0      multiplicand, low word (the result at the end)
#   a1      x, the magnitude being scanned
#   t0, t1  accumulator, low and high words
#   t2      multiplicand, high word
#   t3      bit 31 is the sign of the product
#   t4      scratch
# The rounding constant is seeded into the accumulator, so no 64-bit negation
# and no addition with carry follows the loop. With s = 1 for a negative
# product, the accumulator ends as M + 2^19 - s and the result is
# (acc >> 20), negated when s = 1, because
#   floor((-M + 2^19) / 2^20) = -floor((M + 2^19 - 1) / 2^20).
# The loop is entered at the bit test, so the final shift of the multiplicand
# never runs. Cost: at most 388 instructions, ret included.
    .globl  q_mul_asm
    .type   q_mul_asm, @function
    .align  2
q_mul_asm:
    xor   t3, a0, a1
    bgez  a0, .Lq_a_pos
    neg   a0, a0
.Lq_a_pos:
    bgez  a1, .Lq_b_pos
    neg   a1, a1
.Lq_b_pos:
    bgeu  a0, a1, .Lq_ordered
    mv    t4, a0
    mv    a0, a1
    mv    a1, t4
.Lq_ordered:
    lui   t0, 0x80
    bgez  t3, .Lq_seeded
    addi  t0, t0, -1
.Lq_seeded:
    li    t1, 0
    li    t2, 0
    bnez  a1, .Lq_test
    li    a0, 0
    ret
.Lq_loop:
    slli  t2, t2, 1
    bgez  a0, .Lq_no_top
    ori   t2, t2, 1
.Lq_no_top:
    slli  a0, a0, 1
.Lq_test:
    andi  t4, a1, 1
    beqz  t4, .Lq_next
    add   t0, t0, a0
    bgeu  t0, a0, .Lq_no_carry
    addi  t1, t1, 1
.Lq_no_carry:
    add   t1, t1, t2
.Lq_next:
    srli  a1, a1, 1
    bnez  a1, .Lq_loop
    srli  a0, t0, 20
    slli  t1, t1, 12
    or    a0, a0, t1
    bgez  t3, .Lq_end
    neg   a0, a0
.Lq_end:
    ret
    .size   q_mul_asm, .-q_mul_asm
