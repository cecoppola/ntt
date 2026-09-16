/* todec.h - radix conversion by top-down Barrett division (PLAN.md 8, step 7).
 *
 * todec(out, X, ndig): the decimal digits of X (< 10^ndig) as ndig characters
 * with leading zeros.  The digit count is padded to Lg 2^m with Lg = 18 u,
 * 128 <= u < 256, so every piece at level i has Lg 2^(m-i) digits and the
 * level's divisor is one T_i = 10^(Lg 2^(m-i-1)) with one cached reciprocal
 * mu_i (Newton).  Levels (top down):
 *   TOP   <= 2 pieces                     newton_divmod per piece (mdev products)
 *   MID   2 nl > 2^RNS_BATCH_LOGL_MAX     newton_divmod per piece with the cached mu
 *   DEEP  otherwise                        two grpB batches per level: X = (A mu) >> s,
 *                                          R = A - X T, then +-T corrections on the CPU
 *   LEAF  pieces of Lg digits (< 256 limbs) GPU: repeated 128-bit Barrett division by
 *                                          10^18 (mu = floor(2^123 / 10^18)), 18-digit
 *                                          blocks written straight into `out`
 * Pieces live in two alternating registered level pools; T_i and mu_i in
 * registered memory (the divisor cache), prewarmed bottom-up by squaring.
 */
#ifndef EC_TODEC_H
#define EC_TODEC_H
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct { double t_prewarm, t_top, t_mid, t_deep, t_leaf, t_total; int levels, top_levels, mid_levels, deep_levels; size_t pieces, peak_pool_limbs; } dec_stats;
extern dec_stats dec_st;
extern int dec_verbose;
extern int dec_leaf_u_max;      /* 256: leaf pieces of 18 u digits, u < this */
extern int todec_free_input;    /* 1: todec frees X's limbs after copying them (saves its 16.6 GB at 4e10) */
extern int dec_seed_prewarm;    /* 1: mu(2h) seeded from mu(h)^2 (paper); 0: fresh Newton per level */

/* out must hold ndig bytes; registered memory lets the LEAF kernel write it directly;
 * out == NULL: todec allocates a registered buffer at LEAF time (todec_out; free with mem_hreg_free) */
extern char *todec_out;
void todec(char *out, const bigint *X, unsigned long ndig);
#ifdef __cplusplus
}
#endif
#endif
