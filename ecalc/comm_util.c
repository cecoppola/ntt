/* comm_util.c - operations built on the transports' ops. comm_allgather: the transport's allgather, else an
 * all-to-all of `size` copies of the block (a device temporary of size x bytes). comm_allgather_host: the transport's
 * host op, else the device op through temporaries. */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
/* MN_WAIT_STATS=1, see comm.h. Waits are timed on the outermost call only (a layered communicator's inner calls run
 * inside the outer one); sums are per phase, integer nanoseconds, atomic across the APU threads. */
#include <time.h>
#include <stdlib.h>
int comm_wst_on = -1;
static int wst_phase = WST_OTHER;
static uint64_t wst_ns[WST_NP][3], wst_nn[WST_NP][3];   /* [phase][0 barrier, 1 wait, 2 ready] */
static __thread int wst_depth;
static double wst_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + 1e-9 * ts.tv_nsec; }
int comm_wst_enabled(void) { if (comm_wst_on < 0) { const char *e = getenv("MN_WAIT_STATS"); comm_wst_on = e && atoi(e) > 0; } return comm_wst_on; }
__thread int comm_xtag = 0;
int comm_wst_set_phase(int p) { return __atomic_exchange_n(&wst_phase, p, __ATOMIC_RELAXED); }
static void wst_add(int k, double t0)
{
    int ph = __atomic_load_n(&wst_phase, __ATOMIC_RELAXED);
    __atomic_fetch_add(&wst_ns[ph][k], (uint64_t)((wst_now() - t0) * 1e9), __ATOMIC_RELAXED);
    __atomic_fetch_add(&wst_nn[ph][k], 1, __ATOMIC_RELAXED);
}
void comm_wst_wait(comm *c)
{
    if (!comm_wst_enabled() || wst_depth) { c->ops->wait(c); return; }
    wst_depth++; double t0 = wst_now(); c->ops->wait(c); wst_add(1, t0); wst_depth--;
}
void comm_wst_barrier(comm *c)
{
    if (!comm_wst_enabled() || wst_depth) { c->ops->barrier(c); return; }
    wst_depth++; double t0 = wst_now(); c->ops->barrier(c); wst_add(0, t0); wst_depth--;
}
/* 'ready' = the peer-ready mailbox/flag waits inside the xshmem transport; a subset of the wait/barrier time, so skew
 * (a peer not yet there) can be told from transfer. comm_wst_t0() is 0 when off; comm_wst_ready_end(t0) adds the
 * elapsed time. */
double comm_wst_t0(void) { return comm_wst_enabled() ? wst_now() : 0; }
void comm_wst_ready_end(double t0) { if (t0 != 0) wst_add(2, t0); }
void comm_wst_totals(uint64_t ns[WST_NP][3], uint64_t n[WST_NP][3])
{
    for (int p = 0; p < WST_NP; p++) for (int k = 0; k < 3; k++) { ns[p][k] = __atomic_load_n(&wst_ns[p][k], __ATOMIC_RELAXED); n[p][k] = __atomic_load_n(&wst_nn[p][k], __ATOMIC_RELAXED); }
}
void comm_allgather(comm *c, const void *sendbuf, void *recvbuf, size_t bytes)
{
    if (c->ops->allgather) { c->ops->allgather(c, sendbuf, recvbuf, bytes); return; }
    int n = comm_size(c);
    if (n == 1) {
        HIP_CHECK(hipMemcpy(recvbuf, sendbuf, bytes, hipMemcpyDefault));
        return;
    }
    void *tmp; HIP_CHECK(hipMalloc(&tmp, (size_t)n * bytes));
    for (int r = 0; r < n; r++) HIP_CHECK(hipMemcpy((char *)tmp + (size_t)r * bytes, sendbuf, bytes, hipMemcpyDefault));
    comm_alltoall(c, tmp, recvbuf, bytes, 0); comm_wait(c);
    HIP_CHECK(hipFree(tmp));
}
void comm_allgather_host(comm *c, const void *sendbuf, void *recvbuf, size_t bytes)
{
    if (c->ops->allgather_host) { c->ops->allgather_host(c, sendbuf, recvbuf, bytes); return; }
    int n = comm_size(c); void *ds, *dr;
    HIP_CHECK(hipMalloc(&ds, bytes)); HIP_CHECK(hipMalloc(&dr, (size_t)n * bytes));
    HIP_CHECK(hipMemcpy(ds, sendbuf, bytes, hipMemcpyHostToDevice));
    comm_allgather(c, ds, dr, bytes);
    HIP_CHECK(hipMemcpy(recvbuf, dr, (size_t)n * bytes, hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
}
/* the unequal all-to-all. Every transport has the device op; the host variant is the transport's, else the device op
 * through temporaries (a receiver's rcnt mismatching the sender's scnt is caught by the transport). */
void comm_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    if (!c->ops->alltoallv) { ec_fatal(EC_RC_FATAL, "comm: alltoallv not provided by this transport\n"); }
    c->ops->alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, s);
}
void comm_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    if (c->ops->alltoallv_host) { c->ops->alltoallv_host(c, sb, scnt, sdsp, rb, rcnt, rdsp); return; }
    int n = comm_size(c); size_t st = 0, rt = 0;
    for (int r = 0; r < n; r++) { size_t e = sdsp[r] + scnt[r]; if (e > st) st = e; e = rdsp[r] + rcnt[r]; if (e > rt) rt = e; }
    void *ds, *dr; HIP_CHECK(hipMalloc(&ds, st ? st : 1)); HIP_CHECK(hipMalloc(&dr, rt ? rt : 1));
    if (st) HIP_CHECK(hipMemcpy(ds, sb, st, hipMemcpyHostToDevice));
    comm_alltoallv(c, ds, scnt, sdsp, dr, rcnt, rdsp, 0); comm_wait(c);
    if (rt) HIP_CHECK(hipMemcpy(rb, dr, rt, hipMemcpyDeviceToHost));
    HIP_CHECK(hipFree(ds)); HIP_CHECK(hipFree(dr));
}

