/* t_mul - the product tiers against the test reference (tests/tref.h: GMP exact up to 2^21 product limbs, the residue form above).
 *
 *  0. the reference itself: the product-tree GMP conversions (harness.h) against the 18-digit string forms
 *  1. the device path (rns_mul_dist_db) at small and boundary sizes -- tiny, around 2^10 and 2^20 points, unbalanced, x * 0,
 *     x * 1, all five generators -- against GMP exactly, at two plane caps (the default, one plane; DIST_LOGN_TEST=20, the grid
 *     split) which must also agree with each other; low products (rns_mul_low_db) against GMP mod B^w
 *  2. sizes 2^16 .. 2^30 limbs per operand (crossing 2^17, 2^18, 2^27, 2^28) through the device path: exact up to 2^20, the
 *     residue form above; at 2^20, 2^24, 2^26 also at half the plane (a 2-piece grid), which must agree
 *     every product of 1 and 2 also checks the reference: one limb changed low / middle / top must fail it (tref_faults)
 *  4. batch tier: products of <= 2^logL points for logL 10 .. 18, GPU CRT (N >= 8)
 *     and CPU CRT, registered and unregistered (staged) operands, vs mpz_mul (every product for groups up to 2^24
 *     limbs; larger groups: four products vs mpz_mul and every product by residues mod three primes, see batch_sampled)
 *  4b. the locality-aware batch tier (pairs sharing B in device pools)
 *
 * Usage: t_mul [POOL (20; 0 skips sections 0-2)] [batch | dist | split]   (split: only section 4c, the RNS_BATCH_SPLIT forced-split check)   (dist: only the distributed tier, 6e5 .. 2^27 limbs; DIST_BIG=1
 *        2^29, 2^30, time only).  The host product tier (rns_mul, Karatsuba split, mdev) and its sections were removed by TREF
 *        (results/CODE_REDUCTION.md §TREF); decimal limbs only since archive:PLAN §28.5.
 */
#include "harness.h"
#include "tref.h"
#include "../rns_mul.h"
#include "../dbig.h"
#include "../mem.h"
#include <hip/hip_runtime.h>

/* C = A B on the device path (host operands staged through dbig); returns the product's seconds */
static double dev_mul(bigint *C, const bigint *A, const bigint *B)
{
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z);
    db_from_bi(&x, A); db_from_bi(&y, B);
    double t0 = now(); rns_mul_dist_db(&z, &x, &y); double t = now() - t0;
    db_to_bi(C, &z); db_free(&x); db_free(&y); db_free(&z);
    return t;
}
static void dev_low(bigint *C, const bigint *A, const bigint *B, size_t w)
{
    dbig x, y, z; db_init(&x); db_init(&y); db_init(&z);
    db_from_bi(&x, A); db_from_bi(&y, B); rns_mul_low_db(&z, &x, &y, w); db_to_bi(C, &z);
    db_free(&x); db_free(&y); db_free(&z);
}
static void set_cap(int logn)                        /* the plane cap of the device path: 0 = the default */
{
    if (!logn) { unsetenv("DIST_LOGN_TEST"); return; }
    char b[16]; snprintf(b, sizeof b, "%d", logn); setenv("DIST_LOGN_TEST", b, 1);
}
/* the product checked by the reference, and the reference checked by the product: one limb changed must fail it */
static rng_t g_frng = { 0xFA17 };
static size_t g_fault_sets, g_fault_missed;
static void check_dev(const char *what, const bigint *A, const bigint *B, bigint *C, int faults)
{
    if (!tref_check(what, A, B, C) || !faults || !C->n) return;
    int m = tref_faults(A->l, A->n, B->l, B->n, C->l, C->n, TREF_AUTO, &g_frng);   /* (flips and restores limbs of C in place) */
    g_fault_sets++; g_fault_missed += (size_t)m;
    VERIFY(m == 0, "%s: the reference missed %d of 3 injected faults", what, m);
}

/* batch groups (sections 4, 4b): the GMP string bridge of decimal limbs costs about as much per product as a 2^17-limb
 * multiply, so a group of thousands of products took > 700 s (results/CODE_REDUCTION.md §H4 7).  Groups up to BATCH_EXACT_LIMBS operand and
 * product limbs are still checked exactly for every product; larger groups check four products exactly (the first two, the
 * middle, the last) and every product by residues: c == a b (+ x) mod 2^61-1, 10^18+3 and 4611686018427387847 (a modulus of
 * about 2^184), with every limb of c < B (so c < B^nc and the residues pin c up to that modulus) */
