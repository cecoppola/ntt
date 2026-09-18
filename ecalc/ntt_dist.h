/* ntt_dist.h - the distributed four-step transform over a communicator (WP5).
 *
 * A plane of n = R x C points is held row-major, rank r owning rows
 * [r R/size, (r+1) R/size) ("row layout", R/size x C per rank).
 * Index convention: row i, column j holds point m = i + R j.  This is forced
 * by doing the local (row) pass first: F_n = P (F_R (x) I_C) D (I_R (x) F_C) S
 * where the input stride permutation S sorts points by m mod R.  So a rank's
 * rows are NOT one contiguous range of the number's limbs but C runs of
 * R/size contiguous limbs (limbs [R j + r R/size, R j + (r+1) R/size) for
 * every j), i.e. a block-cyclic ownership with block R/size.  Loading limbs
 * into the row layout is a local rows x C <-> C x rows transpose; carries at
 * run boundaries cross ranks (one neighbour exchange of C flags per product).
 * The alternative -- contiguous ownership -- costs two all-to-alls per
 * transform instead of one.
 * Forward: local length-C transforms on the rows -> twiddle w_n^(i j) (j read
 * in bit-reversed order, as the local pass leaves it) -> one slab all-to-all
 * plus a local block transpose -> local length-R transforms on the columns.
 * The result stays in "column layout" (C/size columns of R points each,
 * contiguous per column, bit-reversed within); pointwise products need no
 * transpose.  Inverse: local length-R inverse on the columns -> transpose
 * back -> inverse twiddle -> local length-C inverse on the rows; the n^-1
 * scaling is split between the two local inverse passes (R^-1 and C^-1).
 * One all-to-all per transform.  logR, logC >= 10 (the local engine's minimum).
 */
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
} dist_plan;
void dist_plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC);
void dist_plan_create_shared(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, uint64_t *sbuf, uint64_t *rbuf);   /* caller's slab buffers (rows x C each) */
void dist_plan_free(dist_plan *p);
/* x: this rank's rows (rows x C, canonical); result in column layout in x (cols x R) */
void dist_fwd(dist_plan *p, uint64_t *x, hipStream_t s);
/* x, y in column layout: x <- x y pointwise */
void dist_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s);
/* x in column layout -> row layout, natural order, canonical, scaled by n^-1 */
void dist_inv(dist_plan *p, uint64_t *x, hipStream_t s);
/* the halves around the all-to-all (pre posts it, post waits for it): for slab pipelining, and for the
 * synthetic communicator, whose ranks are driven by one thread and must all post before any waits */
/* the transposed inverse: the same algorithm as the forward run backwards on the column layout (which is
 * the row layout of the C x R problem).  It ends in the SAME block-cyclic row layout as dist_inv (row k1
 * holds a[k1 + R k2]) -- there is no single-all-to-all inverse to contiguous ownership (RESULTS.md 59);
 * kept as an equivalent inverse. */
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
