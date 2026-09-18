/* comm_xgmi.c - four real ranks, one per APU, on one node (WP5).  Rank r is
 * device r.  alltoall: slab s of my send buffer goes to rank s's receive slab r
 * by a peer copy on my stream; wait() synchronises my stream and then the
 * shared barrier, so every rank's receive buffer is complete.  Ranks are driven
 * by four host threads (one per device), as the batch tier does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
static struct { void *rb[NR]; size_t bytes; hipStream_t s[NR]; pthread_barrier_t bar; uint64_t red[NR];
                hipStream_t ps[NR][NR]; hipEvent_t ev[NR][NR], start[NR]; int streams[NR]; } G;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void g_init(void) { pthread_barrier_init(&G.bar, NULL, NR); }
/* push: 16-byte vectors, one kernel per peer on its own stream so the three links run concurrently
 * (a push kernel beats hipMemcpyPeerAsync by 1.67x, RESULTS.md 11; the copies were serialised on one stream) */
struct push3 { const ulonglong2 *src[3]; ulonglong2 *dst[3]; };
/* one kernel drives the three links: block b serves peer b % 3, so the three streams of stores are concurrent */
__global__ void k_push3(struct push3 a, size_t n)
{
    int peer = blockIdx.x % 3; size_t nb = gridDim.x / 3, b = blockIdx.x / 3;
    const ulonglong2 *src = a.src[peer]; ulonglong2 *dst = a.dst[peer];
    size_t i = b * blockDim.x + threadIdx.x, stride = nb * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}
static int x_rank(comm *c) { return c->rank; }
static int x_size(comm *c) { (void)c; return NR; }
static void x_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    int me = c->rank; G.rb[me] = rb; G.bytes = bytes; G.s[me] = s;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer is known */
    HIP_CHECK(hipSetDevice(me));
    if (!G.streams[me]) { for (int r = 0; r < NR; r++) { HIP_CHECK(hipStreamCreateWithFlags(&G.ps[me][r], hipStreamNonBlocking)); HIP_CHECK(hipEventCreateWithFlags(&G.ev[me][r], hipEventDisableTiming)); } HIP_CHECK(hipEventCreateWithFlags(&G.start[me], hipEventDisableTiming)); G.streams[me] = 1; }
    size_t nvec = bytes / 16; struct push3 a; int k = 0;
    for (int r = 0; r < NR; r++) {                       /* my slab r -> rank r's slab me */
        const void *src = (const char *)sb + (size_t)r * bytes; void *dst = (char *)G.rb[r] + (size_t)me * bytes;
        if (r == me) { HIP_CHECK(hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToDevice, s)); continue; }
        a.src[k] = (const ulonglong2 *)src; a.dst[k] = (ulonglong2 *)dst; k++;
    }
    k_push3<<<228 * 3, 256, 0, s>>>(a, nvec);
}
static void x_wait(comm *c)
{
    HIP_CHECK(hipSetDevice(c->rank));
    HIP_CHECK(hipStreamSynchronize(G.s[c->rank]));
    pthread_barrier_wait(&G.bar);                       /* everyone's sends have landed */
}
static void x_barrier(comm *c) { (void)c; pthread_barrier_wait(&G.bar); }
static uint64_t mulmod128(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
static uint64_t x_modq(comm *c, uint64_t v, uint64_t q, uint64_t w)
{
    G.red[c->rank] = v; pthread_barrier_wait(&G.bar);
    uint64_t acc = 0, wr = 1;
    for (int r = 0; r < NR; r++) { acc = (acc + mulmod128(G.red[r] % q, wr, q)) % q; wr = mulmod128(wr, w % q, q); }
    pthread_barrier_wait(&G.bar);
    return acc;
}
static size_t x_max(comm *c, size_t v)
{
    G.red[c->rank] = v; pthread_barrier_wait(&G.bar);
    uint64_t m = 0; for (int r = 0; r < NR; r++) if (G.red[r] > m) m = G.red[r];
    pthread_barrier_wait(&G.bar);
    return (size_t)m;
}
static void x_destroy(comm *c) { free(c); }
static const struct comm_ops xgmi_ops = { x_rank, x_size, x_alltoall, x_wait, x_barrier, x_modq, x_max, x_destroy };
comm *comm_xgmi_create(int rank)
{
    pthread_once(&g_once, g_init);
    HIP_CHECK(hipSetDevice(rank));
    for (int r = 0; r < NR; r++) if (r != rank) { hipError_t e = hipDeviceEnablePeerAccess(r, 0); (void)e; (void)hipGetLastError(); }
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &xgmi_ops; c->rank = rank; c->size = NR;
    return c;
}