#define BATCH_EXACT_LIMBS ((size_t)1 << 24)
static int batch_sampled(size_t i, size_t N) { return i < 2 || i == N / 2 || i + 1 == N; }
static int prod_residues_ok(const uint64_t *a, size_t na, const uint64_t *b, size_t nb, const uint64_t *x, size_t nx,
                            const uint64_t *c, size_t nc)
{
    for (size_t k = 0; k < nc; k++) if (c[k] >= BI_B10) return 0;
    uint64_t rx = x ? limbs_mod_m61(x, nx) : 0;
    if (m61_add(m61_mul(limbs_mod_m61(a, na), limbs_mod_m61(b, nb)), rx) != limbs_mod_m61(c, nc)) return 0;
    static const uint64_t qs[2] = { HV_Q1, HV_Q2 };
    for (int j = 0; j < 2; j++) {
        uint64_t q = qs[j], xx = x ? limbs_mod_u64(x, nx, q) : 0;
        if ((hv_mulmod(limbs_mod_u64(a, na, q), limbs_mod_u64(b, nb, q), q) + xx) % q != limbs_mod_u64(c, nc, q)) return 0;
    }
    return 1;
}
/* products of a batch group by residues (OpenMP over the products); returns the count wrong */
static size_t batch_residues_bad(const rns_prod *P, size_t N)
{
    size_t bad = 0;
    #pragma omp parallel for schedule(dynamic, 8) reduction(+:bad)
    for (long i = 0; i < (long)N; i++)
        if (!prod_residues_ok(P[i].a, P[i].na, P[i].b, P[i].nb, P[i].x, P[i].nx, P[i].c, P[i].na + P[i].nb + (P[i].x ? 1 : 0))) bad++;
    return bad;
}

