/* newton.h - reciprocal by Newton iteration and Barrett-style division
 * (ALGORITHM.md §5), on device-resident numbers.  newton.c keeps the host pieces the device flow uses:
 *
 *   newton_seed_host / newton_seed_top   the seed r ~ B^(nq + 2) / Q (2 limbs of
 *                            precision) by schoolbook on the top 4 limbs of Q.
 *   bi_divmod_school(X, R, A, Q)   Knuth D, for seeds and tests.
 *
 *   newton_db_recip(mu, Q, k)   mu ~ floor(B^(nq + k) / Q), k limbs of precision
 *                            (mu has k + 1 limbs), |error| <= a few units; doubling
 *                            from the seed in the correction form.  Overshoot shrinks
 *                            r by 1/16 and retries; a step whose correction is not
 *                            below B^j repeats at the same precision.
 *   newton_db_divmod(X, R, A, Q [, mu])   X = floor(A / Q), R = A - X Q, then
 *                            down/up-corrections; the counts are reported in newton_st.
 */
#ifndef EC_NEWTON_H
#define EC_NEWTON_H
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { size_t iters, overshoots, repeats, down_corr, up_corr; double t_recip, t_div; size_t dkm_corr; } newton_stats;   /* dkm_corr: NEWTON_DKM's step-1 corrections (not in down/up) */
/* NEWTON_DKM=1 (default; newton_db.c) -- the division in two quotient halves with a half-length reciprocal
 * (GMP mu_div): mu to h = floor(k/2) + 1 limbs; step 1 = the shifted division of A >> s (s = min(floor(k/2), dl)) by Q, exact (its own
 * corrections, applied to X_hi); step 2 = the shifted division of R1 B^s; X = X_hi B^s + X_lo.  The hook, ECALC_TEST_CORR and the
 * deferred corrections act at step 2 as they do today.  NEWTON_DKM_TEST_HI=<k> (a test hook, |k| <= 60) moves X_hi by -k before
 * step 1's corrections.  Size 1: newton_db_divmod_shifted (and newton_db_recip when newton_db_Qd is set: the device flow's prewarm);
 * size > 1: newton_mn_divmod */
int newton_dkm_on(void);
void newton_dkm_set(int on);                          /* tests: the division in two quotient halves off / on (on by default) */
size_t newton_dkm_h(size_t k);                        /* the reciprocal's length under DKM for a quotient of k limbs */
extern newton_stats newton_st;
extern int newton_seed_perturb;      /* test hook: multiply the seed by this/16 (0 = off) */
void newton_seed_host(bigint *r, const bigint *Q, size_t *j);
void newton_seed_top(bigint *r, const uint64_t *top4, size_t top, size_t nq, size_t *j);   /* from Q's top limbs (I3) */
/* WP5: on device-resident numbers (newton_db.c) */
void newton_db_recip(bigint *mu, const bigint *Q, size_t k);
void newton_db_divmod(bigint *X, bigint *R, const bigint *A, const bigint *Q, const bigint *mu_opt);
void newton_db_free_scratch(void);
/* the reciprocal's Q_t r as a middle product (always); counts of the rounds that took it / the whole product */
struct newton_mid_stats { size_t mid, whole; };
extern struct newton_mid_stats newton_mid_st;
extern int newton_db_free_inputs;
struct dbig_s;                                       /* declared at file scope (ROCm 7.0.3's clang warns -Wvisibility otherwise) */
void newton_db_divmod_shifted(bigint *X, const struct dbig_s *S, size_t dl, const struct dbig_s *Qd, const uint64_t *qs, int nres, uint64_t *rres);   /* I3: A = S B^dl, all on device; R's residues out */
extern struct dbig_s *newton_db_Qd;                  /* Q already on device (owned by the caller) */
extern int newton_db_mu_host;                        /* 0: no host copy of mu after the reciprocal */
extern void (*newton_db_x_hook)(bigint *X, void *arg); extern void *newton_db_x_arg;   /* X on the host before the low product */
extern struct dbig_s *newton_db_x_dev;               /* when set, X stays on the device and is returned here (corrected in place); no host X */
/* newton_x_defer = 1 -- the division's +/-1 corrections are NOT applied to X once the hook has
 * started the writer on it (size 1) / at all (newton_mn_divmod, size > 1): their sum comes back in newton_x_dx and the output
 * stage patches the written digits' tail instead (mn_out_tail_fix).  newton_test_corr(): ECALC_TEST_CORR=<k> (a test hook,
 * |k| <= 60) moves the division's X by -k before the corrections, which then make k more up- (k > 0) or down- (k < 0) corrections */
extern int newton_x_defer; extern long newton_x_dx;
long newton_test_corr(void);
/* the reciprocal and division over sharded numbers (mdb over the top-level group G): X = floor((P + Q) B^dl / Q)
 * stays sharded; P, Q are consumed; the residues of P, Q and R mod qs[nres] come back; t_recip = the reciprocal's seconds */
struct mdb_s; struct mn_group;
extern void (*newton_mn_pq_hook)(int stage, struct mdb_s *x);   /* called before S = P + Q overwrites P (0) and before Q is freed (1; may take Q->sh) */
extern void (*newton_mn_x_hook)(struct mdb_s *X, void *arg); extern void *newton_mn_x_arg;   /* X over the group before the low product (corrections may still change it) */
void newton_mn_divmod(struct mdb_s *X, struct mdb_s *P, struct mdb_s *Q, size_t dl, struct mn_group *G, const uint64_t *qs, int nres, uint64_t *pres, uint64_t *qres, uint64_t *rres, double *t_recip);
/* the writer on X_hi after DKM's step 1.  Set (by ecalc.c) with the corrections
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
#ifdef __cplusplus
}
#endif
#endif
