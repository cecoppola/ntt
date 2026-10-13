/* dbig.c - see dbig.h */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <pthread.h>
#include <unistd.h>
#include <hip/hip_runtime.h>
#include "dbig.h"
#include "mem.h"
#include <time.h>
#include <stdarg.h>
#include "p24.h"                                       /* a P24 product's spill decode (sparse_get) */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define CH 4096                                        /* limbs per carry chunk (256 threads x 16) */
static const uint64_t B10 = 1000000000000000000ULL;
struct db_stats db_st;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int g_inited = 0;                               /*  */
static void db_acct(int ndev, size_t b[][MEM_DEV_NCAT]);
static void par_init(void) { if (!g_inited) { g_inited = 1; mem_acct_register(db_acct); } }

struct dv { const uint64_t *q[DB_NQ]; size_t qc, off, shift; };     /* shift: the operand as a << shift limbs; qc: limbs per quarter (any multiple of DB_ALIGN) */
/* the quarter of global limb g: the number of quarter boundaries at or below g (no division: qc is any size) */
__device__ static inline size_t dq(size_t g, size_t qc) { return (g >= qc) + (g >= 2 * qc) + (g >= 3 * qc); }
/* S41: the quarter's base pointer by a select over constant indices (a runtime-indexed pointer array put the by-value struct in per-lane
 * scratch, 112 B/lane in k_gather); dget_s for the gather_shift / addsub3 / maxidx kernels, dget (indexed) for k_complement */
__device__ static inline const uint64_t *qsel_p(const uint64_t *const *q, size_t d) { const uint64_t *p = q[0]; if (d == 1) p = q[1]; if (d == 2) p = q[2]; if (d == 3) p = q[3]; return p; }
__device__ static inline uint64_t dget_s(const struct dv &v, size_t i) { if (i < v.shift) return 0; size_t g = v.off + i - v.shift, d = dq(g, v.qc); return qsel_p(v.q, d)[g - d * v.qc]; }
__device__ static inline uint64_t dget(const struct dv v, size_t i) { if (i < v.shift) return 0; size_t g = v.off + i - v.shift, d = dq(g, v.qc); return v.q[d][g - d * v.qc]; }
static struct dv view_of(const dbig *a) { struct dv v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.off = a->off; v.shift = 0; return v; }
static inline size_t hq(const dbig *a, size_t g) { return (g >= a->qc) + (g >= 2 * a->qc) + (g >= 3 * a->qc); }   /* host: quarter of global limb g */
#define DB_ALIGN 4096                                   /* limbs: quarters are multiples of the carry chunk */
static void need_owner(const dbig *r, const char *what) { if (r->off || (!r->cap && r->n)) { ec_fatal(EC_RC_FATAL, "dbig: %s into a view\n", what); } }

/* a host -> device copy by a kernel on the device (pinned host memory read over the fabric) on a non-blocking
 * stream per device, and the wait: hipMemcpyAsync into a VMM range ran at 6 GB/s and blocked the seed thread for the copy's duration */
__global__ void k_copy_h2d(uint64_t *dst, const uint64_t *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}
void db_copy_kernel(unsigned blocks, hipStream_t s, uint64_t *dst, const uint64_t *src, size_t n) { k_copy_h2d<<<blocks, 256, 0, s>>>(dst, src, n); }   /* the copy kernel for rns_mul.c (one definition) */
static hipStream_t g_h2d_s[DB_NQ]; static int g_h2d_init[DB_NQ];
void db_copy_h2d_async(int dev, void *dst, const void *src, size_t bytes)
{
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    if (!g_h2d_init[dev]) { HIP_CHECK(hipStreamCreateWithFlags(&g_h2d_s[dev], hipStreamNonBlocking)); g_h2d_init[dev] = 1; }
    void *dsrc = 0; if (hipHostGetDevicePointer(&dsrc, (void *)src, 0) != hipSuccess || !dsrc) dsrc = (void *)src;
    size_t n = bytes / 8; unsigned blocks = (unsigned)((n + 255) / 256); if (blocks > 228 * 16) blocks = 228 * 16;
    k_copy_h2d<<<blocks, 256, 0, g_h2d_s[dev]>>>((uint64_t *)dst, (const uint64_t *)dsrc, n);
    if (bytes & 7) HIP_CHECK(hipMemcpyAsync((char *)dst + n * 8, (const char *)src + n * 8, bytes & 7, hipMemcpyDefault, g_h2d_s[dev]));
    HIP_CHECK(hipSetDevice(cur));
}
void db_copy_h2d_wait(int dev) { if (g_h2d_init[dev]) HIP_CHECK(hipStreamSynchronize(g_h2d_s[dev])); }
/* K8b (results/K8b.md): BS_SEED_PULL=1 -- the seeds' host -> device copies without a HIP runtime call inside init's mapping window.
 * K8 measured the seed thread blocked 3.1 s at 6.441e10 inside db_copy_h2d_async (its launch and hipSetDevice queue behind the
 * background mappers' hipMemCreate / hipMemMap / hipMemSetAccess, 0.24 s with no mapper).  Here one persistent kernel per APU
 * (k_pull, 456 blocks, launched by db_pull_start_all from binsplit_seeds_begin once the region pools exist and before the
 * background mapping is released) polls a control block in coherent pinned host memory; a copy is a descriptor plus a release
 * store of `seq` (db_pull_copy_async), its completion a host spin on the kernel's `done` counter (db_pull_wait), and the kernels
 * leave on `quit` (db_pull_stop_all, at the end of seeds_stream, before the buffers are unregistered).  The source's device
 * pointer is resolved once per buffer (db_pull_resolve, from the seed thread right after the buffers are registered).  Same bytes
 * to the same addresses as db_copy_h2d_async: digits identical.  Default 1 (results/CAND.md); on a CPX
 * XCD the 456 blocks need not all be resident and the first copy never completes (CPX runs set BS_SEED_PULL=0. */
struct db_pullctl { unsigned long long seq, done, exited, quit; unsigned long long dst, src, n; };
#define DB_PULL_BLOCKS 456
__global__ void k_pull(struct db_pullctl *c)
{
    unsigned long long my = 0;
    for (;;) {
        unsigned long long s;
        while ((s = __hip_atomic_load(&c->seq, __ATOMIC_ACQUIRE, __HIP_MEMORY_SCOPE_SYSTEM)) == my) {
            if (__hip_atomic_load(&c->quit, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM)) {
                __syncthreads();
                if (threadIdx.x == 0) __hip_atomic_fetch_add(&c->exited, 1ull, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
                return;
            }
            __builtin_amdgcn_s_sleep(127);
        }
        my = s;
        uint64_t *dst = (uint64_t *)__hip_atomic_load(&c->dst, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        const uint64_t *src = (const uint64_t *)__hip_atomic_load(&c->src, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        size_t n = __hip_atomic_load(&c->n, __ATOMIC_RELAXED, __HIP_MEMORY_SCOPE_SYSTEM);
        size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
        for (; i < n; i += stride) dst[i] = src[i];
        __threadfence_system();
        __syncthreads();
        if (threadIdx.x == 0) __hip_atomic_fetch_add(&c->done, 1ull, __ATOMIC_RELEASE, __HIP_MEMORY_SCOPE_SYSTEM);
    }
}
int db_seed_pull_on(void) { static int on = -1; if (on < 0) on = getenv("BS_SEED_PULL") ? atoi(getenv("BS_SEED_PULL")) : 1; return on; }   /* default 1; 0 = hipMemcpy */
static hipStream_t g_pull_s[DB_NQ]; static struct db_pullctl *g_pull[DB_NQ]; static int g_pull_on[DB_NQ]; static unsigned long long g_pull_seq[DB_NQ];
static pthread_mutex_t g_pull_mx = PTHREAD_MUTEX_INITIALIZER;
static struct { const void *h, *d; } g_pull_src[4]; static int g_pull_nsrc;   /* db_pull_resolve: the seed buffers' device pointers */
void db_pull_resolve(const void *src)
{
    pthread_mutex_lock(&g_pull_mx);
    for (int i = 0; i < g_pull_nsrc; i++) if (g_pull_src[i].h == src) { pthread_mutex_unlock(&g_pull_mx); return; }
    void *d = 0; if (hipHostGetDevicePointer(&d, (void *)src, 0) != hipSuccess || !d) d = (void *)src;
    if (g_pull_nsrc < 4) { g_pull_src[g_pull_nsrc].h = src; g_pull_src[g_pull_nsrc].d = d; g_pull_nsrc++; }
    pthread_mutex_unlock(&g_pull_mx);
}
void db_pull_start_all(void)                             /* idempotent; the launches cost nothing when no mapping is in flight */
{
    pthread_mutex_lock(&g_pull_mx);
    int nd = mem_device_count(), cur; HIP_CHECK(hipGetDevice(&cur)); double t0 = mem_now(); int n = 0;
    for (int d = 0; d < nd && d < DB_NQ; d++) if (!g_pull_on[d]) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipStreamCreateWithFlags(&g_pull_s[d], hipStreamNonBlocking));
        HIP_CHECK(hipHostMalloc((void **)&g_pull[d], sizeof *g_pull[d], hipHostMallocCoherent)); memset(g_pull[d], 0, sizeof *g_pull[d]);
        k_pull<<<DB_PULL_BLOCKS, 256, 0, g_pull_s[d]>>>(g_pull[d]); HIP_CHECK(hipGetLastError());
        g_pull_on[d] = 1; g_pull_seq[d] = 0; n++;
    }
    HIP_CHECK(hipSetDevice(cur));
    if (n) db_tl("seed pull kernels launched on %d APUs (%.3f s; BS_SEED_PULL)", n, mem_now() - t0);
    pthread_mutex_unlock(&g_pull_mx);
}
void db_pull_copy_async(int dev, void *dst, const void *src, size_t bytes)
{
    if (!g_pull_on[dev]) db_pull_start_all();
    db_pull_wait(dev);   /* K8c: one descriptor per APU -- the previous copy must be finished before it is overwritten (seeds_stream issues both
                          * buffered chunks of one region to the same APU back to back; without this the blocks saw seq jump 0 -> 2, skipped copy 1
                          * and `done` never reached 2 x DB_PULL_BLOCKS: the seed thread hung, results/K8b.md section 6) */
    const void *dsrc = 0; for (int i = 0; i < g_pull_nsrc; i++) if (g_pull_src[i].h == src) dsrc = g_pull_src[i].d;
    if (!dsrc) { db_pull_resolve(src); for (int i = 0; i < g_pull_nsrc; i++) if (g_pull_src[i].h == src) dsrc = g_pull_src[i].d; }
    if (!dsrc) dsrc = src;
    size_t n = bytes / 8; struct db_pullctl *c = g_pull[dev];
    __atomic_store_n(&c->dst, (unsigned long long)(uintptr_t)dst, __ATOMIC_RELAXED); __atomic_store_n(&c->src, (unsigned long long)(uintptr_t)dsrc, __ATOMIC_RELAXED); __atomic_store_n(&c->n, (unsigned long long)n, __ATOMIC_RELAXED);
    __atomic_store_n(&c->seq, ++g_pull_seq[dev], __ATOMIC_RELEASE);
    if (bytes & 7) { db_pull_wait(dev); db_copy_h2d_async(dev, (char *)dst + n * 8, (const char *)src + n * 8, bytes & 7); db_copy_h2d_wait(dev); }   /* never for the seeds (whole limbs) */
}
void db_pull_wait(int dev)
{
    if (!g_pull_on[dev]) return;
    unsigned long long want = g_pull_seq[dev] * DB_PULL_BLOCKS; double t0 = mem_now();
    while (__atomic_load_n(&g_pull[dev]->done, __ATOMIC_ACQUIRE) < want) { usleep(20); if (mem_now() - t0 > 600) { ec_fatal(EC_RC_FATAL, "db_pull_wait: APU%d's pull kernel did not finish copy %llu in 600 s\n", dev, g_pull_seq[dev]); } }
}
void db_pull_stop_all(void)                              /* the kernels leave; no runtime call (the streams stay) */
{
    pthread_mutex_lock(&g_pull_mx);
    int nd = mem_device_count(), left = 0; double t0 = mem_now();
    for (int d = 0; d < nd && d < DB_NQ; d++) if (g_pull_on[d]) { db_pull_wait(d); __atomic_store_n(&g_pull[d]->quit, 1ull, __ATOMIC_RELEASE); }
    for (int d = 0; d < nd && d < DB_NQ; d++) if (g_pull_on[d]) { while (__atomic_load_n(&g_pull[d]->exited, __ATOMIC_ACQUIRE) < DB_PULL_BLOCKS && mem_now() - t0 < 60) usleep(50); if (__atomic_load_n(&g_pull[d]->exited, __ATOMIC_ACQUIRE) < DB_PULL_BLOCKS) left++; g_pull_on[d] = 0; }
    if (left) { for (int d = 0; d < nd && d < DB_NQ; d++) HIP_CHECK(hipStreamSynchronize(g_pull_s[d])); }   /* not expected: the fallback is the runtime */
    db_tl("seed pull kernels stopped (%.3f s%s)", mem_now() - t0, left ? ", by stream sync" : "");
    pthread_mutex_unlock(&g_pull_mx);
}
/* quarter blocks come from per-device free lists by size class (hipMalloc costs ~0.06 s/GB and the Newton
 * loop allocates and frees temporaries every iteration); db_release_pools gives everything back */
/* Block pool per device: address-ordered free extents with coalescing (best fit, carve from the front,
 * the remainder stays an extent; neighbours merge on free).  Sizes are 2^l or 3 2^l limbs but nothing
 * depends on it, so the bs phase's many sizes and the dm phase's few large ones share the pool without
 * fragmentation (RESULTS.md 64).  Extents come from donated regions (db_donate) or from hipMalloc when
 * nothing fits (0.057 s/GB); regions are freed whole by db_release_pools. */
struct ext { char *p; size_t bytes; int reg; };                   /* reg: the whole region (g_donated) it lies in; extents never merge across regions, since a block must stay inside one hipMalloc allocation */
static struct { struct ext e[8192]; int n; } g_ext[DB_NQ];
static size_t g_pool_bytes;
static struct { uint64_t *p; int dev; size_t bytes; int reg; } g_live[8192]; static int g_nlive;
static pthread_mutex_t g_pool_mx = PTHREAD_MUTEX_INITIALIZER;   /* a background thread may free blocks */
static struct { void *p; int dev; size_t bytes; int own; int kind; } g_donated[256]; static int g_ndonated;   /* whole hipMalloc'd or donated regions (own: freed by db_release_pools; kind: M9 accounting -- 0 donated by a caller, 1 borrowed, 2 the pool's own hipMalloc) */
static size_t g_live_bytes[DB_NQ], g_peak_live[DB_NQ];   /* accounting: bytes handed out per device now, and the peak */
static size_t g_win_peak[DB_NQ];                           /* the peak since the last db_pool_window_peak(dev, 1) -- one level or one doubling */
/* ECALC_INIT_TL=1 prints init's timeline (`tl` lines on stdout: seconds since rns_init began; the arenas' mapping with
 * its HIP calls split, the seed thread, level 1's waits).  Print only; off by default.  =2 adds a line per background chunk */
static int g_tl = -1; static double g_tl_t0;
int db_tl_on(void) { if (g_tl < 0) g_tl = getenv("ECALC_INIT_TL") ? atoi(getenv("ECALC_INIT_TL")) : 0; return g_tl; }
void db_tl_start(void) { if (!g_tl_t0) g_tl_t0 = mem_now(); }
double db_tl_now(void) { return g_tl_t0 ? mem_now() - g_tl_t0 : 0.0; }
void db_tl(const char *fmt, ...)
{
    if (!db_tl_on()) return;
    char b[640]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("tl %7.2f  %s\n", db_tl_now(), b);
}
static void live_add(uint64_t *p, int d, size_t bytes, int reg) { if (g_nlive < 8192) { g_live[g_nlive].p = p; g_live[g_nlive].dev = d; g_live[g_nlive].bytes = bytes; g_live[g_nlive].reg = reg; g_nlive++; g_live_bytes[d] += bytes; if (g_live_bytes[d] > g_peak_live[d]) g_peak_live[d] = g_live_bytes[d]; if (g_live_bytes[d] > g_win_peak[d]) g_win_peak[d] = g_live_bytes[d]; } else { ec_fatal(EC_RC_FATAL, "dbig: live table full\n"); } }
static size_t live_take(uint64_t *p, int *reg) { for (int i = 0; i < g_nlive; i++) if (g_live[i].p == p) { size_t b = g_live[i].bytes; *reg = g_live[i].reg; g_live_bytes[g_live[i].dev] -= b; g_live[i] = g_live[--g_nlive]; return b; } return 0; }
static void ext_insert(int d, char *p, size_t bytes, int reg)
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n, i = 0;
    while (i < n && e[i].p < p) i++;
    int mprev = i > 0 && e[i - 1].reg == reg && e[i - 1].p + e[i - 1].bytes == p, mnext = i < n && e[i].reg == reg && p + bytes == e[i].p;
    if (mprev && mnext) { e[i - 1].bytes += bytes + e[i].bytes; memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; }
    else if (mprev) e[i - 1].bytes += bytes;
    else if (mnext) { e[i].p = p; e[i].bytes += bytes; }
    else { if (n >= 8192) { ec_fatal(EC_RC_FATAL, "dbig: extent table full\n"); } memmove(&e[i + 1], &e[i], (n - i) * sizeof *e); e[i].p = p; e[i].bytes = bytes; e[i].reg = reg; g_ext[d].n++; }
}
/* a reserved tail per device -- the last `bytes` of a region (the bs arena's end, laid out
 * by binsplit_pregrow to hold the largest block of the dm phase, t1's quarter).  Requests >= thresh are carved from the BACK of
 * the extent that ends at the tail's end (so the largest block lands there, contiguous); requests below it are best-fit over
 * the extents with the tail's bytes excluded, so the tail stays whole until the large request comes.  Both preferences are
 * soft: a large request the tail cannot hold takes the best fit anywhere, a small one that fits nowhere else takes the tail,
 * and only then does the pool hipMalloc -- never a failure that today's pool would not have had. */
