/* rns_mul.c - see rns_mul.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <pthread.h>
#include <unistd.h>
#include "rns_mul.h"
#include "dbig.h"
#include "ntt.h"
#include "mem.h"
#include "crt.h"
#include "ntt2.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

rns_stats rns_st;
int rns_repack_wide = 0;          /* RNS_REPACK_WIDE=1: one full-width copy per device (slower: 1.20 vs 0.78 s at 2^31) */
int rns_school_max = 1024;
int rns_crt_threads = 0;
size_t rns_batch_tile_bytes = 15000000000ULL;
int rns_gpucrt_min = 1;
int rns_gpucrt_blocks = 912;      /* GPUCRT_MIN_BLOCKS: stripes so that M S >= this */
static uint64_t *g_spill; static size_t g_spill_cap;
static int g_mdev_S;
int rns_crt_layout = 0;          /* 0: one plane per node (paper, default); 1: quartered node-local planes (no gain, RESULTS.md 38) */

static int g_nd = 0, g_pool_log = 31;
static uint64_t *g_snap[EC_NP]; static size_t g_snap_cap, g_snap_n; static const uint64_t *g_snap_b; static int g_snap_on = -1;
int rns_engine = 1;               /* RNS_ENGINE: 1 paper (4 x 52-bit FP64), 2 two 62-bit primes / 45-bit points */
static struct dev {
    ntt_ctx *ctx;
    ntt2_ctx *ctx2;                /* engine 2: prime d & 1 */
    hipStream_t s;
    uint64_t *hstage;            /* 2^pool_log limbs, pinned, NUMA-local */
    dpool da, db;                /* ch_da / ch_db */
    int ncpu;
    ntt_ctx *ctxp[EC_NP];        /* WP3 local mode: this device transforms all EC_NP primes of its own products */
    uint64_t *spill;             /* WP3 local mode: this device's stripe spills (host registered) */
    size_t spill_cap;
    uint32_t *len, *dlen; size_t len_cap;   /* WP3 local mode: normalised lengths (pinned host, device) */
} D[EC_NP];
int rns_batch_local = -1;        /* RNS_BATCH_LOCAL: 1 (default) products in device pools are done by their own APU */
int rns_batch_local_min = 16;    /* RNS_BATCH_LOCAL_MIN: below this many products the striped path is used */
int rns_batch_pair = -1;         /* RNS_BATCH_PAIR: 1 (default) products 2i, 2i+1 sharing B (the tree's P1 Q2 + P2, Q1 Q2) transform B once (Phase 9 B1) */

__global__ void k_store(uint64_t *dst, const uint64_t *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}

void (*rns_after_staging_hook)(void *) = 0; void *rns_hook_arg = 0;
size_t rns_staging_bytes_req = 0;                                  /* Phase 8 step 3: the pinned staging per APU (0 = 8 << pool_log, the paper's) */
static size_t g_staging_bytes;
size_t rns_staging_bytes(void) { return g_staging_bytes; }
size_t rns_pool1_bytes_req = 0;                                    /* Phase 9 C4: plane pool 1 per APU (0 = 8 << pool_log, the paper's; the device flow needs 3 q + 16 limbs) */
/* Phase 11 B3 (agent P): plane pool 0 sized at init to 3 2^(pool_log-1) limbs (24 GiB at 2^31) and pool 1 to the dist tier's
 * 3 q + 16 at q = 3 2^(pool_log-3) (18 GiB), so the top levels and the dm phase run their products on 3 2^28 .. 3 2^30-point
 * planes (rns_dist.c's DIST_R3, whose default follows this switch) with nothing mapped inside a phase (A-grid's C5 paid 15 s
 * at first use).  RNS_PLANES_3Q30=0/1 overrides.  Measured at 4e10 (results/P.md): the phases lose 3.6 s (top levels -2.8,
 * reciprocal -0.6, level 21 paired -0.2) but the 60 GB more of plane pools cost 4.7-6.3 s of mapping at init (0.08-0.1 s/GB
 * while the seeds stream), so P left the default OFF.  Phase 12 I: with M11's tail layout the phases gain 5 s (the dm phase
 * 2.5 of them) for the same 5 s of init, and the size rule (on below 5e10 at 2^31 pools) is the default -- see
 * rns_planes_3q30_default. */
int rns_planes_3q30 = -1;
int rns_planes_3q30_default(int pool_log, double digits)
{
    const char *e = getenv("RNS_PLANES_3Q30");
    if (e && strcmp(e, "auto")) return atoi(e) != 0;
    /* Phase 12 I: the size rule is the default (RNS_PLANES_3Q30=auto, or unset): on below 5e10 at 2^31 pools (7-8e10 do not fit).
     * Re-measured on the same node in one batch with M11's tail layout (results/I.md): phases 59.2 s against 64.2 / 64.8 (bs
     * 32.7 vs 35.2-35.8, dm 26.4 vs 28.9), init 22.5 against 17.3-18.1 (the 60 GB more map at the same 0.075 s/GB), wall
     * 81.7 against 82.1-82.3: the planes now pay for themselves by a hair, and the phases are 5 s faster. */
    return (pool_log ? pool_log : 31) >= 31 && digits < 5e10;
}
static int planes_3q30(void) { if (rns_planes_3q30 < 0) { const char *e = getenv("RNS_PLANES_3Q30"); rns_planes_3q30 = e && strcmp(e, "auto") ? atoi(e) != 0 : 0; } return rns_planes_3q30; }   /* (unset by the driver: off -- the tests' rns_init) */
/* Phase 13a P3: with three primes plane pool 0 holds the dist tier's ec_np q = 3 q limbs (q = 2^(pool_log-2), or
 * 3 2^(pool_log-3) with the B3 planes): 3/4 of the four-prime pool; pool 1 (xb | sbuf | rbuf, one prime at a time) keeps
 * its 3 q + 16.  The mdev tier's plane is then capped by pool 0 (mdev_pts). */
static size_t pool0_np3_bytes(void)
{
    size_t p0; rns_plane_pool_bytes(g_pool_log ? g_pool_log : 31, planes_3q30(), 3, &p0, 0); return p0;
}
/* Phase 13b P (PLAN 31 step 0.3): the plane pools' bytes per APU as rns_init makes them, from (pool_log, the 3 2^k planes,
 * the prime count) alone -- the one formula for rns_init, rns_pool1_default_bytes and binsplit's BS_LAYOUT_ONLY report (and
 * mem_model.py's port).  q = the dist tier's rank plane per prime: 2^(pool_log-2), or 3 2^(pool_log-3) with the 3 2^k planes
 * (the plane cap n = 4 q: 2^pool_log or 3 2^(pool_log-1)).  Pool 0 = np q limbs (the np planes xa[]; four primes without the
 * 3 2^k planes: the power of two 2^pool_log = 4 q, the same bytes), exact.  Pool 1 = xb | sbuf | rbuf, one prime at a time:
 * 3 q + 16 limbs whatever np, never below the batch tier's 2^min(pool_log, 30)-limb tile (pool_log <= 30), 2 MiB-aligned.
 * Returns p0 + p1. */
size_t rns_plane_pool_bytes(int pool_log, int b3, int np, size_t *p0, size_t *p1)
{
    int pl = pool_log ? pool_log : 31;
    size_t q = b3 ? (size_t)3 << (pl - 3) : (size_t)1 << (pl - 2), al = (size_t)2 << 20;
    size_t a = (size_t)np * q * 8, b = (3 * q + 16) * 8, full = (size_t)8 << (pl < 30 ? pl : 30);
    if (b < full) b = full;
    a = (a + al - 1) / al * al; b = (b + al - 1) / al * al;
    if (p0) *p0 = a; if (p1) *p1 = b;
    return a + b;
}
/* Phase 13b P: BS_LAYOUT_ONLY runs before rns_init (no device): binsplit's sizing reads rns_pool_log() */
void rns_preinit_pool_log(int pool_log) { if (!g_nd) g_pool_log = pool_log ? pool_log : 31; }
size_t rns_plane_limbs(void)                                       /* plane pool 0's capacity in limbs: 2^pool_log, or 3 2^(pool_log-1) with the B3 planes */
{
    int pl = g_pool_log ? g_pool_log : 31;
    if (g_nd && D[0].da.p) return D[0].da.cap / 8;
    if (ec_np_init() == 3) return pool0_np3_bytes() / 8;
    return planes_3q30() ? (size_t)3 << (pl - 1) : (size_t)1 << pl;
}
size_t rns_dpool_cap(int dev, int which) { return which ? D[dev].db.cap : D[dev].da.cap; }
size_t rns_pool1_default_bytes(int pool_log)                       /* what the dist tier uses of pool 1 at 2^pool_log points: xb (q + 16) | sbuf (q) | rbuf (q), 2 MiB-aligned;
                                                                    * never below the paper's full pool for pool_log <= 30, where the batch tier's 2^30 tile needs it (so the pool never grows inside a phase there);
                                                                    * B3: q = 3 2^(pool_log-3) with the 3 2^k planes */
{
    size_t b; rns_plane_pool_bytes(pool_log, planes_3q30(), 4, 0, &b); return b;   /* Phase 13b P: the one formula (pool 1 does not depend on the prime count) */
}
static size_t g_tables[EC_NP];                                     /* M9: device bytes of the transform contexts (twiddle tables), by hipMemGetInfo around their creation */
void (*rns_shutdown_hook)(void) = 0;                               /* Phase 9 C4: binsplit releases its region arenas here (they outlive the block pool's use of them) */
static void rns_acct(int ndev, size_t b[][MEM_DEV_NCAT])
{
    for (int d = 0; d < ndev && d < EC_NP; d++) { b[d][MEM_DEV_PLANES] += D[d].da.cap + D[d].db.cap; b[d][MEM_DEV_TABLES] += g_tables[d]; }
}
static size_t dev_used(int d) { size_t f = 0, t = 0; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); if (hipMemGetInfo(&f, &t) != hipSuccess) f = t = 0; HIP_CHECK(hipSetDevice(cur)); return t - f; }
int rns_init(int pool_log)
{
    if (g_nd) return g_nd;
    HIP_CHECK(hipGetDeviceCount(&g_nd));
    if (g_nd > EC_NP) g_nd = EC_NP;
    if (g_nd < EC_NP) { fprintf(stderr, "rns_init: need %d devices, have %d\n", EC_NP, g_nd); exit(1); }
    g_pool_log = pool_log ? pool_log : 31;
    omp_set_max_active_levels(2);
    if (getenv("MEM_PIN") && atoi(getenv("MEM_PIN"))) mem_pin_threads(g_nd);   /* WP3: optional; no CPU pass touches device memory now, and pinning costs the decimal seeds 30 % (RESULTS.md 56) */
    if (getenv("RNS_CRT_LAYOUT")) rns_crt_layout = atoi(getenv("RNS_CRT_LAYOUT"));
    if (getenv("RNS_REPACK_WIDE")) rns_repack_wide = atoi(getenv("RNS_REPACK_WIDE"));
    if (getenv("RNS_ENGINE")) rns_engine = atoi(getenv("RNS_ENGINE"));
    if (getenv("RNS_BATCH_TILE_GB")) rns_batch_tile_bytes = (size_t)(atof(getenv("RNS_BATCH_TILE_GB")) * 1e9);   /* Phase 11 A4 (agent P): the batch tiers' plane budget per device (a + b planes; 15 GB = the paper's), capped by the pools */
    bi_env_base();
    crt_init();                                   /* (reads ECALC_NP: ec_np_init) */
    if (ec_np == 3) { ec_np_check(0, bi_decimal, "rns_init"); printf("rns_init: three primes (ECALC_NP=3): c 2^44 + 1, c = 240, 216, 207; at most %zu terms per product\n", ec_np3_max_terms); }   /* P3: refuse three primes with binary limbs at start (the tests set the base before rns_init) */
    size_t bytes = (size_t)8 << g_pool_log, sbytes = rns_staging_bytes_req ? rns_staging_bytes_req : bytes; g_staging_bytes = sbytes;
    int par = getenv("ECALC_OVERLAP") ? atoi(getenv("ECALC_OVERLAP")) : 1;   /* Phase 8 (PLAN 18, O1): one thread per device */
    mem_par_init = par;
    double ti0 = mem_now();
#pragma omp parallel for num_threads(g_nd) schedule(static) if(par)
    for (int d = 0; d < g_nd; d++) {
        double tt, tr, x0 = mem_now(), x1, x2, x3;
        HIP_CHECK(hipSetDevice(d));
        size_t u0 = dev_used(d); x1 = mem_now();
        D[d].ctx = ntt_ctx_create(d);
        D[d].ctx2 = rns_engine == 2 ? ntt2_ctx_create(d & 1) : 0;   /* Phase 11 I11 (agent P): engine 2's context only when engine 2 runs */
        size_t u1 = dev_used(d); g_tables[d] = u1 > u0 ? u1 - u0 : 0; x2 = mem_now();
        HIP_CHECK(hipStreamCreate(&D[d].s));
        D[d].hstage = (uint64_t *)mem_hstage_alloc(d, sbytes, &tt, &tr); x3 = mem_now();
        D[d].ncpu = mem_ncpus_node(mem_numa_node_of_device(d));
        if (getenv("RNS_VERBOSE")) printf("rns_init: APU%d staging %.1f GiB touch %.2f s register %.2f s, %d cpus; device open %.2f s, contexts %.2f s, stream+staging %.2f s\n", d, sbytes / 1073741824.0, tt, tr, D[d].ncpu, x1 - x0, x2 - x1, x3 - x2);
    }
    double ti1 = mem_now();
    if (rns_after_staging_hook) rns_after_staging_hook(rns_hook_arg);     /* Phase 8 I2: the seeds start now, during the pool allocations below */
    size_t b1 = rns_pool1_bytes_req ? rns_pool1_bytes_req : bytes;   /* C4: pool 1 sized to the dist tier's 3 q when the flow is all-device (the host mdev tier needs the full 2^pool_log) */
    if (getenv("RNS_POOL1_GB")) b1 = (size_t)(atof(getenv("RNS_POOL1_GB")) * 1e9);
#pragma omp parallel for num_threads(g_nd) schedule(static) if(par)
    for (int d = 0; d < g_nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        if (ec_np == 3) dpool_get_exact(&D[d].da, d, pool0_np3_bytes());   /* P3: the dist tier's 3 q (3/4 of the four-prime pool) */
        else if (planes_3q30()) dpool_get_exact(&D[d].da, d, ((size_t)3 << (g_pool_log - 1)) * 8);   /* B3: 3 2^(pool_log-1) limbs exactly (24 GiB), not the power of two above it */
        else dpool_get(&D[d].da, d, bytes);      /* pregrow to 2^pool_log (paper) */
        dpool_get_exact(&D[d].db, d, b1);
    }
    mem_par_init = 0;
    if (g_snap_on < 0) g_snap_on = getenv("ECALC_B_SNAPSHOT") ? atoi(getenv("ECALC_B_SNAPSHOT")) : 0;
    if (g_snap_on) { g_snap_cap = (size_t)1 << 30; for (int d = 0; d < g_nd; d++) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMalloc(&g_snap[d], g_snap_cap)); } }   /* Phase 12 R witness buffers */
    mem_acct_register(rns_acct);
    double ti2 = mem_now();
    for (int d = 0; d < g_nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        for (int c = 0; c < g_nd; c++) if (c != d) {
            hipError_t e = hipDeviceEnablePeerAccess(c, 0);
            if (e != hipSuccess && e != hipErrorPeerAccessAlreadyEnabled) { fprintf(stderr, "peer access %d->%d: %s\n", d, c, hipGetErrorString(e)); exit(1); }
            (void)hipGetLastError();
        }
    }
    HIP_CHECK(hipSetDevice(0));
    db_pool_vmm_on() ? db_vmm_bg_release() : (void)0;   /* Phase 14 R1 (DB_POOL_VMM): the arenas' second halves map from here, after the plane pools */
    if (getenv("RNS_VERBOSE")) printf("rns_init: plane pools per APU %.2f + %.2f GiB%s (%s), staging %.2f GiB, tables %.3f GB; staging+contexts %.2f s, pools %.2f s, peer access %.2f s\n", D[0].da.cap / 1073741824.0, b1 / 1073741824.0, planes_3q30() ? " (3*2^k planes, B3)" : "", mem_alloc_form_name(), sbytes / 1073741824.0, g_tables[0] / 1e9, ti1 - ti0, ti2 - ti1, mem_now() - ti2);
    return g_nd;
}
int rns_pool_log(void) { return g_pool_log; }
uint64_t *rns_hstage(int dev) { return D[dev].hstage; }
/* WP5: the pinned staging (64 GB) is idle while the dm phase runs on device: release it, re-create for dc */
void rns_release_staging(void)
{
    for (int d = 0; d < g_nd; d++) if (D[d].hstage) { mem_hstage_free(D[d].hstage); D[d].hstage = 0; }
}
void rns_ensure_staging(void)
{
    size_t bytes = g_staging_bytes ? g_staging_bytes : (size_t)8 << g_pool_log;
    for (int d = 0; d < g_nd; d++) if (!D[d].hstage) { double tt, tr; HIP_CHECK(hipSetDevice(d)); D[d].hstage = (uint64_t *)mem_hstage_alloc(d, bytes, &tt, &tr); }
}
/* WP5: device dev's plane pools (da: which 0, db: which 1), grown to bytes if needed; the tiers share them */
int rns_pool_grow = -1;                                            /* RNS_POOL_GROW: 1 lets a plane pool grow inside a phase (the stress recipe); 0 (default) aborts */
static size_t g_n_grow;
size_t rns_pool_n_grow(void) { return g_n_grow; }
void *rns_dpool(int dev, int which, size_t bytes)
{
    dpool *dp = which ? &D[dev].db : &D[dev].da;
    if (mem_pool_guard && dp->p && dp->dev == dev && dp->cap < bytes) {   /* a growth inside a phase (rns_init makes the pools directly) */
        /* Phase 12 R (D5): the plane pools are sized at init (Phase 9 C4, Phase 11 B3) and no default configuration grows one inside
         * a phase; a growth here means the sizing and the tiers disagree, so it aborts with the memory accounting (as mem_oom
         * does) unless RNS_POOL_GROW=1 asks for it (mnaccept.sh --stress).  The growth itself is a hipFree + hipMalloc of the
         * pool between two tiers -- safe as such (every tier re-reads the pool pointer after this call), just never intended. */
        if (rns_pool_grow < 0) rns_pool_grow = getenv("RNS_POOL_GROW") ? atoi(getenv("RNS_POOL_GROW")) : 0;
        if (!rns_pool_grow) {
            fprintf(stderr, "rns_dpool: plane pool %d on APU %d would grow inside a phase, %.2f -> %.2f GB: the pools are sized at init and must not grow (RNS_POOL_GROW=1 allows it)\n", which, dev, dp->cap / 1e9, bytes / 1e9);
            fflush(stderr); mem_report("GROW"); mem_report_summary(); fflush(stdout); exit(1);
        }
#pragma omp atomic
        g_n_grow++;
        if (getenv("RNS_VERBOSE")) printf("rns_dpool: plane pool %d on APU %d grows inside a phase, %.2f -> %.2f GB (RNS_POOL_GROW=1)\n", which, dev, dp->cap / 1e9, bytes / 1e9);
    }
    return dpool_get(dp, dev, bytes);
}
/* Phase 8 I3: the part of plane pool `which` above `used` bytes (the dist tier's slabs take 3 q of pool 1's 4 q at 2^31 points)
 * goes to the dbig block pool; the pool keeps its size (the tail is never handed out again by dpool_get, which only grows) */
