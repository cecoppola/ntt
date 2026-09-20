/* comm_util.c - operations built on the transports' ops (Phase 9, PLAN.md 19).
 * comm_allgather: the transport's allgather (every transport has one since M7); otherwise an all-to-all of `size`
 * copies of the block (a device temporary of size x bytes).  comm_allgather_host: the transport's host op, else
 * the device op through temporaries. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#ifndef COMM_HOST_ONLY
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#endif
void comm_allgather(comm *c, const void *sendbuf, void *recvbuf, size_t bytes)
{
    if (c->ops->allgather) { c->ops->allgather(c, sendbuf, recvbuf, bytes); return; }
    int n = comm_size(c);
    if (n == 1) {
#ifndef COMM_HOST_ONLY
        HIP_CHECK(hipMemcpy(recvbuf, sendbuf, bytes, hipMemcpyDefault));
#else
        memcpy(recvbuf, sendbuf, bytes);
#endif
        return;
    }
#ifndef COMM_HOST_ONLY
    void *tmp; HIP_CHECK(hipMalloc(&tmp, (size_t)n * bytes));
    for (int r = 0; r < n; r++) HIP_CHECK(hipMemcpy((char *)tmp + (size_t)r * bytes, sendbuf, bytes, hipMemcpyDefault));
    comm_alltoall(c, tmp, recvbuf, bytes, 0); comm_wait(c);
    HIP_CHECK(hipFree(tmp));
#else
    char *tmp = (char *)malloc((size_t)n * bytes);
    for (int r = 0; r < n; r++) memcpy(tmp + (size_t)r * bytes, sendbuf, bytes);
    comm_alltoall(c, tmp, recvbuf, bytes, 0); comm_wait(c);
    free(tmp);
#endif
}
void comm_allgather_host(comm *c, const void *sendbuf, void *recvbuf, size_t bytes)
{
    if (c->ops->allgather_host) { c->ops->allgather_host(c, sendbuf, recvbuf, bytes); return; }
#ifndef COMM_HOST_ONLY
    int n = comm_size(c); void *ds, *dr;
    HIP_CHECK(hipMalloc(&ds, bytes)); HIP_CHECK(hipMalloc(&dr, (size_t)n * bytes));
    HIP_CHECK(hipMemcpy(ds, sendbuf, bytes, hipMemcpyHostToDevice));
    comm_allgather(c, ds, dr, bytes);
    HIP_CHECK(hipMemcpy(recvbuf, dr, (size_t)n * bytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
#else
    comm_allgather(c, sendbuf, recvbuf, bytes);
#endif
}
/* B7: the unequal all-to-all.  Every transport has the device op; the host variant is the transport's, else the
 * device op through temporaries (a mismatch of a receiver's rcnt with the sender's scnt is caught by the transport). */
void comm_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    if (!c->ops->alltoallv) { fprintf(stderr, "comm: alltoallv not provided by this transport\n"); exit(1); }
    c->ops->alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, s);
}
void comm_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    if (c->ops->alltoallv_host) { c->ops->alltoallv_host(c, sb, scnt, sdsp, rb, rcnt, rdsp); return; }
#ifndef COMM_HOST_ONLY
    int n = comm_size(c); size_t st = 0, rt = 0;
    for (int r = 0; r < n; r++) { size_t e = sdsp[r] + scnt[r]; if (e > st) st = e; e = rdsp[r] + rcnt[r]; if (e > rt) rt = e; }
    void *ds, *dr; HIP_CHECK(hipMalloc(&ds, st ? st : 1)); HIP_CHECK(hipMalloc(&dr, rt ? rt : 1));
    if (st) HIP_CHECK(hipMemcpy(ds, sb, st, hipMemcpyHostToDevice));
    comm_alltoallv(c, ds, scnt, sdsp, dr, rcnt, rdsp, 0); comm_wait(c);
    if (rt) HIP_CHECK(hipMemcpy(rb, dr, rt, hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
#else
    comm_alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, 0); comm_wait(c);
#endif
}
