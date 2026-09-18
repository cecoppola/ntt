/* newton_db.c - the reciprocal and division of newton.c on device-resident
 * numbers (WP5 step 3): the same iteration, statistics and correction logic,
 * every big operation a dbig kernel and every product the distributed tier.
 * The host bigint interface is kept at the phase boundary: Q (and A) are
 * copied to device once, mu / X / R come back once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "newton.h"
#include "dbig.h"
#include "rns_mul.h"
#include "mem.h"
static int nv = -1, anchor = 1;
static dbig g_r, g_r2, g_qt, g_t1, g_t2, g_mu, g_t, g_xq;
void newton_db_free_scratch(void) { db_free(&g_r); db_free(&g_r2); db_free(&g_qt); db_free(&g_t1); db_free(&g_t2); db_free(&g_mu); db_free(&g_t); db_free(&g_xq); }

/* the seed is small: the host version, copied in */
static void seed_db(dbig *r, const bigint *Q, size_t *j)
{
    bigint hr; bi_init(&hr);
    newton_seed_host(&hr, Q, j);
    db_from_bi(r, &hr); bi_free(&hr);
}
/* r -= r / 16 (overshoot shrink), through the host: rare */
static void shrink_db(dbig *r)
{
    bigint h, d; bi_init(&h); bi_init(&d);
    db_to_bi(&h, r); bi_divmod_u64(&d, &h, 16); bi_sub(&h, &h, &d); db_from_bi(r, &h);
    bi_free(&h); bi_free(&d);
}
/* is any limb of a below index e nonzero? (a - B^e test when a has e+1 limbs with top 1) */
static int maxidx_below(const dbig *a, size_t e) { dbig v = db_view(a, 0, e); v.n = e; db_norm(&v); return v.n != 0; }
/* (a << j) <= b, both with the same limb count (rare path: an explicit shift) */
static int shifted_cmp_le(const dbig *a, size_t j, const dbig *b) { dbig t; db_init(&t); db_shl_limbs(&t, a, j); int le = db_cmp(&t, b) <= 0; db_free(&t); return le; }
static void recip_db(dbig *mu, const dbig *Qd, const bigint *Q, size_t k)
{
    double t0 = mem_now();
    if (nv < 0) { nv = getenv("NEWTON_VERBOSE") ? atoi(getenv("NEWTON_VERBOSE")) : 0; if (getenv("NEWTON_ANCHOR")) anchor = atoi(getenv("NEWTON_ANCHOR")); }
    size_t nq = Qd->n, j;
    dbig r = g_r, r2 = g_r2, t1 = g_t1, t2 = g_t2;
    /* the loop's scratch at its final capacity, once (no reallocation inside the loop): r and mu k + 2 limbs,
     * t1 the Q_t r product (nq + k + 2), r2 the r d product (2k + 2), t2 the shifted u (2k + 2) */
    db_reserve(&r, k + 4); db_reserve(mu, k + 4); db_reserve(&t1, nq + k + 8); db_reserve(&r2, 2 * k + 8); db_reserve(&t2, 2 * k + 8);
    seed_db(&r, Q, &j);
    while (j < k) {
        size_t jn = k;
        if (anchor) { while ((jn + 1) / 2 > j) jn = (jn + 1) / 2; }
        else jn = 2 * j < k ? 2 * j : k;
        for (;;) {
            size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
            dbig qt = db_view(Qd, nq - take, take);                  /* top limbs of Q, in place */
            rns_mul_dist_db(&t1, &qt, &r);                          /* Q_t r */
            if (j <= take) db_shr_limbs(&t2, &t1, take - j); else db_shl_limbs(&t2, &t1, j - take);   /* u ~ B^(2j) */
            /* neg: u > B^(2j), i.e. u has 2j+1 limbs and is not exactly B^(2j) */
            int neg = t2.n > 2 * j + 1 || (t2.n == 2 * j + 1 && (db_top(&t2) > 1 || maxidx_below(&t2, 2 * j)));
            if (neg) db_sub_pow(&t1, &t2, 2 * j); else db_pow_sub(&t1, 2 * j, &t2);   /* |d| */
            rns_mul_dist_db(&r2, &r, &t1);                          /* r |d| */
            db_shr_limbs(&t1, &r2, j);                              /* |corr| */
            int converged = t1.n <= j + 1;
            if (neg) {                                              /* r' = (r << j) - corr; overshoot if that would be <= 0 */
                size_t rn = r.n + j; int over = rn < t1.n || (rn == t1.n && shifted_cmp_le(&r, j, &t1));
                if (over) { newton_st.overshoots++; shrink_db(&r); continue; }
                db_sub_shifted(&r2, &r, j, &t1);
            } else db_add_shifted(&r2, &r, j, &t1);
            if (converged) db_copy(&r, &r2); else db_shr_limbs(&r, &r2, j);
            if (nv) printf("newton(db) j %zu -> %zu (k %zu): take %zu, r %zu limbs%s   dev pools %.1f GB\n", j, jn, k, take, r.n, converged ? "" : " (repeat)", mem_dev_pool_bytes() / 1e9);
            if (!converged) { newton_st.repeats++; continue; }
            break;
        }
        if (jn < 2 * j) { db_shr_limbs(&t2, &r, 2 * j - jn); dbig sw = r; r = t2; t2 = sw; }
        j = jn;
        newton_st.iters++;
    }
    if (j > k) { db_shr_limbs(&t2, &r, j - k); dbig sw = r; r = t2; t2 = sw; }
    db_copy(mu, &r);
    g_r = r; g_r2 = r2; g_t1 = t1; g_t2 = t2;
    db_free(&g_r); db_free(&g_r2); db_free(&g_t1); db_free(&g_t2);          /* back to the free lists: the division reuses the blocks */
    newton_st.t_recip += mem_now() - t0;
    if (getenv("RNS_VERBOSE")) {
        printf("recip(db) %.2f s: dist %zu calls %.2f s (load %.2f ntt %.2f crt %.2f spills %.2f); dbig shift %zu/%.2f addsub %zu/%.2f maxidx %zu/%.2f reserve %zu/%.2f; pools %.1f GB\n",
               mem_now() - t0, rns_dist_st.n, rns_dist_st.t_total, rns_dist_st.t_load, rns_dist_st.t_ntt, rns_dist_st.t_crt, rns_dist_st.t_merge,
               db_st.n_shift, db_st.t_shift, db_st.n_addsub, db_st.t_addsub, db_st.n_maxidx, db_st.t_maxidx, db_st.n_reserve, db_st.t_reserve, db_pool_bytes() / 1e9);
        memset(&rns_dist_st, 0, sizeof rns_dist_st); memset(&db_st, 0, sizeof db_st);
    }
}
void newton_db_recip(bigint *mu, const bigint *Q, size_t k)
{
    dbig Qd, mud; db_init(&Qd); db_init(&mud);
    db_from_bi(&Qd, Q);
    recip_db(&mud, &Qd, Q, k);
    db_to_bi(mu, &mud);
    db_free(&Qd); db_free(&mud);
}
/* X = floor(A / Q), R = A - X Q; mu_opt: a reciprocal of Q with >= k + 1 limbs (host) */
int newton_db_free_inputs = 0;                       /* NEWTON_DEVICE_FREE=1: release the host A once it is in the staging */
void newton_db_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt)
{
    double t0 = mem_now();
    if (!Q->n) { fprintf(stderr, "newton_db_divmod: Q = 0\n"); abort(); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    size_t nq = Q->n, na = A->n, k = na - nq + 1;
    dbig Qd, mu = g_mu, t = g_t, xq = g_xq, Xd, one; db_init(&Qd); db_init(&Xd); db_init(&one);
    db_from_bi(&Qd, Q);
    if (mu_opt && mu_opt->n >= k + 1) { bigint mh; bi_init(&mh); bi_shr(&mh, mu_opt, 64 * (mu_opt->n - (k + 1))); db_from_bi(&mu, &mh); bi_free(&mh); }
    else recip_db(&mu, &Qd, Q, k);
    /* A stays on the host: its top k limbs go through the pinned staging (registered) for the gather, its low
     * window is used by the CPU below.  X = ((A >> (nq-1)) mu) >> (k + 1) */
    const uint64_t *a = A->l; int staged = 0;
    if (!mem_is_registered(a, na * 8)) {
        uint64_t *hs = rns_hstage(0); size_t cap = (size_t)1 << rns_pool_log();
        if (na > cap) { fprintf(stderr, "newton_db_divmod: A (%zu limbs) exceeds the staging (%zu)\n", na, cap); abort(); }
#pragma omp parallel for schedule(static)
        for (size_t i = 0; i < na; i += 1 << 20) { size_t m = na - i < (1 << 20) ? na - i : (1 << 20); memcpy(hs + i, a + i, m * 8); }
        a = hs; staged = 1;
    }
    if (newton_db_free_inputs && staged) bi_free((bigint *)A);          /* the pageable copy is no longer needed */
    rns_mul_dist_hd(&t, a + (nq - 1), na - (nq - 1), &mu);
    db_shr_limbs(&Xd, &t, k + 1);
    /* R = (A - low(X Q)) mod B^w over the window w = nq + 2, on the host (as the host path) */
    size_t w = nq + 2;
    rns_mul_low_db(&xq, &Xd, &Qd, w);
    bigint hxq; bi_init(&hxq); db_to_bi(&hxq, &xq);
    db_to_bi(X, &Xd);
    bi_reserve(R, w + 1); bi_reserve(&hxq, w + 1);
    {
        size_t an = na < w ? na : w;
        for (size_t i = hxq.n; i < w; i++) hxq.l[i] = 0;
        limb_sub(R->l, a, an, hxq.l, an);
        for (size_t i = an; i < w; i++) R->l[i] = 0;
    }
    size_t nc = 0;
    for (;;) {
        if (bi_limb_negative(R->l[w - 1])) {
            bigint o; bi_init(&o); bi_set_u64(&o, 1); bi_sub(X, X, &o); bi_free(&o);
            uint64_t c = limb_add(R->l, R->l, w, Q->l, Q->n); (void)c;
            newton_st.down_corr++;
        } else {
            R->n = w; bi_norm(R);
            if (bi_cmp(R, Q) < 0) break;
            limb_sub(R->l, R->l, w, Q->l, Q->n);
            bi_add_u64(X, 1);
            newton_st.up_corr++;
        }
        if (++nc > 64) { fprintf(stderr, "newton_db_divmod: %zu corrections, mu is wrong\n", nc); abort(); }
    }
    R->n = w; bi_norm(R);
    bi_free(&hxq);
    g_mu = mu; g_t = t; g_xq = xq;
    db_free(&Qd); db_free(&Xd); db_free(&one);
    newton_st.t_div += mem_now() - t0;
}
