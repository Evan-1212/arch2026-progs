# BFloat16 routines: hand-written RV32I.
#
# uint16_t bf16_from_f32_asm(uint32_t f32bits)   a0 -> a0
# uint16_t bf16_mul_asm(uint16_t a, uint16_t b)  a0, a1 -> a0
# uint16_t bf16_sqrt_asm(uint16_t a)             a0 -> a0
#
# Written from src/bf16/bf16.h, the RV32I ISA and the ilp32 ABI only; no
# compiler output was read. All three routines are leaf functions that use
# caller-saved registers and no stack. uint16_t arguments arrive zero-extended
# and the result is returned zero-extended (RISC-V psABI, integer calling
# convention).
#
# Pseudo-instructions used here: li (one instruction for values that fit in 12
# bits, two for the others), mv, neg, j, bnez, beqz, bgez, bltz, bgtu, ret.

    .equ    F32_EXP_TOP,    0xFF000         # upper immediate of 0xFF000000: a
                                            # top byte of all ones
    .equ    ROUND_UPPER,    8               # upper immediate of 0x8000
    .equ    FRAC_MASK,      0x7F
    .equ    HIDDEN,         0x80
    .equ    EXP_MASK,       0xFF
    .equ    EXP_LIMIT,      254             # (exponent field - 1), taken as
                                            # unsigned, is at least this for
                                            # field 0 and field 0xFF
    .equ    BF16_INF,       0x7F80
    .equ    BF16_QNAN,      0x7FC0
    .equ    MUL_BIAS,       126             # (ea - 1) + (eb - 1) - 126 is the
                                            # result exponent field minus one
    .equ    DENORM_SHIFT_0, 7               # a shift of 7 or more clears a
                                            # 7-bit fraction
    .equ    SQRT_LOW,       127             # 128 * m + 127 = 128 * (m + 1) - 1
    .equ    SQRT_ROOT_TOP,  4               # upper immediate of 1 << 14
    .equ    SQRT_ONE_TOP,   1               # upper immediate of 1 << 12
    .equ    SQRT_BIAS,      126             # (field - 1 + 126) / 2 is the
                                            # result exponent field minus one

    .text

# Narrow a binary32 pattern. Exponent field 0xFF returns the top half as is;
# anything else adds 0x7FFF plus bit 16 and keeps the top half. The addition
# cannot wrap: the exponent field is at most 0xFE here, so the input is at
# most 0xFF7FFFFF.
    .globl  bf16_from_f32_asm
    .type   bf16_from_f32_asm, @function
    .align  2
bf16_from_f32_asm:
    slli    t0, a0, 1                       # exponent field is now on top
    lui     t1, F32_EXP_TOP
    bgeu    t0, t1, .Lf32_top_half          # field 0xFF: Inf or NaN
    srli    t0, a0, 16
    andi    t0, t0, 1                       # bit 16, the tie-to-even term
    lui     t1, ROUND_UPPER
    addi    t1, t1, -1                      # 0x7FFF
    add     a0, a0, t1
    add     a0, a0, t0
.Lf32_top_half:
    srli    a0, a0, 16
    ret
    .size   bf16_from_f32_asm, .-bf16_from_f32_asm

# Multiply. The dispatch tests field(a) with the fewest instructions, so a NaN
# or an Inf first operand leaves early, then does the same for b.
# Registers:
#   t0  field(a) - 1, then the result exponent field minus one (called e1 below)
#   t1  field(b) - 1, then the renormalization carry, then scratch
#   t2  EXP_LIMIT
#   t3  significand of a, then the running addend, then the 8-bit result
#       significand
#   t4  fraction of b, left-justified at bit 31
#   t5  result sign at bit 15
#   t6  running product
    .globl  bf16_mul_asm
    .type   bf16_mul_asm, @function
    .align  2
