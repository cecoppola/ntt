/* t_primes - Phase 13a P3 (PLAN 29 E1): three primes instead of four for base-10^18 limbs.
 *
 *  1. the bound, exact (GMP): for every 3-prime subset of c 2^44 + 1, c = 240, 216, 207, 147, the product and the
 *     margin prod p / (n (B-1)^2) at n = 2^20 .. 2^33 terms and at the 3 2^k lengths 3 2^31 .. 3 2^33; binary 2^64 limbs
 *     for contrast; the subset of the pipeline ({0,1,2} = c 240, 216, 207) must be the best and above 1 at every length
 *     the primes allow; ec_np3_max_terms against GMP's floor((p0 p1 p2 - 1) / (B-1)^2)
 *  2. Garner from three residues at the extremes: v = n (B-1)^2 for n = 2^20 .. 2^33, the largest allowed
 *     (max_terms (B-1)^2), p0 p1 p2 - 1, and 10^6 random v < p0 p1 p2: crt_garner3 and ec_words_to_dec3 against GMP
 *  3. crt_carry_par4 with ec_np = 3 (decimal): planes whose every coefficient is one extreme value W (the 2^33 worst case,
 *     the largest allowed, p0 p1 p2 - 1) at 2^16 coefficients and T = 1, 7, 96, 192 against GMP; the exact convolution
 *     profile of two all-(B-1) operands of 2^22 limbs (peak 2^22 (B-1)^2) against the closed form (B^n - 1)^2; and the
 *     three-prime result against the four-prime one on the same coefficients
 *  4. (argument gpu [log2 of the largest dist operand, 30]) end to end on the GPUs at the ec_np of the environment
 *     (ECALC_NP=3 or 4, decimal limbs): all-(B-1) operands (every coefficient at its maximum, the worst case) n x n
 *     through rns_mul (mdev / Karatsuba) and rns_mul_dist up to n = 2^30 (2^31 points, peak coefficient 2^30 (B-1)^2),
 *     checked against the closed form B^2n - 2 B^n + 1 (GMP-checked itself at n <= 2^20); random operands through the
 *     same tiers and the batch tier against mpz_mul (<= 2^20 limbs) or the mod 2^61-1 residue (above)
 *
 * Usage: t_primes [gpu [LOGMAX (30)]]      (parts 1-3 need no GPU; ECALC_NP is read by part 4 only)
 */
#include "harness.h"
#include "../crt.h"
#include "../modarith.h"
#include "../mem.h"
#include <math.h>
#include <omp.h>
#ifdef T_PRIMES_GPU
#include "../rns_mul.h"
#endif

static const uint64_t B = BI_B10;

static void mpz_from_words(mpz_t z, const uint64_t *w, size_t n) { mpz_import(z, n, -1, 8, 0, 0, w); }
static double mpz_log2(const mpz_t z) { long e; double m = mpz_get_d_2exp(&e, z); return e + log2(m); }