size_t rns_dpool_donate_tail(int dev, int which, size_t used)
{
    dpool *dp = which ? &D[dev].db : &D[dev].da;
    if (!dp->p || dp->cap <= used) return 0;
    size_t tail = dp->cap - used; if (tail < ((size_t)1 << 30)) return 0;
    db_donate_ext(dev, (char *)dp->p + used, tail, 0);
    return tail;
}
int rns_ndev(void) { return g_nd; }
void rns_shutdown(void)
{
    if (rns_shutdown_hook) rns_shutdown_hook();
    for (int d = 0; d < g_nd; d++) {
        HIP_CHECK(hipSetDevice(d));
        dpool_free(&D[d].da); dpool_free(&D[d].db);
        mem_hstage_free(D[d].hstage);
        HIP_CHECK(hipStreamDestroy(D[d].s));
        ntt_ctx_free(D[d].ctx); ntt2_ctx_free(D[d].ctx2);
    }
    g_nd = 0;
}

/* Phase 12 R (D5, instrumentation): ECALC_COPY_PROBE=1 -- the level loop's odd-node copy (a device-to-device hipMemcpy on the
 * destination device's null stream) is timed against the next level's first kernel launch: an event recorded on that null
 * stream right after the copy, polled by a thread (no wait, no synchronisation: the probe cannot hide a race), and the
 * time of the next rns_mul_batch's first launch.  The report names the transition when the copy completed after that
 * launch (the other devices' streams are not ordered against a null stream that is not theirs). */
double rns_t_launch;                                             /* mem_now() at the latest batch call's first kernel launch */
static struct { hipEvent_t ev; int dev, on, level; size_t bytes; double t_issue, t_done; pthread_t th; } g_cp;
static void *cp_poll(void *a) { (void)a; HIP_CHECK(hipSetDevice(g_cp.dev)); while (hipEventQuery(g_cp.ev) == hipErrorNotReady) usleep(50); g_cp.t_done = mem_now(); return 0; }
static const uint64_t *g_cp_src[2], *g_cp_dst[2]; static size_t g_cp_n[2];
void rns_copy_probe_pair(int i, const uint64_t *dst, const uint64_t *src, size_t limbs) { g_cp_dst[i] = dst; g_cp_src[i] = src; g_cp_n[i] = limbs; }   /* the two copies (P, Q) of the odd node, checked at the report */
void rns_copy_probe_issue(int dev, size_t bytes, int level)
{
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    HIP_CHECK(hipEventCreateWithFlags(&g_cp.ev, hipEventDisableTiming)); HIP_CHECK(hipEventRecord(g_cp.ev, 0));
    g_cp.dev = dev; g_cp.bytes = bytes; g_cp.level = level; g_cp.t_issue = mem_now(); g_cp.on = 1;
    pthread_create(&g_cp.th, 0, cp_poll, 0); HIP_CHECK(hipSetDevice(cur));
}
void rns_copy_probe_report(int level_next)
{
    if (!g_cp.on) return; pthread_join(g_cp.th, 0); g_cp.on = 0;
    double td = (g_cp.t_done - g_cp.t_issue) * 1e3, tl = (rns_t_launch - g_cp.t_issue) * 1e3;
    printf("COPY probe: level %d's odd-node copy (%.0f MB into APU %d) completed %.1f ms after issue; level %d's first kernel was launched %.1f ms after issue: %s\n",
           g_cp.level, g_cp.bytes / 1e6, g_cp.dev, td, level_next, tl, td > tl ? "UNORDERED (the copy landed after the next level's kernels had started)" : "ordered");
    HIP_CHECK(hipEventDestroy(g_cp.ev));
    for (int i = 0; i < 2; i++) if (g_cp_n[i]) {                       /* the copied node against its source, both still in place after the next level */
        uint64_t *a = (uint64_t *)malloc(g_cp_n[i] * 8), *b = (uint64_t *)malloc(g_cp_n[i] * 8);
        mem_dev_copy(a, g_cp_dst[i], g_cp_n[i] * 8); mem_dev_copy(b, g_cp_src[i], g_cp_n[i] * 8);
        size_t nd = 0, first = 0; for (size_t k = 0; k < g_cp_n[i]; k++) if (a[k] != b[k]) { if (!nd) first = k; nd++; }
        printf("COPY check after level %d: the odd node's %s (%zu limbs, %p <- %p) vs its source: %zu limbs differ%s\n", level_next, i ? "Q" : "P", g_cp_n[i], (const void *)g_cp_dst[i], (const void *)g_cp_src[i], nd, nd ? "  COPY WRONG" : "");
        if (nd) printf("COPY check: first differing limb %zu: dst %016llx src %016llx\n", first, (unsigned long long)a[first], (unsigned long long)b[first]);
        free(a); free(b); g_cp_n[i] = 0;
    }
}

/* Phase 12 R (D5, instrumentation): ECALC_B_SNAPSHOT=1 -- in the striped grpB tier every device copies the shared operand B
 * into a private buffer on its own stream right before its ntt_load reads it (the same read, microseconds earlier); after
 * the level the four snapshots are compared on the host with B as it is in memory then: a device that read stale limbs is
 * named with the first and last differing limb.  The buffers are allocated at rns_init (1 GiB per APU) so nothing is
 * mapped inside the phase. */
static void rns_snap_check(const char *when)
{
    uint64_t *h = (uint64_t *)malloc(g_snap_n * 8), *hb = (uint64_t *)malloc(g_snap_n * 8);
    mem_dev_copy(hb, g_snap_b, g_snap_n * 8);
    for (int d = 0; d < g_nd; d++) {
        HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemcpy(h, g_snap[d], g_snap_n * 8, hipMemcpyDeviceToHost));
        size_t nd = 0, first = 0, last = 0;
        for (size_t i = 0; i < g_snap_n; i++) if (h[i] != hb[i]) { if (!nd) first = i; last = i; nd++; }
        printf("SNAP %s: APU %d's read of B (%zu limbs at %p) vs B in memory now: %zu limbs differ%s", when, d, g_snap_n, (const void *)g_snap_b, nd, nd ? "" : "\n");
        if (nd) printf(", first at limb %zu (read %016llx, memory %016llx), last at limb %zu  STALE READ\n", first, (unsigned long long)h[first], (unsigned long long)hb[first], last);
    }
    free(h); free(hb); g_snap_n = 0;
}
static int ceil_log2(size_t n) { int l = 0; while (((size_t)1 << l) < n) l++; return l; }
/* WP8: transform length for nc points: 2^logn, or 3 * 2^(logn-2) when nc fits it (0.75x the points) */
int rns_r3 = -1;                                     /* RNS_R3: 1 (default when the prime set allows) */
static int pick_len(size_t nc, int *logk)
{
    int logn = ceil_log2(nc); if (logn < NTT_LOGN_MIN) logn = NTT_LOGN_MIN;
    if (rns_r3 < 0) rns_r3 = getenv("RNS_R3") ? atoi(getenv("RNS_R3")) : ec_has_radix3();
    if (rns_r3 && ec_has_radix3() && logn - 2 >= NTT_LOGN_MIN && nc <= ((size_t)3 << (logn - 2))) { *logk = logn - 2; return 1; }
    *logk = logn; return 0;
}
static size_t len_of(int r3, int logk) { return (size_t)(r3 ? 3 : 1) << logk; }
/* Phase 13a P3: the largest product (points) the mdev tier forms: 2^pool_log, or with three primes (pool 0 = 3/4 of it
 * unless the B3 planes make it larger) 3 2^(pool_log-2) when the 3 2^k lengths are on, else 2^(pool_log-1).  Four primes:
 * always 2^pool_log (unchanged). */
static size_t mdev_pts(void)
{
    size_t p = (size_t)1 << g_pool_log;
    if (ec_np == 4 || !g_nd) return p;
    size_t cap = D[0].da.cap / 8;
    if (cap >= p) return p;
    int k; pick_len(3 * (p / 4), &k);                                  /* (initialises rns_r3) */
    return rns_r3 && ec_has_radix3() && 3 * (p / 4) <= cap ? 3 * (p / 4) : p / 2;
}
static void x_fwd(ntt_ctx *c, uint64_t *x, int r3, int logk, size_t batch, hipStream_t s) { if (r3) ntt_fwd3(c, x, logk, batch, s); else ntt_fwd(c, x, logk, batch, s); }
static void x_inv_pw(ntt_ctx *c, uint64_t *x, const uint64_t *y, int r3, int logk, size_t batch, hipStream_t s) { if (r3) ntt_inv3_pw(c, x, y, logk, batch, s); else ntt_inv_pw(c, x, y, logk, batch, s); }
static void x_inv_pw_bcast(ntt_ctx *c, uint64_t *x, const uint64_t *y, int r3, int logk, size_t batch, hipStream_t s) { if (r3) ntt_inv3_pw_bcast(c, x, y, logk, batch, s); else ntt_inv_pw_bcast(c, x, y, logk, batch, s); }
static void x_inv_pw_y(ntt_ctx *c, uint64_t *x, const uint64_t *y, int ymode, int r3, int logk, size_t batch, hipStream_t s) { if (r3) ntt_inv3_pw_y(c, x, y, ymode, logk, batch, s); else ntt_inv_pw_y(c, x, y, ymode, logk, batch, s); }

