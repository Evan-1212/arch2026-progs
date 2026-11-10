/* Target check for bf16_from_f32, bf16_mul and bf16_sqrt on rv32emu.
 *
 * Links against the objects the repository already built (the harness runtime,
 * the C object of one optimization level and the assembly object) and prints:
 *
 *   @@cal      used instructions of known assembly paths, to calibrate the
 *              caller constant c0 of each call site of this ELF and to
 *              cross-check it on other paths
 *   @@m        used instructions of the assembly and of the C version for
 *              every model input (sqrt: all 65,536 inputs)
 *   @@bench    per-set timing in the format of common/bench.h
 *   @@cons     assembly, C and an independent reference on all model inputs
 *              and on fixed-seed random inputs
 *
 * mytests/bf16/target/analyze.py checks the output. Build and run:
 *   bash mytests/bf16/target/run.sh <output-dir>
 *
 * The calibration, model and timing phases run first, while the low word of
 * instret cannot wrap between two reads.
 */
#include <stddef.h>
#include <stdint.h>

#include "bench.h"
#include "print.h"
#include "src/bf16/bf16.h"
#include "tperf.h"

typedef uint16_t (*f32_fn)(uint32_t);
typedef uint16_t (*mul_fn)(uint16_t, uint16_t);
typedef uint16_t (*sqrt_fn)(uint16_t);

#define N_SPEED 1024u
#define N_RANDOM 1000000u
#define N_MODEL_RANDOM 4096u
#define MAX_REPORTS 10u

static uint32_t set_in[N_SPEED];
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
__attribute__((noinline, noclone)) static uint64_t measure_f32(f32_fn f,
                                                               uint32_t x)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f(x));
    BENCH_KEEP(g);
    return used;
}

__attribute__((noinline, noclone)) static uint64_t measure_mul(mul_fn f,
                                                               uint32_t a,
                                                               uint32_t b)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f((uint16_t) a, (uint16_t) b));
    BENCH_KEEP(g);
    return used;
}

__attribute__((noinline, noclone)) static uint64_t measure_sqrt(sqrt_fn f,
                                                                uint32_t a)
{
    uint64_t used;
    uint32_t g;
    PERF_CALL(used, g = f((uint16_t) a));
    BENCH_KEEP(g);
    return used;
}

/* Calibration paths. The first entry of each list is the primary one (the
 * paths named in the design, section 7); the others cross-check the c0 it
 * gives.
 */
static void calibrate(void)
{
    static const uint32_t f32_x[] = {0x7F800000u, 0xFFC00001u, 0x3F800000u,
                                     0x00000000u};
    static const uint16_t mul_ab[][2] = {
        {0x7F80, 0x3F80}, {0x7FC1, 0x3F80}, {0x0000, 0x3F80},
        {0x3F80, 0x3F80}, {0x7F80, 0x0000}, {0x3F80, 0x7FC0},
    };
    static const uint16_t sqrt_a[] = {0x0000, 0x0080, 0x7F80, 0x8000,
                                      0xFFC0, 0xBF80, 0x0040};

    for (int r = 0; r < 4; r++) {
        for (size_t i = 0; i < sizeof f32_x / sizeof f32_x[0]; i++)
            bm_printf(
                "@@cal site=f32 x=%x used=%llu\n", f32_x[i],
                (unsigned long long) measure_f32(bf16_from_f32_asm, f32_x[i]));
        for (size_t i = 0; i < sizeof mul_ab / sizeof mul_ab[0]; i++)
            bm_printf("@@cal site=mul a=%x b=%x used=%llu\n", mul_ab[i][0],
                      mul_ab[i][1],
                      (unsigned long long) measure_mul(
                          bf16_mul_asm, mul_ab[i][0], mul_ab[i][1]));
        for (size_t i = 0; i < sizeof sqrt_a / sizeof sqrt_a[0]; i++)
            bm_printf(
                "@@cal site=sqrt a=%x used=%llu\n", sqrt_a[i],
                (unsigned long long) measure_sqrt(bf16_sqrt_asm, sqrt_a[i]));
    }
}

static void model_f32(uint32_t x)
{
    uint64_t s = measure_f32(bf16_from_f32_asm, x);
    uint64_t c = measure_f32(bf16_from_f32, x);
    bm_printf("@@m f32 %x %llu %llu\n", x, (unsigned long long) s,
              (unsigned long long) c);
}

