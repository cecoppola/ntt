/* t_newton - step 5 test: newton.h against mpz_tdiv_qr.
 *
 *  1. bi_divmod_school vs mpz_tdiv_qr, random sizes to 64 limbs, all generators
 *  2. newton_db_recip: mu vs floor(2^(64(nq+k))/Q) by GMP, error in units, for
 *     nq = 3 .. 2^20 and k = 1, nq/2, nq, 2 nq
 *  3. newton_db_divmod_shifted (A = S B^dl, the device flow's one division) vs mpz_tdiv_qr at 2^10 .. 2^LOGMAX limbs
 *     (default 26; exact to 2^22, above that A == X Q + R is checked by residues mod three primes): random; Q = B^k-ish
 *     (single top limb) and B^k - 1; S = Q Y - 1, Q Y, Q Y + 1 (near multiples); a quotient of one limb; corrections
 *     counted; a perturbed seed forces the overshoot path; a longer prewarm mu.  3b: quotients of 1 .. 8 limbs
 *  4. time at 2^LOGMAX limbs (A = 2 nq limbs) with the split tiers
 *  5. the reciprocal's middle product; 6. the division in two quotient halves (DKM, the one division)
 *  8. the reciprocal's chain guard (NEWTON_CHAIN_GUARD, results/RECIP.md): exact doublings off and on, the DKM estimate
 *
 * Usage: t_newton [LOGMAX (26)]
 * Decimal limbs only since archive:PLAN §28.5.
 */
#include "harness.h"
#include "tref.h"
#include "../newton.h"
/* the X_hi / X_lo hooks of the DKM division -- X_hi copied out and freed (taken), the X_lo hook's carry recorded */
#include "../dbig.h"
static struct { bigint Xh; size_t s; int hi, lo, carry, release; } h7;
static int h7_hi_hook(dbig *Xh, size_t s, void *a) { (void)a; db_to_bi(&h7.Xh, Xh); db_free(Xh); h7.s = s; h7.hi++; return 1; }
static int h7_lo_hook(dbig *Xl, size_t s, int carry, void *a) { (void)a; (void)Xl; (void)s; h7.lo++; h7.carry = carry; return h7.release && !carry; }
#include "../rns_mul.h"
#include "../mem.h"
#include "../dbig.h"

static void mpz_recip(mpz_t mu, const mpz_t Q, size_t nq, size_t k)
{
    mpz_ui_pow_ui(mu, BI_B10, nq + k);
    mpz_fdiv_q(mu, mu, Q);
}
/* the reciprocal's error bound (results/RECIP.md), err = floor(B^(nq+k)/Q) - mu.  mu's units scale with M = B^nq / Q (1 .. B: decimal
 * limbs are not normalised).  A doubling j -> 2j with no limb cut after it keeps its own error: mu - B^(nq+k)/Q < 2 M + 3, so
 * err >= -(2 ceil(M) + 6) when the chain's LAST doubling is exact (ceil(M) <= B / top limb + 1); a doubling j -> jn < 2j cuts it by
 * >= 1 limb (+-8 as before; the upper side is the model's +2 plus margin).  Two exact doublings in a row square the error (no bound
 * without NEWTON_CHAIN_GUARD: returns 0).  With the guard every doubling cuts: +-8 */
