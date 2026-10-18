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
 * device exchanges moves, as under comm_ofi: APU thread d's staging and symmetric slabs live in proxy d's symmetric pool (mapped here from
 * the proxy's heap file, heap_mmap below, or shmem_ptr; HIP-registered when the driver allows), and the puts are posted by proxy d through its own NIC from a command ring in that pool;
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
#include <dirent.h>
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
static hipError_t pcopy(void *d, const void *s, size_t n, hipStream_t st);   /* N7D: COMM_SHMEM_PX_KCOPY (below) */
#define COPY(d, s, n, st) HIP_CHECK(pcopy(d, s, n, st))
#define SYNC(st) HIP_CHECK(hipStreamSynchronize(st))
#endif
#define MAXID 1024                         /* communicator ids (mailbox rows) */
#define ALIGN 256
enum { ORDER_QUIET, ORDER_FENCE, ORDER_PUTSIG };

/* ---- the process state ---- */
struct blk { size_t off, len; int used, kind; struct blk *next; };
static struct {
    int inited, me, npes, serial, devheap, order, registered, prov, shift, layv_any;
    long spin_us;
    char *pool; size_t pool_bytes, mb_bytes;   /* the symmetric pool; the mailbox at [0, mb_bytes) */
    pthread_mutex_t lock;                  /* SHM_LOCK: the library in serial mode; the allocator always (alloc_lock) */
    pthread_mutex_t alloc_lock;
    struct blk *blocks;                    /* the general area's blocks */
    struct blk *slot[4]; size_t slot_bytes; int nslot;   /* the staging regions, one per APU (COMM_SHMEM_STAGE_SLOT_MB, set by the pool rule) */
    size_t cur[3], peak[3], cur_all, peak_all;   /* pool accounting by kind: 0 control blocks, 1 staging, 2 the callers' symmetric buffers */
    size_t at_peak[3], nstage, nstage_at_peak, stage_blk_max, stage_rep;   /* the kinds at the moment of peak_all, live staging blocks, the largest one, the last staging peak reported */
    int verbose;                           /* COMM_SHMEM_VERBOSE=1 or ECALC_VERBOSE >= 2 -- a line per new staging peak (+5 %), every PE's summary */
    int xstats, xsany, pts, vslot_pool;                               /* X1: COMM_XSTATS=1 (per-kind exchange stats, printed at finalize) */
    size_t round_bytes;                    /* COMM_SHMEM_ROUND_MB -- the staging of one alltoallv round, each way (0: off, one round) */
    long nrpath, nround_ex, nrounds; size_t round_stage_max;   /* exchanges through the rounds' path, those in more than one round, their rounds, the largest per-round staging (send + recv) */
    double tv; long nv;                    /* all-to-all time (alltoall, alltoallv), post to completion, summed over the APU threads; the count */
    double bv;                             /* the bytes this PE received from the other PEs in those exchanges (the off-process traffic), summed */
    int ppa, ppn, pme, pnpes, proxy;       /* N7b COMM_SHMEM_PE_PER_APU: proxies per APU (0 off), PEs per node (1 + 4 ppa), my physical PE and the count, 1 = this PE is a proxy */
    size_t pxch;                           /* COMM_SHMEM_PX_BLOCK_MB: the chunk of a PX_SEND=2 put (default PXCH = 4 MiB; the same on every PE) */
    int pxsend;                            /* N7D COMM_SHMEM_PX_SEND: 1 (default) the proxy's batch-then-quiet, 2 chunked put-with-signal into the receiving proxy (px_serve2), 3 (PX3) plain chunks + one fence and signal word per command (px_serve2 too) */
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
static int g_heap_numa = -1, g_heap_mbind = -2;          /* N7C COMM_SHMEM_PX_NUMA=1: the NUMA node a proxy's heap prefers (-1: off), the mbind result */
/* N7G COMM_SHMEM_PX_THP (default 1 since D36, 2026-10-15; =0 restores 4 KiB pages): the helper pools in 2 MiB pages.  aac7 (probe 34_n7g_probe, 2026-10-15): shmem_enabled = never, /dev/shm without
 * huge=, no reserved hugepages -- so MADV_HUGEPAGE does nothing on shmem and MFD_HUGETLB fails -- but MADV_COLLAPSE works on /dev/shm files (1 GiB
 * fully PMD-mapped in 0.29 s).  So the heap file is mapped at an address = its offset mod 2 MiB (helper and node-process: a PMD maps only there) and
 * the helper pre-faults its pool and collapses it to 2 MiB pages before the barrier (pool_collapse).  Raw syscalls: no wrapper recursion. */
static int g_thp, g_thp_err, g_thp_thr = 4; static double g_thp_t; static void *g_heap_hint; static int g_heap_fl;
static void pool_populate(char *p, size_t n, int nt); static int pool_collapse(char *p, size_t n, int nt);
#define THP_SZ ((size_t)2 << 20)
#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif
static void *thp_mmap(size_t l, int prot, int fl, int fd, off_t o)
{
    size_t lp = (l + 4095) & ~(size_t)4095, r = lp + THP_SZ;
    char *v = (char *)syscall(SYS_mmap, (void *)0, r, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, (off_t)0);
    if (v == MAP_FAILED) return MAP_FAILED;
    char *a = v + (((uintptr_t)o - (uintptr_t)v) & (THP_SZ - 1));
    void *m = (void *)syscall(SYS_mmap, a, l, prot, fl | MAP_FIXED, fd, o);
    if (m == MAP_FAILED) { syscall(SYS_munmap, v, r); return MAP_FAILED; }
    if (a > v) syscall(SYS_munmap, v, (size_t)(a - v));
    if (a + lp < v + r) syscall(SYS_munmap, a + lp, (size_t)(v + r - (a + lp)));
    return m;
}
static void *heap_mmap(void *a, size_t l, int prot, int fl, int fd, off_t o)
{
    if (g_heap_arm && !g_heap_addr && (fl & MAP_SHARED) && l >= g_heap_min) {
        int zero = (fl & MAP_ANONYMOUS) != 0;
        if (!zero && fd >= 0) { struct stat st; zero = fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) && major(st.st_rdev) == 1 && minor(st.st_rdev) == 5; }
        if (zero) {
            int sfd = open(g_heap_path, O_RDWR | O_CREAT | O_EXCL, 0600);
            if (sfd >= 0 && ftruncate(sfd, (off_t)l) == 0) {
                int ff = fl & ~(MAP_ANONYMOUS | MAP_HUGETLB | (MAP_HUGE_MASK << MAP_HUGE_SHIFT)); g_heap_hint = a; g_heap_fl = fl;
                void *r = g_thp && !a && !(fl & MAP_FIXED) ? thp_mmap(l, prot, ff, sfd, (off_t)0)   /* N7G: 2 MiB aligned (a library hint is kept) */
                                                           : (void *)syscall(SYS_mmap, a, l, prot, ff, sfd, (off_t)0); close(sfd);
                if (r != MAP_FAILED && g_heap_numa >= 0 && g_heap_numa < 64) {   /* N7C: the file's shared policy, before any page is touched (the
                    * node-process's faults through its own mapping follow it too); preferred, not bound: a full node falls back */
                    unsigned long mask = 1UL << g_heap_numa; g_heap_mbind = (int)syscall(SYS_mbind, r, l, 1 /* MPOL_PREFERRED */, &mask, 64, 0) ? errno : 0; }
                if (r != MAP_FAILED && g_thp) { double tc = now_s(); pool_populate((char *)r, l, g_thp_thr); g_thp_err = pool_collapse((char *)r, l, g_thp_thr); g_thp_t = now_s() - tc; }
                    /* N7G: the whole heap in 2 MiB pages before the library registers it with the NIC (pinned small pages would not collapse) */
                if (r != MAP_FAILED) g_heap_addr = r; else unlink(g_heap_path);
                return r;
            }
            if (sfd >= 0) { close(sfd); unlink(g_heap_path); }
        }
    }
    typedef void *(*mmap_fn)(void *, size_t, int, int, int, off_t);
    static mmap_fn next;                                  /* every other call: libc's own mmap, as without the wrapper */
    if (!next) { mmap_fn f = (mmap_fn)dlsym(RTLD_NEXT, "mmap"); if (!f) return (void *)syscall(SYS_mmap, a, l, prot, fl, fd, o); __atomic_store_n(&next, f, __ATOMIC_RELEASE); }
    return next(a, l, prot, fl, fd, o);
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
struct px_ent { long pe; size_t roff, soff, n; long sigidx, sig; };   /* sigidx, sig: COMM_SHMEM_PX_SEND=2 only (the receiver's signal words, the final value) */
/* N7D COMM_SHMEM_PX_SEND=2: after the ring, a done stamp per command (k + 1 once command k's puts completed: retired out of order).
 * The signal words of an exchange live in the receiving proxy's pool, in its mailbox place (unused by proxies; doubled under the
 * switch): word (0, id, r) sums the bytes of sender r's chunks before its last, word (1, id, r) = PACK(seq, n) put by the last chunk. */
#define PXCH ((size_t)4 << 20)                 /* the chunk of a put (comm_ofi's 4 MiB) */
#define PXNCTX 8                               /* the proxy's contexts: a peer node to each (round robin beyond 8) */
_Static_assert(sizeof(struct px_hdr) + PXRING * sizeof(struct px_ent) + PXRING * sizeof(long) <= PXCTL, "proxy control");
static long *px_dn_of(char *base) { return (long *)(base + S.mb_bytes + sizeof(struct px_hdr) + PXRING * sizeof(struct px_ent)); }
static long *px_sigw(char *base, int w, long idx) { return (long *)base + (size_t)w * MAXID * S.npes + (size_t)idx; }
static size_t px_last_len(size_t n) { return S.pxsend == 3 ? n : n ? n - ((n - 1) / S.pxch) * S.pxch : 0; }   /* (PX_SEND=3: the one signal word announces it all; PX_SEND=2: S.pxch, COMM_SHMEM_PX_BLOCK_MB, is PXCH by default) */
/* N7E COMM_SHMEM_PE_PER_APU=2: an APU's two helper PEs h = 0, 1 (local PEs 2d, 2d + 1, on their own NICs).  The APU's staging/slab area is ONE virtual
 * range `base` (span bytes) mapped by the node-process: [0, PXDATA) is helper 0's control, then 4 MiB blocks (PXCH) alternate between the
 * helpers' heaps (block b of the data area = helper b & 1, local offset PXDATA + (b >> 1) PXCH + ...).  Every offset in the protocol is a
 * virtual one; px_write cuts a put at the blocks' ends of its source and of its destination and gives each piece to the helper that holds the
 * source, aimed at the helper (of the receiving node) that holds the destination.  So both NICs of the APU carry one exchange, each helper
 * receives into its own heap, and the callers, the rounds and the signals are unchanged. */
typedef struct px_hl { char *base; pthread_mutex_t lock; long retired; double tlast; long *cnt[PXRING]; } px_hl;   /* one helper: its heap's control, its ring's state */
typedef struct px_dev { char *base; size_t span; int pe, reg, d, he, nh; double treg; pthread_mutex_t lock; struct blk *blocks; size_t cur, peak; px_hl hl[2]; } px_dev;   /* lock: the allocator */
static px_dev *g_px[4];
static int px_in(const px_dev *x, const void *q);
#ifndef COMM_HOST_ONLY
static int g_kcopy = -1;                                /* N7D COMM_SHMEM_PX_KCOPY=1: copies touching a proxy pool run as a kernel, not on the SDMA engine */
static int px_reg_in(const void *p) { for (int d = 0; d < 4; d++) if (g_px[d] && g_px[d]->reg && px_in(g_px[d], p)) return 1; return 0; }
static hipError_t pcopy(void *d, const void *s, size_t n, hipStream_t st)
{
    if (g_kcopy < 0) { const char *e = getenv("COMM_SHMEM_PX_KCOPY"); g_kcopy = e ? atoi(e) > 0 : S.ppa > 0; }   /* D31: default on in the proxy mode */
    if (g_kcopy && (px_reg_in(d) || px_reg_in(s))) return comm_copy_async(d, s, n, st);
    return hipMemcpyAsync(d, s, n, hipMemcpyDefault, st);
}
#endif
static struct px_hdr *px_hdr_of(char *base) { return (struct px_hdr *)(base + S.mb_bytes); }
static struct px_ent *px_ring_of(char *base) { return (struct px_ent *)(base + S.mb_bytes + sizeof(struct px_hdr)); }
static int px_in(const px_dev *x, const void *q) { return x && (const char *)q >= x->base && (const char *)q < x->base + x->span; }
static size_t px_alloc_q(px_dev *x, size_t len, int quiet)   /* quiet (FA): (size_t)-1 instead of the stop when it does not fit; FA's inbox: from the top */
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&x->lock);
    if (quiet) {                                          /* N7F: an inbox (kept for the communicator's life) is carved from the END of the last free block that holds
                                                           * it, so the inboxes stack at the pool's top and the staging/slabs (first fit from the bottom, freed and
                                                           * regrown) keep one contiguous range below them; first fit put an inbox between them (1_F: 1023 MiB did
                                                           * not fit with 947 of 2816 in use) */
        struct blk *t = 0; for (struct blk *b = x->blocks; b; b = b->next) if (!b->used && b->len >= len) t = b;
        if (!t) { pthread_mutex_unlock(&x->lock); return (size_t)-1; }
        if (t->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = t->off + t->len - len; nb->len = len; nb->used = 0; nb->next = t->next; t->next = nb; t->len -= len; t = nb; }
        t->used = 1; x->cur += len; if (x->cur > x->peak) x->peak = x->cur;
        pthread_mutex_unlock(&x->lock); return t->off;
    }
    for (struct blk *b = x->blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1; x->cur += len; if (x->cur > x->peak) x->peak = x->cur;
        pthread_mutex_unlock(&x->lock); return b->off;
    }
    size_t cur = x->cur; pthread_mutex_unlock(&x->lock);
    if (quiet) return (size_t)-1;
    ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: proxy pool of APU %d (%zu MiB, COMM_SHMEM_POOL_MB per PE under COMM_SHMEM_PE_PER_APU) cannot hold %zu MiB more (in use %zu MiB); the model's need: %s MiB\n",
             S.me, x->d, S.pool_bytes >> 20, len >> 20, cur >> 20, getenv("COMM_SHMEM_POOL_NEED_MB") ? getenv("COMM_SHMEM_POOL_NEED_MB") : "?");
}
static size_t px_alloc(px_dev *x, size_t len, int sym) { (void)sym; return px_alloc_q(x, len, 0); }
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
static void px_progress_hl(px_dev *x, px_hl *l)
{
    struct px_hdr *h = px_hdr_of(l->base);
    pthread_mutex_lock(&l->lock);
    int any = 0; long hd = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
    if (S.pxsend >= 2) {                                  /* N7D: each command as soon as its own stamp says done (out of order) */
        long *dn = px_dn_of(l->base);
        for (long k = l->retired; k < hd; k++) { long **c = &l->cnt[k % PXRING];
            if (*c && __atomic_load_n(&dn[k % PXRING], __ATOMIC_ACQUIRE) == k + 1) { __atomic_sub_fetch(*c, 1, __ATOMIC_RELEASE); *c = 0; any = 1; } }
        while (l->retired < hd && !l->cnt[l->retired % PXRING]) l->retired++;
    } else {
        long d = __atomic_load_n(&h->done, __ATOMIC_ACQUIRE);
        for (; l->retired < d; l->retired++) { __atomic_sub_fetch(l->cnt[l->retired % PXRING], 1, __ATOMIC_RELEASE); any = 1; }
    }
    if (any) l->tlast = now_s();
    else if (l->retired < hd) {                           /* commands outstanding, none completed: is the proxy still there? */
        double t = now_s(); if (!l->tlast) l->tlast = t;
        if (t - l->tlast > 30) { long pid = __atomic_load_n(&h->ppid, __ATOMIC_ACQUIRE); l->tlast = t;
            if (pid > 0 && kill((pid_t)pid, 0) != 0) { pthread_mutex_unlock(&l->lock); ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: the proxy of APU %d (pid %ld) is gone\n", S.me, x->d, pid); } }
    }
    pthread_mutex_unlock(&l->lock);
}
static void px_progress(px_dev *x) { for (int h = 0; h < x->nh; h++) px_progress_hl(x, &x->hl[h]); }
/* post n bytes from src (inside proxy x's pool) to the pool offset roff of PE dst_pe (another node's proxy of the same APU);
 * under COMM_SHMEM_PX_SEND=2 also the receiver's signal words (sigidx = id * npes + my rank) and the final value sig (n may be 0) */
static size_t px_d0(void) { return (S.mb_bytes + PXCTL + 4095) & ~(size_t)4095; }   /* the data area of the interleaved range starts on a page */
static void px_post(px_dev *x, int hh, int dst_pe, size_t roff, size_t soff, size_t n, long *cnt, long sigidx, long sig)   /* one command to helper hh's ring (offsets local to the heaps) */
{
    px_hl *l = &x->hl[hh]; struct px_hdr *h = px_hdr_of(l->base); struct px_ent *ring = px_ring_of(l->base);
    pthread_mutex_lock(&l->lock);
    while (h->head - l->retired >= PXRING) { pthread_mutex_unlock(&l->lock); px_progress_hl(x, l); sched_yield(); pthread_mutex_lock(&l->lock); }
    long k = h->head; struct px_ent *e = &ring[k % PXRING];
    e->pe = dst_pe; e->roff = roff; e->soff = soff; e->n = n; e->sigidx = sigidx; e->sig = sig;
    l->cnt[k % PXRING] = cnt; __atomic_add_fetch(cnt, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&h->head, k + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&l->lock);
}
/* dst_pe: the receiving node's helper 0 (helper hd is dst_pe + hd) */
static void px_write(px_dev *x, int dst_pe, size_t roff, const void *src, size_t n, long *cnt, long sigidx, long sig)
{
    if (n && (!px_in(x, src) || !px_in(x, (const char *)src + n - 1))) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: a proxy put's source is outside the proxy pool\n", S.me);
    if (x->nh == 1) { px_post(x, 0, dst_pe, roff, n ? (size_t)((const char *)src - x->base) : 0, n, cnt, sigidx, sig); return; }
    const size_t d0 = px_d0(); size_t sv = n ? (size_t)((const char *)src - x->base) : 0;
    if (sig || sigidx) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PE_PER_APU=2 does not carry signals with the data\n");
    while (n) {
        if (sv < d0 || roff < d0) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: a proxy put outside the data area (COMM_SHMEM_PE_PER_APU=2)\n", S.me);
        size_t us = sv - d0, ud = roff - d0, len = n;
        if (len > PXCH - us % PXCH) len = PXCH - us % PXCH;
        if (len > PXCH - ud % PXCH) len = PXCH - ud % PXCH;
        int hs = (int)((us / PXCH) & 1), hd = (int)((ud / PXCH) & 1);
        px_post(x, hs, dst_pe + hd, d0 + (ud / PXCH / 2) * PXCH + ud % PXCH, d0 + (us / PXCH / 2) * PXCH + us % PXCH, len, cnt, 0, 0);
        sv += len; roff += len; n -= len;
    }
}
/* N7D COMM_SHMEM_PX_SEND=2: a proxy's life without batch-then-quiet.  Every command is cut into PXCH chunks, posted round robin over the
 * commands in the ring (so the peers' transfers interleave) on the context of its destination node (up to PXNCTX contexts: a slow peer's
 * completion does not hold the others').  Each chunk is a put-with-signal into the receiving proxy's pool: the chunks before the last add
 * their bytes to word 0, the last sets word 1 to PACK(seq, n) -- the receiver needs no second hop through the sending node-process.  A
 * command is reported done (its stamp) after the quiet of its own context; between two contexts' quiets new commands are taken and posted.
 * PX3 (COMM_SHMEM_PX_SEND=3, Phase C step 3): the same loop with plain shmem_ctx_putmem_nbi chunks and, after a command's last chunk, one
 * shmem_ctx_fence on the peer's context + the signal word (set to PACK(seq, n)): the receiver waits for that word alone (px_last_len = n, no
 * chunk-byte word).  Measured 2026-10-18 (ecalc mpss4, t_comm 72b): the 4 MiB put-with-signal chunks of PX_SEND=2 post at 14.8-14.9 GB/s per NIC
 * against 19-20 for whole puts; PX3 keeps 2's structure (no batch coupling, every peer in flight) with the plain put's rate. */
static void px_serve2(void)
{
    struct px_hdr *h = px_hdr_of(S.pool); struct px_ent *ring = px_ring_of(S.pool); long *dn = px_dn_of(S.pool);
    __atomic_store_n(&h->ppid, (long)getpid(), __ATOMIC_RELEASE);
    shmem_ctx_t cx[PXNCTX]; int nc = 0, peers = S.npes - 1 < PXNCTX ? S.npes - 1 : PXNCTX;
    while (nc < peers && shmem_ctx_create(SHMEM_CTX_PRIVATE, &cx[nc]) == 0) nc++;
    int own = nc; if (!nc) { cx[0] = SHMEM_CTX_DEFAULT; nc = 1; }
    static size_t posted[PXRING]; static unsigned char fp[PXRING], fd[PXRING], co[PXRING];   /* bytes posted, fully posted, done, context */
    long t = 0, tp = 0, tl = 0, nput = 0, nq = 0, nsig = 0; double bytes = 0, tchk = now_s(), tbusy = 0; unsigned idle = 0;
    for (;;) {
        long hd = __atomic_load_n(&h->head, __ATOMIC_ACQUIRE);
        for (; t < hd; t++) { int s = (int)(t % PXRING); int node = (int)ring[s].pe / S.ppn; posted[s] = 0; fp[s] = fd[s] = 0; co[s] = (unsigned char)(((node - S.me - 1 + S.npes) % S.npes) % nc); }
        if (tp < t) {                                     /* post: a chunk of every command not yet posted, round by round */
            double t0 = now_s();
            for (int more = 1; more;) { more = 0;
                for (long k = tp; k < t; k++) { int s = (int)(k % PXRING); if (fp[s]) continue;
                    struct px_ent *e = &ring[s]; shmem_ctx_t c = cx[co[s]];
                    if (!e->n) { shmem_ctx_long_p(c, px_sigw(S.pool, 1, e->sigidx), e->sig, (int)e->pe); fp[s] = 1; continue; }
                    size_t o = posted[s], len = e->n - o < S.pxch ? e->n - o : S.pxch; int last = o + len == e->n;
                    if (S.pxsend == 3) {                  /* PX3: a plain chunk; the command's last chunk is followed by a fence and its signal word */
                        shmem_ctx_putmem_nbi(c, S.pool + e->roff + o, S.pool + e->soff + o, len, (int)e->pe);
                        if (last) { shmem_ctx_fence(c); shmem_ctx_long_p(c, px_sigw(S.pool, 1, e->sigidx), e->sig, (int)e->pe); nsig++; }
                    } else
                    shmem_ctx_putmem_signal_nbi(c, S.pool + e->roff + o, S.pool + e->soff + o, len, (uint64_t *)px_sigw(S.pool, last, e->sigidx),
                                                last ? (uint64_t)e->sig : (uint64_t)len, last ? SHMEM_SIGNAL_SET : SHMEM_SIGNAL_ADD, (int)e->pe);
                    posted[s] = o + len; nput++; bytes += (double)len;
                    if (last) fp[s] = 1; else more = 1;
                }
            }
            while (tp < t && fp[tp % PXRING]) tp++;
            tbusy += now_s() - t0; idle = 0; continue;    /* (new commands first, then a quiet) */
        }
        if (tl < t) {                                     /* complete the context of the oldest command not done; then take new commands */
            double t0 = now_s(); int c = co[tl % PXRING];
            shmem_ctx_quiet(cx[c]); nq++;
            for (long k = tl; k < t; k++) { int s = (int)(k % PXRING); if (!fd[s] && co[s] == c) { fd[s] = 1; __atomic_store_n(&dn[s], k + 1, __ATOMIC_RELEASE); } }
            while (tl < t && fd[tl % PXRING]) tl++;
            __atomic_store_n(&h->done, tl, __ATOMIC_RELEASE);
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
    for (int i = 0; i < own; i++) shmem_ctx_destroy(cx[i]);
    if (S.verbose || S.pme < S.ppn) fprintf(stderr, "comm_shmem: proxy pe %d (node %d, APU %d): COMM_SHMEM_PX_SEND=%d, %d contexts: %ld chunks, %.2f GB, %ld context quiets, %ld fence+signal, %.2f s posting + quiet = %.2f GB/s while busy\n",
                                           S.pme, S.pme / S.ppn, S.pme % S.ppn, S.pxsend, own, nput, bytes / 1e9, nq, nsig, tbusy, tbusy > 0 ? bytes / 1e9 / tbusy : 0);
}
/* the receiver's half under COMM_SHMEM_PX_SEND=2: sender r's message of sequence seq (want bytes) has arrived in my proxy's pool */
static void px_spin_ge(long *w, long v) { for (unsigned spins = 0; __atomic_load_n(w, __ATOMIC_ACQUIRE) < v; spins++) if (spins > 64) sched_yield(); }
/* a proxy's life: post every command in its ring, quiet, report them done; leave on quit (or when the node-process is gone) */
static void px_serve(void)
{
    if (S.pxsend >= 2) { px_serve2(); return; }
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
/* ---- N7D: faster proxy-mode init (COMM_SHMEM_PX_INIT=1, default 0) ----
 * The registration of a pool faults its 4 KiB pages one by one (0.53 s/GB, I.md) and the node-process registers five pools in turn.
 * With the switch: every PE pre-faults its own pool with MADV_POPULATE_WRITE in COMM_SHMEM_PX_INIT_THREADS (4) threads, a proxy before the
 * barrier in its own process (its pages follow the heap file's NUMA policy, and its faults overlap those of the other proxies and of the
 * node-process), and the node-process registers the four proxy pools and its own in five threads after the barrier.  No byte changes.
 * COMM_SHMEM_VERBOSE=1 prints the init timeline of every PE (g_t0 = entry of comm_shmem_init) with or without the switch. */
#ifndef MADV_POPULATE_WRITE
#define MADV_POPULATE_WRITE 23
#endif
static double g_t0, g_t_own, g_t_pop, g_t_shinit;   /* entry of comm_shmem_init; own pool's registration (PX_INIT=1); pre-fault; shmem_init */
struct pop_job { char *p; size_t n; int err; };   /* err: errno of the first failed madvise (0 = none) */
static int g_pop_err;                                /* errno of MADV_POPULATE_WRITE in the last pool_populate (0 = it worked) */
static void *pop_thread(void *a) { struct pop_job *j = (struct pop_job *)a; const size_t C = (size_t)256 << 20; size_t o = 0; j->err = 0;
    for (; o < j->n; o += C) { size_t l = j->n - o < C ? j->n - o : C; if (madvise(j->p + o, l, MADV_POPULATE_WRITE)) { j->err = errno ? errno : -1; break; } }
    /* fallback (madvise unsupported or refused): fault the rest by an atomic no-op write per 4 KiB page (no byte changes, safe under concurrent puts) */
    for (; o < j->n; o += 4096) __atomic_fetch_or((volatile unsigned long *)(j->p + o), 0UL, __ATOMIC_RELAXED);
    return 0; }
static void pool_populate(char *p, size_t n, int nt)    /* nt threads over page-aligned slices; COMM_SHMEM_VERBOSE prints a madvise error (then the pages were touched) */
{
    if (nt < 1) nt = 1; if (nt > 16) nt = 16;
    pthread_t th[16]; struct pop_job jb[16]; size_t sl = ((n / nt) + 4095) & ~(size_t)4095; int k = 0;
    uintptr_t b = (uintptr_t)p & ~(uintptr_t)4095; n = (n + ((uintptr_t)p - b) + 4095) & ~(size_t)4095;
    for (int i = 0; i < nt && (size_t)i * sl < n; i++) { jb[i].p = (char *)b + (size_t)i * sl; jb[i].n = n - (size_t)i * sl < sl ? n - (size_t)i * sl : sl; jb[i].err = 0;
        if (pthread_create(&th[i], 0, pop_thread, &jb[i])) { pop_thread(&jb[i]); th[i] = 0; } k++; }
    g_pop_err = 0;
    for (int i = 0; i < k; i++) { if (th[i]) pthread_join(th[i], 0); if (jb[i].err && !g_pop_err) g_pop_err = jb[i].err; }
}
/* N7G COMM_SHMEM_PX_THP=1: the pool's whole 2 MiB pages collapsed to huge pages (MADV_COLLAPSE) in nt threads after pool_populate; returns the
 * first errno (0 = all collapsed).  The new huge pages stay on the node of the small pages they replace (COMM_SHMEM_PX_NUMA's policy). */
static void *col_thread(void *a) { struct pop_job *j = (struct pop_job *)a; const size_t C = (size_t)64 << 20; j->err = 0;
    for (size_t o = 0; o < j->n; o += C) if (madvise(j->p + o, j->n - o < C ? j->n - o : C, MADV_COLLAPSE) && !j->err) j->err = errno ? errno : -1;
    return 0; }
static int pool_collapse(char *p, size_t n, int nt)
{
    nt = nt < 1 ? 1 : nt > 16 ? 16 : nt;
    uintptr_t b = ((uintptr_t)p + THP_SZ - 1) & ~(uintptr_t)(THP_SZ - 1), e = ((uintptr_t)p + n) & ~(uintptr_t)(THP_SZ - 1);
    if (e <= b) return 0;
    size_t m = (e - b) / THP_SZ, per = (m + nt - 1) / nt; pthread_t th[16]; struct pop_job jb[16]; int k = 0, err = 0;
    for (size_t i = 0; i * per < m; i++) { size_t c = m - i * per < per ? m - i * per : per; jb[k].p = (char *)b + i * per * THP_SZ; jb[k].n = c * THP_SZ; jb[k].err = 0;
        if (pthread_create(&th[k], 0, col_thread, &jb[k])) { col_thread(&jb[k]); th[k] = 0; } k++; }
    for (int i = 0; i < k; i++) { if (th[i]) pthread_join(th[i], 0); if (jb[i].err && !err) err = jb[i].err; }
    return err;
}
static long smaps_kb(const void *p, const char *key)    /* N7G: a field (kB) of the mapping holding p in /proc/self/smaps (-1: not found) */
{
    FILE *f = fopen("/proc/self/smaps", "r"); char ln[512]; int in = 0; long v = -1; size_t kl = strlen(key);
    while (f && fgets(ln, sizeof ln, f)) { unsigned long lo, hi;
        if (sscanf(ln, "%lx-%lx ", &lo, &hi) == 2 && strchr(ln, '-') < strchr(ln, ' ')) { if (in) break; in = (uintptr_t)p >= lo && (uintptr_t)p < hi; continue; }
        if (in && !strncmp(ln, key, kl) && ln[kl] == ':') sscanf(ln + kl + 1, "%ld", &v); }
    if (f) fclose(f);
    return v;
}
/* N7G COMM_SHMEM_PX_PIN=1 (default 0): helper h of APU d runs on one CPU of its own -- the (h+1)-th highest allowed CPU of the NUMA node of cxi d (as
 * COMM_SHMEM_PX_NUMA), set for all its threads before shmem_init (the library's threads inherit it) -- and the node-process removes those CPUs from
 * the masks of all its threads (its OpenMP workers and later threads inherit that).  The launch stays --cpu-bind=none: every PE sees the same mask. */
static int g_pin;
static int pin_cpu_of(int d, int h, const cpu_set_t *ok)
{
    char p[96], buf[1024]; int nd = d, lo, hi; cpu_set_t s; CPU_ZERO(&s);
    snprintf(p, sizeof p, "/sys/class/cxi/cxi%d/device/numa_node", d); FILE *f = fopen(p, "r"); if (f) { int n = -1; if (fscanf(f, "%d", &n) == 1 && n >= 0) nd = n; fclose(f); }
    snprintf(p, sizeof p, "/sys/devices/system/node/node%d/cpulist", nd); f = fopen(p, "r"); if (!f) return -1;
    if (fgets(buf, sizeof buf, f)) for (char *t = strtok(buf, ",\n"); t; t = strtok(0, ",\n")) {
        int k = sscanf(t, "%d-%d", &lo, &hi); if (k == 1) hi = lo; if (k >= 1) for (int c = lo; c <= hi && c < CPU_SETSIZE; c++) CPU_SET(c, &s); }
    fclose(f);
    for (int c = CPU_SETSIZE - 1, k = 0; c >= 0; c--) if (CPU_ISSET(c, &s) && CPU_ISSET(c, ok) && k++ == h) return c;
    return -1;
}
static int pin_tasks(const cpu_set_t *only, const cpu_set_t *drop)   /* every thread of this process: mask = only, or its mask minus drop; returns threads set */
{
    int n = 0; DIR *dd = opendir("/proc/self/task"); struct dirent *de;
    if (!dd) return -1;
    while ((de = readdir(dd))) { pid_t t = (pid_t)atoi(de->d_name); cpu_set_t m; if (t <= 0) continue;
        if (only) m = *only; else { if (sched_getaffinity(t, sizeof m, &m)) continue; for (int c = 0; c < CPU_SETSIZE; c++) if (CPU_ISSET(c, drop)) CPU_CLR(c, &m); if (!CPU_COUNT(&m)) continue; }
        if (!sched_setaffinity(t, sizeof m, &m)) n++; }
    closedir(dd);
    return n;
}
static void pin_start(void)   /* before shmem_init: a helper pins itself, the node-process gives the helpers' CPUs up */
{
    cpu_set_t ok, drop; char list[256] = ""; int nd = 0;
    if (sched_getaffinity(0, sizeof ok, &ok)) return;
    const char *ex = getenv("COMM_SHMEM_PROXY"); int lid = getenv("SLURM_LOCALID") ? atoi(getenv("SLURM_LOCALID")) : 0;
    if (ex && atoi(ex)) { int c = pin_cpu_of(lid / S.ppa, lid % S.ppa, &ok); if (c < 0) return; CPU_ZERO(&drop); CPU_SET(c, &drop);
        int n = pin_tasks(&drop, 0); if (S.verbose || !(getenv("SLURM_NODEID") && atoi(getenv("SLURM_NODEID")))) fprintf(stderr, "comm_shmem: helper (local %d, APU %d): COMM_SHMEM_PX_PIN=1: %d threads pinned to cpu %d\n", lid, lid / S.ppa, n, c); return; }
    CPU_ZERO(&drop);
    for (int d = 0; d < 4; d++) for (int h = 0; h < S.ppa; h++) { int c = pin_cpu_of(d, h, &ok); if (c >= 0 && !CPU_ISSET(c, &drop)) { CPU_SET(c, &drop); nd++; size_t l = strlen(list); snprintf(list + l, sizeof list - l, "%s%d", l ? "," : "", c); } }
    if (!nd) return;
    int n = pin_tasks(0, &drop);
    if (S.verbose || !(getenv("SLURM_NODEID") && atoi(getenv("SLURM_NODEID")))) fprintf(stderr, "comm_shmem: node-process: COMM_SHMEM_PX_PIN=1: the helpers' cpus %s removed from %d threads (%d cpus left)\n", list, n, CPU_COUNT(&ok) - nd);
}
#ifndef COMM_HOST_ONLY
struct reg_job { char *p; size_t n; hipError_t he; int dpok; double t; };
static void *reg_thread(void *a) { struct reg_job *j = (struct reg_job *)a; double t = now_s(); void *dp = 0;
    j->he = hipHostRegister(j->p, j->n, hipHostRegisterPortable); j->dpok = 0;
    if (j->he != hipSuccess) (void)hipGetLastError();
    else { j->dpok = hipHostGetDevicePointer(&dp, j->p, 0) == hipSuccess && dp == (void *)j->p; if (!j->dpok) (void)hipGetLastError(); }
    j->t = now_s() - t; return 0; }
#endif
static int g_meshpar;                                  /* N7F-C: COMM_SHMEM_MESH_PAR */
static int g_pxinit, g_pxthr = 4;                       /* COMM_SHMEM_PX_INIT, COMM_SHMEM_PX_INIT_THREADS */
/* the node-process: attach the proxies' pools (shmem_ptr: Cray's on-node XPMEM), register them with HIP, start the allocators */
/* N7E (COMM_SHMEM_PE_PER_APU=2): map APU x->d's two helper heaps (named files) as the interleaved virtual range of px_dev */
static void px_map2(px_dev *x)
{
    const size_t d0 = px_d0(), dsz = (S.pool_bytes - d0) / PXCH * PXCH;
    x->span = d0 + 2 * dsz;
    int fd[2]; long off[2];
    for (int h = 0; h < 2; h++) {
        char path[160]; heap_path(path, sizeof path, x->pe + h); struct stat st;
        if (shmem_ptr(S.pool, x->pe + h)) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU=2 maps the helpers' heap files (a library with an on-node pointer path is not supported)\n", S.me);
        off[h] = shmem_long_g(mailbox(MAXID - 1, 1), x->pe + h);
        fd[h] = open(path, O_RDWR);
        if (fd[h] < 0 || fstat(fd[h], &st) != 0 || off[h] < 0 || (off[h] & 4095) || (size_t)off[h] + S.pool_bytes > (size_t)st.st_size)
            ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU=2: the heap of helper PE %d is not reachable (%s: %s; pool offset %ld)\n", S.me, x->pe + h, path, fd[h] < 0 ? strerror(errno) : "too small or unaligned", off[h]);
        unlink(path);
    }
    char *v = (char *)mmap(0, x->span + (g_thp ? THP_SZ : 0), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int bad = v == MAP_FAILED;
    if (!bad && g_thp) v = (char *)(((uintptr_t)v + THP_SZ - 1) & ~(uintptr_t)(THP_SZ - 1));   /* N7G: the heaps' offsets are 2 MiB multiples (shmem_align) */
    if (!bad) bad = mmap(v, d0, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd[0], off[0]) == MAP_FAILED;
    char *c1 = bad ? (char *)MAP_FAILED : (char *)mmap(0, d0, PROT_READ | PROT_WRITE, MAP_SHARED, fd[1], off[1]);
    if (c1 == MAP_FAILED) bad = 1;
    for (size_t b = 0; !bad && b < 2 * dsz / PXCH; b++)
        bad = mmap(v + d0 + b * PXCH, PXCH, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd[b & 1], off[b & 1] + (long)(d0 + (b >> 1) * PXCH)) == MAP_FAILED;
    if (bad) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU=2: mapping the helpers' heaps of APU %d: %s\n", S.me, x->d, strerror(errno));
    close(fd[0]); close(fd[1]);
    x->base = v; x->hl[0].base = v; x->hl[1].base = c1;
}
static void px_attach(void)
{
    for (int d = 0; d < 4; d++) {
        px_dev *x = (px_dev *)calloc(1, sizeof *x); g_px[d] = x; x->d = d;
        x->pe = S.me * S.ppn + d * S.ppa;                 /* APU d's helper 0 on my node (local PE d ppa; helper h is +h) */
        x->nh = S.ppa; x->span = S.pool_bytes;
        if (S.ppa == 2) px_map2(x);
        else x->base = (char *)shmem_ptr(S.pool, x->pe);  /* (a library with an on-node path; aac7's has none) */
        if (!x->base) {                                   /* the proxy's heap file (heap_mmap), at the pool's offset in the heap (the proxy's mailbox word) */
            char path[160]; heap_path(path, sizeof path, x->pe);
            long off = shmem_long_g(mailbox(MAXID - 1, 1), x->pe);
            int fd = open(path, O_RDWR); struct stat st;
            if (fd < 0 || fstat(fd, &st) != 0 || off < 0 || (size_t)off + S.pool_bytes > (size_t)st.st_size)
                ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU: the heap of proxy PE %d is not reachable (%s: %s; pool offset %ld)\n", S.me, x->pe, path, fd < 0 ? strerror(errno) : "too small", off);
            char *m = g_thp ? (char *)thp_mmap((size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)   /* N7G: PMD-mappable */
                            : (char *)mmap(0, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); close(fd); unlink(path);
            if (m == MAP_FAILED) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: mmap of %s: %s\n", S.me, path, strerror(errno));
            x->base = m + off; x->hl[0].base = x->base;
        }
        for (int h = 0; h < x->nh; h++) {
            if (px_hdr_of(x->hl[h].base)->pad[0] != 0x4e374250) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PE_PER_APU: the proxy PE %d's pool is not the memory mapped here (no mark)\n", S.me, x->pe + h);
            pthread_mutex_init(&x->hl[h].lock, 0);
        }
        pthread_mutex_init(&x->lock, 0);
        x->blocks = (struct blk *)malloc(sizeof *x->blocks); x->blocks->off = S.ppa == 2 ? px_d0() : S.mb_bytes + PXCTL; x->blocks->used = 0; x->blocks->next = 0;
        x->blocks->len = S.ppa == 2 ? x->span - x->blocks->off : S.pool_bytes - S.mb_bytes - PXCTL;
        x->reg = 1;
    }
#ifndef COMM_HOST_ONLY
    double t_reg = now_s();
    if (g_pxinit) {                                       /* N7D: the four proxy pools and the node-process's own pool (registered later than in the serial path) at once */
        pthread_t th[5]; struct reg_job jb[5];
        for (int i = 0; i < 5; i++) { jb[i].p = i < 4 ? g_px[i]->base : S.pool; jb[i].n = i < 4 ? g_px[i]->span : S.pool_bytes;
            if (pthread_create(&th[i], 0, reg_thread, &jb[i])) { reg_thread(&jb[i]); th[i] = 0; } }
        for (int i = 0; i < 5; i++) if (th[i]) pthread_join(th[i], 0);
        for (int i = 0; i < 4; i++) { g_px[i]->he = (int)jb[i].he; g_px[i]->reg = jb[i].he == hipSuccess && jb[i].dpok; g_px[i]->treg = jb[i].t;
            if (jb[i].he == hipSuccess && !jb[i].dpok) { (void)hipHostUnregister(g_px[i]->base); (void)hipGetLastError(); } }
        S.registered = jb[4].he == hipSuccess ? (jb[4].dpok ? 1 : 2) : 0; g_t_own = jb[4].t;
        if (!S.registered) { if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies, no symmetric slabs\n", hipGetErrorString(jb[4].he)); }
        else if (S.registered == 2 && S.me == 0) fprintf(stderr, "comm_shmem: the pool's device pointer differs from its host pointer: no symmetric slabs\n");
    } else if (g_meshpar) {                               /* N7F-C: COMM_SHMEM_MESH_PAR=1: the four proxy pools registered at once (the own pool stays where it is) */
        pthread_t th[4]; struct reg_job jb[4];
        for (int i = 0; i < 4; i++) { jb[i].p = g_px[i]->base; jb[i].n = g_px[i]->span; jb[i].he = hipSuccess; jb[i].dpok = 0; jb[i].t = 0;
            if (pthread_create(&th[i], 0, reg_thread, &jb[i])) { reg_thread(&jb[i]); th[i] = 0; } }
        for (int i = 0; i < 4; i++) if (th[i]) pthread_join(th[i], 0);
        for (int i = 0; i < 4; i++) { px_dev *x = g_px[i]; x->he = (int)jb[i].he; x->reg = jb[i].he == hipSuccess && jb[i].dpok; x->treg = jb[i].t;
            if (jb[i].he == hipSuccess && !jb[i].dpok) { (void)hipHostUnregister(x->base); (void)hipGetLastError(); } }
    } else for (int d = 0; d < 4; d++) { px_dev *x = g_px[d]; struct reg_job j = { x->base, x->span, hipSuccess, 0, 0 }; reg_thread(&j);
        x->he = (int)j.he; x->reg = j.he == hipSuccess && j.dpok; x->treg = j.t; if (j.he == hipSuccess && !j.dpok) { (void)hipHostUnregister(x->base); (void)hipGetLastError(); } }
    if (S.verbose) printf("comm_shmem: pe %d: init timeline (s since entry): shmem_init %.2f, own pool pre-fault %.2f, proxy pools registered %s in %.2f s (pools %.2f %.2f %.2f %.2f; own pool %.2f %s), px_attach done at %.2f\n", S.me, g_t_shinit, g_t_pop, g_pxinit || g_meshpar ? "in parallel" : "one by one", now_s() - t_reg,
                          g_px[0]->treg, g_px[1]->treg, g_px[2]->treg, g_px[3]->treg, g_t_own, g_pxinit ? "in the same threads" : "before the barrier", now_s() - g_t0);
    for (int d = 0; d < 4; d++) { px_dev *x = g_px[d]; hipError_t he = (hipError_t)x->he;
        if (S.me == 0 && d == 0) printf("comm_shmem: COMM_SHMEM_PE_PER_APU=%d: the proxy pools %s (%s)\n", S.ppa, x->reg ? "are HIP-registered: symmetric slabs in them" : "could not be HIP-registered: every device exchange is staged (pageable copies)", x->reg ? "zero-copy" : hipGetErrorString(he));
        if (S.me == 0 || S.verbose) {                     /* N7C: where the pool's pages are (64 sampled pages, after the registration faulted them) vs APU d's node */
            enum { NS = 64 }; void *pg[NS]; int st[NS], cnt[9] = { 0 }; size_t step = (S.pool_bytes / NS) & ~(size_t)4095;
            for (int i = 0; i < NS; i++) pg[i] = x->base + (size_t)i * step;
            long rc = syscall(SYS_move_pages, 0, (unsigned long)NS, pg, (const int *)0, st, 0);
            for (int i = 0; i < NS; i++) cnt[rc == 0 && st[i] >= 0 && st[i] < 8 ? st[i] : 8]++;
            char bus[64] = { 0 }, path[160]; int dn = -1;
            if (hipDeviceGetPCIBusId(bus, sizeof bus, d) == hipSuccess) { for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
                snprintf(path, sizeof path, "/sys/bus/pci/devices/%s/numa_node", bus); FILE *f = fopen(path, "r"); if (f) { if (fscanf(f, "%d", &dn) != 1) dn = -1; fclose(f); } } else (void)hipGetLastError();
            long kps = -1, pmd = -1; FILE *sm = fopen("/proc/self/smaps", "r");   /* the mapping's page size and its huge-page part (kB) */
            if (sm) { char ln[512]; int in = 0; while (fgets(ln, sizeof ln, sm)) { unsigned long lo, hi;
                if (sscanf(ln, "%lx-%lx ", &lo, &hi) == 2 && strchr(ln, '-') < strchr(ln, ' ')) { if (in) break; in = (unsigned long)x->base >= lo && (unsigned long)x->base < hi; continue; }
                if (in) { sscanf(ln, "KernelPageSize: %ld", &kps); sscanf(ln, "ShmemPmdMapped: %ld", &pmd); } } fclose(sm); }
            printf("comm_shmem: pe %d: proxy pool of APU %d (NUMA node %d): sampled pages on nodes 0..7 %d %d %d %d %d %d %d %d, unknown %d; page %ld kB, huge %ld kB\n", S.me, d, dn, cnt[0], cnt[1], cnt[2], cnt[3], cnt[4], cnt[5], cnt[6], cnt[7], cnt[8], kps, pmd);
        }
    }
#endif
    for (int d = 0; d < 4; d++) for (int h = 0; h < g_px[d]->nh; h++) __atomic_store_n(&px_hdr_of(g_px[d]->hl[h].base)->pid, (long)getpid(), __ATOMIC_RELEASE);
}
static void px_shutdown(void)
{
    for (int d = 0; d < 4; d++) { px_dev *x = g_px[d]; if (!x) continue;
        for (int hh = 0; hh < x->nh; hh++) { struct px_hdr *h = px_hdr_of(x->hl[hh].base);
            while (__atomic_load_n(&h->done, __ATOMIC_ACQUIRE) < __atomic_load_n(&h->head, __ATOMIC_ACQUIRE)) { px_progress(x); sched_yield(); } }
        px_progress(x);
        if (S.verbose || S.me == 0) printf("comm_shmem: pe %d: proxy pool of APU %d: peak %zu MiB of %zu\n", S.me, d, x->peak >> 20, x->span >> 20);
        for (int hh = 0; hh < x->nh; hh++) __atomic_store_n(&px_hdr_of(x->hl[hh].base)->quit, 1, __ATOMIC_RELEASE);
    }
}
int comm_shmem_pe_per_apu(void)
{
    const char *e = getenv("COMM_SHMEM_PE_PER_APU"); int v = e ? atoi(e) : 0;
    if (v < 0 || v > 2) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PE_PER_APU=%s: 0 (off), 1 (one helper PE per APU) or 2 (two helper PEs per APU, one per NIC; docs: N7b, N7E)\n", e);
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
    int prov = -1; g_t0 = now_s();
    S.verbose = env_int("COMM_SHMEM_VERBOSE", 0); if (!S.verbose && env_int("ECALC_VERBOSE", 1) >= 2) S.verbose = 1;
    size_t mb = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192;
#ifdef COMM_SHMEM_DEVICE_HEAP
    S.devheap = 1;
#else
    S.devheap = env_int("COMM_SHMEM_DEVHEAP", 0);
#endif
    S.ppa = comm_shmem_pe_per_apu(); g_pxinit = S.ppa && env_int("COMM_SHMEM_PX_INIT", 0) > 0; g_pxthr = env_int("COMM_SHMEM_PX_INIT_THREADS", 4); g_meshpar = S.ppa && env_int("COMM_SHMEM_MESH_PAR", 0) > 0;
    S.ppn = S.ppa ? 1 + 4 * S.ppa : 1;   /* N7b: 1 + 4 ppa PEs per node */
    if (S.ppa && comm_ofi_enabled()) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PE_PER_APU=%d needs COMM_OFI=0 (the proxies carry the data instead of comm_ofi)\n", S.ppa);
    g_thp = S.ppa && env_int("COMM_SHMEM_PX_THP", 1) > 0; g_pin = S.ppa && env_int("COMM_SHMEM_PX_PIN", 0) > 0; g_thp_thr = g_pxthr;   /* N7G: THP default 1 (D36), PIN default 0 */
    if (g_pin) pin_start();
    /* the mailbox needs the PE count, known only after init; sized for the launcher's count when it tells us, else for 4096 PEs (N7b: nodes) */
    int npes_hint = getenv("SLURM_NTASKS") ? atoi(getenv("SLURM_NTASKS")) / S.ppn : getenv("PMI_SIZE") ? atoi(getenv("PMI_SIZE")) / S.ppn : getenv("COMM_SIZE") ? atoi(getenv("COMM_SIZE")) : 4096;
    if (npes_hint < 1) npes_hint = 4096;
    S.pxsend = env_int("COMM_SHMEM_PX_SEND", 1);
    if (S.pxsend < 1 || S.pxsend > 3) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PX_SEND=%d: 1 (default, batch then quiet), 2 (chunked put-with-signal) or 3 (chunked puts, one fence + signal per command)\n", S.pxsend);
    if (S.pxsend >= 2 && S.ppa == 2) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PX_SEND=%d is not built for COMM_SHMEM_PE_PER_APU=2\n", S.pxsend);
    if (S.pxsend >= 2 && (!S.ppa || !HAVE_PUT_SIGNAL)) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PX_SEND=%d needs COMM_SHMEM_PE_PER_APU=1 and an OpenSHMEM 1.5 build (the proxy's context code is compiled only with 1.5 headers; PX_SEND=3 itself uses plain puts, not put-with-signal)\n", S.pxsend);
    { const char *e = getenv("COMM_SHMEM_PX_BLOCK_MB"); double mb = e ? atof(e) : 4;   /* the PX_SEND=2 chunk (fewer chunks and signal adds per peer); value 2's 4 MiB heap alternation keeps PXCH */
      if (!(mb >= 1 && mb <= 1024)) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PX_BLOCK_MB=%s: 1..1024 (MiB; default 4)\n", e ? e : "");
      S.pxch = (size_t)(mb * 1048576.0);
      if (S.pxch != PXCH && S.pxsend < 2) ec_fatal(EC_RC_FATAL, "comm_shmem: COMM_SHMEM_PX_BLOCK_MB needs COMM_SHMEM_PX_SEND=2 or 3 (COMM_SHMEM_PE_PER_APU=1); value 2 keeps the 4 MiB heap blocks\n"); }
    S.mb_bytes = ((size_t)MAXID * npes_hint * 8 * (S.pxsend >= 2 ? 2 : 1) + ALIGN - 1) & ~(size_t)(ALIGN - 1);   /* (N7D: a proxy's signal words in its second half) */
    S.pool_bytes = (mb << 20) + S.mb_bytes;
    { const char *ex = getenv("COMM_SHMEM_PROXY"); if (S.ppa && ex && atoi(ex)) {   /* N7b: a proxy's heap as a named file (heap_mmap) */
        g_heap_min = comm_shmem_heap_env(0); if (!g_heap_min) { fprintf(stderr, "comm_shmem: a proxy needs SHMEM_SYMMETRIC_SIZE (mnrun.sh sets it)\n"); exit(1); }
        heap_path(g_heap_path, sizeof g_heap_path, getenv("SLURM_PROCID") ? atoi(getenv("SLURM_PROCID")) : -1); unlink(g_heap_path);
        if (env_int("COMM_SHMEM_PX_NUMA", 1) > 0) {        /* D31: default 1 (proxy mode only). N7C: proxy d (local PE d, on cxi d) -> cxi d's NUMA node (that of APU d on aac7; checked in px_attach) */
            int d = (getenv("SLURM_LOCALID") ? atoi(getenv("SLURM_LOCALID")) : 0) / S.ppa; char p[96]; snprintf(p, sizeof p, "/sys/class/cxi/cxi%d/device/numa_node", d);
            FILE *f = fopen(p, "r"); g_heap_numa = d; if (f) { int n = -1; if (fscanf(f, "%d", &n) == 1 && n >= 0) g_heap_numa = n; fclose(f); } }
        g_heap_arm = 1; } }
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &prov);
    g_heap_arm = 0; g_t_shinit = now_s() - g_t0;
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
    S.layv_any = env_int("COMM_SHMEM_LAYV_ANY", 0) > 0;   /* N8 task 5: the proxy push polls every pending peer (push_any) with no inbox, so no peer waits behind a late receiver's offset word */
    S.xstats = env_int("COMM_XSTATS", 0) > 0;
    S.pts = env_int("COMM_PHASE_TS", 0) > 0; S.xsany = S.xstats || S.pts;   /* N8 ts: per-PE phase enter/exit timestamps */
    S.vslot_pool = env_int("COMM_LAYER_VSLOT_POOL", 1) > 0;   /* X2: no rounds for an exchange from and into the pool (default 1) */
    { const char *er = getenv("COMM_SHMEM_ROUND_MB"); double rm = er ? atof(er) : 0; S.round_bytes = rm > 0 ? (size_t)(rm * 1048576.0) : 0; }
    /* N7E value 2: the node-process maps the helpers' heap files page by page (px_map2): the pool must start on a page of the file (measured: shmem_malloc
     * gave offset = 1920 mod 4096 in the heap), so the pool is shmem_align'ed to 2 MiB (every PE alike: symmetric) */
    S.pool = S.ppa == 2 ? (char *)shmem_align((size_t)2 << 20, S.pool_bytes) : (char *)shmem_malloc(S.pool_bytes);
    if (!S.pool) { fprintf(stderr, "comm_shmem: pe %d: shmem_malloc of %zu MiB failed: the SHMEM heap (SHMEM_SYMMETRIC_SIZE) must be >= %zu MiB (the pool + 512)\n", S.me, S.pool_bytes >> 20, (S.pool_bytes >> 20) + 512); shmem_global_exit(1); }
    memset(S.pool, 0, S.mb_bytes);                        /* the mailbox: 0 = empty */
    if (S.ppa && !S.proxy) *mailbox(MAXID - 1, 0) = (long)getpid();   /* N7b: the node-process's pid in the reserved mailbox row, for its proxies */
    if (S.proxy) {                                        /* N7b: a proxy -- its control zeroed before the barrier; it serves until the node-process's finalize */
        memset(S.pool + S.mb_bytes, 0, PXCTL); px_hdr_of(S.pool)->pad[0] = 0x4e374250;   /* the mark the node-process checks after mapping */
        *mailbox(MAXID - 1, 1) = (long)(S.pool - (char *)g_heap_addr);              /* the pool's offset in the heap file */
        if (g_heap_numa >= 0 && (S.verbose || S.pme < S.ppn)) fprintf(stderr, "comm_shmem: proxy pe %d: COMM_SHMEM_PX_NUMA=1: heap prefers NUMA node %d (mbind %s)\n", S.pme, g_heap_numa, g_heap_mbind ? strerror(g_heap_mbind) : "ok");
        if (g_pxinit) { double tp = now_s(); pool_populate(S.pool, S.pool_bytes, g_pxthr); g_t_pop = now_s() - tp;   /* N7D: its pages faulted here, by the NUMA policy of the heap file */
            if (g_pop_err && S.verbose) fprintf(stderr, "comm_shmem: proxy pe %d: madvise(MADV_POPULATE_WRITE) failed (%s): pages touched instead\n", S.pme, strerror(g_pop_err > 0 ? g_pop_err : EINVAL)); }
        if (g_thp) { long pmd = smaps_kb(S.pool, "ShmemPmdMapped"); int ce = g_thp_err; double tc = g_thp_t;   /* N7G: done in heap_mmap */
            if (S.verbose || S.pme < S.ppn) fprintf(stderr, "comm_shmem: proxy pe %d: COMM_SHMEM_PX_THP=1: heap at %s2 MiB + %zu kB (library hint %p, flags 0x%x), pool %zu MiB; %ld MiB of the heap mapping in 2 MiB pages (heap collapse %s, %.2f s incl. pre-fault)\n",
                S.pme, ((uintptr_t)g_heap_addr & (THP_SZ - 1)) ? "" : "a multiple of ", (size_t)((uintptr_t)g_heap_addr & (THP_SZ - 1)) >> 10, g_heap_hint, (unsigned)g_heap_fl, S.pool_bytes >> 20, pmd >> 10, ce ? strerror(ce > 0 ? ce : EINVAL) : "ok", tc); }
        if (S.verbose) fprintf(stderr, "comm_shmem: proxy pe %d: init timeline (s since entry): shmem_init %.2f, pool pre-fault %.2f, barrier next at %.2f\n", S.pme, g_t_shinit, g_t_pop, now_s() - g_t0);
        shmem_barrier_all(); S.inited = 1;
        if (S.verbose) fprintf(stderr, "comm_shmem: proxy pe %d: init timeline: barrier passed at %.2f\n", S.pme, now_s() - g_t0);
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
    if (g_pxinit && !S.devheap) { double tp = now_s(); pool_populate(S.pool, S.pool_bytes, g_pxthr); g_t_pop = now_s() - tp;   /* N7D: pre-faulted here; registered after the barrier with the proxy pools */
        if (g_pop_err && S.verbose) fprintf(stderr, "comm_shmem: pe %d: madvise(MADV_POPULATE_WRITE) failed (%s): pages touched instead\n", S.me, strerror(g_pop_err > 0 ? g_pop_err : EINVAL)); }
    else if (!S.devheap) {
        double tr = now_s(); hipError_t he = hipHostRegister(S.pool, S.pool_bytes, hipHostRegisterPortable);
        S.registered = he == hipSuccess;
        if (!S.registered) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies, no symmetric slabs\n", hipGetErrorString(he)); }
        else { void *dp = 0; if (hipHostGetDevicePointer(&dp, S.pool, 0) != hipSuccess || dp != S.pool) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: the pool's device pointer differs from its host pointer: no symmetric slabs\n"); S.registered = 2; } }
        g_t_own = now_s() - tr;
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
    double t_bar = now_s();
    shmem_barrier_all();                                  /* every mailbox is zeroed before anyone posts */
    S.inited = 1;
    if (S.verbose && S.ppa) printf("comm_shmem: pe %d: init timeline: shmem_init %.2f, pool alloc+pre-fault/registration done at %.2f, barrier took %.2f s, passed at %.2f\n", S.me, g_t_shinit, t_bar - g_t0, now_s() - t_bar, now_s() - g_t0);
    if (S.ppa) px_attach();                               /* N7b: (after the barrier: the proxies' control is zeroed) */
    if (S.me == 0 && S.round_bytes) printf("comm_shmem: COMM_SHMEM_ROUND_MB=%.6g: an all-to-all above it is carried in rounds of at most that staging each way\n", S.round_bytes / 1048576.0);
    if (S.me == 0 && S.nslot) printf("comm_shmem: staging in %d regions of %.1f MiB (one per APU) + a general area of %.1f MiB\n", S.nslot, S.slot_bytes / 1048576.0, S.blocks->len / 1048576.0);
    if (S.me == 0) printf("comm_shmem: %s, %d PEs%s, thread level %d (%s), pool %zu MiB%s%s, order %s%s, spin %ld us, peer order %s\n", comm_shmem_impl(), S.npes, S.ppa ? (S.pxsend == 2 ? " (nodes; + 4 proxy PEs each, COMM_SHMEM_PE_PER_APU=1; proxy send COMM_SHMEM_PX_SEND=2: 4 MiB put-with-signal chunks, a context per peer node)" : S.pxsend == 3 ? " (nodes; + 4 proxy PEs each, COMM_SHMEM_PE_PER_APU=1; proxy send COMM_SHMEM_PX_SEND=3: plain chunks, one fence + signal per command, a context per peer node)" : " (nodes; + 4 proxy PEs each, COMM_SHMEM_PE_PER_APU=1)") : "", prov, S.serial ? "calls serialised" : "concurrent, a context per communicator", S.pool_bytes >> 20,
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
enum { W_ROFF, W_SIG, W_VAL0, W_VAL1, W_VSEQ0, W_VSEQ1, W_PROD, W_CONS, W_N, W_IBX = W_N };   /* W_IBX: FA's inbox grant, a word past W_N only under COMM_SHMEM_OFFS_WITH_DATA (nw) */
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
    int nw, ibon, ibtried; size_t ib, ibcap; unsigned char *ibf;   /* FA: control words per rank (W_N, + 1 under the switch); my inbox (offset in db), bytes per sender, which senders' messages of the pending exchange are in it */
} shm_priv;
#define PRIV(c) ((shm_priv *)(c)->priv)
/* ---- X1: COMM_XSTATS=1 (results/XEFF.md M1): per (phase, kind) sums over the completed exchanges of this PE (all APU threads) ---- */
static struct xs_row { long n, rounds; double bytes, tpost, tcall, twait, roff, sig; } g_xs[WST_NP][XK_NK];
static struct xs_apu { long n; double bytes, tpost; } g_xa[8][XK_NK];
static const char *const xs_ph[WST_NP] = { "other", "bs", "dm", "recip" };
static const char *const xs_kn[XK_NK] = { "other", "redist", "result", "spill", "addsh", "layeq", "layv" };
/* N8 ts, COMM_PHASE_TS=1 (default off): one stderr line per exchange enter and exit, phase.kind, wall clock in us (CLOCK_REALTIME: comparable across nodes) */
static void pts_line(shm_priv *p, const char *ev)
{
    static __thread char host[64]; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    if (!host[0]) { gethostname(host, sizeof host - 1); char *d = strchr(host, '.'); if (d) *d = 0; if (!host[0]) strcpy(host, "?"); }
    fprintf(stderr, "phase-ts pe %d host %s %s.%s %s %lld apu %d\n", S.me, host, xs_ph[p->xph], xs_kn[p->xk], ev, (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000, p->xapu);
}
static void xs_begin(shm_priv *p)
{
    p->xk = comm_xtag; if (p->xk < 0 || p->xk >= XK_NK) p->xk = 0;
    p->xph = comm_wst_get_phase(); p->xroff = p->xsig = 0; p->xK = 1; p->xapu = 0;
#ifndef COMM_HOST_ONLY
    { int d = 0; if (hipGetDevice(&d) == hipSuccess && d >= 0 && d < 8) p->xapu = d; }
#endif
    if (S.pts) pts_line(p, "enter");
}
/* at the completion: tw0 = when the caller entered s_wait, bytes = received, t = post to completion */
static void xs_end(shm_priv *p, double tw0, double bytes, double t)
{
    double now = now_s();
    if (S.pts) pts_line(p, "exit");
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
static char *RING(shm_priv *p, size_t base, int r) { return S.pool + base + (size_t)p->nw * p->n * 8 + (size_t)r * p->ring; }
#define PACK(seq, off) (((long)(seq) << 40) | (long)(off))   /* a byte offset / count below 2^40 tagged with the sequence (< 2^23) */
#define UNSEQ(x) ((x) >> 40)
#define UNOFF(x) ((size_t)((x) & (((long)1 << 40) - 1)))
/* the data base of the current exchange -- on: a device op of an OFI communicator (its data over libfabric) */
static void set_xo(shm_priv *p, int on) { p->xo = on && p->ofi; p->db = p->xo ? (p->px ? p->px->base : comm_ofi_pool(p->od)) : S.pool; }
static int in_db(shm_priv *p, const void *x) { return p->xo ? (p->px ? px_in(p->px, x) : comm_ofi_in_pool(p->od, x)) : in_pool(x); }
static size_t off_db(shm_priv *p, const void *x) { return (size_t)((const char *)x - p->db); }
/* ---- FA COMM_SHMEM_OFFS_WITH_DATA=<MiB> (default 0 = off; docs/N7E_T2.md fix 2): the proxy exchange without the late offset word ----
 * Today a sender posts to a receiver only after the receiver's W_ROFF word for this exchange (where its slab lands) arrives, and walks its
 * peers in order: one late receiver holds the transfer to it until it enters the exchange AND the transfers to every later peer (roff_wait
 * x5-6 in the slow runs).  Under the switch every member of a proxy communicator keeps an INBOX in its proxy pool (<MiB> per communicator,
 * a slot of MiB / (n - 1) per sender, allocated at the first device exchange, from the pool's top) and GRANTS each sender its slot for the next exchange in the
 * word W_IBX of the sender's block = PACK(seq, slot offset) -- at once at the first exchange, then when an exchange's s_wait has copied the
 * inbox out, so long before the receiver's next publish.  The sender (push_any) polls every pending peer, and posts to whichever is ready:
 * the receiver's roff when it is there (no copy: an on-time receiver costs nothing), else its granted slot when the message fits; the
 * signal value carries the choice (IBF in the count field), so the receiver learns with the data where it is, and s_wait copies the inbox
 * slot to the caller's buffer before the next grant.  Not in the rounds path (COMM_SHMEM_ROUND_MB): a slot message there stops the run.
 * The inbox is outside the pool rule (an inbox that does not fit is left off for that communicator, a line on stderr: its senders use roff) */
#define IBF ((long)1 << 39)                                   /* in the count field of a signal: the data is in my inbox slot */
static size_t offs_mb(void) { static long v = -1; if (v < 0) { const char *e = getenv("COMM_SHMEM_OFFS_WITH_DATA"); v = e && atol(e) > 0 ? atol(e) : 0; } return (size_t)v; }
static size_t ibx_slot(const shm_priv *p, int r) { return p->ib + (size_t)(r < p->me ? r : r - 1) * p->ibcap; }   /* sender r's slot in my inbox */
static void ibx_grant(shm_priv *p, long d)            /* every sender may write its message of the exchange iseq + d into its slot */
{
    SHM_LOCK();
    for (int r = 0; r < p->n; r++) if (r != p->me) shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], W_IBX, p->me), PACK(p->iseq[r] + d, ibx_slot(p, r)), p->pe[r]);
    SHM_UNLOCK();
}
static void ibx_setup(shm_priv *p)                    /* at a device exchange, after seq_next and before publish: the inbox once, the first grant */
{
    if (p->ibtried || !p->xo || !p->px || p->n < 2 || !offs_mb()) return;
    p->ibtried = 1; p->ibcap = ((offs_mb() << 20) / (size_t)(p->n - 1)) & ~(size_t)(ALIGN - 1);
    p->ibf = (unsigned char *)calloc(p->n, 1);
    size_t off = p->ibcap ? px_alloc_q(p->px, p->ibcap * (size_t)(p->n - 1), 1) : (size_t)-1;
    if (off == (size_t)-1) { fprintf(stderr, "comm_shmem: pe %d: COMM_SHMEM_OFFS_WITH_DATA: no room for a %zu MiB inbox in the proxy pool of APU %d (comm %d): its senders use the offset word\n", S.me, offs_mb(), p->px->d, p->id); return; }
    p->ib = off; p->ibon = 1; ibx_grant(p, 0);
}

static void put_word(shm_priv *p, int r, int w, int idx, long v)   /* word (w, idx) of rank r's block = v (ordered after the context's earlier puts by the caller's fence) */
{
    shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], w, idx), v, p->pe[r]);
}
static void putmem(shm_priv *p, int r, size_t off, const void *src, size_t n) { if (n) shmem_ctx_putmem_nbi(p->ctx, S.pool + off, src, n, p->pe[r]); }
/* the data of one peer and its signal, ordered: put-with-signal, or put + fence + signal; under `quiet` the signals are sent after one quiet (push_peers) */
static void put_signalled(shm_priv *p, int r, size_t off, const void *src, size_t n, long sig)
{
    long *sw = W(p, p->rbase[r], W_SIG, p->me);
    if (p->xo && p->px && S.pxsend >= 2) { px_write(p->px, p->pxpe[r], off, src, n, &p->ocnt[r], (long)p->id * S.npes + p->me, sig); return; }   /* N7D: the signal travels with the data */
    if (p->xo) { if (p->px) { if (n) px_write(p->px, p->pxpe[r], off, src, n, &p->ocnt[r], 0, 0); } else comm_ofi_write(p->op, r, off, src, n, &p->ocnt[r]); p->osig[r] = sig; return; }   /* the signal after delivery (ofi_flush) */
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
    if (p->px && S.pxsend >= 2) {                         /* N7D: the signals went with the data; wait until the proxy is done with my sources */
        for (unsigned spins = 0;; spins++) {
            int left = 0; for (int r = 0; r < p->n && !left; r++) left = __atomic_load_n(&p->ocnt[r], __ATOMIC_ACQUIRE) != 0;
            if (!left) return;
            px_progress(p->px); if (spins > 1024) sched_yield();
        }
    }
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
/* N7D COMM_SHMEM_PX_SEND=2: the receiver's half -- sender r's message `seq` (want bytes) is in my proxy's pool: its last chunk set word 1,
 * its other chunks added their bytes to word 0 (zeroed here for the next message, which r sends only after my next publish) */
static void px_arrive(shm_priv *p, int r, long seq, size_t want, long round)
{
    long idx = (long)p->id * S.npes + r, *b = px_sigw(p->px->base, 1, idx), *a = px_sigw(p->px->base, 0, idx);
    double t0 = comm_wst_t0(), xt = S.xstats ? now_s() : 0;
    px_spin_ge(b, PACK(seq, 0));
    long x = __atomic_load_n(b, __ATOMIC_ACQUIRE);
    if (UNSEQ(x) != seq) die("signal sequence mismatch (COMM_SHMEM_PX_SEND=2)");
    if (p->ibf) { if ((x & IBF) && (!p->ibon || p->rounds)) die("an inbox message outside an ordinary exchange (COMM_SHMEM_OFFS_WITH_DATA)"); p->ibf[r] = (x & IBF) != 0; x &= ~IBF; }   /* FA */
    if (UNOFF(x) != want) ec_fatal(EC_RC_FATAL, "comm_shmem: alltoallv count mismatch (round %ld): rank %d sends %zu bytes to rank %d, which expects %zu\n", round, r, UNOFF(x), p->me, want);
    long rest = (long)(want - px_last_len(want));
    if (rest) { px_spin_ge(a, rest); long v = __atomic_load_n(a, __ATOMIC_ACQUIRE);
        if (v != rest) ec_fatal(EC_RC_FATAL, "comm_shmem: pe %d: COMM_SHMEM_PX_SEND=2: %ld chunk bytes from rank %d, expected %ld\n", S.me, v, r, rest);
        __atomic_store_n(a, 0, __ATOMIC_SEQ_CST); }
    comm_wst_ready_end(t0); if (S.xstats) p->xsig += now_s() - xt;
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
/* FA (COMM_SHMEM_OFFS_WITH_DATA): the sender's half of a proxy exchange -- every pending peer polled, each posted as soon as its roff
 * (preferred: no copy) or, for a message that fits, its inbox grant of this exchange is there; no peer waits behind a late one */
static void push_any(shm_priv *p)
{
    int n = p->n, left = n - 1; unsigned spins = 0;
    unsigned char *dn = (unsigned char *)calloc(n, 1); dn[p->me] = 1;
    double tw = S.xstats ? now_s() : 0;
    while (left) {
        int any = 0;
        PEERS(p, k, r, 0, 1) if (!dn[r]) {
            long seq = p->oseq[r], *w = W(p, p->base, W_ROFF, r), *g = W(p, p->base, W_IBX, r), x, f = 0;
            size_t nb = p->scnt ? p->scnt[r] : p->bytes; const char *s = p->scnt ? p->src + p->sdsp[r] : p->src + p->stride * (size_t)r;
            if (test_ge(w, PACK(seq, 0))) { x = *(volatile long *)w; if (UNSEQ(x) != seq) die("exchange sequence mismatch"); }
            else if (p->ibcap && nb <= p->ibcap && test_ge(g, PACK(seq, 0)) && UNSEQ(x = *(volatile long *)g) == seq) f = IBF;
            else continue;
            SHM_LOCK(); put_signalled(p, r, UNOFF(x), s, nb, PACK(seq, nb) | f); SHM_UNLOCK();
            dn[r] = 1; left--; any = 1;
        }
        if (any) { spins = 0; if (S.xstats) tw = now_s(); }
        else if (++spins > 32) { sched_yield(); if (S.xstats) { double t = now_s(); p->xroff += t - tw; tw = t; } }
    }
    free(dn);
    ofi_flush(p);
}
static void *pusher(void *a) { comm *c = (comm *)a; shm_priv *p = PRIV(c); if ((p->ibcap || S.layv_any) && p->xo && p->px) push_any(p); else push_peers(p, p->next, -1); return 0; }
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
        if (p->xo && p->px && S.pxsend >= 2) { px_arrive(p, r, p->iseq[r], rcnt ? rcnt[r] : bytes, 0); continue; }
        long *sw = W(p, p->base, W_SIG, r); XWAIT(p, xsig, sw, PACK(p->iseq[r], 0));
        long x = *(volatile long *)sw, want = (long)(rcnt ? rcnt[r] : bytes);
        if (UNSEQ(x) != p->iseq[r]) die("signal sequence mismatch");
        if (p->ibf) { if ((x & IBF) && !p->ibon) die("an inbox message without an inbox (COMM_SHMEM_OFFS_WITH_DATA)"); p->ibf[r] = (x & IBF) != 0; x &= ~IBF; }   /* FA */
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
    p->t0 = now_s(); if (S.xsany) xs_begin(p);
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
    ibx_setup(p);                                         /* FA (off: returns) */
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
            if (p->xo && p->px && S.pxsend >= 2) { px_arrive(p, r, p->iseq[r] + 1 + j, rcnt[r] > o ? (rcnt[r] - o < c ? rcnt[r] - o : c) : 0, j); continue; }
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
    p->t0 = now_s(); if (S.xsany) xs_begin(p);
    if (!(sin && rin && S.vslot_pool) && alltoallv_rounds(c, sb, scnt, sdsp, rb, rcnt, rdsp, s, sin, rin)) return;   /* X2 COMM_LAYER_VSLOT_POOL: rounds only bound staging; an exchange from and into the pool stages nothing */
    size_t ts = comm_prefix(scnt, p->spre, n), tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, sin ? 0 : ts + 8, rin ? 0 : tr + 8, "alltoallv");
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    seq_next(p); p->v = 1; p->bytes = 0; p->rb = rb; p->st = s; p->rcnt = rcnt; p->rdsp = rdsp; p->scnt = scnt; p->rin = rin; p->pending = 1;
    p->src = sin ? (const char *)sb : p->db + p->sst; p->sdsp = sin ? sdsp : p->spre;
    if (!sin) for (int r = 0; r < n; r++) if (r != me && scnt[r]) COPY(p->db + p->sst + p->spre[r], (const char *)sb + sdsp[r], scnt[r], s);
    if (scnt[me] && (const char *)sb + sdsp[me] != (char *)rb + rdsp[me]) COPY((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me], s);
    SYNC(s);
    ibx_setup(p);                                         /* FA (off: returns) */
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
    double xtw0 = S.xsany ? now_s() : 0;                 /* X1 */
    if (p->thread) { pthread_join(p->th, 0); p->thread = 0; }
    if (p->rounds) {                                      /* the rounds completed in the helper thread */
        p->rounds = 0; staging_drop(p); p->pending = 0;
        double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock);
        if (S.xsany) xs_end(p, xtw0, recv_bytes(p), t);
        return;
    }
    arrive(p, p->bytes, p->v ? p->rcnt : 0);
    int ibn = 0;                                          /* FA: the messages that came into my inbox, to the caller's buffer */
    if (p->ibon) for (int r = 0; r < n; r++) if (r != me && p->ibf[r]) {
        size_t nb = p->v ? p->rcnt[r] : p->bytes;
        if (nb) { COPY((char *)p->rb + (p->v ? p->rdsp[r] : (size_t)r * p->bytes), p->db + ibx_slot(p, r), nb, p->st); }
        ibn++;
    }
    if (ibn && p->rin) { SYNC(p->st); }
    if (!p->rin) {
        if (!p->v) { for (int r = 0; r < n; r++) if (r != me && !(ibn && p->ibf[r])) COPY((char *)p->rb + (size_t)r * p->bytes, p->db + p->rst + (size_t)r * p->bytes, p->bytes, p->st); }
        else for (int r = 0; r < n; r++) if (r != me && p->rcnt[r] && !(ibn && p->ibf[r])) COPY((char *)p->rb + p->rdsp[r], p->db + p->rst + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    if (p->ibon) ibx_grant(p, 1);                        /* FA: the inbox is free for the next exchange */
    staging_drop(p);
    p->pending = 0;
    { double t = now_s() - p->t0; pthread_mutex_lock(&S.alloc_lock); S.tv += t; S.nv++; S.bv += recv_bytes(p); pthread_mutex_unlock(&S.alloc_lock); if (S.xsany) xs_end(p, xtw0, recv_bytes(p), t); }
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
    if (p->ibon) { px_free(p->px, p->ib); }
    free(p->ibf);                                         /* FA */
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
    p->nw = W_N + (offs_mb() ? 1 : 0);                    /* FA: the grant word (every PE alike: the switch is on the launch line) */
    p->ctrl_bytes = (size_t)p->nw * n * 8 + (size_t)n * p->ring;
    p->base = pool_alloc(p->ctrl_bytes, K_CTRL); memset(S.pool + p->base, 0, (size_t)p->nw * n * 8);
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