static void model_mul(uint32_t a, uint32_t b)
{
    uint64_t s = measure_mul(bf16_mul_asm, a, b);
    uint64_t c = measure_mul(bf16_mul, a, b);
    bm_printf("@@m mul %x %x %llu %llu\n", a, b, (unsigned long long) s,
              (unsigned long long) c);
}

static void model_sqrt(uint32_t a)
{
    uint64_t s = measure_sqrt(bf16_sqrt_asm, a);
    uint64_t c = measure_sqrt(bf16_sqrt, a);
    bm_printf("@@m sqrt %x %llu %llu\n", a, (unsigned long long) s,
              (unsigned long long) c);
}

/* References used only by the consistency phase. They share no code with the
 * routines under test and follow mytests/bf16/ref.c (checked on the host
 * against the golden data), except that sqrt walks the squares downward by
 * subtraction instead of multiplying.
 */
static uint16_t ref_f32(uint32_t x)
{
    uint32_t hi = x >> 16, lo = x & 0xFFFFu;

    if ((x & 0x7F800000u) == 0x7F800000u)
        return (uint16_t) hi;
    if (lo > 0x8000u || (lo == 0x8000u && (hi & 1u)))
        hi++;
    return (uint16_t) hi;
}

static int is_zero(uint16_t x)
{
    return (x & 0x7FFFu) == 0;
}

