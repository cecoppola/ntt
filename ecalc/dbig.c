/* dbig.c - see dbig.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <pthread.h>
#include <hip/hip_runtime.h>
#include "dbig.h"
#include "mem.h"
#include <time.h>
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define CH 4096                                        /* limbs per carry chunk (256 threads x 16) */
static const uint64_t B10 = 1000000000000000000ULL;
struct db_stats db_st;
static double tnow(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int g_par = -1;                                 /* DBIG_SERIAL=1: drive the four quarters from one thread (debug) */
static void db_acct(int ndev, size_t b[][MEM_DEV_NCAT]);
static void par_init(void) { if (g_par < 0) { g_par = !(getenv("DBIG_SERIAL") && atoi(getenv("DBIG_SERIAL"))); mem_acct_register(db_acct); } }

struct dv { const uint64_t *q[DB_NQ]; size_t qc, off, shift; };     /* shift: the operand as a << shift limbs; qc: limbs per quarter (any multiple of DB_ALIGN) */
/* the quarter of global limb g: the number of quarter boundaries at or below g (no division: qc is any size) */
__device__ static inline size_t dq(size_t g, size_t qc) { return (g >= qc) + (g >= 2 * qc) + (g >= 3 * qc); }
__device__ static inline uint64_t dget(const struct dv v, size_t i) { if (i < v.shift) return 0; size_t g = v.off + i - v.shift, d = dq(g, v.qc); return v.q[d][g - d * v.qc]; }
static struct dv view_of(const dbig *a) { struct dv v; for (int d = 0; d < DB_NQ; d++) v.q[d] = a->q[d]; v.qc = a->qc; v.off = a->off; v.shift = 0; return v; }
static inline size_t hq(const dbig *a, size_t g) { return (g >= a->qc) + (g >= 2 * a->qc) + (g >= 3 * a->qc); }   /* host: quarter of global limb g */
#define DB_ALIGN 4096                                   /* limbs: quarters are multiples of the carry chunk */
static void need_owner(const dbig *r, const char *what) { if (r->off || (!r->cap && r->n)) { fprintf(stderr, "dbig: %s into a view\n", what); abort(); } }

/* Phase 14 R1 (DB_POOL_VMM): a host -> device copy by a kernel on the device (pinned host memory read over the fabric) on a non-blocking
 * stream per device, and the wait: hipMemcpyAsync into a VMM range ran at 6 GB/s and blocked the seed thread for the copy's duration */
__global__ void k_copy_h2d(uint64_t *dst, const uint64_t *src, size_t n)
{
    size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, stride = (size_t)gridDim.x * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}
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
__global__ void k_touch(uint64_t *p, size_t n) { size_t step = (1 << 21) / 8; for (size_t i = (size_t)threadIdx.x * step; i < n; i += step * blockDim.x) { uint64_t v = p[i]; if (v == 0x123456789ULL) p[i] = v; } }
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
static pthread_mutex_t g_pool_mx = PTHREAD_MUTEX_INITIALIZER;   /* Phase 8: a background thread may free blocks */
static struct { void *p; int dev; size_t bytes; int own; int kind; } g_donated[256]; static int g_ndonated;   /* whole hipMalloc'd or donated regions (own: freed by db_release_pools; kind: M9 accounting -- 0 donated by a caller, 1 borrowed, 2 the pool's own hipMalloc) */
static size_t g_live_bytes[DB_NQ], g_peak_live[DB_NQ];   /* M9 accounting: bytes handed out per device now, and the peak */
static size_t g_win_peak[DB_NQ];                           /* Phase 14 S1 (E1): the peak since the last db_pool_window_peak(dev, 1) -- one level or one doubling */
static void live_add(uint64_t *p, int d, size_t bytes, int reg) { if (g_nlive < 8192) { g_live[g_nlive].p = p; g_live[g_nlive].dev = d; g_live[g_nlive].bytes = bytes; g_live[g_nlive].reg = reg; g_nlive++; g_live_bytes[d] += bytes; if (g_live_bytes[d] > g_peak_live[d]) g_peak_live[d] = g_live_bytes[d]; if (g_live_bytes[d] > g_win_peak[d]) g_win_peak[d] = g_live_bytes[d]; } else { fprintf(stderr, "dbig: live table full\n"); abort(); } }
static size_t live_take(uint64_t *p, int *reg) { for (int i = 0; i < g_nlive; i++) if (g_live[i].p == p) { size_t b = g_live[i].bytes; *reg = g_live[i].reg; g_live_bytes[g_live[i].dev] -= b; g_live[i] = g_live[--g_nlive]; return b; } return 0; }
static void ext_insert(int d, char *p, size_t bytes, int reg)
{
    struct ext *e = g_ext[d].e; int n = g_ext[d].n, i = 0;
    while (i < n && e[i].p < p) i++;
    int mprev = i > 0 && e[i - 1].reg == reg && e[i - 1].p + e[i - 1].bytes == p, mnext = i < n && e[i].reg == reg && p + bytes == e[i].p;
    if (mprev && mnext) { e[i - 1].bytes += bytes + e[i].bytes; memmove(&e[i], &e[i + 1], (n - i - 1) * sizeof *e); g_ext[d].n--; }
    else if (mprev) e[i - 1].bytes += bytes;
    else if (mnext) { e[i].p = p; e[i].bytes += bytes; }
    else { if (n >= 8192) { fprintf(stderr, "dbig: extent table full\n"); abort(); } memmove(&e[i + 1], &e[i], (n - i) * sizeof *e); e[i].p = p; e[i].bytes = bytes; e[i].reg = reg; g_ext[d].n++; }
}
/* Phase 11 M (PLAN 26, decision 5): a reserved tail per device -- the last `bytes` of a region (the bs arena's end, laid out
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
/* Phase 14 L1 (E5's layout half, DM_TAIL_DEAD): after the bs top level its dead inputs' blocks are free again -- the largest free extent
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
size_t db_pool_tail_bytes(int dev) { return dev >= 0 && dev < DB_NQ ? g_tail[dev].bytes : 0; }
size_t db_pool_tail_stats(int dev, size_t *spills) { if (dev < 0 || dev >= DB_NQ) { if (spills) *spills = 0; return 0; } if (spills) *spills = g_tail[dev].n_spill; return g_tail[dev].n_tail; }
static size_t ext_outside_tail(int d, const struct ext *e)   /* the extent's bytes not in the reserved tail (an extent never straddles the tail's end: it is a region's end) */
{
    if (!g_tail[d].bytes) return e->bytes;
    char *a = e->p, *b = e->p + e->bytes, *ta = g_tail[d].p, *tb = g_tail[d].end;
    if (b <= ta || a >= tb) return e->bytes;
    size_t ov = (size_t)((b < tb ? b : tb) - (a > ta ? a : ta));
    return e->bytes - ov;
}
/* Phase 14 L1 (DM_TIGHT): large requests packed downward from the highest free end -- carved from the back of the highest-ending extent
 * that holds them -- so the big blocks of the top level and the reciprocal (P, Q, t1, r2) stack at the arena's top and the free space
 * below them stays one extent.  With the tail rule alone the top level's P takes the tail's back, nothing ends at the tail's end any
 * more, and the reciprocal's r2 (1.0 n_Q) was best-fit into the middle: at 4e10 it found 10.8 GB free in 3 extents, largest 4.39 (job
 * 21131).  Small requests keep the best fit from the front, outside the tail. */
static int g_pack_large;                              /* 1: bs (the top level's P, Q stay below the tail: t1 finds it whole); 2: the dm phase (the tail first, then the highest end) */
void db_pool_pack_large(int on) { g_pack_large = on; }
static int g_pin_tail;                                /* Phase 14 L1: while set, every request is carved from the back of the tail's free part (r right below t1: the tail is then full, nothing small can spill into it, and it is whole again once both are freed) */
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

/* ---- Phase 14 R1 (E8, results/R114.md 5): the VMM-backed arena, DB_POOL_VMM=1 --------------------------------------------------
 * One VA range per APU (hipMemAddressReserve, DB_POOL_VMM_RESERVE (3) x the arena), physical chunks of DB_POOL_VMM_CHUNK_GB (2) GiB
 * (hipMemCreate, pinned device memory) mapped from the range's start (hipMemMap) with read-write access for the four APUs
 * (hipMemSetAccess).  The pool's extents live over the range as over any region; a request that finds no contiguous extent is
 * satisfied by REMAPPING: the chunks that are wholly free are unmapped and mapped again, contiguously, into the lowest run of
 * empty slots (the same physical chunks: no create, no page work -- 0.002 s/GiB), and only when the free chunks do not cover the
 * request are new chunks created for the remainder (a growth by mapping, counted apart; never a hipMalloc).  The host cannot
 * write a VMM range (a CPU store segfaults): the seed thread's stores go through its buffers and DMA under this switch. */
static int g_vmm_on = -1;
int db_pool_vmm_on(void) { if (g_vmm_on < 0) { const char *e = getenv("DB_POOL_VMM"); g_vmm_on = e ? atoi(e) != 0 : 0; } return g_vmm_on; }
static struct vmm { char *base; size_t reserved, chunk; int nslot, nd, reg; hipMemGenericAllocationHandle_t *h;   /* h[slot]: the chunk mapped there, 0 = empty */
                    size_t n_remap, remap_chunks, n_grow, grow_chunks; double t_remap;
                    int m0, mapped, bg_on; pthread_t bg; size_t bytes; double t_bg; } g_vmm[DB_NQ];   /* m0: the arena's chunks; mapped: how many of them are (the first `mapped` slots), the rest by the background thread */
static pthread_cond_t g_vmm_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_vmm_map_mx = PTHREAD_MUTEX_INITIALIZER;   /* the background mappers one at a time: four at once hold the runtime's lock while blocked on each other in the driver, and the seed thread's launches wait behind them */
static int g_vmm_go;                                   /* the background mapping starts when init's plane pools are allocated (db_vmm_bg_release from rns_init), so that the seeds get their half first and the pools their turn */
void db_vmm_bg_release(void) { pthread_mutex_lock(&g_pool_mx); g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); pthread_mutex_unlock(&g_pool_mx); }
static int vmm_vb(void) { static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0); return vb; }
static int vmm_map_run(int d, int slot0, int m, hipMemGenericAllocationHandle_t *hs)   /* hs[i] (0: create one) mapped at slot0 + i, access for every APU */
{
    struct vmm *v = &g_vmm[d]; hipMemAllocationProp prop; memset(&prop, 0, sizeof prop);
    prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = d;
    for (int i = 0; i < m; i++) {
        if (!hs[i]) { if (hipMemCreate(&hs[i], v->chunk, &prop, 0) != hipSuccess) return 0; v->n_grow++; v->grow_chunks++; }
        if (hipMemMap(v->base + (size_t)(slot0 + i) * v->chunk, v->chunk, 0, hs[i], 0) != hipSuccess) return 0;
        v->h[slot0 + i] = hs[i];
    }
    hipMemAccessDesc ad[DB_NQ]; memset(ad, 0, sizeof ad);
    for (int c = 0; c < v->nd; c++) { ad[c].location.type = hipMemLocationTypeDevice; ad[c].location.id = c; ad[c].flags = hipMemAccessFlagsProtReadWrite; }
    if (hipMemSetAccess(v->base + (size_t)slot0 * v->chunk, (size_t)m * v->chunk, ad, v->nd) != hipSuccess) return 0;
    return 1;
}
static void *vmm_bg_map(void *arg)                       /* the arena's chunks after the first `first` bytes, one at a time, while init and the seeds go on (the driver serialises the page work anyway) */
{
    int dev = (int)(intptr_t)arg; struct vmm *v = &g_vmm[dev];
    pthread_mutex_lock(&g_pool_mx); while (!g_vmm_go) pthread_cond_wait(&g_vmm_cv, &g_pool_mx); pthread_mutex_unlock(&g_pool_mx);
    double t0 = mem_now();
    hipStream_t st; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev)); HIP_CHECK(hipStreamCreateWithFlags(&st, hipStreamNonBlocking));
    for (int k = v->mapped; k < v->m0; k++) {
        hipMemGenericAllocationHandle_t h[1] = { 0 };
        pthread_mutex_lock(&g_vmm_map_mx);
        if (!vmm_map_run(dev, k, 1, h)) mem_oom("db_vmm_arena_alloc (background chunk)", dev, v->chunk);
        HIP_CHECK(hipMemsetAsync(v->base + (size_t)k * v->chunk, 0, v->chunk, st)); HIP_CHECK(hipStreamSynchronize(st));
        pthread_mutex_unlock(&g_vmm_map_mx);
        pthread_mutex_lock(&g_pool_mx); v->mapped = k + 1; pthread_cond_broadcast(&g_vmm_cv); pthread_mutex_unlock(&g_pool_mx);
    }
    HIP_CHECK(hipStreamDestroy(st)); HIP_CHECK(hipSetDevice(cur));
    pthread_mutex_lock(&g_pool_mx);
    if ((size_t)v->m0 * v->chunk > v->bytes) ext_insert(dev, v->base + v->bytes, (size_t)v->m0 * v->chunk - v->bytes, v->reg);   /* the last chunk's remainder: free at once */
    v->n_grow = 0; v->grow_chunks = 0; v->t_bg = mem_now() - t0;
    pthread_mutex_unlock(&g_pool_mx);
    return 0;
}
void db_vmm_arena_wait(int dev, size_t bytes)       /* the arena's first `bytes` are mapped (the parity-1 half before level 1, everything before the halves are donated / released) */
{
    struct vmm *v = &g_vmm[dev]; if (!v->base) return;
    size_t need = bytes < (size_t)v->m0 * v->chunk ? bytes : (size_t)v->m0 * v->chunk; int m = (int)((need + v->chunk - 1) / v->chunk);
    pthread_mutex_lock(&g_pool_mx); if (v->mapped < m && !g_vmm_go) { g_vmm_go = 1; pthread_cond_broadcast(&g_vmm_cv); }   /* (a waiter before rns_init released it: tests, init-only) */
    while (v->mapped < m) pthread_cond_wait(&g_vmm_cv, &g_pool_mx); pthread_mutex_unlock(&g_pool_mx);
    if (m >= v->m0 && v->bg_on) { pthread_join(v->bg, 0); v->bg_on = 0; if (vmm_vb()) printf("dbig pool: APU%d VMM arena: the background thread mapped chunks %d..%d in %.2f s\n", dev, (int)(v->bytes ? 0 : 0), v->m0 - 1, v->t_bg); }
}
void *db_vmm_arena_alloc(int dev, size_t bytes, size_t first)   /* the arena of `bytes` (rounded up to whole chunks: the rest is free pool space) as a VMM range; a borrowed region
                                                                 * record.  The first `first` bytes (the parity-0 half: the seeds' target) are mapped before returning, the rest by a thread
                                                                 * (db_vmm_arena_wait before their first use: Phase 14 R1, the +8 s of init) */
{
    struct vmm *v = &g_vmm[dev]; double t0 = mem_now();
    const char *e = getenv("DB_POOL_VMM_CHUNK_GB"); double cg = e ? atof(e) : 2.0; size_t C = (size_t)(cg * 1073741824.0); if (C < ((size_t)2 << 20)) C = (size_t)2 << 20; C = C / ((size_t)2 << 20) * ((size_t)2 << 20);
    e = getenv("DB_POOL_VMM_RESERVE"); double rf = e ? atof(e) : 3.0; if (rf < 1.0) rf = 1.0;
    int m0 = (int)((bytes + C - 1) / C), nslot = (int)(m0 * rf) + 1; if (nslot < m0 + 1) nslot = m0 + 1;
    int nd; HIP_CHECK(hipGetDeviceCount(&nd)); if (nd > DB_NQ) nd = DB_NQ;
    v->chunk = C; v->nslot = nslot; v->nd = nd; v->reserved = (size_t)nslot * C; v->h = (hipMemGenericAllocationHandle_t *)calloc(nslot, sizeof *v->h);
    hipMemAllocationProp prop; memset(&prop, 0, sizeof prop); prop.type = hipMemAllocationTypePinned; prop.location.type = hipMemLocationTypeDevice; prop.location.id = dev;
    size_t gran = 0; if (hipMemGetAllocationGranularity(&gran, &prop, hipMemAllocationGranularityRecommended) != hipSuccess || !gran) gran = (size_t)2 << 20;
    if (hipMemAddressReserve((void **)&v->base, v->reserved, gran, 0, 0) != hipSuccess) { fprintf(stderr, "db_vmm_arena_alloc: APU %d: cannot reserve %.1f GB of VA\n", dev, v->reserved / 1e9); exit(1); }
    int m1 = (int)((first + C - 1) / C); if (m1 > m0 || first == 0) m1 = m0;
    v->m0 = m0; v->bytes = bytes; v->mapped = 0; v->bg_on = 0;
    if (!vmm_map_run(dev, 0, m1, v->h)) mem_oom("db_vmm_arena_alloc (chunks)", dev, bytes);
    int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev)); HIP_CHECK(hipMemset(v->base, 0, (size_t)m1 * C)); HIP_CHECK(hipDeviceSynchronize()); HIP_CHECK(hipSetDevice(cur));
    pthread_mutex_lock(&g_pool_mx);
    if (g_ndonated >= 256) { fprintf(stderr, "db_vmm_arena_alloc: too many regions\n"); abort(); }
    v->reg = g_ndonated; g_donated[g_ndonated].p = v->base; g_donated[g_ndonated].dev = dev; g_donated[g_ndonated].bytes = (size_t)m0 * C; g_donated[g_ndonated].own = 0; g_donated[g_ndonated].kind = 1; g_ndonated++;
    v->mapped = m1;
    pthread_mutex_unlock(&g_pool_mx);
    mem_acct_register(db_acct);
    if (vmm_vb() || getenv("RNS_VERBOSE")) printf("dbig pool: APU%d VMM arena %.2f GB = %d chunks of %.2f GiB, the first %d mapped in %.2f s (%.3f s/GB)%s, VA %.1f GB reserved at %p\n", dev, (double)m0 * C / 1e9, m0, C / 1073741824.0, m1, mem_now() - t0, (mem_now() - t0) / ((double)m1 * C / 1e9), m1 < m0 ? ", the rest in the background" : "", v->reserved / 1e9, (void *)v->base);
    if (m1 < m0) { v->bg_on = 1; if (pthread_create(&v->bg, 0, vmm_bg_map, (void *)(intptr_t)dev)) { fprintf(stderr, "db_vmm_arena_alloc: pthread_create\n"); abort(); } }
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
    fprintf(stderr, "dbig: ext_remove: range not free\n"); abort();
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
    for (int i = 0; i < nf; i++) { int k = fr[i]; ext_remove(d, v->base + (size_t)k * C, C); if (hipMemUnmap(v->base + (size_t)k * C, C) != hipSuccess) { fprintf(stderr, "dbig: hipMemUnmap failed\n"); abort(); } hs[i] = v->h[k]; v->h[k] = 0; }
    int slot0 = -1;                                               /* the lowest run of m empty slots */
    for (int k = 0, run = 0; k < v->nslot; k++) { run = v->h[k] ? 0 : run + 1; if (run == m) { slot0 = k - m + 1; break; } }
    if (slot0 < 0) {                                              /* no room in the VA: put the free chunks back where they were */
        for (int i = 0; i < nf; i++) { int k = fr[i]; hipMemGenericAllocationHandle_t one[1] = { hs[i] }; if (!vmm_map_run(d, k, 1, one)) { fprintf(stderr, "dbig: VMM restore failed\n"); abort(); } ext_insert(d, v->base + (size_t)k * C, C, v->reg); }
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
size_t db_pool_vmm_stats(int dev, size_t *remaps, size_t *grow_chunks) { struct vmm *v = &g_vmm[dev]; if (remaps) *remaps = v->n_remap; if (grow_chunks) *grow_chunks = v->grow_chunks; return v->base ? v->remap_chunks : 0; }
static uint64_t *q_alloc_locked(int d, size_t need);
static uint64_t *q_alloc(int d, size_t need) { pthread_mutex_lock(&g_pool_mx); uint64_t *p = q_alloc_locked(d, need); pthread_mutex_unlock(&g_pool_mx); return p; }
static uint64_t *q_alloc_locked(int d, size_t need)                 /* need: bytes, a multiple of DB_ALIGN limbs */
{
    int reg;
    char *p = ext_take(d, need, &reg);
    if (!p && vmm_make_room(d, need)) p = ext_take(d, need, &reg);   /* Phase 14 R1 (E8): the VMM arena re-stitches its free chunks */
    if (!p) {
        static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0);
        if (vb) { size_t fr = 0, lg = 0; for (int i = 0; i < g_ext[d].n; i++) { fr += g_ext[d].e[i].bytes; if (g_ext[d].e[i].bytes > lg) lg = g_ext[d].e[i].bytes; }
                  printf("dbig pool: APU%d hipMalloc %.2f GB inside the phase (free %.2f GB in %d extents, largest %.2f; live %.2f GB in %d blocks)\n", d, need / 1e9, fr / 1e9, g_ext[d].n, lg / 1e9, g_live_bytes[d] / 1e9, g_nlive); }   /* Phase 10 B4 (agent M): why the pre-sized pool still grows */
        g_pool_bytes += need;
        int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d));
        void *m; if (hipMalloc(&m, need) != hipSuccess) { pthread_mutex_unlock(&g_pool_mx); mem_oom("dbig block pool (inside a phase)", d, need); } HIP_CHECK(hipMemset(m, 0, need)); HIP_CHECK(hipDeviceSynchronize());
        HIP_CHECK(hipSetDevice(cur));
        if (g_ndonated < 256) { reg = g_ndonated; g_donated[g_ndonated].p = m; g_donated[g_ndonated].dev = d; g_donated[g_ndonated].bytes = need; g_donated[g_ndonated].own = 1; g_donated[g_ndonated].kind = 2; g_ndonated++; } else { fprintf(stderr, "dbig: region table full\n"); abort(); }
        p = (char *)m;
    }
    live_add((uint64_t *)p, d, need, reg); return (uint64_t *)p;
}
static void q_release(int d, uint64_t *p) { int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipFree(p)); HIP_CHECK(hipSetDevice(cur)); }
static void q_free(int d, uint64_t *p)
{
    pthread_mutex_lock(&g_pool_mx);
    int reg; size_t bytes = live_take(p, &reg); if (!bytes) { fprintf(stderr, "dbig: freeing an unknown block\n"); abort(); }
    ext_insert(d, (char *)p, bytes, reg);
    pthread_mutex_unlock(&g_pool_mx);
}
void db_donate(int dev, void *p, size_t bytes) { db_donate_ext(dev, p, bytes, 1); }
/* Phase 14 S1 (E1, E12): the pool's live bytes on a device now (read without the lock by the sampler thread: a size_t, one
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
/* grow the block pool of device dev by one region of `bytes` now (Phase 8: from a background thread while the GPUs run
 * the levels -- hipMalloc is 0.057 s/GB of CPU-side driver work, hidden there instead of inside the Newton loop) */
