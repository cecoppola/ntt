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
    mpz_set_ui(M, 1);
    for (int i = 0; i < 4; i++) mpz_mul_ui(M, M, ec_P[i]);
    for (int i = 0; i < 4; i++) {
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
        for (int i = 0; i < 4; i++) { mpz_mul_ui(t, E[i], res[i][k]); mpz_add(c, c, t); }
        mpz_mod(c, c, M);
        mpz_mul_2exp(v, v, 64); mpz_add(v, v, c);
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
    mpz_import(w, n + 4, -1, 8, 0, 0, out);
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
    {
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
