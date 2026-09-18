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
static struct { void *rb[NR]; size_t bytes; hipStream_t s[NR]; pthread_barrier_t bar; uint64_t red[NR]; } G;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void g_init(void) { pthread_barrier_init(&G.bar, NULL, NR); }
static int x_rank(comm *c) { return c->rank; }
static int x_size(comm *c) { (void)c; return NR; }
static void x_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    int me = c->rank; G.rb[me] = rb; G.bytes = bytes; G.s[me] = s;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer is known */
    HIP_CHECK(hipSetDevice(me));
    for (int r = 0; r < NR; r++)                         /* my slab r -> rank r's slab me */
        HIP_CHECK(hipMemcpyPeerAsync((char *)G.rb[r] + (size_t)me * bytes, r, (const char *)sb + (size_t)r * bytes, me, bytes, s));
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
