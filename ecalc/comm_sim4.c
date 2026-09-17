/* comm_sim4.c - four synthetic ranks inside one APU (WP5 test harness).  The
 * four rank objects share a table; alltoall posts (send, recv) and wait()
 * performs the slab copies once every rank has posted.  Ranks are driven
 * sequentially by one host thread, so no synchronisation is needed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
static struct { const void *sb[NR]; void *rb[NR]; size_t bytes; int posted, done[NR]; hipStream_t s[NR]; } G;
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { (void)c; return NR; }
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    if (G.posted == NR) { G.posted = 0; memset(G.done, 0, sizeof G.done); }
    G.sb[c->rank] = sb; G.rb[c->rank] = rb; G.bytes = bytes; G.s[c->rank] = s; G.posted++;
}
static void s_wait(comm *c)
{
    if (G.posted != NR) { fprintf(stderr, "comm_sim4: wait before all ranks posted\n"); exit(1); }
    if (G.done[c->rank]) return;
    for (int r = 0; r < NR; r++)      /* my slab r comes from rank r's slab c->rank */
        HIP_CHECK(hipMemcpyAsync((char *)G.rb[c->rank] + (size_t)r * G.bytes, (const char *)G.sb[r] + (size_t)c->rank * G.bytes, G.bytes, hipMemcpyDeviceToDevice, G.s[c->rank]));
    HIP_CHECK(hipStreamSynchronize(G.s[c->rank]));
    G.done[c->rank] = 1;
}
static void s_barrier(comm *c) { (void)c; }
static uint64_t s_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)q; (void)w; return v; }
static size_t s_max(comm *c, size_t v) { (void)c; return v; }
static void s_destroy(comm *c) { free(c); }
static const struct comm_ops sim_ops = { s_rank, s_size, s_alltoall, s_wait, s_barrier, s_modq, s_max, s_destroy };
comm *comm_sim4_create(int rank)
{
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &sim_ops; c->rank = rank; c->size = NR;
    return c;
}
