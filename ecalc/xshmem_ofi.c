/* xshmem_ofi.c -- libfabric (cxi) backend of xshmem.h (plan TARGET1, design TARGET12_DESIGN rev1; brief B2).  Built only with make XSHMEM=ofi.
 *
 * What this is: the aac7 stand-in for Cray SHMEM 12.1.  One PE per process (boot: xshmem_boot.h PMI2, topology: pe_geo.h), one libfabric
 * domain per NIC in use, per domain one AV; per PE 1 default endpoint + N lane endpoints (+2 spares for xshmem_ctx_create), every endpoint with its
 * own CQ (selective completion, only errors surface) and write counter.  Two heaps (DATA, CTRL), each one collective allocation (device, fine-grained,
 * or CTRL in pinned host memory with XSHMEM_CTRL=host), each registered once per domain (FI_HMEM_ROCR / SYSTEM).
 * Copied from comm_ofi.c (not edited there): the NIC choice (pe_geo does it), getinfo hints, fi_mr_regattr of fine-grained memory, the lock discipline.
 * Facts copied from libsma 11.8 (RB 8): mr_mode ALLOCATED|ENDPOINT(+HMEM) with client keys and offset addressing (XSHMEM_MRMODE=libsma, the
 * default; the mode actually in force is the one the provider returns, logged once: VIRT_ADDR / PROV_KEY in it switch to base+key addressing, the
 * heap blob that is all-gathered at xshmem_heap_alloc always carries base and keys), FI_INJECT for n <= inject_size, one writemsg per put (cut only at
 * max_msg_size), counter-based completion polled with a deadline, quiet = counter wait.
 * Signal modes: dc (default, implemented): payload FI_DELIVERY_COMPLETE, quiet = counter wait, signal_set = 8 B INJECT|DELIVERY_COMPLETE write.
 *   flush / waw: NOT YET (fatal at init; unproven designs, TARGET12_DESIGN 1.4).
 * One mutex PER PE (pe_t.lock) around every fi_* call of that PE (FI_THREAD_DOMAIN); waits spin with test + yield OUTSIDE the lock, so the engine's worker
 * thread and its caller can both be inside xshmem calls of one PE.
 * Loopback PEs (XSHMEM_LOOPBACK_PES = K, default 1 = the shape above, bit for bit): one process hosts K PEs, PE = K*rank + l, each with its own GPU, NIC,
 * domains, endpoints, heaps and mutex (pe_t below).  The PE of a context call is the context's owner; calls without a context use the
 * calling thread's PE (xshmem_ofi_loopback_select).  Boot is per process: one allgather of K address blobs, one of K heap blobs (heap_alloc is called
 * by the K local PEs in order 0..K-1 from one thread); barrier_all and finalize are once per process.
 * xshmem_ptr (XSHMEM_PEER=1 or XSHMEM_IPC=1, one PE per process): the DATA heaps of the PEs of the same node are exported with hipIpcGetMemHandle (in the
 * heap blob), opened after the allgather, and xshmem_ptr(p, pe) returns the peer's byte at p's offset (NULL for another node's PE, the CTRL heap, or when the
 * export or the open failed: the engine then uses the NIC).  Heap release closes the mappings. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <hip/hip_runtime.h>
#include "xshmem.h"
#include "xshmem_boot.h"
#include "pe_geo.h"

/* xshmem_ofi_priv.h -- private types of the libfabric backend xshmem_ofi.c (built only with make XSHMEM=ofi).
 * One pe_t per PE the process hosts: K = XSHMEM_LOOPBACK_PES (default 1 = one PE per process, the original shape).  Each pe_t owns its NIC
 * domains, endpoints, heaps and its own mutex; lanes point back to their pe_t, so every context call finds its PE from the context. */
#include <stdint.h>
#include <pthread.h>
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include "xshmem.h"
#include "pe_geo.h"

#define MAXD   16          /* NIC slots (domains) per PE */
#define MAXEP  16          /* endpoints per PE: default + lanes + spares */
#define MAXADDR 16         /* bytes of one endpoint address (cxi: 8); small so that the blob is one PMI2 KVS value per PE */
#define MAXLPE 8           /* loopback PEs per process */
#define HALIGN ((size_t)2 << 20)
#define SPARE  2           /* contexts beyond the N lanes that xshmem_ctx_create can still hand out */

struct pe_s;
typedef struct {
    int cxi, open;
    struct fi_info *info; struct fid_fabric *fab; struct fid_domain *dom; struct fid_av *av;
    int first_ep;
} dom_t;
typedef struct {
    int e, slot, used;
    struct pe_s *pe;                           /* owner PE (loopback: the context call derives its PE from here) */
    struct fid_ep *ep; struct fid_cq *cq; struct fid_cntr *cn;
    struct fid_mr *mr[2]; void *desc[2]; uint64_t key[2];   /* one MR per endpoint and heap: cxi binds an MR to one endpoint only (FI_MR_ENDPOINT) */
    uint64_t issued;                           /* posted operations (under pe lock) */
    fi_addr_t *fa;                             /* [pe] */
    unsigned char *touch; int *tlist; int nt;  /* flush mode: target PEs written since the last quiet */
} lane_t;
typedef struct { uint32_t nics, nep; struct { uint32_t alen; char addr[MAXADDR]; } ep[MAXEP]; } ablob_t;
typedef struct { uint64_t base, len; uint64_t key[MAXEP]; uint32_t has_ipc, pad; hipIpcMemHandle_t ipc; } hblob_t;   /* ipc: XSHMEM_PEER, DATA heap only */

typedef struct pe_s {
    int lpe;                                   /* index among the PEs of this process */
    int mype, npes, nnodes, nics, nep, vaddr, pkey, sig_mode, log_sp, log_key;
    xshmem_cfg_t cfg; pe_topo topo; double quiet_s;
    size_t inject, maxmsg;
    pthread_mutex_t lock;
    dom_t dom[MAXD]; lane_t lane[MAXEP];
    char *hbase[2]; size_t hlen[2]; int hhost[2]; size_t flush_off;   /* flush mode: 8 B flush slot (and a source word at +64) in the DATA heap tail beyond hlen */
    hblob_t *peer[2];                          /* [space][pe] */
    char **pmap;                               /* XSHMEM_PEER: [pe] the node-local peer's DATA heap mapped here by hipIpc (NULL: not mapped, use the NIC) */
    xshmem_ofi_cnt_t cnt;
} pe_t;

