/* t_crt - step 4 test: crt.h against GMP, adversarial carries, throughput.
 *
 *  1. random residues, n = 2^16, T = 1, 7, 96, 192: value(out) == sum_k
 *     CRT(r_k) 2^(64k) computed by GMP; n = 5 (fewer coefficients than stripes)
 *  2. adversarial: every coefficient = M - 1 (all four residues p_i - 1), so
 *     every limb carries and every stripe spills; and a single M - 1 at a
 *     stripe boundary with zeros elsewhere; both layouts
 *  3. throughput at n = 2^30 (4 planes x 8 GiB): plane placement (a) each
 *     plane first-touched by the stripe that reads it (bench/20), (b) one
 *     plane per NUMA node (mdev's staging), (c) quartered node-local; T = 48,
 *     96, 192; best of 3.  Paper: CPU CRT must keep up with the multiply
 *     (RESULTS.md 27, 34: 0.29 s / 2^30 tight window, clang).
 *
 * Phase 13a P3: ECALC_NP=3 runs parts 1 and 2 with three primes (the value mod p0 p1 p2) in decimal limbs (three primes
 * are decimal-only: the binary base is switched to decimal with a note); part 3 is binary-only and is skipped then.
 *
 * Usage: t_crt [log2 n for part 3 (30)]
 */
#include "harness.h"
#include "../crt.h"
#include "../mem.h"
#include "../modarith.h"
#include <omp.h>

static mpz_t M, E[4];        /* E[i] = (M/p_i) * ((M/p_i)^-1 mod p_i) */
static void crt_gmp_init(void)
{
    mpz_t t, u; mpz_inits(M, t, u, NULL);
    crt_init();                                          /* (reads ECALC_NP) */
    mpz_set_ui(M, 1);
    for (int i = 0; i < ec_np; i++) mpz_mul_ui(M, M, ec_P[i]);
    for (int i = 0; i < ec_np; i++) {
        mpz_init(E[i]);
        mpz_divexact_ui(t, M, ec_P[i]);
        mpz_set_ui(u, ec_P[i]);
        mpz_invert(u, t, u);
        mpz_mul(E[i], t, u);
    }
    mpz_clears(t, u, NULL);
}
/* value of residue planes as a GMP integer */
static void crt_gmp(mpz_t v, uint64_t *const res[4], size_t n)
{
    mpz_t c, t; mpz_inits(c, t, NULL);
    mpz_set_ui(v, 0);
    for (size_t k = n; k-- > 0;) {
        mpz_set_ui(c, 0);
        for (int i = 0; i < ec_np; i++) { mpz_mul_ui(t, E[i], res[i][k]); mpz_add(c, c, t); }
        mpz_mod(c, c, M);
        if (bi_decimal) mpz_mul_ui(v, v, BI_B10); else mpz_mul_2exp(v, v, 64);
        mpz_add(v, v, c);
    }
    mpz_clears(c, t, NULL);
}
static void to_quartered(uint64_t *const res[4], size_t n, uint64_t *q[4], size_t Q)
{
    for (int qq = 0; qq < 4; qq++) {
        size_t q0 = qq * Q, q1 = (qq + 1) * Q < n ? (qq + 1) * Q : n; if (q0 > n) q0 = n;
        for (int d = 0; d < 4; d++) memcpy(q[qq] + d * Q, res[d] + q0, (q1 - q0) * 8);
    }
}
static int check(const char *what, uint64_t *const res[4], size_t n, int T, int layout)
{
    if (layout && bi_decimal) return 1;                   /* the quartered layout is binary-only */
    uint64_t *out = (uint64_t *)malloc((n + 4) * 8);
    mpz_t v, w; mpz_inits(v, w, NULL);
    for (size_t i = 0; i < n + 4; i++) out[i] = 0xDEADBEEFULL;
    if (layout) {
        size_t Q = (n + 3) / 4; uint64_t *q[4];
        for (int i = 0; i < 4; i++) q[i] = (uint64_t *)malloc(4 * Q * 8 + 8);
        to_quartered(res, n, q, Q);
        crt_carry_par4_q(q, Q, n, out, T);
        for (int i = 0; i < 4; i++) free(q[i]);
    } else crt_carry_par4(res, n, out, T);
    crt_gmp(v, res, n);
    mpz_from_limbs(w, out, n + 4);
    int ok = mpz_cmp(v, w) == 0;
    VERIFY(ok, "%s n=%zu T=%d layout %d", what, n, T, layout);
    mpz_clears(v, w, NULL); free(out);
    return ok;
}

