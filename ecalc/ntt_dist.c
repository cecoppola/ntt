/* ntt_dist.c - see ntt_dist.h */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include "vpe.h"
#include "ntt_dist.h"
#include "modarith.h"
#include <time.h>
#include <pthread.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)

__device__ static inline unsigned brev(unsigned v, int bits) { return __brev(v) >> (32 - bits); }

/* the twiddle convention of the kernels below: x[i][jb] *= w_n^(i * j), i = global row, j = brev(jb) (the row pass left columns bit-reversed).
 * w_n^(i j) = twr[(i j) >> logC] * twc[(i j) & (C-1)] with twr[k] = w_n^(k C) = w_R^k, twc[k] = w_n^k. */
/* after the all-to-all: rb holds size slabs of the chunk (rows [i0, i0 + rows_k) of every rank), slab r = (my cols) x
 * (rank r's chunk rows), column-major -> columns of R points: x[jl R + r rows + i0 + il] */
__global__ void k_unpack(const uint64_t *rb, uint64_t *x, size_t rows_k, size_t i0, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * rows_k * size, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t jl = t / (rows_k * size), i = t % (rows_k * size), r = i / rows_k, il = i % rows_k;
        x[jl * R + r * rows + i0 + il] = rb[r * (cols * rows_k) + jl * rows_k + il];
    }
}
/* the reverse pair for the inverse: columns -> slabs (the chunk: rows [i0, i0 + rows_k) of every rank) -> rows */
__global__ void k_pack_cols(const uint64_t *x, uint64_t *sb, size_t rows_k, size_t i0, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * rows_k * size, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t jl = t / (rows_k * size), i = t % (rows_k * size), r = i / rows_k, il = i % rows_k;
        sb[r * (cols * rows_k) + jl * rows_k + il] = x[jl * R + r * rows + i0 + il];
    }
}

static uint64_t *dev_pow_table(int prime, uint64_t w, size_t cnt)
{
    uint64_t p = ec_P[prime], *h = (uint64_t *)malloc(cnt * 8), *d, a = 1;
    for (size_t k = 0; k < cnt; k++) { h[k] = a; a = ec_mulmod_ref(a, w, p); }
    HIP_CHECK(hipMalloc(&d, cnt * 8)); HIP_CHECK(hipMemcpy(d, h, cnt * 8, hipMemcpyHostToDevice)); free(h);
    return d;
}
static void plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, int slabs);
void dist_plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC) { plan_create(p, cm, ctx, prime, logR, logC, 1); }
static void plan_create(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, int slabs)
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
    p->own_slabs = slabs; p->sbuf = p->rbuf = 0;
    /* the slab buffers from the transport's symmetric pool where it has one (comm_sym_alloc: the xshmem
     * transport puts straight from sbuf into the receivers' rbuf, no staging), else hipMalloc'd. */
    if (slabs) {
        p->sbuf = (uint64_t *)comm_sym_alloc(cm, bytes); p->rbuf = p->sbuf ? (uint64_t *)comm_sym_alloc(cm, bytes) : 0;
        if (p->sbuf && p->rbuf) p->own_slabs = 2;
        else { if (p->sbuf) comm_sym_free(cm, p->sbuf); HIP_CHECK(hipMalloc(&p->sbuf, bytes)); HIP_CHECK(hipMalloc(&p->rbuf, bytes)); }
    }
    /* M7: K chunks of rows/K >= 32 rows (a power of two; DIST_CHUNKS, default 8); 1 over the synthetic communicator */
    int K = getenv("DIST_CHUNKS") ? atoi(getenv("DIST_CHUNKS")) : vpe_on() ? 1 : 8;   /* S1b: vPE (flat xshmem communicator, one exchange at a time, 64 KiB messages at 8 chunks) = one exchange per transform unless DIST_CHUNKS says otherwise */
    if (K < 1 || cm->inflight == 0) K = 1;
    while (K > 1 && (K & (K - 1))) K--;
    while (K > 1 && p->rows / K < 32) K >>= 1;
    p->K = K; p->k_resume = 0;
    HIP_CHECK(hipStreamCreateWithFlags(&p->ts, hipStreamNonBlocking));
    HIP_CHECK(hipEventCreateWithFlags(&p->ev, hipEventDisableTiming)); HIP_CHECK(hipEventCreateWithFlags(&p->evt, hipEventDisableTiming));
    HIP_CHECK(hipEventCreateWithFlags(&p->evr, hipEventDisableTiming));
    p->te = 0; p->tk = 0; p->nte = p->te_cap = 0;
}
void dist_plan_create_shared(dist_plan *p, comm *cm, ntt_ctx *ctx, int prime, int logR, int logC, uint64_t *sbuf, uint64_t *rbuf)
{
    plan_create(p, cm, ctx, prime, logR, logC, 0);
    p->sbuf = sbuf; p->rbuf = rbuf;
}
void dist_plan_free(dist_plan *p)
{
    HIP_CHECK(hipFree(p->twr)); HIP_CHECK(hipFree(p->twc)); HIP_CHECK(hipFree(p->twr_i)); HIP_CHECK(hipFree(p->twc_i));
    if (p->own_slabs == 2) { comm_sym_free(p->cm, p->sbuf); comm_sym_free(p->cm, p->rbuf); }
    else if (p->own_slabs) { HIP_CHECK(hipFree(p->sbuf)); HIP_CHECK(hipFree(p->rbuf)); }
    HIP_CHECK(hipStreamDestroy(p->ts)); HIP_CHECK(hipEventDestroy(p->ev)); HIP_CHECK(hipEventDestroy(p->evt)); HIP_CHECK(hipEventDestroy(p->evr));
    for (int i = 0; i < p->te_cap; i++) HIP_CHECK(hipEventDestroy(p->te[i]));
    free(p->te); free(p->tk);
}
static unsigned nblocks(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }

struct dist_stats dist_st;
static pthread_mutex_t st_mx = PTHREAD_MUTEX_INITIALIZER;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
/* C5: the parts are timed by events on the stream they run on (the compute stream s, the transfer stream ts): a
 * begin/end pair per part; st_flush at the end of a transform synchronises the events and sums the pairs.  Kinds:
 * 0 rows, 1 cols, 2 pack/unpack, 3 exchange; bit 4 = end. */
enum { ST_ROWS, ST_COLS, ST_PACK, ST_XFER, ST_END = 16 };
static void st_mark(dist_plan *p, hipStream_t on, int kind)
{
    if (!dist_st.on) return;
    if (p->nte == p->te_cap) {
        int cap = p->te_cap ? 2 * p->te_cap : 64;
        p->te = (hipEvent_t *)realloc(p->te, cap * sizeof *p->te); p->tk = (unsigned char *)realloc(p->tk, cap);
        for (int i = p->te_cap; i < cap; i++) HIP_CHECK(hipEventCreate(&p->te[i]));
        p->te_cap = cap;
    }
    HIP_CHECK(hipEventRecord(p->te[p->nte], on)); p->tk[p->nte++] = (unsigned char)kind;
}
static void st_flush(dist_plan *p)
{
    if (!dist_st.on || !p->nte) return;
    double sum[4] = { 0, 0, 0, 0 }; int open_[4] = { -1, -1, -1, -1 };
    for (int i = 0; i < p->nte; i++) HIP_CHECK(hipEventSynchronize(p->te[i]));
    for (int i = 0; i < p->nte; i++) {
        int k = p->tk[i] & 15;
        if (!(p->tk[i] & ST_END)) { open_[k] = i; continue; }
        float ms = 0; if (open_[k] >= 0) { HIP_CHECK(hipEventElapsedTime(&ms, p->te[open_[k]], p->te[i])); sum[k] += ms * 1e-3; }
        open_[k] = -1;
    }
    float tot = 0; HIP_CHECK(hipEventElapsedTime(&tot, p->te[0], p->te[p->nte - 1]));
    pthread_mutex_lock(&st_mx);
    dist_st.t_local1 += sum[ST_ROWS]; dist_st.t_local2 += sum[ST_COLS]; dist_st.t_pack += sum[ST_PACK]; dist_st.t_xfer += sum[ST_XFER]; dist_st.t_total += tot * 1e-3;
    pthread_mutex_unlock(&st_mx);
    p->nte = 0;
}
static void st_host(double dt) { if (!dist_st.on) return; pthread_mutex_lock(&st_mx); dist_st.t_a2a += dt; pthread_mutex_unlock(&st_mx); }
#define TS(kind, stmt) do { st_mark(p, s, kind); stmt; st_mark(p, s, (kind) | ST_END); } while (0)
/* ---- the twiddled pack/unpack with the twiddle by a row recurrence.  A plain form would
 * gather twr and twc for every point (w_n^(i j) = twr[i j >> logC] twc[i j mod C]; j = brev(jb) scatters the gathers over the
 * tables: 64 cache lines per 32-point tile row) and run at 1.2-1.5 TB/s against a 3 TB/s copy (measured).  Here a thread's four
 * rows i, i + 8, i + 16, i + 24 of column j take w_n^(i j) from the tables once and step by w_n^(8 j) (one per column, in LDS):
 * two gathers per four points instead of eight (block 32 x 8).  ec_mm returns canonical residues, so w_n^(i j) w_n^(8 j) = w_n^((i + 8) j)
 * exactly: bit-identical to the plain forms. */