/* One pe_t per hosted PE (K = XSHMEM_LOOPBACK_PES, default 1); `cur_l` selects the PE for calls that carry no context (per thread; the engine
 * calls xshmem_ofi_loopback_select).  W is the process level: boot rank/size and the collective heap bookkeeping. */
static pe_t G[MAXLPE];
static __thread int cur_l;
static struct { int init, K, q, P; int nalloc[2], nfree[2]; hblob_t *mine[2]; int dev0; } W;
static inline pe_t *cur_pe(void) { return &G[cur_l]; }

static void xfatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void xfatal(const char *fmt, ...)
{
    pe_t *g = cur_pe(); char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fprintf(stderr, "xshmem_ofi: pe %d: %s\n", g->mype, b); fflush(stderr); boot_abort(b); abort();
}
#define CK(x) do { int r_ = (int)(x); if (r_) xfatal("%s = %d (%s) at line %d", #x, r_, fi_strerror(-r_), __LINE__); } while (0)
static int g_yield = 1;
#define YIELD() do { if (g_yield) sched_yield(); } while (0)
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec; }

/* ---- completion bookkeeping (under g->lock) ---- */
static void err_fatal(lane_t *L)
{
    struct fi_cq_err_entry e; memset(&e, 0, sizeof e);
    ssize_t r = fi_cq_readerr(L->cq, &e, 0);
    xfatal("lane %d: completion error (cq_readerr %zd): %s (prov %d: %s)", L->e, r, fi_strerror(e.err), e.prov_errno, fi_cq_strerror(L->cq, e.prov_errno, e.err_data, NULL, 0));
}
static void drain_locked(lane_t *L)
{
    struct fi_cq_entry ce[8];
    for (;;) {
        ssize_t m = fi_cq_read(L->cq, ce, 8);
        if (m == -FI_EAGAIN) break;
        if (m < 0) err_fatal(L);
        if (m < 8) break;
    }
    if (fi_cntr_readerr(L->cn)) err_fatal(L);
}
static lane_t *lane_of(xshmem_ctx_t c) { return c ? (lane_t *)c : &cur_pe()->lane[0]; }

/* ---- heap addressing ---- */
static int locate(const pe_t *g, const void *p, size_t *off)
{
    for (int s = 0; s < 2; s++)
        if (g->hbase[s] && (const char *)p >= g->hbase[s] && (const char *)p < g->hbase[s] + g->hlen[s]) { *off = (size_t)((const char *)p - g->hbase[s]); return s; }
    return -1;
}
ptrdiff_t xshmem_off(const void *p)
{
    pe_t *g = cur_pe(); size_t o; int s = locate(g, p, &o);
    if (s < 0) xfatal("xshmem_off: %p lies in no heap", p);
    return (ptrdiff_t)o | (s ? XSHMEM_OFF_CTRL : 0);
}
void *xshmem_at(ptrdiff_t off)
{
    pe_t *g = cur_pe(); int s = (off & XSHMEM_OFF_CTRL) != 0; size_t o = (size_t)(off & ~XSHMEM_OFF_CTRL);
    if (!g->hbase[s] || o >= g->hlen[s]) xfatal("xshmem_at: offset %#zx outside heap %d", (size_t)off, s);
    return g->hbase[s] + o;
}

/* ---- posting ---- */
static void post1(lane_t *L, int pe, int space, size_t off, const void *src, size_t n, void *desc, uint64_t flags)
{
    pe_t *g = L->pe; int s = L->slot; dom_t *D = &g->dom[s];
    const hblob_t *pb = &g->peer[space][pe];
    struct iovec iov = { (void *)src, n }; void *dsc = desc;
    struct fi_rma_iov rio = { (g->vaddr ? pb->base : 0) + off, n, pb->key[L->e] };
    struct fi_msg_rma m; memset(&m, 0, sizeof m);
    m.msg_iov = &iov; m.desc = desc ? &dsc : NULL; m.iov_count = 1; m.addr = L->fa[pe]; m.rma_iov = &rio; m.rma_iov_count = 1;
    if (n <= g->inject) flags |= FI_INJECT;
    (void)D;
    pthread_mutex_lock(&g->lock);
    for (unsigned spins = 0;; spins++) {
        ssize_t rc = fi_writemsg(L->ep, &m, flags);
        if (!rc) break;
        if (rc == -FI_EAGAIN) {
            drain_locked(L);
            if (spins > 64) { pthread_mutex_unlock(&g->lock); YIELD(); pthread_mutex_lock(&g->lock); }
            continue;
        }
        xfatal("fi_writemsg of %zu B to pe %d (space %d off %#zx): %zd (%s)", n, pe, space, off, rc, fi_strerror((int)-rc));
    }
    L->issued++; g->cnt.writes++;
    pthread_mutex_unlock(&g->lock);
}
static uint64_t payload_flags(const pe_t *g) { return g->cfg.sig == XSHMEM_SIG_FLUSH ? FI_TRANSMIT_COMPLETE : FI_DELIVERY_COMPLETE; }   /* dc: delivery-complete payload; flush: transmit-complete (libsma 11.8 default) */

