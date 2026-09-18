/* ntt_dist.c - see ntt_dist.h */
#include <stdio.h>
#include <stdlib.h>
#include "ntt_dist.h"
#include "modarith.h"
#include <time.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

__device__ static inline unsigned brev(unsigned v, int bits) { return __brev(v) >> (32 - bits); }

/* twiddle: x[i][jb] *= w_n^(i * j), i = global row, j = brev(jb) (the row pass left columns bit-reversed).
 * w_n^(i j) = twr[(i j) >> logC] * twc[(i j) & (C-1)] with twr[k] = w_n^(k C) = w_R^k, twc[k] = w_n^k. */
__global__ void k_twiddle(uint64_t *x, size_t rows, size_t row0, int logC, const uint64_t *twr, const uint64_t *twc, ec_mod m, int inv_scale_unused)
{
    size_t C = (size_t)1 << logC, total = rows * C;
    size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t i = row0 + t / C, jb = t & (C - 1), j = brev((unsigned)jb, logC);
        size_t e = i * j;                                                 /* < R C = n, no reduction needed */
        double w = ec_mm((double)twr[(e >> logC)], (double)twc[e & (C - 1)], m.p, m.pinv);
        x[t] = (uint64_t)ec_mm((double)x[t], w, m.p, m.pinv);
    }
}
/* slab pack: rows x C row-major -> size slabs, slab s = the block (all my rows) x (columns of rank s), stored as
 * cols_per_rank x rows (column-major within the slab, so the receiver can concatenate slabs into columns) */
__global__ void k_pack(const uint64_t *x, uint64_t *sb, size_t rows, size_t C, size_t cols, int size)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t i = t / C, j = t % C, s = j / cols, jl = j % cols;
        sb[s * (cols * rows) + jl * rows + i] = x[t];
    }
}
/* after the all-to-all: rb holds size slabs, slab r = (my cols) x (rows of rank r), column-major -> columns of R points */
__global__ void k_unpack(const uint64_t *rb, uint64_t *x, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * R, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t jl = t / R, i = t % R, r = i / rows, il = i % rows;
        x[jl * R + i] = rb[r * (cols * rows) + jl * rows + il];
    }
}
/* fused unpack + inverse twiddle (the mirror of k_twpack): slabs (sb[s][jl][il]) -> rows x C, twiddled */
__global__ void k_unpacktw(const uint64_t *rb, uint64_t *x, size_t rows, size_t row0, int logC, size_t cols, int size, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t jb = bj + ty + k, il = bi + tx, s = jb / cols, jl = jb % cols;
        tile[ty + k][tx] = rb[s * (cols * rows) + jl * rows + il];
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx, i = row0 + il, j = brev((unsigned)jb, logC), e = i * j;
        double w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv);
        x[il * C + jb] = (uint64_t)ec_mm((double)ec_fold(tile[tx][ty + k], m.pu), w, m.p, m.pinv);
    }
}
/* the reverse pair for the inverse: columns -> slabs -> rows */
__global__ void k_pack_cols(const uint64_t *x, uint64_t *sb, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * R, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t jl = t / R, i = t % R, r = i / rows, il = i % rows; sb[r * (cols * rows) + jl * rows + il] = x[t]; }
}
__global__ void k_unpack_rows(const uint64_t *rb, uint64_t *x, size_t rows, size_t C, size_t cols, int size)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t i = t / C, j = t % C, s = j / cols, jl = j % cols; x[t] = rb[s * (cols * rows) + jl * rows + i]; }
}

