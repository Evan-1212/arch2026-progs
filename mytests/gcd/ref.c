/* Host reference check for gcd_u32 (src/gcd/gcd.c, compiled unmodified).
 *
 * The reference is Euclid's algorithm with the host's % operator, a separate
 * implementation from the restoring division in tests/tutil.h. Every result
 * is also checked algebraically: g divides a and b, and the cofactors a / g
 * and b / g are coprime (only when g != 0; gcd(0, 0) = 0 has no cofactors).
 *
 * Sets:
 *   exhaustive  every pair 0 <= a, b < 2^12
 *   structured  (u << i, v << j) for odd bases u, v; boundary values and
 *               their neighbours; Fibonacci numbers; directed hard inputs
 *   random      fixed-seed xorshift32 pairs of random bit length
 *
 * Build and run: bash mytests/gcd/run.sh <output-dir>
 * Usage of the binary: ref [random-count] (default 10000000)
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/gcd/gcd.h"

#define RANDOM_SEED 0x9E3779B9u
#define MAX_REPORTS 10

static uint64_t n_checked, n_failed;

static uint32_t ref_euclid(uint32_t a, uint32_t b)
{
    while (b != 0) {
        uint32_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static void report(const char *set,
                   uint32_t a,
                   uint32_t b,
                   uint32_t got,
                   const char *why)
{
    n_failed++;
    if (n_failed <= MAX_REPORTS)
        printf("FAIL %s gcd(0x%08" PRIX32 ", 0x%08" PRIX32 ") = 0x%08" PRIX32
               ": %s\n",
               set, a, b, got, why);
}

/* Check one pair; symmetric checks gcd(b, a) as well. */
static void check(const char *set, uint32_t a, uint32_t b, int symmetric)
{
    uint32_t g = gcd_u32(a, b);
    n_checked++;
    if (g != ref_euclid(a, b)) {
        report(set, a, b, g, "differs from Euclid");
        return;
    }
    if (symmetric && gcd_u32(b, a) != g) {
        report(set, a, b, g, "gcd(b, a) differs");
        return;
    }
    if (g) {
        if (a % g != 0 || b % g != 0)
            report(set, a, b, g, "does not divide both operands");
        else if (ref_euclid(a / g, b / g) != 1)
            report(set, a, b, g, "cofactors are not coprime");
    } else if (a != 0 || b != 0) {
        report(set, a, b, g, "zero result for nonzero operands");
    }
}

static uint64_t run_exhaustive(void)
{
    uint64_t start = n_checked;
    for (uint32_t a = 0; a < (1u << 12); a++) {
        for (uint32_t b = 0; b < (1u << 12); b++)
            check("exhaustive", a, b, 0);
    }
    return n_checked - start;
}

static const uint32_t edges[] = {
    0u,          1u,          2u,          3u,          0x7FFFFFFFu,
    0x80000000u, 0x80000001u, 0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu,
    0x55555555u, 0xAAAAAAAAu, 0x0000FFFFu, 0xFFFF0000u, 0x00010001u,
};
#define N_EDGES (sizeof edges / sizeof edges[0])

/* Directed inputs from the design (v2 section 4.5 and its amendment). */
static const uint32_t directed[][2] = {
    {0xFFFFFFFFu, 1u},          {1u, 0xFFFFFFFFu},
    {0x80000000u, 1u},          {0x80000000u, 0x7FFFFFFFu},
    {0xFFFFFFFFu, 0xFFFFFFFEu}, {2971215073u, 1836311903u},
    {1836311903u, 1134903170u}, {0xFFFFFFFFu, 0xFFFFu},
    {0xDEADBEEFu, 0xCAFEBABEu}, {0x7FFFFFFFu, 0x40000000u},
    {0xAAAAAAAAu, 0x55555555u}, {0x00010001u, 0xFFFF0000u},
    {0x55555FFFu, 0xAAAAA001u}, {2u, 4u},
    {0x80000000u, 0x80000000u}, {0xFFFFFFFEu, 0xFFFFFFFCu},
    {6u, 0x60000000u},
};
#define N_DIRECTED (sizeof directed / sizeof directed[0])

