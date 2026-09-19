/* comm_util.c - operations built on the transports' ops (Phase 9, PLAN.md 19).
 * comm_allgather: the transport's allgather when it has one; otherwise an all-to-all of `size` copies of the
 * block (a device temporary of size x bytes) -- correct for every transport, replaced per transport by A-comm. */
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
