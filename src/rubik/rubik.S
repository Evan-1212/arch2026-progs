# rubik_solve_asm: optimal 2x2x2 cube solver, hand-written RV32I.
#
# int rubik_solve_asm(const char *state, char *out, size_t cap)
#                                   a0 = state, a1 = out, a2 = cap; result in a0
#
# RV32I only: no mul, div, rem, no floating point, no compressed
# instructions, no heap, no recursion. Tests: tests/trubik.c.
#
# Ported from the hand-written Homework 1 assembly (minirubik commit fc2a123):
#   asm_parse   rv32/asm_v5/parse.S   state text -> coordinates (p, o)
#   asm_solve   rv32/asm_v4/solve.S   iterative deepening A* over (p, o)
#   asm_face_rows                     row pointers, from rv32/asm_v1/target.S
# rubik_solve_asm takes the place of the Homework 1 driver (target_main): it
# calls the two routines and then writes the move names, which Homework 1
# left to its host tooling. The algorithm, the coordinates and the tables are
# those of src/rubik/rubik.c; the tables come from rubik_tables.inc, written
# by mytests/rubik/gen_tables.c.
#
# Stack: rubik_solve_asm 32 bytes, asm_parse 16, asm_solve 48; at most 80
# bytes are in use at once. The search frames are static (144 bytes of .bss).

    .option norvc

    .equ    CH_SPACE, 32
    .equ    CH_TWO, 50
    .equ    CH_PRIME, 39

    .section .bss.asm_frames,"aw",@nobits
    .balign 4
    .type asm_frames,@object
asm_frames:
    .zero 144
    .size asm_frames, .-asm_frames

    .section .rodata.asm_face_rows,"a",@progbits
    .balign 4
    .type asm_face_rows,@object
asm_face_rows:
    .word v1_perm_next,         v1_ori_next
    .word v1_perm_next + 10080, v1_ori_next + 1458
    .word v1_perm_next + 20160, v1_ori_next + 2916
    .size asm_face_rows, .-asm_face_rows
    .type asm_face_ch,@object
asm_face_ch:
    .ascii "RBD"
    .size asm_face_ch, .-asm_face_ch

/* rubik_solve_asm(state, out, cap): returns the number of moves, -1 for an
 * invalid state (out untouched) or -2 when the text does not fit in cap.
 * Frame 32 B: start p/o at 0/2 (written by asm_parse); path[11] at 4..14;
 * out at 16; cap at 20; ra at 28. No callee-saved register is used here.
 * After the search: a0 length n; a1 out; a2 cap; a3 path; a4 path end;
 * t0 path cursor; t2 bytes needed; t3 move, then its turn; t4 scratch;
 * t5 constant 3; t6 output cursor; a5 face letters. */
    .section .text.rubik_solve_asm,"ax",@progbits
    .balign 4
    .globl rubik_solve_asm
    .type rubik_solve_asm,@function
rubik_solve_asm:
    addi sp, sp, -32
    sw ra, 28(sp)
    sw a1, 16(sp)
    sw a2, 20(sp)
    mv a1, sp
    call asm_parse
    beqz a0, .Lrubik_invalid
    lhu a0, 0(sp)
    lhu a1, 2(sp)
    addi a2, sp, 4
    call asm_solve
    li t1, 11
    bltu t1, a0, .Lrubik_invalid
    lw a1, 16(sp)
    lw a2, 20(sp)
    addi a3, sp, 4
    add a4, a3, a0

    /* Bytes needed: the NUL, one letter per move, one more for a move with
     * a suffix (turn 1 or 2), one space before every move but the first.
     * A move is 3*face + turn; subtracting 3 until it is below 3 leaves the
     * turn (at most two subtractions). */
    li t2, 1
    li t5, 3
    mv t0, a3
.Lrubik_need_loop:
    beq t0, a4, .Lrubik_need_done
    lbu t3, 0(t0)
.Lrubik_need_turn:
    bltu t3, t5, .Lrubik_need_count
    addi t3, t3, -3
    j .Lrubik_need_turn
.Lrubik_need_count:
    sltu t3, zero, t3
    add t2, t2, t3
    addi t2, t2, 1
    beq t0, a3, .Lrubik_need_next
    addi t2, t2, 1