void db_pregrow(int dev, size_t bytes)
{
    void *m; int cur; HIP_CHECK(hipGetDevice(&cur)); HIP_CHECK(hipSetDevice(dev));
    if (hipMalloc(&m, bytes) != hipSuccess) mem_oom("db_pregrow", dev, bytes); HIP_CHECK(hipMemset(m, 0, bytes)); HIP_CHECK(hipStreamSynchronize(0));
    HIP_CHECK(hipSetDevice(cur));
    pthread_mutex_lock(&g_pool_mx); g_pool_bytes += bytes; pthread_mutex_unlock(&g_pool_mx);
    db_donate_ext(dev, m, bytes, 1);
    g_donated[g_ndonated - 1].kind = 2;                   /* M9: the pool's own hipMalloc */
}
void db_donate_ext(int dev, void *p, size_t bytes, int own)
{
    pthread_mutex_lock(&g_pool_mx);
    if (g_ndonated >= 256) { fprintf(stderr, "db_donate: too many regions\n"); abort(); }
    g_donated[g_ndonated].p = p; g_donated[g_ndonated].dev = dev; g_donated[g_ndonated].bytes = bytes; g_donated[g_ndonated].own = own; g_donated[g_ndonated].kind = own ? 0 : 1; g_ndonated++;
    ext_insert(dev, (char *)p, bytes, g_ndonated - 1);
    pthread_mutex_unlock(&g_pool_mx);
    mem_acct_register(db_acct);
}
/* Phase 9 C4: a borrowed range that is part of the same allocation as an already borrowed region next to it (the
 * two parities of a bs region arena, donated at different moments): joined into that region's record, so that
 * their extents coalesce (extents never merge across regions, a block must stay inside one allocation).  With no
 * neighbour it is an ordinary borrowed region. */
