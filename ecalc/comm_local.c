/* comm_local.c - the one-rank communicator: the single-node pipeline's view. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"

#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)

static int l_rank(comm *c) { (void)c; return 0; }
static int l_size(comm *c) { (void)c; return 1; }
static void l_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{ (void)c; if (sb != rb) HIP_CHECK(hipMemcpyAsync(rb, sb, bytes, hipMemcpyDeviceToDevice, s)); }
static void l_wait(comm *c) { (void)c; }
static void l_barrier(comm *c) { (void)c; }
static uint64_t l_allreduce_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)q; (void)w; return v; }
static size_t l_allreduce_max(comm *c, size_t v) { (void)c; return v; }
static void l_destroy(comm *c) { free(c); }
static const struct comm_ops local_ops = { l_rank, l_size, l_alltoall, l_wait, l_barrier, l_allreduce_modq, l_allreduce_max, l_destroy };
comm *comm_local_create(void)
{
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &local_ops; c->rank = 0; c->size = 1;
    return c;
}
