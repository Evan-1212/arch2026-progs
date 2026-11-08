/* Target check for gcd_u32 and gcd_u32_asm on rv32emu.
 *
 * Links against the objects the repository already built (the harness runtime,
 * the C object of one optimization level and the assembly object) and prints:
 *
 *   @@cal      used instructions of gcd(0, x) and gcd(x, 0), to calibrate the
 *              caller constant c0 of this ELF
 *   @@m        used instructions of the assembly for every model input
 *   @@bench    per-set timing in the format of common/bench.h
 *   @@cons     assembly against C on a large fixed-seed input set
 *
 * mytests/gcd/target/analyze.py checks the output. Build and run:
 *   bash mytests/gcd/target/run.sh <output-dir> [dir-of-gcd_model.py]
 *
 * The counters are read while the 32-bit low word of instret cannot wrap, so
 * the calibration, model and timing phases run first.
 */
#include <stddef.h>
#include <stdint.h>

#include "bench.h"
#include "print.h"
#include "src/gcd/gcd.h"
#include "tperf.h"

typedef uint32_t (*gcd_fn)(uint32_t, uint32_t);

#define MAX_DIRECTED 2048u
#define N_SPEED 1024u
#define N_RANDOM 1000000u

static uint32_t dir_a[MAX_DIRECTED], dir_b[MAX_DIRECTED];
static unsigned n_directed;

static uint32_t s1_a[N_SPEED], s1_b[N_SPEED];
static uint32_t s2_a[N_SPEED], s2_b[N_SPEED];
static uint32_t s3_a[N_SPEED], s3_b[N_SPEED];
static uint32_t len_a[N_SPEED], len_b[N_SPEED];

static uint32_t xs_state;

static uint32_t xs(void)
{
    uint32_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    xs_state = x;
    return x;
}

/* One call site, not inlined or cloned, so every measurement in this ELF has
 * the same caller constant c0.
 */
__attribute__((noinline, noclone)) static uint64_t measure(gcd_fn f,
                                                           uint32_t a,
                                                           uint32_t b)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f(a, b));
    BENCH_KEEP(g);
    return used;
}

static void add_directed(uint32_t a, uint32_t b)
{
    if (n_directed < MAX_DIRECTED) {
        dir_a[n_directed] = a;
        dir_b[n_directed] = b;
        n_directed++;
    }
}

static void build_directed(void)
{
    /* tests/tgcd.c check_perf, then the extra design inputs. */
    static const uint32_t pairs[][2] = {
        {0xFFFFFFFFu, 1},
        {1, 0xFFFFFFFFu},
        {0x80000000u, 1},
        {0x80000000u, 0x7FFFFFFFu},
        {0xFFFFFFFFu, 0xFFFFFFFEu},
        {2971215073u, 1836311903u},
        {1836311903u, 1134903170u},
        {0xFFFFFFFFu, 0xFFFFu},
        {0xDEADBEEFu, 0xCAFEBABEu},
        {0x7FFFFFFFu, 0x40000000u},
        {0xAAAAAAAAu, 0x55555555u},
        {0x00010001u, 0xFFFF0000u},
        {0x55555FFFu, 0xAAAAA001u},
        /* Shared powers of two: k >= 1. */
        {2, 4},
        {0x80000000u, 0x80000000u},
        {0xFFFFFFFEu, 0xFFFFFFFCu},
        {6, 0x60000000u},
        /* Zero operands and equal operands. */
        {0, 0},
        {0, 1},
        {1, 0},
        {0, 0xFFFFFFFFu},
        {0xFFFFFFFFu, 0},
        {1, 1},
        {0xFFFFFFFFu, 0xFFFFFFFFu},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof pairs[0]; i++)
        add_directed(pairs[i][0], pairs[i][1]);

    /* Adjacent Fibonacci numbers, in both orders. */
    uint32_t f0 = 1, f1 = 2;
    for (int i = 0; i < 46; i++) {
        add_directed(f0, f1);
        add_directed(f1, f0);
        uint32_t f2 = f0 + f1;
        f0 = f1;
        f1 = f2;
    }

    /* (u << i, v << j) over odd bases and shift amounts, when they fit. */
    static const uint32_t bases[] = {1, 3, 5, 0x55, 0xFFF, 0x1FFFF};
    static const int shifts[] = {0, 1, 3, 7, 15, 30};
    size_t nb = sizeof bases / sizeof bases[0];
    size_t ns = sizeof shifts / sizeof shifts[0];
    for (size_t p = 0; p < nb; p++) {
        for (size_t q = 0; q < nb; q++) {
            for (size_t i = 0; i < ns; i++) {
                for (size_t j = 0; j < ns; j++) {
                    if (bases[p] <= (UINT32_MAX >> shifts[i]) &&
                        bases[q] <= (UINT32_MAX >> shifts[j]))
                        add_directed(bases[p] << shifts[i],
                                     bases[q] << shifts[j]);
                }
            }
        }
    }
}

