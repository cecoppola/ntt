/* xshmem.h -- the communication contract of the target-shaped engine (plan TARGET1, design TARGET12_DESIGN rev1; FROZEN by brief B1).
 *
 * The engine (comm_xshmem.c, later) and the tests (tests/t_xshmem.c) include ONLY this header: never fi_* , shmem_* or pmi calls.
 * Two backends, chosen at build time (ecalc/Makefile, XSHMEM=ofi|cray; XSHMEM unset or 0 = nothing of this is built):
 *   XSHMEM=ofi   xshmem_ofi.c : libfabric cxi (aac7 development system); boot from xshmem_boot.h (PMI2), topology from pe_geo.h
 *   XSHMEM=cray  xshmem_cray.h: static inline, one forwarding line per call into Cray SHMEM (11.8 on aac7, 12.1 on the target)
 * Every call is xshmem_<name> where shmem_<name> is the OpenSHMEM/Cray name; the context argument is always explicit.
 *
 * ---- API table (tools/xshmem_api_check.py greps this block; keep one `XAPI <name>` line per function) ----
 * XAPI xshmem_init_thread        int  (int req, int *prov)                                   boot, open NIC domains, lane endpoints
 * XAPI xshmem_finalize           void (void)
 * XAPI xshmem_global_exit        void (int status)
 * XAPI xshmem_my_pe              int  (void)
 * XAPI xshmem_n_pes              int  (void)
 * XAPI xshmem_heap_alloc         void*(size_t bytes, int space)       collective, at most ONCE per space, same order on all PEs
 * XAPI xshmem_heap_free          void (void *base)                    collective, once per space, takes the base heap_alloc returned
 * XAPI xshmem_off                ptrdiff_t (const void *p)            inline in the backend; offset inside p's space (see 'Offsets')
 * XAPI xshmem_at                 void*(ptrdiff_t off)                 inverse of xshmem_off on the LOCAL heap
 * XAPI xshmem_ctx_putmem_nbi     void (ctx, void *dest, const void *src, size_t n, int pe)
 * XAPI xshmem_ctx_putmem_signal_nbi void (ctx, void *dest, const void *src, size_t n, uint64_t *sig, uint64_t val, int sig_op, int pe)
 * XAPI xshmem_ctx_signal_set     void (ctx, uint64_t *sig, uint64_t val, int pe)              8 B plain write, no atomic
 * XAPI xshmem_signal_wait_until  uint64_t (uint64_t *sig, int cmp, uint64_t v)       blocking; engine wait loops use _test instead
 * XAPI xshmem_signal_test        int  (const uint64_t *sig, int cmp, uint64_t v)     non-blocking, makes progress
 * XAPI xshmem_ctx_quiet          void (ctx)
 * XAPI xshmem_ctx_fence          void (ctx)                           = quiet at 11.8 (and in the ofi backend)
 * XAPI xshmem_ctx_create         int  (long opt, xshmem_ctx_t *c)
 * XAPI xshmem_ctx_destroy        void (ctx)
 * XAPI xshmem_barrier_all        void (void)                          init/finalize/test only
 * XAPI xshmem_ptr                void*(const void *p, int pe)         NULL unless the backend can map it (XSHMEM_IPC / shmem_ptr)
 * XAPI xshmem_progress           void (void)                          engine hook, no SHMEM name; safe from 1 worker thread + 1 caller
 * XAPI xshmem_topo               int  (xshmem_topo_t *t)              0 = ok; no SHMEM name; ofi: from pe_geo
 * XAPI xshmem_caps_query         void (xshmem_caps_t *c)              no SHMEM name
 * (config: static inline xshmem_cfg_load() below reads every XSHMEM_* switch in one place; backends and tests use the struct)
 *
 * ---- Contracts ----
 * Lane order (design 2.2): a lane is one context.  The engine creates its N lane contexts with xshmem_ctx_create IMMEDIATELY after
 *   xshmem_init_thread, in lane order 0..N-1, before any other context and before xshmem_heap_alloc.  The k-th created context is
 *   lane k = NIC k of the PE (ofi: by construction; Cray: an assumption to be tested on the target, T1).  N = xshmem_topo.nlanes
 *   (aac7 1 per PE, target 2 expected).  XSHMEM_CTX_DEFAULT is the ctx0 used for control words.
 * Heap: xshmem_heap_alloc(bytes, XSHMEM_SPACE_DATA) is the payload heap (the engine's sub-allocator works inside it);
 *   xshmem_heap_alloc(bytes, XSHMEM_SPACE_CTRL) is the control area (8 B words: ROFF, SIG, VAL, VSEQ; polled by the host).  A backend may
 *   place both in one allocation (XSHMEM_CTRL=heap, default) or CTRL in host memory (XSHMEM_CTRL=host, design 1.3 b).  Size is bytes,
 *   2 MiB aligned by the backend.  Memory is zeroed.  Payload/signal pointers passed to put and signal calls must lie inside a heap,
 *   except a put source when caps.src_any = 1.
 * Offsets: xshmem_off(p) = byte offset of p inside the heap of its space; offsets of CTRL pointers carry XSHMEM_OFF_CTRL (bit 62), so
 *   one ptrdiff_t names any heap byte.  Only offsets are ever published to peers (no raw pointer crosses PEs); a sender computes the
 *   remote dest as xshmem_at(off) on its OWN heap (symmetric layout, heap bases may differ).  xshmem_at never returns NULL for a
 *   valid offset.
 * Signals: a signal word is a uint64_t in a CTRL (or DATA) heap, written ONLY by plain 8 B SET writes (signal_set, or the signal of
 *   putmem_signal_nbi with XSHMEM_SIGNAL_SET), one writer per word.  Visibility rule the engine relies on: when the target sees the
 *   signal value, every putmem_nbi that the SAME context completed with xshmem_ctx_quiet BEFORE the signal_set is visible there.
 *   (putmem_signal_nbi: payload visible before the signal, by the backend's means.)  The comparison is on the full 64-bit word
 *   (XSHMEM_CMP_GE only); the engine packs seq<<40|v and owns wrap.
 * Completion: *_nbi calls return at once; xshmem_ctx_quiet(ctx) returns when all earlier puts and signal_sets of ctx are complete
 *   at their targets.  xshmem_ctx_fence = quiet.  Contexts complete independently.
 * Threads: init_thread returns the provided level.  Calls on one context from several threads need the engine's mutex unless prov is
 *   XSHMEM_THREAD_MULTIPLE.  xshmem_progress, _signal_test and _signal_wait_until may be called concurrently with one other caller.
 * Errors: fatal (abort with a message on stderr); only xshmem_ctx_create / xshmem_topo return codes.
 *
 * ---- Stubs implemented by other briefs (declared here, compiled only for XSHMEM=ofi) ----
 *   xshmem_ofi.c  (B2)  every XAPI above; plus xshmem_ofi_counters().  Boot via xshmem_boot.h (B3): boot_init/boot_allgather/boot_fence/boot_finalize.
 *   pe_geo.h/.c   (B3)  topology discovery behind xshmem_topo.
 * Until xshmem_ofi.c lands, xshmem_ofi_stub.c aborts with "not implemented" (the Makefile picks the real file when present; B2 deletes the stub).
 */
