/* comm_layered.c - the layered communicator (Phase 8 M3, PLAN.md 17): the rank space of a whole-machine
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
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NA 4
struct lay_ex { void *rb; size_t bytes; hipStream_t s; char *tmp; int inter_posted; };   /* one pending exchange */
struct lay_v { void *rb; const size_t *rcnt, *rdsp; hipStream_t s; char *x3; size_t *cnt2; int pend; };   /* the pending v-exchange */
typedef struct { comm *intra, *inter; int d, g, na, dev, minor; char *tmp; size_t tmp_cap; int own_tmp;   /* d: my intra rank; dev: my device; na: intra size; minor: rho = na r + d (else g d + r) */
                 struct lay_ex ex[2]; int head, npend, nlog;                /* npend: physically pending; nlog: posted minus waited */
                 char *vtmp; size_t vcap; struct lay_v v; } lay_priv;
#define PRIV(c) ((lay_priv *)(c)->priv)
static inline int rho_of(const lay_priv *p, int d, int r) { return p->minor ? p->na * r + d : p->g * d + r; }   /* the global rank of (intra d, inter r) */
static int y_rank(comm *c) { return c->rank; }
static int y_size(comm *c) { return c->size; }
static void need_tmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->tmp_cap >= bytes) return;
    if (!p->own_tmp && p->tmp) { fprintf(stderr, "comm_layered: the caller's scratch (%zu B) is smaller than the exchange (%zu B)\n", p->tmp_cap, bytes); exit(1); }
    if (p->tmp) HIP_CHECK(hipFree(p->tmp));
    HIP_CHECK(hipSetDevice(p->dev)); HIP_CHECK(hipMalloc((void **)&p->tmp, bytes)); p->tmp_cap = bytes; p->own_tmp = 1;
}
/* [a][b] blocks of `bytes` -> [b][a]: na x nb blocks */
static void block_transpose(void *dst, const void *src, size_t bytes, int na, int nb, hipStream_t s)
{
    for (int a = 0; a < na; a++) for (int b = 0; b < nb; b++)
        HIP_CHECK(hipMemcpyAsync((char *)dst + ((size_t)b * na + a) * bytes, (const char *)src + ((size_t)a * nb + b) * bytes, bytes, hipMemcpyDeviceToDevice, s));
}
/* the inter-node stage of the oldest pending exchange (its intra stage and transpose are done) */
static void inter_post(comm *c, struct lay_ex *e)
{
    lay_priv *p = PRIV(c);
    if (p->minor) comm_alltoall(p->inter, e->tmp, e->rb, e->bytes * p->na, e->s);   /* minor: the transposed scratch out, rb receives [r][d] = rank order */
    else comm_alltoall(p->inter, e->rb, e->tmp, e->bytes * p->na, e->s);
    e->inter_posted = 1;
}
/* complete the oldest pending exchange: tmp slot holds [r][d] after the inter wait; -> [d][r] into rb (major); minor: rb is final */
static void complete_oldest(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->npend) return;
    struct lay_ex *e = &p->ex[p->head];
    if (!e->inter_posted) inter_post(c, e);
    comm_wait(p->inter);
    HIP_CHECK(hipSetDevice(p->dev));
    if (!p->minor) { block_transpose(e->rb, e->tmp, e->bytes, p->g, p->na, e->s); HIP_CHECK(hipStreamSynchronize(e->s)); }   /* -> [d][r] = source rank order */
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
    if (p->minor) {
        /* minor: the slabs for intra peer d' are {na r' + d'}: gathered into the scratch as [d'][r'] blocks of g slabs; the intra
         * exchange into rb ([d''][r']); the transpose into the scratch ([r'][d'']); the inter exchange sends its blocks of na */
        block_transpose(e->tmp, sb, bytes, g, na, s); HIP_CHECK(hipStreamSynchronize(s));
        comm_alltoall(p->intra, e->tmp, rb, bytes * g, s); comm_wait(p->intra);
        block_transpose(e->tmp, rb, bytes, na, g, s); HIP_CHECK(hipStreamSynchronize(s));
    } else {
    /* 1: intra-node, blocks of g slabs; the xGMI wait synchronises the stream and the four threads */
    comm_alltoall(p->intra, sb, e->tmp, bytes * g, s);
    comm_wait(p->intra);
    /* 2: [d][r'] -> [r'][d] into rb, then the inter-node exchange of blocks of na slabs, received into the slot */
    block_transpose(rb, e->tmp, bytes, na, g, s);
    HIP_CHECK(hipStreamSynchronize(s));
    }
    if (p->npend == 2) complete_oldest(c);               /* the inter transport takes one exchange at a time */
    inter_post(c, e);
}
/* ---- B7: the unequal exchange ---- */
static void need_vtmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->vcap >= bytes) return;
    HIP_CHECK(hipSetDevice(p->dev));
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    HIP_CHECK(hipMalloc((void **)&p->vtmp, bytes)); p->vcap = bytes;
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
    t->A = t->B = t->S = 0; t->contig = !p->minor;      /* minor: the slabs for one intra peer are strided in rank order: always packed */
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
    if (host) memmove(dst, src, n); else HIP_CHECK(hipMemcpyAsync(dst, src, n, hipMemcpyDeviceToDevice, s));
}
/* the intra stage and the transpose: my slabs -> x1 [d][r'] blocks (the intra exchange) -> x2 [r'][d]; then the inter
 * stage's post.  x1/x2/x3 are the caller's areas (device or host); the intra and inter ops are the transport's of the kind. */
