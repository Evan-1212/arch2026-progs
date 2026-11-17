/* Independent host reference for rubik_solve (src/rubik/rubik.c, compiled
 * unmodified and linked in).
 *
 * The model is a sticker cube in 3D, written from the geometry and not from
 * the solver, its generator or the tests:
 *   - the eight corners sit at (+-1, +-1, +-1), x toward R, y toward U, z
 *     toward F; a sticker is a corner and one axis, its normal points out;
 *   - a move turns a face clockwise as seen from outside: every sticker on
 *     the face (corner coordinate equal to the face normal n on that axis)
 *     moves by v -> n (n . v) - n x v, for its corner and its normal alike;
 *   - the faces are R (+x), B (-z) and D (-y); the corner L U F (-1, 1, 1)
 *     is on none of them and never moves.
 * The 14-character encoding of rubik.h is read through two definitions:
 *   - position i of the string is the corner that the faces R, B, D of the
 *     rubik.h model move together: 0 RUF, 1 RDF, 2 LDF, 3 RUB, 4 RDB, 5 LDB,
 *     6 LUB (each is the one corner on exactly its set of faces);
 *   - the orientation digit minus one is the number of counterclockwise
 *     steps, seen from outside the corner, from its U/D-axis sticker to the
 *     sticker that shows a U or D colour.
 * A state is keyed by the colours of the 21 stickers of positions 0..6
 * (3 bits each), so the search uses no rank and no solver table.
 *
 * Modes (one per run):
 *   ref bfs           breadth-first search from the solved cube: number of
 *                     states and the distance distribution, encoding round
 *                     trips, and the distances of the tests/rubikvec.h states
 *   ref all           bfs, then rubik_solve on every state: the text must
 *                     parse, its moves must solve the cube in the model, and
 *                     the count must equal the breadth-first distance
 *   ref sample K      bfs, then the same check on at least K states of each
 *                     distance (all of a distance with fewer), evenly spaced
 *                     in search order, and on the tests/rubikvec.h states
 *   ref edge          bfs, then invalid input (-1, output untouched) and the
 *                     cap rule (-2) on sampled states, every cap from 0 to
 *                     the length plus 2
 *   ref inputs K      bfs, then the inputs of the target check: every state
 *                     of distance 11, K states drawn from the search order
 *                     with the fixed-seed generator, and the
 *                     tests/rubikvec.h states, one "@@input kind state
 *                     distance" line each
 *   ref replay FILE   bfs, then results recorded elsewhere (rv32emu runs of
 *                     rubik_solve_asm): each line of FILE is a state, the
 *                     returned length and the text, separated by single
 *                     tabs; the length must be the breadth-first distance
 *                     and the text must solve the state in that many moves
 * Exit status: 0 when every check passes, 1 on any failure, 2 on bad usage.
 *
 * Build and run: bash mytests/rubik/run.sh <output-dir> [K]
 * The modes inputs and replay are used by mytests/rubik/target/verify.sh.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/rubik/rubik.h"
#include "tests/rubikvec.h"

enum {
    CORNERS = 8,
    FIXED = 7,
    STICKERS = CORNERS * 3,
    MAXLEN = 11,
    NSTATES = 3674160,
    HASH_BITS = 23,
    HASH_SIZE = 1 << HASH_BITS
};

/* Corner positions: 0..6 follow the string, 7 is the fixed corner L U F. */
static const int corner[CORNERS][3] = {
    {1, 1, 1},   {1, -1, 1},   {-1, -1, 1}, {1, 1, -1},
    {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {-1, 1, 1},
};
/* Face normals of the turning faces, in move order R, B, D. */
static const int normal[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}};
static const char face_name[3] = {'R', 'B', 'D'};

/* Colours: 0 R, 1 L, 2 U, 3 D, 4 F, 5 B, i.e. axis * 2 + (sign < 0). */
static int colour_of(int axis, int sign)
{
    return axis * 2 + (sign < 0);
}

static int is_ud(int colour)
{
    return colour == 2 || colour == 3;
}

/* ccw[c][k]: axis of the k-th sticker of corner c, counterclockwise from
 * outside, starting at the y axis. */