static int recip_bound(const bigint *Q, size_t k, long *lo, long *hi)
{
    *lo = -8; *hi = 8;
    if (newton_chain_guard()) return 1;
    size_t j = 2; int prev = 0, last = 0;
    while (j < k) { size_t jn = newton_chain_next(j, k); int ex = jn == 2 * j; if (ex && prev) return 0; prev = last = ex; j = jn; }
    if (last) *lo = -(2 * (long)(BI_B10 / Q->l[Q->n - 1] + 1) + 6);
    return 1;
}
static long recip_err(mpz_t m, mpz_t x, mpz_t q, const bigint *Q, const bigint *mu, size_t k)   /* err as above (1 << 62 if it does not fit) */
{
    bi_to_mpz(q, Q); mpz_recip(m, q, Q->n, k); bi_to_mpz(x, mu); mpz_sub(x, m, x);
    return mpz_fits_slong_p(x) ? mpz_get_si(x) : mpz_sgn(x) < 0 ? -(1L << 62) : 1L << 62;
}
#define EXACT_MAX_NQ ((size_t)1 << 22)                  /* exact GMP comparison up to this Q length; residues above (archive:PLAN §28.5) */
static const uint64_t qs3[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
/* the division as ecalc runs it: A = S B^dl, S and Q on the device, the prewarm reciprocal (newton_db_recip with newton_db_Qd, kx limbs
 * more than ecalc asks for), newton_db_divmod_shifted; X exactly and R's residues mod three primes against GMP (above EXACT_MAX_NQ:
 * A == X Q + R by the residues, R's from the division, and X's length) */
static int check_div(const char *what, const bigint *S, size_t dl, const bigint *Q, size_t kx)
{
    bigint X, A, mu; bi_init(&X); bi_init(&A); bi_init(&mu);
    newton_stats before = newton_st;
    dbig Sd, Qd; db_init(&Sd); db_init(&Qd); db_from_bi(&Sd, S); db_from_bi(&Qd, Q);
    newton_db_Qd = &Qd; newton_db_mu_host = 0;
    newton_db_recip(&mu, Q, Sd.n + 1 + dl - Q->n + 1 + kx);
    uint64_t got[3];
    newton_db_divmod_shifted(&X, &Sd, dl, &Qd, qs3, 3, got);
    newton_db_Qd = 0; newton_db_mu_host = 1;
    db_free(&Sd); db_free(&Qd); newton_db_free_scratch();
    bi_shl_limbs(&A, S, dl);
    int ok = 1;
    if (Q->n <= EXACT_MAX_NQ) {
        mpz_t a, q, x, r; mpz_inits(a, q, x, r, NULL);
        bi_to_mpz(a, &A); bi_to_mpz(q, Q); mpz_tdiv_qr(x, r, a, q);
        for (int j = 0; j < 3; j++) if (mpz_fdiv_ui(r, qs3[j]) != got[j]) ok = 0;
        int okx = bi_eq_mpz(&X, x);
        VERIFY(okx && ok, "%s: X %s, R residues %s (na %zu nq %zu dl %zu)", what, okx ? "ok" : "DIFFERS", ok ? "ok" : "DIFFER", A.n, Q->n, dl);
        ok = ok && okx;
        mpz_clears(a, q, x, r, NULL);
    } else {
        for (int j = 0; j < 3; j++) {
            uint64_t q = qs3[j], a1 = limbs_mod_u64(A.l, A.n, q), q1 = limbs_mod_u64(Q->l, Q->n, q), x1 = limbs_mod_u64(X.l, X.n, q);
            if ((hv_mulmod(x1, q1, q) + got[j] % q) % q != a1) ok = 0;
        }
        VERIFY(ok, "%s: A != X Q + R by residues (na %zu nq %zu dl %zu)", what, A.n, Q->n, dl);
        VERIFY(X.n == A.n - Q->n + 1 || X.n == A.n - Q->n, "%s: X has %zu limbs (na %zu nq %zu)", what, X.n, A.n, Q->n);
    }
    size_t dc = newton_st.down_corr - before.down_corr, uc = newton_st.up_corr - before.up_corr;
    VERIFY(dc + uc <= 4, "%s: %zu down + %zu up corrections", what, dc, uc);
    bi_free(&X); bi_free(&A); bi_free(&mu);
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
                newton_db_recip(&mu, &Q, k);
                long err = recip_err(m, x, q, &Q, &mu, k), lo, hi;
                if (recip_bound(&Q, k, &lo, &hi)) VERIFY(err >= lo && err <= hi, "recip nq %zu k %zu %s: error %ld units, bound [%ld, %ld] (mu %zu limbs)", nq, k, gen_name[kinds[ki]], err, lo, hi, mu.n);
                if (ki == 0 && i == 2) printf("   nq %-8zu k %-8zu error %+ld  (%zu iterations)\n", nq, k, err, newton_st.iters);
            }
        }
    }

    printf("-- 3. the division (newton_db_divmod_shifted, A = S B^dl) vs mpz_tdiv_qr (exact to 2^22 limbs of Q; residues mod three primes above)\n");
    for (int lg = 10; lg <= LOGMAX; lg += 4) {
        size_t nq = (size_t)1 << lg, dl = nq / 2;
        bigint Y; bi_init(&Y);
        bi_random(&Q, nq, GEN_UNIFORM, &rng);
        bi_random(&A, 2 * nq - dl, GEN_UNIFORM, &rng);      check_div("random", &A, dl, &Q, 0);
        bi_random(&A, 2 * nq + 3 - dl, GEN_ONES, &rng);     check_div("S all-ones", &A, dl, &Q, 0);
        bi_random(&A, nq + 1 - dl, GEN_UNIFORM, &rng);      check_div("na = nq + 1", &A, dl, &Q, 0);
        bi_random(&A, nq - dl, GEN_UNIFORM, &rng);          if (A.n == nq - dl) check_div("na = nq (k = 1)", &A, dl, &Q, 0);
        /* Q = 2^k, 2^k - 1 */
        bi_random(&Q, nq, GEN_BIT, &rng); for (size_t i = 0; i + 1 < nq; i++) Q.l[i] = 0;
        bi_random(&A, 2 * nq - dl, GEN_UNIFORM, &rng);      check_div("Q = 2^k", &A, dl, &Q, 0);
        bi_random(&Q, nq, GEN_ONES, &rng);                  check_div("Q = 2^k - 1", &A, dl, &Q, 0);
        /* near multiples: S = Q Y - 1, Q Y, Q Y + 1 (A a multiple of Q, and one off) */
        bi_random(&Q, nq, GEN_UNIFORM, &rng); bi_random(&Y, nq - dl, GEN_UNIFORM, &rng);
        tref_mul(&t, &Q, &Y);
        bi_copy(&A, &t); check_div("S = Q Y", &A, dl, &Q, 0);
        bi_add_u64(&A, 1); check_div("S = Q Y + 1", &A, dl, &Q, 0);
        bi_set_u64(&X, 1); bi_sub(&A, &t, &X); check_div("S = Q Y - 1", &A, dl, &Q, 0);
        /* overshoot path via a perturbed seed (seed x 40/16 > 2x: T2 < T1); not at
         * schoolbook sizes (3 nq < 4 x 1024, the removed host tier's schoolbook limit, kept as the test's bound) */
        if (3 * nq >= 4 * (size_t)1024) {
        newton_seed_perturb = 40;
        bi_random(&A, 2 * nq - dl, GEN_UNIFORM, &rng);
        { size_t o = newton_st.overshoots; check_div("perturbed seed", &A, dl, &Q, 0);
          VERIFY(newton_st.overshoots > o, "perturbed seed did not take the overshoot path"); }
        newton_seed_perturb = 12;                       /* 25 % low: the repeat path */
        { size_t o = newton_st.repeats; check_div("low seed", &A, dl, &Q, 0);
          VERIFY(newton_st.repeats > o, "low seed did not repeat a step"); }
        newton_seed_perturb = 0;
        }
        check_div("a longer prewarm mu", &A, dl, &Q, 8);
        bi_free(&Y);
        printf("   2^%d limbs ok%s  (down %zu, up %zu corrections, %zu overshoots, %zu repeats so far)\n", lg, nq > EXACT_MAX_NQ ? " (by residues)" : "", newton_st.down_corr, newton_st.up_corr, newton_st.overshoots, newton_st.repeats);
    }
    printf("-- 3b. small quotients: k = 1 .. 8 limbs, dl = 1 .. 4 and nq - 2, against GMP\n");
    {
        static const size_t nqs[] = { 5, 40, 1500, (1u << 14) + 3 };
        int nk = 0;
        for (int qi = 0; qi < 4; qi++) for (int di = 0; di < 5; di++) for (size_t k = 1; k <= 8; k++) for (int gk = 0; gk < 2; gk++) {
            size_t nq = nqs[qi], dl = di < 4 ? (size_t)di + 1 : nq - 2;
            if (dl + 1 > nq || nq - dl + k - 1 < 1) continue;
            bi_random(&Q, nq, gk ? GEN_ONES : GEN_UNIFORM, &rng); if (Q.n != nq) continue;
            bi_random(&A, nq - dl + k - 1, gk ? GEN_ONES : GEN_UNIFORM, &rng); if (A.n != nq - dl + k - 1) continue;   /* A = S B^dl of nq + k - 1 limbs: a quotient of k limbs */
            char w[64]; snprintf(w, sizeof w, "nq %zu dl %zu k %zu %s", nq, dl, k, gk ? "ones" : "uniform");
            check_div(w, &A, dl, &Q, 0); nk++;
        }
        printf("   %d small-quotient divisions identical\n", nk);
    }

    {
        /* (the reciprocal's middle product, results/R415.md): (a) the band product rns_mul_band_db against the whole product -- mod B^w
         * identical with no low cut, the band [v, w) within one unit (mod B^(w - v)) with the cut at v - 1; (b) the reciprocal (always the cut
         * and middle-product form) against GMP directly -- mu vs floor(B^(nq+k)/Q), within recip_bound (results/RECIP.md; 8 units before), as the
         * whole-product reference it replaced was -- at several sizes, k and generators (a grid at the top doublings under
         * DIST_LOGN_TEST=20..22), and the path taken */
        printf("-- 5. the reciprocal's middle product\n");
        rng_t rng5 = {0x5E37ULL};                       /* its own operands, whatever the sections before draw (SIMP: one Q there missed the old 8-unit bound, results/SIMP.md; the bound is derived now and that Q is section 8 (a), results/RECIP.md) */
        bigint B1, C0, C1; bi_init(&B1); bi_init(&C0); bi_init(&C1);
        static const size_t band_sz[][2] = { {3000, 1000}, {(1u << 20) + 5, (1u << 19) + 3}, {(3u << 20) + 1, (3u << 19) - 5}, {(1u << 22) + 3, (1u << 21) + 1} };   /* the last two: grids split on both operands at cap 2^20..2^22, so the high cut skips pieces */
        for (int si = 0; si < 4; si++) for (int ki = 0; ki < 2; ki++) {
            size_t na = band_sz[si][0], nb = band_sz[si][1], v = na - nb + 1, w = v + nb + 3;   /* the reciprocal's shape: take x (j + 1), v = take - j, w = take + 4 */
            bi_random(&A, na, ki ? GEN_ONES : GEN_UNIFORM, &rng5); bi_random(&B1, nb, ki ? GEN_ONES : GEN_UNIFORM, &rng5);
            dbig a, b, c; db_init(&a); db_init(&b); db_init(&c); db_from_bi(&a, &A); db_from_bi(&b, &B1);
            rns_mul_dist_db(&c, &a, &b); db_to_bi(&C0, &c);
            size_t f0 = rns_dist_st.n_formed, s0 = rns_dist_st.n_skipped;
            rns_mul_band_db(&c, &a, &b, 0, w); db_to_bi(&C1, &c);
            size_t sk = rns_dist_st.n_skipped - s0, fo = rns_dist_st.n_formed - f0;
            mpz_t z0, z1, bw, bv; mpz_inits(z0, z1, bw, bv, NULL); bi_to_mpz(z0, &C0); bi_to_mpz(z1, &C1);
            mpz_ui_pow_ui(bw, BI_B10, w); mpz_ui_pow_ui(bv, BI_B10, v);
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
                bi_random(&Q, nq, kinds[gi], &rng5);
                size_t ks[3] = {nq / 2 + 1, nq, 2 * nq};
                for (int i = 0; i < 3; i++) {
                    size_t k = ks[i];
                    if (nq > (1u << 21) && (gi > 0 || i != 1)) continue;   /* the largest size: uniform, k = nq only (time) */
                    struct newton_mid_stats m0 = newton_mid_st; size_t s0 = rns_dist_st.n_skipped;
                    newton_db_recip(&t, &Q, k);
                    size_t nm = newton_mid_st.mid - m0.mid, sk = rns_dist_st.n_skipped - s0;
                    if (nq > (1u << 21) && getenv("DIST_LOGN_TEST")) VERIFY(sk > 0, "recip mid nq %zu k %zu: the cuts skipped nothing", nq, k);   /* grids need the lowered plane cap */
                    VERIFY(nm > 0, "recip mid nq %zu k %zu: the middle product was never taken", nq, k);
                    long err = recip_err(m, x, q, &Q, &t, k), lo, hi;   /* GMP's floor against the iterate; the bound as derived (results/RECIP.md) */
                    if (recip_bound(&Q, k, &lo, &hi)) VERIFY(err >= lo && err <= hi, "recip mid nq %zu k %zu %s: error %ld units, bound [%ld, %ld]", nq, k, gen_name[kinds[gi]], err, lo, hi);
                    if (gi == 0) printf("   nq %-8zu k %-8zu: error vs GMP %+ld, %zu rounds on the middle product, %zu pieces skipped\n", nq, k, err, nm, sk);
                }
            }
        }
        bi_free(&B1); bi_free(&C0); bi_free(&C1);

        /* newton_db_divmod_shifted (A = S B^dl, Q on the device, the prewarm reciprocal
         * as ecalc's device flow takes it) against GMP -- X exactly, R's residues mod three primes; the shapes:
         * ecalc's (S ~ Q, k ~ dl), k odd / even, dl < k/2 (s = dl: the kept mu too short, a fresh one), A a multiple of Q (R1 = 0, X_lo = 0),
         * Q all-ones / a power; forced corrections: ECALC_TEST_CORR +-7 (the final count moves by k) and NEWTON_DKM_TEST_HI +-7 (step 1's) */
        printf("-- 6. the division in two quotient halves (DKM)\n");
        {
            static const uint64_t qs6[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
            bigint S6, A6, X6, Y6; bi_init(&S6); bi_init(&A6); bi_init(&X6); bi_init(&Y6);
            mpz_t zr; mpz_init(zr);
            static const size_t nq6[] = { 1500, (1u << 14) + 3, (1u << 18) + 5, (1u << 20) + 1 };
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
                if (shape == 3) { bi_random(&Y6, 2, GEN_UNIFORM, &rng); tref_mul(&S6, &Q, &Y6); }   /* A = Q Y B^dl: R1 = 0, X_lo = 0 */
                else bi_random(&S6, sn, GEN_UNIFORM, &rng);
                bi_shl_limbs(&A6, &S6, dl);
                bi_to_mpz(a, &A6); bi_to_mpz(q, &Q); mpz_tdiv_qr(x, zr, a, q);
                uint64_t want[3]; for (int j = 0; j < 3; j++) want[j] = mpz_fdiv_ui(zr, qs6[j]);
                long nat = 0;                                         /* the unforced final signed correction count */
                static const long forced[][2] = { {0, 0}, {7, 0}, {-7, 0}, {0, 7}, {0, -7} };   /* {ECALC_TEST_CORR, NEWTON_DKM_TEST_HI}; row 0: unforced */
                for (int f = 0; f < 5; f++) {
                    if (qi == 3 && f > 1) continue;                  /* the largest size: unforced and one forced (time) */
                    long tk = forced[f][0], th = forced[f][1];
                    if (shape == 3 && tk > 0) continue;              /* X_lo = 0 there: X - k would need a borrow from X_hi (the hook refuses) */
                    char b1[16], b2[16]; snprintf(b1, sizeof b1, "%ld", tk); snprintf(b2, sizeof b2, "%ld", th);
                    setenv("ECALC_TEST_CORR", b1, 1); setenv("NEWTON_DKM_TEST_HI", b2, 1);
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
                    VERIFY(okx && okr, "dkm nq %zu shape %d (tk %ld th %ld): X %s, R residues %s", nq, shape, tk, th, okx ? "ok" : "DIFFERS", okr ? "ok" : "DIFFER");
                    if (f == 0) nat = sg;
                    if (f >= 1 && tk) VERIFY(sg == nat + tk, "dkm nq %zu shape %d: ECALC_TEST_CORR=%ld moved the final count %ld -> %ld", nq, shape, tk, nat, sg);
                    if (f >= 1 && th) VERIFY(sg == nat && hc + 3 >= (size_t)(th < 0 ? -th : th), "dkm nq %zu shape %d: NEWTON_DKM_TEST_HI=%ld: final %ld (unforced %ld), step 1 %zu", nq, shape, th, sg, nat, hc);
                    VERIFY(sg >= -64 && sg <= 64, "dkm: count");
                    if (shape < 3 || f < 1) printf("   nq %-8zu shape %d tk %+3ld th %+3ld: X %zu limbs identical, R ok; final corrections %+ld, step 1 %zu\n", nq, shape, tk, th, X6.n, sg, hc);
                    db_free(&Sd); db_free(&Qd6); newton_db_free_scratch();
                }
            }
            unsetenv("ECALC_TEST_CORR"); unsetenv("NEWTON_DKM_TEST_HI");
            mpz_clear(zr); bi_free(&S6); bi_free(&A6); bi_free(&X6); bi_free(&Y6);
        }

        /* the DKM division with the X_hi / X_lo hooks (no X0): the X_hi hook gets X's limbs
         * >= s exactly (whatever step 2 and the forced corrections do), the division returns X_lo; released (the hook's 1): X_lo0 with the
         * corrections deferred (X_lo0 + newton_x_dx = X mod B^s); not released: X mod B^s itself, newton_x_dx = 0.  Operands: A = X Q + R
         * built from a chosen X (R = -X Q mod B^dl < Q: A a multiple of B^dl), X's low half random or B^s - 3 -- with ECALC_TEST_CORR=-7 the
         * estimate X_lo0 >= B^s (the carry case: the hook refuses, the corrections go in place); ECALC_TEST_CORR +-7, NEWTON_DKM_TEST_HI 7 */
        printf("-- 7. the X_hi / X_lo hooks of the DKM division (MN_OUT_DKM_HI), X_lo0 >= B^s constructed\n");
        {
            static const uint64_t qs7[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
            bigint S7, X7, H7, L7; bi_init(&S7); bi_init(&X7); bi_init(&H7); bi_init(&L7);
            mpz_t xt, bp, bd, xq, r7, hq, lq; mpz_inits(xt, bp, bd, xq, r7, hq, lq, NULL);
            int defer_env = newton_x_defer; newton_x_defer = 1;
            static const size_t nq7[] = { 1500, (1u << 14) + 3, (1u << 18) + 5 };
            int ncarry = 0;
            for (int qi = 0; qi < 3; qi++) for (int low = 0; low < 2; low++) for (int f = 0; f < 5; f++) {
                static const long forced7[5][3] = { {0, 0, 1}, {7, 0, 1}, {-20, 0, 1}, {0, 7, 1}, {-20, 0, 0} };   /* {ECALC_TEST_CORR, NEWTON_DKM_TEST_HI, release} */
                long tk = forced7[f][0], th = forced7[f][1];
                size_t nq = nq7[qi], dl = nq - 3, xn = nq - 3, s = 0;
                bi_random(&Q, nq, GEN_UNIFORM, &rng); if (Q.n != nq) continue;
                bi_to_mpz(q, &Q);
                bi_random(&X7, xn, GEN_UNIFORM, &rng); if (X7.n != xn) continue;
                mpz_ui_pow_ui(bd, 10, 18 * dl);
                for (int pass = 0; pass < 3; pass++) {        /* s from the operands' lengths; X's low half set, then s re-derived (stable) */
                    bi_to_mpz(xt, &X7);
                    if (low && s) { mpz_ui_pow_ui(bp, 10, 18 * s);
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
                mpz_ui_pow_ui(bp, 10, 18 * h7.s);
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
            unsetenv("ECALC_TEST_CORR"); unsetenv("NEWTON_DKM_TEST_HI"); newton_x_defer = defer_env;
            mpz_clears(xt, bp, bd, xq, r7, hq, lq, NULL); bi_free(&S7); bi_free(&X7); bi_free(&H7); bi_free(&L7); bi_free(&h7.Xh);
        }
    }

    /* 8. the chain guard (results/RECIP.md).  (a) SIMP's operand: Q of 131075 limbs, top limb 2.35e16 (M = B^nq / Q = 42.6), k 65538:
     * the last doubling 32769 -> 65538 is exact -- error -22 units off (within -(2 ceil(M) + 6)), +-8 on.  (b) k = 1024, 2048 at nq
     * 1027 (every doubling exact), 100 uniform Q: off, the error squares from doubling to doubling (printed: the count beyond +-8 and the
     * largest); on, +-8 for every Q.  (c) DKM's step-1 estimate X0 = ((A1 >> (nq - 1)) mu_top) >> (k1 + 1) for a quotient of kq = 4095
     * limbs at nq 8200 (h = k1 = 2048: mu_top is all of mu, its error is not cut), 300 Q x 3 A1 (random, B^n - 1, a multiple of Q
     * minus 1): off, max |X1 - X0| printed (> 64 would be fatal in the division: "DKM step 1: N corrections"); on, within 2; and the
     * division itself (newton_db_divmod_shifted, A = A1 B^2047, the guard on) exact against GMP for the first 3 Q and for every (Q, A1)
     * whose estimate was beyond 64 with the guard off */
    printf("-- 8. the reciprocal's chain guard (NEWTON_CHAIN_GUARD)\n");
    {
        int guard_env = newton_chain_guard();
        long lo, hi;
        {   rng_t r8 = {0x9A4EA83C95610E7BULL};                 /* the rng state of SIMP's section-5 draw (t_newton 20 at SIMP 4003968, results/SIMP.md) */
            bi_random(&Q, 131075, GEN_UNIFORM, &r8);
            for (int on = 0; on < 2; on++) {
                newton_chain_guard_set(on); newton_db_recip(&mu, &Q, 65538);
                long err = recip_err(m, x, q, &Q, &mu, 65538); recip_bound(&Q, 65538, &lo, &hi);
                VERIFY(err >= lo && err <= hi, "chain guard %s, SIMP's Q: error %ld, bound [%ld, %ld]", on ? "on" : "off", err, lo, hi);
                printf("   SIMP's Q (nq 131075, top limb %llu), k 65538, guard %s: error %+ld units, bound [%ld, %ld]\n", (unsigned long long)Q.l[Q.n - 1], on ? "on " : "off", err, lo, hi);
            }
        }
        for (int kb = 1024; kb <= 2048; kb *= 2) {
            rng_t r8 = {0xABCDULL}; size_t nq = 1027, k = (size_t)kb; long nout[2] = {0, 0}; double emax[2] = {0, 0};
            for (int i = 0; i < 100; i++) {
                bi_random(&Q, nq, GEN_UNIFORM, &r8);
                for (int on = 0; on < 2; on++) {
                    newton_chain_guard_set(on); newton_db_recip(&mu, &Q, k);
                    long err = recip_err(m, x, q, &Q, &mu, k); double ed = mpz_get_d(x); if (ed < 0) ed = -ed;
                    if (ed > emax[on]) emax[on] = ed;
                    if (err < -8 || err > 8) nout[on]++;
                    if (on) VERIFY(err >= -8 && err <= 8, "chain guard on, nq %zu k %zu Q #%d: error %ld units", nq, k, i, err);
                }
            }
            printf("   nq %zu k %zu, 100 Q: guard off %ld beyond +-8 (largest |error| %.3g units), on %ld (largest %.3g)\n", nq, k, nout[0], emax[0], nout[1], emax[1]);
        }
        {   static const uint64_t qs8[3] = { 2305843009213693951ULL, 1000000000000000003ULL, 4611686018427387847ULL };
            rng_t r8 = {0x99ULL}; size_t nq = 8200, kq = 4095, s8 = kq / 2, k1 = kq - s8, na1 = nq + k1 - 1;
            bigint A1, mu0, mu1; bi_init(&A1); bi_init(&mu0); bi_init(&mu1);
            mpz_t a1, x1, x0, bp; mpz_inits(a1, x1, x0, bp, NULL);
            double cmax[2] = {0, 0}; int nfat = 0, ndiv = 0;
            for (int i = 0; i < 300; i++) {
                bi_random(&Q, nq, GEN_UNIFORM, &r8); bi_to_mpz(q, &Q);
                dbig Qd8; db_init(&Qd8); db_from_bi(&Qd8, &Q);
                newton_db_Qd = &Qd8;                              /* the device flow's reciprocal: to h = newton_dkm_h(4095) = 2048 limbs */
                newton_chain_guard_set(0); newton_db_recip(&mu0, &Q, kq);
                newton_chain_guard_set(1); newton_db_recip(&mu1, &Q, kq);
                newton_db_Qd = 0; newton_db_free_scratch();
                for (int ak = 0; ak < 3; ak++) {
                    bi_random(&A1, na1, GEN_UNIFORM, &r8); bi_to_mpz(a1, &A1);
                    mpz_ui_pow_ui(bp, BI_B10, na1);
                    if (ak == 1) mpz_sub_ui(a1, bp, 1);
                    if (ak == 2) { mpz_fdiv_q(x1, bp, q); mpz_mul(a1, x1, q); mpz_sub_ui(a1, a1, 1); }
                    mpz_fdiv_q(x1, a1, q);
                    double c[2];
                    for (int on = 0; on < 2; on++) {
                        bigint *mt = on ? &mu1 : &mu0;
                        bi_to_mpz(x0, mt); mpz_ui_pow_ui(bp, BI_B10, mt->n - (k1 + 1)); mpz_tdiv_q(x0, x0, bp);   /* mu's top k1 + 1 limbs */
                        mpz_ui_pow_ui(bp, BI_B10, nq - 1); mpz_tdiv_q(r, a1, bp); mpz_mul(x0, x0, r);
                        mpz_ui_pow_ui(bp, BI_B10, k1 + 1); mpz_tdiv_q(x0, x0, bp);
                        mpz_sub(x0, x1, x0); c[on] = mpz_get_d(x0); if (c[on] < 0) c[on] = -c[on];
                        if (c[on] > cmax[on]) cmax[on] = c[on];
                    }
                    VERIFY(c[1] <= 2, "chain guard on, DKM estimate Q #%d A %d: X1 - X0 = %.3g", i, ak, c[1]);
                    if (c[0] > 64) nfat++;
                    if (c[0] > 64 || (i < 3 && ak == 0)) {               /* the division itself, the guard on */
                        bigint S8, X8; bi_init(&S8); bi_init(&X8); bi_from_mpz(&S8, a1);
                        if (S8.n == na1) {
                            size_t dl = s8; dbig Sd, Qd; db_init(&Sd); db_init(&Qd); db_from_bi(&Sd, &S8); db_from_bi(&Qd, &Q);
                            newton_db_Qd = &Qd; newton_db_mu_host = 0;
                            newton_db_recip(&mu, &Q, kq);
                            newton_stats b = newton_st; uint64_t got[3];
                            newton_db_divmod_shifted(&X8, &Sd, dl, &Qd, qs8, 3, got);
                            newton_db_Qd = 0; newton_db_mu_host = 1;
                            mpz_ui_pow_ui(bp, BI_B10, dl); mpz_mul(a, a1, bp); mpz_tdiv_qr(x0, x1, a, q);
                            int okx = bi_eq_mpz(&X8, x0), okr = 1; for (int j = 0; j < 3; j++) if (mpz_fdiv_ui(x1, qs8[j]) != got[j]) okr = 0;
                            size_t hc = newton_st.dkm_corr - b.dkm_corr;
                            VERIFY(okx && okr && hc <= 4, "chain guard on, DKM division Q #%d A %d: X %s, R %s, step-1 corrections %zu", i, ak, okx ? "ok" : "DIFFERS", okr ? "ok" : "DIFFER", hc);
                            db_free(&Sd); db_free(&Qd); newton_db_free_scratch(); ndiv++;
                            mpz_fdiv_q(x1, a1, q);
                        }
                        bi_free(&S8); bi_free(&X8);
                    }
                }
                db_free(&Qd8);
            }
            printf("   DKM step 1, nq %zu kq %zu (h = k1 = %zu), 300 Q x 3 A: max |X1 - X0| guard off %.3g (%d beyond 64: fatal in the division), on %.3g; %d divisions with the guard on exact\n",
                   nq, kq, k1, cmax[0], nfat, cmax[1], ndiv);
            mpz_clears(a1, x1, x0, bp, NULL); bi_free(&A1); bi_free(&mu0); bi_free(&mu1);
        }
        newton_chain_guard_set(guard_env);
    }

    printf("-- 4. time at 2^%d limbs\n", LOGMAX);
    {
        size_t nq = (size_t)1 << LOGMAX, dl = nq / 2;
        bi_random(&Q, nq, GEN_UNIFORM, &rng); bi_random(&A, 2 * nq - dl, GEN_UNIFORM, &rng);
        memset(&newton_st, 0, sizeof newton_st); memset(&rns_st, 0, sizeof rns_st);
        double t0 = mem_now();
        check_div("time", &A, dl, &Q, 0);
        double dt = mem_now() - t0;
        printf("   divmod %zu / %zu limbs (with the prewarm reciprocal and the check): %.2f s (recip %.2f, %zu iters; corrections %zu/%zu)\n",
               A.n + dl, Q.n, dt, newton_st.t_recip, newton_st.iters, newton_st.down_corr, newton_st.up_corr);
        harness_result("divmod_time", "s", dt);
        harness_result("recip_time", "s", newton_st.t_recip);
    }
    printf("VmHWM %.1f GB\n", mem_vmhwm() / 1e9);
    rns_shutdown();
    return verify_done("t_newton");
}