__device__ static inline double tw_base(size_t e, int logC, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    return ec_mm((double)twr[e >> logC], (double)twc[e & (((size_t)1 << logC) - 1)], m.p, m.pinv);
}
template <int BY> __global__ void k_twpack_r(const uint64_t *x, uint64_t *sb, size_t rows, size_t row0, int logC, size_t cols, int size, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    __shared__ double stp[32];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    size_t jb = bj + tx, j = brev((unsigned)jb, logC);
    if (ty == 0) stp[tx] = tw_base((size_t)BY * j, logC, twr, twc, m);
    double w = tw_base((row0 + bi + ty) * j, logC, twr, twc, m);
    __syncthreads();
    double st = stp[tx];
    for (int k = 0; k < 32; k += BY) {
        size_t il = bi + ty + k;
        tile[ty + k][tx] = (uint64_t)ec_mm((double)x[il * C + jb], w, m.p, m.pinv);
        if (k < 32 - BY) w = ec_mm(w, st, m.p, m.pinv);
    }
    __syncthreads();
    for (int k = 0; k < 32; k += BY) {
        size_t jw = bj + ty + k, il = bi + tx, sl = jw / cols, jl = jw % cols;
        sb[sl * (cols * rows) + jl * rows + il] = tile[tx][ty + k];
    }
}
template <int BY> __global__ void k_unpacktw_r(const uint64_t *rb, uint64_t *x, size_t rows, size_t row0, int logC, size_t cols, int size, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    __shared__ double stp[32];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += BY) {
        size_t jr = bj + ty + k, il = bi + tx, sl = jr / cols, jl = jr % cols;
        tile[ty + k][tx] = rb[sl * (cols * rows) + jl * rows + il];
    }
    size_t jb = bj + tx, j = brev((unsigned)jb, logC);
    if (ty == 0) stp[tx] = tw_base((size_t)BY * j, logC, twr, twc, m);
    double w = tw_base((row0 + bi + ty) * j, logC, twr, twc, m);
    __syncthreads();
    double st = stp[tx];
    for (int k = 0; k < 32; k += BY) {
        size_t il = bi + ty + k;
        x[il * C + jb] = (uint64_t)ec_mm((double)ec_fold(tile[tx][ty + k], m.pu), w, m.p, m.pinv);
        if (k < 32 - BY) w = ec_mm(w, st, m.p, m.pinv);
    }
}
/* tiled unpack: slab r (my cols x rank r's rows, column-major per column) -> my columns of R points: reads
 * coalesced along il, writes coalesced along i = r rows + il -- both contiguous, no tile needed */
