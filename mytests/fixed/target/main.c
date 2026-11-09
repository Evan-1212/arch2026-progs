/* Target check for op1, op2, op3 and q_mul on rv32emu.
 *
 * Links against the objects the repository already built (the harness runtime,
 * the C object of one optimization level and the assembly object) and prints:
 *
 *   @@cal      used instructions of known-length assembly paths, to calibrate
 *              the caller constant c0 of each call site of this ELF
 *   @@m        used instructions of one implementation for every model input
 *   @@bench    per-set timing in the format of common/bench.h
 *   @@cons     assembly, C and an independent reference on fixed-seed inputs
 *
 * mytests/fixed/target/analyze.py checks the output. Build and run:
 *   bash mytests/fixed/target/run.sh <output-dir>
 *
 * The calibration, model and timing phases run first, while the low word of
 * instret cannot wrap between two reads.
 */
#include <stddef.h>
#include <stdint.h>

#include "bench.h"
#include "print.h"
#include "src/fixed/fixed.h"
#include "tperf.h"

typedef uint32_t (*op1_fn)(uint32_t, uint32_t, int *);
typedef uint32_t (*u_fn)(uint32_t, uint32_t);
typedef int32_t (*q_fn)(int32_t, int32_t);

#define N_SPEED 1024u
#define N_RANDOM 1000000u
#define MAX_REPORTS 10u

static uint32_t set_a[N_SPEED], set_b[N_SPEED];
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

/* One call site per function type, not inlined or cloned, so that every
 * measurement through it has the same caller constant.
 */
static int last_n;

__attribute__((noinline, noclone)) static uint64_t measure_op1(op1_fn f,
                                                               uint32_t a,
                                                               uint32_t b)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f(a, b, &last_n));
    BENCH_KEEP(g);
    return used;
}

__attribute__((noinline, noclone)) static uint64_t measure_u(u_fn f,
                                                             uint32_t a,
                                                             uint32_t b)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f(a, b));
    BENCH_KEEP(g);
    return used;
}

__attribute__((noinline, noclone)) static uint64_t measure_q(q_fn f,
                                                             uint32_t a,
                                                             uint32_t b)
{
    uint64_t used;
    int32_t g;
    PERF_CALL(used, g = f((int32_t) a, (int32_t) b));
    BENCH_KEEP(g);
    return used;
}

static void calibrate(void)
{
    static const uint32_t v[] = {0u,          1u,          2u,         3u,
                                 0x80000000u, 0xFFFFFFFFu, 0x12345678u};
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) {
        for (size_t j = 0; j < sizeof v / sizeof v[0]; j++) {
            bm_printf("@@cal site=u fn=op2 used=%llu\n",
                      (unsigned long long) measure_u(op2_asm, v[i], v[j]));
            bm_printf("@@cal site=u fn=op3 used=%llu\n",
                      (unsigned long long) measure_u(op3_asm, v[i], v[j]));
        }
        bm_printf("@@cal site=op1 fn=op1 used=%llu\n",
                  (unsigned long long) measure_op1(op1_asm, v[i], 0));
    }
    for (int i = 0; i < 4; i++) {
        bm_printf("@@cal site=q fn=q_mul used=%llu\n",
                  (unsigned long long) measure_q(q_mul_asm, 0, 0));
    }
}

static void model_impl(const char *impl,
                       op1_fn f1,
                       u_fn f2,
                       u_fn f3,
                       q_fn fq,
                       uint32_t a,
                       uint32_t b)
{
    uint64_t u1 = measure_op1(f1, a, b);
    int n = last_n;
    uint64_t u2 = measure_u(f2, a, b);
    uint64_t u3 = measure_u(f3, a, b);
    uint64_t uq = measure_q(fq, a, b);
    bm_printf("@@m %s %u %u %llu %d %llu %llu %llu\n", impl, a, b,
              (unsigned long long) u1, n, (unsigned long long) u2,
              (unsigned long long) u3, (unsigned long long) uq);
}

static void model_pair(uint32_t a, uint32_t b)
{
    model_impl("asm", op1_asm, op2_asm, op3_asm, q_mul_asm, a, b);
    model_impl("c", op1, op2, op3, q_mul, a, b);
}

/* Reference used only by the consistency phase; it shares no code with the
 * routines under test.
 */
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
    return (a >> 1) + (b >> 1) + (a & b & 1u);
}