void xshmem_ctx_putmem_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, int pe)
{
    lane_t *L = lane_of(ctx); pe_t *g = L->pe; size_t doff, soff;
    if (!n) return;
    int ds = locate(g, dest, &doff), ss = locate(g, src, &soff);
    if (ds < 0) xfatal("putmem_nbi: dest %p lies in no heap", dest);
    if (ss < 0 || soff + n > g->hlen[ss]) xfatal("putmem_nbi: source %p (+%zu) lies outside the heaps (caps.src_any = 0: the engine stages)", src, n);
    if (doff + n > g->hlen[ds]) xfatal("putmem_nbi: %zu B at dest offset %#zx exceeds heap %d (%zu B)", n, doff, ds, g->hlen[ds]);
    if (pe < 0 || pe >= g->npes) xfatal("putmem_nbi: pe %d", pe);
    void *desc = L->desc[ss];
    for (size_t o = 0; o < n; o += g->maxmsg) {
        size_t len = n - o < g->maxmsg ? n - o : g->maxmsg;
        post1(L, pe, ds, doff + o, (const char *)src + o, len, desc, payload_flags(g));
    }
    if (g->cfg.sig == XSHMEM_SIG_FLUSH) { pthread_mutex_lock(&g->lock); if (!L->touch[pe]) { L->touch[pe] = 1; L->tlist[L->nt++] = pe; } pthread_mutex_unlock(&g->lock); }
}
void xshmem_ctx_signal_set(xshmem_ctx_t ctx, uint64_t *sig, uint64_t val, int pe)
{
    lane_t *L = lane_of(ctx); pe_t *g = L->pe; size_t o; int s = locate(g, sig, &o);
    if (s < 0) xfatal("signal_set: %p lies in no heap", (void *)sig);
    if (pe < 0 || pe >= g->npes) xfatal("signal_set: pe %d", pe);
    uint64_t v = val;                          /* INJECT copies it at post time: a stack word, no descriptor (as libsma) */
    post1(L, pe, s, o, &v, 8, NULL, FI_DELIVERY_COMPLETE);
}

/* ---- completion: quiet ---- */
static void wait_cntr(lane_t *L, uint64_t target, const char *what)
{
    pe_t *g = L->pe;
    double t0 = now_s(); unsigned spins = 0;
    for (;;) {
        pthread_mutex_lock(&g->lock);
        uint64_t done = fi_cntr_read(L->cn);
        drain_locked(L);
        pthread_mutex_unlock(&g->lock);
        if (done >= target) return;
        __atomic_add_fetch(&g->cnt.wait_spins, 1, __ATOMIC_RELAXED);
        if (++spins > 200) { YIELD(); if (now_s() - t0 > g->quiet_s) xfatal("quiet (%s) on lane %d: %llu of %llu operations complete after %.0f s (deadline)", what, L->e, (unsigned long long)done, (unsigned long long)target, g->quiet_s); }
    }
}
/* flush mode, as libsma 11.8 measured (RB 8.4): per touched target one 8 B fi_writemsg FI_MORE|FI_DELIVERY_COMPLETE to the target's flush slot, then a 0 B fi_inject_write
 * that closes the chain; the delivery-complete write drains every earlier write of this endpoint to that target.  Two counter units per target. */
static void flush_targets(lane_t *L)
{
    pe_t *g = L->pe;
    int s = L->slot; size_t off = g->flush_off;
    pthread_mutex_lock(&g->lock);
    int nt = L->nt;
    for (int k = 0; k < nt; k++) {
        int pe = L->tlist[k]; L->touch[pe] = 0;
        const hblob_t *pb = &g->peer[0][pe];
        uint64_t raddr = (g->vaddr ? pb->base : 0) + off, key = pb->key[L->e];
        const char *srcw = g->hbase[0] + off + 64; struct iovec iov = { (void *)srcw, 8 }; void *dsc = L->desc[0];
        struct fi_rma_iov rio = { raddr, 8, key };
        struct fi_msg_rma m; memset(&m, 0, sizeof m);
        m.msg_iov = &iov; m.desc = &dsc; m.iov_count = 1; m.addr = L->fa[pe]; m.rma_iov = &rio; m.rma_iov_count = 1;
        for (unsigned spins = 0;; spins++) {
            ssize_t rc = fi_writemsg(L->ep, &m, FI_MORE | FI_DELIVERY_COMPLETE | (8 <= g->inject ? FI_INJECT : 0));
            if (!rc) break;
            if (rc != -FI_EAGAIN) xfatal("flush fi_writemsg to pe %d: %zd (%s)", pe, rc, fi_strerror((int)-rc));
            drain_locked(L); if (spins > 64) { pthread_mutex_unlock(&g->lock); YIELD(); pthread_mutex_lock(&g->lock); }
        }
        L->issued++;
        for (unsigned spins = 0;; spins++) {
            ssize_t rc = fi_inject_write(L->ep, srcw, 0, L->fa[pe], raddr, key);
            if (!rc) break;
            if (rc != -FI_EAGAIN) xfatal("flush fi_inject_write to pe %d: %zd (%s)", pe, rc, fi_strerror((int)-rc));
            drain_locked(L); if (spins > 64) { pthread_mutex_unlock(&g->lock); YIELD(); pthread_mutex_lock(&g->lock); }
        }
        L->issued++; g->cnt.flush_chains++;
    }
    L->nt = 0;
    pthread_mutex_unlock(&g->lock);
    (void)s;
}
static void quiet_lane(lane_t *L)
{
    pe_t *g = L->pe;
    pthread_mutex_lock(&g->lock);
    uint64_t target = L->issued;
    pthread_mutex_unlock(&g->lock);
    g->cnt.quiets++;
    wait_cntr(L, target, "payload");
    if (g->cfg.sig == XSHMEM_SIG_FLUSH && L->nt) {
        flush_targets(L);
        pthread_mutex_lock(&g->lock); target = L->issued; pthread_mutex_unlock(&g->lock);
        wait_cntr(L, target, "flush");
    }
}
void xshmem_ctx_quiet(xshmem_ctx_t ctx) { quiet_lane(lane_of(ctx)); }
void xshmem_ctx_fence(xshmem_ctx_t ctx) { quiet_lane(lane_of(ctx)); }

