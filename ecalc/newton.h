/* newton.h - reciprocal by Newton iteration and Barrett-style division
 * (PLAN.md 8, step 5).  All big products go through rns_mul.
 *
 *   newton_recip(mu, Q, k)   mu ~ floor(2^(64 (nq + k)) / Q), k limbs of
 *                            precision (mu has k + 1 limbs), |error| <= a few
 *                            units.  Doubling from a schoolbook seed in the
 *                            correction form: u = (Q_t r) / B^(take - j),
 *                            d = B^(2j) - u (B = 2^64 or 10^18), r' = (r << 64 j) + (r d >> 64 j)
 *                            (j -> 2j) - products Q r and r d instead of the
 *                            paper's r^2 and Q_t r^2 (3 mdevs instead of 4 plus
 *                            a split at the top).  Overshoot (r' <= 0) shrinks
 *                            r by 1/16 and retries; a step whose correction is
 *                            not below 2^(64 j) repeats at the same precision.
 *   newton_divmod(X, R, A, Q [, mu])   X = floor(A / Q), R = A - X Q, using
 *                            mu with k = na - nq + 1 (computed if not given);
 *                            X0 = ((A >> 64(nq-1)) mu) >> 64(k + 1) (the dropped
 *                            low limbs of A are under one unit), then down-corrections
 *                            while X Q > A and up-corrections while R >= Q;
 *                            the correction counts are reported in newton_st.
 *   bi_divmod_school(X, R, A, Q)   Knuth D, for seeds and tests.
 *
 * Deviations from the paper, kept as knobs: the r^2 truncation
 * (NEWTON_R2TRUNC) and the high-product-only A mu are not implemented -
 * full products are used (Phase 5 candidates, the paper's dm is 46.8 s).
 */
#ifndef EC_NEWTON_H
#define EC_NEWTON_H
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { size_t iters, overshoots, repeats, down_corr, up_corr; double t_recip, t_div; size_t dkm_corr; } newton_stats;   /* dkm_corr: NEWTON_DKM's step-1 corrections (not in down/up) */
/* Phase 15 DKM (results/DKM15.md): NEWTON_DKM=1 (off by default) -- the division in two quotient halves with a half-length reciprocal
 * (GMP mu_div): mu to h = floor(k/2) + 1 limbs; step 1 = the shifted division of A >> s (s = min(floor(k/2), dl)) by Q, exact (its own
 * corrections, applied to X_hi); step 2 = the shifted division of R1 B^s; X = X_hi B^s + X_lo.  The hook, ECALC_TEST_CORR and the
 * deferred corrections act at step 2 as they do today.  NEWTON_DKM_TEST_HI=<k> (a test hook, |k| <= 60) moves X_hi by -k before
 * step 1's corrections.  Size 1: newton_db_divmod_shifted (and newton_db_recip when newton_db_Qd is set: the device flow's prewarm);
 * size > 1: newton_mn_divmod */
int newton_dkm_on(void);
void newton_dkm_set(int on);                          /* tests: override NEWTON_DKM */
size_t newton_dkm_h(size_t k);                        /* the reciprocal's length under DKM for a quotient of k limbs */
int newton_mn_lean(void);                             /* int15k: DM_MN_LEAN (off) -- the multi-node reciprocal / DKM division without dead copies; dm_layout counts the lean set */
extern newton_stats newton_st;
extern int newton_seed_perturb;      /* test hook: multiply the seed by this/16 (0 = off) */
void newton_seed_host(bigint *r, const bigint *Q, size_t *j);
void newton_seed_top(bigint *r, const uint64_t *top4, size_t top, size_t nq, size_t *j);   /* from Q's top limbs (I3) */
/* WP5: the same on device-resident numbers (newton_db.c); NEWTON_DEVICE=1 in ecalc selects them */
void newton_db_recip(bigint *mu, const bigint *Q, size_t k);
void newton_db_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt);
void newton_db_free_scratch(void);
/* Phase 15 R4 (NEWTON_RECIP_MID): the reciprocal's Q_t r as a middle product; counts of the rounds that took it / the whole product,
 * and a test hook overriding NEWTON_RECIP_CUT / NEWTON_RECIP_MID (-1 keeps a switch) */
