/* rns_dist.c - the distributed product tier (WP5): C = A * B through the 4-APU
 * four-step transform (ntt_dist over comm_xgmi).
 *
 * n = 2^(logR+logC) >= na + nb points, rows = R/4, cols = C/4 per rank.  Rank r
 * (device r) holds, per prime, its block-cyclic rows of A and B (point
 * m = i + R j, rows i in [r R/4, (r+1) R/4)), gathered from the operands in
 * column runs (limbs R j + r R/4 .. + R/4: coalesced reads) wherever they live
 * (host registered memory or dbig quarters on any APU); forward transforms
 * (one all-to-all each), pointwise, inverse (one all-to-all) -> the rank holds
 * its block-cyclic rows of the product for all four primes.  A local transpose
 * makes each column run contiguous, the CRT treats every run as a stripe (into
 * a local buffer), the runs are scattered to their limb positions in the result
 * and the run spills are added with atomics.  Primes are done one at a time on
 * the B side so five planes and two slabs suffice (5.5 n/4 points per device):
 * up to 2^31 points in the standard pools (4 q in one, 3 q in the other).  3 all-to-alls per product.
 * Operands and results: host bigints (staged) or device bigints (dbig). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "rns_mul.h"
#include "rns_int.h"
#include "ntt_dist.h"
#include "dbig.h"
#include "mdb.h"
#include "mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
#define DIST_LOGN_MAX 31
/* the plane cap; DIST_LOGN_TEST lowers it (tests only) so the grid split runs at small sizes */
static int dist_logn_max(void) { const char *e = getenv("DIST_LOGN_TEST"); int v = e ? atoi(e) : DIST_LOGN_MAX; return v < 20 || v > DIST_LOGN_MAX ? DIST_LOGN_MAX : v; }

/* a limb accessor: either one flat array or four quarters (a dbig view) */
struct acc { const uint64_t *q[NR]; uint64_t *w[NR]; size_t qc, lo, n; int flat; dbig *owner; };
__device__ static inline uint64_t acc_get(const struct acc a, size_t i)      /* i < n */
{
    size_t m = a.lo + i;
    if (a.flat) return a.q[0][m];
    size_t d = (m >= a.qc) + (m >= 2 * a.qc) + (m >= 3 * a.qc); return a.q[d][m - d * a.qc];
}
__device__ static inline uint64_t *acc_ptr(const struct acc a, size_t i)
{
    size_t m = a.lo + i;
    if (a.flat) return a.w[0] + m;
    size_t d = (m >= a.qc) + (m >= 2 * a.qc) + (m >= 3 * a.qc); return a.w[d] + (m - d * a.qc);
}
static struct acc acc_flat(const uint64_t *p, size_t n) { struct acc a; memset(&a, 0, sizeof a); a.q[0] = p; a.w[0] = (uint64_t *)p; a.n = n; a.flat = 1; return a; }
static struct acc acc_db(const dbig *x, size_t lo, size_t n) { struct acc a; memset(&a, 0, sizeof a); for (int d = 0; d < NR; d++) { a.q[d] = x->q[d]; a.w[d] = x->q[d]; } a.qc = x->qc; a.lo = x->off + lo; a.n = n; a.owner = (dbig *)x; return a; }

struct rank_state {
    comm *cm; ntt_ctx *ctx[EC_NP]; hipStream_t s;
    dpool desc; uint64_t *spill; size_t spill_cap;
    struct { dist_plan pl; int logR, logC, built; comm *cm; } plan[EC_NP];   /* cm: the plan's communicator (the node's xGMI, or a group's layered one) */
};
static struct rank_state RS[NR];
static int g_init;
static uint64_t *g_stage; static size_t g_stage_cap;      /* registered host staging for unregistered host operands/result */
struct rns_dist_stats rns_dist_st;

static void rank_init(int r)
{
    struct rank_state *v = &RS[r];
    HIP_CHECK(hipSetDevice(r));
    v->cm = comm_xgmi_create(r);
    for (int p = 0; p < EC_NP; p++) v->ctx[p] = ntt_ctx_create(p);
    HIP_CHECK(hipStreamCreate(&v->s));
}
/* gather rank r's rows of an operand: x[il C + j] = canon(src[R j + r rows + il]) or 0 */
__global__ void k_gather(uint64_t *x, struct acc src, size_t R, size_t rows, size_t row0, size_t C, ec_mod m)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t % rows, mm = R * j + row0 + il;
        x[il * C + j] = mm < src.n ? ec_canon64(acc_get(src, mm), m.pu, m.mu) : 0;
    }
}
static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }
__global__ void k_transpose(const uint64_t *x, uint64_t *y, size_t rows, size_t C)   /* rows x C -> C x rows */
{
    __shared__ uint64_t tile[32][33];
    size_t bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) { size_t i = bi + ty + k, j = bj + tx; if (i < rows && j < C) tile[ty + k][tx] = x[i * C + j]; }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) { size_t j = bj + ty + k, i = bi + tx; if (i < rows && j < C) y[j * rows + i] = tile[tx][ty + k]; }
}
/* run j of the local CRT output -> result limbs R j + row0 .. (within nc) */
__global__ void k_scatter_runs(const uint64_t *loc, struct acc c, size_t R, size_t rows, size_t row0, size_t C)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t j = t / rows, il = t % rows, mm = R * j + row0 + il; if (mm < c.n) *acc_ptr(c, mm) = loc[t]; }
}
/* this rank's run spills into a sparse temporary S (4 limbs at limb R j + (r+1) rows for every column j);
 * C += S is then one chunked-carry addition (db_add) -- an atomic ripple was serial per run and
 * pathological on long carry chains */

/* ---- Phase 9 C5 (PLAN 16, I10; DIST_R3=1, measured only): planes of 3 2^k points for the single-node products of
 * 2^30 points and above (n = 3 2^(logn-2) when nc fits it: 3 2^28, 3 2^29, 3 2^30 -- the last above the 2^31 cap, so
 * the top product's grid has fewer, fuller planes).  The four-step with rows of length C = 3 2^logk through the
 * radix-3 layer (ntt3.c: position t of third r holds X[3 brev(t) + r]) and columns of R = 2^logR; the twiddle
 * w_n^(i j) = twr[e / C] twc[e % C] (twr[k] = w_R^k, twc[k] = w_n^k, w_n the 3 2^(logR+logk)-th root).  The planes
 * xa[4] take 4 q = 3 2^30 limbs of pool 0 (grown to 32 GiB per APU); xb and the slabs (3 q + 16) come from the dbig
 * block pool (pool 1's tail is donated to it at 3 q of the 2^31 layout).  Same comm (xGMI), same CRT. */
