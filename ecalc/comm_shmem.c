/* comm_shmem.c - the SHMEM transport (Phase 11 M8-s, Phase 12 S; PLAN.md 25-27): the comm_ops of comm.h over the OpenSHMEM
 * 1.4 subset that Cray OpenSHMEMX, rocSHMEM, Sandia OpenSHMEM and OpenMPI's OSHMEM share (plus put-with-signal where 1.5
 * exists).  One process per node = one PE; a communicator is a strided PE set (start, stride, size) -- the tree's node
 * groups -- with rank r = PE start + stride r.
 *
 * Memory.  One symmetric pool (COMM_SHMEM_POOL_MB) holds everything remote PEs write: a fixed mailbox table at its start,
 * then a per-PE first-fit allocator for the communicators' control blocks, their staging, and -- comm_sym_alloc -- the
 * callers' slab buffers.  Three forms of the pool (results/S12.md):
 *   - shmem_malloc'd host memory registered with hipHostRegister (OSHMEM, SOS: the default) -- on the APU the same HBM;
 *   - COMM_SHMEM_DEVHEAP=1 with an implementation whose shmem_malloc returns device memory (Cray / rocSHMEM): no
 *     registration, every copy D2D;
 *   - COMM_SHMEM_DEVHEAP=1 on SOS built with the external-heap patch (SHMEMX_EXTERNAL_HEAP_HOST): the pool is a HIP
 *     fine-grained device buffer (=2: hipMallocManaged) that the transport registers as SOS's external symmetric heap.
 * Offsets in the pool are what the PEs exchange: a put targets pool + offset, and the offset is one the RECEIVER chose
 * (its slab buffer, its staging, its control block), so nothing has to be allocated collectively after init -- creation
 * is the only handshake (each member posts its control block's offset into every member's mailbox[id][me]; ids come from
 * the caller, unique per run).
 *
 * The push model (as comm_xgmi): an exchange has a sequence number; the receiver publishes, per sender, where the
 * sender's slab lands (roff[sender] = seq | offset, in the sender's control block); the sender waits for that word and
 * puts its slab there, then the signal word sig[sender] = seq | byte count into the receiver's block, ordered after the
 * data by (COMM_SHMEM_ORDER) `putsig`: shmem_putmem_signal_nbi (1.5; one call, nothing blocks: the default where the
 * headers are 1.5), `fence`: shmem_fence + shmem_long_p (1.4, the spec's form), `quiet`: shmem_quiet + long_p (the OSHMEM
 * 4.1 form: its fence does not order an nbi put before a later put -- results/S.md; the default on 1.4 headers).  The
 * receiver's wait() polls the signals, checks the counts, and quiets its own context (its send buffer is free again).
 * Buffers in the pool (comm_sym_alloc: ntt_dist's slabs, the layered communicator's scratch) are put from and into
 * directly; others are staged through the pool (D2H / H2D on the caller's stream, a copy per direction).
 * The sender's roff waits: alltoall() returns before completion (the slab pipeline, M7), so under `putsig` / `fence` the
 * puts are posted inline for the peers whose roff has arrived within COMM_SHMEM_SPIN_US (2000; in lockstep pipelines all
 * of them) and a helper thread posts the rest; under `quiet` (a blocking call before the signals) the helper thread does
 * all of it, as in Phase 11.  COMM_SHMEM_THREAD=1 forces the thread.
 * all-gather, the unequal all-to-all (counts known on both sides from the descriptors: the receiver's offsets are what it
 * publishes), barrier / max / sum-mod-q (an all-to-all of 8-byte values through the val/vseq words, double-buffered by
 * the sequence's parity) and point-to-point (a ring per (dest, source) with producer / consumer counters,
 * COMM_SHMEM_RING_KB) are the same words and puts.
 *
 * Threads.  The four APU threads drive four communicators at once.  COMM_SHMEM_SERIAL=0 (the target form; the default
 * on a library that provides SHMEM_THREAD_MULTIPLE other than OSHMEM 4.1): every communicator has its own context, the
 * waits are shmem_wait_until, no lock.  COMM_SHMEM_SERIAL=1 (the default on OSHMEM, whose UCX progress crashes under
 * concurrent wait_until): one process-wide mutex around every library call, the waits polled shmem_test under short
 * holds, the default context throughout.  Collectives of the library (shmem_barrier_all, reductions, teams) are used
 * nowhere except init/finalize: they are process-level and would need all four threads to agree; the strided PE set
 * is the shim for shmem_team_split_strided.
 * Build: -DCOMM_SHMEM with the SHMEM headers (Makefile: SHMEM=1 with oshcc, or SHMEM_HOME=<SOS prefix>); without it the
 * entry points abort.  The exact call list is in results/S12.md (the portability contract). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE                                       /* dladdr (COMM_SHMEM_VERBOSE=2) */
#endif
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include <execinfo.h>
#include <dlfcn.h>
#include "comm.h"
#include "comm_ofi.h"                             /* Phase 17: COMM_OFI=1, the data of the device exchanges over libfabric (docs/code/07_COMM_OFI.md) */
/* Phase 14 V1: the library heap the pool is carved from, for the pool rule before init (binsplit_shmem_pool_rule): the heap's
 * size from the launch line's variable (SHMEM_SYMMETRIC_HEAP_SIZE for OSHMEM, SHMEM_SYMMETRIC_SIZE for SOS and Cray; K / M / G / T
 * suffixes, else bytes), 0 when unset; *name = the variable */
size_t comm_shmem_heap_env(const char **name)
{
#if defined(COMM_SHMEM_OSHMEM)
    const char *nm = "SHMEM_SYMMETRIC_HEAP_SIZE";
#else
    const char *nm = "SHMEM_SYMMETRIC_SIZE";
#endif
    if (name) *name = nm;
    const char *e = getenv(nm); if (!e || !*e) return 0;
    char *end; double v = strtod(e, &end);
    switch (*end) { case 'k': case 'K': v *= 1024.0; break; case 'm': case 'M': v *= 1048576.0; break; case 'g': case 'G': v *= 1073741824.0; break; case 't': case 'T': v *= 1099511627776.0; break; default: break; }
    return v > 0 ? (size_t)v : 0;
}
#ifndef COMM_SHMEM
int comm_shmem_pool_in_heap(void) { return 0; }
static void no_shmem(void) { ec_fatal(EC_RC_FATAL, "comm_shmem: built without SHMEM (make SHMEM=1 with oshcc / SHMEM_HOME=<SOS prefix> / the target's SHMEM)\n"); }
int   comm_shmem_init(void) { no_shmem(); return 0; }
int   comm_shmem_rank(void) { return 0; }
int   comm_shmem_size(void) { return 1; }
void  comm_shmem_counters(double *t, long *n, double *b) { *t = 0; *n = 0; *b = 0; }
void  comm_shmem_finalize(void) { }
comm *comm_shmem_create_at(int pe_start, int pe_stride, int n, int id) { (void)pe_start; (void)pe_stride; (void)n; (void)id; no_shmem(); return 0; }
int   comm_shmem_available(void) { return 0; }
const char *comm_shmem_impl(void) { return "none"; }
#else
#include <shmem.h>
#ifdef COMM_SHMEM_SOS
#include <shmemx.h>
#endif
#if SHMEM_MAJOR_VERSION > 1 || (SHMEM_MAJOR_VERSION == 1 && SHMEM_MINOR_VERSION >= 5)
#define HAVE_PUT_SIGNAL 1                  /* shmem_ctx_putmem_signal_nbi (OpenSHMEM 1.5) */
#else
#define HAVE_PUT_SIGNAL 0
#endif
#ifdef COMM_HOST_ONLY
#define COPY(d, s, n, st) memcpy(d, s, n)
#define SYNC(st)
#else
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define COPY(d, s, n, st) HIP_CHECK(hipMemcpyAsync(d, s, n, hipMemcpyDefault, st))
#define SYNC(st) HIP_CHECK(hipStreamSynchronize(st))
#endif
#define MAXID 1024                         /* communicator ids (mailbox rows) */
#define ALIGN 256
enum { ORDER_QUIET, ORDER_FENCE, ORDER_PUTSIG };

/* ---- the process state ---- */
struct blk { size_t off, len; int used, kind; struct blk *next; };
static struct {
    int inited, me, npes, serial, devheap, order, thread_always, registered, extheap, prov, keep_staging;
    size_t put_bytes;                                     /* Phase 16 C: COMM_SHMEM_PUT_MB -- a put larger than this goes in pieces of this size (0 = whole, as before) */
    long spin_us;
    char *pool; size_t pool_bytes, mb_bytes;   /* the symmetric pool; the mailbox at [0, mb_bytes) */
    pthread_mutex_t lock;                  /* SHM_LOCK: the library in serial mode; the allocator always (alloc_lock) */
    pthread_mutex_t alloc_lock;
    struct blk *blocks;                    /* the general area's blocks */
    struct blk *slot[4]; size_t slot_bytes; int nslot;   /* Phase 14 V1: the staging regions, one per APU (COMM_SHMEM_STAGE_SLOT_MB, set by the pool rule) */
    size_t cur[3], peak[3], cur_all, peak_all;   /* pool accounting by kind: 0 control blocks, 1 staging, 2 the callers' symmetric buffers */
    size_t at_peak[3], nstage, nstage_at_peak, stage_blk_max, stage_rep;   /* Phase 14 P2: the kinds at the moment of peak_all, live staging blocks, the largest one, the last staging peak reported */
    int verbose;                           /* Phase 14 P2: COMM_SHMEM_VERBOSE=1 or ECALC_VERBOSE >= 2 -- a line per new staging peak (+5 %), every PE's summary */
    int xstats, porder, vslot_pool;                    /* X1: COMM_XSTATS=1 (per-kind exchange stats, printed at finalize); COMM_SHMEM_PEER_ORDER=rot (rotated, ready-first peer service) */
    size_t round_bytes;                    /* Phase 14 V1: COMM_SHMEM_ROUND_MB -- the staging of one alltoallv round, each way (0: off, one round) */
    long nrpath, nround_ex, nrounds; size_t round_stage_max;   /* Phase 14 V1: exchanges through the rounds' path, those in more than one round, their rounds, the largest per-round staging (send + recv) */
    double tv; long nv;                    /* Phase 14 V1: all-to-all time (alltoall, alltoallv), post to completion, summed over the APU threads; the count */
    double bv;                             /* Phase 16 A: the bytes this PE received from the other PEs in those exchanges (the off-process traffic), summed */
} S;
enum { K_CTRL, K_STAGE, K_SYM };
static void site_print(void);                           /* Phase 14 P2 (below) */
#define SHM_LOCK()   do { if (S.serial) pthread_mutex_lock(&S.lock); } while (0)
#define SHM_UNLOCK() do { if (S.serial) pthread_mutex_unlock(&S.lock); } while (0)
static int g_trace;
#define TRACE(...) do { if (g_trace) { fprintf(stderr, "comm_shmem: pe %d: ", S.me); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void die(const char *m) { ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: %s\n", S.me, m); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static void xs_print(void);   /* X1 (below) */
const char *comm_shmem_impl(void)
{
#if defined(COMM_SHMEM_SOS)
    return "sos";
#elif defined(COMM_SHMEM_OSHMEM)
    return "oshmem";
#else
    return "shmem";
#endif
}

