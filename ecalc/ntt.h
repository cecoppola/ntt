/* ntt.h - the paper's tiled single-transform NTT as library code (PLAN.md 8,
 * step 2).  One context per (device, prime); the caller selects the device.
 *
 *   ntt_fwd      natural order, canonical in  -> bit-reversed, canonical out
 *   ntt_inv      bit-reversed, lazy [0,2p) in -> natural, canonical, x n^-1
 *   ntt_pw       x[i] = x[i] y[i]              (both canonical)
 *   ntt_inv_pw   ntt_pw fused into the first inverse pass (PW_FUSE)
 *   ntt_load     canon64 of nlimbs limbs (device or registered host memory)
 *                into a plane of npoints, zero-extended
 *
 * Forward: b16 passes of NTT_B16_STG stages (default 7, tile 128, 3 blocks/CU)
 * for stages logn-1 .. 10, then one b1 pass for stages 9 .. 0 on 1024-point
 * blocks.  Inverse: the transposed (DIT) stages in the opposite order with the
 * inverse roots.  All kernels: 2048 elements per block, integer add/sub, one
 * lazy operand per modmul, twiddles from two-level tables (rules 1-3b).
 * logn from 10 to 33.
 */
#ifndef EC_NTT_H
#define EC_NTT_H
#include <stdint.h>
#include <stddef.h>
#include <hip/hip_runtime.h>
#include "modarith.h"

#define NTT_LOGN_MIN 10
#define NTT_LOGN_MAX 33
#define NTT_MAXPASS 8

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ntt_ctx ntt_ctx;

extern int ntt_stg;        /* NTT_B16_STG: stages per b16 pass, 3..7 (default 7) */
extern int ntt_pw_fuse;    /* PW_FUSE: fuse pointwise into inverse for logn >= this (default 14) */
extern int ntt_b1_shoup;   /* NTT_B1_SHOUP: Shoup integer modmul in the b1 pass */
extern int ntt_b16_body;   /* NTT_B16_BODY: 0 tile kernel (paper), 1 register-blocked body (Phase 5 item 3; the default since Phase 9) */
extern int ntt_b16_xchg;   /* NTT_B16_XCHG: 1 = the register-blocked body's last exchange by ds_swizzle (Phase 9 B4), 0 = LDS */
/* Phase 13a K (switches, defaults unchanged; -1 = read the environment variable at first use):
 *   ntt_modmul  NTT_MODMUL  H3: 0 FP64 Barrett with Dekker split (default), 1 reduced-correction Barrett
 *               (two-term reciprocal, one correction for data products, two for twiddles), 2 Shoup integer
 *               products (b16 register-blocked body and b1 pass).  All exact: outputs bit-identical.
 *   ntt_mall    NTT_MALL    H2: log2 of a chunk (points) that the passes of narrower span run on to completion
 *               before the next chunk (MALL residency); 0 = off (default).  Bit-identical. */
extern int ntt_modmul, ntt_mall;
int ntt_modmul_get(void);
int ntt_mall_get(void);
/* H2 timing: one pass alone (pass < ntt_npass(logn) - 1: b16 pass; the last: the b1 pass), no scale */
void ntt_pass(ntt_ctx *c, uint64_t *x, int logn, size_t batch, int inv, int pass, hipStream_t s);

/* Phase 9 B1: layout of the pointwise operand y relative to x (a batch of transforms of L points each):
 *   NTT_Y_FULL   y[i] against x[i]                                (one y per x transform)
 *   NTT_Y_BCAST  y[i mod L]                                        (one y for the whole batch, grpB)
 *   NTT_Y_PAIR   y[(t >> 1) L + k] for x transform t, point k      (one y per pair of x transforms) */
enum { NTT_Y_FULL = 0, NTT_Y_BCAST = 1, NTT_Y_PAIR = 2 };

ntt_ctx *ntt_ctx_create(int prime);          /* on the current device */
void     ntt_ctx_free(ntt_ctx *c);
int      ntt_ctx_prime(const ntt_ctx *c);

void ntt_fwd(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s);
void ntt_inv(ntt_ctx *c, uint64_t *x, int logn, size_t batch, hipStream_t s);
void ntt_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, size_t count, hipStream_t s);
void ntt_inv_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s);
/* grpB: one transformed y of 2^logn points against every transform in the batch */
void ntt_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t count, hipStream_t s);
void ntt_inv_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logn, size_t batch, hipStream_t s);
void ntt_inv_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logn, size_t batch, hipStream_t s);
void ntt_load(ntt_ctx *c, uint64_t *dst, const uint64_t *src, size_t nlimbs, size_t npoints, hipStream_t s);

/* pass structure for a given logn (for tests and timing) */
/* WP8: length 3 * 2^logk (ntt3.c); same conventions as above, output order: third r holds X[3 t' + r] */
void ntt_fwd3(ntt_ctx *c, uint64_t *x, int logk, size_t batch, hipStream_t s);
void ntt_inv3(ntt_ctx *c, uint64_t *x, int logk, size_t batch, hipStream_t s);
void ntt_inv3_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logk, size_t batch, hipStream_t s);
void ntt_inv3_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int logk, size_t batch, hipStream_t s);
void ntt_inv3_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logk, size_t batch, hipStream_t s);
/* the pointwise product alone with a y layout (r3: transforms of 3 2^logk points) */
void ntt_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int r3, int logk, size_t batch, hipStream_t s);
/* ntt3.c's use: the 2^logk inverse of the 3 batch thirds with the pointwise product fused (Lt = 3 2^logk) */
void ntt_inv3_core_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int logk, size_t batch, hipStream_t s);
int ntt_npass(int logn);                      /* b16 passes + 1 */
void ntt_pass_bounds(int logn, int pass, int *s_lo, int *s_hi);

/* host reference, O(n log n), any logn >= 1: same DIF / DIT definitions */
void ntt_host_fwd(uint64_t *x, int logn, int prime);
void ntt_host_inv(uint64_t *x, int logn, int prime);

#ifdef __cplusplus
}
#endif
#endif