static void v_stages(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, struct vtab *t, char *x0, char *x1, char *x2, char *x3, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g, na = p->na, n = na * g, d = p->d;
    const char *src = (const char *)sb; size_t soff0 = sdsp[0];
    if (!t->contig) {                                    /* the slabs into [d'][r'] order (= rank order in the major form), back to back, in x0 */
        size_t o = 0; for (int dd = 0; dd < na; dd++) for (int r = 0; r < g; r++) { int rho = rho_of(p, dd, r); vcopy(x0 + o, (const char *)sb + sdsp[rho], scnt[rho], s, host); o += scnt[rho]; }
        src = x0; soff0 = 0;
    }
    size_t *sdI = (size_t *)malloc(2 * (size_t)na * sizeof(size_t)), *rdI = sdI + na; for (int dd = 0; dd < na; dd++) { sdI[dd] = soff0 + t->sIoff[dd]; rdI[dd] = t->rIoff[dd]; }
    if (host) comm_alltoallv_host(p->intra, src, t->sI, sdI, x1, t->rI, rdI);
    else { HIP_CHECK(hipStreamSynchronize(s)); comm_alltoallv(p->intra, src, t->sI, sdI, x1, t->rI, rdI, s); comm_wait(p->intra); }
    free(sdI);
    /* x1: block dd = slabs (dd -> (d, r')) for r' = 0..g-1 back to back; x2: node r' = its na slabs dd = 0..na-1 */
    size_t o2 = 0;
    for (int r = 0; r < g; r++) for (int dd = 0; dd < na; dd++) {
        size_t off = t->rIoff[dd]; for (int rr = 0; rr < r; rr++) off += t->T[(size_t)dd * n + rho_of(p, d, rr)];
        size_t len = t->T[(size_t)dd * n + rho_of(p, d, r)];
        vcopy(x2 + o2, x1 + off, len, s, host); o2 += len;
    }
    if (host) comm_alltoallv_host(p->inter, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g);
    else { HIP_CHECK(hipStreamSynchronize(s)); comm_alltoallv(p->inter, x2, t->cnt2, t->dsp2, x3, t->cnt2 + g, t->dsp2 + g, s); }
}
/* x3 [r][d] -> the receive buffer at the caller's offsets */
static void v_scatter(comm *c, const struct vtab *t, const char *x3, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s, int host)
{
    lay_priv *p = PRIV(c); int g = p->g;
    for (int r = 0; r < g; r++) { size_t off = t->dsp2[g + r]; for (int dd = 0; dd < p->na; dd++) { int rho = rho_of(p, dd, r); vcopy((char *)rb + rdsp[rho], x3 + off, rcnt[rho], s, host); off += rcnt[rho]; } }
}
static void complete_v(comm *c)
{
    lay_priv *p = PRIV(c); struct lay_v *v = &p->v;
    if (!v->pend) return;
    comm_wait(p->inter);
    HIP_CHECK(hipSetDevice(p->dev));
    struct vtab t; t.dsp2 = v->cnt2 + 2 * p->g;         /* only dsp2 is needed by the scatter */
    v_scatter(c, &t, v->x3, v->rb, v->rcnt, v->rdsp, v->s, 0);
    HIP_CHECK(hipStreamSynchronize(v->s));
    free(v->cnt2); v->pend = 0;
}
static void y_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    lay_priv *p = PRIV(c);
    while (p->npend) complete_oldest(c);                 /* one kind at a time */
    complete_v(c);
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 0);
    size_t x0n = t.contig ? 0 : t.S;
    need_vtmp(c, x0n + 2 * t.A + t.B + 4);
    char *x0 = p->vtmp, *x1 = x0 + x0n, *x2 = x1 + t.A, *x3 = x2 + t.A;
    HIP_CHECK(hipSetDevice(p->dev));
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, s, 0);
    p->v.rb = rb; p->v.rcnt = rcnt; p->v.rdsp = rdsp; p->v.s = s; p->v.x3 = x3; p->v.cnt2 = t.cnt2; p->v.pend = 1; p->nlog++;
    free(t.T);
}
static void y_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    struct vtab t; vtab_build(c, scnt, sdsp, rcnt, &t, 1);
    size_t x0n = t.contig ? 0 : t.S;
    char *x0 = (char *)malloc(x0n + 2 * t.A + t.B + 4), *x1 = x0 + x0n, *x2 = x1 + t.A, *x3 = x2 + t.A;
    v_stages(c, sb, scnt, sdsp, &t, x0, x1, x2, x3, 0, 1);
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
static uint64_t y_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)v; (void)q; (void)w; fprintf(stderr, "comm_layered: allreduce_modq not provided\n"); exit(1); }
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
    if (p->minor) {                                      /* the group's na slots are contiguous: intra first, then the group blocks over the inter comm */
        char *slot = (char *)rb + (size_t)p->na * comm_rank(p->inter) * bytes;
        comm_allgather(p->intra, sb, slot, bytes);
        comm_allgather(p->inter, slot, rb, bytes * (size_t)p->na);
        return;
    }
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather(p->inter, sb, slot, bytes);
    comm_allgather(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->minor) {
        char *slot = (char *)rb + (size_t)p->na * comm_rank(p->inter) * bytes;
        comm_allgather_host(p->intra, sb, slot, bytes);
        comm_allgather_host(p->inter, slot, rb, bytes * (size_t)p->na);
        return;
    }
    char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather_host(p->inter, sb, slot, bytes);
    comm_allgather_host(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_destroy(comm *c)
{
    lay_priv *p = PRIV(c);
    if ((p->own_tmp && p->tmp) || p->vtmp) HIP_CHECK(hipSetDevice(p->dev));
    if (p->own_tmp && p->tmp) HIP_CHECK(hipFree(p->tmp));
    if (p->vtmp) HIP_CHECK(hipFree(p->vtmp));
    free(p); free(c);
}
static const struct comm_ops lay_ops = { y_rank, y_size, y_alltoall, y_wait, y_barrier, y_modq, y_max, y_destroy, 0, 0, y_allgather, y_allgather_host, y_alltoallv, y_alltoallv_host };
comm *comm_layered_create(comm *intra, comm *inter, int d)
{
    if (comm_size(intra) != NA) { fprintf(stderr, "comm_layered: the intra communicator must have %d ranks\n", NA); exit(1); }
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = d; p->dev = d; p->na = NA; p->g = comm_size(inter);
    c->ops = &lay_ops; c->priv = p; c->size = NA * p->g; c->rank = p->g * d + comm_rank(inter); c->inflight = 2;
    return c;
}
/* S: the intra-minor form on device dev: rank = size(intra) x rank(inter) + rank(intra) (the dragonfly's third layer: intra
 * = the nodes of my group, inter = my in-group index's nodes across the groups; rank = node) */
comm *comm_layered_create_minor(comm *intra, comm *inter, int dev)
{
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = comm_rank(intra); p->dev = dev; p->na = comm_size(intra); p->g = comm_size(inter); p->minor = 1;
    c->ops = &lay_ops; c->priv = p; c->size = p->na * p->g; c->rank = p->na * comm_rank(inter) + p->d; c->inflight = 2;
    return c;
}
/* a device scratch of at least one slab buffer (size x bytes of the largest exchange; two slab buffers of the
 * pipelined exchanges let two stay in flight); the comm's own is freed */
void comm_layered_scratch(comm *c, void *p, size_t bytes)
{
    lay_priv *v = PRIV(c);
    if (v->npend) { fprintf(stderr, "comm_layered: scratch replaced with an exchange pending\n"); exit(1); }
    if (v->own_tmp && v->tmp) { HIP_CHECK(hipSetDevice(v->dev)); HIP_CHECK(hipFree(v->tmp)); }
    v->tmp = (char *)p; v->tmp_cap = bytes; v->own_tmp = 0;
}