static int ccw[CORNERS][3];
/* step[f][s]: where sticker s goes on a clockwise quarter turn of face f. */
static int step[3][STICKERS];

static void cross(const int a[3], const int b[3], int out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static int dot(const int a[3], const int b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void axis_vec(int c, int axis, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    out[axis] = corner[c][axis];
}

static int find_corner(const int v[3])
{
    for (int c = 0; c < CORNERS; c++) {
        if (corner[c][0] == v[0] && corner[c][1] == v[1] &&
            corner[c][2] == v[2])
            return c;
    }
    return -1;
}

static void quarter(const int n[3], const int v[3], int out[3])
{
    int nx[3];
    cross(n, v, nx);
    int d = dot(n, v);
    for (int k = 0; k < 3; k++)
        out[k] = n[k] * d - nx[k];
}

static int build_geometry(void)
{
    for (int c = 0; c < CORNERS; c++) {
        ccw[c][0] = 1;
        for (int k = 1; k < 3; k++) {
            int a[3], found = -1;
            axis_vec(c, ccw[c][k - 1], a);
            for (int axis = 0; axis < 3; axis++) {
                int b[3], x[3];
                if (axis == ccw[c][k - 1])
                    continue;
                axis_vec(c, axis, b);
                cross(a, b, x);
                if (dot(x, corner[c]) > 0)
                    found = axis;
            }
            if (found < 0)
                return 0;
            ccw[c][k] = found;
        }
    }
    for (int f = 0; f < 3; f++) {
        int axis_f = normal[f][0] ? 0 : normal[f][1] ? 1 : 2;
        int moved = 0, hit[STICKERS] = {0};
        for (int c = 0; c < CORNERS; c++) {
            for (int axis = 0; axis < 3; axis++) {
                int s = c * 3 + axis;
                if (corner[c][axis_f] != normal[f][axis_f]) {
                    step[f][s] = s;
                    hit[s]++;
                    continue;
                }
                int pos[3], nrm[3], a[3];
                quarter(normal[f], corner[c], pos);
                axis_vec(c, axis, a);
                quarter(normal[f], a, nrm);
                int to = find_corner(pos);
                int to_axis = nrm[0] ? 0 : nrm[1] ? 1 : 2;
                if (to < 0 || nrm[to_axis] != pos[to_axis])
                    return 0;
                step[f][s] = to * 3 + to_axis;
                hit[step[f][s]]++;
                moved++;
            }
        }
        for (int s = 0; s < STICKERS; s++) {
            if (hit[s] != 1)
                return 0;
        }
        if (moved != 12 || step[f][FIXED * 3] != FIXED * 3)
            return 0;
    }
    return 1;
}

typedef struct {
    uint8_t col[STICKERS];
} cube;

static cube solved_cube(void)
{
    cube q;
    for (int c = 0; c < CORNERS; c++) {
        for (int axis = 0; axis < 3; axis++)
            q.col[c * 3 + axis] = (uint8_t) colour_of(axis, corner[c][axis]);
    }
    return q;
}

static cube turn(const cube *q, int face, int times)
{
    cube a = *q;
    for (int t = 0; t < times; t++) {
        cube b;
        for (int s = 0; s < STICKERS; s++)
            b.col[step[face][s]] = a.col[s];
        a = b;
    }
    return a;
}

static uint64_t key_of(const cube *q)
{
    uint64_t k = 0;
    for (int s = 0; s < FIXED * 3; s++)
        k = (k << 3) | q->col[s];
    return k;
}

static cube cube_of(uint64_t k)
{
    cube q = solved_cube();
    for (int s = FIXED * 3; s-- > 0;) {
        q.col[s] = (uint8_t) (k & 7);
        k >>= 3;
    }
    return q;
}

/* Independent validity rule of rubik.h for a NUL-terminated string. */
static int valid_text(const char *s)
{
    size_t n = strlen(s);
    if (n != RUBIK_STATE_LEN)
        return 0;
    int seen[CORNERS] = {0}, sum = 0;
    for (int i = 0; i < 7; i++) {
        if (s[i] < '1' || s[i] > '7' || seen[s[i] - '1']++)
            return 0;
        if (s[7 + i] < '1' || s[7 + i] > '3')
            return 0;
        sum += s[7 + i] - '1';
    }
    return sum % 3 == 0;
}

/* Cube from a valid string. */
static cube cube_from_text(const char *s)
{
    cube home = solved_cube(), q = home;
    for (int pos = 0; pos < 7; pos++) {
        int c = s[pos] - '1', t = s[7 + pos] - '1';
        for (int k = 0; k < 3; k++)
            q.col[pos * 3 + ccw[pos][(t + k) % 3]] =
                home.col[c * 3 + ccw[c][k]];
    }
    return q;
}

/* String of a cube; returns 0 when a corner matches no cubie. */
static int text_from_cube(const cube *q, char out[RUBIK_STATE_LEN + 1])
{
    cube home = solved_cube();
    for (int pos = 0; pos < 7; pos++) {
        int t = -1;
        for (int k = 0; k < 3; k++) {
            if (is_ud(q->col[pos * 3 + ccw[pos][k]]))
                t = k;
        }
        if (t < 0)
            return 0;
        int cubie = -1;
        for (int c = 0; c < 7 && cubie < 0; c++) {
            int same = 1;
            for (int k = 0; k < 3; k++) {
                if (q->col[pos * 3 + ccw[pos][(t + k) % 3]] !=
                    home.col[c * 3 + ccw[c][k]])
                    same = 0;
            }
            if (same)
                cubie = c;
        }
        if (cubie < 0)
            return 0;
        out[pos] = (char) ('1' + cubie);
        out[7 + pos] = (char) ('1' + t);
    }
    out[RUBIK_STATE_LEN] = '\0';
    return 1;
}

/* Open-addressing table: key + 1 (0 is empty) and the distance. */
static uint64_t *slot_key;
static uint8_t *slot_dist;
static uint64_t *order; /* states in breadth-first order */
static uint64_t level_start[MAXLEN + 2];

static uint64_t mix(uint64_t x)
{
    x ^= x >> 31;
    x *= 0x7fb5d329728ea185ULL;
    x ^= x >> 27;
    x *= 0x81dadef4bc2dd44dULL;
    x ^= x >> 33;
    return x;
}

/* Returns the slot index of key, inserting it with dist if absent. */
static uint64_t lookup(uint64_t key, int dist, int *inserted)
{
    uint64_t i = mix(key) & (HASH_SIZE - 1);
    *inserted = 0;
    for (;;) {
        if (slot_key[i] == 0) {
            if (dist < 0)
                return UINT64_MAX;
            slot_key[i] = key + 1;
            slot_dist[i] = (uint8_t) dist;
            *inserted = 1;
            return i;
        }
        if (slot_key[i] == key + 1)
            return i;
        i = (i + 1) & (HASH_SIZE - 1);
    }
}

static int distance_of(const cube *q)
{
    int ins;
    uint64_t i = lookup(key_of(q), -1, &ins);
    return i == UINT64_MAX ? -1 : slot_dist[i];
}

static const uint64_t expect_dist[MAXLEN + 1] = {
    1, 9, 54, 321, 1847, 9992, 50136, 227536, 870072, 1887748, 623800, 2644,
};

static uint64_t total;

static int run_bfs(void)
{
    slot_key = calloc(HASH_SIZE, sizeof *slot_key);
    slot_dist = calloc(HASH_SIZE, 1);
    order = malloc((size_t) NSTATES * sizeof *order);
    if (!slot_key || !slot_dist || !order)
        return 0;
    cube start = solved_cube();
    int ins;
    lookup(key_of(&start), 0, &ins);
    order[0] = key_of(&start);
    uint64_t head = 0, tail = 1;
    int depth = 0;
    level_start[0] = 0;
    while (head < tail) {
        uint64_t end = tail;
        level_start[depth + 1] = end;
        for (; head < end; head++) {
            cube q = cube_of(order[head]);
            for (int f = 0; f < 3; f++) {
                cube n = q;
                for (int t = 1; t <= 3; t++) {
                    n = turn(&n, f, 1);
                    uint64_t k = key_of(&n);
                    lookup(k, depth + 1, &ins);
                    if (!ins)
                        continue;
                    if (tail >= NSTATES) {
                        fprintf(stderr, "more states than %d\n", NSTATES);
                        return 0;
                    }
                    order[tail++] = k;
                }
            }
        }
        if (tail > end)
            depth++;
        if (depth > MAXLEN) {
            fprintf(stderr, "depth above %d\n", MAXLEN);
            return 0;
        }
    }
    total = tail;
    int ok = total == NSTATES;
    printf("bfs: states=%llu (expect %d) max_distance=%d\n",
           (unsigned long long) total, NSTATES, depth);
    uint64_t sum = 0;
    for (int d = 0; d <= MAXLEN; d++) {
        uint64_t n = level_start[d + 1] - level_start[d];
        sum += n;
        printf("bfs: distance %2d states=%llu expect=%llu %s\n", d,
               (unsigned long long) n, (unsigned long long) expect_dist[d],
               n == expect_dist[d] ? "ok" : "DIFFERENT");
        ok &= n == expect_dist[d];
    }
    ok &= sum == total;

    /* Every state has a valid string that maps back to it. */
    uint64_t round = 0, bad = 0;
    for (uint64_t i = 0; i < total; i++) {
        cube q = cube_of(order[i]);
        char s[RUBIK_STATE_LEN + 1];
        if (!text_from_cube(&q, s) || !valid_text(s)) {
            bad++;
            continue;
        }
        cube back = cube_from_text(s);
        if (key_of(&back) != order[i])
            bad++;
        else
            round++;
    }
    printf("bfs: encoding round trips=%llu failures=%llu\n",
           (unsigned long long) round, (unsigned long long) bad);
    ok &= bad == 0 && round == total;

    /* Distances of the public vectors in this model. */
    uint64_t vok = 0, vbad = 0;
    for (size_t i = 0; i < RUBIK_NCASES; i++) {
        const char *s = rubik_cases[i].state;
        cube q = cube_from_text(s);
        int d = valid_text(s) ? distance_of(&q) : -2;
        if (d == rubik_cases[i].distance)
            vok++;
        else {
            vbad++;
            printf("vector %s: distance %d, listed %d\n", s, d,
                   rubik_cases[i].distance);
        }
    }
    for (size_t i = 0; i < RUBIK_NINVALID; i++) {
        if (valid_text(rubik_invalid[i])) {
            vbad++;
            printf("invalid vector %zu is valid here\n", i);
        } else
            vok++;
    }
    printf("bfs: vectors agree=%llu disagree=%llu (of %zu)\n",
           (unsigned long long) vok, (unsigned long long) vbad,
           (size_t) (RUBIK_NCASES + RUBIK_NINVALID));
    ok &= vbad == 0;
    return ok;
}

/* Parses solver text into the model and applies it. Returns the number of
 * moves, or -1 when the text is malformed. */
static int apply_text(cube *q, const char *out)
{
    int count = 0;
    size_t i = 0, len = strlen(out);
    if (len == 0)
        return 0;
    for (;;) {
        int f = -1;
        for (int k = 0; k < 3; k++) {
            if (out[i] == face_name[k])
                f = k;
        }
        if (f < 0)
            return -1;
        i++;
        int times = 1;
        if (out[i] == '2') {
            times = 2;
            i++;
        } else if (out[i] == '\'') {
            times = 3;
            i++;
        }
        *q = turn(q, f, times);
        count++;
        if (out[i] == '\0')
            return count;
        if (out[i] != ' ' || out[i + 1] == '\0')
            return -1;
        i++;
    }
}

static uint64_t checked, failed, checksum;

static void check_state(const char *s, int dist)
{
    char out[96];
    memset(out, '#', sizeof out);
    int n = rubik_solve(s, out, sizeof out);
    checked++;
    checksum = checksum * 1000003u + (uint64_t) (n + 2) * 131u + strlen(out);
    cube q = cube_from_text(s);
    int m = memchr(out, '\0', sizeof out) ? apply_text(&q, out) : -1;
    cube goal = solved_cube();
    if (n != dist || m != n || key_of(&q) != key_of(&goal)) {
        if (failed < 10)
            printf(
                "FAIL %s: distance %d, returned %d, text \"%.40s\" moves %d\n",
                s, dist, n, memchr(out, '\0', sizeof out) ? out : "?", m);
        failed++;
    }
}

static int solve_all(uint64_t per_level)
{
    for (int d = 0; d <= MAXLEN; d++) {
        uint64_t lo = level_start[d], n = level_start[d + 1] - lo;
        uint64_t stride = per_level && n > per_level ? n / per_level : 1;
        uint64_t before = checked, fails = failed;
        for (uint64_t i = 0; i < n; i += stride) {
            cube q = cube_of(order[lo + i]);
            char s[RUBIK_STATE_LEN + 1];
            if (!text_from_cube(&q, s)) {
                failed++;
                continue;
            }
            check_state(s, d);
        }
        printf("solve: distance %2d checked=%llu failures=%llu\n", d,
               (unsigned long long) (checked - before),
               (unsigned long long) (failed - fails));
    }
    for (size_t i = 0; i < RUBIK_NCASES; i++)
        check_state(rubik_cases[i].state, rubik_cases[i].distance);
    printf(
        "solve: vectors %zu, total checked=%llu failures=%llu "
        "checksum=%016llx\n",
        (size_t) RUBIK_NCASES, (unsigned long long) checked,
        (unsigned long long) failed, (unsigned long long) checksum);
    return failed == 0;
}

static uint64_t rng = 0x9e3779b97f4a7c15ULL;

static uint64_t next_rand(void)
{
    rng += 0x9e3779b97f4a7c15ULL;
    return mix(rng);
}

/* One input for the -1 rule: the reference decides validity, the solver must
 * agree, leave out untouched on -1 and otherwise return the distance. */
static uint64_t edge_inputs, edge_valid, edge_fail;

static void edge_one(const char *s, size_t n)
{
    char in[32], out[48];
    memcpy(in, s, n);
    in[n] = '\0';
    memset(out, '#', sizeof out);
    int r = rubik_solve(in, out, sizeof out);
    edge_inputs++;
    int expect_valid = strlen(in) == n && valid_text(in);
    if (!expect_valid) {
        int untouched = 1;
        for (size_t k = 0; k < sizeof out; k++)
            untouched &= out[k] == '#';
        if (r != -1 || !untouched) {
            if (edge_fail < 10)
                printf("EDGE invalid \"%s\": returned %d, untouched %d\n", in,
                       r, untouched);
            edge_fail++;
        }
        return;
    }
    edge_valid++;
    cube q = cube_from_text(in);
    if (r != distance_of(&q)) {
        if (edge_fail < 10)
            printf("EDGE valid \"%s\": returned %d\n", in, r);
        edge_fail++;
    }
}

static uint64_t cap_cases, cap_fail;

static void cap_state(const char *s)
{
    char full[96], out[128];
    int n = rubik_solve(s, full, sizeof full);
    size_t len = strlen(full);
    for (size_t cap = 0; cap <= len + 2; cap++) {
        memset(out, '#', sizeof out);
        int r = rubik_solve(s, out, cap);
        int ok;
        cap_cases++;
        if (cap >= len + 1)
            ok = r == n && memcmp(out, full, len + 1) == 0;
        else if (cap >= 1)
            ok = r == -2 && out[0] == '\0';
        else
            ok = r == -2;
        /* Nothing at or beyond the cap (or the text and its NUL) changes. */
        size_t bound = cap > len + 1 ? len + 1 : cap;
        for (size_t k = bound; k < sizeof out; k++)
            ok &= out[k] == '#';
        if (!ok) {
            if (cap_fail < 10)
                printf("CAP %s cap=%zu len=%zu returned %d\n", s, cap, len, r);
            cap_fail++;
        }
    }
}

static int run_edge(void)
{
    const char *bases[6] = {"12345671111111",
                            "21345671111111",
                            "41532672313211",
                            "24513763133333",
                            NULL,
                            NULL};
    char b4[RUBIK_STATE_LEN + 1], b5[RUBIK_STATE_LEN + 1];
    cube q4 = cube_of(order[level_start[7] + 12345]);
    cube q5 = cube_of(order[level_start[10] + 777]);
    if (!text_from_cube(&q4, b4) || !text_from_cube(&q5, b5))
        return 0;
    bases[4] = b4;
    bases[5] = b5;

    /* Public invalid vectors. */
    for (size_t i = 0; i < RUBIK_NINVALID; i++)
        edge_one(rubik_invalid[i], strlen(rubik_invalid[i]));
    /* Every byte value at every position of six valid strings. */
    for (int b = 0; b < 6; b++) {
        for (int pos = 0; pos < RUBIK_STATE_LEN; pos++) {
            for (int v = 1; v < 256; v++) {
                char t[RUBIK_STATE_LEN + 1];
                memcpy(t, bases[b], sizeof t);
                t[pos] = (char) v;
                edge_one(t, RUBIK_STATE_LEN);
            }
        }
        /* Every prefix length, and one or two extra characters. */
        for (size_t n = 0; n < RUBIK_STATE_LEN; n++)
            edge_one(bases[b], n);
        for (int v = 1; v < 256; v++) {
            char t[RUBIK_STATE_LEN + 3];
            memcpy(t, bases[b], RUBIK_STATE_LEN);
            t[RUBIK_STATE_LEN] = (char) v;
            t[RUBIK_STATE_LEN + 1] = '1';
            edge_one(t, RUBIK_STATE_LEN + 1);
            edge_one(t, RUBIK_STATE_LEN + 2);
        }
    }
    /* Random strings over '0'..'9' of length 13..15. */
    for (uint64_t i = 0; i < 1000000; i++) {
        char t[16];
        uint64_t r = next_rand();
        size_t n = 13 + (size_t) (r % 3);
        r /= 3;
        for (size_t k = 0; k < n; k++) {
            t[k] = (char) ('0' + r % 10);
            r /= 10;
            if (k == 8)
                r = next_rand();
        }
        edge_one(t, n);
    }
    /* Random permutations with random orientations, about a third valid. */
    for (uint64_t i = 0; i < 200000; i++) {
        char t[RUBIK_STATE_LEN + 1] = "12345671111111";
        for (int k = 6; k > 0; k--) {
            int j = (int) (next_rand() % (uint64_t) (k + 1));
            char x = t[k];
            t[k] = t[j];
            t[j] = x;
        }
        for (int k = 7; k < RUBIK_STATE_LEN; k++)
            t[k] = (char) ('1' + next_rand() % 3);
        edge_one(t, RUBIK_STATE_LEN);
    }
    printf("edge: inputs=%llu valid=%llu failures=%llu\n",
           (unsigned long long) edge_inputs, (unsigned long long) edge_valid,
           (unsigned long long) edge_fail);

    /* The cap rule on the public vectors and 20 states of each distance. */
    uint64_t states = 0;
    for (size_t i = 0; i < RUBIK_NCASES; i++, states++)
        cap_state(rubik_cases[i].state);
    for (int d = 0; d <= MAXLEN; d++) {
        uint64_t lo = level_start[d], n = level_start[d + 1] - lo;
        uint64_t stride = n > 20 ? n / 20 : 1;
        for (uint64_t i = 0; i < n; i += stride, states++) {
            cube q = cube_of(order[lo + i]);
            char s[RUBIK_STATE_LEN + 1];
            if (!text_from_cube(&q, s))
                return 0;
            cap_state(s);
        }
    }
    printf("cap: states=%llu cases=%llu failures=%llu\n",
           (unsigned long long) states, (unsigned long long) cap_cases,
           (unsigned long long) cap_fail);
    return edge_fail == 0 && cap_fail == 0;
}

/* The inputs of the target check, as "@@input kind state distance" lines. */
static int print_inputs(uint64_t k)
{
    char s[RUBIK_STATE_LEN + 1];
    uint64_t bad = 0;
    for (uint64_t i = level_start[MAXLEN]; i < level_start[MAXLEN + 1]; i++) {
        cube q = cube_of(order[i]);
        if (!text_from_cube(&q, s)) {
            bad++;
            continue;
        }
        printf("@@input far %s %d\n", s, MAXLEN);
    }
    for (uint64_t i = 0; i < k; i++) {
        cube q = cube_of(order[next_rand() % total]);
        if (!text_from_cube(&q, s)) {
            bad++;
            continue;
        }
        printf("@@input random %s %d\n", s, distance_of(&q));
    }
    for (size_t i = 0; i < RUBIK_NCASES; i++)
        printf("@@input vector %s %d\n", rubik_cases[i].state,
               rubik_cases[i].distance);
    printf("inputs: far=%llu random=%llu vectors=%zu failures=%llu\n",
           (unsigned long long) (level_start[MAXLEN + 1] - level_start[MAXLEN]),
           (unsigned long long) k, (size_t) RUBIK_NCASES,
           (unsigned long long) bad);
    return bad == 0;
}

/* Checks recorded results: "state<TAB>length<TAB>text" per line. The state
 * must be valid, the length its breadth-first distance, and the text must
 * parse and solve the state in exactly that many moves. */
static int replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("replay: cannot open %s\n", path);
        return 0;
    }
    char line[256];
    uint64_t records = 0, bad = 0, sum = 0, hist[MAXLEN + 1] = {0};
    cube goal = solved_cube();
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        char *tab1 = strchr(line, '\t');
        char *tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
        int d = -1, m = -1;
        long n = -9;
        int ok = len > 0 && line[len - 1] == '\n' && tab2;
        records++;
        if (ok) {
            char *end;
            line[len - 1] = '\0';
            *tab1 = *tab2 = '\0';
            n = strtol(tab1 + 1, &end, 10);
            ok = end != tab1 + 1 && *end == '\0' && valid_text(line);
        }
        if (ok) {
            cube q = cube_from_text(line);
            d = distance_of(&q);
            m = apply_text(&q, tab2 + 1);
            ok = d >= 0 && n == d && m == d && key_of(&q) == key_of(&goal);
        }
        if (ok) {
            hist[d]++;
            sum = sum * 1000003u + (uint64_t) d * 131u + strlen(tab2 + 1);
            continue;
        }
        if (bad < 10)
            printf(
                "replay FAIL record %llu: distance %d, length %ld, moves %d\n",
                (unsigned long long) records, d, n, m);
        bad++;
    }
    int read_error = ferror(f);
    fclose(f);
    for (int d = 0; d <= MAXLEN; d++)
        printf("replay: distance %2d records=%llu\n", d,
               (unsigned long long) hist[d]);
    printf("replay: records=%llu failures=%llu checksum=%016llx\n",
           (unsigned long long) records, (unsigned long long) bad,
           (unsigned long long) sum);
    return records > 0 && bad == 0 && !read_error;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fputs("usage: ref bfs|all|sample K|edge|inputs K|replay FILE\n",
              stderr);
        return 2;
    }
    if (!build_geometry()) {
        puts("geometry: FAILED");
        return 1;
    }
    int ok = run_bfs();
    if (total != NSTATES) {
        /* The later modes index the levels of a complete search. */
        puts("result: FAIL (incomplete search)");
        free(slot_key);
        free(slot_dist);
        free(order);
        return 1;
    }
    if (strcmp(argv[1], "all") == 0)
        ok &= solve_all(0);
    else if (strcmp(argv[1], "sample") == 0 && argc == 3)
        ok &= solve_all(strtoull(argv[2], NULL, 10));
    else if (strcmp(argv[1], "edge") == 0)
        ok &= run_edge();
    else if (strcmp(argv[1], "inputs") == 0 && argc == 3)
        ok &= print_inputs(strtoull(argv[2], NULL, 10));
    else if (strcmp(argv[1], "replay") == 0 && argc == 3)
        ok &= replay(argv[2]);
    else if (strcmp(argv[1], "bfs") != 0) {
        fputs("usage: ref bfs|all|sample K|edge|inputs K|replay FILE\n",
              stderr);
        return 2;
    }
    printf("result: %s\n", ok ? "PASS" : "FAIL");
    free(slot_key);
    free(slot_dist);
    free(order);
    return ok ? 0 : 1;
}