.Lrubik_need_next:
    addi t0, t0, 1
    j .Lrubik_need_loop
.Lrubik_need_done:
    bgeu a2, t2, .Lrubik_write
    beqz a2, .Lrubik_small
    sb zero, 0(a1)
.Lrubik_small:
    li a0, -2
    j .Lrubik_return

    /* The text fits: nothing was written to out before this point. */
.Lrubik_write:
    mv t0, a3
    mv t6, a1
    la a5, asm_face_ch
.Lrubik_write_loop:
    beq t0, a4, .Lrubik_write_done
    beq t0, a3, .Lrubik_write_move
    li t4, CH_SPACE
    sb t4, 0(t6)
    addi t6, t6, 1
.Lrubik_write_move:
    lbu t3, 0(t0)
    mv t4, a5
.Lrubik_write_face:
    bltu t3, t5, .Lrubik_write_letter
    addi t3, t3, -3
    addi t4, t4, 1            /* next face letter */
    j .Lrubik_write_face
.Lrubik_write_letter:
    lbu t4, 0(t4)
    sb t4, 0(t6)
    addi t6, t6, 1
    beqz t3, .Lrubik_write_next
    li t4, CH_TWO
    addi t3, t3, -1
    beqz t3, .Lrubik_write_suffix
    li t4, CH_PRIME
.Lrubik_write_suffix:
    sb t4, 0(t6)
    addi t6, t6, 1
.Lrubik_write_next:
    addi t0, t0, 1
    j .Lrubik_write_loop
.Lrubik_write_done:
    sb zero, 0(t6)            /* a0 is still the length */
.Lrubik_return:
    lw ra, 28(sp)
    addi sp, sp, 32
    ret
.Lrubik_invalid:
    li a0, -1
    j .Lrubik_return
    .size rubik_solve_asm, .-rubik_solve_asm

/* asm_parse(input, out): out = two uint16_t fields; a0 = 1/0.
 * Leaf; 16-byte local frame containing six inversion counts.
 * Validate and reread the original permutation bytes instead of copying them.
 * Pointer loops and invariant constants reduce per-character overhead. */
    .section .text.asm_parse,"ax",@progbits
    .type asm_parse,@function
asm_parse:
    addi sp, sp, -16
    beqz a0, .Lparse_fail
    beqz a1, .Lparse_fail
    li a3, 7
    li a4, 1
    li a5, 3
    addi a6, a0, 7
    mv t0, a0
    li t1, 0                 /* permutation digit bitset */
.Lparse_perm:
    lbu t2, 0(t0)
    addi t2, t2, -49
    bgeu t2, a3, .Lparse_fail
    sll t3, a4, t2
    and t6, t1, t3
    bnez t6, .Lparse_fail
    or t1, t1, t3
    addi t0, t0, 1
    bne t0, a6, .Lparse_perm

    /* First six trits form o; the seventh participates only in the sum. */
    addi a6, a0, 13
    li t1, 0
    li t2, 0
.Lparse_ori:
    lbu t3, 0(t0)
    addi t3, t3, -49
    bgeu t3, a5, .Lparse_fail
    add t1, t1, t3
    slli t6, t2, 1
    add t2, t2, t6
    add t2, t2, t3
    addi t0, t0, 1
    bne t0, a6, .Lparse_ori
    lbu t3, 13(a0)
    addi t3, t3, -49
    bgeu t3, a5, .Lparse_fail
    add t1, t1, t3
    lbu t3, 14(a0)
    bnez t3, .Lparse_fail
.Lparse_reduce:
    bltu t1, a5, .Lparse_sum_done
    addi t1, t1, -3
    j .Lparse_reduce
.Lparse_sum_done:
    bnez t1, .Lparse_fail
    mv a2, t2                 /* preserve o while calculating p */

    /* ASCII digits have the same order as the validated values 0..6.
     * All input reads precede output stores. Keep the original inversion
     * counts and mixed-radix calculation; only traversal/storage changes. */
    mv t0, a0
    addi a3, a0, 6
    mv a4, sp
    addi a6, a0, 7