static void build_speed_sets(void)
{
    /* S1: full 32-bit random operands. */
    xs_state = 0x1BADB002u;
    for (unsigned i = 0; i < N_SPEED; i++) {
        s1_a[i] = xs();
        s1_b[i] = xs();
    }

    /* S2: operands below 2^16. */
    xs_state = 0x0BADF00Du;
    for (unsigned i = 0; i < N_SPEED; i++) {
        s2_a[i] = xs() & 0xFFFFu;
        s2_b[i] = xs() & 0xFFFFu;
    }

    /* S3: odd << i with i in 4..19 for both operands, so k >= 4. */
    xs_state = 0xC0FFEE11u;
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        int sa = 4 + (int) (r & 15);
        int sb = 4 + (int) ((r >> 4) & 15);
        s3_a[i] = ((xs() >> sa) | 1u) << sa;
        s3_b[i] = ((xs() >> sb) | 1u) << sb;
    }

    /* Extra model inputs: random bit length. */
    xs_state = 0x31415927u;
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        len_a[i] = xs() >> (r & 31);
        len_b[i] = xs() >> ((r >> 5) & 31);
    }
}

static void calibrate(const char *impl, gcd_fn f)
{
    static const uint32_t xs_cal[] = {1,           2,           3,
                                      0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    for (size_t i = 0; i < sizeof xs_cal / sizeof xs_cal[0]; i++) {
        uint32_t x = xs_cal[i];
        bm_printf("@@cal impl=%s kind=a0 x=%u used=%llu\n", impl, x,
                  (unsigned long long) measure(f, 0, x));
        bm_printf("@@cal impl=%s kind=b0 x=%u used=%llu\n", impl, x,
                  (unsigned long long) measure(f, x, 0));
    }
}

static void model_set(const uint32_t *a, const uint32_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        bm_printf("@@m %u %u %llu\n", a[i], b[i],
                  (unsigned long long) measure(gcd_u32_asm, a[i], b[i]));
    }
}

/* The BENCH macro of common/bench.h with two operands taken from two arrays
 * at the same index. The baseline loop loads both operands and keeps them
 * alive, in place of the call; loop control, checksum and counter reads are
 * the same, and the two loops are subtracted exactly as BENCH does.
 */
#define BENCH2(name, impl, a, b, n, fn)                               \
    do {                                                              \
        uint32_t sink = 0, base = 0;                                  \
        struct bench_sample b0 = bench_now();                         \
        for (unsigned i_ = 0; i_ < (n); i_++) {                       \
            uint32_t x_ = (a)[i_], y_ = (b)[i_];                      \
            uint32_t r_ = x_;                                         \
            BENCH_KEEP(r_);                                           \
            BENCH_KEEP(y_);                                           \
            base += r_;                                               \
        }                                                             \
        struct bench_sample b1 = bench_now();                         \
        struct bench_sample t0 = bench_now();                         \
        for (unsigned i_ = 0; i_ < (n); i_++) {                       \
            uint32_t x_ = (a)[i_], y_ = (b)[i_];                      \
            uint32_t r_ = fn(x_, y_);                                 \
            BENCH_KEEP(r_);                                           \
            sink += r_;                                               \
        }                                                             \
        struct bench_sample t1 = bench_now();                         \
        BENCH_KEEP(sink);                                             \
        BENCH_KEEP(base);                                             \
        bench_report("gcd", (name), (impl), (n),                      \
                     (struct bench_sample) {t1.cycles - t0.cycles,    \
                                            t1.instret - t0.instret}, \
                     (struct bench_sample) {b1.cycles - b0.cycles,    \
                                            b1.instret - b0.instret}, \
                     sink);                                           \
    } while (0)