void xshmem_ctx_putmem_signal_nbi(xshmem_ctx_t ctx, void *dest, const void *src, size_t n, uint64_t *sig, uint64_t val, int sig_op, int pe)
{
    pe_t *g = lane_of(ctx)->pe;
    if (sig_op != XSHMEM_SIGNAL_SET) xfatal("putmem_signal_nbi: only XSHMEM_SIGNAL_SET");
    if (!g->log_sp) { g->log_sp = 1; if (g->mype == 0) fprintf(stderr, "xshmem_ofi: putmem_signal_nbi (mode dc) blocks inside the call like libsma 11.8: payload DC, counter wait, signal DC\n"); }
    xshmem_ctx_putmem_nbi(ctx, dest, src, n, pe);
    quiet_lane(lane_of(ctx));                  /* payload delivered at the target before the signal is posted */
    xshmem_ctx_signal_set(ctx, sig, val, pe);
}

/* ---- signals: spin on the local word ---- */
static void progress_try(void)
{
    pe_t *g = cur_pe();
    if (pthread_mutex_trylock(&g->lock)) return;
    for (int e = 0; e < g->nep; e++) if (g->lane[e].ep) drain_locked(&g->lane[e]);
    pthread_mutex_unlock(&g->lock);
}
void xshmem_progress(void) { progress_try(); }
static int cmp_ge(uint64_t w, uint64_t v) { return w >= v; }
int xshmem_signal_test(const uint64_t *sig, int cmp, uint64_t v)
{
    if (cmp != XSHMEM_CMP_GE) xfatal("signal_test: only XSHMEM_CMP_GE");
    if (cmp_ge(__atomic_load_n(sig, __ATOMIC_ACQUIRE), v)) return 1;
    progress_try();
    return cmp_ge(__atomic_load_n(sig, __ATOMIC_ACQUIRE), v);
}
uint64_t xshmem_signal_wait_until(uint64_t *sig, int cmp, uint64_t v)
{
    pe_t *g = cur_pe(); unsigned spins = 0; double t0 = now_s();
    if (cmp != XSHMEM_CMP_GE) xfatal("signal_wait_until: only XSHMEM_CMP_GE");
    for (;;) {
        uint64_t w = __atomic_load_n(sig, __ATOMIC_ACQUIRE);
        if (cmp_ge(w, v)) return w;
        __atomic_add_fetch(&g->cnt.wait_spins, 1, __ATOMIC_RELAXED);
        progress_try();
        if (++spins > 1000) { YIELD(); spins = 0; if (now_s() - t0 > g->quiet_s) xfatal("signal_wait_until: word %p still %llu < %llu after %.0f s (deadline)", (void *)sig, (unsigned long long)w, (unsigned long long)v, g->quiet_s); }
    }
}

/* ---- open ---- */
static int nic_slot(const pe_t *g, int e) { return (e == 0 ? 0 : e - 1) % g->nics; }
static void open_dom(pe_t *g, int s)
{
    dom_t *D = &g->dom[s]; char name[16]; int rc = 0;
    D->cxi = g->topo.nic[s]; snprintf(name, sizeof name, "cxi%d", D->cxi);
    for (int attempt = 0; attempt < 2; attempt++) {
        struct fi_info *h = fi_allocinfo();
        h->fabric_attr->prov_name = strdup("cxi"); h->domain_attr->name = strdup(name);
        h->ep_attr->type = FI_EP_RDM;
        h->caps = FI_RMA | FI_MSG | FI_HMEM;
        int libsma = g->cfg.mrmode == XSHMEM_MR_LIBSMA ? attempt == 0 : attempt != 0;   /* the other set is the fallback */
        h->domain_attr->mr_mode = libsma ? (FI_MR_ALLOCATED | FI_MR_ENDPOINT | FI_MR_HMEM)
                                         : (FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_ENDPOINT | FI_MR_HMEM);
        h->domain_attr->threading = FI_THREAD_DOMAIN;
        h->tx_attr->size = 4096;
        h->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
        rc = fi_getinfo(FI_VERSION(1, 20), NULL, NULL, 0, h, &D->info);
        fi_freeinfo(h);
        if (!rc) { if (attempt && g->mype == 0) fprintf(stderr, "xshmem_ofi: cxi%d: first mr_mode set refused, using the other\n", D->cxi); break; }
    }
    CK(rc);
    CK(fi_fabric(D->info->fabric_attr, &D->fab, NULL));
    CK(fi_domain(D->fab, D->info, &D->dom, NULL));
    struct fi_av_attr ava; memset(&ava, 0, sizeof ava); ava.type = FI_AV_TABLE;
    CK(fi_av_open(D->dom, &ava, &D->av, NULL));
    D->first_ep = -1; D->open = 1;
}
static void open_ep(pe_t *g, int e)
{
    lane_t *L = &g->lane[e]; int s = nic_slot(g, e); dom_t *D = &g->dom[s];
    L->e = e; L->slot = s; L->pe = g;
    if (!D->open) open_dom(g, s);
    struct fi_cq_attr cqa; memset(&cqa, 0, sizeof cqa); cqa.format = FI_CQ_FORMAT_CONTEXT; cqa.size = 1024;
    CK(fi_cq_open(D->dom, &cqa, &L->cq, NULL));
    struct fi_cntr_attr ca; memset(&ca, 0, sizeof ca); ca.events = FI_CNTR_EVENTS_COMP; ca.wait_obj = FI_WAIT_NONE;
    CK(fi_cntr_open(D->dom, &ca, &L->cn, NULL));
    CK(fi_endpoint(D->dom, D->info, &L->ep, NULL));
    CK(fi_ep_bind(L->ep, &L->cq->fid, FI_TRANSMIT | FI_RECV | FI_SELECTIVE_COMPLETION));
    CK(fi_ep_bind(L->ep, &L->cn->fid, FI_WRITE));
    CK(fi_ep_bind(L->ep, &D->av->fid, 0));
    CK(fi_enable(L->ep));
    if (D->first_ep < 0) D->first_ep = e;
    L->fa = (fi_addr_t *)calloc((size_t)g->npes, sizeof(fi_addr_t));
    L->touch = (unsigned char *)calloc((size_t)g->npes, 1); L->tlist = (int *)calloc((size_t)g->npes, sizeof(int));
}

