/* comm_local.c - the one-rank communicator: the single-node pipeline's view. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

static int l_rank(comm *c) { (void)c; return 0; }
static int l_size(comm *c) { (void)c; return 1; }
/* the copy runs on the caller's stream; wait() synchronises that stream (M7: the slab pipeline posts on a
 * transfer stream and consumes on the compute stream, so the wait must be real) */
static void l_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{ c->priv = (void *)s; if (sb != rb) HIP_CHECK(hipMemcpyAsync(rb, sb, bytes, hipMemcpyDeviceToDevice, s)); }
static void l_wait(comm *c) { HIP_CHECK(hipStreamSynchronize((hipStream_t)c->priv)); }
static void l_barrier(comm *c) { (void)c; }
static uint64_t l_allreduce_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)q; (void)w; return v; }
static size_t l_allreduce_max(comm *c, size_t v) { (void)c; return v; }
static void l_destroy(comm *c) { free(c); }
static void l_allgather(comm *c, const void *sb, void *rb, size_t bytes) { (void)c; if (sb != rb) HIP_CHECK(hipMemcpy(rb, sb, bytes, hipMemcpyDeviceToDevice)); }
static void l_allgather_host(comm *c, const void *sb, void *rb, size_t bytes) { (void)c; if (sb != rb) memcpy(rb, sb, bytes); }
static void l_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    if (scnt[0] != rcnt[0]) { fprintf(stderr, "comm_local: alltoallv count mismatch (%zu sent, %zu expected)\n", scnt[0], rcnt[0]); exit(1); }
    c->priv = (void *)s;
    if (scnt[0] && (const char *)sb + sdsp[0] != (char *)rb + rdsp[0]) HIP_CHECK(hipMemcpyAsync((char *)rb + rdsp[0], (const char *)sb + sdsp[0], scnt[0], hipMemcpyDeviceToDevice, s));
}
static void l_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    (void)c; if (scnt[0] != rcnt[0]) { fprintf(stderr, "comm_local: alltoallv count mismatch\n"); exit(1); }
    if (scnt[0]) memmove((char *)rb + rdsp[0], (const char *)sb + sdsp[0], scnt[0]);
}
static const struct comm_ops local_ops = { l_rank, l_size, l_alltoall, l_wait, l_barrier, l_allreduce_modq, l_allreduce_max, l_destroy, 0, 0, l_allgather, l_allgather_host, l_alltoallv, l_alltoallv_host };
comm *comm_local_create(void)
{
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &local_ops; c->rank = 0; c->size = 1; c->inflight = 1;
    return c;
}