/* parallel memcpy by the threads of one NUMA node (called inside the per-device team) */
static void node_copy(uint64_t *dst, const uint64_t *src, size_t n, int node, int nthreads)
{
    if (mem_dev_of(src) >= 0 || mem_dev_of(dst) >= 0) { mem_dev_copy_on(node, dst, src, n * 8); return; }   /* WP3: device pools by DMA */
    if (n < (1u << 20)) { memcpy(dst, src, n * 8); return; }      /* small: no nested team (20 ms of overhead otherwise) */
#pragma omp parallel num_threads(nthreads)
    {
        mem_pin_to_node(node);
#pragma omp for schedule(static)
        for (size_t i = 0; i < n; i += 1 << 16) {
            size_t m = n - i < (1 << 16) ? n - i : (1 << 16);
            memcpy(dst + i, src + i, m * 8);
        }
    }
}

/* GPU CRT of the residue planes in da (one product of nc coefficients, plane
 * length 2^logn) into device 0's registered staging; called by every thread
 * of the per-device team (implemented with the batch tier below) */
static void mdev_gpu_crt(int d, size_t nc, int logn, uint64_t *dst);
int rns_mdev_gpucrt = 1;

static void par_copy(uint64_t *dst, const uint64_t *src, size_t n)
{
    if (mem_dev_of(src) >= 0 || mem_dev_of(dst) >= 0) { mem_dev_copy(dst, src, n * 8); return; }
#pragma omp parallel for schedule(static) if (n > (1u << 22))
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memcpy(dst + i, src + i, m * 8); }
}
static void par_zero(uint64_t *dst, size_t n)
{
#pragma omp parallel for schedule(static) if (n > (1u << 22))
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memset(dst + i, 0, m * 8); }
}


/* one or two products sharing b: C[j] = a[j] * b.  b is transformed once
 * (mdev_pair); the staging buffer holds a[0] | a[1] | b, then the residue
 * planes in turn. */
static void mdev_core(int np, bigint *C[2], const uint64_t *a[2], size_t na[2], const uint64_t *b, size_t nb)
{
    size_t nc[2], ncmax = 0;
    for (int j = 0; j < np; j++) { nc[j] = na[j] + nb; if (nc[j] > ncmax) ncmax = nc[j]; }
    int logk, r3 = pick_len(ncmax, &logk), logn = r3 ? logk + 2 : logk;   /* logn: the 2^k length this replaces (pool checks, stats) */
    size_t n = len_of(r3, logk);
    if (n > ((size_t)1 << g_pool_log) || n > D[0].da.cap / 8) { fprintf(stderr, "rns_mul_mdev: %zu points > pool 2^%d (plane pool 0: %zu limbs)\n", ncmax, g_pool_log, D[0].da.cap / 8); exit(1); }
    if (ec_np == 3) ec_np_check(na[0] < nb ? na[0] : nb, bi_decimal, "rns_mul_mdev");   /* P3 (a pair: both a's are checked below by the CRT's n) */
    if (na[0] + (np > 1 ? na[1] : 0) + nb > ((size_t)1 << g_pool_log)) { fprintf(stderr, "mdev_pair: operands exceed staging\n"); exit(1); }
    double t0 = mem_now(), tcrt = 0;
    double tr[EC_NP], th[EC_NP], tf[EC_NP], ti[EC_NP], td[EC_NP], tg[EC_NP] = {0, 0, 0, 0};
    size_t offb = na[0] + (np > 1 ? na[1] : 0);
    double rp0 = mem_now();
    if (rns_repack_wide) {                     /* all threads, one device after another (RESULTS.md 27: 328 GB/s) */
        for (int d = 0; d < g_nd; d++) {
            par_copy(D[d].hstage, a[0], na[0]);
            if (np > 1) par_copy(D[d].hstage + na[0], a[1], na[1]);
            par_copy(D[d].hstage + offb, b, nb);
        }
    }
    double rp1 = mem_now();
#pragma omp parallel num_threads(g_nd)
    {
        int d = omp_get_thread_num();
        struct dev *v = &D[d];
        hipEvent_t e0, e1, e2, e3;
        float m1, m2, m3;
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1)); HIP_CHECK(hipEventCreate(&e2)); HIP_CHECK(hipEventCreate(&e3));
        double s0 = mem_now();
        const int act = d < ec_np;                     /* P3: device d transforms prime d; with three primes device 3 only joins the GPU CRT */
        if (!rns_repack_wide && act) {
            node_copy(v->hstage, a[0], na[0], mem_numa_node_of_device(d), v->ncpu);
            if (np > 1) node_copy(v->hstage + na[0], a[1], na[1], mem_numa_node_of_device(d), v->ncpu);
            node_copy(v->hstage + offb, b, nb, mem_numa_node_of_device(d), v->ncpu);
        }
        double s1 = mem_now() + (rp1 - rp0);
        uint64_t *da = (uint64_t *)v->da.p, *db = (uint64_t *)v->db.p;
        tf[d] = ti[d] = th[d] = 0;
        HIP_CHECK(hipEventRecord(e0, v->s));
        if (act) ntt_load(v->ctx, db, v->hstage + offb, nb, n, v->s);
        HIP_CHECK(hipEventRecord(e1, v->s));
        if (act) x_fwd(v->ctx, db, r3, logk, 1, v->s);
        HIP_CHECK(hipEventRecord(e2, v->s));
        HIP_CHECK(hipEventSynchronize(e2));
        HIP_CHECK(hipEventElapsedTime(&m1, e0, e1)); HIP_CHECK(hipEventElapsedTime(&m2, e1, e2));
        th[d] += m1 * 1e-3; tf[d] += m2 * 1e-3;
        for (int j = 0; j < np; j++) {
            HIP_CHECK(hipEventRecord(e0, v->s));
            if (act) ntt_load(v->ctx, da, v->hstage + (j ? na[0] : 0), na[j], n, v->s);
            HIP_CHECK(hipEventRecord(e1, v->s));
            if (act) x_fwd(v->ctx, da, r3, logk, 1, v->s);
            HIP_CHECK(hipEventRecord(e2, v->s));
            if (act) x_inv_pw(v->ctx, da, db, r3, logk, 1, v->s);
            HIP_CHECK(hipEventRecord(e3, v->s));
            if (rns_mdev_gpucrt) {
                HIP_CHECK(hipStreamSynchronize(v->s));
                double g0 = mem_now();
                uint64_t *dst = D[0].hstage + (j == np - 1 ? 0 : ((size_t)1 << g_pool_log) - nc[j]);   /* first of a pair: above the operands */
                mdev_gpu_crt(d, nc[j], logn, dst);         /* includes the barriers */
                tg[d] += mem_now() - g0;
                if (d == 0) {
                    bi_reserve(C[j], nc[j] + 4);
                    par_copy(C[j]->l, dst, nc[j]);
                    C[j]->n = nc[j]; bi_norm(C[j]);
                }
#pragma omp barrier                                    /* copy done before the staging is reused */
            } else if (j == np - 1) {
                if (rns_crt_layout) {
                    HIP_CHECK(hipStreamSynchronize(v->s));
#pragma omp barrier
                    size_t Q = (nc[j] + 3) / 4;
                    for (int q = 0; q < g_nd; q++) {
                        size_t q0 = q * Q, q1 = (q + 1) * Q < nc[j] ? (q + 1) * Q : nc[j];
                        if (q0 < q1) k_store<<<228 * 4, 256, 0, v->s>>>(D[q].hstage + d * Q, da + q0, q1 - q0);
                    }
                } else if (act)
                    k_store<<<228 * 8, 256, 0, v->s>>>(v->hstage, da, nc[j]);
            } else {
                /* first product of a pair: keep the result plane in the staging area above the operands
                 * is impossible (operands still needed): store into db? no - db holds B's transform.
                 * Use the top of the staging buffer if it fits, else stage through the host CRT now. */
                if (act) k_store<<<228 * 8, 256, 0, v->s>>>(v->hstage + (((size_t)1 << g_pool_log) - nc[j]), da, nc[j]);
            }
            HIP_CHECK(hipStreamSynchronize(v->s));
            HIP_CHECK(hipEventElapsedTime(&m1, e0, e1)); HIP_CHECK(hipEventElapsedTime(&m2, e1, e2));
            HIP_CHECK(hipEventElapsedTime(&m3, e2, e3));
            th[d] += m1 * 1e-3; tf[d] += m2 * 1e-3; ti[d] += m3 * 1e-3;
        }
        double s2 = mem_now();
        tr[d] = s1 - s0;
        td[d] = (s2 - s1) - th[d] - tf[d] - ti[d];
        HIP_CHECK(hipEventDestroy(e0)); HIP_CHECK(hipEventDestroy(e1)); HIP_CHECK(hipEventDestroy(e2)); HIP_CHECK(hipEventDestroy(e3));
    }
    double t1 = mem_now();
    int T = rns_crt_threads ? rns_crt_threads : omp_get_max_threads();
    uint64_t *planes[EC_NP];
    if (rns_mdev_gpucrt) { tcrt = tg[0]; for (int dd = 0; dd < g_nd; dd++) td[dd] -= tg[dd]; }
    else for (int j = np - 1; j >= 0; j--) {
        bi_reserve(C[j], nc[j] + 4);
        for (int d = 0; d < g_nd; d++) planes[d] = D[d].hstage + (j == np - 1 ? 0 : ((size_t)1 << g_pool_log) - nc[j]);
        if (rns_crt_layout && j == np - 1) crt_carry_par4_q(planes, (nc[j] + 3) / 4, nc[j], C[j]->l, T);
        else crt_carry_par4(planes, nc[j], C[j]->l, T);
        C[j]->n = nc[j] + 4; bi_norm(C[j]);
    }
    if (!rns_mdev_gpucrt) tcrt = mem_now() - t1;
    double mr = 0, mh = 0, mf = 0, mi = 0, md = 0;
    for (int d = 0; d < g_nd; d++) { if (tr[d] > mr) mr = tr[d]; if (th[d] > mh) mh = th[d]; if (tf[d] > mf) mf = tf[d]; if (ti[d] > mi) mi = ti[d]; if (td[d] > md) md = td[d]; }
    rns_st.t_repack += mr; rns_st.t_h2d += mh; rns_st.t_fwd += mf; rns_st.t_inv += mi; rns_st.t_d2h += md;
    rns_st.t_crt += tcrt; rns_st.t_total += mem_now() - t0; rns_st.n_mdev += np; rns_st.points_mdev += n * np;
    if (getenv("RNS_VERBOSE")) printf("mdev%s %s2^%d (%zu limbs): repack %.3f h2d %.3f fwd %.3f inv %.3f d2h %.3f crt %.3f total %.3f s\n",
                                      np > 1 ? "_pair" : "", r3 ? "3*" : "", logk, ncmax, mr, mh, mf, mi, md, tcrt, mem_now() - t0);
}

void rns_mul_mdev(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb)
{
    bigint *Cs[2] = { C, 0 }; const uint64_t *as[2] = { a, 0 }; size_t nas[2] = { na, 0 };
    mdev_core(1, Cs, as, nas, b, nb);
}
/* mdev_pair: C1 = A1 B, C2 = A2 B with B transformed once.  Falls back to two
 * mdevs when the pair does not fit the staging buffer (a1 + a2 + b + result). */
static void rns2_core(int np, bigint *C[2], const uint64_t *a[2], size_t na[2], const uint64_t *b, size_t nb);
void rns_mul_pair(bigint *C1, const bigint *A1, bigint *C2, const bigint *A2, const bigint *B)
{
    size_t pool = mdev_pts();                              /* 2^pool_log (P3: less with three primes) */
    size_t n1 = A1->n + B->n, n2 = A2->n + B->n, nmax = n1 > n2 ? n1 : n2, nmin = n1 < n2 ? n1 : n2;
    if (!A1->n || !A2->n || !B->n || nmax > pool || A1->n + A2->n + B->n + n1 > pool || nmax <= (size_t)rns_school_max) {
        rns_mul(C1, A1, B); rns_mul(C2, A2, B); return;
    }
    bigint *Cs[2] = { C1, C2 }; const uint64_t *as[2] = { A1->l, A2->l }; size_t nas[2] = { A1->n, A2->n };
    if (rns_engine == 2) { if (e2_points(nmax) > pool) { rns_mul(C1, A1, B); rns_mul(C2, A2, B); return; } rns2_core(2, Cs, as, nas, B->l, B->n); return; }
    mdev_core(2, Cs, as, nas, B->l, B->n);
}

/* C = A * B by Karatsuba over halves when the product exceeds the pool */
static void mul_rec(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, int depth);
void rns_free_scratch(void);
void rns2_mul_mdev(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb);

/* grow-only scratch per recursion depth: page-faulted once, reused after */
#define KDEPTH 8
static bigint g_scr[KDEPTH][5];