static int dist_r3(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_R3"); v = e ? atoi(e) : 0; if (v && !ec_has_radix3()) { fprintf(stderr, "DIST_R3: the prime set has no 3 2^k roots\n"); v = 0; } } return v; }
static size_t dist_cap(void) { return dist_r3() ? (size_t)3 << (dist_logn_max() - 1) : (size_t)1 << dist_logn_max(); }   /* plane points */
struct plan3 { int built, logR, logk; uint64_t *twr, *twc, *twr_i, *twc_i; };
static struct plan3 P3[NR][EC_NP];
static uint64_t *pow_table(int prime, uint64_t w, size_t cnt)
{
    uint64_t p = ec_P[prime], *h = (uint64_t *)malloc(cnt * 8), *d, a = 1;
    for (size_t k = 0; k < cnt; k++) { h[k] = a; a = ec_mulmod_ref(a, w, p); }
    HIP_CHECK(hipMalloc(&d, cnt * 8)); HIP_CHECK(hipMemcpy(d, h, cnt * 8, hipMemcpyHostToDevice)); free(h);
    return d;
}
static void plan3_get(struct plan3 *q, int prime, int logR, int logk)
{
    if (q->built && q->logR == logR && q->logk == logk) return;
    if (q->built) { HIP_CHECK(hipFree(q->twr)); HIP_CHECK(hipFree(q->twc)); HIP_CHECK(hipFree(q->twr_i)); HIP_CHECK(hipFree(q->twc_i)); }
    uint64_t wn = ec_root3(prime, logR + logk), wR = ec_root(prime, logR), pp = ec_P[prime];
    q->twr = pow_table(prime, wR, (size_t)1 << logR); q->twc = pow_table(prime, wn, (size_t)3 << logk);
    q->twr_i = pow_table(prime, ec_inv(wR, pp), (size_t)1 << logR); q->twc_i = pow_table(prime, ec_inv(wn, pp), (size_t)3 << logk);
    q->built = 1; q->logR = logR; q->logk = logk;
}
__device__ static inline unsigned brev3(unsigned v, int bits) { return __brev(v) >> (32 - bits); }
/* the logical column j of position jb of a radix-3 row: third r = jb >> logk, t = jb mod 2^logk -> 3 brev(t) + r */
__device__ static inline size_t col3(size_t jb, int logk) { size_t m = (size_t)1 << logk; return 3 * (size_t)brev3((unsigned)(jb & (m - 1)), logk) + (jb >> logk); }
/* twiddle + pack (rows x C row-major -> slab s = the columns of rank s, column-major per slab) */
__global__ void k3_twpack(const uint64_t *x, uint64_t *sb, size_t rows, size_t row0, int logk, size_t cols, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)3 << logk, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx, i = row0 + il, e = i * col3(jb, logk);
        double w = ec_mm((double)twr[e / C], (double)twc[e % C], m.p, m.pinv);
        tile[ty + k][tx] = (uint64_t)ec_mm((double)x[il * C + jb], w, m.p, m.pinv);
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) { size_t jb = bj + ty + k, il = bi + tx, s = jb / cols, jl = jb % cols; sb[s * (cols * rows) + jl * rows + il] = tile[tx][ty + k]; }
}
__global__ void k3_unpack(const uint64_t *rb, uint64_t *x, size_t rows, size_t cols, int size)   /* slabs -> my columns of R points */
{
    size_t R = rows * size, total = cols * R, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t jl = t / R, i = t % R, r = i / rows, il = i % rows; x[jl * R + i] = rb[r * (cols * rows) + jl * rows + il]; }
}
__global__ void k3_pack_cols(const uint64_t *x, uint64_t *sb, size_t rows, size_t cols, int size)
{
    size_t R = rows * size, total = cols * R, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t jl = t / R, i = t % R, r = i / rows, il = i % rows; sb[r * (cols * rows) + jl * rows + il] = x[t]; }
}
__global__ void k3_unpacktw(const uint64_t *rb, uint64_t *x, size_t rows, size_t row0, int logk, size_t cols, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)3 << logk, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) { size_t jb = bj + ty + k, il = bi + tx, s = jb / cols, jl = jb % cols; tile[ty + k][tx] = rb[s * (cols * rows) + jl * rows + il]; }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx, i = row0 + il, e = i * col3(jb, logk);
        double w = ec_mm((double)twr[e / C], (double)twc[e % C], m.p, m.pinv);
        x[il * C + jb] = (uint64_t)ec_mm((double)ec_fold(tile[tx][ty + k], m.pu), w, m.p, m.pinv);
    }
}
struct ctx3 { comm *cm; ntt_ctx *ctx; int prime, logR, logk; size_t rows, cols; uint64_t *sb, *rb; const struct plan3 *pl; };
static void dist3_fwd(const struct ctx3 *c, uint64_t *x, hipStream_t s)
{
    size_t C = (size_t)3 << c->logk, R = (size_t)1 << c->logR; ec_mod m = ec_mod_get(c->prime); int r = comm_rank(c->cm), size = comm_size(c->cm);
    ntt_fwd3(c->ctx, x, c->logk, c->rows, s);
    dim3 grid((unsigned)(C / 32), (unsigned)(c->rows / 32)), blk(32, 8);
    k3_twpack<<<grid, blk, 0, s>>>(x, c->sb, c->rows, (size_t)r * c->rows, c->logk, c->cols, c->pl->twr, c->pl->twc, m);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(c->cm, c->sb, c->rb, c->cols * c->rows * 8, s); comm_wait(c->cm);
    k3_unpack<<<nblk(c->cols * R), 256, 0, s>>>(c->rb, x, c->rows, c->cols, size);
    ntt_fwd(c->ctx, x, c->logR, c->cols, s);
}
static void dist3_inv(const struct ctx3 *c, uint64_t *x, hipStream_t s)
{
    size_t C = (size_t)3 << c->logk, R = (size_t)1 << c->logR; ec_mod m = ec_mod_get(c->prime); int r = comm_rank(c->cm), size = comm_size(c->cm);
    ntt_inv(c->ctx, x, c->logR, c->cols, s);
    k3_pack_cols<<<nblk(c->cols * R), 256, 0, s>>>(x, c->sb, c->rows, c->cols, size);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(c->cm, c->sb, c->rb, c->cols * c->rows * 8, s); comm_wait(c->cm);
    dim3 grid((unsigned)(C / 32), (unsigned)(c->rows / 32)), blk(32, 8);
    k3_unpacktw<<<grid, blk, 0, s>>>(c->rb, x, c->rows, (size_t)r * c->rows, c->logk, c->cols, c->pl->twr_i, c->pl->twc_i, m);
    ntt_inv3(c->ctx, x, c->logk, c->rows, s);
}
/* the core: C = A B, na + nb limbs of result through accessors; nc limbs written */
static void dist_core(struct acc A, struct acc B, struct acc Cw, size_t nc)
{
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    if (logn < 20) logn = 20;                                  /* R, C >= 2^10 */
    int r3 = dist_r3() && logn >= dist_logn_max() - 1 && nc <= ((size_t)3 << (logn - 2));   /* C5: 3 2^(logn-2) points instead of 2^logn (the top three sizes: 3 2^28 .. 3 2^30 at the 2^31 cap) */
    if (r3) logn--;                                            /* the 2^k length whose pool this replaces: n = 3 2^(logn-1) */
    if (logn > dist_logn_max()) { fprintf(stderr, "dist_core: %zu limbs > 2^%d points\n", nc, dist_logn_max()); exit(1); }
    int logR = r3 ? (logn - 1) / 2 : logn / 2, logk = logn - 1 - logR, logC = r3 ? 0 : logn - logR;
    size_t n = r3 ? (size_t)3 << (logn - 1) : (size_t)1 << logn, R = (size_t)1 << logR, C = r3 ? (size_t)3 << logk : (size_t)1 << logC, rows = R / NR, q = n / NR;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    double tl[NR], tf[NR], tc[NR];
#pragma omp parallel num_threads(NR)
    {
        int r = omp_get_thread_num(); struct rank_state *v = &RS[r];
        HIP_CHECK(hipSetDevice(r));
        /* planes: xa[4] in pool 0 (4 q = the standard 2^pool_log limbs at n = 2^31); xb | sbuf | rbuf in pool 1 (3 q);
         * the transpose scratch reuses the slab buffers after the last inverse.  r3: xb and the slabs from the block pool */
        uint64_t *pl = (uint64_t *)rns_dpool(r, 0, (size_t)EC_NP * q * 8), *p1 = r3 ? db_pool_alloc(r, ((size_t)3 * q + 16) * 8) : (uint64_t *)rns_dpool(r, 1, (size_t)3 * q * 8);
        uint64_t *xa[EC_NP], *xb = p1, *sl = p1 + q + (r3 ? 16 : 0), *xt = sl;
        for (int p = 0; p < EC_NP; p++) xa[p] = pl + (size_t)p * q;
        struct ctx3 c3[EC_NP];
        for (int p = 0; p < EC_NP; p++) {
            if (r3) { plan3_get(&P3[r][p], p, logR, logk); struct ctx3 c = { v->cm, v->ctx[p], p, logR, logk, rows, C / NR, sl, sl + q, &P3[r][p] }; c3[p] = c; continue; }
            if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC || v->plan[p].cm != v->cm) {
                if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                dist_plan_create_shared(&v->plan[p].pl, v->cm, v->ctx[p], p, logR, logC, sl, sl + q);
                v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1; v->plan[p].cm = v->cm;
            }
        }
        double s0 = mem_now(), lg = 0, lf = 0;
        for (int p = 0; p < EC_NP; p++) {
            ec_mod m = ec_mod_get(p);
            double g0 = mem_now();
            k_gather<<<nblk(q), 256, 0, v->s>>>(xa[p], A, R, rows, (size_t)r * rows, C, m);
            k_gather<<<nblk(q), 256, 0, v->s>>>(xb, B, R, rows, (size_t)r * rows, C, m);
            HIP_CHECK(hipStreamSynchronize(v->s)); lg += mem_now() - g0;
            if (r3) { dist3_fwd(&c3[p], xa[p], v->s); dist3_fwd(&c3[p], xb, v->s); ntt_pw(v->ctx[p], xa[p], xb, q, v->s); dist3_inv(&c3[p], xa[p], v->s); }
            else {
            dist_fwd(&v->plan[p].pl, xa[p], v->s);
            dist_fwd(&v->plan[p].pl, xb, v->s);
            dist_pw(&v->plan[p].pl, xa[p], xb, v->s);
            dist_inv(&v->plan[p].pl, xa[p], v->s);
            }
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
        double s2 = mem_now(); lf = s2 - s0 - lg;
        /* CRT over this rank's runs: transpose each plane in place (via xt), one stripe per run into xb */
        for (int p = 0; p < EC_NP; p++) {
            dim3 grid((unsigned)((C + 31) / 32), (unsigned)((rows + 31) / 32)), blk(32, 8);
            k_transpose<<<grid, blk, 0, v->s>>>(xa[p], xt, rows, C);
            HIP_CHECK(hipMemcpyAsync(xa[p], xt, q * 8, hipMemcpyDeviceToDevice, v->s));
        }
        struct bdesc hd = { 0, 0, 0, xb, (uint32_t)q, 0, 0 };
        struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, r, sizeof hd);
        HIP_CHECK(hipMemcpyAsync(dd, &hd, sizeof hd, hipMemcpyHostToDevice, v->s));
        if (v->spill_cap < C) { if (v->spill) HIP_CHECK(hipFree(v->spill)); v->spill_cap = C + 16; HIP_CHECK(hipMalloc(&v->spill, v->spill_cap * 4 * 8)); }
        k_crt_batch<<<(unsigned)C, CRT_THREADS, 0, v->s>>>(xa[0], xa[1], xa[2], xa[3], dd, 0, (int)C, q, rns_gconst(), v->spill, bi_decimal);
        k_scatter_runs<<<nblk(q), 256, 0, v->s>>>(xb, Cw, R, rows, (size_t)r * rows, C);
        HIP_CHECK(hipStreamSynchronize(v->s));
        tl[r] = lg; tf[r] = lf; tc[r] = mem_now() - s2;
        if (r3) db_pool_free(r, p1);
    }
    /* the spills: device results add them as a sparse operand of the carry kernel; host results add them on the host */
    double tsp = mem_now();
    if (Cw.flat) {
        const uint64_t BB = EC_1E18; uint64_t *cc = Cw.w[0];
        uint64_t *hs = (uint64_t *)malloc(C * 4 * 8);
        for (int r = 0; r < NR; r++) {
            HIP_CHECK(hipSetDevice(r)); HIP_CHECK(hipMemcpy(hs, RS[r].spill, C * 4 * 8, hipMemcpyDeviceToHost));
            for (size_t j = 0; j < C; j++) {
                size_t k = R * j + (size_t)(r + 1) * rows; const uint64_t *w = hs + j * 4; uint64_t cy = 0;
                if (k >= nc) continue;
                if (bi_decimal) { for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = cc[k] + w[t] + cy; cy = sm >= BB; cc[k] = cy ? sm - BB : sm; } while (cy && k < nc) { uint64_t sm = cc[k] + cy; cy = sm >= BB; cc[k] = cy ? sm - BB : sm; k++; } }
                else { for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = cc[k] + w[t], c1 = sm < cc[k]; sm += cy; c1 += sm < cy; cc[k] = sm; cy = c1; } while (cy && k < nc) { uint64_t sm = cc[k] + cy; cy = sm < cy; cc[k] = sm; k++; } }
            }
        }
        free(hs);
    } else {
        dbig *Cd = Cw.owner; Cd->n = nc;
        const uint64_t *sp[4] = { RS[0].spill, RS[1].spill, RS[2].spill, RS[3].spill };
        db_add_spills(Cd, Cd, sp, R, rows, C, nc);               /* in place, one chunked-carry pass */
        if (Cd->n > nc) { fprintf(stderr, "dist: carry out of the product\n"); exit(1); }
    }
    rns_dist_st.t_merge += mem_now() - tsp;
    rns_dist_st.n++; rns_dist_st.t_total += mem_now() - t0;
    double ml = 0, mf = 0, mc = 0; for (int r = 0; r < NR; r++) { if (tl[r] > ml) ml = tl[r]; if (tf[r] > mf) mf = tf[r]; if (tc[r] > mc) mc = tc[r]; }
    rns_dist_st.t_load += ml; rns_dist_st.t_ntt += mf; rns_dist_st.t_crt += mc;
    if (getenv("RNS_VERBOSE")) printf("dist %s2^%d = 2^%d x %s2^%d (%zu limbs): load %.3f ntt %.3f crt %.3f spills %.3f total %.3f s\n", r3 ? "3*" : "", r3 ? logn - 1 : logn, logR, r3 ? "3*" : "", r3 ? logk : logC, nc, ml, mf, mc, mem_now() - tsp, mem_now() - t0);
    if (dist_st.on) { printf("   ntt parts (all ranks summed / 4): local rows %.3f cols %.3f pack+unpack %.3f all-to-all %.3f\n", dist_st.t_local1 / 4, dist_st.t_local2 / 4, dist_st.t_pack / 4, dist_st.t_a2a / 4);
                      dist_st.t_local1 = dist_st.t_local2 = dist_st.t_tw = dist_st.t_pack = dist_st.t_a2a = 0; }
}