/* ---- 1. the bound ---------------------------------------------------------------------------------------------- */
static void part1(void)
{
    printf("-- 1. the three-prime bound: margin = prod p / (n (B-1)^2), B = 10^18 (exact, GMP)\n");
    mpz_t P, D, Db, t, u; mpz_inits(P, D, Db, t, u, NULL);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);                                   /* (B-1)^2 */
    mpz_set_ui(Db, 1); mpz_mul_2exp(Db, Db, 64); mpz_sub_ui(Db, Db, 1); mpz_mul(Db, Db, Db);   /* (2^64-1)^2 */
    static const int cs[4] = { 240, 216, 207, 147 };
    double best = 0; int bestdrop = -1;
    printf("   %-18s %8s", "subset (c)", "log2 P");
    for (int k = 20; k <= 33; k++) if (k >= 26 || k == 20) printf("   2^%d", k);
    printf("  3*2^31  3*2^32  3*2^33\n");
    for (int drop = 3; drop >= -1; drop--) {                                  /* drop = -1: all four (for reference) */
        mpz_set_ui(P, 1); char name[64] = ""; int nn = 0;
        for (int i = 0; i < 4; i++) if (i != drop) { mpz_mul_ui(P, P, ec_P[i]); nn += snprintf(name + nn, sizeof name - nn, "%s%d", nn ? "," : "", cs[i]); }
        VERIFY(drop < 0 || mpz_sizeinbase(P, 2) <= 156, "3-prime product < 2^156");
        printf("   %-18s %8.3f", name, mpz_log2(P));
        for (int k = 20; k <= 33; k++) {
            if (k < 26 && k != 20) continue;
            mpz_mul_2exp(t, D, k); mpz_set(u, P);
            printf(" %6.3g", mpz_get_d(u) / mpz_get_d(t));
        }
        for (int k = 31; k <= 33; k++) { mpz_mul_2exp(t, D, k); mpz_mul_ui(t, t, 3); printf(" %7.3g", mpz_get_d(P) / mpz_get_d(t)); }
        printf("\n");
        if (drop >= 0) {
            mpz_mul_2exp(t, D, 33); double m33 = mpz_get_d(P) / mpz_get_d(t);
            if (m33 > best) { best = m33; bestdrop = drop; }
            if (drop == 3) {                                                  /* the pipeline's subset: primes 0, 1, 2 */
                for (int k = 20; k <= 33; k++) { mpz_mul_2exp(t, D, k); VERIFY(mpz_cmp(t, P) < 0, "2^%d (B-1)^2 < p0 p1 p2", k); }
                for (int k = 20; k <= 33; k++) { mpz_mul_2exp(t, D, k); mpz_mul_ui(t, t, 3); VERIFY(mpz_cmp(t, P) < 0, "3 2^%d (B-1)^2 < p0 p1 p2", k); }
                mpz_sub_ui(u, P, 1); mpz_fdiv_q(u, u, D);
                crt_init();
                VERIFY(mpz_cmp_ui(u, (unsigned long)ec_np3_max_terms) == 0, "ec_np3_max_terms %zu == GMP %lu", ec_np3_max_terms, mpz_get_ui(u));
                printf("   the pipeline's subset {240,216,207}: at most %zu terms (2^%.3f); margin 27x at 2^31 is %.2fx, at 2^33 %.2fx; "
                       "3 2^33 (the longest 3 2^k length) %.3fx\n", ec_np3_max_terms, log2((double)ec_np3_max_terms),
                       mpz_get_d(P) / (mpz_get_d(D) * 0x1p31), mpz_get_d(P) / (mpz_get_d(D) * 0x1p33), mpz_get_d(P) / (mpz_get_d(D) * 3 * 0x1p33));
                harness_result("margin_2e31", "x", mpz_get_d(P) / (mpz_get_d(D) * 0x1p31));
                harness_result("margin_2e33", "x", mpz_get_d(P) / (mpz_get_d(D) * 0x1p33));
                /* binary limbs: the same three primes hold only below 2^27 terms */
                mpz_fdiv_q(u, P, Db);
                printf("   binary 2^64 limbs, the same subset: at most %lu terms (2^%.2f) -- ECALC_NP=3 is refused with LIMB_BASE=2\n", mpz_get_ui(u), log2(mpz_get_d(u)));
                VERIFY(mpz_cmp_ui(u, 1UL << 28) < 0, "binary limbs do not fit three primes at 2^28 terms");
            }
        }
    }
    printf("   best subset for the margin: drop c = %d (%.2fx at 2^33); the four primes share the kernels (c 2^44 + 1, 3 | c: 2^k and 3 2^k lengths, p < 2^52 for the FP64 Barrett)\n", cs[bestdrop], best);
    VERIFY(bestdrop == 3, "the pipeline's subset {0,1,2} has the best margin");
    mpz_clears(P, D, Db, t, u, NULL);
}

