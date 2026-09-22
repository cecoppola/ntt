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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>
#include "comm.h"
#ifndef COMM_SHMEM
static void no_shmem(void) { fprintf(stderr, "comm_shmem: built without SHMEM (make SHMEM=1 with oshcc / SHMEM_HOME=<SOS prefix> / the target's SHMEM)\n"); exit(1); }
int   comm_shmem_init(void) { no_shmem(); return 0; }
int   comm_shmem_rank(void) { return 0; }
int   comm_shmem_size(void) { return 1; }
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
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
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
    long spin_us;
    char *pool; size_t pool_bytes, mb_bytes;   /* the symmetric pool; the mailbox at [0, mb_bytes) */
    pthread_mutex_t lock;                  /* SHM_LOCK: the library in serial mode; the allocator always (alloc_lock) */
    pthread_mutex_t alloc_lock;
    struct blk *blocks;
    size_t cur[3], peak[3], cur_all, peak_all;   /* pool accounting by kind: 0 control blocks, 1 staging, 2 the callers' symmetric buffers */
} S;
enum { K_CTRL, K_STAGE, K_SYM };
#define SHM_LOCK()   do { if (S.serial) pthread_mutex_lock(&S.lock); } while (0)
#define SHM_UNLOCK() do { if (S.serial) pthread_mutex_unlock(&S.lock); } while (0)
static int g_trace;
#define TRACE(...) do { if (g_trace) { fprintf(stderr, "comm_shmem: pe %d: ", S.me); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static void die(const char *m) { fprintf(stderr, "comm_shmem: pe %d: %s\n", S.me, m); exit(1); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
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