static uint16_t ref_mul(uint16_t a, uint16_t b)
{
    const uint16_t sign = (uint16_t) ((a ^ b) & 0x8000u);
    unsigned fa = (a >> 7) & 0xFFu, fb = (b >> 7) & 0xFFu;

    if (fa == 0xFFu) {
        if (a & 0x7Fu)
            return a;
        return is_zero(b) ? 0x7FC0u : (uint16_t) (sign | 0x7F80u);
    }
    if (fb == 0xFFu) {
        if (b & 0x7Fu)
            return b;
        return is_zero(a) ? 0x7FC0u : (uint16_t) (sign | 0x7F80u);
    }
    if (is_zero(a) || is_zero(b))
        return sign;

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
    unsigned field = (a >> 7) & 0xFFu;

    if (field == 0xFFu && ((a & 0x7Fu) || !(a & 0x8000u)))
        return a;
    if ((a & 0x8000u) && !is_zero(a))
        return 0x7FC0u;
    if (field == 0)
        return 0;

    int e = (int) field - 127;
    uint32_t m = 0x80u | (a & 0x7Fu);
    int odd = (field & 1u) == 0;
    if (odd)
        m <<= 1;
    int new_exp = 127 + (odd ? (e - 1) : e) / 2;

    /* r = 256 down to 90, sq = r * r kept by subtracting 2r - 1. */
    uint32_t r = 256, sq = 65536;
    while (r > 90u && (sq >> 7) > m) {
        sq -= 2u * r - 1u;
        r--;
    }
    if ((sq >> 7) > m)
        r = 128;
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

static uint32_t n_compared, n_mismatch;

static void mismatch(const char *what, uint32_t a, uint32_t b)
{
    n_mismatch++;
    if (n_mismatch <= MAX_REPORTS)
        bm_printf("@@diff %s a=%x b=%x\n", what, a, b);
}

static void cons_f32(uint32_t x)
{
    uint16_t r = ref_f32(x);
    n_compared++;
    if (bf16_from_f32(x) != r)
        mismatch("f32_c", x, 0);
    if (bf16_from_f32_asm(x) != r)
        mismatch("f32_asm", x, 0);
}

static void cons_mul(uint32_t a, uint32_t b)
{
    uint16_t r = ref_mul((uint16_t) a, (uint16_t) b);
    n_compared++;
    if (bf16_mul((uint16_t) a, (uint16_t) b) != r)
        mismatch("mul_c", a, b);
    if (bf16_mul_asm((uint16_t) a, (uint16_t) b) != r)
        mismatch("mul_asm", a, b);
}

static void cons_sqrt(uint32_t a)
{
    uint16_t r = ref_sqrt((uint16_t) a);
    n_compared++;
    if (bf16_sqrt((uint16_t) a) != r)
        mismatch("sqrt_c", a, 0);
    if (bf16_sqrt_asm((uint16_t) a) != r)
        mismatch("sqrt_asm", a, 0);
}

/* Representatives of every class: zeros, denormals, normals, the largest
 * finite values, Inf and NaN, with both signs.
 */
static const uint16_t reps[] = {
    0x0000, 0x8000, 0x0001, 0x0040, 0x007F, 0x8001, 0x807F,
    0x0080, 0x3F80, 0xBF80, 0x7F7F, 0xFF7F, 0x4049, 0x7F80,
    0xFF80, 0x7FC0, 0x7F81, 0xFFC0, 0xFFFF,
};
#define N_REPS (sizeof reps / sizeof reps[0])

static const uint16_t fields[] = {1,   2,   3,   4,   5,   6,   7,
                                  8,   63,  64,  65,  120, 121, 126,
                                  127, 128, 190, 191, 192, 253, 254};
#define N_FIELDS (sizeof fields / sizeof fields[0])

static const uint16_t partners[] = {0x3F80, 0x3FFF, 0x4000, 0x7F00,
                                    0x43D5, 0x3F00, 0x0080, 0x4B7F};
#define N_PARTNERS (sizeof partners / sizeof partners[0])

/* The directed mul inputs of the design: every branch of section 5.3, every
 * loop length, every normalization length, every result exit.
 */
static void directed_mul(void (*cb)(uint32_t, uint32_t))
{
    for (size_t i = 0; i < N_REPS; i++) {
        for (size_t j = 0; j < N_REPS; j++)
            cb(reps[i], reps[j]);
    }
    /* The longest path of the design, section 5.3: a = 0x0001 takes all
     * seven normalization steps, b has all seven fraction bits set, and the
     * result is a nonzero denormal. Body 97, the static bound.
     */
    cb(0x0001u, 0x407Fu);
    /* All 128 fractions of b against four significands of a. */
    static const uint16_t as[] = {0x3F80, 0x3FFF, 0x3FC0, 0xC049};
    for (size_t i = 0; i < 4; i++) {
        for (uint32_t f = 0; f < 128; f++) {
            cb(as[i], 0x3F80u | f);
            cb(as[i], 0xBF00u | f);
        }
    }
    /* Exponent sums around both ends of the range. */
    for (size_t i = 0; i < N_FIELDS; i++) {
        for (size_t j = 0; j < N_FIELDS; j++) {
            cb(((uint32_t) fields[i] << 7) | 0x55u,
               ((uint32_t) fields[j] << 7) | 0x2Au);
            cb(((uint32_t) fields[i] << 7) | 0x7Fu,
               0x8000u | ((uint32_t) fields[j] << 7) | 0x7Fu);
        }
    }
    /* Denormal operands with leading one at bit j: k = 7 - j steps. */
    for (uint32_t j = 0; j < 7; j++) {
        uint32_t d[2] = {1u << j, (2u << j) - 1u};
        for (int v = 0; v < 2; v++) {
            for (size_t p = 0; p < N_PARTNERS; p++) {
                cb(d[v], partners[p]);
                cb(partners[p], d[v]);
                cb(d[v] | 0x8000u, partners[p]);
            }
            for (uint32_t j2 = 0; j2 < 7; j2++) {
                cb(d[v], 1u << j2);
                cb(d[v], (2u << j2) - 1u);
            }
        }
    }
}

static void directed_f32(void (*cb)(uint32_t))
{
    static const uint32_t xs_[] = {
        0x00000000u, 0x80000000u, 0x00010000u, 0x00008000u, 0x00018000u,
        0x00017FFFu, 0x3F800000u, 0x3F808000u, 0x3F818000u, 0x3F807FFFu,
        0x3F80FFFFu, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F7F7FFFu, 0x7F800000u,
        0xFF800000u, 0x7F800001u, 0x7FC00000u, 0xFFC00001u, 0x7FFFFFFFu,
        0xFFFFFFFFu, 0x40490FDBu, 0x007FFFFFu, 0x00800000u,
    };
    for (size_t i = 0; i < sizeof xs_ / sizeof xs_[0]; i++)
        cb(xs_[i]);
}

/* Input sets of the design, section 7 (A6). Each fills set_in[]; mul packs
 * a << 16 | b as tests/mbf16.c does.
 */

static uint32_t normal16(uint32_t r)
{
    uint32_t field = 64u + ((r >> 7) & 0x7Fu); /* 64 to 191 */
    return (r & 0x8000u) | (field << 7) | (r & 0x7Fu);
}

static void gen_mul_s1(void)
{
    for (unsigned i = 0; i < N_SPEED; i++)
        set_in[i] = (normal16(xs()) << 16) | normal16(xs());
}

static uint32_t denormal16(uint32_t r)
{
    return (r & 0x8000u) | (r & 0x7Fu) | 1u;
}

static void gen_mul_s2(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs(), s = xs();
        uint32_t d = denormal16(r);
        uint32_t n = (i & 3) == 3 ? denormal16(s) : normal16(s);
        set_in[i] = (i & 1) ? (n << 16) | d : (d << 16) | n;
    }
}

