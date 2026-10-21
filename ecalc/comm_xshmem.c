/* comm_xshmem.c -- the exchanges of ecalc on xshmem.h (plan TARGET1 stage S3a; design TARGET12_DESIGN rev1 sections 3, 5, 6).
 *
 * COMM_TRANSPORT=xshmem (make XSHMEM=ofi; compiled with -DXSHMEM_ENGINE, never part of the default build).  This file is the target-shaped
 * engine: it speaks ONLY xshmem.h (no fi_*, no shmem_*), so on the target the same file runs on Cray SHMEM 12.1 behind xshmem_cray.h.
 * It is the exchange prototype of tests/t_xshmem.c (xex_post / xex_progress / xex_rx / xex_wait) moved behind struct comm_ops.
 *
 * Model.  One node-process hosts K loopback PEs (xshmem_ofi_loopback_pes(); K = 1 on a backend without loopback): PE = K * node + d, the
 * PE d owns GPU d, its NIC, its DATA heap (payload, sub-allocated here) and its CTRL heap (the control words).  A mesh communicator
 * (mn.c: node set {pe_start + pe_stride r}, one per APU thread d) is the PE set {K (pe_start + pe_stride r) + d}: each NIC carries an
 * independent one-PE-per-node all-to-all.  comm_create_at takes NODE indices, as comm_shmem_create_at does.
 *
 * Protocol (one writer per word, plain 8 B SET writes, no atomics; seq << 40 | v, seq < 2^22 per communicator, fatal beyond):
 *   post  : stage non-heap buffers into the PE's DATA heap, copy the self slab, stream sync, quiet ctx0 (older words), publish
 *           ROFF[me]@peer = (seq, offset of the slab's landing place in my heap) to every peer, hand the exchange to the PE's worker
 *   worker: (ECALC_XEX_THREAD=1, default; 0 = poll-only, the APU thread does it in post and wait) per peer whose ROFF arrived putmem_nbi the
 *           slab to the offset; when every peer is posted ONE quiet of the lane, then SIG[me]@peer = (seq, bytes) per peer, a quiet, flushed = 1
 *   wait  : the APU thread tests SIG[peer] (+ progress) of every sender, checks seq and count (fatal on a mismatch, the texts of comm_shmem),
 *           waits for flushed, copies staged receive slabs out on the stream and syncs.  One exchange per communicator in flight.
 * Peers are walked in rotated order (me + q) % n (XSHMEM_ORDER=rot, default: no incast on member 0; seq = rank order, the reference's).
 * Small collectives (barrier, allreduce_max, allgather_host) are word protocols VAL[2][n] + VSEQ[2][n] on ctx0 (parity slots as comm_shmem's
 * tiny_exchange).  send / recv / allreduce_modq / alltoallv_host / allgather (device) are 0: comm_util.c's fallbacks apply.
 *
 * CTRL layout (words, per communicator id < MAXID, a pure function of the id, so no exchange at create):
 *   ctrl + id * 7 * nw : ROFF[n] SIG[n] VAL0[n] VAL1[n] VSEQ0[n] VSEQ1[n] SOFF[n]   (n = nw: the largest member count = the nodes; with ECALC_VPE
 *   (S1a) the PEs, K * nodes, and only XS_MAXID_VPE ids)
 * Threads: mesh d is driven by APU thread d (device d current at create; the main thread is thread 0).  Every entry selects the loopback
 * PE of the comm (XSHMEM_SELECT); a debug flag catches two threads inside one comm. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "comm.h"
#include "xshmem.h"
#include "vpe.h"
#ifdef XSHMEM_OFI
#define XS_LPES() xshmem_ofi_loopback_pes()
#else
#define XS_LPES() 1
#endif

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define COPY(d, s, n, st) HIP_CHECK(comm_copy_async(d, s, n, st))
#define SYNC(st) do { int ph_ = ec_ph(EC_PH_HIP_SYNC); HIP_CHECK(hipStreamSynchronize(st)); ec_ph(ph_); } while (0)
#define MAXID 1024                         /* communicator ids (control blocks) */
#define MAXID_VPE 512                      /* ... with ECALC_VPE (flat PE communicators: the control block grows with the PE count) */
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
    int lpe; xshmem_ctx_t lane;
    char *data; size_t dlen; uint64_t *ctrl;
    pthread_mutex_t alock; struct xblk *blocks; size_t cur, peak, symcur, sympeak;
    pthread_t th; int th_on, stop; volatile int busy; pthread_mutex_t qm; pthread_cond_t qcv; struct xpriv *queue[MAXPEND]; volatile int nq;
} xpe;
static struct {
    int yield, thr_set; double sp[8]; long nsp;               /* summed split of the whole-exchange path, see xex_wait */
    int inited, thr, rot, K, nodes, nw, maxid, q; long nev; double tv; double bv; size_t rbytes; long nrex, nrounds;
    pthread_mutex_t lock;                  /* init and the counters */
    uint64_t maxseq; size_t dbytes;
    xpe pe[MAXLPE]; unsigned char used[MAXLPE][MAXID];
} X = { 1, 0, {0}, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.0, 0.0, 0, 0, 0, PTHREAD_MUTEX_INITIALIZER, 0, 0, {}, {} };