/* ---- the chunk pipeline (M7) ---- */
#define TSK(kind, stmt) TS(kind, stmt)
/* DIST_STATS=1 (diagnostic, off by default): host time per phase of the dist transforms with the compute stream drained around each
 * phase (so it distorts overlap, never digits); per thread, printed and zeroed by dist_phase_dump (mn_core, RNS_VERBOSE) */
static __thread double dph[16]; static __thread int dph_dir; static __thread int dph_on;
static inline int dph_chk(void) { dph_on = dist_st.on; return dph_on; }
#define PH(i, s_, stmt) do { if (dph_chk()) { HIP_CHECK(hipStreamSynchronize(s_)); double t_ = tnow(); stmt; HIP_CHECK(hipStreamSynchronize(s_)); dph[dph_dir * 8 + (i)] += tnow() - t_; } else { stmt; } } while (0)
void dist_phase_dump(int pe)
{
    if (!dph_chk()) return;
    static const char *nm[8] = { "prod", "post", "wait", "cons", "cols", "rbfree", "colinv", "pack" };
    printf("   dist phases pe %d (s, drained):", pe);
    for (int d = 0; d < 2; d++) { printf(" %s{", d ? "inv" : "fwd"); for (int i = 0; i < 8; i++) { if (dph[d * 8 + i] != 0) printf(" %s %.3f", nm[i], dph[d * 8 + i]); dph[d * 8 + i] = 0; } printf(" }"); }
    printf("\n");
}
/* chunk k: rows [k rows_k, (k+1) rows_k) of this rank; its region of the slab buffers */
static inline size_t chunk_rows(const dist_plan *p) { return p->rows / p->K; }
static inline size_t chunk_limbs(const dist_plan *p) { return p->cols * chunk_rows(p); }      /* per slab */
static inline uint64_t *chunk_sb(const dist_plan *p, int k) { return p->sbuf + (size_t)k * comm_size(p->cm) * chunk_limbs(p); }
static inline uint64_t *chunk_rb(const dist_plan *p, int k) { return p->rbuf + (size_t)k * comm_size(p->cm) * chunk_limbs(p); }
/* post chunk k's exchange on the transfer stream once the compute stream has produced it (event ev) */
static void chunk_post_(dist_plan *p, int k)
{
    HIP_CHECK(hipStreamWaitEvent(p->ts, p->ev, 0));
    st_mark(p, p->ts, ST_XFER);
    double t0 = dist_st.on ? tnow() : 0;
    comm_alltoall(p->cm, chunk_sb(p, k), chunk_rb(p, k), chunk_limbs(p) * 8, p->ts);
    if (dist_st.on) st_host(tnow() - t0);
    st_mark(p, p->ts, ST_XFER | ST_END);
}
/* wait for chunk k (the k-th wait) and make the compute stream follow the transfer stream */
static void chunk_wait_(dist_plan *p, hipStream_t s)
{
    double t0 = dist_st.on ? tnow() : 0;
    comm_wait(p->cm);
    if (dist_st.on) st_host(tnow() - t0);
    HIP_CHECK(hipEventRecord(p->evt, p->ts)); HIP_CHECK(hipStreamWaitEvent(s, p->evt, 0));
}
/* a transport that PUSHES into the receivers' slab buffers (comm_xgmi: a kernel on the
 * sender's device stores into rbuf on the receiver's device as soon as every rank has entered the exchange) writes rbuf while
 * the receiver's compute stream may still hold the previous transform's unpacks of the same chunk: nothing on the GPU orders
 * the sender's transfer stream after the receiver's compute stream, and the exchange's barrier orders only the host threads.
 * The xshmem transport synchronises the caller's stream before it publishes / receives (comm_xshmem.c), the
 * layered one pushes only into its own scratch (or synchronises first, minor); comm_xgmi did not.  So before the first post of
 * a transform the host waits for evr -- recorded on s at the transform's start, i.e. every earlier read of rbuf on this rank --
 * and the exchange's barrier then holds every sender until every receiver has passed that point.  The later posts of the same
 * transform need nothing (each chunk has its own region of rbuf, written once per transform).  The wait is for work queued
 * before the row pass that follows it, so the GPU never idles on it; digits identical (an ordering, no arithmetic). */
