/* BFloat16 routines in C, written after the Quiz 1 Problem B code with the
 * behavior of bf16.h. Every quantity is unsigned or a small int that never
 * reaches a signed right shift.
 */
#include "bf16.h"

#define BF16_FRAC_MASK 0x7Fu
#define BF16_HIDDEN 0x80u
#define BF16_EXP_MAX 0xFFu
#define BF16_INF 0x7F80u
#define BF16_QNAN 0x7FC0u
#define BF16_SIGN 0x8000u

uint16_t bf16_from_f32(uint32_t f32bits)
{
    if (((f32bits >> 23) & BF16_EXP_MAX) == BF16_EXP_MAX)
        return (uint16_t) (f32bits >> 16);
    f32bits += 0x7FFFu + ((f32bits >> 16) & 1u);
    return (uint16_t) (f32bits >> 16);
}

uint16_t bf16_mul(uint16_t a, uint16_t b)
{
    uint32_t sign = (uint32_t) (a ^ b) & BF16_SIGN;
    int exp_a = (a >> 7) & 0xFF, exp_b = (b >> 7) & 0xFF;
    uint32_t mant_a = a & BF16_FRAC_MASK, mant_b = b & BF16_FRAC_MASK;

    if (exp_a == 0xFF) {
        if (mant_a)
            return a;
        if (!exp_b && !mant_b)
            return BF16_QNAN;
        return (uint16_t) (sign | BF16_INF);
    }
    if (exp_b == 0xFF) {
        if (mant_b)
            return b;
        if (!exp_a && !mant_a)
            return BF16_QNAN;
        return (uint16_t) (sign | BF16_INF);
    }
    if ((!exp_a && !mant_a) || (!exp_b && !mant_b))
        return (uint16_t) sign;

    int exp_adjust = 0;
    if (exp_a) {
        mant_a |= BF16_HIDDEN;
    } else {
        while (!(mant_a & BF16_HIDDEN)) {
            mant_a <<= 1;
            exp_adjust--;
        }
        exp_a = 1;
    }
    if (exp_b) {
        mant_b |= BF16_HIDDEN;
    } else {
        while (!(mant_b & BF16_HIDDEN)) {
            mant_b <<= 1;
            exp_adjust--;
        }
        exp_b = 1;
    }

    uint32_t product = mant_a * mant_b;
    int exp = exp_a + exp_b - 127 + exp_adjust;
    uint32_t frac;
    if (product & BF16_SIGN) {
        frac = (product >> 8) & BF16_FRAC_MASK;
        exp++;
    } else {
        frac = (product >> 7) & BF16_FRAC_MASK;
    }

    if (exp >= 0xFF)
        return (uint16_t) (sign | BF16_INF);
    if (exp <= 0) {
        if (exp < -6)
            return (uint16_t) sign;
        return (uint16_t) (sign | (frac >> (1 - exp)));
    }
    return (uint16_t) (sign | ((uint32_t) exp << 7) | frac);
}

uint16_t bf16_sqrt(uint16_t a)
{
    uint32_t sign = a >> 15, exp = (a >> 7) & BF16_EXP_MAX;
    uint32_t mant = a & BF16_FRAC_MASK;

    if (exp == BF16_EXP_MAX) {
        if (mant || !sign)
            return a;
        return BF16_QNAN;
    }
    if (!exp && !mant)
        return 0;
    if (sign)
        return BF16_QNAN;
    if (!exp)
        return 0;

    /* The unbiased exponent is odd exactly when the field is even. Working
     * from the biased field keeps every quantity non-negative, so the halving
     * of a negative exponent never needs a signed right shift.
     */
    uint32_t m = BF16_HIDDEN | mant;
    if (!(exp & 1u))
        m <<= 1;
    uint32_t new_exp = (exp + 127u) >> 1;

    uint32_t low = 90, high = 256, result = 128;
    while (low <= high) {
        uint32_t mid = (low + high) >> 1;
        if ((mid * mid) >> 7 <= m) {
            result = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return (uint16_t) ((new_exp << 7) | (result & BF16_FRAC_MASK));
}
