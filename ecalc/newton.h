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
typedef struct { size_t iters, overshoots, repeats, down_corr, up_corr; double t_recip, t_div; } newton_stats;
extern newton_stats newton_st;
extern int newton_seed_perturb;      /* test hook: multiply the seed by this/16 (0 = off) */

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
