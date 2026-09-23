/* crt.h - CRT of the four residue planes into limbs (PLAN.md 8, step 4).
 *
 * CPU: Garner with M1 = P0 P1 (128-bit) and M2 = M1 P2 (192-bit) precomputed,
 * __int128 % for the modular steps (rule 7: 1.8x faster than FP64 Barrett on
 * Zen4), T stripes each with a 3-limb + carry-bit window (rule 7, RESULTS.md
 * 34: 2.06x the sliding window), sequential spill merge.
 *
 *   crt_carry_par4(res, n, out, T):  out[0 .. n+3] =
 *   sum_k value(res[0..3][k]) 2^(64 k), value < 2^206 < 2^256.
 *   Phase 13a P3: with ec_np = 3 (ECALC_NP=3, decimal limbs only) planes 0..2 are read (res[3] is not touched),
 *   value < p0 p1 p2 < 2^156 in three base-10^18 digits.
 */
#ifndef EC_CRT_H
#define EC_CRT_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void crt_init(void);
void crt_garner4(const uint64_t r[4], uint64_t out[4]);      /* one coefficient */
void crt_garner3(const uint64_t r[3], uint64_t out[3]);      /* P3: one coefficient from three primes (< p0 p1 p2) */
void crt_carry_par4(uint64_t *const res[4], size_t n, uint64_t *out, int T);
/* quartered layout: buf[q] holds, for each plane d, coefficients [qQ, min((q+1)Q, n)) at
 * buf[q] + d Q; stripes of quarter q run on NUMA node q (T stripes in total, T % 4 == 0) */
void crt_carry_par4_q(uint64_t *const buf[4], size_t Q, size_t n, uint64_t *out, int T);
#ifdef __cplusplus
}
#endif
#endif

/* engine 2 (crt2.c): two 62-bit primes, coefficients c_i < P0 P1 < 2^124 at
 * bit positions 45 i.  out[0 .. limbs_out) = sum_i c_i 2^(45 i) mod 2^(64 limbs_out),
 * T stripes with 4-limb spills merged sequentially. */
#ifdef __cplusplus
extern "C" {
#endif
void crt2_carry(uint64_t *const res[2], size_t n, uint64_t *out, size_t limbs_out, int T);
#ifdef __cplusplus
}
#endif
