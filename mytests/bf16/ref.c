/* Host reference check for bf16_from_f32, bf16_mul and bf16_sqrt
 * (src/bf16/bf16.c, compiled unmodified).
 *
 * The references follow src/bf16/bf16.h but are written differently from the
 * code under test:
 *   from_f32   compares the low half with 0x8000 instead of adding 0x7FFF
 *   mul        classifies both operands first, forms the exact product of the
 *              raw significands with the host multiply and finds its leading
 *              one, so no operand is normalized; a denormal result shifts the
 *              7-bit fraction and is zero once the shift reaches 7
 *   sqrt       scans mid downward from 256 for the largest mid with
 *              (mid * mid) / 128 <= m, halves the exponent with an exact
 *              signed division, and keeps the renormalization and range
 *              branches of the header even though they are unreachable
 * Before any comparison the references are checked against the golden data
 * of tests/bf16vec.h (cases and hashes).
 *
 * Modes (one per run, so that each can be timed on its own):
 *   ref sets [random-count]   golden self-check, then the public cases, all
 *                             sqrt inputs, the mul grid and columns, every
 *                             denormal against every value (both orders),
 *                             structured from_f32 low halves, random mul and
 *                             random from_f32 pairs (default 10,000,000 each)
 *   ref f32-all               from_f32 over all 2^32 inputs
 *   ref mul-all               mul over all 2^32 operand pairs
 *   ref sqrt-all              sqrt over all 2^16 inputs
 * Exit status: 0 when nothing differs, 1 on any mismatch, 2 on bad usage.
 *
 * Build and run: bash mytests/bf16/run.sh <output-dir> [random-count]
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/bf16/bf16.h"
#include "tests/bf16vec.h"
#include "tests/tutil.h"

#define MAX_REPORTS 10
#define RANDOM_SEED UINT64_C(0x2545F4914F6CDD1D)

enum cls { C_ZERO, C_DENORMAL, C_NORMAL, C_INF, C_NAN };

static enum cls classify(uint16_t x)
{
    unsigned field = (x >> 7) & 0xFFu, frac = x & 0x7Fu;

    if (field == 0xFFu)
        return frac ? C_NAN : C_INF;
    if (field == 0u)
        return frac ? C_DENORMAL : C_ZERO;
    return C_NORMAL;
}

static uint16_t ref_f32(uint32_t x)
{
    uint32_t hi = x >> 16, lo = x & 0xFFFFu;

    if ((x & 0x7F800000u) == 0x7F800000u)
        return (uint16_t) hi;
    if (lo > 0x8000u || (lo == 0x8000u && (hi & 1u)))
        hi++;
    return (uint16_t) hi;
}

static uint16_t ref_mul(uint16_t a, uint16_t b)
{
    const uint16_t sign = (uint16_t) ((a ^ b) & 0x8000u);
    const enum cls ca = classify(a), cb = classify(b);

    if (ca == C_NAN)
        return a;
    if (ca == C_INF)
        return cb == C_ZERO ? 0x7FC0u : (uint16_t) (sign | 0x7F80u);
    if (cb == C_NAN)
        return b;
    if (cb == C_INF)
        return ca == C_ZERO ? 0x7FC0u : (uint16_t) (sign | 0x7F80u);
    if (ca == C_ZERO || cb == C_ZERO)
        return sign;

    /* x = sig * 2^(scale - 134) with scale = max(field, 1) for both normal
     * and denormal operands, so the product is p * 2^(scale_a + scale_b -
     * 268). With the leading one of p at bit n the result exponent field is
     * n + scale_a + scale_b - 141, and the significand, truncated to 8 bits,
     * is the top 8 bits of p.
     */
    unsigned fa = (a >> 7) & 0xFFu, fb = (b >> 7) & 0xFFu;
    uint32_t sa = fa ? 0x80u | (a & 0x7Fu) : a & 0x7Fu;
    uint32_t sb = fb ? 0x80u | (b & 0x7Fu) : b & 0x7Fu;
    uint32_t p = sa * sb;
    int n = 15;
    while (!(p >> n))
        n--;
    uint32_t sig8 = n >= 7 ? p >> (n - 7) : p << (7 - n);
    int field = n + (int) (fa ? fa : 1u) + (int) (fb ? fb : 1u) - 141;

    if (field >= 0xFF)
        return (uint16_t) (sign | 0x7F80u);
    if (field >= 1)
        return (uint16_t) (sign | ((uint32_t) field << 7) | (sig8 & 0x7Fu));
    unsigned shift = (unsigned) (1 - field);
    return (uint16_t) (sign | (shift >= 7u ? 0u : (sig8 & 0x7Fu) >> shift));
}

