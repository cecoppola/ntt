/*
 * ntt.h — shared interface for all NTT implementations in this project.
 *
 * Include this header in every implementation file (.c or .hip) to ensure
 * the parameter struct and public function signatures stay consistent.
 * Each binary links against exactly one implementation of ntt_forward/
 * ntt_inverse (either ntt_cpu.c, ntt_gpu.hip, or ntt_mont.c).
 *
 * Compatibility: C99 and C++ (hipcc). Uses extern "C" guard for C++ callers.
 */

#ifndef NTT_H
#define NTT_H

#include <stdint.h>
#include <stdlib.h>   /* size_t, malloc */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Parameter struct ────────────────────────────────────────────────────────
 * All transform parameters in one struct. Passed explicitly to every
 * function; no global mutable state. Filled by ntt_params_init().
 */
typedef struct {
    uint64_t n;          /* transform length; must be a power of 2          */
    uint64_t q;          /* NTT-friendly prime modulus                       */
    uint64_t omega;      /* primitive n-th root of unity mod q               */
    uint64_t omega_inv;  /* modular inverse of omega (for INTT twiddles)     */
    uint64_t n_inv;      /* modular inverse of n mod q (INTT final scaling)  */
    uint32_t log2_n;     /* log2(n); precomputed                             */
} ntt_params_t;

/* ── Parameter initialisation ────────────────────────────────────────────────
 * ntt_params_init: compute derived fields (omega_inv, n_inv, log2_n).
 * Caller must set n, q, omega before calling. Uses Fermat's little theorem.
 * Returns 0 on success; -1 if n is not a power of 2.
 */
int ntt_params_init(ntt_params_t *p);

/* ── Twiddle factor allocation ───────────────────────────────────────────────
 * All functions return heap-allocated arrays; caller must free().
 * Returns NULL on allocation failure.
 *
 * ntt_alloc_twiddles:      tw[k] = omega^k mod q,     k in [0, n/2)
 * ntt_alloc_twiddles_inv:  tw[k] = omega_inv^k mod q, k in [0, n/2)
 * ntt_alloc_twiddles_mont: tw[k] = omega^k * R mod q  (Montgomery form)
 *   where R = 2^32. Use with ntt_forward_mont / ntt_inverse_mont only.
 * ntt_alloc_twiddles_inv_mont: same with omega_inv.
 */
uint64_t *ntt_alloc_twiddles(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_mont(const ntt_params_t *p);
uint64_t *ntt_alloc_twiddles_inv_mont(const ntt_params_t *p);

/* ── Core NTT — lazy reduction (ntt_cpu.c / ntt_gpu.hip) ────────────────────
 * ntt_forward: in-place CT-DIT NTT over Z_q.
 *   Input:  a[0..n-1] in natural order, values in [0, q).
 *   Output: NTT(a), values in [0, q).
 *   twiddles: from ntt_alloc_twiddles(p).
 *
 * ntt_inverse: in-place INTT. Applies forward NTT with omega_inv twiddles,
 *   then scales by n^{-1} mod q.
 *   twiddles_inv: from ntt_alloc_twiddles_inv(p).
 */
void ntt_forward(uint64_t *a, const uint64_t *twiddles,     const ntt_params_t *p);
void ntt_inverse(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p);

/* ── Core NTT — Montgomery multiplication (ntt_mont.c) ──────────────────────
 * Drop-in replacements for ntt_forward/ntt_inverse with Montgomery butterfly.
 * External interface is identical: a[] in standard form, values in [0, q).
 * Montgomery conversion is handled internally.
 *   twiddles:     from ntt_alloc_twiddles_mont(p).
 *   twiddles_inv: from ntt_alloc_twiddles_inv_mont(p).
 */
void ntt_forward_mont(uint64_t *a, const uint64_t *twiddles,     const ntt_params_t *p);
void ntt_inverse_mont(uint64_t *a, const uint64_t *twiddles_inv, const ntt_params_t *p);

#ifdef __cplusplus
}
#endif

#endif /* NTT_H */
