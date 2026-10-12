/* ntt.h - the paper's tiled single-transform NTT as library code (PLAN.md 8,
 * step 2).  One context per (device, prime); the caller selects the device.
 *
 *   ntt_fwd      natural order, canonical in  -> bit-reversed, canonical out
 *   ntt_inv      bit-reversed, lazy [0,2p) in -> natural, canonical, x n^-1
 *   ntt_pw       x[i] = x[i] y[i]              (both canonical)
 *   ntt_inv_pw   ntt_pw fused into the first inverse pass (from 2^14 on: ntt_pw_fuse)
 *   ntt_load     canon64 of nlimbs limbs (device or registered host memory)
 *                into a plane of npoints, zero-extended
 *
 * Forward: b16 passes of 7 stages (tile 128; a partial pass of fewer) and one b1
 * pass on 2^10..2^12-point blocks, as the per-logn plan places them (ntt.c).  Inverse: the transposed (DIT) stages in the opposite order with the
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

extern int ntt_stg;        /* stages per b16 pass, 3..7 (7; the tests' pass-split check sets it; NTT_B16_STG removed 2026-10-12) */
extern int ntt_pw_fuse;    /* fuse pointwise into inverse for logn >= this (14; tests set it; PW_FUSE removed 2026-10-12) */
/* The kernel switches NTT_B16_BODY, NTT_MODMUL, NTT_B1R, NTT_PLAN (Phase 9, 13a/b K) and NTT_R3_FUSE (Phase 15 N3x) were
 * removed 2026-10-12 at their defaults: the register-blocked 7-stage body, the reduced-correction Barrett, the
 * register-blocked b1 pass (2^3 points per thread), the per-logn plan; the radix-3 stage stays its own pass (ntt3.c). */
/* bench: one b16 pass of stg stages at row stride 2^s_lo (its own twiddles) over batch x 2^logn points */
void ntt_pass_at(ntt_ctx *c, uint64_t *x, int logn, size_t batch, int s_lo, int stg, int inv, hipStream_t s);
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

/* S22: NTT_SIZE_STATS=1 (default off; print only) -- per transform size, the count and the summed GPU time (hip events on the call's stream, so the
 * transform's own passes; summed over the APUs) of the outermost forward / inverse calls, by the local length and by the logical distributed size
 * (tag: ntt_dist.c sets it to logR + logC around its transforms, 0 outside).  ntt_size_stats_print() prints once per rank (call at the end of a run). */
int  ntt_ss_enabled(void);
int  ntt_ss_begin(int dir, int r3, int logn, size_t batch, hipStream_t s);   /* dir 0 forward, 1 inverse; r3: length 3 2^logn; -1 when off or nested */
void ntt_ss_end(int tok, hipStream_t s);
int  ntt_ss_tag(int packed);                                                  /* thread-local, packed = logical | logR << 8 | logC << 16 (0: none); returns the previous */
void ntt_size_stats_print(int rank, double wall_s);
#ifdef __cplusplus
}
struct ntt_ss_scope {
    int tok; hipStream_t s;
    ntt_ss_scope(int d, int r3, int l, size_t b, hipStream_t st) : tok(ntt_ss_enabled() ? ntt_ss_begin(d, r3, l, b, st) : -1), s(st) {}
    ~ntt_ss_scope() { if (tok >= 0) ntt_ss_end(tok, s); }
};
struct ntt_ss_tagger { int prev, on; ntt_ss_tagger(int logR, int logC) : prev(0), on(ntt_ss_enabled()) { if (on) prev = ntt_ss_tag((logR + logC) | (logR << 8) | (logC << 16)); } ~ntt_ss_tagger() { if (on) ntt_ss_tag(prev); } };
#endif
#endif