static struct { char *p, *end; size_t bytes, thresh; size_t n_tail, n_spill; } g_tail[DB_NQ];
void db_pool_set_tail(int dev, void *p, size_t bytes, size_t thresh)
{
    if (dev < 0 || dev >= DB_NQ) return;
    pthread_mutex_lock(&g_pool_mx);
    g_tail[dev].p = (char *)p; g_tail[dev].bytes = bytes; g_tail[dev].end = (char *)p + bytes; g_tail[dev].thresh = thresh; g_tail[dev].n_tail = g_tail[dev].n_spill = 0;
    pthread_mutex_unlock(&g_pool_mx);
}
/* after the bs top level its dead inputs' blocks are free again -- the largest free extent
 * of the device is where the reciprocal's largest block (t1) should land: its last `bytes` become the tail (the whole extent when it is
 * smaller, and then t1 cannot land in it whole: said on the line).  The former tail (the arena's end) is forgotten. */
void db_pool_retarget_tail(int dev, size_t bytes, size_t thresh)
{
    if (dev < 0 || dev >= DB_NQ) return;
    static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0);
    pthread_mutex_lock(&g_pool_mx);
    int best = -1; size_t fr = 0; for (int i = 0; i < g_ext[dev].n; i++) { fr += g_ext[dev].e[i].bytes; if (best < 0 || g_ext[dev].e[i].bytes > g_ext[dev].e[best].bytes) best = i; }
    size_t lg = best >= 0 ? g_ext[dev].e[best].bytes : 0, tb = lg < bytes ? lg : bytes;
    if (best >= 0) { g_tail[dev].end = g_ext[dev].e[best].p + lg; g_tail[dev].p = g_tail[dev].end - tb; g_tail[dev].bytes = tb; g_tail[dev].thresh = thresh; g_tail[dev].n_tail = g_tail[dev].n_spill = 0; }
    pthread_mutex_unlock(&g_pool_mx);
    if (vb || lg < bytes) printf("dbig pool: APU%d tail moved to the top level's dead inputs: %.2f GB at the back of the largest free extent (%.2f GB; free %.2f GB in %d extents, live %.2f GB)%s\n",
                                 dev, tb / 1e9, lg / 1e9, fr / 1e9, g_ext[dev].n, g_live_bytes[dev] / 1e9, lg < bytes ? "  SHORT: t1's quarter will not fit it whole" : "");
}
static size_t ext_outside_tail(int d, const struct ext *e)   /* the extent's bytes not in the reserved tail (an extent never straddles the tail's end: it is a region's end) */
{
    if (!g_tail[d].bytes) return e->bytes;
    char *a = e->p, *b = e->p + e->bytes, *ta = g_tail[d].p, *tb = g_tail[d].end;
    if (b <= ta || a >= tb) return e->bytes;
    size_t ov = (size_t)((b < tb ? b : tb) - (a > ta ? a : ta));
    return e->bytes - ov;
}
/* large requests packed downward from the highest free end -- carved from the back of the highest-ending extent
 * that holds them -- so the big blocks of the top level and the reciprocal (P, Q, t1, r2) stack at the arena's top and the free space
 * below them stays one extent.  With the tail rule alone the top level's P takes the tail's back, nothing ends at the tail's end any
 * more, and the reciprocal's r2 (1.0 n_Q) was best-fit into the middle: at 4e10 it found 10.8 GB free in 3 extents, largest 4.39 (job
 * 21131).  Small requests keep the best fit from the front, outside the tail. */
static int g_pack_large;                              /* 1: bs (the top level's P, Q stay below the tail: t1 finds it whole); 2: the dm phase (the tail first, then the highest end) */
void db_pool_pack_large(int on) { g_pack_large = on; }
static int g_pin_tail;                                /* while set, every request is carved from the back of the tail's free part (r right below t1: the tail is then full, nothing small can spill into it, and it is whole again once both are freed) */
void db_pool_pin_tail(int on) { g_pin_tail = on; }
static char *carve_at(int d, int i, char *p, size_t need)   /* the block [p, p + need) out of extent i (front, back or middle: the remainder above becomes a new extent) */
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n; char *a = e[i].p, *b = e[i].p + e[i].bytes; int reg = e[i].reg;
    if (p + need == g_tail[d].end) g_tail[d].n_tail++;
    if (p == a) { e[i].p += need; e[i].bytes -= need; if (!e[i].bytes) { memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; } }
    else { e[i].bytes = (size_t)(p - a); if (p + need < b) ext_insert(d, p + need, (size_t)(b - (p + need)), reg); }
    return p;
}
static char *ext_take(int d, size_t need, int *reg)        /* best fit, carved from the front (the reserved tail as above) */
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n, best = -1;
    if (g_pin_tail && g_tail[d].bytes) {                  /* into the tail's free part, from its highest end (right below what is already there) */
        char *ta = g_tail[d].p, *tb = g_tail[d].end, *bp = 0;
        for (int i = 0; i < n; i++) { char *a = e[i].p, *b = e[i].p + e[i].bytes; if (b > tb) b = tb; if (a < ta) a = ta; if (b > a && (size_t)(b - a) >= need && (best < 0 || b > bp)) { best = i; bp = b; } }
        if (best >= 0) { *reg = e[best].reg; return carve_at(d, best, bp - need, need); }
        best = -1;
    }
    if (g_pack_large && g_tail[d].bytes && need >= g_tail[d].thresh) {
        char *ta = g_tail[d].p, *tb = g_tail[d].end;
        if (g_pack_large >= 2) for (int i = 0; i < n; i++) if (e[i].p + e[i].bytes == tb && e[i].bytes >= need) { *reg = e[i].reg; return carve_at(d, i, e[i].p + e[i].bytes - need, need); }   /* the dm phase: the tail's back first */
        char *bp = 0; for (int i = 0; i < n; i++) {          /* else the highest end outside the tail: the block ends at min(extent end, tail start) */
            char *a = e[i].p, *b = e[i].p + e[i].bytes; if (b > ta && a < tb) b = a < ta ? ta : a;
            if (b > a && (size_t)(b - a) >= need && (best < 0 || b > bp)) { best = i; bp = b; }
        }
        if (best >= 0) { *reg = e[best].reg; return carve_at(d, best, bp - need, need); }
        for (int i = 0; i < n; i++) if (e[i].bytes >= need && (best < 0 || e[i].p + e[i].bytes > e[best].p + e[best].bytes)) best = i;   /* nothing outside it: the highest end anywhere */
        if (best >= 0) { *reg = e[best].reg; return carve_at(d, best, e[best].p + e[best].bytes - need, need); }
    }
    if (g_tail[d].bytes && need >= g_tail[d].thresh) {       /* a large request: from the back of the extent ending at the tail's end */
        for (int i = 0; i < n; i++) if (e[i].p + e[i].bytes == g_tail[d].end && e[i].bytes >= need) {
            char *p = e[i].p + e[i].bytes - need; *reg = e[i].reg; e[i].bytes -= need; g_tail[d].n_tail++;
            if (!e[i].bytes) { memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; }
            return p;
        }
    }
    int excl = g_tail[d].bytes && need < g_tail[d].thresh, spilled = 0;   /* a small request: the tail's bytes do not count ... */
    for (int pass = 0; pass < 2 && best < 0; pass++, excl = 0) {   /* ... unless nothing else fits (pass 2) */
        for (int i = 0; i < n; i++) { size_t us = excl ? ext_outside_tail(d, &e[i]) : e[i].bytes; if (us >= need && (best < 0 || us < (excl ? ext_outside_tail(d, &e[best]) : e[best].bytes))) best = i; }
        if (best < 0 && excl) { g_tail[d].n_spill++; spilled = 1; }
    }
    if (best < 0) return 0;
    if (spilled && e[best].p + e[best].bytes == g_tail[d].end) {   /* a spill into the tail goes to its very end: what it leaves is one hole, contiguous with the free bytes before it (a block in the middle
                                                                    * split the 33 GB free of an APU into 17 + 16 at 1e11 and t1 fell back; at the end, 22 GB of it stay one extent) */
        char *p = e[best].p + e[best].bytes - need; *reg = e[best].reg; e[best].bytes -= need;
        if (!e[best].bytes) { memmove(&e[best], &e[best + 1], (n - best - 1) * sizeof *e); g_ext[d].n--; }
        return p;
    }
    char *p = e[best].p; *reg = e[best].reg; e[best].p += need; e[best].bytes -= need;
    if (!e[best].bytes) { memmove(&e[best], &e[best + 1], (n - best - 1) * sizeof *e); g_ext[d].n--; }
    return p;
}

