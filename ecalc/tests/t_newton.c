/* t_newton - step 5 test: newton.h against mpz_tdiv_qr.
 *
 *  1. bi_divmod_school vs mpz_tdiv_qr, random sizes to 64 limbs, all generators
 *  2. newton_recip: mu vs floor(2^(64(nq+k))/Q) by GMP, error in units, for
 *     nq = 3 .. 2^20 and k = 1, nq/2, nq, 2 nq
 *  3. newton_divmod vs mpz_tdiv_qr at 2^10 .. 2^LOGMAX limbs (default 26):
 *     random; Q = 2^k and 2^k - 1; A = Q^2 - 1, Q^2, Q^2 + 1 (near multiples);
 *     A < Q; corrections counted; 0 <= R < Q; a perturbed seed forces the
 *     overshoot path; a supplied longer mu
 *  4. time at 2^LOGMAX limbs (A = 2 nq limbs) with the split tiers
 *
 * Usage: t_newton [LOGMAX (26)]
 */
#include "harness.h"
#include "../newton.h"
#include "../rns_mul.h"
#include "../mem.h"

static void mpz_recip(mpz_t mu, const mpz_t Q, size_t nq, size_t k)
{
    mpz_set_ui(mu, 1); mpz_mul_2exp(mu, mu, 64 * (nq + k)); mpz_fdiv_q(mu, mu, Q);
}
static int check_div(const char *what, const bigint *A, const bigint *Q, const bigint *mu_opt)
{
    bigint X, R; bi_init(&X); bi_init(&R);
    mpz_t a, q, x, r, xx, rr; mpz_inits(a, q, x, r, xx, rr, NULL);
    newton_stats before = newton_st;
    newton_divmod(&X, &R, A, Q, mu_opt);
    bi_to_mpz(a, A); bi_to_mpz(q, Q); mpz_tdiv_qr(x, r, a, q);
    bi_to_mpz(xx, &X); bi_to_mpz(rr, &R);
    int ok = mpz_cmp(x, xx) == 0 && mpz_cmp(r, rr) == 0;
    VERIFY(ok, "%s: quotient/remainder differ (na %zu nq %zu)", what, A->n, Q->n);
    VERIFY(bi_cmp(&R, Q) < 0, "%s: R >= Q", what);
    size_t dc = newton_st.down_corr - before.down_corr, uc = newton_st.up_corr - before.up_corr;
    VERIFY(dc + uc <= 4, "%s: %zu down + %zu up corrections", what, dc, uc);
    mpz_clears(a, q, x, r, xx, rr, NULL); bi_free(&X); bi_free(&R);
    return ok;
}

