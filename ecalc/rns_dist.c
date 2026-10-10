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
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include "rns_mul.h"
#include "rns_int.h"
#include "ntt_dist.h"
#include "dbig.h"
#include "mdb.h"
#include "mem.h"
#include "mn_plan.h"                                        /* Phase 13d L: the plan printer asks this file's decisions */
#include "p24.h"                                            /* Phase 15 Batch 3 P24 (results/P2415.md): 24 digits per point in the mn tier (MN_P24) */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define NR 4
#define DIST_LOGN_MAX 31
/* the plane cap; DIST_LOGN_TEST lowers it (tests only) so the grid split runs at small sizes */
static int g_cap_test;                                        /* Phase 12 G: rns_dist_cap_test -- the tree's own forced cap (MN_TREE_LOGN_TEST), 0 = DIST_LOGN_TEST / the default */
static int dist_logn_max(void) { const char *e = getenv("DIST_LOGN_TEST"); int v = g_cap_test ? g_cap_test : e ? atoi(e) : DIST_LOGN_MAX; return v < 20 || v > DIST_LOGN_MAX ? DIST_LOGN_MAX : v; }
void rns_dist_cache_release(void);
/* Phase 12 G (tests): force the plane cap 2^logn (0: back to the default) for the products that follow -- mn_tree brackets its
 * levels with it (MN_TREE_LOGN_TEST) so the tree's grid runs at 10^10 on one node while the division keeps its own cap.  The
 * transform cache's slots are sized by the cap, so they are released at every change */
void rns_dist_cap_test(int logn) { rns_dist_cache_release(); g_cap_test = logn; }
/* Phase 10 A6: the pointwise product fused into the column inverse's first pass (DIST_PW_FUSE, default 1; bit-identical),
 * and the four-step split logR = logn / 2 + DIST_LOGR_DELTA (0: as before; +1 puts 7-stage passes -- the register-blocked
 * body -- on the rows of a 2^31 plane: logR 16, logC 15 ... measured, see results/G.md) */
static int dist_pw_fused(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_PW_FUSE"); v = e ? atoi(e) != 0 : 1; } return v; }
static int dist_logr_delta(void) { static int v = -99; if (v == -99) { const char *e = getenv("DIST_LOGR_DELTA"); v = e ? atoi(e) : 0; if (v < -3 || v > 3) v = 0; } return v; }

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
    struct { dist_plan pl; int logR, logC, built; comm *cm; uint64_t *sl; } plan[EC_NP];   /* cm: the plan's communicator (the node's xGMI, or a group's layered one); sl: its slabs */
    uint64_t *ssl, *stmp; size_t ssl_q, stmp_q;          /* Phase 14 P2 (TASKS 2.4, DIST_MN_SYM_SLABS=1): mn_core's slabs and layered scratch in the transport's symmetric pool, kept */
};
static struct rank_state RS[NR];
static int g_init;
static uint64_t *g_stage; static size_t g_stage_cap;      /* registered host staging for unregistered host operands/result */
struct rns_dist_stats rns_dist_st;

static int mn_sym_slabs(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_MN_SYM_SLABS"); v = e ? atoi(e) != 0 : 0; } return v; }   /* Phase 14 P2 (TASKS 2.4) */
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
static int dist_r3(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_R3"); v = e ? atoi(e) : rns_planes_3q30 > 0; if (v && !ec_has_radix3()) { fprintf(stderr, "DIST_R3: the prime set has no 3 2^k roots\n"); v = 0; } } return v; }   /* Phase 11 B3 (agent P): the default follows the plane pools sized for it at init (rns_mul.c) */
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
/* ---- Phase 10 A1 (PLAN 20, I4; results/A-div.md "B2"): the transform cache ---------------------------------------
 * A slot holds the forward transform of one device operand (a dbig view, identified by its quarters, offset and length)
 * on planes of q limbs per prime per rank.  dist_core looks every device
 * operand up in all slots (a hit skips its gather and forward transform and reads the cached planes in the pointwise
 * product) and on a miss forwards it into the slot the caller designated (-1: not cached).  mul_grid designates the
 * slots by the grid's loop (B piece j is reused by every A piece inside iteration j; A piece i by every j): with N free
 * slots the A pieces get min(ka, N - 1), B the rest, so a ka x kb grid costs about ka + kb forward transforms instead
 * of 2 ka kb.  The keys are addresses, so no entry outlives the product that made it: the slots are emptied at the end
 * of every grid product -- except pinned slots: rns_dist_cache_hold(1) makes the next product keep its B pieces (the
 * reciprocal's last doubling: Q_t = Q, the operand of the division's X Q, owned by the caller across both) in pinned
 * slots until hold(0).  The planes: 16 GiB per slot per APU (the largest plane, EC_NP x 2^29 limbs), allocated ONCE per
 * process by hipMalloc on the four APUs in parallel at the first product that wants them and kept until
 * rns_dist_cache_release() (the end of the dm phase) -- not from the block pool: at 4e10 the pool has no room at the
 * products' peaks and its fallback hipMalloc of 3 x 64 GB oversubscribed the node (the transforms ran 8x slower, 208 s).
 * Only as many slots are allocated as the free device memory (hipMemGetInfo, minus RNS_DIST_CACHE_MARGIN_GB, default 24)
 * allows on every APU.  RNS_DIST_CACHE = the slot count (default 2, 0: off). */
#define DIST_CACHE_MAX 8
struct dist_slot { const void *key; size_t lo, n, q; uint64_t *pl[NR]; int pinned, mn, np;
                  int k, p24; uint64_t *pp[NR][EC_NP]; };     /* mn: a sharded operand (key = its mdb); np: the primes of its planes (Phase 15 NP: ECALC_NP=auto);
                                                               * Phase 15 PC: k = the primes the slot holds (0: all -- a held slot), p24 = the key's 24-digit form, pp = the planes per prime (pool slots) */
static struct { int n, nmn, navail, tried, hold, pin_next, init; struct dist_slot s[DIST_CACHE_MAX]; size_t hits, misses, bytes; double t_alloc;
                size_t slot_bytes, slot_q; int pc; } g_cache;     /* Phase 15 TC: one slot's bytes per APU, and the q (limbs per prime per rank) it holds under RNS_DIST_CACHE_FIT (0: unbounded);
                                                               * Phase 15 PC: pc = the slots now are RNS_DIST_CACHE_PARTIAL's (planes from the block pool for one grid product) */
/* ---- Phase 15 TC (docs/TARGET_TASKS.md T7, results/TC15.md): RNS_DIST_CACHE_FIT=1 -- the slots sized to the cap and bounded by the budget ----
 * Without it (the default) a slot is EC_NP x 2^(dist_logn_max - 2) limbs per APU (16 GiB at four primes) whatever POOL_LOG is, and the
 * count is what hipMemGetInfo's free memory less the margin allows -- every node-process on a node reading the same free memory, none
 * of it in the budget check or the memory model.  With it:
 *   size   a slot is rns_pool0_np() x 2^(c - 2) limbs per APU, c = min(dist_logn_max, POOL_LOG): the largest rank plane of either tier
 *          (mn_logn_cap: q <= 2^(c - 2); the dist tier's planes live in pools of 2^POOL_LOG points); a product whose q exceeds the slot
 *          is not cached (a guard in cache_plan);
 *   count  min(the configured slots, (free - margin) / (slot x the node-processes on this node), (room - reserve / ranks) / (4 x slot)),
 *          where the room = this rank's share of ECALC_NODE_GB less its peak as ECALC_BUDGET_CHECK models it (rns_dist_cache_budget,
 *          called by the check; without the check: no budget bound) and the reserve = RNS_DIST_CACHE_FIT_RESERVE_GB (32) per node.  At
 *          the 5.1e13 target (and 4.74e13) the room less the reserve is below one slot's 68.7 GB per node, so the cache stays
 *          unallocated (0 slots) and the node stays at its modelled peak.
 * The slots' bytes are in the memory report ("other") whether or not the switch is on. */
static int cache_fit_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RNS_DIST_CACHE_FIT"); v = e ? atoi(e) != 0 : 0; } return v; }
static double g_cache_room; static int g_cache_room_set, g_cache_ranks = 1;   /* this rank's budget room (bytes), whether the check gave one, the node-processes on its node */
void rns_dist_cache_budget(double room_bytes, int ranks_on_node) { g_cache_room = room_bytes; g_cache_room_set = 1; g_cache_ranks = ranks_on_node > 0 ? ranks_on_node : 1; }
/* FIT's reserve per node (RNS_DIST_CACHE_FIT_RESERVE_GB, 32): what the budget check's init peak does not hold and the run still takes
 * -- at the target mem_model.py's node (455.4 GB: the SHMEM pool, the VMM seed buffers, the bs growth, the writer) is 28.8 GB above
 * the layout's (426.6); shared by the node's ranks */
static double cache_fit_reserve(void) { const char *e = getenv("RNS_DIST_CACHE_FIT_RESERVE_GB"); return (e ? atof(e) : 32.0) * 1e9; }
static void cache_acct(int ndev, size_t b[][MEM_DEV_NCAT])   /* the memory report: the slots' planes as "other" */
{
    for (int d = 0; d < ndev && d < NR; d++) for (int i = 0; i < DIST_CACHE_MAX; i++) if (g_cache.s[i].pl[d]) b[d][MEM_DEV_OTHER] += g_cache.slot_bytes;
}
/* the configured slot count: RNS_DIST_CACHE for the single-node tier (default 0: at 4e10 the planes' mapping, 7-9 s for
 * 2 x 16 GiB per APU at 0.055 s/GB, costs more than the ~4 s of transforms it saves -- results/G.md), RNS_DIST_CACHE_MN over
 * shares (default 2: the planes are 1/gt the size and a hit also skips the operand's all-to-all redistribution) */
static int cache_slots_of(int mn)
{
    if (!g_cache.init) {
        const char *e = getenv("RNS_DIST_CACHE"), *m = getenv("RNS_DIST_CACHE_MN"); int v = e ? atoi(e) : 0, w = m ? atoi(m) : 2;
        if (v < 0) v = 0; if (v > DIST_CACHE_MAX) v = DIST_CACHE_MAX; if (w < 0) w = 0; if (w > DIST_CACHE_MAX) w = DIST_CACHE_MAX;
        g_cache.n = v; g_cache.nmn = w; g_cache.init = 1;
    }
    return mn ? g_cache.nmn : g_cache.n;
}
static int g_cache_mn;                                        /* the tier asking (set by mul_grid / mn_grid before the products) */
static int cache_slots(void) { return cache_slots_of(g_cache_mn); }
static int cache_slot_log(void) { int c = dist_logn_max(); if (cache_fit_on() && rns_pool_log() > 0 && rns_pool_log() < c) c = rns_pool_log(); return c - 2; }   /* Phase 15 TC: log2 of a slot's q (FIT: at the cap min(dist_logn_max, POOL_LOG)) */
static size_t cache_slot_bytes(void) { return (size_t)rns_pool0_np() * ((size_t)1 << cache_slot_log()) * 8; }   /* the largest plane per prime per rank (NP: the primes pool 0 is made for) */
/* the slots' planes, allocated at the first product that wants them: as many of the configured slots as every APU's free
 * memory allows (hipMemGetInfo minus the margin), the four APUs in parallel; the count is decided once */
static int cache_avail(void)
{
    if (g_cache.tried || !cache_slots()) return g_cache.navail;
    g_cache.tried = 1;
    size_t bytes = cache_slot_bytes(), margin = (size_t)(getenv("RNS_DIST_CACHE_MARGIN_GB") ? atof(getenv("RNS_DIST_CACHE_MARGIN_GB")) : 24.0) * 1e9;
    int n = cache_slots(), cur, fit_on = cache_fit_on(), nbud = -1; HIP_CHECK(hipGetDevice(&cur));
    size_t share = fit_on ? (size_t)g_cache_ranks : 1;           /* Phase 15 TC (FIT): the free memory is the node's, shared by its node-processes */
    for (int r = 0; r < NR; r++) {
        size_t fr = 0, tot = 0; HIP_CHECK(hipSetDevice(r)); HIP_CHECK(hipMemGetInfo(&fr, &tot));
        int fit = fr > margin ? (int)((fr - margin) / (bytes * share)) : 0; if (fit < n) n = fit;
    }
    HIP_CHECK(hipSetDevice(cur));
    double room = g_cache_room - cache_fit_reserve() / g_cache_ranks;
    if (fit_on && g_cache_room_set) { nbud = room > 0 ? (int)(room / ((double)NR * bytes)) : 0; if (nbud < n) n = nbud; }   /* Phase 15 TC (FIT): the budget room less the reserve */
    if (n < 0) n = 0;
    g_cache.slot_bytes = bytes; g_cache.slot_q = fit_on ? (size_t)1 << cache_slot_log() : 0;
    double t0 = mem_now();
    if (n > 0) {
#pragma omp parallel num_threads(NR)
        {
            int r = omp_get_thread_num(); HIP_CHECK(hipSetDevice(r));
            for (int i = 0; i < n; i++) HIP_CHECK(hipMalloc(&g_cache.s[i].pl[r], bytes));
        }
        HIP_CHECK(hipSetDevice(cur));
    }
    g_cache.navail = n; g_cache.bytes = (size_t)n * NR * bytes; g_cache.t_alloc = mem_now() - t0;
    if (n > 0) mem_acct_register(cache_acct);                  /* Phase 15 TC: the slots in the memory report */
    if (getenv("RNS_VERBOSE") || getenv("ECALC_VERBOSE")) {
        printf("   transform cache: %d of %d slots of %.1f GiB per APU allocated in %.2f s (margin %.0f GB)", n, cache_slots(), bytes / 1073741824.0, g_cache.t_alloc, margin / 1e9);
        if (fit_on && g_cache_room_set) printf(" [RNS_DIST_CACHE_FIT: slot 2^%d limbs x %d primes, %d node-process%s on the node, budget room %.1f GB less the reserve %.1f -> at most %d slot%s]",
                                               cache_slot_log(), rns_pool0_np(), g_cache_ranks, g_cache_ranks > 1 ? "es" : "", g_cache_room / 1e9, cache_fit_reserve() / g_cache_ranks / 1e9, nbud, nbud == 1 ? "" : "s");
        else if (fit_on) printf(" [RNS_DIST_CACHE_FIT: slot 2^%d limbs x %d primes; no budget room (ECALC_BUDGET_CHECK=0): the free memory alone]", cache_slot_log(), rns_pool0_np());
        printf(" = %.2f GB per node-process\n", (double)g_cache.bytes / 1e9);
    }
    return n;
}
static void cache_drop(int pinned_too)                        /* the slots emptied (the planes stay allocated) */
{
    for (int i = 0; i < DIST_CACHE_MAX; i++) { struct dist_slot *s = &g_cache.s[i]; if (s->pinned && !pinned_too) continue; s->key = 0; s->q = 0; s->pinned = 0; }
}
static void b_extra_release(void);
static void cache_pc_report(void);                          /* Phase 15 PC */
void rns_dist_cache_release(void)                             /* the planes freed (the end of the dm phase); the next product may allocate again */
{
    cache_pc_report();                                        /* Phase 15 PC: the run's partial slots */
    cache_drop(1);
    b_extra_release();                                        /* Phase 13b B: the B forms' extra planes (RNS_STRATEGY=B|B4) */
    if (!g_cache.tried) return;
    int cur; HIP_CHECK(hipGetDevice(&cur));
    for (int i = 0; i < DIST_CACHE_MAX; i++) for (int r = 0; r < NR; r++) if (g_cache.s[i].pl[r]) { HIP_CHECK(hipSetDevice(r)); HIP_CHECK(hipFree(g_cache.s[i].pl[r])); g_cache.s[i].pl[r] = 0; }
    HIP_CHECK(hipSetDevice(cur));
    g_cache.tried = 0; g_cache.navail = 0; g_cache.bytes = 0; g_cache.slot_q = 0;
}
/* Holding is off unless RNS_DIST_CACHE_HOLD=1: the in-product policy already transforms each of Q's pieces once inside X Q
 * (its B slot cycles through them), so pinning them from the reciprocal only moves those transforms there -- and the pinned
 * slots are missing from the A_h mu product in between (with N slots: 4 + 5 + 5 transforms unheld vs 4 + 8 + 4 held at
 * N = 4 for the reciprocal's top, A_h mu and X Q at 4e10).  Kept as the measured form of A-div's B2. */
int rns_dist_cache_hold(int on)
{
    if (on) { const char *e = getenv("RNS_DIST_CACHE_HOLD"); int f = e ? atoi(e) : 0; if (!cache_slots() || !f) on = 0; }
    g_cache.hold = g_cache.pin_next = on; if (!on) cache_drop(1); return on;
}
void rns_dist_cache_stats(size_t *hits, size_t *misses) { *hits = g_cache.hits; *misses = g_cache.misses; g_cache.hits = g_cache.misses = 0; }
static int cache_lookup(const void *key, size_t lo, size_t n, size_t q, int mn, int np, int p24)   /* the slot holding this operand's transform on q-limb planes (np primes; PC: p24 its form), or -1 */
{
    for (int i = 0; i < g_cache.navail; i++) { const struct dist_slot *s = &g_cache.s[i]; if (s->key == key && s->lo == lo && s->n == n && s->q == q && s->mn == mn && s->np == np && s->p24 == p24) return i; }
    return -1;
}
static int cache_find(const struct acc *a, size_t q, int np) { return a->flat || !cache_slots() ? -1 : cache_lookup(a->q[0], a->lo, a->n, q, 0, np, 0); }
/* the slots of the two operands of one product: hits taken anywhere, misses filled in the designated slots (never the slot
 * the other operand hits in); the misses' keys set here (the planes are allocated by the ranks) */
static int g_cache_last[2];                                   /* Phase 15 CX (RNS_DIST_CACHE_TRACE): the last product's operands A, B: 2 hit, 1 cached (a miss into a slot), 0 not cached */
static void cache_plan(int *sa, int *sb, int ha, int hb, const void *ka, size_t la, size_t na, const void *kb, size_t lb, size_t nb, size_t q, int mn, int np, int p24)
{
    if (np > rns_pool0_np() && !g_cache.pc) *sa = *sb = -1;   /* NP: the slots hold rns_pool0_np() planes (never fewer than a product of the run needs; a guard); PC: a pool slot holds k of any np */
    if (g_cache.slot_q && q > g_cache.slot_q) *sa = *sb = -1;   /* Phase 15 TC (FIT): a plane larger than the slot is not cached (a guard) */
    if (*sa >= g_cache.navail) *sa = -1; if (*sb >= g_cache.navail) *sb = -1;
    if (ha >= 0) *sa = ha; else if (*sa >= 0 && *sa == hb) *sa = -1;
    if (hb >= 0) *sb = hb; else if (*sb >= 0 && *sb == ha) *sb = -1;
    if (*sa >= 0 && *sa == *sb && ha < 0) *sb = -1;
    for (int k = 0; k < 2; k++) {
        int si = k ? *sb : *sa, hit = k ? hb : ha;
        if (si < 0) continue;
        struct dist_slot *s = &g_cache.s[si];
        if (hit >= 0) { g_cache.hits++; continue; }
        g_cache.misses++;
        s->key = k ? kb : ka; s->lo = k ? lb : la; s->n = k ? nb : na; s->q = q; s->mn = mn; s->np = np; s->p24 = p24;
    }
    g_cache_last[0] = ha >= 0 ? 2 : *sa >= 0 ? 1 : 0; g_cache_last[1] = hb >= 0 ? 2 : *sb >= 0 ? 1 : 0;
}
/* ---- Phase 15 Batch 3 PC (results/PC15.md): RNS_DIST_CACHE_PARTIAL=1 -- partial slots that fit, drawn per grid product from the block pool ----
 * At the 5.1e13 target a full slot (68.7 GB per node) fits no phase; the block pool (the arena, mapped at init and inside the budget) has
 * room at the grid products -- the arena's slack at the tree (36.5 GB per node, modelled), DKM's unused hole in the division (the arena is
 * laid for the default division) -- so under the switch the mn tier's slots are not the held hipMalloc'd planes but, for each grid
 * product (mn_grid, pieces > 1), planes of the product's own q taken from the pool at its start and returned at its end:
 *   planes  P = min over the four APUs of (free bytes - the product's own peak need (rns_mul_dist_mn_scratch) - the margin
 *           RNS_DIST_CACHE_PARTIAL_MARGIN_GB per APU (0.5)) / (q x 8), and at most what the free extents hold as they are
 *           (db_pool_fit_count: no remap, no growth); the group's minimum (every node must hold the same slots);
 *   slots   P >= np: min(RNS_DIST_CACHE_MN, P / np) full slots; else 1 slot of P primes (primes 0 .. P - 1 of the operand);
 *           RNS_DIST_CACHE_PARTIAL_K = k caps a slot at k primes (then one slot); RNS_DIST_CACHE_PARTIAL_FORCE = P sets P whatever the
 *           room (tests only: the pool may then grow by hipMalloc, reported);
 *   hits    a hit on a k-prime slot skips primes < k's gathers and forward transforms (A: copied from the slot; B: read in place); the
 *           operand's redistribution is skipped only when k >= the product's primes (a full hit);
 *   P24     the P24 products (MN_P24) are cached too: the key carries the form (s->p24), the planes are the same q per prime;
 *   loop    mn_grid loops along the grid's longer axis (kb > ka: A's pieces outer, the slots held by A's pieces; CX's proposal);
 *   memory  nothing outside the pool: the slots add no byte to the node (the pool's free bytes at the product are the room); the memory
 *           report's pool figures hold them; ECALC_VERBOSE prints the slots each grid product takes and the run's summary.
 * Held slots (RNS_DIST_CACHE_FIT / the default) are not allocated by the mn tier under the switch; pins (RNS_DIST_CACHE_HOLD) are dropped. */
static size_t grp_max(mn_group *G, size_t v);
static int g_node_of(mn_group *G);
static int pc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RNS_DIST_CACHE_PARTIAL"); v = e ? atoi(e) != 0 : 0; } return v; }
static int cache_slot_k(int si, int np) { if (si < 0) return 0; int k = g_cache.s[si].k; return k > 0 && k < np ? k : np; }   /* the primes slot si holds for an np-prime product */
static uint64_t *cache_pl(int si, int d, int p, size_t q) { const struct dist_slot *s = &g_cache.s[si]; return g_cache.pc ? s->pp[d][p] : s->pl[d] + (size_t)p * q; }   /* slot si's plane of prime p on APU d */
static struct { size_t products, taken, planes_max, bytes_max, planes_sum, grown; } g_pcst;   /* PC: the run's partial slots (printed at the release) */
/* the slots of one grid product: q limbs per prime per rank (the grid's largest piece), np its primes; na x nb the product, A / B / C the
 * operands and the result (this node's shares for the need); returns the slot count (g_cache.navail), the primes per slot in *kout */
static int cache_pc_take(mn_group *G, size_t na, size_t nb, const mdbv *A, const mdbv *B, const mdb *Cn, size_t q, int np, int *kout)
{
    int g = G->g, node = g_node_of(G); *kout = 0;
    const int held = g_cache.tried && g_cache.navail;         /* held slots exist (the dist tier's RNS_DIST_CACHE): not mixed (P = 0, after the collective) */
    size_t al = (size_t)4096 * 8, plane = (q * 8 + al - 1) / al * al;   /* (db_pool_alloc's rounding: DB_ALIGN limbs) */
    size_t sa0, sa1, sb0, sb1, sc0, sc1; mdb_share(A->m, node, &sa0, &sa1); mdb_share(B->m, node, &sb0, &sb1); mdb_share(Cn, node, &sc0, &sc1);
#define OVL(lo, hi, o, n) ((hi) > (o) && (lo) < (o) + (n) ? ((hi) < (o) + (n) ? (hi) : (o) + (n)) - ((lo) > (o) ? (lo) : (o)) : 0)
    size_t sha = OVL(sa0, sa1, A->off, A->len), shb = OVL(sb0, sb1, B->off, B->len);
#undef OVL
    size_t need = rns_mul_dist_mn_scratch(na, nb, 0, g, sha, shb, sc1 - sc0, 0);
    double mg = (getenv("RNS_DIST_CACHE_PARTIAL_MARGIN_GB") ? atof(getenv("RNS_DIST_CACHE_PARTIAL_MARGIN_GB")) : 0.5) * 1e9;
    const size_t PM = (size_t)DIST_CACHE_MAX * EC_NP; size_t P = PM, fmin = (size_t)-1;
    for (int d = 0; d < NR; d++) {
        size_t fr = db_pool_free_bytes(d), fc = db_pool_fit_count(d, plane); if (fr < fmin) fmin = fr;
        size_t a = (double)fr > (double)need + mg ? (size_t)(((double)fr - (double)need - mg) / (double)plane) : 0; if (fc < a) a = fc; if (a < P) P = a;
    }
    const char *fe = getenv("RNS_DIST_CACHE_PARTIAL_FORCE"); if (fe) P = (size_t)atoi(fe);
    if (P > PM) P = PM;
    if (held) P = 0;
    P = PM - grp_max(G, PM - P);                              /* the group's minimum (a collective: every node of the group is here) */
    if (held) return 0;
    int kmax = getenv("RNS_DIST_CACHE_PARTIAL_K") ? atoi(getenv("RNS_DIST_CACHE_PARTIAL_K")) : np; if (kmax < 1) kmax = 1; if (kmax > np) kmax = np;
    int ns, k;
    if ((int)P >= kmax) { k = kmax; ns = (int)(P / (size_t)kmax); if (ns > cache_slots()) ns = cache_slots(); if (k < np && ns > 1) ns = 1; }   /* k < np: one slot (CX: one slot of more primes beats more slots of fewer) */
    else { ns = P > 0 ? 1 : 0; k = (int)P; }
    if (ns > DIST_CACHE_MAX) ns = DIST_CACHE_MAX;
    g_pcst.products++;
    if (!ns || !k) { g_cache.navail = 0; return 0; }
    size_t hm0 = 0; for (int d = 0; d < NR; d++) hm0 += db_pool_hipmalloc_bytes(d);
    for (int i = 0; i < ns; i++) { struct dist_slot *s = &g_cache.s[i]; memset(s->pp, 0, sizeof s->pp); s->key = 0; s->q = 0; s->pinned = 0; s->k = k;
                                   for (int d = 0; d < NR; d++) for (int p = 0; p < k; p++) s->pp[d][p] = db_pool_alloc(d, plane); }
    size_t hm1 = 0; for (int d = 0; d < NR; d++) hm1 += db_pool_hipmalloc_bytes(d);
    if (hm1 > hm0) g_pcst.grown += hm1 - hm0;                 /* (only under RNS_DIST_CACHE_PARTIAL_FORCE: the rule never takes more than the extents hold) */
    g_cache.pc = 1; g_cache.navail = ns; g_cache.slot_q = q;
    g_pcst.taken++; g_pcst.planes_sum += (size_t)ns * k; if ((size_t)ns * k > g_pcst.planes_max) g_pcst.planes_max = (size_t)ns * k;
    if ((size_t)ns * k * plane > g_pcst.bytes_max) g_pcst.bytes_max = (size_t)ns * k * plane;
    *kout = k;
    if (getenv("RNS_VERBOSE") || getenv("ECALC_VERBOSE"))
        printf("   transform cache (RNS_DIST_CACHE_PARTIAL) node %d: %d slot%s of %d of %d primes, q 2^%.2f, from the block pool: %.2f GB per APU (%.2f per node) | pool free %.2f GB per APU (the smallest), the product's need %.2f, margin %.2f -> %zu planes%s%s\n",
               node, ns, ns > 1 ? "s" : "", k, np, log2((double)q), (double)ns * k * plane / 1e9, 4.0 * ns * k * plane / 1e9, fmin / 1e9, need / 1e9, mg / 1e9, P,
               fe ? " (RNS_DIST_CACHE_PARTIAL_FORCE)" : "", hm1 > hm0 ? " -- the pool GREW" : "");
    return ns;
}
static void cache_pc_give(void)                               /* the product's partial slots back to the pool */
{
    if (!g_cache.pc) return;
    for (int i = 0; i < DIST_CACHE_MAX; i++) { struct dist_slot *s = &g_cache.s[i];
        for (int d = 0; d < NR; d++) for (int p = 0; p < EC_NP; p++) if (s->pp[d][p]) { db_pool_free(d, s->pp[d][p]); s->pp[d][p] = 0; }
        s->key = 0; s->q = 0; s->k = 0; s->pinned = 0; }
    g_cache.pc = 0; g_cache.navail = 0; g_cache.slot_q = 0;
}
static void cache_pc_report(void)                             /* the run's partial slots (rns_dist_cache_release: the division's end) */
{
    if (!pc_on() || !g_pcst.products) return;
    if (getenv("RNS_VERBOSE") || getenv("ECALC_VERBOSE"))
        printf("   transform cache (RNS_DIST_CACHE_PARTIAL): %zu grid products, %zu with slots (planes per product: max %zu, mean %.2f), the largest %.2f GB per APU from the block pool%s\n",
               g_pcst.products, g_pcst.taken, g_pcst.planes_max, g_pcst.taken ? (double)g_pcst.planes_sum / g_pcst.taken : 0.0, g_pcst.bytes_max / 1e9,
               g_pcst.grown ? " -- the pool GREW (RNS_DIST_CACHE_PARTIAL_FORCE)" : ", no growth");
    memset(&g_pcst, 0, sizeof g_pcst);
}
/* Phase 15 CX (results/CX15.md; instrumentation, off by default): RNS_DIST_CACHE_TRACE=1 -- mn_grid prints one line per piece of a grid
 * product (its (i, j), limbs, the operands' cache state, the piece's wall and mn_core's parts; with DIST_STATS=1 also the transforms'
 * event-timed parts, summed over the four APU threads / 4).  rns_dist_cache_test_slots(n): tests (tests/cx_grid.c) set the mn tier's
 * slot count in-process, 0..the slots already allocated (the first product that wants the cache allocates RNS_DIST_CACHE_MN of them) */
static int cache_trace_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RNS_DIST_CACHE_TRACE"); v = e ? atoi(e) != 0 : 0; } return v; }
extern "C" void rns_dist_cache_test_slots(int n);
extern "C" double rns_dist_cache_alloc_s(void);
void rns_dist_cache_test_slots(int n)
{
    cache_slots_of(1); if (n < 0) n = 0; if (n > DIST_CACHE_MAX) n = DIST_CACHE_MAX;
    g_cache.nmn = n; cache_drop(1);
    if (g_cache.tried) { int k = 0; while (k < DIST_CACHE_MAX && g_cache.s[k].pl[0]) k++; g_cache.navail = n < k ? n : k; }
}
double rns_dist_cache_alloc_s(void) { return g_cache.t_alloc; }
/* ---- Phase 13b B (PLAN 29 E4, 31 axis S): the prime-per-APU product ("B form") ------------------------------------
 * RNS_STRATEGY selects the product strategy of dist_core, the single-node product under every size-1 dist product (the
 * top tree levels, the reciprocal's doublings, the division's products) and the node-local products at size > 1:
 *   C     the four-step over the four APUs (dist_core below; the original behaviour, used today where auto falls back)
 *   B     prime-per-APU: APU p transforms prime p of the whole product (planes X_p, Y_p of n points, 2^k or 3 2^k),
 *         loading both operands from wherever their quarters live (xGMI pulls), the pointwise fused into the inverse,
 *         then every APU forms a quarter of the result by the CRT over the prime planes (peer reads).  No all-to-all.
 *         2 n points per APU on ec_np APUs (at P = 3 APU 3 only takes part in the CRT).
 *   B4    B spread over all four APUs at P = 3 (ec_np = 3; at P = 4 it is B): the product mod x^n - 1 splits into its
 *         residues mod x^(n/2) - 1 and x^(n/2) + 1 -- in the forward DIF's bit-reversed output (of each third for
 *         3 2^k: ntt3.c's radix-3 stage first) the lower and the upper half.  APU p < 3 transforms X (the longer operand)
 *         whole and Y's lower residue (n/2 points: y[j] + y[j + h] after the radix-3 stage); APU 3 forms and transforms
 *         the three primes' upper residues ((y[j] - y[j + h]) w_m^j, Y read once); APU p multiplies its upper half by
 *         APU 3's plane (a peer read), inverts, and the CRT as in B.  1.5 n points per APU (C's 12 n bytes at P = 3),
 *         transforms 2.5 n on APUs 0-2 and 1.5 n on APU 3 (B: 3 n).
 *   auto  the B form (RNS_STRATEGY_FORM=B|B4, default B -- results/B13b.md) wherever its planes fit the plane
 *         pools as sized at init, else C.
 * B and B4 run every product in their form: the planes that do not fit the pools come from one grow-only buffer per APU
 * (hipMalloc, kept until rns_dist_cache_release at the end of the dm phase) -- the "B at more memory" row of the
 * design table.  auto never allocates.  Products whose operands are host arrays, whose result is not an owning dbig, or
 * that use the transform cache (RNS_DIST_CACHE, off at size 1 by default) stay C.  Every form is exact: the digits are
 * the same as C's (the product is the product; only the transform length and the stripes differ).
 * The CRT: stripes of `rows` coefficients (a power of two dividing the result's quarter length), APU d the stripes
 * [G d/4, G (d+1)/4) written straight into the result's quarters; the stripe spills (4 limbs at (g + 1) rows) go
 * through db_add_spills in the four-step's sparse layout (R = 4 rows: stripe g = 4 j + r <-> rank r, column j). */
enum { STRAT_C = 0, STRAT_B = 1, STRAT_B4 = 2, STRAT_AUTO = 3 };
static int g_strat = -1, g_strat_form = -1;
static const char *strat_name(int s) { return s == STRAT_B ? "B" : s == STRAT_B4 ? "B4" : s == STRAT_AUTO ? "auto" : "C"; }
static struct { size_t n[3]; double t[3], t_alloc; size_t extra_bytes; } g_bst;      /* products and seconds by form (C, B, B4) */
static void strat_report(void)
{
    printf("rns_dist: RNS_STRATEGY=%s%s%s: C %zu products %.2f s, B %zu products %.2f s, B4 %zu products %.2f s; extra plane memory %.2f GiB/APU (allocations %.2f s)\n",
           strat_name(g_strat), g_strat == STRAT_AUTO ? " form " : "", g_strat == STRAT_AUTO ? strat_name(g_strat_form) : "",
           g_bst.n[0], g_bst.t[0], g_bst.n[1], g_bst.t[1], g_bst.n[2], g_bst.t[2], g_bst.extra_bytes / 1073741824.0, g_bst.t_alloc);
}
/* tests: set the strategy by name ("C", "B", "B4", "auto"; 0 = RNS_STRATEGY again); returns the previous one */
extern "C" int rns_dist_strategy_set(const char *name);
static int strat_get(void);
int rns_dist_strategy_set(const char *name)
{
    int prev = strat_get();
    if (!name) { g_strat = -1; strat_get(); return prev; }
    g_strat = !strcmp(name, "B") ? STRAT_B : !strcmp(name, "B4") ? STRAT_B4 : !strcmp(name, "auto") ? STRAT_AUTO : STRAT_C;
    return prev;
}
static int strat_get(void)
{
    if (g_strat >= 0) return g_strat;
    const char *e = getenv("RNS_STRATEGY"), *f = getenv("RNS_STRATEGY_FORM");
    g_strat = STRAT_AUTO;                                                /* default since Phase 13c: auto (RESULTS 79-80); RNS_STRATEGY=C for the four-step only */
    if (e && *e) {
        if (!strcmp(e, "C") || !strcmp(e, "c")) g_strat = STRAT_C;
        else if (!strcmp(e, "B") || !strcmp(e, "b")) g_strat = STRAT_B;
        else if (!strcmp(e, "B4") || !strcmp(e, "b4")) g_strat = STRAT_B4;
        else if (!strcmp(e, "auto")) g_strat = STRAT_AUTO;
        else { ec_fatal(EC_RC_FATAL, "RNS_STRATEGY=%s: C, B, B4 or auto\n", e); }
    }
    g_strat_form = f && (!strcmp(f, "B4") || !strcmp(f, "b4")) ? STRAT_B4 : STRAT_B;   /* auto's form: B (results/B13b.md: B4 is 3-7 % slower and fits the same pools) */
    if (g_strat != STRAT_C) atexit(strat_report);
    return g_strat;
}
/* the transform length for a product of nc limbs in the B form: the smaller of 2^k and 3 2^(k-1) (radix-3 when the prime set
 * has it), at least 2^20; *T = 3 for 3 2^logk (m = 2^logk points per third), 1 for 2^logk */
static size_t b_len(size_t nc, int *T, int *logk)
{
    int k = 20; while (((size_t)1 << k) < nc) k++;
    if (ec_has_radix3() && k > 20 && ((size_t)3 << (k - 2)) >= nc) { *T = 3; *logk = k - 2; return (size_t)3 << (k - 2); }
    *T = 1; *logk = k; return (size_t)1 << k;
}
/* two-level tables of w (entries j < cnt): t2 = w^(j & 0xffff), t1 = w^(j >> 16 << 16); w^j = t2[j & 0xffff] t1[j >> 16] */
struct btw { const uint64_t *t1, *t2; };
#define BTW_SLOTS 8
static struct { int built; uint64_t w; size_t cnt; struct btw t; } g_btw[NR][EC_NP][BTW_SLOTS];   /* [dev][prime][slot] */
static int g_btw_next[NR][EC_NP];                                                             /* round-robin eviction: a product's two tables never evict each other */
static struct btw b_tables(int dev, int prime, uint64_t w, size_t cnt)
{
    for (int s = 0; s < BTW_SLOTS; s++) if (g_btw[dev][prime][s].built && g_btw[dev][prime][s].w == w && g_btw[dev][prime][s].cnt >= cnt) return g_btw[dev][prime][s].t;
    int s = g_btw_next[dev][prime]; g_btw_next[dev][prime] = (s + 1) % BTW_SLOTS;
    if (g_btw[dev][prime][s].built) { HIP_CHECK(hipFree((void *)g_btw[dev][prime][s].t.t1)); HIP_CHECK(hipFree((void *)g_btw[dev][prime][s].t.t2)); g_btw[dev][prime][s].built = 0; }
    uint64_t P = ec_P[prime], a = 1; size_t n2 = cnt < 65536 ? cnt : 65536, n1 = (cnt + 65535) / 65536;
    uint64_t *h2 = (uint64_t *)malloc(n2 * 8), *h1 = (uint64_t *)malloc(n1 * 8), *d1, *d2;
    for (size_t i = 0; i < n2; i++) { h2[i] = a; a = ec_mulmod_ref(a, w, P); }
    uint64_t w16 = ec_powmod(w, 65536, P); a = 1;
    for (size_t i = 0; i < n1; i++) { h1[i] = a; a = ec_mulmod_ref(a, w16, P); }
    HIP_CHECK(hipMalloc(&d1, n1 * 8)); HIP_CHECK(hipMalloc(&d2, n2 * 8));
    HIP_CHECK(hipMemcpy(d1, h1, n1 * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(d2, h2, n2 * 8, hipMemcpyHostToDevice));
    free(h1); free(h2);
    g_btw[dev][prime][s].built = 1; g_btw[dev][prime][s].w = w; g_btw[dev][prime][s].cnt = cnt; g_btw[dev][prime][s].t.t1 = d1; g_btw[dev][prime][s].t.t2 = d2;
    return g_btw[dev][prime][s].t;
}
__device__ static inline uint64_t btw_at(const struct btw t, size_t j, ec_mod m) { return ec_mmu(t.t2[j & 0xffff], t.t1[j >> 16], m); }
/* the operand's n points, canonical, zero beyond its length */
__global__ void k_bload(uint64_t *dst, struct acc src, size_t n, ec_mod m)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = i < src.n ? ec_canon64(acc_get(src, i), m.pu, m.mu) : 0;
}
/* B4's residues of Y, h = m/2 points per third (T thirds of m = 2^logk points; T = 1: m = n):
 *   z_r[j'] = the radix-3 stage's third r at j' < m (T = 1: y[j'])   (ntt3.c: (a0 + w3^r a1 + w3^2r a2) w_n^(r j'))
 *   lo[r h + j] = z_r[j] + z_r[j + h],  hi[r h + j] = (z_r[j] - z_r[j + h]) w_m^j     (j < h), canonical,
 * for nk primes at once (the operand read once); lo or hi null: not formed */
struct bsplit { uint64_t *lo[3], *hi[3]; ec_mod m[3]; struct btw wn[3], wm[3]; uint64_t w3[3], w3s[3]; int nk; };
__device__ static inline uint64_t add3u(uint64_t a, uint64_t b, uint64_t c, uint64_t p) { uint64_t s = ec_fold(a + b, p); return ec_fold(s + c, p); }
template <int T>
__global__ void k_bsplit(struct bsplit a, struct acc y, int logk, size_t h)
{
    const size_t m = (size_t)1 << logk;
    size_t j = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; j < h; j += stride) {
        uint64_t raw[T][2];
        for (int s = 0; s < T; s++) for (int e = 0; e < 2; e++) { size_t i = j + e * h + s * m; raw[s][e] = i < y.n ? acc_get(y, i) : 0; }
        for (int k = 0; k < a.nk; k++) {
            const ec_mod md = a.m[k]; const uint64_t p = md.pu;
            uint64_t c[T][2];
            for (int s = 0; s < T; s++) for (int e = 0; e < 2; e++) c[s][e] = ec_canon64(raw[s][e], p, md.mu);
            uint64_t v = a.hi[k] ? btw_at(a.wm[k], j, md) : 0;
            for (int r = 0; r < T; r++) {
                uint64_t z[2];
                for (int e = 0; e < 2; e++) {
                    if (T == 1) { z[e] = c[0][e]; continue; }
                    uint64_t a0 = c[0][e], a1 = c[T > 1 ? 1 : 0][e], a2 = c[T > 2 ? 2 : 0][e];
                    if (r == 0) { z[e] = add3u(a0, a1, a2, p); continue; }
                    uint64_t wa = r == 1 ? a.w3[k] : a.w3s[k], wb = r == 1 ? a.w3s[k] : a.w3[k];
                    uint64_t t = add3u(a0, ec_mmu(a1, wa, md), ec_mmu(a2, wb, md), p);
                    uint64_t w = btw_at(a.wn[k], j + e * h, md); if (r == 2) w = ec_mmu(w, w, md);
                    z[e] = ec_mmu(t, w, md);
                }
                if (a.lo[k]) a.lo[k][r * h + j] = ec_fold(z[0] + z[1], p);
                if (a.hi[k]) a.hi[k][r * h + j] = ec_mmu(z[0] + p - z[1], v, md);
            }
        }
    }
}
/* B4's pointwise product: x[r m + t] *= t < h ? lo[r h + t] : hi[r h + t - h] (hi: APU 3's plane, a peer read) */
__global__ void k_b4_pw(uint64_t *x, const uint64_t *lo, const uint64_t *hi, int logk, size_t n, ec_mod md)
{
    const size_t m = (size_t)1 << logk, h = m / 2;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) { size_t r = i >> logk, t = i & (m - 1); uint64_t y = t < h ? lo[r * h + t] : hi[r * h + t - h]; x[i] = ec_mmu(x[i], y, md); }
}
/* the planes of one B-form product on APU d: first fit into pool 0, pool 1, then (forced forms only) the extra buffer */
static uint64_t *g_bx[NR]; static size_t g_bx_cap[NR];
static void b_acct(int ndev, size_t b[][MEM_DEV_NCAT]) { for (int d = 0; d < ndev && d < NR; d++) b[d][MEM_DEV_PLANES] += g_bx_cap[d] * 8; }   /* M9: the extra planes count as planes */
static void b_extra_release(void)
{
    int cur; HIP_CHECK(hipGetDevice(&cur));
    for (int d = 0; d < NR; d++) if (g_bx[d]) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(g_bx[d])); g_bx[d] = 0; g_bx_cap[d] = 0; }
    HIP_CHECK(hipSetDevice(cur));
}
/* place k planes of sz[i] limbs; returns the extra limbs needed beyond the pools (0: all in the pools); with out != 0 and
 * the extra buffer grown to it, the pointers */