static void rb_free_wait_(dist_plan *p) { HIP_CHECK(hipEventSynchronize(p->evr)); }
static void chunk_post(dist_plan *p, int k) { PH(1, p->ts, chunk_post_(p, k)); }
static void chunk_wait(dist_plan *p, hipStream_t s) { PH(2, s, chunk_wait_(p, s)); }
static void rb_free_wait(dist_plan *p) { double t_ = dph_chk() ? tnow() : 0; rb_free_wait_(p); if (dph_on) dph[dph_dir * 8 + 5] += tnow() - t_; }
static inline int depth(const dist_plan *p) { int D = p->cm->inflight; if (D < 1) D = 1; if (D > p->K) D = p->K; return D; }
/* forward, chunk k: the row pass of its rows, twiddle and pack into its slab region */
static void fwd_prod(dist_plan *p, uint64_t *x, int k, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC, rk = chunk_rows(p), i0 = (size_t)k * rk;
    ec_mod m = ec_mod_get(p->prime);
    TSK(ST_ROWS, ntt_fwd(p->ctx, x + i0 * C, p->logC, rk, s));                    /* rows: length-C, bit-reversed columns */
    dim3 grid((unsigned)(C / 32), (unsigned)(rk / 32)), blk(32, 8);
    TSK(ST_PACK, (k_twpack_r<8><<<grid, blk, 0, s>>>(x + i0 * C, chunk_sb(p, k), rk, (size_t)r * p->rows + i0, p->logC, p->cols, size, p->twr, p->twc, m)));
    HIP_CHECK(hipGetLastError());   /* B8: a bad grid (rk or C/32 rounding to 0 blocks) launches silently -- no HIP error, just no work -- so a launch check alone would not have caught it; mn.c's logR growth is the real fix, this catches any other mis-sized launch */
    HIP_CHECK(hipEventRecord(p->ev, s));
}
static void fwd_cons(dist_plan *p, uint64_t *x, int k, hipStream_t s)
{
    int size = comm_size(p->cm); size_t rk = chunk_rows(p);
    TSK(ST_PACK, (k_unpack<<<nblocks(p->cols * rk * size), 256, 0, s>>>(chunk_rb(p, k), x, rk, (size_t)k * rk, p->rows, p->cols, size)));
}
/* the forward up to the last chunk's post: every chunk's row pass and pack are enqueued before any unpack (the
 * unpack writes the column layout over the whole of x), so the unpacks of chunks 0 .. K-2 run under the last exchange */
void dist_fwd_pre(dist_plan *p, uint64_t *x, hipStream_t s)
{
    int K = p->K, D = depth(p);
    dph_dir = 0;
    HIP_CHECK(hipEventRecord(p->evr, s));                /* WM15 H1: the previous transform's reads of rbuf */
    PH(0, s, fwd_prod(p, x, 0, s)); rb_free_wait(p); chunk_post(p, 0);
    for (int k = 1; k < K; k++) { PH(0, s, fwd_prod(p, x, k, s)); if (D < 2) chunk_wait(p, s); chunk_post(p, k); }
    for (int k = 0; k < K - 1; k++) { if (D >= 2) chunk_wait(p, s); PH(3, s, fwd_cons(p, x, k, s)); }
}
void dist_fwd_post(dist_plan *p, uint64_t *x, hipStream_t s)
{
    dph_dir = 0;
    chunk_wait(p, s);
    PH(3, s, fwd_cons(p, x, p->K - 1, s));
    PH(4, s, TS(ST_COLS, ntt_fwd(p->ctx, x, p->logR, p->cols, s)));                          /* columns: length-R */
    st_flush(p);
}
void dist_fwd(dist_plan *p, uint64_t *x, hipStream_t s) { dist_fwd_pre(p, x, s); dist_fwd_post(p, x, s); }
void dist_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s)
{
    ntt_pw(p->ctx, x, y, p->cols * ((size_t)1 << p->logR), s);
}
/* inverse: the column pass, then every chunk packed (the pack reads the column layout over the whole of x, so all
 * packs precede the first unpack), D chunks posted; the loop waits chunk k, unpacks and row-inverts it under the
 * exchange of chunk k+1 (k+2 with two in flight) and posts the next.  _pre ends with the last post. */
