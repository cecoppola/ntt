/* comm_xgmi.c - four real ranks, one per APU, on one node (WP5).  Rank r is
 * device r.  alltoall: slab s of my send buffer goes to rank s's receive slab r
 * by a peer copy on my stream; wait() synchronises my stream and then the
 * shared barrier, so every rank's receive buffer is complete.  Ranks are driven
 * by four host threads (one per device), as the batch tier does.
 * alltoallv (B7): the same pushes with per-peer lengths to the receivers' offsets, the count tables through the shared
 * table (a mismatch aborts).
 * allgather (M7): every rank pushes its one block into the three peers' receive
 * buffers (the same push kernel; a memcpy for blocks that are not 16-byte
 * multiples) and copies its own; complete after the closing barrier.  The host
 * variant is three memcpys from the peers' host blocks between two barriers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
static struct { void *rb[NR]; size_t bytes; hipStream_t s[NR]; pthread_barrier_t bar; uint64_t red[NR];
                hipStream_t ps[NR][NR]; hipEvent_t ev[NR][NR], start[NR]; int streams[NR];
                const void *ag_sb[NR]; void *ag_rb[NR];
                const size_t *scnt[NR], *rcnt[NR], *rdsp[NR]; const void *hsb[NR]; } G;   /* B7: the v-exchange's tables */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_push64, g_push_blocks;   /* COMM_PUSH64 (default 1: 64-bit stores -- measured in results/C.md: the 2^31 convolution -8..-10 % against 16-byte vectors at 228 blocks; RESULTS.md 11 had 909 vs 699 GB/s); COMM_PUSH_BLOCKS: blocks per peer (default 76: fewer blocks leave the CUs to the row pass that runs under the exchange) */
static void g_init(void)
{
    pthread_barrier_init(&G.bar, NULL, NR);
    g_push64 = getenv("COMM_PUSH64") ? atoi(getenv("COMM_PUSH64")) : 1;
    g_push_blocks = getenv("COMM_PUSH_BLOCKS") ? atoi(getenv("COMM_PUSH_BLOCKS")) : 76;
    if (g_push_blocks < 1) g_push_blocks = 1;
}
/* push: one kernel drives the three links -- block b serves peer b % 3, so the three streams of stores are
 * concurrent (a push kernel beats hipMemcpyPeerAsync by 1.67x, RESULTS.md 11); 64-bit words (default), or 16-byte
 * vectors with COMM_PUSH64=0.  Per-peer lengths n[peer] in elements (B7: the unequal exchange; a zero is allowed). */
struct push3 { const void *src[3]; void *dst[3]; size_t n[3]; };
template <typename T> __global__ void k_push3(struct push3 a)
{
    int peer = blockIdx.x % 3; size_t nb = gridDim.x / 3, b = blockIdx.x / 3, n = a.n[peer];
    const T *src = (const T *)a.src[peer]; T *dst = (T *)a.dst[peer];
    size_t i = b * blockDim.x + threadIdx.x, stride = nb * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
}
/* the element width of a push: 16 when every (src, dst, bytes) is a 16-byte multiple, 8 for 8-byte ones, else 0
 * (the transfer goes by memcpy) */
static int push_width(const struct push3 *a, const size_t *bytes)
{
    int w = g_push64 ? 8 : 16;
    for (int k = 0; k < 3; k++) if (bytes[k] && (bytes[k] % w || (uintptr_t)a->src[k] % w || (uintptr_t)a->dst[k] % w)) w = 8;
    for (int k = 0; k < 3; k++) if (bytes[k] && (bytes[k] % 8 || (uintptr_t)a->src[k] % 8 || (uintptr_t)a->dst[k] % 8)) return 0;
    return w;
}
/* the three peers' blocks (bytes[k] each, k = the peer's slot) pushed on stream s */
static void push_run(struct push3 a, const size_t *bytes, hipStream_t s)
{
    int w = push_width(&a, bytes);
    if (!w) { for (int k = 0; k < 3; k++) if (bytes[k]) HIP_CHECK(hipMemcpyAsync(a.dst[k], a.src[k], bytes[k], hipMemcpyDeviceToDevice, s)); return; }
    for (int k = 0; k < 3; k++) a.n[k] = bytes[k] / w;
    if (w == 16) k_push3<ulonglong2><<<g_push_blocks * 3, 256, 0, s>>>(a); else k_push3<uint64_t><<<g_push_blocks * 3, 256, 0, s>>>(a);
}
static int x_rank(comm *c) { return c->rank; }
static int x_size(comm *c) { (void)c; return NR; }
static void streams_init(int me)
{
    if (G.streams[me]) return;
    for (int r = 0; r < NR; r++) { HIP_CHECK(hipStreamCreateWithFlags(&G.ps[me][r], hipStreamNonBlocking)); HIP_CHECK(hipEventCreateWithFlags(&G.ev[me][r], hipEventDisableTiming)); }
    HIP_CHECK(hipEventCreateWithFlags(&G.start[me], hipEventDisableTiming)); G.streams[me] = 1;
}
static void x_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s)
{
    int me = c->rank; G.rb[me] = rb; G.bytes = bytes; G.s[me] = s;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer is known */
    HIP_CHECK(hipSetDevice(me)); streams_init(me);
    struct push3 a; size_t nb[3]; int k = 0;
    for (int r = 0; r < NR; r++) {                       /* my slab r -> rank r's slab me */
        const void *src = (const char *)sb + (size_t)r * bytes; void *dst = (char *)G.rb[r] + (size_t)me * bytes;
        if (r == me) { HIP_CHECK(hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToDevice, s)); continue; }
        a.src[k] = src; a.dst[k] = dst; nb[k] = bytes; k++;
    }
    push_run(a, nb, s);
}
/* B7: the unequal exchange -- the receivers' tables are published, checked against my counts, and my blocks pushed
 * to their offsets (a memcpy where a block is not 8-byte aligned) */
