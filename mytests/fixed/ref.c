/* Host reference check for op1, op2, op3 and q_mul (src/fixed/fixed.c, compiled
 * unmodified).
 *
 * The references use host arithmetic that the routines under test cannot:
 * q_mul is an exact int64_t product, the op1 sum is the machine addition with
 * the pass count from a separate loop, and op3 is a 64-bit addition.
 *
 * Sets:
 *   public      the operands and golden values of tests/fixedvec.h
 *   grid        the 64 x 64 operand grid of tests/fixedvec.h
 *   boundary    INT32_MIN, INT32_MAX, 0, +-1, +-1.0, +-0.5 and neighbours
 *   ties        rounding ties (and near ties) of q_mul
 *   chains      long carry chains of op1
 *   bitlen      fixed-seed pairs of random bit length and sign
 *   random      fixed-seed uniform 32-bit pairs
 *
 * Build and run: bash mytests/fixed/run.sh <output-dir> [random-count]
 * Usage of the binary: ref [random-count] [directed-dump-path]
 * The optional dump lists every directed input and result as hex, one per
 * line, for mytests/fixed/check_directed.py.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/fixed/fixed.h"
#include "tests/fixedvec.h"

#define RANDOM_SEED UINT64_C(0x9E3779B97F4A7C15)
#define BITLEN_SEED UINT64_C(0xD1B54A32D192ED03)
#define BITLEN_COUNT 1000000u
#define MAX_REPORTS 10

static const char *set_name;
static uint64_t set_inputs, set_errors, n_inputs, n_errors;
static FILE *dump;

static int ref_passes(uint32_t a, uint32_t b)
{
    int n;

    for (n = 0; b != 0; n++) {
        uint32_t c = a & b;
        a ^= b;
        b = c << 1;
    }
    return n;
}

static uint32_t ref_avg(uint32_t a, uint32_t b)
{
    return (uint32_t) (((uint64_t) a + b) >> 1);
}

static uint32_t ref_qmul(uint32_t a, uint32_t b)
{
    int64_t p = (int64_t) (int32_t) a * (int64_t) (int32_t) b;

    p += INT64_C(1) << 19;
    return (uint32_t) (p >> 20);
}

static void fail(const char *fn, uint32_t a, uint32_t b, uint32_t got)
{
    set_errors++;
    n_errors++;
    if (n_errors <= MAX_REPORTS)
        printf("FAIL %s %s(0x%08" PRIX32 ", 0x%08" PRIX32 ") = 0x%08" PRIX32
               "\n",
               set_name, fn, a, b, got);
}

static void check(uint32_t a, uint32_t b)
{
    int n = -1;
    uint32_t sum = op1(a, b, &n);
    uint32_t diff = op2(a, b);
    uint32_t avg = op3(a, b);
    uint32_t q = (uint32_t) q_mul((int32_t) a, (int32_t) b);

    set_inputs++;
    n_inputs++;
    if (sum != a + b)
        fail("op1", a, b, sum);
    if (n != ref_passes(a, b))
        fail("op1.n", a, b, (uint32_t) n);
    if (diff != a - b)
        fail("op2", a, b, diff);
    if (avg != ref_avg(a, b))
        fail("op3", a, b, avg);
    if (q != ref_qmul(a, b))
        fail("q_mul", a, b, q);
    if (dump)
        fprintf(dump,
                "%08" PRIX32 " %08" PRIX32 " %08" PRIX32 " %d %08" PRIX32
                " %08" PRIX32 " %08" PRIX32 "\n",
                a, b, sum, n, diff, avg, q);
}

static void both(uint32_t a, uint32_t b)
{
    check(a, b);
    check(b, a);
}

static void begin_set(const char *name)
{
    set_name = name;
    set_inputs = 0;
    set_errors = 0;
}

static void end_set(void)
{
    printf("set=%-8s inputs=%" PRIu64 " errors=%" PRIu64 "\n", set_name,
           set_inputs, set_errors);
}

static void run_public(void)
{
    begin_set("public");
    for (int i = 0; i < OP1_NCASES; i++) {
        const uint32_t *c = &op1_cases[4 * i];
        int n = -1;
        uint32_t s = op1(c[0], c[1], &n);
        if (s != c[2])
            fail("op1", c[0], c[1], s);
        if (n != (int) c[3])
            fail("op1.n", c[0], c[1], (uint32_t) n);
        check(c[0], c[1]);
    }
    for (int i = 0; i < QMUL_NCASES; i++) {
        const uint32_t *c = &qmul_cases[3 * i];
        uint32_t q = (uint32_t) q_mul((int32_t) c[0], (int32_t) c[1]);
        if (q != c[2])
            fail("q_mul", c[0], c[1], q);
        check(c[0], c[1]);
    }
    end_set();
}

static void run_grid(void)
{
    begin_set("grid");
    for (int i = 0; i < 64; i++) {
        for (int j = 0; j < 64; j++)
            check(op_pts[i], op_pts[j]);
    }
    end_set();
}

static void run_boundary(void)
{
    static const uint32_t v[] = {
        0u,          1u,          2u,          3u,          0xFFFFFFFFu,
        0xFFFFFFFEu, 0xFFFFFFFDu, 0x7FFFFFFFu, 0x7FFFFFFEu, 0x7FFFFFFDu,
        0x80000000u, 0x80000001u, 0x80000002u, 0x00100000u, 0xFFF00000u,
        0x000FFFFFu, 0x00100001u, 0xFFEFFFFFu, 0xFFF00001u, 0x00080000u,
        0xFFF80000u, 0x0007FFFFu, 0x00080001u, 0xFFF7FFFFu, 0xFFF80001u,
        0x0000FFFFu, 0x00010000u, 0xFFFF0000u,
    };
    size_t count = sizeof v / sizeof v[0];

    begin_set("boundary");
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++)
            check(v[i], v[j]);
    }
    end_set();
}

static void run_ties(void)
{
    const uint32_t half = 1u << 19;

    begin_set("ties");
    for (int32_t k = -4096; k <= 4096; k++) {
        uint32_t t = (uint32_t) k * half;
        for (int32_t d = -1; d <= 1; d++) { /* the tie and its neighbors */
            uint32_t x = t + (uint32_t) d;
            both(x, 1u);
            both(x, 0xFFFFFFFFu);
        }
    }
    for (int32_t odd = -8191; odd <= 8191; odd += 2) {
        both((uint32_t) odd, half);
        both((uint32_t) odd, 0u - half);
    }
    for (unsigned m = 0; m <= 19; m++) {
        for (int32_t odd = -255; odd <= 255; odd += 2) {
            uint32_t x = (uint32_t) odd << m, y = 1u << (19 - m);
            both(x, y);
            both(x, 0u - y);
        }
    }
    end_set();
}