static void mul_karatsuba(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, int depth)
{
    size_t m = na > nb ? na : nb, h = (m + 1) / 2;
    if (depth >= KDEPTH) { fprintf(stderr, "karatsuba: depth\n"); exit(1); }
    bigint *z0 = &g_scr[depth][0], *z1 = &g_scr[depth][2], *sa = &g_scr[depth][3], *sb = &g_scr[depth][4];
    /* z0 and z2 are formed in place inside C (views: l into C, cap the room
     * available); every tier reserves at most nc + 4 and the Karatsuba below
     * na + nb + 8, so the views get that much room */
    size_t ncap = na + nb + 8;
    bi_reserve(C, ncap);
    par_zero(C->l, ncap);
    C->n = na + nb + 1;
    if (nb <= h || na <= h) {
        /* the short operand is one piece: C = a1 b << 64h (in place at C + h) + a0 b (scratch) */
        if (na < nb) { const uint64_t *t = a; a = b; b = t; size_t tn = na; na = nb; nb = tn; }
        bigint v2 = { C->l + h, 0, ncap - h };
        mul_rec(&v2, a + h, na - h, b, nb, depth + 1);
        mul_rec(z0, a, h, b, nb, depth + 1);
        C->n = na + nb + 1;
        bi_add_shifted(C, z0, 0);
    } else {
        /* a = a1 2^(64h) + a0, b likewise: z0 = a0 b0 at C, z2 = a1 b1 at C + 2h, z1 = (a0+a1)(b0+b1) - z0 - z2 */
        bigint v0 = { C->l, 0, 2 * h + 8 }, v2 = { C->l + 2 * h, 0, ncap - 2 * h };
        mul_rec(&v0, a, h, b, h, depth + 1);
        size_t n0 = v0.n;
        for (size_t i = n0; i < 2 * h + 8 && i < ncap; i++) C->l[i] = 0;          /* clear v0's slack before z2 lands */
        mul_rec(&v2, a + h, na - h, b + h, nb - h, depth + 1);
        bi_reserve(sa, h + 1); bi_reserve(sb, h + 1);
        { uint64_t c = limb_add(sa->l, a, h, a + h, na - h); sa->l[h] = c; sa->n = h + 1; bi_norm(sa); }
        { uint64_t c = limb_add(sb->l, b, h, b + h, nb - h); sb->l[h] = c; sb->n = h + 1; bi_norm(sb); }
        mul_rec(z1, sa->l, sa->n, sb->l, sb->n, depth + 1);
        bigint z0v = { C->l, n0, 0 }, z2v = { C->l + 2 * h, v2.n, 0 };
        bi_sub(z1, z1, &z0v);
        bi_sub(z1, z1, &z2v);
        C->n = na + nb + 1;
        bi_add_shifted(C, z1, h);
    }
    bi_norm(C);
    rns_st.n_split++;
}

/* unbalanced: chunk the long operand so each chunk product fits the pool */
static void mul_chunked(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, int depth)
{
    size_t pool = mdev_pts();                              /* 2^pool_log (P3: less with three primes) */
    if (rns_engine == 2) pool = pool * E2_BITS / 64 - 2;
    size_t chunk = pool - nb;
    if (depth >= KDEPTH) { fprintf(stderr, "chunked: depth\n"); exit(1); }
    bigint *part = &g_scr[depth][0];
    bi_reserve(C, na + nb + 1);
    par_zero(C->l, na + nb + 1);
    C->n = na + nb + 1;
    for (size_t off = 0; off < na; off += chunk) {
        size_t m = na - off < chunk ? na - off : chunk;
        mul_rec(part, a + off, m, b, nb, depth + 1);
        bi_add_shifted(C, part, off);
        C->n = na + nb + 1;
    }
    bi_norm(C);
}

static void mul_rec(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, int depth)
{
    na = limb_norm(a, na); nb = limb_norm(b, nb);
    if (!na || !nb) { C->n = 0; return; }
    size_t nc = na + nb, pool = mdev_pts();                /* 2^pool_log (P3: less with three primes) */
    if (nc <= (size_t)rns_school_max) {
        bi_reserve(C, nc);
        limb_mul_school(C->l, a, na, b, nb);
        C->n = nc; bi_norm(C);
        rns_st.n_school++;
        return;
    }
    if (rns_engine == 2) {
        if (bi_decimal) { fprintf(stderr, "engine 2 is binary-base only\n"); exit(1); }
        size_t pts = e2_points(nc);
        if (pts <= pool) { rns2_mul_mdev(C, a, na, b, nb); return; }
        size_t lim = pool * E2_BITS / 64 - 2;        /* limbs whose 45-bit points fit a plane */
        if (nb * 2 <= lim && nb * 4 <= na) mul_chunked(C, a, na, b, nb, depth);
        else if (na * 2 <= lim && na * 4 <= nb) mul_chunked(C, b, nb, a, na, depth);
        else mul_karatsuba(C, a, na, b, nb, depth);
        return;
    }
    if (nc <= pool) { rns_mul_mdev(C, a, na, b, nb); return; }
    if (nb * 2 <= pool && nb * 4 <= na) mul_chunked(C, a, na, b, nb, depth);
    else if (na * 2 <= pool && na * 4 <= nb) mul_chunked(C, b, nb, a, na, depth);
    else mul_karatsuba(C, a, na, b, nb, depth);
}

void rns_free_scratch(void)
{
    for (int d = 0; d < KDEPTH; d++) for (int i = 0; i < 5; i++) bi_free(&g_scr[d][i]);
}
void rns_mul(bigint *C, const bigint *A, const bigint *B)
{
    if (C == A || C == B) { fprintf(stderr, "rns_mul: aliasing\n"); exit(1); }
    mul_rec(C, A->l, A->n, B->l, B->n, 0);
}

/* ======================================================================== */
/* batch tier                                                                */
/* ======================================================================== */
#include "rns_int.h"

__global__ void k_scatter(uint64_t *da, uint64_t *db, const struct bdesc *P, size_t M, int logL, ec_mod m)
{
    size_t total = M << logL, L = (size_t)1 << logL;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t pi = i >> logL, k = i & (L - 1);
        const struct bdesc d = P[pi];
        da[i] = k < d.na ? ec_canon64(d.a[k], m.pu, m.mu) : 0;
        if (db) db[i] = k < d.nb ? ec_canon64(d.b[k], m.pu, m.mu) : 0;
    }
}

/* Phase 11 A2 (agent P): the striped tier's scatter with the batch-local tier's pair layout (B of products 2j, 2j+1 once,
 * at transform j) and 3 2^logk lengths; one prime per device */
__global__ void k_scatter1(uint64_t *da, uint64_t *db, size_t L, int pair, const struct bdesc *P, size_t M, int logk, int r3, ec_mod m)
{
    size_t total = M * L;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t pi = r3 ? (i >> logk) / 3 : i >> logk, k = i - pi * L;
        const struct bdesc d = P[pi];
        da[i] = k < d.na ? ec_canon64(d.a[k], m.pu, m.mu) : 0;
        if (db && !(pair && (pi & 1))) { size_t ib = pair ? (pi >> 1) * L + k : i; db[ib] = k < d.nb ? ec_canon64(d.b[k], m.pu, m.mu) : 0; }
    }
}

/* device Garner: FP64-Barrett for the modular steps */
__device__ static inline uint64_t dmod128(uint64_t hi, uint64_t lo, const ec_mod m, uint64_t c64)
{
    uint64_t r = (uint64_t)ec_mm((double)ec_canon64(hi, m.pu, m.mu), (double)c64, m.p, m.pinv) + ec_canon64(lo, m.pu, m.mu);
    return ec_fold(r, m.pu);
}
__device__ static inline uint64_t dmod192(uint64_t x2, uint64_t x1, uint64_t x0, const ec_mod m, uint64_t c64)
{
    uint64_t r = dmod128(x2, x1, m, c64);
    r = (uint64_t)ec_mm((double)r, (double)c64, m.p, m.pinv) + ec_canon64(x0, m.pu, m.mu);
    return ec_fold(r, m.pu);
}
__device__ static inline void dgarner4(const struct gconst *g, const uint64_t r[4], uint64_t out[4])
{
    uint64_t x0 = r[0], x1, x2;
    /* t1 */
    { uint64_t xm = ec_canon64(x0, g->m[1].pu, g->m[1].mu), df = ec_fold(r[1] + g->m[1].pu - xm, g->m[1].pu);
      uint64_t t1 = (uint64_t)ec_mm((double)df, (double)g->c1, g->m[1].p, g->m[1].pinv);
      uint64_t lo = t1 * g->m[0].pu, hi = __umul64hi(t1, g->m[0].pu);
      uint64_t s = lo + x0; hi += s < lo; x0 = s; x1 = hi; }
    /* t2 */
    { uint64_t xm = dmod128(x1, x0, g->m[2], g->c64[2]), df = ec_fold(r[2] + g->m[2].pu - xm, g->m[2].pu);
      uint64_t t2 = (uint64_t)ec_mm((double)df, (double)g->c2, g->m[2].p, g->m[2].pinv);
      uint64_t l0 = t2 * g->M1[0], h0 = __umul64hi(t2, g->M1[0]);
      uint64_t l1 = t2 * g->M1[1], h1 = __umul64hi(t2, g->M1[1]);
      uint64_t s0 = l0 + x0, c0 = s0 < l0;
      uint64_t s1 = h0 + l1, c1 = s1 < h0; s1 += x1; c1 += s1 < x1; s1 += c0; c1 += s1 < c0;
      x0 = s0; x1 = s1; x2 = h1 + c1; }
    /* t3 */
    { uint64_t xm = dmod192(x2, x1, x0, g->m[3], g->c64[3]), df = ec_fold(r[3] + g->m[3].pu - xm, g->m[3].pu);
      uint64_t t3 = (uint64_t)ec_mm((double)df, (double)g->c3, g->m[3].p, g->m[3].pinv);
      uint64_t l0 = t3 * g->M2[0], h0 = __umul64hi(t3, g->M2[0]);
      uint64_t l1 = t3 * g->M2[1], h1 = __umul64hi(t3, g->M2[1]);
      uint64_t l2 = t3 * g->M2[2], h2 = __umul64hi(t3, g->M2[2]);
      uint64_t s0 = l0 + x0, c0 = s0 < l0;
      uint64_t s1 = h0 + l1, c1 = s1 < h0; s1 += x1; c1 += s1 < x1; s1 += c0; c1 += s1 < c0;
      uint64_t s2 = h1 + l2, c2 = s2 < h1; s2 += x2; c2 += s2 < x2; s2 += c1; c2 += s2 < c1;
      out[0] = s0; out[1] = s1; out[2] = s2; out[3] = h2 + c2; }
}
/* Phase 13a P3: three primes -- dgarner4's first two steps: x = r0 + t1 p0 + t2 p0 p1 < p0 p1 p2 < 2^156 */
__device__ static inline void dgarner3(const struct gconst *g, const uint64_t r[3], uint64_t out[3])
{
    uint64_t x0 = r[0], x1;
    { uint64_t xm = ec_canon64(x0, g->m[1].pu, g->m[1].mu), df = ec_fold(r[1] + g->m[1].pu - xm, g->m[1].pu);
      uint64_t t1 = (uint64_t)ec_mm((double)df, (double)g->c1, g->m[1].p, g->m[1].pinv);
      uint64_t lo = t1 * g->m[0].pu, hi = __umul64hi(t1, g->m[0].pu);
      uint64_t s = lo + x0; hi += s < lo; x0 = s; x1 = hi; }
    { uint64_t xm = dmod128(x1, x0, g->m[2], g->c64[2]), df = ec_fold(r[2] + g->m[2].pu - xm, g->m[2].pu);
      uint64_t t2 = (uint64_t)ec_mm((double)df, (double)g->c2, g->m[2].p, g->m[2].pinv);
      uint64_t l0 = t2 * g->M1[0], h0 = __umul64hi(t2, g->M1[0]);
      uint64_t l1 = t2 * g->M1[1], h1 = __umul64hi(t2, g->M1[1]);
      uint64_t s0 = l0 + x0, c0 = s0 < l0;
      uint64_t s1 = h0 + l1, c1 = s1 < h0; s1 += x1; c1 += s1 < x1; s1 += c0; c1 += s1 < c0;
      out[0] = s0; out[1] = s1; out[2] = h1 + c1; }
}

/* S stripes per product: global stripe g = pi S + s covers coefficients
 * [nc s / S, nc (s+1) / S).  The block walks its range in chunks of 256
 * consecutive coefficients (coalesced plane reads and result stores):
 * thread j does Garner for coefficient c0 + j into LDS (4 limbs), then
 * limb k = C0[k] + C1[k-1] + C2[k-2] + C3[k-3] + carry, the carry chain run
 * by thread 0 over LDS, then a coalesced store.  Contributions past the
 * stripe's end form its 4-limb spill for the CPU merge. */
