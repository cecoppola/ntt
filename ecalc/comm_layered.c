/* comm_layered.c - the layered communicator: the rank space of a whole-machine
 * transform is (4 APUs) x (g nodes).  APU d of node r is global rank rho = g d + r (APU-major), so the g
 * slabs bound for the APUs d' of every node are contiguous in the sender's slab buffer.  An all-to-all is
 * two exchanges with a block transpose between them:
 *   1. intra-node (xGMI push, the four APU threads): APU d sends block d' = slabs {g d' + r'} to APU d';
 *      the receive holds [d source][r' dest] blocks
 *   2. transpose to [r'][d]; inter-node over mesh d' (TCP here, RDMA on the target; rank = node): node r
 *      sends block r' = the four slabs (r, 0..3) -> (r', d') to node r'; the receive holds [r][d]
 *   3. transpose to [d][r] = global source rank order.
 * The transposes are 4 g device copies of one slab each; the scratch (one slab buffer) is the caller's
 * (comm_layered_scratch) or hipMalloc'd on first use.
 * M7: two exchanges may be in flight (inflight 2): the second's intra-node stage runs while the first's
 * inter-node stage is on the wire (the inter transport takes one at a time, so posting the second completes
 * the first before its own inter stage starts); each pending exchange has its own slot of the scratch (two
 * slots of 4 g x bytes -- when only one fits, the exchanges serialise).  wait() completes the oldest exchange
 * not yet waited for (one wait per post: the k-th wait guarantees the k-th exchange, whether the transport
 * finished it early or not).
 * allgather: inter first (mesh d gathers my block over the nodes into my slot [d][0..g) of the result --
 * every mesh carries one block per node), then the xGMI all-gather of the g-block slots; no transpose, and the
 * fabric moves 4 x less than intra-first would.
 * B7 alltoallv: the same three stages with per-peer counts.  The node's send counts are all-gathered over the four
 * APU threads (4 x 4 g size_t) so that every stage's receive sizes are known: intra -- APU d sends APU d' the g
 * slabs bound for ranks (d', 0..g-1) as one block (the caller's slabs are used in place when they lie back to back in
 * rank order, else copied into that order); transpose to [r'][d]; inter over mesh d with per-node counts; wait --
 * the [r][d] slabs into the receive buffer at the caller's offsets.  Its scratch (2 x the intra total + the receive
 * total) is the communicator's own; a v-exchange never overlaps an equal-slab one (either kind completes the other). */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#include "comm_ofi.h"                                /* comm_ofi_planned: the COMM_LAYER_INTER2 default */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#define NA 4
/* (the stage-timeline statistics, COMM_LAYER_STATS, and the xGMI link accounting, COMM_XGMI_STATS, were removed 2026-10-19, D56) */
static inline double lay_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
struct lay_ex { void *rb; size_t bytes; hipStream_t s; char *tmp; int inter_posted; };   /* one pending exchange */
struct lay_v { void *rb; const size_t *rcnt, *rdsp; hipStream_t s; char *x3; size_t *cnt2; int pend; int slot; comm *ic; };   /* the pending v-exchange (slot: its x2/x3 area at depth 2) */
typedef struct { comm *intra, *inter; int d, g, na, dev; char *tmp; size_t tmp_cap; int own_tmp, tmp_sym;   /* tmp_sym: the scratch is the inter transport's symmetric memory (S12) */   /* d: my intra rank; dev: my device; na: intra size; minor: rho = na r + d (else g d + r) */
                 struct lay_ex ex[2]; int head, npend, nlog;                /* npend: physically pending; nlog: posted minus waited */
                 char *vtmp; size_t vcap; struct lay_v v;
                 comm *inter2; int i2;   /* X2: the second inter communicator (COMM_LAYER_INTER2); the v-slots always come from the comm pool */
                 int tker; char *vx[2]; size_t vxcap[2]; struct cpy *ctab; int ctcap; } lay_priv;   /* see below */
