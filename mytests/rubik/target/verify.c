/* rv32emu check of rubik_solve_asm next to rubik_solve. The program only
 * records what the two functions did; mytests/rubik/target/judge.py and the
 * host reference (mytests/rubik/ref.c, mode replay) decide what is right.
 *
 *   @@v     one line per state of verify_inputs.h (written by verify.sh):
 *           both return values, the instructions each call retired (PERF_CALL
 *           of tests/tperf.h, as tests/trubik.c measures them), whether the
 *           two texts are the same, and the text of the assembly version
 *   @@inv   one line per invalid input: return value and the number of bytes
 *           of a 48-byte buffer that changed, for both versions
 *   @@cap   the cap rule on the tests/rubikvec.h states: return value and
 *           the buffer after a call with cap 96, length + 1, length, 1 and 0
 *
 * Linked with build/rubik/<O>/impl.o and build/rubik/impl_asm.o, the objects
 * the harness builds, so the code under test is the graded one.
 *
 * Build and run: bash mytests/rubik/target/verify.sh <new-output-dir>
 */
#include <stddef.h>
#include <stdint.h>
#include "print.h"
#include "rubikvec.h"
#include "tperf.h"

int rubik_solve(const char *state, char *out, size_t cap);
int rubik_solve_asm(const char *state, char *out, size_t cap);

#include "verify_inputs.h"

typedef int (*solve_fn)(const char *, char *, size_t);

enum { BUF = 96, DUMP = 40, SMALL = 48 };

static void fill(char *buf, unsigned n)
{
    for (unsigned k = 0; k < n; ++k)
        buf[k] = '#';
}

static unsigned changed(const char *buf, unsigned from, unsigned to)
{
    unsigned n = 0;
    for (unsigned k = from; k < to; ++k)
        n += buf[k] != '#';
    return n;
}

static void put_hex(const char *buf, unsigned n)
{
    static const char digit[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        unsigned b = (unsigned char) buf[i];
        bm_printf("%c%c", digit[b >> 4], digit[b & 15U]);
    }
}

static void check_states(void)
{
    static char a[BUF + 1], c[BUF + 1];
    for (unsigned i = 0; i < sizeof states / sizeof states[0]; ++i) {
        uint64_t ia, ic;
        int na, nc;
        fill(a, BUF);
        fill(c, BUF);
        PERF_CALL(ia, na = rubik_solve_asm(states[i], a, BUF));
        PERF_CALL(ic, nc = rubik_solve(states[i], c, BUF));
        int same = 1;
        for (unsigned k = 0; k < BUF; ++k)
            same &= a[k] == c[k];
        bm_printf("@@v %u %s na=%d nc=%d ia=%llu ic=%llu same=%d out=%s\n", i,
                  states[i], na, nc, (unsigned long long) ia,
                  (unsigned long long) ic, same, a);
    }
}

static void invalid_one(const char *tag, unsigned i, const char *state)
{
    static char a[SMALL], c[SMALL];
    fill(a, SMALL);
    fill(c, SMALL);
    int na = rubik_solve_asm(state, a, SMALL);
    int nc = rubik_solve(state, c, SMALL);
    bm_printf("@@inv %s %u na=%d ta=%u nc=%d tc=%u\n", tag, i, na,
              changed(a, 0, SMALL), nc, changed(c, 0, SMALL));
}

static void cap_case(unsigned i,
                     char tag,
                     solve_fn solve,
                     const char *state,
                     size_t cap)
{
    static char buf[BUF];
    fill(buf, BUF);
    int r = solve(state, buf, cap);
    bm_printf("@@cap %u impl=%c cap=%u ret=%d rest=%u buf=", i, tag,
              (unsigned) cap, r, changed(buf, DUMP, BUF));
    put_hex(buf, DUMP);
    bm_printf("\n");
}

static void cap_state(unsigned i, char tag, solve_fn solve, const char *state)
{
    static char full[BUF + 1];
    fill(full, BUF);
    int n = solve(state, full, BUF);
    size_t len = 0;
    while (len < BUF && full[len])
        ++len;
    cap_case(i, tag, solve, state, BUF);
    if (n < 0 || len >= DUMP)
        return; /* the judge reports the missing lines */
    cap_case(i, tag, solve, state, len + 1);
    if (len >= 1)
        cap_case(i, tag, solve, state, len);
    if (len >= 2)
        cap_case(i, tag, solve, state, 1);
    cap_case(i, tag, solve, state, 0);
}

int main(void)
{
    check_states();
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i)
        invalid_one("own", i, invalid[i]);
    for (unsigned i = 0; i < RUBIK_NINVALID; ++i)
        invalid_one("public", i, rubik_invalid[i]);
    for (unsigned i = 0; i < RUBIK_NCASES; ++i) {
        cap_state(i, 'a', rubik_solve_asm, rubik_cases[i].state);
        cap_state(i, 'c', rubik_solve, rubik_cases[i].state);
    }
    bm_printf("@@done states=%u invalid=%u public_invalid=%u cap_states=%u\n",
              (unsigned) (sizeof states / sizeof states[0]),
              (unsigned) (sizeof invalid / sizeof invalid[0]),
              (unsigned) RUBIK_NINVALID, (unsigned) RUBIK_NCASES);
    return 0;
}