bf16_mul_asm:
    srli    t0, a0, 7
    andi    t0, t0, EXP_MASK
    addi    t0, t0, -1
    li      t2, EXP_LIMIT
    bgeu    t0, t2, .Lmul_a_outlier         # field(a) is 0 or 0xFF
    srli    t1, a1, 7
    andi    t1, t1, EXP_MASK
    addi    t1, t1, -1
    bgeu    t1, t2, .Lmul_b_outlier         # field(b) is 0 or 0xFF
    andi    t3, a0, FRAC_MASK
    ori     t3, t3, HIDDEN
    slli    t4, a1, 25
.Lmul_core:
    xor     t5, a0, a1
    srli    t5, t5, 15
    slli    t5, t5, 15                      # product sign at bit 15
    add     t0, t0, t1
    addi    t0, t0, -MUL_BIAS
    beqz    t4, .Lmul_check                 # b is a power of two: the product
                                            # is (significand of a) << 7, which
                                            # never reaches bit 15
    slli    t6, t3, 7                       # hidden bit of b
    srli    t3, t6, 1                       # weight of the next fraction bit
.Lmul_test:
    bltz    t4, .Lmul_one                   # next fraction bit of b is set
.Lmul_zero:
    slli    t4, t4, 1
    srli    t3, t3, 1
    bgez    t4, .Lmul_zero                  # t4 stays nonzero, so this loop
                                            # ends at the next set bit
.Lmul_one:
    add     t6, t6, t3
    slli    t4, t4, 1
    srli    t3, t3, 1
    bnez    t4, .Lmul_test                  # no fraction bit left: stop early
    srli    t1, t6, 15                      # 1 when the product reached bit 15
    add     t0, t0, t1
    srli    t3, t6, 7
    srl     t3, t3, t1                      # truncated 8-bit significand
.Lmul_check:
    bgeu    t0, t2, .Lmul_range             # e1 outside [0, 253]
    slli    t0, t0, 7
    add     a0, t0, t3                      # the hidden bit adds the one back
    or      a0, a0, t5
    ret

.Lmul_range:
    bgez    t0, .Lmul_inf_signed            # e1 >= 254: overflow
    neg     t0, t0                          # shift count 1 - exponent = -e1
    li      t1, DENORM_SHIFT_0
    bgeu    t0, t1, .Lmul_zero_signed       # exponent <= -6: fraction is gone
    andi    t3, t3, FRAC_MASK               # the fraction, without the hidden
    srl     a0, t3, t0                      # bit, is what is shifted
    or      a0, a0, t5
    ret
.Lmul_zero_signed:
    mv      a0, t5
    ret

.Lmul_a_outlier:
    beq     t0, t2, .Lmul_a_special         # a is examined first
    srli    t1, a1, 7
    andi    t1, t1, EXP_MASK
    addi    t1, t1, -1
    beq     t1, t2, .Lmul_b_special         # field(b) is 0xFF
    andi    t3, a0, FRAC_MASK
    beqz    t3, .Lmul_signed_zero           # a is zero and b is finite
    slli    t4, a1, 17
    beqz    t4, .Lmul_signed_zero           # b is zero and a is denormal
    addi    t0, t0, 1                       # field taken as 1, so field - 1 = 0
    slli    t3, a0, 24                      # bit 31 is the hidden position
.Lmul_den_a_loop:
    slli    t3, t3, 1
    addi    t0, t0, -1
    bgez    t3, .Lmul_den_a_loop            # at most 7 steps: the fraction is
                                            # nonzero
    srli    t3, t3, 24
    bltz    t1, .Lmul_den_b                 # field(b) is 0 and b is nonzero
    slli    t4, a1, 25
    j       .Lmul_core

.Lmul_b_outlier:
    beq     t1, t2, .Lmul_b_special         # field(b) is 0xFF
    slli    t4, a1, 17
    beqz    t4, .Lmul_signed_zero           # b is zero and a is normal
    andi    t3, a0, FRAC_MASK
    ori     t3, t3, HIDDEN
.Lmul_den_b:
    addi    t1, t1, 1
    slli    t4, a1, 24