int main(int argc, char **argv)
{
    int LOG = argc > 1 ? atoi(argv[1]) : 30;
    rng_t rng = {0xC47ULL};
    printf("== t_crt ==\n");
    harness_meta("t_crt");
    crt_gmp_init();
    if (ec_np == 3 && !bi_decimal) { printf("   ECALC_NP=3: three primes are decimal-only -- running parts 1 and 2 in base 10^18\n"); bi_set_decimal(1); }
    printf("   %d primes, %s limbs\n", ec_np, bi_decimal ? "decimal" : "binary");
    /* 0. Phase 15 NP: ECALC_NP=auto (run with LIMB_BASE=10; ECALC_NP_AUTO_TERMS lowers the switch-over) -- the per-product count
     *    and pool 0's planes, and the two CRTs agree on every value three primes hold: v = k (B-1)^2 + random < p0 p1 p2 for k up
     *    to the bound gives garner3(v mod p0..p2) == garner4(v mod p0..p3) == v, and at the bound + 1 garner3 wraps (the bound is exact) */
    if (ec_np_auto) {
        size_t b3 = ec_np3_max_terms, bt = ec_np_auto_terms;
        printf("-- 0. ECALC_NP=auto: three-prime bound %zu terms, auto switch-over %zu terms\n", b3, bt);
        VERIFY(ec_np == 3 && bi_decimal, "auto: ec_np %d decimal %d", ec_np, bi_decimal);
        VERIFY(ec_np_for(1) == 3 && ec_np_for(bt) == 3 && ec_np_for(bt + 1) == 4 && ec_np_for(b3 + 1) == 4 && ec_np_for((size_t)1 << 40) == 4, "ec_np_for around %zu", bt);
        VERIFY(ec_np_prod(bt, 1, "t_crt") == 3 && ec_np_prod(bt + 1, 1, "t_crt") == 4, "ec_np_prod");
        /* Phase 15 MPB: ECALC_NP_AUTO_MIN=1 -- the term count min(na, nb) (a product with min <= bt < na + nb runs three primes); off: nc */
        printf("   ECALC_NP_AUTO_MIN=%d\n", ec_np_auto_min);
        if (ec_np_auto_min) {
            VERIFY(ec_np_terms(2 * bt, bt, bt) == bt && ec_np_terms(bt + 7, 7, bt) == 7 && ec_np_terms(bt + 7, bt, 7) == 7, "ec_np_terms (min)");
            VERIFY(ec_np_for(ec_np_terms(2 * bt, bt, bt)) == 3 && ec_np_for(ec_np_terms(bt + 1 + 5 * bt, bt + 1, 5 * bt)) == 4 && ec_np_for(ec_np_terms(bt + 1, 1, bt)) == 3, "ec_np_for by min");
            VERIFY(ec_np_prod(ec_np_terms(2 * bt, bt, bt), 1, "t_crt") == 3, "ec_np_prod by min (the check passes at min = bt)");
        } else VERIFY(ec_np_terms(2 * bt, bt, bt) == 2 * bt && ec_np_terms(bt + 1, 1, bt) == bt + 1, "ec_np_terms (pa + pb)");
        for (int pl = 27; pl <= 31; pl++) for (int g = 1; g <= 1024; g = g < 4 ? g + 1 : g * 3 / 2) {
            int lg = 0; while ((2 << lg) <= g) lg++;
            int want = ((size_t)1 << (pl + lg)) > bt ? 4 : 3;
            VERIFY(ec_np_planes(pl, g) == want, "ec_np_planes(%d, %d) = %d, want %d", pl, g, ec_np_planes(pl, g), want);
        }
        printf("   pool 0 planes at pool_log 31: g 1 -> %d, 2 -> %d, 32 -> %d, 576 -> %d\n", ec_np_planes(31, 1), ec_np_planes(31, 2), ec_np_planes(31, 32), ec_np_planes(31, 576));
        mpz_t v, D, P3, t; mpz_inits(v, D, P3, t, NULL);
        mpz_set_ui(D, BI_B10 - 1); mpz_mul(D, D, D);                          /* (B-1)^2 */
        mpz_set_ui(P3, ec_P[0]); mpz_mul_ui(P3, P3, ec_P[1]); mpz_mul_ui(P3, P3, ec_P[2]);
        int agree = 0, n3 = 0;
        for (int it = 0; it < 20000; it++) {
            size_t k = it == 0 ? b3 : it == 1 ? 1 : it == 2 ? bt : 1 + rng_next(&rng) % b3;
            mpz_mul_ui(v, D, k);                                              /* a coefficient of k terms at its largest */
            if (it > 2) { mpz_set_ui(t, rng_next(&rng)); mpz_mul_ui(t, t, (unsigned long)(k & 0xffffffff)); mpz_sub(v, v, t); if (mpz_sgn(v) < 0) mpz_set_ui(v, 0); }
            uint64_t r[4], o3[3], o4[4]; for (int i = 0; i < 4; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]);
            crt_garner3(r, o3); crt_garner4(r, o4);
            mpz_t w3, w4; mpz_inits(w3, w4, NULL); mpz_import(w3, 3, -1, 8, 0, 0, o3); mpz_import(w4, 4, -1, 8, 0, 0, o4);   /* (binary words: garner's output) */
            int ok = !mpz_cmp(w3, v) && !mpz_cmp(w4, v); agree += ok; n3++;
            if (!ok && it < 5) printf("   k %zu: garner3 %s, garner4 %s\n", k, mpz_cmp(w3, v) ? "WRONG" : "ok", mpz_cmp(w4, v) ? "WRONG" : "ok");
            mpz_clears(w3, w4, NULL);
        }
        VERIFY(agree == n3, "garner3 == garner4 == v for values of at most the bound's terms: %d of %d", agree, n3);
        { mpz_mul_ui(v, D, b3 + 1); uint64_t r[4], o3[3], o4[4]; for (int i = 0; i < 4; i++) r[i] = mpz_fdiv_ui(v, ec_P[i]);
          crt_garner3(r, o3); crt_garner4(r, o4); mpz_t w3, w4; mpz_inits(w3, w4, NULL); mpz_import(w3, 3, -1, 8, 0, 0, o3); mpz_import(w4, 4, -1, 8, 0, 0, o4);   /* (binary words: garner's output) */
          VERIFY(mpz_cmp(v, P3) >= 0 && mpz_cmp(w3, v) != 0 && mpz_cmp(w4, v) == 0, "at the bound + 1 (%zu terms): three primes wrap, four hold", b3 + 1);
          mpz_clears(w3, w4, NULL); }
        printf("   garner3 == garner4 on %d values up to %zu terms of (B-1)^2; at %zu terms three primes wrap and four hold\n", n3, b3, b3 + 1);
        mpz_clears(v, D, P3, t, NULL);
    }
    /* 1 + 2 */
    {
        size_t n = 1 << 16; uint64_t *res[4];
        for (int i = 0; i < 4; i++) res[i] = (uint64_t *)malloc(n * 8);
        static const int Ts[] = {1, 7, 96, 192};
        for (int i = 0; i < 4; i++) for (size_t k = 0; k < n; k++) res[i][k] = rng_next(&rng) % ec_P[i];
        for (int ti = 0; ti < 4; ti++) for (int lay = 0; lay < 2; lay++) check("random", res, n, Ts[ti], lay);
        check("random", res, 5, 192, 0); check("random", res, 5, 192, 1);
        check("random", res, 1, 1, 0);
        for (int i = 0; i < 4; i++) for (size_t k = 0; k < n; k++) res[i][k] = ec_P[i] - 1;
        for (int ti = 0; ti < 4; ti++) for (int lay = 0; lay < 2; lay++) check("all M-1", res, n, Ts[ti], lay);
        for (int i = 0; i < 4; i++) memset(res[i], 0, n * 8);
        size_t kb = n * 5 / 96;                      /* first limb of stripe 5 at T = 96 */
        for (int i = 0; i < 4; i++) res[i][kb - 1] = res[i][kb] = ec_P[i] - 1;
        for (int lay = 0; lay < 2; lay++) { check("boundary M-1", res, n, 96, lay); check("boundary M-1", res, n, 192, lay); }
        for (int i = 0; i < 4; i++) free(res[i]);
    }
    /* 3. throughput */
    if (!bi_decimal) {
        size_t n = (size_t)1 << LOG, Q = n / 4;
        uint64_t *res[4], *q[4], *out = (uint64_t *)malloc((n + 4) * 8);
        printf("-- throughput, n = 2^%d, best of 3 (s)\n", LOG);
        static const int Ts[] = {48, 96, 192};
        /* (a) stripe-local first touch (static schedule, same as the CRT loop at T = 192) */
        for (int i = 0; i < 4; i++) res[i] = (uint64_t *)malloc(n * 8);
#pragma omp parallel for num_threads(192) schedule(static)
        for (int t = 0; t < 192; t++) {
            size_t k0 = n * t / 192, k1 = n * (t + 1) / 192; rng_t r = {(uint64_t)(0xABC + t)};
            for (size_t k = k0; k < k1; k++) { for (int i = 0; i < 4; i++) res[i][k] = rng_next(&r) % ec_P[i]; out[k] = 0; }
        }
        for (int ti = 0; ti < 3; ti++) {
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { double t0 = mem_now(); crt_carry_par4(res, n, out, Ts[ti]); double dt = mem_now() - t0; if (dt < best) best = dt; }
            printf("   (a) stripe-local planes      T=%3d  %.3f s  %.2f Gcoef/s\n", Ts[ti], best, n / best / 1e9);
            if (Ts[ti] == 192) harness_result("crt_2e30_stripe_local_T192", "s", best);
        }
        /* (b) one plane per node */
        for (int i = 0; i < 4; i++) {
            free(res[i]);
            res[i] = (uint64_t *)malloc(n * 8);
#pragma omp parallel num_threads(48)
            { mem_pin_to_node(i); rng_t r = {(uint64_t)(0xDEF + omp_get_thread_num())};
#pragma omp for schedule(static)
              for (size_t k = 0; k < n; k++) res[i][k] = rng_next(&r) % ec_P[i];
              mem_unpin(); }
        }
        for (int ti = 0; ti < 3; ti++) {
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { double t0 = mem_now(); crt_carry_par4(res, n, out, Ts[ti]); double dt = mem_now() - t0; if (dt < best) best = dt; }
            printf("   (b) one plane per node       T=%3d  %.3f s  %.2f Gcoef/s\n", Ts[ti], best, n / best / 1e9);
            if (Ts[ti] == 192) harness_result("crt_2e30_plane_per_node_T192", "s", best);
        }
        /* (c) quartered, quarter q on node q */
        for (int i = 0; i < 4; i++) {
            q[i] = (uint64_t *)malloc(n * 8);
#pragma omp parallel num_threads(48)
            { mem_pin_to_node(i);
#pragma omp for schedule(static)
              for (size_t k = 0; k < n; k++) q[i][k] = 0;
              mem_unpin(); }
        }
        to_quartered(res, n, q, Q);
        for (int ti = 0; ti < 3; ti++) {
            double best = 1e9;
            for (int rep = 0; rep < 3; rep++) { double t0 = mem_now(); crt_carry_par4_q(q, Q, n, out, Ts[ti]); double dt = mem_now() - t0; if (dt < best) best = dt; }
            printf("   (c) quartered node-local     T=%3d  %.3f s  %.2f Gcoef/s\n", Ts[ti], best, n / best / 1e9);
            if (Ts[ti] == 192) harness_result("crt_2e30_quartered_T192", "s", best);
        }
        /* (d) registered hugepage staging, one plane per node, as rns_mul's hstage */
        for (int i = 0; i < 4; i++) {
            uint64_t *h = (uint64_t *)mem_hstage_alloc(i, n * 8, 0, 0);
            memcpy(h, res[i], n * 8);
            free(res[i]); res[i] = h;
        }
        for (int rep = 0; rep < 5; rep++) { double t0 = mem_now(); crt_carry_par4(res, n, out, 192); double dt = mem_now() - t0;
            printf("   (d) registered hugepage, plane per node  T=192  run %d  %.3f s\n", rep, dt); }
        /* (e) the same, right after a nested 4 x 48 parallel region as in rns_mul_mdev */
        omp_set_max_active_levels(2);
        for (int rep = 0; rep < 3; rep++) {
#pragma omp parallel num_threads(4)
            { int d = omp_get_thread_num();
#pragma omp parallel num_threads(48)
              { mem_pin_to_node(d); volatile double x = 0; for (int i = 0; i < 1000000; i++) x += i; mem_unpin(); } }
            double t0 = mem_now(); crt_carry_par4(res, n, out, 192); double dt = mem_now() - t0;
            printf("   (e) after nested 4x48 region             T=192  run %d  %.3f s\n", rep, dt);
        }
        /* (c) result must equal (b) */
        { uint64_t *o2 = (uint64_t *)malloc((n + 4) * 8); crt_carry_par4(res, n, o2, 192);
          VERIFY(memcmp(o2, out, (n + 4) * 8) == 0, "quartered result differs from plain at 2^%d", LOG); free(o2); }
    }
    return verify_done("t_crt");
}