static const uint16_t specials[] = {0x0000, 0x8000, 0x7F80, 0xFF80,
                                    0x7FC0, 0xFFC0, 0x7F81, 0xFFFF};

static void gen_mul_s3(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t s = specials[r & 7u];
        uint32_t o = (r & 0x80u) ? specials[(r >> 3) & 7u] : normal16(xs());
        set_in[i] = (r & 0x40u) ? (o << 16) | s : (s << 16) | o;
    }
}

static void gen_mul_s4(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs(), s = xs();
        uint32_t fa, fb;
        if (i & 1) {
            fa = 192u + (r & 0x3Fu); /* 192 to 255; 255 maps below */
            fb = 192u + ((r >> 6) & 0x3Fu);
        } else {
            fa = 1u + (r & 0x3Fu); /* 1 to 64 */
            fb = 1u + ((r >> 6) & 0x3Fu);
        }
        if (fa == 255u)
            fa = 254u;
        if (fb == 255u)
            fb = 254u;
        uint32_t a = (s & 0x8000u) | (fa << 7) | (s & 0x7Fu);
        uint32_t b = ((s >> 1) & 0x8000u) | (fb << 7) | ((s >> 8) & 0x7Fu);
        set_in[i] = (a << 16) | b;
    }
}

static void gen_mul_s5(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t b = normal16(xs()) & 0xFF80u;
        set_in[i] = (normal16(xs()) << 16) | b;
    }
}

static void gen_sqrt_s6(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t field = (r & 0xFEu) | 1u; /* odd field: even exponent */
        if (field == 255u)
            field = 253u;
        set_in[i] = (field << 7) | ((r >> 8) & 0x7Fu);
    }
}

static void gen_sqrt_s7(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t field = r & 0xFEu; /* even field: odd exponent */
        if (field == 0u)
            field = 2u;
        set_in[i] = (field << 7) | ((r >> 8) & 0x7Fu);
    }
}

static void gen_sqrt_s8(void)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        uint32_t f = (r >> 8) & 0x7Fu;
        uint32_t v;
        switch (r & 7u) {
        case 0:
            v = r & 0x8000u; /* +0 or -0 */
            break;
        case 1:
            v = f | 1u; /* positive denormal */
            break;
        case 2:
            v = 0x8000u | normal16(r >> 3); /* negative normal */
            break;
        case 3:
            v = 0xFF80u;
            break;
        case 4:
            v = 0x7F80u;
            break;
        case 5:
            v = 0x7F80u | f | 1u;
            break;
        case 6:
            v = 0xFF80u | f | 1u;
            break;
        default:
            v = 0x8000u | f | 1u; /* negative denormal */
            break;
        }
        set_in[i] = v;
    }
}

static void gen_f32_s9(void)
{
    for (unsigned i = 0; i < N_SPEED; i++)
        set_in[i] = xs();
}

static void gen_f32_s10(void)
{
    static const uint32_t lows[4] = {0x7FFFu, 0x8000u, 0x8001u, 0x8000u};
    for (unsigned i = 0; i < N_SPEED; i++) {
        uint32_t r = xs();
        set_in[i] = (r & 0xFFFF0000u) | lows[i & 3u];
    }
}

__attribute__((noinline, noclone)) static void bench_f32(const char *name)
{
    BENCH("bf16", name, "c", set_in, N_SPEED, bf16_from_f32(in));
    BENCH("bf16", name, "asm", set_in, N_SPEED, bf16_from_f32_asm(in));
}

__attribute__((noinline, noclone)) static void bench_mul(const char *name)
{
    BENCH("bf16", name, "c", set_in, N_SPEED, bf16_mul(in >> 16, in & 0xffff));
    BENCH("bf16", name, "asm", set_in, N_SPEED,
          bf16_mul_asm(in >> 16, in & 0xffff));
}

