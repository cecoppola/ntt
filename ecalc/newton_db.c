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
static void seed_db(dbig *r, const bigint *Q, const dbig *Qd, size_t *j)
{
    bigint hr; bi_init(&hr);
    if (Q && Q->l) newton_seed_host(&hr, Q, j);
    else { size_t nq = Qd->n, top = nq < 4 ? nq : 4; uint64_t t4[4]; for (size_t i = 0; i < top; i++) t4[i] = db_limb(Qd, nq - top + i); newton_seed_top(&hr, t4, top, nq, j); }   /* I3: Q only on the device */
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
    /* scratch at its final capacity, once: r and r2 (k + 4 limbs, swapped as the iterate advances), t1 for
     * the products (max(nq + k, 2k) + 8); u, d, corr are views or land in the free one of r2 / t1 -- no
     * other temporaries, so the donated bs regions cover everything (RESULTS.md 59) */
    dbig r = g_r, r2 = g_r2, t1 = g_t1, t2 = g_t2;
    size_t tcap = (nq + k > 2 * k ? nq + k : 2 * k) + 8;
    db_reserve(&r, k + 4); db_reserve(&r2, k + 4); db_reserve(&t1, tcap);
    seed_db(&r, Q, Qd, &j);
    while (j < k) {
        size_t jn = k;
        if (anchor) { while ((jn + 1) / 2 > j) jn = (jn + 1) / 2; }
        else jn = 2 * j < k ? 2 * j : k;
        for (;;) {
            size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
            dbig qt = db_view(Qd, nq - take, take);                  /* top limbs of Q */
            rns_mul_dist_db(&t1, &qt, &r);                          /* Q_t r */
            dbig u;                                                 /* u ~ B^(2j): t1 >> (take - j) as a view, or << (j - take) (rare) */
            if (j <= take) { u = db_view(&t1, take - j, t1.n > take - j ? t1.n - (take - j) : 0); db_norm(&u); }
            else { db_shl_limbs(&t2, &t1, j - take); u = t2; }
            int neg = u.n > 2 * j + 1 || (u.n == 2 * j + 1 && (db_top(&u) > 1 || maxidx_below(&u, 2 * j)));
            if (neg) db_sub_pow(&r2, &u, 2 * j); else db_pow_sub(&r2, 2 * j, &u);   /* d = |B^(2j) - u| -> r2 */
            rns_mul_dist_db(&t1, &r, &r2);                          /* r |d| -> t1 (u is consumed) */
            dbig corr = db_view(&t1, j, t1.n > j ? t1.n - j : 0); db_norm(&corr);   /* |corr| = t1 >> j */
            int converged = corr.n <= j + 1;
            if (neg) {                                              /* r' = (r << j) - corr; overshoot if that would be <= 0 */
                size_t rn = r.n + j; int over = rn < corr.n || (rn == corr.n && shifted_cmp_le(&r, j, &corr));
                if (over) { newton_st.overshoots++; shrink_db(&r); continue; }
                db_sub_shifted(&r2, &r, j, &corr);
            } else db_add_shifted(&r2, &r, j, &corr);
            if (converged) { dbig sw = r; r = r2; r2 = sw; }          /* r <- r' by swap */
            else db_shr_limbs(&r, &r2, j);
            if (nv) printf("newton(db) j %zu -> %zu (k %zu): take %zu, r %zu limbs%s   dev pools %.1f GB\n", j, jn, k, take, r.n, converged ? "" : " (repeat)", mem_dev_pool_bytes() / 1e9);
            if (!converged) { newton_st.repeats++; continue; }
            break;
        }
        if (jn < 2 * j) { db_shr_limbs(&r2, &r, 2 * j - jn); dbig sw = r; r = r2; r2 = sw; }
        j = jn;
        newton_st.iters++;
    }
    if (j > k) { db_shr_limbs(&r2, &r, j - k); dbig sw = r; r = r2; r2 = sw; }
    { dbig sw = *mu; *mu = r; r = sw; }                          /* mu takes r's block */
    g_r = r; g_r2 = r2; g_t1 = t1; g_t2 = t2;
    newton_st.t_recip += mem_now() - t0;
    if (getenv("RNS_VERBOSE")) {
        printf("recip(db) %.2f s: dist %zu calls %.2f s (load %.2f ntt %.2f crt %.2f spills %.2f); dbig shift %zu/%.2f addsub %zu/%.2f maxidx %zu/%.2f reserve %zu/%.2f; pools %.1f GB\n",
               mem_now() - t0, rns_dist_st.n, rns_dist_st.t_total, rns_dist_st.t_load, rns_dist_st.t_ntt, rns_dist_st.t_crt, rns_dist_st.t_merge,
               db_st.n_shift, db_st.t_shift, db_st.n_addsub, db_st.t_addsub, db_st.n_maxidx, db_st.t_maxidx, db_st.n_reserve, db_st.t_reserve, db_pool_bytes() / 1e9);
        memset(&rns_dist_st, 0, sizeof rns_dist_st); memset(&db_st, 0, sizeof db_st);
    }
    db_free(&g_r); db_free(&g_r2); db_free(&g_t1); db_free(&g_t2);          /* back to the free lists: the division reuses the blocks */
}
static dbig g_mu_kept; static size_t g_mu_k;               /* the prewarm's mu stays on device for the division ... */
static const uint64_t *g_mu_ql; static size_t g_mu_qn; static uint64_t g_mu_qtop;   /* ... tagged with the Q it belongs to */
/* Phase 8 overlap: Q may already be on device (the top level of bs left it there); the caller owns it.
 * mu_host: 0 = no host copy of mu (the division takes the kept device mu) */