/* host bigints: stage unregistered operands into registered memory */
void rns_mul_dist(bigint *Cout, const bigint *A, const bigint *B)
{
    size_t na = A->n, nb = B->n, nc = na + nb;
    if (!na || !nb) { Cout->n = 0; return; }
    double t0 = mem_now();
    const uint64_t *a = A->l, *b = B->l; uint64_t *c;
    int stage_a = !mem_is_registered(a, na * 8), stage_b = !mem_is_registered(b, nb * 8);
    bi_reserve(Cout, nc + 8);
    int stage_c = !mem_is_registered(Cout->l, (nc + 8) * 8);
    size_t need = (stage_a ? na : 0) + (stage_b ? nb : 0) + (stage_c ? nc + 8 : 0);
    if (need) {
        if (g_stage_cap < need) { if (g_stage) mem_hreg_free(g_stage); g_stage_cap = need + need / 8; g_stage = (uint64_t *)mem_hreg_alloc(g_stage_cap * 8); }
        uint64_t *s = g_stage;
        if (stage_a) { memcpy(s, a, na * 8); a = s; s += na; }
        if (stage_b) { memcpy(s, b, nb * 8); b = s; s += nb; }
        c = stage_c ? s : Cout->l;
    } else c = Cout->l;
    rns_dist_st.t_stage += mem_now() - t0;
    dist_core(acc_flat(a, na), acc_flat(b, nb), acc_flat(c, nc), nc);
    double t2 = mem_now();
    if (c != Cout->l) memcpy(Cout->l, c, nc * 8);
    Cout->n = nc; bi_norm(Cout);
    rns_dist_st.t_merge += mem_now() - t2;
}
/* host A (registered) x device B -> device C */
void rns_mul_dist_hd(dbig *Cd, const uint64_t *a, size_t na, const dbig *B)
{
    size_t nb = B->n, nc = na + nb;
    if (!na || !nb) { Cd->n = 0; return; }
    db_reserve(Cd, nc + 8);
    if (nc <= dist_cap()) {
        dist_core(acc_flat(a, na), acc_db(B, 0, nb), acc_db(Cd, 0, nc), nc);
        Cd->n = nc; db_norm(Cd);
        return;
    }
    size_t h = na / 2;                                         /* split A: C = a_lo B + (a_hi B) << h, a_lo B straight into C */
    dbig t2; db_init(&t2);
    size_t nlo = h; while (nlo && !a[nlo - 1]) nlo--;
    rns_mul_dist_hd(Cd, a, nlo, B);
    rns_mul_dist_hd(&t2, a + h, na - h, B);
    db_add_shifted(Cd, &t2, h, Cd);                            /* in place */
    db_free(&t2);
}
/* the low w limbs of A B (device operands): the grid below with the pieces above w skipped, then truncated
 * (the earlier halving recursion is no cheaper than the grid: for decimal's X Q at 4e10 both need 6 planes,
 * the skip makes it 5) */
