/* verify.h - tier-1 residue identities and tier-2 digit windows (PLAN.md 8, step 8).
 *
 * T1: eight fixed 62-bit primes q_i (the first primes above 2^62, results/
 * 0_params.txt).  P(1,N+1), Q(1,N+1) mod q by the recursion in Z/q (OpenMP
 * over chunks of terms, serial combine); X, R, T mod q by Horner over limbs
 * (OpenMP over chunks); the digit string mod q by Horner over digits.  Checks:
 *   T (P + Q)  ==  X Q + R   (mod q)      the multiply and the division
 *   digits(X)  ==  X         (mod q)      the radix conversion
 * T2: 50-digit windows at 1-based fractional offsets 50, 10^6, 10^8, 10^9,
 * 10^10, 4 x 10^10 against known digits (ref/ and published values).
 */
#ifndef EC_VERIFY_H
#define EC_VERIFY_H
#include "bigint.h"

#ifdef __cplusplus
extern "C" {
#endif
#define T1_NQ 8
extern const uint64_t t1_q[T1_NQ];
uint64_t vf_limbs_mod(const uint64_t *a, size_t n, uint64_t q);               /* Horner, parallel */
uint64_t vf_digits_mod(const char *s, size_t n, uint64_t q);                  /* decimal string, parallel */
void     vf_pq_mod(unsigned long N, uint64_t q, uint64_t *p, uint64_t *qq);   /* P(1,N+1), Q(1,N+1) mod q */
uint64_t vf_pow_mod(uint64_t b, unsigned long e, uint64_t q);
/* Phase 9 (A-out, M5): the pieces of T1/T2 as a node holds them -- a term range, a limb share, a digit chunk.
 *   vf_pq_range_mod   P(a,b), Q(a,b) mod q over the terms [a, b) (vf_pq_mod = the range [1, N+1))
 *   vf_pq_join        consecutive ranges A (lower terms) then B: P = P_A Q_B + P_B, Q = Q_A Q_B (mod q)
 *   vf_digits_mods    a digit string modulo nq primes in one pass (18 digits per modmul)
 *   vf_digits_join    the residue of a string split in chunks: acc = acc 10^len + v (mod q), chunks from the left
 *   vf_shift_res      a limb share's residue placed at its offset: res B^lo (mod q), B = the limb base */
void     vf_pq_range_mod(unsigned long a, unsigned long b, uint64_t q, uint64_t *p, uint64_t *qq);
void     vf_pq_join(uint64_t *P, uint64_t *Q, uint64_t pb, uint64_t qb, uint64_t q);
void     vf_digits_mods(const char *s, size_t n, const uint64_t *qs, int nq, uint64_t *out);
uint64_t vf_digits_join(uint64_t acc, size_t len, uint64_t v, uint64_t q);
uint64_t vf_shift_res(uint64_t res, size_t lo, uint64_t q);
uint64_t vf_add_mod(uint64_t a, uint64_t b, uint64_t q);
/* returns the number of failing primes (0 = pass); prints per-prime lines when verbose */
int tier1_res(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, int verbose);
int tier1_res_pq(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, const uint64_t *pq_pre, const uint64_t *qq_pre, const uint64_t *xres_pre, const uint64_t *rres_pre, int verbose);
int tier1(unsigned long N, unsigned long d, const bigint *P, const bigint *Q, const bigint *X, const bigint *R, int verbose);
int tier1_digits_res(const char *digits, size_t ndig, const uint64_t *Xres, int verbose);
int tier1_digits_cmp(const uint64_t *Dres, const uint64_t *Xres, int verbose);   /* the digit residues (already computed) against X's */
int tier1_digits(const char *digits, size_t ndig, const bigint *X, int verbose);
/* digits = "2" + fractional digits (ndig chars).  Returns failing windows. */
int tier2(const char *digits, size_t ndig, int verbose);
/* T2 on a piece of the string: s[i] = digit k0 + i for i < k1 - k0, head = the nhead digits before k0 (a chunk's
 * or a node's predecessor).  Windows starting in [k0 - nhead, k1) that end at or before k1 are checked (one
 * ending later is the next piece's, whose head covers it); ndig = d_out + 1 as in tier2.  nchecked counts them.
 * Returns the failing windows.  tier2(s, n) == tier2_range(s, 0, n, 0, 0, n). */
void tier2_windows_reset(void);   /* forget the loaded window table (tests that change ECALC_WINDOWS) */
int tier2_range(const char *s, size_t k0, size_t k1, const char *head, size_t nhead, size_t ndig, int verbose, int *nchecked);
#ifdef __cplusplus
}
#endif
#endif
