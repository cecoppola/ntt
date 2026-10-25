/* comm_xshmem.c -- the exchanges of ecalc on xshmem.h (plan TARGET1 stage S3a; design TARGET12_DESIGN rev1 sections 3, 5, 6).
 *
 * COMM_TRANSPORT=xshmem (make XSHMEM=ofi or XSHMEM=cray; compiled with -DXSHMEM_ENGINE).  The only multi-node transport.  This file is the target-shaped
 * engine: it speaks ONLY xshmem.h (no fi_*, no shmem_*), so on the target the same file runs on Cray SHMEM 12.1 behind xshmem_cray.h.
 * It is the exchange prototype of tests/t_xshmem.c (xex_post / xex_progress / xex_rx / xex_wait) moved behind struct comm_ops.
 *
 * Model.  One node-process hosts K loopback PEs (xshmem_ofi_loopback_pes(); K = 1 on a backend without loopback): PE = K * node + d, the
 * PE d owns GPU d, its NIC, its DATA heap (payload, sub-allocated here) and its CTRL heap (the control words).  A mesh communicator
 * (mn.c: node set {pe_start + pe_stride r}, one per APU thread d) is the PE set {K (pe_start + pe_stride r) + d}: each NIC carries an
 * independent one-PE-per-node all-to-all.  comm_create_at takes NODE indices.
 *
 * Protocol (one writer per word, plain 8 B SET writes, no atomics; seq << 40 | v, seq < 2^22 per communicator, fatal beyond):
 *   post  : stage non-heap buffers into the PE's DATA heap, copy the self slab, stream sync, quiet ctx0 (older words), publish
 *           ROFF[me]@peer = (seq, offset of the slab's landing place in my heap) to every peer, hand the exchange to the PE's worker
 *   worker: (one thread per PE) per peer whose ROFF arrived putmem_nbi the
 *           slab to the offset; when every peer is posted ONE quiet of the lane, then SIG[me]@peer = (seq, bytes) per peer, a quiet, flushed = 1
 *   wait  : the APU thread tests SIG[peer] (+ progress) of every sender, checks seq and count (fatal on a mismatch),
 *           waits for flushed, copies staged receive slabs out on the stream and syncs.  One exchange per communicator in flight.
 * Peers are walked in rotated order (me + q) % n (no incast on member 0).
 * Small collectives (barrier, allreduce_max, allgather_host) are word protocols VAL[2][n] + VSEQ[2][n] on ctx0 (parity slots
 * tiny_exchange).  send / recv / allreduce_modq / alltoallv_host / allgather (device) are 0: comm_util.c's fallbacks apply.
 *
 * CTRL layout (words, per communicator id < MAXID, a pure function of the id, so no exchange at create):
 *   ctrl + id * 7 * nw : ROFF[n] SIG[n] VAL0[n] VAL1[n] VSEQ0[n] VSEQ1[n] SOFF[n]   (n = nw: the largest member count = the nodes; with ECALC_PE_PER_GPU
 *   (S1a) the PEs, K * nodes, and only XS_MAXID_VPE ids)
 * Threads: mesh d is driven by APU thread d (device d current at create; the main thread is thread 0).  Every entry selects the loopback
 * PE of the comm (XSHMEM_SELECT); a debug flag catches two threads inside one comm. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <sys/prctl.h>
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "comm.h"
#include "xshmem_round.h"
#include "xshmem.h"
#include "vpe.h"
#include "pe_geo.h"
#ifdef XSHMEM_OFI
#define XS_LPES() xshmem_ofi_loopback_pes()
#else
#define XS_LPES() 1
#endif

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define COPY(d, s, n, st) HIP_CHECK(comm_copy_async(d, s, n, st))
#define SYNC(st) do { HIP_CHECK(hipStreamSynchronize(st)); } while (0)
#define MAXID 1024                         /* communicator ids (control blocks) */
#define MAXID_VPE 512                      /* ... with ECALC_PE_PER_GPU (flat PE communicators: the control block grows with the PE count) */
#define MAXL 4                             /* lane contexts per PE (XSHMEM_LANES) */
#define MAXPST 8                           /* XSHMEM_PEER copy streams per PE */
#define PEERBIT (1u << 8)                  /* lane_put's mask: a same-node device copy is outstanding (lane_quiet syncs the copy streams) */
#define MAXLPE 8                           /* loopback PEs per process */
#define MAXPEND 256                        /* exchanges one worker can hold */
#define ALIGN 256
#define HALIGN ((size_t)2 << 20)           /* the backend rounds heaps to 2 MiB */
#define GBLK 4096                          /* allgather_host: bytes per member block (larger is fatal) */
#define SEQ_LIMIT ((uint64_t)1 << 22)
#define PACK(seq, v) (((uint64_t)(seq) << 40) | (uint64_t)(v))
#define VMASK (((uint64_t)1 << 40) - 1)
enum { W_ROFF, W_SIG, W_VAL0, W_VAL1, W_VSEQ0, W_VSEQ1, W_SOFF, W_N };

struct xblk { size_t off, len; int used; struct xblk *next; };
struct xpriv;
typedef struct xpe {                       /* one loopback PE of this process */
    int lpe; xshmem_ctx_t lane; xshmem_ctx_t lanes[MAXL];   /* lane = lanes[0] */
    char *data; size_t dlen; uint64_t *ctrl;
    pthread_mutex_t alock; struct xblk *blocks; size_t cur, peak, symcur, sympeak;
    hipStream_t pst[MAXPST]; unsigned pst_made, pst_used;   /* XSHMEM_PEER: one stream per same-node peer (pe mod MAXPST) for the device copies */
    double lqt[MAXL]; double lqlast[MAXL];                  /* XSHMEM_LANE_TRACE: summed and last quiet time per lane */
    pthread_t th; int th_on, stop; volatile int busy; pthread_mutex_t qm; pthread_cond_t qcv; struct xpriv *queue[MAXPEND]; volatile int nq;
} xpe;
static struct {
    int yield; double sp[8]; long nsp;               /* summed split of the whole-exchange path, see xex_wait */
    int inited, thr, rot, K, nodes, nw, maxid, q; long nev; double tv; double bv; size_t rbytes; long nrex, nrounds;
    pthread_mutex_t lock;                  /* init and the counters */
    uint64_t maxseq; size_t dbytes;
    xpe pe[MAXLPE]; unsigned char used[MAXLPE][MAXID];
    double t00; int ltrace, lpoll, sigl, lctx; double lslow; int nslow; double lqmax[MAXL]; long lqn[MAXL];   /* XSHMEM_LANE_TRACE / _POLL / _SIG_LANES / _CTX (the Cray stall diagnosis, all off by default) */
    unsigned long long pbytes;      /* payload bytes put by device copy (XSHMEM_PEER) */
    int peer;                       /* XSHMEM_PEER=1 under the ofi backend: same-node payload by device copy through xshmem_ptr */
    int nl; size_t lmin; unsigned long long lb[MAXL];   /* XSHMEM_LANES > 1: lanes in use, the smallest chunk per lane, payload bytes put per lane */
} X = { 1, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, 0, 0, {}, {} };

#define YIELD() do { if (X.yield) sched_yield(); } while (0)
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static inline uint64_t rd64(const uint64_t *p) { return *(volatile const uint64_t *)p; }
static inline int sigge(const uint64_t *p, uint64_t v) { return xshmem_signal_test(p, XSHMEM_CMP_GE, v); }