static void inv_cons(dist_plan *p, uint64_t *x, int k, hipStream_t s)
{
    int size = comm_size(p->cm), r = comm_rank(p->cm);
    size_t C = (size_t)1 << p->logC, rk = chunk_rows(p), i0 = (size_t)k * rk;
    ec_mod m = ec_mod_get(p->prime);
    dim3 grid((unsigned)(C / 32), (unsigned)(rk / 32)), blk(32, 8);
    TSK(ST_PACK, (k_unpacktw_r<8><<<grid, blk, 0, s>>>(chunk_rb(p, k), x + i0 * C, rk, (size_t)r * p->rows + i0, p->logC, p->cols, size, p->twr_i, p->twc_i, m)));
    TSK(ST_ROWS, ntt_inv(p->ctx, x + i0 * C, p->logC, rk, s));                     /* rows: length-C inverse, x C^-1 */
}
static void inv_prod(dist_plan *p, const uint64_t *x, int k, hipStream_t s)
{
    int size = comm_size(p->cm); size_t rk = chunk_rows(p);
    TSK(ST_PACK, (k_pack_cols<<<nblocks(p->cols * rk * size), 256, 0, s>>>(x, chunk_sb(p, k), rk, (size_t)k * rk, p->rows, p->cols, size)));
}
static void inv_loop(dist_plan *p, uint64_t *x, hipStream_t s, int kend)
{
    int K = p->K, D = depth(p);
    dph_dir = 1;
    for (int k = p->k_resume; k < kend; k++) {
        chunk_wait(p, s); PH(3, s, inv_cons(p, x, k, s));
        if (k + D < K) chunk_post(p, k + D);
    }
    p->k_resume = kend;
}
/* y != 0 fuses the pointwise product x <- x y into the column
 * inverse's first pass (ntt_inv_pw_y, FULL layout: y in the same column layout as x) -- the same modmul as dist_pw on the
 * same canonical operands, so bit-identical to dist_pw + dist_inv, one plane read and write per prime less */
static void inv_pre_y(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s)
{
    int size = comm_size(p->cm), K = p->K, D = depth(p);
    size_t rk = chunk_rows(p);
    dph_dir = 1;
    HIP_CHECK(hipEventRecord(p->evr, s));                /* WM15 H1: the previous transform's reads of rbuf */
    if (y) PH(6, s, TS(ST_COLS, ntt_inv_pw_y(p->ctx, x, y, NTT_Y_FULL, p->logR, p->cols, s)));   /* columns: pointwise + length-R inverse, x R^-1, natural */
    else PH(6, s, TS(ST_COLS, ntt_inv(p->ctx, x, p->logR, p->cols, s)));                         /* columns: length-R inverse, x R^-1, natural */
    for (int k = 0; k < K; k++) PH(7, s, inv_prod(p, x, k, s));
    HIP_CHECK(hipEventRecord(p->ev, s));
    rb_free_wait(p);
    for (int k = 0; k < D; k++) chunk_post(p, k);
    p->k_resume = 0;
    inv_loop(p, x, s, K - D);                             /* through the last post (chunk K-1, posted at k = K-1-D) */
    (void)rk; (void)size;
}
void dist_inv_pre(dist_plan *p, uint64_t *x, hipStream_t s) { inv_pre_y(p, x, 0, s); }
void dist_inv_post(dist_plan *p, uint64_t *x, hipStream_t s) { inv_loop(p, x, s, p->K); st_flush(p); }
void dist_inv(dist_plan *p, uint64_t *x, hipStream_t s) { dist_inv_pre(p, x, s); dist_inv_post(p, x, s); }
void dist_inv_pw(dist_plan *p, uint64_t *x, const uint64_t *y, hipStream_t s) { inv_pre_y(p, x, y, s); dist_inv_post(p, x, s); }   /* A6: = dist_pw + dist_inv */
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