/* the pool's first-fit allocator (offsets; a block list sorted by offset, coalesced on free) */
static size_t pool_alloc(size_t len, int kind)
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&S.alloc_lock);
    for (struct blk *b = S.blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1; b->kind = kind;
        S.cur[kind] += len; if (S.cur[kind] > S.peak[kind]) S.peak[kind] = S.cur[kind]; S.cur_all += len; if (S.cur_all > S.peak_all) S.peak_all = S.cur_all;
        pthread_mutex_unlock(&S.alloc_lock); return b->off;
    }
    pthread_mutex_unlock(&S.alloc_lock);
    fprintf(stderr, "comm_shmem: pe %d: the symmetric pool (%zu MiB, COMM_SHMEM_POOL_MB) cannot hold %zu MiB more\n", S.me, S.pool_bytes >> 20, len >> 20); exit(1);
}
static void pool_free(size_t off)
{
    pthread_mutex_lock(&S.alloc_lock);
    struct blk *p = 0;
    for (struct blk *b = S.blocks; b; p = b, b = b->next) if (b->off == off) {
        b->used = 0; S.cur[b->kind] -= b->len; S.cur_all -= b->len;
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
static void wait_ge(long *p, long v)
{
    if (!S.serial) { shmem_long_wait_until(p, SHMEM_CMP_GE, v); return; }
    for (unsigned spins = 0;; spins++) {
        pthread_mutex_lock(&S.lock); int ok = shmem_long_test(p, SHMEM_CMP_GE, v); pthread_mutex_unlock(&S.lock);
        if (ok) return;
        if (spins > 64) sched_yield();
    }
}
static void wait_ne(long *p, long v)
{
    if (!S.serial) { shmem_long_wait_until(p, SHMEM_CMP_NE, v); return; }
    for (unsigned spins = 0;; spins++) {
        pthread_mutex_lock(&S.lock); int ok = shmem_long_test(p, SHMEM_CMP_NE, v); pthread_mutex_unlock(&S.lock);
        if (ok) return;
        if (spins > 64) sched_yield();
    }
}
static int test_ge(long *p, long v) { SHM_LOCK(); int ok = shmem_long_test(p, SHMEM_CMP_GE, v); SHM_UNLOCK(); return ok; }

int comm_shmem_available(void) { return 1; }
int comm_shmem_rank(void) { return S.me; }
int comm_shmem_size(void) { return S.npes; }
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
    S.keep_staging = env_int("COMM_SHMEM_KEEP_STAGING", 0);
    if (!S.extheap) {
        S.pool = (char *)shmem_malloc(S.pool_bytes);
        if (!S.pool) { fprintf(stderr, "comm_shmem: pe %d: shmem_malloc of %zu MiB failed (SHMEM_SYMMETRIC_HEAP_SIZE / SHMEM_SYMMETRIC_SIZE?)\n", S.me, S.pool_bytes >> 20); shmem_global_exit(1); }
    }
    memset(S.pool, 0, S.mb_bytes);                        /* the mailbox: 0 = empty */
#ifndef COMM_HOST_ONLY
    if (!S.devheap) {
        hipError_t he = hipHostRegister(S.pool, S.pool_bytes, hipHostRegisterPortable);
        S.registered = he == hipSuccess;
        if (!S.registered) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies, no symmetric slabs\n", hipGetErrorString(he)); }
        else { void *dp = 0; if (hipHostGetDevicePointer(&dp, S.pool, 0) != hipSuccess || dp != S.pool) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: the pool's device pointer differs from its host pointer: no symmetric slabs\n"); S.registered = 2; } }
    } else if (S.extheap) {                               /* the other APUs of this process reach the device buffer through peer access */
        int nd = 0, cur = 0; HIP_CHECK(hipGetDeviceCount(&nd)); HIP_CHECK(hipGetDevice(&cur));
        for (int d = 0; d < nd; d++) if (d != cur) { HIP_CHECK(hipSetDevice(d)); hipError_t pe = hipDeviceEnablePeerAccess(cur, 0); if (pe != hipSuccess && pe != hipErrorPeerAccessAlreadyEnabled) { fprintf(stderr, "comm_shmem: peer access %d -> %d: %s\n", d, cur, hipGetErrorString(pe)); exit(1); } (void)hipGetLastError(); }
        HIP_CHECK(hipSetDevice(cur));
    }
#endif
    S.blocks = (struct blk *)malloc(sizeof *S.blocks); S.blocks->off = S.mb_bytes; S.blocks->len = S.pool_bytes - S.mb_bytes; S.blocks->used = 0; S.blocks->next = 0;
    shmem_barrier_all();                                  /* every mailbox is zeroed before anyone posts */
    S.inited = 1;
    if (S.me == 0) printf("comm_shmem: %s, %d PEs, thread level %d (%s), pool %zu MiB%s%s, order %s%s, spin %ld us\n", comm_shmem_impl(), S.npes, prov, S.serial ? "calls serialised" : "concurrent, a context per communicator", S.pool_bytes >> 20,
                          S.extheap ? (S.devheap == 2 ? " (HIP managed, SOS external heap)" : " (HIP fine-grained device, SOS external heap)") : S.devheap ? " (device heap)" : "", S.registered == 1 ? ", HIP-registered" : "",
                          S.order == ORDER_PUTSIG ? "putsig" : S.order == ORDER_FENCE ? "fence" : "quiet", S.thread_always ? ", helper thread" : S.order == ORDER_QUIET ? " (helper thread)" : " (inline)", S.spin_us);
    return S.npes;
}
void comm_shmem_finalize(void)
{
    if (!S.inited) return;
    shmem_barrier_all();
    if (S.me == 0 || g_trace) printf("comm_shmem: pe %d: pool peak %zu MiB of %zu (control %zu, staging %zu, symmetric buffers %zu MiB peaks)\n", S.me, S.peak_all >> 20, S.pool_bytes >> 20, S.peak[K_CTRL] >> 20, S.peak[K_STAGE] >> 20, S.peak[K_SYM] >> 20);
#ifndef COMM_HOST_ONLY
    if (S.registered) HIP_CHECK(hipHostUnregister(S.pool));
#endif
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
} shm_priv;
#define PRIV(c) ((shm_priv *)(c)->priv)
static long *W(shm_priv *p, size_t base, int w, int r) { return (long *)(S.pool + base + ((size_t)w * p->n + r) * 8); }   /* word w of rank r in the block at base */
static char *RING(shm_priv *p, size_t base, int r) { return S.pool + base + (size_t)W_N * p->n * 8 + (size_t)r * p->ring; }
#define PACK(seq, off) (((long)(seq) << 40) | (long)(off))   /* a byte offset / count below 2^40 tagged with the sequence (< 2^23) */
#define UNSEQ(x) ((x) >> 40)
#define UNOFF(x) ((size_t)((x) & (((long)1 << 40) - 1)))

static void put_word(shm_priv *p, int r, int w, int idx, long v)   /* word (w, idx) of rank r's block = v (ordered after the context's earlier puts by the caller's fence) */
{
    shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], w, idx), v, p->pe[r]);
}
static void putmem(shm_priv *p, int r, size_t off, const void *src, size_t n) { if (n) shmem_ctx_putmem_nbi(p->ctx, S.pool + off, src, n, p->pe[r]); }
/* the data of one peer and its signal, ordered: put-with-signal, or put + fence + signal; under `quiet` the signals are sent after one quiet (push_peers) */
static void put_signalled(shm_priv *p, int r, size_t off, const void *src, size_t n, long sig)
{
    long *sw = W(p, p->rbase[r], W_SIG, p->me);
#if HAVE_PUT_SIGNAL
    if (S.order == ORDER_PUTSIG) {
        if (n) shmem_ctx_putmem_signal_nbi(p->ctx, S.pool + off, src, n, (uint64_t *)sw, (uint64_t)sig, SHMEM_SIGNAL_SET, p->pe[r]);
        else shmem_ctx_long_p(p->ctx, sw, sig, p->pe[r]);
        return;
    }
#endif
    putmem(p, r, off, src, n);
    if (S.order == ORDER_FENCE) { shmem_ctx_fence(p->ctx); shmem_ctx_long_p(p->ctx, sw, sig, p->pe[r]); }
}
static void order_ctx(shm_priv *p) { if (S.order == ORDER_FENCE) shmem_ctx_fence(p->ctx); else shmem_ctx_quiet(p->ctx); }   /* under SHM_LOCK: the words before their sequence (tiny exchange, rings) */