__attribute__((noinline, noclone)) static void bench_sqrt(const char *name)
{
    BENCH("bf16", name, "c", set_in, N_SPEED, bf16_sqrt(in & 0xffff));
    BENCH("bf16", name, "asm", set_in, N_SPEED, bf16_sqrt_asm(in & 0xffff));
}

enum kind { K_F32, K_MUL, K_SQRT };

struct speed_set {
    const char *name;
    uint32_t seed;
    enum kind kind;
    void (*gen)(void);
};

static const struct speed_set sets[] = {
    {"mul_S1", 0x1BADB002u, K_MUL, gen_mul_s1},
    {"mul_S2", 0x0BADF00Du, K_MUL, gen_mul_s2},
    {"mul_S3", 0xC0FFEE11u, K_MUL, gen_mul_s3},
    {"mul_S4", 0x31415927u, K_MUL, gen_mul_s4},
    {"mul_S5", 0x27182818u, K_MUL, gen_mul_s5},
    {"sqrt_S6", 0x16180339u, K_SQRT, gen_sqrt_s6},
    {"sqrt_S7", 0x14142135u, K_SQRT, gen_sqrt_s7},
    {"sqrt_S8", 0x17320508u, K_SQRT, gen_sqrt_s8},
    {"f32_S9", 0x22360679u, K_F32, gen_f32_s9},
    {"f32_S10", 0x26457513u, K_F32, gen_f32_s10},
};
#define N_SETS (sizeof sets / sizeof sets[0])

static void model_set(enum kind k)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        if (k == K_F32)
            model_f32(set_in[i]);
        else if (k == K_MUL)
            model_mul(set_in[i] >> 16, set_in[i] & 0xFFFFu);
        else
            model_sqrt(set_in[i] & 0xFFFFu);
    }
}

static void cons_set(enum kind k)
{
    for (unsigned i = 0; i < N_SPEED; i++) {
        if (k == K_F32)
            cons_f32(set_in[i]);
        else if (k == K_MUL)
            cons_mul(set_in[i] >> 16, set_in[i] & 0xFFFFu);
        else
            cons_sqrt(set_in[i] & 0xFFFFu);
    }
}

static void model_random(void)
{
    xs_state = 0x600DF00Du;
    for (unsigned i = 0; i < N_MODEL_RANDOM; i++) {
        uint32_t r = xs();
        model_mul(r >> 16, r & 0xFFFFu);
    }
    for (unsigned i = 0; i < N_MODEL_RANDOM; i++)
        model_f32(xs());
}

int main(void)
{
    bm_printf("@@info opt=%s n_speed=%u sets=%u\n", OPT_NAME, N_SPEED,
              (unsigned) N_SETS);

    calibrate();
    directed_f32(model_f32);
    directed_mul(model_mul);
    for (uint32_t a = 0; a < 0x10000u; a++)
        model_sqrt(a);
    model_random();
    for (size_t s = 0; s < N_SETS; s++) {
        xs_state = sets[s].seed;
        sets[s].gen();
        model_set(sets[s].kind);
        if (sets[s].kind == K_F32)
            bench_f32(sets[s].name);
        else if (sets[s].kind == K_MUL)
            bench_mul(sets[s].name);
        else
            bench_sqrt(sets[s].name);
    }

    directed_f32(cons_f32);
    directed_mul(cons_mul);
    for (uint32_t a = 0; a < 0x10000u; a++)
        cons_sqrt(a);
    uint32_t nd = n_compared;
    for (size_t s = 0; s < N_SETS; s++) {
        xs_state = sets[s].seed;
        sets[s].gen();
        cons_set(sets[s].kind);
    }
    uint32_t ns = n_compared - nd;
    xs_state = 0x9E3779B9u;
    for (uint32_t n = 0; n < N_RANDOM; n++) {
        uint32_t r = xs();
        cons_mul(r >> 16, r & 0xFFFFu);
        cons_f32(xs());
    }
    bm_printf("@@cons directed=%u sets=%u random=%u total=%u mismatches=%u\n",
              nd, ns, 2u * N_RANDOM, n_compared, n_mismatch);
    return n_mismatch ? 1 : 0;
}