int main(int argc, char **argv)
{
    int pool = argc > 1 ? atoi(argv[1]) : 20;
    int only_batch = argc > 2 && !strcmp(argv[2], "batch");
    int only_big = argc > 2 && !strcmp(argv[2], "splitbig"), only_split = only_big || (argc > 2 && !strcmp(argv[2], "split"));   /* splitbig: only the 2^27 / 2^28-point cases (residues only, no unsplit reference) */   /* only section 4c (RNS_BATCH_SPLIT) */
    int only_dist = argc > 2 && !strcmp(argv[2], "dist");     /* WP5: the distributed tier against the reference, with times */
    rng_t rng = {0xB1B0ULL};
    bigint A, B, C; bi_init(&A); bi_init(&B); bi_init(&C);
    printf("== t_mul ==\n");
    harness_meta("t_mul");

    if (only_dist) {
        rns_init(pool ? pool : 31);
        int big = getenv("DIST_BIG") != 0;                   /* DIST_BIG=1: the 2^30 and 2^31-point products (time only) */
        struct { size_t na, nb; } dc[] = { {600000, 500000}, {1u << 20, 1u << 20}, {(1u << 22) + 3, (1u << 22) - 5}, {1u << 25, 1u << 25}, {(1u << 27) + 11, (1u << 27) - 11}, {(size_t)1 << 29, (size_t)1 << 29}, {(size_t)1 << 30, (size_t)1 << 30} };
        size_t ndc = big ? 7 : 5;
        for (size_t i = big ? 5 : 0; i < ndc; i++) for (int kind = 0; kind < (big ? 1 : 2); kind++) {
            bi_random(&A, dc[i].na, kind, &rng); bi_random(&B, dc[i].nb, kind, &rng);
            dev_mul(&C, &A, &B);                               /* first call builds the plans */
            double dt = dev_mul(&C, &A, &B);
            char what[96]; snprintf(what, sizeof what, "dist %zux%zu %s", dc[i].na, dc[i].nb, gen_name[kind]);
            if (!big) check_dev(what, &A, &B, &C, 0);
            if (kind == 0) printf("   %-40s dist %.3f s\n", what, dt);
        }
        bi_free(&A); bi_free(&B); bi_free(&C);
        return verify_done("t_mul");
    }
    rns_init(31);
    if (only_split) goto split4c;
    if (pool && !only_batch) {
    /* 0. the reference's GMP conversions */
    printf("-- 0. GMP conversions: product tree == string form\n");
    {
        static const size_t cn[] = { 1, 2, 16, 17, 33, 1000, 4097, 65537 };
        mpz_t x, y; mpz_inits(x, y, NULL);
        for (size_t i = 0; i < sizeof cn / sizeof *cn; i++) for (int kind = 0; kind < GEN_KINDS; kind++) {
            size_t n = cn[i]; uint64_t *a = (uint64_t *)malloc(n * 8), *b = (uint64_t *)malloc((n + 2) * 8), *c = (uint64_t *)malloc((n + 2) * 8);
            gen_limbs(a, n, kind, &rng);
            mpz_from_limbs(x, a, n); mpz_from_limbs_str(y, a, n);
            size_t m = mpz_to_limbs(b, n + 2, x), m2 = mpz_to_limbs_str(c, n + 2, y);
            VERIFY(mpz_cmp(x, y) == 0 && m == m2 && m == tref_norm(a, n) && !memcmp(a, b, m * 8) && !memcmp(b, c, m * 8), "conversions n %zu %s", n, gen_name[kind]);
            free(a); free(b); free(c);
        }
        mpz_clears(x, y, NULL);
    }
    /* 1. small and boundary sizes, exact, at two plane caps */
    printf("-- 1. device path, small and boundary sizes vs GMP, plane caps default and 2^20\n");
    {
        const size_t M = (size_t)1 << 19;                    /* 2 M = 2^20 points: the smallest plane */
        struct { size_t na, nb; const char *why; } cases[] = {
            {1, 1, "one limb"}, {3, 5, "tiny"}, {511, 512, "below 2^10"}, {512, 512, "exactly 2^10"}, {513, 512, "just over 2^10"},
            {1000, 30, "unbalanced small"}, {1, 100000, "one limb x 1e5"}, {65535, 65537, "2^16 -+ 1"},
            {M, M, "exactly 2^20 points"}, {M + 1, M, "2^20 + 1 points"}, {3 * M, 1u << 12, "unbalanced 384:1"},
            {2 * M - 3, 2 * M + 3, "2^21 limbs (the exact limit)"},
        };
        bigint D; bi_init(&D);
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
            for (int kind = 0; kind < GEN_KINDS; kind++) {
                bi_random(&A, cases[i].na, kind, &rng); bi_random(&B, cases[i].nb, kind == GEN_KINDS - 1 ? GEN_UNIFORM : kind, &rng);
                char what[128]; snprintf(what, sizeof what, "%s %zux%zu %s", cases[i].why, cases[i].na, cases[i].nb, gen_name[kind]);
                set_cap(0); double t0 = dev_mul(&C, &A, &B);
                check_dev(what, &A, &B, &C, kind == 0);
                set_cap(20); double t1 = dev_mul(&D, &A, &B); set_cap(0);
                VERIFY(bi_cmp(&C, &D) == 0, "%s: the plane caps 2^20 and default differ", what);
                if (kind == 0) printf("   %-44s %.3f s (cap 2^20: %.3f s)\n", what, t0, t1);
            }
        /* low products (A B) mod B^w at the 2^20 cap (the grid with the pieces above w skipped) */
        {
            mpz_t a, b, c, m; mpz_inits(a, b, c, m, NULL);
            struct { size_t na, nb, w; } lc[] = { {M, M, M + 2}, {M, M, M / 2 + 1}, {M + 7, M - 3, M + 2}, {3 * M, M / 2, M / 2 + 2},
                                                  {2 * M + 5, 2 * M + 5, 2 * M + 7}, {2 * M, 2 * M, 3}, {M / 2, M / 2, 100} };
            set_cap(20);
            for (size_t i = 0; i < sizeof lc / sizeof *lc; i++) for (int kind = 0; kind < GEN_KINDS; kind += 2) {
                bi_random(&A, lc[i].na, kind, &rng); bi_random(&B, lc[i].nb, kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
                dev_low(&C, &A, &B, lc[i].w);
                bi_to_mpz(a, &A); bi_to_mpz(b, &B); mpz_mul(c, a, b);
                mpz_ui_pow_ui(m, BI_B10, lc[i].w); mpz_fdiv_r(c, c, m);
                VERIFY(bi_eq_mpz(&C, c), "low product %zux%zu w %zu %s", lc[i].na, lc[i].nb, lc[i].w, gen_name[kind]);
            }
            set_cap(0);
            mpz_clears(a, b, c, m, NULL);
        }
        /* zero and one */
        bi_random(&A, 5000, GEN_UNIFORM, &rng); bi_set_zero(&B); dev_mul(&C, &A, &B); VERIFY(C.n == 0, "x * 0");
        bi_set_u64(&B, 1); dev_mul(&C, &A, &B); VERIFY(bi_cmp(&C, &A) == 0, "x * 1");
        bi_free(&D);
    }

    /* 2. large sizes */
    printf("-- 2. device path, 2^16 .. 2^30 limbs per operand (exact to 2^20, the residue form above)\n");
    {
        static const size_t sizes[] = {1 << 16, (1 << 17) - 1, 1 << 17, (1 << 18) - 1, 1 << 18, 1 << 20, 1 << 24, 1 << 26, 1 << 27, 1 << 28, 1 << 30};
        bigint D; bi_init(&D);
        for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
            size_t n = sizes[i];
            bi_random(&A, n, GEN_UNIFORM, &rng); bi_random(&B, n, GEN_UNIFORM, &rng);
            double dt = dev_mul(&C, &A, &B);
            char what[64]; snprintf(what, sizeof what, "%zu x %zu limbs", n, n);
            double t0 = now(); check_dev(what, &A, &B, &C, 1); double tc = now() - t0;
            printf("   %-36s %.3f s (reference %s %.2f s with the faults)\n", what, dt, 2 * n <= TREF_EXACT_LIMBS ? "exact" : "residues", tc);
            if (n == (1 << 20) || n == (1 << 24) || n == (1 << 26)) {
                int lg = 0; while (((size_t)1 << lg) < 2 * n) lg++;
                set_cap(lg - 1); dev_mul(&D, &A, &B); set_cap(0);
                VERIFY(bi_cmp(&C, &D) == 0, "%s: the plane caps 2^%d and 2^%d differ", what, lg - 1, lg);
            }
            if (n == (1 << 30)) harness_result("dist_2e31_total", "s", dt);
        }
        bi_free(&D);
    }
    printf("   the reference caught every injected fault: %zu sets of 3, %zu missed\n", g_fault_sets, g_fault_missed);
    }
    /* 4. batch tier */
    printf("-- 4. batch: N products of <= 2^logL points, GPU and CPU CRT, registered and staged operands\n");
    {
        struct { int logL; size_t N; int reg; const char *why; } bc[] = {
            {14, 200, 3, "grpB: 200 A x one B, 2^14"}, {10, 5, 3, "grpB: CPU CRT"}, {12, 300, 2, "grpB staged"}, {17, 3000, 3, "grpB: 3000 x 2^17, tiles"},
            {22, 12, 1, "2^22 (max)"}, {20, 6, 1, "2^20 CPU CRT"},
            {10, 5, 1, "N < 8: CPU CRT"}, {10, 8, 1, "N = 8: GPU CRT"}, {10, 1000, 1, "many tiny"},
            {12, 300, 1, "2^12"}, {14, 200, 1, "2^14"}, {17, 100, 1, "2^17"}, {18, 40, 1, "2^18"},
            {14, 200, 0, "2^14 staged (unregistered)"}, {10, 5, 0, "tiny staged, CPU CRT"},
            {14, 40000, 1, "2^14 x 40000: several tiles"},
        };
        mpz_t a, b, c, d; mpz_inits(a, b, c, d, NULL);
        for (size_t ci = 0; ci < sizeof bc / sizeof *bc; ci++) {
            size_t L = (size_t)1 << bc[ci].logL, N = bc[ci].N;
            rns_prod *P = (rns_prod *)calloc(1, N * sizeof *P);
            size_t need = 0;
            uint64_t *pool;
            size_t *na = (size_t *)malloc(N * 8), *nb = (size_t *)malloc(N * 8);
            for (size_t i = 0; i < N; i++) {
                size_t nc = L / 2 + rng_next(&rng) % (L / 2) + 1; if (nc > L) nc = L;    /* nc in (L/2, L] */
                if (i == 0) nc = L;                                                     /* exactly L */
                na[i] = 1 + rng_next(&rng) % (nc - 1); nb[i] = nc - na[i];
                if (i == 0) { na[i] = L / 2; nb[i] = L / 2; }
            }
            int reg = bc[ci].reg & 1, shared = bc[ci].reg >= 2;
            if (shared) for (size_t i = 1; i < N; i++) { nb[i] = nb[0]; if (na[i] + nb[i] > L) na[i] = L - nb[i]; }
            for (size_t i = 0; i < N; i++) need += 2 * na[i] + nb[i] + (shared && i ? 0 : nb[i]);
            pool = reg ? (uint64_t *)mem_hreg_alloc(need * 8) : (uint64_t *)malloc(need * 8);
            size_t off = 0;
            for (size_t i = 0; i < N; i++) {
                P[i].a = pool + off; P[i].na = na[i]; off += na[i];
                P[i].b = (shared && i) ? P[0].b : pool + off; P[i].nb = nb[i]; if (!shared || i == 0) off += nb[i];
                P[i].c = pool + off; off += na[i] + nb[i];
                int kind = (int)(i % GEN_KINDS);
                gen_limbs((uint64_t *)P[i].a, na[i], kind, &rng);
                if (!shared || i == 0) gen_limbs((uint64_t *)P[i].b, nb[i], kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
            }
            memset(&rns_st, 0, sizeof rns_st);
            fprintf(stderr, "   running %s (need %zu limbs, pool %p)\n", bc[ci].why, need, (void *)pool);
            double t0 = mem_now();
            rns_mul_batch(P, N);
            double dt = mem_now() - t0;
            size_t bad = 0, pts = 0, nex = 0, badr = 0;
            int all_exact = need <= BATCH_EXACT_LIMBS;
            for (size_t i = 0; i < N; i++) {
                pts += L;
                if (!all_exact && !batch_sampled(i, N)) continue;
                mpz_from_limbs(a, P[i].a, P[i].na); mpz_from_limbs(b, P[i].b, P[i].nb);
                mpz_mul(c, a, b); mpz_from_limbs(d, P[i].c, P[i].na + P[i].nb); nex++;
                if (mpz_cmp(c, d) != 0) { if (!bad) printf("   first bad product %zu: na %zu nb %zu\n", i, P[i].na, P[i].nb); bad++; }
            }
            if (!all_exact) badr = batch_residues_bad(P, N);
            VERIFY(bad == 0 && badr == 0, "batch %s: %zu of %zu exact products wrong, %zu of %zu by residues", bc[ci].why, bad, nex, badr, all_exact ? 0 : N);
            printf("   %-34s N=%-6zu %.3f s  %.2f Gpoint/s  (%zu exact%s)\n", bc[ci].why, N, dt, pts / dt / 1e9, nex, all_exact ? "" : ", all by residues");
            if (N == 40000) harness_result("batch_2e14_Gpoints_per_s", "Gpoint/s", pts / dt / 1e9);
            if (reg) mem_hreg_free(pool); else free(pool);
            free(P); free(na); free(nb);
        }
        mpz_clears(a, b, c, d, NULL);
    }
    /* 4b. the locality-aware tier as the tree uses it - products in device pools, pairs
     * (P1 Q2 + P2, Q1 Q2) sharing B, 2^k and 3 2^k lengths, several tiles - vs GMP and the normalised lengths
     * (the unpaired form is still taken where a batch does not pair) */
    printf("-- 4b. batch-local: device-pool products in pairs sharing B\n");
    {
        struct { int logL; size_t npairs; int r3; size_t tile; const char *why; } bc[] = {
            {12, 40, 0, 0, "2^12, 40 pairs"}, {14, 300, 0, 0, "2^14, 300 pairs (fused pointwise)"},
            {17, 120, 1, 0, "3*2^15, 120 pairs"}, {20, 8, 1, 0, "3*2^18, 8 pairs"},
            {13, 8, 1, 0, "3*2^11, 8 pairs (local min)"}, {16, 90, 0, 60000000, "2^16, 90 pairs, 60 MB tiles"},
            {18, 30, 1, 30000000, "3*2^16, 30 pairs, 30 MB tiles (one pair per tile)"}, {15, 33, 1, 0, "3*2^13, 33 pairs (odd count per device)"},
        };
        mpz_t a, b, c, d, x; mpz_inits(a, b, c, d, x, NULL);
        int nd = mem_device_count(); if (nd > 4) nd = 4;
        for (size_t ci = 0; ci < sizeof bc / sizeof *bc; ci++) {
            size_t L = (size_t)1 << bc[ci].logL, np = bc[ci].npairs, N = 2 * np;
            size_t lo = bc[ci].r3 ? L / 2 + 1 : 3 * L / 4 + 1, hi = bc[ci].r3 ? 3 * L / 4 : L;   /* nc range selecting the length */
            rns_prod *P = (rns_prod *)calloc(1, N * sizeof *P);
            size_t need[4] = {0, 0, 0, 0}; uint64_t *pool[4]; size_t off[4] = {0, 0, 0, 0};
            size_t *na1 = (size_t *)malloc(np * 8), *na2 = (size_t *)malloc(np * 8), *nb = (size_t *)malloc(np * 8), *nx = (size_t *)malloc(np * 8);
            for (size_t j = 0; j < np; j++) {
                size_t nc = lo + rng_next(&rng) % (hi - lo + 1); if (j == 0) nc = hi;
                nb[j] = 1 + rng_next(&rng) % (nc - 1); na1[j] = nc - nb[j]; na2[j] = 1 + rng_next(&rng) % (nc - nb[j]);
                nx[j] = j % 3 == 2 ? 0 : 1 + rng_next(&rng) % (na1[j] + nb[j]);
                need[j % nd] += na1[j] + na2[j] + nb[j] + nx[j] + (na1[j] + nb[j] + 1) + (na2[j] + nb[j]);
            }
            for (int dv = 0; dv < nd; dv++) pool[dv] = (uint64_t *)mem_dev_alloc(dv, need[dv] * 8 + 64);
            for (size_t j = 0; j < np; j++) {
                int dv = (int)(j % nd); uint64_t *q = pool[dv]; size_t *o = &off[dv]; int kind = (int)(j % GEN_KINDS);
                rns_prod *p1 = &P[2 * j], *p2 = &P[2 * j + 1];
                p1->a = q + *o; p1->na = na1[j]; *o += na1[j];
                p2->a = q + *o; p2->na = na2[j]; *o += na2[j];
                p1->b = p2->b = q + *o; p1->nb = p2->nb = nb[j]; *o += nb[j];
                p1->x = nx[j] ? q + *o : 0; p1->nx = nx[j]; *o += nx[j];
                p1->c = q + *o; *o += na1[j] + nb[j] + 1;
                p2->c = q + *o; *o += na2[j] + nb[j];
                gen_limbs((uint64_t *)p1->a, na1[j], kind, &rng); gen_limbs((uint64_t *)p2->a, na2[j], kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
                gen_limbs((uint64_t *)p1->b, nb[j], kind == GEN_ZEROS ? GEN_ONES : kind, &rng);
                if (nx[j]) gen_limbs((uint64_t *)p1->x, nx[j], kind, &rng);
            }
            size_t save_tile = rns_batch_tile_bytes; if (bc[ci].tile) rns_batch_tile_bytes = bc[ci].tile;
            double dt[2] = {0, 0};
            { const int mode = 1;   /* paired */
                for (size_t i = 0; i < N; i++) { P[i].ncn = 0; memset(P[i].c, 0, (P[i].na + P[i].nb + (P[i].x ? 1 : 0)) * 8); }
                memset(&rns_st, 0, sizeof rns_st);
                double t0 = mem_now(); rns_mul_batch(P, N); dt[mode] = mem_now() - t0;
                size_t bad = 0, badn = 0, nex = 0, badr = 0;
                int all_exact = need[0] + need[1] + need[2] + need[3] <= BATCH_EXACT_LIMBS;
                for (size_t i = 0; i < N; i++) {
                    size_t nc = P[i].na + P[i].nb + (P[i].x ? 1 : 0);
                    if (all_exact || batch_sampled(i, N)) {
                        mpz_from_limbs(a, P[i].a, P[i].na); mpz_from_limbs(b, P[i].b, P[i].nb); mpz_mul(c, a, b);
                        if (P[i].x) { mpz_from_limbs(x, P[i].x, P[i].nx); mpz_add(c, c, x); }
                        mpz_from_limbs(d, P[i].c, nc); nex++;
                        if (mpz_cmp(c, d) != 0) { if (!bad) printf("   first bad product %zu: na %zu nb %zu nx %zu\n", i, P[i].na, P[i].nb, P[i].nx); bad++; }
                    }
                    size_t n = nc; while (n && P[i].c[n - 1] == 0) n--;
                    if (P[i].ncn != n) badn++;
                }
                if (!all_exact) badr = batch_residues_bad(P, N);
                VERIFY(bad == 0 && badr == 0, "batch-local %s pair=%d: %zu of %zu exact products wrong, %zu of %zu by residues", bc[ci].why, mode, bad, nex, badr, all_exact ? 0 : N);
                VERIFY(badn == 0, "batch-local %s pair=%d: %zu normalised lengths wrong", bc[ci].why, mode, badn);
                VERIFY(rns_st.n_batch_local == N && rns_st.n_batch_pair == (mode ? N : 0), "batch-local %s pair=%d: the local tier did not run as expected (local %zu paired %zu of %zu)", bc[ci].why, mode, rns_st.n_batch_local, rns_st.n_batch_pair, N);
            }
            printf("   %-48s N=%-5zu pair %.3f s  (%s)\n", bc[ci].why, N, dt[1],
                   need[0] + need[1] + need[2] + need[3] <= BATCH_EXACT_LIMBS ? "all exact" : "4 exact, all by residues");
            rns_batch_tile_bytes = save_tile;
            for (int dv = 0; dv < nd; dv++) mem_dev_free(pool[dv]);
            free(P); free(na1); free(na2); free(nb); free(nx);
        }
        mpz_clears(a, b, c, d, x, NULL);
    }
    split4c:
    /* 4c. RNS_BATCH_SPLIT (rns_mul_batch_split): products larger than 2^k limbs cut into operand pieces on one device, k forced (20, 21), against the
     * unsplit batch tier limb for limb (and the lengths) and by residues: all products sharing one B, pairs (P1 Q2 + P2, Q1 Q2), distinct B, with and without x, sizes 2^22 .. 2^24 points */
    printf("-- 4c. batch split: forced pieces of at most 2^k limbs vs the unsplit tier\n");
    {   /* the grid chooser on the shapes that failed gates (lopsided: the long operand is cut further) and its planner use */
        struct { size_t na, nb; int G, lg; } gs[] = { {132513923, 10032291, 1, 22}, {131605731, 7283159, 1, 22}, {32470355, 32789908, 1, 20}, {132513923, 10032291, 2, 22}, {1022929560 / 2, 1022929560 / 2, 2, 28} };
        for (size_t i = 0; i < sizeof gs / sizeof *gs; i++) {
            int ka = 0, kb = 0; int ok = rns_batch_split_shape(gs[i].na, gs[i].nb, gs[i].G, (size_t)1 << gs[i].lg, &ka, &kb);
            size_t sa = ok ? (gs[i].na + ka - 1) / ka : 0, sb = ok ? (gs[i].nb + kb - 1) / kb : 0;
            int want = !(gs[i].na == 32470355);   /* 32.5M x 32.8M within 2^20 needs ~65 x 65 pieces: refused (the engine and the planner say so) */
            VERIFY(ok == want && (!ok || ka * kb <= 1024) && (!ok || sa + sb <= ((size_t)1 << gs[i].lg)), "split grid for %zu x %zu within 2^%d: ok %d, %d x %d pieces of %zu x %zu", gs[i].na, gs[i].nb, gs[i].lg, ok, ka, kb, sa, sb);
            printf("   grid %zu x %zu within 2^%d: %d x %d pieces of %zu x %zu\n", gs[i].na, gs[i].nb, gs[i].lg, ka, kb, sa, sb);
        }
    }
    {
        struct { int k; int shape; int logL; const char *why; } sc[] = {
            {20, 0, 22, "one B (3 products), 2^22, k 20"}, {20, 1, 22, "pairs (2 pairs, x), 2^22, k 20"}, {21, 2, 23, "distinct B (2), 2^23, k 21"},
            {20, 1, 23, "pairs (2 pairs, x), 2^23, k 20"}, {21, 0, 24, "one B (2 products), 2^24, k 21"}, {20, 3, 22, "mixed: small + large, 2^22, k 20"},
            {26, 0, 27, "big: one B (2 products), 2^27, k 26"}, {26, 0, 28, "big: one B (2 products), 2^28, k 26"}, {27, 0, 28, "big: one B (2 products), 2^28, k 27"}, {28, 0, 29, "big: one B (2 products), 2^29, k 28: shifts above 2^31 bytes"},
        };
        for (size_t ci = only_big ? 6 : 0; ci < (only_big ? sizeof sc / sizeof *sc : 6); ci++) {
            size_t L = (size_t)1 << sc[ci].logL; int nou = sc[ci].logL >= 27; size_t N = sc[ci].shape == 0 ? (ci == 4 || nou ? 2 : 3) : sc[ci].shape == 1 ? 4 : sc[ci].shape == 2 ? 2 : 3;
            rns_prod *P = (rns_prod *)calloc(1, N * sizeof *P), *Pr = (rns_prod *)calloc(1, N * sizeof *Pr);
            size_t tot = 0, na[4], nbv[4], nx[4], bi[4];
            for (size_t i = 0; i < N; i++) {
                size_t nc = 3 * L / 4 + 1 + rng_next(&rng) % (L / 4);
                bi[i] = sc[ci].shape == 0 ? 0 : sc[ci].shape == 1 ? i / 2 : i;
                if (sc[ci].shape == 3 && i == 1) nc = 1000 + rng_next(&rng) % 5000;
                nbv[i] = 1 + nc / 3 + rng_next(&rng) % (nc / 3); na[i] = nc - nbv[i];
                nx[i] = (sc[ci].shape == 1 && i % 2 == 0) ? 1 + rng_next(&rng) % nc : 0;
            }
            for (size_t i = 0; i < N; i++) if (i && bi[i] == bi[i - 1]) nbv[i] = nbv[i - 1];
            for (size_t i = 0; i < N; i++) tot += na[i] + nbv[i] + nx[i] + (na[i] + nbv[i] + 1) * 2;
            uint64_t *pool = (uint64_t *)mem_dev_alloc(0, (tot + 4 * N) * 8 + 64), *q = pool;
            uint64_t *cr = (uint64_t *)mem_dev_alloc(0, (tot + 4 * N) * 8 + 64), *qr = cr;
            for (size_t i = 0; i < N; i++) {
                int kind = (int)(i % GEN_KINDS);
                P[i].na = na[i]; P[i].nb = nbv[i]; P[i].nx = nx[i];
                P[i].a = q; q += na[i];
                if (i && bi[i] == bi[i - 1]) P[i].b = P[i - 1].b; else { P[i].b = q; q += nbv[i]; gen_limbs((uint64_t *)P[i].b, nbv[i], kind, &rng); }
                P[i].x = nx[i] ? q : 0; q += nx[i];
                P[i].c = q; q += na[i] + nbv[i] + 1;
                gen_limbs((uint64_t *)P[i].a, na[i], kind, &rng); if (nx[i]) gen_limbs((uint64_t *)P[i].x, nx[i], kind, &rng);
                Pr[i] = P[i]; Pr[i].c = qr; qr += na[i] + nbv[i] + 1;
            }
            for (size_t i = 0; i < N; i++) { memset(P[i].c, 0xAB, (P[i].na + P[i].nb + 1) * 8); memset(Pr[i].c, 0, (P[i].na + P[i].nb + 1) * 8); }
            unsetenv("RNS_BATCH_SPLIT"); double t0 = mem_now(); if (!nou) rns_mul_batch(Pr, N); double dtu = mem_now() - t0;
            char kb[8]; snprintf(kb, sizeof kb, "%d", sc[ci].k); setenv("RNS_BATCH_SPLIT", kb, 1);
            t0 = mem_now(); rns_mul_batch(P, N); double dts = mem_now() - t0;
            unsetenv("RNS_BATCH_SPLIT");
            size_t bad = 0, badn = 0, badr = 0;
            for (size_t i = 0; i < N; i++) {
                size_t nc = P[i].na + P[i].nb + (P[i].x ? 1 : 0), n = nc; if (!nou && memcmp(P[i].c, Pr[i].c, nc * 8)) bad++;
                while (n && P[i].c[n - 1] == 0) n--;
                if (P[i].ncn != n) badn++;
                if (!prod_residues_ok(P[i].a, P[i].na, P[i].b, P[i].nb, P[i].x, P[i].nx, P[i].c, nc)) badr++;
            }
            VERIFY(bad == 0 && badn == 0 && badr == 0, "batch split %s: %zu of %zu products differ from the unsplit tier, %zu lengths wrong, %zu wrong by residues", sc[ci].why, bad, N, badn, badr);
            printf("   %-48s N=%zu unsplit %.3f s  split %.3f s\n", sc[ci].why, N, dtu, dts);
            mem_dev_free(pool); mem_dev_free(cr); free(P); free(Pr);
        }
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_mul");
}