/* ---- the VMM-backed arena, DB_POOL_VMM=1 --------------------------------------------------
 * One VA range per APU (hipMemAddressReserve, DB_POOL_VMM_RESERVE (3) x the arena), physical chunks of DB_POOL_VMM_CHUNK_GB (2) GiB
 * (hipMemCreate, pinned device memory) mapped from the range's start (hipMemMap) with read-write access for the four APUs
 * (hipMemSetAccess).  The pool's extents live over the range as over any region; a request that finds no contiguous extent is
 * satisfied by REMAPPING: the chunks that are wholly free are unmapped and mapped again, contiguously, into the lowest run of
 * empty slots (the same physical chunks: no create, no page work -- 0.002 s/GiB), and only when the free chunks do not cover the
 * request are new chunks created for the remainder (a growth by mapping, counted apart; never a hipMalloc).  The host cannot
 * write a VMM range (a CPU store segfaults): the seed thread's stores go through its buffers and DMA under this switch. */
static int g_vmm_on = -1;
int db_pool_vmm_on(void)
{
    if (g_vmm_on < 0) {
        const char *e = getenv("DB_POOL_VMM");
        g_vmm_on = e ? atoi(e) != 0 : 1;                                  /* default 1 */
    }
    return g_vmm_on;
}
static struct vmm { char *base; size_t reserved, chunk; int nslot, nd, reg; hipMemGenericAllocationHandle_t *h;   /* h[slot]: the chunk mapped there, 0 = empty */
                    size_t n_remap, remap_chunks, n_grow, grow_chunks; double t_remap;
                    int m0, mapped, bg_on; pthread_t bg; size_t bytes; double t_bg;
                    double t_cr, t_mp, t_ac, t_ms; int mark_m; } g_vmm[DB_NQ];   /* seconds in hipMemCreate / hipMemMap / hipMemSetAccess / the zeroing, summed (ECALC_INIT_TL); mark_m: the chunk count of the parity-1 half's end */   /* m0: the arena's chunks; mapped: how many of them are (the first `mapped` slots), the rest by the background thread */
