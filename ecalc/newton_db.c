/* newton_db.c - the reciprocal and division of newton.c on device-resident
 * numbers (WP5 step 3): the same iteration, statistics and correction logic,
 * every big operation a dbig kernel and every product the distributed tier.
 * The host bigint interface is kept at the phase boundary: Q (and A) are
 * copied to device once, mu / X / R come back once. */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "newton.h"
#include "dbig.h"
#include "rns_mul.h"
#include "mem.h"
#include "memsample.h"                                       /* Phase 14 S1 (E1) */
#include "mn_plan.h"                                         /* Phase 13d L: the chain and the X1 group, asked by the plan printer too */
#include "binsplit.h"                                        /* Phase 14 L1: dm_tight (DM_TIGHT), read once with the layout's switches */
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
/* (a << j) <= b, both with the same limb count (rare path: an explicit shift).  plus1 (Phase 14 R1, E7): (a << j) <= b + 1 -- under the
 * cut, corr may be one below the exact value, so the overshoot test contains every exact overshoot (results/R114.md 1.4) */
static int shifted_cmp_le(const dbig *a, size_t j, const dbig *b, int plus1)
{
    dbig t; db_init(&t); db_shl_limbs(&t, a, j); int c = db_cmp(&t, b), le = c <= 0;
    if (!le && plus1) { db_sub(&t, &t, b); le = t.n == 1 && db_limb(&t, 0) == 1; }   /* (a << j) - b == 1: r' would be 1 */
    db_free(&t); return le;
}
/* Phase 14 R1 (E7, results/R114.md 1): NEWTON_RECIP_CUT=1 forms the reciprocal's two products (Q_t r, read as t1 >> (take - j); r d, read
 * as t1 >> j) as the grid with the pieces wholly below the read band skipped (rns_mul_high_db's low cut), NEWTON_RECIP_GUARD (1) limbs
 * below the band kept: the band is then off by at most one unit (in practice identical), the same kind of error as the floor the
 * step already makes; healed by the next doubling, absorbed by the division's corrections at the last.  0 (the default): whole */
static int recip_cut = -1, recip_guard = 1;
static void cut_init(void) { if (recip_cut < 0) { const char *e = getenv("NEWTON_RECIP_CUT"); recip_cut = e ? atoi(e) != 0 : 1;   /* default 1 since Phase 14 (R114) */ e = getenv("NEWTON_RECIP_GUARD"); if (e) recip_guard = atoi(e); if (recip_guard < 0) recip_guard = 0; } }
size_t newton_recip_cut(size_t v) { cut_init(); return recip_cut && v > (size_t)recip_guard ? v - (size_t)recip_guard : 0; }
/* Phase 15 R4 (V3 item 4 = PLAN 33 B2, results/R415.md 1): NEWTON_RECIP_MID=1 forms Q_t r as a middle product -- the grid with the
 * pieces at or above w = take + MID_HI skipped too (mul_grid's / mn_grid's high cut), the result taken mod B^w.  The step reads
 * u = t1 >> v (v = take - j) only through e = u - B^(2j); when the previous round of the chain bounds |e| < B^(j+3) (mid_ok), u is
 * fixed by u mod B^(j+4) = t1's limbs [v, take + 4), and the reconstructed u equals the cut path's u exactly (the same bits).  The
 * first round after the seed, the round after an overshoot, and the round after one whose |d| >= B^(j+1) form the whole (cut)
 * product.  0 (the default): as before */
#define MID_HI 4
static int recip_mid = -1;
static void mid_init(void) { if (recip_mid < 0) { const char *e = getenv("NEWTON_RECIP_MID"); recip_mid = e ? atoi(e) != 0 : 1; } }   /* default 1 since Phase 15 (the user's decision) */
size_t newton_recip_mid(size_t j, size_t take) { mid_init(); return recip_mid && j <= take && j >= MID_HI ? take + MID_HI : (size_t)-1; }   /* Q_t r's high cut ((size_t)-1: none; j >= MID_HI: w < take + j + 1, the top has something to skip) */
void newton_recip_set(int cut, int mid) { cut_init(); mid_init(); if (cut >= 0) recip_cut = cut != 0; if (mid >= 0) recip_mid = mid != 0; }   /* tests: override the switches (-1: keep) */
struct newton_mid_stats newton_mid_st;
/* u from its low j + 4 limbs ul (u mod B^(j+4), normalised), given |u - B^(2j)| < B^(j+3): e = ul when ul < B^(j+3), e = ul - B^(j+4)
 * when ul > B^(j+4) - B^(j+3); d = |e| -> r2; returns neg (e > 0, i.e. u > B^(2j)).  Anything else means the bound failed: fatal (R415.md 1.4) */
static int mid_d(dbig *r2, const dbig *ul, size_t j)
{
    int neg;
    if (ul->n <= j + 3) { neg = ul->n != 0; if (neg) db_copy(r2, ul); else db_set_zero(r2); }
    else { neg = 0; db_pow_sub(r2, j + 4, ul); }
    if (r2->n > j + 3) { ec_fatal(EC_RC_FATAL, "newton_db: the middle product's bound failed at j %zu (|e| has %zu limbs)\n", j, r2->n); }
    return neg;
}
static void recip_db2(dbig *mu, const dbig *Qd, const bigint *Q, size_t k, size_t nq_seed);
/* Phase 13d L: the chain's next target and the sharded reciprocal's start, extracted unchanged from recip_db2 / recip_mn (the plan
 * printer, mn_plan.c, asks them too).  Anchored (the default): the targets k, ceil(k/2), ... read from the top -- the next target
 * after j is the smallest of them above j; NEWTON_ANCHOR=0: doubling */
