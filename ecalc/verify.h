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
/* returns the number of failing primes (0 = pass); prints per-prime lines when verbose */
int tier1_res(unsigned long N, unsigned long d, const uint64_t *Pres, const uint64_t *Qres, const bigint *X, const bigint *R, int verbose);
int tier1(unsigned long N, unsigned long d, const bigint *P, const bigint *Q, const bigint *X, const bigint *R, int verbose);
int tier1_digits_res(const char *digits, size_t ndig, const uint64_t *Xres, int verbose);
int tier1_digits(const char *digits, size_t ndig, const bigint *X, int verbose);
/* digits = "2" + fractional digits (ndig chars).  Returns failing windows. */
int tier2(const char *digits, size_t ndig, int verbose);
#ifdef __cplusplus
}
#endif
#endif