static pthread_cond_t g_vmm_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_vmm_map_mx = PTHREAD_MUTEX_INITIALIZER;   /* the background mappers one at a time: four at once hold the runtime's lock while blocked on each other in the driver, and the seed thread's launches wait behind them */
static int g_vmm_go;                                   /* the background mapping starts when init's plane pools are allocated (db_vmm_bg_release from rns_init), so that the seeds get their half first and the pools their turn */
void db_vmm_bg_release(void) { db_tl("the background mapping is released"); pthread_mutex_lock(&g_pool_mx); g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); pthread_mutex_unlock(&g_pool_mx); }
static int vmm_vb(void) { static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0); return vb; }
static int vmm_map_run(int d, int slot0, int m, hipMemGenericAllocationHandle_t *hs)   /* hs[i] (0: create one) mapped at slot0 + i, access for every APU */
{
    struct vmm *v = &g_vmm[d]; hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
    prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
    for (int i = 0; i < m; i++) {
        double t0 = mem_now();
        if (!hs[i]) { if (hipMemCreate(&hs[i], v->chunk, &prop, 0) != hipSuccess) return 0; v->n_grow++; v->grow_chunks++; }
        double t1 = mem_now();
        if (hipMemMap(v->base + (size_t)(slot0 + i) * v->chunk, v->chunk, 0, hs[i], 0) != hipSuccess) return 0;
        v->t_cr += t1 - t0; v->t_mp += mem_now() - t1;
        v->h[slot0 + i] = hs[i];
    }
    hipMemAccessDesc ad[DB_NQ]; memset(ad, 0, sizeof ad);
    for (int c = 0; c < v->nd; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
    double ta = mem_now();
    if (hipMemSetAccess(v->base + (size_t)slot0 * v->chunk, (size_t)m * v->chunk, ad, v->nd) != hipSuccess) return 0;
    v->t_ac += mem_now() - ta;
    return 1;
}
static void *vmm_bg_map(void *arg)                       /* the arena's chunks after the first `first` bytes, one at a time, while init and the seeds go on (the driver serialises the page work anyway) */
{
    int dev = (int)(intptr_t)arg; struct vmm *v = &g_vmm[dev];
    pthread_mutex_lock(&g_pool_mx); while (!g_vmm_go) pthread_cond_wait(&g_vmm_cv, &g_pool_mx); pthread_mutex_unlock(&g_pool_mx);
    double t0 = mem_now();
    double c0 = v->t_cr, p0 = v->t_mp, a0 = v->t_ac, s0 = v->t_ms, tlk = 0; int k0 = v->mapped;
    db_tl("APU%d background mapping starts at chunk %d of %d (the parity-1 half ends at chunk %d)", dev, v->mapped, v->m0, v->mark_m - 1);
    hipStream_t st; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev)); HIP_CHECK(hipStreamCreateWithFlags(&st, hipStreamNonBlocking));
    for (int k = v->mapped; k < v->m0; k++) {
        hipMemGenericAllocationHandle_t h[1] = { 0 };
        double tq = mem_now();
        pthread_mutex_lock(&g_vmm_map_mx);
        double tg = mem_now(); tlk += tg - tq;
        if (!vmm_map_run(dev, k, 1, h)) mem_oom("db_vmm_arena_alloc (background chunk)", dev, v->chunk);
        double tz = mem_now();
        HIP_CHECK(hipMemsetAsync(v->base + (size_t)k * v->chunk, 0, v->chunk, st)); HIP_CHECK(hipStreamSynchronize(st));
        v->t_ms += mem_now() - tz;
        pthread_mutex_unlock(&g_vmm_map_mx);
        if (db_tl_on() >= 2) db_tl("APU%d chunk %d mapped (%.3f s held, %.3f s queued)", dev, k, mem_now() - tg, tg - tq);
        if (k + 1 == v->mark_m) db_tl("APU%d background: the parity-1 half is mapped (chunk %d)", dev, k);
        pthread_mutex_lock(&g_pool_mx); v->mapped = k + 1; pthread_cond_broadcast(&g_vmm_cv); pthread_mutex_unlock(&g_pool_mx);
    }
    db_tl("APU%d background mapping done: %d chunks in %.2f s (create %.2f, map %.2f, access %.2f, zero %.2f, queued for the mapper lock %.2f)", dev, v->m0 - k0, mem_now() - t0, v->t_cr - c0, v->t_mp - p0, v->t_ac - a0, v->t_ms - s0, tlk);
    HIP_CHECK(hipStreamDestroy(st)); HIP_CHECK(hipSetDevice(cur));
    pthread_mutex_lock(&g_pool_mx);
    if ((size_t)v->m0 * v->chunk > v->bytes) { ext_insert(dev, v->base + v->bytes, (size_t)v->m0 * v->chunk - v->bytes, v->reg); }   /* the last chunk's remainder: free at once */
    v->n_grow = 0; v->grow_chunks = 0; v->t_bg = mem_now() - t0;
    pthread_mutex_unlock(&g_pool_mx);
    return 0;
}
void db_vmm_arena_wait(int dev, size_t bytes)       /* the arena's first `bytes` are mapped (the parity-1 half before level 1, everything before the halves are donated / released) */
{
    struct vmm *v = &g_vmm[dev]; if (!v->base) return;
    size_t need = bytes < (size_t)v->m0 * v->chunk ? bytes : (size_t)v->m0 * v->chunk; int m = (int)((need + v->chunk - 1) / v->chunk);
    double tw = mem_now(); int blocked;
    pthread_mutex_lock(&g_pool_mx); if (v->mapped < m && !g_vmm_go) { g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); }   /* (a waiter before rns_init released it: tests, init-only) */
    blocked = v->mapped < m ? v->mapped : -1;
    while (v->mapped < m) pthread_cond_wait(&g_vmm_cv, &g_pool_mx); pthread_mutex_unlock(&g_pool_mx);
    if (blocked >= 0) db_tl("APU%d waited %.2f s for chunks %d..%d of the arena", dev, mem_now() - tw, blocked, m - 1);
    if (m >= v->m0 && v->bg_on) { pthread_join(v->bg, 0); v->bg_on = 0; if (vmm_vb()) printf("dbig pool: APU%d VMM arena: the background thread mapped chunks %d..%d in %.2f s\n", dev, (int)(v->bytes ? 0 : 0), v->m0 - 1, v->t_bg); }
}
size_t db_pool_vmm_chunk(void)                      /* the VMM arena's chunk (DB_POOL_VMM_CHUNK_GB, 2 GiB), as db_vmm_arena_alloc takes it; binsplit.c's BS_ARENA_ROOM rounds the arenas to it */
{
    const char *e = getenv("DB_POOL_VMM_CHUNK_GB"); double cg = e ? atof(e) : 2.0; size_t C = (size_t)(cg * 1073741824.0); if (C < ((size_t)2 << 20)) C = (size_t)2 << 20;
    return C / ((size_t)2 << 20) * ((size_t)2 << 20);
}
void *db_vmm_arena_alloc(int dev, size_t bytes, size_t first)   /* the arena of `bytes` (rounded up to whole chunks: the rest is free pool space) as a VMM range; a borrowed region
                                                                 * record.  The first `first` bytes (the parity-0 half: the seeds' target) are mapped before returning, the rest by a thread
                                                                 * (db_vmm_arena_wait before their first use; mapping them in the background saves ~8 s of init) */
{
    struct vmm *v = &g_vmm[dev]; double t0 = mem_now();
    size_t C = db_pool_vmm_chunk();
    const char *e = getenv("DB_POOL_VMM_RESERVE"); double rf = e ? atof(e) : 3.0; if (rf < 1.0) rf = 1.0;
    int m0 = (int)((bytes + C - 1) / C), nslot = (int)(m0 * rf) + 1; if (nslot < m0 + 1) nslot = m0 + 1;
    int nd; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd > DB_NQ) nd = DB_NQ;
    v->chunk = C; v->nslot = nslot; v->nd = nd; v->reserved = (size_t)nslot * C; v->h = (hipMemGenericAllocationHandle_t *)calloc(nslot, sizeof *v->h);
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t gran = 0; if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
    if (hipMemAddressReserve((void **)&v->base, v->reserved, gran, 0, 0) != hipSuccess) { ec_fatal(EC_RC_FATAL, "db_vmm_arena_alloc: APU %d: cannot reserve %.1f GB of VA\n", dev, v->reserved / 1e9); }
    int m1 = (int)((first + C - 1) / C); if (m1 > m0 || first == 0) m1 = m0;
    { const char *eb = getenv("ECALC_VMM_BG"); if (eb && atoi(eb) == 0) m1 = m0; }   /* s34: ECALC_VMM_BG=0 maps every chunk here, in the calling thread, and starts no background mapper (the rc139 crash is hipMemMap inside vmm_bg_map); default 1 = unchanged */
    v->m0 = m0; v->bytes = bytes; v->mapped = 0; v->bg_on = 0;
    v->mark_m = first ? (int)((2 * first + C - 1) / C) : m0; if (v->mark_m > m0) v->mark_m = m0;   /* binsplit.c's arena_get passes the parity half as `first`; level 1 waits for 2 first */
    double tm = mem_now(); db_tl("APU%d arena: mapping chunks 0..%d of %d (the parity-0 half)", dev, m1 - 1, m0);
    if (!vmm_map_run(dev, 0, m1, v->h)) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes);
    double tz = mem_now();
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev)); HIP_CHECK(hipMemset(v->base, 0, (size_t)m1 * C)); HIP_CHECK(hipDeviceSynchronize()); HIP_CHECK(hipSetDevice(cur));
    v->t_ms += mem_now() - tz;
    db_tl("APU%d arena: %d chunks mapped in %.2f s (create %.2f, map %.2f, access %.2f, zero %.2f)", dev, m1, mem_now() - tm, v->t_cr, v->t_mp, v->t_ac, v->t_ms);
    pthread_mutex_lock(&g_pool_mx);
    if (g_ndonated >= 256) { ec_fatal(EC_RC_FATAL, "db_vmm_arena_alloc: too many regions\n"); }
    v->reg = g_ndonated; g_donated[g_ndonated].p = v->base; g_donated[g_ndonated].dev = dev; g_donated[g_ndonated].bytes = (size_t)m0 * C; g_donated[g_ndonated].own = 0; g_donated[g_ndonated].kind = 1; g_ndonated++;
    v->mapped = m1;
    pthread_mutex_unlock(&g_pool_mx);
    mem_acct_register(db_acct);
    if (vmm_vb() || getenv("RNS_VERBOSE")) printf("dbig pool: APU%d VMM arena %.2f GB = %d chunks of %.2f GiB, the first %d mapped in %.2f s (%.3f s/GB)%s, VA %.1f GB reserved at %p\n", dev, (double)m0 * C / 1e9, m0, C / 1073741824.0, m1, mem_now() - t0, (mem_now() - t0) / ((double)m1 * C / 1e9), m1 < m0 ? ", the rest in the background" : "", v->reserved / 1e9, (void *)v->base);
    if (m1 < m0) { v->bg_on = 1; if (pthread_create(&v->bg, 0, vmm_bg_map, (void *)(intptr_t)dev)) { ec_fatal(EC_RC_FATAL, "db_vmm_arena_alloc: pthread_create\n"); } }
    else { v->n_grow = 0; v->grow_chunks = 0; if ((size_t)m0 * C > bytes) { pthread_mutex_lock(&g_pool_mx); ext_insert(dev, v->base + bytes, (size_t)m0 * C - bytes, v->reg); pthread_mutex_unlock(&g_pool_mx); } }
    return v->base;
}
void db_vmm_arena_release(int dev)                     /* after the pool is done with it (rns_shutdown): every chunk unmapped and released, the VA freed */
{
    struct vmm *v = &g_vmm[dev]; if (!v->base) return;
    db_vmm_arena_wait(dev, (size_t)-1);
    for (int k = 0; k < v->nslot; k++) if (v->h[k]) { (void)hipMemUnmap(v->base + (size_t)k * v->chunk, v->chunk); (void)hipMemRelease(v->h[k]); v->h[k] = 0; }
    (void)hipMemAddressFree(v->base, v->reserved); free(v->h);
    if (vmm_vb()) printf("dbig pool: APU%d VMM arena released (%zu remaps of %zu chunks in %.2f s, %zu growths of %zu chunks)\n", dev, v->n_remap, v->remap_chunks, v->t_remap, v->n_grow, v->grow_chunks);
    memset(v, 0, sizeof *v);
}
static int vmm_region_has(int dev, const char *p, size_t bytes, int *reg)   /* is [p, p + bytes) inside device dev's VMM range? */
{
    struct vmm *v = &g_vmm[dev]; if (!v->base || p < v->base || p + bytes > v->base + v->reserved) return 0;
    *reg = v->reg; return 1;
}
static void ext_remove(int d, char *p, size_t bytes)   /* [p, p + bytes) out of the free extents (it lies inside one extent: a wholly free chunk) */
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n;
    for (int i = 0; i < n; i++) if (p >= e[i].p && p + bytes <= e[i].p + e[i].bytes) {
        char *a = e[i].p, *b = e[i].p + e[i].bytes; int reg = e[i].reg;
        if (p == a) { e[i].p += bytes; e[i].bytes -= bytes; if (!e[i].bytes) { memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; } }
        else { e[i].bytes = (size_t)(p - a); if (p + bytes < b) ext_insert(d, p + bytes, (size_t)(b - (p + bytes)), reg); }
        return;
    }
    ec_fatal(EC_RC_FATAL, "dbig: ext_remove: range not free\n");
}
static int vmm_make_room(int d, size_t need)           /* (the pool lock held) a contiguous free extent of >= need bytes by remapping; 0 = not possible (the VA is exhausted) */
{
    struct vmm *v = &g_vmm[d]; if (!v->base) return 0;
    if (v->mapped < v->m0 && !g_vmm_go) { g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); }
    while (v->mapped < v->m0) pthread_cond_wait(&g_vmm_cv, &g_pool_mx);   /* (the lock is held) the background mapping first: the slot table must be complete */
    double t0 = mem_now(); size_t C = v->chunk; int m = (int)((need + C - 1) / C);
    int *fr = (int *)malloc(v->nslot * sizeof *fr), nf = 0;      /* the wholly free mapped slots */
    for (int k = 0; k < v->nslot && nf < m; k++) if (v->h[k]) {
        char *p = v->base + (size_t)k * C; int inside = 0;
        for (int i = 0; i < g_ext[d].n; i++) if (p >= g_ext[d].e[i].p && p + C <= g_ext[d].e[i].p + g_ext[d].e[i].bytes) { inside = 1; break; }
        if (inside) fr[nf++] = k;
    }
    hipMemGenericAllocationHandle_t *hs = (hipMemGenericAllocationHandle_t *)calloc(m, sizeof *hs);
    for (int i = 0; i < nf; i++) { int k = fr[i]; ext_remove(d, v->base + (size_t)k * C, C); if (hipMemUnmap(v->base + (size_t)k * C, C) != hipSuccess) { ec_fatal(EC_RC_FATAL, "dbig: hipMemUnmap failed\n"); } hs[i] = v->h[k]; v->h[k] = 0; }
    int slot0 = -1;                                               /* the lowest run of m empty slots */
    for (int k = 0, run = 0; k < v->nslot; k++) { run = v->h[k] ? 0 : run + 1; if (run == m) { slot0 = k - m + 1; break; } }
    if (slot0 < 0) {                                              /* no room in the VA: put the free chunks back where they were */
        for (int i = 0; i < nf; i++) { int k = fr[i]; hipMemGenericAllocationHandle_t one[1] = { hs[i] }; if (!vmm_map_run(d, k, 1, one)) { ec_fatal(EC_RC_FATAL, "dbig: VMM restore failed\n"); } ext_insert(d, v->base + (size_t)k * C, C, v->reg); }
        free(fr); free(hs); return 0;
    }
    if (!vmm_map_run(d, slot0, m, hs)) { pthread_mutex_unlock(&g_pool_mx); mem_oom("dbig block pool (VMM chunks)", d, (size_t)(m - nf) * C); }
    ext_insert(d, v->base + (size_t)slot0 * C, (size_t)m * C, v->reg);
    v->n_remap++; v->remap_chunks += (size_t)nf; v->t_remap += mem_now() - t0;
    if (vmm_vb()) { size_t fb = 0; for (int i = 0; i < g_ext[d].n; i++) fb += g_ext[d].e[i].bytes;
                    printf("dbig pool: APU%d VMM remap for a %.2f GB request: %d free chunks moved%s to slot %d (%.2f GB contiguous) in %.3f s; free %.2f GB in %d extents, live %.2f GB\n",
                           d, need / 1e9, nf, m > nf ? " + new chunks mapped" : "", slot0, (double)m * C / 1e9, mem_now() - t0, fb / 1e9, g_ext[d].n, g_live_bytes[d] / 1e9);
                    if (m > nf) printf("dbig pool: APU%d VMM growth by %d chunks (%.2f GB) inside the phase\n", d, m - nf, (double)(m - nf) * C / 1e9); }
    free(fr); free(hs); return 1;
}
static uint64_t *q_alloc_locked(int d, size_t need);
static uint64_t *q_alloc(int d, size_t need) { pthread_mutex_lock(&g_pool_mx); uint64_t *p = q_alloc_locked(d, need); pthread_mutex_unlock(&g_pool_mx); return p; }
static uint64_t *q_alloc_locked(int d, size_t need)                 /* need: bytes, a multiple of DB_ALIGN limbs */
{
    int reg;
    char *p = ext_take(d, need, &reg);
    if (!p && vmm_make_room(d, need)) p = ext_take(d, need, &reg);   /* the VMM arena re-stitches its free chunks */
    if (!p) {
        static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0);
        if (vb) { size_t fr = 0, lg = 0; for (int i = 0; i < g_ext[d].n; i++) { fr += g_ext[d].e[i].bytes; if (g_ext[d].e[i].bytes > lg) lg = g_ext[d].e[i].bytes; }
                  printf("dbig pool: APU%d hipMalloc %.2f GB inside the phase (free %.2f GB in %d extents, largest %.2f; live %.2f GB in %d blocks)\n", d, need / 1e9, fr / 1e9, g_ext[d].n, lg / 1e9, g_live_bytes[d] / 1e9, g_nlive); }   /* why the pre-sized pool still grows */
        g_pool_bytes += need;
        int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d));
        void *m; if (hipMalloc(&m, need) != hipSuccess) { pthread_mutex_unlock(&g_pool_mx); mem_oom("dbig block pool (inside a phase)", d, need); } HIP_CHECK(hipMemset(m, 0, need)); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipSetDevice(cur));
        if (g_ndonated < 256) { reg = g_ndonated; g_donated[g_ndonated].p = m; g_donated[g_ndonated].dev = d; g_donated[g_ndonated].bytes = need; g_donated[g_ndonated].own = 1; g_donated[g_ndonated].kind = 2; g_ndonated++; } else { ec_fatal(EC_RC_FATAL, "dbig: region table full\n"); }
        p = (char *)m;
    }
    live_add((uint64_t *)p, d, need, reg);
    return (uint64_t *)p;
}
static void q_release(int d, uint64_t *p) { int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(p)); HIP_CHECK(hipSetDevice(cur)); }
static void q_free(int d, uint64_t *p)
{
    pthread_mutex_lock(&g_pool_mx);
    int reg; size_t bytes = live_take(p, &reg); if (!bytes) { ec_fatal(EC_RC_FATAL, "dbig: freeing an unknown block\n"); }
    ext_insert(d, (char *)p, bytes, reg);
    pthread_mutex_unlock(&g_pool_mx);
}
void db_donate(int dev, void *p, size_t bytes) { db_donate_ext(dev, p, bytes, 1); }
/* the pool's live bytes on a device now (read without the lock by the sampler thread: a size_t, one
 * value may be a moment old), and the peak since the last reset (reset = 1 restarts the window at the current value) */
size_t db_pool_live(int dev) { return dev >= 0 && dev < DB_NQ ? __atomic_load_n(&g_live_bytes[dev], __ATOMIC_RELAXED) : 0; }
size_t db_pool_window_peak(int dev, int reset)
{
    if (dev < 0 || dev >= DB_NQ) return 0;
    pthread_mutex_lock(&g_pool_mx);
    size_t p = g_win_peak[dev]; if (reset) g_win_peak[dev] = g_live_bytes[dev];
    pthread_mutex_unlock(&g_pool_mx);
    return p;
}
/* M3: per-APU scratch from the block pool (the multi-node product's packed slabs and the layered comm's transposes) */
uint64_t *db_pool_alloc(int dev, size_t bytes) { size_t al = (size_t)DB_ALIGN * 8; return q_alloc(dev, (bytes + al - 1) / al * al); }
void db_pool_free(int dev, uint64_t *p) { if (p) q_free(dev, p); }
void db_donate_ext(int dev, void *p, size_t bytes, int own)
{
    pthread_mutex_lock(&g_pool_mx);
    if (g_ndonated >= 256) { ec_fatal(EC_RC_FATAL, "db_donate: too many regions\n"); }
    g_donated[g_ndonated].p = p; g_donated[g_ndonated].dev = dev; g_donated[g_ndonated].bytes = bytes; g_donated[g_ndonated].own = own; g_donated[g_ndonated].kind = own ? 0 : 1; g_ndonated++;
    ext_insert(dev, (char *)p, bytes, g_ndonated - 1);
    pthread_mutex_unlock(&g_pool_mx);
    mem_acct_register(db_acct);
}
/* a borrowed range that is part of the same allocation as an already borrowed region next to it (the
 * two parities of a bs region arena, donated at different moments): joined into that region's record, so that
 * their extents coalesce (extents never merge across regions, a block must stay inside one allocation).  With no
 * neighbour it is an ordinary borrowed region. */