static void mul_grid(dbig *Cd, const dbig *A, const dbig *B, size_t w);
void rns_mul_low_db(dbig *Cd, const dbig *A, const dbig *B, size_t w)
{
    dbig a = db_view(A, 0, A->n < w ? A->n : w), b = db_view(B, 0, B->n < w ? B->n : w);
    db_norm(&a); db_norm(&b);
    if (!a.n || !b.n || !w) { Cd->n = 0; return; }
    mul_grid(Cd, &a, &b, w);
    if (Cd->n > w) { Cd->n = w; db_norm(Cd); }
}
/* plane points for a product of nc limbs (dist_core rounds to 2^logn, at least 2^20) */
static size_t plane_pts(size_t nc)
{
    size_t n = (size_t)1 << 20; while (n < nc) { if (dist_r3() && n >= ((size_t)1 << (dist_logn_max() - 2)) && (n / 2 * 3) >= nc) return n / 2 * 3; n <<= 1; }   /* C5: 3 2^(k-1) between 2^k and 2^(k+1) */
    return n;
}
/* piece counts (ka, kb) for a product too long for one plane: every piece product ceil(na/ka) + ceil(nb/kb)
 * must fit 2^31 points; choose the grid with the fewest plane points in total (then the fewest products).
 * Halving the longer operand alone -- the first version -- gave 8 planes of 2^31 for the decimal top product
 * (2.22e9 x 2.22e9 limbs: halves of 1.11e9 still exceed a plane together); 2 x 3 pieces give 6 (RESULTS.md 66) */
static void split_grid_cap(size_t na, size_t nb, size_t cap, size_t minpts, int *ka, int *kb)   /* cap, minpts: plane points (the mn tier's differ) */
{
    size_t best = 0; *ka = *kb = 0;
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j;
        if (pa + pb > cap) continue;
        size_t pts = plane_pts(pa + pb); if (pts < minpts) pts = minpts;
        size_t cost = (size_t)i * j * pts;
        if (!*ka || cost < best || (cost == best && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
    if (!*ka) { fprintf(stderr, "split_grid: %zu x %zu limbs\n", na, nb); exit(1); }
}
static void split_grid(size_t na, size_t nb, int *ka, int *kb) { split_grid_cap(na, nb, dist_cap(), 0, ka, kb); }
/* device bigints: C = A B (nc limbs) in place in C's quarters; up to 2^31 points, larger products as a grid of
 * piece products (views, no copies): the first straight into C, the others through one temporary and a
 * shifted in-place add */
void rns_mul_dist_db(dbig *Cd, const dbig *A, const dbig *B) { mul_grid(Cd, A, B, (size_t)-1); }
/* the grid; only the pieces whose limbs start below w are formed (w = -1: all) */
static void mul_grid(dbig *Cd, const dbig *A, const dbig *B, size_t w)
{
    size_t na = A->n, nb = B->n, nc = na + nb;
    if (!na || !nb) { Cd->n = 0; return; }
    db_reserve(Cd, nc + 8);
    if (nc <= dist_cap()) {
        struct db_stats s0 = db_st; double t0 = mem_now();
        dist_core(acc_db(A, 0, na), acc_db(B, 0, nb), acc_db(Cd, 0, nc), nc);
        Cd->n = nc; db_norm(Cd);
        if (getenv("RNS_VERBOSE")) printf("   dist_db %zu limbs: %.3f s (dbig: shift %.3f addsub %.3f maxidx %.3f reserve %.3f)\n", nc, mem_now() - t0,
                                          db_st.t_shift - s0.t_shift, db_st.t_addsub - s0.t_addsub, db_st.t_maxidx - s0.t_maxidx, db_st.t_reserve - s0.t_reserve);
        return;
    }
    int ka, kb; split_grid(na, nb, &ka, &kb);
    size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb;
    if (getenv("RNS_VERBOSE")) printf("   dist_db %zu x %zu limbs: %d x %d pieces of %zu + %zu\n", na, nb, ka, kb, pa, pb);
    dbig t; db_init(&t); int first = 1;
    for (int j = 0; j < kb; j++) for (int i = 0; i < ka; i++) {
        size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
        dbig ai = db_view(A, oa, na - oa < pa ? na - oa : pa), bj = db_view(B, ob, nb - ob < pb ? nb - ob : pb);
        db_norm(&ai); db_norm(&bj);
        if (first) { rns_mul_dist_db(Cd, &ai, &bj); first = 0; continue; }   /* (0,0): shift 0, straight into C */
        if (!ai.n || !bj.n || oa + ob >= w) continue;                          /* nothing of it below w */
        rns_mul_dist_db(&t, &ai, &bj);
        db_add_shifted(Cd, &t, oa + ob, Cd);                   /* in place */
    }
    db_free(&t);
}

/* ---- Phase 8 M3: the product over a node group (mdb.h) -------------------------------------------------
 * Rank rho = gt d + r (APU d of the group's node r < gt) holds the block-cyclic rows [rho rows, (rho+1) rows)
 * of the R x C plane, rows = R / (4 gt); its row sequence is t = j rows + il <-> limb m = R j + rho rows + il
 * (columns j in order, so m increases with t).  A node's contiguous share [lo, hi) meets the sequence in
 * one segment [seq_start(lo), seq_start(hi)).
 * Phase 9 A3: the operands are views (mdbv: a window [off, off + len) of a sharded number, the pack kernel
 * reading the share at the window's offset) and the result of a piece product is delivered to the window of
 * every node's share of C that the piece covers (its limbs [shift, shift + n)): the first piece straight into
 * the zero-filled shares, the others into a temporary of that window, the spills added there, then
 * C's share += T << offset (fixed length, the carry scan over the nodes). */
__host__ __device__ static inline size_t seq_start(size_t m, size_t R, size_t rows, size_t rho)   /* the first t with m(t) >= m */
{
    size_t j = m / R, i = m - j * R, a = rho * rows;
    if (i <= a) return j * rows;
    if (i < a + rows) return j * rows + (i - a);
    return (j + 1) * rows;
}
struct seg { size_t t0, t1; };
/* pack this node's part [lo, hic) of an operand (local index m - lo in src) for the ranks (r, d), r < gt: slab r of sb */
__global__ void k_pack_mn(uint64_t *sb, struct acc src, size_t lo, size_t hic, size_t R, size_t rows, int gt, int d, size_t S)
{
    size_t total = (size_t)gt * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S, rho = (size_t)gt * d + r;
        size_t t0 = seq_start(lo, R, rows, rho), t1 = seq_start(hic, R, rows, rho);
        if (k >= t1 - t0) continue;
        size_t u = t0 + k, j = u / rows, il = u - j * rows, m = R * j + rho * rows + il;
        sb[t] = acc_get(src, m - lo);
    }
}
/* the received slabs (segment r of node r at rb + r S, [sg[r].t0, sg[r].t1) of my sequence, contiguous in node order)
 * -> my rows: x[il C + j] (transpose = 1, the transform's row layout) or x[t] (the sequence, the CRT's added operand) */
__global__ void k_gather_mn(uint64_t *x, const uint64_t *rb, const struct seg *sg, int g, size_t S, size_t rows, size_t C, ec_mod m, int canon, int transpose)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        int lo = 0, hi = g;                                     /* the last segment with t0 <= t */
        while (hi - lo > 1) { int mid = (lo + hi) / 2; if (sg[mid].t0 <= t) lo = mid; else hi = mid; }
        uint64_t v = (sg[lo].t0 <= t && t < sg[lo].t1) ? rb[(size_t)lo * S + (t - sg[lo].t0)] : 0;
        if (canon) v = ec_canon64(v, m.pu, m.mu);
        size_t j = t / rows, il = t - j * rows;
        x[transpose ? il * C + j : t] = v;
    }
}
/* my result sequence (the CRT output, q limbs) -> slabs: slab r' = the segment of node r' */
__global__ void k_pack_out_mn(uint64_t *sb, const uint64_t *xb, const struct seg *sg, int g, size_t S)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t r = t / S, k = t - r * S; if (k < sg[r].t1 - sg[r].t0) sb[t] = xb[sg[r].t0 + k]; }
}
/* the received slabs of the ranks (r, d), r < gt (segments of my window [lo, ..)) -> the window's limbs */
__global__ void k_scatter_mn(struct acc dst, size_t lo, const uint64_t *rb, const struct seg *sg, int gt, size_t S, size_t R, size_t rows, int d)
{
    size_t total = (size_t)gt * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S; if (k >= sg[r].t1 - sg[r].t0) continue;
        size_t u = sg[r].t0 + k, j = u / rows, il = u - j * rows, rho = (size_t)gt * d + r, m = R * j + rho * rows + il;
        *acc_ptr(dst, m - lo) = rb[t];
    }
}
/* the slab (limbs) that holds any node's segment of an operand sharded with shares of at most `maxshare` limbs */
static size_t slab_limbs(size_t maxshare, size_t R, size_t rows, size_t q)
{
    size_t S = maxshare ? ((maxshare - 1) / R + 2) * rows : rows;
    if (S > q) S = q;
    return (S + 15) / 16 * 16;
}
static size_t max_share(const mdb *x) { return x->g ? (x->N + x->g - 1) / x->g : 0; }
static int g_node_of(mn_group *G) { return G->g0 + G->me; }
static size_t grp_max(mn_group *G, size_t v) { return G->g > 1 ? comm_allreduce_max(G->all[0], v) : v; }

