/* comm_shmem.c - the SHMEM transport (Phase 11 M8-s, PLAN.md 25-26): the comm_ops of comm.h over the OpenSHMEM 1.4
 * subset that Cray OpenSHMEMX, rocSHMEM and OpenMPI's OSHMEM share.  One process per node = one PE; a communicator is
 * a strided PE set (start, stride, size) -- the tree's node groups -- with rank r = PE start + stride r.
 *
 * Memory.  One symmetric pool (shmem_malloc at init, COMM_SHMEM_POOL_MB; host memory registered with hipHostRegister on
 * OSHMEM, device memory with COMM_SHMEM_DEVHEAP=1 / -DCOMM_SHMEM_DEVICE_HEAP where the implementation's heap is device
 * memory) holds everything remote PEs write: a fixed mailbox table at its start, then a per-PE first-fit allocator for
 * the communicators' control blocks and staging.  Offsets in the pool are what the PEs exchange: a put targets
 * pool + offset, and the offset is one the RECEIVER chose (its staging, its control block), so nothing has to be
 * allocated collectively after init -- creation is the only handshake (each member posts its control block's offset
 * into every member's mailbox[id][me]; ids come from the caller, unique per run).
 *
 * The push model (as comm_xgmi): an exchange has a sequence number; the receiver publishes, per sender, where the
 * sender's slab lands (roff[sender] = seq | offset in the sender's control block); the sender waits for that word,
 * shmem_putmem_nbi's its slab from the send staging on the communicator's context, shmem_fence, then puts the count
 * and the signal word (sig[sender] = seq) into the receiver's block; the receiver's wait() polls the signals, checks
 * the counts, and copies the received slabs from its staging to the caller's buffer (H2D on the caller's stream).  The
 * publication and the puts run in a helper thread so alltoall() returns after staging, as the slab pipeline (M7) needs;
 * the device buffers are staged through the pool (D2H / H2D), which on the target with a device heap and the callers'
 * slabs living in the pool would go away.  all-gather, the unequal all-to-all (counts known on both sides from the
 * descriptors: the receiver's prefix offsets are what it publishes), barrier / max / sum-mod-q (an all-to-all of 8-byte
 * values through the val/vseq words, double-buffered by the sequence's parity) and point-to-point (a ring per (dest,
 * source) with producer / consumer counters, COMM_SHMEM_RING_KB) are the same words and puts.
 *
 * Threads.  The four APU threads drive four communicators at once; every SHMEM call goes through SHM_LOCK: a no-op when
 * the library provides SHMEM_THREAD_MULTIPLE and COMM_SHMEM_SERIAL=0 (the target's Cray SHMEM), else one process-wide
 * mutex with the waits as polled shmem_long_test under short holds (OSHMEM 4.1 claims MULTIPLE but its UCX progress
 * crashes under concurrent wait_until: the default is serial).  Collectives of the library (shmem_barrier_all,
 * reductions, teams) are used nowhere except init/finalize: they are process-level and would need all four threads to
 * agree, and OSHMEM has no teams; the strided PE set is the shim for shmem_team_split_strided.
 * Build: -DCOMM_SHMEM with the SHMEM headers (Makefile SHMEM=1, oshcc's flags); without it the entry points abort. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include "comm.h"
#ifndef COMM_SHMEM
static void no_shmem(void) { fprintf(stderr, "comm_shmem: built without SHMEM (make SHMEM=1 with oshcc / the target's SHMEM)\n"); exit(1); }
int   comm_shmem_init(void) { no_shmem(); return 0; }
int   comm_shmem_rank(void) { return 0; }
int   comm_shmem_size(void) { return 1; }
void  comm_shmem_finalize(void) { }
comm *comm_shmem_create_at(int pe_start, int pe_stride, int n, int id) { (void)pe_start; (void)pe_stride; (void)n; (void)id; no_shmem(); return 0; }
int   comm_shmem_available(void) { return 0; }
#else
#include <shmem.h>
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

/* ---- the process state ---- */
static struct {
    int inited, me, npes, serial, devheap;
    char *pool; size_t pool_bytes, mb_bytes;   /* the symmetric pool; the mailbox at [0, mb_bytes) */
    pthread_mutex_t lock;                  /* SHM_LOCK: the library in serial mode; the allocator always (alloc_lock) */
    pthread_mutex_t alloc_lock;
    struct blk { size_t off, len; int used; struct blk *next; } *blocks;
    int registered;
} S;
#define SHM_LOCK()   do { if (S.serial) pthread_mutex_lock(&S.lock); } while (0)
#define SHM_UNLOCK() do { if (S.serial) pthread_mutex_unlock(&S.lock); } while (0)
static void die(const char *m) { fprintf(stderr, "comm_shmem: pe %d: %s\n", S.me, m); exit(1); }