void db_donate_adjacent(int dev, void *p, size_t bytes)
{
    pthread_mutex_lock(&g_pool_mx);
    { int reg; if (vmm_region_has(dev, (const char *)p, bytes, &reg)) {                                   /* a range of the VMM arena joins its record (mapped first) */
        struct vmm *v = &g_vmm[dev]; int m = (int)(((char *)p + bytes - v->base + v->chunk - 1) / v->chunk); if (m > v->m0) m = v->m0;
        if (v->mapped < m && !g_vmm_go) { g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); }
        while (v->mapped < m) pthread_cond_wait(&g_vmm_cv, &g_pool_mx);
        ext_insert(dev, (char *)p, bytes, reg); pthread_mutex_unlock(&g_pool_mx); return; } }
    for (int i = 0; i < g_ndonated; i++) {
        if (g_donated[i].dev != dev || g_donated[i].own) continue;
        char *rp = (char *)g_donated[i].p; size_t rb = g_donated[i].bytes;
        if ((char *)p == rp + rb) { g_donated[i].bytes += bytes; ext_insert(dev, (char *)p, bytes, i); pthread_mutex_unlock(&g_pool_mx); return; }
        if ((char *)p + bytes == rp) { g_donated[i].p = p; g_donated[i].bytes += bytes; ext_insert(dev, (char *)p, bytes, i); pthread_mutex_unlock(&g_pool_mx); return; }
    }
    pthread_mutex_unlock(&g_pool_mx);
    db_donate_ext(dev, p, bytes, 0);
}
/* M9: the block pool's bytes per device for mem_report */
static void db_acct(int ndev, size_t b[][MEM_DEV_NCAT])
{
    pthread_mutex_lock(&g_pool_mx);
    for (int i = 0; i < g_ndonated; i++) { int d = g_donated[i].dev; if (d < 0 || d >= ndev) continue;
        size_t bytes = g_donated[i].bytes;
        if (g_vmm[d].base && i == g_vmm[d].reg) { bytes = 0; for (int k = 0; k < g_vmm[d].nslot; k++) if (g_vmm[d].h[k]) bytes += g_vmm[d].chunk; }   /* the chunks mapped now */
        b[d][g_donated[i].kind == 2 ? MEM_DEV_POOL_HIPMALLOC : g_donated[i].kind == 1 ? MEM_DEV_POOL_BORROWED : MEM_DEV_POOL_DONATED] += bytes; }
    for (int d = 0; d < DB_NQ && d < ndev; d++) { b[d][MEM_DEV_POOL_LIVE] += g_live_bytes[d]; b[d][MEM_DEV_POOL_PEAK_LIVE] += g_peak_live[d]; b[d][MEM_DEV_POOL_FREE] += db_pool_free_bytes(d); }
    pthread_mutex_unlock(&g_pool_mx);
}
size_t db_pool_free_bytes(int d) { size_t s = 0; for (int i = 0; i < g_ext[d].n; i++) s += g_ext[d].e[i].bytes; return s; }
int db_pool_extents(int d) { return g_ext[d].n; }
/* (results/PC15.md; RNS_DIST_CACHE_PARTIAL, rns_dist.c cache_pc_take): how many blocks of `bytes` the free extents hold as
 * they are -- no remap, no growth (each block within one extent) */
size_t db_pool_fit_count(int d, size_t bytes) { size_t al = (size_t)DB_ALIGN * 8, b = (bytes + al - 1) / al * al, c = 0; if (!b) return 0; pthread_mutex_lock(&g_pool_mx); for (int i = 0; i < g_ext[d].n; i++) c += g_ext[d].e[i].bytes / b; pthread_mutex_unlock(&g_pool_mx); return c; }
size_t db_pool_hipmalloc_bytes(int d) { size_t s = 0; pthread_mutex_lock(&g_pool_mx); for (int i = 0; i < g_ndonated; i++) if (g_donated[i].dev == d && g_donated[i].kind == 2) s += g_donated[i].bytes; pthread_mutex_unlock(&g_pool_mx); return s; }
void db_release_pools(void)
{
    { static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0);   /* how the reserved tails were used */
      if (vb) for (int d = 0; d < DB_NQ; d++) if (g_tail[d].bytes) printf("dbig pool: APU%d tail %.2f GB: %zu large blocks from its back, %zu small blocks spilled into it\n", d, g_tail[d].bytes / 1e9, g_tail[d].n_tail, g_tail[d].n_spill); }
    for (int d = 0; d < DB_NQ; d++) g_ext[d].n = 0;              /* every extent is a piece of a whole region */
    for (int i = 0; i < g_ndonated; i++) if (g_donated[i].own) q_release(g_donated[i].dev, (uint64_t *)g_donated[i].p);
    g_ndonated = 0; g_nlive = 0; g_pool_bytes = 0;
    memset(g_live_bytes, 0, sizeof g_live_bytes); memset(g_win_peak, 0, sizeof g_win_peak); memset(g_tail, 0, sizeof g_tail);   /* the reserved tails go with their regions */
}
size_t db_pool_bytes(void) { return g_pool_bytes; }
void db_init(dbig *x) { par_init(); memset(x, 0, sizeof *x); }
void db_free(dbig *x) { if (x->cap) for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->q[d]); memset(x, 0, sizeof *x); }
void db_reserve(dbig *x, size_t limbs)
{
    if (limbs <= x->cap) return;
    double t0 = tnow(); db_st.n_reserve++;
    if (x->off) { ec_fatal(EC_RC_FATAL, "db_reserve: a view\n"); }
    /* quarters of exactly ceil(limbs/4) rounded up to DB_ALIGN: no size classes (a 2^l / 3 2^l class wasted up to
     * 45 % of the dm phase's device memory at 4e10, RESULTS.md 70); the pool coalesces any sizes */
    size_t need = (limbs + DB_NQ - 1) / DB_NQ, qc = (need + DB_ALIGN - 1) / DB_ALIGN * DB_ALIGN;
    dbig y; db_init(&y); y.cap = qc * DB_NQ; y.qc = qc;
    for (int d = 0; d < DB_NQ; d++) y.q[d] = q_alloc(d, qc * 8);
    if (x->n) {                                           /* keep the contents: quarter-wise DMA through the limb map */
        size_t n = x->n;
        for (int d = 0; d < DB_NQ; d++) {
            size_t lo = (size_t)d * y.qc, hi = lo + y.qc; if (hi > n) hi = n;
            for (size_t i = lo; i < hi;) {                 /* the source run containing i */
                size_t sd = hq(x, i), so = i - sd * x->qc, run = x->qc - so; if (i + run > hi) run = hi - i;
                mem_dev_copy_on(d, y.q[d] + (i - lo), x->q[sd] + so, run * 8); i += run;
            }
        }
        y.n = n;
    }
    for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->q[d]);
    *x = y;
    db_st.t_reserve += tnow() - t0;
}
/* (the kernels above synchronise the null stream only, not the device: a background copy on the non-blocking
 * stream below must not be waited for by every kernel of the Newton loop) */
/* host <-> device copies through a pinned bounce buffer per device (1 GiB): hipMemcpy with pageable host
 * memory runs at ~3 GB/s, pinned at ~50 GB/s; the host side is a parallel memcpy.  Quarters one after
 * another (concurrent hipMemcpy with pageable memory faults, RESULTS.md 59), chunks pipelined by two. */
#define BOUNCE ((size_t)1 << 27)                              /* limbs: 1 GiB */
static uint64_t *g_bounce[DB_NQ][2]; static hipStream_t g_bs[DB_NQ];
static pthread_mutex_t g_bounce_mx = PTHREAD_MUTEX_INITIALIZER;   /* copies may be issued from a background thread; one at a time per process */
static void bs_init(int d) { if (!g_bs[d]) HIP_CHECK(hipStreamCreateWithFlags(&g_bs[d], hipStreamNonBlocking)); }   /* copies from a background thread must not serialise with the kernels on the null stream */
static void bounce_init(int d)
{
    if (g_bounce[d][0]) return;
    HIP_CHECK(hipSetDevice(d));
    HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][0], BOUNCE * 8, 0)); HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][1], BOUNCE * 8, 0));
    bs_init(d);
}
/* DBIG_SMALL_BOUNCE=1 (default on; 0 = the two big buffers) -- a quarter of at most SBOUNCE limbs (8 MiB) goes through one small pinned buffer
 * per device instead of the two 1 GiB ones.  The first host copy of a run allocated 4 x 2 GiB of pinned host memory (≈2.2 s on
 * every aac7 node, longer on one, in the reciprocal's gather of 0.2 MB, results/O2.md); the small buffer costs milliseconds.
 * The bytes copied are the same, so digits are identical; the big buffers are still allocated on the first large copy. */
#define SBOUNCE ((size_t)1 << 20)
static uint64_t *g_sbounce[DB_NQ];
static int small_bounce_on(void) { static int v = -1; if (v < 0) v = getenv("DBIG_SMALL_BOUNCE") ? atoi(getenv("DBIG_SMALL_BOUNCE")) : 1; return v; }   /* default 1 since 2026-10-12 (user-adopted, results/CAND.md) */
static uint64_t *sbounce_get(int d)
{
    if (!g_sbounce[d]) {
        HIP_CHECK(hipSetDevice(d));
        HIP_CHECK(hipHostMalloc((void **)&g_sbounce[d], SBOUNCE * 8, 0)); bs_init(d);
    }
    return g_sbounce[d];
}
static void par_memcpy(uint64_t *dst, const uint64_t *src, size_t n)
{
#pragma omp parallel for schedule(static)
    for (size_t i = 0; i < n; i += 1 << 18) { size_t m = n - i < (1 << 18) ? n - i : (1 << 18); memcpy(dst + i, src + i, m * 8); }
}
void db_from_bi(dbig *x, const bigint *a)
{
    pthread_mutex_lock(&g_bounce_mx);
    db_reserve(x, a->n ? a->n : 1); x->n = a->n;
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo = (size_t)d * x->qc; if (lo >= a->n) break;
        size_t len = a->n - lo < x->qc ? a->n - lo : x->qc;
        if (len <= SBOUNCE && small_bounce_on()) {                  /* O2: one chunk through the small pinned buffer */
            uint64_t *sb = sbounce_get(d); HIP_CHECK(hipSetDevice(d));
            HIP_CHECK(hipStreamSynchronize(0));
            memcpy(sb, a->l + lo, len * 8);
            HIP_CHECK(hipMemcpyAsync(x->q[d], sb, len * 8, hipMemcpyHostToDevice, g_bs[d]));
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));
            continue;
        }
        bounce_init(d); HIP_CHECK(hipSetDevice(d)); int b = 0;
        HIP_CHECK(hipStreamSynchronize(0));                         /* nothing of ours still runs on the target */
        for (size_t i = 0; i < len; i += BOUNCE, b ^= 1) {
            size_t m = len - i < BOUNCE ? len - i : BOUNCE;
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));               /* the buffer's previous upload is done */
            par_memcpy(g_bounce[d][b], a->l + lo + i, m);
            HIP_CHECK(hipMemcpyAsync(x->q[d] + i, g_bounce[d][b], m * 8, hipMemcpyHostToDevice, g_bs[d]));
        }
        HIP_CHECK(hipStreamSynchronize(g_bs[d]));
    }
    pthread_mutex_unlock(&g_bounce_mx);
}
void db_to_bi(bigint *r, const dbig *x)
{
    pthread_mutex_lock(&g_bounce_mx);
    bi_reserve(r, x->n ? x->n : 1); r->n = x->n;
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo = (size_t)d * x->qc; if (lo >= x->n) break;
        size_t len = x->n - lo < x->qc ? x->n - lo : x->qc;
        if (len <= SBOUNCE && small_bounce_on()) {                  /* O2: one chunk through the small pinned buffer */
            uint64_t *sb = sbounce_get(d); HIP_CHECK(hipSetDevice(d));
            HIP_CHECK(hipStreamSynchronize(0));                     /* the source is complete */
            HIP_CHECK(hipMemcpyAsync(sb, x->q[d], len * 8, hipMemcpyDeviceToHost, g_bs[d]));
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));
            memcpy(r->l + lo, sb, len * 8);
            continue;
        }
        bounce_init(d); HIP_CHECK(hipSetDevice(d)); int b = 0; size_t prev = 0, prevm = 0; int have = 0;
        HIP_CHECK(hipStreamSynchronize(0));                         /* the source is complete (its kernels ran on the null stream) */
        for (size_t i = 0; i < len; i += BOUNCE, b ^= 1) {
            size_t m = len - i < BOUNCE ? len - i : BOUNCE;
            HIP_CHECK(hipMemcpyAsync(g_bounce[d][b], x->q[d] + i, m * 8, hipMemcpyDeviceToHost, g_bs[d]));
            if (have) par_memcpy(r->l + lo + prev, g_bounce[d][b ^ 1], prevm);   /* the previous chunk, already landed */
            HIP_CHECK(hipStreamSynchronize(g_bs[d]));
            prev = i; prevm = m; have = 1;
        }
        if (have) par_memcpy(r->l + lo + prev, g_bounce[d][b ^ 1], prevm);
    }
    pthread_mutex_unlock(&g_bounce_mx);
}