/* ---- the DATA heap's sub-allocator: first fit, 256 B, symmetric buffers top-down (a copy of comm_ofi_alloc); a lock per PE ---- */
static size_t xalloc(xpe *lp, size_t len, int sym)
{
    len = (len + ALIGN - 1) & ~(size_t)(ALIGN - 1); if (!len) len = ALIGN;
    pthread_mutex_lock(&lp->alock);
    if (sym) {
        struct xblk *best = 0; for (struct xblk *b = lp->blocks; b; b = b->next) if (!b->used && b->len >= len) best = b;
        if (best) {
            struct xblk *b = best;
            if (b->len > len) { struct xblk *nb = (struct xblk *)malloc(sizeof *nb); nb->off = b->off + b->len - len; nb->len = len; nb->next = b->next; b->next = nb; b->len -= len; b = nb; }
            b->used = 2; lp->cur += len; if (lp->cur > lp->peak) lp->peak = lp->cur;
            lp->symcur += len; if (lp->symcur > lp->sympeak) lp->sympeak = lp->symcur;
            size_t off = b->off; pthread_mutex_unlock(&lp->alock); return off;
        }
    } else
        for (struct xblk *b = lp->blocks; b; b = b->next) if (!b->used && b->len >= len) {
            if (b->len > len) { struct xblk *nb = (struct xblk *)malloc(sizeof *nb); nb->off = b->off + len; nb->len = b->len - len; nb->used = 0; nb->next = b->next; b->next = nb; b->len = len; }
            b->used = 1; lp->cur += len; if (lp->cur > lp->peak) lp->peak = lp->cur;
            size_t off = b->off; pthread_mutex_unlock(&lp->alock); return off;
        }
    size_t cur = lp->cur, hole = 0, freetot = 0;
    for (struct xblk *b = lp->blocks; b; b = b->next) if (!b->used) { freetot += b->len; if (b->len > hole) hole = b->len; }
    fprintf(stderr, "comm_xshmem: PE %d pool dump (request %zu MiB; free %zu MiB in total, largest hole %zu MiB; used blocks: off len kind):", X.K * X.q + lp->lpe, len >> 20, freetot >> 20, hole >> 20);
    { int n = 0; for (struct xblk *b = lp->blocks; b && n < 40; b = b->next) if (b->used) { fprintf(stderr, " [%zu %zu %s]", b->off >> 20, b->len >> 20, b->used == 2 ? "sym" : "stg"); n++; } }
    fprintf(stderr, "\n");
    pthread_mutex_unlock(&lp->alock);
    ec_fatal(EC_RC_OOM, "comm_xshmem: the DATA heap of PE %d (%zu MiB, XSHMEM_HEAP_MB) cannot hold %zu MiB more (in use %zu MiB): XSHMEM_HEAP_MB >= %zu needed\n",
             X.K * X.q + lp->lpe, lp->dlen >> 20, len >> 20, cur >> 20, ((cur + len) >> 20) + 2);
    return 0;
}
static void xfree(xpe *lp, size_t off)
{
    pthread_mutex_lock(&lp->alock);
    struct xblk *p = 0;
    for (struct xblk *b = lp->blocks; b; p = b, b = b->next) if (b->off == off && b->used) {
        lp->cur -= b->len; if (b->used == 2) lp->symcur -= b->len; b->used = 0;
        if (b->next && !b->next->used) { struct xblk *n = b->next; b->len += n->len; b->next = n->next; free(n); }
        if (p && !p->used) { p->len += b->len; p->next = b->next; free(b); }
        break;
    }
    pthread_mutex_unlock(&lp->alock);
}
static int in_heap(const xpe *lp, const void *q) { return (const char *)q >= lp->data && (const char *)q < lp->data + lp->dlen; }

/* ---- a communicator and its pending exchange ---- */
typedef struct xpriv {
    int n, me, id, lpe, dev, phk; xpe *lp; int *pe;       /* pe[r]: the xshmem PE of member r */
    uint64_t seq, tseq; volatile int inuse; double te;      /* te: XSHMEM_LANE_TRACE, when the current op was entered */
    /* the pending exchange */
    int pending, v, rin, sin; size_t bytes, sst, rst; double *tarr; double tpost[4]; double t0, t_pe, t_w, t_a, t_p, t_f, t_w0, t_rx;   /* t_*: the exchange's split (post end, worker first visit, all peers posted, payload quiet, signals quiet, wait entry, receive done) */
    hipStream_t st; void *rb;
    size_t *scnt, *rcnt, *sdsp, *rdsp, *spre, *rpre;    /* per member: send / receive counts; offsets in the caller's buffers; offsets in the staging */
    const char *sbase; const size_t *soff;              /* where the slabs are sent from: base + soff[r] (the caller's sb or my staging) */
    char *rbase; const size_t *roff;                    /* where they land: base + roff[r] (the caller's rb or my staging) */
    char *posted, *rxdone; int nposted, touched; volatile int flushed;
    /* rounds (XSHMEM_ROUND_MB > 0): per-pair sequences (oseq[r] my sends to r, iseq[r] r's sends to me; a pair advances by its round count,
     * which both ends derive from the pair's byte count), the state machine of the pending exchange, the own stream of the staging copies */
    int rounds, rph, rneed, rdone; long rK, rj; size_t rch; const char *vsb; char *vrb; size_t *oseq, *iseq; hipStream_t rs; int rs_made;
    size_t gscr; int gready; size_t *gpeer;             /* allgather_host: my scratch (heap offset), the members' */
} xpriv;
#define PRIV(c) ((xpriv *)(c)->priv)
#define ORD(p, q) (((p)->me + (q)) % (p)->n)
static void die(const xpriv *p, const char *m) { ec_fatal(EC_RC_FATAL, "comm_xshmem: pe %d: %s\n", X.K * X.q + (p ? p->lpe : 0), m); }
#define WORD(p, w, i) ((p)->lp->ctrl + ((size_t)(p)->id * W_N + (w)) * (size_t)X.nw + (i))
static void enter(xpriv *p) { if (__atomic_exchange_n(&p->inuse, 1, __ATOMIC_ACQUIRE)) die(p, "two threads inside one communicator"); XSHMEM_SELECT(p->lpe); if (X.ltrace) p->te = now_s(); }
static void leave(xpriv *p)
{
    if (X.ltrace && now_s() - p->te > X.lslow && X.nslow < 120) {   /* XSHMEM_LANE_TRACE: any communicator op (exchange, collective, barrier) slower than the threshold, with the engine's phase tag */
        char hn[64] = "?"; gethostname(hn, sizeof hn); pthread_mutex_lock(&X.lock); int nn = X.nslow++; pthread_mutex_unlock(&X.lock);
        if (nn < 120) fprintf(stderr, "comm_xshmem: SLOWOP host %s pe %d comm %d (n %d) phase tag %d: %.2f s in the op (%.0f s into the run)\n", hn, X.K * X.q + p->lpe, p->id, p->n, p->phk, now_s() - p->te, now_s() - X.t00);
    }
    __atomic_store_n(&p->inuse, 0, __ATOMIC_RELEASE);
}
/* Lane striping (XSHMEM_LANES = N > 1; N = 1 is the unchanged single-context path).  A payload put of len bytes goes as N contiguous chunks, chunk k on
 * lane k (a 256 B multiple), when len >= N x 256 KiB; a smaller one stays whole on lane 0.  The returned mask names the lanes used:
 * the caller quiets every lane of the mask (lane_quiet) BEFORE any signal_set, and the signals all go on lane 0, so each signal follows the completion
 * (at the target) of every payload chunk, whatever lane carried it (xshmem.h: quiet returns when the puts are complete at their targets). */
static unsigned lane_put(xpe *lp, void *dst, const void *src, size_t len, int pe)
{
    int N = X.nl;
    if (X.peer) { void *dp = xshmem_ptr(dst, pe);
        if (dp) { int k = pe % MAXPST;
            if (!(lp->pst_made >> k & 1)) { HIP_CHECK(hipStreamCreateWithFlags(&lp->pst[k], hipStreamNonBlocking)); lp->pst_made |= 1u << k; }
            HIP_CHECK(hipMemcpyAsync(dp, src, len, hipMemcpyDefault, lp->pst[k])); lp->pst_used |= 1u << k;
            __atomic_add_fetch(&X.pbytes, len, __ATOMIC_RELAXED); return PEERBIT; } }
    if (N <= 1) { xshmem_ctx_putmem_nbi(lp->lane, dst, src, len, pe); return 1; }
    if (len < (size_t)N * X.lmin) { xshmem_ctx_putmem_nbi(lp->lanes[0], dst, src, len, pe); __atomic_add_fetch(&X.lb[0], len, __ATOMIC_RELAXED); return 1; }
    size_t ch = (((len + (size_t)N - 1) / (size_t)N) + 255) & ~(size_t)255; unsigned m = 0;
    for (int k = 0; k < N; k++) { size_t o = (size_t)k * ch; if (o >= len) break; size_t l = len - o < ch ? len - o : ch;
        xshmem_ctx_putmem_nbi(lp->lanes[k], (char *)dst + o, (const char *)src + o, l, pe); __atomic_add_fetch(&X.lb[k], l, __ATOMIC_RELAXED); m |= 1u << k; }
    return m;
}
static void lane_quiet(xpe *lp, unsigned m)
{
    if (m & PEERBIT) { for (int k = 0; k < MAXPST; k++) if (lp->pst_used >> k & 1) HIP_CHECK(hipStreamSynchronize(lp->pst[k])); lp->pst_used = 0; }   /* the device copies are complete (before any signal) */
    for (int k = 0; k < (X.nl > 1 ? X.nl : 1); k++) if (m >> k & 1) {
        if (!X.ltrace) { xshmem_ctx_quiet(lp->lanes[k]); continue; }
        double t = now_s(); xshmem_ctx_quiet(lp->lanes[k]); t = now_s() - t; lp->lqlast[k] = t; lp->lqt[k] += t;
        pthread_mutex_lock(&X.lock); if (t > X.lqmax[k]) X.lqmax[k] = t; X.lqn[k]++; pthread_mutex_unlock(&X.lock);
    }
}
/* XSHMEM_LANE_POLL=1: a PE that waits (signals, worker idle) also drives the progress of its extra lanes: a quiet on a lane with nothing outstanding only polls its completion queue */
static void lane_poll(xpe *lp) { if (X.lpoll && X.nl > 1) for (int k = 1; k < X.nl; k++) xshmem_ctx_quiet(lp->lanes[k]); }
/* the lane a signal to member i goes on: lane 0, or with XSHMEM_SIG_LANES=1 lane i mod N (every signal still follows the quiet of every payload lane) */
static xshmem_ctx_t sig_lane(xpe *lp, int i) { return X.sigl && X.nl > 1 ? lp->lanes[i % X.nl] : lp->lanes[0]; }
static void sig_quiet(xpe *lp) { if (X.sigl && X.nl > 1) for (int k = 1; k < X.nl; k++) xshmem_ctx_quiet(lp->lanes[k]); xshmem_ctx_quiet(lp->lanes[0]); }

