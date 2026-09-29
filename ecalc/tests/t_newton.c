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
 *  5. (NEWTON_DEVICE=1) the reciprocal's middle product; 6. the division in two quotient halves (NEWTON_DKM, Phase 15 DKM)
 *
 * Usage: t_newton [LOGMAX (26)]
 */
#include "harness.h"
#include "../newton.h"
static int use_db = -1;
/* Phase 15 EW (section 7): the X_hi / X_lo hooks of the DKM division -- X_hi copied out and freed (taken), the X_lo hook's carry recorded */
#include "../dbig.h"
static struct { bigint Xh; size_t s; int hi, lo, carry, release; } h7;
static int h7_hi_hook(dbig *Xh, size_t s, void *a) { (void)a; db_to_bi(&h7.Xh, Xh); db_free(Xh); h7.s = s; h7.hi++; return 1; }
static int h7_lo_hook(dbig *Xl, size_t s, int carry, void *a) { (void)a; (void)Xl; (void)s; h7.lo++; h7.carry = carry; return h7.release && !carry; }
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

        /* Phase 15 DKM (NEWTON_DKM, results/DKM15.md 1.6): newton_db_divmod_shifted (A = S B^dl, Q on the device, the prewarm reciprocal
         * as ecalc's device flow takes it) with the switch off and on against GMP -- X exactly, R's residues mod three primes; the shapes:
         * ecalc's (S ~ Q, k ~ dl), k odd / even, dl < k/2 (s = dl: the kept mu too short, a fresh one), A a multiple of Q (R1 = 0, X_lo = 0),
         * Q all-ones / a power; forced corrections: ECALC_TEST_CORR +-7 (the final count moves by k) and NEWTON_DKM_TEST_HI +-7 (step 1's) */
        printf("-- 6. the division in two quotient halves (NEWTON_DKM)\n");
        {
            static const uint64_t qs6[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
            bigint S6, A6, X6, Y6; bi_init(&S6); bi_init(&A6); bi_init(&X6); bi_init(&Y6);
            mpz_t zr; mpz_init(zr);
            static const size_t nq6[] = { 1500, (1u << 14) + 3, (1u << 18) + 5, (1u << 20) + 1 };
            int dkm_env = newton_dkm_on();
            for (int qi = 0; qi < 4; qi++) for (int shape = 0; shape < 7; shape++) {
                size_t nq = nq6[qi], dl, sn;
                int qk = shape == 4 ? GEN_ONES : shape == 5 ? GEN_BIT : GEN_UNIFORM;
                bi_random(&Q, nq, qk, &rng); if (qk == GEN_BIT) for (size_t i = 0; i + 1 < nq; i++) Q.l[i] = 0;
                if (Q.n != nq) continue;
                switch (shape) {
                case 0: dl = nq - 3; sn = nq; break;               /* ecalc's shape: k = dl + 1 (even / odd by nq) */
                case 1: dl = nq - 4; sn = nq + 1; break;           /* k = dl + 2 */
                case 2: dl = nq / 4; sn = 2 * nq; break;           /* dl < k/2: s = dl, a fresh reciprocal */
                default: dl = nq - 3 - (shape & 1); sn = nq; break;
                }
                if (shape == 3) { bi_random(&Y6, 2, GEN_UNIFORM, &rng); rns_mul(&S6, &Q, &Y6); }   /* A = Q Y B^dl: R1 = 0, X_lo = 0 */
                else bi_random(&S6, sn, GEN_UNIFORM, &rng);
                bi_shl_limbs(&A6, &S6, dl);
                bi_to_mpz(a, &A6); bi_to_mpz(q, &Q); mpz_tdiv_qr(x, zr, a, q);
                uint64_t want[3]; for (int j = 0; j < 3; j++) want[j] = mpz_fdiv_ui(zr, qs6[j]);
                long nat = 0;                                         /* the unforced final signed correction count (switch on) */
                static const long forced[][2] = { {0, 0}, {0, 0}, {7, 0}, {-7, 0}, {0, 7}, {0, -7} };   /* {ECALC_TEST_CORR, NEWTON_DKM_TEST_HI}; row 0: off */
                for (int f = 0; f < 6; f++) {
                    int on = f > 0;
                    if (qi == 3 && f > 2) continue;                  /* the largest size: off, on, one forced (time) */
                    long tk = forced[f][0], th = forced[f][1];
                    if (shape == 3 && tk > 0) continue;              /* X_lo = 0 there: X - k would need a borrow from X_hi (the hook refuses) */
                    char b1[16], b2[16]; snprintf(b1, sizeof b1, "%ld", tk); snprintf(b2, sizeof b2, "%ld", th);
                    setenv("ECALC_TEST_CORR", b1, 1); setenv("NEWTON_DKM_TEST_HI", b2, 1);
                    newton_dkm_set(on);
                    dbig Sd, Qd6; db_init(&Sd); db_init(&Qd6); db_from_bi(&Sd, &S6); db_from_bi(&Qd6, &Q);
                    size_t k_mu = Sd.n + 1 + dl - nq + 1;             /* ecalc.c's prewarm k (S has at most one limb more than P) */
                    newton_db_Qd = &Qd6; newton_db_mu_host = 0;
                    newton_db_recip(&mu, &Q, k_mu);
                    newton_stats b = newton_st;
                    uint64_t got[3];
                    newton_db_divmod_shifted(&X6, &Sd, dl, &Qd6, qs6, 3, got);
                    newton_db_Qd = 0; newton_db_mu_host = 1;
                    long sg = (long)(newton_st.up_corr - b.up_corr) - (long)(newton_st.down_corr - b.down_corr);
                    size_t hc = newton_st.dkm_corr - b.dkm_corr;
                    int okx = bi_eq_mpz(&X6, x), okr = got[0] == want[0] && got[1] == want[1] && got[2] == want[2];
                    VERIFY(okx && okr, "dkm nq %zu shape %d %s (tk %ld th %ld): X %s, R residues %s", nq, shape, on ? "on" : "off", tk, th, okx ? "ok" : "DIFFERS", okr ? "ok" : "DIFFER");
                    if (f == 1) nat = sg;
                    if (f >= 2 && tk) VERIFY(sg == nat + tk, "dkm nq %zu shape %d: ECALC_TEST_CORR=%ld moved the final count %ld -> %ld", nq, shape, tk, nat, sg);
                    if (f >= 2 && th) VERIFY(sg == nat && hc + 3 >= (size_t)(th < 0 ? -th : th), "dkm nq %zu shape %d: NEWTON_DKM_TEST_HI=%ld: final %ld (unforced %ld), step 1 %zu", nq, shape, th, sg, nat, hc);
                    VERIFY(sg >= -64 && sg <= 64, "dkm: count");
                    if (shape < 3 || f < 2) printf("   nq %-8zu shape %d %-3s tk %+3ld th %+3ld: X %zu limbs identical, R ok; final corrections %+ld, step 1 %zu\n", nq, shape, on ? "on" : "off", tk, th, X6.n, sg, hc);
                    db_free(&Sd); db_free(&Qd6); newton_db_free_scratch();
                }
            }
            unsetenv("ECALC_TEST_CORR"); unsetenv("NEWTON_DKM_TEST_HI"); newton_dkm_set(dkm_env);
            mpz_clear(zr); bi_free(&S6); bi_free(&A6); bi_free(&X6); bi_free(&Y6);
        }

        /* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.2-1.3): the DKM division with the X_hi / X_lo hooks (no X0): the X_hi hook gets X's limbs
         * >= s exactly (whatever step 2 and the forced corrections do), the division returns X_lo; released (the hook's 1): X_lo0 with the
         * corrections deferred (X_lo0 + newton_x_dx = X mod B^s); not released: X mod B^s itself, newton_x_dx = 0.  Operands: A = X Q + R
         * built from a chosen X (R = -X Q mod B^dl < Q: A a multiple of B^dl), X's low half random or B^s - 3 -- with ECALC_TEST_CORR=-7 the
         * estimate X_lo0 >= B^s (the carry case: the hook refuses, the corrections go in place); ECALC_TEST_CORR +-7, NEWTON_DKM_TEST_HI 7 */
        printf("-- 7. the X_hi / X_lo hooks of the DKM division (MN_OUT_DKM_HI), X_lo0 >= B^s constructed\n");
        {
            static const uint64_t qs7[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
            bigint S7, X7, H7, L7; bi_init(&S7); bi_init(&X7); bi_init(&H7); bi_init(&L7);
            mpz_t xt, bp, bd, xq, r7, hq, lq; mpz_inits(xt, bp, bd, xq, r7, hq, lq, NULL);
            int dkm_env = newton_dkm_on(), defer_env = newton_x_defer; newton_dkm_set(1); newton_x_defer = 1;
            static const size_t nq7[] = { 1500, (1u << 14) + 3, (1u << 18) + 5 };
            int ncarry = 0;
            for (int qi = 0; qi < 3; qi++) for (int low = 0; low < 2; low++) for (int f = 0; f < 5; f++) {
                static const long forced7[5][3] = { {0, 0, 1}, {7, 0, 1}, {-20, 0, 1}, {0, 7, 1}, {-20, 0, 0} };   /* {ECALC_TEST_CORR, NEWTON_DKM_TEST_HI, release} */
                long tk = forced7[f][0], th = forced7[f][1];
                size_t nq = nq7[qi], dl = nq - 3, xn = nq - 3, s = 0;
                bi_random(&Q, nq, GEN_UNIFORM, &rng); if (Q.n != nq) continue;
                bi_to_mpz(q, &Q);
                bi_random(&X7, xn, GEN_UNIFORM, &rng); if (X7.n != xn) continue;
                if (bi_decimal) mpz_ui_pow_ui(bd, 10, 18 * dl); else { mpz_set_ui(bd, 0); mpz_setbit(bd, 64 * dl); }
                for (int pass = 0; pass < 3; pass++) {        /* s from the operands' lengths; X's low half set, then s re-derived (stable) */
                    bi_to_mpz(xt, &X7);
                    if (low && s) { if (bi_decimal) mpz_ui_pow_ui(bp, 10, 18 * s); else { mpz_set_ui(bp, 0); mpz_setbit(bp, 64 * s); }
                                    mpz_tdiv_q(xt, xt, bp); mpz_mul(xt, xt, bp); mpz_add(xt, xt, bp); mpz_sub_ui(xt, xt, 3); }   /* X mod B^s = B^s - 3 */
                    mpz_mul(xq, xt, q); mpz_neg(r7, xq); mpz_fdiv_r(r7, r7, bd); mpz_add(a, xq, r7); mpz_tdiv_q(a, a, bd);   /* S = (X Q + R) / B^dl */
                    bi_from_mpz(&S7, a);
                    size_t k = S7.n + dl - nq + 1, s2 = k / 2 < dl ? k / 2 : dl;
                    if (s2 == s) break;
                    s = s2;
                }
                char b1[16], b2[16]; snprintf(b1, sizeof b1, "%ld", tk); snprintf(b2, sizeof b2, "%ld", th);
                setenv("ECALC_TEST_CORR", b1, 1); setenv("NEWTON_DKM_TEST_HI", b2, 1);
                h7.hi = h7.lo = h7.carry = 0; h7.release = (int)forced7[f][2];
                dbig Sd, Qd7, Xd7; db_init(&Sd); db_init(&Qd7); db_init(&Xd7); db_from_bi(&Sd, &S7); db_from_bi(&Qd7, &Q);
                newton_db_Qd = &Qd7; newton_db_mu_host = 0;
                newton_db_recip(&mu, &Q, Sd.n + 1 + dl - nq + 1);
                newton_db_xhi_hook = h7_hi_hook; newton_db_xlo_hook = h7_lo_hook; newton_db_x_dev = &Xd7;
                uint64_t got[3], want[3]; for (int j = 0; j < 3; j++) want[j] = mpz_fdiv_ui(r7, qs7[j]);
                newton_db_divmod_shifted(&H7, &Sd, dl, &Qd7, qs7, 3, got);   /* (H7: the host X, unused -- X stays on the device) */
                newton_db_xhi_hook = 0; newton_db_xlo_hook = 0; newton_db_x_dev = 0; newton_db_Qd = 0; newton_db_mu_host = 1;
                db_to_bi(&L7, &Xd7); db_free(&Xd7);
                if (bi_decimal) mpz_ui_pow_ui(bp, 10, 18 * h7.s); else { mpz_set_ui(bp, 0); mpz_setbit(bp, 64 * h7.s); }
                mpz_tdiv_qr(hq, lq, xt, bp);                     /* X's limbs >= s and < s */
                bi_to_mpz(x, &h7.Xh); int okh = mpz_cmp(x, hq) == 0;
                bi_to_mpz(x, &L7); if (h7.release && !h7.carry) { if (newton_x_dx >= 0) mpz_add_ui(x, x, (unsigned long)newton_x_dx); else mpz_sub_ui(x, x, (unsigned long)(-newton_x_dx)); }
                int okl = mpz_cmp(x, lq) == 0, okdx = (h7.release && !h7.carry) || newton_x_dx == 0, okr = got[0] == want[0] && got[1] == want[1] && got[2] == want[2];
                int wantc = low && tk < 0;                         /* B^s - 3 + 20 - the estimate's shortfall (a few units) >= B^s */
                ncarry += h7.carry;
                VERIFY(h7.hi == 1 && h7.lo == 1 && h7.s == s && okh && okl && okdx && okr && (!wantc || h7.carry),
                       "dkm hooks nq %zu low %d tk %ld th %ld release %d: hooks %d/%d, s %zu (%zu), X_hi %s, X_lo %s (dx %ld), R %s, carry %d (expected %d)",
                       nq, low, tk, th, h7.release, h7.hi, h7.lo, h7.s, s, okh ? "ok" : "DIFFERS", okl ? "ok" : "DIFFERS", newton_x_dx, okr ? "ok" : "DIFFER", h7.carry, wantc);
                printf("   nq %-8zu X mod B^s %-9s tk %+3ld th %+3ld release %d: s %zu, X_hi %zu limbs exact, X_lo0 %s B^s -> %s, dx %+ld; R ok\n", nq, low ? "B^s - 3" : "random", tk, th, h7.release,
                       h7.s, h7.Xh.n, h7.carry ? ">=" : "<", h7.release && !h7.carry ? "deferred" : "corrected in place", newton_x_dx);
                db_free(&Sd); db_free(&Qd7); newton_db_free_scratch(); newton_x_dx = 0;
            }
            VERIFY(ncarry >= 3, "dkm hooks: the carry case X_lo0 >= B^s arose %d times (3 constructed)", ncarry);
            unsetenv("ECALC_TEST_CORR"); unsetenv("NEWTON_DKM_TEST_HI"); newton_dkm_set(dkm_env); newton_x_defer = defer_env;
            mpz_clears(xt, bp, bd, xq, r7, hq, lq, NULL); bi_free(&S7); bi_free(&X7); bi_free(&H7); bi_free(&L7); bi_free(&h7.Xh);
        }
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