/* Exact product from 16-bit partial products of the magnitudes. */
static uint32_t ref_qmul(uint32_t ua, uint32_t ub)
{
    int neg = ((ua ^ ub) >> 31) != 0;
    uint32_t ma = ua >> 31 ? 0u - ua : ua;
    uint32_t mb = ub >> 31 ? 0u - ub : ub;
    uint32_t al = ma & 0xFFFFu, ah = ma >> 16;
    uint32_t bl = mb & 0xFFFFu, bh = mb >> 16;
    uint64_t mag = (uint64_t) (al * bl) + ((uint64_t) (al * bh) << 16) +
                   ((uint64_t) (ah * bl) << 16) + ((uint64_t) (ah * bh) << 32);
    uint64_t p = neg ? 0u - mag : mag;

    p += UINT64_C(1) << 19;
    return (uint32_t) (p >> 20);
}

static uint32_t n_compared, n_mismatch;

static void mismatch(const char *what, uint32_t a, uint32_t b)
{
    n_mismatch++;
    if (n_mismatch <= MAX_REPORTS)
        bm_printf("@@diff %s a=%u b=%u\n", what, a, b);
}

static void compare(uint32_t a, uint32_t b)
{
    int nc = -1, ns = -2;
    uint32_t c = op1(a, b, &nc), s = op1_asm(a, b, &ns);

    n_compared++;
    if (c != a + b)
        mismatch("op1_c", a, b);
    if (s != a + b)
        mismatch("op1_asm", a, b);
    if (nc != ref_passes(a, b))
        mismatch("op1n_c", a, b);
    if (ns != ref_passes(a, b))
        mismatch("op1n_asm", a, b);
    if (op2(a, b) != a - b)
        mismatch("op2_c", a, b);
    if (op2_asm(a, b) != a - b)
        mismatch("op2_asm", a, b);
    if (op3(a, b) != ref_avg(a, b))
        mismatch("op3_c", a, b);
    if (op3_asm(a, b) != ref_avg(a, b))
        mismatch("op3_asm", a, b);
    uint32_t r = ref_qmul(a, b);
    if ((uint32_t) q_mul((int32_t) a, (int32_t) b) != r)
        mismatch("q_c", a, b);
    if ((uint32_t) q_mul_asm((int32_t) a, (int32_t) b) != r)
        mismatch("q_asm", a, b);
}

static void both(void (*cb)(uint32_t, uint32_t), uint32_t a, uint32_t b)
{
    cb(a, b);
    cb(b, a);
}

/* The directed inputs of the design, section c, and the structured sets. */
static void directed(void (*cb)(uint32_t, uint32_t))
{
    static const uint32_t pts[] = {
        0u,          1u,          2u,          3u,          0xFFFFFFFFu,
        0xFFFFFFFEu, 0x7FFFFFFFu, 0x7FFFFFFEu, 0x7FFFFFFDu, 0x80000000u,
        0x80000001u, 0x80000002u, 0x00100000u, 0xFFF00000u, 0x00080000u,
        0xFFF80000u, 0x00180000u, 0xFFE80000u, 0x55555555u, 0xAAAAAAAAu,
        0xAAAAAAABu, 0x2AAAAAAAu, 0x6DB6DB6Du, 0x7F7F7F7Fu, 0x7FFF7FFFu,
        0x3FFFFFFFu, 0x40000000u, 0xC0000000u,
    };
    const size_t count = sizeof pts / sizeof pts[0];
    const uint32_t half = 1u << 19;

    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++)
            cb(pts[i], pts[j]);
    }
    for (unsigned k = 0; k <= 32; k++) {
        uint32_t v = k == 32 ? 0xFFFFFFFFu : (1u << k) - 1u;
        both(cb, v, 1u);
        both(cb, v, v + 1u);
    }
    for (unsigned k = 0; k <= 31; k++)
        both(cb, 0xFFFFFFFFu, 1u << k);
    for (int32_t k = -32; k <= 32; k++) {
        both(cb, (uint32_t) k * half, 1u);
        both(cb, (uint32_t) k * half, 0xFFFFFFFFu);
    }
    for (int32_t odd = -63; odd <= 63; odd += 2) {
        both(cb, (uint32_t) odd, half);
        both(cb, (uint32_t) odd, 0u - half);
    }
    for (unsigned m = 0; m <= 19; m++) {
        static const int32_t odds[] = {-3, -1, 1, 3};
        for (size_t i = 0; i < 4; i++) {
            uint32_t x = (uint32_t) odds[i] << m, y = 1u << (19 - m);
            both(cb, x, y);
            both(cb, x, 0u - y);
        }
    }
}

/* Input sets of the design, section e. */

static void gen_uniform(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        set_a[i] = xs();
        set_b[i] = xs();
    }
}

static void gen_op1_short(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        set_a[i] = xs();
        set_b[i] = (i & 3) ? xs() & 0xFFu : 0u;
    }
}