/* open one PE: its NIC domains, endpoints (default + lanes + spares); the address exchange is done by the caller (one allgather per process) */
static void pe_open(pe_t *g)
{
    if (g->topo.nics < 1) xfatal("no cxi NIC for this PE (pe_geo nics = 0): the libfabric backend needs one");
    g->nics = g->topo.nics > MAXD ? MAXD : g->topo.nics;
    int nl = g->topo.nlanes < 1 ? 1 : g->topo.nlanes;
    g->nep = 1 + nl + SPARE;
    if (g->nep > MAXEP) xfatal("XSHMEM_LANES = %d: at most %d lanes", nl, MAXEP - 1 - SPARE);
    g->quiet_s = 130.0;                        /* deadline like libsma; the env override XSHMEM_QUIET_TIMEOUT waits for a registry row (B1) */
    { int d = g->topo.gpu < 0 ? 0 : g->topo.gpu; if (hipSetDevice(d) != hipSuccess) xfatal("hipSetDevice(%d)", d); }
    for (int e = 0; e < g->nep; e++) open_ep(g, e);
    struct fi_info *i0 = g->dom[0].info;
    g->vaddr = (i0->domain_attr->mr_mode & FI_MR_VIRT_ADDR) != 0; g->pkey = (i0->domain_attr->mr_mode & FI_MR_PROV_KEY) != 0;
    g->inject = i0->tx_attr->inject_size; g->maxmsg = i0->ep_attr->max_msg_size ? i0->ep_attr->max_msg_size : ((size_t)1 << 31);
    if (g->maxmsg > ((size_t)1 << 31)) g->maxmsg = (size_t)1 << 31;
}

int xshmem_init_thread(int req, int *prov)
{
    (void)req;
    if (W.init) xfatal("xshmem_init_thread called twice");
    xshmem_cfg_t cfg; xshmem_cfg_load(&cfg); g_yield = !cfg.noyield;
    { const char *e = getenv("XSHMEM_OFI_SIG"); if (e && *e && strcmp(e, "dc") && strcmp(e, "flush") && strcmp(e, "waw")) { fprintf(stderr, "xshmem_ofi: XSHMEM_OFI_SIG=%s: use dc, flush or waw\n", e); abort(); } }
    if (cfg.sig == XSHMEM_SIG_WAW) { fprintf(stderr, "xshmem_ofi: XSHMEM_OFI_SIG=waw is not yet implemented (dc and flush)\n"); abort(); }
    int K = cfg.loopback_pes;
    if (K < 1 || K > MAXLPE) { fprintf(stderr, "xshmem_ofi: XSHMEM_LOOPBACK_PES=%d: use 1..%d\n", K, MAXLPE); abort(); }
    pe_topo t0; pe_geo_init(&t0);              /* process level: boot rank and size, node placement */
    if (getenv("XSHMEM_LOCAL") && atoi(getenv("XSHMEM_LOCAL"))) fprintf(stderr, "xshmem_ofi: XSHMEM_LOCAL is ignored by this backend (XSHMEM_PEER=1 is the node-local path)\n");
    if ((cfg.peer == 1 || cfg.ipc) && K > 1) fprintf(stderr, "xshmem_ofi: XSHMEM_PEER / XSHMEM_IPC need one PE per process (XSHMEM_LOOPBACK_PES=1): off\n");
    int q = t0.pe, P = t0.npes;
    memset(&W, 0, sizeof W); W.K = K; W.q = q; W.P = P;
    int npes = P * K;
    /* K = 1: the topology of pe_geo_init as it is.  K > 1: PE q*K + l is local PE (local_proc*K + l) of a node with pes_per_node*K PEs, its own GPU and NIC */
    for (int l = 0; l < K; l++) {
        pe_t *g = &G[l]; memset(g, 0, sizeof *g); pthread_mutex_init(&g->lock, NULL);
        g->lpe = l; g->cfg = cfg; cur_l = l;
        if (K == 1) g->topo = t0;
        else {
            int ngpu = 0; char bus[64] = { 0 }, *bp = NULL;
            if (hipGetDeviceCount(&ngpu) != hipSuccess) { (void)hipGetLastError(); ngpu = 0; }
            int ppn = t0.pes_per_node * K, local = (&t0)->local * K + l;
            if (pe_geo_from(&g->topo, q * K + l, npes, local, ppn, ngpu, NULL)) xfatal("pe_geo: %s", pe_geo_err());
            if (ngpu > 0 && g->topo.gpu >= 0 && hipDeviceGetPCIBusId(bus, sizeof bus, g->topo.gpu) == hipSuccess) {
                for (char *c = bus; *c; c++) if (*c >= 'A' && *c <= 'F') *c += 'a' - 'A';
                bp = bus;
                if (pe_geo_from(&g->topo, q * K + l, npes, local, ppn, ngpu, bp)) xfatal("pe_geo: %s", pe_geo_err());
            } else (void)hipGetLastError();
        }
        g->mype = g->topo.pe; g->npes = g->topo.npes; g->nnodes = g->topo.nnodes;
        if (K > 1) { char ln[400]; pe_geo_str(&g->topo, ln, sizeof ln); if (q == 0) fprintf(stderr, "pe_geo: %s\n", ln); }
        pe_open(g);
    }
    cur_l = 0;
    /* address exchange: ONE allgather per process, K blobs each (every endpoint's name), then each peer's endpoint e into the AV of e's domain */
    ablob_t *mine = (ablob_t *)calloc((size_t)K, sizeof *mine);
    for (int l = 0; l < K; l++) {
        pe_t *g = &G[l]; mine[l].nics = (uint32_t)g->nics; mine[l].nep = (uint32_t)g->nep;
        for (int e = 0; e < g->nep; e++) { size_t al = MAXADDR; cur_l = l; CK(fi_getname(&g->lane[e].ep->fid, mine[l].ep[e].addr, &al)); mine[l].ep[e].alen = (uint32_t)al; }
    }
    ablob_t *all = (ablob_t *)malloc((size_t)npes * sizeof *all);
    boot_allgather(mine, (size_t)K * sizeof *mine, all);
    for (int l = 0; l < K; l++) {
        pe_t *g = &G[l]; cur_l = l;
        for (int p = 0; p < npes; p++) {
            if (all[p].nics != mine[l].nics || all[p].nep != mine[l].nep) xfatal("pe %d has %u NICs and %u endpoints, I have %u and %u (NIC symmetry required; set COMM_OFI_NICS / XSHMEM_LANES alike)", p, all[p].nics, all[p].nep, mine[l].nics, mine[l].nep);
            for (int e = 0; e < g->nep; e++) {
                int rc = fi_av_insert(g->dom[g->lane[e].slot].av, all[p].ep[e].addr, 1, &g->lane[e].fa[p], 0, NULL);
                if (rc != 1) xfatal("fi_av_insert of pe %d endpoint %d: %d", p, e, rc);
            }
        }
        g->lane[0].used = 1;
    }
    cur_l = 0;
    free(all); free(mine);
    W.init = 1;
    if (prov) *prov = XSHMEM_THREAD_SERIALIZED;   /* issued counts, ctx_create and the counters are not locked per lane */
    pe_t *g = &G[0]; struct fi_info *i0 = g->dom[0].info; int nl = g->topo.nlanes < 1 ? 1 : g->topo.nlanes;
    if (g->mype == 0) {
        fprintf(stderr, "xshmem_ofi: %d PEs (%d nodes), sig %s, mrmode requested %s, in force mr_mode %#x (%s%s), inject %zu B, max_msg %zu, lanes %d + default + %d spare, NICs/PE %d (cxi%d..), "
                "ctrl %s, quiet deadline %.0f s, caps dev_heap 1 host_pollable 1 src_any 0 ptr 0\n",
                g->npes, g->nnodes, g->cfg.sig == XSHMEM_SIG_FLUSH ? "flush" : "dc", g->cfg.mrmode == XSHMEM_MR_LIBSMA ? "libsma" : "ofi", (unsigned)i0->domain_attr->mr_mode, g->vaddr ? "VIRT_ADDR " : "offset ", g->pkey ? "PROV_KEY" : "client keys",
                g->inject, g->maxmsg, nl, SPARE, g->nics, g->dom[0].cxi, g->cfg.ctrl_host ? "host" : "heap", g->quiet_s);
        if (K > 1) fprintf(stderr, "xshmem_ofi: loopback: %d PEs per process (XSHMEM_LOOPBACK_PES), %d processes\n", K, P);
    }
    return 0;
}
int xshmem_my_pe(void) { return cur_pe()->mype; }
int xshmem_n_pes(void) { return cur_pe()->npes; }

