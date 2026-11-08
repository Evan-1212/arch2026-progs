#include "gcd.h"

/* Binary GCD (Stein's algorithm) with bit-by-bit shifts only: no multiply,
 * divide or remainder, and no libgcc routine such as __ctzsi2.
 *
 * gcd(2^i u, 2^j v) = 2^min(i, j) gcd(u, v) for odd u and v, and for odd
 * a != b, gcd(a, b) = gcd(|a - b|, min(a, b)), where |a - b| is even and
 * nonzero, so each subtraction removes at least one factor of two.
 */
uint32_t gcd_u32(uint32_t a, uint32_t b)
{
    if (a == 0)
        return b;
    if (b == 0)
        return a;

    /* k = ctz(a | b) = min(ctz(a), ctz(b)); a | b != 0, so this ends. */
    uint32_t t = a | b;
    unsigned k = 0;
    while ((t & 1) == 0) {
        t >>= 1;
        k++;
    }
    a >>= k;
    b >>= k;

    /* Strip the remaining twos; a and b are nonzero, so these end. */
    while ((a & 1) == 0)
        a >>= 1;
    while ((b & 1) == 0)
        b >>= 1;

    /* Both odd. The difference is nonzero and even, so shift first. */
    while (a != b) {
        if (a > b) {
            a -= b;
            do
                a >>= 1;
            while ((a & 1) == 0);
        } else {
            b -= a;
            do
                b >>= 1;
            while ((b & 1) == 0);
        }
    }
    return a << k;
}