static uint16_t ref_sqrt(uint16_t a)
{
    const enum cls c = classify(a);

    if (c == C_NAN || a == 0x7F80u)
        return a;
    if ((a & 0x8000u) && c != C_ZERO)
        return 0x7FC0u;
    if (c == C_ZERO || c == C_DENORMAL)
        return 0;

    int e = (int) ((a >> 7) & 0xFFu) - 127;
    uint32_t m = 0x80u | (a & 0x7Fu);
    if (e % 2 != 0) {
        m *= 2u;
        e -= 1;
    }
    int new_exp = e / 2 + 127; /* e is even: the division is exact */

    uint32_t r = 256;
    while (r > 90u && (r * r) / 128u > m)
        r--;
    if ((r * r) / 128u > m)
        r = 128; /* no mid qualifies: the search keeps its start value */
    while (r >= 256u) {
        r >>= 1;
        new_exp++;
    }
    while (r < 128u) {
        r <<= 1;
        new_exp--;
    }
    if (new_exp >= 0xFF)
        return 0x7F80u;
    if (new_exp <= 0)
        return 0;
    return (uint16_t) (((uint32_t) new_exp << 7) | (r & 0x7Fu));
}

/* Golden self-check of the references. */

static int golden_errors;

static void golden_fail(const char *what, uint32_t index)
{
    golden_errors++;
    if (golden_errors <= MAX_REPORTS)
        printf("GOLDEN %s %" PRIu32 " differs\n", what, index);
}

static void check_golden(void)
{
    for (uint32_t i = 0; i < BF16_NMUL; i++) {
        const uint16_t *c = &bf16_mul_cases[3 * i];
        if (ref_mul(c[0], c[1]) != c[2])
            golden_fail("mul case", i);
    }
    for (uint32_t i = 0; i < BF16_NSQRT; i++) {
        if (ref_sqrt(bf16_sqrt_cases[2 * i]) != bf16_sqrt_cases[2 * i + 1])
            golden_fail("sqrt case", i);
    }
    for (uint32_t i = 0; i < BF16_NF32; i++) {
        if (ref_f32(bf16_f32_cases[2 * i]) != bf16_f32_cases[2 * i + 1])
            golden_fail("f32 case", i);
    }
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t h = HASH_INIT;
        for (uint32_t j = 0; j < 256; j++)
            h = hash_add(h, ref_mul(bf16_pts[i], bf16_pts[j]));
        if (hash_fin(h) != bf16_mul_row_hash[i])
            golden_fail("mul grid row", i);
    }
    for (uint32_t c = 0; c < 8; c++) {
        for (uint32_t blk = 0; blk < 64; blk++) {
            uint32_t h = HASH_INIT;
            for (uint32_t a = blk * 1024u; a < blk * 1024u + 1024u; a++)
                h = hash_add(h, ref_mul((uint16_t) a, bf16_consts[c]));
            if (hash_fin(h) != bf16_mul_col_hash[c * 64 + blk])
                golden_fail("mul column block", c * 64 + blk);
        }
    }
    for (uint32_t blk = 0; blk < 256; blk++) {
        uint32_t h = HASH_INIT;
        for (uint32_t a = blk * 256u; a < blk * 256u + 256u; a++)
            h = hash_add(h, ref_sqrt((uint16_t) a));
        if (hash_fin(h) != bf16_sqrt_block_hash[blk])
            golden_fail("sqrt block", blk);
    }
    printf("golden self-check of the references: %d differences\n",
           golden_errors);
}

/* Comparison bookkeeping. Every result of the code under test is folded into
 * a checksum that is printed, so no call can be dropped.
 */

static const char *set_name;
static uint64_t set_inputs, set_errors, n_inputs, n_errors, checksum;
static clock_t set_start;

static void begin(const char *name)
{
    set_name = name;
    set_inputs = 0;
    set_errors = 0;
    set_start = clock();
}

static void end(void)
{
    double secs = (double) (clock() - set_start) / CLOCKS_PER_SEC;
    printf("%-22s inputs %12" PRIu64 "  mismatches %10" PRIu64 "  cpu %.2f s\n",
           set_name, set_inputs, set_errors, secs);
}

static void fold(uint16_t got)
{
    checksum = (checksum ^ got) * UINT64_C(0x100000001B3);
}

static void fail(const char *fn,
                 uint32_t a,
                 uint32_t b,
                 uint16_t got,
                 uint16_t want)
{
    set_errors++;
    n_errors++;
    if (n_errors <= MAX_REPORTS)
        printf("FAIL %s %s(0x%08" PRIX32 ", 0x%04" PRIX32
               ") = 0x%04X, reference 0x%04X\n",
               set_name, fn, a, b, got, want);
}

static void cmp_f32(uint32_t x)
{
    uint16_t got = bf16_from_f32(x), want = ref_f32(x);

    set_inputs++;
    n_inputs++;
    fold(got);
    if (got != want)
        fail("from_f32", x, 0, got, want);
}

static void cmp_mul(uint16_t a, uint16_t b)
{
    uint16_t got = bf16_mul(a, b), want = ref_mul(a, b);

    set_inputs++;
    n_inputs++;
    fold(got);
    if (got != want)
        fail("mul", a, b, got, want);
}