/* the sender's half of the pending exchange (worker, or the APU thread in poll-only mode).  Returns 1 when done (every slab put, signalled,
 * quieted); the caller then publishes flushed.  Touches only the exchange's constant fields and `posted` / `nposted` / `touched`. */
static int xex_send(xpriv *p)
{
    int n = p->n; uint64_t seq = p->seq;
    if (p->t_w == 0) p->t_w = now_s();
    for (int q = 0; q < n; q++) { int i = ORD(p, q); if (p->posted[i]) continue;
        const uint64_t *w = WORD(p, W_ROFF, i);
        if (!sigge(w, PACK(seq, 0))) continue;
        uint64_t v = rd64(w);
        if ((v >> 40) != seq) die(p, "offset sequence mismatch");
        if (p->scnt[i]) p->touched |= (int)lane_put(p->lp, xshmem_at((ptrdiff_t)(v & VMASK)), p->sbase + p->soff[i], p->scnt[i], p->pe[i]);
        p->posted[i] = 1; p->nposted++; if (X.ltrace) p->tarr[i] = now_s() - p->t0;
    }
    if (p->nposted < n) return 0;
    p->t_a = now_s();
    if (p->touched) lane_quiet(p->lp, (unsigned)p->touched);                         /* the batched flush: every payload visible (every lane used) */
    p->t_p = now_s();
    for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != p->me) xshmem_ctx_signal_set(sig_lane(p->lp, i), WORD(p, W_SIG, p->me), PACK(seq, p->scnt[i]), p->pe[i]); }
    sig_quiet(p->lp);                                                   /* the signals are out too: sb reusable */
    p->t_f = now_s();
    return 1;
}
static void xex_hang(xpriv *p);
static int xex_round(xpriv *p);
static void *worker(void *a)
{
    xpe *lp = (xpe *)a; struct xpriv *act[MAXPEND]; int na = 0; unsigned idle = 0, wdev_ = (unsigned)-1;
    XSHMEM_SELECT(lp->lpe);
    for (;;) {
        if (lp->nq > 0 || na == 0) {
            pthread_mutex_lock(&lp->qm);
            while (na == 0 && lp->nq == 0 && !lp->stop) pthread_cond_wait(&lp->qcv, &lp->qm);
            if (na + lp->nq > MAXPEND) { pthread_mutex_unlock(&lp->qm); ec_fatal(EC_RC_FATAL, "comm_xshmem: PE %d: more than %d pending exchanges\n", X.K * X.q + lp->lpe, MAXPEND); }
            for (int i = 0; i < lp->nq; i++) act[na++] = lp->queue[i];
            lp->nq = 0;
            int stop = lp->stop && na == 0;
            pthread_mutex_unlock(&lp->qm);
            lp->busy = na;
            if (stop) return 0;
        }
        int prog = 0;
        for (int i = 0; i < na; i++) {
            xpriv *p = act[i];
            if (!p->rounds && wdev_ != (unsigned)p->dev) { HIP_CHECK(hipSetDevice(p->dev)); wdev_ = (unsigned)p->dev; }   /* S1b: the worker works on its PE's device (it was device 0 for every PE: under vPE four workers on one device) */
            if (p->rounds ? xex_round(p) : xex_send(p)) { act[i] = act[--na]; i--; lp->busy = na; __atomic_store_n(&p->flushed, 1, __ATOMIC_RELEASE); prog = 1; }
        }
        if (prog) idle = 0;
        else {
            if (X.lpoll && !(idle & 255)) lane_poll(lp);
            if (++idle > 32) YIELD();
            if (!(idle & 4095)) { double t = now_s(); for (int i = 0; i < na; i++) if (t - act[i]->t0 > 900) xex_hang(act[i]); }   /* a pending exchange nobody waits for hangs too */
        }
    }
}

/* ---- the exchange in rounds (XSHMEM_ROUND_MB = m > 0; a non-blocking state machine run by the PE's worker) ----
 * A pair (s -> r) of b bytes goes in round_count(b, c) rounds of c = m / (n - 1) bytes (>= 4 KiB, 256 B multiple); every round stages at most m
 * each way and leaves the self slab out.  Round j: publish ROFF for every sender still active (the landing place of its chunk j: rb + rdsp + j c, or the
 * receive staging slot), stage my chunks j, put each after the receiver's word of round j, ONE quiet, one signal (seq, bytes) per peer and round, wait for
 * the signals of round j, quiet, copy the received chunks out.  Round j + 1 starts only after round j completed, so a staging slot is never overwritten
 * early.  The words carry the pair's sequence + 1 + j, so a signal of another round or exchange is a fatal mismatch, never silently taken. */