__global__ __launch_bounds__(CRT_THREADS)
void k_crt_batch(const uint64_t *p0, const uint64_t *p1, const uint64_t *p2, const uint64_t *p3,
                 const struct bdesc *P, size_t first_stripe, int S, size_t Lpts, struct gconst g, uint64_t *stripe_spill, int dec)
{
    const uint64_t BB = EC_1E18;
    __shared__ uint64_t C0[CRT_THREADS], C1[CRT_THREADS + 3], C2[CRT_THREADS + 3], C3[CRT_THREADS + 3];
    __shared__ uint64_t V[CRT_THREADS], CY[CRT_THREADS];
    __shared__ uint64_t carry;
    const size_t gs = first_stripe + blockIdx.x, pi = gs / S, s = gs % S, base = pi * Lpts;
    const struct bdesc d = P[pi];
    const uint32_t nc = d.na + d.nb;
    const uint32_t K0 = (uint32_t)((uint64_t)nc * s / S), K1 = (uint32_t)((uint64_t)nc * (s + 1) / S);
    uint64_t *out = d.c;
    const int j = threadIdx.x;
    if (j < 3) { C1[j] = 0; C2[j] = 0; C3[j] = 0; }
    if (j == 0) carry = 0;
    __syncthreads();
    for (uint32_t c0 = K0; c0 < K1; c0 += CRT_THREADS) {
        uint32_t k = c0 + j;
        uint64_t c[4] = {0, 0, 0, 0};
        uint64_t xk = (k < K1 && k < d.nx) ? d.x[k] : 0;                 /* the added operand's limb */
        if (k < K1 && g.np == 3) {                                       /* P3: three primes, decimal only (ec_np_check); p3 is not read */
            uint64_t r[3] = { p0[base + k], p1[base + k], p2[base + k] }, w[3], d[3]; dgarner3(&g, r, w);
            ec_words_to_dec3(w, d); c[0] = d[0]; c[1] = d[1]; c[2] = d[2];
        } else if (k < K1) { uint64_t r[4] = { p0[base + k], p1[base + k], p2[base + k], p3[base + k] }; dgarner4(&g, r, c);
                      if (dec) { uint64_t d[4]; ec_words_to_dec4(c, d); c[0] = d[0]; c[1] = d[1]; c[2] = d[2]; c[3] = d[3]; } }
        C0[j] = c[0]; C1[j + 3] = c[1]; C2[j + 3] = c[2]; C3[j + 3] = c[3];
        __syncthreads();
        if (dec) {  /* digits < B: the sum of five digits < 5B fits a word */
            uint64_t sm = C0[j] + C1[j + 2] + C2[j + 1] + C3[j] + xk, q = 0;
            while (sm >= BB) { sm -= BB; q++; }
            V[j] = sm; CY[j] = q;
        } else {   /* limb j of this chunk: C0[j] + C1[j-1] + C2[j-2] + C3[j-3] + x[k] (indices shifted by 3) */
            unsigned __int128 sm = (unsigned __int128)C0[j] + C1[j + 2] + C2[j + 1] + C3[j] + xk;
            V[j] = (uint64_t)sm; CY[j] = (uint64_t)(sm >> 64);
        }
        __syncthreads();
        if (j == 0) {
            uint64_t cy = carry;
            int n = (int)(K1 - c0 < CRT_THREADS ? K1 - c0 : CRT_THREADS);
            if (dec) for (int i = 0; i < n; i++) { uint64_t sm = V[i] + cy; cy = CY[i]; if (sm >= BB) { sm -= BB; cy++; } V[i] = sm; }
            else for (int i = 0; i < n; i++) { uint64_t sm = V[i] + cy; cy = CY[i] + (sm < V[i]); V[i] = sm; }
            carry = cy;
        }
        __syncthreads();
        if (k < K1) out[k] = V[j];
        /* tails for the next chunk: the last three C1/C2/C3 entries */
        __syncthreads();
        if (j < 3) { int n = (int)(K1 - c0 < CRT_THREADS ? K1 - c0 : CRT_THREADS); C1[j] = C1[n + j]; C2[j] = C2[n + j]; C3[j] = C3[n + j]; }
        __syncthreads();
    }
    if (j == 0) {
        /* spill at K1: [C1[-1] + C2[-2] + C3[-3] + carry, C2[-1] + C3[-2], C3[-1], overflow] */
        uint64_t *sp = stripe_spill + gs * 4;
        if (dec) {
            uint64_t s0 = C1[2] + C2[1] + C3[0] + carry, q0 = 0; while (s0 >= BB) { s0 -= BB; q0++; }
            uint64_t s1 = C2[2] + C3[1] + q0, q1 = 0; while (s1 >= BB) { s1 -= BB; q1++; }
            uint64_t s2 = C3[2] + q1, q2 = 0; while (s2 >= BB) { s2 -= BB; q2++; }
            sp[0] = s0; sp[1] = s1; sp[2] = s2; sp[3] = q2;
            if (d.nx && s == S - 1) out[nc] = s0;                 /* a b + x < 2 B^nc: the final carry is the top limb */
        } else {
            unsigned __int128 s0 = (unsigned __int128)C1[2] + C2[1] + C3[0] + carry;
            unsigned __int128 s1 = (unsigned __int128)C2[2] + C3[1] + (uint64_t)(s0 >> 64);
            unsigned __int128 s2 = (unsigned __int128)C3[2] + (uint64_t)(s1 >> 64);
            sp[0] = (uint64_t)s0; sp[1] = (uint64_t)s1; sp[2] = (uint64_t)s2; sp[3] = (uint64_t)(s2 >> 64);
            if (d.nx && s == S - 1) out[nc] = (uint64_t)s0;
        }
    }
}

struct gconst rns_gconst(void)
{
    struct gconst g;
    unsigned __int128 m1 = (unsigned __int128)ec_P[0] * ec_P[1];
    for (int i = 0; i < 4; i++) { g.m[i] = ec_mod_get(i); g.c64[i] = (uint64_t)(((unsigned __int128)1 << 64) % ec_P[i]); }
    g.c1 = ec_inv(ec_P[0] % ec_P[1], ec_P[1]);
    g.M1[0] = (uint64_t)m1; g.M1[1] = (uint64_t)(m1 >> 64);
    g.c2 = ec_inv((uint64_t)(m1 % ec_P[2]), ec_P[2]);
    { unsigned __int128 lo = (unsigned __int128)g.M1[0] * ec_P[2], hi = (unsigned __int128)g.M1[1] * ec_P[2] + (lo >> 64);
      g.M2[0] = (uint64_t)lo; g.M2[1] = (uint64_t)hi; g.M2[2] = (uint64_t)(hi >> 64); }
    { unsigned __int128 t = g.M2[2] % ec_P[3]; t = ((t << 64) | g.M2[1]) % ec_P[3]; t = ((t << 64) | g.M2[0]) % ec_P[3];
      g.c3 = ec_inv((uint64_t)t, ec_P[3]); }
    g.np = ec_np_init();                         /* P3: the kernel's prime count */
    return g;
}

static struct bdesc *g_desc[EC_NP];      /* device-side descriptor arrays, grown as needed */
static size_t g_desc_cap[EC_NP];

static void rns2_mul_batch(rns_prod *P, size_t N);

/* WP3: the locality-aware batch tier.  Every product's result lives in a device
 * pool (RESULTS.md 55); the device that owns the result computes all four
 * primes of that product itself: scatter reads its own node's memory at HBM
 * rate, the CRT reads its own four planes, the result is written in place.
 * No staging, no peer traffic except the boundary pairs whose operands sit in a
 * neighbouring region.  Planes: da holds EC_NP planes of M x L (a, then the
 * products), db the B planes (one plane when every product shares B). */
__global__ void k_scatter4(uint64_t *da, uint64_t *db, size_t plane, size_t plane_b, int pair, const struct bdesc *P, size_t M, int logk, int r3, ec_mod m0, ec_mod m1, ec_mod m2, ec_mod m3, int np)   /* P3: np = 3 leaves plane 3 unwritten */
{
    size_t L = (size_t)(r3 ? 3 : 1) << logk, total = M * L;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t pi = r3 ? (i >> logk) / 3 : i >> logk, k = i - pi * L;
        const struct bdesc d = P[pi];
        uint64_t a = k < d.na ? d.a[k] : 0;
        da[i] = ec_canon64(a, m0.pu, m0.mu); da[i + plane] = ec_canon64(a, m1.pu, m1.mu); da[i + 2 * plane] = ec_canon64(a, m2.pu, m2.mu); if (np > 3) da[i + 3 * plane] = ec_canon64(a, m3.pu, m3.mu);
        if (db && !(pair && (pi & 1))) {                       /* B1: pair mode, B of products 2j, 2j+1 once, at transform j */
            uint64_t b = k < d.nb ? d.b[k] : 0; size_t ib = pair ? (pi >> 1) * L + k : i;
            db[ib] = ec_canon64(b, m0.pu, m0.mu); db[ib + plane_b] = ec_canon64(b, m1.pu, m1.mu); db[ib + 2 * plane_b] = ec_canon64(b, m2.pu, m2.mu); if (np > 3) db[ib + 3 * plane_b] = ec_canon64(b, m3.pu, m3.mu); }
    }
}
__global__ void k_norm(const struct bdesc *P, size_t M, uint32_t *len)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= M) return;
    const struct bdesc d = P[i]; uint32_t n = d.na + d.nb + (d.nx ? 1 : 0);
    while (n && d.c[n - 1] == 0) n--;
    len[i] = n;
}
static void spill_merge(const rns_prod *q, const uint64_t *sp, int S)
{
    size_t nc = q->na + q->nb, top = nc + (q->x && q->nx ? 1 : 0); uint64_t *out = q->c;   /* with x the carry limb nc exists */
    for (int s = 0; s + 1 < S; s++) {
        size_t k = nc * (s + 1) / S; const uint64_t *w = sp + s * 4; uint64_t cy = 0;
        if (bi_decimal) {
            for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = out[k] + w[t] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; }
            while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; k++; }
        } else {
            for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = out[k] + w[t], c1 = sm < out[k]; sm += cy; c1 += sm < cy; out[k] = sm; cy = c1; }
            while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm < cy; out[k] = sm; k++; }
        }
    }
}
__global__ void k_spill_merge(const struct bdesc *P, size_t M, const uint64_t *sp, int S, int decimal)   /* Phase 14 R1 (E8): spill_merge on the device, one thread per product (a VMM pool takes no CPU access) */
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; if (i >= M) return;
    const struct bdesc d = P[i]; size_t nc = d.na + d.nb, top = nc + (d.nx ? 1 : 0); uint64_t *out = d.c; const uint64_t *spi = sp + i * (size_t)S * 4;
    for (int s = 0; s + 1 < S; s++) {
        size_t k = nc * (s + 1) / S; const uint64_t *w = spi + s * 4; uint64_t cy = 0;
        if (decimal) {
            for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = out[k] + w[t] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; }
            while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; k++; }
        } else {
            for (int t = 0; t < 4 && k < nc; t++, k++) { uint64_t sm = out[k] + w[t], c1 = sm < out[k]; sm += cy; c1 += sm < cy; out[k] = sm; cy = c1; }
            while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm < cy; out[k] = sm; k++; }
        }
    }
}
static void rns_mul_batch_local(rns_prod *P, size_t N, int logL, int grpB, size_t maxnc)
{
    int logk, r3 = pick_len(maxnc, &logk); (void)logL;
    size_t L = len_of(r3, logk), plane_cap = rns_plane_limbs();   /* B3: pool 0's real capacity (3 2^(pool_log-1) with the 3 2^k planes) */
    size_t Mmax = plane_cap / (ec_np * L); if (Mmax < 1) { fprintf(stderr, "rns_mul_batch_local: L %zu does not fit the pools\n", L); exit(1); }
    /* Phase 9 B1: products 2j, 2j+1 with the same B (the tree's P1 Q2 + P2 and Q1 Q2) on the same device: B is
     * scattered and transformed once per pair (M/2 B transforms per tile), the fused inverse reads y transform
     * t >> 1 for x transform t.  Pairs stay adjacent in every device's list (both go to the region's device, in
     * order) and tiles start at even positions when Mmax is even. */
    if (rns_batch_pair < 0) rns_batch_pair = getenv("RNS_BATCH_PAIR") ? atoi(getenv("RNS_BATCH_PAIR")) : 1;
    int pair = rns_batch_pair && !grpB && N >= 2 && !(N & 1);
    for (size_t i = 0; i < N && pair; i += 2)
        if (P[i].b != P[i + 1].b || P[i].nb != P[i + 1].nb || mem_dev_of(P[i].c) != mem_dev_of(P[i + 1].c)) pair = 0;
    if (pair) {                                    /* tile budget over a (M) + b (M/2) planes; at least one pair when the pools allow it */
        size_t tile_limit = rns_batch_tile_bytes * 2 / (3 * L * 8) / ec_np; if (tile_limit < 2) tile_limit = 2;
        if (Mmax > tile_limit) Mmax = tile_limit;
        if (Mmax < 2) pair = 0; else Mmax &= ~(size_t)1;
    }
    if (!pair) { size_t tile_limit = rns_batch_tile_bytes / (3 * L * 8) / ec_np; if (tile_limit >= 1 && Mmax > tile_limit) Mmax = tile_limit; }
    static struct gconst G; static int ginit = 0;
    if (!ginit || G.np != ec_np) { G = rns_gconst(); ginit = 1; }   /* (P3: refreshed when the prime count changes) */
    /* products by owning device */
    size_t *idx = (size_t *)malloc(N * sizeof *idx), cnt[EC_NP] = {0}, start[EC_NP + 1];
    for (size_t i = 0; i < N; i++) cnt[mem_dev_of(P[i].c)]++;
    start[0] = 0; for (int d = 0; d < g_nd; d++) start[d + 1] = start[d] + cnt[d];
    { size_t fill[EC_NP]; for (int d = 0; d < g_nd; d++) fill[d] = start[d];
      for (size_t i = 0; i < N; i++) idx[fill[mem_dev_of(P[i].c)]++] = i; }
    double tsc = 0, tnt = 0, tcr = 0, tmg = 0;
    rns_t_launch = mem_now();                                          /* Phase 12 R: the copy probe's reference */
#pragma omp parallel num_threads(g_nd)
    {
        int d = omp_get_thread_num(); struct dev *v = &D[d];
        HIP_CHECK(hipSetDevice(d));
        for (int p = 0; p < ec_np; p++) if (!v->ctxp[p]) v->ctxp[p] = p == d ? v->ctx : ntt_ctx_create(p);
        rns_dpool(d, 0, (size_t)ec_np * Mmax * L * 8); rns_dpool(d, 1, (grpB ? (size_t)ec_np * L : (size_t)ec_np * (pair ? Mmax / 2 : Mmax) * L) * 8);   /* A-mem C4: pool 1 is 3 q by default; grown here if a tile needs more (rare: L = 2^28, 2^29 at 2^31 pools). P: in pair mode pool 1 holds M/2 B planes per prime.  Phase 12 R: grpB holds EC_NP B planes (db + p L below), not one -- the request said L (reached only with RNS_BATCH_LOCAL_MIN <= 2) */
        uint64_t *da = (uint64_t *)v->da.p, *db = (uint64_t *)v->db.p;
        double lsc = 0, lnt = 0, lcr = 0, lmg = 0;
        struct bdesc *hd = (struct bdesc *)malloc((Mmax < cnt[d] ? Mmax : cnt[d] ? cnt[d] : 1) * sizeof *hd);
        if (grpB) {                                   /* one B plane per prime, transformed once */
            for (int p = 0; p < ec_np; p++) { ntt_load(v->ctxp[p], db + p * L, P[0].b, P[0].nb, L, v->s); x_fwd(v->ctxp[p], db + p * L, r3, logk, 1, v->s); }
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
        for (size_t first = start[d]; first < start[d + 1]; first += Mmax) {
            size_t M = start[d + 1] - first < Mmax ? start[d + 1] - first : Mmax, plane = M * L, Mb = pair ? M / 2 : M, plane_b = Mb * L;
            for (size_t i = 0; i < M; i++) { const rns_prod *q = &P[idx[first + i]]; hd[i].a = q->a; hd[i].b = q->b; hd[i].c = q->c; hd[i].x = q->x; hd[i].na = (uint32_t)q->na; hd[i].nb = (uint32_t)q->nb; hd[i].nx = (uint32_t)(q->x ? q->nx : 0); }
            if (g_desc_cap[d] < M) { if (g_desc[d]) HIP_CHECK(hipFree(g_desc[d])); HIP_CHECK(hipMalloc(&g_desc[d], M * sizeof(struct bdesc))); g_desc_cap[d] = M; }
            HIP_CHECK(hipMemcpyAsync(g_desc[d], hd, M * sizeof(struct bdesc), hipMemcpyHostToDevice, v->s));
            int S = (int)((rns_gpucrt_blocks + M - 1) / M); if (S < 1) S = 1;
            if ((size_t)S > (L >> 10)) S = (int)(L >> 10) > 0 ? (int)(L >> 10) : 1;
            if (v->spill_cap < M * S) { if (v->spill) mem_hreg_free(v->spill); v->spill_cap = M * S + 64; v->spill = (uint64_t *)mem_hreg_alloc(v->spill_cap * 4 * 8); }
            size_t blocks = (plane + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
            hipEvent_t e0, e1, e2, e3; float ms1, ms2, ms3;
            HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1)); HIP_CHECK(hipEventCreate(&e2)); HIP_CHECK(hipEventCreate(&e3));
            HIP_CHECK(hipEventRecord(e0, v->s));
            k_scatter4<<<(unsigned)blocks, 256, 0, v->s>>>(da, grpB ? 0 : db, plane, plane_b, pair, g_desc[d], M, logk, r3, ec_mod_get(0), ec_mod_get(1), ec_mod_get(2), ec_mod_get(3), ec_np);
            HIP_CHECK(hipEventRecord(e1, v->s));
            for (int p = 0; p < ec_np; p++) {
                x_fwd(v->ctxp[p], da + p * plane, r3, logk, M, v->s);
                if (grpB) x_inv_pw_bcast(v->ctxp[p], da + p * plane, db + p * L, r3, logk, M, v->s);
                else { x_fwd(v->ctxp[p], db + p * plane_b, r3, logk, Mb, v->s); x_inv_pw_y(v->ctxp[p], da + p * plane, db + p * plane_b, pair ? NTT_Y_PAIR : NTT_Y_FULL, r3, logk, M, v->s); }
            }
            HIP_CHECK(hipEventRecord(e2, v->s));
            k_crt_batch<<<(unsigned)(M * S), CRT_THREADS, 0, v->s>>>(da, da + plane, da + 2 * plane, da + 3 * plane, g_desc[d], 0, S, L, G, v->spill, bi_decimal);
            HIP_CHECK(hipEventRecord(e3, v->s));
            HIP_CHECK(hipStreamSynchronize(v->s));
            double tm0 = mem_now();
            if (S > 1) {
                if (db_pool_vmm_on()) { k_spill_merge<<<(unsigned)((M + 255) / 256), 256, 0, v->s>>>(g_desc[d], M, v->spill, S, bi_decimal); HIP_CHECK(hipStreamSynchronize(v->s)); }   /* Phase 14 R1 (E8): the outputs may be a VMM range */
                else for (size_t i = 0; i < M; i++) spill_merge(&P[idx[first + i]], v->spill + i * S * 4, S);
            }
            /* normalised lengths (the CPU reading one limb per product from device memory is latency-bound) */
            if (v->len_cap < M) { if (v->len) { HIP_CHECK(hipHostFree(v->len)); HIP_CHECK(hipFree(v->dlen)); } v->len_cap = M + 1024; HIP_CHECK(hipHostMalloc((void **)&v->len, v->len_cap * 4, 0)); HIP_CHECK(hipMalloc(&v->dlen, v->len_cap * 4)); }
            k_norm<<<(unsigned)((M + 255) / 256), 256, 0, v->s>>>(g_desc[d], M, v->dlen);
            HIP_CHECK(hipMemcpyAsync(v->len, v->dlen, M * 4, hipMemcpyDeviceToHost, v->s));
            HIP_CHECK(hipStreamSynchronize(v->s));
            for (size_t i = 0; i < M; i++) P[idx[first + i]].ncn = v->len[i];
            lmg += mem_now() - tm0;
            HIP_CHECK(hipEventElapsedTime(&ms1, e0, e1)); HIP_CHECK(hipEventElapsedTime(&ms2, e1, e2)); HIP_CHECK(hipEventElapsedTime(&ms3, e2, e3));
            lsc += ms1 * 1e-3; lnt += ms2 * 1e-3; lcr += ms3 * 1e-3;
            HIP_CHECK(hipEventDestroy(e0)); HIP_CHECK(hipEventDestroy(e1)); HIP_CHECK(hipEventDestroy(e2)); HIP_CHECK(hipEventDestroy(e3));
        }
        free(hd);
#pragma omp critical
        { if (lsc > tsc) tsc = lsc; if (lnt > tnt) tnt = lnt; if (lcr > tcr) tcr = lcr; if (lmg > tmg) tmg = lmg; }
    }
    free(idx);
    rns_st.tb_scatter += tsc; rns_st.tb_ntt += tnt; rns_st.tb_crt += tcr; rns_st.tb_merge += tmg; rns_st.n_batch_local += N; if (pair) rns_st.n_batch_pair += N;
    if (getenv("RNS_VERBOSE")) printf("batch-local N=%zu (%zu/%zu/%zu/%zu) L=%s2^%d tile %zu%s%s: scatter %.3f ntt %.3f crt %.3f merge %.3f\n", N, cnt[0], cnt[1], cnt[2], cnt[3], r3 ? "3*" : "", logk, Mmax, grpB ? " grpB" : "", pair ? " pair" : "", tsc, tnt, tcr, tmg);
}