static void nv_init(void) { if (nv < 0) { nv = getenv("NEWTON_VERBOSE") ? atoi(getenv("NEWTON_VERBOSE")) : 0; if (getenv("NEWTON_ANCHOR")) anchor = atoi(getenv("NEWTON_ANCHOR")); } }
size_t newton_chain_next(size_t j, size_t k)
{
    nv_init();
    size_t jn = k;
    if (anchor) { while ((jn + 1) / 2 > j) jn = (jn + 1) / 2; }
    else jn = 2 * j < k ? 2 * j : k;
    return jn;
}
/* recip_mn: the single-node part ends at the largest target <= split (NEWTON_MN_SPLIT, 2^16), or the seed's 2 */
size_t newton_mn_chain_start(size_t k)
{
    nv_init();
    size_t split = getenv("NEWTON_MN_SPLIT") ? strtoull(getenv("NEWTON_MN_SPLIT"), 0, 10) : ((size_t)1 << 16), kp = k;
    if (anchor) { while (kp > split && (kp + 1) / 2 > 2) kp = (kp + 1) / 2; if (kp > split) kp = 2; }
    else { kp = 2; while (2 * kp <= split && 2 * kp < k) kp *= 2; if (kp > k) kp = k; }
    return kp;
}
static void recip_db(dbig *mu, const dbig *Qd, const bigint *Q, size_t k) { recip_db2(mu, Qd, Q, k, 0); }
/* Phase 15 R4: C = A B mod B^w with the low cut (w = (size_t)-1: the whole product, rns_mul_high_db as before) */
static void mul_band_db(dbig *C, const dbig *A, const dbig *B, size_t lowcut, size_t w)
{
    if (w == (size_t)-1) rns_mul_high_db(C, A, B, lowcut);
    else rns_mul_band_db(C, A, B, lowcut, w);
}
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
    /* Phase 14 L1 (APUMULT_STUDY E2, DM_TIGHT=1): the scratch at its use instead -- t1 per doubling at Q_t r's size (take + r.n + 8,
     * freed first: nothing of it survives a doubling), r2 at 2j + 4 right before d (db_pow_sub / db_sub_pow write 2j + 1 limbs; r' = (r << j)
     * +/- corr needs r.n + j + 1) and freed whenever its content is dead (after every swap).  The values are the same: only capacities
     * change.  The last doubling's set is P + Q + r (j + 1) + r2 (2j + 4) + t1 (n_Q + j + 9) + a piece = 5.2 n_Q + piece against 6.2 */
    dm_switches(); int tight = dm_tight > 0;
    if (!tight) { db_reserve(&r, k + 4); db_reserve(&r2, k + 4); db_reserve(&t1, tcap); }
    seed_db(&r, Q, Qd, nq_seed, &j);
    int mid_ok = 0;                                              /* Phase 15 R4: the seed's error is not bounded -- its first round forms the whole product */
    /* Phase 14 L1 (E2, second form, after job 21132): the three blocks reserved ONCE at the last doubling's sizes, now, while the pool holds
     * only P and Q -- r at jl + 4 (r has j + 2 limbs, measured), r2 at 2 jl + 4, t1 at Q_t r's size at jl (the hole of dm_layout) -- and
     * never swapped: r' is copied from r2 into r's block at every doubling but the last (about n_Q limbs of device copies in all), so the
     * blocks never move and the free space beside them stays one extent.  The per-doubling free-and-reserve form left r in the middle of
     * the free space and r2 without a contiguous fit at the last doubling (jobs 21131, 21132: a 4.44 GB hipMalloc with 8-11 GB free) */
    size_t jl = j; if (tight) { while (newton_chain_next(jl, k) < k) jl = newton_chain_next(jl, k);
        size_t tk = 2 * jl + 2 < nq ? 2 * jl + 2 : nq; db_reserve(&t1, tk + (jl + 2) + 8);   /* t1: the tail's back */
        db_pool_pin_tail(1); db_reserve(&r, jl + 4); db_pool_pin_tail(0);                    /* r: right below it -- the tail is full (job 21154: 7 small blocks had spilled into its front and t found it 0.11 GB short) */
        db_reserve(&r2, 2 * jl + 4); }                                                        /* r2: outside, just below Q (the highest free end); mu takes it */
    int ts_on = getenv("NEWTON_DOUBLING_TS") ? atoi(getenv("NEWTON_DOUBLING_TS")) : 0; double ts_prev = mem_now();   /* S21 (A37-R6): NEWTON_DOUBLING_TS=1 (off by default; print only) -- one line per doubling with its end time */
    while (j < k) {
        size_t jn = newton_chain_next(j, k);                       /* Phase 13d L: extracted (unchanged) */
        for (;;) {
            size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
            dbig qt = db_view(Qd, nq - take, take);                  /* top limbs of Q */
            /* Phase 10 A1: at the top Q_t is Q itself, the operand of the division's X Q: its pieces' transforms are kept
             * (rns_dist_cache_hold: only when the cache has the slots for them) -- Q as the B operand, whose pieces the
             * grid keeps in distinct slots; the product is the same either way */
            size_t c1 = j <= take ? newton_recip_cut(take - j) : 0, f0 = rns_dist_st.n_formed, s0 = rns_dist_st.n_skipped;   /* Phase 14 R1 (E7): the low cut under the band read */
            size_t hw = mid_ok ? newton_recip_mid(j, take) : (size_t)-1;   /* Phase 15 R4: the high cut (the middle product) when the previous round bounds |e| */
            if (g_hold_q && take == nq && rns_dist_cache_hold(1)) mul_band_db(&t1, &r, &qt, c1, hw);
            else mul_band_db(&t1, &qt, &r, c1, hw);                 /* Q_t r (the pieces below the cut skipped; c1 = 0: whole, as before; R4: mod B^hw) */
            dbig u; int neg;                                        /* u ~ B^(2j): t1 >> (take - j) as a view, or << (j - take) (rare) */
            if (hw != (size_t)-1) {                                 /* R4: u from u mod B^(j+4) (R415.md 1.2) */
                u = db_view(&t1, take - j, t1.n > take - j ? t1.n - (take - j) : 0); db_norm(&u);
                neg = mid_d(&r2, &u, j); newton_mid_st.mid++;
            } else {
                if (j <= take) { u = db_view(&t1, take - j, t1.n > take - j ? t1.n - (take - j) : 0); db_norm(&u); }
                else { db_shl_limbs(&t2, &t1, j - take); u = t2; }
                neg = u.n > 2 * j + 1 || (u.n == 2 * j + 1 && (db_top(&u) > 1 || maxidx_below(&u, 2 * j)));
                if (neg) db_sub_pow(&r2, &u, 2 * j); else db_pow_sub(&r2, 2 * j, &u);   /* d = |B^(2j) - u| -> r2 */
                newton_mid_st.whole++;
            }
            size_t dn = r2.n;
            mid_ok = dn <= j + 1;                                   /* R4: |e| < B^(j+1) here bounds the next round's |e| < B^(j'+3) (R415.md 1.3); an overshoot clears it */
            rns_mul_high_db(&t1, &r, &r2, newton_recip_cut(j));    /* r |d| -> t1 (u is consumed); E7: the pieces below j - guard skipped */
            dbig corr = db_view(&t1, j, t1.n > j ? t1.n - j : 0); db_norm(&corr);   /* |corr| = t1 >> j */
            int converged = corr.n <= j + 1;
            if (recip_cut && rns_dist_st.n_formed + rns_dist_st.n_skipped > f0 + s0)   /* the doublings whose products are grids */
                printf("newton(db) j %zu (k %zu): recip cut %zu pieces skipped, %zu formed in the step's two products\n", j, k, rns_dist_st.n_skipped - s0, rns_dist_st.n_formed - f0);
            if (neg) {                                              /* r' = (r << j) - corr; overshoot if that would be <= 0 (E7: <= 1) */
                size_t rn = r.n + j; int over = rn < corr.n || (rn == corr.n && shifted_cmp_le(&r, j, &corr, recip_cut));
                if (over) { newton_st.overshoots++; shrink_db(&r); mid_ok = 0; continue; }
                db_sub_shifted(&r2, &r, j, &corr);
            } else db_add_shifted(&r2, &r, j, &corr);
            if (converged) { if (tight && jn < k) db_copy(&r, &r2); else { dbig sw = r; r = r2; r2 = sw; } }   /* r <- r' by swap; E2: by a copy into r's block, except at the last doubling (r' becomes mu) */
            else db_shr_limbs(&r, &r2, j);
            if (nv) printf("newton(db) j %zu -> %zu (k %zu): take %zu, r %zu limbs%s   dev pools %.1f GB%s (d %zu limbs, t1 %zu of cap %zu, r2 cap %zu)\n", j, jn, k, take, r.n, converged ? "" : " (repeat)", mem_dev_pool_bytes() / 1e9, tight ? "  tight" : "", dn, t1.n, t1.cap, r2.cap);
            if (mem_live_on()) { char w_[64]; snprintf(w_, sizeof w_, "recip(db) j %zu -> %zu", j, jn); mem_live_line(w_); }   /* Phase 14 S1 (E1): the pool's live bytes per doubling */
            if (!converged) { newton_st.repeats++; continue; }
            break;
        }
        if (jn < 2 * j) {                                           /* the anchored target is below 2j: r >>= 2j - jn */
            if (tight && jn == k) { db_shr_limbs(&t1, &r, 2 * j - jn); db_copy(&r, &t1); }   /* E2: the last doubling -- r2 is the small block now; through the dead t1 (job 21151: the shift into r2 regrew it by 1.0 n_Q with everything live) */
            else { db_shr_limbs(&r2, &r, 2 * j - jn); if (tight && jn < k) db_copy(&r, &r2); else { dbig sw = r; r = r2; r2 = sw; } }
        }
        if (ts_on) { double ts_now = mem_now(); printf("recip ts: doubling j %zu -> %zu (k %zu, Q %zu limbs, r %zu limbs): +%.4f s, %.4f s since the chain began (at %.3f s of the run), %zu repeats so far\n", j, jn, k, nq, r.n, ts_now - ts_prev, ts_now - t0, ts_now, (size_t)newton_st.repeats); ts_prev = ts_now; }   /* S21 (A37-R6) */
        j = jn;
        newton_st.iters++;
    }
    if (j > k) { if (tight) { db_shr_limbs(&t1, &r, j - k); db_copy(&r, &t1); } else { db_shr_limbs(&r2, &r, j - k); dbig sw = r; r = r2; r2 = sw; } }
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
    if (co) { ec_fatal(EC_RC_FATAL, "newton_db: X %s out of its top limb\n", dx < 0 ? "borrows" : "carries"); }
    if (dx < 0) db_norm(x);
}
int newton_x_defer = 0; long newton_x_dx = 0;          /* Phase 15 K (ECALC_CORR_PATCH): newton.h */
int (*newton_db_xhi_hook)(dbig *Xh, size_t s, void *arg) = 0; int (*newton_db_xlo_hook)(dbig *Xl, size_t s, int carry, void *arg) = 0;   /* Phase 15 EW (MN_OUT_DKM_HI): newton.h */
long newton_test_corr(void)                          /* Phase 15 K: ECALC_TEST_CORR=<k>, the forced-correction test hook (0: off) */
{
    const char *e = getenv("ECALC_TEST_CORR"); long k = e ? atol(e) : 0;
    if (k > 60 || k < -60) { ec_fatal(EC_RC_FATAL, "ECALC_TEST_CORR=%ld: |k| <= 60 (the division stops at 64 corrections)\n", k); }
    return k;
}
/* Phase 15 DKM (newton.h): the switch and the reciprocal's length */
static int dkm_sw = -1;
int newton_dkm_on(void) { if (dkm_sw < 0) { const char *e = getenv("NEWTON_DKM"); dkm_sw = e ? atoi(e) != 0 : 1; } return dkm_sw; }   /* default 1 since Phase 15 Batch 3 (the user's decision, 2026-09-29) */
void newton_dkm_set(int on) { dkm_sw = on != 0; }
size_t newton_dkm_h(size_t k) { return k / 2 + 1; }         /* max(k - s, s + 1) at s = floor(k/2) */
/* Phase 15 int15k (DM_MN_LEAN, results/M615.md 1.2's target part; off by default): the multi-node reciprocal and DKM division without
 * their dead copies -- recip_mn frees t once u (then corr) is formed; mn_divmod_dkm takes A_h and mu's top as VIEWS of S / R1 and mu
 * (mdb_view) instead of mdb_shift copies, and Q in basis w (Qw) replaces Q once the window is formed.  Value-identical (the same
 * products on the same limbs); the layout (binsplit.c dm_layout, mem_model.dm_layout) counts the lean division set at size > 1 */
static int lean_sw = -1;
int newton_mn_lean(void) { if (lean_sw < 0) { const char *e = getenv("DM_MN_LEAN"); lean_sw = e ? atoi(e) != 0 : 0; } return lean_sw; }
static long dkm_test_hi(void)                               /* NEWTON_DKM_TEST_HI=<k>: X_hi - k before step 1's corrections (a test hook) */
{
    const char *e = getenv("NEWTON_DKM_TEST_HI"); long k = e ? atol(e) : 0;
    if (k > 60 || k < -60) { ec_fatal(EC_RC_FATAL, "NEWTON_DKM_TEST_HI=%ld: |k| <= 60\n", k); }
    return k;
}
void newton_db_recip(bigint *mu, const bigint *Q, size_t k)
{
    dbig Qd; db_init(&Qd);
    if (newton_db_Qd) Qd = *newton_db_Qd; else db_from_bi(&Qd, Q);
    size_t kr = newton_db_Qd && newton_dkm_on() && k >= 4 ? newton_dkm_h(k) : k;   /* DKM: the device flow's prewarm stops at h (the division needs no more) */
    if (kr != k && getenv("NEWTON_VERBOSE")) printf("newton(db): DKM -- the reciprocal to %zu limbs for k %zu\n", kr, k);
    g_hold_q = newton_db_Qd != 0; recip_db(&g_mu_kept, &Qd, Q, kr); g_hold_q = 0; g_mu_k = k;   /* (A1: the hold needs Q alive across both phases: the caller's device Q) */
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
    if (!Q->n) { ec_fatal(EC_RC_FATAL, "newton_db_divmod: Q = 0\n"); }
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
    { long tk = newton_test_corr(); if (tk) db_add_small(&Xd, -tk); }   /* Phase 15 K: ECALC_TEST_CORR */
    int defer = newton_x_defer && newton_db_x_hook; long dxd = 0; newton_x_dx = 0;   /* Phase 15 K: the corrections deferred to the output's tail patch */
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
            if (defer) dxd--; else { bigint o; bi_init(&o); bi_set_u64(&o, 1); bi_sub(X, X, &o); bi_free(&o); }   /* (Phase 15 K: deferred) */
            uint64_t c = limb_add(R->l, R->l, w, Q->l, Q->n); (void)c;
            newton_st.down_corr++;
        } else {
            R->n = w; bi_norm(R);
            if (bi_cmp(R, Q) < 0) break;
            limb_sub(R->l, R->l, w, Q->l, Q->n);
            if (defer) dxd++; else bi_add_u64(X, 1);           /* (Phase 15 K: deferred) */
            newton_st.up_corr++;
        }
        if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_db_divmod: %zu corrections, mu is wrong\n", nc); }
    }
    R->n = w; bi_norm(R);
    newton_x_dx = dxd;                                           /* Phase 15 K: X is the hook's; the output stage patches the digits by dxd */
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
/* ---- Phase 15 DKM (results/DKM15.md 1.2): the shifted division in two quotient halves, size 1 -------------------------------- */
/* X0 = ((S >> (nq - 1 - dl)) mu) >> (k + 1), mu (a view of) k + 1 limbs; X0 = 0 when S has nothing at or above nq - 1 - dl */
static void dkm_est_db(dbig *Xd, const dbig *S, size_t dl, size_t nq, size_t k, const dbig *mu)
{
    size_t sh = nq - 1 - dl; dbig Ah = db_view(S, sh, S->n > sh ? S->n - sh : 0); db_norm(&Ah);
    if (!Ah.n || !mu->n) { db_set_zero(Xd); return; }
    dbig t; db_init(&t);
    if (env_on("NEWTON_HIGHPROD")) rns_mul_high_db(&t, &Ah, mu, k + 1); else rns_mul_dist_db(&t, &Ah, mu);
    db_shr_limbs(Xd, &t, k + 1); db_free(&t);
}
/* A mod B^w for A = S B^dl: (S mod B^(w - dl)) B^dl by a device shift (db_set_shifted_low copies limb by limb: a few limbs only) */
static void dkm_window_db(dbig *Aw, const dbig *S, size_t dl, size_t w)
{
    size_t m = w > dl ? w - dl : 0; if (m > S->n) m = S->n;
    dbig lo = db_view(S, 0, m); db_norm(&lo);
    if (!lo.n) { db_set_zero(Aw); return; }
    db_shl_limbs(Aw, &lo, dl);
}
static void dkm_add_small(dbig *x, long dx)
{
    if (!dx) return;
    if (!x->n && dx > 0) db_set_u64(x, (uint64_t)dx);
    else if (dx > 0) { dbig v; db_init(&v); db_set_u64(&v, (uint64_t)dx); db_add(x, x, &v); db_free(&v); }   /* Phase 15 EW: may grow a limb (ECALC_TEST_CORR < 0 on an X_lo0 near B^s: t_newton 7) */
    else db_add_small(x, dx);
}
/* R = A - X Q from the window Aw and xq = X Q mod B^w (both consumed): the corrections of newton_db_divmod_shifted, R >= 0 < Q in *Rd;
 * returns the change to X */