#define YIELD() do { if (X.yield) sched_yield(); } while (0)
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
static int env_int(const char *name, int def) { const char *e = getenv(name); return e ? atoi(e) : def; }
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
    uint64_t seq, tseq; volatile int inuse;
    /* the pending exchange */
    int pending, v, rin, sin; size_t bytes, sst, rst; double t0, t_pe, t_w, t_a, t_p, t_f, t_w0, t_rx;   /* t_*: the exchange's split (post end, worker first visit, all peers posted, payload quiet, signals quiet, wait entry, receive done) */
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
#define ORD(p, q) (X.rot ? ((p)->me + (q)) % (p)->n : (q))
static void die(const xpriv *p, const char *m) { ec_fatal(EC_RC_FATAL, "comm_xshmem: pe %d: %s\n", X.K * X.q + (p ? p->lpe : 0), m); }
#define WORD(p, w, i) ((p)->lp->ctrl + ((size_t)(p)->id * W_N + (w)) * (size_t)X.nw + (i))
static void enter(xpriv *p) { if (__atomic_exchange_n(&p->inuse, 1, __ATOMIC_ACQUIRE)) die(p, "two threads inside one communicator"); XSHMEM_SELECT(p->lpe); }
static void leave(xpriv *p) { __atomic_store_n(&p->inuse, 0, __ATOMIC_RELEASE); }

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
        if (p->scnt[i]) { xshmem_ctx_putmem_nbi(p->lp->lane, xshmem_at((ptrdiff_t)(v & VMASK)), p->sbase + p->soff[i], p->scnt[i], p->pe[i]); p->touched = 1; }
        p->posted[i] = 1; p->nposted++;
    }
    if (p->nposted < n) return 0;
    p->t_a = now_s();
    if (p->touched) xshmem_ctx_quiet(p->lp->lane);                                   /* the batched flush: every payload visible */
    p->t_p = now_s();
    for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != p->me) xshmem_ctx_signal_set(p->lp->lane, WORD(p, W_SIG, p->me), PACK(seq, p->scnt[i]), p->pe[i]); }
    xshmem_ctx_quiet(p->lp->lane);                                                   /* the signals are out too: sb reusable */
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
            if (++idle > 32) YIELD();
            if (!(idle & 4095)) { double t = now_s(); for (int i = 0; i < na; i++) if (t - act[i]->t0 > 900) xex_hang(act[i]); }   /* a pending exchange nobody waits for hangs too */
        }
    }
}