#define PRIV(c) ((lay_priv *)(c)->priv)
static inline int rho_of(const lay_priv *p, int d, int r) { return p->g * d + r; }   /* the global rank of (intra d, inter r) */
static int lay_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int y_rank(comm *c) { return c->rank; }
static int y_size(comm *c) { return c->size; }
static void need_tmp_i(comm *c, size_t bytes);
static void need_tmp(comm *c, size_t bytes) { int ph_ = ec_ph(EC_PH_HIP_MALLOC); need_tmp_i(c, bytes); ec_ph(ph_); }   /* (comm_phase.h) */
static void need_tmp_i(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->tmp_cap >= bytes) return;
    if (!p->own_tmp && p->tmp) { ec_fatal(EC_RC_FATAL, "comm_layered: the caller's scratch (%zu B) is smaller than the exchange (%zu B)\n", p->tmp_cap, bytes); }
    if (p->tmp) { if (p->tmp_sym) comm_sym_free(p->inter, p->tmp); else HIP_CHECK(hipFree(p->tmp)); }
    HIP_CHECK(hipSetDevice(p->dev));
    /* the scratch from the inter transport's symmetric pool where it has one (the SHMEM
     * transport then receives the inter stage straight into it, no staging), else hipMalloc'd as before */
    p->tmp = (char *)comm_sym_alloc(p->inter, bytes); p->tmp_sym = p->tmp != 0;
    if (!p->tmp) HIP_CHECK(hipMalloc((void **)&p->tmp, bytes));
    p->tmp_cap = bytes; p->own_tmp = 1;
}
/* ---- the block transposes (one kernel; the 4 x g hipMemcpyAsync copies remain only as the fallback of the size guards below) and the v-exchange's reorder copies as one kernel
 * each instead of na x nb hipMemcpyAsync calls (2304 per transpose at g = 576).  Equal slabs: grid.y = the destination block
 * (b, a), grid.x strides over its 16-byte words (coalesced on both sides).  Unequal: a table of (dst, src, len) entries in pinned
 * host memory read by the kernel (grid.y = entry); every use is followed by a stream synchronise before the table is refilled.
 * Falls back to the copies when a pointer is not device-accessible (plain host memory) or a size is not a multiple of 8/16 B. */