static long dkm_corr_db(dbig *Rd, dbig *Aw, dbig *xq, const dbig *Qd, const char *who)
{
    size_t nc = 0; long dx = 0;
    if (db_cmp(Aw, xq) >= 0) {                                        /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        if (xq->n) db_sub(Aw, Aw, xq);
        db_free(xq); *Rd = *Aw; db_init(Aw);
        while (db_cmp(Rd, Qd) >= 0) { db_sub(Rd, Rd, Qd); dx++; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_db (DKM %s): %zu corrections\n", who, nc); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        if (Aw->n) db_sub(xq, xq, Aw);
        db_free(Aw); *Rd = *xq; db_init(xq);
        for (;;) { dx--; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_db (DKM %s): %zu corrections\n", who, nc); }
                   if (db_cmp(Rd, Qd) <= 0) { db_sub(Rd, Qd, Rd); break; } db_sub(Rd, Rd, Qd); }
    }
    return dx;
}
/* A = S B^dl; s = min(floor(k/2), dl) >= 1 (the caller checks).  Step 1: X_hi, R1 = floor / remainder of A >> s = S B^(dl - s) by Q, exact;
 * step 2: X_lo, R of R1 B^s; X = X_hi B^s + X_lo.  The hook, x_dev, the deferral and ECALC_TEST_CORR at step 2 as in newton_db_divmod_shifted */
static void divmod_shifted_dkm(bigint *X, const dbig *S, size_t dl, const dbig *Qd, const uint64_t *qs, int nres, uint64_t *rres, size_t s)
{
    double t0 = mem_now();
    size_t nq = Qd->n, na = S->n + dl, k = na - nq + 1, w = nq + 2, k1 = k - s, h = k1 > s + 1 ? k1 : s + 1;
    dbig mu, Xh, Aw, xq, R1, Xl, Xd, Rd; db_init(&mu); db_init(&Xh); db_init(&Aw); db_init(&xq); db_init(&R1); db_init(&Xl); db_init(&Xd); db_init(&Rd);
    int kept_ok = g_mu_kept.n >= h + 1 && g_mu_ql == Qd->q[0] && g_mu_qn == nq && g_mu_qtop == db_top(Qd);
    if (g_mu_kept.n && !kept_ok) { printf("newton(db) DKM: the kept reciprocal (%zu limbs) does not serve h %zu: a fresh one\n", g_mu_kept.n, h); db_free(&g_mu_kept); }
    if (kept_ok) { mu = g_mu_kept; db_init(&g_mu_kept); }
    else recip_db(&mu, Qd, 0, h);
    double ta = mem_now();
    /* step 1: the high half.  mu's top k1 + 1 limbs as a view */
    { dbig m1 = db_view(&mu, mu.n - (k1 + 1), k1 + 1); dkm_est_db(&Xh, S, dl - s, nq, k1, &m1); }
    { long th = dkm_test_hi(); if (th) { if (th > 0 && (!Xh.n || (Xh.n == 1 && db_limb(&Xh, 0) < (uint64_t)th))) { ec_fatal(EC_RC_FATAL, "NEWTON_DKM_TEST_HI=%ld: X_hi is below it\n", th); } dkm_add_small(&Xh, -th); } }
    dkm_window_db(&Aw, S, dl - s, w);
    dm_switches(); if (dm_tight > 0) db_free((dbig *)S);              /* (as newton_db_divmod_shifted under DM_TIGHT: S's only reader was the window) */
    double tb = mem_now();
    if (Xh.n) rns_mul_low_db(&xq, &Xh, Qd, w); else db_set_zero(&xq);
    long dx1 = dkm_corr_db(&R1, &Aw, &xq, Qd, "step 1");
    dkm_add_small(&Xh, dx1);                                          /* X_hi exact: no one has seen it */
    newton_st.dkm_corr += (size_t)(dx1 < 0 ? -dx1 : dx1);
    /* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3) -- the X_hi hook: X_hi is final here (X's limbs >= s, whatever step 2 does);
     * the hook takes it and starts the writer on it.  No X0 is formed below: X_lo goes to newton_db_x_dev, X_hi stays the hook's */
    int xhi = newton_db_xhi_hook && newton_db_xlo_hook && newton_db_x_dev && newton_x_defer;
    if (xhi) xhi = newton_db_xhi_hook(&Xh, s, newton_db_x_arg);
    double tc = mem_now();
    /* step 2: the low half, A2 = R1 B^s (A mod B^s = 0: s <= dl) */
    size_t na2 = R1.n + s, k2 = na2 >= nq ? na2 - nq + 1 : 0;
    if (na2 >= nq) { dbig m2 = db_view(&mu, mu.n - (k2 + 1), k2 + 1); dkm_est_db(&Xl, &R1, s, nq, k2, &m2); }
    else db_set_zero(&Xl);                                            /* A2 < Q: X_lo = 0 */
    db_free(&mu);
    dkm_window_db(&Aw, &R1, s, w); db_free(&R1);
    { long tk = newton_test_corr();                                    /* Phase 15 K: ECALC_TEST_CORR on X_lo (X - k) */
      if (tk) { if (tk > 0 && (!Xl.n || (Xl.n == 1 && db_limb(&Xl, 0) < (uint64_t)tk))) { ec_fatal(EC_RC_FATAL, "ECALC_TEST_CORR=%ld under NEWTON_DKM: X_lo is below it\n", tk); } dkm_add_small(&Xl, -tk); } }
    if (xhi) {   /* Phase 15 EW -- the X_lo hook: X_lo0 to the caller's device X; the hook releases the writer onto it unless X_lo0 >= B^s (then
                  * the corrections go into X_lo0 in place, results/EW15.md 1.2 item 3); X = X_hi B^s + X_lo is never formed */
        double td = mem_now();
        int carry = Xl.n > s; *newton_db_x_dev = Xl; db_init(&Xl);
        newton_x_dx = 0;
        int rel = newton_db_xlo_hook(newton_db_x_dev, s, carry, newton_db_x_arg);
        if (newton_db_x_dev->n) rns_mul_low_db(&xq, newton_db_x_dev, Qd, w); else db_set_zero(&xq);
        rns_dist_cache_hold(0); rns_dist_cache_release();
        double te = mem_now();
        long dx = dkm_corr_db(&Rd, &Aw, &xq, Qd, "step 2");
        if (dx < 0) newton_st.down_corr += (size_t)(-dx); else newton_st.up_corr += (size_t)dx;
        if (dx && rel) newton_x_dx = dx; else dkm_add_small(newton_db_x_dev, dx);
        double tr = mem_now();
        db_mod_qs(&Rd, qs, nres, rres);
        double tf = mem_now();
        if (mem_live_on()) mem_live_line("divmod(dev, DKM) end");
        db_free(&Rd); db_free(&xq); db_free(&Aw);
        if (getenv("RNS_VERBOSE") || getenv("NEWTON_VERBOSE"))
            printf("divmod(dev, DKM, X_hi writer) %.2f s: k %zu = %zu + %zu (s), h %zu%s; mu %.2f, step 1 A mu %.2f, X_hi Q + corrections %.2f (%ld), step 2 A mu %.2f, X_lo Q %.2f, corrections %.2f (%ld, %s), R residues %.2f; X_lo0 %s B^s; pools %.1f GB\n",
                   mem_now() - t0, k, k1, s, h, kept_ok ? " (kept)" : " (fresh)", ta - t0, tb - ta, tc - tb, dx1, td - tc, te - td, tr - te, dx, rel ? "deferred" : "in place", tf - tr, carry ? ">=" : "<", db_pool_bytes() / 1e9);
        newton_st.t_div += mem_now() - t0;
        return;
    }
    db_add_shifted(&Xd, &Xh, s, &Xl); db_free(&Xh);                   /* X0 = X_hi B^s + X_lo0 */
    int view = Xl.n <= s; if (view) db_free(&Xl);                     /* X_lo0 < B^s: it is X0's low s limbs */
    double td = mem_now();
    int defer = newton_x_defer && newton_db_x_hook; newton_x_dx = 0;
    dbig *Xp = &Xd;
    if (newton_db_x_dev) { *newton_db_x_dev = Xd; db_init(&Xd); Xp = newton_db_x_dev; if (newton_db_x_hook) newton_db_x_hook(X, newton_db_x_arg); }
    else if (newton_db_x_hook) { db_to_bi(X, &Xd); newton_db_x_hook(X, newton_db_x_arg); }
    dbig Xlv = Xl; if (view) { Xlv = db_view(Xp, 0, Xp->n < s ? Xp->n : s); db_norm(&Xlv); }
    if (Xlv.n) rns_mul_low_db(&xq, &Xlv, Qd, w); else db_set_zero(&xq);
    if (!view) db_free(&Xl);
    rns_dist_cache_hold(0); rns_dist_cache_release();
    if (newton_db_x_hook && !newton_db_x_dev) db_free(&Xd);           /* X is on the host; corrections go to the host copy */
    double te = mem_now();
    long dx = dkm_corr_db(&Rd, &Aw, &xq, Qd, "step 2");
    if (dx < 0) newton_st.down_corr += (size_t)(-dx); else newton_st.up_corr += (size_t)dx;
    if (!newton_db_x_hook && !newton_db_x_dev) { db_to_bi(X, &Xd); db_free(&Xd); }
    if (dx && defer) newton_x_dx = dx;
    else if (dx && newton_db_x_dev) db_add_small(newton_db_x_dev, dx);
    else if (dx) { bigint o; bi_init(&o); bi_set_u64(&o, (uint64_t)(dx < 0 ? -dx : dx)); if (dx < 0) bi_sub(X, X, &o); else bi_add(X, X, &o); bi_free(&o); }
    double tr = mem_now();
    db_mod_qs(&Rd, qs, nres, rres);
    double tf = mem_now();
    if (mem_live_on()) mem_live_line("divmod(dev, DKM) end");
    db_free(&Rd); db_free(&xq); db_free(&Aw);
    if (getenv("RNS_VERBOSE") || getenv("NEWTON_VERBOSE"))
        printf("divmod(dev, DKM) %.2f s: k %zu = %zu + %zu (s), h %zu%s; mu %.2f, step 1 A mu %.2f, X_hi Q + corrections %.2f (%ld), step 2 A mu + assembly %.2f, X_lo Q %.2f, corrections %.2f (%ld), R residues %.2f; pools %.1f GB\n",
               mem_now() - t0, k, k1, s, h, kept_ok ? " (kept)" : " (fresh)", ta - t0, tb - ta, tc - tb, dx1, td - tc, te - td, tr - te, dx, tf - tr, db_pool_bytes() / 1e9);
    newton_st.t_div += mem_now() - t0;
}
void newton_db_divmod_shifted(bigint *X, const dbig *S, size_t dl, const dbig *Qd, const uint64_t *qs, int nres, uint64_t *rres)
{
    double t0 = mem_now();
    size_t nq = Qd->n, na = S->n + dl, k = na - nq + 1, w = nq + 2;
    if (!nq || na < nq) { ec_fatal(EC_RC_FATAL, "newton_db_divmod_shifted: A < Q not supported here\n"); }
    if (dl + 1 > nq) { ec_fatal(EC_RC_FATAL, "newton_db_divmod_shifted: dl >= nq\n"); }
    if (newton_dkm_on() && k >= 4 && dl >= 1) { divmod_shifted_dkm(X, S, dl, Qd, qs, nres, rres, k / 2 < dl ? k / 2 : dl); return; }   /* Phase 15 DKM */
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
    { long tk = newton_test_corr(); if (tk) db_add_small(&Xd, -tk); }   /* Phase 15 K: ECALC_TEST_CORR */
    int defer = newton_x_defer && newton_db_x_hook; newton_x_dx = 0;   /* Phase 15 K: the corrections deferred to the output's tail patch */
    double tc = mem_now();
    const dbig *Xp = &Xd;
    if (newton_db_x_dev) { *newton_db_x_dev = Xd; db_init(&Xd); Xp = newton_db_x_dev; if (newton_db_x_hook) newton_db_x_hook(X, newton_db_x_arg); }   /* H B1: X stays on the device; the hook starts the writer on it */
    else if (newton_db_x_hook) { db_to_bi(X, &Xd); newton_db_x_hook(X, newton_db_x_arg); }
    /* Phase 14 L1 (DM_TIGHT): the window Aw is formed BEFORE the low product and S (the caller's bs_Pd: its only reader from here) is freed
     * right after, so the low product's set is Q + X + Aw + xq (+ a piece) and xq finds S's slot + the tail's rest as one extent; the
     * caller's own db_free of S afterwards finds cap 0 and does nothing.  Values unchanged: Aw depends on S alone */
    dm_switches(); int tight = dm_tight > 0;
    if (tight) { db_set_shifted_low(&Aw, S, w - dl, dl, w); db_free((dbig *)S); }
    rns_mul_low_db(&xq, Xp, Qd, w);                                   /* low_w(X Q): Q's cached transforms hit when held (A1) */
    rns_dist_cache_hold(0); rns_dist_cache_release();                 /* A1: the low product was the last grid product; the planes go */
    if (newton_db_x_hook && !newton_db_x_dev) db_free(&Xd);           /* X is on the host; corrections go to the host copy */
    double td = mem_now();
    /* the window: A mod B^w = (S mod B^(w - dl)) B^dl; R formed in place in it */
    if (!tight) db_set_shifted_low(&Aw, S, w - dl, dl, w);
    size_t nc = 0; long dx = 0;                                       /* corrections to X: applied to the host copy at the end */
    if (db_cmp(&Aw, &xq) >= 0) {                                      /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        db_sub(&Aw, &Aw, &xq); db_free(&xq); Rd = Aw; db_init(&Aw);
        while (db_cmp(&Rd, Qd) >= 0) { db_sub(&Rd, &Rd, Qd); dx++; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_db_divmod_shifted: %zu corrections\n", nc); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        db_sub(&xq, &xq, &Aw); db_free(&Aw); Rd = xq; db_init(&xq);
        for (;;) { dx--; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_db_divmod_shifted: %zu corrections\n", nc); }
                   if (db_cmp(&Rd, Qd) <= 0) { db_sub(&Rd, Qd, &Rd); break; } db_sub(&Rd, &Rd, Qd); }
        newton_st.down_corr += (size_t)(-dx);
    }
    if (dx > 0) newton_st.up_corr += (size_t)dx;
    double te = mem_now();
    if (!newton_db_x_hook && !newton_db_x_dev) { db_to_bi(X, &Xd); db_free(&Xd); }
    if (dx && defer) newton_x_dx = dx;                                /* Phase 15 K: X stays the hook's (the writer reads it); the output patches the tail */
    else if (dx && newton_db_x_dev) db_add_small(newton_db_x_dev, dx);     /* H B1: the correction on the device X (the writer redoes the digits) */
    else if (dx) { bigint o; bi_init(&o); bi_set_u64(&o, (uint64_t)(dx < 0 ? -dx : dx)); if (dx < 0) bi_sub(X, X, &o); else bi_add(X, X, &o); bi_free(&o); }
    db_mod_qs(&Rd, qs, nres, rres);
    double tf = mem_now();
    if (mem_live_on()) mem_live_line("divmod(dev) end");         /* Phase 14 S1 (E1): the division's window peak */
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
#define MN_HIP(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
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
static struct { size_t n_shift, n_addsub, n_small; double t_shift, t_addsub, t_small, t_prod; size_t shift_max; } mn_st;   /* shift_max: Phase 13a M, the largest sb + rb of one mdb_shift (bytes per node-process, measured) */
extern size_t rns_dist_tscratch_max;                                 /* rns_dist.c (Phase 13a M): the largest T + rbO of a product or shifted add */
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
    /* Phase 13a M (TASKS 1.2): MDB_SHIFT_CHUNK_MB=m > 0 exchanges the pieces in K rounds, round c carrying the sub-range
     * [a + len c / K, a + len (c + 1) / K) of every piece part, so sb and rb hold about SL / K limbs per APU (m MB each)
     * instead of my share / 4 (71 GB per node at 576 x 8e10).  K = ceil(SL / chunk) from the group's share bound, the same
     * on every node (one alltoallv per round on every mesh).  The limbs moved are the same: the result is bit-identical.
     * 0 (the default): one round, as before. */
    static long shift_chunk = -1; if (shift_chunk < 0) { const char *e = getenv("MDB_SHIFT_CHUNK_MB"); shift_chunk = (long)((e ? atof(e) : 1024.0) * 1048576.0 / 8);   /* default 1024 MB since Phase 13c; =0 exchanges in one round */ if (shift_chunk < 0) shift_chunk = 0; }
    int K = shift_chunk > 0 && SL > (size_t)shift_chunk ? (int)((SL + shift_chunk - 1) / shift_chunk) : 1;
    size_t SLk = K > 1 ? ((SL + K - 1) / K + 1 + 15) / 16 * 16 : SL;
    size_t shift_bytes = 0;
    if (X->n && n2) {
#pragma omp parallel num_threads(4)
    {
        int d = omp_get_thread_num(); hipStream_t st = g_ms[d];
        MN_HIP(hipSetDevice(d));
        struct piece *hp = (struct piece *)malloc(4 * g * sizeof *hp), *hq = hp + g, *hpk = hp + 2 * g, *hqk = hp + 3 * g, *dp = (struct piece *)db_pool_alloc(d, g * sizeof *hp + 64);
        size_t *cnt = (size_t *)malloc(4 * (size_t)g * sizeof *cnt), *scnt = cnt, *sdsp = cnt + g, *rcnt = cnt + 2 * g, *rdsp = cnt + 3 * g, ts = 0, tr = 0, tsm = 0, trm = 0;
        int any = 0, anyr = 0;
        for (int r = 0; r < g; r++) { any |= piece_of(X, s, &Yn, n2, node, G->g0 + r, d, &hp[r]); }
        for (int r = 0; r < g; r++) { anyr |= piece_of(X, s, &Yn, n2, G->g0 + r, node, d, &hq[r]); }
        for (int c = 0; c < K; c++) {                     /* the largest round's totals size the slabs */
            size_t a = 0, b = 0;
            for (int r = 0; r < g; r++) a += hp[r].len * (c + 1) / K - hp[r].len * c / K;
            for (int r = 0; r < g; r++) b += hq[r].len * (c + 1) / K - hq[r].len * c / K;
            if (a > tsm) tsm = a; if (b > trm) trm = b;
        }
        uint64_t *sb = db_pool_alloc(d, (tsm + 16) * 8), *rb = db_pool_alloc(d, (trm + 16) * 8);
#pragma omp atomic
        shift_bytes += (tsm + trm + 32) * 8;
        for (int c = 0; c < K; c++) {
            ts = tr = 0;
            for (int r = 0; r < g; r++) { size_t l0 = hp[r].len * c / K, l1 = hp[r].len * (c + 1) / K; hpk[r].a = hp[r].a + l0; hpk[r].len = l1 - l0; hpk[r].off = ts; scnt[r] = hpk[r].len * 8; sdsp[r] = ts * 8; ts += hpk[r].len; }
            for (int r = 0; r < g; r++) { size_t l0 = hq[r].len * c / K, l1 = hq[r].len * (c + 1) / K; hqk[r].a = hq[r].a + l0; hqk[r].len = l1 - l0; hqk[r].off = tr; rcnt[r] = hqk[r].len * 8; rdsp[r] = tr * 8; tr += hqk[r].len; }
            if (any) {
                MN_HIP(hipMemcpyAsync(dp, hpk, g * sizeof *hp, hipMemcpyHostToDevice, st));
                k_mn_pack<<<mn_nblk((size_t)g * SLk), 256, 0, st>>>(sb, sacc_of(&X->sh), s - (long)lo1, dp, g, SLk);
            }
            MN_HIP(hipStreamSynchronize(st));
            comm_alltoallv(G->all[d], sb, scnt, sdsp, rb, rcnt, rdsp, st); comm_wait(G->all[d]);
            if (anyr && cn) {
                MN_HIP(hipMemcpyAsync(dp, hqk, g * sizeof *hq, hipMemcpyHostToDevice, st));
                k_mn_scatter<<<mn_nblk((size_t)g * SLk), 256, 0, st>>>(sacc_of(&Yn.sh), lo2, rb, dp, g, SLk);
            }
            MN_HIP(hipStreamSynchronize(st));
        }
        db_pool_free(d, sb); db_pool_free(d, rb); db_pool_free(d, (uint64_t *)dp); free(hp); free(cnt);
    }
    MN_HIP(hipSetDevice(0));
    }
    Yn.sh.n = cn;
    if (trunc) { size_t top = 0; if (cn) { dbig t = Yn.sh; db_norm(&t); top = t.n ? lo2 + t.n : 0; } Yn.n = comm_allreduce_max(G->all[0], top); }
    if (Y->sh.cap) db_free(&Y->sh);
    *Y = Yn;
    if (shift_bytes > mn_st.shift_max) mn_st.shift_max = shift_bytes;
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
    if (A->N != B->N || A->g0 != B->g0 || A->g != B->g) { ec_fatal(EC_RC_FATAL, "mdb_addsub: bases %zu / %zu\n", A->N, B->N); }
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(A, node, &lo, &hi); size_t cn = hi - lo;
    mdb Yn; if (Y == A || Y == B) Yn = *Y; else { memset(&Yn, 0, sizeof Yn); Yn.N = A->N; Yn.g0 = A->g0; Yn.g = A->g; db_init(&Yn.sh); db_zero_fill(&Yn.sh, cn); }
    int co = 0, pr = 1, top = 0;
    if (cn) db_share_addsub(&Yn.sh, &A->sh, &B->sh, cn, sub, &co, &pr);
    int cin = node_scan(G, co, pr, &top);
    if (top) { ec_fatal(EC_RC_FATAL, "mdb_addsub: %s out of the top (basis %zu)\n", sub ? "borrow" : "carry", A->N); }
    if (cin) { int c2 = 0; if (!cn) { ec_fatal(EC_RC_FATAL, "mdb_addsub: a carry into an empty share\n"); } db_share_add_val(&Yn.sh, cn, 0, 1, sub, &c2, 0); }
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
    if (top) { ec_fatal(EC_RC_FATAL, "mdb_add_val: %s out of the top\n", sub ? "borrow" : "carry"); }
    if (cin) { int c2 = 0; db_share_add_val(&Y->sh, cn, 0, 1, sub, &c2, 0); }
    Y->sh.n = cn; mdb_norm(Y, G);
    mn_st.t_small += mem_now() - t0;
}
/* Y = B^e in basis N2 */
static void mdb_pow(mdb *Y, size_t e, size_t N2, mn_group *G)
{
    mdb Yn; memset(&Yn, 0, sizeof Yn); Yn.N = N2; Yn.g0 = G->g0; Yn.g = G->g; Yn.n = e + 1; db_init(&Yn.sh);
    int node = G->g0 + G->me; size_t lo, hi; mdb_share(&Yn, node, &lo, &hi); size_t cn = hi - lo;
    if (e >= N2) { ec_fatal(EC_RC_FATAL, "mdb_pow: B^%zu in basis %zu\n", e, N2); }
    db_zero_fill(&Yn.sh, cn);
    if (e >= lo && e < hi) { uint64_t one = 1; size_t gg = e - lo, d = (gg >= Yn.sh.qc) + (gg >= 2 * Yn.sh.qc) + (gg >= 3 * Yn.sh.qc); mem_dev_copy_on((int)d, Yn.sh.q[d] + (gg - d * Yn.sh.qc), &one, 8); }
    Yn.sh.n = cn;
    if (Y->sh.cap) db_free(&Y->sh);
    *Y = Yn;
}
/* sign of A - B (one basis): the highest node whose shares differ decides */
static int mdb_cmp(const mdb *A, const mdb *B, mn_group *G)
{
    if (A->N != B->N) { ec_fatal(EC_RC_FATAL, "mdb_cmp: bases %zu / %zu\n", A->N, B->N); }
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
    if (ms > 0x7fffffff) { ec_fatal(EC_RC_FATAL, "mdb_to_host_all: share too large\n"); }
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
    if (r->n > N2) { ec_fatal(EC_RC_FATAL, "mdb_from_db: %zu limbs in basis %zu\n", r->n, N2); }
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
/* Phase 13d L: the rule as a function of the group's size g and the machine's size (the plan printer asks it without groups);
 * x1_level below is the run's call, for node groups that start at node 0 */
int newton_mn_x1_level(size_t na, size_t nb, int g, int size, size_t reshard_limbs)
{
    static int on = -1; static double bw, lat, fixed, share;
    if (on < 0) {
        on = getenv("NEWTON_MN_GROUPS") ? atoi(getenv("NEWTON_MN_GROUPS")) : 1;
        int tcp = getenv("MN_MODEL_TCP") ? atoi(getenv("MN_MODEL_TCP")) : 0;
        bw = tcp ? 0.8 : 100.0; lat = tcp ? 0 : 2e-6; fixed = tcp ? 1e-3 : 0; share = tcp ? (double)size : 1.0;
        if (getenv("NEWTON_MN_BW")) bw = atof(getenv("NEWTON_MN_BW"));
        if (getenv("NEWTON_MN_LAT")) lat = atof(getenv("NEWTON_MN_LAT"));
        if (getenv("NEWTON_MN_FIXED")) fixed = atof(getenv("NEWTON_MN_FIXED"));
        if (getenv("NEWTON_MN_SHARE")) share = atof(getenv("NEWTON_MN_SHARE"));
    }
    if (!on || g <= 2) return 0;
    int best = 0; double bc = x1_cost(na, nb, g, reshard_limbs, bw, lat, fixed, share);
    for (int L = 1; (1 << L) < g; L++) { double c = x1_cost(na, nb, 1 << L, reshard_limbs, bw, lat, fixed, share); if (c < bc) { bc = c; best = L; } }
    return best;
}
static int x1_level(size_t na, size_t nb, mn_group *G, size_t reshard_limbs) { return G->g0 != 0 ? 0 : newton_mn_x1_level(na, nb, G->g, mn_size(), reshard_limbs); }
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
static void recip_mn_body(mdb *mu, const mdb *Q, size_t k, mn_group *G);
/* D3: the reciprocal's waits are their own phase (MN_WAIT_STATS) */
static void recip_mn(mdb *mu, const mdb *Q, size_t k, mn_group *G) { int pp = comm_wst_set_phase(WST_RECIP); recip_mn_body(mu, Q, k, G); comm_wst_set_phase(pp); }
/* the reciprocal mu of Q (k + 1 limbs) over G: the single-node chain up to the split precision, then the sharded steps */
static void recip_mn_body(mdb *mu, const mdb *Q, size_t k, mn_group *G)
{
    double t0 = mem_now();
    nv_init();
    size_t nq = Q->n;
    int me = G->me, verbose = getenv("RNS_VERBOSE") != 0;
    /* the anchored chain from k: k, ceil(k/2), ...; the single-node part ends at the largest target <= split (or the seed's 2) */
    size_t kp = newton_mn_chain_start(k);                              /* Phase 13d L: extracted (unchanged) */
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
    mn_group *Gs = 0; int member = 0, mid_ok = 0;                     /* R4: the first sharded round forms the whole product (the chain's last |d| is not carried over) */
    mdb r; memset(&r, 0, sizeof r);
    mdb qt, t, u, pw, d, corr, rs; memset(&qt, 0, sizeof qt); memset(&t, 0, sizeof t); memset(&u, 0, sizeof u); memset(&pw, 0, sizeof pw); memset(&d, 0, sizeof d); memset(&corr, 0, sizeof corr); memset(&rs, 0, sizeof rs);
    if (me == 0) printf("recip(mn): the single-node chain to %zu limbs (%.2f s), then the sharded steps to %zu over %d nodes\n", kp, t1 - t0, k, G->g);
    while (j < k) {
        size_t jn = newton_chain_next(j, k);                           /* Phase 13d L: extracted (unchanged) */
        size_t take = 2 * j + 2 < nq ? 2 * j + 2 : nq;
        int Ln = x1_level(take, j + 1, G, 0); mn_group *Gn = x1_group(Ln, G);
        if (Gn != Gs) {                                                /* the group grows (never shrinks: the products only get longer) */
            int was = member; member = x1_member(Gn);
            if (!Gs) { r.n = r.N = r0.n; r.g0 = Gn->g0; r.g = Gn->g; if (member) mdb_from_db(&r, &r0, r0.n, Gn); }   /* every node has r0: the members take their share */
            else if (member) x1_regroup(&r, Gn, was);
            Gs = Gn;
            mid_ok = 0;                                                /* R4: the new group's nodes must agree (a node that was not a member has no previous round); job 21475: size 3, 4 */
            if (nv && me == 0 && Gs != G) printf("newton(mn): j %zu on the group [0, %d)\n", j, Gs->g);
        }
        if (take == nq && Gs == G) ;                                   /* Q_t = Q itself (no shift copy) */
        else mdb_shift_g(&qt, Q, (long)(nq - take), take, G, 0, Ln ? 1 << Ln : G->g);   /* Q_t: the top limbs of Q, onto the step's group [0, 2^L) (once per step: a repeat reuses it) */
        if (!member) { j = jn; newton_st.iters++; continue; }
        for (;;) {
            double s0 = mem_now();
            size_t c1 = j <= take ? newton_recip_cut(take - j) : 0;   /* Phase 14 R1 (E7): the same cut over shares (R114.md 1.7) */
            size_t hw = mid_ok ? newton_recip_mid(j, take) : (size_t)-1;   /* Phase 15 R4: the middle product (R415.md 1.6) -- the result in basis hw */
            if (take == nq && Gs == G) { if (rns_dist_cache_hold(1)) mn_prod_cut(&t, &r, Q, Gs, c1, hw); else mn_prod_cut(&t, Q, &r, Gs, c1, hw); }   /* A1: Q's pieces' transforms may be kept for the division's X Q */
            else mn_prod_cut(&t, &qt, &r, Gs, c1, hw);                 /* Q_t r */
            int neg;
            if (hw != (size_t)-1) {                                    /* R4: u mod B^(j+4) = t >> (take - j) (t is mod B^(take+4)); d = |e| from it */
                mdb_shift(&u, &t, (long)take - (long)j, j + 5, Gs);
                if (u.n <= j + 3) { neg = u.n != 0; d = u; memset(&u, 0, sizeof u); }
                else { neg = 0; mdb_pow(&pw, j + 4, j + 5, Gs); mdb_addsub(&d, &pw, &u, 1, Gs); }
                if (d.n > j + 3) { ec_fatal(EC_RC_FATAL, "newton_mn: the middle product's bound failed at j %zu (|e| has %zu limbs)\n", j, d.n); }
                newton_mid_st.mid++;
            } else {
            mdb_shift(&u, &t, (long)take - (long)j, 2 * j + 2, Gs);    /* u ~ B^(2j) */
            neg = u.n > 2 * j + 1 || (u.n == 2 * j + 1 && (mdb_limb(&u, u.n - 1, Gs) > 1 || mdb_nonzero_below(&u, 2 * j, Gs)));
            mdb_pow(&pw, 2 * j, 2 * j + 2, Gs);
            if (neg) mdb_addsub(&d, &u, &pw, 1, Gs); else mdb_addsub(&d, &pw, &u, 1, Gs);   /* d = |B^(2j) - u| */
            newton_mid_st.whole++;
            }
            mfree(&u); mfree(&pw);
            mid_ok = d.n <= j + 1;                                     /* R4: as in recip_db2 */
            size_t NB = (r.n + j > t.n ? r.n + j : t.n) + 2;          /* the basis of r' (>= r << j and corr) */
            if (newton_mn_lean()) mfree(&t);                           /* int15k (DM_MN_LEAN): t = Q_t r is dead once u is formed (NB is computed) */
            if (d.n) { mn_prod_cut(&t, &r, &d, Gs, newton_recip_cut(j), (size_t)-1); mdb_shift(&corr, &t, (long)j, NB, Gs); if (newton_mn_lean()) mfree(&t); }   /* |corr| = r |d| >> j (E7: the pieces below j - guard skipped); lean: t dead once corr is formed */
            else { mfree(&t); mdb_shift(&corr, &r, (long)r.n + 1, NB, Gs); }              /* d = 0: corr = 0 */
            mfree(&d);
            int converged = corr.n <= j + 1;
            mdb_shift(&rs, &r, -(long)j, NB, Gs);                       /* r << j */
            if (neg) {
                int over = rs.n < corr.n || (rs.n == corr.n && mdb_cmp(&rs, &corr, Gs) <= 0);
                if (!over && recip_cut && rs.n == corr.n) {             /* E7 (R114.md 1.4): rs <= corr + 1 -- rs - corr == 1 also overshoots */
                    mdb df; memset(&df, 0, sizeof df); mdb_addsub(&df, &rs, &corr, 1, Gs); over = df.n == 1 && mdb_limb(&df, 0, Gs) == 1; mfree(&df); }
                if (over) {                                             /* overshoot: r -= r / 16, through the host (rare) */
                    newton_st.overshoots++; bigint h, dd; bi_init(&h); bi_init(&dd); mdb_to_host_all(&h, &r, Gs);
                    bi_divmod_u64(&dd, &h, 16); bi_sub(&h, &h, &dd); mdb_from_bi(&r, &h, r.N, Gs); bi_free(&h); bi_free(&dd);
                    mfree(&corr); mfree(&rs); mid_ok = 0; continue;
                }
                mdb_addsub(&rs, &rs, &corr, 1, Gs);
            } else mdb_addsub(&rs, &rs, &corr, 0, Gs);
            mfree(&corr);
            if (converged) { mfree(&r); r = rs; memset(&rs, 0, sizeof rs); }
            else { mdb_shift(&r, &rs, (long)j, rs.n > j ? rs.n - j : 1, Gs); mfree(&rs); }
            if (nv && me == 0) printf("newton(mn) j %zu -> %zu (k %zu): take %zu, r %zu limbs%s   %.2f s%s\n", j, jn, k, take, r.n, converged ? "" : " (repeat)", mem_now() - s0, Gs != G ? " (subgroup)" : "");
            if (mem_live_on() && me == 0) { char w_[64]; snprintf(w_, sizeof w_, "recip(mn) j %zu -> %zu", j, jn); mem_live_line(w_); }   /* Phase 14 S1 (E1) */
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
    mn_comm_mark("reciprocal");   /* Phase 16 D */
    if (verbose && me == 0) printf("recip(mn) %.2f s: single-node part %.2f, products %.2f, shifts %zu/%.2f, addsub %zu/%.2f, small %zu/%.2f\n", mem_now() - t0, t1 - t0, mn_st.t_prod, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub, mn_st.n_small, mn_st.t_small);
}
/* X = floor((P + Q) B^dl / Q) over G (P, Q consumed); pres/qres/rres: residues mod qs[nres] of P, Q and R = A - X Q;
 * t_recip: the reciprocal's seconds.  Mirrors newton_db_divmod_shifted with S = P + Q. */
void (*newton_mn_pq_hook)(int stage, mdb *x) = 0;   /* Phase 13 N (mn.c): 0 = before S = P + Q overwrites P, 1 = before Q is freed */
void (*newton_mn_x_hook)(mdb *X, void *arg) = 0; void *newton_mn_x_arg = 0;   /* Phase 15 IO (W5d): the part file streamed during the low product (ecalc.c, MN_OUT_EARLY) */
int (*newton_mn_xhi_hook)(mdb *Xh, size_t s, void *arg) = 0; int (*newton_mn_xlo_hook)(mdb *Xl, size_t s, int carry, void *arg) = 0;   /* Phase 15 EW (MN_OUT_DKM_HI): newton.h */
/* ---- Phase 15 DKM (results/DKM15.md 1.2): newton_mn_divmod in two quotient halves ------------------------------------------- */
/* Xo = ((Sx >> (nq - 1 - dl)) mu) >> (k + 1) in basis (its length + 1: room for the +1 corrections); mu has exactly k + 1 limbs */
static void mn_dkm_est(mdb *Xo, const mdb *Sx, size_t dl, size_t nq, size_t k, const mdb *mu, mn_group *G)
{
    mdb Ah, t; memset(&Ah, 0, sizeof Ah); memset(&t, 0, sizeof t);
    size_t sh = nq - 1 - dl;
    mdb_shift(&Ah, Sx, (long)sh, Sx->n > sh ? Sx->n - sh : 1, G);
    if (!Ah.n) { mdb_shift(Xo, &Ah, 0, 2, G); mfree(&Ah); return; }   /* X = 0 (the shift of a zero is a zero, collectively) */
    if (env_on("NEWTON_HIGHPROD")) mn_prod_cut_x1(&t, &Ah, mu, G, k + 1, (size_t)-1); else mn_prod(&t, &Ah, mu, G);
    mfree(&Ah);
    mdb_shift(Xo, &t, (long)(k + 1), (t.n > k + 1 ? t.n - (k + 1) : 1) + 1, G); mfree(&t);
}
/* mu's top m limbs (a copy in basis m; mu itself when it has exactly m) */
static void mn_dkm_top(mdb *Y, const mdb *mu, size_t m, mn_group *G) { mdb_shift(Y, mu, (long)(mu->n - m), m, G); }
/* int15k (DM_MN_LEAN): mn_dkm_est with A_h = Sx >> sh and mu's top m limbs as views (no copies): the same product on the same limbs.
 * The X1 rule (mn_prod_cut_x1: a subgroup for small products on many nodes) needs operands it can re-shard, so where it would take a
 * subgroup the copy path runs as before (never at the target's sizes) */
static void mn_dkm_est_lean(mdb *Xo, const mdb *Sx, size_t dl, size_t nq, size_t k, const mdb *mu, size_t m, mn_group *G)
{
    size_t sh = nq - 1 - dl;
    mdbv a = mdb_view(Sx, sh, Sx->n > sh ? Sx->n - sh : 0, G), b = mdb_view(mu, mu->n - m, m, G);
    int hp = env_on("NEWTON_HIGHPROD");
    if (a.len && b.len && hp && x1_level(a.len, b.len, G, 2 * (a.len + b.len))) {   /* the X1 rule wants a subgroup: the copy path */
        mdb mt; memset(&mt, 0, sizeof mt); mn_dkm_top(&mt, mu, m, G); mn_dkm_est(Xo, Sx, dl, nq, k, &mt, G); mfree(&mt); return; }
    mdb t; memset(&t, 0, sizeof t);
    if (!a.len) { mdb z; memset(&z, 0, sizeof z); db_init(&z.sh); z.g0 = G->g0; z.g = G->g; mdb_shift(Xo, &z, 0, 2, G); mfree(&z); return; }   /* X = 0 */
    double t0 = mem_now();
    if (hp) rns_mul_dist_mn_cut_v(&t, &a, &b, G, k + 1, (size_t)-1); else rns_mul_dist_mn_v(&t, &a, &b, 0, G, (size_t)-1);
    mn_st.t_prod += mem_now() - t0;
    mdb_shift(Xo, &t, (long)(k + 1), (t.n > k + 1 ? t.n - (k + 1) : 1) + 1, G); mfree(&t);
}
/* the low product X Q mod B^w in basis w (as newton_mn_divmod: A5's cut, NEWTON_LOWPROD=0 the full product); X = 0: a zero */
static void mn_dkm_low(mdb *xql, const mdb *Xn, mdb *Q, size_t w, mn_group *G)
{
    mdb xq; memset(&xq, 0, sizeof xq);
    if (!Xn->n) { mdb_shift(xql, Xn, 0, w, G); return; }
    if (env_on("NEWTON_LOWPROD")) { mn_prod_cut_x1(xql, Xn, Q, G, 0, w); if (xql->N != w) { mdb_shift(&xq, xql, 0, w, G); mfree(xql); *xql = xq; } }
    else { mn_prod(&xq, Xn, Q, G); mdb_shift(xql, &xq, 0, w, G); mfree(&xq); }
}
/* R from the window and xq (both basis w, consumed): newton_mn_divmod's corrections; returns the change to X */
static long mn_dkm_corr(mdb *Rd, mdb *Aw, mdb *xql, const mdb *Qw, mn_group *G, const char *who)
{
    size_t nc = 0; long dx = 0;
    if (mdb_cmp(Aw, xql, G) >= 0) {
        mdb_addsub(Aw, Aw, xql, 1, G); mfree(xql); *Rd = *Aw; memset(Aw, 0, sizeof *Aw);
        while (mdb_cmp(Rd, Qw, G) >= 0) { mdb_addsub(Rd, Rd, Qw, 1, G); dx++; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod (DKM %s): %zu corrections\n", who, nc); } }
    } else {
        mdb_addsub(xql, xql, Aw, 1, G); mfree(Aw); *Rd = *xql; memset(xql, 0, sizeof *xql);
        for (;;) { dx--; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod (DKM %s): %zu corrections\n", who, nc); }
                   if (mdb_cmp(Rd, Qw, G) <= 0) { mdb_addsub(Rd, Qw, Rd, 1, G); break; } mdb_addsub(Rd, Rd, Qw, 1, G); }
    }
    return dx;
}
static void mn_dkm_add(mdb *Y, long dx, mn_group *G) { if (dx) mdb_add_val(Y, 0, (uint64_t)(dx < 0 ? -dx : dx), dx < 0, G); }
static void mn_divmod_dkm(mdb *X, mdb *P, mdb *Q, size_t dl, struct mn_group *G, const uint64_t *qs, int nres, uint64_t *pres, uint64_t *qres, uint64_t *rres, double *t_recip)
{
    double t0 = mem_now(); int me = G->me;
    memset(&mn_st, 0, sizeof mn_st);
    size_t nq = Q->n;
    mdb_mod_qs(P, qs, nres, pres, G); mdb_mod_qs(Q, qs, nres, qres, G);
    size_t na_est = P->n + 1 + dl, k_mu = na_est - nq + 1, h_mu = newton_dkm_h(k_mu);
    mdb mu; memset(&mu, 0, sizeof mu);
    recip_mn(&mu, Q, h_mu, G);                                         /* the chain to h: one doubling fewer */
    double ta = mem_now(); *t_recip = ta - t0;
    double p_rec = mn_st.t_prod;
    mdb Qp, S; memset(&Qp, 0, sizeof Qp); memset(&S, 0, sizeof S);
    mdb_shift(&Qp, Q, 0, P->N, G);
    if (newton_mn_pq_hook) newton_mn_pq_hook(0, P);
    mdb_addsub(P, P, &Qp, 0, G); mfree(&Qp); S = *P; memset(P, 0, sizeof *P);
    size_t na = S.n + dl, k = na - nq + 1, w = nq + 2;
    if (!nq || na < nq) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: A < Q not supported\n"); }
    if (dl + 1 > nq) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: dl >= nq\n"); }
    size_t s = k / 2 < dl ? k / 2 : dl, k1 = k - s, h = k1 > s + 1 ? k1 : s + 1;
    if (!s) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod (DKM): k %zu dl %zu leave no low half\n", k, dl); }
    if (mu.n < h + 1) { if (me == 0) printf("divmod(mn) DKM: mu has %zu limbs, h %zu: a fresh reciprocal\n", mu.n, h); mfree(&mu); recip_mn(&mu, Q, h, G); }
    double tb = mem_now();
    /* step 1: X_hi, R1 of A >> s = S B^(dl - s) */
    mdb m1, Xh, Aw, Qw, xql, R1, Xl, Xs, Xl2, Xn, Rd;
    memset(&m1, 0, sizeof m1); memset(&Xh, 0, sizeof Xh); memset(&Aw, 0, sizeof Aw); memset(&Qw, 0, sizeof Qw); memset(&xql, 0, sizeof xql); memset(&R1, 0, sizeof R1);
    memset(&Xl, 0, sizeof Xl); memset(&Xs, 0, sizeof Xs); memset(&Xl2, 0, sizeof Xl2); memset(&Xn, 0, sizeof Xn); memset(&Rd, 0, sizeof Rd);
    const int lean = newton_mn_lean();                                 /* int15k (DM_MN_LEAN): views instead of copies, Qw in place of Q */
    if (lean) mn_dkm_est_lean(&Xh, &S, dl - s, nq, k1, &mu, k1 + 1, G);
    else { mn_dkm_top(&m1, &mu, k1 + 1, G); mn_dkm_est(&Xh, &S, dl - s, nq, k1, &m1, G); mfree(&m1); }
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 1 A mu");   /* int15k: the division's moments (ECALC_LIVE) */
    { long th = dkm_test_hi(); if (th) mn_dkm_add(&Xh, -th, G); }     /* NEWTON_DKM_TEST_HI (a borrow out of X_hi is fatal in mdb_add_val) */
    mdb_shift(&Aw, &S, -(long)(dl - s), w, G); mfree(&S);              /* the window (S's last use) */
    mdb_shift(&Qw, Q, 0, w, G);
    mdb *Qd = Q;                                                       /* the low products' Q: lean = Qw (the same limbs in basis w; Q freed now, the hook as before) */
    if (lean) { if (newton_mn_pq_hook) newton_mn_pq_hook(1, Q); mfree(Q); Qd = &Qw; }
    mn_dkm_low(&xql, &Xh, Qd, w, G);
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 1 X_hi Q");
    long dx1 = mn_dkm_corr(&R1, &Aw, &xql, &Qw, G, "step 1");
    mn_dkm_add(&Xh, dx1, G);                                           /* X_hi exact (no one has seen it) */
    newton_st.dkm_corr += (size_t)(dx1 < 0 ? -dx1 : dx1);
    /* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3) -- the X_hi hook (every rank, the same point: the hook runs a collective): X_hi is
     * final here; the hook takes it and starts the part files of X_hi's shares.  No X0 below: the division returns X_lo */
    int xhi = newton_mn_xhi_hook && newton_mn_xlo_hook && newton_x_defer;
    if (xhi) xhi = newton_mn_xhi_hook(&Xh, s, newton_mn_x_arg);
    double tc = mem_now();
    /* step 2: X_lo, R of A2 = R1 B^s */
    size_t na2 = R1.n + s, k2 = na2 >= nq ? na2 - nq + 1 : 0;
    if (na2 >= nq && lean) { mn_dkm_est_lean(&Xl, &R1, s, nq, k2, &mu, k2 + 1, G); mfree(&mu); }
    else if (na2 >= nq) { mdb m2; memset(&m2, 0, sizeof m2); if (mu.n == k2 + 1) { m2 = mu; memset(&mu, 0, sizeof mu); } else { mn_dkm_top(&m2, &mu, k2 + 1, G); mfree(&mu); }
                     mn_dkm_est(&Xl, &R1, s, nq, k2, &m2, G); mfree(&m2); }
    else { mfree(&mu); mdb_shift(&Xl, &R1, (long)R1.n + 1, 2, G); }      /* A2 < Q: X_lo = 0 */
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 2 A mu");
    mdb_shift(&Aw, &R1, -(long)s, w, G); mfree(&R1);
    { long tk = newton_test_corr(); if (tk) mn_dkm_add(&Xl, -tk, G); }  /* Phase 15 K: ECALC_TEST_CORR on X_lo (X - k; a borrow out of X_lo is fatal) */
    if (xhi) {   /* Phase 15 EW -- the X_lo hook (every rank; carry = X_lo0 >= B^s is the group's): released onto X_lo0 unless carry, then the
                  * corrections deferred as today; else added to X_lo0 in place.  X_lo is kept (the division's result) */
        int carry = Xl.n > s;
        newton_x_dx = 0;
        int rel = newton_mn_xlo_hook(&Xl, s, carry, newton_mn_x_arg);
        double td = mem_now();
        mn_dkm_low(&xql, &Xl, Qd, w, G);
        if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 2 X_lo Q");
        rns_dist_cache_hold(0); rns_dist_cache_release();
        if (!lean) { if (newton_mn_pq_hook) newton_mn_pq_hook(1, Q); mfree(Q); }   /* (lean: freed after the window) */
        double te = mem_now();
        long dx = mn_dkm_corr(&Rd, &Aw, &xql, &Qw, G, "step 2");
        if (dx < 0) newton_st.down_corr += (size_t)(-dx); else newton_st.up_corr += (size_t)dx;
        if (dx && rel) newton_x_dx = dx; else mn_dkm_add(&Xl, dx, G);
        mfree(&Qw);
        double tf = mem_now();
        if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) end");
        mdb_mod_qs(&Rd, qs, nres, rres, G);
        mfree(&Rd);
        if (X->sh.cap) db_free(&X->sh);
        *X = Xl;
        newton_st.t_div += mem_now() - ta;
        mn_comm_mark("division (after the reciprocal)");   /* Phase 16 D */
        if (me == 0) printf("divmod(mn, DKM, X_hi writer) %.2f s: k %zu = %zu + %zu (s), h %zu; reciprocal %.2f (products %.2f), S = P + Q %.2f, step 1 (A mu, X_hi Q, corrections %ld) %.2f, step 2 A mu %.2f, X_lo Q %.2f, corrections %.2f (%ld, %s), R residues %.2f; X_lo0 %s B^s; division products %.2f s; shifts %zu/%.2f s, addsub %zu/%.2f s\n",
                            mem_now() - t0, k, k1, s, h, ta - t0, p_rec, tb - ta, dx1, tc - tb, td - tc, te - td, tf - te, dx, rel ? "deferred" : "in place", mem_now() - tf, carry ? ">=" : "<", mn_st.t_prod - p_rec, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub);
        return;
    }
    {   /* X0 = X_hi B^s + X_lo0, then in the basis of its length (as today's X) */
        size_t NX = (Xh.n + s > Xl.n ? Xh.n + s : Xl.n) + 1;
        mdb_shift(&Xs, &Xh, -(long)s, NX, G); mfree(&Xh);
        mdb_shift(&Xl2, &Xl, 0, NX, G);
        mdb_addsub(&Xs, &Xs, &Xl2, 0, G); mfree(&Xl2);
        mdb_shift(&Xn, &Xs, 0, Xs.n ? Xs.n : 1, G); mfree(&Xs);
    }
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 2 assembly");
    newton_x_dx = 0;
    if (newton_mn_x_hook) newton_mn_x_hook(&Xn, newton_mn_x_arg);     /* Phase 15 IO (W5d): the writer starts on X0, before step 2's low product */
    double td = mem_now();
    mn_dkm_low(&xql, &Xl, Qd, w, G); mfree(&Xl);
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) step 2 X_lo Q");
    rns_dist_cache_hold(0); rns_dist_cache_release();
    if (!lean) { if (newton_mn_pq_hook) newton_mn_pq_hook(1, Q); mfree(Q); }   /* (lean: freed after the window) */
    double te = mem_now();
    long dx = mn_dkm_corr(&Rd, &Aw, &xql, &Qw, G, "step 2");
    if (dx < 0) newton_st.down_corr += (size_t)(-dx); else newton_st.up_corr += (size_t)dx;
    if (dx && newton_x_defer) newton_x_dx = dx;                       /* Phase 15 K (ECALC_CORR_PATCH=2) */
    else if (dx) mdb_add_val(&Xn, 0, (uint64_t)(dx < 0 ? -dx : dx), dx < 0, G);
    mfree(&Qw);
    double tf = mem_now();
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn, DKM) end");
    mdb_mod_qs(&Rd, qs, nres, rres, G);
    mfree(&Rd);
    if (X->sh.cap) db_free(&X->sh);
    *X = Xn;
    newton_st.t_div += mem_now() - ta;
    mn_comm_mark("division (after the reciprocal)");   /* Phase 16 D */
    if (me == 0) printf("divmod(mn, DKM) %.2f s: k %zu = %zu + %zu (s), h %zu; reciprocal %.2f (products %.2f), S = P + Q %.2f, step 1 (A mu, X_hi Q, corrections %ld) %.2f, step 2 A mu + assembly %.2f, X_lo Q %.2f, corrections %.2f (%ld), R residues %.2f; division products %.2f s; shifts %zu/%.2f s, addsub %zu/%.2f s\n",
                        mem_now() - t0, k, k1, s, h, ta - t0, p_rec, tb - ta, dx1, tc - tb, td - tc, te - td, tf - te, dx, mem_now() - tf, mn_st.t_prod - p_rec, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub);
    if (me == 0) printf("scratch(mn): mdb_shift slabs %.3f GB per node-process at most (MDB_SHIFT_CHUNK_MB=%s), window temporaries T + rbO %.3f GB (MN_T_CHUNK_MB=%s)\n",
                        mn_st.shift_max * 1e-9, getenv("MDB_SHIFT_CHUNK_MB") ? getenv("MDB_SHIFT_CHUNK_MB") : "1024", rns_dist_tscratch_max * 1e-9, getenv("MN_T_CHUNK_MB") ? getenv("MN_T_CHUNK_MB") : "1024");
}
void newton_mn_divmod(mdb *X, mdb *P, mdb *Q, size_t dl, struct mn_group *G, const uint64_t *qs, int nres, uint64_t *pres, uint64_t *qres, uint64_t *rres, double *t_recip)
{
    if (newton_dkm_on() && dl >= 1 && P->n + 1 + dl >= Q->n + 4) { mn_divmod_dkm(X, P, Q, dl, G, qs, nres, pres, qres, rres, t_recip); return; }   /* Phase 15 DKM (k_mu >= 5) */
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
    if (newton_mn_pq_hook) newton_mn_pq_hook(0, P);   /* Phase 13 N (1.4): P is overwritten next -- the background top set's writer lets go of it */
    mdb_addsub(P, P, &Qp, 0, G); mfree(&Qp); S = *P; memset(P, 0, sizeof *P);
    size_t na = S.n + dl, k = na - nq + 1, w = nq + 2;
    if (!nq || na < nq) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: A < Q not supported\n"); }
    if (dl + 1 > nq) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: dl >= nq\n"); }
    if (mu.n < k + 1) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: mu has %zu limbs, k %zu\n", mu.n, k); }
    if (mu.n > k + 1) { mdb m2; memset(&m2, 0, sizeof m2); mdb_shift(&m2, &mu, (long)(mu.n - (k + 1)), k + 1, G); mfree(&mu); mu = m2; }
    double tb = mem_now();
    /* X = ((S >> (nq - 1 - dl)) mu) >> (k + 1) */
    mdb Ah, t, Xn; memset(&Ah, 0, sizeof Ah); memset(&t, 0, sizeof t); memset(&Xn, 0, sizeof Xn);
    { size_t sh = nq - 1 - dl; mdb_shift(&Ah, &S, (long)sh, S.n > sh ? S.n - sh : 1, G); }
    if (env_on("NEWTON_HIGHPROD")) mn_prod_cut_x1(&t, &Ah, &mu, G, k + 1, (size_t)-1);   /* A5/B3: the pieces below the cut k + 1 skipped */
    else mn_prod(&t, &Ah, &mu, G);
    mfree(&Ah); mfree(&mu);
    mdb_shift(&Xn, &t, (long)(k + 1), t.n > k + 1 ? t.n - (k + 1) : 1, G); mfree(&t);
    { long tk = newton_test_corr(); if (tk) mdb_add_val(&Xn, 0, (uint64_t)(tk < 0 ? -tk : tk), tk > 0, G); }   /* Phase 15 K: ECALC_TEST_CORR (X - k) */
    newton_x_dx = 0;
    if (newton_mn_x_hook) newton_mn_x_hook(&Xn, newton_mn_x_arg);   /* Phase 15 IO (W5d): every rank, the same point (the hook may run a collective) */
    double tc = mem_now();
    /* the low product X Q mod B^w (A5: the grid with the pieces above w skipped, delivered in basis w), the window
     * A mod B^w = (S mod B^(w - dl)) B^dl, both in basis w; Q in basis w for the corrections */
    mdb xq, xql, Aw, Qw, Rd; memset(&xq, 0, sizeof xq); memset(&xql, 0, sizeof xql); memset(&Aw, 0, sizeof Aw); memset(&Qw, 0, sizeof Qw); memset(&Rd, 0, sizeof Rd);
    if (env_on("NEWTON_LOWPROD")) { mn_prod_cut_x1(&xql, &Xn, Q, G, 0, w); if (xql.N != w) { mdb_shift(&xq, &xql, 0, w, G); mfree(&xql); xql = xq; memset(&xq, 0, sizeof xq); } }   /* (the basis is w unless the product was shorter) */
    else { mn_prod(&xq, &Xn, Q, G); mdb_shift(&xql, &xq, 0, w, G); mfree(&xq); }
    rns_dist_cache_hold(0); rns_dist_cache_release();                 /* A1: Q's kept transforms served the low product; the planes go */
    mdb_shift(&Aw, &S, -(long)dl, w, G); mfree(&S);
    mdb_shift(&Qw, Q, 0, w, G);
    if (newton_mn_pq_hook) newton_mn_pq_hook(1, Q);   /* Phase 13 N (4.1): the hook may take Q's share (Q->sh zeroed) and free it after the output stage */
    mfree(Q);
    double td = mem_now();
    size_t nc = 0; long dx = 0;
    if (mdb_cmp(&Aw, &xql, G) >= 0) {                                 /* R = Aw - xq >= 0; while R >= Q: R -= Q, X += 1 */
        mdb_addsub(&Aw, &Aw, &xql, 1, G); mfree(&xql); Rd = Aw; memset(&Aw, 0, sizeof Aw);
        while (mdb_cmp(&Rd, &Qw, G) >= 0) { mdb_addsub(&Rd, &Rd, &Qw, 1, G); dx++; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: %zu corrections\n", nc); } }
    } else {                                                          /* D = xq - Aw > 0: X -= 1, R = Q - D; while D > Q: D -= Q, X -= 1 */
        mdb_addsub(&xql, &xql, &Aw, 1, G); mfree(&Aw); Rd = xql; memset(&xql, 0, sizeof xql);
        for (;;) { dx--; if (++nc > 64) { ec_fatal(EC_RC_FATAL, "newton_mn_divmod: %zu corrections\n", nc); }
                   if (mdb_cmp(&Rd, &Qw, G) <= 0) { mdb_addsub(&Rd, &Qw, &Rd, 1, G); break; } mdb_addsub(&Rd, &Rd, &Qw, 1, G); }
        newton_st.down_corr += (size_t)(-dx);
    }
    if (dx > 0) newton_st.up_corr += (size_t)dx;
    if (dx && newton_x_defer) newton_x_dx = dx;                       /* Phase 15 K (ECALC_CORR_PATCH=2): the output patches the part files' tail */
    else if (dx) mdb_add_val(&Xn, 0, (uint64_t)(dx < 0 ? -dx : dx), dx < 0, G);
    mfree(&Qw);
    double te = mem_now();
    if (mem_live_on() && me == 0) mem_live_line("divmod(mn) end");   /* Phase 14 S1 (E1) */
    mdb_mod_qs(&Rd, qs, nres, rres, G);
    mfree(&Rd);
    if (X->sh.cap) db_free(&X->sh);
    *X = Xn;
    newton_st.t_div += mem_now() - ta;
    mn_comm_mark("division (after the reciprocal)");   /* Phase 16 D */
    if (me == 0) printf("divmod(mn) %.2f s: reciprocal %.2f (products %.2f), S = P + Q %.2f, A mu + shift %.2f, X Q + window %.2f, corrections %.2f (%ld), R residues %.2f; division products %.2f s; shifts %zu/%.2f s, addsub %zu/%.2f s\n",
                        mem_now() - t0, ta - t0, p_rec, tb - ta, tc - tb, td - tc, te - td, dx, mem_now() - te, mn_st.t_prod - p_rec, mn_st.n_shift, mn_st.t_shift, mn_st.n_addsub, mn_st.t_addsub);
    if (me == 0) printf("scratch(mn): mdb_shift slabs %.3f GB per node-process at most (MDB_SHIFT_CHUNK_MB=%s), window temporaries T + rbO %.3f GB (MN_T_CHUNK_MB=%s)\n",
                        mn_st.shift_max * 1e-9, getenv("MDB_SHIFT_CHUNK_MB") ? getenv("MDB_SHIFT_CHUNK_MB") : "1024", rns_dist_tscratch_max * 1e-9, getenv("MN_T_CHUNK_MB") ? getenv("MN_T_CHUNK_MB") : "1024");   /* Phase 13a M */
}