void db_donate_adjacent(int dev, void *p, size_t bytes)
{
    pthread_mutex_lock(&g_pool_mx);
    { int reg; if (vmm_region_has(dev, (const char *)p, bytes, &reg)) {                                   /* Phase 14 R1 (E8): a range of the VMM arena joins its record (mapped first) */
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
        if (g_vmm[d].base && i == g_vmm[d].reg) { bytes = 0; for (int k = 0; k < g_vmm[d].nslot; k++) if (g_vmm[d].h[k]) bytes += g_vmm[d].chunk; }   /* Phase 14 R1 (E8): the chunks mapped now */
        b[d][g_donated[i].kind == 2 ? MEM_DEV_POOL_HIPMALLOC : g_donated[i].kind == 1 ? MEM_DEV_POOL_BORROWED : MEM_DEV_POOL_DONATED] += bytes; }
    for (int d = 0; d < DB_NQ && d < ndev; d++) { b[d][MEM_DEV_POOL_LIVE] += g_live_bytes[d]; b[d][MEM_DEV_POOL_PEAK_LIVE] += g_peak_live[d]; b[d][MEM_DEV_POOL_FREE] += db_pool_free_bytes(d); }
    pthread_mutex_unlock(&g_pool_mx);
}
size_t db_pool_free_bytes(int d) { size_t s = 0; for (int i = 0; i < g_ext[d].n; i++) s += g_ext[d].e[i].bytes; return s; }
int db_pool_extents(int d) { return g_ext[d].n; }
size_t db_pool_largest_free(int d) { size_t m = 0; pthread_mutex_lock(&g_pool_mx); for (int i = 0; i < g_ext[d].n; i++) if (g_ext[d].e[i].bytes > m) m = g_ext[d].e[i].bytes; pthread_mutex_unlock(&g_pool_mx); return m; }
size_t db_pool_hipmalloc_bytes(int d) { size_t s = 0; pthread_mutex_lock(&g_pool_mx); for (int i = 0; i < g_ndonated; i++) if (g_donated[i].dev == d && g_donated[i].kind == 2) s += g_donated[i].bytes; pthread_mutex_unlock(&g_pool_mx); return s; }
void db_release_pools(void)
{
    { static int vb = -1; if (vb < 0) vb = getenv("DB_POOL_VERBOSE") ? atoi(getenv("DB_POOL_VERBOSE")) : (getenv("RNS_VERBOSE") ? 1 : 0);   /* Phase 11 M: how the reserved tails were used */
      if (vb) for (int d = 0; d < DB_NQ; d++) if (g_tail[d].bytes) printf("dbig pool: APU%d tail %.2f GB: %zu large blocks from its back, %zu small blocks spilled into it\n", d, g_tail[d].bytes / 1e9, g_tail[d].n_tail, g_tail[d].n_spill); }
    for (int d = 0; d < DB_NQ; d++) g_ext[d].n = 0;              /* every extent is a piece of a whole region */
    for (int i = 0; i < g_ndonated; i++) if (g_donated[i].own) q_release(g_donated[i].dev, (uint64_t *)g_donated[i].p);
    g_ndonated = 0; g_nlive = 0; g_pool_bytes = 0;
    memset(g_live_bytes, 0, sizeof g_live_bytes); memset(g_win_peak, 0, sizeof g_win_peak); memset(g_tail, 0, sizeof g_tail);   /* Phase 11 M: the reserved tails go with their regions */
}
size_t db_pool_bytes(void) { return g_pool_bytes; }
void db_init(dbig *x) { par_init(); memset(x, 0, sizeof *x); }
void db_free(dbig *x) { if (x->cap) for (int d = 0; d < DB_NQ; d++) if (x->q[d]) q_free(d, x->q[d]); memset(x, 0, sizeof *x); }
void db_reserve(dbig *x, size_t limbs)
{
    if (limbs <= x->cap) return;
    double t0 = tnow(); db_st.n_reserve++;
    if (x->off) { fprintf(stderr, "db_reserve: a view\n"); abort(); }
    /* quarters of exactly ceil(limbs/4) rounded up to DB_ALIGN: no size classes (a 2^l / 3 2^l class wasted up to
     * 45 % of the dm phase's device memory at 4e10, RESULTS.md 70); the pool coalesces any sizes */
    size_t need = (limbs + DB_NQ - 1) / DB_NQ, qc = (need + DB_ALIGN - 1) / DB_ALIGN * DB_ALIGN;
    dbig y; db_init(&y); y.cap = qc * DB_NQ; y.qc = qc;
    for (int d = 0; d < DB_NQ; d++) y.q[d] = q_alloc(d, qc * 8);
    if (getenv("DBIG_WARM")) {                             /* touch every 2 MiB page of each quarter from every other device */
        for (int d = 0; d < DB_NQ; d++) for (int c = 0; c < DB_NQ; c++) if (c != d) {
            HIP_CHECK(hipSetDevice(c));
            k_touch<<<1, 256>>>(y.q[d], qc);
            HIP_CHECK(hipStreamSynchronize(0));
        }
        HIP_CHECK(hipSetDevice(0));
    }
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
 * stream below must not be waited for by every kernel of the Newton loop -- Phase 8, RESULTS.md 68) */
/* host <-> device copies through a pinned bounce buffer per device (1 GiB): hipMemcpy with pageable host
 * memory runs at ~3 GB/s, pinned at ~50 GB/s; the host side is a parallel memcpy.  Quarters one after
 * another (concurrent hipMemcpy with pageable memory faults, RESULTS.md 59), chunks pipelined by two. */
#define BOUNCE ((size_t)1 << 27)                              /* limbs: 1 GiB */
static uint64_t *g_bounce[DB_NQ][2]; static hipStream_t g_bs[DB_NQ];
static pthread_mutex_t g_bounce_mx = PTHREAD_MUTEX_INITIALIZER;   /* Phase 8: copies may be issued from a background thread; one at a time per process */
static void bounce_init(int d)
{
    if (g_bounce[d][0]) return;
    HIP_CHECK(hipSetDevice(d));
    HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][0], BOUNCE * 8, 0)); HIP_CHECK(hipHostMalloc((void **)&g_bounce[d][1], BOUNCE * 8, 0));
    HIP_CHECK(hipStreamCreateWithFlags(&g_bs[d], hipStreamNonBlocking));   /* Phase 8: copies from a background thread must not serialise with the kernels on the null stream */
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