struct cpy { size_t dst, src, len; };
__global__ void k_btrans(uint4 *dst, const uint4 *src, size_t w, int na, int nb)
{
    unsigned q = blockIdx.y; int b = q / na, a = q % na;  /* destination block (b, a) <- source block (a, b) */
    uint4 *d = dst + (size_t)q * w; const uint4 *sp = src + ((size_t)a * nb + b) * w;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < w; i += (size_t)gridDim.x * blockDim.x) d[i] = sp[i];
}
__global__ void k_bcopy(char *dst, const char *src, const struct cpy *t)
{
    struct cpy e = t[blockIdx.y]; uint64_t *d = (uint64_t *)(dst + e.dst); const uint64_t *sp = (const uint64_t *)(src + e.src); size_t w = e.len >> 3;
    for (size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x; i < w; i += (size_t)gridDim.x * blockDim.x) d[i] = sp[i];
}
static int dev_ok(const void *ptr)                       /* device memory, pinned or registered host memory, managed: a kernel may touch it */
{
    hipPointerAttribute_t a;
    if (hipPointerGetAttributes(&a, ptr) != hipSuccess) { (void)hipGetLastError(); return 0; }
    if (a.type == hipMemoryTypeDevice || a.type == hipMemoryTypeManaged || a.type == hipMemoryTypeUnified) return 1;
    return a.type == hipMemoryTypeHost && a.devicePointer == ptr;   /* pinned / registered host memory mapped at the same address */
}
static unsigned gx_of(size_t w) { size_t b = (w + 255) / 256; return (unsigned)(b > 64 ? 64 : b ? b : 1); }
/* [a][b] blocks of `bytes` -> [b][a]: na x nb blocks */
static void block_transpose(void *dst, const void *src, size_t bytes, int na, int nb, hipStream_t s)
{
    if (bytes && !(bytes & 15) && (size_t)na * nb <= 65535 && dev_ok(dst) && dev_ok(src)) {
        k_btrans<<<dim3(gx_of(bytes >> 4), (unsigned)(na * nb)), 256, 0, s>>>((uint4 *)dst, (const uint4 *)src, bytes >> 4, na, nb);
        HIP_CHECK(hipGetLastError()); return;
    }
    for (int a = 0; a < na; a++) for (int b = 0; b < nb; b++)
        HIP_CHECK(hipMemcpyAsync((char *)dst + ((size_t)b * na + a) * bytes, (const char *)src + ((size_t)a * nb + b) * bytes, bytes, hipMemcpyDefault, s));   /* (S12: Default -- the scratch may be registered host memory) */
}
/* the batched form: n copies dst + e.dst <- src + e.src (e.len bytes); one kernel when allowed, else the copies */
static struct cpy *ctab_get(lay_priv *p, int n)
{
    if (n > p->ctcap) { if (p->ctab) HIP_CHECK(hipHostFree(p->ctab)); HIP_CHECK(hipHostMalloc((void **)&p->ctab, (size_t)n * sizeof(struct cpy), 0)); p->ctcap = n; }
    return p->ctab;
}
static void bcopy_run(lay_priv *p, char *dst, const char *src, int n, hipStream_t s)
{
    struct cpy *t = p->ctab; size_t mx = 0; int ok = n > 0 && n <= 65535 && dev_ok(dst) && dev_ok(src);
    for (int i = 0; i < n && ok; i++) { if ((t[i].dst | t[i].src | t[i].len) & 7) ok = 0; if (t[i].len > mx) mx = t[i].len; }
    if (ok) {
        k_bcopy<<<dim3(gx_of(mx >> 3), (unsigned)n), 256, 0, s>>>(dst, src, t);
        HIP_CHECK(hipGetLastError()); return; }
    for (int i = 0; i < n; i++) if (t[i].len) HIP_CHECK(hipMemcpyAsync(dst + t[i].dst, src + t[i].src, t[i].len, hipMemcpyDefault, s));
}
/* the inter-node stage of the oldest pending exchange (its intra stage and transpose are done) */
static void inter_post(comm *c, struct lay_ex *e)
{
    lay_priv *p = PRIV(c);
    int xk = comm_xtag_set(XK_LAYEQ);                                               /* X1: the xstats kind */
    comm_alltoall(p->inter, e->rb, e->tmp, e->bytes * p->na, e->s);
    comm_xtag_set(xk);
    e->inter_posted = 1;
}
/* complete the oldest pending exchange: tmp slot holds [r][d] after the inter wait; -> [d][r] into rb */
static void complete_oldest(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->npend) return;
    struct lay_ex *e = &p->ex[p->head];
    if (!e->inter_posted) inter_post(c, e);
    comm_wait(p->inter);
    HIP_CHECK(hipSetDevice(p->dev));
    block_transpose(e->rb, e->tmp, e->bytes, p->g, p->na, e->s); HIP_SYNC(e->s);   /* -> [d][r] = source rank order */
    p->head ^= 1; p->npend--;
}
static void complete_v(comm *c);
static void y_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na;
    size_t slot = bytes * (size_t)na * g;
    complete_v(c);                                       /* one kind at a time */
    need_tmp(c, (!p->tmp || p->own_tmp) ? 2 * slot : slot);   /* our own scratch holds two slots; the caller's what it is */
    if (p->npend == 2) complete_oldest(c);
    int two = p->tmp_cap >= 2 * slot;                    /* two slots fit: the previous exchange may stay on the wire */
    if (p->npend && !two) complete_oldest(c);
    struct lay_ex *e = &p->ex[(p->head + p->npend) & 1];
    e->rb = rb; e->bytes = bytes; e->s = s; e->tmp = p->tmp + (two ? ((p->head + p->npend) & 1) * slot : 0); e->inter_posted = 0;
    p->npend++; p->nlog++;
    HIP_CHECK(hipSetDevice(p->dev));
    /* 1: intra-node, blocks of g slabs; the xGMI wait synchronises the stream and the four threads */
    comm_alltoall(p->intra, sb, e->tmp, bytes * g, s);
    comm_wait(p->intra);
    /* 2: [d][r'] -> [r'][d] into rb, then the inter-node exchange of blocks of na slabs, received into the slot */
    block_transpose(rb, e->tmp, bytes, na, g, s);
    HIP_SYNC(s);
    if (p->npend == 2) complete_oldest(c);               /* the inter transport takes one exchange at a time */
    inter_post(c, e);
}
/* ---- B7: the unequal exchange ---- */
/* B7ACCT (results/GRID_GROUPS.md §B7ACCT): the v-exchange scratch hipMalloc'd now per device, over every layered communicator (need_vtmp, need_vslot,
 * freed by y_destroy) -- MEM_REPORT_DEVS prints it beside the layout's term (binsplit_vslot_bytes); a counter only, nothing else reads it */
static size_t g_vlive[16];
static void vlive_add(int dev, size_t add, size_t sub) { if (dev >= 0 && dev < 16) { __atomic_add_fetch(&g_vlive[dev], add, __ATOMIC_RELAXED); __atomic_sub_fetch(&g_vlive[dev], sub, __ATOMIC_RELAXED); } }
extern "C" size_t comm_layered_vbytes(int dev) { return dev >= 0 && dev < 16 ? __atomic_load_n(&g_vlive[dev], __ATOMIC_RELAXED) : 0; }
static void need_vtmp_i(comm *c, size_t bytes);
static void need_vtmp(comm *c, size_t bytes) { int ph_ = ec_ph(EC_PH_VTMP_ALLOC); need_vtmp_i(c, bytes); ec_ph(ph_); }
static void need_vtmp_i(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->vcap >= bytes) return;
    HIP_CHECK(hipSetDevice(p->dev));
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    HIP_CHECK(hipMalloc((void **)&p->vtmp, bytes)); vlive_add(p->dev, bytes, p->vcap); p->vcap = bytes;
}
/* the count tables of a v-exchange on this APU thread (rank g d + node, size 4 g): T[d'][rho] = APU d' of my node
 * sends rho (all-gathered over the intra communicator); the intra stage's counts sI/rI (per APU), the inter stage's
 * cnt2 (per node; send [0, g), recv [g, 2 g)).  All in bytes. */
