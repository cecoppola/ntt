/* newton_db.c - the reciprocal and division of newton.c on device-resident
 * numbers (WP5 step 3): the same iteration, statistics and correction logic,
 * every big operation a dbig kernel and every product the distributed tier.
 * The host bigint interface is kept at the phase boundary: Q (and A) are
 * copied to device once, mu / X / R come back once. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "newton.h"
#include "dbig.h"
#include "rns_mul.h"
#include "mem.h"
static int nv = -1, anchor = 1, g_hold_q;                    /* g_hold_q (A1): the size-1 flow -- the reciprocal may keep Q's transforms for the division */
static dbig g_r, g_r2, g_qt, g_t1, g_t2, g_mu, g_t, g_xq;
void newton_db_free_scratch(void) { db_free(&g_r); db_free(&g_r2); db_free(&g_qt); db_free(&g_t1); db_free(&g_t2); db_free(&g_mu); db_free(&g_t); db_free(&g_xq); }
static int env_on(const char *name) { const char *e = getenv(name); return !e || atoi(e); }   /* a switch that is on unless set to 0 */

/* the seed is small: the host version, copied in */
static void seed_db(dbig *r, const bigint *Q, const dbig *Qd, size_t nq_seed, size_t *j)   /* nq_seed (M4): Qd is only the top of a Q of nq_seed limbs; 0 = Qd is Q */
{
    bigint hr; bi_init(&hr);
    if (Q && Q->l) newton_seed_host(&hr, Q, j);
    else { size_t nq = Qd->n, top = nq < 4 ? nq : 4; uint64_t t4[4]; for (size_t i = 0; i < top; i++) t4[i] = db_limb(Qd, nq - top + i); newton_seed_top(&hr, t4, top, nq_seed ? nq_seed : nq, j); }   /* I3: Q only on the device */
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
static void recip_db2(dbig *mu, const dbig *Qd, const bigint *Q, size_t k, size_t nq_seed);
static void recip_db(dbig *mu, const dbig *Qd, const bigint *Q, size_t k) { recip_db2(mu, Qd, Q, k, 0); }
static void recip_db2(dbig *mu, const dbig *Qd, const bigint *Q, size_t k, size_t nq_seed)
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
    seed_db(&r, Q, Qd, nq_seed, &j);
    while (j < k) {
        size_t jn = k;
        if (anchor) { while ((jn + 1) / 2 > j) jn = (jn + 1) / 2; }
        else jn = 2 * j < k ? 2 * j : k;
        for (;;) {
            size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
            dbig qt = db_view(Qd, nq - take, take);                  /* top limbs of Q */
            /* Phase 10 A1: at the top Q_t is Q itself, the operand of the division's X Q: its pieces' transforms are kept
             * (rns_dist_cache_hold: only when the cache has the slots for them) -- Q as the B operand, whose pieces the
             * grid keeps in distinct slots; the product is the same either way */
            if (g_hold_q && take == nq && rns_dist_cache_hold(1)) rns_mul_dist_db(&t1, &r, &qt);
            else rns_mul_dist_db(&t1, &qt, &r);                     /* Q_t r */
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
dbig *newton_db_x_dev = 0;   /* Phase 10 H (B1): when set, X stays on the device -- moved here before the low product (the x hook, if set, is then called with the
                              * empty host X and reads the device one from here), the +/-1 corrections applied to it in place at the end; the caller owns it.  No host X */
static void db_add_small(dbig *x, long dx)           /* x +/- |dx| in place (H, B1: the corrections on the device X) */
{
    int co = 0, pr = 0; db_share_add_val(x, x->n, 0, (uint64_t)(dx < 0 ? -dx : dx), dx < 0, &co, &pr);
    if (co) { fprintf(stderr, "newton_db: X %s out of its top limb\n", dx < 0 ? "borrows" : "carries"); abort(); }
    if (dx < 0) db_norm(x);
}
void newton_db_recip(bigint *mu, const bigint *Q, size_t k)
{
    dbig Qd; db_init(&Qd);
    if (newton_db_Qd) Qd = *newton_db_Qd; else db_from_bi(&Qd, Q);
    g_hold_q = newton_db_Qd != 0; recip_db(&g_mu_kept, &Qd, Q, k); g_hold_q = 0; g_mu_k = k;   /* (A1: the hold needs Q alive across both phases: the caller's device Q) */
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
    rns_dist_cache_hold(0); rns_dist_cache_release();                 /* A1: the low product was the last grid product; the planes go */
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
    /* X = ((A >> (nq - 1)) mu) >> (k + 1);  A >> (nq - 1) = S >> (nq - 1 - dl).  Every large temporary is freed at its
     * last use: the block pool (the donated bs regions and the plane tails) must hold the peak, or hipMalloc costs
     * 0.057 s/GB inside the phase (RESULTS.md 70) */
    { size_t sh = nq - 1 - dl; dbig Ah = db_view(S, sh, S->n > sh ? S->n - sh : 0); db_norm(&Ah);
      if (env_on("NEWTON_HIGHPROD")) rns_mul_high_db(&t, &Ah, &mu, k + 1); else rns_mul_dist_db(&t, &Ah, &mu); }   /* B3: the pieces below the cut skipped (rns_dist.c's grid, A5) */
    db_free(&mu);                                                     /* the reciprocal's last use */
    db_shr_limbs(&Xd, &t, k + 1);
    db_free(&t);
    double tc = mem_now();
    const dbig *Xp = &Xd;
    if (newton_db_x_dev) { *newton_db_x_dev = Xd; db_init(&Xd); Xp = newton_db_x_dev; if (newton_db_x_hook) newton_db_x_hook(X, newton_db_x_arg); }   /* H B1: X stays on the device; the hook starts the writer on it */
    else if (newton_db_x_hook) { db_to_bi(X, &Xd); newton_db_x_hook(X, newton_db_x_arg); }
    rns_mul_low_db(&xq, Xp, Qd, w);                                   /* low_w(X Q): Q's cached transforms hit when held (A1) */
    rns_dist_cache_hold(0); rns_dist_cache_release();                 /* A1: the low product was the last grid product; the planes go */
    if (newton_db_x_hook && !newton_db_x_dev) db_free(&Xd);           /* X is on the host; corrections go to the host copy */
    double td = mem_now();
    /* the window: A mod B^w = (S mod B^(w - dl)) B^dl; R formed in place in it */
    db_set_shifted_low(&Aw, S, w - dl, dl, w);
    size_t nc = 0; long dx = 0;                                       /* corrections to X: applied to the host copy at the end */
    if (db_cmp(&Aw, &xq) >= 0) {                                      /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        db_sub(&Aw, &Aw, &xq); db_free(&xq); Rd = Aw; db_init(&Aw);
        while (db_cmp(&Rd, Qd) >= 0) { db_sub(&Rd, &Rd, Qd); dx++; if (++nc > 64) { fprintf(stderr, "newton_db_divmod_shifted: %zu corrections\n", nc); abort(); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        db_sub(&xq, &xq, &Aw); db_free(&Aw); Rd = xq; db_init(&xq);
        for (;;) { dx--; if (++nc > 64) { fprintf(stderr, "newton_db_divmod_shifted: %zu corrections\n", nc); abort(); }
                   if (db_cmp(&Rd, Qd) <= 0) { db_sub(&Rd, Qd, &Rd); break; } db_sub(&Rd, &Rd, Qd); }
        newton_st.down_corr += (size_t)(-dx);
    }
    if (dx > 0) newton_st.up_corr += (size_t)dx;
    double te = mem_now();
    if (!newton_db_x_hook && !newton_db_x_dev) { db_to_bi(X, &Xd); db_free(&Xd); }
    if (dx && newton_db_x_dev) db_add_small(newton_db_x_dev, dx);     /* H B1: the correction on the device X (the writer redoes the digits) */
    else if (dx) { bigint o; bi_init(&o); bi_set_u64(&o, (uint64_t)(dx < 0 ? -dx : dx)); if (dx < 0) bi_sub(X, X, &o); else bi_add(X, X, &o); bi_free(&o); }
    db_mod_qs(&Rd, qs, nres, rres);
    double tf = mem_now();
    db_free(&Rd); db_free(&xq); db_free(&Aw);
    db_init(&g_mu); db_init(&g_t); db_init(&g_xq);
    size_t ch = 0, cm = 0; rns_dist_cache_stats(&ch, &cm);
    if (getenv("RNS_VERBOSE")) printf("divmod(dev) %.2f s: mu %.2f, A mu + shift %.2f, X out + low product %.2f, window + corrections %.2f (%ld), X out + R residues %.2f; pools %.1f GB; transform cache %zu hits / %zu misses\n",
                                      mem_now() - t0, tb - ta, tc - tb, td - tc, te - td, dx, tf - te, db_pool_bytes() / 1e9, ch, cm);
    newton_st.t_div += mem_now() - t0;
}


/* ==== Phase 9 M4 (A-div, PLAN.md 17/19): the reciprocal and the division over sharded numbers ====================
 * After the tree P and Q are mdb over the whole machine (the top-level group G).  Every big product is
 * rns_mul_dist_mn over G; everything else is one of a few share-level primitives:
 *   mdb_shift    Y = X >> s (s < 0: <<) in a chosen basis N2 -- one all-to-all per APU thread of the pieces
 *                (a piece = one source share x one target share; APU d carries its quarter of each piece),
 *                truncating to N2 limbs when asked (the window, low(X Q));
 *   mdb_addsub   Y = A +/- B in one basis: the fixed-length share add with the (carry, propagate) scan over
 *                the nodes (the pattern of the product's spill add), a second pass on the nodes receiving one;
 *   mdb_add_val  +/- a small value at one limb (the corrections of X);
 *   mdb_cmp, mdb_norm, mdb_limb, mdb_mod_qs   all-gathers / max-reductions of per-share values.
 * The reciprocal: the anchored doubling chain from k is followed on every node identically with the
 * single-node recip_db on the top limbs of Q up to the largest target <= NEWTON_MN_SPLIT limbs (the iterates
 * depend only on Q's top 2j + 2 limbs, so they are the single-node ones); each node then takes its share of
 * r, and the remaining steps run the correction-form step of recip_db over shares.  The division mirrors
 * newton_db_divmod_shifted: S = P + Q, X = ((S >> (nq - 1 - dl)) mu) >> (k + 1), the window and the +/-Q
 * corrections on shares, R's residues by the sharded kernel scaled by B^lo per node and reduced over G. */
#include <omp.h>
#include "mdb.h"
#include "mn.h"
#define MN_HIP(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
struct sacc { uint64_t *q[4]; size_t qc, off; };                     /* a dbig's limbs by index (any quarter: peer access) */
__device__ static inline uint64_t *sacc_p(const struct sacc a, size_t i) { size_t g = a.off + i, d = (g >= a.qc) + (g >= 2 * a.qc) + (g >= 3 * a.qc); return a.q[d] + (g - d * a.qc); }
static struct sacc sacc_of(const dbig *x) { struct sacc a; for (int d = 0; d < 4; d++) a.q[d] = x->q[d]; a.qc = x->qc; a.off = x->off; return a; }
struct piece { size_t a, len, off; };                                /* target indices [a, a + len) of one (source, target) piece's part on this APU, at limb off of the slab buffer (Phase 11 L, B7: exact slabs) */
__global__ void k_mn_pack(uint64_t *sb, struct sacc src, long off, const struct piece *pc, int g, size_t SL)   /* sb[pc[r'].off + k] = src[a + k + off] */
{
    size_t total = (size_t)g * SL, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t r = t / SL, k = t - r * SL; if (k < pc[r].len) sb[pc[r].off + k] = *sacc_p(src, (size_t)((long)(pc[r].a + k) + off)); }
}
__global__ void k_mn_scatter(struct sacc dst, size_t lo2, const uint64_t *rb, const struct piece *pc, int g, size_t SL)
{
    size_t total = (size_t)g * SL, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t r = t / SL, k = t - r * SL; if (k < pc[r].len) *sacc_p(dst, pc[r].a + k - lo2) = rb[pc[r].off + k]; }
}
static unsigned mn_nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 8 ? 228 * 8 : b); }
static hipStream_t g_ms[4]; static int g_ms_init;
static void ms_init(void) { if (g_ms_init) return; for (int d = 0; d < 4; d++) { MN_HIP(hipSetDevice(d)); MN_HIP(hipStreamCreateWithFlags(&g_ms[d], hipStreamNonBlocking)); } MN_HIP(hipSetDevice(0)); g_ms_init = 1; }
static struct { size_t n_shift, n_addsub, n_small; double t_shift, t_addsub, t_small, t_prod; } mn_st;
static void mfree(mdb *x) { if (x->sh.cap) db_free(&x->sh); memset(x, 0, sizeof *x); }
static size_t mshare_max(const mdb *x) { return x->g ? (x->N + x->g - 1) / x->g : 0; }
/* the piece (source node r, target node rt): target indices [a, b); APU d's quarter of it */
static int piece_of(const mdb *X, long s, const mdb *Y, size_t n2, int r, int rt, int d, struct piece *pc)
{
    size_t lo1, hi1, lo2, hi2; mdb_share(X, r, &lo1, &hi1); mdb_share(Y, rt, &lo2, &hi2);
    if (hi1 > X->n) hi1 = X->n; if (hi2 > n2) hi2 = n2;
    pc->a = pc->len = 0;
    if (hi1 <= lo1 || hi2 <= lo2) return 0;
    long a = (long)lo2, b = (long)hi2; if ((long)lo1 - s > a) a = (long)lo1 - s; if ((long)hi1 - s < b) b = (long)hi1 - s;
    if (b <= a) return 0;
    size_t len = (size_t)(b - a), a0 = (size_t)a + len * d / 4, a1 = (size_t)a + len * (d + 1) / 4;
    pc->a = a0; pc->len = a1 - a0; return 1;
}
/* Y = X >> s (s < 0: X << -s) in basis N2 over G; if N2 is below the shifted length the result is truncated to N2
 * limbs (mod B^N2) and re-normalised.  Y is a fresh number (its share zero-filled, fixed length) */
/* Phase 11 X1: the same with Y sharded over the group Gt (a subgroup of the mesh's group G, or a group containing X's:
 * the pieces of nodes outside Gt or outside X's group are empty; every node of G takes part in the exchange) */
static void mdb_shift_g(mdb *Y, const mdb *X, long s, size_t N2, mn_group *G, int tg0, int tg)
{
    double t0 = mem_now(); mn_st.n_shift++;
    ms_init();
    int g = G->g, me = G->me, node = G->g0 + me;
    size_t n2 = s >= 0 ? (X->n > (size_t)s ? X->n - (size_t)s : 0) : X->n + (size_t)(-s);
    int trunc = n2 > N2; if (trunc) n2 = N2;
    mdb Yn; memset(&Yn, 0, sizeof Yn); Yn.N = N2; Yn.g0 = tg0; Yn.g = tg; Yn.n = n2; db_init(&Yn.sh);
    size_t lo2, hi2; mdb_share(&Yn, node, &lo2, &hi2); size_t cn = hi2 - lo2;
    db_zero_fill(&Yn.sh, cn);
    size_t lo1, hi1; mdb_share(X, node, &lo1, &hi1);
    /* Phase 11 L (agent L, B7): the slabs at their exact lengths (an alltoallv; the buffers hold my pieces' limbs, at most my
     * share / 4 per APU each, instead of g x the longest piece).  SL (the longest piece part) bounds the kernels' iteration only */
    size_t pm = mshare_max(X) < mshare_max(&Yn) ? mshare_max(X) : mshare_max(&Yn), SL = (pm + 3) / 4 + 1; SL = (SL + 15) / 16 * 16;
    if (X->n && n2) {
#pragma omp parallel num_threads(4)
    {
        int d = omp_get_thread_num(); hipStream_t st = g_ms[d];
        MN_HIP(hipSetDevice(d));
        struct piece *hp = (struct piece *)malloc(2 * g * sizeof *hp), *hq = hp + g, *dp = (struct piece *)db_pool_alloc(d, g * sizeof *hp + 64);
        size_t *cnt = (size_t *)malloc(4 * (size_t)g * sizeof *cnt), *scnt = cnt, *sdsp = cnt + g, *rcnt = cnt + 2 * g, *rdsp = cnt + 3 * g, ts = 0, tr = 0;
        int any = 0, anyr = 0;
        for (int r = 0; r < g; r++) { any |= piece_of(X, s, &Yn, n2, node, G->g0 + r, d, &hp[r]); hp[r].off = ts; scnt[r] = hp[r].len * 8; sdsp[r] = ts * 8; ts += hp[r].len; }
        for (int r = 0; r < g; r++) { anyr |= piece_of(X, s, &Yn, n2, G->g0 + r, node, d, &hq[r]); hq[r].off = tr; rcnt[r] = hq[r].len * 8; rdsp[r] = tr * 8; tr += hq[r].len; }
        uint64_t *sb = db_pool_alloc(d, (ts + 16) * 8), *rb = db_pool_alloc(d, (tr + 16) * 8);
        if (any) {
            MN_HIP(hipMemcpyAsync(dp, hp, g * sizeof *hp, hipMemcpyHostToDevice, st));
            k_mn_pack<<<mn_nblk((size_t)g * SL), 256, 0, st>>>(sb, sacc_of(&X->sh), s - (long)lo1, dp, g, SL);
        }
        MN_HIP(hipStreamSynchronize(st));
        comm_alltoallv(G->all[d], sb, scnt, sdsp, rb, rcnt, rdsp, st); comm_wait(G->all[d]);
        if (anyr && cn) {
            MN_HIP(hipMemcpyAsync(dp, hq, g * sizeof *hq, hipMemcpyHostToDevice, st));
            k_mn_scatter<<<mn_nblk((size_t)g * SL), 256, 0, st>>>(sacc_of(&Yn.sh), lo2, rb, dp, g, SL);
        }
        MN_HIP(hipStreamSynchronize(st));
        db_pool_free(d, sb); db_pool_free(d, rb); db_pool_free(d, (uint64_t *)dp); free(hp); free(cnt);
    }
    MN_HIP(hipSetDevice(0));
    }
    Yn.sh.n = cn;
    if (trunc) { size_t top = 0; if (cn) { dbig t = Yn.sh; db_norm(&t); top = t.n ? lo2 + t.n : 0; } Yn.n = comm_allreduce_max(G->all[0], top); }
    if (Y->sh.cap) db_free(&Y->sh);
    *Y = Yn;
    mn_st.t_shift += mem_now() - t0;
}
static void mdb_shift(mdb *Y, const mdb *X, long s, size_t N2, mn_group *G) { mdb_shift_g(Y, X, s, N2, G, G->g0, G->g); }
/* the node-level carry scan: my (carry-out, propagate) with everyone's -> my carry-in; the top node's carry-out in *top_out */
static int node_scan(mn_group *G, int c, int p, int *top_out)
{
    int g = G->g, me = G->me, cin = 0; uint64_t v = (uint64_t)(c | (p << 1)), *all = (uint64_t *)malloc((size_t)g * 8);
    mn_allgather(G->all[0], &v, 1, all);
    for (int r = 0; r < me; r++) cin = (int)(all[r] & 1) | ((int)((all[r] >> 1) & 1) & cin);
    int cc = 0; for (int r = 0; r < g; r++) cc = (int)(all[r] & 1) | ((int)((all[r] >> 1) & 1) & cc);
    if (top_out) *top_out = cc;
    free(all); return cin;
}
static void mdb_norm(mdb *Y, mn_group *G)
{
    size_t lo, hi; mdb_share(Y, G->g0 + G->me, &lo, &hi); size_t cn = hi - lo, top = 0;
    if (cn) { dbig t = Y->sh; db_norm(&t); top = t.n ? lo + t.n : 0; Y->sh.n = cn; }
    Y->n = comm_allreduce_max(G->all[0], top);
}
/* Y = A +/- B in one basis (A->N == B->N; Y may be A or B, else a fresh number); sub needs A >= B */
static void mdb_addsub(mdb *Y, const mdb *A, const mdb *B, int sub, mn_group *G)
{
    double t0 = mem_now(); mn_st.n_addsub++;
    if (A->N != B->N || A->g0 != B->g0 || A->g != B->g) { fprintf(stderr, "mdb_addsub: bases %zu / %zu\n", A->N, B->N); exit(1); }
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(A, node, &lo, &hi); size_t cn = hi - lo;
    mdb Yn; if (Y == A || Y == B) Yn = *Y; else { memset(&Yn, 0, sizeof Yn); Yn.N = A->N; Yn.g0 = A->g0; Yn.g = A->g; db_init(&Yn.sh); db_zero_fill(&Yn.sh, cn); }
    int co = 0, pr = 1, top = 0;
    if (cn) db_share_addsub(&Yn.sh, &A->sh, &B->sh, cn, sub, &co, &pr);
    int cin = node_scan(G, co, pr, &top);
    if (top) { fprintf(stderr, "mdb_addsub: %s out of the top (basis %zu)\n", sub ? "borrow" : "carry", A->N); exit(1); }
    if (cin) { int c2 = 0; if (!cn) { fprintf(stderr, "mdb_addsub: a carry into an empty share\n"); exit(1); } db_share_add_val(&Yn.sh, cn, 0, 1, sub, &c2, 0); }
    Yn.sh.n = cn;
    if (Y != A && Y != B && Y->sh.cap) db_free(&Y->sh);
    *Y = Yn; mdb_norm(Y, G);
    mn_st.t_addsub += mem_now() - t0;
}
/* Y +/- val at limb pos (val < B), in place */
static void mdb_add_val(mdb *Y, size_t pos, uint64_t val, int sub, mn_group *G)
{
    double t0 = mem_now(); mn_st.n_small++;
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(Y, node, &lo, &hi); size_t cn = hi - lo;
    int co = 0, pr = 1, top = 0, mine = pos >= lo && pos < hi;
    if (cn) db_share_add_val(&Y->sh, cn, mine ? pos - lo : 0, mine ? val : 0, sub, &co, &pr);
    int cin = node_scan(G, co, pr, &top);
    if (top) { fprintf(stderr, "mdb_add_val: %s out of the top\n", sub ? "borrow" : "carry"); exit(1); }
    if (cin) { int c2 = 0; db_share_add_val(&Y->sh, cn, 0, 1, sub, &c2, 0); }
    Y->sh.n = cn; mdb_norm(Y, G);
    mn_st.t_small += mem_now() - t0;
}
/* Y = B^e in basis N2 */
static void mdb_pow(mdb *Y, size_t e, size_t N2, mn_group *G)
{
    mdb Yn; memset(&Yn, 0, sizeof Yn); Yn.N = N2; Yn.g0 = G->g0; Yn.g = G->g; Yn.n = e + 1; db_init(&Yn.sh);
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(&Yn, node, &lo, &hi); size_t cn = hi - lo;
    if (e >= N2) { fprintf(stderr, "mdb_pow: B^%zu in basis %zu\n", e, N2); exit(1); }
    db_zero_fill(&Yn.sh, cn);
    if (e >= lo && e < hi) { uint64_t one = 1; size_t gg = e - lo, d = (gg >= Yn.sh.qc) + (gg >= 2 * Yn.sh.qc) + (gg >= 3 * Yn.sh.qc); mem_dev_copy_on((int)d, Yn.sh.q[d] + (gg - d * Yn.sh.qc), &one, 8); }
    Yn.sh.n = cn;
    if (Y->sh.cap) db_free(&Y->sh);
    *Y = Yn;
}
/* sign of A - B (one basis): the highest node whose shares differ decides */
static int mdb_cmp(const mdb *A, const mdb *B, mn_group *G)
{
    if (A->N != B->N) { fprintf(stderr, "mdb_cmp: bases %zu / %zu\n", A->N, B->N); exit(1); }
    int g = G->g, node = G->g0 + G->me; size_t lo, hi; mdb_share(A, node, &lo, &hi); size_t cn = hi - lo;
    int c = 0; if (cn) { dbig a = A->sh, b = B->sh; a.n = b.n = cn; c = db_cmp(&a, &b); }
    uint64_t v = (uint64_t)(c + 1), *all = (uint64_t *)malloc((size_t)g * 8);
    mn_allgather(G->all[0], &v, 1, all);
    int res = 0; for (int r = g; r-- > 0;) if (all[r] != 1) { res = (int)all[r] - 1; break; }
    free(all); return res;
}
/* limb i of X (every node gets it) */
static uint64_t mdb_limb(const mdb *X, size_t i, mn_group *G)
{
    int g = G->g, node = G->g0 + G->me; size_t lo, hi; mdb_share(X, node, &lo, &hi);
    uint64_t v = (i >= lo && i < hi) ? db_limb(&X->sh, i - lo) : 0, *all = (uint64_t *)malloc((size_t)g * 8);
    mn_allgather(G->all[0], &v, 1, all);
    uint64_t r = 0; for (int k = 0; k < g; k++) r |= all[k];
    free(all); return r;
}
/* is any limb of X below index e nonzero? */
static int mdb_nonzero_below(const mdb *X, size_t e, mn_group *G)
{
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(X, node, &lo, &hi); if (hi > e) hi = e; if (hi > X->n) hi = X->n;
    size_t nz = 0; if (hi > lo) { dbig v = db_view(&X->sh, 0, hi - lo); db_norm(&v); nz = v.n != 0; }
    return comm_allreduce_max(G->all[0], nz) != 0;
}
/* residues of X mod nq primes: each share's residue scaled by B^lo mod q, summed over the group */
static uint64_t mn_powmod(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1; b %= q; while (e) { if (e & 1) r = (uint64_t)((unsigned __int128)r * b % q); b = (uint64_t)((unsigned __int128)b * b % q); e >>= 1; } return r; }
static void mdb_mod_qs(const mdb *X, const uint64_t *qs, int nq, uint64_t *res, mn_group *G)
{
    int g = G->g, node = G->g0 + G->me; size_t lo, hi; mdb_share(X, node, &lo, &hi); if (hi > X->n) hi = X->n;
    uint64_t v[16]; memset(v, 0, sizeof v);
    if (hi > lo) { dbig s = X->sh; s.n = hi - lo; db_mod_qs(&s, qs, nq, v);
                   for (int j = 0; j < nq; j++) { uint64_t q = qs[j], Bq = bi_decimal ? BI_B10 % q : (uint64_t)(((unsigned __int128)1 << 64) % q); v[j] = (uint64_t)((unsigned __int128)v[j] * mn_powmod(Bq, lo, q) % q); } }
    uint64_t *all = (uint64_t *)malloc((size_t)g * nq * 8);
    mn_allgather(G->all[0], v, nq, all);
    for (int j = 0; j < nq; j++) { uint64_t r = 0; for (int k = 0; k < g; k++) { r += all[(size_t)k * nq + j]; if (r >= qs[j]) r -= qs[j]; } res[j] = r; }
    if (db_res_log_on()) {                                            /* Phase 11 V (D5): the share's scaled residues, every node's as gathered, and the sum */
        printf("RES node %d mdb_mod_qs n=%zu N=%zu share [%zu, %zu) sh.n=%zu: mine", node, X->n, X->N, lo, hi, X->sh.n); for (int j = 0; j < nq; j++) printf(" %llu", (unsigned long long)v[j]);
        for (int k = 0; k < g; k++) { printf(" | node %d", G->g0 + k); for (int j = 0; j < nq; j++) printf(" %llu", (unsigned long long)all[(size_t)k * nq + j]); }
        printf(" | sum"); for (int j = 0; j < nq; j++) printf(" %llu", (unsigned long long)res[j]); printf("\n");
    }
    free(all);
}
/* the whole number on every node's host (small numbers: Q's top for the seed phase; the rare shrink) */
static void mdb_to_host_all(bigint *out, const mdb *X, mn_group *G)
{
    int g = G->g, node = G->g0 + G->me; size_t lo, hi; mdb_share(X, node, &lo, &hi); size_t ms = mshare_max(X); if (!ms) ms = 1;
    if (ms > 0x7fffffff) { fprintf(stderr, "mdb_to_host_all: share too large\n"); exit(1); }
    uint64_t *buf = (uint64_t *)calloc(ms, 8), *all = (uint64_t *)malloc((size_t)g * ms * 8);
    if (hi > lo) { bigint h; bi_init(&h); dbig v = X->sh; v.n = hi - lo; db_to_bi(&h, &v); memcpy(buf, h.l, (hi - lo) * 8); bi_free(&h); }
    {   /* an all-gather over mesh 0 (device blocks; Phase 11 L (agent L, B7): one copy of the block instead of g) */
        MN_HIP(hipSetDevice(0)); uint64_t *sb = db_pool_alloc(0, ms * 8), *rb = db_pool_alloc(0, (size_t)g * ms * 8);
        MN_HIP(hipMemcpy(sb, buf, ms * 8, hipMemcpyHostToDevice));
        comm_allgather(G->all[0], sb, rb, ms * 8);
        MN_HIP(hipMemcpy(all, rb, (size_t)g * ms * 8, hipMemcpyDeviceToHost));
        db_pool_free(0, sb); db_pool_free(0, rb);
    }
    bi_reserve(out, X->N + 1);
    for (int r = 0; r < g; r++) { mdb_share(X, G->g0 + r, &lo, &hi); if (hi > lo) memcpy(out->l + lo, all + (size_t)r * ms, (hi - lo) * 8); }
    out->n = X->n; free(buf); free(all);
}
/* a number every node holds (device) -> its shares in basis N2 */
static void mdb_from_db(mdb *Y, const dbig *r, size_t N2, mn_group *G)
{
    mdb Yn; memset(&Yn, 0, sizeof Yn); Yn.N = N2; Yn.g0 = G->g0; Yn.g = G->g; Yn.n = r->n; db_init(&Yn.sh);
    if (r->n > N2) { fprintf(stderr, "mdb_from_db: %zu limbs in basis %zu\n", r->n, N2); exit(1); }
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(&Yn, node, &lo, &hi); size_t cn = hi - lo;
    db_zero_fill(&Yn.sh, cn);
    if (cn && lo < r->n) { size_t h = hi < r->n ? hi : r->n; dbig v = db_view(r, lo, h - lo); db_copy(&Yn.sh, &v); }
    Yn.sh.n = cn;
    if (Y->sh.cap) db_free(&Y->sh);
    *Y = Yn;
}
static void mdb_from_bi(mdb *Y, const bigint *h, size_t N2, mn_group *G) { dbig t; db_init(&t); db_from_bi(&t, h); mdb_from_db(Y, &t, N2, G); db_free(&t); }
static void mn_prod(mdb *C, const mdb *A, const mdb *B, mn_group *G) { double t0 = mem_now(); rns_mul_dist_mn(C, A, B, 0, G); mn_st.t_prod += mem_now() - t0; }
/* Phase 10 A5: the division's two products with the grid's cuts (rns_mul_dist_mn_cut): the A_h mu product without the pieces
 * below k + 1 (B3 over shares; NEWTON_HIGHPROD=0 keeps them), the low product X Q mod B^w without the pieces above w and
 * delivered in basis w (NEWTON_LOWPROD=0: the full product re-sharded into basis w) */
static void mn_prod_cut(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t lowcut, size_t highcut) { double t0 = mem_now(); rns_mul_dist_mn_cut(C, A, B, G, lowcut, highcut); mn_st.t_prod += mem_now() - t0; }

/* Phase 11 X1 (agent X; results/X.md, mn_model.py): the group a product runs on.  A product of a few million limbs over
 * hundreds of nodes is latency-bound (15 all-to-alls of g - 1 messages per APU, a plane of at least 2^(2 (7 + log2 g))
 * points), so the reciprocal's early doublings run on the smallest subgroup [0, 2^L) of the full group (mn_group_at(L):
 * the tree's groups, created by mn_tree on every node) whose modelled cost is least; the operands are re-sharded onto it
 * by mdb_shift_g.  The cost per candidate group g' (the model of mn_model.py in three constants): the pieces at the group's
 * plane cap, each T_PIECE x (points per APU / 2^29) of local work times the APU sharing, plus 15 exchanges of
 * 8 q (g' - 1) / g' bytes at NEWTON_MN_BW GB/s per APU with K (g' - 1) messages of NEWTON_MN_LAT seconds and NEWTON_MN_FIXED
 * per exchange (defaults: the target fabric, 100 GB/s, 2 us, 0; MN_MODEL_TCP=1 sets aac6's loopback numbers).
 * NEWTON_MN_GROUPS=0 keeps every product on the full group.  The digits do not depend on the choice (every operation
 * is exact); size 1 never comes here. */
static double x1_cost(size_t na, size_t nb, int gp, size_t reshard_limbs, double bw, double lat, double fixed, double share)
{
    int lgt = 0; while ((1 << lgt) < gp) lgt++;
    size_t nc = na + nb, cap = (size_t)1 << (31 + lgt), logmin = 2 * (7 + lgt) < 20 ? 20 : 2 * (7 + lgt);
    double pieces = 1; size_t pts;
    if (nc > cap) { double ka = ceil((double)na / (cap / 2)), kb = ceil((double)nb / (cap / 2)); pieces = ka * kb; pts = cap; }
    else { pts = (size_t)1 << 20; while (pts < nc) pts <<= 1; if (pts < ((size_t)1 << logmin)) pts = (size_t)1 << logmin; }
    double q = (double)pts / (4.0 * gp), t_loc = 1.11 * q / (double)(1 << 29) * share + 0.005;
    double t_x = gp > 1 ? 15 * (8 * q * (gp - 1) / gp / (bw * 1e9) + 4 * (gp - 1) * lat + 4 * fixed) : 0;
    double t_re = gp > 1 && reshard_limbs ? 3 * (8.0 * reshard_limbs / (4.0 * gp) / (bw * 1e9) + (gp - 1) * lat + fixed) : 0;
    return pieces * (t_loc + t_x) + t_re;
}
/* the level whose group [0, 2^L) the product should run on (0: the full group G); every node computes the same answer */
static int x1_level(size_t na, size_t nb, mn_group *G, size_t reshard_limbs)
{
    static int on = -1; static double bw, lat, fixed, share;
    if (on < 0) {
        on = getenv("NEWTON_MN_GROUPS") ? atoi(getenv("NEWTON_MN_GROUPS")) : 1;
        int tcp = getenv("MN_MODEL_TCP") ? atoi(getenv("MN_MODEL_TCP")) : 0;
        bw = tcp ? 0.8 : 100.0; lat = tcp ? 0 : 2e-6; fixed = tcp ? 1e-3 : 0; share = tcp ? (double)mn_size() : 1.0;
        if (getenv("NEWTON_MN_BW")) bw = atof(getenv("NEWTON_MN_BW"));
        if (getenv("NEWTON_MN_LAT")) lat = atof(getenv("NEWTON_MN_LAT"));
        if (getenv("NEWTON_MN_FIXED")) fixed = atof(getenv("NEWTON_MN_FIXED"));
        if (getenv("NEWTON_MN_SHARE")) share = atof(getenv("NEWTON_MN_SHARE"));
    }
    if (!on || G->g0 != 0 || G->g <= 2) return 0;
    int best = 0; double bc = x1_cost(na, nb, G->g, reshard_limbs, bw, lat, fixed, share);
    for (int L = 1; (1 << L) < G->g; L++) { double c = x1_cost(na, nb, 1 << L, reshard_limbs, bw, lat, fixed, share); if (c < bc) { bc = c; best = L; } }
    return best;
}
static mn_group *x1_group(int level, mn_group *G) { return level ? mn_group_at(level) : G; }     /* (mn_group_at: this node's group at the level -- node 0's is [0, 2^L)) */
static int x1_member(const mn_group *Gs) { return Gs->g0 == 0; }                                 /* this node is in [0, 2^L) */
/* r (over a subgroup of `to`) re-sharded over `to`: the descriptor from node 0, then one exchange over `to` */
static void x1_regroup(mdb *r, mn_group *to, int was_member)
{
    uint64_t v[4] = { was_member ? r->n : 0, was_member ? r->N : 0, was_member ? (uint64_t)r->g0 : 0, was_member ? (uint64_t)r->g : 0 }, *all = (uint64_t *)malloc((size_t)to->g * 4 * 8);
    mn_allgather(to->all[0], v, 4, all);
    if (!was_member) { r->n = all[0]; r->N = all[1]; r->g0 = (int)all[2]; r->g = (int)all[3]; if (r->sh.cap) db_free(&r->sh); memset(&r->sh, 0, sizeof r->sh); }
    free(all);
    mdb rn; memset(&rn, 0, sizeof rn); mdb_shift_g(&rn, r, 0, r->N, to, to->g0, to->g); mfree(r); *r = rn;
}
/* X1 for the division's two products: when the rule (with the cost of re-sharding A, B and C counted) prefers a
 * subgroup, the operands go onto it, the product runs there, the result comes back onto G (never on the target's
 * sizes -- both products are >= 2 n_Q points -- but the rule decides, e.g. small digit counts on many nodes) */
static void mn_prod_cut_x1(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t lowcut, size_t highcut)
{
    int L = x1_level(A->n, B->n, G, 2 * (A->n + B->n));
    if (!L) { mn_prod_cut(C, A, B, G, lowcut, highcut); return; }
    mn_group *Gs = mn_group_at(L); int member = x1_member(Gs);
    if (getenv("NEWTON_VERBOSE") && G->me == 0) printf("divmod(mn): the product %zu x %zu limbs on the group [0, %d)\n", A->n, B->n, Gs->g);
    mdb As, Bs, Cs; memset(&As, 0, sizeof As); memset(&Bs, 0, sizeof Bs); memset(&Cs, 0, sizeof Cs);
    mdb_shift_g(&As, A, 0, A->N, G, 0, 1 << L); mdb_shift_g(&Bs, B, 0, B->N, G, 0, 1 << L);   /* the target [0, 2^L) on every node (a non-member's Gs is its own group) */
    if (member) mn_prod_cut(&Cs, &As, &Bs, Gs, lowcut, highcut);
    mfree(&As); mfree(&Bs);
    x1_regroup(&Cs, G, member);
    if (C->sh.cap) db_free(&C->sh);
    *C = Cs;
}
/* the reciprocal mu of Q (k + 1 limbs) over G: the single-node chain up to the split precision, then the sharded steps */
static void recip_mn(mdb *mu, const mdb *Q, size_t k, mn_group *G)
{
    double t0 = mem_now();
    if (nv < 0) { nv = getenv("NEWTON_VERBOSE") ? atoi(getenv("NEWTON_VERBOSE")) : 0; if (getenv("NEWTON_ANCHOR")) anchor = atoi(getenv("NEWTON_ANCHOR")); }
    size_t nq = Q->n, split = getenv("NEWTON_MN_SPLIT") ? strtoull(getenv("NEWTON_MN_SPLIT"), 0, 10) : ((size_t)1 << 16);
    int me = G->me, verbose = getenv("RNS_VERBOSE") != 0;
    /* the anchored chain from k: k, ceil(k/2), ...; the single-node part ends at the largest target <= split (or the seed's 2) */
    size_t kp = k;
    if (anchor) { while (kp > split && (kp + 1) / 2 > 2) kp = (kp + 1) / 2; if (kp > split) kp = 2; }
    else { kp = 2; while (2 * kp <= split && 2 * kp < k) kp *= 2; if (kp > k) kp = k; }
    size_t T = 2 * kp + 2 < nq ? 2 * kp + 2 : nq;                     /* the top limbs of Q the single-node part reads */
    mdb Qt; memset(&Qt, 0, sizeof Qt); mdb_shift(&Qt, Q, (long)(nq - T), T, G);
    bigint hq; bi_init(&hq); mdb_to_host_all(&hq, &Qt, G); mfree(&Qt);
    dbig Qtop, r0; db_init(&Qtop); db_init(&r0); db_from_bi(&Qtop, &hq); bi_free(&hq);
    recip_db2(&r0, &Qtop, 0, kp, nq);                                  /* on every node: r ~ B^(nq + kp) / Q, kp + 1 limbs */
    db_free(&Qtop);
    double t1 = mem_now();
    size_t j = kp;
    /* X1: the step's group.  r lives on the group of the current step (Gs, a prefix [0, 2^L) of G or G itself); Q stays
     * on G, so Q_t is cut out of Q by an exchange over G into Gs (every node takes part), the step itself runs on Gs's
     * members only (the others wait at the next exchange over G), and r moves to the next step's group when it grows */
    mn_group *Gs = 0; int member = 0;
    mdb r; memset(&r, 0, sizeof r);
    mdb qt, t, u, pw, d, corr, rs; memset(&qt, 0, sizeof qt); memset(&t, 0, sizeof t); memset(&u, 0, sizeof u); memset(&pw, 0, sizeof pw); memset(&d, 0, sizeof d); memset(&corr, 0, sizeof corr); memset(&rs, 0, sizeof rs);
    if (me == 0) printf("recip(mn): the single-node chain to %zu limbs (%.2f s), then the sharded steps to %zu over %d nodes\n", kp, t1 - t0, k, G->g);
    while (j < k) {
        size_t jn = k;
        if (anchor) { while ((jn + 1) / 2 > j) jn = (jn + 1) / 2; }
        else jn = 2 * j < k ? 2 * j : k;
        size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
        int Ln = x1_level(take, j + 1, G, 0); mn_group *Gn = x1_group(Ln, G);
        if (Gn != Gs) {                                                /* the group grows (never shrinks: the products only get longer) */
            int was = member; member = x1_member(Gn);
            if (!Gs) { r.n = r.N = r0.n; r.g0 = Gn->g0; r.g = Gn->g; if (member) mdb_from_db(&r, &r0, r0.n, Gn); }   /* every node has r0: the members take their share */
            else if (member) x1_regroup(&r, Gn, was);
            Gs = Gn;
            if (nv && me == 0 && Gs != G) printf("newton(mn): j %zu on the group [0, %d)\n", j, Gs->g);
        }
        if (take == nq && Gs == G) ;                                   /* Q_t = Q itself (no shift copy) */
        else mdb_shift_g(&qt, Q, (long)(nq - take), take, G, 0, Ln ? 1 << Ln : G->g);   /* Q_t: the top limbs of Q, onto the step's group [0, 2^L) (once per step: a repeat reuses it) */
        if (!member) { j = jn; newton_st.iters++; continue; }
        for (;;) {
            double s0 = mem_now();
            if (take == nq && Gs == G) { if (rns_dist_cache_hold(1)) mn_prod(&t, &r, Q, Gs); else mn_prod(&t, Q, &r, Gs); }   /* A1: Q's pieces' transforms may be kept for the division's X Q */
            else mn_prod(&t, &qt, &r, Gs);                             /* Q_t r */
            mdb_shift(&u, &t, (long)take - (long)j, 2 * j + 2, Gs);    /* u ~ B^(2j) */
            int neg = u.n > 2 * j + 1 || (u.n == 2 * j + 1 && (mdb_limb(&u, u.n - 1, Gs) > 1 || mdb_nonzero_below(&u, 2 * j, Gs)));
            mdb_pow(&pw, 2 * j, 2 * j + 2, Gs);
            if (neg) mdb_addsub(&d, &u, &pw, 1, Gs); else mdb_addsub(&d, &pw, &u, 1, Gs);   /* d = |B^(2j) - u| */
            mfree(&u); mfree(&pw);
            size_t NB = (r.n + j > t.n ? r.n + j : t.n) + 2;          /* the basis of r' (>= r << j and corr) */
            if (d.n) { mn_prod(&t, &r, &d, Gs); mdb_shift(&corr, &t, (long)j, NB, Gs); }   /* |corr| = r |d| >> j */
            else { mfree(&t); mdb_shift(&corr, &r, (long)r.n + 1, NB, Gs); }              /* d = 0: corr = 0 */
            mfree(&d);
            int converged = corr.n <= j + 1;
            mdb_shift(&rs, &r, -(long)j, NB, Gs);                       /* r << j */
            if (neg) {
                int over = rs.n < corr.n || (rs.n == corr.n && mdb_cmp(&rs, &corr, Gs) <= 0);
                if (over) {                                             /* overshoot: r -= r / 16, through the host (rare) */
                    newton_st.overshoots++; bigint h, dd; bi_init(&h); bi_init(&dd); mdb_to_host_all(&h, &r, Gs);
                    bi_divmod_u64(&dd, &h, 16); bi_sub(&h, &h, &dd); mdb_from_bi(&r, &h, r.N, Gs); bi_free(&h); bi_free(&dd);
                    mfree(&corr); mfree(&rs); continue;
                }
                mdb_addsub(&rs, &rs, &corr, 1, Gs);
            } else mdb_addsub(&rs, &rs, &corr, 0, Gs);
            mfree(&corr);
            if (converged) { mfree(&r); r = rs; memset(&rs, 0, sizeof rs); }
            else { mdb_shift(&r, &rs, (long)j, rs.n > j ? rs.n - j : 1, Gs); mfree(&rs); }
            if (nv && me == 0) printf("newton(mn) j %zu -> %zu (k %zu): take %zu, r %zu limbs%s   %.2f s%s\n", j, jn, k, take, r.n, converged ? "" : " (repeat)", mem_now() - s0, Gs != G ? " (subgroup)" : "");
            if (!converged) { newton_st.repeats++; continue; }
            break;
        }
        if (jn < 2 * j) { mdb_shift(&rs, &r, (long)(2 * j - jn), r.n - (2 * j - jn), Gs); mfree(&r); r = rs; memset(&rs, 0, sizeof rs); }
        j = jn;
        newton_st.iters++;
    }
    if (!Gs) { Gs = G; member = 1; mdb_from_db(&r, &r0, r0.n, G); }   /* (no sharded step: kp == k) */
    db_free(&r0);
    if (Gs != G) { x1_regroup(&r, G, member); Gs = G; member = 1; }    /* mu over the full group */
    if (j > k) { mdb_shift(&rs, &r, (long)(j - k), r.n - (j - k), G); mfree(&r); r = rs; memset(&rs, 0, sizeof rs); }
    mfree(&qt); mfree(&t);
    if (mu->sh.cap) db_free(&mu->sh);
    *mu = r;
    newton_st.t_recip += mem_now() - t0;
    if (verbose && me == 0) printf("recip(mn) %.2f s: single-node part %.2f, products %.2f, shifts %zu/%.2f, addsub %zu/%.2f, small %zu/%.2f\n", mem_now() - t0, t1 - t0, mn_st.t_prod, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub, mn_st.n_small, mn_st.t_small);
}
/* X = floor((P + Q) B^dl / Q) over G (P, Q consumed); pres/qres/rres: residues mod qs[nres] of P, Q and R = A - X Q;
 * t_recip: the reciprocal's seconds.  Mirrors newton_db_divmod_shifted with S = P + Q. */
void newton_mn_divmod(mdb *X, mdb *P, mdb *Q, size_t dl, struct mn_group *G, const uint64_t *qs, int nres, uint64_t *pres, uint64_t *qres, uint64_t *rres, double *t_recip)
{
    double t0 = mem_now(); int me = G->me;
    memset(&mn_st, 0, sizeof mn_st);
    size_t nq = Q->n;
    mdb_mod_qs(P, qs, nres, pres, G); mdb_mod_qs(Q, qs, nres, qres, G);
    /* the reciprocal first (the memory peak, as on one node: S does not exist yet); k from S's largest possible length */
    size_t na_est = P->n + 1 + dl, k_mu = na_est - nq + 1;
    mdb mu; memset(&mu, 0, sizeof mu);
    recip_mn(&mu, Q, k_mu, G);
    double ta = mem_now(); *t_recip = ta - t0;
    double p_rec = mn_st.t_prod;
    /* S = P + Q in P's basis (P.N = na + nb + 1 > P.n, so the sum fits) */
    mdb Qp, S; memset(&Qp, 0, sizeof Qp); memset(&S, 0, sizeof S);
    mdb_shift(&Qp, Q, 0, P->N, G);
    mdb_addsub(P, P, &Qp, 0, G); mfree(&Qp); S = *P; memset(P, 0, sizeof *P);
    size_t na = S.n + dl, k = na - nq + 1, w = nq + 2;
    if (!nq || na < nq) { fprintf(stderr, "newton_mn_divmod: A < Q not supported\n"); exit(1); }
    if (dl + 1 > nq) { fprintf(stderr, "newton_mn_divmod: dl >= nq\n"); exit(1); }
    if (mu.n < k + 1) { fprintf(stderr, "newton_mn_divmod: mu has %zu limbs, k %zu\n", mu.n, k); exit(1); }
    if (mu.n > k + 1) { mdb m2; memset(&m2, 0, sizeof m2); mdb_shift(&m2, &mu, (long)(mu.n - (k + 1)), k + 1, G); mfree(&mu); mu = m2; }
    double tb = mem_now();
    /* X = ((S >> (nq - 1 - dl)) mu) >> (k + 1) */
    mdb Ah, t, Xn; memset(&Ah, 0, sizeof Ah); memset(&t, 0, sizeof t); memset(&Xn, 0, sizeof Xn);
    { size_t sh = nq - 1 - dl; mdb_shift(&Ah, &S, (long)sh, S.n > sh ? S.n - sh : 1, G); }
    if (env_on("NEWTON_HIGHPROD")) mn_prod_cut_x1(&t, &Ah, &mu, G, k + 1, (size_t)-1);   /* A5/B3: the pieces below the cut k + 1 skipped */
    else mn_prod(&t, &Ah, &mu, G);
    mfree(&Ah); mfree(&mu);
    mdb_shift(&Xn, &t, (long)(k + 1), t.n > k + 1 ? t.n - (k + 1) : 1, G); mfree(&t);
    double tc = mem_now();
    /* the low product X Q mod B^w (A5: the grid with the pieces above w skipped, delivered in basis w), the window
     * A mod B^w = (S mod B^(w - dl)) B^dl, both in basis w; Q in basis w for the corrections */
    mdb xq, xql, Aw, Qw, Rd; memset(&xq, 0, sizeof xq); memset(&xql, 0, sizeof xql); memset(&Aw, 0, sizeof Aw); memset(&Qw, 0, sizeof Qw); memset(&Rd, 0, sizeof Rd);
    if (env_on("NEWTON_LOWPROD")) { mn_prod_cut_x1(&xql, &Xn, Q, G, 0, w); if (xql.N != w) { mdb_shift(&xq, &xql, 0, w, G); mfree(&xql); xql = xq; memset(&xq, 0, sizeof xq); } }   /* (the basis is w unless the product was shorter) */
    else { mn_prod(&xq, &Xn, Q, G); mdb_shift(&xql, &xq, 0, w, G); mfree(&xq); }
    rns_dist_cache_hold(0); rns_dist_cache_release();                 /* A1: Q's kept transforms served the low product; the planes go */
    mdb_shift(&Aw, &S, -(long)dl, w, G); mfree(&S);
    mdb_shift(&Qw, Q, 0, w, G); mfree(Q);
    double td = mem_now();
    size_t nc = 0; long dx = 0;
    if (mdb_cmp(&Aw, &xql, G) >= 0) {                                 /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        mdb_addsub(&Aw, &Aw, &xql, 1, G); mfree(&xql); Rd = Aw; memset(&Aw, 0, sizeof Aw);
        while (mdb_cmp(&Rd, &Qw, G) >= 0) { mdb_addsub(&Rd, &Rd, &Qw, 1, G); dx++; if (++nc > 64) { fprintf(stderr, "newton_mn_divmod: %zu corrections\n", nc); exit(1); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        mdb_addsub(&xql, &xql, &Aw, 1, G); mfree(&Aw); Rd = xql; memset(&xql, 0, sizeof xql);
        for (;;) { dx--; if (++nc > 64) { fprintf(stderr, "newton_mn_divmod: %zu corrections\n", nc); exit(1); }
                   if (mdb_cmp(&Rd, &Qw, G) <= 0) { mdb_addsub(&Rd, &Qw, &Rd, 1, G); break; } mdb_addsub(&Rd, &Rd, &Qw, 1, G); }
        newton_st.down_corr += (size_t)(-dx);
    }
    if (dx > 0) newton_st.up_corr += (size_t)dx;
    if (dx) mdb_add_val(&Xn, 0, (uint64_t)(dx < 0 ? -dx : dx), dx < 0, G);
    mfree(&Qw);
    double te = mem_now();
    mdb_mod_qs(&Rd, qs, nres, rres, G);
    mfree(&Rd);
    if (X->sh.cap) db_free(&X->sh);
    *X = Xn;
    newton_st.t_div += mem_now() - ta;
    if (me == 0) printf("divmod(mn) %.2f s: reciprocal %.2f (products %.2f), S = P + Q %.2f, A mu + shift %.2f, X Q + window %.2f, corrections %.2f (%ld), R residues %.2f; division products %.2f s; shifts %zu/%.2f s, addsub %zu/%.2f s\n",
                        mem_now() - t0, ta - t0, p_rec, tb - ta, tc - tb, td - tc, te - td, dx, mem_now() - te, mn_st.t_prod - p_rec, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub);
}
