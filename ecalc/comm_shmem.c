/* comm_shmem.c - the SHMEM transport: the comm_ops of comm.h over the OpenSHMEM
 * 1.4 subset (plus put-with-signal where 1.5 exists), built and launched against Cray OpenSHMEMX (the only form mnrun.sh
 * launches since D14, 2026-10-12; another implementation of the same subset builds with SHMEM=1 and its SHMEM_CFLAGS).  One process per node = one PE; a communicator is a strided PE set (start, stride, size) -- the tree's node
 * groups -- with rank r = PE start + stride r.
 *
 * Memory.  One symmetric pool (COMM_SHMEM_POOL_MB) holds everything remote PEs write: a fixed mailbox table at its start,
 * then a per-PE first-fit allocator for the communicators' control blocks, their staging, and -- comm_sym_alloc -- the
 * callers' slab buffers.  Two forms of the pool (results/S12.md):
 *   - shmem_malloc'd host memory registered with hipHostRegister (the default; Cray OpenSHMEMX's heap is host memory) --
 *     on the APU the same HBM;
 *   - COMM_SHMEM_DEVHEAP=1 with an implementation whose shmem_malloc returns device memory (rocSHMEM): no
 *     registration, every copy D2D.
 * Offsets in the pool are what the PEs exchange: a put targets pool + offset, and the offset is one the RECEIVER chose
 * (its slab buffer, its staging, its control block), so nothing has to be allocated collectively after init -- creation
 * is the only handshake (each member posts its control block's offset into every member's mailbox[id][me]; ids come from
 * the caller, unique per run).
 *
 * The push model (as comm_xgmi): an exchange has a sequence number; the receiver publishes, per sender, where the
 * sender's slab lands (roff[sender] = seq | offset, in the sender's control block); the sender waits for that word and
 * puts its slab there, then the signal word sig[sender] = seq | byte count into the receiver's block, ordered after the
 * data by (COMM_SHMEM_ORDER) `putsig`: shmem_putmem_signal_nbi (1.5; one call, nothing blocks: the default where the
 * headers are 1.5), `fence`: shmem_fence + shmem_long_p (1.4, the spec's form), `quiet`: shmem_quiet + long_p (the default on
 * 1.4 headers; some 1.4 libraries' fence does not order an nbi put before a later put -- results/S.md).  The
 * receiver's wait() polls the signals, checks the counts, and quiets its own context (its send buffer is free again).
 * Buffers in the pool (comm_sym_alloc: ntt_dist's slabs, the layered communicator's scratch) are put from and into
 * directly; others are staged through the pool (D2H / H2D on the caller's stream, a copy per direction).
 * The sender's roff waits: alltoall() returns before completion (the slab pipeline, M7), so under `putsig` / `fence` the
 * puts are posted inline for the peers whose roff has arrived within COMM_SHMEM_SPIN_US (2000; in lockstep pipelines all
 * of them) and a helper thread posts the rest; under `quiet` (a blocking call before the signals) the helper thread does
 * all of it.
 * all-gather, the unequal all-to-all (counts known on both sides from the descriptors: the receiver's offsets are what it
 * publishes), barrier / max / sum-mod-q (an all-to-all of 8-byte values through the val/vseq words, double-buffered by
 * the sequence's parity) and point-to-point (a ring per (dest, source) with producer / consumer counters,
 * COMM_SHMEM_RING_KB) are the same words and puts.
 *
 * Threads.  The four APU threads drive four communicators at once.  COMM_SHMEM_SERIAL=0 (the target form; the default
 * on a library that provides SHMEM_THREAD_MULTIPLE): every communicator has its own context, the
 * waits are shmem_wait_until, no lock.  COMM_SHMEM_SERIAL=1 (the default below MULTIPLE): one process-wide mutex around every library call, the waits polled shmem_test under short
 * holds, the default context throughout.  Collectives of the library (shmem_barrier_all, reductions, teams) are used
 * nowhere except init/finalize: they are process-level and would need all four threads to agree; the strided PE set
 * is the shim for shmem_team_split_strided.
 * Build: -DCOMM_SHMEM with the SHMEM headers (Makefile: SHMEM_CRAY=1); without it the
 * entry points abort.  The exact call list is in results/S12.md (the portability contract).
 *
 * N7b (COMM_SHMEM_PE_PER_APU=1, default 0 = off; needs COMM_OFI=0; docs: ecalc/README.md): a node runs 1 + 4 PEs (mnrun.sh launches
 * them): local PEs 0..3 are proxies, one per APU (SHMEM_OFI_NIC_POLICY=ROUND-ROBIN puts proxy d on NIC d), the last local PE is the ecalc
 * node-process.  Ranks, meshes and every control word stay on the node-processes (logical PE = node; phys_pe maps it); only the data of the
 * device exchanges moves, as under comm_ofi: APU thread d's staging and symmetric slabs live in proxy d's symmetric pool (attached here with
 * shmem_ptr, HIP-registered when the driver allows), and the puts are posted by proxy d through its own NIC from a command ring in that pool;
 * the node-process sends each peer's signal once the proxy reports the puts complete (ofi_flush).  Proxies serve in comm_shmem_init and
 * never return.  The pool per PE is the rule's (binsplit_shmem_pool_rule); every PE's heap is the same size. */
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
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include "comm.h"
#include "comm_ofi.h"                             /* COMM_OFI=1, the data of the device exchanges over libfabric (internal code notes) */
/* the library heap the pool is carved from, for the pool rule before init (binsplit_shmem_pool_rule): the heap's
 * size from the launch line's variable (SHMEM_SYMMETRIC_SIZE; K / M / G / T
 * suffixes, else bytes), 0 when unset; *name = the variable */