/* the pool's first-fit allocator (offsets; a block list sorted by offset, coalesced on free).
 * Phase 14 V1: with the pool sized to the model's need (COMM_SHMEM_POOL_AUTO, mnrun.sh's plan) one first-fit area fragments:
 * the four APU threads allocate and free staging of different sizes out of step (4 processes at 1e10: 529 MiB not found with
 * 1441 MiB free in a 4608 MiB pool).  So the staging has one region per APU (the thread's HIP device), each the model's
 * per-APU need (COMM_SHMEM_STAGE_SLOT_MB, exported by binsplit_shmem_pool_rule): a thread's staging is its current exchange's
 * send + receive, freed before the next, so its region never fragments.  A request that does not fit its region (the model
 * low, a fifth thread) falls back to the general area; COMM_SHMEM_STAGE_SLOTS=0 turns the regions off. */
static struct blk **alloc_list(int kind)
{
#ifndef COMM_HOST_ONLY
    if (kind == K_STAGE && S.nslot) { int d = -1; if (hipGetDevice(&d) != hipSuccess) { (void)hipGetLastError(); d = -1; } if (d >= 0 && d < S.nslot) return &S.slot[d]; }
#endif
    (void)kind; return &S.blocks;
}
static size_t pool_alloc(size_t len, int kind)
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    struct blk **lists[2] = { alloc_list(kind), &S.blocks };
    pthread_mutex_lock(&S.alloc_lock);
    for (int li = 0; li < (lists[0] == lists[1] ? 1 : 2); li++)
    for (struct blk *b = *lists[li]; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1; b->kind = kind;
        S.cur[kind] += len; if (S.cur[kind] > S.peak[kind]) S.peak[kind] = S.cur[kind]; S.cur_all += len;
        if (kind == K_STAGE) { S.nstage++; if (len > S.stage_blk_max) S.stage_blk_max = len; }
        if (S.cur_all > S.peak_all) { S.peak_all = S.cur_all; memcpy(S.at_peak, S.cur, sizeof S.at_peak); S.nstage_at_peak = S.nstage; }
        pthread_mutex_unlock(&S.alloc_lock); return b->off;
    }
    size_t used = S.cur_all, need = ((used + len + S.mb_bytes) >> 20) + 1;   /* Phase 14 P2: name the size that would have held it (first fit: at least) */
    pthread_mutex_unlock(&S.alloc_lock);
    ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: the symmetric pool (%zu MiB, COMM_SHMEM_POOL_MB) cannot hold %zu MiB more (in use %zu MiB: control %zu, staging %zu, symmetric buffers %zu): "
             "COMM_SHMEM_POOL_MB >= %zu needed now; the model's need for this run: %s MiB (COMM_SHMEM_POOL_AUTO=1 sizes it at init)\n",
             S.me, S.pool_bytes >> 20, len >> 20, used >> 20, S.cur[K_CTRL] >> 20, S.cur[K_STAGE] >> 20, S.cur[K_SYM] >> 20, need, getenv("COMM_SHMEM_POOL_NEED_MB") ? getenv("COMM_SHMEM_POOL_NEED_MB") : "?");
}
static void pool_free(size_t off)
{
    pthread_mutex_lock(&S.alloc_lock);
    struct blk *p = 0, *head = S.blocks;
    if (S.nslot && off >= S.pool_bytes - (size_t)S.nslot * S.slot_bytes) head = S.slot[(off - (S.pool_bytes - (size_t)S.nslot * S.slot_bytes)) / S.slot_bytes];
    for (struct blk *b = head; b; p = b, b = b->next) if (b->off == off) {
        b->used = 0; S.cur[b->kind] -= b->len; S.cur_all -= b->len; if (b->kind == K_STAGE) S.nstage--;
        if (b->next && !b->next->used) { struct blk *n = b->next; b->len += n->len; b->next = n->next; free(n); }
        if (p && !p->used) { p->len += b->len; p->next = b->next; free(b); }
        break;
    }
    pthread_mutex_unlock(&S.alloc_lock);
}
static long *mailbox(int id, int pe) { return (long *)S.pool + (size_t)id * S.npes + pe; }
static int in_pool(const void *p) { return (const char *)p >= S.pool && (const char *)p < S.pool + S.pool_bytes; }
static size_t off_of(const void *p) { return (size_t)((const char *)p - S.pool); }

/* waits: the library's wait_until when threads may call it concurrently, else polled tests under the lock */
static void wait_ge_(long *p, long v)
{
    if (!S.serial) { shmem_long_wait_until(p, SHMEM_CMP_GE, v); return; }
    for (unsigned spins = 0;; spins++) {
        pthread_mutex_lock(&S.lock); int ok = shmem_long_test(p, SHMEM_CMP_GE, v); pthread_mutex_unlock(&S.lock);
        if (ok) return;
        if (spins > 64) sched_yield();
    }
}
static void wait_ge(long *p, long v) { double t0 = comm_wst_t0(); wait_ge_(p, v); comm_wst_ready_end(t0); }
static void wait_ne_(long *p, long v)
{
    if (!S.serial) { shmem_long_wait_until(p, SHMEM_CMP_NE, v); return; }
    for (unsigned spins = 0;; spins++) {
        pthread_mutex_lock(&S.lock); int ok = shmem_long_test(p, SHMEM_CMP_NE, v); pthread_mutex_unlock(&S.lock);
        if (ok) return;
        if (spins > 64) sched_yield();
    }
}
static void wait_ne(long *p, long v) { double t0 = comm_wst_t0(); wait_ne_(p, v); comm_wst_ready_end(t0); }
static int test_ge(long *p, long v) { SHM_LOCK(); int ok = shmem_long_test(p, SHMEM_CMP_GE, v); SHM_UNLOCK(); return ok; }

int comm_shmem_available(void) { return 1; }
/* Phase 14 V1: 1 when the pool will be shmem_malloc'd from the library's heap (OSHMEM, SOS without the device heap, Cray /
 * rocSHMEM whose heap is device memory but sized by the launch line), 0 when it is SOS's external heap (COMM_SHMEM_DEVHEAP=1
 * with the patch: a HIP buffer of the pool's own size) */