int main(int argc, char **argv)
{
    int LOGMAX = argc > 1 ? atoi(argv[1]) : 26;
    rng_t rng = {0x4E37ULL};
    bigint A, Q, X, R, mu, t; bi_init(&A); bi_init(&Q); bi_init(&X); bi_init(&R); bi_init(&mu); bi_init(&t);
    mpz_t a, q, x, r, m; mpz_inits(a, q, x, r, m, NULL);
    printf("== t_newton ==\n");
    harness_meta("t_newton");
    rns_init(31);

    printf("-- 1. schoolbook division\n");
    for (int it = 0; it < 400; it++) {
        size_t nq = 1 + rng_next(&rng) % 64, na = nq + rng_next(&rng) % 64;
        int ka = it % GEN_KINDS, kq = (it / GEN_KINDS) % GEN_KINDS;
        bi_random(&A, na, ka, &rng); bi_random(&Q, nq, kq, &rng);
        if (!Q.n) continue;
        bi_divmod_school(&X, &R, &A, &Q);
        bi_to_mpz(a, &A); bi_to_mpz(q, &Q); mpz_tdiv_qr(x, r, a, q);
        VERIFY(bi_eq_mpz(&X, x) && bi_eq_mpz(&R, r), "school %zu/%zu %s/%s", na, nq, gen_name[ka], gen_name[kq]);
    }

    printf("-- 2. reciprocal error\n");
    for (int lg = 2; lg <= 20; lg += 3) {
        size_t nq = ((size_t)1 << lg) + 1;
        static const int kinds[] = {GEN_UNIFORM, GEN_ONES, GEN_ZEROS, GEN_BIT};
        for (int ki = 0; ki < 4; ki++) {
            bi_random(&Q, nq, kinds[ki], &rng);
            size_t ks[4] = {1, nq / 2 + 1, nq, 2 * nq};
            for (int i = 0; i < 4; i++) {
                size_t k = ks[i];
                newton_recip(&mu, &Q, k);
                bi_to_mpz(q, &Q); mpz_recip(m, q, nq, k);
                bi_to_mpz(x, &mu); mpz_sub(x, m, x);
                long err = mpz_fits_slong_p(x) ? mpz_get_si(x) : 1L << 40;
                VERIFY(err >= -8 && err <= 8, "recip nq %zu k %zu %s: error %ld units (mu %zu limbs)", nq, k, gen_name[kinds[ki]], err, mu.n);
                if (ki == 0 && i == 2) printf("   nq %-8zu k %-8zu error %+ld  (%zu iterations)\n", nq, k, err, newton_st.iters);
            }
        }
    }

    printf("-- 3. divmod vs mpz_tdiv_qr\n");
    for (int lg = 10; lg <= LOGMAX; lg += 4) {
        size_t nq = (size_t)1 << lg;
        bi_random(&Q, nq, GEN_UNIFORM, &rng);
        bi_random(&A, 2 * nq, GEN_UNIFORM, &rng);       check_div("random", &A, &Q, 0);
        bi_random(&A, 2 * nq + 3, GEN_ONES, &rng);      check_div("A all-ones", &A, &Q, 0);
        bi_random(&A, nq + 1, GEN_UNIFORM, &rng);       check_div("na = nq + 1", &A, &Q, 0);
        bi_random(&A, nq - 1, GEN_UNIFORM, &rng);       check_div("A < Q", &A, &Q, 0);
        /* Q = 2^k, 2^k - 1 */
        bi_random(&Q, nq, GEN_BIT, &rng); for (size_t i = 0; i + 1 < nq; i++) Q.l[i] = 0;
        bi_random(&A, 2 * nq, GEN_UNIFORM, &rng);       check_div("Q = 2^k", &A, &Q, 0);
        bi_random(&Q, nq, GEN_ONES, &rng);              check_div("Q = 2^k - 1", &A, &Q, 0);
        /* near multiples: Q^2 - 1, Q^2, Q^2 + 1 */
        bi_random(&Q, nq, GEN_UNIFORM, &rng);
        rns_mul(&t, &Q, &Q);
        bi_copy(&A, &t); check_div("A = Q^2", &A, &Q, 0);
        bi_add_u64(&A, 1); check_div("A = Q^2 + 1", &A, &Q, 0);
        bi_set_u64(&X, 1); bi_sub(&A, &t, &X); check_div("A = Q^2 - 1", &A, &Q, 0);
        /* overshoot path via a perturbed seed (seed x 40/16 > 2x: T2 < T1); not at
         * schoolbook sizes (3 nq < 4 rns_school_max) */
        if (3 * nq >= 4 * (size_t)rns_school_max) {
        newton_seed_perturb = 40;
        bi_random(&A, 2 * nq, GEN_UNIFORM, &rng);
        { size_t o = newton_st.overshoots; check_div("perturbed seed", &A, &Q, 0);
          VERIFY(newton_st.overshoots > o, "perturbed seed did not take the overshoot path"); }
        newton_seed_perturb = 12;                       /* 25 % low: the repeat path */
        { size_t o = newton_st.repeats; check_div("low seed", &A, &Q, 0);
          VERIFY(newton_st.repeats > o, "low seed did not repeat a step"); }
        newton_seed_perturb = 0;
        }
        /* a supplied longer mu */
        newton_recip(&mu, &Q, nq + 8);
        check_div("supplied mu", &A, &Q, &mu);
        printf("   2^%d limbs ok  (down %zu, up %zu corrections, %zu overshoots, %zu repeats so far)\n", lg, newton_st.down_corr, newton_st.up_corr, newton_st.overshoots, newton_st.repeats);
    }

    printf("-- 4. time at 2^%d limbs\n", LOGMAX);
    {
        size_t nq = (size_t)1 << LOGMAX;
        bi_random(&Q, nq, GEN_UNIFORM, &rng); bi_random(&A, 2 * nq, GEN_UNIFORM, &rng);
        memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
        double t0 = mem_now();
        newton_divmod(&X, &R, &A, &Q, 0);
        double dt = mem_now() - t0;
        uint64_t ra = limbs_mod_m61(A.l, A.n), rq = limbs_mod_m61(Q.l, Q.n), rx = limbs_mod_m61(X.l, X.n), rr = limbs_mod_m61(R.l, R.n);
        VERIFY(m61_add(m61_mul(rx, rq), rr) == ra, "2^%d: A != XQ + R mod 2^61-1", LOGMAX);
        VERIFY(bi_cmp(&R, &Q) < 0, "2^%d: R >= Q", LOGMAX);
        printf("   divmod %zu / %zu limbs: %.2f s (recip %.2f, %zu iters; %zu mdev, %zu splits; corrections %zu/%zu)\n",
               A.n, Q.n, dt, newton_st.t_recip, newton_st.iters, rns_st.n_mdev, rns_st.n_split, newton_st.down_corr, newton_st.up_corr);
        harness_result("divmod_time", "s", dt);
        harness_result("recip_time", "s", newton_st.t_recip);
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_newton");
}