/* ---- residue mod q of a device number (the T1 residues of P, Q, R without host copies) ----
 * coalesced: a block of MQ_T threads owns a chunk of MQ_T MQ_K limbs; thread t takes the limbs i = t (mod MQ_T)
 * of the chunk (Horner with B^MQ_T from the top), scales by B^t and the block sums mod q; the host combines
 * the chunk values with B^(MQ_T MQ_K), Horner from the top quarter down */
#define MQ_T 256
#define MQ_K 64
#define MQ_CH ((size_t)MQ_T * MQ_K)
__global__ void k_modq(const uint64_t *x, size_t n, int nq, const uint64_t *qs, const uint64_t *Bts, const uint64_t *powt, uint64_t *out)   /* nq primes at once; powt[j MQ_T + t] = B^t mod q_j; out[c nq + j] */
{
    __shared__ uint64_t sh[MQ_T];
    size_t base = (size_t)blockIdx.x * MQ_CH; unsigned t = threadIdx.x;
    for (int j = 0; j < nq; j++) {
        uint64_t q = qs[j], Bt = Bts[j], v = 0;
        for (int k = MQ_K; k-- > 0;) { size_t i = base + (size_t)k * MQ_T + t; uint64_t xi = i < n ? x[i] : 0; v = (uint64_t)(((unsigned __int128)v * Bt + xi) % q); }
        sh[t] = (uint64_t)((unsigned __int128)v * powt[j * MQ_T + t] % q);
        __syncthreads();
        for (unsigned s2 = MQ_T / 2; s2 > 0; s2 >>= 1) { if (t < s2) { uint64_t a = sh[t] + sh[t + s2]; sh[t] = a >= q ? a - q : a; } __syncthreads(); }
        if (t == 0) out[blockIdx.x * nq + j] = sh[0];
        __syncthreads();
    }
}
/* k_modq with Montgomery products (R = 2^64) in place of the 128-bit `%`.  Needs q odd and 2^62 <= q < 2^63 (the T1 primes; the host falls back to k_modq otherwise):
 * v <- montmul(v, B^MQ_T R mod q) + (x mod q) is (v B^MQ_T + x) mod q exactly, and sh <- montmul(v, B^t R mod q); the residues are those of k_modq, bit for bit. */
__device__ __forceinline__ uint64_t mq_mm(uint64_t a, uint64_t b, uint64_t q, uint64_t qi)   /* a b / 2^64 mod q, a < 2^64, b < q, qi = q^-1 mod 2^64 */
{
    uint64_t lo = a * b, hi = __umul64hi(a, b), m = lo * qi, t = __umul64hi(m, q);
    return hi >= t ? hi - t : hi - t + q;
}
__global__ void k_modq_m(const uint64_t *x, size_t n, int nq, const uint64_t *qs, const uint64_t *Bts, const uint64_t *powt, uint64_t *out)   /* Bts, powt in Montgomery form (times 2^64 mod q) */
{
    __shared__ uint64_t sh[MQ_T];
    size_t base = (size_t)blockIdx.x * MQ_CH; unsigned t = threadIdx.x;
    for (int j = 0; j < nq; j++) {
        uint64_t q = qs[j], Bt = Bts[j], v = 0, qi = q;
        qi *= 2 - q * qi; qi *= 2 - q * qi; qi *= 2 - q * qi; qi *= 2 - q * qi; qi *= 2 - q * qi; qi *= 2 - q * qi;   /* Newton: q is its own inverse mod 8; 3 -> 6 -> 12 -> 24 -> 48 -> 96 bits */
        const uint64_t q2 = 2 * q;
        for (int k = MQ_K; k-- > 0;) {
            size_t i = base + (size_t)k * MQ_T + t; uint64_t xi = i < n ? x[i] : 0;
            xi = xi >= q2 ? xi - q2 : xi; xi = xi >= q ? xi - q : xi;
            v = mq_mm(v, Bt, q, qi) + xi; v = v >= q ? v - q : v;
        }
        sh[t] = mq_mm(v, powt[j * MQ_T + t], q, qi);
        __syncthreads();
        for (unsigned s2 = MQ_T / 2; s2 > 0; s2 >>= 1) { if (t < s2) { uint64_t a = sh[t] + sh[t + s2]; sh[t] = a >= q ? a - q : a; } __syncthreads(); }
        if (t == 0) out[blockIdx.x * nq + j] = sh[0];
        __syncthreads();
    }
}
static uint64_t powmod_h(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1; b %= q; while (e) { if (e & 1) r = (uint64_t)((unsigned __int128)r * b % q); b = (uint64_t)((unsigned __int128)b * b % q); e >>= 1; } return r; }
static uint64_t *g_mq_out[DB_NQ], *g_mq_pow[DB_NQ]; static size_t g_mq_cap[DB_NQ];
static void qrange(const dbig *x, int d, size_t n, size_t *lo, size_t *hi);
#define MQ_MAXQ 16
/* residues of x modulo nq primes (each < 2^63) at once: the quarters in parallel, one launch per quarter */
static pthread_mutex_t g_mq_mx = PTHREAD_MUTEX_INITIALIZER;   /* the output stage's background thread takes X's residues while the division goes on (R's) -- the scratch above is shared */
static void db_mod_qs_locked(const dbig *x, const uint64_t *qs, int nq, uint64_t *res);
void db_mod_qs(const dbig *x, const uint64_t *qs, int nq, uint64_t *res)
{
    if (nq > MQ_MAXQ) { ec_fatal(EC_RC_FATAL, "db_mod_qs: %d primes\n", nq); }
    for (int j = 0; j < nq; j++) res[j] = 0;
    if (!x->n) return;
    pthread_mutex_lock(&g_mq_mx); db_mod_qs_locked(x, qs, nq, res); pthread_mutex_unlock(&g_mq_mx);
}
static void db_mod_qs_locked(const dbig *x, const uint64_t *qs, int nq, uint64_t *res)
{
    uint64_t Bq[MQ_MAXQ], Bt[MQ_MAXQ], Bch[MQ_MAXQ], powt[MQ_MAXQ * MQ_T];
    for (int j = 0; j < nq; j++) {
        uint64_t q = qs[j]; Bq[j] = BI_B10 % q; Bt[j] = powmod_h(Bq[j], MQ_T, q); Bch[j] = powmod_h(Bq[j], MQ_CH, q);
        powt[j * MQ_T] = 1; for (int t = 1; t < MQ_T; t++) powt[j * MQ_T + t] = (uint64_t)((unsigned __int128)powt[j * MQ_T + t - 1] * Bq[j] % q);
    }
    int mont = 1;   /* S51 K3 (k_modq_m); k_modq when a q is even or outside [2^62, 2^63) */
    for (int j = 0; j < nq; j++) if (!(qs[j] & 1) || qs[j] < (1ULL << 62) || qs[j] >= (1ULL << 63)) mont = 0;
    uint64_t Btm[MQ_MAXQ], powtm[MQ_MAXQ * MQ_T];
    if (mont) for (int j = 0; j < nq; j++) {
        uint64_t q = qs[j]; Btm[j] = (uint64_t)(((unsigned __int128)Bt[j] << 64) % q);
        for (int t = 0; t < MQ_T; t++) powtm[j * MQ_T + t] = (uint64_t)(((unsigned __int128)powt[j * MQ_T + t] << 64) % q);
    }
    uint64_t vq[DB_NQ][MQ_MAXQ]; size_t qlen[DB_NQ]; memset(vq, 0, sizeof vq); memset(qlen, 0, sizeof qlen);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(x, d, x->n, &lo, &hi); if (lo >= hi) continue;
        size_t first = x->off + lo - (size_t)d * x->qc, len = hi - lo, nc = (len + MQ_CH - 1) / MQ_CH; qlen[d] = len;
        HIP_CHECK(hipSetDevice(d));
        if (g_mq_cap[d] < nc * MQ_MAXQ) { if (g_mq_out[d]) HIP_CHECK(hipFree(g_mq_out[d])); HIP_CHECK(hipMalloc(&g_mq_out[d], (nc * MQ_MAXQ + 1024) * 8)); g_mq_cap[d] = nc * MQ_MAXQ + 1024; }
        if (!g_mq_pow[d]) HIP_CHECK(hipMalloc(&g_mq_pow[d], (MQ_MAXQ * MQ_T + 2 * MQ_MAXQ) * 8));
        uint64_t *dq = g_mq_pow[d] + MQ_MAXQ * MQ_T, *dbt = dq + MQ_MAXQ;
        HIP_CHECK(hipMemcpy(g_mq_pow[d], mont ? powtm : powt, (size_t)nq * MQ_T * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dq, qs, (size_t)nq * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dbt, mont ? Btm : Bt, (size_t)nq * 8, hipMemcpyHostToDevice));
#pragma omp critical
        if (mont) k_modq_m<<<(unsigned)nc, MQ_T>>>(x->q[d] + first, len, nq, dq, dbt, g_mq_pow[d], g_mq_out[d]); else
        k_modq<<<(unsigned)nc, MQ_T>>>(x->q[d] + first, len, nq, dq, dbt, g_mq_pow[d], g_mq_out[d]);
        HIP_CHECK(hipStreamSynchronize(0));
        uint64_t *hv = (uint64_t *)malloc(nc * nq * 8);
        HIP_CHECK(hipMemcpy(hv, g_mq_out[d], nc * nq * 8, hipMemcpyDeviceToHost));
        for (int j = 0; j < nq; j++) { uint64_t v = 0; for (size_t c = nc; c-- > 0;) v = (uint64_t)(((unsigned __int128)v * Bch[j] + hv[c * nq + j]) % qs[j]); vq[d][j] = v; }
        free(hv);
    }
    for (int j = 0; j < nq; j++) { uint64_t r = 0; for (int d = DB_NQ; d-- > 0;) if (qlen[d]) r = (uint64_t)(((unsigned __int128)r * powmod_h(Bq[j], qlen[d], qs[j]) + vq[d][j]) % qs[j]); res[j] = r; }
}
uint64_t db_mod_q(const dbig *x, uint64_t q) { uint64_t r; db_mod_qs(x, &q, 1, &r); return r; }
/* ---- kernels: one per quarter, over the result's limbs [lo, hi) of that quarter ---- */
__global__ void k_gather_shift(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, long shift)   /* out[i] = a[i + shift] or 0 */
{
    size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < hi; i += stride) { long s = (long)i + shift; out[i - lo] = (s >= 0 && (size_t)s < an) ? dget_s(a, (size_t)s) : 0; }
}
/* add/sub of one chunk (CH = 4096 limbs, k_addsub3 below) with a block-level carry scan: each segment's generate / propagate with carry-in 0, a scan
 * gives every segment's carry-in, the segment is redone with it.  r may be a or b (same layout: a thread only
 * touches its own limbs).  cout/prop per chunk for the host scan across chunks and quarters.
 * The b operand is either a dbig or a sparse set of 4-limb spills (sp != 0): spill j sits at limb
 * R j + row0 + rows for row0 in {0, rows, 2 rows, 3 rows} (four ranks' spill arrays). */
struct sparse { const uint64_t *sp[4]; size_t R, rows, C; int single; size_t pos; uint64_t val;
                size_t lo; int gt; const size_t *tab[4]; int p24; };   /* p24 (results/P2415.md): the blocks at L(R j + a') of p24.h */   /* tab[d] (the exact spill exchange): sp[d] holds only the blocks that meet this share, per source node r the columns [tab[3r], tab[3r+1]) at block offset tab[3r+2]; 0 = the full [r][j][4] layout */   /* single: one limb val at pos.  gt > 0 (M3): the node's share [lo, ..) of a number whose product ran on 4 gt ranks,
                                         * rank rho = gt d + r: sp[d] holds [r][j][4], spill (rho, j) at global limb R j + (rho + 1) rows -- or, when rows nr != R
                                         * (gt = g nodes of any count, rows = floor(R / nr)), at R j + R (rho + 1) / nr */
