/* rns_dist.c - the distributed product tier (WP5): C = A * B through the 4-APU
 * four-step transform (ntt_dist over comm_xgmi), contiguous ownership.
 *
 * n = 2^(logR+logC) >= na + nb points, rows = R/4, cols = C/4 per rank.  Rank
 * r (device r) holds, per prime, its block-cyclic rows of A and B (point
 * m = i + R j, rows i in [r R/4, (r+1) R/4)), gathered from the contiguous
 * operands in column runs (limbs R j + r R/4 .. + R/4: coalesced reads);
 * forward transforms (one all-to-all each), pointwise, inverse (one all-to-all)
 * -> the rank holds its block-cyclic rows of the product for all four primes:
 * for every column j the run of rows/4 consecutive limbs R j + r R/4 ...  A
 * local transpose makes each run contiguous, the CRT treats every run as a
 * stripe (into a local buffer), the runs are scattered to their limb
 * positions in the result and the run spills are merged on the host in limb
 * order.  Operands and result: registered host memory (the caller's, or a
 * staging copy) or device pools.  3 all-to-alls per product. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "rns_mul.h"
#include "rns_int.h"
#include "ntt_dist.h"
#include "mem.h"
#include "bigint.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4

struct rank_state {
    comm *cm; ntt_ctx *ctx[EC_NP]; hipStream_t s;
    dpool planes, slabs; dpool desc;
    uint64_t *spill; size_t spill_cap;
    struct { dist_plan pl; int logR, logC, built; } plan[EC_NP];
};
static struct rank_state RS[NR];
static int g_init;
static uint64_t *g_stage; static size_t g_stage_cap;      /* registered host staging for unregistered operands/result */
struct rns_dist_stats rns_dist_st;

static void rank_init(int r)
{
    struct rank_state *v = &RS[r];
    HIP_CHECK(hipSetDevice(r));
    v->cm = comm_xgmi_create(r);
    for (int p = 0; p < EC_NP; p++) v->ctx[p] = ntt_ctx_create(p);
    HIP_CHECK(hipStreamCreate(&v->s));
}
/* gather rank r's rows of a contiguous operand: x[il C + j] = canon(src[R j + r rows + il]) or 0 */
__global__ void k_gather(uint64_t *x, const uint64_t *src, size_t nlimbs, size_t R, size_t rows, size_t row0, size_t C, ec_mod m)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t j = t / rows, il = t % rows, mm = R * j + row0 + il;      /* consecutive threads: consecutive limbs */
        x[il * C + j] = mm < nlimbs ? ec_canon64(src[mm], m.pu, m.mu) : 0;
    }
}
static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 16 ? 228 * 16 : b); }
/* rows x C row-major -> C x rows (run j = column j, contiguous) */
__global__ void k_transpose(const uint64_t *x, uint64_t *y, size_t rows, size_t C)
{
    __shared__ uint64_t tile[32][33];
    size_t bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;                        /* 32 x 8 threads */
    for (int k = 0; k < 32; k += 8) { size_t i = bi + ty + k, j = bj + tx; if (i < rows && j < C) tile[ty + k][tx] = x[i * C + j]; }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) { size_t j = bj + ty + k, i = bi + tx; if (i < rows && j < C) y[j * rows + i] = tile[tx][ty + k]; }
}
/* run j of the local CRT output (rows limbs at j rows) -> result limbs R j + row0 .. */
__global__ void k_scatter_runs(const uint64_t *loc, uint64_t *c, size_t nc, size_t R, size_t rows, size_t row0, size_t C)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t j = t / rows, il = t % rows, mm = R * j + row0 + il; if (mm < nc) c[mm] = loc[t]; }
}

