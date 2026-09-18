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
static void recip_db(dbig *mu, const dbig *Qd, const bigint *Q, size_t k)
{
    double t0 = mem_now();
    if (nv < 0) { nv = getenv("NEWTON_VERBOSE") ? atoi(getenv("NEWTON_VERBOSE")) : 0; if (getenv("NEWTON_ANCHOR")) anchor = atoi(getenv("NEWTON_ANCHOR")); }
    size_t nq = Qd->n, j;
    dbig r = g_r, r2 = g_r2, t1 = g_t1, t2 = g_t2, pw; db_init(&pw);
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
            db_set_base_pow(&pw, 2 * j);
            int neg = db_cmp(&t2, &pw) > 0;
            if (neg) db_sub(&t1, &t2, &pw); else db_sub(&t1, &pw, &t2);   /* |d| */
            rns_mul_dist_db(&r2, &r, &t1);                          /* r |d| */
            db_shr_limbs(&t1, &r2, j);                              /* |corr| */
            int converged = t1.n <= j + 1;
            db_shl_limbs(&t2, &r, j);                               /* r << 64 j */
            if (neg) {
                if (db_cmp(&t2, &t1) <= 0) { newton_st.overshoots++; shrink_db(&r); continue; }
                db_sub(&r2, &t2, &t1);
            } else db_add(&r2, &t2, &t1);
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
    db_free(&pw);
    g_r = r; g_r2 = r2; g_t1 = t1; g_t2 = t2;
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
void newton_db_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt)
{
    double t0 = mem_now();
    if (!Q->n) { fprintf(stderr, "newton_db_divmod: Q = 0\n"); abort(); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    size_t nq = Q->n, na = A->n, k = na - nq + 1;
    dbig Ad, Qd, mu = g_mu, t = g_t, xq = g_xq, Xd, Rd, one, tmp; db_init(&Ad); db_init(&Qd); db_init(&Xd); db_init(&Rd); db_init(&one); db_init(&tmp);
    db_from_bi(&Ad, A); db_from_bi(&Qd, Q);
    if (mu_opt && mu_opt->n >= k + 1) { bigint mh; bi_init(&mh); bi_shr(&mh, mu_opt, 64 * (mu_opt->n - (k + 1))); db_from_bi(&mu, &mh); bi_free(&mh); }
    else recip_db(&mu, &Qd, Q, k);
    /* X = ((A >> (nq-1)) mu) >> (k + 1) */
    { dbig Ah = db_view(&Ad, nq - 1, na - (nq - 1));
      rns_mul_dist_db(&t, &Ah, &mu);
      db_shr_limbs(&Xd, &t, k + 1); }
    /* R = (A - X Q) mod B^w over the window w = nq + 2: the low w limbs of X Q from the truncated operands */
    size_t w = nq + 2;
    { dbig Xw = db_view(&Xd, 0, Xd.n < w ? Xd.n : w), Qw = db_view(&Qd, 0, nq < w ? nq : w);
      db_norm(&Xw); db_norm(&Qw);
      rns_mul_dist_db(&xq, &Xw, &Qw);
      if (xq.n > w) { xq.n = w; db_norm(&xq); }                 /* mod B^w */
      dbig Aw = db_view(&Ad, 0, na < w ? na : w); db_norm(&Aw);
      /* R = Aw - xq mod B^w: if Aw < xq the true value is negative: R = Aw + B^w - xq */
      if (db_cmp(&Aw, &xq) >= 0) db_sub(&Rd, &Aw, &xq);
      else { db_set_base_pow(&tmp, w); db_add(&Rd, &Aw, &tmp); db_sub(&Rd, &Rd, &xq); if (Rd.n > w) { Rd.n = w; db_norm(&Rd); } }
    }
    db_set_u64(&one, 1);
    size_t nc = 0;
    for (;;) {
        uint64_t top = Rd.n == w ? db_top(&Rd) : 0;
        if (bi_limb_negative(top)) {                             /* negative: X too large */
            db_sub(&Xd, &Xd, &one);
            db_add(&Rd, &Rd, &Qd); if (Rd.n > w) { Rd.n = w; db_norm(&Rd); }   /* mod B^w */
            newton_st.down_corr++;
        } else {
            if (db_cmp(&Rd, &Qd) < 0) break;
            db_sub(&Rd, &Rd, &Qd);
            db_add(&Xd, &Xd, &one);
            newton_st.up_corr++;
        }
        if (++nc > 64) { fprintf(stderr, "newton_db_divmod: %zu corrections, mu is wrong\n", nc); abort(); }
    }
    db_to_bi(X, &Xd); db_to_bi(R, &Rd);
    g_mu = mu; g_t = t; g_xq = xq;
    db_free(&Ad); db_free(&Qd); db_free(&Xd); db_free(&Rd); db_free(&one); db_free(&tmp);
    if (g_t.cap > ((size_t)1 << 28)) { db_free(&g_t); db_free(&g_xq); db_free(&g_mu); }
    newton_st.t_div += mem_now() - t0;
}