static comm *lay_get(mn_group *G, int d) { if (!G->lay[d]) G->lay[d] = comm_layered_create(RS[d].cm, G->tr[d], d); return G->lay[d]; }
struct mn_ctx { mn_group *G; int node, gt, g; size_t R, rows, C, n; };
/* node r's part of a view, in view coordinates [lo, hi) (its share of m cut to the window and to the view's length) */
static void view_share(const mdbv *v, int r, size_t *lo, size_t *hi)
{
    size_t slo, shi; mdb_share(v->m, r, &slo, &shi);
    size_t a = slo > v->off ? slo - v->off : 0, b = shi > v->off ? shi - v->off : 0;
    if (a > v->len) a = v->len; if (b > v->len) b = v->len; if (a > b) a = b;
    *lo = a; *hi = b;
}
static size_t view_max_share(const mdbv *v) { size_t s = max_share(v->m); return s < v->len ? s : v->len; }
/* one operand: pack my part, exchange over mesh d, and the segment table of the receiver (rank (me, d)) */
static void redistribute(const struct mn_ctx *X, const mdbv *op, size_t S, uint64_t *sb, uint64_t *rb, struct seg *hseg, struct seg *dseg, int d, hipStream_t s)
{
    size_t lo, hi; view_share(op, X->node, &lo, &hi);
    if (hi > lo) {
        size_t slo, shi; mdb_share(op->m, X->node, &slo, &shi);
        struct acc a = acc_db(&op->m->sh, op->off + lo - slo, hi - lo);   /* the window's first limb within my share */
        k_pack_mn<<<nblk((size_t)X->gt * S), 256, 0, s>>>(sb, a, lo, hi, X->R, X->rows, X->gt, d, S);
    }
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(X->G->all[d], sb, rb, S * 8, s); comm_wait(X->G->all[d]);
    size_t rho = (size_t)X->gt * d + X->G->me;
    for (int r = 0; r < X->g; r++) {
        view_share(op, X->G->g0 + r, &lo, &hi);
        hseg[r].t0 = seq_start(lo, X->R, X->rows, rho); hseg[r].t1 = seq_start(hi, X->R, X->rows, rho);
    }
    HIP_CHECK(hipMemcpyAsync(dseg, hseg, X->g * sizeof *hseg, hipMemcpyHostToDevice, s)); HIP_CHECK(hipStreamSynchronize(s));
}
/* the (carry, propagate) flags of the g nodes' shares, all-gathered over the group's mesh 0 (one byte per node, host
 * point-to-point: write to all, then read from all), and the scan: carry into node r = c_{r-1} | (p_{r-1} & carry into r-1);
 * returns the carry into this node, and aborts on a carry out of the last node (the fixed-length sum overflowed) */
static int node_carry_in(mn_group *G, int c, int p)
{
    comm *cm = G->all[0]; int g = G->g, me = G->me, cin = 0;
    uint8_t v = (uint8_t)(c | (p << 1)), *all = (uint8_t *)malloc(g); all[me] = v;
    for (int r = 0; r < g; r++) if (r != me) comm_send(cm, r, &v, 1);
    for (int r = 0; r < g; r++) if (r != me) comm_recv(cm, r, &all[r], 1);
    for (int r = 0; r < me; r++) cin = (all[r] & 1) | (((all[r] >> 1) & 1) & cin);
    int top = cin; for (int r = me; r < g; r++) top = (all[r] & 1) | (((all[r] >> 1) & 1) & top);
    free(all);
    if (top) { fprintf(stderr, "rns_mul_dist_mn: carry out of the top share (node %d)\n", G->g0 + me); exit(1); }
    return cin;
}
/* the carries across the nodes after a fixed-length add on the shares (n limbs; co, pr its flags -- a node that had nothing
 * to add reports co = 0 and pr = 0 unless n = 0, since it did not look at its limbs): the scan, then + 1 on the shares that
 * receive a carry.  A + 1 that carries out of a share whose add reported propagate was already passed on by the scan; one
 * that carries out of a share that reported no propagate (it had nothing to add) is a new carry: another scan round
 * (at most g rounds; none in practice) */
static void share_carry_fix(mn_group *G, dbig *sh, size_t n, int co, int pr)
{
    int cin = node_carry_in(G, co, pr);
    for (;;) {
        int co2 = 0;
        if (cin && n) db_share_add_one(sh, n, &co2);
        int newc = (cin && !pr) ? co2 : 0;
        if (!grp_max(G, (size_t)newc)) break;
        cin = node_carry_in(G, newc, 0); pr = 0;
    }
}
/* the window of node r's share of C that a piece product delivered at `shift` with basis Np covers: [lo, hi) in the piece's
 * coordinates */
static void piece_window(const mdb *C, int r, size_t shift, size_t Np, size_t *lo, size_t *hi)
{
    size_t clo, chi; mdb_share(C, r, &clo, &chi);
    size_t a = clo > shift ? clo - shift : 0, b = chi > shift ? chi - shift : 0;
    if (a > Np) a = Np; if (b > Np) b = Np; if (a > b) a = b;
    *lo = a; *hi = b;
}
struct mn_times { double redistribute, ntt, crt, out, carry, total; };
/* the core of one plane: C's shares += (A B (+ X)) << shift.  direct: shift 0, the piece is the whole product (X allowed),
 * the rows go straight into C's zero-filled shares (M3's path, bit for bit).  Otherwise the rows go into a temporary T
 * of this node's window [tlo, thi) of the piece (in piece coordinates), the spills are added there, and C's share
 * += T << (tlo + shift - clo).  Np = the piece's basis (na + nb, + 1 with X). */