/* the pool's first-fit allocator (offsets; a block list sorted by offset, coalesced on free) */
static size_t pool_alloc(size_t len)
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&S.alloc_lock);
    for (struct blk *b = S.blocks; b; b = b->next) if (!b->used && b->len >= len) {
        if (b->len > len) { struct blk *nb = (struct blk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
        b->used = 1; pthread_mutex_unlock(&S.alloc_lock); return b->off;
    }
    pthread_mutex_unlock(&S.alloc_lock);
    fprintf(stderr, "comm_shmem: pe %d: the symmetric pool (%zu MiB, COMM_SHMEM_POOL_MB) cannot hold %zu MiB more\n", S.me, S.pool_bytes >> 20, len >> 20); exit(1);
}
static void pool_free(size_t off)
{
    pthread_mutex_lock(&S.alloc_lock);
    struct blk *p = 0;
    for (struct blk *b = S.blocks; b; p = b, b = b->next) if (b->off == off) {
        b->used = 0;
        if (b->next && !b->next->used) { struct blk *n = b->next; b->len += n->len; b->next = n->next; free(n); }
        if (p && !p->used) { p->len += b->len; p->next = b->next; free(b); }
        break;
    }
    pthread_mutex_unlock(&S.alloc_lock);
}
static long *mailbox(int id, int pe) { return (long *)S.pool + (size_t)id * S.npes + pe; }

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

int comm_shmem_available(void) { return 1; }
int comm_shmem_rank(void) { return S.me; }
int comm_shmem_size(void) { return S.npes; }
int comm_shmem_init(void)
{
    if (S.inited) return S.npes;
    int prov = -1;
    shmem_init_thread(SHMEM_THREAD_MULTIPLE, &prov);
    S.me = shmem_my_pe(); S.npes = shmem_n_pes();
    pthread_mutex_init(&S.lock, 0); pthread_mutex_init(&S.alloc_lock, 0);
    const char *e = getenv("COMM_SHMEM_SERIAL");
    S.serial = e ? atoi(e) != 0 : 1;                      /* default serial: OSHMEM 4.1's MULTIPLE is nominal (see the header) */
    if (prov < SHMEM_THREAD_MULTIPLE && !S.serial) { if (S.me == 0) fprintf(stderr, "comm_shmem: the library provides thread level %d, not MULTIPLE: serialising the calls\n", prov); S.serial = 1; }
    size_t mb = getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192;
    S.mb_bytes = ((size_t)MAXID * S.npes * 8 + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    S.pool_bytes = (mb << 20) + S.mb_bytes;
#ifdef COMM_SHMEM_DEVICE_HEAP
    S.devheap = 1;
#else
    S.devheap = getenv("COMM_SHMEM_DEVHEAP") ? atoi(getenv("COMM_SHMEM_DEVHEAP")) : 0;
#endif
    S.pool = (char *)shmem_malloc(S.pool_bytes);
    if (!S.pool) { fprintf(stderr, "comm_shmem: pe %d: shmem_malloc of %zu MiB failed (SHMEM_SYMMETRIC_HEAP_SIZE?)\n", S.me, S.pool_bytes >> 20); shmem_global_exit(1); }
    memset(S.pool, 0, S.mb_bytes);                        /* the mailbox: 0 = empty */
#ifndef COMM_HOST_ONLY
    if (!S.devheap) {
        hipError_t he = hipHostRegister(S.pool, S.pool_bytes, hipHostRegisterPortable);
        S.registered = he == hipSuccess;
        if (!S.registered) { (void)hipGetLastError(); if (S.me == 0) fprintf(stderr, "comm_shmem: hipHostRegister of the pool failed (%s): pageable copies\n", hipGetErrorString(he)); }
    }
#endif
    S.blocks = (struct blk *)malloc(sizeof *S.blocks); S.blocks->off = S.mb_bytes; S.blocks->len = S.pool_bytes - S.mb_bytes; S.blocks->used = 0; S.blocks->next = 0;
    shmem_barrier_all();                                  /* every mailbox is zeroed before anyone posts */
    S.inited = 1;
    if (S.me == 0) printf("comm_shmem: %d PEs, thread level %d (%s), pool %zu MiB%s%s\n", S.npes, prov, S.serial ? "calls serialised" : "concurrent", S.pool_bytes >> 20, S.devheap ? " (device heap)" : "", S.registered ? ", HIP-registered" : "");
    return S.npes;
}
void comm_shmem_finalize(void)
{
    if (!S.inited) return;
    shmem_barrier_all();
#ifndef COMM_HOST_ONLY
    if (S.registered) HIP_CHECK(hipHostUnregister(S.pool));
#endif
    shmem_free(S.pool);
    shmem_finalize(); S.inited = 0;
}

/* ---- a communicator ---- */
/* the control block of a member (offsets from its base): the words other members write */
enum { W_ROFF, W_SIG, W_CNT, W_VAL0, W_VAL1, W_VSEQ0, W_VSEQ1, W_PROD, W_CONS, W_N };
typedef struct {
    int n, me, id, *pe;                    /* pe[r]: the PE of rank r */
    shmem_ctx_t ctx; int own_ctx;
    size_t base, *rbase, ring, ctrl_bytes; /* my block, the members' blocks, the ring bytes per source */
    long seq, tseq;                        /* exchanges; tiny exchanges */
    size_t sst, sst_cap, rst, rst_cap;     /* staging (send, receive): pool offsets, 0 = none */
    /* the pending exchange */
    int pending, v, thread; pthread_t th; void *rb; hipStream_t st; size_t bytes; const size_t *rcnt, *rdsp; size_t *rpre, *spre;
    const void *sb; const size_t *scnt, *sdsp; int host;   /* the helper thread's inputs (host: the source is the caller's host buffer) */
    long *sent, *got;                      /* point-to-point: bytes sent to / received from each rank */
} shm_priv;
#define PRIV(c) ((shm_priv *)(c)->priv)
static long *W(shm_priv *p, size_t base, int w, int r) { return (long *)(S.pool + base + ((size_t)w * p->n + r) * 8); }   /* word w of rank r in the block at base */
static char *RING(shm_priv *p, size_t base, int r) { return S.pool + base + (size_t)W_N * p->n * 8 + (size_t)r * p->ring; }
#define PACK(seq, off) (((long)(seq) << 40) | (long)((off) >> 3))
#define UNSEQ(x) ((x) >> 40)
#define UNOFF(x) ((size_t)((x) & (((long)1 << 40) - 1)) << 3)

static void put_word(shm_priv *p, int r, int w, int idx, long v)   /* word (w, idx) of rank r's block = v (ordered after the context's earlier puts by the caller's fence) */
{
    shmem_ctx_long_p(p->ctx, W(p, p->rbase[r], w, idx), v, p->pe[r]);
}
static void putmem(shm_priv *p, int r, size_t off, const void *src, size_t n) { if (n) shmem_ctx_putmem_nbi(p->ctx, S.pool + off, src, n, p->pe[r]); }

static void staging(shm_priv *p, size_t send, size_t recv)
{
    if (send > p->sst_cap) { if (p->sst) pool_free(p->sst); p->sst = pool_alloc(send); p->sst_cap = send; }
    if (recv > p->rst_cap) { if (p->rst) pool_free(p->rst); p->rst = pool_alloc(recv); p->rst_cap = recv; }
}
/* the sender's half of an exchange: wait for each receiver's offset, put, fence, count + signal.  Equal slabs: src +
 * r * stride (stride 0: one block to all); unequal: src + sdsp[r], scnt[r] */
static void push_all(shm_priv *p, const char *src, size_t stride, size_t bytes, const size_t *scnt, const size_t *sdsp)
{
    long seq = p->seq;
    for (int r = 0; r < p->n; r++) if (r != p->me) {
        long *w = W(p, p->base, W_ROFF, r); wait_ge(w, PACK(seq, 0));
        long x = *(volatile long *)w; if (UNSEQ(x) != seq) die("exchange sequence mismatch");
        size_t n = scnt ? scnt[r] : bytes; const char *s = scnt ? src + sdsp[r] : src + stride * (size_t)r;
        SHM_LOCK(); putmem(p, r, UNOFF(x), s, n); SHM_UNLOCK();
    }
    SHM_LOCK(); shmem_ctx_fence(p->ctx);
    for (int r = 0; r < p->n; r++) if (r != p->me) { put_word(p, r, W_CNT, p->me, (long)(scnt ? scnt[r] : bytes)); put_word(p, r, W_SIG, p->me, seq); }
    SHM_UNLOCK();
}
static void *pusher(void *a)
{
    comm *c = (comm *)a; shm_priv *p = PRIV(c);
    if (p->v) push_all(p, p->host ? (const char *)p->sb : S.pool + p->sst, 0, 0, p->scnt, p->host ? p->sdsp : p->spre);
    else push_all(p, S.pool + p->sst, p->bytes, p->bytes, 0, 0);
    return 0;
}
/* the receiver's half: publish where every sender's slab lands (rst + r * bytes, or the prefix offsets) */
static void publish(shm_priv *p, size_t bytes, const size_t *rpre)
{
    SHM_LOCK();
    for (int r = 0; r < p->n; r++) if (r != p->me) put_word(p, r, W_ROFF, p->me, PACK(p->seq, p->rst + (rpre ? rpre[r] : bytes * (size_t)r)));
    SHM_UNLOCK();
}
/* wait for every sender's signal (and count), then the context's completion (my send staging is free again) */
static void arrive(shm_priv *p, size_t bytes, const size_t *rcnt)
{
    for (int r = 0; r < p->n; r++) if (r != p->me) {
        wait_ge(W(p, p->base, W_SIG, r), p->seq);
        long cnt = *(volatile long *)W(p, p->base, W_CNT, r), want = (long)(rcnt ? rcnt[r] : bytes);
        if (cnt != want) { fprintf(stderr, "comm_shmem: alltoallv count mismatch: rank %d sends %ld bytes to rank %d, which expects %ld\n", r, cnt, p->me, want); exit(1); }
    }
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); SHM_UNLOCK();
}
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { return c->size; }
static void start_thread(comm *c)
{
    shm_priv *p = PRIV(c);
    if (pthread_create(&p->th, 0, pusher, c)) die("pthread_create"); p->thread = 1;
}
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoall while one is pending");
    staging(p, bytes * n, bytes * n);
    p->seq++; p->v = 0; p->host = 0; p->bytes = bytes; p->rb = rb; p->st = s; p->pending = 1;
    publish(p, bytes, 0);
    COPY(S.pool + p->sst, sb, bytes * n, s);                                                    /* my slabs (all of them: simpler than skipping the self slab) */
    COPY((char *)rb + (size_t)me * bytes, (const char *)sb + (size_t)me * bytes, bytes, s);   /* the self slab */
    SYNC(s);
    start_thread(c);
}
static void s_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("alltoallv while an exchange is pending");
    size_t ts = comm_prefix(scnt, p->spre, n), tr = comm_prefix(rcnt, p->rpre, n);
    staging(p, ts + 8, tr + 8);
    if (scnt[me] != rcnt[me]) die("alltoallv self count mismatch");
    p->seq++; p->v = 1; p->host = 0; p->rb = rb; p->st = s; p->rcnt = rcnt; p->rdsp = rdsp; p->scnt = scnt; p->pending = 1;
    publish(p, 0, p->rpre);
    for (int r = 0; r < n; r++) if (r != me && scnt[r]) COPY(S.pool + p->sst + p->spre[r], (const char *)sb + sdsp[r], scnt[r], s);
    if (scnt[me] && (const char *)sb + sdsp[me] != (char *)rb + rdsp[me]) COPY((char *)rb + rdsp[me], (const char *)sb + sdsp[me], scnt[me], s);
    SYNC(s);
    start_thread(c);
}
static void s_wait(comm *c)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (!p->pending) return;
    if (p->thread) { pthread_join(p->th, 0); p->thread = 0; }
    arrive(p, p->bytes, p->v ? p->rcnt : 0);
    if (!p->v) { for (int r = 0; r < n; r++) if (r != me) COPY((char *)p->rb + (size_t)r * p->bytes, S.pool + p->rst + (size_t)r * p->bytes, p->bytes, p->st); }
    else for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], S.pool + p->rst + p->rpre[r], p->rcnt[r], p->st);
    SYNC(p->st);
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
    p->seq++;
    publish(p, 0, p->rpre);
    push_all(p, (const char *)sb, 0, 0, scnt, sdsp);
    arrive(p, 0, rcnt);
    for (int r = 0; r < n; r++) if (r != me && rcnt[r]) memcpy((char *)rb + rdsp[r], S.pool + p->rst + p->rpre[r], rcnt[r]);
}
static void s_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("allgather while an exchange is pending");
    staging(p, 0, bytes * n);
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) memcpy(self, sb, bytes);
    p->seq++;
    publish(p, bytes, 0);
    push_all(p, (const char *)sb, 0, bytes, 0, 0);
    arrive(p, bytes, 0);
    for (int r = 0; r < n; r++) if (r != me) memcpy((char *)rb + (size_t)r * bytes, S.pool + p->rst + (size_t)r * bytes, bytes);
}
static void s_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    shm_priv *p = PRIV(c); int n = p->n, me = p->me;
    if (p->pending) die("allgather while an exchange is pending");
    staging(p, bytes, bytes * n);
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) COPY(self, sb, bytes, 0);
    COPY(S.pool + p->sst, sb, bytes, 0); SYNC(0);
    p->seq++;
    publish(p, bytes, 0);
    push_all(p, S.pool + p->sst, 0, bytes, 0, 0);
    arrive(p, bytes, 0);
    for (int r = 0; r < n; r++) if (r != me) COPY((char *)rb + (size_t)r * bytes, S.pool + p->rst + (size_t)r * bytes, bytes, 0);
    SYNC(0);
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
    shmem_ctx_fence(p->ctx);
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
        shmem_ctx_fence(p->ctx);
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
static void s_destroy(comm *c)
{
    shm_priv *p = PRIV(c);
    if (p->pending) s_wait(c);
    s_barrier(c);                                          /* nobody's block goes while a member may still write to it */
    SHM_LOCK(); shmem_ctx_quiet(p->ctx); if (p->own_ctx) shmem_ctx_destroy(p->ctx); SHM_UNLOCK();
    if (p->sst) pool_free(p->sst); if (p->rst) pool_free(p->rst); pool_free(p->base);
    free(p->pe); free(p->rbase); free(p->rpre); free(p->spre); free(p->sent); free(p->got); free(p); free(c);
}
static const struct comm_ops shm_ops = { s_rank, s_size, s_alltoall, s_wait, s_barrier, s_modq, s_max, s_destroy, s_send, s_recv, s_allgather, s_allgather_host, s_alltoallv, s_alltoallv_host };

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
    p->base = pool_alloc(p->ctrl_bytes); memset(S.pool + p->base, 0, (size_t)W_N * n * 8);
    p->rbase = (size_t *)calloc(n, sizeof(size_t)); p->rpre = (size_t *)calloc(n, sizeof(size_t)); p->spre = (size_t *)calloc(n, sizeof(size_t));
    p->sent = (long *)calloc(n, sizeof(long)); p->got = (long *)calloc(n, sizeof(long));
    SHM_LOCK();
    p->own_ctx = shmem_ctx_create(0, &p->ctx) == 0;
    if (!p->own_ctx) p->ctx = SHMEM_CTX_DEFAULT;
    if (*mailbox(id, me) != 0) { SHM_UNLOCK(); fprintf(stderr, "comm_shmem: communicator id %d used twice\n", id); exit(1); }
    for (int r = 0; r < n; r++) shmem_ctx_long_p(p->ctx, mailbox(id, me), (long)p->base + 1, p->pe[r]);   /* my block's offset into every member's row (mine included) */
    shmem_ctx_quiet(p->ctx);
    SHM_UNLOCK();
    for (int r = 0; r < n; r++) { long *m = mailbox(id, p->pe[r]); wait_ne(m, 0); p->rbase[r] = (size_t)(*(volatile long *)m - 1); }
    return c;
}
#endif