void rns_mul_batch(rns_prod *P, size_t N)
{
    if (!N) return;
    if (rns_engine == 2) { rns2_mul_batch(P, N); return; }
    if (rns_batch_local < 0) { rns_batch_local = getenv("RNS_BATCH_LOCAL") ? atoi(getenv("RNS_BATCH_LOCAL")) : 1; if (getenv("RNS_BATCH_LOCAL_MIN")) rns_batch_local_min = atoi(getenv("RNS_BATCH_LOCAL_MIN")); }
    size_t maxnc = 0, in_limbs = 0, out_limbs = 0;
    int staged = 0;
    for (size_t i = 0; i < N; i++) {
        size_t nc = P[i].na + P[i].nb;
        if (nc > maxnc) maxnc = nc;
        in_limbs += nc; out_limbs += nc;
        if (!mem_is_registered(P[i].a, P[i].na * 8) || !mem_is_registered(P[i].b, P[i].nb * 8) || !mem_is_registered(P[i].c, (nc + (P[i].x ? 1 : 0)) * 8) || (P[i].x && !mem_is_registered(P[i].x, P[i].nx * 8))) {
            if (!staged && getenv("RNS_VERBOSE")) printf("batch: product %zu not registered (a %p na %zu, b %p, c %p nc %zu)\n", i, (void *)P[i].a, P[i].na, (void *)P[i].b, (void *)P[i].c, nc);
            staged = 1;
        }
    }
    if (ec_np == 3) { size_t mt = 0; for (size_t i = 0; i < N; i++) { size_t t = P[i].na < P[i].nb ? P[i].na : P[i].nb; if (t > mt) mt = t; } ec_np_check(mt, bi_decimal, "rns_mul_batch"); }   /* P3 */
    int logL = ceil_log2(maxnc); if (logL < NTT_LOGN_MIN) logL = NTT_LOGN_MIN;
    if (logL > RNS_BATCH_LOGL_MAX) { fprintf(stderr, "rns_mul_batch: L 2^%d too large\n", logL); exit(1); }
    size_t L = (size_t)1 << logL;
    int grpB = N > 1;                                   /* every product shares one B: transform it once */
    for (size_t i = 1; i < N && grpB; i++) if (P[i].b != P[0].b || P[i].nb != P[0].nb) grpB = 0;
    if (rns_batch_local && !staged && N >= (size_t)rns_batch_local_min && (rns_plane_limbs() / (ec_np * L)) >= 1) {
        int local = 1;                                  /* few or huge products: the prime-per-device path balances the four APUs */
        for (size_t i = 0; i < N && local; i++) if (mem_dev_of(P[i].c) < 0) local = 0;
        if (local) {
            double tl = mem_now();
            rns_mul_batch_local(P, N, logL, grpB, maxnc);
            rns_st.t_total += mem_now() - tl; rns_st.n_batch += N; rns_st.tb_total += mem_now() - tl;
            return;
        }
    }
    /* Phase 11 A2 (agent P): the striped path pairs products 2j, 2j+1 sharing B (the tree's P1 Q2 + P2, Q1 Q2 -- at 4e10 the
     * level of 8 products at 2^30 points, which the batch-local tier cannot hold) and takes the 3 2^k length when nc fits it:
     * per pair 5 transforms of 3 2^28 points instead of 6 of 2^30.  Both need the GPU CRT (the host CRT path reads 2^logL
     * planes from the staging); RNS_STRIPED_PAIR=0 restores the old form (RNS_BATCH_PAIR=0 / RNS_R3=0 also apply). */
    int logk = logL, r3 = 0, pair = 0;
    if (rns_batch_pair < 0) rns_batch_pair = getenv("RNS_BATCH_PAIR") ? atoi(getenv("RNS_BATCH_PAIR")) : 1;
    static int striped_pair = -1; if (striped_pair < 0) striped_pair = getenv("RNS_STRIPED_PAIR") ? atoi(getenv("RNS_STRIPED_PAIR")) : 1;   /* the switch for A2 as a unit (pair + 3 2^k in this path) */
    if (striped_pair && rns_gpucrt_min <= 1 && !staged) {
        r3 = pick_len(maxnc, &logk); if (!r3) logk = logL;
        pair = rns_batch_pair && !grpB && N >= 2 && !(N & 1);
        for (size_t i = 0; i < N && pair; i += 2) if (P[i].b != P[i + 1].b || P[i].nb != P[i + 1].nb) pair = 0;
    }
    size_t L3 = len_of(r3, logk);                       /* the plane length (L stays the 2^logL of the host paths) */
    size_t cap0 = D[0].da.cap / 8, cap1 = D[0].db.cap / 8;   /* the pools as they are: no growth inside the phase for the new forms */
    if (r3 && (L3 > cap0 || (!grpB && L3 > cap1))) { r3 = 0; logk = logL; L3 = L; }
    size_t Mmax = pair ? rns_batch_tile_bytes * 2 / (3 * L3 * 8) : rns_batch_tile_bytes / (3 * L3 * 8);
    if (Mmax > ((size_t)1 << g_pool_log) / L / 2) Mmax = ((size_t)1 << g_pool_log) / L / 2;   /* plane stores use the low half of hstage */
    if (pair) {
        size_t mp = cap0 / L3, mb = 2 * (cap1 / L3); if (mb < mp) mp = mb;   /* a pair: 2 A planes in pool 0, 1 B plane in pool 1 */
        if (Mmax < 2) Mmax = 2; if (Mmax > mp) Mmax = mp; Mmax &= ~(size_t)1;
        if (Mmax < 2) { pair = 0; Mmax = rns_batch_tile_bytes / (3 * L3 * 8); if (Mmax > ((size_t)1 << g_pool_log) / L / 2) Mmax = ((size_t)1 << g_pool_log) / L / 2; }
    }
    if (Mmax < 1) Mmax = 1;
    static struct gconst G; static int ginit = 0;
    if (!ginit || G.np != ec_np) { G = rns_gconst(); ginit = 1; }   /* (P3: refreshed when the prime count changes) */
    double t0 = mem_now();

    /* staging fallback: copy the tile's operands into every device's staging
     * buffer at the same offsets, results into device 0's; then memcpy out */
    rns_prod *Q = P;
    rns_prod *stagedP = 0;
    if (staged) {
        size_t cap = (size_t)1 << (g_pool_log - 1);        /* staging region: top half of device 0's buffer */
        if (in_limbs + out_limbs > cap) {          /* too big for one staging pass: split the batch */
            size_t half = N / 2;
            rns_mul_batch(P, half); rns_mul_batch(P + half, N - half);
            return;
        }
        stagedP = (rns_prod *)calloc(N, sizeof *stagedP);
        size_t off = cap;
        for (size_t i = 0; i < N; i++) {
            if (P[i].x && P[i].nx) { fprintf(stderr, "rns_mul_batch: an added operand needs registered products\n"); exit(1); }
            stagedP[i].na = P[i].na; stagedP[i].nb = P[i].nb;
            stagedP[i].a = D[0].hstage + off; off += P[i].na;
            stagedP[i].b = D[0].hstage + off; off += P[i].nb;
            stagedP[i].c = D[0].hstage + off; off += P[i].na + P[i].nb;
        }
        /* device 0's staging holds everything; the other devices read it remotely */
#pragma omp parallel for schedule(dynamic, 64)
        for (size_t i = 0; i < N; i++) {
            memcpy((void *)stagedP[i].a, P[i].a, P[i].na * 8);
            memcpy((void *)stagedP[i].b, P[i].b, P[i].nb * 8);
        }
        Q = stagedP;
    }

    struct bdesc *hd = (struct bdesc *)malloc(N * sizeof *hd);
    for (size_t i = 0; i < N; i++) { hd[i].a = Q[i].a; hd[i].b = Q[i].b; hd[i].c = Q[i].c; hd[i].x = Q[i].x; hd[i].na = (uint32_t)Q[i].na; hd[i].nb = (uint32_t)Q[i].nb; hd[i].nx = (uint32_t)(Q[i].x ? Q[i].nx : 0); }
    rns_t_launch = mem_now();                                          /* Phase 12 R: the copy probe's reference (grpB's ntt_load or the first tile's scatter follows) */

    if (grpB) {
#pragma omp parallel num_threads(g_nd)
        {
            int d = omp_get_thread_num(); struct dev *v = &D[d];
            HIP_CHECK(hipSetDevice(d));
            rns_dpool(d, 1, L3 * 8);                   /* A-mem C4 */
            if (g_snap_on && Q[0].nb * 8 <= g_snap_cap) { k_store<<<228 * 8, 256, 0, v->s>>>(g_snap[d], Q[0].b, Q[0].nb); g_snap_n = Q[0].nb; g_snap_b = Q[0].b; }   /* Phase 12 R witness: what this device reads of B, on its stream just before ntt_load reads it */
            if (d < ec_np) {                           /* P3: device d transforms prime d (three primes: device 3 joins only the CRT) */
            ntt_load(v->ctx, (uint64_t *)v->db.p, Q[0].b, Q[0].nb, L3, v->s);
            x_fwd(v->ctx, (uint64_t *)v->db.p, r3, logk, 1, v->s);
            }
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
    }
    for (size_t first = 0; first < N; first += Mmax) {
        size_t M = N - first < Mmax ? N - first : Mmax;
        double tsc = 0, tnt = 0, tcr = 0;
        int gpucrt = M >= (size_t)rns_gpucrt_min;
        int S = (int)((rns_gpucrt_blocks + M - 1) / M); if (S < 1) S = 1;
        if ((size_t)S > (L3 >> 10)) S = (int)(L3 >> 10) > 0 ? (int)(L3 >> 10) : 1;   /* >= 1024 coefficients per stripe */
        if (g_spill_cap < M * S) { if (g_spill) mem_hreg_free(g_spill); g_spill_cap = M * S + 64; g_spill = (uint64_t *)mem_hreg_alloc(g_spill_cap * 4 * 8); }
        size_t Mb = pair ? M / 2 : M;                     /* B planes in the tile */
#pragma omp parallel num_threads(g_nd)
        {
            int d = omp_get_thread_num();
            struct dev *v = &D[d];
            HIP_CHECK(hipSetDevice(d));
            if (g_desc_cap[d] < M) {
                if (g_desc[d]) HIP_CHECK(hipFree(g_desc[d]));
                HIP_CHECK(hipMalloc(&g_desc[d], M * sizeof(struct bdesc)));
                g_desc_cap[d] = M;
            }
            HIP_CHECK(hipMemcpyAsync(g_desc[d], hd + first, M * sizeof(struct bdesc), hipMemcpyHostToDevice, v->s));
            rns_dpool(d, 0, (size_t)M * L3 * 8); rns_dpool(d, 1, (grpB ? L3 : (size_t)Mb * L3) * 8);   /* A-mem C4: pool 1 is 3 q by default (the 2^30 tile at 2^30 pools needs the full pool) */
            uint64_t *da = (uint64_t *)v->da.p, *db = (uint64_t *)v->db.p;
            size_t total = M * L3, blocks = (total + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
            hipEvent_t e0, e1, e2, e3; float ms1, ms2, ms3 = 0;
            HIP_CHECK(hipEventCreate(&e0)); HIP_CHECK(hipEventCreate(&e1)); HIP_CHECK(hipEventCreate(&e2)); HIP_CHECK(hipEventCreate(&e3));
            const int act = d < ec_np;                 /* P3: device d transforms prime d (three primes: device 3 joins only the GPU CRT) */
            HIP_CHECK(hipEventRecord(e0, v->s));
            if (act && (r3 || pair)) k_scatter1<<<(unsigned)blocks, 256, 0, v->s>>>(da, grpB ? 0 : db, L3, pair, g_desc[d], M, logk, r3, ec_mod_get(d));
            else if (act) k_scatter<<<(unsigned)blocks, 256, 0, v->s>>>(da, grpB ? 0 : db, g_desc[d], M, logL, ec_mod_get(d));
            HIP_CHECK(hipEventRecord(e1, v->s));
            if (act) {
            x_fwd(v->ctx, da, r3, logk, M, v->s);
            if (grpB) x_inv_pw_bcast(v->ctx, da, db, r3, logk, M, v->s);
            else { x_fwd(v->ctx, db, r3, logk, Mb, v->s); x_inv_pw_y(v->ctx, da, db, pair ? NTT_Y_PAIR : NTT_Y_FULL, r3, logk, M, v->s); }
            }
            HIP_CHECK(hipEventRecord(e2, v->s));
            HIP_CHECK(hipStreamSynchronize(v->s));
            if (gpucrt) {
#pragma omp barrier                                    /* all four planes complete */
                size_t g0 = M * S * d / g_nd, g1 = M * S * (d + 1) / g_nd;
                HIP_CHECK(hipEventRecord(e2, v->s));
                if (g1 > g0)
                    k_crt_batch<<<(unsigned)(g1 - g0), CRT_THREADS, 0, v->s>>>((const uint64_t *)D[0].da.p, (const uint64_t *)D[1].da.p,
                        (const uint64_t *)D[2].da.p, (const uint64_t *)D[3].da.p, g_desc[d], g0, S, L3, G, g_spill, bi_decimal);
                HIP_CHECK(hipEventRecord(e3, v->s));
                HIP_CHECK(hipStreamSynchronize(v->s));
                HIP_CHECK(hipEventElapsedTime(&ms3, e2, e3));
#pragma omp barrier                                    /* before anyone overwrites a plane */
            } else {
                if (act) k_store<<<228 * 8, 256, 0, v->s>>>(v->hstage, da, total);
                HIP_CHECK(hipStreamSynchronize(v->s));
            }
            HIP_CHECK(hipEventElapsedTime(&ms1, e0, e1)); HIP_CHECK(hipEventElapsedTime(&ms2, e1, e2));
#pragma omp critical
            { if (ms1 * 1e-3 > tsc) tsc = ms1 * 1e-3; if (ms2 * 1e-3 > tnt) tnt = ms2 * 1e-3; if (ms3 * 1e-3 > tcr) tcr = ms3 * 1e-3; }
            HIP_CHECK(hipEventDestroy(e0)); HIP_CHECK(hipEventDestroy(e1)); HIP_CHECK(hipEventDestroy(e2)); HIP_CHECK(hipEventDestroy(e3));
        }
        rns_st.tb_scatter += tsc; rns_st.tb_ntt += tnt; rns_st.tb_crt += tcr;
        double tm0 = mem_now();
        if (gpucrt && db_pool_vmm_on()) {                 /* Phase 14 R1 (E8): the outputs may be a VMM range -- the merge on device 0 (g_desc[0] holds the M descriptors) */
            int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(0));
            k_spill_merge<<<(unsigned)((M + 255) / 256), 256, 0, D[0].s>>>(g_desc[0], M, g_spill, S, bi_decimal); HIP_CHECK(hipStreamSynchronize(D[0].s));
            HIP_CHECK(hipSetDevice(cur));
        } else if (gpucrt) {
            /* merge the stripe spills on the CPU: stripe s of product i spills at coefficient nc (s+1) / S */
#pragma omp parallel for schedule(dynamic, 16)
            for (size_t i = 0; i < M; i++) {
                size_t nc = Q[first + i].na + Q[first + i].nb, top = nc + (Q[first + i].x && Q[first + i].nx ? 1 : 0); uint64_t *out = Q[first + i].c;
                for (int s = 0; s + 1 < S; s++) {
                    size_t k = nc * (s + 1) / S; const uint64_t *sp = g_spill + (i * S + s) * 4; uint64_t cy = 0;
                    if (bi_decimal) {
                        for (int q = 0; q < 4 && k < nc; q++, k++) { uint64_t sm = out[k] + sp[q] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; }
                        while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; k++; }
                    } else {
                    for (int q = 0; q < 4 && k < nc; q++, k++) { uint64_t sm = out[k] + sp[q], c1 = sm < out[k]; sm += cy; c1 += sm < cy; out[k] = sm; cy = c1; }
                    while (cy && k < top) { uint64_t sm = out[k] + cy; cy = sm < cy; out[k] = sm; k++; }
                    }
                }
            }
        }
        rns_st.tb_merge += mem_now() - tm0;
        if (!gpucrt) {
            uint64_t *planes[EC_NP];
#pragma omp parallel for schedule(dynamic, 1)
            for (size_t i = 0; i < M; i++) {
                uint64_t *pl[EC_NP]; size_t nc = Q[first + i].na + Q[first + i].nb;
                uint64_t tmp[4];
                for (int c = 0; c < g_nd; c++) pl[c] = D[c].hstage + (i << logL);
                /* out needs nc + 4 limbs of room: use a small tail buffer */
                uint64_t *o = (uint64_t *)malloc((nc + 4) * 8);
                crt_carry_par4(pl, nc, o, 1);
                if (Q[first + i].x && Q[first + i].nx) { o[nc] = limb_add(o, o, nc, Q[first + i].x, Q[first + i].nx); memcpy(Q[first + i].c, o, (nc + 1) * 8); }
                else memcpy(Q[first + i].c, o, nc * 8);
                free(o); (void)tmp;
            }
            (void)planes;
        }
    }
    if (staged) {
#pragma omp parallel for schedule(dynamic, 64)
        for (size_t i = 0; i < N; i++) memcpy(P[i].c, stagedP[i].c, (P[i].na + P[i].nb) * 8);
        free(stagedP);
    }
    if (g_snap_on && grpB && g_snap_n) rns_snap_check("after the level");   /* Phase 12 R witness: each device's read of B against B as it is now */
    free(hd);
    rns_st.t_total += mem_now() - t0;
    rns_st.n_batch += N;
    rns_st.tb_total += mem_now() - t0;
    if (pair) rns_st.n_batch_pair += N;
    if (getenv("RNS_VERBOSE")) printf("batch N=%zu L=%s2^%d tile %zu%s%s%s: %.3f s (scatter %.3f ntt %.3f crt %.3f merge %.3f)\n", N, r3 ? "3*" : "", logk, Mmax, staged ? " staged" : "", grpB ? " grpB" : "", pair ? " pair" : "", mem_now() - t0, rns_st.tb_scatter, rns_st.tb_ntt, rns_st.tb_crt, rns_st.tb_merge);
}