static void cmp_sqrt(uint16_t a)
{
    uint16_t got = bf16_sqrt(a), want = ref_sqrt(a);

    set_inputs++;
    n_inputs++;
    fold(got);
    if (got != want)
        fail("sqrt", a, 0, got, want);
}

static uint64_t rng_state = RANDOM_SEED;

static uint64_t rng(void)
{
    uint64_t z = (rng_state += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static const uint16_t f32_highs[] = {
    0x0000, 0x0001, 0x007F, 0x0080, 0x00FF, 0x0100, 0x3F7F, 0x3F80,
    0x3F81, 0x3FFF, 0x4000, 0x7F00, 0x7F7E, 0x7F7F, 0x7F80, 0x7F81,
    0x7FC0, 0x7FFF, 0x8000, 0x8001, 0x807F, 0x8080, 0xBF80, 0xBFFF,
    0xFF00, 0xFF7E, 0xFF7F, 0xFF80, 0xFF81, 0xFFC0, 0xFFFE, 0xFFFF,
};

static void run_sets(uint64_t count)
{
    check_golden();

    begin("public cases");
    for (uint32_t i = 0; i < BF16_NMUL; i++)
        cmp_mul(bf16_mul_cases[3 * i], bf16_mul_cases[3 * i + 1]);
    for (uint32_t i = 0; i < BF16_NSQRT; i++)
        cmp_sqrt(bf16_sqrt_cases[2 * i]);
    for (uint32_t i = 0; i < BF16_NF32; i++)
        cmp_f32(bf16_f32_cases[2 * i]);
    end();

    begin("sqrt all");
    for (uint32_t a = 0; a < 0x10000u; a++)
        cmp_sqrt((uint16_t) a);
    end();

    begin("mul grid");
    for (uint32_t i = 0; i < 256; i++) {
        for (uint32_t j = 0; j < 256; j++)
            cmp_mul(bf16_pts[i], bf16_pts[j]);
    }
    end();

    begin("mul columns");
    for (uint32_t c = 0; c < 8; c++) {
        for (uint32_t a = 0; a < 0x10000u; a++)
            cmp_mul((uint16_t) a, bf16_consts[c]);
    }
    end();

    /* 0x0000-0x00FF and 0x8000-0x80FF: every zero and denormal (and the
     * smallest normals), each against every value in both orders.
     */
    begin("mul denormal x all");
    for (uint32_t d = 0; d < 0x200u; d++) {
        uint16_t x = (uint16_t) ((d & 0xFFu) | ((d >> 8) << 15));
        for (uint32_t b = 0; b < 0x10000u; b++) {
            cmp_mul(x, (uint16_t) b);
            cmp_mul((uint16_t) b, x);
        }
    }
    end();

    begin("f32 low halves");
    for (size_t h = 0; h < sizeof f32_highs / sizeof f32_highs[0]; h++) {
        for (uint32_t lo = 0; lo < 0x10000u; lo++)
            cmp_f32(((uint32_t) f32_highs[h] << 16) | lo);
    }
    end();

    begin("mul random");
    for (uint64_t i = 0; i < count; i++) {
        uint64_t r = rng();
        cmp_mul((uint16_t) r, (uint16_t) (r >> 16));
    }
    end();

    begin("f32 random");
    for (uint64_t i = 0; i < count; i++)
        cmp_f32((uint32_t) rng());
    end();
}

static void run_f32_all(void)
{
    begin("f32 all 2^32");
    for (uint64_t i = 0; i < (UINT64_C(1) << 32); i++)
        cmp_f32((uint32_t) i);
    end();
}

static void run_mul_all(void)
{
    begin("mul all 2^32");
    for (uint64_t i = 0; i < (UINT64_C(1) << 32); i++)
        cmp_mul((uint16_t) (i >> 16), (uint16_t) i);
    end();
}

static void run_sqrt_all(void)
{
    begin("sqrt all 2^16");
    for (uint64_t i = 0; i < (UINT64_C(1) << 16); i++)
        cmp_sqrt((uint16_t) i);
    end();
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: ref sets [count] | f32-all | mul-all | "
                "sqrt-all\n");
        return 2;
    }
    if (strcmp(argv[1], "sets") == 0) {
        uint64_t count = 10000000u;
        if (argc > 2)
            count = strtoull(argv[2], NULL, 10);
        run_sets(count);
    } else if (strcmp(argv[1], "f32-all") == 0) {
        run_f32_all();
    } else if (strcmp(argv[1], "mul-all") == 0) {
        run_mul_all();
    } else if (strcmp(argv[1], "sqrt-all") == 0) {
        run_sqrt_all();
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    printf("total inputs %" PRIu64 ", mismatches %" PRIu64
           ", checksum 0x%016" PRIX64 "\n",
           n_inputs, n_errors, checksum);
    return (n_errors || golden_errors) ? 1 : 0;
}