#ifndef XSHMEM_H
#define XSHMEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XSHMEM_CRAY) && defined(XSHMEM_OFI)
#error "xshmem: choose one backend (XSHMEM_CRAY or XSHMEM_OFI)"
#endif

/* context handle: opaque pointer, same width as shmem_ctx_t (void *) so the Cray header forwards without a table */
typedef void *xshmem_ctx_t;

/* constants (values fixed here; the Cray header translates to the library's) */
#define XSHMEM_SPACE_DATA       0
#define XSHMEM_SPACE_CTRL       1
#define XSHMEM_CMP_GE           5      /* the only comparison the engine uses */
#define XSHMEM_SIGNAL_SET       1      /* the only signal op */
#define XSHMEM_THREAD_SINGLE    0
#define XSHMEM_THREAD_FUNNELED  1
#define XSHMEM_THREAD_SERIALIZED 2
#define XSHMEM_THREAD_MULTIPLE  3
#define XSHMEM_CTX_SERIALIZED   2      /* xshmem_ctx_create options (values of Cray SHMEM_CTX_*) */
#define XSHMEM_CTX_PRIVATE      1
#define XSHMEM_OFF_CTRL         ((ptrdiff_t)1 << 62)
#define XSHMEM_TOPO_MAXNIC      16     /* capacity of xshmem_topo_t.nic[], not a NIC count */