static void mdev_gpu_crt(int d, size_t nc, int logn, uint64_t *dst)
{
    static struct gconst G; static int ginit = 0;
    static struct bdesc *hdesc; static int hinit = 0;
    struct dev *v = &D[d];
    if (d == 0) {
        if (!ginit || G.np != ec_np) { G = rns_gconst(); ginit = 1; }   /* (P3: refreshed when the prime count changes) */
        if (!hinit) { hdesc = (struct bdesc *)malloc(sizeof *hdesc); hinit = 1; }
        hdesc->a = hdesc->b = hdesc->x = 0; hdesc->c = dst; hdesc->na = (uint32_t)nc; hdesc->nb = 0; hdesc->nx = 0;
        int S = rns_gpucrt_blocks; if ((size_t)S > (nc >> 10)) S = (int)(nc >> 10) > 0 ? (int)(nc >> 10) : 1;
        if (g_spill_cap < (size_t)S) { if (g_spill) mem_hreg_free(g_spill); g_spill_cap = S + 64; g_spill = (uint64_t *)mem_hreg_alloc(g_spill_cap * 4 * 8); }
        g_mdev_S = S;
    }
#pragma omp barrier                                    /* planes complete on every device, descriptor ready */
    int S = g_mdev_S;
    if (g_desc_cap[d] < 1) { if (g_desc[d]) HIP_CHECK(hipFree(g_desc[d])); HIP_CHECK(hipMalloc(&g_desc[d], sizeof(struct bdesc))); g_desc_cap[d] = 1; }
    HIP_CHECK(hipMemcpyAsync(g_desc[d], hdesc, sizeof(struct bdesc), hipMemcpyHostToDevice, v->s));
    size_t g0 = (size_t)S * d / g_nd, g1 = (size_t)S * (d + 1) / g_nd;
    if (g1 > g0)
        k_crt_batch<<<(unsigned)(g1 - g0), CRT_THREADS, 0, v->s>>>((const uint64_t *)D[0].da.p, (const uint64_t *)D[1].da.p,
            (const uint64_t *)D[2].da.p, (const uint64_t *)D[3].da.p, g_desc[d], g0, S, (size_t)1 << logn, G, g_spill, bi_decimal);
    HIP_CHECK(hipStreamSynchronize(v->s));
#pragma omp barrier
    if (d == 0) {
        uint64_t *out = dst;
        for (int s = 0; s + 1 < S; s++) {
            size_t k = nc * (s + 1) / S; const uint64_t *sp = g_spill + s * 4; uint64_t cy = 0;
            if (bi_decimal) {
                for (int q = 0; q < 4 && k < nc; q++, k++) { uint64_t sm = out[k] + sp[q] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; }
                while (cy && k < nc) { uint64_t sm = out[k] + cy; cy = sm >= EC_1E18; out[k] = cy ? sm - EC_1E18 : sm; k++; }
            } else {
            for (int q = 0; q < 4 && k < nc; q++, k++) { uint64_t sm = out[k] + sp[q], c1 = sm < out[k]; sm += cy; c1 += sm < cy; out[k] = sm; cy = c1; }
            while (cy && k < nc) { uint64_t sm = out[k] + cy; cy = sm < cy; out[k] = sm; k++; }
            }
        }
    }
}

/* C = (a b) mod 2^(64 w): limbs of a, b at or above w cannot matter; products
 * that fit the pool are formed in full and truncated; larger ones split at
 * h = ceil(max(na, nb) / 2): low(a0 b0) + (low(a0 b1 + a1 b0, w - h) << 64 h)
 * + (low(a1 b1, w - 2h) << 128 h), each term truncated to its window. */