static uint64_t *dev_pow_table(int prime, uint64_t w, size_t cnt)
{
    uint64_t p = ec_P[prime], *h = (uint64_t *)malloc(cnt * 8), *d, a = 1;
    for (size_t k = 0; k < cnt; k++) { h[k] = a; a = ec_mulmod_ref(a, w, p); }
    HIP_CHECK(hipMalloc(&d, cnt * 8)); HIP_CHECK(hipMemcpy(d, h, cnt * 8, hipMemcpyHostToDevice)); free(h);
    return d;
}
void dist_plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC)
{
    int size = comm_size(cm);
    p->cm = cm; p->ctx = ctx; p->prime = prime; p->logR = logR; p->logC = logC;
    p->rows = ((size_t)1 << logR) / size; p->cols = ((size_t)1 << logC) / size;
    int logn = logR + logC;
    uint64_t wn = ec_root(prime, logn), wR = ec_root(prime, logR);
    p->twr = dev_pow_table(prime, wR, (size_t)1 << logR);          /* w_R^k = w_n^(k C) */
    p->twc = dev_pow_table(prime, wn, (size_t)1 << logC);          /* w_n^k, k < C */
    p->twr_i = dev_pow_table(prime, ec_root_inv(prime, logR), (size_t)1 << logR);
    p->twc_i = dev_pow_table(prime, ec_root_inv(prime, logn), (size_t)1 << logC);
    size_t bytes = p->rows * ((size_t)1 << logC) * 8;
    p->own_slabs = 1;
    HIP_CHECK(hipMalloc(&p->sbuf, bytes)); HIP_CHECK(hipMalloc(&p->rbuf, bytes));
}
void dist_plan_create_shared(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, uint64_t *sbuf, uint64_t *rbuf)
{
    dist_plan_create(p, cm, ctx, prime, logR, logC);
    HIP_CHECK(hipFree(p->sbuf)); HIP_CHECK(hipFree(p->rbuf));
    p->sbuf = sbuf; p->rbuf = rbuf; p->own_slabs = 0;
}
void dist_plan_free(dist_plan *p)
{
    HIP_CHECK(hipFree(p->twr)); HIP_CHECK(hipFree(p->twc)); HIP_CHECK(hipFree(p->twr_i)); HIP_CHECK(hipFree(p->twc_i));
    if (p->own_slabs) { HIP_CHECK(hipFree(p->sbuf)); HIP_CHECK(hipFree(p->rbuf)); }
}
static unsigned nblocks(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

struct dist_stats dist_st;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
#define TS(field, stmt) do { double t0_ = 0; if (dist_st.on) { HIP_CHECK(hipStreamSynchronize(s)); t0_ = tnow(); } stmt; if (dist_st.on) { HIP_CHECK(hipStreamSynchronize(s)); dist_st.field += tnow() - t0_; } } while (0)
/* fused twiddle + pack through a 32 x 32 LDS tile: read rows x C coalesced (row-major), twiddle, write the
 * slab (column-major per slab: sb[s][jl][i]) coalesced along i.  grid (C/32, rows/32), block (32, 8). */
__global__ void k_twpack(const uint64_t *x, uint64_t *sb, size_t rows, size_t row0, int logC, size_t cols, int size, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx, i = row0 + il, j = brev((unsigned)jb, logC), e = i * j;
        double w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv);
        tile[ty + k][tx] = (uint64_t)ec_mm((double)x[il * C + jb], w, m.p, m.pinv);
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t jb = bj + ty + k, il = bi + tx, s = jb / cols, jl = jb % cols;
        sb[s * (cols * rows) + jl * rows + il] = tile[tx][ty + k];
    }
}
/* tiled unpack: slab r (my cols x rank r's rows, column-major per column) -> my columns of R points: reads
 * coalesced along il, writes coalesced along i = r rows + il -- both contiguous, no tile needed */
void dist_fwd_pre(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC;
    ec_mod m = ec_mod_get(p->prime);
    TS(t_local1, ntt_fwd(p->ctx, x, p->logC, p->rows, s));                         /* rows: length-C, bit-reversed columns */
    { dim3 grid((unsigned)(C / 32), (unsigned)(p->rows / 32)), blk(32, 8);
      TS(t_pack, (k_twpack<<<grid, blk, 0, s>>>(x, p->sbuf, p->rows, (size_t)r * p->rows, p->logC, p->cols, size, p->twr, p->twc, m))); }
    HIP_CHECK(hipStreamSynchronize(s));
    if (dist_st.on) dist_st.t0_a2a = tnow();
    comm_alltoall(p->cm, p->sbuf, p->rbuf, p->cols * p->rows * 8, s);
}
void dist_fwd_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm);
    size_t R = (size_t)1 << p->logR;
    comm_wait(p->cm);
    if (dist_st.on) dist_st.t_a2a += tnow() - dist_st.t0_a2a;
    TS(t_pack, (k_unpack<<<nblocks(p->cols * R), 256, 0, s>>>(p->rbuf, x, p->rows, p->cols, size)));
    TS(t_local2, ntt_fwd(p->ctx, x, p->logR, p->cols, s));                         /* columns: length-R */
}
void dist_fwd(dist_plan *p, uint64_t *x, hipStream_t s) { dist_fwd_pre(p, x, s); dist_fwd_post(p, x, s); }
void dist_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s)
{
    ntt_pw(p->ctx, x, y, p->cols * ((size_t)1 << p->logR), s);
}
/* inverse twiddle tables are the forward ones read with negated exponent: use w^(n - e) = inverse of w^e via
 * tables of the inverse roots built once in the plan (twr_i, twc_i) */