/* ---- the exchange in rounds (XSHMEM_ROUND_MB = m > 0; the design of comm_shmem.c's rounder, as a non-blocking state machine run by the PE's worker) ----
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
                if (len) { xshmem_ctx_putmem_nbi(lp->lane, xshmem_at((ptrdiff_t)(v & VMASK)), p->sin ? p->vsb + p->sdsp[i] + o : lp->data + p->sst + p->spre[i], len, p->pe[i]); p->touched = 1; }
                p->posted[i] = 1; p->rdone++;
            }
            if (p->rdone < p->rneed) return 0;
            if (p->touched) xshmem_ctx_quiet(lp->lane);
            for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != me && j < rcount(p->scnt[i], c))
                xshmem_ctx_signal_set(lp->lane, WORD(p, W_SIG, me), PACK(p->oseq[i] + 1 + j, rlen(p->scnt[i], c, j)), p->pe[i]); }
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
            xshmem_ctx_quiet(lp->lane);
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
    SYNC(s);                                              /* the stream is done with sb and rb: publish only after */
    xshmem_ctx_quiet(XSHMEM_CTX_DEFAULT);                 /* older word writes (ROFF, collectives) */
    p->t_w = p->t_a = p->t_p = p->t_f = p->t_rx = 0;
    memset(p->posted, 0, (size_t)n); memset(p->rxdone, 0, (size_t)n); p->posted[me] = 1; p->nposted = 1; p->touched = 0; p->flushed = 0;
    for (int q = 0; q < n; q++) { int i = ORD(p, q); if (i != me)
        xshmem_ctx_signal_set(XSHMEM_CTX_DEFAULT, WORD(p, W_ROFF, me), PACK(p->seq, xshmem_off(p->rbase + (p->rcnt[i] ? p->roff[i] : 0))), p->pe[i]); }
    if (n == 1) { p->flushed = 1; return; }
    if (X.thr) {
        pthread_mutex_lock(&lp->qm);
        if (lp->nq >= MAXPEND) { pthread_mutex_unlock(&lp->qm); die(p, "too many pending exchanges on one PE"); }
        lp->queue[lp->nq++] = p; pthread_cond_signal(&lp->qcv);
        pthread_mutex_unlock(&lp->qm);
    } else if (xex_send(p)) p->flushed = 1;
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
        if (!X.thr && !p->flushed && xex_send(p)) p->flushed = 1;
        if (!rx) { rx = xex_rx(p); if (rx) p->t_rx = now_s(); }
        if (rx && __atomic_load_n(&p->flushed, __ATOMIC_ACQUIRE)) break;
        if (!(++spins & 4095)) { if (now_s() - t0 > 900) xex_hang(p); if (spins > 100000) YIELD(); }
    }
    if (!p->rin && !rdone) {
        for (int r = 0; r < n; r++) if (r != me && p->rcnt[r]) COPY((char *)p->rb + p->rdsp[r], p->rbase + p->rpre[r], p->rcnt[r], p->st);
        SYNC(p->st);
    }
    if (p->sst) xfree(p->lp, p->sst);
    if (p->rst) xfree(p->lp, p->rst);
    p->sst = p->rst = 0; p->pending = 0;
    double t = now_s() - p->t0, b = recv_bytes(p);
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
    xpriv *p = PRIV(c); p->phk = comm_xtag < 0 || comm_xtag >= XK_NK ? 0 : comm_xtag; int ph_ = ec_ph(EC_PH_POST(p->phk));
    enter(p);
    for (int r = 0; r < p->n; r++) { p->scnt[r] = p->rcnt[r] = bytes; p->sdsp[r] = p->rdsp[r] = (size_t)r * bytes; }
    p->v = 0; xex_post(p, sb, rb, s);
    leave(p); ec_ph(ph_);
}
static void x_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    xpriv *p = PRIV(c); p->phk = comm_xtag < 0 || comm_xtag >= XK_NK ? 0 : comm_xtag; int ph_ = ec_ph(EC_PH_POST(p->phk));
    enter(p);
    if (p->pending) die(p, "alltoallv while an exchange is pending");   /* (before the caller's arrays overwrite the pending counts) */
    memcpy(p->scnt, scnt, (size_t)p->n * sizeof(size_t)); memcpy(p->sdsp, sdsp, (size_t)p->n * sizeof(size_t));
    memcpy(p->rcnt, rcnt, (size_t)p->n * sizeof(size_t)); memcpy(p->rdsp, rdsp, (size_t)p->n * sizeof(size_t));
    p->v = 1; xex_post(p, sb, rb, s);
    leave(p); ec_ph(ph_);
}
static void x_wait(comm *c)
{
    xpriv *p = PRIV(c); if (!p->pending) return;
    int ph_ = ec_ph(EC_PH_WAIT(p->phk));
    enter(p); xex_wait(p); leave(p); ec_ph(ph_);
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
    xpriv *p = PRIV(c); int ph_ = ec_ph(EC_PH_SYM_ALLOC);
    void *r = p->lp->data + xalloc(p->lp, bytes, 1); ec_ph(ph_); return r;
}
static void x_sym_free(comm *c, void *ptr)
{
    (void)c; int ph_ = ec_ph(EC_PH_SYM_FREE);
    for (int l = 0; l < X.K; l++) if (in_heap(&X.pe[l], ptr)) { xfree(&X.pe[l], (size_t)((char *)ptr - X.pe[l].data)); ec_ph(ph_); return; }
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
    const char *eo = getenv("XSHMEM_ORDER");
    if (eo && strcmp(eo, "seq") && strcmp(eo, "rot")) ec_fatal(EC_RC_FATAL, "comm_xshmem: XSHMEM_ORDER=%s: seq or rot\n", eo);
    X.rot = !(eo && !strcmp(eo, "seq")); { const char *et_ = getenv("ECALC_XEX_THREAD"); X.thr = et_ ? atoi(et_) != 0 : !vpe_on(); X.thr_set = et_ != 0; }   /* S1b: vPE polls by default.  Measured 9q6g/9q6h (1 n, 4 PEs): with the worker 35 ms per exchange (quiets ~20 ms each), polling 3 ms; the worker still costs 2.4-2.8 s per product after it was given its PE's device, so the cause is the worker design under 4 PE threads in one process, not the device */ { xshmem_cfg_t cf_; xshmem_cfg_load(&cf_); X.yield = !cf_.noyield; }
    { const char *er = getenv("XSHMEM_ROUND_MB"); double m = er ? atof(er) : 0; X.rbytes = m > 0 ? (size_t)(m * 1048576.0) : 0;
      if (X.rbytes && !X.thr && !X.thr_set) X.thr = 1;   /* vPE with an explicit XSHMEM_ROUND_MB: the rounds run in the worker */
      if (X.rbytes && !X.thr) ec_fatal(EC_RC_FATAL, "comm_xshmem: XSHMEM_ROUND_MB needs the worker thread (ECALC_XEX_THREAD=1): the rounds run in it\n"); }
    int prov = 0; if (xshmem_init_thread(XSHMEM_THREAD_MULTIPLE, &prov)) ec_fatal(EC_RC_FATAL, "comm_xshmem: xshmem_init_thread failed\n");
    int K = XS_LPES(); if (K < 1 || K > MAXLPE) ec_fatal(EC_RC_FATAL, "comm_xshmem: %d loopback PEs per process (1..%d; XSHMEM_LOOPBACK_PES)\n", K, MAXLPE);
    X.K = K; XSHMEM_SELECT(0);
    int np = xshmem_n_pes(); if (np % K) ec_fatal(EC_RC_FATAL, "comm_xshmem: %d PEs are not whole processes of %d loopback PEs\n", np, K);
    X.nodes = np / K; X.q = xshmem_my_pe() / K;
    for (int l = 0; l < K; l++) {                                     /* lane contexts first, in order, before any heap (xshmem.h: lane order) */
        XSHMEM_SELECT(l); xpe *lp = &X.pe[l]; lp->lpe = l;
        if (xshmem_ctx_create(XSHMEM_CTX_SERIALIZED, &lp->lane)) ec_fatal(EC_RC_FATAL, "comm_xshmem: PE %d: no context for the lane\n", K * X.q + l);
    }
    size_t db = cfg.heap_mb > 0 ? (size_t)cfg.heap_mb << 20 : ((getenv("COMM_OFI_POOL_MB") ? (size_t)atol(getenv("COMM_OFI_POOL_MB")) : (getenv("COMM_SHMEM_POOL_MB") ? (size_t)atol(getenv("COMM_SHMEM_POOL_MB")) : 8192) / 4 + 256) << 20);
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
    if (X.thr) for (int l = 0; l < K; l++) { if (pthread_create(&X.pe[l].th, 0, worker, &X.pe[l])) ec_fatal(EC_RC_FATAL, "comm_xshmem: pthread_create\n"); X.pe[l].th_on = 1; }
    XSHMEM_SELECT(0);
    X.inited = 1;
    if (X.q == 0) fprintf(stderr, "comm_xshmem: %d node-processes x %d loopback PEs, DATA heap %zu MiB + CTRL %zu KiB per PE, order %s, worker thread %s, rounds %s, thread level %d\n",
                          X.nodes, K, db >> 20, cb >> 10, X.rot ? "rot" : "seq", X.thr ? "on" : "off (poll-only)", X.rbytes ? "XSHMEM_ROUND_MB on" : "off (whole exchange)", prov);
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
    p->posted = (char *)calloc(2 * (size_t)n, 1); p->rxdone = p->posted + n;
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
/* S1a (ECALC_VPE): the communicator over the contiguous PE run [pe_start, pe_start + n) in PE units (PE = K * node + loopback PE), rank = PE - pe_start.
 * Created by the vPE thread of the calling device (device = loopback PE) that is a member; local, like create_at.  Ids as create_at's (< X.maxid). */
comm *comm_xshmem_create_flat(int pe_start, int n, int id)
{
    if (!X.inited) comm_xshmem_init();
    if (!vpe_on()) die(0, "create_flat without ECALC_VPE");
    if (n < 1 || pe_start < 0 || pe_start + n > X.K * X.nodes || id < 0 || id >= X.maxid) die(0, "bad PE run");
    int dev = 0, lpe = cur_lpe(&dev), my = X.K * X.q + lpe;
    if (my < pe_start || my >= pe_start + n) die(0, "this PE is not in the run");
    return make_comm(n, my - pe_start, id, lpe, dev, pe_start, 1, 1);
}
void comm_xshmem_select(int lpe) { XSHMEM_SELECT(lpe); }     /* the vPE thread hook (vpe.h): this thread's current loopback PE */
int comm_xshmem_loopback_pes(void) { return X.inited ? X.K : XS_LPES(); }
int comm_xshmem_nodes(void) { return X.nodes; }
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
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_ctx_quiet(X.pe[l].lane); }
    XSHMEM_SELECT(0); xshmem_barrier_all();
    if (X.q == 0) {
        size_t pk = 0; for (int l = 0; l < X.K; l++) if (X.pe[l].peak > pk) pk = X.pe[l].peak;
        xsplit_print();
        fprintf(stderr, "comm_xshmem: node-process %d: %ld exchanges, %.2f s post to completion (summed over the APU threads), %.2f GB received = %.2f GB/s per APU thread; DATA heap peak %zu MiB of %zu; largest exchange sequence %llu of %llu; %ld exchanges in rounds (%ld rounds)\n",
                X.q, X.nev, X.tv, X.bv / 1e9, X.tv > 0 ? X.bv / 1e9 / X.tv : 0.0, pk >> 20, X.dbytes >> 20, (unsigned long long)X.maxseq, (unsigned long long)SEQ_LIMIT, X.nrex, X.nrounds);
    }
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(X.pe[l].ctrl); }
    for (int l = 0; l < X.K; l++) { XSHMEM_SELECT(l); xshmem_heap_free(X.pe[l].data); }
    XSHMEM_SELECT(0); xshmem_finalize();
    X.inited = 0;
}