static size_t g_plan_pool[2];                                 /* Phase 13d L (mn_plan.c, MN_PLAN_ONLY): the plane pools' bytes per APU as rns_init would make them -- no pools exist in the plan */
static size_t b_place(int d, int k, const size_t *sz, uint64_t **out)
{
    size_t cap[2] = { (g_plan_pool[0] ? g_plan_pool[0] : rns_dpool_cap(d, 0)) / 8, (g_plan_pool[1] ? g_plan_pool[1] : rns_dpool_cap(d, 1)) / 8 }, used[2] = { 0, 0 }, ex = 0;
    for (int i = 0; i < k; i++) {
        int w = -1; for (int r = 0; r < 2; r++) if (cap[r] - used[r] >= sz[i] && cap[r] >= used[r]) { w = r; break; }
        if (w >= 0) { if (out) out[i] = (uint64_t *)rns_dpool(d, w, cap[w] * 8) + used[w]; used[w] += sz[i]; }
        else { if (out) out[i] = g_bx[d] + ex; ex += sz[i]; }
    }
    return ex;
}
/* the planes' sizes of a product of length n on APU d (form f); returns the count */
static int b_planes(int f, int d, size_t n, size_t *sz, int np)   /* np: the product's primes (Phase 15 NP) */
{
    if (f == STRAT_B) { if (d >= np) return 0; sz[0] = n; sz[1] = n; return 2; }
    if (d < 3) { sz[0] = n; sz[1] = n / 2; return 2; }
    sz[0] = sz[1] = sz[2] = n / 2; return 3;
}
/* the form this product runs in (C, B or B4) */
static int b_choose(struct acc A, struct acc B, struct acc Cw, size_t nc, int sa, int sb, int np)
{
    int s = strat_get();
    if (s == STRAT_C || cache_slots() || A.flat || B.flat || Cw.flat || !Cw.owner || Cw.lo || sa >= 0 || sb >= 0) return STRAT_C;
    if (Cw.owner->qc % 4096) return STRAT_C;
    int f = s == STRAT_AUTO ? g_strat_form : s;
    if (f == STRAT_B4 && np != 3) f = STRAT_B;
    if (np > NR) return STRAT_C;
    if (s == STRAT_AUTO) {
        int T, lk; size_t n = b_len(nc, &T, &lk), sz[3];
        for (int d = 0; d < NR; d++) if (b_place(d, b_planes(f, d, n, sz, np), sz, 0)) return STRAT_C;
    }
    return f;
}
static int b_core(int f, struct acc A, struct acc B, struct acc Cw, size_t nc, const int np)   /* 0: the extra planes could not be allocated (the product runs C); np: the product's primes */
{
    double t0 = mem_now();
    if (A.n < B.n) { struct acc t = A; A = B; B = t; }          /* X = the longer operand (whole on APU p), Y = the shorter */
    int T, logk; size_t n = b_len(nc, &T, &logk), m = (size_t)1 << logk, h = m / 2;
    dbig *Cd = Cw.owner;
    /* the extra plane memory (forced forms), grown before the products' parallel region, the four APUs in parallel */
    {
        size_t ex[NR]; int grow = 0, bad = 0, cur; HIP_CHECK(hipGetDevice(&cur));
        for (int d = 0; d < NR; d++) { size_t sz[3]; ex[d] = b_place(d, b_planes(f, d, n, sz, np), sz, 0); if (ex[d] > g_bx_cap[d]) grow = 1; }
        if (grow) {
            double ta = mem_now();
#pragma omp parallel for num_threads(NR) reduction(+:bad)
            for (int d = 0; d < NR; d++) {
                if (ex[d] <= g_bx_cap[d]) continue;
                HIP_CHECK(hipSetDevice(d)); if (g_bx[d]) HIP_CHECK(hipFree(g_bx[d])); g_bx[d] = 0; g_bx_cap[d] = 0;
                if (hipMalloc(&g_bx[d], ex[d] * 8) != hipSuccess) { (void)hipGetLastError(); g_bx[d] = 0; bad++; }
                else g_bx_cap[d] = ex[d];
            }
            HIP_CHECK(hipSetDevice(cur));
            mem_acct_register(b_acct); g_bst.t_alloc += mem_now() - ta;
            size_t tot = 0; for (int e = 0; e < NR; e++) if (g_bx_cap[e] > tot) tot = g_bx_cap[e]; g_bst.extra_bytes = tot * 8;
            if (getenv("RNS_VERBOSE") || bad) printf("rns_dist %s: extra planes %.2f GiB per APU%s in %.2f s (the pools hold %.2f + %.2f GiB)\n", strat_name(f), tot * 8 / 1073741824.0, bad ? " NOT AVAILABLE: this product runs C" : "", mem_now() - ta, rns_dpool_cap(0, 0) / 1073741824.0, rns_dpool_cap(0, 1) / 1073741824.0);
            if (bad) return 0;
        }
    }
    uint64_t *pl[NR][3];
    for (int d = 0; d < NR; d++) { size_t sz[3]; b_place(d, b_planes(f, d, n, sz, np), sz, pl[d]); }
    /* the CRT stripes: rows | qc; G stripes over [0, nc); APU d stripes [G d/4, G (d+1)/4) */
    size_t rows = 16384; while (Cd->qc % rows) rows /= 2;
    size_t G = (nc + rows - 1) / rows, Ccol = (G + 3) / 4, R = 4 * rows;
    static uint64_t *spall, *sp4[4]; static size_t spcap;
    if (spcap < Ccol) {
        if (spall) { HIP_CHECK(hipHostFree(spall)); for (int r = 0; r < 4; r++) HIP_CHECK(hipHostFree(sp4[r])); }
        spcap = Ccol + 1024; HIP_CHECK(hipHostMalloc((void **)&spall, spcap * 16 * 8, 0));
        for (int r = 0; r < 4; r++) HIP_CHECK(hipHostMalloc((void **)&sp4[r], spcap * 4 * 8, 0));
    }
    memset(spall, 0, G * 4 * 8);
    struct gconst gc = rns_gconst(); gc.np = np;               /* NP: the kernel's prime count is the product's */
    double tl[NR], tf[NR], tc[NR];
#pragma omp parallel num_threads(NR)
    {
        int d = omp_get_thread_num(); struct rank_state *v = &RS[d];
        HIP_CHECK(hipSetDevice(d));
        double x0 = mem_now(), x1 = x0, x2 = x0;
        uint64_t *X = pl[d][0], *Y = pl[d][1];
        if (f == STRAT_B) {
            if (d < np) {
                ec_mod md = ec_mod_get(d);
                k_bload<<<nblk(n), 256, 0, v->s>>>(X, A, n, md); k_bload<<<nblk(n), 256, 0, v->s>>>(Y, B, n, md);
                HIP_CHECK(hipStreamSynchronize(v->s)); x1 = mem_now();
                if (T == 3) { ntt_fwd3(v->ctx[d], X, logk, 1, v->s); ntt_fwd3(v->ctx[d], Y, logk, 1, v->s); ntt_inv3_pw(v->ctx[d], X, Y, logk, 1, v->s); }
                else { ntt_fwd(v->ctx[d], X, logk, 1, v->s); ntt_fwd(v->ctx[d], Y, logk, 1, v->s); ntt_inv_pw(v->ctx[d], X, Y, logk, 1, v->s); }
                HIP_CHECK(hipStreamSynchronize(v->s));
            }
            x2 = mem_now();
        } else {
            struct bsplit sp; memset(&sp, 0, sizeof sp);
            int ks[3], nk = 0; if (d < 3) ks[nk++] = d; else for (int k = 0; k < 3; k++) ks[nk++] = k;
            sp.nk = nk;
            for (int i = 0; i < nk; i++) {
                int p = ks[i]; sp.m[i] = ec_mod_get(p);
                if (d < 3) sp.lo[i] = Y; else sp.hi[i] = pl[d][i];
                if (d == 3) sp.wm[i] = b_tables(d, p, ec_root(p, logk), h);
                if (T == 3) { uint64_t wn = ec_root3(p, logk); sp.wn[i] = b_tables(d, p, wn, m); sp.w3[i] = ec_powmod(wn, m, ec_P[p]); sp.w3s[i] = ec_mulmod_ref(sp.w3[i], sp.w3[i], ec_P[p]); }
            }
            if (d < 3) k_bload<<<nblk(n), 256, 0, v->s>>>(X, A, n, sp.m[0]);
            if (T == 3) k_bsplit<3><<<nblk(h), 256, 0, v->s>>>(sp, B, logk, h); else k_bsplit<1><<<nblk(h), 256, 0, v->s>>>(sp, B, logk, h);
            HIP_CHECK(hipStreamSynchronize(v->s)); x1 = mem_now();
            if (d < 3) {
                if (T == 3) ntt_fwd3(v->ctx[d], X, logk, 1, v->s); else ntt_fwd(v->ctx[d], X, logk, 1, v->s);
                ntt_fwd(v->ctx[d], Y, logk - 1, T, v->s);
            } else for (int k = 0; k < 3; k++) ntt_fwd(v->ctx[k], pl[d][k], logk - 1, T, v->s);
            HIP_CHECK(hipStreamSynchronize(v->s));
#pragma omp barrier
            if (d < 3) {
                k_b4_pw<<<nblk(n), 256, 0, v->s>>>(X, Y, pl[3][d], logk, n, sp.m[0]);
                if (T == 3) ntt_inv3(v->ctx[d], X, logk, 1, v->s); else ntt_inv(v->ctx[d], X, logk, 1, v->s);
                HIP_CHECK(hipStreamSynchronize(v->s));
            }
            x2 = mem_now();
        }
#pragma omp barrier
        /* the CRT of stripes [g0, g1) into the result's quarters */
        const uint64_t *P0 = pl[0][0], *P1 = pl[1][0], *P2 = pl[2][0], *P3 = np > 3 ? pl[3][0] : pl[0][0];
        size_t g0 = G * d / 4, g1 = G * (d + 1) / 4, a = g0 * rows, b = g1 * rows < nc ? g1 * rows : nc;
        struct bdesc hd[16]; size_t c0s[16], S_[16]; int ns = 0;
        for (size_t c0 = a; c0 < b;) {
            size_t j = c0 / Cd->qc, qe = (j + 1) * Cd->qc, e = qe < b ? qe : b;
            if (e - c0 > ((size_t)1 << 31)) e = c0 + ((size_t)1 << 31);
            size_t len = e - c0, full = len / rows * rows;
            if (full) { memset(&hd[ns], 0, sizeof hd[ns]); hd[ns].c = Cd->q[j] + (c0 - j * Cd->qc); hd[ns].na = (uint32_t)full; c0s[ns] = c0; S_[ns] = full / rows; ns++; }
            if (len > full) { memset(&hd[ns], 0, sizeof hd[ns]); hd[ns].c = Cd->q[j] + (c0 + full - j * Cd->qc); hd[ns].na = (uint32_t)(len - full); c0s[ns] = c0 + full; S_[ns] = 1; ns++; }
            c0 = e;
            if (ns > 14) { ec_fatal(EC_RC_FATAL, "b_core: too many CRT segments\n"); }
        }
        double x3 = mem_now();
        if (ns) {
            struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, d, sizeof hd);
            HIP_CHECK(hipMemcpyAsync(dd, hd, ns * sizeof hd[0], hipMemcpyHostToDevice, v->s));
            for (int i = 0; i < ns; i++) {
                size_t c0 = c0s[i];
                k_crt_batch<<<(unsigned)S_[i], CRT_THREADS, 0, v->s>>>(P0 + c0, P1 + c0, P2 + c0, P3 + c0, dd + i, 0, (int)S_[i], 0, gc, spall + (c0 / rows) * 4, bi_decimal);
            }
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
        tl[d] = x1 - x0; tf[d] = x2 - x1; tc[d] = mem_now() - x3;
    }
    /* the spills: stripe g = 4 j + r -> sp4[r][j] (the four-step's sparse layout), then one chunked-carry add */
    double tsp = mem_now();
    for (size_t g = 0; g < 4 * Ccol; g++) { uint64_t *o = sp4[g & 3] + (g >> 2) * 4; if (g < G) memcpy(o, spall + g * 4, 32); else memset(o, 0, 32); }
    Cd->n = nc;
    const uint64_t *spp[4] = { sp4[0], sp4[1], sp4[2], sp4[3] };
    db_add_spills(Cd, Cd, spp, R, rows, Ccol, nc);
    if (Cd->n > nc) { ec_fatal(EC_RC_FATAL, "b_core: carry out of the product\n"); }
    double ml = 0, mf = 0, mc = 0; for (int r = 0; r < NR; r++) { if (tl[r] > ml) ml = tl[r]; if (tf[r] > mf) mf = tf[r]; if (tc[r] > mc) mc = tc[r]; }
    rns_dist_st.t_merge += mem_now() - tsp; rns_dist_st.n++; rns_dist_st.t_total += mem_now() - t0;
    rns_dist_st.t_load += ml; rns_dist_st.t_ntt += mf; rns_dist_st.t_crt += mc;
    g_bst.n[f] += 1; g_bst.t[f] += mem_now() - t0;
    if (getenv("RNS_VERBOSE")) printf("dist %s %s2^%d (%zu limbs = %zu x %zu%s): load %.3f ntt %.3f crt %.3f spills %.3f total %.3f s\n", strat_name(f), T == 3 ? "3*" : "", logk, nc, A.n, B.n, ec_np_auto ? (np == 4 ? ", 4 primes" : ", 3 primes") : "", ml, mf, mc, mem_now() - tsp, mem_now() - t0);
    return 1;
}
/* RNS_STRATEGY_CHECK=1 (test): every B-form product formed again by C into a temporary and compared (abort on a difference) */
static int b_check_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RNS_STRATEGY_CHECK"); v = e ? atoi(e) : 0; } return v; }
static void dist_core(struct acc A, struct acc B, struct acc Cw, size_t nc, int sa, int sb);
static void b_check(int f, struct acc A, struct acc B, struct acc Cw, size_t nc)
{
    dbig T; db_init(&T); db_reserve(&T, nc + 8);
    int s = g_strat; g_strat = STRAT_C; dist_core(A, B, acc_db(&T, 0, nc), nc, -1, -1); g_strat = s;
    T.n = nc;
    bigint x, y; bi_init(&x); bi_init(&y); db_to_bi(&x, Cw.owner); db_to_bi(&y, &T); bi_norm(&x); bi_norm(&y);
    size_t k = 0, m = x.n < y.n ? x.n : y.n; while (k < m && x.l[k] == y.l[k]) k++;
    int T3; int lk; size_t n = b_len(nc, &T3, &lk);
    if (x.n != y.n || k < m) { ec_fatal(7, "RNS_STRATEGY_CHECK: %s differs from C: %zu x %zu limbs (views at %zu, %zu), nc %zu, n %s2^%d: first limb %zu of %zu / %zu\n", strat_name(f), A.n, B.n, A.lo, B.lo, nc, T3 == 3 ? "3*" : "", lk, k, x.n, y.n); }
    static size_t nchk; if (++nchk % 16 == 1 && getenv("RNS_VERBOSE")) printf("RNS_STRATEGY_CHECK: %zu products identical\n", nchk);
    bi_free(&x); bi_free(&y); db_free(&T);
}
/* the core: C = A B, na + nb limbs of result through accessors; nc limbs written.  sa, sb: the cache slots for A and B
 * (-1: not cached; a hit anywhere in the cache is taken regardless) */