__device__ static inline uint64_t sparse_get(const struct sparse s, size_t i)
{
    if (s.single) return i == s.pos ? s.val : 0;
    if (s.gt && s.p24) {                                      /* P24: the block meeting limb i + lo (p24_spill_at: at most one), then the lookup below */
        int rho; size_t j, t; if (!p24_spill_at(i + s.lo, s.R, s.C, 4 * s.gt, &rho, &j, &t)) return 0;
        int d = rho / s.gt, r = rho - d * s.gt;
        if (s.tab[d]) { const size_t *e = s.tab[d] + 3 * (size_t)r; if (j < e[0] || j >= e[1]) return 0; return s.sp[d][(e[2] + (j - e[0])) * 4 + t]; }
        return s.sp[d][((size_t)r * s.C + j) * 4 + t];
    }
    if (s.gt) {
        size_t m = i + s.lo, j = m / s.R, rem = m - j * s.R, q, t; int nr = 4 * s.gt, rho;
        if (s.rows * (size_t)nr == s.R) { q = rem / s.rows; t = rem - q * s.rows; }
        else { q = ((rem + 1) * (size_t)nr + s.R - 1) / s.R - 1; t = rem - s.R * q / nr; }   /* unequal parts -- rank rho holds rows [R rho / nr, R (rho+1) / nr); q = the rank whose part starts at or below rem, i.e. rho + 1 of the spill's rank */
        if (t >= 4) return 0; if (q == 0) { if (j < 1 || j - 1 >= s.C) return 0; rho = nr - 1; j--; } else { rho = (int)q - 1; if (j >= s.C) return 0; }
        int d = rho / s.gt, r = rho - d * s.gt;
        if (s.tab[d]) { const size_t *e = s.tab[d] + 3 * (size_t)r; if (j < e[0] || j >= e[1]) return 0; return s.sp[d][(e[2] + (j - e[0])) * 4 + t]; }   /* the compact form */
        return s.sp[d][((size_t)r * s.C + j) * 4 + t];
    }
    /* i = R j + (r+1) rows + t, t < 4: j = i / R, rem = i - R j; r+1 = rem / rows if rem % rows < 4 */
    size_t j = i / s.R, rem = i - j * s.R, q = rem / s.rows, t = rem - q * s.rows;
    if (t >= 4) return 0;
    if (q == 0) return (j >= 1 && j - 1 < s.C) ? s.sp[3][(j - 1) * 4 + t] : 0;   /* rank 3's spill of column j-1 lands at R j */
    return j < s.C ? s.sp[q - 1][j * 4 + t] : 0;
}
/* 16 KB tile and a wave-level carry scan.
 * The chunk is CH = 4096 limbs with the flags (cout / prop) that the host scan and k_carry use.  Each thread owns two 8-limb segments (A: limbs 8 tid .. , B: 2048 + 8 tid ..): the block loads and stores
 * the chunk in two halves of 2048 limbs through a 16 KB XOR-swizzled tile (four blocks per CU, against two), and the (generate, propagate) pairs of the 512 segments in
 * order are scanned by __shfl_up inside each wave plus a four-entry combine, not by thread 0 over 2 x 256 entries. */
__device__ __forceinline__ void a3_gp(const uint64_t *x, const uint64_t *y, size_t s0, size_t c1, int sub, unsigned &g, unsigned &p)
{
    uint64_t cy = 0; int pr = 1;
    for (int k = 0; k < 8; k++) { if (s0 + k >= c1) break;
        uint64_t xx = x[k], yy = y[k], s;
        { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; pr &= (xx == yy); } else { s = xx + yy + cy; cy = s >= B10; pr &= (xx + yy == B10 - 1); } }
    }
    g = (unsigned)cy; p = (s0 < c1) ? (unsigned)pr : 1u;
}
__device__ __forceinline__ void a3_apply(const uint64_t *x, const uint64_t *y, uint64_t *o, size_t s0, size_t c1, int sub, uint64_t cy)
{
    for (int k = 0; k < 8; k++) { o[k] = 0; if (s0 + k >= c1) break;
        uint64_t xx = x[k], yy = y[k], s;
        { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; s = cy ? s : s - B10; } else { s = xx + yy + cy; cy = s >= B10; s = cy ? s - B10 : s; } }
        o[k] = s;
    }
}
/* ordered scan of (g, p) over the block's 256 threads (thread order = segment order), starting from carry cinit; Wg / Wp: one entry per wave (shared);
 * returns this segment's carry-in, the carry out of the whole set and the AND of all p.  Must be called by every thread. */
__device__ __forceinline__ void a3_scan(unsigned g, unsigned p, unsigned cinit, volatile unsigned *Wg, volatile unsigned *Wp, unsigned &cin, unsigned &cend, unsigned &pall)
{
    const int lane = threadIdx.x % warpSize, wv = threadIdx.x / warpSize, nw = blockDim.x / warpSize;
    unsigned G = g, P = p;
    for (int o = 1; o < warpSize; o <<= 1) { unsigned g2 = __shfl_up(G, o), p2 = __shfl_up(P, o); if (lane >= o) { G = G | (P & g2); P = P & p2; } }
    if (lane == warpSize - 1) { Wg[wv] = G; Wp[wv] = P; }
    __syncthreads();
    unsigned c = cinit, pa = 1, cw = cinit;
    for (int w = 0; w < nw; w++) { if (w == wv) cw = c; c = Wg[w] | (Wp[w] & c); pa &= Wp[w]; }
    unsigned co = G | (P & cw);                              /* carry out of this segment */
    unsigned prev = __shfl_up(co, 1);
    cin = lane ? prev : cw; cend = c; pall = pa;
}
__global__ void __launch_bounds__(256) k_addsub3(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, struct dv b, size_t bn, struct sparse sp, int has_sp, int sub, uint8_t *cout, uint8_t *prop)
{
    __shared__ uint64_t T[2048];
    __shared__ unsigned W[4][8];
    const int tid = threadIdx.x;
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi;
    uint64_t x[16], y[16], o[16];
#define S3_IDX(e) ((((e) >> 3) << 3) + (((e) & 7) ^ (((e) >> 3) & 7)))
#pragma unroll
    for (int h = 0; h < 2; h++) {
#pragma unroll
        for (int j = 0; j < 8; j++) { int e = j * 256 + tid; size_t i = c0 + h * 2048 + e; T[S3_IDX(e)] = (i < c1 && i < an) ? dget_s(a, i) : 0; }
        __syncthreads();
#pragma unroll
        for (int k = 0; k < 8; k++) x[h * 8 + k] = T[tid * 8 + (k ^ (tid & 7))];
        __syncthreads();
#pragma unroll
        for (int j = 0; j < 8; j++) { int e = j * 256 + tid; size_t i = c0 + h * 2048 + e; T[S3_IDX(e)] = (i < c1) ? (has_sp ? (i < bn ? sparse_get(sp, i) : 0) : (i < bn ? dget_s(b, i) : 0)) : 0; }
        __syncthreads();
#pragma unroll
        for (int k = 0; k < 8; k++) y[h * 8 + k] = T[tid * 8 + (k ^ (tid & 7))];
        __syncthreads();
    }
    size_t sA = c0 + (size_t)tid * 8, sB = sA + 2048;
    unsigned gA, pA, gB, pB, cinA, cendA, pallA, cinB, cendB, pallB;
    a3_gp(x, y, sA, c1, sub, gA, pA);
    a3_gp(x + 8, y + 8, sB, c1, sub, gB, pB);
    a3_scan(gA, pA, 0u, W[0], W[1], cinA, cendA, pallA);
    a3_scan(gB, pB, cendA, W[2], W[3], cinB, cendB, pallB);
    if (tid == 0) { cout[blockIdx.x] = (uint8_t)cendB; prop[blockIdx.x] = (uint8_t)(pallA & pallB); }
    a3_apply(x, y, o, sA, c1, sub, cinA);
    a3_apply(x + 8, y + 8, o + 8, sB, c1, sub, cinB);
#pragma unroll
    for (int h = 0; h < 2; h++) {
#pragma unroll
        for (int k = 0; k < 8; k++) T[tid * 8 + (k ^ (tid & 7))] = o[h * 8 + k];
        __syncthreads();
#pragma unroll
        for (int j = 0; j < 8; j++) { int e = j * 256 + tid; size_t i = c0 + h * 2048 + e; if (i < c1) out[i - lo] = T[S3_IDX(e)]; }
        __syncthreads();
    }
#undef S3_IDX
}
/* apply a carry-in of 1 (or borrow) to a chunk, rippling until absorbed */
__global__ void k_carry(uint64_t *out, size_t lo, size_t hi, const uint8_t *cin, int sub)
{
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi || !cin[blockIdx.x]) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi;
    if (threadIdx.x) return;
    uint64_t *o = out + (c0 - lo);
    for (size_t i = c0; i < c1; i++) {
        uint64_t v = o[i - c0];
        { if (sub) { if (v) { o[i - c0] = v - 1; return; } o[i - c0] = B10 - 1; } else { if (v + 1 < B10) { o[i - c0] = v + 1; return; } o[i - c0] = 0; } }
    }
}
__global__ void k_maxidx(struct dv a, struct dv b, int hasb, size_t lo, size_t hi, size_t *res)   /* 1 + highest i in [lo,hi) with a[i] != b[i] (or != 0), per block */
{
    __shared__ size_t sm[256];
    size_t best = 0; int found = 0;
    for (size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < hi; i += (size_t)gridDim.x * blockDim.x)
        if (hasb ? dget_s(a, i) != dget_s(b, i) : dget_s(a, i) != 0) { if (!found || i > best) best = i; found = 1; }
    sm[threadIdx.x] = found ? best + 1 : 0;
    __syncthreads();
    for (int s = 128; s > 0; s >>= 1) { if (threadIdx.x < s && sm[threadIdx.x + s] > sm[threadIdx.x]) sm[threadIdx.x] = sm[threadIdx.x + s]; __syncthreads(); }
    if (threadIdx.x == 0) res[blockIdx.x] = sm[0];
}

static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 8 ? 228 * 8 : b); }
static uint8_t *g_flags[DB_NQ][2]; static size_t g_flags_cap[DB_NQ]; static size_t *g_red[DB_NQ];   /* per quarter, touched only by the thread driving quarter d (the shared host reduction buffer g_hred is gone -- its first-use allocation raced between the four threads) */
static void flags_reserve(int d, size_t chunks)
{
    if (g_flags_cap[d] >= chunks) return;
    HIP_CHECK(hipSetDevice(d));
    if (g_flags[d][0]) { HIP_CHECK(hipHostFree(g_flags[d][0])); HIP_CHECK(hipHostFree(g_flags[d][1])); }
    HIP_CHECK(hipHostMalloc((void **)&g_flags[d][0], chunks + 16, 0)); HIP_CHECK(hipHostMalloc((void **)&g_flags[d][1], chunks + 16, 0));   /* pinned host: written by the kernel, scanned by the host */
    g_flags_cap[d] = chunks;
    if (!g_red[d]) { HIP_CHECK(hipMalloc(&g_red[d], 228 * 8 * 8)); }
}
/* the limbs [lo, hi) of x (n limbs, possibly a view at x->off) whose storage is in quarter d */
static void qrange(const dbig *x, int d, size_t n, size_t *lo, size_t *hi)
{
    size_t g0 = (size_t)d * x->qc, g1 = g0 + x->qc;              /* global limb range of quarter d */
    *lo = g0 > x->off ? g0 - x->off : 0; *hi = g1 > x->off ? g1 - x->off : 0;
    if (*hi > n) *hi = n; if (*lo > n) *lo = n;
}

static void shift_into(dbig *r, const dbig *a, long shift, size_t n)      /* r[i] = a[i + shift], n limbs */
{
    double t0 = tnow(); db_st.n_shift++;
    if (r == a) { ec_fatal(EC_RC_FATAL, "db shift: in place\n"); }
    need_owner(r, "shift"); db_reserve(r, n ? n : 1);
    struct dv v = view_of(a);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(r, d, n, &lo, &hi); if (lo >= hi) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_gather_shift<<<nblk(hi - lo), 256>>>(r->q[d], lo, hi, v, a->n, shift);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    r->n = n; db_norm(r);
    db_st.t_shift += tnow() - t0;
}
void db_shr_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, (long)k, a->n > k ? a->n - k : 0); }
void db_shl_limbs(dbig *r, const dbig *a, size_t k) { shift_into(r, a, -(long)k, a->n ? a->n + k : 0); }
void db_copy(dbig *r, const dbig *a) { if (r == a) return; shift_into(r, a, 0, a->n); }

static void addsub_core2(dbig *r, const dbig *a, size_t ashift, const dbig *b, const struct sparse *spx, size_t bn, int sub, size_t nfix, int *cout, int *prop);
static void addsub_core(dbig *r, const dbig *a, size_t ashift, const dbig *b, const struct sparse *spx, size_t bn, int sub) { addsub_core2(r, a, ashift, b, spx, bn, sub, 0, 0, 0); }
/* nfix > 0 (M3): the result has exactly nfix limbs, the carry out of the top is reported in *cout instead of aborting, and
 * *prop tells whether a carry-in at limb 0 would ripple through every limb (the node-level carry scan of a sharded number) */