static void x_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    int me = c->rank; G.rb[me] = rb; G.s[me] = s; G.scnt[me] = scnt; G.rcnt[me] = rcnt; G.rdsp[me] = rdsp;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer and table is known */
    HIP_CHECK(hipSetDevice(me)); streams_init(me);
    struct push3 a; size_t nb[3]; int k = 0;
    for (int r = 0; r < NR; r++) {
        if (scnt[r] != G.rcnt[r][me]) { fprintf(stderr, "comm_xgmi: alltoallv count mismatch: rank %d sends %zu to rank %d, which expects %zu\n", me, scnt[r], r, G.rcnt[r][me]); exit(1); }
        const void *src = (const char *)sb + sdsp[r]; void *dst = (char *)G.rb[r] + G.rdsp[r][me];
        if (r == me) { if (scnt[r] && src != dst) HIP_CHECK(hipMemcpyAsync(dst, src, scnt[r], hipMemcpyDeviceToDevice, s)); continue; }
        a.src[k] = src; a.dst[k] = dst; nb[k] = scnt[r]; k++;
    }
    push_run(a, nb, s);
}
static void x_wait(comm *c)
{
    HIP_CHECK(hipSetDevice(c->rank));
    HIP_CHECK(hipStreamSynchronize(G.s[c->rank]));
    pthread_barrier_wait(&G.bar);                       /* everyone's sends have landed */
}
static void x_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    int me = c->rank; G.hsb[me] = sb; G.scnt[me] = scnt; G.rdsp[me] = sdsp;   /* rdsp slot: the senders' offsets */
    pthread_barrier_wait(&G.bar);
    for (int r = 0; r < NR; r++) {
        size_t n = G.scnt[r][me];
        if (n != rcnt[r]) { fprintf(stderr, "comm_xgmi: alltoallv_host count mismatch (%d -> %d: %zu vs %zu)\n", r, me, n, rcnt[r]); exit(1); }
        if (n) memmove((char *)rb + rdsp[r], (const char *)G.hsb[r] + G.rdsp[r][me], n);
    }
    pthread_barrier_wait(&G.bar);                       /* nobody's send buffer is reused before every read */
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
static void x_allgather(comm *c, const void *sb, void *rb, size_t bytes)
{
    int me = c->rank; G.ag_sb[me] = sb; G.ag_rb[me] = rb;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer is known */
    HIP_CHECK(hipSetDevice(me)); streams_init(me);
    hipStream_t s = G.ps[me][me];
    void *self = (char *)rb + (size_t)me * bytes;
    if (self != sb) HIP_CHECK(hipMemcpyAsync(self, sb, bytes, hipMemcpyDeviceToDevice, s));
    { struct push3 a; size_t nb[3]; int k = 0;
      for (int r = 0; r < NR; r++) if (r != me) { a.src[k] = sb; a.dst[k] = (char *)G.ag_rb[r] + (size_t)me * bytes; nb[k] = bytes; k++; }
      push_run(a, nb, s); }
    HIP_CHECK(hipStreamSynchronize(s));
    pthread_barrier_wait(&G.bar);                       /* everyone's block has landed */
}
static void x_allgather_host(comm *c, const void *sb, void *rb, size_t bytes)
{
    int me = c->rank; G.ag_sb[me] = sb;
    pthread_barrier_wait(&G.bar);
    for (int r = 0; r < NR; r++) { void *dst = (char *)rb + (size_t)r * bytes; if (dst != G.ag_sb[r]) memcpy(dst, G.ag_sb[r], bytes); }
    pthread_barrier_wait(&G.bar);                       /* nobody's send block is reused before every read */
}
static const struct comm_ops xgmi_ops = { x_rank, x_size, x_alltoall, x_wait, x_barrier, x_modq, x_max, x_destroy, 0, 0, x_allgather, x_allgather_host, x_alltoallv, x_alltoallv_host };
comm *comm_xgmi_create(int rank)
{
    pthread_once(&g_once, g_init);
    HIP_CHECK(hipSetDevice(rank));
    for (int r = 0; r < NR; r++) if (r != rank) { hipError_t e = hipDeviceEnablePeerAccess(r, 0); (void)e; (void)hipGetLastError(); }
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &xgmi_ops; c->rank = rank; c->size = NR; c->inflight = 1;
    return c;
}