static long rcount(size_t b, size_t c) { return b <= c ? 1 : (long)((b + c - 1) / c); }
static size_t rlen(size_t cnt, size_t c, long j) { size_t o = (size_t)j * c; return cnt > o ? (cnt - o < c ? cnt - o : c) : 0; }
static __thread int wdev = -1;
static int xex_round(xpriv *p)    /* one step; 1 when every round is done (the pairs' sequences advanced) */
{
    int n = p->n, me = p->me; size_t c = p->rch; xpe *lp = p->lp;
    if (wdev != p->dev) { HIP_CHECK(hipSetDevice(p->dev)); wdev = p->dev; }
    for (;;) {
        long j = p->rj; size_t o = (size_t)j * c;
        if (j >= p->rK) { for (int r = 0; r < n; r++) if (r != me) { p->oseq[r] += (uint64_t)rcount(p->scnt[r], c); p->iseq[r] += (uint64_t)rcount(p->rcnt[r], c); } return 1; }
        if (p->rph == 0) {                                                   /* publish round j, stage my chunks j */
            for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i == me || j >= rcount(p->rcnt[i], c)) continue;
                const char *land = p->rin ? p->vrb + (p->rcnt[i] ? p->rdsp[i] + o : 0) : lp->data + p->rst + p->rpre[i];
                xshmem_ctx_signal_set(lp->lane, WORD(p, W_ROFF, me), PACK(p->iseq[i] + 1 + j, xshmem_off(land)), p->pe[i]); }
            if (!p->sin) {
                int any = 0;
                for (int r = 0; r < n; r++) if (r != me && j < rcount(p->scnt[r], c) && p->scnt[r] > o) { COPY(lp->data + p->sst + p->spre[r], p->vsb + p->sdsp[r] + o, rlen(p->scnt[r], c, j), p->rs); any = 1; }
                if (any) SYNC(p->rs);
            }
            memset(p->posted, 0, (size_t)n); memset(p->rxdone, 0, (size_t)n); p->touched = 0; p->rdone = 0; p->rneed = 0;
            for (int r = 0; r < n; r++) if (r != me && j < rcount(p->scnt[r], c)) p->rneed++;
            p->rph = 1;
        }
        if (p->rph == 1) {                                                   /* the sends of round j, after the receivers' words */
            for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i == me || p->posted[i] || j >= rcount(p->scnt[i], c)) continue;
                uint64_t seq = p->oseq[i] + 1 + (uint64_t)j; const uint64_t *w = WORD(p, W_ROFF, i);
                if (!sigge(w, PACK(seq, 0))) continue;
                uint64_t v = rd64(w); if ((v >> 40) != seq) die(p, "round sequence mismatch");
                size_t len = rlen(p->scnt[i], c, j);
                if (len) p->touched |= (int)lane_put(lp, xshmem_at((ptrdiff_t)(v & VMASK)), p->sin ? p->vsb + p->sdsp[i] + o : lp->data + p->sst + p->spre[i], len, p->pe[i]);
                p->posted[i] = 1; p->rdone++;
            }
            if (p->rdone < p->rneed) return 0;
            if (p->touched) lane_quiet(lp, (unsigned)p->touched);
            for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != me && j < rcount(p->scnt[i], c))
                xshmem_ctx_signal_set(sig_lane(lp, i), WORD(p, W_SIG, me), PACK(p->oseq[i] + 1 + j, rlen(p->scnt[i], c, j)), p->pe[i]); }
            p->rdone = 0; p->rneed = 0;
            for (int r = 0; r < n; r++) if (r != me && j < rcount(p->rcnt[r], c)) p->rneed++;
            p->rph = 2;
        }
        {                                                                    /* the signals of round j, then the staging is free again */
            for (int i = 0; i < n; i++) { if (i == me || p->rxdone[i] || j >= rcount(p->rcnt[i], c)) continue;
                uint64_t seq = p->iseq[i] + 1 + (uint64_t)j; const uint64_t *w = WORD(p, W_SIG, i);
                if (!sigge(w, PACK(seq, 0))) continue;
                uint64_t v = rd64(w); if ((v >> 40) != seq) die(p, "round signal sequence mismatch");
                size_t want = rlen(p->rcnt[i], c, j);
                if ((v & VMASK) != want) ec_fatal(EC_RC_FATAL, "comm_xshmem: alltoallv count mismatch (round %ld): rank %d sends %ld bytes to rank %d, which expects %zu\n", j, i, (long)(v & VMASK), me, want);
                p->rxdone[i] = 1; p->rdone++;
            }
            if (p->rdone < p->rneed) return 0;
            sig_quiet(lp);
            if (!p->rin) {
                int any = 0;
                for (int r = 0; r < n; r++) if (r != me && j < rcount(p->rcnt[r], c) && p->rcnt[r] > o) { COPY(p->vrb + p->rdsp[r] + o, lp->data + p->rst + p->rpre[r], rlen(p->rcnt[r], c, j), p->rs); any = 1; }
                if (any) SYNC(p->rs);                                        /* before round j + 1 publishes the same slots */
            }
            p->rj++; p->rph = 0;
        }
    }
}
static void xex_post_rounds(xpriv *p, const void *sb, void *rb, hipStream_t s)
{
    int n = p->n, me = p->me; xpe *lp = p->lp; size_t c = X.rbytes / (size_t)(n - 1); c &= ~(size_t)(ALIGN - 1); if (c < 4096) c = 4096;
    if (p->scnt[me] != p->rcnt[me]) die(p, "alltoallv self count mismatch");
    long K = 1;
    for (int r = 0; r < n; r++) if (r != me) { long ko = rcount(p->scnt[r], c), ki = rcount(p->rcnt[r], c); if (ko > K) K = ko; if (ki > K) K = ki; }
    for (int r = 0; r < n; r++) if (r != me && (p->oseq[r] + (uint64_t)K >= SEQ_LIMIT || p->iseq[r] + (uint64_t)K >= SEQ_LIMIT)) die(p, "exchange sequence overflow (2^22 exchanges or rounds on one communicator)");
    p->t0 = now_s();
    size_t ss = 0, rs = 0;                                /* a slot per peer: its chunk, min(c, count); the self slab is not staged */
    for (int r = 0; r < n; r++) { size_t a = r == me ? 0 : (p->scnt[r] < c ? p->scnt[r] : c), b = r == me ? 0 : (p->rcnt[r] < c ? p->rcnt[r] : c); p->spre[r] = ss; p->rpre[r] = rs; ss += a; rs += b; }
    p->sin = in_heap(lp, sb); p->rin = in_heap(lp, rb); p->sst = p->rst = 0;
    if (!p->sin) p->sst = xalloc(lp, ss + 8, 0);
    if (!p->rin) p->rst = xalloc(lp, rs + 8, 0);
    if (!p->rs_made) { HIP_CHECK(hipGetDevice(&p->dev)); HIP_CHECK(hipStreamCreateWithFlags(&p->rs, hipStreamNonBlocking)); p->rs_made = 1; }
    p->rounds = 1; p->rch = c; p->rK = K; p->rj = 0; p->rph = 0; p->vsb = (const char *)sb; p->vrb = (char *)rb; p->rb = rb; p->st = s; p->pending = 1; p->flushed = 0;
    if (p->scnt[me] && (const char *)sb + p->sdsp[me] != (char *)rb + p->rdsp[me]) COPY((char *)rb + p->rdsp[me], (const char *)sb + p->sdsp[me], p->scnt[me], s);
    SYNC(s);                                              /* the caller's stream is done with sb and rb: publish only after */
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
    pthread_mutex_lock(&X.lock); X.nrex++; X.nrounds += K; pthread_mutex_unlock(&X.lock);
    pthread_mutex_lock(&lp->qm);
    if (lp->nq >= MAXPEND) { pthread_mutex_unlock(&lp->qm); die(p, "too many pending exchanges on one PE"); }
    lp->queue[lp->nq++] = p; pthread_cond_signal(&lp->qcv);
    pthread_mutex_unlock(&lp->qm);
}