/* ---- residue mod q of a device number (Phase 8 I3: the T1 residues of P, Q, R without host copies) ----
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
static uint64_t powmod_h(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1; b %= q; while (e) { if (e & 1) r = (uint64_t)((unsigned __int128)r * b % q); b = (uint64_t)((unsigned __int128)b * b % q); e >>= 1; } return r; }
static uint64_t *g_mq_out[DB_NQ], *g_mq_pow[DB_NQ]; static size_t g_mq_cap[DB_NQ];
static void qrange(const dbig *x, int d, size_t n, size_t *lo, size_t *hi);
#define MQ_MAXQ 16
/* residues of x modulo nq primes (each < 2^63) at once: the quarters in parallel, one launch per quarter */
static pthread_mutex_t g_mq_mx = PTHREAD_MUTEX_INITIALIZER;   /* Phase 10 H (B1): the output stage's background thread takes X's residues while the division goes on (R's) -- the scratch above is shared */
static void db_mod_qs_locked(const dbig *x, const uint64_t *qs, int nq, uint64_t *res);
/* Phase 11 V (D5): ECALC_RES_LOG=1 -- every residue the kernel computes is cross-checked against the host Horner over a
 * copy of the number (vf_limbs_mod) and both are printed, so a wrong residue names its side (RES lines, one per call) */