static void addsub_core2(dbig *r, const dbig *a, size_t ashift, const dbig *b, const struct sparse *spx, size_t bn, int sub, size_t nfix, int *cout, int *prop)
{
    double t0 = tnow(); db_st.n_addsub++;
    size_t an = a->n ? a->n + ashift : 0, n = an > bn ? an : bn; if (!sub) n++;
    if (nfix) n = nfix;
    /* in place is fine when the layouts match (a thread only touches its own limbs); otherwise a temporary */
    dbig tmp; int inplace = (r == a || r == b), same = (r == a && !r->off && !ashift) || (b && r == b && !r->off);   /* r == b is safe even with a shifted: a thread reads only its own limbs of r */
    dbig *out = r;
    if (inplace && !same) { db_init(&tmp); out = &tmp; }
    need_owner(out, "add/sub"); db_reserve(out, n ? n : 1);
    struct dv va = view_of(a), vb = b ? view_of(b) : va; va.shift = ashift; struct sparse sp; memset(&sp, 0, sizeof sp); if (spx) sp = *spx;
    size_t chunks[DB_NQ], lo[DB_NQ], hi[DB_NQ];
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        qrange(out, d, n, &lo[d], &hi[d]); chunks[d] = (hi[d] - lo[d] + CH - 1) / CH;
        if (!chunks[d]) continue;
        flags_reserve(d, chunks[d]);
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_addsub3<<<(unsigned)chunks[d], 256>>>(out->q[d], lo[d], hi[d], va, an, vb, bn, sp, spx != 0, sub, g_flags[d][0], g_flags[d][1]);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    /* scan the chunk flags in order: carry-in of chunk = carry-out of the previous, or its carry-in if it propagates */
    uint8_t cy = 0; int allp = 1;
    for (int d = 0; d < DB_NQ; d++) for (size_t c = 0; c < chunks[d]; c++) {
        uint8_t co = g_flags[d][0][c], pr = g_flags[d][1][c];
        g_flags[d][0][c] = cy;                          /* reuse as carry-in */
        cy = co | (pr & cy); allp &= pr;
    }
    if (nfix) { if (cout) *cout = cy; if (prop) *prop = allp; }
    else {
    if (cy && !sub) { ec_fatal(EC_RC_FATAL, "db_add: carry out of the top (n undersized)\n"); }
    if (cy && sub) { ec_fatal(EC_RC_FATAL, "db_sub: a < b\n"); }
    }
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        if (!chunks[d]) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_carry<<<(unsigned)chunks[d], 1>>>(out->q[d], lo[d], hi[d], g_flags[d][0], sub);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    out->n = n; if (!nfix) db_norm(out);
    if (inplace && !same) { dbig sw = *r; *r = tmp; tmp = sw; db_free(&tmp); }
    db_st.t_addsub += tnow() - t0;
}
/* the same with the compact spill buffers of the exact exchange (rns_dist.c mn_core): sp[d] holds, per
 * source node r in order, the 4-limb blocks of the columns [tab[d][3r], tab[d][3r+1]) at block offset tab[d][3r+2] (device tables) */
void db_share_add_spills_x(dbig *r, size_t n, size_t lo, const uint64_t *const sp[4], const size_t *const tab[4], size_t R, size_t rows, size_t C, int gt, int *cout, int *prop)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) { s.sp[q] = sp[q]; s.tab[q] = tab[q]; } s.R = R; s.rows = rows; s.C = C; s.lo = lo; s.gt = gt;
    addsub_core2(r, r, 0, 0, &s, n, 0, n, cout, prop);
}
/* the same for a P24 product -- the spill of (rank rho, column j) at limb L(R j + a'), a' the
 * next rank's first row (p24.h) */
void db_share_add_spills_x24(dbig *r, size_t n, size_t lo, const uint64_t *const sp[4], const size_t *const tab[4], size_t R, size_t C, int gt, int *cout, int *prop)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) { s.sp[q] = sp[q]; s.tab[q] = tab[q]; } s.R = R; s.rows = R / (4 * (size_t)gt); s.C = C; s.lo = lo; s.gt = gt; s.p24 = 1;
    addsub_core2(r, r, 0, 0, &s, n, 0, n, cout, prop);
}
/* M3: r (n limbs, in place) += 1 at limb 0; the carry out reported */
void db_share_add_one(dbig *r, size_t n, int *cout)
{
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = 0; s.val = 1;
    addsub_core2(r, r, 0, 0, &s, n, 0, n, cout, 0);
}
/* M4 (A-div, newton_db.c: the sharded division): r = a +/- b over exactly n limbs -- the shares of two numbers in
 * the same basis (r may be a or b); and r +/- val at limb pos (val < B; every node runs it, val = 0 elsewhere, so
 * the propagate flag of every share is known); carry / borrow out and propagate reported for the node scan */
void db_share_addsub(dbig *r, const dbig *a, const dbig *b, size_t n, int sub, int *cout, int *prop) { addsub_core2(r, a, 0, b, 0, n, sub, n, cout, prop); }
void db_share_add_val(dbig *r, size_t n, size_t pos, uint64_t val, int sub, int *cout, int *prop)
{
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = pos; s.val = val;
    addsub_core2(r, r, 0, 0, &s, n, sub, n, cout, prop);
}
/* r (n limbs, in place) += a << k; fixed length, carry out and propagate reported */
void db_share_add_shifted(dbig *r, size_t n, const dbig *a, size_t k, int *cout, int *prop) { addsub_core2(r, a, k, r, 0, n, 0, n, cout, prop); }
void db_add(dbig *r, const dbig *a, const dbig *b) { addsub_core(r, a, 0, b, 0, b->n, 0); }
void db_sub(dbig *r, const dbig *a, const dbig *b) { addsub_core(r, a, 0, b, 0, b->n, 1); }
void db_add_shifted(dbig *r, const dbig *a, size_t k, const dbig *b) { addsub_core(r, a, k, b, 0, b->n, 0); }   /* r = (a << k) + b */
void db_sub_shifted(dbig *r, const dbig *a, size_t k, const dbig *b) { addsub_core(r, a, k, b, 0, b->n, 1); }   /* r = (a << k) - b */
/* r = a + the sparse spill set (4 limbs at R j + (q+1) rows for q = 0..3, j < C), n limbs of result */
void db_add_spills(dbig *r, const dbig *a, const uint64_t *const sp[4], size_t R, size_t rows, size_t C, size_t n)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) s.sp[q] = sp[q]; s.R = R; s.rows = rows; s.C = C;
    addsub_core(r, a, 0, 0, &s, n, 0);
}
/* r = a - B^e  (a >= B^e) */
void db_sub_pow(dbig *r, const dbig *a, size_t e)
{
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = e; s.val = 1;
    addsub_core(r, a, 0, 0, &s, e + 1, 1);
}
/* r = B^e - a  (0 < a < B^e): the limb-wise complement (B-1-a[i], i < e) plus one */
__global__ void k_complement(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, uint64_t top)
{
    size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < hi; i += stride) out[i - lo] = top - (i < an ? dget(a, i) : 0);
}
void db_pow_sub(dbig *r, size_t e, const dbig *a)
{
    if (r == a) { ec_fatal(EC_RC_FATAL, "db_pow_sub: in place\n"); }
    need_owner(r, "pow_sub"); db_reserve(r, e + 1);
    struct dv va = view_of(a); uint64_t top = B10 - 1;
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(r, d, e, &lo, &hi); if (lo >= hi) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_complement<<<nblk(hi - lo), 256>>>(r->q[d], lo, hi, va, a->n, top);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    r->n = e; db_norm(r);
    struct sparse s; memset(&s, 0, sizeof s); s.single = 1; s.pos = 0; s.val = 1;
    addsub_core(r, r, 0, 0, &s, 1, 0);
}

static size_t maxidx(const dbig *a, const dbig *b, size_t n)      /* 1 + highest index i < n with a[i] != b[i] (b null: != 0), or 0 */
{
    double t0 = tnow(); db_st.n_maxidx++;
    size_t best = 0; struct dv va = view_of(a), vb = b ? view_of(b) : va;
#pragma omp parallel for num_threads(DB_NQ) reduction(max:best)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(a, d, n, &lo, &hi); if (lo >= hi) continue;
        flags_reserve(d, 1);
        HIP_CHECK(hipSetDevice(d));
        /* S40: scan windows from the top of the quarter (4096 limbs, then x16 wider), stop at the first window with a hit; the last
         * window reaches lo, so the answer is exactly that of the single full scan (the highest differing index is in the topmost window that has one) */
        size_t whi = hi, wsz = 4096; size_t m = 0; unsigned blocks;
        for (;;) {
            size_t wlo = whi - lo <= wsz ? lo : whi - wsz;
            blocks = nblk(whi - wlo);
#pragma omp critical
            k_maxidx<<<blocks, 256>>>(va, vb, b != 0, wlo, whi, g_red[d]);
        /* (results/A114.md) the per-block results come to this thread's own
         * buffer.  They went to a process-wide malloc'd g_hred whose first-use allocation (`if (!g_hred) g_hred = malloc()`)
         * ran unguarded inside this four-thread region: two threads could allocate, and a thread whose hipMemcpy landed in
         * one buffer read the other, uninitialised one.  The first db_norm of a process is the leaf hand-over's db_copy at
         * size > 1 (binsplit.c), so it returned heap garbage as P's length once (mn e8 size 4, job 21222: 18385101070989787659) */
            size_t hred[228 * 8];
            HIP_CHECK(hipMemcpy(hred, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
            for (unsigned i = 0; i < blocks; i++) if (hred[i] > m) m = hred[i];
            if (m || wlo == lo) break;
            whi = wlo; wsz *= 16;
        }
        if (m > best) best = m;
    }
    db_st.t_maxidx += tnow() - t0;
    if (best > n) { ec_fatal(EC_RC_FATAL, "dbig: maxidx returned %zu for %zu limbs (a corrupted length reduction)\n", best, n); }   /* a length is never garbage silently */
    return best;
}
void db_norm(dbig *r) { r->n = maxidx(r, 0, r->n); }
uint64_t db_limb(const dbig *a, size_t i) { uint64_t v; size_t g = a->off + i, d = hq(a, g); mem_dev_copy_on((int)d, &v, a->q[d] + (g - d * a->qc), 8); return v; }
uint64_t db_top(const dbig *a) { return a->n ? db_limb(a, a->n - 1) : 0; }
int db_cmp(const dbig *a, const dbig *b)
{
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    size_t m = maxidx(a, b, a->n); if (!m) return 0;
    uint64_t x = db_limb(a, m - 1), y = db_limb(b, m - 1);
    return x < y ? -1 : 1;
}
void db_set_zero(dbig *r) { r->n = 0; }
void db_set_u64(dbig *r, uint64_t v) { db_reserve(r, 1); mem_dev_copy_on(0, r->q[0], &v, 8); r->n = v ? 1 : 0; }
void db_set_base_pow(dbig *r, size_t k)
{
    db_reserve(r, k + 1);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, k + 1, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(r->q[d], 0, (hi - lo) * 8)); HIP_CHECK(hipDeviceSynchronize()); } }
    uint64_t one = 1; size_t d = hq(r, k); mem_dev_copy_on((int)d, r->q[d] + (k - d * r->qc), &one, 8);
    r->n = k + 1;
}
/* r = (the low m limbs of a) B^k, as an n-limb number: zeros plus a small copy (I3: the division's remainder window) */
__global__ void k_fill0(uint64_t *p, size_t n) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) p[i] = 0; }
void db_set_shifted_low(dbig *r, const dbig *a, size_t m, size_t k, size_t n)
{
    db_reserve(r, n);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, n, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_fill0<<<228 * 8, 256>>>(r->q[d], hi - lo);                  /* (hipMemset runs at ~10 GB/s here) */
        HIP_CHECK(hipStreamSynchronize(0)); } }
    if (m > a->n) m = a->n;
    for (size_t i = 0; i < m && k + i < n; i++) { uint64_t v = db_limb(a, i); size_t g = k + i, d = hq(r, g); mem_dev_copy_on((int)d, r->q[d] + (g - d * r->qc), &v, 8); }
    r->n = n; db_norm(r);
}
/* M3: r = n zero limbs (r->n = n, not normalised: a share to be filled by a scatter) */
void db_zero_fill(dbig *r, size_t n)
{
    db_reserve(r, n ? n : 1);
#pragma omp parallel for num_threads(DB_NQ)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, n, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_fill0<<<228 * 8, 256>>>(r->q[d], hi - lo);
        HIP_CHECK(hipStreamSynchronize(0)); } }
    r->n = n;
}
dbig db_view(const dbig *a, size_t lo, size_t len)
{
    dbig v = *a; v.off = a->off + lo; v.n = len; v.cap = 0; return v;   /* not owning: never db_free it */
}