/* ---- 2. Garner at the extremes ------------------------------------------------------------------------------------ */
static int garner_one(const mpz_t v, const char *what)
{
    uint64_t r[3], w[3] = { 0, 0, 0 }, d[3];
    for (int i = 0; i < 3; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]);
    crt_garner3(r, w); ec_words_to_dec3(w, d);
    mpz_t g, q, rem; mpz_inits(g, q, rem, NULL);
    mpz_from_words(g, w, 3);
    int ok = mpz_cmp(g, v) == 0;
    mpz_set(q, v);
    for (int k = 0; k < 3 && ok; k++) { mpz_t bb; mpz_init_set_ui(bb, B); mpz_fdiv_qr(q, rem, q, bb); ok = mpz_cmp_ui(rem, (unsigned long)d[k]) == 0 && d[k] < B; mpz_clear(bb); }
    ok = ok && mpz_sgn(q) == 0;
    if (!ok) VERIFY(0, "garner3 %s", what);
    else hv_checks++;
    mpz_clears(g, q, rem, NULL);
    return ok;
}
static void part2(void)
{
    printf("-- 2. crt_garner3 + ec_words_to_dec3 at the extremes, against GMP\n");
    mpz_t P, D, v; mpz_inits(P, D, v, NULL);
    mpz_set_ui(P, 1); for (int i = 0; i < 3; i++) mpz_mul_ui(P, P, ec_P[i]);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);
    char what[64]; int bad = 0;
    for (int k = 20; k <= 33; k++) { mpz_mul_2exp(v, D, k); snprintf(what, sizeof what, "2^%d (B-1)^2", k); bad += !garner_one(v, what);
                                     mpz_mul_ui(v, v, 3); if (mpz_cmp(v, P) < 0) { snprintf(what, sizeof what, "3 2^%d (B-1)^2", k); bad += !garner_one(v, what); } }
    mpz_mul_ui(v, D, (unsigned long)ec_np3_max_terms); bad += !garner_one(v, "max_terms (B-1)^2");
    mpz_sub_ui(v, P, 1); bad += !garner_one(v, "p0 p1 p2 - 1");
    mpz_set_ui(v, 0); bad += !garner_one(v, "0");
    mpz_set_ui(v, B - 1); bad += !garner_one(v, "B-1");
    mpz_set_ui(v, B); bad += !garner_one(v, "B");
    gmp_randstate_t rs; gmp_randinit_default(rs); gmp_randseed_ui(rs, 0x9E37);
    for (int i = 0; i < 1000000; i++) { mpz_urandomm(v, rs, P); if (!garner_one(v, "random")) bad++; }
    gmp_randclear(rs);
    printf("   %d extreme + 10^6 random values: %s\n", 33 - 20 + 1 + 5, bad ? "MISMATCH" : "all exact");
    mpz_clears(P, D, v, NULL);
}