static void mn_core(mdb *Cn, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t shift, int direct, struct mn_times *tm)
{
    int g = G->g, gt = G->gt, me = G->me, nr = 4 * gt, lgt = 0; while ((1 << lgt) < gt) lgt++;
    int node = G->g0 + me, tnode = me < gt, verbose = getenv("RNS_VERBOSE") != 0;
    size_t na = A->len, nb = B->len, nc = na + nb, Np = nc + (X ? 1 : 0);
    if (!na || !nb) { fprintf(stderr, "rns_mul_dist_mn: a zero operand\n"); exit(1); }
    if (X && X->n > nc) { fprintf(stderr, "rns_mul_dist_mn: the added operand (%zu limbs) exceeds the product (%zu)\n", X->n, nc); exit(1); }
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    int logmin = 2 * (7 + lgt); if (logmin < 20) logmin = 20;   /* rows = R / nr >= 32 (the tiled pack), R, C >= 2^10 */
    if (logn < logmin) logn = logmin;
    if (logn > DIST_LOGN_MAX + lgt) { fprintf(stderr, "rns_mul_dist_mn: %zu limbs > 2^%d points over %d transform nodes\n", nc, DIST_LOGN_MAX + lgt, gt); exit(1); }
    int logR = logn / 2, logC = logn - logR;
    size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rows = R / nr, q = n / nr;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    struct mn_ctx X0 = { G, node, gt, g, R, rows, C, n };
    mdbv Xv = { X, 0, X ? X->n : 0 };
    /* slab sizes per operand and for the result (the largest window of any node) */
    size_t SA = slab_limbs(view_max_share(A), R, rows, q), SB = slab_limbs(view_max_share(B), R, rows, q), SX = X ? slab_limbs(max_share(X), R, rows, q) : 0;
    size_t maxwin = 0; for (int r = 0; r < g; r++) { size_t lo, hi; piece_window(Cn, G->g0 + r, shift, Np, &lo, &hi); if (hi - lo > maxwin) maxwin = hi - lo; }
    size_t SC = slab_limbs(maxwin, R, rows, q), Smax = SA; if (SB > Smax) Smax = SB; if (SX > Smax) Smax = SX; if (SC > Smax) Smax = SC;
    size_t clo, chi; mdb_share(Cn, node, &clo, &chi); size_t cn = chi - clo;
    size_t tlo, thi; piece_window(Cn, node, shift, Np, &tlo, &thi); size_t tn = thi - tlo;
    if (direct && shift) { fprintf(stderr, "rns_mul_dist_mn: a direct piece at a shift\n"); exit(1); }   /* direct: the window is the share's first tn limbs */
    dbig T; db_init(&T); dbig *dst = direct ? &Cn->sh : &T;
    if (!direct && tn) db_zero_fill(&T, tn);
    const uint64_t *sp[NR]; uint64_t *spill_rb[NR];
    double tr[NR] = {0}, tf[NR] = {0}, tc[NR] = {0}, to[NR] = {0};
#pragma omp parallel num_threads(NR)
    {
        int d = omp_get_thread_num(); struct rank_state *v = &RS[d]; hipStream_t s = v->s;
        HIP_CHECK(hipSetDevice(d));
        uint64_t *sb = db_pool_alloc(d, g * Smax * 8), *rbA = db_pool_alloc(d, g * Smax * 8), *rbB = db_pool_alloc(d, g * SB * 8);
        uint64_t *cx = (X && tnode) ? db_pool_alloc(d, q * 8) : 0, *tmp = tnode ? db_pool_alloc(d, q * 8) : 0;
        uint64_t *spill_sb = db_pool_alloc(d, (size_t)g * C * 4 * 8); spill_rb[d] = db_pool_alloc(d, (size_t)g * C * 4 * 8);
        struct seg *hseg = (struct seg *)malloc(4 * g * sizeof *hseg), *hsA = hseg, *hsB = hseg + g, *hsX = hseg + 2 * g, *hsO = hseg + 3 * g;
        struct seg *dseg = (struct seg *)db_pool_alloc(d, 4 * g * sizeof *hseg), *dsA = dseg, *dsB = dseg + g, *dsX = dseg + 2 * g, *dsO = dseg + 3 * g;
        /* planes: xa[4] in pool 0, xb (q + 16: the CRT's carry limb) | sbuf | rbuf in pool 1 */
        uint64_t *pl = (uint64_t *)rns_dpool(d, 0, (size_t)EC_NP * q * 8), *p1 = (uint64_t *)rns_dpool(d, 1, (size_t)(3 * q + 16) * 8);
        uint64_t *xa[EC_NP], *xb = p1, *sl = p1 + q + 16, *xt = sl;
        for (int p = 0; p < EC_NP; p++) xa[p] = pl + (size_t)p * q;
        comm *cl = 0;
        if (tnode) {
            cl = lay_get(G, d); comm_layered_scratch(cl, tmp, q * 8);
            for (int p = 0; p < EC_NP; p++) {
                if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC || v->plan[p].cm != cl) {
                    if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                    dist_plan_create_shared(&v->plan[p].pl, cl, v->ctx[p], p, logR, logC, sl, sl + q);
                    v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1; v->plan[p].cm = cl;
                }
            }
        }
        double s0 = mem_now();
        /* A: every node packs and exchanges; the transform ranks gather their rows for the four primes */
        redistribute(&X0, A, SA, sb, rbA, hsA, dsA, d, s);
        if (tnode) for (int p = 0; p < EC_NP; p++) k_gather_mn<<<nblk(q), 256, 0, s>>>(xa[p], rbA, dsA, g, SA, rows, C, ec_mod_get(p), 1, 1);
        redistribute(&X0, B, SB, sb, rbB, hsB, dsB, d, s);
        if (X) { redistribute(&X0, &Xv, SX, sb, rbA, hsX, dsX, d, s); if (tnode) k_gather_mn<<<nblk(q), 256, 0, s>>>(cx, rbA, dsX, g, SX, rows, C, ec_mod_get(0), 0, 0); }
        HIP_CHECK(hipStreamSynchronize(s));
        double s1 = mem_now(); tr[d] = s1 - s0;
        if (tnode) {
            for (int p = 0; p < EC_NP; p++) {
                k_gather_mn<<<nblk(q), 256, 0, s>>>(xb, rbB, dsB, g, SB, rows, C, ec_mod_get(p), 1, 1);
                dist_fwd(&v->plan[p].pl, xa[p], s);
                dist_fwd(&v->plan[p].pl, xb, s);
                dist_pw(&v->plan[p].pl, xa[p], xb, s);
                dist_inv(&v->plan[p].pl, xa[p], s);
                HIP_CHECK(hipStreamSynchronize(s));
            }
            double s2 = mem_now(); tf[d] = s2 - s1;
            for (int p = 0; p < EC_NP; p++) {
                dim3 grid((unsigned)((C + 31) / 32), (unsigned)((rows + 31) / 32)), blk(32, 8);
                k_transpose<<<grid, blk, 0, s>>>(xa[p], xt, rows, C);
                HIP_CHECK(hipMemcpyAsync(xa[p], xt, q * 8, hipMemcpyDeviceToDevice, s));
            }
            struct bdesc hd = { 0, 0, cx, xb, (uint32_t)q, 0, (uint32_t)(cx ? q : 0) };
            struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, d, sizeof hd);
            HIP_CHECK(hipMemcpyAsync(dd, &hd, sizeof hd, hipMemcpyHostToDevice, s));
            if (v->spill_cap < C) { if (v->spill) HIP_CHECK(hipFree(v->spill)); v->spill_cap = C + 16; HIP_CHECK(hipMalloc(&v->spill, v->spill_cap * 4 * 8)); }
            k_crt_batch<<<(unsigned)C, CRT_THREADS, 0, s>>>(xa[0], xa[1], xa[2], xa[3], dd, 0, (int)C, q, rns_gconst(), v->spill, bi_decimal);
            HIP_CHECK(hipStreamSynchronize(s));
            tc[d] = mem_now() - s2;
            /* the result's rows -> the nodes' windows: segments of every node's window in my sequence */
            size_t rho = (size_t)gt * d + me;
            for (int r = 0; r < g; r++) { size_t lo, hi; piece_window(Cn, G->g0 + r, shift, Np, &lo, &hi); if (lo > n) lo = n; if (hi > n) hi = n;
                                          hsO[r].t0 = seq_start(lo, R, rows, rho); hsO[r].t1 = seq_start(hi, R, rows, rho); }
            HIP_CHECK(hipMemcpyAsync(dsO, hsO, g * sizeof *hsO, hipMemcpyHostToDevice, s));
            k_pack_out_mn<<<nblk((size_t)g * SC), 256, 0, s>>>(sb, xb, dsO, g, SC);
            for (int r = 0; r < g; r++) HIP_CHECK(hipMemcpyAsync(spill_sb + (size_t)r * C * 4, v->spill, C * 4 * 8, hipMemcpyDeviceToDevice, s));
            HIP_CHECK(hipStreamSynchronize(s));
        }
        double s3 = mem_now();
        comm_alltoall(G->all[d], sb, rbA, SC * 8, s); comm_wait(G->all[d]);
        { size_t lo = tlo < n ? tlo : n, hi = thi < n ? thi : n;
          for (int r = 0; r < gt; r++) { size_t rho = (size_t)gt * d + r; hsA[r].t0 = seq_start(lo, R, rows, rho); hsA[r].t1 = seq_start(hi, R, rows, rho); }
          HIP_CHECK(hipMemcpyAsync(dsA, hsA, gt * sizeof *hsA, hipMemcpyHostToDevice, s)); }
        if (tn) { struct acc c = acc_db(dst, 0, tn); k_scatter_mn<<<nblk((size_t)gt * SC), 256, 0, s>>>(c, tlo, rbA, dsA, gt, SC, R, rows, d); }
        comm_alltoall(G->all[d], spill_sb, spill_rb[d], C * 4 * 8, s); comm_wait(G->all[d]);
        HIP_CHECK(hipStreamSynchronize(s));
        sp[d] = spill_rb[d];
        to[d] = mem_now() - s3;
        db_pool_free(d, sb); db_pool_free(d, rbA); db_pool_free(d, rbB); if (cx) db_pool_free(d, cx); db_pool_free(d, spill_sb);
        db_pool_free(d, (uint64_t *)dseg); free(hseg);
        if (tmp) { comm_layered_scratch(cl, 0, 0); db_pool_free(d, tmp); }
    }
    HIP_CHECK(hipSetDevice(0));
    /* the spills into the windows, then the carries across the nodes */
    double t4 = mem_now(); int co = 0, pr = 1;                /* an empty window propagates (the windows tile the piece) */
    if (tn) db_share_add_spills(dst, tn, tlo, sp, R, rows, C, gt, &co, &pr);
    share_carry_fix(G, dst, tn, co, pr);
    for (int d = 0; d < NR; d++) db_pool_free(d, spill_rb[d]);
    if (!direct) {                                            /* C's share += T << (its window's offset); the carries across the nodes */
        co = 0; pr = cn == 0;
        if (tn) db_share_add_shifted(&Cn->sh, cn, &T, tlo + shift - clo, &co, &pr);
        share_carry_fix(G, &Cn->sh, cn, co, pr);
        db_free(&T);
    }
    Cn->sh.n = cn;                                            /* the fixed-length adds set sh.n to their length */
    double mr = 0, mf = 0, mc = 0, mo = 0; for (int d = 0; d < NR; d++) { if (tr[d] > mr) mr = tr[d]; if (tf[d] > mf) mf = tf[d]; if (tc[d] > mc) mc = tc[d]; if (to[d] > mo) mo = to[d]; }
    double tcar = mem_now() - t4, tt = mem_now() - t0;
    rns_dist_st.n++; rns_dist_st.t_total += tt; rns_dist_st.t_load += mr; rns_dist_st.t_ntt += mf; rns_dist_st.t_crt += mc; rns_dist_st.t_merge += mo + tcar;
    tm->redistribute += mr; tm->ntt += mf; tm->crt += mc; tm->out += mo; tm->carry += tcar; tm->total += tt;
    if (verbose) printf("dist_mn node %d: 2^%d = 2^%d x 2^%d over %d x 4 ranks (%zu + %zu%s limbs at %zu, window [%zu, %zu) of share [%zu, %zu)%s): redistribute %.3f ntt %.3f crt %.3f out %.3f spills+carry %.3f total %.3f s\n",
                        node, logn, logR, logC, gt, na, nb, X ? " + x" : "", shift, tlo, thi, clo, chi, direct ? "" : ", accumulated", mr, mf, mc, mo, tcar, tt);
}
mdbv mdb_view(const mdb *m, size_t off, size_t len, mn_group *G)
{
    mdbv v; v.m = m; v.off = off; v.len = 0;
    size_t n = m->n; if (off < n && len) { v.len = n - off < len ? n - off : len; }
    size_t top = 0;
    if (v.len) {
        size_t slo, shi; mdb_share(m, g_node_of(G), &slo, &shi);
        size_t a = slo > off ? slo : off, b = shi < off + v.len ? shi : off + v.len;   /* my share cut to the window */
        if (b > a) { dbig t = db_view(&m->sh, a - slo, b - a); db_norm(&t); if (t.n) top = a - off + t.n; }
    }
    v.len = grp_max(G, top);
    return v;
}
void mdb_norm(mdb *C, mn_group *G, size_t below)
{
    size_t lim = below < C->N ? below : C->N, top = 0;
    size_t clo, chi; mdb_share(C, g_node_of(G), &clo, &chi); if (chi > lim) chi = lim; if (clo > chi) clo = chi;
    if (chi > clo) { dbig t = db_view(&C->sh, 0, chi - clo); db_norm(&t); if (t.n) top = clo + t.n; }
    C->n = grp_max(G, top);
}
__global__ void k_zero_acc(struct acc a, size_t n) { size_t t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; t < n; t += st) *acc_ptr(a, t) = 0; }
/* the limbs of C's share at or above `from` (global) set to zero: a truncated result stays a valid operand of the adds */
static void share_zero_from(mdb *C, mn_group *G, size_t from)
{
    size_t clo, chi; mdb_share(C, g_node_of(G), &clo, &chi); if (from < clo) from = clo; if (from >= chi) return;
    struct acc a = acc_db(&C->sh, from - clo, chi - from);
    HIP_CHECK(hipSetDevice(0)); k_zero_acc<<<nblk(chi - from), 256>>>(a, chi - from); HIP_CHECK(hipDeviceSynchronize());
}
/* the plane cap of the mn tier: one plane per node pool (4 q limbs, q = n / 4 gt, in pool 0 of 2^pool_log limbs), at most 2^31 per node */
static int mn_logn_cap(int gt) { int lgt = 0; while ((1 << lgt) < gt) lgt++; int c = dist_logn_max(); if (rns_pool_log() < c) c = rns_pool_log(); return c + lgt; }
/* C = A B (+ X) over the group, as one plane or as the grid of piece products (the pieces at or above w skipped) */
static void mn_grid(mdb *Cm, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w)
{
    int g = G->g, node = g_node_of(G), verbose = getenv("RNS_VERBOSE") != 0, lgt = 0; while ((1 << lgt) < G->gt) lgt++;
    size_t na = A->len, nb = B->len, nc = na + nb, N = nc + (X ? 1 : 0), cap = (size_t)1 << mn_logn_cap(G->gt);
    if (!na || !nb) { fprintf(stderr, "rns_mul_dist_mn: a zero operand\n"); exit(1); }
    double t0 = mem_now();
    mdb Cn; memset(&Cn, 0, sizeof Cn); Cn.N = N; Cn.g0 = G->g0; Cn.g = g; db_init(&Cn.sh);
    size_t clo, chi; mdb_share(&Cn, node, &clo, &chi); size_t cn = chi - clo;
    db_zero_fill(&Cn.sh, cn);
    struct mn_times tm; memset(&tm, 0, sizeof tm);
    if (nc <= cap) mn_core(&Cn, A, B, X, G, 0, 1, &tm);
    else {
        int ka, kb; int logmin = 2 * (7 + lgt); if (logmin < 20) logmin = 20;
        split_grid_cap(na, nb, cap, (size_t)1 << logmin, &ka, &kb);
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb;
        if (verbose || getenv("ECALC_VERBOSE")) printf("dist_mn node %d: %zu x %zu limbs over %d x 4 ranks (cap 2^%d): %d x %d pieces of %zu + %zu%s\n", node, na, nb, G->gt, mn_logn_cap(G->gt), ka, kb, pa, pb, w != (size_t)-1 ? " (low product)" : "");
        mdbv *av = (mdbv *)malloc((ka + kb) * sizeof *av), *bv = av + ka;
        for (int i = 0; i < ka; i++) av[i] = mdb_view(A->m, A->off + (size_t)i * pa, na - (size_t)i * pa < pa ? na - (size_t)i * pa : pa, G);
        for (int j = 0; j < kb; j++) bv[j] = mdb_view(B->m, B->off + (size_t)j * pb, nb - (size_t)j * pb < pb ? nb - (size_t)j * pb : pb, G);
        for (int j = 0; j < kb; j++) for (int i = 0; i < ka; i++) {
            size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
            if (!av[i].len || !bv[j].len || oa + ob >= w) continue;                  /* nothing, or nothing of it below w */
            mn_core(&Cn, &av[i], &bv[j], 0, G, oa + ob, i == 0 && j == 0, &tm);   /* (0,0): shift 0, straight into the zero-filled C */
        }
        free(av);
        if (X) mdb_add_shifted(&Cn, X, 0, G);
    }
    mdb_norm(&Cn, G, w);
    if (w < N) share_zero_from(&Cn, G, w);                    /* the limbs above the window are not part of the result */
    if (Cm->sh.cap) db_free(&Cm->sh);
    *Cm = Cn;
    if (verbose) printf("dist_mn node %d: %zu + %zu%s limbs -> %zu (share %zu): redistribute %.3f ntt %.3f crt %.3f out %.3f spills+carry %.3f, total %.3f s\n",
                        node, na, nb, X ? " + x" : "", Cn.n, cn, tm.redistribute, tm.ntt, tm.crt, tm.out, tm.carry, mem_now() - t0);
}
void rns_mul_dist_mn(mdb *Cm, const mdb *A, const mdb *B, const mdb *X, mn_group *G)
{
    mdbv a = { A, 0, A->n }, b = { B, 0, B->n };
    mn_grid(Cm, &a, &b, X, G, (size_t)-1);
}
void rns_mul_dist_mn_v(mdb *Cm, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w) { mn_grid(Cm, A, B, X, G, w); }
static void mdb_empty(mdb *Cm, mn_group *G) { if (Cm->sh.cap) db_free(&Cm->sh); memset(Cm, 0, sizeof *Cm); db_init(&Cm->sh); Cm->g0 = G->g0; Cm->g = G->g; }
void rns_mul_low_mn(mdb *Cm, const mdb *A, const mdb *B, mn_group *G, size_t w)
{
    if (!w) { mdb_empty(Cm, G); return; }
    mdbv a = mdb_view(A, 0, w, G), b = mdb_view(B, 0, w, G);
    if (!a.len || !b.len) { mdb_empty(Cm, G); return; }
    mn_grid(Cm, &a, &b, 0, G, w);
}