static void gen_op1_chain(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        unsigned k = (i >> 1) & 31; /* the run of ones is k + 1 bits long */
        uint32_t r = xs();
        if (i & 1) {
            set_a[i] = 0xFFFFFFFFu;
            set_b[i] = 1u << (r & 31);
        } else {
            uint32_t v = k == 31 ? 0xFFFFFFFFu : (2u << k) - 1u;
            set_a[i] = v ^ (r & 1u);
            set_b[i] = 1u;
        }
    }
}

static void gen_op1_sparse(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        set_a[i] = xs();
        set_b[i] = (1u << (r & 31)) | (1u << ((r >> 8) & 31));
    }
}

static const uint32_t edge[16] = {
    0u,          1u,          2u,          0xFFFFFFFFu,
    0xFFFFFFFEu, 0x7FFFFFFFu, 0x7FFFFFFEu, 0x80000000u,
    0x80000001u, 0x00100000u, 0xFFF00000u, 0x00080000u,
    0xFFF80000u, 0x55555555u, 0xAAAAAAAAu, 0x0000FFFFu,
};

static void gen_edges(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        set_a[i] = edge[i & 15];
        set_b[i] = edge[(i >> 4) & 15];
    }
}

static void gen_avg_high(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        if (i & 1) {
            set_a[i] = edge[(i >> 1) & 15];
            set_b[i] = edge[(i >> 5) & 15];
        } else {
            set_a[i] = xs() | 0x80000000u;
            set_b[i] = xs() | 0x80000000u;
        }
    }
}

static uint32_t apply_sign(uint32_t magnitude, uint32_t bit)
{
    return bit ? 0u - magnitude : magnitude;
}

static void gen_q_bitlen(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        set_a[i] = apply_sign((xs() >> (r & 31)) | 1u, r & 0x10000u);
        set_b[i] = apply_sign((xs() >> ((r >> 8) & 31)) | 1u, r & 0x20000u);
    }
}

static void gen_q_round(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        set_a[i] = apply_sign((r & 0x3FFFu) << 12, r & 0x10000u);
        set_b[i] = apply_sign((xs() & 0x3FFFu) << 12, r & 0x20000u);
    }
}

static void gen_q_small_full(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t small = apply_sign((r & 0xFFu) | 1u, r & 0x10000u);
        uint32_t full = xs();
        set_a[i] = (i & 1) ? small : full;
        set_b[i] = (i & 1) ? full : small;
    }
}

/* One of the bits 0 to 30; the runtime has no modulo. */
static uint32_t low_bit(uint32_t r)
{
    r &= 31;
    return 1u << (r == 31 ? 30 : r);
}

static void gen_q_dense(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t a = 0x7FFFFFFFu ^ low_bit(r);
        uint32_t b = 0x7FFFFFFFu ^ low_bit(r >> 8);
        if (i & 1) {
            a ^= low_bit(r >> 16);
            b ^= low_bit(r >> 24);
        }
        set_a[i] = apply_sign(a, r & 0x40u);
        set_b[i] = apply_sign(b, r & 0x4000u);
    }
}

static void gen_q_near_one(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t a = (1u << 20) + (r & 0xFFFu) - 0x800u;
        uint32_t b = (1u << 20) + ((r >> 12) & 0xFFFu) - 0x800u;
        set_a[i] = apply_sign(a, r & 0x1000000u);
        set_b[i] = apply_sign(b, r & 0x2000000u);
    }
}

/* op1 also reports its pass count; fold it in, as tests/mfixed.c does. */
static uint32_t run_op1(uint32_t a, uint32_t b)
{
    int n;
    uint32_t r = op1(a, b, &n);
    return r + (uint32_t) n;
}

static uint32_t run_op1_asm(uint32_t a, uint32_t b)
{
    int n;
    uint32_t r = op1_asm(a, b, &n);
    return r + (uint32_t) n;
}

/* The BENCH macro of common/bench.h with two operands taken from two arrays
 * at the same index. The baseline loop loads both operands and keeps them
 * alive, in place of the call; loop control, checksum and counter reads are
 * the same, and the two loops are subtracted exactly as BENCH does.
 */
