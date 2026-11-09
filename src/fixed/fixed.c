#include "fixed.h"

/* The bitwise adder of the contract. After pass k the low k bits of the carry
 * word are zero, so at most 32 passes run.
 */
uint32_t op1(uint32_t a, uint32_t b, int *n)
{
    int passes = 0;

    while (b) {
        uint32_t carry = (a & b) << 1;
        a ^= b;
        b = carry;
        passes++;
    }
    *n = passes;
    return a;
}

uint32_t op2(uint32_t a, uint32_t b)
{
    return a - b;
}

/* a + b = (a ^ b) + 2 * (a & b), so halving the XOR term alone gives the floor
 * of the exact average, and no intermediate value exceeds the result.
 */
uint32_t op3(uint32_t a, uint32_t b)
{
    return (a & b) + ((a ^ b) >> 1);
}

/* Shift-and-add on the magnitudes, with every 64-bit value held as two 32-bit
 * words. A uint64_t accumulator is not an option: GCC -Os turns its conditional
 * addition into a multiply by 0 or 1, which needs __muldi3.
 */
int32_t q_mul(int32_t a, int32_t b)
{
    uint32_t y_lo = a < 0 ? 0u - (uint32_t) a : (uint32_t) a;
    uint32_t x = b < 0 ? 0u - (uint32_t) b : (uint32_t) b;

    /* Scan the smaller magnitude: the loop ends after its top set bit. */
    if (y_lo < x) {
        uint32_t t = y_lo;
        y_lo = x;
        x = t;
    }

    uint32_t y_hi = 0, lo = 0, hi = 0;
    while (x) {
        if (x & 1) {
            lo += y_lo;
            hi += y_hi + (lo < y_lo);
        }
        y_hi = (y_hi << 1) | (y_lo >> 31);
        y_lo <<= 1;
        x >>= 1;
    }

    if ((a < 0) != (b < 0)) {
        hi = ~hi + (lo == 0);
        lo = 0u - lo;
    }

    /* Add half a unit, then keep bits 20 to 51 of the 64-bit sum. */
    const uint32_t half = 1u << (Q - 1);
    lo += half;
    hi += lo < half;
    return (int32_t) ((lo >> Q) | (hi << (32 - Q)));
}