size_t comm_shmem_heap_env(const char **name)
{
    const char *nm = "SHMEM_SYMMETRIC_SIZE";
    if (name) *name = nm;
    const char *e = getenv(nm); if (!e || !*e) return 0;
    char *end; double v = strtod(e, &end);
    switch (*end) { case 'k': case 'K': v *= 1024.0; break; case 'm': case 'M': v *= 1048576.0; break; case 'g': case 'G': v *= 1073741824.0; break; case 't': case 'T': v *= 1099511627776.0; break; default: break; }
    return v > 0 ? (size_t)v : 0;
}
#ifndef COMM_SHMEM
int comm_shmem_pool_in_heap(void) { return 0; }
static void no_shmem(void) { ec_fatal(EC_RC_FATAL, "comm_shmem: built without SHMEM (make SHMEM_CRAY=1)\n"); }
int   comm_shmem_init(void) { no_shmem(); return 0; }
int   comm_shmem_rank(void) { return 0; }
void  comm_shmem_counters(double *t, long *n, double *b) { *t = 0; *n = 0; *b = 0; }
void  comm_shmem_finalize(void) { }
comm *comm_shmem_create_at(int pe_start, int pe_stride, int n, int id) { (void)pe_start; (void)pe_stride; (void)n; (void)id; no_shmem(); return 0; }
int   comm_shmem_available(void) { return 0; }
const char *comm_shmem_impl(void) { return "none"; }
int   comm_shmem_pe_per_apu(void) { return 0; }
#else
#include <shmem.h>
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
    int inited, me, npes, serial, devheap, order, registered, prov, shift;
    long spin_us;
    char *pool; size_t pool_bytes, mb_bytes;   /* the symmetric pool; the mailbox at [0, mb_bytes) */
    pthread_mutex_t lock;                  /* SHM_LOCK: the library in serial mode; the allocator always (alloc_lock) */
    pthread_mutex_t alloc_lock;
    struct blk *blocks;                    /* the general area's blocks */
    struct blk *slot[4]; size_t slot_bytes; int nslot;   /* the staging regions, one per APU (COMM_SHMEM_STAGE_SLOT_MB, set by the pool rule) */
    size_t cur[3], peak[3], cur_all, peak_all;   /* pool accounting by kind: 0 control blocks, 1 staging, 2 the callers' symmetric buffers */
    size_t at_peak[3], nstage, nstage_at_peak, stage_blk_max, stage_rep;   /* the kinds at the moment of peak_all, live staging blocks, the largest one, the last staging peak reported */
    int verbose;                           /* COMM_SHMEM_VERBOSE=1 or ECALC_VERBOSE >= 2 -- a line per new staging peak (+5 %), every PE's summary */
    int xstats, vslot_pool;                               /* X1: COMM_XSTATS=1 (per-kind exchange stats, printed at finalize) */
    size_t round_bytes;                    /* COMM_SHMEM_ROUND_MB -- the staging of one alltoallv round, each way (0: off, one round) */
    long nrpath, nround_ex, nrounds; size_t round_stage_max;   /* exchanges through the rounds' path, those in more than one round, their rounds, the largest per-round staging (send + recv) */
    double tv; long nv;                    /* all-to-all time (alltoall, alltoallv), post to completion, summed over the APU threads; the count */
    double bv;                             /* the bytes this PE received from the other PEs in those exchanges (the off-process traffic), summed */
    int ppa, ppn, pme, pnpes, proxy;       /* N7b COMM_SHMEM_PE_PER_APU: proxies per APU (0 off), PEs per node (1 + 4 ppa), my physical PE and the count, 1 = this PE is a proxy */
} S;
enum { K_CTRL, K_STAGE, K_SYM };
static void site_print(void);                           /* (below) */
#define SHM_LOCK()   do { if (S.serial) pthread_mutex_lock(&S.lock); } while (0)
#define SHM_UNLOCK() do { if (S.serial) pthread_mutex_unlock(&S.lock); } while (0)
static void die(const char *m) { ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: %s\n", S.me, m); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }

static void xs_print(void);   /* X1 (below) */
const char *comm_shmem_impl(void)
{
    return "shmem";
}

/* the pool's first-fit allocator (offsets; a block list sorted by offset, coalesced on free).
 * with the pool sized to the model's need (COMM_SHMEM_POOL_AUTO, mnrun.sh's plan) one first-fit area fragments:
 * the four APU threads allocate and free staging of different sizes out of step (4 processes at 1e10: 529 MiB not found with
 * 1441 MiB free in a 4608 MiB pool).  So the staging has one region per APU (the thread's HIP device), each the model's
 * per-APU need (COMM_SHMEM_STAGE_SLOT_MB, exported by binsplit_shmem_pool_rule): a thread's staging is its current exchange's
 * send + receive, freed before the next, so its region never fragments.  A request that does not fit its region (the model
 * low, a fifth thread) falls back to the general area. */
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
    size_t used = S.cur_all, need = ((used + len + S.mb_bytes) >> 20) + 1;   /* name the size that would have held it (first fit: at least) */
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
static long *mailbox(int id, int pe) { return (long *)S.pool + (size_t)id * S.npes + pe; }   /* pe: logical (the node) */
static int phys_pe(int l) { return S.ppn > 1 ? l * S.ppn + S.ppn - 1 : l; }   /* N7b: the node-process of node l (the last PE of its block) */
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

/* ---- N7b: the proxies' heaps as named shared memory ----
 * Cray OpenSHMEMX 11.8 on aac7 maps the symmetric heap as shared anonymous memory (/proc maps: "/dev/zero (deleted)", rw-s) and has no
 * on-node path between PEs there (no /dev/xpmem: shmem_ptr gives NULL; SHMEM_LOCALITY_ONNODE=1 segfaults; B2 probe 2026-10-14).  So a proxy
 * arms this mmap wrapper around its shmem_init: the one MAP_SHARED anonymous (or /dev/zero) mapping at least the heap's size becomes the same
 * mapping of a file /dev/shm/ecalc_px_<job>_<step>_<PE> -- the same kind of pages (shmem), the same address and flags -- and the node-process
 * maps that file (px_attach), then unlinks it.  The wrapper is a pass-through everywhere else (not armed: every other process and call). */
static volatile int g_heap_arm; static char g_heap_path[160]; static void *g_heap_addr; static size_t g_heap_min;
static void *heap_mmap(void *a, size_t l, int prot, int fl, int fd, off_t o)
{
    if (g_heap_arm && !g_heap_addr && (fl & MAP_SHARED) && l >= g_heap_min) {
        int zero = (fl & MAP_ANONYMOUS) != 0;
        if (!zero && fd >= 0) { struct stat st; zero = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 1 && minor(st.st_rdev) == 5; }
        if (zero) {
            int sfd = open(g_heap_path, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (sfd >= 0 && ftruncate(sfd, (off_t)l) == 0) {
                void *r = (void *)syscall(SYS_mmap, a, l, prot, fl & ~(MAP_ANONYMOUS | MAP_HUGETLB | (MAP_HUGE_MASK << MAP_HUGE_SHIFT)), sfd, (off_t)0); close(sfd);
                if (r != MAP_FAILED) g_heap_addr = r; else unlink(g_heap_path);
                return r;
            }
            if (sfd >= 0) { close(sfd); unlink(g_heap_path); }
        }
    }
    return (void *)syscall(SYS_mmap, a, l, prot, fl, fd, o);
}
#ifdef __cplusplus
#define HEAP_THROW __THROW                                /* (C++: the headers' noexcept; C: attributes may not follow a definition's declarator) */
extern "C" {
#else
#define HEAP_THROW
#endif
void *mmap(void *a, size_t l, int prot, int fl, int fd, off_t o) HEAP_THROW { return heap_mmap(a, l, prot, fl, fd, o); }
void *mmap64(void *a, size_t l, int prot, int fl, int fd, off64_t o) HEAP_THROW { return heap_mmap(a, l, prot, fl, fd, (off_t)o); }
#ifdef __cplusplus
}
#endif
static void heap_path(char *buf, size_t n, int pe) { snprintf(buf, n, "/dev/shm/ecalc_px_%s_%s_%d", getenv("SLURM_JOB_ID") ? getenv("SLURM_JOB_ID") : "0", getenv("SLURM_STEP_ID") ? getenv("SLURM_STEP_ID") : "0", pe); }

/* ---- N7b: the proxy data plane (COMM_SHMEM_PE_PER_APU=1) ----
 * Proxy d's pool, as seen by both processes: [0, mb_bytes) unused (the mailbox's place), then PXCTL bytes of control -- a header
 * (head: commands posted by the node-process, done: commands whose puts are complete, quit, the two pids) and a ring of PXRING
 * commands -- then the general area, a first-fit allocator kept by the node-process (APU thread d's staging and slabs).  Offsets in
 * the pool are the same in every proxy (the pools are symmetric), so a receiver's published offset is where the sender's proxy puts. */
#define PXCTL ((size_t)1 << 20)
#define PXRING 16384
struct px_hdr { long head, done, quit, pid, ppid, pad[27]; };
struct px_ent { long pe; size_t roff, soff, n; };
typedef struct px_dev { char *base; int pe, reg, d; pthread_mutex_t lock; struct blk *blocks; long retired; double tlast; size_t cur, peak; long *cnt[PXRING]; } px_dev;
static px_dev *g_px[4];
static struct px_hdr *px_hdr_of(char *base) { return (struct px_hdr *)(base + S.mb_bytes); }
static struct px_ent *px_ring_of(char *base) { return (struct px_ent *)(base + S.mb_bytes + sizeof(struct px_hdr)); }
static int px_in(const px_dev *x, const void *q) { return x && (const char *)q >= x->base && (const char *)q < x->base + S.pool_bytes; }
static size_t px_alloc(px_dev *x, size_t len, int sym)
{
    (void)sym;
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&x->lock);
    for (struct blk *b = x->blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1; x->cur += len; if (x->cur > x->peak) x->peak = x->cur;
        pthread_mutex_unlock(&x->lock); return b->off;
    }
    size_t cur = x->cur; pthread_mutex_unlock(&x->lock);
    ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: proxy pool of APU %d (%zu MiB, COMM_SHMEM_POOL_MB per PE under COMM_SHMEM_PE_PER_APU) cannot hold %zu MiB more (in use %zu MiB); the model's need: %s MiB\n",
             S.me, x->d, S.pool_bytes >> 20, len >> 20, cur >> 20, getenv("COMM_SHMEM_POOL_NEED_MB") ? getenv("COMM_SHMEM_POOL_NEED_MB") : "?");
}
static void px_free(px_dev *x, size_t off)
{
    pthread_mutex_lock(&x->lock);
    struct blk *p = 0;
    for (struct blk *b = x->blocks; b; p = b, b = b->next) if (b->off == off) {
        b->used = 0; x->cur -= b->len;
        if (b->next && !b->next->used) { struct blk *n = b->next; b->len += n->len; b->next = n->next; free(n); }
        if (p && !p->used) { p->len += b->len; p->next = b->next; free(b); }
        break;
    }
    pthread_mutex_unlock(&x->lock);
}
/* retire the commands the proxy has completed: each lowers its exchange's per-peer count (ofi_flush then sends that peer's signal) */
static void px_progress(px_dev *x)
{
    struct px_hdr *h = px_hdr_of(x->base);
    pthread_mutex_lock(&x->lock);
    long d = __atomic_load_n(&h->done, __ATOMIC_ACQUIRE);
    if (d > x->retired) { for (; x->retired < d; x->retired++) __atomic_sub_fetch(x->cnt[x->retired % PXRING], 1, __ATOMIC_RELEASE); x->tlast = now_s(); }
    else if (x->retired < __atomic_load_n(&h->head, __ATOMIC_ACQUIRE)) {   /* commands outstanding, none completed: is the proxy still there? */
        double t = now_s(); if (!x->tlast) x->tlast = t;
        if (t - x->tlast > 30) { long pid = __atomic_load_n(&h->ppid, __ATOMIC_ACQUIRE); x->tlast = t;
            if (pid > 0 && kill((pid_t)pid, 0) != 0) { pthread_mutex_unlock(&x->lock); ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: the proxy of APU %d (pid %ld) is gone\n", S.me, x->d, pid); } }
    }
    pthread_mutex_unlock(&x->lock);
}
/* post n bytes from src (inside proxy x's pool) to the pool offset roff of PE dst_pe (another node's proxy of the same APU) */
static void px_write(px_dev *x, int dst_pe, size_t roff, const void *src, size_t n, long *cnt)
{
    if (!px_in(x, src) || !px_in(x, (const char *)src + (n ? n - 1 : 0))) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: a proxy put's source is outside the proxy pool\n", S.me);
    struct px_hdr *h = px_hdr_of(x->base); struct px_ent *ring = px_ring_of(x->base);
    pthread_mutex_lock(&x->lock);
    while (h->head - x->retired >= PXRING) { pthread_mutex_unlock(&x->lock); px_progress(x); sched_yield(); pthread_mutex_lock(&x->lock); }
    long k = h->head; struct px_ent *e = &ring[k % PXRING];
    e->pe = dst_pe; e->roff = roff; e->soff = (size_t)((const char *)src - x->base); e->n = n;
    x->cnt[k % PXRING] = cnt; __atomic_add_fetch(cnt, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&h->head, k + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->lock);
}
/* a proxy's life: post every command in its ring, quiet, report them done; leave on quit (or when the node-process is gone) */
static void px_serve(void)
{
    struct px_hdr *h = px_hdr_of(S.pool); struct px_ent *ring = px_ring_of(S.pool);
    __atomic_store_n(&h->ppid, (long)getpid(), __ATOMIC_RELEASE);
    long t = 0, nput = 0, nq = 0; double bytes = 0, tchk = now_s(), tbusy = 0; unsigned idle = 0;
    for (;;) {
        long hd = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
        if (hd > t) {
            double t0 = now_s();
            for (; t < hd; t++) { struct px_ent e = ring[t % PXRING]; if (e.n) { shmem_putmem_nbi(S.pool + e.roff, S.pool + e.soff, e.n, (int)e.pe); nput++; bytes += (double)e.n; } }
            shmem_quiet(); nq++;
            __atomic_store_n(&h->done, hd, __ATOMIC_RELEASE);
            tbusy += now_s() - t0; idle = 0; continue;
        }
        if (__atomic_load_n(&h->quit, __ATOMIC_ACQUIRE)) break;
        if (++idle > 4096) {
            struct timespec ts = { 0, 20000 }; nanosleep(&ts, 0);
            double tn = now_s();
            if (tn - tchk > 1.0) { tchk = tn; long pid = __atomic_load_n(&h->pid, __ATOMIC_ACQUIRE);
                if (pid > 0 && kill((pid_t)pid, 0) != 0) { fprintf(stderr, "comm_shmem: proxy pe %d: the node-process (pid %ld) is gone: exiting\n", S.pme, pid); unlink(g_heap_path); _exit(3); } }
        }
    }
    if (S.verbose || S.pme < S.ppn) fprintf(stderr, "comm_shmem: proxy pe %d (node %d, APU %d): %ld puts, %.2f GB in %ld batches, %.2f s posting + quiet = %.2f GB/s while busy\n",
                                           S.pme, S.pme / S.ppn, S.pme % S.ppn, nput, bytes / 1e9, nq, tbusy, tbusy > 0 ? bytes / 1e9 / tbusy : 0);
}
/* the node-process: attach the proxies' pools (shmem_ptr: Cray's on-node XPMEM), register them with HIP, start the allocators */
static void px_attach(void)
{
    for (int d = 0; d < 4; d++) {
        px_dev *x = (px_dev *)calloc(1, sizeof *x); g_px[d] = x; x->d = d;
        x->pe = S.me * S.ppn + d;                         /* proxy d of my node (ppa 1: local PE d) */
        x->base = (char *)shmem_ptr(S.pool, x->pe);       /* (a library with an on-node path; aac7's has none) */
        if (!x->base) {                                   /* the proxy's heap file (heap_mmap), at the pool's offset in the heap (the proxy's mailbox word) */
            char path[160]; heap_path(path, sizeof path, x->pe);
            long off = shmem_long_g(mailbox(MAXID - 1, 1), x->pe);
            int fd = open(path, O_RDWR); struct stat st;
            if (fd < 0 || fstat(fd, &st) != 0 || off < 0 || (size_t)off + S.pool_bytes > (size_t)st.st_size)
                ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU: the heap of proxy PE %d is not reachable (%s: %s; pool offset %ld)\n", S.me, x->pe, path, fd < 0 ? strerror(errno) : "too small", off);
            char *m = (char *)mmap(0, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); unlink(path);
            if (m == MAP_FAILED) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: mmap of %s: %s\n", S.me, path, strerror(errno));
            x->base = m + off;
        }
        if (px_hdr_of(x->base)->pad[0] != 0x4e374250) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU: the proxy PE %d's pool is not the memory mapped here (no mark)\n", S.me, x->pe);
        pthread_mutex_init(&x->lock, 0);
        x->blocks = (struct blk *)malloc(sizeof *x->blocks); x->blocks->off = S.mb_bytes + PXCTL; x->blocks->len = S.pool_bytes - S.mb_bytes - PXCTL; x->blocks->used = 0; x->blocks->next = 0;
        x->reg = 1;
#ifndef COMM_HOST_ONLY
        hipError_t he = hipHostRegister(x->base, S.pool_bytes, hipHostRegisterPortable);
        if (he != hipSuccess) { (void)hipGetLastError(); x->reg = 0; }
        else { void *dp = 0; if (hipHostGetDevicePointer(&dp, x->base, 0) != hipSuccess || dp != x->base) { (void)hipGetLastError(); x->reg = 0; (void)hipHostUnregister(x->base); (void)hipGetLastError(); } }
        if (S.me == 0 && d == 0) printf("comm_shmem: COMM_SHMEM_PE_PER_APU=1: the proxy pools %s (%s)\n", x->reg ? "are HIP-registered: symmetric slabs in them" : "could not be HIP-registered: every device exchange is staged (pageable copies)", x->reg ? "zero-copy" : hipGetErrorString(he));
#endif
        __atomic_store_n(&px_hdr_of(x->base)->pid, (long)getpid(), __ATOMIC_RELEASE);
    }
}
static void px_shutdown(void)
{
    for (int d = 0; d < 4; d++) { px_dev *x = g_px[d]; if (!x) continue;
        struct px_hdr *h = px_hdr_of(x->base);
        while (__atomic_load_n(&h->done, __ATOMIC_ACQUIRE) < __atomic_load_n(&h->head, __ATOMIC_ACQUIRE)) { px_progress(x); sched_yield(); }
        px_progress(x);
        if (S.verbose || S.me == 0) printf("comm_shmem: pe %d: proxy pool of APU %d: peak %zu MiB of %zu\n", S.me, d, x->peak >> 20, S.pool_bytes >> 20);
        __atomic_store_n(&h->quit, 1, __ATOMIC_RELEASE);
    }
}
int comm_shmem_pe_per_apu(void)
{
    const char *e = getenv("COMM_SHMEM_PE_PER_APU"); int v = e ? atoi(e) : 0;
    if (v < 0 || v > 1) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PE_PER_APU=%s: 0 (off) or 1 (one proxy PE per APU); 2 (two NICs per APU) is designed, not built (docs: N7b)\n", e);
    return v;
}
int comm_shmem_available(void) { return 1; }
/* 1: the pool is shmem_malloc'd from the library's heap (sized by the launch line) */
int comm_shmem_pool_in_heap(void) { return 1; }
int comm_shmem_rank(void) { return S.me; }
void comm_shmem_counters(double *t, long *n, double *b) { pthread_mutex_lock(&S.alloc_lock); *t = S.tv; *n = S.nv; *b = S.bv; pthread_mutex_unlock(&S.alloc_lock); }
static int env_int(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }
int comm_shmem_init(void)
{
    if (S.inited) return S.npes;
    int prov = -1;
    S.verbose = env_int("COMM_SHMEM_VERBOSE", 0); if (!S.verbose && env_int("ECALC_VERBOSE", 1) >= 2) S.verbose = 1;
    size_t mb = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192;
#ifdef COMM_SHMEM_DEVICE_HEAP
    S.devheap = 1;
#else
    S.devheap = env_int("COMM_SHMEM_DEVHEAP", 0);
#endif
    S.ppa = comm_shmem_pe_per_apu(); S.ppn = S.ppa ? 1 + 4 * S.ppa : 1;   /* N7b: 1 + 4 ppa PEs per node */
    if (S.ppa && comm_ofi_enabled()) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PE_PER_APU=%d needs COMM_OFI=0 (the proxies carry the data instead of comm_ofi)\n", S.ppa);
    /* the mailbox needs the PE count, known only after init; sized for the launcher's count when it tells us, else for 4096 PEs (N7b: nodes) */
    int npes_hint = getenv("SLURM_NTASKS") ? atoi(getenv("SLURM_NTASKS")) / S.ppn : getenv("PMI_SIZE") ? atoi(getenv("PMI_SIZE")) / S.ppn : getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 4096;
    if (npes_hint < 1) npes_hint = 4096;
    S.mb_bytes = ((size_t)MAXID * npes_hint * 8 + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    S.pool_bytes = (mb << 20) + S.mb_bytes;
    { const char *ex = getenv("COMM_SHMEM_PROXY"); if (S.ppa && ex && atoi(ex)) {   /* N7b: a proxy's heap as a named file (heap_mmap) */
        g_heap_min = comm_shmem_heap_env(0); if (!g_heap_min) { fprintf(stderr, "comm_shmem: a proxy needs SHMEM_SYMMETRIC_SIZE (mnrun.sh sets it)\n"); exit(1); }
        heap_path(g_heap_path, sizeof g_heap_path, getenv("SLURM_PROCID") ? atoi(getenv("SLURM_PROCID")) : -1); unlink(g_heap_path); g_heap_arm = 1; } }
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &prov);
    g_heap_arm = 0;
    S.prov = prov;
    S.pme = S.me = shmem_my_pe(); S.pnpes = S.npes = shmem_n_pes();
    if (S.ppa) {                                          /* N7b: logical PE = node; the last PE of a node's block is its node-process */
        if (S.pnpes % S.ppn) { fprintf(stderr, "comm_shmem: COMM_SHMEM_PE_PER_APU=%d: %d PEs are not whole nodes of %d (launch with mnrun.sh)\n", S.ppa, S.pnpes, S.ppn); shmem_global_exit(1); }
        S.me = S.pme / S.ppn; S.npes = S.pnpes / S.ppn; S.proxy = S.pme % S.ppn != S.ppn - 1;
        if (S.npes < 2) { fprintf(stderr, "comm_shmem: COMM_SHMEM_PE_PER_APU needs at least 2 nodes\n"); shmem_global_exit(1); }
        if (!getenv("SLURM_PROCID") || atoi(getenv("SLURM_PROCID")) != S.pme) { fprintf(stderr, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU needs PE = SLURM_PROCID (srun, block placement; got %s)\n", S.pme, getenv("SLURM_PROCID") ? getenv("SLURM_PROCID") : "unset"); shmem_global_exit(1); }
        if (S.proxy && !g_heap_addr) { fprintf(stderr, "comm_shmem: proxy pe %d: the symmetric heap was not mapped through the named file (no shared anonymous mapping >= SHMEM_SYMMETRIC_SIZE seen)\n", S.pme); shmem_global_exit(1); }
        const char *ex = getenv("COMM_SHMEM_PROXY"); if (ex && (atoi(ex) != 0) != S.proxy) { fprintf(stderr, "comm_shmem: pe %d: COMM_SHMEM_PROXY=%s but the PE's place in its node makes it %s (block placement expected)\n", S.pme, ex, S.proxy ? "a proxy" : "the node-process"); shmem_global_exit(1); }
    }
    if (S.npes > npes_hint) { fprintf(stderr, "comm_shmem: %d PEs but the mailbox was sized for %d (set COMM_SIZE)\n", S.npes, npes_hint); shmem_global_exit(1); }
    pthread_mutex_init(&S.lock, 0); pthread_mutex_init(&S.alloc_lock, 0);
    const char *e = getenv("COMM_SHMEM_SERIAL");
    int serial_default = prov < SHMEM_THREAD_MULTIPLE;
    S.serial = e ? atoi(e) != 0 : serial_default;
    if (prov < SHMEM_THREAD_MULTIPLE && !S.serial) { if (S.me == 0) fprintf(stderr, "comm_shmem: the library provides thread level %d, not MULTIPLE: serialising the calls\n", prov); S.serial = 1; }
    const char *eo = getenv("COMM_SHMEM_ORDER");
    if (eo) S.order = !strcmp(eo, "putsig") ? ORDER_PUTSIG : !strcmp(eo, "fence") ? ORDER_FENCE : ORDER_QUIET;
    else S.order = HAVE_PUT_SIGNAL ? ORDER_PUTSIG : ORDER_QUIET;
    if (S.order == ORDER_PUTSIG && !HAVE_PUT_SIGNAL) { if (S.me == 0) fprintf(stderr, "comm_shmem: putsig needs OpenSHMEM 1.5 headers: using quiet\n"); S.order = ORDER_QUIET; }
    S.spin_us = env_int("COMM_SHMEM_SPIN_US", 2000);
    { const char *ep = getenv("COMM_SHMEM_PEER_ORDER"); S.shift = ep && !strcmp(ep, "shift"); }   /* N7: peers as a shifted ring, not in rank order */
    S.xstats = env_int("COMM_XSTATS", 0) > 0;
    S.vslot_pool = env_int("COMM_LAYER_VSLOT_POOL", 1) > 0;   /* X2: no rounds for an exchange from and into the pool (default 1) */
    { const char *er = getenv("COMM_SHMEM_ROUND_MB"); double rm = er ? atof(er) : 0; S.round_bytes = rm > 0 ? (size_t)(rm * 1048576.0) : 0; }
    S.pool = (char *)shmem_malloc(S.pool_bytes);
    if (!S.pool) { fprintf(stderr, "comm_shmem: pe %d: shmem_malloc of %zu MiB failed: the SHMEM heap (SHMEM_SYMMETRIC_SIZE) must be >= %zu MiB (the pool + 512)\n", S.me, S.pool_bytes >> 20, (S.pool_bytes >> 20) + 512); shmem_global_exit(1); }
    memset(S.pool, 0, S.mb_bytes);                        /* the mailbox: 0 = empty */
    if (S.ppa && !S.proxy) *mailbox(MAXID - 1, 0) = (long)getpid();   /* N7b: the node-process's pid in the reserved mailbox row, for its proxies */
    if (S.proxy) {                                        /* N7b: a proxy -- its control zeroed before the barrier; it serves until the node-process's finalize */
        memset(S.pool + S.mb_bytes, 0, PXCTL); px_hdr_of(S.pool)->pad[0] = 0x4e374250;   /* the mark the node-process checks after mapping */
        *mailbox(MAXID - 1, 1) = (long)(S.pool - (char *)g_heap_addr);              /* the pool's offset in the heap file */
        shmem_barrier_all(); S.inited = 1;
        px_hdr_of(S.pool)->pid = shmem_long_g(mailbox(MAXID - 1, 0), phys_pe(S.me));   /* (the liveness check: no shared memory needed) */
        px_serve();
        shmem_barrier_all();                              /* (comm_shmem_finalize's) */
        shmem_free(S.pool); shmem_finalize(); unlink(g_heap_path); fflush(stdout); fflush(stderr); _exit(0);
    }
#ifndef COMM_HOST_ONLY
    if (S.devheap) {                                      /* the transport takes COMM_SHMEM_DEVHEAP=1 at its word only when the
                                                           * library's heap is device memory; Cray OpenSHMEMX 11.8's heap is host hugepages (no HIP in libsma), and
                                                           * handing that to kernels as a device pool would fault -- so it is registered like the default host pool */
        hipPointerAttribute_t pa; hipError_t pe = hipPointerGetAttributes(&pa, S.pool);
        int dev = pe == hipSuccess && (pa.type == hipMemoryTypeDevice || pa.type == hipMemoryTypeManaged);
        (void)hipGetLastError();
        if (!dev) { if (S.me == 0) fprintf(stderr, "comm_shmem: COMM_SHMEM_DEVHEAP=%d but the library's shmem_malloc returned host memory (%s): the pool is HIP-registered host memory instead\n", S.devheap, pe == hipSuccess ? "known to HIP, not device" : hipGetErrorString(pe)); S.devheap = 0; }
    }
    if (!S.devheap) {
        hipError_t he = hipHostRegister(S.pool, S.pool_bytes, hipHostRegisterPortable);
        S.registered = he == hipSuccess;
        if (!S.registered) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies, no symmetric slabs\n", hipGetErrorString(he)); }
        else { void *dp = 0; if (hipHostGetDevicePointer(&dp, S.pool, 0) != hipSuccess || dp != S.pool) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: the pool's device pointer differs from its host pointer: no symmetric slabs\n"); S.registered = 2; } }
    }
#endif
    S.blocks = (struct blk *)malloc(sizeof *S.blocks); S.blocks->off = S.mb_bytes; S.blocks->len = S.pool_bytes - S.mb_bytes; S.blocks->used = 0; S.blocks->next = 0;
#ifndef COMM_HOST_ONLY
    {   /* the four staging regions at the pool's end, when the rule sized them and the general area keeps >= 64 MiB */
        const char *es = getenv("COMM_SHMEM_STAGE_SLOT_MB"); size_t sb = es ? (size_t)(atof(es) * 1048576.0) : 0; sb = (sb + ALIGN - 1) & ~(size_t)(ALIGN - 1);
        if (sb && 4 * sb + ((size_t)64 << 20) <= S.blocks->len) {
            S.nslot = 4; S.slot_bytes = sb; S.blocks->len -= 4 * sb;
            for (int i = 0; i < 4; i++) { struct blk *b = (struct blk *)malloc(sizeof *b); b->off = S.pool_bytes - (size_t)(4 - i) * sb; b->len = sb; b->used = 0; b->next = 0; S.slot[i] = b; }
        } else if (sb && S.me == 0) fprintf(stderr, "comm_shmem: no staging regions (%zu MiB each do not fit the pool beside 64 MiB): one first-fit area\n", sb >> 20);
    }
#endif
    shmem_barrier_all();                                  /* every mailbox is zeroed before anyone posts */
    S.inited = 1;
    if (S.ppa) px_attach();                               /* N7b: (after the barrier: the proxies' control is zeroed) */
    if (S.me == 0 && S.round_bytes) printf("comm_shmem: COMM_SHMEM_ROUND_MB=%.6g: an all-to-all above it is carried in rounds of at most that staging each way\n", S.round_bytes / 1048576.0);
    if (S.me == 0 && S.nslot) printf("comm_shmem: staging in %d regions of %.1f MiB (one per APU) + a general area of %.1f MiB\n", S.nslot, S.slot_bytes / 1048576.0, S.blocks->len / 1048576.0);
    if (S.me == 0) printf("comm_shmem: %s, %d PEs%s, thread level %d (%s), pool %zu MiB%s%s, order %s%s, spin %ld us, peer order %s\n", comm_shmem_impl(), S.npes, S.ppa ? " (nodes; + 4 proxy PEs each, COMM_SHMEM_PE_PER_APU=1)" : "", prov, S.serial ? "calls serialised" : "concurrent, a context per communicator", S.pool_bytes >> 20,
                          S.devheap ? " (device heap)" : "", S.registered == 1 ? ", HIP-registered" : "",
                          S.order == ORDER_PUTSIG ? "putsig" : S.order == ORDER_FENCE ? "fence" : "quiet", S.order == ORDER_QUIET ? " (helper thread)" : " (inline)", S.spin_us, S.shift ? "shift" : "rank");
    return S.npes;
}
void comm_shmem_finalize(void)
{
    if (!S.inited) return;
    if (S.ppa) px_shutdown();                             /* N7b: the proxies' last puts complete, then they leave for the barrier */
    shmem_barrier_all();
    if (S.me == 0 || S.verbose) {
        printf("comm_shmem: pe %d: pool peak %zu MiB of %zu (control %zu, staging %zu, symmetric buffers %zu MiB peaks)\n", S.me, S.peak_all >> 20, S.pool_bytes >> 20, S.peak[K_CTRL] >> 20, S.peak[K_STAGE] >> 20, S.peak[K_SYM] >> 20);
        /* what occupied the pool at its peak (the kinds then, the live staging blocks), the largest staging block, the mailbox */
        printf("comm_shmem: pe %d: at the peak: control %.1f, staging %.1f (%zu blocks; the largest staging block of the run %.1f), symmetric buffers %.1f MiB; mailbox %.2f MiB\n", S.me,
               S.at_peak[K_CTRL] / 1048576.0, S.at_peak[K_STAGE] / 1048576.0, S.nstage_at_peak, S.stage_blk_max / 1048576.0, S.at_peak[K_SYM] / 1048576.0, S.mb_bytes / 1048576.0);
        if (S.verbose >= 2) site_print();
        printf("comm_shmem: pe %d: all-to-all %ld exchanges, %.2f s post to completion (summed over the APU threads), %.2f GB received = %.2f GB/s per APU thread", S.me, S.nv, S.tv, S.bv / 1e9, S.tv > 0 ? S.bv / 1e9 / S.tv : 0);   /* the bytes */
        if (S.round_bytes) printf("; %ld through the rounds' path, %ld of them in %ld rounds (COMM_SHMEM_ROUND_MB=%.6g, the largest round's staging %.1f MiB send + recv)", S.nrpath, S.nround_ex, S.nrounds, S.round_bytes / 1048576.0, S.round_stage_max / 1048576.0);
        printf("\n");
    }
#ifndef COMM_HOST_ONLY
    if (S.registered) HIP_CHECK(hipHostUnregister(S.pool));
    for (int d = 0; d < 4; d++) if (g_px[d] && g_px[d]->reg) { (void)hipHostUnregister(g_px[d]->base); (void)hipGetLastError(); }
#endif
    if (S.xstats) xs_print();                             /* X1: every PE prints its own lines */
    comm_ofi_finalize();                                  /* the per-NIC bytes (COMM_OFI_VERBOSE), then the domains and pools */
    shmem_free(S.pool);
    shmem_finalize(); S.inited = 0;
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
    /* the words' sequences per pair -- oseq[r] my sends to r (the roff word r writes into my block, my signal
     * into r's), iseq[r] r's sends to me.  An ordinary exchange advances every pair by one (so they equal seq); an exchange in
     * rounds advances a pair by its round count, which both ends derive from the pair's byte count alone */
    long *oseq, *iseq;
    size_t *acnt, *adsp;                   /* an equal exchange's counts and offsets for the rounds */
    int rounds, dev, rs_made; hipStream_t rs; size_t chunk, rsb_cap; const char *vsb; char *vrb; int vsin; double t0;   /* the pending exchange in rounds (helper thread, own stream) */
    /* the communicator's libfabric data plane.  xo: the current exchange's data goes over it (the device ops);
     * db: the base its staging offsets, the published roff offsets and the in-pool test refer to (the SHMEM pool, or the device's
     * comm pool under xo); sdb: the pool the cached staging lives in; ocnt[r]: writes to r in flight; osig[r]: r's signal to send
     * once they completed (0: none) */
    int xk, xph, xapu; long xK; double xroff, xsig;   /* X1 xstats: the pending exchange's kind, phase, APU, rounds, and the seconds blocked on offset words (skew) / signal words (transfer, tail) */
    int ofi, xo, sdb; char *db; ofi_dev *od; ofi_peers *op; long *ocnt, *osig;
    px_dev *px; int *pxpe;                 /* N7b: the proxy data plane (ofi = 1 with px set): my APU's proxy, the members' proxies (physical PEs) */
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
/* the data base of the current exchange -- on: a device op of an OFI communicator (its data over libfabric) */
static void set_xo(shm_priv *p, int on) { p->xo = on && p->ofi; p->db = p->xo ? (p->px ? p->px->base : comm_ofi_pool(p->od)) : S.pool; }
static int in_db(shm_priv *p, const void *x) { return p->xo ? (p->px ? px_in(p->px, x) : comm_ofi_in_pool(p->od, x)) : in_pool(x); }
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
    if (p->xo) { if (p->px) { if (n) px_write(p->px, p->pxpe[r], off, src, n, &p->ocnt[r]); } else comm_ofi_write(p->op, r, off, src, n, &p->ocnt[r]); p->osig[r] = sig; return; }   /* the signal after delivery (ofi_flush) */
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
/* progress the device's writes; each peer whose writes have all completed (delivered) gets its signal word through SHMEM.
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
        if (p->px) px_progress(p->px); else comm_ofi_progress(p->od);
        if (spins > 1024) sched_yield();
    }
}
static void order_ctx(shm_priv *p) { if (S.order == ORDER_FENCE) shmem_ctx_fence(p->ctx); else shmem_ctx_quiet(p->ctx); }   /* under SHM_LOCK: the words before their sequence (tiny exchange, rings) */

/* the staging of one exchange (send, receive): allocated per exchange, released when it completes (staging_release) --
 * a level's meshes live to the end of the run, and staging held per communicator would add up over the levels;
 * pool-resident buffers (comm_sym_alloc) need none */
/* COMM_SHMEM_VERBOSE=2 -- every staged exchange by its call site (the return addresses of the frames above the
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
static void stage_free(shm_priv *p, size_t off) { if (p->sdb) { if (p->px) px_free(p->px, off); else comm_ofi_free(p->od, off); } else pool_free(off); }   /* in the pool it came from */
static void staging_drop(shm_priv *p) { if (p->sst) stage_free(p, p->sst); if (p->rst) stage_free(p, p->rst); p->sst = p->rst = 0; p->sst_cap = p->rst_cap = 0; }
static void staging(shm_priv *p, size_t send, size_t recv, const char *what)
{
    if ((p->sst || p->rst) && p->sdb != p->xo) staging_drop(p);   /* the cached staging is in the other pool */
    p->sdb = p->xo;
    if (send > p->sst_cap) { if (p->sst) stage_free(p, p->sst); p->sst = p->xo ? (p->px ? px_alloc(p->px, send, 0) : comm_ofi_alloc(p->od, send, 0)) : pool_alloc(send, K_STAGE); p->sst_cap = send; }
    if (recv > p->rst_cap) { if (p->rst) stage_free(p, p->rst); p->rst = p->xo ? (p->px ? px_alloc(p->px, recv, 0) : comm_ofi_alloc(p->od, recv, 0)) : pool_alloc(recv, K_STAGE); p->rst_cap = recv; }
    if (S.verbose >= 2 && (send || recv)) site_note(send, recv);
    if (S.verbose && S.cur[K_STAGE] > S.stage_rep + S.stage_rep / 20 && (send || recv)) {   /* which exchange raises the staging peak (racy read: a report, not the accounting) */
        S.stage_rep = S.cur[K_STAGE];
        printf("comm_shmem: pe %d: staging %.1f MiB in use (%zu blocks, pool %.1f MiB): comm %d (%d PEs) %s send %.1f + recv %.1f MiB\n", S.me, S.cur[K_STAGE] / 1048576.0, S.nstage, S.cur_all / 1048576.0,
               p->id, p->n, what, send / 1048576.0, recv / 1048576.0);
    }
}
/* the order the peers are walked in: step k of a send loop goes to rank k (COMM_SHMEM_PEER_ORDER=rank) or to (me + k) mod n (shift: every
 * step is a one-to-one pairing, so a receiver has one sender at a time); dir -1 is the receive side (the sender at step k).  Step 0 is me. */
static inline int peer_at(const shm_priv *p, int k, int dir) { return S.shift ? ((p->me + dir * k) % p->n + p->n) % p->n : k; }
#define PEERS(p, k, r, from, dir) for (int k = (from), r; k < (p)->n && ((r = peer_at(p, k, dir)), 1); k++) if (r != (p)->me)
/* the sender's half of an exchange for the peers from step `from`: wait for each receiver's offset (at most spin_us when
 * >= 0: returns the first step not ready), put, signal.  Returns n when every peer is done. */
static int push_peers(shm_priv *p, int from, long spin_us)
{
    PEERS(p, k, r, from, 1) {
        long seq = p->oseq[r], *w = W(p, p->base, W_ROFF, r);
        if (spin_us < 0) XWAIT(p, xroff, w, PACK(seq, 0));
        else { double t0 = 0; for (unsigned spins = 0; !test_ge(w, PACK(seq, 0)); spins++) { if (spins > 32) { double t = now_s(); if (!t0) t0 = t; else if ((t - t0) * 1e6 > (double)spin_us) return k; sched_yield(); } } }
        long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("exchange sequence mismatch");
        size_t n = p->scnt ? p->scnt[r] : p->bytes; const char *s = p->scnt ? p->src + p->sdsp[r] : p->src + p->stride * (size_t)r;
        SHM_LOCK(); put_signalled(p, r, UNOFF(x), s, n, PACK(seq, n)); SHM_UNLOCK();
    }
    if (p->xo) ofi_flush(p);
    else if (S.order == ORDER_QUIET) {
        SHM_LOCK(); shmem_ctx_quiet(p->ctx);
        PEERS(p, k, r, from, 1) put_word(p, r, W_SIG, p->me, PACK(p->oseq[r], p->scnt ? p->scnt[r] : p->bytes));
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
    if (S.order != ORDER_QUIET && !p->xo) p->next = push_peers(p, 0, S.spin_us);   /* (an OFI push blocks until delivery: always the thread) */
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
    PEERS(p, k, r, 0, -1) put_word(p, r, W_ROFF, p->me, PACK(p->iseq[r], base + (dsp ? dsp[r] : bytes * (size_t)r)));
    SHM_UNLOCK();
}
/* wait for every sender's signal (and count), then the context's completion (my send buffer is free again) */
static void arrive(shm_priv *p, size_t bytes, const size_t *rcnt)
{
    PEERS(p, k, r, 0, -1) {
        long *sw = W(p, p->base, W_SIG, r); XWAIT(p, xsig, sw, PACK(p->iseq[r], 0));
        long x = *(volatile long *)sw, want = (long)(rcnt ? rcnt[r] : bytes);
        if (UNSEQ(x) != p->iseq[r]) die("signal sequence mismatch");
        if ((long)UNOFF(x) != want) { ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch: rank %d sends %ld bytes to rank %d, which expects %ld\n", r, (long)UNOFF(x), p->me, want); }
    }
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();
}
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { return c->size; }
static int alltoallv_rounds(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int sin, int rin);   /* (below) */
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoall while one is pending");
    set_xo(p, 1);
    int sin = in_db(p, sb), rin = in_db(p, rb);
    p->t0 = now_s(); if (S.xstats) xs_begin(p);
    if (S.round_bytes) {                                  /* the equal exchange as an unequal one, in rounds when a slab exceeds the round's chunk */
        for (int r = 0; r < n; r++) { p->acnt[r] = bytes; p->adsp[r] = (size_t)r * bytes; }
        if (alltoallv_rounds(c, sb, p->acnt, p->adsp, rb, p->acnt, p->adsp, s, sin, rin)) return;
    }
    staging(p, sin ? 0 : bytes * n, rin ? 0 : bytes * n, "alltoall");
    seq_next(p); p->v = 0; p->bytes = bytes; p->stride = bytes; p->scnt = p->sdsp = 0; p->rb = rb; p->st = s; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : p->db + p->sst;
    if (!sin) COPY(p->db + p->sst, sb, bytes * n, s);                                         /* my slabs (all of them: simpler than skipping the self slab) */
    if ((const char *)sb + (size_t)me * bytes != (char *)rb + (size_t)me * bytes) COPY((char *)rb + (size_t)me * bytes, (const char *)sb + (size_t)me * bytes, bytes, s);   /* the self slab */
    SYNC(s);                                              /* the stream is done with sb and rb (a pool-resident rb may still be read by the caller's earlier kernels: publish only after) */
    publish(p, rin ? off_db(p, rb) : p->rst, bytes, 0);
    start_push(c);
}
/* ---- the all-to-alls in rounds (COMM_SHMEM_ROUND_MB; results/V114.md; the equal one as an unequal one) ----
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
        PEERS(p, k, r, 0, -1) if (j < round_count(rcnt[r], c))
            put_word(p, r, W_ROFF, me, PACK(p->iseq[r] + 1 + j, p->rin ? off_db(p, p->vrb) + rdsp[r] + o : p->rst + p->rpre[r]));
        SHM_UNLOCK();
        if (!p->vsin) {                                   /* my chunks j into the send staging (free: round j - 1's quiet) */
            int any = 0;
            for (int r = 0; r < n; r++) if (r != me && j < round_count(scnt[r], c) && scnt[r] > o) { size_t len = scnt[r] - o < c ? scnt[r] - o : c; COPY(p->db + p->sst + p->spre[r], p->vsb + sdsp[r] + o, len, p->rs); any = 1; }
            if (any) SYNC(p->rs);
        }
        PEERS(p, k, r, 0, 1) if (j < round_count(scnt[r], c)) {   /* the sender's half */
            long seq = p->oseq[r] + 1 + j, *w = W(p, p->base, W_ROFF, r); XWAIT(p, xroff, w, PACK(seq, 0));
            long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("round sequence mismatch");
            size_t len = scnt[r] > o ? (scnt[r] - o < c ? scnt[r] - o : c) : 0;
            const char *src = p->vsin ? p->vsb + sdsp[r] + o : p->db + p->sst + p->spre[r];
            SHM_LOCK(); put_signalled(p, r, UNOFF(x), src, len, PACK(seq, len)); SHM_UNLOCK();
        }
        if (p->xo) ofi_flush(p);
        else if (S.order == ORDER_QUIET) {
            SHM_LOCK(); shmem_ctx_quiet(p->ctx);
            PEERS(p, k, r, 0, 1) if (j < round_count(scnt[r], c)) { size_t len = scnt[r] > o ? (scnt[r] - o < c ? scnt[r] - o : c) : 0; put_word(p, r, W_SIG, me, PACK(p->oseq[r] + 1 + j, len)); }
            SHM_UNLOCK();
        }
        PEERS(p, k, r, 0, -1) if (j < round_count(rcnt[r], c)) {   /* the signals of round j */
            long seq = p->iseq[r] + 1 + j, *sw = W(p, p->base, W_SIG, r); XWAIT(p, xsig, sw, PACK(seq, 0));
            long x = *(volatile long *)sw; size_t want = rcnt[r] > o ? (rcnt[r] - o < c ? rcnt[r] - o : c) : 0;
            if (UNSEQ(x) != seq) die("round signal sequence mismatch");
            if (UNOFF(x) != want) ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch (round %ld): rank %d sends %ld bytes to rank %d, which expects %zu\n", j, r, (long)UNOFF(x), me, want);
        }
        SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();   /* my puts of round j are complete: the send staging is free */
        if (!p->rin) {
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
/* the bytes an exchange brought in from the other PEs (alltoall: (n - 1) slabs; alltoallv: the counts) */
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
    if (p->thread) { pthread_join(p->th, 0); p->thread = 0; }
    if (p->rounds) {                                      /* the rounds completed in the helper thread */
        p->rounds = 0; staging_drop(p); p->pending = 0;
        double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock);
        if (S.xstats) xs_end(p, xtw0, recv_bytes(p), t);
        return;
    }
    arrive(p, p->bytes, p->v ? p->rcnt : 0);
    if (!p->rin) {
        if (!p->v) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)p->rb + (size_t)r * p->bytes, p->db + p->rst + (size_t)r * p->bytes, p->bytes, p->st); }
        else for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], p->db + p->rst + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    staging_drop(p);
    p->pending = 0;
    { double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock); if (S.xstats) xs_end(p, xtw0, recv_bytes(p), t); }
}
/* the host variants: complete on return; the source is the caller's buffer (a put may read private memory) */
static void s_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv_host while an exchange is pending");
    set_xo(p, 0);                                         /* (host buffers stay on SHMEM) */
    size_t tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, 0, tr + 8, "alltoallv_host");
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    if (scnt[me]) memmove((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me]);
    seq_next(p); p->src = (const char *)sb; p->scnt = scnt; p->sdsp = sdsp; p->bytes = 0;
    publish(p, p->rst, 0, p->rpre);
    push_peers(p, 0, -1);
    arrive(p, 0, rcnt);
    for (int r = 0; r < n; r++) if (r != me && rcnt[r]) memcpy((char *)rb + rdsp[r], S.pool + p->rst + p->rpre[r], rcnt[r]);
    staging_drop(p);
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
    staging_drop(p);
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
    staging_drop(p);
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
    if (op->px) return op->px->reg ? op->px->base + px_alloc(op->px, bytes, 1) : 0;   /* N7b: the proxy's pool (when HIP could register it) */
    if (op->ofi) {                                        /* the device's comm pool (device-accessible, registered with the NICs) */
        return comm_ofi_pool(op->od) + comm_ofi_alloc(op->od, bytes, 1);
    }
