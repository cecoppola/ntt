/* binsplit.h - bottom-up binary splitting of e = sum 1/k! (PLAN.md 8, step 6).
 *
 * Two-variable recursion over [a, b): Q = a (a+1) ... (b-1), P = sum_{k=a}^{b-1}
 * (k+1) ... (b-1), so P/Q = sum_{k=a}^{b-1} 1/(a ... k) and e = 1 + P(1,N+1)/Q(1,N+1);
 * combine P = P1 Q2 + P2, Q = Q1 Q2.  Seeds: spans of bs_seed_terms terms by
 * schoolbook (OpenMP over spans).  Each level pairs adjacent nodes; the tier
 * is chosen per level from the largest node length max_nl:
 *   max_nl <= bs_school_nl (160)                  CPU schoolbook, OpenMP over pairs
 *   2 max_nl + 1 <= 2^RNS_BATCH_LOGL_MAX (2^18)   one rns_mul_batch of 2 npairs products
 *   otherwise                                      rns_mul_pair per pair (Q2 transformed once)
 * Nodes live in two alternating registered level pools (grow-only), so the
 * batch tier reads them in place.
 */
#ifndef EC_BINSPLIT_H
#define EC_BINSPLIT_H
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { double t_seed, t_school, t_batch, t_mdev, t_total; int levels, school_levels, batch_levels, mdev_levels; size_t peak_pool_limbs; } bs_stats;
extern bs_stats bs_st;
extern int bs_seed_terms;     /* 512 */
extern int bs_school_nl;      /* 160 limbs */
extern int bs_verbose;

unsigned long e_terms(unsigned long digits);            /* N = min{m : lgamma(m+1)/ln10 >= d + 50} */
void binsplit_e(bigint *P, bigint *Q, unsigned long N); /* P(1,N+1), Q(1,N+1) */
void binsplit_pregrow(unsigned long N);                  /* WP3: allocate the region pools at init (outside the timed phase) */
void binsplit_free_pools(void);                          /* release the two level pools */
/* reference: the same recursion on the CPU with schoolbook products, any N */
void binsplit_ref(bigint *P, bigint *Q, unsigned long a, unsigned long b);
#ifdef __cplusplus
}
#endif
#endif