/* the staging of one exchange (send, receive): allocated per exchange, released when it completes (staging_release) --
 * a level's meshes live to the end of the run, and staging held per communicator would add up over the levels
 * (Q, Phase 12); pool-resident buffers (comm_sym_alloc) need none */
static void staging(shm_priv *p, size_t send, size_t recv)
{
    if (send > p->sst_cap) { if (p->sst) pool_free(p->sst); p->sst = pool_alloc(send, K_STAGE); p->sst_cap = send; }
    if (recv > p->rst_cap) { if (p->rst) pool_free(p->rst); p->rst = pool_alloc(recv, K_STAGE); p->rst_cap = recv; }
}
static void staging_release(shm_priv *p)
{
    if (S.keep_staging) return;                           /* COMM_SHMEM_KEEP_STAGING=1: Phase 11's form, kept per communicator */
    if (p->sst) pool_free(p->sst);
    if (p->rst) pool_free(p->rst);
    p->sst = p->rst = 0; p->sst_cap = p->rst_cap = 0;
}
/* the sender's half of an exchange for the peers from `from`: wait for each receiver's offset (at most spin_us when
 * >= 0: returns the first peer not ready), put, signal.  Returns n when every peer is done. */
static int push_peers(shm_priv *p, int from, long spin_us)
{
    long seq = p->seq;
    for (int r = from; r < p->n; r++) if (r != p->me) {
        long *w = W(p, p->base, W_ROFF, r);
        if (spin_us < 0) wait_ge(w, PACK(seq, 0));
        else { double t0 = 0; for (unsigned spins = 0; !test_ge(w, PACK(seq, 0)); spins++) { if (spins > 32) { double t = now_s(); if (!t0) t0 = t; else if ((t - t0) * 1e6 > (double)spin_us) return r; sched_yield(); } } }
        long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("exchange sequence mismatch");
        size_t n = p->scnt ? p->scnt[r] : p->bytes; const char *s = p->scnt ? p->src + p->sdsp[r] : p->src + p->stride * (size_t)r;
        SHM_LOCK(); put_signalled(p, r, UNOFF(x), s, n, PACK(seq, n)); SHM_UNLOCK();
    }
    if (S.order == ORDER_QUIET) {
        SHM_LOCK(); shmem_ctx_quiet(p->ctx);
        for (int r = from; r < p->n; r++) if (r != p->me) put_word(p, r, W_SIG, p->me, PACK(seq, p->scnt ? p->scnt[r] : p->bytes));
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
    if (S.order != ORDER_QUIET && !S.thread_always) p->next = push_peers(p, 0, S.spin_us);
    if (p->next < p->n) { if (pthread_create(&p->th, 0, pusher, c)) die("pthread_create"); p->thread = 1; }
}
/* the receiver's half: publish where every sender's slab lands: base + r * bytes, or base + the caller's / prefix offsets */
static void publish(shm_priv *p, size_t base, size_t bytes, const size_t *dsp)
{
    if (p->seq >= ((long)1 << 23)) die("exchange sequence overflow (2^23 exchanges on one communicator)");
    SHM_LOCK();
    for (int r = 0; r < p->n; r++) if (r != p->me) put_word(p, r, W_ROFF, p->me, PACK(p->seq, base + (dsp ? dsp[r] : bytes * (size_t)r)));
    SHM_UNLOCK();
}
/* wait for every sender's signal (and count), then the context's completion (my send buffer is free again) */
static void arrive(shm_priv *p, size_t bytes, const size_t *rcnt)
{
    for (int r = 0; r < p->n; r++) if (r != p->me) {
        long *sw = W(p, p->base, W_SIG, r); wait_ge(sw, PACK(p->seq, 0));
        long x = *(volatile long *)sw, want = (long)(rcnt ? rcnt[r] : bytes);
        if (UNSEQ(x) != p->seq) die("signal sequence mismatch");
        if ((long)UNOFF(x) != want) { fprintf(stderr, "comm_shmem: alltoallv count mismatch: rank %d sends %ld bytes to rank %d, which expects %ld\n", r, (long)UNOFF(x), p->me, want); exit(1); }
    }
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();
}
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { return c->size; }
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoall while one is pending");
    int sin = in_pool(sb), rin = in_pool(rb);
    staging(p, sin ? 0 : bytes * n, rin ? 0 : bytes * n);
    p->seq++; p->v = 0; p->bytes = bytes; p->stride = bytes; p->scnt = p->sdsp = 0; p->rb = rb; p->st = s; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : S.pool + p->sst;
    TRACE("comm %d: alltoall seq %ld, %zu B per slab%s%s", p->id, p->seq, bytes, sin ? ", send in pool" : "", rin ? ", recv in pool" : "");
    if (!sin) COPY(S.pool + p->sst, sb, bytes * n, s);                                         /* my slabs (all of them: simpler than skipping the self slab) */
    if ((const char *)sb + (size_t)me * bytes != (char *)rb + (size_t)me * bytes) COPY((char *)rb + (size_t)me * bytes, (const char *)sb + (size_t)me * bytes, bytes, s);   /* the self slab */
    SYNC(s);                                              /* the stream is done with sb and rb (a pool-resident rb may still be read by the caller's earlier kernels: publish only after) */
    publish(p, rin ? off_of(rb) : p->rst, bytes, 0);
    start_push(c);
}
static void s_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv while an exchange is pending");
    int sin = in_pool(sb), rin = in_pool(rb);
    size_t ts = comm_prefix(scnt, p->spre, n), tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, sin ? 0 : ts + 8, rin ? 0 : tr + 8);
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    p->seq++; p->v = 1; p->bytes = 0; p->rb = rb; p->st = s; p->rcnt = rcnt; p->rdsp = rdsp; p->scnt = scnt; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : S.pool + p->sst; p->sdsp = sin ? sdsp : p->spre;
    if (!sin) for (int r = 0; r < n; r++) if (r != me && scnt[r]) COPY(S.pool + p->sst + p->spre[r], (const char *)sb + sdsp[r], scnt[r], s);
    if (scnt[me] && (const char *)sb + sdsp[me] != (char *)rb + rdsp[me]) COPY((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me], s);
    SYNC(s);
    publish(p, rin ? off_of(rb) : p->rst, 0, rin ? rdsp : p->rpre);
    start_push(c);
}
static void s_wait(comm *c)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (!p->pending) return;
    TRACE("comm %d: wait seq %ld", p->id, p->seq);
    if (p->thread) { pthread_join(p->th, 0); p->thread = 0; }
    arrive(p, p->bytes, p->v ? p->rcnt : 0);
    TRACE("comm %d: wait seq %ld: arrived", p->id, p->seq);
    if (!p->rin) {
        if (!p->v) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)p->rb + (size_t)r * p->bytes, S.pool + p->rst + (size_t)r * p->bytes, p->bytes, p->st); }
        else for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], S.pool + p->rst + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    staging_release(p);
    p->pending = 0;
}
/* the host variants: complete on return; the source is the caller's buffer (a put may read private memory) */
static void s_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv_host while an exchange is pending");
    size_t tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, 0, tr + 8);
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    if (scnt[me]) memmove((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me]);
    p->seq++; p->src = (const char *)sb; p->scnt = scnt; p->sdsp = sdsp; p->bytes = 0;
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
    staging(p, 0, bytes * n);
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) memcpy(self, sb, bytes);
    p->seq++; p->src = (const char *)sb; p->scnt = p->sdsp = 0; p->stride = 0; p->bytes = bytes;
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
    int sin = in_pool(sb), rin = in_pool(rb);
    staging(p, sin ? 0 : bytes, rin ? 0 : bytes * n);
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) COPY(self, sb, bytes, 0);
    if (!sin) COPY(S.pool + p->sst, sb, bytes, 0);
    SYNC(0);
    p->seq++; p->src = sin ? (const char *)sb : S.pool + p->sst; p->scnt = p->sdsp = 0; p->stride = 0; p->bytes = bytes;
    publish(p, rin ? off_of(rb) : p->rst, bytes, 0);
    push_peers(p, 0, -1);
    arrive(p, bytes, 0);
    if (!rin) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)rb + (size_t)r * bytes, S.pool + p->rst + (size_t)r * bytes, bytes, 0); SYNC(0); }
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
    (void)c;
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
    if (p->sst) pool_free(p->sst);
    if (p->rst) pool_free(p->rst);
    pool_free(p->base);
    free(p->pe); free(p->rbase); free(p->rpre); free(p->spre); free(p->sent); free(p->got); free(p); free(c);
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
    SHM_LOCK();
    p->own_ctx = !S.serial && shmem_ctx_create(0, &p->ctx) == 0;   /* one context per communicator (= per APU thread) when the calls run concurrently; under the lock the default context serves (and OSHMEM 4.1 loses puts on a context created after another was destroyed) */
    if (!p->own_ctx) p->ctx = SHMEM_CTX_DEFAULT;
    if (*mailbox(id, S.me) != 0) { SHM_UNLOCK(); fprintf(stderr, "comm_shmem: communicator id %d used twice\n", id); exit(1); }
    for (int r = 0; r < n; r++) shmem_ctx_long_p(p->ctx, mailbox(id, S.me), (long)p->base + 1, p->pe[r]);   /* my block's offset into every member's row (mine included), indexed by PE */
    shmem_ctx_quiet(p->ctx);
    SHM_UNLOCK();
    TRACE("create comm id %d (%d PEs from %d stride %d): posted, waiting for the members", id, n, pe_start, pe_stride);
    for (int r = 0; r < n; r++) { long *m = mailbox(id, p->pe[r]); wait_ne(m, 0); p->rbase[r] = (size_t)(*(volatile long *)m - 1); }
    TRACE("create comm id %d: done", id);
    return c;
}
#endif
