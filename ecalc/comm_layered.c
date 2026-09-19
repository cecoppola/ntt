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
 * fabric moves 4 x less than intra-first would. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NA 4
struct lay_ex { void *rb; size_t bytes; hipStream_t s; char *tmp; int inter_posted; };   /* one pending exchange */
typedef struct { comm *intra, *inter; int d, g; char *tmp; size_t tmp_cap; int own_tmp;
                 struct lay_ex ex[2]; int head, npend, nlog; } lay_priv;   /* npend: physically pending; nlog: posted minus waited */
#define PRIV(c) ((lay_priv *)(c)->priv)
static int y_rank(comm *c) { return c->rank; }
static int y_size(comm *c) { return c->size; }
static void need_tmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->tmp_cap >= bytes) return;
    if (!p->own_tmp && p->tmp) { fprintf(stderr, "comm_layered: the caller's scratch (%zu B) is smaller than the exchange (%zu B)\n", p->tmp_cap, bytes); exit(1); }
    if (p->tmp) HIP_CHECK(hipFree(p->tmp));
    HIP_CHECK(hipSetDevice(p->d)); HIP_CHECK(hipMalloc((void **)&p->tmp, bytes)); p->tmp_cap = bytes; p->own_tmp = 1;
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
    comm_alltoall(p->inter, e->rb, e->tmp, e->bytes * NA, e->s);
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
    HIP_CHECK(hipSetDevice(p->d));
    block_transpose(e->rb, e->tmp, e->bytes, p->g, NA, e->s);   /* -> [d][r] = source rank order */
    HIP_CHECK(hipStreamSynchronize(e->s));
    p->head ^= 1; p->npend--;
}
static void y_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    lay_priv *p = PRIV(c); int g = p->g;
    size_t slot = bytes * (size_t)NA * g;
    need_tmp(c, (!p->tmp || p->own_tmp) ? 2 * slot : slot);   /* our own scratch holds two slots; the caller's what it is */
    if (p->npend == 2) complete_oldest(c);
    int two = p->tmp_cap >= 2 * slot;                    /* two slots fit: the previous exchange may stay on the wire */
    if (p->npend && !two) complete_oldest(c);
    struct lay_ex *e = &p->ex[(p->head + p->npend) & 1];
    e->rb = rb; e->bytes = bytes; e->s = s; e->tmp = p->tmp + (two ? ((p->head + p->npend) & 1) * slot : 0); e->inter_posted = 0;
    p->npend++; p->nlog++;
    HIP_CHECK(hipSetDevice(p->d));
    /* 1: intra-node, blocks of g slabs; the xGMI wait synchronises the stream and the four threads */
    comm_alltoall(p->intra, sb, e->tmp, bytes * g, s);
    comm_wait(p->intra);
    /* 2: [d][r'] -> [r'][d] into rb, then the inter-node exchange of blocks of NA slabs, received into the slot */
    block_transpose(rb, e->tmp, bytes, NA, g, s);
    HIP_CHECK(hipStreamSynchronize(s));
    if (p->npend == 2) complete_oldest(c);               /* the inter transport takes one exchange at a time */
    inter_post(c, e);
}
static void y_wait(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->nlog) return;
    if (p->nlog <= p->npend) complete_oldest(c);           /* else the oldest waited-for exchange was completed early */
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
    lay_priv *p = PRIV(c); char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    HIP_CHECK(hipSetDevice(p->d));
    comm_allgather(p->inter, sb, slot, bytes);
    comm_allgather(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    lay_priv *p = PRIV(c); char *slot = (char *)rb + (size_t)p->d * p->g * bytes;
    comm_allgather_host(p->inter, sb, slot, bytes);
    comm_allgather_host(p->intra, slot, rb, bytes * (size_t)p->g);
}
static void y_destroy(comm *c) { lay_priv *p = PRIV(c); if (p->own_tmp && p->tmp) { HIP_CHECK(hipSetDevice(p->d)); HIP_CHECK(hipFree(p->tmp)); } free(p); free(c); }
static const struct comm_ops lay_ops = { y_rank, y_size, y_alltoall, y_wait, y_barrier, y_modq, y_max, y_destroy, 0, 0, y_allgather, y_allgather_host };
comm *comm_layered_create(comm *intra, comm *inter, int d)
{
    if (comm_size(intra) != NA) { fprintf(stderr, "comm_layered: the intra communicator must have %d ranks\n", NA); exit(1); }
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = d; p->g = comm_size(inter);
    c->ops = &lay_ops; c->priv = p; c->size = NA * p->g; c->rank = p->g * d + comm_rank(inter); c->inflight = 2;
    return c;
}
/* a device scratch of at least one slab buffer (size x bytes of the largest exchange; two slab buffers of the
 * pipelined exchanges let two stay in flight); the comm's own is freed */
void comm_layered_scratch(comm *c, void *p, size_t bytes)
{
    lay_priv *v = PRIV(c);
    if (v->npend) { fprintf(stderr, "comm_layered: scratch replaced with an exchange pending\n"); exit(1); }
    if (v->own_tmp && v->tmp) { HIP_CHECK(hipSetDevice(v->d)); HIP_CHECK(hipFree(v->tmp)); }
    v->tmp = (char *)p; v->tmp_cap = bytes; v->own_tmp = 0;
}