/* stage, copy the self slab, publish where every sender's slab lands, start the sender half; one exchange per communicator */
static void xex_post(xpriv *p, const void *sb, void *rb, hipStream_t s)
{
    int n = p->n, me = p->me; xpe *lp = p->lp;
    if (p->pending) die(p, p->v ? "alltoallv while an exchange is pending" : "alltoall while one is pending");
    if (X.rbytes && n > 1) { xex_post_rounds(p, sb, rb, s); return; }
    p->t0 = now_s();
    if (p->scnt[me] != p->rcnt[me]) die(p, "alltoallv self count mismatch");
    size_t ts = comm_prefix(p->scnt, p->spre, n), tr = comm_prefix(p->rcnt, p->rpre, n);
    p->sin = in_heap(lp, sb); p->rin = in_heap(lp, rb); p->sst = p->rst = 0;
    if (!p->sin) p->sst = xalloc(lp, ts + 8, 0);
    if (!p->rin) p->rst = xalloc(lp, tr + 8, 0);
    if (X.ltrace) { p->tpost[0] = now_s() - p->t0; memset(p->tarr, 0, (size_t)n * sizeof(double)); }
    p->seq++; if (p->seq >= SEQ_LIMIT) die(p, "exchange sequence overflow (2^22 exchanges on one communicator)");
    if (p->seq > X.maxseq) X.maxseq = p->seq;
    p->rb = rb; p->st = s; p->pending = 1;
    if (p->sin) { p->sbase = (const char *)sb; p->soff = p->sdsp; } else { p->sbase = lp->data + p->sst; p->soff = p->spre; }
    if (p->rin) { p->rbase = (char *)rb; p->roff = p->rdsp; } else { p->rbase = lp->data + p->rst; p->roff = p->rpre; }
    if (!p->sin) {
        if (!p->v) COPY(lp->data + p->sst, sb, ts, s);                                      /* equal slabs: contiguous, all of them */
        else for (int r = 0; r < n; r++) if (r != me && p->scnt[r]) COPY(lp->data + p->sst + p->spre[r], (const char *)sb + p->sdsp[r], p->scnt[r], s);
    }
    if (p->scnt[me] && (const char *)sb + p->sdsp[me] != (char *)rb + p->rdsp[me]) COPY((char *)rb + p->rdsp[me], (const char *)sb + p->sdsp[me], p->scnt[me], s);
    if (X.ltrace) p->tpost[1] = now_s() - p->t0;
    SYNC(s);                                              /* the stream is done with sb and rb: publish only after */
    if (X.ltrace) p->tpost[2] = now_s() - p->t0;
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);                 /* older word writes (ROFF, collectives) */
    if (X.ltrace) p->tpost[3] = now_s() - p->t0;
    p->t_w = p->t_a = p->t_p = p->t_f = p->t_rx = 0;
    memset(p->posted, 0, (size_t)n); memset(p->rxdone, 0, (size_t)n); p->posted[me] = 1; p->nposted = 1; p->touched = 0; p->flushed = 0;
    for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != me)
        xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_ROFF, me), PACK(p->seq, xshmem_off(p->rbase + (p->rcnt[i] ? p->roff[i] : 0))), p->pe[i]); }
    if (n == 1) { p->flushed = 1; return; }
    {
        pthread_mutex_lock(&lp->qm);
        if (lp->nq >= MAXPEND) { pthread_mutex_unlock(&lp->qm); die(p, "too many pending exchanges on one PE"); }
        lp->queue[lp->nq++] = p; pthread_cond_signal(&lp->qcv);
        pthread_mutex_unlock(&lp->qm);
    }
    p->t_pe = now_s();
}
static void xex_hang(xpriv *p)
{
    int mp = 0, mr = 0; for (int i = 0; i < p->n; i++) { if (!p->posted[i]) mp++; if (i != p->me && !p->rxdone[i]) mr++; }
    ec_fatal(EC_RC_FATAL, "comm_xshmem: pe %d: HANG (> 900 s) in an exchange: comm %d seq %llu%s: %d peers not posted (no offset word), %d signal words missing, flushed %d (rounds: round %ld of %ld, phase %d)\n",
             X.K * X.q + p->lpe, p->id, (unsigned long long)p->seq, p->rounds ? " (in rounds)" : "", mp, mr, p->flushed, p->rj, p->rK, p->rph);
}
static int xex_rx(xpriv *p)    /* test the receive words; 1 when every slab is in */
{
    int left = 0;
    for (int i = 0; i < p->n; i++) if (i != p->me && !p->rxdone[i]) {
        const uint64_t *w = WORD(p, W_SIG, i);
        if (!sigge(w, PACK(p->seq, 0))) { left++; continue; }
        uint64_t v = rd64(w);
        if ((v >> 40) != p->seq) die(p, "signal sequence mismatch");
        if ((v & VMASK) != p->rcnt[i]) ec_fatal(EC_RC_FATAL, "comm_xshmem: alltoallv count mismatch: rank %d sends %ld bytes to rank %d, which expects %ld\n", i, (long)(v & VMASK), p->me, (long)p->rcnt[i]);
        p->rxdone[i] = 1;
    }
    return left == 0;
}
static double recv_bytes(const xpriv *p) { double b = 0; for (int r = 0; r < p->n; r++) if (r != p->me) b += (double)p->rcnt[r]; return b; }
static void xsplit_print(void);
static void xex_wait(xpriv *p)
{
    int n = p->n, me = p->me; double t0 = now_s(); unsigned spins = 0; int rx = 0, rdone = p->rounds;
    p->t_w0 = t0;
    if (p->rounds) {                                      /* the worker ran every round, copies included: wait for it */
        while (!__atomic_load_n(&p->flushed, __ATOMIC_ACQUIRE)) if (!(++spins & 4095)) { if (now_s() - p->t0 > 900) xex_hang(p); if (spins > 100000) YIELD(); }
        p->rounds = 0; rx = 1;
    } else
    for (;;) {
        if (!rx) { rx = xex_rx(p); if (rx) p->t_rx = now_s(); }
        if (rx && __atomic_load_n(&p->flushed, __ATOMIC_ACQUIRE)) break;
        if (!(++spins & 4095)) { if (now_s() - t0 > 900) xex_hang(p); if (spins > 100000) YIELD(); }
        if (X.lpoll && !(spins & 255)) lane_poll(p->lp);
    }
    if (!p->rin && !rdone) {
        for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], p->rbase + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    if (p->sst) xfree(p->lp, p->sst);
    if (p->rst) xfree(p->lp, p->rst);
    p->sst = p->rst = 0; p->pending = 0;
    double t = now_s() - p->t0, b = recv_bytes(p);
    if (X.ltrace && t > X.lslow && X.nslow < 120) {            /* XSHMEM_LANE_TRACE=<s>: where a slow exchange spent its time (one line per exchange, at most 120 per process) */
        int mr = 0, mp = 0; for (int i = 0; i < n; i++) { if (i != me && !p->rxdone[i]) mr++; if (!p->posted[i]) mp++; }
        int l1 = -1, l2 = -1; double v1 = 0, v2 = 0; for (int i = 0; i < n; i++) if (i != me) { double a = p->tarr[i]; if (a > v1) { l2 = l1; v2 = v1; l1 = i; v1 = a; } else if (a > v2) { l2 = i; v2 = a; } }
        pthread_mutex_lock(&X.lock); int nn = X.nslow++; pthread_mutex_unlock(&X.lock);
        if (nn < 120) { char hn[64] = "?"; gethostname(hn, sizeof hn); fprintf(stderr, "comm_xshmem: SLOW host %s pe %d comm %d phase tag %d seq %llu%s: %.2f s for %.1f MB in: post %.3f, worker wake %.3f, peers posted %.3f, payload quiet %.3f (lane quiet last: %.3f %.3f %.3f %.3f), signals quiet %.3f; wait entered at %.3f, receive done at %.3f, send done at %.3f; unposted %d unreceived %d; post split (cumulative s): staging alloc %.3f, copies queued %.3f, stream sync %.3f, default quiet %.3f; last offset words from member %d at %.3f s, member %d at %.3f s\n",
                hn, X.K * X.q + p->lpe, p->id, p->phk, (unsigned long long)p->seq, p->rounds ? " (rounds)" : "", t, b / 1e6, p->t_pe - p->t0, p->t_w - p->t_pe, p->t_a - p->t_w, p->t_p - p->t_a,
                p->lp->lqlast[0], p->lp->lqlast[1], p->lp->lqlast[2], p->lp->lqlast[3], p->t_f - p->t_p, p->t_w0 - p->t0, p->t_rx - p->t0, p->t_f - p->t0, mp, mr, p->tpost[0], p->tpost[1], p->tpost[2], p->tpost[3], l1, v1, l2, v2); }
    }
    pthread_mutex_lock(&X.lock); X.tv += t; X.nev++; X.bv += b;
    if (p->t_f > 0 && p->t_w > 0 && p->t_rx > 0) {   /* the whole-exchange path with the worker: the split */
        X.sp[0] += p->t_pe - p->t0; X.sp[1] += p->t_w - p->t_pe; X.sp[2] += p->t_a - p->t_w; X.sp[3] += p->t_p - p->t_a; X.sp[4] += p->t_f - p->t_p;
        X.sp[5] += p->t_w0 - p->t0; X.sp[6] += p->t_rx - p->t0; X.sp[7] += (p->t_f > p->t_rx ? p->t_f : p->t_rx) - p->t0; X.nsp++;
        if (X.nsp % 4000 == 0) xsplit_print();   /* (the diagnostic runs are killed by a timeout: no finalize) */
    }
    pthread_mutex_unlock(&X.lock);
}

