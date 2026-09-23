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
#include <time.h>
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
/* Phase 13a X (PLAN.md 29 H4): the same push with each block's start and end stamped (wall_clock64, 100 MHz), so the time
 * each link (= each peer's blocks) is active can be read back.  map 0: block b serves peer b % 3 (the push's own order);
 * map 1: contiguous thirds of the grid (the H4 ordering variant).  COMM_XGMI_STATS=1 runs the exchanges through it. */
template <typename T> __global__ void k_push3_ts(struct push3 a, unsigned long long *ts, int map)
{
    size_t nb = gridDim.x / 3; int peer = map ? (int)(blockIdx.x / nb) : blockIdx.x % 3; size_t b = map ? blockIdx.x % nb : blockIdx.x / 3, n = a.n[peer];
    if (threadIdx.x == 0) ts[2 * blockIdx.x] = wall_clock64();
    const T *src = (const T *)a.src[peer]; T *dst = (T *)a.dst[peer];
    size_t i = b * blockDim.x + threadIdx.x, stride = nb * blockDim.x;
    for (; i < n; i += stride) dst[i] = src[i];
    __syncthreads();
    if (threadIdx.x == 0) { __threadfence_system(); ts[2 * blockIdx.x + 1] = wall_clock64(); }
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
static void h4_launch(int me, struct push3 a, const size_t *bytes, int w, int blocks, int map, hipStream_t s, const int *peer);
static int xstats_on(void);
static void push_run(struct push3 a, const size_t *bytes, hipStream_t s, int me = -1, const int *peer = 0)   /* me, peer: the H4 stamps (COMM_XGMI_STATS) */
{
    int w = push_width(&a, bytes);
    if (!w) { for (int k = 0; k < 3; k++) if (bytes[k]) HIP_CHECK(hipMemcpyAsync(a.dst[k], a.src[k], bytes[k], hipMemcpyDeviceToDevice, s)); return; }
    for (int k = 0; k < 3; k++) a.n[k] = bytes[k] / w;
    if (me >= 0 && xstats_on()) { h4_launch(me, a, bytes, w, g_push_blocks, 0, s, peer); return; }
    if (w == 16) k_push3<ulonglong2><<<g_push_blocks * 3, 256, 0, s>>>(a); else k_push3<uint64_t><<<g_push_blocks * 3, 256, 0, s>>>(a);
}
/* ---- H4: per-link bytes and active time (COMM_XGMI_STATS=1) ---- */
static int g_xstats = -1;
static struct { unsigned long long *d_ts[NR], *h_ts[NR]; int nts[NR], pend[NR], map[NR], peer[NR][3]; size_t pb[NR][3]; double khz;
                double bytes[NR][NR], active[NR][NR], kbytes[NR], kspan[NR], n[NR]; } H;
static pthread_mutex_t h_mx = PTHREAD_MUTEX_INITIALIZER;
static void h4_print(void)
{
    printf("xgmi-stats (the push's blocks stamped; per link = the time any of its blocks ran): per sender -> peer: GB, active s, GB/s\n");
    for (int me = 0; me < NR; me++) {
        if (!H.n[me]) continue;
        printf("xgmi-stats  APU %d: %.0f pushes, %.3f GB in %.4f s of kernel = %.1f GB/s aggregate |", me, H.n[me], H.kbytes[me] * 1e-9, H.kspan[me], H.kspan[me] > 0 ? H.kbytes[me] / H.kspan[me] * 1e-9 : 0);
        for (int r = 0; r < NR; r++) if (r != me) printf("  ->%d %.3f GB %.4f s %.1f GB/s", r, H.bytes[me][r] * 1e-9, H.active[me][r], H.active[me][r] > 0 ? H.bytes[me][r] / H.active[me][r] * 1e-9 : 0);
        printf("\n");
    }
    fflush(stdout);
}
static int xstats_on(void)
{
    if (g_xstats < 0) { pthread_mutex_lock(&h_mx); if (g_xstats < 0) { const char *e = getenv("COMM_XGMI_STATS"); int v = e ? atoi(e) : 0; if (v) atexit(h4_print); g_xstats = v; } pthread_mutex_unlock(&h_mx); }
    return g_xstats;
}
/* read back the stamps of rank me's last stamped push (its stream is synchronised) and add them to the per-link totals */
static void h4_collect(int me)
{
    if (!H.pend[me]) return;
    int nt = H.nts[me], nb = nt / 3;
    HIP_CHECK(hipMemcpy(H.h_ts[me], H.d_ts[me], 2 * (size_t)nt * sizeof(unsigned long long), hipMemcpyDeviceToHost));
    unsigned long long lo[NR] = { ~0ull, ~0ull, ~0ull, ~0ull }, hi[NR] = { 0, 0, 0, 0 }, klo = ~0ull, khi = 0;   /* per peer (several slots may serve one) */
    for (int b = 0; b < nt; b++) { int k = H.map[me] ? b / nb : b % 3; if (!H.pb[me][k]) continue; int r = H.peer[me][k]; unsigned long long a = H.h_ts[me][2 * b], e = H.h_ts[me][2 * b + 1];
        if (a < lo[r]) lo[r] = a; if (e > hi[r]) hi[r] = e; if (a < klo) klo = a; if (e > khi) khi = e; }
    double f = 1.0 / (H.khz * 1e3); size_t kb = 0;
    for (int k = 0; k < 3; k++) if (H.pb[me][k]) { H.bytes[me][H.peer[me][k]] += H.pb[me][k]; kb += H.pb[me][k]; }
    for (int r = 0; r < NR; r++) if (hi[r]) H.active[me][r] += (hi[r] - lo[r]) * f;
    if (!kb) klo = khi = 0;
    H.kbytes[me] += kb; H.kspan[me] += (khi - klo) * f; H.n[me] += 1; H.pend[me] = 0;
}
static void h4_launch(int me, struct push3 a, const size_t *bytes, int w, int blocks, int map, hipStream_t s, const int *peer)
{
    int nt = blocks * 3;
    if (H.nts[me] < nt) {
        if (H.d_ts[me]) { HIP_CHECK(hipFree(H.d_ts[me])); free(H.h_ts[me]); }
        HIP_CHECK(hipMalloc(&H.d_ts[me], 2 * (size_t)nt * sizeof(unsigned long long))); H.h_ts[me] = (unsigned long long *)malloc(2 * (size_t)nt * sizeof(unsigned long long));
        if (!H.khz) { int khz = 0; HIP_CHECK(hipDeviceGetAttribute(&khz, hipDeviceAttributeWallClockRate, me)); H.khz = khz ? khz : 100000; }
    }
    H.nts[me] = nt;
    for (int k = 0; k < 3; k++) { H.pb[me][k] = bytes[k]; H.peer[me][k] = peer[k]; }
    if (w == 16) k_push3_ts<ulonglong2><<<nt, 256, 0, s>>>(a, H.d_ts[me], map); else k_push3_ts<uint64_t><<<nt, 256, 0, s>>>(a, H.d_ts[me], map);
    H.pend[me] = 1; H.map[me] = map;
}
static double lst_now_x(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + 1e-9 * t.tv_nsec; }
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
    struct push3 a; size_t nb[3]; int k = 0, pr[3];
    for (int r = 0; r < NR; r++) {                       /* my slab r -> rank r's slab me */
        const void *src = (const char *)sb + (size_t)r * bytes; void *dst = (char *)G.rb[r] + (size_t)me * bytes;
        if (r == me) { HIP_CHECK(hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToDevice, s)); continue; }
        a.src[k] = src; a.dst[k] = dst; nb[k] = bytes; pr[k] = r; k++;
    }
    push_run(a, nb, s, me, pr);
}
/* B7: the unequal exchange -- the receivers' tables are published, checked against my counts, and my blocks pushed
 * to their offsets (a memcpy where a block is not 8-byte aligned) */