static void dist_core(struct acc A, struct acc B, struct acc Cw, size_t nc, int sa, int sb)
{
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    if (logn < 20) logn = 20;                                  /* R, C >= 2^10 */
    int r3 = dist_r3() && logn >= dist_logn_max() - 1 && nc <= ((size_t)3 << (logn - 2));   /* C5: 3 2^(logn-2) points instead of 2^logn (the top three sizes: 3 2^28 .. 3 2^30 at the 2^31 cap) */
    if (r3) logn--;                                            /* the 2^k length whose pool this replaces: n = 3 2^(logn-1) */
    if (logn > dist_logn_max()) { ec_fatal(EC_RC_FATAL, "dist_core: %zu limbs > 2^%d points\n", nc, dist_logn_max()); }
    const int np = ec_np_prod(ec_np_terms(nc, A.n, B.n), bi_decimal, "dist_core");    /* Phase 13a P3: three primes -- decimal limbs, within the bound; Phase 15 NP: ECALC_NP=auto -- four over the bound; Phase 15 MPB: the terms min(A.n, B.n) under ECALC_NP_AUTO_MIN=1 (modarith.h), else nc */
    int logR = r3 ? (logn - 1) / 2 : logn / 2 + dist_logr_delta(), logk, logC;
    if (!r3) { if (logR < 10) logR = 10; if (logR > logn - 10) logR = logn - 10; }   /* (A6: DIST_LOGR_DELTA; R, C >= 2^10) */
    logk = logn - 1 - logR; logC = r3 ? 0 : logn - logR;
    size_t n = r3 ? (size_t)3 << (logn - 1) : (size_t)1 << logn, R = (size_t)1 << logR, C = r3 ? (size_t)3 << logk : (size_t)1 << logC, rows = R / NR, q = n / NR;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    { int f = b_choose(A, B, Cw, nc, sa, sb, np); if (f != STRAT_C && b_core(f, A, B, Cw, nc, np)) { if (b_check_on()) b_check(f, A, B, Cw, nc); return; } }   /* Phase 13b B: RNS_STRATEGY */
    /* A1: the cache slots -- hits anywhere, misses filled in the designated slots (never the slot the other operand hits in) */
    int ha = r3 ? -1 : cache_find(&A, q, np), hb = r3 ? -1 : cache_find(&B, q, np);
    if (r3 || A.flat) sa = -1; if (r3 || B.flat) sb = -1;
    cache_plan(&sa, &sb, ha, hb, A.q[0], A.lo, A.n, B.q[0], B.lo, B.n, q, 0, np, 0);
    struct gconst gc = rns_gconst(); gc.np = np;               /* NP: the CRT of this product's primes */
    double tl[NR], tf[NR], tc[NR];
#pragma omp parallel num_threads(NR)
    {
        int r = omp_get_thread_num(); struct rank_state *v = &RS[r];
        HIP_CHECK(hipSetDevice(r));
        /* planes: xa[4] in pool 0 (4 q = the standard 2^pool_log limbs at n = 2^31); xb | sbuf | rbuf in pool 1 (3 q);
         * the transpose scratch reuses the slab buffers after the last inverse.  r3: xb and the slabs from the block pool */
        int p1_pool = r3 && rns_dpool_cap(r, 1) < ((size_t)3 * q + 16) * 8;   /* Phase 11 B3 (agent P): pool 1 holds 3 q + 16 at the 3 2^k sizes when sized for it at init; the block pool only when it was not (DIST_R3=1 alone) */
        uint64_t *pl = (uint64_t *)rns_dpool(r, 0, (size_t)np * q * 8), *p1 = p1_pool ? db_pool_alloc(r, ((size_t)3 * q + 16) * 8) : (uint64_t *)rns_dpool(r, 1, ((size_t)3 * q + (r3 ? 16 : 0)) * 8);
        uint64_t *xa[EC_NP] = { 0 }, *xb = p1, *sl = p1 + q + (r3 ? 16 : 0), *xt = sl;
        for (int p = 0; p < np; p++) xa[p] = pl + (size_t)p * q;
        uint64_t *ca = sa >= 0 ? g_cache.s[sa].pl[r] : 0, *cb = sb >= 0 ? g_cache.s[sb].pl[r] : 0;   /* A1: the cache planes of A and B on this rank (EC_NP x q limbs) */
        struct ctx3 c3[EC_NP];
        for (int p = 0; p < np; p++) {
            if (r3) { plan3_get(&P3[r][p], p, logR, logk); struct ctx3 c = { v->cm, v->ctx[p], p, logR, logk, rows, C / NR, sl, sl + q, &P3[r][p] }; c3[p] = c; continue; }
            if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC || v->plan[p].cm != v->cm || v->plan[p].sl != sl) {
                if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                dist_plan_create_shared(&v->plan[p].pl, v->cm, v->ctx[p], p, logR, logC, sl, sl + q);
                v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1; v->plan[p].cm = v->cm; v->plan[p].sl = sl;
            }
        }
        double s0 = mem_now(), lg = 0, lf = 0;
        for (int p = 0; p < np; p++) {
            ec_mod m = ec_mod_get(p);
            double g0 = mem_now();
            uint64_t *yb = cb ? cb + (size_t)p * q : xb;        /* B's transform: the cache plane (hit: already there) or xb */
            if (ha < 0) k_gather<<<nblk(q), 256, 0, v->s>>>(xa[p], A, R, rows, (size_t)r * rows, C, m);
            else HIP_CHECK(hipMemcpyAsync(xa[p], ca + (size_t)p * q, q * 8, hipMemcpyDeviceToDevice, v->s));   /* A hit: the product forms over a copy */
            if (hb < 0) k_gather<<<nblk(q), 256, 0, v->s>>>(yb, B, R, rows, (size_t)r * rows, C, m);
            HIP_CHECK(hipStreamSynchronize(v->s)); lg += mem_now() - g0;
            if (r3) { dist3_fwd(&c3[p], xa[p], v->s); dist3_fwd(&c3[p], xb, v->s); ntt_pw(v->ctx[p], xa[p], xb, q, v->s); dist3_inv(&c3[p], xa[p], v->s); }
            else {
            if (ha < 0) { dist_fwd(&v->plan[p].pl, xa[p], v->s); if (ca) HIP_CHECK(hipMemcpyAsync(ca + (size_t)p * q, xa[p], q * 8, hipMemcpyDeviceToDevice, v->s)); }   /* A miss: fill its slot */
            if (hb < 0) dist_fwd(&v->plan[p].pl, yb, v->s);
            if (dist_pw_fused()) dist_inv_pw(&v->plan[p].pl, xa[p], yb, v->s);   /* A6: the pointwise fused into the column inverse */
            else { dist_pw(&v->plan[p].pl, xa[p], yb, v->s); dist_inv(&v->plan[p].pl, xa[p], v->s); }
            }
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
        double s2 = mem_now(); lf = s2 - s0 - lg;
        /* CRT over this rank's runs: transpose each plane in place (via xt), one stripe per run into xb */
        for (int p = 0; p < np; p++) {
            dim3 grid((unsigned)((C + 31) / 32), (unsigned)((rows + 31) / 32)), blk(32, 8);
            k_transpose<<<grid, blk, 0, v->s>>>(xa[p], xt, rows, C);
            HIP_CHECK(hipMemcpyAsync(xa[p], xt, q * 8, hipMemcpyDeviceToDevice, v->s));
        }
        struct bdesc hd = { 0, 0, 0, xb, (uint32_t)q, 0, 0 };
        struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, r, sizeof hd);
        HIP_CHECK(hipMemcpyAsync(dd, &hd, sizeof hd, hipMemcpyHostToDevice, v->s));
        if (v->spill_cap < C) { if (v->spill) HIP_CHECK(hipFree(v->spill)); v->spill_cap = C + 16; HIP_CHECK(hipMalloc(&v->spill, v->spill_cap * 4 * 8)); }
        k_crt_batch<<<(unsigned)C, CRT_THREADS, 0, v->s>>>(xa[0], xa[1], xa[2], xa[3], dd, 0, (int)C, q, gc, v->spill, bi_decimal);
        k_scatter_runs<<<nblk(q), 256, 0, v->s>>>(xb, Cw, R, rows, (size_t)r * rows, C);
        HIP_CHECK(hipStreamSynchronize(v->s));
        tl[r] = lg; tf[r] = lf; tc[r] = mem_now() - s2;
        if (p1_pool) db_pool_free(r, p1);
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
        if (Cd->n > nc) { ec_fatal(EC_RC_FATAL, "dist: carry out of the product\n"); }
    }
    rns_dist_st.t_merge += mem_now() - tsp;
    rns_dist_st.n++; rns_dist_st.t_total += mem_now() - t0; g_bst.n[0]++; g_bst.t[0] += mem_now() - t0;
    double ml = 0, mf = 0, mc = 0; for (int r = 0; r < NR; r++) { if (tl[r] > ml) ml = tl[r]; if (tf[r] > mf) mf = tf[r]; if (tc[r] > mc) mc = tc[r]; }
    rns_dist_st.t_load += ml; rns_dist_st.t_ntt += mf; rns_dist_st.t_crt += mc;
    if (getenv("RNS_VERBOSE")) printf("dist %s2^%d = 2^%d x %s2^%d (%zu limbs%s): load %.3f ntt %.3f crt %.3f spills %.3f total %.3f s%s%s\n", r3 ? "3*" : "", r3 ? logn - 1 : logn, logR, r3 ? "3*" : "", r3 ? logk : logC, nc, ec_np_auto ? (np == 4 ? ", 4 primes" : ", 3 primes") : "", ml, mf, mc, mem_now() - tsp, mem_now() - t0, ha >= 0 ? " [A hit]" : sa >= 0 ? " [A cached]" : "", hb >= 0 ? " [B hit]" : sb >= 0 ? " [B cached]" : "");
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
    dist_core(acc_flat(a, na), acc_flat(b, nb), acc_flat(c, nc), nc, -1, -1);
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
        dist_core(acc_flat(a, na), acc_db(B, 0, nb), acc_db(Cd, 0, nc), nc, -1, -1);
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
static void mul_grid(dbig *Cd, const dbig *A, const dbig *B, size_t lowcut, size_t w);
void rns_mul_low_db(dbig *Cd, const dbig *A, const dbig *B, size_t w)
{
    dbig a = db_view(A, 0, A->n < w ? A->n : w), b = db_view(B, 0, B->n < w ? B->n : w);
    db_norm(&a); db_norm(&b);
    if (!a.n || !b.n || !w) { Cd->n = 0; return; }
    mul_grid(Cd, &a, &b, 0, w);
    if (Cd->n > w) { Cd->n = w; db_norm(Cd); }
}
/* plane points for a product of nc limbs (dist_core rounds to 2^logn, at least 2^20) */
static size_t plane_pts(size_t nc, int r3)   /* r3: the single-node tier's 3 2^k planes (the mn tier's are 2^k) */
{
    size_t n = (size_t)1 << 20; while (n < nc) { if (r3 && n >= ((size_t)1 << (dist_logn_max() - 2)) && (n / 2 * 3) >= nc) return n / 2 * 3; n <<= 1; }   /* C5: 3 2^(k-1) between 2^k and 2^(k+1) */
    return n;
}
/* piece counts (ka, kb) for a product too long for one plane: every piece product ceil(na/ka) + ceil(nb/kb)
 * must fit 2^31 points; choose the grid with the fewest plane points in total (then the fewest products).
 * Halving the longer operand alone -- the first version -- gave 8 planes of 2^31 for the decimal top product
 * (2.22e9 x 2.22e9 limbs: halves of 1.11e9 still exceed a plane together); 2 x 3 pieces give 6 (RESULTS.md 66) */