dbig *newton_db_Qd = 0; int newton_db_mu_host = 1;
void (*newton_db_x_hook)(bigint *X, void *arg) = 0; void *newton_db_x_arg = 0;   /* called with X on the host before the low product */
void newton_db_recip(bigint *mu, const bigint *Q, size_t k)
{
    dbig Qd; db_init(&Qd);
    if (newton_db_Qd) Qd = *newton_db_Qd; else db_from_bi(&Qd, Q);
    recip_db(&g_mu_kept, &Qd, Q, k); g_mu_k = k;
    if (newton_db_Qd) { g_mu_ql = newton_db_Qd->q[0]; g_mu_qn = newton_db_Qd->n; g_mu_qtop = db_top(newton_db_Qd); }   /* tagged by the device Q */
    else { g_mu_ql = Q->l; g_mu_qn = Q->n; g_mu_qtop = Q->n ? Q->l[Q->n - 1] : 0; }
    if (newton_db_mu_host) db_to_bi(mu, &g_mu_kept);             /* the host copy too (tests, the host path's fallback) */
    else { mu->n = 0; }
    if (!newton_db_Qd) db_free(&Qd);
}
/* X = floor(A / Q), R = A - X Q; mu_opt: a reciprocal of Q with >= k + 1 limbs (host) */
int newton_db_free_inputs = 0;                       /* the host A shrinks to its remainder window once its top is on device */
void newton_db_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt)
{
    double t0 = mem_now();
    if (!Q->n) { fprintf(stderr, "newton_db_divmod: Q = 0\n"); abort(); }
    if (bi_cmp(A, Q) < 0) { bi_set_zero(X); bi_copy(R, A); return; }
    size_t nq = Q->n, na = A->n, k = na - nq + 1;
    dbig Qd, mu = g_mu, t = g_t, xq = g_xq, Xd, one; db_init(&Qd); db_init(&Xd); db_init(&one);
    double ta = mem_now();
    if (newton_db_Qd) Qd = *newton_db_Qd; else db_from_bi(&Qd, Q);
    int kept_ok = g_mu_kept.n >= k + 1 && g_mu_ql == Q->l && g_mu_qn == Q->n && g_mu_qtop == Q->l[Q->n - 1];
    if (g_mu_kept.n && !kept_ok) db_free(&g_mu_kept);           /* a reciprocal of some other Q */
    if (kept_ok) {                                               /* the prewarm's mu, on device: use its top k+1 limbs */
        if (g_mu_kept.n == k + 1) { dbig sw = mu; mu = g_mu_kept; g_mu_kept = sw; }
        else db_shr_limbs(&mu, &g_mu_kept, g_mu_kept.n - (k + 1));
        db_free(&g_mu_kept);
    } else if (mu_opt && mu_opt->n >= k + 1 && mu_opt->l) { bigint mh; bi_init(&mh); bi_shr(&mh, mu_opt, 64 * (mu_opt->n - (k + 1))); db_from_bi(&mu, &mh); bi_free(&mh); }
    else recip_db(&mu, &Qd, Q, k);
    double tb = mem_now();
    /* A's top k limbs go to device for the gather (the host copy stays for the remainder window below).
     * X = ((A >> (nq-1)) mu) >> (k + 1) */
    const uint64_t *a = A->l;
    { dbig Ah; db_init(&Ah); bigint hv = { (uint64_t *)(A->l + (nq - 1)), na - (nq - 1), 0 };
      db_from_bi(&Ah, &hv);
      if (newton_db_free_inputs) { bigint *Am = (bigint *)A; uint64_t *s = (uint64_t *)realloc(Am->l, (nq + 2) * 8); if (s) { Am->l = s; Am->cap = nq + 2; } a = A->l; }   /* the host A shrinks to its window (its n stays: only limbs < nq + 2 are read below) */
      rns_mul_dist_db(&t, &Ah, &mu);
      db_free(&Ah); }
    db_shr_limbs(&Xd, &t, k + 1);
    db_free(&t);                                                 /* its blocks serve the low product below */
    double tc = mem_now();
    /* R = (A - low(X Q)) mod B^w over the window w = nq + 2, on the host (as the host path).  The low
     * product: the grid with the pieces above w skipped (NEWTON_LOWPROD=0: the full X Q truncated) */
    size_t w = nq + 2;
    if (newton_db_x_hook) { db_to_bi(X, &Xd); newton_db_x_hook(X, newton_db_x_arg); }   /* Phase 8: the CPU formats X while the low product runs */
    if (getenv("NEWTON_LOWPROD") && !atoi(getenv("NEWTON_LOWPROD"))) { rns_mul_dist_db(&xq, &Xd, &Qd); if (xq.n > w) { xq.n = w; db_norm(&xq); } }
    else rns_mul_low_db(&xq, &Xd, &Qd, w);
    double td = mem_now();
    bigint hxq; bi_init(&hxq); db_to_bi(&hxq, &xq);
    if (!newton_db_x_hook) db_to_bi(X, &Xd);
    double te = mem_now();
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
    if (!newton_db_Qd) db_free(&Qd);
    db_free(&Xd); db_free(&one);
    if (getenv("RNS_VERBOSE")) printf("divmod(db) %.2f s: Q in + mu %.2f, A mu + shift %.2f, low product %.2f, copies out %.2f, window + corrections %.2f; pools %.1f GB\n",
                                      mem_now() - t0, tb - ta, tc - tb, td - tc, te - td, mem_now() - te, db_pool_bytes() / 1e9);
    newton_st.t_div += mem_now() - t0;
}

