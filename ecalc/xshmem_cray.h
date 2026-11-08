/* xshmem_cray.h -- the Cray SHMEM backend of xshmem.h (static inline, one forwarding line per call), included when XSHMEM_CRAY is defined
 * (make XSHMEM=cray).  Compile-time selectors (-D via XSHMEM_CRAY_DEFS; the default is what works on the 11.8 headers):
 *   XSHMEM_CRAY_HEAP     0 host heap shmem_malloc (default; MI300A host memory is the same HBM)
 *                        1 shmem_space_malloc(SHMEM_SPACE_DEVICE, n)  2 shmem_malloc_with_hints(n, XSHMEM_CRAY_HINT)  3 shmemg_malloc(n)   [12.1 names, UNVERIFIED]
 *   XSHMEM_CRAY_WAIT     0 shmem_signal_wait_until (default)  1 shmem_uint64_wait_until
 *   XSHMEM_CRAY_HOSTREG  1 (default): HEAP=0 registers the DATA heap with hipHostRegister (needs -DUSE_HIP); 0 = no */
#ifndef XSHMEM_CRAY_H
#define XSHMEM_CRAY_H

#ifndef XSHMEM_H
#error "include xshmem.h, not xshmem_cray.h"
#endif

#include <shmem.h>
#include <shmemx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include "pe_geo.h"

#ifndef XSHMEM_CRAY_HEAP
#define XSHMEM_CRAY_HEAP 0
#endif
#ifndef XSHMEM_CRAY_WAIT
#define XSHMEM_CRAY_WAIT 0
#endif

#if XSHMEM_CRAY_HEAP == 1 && !defined(SHMEM_SPACE_DEVICE)
#error "xshmem_cray.h: XSHMEM_CRAY_HEAP=1 needs shmem_space_malloc / SHMEM_SPACE_DEVICE from the 12.1 headers; not in 11.8 (check T3, design 8.2); use HEAP=0 on aac7"
#endif
#if XSHMEM_CRAY_HEAP == 2 && !defined(XSHMEM_CRAY_HINT)
#error "xshmem_cray.h: XSHMEM_CRAY_HEAP=2 needs -DXSHMEM_CRAY_HINT=<the 12.1 device-memory hint constant>; not guessed"
#endif
#if XSHMEM_CRAY_HEAP < 0 || XSHMEM_CRAY_HEAP > 3
#error "xshmem_cray.h: XSHMEM_CRAY_HEAP must be 0..3"
#endif
#ifndef XSHMEM_CRAY_HOSTREG
#define XSHMEM_CRAY_HOSTREG 1       /* HEAP=0: hipHostRegister the DATA heap (the engine's copy kernels and hipMemcpy touch it from the GPU) */
#endif
#if XSHMEM_CRAY_HEAP == 1 || XSHMEM_CRAY_HEAP == 3 || (defined(USE_HIP) && XSHMEM_CRAY_HOSTREG)
#include <hip/hip_runtime_api.h>      /* zeroing a device heap from the host; registering the host heap */
#endif

#define XSHMEM_CTX_DEFAULT ((xshmem_ctx_t)SHMEM_CTX_DEFAULT)

/* heap bookkeeping shared by all translation units (one weak definition).
 * ASSUMES K=1 (one heap slot per space: comm_xshmem.c forces K=1 for cray): a second DATA heap while one is registered is fatal. */
typedef struct xshmem_cray_state {
    char *base[2];            /* per space */
    size_t size[2];
    char *reg_base;           /* hipHostRegister'ed by us (page range enclosing the heap), else NULL; never set when libsma already owns the mapping */
    int prov;
} xshmem_cray_state_t;
extern xshmem_cray_state_t xshmem_cray_st;
__attribute__((weak)) xshmem_cray_state_t xshmem_cray_st;

static inline int xshmem_init_thread(int req, int *prov) { int pv = 0; int r = shmem_init_thread(req, &pv); if (prov) *prov = pv; xshmem_cray_st.prov = pv; return r; }
static inline void xshmem_finalize(void) { shmem_finalize(); }
static inline void xshmem_global_exit(int status) { shmem_global_exit(status); }
static inline int xshmem_my_pe(void) { return shmem_my_pe(); }
static inline int xshmem_n_pes(void) { return shmem_n_pes(); }


