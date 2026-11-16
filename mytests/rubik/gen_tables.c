/* Table generator for the 2x2x2 solver of src/rubik/.
 *
 * Derives every table of the HW1 v2 design (IDA* with the maximum of two
 * pattern databases) from the cube model of src/rubik/rubik.h: three faces
 * R, B, D, each destination cubie taking the cubie and the twist of
 * source[face][destination].
 *
 * Coordinates:
 *   p  rank of the permutation, Lehmer code with radices 7, 6, ..., 1
 *      (0..5039, 0 is solved)
 *   o  orientations of positions 0..5 in base 3, position 0 most significant
 *      (0..728, 0 is solved); position 6 follows from the sum rule
 *   q  placement of cubies 0, 1 and 6: (a * 6 + b - (a < b)) * 5 + c - (a < c)
 *      - (b < c) for their positions a, b, c (0..209)
 *
 * Tables, in the order they are emitted:
 *   v2_row_offset[210]     q * 365, the first byte of row q of v2_packed
 *   v1_perm_next[3][5040]  p after one clockwise quarter turn of a face
 *   v1_ori_next[3][729]    o after one clockwise quarter turn of a face
 *   v1_perm_dist[5040]     distance of p alone to the solved permutation
 *   v2_projection[5040]    q of permutation p
 *   v2_packed[76650]       distance of (q, o) to (goal q, 0) as 4-bit values,
 *                          o even in the low nibble, 365 bytes per row; the
 *                          unused high nibble ending each row is 15
 * All distances count the nine moves (R R2 R' B B2 B' D D2 D').
 *
 * Usage:
 *   gen_tables c     C header for src/rubik/rubik_tables.h (static const)
 *   gen_tables asm   GNU as source with local labels (no .globl)
 * Both write to stdout. Exit status 0 on success, 1 when a self-check fails,
 * 2 on bad usage.
 *
 * Build and run: bash mytests/rubik/gen.sh <output-dir>
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CUBIES = 7,
    PERMS = 5040,
    ORIS = 729,
    PLACEMENTS = 210,
    ROW_BYTES = (ORIS + 1) / 2,
    PACKED = PLACEMENTS * ROW_BYTES,
    MIXED = PLACEMENTS * ORIS,
    UNSEEN = 255
};

static const uint8_t source[3][CUBIES] = {
    {1, 4, 2, 0, 3, 5, 6},
    {0, 1, 2, 4, 5, 6, 3},
    {0, 2, 5, 3, 1, 4, 6},
};
static const uint8_t twist[3][CUBIES] = {
    {1, 2, 0, 2, 1, 0, 0},
    {0, 0, 0, 1, 2, 1, 2},
    {0, 0, 0, 0, 0, 0, 0},
};
static const uint8_t pattern_cubies[3] = {0, 1, 6};

static uint32_t row_offset[PLACEMENTS];
static uint16_t perm_next[3][PERMS], ori_next[3][ORIS];
static uint8_t perm_dist[PERMS], projection[PERMS], packed[PACKED];
static uint8_t mixed_dist[MIXED];
static uint32_t queue[MIXED];

static unsigned rank_perm(const uint8_t p[CUBIES])
{
    unsigned r = 0;
    for (unsigned i = 0; i < CUBIES; i++) {
        unsigned smaller = 0;
        for (unsigned j = i + 1; j < CUBIES; j++)
            smaller += p[j] < p[i];
        r = r * (CUBIES - i) + smaller;
    }
    return r;
}

static void unrank_perm(unsigned r, uint8_t p[CUBIES])
{
    unsigned code[CUBIES];
    uint8_t used[CUBIES] = {0};
    for (unsigned i = CUBIES; i-- > 0;) {
        unsigned radix = CUBIES - i;
        code[i] = r % radix;
        r /= radix;
    }
    for (unsigned i = 0; i < CUBIES; i++) {
        unsigned k = code[i];
        for (unsigned c = 0; c < CUBIES; c++) {
            if (used[c])
                continue;
            if (k == 0) {
                p[i] = (uint8_t) c;
                used[c] = 1;
                break;
            }
            k--;
        }
    }
}

static unsigned rank_ori(const uint8_t o[CUBIES])
{
    unsigned r = 0;
    for (unsigned i = 0; i < CUBIES - 1; i++)
        r = r * 3 + o[i];
    return r;
}

static void unrank_ori(unsigned r, uint8_t o[CUBIES])
{
    unsigned sum = 0;
    for (unsigned i = CUBIES - 1; i-- > 0;) {
        o[i] = (uint8_t) (r % 3);
        sum += o[i];
        r /= 3;
    }
    o[CUBIES - 1] = (uint8_t) ((3 - sum % 3) % 3);
}

static unsigned placement_rank(unsigned a, unsigned b, unsigned c)
{
    return (a * 6 + b - (a < b)) * 5 + c - (a < c) - (b < c);
}

static void build_moves(void)
{
    for (unsigned f = 0; f < 3; f++) {
        for (unsigned r = 0; r < PERMS; r++) {
            uint8_t p[CUBIES], n[CUBIES];
            unrank_perm(r, p);
            for (unsigned i = 0; i < CUBIES; i++)
                n[i] = p[source[f][i]];
            perm_next[f][r] = (uint16_t) rank_perm(n);
        }
        for (unsigned r = 0; r < ORIS; r++) {
            uint8_t o[CUBIES], n[CUBIES];
            unrank_ori(r, o);
            for (unsigned i = 0; i < CUBIES; i++)
                n[i] = (uint8_t) ((o[source[f][i]] + twist[f][i]) % 3);
            ori_next[f][r] = (uint16_t) rank_ori(n);
        }
    }
}

/* Breadth-first distances of the permutation coordinate. */
static int build_perm_dist(void)
{
    unsigned head = 0, tail = 1;
    memset(perm_dist, UNSEEN, sizeof perm_dist);
    perm_dist[0] = 0;
    queue[0] = 0;
    while (head < tail) {
        unsigned here = queue[head++];
        for (unsigned f = 0; f < 3; f++) {
            unsigned next = here;
            for (unsigned t = 0; t < 3; t++) {
                next = perm_next[f][next];
                if (perm_dist[next] != UNSEEN)
                    continue;
                perm_dist[next] = (uint8_t) (perm_dist[here] + 1);
                queue[tail++] = next;
            }
        }
    }
    unsigned maximum = 0;
    for (unsigned r = 0; r < PERMS; r++) {
        if (perm_dist[r] > maximum)
            maximum = perm_dist[r];
    }
    fprintf(stderr, "perm: visited %u of %u, maximum %u\n", tail,
            (unsigned) PERMS, maximum);
    return tail == PERMS && maximum <= 11;
}