struct newton_mid_stats { size_t mid, whole; };
extern struct newton_mid_stats newton_mid_st;
void newton_recip_set(int cut, int mid);
extern int newton_db_free_inputs;
void newton_db_divmod_shifted(bigint *X, const struct dbig_s *S, size_t dl, const struct dbig_s *Qd, const uint64_t *qs, int nres, uint64_t *rres);   /* I3: A = S B^dl, all on device; R's residues out */
extern struct dbig_s *newton_db_Qd;                  /* Phase 8: Q already on device (owned by the caller) */
extern int newton_db_mu_host;                        /* 0: no host copy of mu after the reciprocal */
extern void (*newton_db_x_hook)(bigint *X, void *arg); extern void *newton_db_x_arg;   /* X on the host before the low product */
extern struct dbig_s *newton_db_x_dev;               /* Phase 10 H (B1): when set, X stays on the device and is returned here (corrected in place); no host X */
/* Phase 15 K (ECALC_CORR_PATCH): newton_x_defer = 1 -- the division's +/-1 corrections are NOT applied to X once the hook has
 * started the writer on it (size 1) / at all (newton_mn_divmod, size > 1): their sum comes back in newton_x_dx and the output
 * stage patches the written digits' tail instead (mn_out_tail_fix).  newton_test_corr(): ECALC_TEST_CORR=<k> (a test hook,
 * |k| <= 60) moves the division's X by -k before the corrections, which then make k more up- (k > 0) or down- (k < 0) corrections */
extern int newton_x_defer; extern long newton_x_dx;
long newton_test_corr(void);
/* Phase 9 M4 (A-div): the reciprocal and division over sharded numbers (mdb over the top-level group G): X = floor((P + Q) B^dl / Q)
 * stays sharded; P, Q are consumed; the residues of P, Q and R mod qs[nres] come back; t_recip = the reciprocal's seconds */
struct mdb_s; struct mn_group;
extern void (*newton_mn_pq_hook)(int stage, struct mdb_s *x);   /* Phase 13 N: called before S = P + Q overwrites P (0) and before Q is freed (1; may take Q->sh) */
extern void (*newton_mn_x_hook)(struct mdb_s *X, void *arg); extern void *newton_mn_x_arg;   /* Phase 15 IO (W5d, MN_OUT_EARLY): X over the group before the low product (corrections may still change it) */
void newton_mn_divmod(struct mdb_s *X, struct mdb_s *P, struct mdb_s *Q, size_t dl, struct mn_group *G, const uint64_t *qs, int nres, uint64_t *pres, uint64_t *qres, uint64_t *rres, double *t_recip);
/* Phase 15 EW (MN_OUT_DKM_HI, results/EW15.md 1.3): the writer on X_hi after DKM's step 1.  Set (by ecalc.c) with the corrections
 * deferred (newton_x_defer) -- size 1 also with newton_db_x_dev -- the DKM division forms no X0: the xhi hook gets X_hi right after
 * step 1's corrections (final: X's limbs >= s) and takes it (*Xh left empty); the xlo hook gets X_lo0 before step 2's low product
 * (size 1: moved into newton_db_x_dev first), carry = X_lo0 >= B^s, and returns 1 when it released the writer onto X_lo0 -- then the
 * step-2 corrections are deferred (newton_x_dx) as today, else they are added to X_lo0 in place (newton_x_dx = 0).  X = X_hi B^s +
 * X_lo: the division returns X_lo (newton_db_x_dev / *X); X_hi is the hook's */
extern int (*newton_db_xhi_hook)(struct dbig_s *Xh, size_t s, void *arg);   /* returns 1 when it took X_hi (0: the division goes on as without the hooks) */
extern int (*newton_db_xlo_hook)(struct dbig_s *Xl, size_t s, int carry, void *arg);
extern int (*newton_mn_xhi_hook)(struct mdb_s *Xh, size_t s, void *arg);
extern int (*newton_mn_xlo_hook)(struct mdb_s *Xl, size_t s, int carry, void *arg);

void bi_divmod_school(bigint *X, bigint *R, const bigint *A, const bigint *Q);
void newton_recip(bigint *mu, const bigint *Q, size_t k);
/* start from seed_r ~ 2^(64 (nq + seed_j)) / Q with seed_j limbs of precision (any accuracy: steps repeat until they converge) */
void newton_recip_seeded(bigint *mu, const bigint *Q, size_t k, const bigint *seed_r, size_t seed_j);
void newton_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt);
void newton_free_scratch(void);   /* release the grow-only temporaries */
#ifdef __cplusplus
}
#endif
#endif