void rns_mul_low(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb, size_t w)
{
    na = limb_norm(a, na < w ? na : w); nb = limb_norm(b, nb < w ? nb : w);
    if (!na || !nb || !w) { C->n = 0; return; }
    size_t pool = mdev_pts();                              /* 2^pool_log (P3: less with three primes) */
    if (na + nb <= pool || na + nb <= w) {
        mul_rec(C, a, na, b, nb, 0);
        if (C->n > w) { C->n = w; bi_norm(C); }
        return;
    }
    size_t m = na > nb ? na : nb, h = (m + 1) / 2;
    if (h >= na || h >= nb) {                          /* one operand is one piece: split the other only */
        if (na < nb) { const uint64_t *t = a; a = b; b = t; size_t tn = na; na = nb; nb = tn; }
        bigint z; bi_init(&z);
        rns_mul_low(C, a, h, b, nb, w);                /* a0 b */
        if (w > h) {
            rns_mul_low(&z, a + h, na - h, b, nb, w - h);   /* a1 b, low w - h limbs */
            bi_reserve(C, w + 1); for (size_t i = C->n; i < w + 1; i++) C->l[i] = 0; C->n = w + 1;
            bi_add_shifted(C, &z, h);
        }
        bi_free(&z);
    } else {
        bigint z; bi_init(&z);
        rns_mul_low(C, a, h, b, h, w);                 /* a0 b0 */
        bi_reserve(C, w + 1); for (size_t i = C->n; i < w + 1; i++) C->l[i] = 0; C->n = w + 1;
        if (w > h) {
            rns_mul_low(&z, a, h, b + h, nb - h, w - h); bi_add_shifted(C, &z, h);
            rns_mul_low(&z, a + h, na - h, b, h, w - h); bi_add_shifted(C, &z, h);
        }
        if (w > 2 * h) { rns_mul_low(&z, a + h, na - h, b + h, nb - h, w - 2 * h); bi_add_shifted(C, &z, 2 * h); }
        bi_free(&z);
    }
    if (C->n > w) C->n = w;
    bi_norm(C);
}

/* ======================================================================== */
/* engine 2: two 62-bit primes, 45-bit points, Montgomery kernels (ntt2),    */
/* CPU CRT with the 45-bit carry.  Device d: prime d & 1, product slot d >> 1 */
/* ======================================================================== */
static void rns2_core(int np, bigint *C[2], const uint64_t *a[2], size_t na[2], const uint64_t *b, size_t nb)
{
    size_t nc[2], pts[2]; int logn[2];
    for (int j = 0; j < np; j++) { nc[j] = na[j] + nb; pts[j] = e2_points(nc[j]); logn[j] = ceil_log2(pts[j]); if (logn[j] < NTT_LOGN_MIN) logn[j] = NTT_LOGN_MIN; }
    double t0 = mem_now();
#pragma omp parallel num_threads(g_nd)
    {
        int d = omp_get_thread_num(), j = d >> 1;
        struct dev *v = &D[d];
        HIP_CHECK(hipSetDevice(d));
        if (j < np) {
            size_t n = (size_t)1 << logn[j];
            node_copy(v->hstage, a[j], na[j], mem_numa_node_of_device(d), v->ncpu);
            node_copy(v->hstage + na[j], b, nb, mem_numa_node_of_device(d), v->ncpu);
            uint64_t *da = (uint64_t *)v->da.p, *db = (uint64_t *)v->db.p;
            ntt2_load(v->ctx2, da, v->hstage, na[j], n, v->s);
            ntt2_load(v->ctx2, db, v->hstage + na[j], nb, n, v->s);
            ntt2_fwd(v->ctx2, da, logn[j], 1, v->s);
            ntt2_fwd(v->ctx2, db, logn[j], 1, v->s);
            ntt2_inv_pw(v->ctx2, da, db, logn[j], 1, v->s);
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
#pragma omp barrier                                    /* both planes of a product loaded before the store overwrites staging */
        if (j < np) {
            uint64_t *da = (uint64_t *)v->da.p;
            k_store<<<228 * 8, 256, 0, v->s>>>(v->hstage, da, pts[j]);
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
    }
    double t1 = mem_now();
    int T = rns_crt_threads ? rns_crt_threads : omp_get_max_threads();
    for (int j = 0; j < np; j++) {
        uint64_t *planes[2] = { D[2 * j].hstage, D[2 * j + 1].hstage };
        bi_reserve(C[j], nc[j] + 2);
        crt2_carry(planes, pts[j], C[j]->l, nc[j] + 1, T);
        C[j]->n = nc[j] + 1; bi_norm(C[j]);
    }
    rns_st.t_crt += mem_now() - t1; rns_st.t_total += mem_now() - t0; rns_st.n_mdev += np; rns_st.points_mdev += ((size_t)1 << logn[0]) * np;
    if (getenv("RNS_VERBOSE")) printf("mdev2%s 2^%d (%zu limbs = %zu points): gpu %.3f crt %.3f total %.3f s\n", np > 1 ? "_pair" : "", logn[0], nc[0], pts[0], t1 - t0, mem_now() - t1, mem_now() - t0);
}
void rns2_mul_mdev(bigint *C, const uint64_t *a, size_t na, const uint64_t *b, size_t nb)
{
    bigint *Cs[2] = { C, 0 }; const uint64_t *as[2] = { a, 0 }; size_t nas[2] = { na, 0 };
    rns2_core(1, Cs, as, nas, b, nb);
}

/* batch: scatter with the 45-bit repack; device d handles prime d & 1 for products half d >> 1 */
__global__ void k_scatter2(uint64_t *da, uint64_t *db, const struct bdesc *P, size_t M, int logL, e2_mod m)
{
    size_t total = M << logL, L = (size_t)1 << logL;
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < total; i += stride) {
        size_t pi = i >> logL, k = i & (L - 1);
        const struct bdesc d = P[pi];
        size_t bit = k * E2_BITS, l = bit >> 6; unsigned o = (unsigned)(bit & 63);
        uint64_t va = 0, vb = 0;
        if (l < d.na) { va = d.a[l] >> o; if (o > 64 - E2_BITS && l + 1 < d.na) va |= d.a[l + 1] << (64 - o); va &= ((uint64_t)1 << E2_BITS) - 1; }
        da[i] = va ? e2_mm(va, m.r2, m) : 0;
        if (db) { if (l < d.nb) { vb = d.b[l] >> o; if (o > 64 - E2_BITS && l + 1 < d.nb) vb |= d.b[l + 1] << (64 - o); vb &= ((uint64_t)1 << E2_BITS) - 1; }
                  db[i] = vb ? e2_mm(vb, m.r2, m) : 0; }
    }
}
static void rns2_mul_batch(rns_prod *P, size_t N)
{
    size_t maxpts = 0;
    int staged = 0;
    for (size_t i = 0; i < N; i++) {
        size_t nc = P[i].na + P[i].nb, pts = e2_points(nc);
        if (pts > maxpts) maxpts = pts;
        if (!mem_is_registered(P[i].a, P[i].na * 8) || !mem_is_registered(P[i].b, P[i].nb * 8)) staged = 1;
    }
    int logL = ceil_log2(maxpts); if (logL < NTT_LOGN_MIN) logL = NTT_LOGN_MIN;
    if (logL > g_pool_log - 1) {                       /* too long for a batched plane: one mdev per product */
        bigint c; bi_init(&c);
        for (size_t i = 0; i < N; i++) {
            rns2_mul_mdev(&c, P[i].a, P[i].na, P[i].b, P[i].nb);
            size_t nc = P[i].na + P[i].nb;
            memcpy(P[i].c, c.l, c.n * 8); if (c.n < nc) memset(P[i].c + c.n, 0, (nc - c.n) * 8);
        }
        bi_free(&c);
        return;
    }
    size_t L = (size_t)1 << logL;
    size_t Mmax = ((size_t)1 << (g_pool_log - 1)) / L;          /* plane store must fit half the staging (the other half stages operands) */
    if (Mmax > rns_batch_tile_bytes / (3 * L * 8)) Mmax = rns_batch_tile_bytes / (3 * L * 8);
    if (Mmax < 1) Mmax = 1;
    int grpB = N > 1;
    for (size_t i = 1; i < N && grpB; i++) if (P[i].b != P[0].b || P[i].nb != P[0].nb) grpB = 0;
    double t0 = mem_now();
    rns_prod *Q = P, *stagedP = 0;
    if (staged) {                                     /* operands into the top half of every device's staging (same offsets) */
        size_t cap = (size_t)1 << (g_pool_log - 1), in = 0;
        for (size_t i = 0; i < N; i++) in += P[i].na + (grpB && i ? 0 : P[i].nb);
        if (in > cap) { size_t half = N / 2; rns2_mul_batch(P, half); rns2_mul_batch(P + half, N - half); return; }
        stagedP = (rns_prod *)malloc(N * sizeof *stagedP);
        size_t off = cap;
        for (size_t i = 0; i < N; i++) {
            stagedP[i] = P[i];
            stagedP[i].a = D[0].hstage + off; off += P[i].na;
            if (!grpB || i == 0) { stagedP[i].b = D[0].hstage + off; off += P[i].nb; } else stagedP[i].b = stagedP[0].b;
        }
        for (int d = 0; d < g_nd; d++) {
#pragma omp parallel for schedule(dynamic, 64)
            for (size_t i = 0; i < N; i++) {
                memcpy(D[d].hstage + (stagedP[i].a - D[0].hstage), P[i].a, P[i].na * 8);
                if (!grpB || i == 0) memcpy(D[d].hstage + (stagedP[i].b - D[0].hstage), P[i].b, P[i].nb * 8);
            }
        }
        Q = stagedP;
    }
    struct bdesc *hd = (struct bdesc *)malloc(N * sizeof *hd);
    for (size_t i = 0; i < N; i++) { hd[i].a = Q[i].a; hd[i].b = Q[i].b; hd[i].c = Q[i].c; hd[i].x = Q[i].x; hd[i].na = (uint32_t)Q[i].na; hd[i].nb = (uint32_t)Q[i].nb; hd[i].nx = (uint32_t)(Q[i].x ? Q[i].nx : 0); }
    /* product halves per device pair; within a pair both devices see the same products */
    size_t h0[2] = { 0, N / 2 }, h1[2] = { N / 2, N };
    if (grpB) {
#pragma omp parallel num_threads(g_nd)
        {
            int d = omp_get_thread_num(); struct dev *v = &D[d];
            HIP_CHECK(hipSetDevice(d));
            const uint64_t *bsrc = staged ? D[d].hstage + (Q[0].b - D[0].hstage) : Q[0].b;
            ntt2_load(v->ctx2, (uint64_t *)v->db.p, bsrc, Q[0].nb, L, v->s);
            ntt2_fwd(v->ctx2, (uint64_t *)v->db.p, logL, 1, v->s);
            HIP_CHECK(hipStreamSynchronize(v->s));
        }
    }
    size_t hmax = h1[0] - h0[0] > h1[1] - h0[1] ? h1[0] - h0[0] : h1[1] - h0[1];
    size_t tiles = (hmax + Mmax - 1) / Mmax;
    for (size_t ti = 0; ti < tiles; ti++) {
#pragma omp parallel num_threads(g_nd)
        {
            int d = omp_get_thread_num(), half = d >> 1; struct dev *v = &D[d];
            HIP_CHECK(hipSetDevice(d));
            size_t first = h0[half] + ti * Mmax, M = first < h1[half] ? (h1[half] - first < Mmax ? h1[half] - first : Mmax) : 0;
            if (M) {
                if (g_desc_cap[d] < M) { if (g_desc[d]) HIP_CHECK(hipFree(g_desc[d])); HIP_CHECK(hipMalloc(&g_desc[d], M * sizeof(struct bdesc))); g_desc_cap[d] = M; }
                struct bdesc *hdd = hd + first;
                if (staged) {                                          /* this device's staging copy of the operands */
                    struct bdesc *tmp = (struct bdesc *)malloc(M * sizeof *tmp);
                    for (size_t i = 0; i < M; i++) { tmp[i] = hdd[i]; tmp[i].a = D[d].hstage + (hdd[i].a - D[0].hstage); tmp[i].b = D[d].hstage + (hdd[i].b - D[0].hstage); }
                    HIP_CHECK(hipMemcpy(g_desc[d], tmp, M * sizeof(struct bdesc), hipMemcpyHostToDevice)); free(tmp);
                } else HIP_CHECK(hipMemcpy(g_desc[d], hdd, M * sizeof(struct bdesc), hipMemcpyHostToDevice));
                uint64_t *da = (uint64_t *)v->da.p, *db = (uint64_t *)v->db.p;
                size_t total = M << logL, blocks = (total + 255) / 256; if (blocks > 228 * 16) blocks = 228 * 16;
                k_scatter2<<<(unsigned)blocks, 256, 0, v->s>>>(da, grpB ? 0 : db, g_desc[d], M, logL, e2_mod_get(d & 1));
                ntt2_fwd(v->ctx2, da, logL, M, v->s);
                if (grpB) ntt2_inv_pw_bcast(v->ctx2, da, db, logL, M, v->s);
                else { ntt2_fwd(v->ctx2, db, logL, M, v->s); ntt2_inv_pw(v->ctx2, da, db, logL, M, v->s); }
                k_store<<<228 * 8, 256, 0, v->s>>>(v->hstage, da, total);   /* planes into the low half of staging */
                HIP_CHECK(hipStreamSynchronize(v->s));
            }
        }
        /* CPU CRT per product: planes of product i (local index) at hstage[2 half + {0,1}] + (i << logL) */
        for (int half = 0; half < 2; half++) {
            size_t first = h0[half] + ti * Mmax, M = first < h1[half] ? (h1[half] - first < Mmax ? h1[half] - first : Mmax) : 0;
            int T = M >= 64 ? 1 : (int)(omp_get_max_threads() / (M ? M : 1));
#pragma omp parallel for schedule(dynamic, 1) if (M >= 64)
            for (size_t i = 0; i < M; i++) {
                uint64_t *pl[2] = { D[2 * half].hstage + (i << logL), D[2 * half + 1].hstage + (i << logL) };
                size_t nc = Q[first + i].na + Q[first + i].nb;
                uint64_t *o = (uint64_t *)malloc((nc + 2) * 8);
                crt2_carry(pl, e2_points(nc), o, nc + 1, T);
                memcpy(Q[first + i].c, o, nc * 8);
                free(o);
            }
        }
    }
    free(hd); free(stagedP);
    rns_st.tb_total += mem_now() - t0; rns_st.n_batch += N;
    if (getenv("RNS_VERBOSE")) printf("batch2 N=%zu L=2^%d tile %zu%s%s: %.3f s\n", N, logL, Mmax, staged ? " staged" : "", grpB ? " grpB" : "", mem_now() - t0);
}