static uint64_t run_structured(void)
{
    uint64_t start = n_checked;

    /* Odd bases: small odds, 2^m - 1, 2^m + 1, and a few bit patterns. */
    uint32_t odd[80];
    size_t n_odd = 0;
    odd[n_odd++] = 1u;
    odd[n_odd++] = 3u;
    odd[n_odd++] = 5u;
    odd[n_odd++] = 7u;
    for (int m = 2; m <= 32; m++)
        odd[n_odd++] = (uint32_t) ((1ull << m) - 1u);
    for (int m = 1; m <= 31; m++)
        odd[n_odd++] = (1u << m) + 1u;
    odd[n_odd++] = 0x55555555u;
    odd[n_odd++] = 0xAAAAAAABu;
    odd[n_odd++] = 0x55555FFFu;
    odd[n_odd++] = 0xAAAAA001u;
    odd[n_odd++] = 2971215073u;
    odd[n_odd++] = 1836311903u;

    /* (u << i, v << j) whenever both fit in 32 bits. */
    for (size_t p = 0; p < n_odd; p++) {
        for (size_t q = 0; q < n_odd; q++) {
            for (int i = 0; i < 32; i++) {
                if (odd[p] > (UINT32_MAX >> i))
                    break;
                for (int j = 0; j < 32; j++) {
                    if (odd[q] > (UINT32_MAX >> j))
                        break;
                    check("structured", odd[p] << i, odd[q] << j, 0);
                }
            }
        }
    }

    /* Boundary values against each other and against their neighbours. */
    for (size_t p = 0; p < N_EDGES; p++) {
        for (size_t q = 0; q < N_EDGES; q++)
            check("structured", edges[p], edges[q], 1);
        for (uint32_t d = 1; d <= 2; d++) {
            check("structured", edges[p], edges[p] + d, 1);
            check("structured", edges[p], edges[p] - d, 1);
        }
    }

    /* Fibonacci numbers up to F(47), the largest below 2^32. */
    uint32_t f[48];
    f[0] = 0;
    f[1] = 1;
    for (int i = 2; i < 48; i++)
        f[i] = f[i - 1] + f[i - 2];
    for (int n = 0; n < 48; n++) {
        for (int m = 0; m < 48; m++)
            check("structured", f[n], f[m], 0);
    }

    for (size_t i = 0; i < N_DIRECTED; i++)
        check("structured", directed[i][0], directed[i][1], 1);

    return n_checked - start;
}

static uint32_t xorshift32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Operand lengths are random: shift a full random word right by 0..31. */
static uint64_t run_random(uint64_t count)
{
    uint64_t start = n_checked;
    uint32_t s = RANDOM_SEED;
    for (uint64_t n = 0; n < count; n++) {
        uint32_t r = xorshift32(&s);
        uint32_t a = xorshift32(&s) >> (r & 31);
        uint32_t b = xorshift32(&s) >> ((r >> 5) & 31);
        check("random", a, b, 1);
    }
    return n_checked - start;
}

int main(int argc, char **argv)
{
    uint64_t count = 10000000u;
    if (argc > 1) {
        char *end;
        count = strtoull(argv[1], &end, 10);
        if (*end != '\0') {
            fprintf(stderr, "usage: %s [random-count]\n", argv[0]);
            return 2;
        }
    }

    uint64_t ne = run_exhaustive();
    printf("exhaustive: %" PRIu64 " pairs\n", ne);
    uint64_t ns = run_structured();
    printf("structured: %" PRIu64 " pairs\n", ns);
    uint64_t nr = run_random(count);
    printf("random: %" PRIu64 " pairs, xorshift32 seed 0x%08" PRIX32 "\n", nr,
           (uint32_t) RANDOM_SEED);

    printf("checked %" PRIu64 ", failed %" PRIu64 "\n", n_checked, n_failed);
    printf("RESULT: %s\n", n_failed ? "FAIL" : "PASS");
    return n_failed ? 1 : 0;
}