#ifndef COMM_HOST_ONLY
    if (!S.devheap && S.registered != 1) return 0;        /* not device-accessible: the caller allocates, the transport stages */
#endif
    size_t off = pool_alloc(bytes, K_SYM);
    return S.pool + off;
}
static void s_sym_free(comm *c, void *ptr)
{
    (void)c;
    for (int d = 0; d < 4; d++) if (px_in(g_px[d], ptr)) { px_free(g_px[d], (size_t)((char *)ptr - g_px[d]->base)); return; }   /* N7b */
    { ofi_dev *od = comm_ofi_owner(ptr); if (od) { comm_ofi_free(od, (size_t)((char *)ptr - comm_ofi_pool(od))); return; } }
    if (!in_pool(ptr)) die("sym_free of a pointer outside the pool");
    pool_free(off_of(ptr));
}
static void s_destroy(comm *c)
{
    shm_priv *p = PRIV(c);
    if (p->pending) s_wait(c);
    s_barrier(c);                                          /* nobody's block goes while a member may still write to it */
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); if (p->own_ctx) shmem_ctx_destroy(p->ctx); SHM_UNLOCK();
    staging_drop(p);
    comm_ofi_peers_free(p->op); free(p->ocnt); free(p->osig); free(p->pxpe);
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
    if (n < 1 || pe_stride < 1 || pe_start < 0 || pe_start + (n - 1) * pe_stride >= S.npes || id < 0 || id >= MAXID - (S.ppa ? 1 : 0)) die("bad PE set");   /* (N7b reserves the last row) */
    int me = -1; for (int r = 0; r < n; r++) if (pe_start + r * pe_stride == S.me) me = r;   /* (logical PEs = nodes; p->pe[] physical) */
    if (me < 0) die("this PE is not in the set");
    comm *c = (comm *)calloc(1, sizeof *c); shm_priv *p = (shm_priv *)calloc(1, sizeof *p);
    c->ops = &shm_ops; c->priv = p; c->rank = me; c->size = n; c->inflight = 1;
    p->n = n; p->me = me; p->id = id; p->pe = (int *)malloc(n * sizeof(int)); for (int r = 0; r < n; r++) p->pe[r] = phys_pe(pe_start + r * pe_stride);
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
    for (int r = 0; r < n; r++) { long *m = mailbox(id, pe_start + r * pe_stride); wait_ne(m, 0); p->rbase[r] = (size_t)(*(volatile long *)m - 1); }
    set_xo(p, 0);
    if (comm_ofi_enabled()) {                             /* the device's NICs and comm pool; the members' blobs over SHMEM */
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
    } else if (S.ppa) {                                   /* N7b: my APU's proxy carries the data; the members' proxies of the same APU (physical PEs) over SHMEM */
        int dev = 0;
#ifdef COMM_HOST_ONLY
        dev = getenv("COMM_OFI_DEV") ? atoi(getenv("COMM_OFI_DEV")) : id % 4;
#else
        HIP_CHECK(hipGetDevice(&dev));
#endif
        if (dev < 0 || dev > 3) die("COMM_SHMEM_PE_PER_APU: device outside 0..3");
        p->px = g_px[dev];
        long *pes = (long *)malloc((size_t)n * sizeof(long)), mine = p->px->pe; s_allgather_host(c, &mine, pes, sizeof(long));
        p->pxpe = (int *)malloc((size_t)n * sizeof(int)); for (int r = 0; r < n; r++) p->pxpe[r] = (int)pes[r]; free(pes);
        p->ocnt = (long *)calloc(n, sizeof(long)); p->osig = (long *)calloc(n, sizeof(long));
        p->ofi = 1;
    }
    return c;
}
#endif