struct vtab { size_t *T, *sI, *rI, *sIoff, *rIoff, *cnt2, *dsp2, A, B, S; int contig; };   /* sI.. rIoff: na each (after T) */
static void vtab_build(comm *c, const size_t *scnt, const size_t *sdsp, const size_t *rcnt, struct vtab *t, int host)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na, n = na * g, d = p->d;
    t->T = (size_t *)malloc(((size_t)na * n + 4 * (size_t)na) * sizeof(size_t)); t->sI = t->T + (size_t)na * n; t->rI = t->sI + na; t->sIoff = t->rI + na; t->rIoff = t->sIoff + na;
    t->cnt2 = (size_t *)malloc(4 * (size_t)g * sizeof(size_t)); t->dsp2 = t->cnt2 + 2 * g;
    (void)host; comm_allgather_host(p->intra, scnt, t->T, (size_t)n * sizeof(size_t));
    t->A = t->B = t->S = 0; t->contig = 1;
    for (int dd = 0; dd < na; dd++) {
        size_t si = 0, ri = 0;
        for (int r = 0; r < g; r++) { si += scnt[rho_of(p, dd, r)]; ri += t->T[(size_t)dd * n + rho_of(p, d, r)]; }
        t->sI[dd] = si; t->rI[dd] = ri; t->sIoff[dd] = t->S; t->rIoff[dd] = t->A; t->S += si; t->A += ri;
    }
    for (int rho = 1; rho < n; rho++) if (sdsp[rho] != sdsp[rho - 1] + scnt[rho - 1]) t->contig = 0;
    for (int r = 0; r < g; r++) { size_t sn = 0, rn = 0; for (int dd = 0; dd < na; dd++) { sn += t->T[(size_t)dd * n + rho_of(p, d, r)]; rn += rcnt[rho_of(p, dd, r)]; } t->cnt2[r] = sn; t->cnt2[g + r] = rn; }
    comm_prefix(t->cnt2, t->dsp2, g); t->B = comm_prefix(t->cnt2 + g, t->dsp2 + g, g);
}
static void vtab_free(struct vtab *t) { free(t->T); free(t->cnt2); }
/* a copy on the device stream or the host */
static void vcopy(void *dst, const void *src, size_t n, hipStream_t s, int host)
{
    if (!n) return;
    if (host) memmove(dst, src, n); else HIP_CHECK(hipMemcpyAsync(dst, src, n, hipMemcpyDefault, s));
}
/* the intra stage and the transpose: my slabs -> x1 [d][r'] blocks (the intra exchange) -> x2 [r'][d]; then the inter
 * stage's post.  x1/x2/x3 are the caller's areas (device or host); the intra and inter ops are the transport's of the kind. */