/* XSHMEM_CRAY_HEAP_THP (default 0 = off, nothing runs): 1 = madvise(MADV_HUGEPAGE) on the DATA heap before it is zeroed, then MADV_COLLAPSE after (the
 * COMM_SHMEM_NP_HEAP_THP idea for the libsma symmetric heap); 2 = only report the heap's backing.  Both print one line per PE from /proc/self/smaps:
 * the mapping holding the heap (path, KernelPageSize, Rss, AnonHugePages, ShmemPmdMapped).  Diagnosis of multi-second stream syncs (craylane). */
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
static inline int xshmem_cray_thp_mode(void) { const char *e = getenv("XSHMEM_CRAY_HEAP_THP"); int m = e ? atoi(e) : 0; return m < 0 || m > 2 ? 0 : m; }
static inline void xshmem_cray_thp_report(void *p, size_t n, const char *when)
{
    FILE *f = fopen("/proc/self/smaps", "r"); char l[512], hdr[512] = ""; int in = 0; long kps = 0, rss = 0, ahp = 0, spmd = 0, fpmd = 0;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        unsigned long lo, hi;
        if (sscanf(l, "%lx-%lx ", &lo, &hi) == 2 && strchr(l, '-') && l[0] != 'K') {
            if (in) break;
            in = (uintptr_t)p >= lo && (uintptr_t)p < hi; if (in) { strncpy(hdr, l, sizeof hdr - 1); hdr[strcspn(hdr, "\n")] = 0; }
        } else if (in) {
            sscanf(l, "KernelPageSize: %ld", &kps); sscanf(l, "Rss: %ld", &rss); sscanf(l, "AnonHugePages: %ld", &ahp);
            sscanf(l, "ShmemPmdMapped: %ld", &spmd); sscanf(l, "FilePmdMapped: %ld", &fpmd);
        }
    }
    fclose(f);
    fprintf(stderr, "xshmem_cray: pe %d: DATA heap %p %zu MiB %s: mapping [%s] KernelPageSize %ld kB, Rss %ld MiB, AnonHugePages %ld MiB, ShmemPmdMapped %ld MiB, FilePmdMapped %ld MiB\n",
            shmem_my_pe(), p, n >> 20, when, hdr, kps, rss >> 10, ahp >> 10, spmd >> 10, fpmd >> 10);
}

static inline void *xshmem_heap_alloc(size_t bytes, int space)
{
    xshmem_cfg_t cfg; void *p; int host;
    size_t n = (bytes + (2u << 20) - 1) & ~(size_t)((2u << 20) - 1);
    xshmem_cfg_load(&cfg);
    if (xshmem_cray_st.base[space]) { fprintf(stderr, "xshmem_cray: second heap allocation in space %d while one is registered (single-slot bookkeeping assumes K=1)\n", space); shmem_global_exit(1); }
    host = (XSHMEM_CRAY_HEAP == 0);
    if (host) p = shmem_malloc(n);
#if XSHMEM_CRAY_HEAP == 1
    else p = shmem_space_malloc(SHMEM_SPACE_DEVICE, n);
#elif XSHMEM_CRAY_HEAP == 2
    else p = shmem_malloc_with_hints(n, XSHMEM_CRAY_HINT);
#elif XSHMEM_CRAY_HEAP == 3
    else p = shmemg_malloc(n);
#else
    else p = NULL;
#endif
    if (!p) { fprintf(stderr, "xshmem: heap_alloc(%zu, space %d) failed\n", n, space); shmem_global_exit(1); }
    int thp = (host && space == XSHMEM_SPACE_DATA) ? xshmem_cray_thp_mode() : 0;
    if (thp == 1) { uintptr_t a = (uintptr_t)p & ~(uintptr_t)4095; if (madvise((void *)a, n + ((uintptr_t)p - a), MADV_HUGEPAGE)) fprintf(stderr, "xshmem_cray: pe %d: MADV_HUGEPAGE failed: errno %d\n", shmem_my_pe(), errno); }
#if XSHMEM_CRAY_HEAP == 1 || XSHMEM_CRAY_HEAP == 3
    if (!host) hipMemset(p, 0, n); else
#endif
    memset(p, 0, n);
    if (thp) {
        if (thp == 1) { const uintptr_t H = 2u << 20; uintptr_t a = ((uintptr_t)p + H - 1) & ~(H - 1), e = ((uintptr_t)p + n) & ~(H - 1); int err = 0; for (uintptr_t o = a; o < e; o += (256u << 20)) { size_t l = e - o < (256u << 20) ? e - o : (256u << 20); if (madvise((void *)o, l, MADV_COLLAPSE) && !err) err = errno; } if (err) fprintf(stderr, "xshmem_cray: pe %d: MADV_COLLAPSE: errno %d\n", shmem_my_pe(), err); }
        xshmem_cray_thp_report(p, n, thp == 1 ? "after MADV_HUGEPAGE+memset+COLLAPSE" : "after memset");
    }
#if defined(USE_HIP) && XSHMEM_CRAY_HOSTREG
    /* host heap (11.8 has no device heap): register it so GPU kernels / hipMemcpyAsync of the engine reach it at memory speed (the 12.1 device heap needs none) */
    if (host && space == XSHMEM_SPACE_DATA) {
        const uintptr_t pg = 4096; uintptr_t a = (uintptr_t)p, lo = a & ~(pg - 1), hi = (a + n + pg - 1) & ~(pg - 1); /* the enclosing pages: the heap's partial first/last page must be GPU-reachable too (the engine touches the tail; an inside range faulted) */
        static int warned;
        if (a & (pg - 1) && !warned) { warned = 1; fprintf(stderr, "xshmem_cray: warning: DATA heap base %p is not page aligned\n", p); }
        if (hi <= lo) fprintf(stderr, "xshmem_cray: warning: empty DATA heap range: not registered (pageable access)\n");
        else {
            hipError_t he = hipHostRegister((void *)lo, hi - lo, hipHostRegisterPortable);
            if (he == hipSuccess) xshmem_cray_st.reg_base = (char *)lo;
            else if (he == hipErrorHostMemoryAlreadyRegistered) (void)hipGetLastError(); /* libsma owns it: never unregister */
            else { (void)hipGetLastError(); fprintf(stderr, "xshmem_cray: hipHostRegister of the DATA heap (%zu MiB) failed: pageable access\n", n >> 20); }
        }
    }
#endif
    xshmem_cray_st.base[space] = (char *)p; xshmem_cray_st.size[space] = n;
    shmem_barrier_all();
    return p;
}
static inline void xshmem_heap_free(void *base)
{
    shmem_barrier_all();
#if defined(USE_HIP) && XSHMEM_CRAY_HOSTREG
    if (base == xshmem_cray_st.base[0] && xshmem_cray_st.reg_base) { (void)hipHostUnregister(xshmem_cray_st.reg_base); xshmem_cray_st.reg_base = NULL; }
    (void)hipGetLastError();
#endif
    shmem_free(base);
    if (base == xshmem_cray_st.base[0]) xshmem_cray_st.base[0] = NULL;
    if (base == xshmem_cray_st.base[1]) xshmem_cray_st.base[1] = NULL;
}