#include "verify.h"
int db_res_log = -1;
int db_res_log_on(void) { if (db_res_log < 0) db_res_log = getenv("ECALC_RES_LOG") ? atoi(getenv("ECALC_RES_LOG")) : 0; return db_res_log; }
static void db_mod_qs_check(const dbig *x, const uint64_t *qs, int nq, const uint64_t *res)
{
    bigint h; bi_init(&h); bi_reserve(&h, x->n); h.n = x->n; int bad = 0; char line[1024]; int k = 0, dev0; HIP_CHECK(hipGetDevice(&dev0));
    for (int d = 0; d < DB_NQ; d++) {                          /* (db_to_bi ignores a view's off: copy the limbs [off, off + n) quarter by quarter) */
        size_t g0 = (size_t)d * x->qc, g1 = g0 + x->qc, s0 = x->off > g0 ? x->off : g0, s1 = x->off + x->n < g1 ? x->off + x->n : g1;
        if (s0 >= s1) continue;
        HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemcpy(h.l + (s0 - x->off), x->q[d] + (s0 - g0), (s1 - s0) * 8, hipMemcpyDeviceToHost));
    }
    HIP_CHECK(hipSetDevice(dev0));
    k += snprintf(line + k, sizeof line - k, "RES db_mod_qs n=%zu off=%zu qc=%zu:", x->n, x->off, x->qc);
    for (int j = 0; j < nq && k < 900; j++) { uint64_t hv = vf_limbs_mod(h.l, h.n, qs[j]); if (hv != res[j]) bad++; k += snprintf(line + k, sizeof line - k, " %llu%s", (unsigned long long)res[j], hv == res[j] ? "" : "!=host"); }
    printf("%s%s\n", line, bad ? "  MISMATCH kernel vs host" : "  (host Horner agrees)");
    bi_free(&h);
}
void db_mod_qs(const dbig *x, const uint64_t *qs, int nq, uint64_t *res)
{
    if (nq > MQ_MAXQ) { fprintf(stderr, "db_mod_qs: %d primes\n", nq); abort(); }
    for (int j = 0; j < nq; j++) res[j] = 0;
    if (!x->n) return;
    pthread_mutex_lock(&g_mq_mx); db_mod_qs_locked(x, qs, nq, res); pthread_mutex_unlock(&g_mq_mx);
    if (db_res_log_on()) db_mod_qs_check(x, qs, nq, res);
}
static void db_mod_qs_locked(const dbig *x, const uint64_t *qs, int nq, uint64_t *res)
{
    uint64_t Bq[MQ_MAXQ], Bt[MQ_MAXQ], Bch[MQ_MAXQ], powt[MQ_MAXQ * MQ_T];
    for (int j = 0; j < nq; j++) {
        uint64_t q = qs[j]; Bq[j] = bi_decimal ? BI_B10 % q : (uint64_t)(((unsigned __int128)1 << 64) % q); Bt[j] = powmod_h(Bq[j], MQ_T, q); Bch[j] = powmod_h(Bq[j], MQ_CH, q);
        powt[j * MQ_T] = 1; for (int t = 1; t < MQ_T; t++) powt[j * MQ_T + t] = (uint64_t)((unsigned __int128)powt[j * MQ_T + t - 1] * Bq[j] % q);
    }
    uint64_t vq[DB_NQ][MQ_MAXQ]; size_t qlen[DB_NQ]; memset(vq, 0, sizeof vq); memset(qlen, 0, sizeof qlen);
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(x, d, x->n, &lo, &hi); if (lo >= hi) continue;
        size_t first = x->off + lo - (size_t)d * x->qc, len = hi - lo, nc = (len + MQ_CH - 1) / MQ_CH; qlen[d] = len;
        HIP_CHECK(hipSetDevice(d));
        if (g_mq_cap[d] < nc * MQ_MAXQ) { if (g_mq_out[d]) HIP_CHECK(hipFree(g_mq_out[d])); HIP_CHECK(hipMalloc(&g_mq_out[d], (nc * MQ_MAXQ + 1024) * 8)); g_mq_cap[d] = nc * MQ_MAXQ + 1024; }
        if (!g_mq_pow[d]) HIP_CHECK(hipMalloc(&g_mq_pow[d], (MQ_MAXQ * MQ_T + 2 * MQ_MAXQ) * 8));
        uint64_t *dq = g_mq_pow[d] + MQ_MAXQ * MQ_T, *dbt = dq + MQ_MAXQ;
        HIP_CHECK(hipMemcpy(g_mq_pow[d], powt, (size_t)nq * MQ_T * 8, hipMemcpyHostToDevice));
        HIP_CHECK(hipMemcpy(dq, qs, (size_t)nq * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dbt, Bt, (size_t)nq * 8, hipMemcpyHostToDevice));
#pragma omp critical
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
    for (; i < hi; i += stride) { long s = (long)i + shift; out[i - lo] = (s >= 0 && (size_t)s < an) ? dget(a, (size_t)s) : 0; }
}
/* add/sub of one chunk (CH = 256 threads x SEG limbs) with a block-level carry scan: thread t sums its
 * SEG limbs with carry-in 0 (generate g, propagate p over the segment), a scan over the 256 (g, p) gives
 * every segment's carry-in, the segment is redone with it.  r may be a or b (same layout: a thread only
 * touches its own limbs).  cout/prop per chunk for the host scan across chunks and quarters.
 * The b operand is either a dbig or a sparse set of 4-limb spills (sp != 0): spill j sits at limb
 * R j + row0 + rows for row0 in {0, rows, 2 rows, 3 rows} (four ranks' spill arrays). */