static void v_stages(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, struct vtab *t, char *x0, char *x1, char *x2, char *x3, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na, n = na * g, d = p->d;
    const char *src = (const char *)sb; size_t soff0 = sdsp[0];
    int tk = !host;                       /* (13b X: the reorder copies as one kernel each) */
    if (!t->contig) {                                    /* the slabs into [d'][r'] order (= rank order in the major form), back to back, in x0 */
        struct cpy *ct = tk ? ctab_get(p, n) : 0; int m = 0;
        size_t o = 0; for (int dd = 0; dd < na; dd++) for (int r = 0; r < g; r++) { int rho = rho_of(p, dd, r); if (tk) { ct[m].dst = o; ct[m].src = sdsp[rho]; ct[m++].len = scnt[rho]; } else vcopy(x0 + o, (const char *)sb + sdsp[rho], scnt[rho], s, host); o += scnt[rho]; }
        if (tk) bcopy_run(p, x0, (const char *)sb, m, s);
        src = x0; soff0 = 0;
    }
    size_t *sdI = (size_t *)malloc(2 * (size_t)na * sizeof(size_t)), *rdI = sdI + na; for (int dd = 0; dd < na; dd++) { sdI[dd] = soff0 + t->sIoff[dd]; rdI[dd] = t->rIoff[dd]; }
    if (host) comm_alltoallv_host(p->intra, src, t->sI, sdI, x1, t->rI, rdI);
    else { HIP_SYNC(s); comm_alltoallv(p->intra, src, t->sI, sdI, x1, t->rI, rdI, s); comm_wait(p->intra); }
    free(sdI);
    /* x1: block dd = slabs (dd -> (d, r')) for r' = 0..g-1 back to back; x2: node r' = its na slabs dd = 0..na-1 */
    size_t o2 = 0; struct cpy *ct = tk ? ctab_get(p, n) : 0; int m = 0;
    size_t *offd = (size_t *)malloc((size_t)na * sizeof(size_t)); for (int dd = 0; dd < na; dd++) offd[dd] = t->rIoff[dd];   /* (running offsets: O(n), was O(n g)) */
    for (int r = 0; r < g; r++) for (int dd = 0; dd < na; dd++) {
        size_t off = offd[dd], len = t->T[(size_t)dd * n + rho_of(p, d, r)]; offd[dd] += len;
        if (tk) { ct[m].dst = o2; ct[m].src = off; ct[m++].len = len; } else vcopy(x2 + o2, x1 + off, len, s, host);
        o2 += len;
    }
    free(offd);
    if (tk) bcopy_run(p, x2, x1, m, s);
    if (!host) HIP_SYNC(s);
}
/* the inter stage's post (after v_stages; at depth 2 after the previous v-exchange's completion) */
static void v_post(comm *c, comm *ic, struct vtab *t, char *x2, char *x3, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g;
    int xk = comm_xtag_set(XK_LAYV);                      /* X1: the xstats kind */
    if (host) comm_alltoallv_host(ic, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g);
    else comm_alltoallv(ic, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g, s);
    comm_xtag_set(xk);
}
/* x3 [r][d] -> the receive buffer at the caller's offsets */
static void v_scatter(comm *c, const struct vtab *t, const char *x3, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g, tk = !host, m = 0;
    struct cpy *ct = tk ? ctab_get(p, p->na * g) : 0;
    for (int r = 0; r < g; r++) { size_t off = t->dsp2[g + r]; for (int dd = 0; dd < p->na; dd++) { int rho = rho_of(p, dd, r); if (tk) { ct[m].dst = rdsp[rho]; ct[m].src = off; ct[m++].len = rcnt[rho]; } else vcopy((char *)rb + rdsp[rho], x3 + off, rcnt[rho], s, host); off += rcnt[rho]; } }
    if (tk) bcopy_run(p, (char *)rb, x3, m, s);
}
static void complete_v(comm *c)
{
    lay_priv *p = PRIV(c); struct lay_v *v = &p->v;
    if (!v->pend) return;
    comm_wait(v->ic);
    HIP_CHECK(hipSetDevice(p->dev));
    struct vtab t; t.dsp2 = v->cnt2 + 2 * p->g;         /* only dsp2 is needed by the scatter */
    v_scatter(c, &t, v->x3, v->rb, v->rcnt, v->rdsp, v->s, 0);
    HIP_SYNC(v->s);
    free(v->cnt2); v->pend = 0;
}
/* ---- the v-exchange pipelined two deep (two deep, built in)
 * like the equal-slab one: the intra stage and the transpose of exchange k run while exchange k-1's inter stage is on the
 * wire; then k-1 is completed (its inter wait and scatter: "posting k completes k-1" still holds, which gen_inv_pw relies on)
 * and k's inter stage is posted (the inter transport takes one at a time).  The x0 area (the send slabs reordered, only when
 * they are not back to back) is shared; each exchange's x2 (the inter send, A) and x3 (the inter receive, B) are a slot of
 * A + max(A, B) bytes, two slots alternating, each the communicator's own and grown only while not pending.  The intra receive
 * x1 (A) lives in the slot's x3 area: it is dead once reordered into x2, before the inter stage receives into x3.  Scratch:
 * x0 + 2 (A + max(A, B)) against depth 1's x0 + 2 A + B -- with A ~ B one exchange's A more (4 S against 3 S). */
/* S36 (results/GRID_GROUPS.md §S36): the v-slots are one pair per device, shared by every layered communicator of the APU thread.  The tree's levels run one
 * after the other and a node is in one group per level, so a level's slots are dead when the next level's exchange begins: instead of each communicator holding its own
 * pair to the run's end (the sum over the levels), the pair is handed on (the previous owner's pending exchange is completed first) and held at the largest level's size.
 * Only where the buffers live and when they are reused: the exchanges, their order and the data are the same.  The pair is freed with the last communicator. */