.Lparse_count_outer:
    lbu t3, 0(t0)
    li t1, 0
    addi t5, t0, 1
.Lparse_count_inner:
    lbu t4, 0(t5)
    sltu t4, t4, t3
    add t1, t1, t4
    addi t5, t5, 1
    bne t5, a6, .Lparse_count_inner
    sb t1, 0(a4)
    addi a4, a4, 1
    addi t0, t0, 1
    bne t0, a3, .Lparse_count_outer

    lbu t0, 0(sp)
    slli t1, t0, 2
    slli t0, t0, 1
    add t0, t0, t1            /* x6 */
    lbu t1, 1(sp)
    add t0, t0, t1
    slli t1, t0, 2
    add t0, t0, t1            /* x5 */
    lbu t1, 2(sp)
    add t0, t0, t1
    slli t0, t0, 2            /* x4 */
    lbu t1, 3(sp)
    add t0, t0, t1
    slli t1, t0, 1
    add t0, t0, t1            /* x3 */
    lbu t1, 4(sp)
    add t0, t0, t1
    slli t0, t0, 1            /* x2 */
    lbu t1, 5(sp)
    add t0, t0, t1
    sh t0, 0(a1)
    sh a2, 2(a1)
    li a0, 1
    j .Lparse_return
.Lparse_fail:
    li a0, 0
.Lparse_return:
    addi sp, sp, 16
    ret
    .size asm_parse, .-asm_parse

/* asm_solve(p, o, path): scalar ABI; returns length 0..11 or -1.
 * Leaf function: 0 stack bytes for solved/invalid early returns;
 * 48-byte ABI save area for search; search uses static frames.
 * s0 frame; s1 path; s2 depth; s3 bound; s4/s5 current transition rows;
 * s6 perm_dist; s7 projection; s8 row_offset; s9 packed; s10/s11 root p/o.
 * Active continuation: t2/t3 cursor p/o; a3 face; a4 next turn; a5 prev.
 * Frame offsets: p=0,o=2,cursor_p=4,cursor_o=6,face=8,turn=9,prev=10.
 * Only p/o are authoritative in the active frame. Continuation slots are
 * written before descent and read on backtrack; they may otherwise be stale.
 * No recursion, heap, runtime table generation, or search statistics. */
    .section .text.asm_solve,"ax",@progbits
    .type asm_solve,@function
asm_solve:
    /* Keep the argument contract, including non-null path for solved input.
     * These checks touch only caller-saved registers. */
    beqz a2, .Lsolve_invalid
    li t0, 5040
    bgeu a0, t0, .Lsolve_invalid
    li t0, 729
    bgeu a1, t0, .Lsolve_invalid
    or t0, a0, a1
    /* Both coordinates are zero, hence a0 is already the return value.
     * Jump directly to ret, skipping both the prologue and epilogue. */
    beqz t0, .Lsolve_direct_return

    addi sp, sp, -48
    sw s0, 0(sp)
    sw s1, 4(sp)
    sw s2, 8(sp)
    sw s3, 12(sp)
    sw s4, 16(sp)
    sw s5, 20(sp)
    sw s6, 24(sp)
    sw s7, 28(sp)
    sw s8, 32(sp)
    sw s9, 36(sp)
    sw s10, 40(sp)
    sw s11, 44(sp)
    mv s10, a0
    mv s11, a1
    mv s1, a2
    la s6, v1_perm_dist
    la s7, v2_projection
    la s8, v2_row_offset
    la s9, v2_packed

    /* Non-solved root: max(permutation distance, mixed distance). */
    add t0, s6, s10
    lbu s3, 0(t0)
    add t0, s7, s10
    lbu t0, 0(t0)
    slli t0, t0, 2
    add t0, s8, t0
    lw t0, 0(t0)
    srli t1, s11, 1
    add t0, t0, t1
    add t0, s9, t0
    lbu t0, 0(t0)
    andi t1, s11, 1
    slli t1, t1, 2
    srl t0, t0, t1
    andi t0, t0, 15
    bgeu s3, t0, .Lsolve_initial_bound
    mv s3, t0
.Lsolve_initial_bound:
    li t0, 12
    bgeu s3, t0, .Lsolve_fail