.Lmul_den_b_loop:
    slli    t4, t4, 1
    addi    t1, t1, -1
    bgez    t4, .Lmul_den_b_loop
    slli    t4, t4, 1                       # drop the hidden bit
    j       .Lmul_core

.Lmul_a_special:
    andi    t3, a0, FRAC_MASK
    bnez    t3, .Lmul_ret                   # a is a NaN: a0 already holds it
    slli    t3, a1, 17
    beqz    t3, .Lmul_qnan                  # Inf times zero
.Lmul_inf:
    xor     t5, a0, a1
    srli    t5, t5, 15
    slli    t5, t5, 15
.Lmul_inf_signed:
    li      t1, BF16_INF
    or      a0, t5, t1
    ret
.Lmul_b_special:
    andi    t3, a1, FRAC_MASK
    bnez    t3, .Lmul_ret_b                 # b is a NaN
    slli    t3, a0, 17
    bnez    t3, .Lmul_inf                   # b is Inf and a is nonzero
.Lmul_qnan:
    li      a0, BF16_QNAN
.Lmul_ret:
    ret
.Lmul_ret_b:
    mv      a0, a1
    ret
.Lmul_signed_zero:
    xor     a0, a0, a1
    srli    a0, a0, 15
    slli    a0, a0, 15
    ret
    .size   bf16_mul_asm, .-bf16_mul_asm

# Square root. For a positive normal input with exponent field f, sets
#   m = (0x80 | fraction) << (f is even)        128 <= m <= 510
# and takes the largest r with (r * r) / 128 <= m, that is r = isqrt(128 * m +
# 127), r in [128, 255]. Digit-by-digit extraction: N = 128 * m + 127 is at
# least 1 << 14, so the top digit is known to be 1 and seven digits remain.
# Registers:
#   t0  input >> 7 (sign and exponent field)
#   t1  field - 1, later the result exponent field minus one
#   t2  EXP_LIMIT
#   t3  N, then the remainder
#   t4  root under construction
#   t5  weight of the digit being decided
#   t6  trial value
    .globl  bf16_sqrt_asm
    .type   bf16_sqrt_asm, @function
    .align  2
bf16_sqrt_asm:
    srli    t0, a0, 7
    addi    t1, t0, -1
    li      t2, EXP_LIMIT
    bgeu    t1, t2, .Lsqrt_special          # not a positive normal
    andi    t3, a0, FRAC_MASK
    ori     t3, t3, HIDDEN
    andi    t4, t1, 1                       # field even, exponent odd
    sll     t3, t3, t4                      # double the significand
    slli    t3, t3, 7
    addi    t3, t3, SQRT_LOW
    lui     t4, SQRT_ROOT_TOP
    sub     t3, t3, t4
    lui     t5, SQRT_ONE_TOP
.Lsqrt_loop:
    add     t6, t4, t5
    bltu    t3, t6, .Lsqrt_digit_zero
    sub     t3, t3, t6
    add     t4, t6, t5
.Lsqrt_digit_zero:
    srli    t4, t4, 1
    srli    t5, t5, 2
    bnez    t5, .Lsqrt_loop
    addi    t1, t1, SQRT_BIAS
    srli    t1, t1, 1
    slli    t1, t1, 7
    add     a0, t1, t4                      # the hidden bit adds the one back
    ret

.Lsqrt_special:
    beqz    t0, .Lsqrt_zero                 # 0x0000 to 0x007F: zero, denormal
    beq     t1, t2, .Lsqrt_ret              # positive Inf or NaN: unchanged
    slli    t3, a0, 17                      # negative: drop the sign
    beqz    t3, .Lsqrt_zero                 # -0 gives +0
    lui     t4, F32_EXP_TOP
    bgtu    t3, t4, .Lsqrt_ret              # negative NaN: unchanged
    li      a0, BF16_QNAN                   # -Inf or negative nonzero
    ret
.Lsqrt_zero:
    li      a0, 0
.Lsqrt_ret:
    ret
    .size   bf16_sqrt_asm, .-bf16_sqrt_asm