static struct vshare { char *vx[2]; size_t cap[2]; comm *owner; int refs; } g_vsh[16];
static void need_vslot(comm *c, int k, size_t bytes);
static void complete_v(comm *c);
static void vslot_attach(lay_priv *p) { if (p->dev < 0 || p->dev >= 16) ec_fatal(EC_RC_FATAL, "comm_layered: device %d outside 0..15\n", p->dev); g_vsh[p->dev].refs++; }
static void need_vslot_shared(comm *c, int k, size_t bytes)
{
    lay_priv *p = PRIV(c); struct vshare *S = &g_vsh[p->dev];
    if (S->owner && S->owner != c) { complete_v(S->owner); }   /* the previous level's last exchange still uses the pair */
    S->owner = c;
    if (S->cap[k] < bytes) {
        HIP_CHECK(hipSetDevice(p->dev));
        size_t minb = 1; { const char *e = getenv("COMM_LAYER_VSLOT_MB"); char nm[48]; snprintf(nm, sizeof nm, "COMM_LAYER_VSLOT_MB_%d", comm_size(p->inter));
                          if (!e) e = getenv(nm); if (e) minb = (size_t)(atof(e) * 1048576.0); }   /* (the pool rule sets every group size to the overall maximum under SHARE) */
        size_t want = bytes > minb ? bytes : minb;
        if (S->vx[k]) { comm_sym_free(p->inter, S->vx[k]); S->vx[k] = 0; S->cap[k] = 0; }
        S->vx[k] = (char *)comm_sym_alloc(p->inter, want);
        if (!S->vx[k]) ec_fatal(EC_RC_FATAL, "comm_layered: the inter transport has no symmetric pool (%zu B v-slot)\n", want);
        S->cap[k] = want;
    }
    p->vx[k] = S->vx[k]; p->vxcap[k] = S->cap[k];          /* (a stale view of the other slot is refreshed too: only the owner's view is used) */
    p->vx[k ^ 1] = S->vx[k ^ 1]; p->vxcap[k ^ 1] = S->cap[k ^ 1];
}
static void need_vslot(comm *c, int k, size_t bytes) { int ph_ = ec_ph(EC_PH_VSLOT_ALLOC); need_vslot_shared(c, k, bytes); ec_ph(ph_); }
/* M3 (always on; results/GRID_GROUPS.md §C06): rns_dist.c calls this when a product's last v-exchange has been waited for, before the CRT and the
 * result exchanges.  The comm pool holds the slots only while a product's v-exchanges run; the staged exchanges around them (the redistribution, the result, the shifted add) use the
 * same pool, so the pool rule sizes it for the larger of the two, not their sum.  The slots are scratch (every exchange writes its x2 / x3 before reading them): nothing carries over;
 * the next product's first v-exchange allocates them again (need_vslot*: at the pool rule's size, so the same size; the receivers publish where a slab lands, so the offset need not match).
 * Digits cannot change. */
static void release_vslots_i(comm *c);
void comm_layered_release_vslots(comm *c) { int ph_ = ec_ph(EC_PH_VSLOT_RELEASE); release_vslots_i(c); ec_ph(ph_); }
static void release_vslots_i(comm *c)
{
    lay_priv *p = PRIV(c);
    complete_v(c);                                       /* (nothing is pending after a product's waits: the pair is never freed under an exchange) */
    if (p->npend) { ec_fatal(EC_RC_FATAL, "comm_layered: v-slot release with an exchange pending\n"); }
    HIP_CHECK(hipSetDevice(p->dev));
    struct vshare *S = &g_vsh[p->dev];
    if (S->owner && S->owner != c) complete_v(S->owner);
    for (int k = 0; k < 2; k++) if (S->vx[k]) { comm_sym_free(p->inter, S->vx[k]); S->vx[k] = 0; S->cap[k] = 0; }
    S->owner = 0;
    p->vx[0] = p->vx[1] = 0; p->vxcap[0] = p->vxcap[1] = 0;
}
static void y_alltoallv2(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    lay_priv *p = PRIV(c);
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 0);
    size_t x0n = t.contig ? 0 : t.S;
    int k = p->v.pend ? p->v.slot ^ 1 : 0;              /* the slot the pending exchange does not hold */
    size_t mab = t.A > t.B ? t.A : t.B;
    if (x0n) need_vtmp(c, x0n + 4);                       /* (not used by the pending exchange) */
    need_vslot(c, k, t.A + mab + 4);
    char *x0 = p->vtmp, *x2 = p->vx[k], *x3 = x2 + t.A, *x1 = x3;   /* x1 in the x3 area (see above) */
    HIP_CHECK(hipSetDevice(p->dev));
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, s, 0);   /* under the pending exchange's inter stage */
    comm *ic = p->inter;
    if (p->i2 && p->inter2) {                             /* X2 COMM_LAYER_INTER2: k posts on its own communicator (slot parity) BEFORE k-1 completes: two inter stages in flight */
        ic = k ? p->inter2 : p->inter;
        v_post(c, ic, &t, x2, x3, s, 0);
        complete_v(c);                                    /* the previous one (p->v still describes it): its inter wait and scatter */
    } else {
        complete_v(c);                                    /* the previous one: its inter wait and scatter */
        v_post(c, ic, &t, x2, x3, s, 0);
    }
    p->v.ic = ic; p->v.rb = rb; p->v.rcnt = rcnt; p->v.rdsp = rdsp; p->v.s = s; p->v.x3 = x3; p->v.cnt2 = t.cnt2; p->v.slot = k; p->v.pend = 1; p->nlog++;
    free(t.T);
}
static void y_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    lay_priv *p = PRIV(c);
    while (p->npend) complete_oldest(c);                 /* one kind at a time */
    y_alltoallv2(c, sb, scnt, sdsp, rb, rcnt, rdsp, s);   /* two deep (two deep, built in) */
}
static void y_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 1);
    size_t x0n = t.contig ? 0 : t.S;
    char *x0 = (char *)malloc(x0n + 2 * t.A + t.B + 4), *x1 = x0 + x0n, *x2 = x1 + t.A, *x3 = x2 + t.A;
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, 0, 1);
    v_post(c, PRIV(c)->inter, &t, x2, x3, 0, 1);
    v_scatter(c, &t, x3, rb, rcnt, rdsp, 0, 1);
    free(x0); vtab_free(&t);
}
static void y_wait(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->nlog) return;
    if (p->nlog <= p->npend + p->v.pend) { if (p->v.pend) complete_v(c); else complete_oldest(c); }   /* else the oldest waited-for exchange was completed early */
    p->nlog--;
}
static void y_barrier(comm *c) { lay_priv *p = PRIV(c); comm_barrier(p->intra); comm_barrier(p->inter); comm_barrier(p->intra); }
static uint64_t y_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)v; (void)q; (void)w; ec_fatal(EC_RC_FATAL, "comm_layered: allreduce_modq not provided\n"); }
static size_t y_max(comm *c, size_t v)
{
    lay_priv *p = PRIV(c);
    size_t m = comm_allreduce_max(p->intra, v);          /* the node's max (every APU thread) */
    m = comm_allreduce_max(p->inter, m);                 /* over the nodes, per mesh: every mesh sees the same values */
    return m;
}
/* all-gather in global rank order rho = g d + r: my block over the nodes into slot d of the result, then the
 * four APUs' slots over xGMI (APU d's slot is already in place: the intra op skips the self copy) */