/* ---- 3. the CPU CRT with carry at ec_np = 3 ----------------------------------------------------------------------- */
static void residues_of(const mpz_t v, uint64_t r[4]) { for (int i = 0; i < 4; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]); }
static void part3(void)
{
    printf("-- 3. crt_carry_par4 at ec_np = 3, decimal limbs\n");
    int save = ec_np; ec_np = 3; bi_set_decimal(1); crt_init();
    mpz_t P, D, W, v, w, bm; mpz_inits(P, D, W, v, w, bm, NULL);
    mpz_set_ui(P, 1); for (int i = 0; i < 3; i++) mpz_mul_ui(P, P, ec_P[i]);
    mpz_set_ui(D, B - 1); mpz_mul(D, D, D);
    size_t m = (size_t)1 << 16; uint64_t *res[4], *out = (uint64_t *)malloc((m + 4) * 8);
    for (int i = 0; i < 4; i++) res[i] = (uint64_t *)malloc(m * 8);
    static const int Ts[] = { 1, 7, 96, 192 };
    for (int which = 0; which < 3; which++) {
        const char *nm = which == 0 ? "2^33 (B-1)^2" : which == 1 ? "max_terms (B-1)^2" : "p0 p1 p2 - 1";
        if (which == 0) mpz_mul_2exp(W, D, 33); else if (which == 1) mpz_mul_ui(W, D, (unsigned long)ec_np3_max_terms); else mpz_sub_ui(W, P, 1);
        uint64_t r[4]; residues_of(W, r);
        for (int i = 0; i < 4; i++) for (size_t k = 0; k < m; k++) res[i][k] = i < 3 ? r[i] : 0xDEADBEEFDEADBEEFULL;   /* plane 3 must not be read */
        mpz_set_ui(bm, B); mpz_pow_ui(v, bm, m); mpz_sub_ui(v, v, 1); mpz_divexact_ui(v, v, B - 1); mpz_mul(v, v, W);    /* W (B^m - 1) / (B - 1) */
        for (int ti = 0; ti < 4; ti++) {
            for (size_t k = 0; k < m + 4; k++) out[k] = 0xDEADBEEFULL;
            crt_carry_par4(res, m, out, Ts[ti]);
            mpz_from_limbs(w, out, m + 4);
            VERIFY(mpz_cmp(v, w) == 0, "every coefficient %s, n=%zu T=%d", nm, m, Ts[ti]);
        }
    }
    /* the exact convolution profile of two all-(B-1) operands of n limbs: c_k = min(k+1, 2n-1-k) (B-1)^2 */
    {
        size_t n = (size_t)1 << 22, nc = 2 * n; uint64_t *pl[4], *o = (uint64_t *)malloc((nc + 4) * 8), *o4 = (uint64_t *)malloc((nc + 4) * 8);
        for (int i = 0; i < 4; i++) pl[i] = (uint64_t *)malloc(nc * 8);
        unsigned __int128 d2 = (unsigned __int128)(B - 1) * (B - 1);
        double t0 = now();
#pragma omp parallel for schedule(static)
        for (size_t k = 0; k < nc; k++) {
            uint64_t cnt = k + 1 < nc - 1 - k + 1 ? k + 1 : nc - 1 - k; if (k == nc - 1) cnt = 0;
            for (int i = 0; i < 4; i++) { unsigned __int128 x = d2 % ec_P[i]; pl[i][k] = (uint64_t)((x * (cnt % ec_P[i])) % ec_P[i]); }
        }
        crt_carry_par4(pl, nc, o, 96);
        double t1 = now();
        int ok = o[0] == 1; for (size_t k = 1; k < n && ok; k++) ok = o[k] == 0;
        ok = ok && o[n] == B - 2; for (size_t k = n + 1; k < nc && ok; k++) ok = o[k] == B - 1;
        for (size_t k = nc; k < nc + 4 && ok; k++) ok = o[k] == 0;
        VERIFY(ok, "all-(B-1) profile at n = 2^22 (peak 2^22 (B-1)^2) == (B^n - 1)^2");
        ec_np = 4; crt_carry_par4(pl, nc, o4, 96); ec_np = 3;
        VERIFY(memcmp(o, o4, (nc + 4) * 8) == 0, "three-prime CRT == four-prime CRT on the same coefficients (2^23)");
        printf("   profile of (B^n-1)^2 at n = 2^22 (2^23 coefficients): %s, %.2f s\n", ok ? "exact" : "WRONG", t1 - t0);
        for (int i = 0; i < 4; i++) free(pl[i]); free(o); free(o4);
    }
    /* throughput: three against four primes on the same random coefficients < p0 p1 p2 (2^26 coefficients, T = 192) */
    {
        size_t n = (size_t)1 << 26; uint64_t *pl[4], *o = (uint64_t *)malloc((n + 4) * 8);
        for (int i = 0; i < 4; i++) pl[i] = (uint64_t *)malloc(n * 8);
#pragma omp parallel
        { rng_t r = { 0xF00D + (uint64_t)omp_get_thread_num() };
#pragma omp for schedule(static)
          for (size_t k = 0; k < n; k++) { uint64_t a = rng_next(&r) % (B - 1), b = rng_next(&r) % (B - 1), c = rng_next(&r) % 1000000;   /* v = a b c-ish: (a b mod p) scaled */
                                           for (int i = 0; i < 4; i++) pl[i][k] = (uint64_t)(((unsigned __int128)(a % ec_P[i]) * (b % ec_P[i]) % ec_P[i]) * (c % ec_P[i]) % ec_P[i]); } }
        for (int np = 4; np >= 3; np--) {
            ec_np = np; double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { double t0 = now(); crt_carry_par4(pl, n, o, 192); double dt = now() - t0; if (dt < best) best = dt; }
            printf("   CPU CRT (decimal) 2^26 coefficients, T=192: %d primes %.3f s (%.2f Gcoef/s)\n", np, best, n / best / 1e9);
        }
        ec_np = 3;
        for (int i = 0; i < 4; i++) free(pl[i]); free(o);
    }
    for (int i = 0; i < 4; i++) free(res[i]); free(out);
    mpz_clears(P, D, W, v, w, bm, NULL);
    ec_np = save;
}