/* ---- the shifted distributed add: C += X << k on C's shares (Phase 9 A3) -------------------------------------
 * Node r's share [clo, chi) of C needs X's limbs [clo - k, chi - k) (cut to [0, nX)): the window [tlo, thi) of its share.
 * APU thread d serves the quarter d of every node's window, in rounds of CH limbs (slabs of CH per pair over mesh d,
 * padded; a pair's slab is the cut of the sender's share of X with the receiver's quarter chunk, contiguous), unpacked
 * into a temporary T of this node's window; then C's share += T << (tlo - clo) with the carry scan over the nodes. */
struct rng { size_t a, b, src; };                              /* a slab: limbs [a, b) of X, from local index src of the sender's share */
__global__ void k_pack_rng(uint64_t *sb, struct acc src, const struct rng *tb, int g, size_t S)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t r = t / S, i = t - r * S; if (i < tb[r].b - tb[r].a) sb[t] = acc_get(src, tb[r].src + i); }
}
__global__ void k_unpack_rng(struct acc dst, const uint64_t *rb, const struct rng *tb, int g, size_t S)   /* tb[s].src: the T index of limb a */
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t s = t / S, i = t - s * S; if (i < tb[s].b - tb[s].a) *acc_ptr(dst, tb[s].src + i) = rb[t]; }
}
static void add_window(const mdb *C, int r, size_t k, size_t nX, size_t *lo, size_t *hi)   /* node r's window of C, in C's coordinates */
{
    size_t clo, chi; mdb_share(C, r, &clo, &chi);
    size_t a = clo > k ? clo : k, b = chi < k + nX ? chi : k + nX; if (b < a) b = a;
    *lo = a; *hi = b;
}
static void x_part(const mdb *X, int r, size_t *lo, size_t *hi) { mdb_share(X, r, lo, hi); if (*lo > X->n) *lo = X->n; if (*hi > X->n) *hi = X->n; }
#define MDB_ADD_CHUNK ((size_t)1 << 26)
void mdb_add_shifted(mdb *C, const mdb *X, size_t k, mn_group *G)
{
    int g = G->g, node = g_node_of(G); size_t nX = X->n;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; }
    if (nX && k + nX > C->N) { fprintf(stderr, "mdb_add_shifted: %zu limbs at %zu exceed the basis %zu\n", nX, k, C->N); exit(1); }
    size_t clo, chi; mdb_share(C, node, &clo, &chi); size_t cn = chi - clo;
    size_t tlo, thi; add_window(C, node, k, nX, &tlo, &thi); size_t tn = thi - tlo;
    size_t maxq = 0; for (int r = 0; r < g; r++) { size_t lo, hi; add_window(C, r, k, nX, &lo, &hi); size_t qq = (hi - lo + 3) / 4; if (qq > maxq) maxq = qq; }
    size_t S = maxq < MDB_ADD_CHUNK ? maxq : MDB_ADD_CHUNK; S = (S + 15) / 16 * 16; int rounds = maxq ? (int)((maxq + S - 1) / S) : 0;
    dbig T; db_init(&T); if (tn) db_zero_fill(&T, tn);
    size_t xlo, xhi; x_part(X, node, &xlo, &xhi);
    if (rounds) {
#pragma omp parallel num_threads(NR)
    {
        int d = omp_get_thread_num(); hipStream_t s = RS[d].s;
        HIP_CHECK(hipSetDevice(d));
        uint64_t *sb = db_pool_alloc(d, g * S * 8), *rb = db_pool_alloc(d, g * S * 8);
        struct rng *ht = (struct rng *)malloc(g * sizeof *ht), *dt = (struct rng *)db_pool_alloc(d, g * sizeof *ht);
        struct acc src = acc_db(&X->sh, 0, xhi > xlo ? xhi - xlo : 0), dst = acc_db(&T, 0, tn);
        for (int c = 0; c < rounds; c++) {
            /* pack: for every receiver r, its quarter d, chunk c (in C's coordinates) -> X's [a, b) cut to my part */
            for (int r = 0; r < g; r++) {
                size_t lo, hi; add_window(C, G->g0 + r, k, nX, &lo, &hi); size_t wn = hi - lo;
                size_t qlo = lo + wn * d / 4 + (size_t)c * S, qhi = lo + wn * (d + 1) / 4; if (qlo + S < qhi) qhi = qlo + S; if (qhi < qlo) qhi = qlo;
                size_t a = qlo - k, b = qhi - k; if (a < xlo) a = xlo; if (b > xhi) b = xhi; if (b < a) b = a;   /* qlo >= k: the window starts at k or above */
                ht[r].a = a; ht[r].b = b; ht[r].src = a - xlo;
            }
            HIP_CHECK(hipMemcpyAsync(dt, ht, g * sizeof *ht, hipMemcpyHostToDevice, s));
            if (xhi > xlo) k_pack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(sb, src, dt, g, S);
            HIP_CHECK(hipStreamSynchronize(s));
            comm_alltoall(G->all[d], sb, rb, S * 8, s); comm_wait(G->all[d]);
            /* unpack: sender s's slab holds X's [a, b) = its part cut to my quarter chunk -> T at a + k - tlo */
            size_t qlo = tlo + tn * d / 4 + (size_t)c * S, qhi = tlo + tn * (d + 1) / 4; if (qlo + S < qhi) qhi = qlo + S; if (qhi < qlo) qhi = qlo;
            for (int r = 0; r < g; r++) {
                size_t plo, phi; x_part(X, G->g0 + r, &plo, &phi);
                size_t a = qlo - k, b = qhi - k; if (a < plo) a = plo; if (b > phi) b = phi; if (b < a) b = a;
                ht[r].a = a; ht[r].b = b; ht[r].src = a + k - tlo;
            }
            HIP_CHECK(hipMemcpyAsync(dt, ht, g * sizeof *ht, hipMemcpyHostToDevice, s));
            if (tn) k_unpack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(dst, rb, dt, g, S);
            HIP_CHECK(hipStreamSynchronize(s));
        }
        db_pool_free(d, sb); db_pool_free(d, rb); db_pool_free(d, (uint64_t *)dt); free(ht);
    }
    HIP_CHECK(hipSetDevice(0));
    }
    int co = 0, pr = cn == 0;
    if (tn) db_share_add_shifted(&C->sh, cn, &T, tlo - clo, &co, &pr);
    share_carry_fix(G, &C->sh, cn, co, pr);
    db_free(&T);
    if (getenv("RNS_VERBOSE")) printf("mdb_add_shifted node %d: %zu limbs at %zu into a basis of %zu (my window [%zu, %zu), %d rounds of %zu): %.3f s\n", node, nX, k, C->N, tlo, thi, rounds, S, mem_now() - t0);
}