static void split_grid_cap(size_t na, size_t nb, size_t cap, size_t minpts, int r3, int *ka, int *kb)   /* cap, minpts: plane points (the mn tier's differ); r3: 3 2^k planes allowed (Phase 11 P) */
{
    size_t best = 0; *ka = *kb = 0;
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j;
        if (pa + pb > cap) continue;
        size_t pts = plane_pts(pa + pb, r3); if (pts < minpts) pts = minpts;
        size_t cost = (size_t)i * j * pts * ((pts & (pts - 1)) ? 21 : 20);   /* Phase 11 B3 (agent P): a 3 2^k plane costs ~5 % more per point (A-grid C5: 1.57x for 1.5x the points) */
        if (!*ka || cost < best || (cost == best && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
    if (!*ka) { ec_fatal(EC_RC_FATAL, "split_grid: %zu x %zu limbs\n", na, nb); }
}
/* Phase 13b B: under RNS_STRATEGY=auto the grid knows which pieces run in the B form (RNS_STRATEGY_GRID, default 1 under auto):
 * a piece that fits the B form's planes costs its B-length points x 0.70 (the library's B against C per product, measured
 * 0.66-0.74 at 2^26..2^31, results/B13b.md), one that does not its C points x 1; 3 2^k lengths x 1.05 as above.  The digits do
 * not depend on the grid.  Off (or any other strategy): C's grid as before. */
static int b_fits(size_t nc)
{
    int np = ec_np_for(nc);                                   /* Phase 15 NP: the piece's primes */
    int f = g_strat_form; if (f == STRAT_B4 && np != 3) f = STRAT_B; if (np > NR) return 0;
    int T, lk; size_t n = b_len(nc, &T, &lk), sz[3];
    for (int d = 0; d < NR; d++) if (b_place(d, b_planes(f, d, n, sz, np), sz, 0)) return 0;
    return 1;
}
static int b_grid_on(void)
{
    static int v = -1; if (v < 0) { const char *e = getenv("RNS_STRATEGY_GRID"); v = e ? atoi(e) != 0 : 1; }
    return v && strat_get() == STRAT_AUTO && !cache_slots();
}
/* Phase 14 V2: RNS_AUTO_PIECE_COST=1 (the default since Phase 15, D1) -- auto's grid also prices what each extra piece costs in the pipeline
 * (results/D213d.md, fit_pipe on G13d's logs: a piece of a grid adds 0.075 s per 2^31 B points of the piece + 0.079 s per
 * 2^31 limbs of the whole product, its accumulation pass over C).  In split_grid's units (C plane points; the pipeline's C
 * product costs ~0.86 s per 2^31 points, mn_model.t_prod x PIPE_ONE) that is + 0.087 x the piece's B points + 0.092 x nc per
 * piece of a grid of more than one piece.  Without it auto forms up to 7 x 10 grids of small B pieces at >= 1e11 on one node. */
static int auto_piece_cost(void)
{
    static int v = -1; if (v < 0) { const char *e = getenv("RNS_AUTO_PIECE_COST"); v = e ? atoi(e) != 0 : 1; }   /* default 1 since Phase 15 (the user's decision D1; results/C215.md) */
    return v;
}
static void split_grid(size_t na, size_t nb, int *ka, int *kb)
{
    if (!b_grid_on()) { split_grid_cap(na, nb, dist_cap(), 0, dist_r3(), ka, kb); return; }
    size_t cap = dist_cap(); double best = 0; *ka = *kb = 0; const int pc = auto_piece_cost();
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j;
        if (pa + pb > cap) continue;
        double cost;
        const int bf = b_fits(pa + pb);
        if (bf) { int T, lk; size_t n = b_len(pa + pb, &T, &lk); cost = (double)i * j * n * (T == 3 ? 1.05 : 1.0) * 0.70; }
        else { size_t pts = plane_pts(pa + pb, dist_r3()); cost = (double)i * j * pts * ((pts & (pts - 1)) ? 1.05 : 1.0); }
        if (pc && i * j > 1) { int T, lk; size_t n = bf ? b_len(pa + pb, &T, &lk) : 0; cost += (double)i * j * (0.087 * n + 0.092 * (na + nb)); }   /* Phase 14 V2: the per-piece cost */
        if (!*ka || cost < best * 0.999 || (cost <= best * 1.001 && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
    if (!*ka) { ec_fatal(EC_RC_FATAL, "split_grid: %zu x %zu limbs\n", na, nb); }
}
/* device bigints: C = A B (nc limbs) in place in C's quarters; up to 2^31 points, larger products as a grid of
 * piece products (views, no copies): the first straight into C, the others through one temporary and a
 * shifted in-place add */
/* Phase 13d L: the grid's two cuts as one rule -- mul_grid, mn_grid and the plan printer (rns_dist_db_plan / rns_dist_mn_plan,
 * mn_plan.c) all ask it: a piece at (oa, ob) of la x lb limbs is skipped when nothing of it lies below w or all of it at or below
 * the low cut */
static inline int grid_piece_skipped(size_t oa, size_t ob, size_t la, size_t lb, size_t lowcut, size_t w) { return oa + ob >= w || oa + ob + la + lb <= lowcut; }
/* Phase 13d L: mul_grid's choice between one plane and the grid, extracted unchanged (the plan printer calls it too): 1 = one
 * plane (ka = kb = 1), 0 = the grid split_grid forms (ka x kb) */
static int db_grid_shape(size_t na, size_t nb, int *ka, int *kb)
{
    size_t nc = na + nb; int one = nc <= dist_cap(); *ka = *kb = 1;
    if (one && (nc > ((size_t)1 << dist_logn_max()) || (b_grid_on() && !b_fits(nc)))) { split_grid(na, nb, ka, kb); one = *ka * *kb == 1; }   /* (Phase 13b B: under auto also a single plane that does not fit the B form) */   /* Phase 11 B3 (agent P): the 3 2^30 plane only when no grid of smaller planes is cheaper (A-grid C5: level 25's 2.19e9 x 2.7e7 is 5 x 1 pieces of 2^29) */
    if (!one) split_grid(na, nb, ka, kb);
    return one;
}
void rns_mul_dist_db(dbig *Cd, const dbig *A, const dbig *B) { mul_grid(Cd, A, B, 0, (size_t)-1); }
/* Phase 10 A5/B3: the product of which only C >> cut is used: the grid with the pieces ending at or below the cut skipped (a
 * product that fits one plane is formed whole, as before) */
void rns_mul_high_db(dbig *Cd, const dbig *A, const dbig *B, size_t cut) { mul_grid(Cd, A, B, cut, (size_t)-1); }
/* Phase 15 R4 (the reciprocal's middle product, results/R415.md): both cuts, the result mod B^w -- the formed pieces are added whole, a
 * skipped piece at oa + ob >= w is a multiple of B^w, so the low w limbs are those of A B less the low cut's pieces; truncated to w */
void rns_mul_band_db(dbig *Cd, const dbig *A, const dbig *B, size_t lowcut, size_t w) { mul_grid(Cd, A, B, lowcut, w); if (Cd->n > w) { Cd->n = w; db_norm(Cd); } }
/* the grid; only the pieces whose limbs start below w are formed (w = -1: all) and, Phase 10 A5, only those whose limbs
 * do not all lie at or below lowcut (0: all; the A_h mu product of the division, results/A-div.md B3: each skipped piece
 * is < B^cut, X is low by at most their number + 1, absorbed by the up-corrections).  A1: the pieces' transforms cached
 * over the grid's loop (dist_core; the slots planned here) */
static void mul_grid(dbig *Cd, const dbig *A, const dbig *B, size_t lowcut, size_t w)
{
    size_t na = A->n, nb = B->n, nc = na + nb;
    int verbose = getenv("RNS_VERBOSE") != 0, pin = g_cache.pin_next, N = 0, fs[DIST_CACHE_MAX], nf = 0;
    if (!na || !nb) { Cd->n = 0; return; }
    db_reserve(Cd, nc + 8);
    g_cache_mn = 0;
    int ka, kb, one = db_grid_shape(na, nb, &ka, &kb);        /* Phase 13d L: the decision extracted (unchanged) */
    if (cache_slots() && (!one || pin)) { N = cache_avail(); for (int i = 0; i < N; i++) if (!g_cache.s[i].pinned) fs[nf++] = i; }   /* the free (unpinned) slots */
    if (one) {
        struct db_stats s0 = db_st; double t0 = mem_now();
        dist_core(acc_db(A, 0, na), acc_db(B, 0, nb), acc_db(Cd, 0, nc), nc, -1, pin && nf ? fs[0] : -1);   /* one plane: B kept only for a hold */
        if (pin && nf) { g_cache.s[fs[0]].pinned = 1; g_cache.pin_next = 0; }
        Cd->n = nc; db_norm(Cd);
        if (verbose) printf("   dist_db %zu limbs: %.3f s (dbig: shift %.3f addsub %.3f maxidx %.3f reserve %.3f)\n", nc, mem_now() - t0,
                            db_st.t_shift - s0.t_shift, db_st.t_addsub - s0.t_addsub, db_st.t_maxidx - s0.t_maxidx, db_st.t_reserve - s0.t_reserve);
        return;
    }
    size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb;
    /* the free slots: for a hold, B's pieces get distinct slots (pinned afterwards), A the rest; otherwise A's pieces (reused
     * across every j) get up to nf - 1 and B's piece of the current j cycles through the rest */
    int nB = pin ? (kb < nf ? kb : nf) : 0, nA = pin ? nf - nB : (ka < nf - 1 ? ka : nf - 1); if (nA < 0) nA = 0;
    if (!pin) nB = nf - nA;
    dbig t; db_init(&t); int first = 1, formed = 0, skipped = 0; double t0 = mem_now();
    for (int j = 0; j < kb; j++) for (int i = 0; i < ka; i++) {
        size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
        dbig ai = db_view(A, oa, na - oa < pa ? na - oa : pa), bj = db_view(B, ob, nb - ob < pb ? nb - ob : pb);
        db_norm(&ai); db_norm(&bj);
        if (!ai.n || !bj.n) continue;
        if (grid_piece_skipped(oa, ob, ai.n, bj.n, lowcut, w)) { skipped++; continue; }   /* nothing of it below w, or all of it below the low cut */
        int sa = pin ? (i < nA ? fs[nB + i] : -1) : (i < nA ? fs[i] : -1), sb = pin ? (j < nB ? fs[j] : -1) : (nB ? fs[nA + j % nB] : -1);
        size_t pn = ai.n + bj.n;
        if (first) {                                                                   /* the first formed piece straight into C (shifted up if not (0,0)) */
            if (oa + ob) { db_reserve(&t, pn + 8); dist_core(acc_db(&ai, 0, ai.n), acc_db(&bj, 0, bj.n), acc_db(&t, 0, pn), pn, sa, sb); t.n = pn; db_norm(&t); db_shl_limbs(Cd, &t, oa + ob); }
            else { dist_core(acc_db(&ai, 0, ai.n), acc_db(&bj, 0, bj.n), acc_db(Cd, 0, pn), pn, sa, sb); Cd->n = pn; db_norm(Cd); }
            first = 0; formed++; continue;
        }
        db_reserve(&t, pn + 8); dist_core(acc_db(&ai, 0, ai.n), acc_db(&bj, 0, bj.n), acc_db(&t, 0, pn), pn, sa, sb); t.n = pn; db_norm(&t);
        db_add_shifted(Cd, &t, oa + ob, Cd);                   /* in place */
        formed++;
    }
    if (first) Cd->n = 0;
    db_free(&t);
    rns_dist_st.n_formed += (size_t)formed; rns_dist_st.n_skipped += (size_t)skipped;   /* Phase 14 R1 (E7): the reciprocal reports its cut per doubling */
    if (pin) { for (int j = 0; j < nB; j++) g_cache.s[fs[j]].pinned = 1; g_cache.pin_next = 0; }   /* B's pieces stay for the next product */
    cache_drop(0);
    if (verbose || skipped) printf("   dist_db %zu x %zu limbs: %d x %d pieces of %zu + %zu, %d formed, %d skipped%s%s: %.3f s (cache %d slots%s: %zu hits, %zu misses)\n", na, nb, ka, kb, pa, pb, formed, skipped,
                                   lowcut ? " (low cut)" : "", w != (size_t)-1 ? " (low product)" : "", mem_now() - t0, N, pin ? ", B pinned" : g_cache.hold ? ", held" : "", g_cache.hits, g_cache.misses);
}

/* ---- Phase 8 M3: the product over a node group (mdb.h) -------------------------------------------------
 * Phase 11 L (C3-576, B7): the block-cyclic map for a group of ANY size.  Rank rho = g d + r (APU d of the
 * group's node r, r < g -- every node of the group takes part in the transform) holds the rows
 * [R rho / nr, R (rho+1) / nr) of the R x C plane, nr = 4 g (floor or ceil of R / nr rows each), and the
 * columns [C rho / nr, C (rho+1) / nr) of the column layout.  Its row sequence is t = j rows + il <-> limb
 * m = R j + row0 + il (columns j in order, so m increases with t).  A node's contiguous share [lo, hi) meets
 * the sequence in one segment [seq_start(lo), seq_start(hi)).  For g a power of two the map is ntt_dist's
 * (rows = R / nr exactly) and the transform is its pipelined one, bit for bit as before; otherwise (3, 5, 6,
 * 9, 576 ...) the general four-step below (gen_fwd / gen_inv_pw) whose exchanges are alltoallv's of per-pair
 * slabs (rows(rho) x cols(sigma) points) in K chunks of the sender's rows.  Every exchange of an operand or
 * a result is an alltoallv of the exact segments (B7): the received slabs ARE the rank's sequence, back to
 * back in node order -- no padded scratch, no per-point search.
 * Phase 9 A3: the operands are views (mdbv: a window [off, off + len) of a sharded number, the pack kernel
 * reading the share at the window's offset) and the result of a piece product is delivered to the window of
 * every node's share of C that the piece covers (its limbs [shift, shift + n)): the first piece straight into
 * the zero-filled shares, the others into a temporary of that window, the spills added there, then
 * C's share += T << offset (fixed length, the carry scan over the nodes). */
__host__ __device__ static inline size_t part0(size_t R, int nr, int rho) { return R * (size_t)rho / nr; }   /* the first row (column) of rank rho */
__host__ __device__ static inline size_t partn(size_t R, int nr, int rho) { return part0(R, nr, rho + 1) - part0(R, nr, rho); }
__host__ __device__ static inline int part_owner(size_t R, int nr, size_t i) { return (int)(((i + 1) * (size_t)nr + R - 1) / R) - 1; }   /* the rank whose part holds index i */
__host__ __device__ static inline size_t seq_start(size_t m, size_t R, int nr, int rho)   /* the first t of rank rho's sequence with m(t) >= m */
{
    size_t j = m / R, i = m - j * R, a = part0(R, nr, rho), rows = partn(R, nr, rho);
    if (i <= a) return j * rows;
    if (i < a + rows) return j * rows + (i - a);
    return (j + 1) * rows;
}
struct seg { size_t t0, t1, off; };                            /* a segment [t0, t1) of a rank's sequence and its limb offset in a slab buffer */
/* pack this node's part [lo, hic) of an operand (local index m - lo in src) for the ranks (r, d), r < g: the segment
 * sg[r] of rank rho = g d + r's sequence, at sg[r].off of sb (S bounds any segment: the iteration space) */
__global__ void k_pack_mn(uint64_t *sb, struct acc src, size_t lo, size_t R, int nr, int g, int d, size_t S, const struct seg *sg)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S; int rho = g * d + (int)r;
        if (k >= sg[r].t1 - sg[r].t0) continue;
        size_t rows = partn(R, nr, rho), u = sg[r].t0 + k, j = u / rows, il = u - j * rows, m = R * j + part0(R, nr, rho) + il;
        sb[sg[r].off + k] = acc_get(src, m - lo);
    }
}
/* the received segments, back to back = my sequence [0, tend) (zero beyond) -> my rows: x[il C + j] (transpose = 1, the
 * transform's row layout) or x[t] (the sequence, the CRT's added operand) */
__global__ void k_gather_mn(uint64_t *x, const uint64_t *rb, size_t tend, size_t rows, size_t C, ec_mod m, int canon, int transpose)
{
    size_t total = rows * C, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        uint64_t v = t < tend ? rb[t] : 0;
        if (canon) v = ec_canon64(v, m.pu, m.mu);
        size_t j = t / rows, il = t - j * rows;
        x[transpose ? il * C + j : t] = v;
    }
}
/* the received slabs of the ranks (r, d), r < g (segments sg[r] of my window [lo, ..) in their sequences, at sg[r].off of rb)
 * -> the window's limbs */
__global__ void k_scatter_mn(struct acc dst, size_t lo, const uint64_t *rb, const struct seg *sg, int g, size_t S, size_t R, int nr, int d)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) {
        size_t r = t / S, k = t - r * S; if (k >= sg[r].t1 - sg[r].t0) continue;
        int rho = g * d + (int)r; size_t rows = partn(R, nr, rho), u = sg[r].t0 + k, j = u / rows, il = u - j * rows, m = R * j + part0(R, nr, rho) + il;
        *acc_ptr(dst, m - lo) = rb[sg[r].off + k];
    }
}
#include "p24_mn.h"                                          /* Phase 15 Batch 3 P24: k_pack_mn24, k_gather_mn24, k_scatter_mn24 (the maps of p24.h) */
static size_t max_share(const mdb *x) { return x->g ? (x->N + x->g - 1) / x->g : 0; }
static int g_node_of(mn_group *G) { return G->g0 + G->me; }
static size_t grp_max(mn_group *G, size_t v) { return G->g > 1 ? comm_allreduce_max(G->all[0], v) : v; }
static int is_pow2(int g) { return g > 0 && !(g & (g - 1)); }
static int dist_gen_forced(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_GEN"); v = e ? atoi(e) != 0 : 0; } return v; }   /* tests: the general transform at a power-of-two g too */
/* the layered communicator of the group over mesh d: all g nodes (rank rho = g d + r) */
static comm *lay_get(mn_group *G, int d)
{
    if (!G->lay[d]) {
        G->lay[d] = comm_layered_create(RS[d].cm, G->all[d], d);
        const char *e = getenv("COMM_LAYER_INTER2"); if (e && atoi(e)) comm_layered_set_inter2(G->lay[d], mn_mesh2(G, d, 0));   /* X2: collective over the group's nodes (all call lay_get for the same product) */
    }
    return G->lay[d];
}
struct mn_ctx { mn_group *G; int node, g, nr; size_t R, C, n; int p24; };   /* p24 (Phase 15 Batch 3): the operands' sequences are the in-runs of p24.h */
/* P24: rank rho's in-run (ex 1) or out-run (ex 0) map, and the first index of its sequence at or above limb l */
static inline struct p24_run mn_run(const struct mn_ctx *X, int rho, int ex) { return p24_rank_run(X->R, X->C, X->nr, rho, ex); }
static inline size_t mn_seq_start(const struct mn_ctx *X, size_t l, int rho) { if (!X->p24) return seq_start(l, X->R, X->nr, rho); struct p24_run m = mn_run(X, rho, 1); return p24_seq_start(&m, l); }
/* node r's part of a view, in view coordinates [lo, hi) (its share of m cut to the window and to the view's length) */
static void view_share(const mdbv *v, int r, size_t *lo, size_t *hi)
{
    size_t slo, shi; mdb_share(v->m, r, &slo, &shi);
    size_t a = slo > v->off ? slo - v->off : 0, b = shi > v->off ? shi - v->off : 0;
    if (a > v->len) a = v->len; if (b > v->len) b = v->len; if (a > b) a = b;
    *lo = a; *hi = b;
}
/* the segment tables of one operand on APU thread d: send -- my part in the sequence of every rank (r, d); receive -- every
 * node's part in my sequence (rank (me, d)); offsets back to back (limbs); the totals returned (limbs).  The received
 * segments are contiguous in node order (the parts tile the view), so they form my sequence [0, seq_start(len)). */
static void seg_tables(const struct mn_ctx *X, const mdbv *op, int d, struct seg *ss, struct seg *rs, size_t *stot, size_t *rtot, size_t *S)
{
    size_t lo, hi, o = 0, mx = 0; view_share(op, X->node, &lo, &hi);
    for (int r = 0; r < X->g; r++) { int rho = X->g * d + r; ss[r].t0 = mn_seq_start(X, lo, rho); ss[r].t1 = mn_seq_start(X, hi, rho); ss[r].off = o; o += ss[r].t1 - ss[r].t0; if (ss[r].t1 - ss[r].t0 > mx) mx = ss[r].t1 - ss[r].t0; }   /* (P24: the in-runs) */
    *stot = o; o = 0;
    int rho = X->g * d + X->G->me;
    for (int r = 0; r < X->g; r++) { view_share(op, X->G->g0 + r, &lo, &hi); rs[r].t0 = mn_seq_start(X, lo, rho); rs[r].t1 = mn_seq_start(X, hi, rho); rs[r].off = o; o += rs[r].t1 - rs[r].t0; }
    *rtot = o; *S = mx;
}
static void seg_bytes(const struct seg *sg, int g, size_t *cnt, size_t *dsp) { for (int r = 0; r < g; r++) { cnt[r] = (sg[r].t1 - sg[r].t0) * 8; dsp[r] = sg[r].off * 8; } }
/* one operand: pack my part, exchange over mesh d (an alltoallv of the segments), the received sequence in rb */
struct rdst { struct seg *hs, *ds; size_t *cnt; uint64_t *sb, *rb; size_t tend; };   /* per operand and APU: the tables, the buffers, the received sequence's end */
static void redistribute(const struct mn_ctx *X, const mdbv *op, struct rdst *o, int d, hipStream_t s)
{
    int g = X->g; size_t stot, rtot, S;
    struct seg *ss = o->hs, *rs = o->hs + g;
    seg_tables(X, op, d, ss, rs, &stot, &rtot, &S);
    size_t *scnt = o->cnt, *sdsp = scnt + g, *rcnt = sdsp + g, *rdsp = rcnt + g;
    seg_bytes(ss, g, scnt, sdsp); seg_bytes(rs, g, rcnt, rdsp);
    o->sb = db_pool_alloc(d, (stot + 16) * 8); o->rb = db_pool_alloc(d, (rtot + 16) * 8);
    o->tend = rtot;
    if (stot) {
        size_t lo, hi; view_share(op, X->node, &lo, &hi);
        size_t slo, shi; mdb_share(op->m, X->node, &slo, &shi);
        struct acc a = acc_db(&op->m->sh, op->off + lo - slo, hi - lo);   /* the window's first limb within my share */
        HIP_CHECK(hipMemcpyAsync(o->ds, ss, g * sizeof *ss, hipMemcpyHostToDevice, s));
        if (X->p24) k_pack_mn24<<<nblk((size_t)g * S), 256, 0, s>>>(o->sb, a, lo, X->R, X->C, X->nr, g, d, S, o->ds);   /* P24: the in-runs */
        else k_pack_mn<<<nblk((size_t)g * S), 256, 0, s>>>(o->sb, a, lo, X->R, X->nr, g, d, S, o->ds);
    }
    HIP_CHECK(hipStreamSynchronize(s));
    { int xk_ = comm_xtag_set(XK_REDIST); comm_alltoallv(X->G->all[d], o->sb, scnt, sdsp, o->rb, rcnt, rdsp, s); comm_xtag_set(xk_); } comm_wait(X->G->all[d]);
    db_pool_free(d, o->sb); o->sb = 0;
}
/* the (carry, propagate) flags of the g nodes' shares, all-gathered over the group's mesh 0 (one byte per node, host
 * point-to-point: write to all, then read from all), and the scan: carry into node r = c_{r-1} | (p_{r-1} & carry into r-1);
 * returns the carry into this node, and aborts on a carry out of the last node (the fixed-length sum overflowed) */
static int g_top_ok;                                          /* Phase 10 A5: a truncated product (basis w): the carry out of the top node is dropped (mod B^w) */
static int node_carry_in(mn_group *G, int c, int p)
{
    comm *cm = G->all[0]; int g = G->g, me = G->me, cin = 0;
    uint8_t v = (uint8_t)(c | (p << 1)), *all = (uint8_t *)malloc(g); all[me] = v;
    for (int r = 0; r < g; r++) if (r != me) comm_send(cm, r, &v, 1);
    for (int r = 0; r < g; r++) if (r != me) comm_recv(cm, r, &all[r], 1);
    for (int r = 0; r < me; r++) cin = (all[r] & 1) | (((all[r] >> 1) & 1) & cin);
    int top = cin; for (int r = me; r < g; r++) top = (all[r] & 1) | (((all[r] >> 1) & 1) & top);
    free(all);
    if (top && !g_top_ok) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: carry out of the top share (node %d)\n", G->g0 + me); }
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

/* Phase 12 G (the exact spill exchange): the columns j whose 4-limb spill block [R j + a, R j + a + 4) meets the window
 * [lo, hi) of a node's share (piece coordinates): a contiguous range [j0, j1) -- a = the first row of the rank after the
 * spilling one (the block-cyclic map: rank rho's stripe j spills at R j + part0(rho + 1), the last rank's at R (j + 1)) */
static void spill_cols(size_t lo, size_t hi, size_t a, size_t R, size_t C, size_t *j0, size_t *j1)
{
    size_t x0 = lo > a + 3 ? (lo - a - 3 + R - 1) / R : 0, x1 = hi > a ? (hi - a + R - 1) / R : 0;   /* R j + a + 3 >= lo; R j + a < hi */
    if (x0 > C) x0 = C; if (x1 > C) x1 = C; if (x1 < x0) x1 = x0;
    *j0 = x0; *j1 = x1;
}
/* ---- Phase 11 L: the general four-step over 4 g ranks with unequal parts (g not a power of two) ------------------
 * Row layout: my rows x C row-major; column layout: my cols columns of R points.  Forward: the row pass, then chunk k
 * of my rows (rows k / K ..) twiddled and packed into slabs, slab sigma = my chunk rows x sigma's columns column-major
 * (sb[col0(sigma) rk + jl rk + il]: the slabs back to back in rank order), one alltoallv per chunk over the layered
 * communicator (the wire time of chunk k under the row pass and pack of chunk k + 1); then the unpacks (slab rho of
 * chunk k = my cols x rho's chunk rows -> x[jl R + i0(rho, k) + il]) and the column pass.  Inverse: the mirror.  The
 * tables T[k][rho] = (offset, first global row, rows) of every rank's chunk k in the "my columns" side of the slab
 * buffers; the counts of both directions from them.  The twiddle tables are the dist_plan's (ntt_dist.c). */
struct rkt { size_t off, i0, n; };
struct gplan { int K, nr, logR, logC; size_t R, C, rows, row0, cols, qs, qr; struct rkt *hT, *dT; size_t *regs, *regr, *fsc, *fsd, *frc, *frd; };
static void gplan_build(struct gplan *p, int g, int rho, int logR, int logC, int d)
{
    int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC;
    p->nr = nr; p->logR = logR; p->logC = logC; p->R = R; p->C = C;
    p->rows = partn(R, nr, rho); p->row0 = part0(R, nr, rho); p->cols = partn(C, nr, rho);
    p->qs = p->rows * C; p->qr = p->cols * R;
    size_t rmin = R / nr; int K = getenv("DIST_CHUNKS") ? atoi(getenv("DIST_CHUNKS")) : 8; if (K < 1) K = 1; if (K > 16) K = 16;
    while (K > 1 && rmin / K < 32) K--;
    p->K = K;
    p->hT = (struct rkt *)malloc((size_t)K * nr * sizeof *p->hT);
    p->regs = (size_t *)malloc((size_t)(2 * (K + 1) + 4 * K * nr) * sizeof(size_t)); p->regr = p->regs + K + 1;
    p->fsc = p->regr + K + 1; p->fsd = p->fsc + (size_t)K * nr; p->frc = p->fsd + (size_t)K * nr; p->frd = p->frc + (size_t)K * nr;
    for (int k = 0; k <= K; k++) {
        p->regs[k] = C * (p->rows * k / K);
        size_t sum = 0; for (int r = 0; r < nr; r++) sum += partn(R, nr, r) * k / K;
        p->regr[k] = p->cols * sum;
    }
    for (int k = 0; k < K; k++) {
        size_t rk = p->rows * (k + 1) / K - p->rows * k / K, o = 0;
        for (int r = 0; r < nr; r++) {
            size_t rr = partn(R, nr, r), i0 = rr * k / K, n = rr * (k + 1) / K - i0;
            struct rkt *t = &p->hT[(size_t)k * nr + r]; t->off = o; t->i0 = part0(R, nr, r) + i0; t->n = n; o += p->cols * n;
            /* forward: send my chunk rows x r's columns (at col0(r) rk of region k), receive my columns x r's chunk rows */
            p->fsc[(size_t)k * nr + r] = partn(C, nr, r) * rk * 8; p->fsd[(size_t)k * nr + r] = (p->regs[k] + part0(C, nr, r) * rk) * 8;
            p->frc[(size_t)k * nr + r] = p->cols * n * 8; p->frd[(size_t)k * nr + r] = (p->regr[k] + t->off) * 8;
        }
    }
    p->dT = (struct rkt *)db_pool_alloc(d, (size_t)K * nr * sizeof *p->hT + 64);
    HIP_CHECK(hipMemcpy(p->dT, p->hT, (size_t)K * nr * sizeof *p->hT, hipMemcpyHostToDevice));
}
static void gplan_free(struct gplan *p, int d) { db_pool_free(d, (uint64_t *)p->dT); free(p->hT); free(p->regs); }
/* twiddle + pack of my rows chunk [i0l, i0l + rk) (local row index): x[(i0l + il) C + jb] w_n^(i j), i = row0 + i0l + il,
 * j = brev(jb) -> sb[col0(sigma) rk + jl rk + il], sigma the owner of column jb.  Tiles of 32 x 32 through LDS. */
__global__ void k_twpack_g(const uint64_t *x, uint64_t *sb, size_t rk, size_t i0g, int logC, int nr, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx; if (il >= rk) continue;
        size_t i = i0g + il, j = brev3((unsigned)jb, logC), e = i * j;
        double w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv);
        tile[ty + k][tx] = (uint64_t)ec_mm((double)x[il * C + jb], w, m.p, m.pinv);
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t jb = bj + ty + k, il = bi + tx; if (il >= rk) continue;
        int sg = part_owner(C, nr, jb); size_t jl = jb - part0(C, nr, sg);
        sb[part0(C, nr, sg) * rk + jl * rk + il] = tile[tx][ty + k];
    }
}
/* the mirror: slabs of my rows chunk -> x rows, with the inverse twiddle (the column inverse's output folded first) */
__global__ void k_unpacktw_g(const uint64_t *rb, uint64_t *x, size_t rk, size_t i0g, int logC, int nr, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t jb = bj + ty + k, il = bi + tx; if (il >= rk) continue;
        int sg = part_owner(C, nr, jb); size_t jl = jb - part0(C, nr, sg);
        tile[ty + k][tx] = rb[part0(C, nr, sg) * rk + jl * rk + il];
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k, jb = bj + tx; if (il >= rk) continue;
        size_t i = i0g + il, j = brev3((unsigned)jb, logC), e = i * j;
        double w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv);
        x[il * C + jb] = (uint64_t)ec_mm((double)ec_fold(tile[tx][ty + k], m.pu), w, m.p, m.pinv);
    }
}
/* ---- Phase 15 G5: DIST_TWREC_G=1 (default since Batch 2, 2026-09-28; see twrec_g below) -- the general map's twiddled pack / unpack with the twiddle by a row recurrence
 * (ntt_dist.c k_twpack_r's form): a thread's four rows il, il + 8, il + 16, il + 24 of column j take w_n^(i j) from the tables
 * once and step by w_n^(8 j) (one per column, in LDS) -- two table gathers per four points instead of eight.  ec_mm returns
 * canonical residues, so the chain gives exactly w_n^((i + 8) j): bit-identical (gen_pack_bench checks it).  The slab address
 * part0(C, nr, sg) rk + jl rk + il of k_twpack_g is jb rk + il for any owner sg (jl = jb - part0(C, nr, sg)): written so, with no
 * per-point owner search.  rk need not be a multiple of 32: a thread whose first row is past rk reads no table (its rows are all
 * past rk); the others step through rows past rk without reading or writing them. */
__global__ void k_twpack_gr(const uint64_t *x, uint64_t *sb, size_t rk, size_t i0g, int logC, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    __shared__ double stp[32];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    size_t jb = bj + tx, j = brev3((unsigned)jb, logC);
    if (ty == 0) { size_t e = 8 * j; stp[tx] = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv); }
    double w = 0;
    if (bi + ty < rk) { size_t e = (i0g + bi + ty) * j; w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv); }
    __syncthreads();
    double st = stp[tx];
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k;
        if (il < rk) tile[ty + k][tx] = (uint64_t)ec_mm((double)x[il * C + jb], w, m.p, m.pinv);
        if (k < 24) w = ec_mm(w, st, m.p, m.pinv);
    }
    __syncthreads();
    for (int k = 0; k < 32; k += 8) {
        size_t jw = bj + ty + k, il = bi + tx; if (il >= rk) continue;
        sb[jw * rk + il] = tile[tx][ty + k];
    }
}
__global__ void k_unpacktw_gr(const uint64_t *rb, uint64_t *x, size_t rk, size_t i0g, int logC, const uint64_t *twr, const uint64_t *twc, ec_mod m)
{
    __shared__ uint64_t tile[32][33];
    __shared__ double stp[32];
    size_t C = (size_t)1 << logC, bj = (size_t)blockIdx.x * 32, bi = (size_t)blockIdx.y * 32;
    int tx = threadIdx.x, ty = threadIdx.y;
    for (int k = 0; k < 32; k += 8) {
        size_t jr = bj + ty + k, il = bi + tx; if (il >= rk) continue;
        tile[ty + k][tx] = rb[jr * rk + il];
    }
    size_t jb = bj + tx, j = brev3((unsigned)jb, logC);
    if (ty == 0) { size_t e = 8 * j; stp[tx] = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv); }
    double w = 0;
    if (bi + ty < rk) { size_t e = (i0g + bi + ty) * j; w = ec_mm((double)twr[e >> logC], (double)twc[e & (C - 1)], m.p, m.pinv); }
    __syncthreads();
    double st = stp[tx];
    for (int k = 0; k < 32; k += 8) {
        size_t il = bi + ty + k;
        if (il < rk) x[il * C + jb] = (uint64_t)ec_mm((double)ec_fold(tile[tx][ty + k], m.pu), w, m.p, m.pinv);
        if (k < 24) w = ec_mm(w, st, m.p, m.pinv);
    }
}
static int twrec_g(void) { static int v = -1; if (v < 0) { const char *e = getenv("DIST_TWREC_G"); v = e ? atoi(e) != 0 : 1; } return v; }   /* Phase 15 G5 (default 1 since Batch 2, the user's decision 2026-09-28) */
/* slab rho of a chunk (my cols x rho's chunk rows, column-major) <-> my columns of R points; block y = rho */
__global__ void k_unpack_g(const uint64_t *rb, uint64_t *x, const struct rkt *T, size_t cols, size_t R)
{
    const struct rkt t = T[blockIdx.y]; size_t total = cols * t.n, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) { size_t jl = i / t.n, il = i - jl * t.n; x[jl * R + t.i0 + il] = rb[t.off + i]; }
}
__global__ void k_pack_cols_g(const uint64_t *x, uint64_t *sb, const struct rkt *T, size_t cols, size_t R)
{
    const struct rkt t = T[blockIdx.y]; size_t total = cols * t.n, i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) { size_t jl = i / t.n, il = i - jl * t.n; sb[t.off + i] = x[jl * R + t.i0 + il]; }
}
static size_t gp_rk(const struct gplan *p, int k) { return p->rows * (k + 1) / p->K - p->rows * k / p->K; }
static size_t gp_nmax(const struct gplan *p, int k) { size_t mx = 0; for (int r = 0; r < p->nr; r++) if (p->hT[(size_t)k * p->nr + r].n > mx) mx = p->hT[(size_t)k * p->nr + r].n; return mx; }
/* Phase 15 G5: DIST_STATS=1 -- the general map's twiddled packs timed by events per APU (the host waits for each: a stats-mode
 * perturbation); g_gpk[d][0] the forward packs, [1] the inverse unpacks, seconds summed over the run; mn_core prints them */