static void xsplit_print(void)   /* mean per exchange, ms; the caller holds X.lock or is alone */
{
    fprintf(stderr, "comm_xshmem: node-process %d: exchange split (mean ms over %ld PE-exchanges: post %.3f, worker wake %.3f, peers posted %.3f, payload quiet %.3f, signals quiet %.3f; wait entered at %.3f, receive done at %.3f, send+receive done at %.3f)\n", X.q, X.nsp, X.nsp ? 1e3 * X.sp[0] / X.nsp : 0, X.nsp ? 1e3 * X.sp[1] / X.nsp : 0, X.nsp ? 1e3 * X.sp[2] / X.nsp : 0, X.nsp ? 1e3 * X.sp[3] / X.nsp : 0, X.nsp ? 1e3 * X.sp[4] / X.nsp : 0, X.nsp ? 1e3 * X.sp[5] / X.nsp : 0, X.nsp ? 1e3 * X.sp[6] / X.nsp : 0, X.nsp ? 1e3 * X.sp[7] / X.nsp : 0);
}
/* ---- the ops ---- */
static int x_rank(comm *c) { return c->rank; }
static int x_size(comm *c) { return c->size; }
static void x_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    xpriv *p = PRIV(c); p->phk = comm_xtag < 0 || comm_xtag >= XK_NK ? 0 : comm_xtag;
    enter(p);
    for (int r = 0; r < p->n; r++) { p->scnt[r] = p->rcnt[r] = bytes; p->sdsp[r] = p->rdsp[r] = (size_t)r * bytes; }
    p->v = 0; xex_post(p, sb, rb, s);
    leave(p);
}
static void x_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    xpriv *p = PRIV(c); p->phk = comm_xtag < 0 || comm_xtag >= XK_NK ? 0 : comm_xtag;
    enter(p);
    if (p->pending) die(p, "alltoallv while an exchange is pending");   /* (before the caller's arrays overwrite the pending counts) */
    memcpy(p->scnt, scnt, (size_t)p->n * sizeof(size_t)); memcpy(p->sdsp, sdsp, (size_t)p->n * sizeof(size_t));
    memcpy(p->rcnt, rcnt, (size_t)p->n * sizeof(size_t)); memcpy(p->rdsp, rdsp, (size_t)p->n * sizeof(size_t));
    p->v = 1; xex_post(p, sb, rb, s);
    leave(p);
}
static void x_wait(comm *c)
{
    xpriv *p = PRIV(c); if (!p->pending) return;
    enter(p); xex_wait(p); leave(p);
}
/* the tiny exchange: every member's 8-byte value to every member (value, quiet, then the sequence word; slots alternate with the parity) */
static void tiny_exchange(xpriv *p, uint64_t v, uint64_t *all)
{
    int n = p->n, me = p->me; uint64_t seq = ++p->tseq; int par = (int)(seq & 1);
    all[me] = v;
    for (int q = 0; q < n; q++) { int r = ORD(p, q); if (r != me) xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_VAL0 + par, me), v, p->pe[r]); }
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
    for (int q = 0; q < n; q++) { int r = ORD(p, q); if (r != me) xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_VSEQ0 + par, me), seq, p->pe[r]); }
    double w0 = comm_wst_t0(), t0 = now_s(); unsigned spins = 0;
    for (int r = 0; r < n; r++) if (r != me) {
        const uint64_t *w = WORD(p, W_VSEQ0 + par, r);
        while (!sigge(w, seq)) if (!(++spins & 4095)) { if (now_s() - t0 > 900) die(p, "HANG (> 900 s) in a collective"); if (spins > 100000) YIELD(); }
        all[r] = rd64(WORD(p, W_VAL0 + par, r));
    }
    comm_wst_ready_end(w0);
}
static void x_barrier(comm *c)
{
    xpriv *p = PRIV(c); if (p->n == 1) return;
    uint64_t *all = (uint64_t *)malloc((size_t)p->n * 8);
    enter(p); tiny_exchange(p, 0, all); leave(p); free(all);
}
static size_t x_max(comm *c, size_t v)
{
    xpriv *p = PRIV(c); if (p->n == 1) return v;
    uint64_t *all = (uint64_t *)malloc((size_t)p->n * 8), m = v;
    enter(p); tiny_exchange(p, v, all); leave(p);
    for (int r = 0; r < p->n; r++) if (all[r] > m) m = all[r];
    free(all); return (size_t)m;
}
/* allgather of host blocks (k u64 per node): blocks <= 4 KiB go through a scratch of the heap, [parity][member][4 KiB] per member, offsets
 * exchanged once at the first call (SOFF words); then put, quiet, VSEQ word -- the tiny exchange's sequence stream */
static void x_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    xpriv *p = PRIV(c); int n = p->n, me = p->me; xpe *lp = p->lp;
    if (p->pending) die(p, "allgather while an exchange is pending");
    if (bytes > GBLK) ec_fatal(EC_RC_FATAL, "comm_xshmem: pe %d: allgather_host of %zu bytes per member: at most %d\n", X.K * X.q + p->lpe, bytes, GBLK);
    char *self = (char *)rb + (size_t)me * bytes; if (self != sb) memcpy(self, sb, bytes);
    if (n == 1) return;
    enter(p);
    if (!p->gready) {
        p->gscr = xalloc(lp, (size_t)(2 * n + 1) * GBLK, 1); p->gpeer = (size_t *)calloc((size_t)n, sizeof(size_t));
        for (int r = 0; r < n; r++) if (r != me) xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_SOFF, me), (uint64_t)p->gscr + 1, p->pe[r]);
        xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
        double t0 = now_s(); unsigned spins = 0;
        for (int r = 0; r < n; r++) if (r != me) {
            const uint64_t *w = WORD(p, W_SOFF, r);
            while (!sigge(w, 1)) if (!(++spins & 4095) && now_s() - t0 > 900) die(p, "HANG (> 900 s) in the allgather handshake");
            p->gpeer[r] = (size_t)rd64(w) - 1;
        }
        p->gready = 1;
    }
    uint64_t seq = ++p->tseq; int par = (int)(seq & 1);
    char *mine = lp->data + p->gscr + (size_t)2 * n * GBLK;                          /* my send block */
    if (bytes) HIP_CHECK(hipMemcpy(mine, sb, bytes, hipMemcpyDefault));
    for (int q = 0; q < n; q++) { int r = ORD(p, q); if (r != me && bytes)
        xshmem_ctx_putmem_nbi(XSHMEM_CTX_DEFAULT, xshmem_at((ptrdiff_t)(p->gpeer[r] + ((size_t)par * n + me) * GBLK)), mine, bytes, p->pe[r]); }
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);
    for (int q = 0; q < n; q++) { int r = ORD(p, q); if (r != me) xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_VSEQ0 + par, me), seq, p->pe[r]); }
    double w0 = comm_wst_t0(), t0 = now_s(); unsigned spins = 0;
    for (int r = 0; r < n; r++) if (r != me) {
        const uint64_t *w = WORD(p, W_VSEQ0 + par, r);
        while (!sigge(w, seq)) if (!(++spins & 4095)) { if (now_s() - t0 > 900) die(p, "HANG (> 900 s) in allgather_host"); if (spins > 100000) YIELD(); }
        if (bytes) HIP_CHECK(hipMemcpy((char *)rb + (size_t)r * bytes, lp->data + p->gscr + ((size_t)par * n + r) * GBLK, bytes, hipMemcpyDefault));
    }
    comm_wst_ready_end(w0);
    leave(p);
}
static void *x_sym_alloc(comm *c, size_t bytes)
{
    xpriv *p = PRIV(c);
    void *r = p->lp->data + xalloc(p->lp, bytes, 1); return r;
}
static void x_sym_free(comm *c, void *ptr)
{
    (void)c;
    for (int l = 0; l < X.K; l++) if (in_heap(&X.pe[l], ptr)) { xfree(&X.pe[l], (size_t)((char *)ptr - X.pe[l].data)); return; }
    ec_fatal(EC_RC_FATAL, "comm_xshmem: sym_free of a pointer outside the heaps\n");
}
/* COLLECTIVE (x_barrier over the members): every member must destroy its communicators in one agreed order, or the barriers deadlock.  mn.c sub_destroy_all
 * orders by descending communicator id (the id is a function of the run, identical on all members), not by creation order. */
static void x_destroy(comm *c)
{
    xpriv *p = PRIV(c);
    if (p->pending) x_wait(c);
    x_barrier(c);                                          /* nobody's words go while a member may still write to them */
    enter(p); xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT); leave(p);
    if (p->gready) { xfree(p->lp, p->gscr); free(p->gpeer); }
    if (p->rs_made) { HIP_CHECK(hipStreamDestroy(p->rs)); }
    free(p->pe); free(p->oseq); free(p->scnt); free(p->posted); free(p); free(c);
}
static const struct comm_ops xsh_ops = { x_rank, x_size, x_alltoall, x_wait, x_barrier, 0, x_max, x_destroy, 0, 0, 0, x_allgather_host, x_alltoallv, 0, x_sym_alloc, x_sym_free };