struct sparse { const uint64_t *sp[4]; size_t R, rows, C; int single; size_t pos; uint64_t val;
                size_t lo; int gt; const size_t *tab[4]; };   /* tab[d] (Phase 12 G, agent G: the exact spill exchange): sp[d] holds only the blocks that meet this share, per source node r the columns [tab[3r], tab[3r+1]) at block offset tab[3r+2]; 0 = the full [r][j][4] layout */   /* single: one limb val at pos.  gt > 0 (M3): the node's share [lo, ..) of a number whose product ran on 4 gt ranks,
                                         * rank rho = gt d + r: sp[d] holds [r][j][4], spill (rho, j) at global limb R j + (rho + 1) rows -- or, when rows nr != R
                                         * (Phase 11 L: gt = g nodes of any count, rows = floor(R / nr)), at R j + R (rho + 1) / nr */
__device__ static inline uint64_t sparse_get(const struct sparse s, size_t i)
{
    if (s.single) return i == s.pos ? s.val : 0;
    if (s.gt) {
        size_t m = i + s.lo, j = m / s.R, rem = m - j * s.R, q, t; int nr = 4 * s.gt, rho;
        if (s.rows * (size_t)nr == s.R) { q = rem / s.rows; t = rem - q * s.rows; }
        else { q = ((rem + 1) * (size_t)nr + s.R - 1) / s.R - 1; t = rem - s.R * q / nr; }   /* Phase 11 L (agent L, minimal): unequal parts -- rank rho holds rows [R rho / nr, R (rho+1) / nr); q = the rank whose part starts at or below rem, i.e. rho + 1 of the spill's rank */
        if (t >= 4) return 0; if (q == 0) { if (j < 1 || j - 1 >= s.C) return 0; rho = nr - 1; j--; } else { rho = (int)q - 1; if (j >= s.C) return 0; }
        int d = rho / s.gt, r = rho - d * s.gt;
        if (s.tab[d]) { const size_t *e = s.tab[d] + 3 * (size_t)r; if (j < e[0] || j >= e[1]) return 0; return s.sp[d][(e[2] + (j - e[0])) * 4 + t]; }   /* Phase 12 G: the compact form */
        return s.sp[d][((size_t)r * s.C + j) * 4 + t];
    }
    /* i = R j + (r+1) rows + t, t < 4: j = i / R, rem = i - R j; r+1 = rem / rows if rem % rows < 4 */
    size_t j = i / s.R, rem = i - j * s.R, q = rem / s.rows, t = rem - q * s.rows;
    if (t >= 4) return 0;
    if (q == 0) return (j >= 1 && j - 1 < s.C) ? s.sp[3][(j - 1) * 4 + t] : 0;   /* rank 3's spill of column j-1 lands at R j */
    return j < s.C ? s.sp[q - 1][j * 4 + t] : 0;
}
#define SEG 16
__global__ void k_addsub(uint64_t *out, size_t lo, size_t hi, struct dv a, size_t an, struct dv b, size_t bn, struct sparse sp, int has_sp, int sub, int dec, uint8_t *cout, uint8_t *prop)
{
    __shared__ uint8_t G[256], P[256];
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi, s0 = c0 + (size_t)threadIdx.x * SEG, s1 = s0 + SEG < c1 ? s0 + SEG : c1;
    uint64_t x[SEG], y[SEG]; int g = 0, p = 1;
    for (int k = 0; k < SEG; k++) {
        size_t i = s0 + k;
        x[k] = (i < s1 && i < an) ? dget(a, i) : 0;
        y[k] = (i < s1) ? (has_sp ? (i < bn ? sparse_get(sp, i) : 0) : (i < bn ? dget(b, i) : 0)) : 0;
    }
    {   /* pass 1: carry-in 0 -> generate / propagate of the segment */
        uint64_t cy = 0; int pr = 1;
        for (int k = 0; k < SEG; k++) { if (s0 + k >= s1) break;
            uint64_t xx = x[k], yy = y[k], s;
            if (dec) { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; pr &= (xx == yy); } else { s = xx + yy + cy; cy = s >= B10; pr &= (xx + yy == B10 - 1); } }
            else { if (sub) { s = xx - yy - cy; cy = (xx < yy) || (xx == yy && cy); pr &= (xx == yy); } else { s = xx + yy + cy; cy = (s < xx) || (cy && s == xx); pr &= (xx + yy == ~0ULL); } }
        }
        g = (int)cy; p = (s0 < s1) ? pr : 1;
    }
    G[threadIdx.x] = (uint8_t)g; P[threadIdx.x] = (uint8_t)p;
    __syncthreads();
    if (threadIdx.x == 0) {                                   /* serial scan over 256 segments (cheap) */
        uint8_t cy = 0;
        for (int t = 0; t < 256; t++) { uint8_t gg = G[t], pp = P[t]; G[t] = cy; cy = gg | (pp & cy); }
        cout[blockIdx.x] = cy; prop[blockIdx.x] = 1;
        for (int t = 0; t < 256; t++) if (!P[t]) { prop[blockIdx.x] = 0; break; }
    }
    __syncthreads();
    {   /* pass 2: with the segment's carry-in */
        uint64_t cy = G[threadIdx.x];
        for (int k = 0; k < SEG; k++) { size_t i = s0 + k; if (i >= s1) break;
            uint64_t xx = x[k], yy = y[k], s;
            if (dec) { if (sub) { s = xx + B10 - yy - cy; cy = s < B10; s = cy ? s : s - B10; } else { s = xx + yy + cy; cy = s >= B10; s = cy ? s - B10 : s; } }
            else { if (sub) { s = xx - yy - cy; cy = (xx < yy) || (xx == yy && cy); } else { s = xx + yy + cy; cy = (s < xx) || (cy && s == xx); } }
            out[i - lo] = s;
        }
    }
}
/* apply a carry-in of 1 (or borrow) to a chunk, rippling until absorbed */
__global__ void k_carry(uint64_t *out, size_t lo, size_t hi, const uint8_t *cin, int sub, int dec)
{
    size_t c0 = lo + (size_t)blockIdx.x * CH; if (c0 >= hi || !cin[blockIdx.x]) return;
    size_t c1 = c0 + CH < hi ? c0 + CH : hi;
    if (threadIdx.x) return;
    uint64_t *o = out + (c0 - lo);
    for (size_t i = c0; i < c1; i++) {
        uint64_t v = o[i - c0];
        if (dec) { if (sub) { if (v) { o[i - c0] = v - 1; return; } o[i - c0] = B10 - 1; } else { if (v + 1 < B10) { o[i - c0] = v + 1; return; } o[i - c0] = 0; } }
        else { if (sub) { o[i - c0] = v - 1; if (v) return; } else { o[i - c0] = v + 1; if (v != ~0ULL) return; } }
    }
}
__global__ void k_maxidx(struct dv a, struct dv b, int hasb, size_t lo, size_t hi, size_t *res)   /* 1 + highest i in [lo,hi) with a[i] != b[i] (or != 0), per block */
{
    __shared__ size_t sm[256];
    size_t best = 0; int found = 0;
    for (size_t i = lo + (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < hi; i += (size_t)gridDim.x * blockDim.x)
        if (hasb ? dget(a, i) != dget(b, i) : dget(a, i) != 0) { if (!found || i > best) best = i; found = 1; }
    sm[threadIdx.x] = found ? best + 1 : 0;
    __syncthreads();
    for (int s = 128; s > 0; s >>= 1) { if (threadIdx.x < s && sm[threadIdx.x + s] > sm[threadIdx.x]) sm[threadIdx.x] = sm[threadIdx.x + s]; __syncthreads(); }
    if (threadIdx.x == 0) res[blockIdx.x] = sm[0];
}

static unsigned nblk(size_t total) { size_t b = (total + 255) / 256; return (unsigned)(b > 228 * 8 ? 228 * 8 : b); }
static uint8_t *g_flags[DB_NQ][2]; static size_t g_flags_cap[DB_NQ]; static size_t *g_red[DB_NQ];   /* per quarter, touched only by the thread driving quarter d (Phase 14 A1: the shared host reduction buffer g_hred is gone -- its first-use allocation raced between the four threads) */
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
    if (r == a) { fprintf(stderr, "db shift: in place\n"); abort(); }
    need_owner(r, "shift"); db_reserve(r, n ? n : 1);
    struct dv v = view_of(a);
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
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
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        qrange(out, d, n, &lo[d], &hi[d]); chunks[d] = (hi[d] - lo[d] + CH - 1) / CH;
        if (!chunks[d]) continue;
        flags_reserve(d, chunks[d]);
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_addsub<<<(unsigned)chunks[d], 256>>>(out->q[d], lo[d], hi[d], va, an, vb, bn, sp, spx != 0, sub, bi_decimal, g_flags[d][0], g_flags[d][1]);
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
    if (cy && !sub) { fprintf(stderr, "db_add: carry out of the top (n undersized)\n"); abort(); }
    if (cy && sub) { fprintf(stderr, "db_sub: a < b\n"); abort(); }
    }
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        if (!chunks[d]) continue;
        HIP_CHECK(hipSetDevice(d));