/* Breadth-first distances of (placement of cubies 0, 1, 6; orientation). */
static int build_mixed(void)
{
    uint8_t place_next[3][PLACEMENTS];
    uint8_t where[PLACEMENTS][3];
    for (unsigned a = 0; a < CUBIES; a++) {
        for (unsigned b = 0; b < CUBIES; b++) {
            for (unsigned c = 0; c < CUBIES; c++) {
                if (a == b || a == c || b == c)
                    continue;
                unsigned q = placement_rank(a, b, c);
                where[q][0] = (uint8_t) a;
                where[q][1] = (uint8_t) b;
                where[q][2] = (uint8_t) c;
            }
        }
    }
    for (unsigned f = 0; f < 3; f++) {
        uint8_t dest[CUBIES];
        for (unsigned i = 0; i < CUBIES; i++)
            dest[source[f][i]] = (uint8_t) i;
        for (unsigned q = 0; q < PLACEMENTS; q++)
            place_next[f][q] = (uint8_t) placement_rank(
                dest[where[q][0]], dest[where[q][1]], dest[where[q][2]]);
    }
    for (unsigned r = 0; r < PERMS; r++) {
        uint8_t p[CUBIES], pos[3] = {0};
        unrank_perm(r, p);
        for (unsigned i = 0; i < CUBIES; i++) {
            for (unsigned k = 0; k < 3; k++) {
                if (p[i] == pattern_cubies[k])
                    pos[k] = (uint8_t) i;
            }
        }
        projection[r] = (uint8_t) placement_rank(pos[0], pos[1], pos[2]);
    }
    for (unsigned q = 0; q < PLACEMENTS; q++)
        row_offset[q] = q * ROW_BYTES;

    unsigned goal = placement_rank(pattern_cubies[0], pattern_cubies[1],
                                   pattern_cubies[2]) *
                    ORIS;
    unsigned head = 0, tail = 1, maximum = 0;
    memset(mixed_dist, UNSEEN, sizeof mixed_dist);
    mixed_dist[goal] = 0;
    queue[0] = goal;
    while (head < tail) {
        unsigned here = queue[head++];
        unsigned q0 = here / ORIS, o0 = here % ORIS;
        for (unsigned f = 0; f < 3; f++) {
            unsigned q = q0, o = o0;
            for (unsigned t = 0; t < 3; t++) {
                q = place_next[f][q];
                o = ori_next[f][o];
                unsigned there = q * ORIS + o;
                if (mixed_dist[there] != UNSEEN)
                    continue;
                mixed_dist[there] = (uint8_t) (mixed_dist[here] + 1);
                if (mixed_dist[there] > maximum)
                    maximum = mixed_dist[there];
                queue[tail++] = there;
            }
        }
    }
    fprintf(stderr, "mixed: visited %u of %u, maximum %u, goal q %u\n", tail,
            (unsigned) MIXED, maximum, goal / ORIS);
    if (tail != MIXED || maximum > 11)
        return 0;
    memset(packed, UNSEEN, sizeof packed);
    for (unsigned q = 0; q < PLACEMENTS; q++) {
        for (unsigned o = 0; o < ORIS; o++) {
            unsigned i = q * ROW_BYTES + o / 2, shift = (o & 1U) * 4;
            packed[i] =
                (uint8_t) ((packed[i] & ~(15U << shift)) |
                           ((unsigned) mixed_dist[q * ORIS + o] << shift));
        }
    }
    return 1;
}

