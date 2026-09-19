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
 * (comm_layered_scratch) or hipMalloc'd on first use.  This is M7's hierarchical structure from the start. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NA 4
typedef struct { comm *intra, *inter; int d, g; void *tmp; size_t tmp_cap; int own_tmp;
                 void *rb; size_t bytes; hipStream_t s; int pending; } lay_priv;
#define PRIV(c) ((lay_priv *)(c)->priv)
static int y_rank(comm *c) { return c->rank; }
static int y_size(comm *c) { return c->size; }
static void need_tmp(comm *c, size_t bytes)
{
    lay_priv *p = PRIV(c);
    if (p->tmp_cap >= bytes) return;
    if (!p->own_tmp && p->tmp) { fprintf(stderr, "comm_layered: the caller's scratch (%zu B) is smaller than the exchange (%zu B)\n", p->tmp_cap, bytes); exit(1); }
    if (p->tmp) HIP_CHECK(hipFree(p->tmp));
    HIP_CHECK(hipSetDevice(p->d)); HIP_CHECK(hipMalloc(&p->tmp, bytes)); p->tmp_cap = bytes; p->own_tmp = 1;
}
/* [a][b] blocks of `bytes` -> [b][a]: na x nb blocks */
static void block_transpose(void *dst, const void *src, size_t bytes, int na, int nb, hipStream_t s)
{
    for (int a = 0; a < na; a++) for (int b = 0; b < nb; b++)
        HIP_CHECK(hipMemcpyAsync((char *)dst + ((size_t)b * na + a) * bytes, (const char *)src + ((size_t)a * nb + b) * bytes, bytes, hipMemcpyDeviceToDevice, s));
}
static void y_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    lay_priv *p = PRIV(c); int g = p->g;
    if (p->pending) { fprintf(stderr, "comm_layered: alltoall while one is pending\n"); exit(1); }
    need_tmp(c, bytes * (size_t)NA * g);
    HIP_CHECK(hipSetDevice(p->d));
    /* 1: intra-node, blocks of g slabs; the xGMI wait synchronises the stream and the four threads */
    comm_alltoall(p->intra, sb, p->tmp, bytes * g, s);
    comm_wait(p->intra);
    /* 2: [d][r'] -> [r'][d] into rb, then the inter-node exchange of blocks of NA slabs, received into tmp */
    block_transpose(rb, p->tmp, bytes, NA, g, s);
    HIP_CHECK(hipStreamSynchronize(s));
    comm_alltoall(p->inter, rb, p->tmp, bytes * NA, s);
    p->rb = rb; p->bytes = bytes; p->s = s; p->pending = 1;
}
static void y_wait(comm *c)
{
    lay_priv *p = PRIV(c);
    if (!p->pending) return;
    comm_wait(p->inter);                                 /* tmp holds [r][d] */
    HIP_CHECK(hipSetDevice(p->d));
    block_transpose(p->rb, p->tmp, p->bytes, p->g, NA, p->s);   /* -> [d][r] = source rank order */
    HIP_CHECK(hipStreamSynchronize(p->s));
    p->pending = 0;
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
static void y_destroy(comm *c) { lay_priv *p = PRIV(c); if (p->own_tmp && p->tmp) { HIP_CHECK(hipSetDevice(p->d)); HIP_CHECK(hipFree(p->tmp)); } free(p); free(c); }
static const struct comm_ops lay_ops = { y_rank, y_size, y_alltoall, y_wait, y_barrier, y_modq, y_max, y_destroy };
comm *comm_layered_create(comm *intra, comm *inter, int d)
{
    if (comm_size(intra) != NA) { fprintf(stderr, "comm_layered: the intra communicator must have %d ranks\n", NA); exit(1); }
    comm *c = (comm *)calloc(1, sizeof *c); lay_priv *p = (lay_priv *)calloc(1, sizeof *p);
    p->intra = intra; p->inter = inter; p->d = d; p->g = comm_size(inter);
    c->ops = &lay_ops; c->priv = p; c->size = NA * p->g; c->rank = p->g * d + comm_rank(inter);
    return c;
}
/* a device scratch of at least one slab buffer (size x bytes of the largest exchange); the comm's own is freed */
void comm_layered_scratch(comm *c, void *p, size_t bytes)
{
    lay_priv *v = PRIV(c);
    if (v->own_tmp && v->tmp) { HIP_CHECK(hipSetDevice(v->d)); HIP_CHECK(hipFree(v->tmp)); }
    v->tmp = p; v->tmp_cap = bytes; v->own_tmp = 0;
}