static double g_gpk[NR][2]; static hipEvent_t g_gev[NR][2];
static int gpk_begin(hipStream_t s, int *dev)
{
    if (!dist_st.on) return 0;
    HIP_CHECK(hipGetDevice(dev)); if (!g_gev[*dev][0]) { HIP_CHECK(hipEventCreate(&g_gev[*dev][0])); HIP_CHECK(hipEventCreate(&g_gev[*dev][1])); }
    HIP_CHECK(hipEventRecord(g_gev[*dev][0], s)); return 1;
}
static void gpk_end(hipStream_t s, int dev, int which)
{
    float ms; HIP_CHECK(hipEventRecord(g_gev[dev][1], s)); HIP_CHECK(hipEventSynchronize(g_gev[dev][1]));
    HIP_CHECK(hipEventElapsedTime(&ms, g_gev[dev][0], g_gev[dev][1])); g_gpk[dev][which] += ms * 1e-3;
}
static void gen_fwd(const struct gplan *p, const dist_plan *pl, ntt_ctx *ctx, int prime, comm *cl, uint64_t *x, uint64_t *sb, uint64_t *rb, hipStream_t s)
{
    ec_mod m = ec_mod_get(prime); int nr = p->nr;
    ntt_fwd(ctx, x, p->logC, p->rows, s);                                                 /* rows: length-C, bit-reversed columns */
    for (int k = 0; k < p->K; k++) {
        size_t i0l = p->rows * k / p->K, rk = gp_rk(p, k);
        dim3 grid((unsigned)(p->C / 32), (unsigned)((rk + 31) / 32)), blk(32, 8);
        int dv, st = gpk_begin(s, &dv);
        if (twrec_g()) k_twpack_gr<<<grid, blk, 0, s>>>(x + i0l * p->C, sb + p->regs[k], rk, p->row0 + i0l, p->logC, pl->twr, pl->twc, m);   /* Phase 15 G5 */
        else k_twpack_g<<<grid, blk, 0, s>>>(x + i0l * p->C, sb + p->regs[k], rk, p->row0 + i0l, p->logC, nr, pl->twr, pl->twc, m);
        if (st) gpk_end(s, dv, 0);
        comm_alltoallv(cl, sb, p->fsc + (size_t)k * nr, p->fsd + (size_t)k * nr, rb, p->frc + (size_t)k * nr, p->frd + (size_t)k * nr, s);   /* (posting k completes k - 1) */
    }
    for (int k = 0; k < p->K; k++) comm_wait(cl);                                          /* one wait per post (the last completes the last chunk) */
    for (int k = 0; k < p->K; k++) { size_t nm = gp_nmax(p, k); if (nm) k_unpack_g<<<dim3(nblk(p->cols * nm), nr), 256, 0, s>>>(rb + p->regr[k], x, p->dT + (size_t)k * nr, p->cols, p->R); }
    ntt_fwd(ctx, x, p->logR, p->cols, s);                                                 /* columns: length-R */
}
/* the inverse with the pointwise product x <- x y fused into the column inverse (y = 0: none), as dist_inv_pw */
static void gen_inv_pw(const struct gplan *p, const dist_plan *pl, ntt_ctx *ctx, int prime, comm *cl, uint64_t *x, const uint64_t *y, uint64_t *sb, uint64_t *rb, hipStream_t s)
{
    ec_mod m = ec_mod_get(prime); int nr = p->nr, K = p->K;
    if (y) ntt_inv_pw_y(ctx, x, y, NTT_Y_FULL, p->logR, p->cols, s); else ntt_inv(ctx, x, p->logR, p->cols, s);   /* columns: x R^-1, natural */
    for (int k = 0; k < K; k++) { size_t nm = gp_nmax(p, k); if (nm) k_pack_cols_g<<<dim3(nblk(p->cols * nm), nr), 256, 0, s>>>(x, sb + p->regr[k], p->dT + (size_t)k * nr, p->cols, p->R); }
    for (int k = 0; k <= K; k++) {
        if (k < K) comm_alltoallv(cl, sb, p->frc + (size_t)k * nr, p->frd + (size_t)k * nr, rb, p->fsc + (size_t)k * nr, p->fsd + (size_t)k * nr, s);
        else for (int w = 0; w < K; w++) comm_wait(cl);                                  /* one wait per post */
        if (k == 0) continue;
        int kk = k - 1; size_t i0l = p->rows * kk / K, rk = gp_rk(p, kk);                  /* chunk k - 1 has arrived: unpack, twiddle, row inverse under the wire of chunk k */
        dim3 grid((unsigned)(p->C / 32), (unsigned)((rk + 31) / 32)), blk(32, 8);
        int dv, st = gpk_begin(s, &dv);
        if (twrec_g()) k_unpacktw_gr<<<grid, blk, 0, s>>>(rb + p->regs[kk], x + i0l * p->C, rk, p->row0 + i0l, p->logC, pl->twr_i, pl->twc_i, m);   /* Phase 15 G5 */
        else k_unpacktw_g<<<grid, blk, 0, s>>>(rb + p->regs[kk], x + i0l * p->C, rk, p->row0 + i0l, p->logC, nr, pl->twr_i, pl->twc_i, m);
        if (st) gpk_end(s, dv, 1);
        ntt_inv(ctx, x + i0l * p->C, p->logC, rk, s);                                     /* rows: length-C inverse, x C^-1 */
    }
}
/* Phase 15 G5: the general map's pack / unpack kernels on one APU (prime 0) for rank rho of nr = 4 g ranks, the plane 2^logR x
 * 2^logC, chunk 0 of K (the production chunking, gplan_build's K); median of reps; rate = 16 B x points / t.  The recurrence
 * forms are checked bit-identical against the plain ones.  Returns the number of mismatches.  (tests/t_dist: DIST_GBENCH) */
static int gb_cmp(const uint64_t *a, const uint64_t *b, size_t n)
{
    uint64_t *h = (uint64_t *)malloc(2 * n * 8); HIP_CHECK(hipMemcpy(h, a, n * 8, hipMemcpyDeviceToHost)); HIP_CHECK(hipMemcpy(h + n, b, n * 8, hipMemcpyDeviceToHost));
    int bad = memcmp(h, h + n, n * 8) != 0; free(h); return bad;
}
static int gb_dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
int gen_pack_bench(int logR, int logC, int g, int rho, int reps)
{
    int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rows = partn(R, nr, rho), row0 = part0(R, nr, rho), cols = partn(C, nr, rho);
    int K = getenv("DIST_CHUNKS") ? atoi(getenv("DIST_CHUNKS")) : 8; if (K < 1) K = 1; if (K > 16) K = 16;
    while (K > 1 && (R / nr) / K < 32) K--;
    size_t rk = rows / K, n = rk * C;                         /* chunk 0 (rows * 1 / K - 0) */
    size_t ncol = 0; struct rkt *hT = (struct rkt *)malloc(nr * sizeof *hT);
    for (int r = 0; r < nr; r++) { size_t rr = partn(R, nr, r), nn = rr / K; hT[r].off = ncol; hT[r].i0 = part0(R, nr, r); hT[r].n = nn; ncol += cols * nn; }
    size_t nmax = 0; for (int r = 0; r < nr; r++) if (hT[r].n > nmax) nmax = hT[r].n;
    size_t nx = n > cols * R ? n : cols * R, nb = n > ncol ? n : ncol;
    uint64_t *x, *x2, *sb, *sb2; struct rkt *dT;
    HIP_CHECK(hipMalloc(&x, nx * 8)); HIP_CHECK(hipMalloc(&x2, nx * 8)); HIP_CHECK(hipMalloc(&sb, nb * 8)); HIP_CHECK(hipMalloc(&sb2, nb * 8)); HIP_CHECK(hipMalloc(&dT, nr * sizeof *hT));
    HIP_CHECK(hipMemcpy(dT, hT, nr * sizeof *hT, hipMemcpyHostToDevice));
    { uint64_t *h = (uint64_t *)malloc(nx * 8), z = 88172645463325252ull, p0 = ec_P[0]; for (size_t i = 0; i < nx; i++) { z ^= z << 13; z ^= z >> 7; z ^= z << 17; h[i] = z % p0; }
      HIP_CHECK(hipMemcpy(x, h, nx * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(sb, h, nb * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(sb2, h, nb * 8, hipMemcpyHostToDevice)); free(h); }
    ec_mod m = ec_mod_get(0);
    uint64_t *twr = pow_table(0, ec_root(0, logR), R), *twc = pow_table(0, ec_root(0, logR + logC), C);
    uint64_t *ref; HIP_CHECK(hipMalloc(&ref, nb * 8));
    uint64_t *rin; HIP_CHECK(hipMalloc(&rin, nb * 8)); HIP_CHECK(hipMemcpy(rin, sb, nb * 8, hipMemcpyDeviceToDevice));   /* the unpack's input slabs */
    hipEvent_t e0, e1; HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1));
    double *t = (double *)malloc(reps * sizeof(double)); int bad = 0;
    dim3 blk(32, 8), gr((unsigned)(C / 32), (unsigned)((rk + 31) / 32));
    printf("gen-bench 2^%d x 2^%d over %d ranks (g %d), rank %d: rows %zu, chunk 0 of %d = %zu rows x %zu (%.1f MB), cols %zu\n", logR, logC, nr, g, rho, rows, K, rk, C, n * 8e-6, cols);
    const char *nm[] = { "device copy (hipMemcpyAsync D2D)", "k_twpack_g (fwd pack, twiddled: production)", "k_twpack_gr (G5, DIST_TWREC_G=1)",
                         "k_unpacktw_g (inv unpack, twiddled: production)", "k_unpacktw_gr (G5, DIST_TWREC_G=1)",
                         "k_unpack_g (fwd unpack, one chunk)", "k_pack_cols_g (inv pack, one chunk)" };
    for (int kind = 0; kind < 7; kind++) {
        size_t pts = kind >= 5 ? ncol : n;
        for (int r = 0; r <= reps; r++) {
            HIP_CHECK(hipEventRecord(e0, 0));
            switch (kind) {
            case 0: HIP_CHECK(hipMemcpyAsync(sb, x, n * 8, hipMemcpyDeviceToDevice, 0)); break;
            case 1: k_twpack_g<<<gr, blk>>>(x, sb, rk, row0, logC, nr, twr, twc, m); break;
            case 2: k_twpack_gr<<<gr, blk>>>(x, sb2, rk, row0, logC, twr, twc, m); break;
            case 3: k_unpacktw_g<<<gr, blk>>>(rin, x2, rk, row0, logC, nr, twr, twc, m); break;
            case 4: k_unpacktw_gr<<<gr, blk>>>(rin, x, rk, row0, logC, twr, twc, m); break;
            case 5: k_unpack_g<<<dim3(nblk(cols * nmax), nr), 256>>>(rin, x2, dT, cols, R); break;
            case 6: k_pack_cols_g<<<dim3(nblk(cols * nmax), nr), 256>>>(x2, sb2, dT, cols, R); break;
            }
            HIP_CHECK(hipEventRecord(e1, 0)); HIP_CHECK(hipEventSynchronize(e1));
            float ms; HIP_CHECK(hipEventElapsedTime(&ms, e0, e1)); if (r) t[r - 1] = ms * 1e-3;
        }
        qsort(t, reps, sizeof(double), gb_dcmp);
        double md = t[reps / 2]; const char *chk = "";
        if (kind == 2) { int b = gb_cmp(sb, sb2, n); bad += b; chk = b ? "  DIFFERS from the plain form" : "  bit-identical to the plain form"; }
        if (kind == 4) { int b = gb_cmp(x, x2, n); bad += b; chk = b ? "  DIFFERS from the plain form" : "  bit-identical to the plain form"; }
        printf("gen-bench 2^%d x 2^%d g %d  %-50s %9.3f ms  %7.1f GB/s%s\n", logR, logC, g, nm[kind], md * 1e3, 16.0 * pts / md * 1e-9, chk);
    }
    fflush(stdout);
    HIP_CHECK(hipFree(x)); HIP_CHECK(hipFree(x2)); HIP_CHECK(hipFree(sb)); HIP_CHECK(hipFree(sb2)); HIP_CHECK(hipFree(ref)); HIP_CHECK(hipFree(rin)); HIP_CHECK(hipFree(dT));
    HIP_CHECK(hipFree(twr)); HIP_CHECK(hipFree(twc)); HIP_CHECK(hipEventDestroy(e0)); HIP_CHECK(hipEventDestroy(e1)); free(t); free(hT);
    return bad;
}
/* the plane's shape for nc limbs over g nodes: logn, logR, logC, and the largest per-rank plane (limbs) */
static void mn_shape(size_t nc, int g, int *logn_, int *logR_, int *logC_, size_t *qmax)
{
    int nr = 4 * g, lg = 0; while ((1 << lg) < nr) lg++;
    int logn = 0; while (((size_t)1 << logn) < nc) logn++;
    int logmin = 2 * (5 + lg); if (logmin < 20) logmin = 20;   /* rows >= 32, R, C >= 2^10 */
    if (logn < logmin) logn = logmin;
    int logR = logn / 2 + dist_logr_delta(), logC; { int lo = 5 + lg < 10 ? 10 : 5 + lg; if (logR < lo) logR = lo; if (logR > logn - 10) logR = logn - 10; } logC = logn - logR;
    size_t R = (size_t)1 << logR, C = (size_t)1 << logC, qs = ((R + nr - 1) / nr) * C, qr = ((C + nr - 1) / nr) * R;
    *logn_ = logn; *logR_ = logR; *logC_ = logC; *qmax = qs > qr ? qs : qr;
}
/* the plane cap of the mn tier: one plane per node pool (EC_NP q limbs in pool 0 of 2^pool_log limbs, 3 q + 16 in pool 1,
 * q = the largest rank plane), at most 2^31 points per rank plane's power of two: 2^(min(31, pool_log) + floor(log2 g))
 * for g a power of two, one less where the rounding of R / nr does not fit (never for g <= 2304) */
static int mn_logn_cap(int g)
{
    int lgt = 0; while ((2 << lgt) <= g) lgt++;
    int c = dist_logn_max(); if (rns_pool_log() < c) c = rns_pool_log();
    int logn = c + lgt;
    if (!is_pow2(g)) { int ln, lr, lc; size_t qm; mn_shape((size_t)1 << logn, g, &ln, &lr, &lc, &qm); if (qm > ((size_t)1 << (c - 2))) logn--; }
    return logn;
}
int rns_mul_dist_mn_logcap(mn_group *G) { return mn_logn_cap(G->g); }
struct mn_times { double redistribute, ntt, crt, out, carry, total; };
size_t rns_dist_tscratch_max;                                 /* Phase 13a M: the largest window temporary + result slabs of one product or shifted add (bytes per node-process, measured) */
/* ---- Phase 13a M (TASKS 1.3): the accumulating piece's window in rounds (MN_T_CHUNK_MB) ------------------------------ */
static size_t mn_t_chunk_limbs(void) { static long v = -1; if (v < 0) { const char *e = getenv("MN_T_CHUNK_MB"); v = (long)((e ? atof(e) : 1024.0) * 1048576.0 / 8) * 4;   /* default 1024 MB since Phase 14 (RESULTS §84: the target needs it to fit 480 GB); =0 one round */ if (v < 0) v = 0; } return (size_t)v; }   /* limbs per node per round (4 APUs) */
/* node r's chunk c of its window (piece coordinates): [lo + c W, min(lo + (c + 1) W, hi)); the whole window when not chunked */
static void mn_win_chunk(const mdb *C, int r, size_t shift, size_t Np, int c, size_t W, int chunked, size_t *lo, size_t *hi)
{
    size_t a, b; piece_window(C, r, shift, Np, &a, &b);
    if (chunked) { size_t x = a + (size_t)c * W; if (x > b) x = b; size_t y = x + W < b ? x + W : b; a = x; b = y; }
    *lo = a; *hi = b;
}
/* the running state of the rounds on this node: cy the piece's carry out of the last chunk (into the next chunk's limb 0), kc
 * C's carries out of its share (at most 1 in all: C < B^cn and the window's normalised part < B^tn before its carry-out), pr
 * the propagate flag of C's last add, cot the carry out of the window's top when the window ends at the share's top (a unit
 * at the next node's share bottom: the second scan) */
struct mn_rounds { int cy, kc, pr, cot; };
/* one round on this node (one thread; the db calls use the four APUs): T (this round's chunk [wlo, wlo + wn) of the window)
 * += its spills and the previous chunk's carry (together at most one carry out: the chunk's digits are < 2B each), then
 * C's share += T << (wlo + shift - clo); T re-zeroed for the next round.  The value added to C over the rounds is the
 * window's part of the piece, exactly as the one-T form adds it: the result is bit-identical. */
static void mn_round_add(struct mn_rounds *rd, mdb *Cn, dbig *T, size_t wlo, size_t wn, int last, size_t shift, size_t clo, size_t cn,
                         const uint64_t *const sp[NR], const size_t *const sptab[NR], size_t R, int nr, size_t C, int g, uint64_t **spill_rb, size_t **sptab_d, size_t tcap, int p24)
{
    if (wn) {
        int co = 0, pr = 1, c2 = 0, p2 = 0;
        if (p24) db_share_add_spills_x24(T, wn, wlo, sp, sptab, R, C, g, &co, &pr);   /* P24: the blocks at L(R j + a') */
        else db_share_add_spills_x(T, wn, wlo, sp, sptab, R, R / nr, C, g, &co, &pr);
        if (rd->cy) { int co1 = 0, pr1 = 0; db_share_add_val(T, wn, 0, 1, 0, &co1, &pr1); co += co1; }
        if (co > 1) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: two carries out of a chunk (the bound is 1)\n"); }
        db_share_add_shifted(&Cn->sh, cn, T, wlo + shift - clo, &c2, &p2); rd->kc += c2; rd->pr = p2;
        rd->cy = co;
        if (last) {
            size_t top = wlo + wn + shift - clo;
            if (top >= cn) rd->cot = rd->cy;
            else if (rd->cy) { db_share_add_val(&Cn->sh, cn, top, 1, 0, &c2, &p2); rd->kc += c2; rd->pr = p2; }
            rd->cy = 0;
        } else db_zero_fill(T, tcap);
    }
    for (int d = 0; d < NR; d++) { db_pool_free(d, spill_rb[d]); db_pool_free(d, sptab_d[d]); }
}
/* the core of one plane: C's shares += (A B (+ X)) << shift.  direct: shift 0, the piece is the whole product (X allowed),
 * the rows go straight into C's zero-filled shares (M3's path, bit for bit).  Otherwise the rows go into a temporary T
 * of this node's window [tlo, thi) of the piece (in piece coordinates), the spills are added there, and C's share
 * += T << (tlo + shift - clo).  Np = the piece's basis (na + nb, + 1 with X). */
