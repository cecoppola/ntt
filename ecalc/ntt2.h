/* ntt2.h - engine 2: the same kernels with Montgomery arithmetic on two 62-bit primes and 45-bit points (PLAN.md 8,
 * step 2).  One context per (device, prime); the caller selects the device.
 *
 *   ntt2_fwd      natural order, canonical in  -> bit-reversed, canonical out
 *   ntt2_inv      bit-reversed, lazy [0,2p) in -> natural, canonical, x n^-1
 *   ntt2_pw       x[i] = x[i] y[i]              (both canonical)
 *   ntt2_inv_pw   ntt2_pw fused into the first inverse pass (PW_FUSE)
 *   ntt2_load     canon64 of nlimbs limbs (device or registered host memory)
 *                into a plane of npoints, zero-extended
 *
 * Forward: b16 passes of NTT_B16_STG stages (default 7, tile 128, 3 blocks/CU)
 * for stages logn-1 .. 10, then one b1 pass for stages 9 .. 0 on 1024-point
 * blocks.  Inverse: the transposed (DIT) stages in the opposite order with the
 * inverse roots.  All kernels: 2048 elements per block, integer add/sub, one
 * lazy operand per modmul, twiddles from two-level tables (rules 1-3b).
 * logn from 10 to 33.
 */
#ifndef EC_NTT2_H
#define EC_NTT2_H
#include <stdint.h>
#include <stddef.h>
#include <hip/hip_runtime.h>
#include "modarith2.h"

#ifndef NTT_LOGN_MIN
#define NTT_LOGN_MIN 10
#define NTT_LOGN_MAX 33
#define NTT_MAXPASS 8
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntt2_ctx ntt2_ctx;

extern int ntt2_stg;        /* NTT_B16_STG: stages per b16 pass, 3..7 (default 7) */
extern int ntt2_pw_fuse;    /* PW_FUSE: fuse pointwise into inverse for logn >= this (default 14) */
extern int ntt2_b16_body;   /* NTT_B16_BODY: 0 tile kernel (paper), 1 register-blocked body (Phase 5 item 3) */

ntt2_ctx *ntt2_ctx_create(int prime);          /* on the current device */
void     ntt2_ctx_free(ntt2_ctx *c);
int      ntt2_ctx_prime(const ntt2_ctx *c);

void ntt2_fwd(ntt2_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s);
void ntt2_inv(ntt2_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s);
void ntt2_pw(ntt2_ctx *c, uint64_t *x, const uint64_t *y, size_t count, hipStream_t s);
void ntt2_inv_pw(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s);
/* grpB: one transformed y of 2^logn points against every transform in the batch */
void ntt2_pw_bcast(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t count, hipStream_t s);
void ntt2_inv_pw_bcast(ntt2_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s);
void ntt2_load(ntt2_ctx *c, uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, hipStream_t s);   /* 45-bit points from limbs, Montgomery form */
static inline size_t e2_points(size_t limbs) { return (limbs * 64 + E2_BITS - 1) / E2_BITS; }

/* pass structure for a given logn (for tests and timing) */
int ntt2_npass(int logn);                      /* b16 passes + 1 */
void ntt2_pass_bounds(int logn, int pass, int *s_lo, int *s_hi);

/* host reference, O(n log n), any logn >= 1: same DIF / DIT definitions */
void ntt2_host_fwd(uint64_t *x, int logn, int prime);
void ntt2_host_inv(uint64_t *x, int logn, int prime);

#ifdef __cplusplus
}
#endif
#endif