void xshmem_ofi_loopback_select(int lpe)
{
    if (lpe < 0 || lpe >= MAXLPE || (W.init && lpe >= W.K)) xfatal("loopback_select(%d): this process hosts %d PEs", lpe, W.init ? W.K : 1);
    cur_l = lpe;
}
int xshmem_ofi_loopback_pes(void)
{
    if (W.init) return W.K;
    xshmem_cfg_t c; xshmem_cfg_load(&c); return c.loopback_pes < 1 ? 1 : c.loopback_pes;
}

int xshmem_ctx_create(long opt, xshmem_ctx_t *c)
{
    pe_t *g = cur_pe(); (void)opt;
    for (int e = 1; e < g->nep; e++) if (!g->lane[e].used) { g->lane[e].used = 1; *c = &g->lane[e]; return 0; }
    return -1;
}
void xshmem_ctx_destroy(xshmem_ctx_t ctx) { lane_t *L = lane_of(ctx); if (L->e) { quiet_lane(L); L->used = 0; } }

/* ---- heaps ---- */
/* Collective.  Loopback: the K local PEs call in order 0..K-1 from one thread (XSHMEM_SELECT before each); the K-th call does the one
 * process-level allgather of the K heap blobs. */
void *xshmem_heap_alloc(size_t bytes, int space)
{
    pe_t *g = cur_pe(); int K = W.K, l = g->lpe, dev_prev = 0;
    if (!W.init) xfatal("heap_alloc before init");
    if (space < 0 || space > 1) xfatal("heap_alloc: space %d", space);
    if (g->hbase[space]) xfatal("heap_alloc: space %d allocated twice", space);
    if (W.nalloc[space] != l) xfatal("heap_alloc: loopback PEs allocate in order 0..%d from one thread: got PE %d, expected %d", K - 1, l, W.nalloc[space]);
    if (space == XSHMEM_SPACE_DATA && g->cfg.heap_mb > 0) bytes = (size_t)g->cfg.heap_mb << 20;
    bytes = (bytes + HALIGN - 1) & ~(HALIGN - 1); if (!bytes) bytes = HALIGN;
    size_t user = bytes; int host = space == XSHMEM_SPACE_CTRL && g->cfg.ctrl_host; void *p = 0;
    if (space == XSHMEM_SPACE_DATA && g->cfg.sig == XSHMEM_SIG_FLUSH) { g->flush_off = user; bytes += HALIGN; }   /* hidden tail: flush slot, not addressable by the engine */
    if (K > 1) (void)hipGetDevice(&dev_prev);
    { int d = g->topo.gpu < 0 ? 0 : g->topo.gpu; if (hipSetDevice(d) != hipSuccess) xfatal("hipSetDevice(%d)", d); }
    if (host) {
        p = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) xfatal("mmap of the %zu MiB host CTRL heap failed", bytes >> 20);
        memset(p, 0, bytes);
        if (hipHostRegister(p, bytes, hipHostRegisterPortable) != hipSuccess) xfatal("hipHostRegister of the CTRL heap");
    } else {
        hipError_t he = hipExtMallocWithFlags(&p, bytes, hipDeviceMallocFinegrained);
        if (he != hipSuccess) xfatal("the %s heap (%zu MiB, XSHMEM_HEAP_MB): %s", space ? "CTRL" : "DATA", bytes >> 20, hipGetErrorString(he));
        he = hipMemset(p, 0, bytes); if (he == hipSuccess) he = hipDeviceSynchronize();
        if (he != hipSuccess) xfatal("hipMemset of the heap: %s", hipGetErrorString(he));
    }
    if ((uintptr_t)p & (HALIGN - 1)) fprintf(stderr, "xshmem_ofi: pe %d: heap %d base %p is not 2 MiB aligned\n", g->mype, space, p);
    hblob_t mine; memset(&mine, 0, sizeof mine); mine.base = (uint64_t)(uintptr_t)p; mine.len = user;
    if (space == XSHMEM_SPACE_DATA && K == 1 && (g->cfg.peer || g->cfg.ipc) && !host) {
        hipError_t ie = hipIpcGetMemHandle(&mine.ipc, p);
        if (ie == hipSuccess) mine.has_ipc = 1; else { (void)hipGetLastError(); fprintf(stderr, "xshmem_ofi: pe %d: hipIpcGetMemHandle of the DATA heap: %s: XSHMEM_PEER off for this PE\n", g->mype, hipGetErrorString(ie)); }
    }
    for (int e = 0; e < g->nep; e++) {
        lane_t *L = &g->lane[e]; dom_t *D = &g->dom[L->slot];
        struct iovec iov = { p, bytes }; struct fi_mr_attr ma; memset(&ma, 0, sizeof ma);
        ma.mr_iov = &iov; ma.iov_count = 1; ma.access = FI_READ | FI_WRITE | FI_REMOTE_READ | FI_REMOTE_WRITE;
        ma.iface = host ? FI_HMEM_SYSTEM : FI_HMEM_ROCR; ma.device.reserved = host ? 0 : (g->topo.gpu < 0 ? 0 : g->topo.gpu);
        int rc = 0;
        for (int attempt = 0; attempt < (K > 1 ? 3 : 2); attempt++) {   /* same client key on every endpoint first (as libsma), else distinct keys (loopback: also per PE) */
            ma.requested_key = (uint64_t)(2 + space + (attempt ? 4 * e : 0) + (attempt > 1 ? 64 * l : 0));
            rc = fi_mr_regattr(D->dom, &ma, 0, &L->mr[space]);
            if (!rc) { if (attempt && !g->log_key) { g->log_key = 1; if (g->mype == 0) fprintf(stderr, "xshmem_ofi: duplicate client key refused, distinct keys per endpoint\n"); } break; }
        }
        if (rc) xfatal("registering the %s heap (%zu MiB) with cxi%d endpoint %d: %d (%s); XSHMEM_MRMODE=%s may work", space ? "CTRL" : "DATA", bytes >> 20, D->cxi, e, rc, fi_strerror(-rc), g->cfg.mrmode == XSHMEM_MR_LIBSMA ? "ofi" : "libsma");
        if (D->info->domain_attr->mr_mode & FI_MR_ENDPOINT) {
            rc = fi_mr_bind(L->mr[space], &L->ep->fid, 0); if (rc) xfatal("fi_mr_bind of the %s heap to endpoint %d on cxi%d: %d (%s)", space ? "CTRL" : "DATA", e, D->cxi, rc, fi_strerror(-rc));
            CK(fi_mr_enable(L->mr[space]));
        }
        L->desc[space] = fi_mr_desc(L->mr[space]); L->key[space] = fi_mr_key(L->mr[space]);
        mine.key[e] = L->key[space];
    }
    g->hbase[space] = (char *)p; g->hlen[space] = user; g->hhost[space] = host;
    if (!l) W.mine[space] = (hblob_t *)calloc((size_t)K, sizeof(hblob_t));
    W.mine[space][l] = mine;
    if (K > 1) (void)hipSetDevice(dev_prev);
    if (++W.nalloc[space] < K) return p;
    /* the K-th local PE: one process-level allgather; also the collective barrier: every MR is enabled before any peer writes */
    int nopen = 0; for (int s = 0; s < MAXD; s++) nopen += G[0].dom[s].open;
    hblob_t *all = (hblob_t *)malloc((size_t)g->npes * sizeof(hblob_t));
    boot_allgather(W.mine[space], (size_t)K * sizeof(hblob_t), all);
    for (int k = 0; k < K; k++) {
        pe_t *h = &G[k];
        h->peer[space] = (hblob_t *)malloc((size_t)h->npes * sizeof(hblob_t)); memcpy(h->peer[space], all, (size_t)h->npes * sizeof(hblob_t));
        for (int r = 0; r < h->npes; r++) if (all[r].len != user) xfatal("heap %d: pe %d has %llu B, I have %zu B (same size on every PE)", space, r, (unsigned long long)all[r].len, user);
    }
    if (space == XSHMEM_SPACE_DATA && K == 1 && (g->cfg.peer || g->cfg.ipc) && !host) {        /* open the node-local peers' DATA heaps on my device */
        g->pmap = (char **)calloc((size_t)g->npes, sizeof(char *)); int nmap = 0, ppn = g->topo.pes_per_node;
        int d = g->topo.gpu < 0 ? 0 : g->topo.gpu; if (hipSetDevice(d) != hipSuccess) xfatal("hipSetDevice(%d)", d);
        for (int r = 0; r < g->npes; r++) if (r != g->mype && r / ppn == g->topo.node && all[r].has_ipc) {
            void *mp = 0; hipError_t oe = hipIpcOpenMemHandle(&mp, all[r].ipc, hipIpcMemLazyEnablePeerAccess);
            if (oe != hipSuccess) { (void)hipGetLastError(); fprintf(stderr, "xshmem_ofi: pe %d: hipIpcOpenMemHandle of pe %d: %s: that peer goes over the NIC\n", g->mype, r, hipGetErrorString(oe)); continue; }
            g->pmap[r] = (char *)mp; nmap++;
        }
        if (g->mype == 0) fprintf(stderr, "xshmem_ofi: XSHMEM_PEER: pe 0 mapped %d node-local peer DATA heap(s) by hipIpc\n", nmap);
    }
    free(all); free(W.mine[space]); W.mine[space] = 0;
    if (G[0].mype == 0) fprintf(stderr, "xshmem_ofi: heap %s %zu MiB (%s) registered per endpoint on %d domain(s)%s, key %s\n", space ? "CTRL" : "DATA", user >> 20, host ? "pinned host" : "fine-grained device", nopen, K > 1 ? " of PE 0 (per PE: its own heap on its own GPU)" : "", g->pkey ? "provider" : "client");
    return p;
}
static void heap_release(pe_t *g, int space)
{
    if (!g->hbase[space]) return;
    if (space == XSHMEM_SPACE_DATA && g->pmap) { for (int r = 0; r < g->npes; r++) if (g->pmap[r]) (void)hipIpcCloseMemHandle(g->pmap[r]); free(g->pmap); g->pmap = 0; }
    for (int e = 0; e < g->nep; e++) if (g->lane[e].mr[space]) { fi_close(&g->lane[e].mr[space]->fid); g->lane[e].mr[space] = 0; }
    if (g->hhost[space]) { (void)hipHostUnregister(g->hbase[space]); munmap(g->hbase[space], g->hlen[space]); } else (void)hipFree(g->hbase[space]);
    free(g->peer[space]); g->peer[space] = 0; g->hbase[space] = 0; g->hlen[space] = 0;
}
static void quiet_all(void)
{
    for (int l = 0; l < W.K; l++) for (int e = 0; e < G[l].nep; e++) quiet_lane(&G[l].lane[e]);
}
/* Collective, once per space and PE (loopback: K calls from one thread; the first quiets every local PE and fences once per process) */
void xshmem_heap_free(void *base)
{
    pe_t *g = cur_pe();
    int s = (base && base == g->hbase[0]) ? 0 : (base && base == g->hbase[1]) ? 1 : -1;
    if (s < 0) xfatal("heap_free: %p is not a heap base", base);
    if (!W.nfree[s]) { quiet_all(); boot_fence(); }
    heap_release(g, s);
    if (++W.nfree[s] == W.K) { W.nfree[s] = 0; W.nalloc[s] = 0; }
}