#pragma omp critical
        k_carry<<<(unsigned)chunks[d], 1>>>(out->q[d], lo[d], hi[d], g_flags[d][0], sub, bi_decimal);
        HIP_CHECK(hipStreamSynchronize(0));
    }
    out->n = n; if (!nfix) db_norm(out);
    if (inplace && !same) { dbig sw = *r; *r = tmp; tmp = sw; db_free(&tmp); }
    db_st.t_addsub += tnow() - t0;
}
/* M3: r (n limbs, a node's share, in place) += the spills of a product over 4 gt ranks, sp[d] = [r][j][4] on APU d;
 * fixed length, carry out and propagate reported */
void db_share_add_spills(dbig *r, size_t n, size_t lo, const uint64_t *const sp[4], size_t R, size_t rows, size_t C, int gt, int *cout, int *prop)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) s.sp[q] = sp[q]; s.R = R; s.rows = rows; s.C = C; s.lo = lo; s.gt = gt;
    addsub_core2(r, r, 0, 0, &s, n, 0, n, cout, prop);
}
/* Phase 12 G (agent G, minimal): the same with the compact spill buffers of the exact exchange (rns_dist.c mn_core): sp[d] holds, per
 * source node r in order, the 4-limb blocks of the columns [tab[d][3r], tab[d][3r+1]) at block offset tab[d][3r+2] (device tables) */