int comm_shmem_pool_in_heap(void)
{
#if defined(COMM_SHMEM_SOS) && defined(SHMEMX_EXTERNAL_HEAP_HOST) && !defined(COMM_HOST_ONLY)
#ifdef COMM_SHMEM_DEVICE_HEAP
    return 0;
#else
    return !(getenv("COMM_SHMEM_DEVHEAP") && atoi(getenv("COMM_SHMEM_DEVHEAP")));
#endif
#else
    return 1;
#endif
}
int comm_shmem_rank(void) { return S.me; }
int comm_shmem_size(void) { return S.npes; }
void comm_shmem_counters(double *t, long *n, double *b) { pthread_mutex_lock(&S.alloc_lock); *t = S.tv; *n = S.nv; *b = S.bv; pthread_mutex_unlock(&S.alloc_lock); }   /* Phase 16 D */
static int env_int(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }
#ifndef COMM_HOST_ONLY
/* the device buffer of a device-heap pool (COMM_SHMEM_DEVHEAP on SOS): fine-grained (host-accessible; =2 managed) on the current device */
static void *dev_pool_alloc(size_t bytes, int kind)
{
    void *p = 0;
    if (kind == 2) HIP_CHECK(hipMallocManaged(&p, bytes, hipMemAttachGlobal));
    else HIP_CHECK(hipExtMallocWithFlags(&p, bytes, hipDeviceMallocFinegrained));
    return p;
}
#endif
int comm_shmem_init(void)
{
    if (S.inited) return S.npes;
    int prov = -1;
    g_trace = getenv("COMM_SHMEM_TRACE") != 0;
    S.verbose = env_int("COMM_SHMEM_VERBOSE", 0); if (!S.verbose && env_int("ECALC_VERBOSE", 1) >= 2) S.verbose = 1;
    size_t mb = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192;
#ifdef COMM_SHMEM_DEVICE_HEAP
    S.devheap = 1;
#else
    S.devheap = env_int("COMM_SHMEM_DEVHEAP", 0);
#endif
    /* the mailbox needs the PE count, known only after init; sized for the launcher's count when it tells us, else for 4096 PEs */
    int npes_hint = getenv("SLURM_NTASKS") ? atoi(getenv("SLURM_NTASKS")) : getenv("PMI_SIZE") ? atoi(getenv("PMI_SIZE")) : getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 4096;
    if (npes_hint < 1) npes_hint = 4096;
    S.mb_bytes = ((size_t)MAXID * npes_hint * 8 + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    S.pool_bytes = (mb << 20) + S.mb_bytes;
#if defined(COMM_SHMEM_SOS) && defined(SHMEMX_EXTERNAL_HEAP_HOST) && !defined(COMM_HOST_ONLY)
    if (S.devheap) {                                      /* SOS with the external-heap patch: the pool is a HIP device buffer registered as the symmetric heap */
        shmemx_heap_preinit_thread(SHMEM_THREAD_MULTIPLE, &prov);
        S.pool = (char *)dev_pool_alloc(S.pool_bytes, S.devheap);
        shmemx_heap_create(S.pool, S.pool_bytes, SHMEMX_EXTERNAL_HEAP_HOST, -1);
        shmemx_heap_postinit();
        S.extheap = 1;
    } else
#endif
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &prov);
    S.prov = prov;
    S.me = shmem_my_pe(); S.npes = shmem_n_pes();
    if (S.npes > npes_hint) { fprintf(stderr, "comm_shmem: %d PEs but the mailbox was sized for %d (set COMM_SIZE)\n", S.npes, npes_hint); shmem_global_exit(1); }
    pthread_mutex_init(&S.lock, 0); pthread_mutex_init(&S.alloc_lock, 0);
    const char *e = getenv("COMM_SHMEM_SERIAL");
#if defined(COMM_SHMEM_OSHMEM)
    int serial_default = 1;                               /* OSHMEM 4.1's MULTIPLE is nominal (see the header) */
#else
    int serial_default = prov < SHMEM_THREAD_MULTIPLE;
#endif
    S.serial = e ? atoi(e) != 0 : serial_default;
    if (prov < SHMEM_THREAD_MULTIPLE && !S.serial) { if (S.me == 0) fprintf(stderr, "comm_shmem: the library provides thread level %d, not MULTIPLE: serialising the calls\n", prov); S.serial = 1; }
    const char *eo = getenv("COMM_SHMEM_ORDER");
    if (eo) S.order = !strcmp(eo, "putsig") ? ORDER_PUTSIG : !strcmp(eo, "fence") ? ORDER_FENCE : ORDER_QUIET;
    else if (getenv("COMM_SHMEM_FENCE") && atoi(getenv("COMM_SHMEM_FENCE"))) S.order = ORDER_FENCE;   /* Phase 11's switch */
    else S.order = HAVE_PUT_SIGNAL ? ORDER_PUTSIG : ORDER_QUIET;
    if (S.order == ORDER_PUTSIG && !HAVE_PUT_SIGNAL) { if (S.me == 0) fprintf(stderr, "comm_shmem: putsig needs OpenSHMEM 1.5 headers: using quiet\n"); S.order = ORDER_QUIET; }
    S.thread_always = env_int("COMM_SHMEM_THREAD", 0);
    S.spin_us = env_int("COMM_SHMEM_SPIN_US", 2000);
    S.xstats = env_int("COMM_XSTATS", 0) > 0;
    S.vslot_pool = env_int("COMM_LAYER_VSLOT_POOL", 0) > 0;   /* X2: no rounds for an exchange from and into the pool */
    { const char *eo = getenv("COMM_SHMEM_PEER_ORDER"); S.porder = eo && (!strcmp(eo, "rot") || !strcmp(eo, "1")); }   /* X1 (results/XEFF.md item 1); anything else: today's order */
    if (S.me == 0 && S.porder) printf("comm_shmem: COMM_SHMEM_PEER_ORDER=rot: rotated, ready-first peer service\n");
    S.keep_staging = env_int("COMM_SHMEM_KEEP_STAGING", 0);
    { const char *ep = getenv("COMM_SHMEM_PUT_MB"); double pm = ep ? atof(ep) : 0; S.put_bytes = pm > 0 ? (size_t)(pm * 1048576.0) : 0; }   /* Phase 16 C (results/C16.md): on Slingshot-11 one 4 MiB put runs at 14 GB/s per context, one 256 MiB put at 7.7 */
    { const char *er = getenv("COMM_SHMEM_ROUND_MB"); double rm = er ? atof(er) : 0; S.round_bytes = rm > 0 ? (size_t)(rm * 1048576.0) : 0; }   /* Phase 14 V1 */
    if (!S.extheap) {
        S.pool = (char *)shmem_malloc(S.pool_bytes);
        if (!S.pool) { fprintf(stderr, "comm_shmem: pe %d: shmem_malloc of %zu MiB failed: the SHMEM heap (SHMEM_SYMMETRIC_HEAP_SIZE / SHMEM_SYMMETRIC_SIZE) must be >= %zu MiB (the pool + 512)\n", S.me, S.pool_bytes >> 20, (S.pool_bytes >> 20) + 512); shmem_global_exit(1); }
    }
    memset(S.pool, 0, S.mb_bytes);                        /* the mailbox: 0 = empty */
#ifndef COMM_HOST_ONLY
#if !defined(COMM_SHMEM_SOS) && !defined(COMM_SHMEM_OSHMEM)
    if (S.devheap && !S.extheap) {                        /* Phase 16 A (results/A16.md): the generic branch takes COMM_SHMEM_DEVHEAP=1 at its word only when the
                                                           * library's heap is device memory; Cray OpenSHMEMX 11.8's heap is host hugepages (no HIP in libsma), and
                                                           * handing that to kernels as a device pool would fault -- so it is registered like the default host pool */
        hipPointerAttribute_t pa; hipError_t pe = hipPointerGetAttributes(&pa, S.pool);
        int dev = pe == hipSuccess && (pa.type == hipMemoryTypeDevice || pa.type == hipMemoryTypeManaged);
        (void)hipGetLastError();
        if (!dev) { if (S.me == 0) fprintf(stderr, "comm_shmem: COMM_SHMEM_DEVHEAP=%d but the library's shmem_malloc returned host memory (%s): the pool is HIP-registered host memory instead\n", S.devheap, pe == hipSuccess ? "known to HIP, not device" : hipGetErrorString(pe)); S.devheap = 0; }
    }
#endif
    if (!S.devheap) {
        hipError_t he = hipHostRegister(S.pool, S.pool_bytes, hipHostRegisterPortable);
        S.registered = he == hipSuccess;
        if (!S.registered) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies, no symmetric slabs\n", hipGetErrorString(he)); }
        else { void *dp = 0; if (hipHostGetDevicePointer(&dp, S.pool, 0) != hipSuccess || dp != S.pool) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: the pool's device pointer differs from its host pointer: no symmetric slabs\n"); S.registered = 2; } }
    } else if (S.extheap) {                               /* the other APUs of this process reach the device buffer through peer access */
        int nd = 0, cur = 0; HIP_CHECK(hipGetDeviceCount(&nd)); HIP_CHECK(hipGetDevice(&cur));
        for (int d = 0; d < nd; d++) if (d != cur) { HIP_CHECK(hipSetDevice(d)); hipError_t pe = hipDeviceEnablePeerAccess(cur, 0); if (pe != hipSuccess && pe != hipErrorPeerAccessAlreadyEnabled) { ec_fatal(EC_RC_FATAL, "comm_shmem: peer access %d -> %d: %s\n", d, cur, hipGetErrorString(pe)); } (void)hipGetLastError(); }
        HIP_CHECK(hipSetDevice(cur));
    }
#endif
    S.blocks = (struct blk *)malloc(sizeof *S.blocks); S.blocks->off = S.mb_bytes; S.blocks->len = S.pool_bytes - S.mb_bytes; S.blocks->used = 0; S.blocks->next = 0;
#ifndef COMM_HOST_ONLY
    {   /* Phase 14 V1: the four staging regions at the pool's end, when the rule sized them and the general area keeps >= 64 MiB */
        const char *es = getenv("COMM_SHMEM_STAGE_SLOT_MB"); size_t sb = es ? (size_t)(atof(es) * 1048576.0) : 0; sb = (sb + ALIGN - 1) & ~(size_t)(ALIGN - 1);
        if (sb && env_int("COMM_SHMEM_STAGE_SLOTS", 1) && 4 * sb + ((size_t)64 << 20) <= S.blocks->len) {
            S.nslot = 4; S.slot_bytes = sb; S.blocks->len -= 4 * sb;
            for (int i = 0; i < 4; i++) { struct blk *b = (struct blk *)malloc(sizeof *b); b->off = S.pool_bytes - (size_t)(4 - i) * sb; b->len = sb; b->used = 0; b->next = 0; S.slot[i] = b; }
        } else if (sb && S.me == 0) fprintf(stderr, "comm_shmem: no staging regions (%zu MiB each do not fit the pool beside 64 MiB): one first-fit area\n", sb >> 20);
    }
#endif
    shmem_barrier_all();                                  /* every mailbox is zeroed before anyone posts */
    S.inited = 1;
    if (S.me == 0 && S.round_bytes) printf("comm_shmem: COMM_SHMEM_ROUND_MB=%.6g: an all-to-all above it is carried in rounds of at most that staging each way\n", S.round_bytes / 1048576.0);
    if (S.me == 0 && S.nslot) printf("comm_shmem: staging in %d regions of %.1f MiB (one per APU) + a general area of %.1f MiB\n", S.nslot, S.slot_bytes / 1048576.0, S.blocks->len / 1048576.0);
    if (S.me == 0) printf("comm_shmem: %s, %d PEs, thread level %d (%s), pool %zu MiB%s%s, order %s%s, spin %ld us\n", comm_shmem_impl(), S.npes, prov, S.serial ? "calls serialised" : "concurrent, a context per communicator", S.pool_bytes >> 20,
                          S.extheap ? (S.devheap == 2 ? " (HIP managed, SOS external heap)" : " (HIP fine-grained device, SOS external heap)") : S.devheap ? " (device heap)" : "", S.registered == 1 ? ", HIP-registered" : "",
                          S.order == ORDER_PUTSIG ? "putsig" : S.order == ORDER_FENCE ? "fence" : "quiet", S.thread_always ? ", helper thread" : S.order == ORDER_QUIET ? " (helper thread)" : " (inline)", S.spin_us);
    return S.npes;
}
void comm_shmem_finalize(void)
{
    if (!S.inited) return;
    shmem_barrier_all();
    if (S.me == 0 || g_trace || S.verbose) {
        printf("comm_shmem: pe %d: pool peak %zu MiB of %zu (control %zu, staging %zu, symmetric buffers %zu MiB peaks)\n", S.me, S.peak_all >> 20, S.pool_bytes >> 20, S.peak[K_CTRL] >> 20, S.peak[K_STAGE] >> 20, S.peak[K_SYM] >> 20);
        /* Phase 14 P2: what occupied the pool at its peak (the kinds then, the live staging blocks), the largest staging block, the mailbox */
        printf("comm_shmem: pe %d: at the peak: control %.1f, staging %.1f (%zu blocks; the largest staging block of the run %.1f), symmetric buffers %.1f MiB; mailbox %.2f MiB\n", S.me,
               S.at_peak[K_CTRL] / 1048576.0, S.at_peak[K_STAGE] / 1048576.0, S.nstage_at_peak, S.stage_blk_max / 1048576.0, S.at_peak[K_SYM] / 1048576.0, S.mb_bytes / 1048576.0);
        if (S.verbose >= 2) site_print();
        printf("comm_shmem: pe %d: all-to-all %ld exchanges, %.2f s post to completion (summed over the APU threads), %.2f GB received = %.2f GB/s per APU thread", S.me, S.nv, S.tv, S.bv / 1e9, S.tv > 0 ? S.bv / 1e9 / S.tv : 0);   /* Phase 14 V1; Phase 16 A: the bytes */
        if (S.round_bytes) printf("; %ld through the rounds' path, %ld of them in %ld rounds (COMM_SHMEM_ROUND_MB=%.6g, the largest round's staging %.1f MiB send + recv)", S.nrpath, S.nround_ex, S.nrounds, S.round_bytes / 1048576.0, S.round_stage_max / 1048576.0);
        printf("\n");
    }
#ifndef COMM_HOST_ONLY
    if (S.registered) HIP_CHECK(hipHostUnregister(S.pool));
#endif
    if (S.xstats) xs_print();                             /* X1: every PE prints its own lines */
    comm_ofi_finalize();                                  /* Phase 17: the per-NIC bytes (COMM_OFI_VERBOSE), then the domains and pools */
    if (!S.extheap) shmem_free(S.pool);
    shmem_finalize(); S.inited = 0;
#ifndef COMM_HOST_ONLY
    if (S.extheap) HIP_CHECK(hipFree(S.pool));
#endif
}

/* ---- a communicator ---- */
/* the control block of a member (offsets from its base): the words other members write */
enum { W_ROFF, W_SIG, W_VAL0, W_VAL1, W_VSEQ0, W_VSEQ1, W_PROD, W_CONS, W_N };
typedef struct {
    int n, me, id, *pe;                    /* pe[r]: the PE of rank r */
    shmem_ctx_t ctx; int own_ctx;
    size_t base, *rbase, ring, ctrl_bytes; /* my block, the members' blocks, the ring bytes per source */
    long seq, tseq;                        /* exchanges; tiny exchanges */
    size_t sst, sst_cap, rst, rst_cap;     /* staging (send, receive): pool offsets, 0 = none */
    /* the pending exchange */
    int pending, v, thread, rin, next; pthread_t th; void *rb; hipStream_t st; size_t bytes; const size_t *rcnt, *rdsp; size_t *rpre, *spre;
    const char *src; const size_t *scnt, *sdsp;   /* the push's inputs: src + stride r (equal slabs; stride 0: one block to all) or src + sdsp[r], scnt[r] */
    size_t stride;
    long *sent, *got;                      /* point-to-point: bytes sent to / received from each rank */
    /* Phase 14 V1: the words' sequences per pair -- oseq[r] my sends to r (the roff word r writes into my block, my signal
     * into r's), iseq[r] r's sends to me.  An ordinary exchange advances every pair by one (so they equal seq); an exchange in
     * rounds advances a pair by its round count, which both ends derive from the pair's byte count alone */
    long *oseq, *iseq;
    size_t *acnt, *adsp;                   /* an equal exchange's counts and offsets for the rounds */
    int rounds, dev, rs_made; hipStream_t rs; size_t chunk, rsb_cap; const char *vsb; char *vrb; int vsin; double t0;   /* the pending exchange in rounds (helper thread, own stream) */
    /* Phase 17 (COMM_OFI=1): the communicator's libfabric data plane.  xo: the current exchange's data goes over it (the device ops);
     * db: the base its staging offsets, the published roff offsets and the in-pool test refer to (the SHMEM pool, or the device's
     * comm pool under xo); sdb: the pool the cached staging lives in; ocnt[r]: writes to r in flight; osig[r]: r's signal to send
     * once they completed (0: none) */
    int xk, xph, xapu; long xK; double xroff, xsig;   /* X1 xstats: the pending exchange's kind, phase, APU, rounds, and the seconds blocked on offset words (skew) / signal words (transfer, tail) */
    int ofi, xo, sdb; char *db; ofi_dev *od; ofi_peers *op; long *ocnt, *osig;
} shm_priv;
#define PRIV(c) ((shm_priv *)(c)->priv)
/* ---- X1: COMM_XSTATS=1 (results/XEFF.md M1): per (phase, kind) sums over the completed exchanges of this PE (all APU threads) ---- */
static struct xs_row { long n, rounds; double bytes, tpost, tcall, twait, roff, sig; } g_xs[WST_NP][XK_NK];
static struct xs_apu { long n; double bytes, tpost; } g_xa[8][XK_NK];
static const char *const xs_ph[WST_NP] = { "other", "bs", "dm", "recip" };
static const char *const xs_kn[XK_NK] = { "other", "redist", "result", "spill", "addsh", "layeq", "layv" };
static void xs_begin(shm_priv *p)
{
    p->xk = comm_xtag; if (p->xk < 0 || p->xk >= XK_NK) p->xk = 0;
    p->xph = comm_wst_get_phase(); p->xroff = p->xsig = 0; p->xK = 1; p->xapu = 0;
#ifndef COMM_HOST_ONLY
    { int d = 0; if (hipGetDevice(&d) == hipSuccess && d >= 0 && d < 8) p->xapu = d; }
#endif
}
/* at the completion: tw0 = when the caller entered s_wait, bytes = received, t = post to completion */
static void xs_end(shm_priv *p, double tw0, double bytes, double t)
{
    double now = now_s();
    pthread_mutex_lock(&S.alloc_lock);
    struct xs_row *x = &g_xs[p->xph][p->xk]; x->n++; x->rounds += p->xK; x->bytes += bytes; x->tpost += t; x->tcall += tw0 - p->t0; x->twait += now - tw0; x->roff += p->xroff; x->sig += p->xsig;
    struct xs_apu *a = &g_xa[p->xapu][p->xk]; a->n++; a->bytes += bytes; a->tpost += t;
    pthread_mutex_unlock(&S.alloc_lock);
}
static void xs_print(void)
{
    for (int ph = 0; ph < WST_NP; ph++) for (int k = 0; k < XK_NK; k++) { struct xs_row *x = &g_xs[ph][k]; if (!x->n) continue;
        printf("xstats pe %d phase %s kind %s: n %ld rounds %ld GB %.3f t_post %.3f s t_call %.3f s t_wait %.3f s roff_wait %.3f s sig_wait %.3f s (summed over the APU threads; t_call = post to the caller's wait, t_wait = blocked in wait)\n",
               S.me, xs_ph[ph], xs_kn[k], x->n, x->rounds, x->bytes / 1e9, x->tpost, x->tcall, x->twait, x->roff, x->sig); }
    for (int d = 0; d < 8; d++) for (int k = XK_LAYEQ; k < XK_NK; k++) { struct xs_apu *a = &g_xa[d][k]; if (!a->n) continue;
        printf("xstats pe %d apu %d kind %s: n %ld GB %.3f t_post %.3f s = %.2f GB/s\n", S.me, d, xs_kn[k], a->n, a->bytes / 1e9, a->tpost, a->tpost > 0 ? a->bytes / 1e9 / a->tpost : 0); }
}
/* a timed wait on a peer's offset word (skew) or signal word (transfer, tail) when COMM_XSTATS is on */
#define XWAIT(p, acc, w, v) do { if (S.xstats) { double xt_ = now_s(); wait_ge((w), (v)); (p)->acc += now_s() - xt_; } else wait_ge((w), (v)); } while (0)
static long *W(shm_priv *p, size_t base, int w, int r) { return (long *)(S.pool + base + ((size_t)w * p->n + r) * 8); }   /* word w of rank r in the block at base */
static char *RING(shm_priv *p, size_t base, int r) { return S.pool + base + (size_t)W_N * p->n * 8 + (size_t)r * p->ring; }
#define PACK(seq, off) (((long)(seq) << 40) | (long)(off))   /* a byte offset / count below 2^40 tagged with the sequence (< 2^23) */
#define UNSEQ(x) ((x) >> 40)
#define UNOFF(x) ((size_t)((x) & (((long)1 << 40) - 1)))
/* Phase 17: the data base of the current exchange -- on: a device op of an OFI communicator (its data over libfabric) */
static void set_xo(shm_priv *p, int on) { p->xo = on && p->ofi; p->db = p->xo ? comm_ofi_pool(p->od) : S.pool; }
static int in_db(shm_priv *p, const void *x) { return p->xo ? comm_ofi_in_pool(p->od, x) : in_pool(x); }
static size_t off_db(shm_priv *p, const void *x) { return (size_t)((const char *)x - p->db); }

static void put_word(shm_priv *p, int r, int w, int idx, long v)   /* word (w, idx) of rank r's block = v (ordered after the context's earlier puts by the caller's fence) */
{
    shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], w, idx), v, p->pe[r]);
}
static void putmem(shm_priv *p, int r, size_t off, const void *src, size_t n) { if (n) shmem_ctx_putmem_nbi(p->ctx, S.pool + off, src, n, p->pe[r]); }
/* the data of one peer and its signal, ordered: put-with-signal, or put + fence + signal; under `quiet` the signals are sent after one quiet (push_peers) */
static void put_signalled(shm_priv *p, int r, size_t off, const void *src, size_t n, long sig)
{
    long *sw = W(p, p->rbase[r], W_SIG, p->me);
    if (p->xo) { comm_ofi_write(p->op, r, off, src, n, &p->ocnt[r]); p->osig[r] = sig; return; }   /* Phase 17: the signal after delivery (ofi_flush) */
#if HAVE_PUT_SIGNAL
    if (S.order == ORDER_PUTSIG) {
        if (S.put_bytes && n > S.put_bytes) {                 /* Phase 16 C: the pieces before the last without a signal, a fence (delivery order to the PE), the last with it */
            size_t done = 0;
            while (n - done > S.put_bytes) { shmem_ctx_putmem_nbi(p->ctx, S.pool + off + done, (const char *)src + done, S.put_bytes, p->pe[r]); done += S.put_bytes; }
            shmem_ctx_fence(p->ctx);
            shmem_ctx_putmem_signal_nbi(p->ctx, S.pool + off + done, (const char *)src + done, n - done, (uint64_t *)sw, (uint64_t)sig, SHMEM_SIGNAL_SET, p->pe[r]);
            return;
        }
        if (n) shmem_ctx_putmem_signal_nbi(p->ctx, S.pool + off, src, n, (uint64_t *)sw, (uint64_t)sig, SHMEM_SIGNAL_SET, p->pe[r]);
        else shmem_ctx_long_p(p->ctx, sw, sig, p->pe[r]);
        return;
    }
#endif
    putmem(p, r, off, src, n);
    if (S.order == ORDER_FENCE) { shmem_ctx_fence(p->ctx); shmem_ctx_long_p(p->ctx, sw, sig, p->pe[r]); }
}
/* Phase 17: progress the device's writes; each peer whose writes have all completed (delivered) gets its signal word through SHMEM.
 * Returns when every recorded signal is sent (the data of this exchange is in the receivers' memory) */