.Lsolve_threshold:
    la s0, asm_frames
    li s2, 0
    sh s10, 0(s0)
    sh s11, 2(s0)
    mv t2, s10
    mv t3, s11
    li a3, 0
    li a4, 0
    li a5, 3

.Lsolve_select_rows:
    li t1, 3
    beq a3, t1, .Lsolve_loop /* no descriptor entry for exhausted face */
    la t1, asm_face_rows
    slli t0, a3, 3
    add t1, t1, t0
    lw s4, 0(t1)
    lw s5, 4(t1)
.Lsolve_loop:
    li t1, 3
    beq a3, t1, .Lsolve_exhausted
    beq a3, a5, .Lsolve_next_face
    beq a4, t1, .Lsolve_next_face

    slli t0, t2, 1
    add t0, s4, t0
    lhu t2, 0(t0)             /* advance cursor even if candidate pruned */
    slli t0, t3, 1
    add t0, s5, t0
    lhu t3, 0(t0)
    slli a0, a3, 1
    add a0, a0, a3
    add a0, a0, a4            /* move = 3*face + old turn */
    addi a4, a4, 1            /* advance parent's continuation first */
    addi a1, s2, 1            /* child_depth */
    /* depth < bound at loop entry, so child_depth <= bound. */
    sub a2, s3, a1            /* remaining >= 0 */

    add t4, s6, t2
    lbu t4, 0(t4)
    bltu a2, t4, .Lsolve_loop
    add t4, s7, t2
    lbu t4, 0(t4)
    slli t4, t4, 2
    add t4, s8, t4
    lw t4, 0(t4)             /* uint32 row offset, can exceed 65535 */
    srli t5, t3, 1
    add t4, t4, t5
    add t4, s9, t4
    lbu t4, 0(t4)
    andi t5, t3, 1
    slli t5, t5, 2
    srl t4, t4, t5
    andi t4, t4, 15
    bltu a2, t4, .Lsolve_loop

    add t4, s1, s2
    sb a0, 0(t4)
    or t4, t2, t3
    beqz t4, .Lsolve_found
    bgeu a1, s3, .Lsolve_loop
    /* Suspend the parent's already-advanced continuation. These five
     * stores occur only on actual descent, not on every candidate. */
    sh t2, 4(s0)
    sh t3, 6(s0)
    sb a3, 8(s0)
    sb a4, 9(s0)
    sb a5, 10(s0)
    mv s2, a1
    addi s0, s0, 12
    sh t2, 0(s0)
    sh t3, 2(s0)
    mv a5, a3                /* child's previous face is parent's face */
    li a3, 0
    li a4, 0
    j .Lsolve_select_rows

.Lsolve_next_face:
    addi a3, a3, 1
    li a4, 0
    lhu t2, 0(s0)
    lhu t3, 2(s0)
    j .Lsolve_select_rows
.Lsolve_exhausted:
    beqz s2, .Lsolve_next_threshold
    addi s2, s2, -1
    addi s0, s0, -12
    lhu t2, 4(s0)
    lhu t3, 6(s0)
    lbu a3, 8(s0)
    lbu a4, 9(s0)
    lbu a5, 10(s0)
    j .Lsolve_select_rows    /* restore parent's cached row addresses */
.Lsolve_next_threshold:
    addi s3, s3, 1
    li t0, 12
    bltu s3, t0, .Lsolve_threshold
.Lsolve_fail:
    li a0, -1
    j .Lsolve_return
.Lsolve_found:
    mv a0, a1
.Lsolve_return:
    lw s0, 0(sp)
    lw s1, 4(sp)
    lw s2, 8(sp)
    lw s3, 12(sp)
    lw s4, 16(sp)
    lw s5, 20(sp)
    lw s6, 24(sp)
    lw s7, 28(sp)
    lw s8, 32(sp)
    lw s9, 36(sp)
    lw s10, 40(sp)
    lw s11, 44(sp)
    addi sp, sp, 48
.Lsolve_direct_return:
    ret
.Lsolve_invalid:
    li a0, -1
    ret
    .size asm_solve, .-asm_solve

    .include "src/rubik/rubik_tables.inc"