/* ---- process level ---- */
int comm_xshmem_available(void) { return 1; }
int comm_xshmem_rank(void) { return X.q; }
void comm_xshmem_counters(double *t, long *n, double *b) { pthread_mutex_lock(&X.lock); *t = X.tv; *n = X.nev; *b = X.bv; pthread_mutex_unlock(&X.lock); }
static size_t round_up(size_t b) { return (b + HALIGN - 1) & ~(HALIGN - 1); }
int comm_xshmem_init(void)
{
    pthread_mutex_lock(&X.lock);
    if (X.inited) { pthread_mutex_unlock(&X.lock); return X.nodes; }
    xshmem_cfg_t cfg; xshmem_cfg_load(&cfg);
    { const char *er = xshmem_round_env(); double m = er ? atof(er) : 0; X.rbytes = m > 0 ? (size_t)(m * 1048576.0) : 0; }
#ifdef XSHMEM_CRAY
    { const char *en = getenv("XSHMEM_CRAY_NUMA");   /* 1: pin this process to its NUMA node's cpus BEFORE libsma init and our heap first touch (the Cray topo has no cpu list otherwise; sysfs via pe_geo, GPU index -> NUMA node as the ofi backend) */
      if (en && atoi(en) > 0 && vpe_ppg()) {   /* one process per PE only: under the default layout (one process per node) there is no per-PE NUMA node */
          const char *a = getenv("SLURM_PROCID"), *b = getenv("SLURM_NTASKS"), *c = getenv("SLURM_LOCALID"), *d = getenv("SLURM_NTASKS_PER_NODE"); pe_topo g;
          int ngpu = 0; char bus[64] = { 0 }; char *bp = NULL;   /* the same GPU selection and bus id as xshmem_topo, so the early and the late NUMA node agree */
#ifdef USE_HIP
          if (c && hipGetDeviceCount(&ngpu) != hipSuccess) { (void)hipGetLastError(); ngpu = 0; }
          if (c && ngpu > 0) { const char *pg = getenv("XSHMEM_PES_PER_GPU"); int ppg_ = pg && atoi(pg) > 0 ? atoi(pg) : 1, dev = ngpu == 1 ? 0 : (atoi(c) / ppg_) % ngpu;
              if (hipDeviceGetPCIBusId(bus, sizeof bus, dev) == hipSuccess) { for (char *q = bus; *q; q++) if (*q >= 'A' && *q <= 'F') *q += 'a' - 'A'; bp = bus; } else (void)hipGetLastError(); }
#endif
          if (a && b && c && d && !pe_geo_from(&g, atoi(a), atoi(b), atoi(c), atoi(d), ngpu, bp)) { xshmem_early_numa = g.numa; vpe_pe_affinity(g.cpus); }
          else fprintf(stderr, "comm_xshmem: XSHMEM_CRAY_NUMA=1: no SLURM geometry or pe_geo failed (%s): no pinning\n", a && b && c && d ? pe_geo_err() : "missing env"); } }
#endif
    int tl = XSHMEM_THREAD_MULTIPLE; { const char *et = getenv("XSHMEM_THREAD_LEVEL"); if (et && atoi(et) >= 0 && atoi(et) <= 3 && *et) tl = atoi(et); }   /* 2 = SERIALIZED (the ofi backend's level); default 3 */
    int prov = 0; if (xshmem_init_thread(tl, &prov)) ec_fatal(EC_RC_FATAL, "comm_xshmem: xshmem_init_thread failed\n");
    int K = XS_LPES(); if (K < 1 || K > MAXLPE) ec_fatal(EC_RC_FATAL, "comm_xshmem: %d loopback PEs per process (1..%d; XSHMEM_LOOPBACK_PES)\n", K, MAXLPE);
    if (vpe_ppg() && K != 1) ec_fatal(EC_RC_FATAL, "comm_xshmem: ECALC_PE_PER_GPU=1 is one process per PE: XSHMEM_LOOPBACK_PES=%d, it must be 1\n", K);   /* S2a */
    X.K = K; XSHMEM_SELECT(0);
    int np = xshmem_n_pes(); if (np % K) ec_fatal(EC_RC_FATAL, "comm_xshmem: %d PEs are not whole processes of %d loopback PEs\n", np, K);
    X.nodes = np / K; X.q = xshmem_my_pe() / K;
    X.peer = cfg.peer && K == 1;
#ifdef XSHMEM_CRAY
    if (cfg.peer == 1 && xshmem_my_pe() == 0) fprintf(stderr, "comm_xshmem: XSHMEM_PEER=1 is inert on the Cray backend (the node-local path is the library's)\n");
    X.peer = 0;
#endif
    int nlw = cfg.lanes > 1 ? (cfg.lanes > MAXL ? MAXL : cfg.lanes) : 1; X.nl = nlw; if (cfg.lanes > MAXL && xshmem_my_pe() == 0) fprintf(stderr, "comm_xshmem: XSHMEM_LANES=%d clamped to %d (the limit)\n", cfg.lanes, MAXL);
    { const char *e;
      e = getenv("XSHMEM_LANE_TRACE"); X.ltrace = e && atof(e) > 0; X.lslow = X.ltrace ? atof(e) : 0;       /* seconds: an exchange slower than this is described */
      X.t00 = now_s();
      e = getenv("XSHMEM_THP_DISABLE");   /* 1: prctl(PR_SET_THP_DISABLE) for this process: no THP faults, so no direct compaction stalls on a fragmented node */
      if (e && atoi(e) > 0) { int r = prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); if (xshmem_my_pe() == 0 || r) fprintf(stderr, "comm_xshmem: XSHMEM_THP_DISABLE=1: pe %d prctl rc %d\n", xshmem_my_pe(), r); }
      e = getenv("XSHMEM_LANE_POLL"); X.lpoll = e && atoi(e) > 0;
      e = getenv("XSHMEM_SIG_LANES"); X.sigl = e && atoi(e) > 0;
      e = getenv("XSHMEM_LANE_CTX"); X.lctx = e && !strcmp(e, "private") ? XSHMEM_CTX_PRIVATE : e && !strcmp(e, "none") ? 0 : XSHMEM_CTX_SERIALIZED; }
    { X.lmin = (size_t)256 << 10; }
    for (int l = 0; l < K; l++) {                                     /* lane contexts first, in order, before any heap (xshmem.h: lane order) */
        XSHMEM_SELECT(l); xpe *lp = &X.pe[l]; lp->lpe = l;
        if (xshmem_ctx_create(X.lctx, &lp->lanes[0])) ec_fatal(EC_RC_FATAL, "comm_xshmem: PE %d: no context for the lane\n", K * X.q + l);
        lp->lane = lp->lanes[0];
        for (int k = 1; k < nlw; k++) if (xshmem_ctx_create(X.lctx, &lp->lanes[k])) ec_fatal(EC_RC_FATAL, "comm_xshmem: PE %d: no context for lane %d (XSHMEM_LANES=%d: the backend serves fewer)\n", K * X.q + l, k, nlw);
    }
    size_t db = cfg.heap_mb > 0 ? (size_t)cfg.heap_mb << 20 : ((getenv("COMM_OFI_POOL_MB") ? (size_t)atol(getenv("COMM_OFI_POOL_MB")) : (getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192) / 4 + 256) << 20);
    if (getenv("XSHMEM_HEAP_SLACK_MB")) db += (size_t)atol(getenv("XSHMEM_HEAP_SLACK_MB")) << 20;   /* (default off: the Cray backend takes the control and alignment out of the same heap: the plan's figure is exact, a few MiB more are needed there) */
    db = round_up(db); X.dbytes = db;
    X.nw = vpe_on() ? K * X.nodes : X.nodes; X.maxid = vpe_on() ? MAXID_VPE : MAXID;
    size_t cb = (size_t)X.maxid * W_N * (size_t)X.nw * 8;
    for (int l = 0; l < K; l++) { XSHMEM_SELECT(l); X.pe[l].ctrl = (uint64_t *)xshmem_heap_alloc(cb, XSHMEM_SPACE_CTRL); }   /* one space at a time, PEs in order 0..K-1 (collective) */
    for (int l = 0; l < K; l++) {
        XSHMEM_SELECT(l); xpe *lp = &X.pe[l];
        lp->data = (char *)xshmem_heap_alloc(db, XSHMEM_SPACE_DATA); lp->dlen = db;
        pthread_mutex_init(&lp->alock, 0); pthread_mutex_init(&lp->qm, 0); pthread_cond_init(&lp->qcv, 0);
        lp->blocks = (struct xblk *)calloc(1, sizeof *lp->blocks); lp->blocks->off = ALIGN; lp->blocks->len = db - ALIGN;   /* offset 0 stays unused */
    }
    for (int l = 0; l < K; l++) { if (pthread_create(&X.pe[l].th, 0, worker, &X.pe[l])) ec_fatal(EC_RC_FATAL, "comm_xshmem: pthread_create\n"); X.pe[l].th_on = 1; }
    XSHMEM_SELECT(0);
    X.inited = 1;
    char rbuf[40]; if (X.rbytes) snprintf(rbuf, sizeof rbuf, "%zu MiB", X.rbytes >> 20); else snprintf(rbuf, sizeof rbuf, "off (whole exchange)");
    if (X.q == 0) fprintf(stderr, "comm_xshmem: %d node-processes x %d loopback PEs, DATA heap %zu MiB + CTRL %zu KiB per PE, order %s, worker thread %s, rounds %s, thread level %d\n",
                          X.nodes, K, db >> 20, cb >> 10, "rot", "on", rbuf, prov);
    pthread_mutex_unlock(&X.lock);
    return X.nodes;
}
static comm *make_comm(int n, int me, int id, int lpe, int dev, int pe_start, int pe_stride, int flat)
{
    if (__atomic_exchange_n(&X.used[lpe][id], 1, __ATOMIC_ACQ_REL)) ec_fatal(EC_RC_FATAL, "comm_xshmem: communicator id %d used twice\n", id);
    comm *c = (comm *)calloc(1, sizeof *c); xpriv *p = (xpriv *)calloc(1, sizeof *p);
    c->ops = &xsh_ops; c->priv = p; c->rank = me; c->size = n; c->inflight = 1;
    p->n = n; p->me = me; p->id = id; p->lpe = lpe; p->dev = dev; p->lp = &X.pe[lpe];
    p->pe = (int *)malloc((size_t)n * sizeof(int)); for (int r = 0; r < n; r++) p->pe[r] = flat ? pe_start + r : X.K * (pe_start + r * pe_stride) + lpe;
    p->scnt = (size_t *)calloc(6 * (size_t)n, sizeof(size_t)); p->rcnt = p->scnt + n; p->sdsp = p->scnt + 2 * n; p->rdsp = p->scnt + 3 * n; p->spre = p->scnt + 4 * n; p->rpre = p->scnt + 5 * n;
    p->oseq = (size_t *)calloc(2 * (size_t)n, sizeof(size_t)); p->iseq = p->oseq + n;
    p->posted = (char *)calloc(2 * (size_t)n, 1); p->rxdone = p->posted + n; p->tarr = (double *)calloc((size_t)n, sizeof(double));
    return c;
}
static int cur_lpe(int *dev)
{
    HIP_CHECK(hipGetDevice(dev));
    int lpe = X.K > 1 ? *dev : 0; if (lpe >= X.K) ec_fatal(EC_RC_FATAL, "comm_xshmem: device %d has no loopback PE (%d per process)\n", *dev, X.K);
    return lpe;
}
comm *comm_xshmem_create_at(int pe_start, int pe_stride, int n, int id)
{
    if (!X.inited) comm_xshmem_init();
    if (n < 1 || pe_stride < 1 || pe_start < 0 || pe_start + (n - 1) * pe_stride >= X.nodes || id < 0 || id >= X.maxid) die(0, "bad PE set");
    int me = -1; for (int r = 0; r < n; r++) if (pe_start + r * pe_stride == X.q) me = r;
    if (me < 0) die(0, "this PE is not in the set");
    int dev = 0, lpe = cur_lpe(&dev);
    return make_comm(n, me, id, lpe, dev, pe_start, pe_stride, 0);
}
/* S1a (PE layout): the communicator over the contiguous PE run [pe_start, pe_start + n) in PE units (PE = K * node + loopback PE), rank = PE - pe_start.
 * Created by the calling PE (device = loopback PE) if it is a member; local, like create_at.  Ids as create_at's (< X.maxid). */