static void ofi_flush(shm_priv *p)
{
    for (unsigned spins = 0;; spins++) {
        int left = 0;
        for (int r = 0; r < p->n; r++) if (p->osig[r]) {
            if (__atomic_load_n(&p->ocnt[r], __ATOMIC_ACQUIRE)) { left = 1; continue; }
            SHM_LOCK(); shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], W_SIG, p->me), p->osig[r], p->pe[r]); SHM_UNLOCK();
            p->osig[r] = 0;
        }
        if (!left) return;
        comm_ofi_progress(p->od);
        if (spins > 1024) sched_yield();
    }
}
static void order_ctx(shm_priv *p) { if (S.order == ORDER_FENCE) shmem_ctx_fence(p->ctx); else shmem_ctx_quiet(p->ctx); }   /* under SHM_LOCK: the words before their sequence (tiny exchange, rings) */

/* the staging of one exchange (send, receive): allocated per exchange, released when it completes (staging_release) --
 * a level's meshes live to the end of the run, and staging held per communicator would add up over the levels
 * (Q, Phase 12); pool-resident buffers (comm_sym_alloc) need none */
/* Phase 14 P2: COMM_SHMEM_VERBOSE=2 -- every staged exchange by its call site (the return addresses of the frames above the
 * transport, as object offsets for addr2line -f -e <object> <offset>): count, largest send and receive, printed at finalize */