void rns_mul_dist(bigint *Cout, const bigint *A, const bigint *B)
{
    size_t na = A->n, nb = B->n, nc = na + nb;
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    if (logn < 20) logn = 20;                                  /* R, C >= 2^10 */
    int logR = logn / 2, logC = logn - logR;
    size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rows = R / NR, q = n / NR;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    /* operands and result in memory the kernels can reach */
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
    double t1 = mem_now();
    double tl[NR], tf[NR], tc[NR];
#pragma omp parallel num_threads(NR)
    {
        int r = omp_get_thread_num(); struct rank_state *v = &RS[r];
        HIP_CHECK(hipSetDevice(r));
        uint64_t *pl = (uint64_t *)dpool_get(&v->planes, r, (size_t)2 * EC_NP * q * 8);        /* xa[4] | xb[4], q points each */
        uint64_t *sl = (uint64_t *)dpool_get(&v->slabs, r, (size_t)2 * q * 8);
        uint64_t *xa[EC_NP], *xb[EC_NP];
        for (int p = 0; p < EC_NP; p++) { xa[p] = pl + (size_t)p * q; xb[p] = pl + (size_t)(EC_NP + p) * q; }
        for (int p = 0; p < EC_NP; p++) {
            if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC) {
                if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                dist_plan_create_shared(&v->plan[p].pl, v->cm, v->ctx[p], p, logR, logC, sl, sl + q);
                v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1;
            }
        }
        double s0 = mem_now();
        for (int p = 0; p < EC_NP; p++) {
            ec_mod m = ec_mod_get(p);
            k_gather<<<nblk(q), 256, 0, v->s>>>(xa[p], a, na, R, rows, (size_t)r * rows, C, m);
            k_gather<<<nblk(q), 256, 0, v->s>>>(xb[p], b, nb, R, rows, (size_t)r * rows, C, m);
        }
        HIP_CHECK(hipStreamSynchronize(v->s));
        double s1 = mem_now();
        for (int p = 0; p < EC_NP; p++) {
            dist_fwd(&v->plan[p].pl, xa[p], v->s);
            dist_fwd(&v->plan[p].pl, xb[p], v->s);
            dist_pw(&v->plan[p].pl, xa[p], xb[p], v->s);
            dist_inv(&v->plan[p].pl, xa[p], v->s);
        }
        HIP_CHECK(hipStreamSynchronize(v->s));
        double s2 = mem_now();
        /* CRT over this rank's runs: transpose each plane to C x rows (run j contiguous), one stripe per run */
        for (int p = 0; p < EC_NP; p++) {
            dim3 grid((unsigned)((C + 31) / 32), (unsigned)((rows + 31) / 32)), blk(32, 8);
            k_transpose<<<grid, blk, 0, v->s>>>(xa[p], xb[p], rows, C);
        }
        struct bdesc hd = { 0, 0, 0, xa[0], (uint32_t)q, 0, 0 };                /* local output: q limbs, run j at j rows */
        struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, r, sizeof hd);
        HIP_CHECK(hipMemcpyAsync(dd, &hd, sizeof hd, hipMemcpyHostToDevice, v->s));
        if (v->spill_cap < C) { if (v->spill) mem_hreg_free(v->spill); v->spill_cap = C + 16; v->spill = (uint64_t *)mem_hreg_alloc(v->spill_cap * 4 * 8); }
        k_crt_batch<<<(unsigned)C, CRT_THREADS, 0, v->s>>>(xb[0], xb[1], xb[2], xb[3], dd, 0, (int)C, q, rns_gconst(), v->spill, bi_decimal);
        k_scatter_runs<<<nblk(q), 256, 0, v->s>>>(xa[0], c, nc, R, rows, (size_t)r * rows, C);
        HIP_CHECK(hipStreamSynchronize(v->s));
        tl[r] = s1 - s0; tf[r] = s2 - s1; tc[r] = mem_now() - s2;
    }
    double t2 = mem_now();
    /* merge the run spills in limb order: run (j, r) spills at limb R j + (r+1) rows */
    const uint64_t BB = EC_1E18;
    for (size_t j = 0; j < C; j++) for (int r = 0; r < NR; r++) {
        {
            size_t k = R * j + (size_t)(r + 1) * rows; const uint64_t *w = RS[r].spill + j * 4; uint64_t cy = 0;
            if (k >= nc) continue;
            if (bi_decimal) {
                for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = c[k] + w[t] + cy; cy = sm >= BB; c[k] = cy ? sm - BB : sm; }
                while (cy && k < nc) { uint64_t sm = c[k] + cy; cy = sm >= BB; c[k] = cy ? sm - BB : sm; k++; }
            } else {
                for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = c[k] + w[t], c1 = sm < c[k]; sm += cy; c1 += sm < cy; c[k] = sm; cy = c1; }
                while (cy && k < nc) { uint64_t sm = c[k] + cy; cy = sm < cy; c[k] = sm; k++; }
            }
        }
    }
    if (c != Cout->l) memcpy(Cout->l, c, nc * 8);
    Cout->n = nc; bi_norm(Cout);
    double t3 = mem_now();
    rns_dist_st.n++; rns_dist_st.t_total += t3 - t0; rns_dist_st.t_stage += t1 - t0; rns_dist_st.t_merge += t3 - t2;
    double ml = 0, mf = 0, mc = 0; for (int r = 0; r < NR; r++) { if (tl[r] > ml) ml = tl[r]; if (tf[r] > mf) mf = tf[r]; if (tc[r] > mc) mc = tc[r]; }
    rns_dist_st.t_load += ml; rns_dist_st.t_ntt += mf; rns_dist_st.t_crt += mc;
    if (getenv("RNS_VERBOSE")) printf("dist 2^%d = 2^%d x 2^%d (%zu limbs): stage %.3f load %.3f ntt %.3f crt %.3f merge %.3f total %.3f s\n",
                                      logn, logR, logC, nc, t1 - t0, ml, mf, mc, t3 - t2, t3 - t0);
    if (dist_st.on) { printf("   ntt parts (all ranks summed / 4): local rows %.3f cols %.3f twiddle %.3f pack+unpack %.3f all-to-all %.3f\n", dist_st.t_local1 / 4, dist_st.t_local2 / 4, dist_st.t_tw / 4, dist_st.t_pack / 4, dist_st.t_a2a / 4);
                      dist_st.t_local1 = dist_st.t_local2 = dist_st.t_tw = dist_st.t_pack = dist_st.t_a2a = 0; }
}