/* what the backend can do (filled once at init; design 1.3) */
typedef struct xshmem_caps {
    int dev_heap;       /* DATA heap is GPU device memory (0: host heap fallback) */
    int host_pollable;  /* CTRL words can be polled by the host while the NIC / library writes them */
    int src_any;        /* putmem_nbi source may lie outside the heap (0: engine stages) */
    int ptr;            /* xshmem_ptr can return non-NULL for node-local PEs */
    int nlanes_ok;      /* lanes the backend can serve: contexts creatable and mapped to distinct NICs (>= 1) */
} xshmem_caps_t;

/* topology of this PE (the pe_geo module's data, copied; fields as B3's pe_topo (pe_geo.h)) */
typedef struct xshmem_topo {
    int pe, npes;
    int node, nnodes;
    int local, pes_per_node;
    int gpu;                       /* device index this PE uses */
    int gpu_node;                  /* node-local GPU index = local / pes_per_gpu */
    int nics;                      /* NICs of this PE */
    int nic[XSHMEM_TOPO_MAXNIC];   /* cxi device numbers, nics entries */
    int nlanes;                    /* lanes the engine creates (default 1: XSHMEM_LANES unset = 1, also on the target for the first engine; the target default follows day-one check T1) */
    int numa;
    int pes_per_gpu;
    char cpus[512];                /* CPU list string of the PE's NUMA node */
} xshmem_topo_t;

/* ---- configuration: every XSHMEM_* switch is read here and nowhere else (registry sites; defaults all "off / unset") ---- */
enum { XSHMEM_SIG_DC = 0, XSHMEM_SIG_FLUSH = 1, XSHMEM_SIG_WAW = 2 };
enum { XSHMEM_MR_LIBSMA = 0, XSHMEM_MR_OFI = 1 };
typedef struct xshmem_cfg {
    int sig;          /* XSHMEM_OFI_SIG   dc|flush|waw            (ofi backend; default dc) */
    int lanes;        /* XSHMEM_LANES     lanes per PE, default 1 (same literal as pe_geo.c) */
    long heap_mb;     /* XSHMEM_HEAP_MB   0 = backend / planner default */
    int mrmode;       /* XSHMEM_MRMODE    libsma|ofi              (ofi backend; default libsma, B2 reports the other) */
    int ctrl_host;    /* XSHMEM_CTRL      heap|host               (default heap) */
    int local;        /* XSHMEM_LOCAL     0|1  engine uses xshmem_ptr paths (default 0) */
    int ipc;          /* XSHMEM_IPC       0|1  ofi backend maps node-local heaps with hipIpc (default 0) */
    int noyield;      /* XSHMEM_YIELD     0 = the waits and the worker never sched_yield (default 1: they do) */
    int loopback_pes; /* XSHMEM_LOOPBACK_PES  PEs hosted by one process (ofi backend; default 1 = one PE per process) */
} xshmem_cfg_t;