#define NSITE 64
static struct site { void *key[5]; int n; long count; size_t smax, rmax; } g_site[NSITE]; static int g_nsite;
static __attribute__((noinline)) void site_note(size_t send, size_t recv)
{
    void *bt[9]; int nb = backtrace(bt, 9), n = nb - 2 < 5 ? nb - 2 : 5; if (n < 1) return;   /* bt[0] here, bt[1] staging (or the exchange, inlined): the key from bt[2] */
    pthread_mutex_lock(&S.alloc_lock);
    int i; for (i = 0; i < g_nsite; i++) if (g_site[i].n == n && !memcmp(g_site[i].key, bt + 2, n * sizeof(void *))) break;
    if (i == g_nsite && g_nsite < NSITE) { g_nsite++; memcpy(g_site[i].key, bt + 2, n * sizeof(void *)); g_site[i].n = n; }
    if (i < NSITE) { struct site *t = &g_site[i]; t->count++; if (send > t->smax) t->smax = send; if (recv > t->rmax) t->rmax = recv; }
    pthread_mutex_unlock(&S.alloc_lock);
}
static void site_print(void)
{
    for (int i = 0; i < g_nsite; i++) {
        struct site *t = &g_site[i]; char line[640]; int k = snprintf(line, sizeof line, "comm_shmem: pe %d: staged site %d: %ld exchanges, largest send %.1f recv %.1f MiB; callers", S.me, i, t->count, t->smax / 1048576.0, t->rmax / 1048576.0);
        for (int j = 0; j < t->n && k < (int)sizeof line - 48; j++) { Dl_info di; if (dladdr(t->key[j], &di) && di.dli_fbase) k += snprintf(line + k, sizeof line - k, " %s+0x%lx", strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname, (unsigned long)((char *)t->key[j] - (char *)di.dli_fbase - 1)); }
        printf("%s\n", line);
    }
}
static void stage_free(shm_priv *p, size_t off) { if (p->sdb) comm_ofi_free(p->od, off); else pool_free(off); }   /* Phase 17: in the pool it came from */
static void staging_drop(shm_priv *p) { if (p->sst) stage_free(p, p->sst); if (p->rst) stage_free(p, p->rst); p->sst = p->rst = 0; p->sst_cap = p->rst_cap = 0; }
static void staging(shm_priv *p, size_t send, size_t recv, const char *what)
{
    if ((p->sst || p->rst) && p->sdb != p->xo) staging_drop(p);   /* Phase 17: the cached staging is in the other pool */
    p->sdb = p->xo;
    if (send > p->sst_cap) { if (p->sst) stage_free(p, p->sst); p->sst = p->xo ? comm_ofi_alloc(p->od, send, 0) : pool_alloc(send, K_STAGE); p->sst_cap = send; }
    if (recv > p->rst_cap) { if (p->rst) stage_free(p, p->rst); p->rst = p->xo ? comm_ofi_alloc(p->od, recv, 0) : pool_alloc(recv, K_STAGE); p->rst_cap = recv; }
    if (S.verbose >= 2 && (send || recv)) site_note(send, recv);
    if (S.verbose && S.cur[K_STAGE] > S.stage_rep + S.stage_rep / 20 && (send || recv)) {   /* Phase 14 P2: which exchange raises the staging peak (racy read: a report, not the accounting) */
        S.stage_rep = S.cur[K_STAGE];
        printf("comm_shmem: pe %d: staging %.1f MiB in use (%zu blocks, pool %.1f MiB): comm %d (%d PEs) %s send %.1f + recv %.1f MiB\n", S.me, S.cur[K_STAGE] / 1048576.0, S.nstage, S.cur_all / 1048576.0,
               p->id, p->n, what, send / 1048576.0, recv / 1048576.0);
    }
}
static void staging_release(shm_priv *p)
{
    if (S.keep_staging) return;                           /* COMM_SHMEM_KEEP_STAGING=1: Phase 11's form, kept per communicator */
    staging_drop(p);
}
/* the sender's half of an exchange for the peers from `from`: wait for each receiver's offset (at most spin_us when
 * >= 0: returns the first peer not ready), put, signal.  Returns n when every peer is done. */
static int peer_at(const shm_priv *p, int i) { return (p->me + 1 + i) % p->n; }   /* X1: the rotated order, i < n - 1: rank r starts with r + 1 */
/* X1 COMM_SHMEM_PEER_ORDER=rot: the sender's half for the peers in `act` (n - 1 slots in the rotated order; the sender's byte count / source of
 * peer r by `post(r)`): post to whichever receiver's offset word has arrived, poll the rest, yield when a pass finds none.  The idle passes are
 * the skew (xroff). */
static void ready_first(shm_priv *p, long (*seqof)(const shm_priv *, int, long), long arg, int (*need)(const shm_priv *, int, long), void (*post)(shm_priv *, int, long, long), long x)
{
    int n1 = p->n - 1; if (n1 < 1) return;
    char *done = (char *)calloc((size_t)n1, 1); if (!done) die("calloc");
    int left = 0; for (int i = 0; i < n1; i++) { if (need(p, peer_at(p, i), arg)) left++; else done[i] = 1; }
    double tidle = 0; unsigned spins = 0;
    while (left) {
        int prog = 0;
        for (int i = 0; i < n1; i++) if (!done[i]) {
            int r = peer_at(p, i); long seq = seqof(p, r, arg), *w = W(p, p->base, W_ROFF, r);
            if (!test_ge(w, PACK(seq, 0))) continue;
            long xw = *(volatile long *)w; if (UNSEQ(xw) != seq) die("exchange sequence mismatch");
            if (S.xstats && tidle) { p->xroff += now_s() - tidle; tidle = 0; }
            post(p, r, xw, x); done[i] = 1; left--; prog = 1; spins = 0;
        }
        if (!prog) { if (S.xstats && !tidle) tidle = now_s(); if (++spins > 32) sched_yield(); }
    }
    if (S.xstats && tidle) p->xroff += now_s() - tidle;
    free(done);
}
static long rf_seq_plain(const shm_priv *p, int r, long a) { (void)a; return p->oseq[r]; }
static int rf_need_all(const shm_priv *p, int r, long a) { (void)p; (void)r; (void)a; return 1; }
static void rf_post_plain(shm_priv *p, int r, long xw, long a)
{
    (void)a; long seq = p->oseq[r];
    size_t n = p->scnt ? p->scnt[r] : p->bytes; const char *s = p->scnt ? p->src + p->sdsp[r] : p->src + p->stride * (size_t)r;
    SHM_LOCK(); put_signalled(p, r, UNOFF(xw), s, n, PACK(seq, n)); SHM_UNLOCK();
}
static int push_peers(shm_priv *p, int from, long spin_us)
{
    if (S.porder && spin_us < 0 && from == 0) ready_first(p, rf_seq_plain, 0, rf_need_all, rf_post_plain, 0);
    else for (int r = from; r < p->n; r++) if (r != p->me) {
        long seq = p->oseq[r], *w = W(p, p->base, W_ROFF, r);
        if (spin_us < 0) XWAIT(p, xroff, w, PACK(seq, 0));
        else { double t0 = 0; for (unsigned spins = 0; !test_ge(w, PACK(seq, 0)); spins++) { if (spins > 32) { double t = now_s(); if (!t0) t0 = t; else if ((t - t0) * 1e6 > (double)spin_us) return r; sched_yield(); } } }
        long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("exchange sequence mismatch");
        size_t n = p->scnt ? p->scnt[r] : p->bytes; const char *s = p->scnt ? p->src + p->sdsp[r] : p->src + p->stride * (size_t)r;
        SHM_LOCK(); put_signalled(p, r, UNOFF(x), s, n, PACK(seq, n)); SHM_UNLOCK();
    }
    if (p->xo) ofi_flush(p);                              /* Phase 17 */
    else if (S.order == ORDER_QUIET) {
        SHM_LOCK(); shmem_ctx_quiet(p->ctx);
        for (int r = from; r < p->n; r++) if (r != p->me) put_word(p, r, W_SIG, p->me, PACK(p->oseq[r], p->scnt ? p->scnt[r] : p->bytes));
        SHM_UNLOCK();
    }
    return p->n;
}
static void *pusher(void *a) { comm *c = (comm *)a; shm_priv *p = PRIV(c); push_peers(p, p->next, -1); return 0; }
/* post the pending exchange's puts: inline for the peers ready within the spin (only where nothing blocks: putsig /
 * fence), a helper thread for the rest */