static void run_chains(void)
{
    begin_set("chains");
    for (unsigned k = 0; k <= 32; k++) {
        uint32_t v = k == 32 ? 0xFFFFFFFFu : (1u << k) - 1u;
        both(v, 1u);
        both(v, v);
        both(v, v + 1u);
    }
    for (unsigned k = 0; k <= 31; k++)
        both(0xFFFFFFFFu, 1u << k);
    end_set();
}

static uint64_t xs64(uint64_t *s)
{
    uint64_t x = *s;

    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

static void run_bitlen(void)
{
    uint64_t s = BITLEN_SEED;

    begin_set("bitlen");
    for (uint32_t i = 0; i < BITLEN_COUNT; i++) {
        uint64_t r = xs64(&s), v = xs64(&s);
        uint32_t a = (uint32_t) v >> (r & 31);
        uint32_t b = (uint32_t) (v >> 32) >> ((r >> 8) & 31);
        if (r & (UINT64_C(1) << 16))
            a = 0u - a;
        if (r & (UINT64_C(1) << 17))
            b = 0u - b;
        check(a, b);
    }
    end_set();
}

static void run_random(uint64_t count)
{
    uint64_t s = RANDOM_SEED;

    begin_set("random");
    for (uint64_t i = 0; i < count; i++) {
        uint64_t v = xs64(&s);
        check((uint32_t) v, (uint32_t) (v >> 32));
    }
    end_set();
}

int main(int argc, char **argv)
{
    uint64_t count = 10000000u;

    if (argc > 1)
        count = strtoull(argv[1], NULL, 0);
    if (argc > 2) {
        dump = fopen(argv[2], "w");
        if (!dump) {
            perror(argv[2]);
            return 2;
        }
    }

    run_public();
    run_grid();
    run_boundary();
    run_ties();
    run_chains();
    if (dump) {
        fclose(dump);
        dump = NULL;
    }
    run_bitlen();
    run_random(count);

    printf("total inputs=%" PRIu64 " errors=%" PRIu64 "\n", n_inputs, n_errors);
    return n_errors ? 1 : 0;
}