void xshmem_barrier_all(void)     /* once per process */
{
    quiet_all();
    boot_fence();
}
void xshmem_finalize(void)        /* once per process */
{
    if (!W.init) return;
    xshmem_barrier_all();
    xshmem_ofi_cnt_t sum; memset(&sum, 0, sizeof sum);
    for (int l = 0; l < W.K; l++) {
        pe_t *g = &G[l];
        heap_release(g, 0); heap_release(g, 1);
        for (int e = 0; e < g->nep; e++) {
            lane_t *L = &g->lane[e]; if (!L->ep) continue;
            fi_close(&L->ep->fid); fi_close(&L->cq->fid); fi_close(&L->cn->fid); free(L->fa); free(L->touch); free(L->tlist); L->ep = 0;
        }
        for (int s = 0; s < MAXD; s++) {
            dom_t *D = &g->dom[s]; if (!D->open) continue;
            fi_close(&D->av->fid); fi_close(&D->dom->fid); fi_close(&D->fab->fid); fi_freeinfo(D->info); D->open = 0;
        }
        sum.writes += g->cnt.writes; sum.flush_chains += g->cnt.flush_chains; sum.quiets += g->cnt.quiets; sum.wait_spins += g->cnt.wait_spins;
    }
    if (G[0].mype == 0) fprintf(stderr, "xshmem_ofi: pe 0 counters%s: writes %llu flush_chains %llu quiets %llu wait_spins %llu\n", W.K > 1 ? " (sum of this process's PEs)" : "", (unsigned long long)sum.writes, (unsigned long long)sum.flush_chains, (unsigned long long)sum.quiets, (unsigned long long)sum.wait_spins);
    W.init = 0;
    boot_finalize();
}
void xshmem_global_exit(int status) { pe_t *g = cur_pe(); char b[64]; snprintf(b, sizeof b, "global_exit(%d)", status); fprintf(stderr, "xshmem_ofi: pe %d: %s\n", g->mype, b); fflush(stderr); boot_abort(b); }