/* Emitter: one table, `per_line` values per line. */
typedef struct {
    const char *ctype, *dir;
    unsigned per_line;
} kind;

static const kind k8 = {"uint8_t", ".byte", 24};
static const kind k16 = {"uint16_t", ".half", 16};
static const kind k32 = {"uint32_t", ".word", 8};

static void values(const kind *k,
                   const void *data,
                   unsigned n,
                   int asm_out,
                   const char *indent)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned v;
        if (k == &k8)
            v = ((const uint8_t *) data)[i];
        else if (k == &k16)
            v = ((const uint16_t *) data)[i];
        else
            v = ((const uint32_t *) data)[i];
        if (i % k->per_line == 0 && asm_out)
            printf("%s%s %u", indent, k->dir, v);
        else if (i % k->per_line == 0)
            printf("%s%u,", indent, v);
        else
            printf(asm_out ? ",%u" : " %u,", v);
        if (i % k->per_line == k->per_line - 1 || i + 1 == n)
            putchar('\n');
    }
}

static void emit_c(const char *name,
                   const kind *k,
                   const void *data,
                   unsigned rows,
                   unsigned n)
{
    if (rows == 1) {
        printf("static const %s %s[%u] = {\n", k->ctype, name, n);
        values(k, data, n, 0, "    ");
    } else {
        printf("static const %s %s[%u][%u] = {\n", k->ctype, name, rows, n);
        for (unsigned r = 0; r < rows; r++) {
            puts("    {");
            values(
                k,
                (const uint8_t *) data + (size_t) r * n * (k == &k16 ? 2 : 1),
                n, 0, "        ");
            puts("    },");
        }
    }
    puts("};");
}

static void emit_asm(const char *name,
                     const kind *k,
                     const void *data,
                     unsigned count)
{
    printf("%s:\n", name);
    values(k, data, count, 1, "    ");
}

int main(int argc, char **argv)
{
    int asm_out;
    if (argc == 2 && strcmp(argv[1], "c") == 0)
        asm_out = 0;
    else if (argc == 2 && strcmp(argv[1], "asm") == 0)
        asm_out = 1;
    else {
        fputs("usage: gen_tables c|asm\n", stderr);
        return 2;
    }
    build_moves();
    if (!build_perm_dist() || !build_mixed())
        return 1;

    if (!asm_out) {
        puts(
            "/* Generated by mytests/rubik/gen_tables.c (gen_tables c); do "
            "not edit.\n"
            " * Included by src/rubik/rubik.c only. The coordinates and the "
            "layout are\n"
            " * described in the generator. clang-format is off for the "
            "values: the\n"
            " * generator, not the formatter, owns their layout.\n"
            " */\n"
            "#pragma once\n\n"
            "#include <stdint.h>\n\n"
            "// clang-format off");
        emit_c("v2_row_offset", &k32, row_offset, 1, PLACEMENTS);
        emit_c("v1_perm_next", &k16, perm_next, 3, PERMS);
        emit_c("v1_ori_next", &k16, ori_next, 3, ORIS);
        emit_c("v1_perm_dist", &k8, perm_dist, 1, PERMS);
        emit_c("v2_projection", &k8, projection, 1, PERMS);
        emit_c("v2_packed", &k8, packed, 1, PACKED);
        puts("// clang-format on");
    } else {
        puts(
            "# Generated by mytests/rubik/gen_tables.c (gen_tables asm); do "
            "not edit.\n"
            "# Same tables, order and layout as src/rubik/rubik_tables.h; "
            "local labels.\n"
            "# v1_perm_next and v1_ori_next are three rows of 5040 and 729 "
            "halfwords.\n\n"
            "    .section .rodata\n"
            "    .balign 4");
        emit_asm("v2_row_offset", &k32, row_offset, PLACEMENTS);
        emit_asm("v1_perm_next", &k16, perm_next, 3 * PERMS);
        emit_asm("v1_ori_next", &k16, ori_next, 3 * ORIS);
        emit_asm("v1_perm_dist", &k8, perm_dist, PERMS);
        emit_asm("v2_projection", &k8, projection, PERMS);
        emit_asm("v2_packed", &k8, packed, PACKED);
    }
    return fflush(stdout) != 0 || ferror(stdout) ? 1 : 0;
}