static inline ptrdiff_t xshmem_off(const void *p)
{
    const char *c = (const char *)p;
    if (xshmem_cray_st.base[1] && c >= xshmem_cray_st.base[1] && c < xshmem_cray_st.base[1] + xshmem_cray_st.size[1])
        return (ptrdiff_t)(c - xshmem_cray_st.base[1]) | XSHMEM_OFF_CTRL;
    return (ptrdiff_t)(c - xshmem_cray_st.base[0]);
}
static inline void *xshmem_at(ptrdiff_t off)
{
    return (off & XSHMEM_OFF_CTRL) ? (void *)(xshmem_cray_st.base[1] + (off & ~XSHMEM_OFF_CTRL)) : (void *)(xshmem_cray_st.base[0] + off);
}

static inline void xshmem_ctx_signal_set(xshmem_ctx_t ctx, uint64_t *sig, uint64_t val, int pe) { shmemx_ctx_signal_set((shmem_ctx_t)ctx, sig, val, pe); }
static inline void xshmem_ctx_putmem_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, int pe)
{ shmem_ctx_putmem_nbi((shmem_ctx_t)ctx, dest, src, n, pe); }
static inline void xshmem_ctx_quiet(xshmem_ctx_t ctx) { shmem_ctx_quiet((shmem_ctx_t)ctx); }
static inline void xshmem_ctx_fence(xshmem_ctx_t ctx) { shmem_ctx_fence((shmem_ctx_t)ctx); }
static inline void xshmem_ctx_putmem_signal_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, uint64_t *sig, uint64_t val, int sig_op, int pe)
{
    (void)sig_op;                                  /* XSHMEM_SIGNAL_SET only */
    shmem_ctx_putmem_signal_nbi((shmem_ctx_t)ctx, dest, src, n, sig, val, SHMEM_SIGNAL_SET, pe);
}

static inline uint64_t xshmem_signal_wait_until(uint64_t *sig, int cmp, uint64_t v)
{
    (void)cmp;                                     /* XSHMEM_CMP_GE only */
#if XSHMEM_CRAY_WAIT == 0
    return shmem_signal_wait_until(sig, SHMEM_CMP_GE, v);
#else
    shmem_uint64_wait_until(sig, SHMEM_CMP_GE, v);
    return *(volatile uint64_t *)sig;
#endif
}
static inline int xshmem_signal_test(const uint64_t *sig, int cmp, uint64_t v)
{
    (void)cmp;
    return shmem_signal_fetch(sig) >= v;
}