/* Phase 8 I3 (decimal): X = floor(A / Q) with A = S B^dl entirely on the device (S = P + Q, dl = d/18 limbs):
 * the top of A is S shifted right by (nq - 1) - dl limbs (a view: dl < nq always, since 10^d < N!), the
 * remainder window A mod B^w (w = nq + 2) is S's low w - dl limbs shifted up by dl, and the corrections run
 * on device numbers.  X is copied out (through the hook, before the low product, when set); R stays on the
 * device: its residues mod the T1 primes come back in rres (nres of them), R itself is freed. */
void newton_db_divmod_shifted(bigint *X, const dbig *S, size_t dl, const dbig *Qd, const uint64_t *qs, int nres, uint64_t *rres)
{
    double t0 = mem_now();
    size_t nq = Qd->n, na = S->n + dl, k = na - nq + 1, w = nq + 2;
    if (!nq || na < nq) { fprintf(stderr, "newton_db_divmod_shifted: A < Q not supported here\n"); abort(); }
    if (dl + 1 > nq) { fprintf(stderr, "newton_db_divmod_shifted: dl >= nq\n"); abort(); }
    dbig mu = g_mu, t = g_t, xq = g_xq, Xd, Aw, Rd; db_init(&Xd); db_init(&Aw); db_init(&Rd);
    double ta = mem_now();
    int kept_ok = g_mu_kept.n >= k + 1 && g_mu_ql == Qd->q[0] && g_mu_qn == nq && g_mu_qtop == db_top(Qd);
    if (g_mu_kept.n && !kept_ok) db_free(&g_mu_kept);
    if (kept_ok) {
        if (g_mu_kept.n == k + 1) { dbig sw = mu; mu = g_mu_kept; g_mu_kept = sw; }
        else db_shr_limbs(&mu, &g_mu_kept, g_mu_kept.n - (k + 1));
        db_free(&g_mu_kept);
    } else recip_db(&mu, Qd, 0, k);
    double tb = mem_now();
    /* X = ((A >> (nq - 1)) mu) >> (k + 1);  A >> (nq - 1) = S >> (nq - 1 - dl) */
    { size_t sh = nq - 1 - dl; dbig Ah = db_view(S, sh, S->n > sh ? S->n - sh : 0); db_norm(&Ah);
      rns_mul_dist_db(&t, &Ah, &mu); }
    db_shr_limbs(&Xd, &t, k + 1);
    db_free(&t);
    double tc = mem_now();
    if (newton_db_x_hook) { db_to_bi(X, &Xd); newton_db_x_hook(X, newton_db_x_arg); }
    rns_mul_low_db(&xq, &Xd, Qd, w);                                  /* low_w(X Q) */
    double td = mem_now();
    /* the window: A mod B^w = (S mod B^(w - dl)) B^dl */
    db_set_shifted_low(&Aw, S, w - dl, dl, w);                        /* zeros with S's low w - dl limbs at dl: no 17 GB shift */
    size_t nc = 0; long dx = 0;                                       /* corrections to X: applied to the host copy at the end */
    if (db_cmp(&Aw, &xq) >= 0) {                                      /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        db_sub(&Rd, &Aw, &xq);
        while (db_cmp(&Rd, Qd) >= 0) { db_sub(&Rd, &Rd, Qd); dx++; if (++nc > 64) { fprintf(stderr, "newton_db_divmod_shifted: %zu corrections\n", nc); abort(); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        db_sub(&Rd, &xq, &Aw);
        for (;;) { dx--; if (++nc > 64) { fprintf(stderr, "newton_db_divmod_shifted: %zu corrections\n", nc); abort(); }
                   if (db_cmp(&Rd, Qd) <= 0) { db_sub(&Rd, Qd, &Rd); break; } db_sub(&Rd, &Rd, Qd); }
        newton_st.down_corr += (size_t)(-dx);
    }
    if (dx > 0) newton_st.up_corr += (size_t)dx;
    double te = mem_now();
    if (!newton_db_x_hook) db_to_bi(X, &Xd);
    if (dx) { bigint o; bi_init(&o); bi_set_u64(&o, (uint64_t)(dx < 0 ? -dx : dx)); if (dx < 0) bi_sub(X, X, &o); else bi_add(X, X, &o); bi_free(&o); }
    for (int i = 0; i < nres; i++) rres[i] = db_mod_q(&Rd, qs[i]);
    double tf = mem_now();
    g_mu = mu; g_t = t; g_xq = xq;
    db_free(&Xd); db_free(&Aw); db_free(&Rd);
    if (getenv("RNS_VERBOSE")) printf("divmod(dev) %.2f s: mu %.2f, A mu + shift %.2f, X out + low product %.2f, window + corrections %.2f (%ld), X out + R residues %.2f; pools %.1f GB\n",
                                      mem_now() - t0, tb - ta, tc - tb, td - tc, te - td, dx, tf - te, db_pool_bytes() / 1e9);
    newton_st.t_div += mem_now() - t0;
}

