/* ntt_dist.h - the distributed four-step transform over a communicator. A plane of n = R x C points is held
 * row-major, rank r owning rows [r R/size, (r+1) R/size) ("row layout"). Index convention: row i, column j holds
 * point m = i + R j, forced by doing the local (row) pass first: F_n = P (F_R (x) I_C) D (I_R (x) F_C) S where the
 * input stride permutation S sorts points by m mod R. So a rank's rows are NOT one contiguous range of limbs but C
 * runs of R/size contiguous limbs (limbs [R j + r R/size, R j + (r+1) R/size) for every j): block-cyclic ownership
 * with block R/size. Loading limbs into the row layout is a local rows x C <-> C x rows transpose; carries at run
 * boundaries cross ranks (one neighbour exchange of C flags per product). Contiguous ownership would cost two
 * all-to-alls per transform instead of one.
 * Forward: local length-C transforms on the rows -> twiddle w_n^(i j) (j in bit-reversed order, as the local pass
 * leaves it) -> one slab all-to-all plus a local block transpose -> local length-R transforms on the columns. The
 * result stays in "column layout" (C/size columns of R points each, contiguous per column, bit-reversed within);
 * pointwise products need no transpose. Inverse: local length-R inverse on the columns -> transpose back -> inverse
 * twiddle -> local length-C inverse on the rows; the n^-1 scaling is split between the two local inverse passes (R^-1
 * and C^-1). logR, logC >= 10 (the local engine's minimum).
 * Slab pipelining (DIST_CHUNKS, default 8; 1 = one exchange): the rank's rows are cut into K chunks of rows/K rows
 * (>= 32), the slab buffers into K chunk regions [k][size][cols][rows/K], and each chunk is exchanged as its own
 * all-to-all on the plan's transfer stream while the compute stream works on the next: forward, the row pass and
 * twiddle-pack of chunk k+1 run under the exchange of chunk k and the unpacks of earlier chunks under the last
 * exchange; inverse, the unpack-twiddle and row inverse of chunk k run under the exchange of chunk k+1. With inflight
 * 2 (layered) two chunks are on the wire; the synthetic communicator (inflight 0) runs one exchange. Values are the
 * same in either mode.
 * DIST_STATS times the parts with events on the two streams (so row passes and packs are timed even under an
 * exchange): t_local1/t_local2/t_pack are kernel device times, t_xfer the exchanges' device time, t_a2a the host time
 * blocked in post + wait (the exposed exchange), t_total first event to last. */
#ifndef EC_NTT_DIST_H
#define EC_NTT_DIST_H
#include "ntt.h"
#include "comm.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    comm *cm; ntt_ctx *ctx; int prime, logR, logC;
    size_t rows;            /* R / size */
    size_t cols;            /* C / size */
    uint64_t *twr, *twc;    /* device: twr[k] = w_R^k (= w_n^(kC)), twc[k] = w_n^k: w_n^(ij) = twr[(ij)>>logC] twc[(ij)&(C-1)] */
    uint64_t *twr_i, *twc_i; /* the same with inverse roots */
    uint64_t *sbuf, *rbuf;  /* device slab buffers, rows x C each */
    int own_slabs;
    int K, k_resume;        /* chunks of the slab pipeline; the inverse's loop index between _pre and _post */
    hipStream_t ts;         /* the transfer stream (the exchanges), events: pack done (s -> ts), exchange done (ts -> s) */
    hipEvent_t ev, evt;
    hipEvent_t evr;         /* recorded on s at a transform's start -- the previous transform's reads of rbuf */
    hipEvent_t *te; int nte, te_cap; unsigned char *tk;  /* timing events of one transform and their kinds */
} dist_plan;
/* timing of the parts (DIST_STATS=1; summed over ranks and calls; the caller resets) */
struct dist_stats { int on; double t_local1, t_local2, t_tw, t_pack, t_a2a, t_xfer, t_total; };
extern struct dist_stats dist_st;
void dist_plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC);
void dist_plan_create_shared(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, uint64_t *sbuf, uint64_t *rbuf);   /* caller's slab buffers (rows x C each) */
void dist_plan_free(dist_plan *p);
void dist_phase_dump(int pe);   /* DIST_STATS=1 diagnostic: this thread's per-phase host times of the transforms, printed and zeroed */
/* x: this rank's rows (rows x C, canonical); result in column layout in x (cols x R) */
void dist_fwd(dist_plan *p, uint64_t *x, hipStream_t s);
/* x, y in column layout: x <- x y pointwise */
void dist_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s);
/* x in column layout -> row layout, natural order, canonical, scaled by n^-1 */
void dist_inv(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_inv_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s);   /* dist_pw + dist_inv with the pointwise fused into the column inverse (bit-identical) */
/* the halves around the all-to-all (pre posts it, post waits for it): for slab pipelining, and for the synthetic
 * communicator, whose ranks are driven by one thread and must all post before any waits */
/* the transposed inverse: the forward algorithm run backwards on the column layout. It ends in the SAME block-cyclic
 * row layout as dist_inv (row k1 holds a[k1 + R k2]); there is no single-all-to-all inverse to contiguous ownership.
 * Kept as an equivalent inverse. */
void dist_inv_t(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_inv_t_pre(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_inv_t_post(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_fwd_pre(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_fwd_post(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_inv_pre(dist_plan *p, uint64_t *x, hipStream_t s);
void dist_inv_post(dist_plan *p, uint64_t *x, hipStream_t s);
#ifdef __cplusplus
}
#endif
#endif