static void start_push(comm *c)
{
    shm_priv *p = PRIV(c);
    p->next = 0;
    if (S.order != ORDER_QUIET && !S.thread_always && !p->xo && !S.porder) p->next = push_peers(p, 0, S.spin_us);   /* (Phase 17: an OFI push blocks until delivery: always the thread) */
    if (p->next < p->n) { if (pthread_create(&p->th, 0, pusher, c)) die("pthread_create"); p->thread = 1; }
}
/* the receiver's half: publish where every sender's slab lands: base + r * bytes, or base + the caller's / prefix offsets */
/* the next exchange: every pair's sequence + 1 */
static void seq_next(shm_priv *p)
{
    p->seq++;
    for (int r = 0; r < p->n; r++) if (r != p->me) { p->oseq[r]++; p->iseq[r]++; if (p->oseq[r] >= ((long)1 << 23) || p->iseq[r] >= ((long)1 << 23)) die("exchange sequence overflow (2^23 exchanges or rounds on one communicator)"); }
}
static void publish(shm_priv *p, size_t base, size_t bytes, const size_t *dsp)
{
    SHM_LOCK();
    for (int r = 0; r < p->n; r++) if (r != p->me) put_word(p, r, W_ROFF, p->me, PACK(p->iseq[r], base + (dsp ? dsp[r] : bytes * (size_t)r)));
    SHM_UNLOCK();
}
/* wait for every sender's signal (and count), then the context's completion (my send buffer is free again) */
static void arrive(shm_priv *p, size_t bytes, const size_t *rcnt)
{
    for (int r = 0; r < p->n; r++) if (r != p->me) {
        long *sw = W(p, p->base, W_SIG, r); XWAIT(p, xsig, sw, PACK(p->iseq[r], 0));
        long x = *(volatile long *)sw, want = (long)(rcnt ? rcnt[r] : bytes);
        if (UNSEQ(x) != p->iseq[r]) die("signal sequence mismatch");
        if ((long)UNOFF(x) != want) { ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch: rank %d sends %ld bytes to rank %d, which expects %ld\n", r, (long)UNOFF(x), p->me, want); }
    }
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();
}
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { return c->size; }
static int alltoallv_rounds(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int sin, int rin);   /* Phase 14 V1 (below) */
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoall while one is pending");
    set_xo(p, 1);
    int sin = in_db(p, sb), rin = in_db(p, rb);
    p->t0 = now_s(); if (S.xstats) xs_begin(p);
    if (S.round_bytes) {                                  /* Phase 14 V1: the equal exchange as an unequal one, in rounds when a slab exceeds the round's chunk */
        for (int r = 0; r < n; r++) { p->acnt[r] = bytes; p->adsp[r] = (size_t)r * bytes; }
        if (alltoallv_rounds(c, sb, p->acnt, p->adsp, rb, p->acnt, p->adsp, s, sin, rin)) return;
    }
    staging(p, sin ? 0 : bytes * n, rin ? 0 : bytes * n, "alltoall");
    seq_next(p); p->v = 0; p->bytes = bytes; p->stride = bytes; p->scnt = p->sdsp = 0; p->rb = rb; p->st = s; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : p->db + p->sst;
    TRACE("comm %d: alltoall seq %ld, %zu B per slab%s%s", p->id, p->seq, bytes, sin ? ", send in pool" : "", rin ? ", recv in pool" : "");
    if (!sin) COPY(p->db + p->sst, sb, bytes * n, s);                                         /* my slabs (all of them: simpler than skipping the self slab) */
    if ((const char *)sb + (size_t)me * bytes != (char *)rb + (size_t)me * bytes) COPY((char *)rb + (size_t)me * bytes, (const char *)sb + (size_t)me * bytes, bytes, s);   /* the self slab */
    SYNC(s);                                              /* the stream is done with sb and rb (a pool-resident rb may still be read by the caller's earlier kernels: publish only after) */
    publish(p, rin ? off_db(p, rb) : p->rst, bytes, 0);
    start_push(c);
}
/* ---- Phase 14 V1: the all-to-alls in rounds (COMM_SHMEM_ROUND_MB; results/V114.md; the equal one as an unequal one) ----
 * The staging of an ordinary exchange is its whole send and receive, so the pool's high-water is 4 APU threads x the largest
 * exchange's pair -- a second copy of buffers the callers already hold (results/P214.md 2).  With COMM_SHMEM_ROUND_MB = m, a
 * pair (s -> r) of b bytes goes in K = ceil(b / c) rounds of c = m / (n - 1) bytes (K = 1 when b <= c: then exactly the
 * ordinary exchange's words), so one round stages at most m each way whatever the exchange.  Both ends know b (scnt / rcnt,
 * checked by the signal), so both derive K and the pair's sequence advances by K on both -- no collective.  Round j: every
 * member publishes, per sender still active, where its chunk j lands (its slot in the receive staging, or the pool-resident
 * rb directly); stages its own chunks j; puts each after the receiver's word of round j; waits for the signals of round j;
 * quiets (the send staging is free again) and copies the received chunks out.  A member enters round j only after round j - 1
 * completed, and every member publishes before it blocks, so the rounds cannot deadlock.  The rounds run in a helper thread on
 * the communicator's own stream: the call returns at once, as the ordinary exchange does (comm_layered overlaps its intra
 * stage with the inter exchange), and s_wait joins it. */
static size_t round_chunk(const shm_priv *p)
{
    size_t c = S.round_bytes / (size_t)(p->n > 1 ? p->n - 1 : 1); c &= ~(size_t)(ALIGN - 1);
    return c < 4096 ? 4096 : c;
}
static long round_count(size_t b, size_t c) { return b <= c ? 1 : (long)((b + c - 1) / c); }
/* X1: the rounder's sender half in the ready-first order: round j = arg */
static long rf_seq_round(const shm_priv *p, int r, long j) { return p->oseq[r] + 1 + j; }
static int rf_need_round(const shm_priv *p, int r, long j) { return j < round_count(p->scnt[r], p->chunk); }
static void rf_post_round(shm_priv *p, int r, long xw, long j)
{
    size_t c = p->chunk, o = (size_t)j * c; const size_t *scnt = p->scnt, *sdsp = p->sdsp; long seq = p->oseq[r] + 1 + j;
    size_t len = scnt[r] > o ? (scnt[r] - o < c ? scnt[r] - o : c) : 0;
    const char *src = p->vsin ? p->vsb + sdsp[r] + o : p->db + p->sst + p->spre[r];
    SHM_LOCK(); put_signalled(p, r, UNOFF(xw), src, len, PACK(seq, len)); SHM_UNLOCK();
}
static void *rounder(void *a)
{
    shm_priv *p = (shm_priv *)a; int n = p->n, me = p->me; size_t c = p->chunk;
    const size_t *scnt = p->scnt, *sdsp = p->sdsp, *rcnt = p->rcnt, *rdsp = p->rdsp;
#ifndef COMM_HOST_ONLY
    HIP_CHECK(hipSetDevice(p->dev));
#endif
    long K = 1;
    for (int r = 0; r < n; r++) if (r != me) { long ko = round_count(scnt[r], c), ki = round_count(rcnt[r], c); if (ko > K) K = ko; if (ki > K) K = ki; }
    for (long j = 0; j < K; j++) {
        size_t o = (size_t)j * c;
        SHM_LOCK();                                       /* the receiver's half: where each active sender's chunk j lands */
        for (int r = 0; r < n; r++) if (r != me && j < round_count(rcnt[r], c))
            put_word(p, r, W_ROFF, me, PACK(p->iseq[r] + 1 + j, p->rin ? off_db(p, p->vrb) + rdsp[r] + o : p->rst + p->rpre[r]));
        SHM_UNLOCK();
        if (!p->vsin) {                                   /* my chunks j into the send staging (free: round j - 1's quiet) */
            int any = 0;
            for (int r = 0; r < n; r++) if (r != me && j < round_count(scnt[r], c) && scnt[r] > o) { size_t len = scnt[r] - o < c ? scnt[r] - o : c; COPY(p->db + p->sst + p->spre[r], p->vsb + sdsp[r] + o, len, p->rs); any = 1; }
            if (any) SYNC(p->rs);
        }
        if (S.porder) ready_first(p, rf_seq_round, j, rf_need_round, rf_post_round, j);   /* X1 */
        else for (int r = 0; r < n; r++) if (r != me && j < round_count(scnt[r], c)) {   /* the sender's half */
            long seq = p->oseq[r] + 1 + j, *w = W(p, p->base, W_ROFF, r); XWAIT(p, xroff, w, PACK(seq, 0));
            long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("round sequence mismatch");
            size_t len = scnt[r] > o ? (scnt[r] - o < c ? scnt[r] - o : c) : 0;
            const char *src = p->vsin ? p->vsb + sdsp[r] + o : p->db + p->sst + p->spre[r];
            SHM_LOCK(); put_signalled(p, r, UNOFF(x), src, len, PACK(seq, len)); SHM_UNLOCK();
        }
        if (p->xo) ofi_flush(p);                          /* Phase 17 */
        else if (S.order == ORDER_QUIET) {
            SHM_LOCK(); shmem_ctx_quiet(p->ctx);
            for (int r = 0; r < n; r++) if (r != me && j < round_count(scnt[r], c)) { size_t len = scnt[r] > o ? (scnt[r] - o < c ? scnt[r] - o : c) : 0; put_word(p, r, W_SIG, me, PACK(p->oseq[r] + 1 + j, len)); }
            SHM_UNLOCK();
        }
        int xany = 0;                                     /* X1: rot copies each peer's chunk out as its signal arrives (before the quiet) */
        if (S.porder) {
            int n1 = n - 1; char *done = (char *)calloc((size_t)(n1 > 0 ? n1 : 1), 1); if (!done) die("calloc");
            int left = 0; for (int i = 0; i < n1; i++) { if (j < round_count(rcnt[peer_at(p, i)], c)) left++; else done[i] = 1; }
            double tidle = 0; unsigned spins = 0;
            while (left) {
                int prog = 0;
                for (int i = 0; i < n1; i++) if (!done[i]) {
                    int r = peer_at(p, i); long seq = p->iseq[r] + 1 + j, *sw = W(p, p->base, W_SIG, r);
                    if (!test_ge(sw, PACK(seq, 0))) continue;
                    long x = *(volatile long *)sw; size_t want = rcnt[r] > o ? (rcnt[r] - o < c ? rcnt[r] - o : c) : 0;
                    if (UNSEQ(x) != seq) die("round signal sequence mismatch");
                    if (UNOFF(x) != want) ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch (round %ld): rank %d sends %ld bytes to rank %d, which expects %zu\n", j, r, (long)UNOFF(x), me, want);
                    if (S.xstats && tidle) { p->xsig += now_s() - tidle; tidle = 0; }
                    if (!p->rin && rcnt[r] > o) { COPY(p->vrb + rdsp[r] + o, p->db + p->rst + p->rpre[r], want, p->rs); xany = 1; }
                    done[i] = 1; left--; prog = 1; spins = 0;
                }
                if (!prog) { if (S.xstats && !tidle) tidle = now_s(); if (++spins > 32) sched_yield(); }
            }
            if (S.xstats && tidle) p->xsig += now_s() - tidle;
            free(done);
        } else
        for (int r = 0; r < n; r++) if (r != me && j < round_count(rcnt[r], c)) {   /* the signals of round j */
            long seq = p->iseq[r] + 1 + j, *sw = W(p, p->base, W_SIG, r); XWAIT(p, xsig, sw, PACK(seq, 0));
            long x = *(volatile long *)sw; size_t want = rcnt[r] > o ? (rcnt[r] - o < c ? rcnt[r] - o : c) : 0;
            if (UNSEQ(x) != seq) die("round signal sequence mismatch");
            if (UNOFF(x) != want) ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch (round %ld): rank %d sends %ld bytes to rank %d, which expects %zu\n", j, r, (long)UNOFF(x), me, want);
        }
        SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();   /* my puts of round j are complete: the send staging is free */
        if (S.porder) { if (xany) SYNC(p->rs); }
        else if (!p->rin) {
            int any = 0;
            for (int r = 0; r < n; r++) if (r != me && j < round_count(rcnt[r], c) && rcnt[r] > o) { size_t len = rcnt[r] - o < c ? rcnt[r] - o : c; COPY(p->vrb + rdsp[r] + o, p->db + p->rst + p->rpre[r], len, p->rs); any = 1; }
            if (any) SYNC(p->rs);                         /* before round j + 1 publishes the same slots */
        }
    }
    for (int r = 0; r < n; r++) if (r != me) { p->oseq[r] += round_count(scnt[r], c); p->iseq[r] += round_count(rcnt[r], c); }
    pthread_mutex_lock(&S.alloc_lock); S.nrpath++; if (K > 1) { S.nround_ex++; S.nrounds += K; } pthread_mutex_unlock(&S.alloc_lock);
    return 0;
}
/* the exchange through the rounds (under COMM_SHMEM_ROUND_MB; else 0: the ordinary exchange.  A pair of one round is the
 * ordinary exchange's words, so the two paths interoperate pair by pair) */