static void mn_core(mdb *Cn, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t shift, int direct, struct mn_times *tm, int slA, int slB, int p24)
{
    int g = G->g, me = G->me, nr = 4 * g, node = G->g0 + me, verbose = getenv("RNS_VERBOSE") != 0;
    int gen = !is_pow2(g) || dist_gen_forced();                /* the general transform (unequal parts) or ntt_dist's pipelined one */
    size_t na = A->len, nb = B->len, nc = na + nb, Np = nc + (X ? 1 : 0);
    if (!na || !nb) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: a zero operand\n"); }
    if (X && X->n > nc) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: the added operand (%zu limbs) exceeds the product (%zu)\n", X->n, nc); }
    /* Phase 15 Batch 3 P24 (results/P2415.md): the plane holds 24-digit points, p24_pts(na) + p24_pts(nb) of them, at four primes;
     * the operands' sequences are the ranks' in-runs of limbs (p24.h), the result's their out-runs; X is added by the caller */
    const size_t npt = p24 ? p24_pts(na) + p24_pts(nb) : nc;
    if (p24 && (X || !bi_decimal)) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: P24 with %s\n", X ? "an added operand" : "binary limbs"); }
    if (p24 && (p24_pts(na) < p24_pts(nb) ? p24_pts(na) : p24_pts(nb)) > P24_MIN_MAX) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: P24 piece %zu x %zu limbs: min(Pa, Pb) > 10^12 points (the CRT's four limbs)\n", na, nb); }
    int logn, logR, logC; size_t q; mn_shape(npt, g, &logn, &logR, &logC, &q);
    if (logn > mn_logn_cap(g) || (p24 && logn > P24_CAP_LOG)) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: %zu %s > 2^%d points over %d nodes\n", npt, p24 ? "P24 points" : "limbs", p24 && mn_logn_cap(g) > P24_CAP_LOG ? P24_CAP_LOG : mn_logn_cap(g), g); }
    const int np = p24 ? 4 : ec_np_prod(ec_np_terms(nc, na, nb), bi_decimal, "mn_core");   /* Phase 13a P3: three primes -- decimal limbs, within the bound; Phase 15 NP: ECALC_NP=auto -- four over the bound; Phase 15 MPB: min(na, nb) under ECALC_NP_AUTO_MIN=1, else nc; P24: four (INT3: the two composed) */
    size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; dist_st.on = getenv("DIST_STATS") != 0; }
    struct mn_ctx X0 = { G, node, g, nr, R, C, n }; X0.p24 = p24;
    mdbv Xv = { X, 0, X ? X->n : 0 };
    /* A1 over shares: an operand whose transform is cached skips its redistribution, gathers and forward transforms on every
     * node (the keys are the views' mdb, offset and length -- the same decision on every node of the group).  P24: no cache (Phase 15 PC:
     * under RNS_DIST_CACHE_PARTIAL=1 cached too, keyed apart; a slot of k < np primes holds primes 0 .. k - 1: kA / kB below) */
    const int cok = cache_slots() && (!p24 || g_cache.pc);
    int ha = cok ? cache_lookup(A->m, A->off, A->len, q, 1, np, p24) : -1, hb = cok ? cache_lookup(B->m, B->off, B->len, q, 1, np, p24) : -1;
    if (!cok) slA = slB = -1; else cache_plan(&slA, &slB, ha, hb, A->m, A->off, A->len, B->m, B->off, B->len, q, 1, np, p24);
    const int kA = cache_slot_k(slA, np), kB = cache_slot_k(slB, np);   /* PC: the primes A's / B's slot holds (np for a held slot) */
    const int afull = ha >= 0 && kA >= np, bfull = hb >= 0 && kB >= np;   /* a full hit: no redistribution of that operand */
    size_t clo, chi; mdb_share(Cn, node, &clo, &chi); size_t cn = chi - clo;
    size_t tlo, thi; piece_window(Cn, node, shift, Np, &tlo, &thi); size_t tn = thi - tlo;
    if (direct && shift) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: a direct piece at a shift\n"); }   /* direct: the window is the share's first tn limbs */
    /* Phase 13a M (TASKS 1.3): MN_T_CHUNK_MB=m > 0 -- an accumulating piece's window goes through T in rounds of W = 4 m MB of
     * limbs per node (m MB per APU): round c exchanges the rows and spills of every node's chunk c of its window, and this node
     * adds its chunk into C's share at once (mn_round_add), so T and rbO are O(W) instead of O(my share of C) (35 GB per node
     * at 576 x 8e10).  K = the largest window's chunk count, the same on every node.  0 (default) or K = 1: one round. */
    size_t Wt = mn_t_chunk_limbs(); int Kt = 1;
    if (Wt) { size_t mx = 0; for (int r = 0; r < g; r++) { size_t lo, hi; piece_window(Cn, G->g0 + r, shift, Np, &lo, &hi); if (hi - lo > mx) mx = hi - lo; } Kt = mx > Wt ? (int)((mx + Wt - 1) / Wt) : 1; }
    int chunked = Kt > 1; if (chunked) direct = 0;              /* a direct piece (into the zero-filled C) goes through the rounds too: the same value added to zeros */
    struct mn_rounds rd; memset(&rd, 0, sizeof rd); rd.pr = cn == 0;
    dbig T; db_init(&T); dbig *dst = direct ? &Cn->sh : &T;
    if (!direct && tn && !chunked) db_zero_fill(&T, tn);         /* (chunked: T of one chunk, allocated after the operands, below) */
    size_t rbo_max[NR] = {0};                                 /* Phase 13a M: the result exchange's rbO per APU, the largest round */
    const uint64_t *sp[NR]; uint64_t *spill_rb[NR]; const size_t *sptab[NR]; size_t *sptab_d[NR];
    double tr[NR] = {0}, tf[NR] = {0}, tc[NR] = {0}, to[NR] = {0};
    struct gconst gc = rns_gconst(); gc.np = np;               /* NP: the CRT of this product's primes */
    uint64_t c18[EC_NP][3]; for (int p = 0; p < EC_NP; p++) p24_c18(p, c18[p]);   /* P24: 10^(18 - s) mod p (the gathers) */
#pragma omp parallel num_threads(NR)
    {
        int d = omp_get_thread_num(); struct rank_state *v = &RS[d]; hipStream_t s = v->s; int rho = g * d + me;
        HIP_CHECK(hipSetDevice(d));
        size_t rows = partn(R, nr, rho), qs = rows * C, a0 = part0(R, nr, rho);   /* my rows and sequence length (= q for a power-of-two g); a0 my first row (P24) */
        struct rdst oA, oB, oX; memset(&oA, 0, sizeof oA); memset(&oB, 0, sizeof oB); memset(&oX, 0, sizeof oX);
        struct seg *hseg = (struct seg *)malloc(8 * g * sizeof *hseg), *hsO = hseg + 6 * g, *hsI = hseg + 7 * g;
        struct seg *dseg = (struct seg *)db_pool_alloc(d, 8 * g * sizeof *hseg + 64), *dsI = dseg + 7 * g;
        size_t *cnt = (size_t *)malloc(23 * g * sizeof *cnt), *ocnt = cnt + 12 * g, *pcnt = cnt + 16 * g, *htab = cnt + 20 * g;   /* Phase 12 G: pcnt (4 g) and htab (3 g) for the spill exchange */
        oA.hs = hseg; oA.ds = dseg; oA.cnt = cnt; oB.hs = hseg + 2 * g; oB.ds = dseg + 2 * g; oB.cnt = cnt + 4 * g; oX.hs = hseg + 4 * g; oX.ds = dseg + 4 * g; oX.cnt = cnt + 8 * g;
        comm *cl = lay_get(G, d);
        /* Phase 14 P2 (TASKS 2.4): DIST_MN_SYM_SLABS=1 -- the transform's slabs (2 q) and the layered scratch (q) from the transport's
         * symmetric pool (comm_sym_alloc: 0 under TCP or an unregistered pool -> as before), kept per APU at the largest q: the
         * inter-node stage is then put from / into them, no staging.  The transpose scratch stays in pool 1 (it is local work).
         * Costs 3 q x 8 B per APU of pool on top of pool 1 (which the size-1 tiers still need); off by default (results/P214.md) */
        int symsl = mn_sym_slabs();
        if (symsl && v->ssl_q < q) { if (v->ssl) comm_sym_free(cl, v->ssl); v->ssl = (uint64_t *)comm_sym_alloc(cl, 2 * q * 8); v->ssl_q = v->ssl ? q : 0; }
        if (symsl && !gen && v->stmp_q < q) { if (v->stmp) comm_sym_free(cl, v->stmp); v->stmp = (uint64_t *)comm_sym_alloc(cl, q * 8); v->stmp_q = v->stmp ? q : 0; }
        int tmp_sym = symsl && !gen && v->stmp;
        uint64_t *cx = X ? db_pool_alloc(d, q * 8) : 0, *tmp = gen ? 0 : tmp_sym ? v->stmp : db_pool_alloc(d, q * 8);
        /* A1: the cache planes of A and B on this rank: cache_pl(slA / slB, d, p, q) for primes p < kA / kB (Phase 15 PC) */
        /* planes: xa[4] in pool 0, xb (q + 16: the CRT's carry limb) | sbuf | rbuf in pool 1 */
        uint64_t *pl = (uint64_t *)rns_dpool(d, 0, (size_t)np * q * 8), *p1 = (uint64_t *)rns_dpool(d, 1, (size_t)(3 * q + 16) * 8);
        uint64_t *xa[EC_NP] = { 0 }, *xb = p1, *sl = p1 + q + 16, *xt = sl;
        if (symsl && v->ssl) sl = v->ssl;                   /* (xt stays p1 + q + 16) */
        for (int p = 0; p < np; p++) xa[p] = pl + (size_t)p * q;
        if (comm_rank(cl) != rho || comm_size(cl) != nr) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: layered rank %d/%d, expected %d/%d\n", comm_rank(cl), comm_size(cl), rho, nr); }
        if (tmp) comm_layered_scratch(cl, tmp, q * 8);
        for (int p = 0; p < np; p++) {
            if (!v->plan[p].built || v->plan[p].logR != logR || v->plan[p].logC != logC || v->plan[p].cm != cl || v->plan[p].sl != sl) {
                if (v->plan[p].built) dist_plan_free(&v->plan[p].pl);
                dist_plan_create_shared(&v->plan[p].pl, cl, v->ctx[p], p, logR, logC, sl, sl + q);
                v->plan[p].logR = logR; v->plan[p].logC = logC; v->plan[p].built = 1; v->plan[p].cm = cl; v->plan[p].sl = sl;
            }
        }
        struct gplan gp; if (gen) gplan_build(&gp, g, rho, logR, logC, d);
        double s0 = mem_now();
        /* A: every node packs and exchanges; every rank gathers its rows for the four primes */
        if (ha < 0 && p24) {                                /* P24: A's points from its in-runs; A's sequence freed before B's (the operands' peak max(A, B)) */
            redistribute(&X0, A, &oA, d, s);
            struct p24_c18s cs; for (int p = 0; p < EC_NP; p++) for (int i = 0; i < 3; i++) cs.c[3 * p + i] = c18[p][i];
            k_gather_mn24_np<<<nblk(qs), 256, 0, s>>>(xa[0], xa[1], xa[2], xa[3], np, oA.rb, oA.tend, R, C, a0, rows, ec_mod_get(0), ec_mod_get(1), ec_mod_get(2), ec_mod_get(3), cs);   /* the four planes in one pass */
            HIP_CHECK(hipStreamSynchronize(s)); db_pool_free(d, oA.rb); oA.rb = 0;
        } else if (ha < 0) {
            redistribute(&X0, A, &oA, d, s);
            for (int p = 0; p < np; p++) k_gather_mn<<<nblk(qs), 256, 0, s>>>(xa[p], oA.rb, oA.tend, rows, C, ec_mod_get(p), 1, 1);
        } else if (!afull) {                                /* Phase 15 PC: a partial hit -- primes < kA from the slot, the others gathered as a miss */
            redistribute(&X0, A, &oA, d, s);
            for (int p = kA; p < np; p++) {
                if (p24) k_gather_mn24<<<nblk(qs), 256, 0, s>>>(xa[p], oA.rb, oA.tend, R, C, a0, rows, ec_mod_get(p), c18[p][0], c18[p][1], c18[p][2]);
                else k_gather_mn<<<nblk(qs), 256, 0, s>>>(xa[p], oA.rb, oA.tend, rows, C, ec_mod_get(p), 1, 1);
            }
            if (p24) { HIP_CHECK(hipStreamSynchronize(s)); db_pool_free(d, oA.rb); oA.rb = 0; }   /* (as the P24 miss: A's sequence freed before B's) */
            for (int p = 0; p < kA; p++) HIP_CHECK(hipMemcpyAsync(xa[p], cache_pl(slA, d, p, q), q * 8, hipMemcpyDeviceToDevice, s));
        } else for (int p = 0; p < np; p++) HIP_CHECK(hipMemcpyAsync(xa[p], cache_pl(slA, d, p, q), q * 8, hipMemcpyDeviceToDevice, s));   /* A hit: the product forms over a copy */
        if (!bfull) redistribute(&X0, B, &oB, d, s);
        if (X) { redistribute(&X0, &Xv, &oX, d, s); k_gather_mn<<<nblk(qs), 256, 0, s>>>(cx, oX.rb, oX.tend, rows, C, ec_mod_get(0), 0, 0); }
        HIP_CHECK(hipStreamSynchronize(s));
        if (oA.rb) { db_pool_free(d, oA.rb); oA.rb = 0; } if (oX.rb) { db_pool_free(d, oX.rb); oX.rb = 0; }
        double s1 = mem_now(); tr[d] = s1 - s0;
        for (int p = 0; p < np; p++) {
            const int ah = ha >= 0 && p < kA, bh = hb >= 0 && p < kB, ac = slA >= 0 && p < kA;   /* Phase 15 PC: prime p's hit / A's plane to cache (full slots: every p) */
            uint64_t *yb = slB >= 0 && p < kB ? cache_pl(slB, d, p, q) : xb;
            if (!bh && p24) k_gather_mn24<<<nblk(qs), 256, 0, s>>>(yb, oB.rb, oB.tend, R, C, a0, rows, ec_mod_get(p), c18[p][0], c18[p][1], c18[p][2]);   /* P24 */
            else if (!bh) k_gather_mn<<<nblk(qs), 256, 0, s>>>(yb, oB.rb, oB.tend, rows, C, ec_mod_get(p), 1, 1);
            if (gen) {
                if (!ah) { gen_fwd(&gp, &v->plan[p].pl, v->ctx[p], p, cl, xa[p], sl, sl + q, s); if (ac) HIP_CHECK(hipMemcpyAsync(cache_pl(slA, d, p, q), xa[p], q * 8, hipMemcpyDeviceToDevice, s)); }
                if (!bh) gen_fwd(&gp, &v->plan[p].pl, v->ctx[p], p, cl, yb, sl, sl + q, s);
                gen_inv_pw(&gp, &v->plan[p].pl, v->ctx[p], p, cl, xa[p], yb, sl, sl + q, s);
            } else {
                if (!ah) { dist_fwd(&v->plan[p].pl, xa[p], s); if (ac) HIP_CHECK(hipMemcpyAsync(cache_pl(slA, d, p, q), xa[p], q * 8, hipMemcpyDeviceToDevice, s)); }
                if (!bh) dist_fwd(&v->plan[p].pl, yb, s);
                if (dist_pw_fused()) dist_inv_pw(&v->plan[p].pl, xa[p], yb, s);   /* A6 */
                else { dist_pw(&v->plan[p].pl, xa[p], yb, s); dist_inv(&v->plan[p].pl, xa[p], s); }
            }
            HIP_CHECK(hipStreamSynchronize(s));
        }
        if (oB.rb) { db_pool_free(d, oB.rb); oB.rb = 0; }
        double s2 = mem_now(); tf[d] = s2 - s1;
        for (int p = 0; p < np; p++) {
            dim3 grid((unsigned)((C + 31) / 32), (unsigned)((rows + 31) / 32)), blk(32, 8);
            k_transpose<<<grid, blk, 0, s>>>(xa[p], xt, rows, C);
            HIP_CHECK(hipMemcpyAsync(xa[p], xt, qs * 8, hipMemcpyDeviceToDevice, s));
        }
        struct bdesc hd = { 0, 0, cx, xb, (uint32_t)qs, 0, (uint32_t)(cx ? qs : 0) };
        struct bdesc *dd = (struct bdesc *)dpool_get(&v->desc, d, sizeof hd);
        HIP_CHECK(hipMemcpyAsync(dd, &hd, sizeof hd, hipMemcpyHostToDevice, s));
        if (v->spill_cap < C) { if (v->spill) HIP_CHECK(hipFree(v->spill)); v->spill_cap = C + 16; HIP_CHECK(hipMalloc(&v->spill, v->spill_cap * 4 * 8)); }
        if (p24) {                                          /* P24: the CRT into my out-run sequence (<= 4 qs / 3 + 2 C limbs: xb and the idle slabs of pool 1, 3 q + 16) */
            struct p24_run om = p24_run_make(R, C, a0, rows, 0);
            if (p24_run_total(&om) > 3 * q + 16) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: P24 out-runs %zu limbs > pool 1's 3 q + 16 = %zu\n", p24_run_total(&om), 3 * q + 16); }
            p24_crt_launch(xa[0], xa[1], xa[2], xa[3], &gc, R, C, a0, rows, xb, v->spill, s);
            unsigned fl = p24_crt_err(s);
            if (fl) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: P24 CRT flag %u (a coefficient past 10^60: the piece's bound)\n", fl); }
        } else
        k_crt_batch<<<(unsigned)C, CRT_THREADS, 0, s>>>(xa[0], xa[1], xa[2], xa[3], dd, 0, (int)C, qs, gc, v->spill, bi_decimal);
        HIP_CHECK(hipStreamSynchronize(s));
        tc[d] = mem_now() - s2;
        /* the result's rows -> the nodes' windows: the segments of every node's window in my sequence are sent straight from
         * the CRT output (back to back in node order); the received segments of my window from the ranks (r, d) at prefix offsets.
         * Phase 13a M: in Kt rounds when chunked (each round = every node's chunk rc of its window, then this node's chunk added
         * into C by mn_round_add on one thread); Kt = 1 and the whole windows otherwise, as before */
        if (chunked) {                                    /* Phase 13a M: T (one chunk) only now -- not beside the operands' sequences */
#pragma omp barrier
#pragma omp single
          { if (tn) db_zero_fill(&T, tn < Wt ? tn : Wt); }
          HIP_CHECK(hipSetDevice(d));
        }
        for (int rc = 0; rc < Kt; rc++) {
        size_t wlo, whi; mn_win_chunk(Cn, node, shift, Np, rc, Wt, chunked, &wlo, &whi); size_t wn = whi - wlo;
        size_t *scnt = ocnt, *sdsp = ocnt + g, *rcnt = ocnt + 2 * g, *rdsp = ocnt + 3 * g, rtot = 0;
        const size_t nlim = p24 ? p24_L(n) : n;             /* the plane's limbs (P24: its out-runs tile [0, L(n))) */
        for (int r = 0; r < g; r++) { size_t lo, hi; mn_win_chunk(Cn, G->g0 + r, shift, Np, rc, Wt, chunked, &lo, &hi); if (lo > nlim) lo = nlim; if (hi > nlim) hi = nlim;
                                      if (p24) { struct p24_run om = p24_rank_run(R, C, nr, rho, 0); hsO[r].t0 = p24_seq_start(&om, lo); hsO[r].t1 = p24_seq_start(&om, hi); }
                                      else { hsO[r].t0 = seq_start(lo, R, nr, rho); hsO[r].t1 = seq_start(hi, R, nr, rho); }
                                      hsO[r].off = hsO[r].t0; }
        { size_t lo = wlo < nlim ? wlo : nlim, hi = whi < nlim ? whi : nlim, S = 0;
          for (int r = 0; r < g; r++) { int rr = g * d + r;
                                        if (p24) { struct p24_run om = p24_rank_run(R, C, nr, rr, 0); hsI[r].t0 = p24_seq_start(&om, lo); hsI[r].t1 = p24_seq_start(&om, hi); }
                                        else { hsI[r].t0 = seq_start(lo, R, nr, rr); hsI[r].t1 = seq_start(hi, R, nr, rr); }
                                        hsI[r].off = rtot; rtot += hsI[r].t1 - hsI[r].t0; if (hsI[r].t1 - hsI[r].t0 > S) S = hsI[r].t1 - hsI[r].t0; }
          seg_bytes(hsO, g, scnt, sdsp); seg_bytes(hsI, g, rcnt, rdsp);
          uint64_t *rbO = db_pool_alloc(d, (rtot + 16) * 8); if ((rtot + 16) * 8 > rbo_max[d]) rbo_max[d] = (rtot + 16) * 8;
          double s3 = mem_now();
          { int xk_ = comm_xtag_set(XK_RESULT); comm_alltoallv(G->all[d], xb, scnt, sdsp, rbO, rcnt, rdsp, s); comm_xtag_set(xk_); } comm_wait(G->all[d]);
          if (wn && rtot) { HIP_CHECK(hipMemcpyAsync(dsI, hsI, g * sizeof *hsI, hipMemcpyHostToDevice, s));
                            struct acc c = acc_db(dst, 0, wn);
                            if (p24) k_scatter_mn24<<<nblk((size_t)g * S), 256, 0, s>>>(c, wlo, rbO, dsI, g, S, R, C, nr, d);   /* P24: the out-runs */
                            else k_scatter_mn<<<nblk((size_t)g * S), 256, 0, s>>>(c, wlo, rbO, dsI, g, S, R, nr, d); }
          /* Phase 12 G: the spills exchanged exactly (was an all-gather of every rank's 4 C limbs: g x 4 C per APU, 77 GB per node at
           * 576 nodes).  My stripe j spills into limbs [R j + a, +4) of the piece, a = the first row of the next rank: node r gets the
           * blocks of the columns [j0, j1) that meet its window (spill_cols; adjacent windows share at most one block, added on each
           * side within its own limbs, the carry between them by the node scan); I receive, per source rank (r, d), the blocks that
           * meet mine, back to back in node order -- about 4 C limbs in all, whatever g (the sparse add decodes them by the table) */
          { size_t *scnt2 = pcnt, *sdsp2 = pcnt + g, *rcnt2 = pcnt + 2 * g, *rdsp2 = pcnt + 3 * g, blocks = 0, a_me = part0(R, nr, rho + 1);
            for (int r = 0; r < g; r++) { size_t lo, hi, j0, j1; mn_win_chunk(Cn, G->g0 + r, shift, Np, rc, Wt, chunked, &lo, &hi);
                                          if (p24) p24_spill_cols(lo, hi, a_me, R, C, &j0, &j1); else spill_cols(lo, hi, a_me, R, C, &j0, &j1);   /* P24: the blocks at L(R j + a) */
                                          scnt2[r] = (j1 - j0) * 4 * 8; sdsp2[r] = j0 * 4 * 8; }
            for (int r = 0; r < g; r++) { size_t j0, j1;
                                          if (p24) p24_spill_cols(wlo, whi, part0(R, nr, g * d + r + 1), R, C, &j0, &j1); else spill_cols(wlo, whi, part0(R, nr, g * d + r + 1), R, C, &j0, &j1);
                                          htab[3 * r] = j0; htab[3 * r + 1] = j1; htab[3 * r + 2] = blocks; rcnt2[r] = (j1 - j0) * 4 * 8; rdsp2[r] = blocks * 4 * 8; blocks += j1 - j0; }
            spill_rb[d] = db_pool_alloc(d, (blocks * 4 + 16) * 8); sptab_d[d] = (size_t *)db_pool_alloc(d, 3 * (size_t)g * 8 + 64);
            HIP_CHECK(hipMemcpyAsync(sptab_d[d], htab, 3 * (size_t)g * 8, hipMemcpyHostToDevice, s)); HIP_CHECK(hipStreamSynchronize(s));
            { int xk_ = comm_xtag_set(XK_SPILL); comm_alltoallv(G->all[d], v->spill, scnt2, sdsp2, spill_rb[d], rcnt2, rdsp2, s); comm_xtag_set(xk_); } comm_wait(G->all[d]);
            HIP_CHECK(hipStreamSynchronize(s));
            sp[d] = spill_rb[d]; sptab[d] = sptab_d[d]; }
          to[d] += mem_now() - s3;
          db_pool_free(d, rbO); }
        if (chunked) {
#pragma omp barrier
#pragma omp single
          mn_round_add(&rd, Cn, &T, wlo, wn, whi == thi, shift, clo, cn, sp, sptab, R, nr, C, g, spill_rb, sptab_d, tn < Wt ? tn : Wt, p24);
          HIP_CHECK(hipSetDevice(d));                         /* (the single's db calls moved this thread's device) */
        }
        }
        if (cx) db_pool_free(d, cx);
        db_pool_free(d, (uint64_t *)dseg); free(hseg); free(cnt);
        if (gen) gplan_free(&gp, d);
        if (tmp) { comm_layered_scratch(cl, 0, 0); if (!tmp_sym) db_pool_free(d, tmp); }
    }
    HIP_CHECK(hipSetDevice(0));
    /* the spills into the windows, then the carries across the nodes */
    { size_t b = (T.cap ? T.cap * 8 : 0); for (int d = 0; d < NR; d++) b += rbo_max[d]; if (b > rns_dist_tscratch_max) rns_dist_tscratch_max = b; }   /* Phase 13a M: measured T + rbO */
    double t4 = mem_now(); int co = 0, pr = 1;                /* an empty window propagates (the windows tile the piece) */
    if (chunked) {                                            /* Phase 13a M: the rounds added the chunks into C; the two carry scans */
        if (rd.kc > 1) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: %d carries out of a share in one piece (the bound is 1)\n", rd.kc); }
        share_carry_fix(G, &Cn->sh, cn, rd.kc, rd.pr);        /* C's own carries, with the propagate flag of its last add */
        share_carry_fix(G, &Cn->sh, cn, rd.cot, 0);           /* the piece's carries out of the windows that end at their share's top */
        db_free(&T);
    } else {
    if (tn && p24) db_share_add_spills_x24(dst, tn, tlo, sp, sptab, R, C, g, &co, &pr);   /* P24: the blocks at L(R j + a) */
    else if (tn) db_share_add_spills_x(dst, tn, tlo, sp, sptab, R, R / nr, C, g, &co, &pr);   /* (rows = R / nr: dbig.c decodes the unequal parts when it does not divide) */
    share_carry_fix(G, dst, tn, co, pr);
    for (int d = 0; d < NR; d++) { db_pool_free(d, spill_rb[d]); db_pool_free(d, sptab_d[d]); }
    }
    if (!direct && !chunked) {                                            /* C's share += T << (its window's offset); the carries across the nodes */
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
    if (verbose) printf("dist_mn node %d: 2^%d = 2^%d x 2^%d over %d x 4 ranks%s%s (rows %zu..%zu; %zu + %zu%s limbs at %zu, window [%zu, %zu) of share [%zu, %zu)%s): redistribute %.3f ntt %.3f crt %.3f out %.3f spills+carry %.3f total %.3f s%s%s\n",
                        node, logn, logR, logC, g, gen ? " (general map)" : "", p24 ? ", 4 primes, P24" : ec_np_auto ? (np == 4 ? ", 4 primes" : ", 3 primes") : "", R / nr, (R + nr - 1) / nr, na, nb, X ? " + x" : "", shift, tlo, thi, clo, chi, direct ? "" : ", accumulated", mr, mf, mc, mo, tcar, tt, ha >= 0 ? " [A hit]" : slA >= 0 ? " [A cached]" : "", hb >= 0 ? " [B hit]" : slB >= 0 ? " [B cached]" : "");
    if (gen && dist_st.on) {                                  /* Phase 15 G5: the general map's twiddled packs, the run so far (max over the APUs) */
        double f = 0, u = 0; for (int d = 0; d < NR; d++) { if (g_gpk[d][0] > f) f = g_gpk[d][0]; if (g_gpk[d][1] > u) u = g_gpk[d][1]; }
        printf("dist_mn node %d: general-map packs so far (%s, max over APUs): twpack %.3f unpacktw %.3f s\n", node, twrec_g() ? "DIST_TWREC_G=1" : "plain", f, u);
    }
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
/* C = A B (+ X) over the group, as one plane or as the grid of piece products.  Phase 10 A5 (the cuts of the division,
 * results/A-div.md): pieces at or above w (the high cut: oa + ob >= w) are skipped and the result is truncated to w limbs --
 * delivered directly in basis w (the piece windows clip to the shares, the carry out of the top node is dropped), so the
 * low product X Q comes out in the corrections' basis without a re-sharding shift; pieces whose limbs end at or below
 * lowcut (oa + ob + len_a + len_b <= lowcut: the A_h mu product's pieces below k + 1, B3) are skipped as on one node.  A
 * skipped (0,0) piece just leaves the first formed piece on the accumulating path (C's shares start zero-filled). */
static size_t mn_logmin(int g) { int nr = 4 * g, lg = 0; while ((1 << lg) < nr) lg++; int logmin = 2 * (5 + lg); return logmin < 20 ? 20 : logmin; }
/* Phase 13d L: mn_grid's choice, extracted unchanged (the plan printer calls it too): 1 = one plane (nc <= the group's cap), else the
 * grid split_grid_cap forms at the cap (no radix-3 planes in the mn tier) */
static int mn_grid_shape(size_t na, size_t nb, int g, int *ka, int *kb)
{
    size_t cap = (size_t)1 << mn_logn_cap(g); *ka = *kb = 1;
    if (na + nb <= cap) return 1;
    split_grid_cap(na, nb, cap, (size_t)1 << mn_logmin(g), 0, ka, kb);
    return 0;
}
/* ---- Phase 15 Batch 3 P24 (results/P2415.md): four primes at 24 digits per transform point in the mn tier ------------------------
 * MN_P24=1: a product runs P24 when its 18-digit grid's largest piece would run four primes (ec_np_for(ec_np_terms(pa + pb, pa, pb))
 * == 4, the test mn_core's ec_np_prod makes -- keep the two in step; INT3: MPB's min(pa, pb) under ECALC_NP_AUTO_MIN=1); 2: every mn product (pool 0 must
 * hold four planes: ECALC_NP=4, or auto where the run's largest group can form a four-prime piece).  0: off; default
 * is 2, since Phase 15 Batch 3 (the user's decision, 2026-09-29) -- see mn_p24_on below. */
static int mn_p24_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("MN_P24"); v = e ? atoi(e) : 2; if (v < 0 || v > 2) v = 0; } return v; }   /* default 2 since Phase 15 Batch 3 (the user's decision, 2026-09-29) */
static int mn_p24_of(size_t na, size_t nb, int g)
{
    int v = mn_p24_on(); if (!v || !bi_decimal) return 0;
    if (v >= 2) return rns_pool0_np() >= 4;
    int ka, kb; size_t pa = na, pb = nb, cap = (size_t)1 << mn_logn_cap(g);   /* INT3: the 18-digit grid's largest piece pa x pb */
    if (na + nb > cap) {
        if ((na + 31) / 32 + (nb + 31) / 32 > cap) { pa = (na + 31) / 32; pb = (nb + 31) / 32; }   /* (beyond any grid: the sizing guesses of rns_mul_dist_mn_stage; no abort here) */
        else { mn_grid_shape(na, nb, g, &ka, &kb); pa = (na + ka - 1) / ka; pb = (nb + kb - 1) / kb; }
    }
    return ec_np_for(ec_np_terms(pa + pb, pa, pb)) == 4;      /* the test mn_core's ec_np_prod makes on that piece; INT3 (Phase 15 Batch 3): pa + pb, or min(pa, pb) under ECALC_NP_AUTO_MIN=1 (MPB) */
}
/* P24: the plane cap in points (the group's, at most 2^P24_CAP_LOG: the CRT's four limbs) */
static size_t p24_cap(int g) { int c = mn_logn_cap(g); return (size_t)1 << (c < P24_CAP_LOG ? c : P24_CAP_LOG); }
/* P24's grid: split_grid_cap's rule (the fewest plane points in all, then the fewest pieces, i, j <= 32) with the pieces' points
 * p24_pts(pa) + p24_pts(pb) <= cap */
static void p24_split(size_t na, size_t nb, size_t cap, size_t minpts, int *ka, int *kb)
{
    size_t best = 0; *ka = *kb = 0;
    for (int i = 1; i <= 32; i++) for (int j = 1; j <= 32; j++) {
        size_t pa = (na + i - 1) / i, pb = (nb + j - 1) / j, np_ = p24_pts(pa) + p24_pts(pb);
        if (np_ > cap) continue;
        size_t pts = plane_pts(np_, 0); if (pts < minpts) pts = minpts;
        size_t cost = (size_t)i * j * pts;
        if (!*ka || cost < best || (cost == best && i * j < *ka * *kb)) { best = cost; *ka = i; *kb = j; }
    }
    if (!*ka) { ec_fatal(EC_RC_FATAL, "p24_split: %zu x %zu limbs at %zu points\n", na, nb, cap); }
}
/* P24's mn_grid_shape: 1 = one plane, else the grid */
static int p24_grid_shape(size_t na, size_t nb, int g, int *ka, int *kb)
{
    size_t cap = p24_cap(g); *ka = *kb = 1;
    if (p24_pts(na) + p24_pts(nb) <= cap) return 1;
    p24_split(na, nb, cap, (size_t)1 << mn_logmin(g), ka, kb);
    return 0;
}
static void mn_grid(mdb *Cm, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t lowcut, size_t w)
{
    int g = G->g, node = g_node_of(G), verbose = getenv("RNS_VERBOSE") != 0;
    size_t na = A->len, nb = B->len, nc = na + nb, N = nc + (X ? 1 : 0), cap = (size_t)1 << mn_logn_cap(g);
    if (!na || !nb) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: a zero operand\n"); }
    if (X && (w < N || lowcut)) { ec_fatal(EC_RC_FATAL, "rns_mul_dist_mn: the added operand with a cut\n"); }
    int trunc = w < N; if (trunc) N = w;                       /* the result in basis w: the limbs at or above w are never formed */
    double t0 = mem_now();
    mdb Cn; memset(&Cn, 0, sizeof Cn); Cn.N = N; Cn.g0 = G->g0; Cn.g = g; db_init(&Cn.sh);
    size_t clo, chi; mdb_share(&Cn, node, &clo, &chi); size_t cn = chi - clo;
    db_zero_fill(&Cn.sh, cn);
    struct mn_times tm; memset(&tm, 0, sizeof tm);
    g_top_ok = trunc;
    int ka = 1, kb = 1, formed = 0, skipped = 0;
    int NS = 0, pin = g_cache.pin_next, fs[DIST_CACHE_MAX], nf = 0;
    g_cache_mn = 1;
    const int p24 = mn_p24_of(na, nb, g);                     /* Phase 15 Batch 3 P24 (MN_P24): the same on every node of the group */
    const int pc = pc_on(); int pck = 0;                      /* Phase 15 PC (RNS_DIST_CACHE_PARTIAL=1): the slots from the block pool per grid product (cache_pc_take, below) */
    if (pc && pin) { g_cache.pin_next = 0; pin = 0; }         /* (PC: pins dropped -- a pool slot does not outlive its product) */
    if (cache_slots() && (nc > cap || pin) && !p24 && !pc) {         /* the slots: every node must hold the same count -- the group's minimum of what each could allocate (P24: no cache) */
        int mine = cache_avail(); int agreed = DIST_CACHE_MAX - (int)grp_max(G, (size_t)(DIST_CACHE_MAX - mine)); if (agreed < g_cache.navail) g_cache.navail = agreed;   /* (a collective: the condition is group-wide) */
        NS = g_cache.navail; for (int i = 0; i < NS; i++) if (!g_cache.s[i].pinned) fs[nf++] = i;
    }
    if (p24 && pin) g_cache.pin_next = 0;                     /* P24: a pin request is dropped (nothing is cached; the next product misses) */
    int one = p24 ? p24_grid_shape(na, nb, g, &ka, &kb) : mn_grid_shape(na, nb, g, &ka, &kb);   /* Phase 13d L: the decision extracted (unchanged); P24: on the points */
    if (one && p24) { if (nc > lowcut) { mn_core(&Cn, A, B, 0, G, 0, 1, &tm, -1, -1, 1); formed = 1; if (X) mdb_add_shifted(&Cn, X, 0, G); } else skipped = 1; }   /* P24: X added afterwards */
    else if (one) { if (nc > lowcut) { mn_core(&Cn, A, B, X, G, 0, 1, &tm, -1, pin && nf ? fs[0] : -1, 0); formed = 1; if (pin && nf) { g_cache.s[fs[0]].pinned = 1; g_cache.pin_next = 0; } } else skipped = 1; }
    else {
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb;
        if (pc && cache_slots()) {                                /* Phase 15 PC: this product's slots from the pool, at its largest piece's plane (every node the same: a collective) */
            size_t npt0 = p24 ? p24_pts(pa) + p24_pts(pb) : pa + pb; int ln0, lr0, lc0; size_t q0; mn_shape(npt0, g, &ln0, &lr0, &lc0, &q0);
            int np0 = p24 ? 4 : ec_np_for(ec_np_terms(pa + pb, pa, pb));
            NS = cache_pc_take(G, na, nb, A, B, &Cn, q0, np0, &pck); nf = NS; for (int i = 0; i < NS; i++) fs[i] = i;
        }
        const int lng = pc && kb > ka;                           /* Phase 15 PC: the loop along the longer axis -- A's pieces outer, the slots' roles swapped (CX15 3.3) */
        int nB = pin ? (kb < nf ? kb : nf) : 0, nA = pin ? nf - nB : (ka < nf - 1 ? ka : nf - 1); if (nA < 0) nA = 0;   /* the slots as in mul_grid */
        if (!pin) nB = nf - nA;
        if (lng) { nB = kb < nf - 1 ? kb : nf - 1; if (nB < 0) nB = 0; nA = nf - nB; }   /* (PC: B's pieces j < nB inside the loop, A's piece i in slot nB + i % nA) */
        mdbv *av = (mdbv *)malloc((ka + kb) * sizeof *av), *bv = av + ka;
        for (int i = 0; i < ka; i++) av[i] = mdb_view(A->m, A->off + (size_t)i * pa, na - (size_t)i * pa < pa ? na - (size_t)i * pa : pa, G);
        for (int j = 0; j < kb; j++) bv[j] = mdb_view(B->m, B->off + (size_t)j * pb, nb - (size_t)j * pb < pb ? nb - (size_t)j * pb : pb, G);
        const int K_out = lng ? ka : kb, K_in = lng ? kb : ka;
        for (int o_ = 0; o_ < K_out; o_++) for (int n_ = 0; n_ < K_in; n_++) {
            const int i = lng ? o_ : n_, j = lng ? n_ : o_;       /* (default: j outer, i inner, as before) */
            size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
            if (!av[i].len || !bv[j].len) continue;                                   /* nothing */
            if (grid_piece_skipped(oa, ob, av[i].len, bv[j].len, lowcut, w)) { skipped++; continue; }   /* nothing of it below w, or all of it below the low cut */
            int sa = pin ? (i < nA ? fs[nB + i] : -1) : (i < nA ? fs[i] : -1), sb = pin ? (j < nB ? fs[j] : -1) : (nB ? fs[nA + j % nB] : -1);
            if (lng) { sb = j < nB ? fs[j] : -1; sa = nA ? fs[nB + i % nA] : -1; }
            struct mn_times t_b = tm; double tp = mem_now(); g_cache_last[0] = g_cache_last[1] = 0;
            if (cache_trace_on() && dist_st.on) dist_st.t_local1 = dist_st.t_local2 = dist_st.t_tw = dist_st.t_pack = dist_st.t_xfer = dist_st.t_a2a = dist_st.t_total = 0;
            mn_core(&Cn, &av[i], &bv[j], 0, G, oa + ob, oa + ob == 0 && !formed, &tm, sa, sb, p24);   /* the first piece at shift 0 straight into the zero-filled C */
            if (cache_trace_on()) {                                   /* Phase 15 CX: the per-piece line (instrumentation) */
                static const char *st[3] = { "-", "cached", "HIT" };
                printf("cache_trace node %d: piece (%d, %d) of %d x %d, %zu + %zu limbs at %zu, slots %d: A %s B %s | %.3f s: redistribute %.3f ntt %.3f crt %.3f out %.3f carry %.3f",
                       node, i, j, ka, kb, av[i].len, bv[j].len, oa + ob, NS, st[g_cache_last[0]], st[g_cache_last[1]], mem_now() - tp,
                       tm.redistribute - t_b.redistribute, tm.ntt - t_b.ntt, tm.crt - t_b.crt, tm.out - t_b.out, tm.carry - t_b.carry);
                if (dist_st.on) { printf(" | ntt parts rows %.3f cols %.3f pack %.3f xfer %.3f a2a %.3f", dist_st.t_local1 / 4, dist_st.t_local2 / 4, dist_st.t_pack / 4, dist_st.t_xfer / 4, dist_st.t_a2a / 4);
                                  dist_st.t_local1 = dist_st.t_local2 = dist_st.t_tw = dist_st.t_pack = dist_st.t_xfer = dist_st.t_a2a = dist_st.t_total = 0; }
                if (pc) printf(" | partial: %d prime%s per slot%s", pck, pck == 1 ? "" : "s", p24 ? ", P24" : "");   /* Phase 15 PC */
                printf("\n");
            }
            formed++;
        }
        free(av);
        cache_pc_give();                                          /* Phase 15 PC: the partial slots back to the pool (before X's add and the norm) */
        if (pin) { for (int j = 0; j < nB; j++) g_cache.s[fs[j]].pinned = 1; g_cache.pin_next = 0; }
        if (X) mdb_add_shifted(&Cn, X, 0, G);
    }
    g_top_ok = 0;
    cache_drop(0);
    if (verbose || (getenv("ECALC_VERBOSE") && (ka * kb > 1 || skipped))) printf("dist_mn node %d: %zu x %zu limbs over %d x 4 ranks (cap 2^%d%s): %d x %d pieces, %d formed, %d skipped%s%s (cache %d slots%s: %zu hits, %zu misses)\n", node, na, nb, g, p24 ? (int)log2((double)p24_cap(g)) : mn_logn_cap(g), p24 ? " points, P24" : "", ka, kb, formed, skipped, trunc ? " (low product)" : "", lowcut ? " (low cut)" : "", NS,
                                                                            pc ? (pck == 1 ? " of 1 prime, partial" : pck == 2 ? " of 2 primes, partial" : pck == 3 ? " of 3 primes, partial" : NS ? " of 4 primes, partial" : ", partial") : "", g_cache.hits, g_cache.misses);
    mdb_norm(&Cn, G, N);
    if (Cm->sh.cap) db_free(&Cm->sh);
    *Cm = Cn;
    if (verbose) printf("dist_mn node %d: %zu + %zu%s limbs -> %zu (share %zu): redistribute %.3f ntt %.3f crt %.3f out %.3f spills+carry %.3f, total %.3f s\n",
                        node, na, nb, X ? " + x" : "", Cn.n, cn, tm.redistribute, tm.ntt, tm.crt, tm.out, tm.carry, mem_now() - t0);
}
static void mdb_empty(mdb *Cm, mn_group *G) { if (Cm->sh.cap) db_free(&Cm->sh); memset(Cm, 0, sizeof *Cm); db_init(&Cm->sh); Cm->g0 = G->g0; Cm->g = G->g; }
void rns_mul_dist_mn(mdb *Cm, const mdb *A, const mdb *B, const mdb *X, mn_group *G)
{
    mdbv a = { A, 0, A->n }, b = { B, 0, B->n };
    mn_grid(Cm, &a, &b, X, G, 0, (size_t)-1);
}
void rns_mul_dist_mn_v(mdb *Cm, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w) { mn_grid(Cm, A, B, X, G, 0, w); }
void rns_mul_dist_mn_cut(mdb *Cm, const mdb *A, const mdb *B, mn_group *G, size_t lowcut, size_t highcut)
{
    if (!highcut) { mdb_empty(Cm, G); return; }
    mdbv a = mdb_view(A, 0, highcut, G), b = mdb_view(B, 0, highcut, G);   /* only the limbs below the high cut can reach the result */
    if (!a.len || !b.len) { mdb_empty(Cm, G); return; }
    mn_grid(Cm, &a, &b, 0, G, lowcut, highcut);
}
void rns_mul_dist_mn_cut_v(mdb *Cm, const mdbv *A, const mdbv *B, mn_group *G, size_t lowcut, size_t highcut)   /* int15k (DM_MN_LEAN): the cuts on views */
{
    if (!highcut || !A->len || !B->len) { mdb_empty(Cm, G); return; }
    mdbv a = mdb_view(A->m, A->off, A->len < highcut ? A->len : highcut, G), b = mdb_view(B->m, B->off, B->len < highcut ? B->len : highcut, G);
    if (!a.len || !b.len) { mdb_empty(Cm, G); return; }
    mn_grid(Cm, &a, &b, 0, G, lowcut, highcut);
}
void rns_mul_low_mn(mdb *Cm, const mdb *A, const mdb *B, mn_group *G, size_t w) { rns_mul_dist_mn_cut(Cm, A, B, G, 0, w); }
/* the grid (ka x kb pieces of ceil(na/ka) + ceil(nb/kb) limbs) mn_grid forms for na x nb limbs over G; 1 x 1 = one plane (tests) */
void rns_mul_dist_mn_shape(size_t na, size_t nb, mn_group *G, int *ka, int *kb)
{
    if (mn_p24_of(na, nb, G->g)) { p24_grid_shape(na, nb, G->g, ka, kb); return; }   /* P24 (MN_P24): the grid on the points */
    size_t cap = (size_t)1 << mn_logn_cap(G->g);
    if (na + nb <= cap) { *ka = *kb = 1; return; }
    split_grid_cap(na, nb, cap, (size_t)1 << mn_logmin(G->g), 0, ka, kb);   /* mn tier: no radix-3 planes (Phase 11 P) */
}
/* ---- Phase 13d L (PLAN 32, row L): the plan printer's view of the two tiers (mn_plan.h; MN_PLAN_ONLY, mn_plan.c) ----------------
 * The same decisions the products take -- db_grid_shape / split_grid (with b_fits on the pools rns_init would make:
 * rns_dist_plan_pools) for the dist tier, mn_grid_shape for the mn tier, grid_piece_skipped for the cuts -- over the piece loop
 * of mul_grid / mn_grid (a piece past the operand's end is empty and not counted, as there), without a device.  The one thing
 * the real loop sees that the plan does not: a piece view whose top limbs are zero is normalised shorter (a size, never a count,
 * unless a whole piece is zero). */
void rns_dist_plan_pools(size_t pool0_bytes, size_t pool1_bytes) { g_plan_pool[0] = pool0_bytes; g_plan_pool[1] = pool1_bytes; }
void rns_dist_plan_cap_test(int logn) { g_cap_test = logn; }
/* Phase 15 TC (results/TC15.md): the plan's line for the mn transform cache -- where it is first wanted (the first mn grid product;
 * RNS_DIST_CACHE_HOLD also wants it at the reciprocal's last doubling), the slots the default takes if hipMemGetInfo lets it (outside
 * every budget) and what RNS_DIST_CACHE_FIT would take against node_bytes (the layout's node: plane pools + arena + tables + the init
 * host, binsplit_node_bytes -- the run's own check uses its measured init peak where that is larger) */
void rns_dist_cache_plan(int size, int pool_log, int np0, long grids_tree, long grids_recip, long grids_div, double node_bytes)
{
    int w = cache_slots_of(1), hold = getenv("RNS_DIST_CACHE_HOLD") && atoi(getenv("RNS_DIST_CACHE_HOLD"));
    const char *first = grids_tree ? "the tree" : grids_recip ? "the reciprocal" : grids_div ? "the division" : hold ? "the reciprocal's last doubling (RNS_DIST_CACHE_HOLD)" : 0;
    int ld = dist_logn_max(), lf = pool_log > 0 && pool_log < ld ? pool_log : ld;
    double sd = (double)np0 * ((size_t)1 << (ld - 2)) * 8, sf = (double)np0 * ((size_t)1 << (lf - 2)) * 8;   /* one slot per APU: the default's, FIT's */
    double budget = (getenv("ECALC_NODE_GB") ? atof(getenv("ECALC_NODE_GB")) : 480.0) * 1e9, room = budget - node_bytes - cache_fit_reserve();
    int nf = room > 0 ? (int)(room / (NR * sf)) : 0; if (nf > w) nf = w;
    if (!w || !first) { printf("plan cache  the mn transform cache (RNS_DIST_CACHE_MN=%d): %s -- nothing allocated; node %.2f GB\n", w, !w ? "off" : "not wanted (no mn grid product)", node_bytes / 1e9); return; }
    if (pc_on()) {                                            /* Phase 15 PC: the partial slots come from the block pool per grid product -- decided at run time by the pool's free bytes */
        printf("plan cache  the mn transform cache (RNS_DIST_CACHE_PARTIAL=1, RNS_DIST_CACHE_MN=%d): wanted first in %s (mn grids: tree %ld, recip %ld, div %ld); per grid product, slots from the block pool"
               " (the arena, inside the node's %.2f GB: nothing added): P = (the pool's free bytes - the product's need - %.2f GB margin per APU) / (q x 8), at most what the free extents hold;"
               " one prime at the cap 2^%d limbs = %.2f GiB per APU (%.2f GB per node); P >= %d primes: full slots, else 1 slot of P primes; P24 products cached too (MN_P24=%s); the grid's loop along its longer axis"
               " (mn_model.py --cache-partial models the primes per phase)\n",
               w, first, grids_tree, grids_recip, grids_div, node_bytes / 1e9, getenv("RNS_DIST_CACHE_PARTIAL_MARGIN_GB") ? atof(getenv("RNS_DIST_CACHE_PARTIAL_MARGIN_GB")) : 0.5,
               lf - 2, ((size_t)1 << (lf - 2)) * 8 / 1073741824.0, NR * ((size_t)1 << (lf - 2)) * 8 / 1e9, np0, getenv("MN_P24") ? getenv("MN_P24") : "0");
        return;
    }
    printf("plan cache  the mn transform cache (RNS_DIST_CACHE_MN=%d, RNS_DIST_CACHE_FIT=%d): wanted first in %s (mn grids: tree %ld, recip %ld, div %ld), held to the division's end"
           " | default: %d slot%s x %.2f GiB per APU (2^%d limbs x %d primes) = %.2f GB per node when hipMemGetInfo less RNS_DIST_CACHE_MARGIN_GB allows it, in no budget: node %.2f -> %.2f GB"
           " | FIT: %.2f GiB per APU (2^%d x %d), room %.2f GB (ECALC_NODE_GB %.0f less the node and the reserve %.0f) -> %d slot%s = %.2f GB, node %.2f GB\n",
           w, cache_fit_on(), first, grids_tree, grids_recip, grids_div, w, w > 1 ? "s" : "", sd / 1073741824.0, ld - 2, np0, w * NR * sd / 1e9, node_bytes / 1e9, (node_bytes + w * NR * sd) / 1e9,
           sf / 1073741824.0, lf - 2, np0, room / 1e9, budget / 1e9, cache_fit_reserve() / 1e9, nf, nf == 1 ? "" : "s", nf * NR * sf / 1e9, (node_bytes + nf * NR * sf) / 1e9);
}
static void plan_pieces(size_t na, size_t nb, int ka, int kb, size_t lowcut, size_t w, struct rns_grid_plan *p)
{
    size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb;
    p->pa = pa; p->pb = pb; p->formed = p->skipped = 0;
    for (int j = 0; j < kb; j++) for (int i = 0; i < ka; i++) {
        size_t oa = (size_t)i * pa, ob = (size_t)j * pb;
        if (oa >= na || ob >= nb) continue;                                            /* an empty piece (mul_grid: !ai.n) */
        size_t la = na - oa < pa ? na - oa : pa, lb = nb - ob < pb ? nb - ob : pb;
        if (grid_piece_skipped(oa, ob, la, lb, lowcut, w)) p->skipped++; else { p->formed++; p->formed4 += ec_np_for(ec_np_terms(la + lb, la, lb)) == 4; }   /* NP: the piece's primes (MPB: its terms as mn_core / dist_core count them) */
    }
}
void rns_dist_db_plan(size_t na, size_t nb, size_t lowcut, size_t w, struct rns_grid_plan *p)
{
    memset(p, 0, sizeof *p);
    g_cache_mn = 0;                                                                    /* as mul_grid sets it before deciding */
    p->logcap = dist_logn_max(); p->cap = dist_cap();
    p->one = db_grid_shape(na, nb, &p->ka, &p->kb);
    if (p->one) { p->pa = na; p->pb = nb; p->formed = 1; p->formed4 = ec_np_for(ec_np_terms(na + nb, na, nb)) == 4; }   /* one plane: formed whole, whatever the cuts (mul_grid) */
    else plan_pieces(na, nb, p->ka, p->kb, lowcut, w, p);
    size_t pc = p->pa + p->pb; int T, lk;
    p->form_b = b_grid_on() && b_fits(pc);
    p->np = ec_np_for(ec_np_terms(pc, p->pa, p->pb));                                  /* Phase 15 NP: the piece's primes (ECALC_NP=auto; MPB: min under ECALC_NP_AUTO_MIN=1) */
    if (p->form_b) { p->pts = b_len(pc, &T, &lk); p->plane_bytes = 2.0 * p->np * p->pts * 8; }   /* B: two planes of n on each prime's APU */
    else { p->pts = plane_pts(pc, dist_r3()); p->plane_bytes = (double)p->np * p->pts * 8; }       /* C: n / 4 points per prime on each of the four APUs */
}
void rns_dist_mn_plan(size_t na, size_t nb, int g, int has_x, size_t lowcut, size_t w, struct rns_grid_plan *p)
{
    memset(p, 0, sizeof *p);
    size_t nc = na + nb;
    if ((p->p24 = mn_p24_of(na, nb, g))) {                                            /* P24 (MN_P24): mn_grid's P24 decision -- the grid on the points, four primes */
        p->cap = p24_cap(g); p->logcap = 0; while (((size_t)1 << p->logcap) < p->cap) p->logcap++;
        p->one = p24_grid_shape(na, nb, g, &p->ka, &p->kb);
        if (p->one) { p->pa = na; p->pb = nb; if (nc > lowcut) p->formed = 1; else p->skipped = 1; }
        else plan_pieces(na, nb, p->ka, p->kb, lowcut, w, p);
        p->formed4 = p->formed;
        int logn, logR, logC; size_t q; mn_shape(p24_pts(p->pa) + p24_pts(p->pb), g, &logn, &logR, &logC, &q);
        p->pts = (size_t)1 << logn; p->logR = logR; p->logC = logC; p->np = 4; p->plane_bytes = 4.0 * 4 * q * 8;
        (void)has_x; return;
    }
    p->logcap = mn_logn_cap(g); p->cap = (size_t)1 << p->logcap;
    p->one = mn_grid_shape(na, nb, g, &p->ka, &p->kb);
    if (p->one) { p->pa = na; p->pb = nb; if (nc > lowcut) { p->formed = 1; p->formed4 = ec_np_for(ec_np_terms(nc, na, nb)) == 4; } else p->skipped = 1; }   /* mn_grid: one plane, skipped when all of it is below the low cut */
    else plan_pieces(na, nb, p->ka, p->kb, lowcut, w, p);
    (void)has_x;
    int logn, logR, logC; size_t q; mn_shape(p->pa + p->pb, g, &logn, &logR, &logC, &q);
    p->pts = (size_t)1 << logn; p->logR = logR; p->logC = logC;
    p->np = ec_np_for(ec_np_terms(p->pa + p->pb, p->pa, p->pb));                      /* Phase 15 NP: the piece's primes (ECALC_NP=auto; MPB: min under ECALC_NP_AUTO_MIN=1) */
    p->plane_bytes = 4.0 * p->np * q * 8;                                              /* per node: q limbs per prime on each of its four APUs (mn_core's xa[]) */
}
/* Phase 12 G: the block-pool bytes per device at the peak of the product C = A B (+ X) of na x nb limbs over g nodes, on a
 * node whose shares of A, B and C are share_a, share_b, share_c limbs: the largest piece of the grid at the group's cap
 * (mn_grid), with mn_core's buffers -- the received sequences rbA, rbB, rbX (<= qs = my rows x C each), the packed part
 * sb (my share's limbs on APU d's ranks: about a quarter of it), cx and tmp (q each), then the result exchange's rbO (my
 * window's part), the exact spills (~ 4 C limbs) and the window temporary T (a quarter of the window, at most share_c).
 * Not counted: the plane pools (their init size: q <= 2^(min(31, pool_log) - 2) limbs per prime, whatever g and n) and the
 * transform cache's slots (hipMalloc'd as the free memory allows).  binsplit.c's arena layout (tree_need_dev) and
 * mem_model.py (the same formula in Python) use it; *pieces = ka x kb (1 = one plane, the M3 path). */
size_t rns_mul_dist_mn_scratch(size_t na, size_t nb, int has_x, int g, size_t share_a, size_t share_b, size_t share_c, int *pieces)
{
    if (!na || !nb || g < 2) { if (pieces) *pieces = 0; return 0; }
    if (mn_p24_of(na, nb, g)) {                               /* P24 (MN_P24): the grid on the points; the sequences are runs of limbs (rl per column at most); A's
                                                               * received sequence is freed before B's redistribution; X is added afterwards (not in the CRT) */
        int ka = 1, kb = 1; p24_grid_shape(na, nb, g, &ka, &kb);
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb, nc = pa + pb; if (pieces) *pieces = ka * kb;
        int logn, logR, logC; size_t q; mn_shape(p24_pts(pa) + p24_pts(pb), g, &logn, &logR, &logC, &q);
        int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rows = (R + nr - 1) / nr, rl = rows + rows / 3 + 2, qsl = rl * C;
#define RT24(len) ((((p24_pts(len)) + R - 1) / R + 1) * rl < qsl ? (((p24_pts(len)) + R - 1) / R + 1) * rl : qsl)
        size_t va = share_a < pa ? share_a : pa, vb = share_b < pb ? share_b : pb;
        size_t sba = va / 4 + va / R * (size_t)g + 2 * (size_t)g * rl, sbb = vb / 4 + vb / R * (size_t)g + 2 * (size_t)g * rl;
        size_t win = share_c < nc ? share_c : nc, tmp = is_pow2(g) ? q : 0;
        { size_t Wt = mn_t_chunk_limbs(); if (Wt && win > Wt) win = Wt; }
        size_t ia = RT24(pa) + sba, ib = RT24(pb) + sbb, peak1 = (ia > ib ? ia : ib) + tmp + 16 * (size_t)g;
        size_t peak2 = win / 4 + 2 * (size_t)g * rl + 4 * C + 4 * (size_t)g + tmp;
#undef RT24
        return (peak1 > peak2 ? peak1 : peak2) * 8 + 32 * (size_t)g * 8 + ((win + 3) / 4 + 4095) / 4096 * 4096 * 8;
    }
    size_t cap = (size_t)1 << mn_logn_cap(g); int ka = 1, kb = 1;
    if (na + nb > cap) split_grid_cap(na, nb, cap, (size_t)1 << mn_logmin(g), 0, &ka, &kb);
    size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb, nc = pa + pb; if (pieces) *pieces = ka * kb;
    int logn, logR, logC; size_t q; mn_shape(nc, g, &logn, &logR, &logC, &q);
    int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rows = (R + nr - 1) / nr, qs = rows * C;
#define RT(len) ((((len) + R - 1) / R + 1) * rows < qs ? (((len) + R - 1) / R + 1) * rows : qs)   /* my sequence over an operand of len limbs */
    size_t va = share_a < pa ? share_a : pa, vb = share_b < pb ? share_b : pb;                  /* my part of a piece view */
    size_t sb = (va > vb ? va : vb) / 4 + 2 * (size_t)g * rows;                                 /* the packed part on APU d's ranks */
    int xin = has_x && ka * kb == 1;                                                                /* X rides in the CRT only on one plane; a grid adds it afterwards (mdb_add_shifted: O(chunk)) */
    size_t win = share_c < nc ? share_c : nc, xq = xin ? q : 0, tmp = is_pow2(g) ? q : 0;
    { size_t Wt = mn_t_chunk_limbs(); if (Wt && win > Wt) win = Wt; }                             /* Phase 13a M: MN_T_CHUNK_MB -- rbO and T (and mdb_add_shifted's T) hold one chunk */
    size_t peak1 = RT(pa) + RT(pb) + (xin ? RT(nc) : 0) + sb + xq + tmp + 16 * (size_t)g;          /* the operands in: the sequences + one packed part + cx, tmp */
    size_t peak2 = win / 4 + 2 * (size_t)g * rows + 4 * C + 4 * (size_t)g + xq + tmp;              /* the result out: rbO (my window's part), the spills, cx and tmp still held */
    size_t bytes = (peak1 > peak2 ? peak1 : peak2) * 8 + 32 * (size_t)g * 8 + ((win + 3) / 4 + 4095) / 4096 * 4096 * 8;   /* + the tables, + T's quarter (the accumulating pieces) */
#undef RT
    return bytes;
}
/* B7ACCT (results/B7ACCT.md): the layered communicator's v-exchange scratch that one product's general-map transform grows on each APU
 * thread (comm_layered.c need_vslot / need_vtmp: hipMalloc'd OUTSIDE the block pool and the layout, grow-only, freed only when the
 * communicator is destroyed -- mn.c groups_finalize, the run's end).  gen_fwd / gen_inv_pw post one alltoallv per chunk k < K (gplan_build's
 * K) over the 4 g ranks rho = g d + r.  For APU d of node r the exchange's intra-stage receive A (what the node's four APUs send to the ranks
 * (d, 0..g-1)) and inter-stage receive B, in points (x 8 bytes):
 *   forward  A = sum_dd rk(g dd + r, k) x sum_r' cols(g d + r'),   B = cols(rho) x sum_sigma rk(sigma, k)
 *   inverse  A = sum_dd cols(g dd + r) x sum_r' rk(g d + r', k),   B = rk(rho, k) x C
 * (rk(rho, k) = rank rho's rows of chunk k, cols(rho) = its columns; partn).  The send slabs are back to back in rank order (fsd / frd), so
 * the reorder area x0 is never taken (need_vtmp stays unused at depth 2).  COMM_ALLTOALLV_DEPTH >= 2 (the default): two slots of
 * A + max(A, B) + 4 bytes, both counted at the largest exchange (each slot grows to its own exchanges' max: at most one row of chunk
 * rounding high); depth 1: one area of 2 A + B + 4.  Each area is taken in whole 2 MiB (hipMalloc's granularity; conservative).  Bytes per
 * APU thread, the max over the group's APU threads.  0 when the group's transform is ntt_dist's (g a power of two and DIST_GEN unset):
 * its exchanges take the caller's scratch (tmp, from the block pool).  The plane is the grid's largest piece, as rns_mul_dist_mn_scratch. */
static size_t vslot_shape(int logR, int logC, int g)
{
    int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rmin = R / nr;
    int K = getenv("DIST_CHUNKS") ? atoi(getenv("DIST_CHUNKS")) : 8; if (K < 1) K = 1; if (K > 16) K = 16;   /* gplan_build's K */
    while (K > 1 && rmin / K < 32) K--;
    const char *e = getenv("COMM_ALLTOALLV_DEPTH"); int depth = e ? atoi(e) : 2;   /* comm_layered.c lay_opts */
    size_t *rk = (size_t *)malloc((size_t)nr * sizeof(size_t)), colcol[4] = { 0, 0, 0, 0 }, need = 0;
    for (int d = 0; d < 4; d++) for (int r = 0; r < g; r++) colcol[d] += partn(C, nr, g * d + r);
    for (int k = 0; k < K; k++) {
        size_t tot = 0, rkcol[4] = { 0, 0, 0, 0 };
        for (int s = 0; s < nr; s++) { size_t rr = partn(R, nr, s); rk[s] = rr * (k + 1) / K - rr * k / K; tot += rk[s]; rkcol[s / g] += rk[s]; }
        for (int r = 0; r < g; r++) {
            size_t srk = 0, scol = 0; for (int dd = 0; dd < 4; dd++) { srk += rk[g * dd + r]; scol += partn(C, nr, g * dd + r); }
            for (int d = 0; d < 4; d++) {
                int rho = g * d + r;
                size_t Af = srk * colcol[d] * 8, Bf = partn(C, nr, rho) * tot * 8, Ai = scol * rkcol[d] * 8, Bi = rk[rho] * C * 8;
                size_t nf = depth >= 2 ? Af + (Af > Bf ? Af : Bf) + 4 : 2 * Af + Bf + 4, ni = depth >= 2 ? Ai + (Ai > Bi ? Ai : Bi) + 4 : 2 * Ai + Bi + 4;
                if (nf > need) need = nf; if (ni > need) need = ni;
            }
        }
    }
    free(rk);
    need = (need + ((size_t)2 << 20) - 1) / ((size_t)2 << 20) * ((size_t)2 << 20);
    return depth >= 2 ? 2 * need : need;
}
size_t rns_mul_dist_mn_vslot(size_t na, size_t nb, int g)
{
    if (!na || !nb || g < 2 || (is_pow2(g) && !dist_gen_forced())) return 0;
    int logn, logR, logC; size_t q;
    if (mn_p24_of(na, nb, g)) {                               /* P24 (MN_P24): the grid on the points (rns_mul_dist_mn_scratch's P24 branch) */
        int ka = 1, kb = 1; p24_grid_shape(na, nb, g, &ka, &kb);
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb; mn_shape(p24_pts(pa) + p24_pts(pb), g, &logn, &logR, &logC, &q);
    } else {
        size_t cap = (size_t)1 << mn_logn_cap(g); int ka = 1, kb = 1;
        if (na + nb > cap) split_grid_cap(na, nb, cap, (size_t)1 << mn_logmin(g), 0, &ka, &kb);
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb; mn_shape(pa + pb, g, &logn, &logR, &logC, &q);
    }
    return vslot_shape(logR, logC, g);
}
/* Phase 14 P2 (results/P214.md): the SHMEM transport's staging for the same product, limbs per APU thread = the send + receive of
 * its largest staged exchange (comm_shmem.c stages every buffer outside its symmetric pool, per exchange, released after it; the
 * four APU threads run the same exchange at once, so the pool holds 4 of these): the result exchange (xb -> rbO: my rows of the
 * piece RT(nc), and my window's part win / 4), the redistribution of A or B (my share's part va / 4 + 2 g rows out, my rows RT(pa)
 * in), the transform's inter-node stage (q / 4 each way).  mem_model.py mn_stage is the same formula; measured to the MiB at 1e8 -
 * 1e10 on 2 nodes and 1e9 - 1e10 on 4 processes.  *qmax = the product's plane q (DIST_MN_SYM_SLABS keeps 3 q per APU). */
size_t rns_mul_dist_mn_stage(size_t na, size_t nb, int g, size_t share_a, size_t share_b, size_t share_c, size_t *qmax)
{
    if (qmax) *qmax = 0;
    if (!na || !nb || g < 2) return 0;
    const char *er0 = getenv("COMM_SHMEM_ROUND_MB"); size_t Rl0 = er0 && atof(er0) > 0 ? (size_t)(atof(er0) * 1048576.0) / 8 : 0;
    if (mn_p24_of(na, nb, g) && p24_pts(na / 32 + 1) + p24_pts(nb / 32 + 1) <= p24_cap(g)) {   /* P24 (MN_P24): the same exchanges over runs of limbs (as rns_mul_dist_mn_scratch's P24 branch) */
        int ka = 1, kb = 1; p24_grid_shape(na, nb, g, &ka, &kb);
        size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb, nc = pa + pb;
        int logn, logR, logC; size_t q; mn_shape(p24_pts(pa) + p24_pts(pb), g, &logn, &logR, &logC, &q); if (qmax) *qmax = q;
        int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rows = (R + nr - 1) / nr, rl = rows + rows / 3 + 2, qsl = rl * C;
#define RT24(len) ((((p24_pts(len)) + R - 1) / R + 1) * rl < qsl ? (((p24_pts(len)) + R - 1) / R + 1) * rl : qsl)
#define RMIN(x) (Rl0 ? ((x) / (size_t)g * (size_t)(g - 1) > Rl0 ? Rl0 : (x) / (size_t)g * (size_t)(g - 1)) : (x))
        size_t va = share_a < pa ? share_a : pa, vb = share_b < pb ? share_b : pb;
        size_t win = share_c < nc ? share_c : nc, send = RT24(nc);
        { size_t Wt = mn_t_chunk_limbs(); if (Wt && win > Wt) { win = Wt; size_t sc = Wt / 4 + 2 * (size_t)g * rl; if (send > sc) send = sc; } }
        size_t result = RMIN(send) + RMIN(win / 4);
        size_t ra = RMIN(RT24(pa)) + RMIN(va / 4 + va / R * (size_t)g + 2 * (size_t)g * rl), rb = RMIN(RT24(pb)) + RMIN(vb / 4 + vb / R * (size_t)g + 2 * (size_t)g * rl), redist = ra > rb ? ra : rb;
        size_t transform = 2 * RMIN(q / 4);
#undef RMIN
#undef RT24
        size_t m = result > redist ? result : redist; return m > transform ? m : transform;
    }
    size_t cap = (size_t)1 << mn_logn_cap(g); int ka = 1, kb = 1;
    if (na + nb > cap && (na + 31) / 32 + (nb + 31) / 32 > cap) { ka = (int)((na + cap / 2 - 1) / (cap / 2)); kb = (int)((nb + cap / 2 - 1) / (cap / 2)); }   /* beyond split_grid_cap's 32 x 32 (the product itself would stop): a sizing guess, no abort here */
    else if (na + nb > cap) split_grid_cap(na, nb, cap, (size_t)1 << mn_logmin(g), 0, &ka, &kb);
    size_t pa = (na + ka - 1) / ka, pb = (nb + kb - 1) / kb, nc = pa + pb;
    int logn, logR, logC; size_t q; mn_shape(nc, g, &logn, &logR, &logC, &q); if (qmax) *qmax = q;
    int nr = 4 * g; size_t R = (size_t)1 << logR, C = (size_t)1 << logC, rows = (R + nr - 1) / nr, qs = rows * C;
#define RT(len) ((((len) + R - 1) / R + 1) * rows < qs ? (((len) + R - 1) / R + 1) * rows : qs)
    size_t va = share_a < pa ? share_a : pa, vb = share_b < pb ? share_b : pb;
    size_t win = share_c < nc ? share_c : nc, send = RT(nc);
    { size_t Wt = mn_t_chunk_limbs(); if (Wt && win > Wt) { win = Wt; size_t sc = Wt / 4 + 2 * (size_t)g * rows; if (send > sc) send = sc; } }   /* MN_T_CHUNK_MB: the rounds */
    /* Phase 14 V1: COMM_SHMEM_ROUND_MB = m -- comm_shmem.c carries an all-to-all whose pair exceeds m / (n - 1) in rounds that
     * stage at most m each way, and leaves the self slab out of the staging: each side of each exchange is its peers' part
     * (modelled as (g - 1) / g of it) bounded by m (RMIN) */
    const char *er = getenv("COMM_SHMEM_ROUND_MB"); size_t Rl = er && atof(er) > 0 ? (size_t)(atof(er) * 1048576.0) / 8 : 0;
#define RMIN(x) (Rl ? ((x) / (size_t)g * (size_t)(g - 1) > Rl ? Rl : (x) / (size_t)g * (size_t)(g - 1)) : (x))
    size_t result = RMIN(send) + RMIN(win / 4);
    size_t ra = RMIN(RT(pa)) + RMIN(va / 4 + 2 * (size_t)g * rows), rb = RMIN(RT(pb)) + RMIN(vb / 4 + 2 * (size_t)g * rows), redist = ra > rb ? ra : rb;
    size_t transform = 2 * RMIN(q / 4);
#undef RMIN
#undef RT
    size_t m = result > redist ? result : redist; return m > transform ? m : transform;
}
/* ---- Phase 11 L: the level -> group-size schedule of the distributed tree (MN_GROUPS) ------------------------------
 * out[l-1] = the group size of tree level l >= 1 (the nodes [k G_l, min((k+1) G_l, size)), k = rank / G_l); the children of
 * a level are the groups of the previous level (size 1 at level 1), so a ratio G_l / G_{l-1} > 2 is a k-way step (the
 * tree combines the k children in k - 1 products over the level's group, each balanced over all its nodes).  Sizes are
 * increasing; each is a multiple of the previous, or the size itself (the top group is cut by the size: 512 -> 576 is
 * allowed).  MN_GROUPS=2,4,8,16,32,64,576 (the 9-way top) or ..., 64, 512, 576; default (Phase 12 G, agent Q's decision): the
 * powers of two dividing the size, then the odd part's prime factors ascending (576 -> 2, 4, ..., 64, 192, 576: two 3-way
 * steps; a power of two: the binary tree).  Returns the level count (0 at size 1); aborts on an invalid list. */
int mn_groups_parse(int size, int *out, int max)
{
    int n = 0; const char *e = getenv("MN_GROUPS");
    if (size <= 1) return 0;
    if (e && *e) {
        const char *s = e;
        while (*s) {
            char *end; long v = strtol(s, &end, 10);
            if (end == s || (*end && *end != ',')) { ec_fatal(EC_RC_FATAL, "MN_GROUPS: cannot parse '%s'\n", e); }
            s = *end ? end + 1 : end;
            if (v < 2 || (n && v <= out[n - 1])) { ec_fatal(EC_RC_FATAL, "MN_GROUPS: sizes must be > 1 and increasing ('%s')\n", e); }
            if (n == max) { ec_fatal(EC_RC_FATAL, "MN_GROUPS: more than %d levels\n", max); }
            if (v >= size) { out[n++] = size; break; }
            if (n && v % out[n - 1]) { ec_fatal(EC_RC_FATAL, "MN_GROUPS: %ld is not a multiple of %d ('%s')\n", v, out[n - 1], e); }
            out[n++] = (int)v;
        }
    } else {
        /* Phase 12 G (agent Q's decision, results/Q.md 1: the 3 . 3 top at 576): the powers of two that divide the size, then the
         * odd part's prime factors in increasing order -- 576 = 2^6 . 3 . 3 -> 2, 4, ..., 64, 192, 576; 9 -> 3, 9; 6 -> 2, 6; a power
         * of two -> the binary tree as before.  Every group is then exact (no cut top group) and every level is a k-way step by
         * one prime factor */
        int v = 1; while (v * 2 <= size && size % (v * 2) == 0 && n < max) { v *= 2; out[n++] = v; }
        int rest = size / v;
        for (int f = 3; rest > 1 && n < max; f += 2) while (rest % f == 0 && n < max) { v *= f; out[n++] = v; rest /= f; }
    }
    if (!n || out[n - 1] != size) { if (n == max) { ec_fatal(EC_RC_FATAL, "MN_GROUPS: more than %d levels\n", max); } out[n++] = size; }
    return n;
}

/* ---- the shifted distributed add: C += X << k on C's shares (Phase 9 A3; B7: exact slabs) --------------------------
 * Node r's share [clo, chi) of C needs X's limbs [clo - k, chi - k) (cut to [0, nX)): the window [tlo, thi) of its share.
 * APU thread d serves the quarter d of every node's window, in rounds of CH limbs (one alltoallv per round over mesh d;
 * a pair's slab is the cut of the sender's share of X with the receiver's quarter chunk, contiguous, sent at its exact
 * length), unpacked into a temporary T of this node's window; then C's share += T << (tlo - clo) with the carry scan
 * over the nodes.  The rounds bound the receiver's buffer to CH limbs; the sender's to the parts of X that meet the
 * receivers' chunks of the round (about two chunks). */
struct rng { size_t a, b, src, off; };                         /* a slab: limbs [a, b) of X, from local index src of the sender's share (or to T index src), at limb off of the slab buffer */
__global__ void k_pack_rng(uint64_t *sb, struct acc src, const struct rng *tb, int g, size_t S)
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t r = t / S, i = t - r * S; if (i < tb[r].b - tb[r].a) sb[tb[r].off + i] = acc_get(src, tb[r].src + i); }
}
__global__ void k_unpack_rng(struct acc dst, const uint64_t *rb, const struct rng *tb, int g, size_t S)   /* tb[s].src: the T index of limb a */
{
    size_t total = (size_t)g * S, t = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; t < total; t += stride) { size_t s = t / S, i = t - s * S; if (i < tb[s].b - tb[s].a) *acc_ptr(dst, tb[s].src + i) = rb[tb[s].off + i]; }
}
static void add_window(const mdb *C, int r, size_t k, size_t nX, size_t *lo, size_t *hi)   /* node r's window of C, in C's coordinates */
{
    size_t clo, chi; mdb_share(C, r, &clo, &chi);
    size_t a = clo > k ? clo : k, b = chi < k + nX ? chi : k + nX; if (b < a) b = a;
    *lo = a; *hi = b;
}
static void x_part(const mdb *X, int r, size_t *lo, size_t *hi) { mdb_share(X, r, lo, hi); if (*lo > X->n) *lo = X->n; if (*hi > X->n) *hi = X->n; }
#define MDB_ADD_CHUNK ((size_t)1 << 26)
/* Phase 13a M (TASKS 1.3, MN_T_CHUNK_MB): the same add with T bounded -- round c covers every node's chunk c = [lo + c W, +W)
 * of its window (C's coordinates), APU d serving the quarter d of each chunk, so T holds one chunk (W limbs per node, a
 * quarter per APU: the dbig's own quarters) and is added into C's share after each round.  X's limbs are normalised, so the
 * chunks' adds carry out of the share at most once in all (C + X's window << off < 2 B^cn): one scan at the end, as before. */