#define BENCH2(name, impl, call)                                      \
    do {                                                              \
        uint32_t sink = 0, base = 0;                                  \
        struct bench_sample b0 = bench_now();                         \
        for (unsigned i_ = 0; i_ < N_SPEED; i_++) {                   \
            uint32_t x_ = set_a[i_], y_ = set_b[i_];                  \
            uint32_t r_ = x_;                                         \
            BENCH_KEEP(r_);                                           \
            BENCH_KEEP(y_);                                           \
            base += r_;                                               \
        }                                                             \
        struct bench_sample b1 = bench_now();                         \
        struct bench_sample t0 = bench_now();                         \
        for (unsigned i_ = 0; i_ < N_SPEED; i_++) {                   \
            uint32_t x_ = set_a[i_], y_ = set_b[i_];                  \
            uint32_t r_ = (uint32_t) (call);                          \
            BENCH_KEEP(r_);                                           \
            sink += r_;                                               \
        }                                                             \
        struct bench_sample t1 = bench_now();                         \
        BENCH_KEEP(sink);                                             \
        BENCH_KEEP(base);                                             \
        bench_report("fixed", (name), (impl), N_SPEED,                \
                     (struct bench_sample) {t1.cycles - t0.cycles,    \
                                            t1.instret - t0.instret}, \
                     (struct bench_sample) {b1.cycles - b0.cycles,    \
                                            b1.instret - b0.instret}, \
                     sink);                                           \
    } while (0)

__attribute__((noinline, noclone)) static void bench_op1(const char *name)
{
    BENCH2(name, "c", run_op1(x_, y_));
    BENCH2(name, "asm", run_op1_asm(x_, y_));
}

__attribute__((noinline, noclone)) static void bench_op2(const char *name)
{
    BENCH2(name, "c", op2(x_, y_));
    BENCH2(name, "asm", op2_asm(x_, y_));
}

__attribute__((noinline, noclone)) static void bench_op3(const char *name)
{
    BENCH2(name, "c", op3(x_, y_));
    BENCH2(name, "asm", op3_asm(x_, y_));
}

__attribute__((noinline, noclone)) static void bench_q(const char *name)
{
    BENCH2(name, "c", q_mul((int32_t) x_, (int32_t) y_));
    BENCH2(name, "asm", q_mul_asm((int32_t) x_, (int32_t) y_));
}

struct speed_set {
    const char *name;
    uint32_t seed;
    void (*gen)(void);
    void (*bench)(const char *);
};

static const struct speed_set sets[] = {
    {"op1_S1", 0x1BADB002u, gen_uniform, bench_op1},
    {"op1_S2", 0x0BADF00Du, gen_op1_short, bench_op1},
    {"op1_S3", 0xC0FFEE11u, gen_op1_chain, bench_op1},
    {"op1_S4", 0x31415927u, gen_op1_sparse, bench_op1},
    {"op2_S1", 0x27182818u, gen_uniform, bench_op2},
    {"op2_S2", 0x16180339u, gen_edges, bench_op2},
    {"op3_S1", 0x14142135u, gen_uniform, bench_op3},
    {"op3_S2", 0x17320508u, gen_avg_high, bench_op3},
    {"q_mul_S1", 0x22360679u, gen_uniform, bench_q},
    {"q_mul_S2", 0x26457513u, gen_q_bitlen, bench_q},
    {"q_mul_S3", 0x28284271u, gen_q_round, bench_q},
    {"q_mul_S4", 0x30000001u, gen_q_small_full, bench_q},
    {"q_mul_S5", 0x31622776u, gen_q_dense, bench_q},
    {"q_mul_S6", 0x33166247u, gen_q_near_one, bench_q},
};
#define N_SETS (sizeof sets / sizeof sets[0])

static void compare_random(void)
{
    xs_state = 0x9E3779B9u;
    for (uint32_t n = 0; n < N_RANDOM; n++) {
        uint32_t r = xs();
        uint32_t a = xs(), b = xs();
        if (n & 1) {
            a = apply_sign(a >> (r & 31), r & 0x10000u);
            b = apply_sign(b >> ((r >> 8) & 31), r & 0x20000u);
        }
        compare(a, b);
    }
}

int main(void)
{
    bm_printf("@@info opt=%s n_speed=%u sets=%u\n", OPT_NAME, N_SPEED,
              (unsigned) N_SETS);

    calibrate();
    directed(model_pair);
    for (size_t s = 0; s < N_SETS; s++) {
        xs_state = sets[s].seed;
        sets[s].gen();
        for (unsigned i = 0; i < N_SPEED; i++)
            model_pair(set_a[i], set_b[i]);
        sets[s].bench(sets[s].name);
    }

    directed(compare);
    uint32_t nd = n_compared;
    for (size_t s = 0; s < N_SETS; s++) {
        xs_state = sets[s].seed;
        sets[s].gen();
        for (unsigned i = 0; i < N_SPEED; i++)
            compare(set_a[i], set_b[i]);
    }
    uint32_t ns = n_compared - nd;
    compare_random();
    bm_printf("@@cons directed=%u sets=%u random=%u total=%u mismatches=%u\n",
              nd, ns, N_RANDOM, n_compared, n_mismatch);
    return n_mismatch ? 1 : 0;
}