comm *comm_xshmem_create_flat(int pe_start, int n, int id)
{
    if (!X.inited) comm_xshmem_init();
    if (!vpe_on()) die(0, "create_flat outside one process per GPU (ECALC_PE_PER_GPU=1)");
    if (n < 1 || pe_start < 0 || pe_start + n > X.K * X.nodes || id < 0 || id >= X.maxid) die(0, "bad PE run");
    int dev = 0, lpe = cur_lpe(&dev), my = X.K * X.q + lpe;
    if (my < pe_start || my >= pe_start + n) die(0, "this PE is not in the run");
    return make_comm(n, my - pe_start, id, lpe, dev, pe_start, 1, 1);
}
void comm_xshmem_select(int lpe) { if (X.inited && X.K == 1) return; XSHMEM_SELECT(lpe); }     /* the loopback-PE hook (vpe.h): the current loopback PE (K = 1: the only one) */
/* S2a (ECALC_PE_PER_GPU): this process's place as the backend's topology (pe_geo) reports it: v = { pe, npes, local, pes per node, gpu, numa, nics, nic[0] }, cpus = the NUMA node's cpu list.  0 = ok */
int comm_xshmem_geo(int *v, char *cpus, int ncpus)
{
    xshmem_topo_t tt; if (!X.inited || xshmem_topo(&tt)) return -1;
    const xshmem_topo_t *q = &tt;
    v[0] = q->pe; v[1] = q->npes; v[2] = q->local; v[3] = q->pes_per_node; v[4] = q->gpu; v[5] = q->numa; v[6] = q->nics; v[7] = q->nics ? q->nic[0] : -1;
    if (cpus && ncpus > 0) { strncpy(cpus, q->cpus, (size_t)ncpus - 1); cpus[ncpus - 1] = 0; }
    return 0;
}
int comm_xshmem_loopback_pes(void) { return X.inited ? X.K : XS_LPES(); }
void comm_xshmem_finalize(void)
{
    if (!X.inited) return;
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT); }   /* the last words */
    for (int l = 0; l < X.K; l++) if (X.pe[l].th_on) {
        pthread_mutex_lock(&X.pe[l].qm); X.pe[l].stop = 1; pthread_cond_signal(&X.pe[l].qcv); pthread_mutex_unlock(&X.pe[l].qm);
        if (X.pe[l].busy) { fprintf(stderr, "comm_xshmem: PE %d: finalize with %d exchange(s) still pending: worker not joined\n", X.K * X.q + l, X.pe[l].busy); pthread_detach(X.pe[l].th); }
        else pthread_join(X.pe[l].th, 0);
        X.pe[l].th_on = 0;
    }
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); lane_quiet(&X.pe[l], (1u << (X.nl > 1 ? X.nl : 1)) - 1); }
    XSHMEM_SELECT(0); xshmem_barrier_all();
    if (X.q != 0 && X.K == 1) fprintf(stderr, "comm_xshmem: pe %d: DATA heap peak %zu MiB of %zu\n", xshmem_my_pe(), X.pe[0].peak >> 20, X.dbytes >> 20);   /* every PE's own peak (the engine's summary below is PE 0's) */
    if (X.q == 0) {
        size_t pk = 0; for (int l = 0; l < X.K; l++) if (X.pe[l].peak > pk) pk = X.pe[l].peak;
        xsplit_print();
        if (X.ltrace) { fprintf(stderr, "comm_xshmem: node-process %d: lane quiets (n, max s, PE0 sum s):", X.q); for (int k = 0; k < X.nl; k++) fprintf(stderr, " lane %d: %ld %.3f %.2f;", k, X.lqn[k], X.lqmax[k], X.pe[0].lqt[k]); fprintf(stderr, "\n"); }
        if (X.peer) fprintf(stderr, "comm_xshmem: node-process %d: XSHMEM_PEER: %.2f GB put by device copy to same-node PEs (this process)\n", X.q, X.pbytes / 1e9);
        if (X.nl > 1) { fprintf(stderr, "comm_xshmem: node-process %d: lanes %d (chunk >= %zu KiB), payload GB put per lane (this process):", X.q, X.nl, X.lmin >> 10); for (int k = 0; k < X.nl; k++) fprintf(stderr, " %.2f", X.lb[k] / 1e9); fprintf(stderr, "\n"); }
        fprintf(stderr, "comm_xshmem: node-process %d: %ld exchanges, %.2f s post to completion (summed over the APU threads), %.2f GB received = %.2f GB/s per APU thread; DATA heap peak %zu MiB of %zu (%s); largest exchange sequence %llu of %llu; %ld exchanges in rounds (%ld rounds)\n",
                X.q, X.nev, X.tv, X.bv / 1e9, X.tv > 0 ? X.bv / 1e9 / X.tv : 0.0, pk >> 20, X.dbytes >> 20, X.K == 1 ? "PE 0" : "max of its PEs", (unsigned long long)X.maxseq, (unsigned long long)SEQ_LIMIT, X.nrex, X.nrounds);
    }
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(X.pe[l].ctrl); }
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(X.pe[l].data); }
    XSHMEM_SELECT(0); xshmem_finalize();
    X.inited = 0;
}