static int alltoallv_rounds(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int sin, int rin)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (!S.round_bytes || n < 2) return 0;
    size_t ch = round_chunk(p); long K = 1;
    for (int r = 0; r < n; r++) if (r != me) { long ko = round_count(scnt[r], ch), ki = round_count(rcnt[r], ch); if (ko > K) K = ko; if (ki > K) K = ki; }
    /* (K = 1 too: under the switch every all-to-all takes this path -- its staging leaves out the self slab, which the ordinary
     * path stages although it never leaves the node: half the staging at 2 PEs) */
    for (int r = 0; r < n; r++) if (r != me && (p->oseq[r] + K >= ((long)1 << 23) || p->iseq[r] + K >= ((long)1 << 23))) die("exchange sequence overflow (2^23 exchanges or rounds on one communicator)");
    size_t ss = 0, rs = 0;                                /* a slot per peer: its chunk (min(c, count)) */
    for (int r = 0; r < n; r++) { size_t a = r == me ? 0 : scnt[r] < ch ? scnt[r] : ch, b = r == me ? 0 : rcnt[r] < ch ? rcnt[r] : ch; p->spre[r] = ss; p->rpre[r] = rs; ss += a; rs += b; }
    staging(p, sin ? 0 : ss + 8, rin ? 0 : rs + 8, "alltoallv rounds");
    { size_t st = (sin ? 0 : ss) + (rin ? 0 : rs); pthread_mutex_lock(&S.alloc_lock); if (st > S.round_stage_max) S.round_stage_max = st; pthread_mutex_unlock(&S.alloc_lock); }
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    p->seq++;                                             /* (the pairs' sequences advance in the rounds) */
    p->xK = K; p->v = 1; p->rounds = 1; p->bytes = 0; p->rb = rb; p->st = s; p->rcnt = rcnt; p->rdsp = rdsp; p->scnt = scnt; p->sdsp = sdsp; p->rin = rin; p->pending = 1;
    p->vsb = (const char *)sb; p->vrb = (char *)rb; p->vsin = sin; p->chunk = ch;
#ifndef COMM_HOST_ONLY
    if (!p->rs_made) { HIP_CHECK(hipGetDevice(&p->dev)); HIP_CHECK(hipStreamCreateWithFlags(&p->rs, hipStreamNonBlocking)); p->rs_made = 1; }
#endif
    if (scnt[me] && (const char *)sb + sdsp[me] != (char *)rb + rdsp[me]) COPY((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me], s);
    SYNC(s);                                              /* the caller's stream is done with sb and rb */
    TRACE("comm %d: alltoallv in %ld rounds of %zu B per pair (staging %zu + %zu B)", p->id, K, ch, ss, rs);
    if (pthread_create(&p->th, 0, rounder, p)) die("pthread_create");
    p->thread = 1;
    return 1;
}
static void s_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv while an exchange is pending");
    set_xo(p, 1);
    int sin = in_db(p, sb), rin = in_db(p, rb);
    p->t0 = now_s(); if (S.xstats) xs_begin(p);
    if (!(sin && rin && S.vslot_pool) && alltoallv_rounds(c, sb, scnt, sdsp, rb, rcnt, rdsp, s, sin, rin)) return;   /* X2 COMM_LAYER_VSLOT_POOL: rounds only bound staging; an exchange from and into the pool stages nothing */
    size_t ts = comm_prefix(scnt, p->spre, n), tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, sin ? 0 : ts + 8, rin ? 0 : tr + 8, "alltoallv");
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    seq_next(p); p->v = 1; p->bytes = 0; p->rb = rb; p->st = s; p->rcnt = rcnt; p->rdsp = rdsp; p->scnt = scnt; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : p->db + p->sst; p->sdsp = sin ? sdsp : p->spre;
    if (!sin) for (int r = 0; r < n; r++) if (r != me && scnt[r]) COPY(p->db + p->sst + p->spre[r], (const char *)sb + sdsp[r], scnt[r], s);
    if (scnt[me] && (const char *)sb + sdsp[me] != (char *)rb + rdsp[me]) COPY((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me], s);
    SYNC(s);
    publish(p, rin ? off_db(p, rb) : p->rst, 0, rin ? rdsp : p->rpre);
    start_push(c);
}
/* Phase 16 A: the bytes an exchange brought in from the other PEs (alltoall: (n - 1) slabs; alltoallv: the counts) */
static double recv_bytes(const shm_priv *p)
{
    if (!p->v) return (double)(p->n - 1) * (double)p->bytes;
    double b = 0; for (int r = 0; r < p->n; r++) if (r != p->me) b += (double)p->rcnt[r]; return b;
}
static void s_wait(comm *c)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (!p->pending) return;
    double xtw0 = S.xstats ? now_s() : 0;                 /* X1 */
    TRACE("comm %d: wait seq %ld", p->id, p->seq);
    if (p->thread) { pthread_join(p->th, 0); p->thread = 0; }
    if (p->rounds) {                                      /* Phase 14 V1: the rounds completed in the helper thread */
        p->rounds = 0; staging_release(p); p->pending = 0;
        double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock);
        if (S.xstats) xs_end(p, xtw0, recv_bytes(p), t);
        return;
    }
    arrive(p, p->bytes, p->v ? p->rcnt : 0);
    TRACE("comm %d: wait seq %ld: arrived", p->id, p->seq);
    if (!p->rin) {
        if (!p->v) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)p->rb + (size_t)r * p->bytes, p->db + p->rst + (size_t)r * p->bytes, p->bytes, p->st); }
        else for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], p->db + p->rst + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    staging_release(p);
    p->pending = 0;
    { double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock); if (S.xstats) xs_end(p, xtw0, recv_bytes(p), t); }   /* Phase 14 V1 */
}
/* the host variants: complete on return; the source is the caller's buffer (a put may read private memory) */
static void s_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv_host while an exchange is pending");
    set_xo(p, 0);                                         /* (Phase 17: host buffers stay on SHMEM) */
    size_t tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, 0, tr + 8, "alltoallv_host");
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    if (scnt[me]) memmove((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me]);
    seq_next(p); p->src = (const char *)sb; p->scnt = scnt; p->sdsp = sdsp; p->bytes = 0;
    publish(p, p->rst, 0, p->rpre);
    push_peers(p, 0, -1);
    arrive(p, 0, rcnt);
    for (int r = 0; r < n; r++) if (r != me && rcnt[r]) memcpy((char *)rb + rdsp[r], S.pool + p->rst + p->rpre[r], rcnt[r]);
    staging_release(p);
}
static void s_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("allgather while an exchange is pending");
    set_xo(p, 0);
    staging(p, 0, bytes * n, "allgather_host");
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) memcpy(self, sb, bytes);
    seq_next(p); p->src = (const char *)sb; p->scnt = p->sdsp = 0; p->stride = 0; p->bytes = bytes;
    publish(p, p->rst, bytes, 0);
    push_peers(p, 0, -1);
    arrive(p, bytes, 0);
    for (int r = 0; r < n; r++) if (r != me) memcpy((char *)rb + (size_t)r * bytes, S.pool + p->rst + (size_t)r * bytes, bytes);
    staging_release(p);
}
static void s_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("allgather while an exchange is pending");
    set_xo(p, 1);
    int sin = in_db(p, sb), rin = in_db(p, rb);
    staging(p, sin ? 0 : bytes, rin ? 0 : bytes * n, "allgather");
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) COPY(self, sb, bytes, 0);
    if (!sin) COPY(p->db + p->sst, sb, bytes, 0);
    SYNC(0);
    seq_next(p); p->src = sin ? (const char *)sb : p->db + p->sst; p->scnt = p->sdsp = 0; p->stride = 0; p->bytes = bytes;
    publish(p, rin ? off_db(p, rb) : p->rst, bytes, 0);
    push_peers(p, 0, -1);
    arrive(p, bytes, 0);
    if (!rin) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)rb + (size_t)r * bytes, p->db + p->rst + (size_t)r * bytes, bytes, 0); SYNC(0); }
    staging_release(p);
}
/* the tiny exchange: every rank's 8-byte value to every rank (val, then fence, then the sequence word; the slot
 * alternates with the parity of the sequence so a fast rank's next value cannot overwrite one not yet read) */