#ifdef T_PRIMES_GPU
/* ---- 4. end to end on the GPUs ------------------------------------------------------------------------------------ */
static int allmax_ok(const bigint *C, size_t n)
{
    if (C->n != 2 * n) return 0;
    if (C->l[0] != 1) return 0;
    for (size_t k = 1; k < n; k++) if (C->l[k]) return 0;
    if (C->l[n] != B - 2) return 0;
    for (size_t k = n + 1; k < 2 * n; k++) if (C->l[k] != B - 1) return 0;
    return 1;
}
static void check_rand(const char *what, const bigint *A, const bigint *Bb, const bigint *C)
{
    if (A->n + Bb->n <= ((size_t)1 << 21)) {
        mpz_t a, b, c; mpz_inits(a, b, c, NULL); bi_to_mpz(a, A); bi_to_mpz(b, Bb); mpz_mul(c, a, b);
        VERIFY(bi_eq_mpz(C, c), "%s: product differs from mpz_mul", what); mpz_clears(a, b, c, NULL);
    } else {
        uint64_t ra = limbs_mod_m61(A->l, A->n), rb = limbs_mod_m61(Bb->l, Bb->n), rc = limbs_mod_m61(C->l, C->n);
        VERIFY(m61_mul(ra, rb) == rc, "%s: residue mod 2^61-1 differs", what);
    }
}
static void part4(int logmax)
{
    bi_set_decimal(1);
    rns_init(31);
    printf("-- 4. GPU end to end at ec_np = %d (ECALC_NP), decimal limbs, pools 2^31 (plane pool 0: %zu limbs)\n", ec_np, rns_plane_limbs());
    bigint A, Bb, C; bi_init(&A); bi_init(&Bb); bi_init(&C);
    rng_t rng = { 0x3B1ULL };
    /* the closed form itself against GMP */
    { size_t n = (size_t)1 << 20; bi_random(&A, n, GEN_ONES, &rng); bi_random(&Bb, n, GEN_ONES, &rng); rns_mul(&C, &A, &Bb);
      mpz_t a, b, c; mpz_inits(a, b, c, NULL); bi_to_mpz(a, &A); bi_to_mpz(b, &Bb); mpz_mul(c, a, b);
      VERIFY(bi_eq_mpz(&C, c) && allmax_ok(&C, n), "all-(B-1) 2^20 x 2^20: rns_mul == mpz_mul == closed form"); mpz_clears(a, b, c, NULL); }
    for (int tier = 0; tier < 2; tier++) {
        for (int lg = 19; lg <= logmax; lg++) {
            size_t n = (size_t)1 << lg;
            if (tier == 0 && lg > 29) break;                                 /* rns_mul: mdev up to the pool, Karatsuba above (2^29: 2 x 2^30 host) */
            bi_random(&A, n, GEN_ONES, &rng); bi_random(&Bb, n, GEN_ONES, &rng);
            double t0 = now();
            if (tier == 0) rns_mul(&C, &A, &Bb); else rns_mul_dist(&C, &A, &Bb);
            double t1 = now();
            VERIFY(allmax_ok(&C, n), "%s all-(B-1) 2^%d x 2^%d (peak coefficient 2^%d (B-1)^2)", tier ? "dist" : "rns_mul", lg, lg, lg);
            printf("   %-8s all-(B-1) 2^%d x 2^%d (2^%d points): %s  %.2f s\n", tier ? "dist" : "rns_mul", lg, lg, lg + 1, allmax_ok(&C, n) ? "exact" : "WRONG", t1 - t0);
            if (lg <= 25 || lg == logmax) {                                   /* random operands, the same sizes (uneven) */
                bi_random(&A, n + 3, GEN_UNIFORM, &rng); bi_random(&Bb, n - 5, GEN_UNIFORM, &rng);
                if (tier == 0) rns_mul(&C, &A, &Bb); else rns_mul_dist(&C, &A, &Bb);
                char what[64]; snprintf(what, sizeof what, "%s random 2^%d", tier ? "dist" : "rns_mul", lg); check_rand(what, &A, &Bb, &C);
            }
        }
    }
    /* the batch tier (striped, staged operands: host memory): 16 products each of 2^k x 2^k limbs */
    for (int lg = 10; lg <= 18; lg += 4) {
        size_t n = (size_t)1 << lg, N = 16; rns_prod P[16]; bigint As[16], Bs[16]; uint64_t *cs[16];
        for (size_t i = 0; i < N; i++) { bi_init(&As[i]); bi_init(&Bs[i]); bi_random(&As[i], n, i & 1 ? GEN_ONES : GEN_UNIFORM, &rng); bi_random(&Bs[i], n, i & 1 ? GEN_ONES : GEN_UNIFORM, &rng);
                                         cs[i] = (uint64_t *)calloc(2 * n + 1, 8); P[i].a = As[i].l; P[i].na = As[i].n; P[i].b = Bs[i].l; P[i].nb = Bs[i].n; P[i].c = cs[i]; P[i].x = 0; P[i].nx = 0; P[i].ncn = 0; }
        rns_mul_batch(P, N);
        for (size_t i = 0; i < N; i++) { bigint Cv = { cs[i], 2 * n, 2 * n + 1 }; bi_norm(&Cv); char what[64]; snprintf(what, sizeof what, "batch 2^%d #%zu", lg, i);
                                         if (i & 1) VERIFY(allmax_ok(&Cv, n), "%s all-(B-1)", what); else check_rand(what, &As[i], &Bs[i], &Cv);
                                         free(cs[i]); bi_free(&As[i]); bi_free(&Bs[i]); }
        printf("   batch    16 products of 2^%d x 2^%d: checked\n", lg, lg);
    }
    bi_free(&A); bi_free(&Bb); bi_free(&C);
}
#endif

int main(int argc, char **argv)
{
    printf("== t_primes ==\n");
    harness_meta("t_primes");
    int gpu = argc > 1 && !strcmp(argv[1], "gpu");
    int logmax = argc > 2 ? atoi(argv[2]) : 30;
    if (!gpu) {
        crt_init();
        part1(); part2(); part3();
    } else {
#ifdef T_PRIMES_GPU
        part4(logmax);
#else
        (void)logmax; printf("built without the GPU part\n");
#endif
    }
    return verify_done("t_primes");
}