static void y_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c);
    HIP_CHECK(hipSetDevice(p->dev));
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather(p->inter, sb, slot, bytes);
    comm_allgather(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c);
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather_host(p->inter, sb, slot, bytes);
    comm_allgather_host(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_destroy(comm *c)
{
    lay_priv *p = PRIV(c);
    if ((p->own_tmp && p->tmp) || p->vtmp) HIP_CHECK(hipSetDevice(p->dev));
    if (p->own_tmp && p->tmp) { if (p->tmp_sym) comm_sym_free(p->inter, p->tmp); else HIP_CHECK(hipFree(p->tmp)); }
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    {   /* S36: the shared pair goes with the last communicator of the device */
        struct vshare *S = &g_vsh[p->dev];
        if (S->owner == c) S->owner = 0;
        if (--S->refs <= 0) {
            HIP_CHECK(hipSetDevice(p->dev));
            for (int k = 0; k < 2; k++) if (S->vx[k]) comm_sym_free(p->inter, S->vx[k]);
            memset(S, 0, sizeof *S);
        }
        vlive_add(p->dev, 0, p->vtmp ? p->vcap : 0);
    }
    if (p->ctab) HIP_CHECK(hipHostFree(p->ctab));
    free(p); free(c);
}
/* S12: the callers' symmetric buffers come from the inter transport's pool (the inter stage sends the caller's receive
 * buffer and receives into the scratch; a pool-resident buffer is not staged there) */
static void *y_sym_alloc(comm *c, size_t bytes) { return comm_sym_alloc(PRIV(c)->inter, bytes); }
static void y_sym_free(comm *c, void *p) { comm_sym_free(PRIV(c)->inter, p); }
static const struct comm_ops lay_ops = { y_rank, y_size, y_alltoall, y_wait, y_barrier, y_modq, y_max, y_destroy, 0, 0, y_allgather, y_allgather_host, y_alltoallv, y_alltoallv_host, y_sym_alloc, y_sym_free };
/* COMM_LAYER_INTER2: unset = 1 under libfabric (comm_ofi_planned), 0 under SHMEM (one in flight; user 2026-10-21, results/SHMEM.md); an explicit 0/1 wins; =0 gives one inter mesh.  The v-slots always come from the comm pool; the shared v-slot pair (S36) and the
 * release after each product (M3) are always on.  binsplit.c's sizing, mn.c and rns_dist.c read the same function. */
int comm_layered_inter2_on(void) { const char *e = getenv("COMM_LAYER_INTER2"); return e ? atoi(e) != 0 : comm_ofi_planned(); }
static void lay_opts(lay_priv *p) {
    p->i2 = comm_layered_inter2_on(); vslot_attach(p);   /* S36: the shared pair */
}
static comm *layered_create_i(comm *intra, comm *inter, int d);
comm *comm_layered_create(comm *intra, comm *inter, int d) { int ph_ = ec_ph(EC_PH_LAYER_CREATE); comm *r = layered_create_i(intra, inter, d); ec_ph(ph_); return r; }
static comm *layered_create_i(comm *intra, comm *inter, int d)
{
    if (comm_size(intra) != NA) { ec_fatal(EC_RC_FATAL, "comm_layered: the intra communicator must have %d ranks\n", NA); }
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = d; p->dev = d; p->na = NA; p->g = comm_size(inter);
    c->ops = &lay_ops; c->priv = p; c->size = NA * p->g; c->rank = p->g * d + comm_rank(inter); c->inflight = 2;
    lay_opts(p);
    return c;
}
/* X2: the second inter communicator of this layered communicator (COMM_LAYER_INTER2=1; mn.c creates it over the same PE set under a fresh id) */
void comm_layered_set_inter2(comm *c, comm *inter2) { PRIV(c)->inter2 = inter2; }
/* a device scratch of at least one slab buffer (size x bytes of the largest exchange; two slab buffers of the
 * pipelined exchanges let two stay in flight); the comm's own is freed */
void comm_layered_scratch(comm *c, void *p, size_t bytes)
{
    lay_priv *v = PRIV(c);
    if (v->npend) { ec_fatal(EC_RC_FATAL, "comm_layered: scratch replaced with an exchange pending\n"); }
    if (v->own_tmp && v->tmp) { HIP_CHECK(hipSetDevice(v->dev)); if (v->tmp_sym) comm_sym_free(v->inter, v->tmp); else HIP_CHECK(hipFree(v->tmp)); }
    v->tmp = (char *)p; v->tmp_cap = bytes; v->own_tmp = 0; v->tmp_sym = 0;
}
/* the block transpose alone, one APU: 4 x g blocks of `bytes`, the copies against the kernel (median of reps,
 * host wall incl. the launches and the stream sync, as in the exchange); returns 1 when the two outputs differ */
extern "C" int comm_layered_tbench(int g, size_t bytes, int reps)
{
    size_t tot = bytes * (size_t)NA * g; char *a, *b, *c2; hipStream_t s;
    HIP_CHECK(hipMalloc((void **)&a, tot)); HIP_CHECK(hipMalloc((void **)&b, tot)); HIP_CHECK(hipMalloc((void **)&c2, tot)); HIP_CHECK(hipStreamCreate(&s));
    { unsigned char *h = (unsigned char *)malloc(tot); for (size_t i = 0; i < tot; i++) h[i] = (unsigned char)(i * 2654435761u >> 13); HIP_CHECK(hipMemcpy(a, h, tot, hipMemcpyHostToDevice)); free(h); }
    double tm[2] = { 0, 0 }; int bad = 0;
    double *t = (double *)malloc(reps * sizeof(double));
    for (int form = 0; form < 2; form++) {
        for (int r = 0; r <= reps; r++) {
            double t0 = lay_now();
            if (form) k_btrans<<<dim3(gx_of(bytes >> 4), (unsigned)(NA * g)), 256, 0, s>>>((uint4 *)c2, (const uint4 *)a, bytes >> 4, NA, g);   /* [x][y] (4 x g) -> [y][x] */
            else for (int x = 0; x < NA; x++) for (int y = 0; y < g; y++) HIP_CHECK(hipMemcpyAsync(b + ((size_t)y * NA + x) * bytes, a + ((size_t)x * g + y) * bytes, bytes, hipMemcpyDefault, s));
            HIP_SYNC(s);
            if (r) t[r - 1] = lay_now() - t0;
        }
        qsort(t, reps, sizeof(double), lay_cmp); tm[form] = t[reps / 2];
    }
    free(t);
    { char *h = (char *)malloc(2 * tot); HIP_CHECK(hipMemcpy(h, b, tot, hipMemcpyDeviceToHost)); HIP_CHECK(hipMemcpy(h + tot, c2, tot, hipMemcpyDeviceToHost)); bad = memcmp(h, h + tot, tot) != 0; free(h); }
    printf("tbench g %4d: 4 g = %5d blocks of %8zu B (%9.3f MB): copies %9.3f ms (%.2f us per block, %6.1f GB/s)  kernel %8.3f ms (%6.1f GB/s)  %6.1fx  %s\n", g, NA * g, bytes, tot * 1e-6,
           tm[0] * 1e3, tm[0] * 1e6 / (NA * g), 2.0 * tot / tm[0] * 1e-9, tm[1] * 1e3, 2.0 * tot / tm[1] * 1e-9, tm[0] / tm[1], bad ? "DIFFERS" : "identical");
    fflush(stdout);
    HIP_CHECK(hipFree(a)); HIP_CHECK(hipFree(b)); HIP_CHECK(hipFree(c2)); HIP_CHECK(hipStreamDestroy(s));
    return bad;
}