static inline void xshmem_cfg_load(xshmem_cfg_t *c)
{
    const char *e;
    memset(c, 0, sizeof *c);
    e = getenv("XSHMEM_OFI_SIG");
    if (e && !strcmp(e, "flush")) c->sig = XSHMEM_SIG_FLUSH; else if (e && !strcmp(e, "waw")) c->sig = XSHMEM_SIG_WAW;
    e = getenv("XSHMEM_LANES");    c->lanes = e ? atoi(e) : 1;
    e = getenv("XSHMEM_HEAP_MB");  c->heap_mb = e ? atol(e) : 0;
    e = getenv("XSHMEM_MRMODE");   c->mrmode = (e && !strcmp(e, "ofi")) ? XSHMEM_MR_OFI : XSHMEM_MR_LIBSMA;
    e = getenv("XSHMEM_CTRL");     c->ctrl_host = (e && !strcmp(e, "host")) ? 1 : 0;
    e = getenv("XSHMEM_LOCAL");    c->local = e ? atoi(e) : 0;
    e = getenv("XSHMEM_IPC");      c->ipc = e ? atoi(e) : 0;
    e = getenv("XSHMEM_YIELD");    c->noyield = e && atoi(e) == 0;
    e = getenv("XSHMEM_LOOPBACK_PES"); c->loopback_pes = e ? atoi(e) : 1;
}

#if defined(XSHMEM_CRAY)
#include "xshmem_cray.h"
#define XSHMEM_SELECT(lpe) ((void)(lpe))       /* no loopback PEs on the target: one PE per process */
#else
/* ---- libfabric backend (xshmem_ofi.c; B2).  Also what a compile-only check of the header sees without a backend macro. ---- */
#define XSHMEM_CTX_DEFAULT ((xshmem_ctx_t)0)
int       xshmem_init_thread(int req, int *prov);
void      xshmem_finalize(void);
void      xshmem_global_exit(int status);
int       xshmem_my_pe(void);
int       xshmem_n_pes(void);
void     *xshmem_heap_alloc(size_t bytes, int space);
void      xshmem_heap_free(void *base);
ptrdiff_t xshmem_off(const void *p);
void     *xshmem_at(ptrdiff_t off);
void      xshmem_ctx_putmem_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, int pe);
void      xshmem_ctx_putmem_signal_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, uint64_t *sig, uint64_t val, int sig_op, int pe);
void      xshmem_ctx_signal_set(xshmem_ctx_t ctx, uint64_t *sig, uint64_t val, int pe);
uint64_t  xshmem_signal_wait_until(uint64_t *sig, int cmp, uint64_t v);
int       xshmem_signal_test(const uint64_t *sig, int cmp, uint64_t v);
void      xshmem_ctx_quiet(xshmem_ctx_t ctx);
void      xshmem_ctx_fence(xshmem_ctx_t ctx);
int       xshmem_ctx_create(long opt, xshmem_ctx_t *c);
void      xshmem_ctx_destroy(xshmem_ctx_t ctx);
void      xshmem_barrier_all(void);
void     *xshmem_ptr(const void *p, int pe);
void      xshmem_progress(void);
int       xshmem_topo(xshmem_topo_t *t);
void      xshmem_caps_query(xshmem_caps_t *c);

/* ofi-only diagnostics for the tests (B2): counters of the backend since init */
typedef struct xshmem_ofi_cnt { uint64_t writes, flush_chains, quiets, wait_spins; } xshmem_ofi_cnt_t;
void      xshmem_ofi_counters(xshmem_ofi_cnt_t *c);
/* loopback PEs (XSHMEM_LOOPBACK_PES = K, default 1): one process hosts K PEs, PE = K*rank + l.  A thread selects the PE its context-less calls
 * (my_pe, signal_test, progress, off/at, ctx 0, ctx_create, heap_alloc/free) act on; context calls follow their context.  heap_alloc/heap_free are called
 * by the K local PEs in order 0..K-1 from ONE thread (XSHMEM_SELECT(l) before each); barrier_all / finalize once per process.  K = 1: nothing changes. */
void      xshmem_ofi_loopback_select(int lpe);
int       xshmem_ofi_loopback_pes(void);
#define XSHMEM_SELECT(lpe) xshmem_ofi_loopback_select(lpe)
#endif

#ifdef __cplusplus
}
#endif
#endif