void dist_inv_pre(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm);
    size_t R = (size_t)1 << p->logR;
    TS(t_local2, ntt_inv(p->ctx, x, p->logR, p->cols, s));                         /* columns: length-R inverse, x R^-1, natural */
    TS(t_pack, (k_pack_cols<<<nblocks(p->cols * R), 256, 0, s>>>(x, p->sbuf, p->rows, p->cols, size)));
    HIP_CHECK(hipStreamSynchronize(s));
    if (dist_st.on) dist_st.t0_a2a = tnow();
    comm_alltoall(p->cm, p->sbuf, p->rbuf, p->cols * p->rows * 8, s);
}
void dist_inv_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC;
    ec_mod m = ec_mod_get(p->prime);
    comm_wait(p->cm);
    if (dist_st.on) dist_st.t_a2a += tnow() - dist_st.t0_a2a;
    { dim3 grid((unsigned)(C / 32), (unsigned)(p->rows / 32)), blk(32, 8);
      TS(t_pack, (k_unpacktw<<<grid, blk, 0, s>>>(p->rbuf, x, p->rows, (size_t)r * p->rows, p->logC, p->cols, size, p->twr_i, p->twc_i, m))); }
    TS(t_local1, ntt_inv(p->ctx, x, p->logC, p->rows, s));                         /* rows: length-C inverse, x C^-1 */
}
void dist_inv(dist_plan *p, uint64_t *x, hipStream_t s) { dist_inv_pre(p, x, s); dist_inv_post(p, x, s); }

/* The transposed inverse: the column layout (cols columns of R points, bit-reversed within) is the row
 * layout of the C x R problem, so the same algorithm as the forward -- local pass on the contiguous index
 * first -- with inverse roots computes the inverse DFT and lands in the column layout of the C x R
 * problem: R/size "columns" of C contiguous points each, i.e. this rank holds the contiguous point range
 * [r n/size, (r+1) n/size) in natural order.  Steps: ntt_inv on the cols columns (length R, bit-reversed
 * in, natural out, x R^-1) -> twiddle w_n^(-k1 k2), k1 = position (natural), k2 = brev(column) ->
 * pack/all-to-all/unpack into rows of length C: row k1 of this rank holds, for every k2 in stored
 * (bit-reversed) order, one value -> ntt_inv on the rows (length C, bit-reversed in, natural out,
 * x C^-1).  Output: rows x C, row k1 (global) = points [C k1, C k1 + C). */
__global__ void k_twiddle_t(uint64_t *x, size_t cols, size_t col0, int logR, int logC, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    size_t R = (size_t)1 << logR, total = cols * R;
    size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t jl = t / R, k1 = t % R, k2 = brev((unsigned)(col0 + jl), logC);
        size_t e = k1 * k2;                                               /* < R C = n */
        double w = ec_mm((double)twr[e >> logC], (double)twc[e & (((size_t)1 << logC) - 1)], m.p, m.pinv);
        x[t] = (uint64_t)ec_mm((double)ec_fold(x[t], m.pu), w, m.p, m.pinv);
    }
}
/* columns (cols x R, column-major per column) -> slabs: slab s holds my columns' entries for the rows of rank s */
__global__ void k_pack_t(const uint64_t *x, uint64_t *sb, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * R, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t jl = t / R, i = t % R, s = i / rows, il = i % rows; sb[s * (cols * rows) + il * cols + jl] = x[t]; }
}
/* slabs -> rows: my row il, stored column position (r cols + jl) */
__global__ void k_unpack_t(const uint64_t *rb, uint64_t *x, size_t rows, size_t cols, int size)
{
    size_t C = cols * size, total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t il = t / C, j = t % C, r = j / cols, jl = j % cols; x[t] = rb[r * (cols * rows) + il * cols + jl]; }
}
void dist_inv_t_pre(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t R = (size_t)1 << p->logR;
    ec_mod m = ec_mod_get(p->prime);
    ntt_inv(p->ctx, x, p->logR, p->cols, s);                                       /* columns: length-R inverse, natural, x R^-1 */
    k_twiddle_t<<<nblocks(p->cols * R), 256, 0, s>>>(x, p->cols, (size_t)r * p->cols, p->logR, p->logC, p->twr_i, p->twc_i, m);
    k_pack_t<<<nblocks(p->cols * R), 256, 0, s>>>(x, p->sbuf, p->rows, p->cols, size);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(p->cm, p->sbuf, p->rbuf, p->cols * p->rows * 8, s);
}
void dist_inv_t_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm);
    size_t C = (size_t)1 << p->logC;
    comm_wait(p->cm);
    k_unpack_t<<<nblocks(p->rows * C), 256, 0, s>>>(p->rbuf, x, p->rows, p->cols, size);
    ntt_inv(p->ctx, x, p->logC, p->rows, s);                                       /* rows: length-C inverse (bit-reversed in), natural, x C^-1 */
}
void dist_inv_t(dist_plan *p, uint64_t *x, hipStream_t s) { dist_inv_t_pre(p, x, s); dist_inv_t_post(p, x, s); }
