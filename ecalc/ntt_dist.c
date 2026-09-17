/* ntt_dist.c - see ntt_dist.h */
#include <stdio.h>
#include <stdlib.h>
#include "ntt_dist.h"
#include "modarith.h"
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
    HIP_CHECK(hipMalloc(&p->sbuf, bytes)); HIP_CHECK(hipMalloc(&p->rbuf, bytes));
}
void dist_plan_free(dist_plan *p)
{
    HIP_CHECK(hipFree(p->twr)); HIP_CHECK(hipFree(p->twc)); HIP_CHECK(hipFree(p->twr_i)); HIP_CHECK(hipFree(p->twc_i)); HIP_CHECK(hipFree(p->sbuf)); HIP_CHECK(hipFree(p->rbuf));
}
static unsigned nblocks(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

void dist_fwd_pre(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC;
    ec_mod m = ec_mod_get(p->prime);
    ntt_fwd(p->ctx, x, p->logC, p->rows, s);                                       /* rows: length-C, bit-reversed columns */
    k_twiddle<<<nblocks(p->rows * C), 256, 0, s>>>(x, p->rows, (size_t)r * p->rows, p->logC, p->twr, p->twc, m, 0);
    k_pack<<<nblocks(p->rows * C), 256, 0, s>>>(x, p->sbuf, p->rows, C, p->cols, size);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(p->cm, p->sbuf, p->rbuf, p->cols * p->rows * 8, s);
}
void dist_fwd_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm);
    size_t R = (size_t)1 << p->logR;
    comm_wait(p->cm);
    k_unpack<<<nblocks(p->cols * R), 256, 0, s>>>(p->rbuf, x, p->rows, p->cols, size);
    ntt_fwd(p->ctx, x, p->logR, p->cols, s);                                       /* columns: length-R */
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
    ntt_inv(p->ctx, x, p->logR, p->cols, s);                                       /* columns: length-R inverse, x R^-1, natural */
    k_pack_cols<<<nblocks(p->cols * R), 256, 0, s>>>(x, p->sbuf, p->rows, p->cols, size);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(p->cm, p->sbuf, p->rbuf, p->cols * p->rows * 8, s);
}
void dist_inv_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC;
    ec_mod m = ec_mod_get(p->prime);
    comm_wait(p->cm);
    k_unpack_rows<<<nblocks(p->rows * C), 256, 0, s>>>(p->rbuf, x, p->rows, C, p->cols, size);
    /* inverse twiddle: the rows are back in bit-reversed column order (as after the forward row pass) */
    k_twiddle<<<nblocks(p->rows * C), 256, 0, s>>>(x, p->rows, (size_t)r * p->rows, p->logC, p->twr_i, p->twc_i, m, 0);
    ntt_inv(p->ctx, x, p->logC, p->rows, s);                                       /* rows: length-C inverse, x C^-1 */
}
void dist_inv(dist_plan *p, uint64_t *x, hipStream_t s) { dist_inv_pre(p, x, s); dist_inv_post(p, x, s); }