static void tiny_exchange(comm *c, uint64_t v, uint64_t *all)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    long seq = ++p->tseq; int par = (int)(seq & 1);
    all[me] = v;
    SHM_LOCK();
    for (int r = 0; r < n; r++) if (r != me) put_word(p, r, W_VAL0 + par, me, (long)v);
    order_ctx(p);
    for (int r = 0; r < n; r++) if (r != me) put_word(p, r, W_VSEQ0 + par, me, seq);
    SHM_UNLOCK();
    for (int r = 0; r < n; r++) if (r != me) { wait_ge(W(p, p->base, W_VSEQ0 + par, r), seq); all[r] = (uint64_t)*(volatile long *)W(p, p->base, W_VAL0 + par, r); }
}
static void s_barrier(comm *c) { uint64_t *all = (uint64_t *)malloc((size_t)c->size * 8); tiny_exchange(c, 0, all); free(all); }
static uint64_t mulmod128(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
static uint64_t s_modq(comm *c, uint64_t v, uint64_t q, uint64_t w)
{
    uint64_t *all = (uint64_t *)malloc((size_t)c->size * 8), acc = 0, wr = 1;
    tiny_exchange(c, v, all);
    for (int r = 0; r < c->size; r++) { acc = (acc + mulmod128(all[r] % q, wr, q)) % q; wr = mulmod128(wr, w % q, q); }
    free(all); return acc;
}
static size_t s_max(comm *c, size_t v)
{
    uint64_t *all = (uint64_t *)malloc((size_t)c->size * 8), m = v; tiny_exchange(c, v, all);
    for (int r = 0; r < c->size; r++) if (all[r] > m) m = all[r];
    free(all); return (size_t)m;
}
/* point-to-point: a byte stream per (source, dest) through the dest's ring[source]; prod[source] at the dest counts the
 * bytes put, cons[dest] at the source the bytes the dest has taken out.  Sends of at most a ring never block on the
 * receiver (the carry flags: every rank sends to all before it receives from any) */
static void s_send(comm *c, int to, const void *b, size_t n)
{
    shm_priv *p = PRIV(c); const char *s = (const char *)b; size_t ring = p->ring;
    long *cons = W(p, p->base, W_CONS, to);
    while (n) {
        long sent = p->sent[to];
        size_t space = ring - (size_t)(sent - *(volatile long *)cons);
        if (!space) { wait_ge(cons, sent - (long)ring + 1); continue; }
        size_t k = n < space ? n : space, pos = (size_t)sent % ring; if (k > ring - pos) k = ring - pos;
        SHM_LOCK();
        putmem(p, to, (size_t)(RING(p, p->rbase[to], p->me) - S.pool) + pos, s, k);
        order_ctx(p);
        put_word(p, to, W_PROD, p->me, sent + (long)k);
        SHM_UNLOCK();
        p->sent[to] = sent + (long)k; s += k; n -= k;
    }
}
static void s_recv(comm *c, int from, void *b, size_t n)
{
    shm_priv *p = PRIV(c); char *d = (char *)b; size_t ring = p->ring;
    long *prod = W(p, p->base, W_PROD, from);
    while (n) {
        long got = p->got[from];
        wait_ge(prod, got + 1);
        size_t avail = (size_t)(*(volatile long *)prod - got), k = n < avail ? n : avail, pos = (size_t)got % ring; if (k > ring - pos) k = ring - pos;
        memcpy(d, RING(p, p->base, from) + pos, k);
        p->got[from] = got + (long)k; d += k; n -= k;
        SHM_LOCK(); put_word(p, from, W_CONS, p->me, got + (long)k); SHM_UNLOCK();
    }
}
/* S12: the callers' symmetric buffers (device-accessible: the registered host pool, or the device heap) */
static void *s_sym_alloc(comm *c, size_t bytes)
{
    shm_priv *op = PRIV(c);
    if (op->ofi) {                                        /* Phase 17: the device's comm pool (device-accessible, registered with the NICs) */
        if (getenv("COMM_SHMEM_NOSYM") && atoi(getenv("COMM_SHMEM_NOSYM"))) return 0;
        return comm_ofi_pool(op->od) + comm_ofi_alloc(op->od, bytes, 1);
    }
#ifndef COMM_HOST_ONLY
    if (!S.devheap && S.registered != 1) return 0;        /* not device-accessible: the caller allocates, the transport stages */
#endif
    if (getenv("COMM_SHMEM_NOSYM") && atoi(getenv("COMM_SHMEM_NOSYM"))) return 0;
    size_t off = pool_alloc(bytes, K_SYM);
    TRACE("sym_alloc %zu MiB at %zu", bytes >> 20, off);
    return S.pool + off;
}
static void s_sym_free(comm *c, void *ptr)
{
    (void)c;
    { ofi_dev *od = comm_ofi_owner(ptr); if (od) { comm_ofi_free(od, (size_t)((char *)ptr - comm_ofi_pool(od))); return; } }   /* Phase 17 */
    if (!in_pool(ptr)) die("sym_free of a pointer outside the pool");
    pool_free(off_of(ptr));
}
static void s_destroy(comm *c)
{
    shm_priv *p = PRIV(c);
    if (p->pending) s_wait(c);
    TRACE("destroy comm id %d: barrier", p->id);
    s_barrier(c);                                          /* nobody's block goes while a member may still write to it */
    TRACE("destroy comm id %d: quiet + ctx destroy", p->id);
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); if (p->own_ctx) shmem_ctx_destroy(p->ctx); SHM_UNLOCK();
    TRACE("destroy comm id %d: done", p->id);
    staging_drop(p);
    comm_ofi_peers_free(p->op); free(p->ocnt); free(p->osig);   /* Phase 17 */
    pool_free(p->base);
#ifndef COMM_HOST_ONLY
    if (p->rs_made) HIP_CHECK(hipStreamDestroy(p->rs));
#endif
    free(p->pe); free(p->rbase); free(p->rpre); free(p->spre); free(p->sent); free(p->got); free(p->oseq); free(p->iseq); free(p->acnt); free(p); free(c);
}
static const struct comm_ops shm_ops = { s_rank, s_size, s_alltoall, s_wait, s_barrier, s_modq, s_max, s_destroy, s_send, s_recv, s_allgather, s_allgather_host, s_alltoallv, s_alltoallv_host, s_sym_alloc, s_sym_free };

/* the communicator of the strided PE set {pe_start + pe_stride r, r < n} with the run-unique id (its mailbox row); all
 * members call together (several may be created at once from different threads with different ids) */
comm *comm_shmem_create_at(int pe_start, int pe_stride, int n, int id)
{
    if (!S.inited) comm_shmem_init();
    if (n < 1 || pe_stride < 1 || pe_start < 0 || pe_start + (n - 1) * pe_stride >= S.npes || id < 0 || id >= MAXID) die("bad PE set");
    int me = -1; for (int r = 0; r < n; r++) if (pe_start + r * pe_stride == S.me) me = r;
    if (me < 0) die("this PE is not in the set");
    comm *c = (comm *)calloc(1, sizeof *c); shm_priv *p = (shm_priv *)calloc(1, sizeof *p);
    c->ops = &shm_ops; c->priv = p; c->rank = me; c->size = n; c->inflight = 1;
    p->n = n; p->me = me; p->id = id; p->pe = (int *)malloc(n * sizeof(int)); for (int r = 0; r < n; r++) p->pe[r] = pe_start + r * pe_stride;
    p->ring = (getenv("COMM_SHMEM_RING_KB") ? (size_t)atol(getenv("COMM_SHMEM_RING_KB")) : 256) << 10; if (p->ring < 4096) p->ring = 4096;
    p->ctrl_bytes = (size_t)W_N * n * 8 + (size_t)n * p->ring;
    p->base = pool_alloc(p->ctrl_bytes, K_CTRL); memset(S.pool + p->base, 0, (size_t)W_N * n * 8);
    p->rbase = (size_t *)calloc(n, sizeof(size_t)); p->rpre = (size_t *)calloc(n, sizeof(size_t)); p->spre = (size_t *)calloc(n, sizeof(size_t));
    p->sent = (long *)calloc(n, sizeof(long)); p->got = (long *)calloc(n, sizeof(long));
    p->oseq = (long *)calloc(n, sizeof(long)); p->iseq = (long *)calloc(n, sizeof(long));
    p->acnt = (size_t *)calloc(2 * (size_t)n, sizeof(size_t)); p->adsp = p->acnt + n;
    SHM_LOCK();
    p->own_ctx = !S.serial && shmem_ctx_create(0, &p->ctx) == 0;   /* one context per communicator (= per APU thread) when the calls run concurrently; under the lock the default context serves (and OSHMEM 4.1 loses puts on a context created after another was destroyed) */
    if (!p->own_ctx) p->ctx = SHMEM_CTX_DEFAULT;
    if (*mailbox(id, S.me) != 0) { SHM_UNLOCK(); ec_fatal(EC_RC_FATAL, "comm_shmem: communicator id %d used twice\n", id); }
    for (int r = 0; r < n; r++) shmem_ctx_long_p(p->ctx, mailbox(id, S.me), (long)p->base + 1, p->pe[r]);   /* my block's offset into every member's row (mine included), indexed by PE */
    shmem_ctx_quiet(p->ctx);
    SHM_UNLOCK();
    TRACE("create comm id %d (%d PEs from %d stride %d): posted, waiting for the members", id, n, pe_start, pe_stride);
    for (int r = 0; r < n; r++) { long *m = mailbox(id, p->pe[r]); wait_ne(m, 0); p->rbase[r] = (size_t)(*(volatile long *)m - 1); }
    set_xo(p, 0);
    if (comm_ofi_enabled()) {                             /* Phase 17: the device's NICs and comm pool; the members' blobs over SHMEM */
        int dev = 0;
#ifdef COMM_HOST_ONLY
        dev = getenv("COMM_OFI_DEV") ? atoi(getenv("COMM_OFI_DEV")) : id % 4;   /* the test build: a communicator id stands for the APU thread */
#else
        HIP_CHECK(hipGetDevice(&dev));
#endif
        p->od = comm_ofi_dev(dev);
        char *blobs = (char *)malloc((size_t)n * COMM_OFI_BLOB); comm_ofi_blob(p->od, blobs + (size_t)me * COMM_OFI_BLOB);
        s_allgather_host(c, blobs + (size_t)me * COMM_OFI_BLOB, blobs, COMM_OFI_BLOB);
        p->op = comm_ofi_peers_new(p->od, n, me, blobs); free(blobs);
        p->ocnt = (long *)calloc(n, sizeof(long)); p->osig = (long *)calloc(n, sizeof(long));
        p->ofi = 1;
    }
    TRACE("create comm id %d: done", id);
    return c;
}
#endif
