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
#include "mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
#define DIST_LOGN_MAX 31
/* the plane cap; DIST_LOGN_TEST lowers it (tests only) so the grid split runs at small sizes */
static int dist_logn_max(void) { const char *e = getenv("DIST_LOGN_TEST"); int v = e ? atoi(e) : DIST_LOGN_MAX; return v < 20 || v > DIST_LOGN_MAX ? DIST_LOGN_MAX : v; }

/* a limb accessor: either one flat array or four quarters (a dbig view) */
struct acc { const uint64_t *q[NR]; uint64_t *w[NR]; size_t qc, lo, n; int lq, m3, flat; dbig *owner; };
__device__ static inline uint64_t acc_get(const struct acc a, size_t i)      /* i < n */
{
    size_t m = a.lo + i;
    if (a.flat) return a.q[0][m];
    size_t d = a.m3 ? (m >> a.lq) / 3 : m >> a.lq; return a.q[d][m - d * a.qc];
}
__device__ static inline uint64_t *acc_ptr(const struct acc a, size_t i)
{
    size_t m = a.lo + i;
    if (a.flat) return a.w[0] + m;
    size_t d = a.m3 ? (m >> a.lq) / 3 : m >> a.lq; return a.w[d] + (m - d * a.qc);
}
static struct acc acc_flat(const uint64_t *p, size_t n) { struct acc a; memset(&a, 0, sizeof a); a.q[0] = p; a.w[0] = (uint64_t *)p; a.n = n; a.flat = 1; return a; }
static struct acc acc_db(const dbig *x, size_t lo, size_t n) { struct acc a; memset(&a, 0, sizeof a); for (int d = 0; d < NR; d++) { a.q[d] = x->q[d]; a.w[d] = x->q[d]; } a.qc = x->qc; a.lq = x->lq; a.m3 = x->m3; a.lo = x->off + lo; a.n = n; a.owner = (dbig *)x; return a; }

struct rank_state {
    comm *cm; ntt_ctx *ctx[EC_NP]; hipStream_t s;
    dpool desc; uint64_t *spill; size_t spill_cap;
    struct { dist_plan pl; int logR, logC, built; } plan[EC_NP];
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

/* the core: C = A B, na + nb limbs of result through accessors; nc limbs written */
static void dist_core(struct acc A, struct acc B, struct acc Cw, size_t nc)
{
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    if (logn < 20) logn = 20;                                  /* R, C >= 2^10 */
    if (logn > dist_logn_max()) { fprintf(stderr, "dist_core: %zu limbs > 2^%d points\n", nc, dist_logn_max()); exit(1); }
    int logR = logn / 2, logC = logn - logR;
    size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rows = R / NR, q = n / NR;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    double tl[NR], tf[NR], tc[NR];
#pragma omp parallel num_threads(NR)
    {
        int r = omp_get_thread_num(); struct rank_state *v = &RS[r];
        HIP_CHECK(hipSetDevice(r));
        /* planes: xa[4] in pool 0 (4 q = the standard 2^pool_log limbs at n = 2^31); xb | sbuf | rbuf in pool 1 (3 q);
         * the transpose scratch reuses the slab buffers after the last inverse */
        uint64_t *pl = (uint64_t *)rns_dpool(r, 0, (size_t)EC_NP * q * 8), *p1 = (uint64_t *)rns_dpool(r, 1, (size_t)3 * q * 8);
        uint64_t *xa[EC_NP], *xb = p1, *sl = p1 + q, *xt = sl;
        for (int p = 0; p < EC_NP; p++) xa[p] = pl + (size_t)p * q;
        for (int p = 0; p < EC_NP; p++) {
            if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC) {
                if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                dist_plan_create_shared(&v->plan[p].pl, v->cm, v->ctx[p], p, logR, logC, sl, sl + q);
                v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1;
            }
        }
        double s0 = mem_now(), lg = 0, lf = 0;
        for (int p = 0; p < EC_NP; p++) {
            ec_mod m = ec_mod_get(p);
            double g0 = mem_now();
            k_gather<<<nblk(q), 256, 0, v->s>>>(xa[p], A, R, rows, (size_t)r * rows, C, m);
            k_gather<<<nblk(q), 256, 0, v->s>>>(xb, B, R, rows, (size_t)r * rows, C, m);
            HIP_CHECK(hipStreamSynchronize(v->s)); lg += mem_now() - g0;
            dist_fwd(&v->plan[p].pl, xa[p], v->s);
            dist_fwd(&v->plan[p].pl, xb, v->s);
            dist_pw(&v->plan[p].pl, xa[p], xb, v->s);
            dist_inv(&v->plan[p].pl, xa[p], v->s);
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
    if (getenv("RNS_VERBOSE")) printf("dist 2^%d = 2^%d x 2^%d (%zu limbs): load %.3f ntt %.3f crt %.3f spills %.3f total %.3f s\n", logn, logR, logC, nc, ml, mf, mc, mem_now() - tsp, mem_now() - t0);
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
    if (nc <= ((size_t)1 << dist_logn_max())) {
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
static size_t plane_pts(size_t nc) { size_t n = (size_t)1 << 20; while (n < nc) n <<= 1; return n; }
/* piece counts (ka, kb) for a product too long for one plane: every piece product ceil(na/ka) + ceil(nb/kb)
 * must fit 2^31 points; choose the grid with the fewest plane points in total (then the fewest products).
 * Halving the longer operand alone -- the first version -- gave 8 planes of 2^31 for the decimal top product
 * (2.22e9 x 2.22e9 limbs: halves of 1.11e9 still exceed a plane together); 2 x 3 pieces give 6 (RESULTS.md 66) */
static void split_grid(size_t na, size_t nb, int *ka, int *kb)
{
    size_t cap = (size_t)1 << dist_logn_max(), best = 0; *ka = *kb = 0;
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j;
        if (pa + pb > cap) continue;
        size_t cost = (size_t)i * j * plane_pts(pa + pb);
        if (!*ka || cost < best || (cost == best && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
    if (!*ka) { fprintf(stderr, "split_grid: %zu x %zu limbs\n", na, nb); exit(1); }
}
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
    if (nc <= ((size_t)1 << dist_logn_max())) {
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