static inline int xshmem_ctx_create(long opt, xshmem_ctx_t *c) {
    shmem_ctx_t x; long o = 0; int r;
    if (opt & XSHMEM_CTX_SERIALIZED) o |= SHMEM_CTX_SERIALIZED;     /* the xshmem values are the engine's own: translate explicitly */
    if (opt & XSHMEM_CTX_PRIVATE) o |= SHMEM_CTX_PRIVATE;
    r = shmem_ctx_create(o, &x); if (!r) *c = (xshmem_ctx_t)x; return r;
}
static inline void xshmem_ctx_destroy(xshmem_ctx_t ctx) { shmem_ctx_destroy((shmem_ctx_t)ctx); }
static inline void xshmem_barrier_all(void) { shmem_barrier_all(); }
static inline void *xshmem_ptr(const void *p, int pe) { return shmem_ptr(p, pe); }
static inline void xshmem_progress(void) { }       /* libsma progresses inside quiet / wait / test (SHMEM_OFI_PROGRESS = library thread) */

/* topology: pe_geo supplies the full map for the ofi backend; here only what plain SHMEM + the launcher tell (target: replace by pe_geo,
 * B3's module has no SHMEM dependency).  NIC list unknown to SHMEM: nics = nlanes = XSHMEM_LANES or 1. */
static int xshmem_early_numa = -1;   /* NUMA node of the early pin in comm_xshmem.c (-1: none); xshmem_topo warns once if it differs */
static inline int xshmem_topo(xshmem_topo_t *t)
{
    xshmem_cfg_t cfg; const char *e;
    memset(t, 0, sizeof *t); xshmem_cfg_load(&cfg);
    t->pe = shmem_my_pe(); t->npes = shmem_n_pes();
    e = getenv("SLURM_NTASKS_PER_NODE"); t->pes_per_node = e ? atoi(e) : 1; if (t->pes_per_node < 1) t->pes_per_node = 1;
    e = getenv("SLURM_LOCALID"); t->local = e ? atoi(e) : t->pe % t->pes_per_node;
    t->node = t->pe / t->pes_per_node; t->nnodes = (t->npes + t->pes_per_node - 1) / t->pes_per_node;
    e = getenv("XSHMEM_PES_PER_GPU"); t->pes_per_gpu = e && atoi(e) > 0 ? atoi(e) : 1;   /* as pe_geo: 2 PEs per GPU = device local / 2 */
    t->gpu = t->local / t->pes_per_gpu; t->gpu_node = t->gpu;
    t->nlanes = cfg.lanes > 0 ? cfg.lanes : 1; t->nics = t->nlanes; t->numa = t->gpu_node;
    e = getenv("XSHMEM_CRAY_NUMA");   /* 1: the PE's NUMA node and cpu list from pe_geo (sysfs: GPU bus id -> NUMA node -> cpulist), as the ofi backend; 0 (default): none, mn.c leaves the affinity as launched */
    if (e && atoi(e) > 0) {
        int ngpu = 0; char bus[64] = { 0 }; char *bp = NULL; pe_topo g;
#ifdef USE_HIP
        if (hipGetDeviceCount(&ngpu) != hipSuccess) { (void)hipGetLastError(); ngpu = 0; }
        if (ngpu > 0) { int dev = ngpu == 1 ? 0 : t->gpu % ngpu;
            if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) == hipSuccess) { for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A'; bp = bus; } else (void)hipGetLastError(); }
#endif
        if (pe_geo_from(&g, t->pe, t->npes, t->local, t->pes_per_node, ngpu, bp) == 0) { t->numa = g.numa; strncpy(t->cpus, g.cpus, sizeof t->cpus - 1);
            if (xshmem_early_numa >= 0 && xshmem_early_numa != g.numa) fprintf(stderr, "xshmem_cray: pe %d: WARNING early NUMA pin node %d differs from topo node %d\n", t->pe, xshmem_early_numa, g.numa); }
        else fprintf(stderr, "xshmem_cray: XSHMEM_CRAY_NUMA=1: pe_geo: %s (no affinity)\n", pe_geo_err());
    }
    return 0;
}
static inline void xshmem_caps_query(xshmem_caps_t *c)
{
    xshmem_cfg_t cfg; xshmem_cfg_load(&cfg);
    memset(c, 0, sizeof *c);
    c->dev_heap = XSHMEM_CRAY_HEAP != 0;
    c->host_pollable = 1;      /* host heap: yes; device heap [A]: test T2 on the target */
    c->src_any = 0;                                                      /* 11.8 rejects out-of-heap sources [M] */
    c->ptr = 0;
    c->nlanes_ok = cfg.lanes > 0 ? cfg.lanes : 1;
}

#endif