void db_share_add_spills_x(dbig *r, size_t n, size_t lo, const uint64_t *const sp[4], const size_t *const tab[4], size_t R, size_t rows, size_t C, int gt, int *cout, int *prop)
{
    struct sparse s; memset(&s, 0, sizeof s); for (int q = 0; q < 4; q++) { s.sp[q] = sp[q]; s.tab[q] = tab[q]; } s.R = R; s.rows = rows; s.C = C; s.lo = lo; s.gt = gt;
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
/* Phase 9 A3 (the grid over shares, rns_dist.c): r (n limbs, in place) += a << k; fixed length, carry out and propagate reported */
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
    if (r == a) { fprintf(stderr, "db_pow_sub: in place\n"); abort(); }
    need_owner(r, "pow_sub"); db_reserve(r, e + 1);
    struct dv va = view_of(a); uint64_t top = bi_decimal ? B10 - 1 : ~0ULL;
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
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
#pragma omp parallel for num_threads(DB_NQ) reduction(max:best) if(g_par)
    for (int d = 0; d < DB_NQ; d++) {
        size_t lo, hi; qrange(a, d, n, &lo, &hi); if (lo >= hi) continue;
        flags_reserve(d, 1);
        HIP_CHECK(hipSetDevice(d));
        unsigned blocks = nblk(hi - lo);
#pragma omp critical
        k_maxidx<<<blocks, 256>>>(va, vb, b != 0, lo, hi, g_red[d]);
        /* Phase 14 A1 (the intermittent garbage leaf length, results/A114.md): the per-block results come to this thread's own
         * buffer.  They went to a process-wide malloc'd g_hred whose first-use allocation (`if (!g_hred) g_hred = malloc()`)
         * ran unguarded inside this four-thread region: two threads could allocate, and a thread whose hipMemcpy landed in
         * one buffer read the other, uninitialised one.  The first db_norm of a process is the leaf hand-over's db_copy at
         * size > 1 (binsplit.c), so it returned heap garbage as P's length once (mn e8 size 4, job 21222: 18385101070989787659) */
        size_t hred[228 * 8];
        HIP_CHECK(hipMemcpy(hred, g_red[d], blocks * 8, hipMemcpyDeviceToHost));
        size_t m = 0; for (unsigned i = 0; i < blocks; i++) if (hred[i] > m) m = hred[i];
        if (m > best) best = m;
    }
    db_st.t_maxidx += tnow() - t0;
    if (best > n) { fprintf(stderr, "dbig: maxidx returned %zu for %zu limbs (a corrupted length reduction)\n", best, n); abort(); }   /* Phase 14 A1: a length is never garbage silently */
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
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
    for (int d = 0; d < DB_NQ; d++) { size_t lo, hi; qrange(r, d, k + 1, &lo, &hi); if (lo < hi) { HIP_CHECK(hipSetDevice(d)); HIP_CHECK(hipMemset(r->q[d], 0, (hi - lo) * 8)); HIP_CHECK(hipDeviceSynchronize()); } }
    uint64_t one = 1; size_t d = hq(r, k); mem_dev_copy_on((int)d, r->q[d] + (k - d * r->qc), &one, 8);
    r->n = k + 1;
}
/* r = (the low m limbs of a) B^k, as an n-limb number: zeros plus a small copy (I3: the division's remainder window) */
__global__ void k_fill0(uint64_t *p, size_t n) { size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x, st = (size_t)gridDim.x * blockDim.x; for (; i < n; i += st) p[i] = 0; }
void db_set_shifted_low(dbig *r, const dbig *a, size_t m, size_t k, size_t n)
{
    db_reserve(r, n);
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
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
#pragma omp parallel for num_threads(DB_NQ) if(g_par)
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