__attribute__((noinline, noclone)) static void bench_set(const char *name,
                                                         const uint32_t *a,
                                                         const uint32_t *b,
                                                         unsigned n)
{
    BENCH2(name, "c", a, b, n, gcd_u32);
    BENCH2(name, "asm", a, b, n, gcd_u32_asm);
}

static uint32_t n_mismatch;

static void compare(uint32_t a, uint32_t b)
{
    uint32_t c = gcd_u32(a, b), s = gcd_u32_asm(a, b);
    if (c != s) {
        n_mismatch++;
        if (n_mismatch <= 10)
            bm_printf("@@diff a=%u b=%u c=%u asm=%u\n", a, b, c, s);
    }
}

static uint32_t compare_array(const uint32_t *a, const uint32_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        compare(a[i], b[i]);
    return n;
}

static uint32_t compare_random(void)
{
    xs_state = 0x9E3779B9u;
    for (uint32_t n = 0; n < N_RANDOM; n++) {
        uint32_t r = xs();
        uint32_t a = xs(), b = xs();
        if (n & 1) {
            a >>= r & 31;
            b >>= (r >> 5) & 31;
        }
        compare(a, b);
    }
    return N_RANDOM;
}

static uint32_t compare_structured(void)
{
    uint32_t odd[80];
    uint32_t count = 0;
    size_t n_odd = 0;
    odd[n_odd++] = 1u;
    odd[n_odd++] = 3u;
    odd[n_odd++] = 5u;
    odd[n_odd++] = 7u;
    for (int m = 2; m <= 32; m++)
        odd[n_odd++] = m == 32 ? UINT32_MAX : (1u << m) - 1u;
    for (int m = 1; m <= 31; m++)
        odd[n_odd++] = (1u << m) + 1u;
    odd[n_odd++] = 0x55555555u;
    odd[n_odd++] = 0xAAAAAAABu;
    odd[n_odd++] = 0x55555FFFu;
    odd[n_odd++] = 0xAAAAA001u;

    for (size_t p = 0; p < n_odd; p++) {
        for (size_t q = 0; q < n_odd; q++) {
            for (int i = 0; i < 32; i++) {
                if (odd[p] > (UINT32_MAX >> i))
                    break;
                for (int j = 0; j < 32; j++) {
                    if (odd[q] > (UINT32_MAX >> j))
                        break;
                    compare(odd[p] << i, odd[q] << j);
                    count++;
                }
            }
        }
    }
    return count;
}

int main(void)
{
    build_directed();
    build_speed_sets();
    bm_printf("@@info opt=%s directed=%u speed=%u\n", OPT_NAME, n_directed,
              N_SPEED);

    calibrate("asm", gcd_u32_asm);
    calibrate("c", gcd_u32);

    model_set(dir_a, dir_b, n_directed);
    model_set(s1_a, s1_b, N_SPEED);
    model_set(s2_a, s2_b, N_SPEED);
    model_set(s3_a, s3_b, N_SPEED);
    model_set(len_a, len_b, N_SPEED);

    bench_set("S1", s1_a, s1_b, N_SPEED);
    bench_set("S2", s2_a, s2_b, N_SPEED);
    bench_set("S3", s3_a, s3_b, N_SPEED);
    bench_set("S4", dir_a, dir_b, n_directed);

    uint32_t nd = compare_array(dir_a, dir_b, n_directed);
    uint32_t ns = compare_array(s1_a, s1_b, N_SPEED);
    ns += compare_array(s2_a, s2_b, N_SPEED);
    ns += compare_array(s3_a, s3_b, N_SPEED);
    uint32_t nr = compare_random();
    uint32_t nst = compare_structured();
    bm_printf(
        "@@cons directed=%u speed=%u random=%u structured=%u seed=0x9E3779B9 "
        "mismatches=%u\n",
        nd, ns, nr, nst, n_mismatch);
    return n_mismatch ? 1 : 0;
}
