/* Optimal 2x2x2 solver: iterative deepening A* (the HW1 v2 design).
 *
 * Coordinates (see mytests/rubik/gen_tables.c for the exact definitions):
 *   p  Lehmer rank of the permutation (0..5039), formed with fixed radices
 *      7, 6, 5, 4, 3, 2 by shifts and adds
 *   o  orientations of positions 0..5 in base 3 (0..728); position 6 follows
 *      from the sum rule
 * Each move is one to three clockwise quarter turns of a face, looked up in
 * v1_perm_next and v1_ori_next. The heuristic is the larger of two exact
 * distances of abstractions of the cube, so it never overestimates:
 *   v1_perm_dist[p]                 the permutation alone
 *   v2_packed[v2_projection[p], o]  positions of cubies 1, 2, 7 (input
 *                                   labels) and all orientations, 4 bits each
 * The search is depth first with a bound that grows from the heuristic of the
 * start state, so the first solution it finds is a shortest one. It keeps an
 * explicit stack of frames (no recursion) and never turns the face it just
 * turned. A frame keeps a cursor that is advanced one quarter turn per child,
 * so R, R2, R' cost three table lookups together. The faces are tried in the
 * order R, B, D and the turns in the order X, X2, X'; the output is the first
 * solution in that order.
 *
 * The tables are generated: bash mytests/rubik/gen.sh <dir> writes
 * rubik_tables.h (byte-identical to the HW1 tables). No table holds whole
 * cube states; the largest is the 76,650-byte pattern database.
 */
#include "rubik.h"
#include "rubik_tables.h"

enum { MAX_DEPTH = 11, NO_FACE = 3 };

typedef struct {
    uint16_t p, o, cp, co;
    uint8_t face, turn, prev;
} frame;
static frame frames[MAX_DEPTH + 1];

static unsigned smaller_after(const uint8_t p[7], unsigned i)
{
    unsigned n = 0;
    for (unsigned j = i + 1; j < 7; ++j)
        n += p[j] < p[i];
    return n;
}

/* Returns 1 and the coordinates for a valid state, 0 otherwise. Stops at the
 * first bad character, so it never reads past a NUL.
 */
static int parse(const char *in, unsigned *pp, unsigned *po)
{
    uint8_t p[7];
    unsigned seen = 0, sum = 0, o = 0, r;
    for (unsigned i = 0; i < 7; ++i) {
        unsigned d = (unsigned) (unsigned char) in[i] - '1';
        if (d >= 7 || (seen & (1U << d)))
            return 0;
        p[i] = (uint8_t) d;
        seen |= 1U << d;
    }
    for (unsigned i = 0; i < 7; ++i) {
        unsigned d = (unsigned) (unsigned char) in[7 + i] - '1';
        if (d >= 3)
            return 0;
        sum += d;
        if (i < 6)
            o = (o << 1) + o + d;
    }
    if (in[14] != '\0')
        return 0;
    while (sum >= 3)
        sum -= 3;
    if (sum)
        return 0;
    r = smaller_after(p, 0);
    r = (r << 2) + (r << 1) + smaller_after(p, 1); /* x6 */
    r = (r << 2) + r + smaller_after(p, 2);        /* x5 */
    r = (r << 2) + smaller_after(p, 3);            /* x4 */
    r = (r << 1) + r + smaller_after(p, 4);        /* x3 */
    r = (r << 1) + smaller_after(p, 5);            /* x2 */
    *pp = r;
    *po = o;
    return 1;
}

static unsigned mixed(unsigned p, unsigned o)
{
    unsigned off = v2_row_offset[v2_projection[p]] + (o >> 1);
    return (v2_packed[off] >> ((o & 1U) << 2)) & 15U;
}

static void enter(unsigned d, unsigned p, unsigned o, unsigned prev)
{
    frame *f = &frames[d];
    f->p = f->cp = (uint16_t) p;
    f->o = f->co = (uint16_t) o;
    f->face = f->turn = 0;
    f->prev = (uint8_t) prev;
}

/* Writes the moves (face * 3 + turn - 1) to path and returns their number. */
static int search(unsigned sp, unsigned so, uint8_t path[MAX_DEPTH])
{
    unsigned hp = v1_perm_dist[sp], hm = mixed(sp, so);
    unsigned first = hp > hm ? hp : hm;
    if (sp == 0 && so == 0)
        return 0;
    for (unsigned bound = first; bound <= MAX_DEPTH; ++bound) {
        unsigned depth = 0;
        enter(0, sp, so, NO_FACE);
        for (;;) {
            frame *f = &frames[depth];
            if (f->face == 3) {
                if (depth == 0)
                    break;
                --depth;
                continue;
            }
            if (f->face == f->prev || f->turn == 3) {
                ++f->face;
                f->turn = 0;
                f->cp = f->p;
                f->co = f->o;
                continue;
            }
            f->cp = v1_perm_next[f->face][f->cp];
            f->co = v1_ori_next[f->face][f->co];
            unsigned move = (f->face << 1) + f->face + f->turn;
            ++f->turn;
            unsigned np = f->cp, no = f->co, cd = depth + 1;
            if (cd > bound)
                continue;
            unsigned rem = bound - cd;
            if (v1_perm_dist[np] > rem || mixed(np, no) > rem)
                continue;
            path[depth] = (uint8_t) move;
            if (np == 0 && no == 0)
                return (int) cd;
            if (cd < bound) {
                unsigned pf = f->face;
                depth = cd;
                enter(depth, np, no, pf);
            }
        }
    }
    return -1; /* unreachable: every valid state is within 11 moves */
}

static const char face_ch[3] = {'R', 'B', 'D'};

int rubik_solve(const char *state, char *out, size_t cap)
{
    unsigned p, o;
    uint8_t path[MAX_DEPTH];
    if (!parse(state, &p, &o))
        return -1;
    int n = search(p, o, path);
    if (n < 0)
        return -1;
    size_t need = 1;
    for (int i = 0; i < n; ++i) {
        unsigned t = path[i];
        unsigned turn = t >= 6 ? t - 6 : t >= 3 ? t - 3 : t;
        need += (turn ? 2 : 1) + (i ? 1 : 0);
    }
    if (need > cap) {
        if (cap)
            out[0] = '\0';
        return -2;
    }
    size_t k = 0;
    for (int i = 0; i < n; ++i) {
        unsigned t = path[i];
        unsigned face = t >= 6 ? 2 : t >= 3 ? 1 : 0;
        unsigned turn = t - ((face << 1) + face);
        if (i)
            out[k++] = ' ';
        out[k++] = face_ch[face];
        if (turn == 1)
            out[k++] = '2';
        else if (turn == 2)
            out[k++] = '\'';
    }
    out[k] = '\0';
    return n;
}