/* GPU copy kernel for copies that touch a host-registered pool: hipMemcpyAsync there runs on the SDMA engine, a
 * kernel at the memory's speed. Used by comm_xshmem.c's COPY. */
#define CC_THREADS 256
#define CC_MAX_BLOCKS 456            /* two waves of the 228 CUs; the grid-stride loop covers the rest */
template <typename T> __global__ void __launch_bounds__(CC_THREADS) k_ccopy(T *d, const T *s, size_t n)
{
    size_t i = (size_t)blockIdx.x * CC_THREADS + threadIdx.x, st = (size_t)gridDim.x * CC_THREADS;
    for (; i + 3 * st < n; i += 4 * st) { T a = s[i], b = s[i + st], c = s[i + 2 * st], e = s[i + 3 * st]; d[i] = a; d[i + st] = b; d[i + 2 * st] = c; d[i + 3 * st] = e; }
    for (; i < n; i += st) d[i] = s[i];
}
__global__ void k_ccopy_tail(unsigned char *d, const unsigned char *s, size_t n) { for (size_t i = threadIdx.x; i < n; i += blockDim.x) d[i] = s[i]; }
template <typename T> static void launch(void *d, const void *s, size_t n, hipStream_t st)   /* n elements of T */
{
    size_t blocks = (n + CC_THREADS - 1) / CC_THREADS; if (blocks > CC_MAX_BLOCKS) blocks = CC_MAX_BLOCKS;
    hipLaunchKernelGGL(k_ccopy<T>, dim3((unsigned)blocks), dim3(CC_THREADS), 0, st, (T *)d, (const T *)s, n);
}
/* d <- s, n bytes, on stream st; the same ordering as hipMemcpyAsync.  Misaligned (not 8 bytes) or tiny copies go to
 * hipMemcpyAsync. */
hipError_t comm_copy_async(void *d, const void *s, size_t n, hipStream_t st)
{
    uintptr_t a = (uintptr_t)d | (uintptr_t)s;
    if (n < 4096 || (a & 7)) return hipMemcpyAsync(d, s, n, hipMemcpyDefault, st);
    if (!(a & 15)) { size_t m = n / 16; launch<uint4>(d, s, m, st); if (n & 15) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 16, (const unsigned char *)s + m * 16, n & 15); }
    else { size_t m = n / 8; launch<uint64_t>(d, s, m, st); if (n & 7) hipLaunchKernelGGL(k_ccopy_tail, dim3(1), dim3(64), 0, st, (unsigned char *)d + m * 8, (const unsigned char *)s + m * 8, n & 7); }
    return hipGetLastError();
}