static void mdb_add_shifted_rounds(mdb *C, const mdb *X, size_t k, mn_group *G, size_t W, int K)
{
    int g = G->g, node = g_node_of(G); size_t nX = X->n;
    size_t clo, chi; mdb_share(C, node, &clo, &chi); size_t cn = chi - clo;
    size_t tlo, thi; add_window(C, node, k, nX, &tlo, &thi); size_t tn = thi - tlo, tcap = tn < W ? tn : W;
    size_t xlo, xhi; x_part(X, node, &xlo, &xhi);
    dbig T; db_init(&T); if (tn) db_zero_fill(&T, tcap);
    int kc = 0, pr = cn == 0; size_t add_rb = 0;
    struct acc src = acc_db(&X->sh, 0, xhi > xlo ? xhi - xlo : 0);
    for (int c = 0; c < K; c++) {
        size_t wlo = tlo + (size_t)c * W; if (wlo > thi) wlo = thi; size_t whi = wlo + W < thi ? wlo + W : thi, wn = whi - wlo;
#pragma omp parallel num_threads(NR)
        {
            int d = omp_get_thread_num(); hipStream_t s = RS[d].s;
            HIP_CHECK(hipSetDevice(d));
            struct rng *ht = (struct rng *)malloc((size_t)2 * g * sizeof *ht), *hu = ht + g;
            size_t *cnt = (size_t *)malloc((size_t)4 * g * sizeof *cnt), *scnt = cnt, *sdsp = cnt + g, *rcnt = cnt + 2 * g, *rdsp = cnt + 3 * g, o = 0, so = 0, S = 16;
            for (int r = 0; r < g; r++) {                  /* pack: receiver r's chunk c, its quarter d -> X's [a, b) cut to my part */
                size_t lo, hi; add_window(C, G->g0 + r, k, nX, &lo, &hi);
                size_t x = lo + (size_t)c * W; if (x > hi) x = hi; size_t y = x + W < hi ? x + W : hi, ln = y - x;
                size_t qlo = x + ln * d / 4, qhi = x + ln * (d + 1) / 4;
                size_t a = qlo - k, b = qhi - k; if (a < xlo) a = xlo; if (b > xhi) b = xhi; if (b < a) b = a;
                ht[r].a = a; ht[r].b = b; ht[r].src = a - xlo; ht[r].off = o; scnt[r] = (b - a) * 8; sdsp[r] = o * 8; o += b - a; if (b - a > S) S = b - a;
            }
            so = o; o = 0;
            size_t qlo = wlo + wn * d / 4, qhi = wlo + wn * (d + 1) / 4;
            for (int r = 0; r < g; r++) {                  /* unpack: sender r's slab = X's [a, b) in my quarter chunk -> T at a + k - wlo */
                size_t plo, phi; x_part(X, G->g0 + r, &plo, &phi);
                size_t a = qlo - k, b = qhi - k; if (a < plo) a = plo; if (b > phi) b = phi; if (b < a) b = a;
                hu[r].a = a; hu[r].b = b; hu[r].src = a + k - wlo; hu[r].off = o; rcnt[r] = (b - a) * 8; rdsp[r] = o * 8; o += b - a; if (b - a > S) S = b - a;
            }
            uint64_t *sb = db_pool_alloc(d, (so + 16) * 8), *rb = db_pool_alloc(d, (o + 16) * 8);
#pragma omp atomic
            add_rb += (o + 16) * 8;
            struct rng *dt = (struct rng *)db_pool_alloc(d, g * sizeof *dt + 64);
            HIP_CHECK(hipMemcpyAsync(dt, ht, g * sizeof *ht, hipMemcpyHostToDevice, s));
            if (so) k_pack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(sb, src, dt, g, S);
            HIP_CHECK(hipStreamSynchronize(s));
            { int xk_ = comm_xtag_set(XK_ADDSH); comm_alltoallv(G->all[d], sb, scnt, sdsp, rb, rcnt, rdsp, s); comm_xtag_set(xk_); } comm_wait(G->all[d]);
            HIP_CHECK(hipMemcpyAsync(dt, hu, g * sizeof *hu, hipMemcpyHostToDevice, s));
            if (o) { struct acc dst = acc_db(&T, 0, wn); k_unpack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(dst, rb, dt, g, S); }
            HIP_CHECK(hipStreamSynchronize(s));
            db_pool_free(d, sb); db_pool_free(d, rb); db_pool_free(d, (uint64_t *)dt); free(ht); free(cnt);
        }
        HIP_CHECK(hipSetDevice(0));
        if (wn) { int co = 0, p = 0; db_share_add_shifted(&C->sh, cn, &T, wlo - clo, &co, &p); kc += co; pr = p; if (whi < thi) db_zero_fill(&T, tcap); }
    }
    if (kc > 1) { ec_fatal(EC_RC_FATAL, "mdb_add_shifted: %d carries out of a share (the bound is 1)\n", kc); }
    { size_t b = T.cap * 8 + add_rb / (K ? K : 1); if (b > rns_dist_tscratch_max) rns_dist_tscratch_max = b; }
    share_carry_fix(G, &C->sh, cn, kc, pr);
    db_free(&T);
}
void mdb_add_shifted(mdb *C, const mdb *X, size_t k, mn_group *G)
{
    int g = G->g, node = g_node_of(G); size_t nX = X->n;
    double t0 = mem_now();
    if (!g_init) { for (int r = 0; r < NR; r++) rank_init(r); g_init = 1; }
    if (nX && k + nX > C->N) { ec_fatal(EC_RC_FATAL, "mdb_add_shifted: %zu limbs at %zu exceed the basis %zu\n", nX, k, C->N); }
    { size_t W = mn_t_chunk_limbs(), mx = 0; if (W) { for (int r = 0; r < g; r++) { size_t lo, hi; add_window(C, G->g0 + r, k, nX, &lo, &hi); if (hi - lo > mx) mx = hi - lo; }
      if (mx > W) { int K = (int)((mx + W - 1) / W); mdb_add_shifted_rounds(C, X, k, G, W, K);
                    if (getenv("RNS_VERBOSE")) printf("mdb_add_shifted node %d: %zu limbs at %zu into a basis of %zu, %d rounds of %zu limbs (T bounded): %.3f s\n", node, nX, k, C->N, K, W, mem_now() - t0);
                    return; } } }
    size_t clo, chi; mdb_share(C, node, &clo, &chi); size_t cn = chi - clo;
    size_t tlo, thi; add_window(C, node, k, nX, &tlo, &thi); size_t tn = thi - tlo;
    size_t maxq = 0; for (int r = 0; r < g; r++) { size_t lo, hi; add_window(C, G->g0 + r, k, nX, &lo, &hi);   /* (Phase 12 G: was add_window(C, r, ...) -- the local member index as a global node: on a group with g0 > 0 the windows came out empty, 0 rounds, X never added; the tree's P = P_A Q_B + P_B on a gridded level over [2, 4) was wrong) */ size_t qq = (hi - lo + 3) / 4; if (qq > maxq) maxq = qq; }
    size_t S = maxq < MDB_ADD_CHUNK ? maxq : MDB_ADD_CHUNK; S = (S + 15) / 16 * 16; int rounds = maxq ? (int)((maxq + S - 1) / S) : 0;
    dbig T; db_init(&T); if (tn) db_zero_fill(&T, tn);
    size_t xlo, xhi; x_part(X, node, &xlo, &xhi);
    size_t stot_max[NR] = {0}, rtot_max[NR] = {0}, add_rb = 0;
    if (rounds) {
#pragma omp parallel num_threads(NR)
    {
        int d = omp_get_thread_num(); hipStream_t s = RS[d].s;
        HIP_CHECK(hipSetDevice(d));
        /* the tables of every round first (both sides' cuts from the descriptors), then the buffers at the largest round */
        struct rng *hs = (struct rng *)malloc((size_t)2 * rounds * g * sizeof *hs), *hr = hs + (size_t)rounds * g;
        size_t *cnt = (size_t *)malloc((size_t)4 * rounds * g * sizeof *cnt), smax = 0, rmax = 0;
        for (int c = 0; c < rounds; c++) {
            struct rng *ht = hs + (size_t)c * g, *hu = hr + (size_t)c * g; size_t *scnt = cnt + (size_t)4 * c * g, *sdsp = scnt + g, *rcnt = sdsp + g, *rdsp = rcnt + g, o = 0;
            for (int r = 0; r < g; r++) {                      /* pack: for every receiver r, its quarter d, chunk c (in C's coordinates) -> X's [a, b) cut to my part */
                size_t lo, hi; add_window(C, G->g0 + r, k, nX, &lo, &hi); size_t wn = hi - lo;
                size_t qlo = lo + wn * d / 4 + (size_t)c * S, qhi = lo + wn * (d + 1) / 4; if (qlo + S < qhi) qhi = qlo + S; if (qhi < qlo) qhi = qlo;
                size_t a = qlo - k, b = qhi - k; if (a < xlo) a = xlo; if (b > xhi) b = xhi; if (b < a) b = a;   /* qlo >= k: the window starts at k or above */
                ht[r].a = a; ht[r].b = b; ht[r].src = a - xlo; ht[r].off = o; scnt[r] = (b - a) * 8; sdsp[r] = o * 8; o += b - a;
            }
            if (o > smax) smax = o; o = 0;
            size_t qlo = tlo + tn * d / 4 + (size_t)c * S, qhi = tlo + tn * (d + 1) / 4; if (qlo + S < qhi) qhi = qlo + S; if (qhi < qlo) qhi = qlo;
            for (int r = 0; r < g; r++) {                      /* unpack: sender r's slab holds X's [a, b) = its part cut to my quarter chunk -> T at a + k - tlo */
                size_t plo, phi; x_part(X, G->g0 + r, &plo, &phi);
                size_t a = qlo - k, b = qhi - k; if (a < plo) a = plo; if (b > phi) b = phi; if (b < a) b = a;
                hu[r].a = a; hu[r].b = b; hu[r].src = a + k - tlo; hu[r].off = o; rcnt[r] = (b - a) * 8; rdsp[r] = o * 8; o += b - a;
            }
            if (o > rmax) rmax = o;
        }
        stot_max[d] = smax; rtot_max[d] = rmax;
        uint64_t *sb = db_pool_alloc(d, (smax + 16) * 8), *rb = db_pool_alloc(d, (rmax + 16) * 8);
#pragma omp atomic
        add_rb += (rmax + 16) * 8;
        struct rng *dt = (struct rng *)db_pool_alloc(d, g * sizeof *dt + 64);
        struct acc src = acc_db(&X->sh, 0, xhi > xlo ? xhi - xlo : 0), dst = acc_db(&T, 0, tn);
        for (int c = 0; c < rounds; c++) {
            struct rng *ht = hs + (size_t)c * g, *hu = hr + (size_t)c * g; size_t *scnt = cnt + (size_t)4 * c * g, *sdsp = scnt + g, *rcnt = sdsp + g, *rdsp = rcnt + g;
            HIP_CHECK(hipMemcpyAsync(dt, ht, g * sizeof *ht, hipMemcpyHostToDevice, s));
            if (xhi > xlo) k_pack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(sb, src, dt, g, S);
            HIP_CHECK(hipStreamSynchronize(s));
            { int xk_ = comm_xtag_set(XK_ADDSH); comm_alltoallv(G->all[d], sb, scnt, sdsp, rb, rcnt, rdsp, s); comm_xtag_set(xk_); } comm_wait(G->all[d]);
            HIP_CHECK(hipMemcpyAsync(dt, hu, g * sizeof *hu, hipMemcpyHostToDevice, s));
            if (tn) k_unpack_rng<<<nblk((size_t)g * S), 256, 0, s>>>(dst, rb, dt, g, S);
            HIP_CHECK(hipStreamSynchronize(s));
        }
        db_pool_free(d, sb); db_pool_free(d, rb); db_pool_free(d, (uint64_t *)dt); free(hs); free(cnt);
    }
    HIP_CHECK(hipSetDevice(0));
    }
    { size_t b = T.cap * 8 + add_rb; if (b > rns_dist_tscratch_max) rns_dist_tscratch_max = b; }
    int co = 0, pr = cn == 0;
    if (tn) db_share_add_shifted(&C->sh, cn, &T, tlo - clo, &co, &pr);
    share_carry_fix(G, &C->sh, cn, co, pr);
    db_free(&T);
    if (getenv("RNS_VERBOSE")) printf("mdb_add_shifted node %d: %zu limbs at %zu into a basis of %zu (my window [%zu, %zu), %d rounds of %zu; slabs %zu + %zu limbs per APU 0): %.3f s\n", node, nX, k, C->N, tlo, thi, rounds, S, stot_max[0], rtot_max[0], mem_now() - t0);
}
