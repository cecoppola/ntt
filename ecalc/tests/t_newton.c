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
static int use_db = -1;
#define newton_recip(m, q, k) ((use_db < 0 ? (use_db = getenv("NEWTON_DEVICE") ? atoi(getenv("NEWTON_DEVICE")) : 0) : 0), use_db ? newton_db_recip(m, q, k) : newton_recip(m, q, k))
#define newton_divmod(x, r, a, q, mu) ((use_db < 0 ? (use_db = getenv("NEWTON_DEVICE") ? atoi(getenv("NEWTON_DEVICE")) : 0) : 0), use_db ? newton_db_divmod(x, r, a, q, mu) : newton_divmod(x, r, a, q, mu))
#include "../rns_mul.h"
#include "../mem.h"
#include "../dbig.h"

static void mpz_recip(mpz_t mu, const mpz_t Q, size_t nq, size_t k)
{
    if (bi_decimal) mpz_ui_pow_ui(mu, BI_B10, nq + k); else { mpz_set_ui(mu, 1); mpz_mul_2exp(mu, mu, 64 * (nq + k)); }
    mpz_fdiv_q(mu, mu, Q);
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

    printf("-- 3. divmod vs mpz_tdiv_qr%s\n", bi_decimal ? " (decimal: exact to 2^22, the GMP string bridge is slow above)" : "");
    for (int lg = 10; lg <= (bi_decimal && LOGMAX > 22 ? 22 : LOGMAX); lg += 4) {
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

    if (use_db > 0) {
        /* Phase 15 R4 (NEWTON_RECIP_MID, results/R415.md): (a) the band product rns_mul_band_db against the whole product -- mod B^w
         * identical with no low cut, the band [v, w) within one unit (mod B^(w - v)) with the cut at v - 1; (b) the reciprocal with the
         * middle product bit-identical to the one without, under NEWTON_RECIP_CUT 0 and 1, at several sizes, k and generators (a grid at
         * the top doublings under DIST_LOGN_TEST=20..22), and the path taken */
        printf("-- 5. the reciprocal's middle product (NEWTON_RECIP_MID)\n");
        int cut_env = getenv("NEWTON_RECIP_CUT") ? atoi(getenv("NEWTON_RECIP_CUT")) != 0 : 1, mid_env = getenv("NEWTON_RECIP_MID") ? atoi(getenv("NEWTON_RECIP_MID")) != 0 : 0;
        bigint B1, C0, C1; bi_init(&B1); bi_init(&C0); bi_init(&C1);
        static const size_t band_sz[][2] = { {3000, 1000}, {(1u << 20) + 5, (1u << 19) + 3}, {(3u << 20) + 1, (3u << 19) - 5}, {(1u << 22) + 3, (1u << 21) + 1} };   /* the last two: grids split on both operands at cap 2^20..2^22, so the high cut skips pieces */
        for (int si = 0; si < 4; si++) for (int ki = 0; ki < 2; ki++) {
            size_t na = band_sz[si][0], nb = band_sz[si][1], v = na - nb + 1, w = v + nb + 3;   /* the reciprocal's shape: take x (j + 1), v = take - j, w = take + 4 */
            bi_random(&A, na, ki ? GEN_ONES : GEN_UNIFORM, &rng); bi_random(&B1, nb, ki ? GEN_ONES : GEN_UNIFORM, &rng);
            dbig a, b, c; db_init(&a); db_init(&b); db_init(&c); db_from_bi(&a, &A); db_from_bi(&b, &B1);
            rns_mul_dist_db(&c, &a, &b); db_to_bi(&C0, &c);
            size_t f0 = rns_dist_st.n_formed, s0 = rns_dist_st.n_skipped;
            rns_mul_band_db(&c, &a, &b, 0, w); db_to_bi(&C1, &c);
            size_t sk = rns_dist_st.n_skipped - s0, fo = rns_dist_st.n_formed - f0;
            mpz_t z0, z1, bw, bv; mpz_inits(z0, z1, bw, bv, NULL); bi_to_mpz(z0, &C0); bi_to_mpz(z1, &C1);
            if (bi_decimal) { mpz_ui_pow_ui(bw, BI_B10, w); mpz_ui_pow_ui(bv, BI_B10, v); } else { mpz_set_ui(bw, 1); mpz_mul_2exp(bw, bw, 64 * w); mpz_set_ui(bv, 1); mpz_mul_2exp(bv, bv, 64 * v); }
            mpz_mod(z0, z0, bw);
            VERIFY(mpz_cmp(z0, z1) == 0 && C1.n <= w, "band %zu x %zu w %zu: mod B^w differs", na, nb, w);
            rns_mul_band_db(&c, &a, &b, v - 1, w); db_to_bi(&C1, &c); bi_to_mpz(z1, &C1);   /* with the low cut one limb below the band */
            mpz_t bb; mpz_init(bb); mpz_tdiv_q(bb, bw, bv);                               /* B^(w - v) */
            mpz_fdiv_q(z0, z0, bv); mpz_fdiv_q(z1, z1, bv); mpz_sub(z0, z0, z1); mpz_mod(z0, z0, bb);
            VERIFY(mpz_cmp_ui(z0, 1) <= 0, "band %zu x %zu w %zu, low cut %zu: the band is off by more than one unit", na, nb, w, v - 1);
            printf("   band %zu x %zu (w %zu): %zu pieces formed, %zu skipped; mod B^w identical, cut band off by %lu\n", na, nb, w, fo, sk, mpz_get_ui(z0));
            mpz_clears(z0, z1, bw, bv, bb, NULL); db_free(&a); db_free(&b); db_free(&c);
        }
        static const size_t rq[] = { 4097, (1u << 17) + 3, (1u << 20) + 1, (3u << 20) + 5, (1u << 22) + 1 };   /* the last two: the top doublings are 2-D grids at cap 2^20 (the high cut skips) */
        for (int qi = 0; qi < 5; qi++) {
            size_t nq = rq[qi];
            static const int kinds[] = {GEN_UNIFORM, GEN_ONES, GEN_BIT};
            for (int gi = 0; gi < 3; gi++) {
                bi_random(&Q, nq, kinds[gi], &rng);
                size_t ks[3] = {nq / 2 + 1, nq, 2 * nq};
                for (int i = 0; i < 3; i++) for (int cut = 0; cut <= 1; cut++) {
                    size_t k = ks[i];
                    if (nq > (1u << 21) && (gi > 0 || i != 1)) continue;   /* the largest size: uniform, k = nq only (time) */
                    size_t sa = rns_dist_st.n_skipped;
                    newton_recip_set(cut, 0); newton_db_recip(&mu, &Q, k);
                    struct newton_mid_stats m0 = newton_mid_st; size_t s0 = rns_dist_st.n_skipped, sk0 = s0 - sa;
                    newton_recip_set(cut, 1); newton_db_recip(&t, &Q, k);
                    size_t nm = newton_mid_st.mid - m0.mid, sk = rns_dist_st.n_skipped - s0;
                    if (nq > (1u << 21)) VERIFY(sk > sk0, "recip mid nq %zu k %zu cut %d: the high cut skipped nothing (%zu vs %zu)", nq, k, cut, sk, sk0);
                    VERIFY(bi_cmp(&mu, &t) == 0, "recip mid nq %zu k %zu %s cut %d: mu differs from the whole product's", nq, k, gen_name[kinds[gi]], cut);
                    VERIFY(nm > 0, "recip mid nq %zu k %zu: the middle product was never taken", nq, k);
                    bi_to_mpz(q, &Q); mpz_recip(m, q, nq, k); bi_to_mpz(x, &t); mpz_sub(x, m, x);
                    long err = mpz_fits_slong_p(x) ? mpz_get_si(x) : 1L << 40;
                    VERIFY(err >= -8 && err <= 8, "recip mid nq %zu k %zu %s cut %d: error %ld units", nq, k, gen_name[kinds[gi]], cut, err);
                    if (gi == 0) printf("   nq %-8zu k %-8zu cut %d: identical, error %+ld, %zu rounds on the middle product, pieces skipped %zu (switch off) -> %zu (on)\n", nq, k, cut, err, nm, sk0, sk);
                }
            }
        }
        newton_recip_set(cut_env, mid_env);
        bi_free(&B1); bi_free(&C0); bi_free(&C1);
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