void *xshmem_ptr(const void *p, int pe)       /* XSHMEM_PEER: the node-local peer's copy of p (same offset in its DATA heap), else NULL */
{
    pe_t *g = cur_pe(); size_t o;
    if (!g->pmap || pe < 0 || pe >= g->npes || !g->pmap[pe]) return NULL;
    if (locate(g, p, &o) != XSHMEM_SPACE_DATA) return NULL;
    return g->pmap[pe] + o;
}
int xshmem_topo(xshmem_topo_t *t)
{
    const pe_topo *q = &cur_pe()->topo;
    if (!W.init) return -1;
    memset(t, 0, sizeof *t);
    t->pe = q->pe; t->npes = q->npes; t->node = q->node; t->nnodes = q->nnodes; t->local = q->local; t->pes_per_node = q->pes_per_node;
    t->gpu = q->gpu; t->gpu_node = q->gpu_node; t->nics = q->nics; t->nlanes = q->nlanes; t->numa = q->numa; t->pes_per_gpu = q->pes_per_gpu;
    for (int i = 0; i < q->nics && i < XSHMEM_TOPO_MAXNIC; i++) t->nic[i] = q->nic[i];
    strncpy(t->cpus, q->cpus, sizeof t->cpus - 1);
    return 0;
}
void xshmem_caps_query(xshmem_caps_t *c)
{
    c->dev_heap = 1; c->host_pollable = 1; c->src_any = 0; c->ptr = W.init && cur_pe()->pmap != 0; c->nlanes_ok = W.init ? cur_pe()->nics : 1;
}
void xshmem_ofi_counters(xshmem_ofi_cnt_t *c)
{
    memset(c, 0, sizeof *c);
    for (int l = 0; l < (W.init ? W.K : 1); l++) { c->writes += G[l].cnt.writes; c->flush_chains += G[l].cnt.flush_chains; c->quiets += G[l].cnt.quiets; c->wait_spins += G[l].cnt.wait_spins; }
}