static void x_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    int me = c->rank; G.rb[me] = rb; G.s[me] = s; G.scnt[me] = scnt; G.rcnt[me] = rcnt; G.rdsp[me] = rdsp;
    pthread_barrier_wait(&G.bar);                       /* every receive buffer and table is known */
    HIP_CHECK(hipSetDevice(me)); streams_init(me);
    struct push3 a; size_t nb[3]; int k = 0, pr[3];
    for (int r = 0; r < NR; r++) {
        if (scnt[r] != G.rcnt[r][me]) { fprintf(stderr, "comm_xgmi: alltoallv count mismatch: rank %d sends %zu to rank %d, which expects %zu\n", me, scnt[r], r, G.rcnt[r][me]); exit(1); }
        const void *src = (const char *)sb + sdsp[r]; void *dst = (char *)G.rb[r] + G.rdsp[r][me];
        if (r == me) { if (scnt[r] && src != dst) HIP_CHECK(hipMemcpyAsync(dst, src, scnt[r], hipMemcpyDeviceToDevice, s)); continue; }
        a.src[k] = src; a.dst[k] = dst; nb[k] = scnt[r]; pr[k] = r; k++;
    }
    push_run(a, nb, s, me, pr);
}
static void x_wait(comm *c)
{
    HIP_CHECK(hipSetDevice(c->rank));
    HIP_CHECK(hipStreamSynchronize(G.s[c->rank]));
    if (g_xstats > 0) h4_collect(c->rank);
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

/* ---- H4 (PLAN.md 29): the xGMI push's link concurrency.  comm_xgmi_h4 is called by the four APU threads at once (each
 * after comm_xgmi_create), `bytes` per (sender, peer) pair, `blocks` per peer, `reps` repetitions after one warm-up.
 * Modes: 0 all-to-all (the exchange: every APU pushes to its three peers, one kernel, block b -> peer b % 3);
 * 1 the same, contiguous thirds of the grid per peer; 2 fan-out (APU 0 alone, three links); 3 one link (APU 0 -> 1,
 * all 3 x blocks blocks on it); 4 one link with `blocks` blocks (the per-link share of the all-to-all kernel);
 * 5 one link both ways (0 <-> 1 at once, 3 x blocks each); 6 all-to-all as three kernels on three streams (blocks each);
 * 7 all-to-all by hipMemcpyAsync on three streams (the copy engines); 8 all-to-all in three rounds (round i: APU d ->
 * d + i mod 4, 3 x blocks blocks, a barrier between rounds -- one link per APU at a time).
 * out[0] = seconds per repetition (host, barrier to barrier), out[1] = bytes this APU sent per repetition, out[2..5] =
 * this APU's per-peer rates (GB/s, from the stamps, the mean over the repetitions; 0 where not stamped or no traffic). */
static struct { char *src[NR], *dst[NR]; size_t cap; hipStream_t st[NR][3]; } B;
extern "C" void comm_xgmi_h4(int me, int mode, size_t bytes, int blocks, int reps, double *out)
{
    HIP_CHECK(hipSetDevice(me));
    pthread_barrier_wait(&G.bar);
    if (B.cap < bytes) {
        if (B.src[me]) { HIP_CHECK(hipFree(B.src[me])); HIP_CHECK(hipFree(B.dst[me])); }
        HIP_CHECK(hipMalloc(&B.src[me], NR * bytes)); HIP_CHECK(hipMalloc(&B.dst[me], NR * bytes));
        HIP_CHECK(hipMemset(B.src[me], me + 1, NR * bytes));
        if (!B.st[me][0]) for (int k = 0; k < 3; k++) HIP_CHECK(hipStreamCreateWithFlags(&B.st[me][k], hipStreamNonBlocking));
        HIP_CHECK(hipDeviceSynchronize());
    }
    pthread_barrier_wait(&G.bar);
    if (me == 0) B.cap = bytes > B.cap ? bytes : B.cap;
    pthread_barrier_wait(&G.bar);
    xstats_on();
    hipStream_t s = B.st[me][0];
    size_t sent = 0;
    struct push3 a; size_t nb[3]; int pr[3];
    int active = (mode == 2 || mode == 3 || mode == 4) ? me == 0 : mode == 5 ? me < 2 : 1;
    int one = mode >= 3 && mode <= 5;                   /* one link: the peer and the three slots its thirds */
    int peer1 = me == 0 ? 1 : 0;
    double t0 = 0;
    for (int it = -1; it < reps; it++) {
        pthread_barrier_wait(&G.bar);
        if (it == 0) { t0 = lst_now_x(); for (int r = 0; r < NR; r++) H.bytes[me][r] = H.active[me][r] = 0; H.kspan[me] = H.kbytes[me] = H.n[me] = 0; }
        if (!active) { pthread_barrier_wait(&G.bar); continue; }
        if (mode == 8) {
            for (int i = 1; i < NR; i++) {
                int r = (me + i) % NR; size_t t3 = bytes / 3;
                for (int k = 0; k < 3; k++) { a.src[k] = B.src[me] + r * bytes + k * t3; a.dst[k] = B.dst[r] + me * bytes + k * t3; nb[k] = k < 2 ? t3 : bytes - 2 * t3; pr[k] = r; }
                for (int k = 0; k < 3; k++) a.n[k] = nb[k] / 8;
                h4_launch(me, a, nb, 8, blocks, 0, s, pr); HIP_CHECK(hipStreamSynchronize(s)); h4_collect(me);
                pthread_barrier_wait(&G.bar);
            }
            if (it == reps - 1) { sent = 3 * bytes; }
            pthread_barrier_wait(&G.bar);
            continue;
        }
        if (mode == 6 || mode == 7) {
            int k = 0;
            for (int r = 0; r < NR; r++) if (r != me) {
                if (mode == 7) HIP_CHECK(hipMemcpyAsync(B.dst[r] + me * bytes, B.src[me] + r * bytes, bytes, hipMemcpyDeviceToDevice, B.st[me][k]));
                else { struct push3 b1; size_t t3 = bytes / 3;
                       for (int j = 0; j < 3; j++) { b1.src[j] = B.src[me] + r * bytes + j * t3; b1.dst[j] = B.dst[r] + me * bytes + j * t3; b1.n[j] = (j < 2 ? t3 : bytes - 2 * t3) / 8; }
                       k_push3<uint64_t><<<3 * ((blocks + 2) / 3), 256, 0, B.st[me][k]>>>(b1); }
                k++;
            }
            for (int j = 0; j < 3; j++) HIP_CHECK(hipStreamSynchronize(B.st[me][j]));
            sent = 3 * bytes;
            pthread_barrier_wait(&G.bar);
            continue;
        }
        int k = 0;
        if (one) {
            size_t t3 = bytes / 3;
            for (int j = 0; j < 3; j++) { a.src[j] = B.src[me] + peer1 * bytes + j * t3; a.dst[j] = B.dst[peer1] + me * bytes + j * t3; nb[j] = j < 2 ? t3 : bytes - 2 * t3; pr[j] = peer1; }
            if (mode == 4) { nb[0] = bytes; nb[1] = nb[2] = 0; a.src[0] = B.src[me] + peer1 * bytes; a.dst[0] = B.dst[peer1] + me * bytes; }
        } else for (int r = 0; r < NR; r++) if (r != me) { a.src[k] = B.src[me] + r * bytes; a.dst[k] = B.dst[r] + me * bytes; nb[k] = bytes; pr[k] = r; k++; }
        for (int j = 0; j < 3; j++) a.n[j] = nb[j] / 8;
        h4_launch(me, a, nb, 8, blocks, mode == 1, s, pr);
        HIP_CHECK(hipStreamSynchronize(s)); h4_collect(me);
        size_t b0 = 0; for (int j = 0; j < 3; j++) b0 += nb[j];
        sent = b0;
        pthread_barrier_wait(&G.bar);
    }
    HIP_CHECK(hipDeviceSynchronize());
    pthread_barrier_wait(&G.bar);
    double t1 = lst_now_x();
    for (int r = 0; r < NR; r++) out[2 + r] = H.active[me][r] > 0 ? H.bytes[me][r] / H.active[me][r] * 1e-9 : 0;   /* the stamped per-peer rates */
    out[0] = (t1 - t0) / (reps > 0 ? reps : 1); out[1] = (double)sent;
}
/* the per-link totals of the stamped pushes so far (bytes, active seconds) for sender me, and a reset */
extern "C" void comm_xgmi_h4_links(int me, double *bytes, double *active, double *kspan, int reset)
{
    for (int r = 0; r < NR; r++) { bytes[r] = H.bytes[me][r]; active[r] = H.active[me][r]; }
    *kspan = H.kspan[me];
    if (reset) { for (int r = 0; r < NR; r++) H.bytes[me][r] = H.active[me][r] = 0; H.kspan[me] = H.kbytes[me] = H.n[me] = 0; }
}
