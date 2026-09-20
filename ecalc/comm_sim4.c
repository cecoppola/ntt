/* comm_sim4.c - four synthetic ranks inside one APU (WP5 test harness).  The
 * four rank objects share a table; alltoall posts (send, recv) and wait()
 * performs the slab copies once every rank has posted.  Ranks are driven
 * sequentially by one host thread, so no synchronisation is needed; the
 * all-gathers likewise complete when the fourth rank has called (inflight 0:
 * ntt_dist does not pipeline over this communicator). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comm.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) {                    \
    fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NR 4
static struct { const void *sb[NR]; void *rb[NR]; size_t bytes; int posted, done[NR]; hipStream_t s[NR];
                const void *ag_sb[NR]; void *ag_rb[NR]; int ag_posted;
                const size_t *scnt[NR], *sdsp[NR], *rcnt[NR], *rdsp[NR]; int v; } G;   /* v: the posted exchange is an alltoallv */
static int s_rank(comm *c) { return c->rank; }
static int s_size(comm *c) { (void)c; return NR; }
static void s_post(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s, int v)
{
    if (G.posted == NR) { G.posted = 0; memset(G.done, 0, sizeof G.done); }
    if (G.posted && G.v != v) { fprintf(stderr, "comm_sim4: alltoall and alltoallv posted in one exchange\n"); exit(1); }
    G.sb[c->rank] = sb; G.rb[c->rank] = rb; G.bytes = bytes; G.s[c->rank] = s; G.v = v; G.posted++;
}
static void s_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s) { s_post(c, sb, rb, bytes, s, 0); }
static void s_alltoallv(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp, hipStream_t s)
{
    G.scnt[c->rank] = scnt; G.sdsp[c->rank] = sdsp; G.rcnt[c->rank] = rcnt; G.rdsp[c->rank] = rdsp;
    s_post(c, sb, rb, 0, s, 1);
}
static void s_wait(comm *c)
{
    if (G.posted != NR) { fprintf(stderr, "comm_sim4: wait before all ranks posted\n"); exit(1); }
    if (G.done[c->rank]) return;
    HIP_CHECK(hipDeviceSynchronize());   /* the four ranks' packs ran on their own streams (M7: non-blocking transfer streams) */
    int me = c->rank;
    for (int r = 0; r < NR; r++) {     /* my slab r comes from rank r's slab c->rank */
        const char *src; char *dst; size_t n;
        if (G.v) {
            n = G.scnt[r][me];
            if (n != G.rcnt[me][r]) { fprintf(stderr, "comm_sim4: alltoallv count mismatch: rank %d sends %zu to rank %d, which expects %zu\n", r, n, me, G.rcnt[me][r]); exit(1); }
            src = (const char *)G.sb[r] + G.sdsp[r][me]; dst = (char *)G.rb[me] + G.rdsp[me][r];
        } else { n = G.bytes; src = (const char *)G.sb[r] + (size_t)me * n; dst = (char *)G.rb[me] + (size_t)r * n; }
        if (n) HIP_CHECK(hipMemcpyAsync(dst, src, n, hipMemcpyDeviceToDevice, G.s[me]));
    }
    HIP_CHECK(hipStreamSynchronize(G.s[me]));
    G.done[me] = 1;
}
/* the host variant: complete when the fourth rank has called (as the all-gathers) */
static void s_alltoallv_host(comm *c, const void *sb, const size_t *scnt, const size_t *sdsp, void *rb, const size_t *rcnt, const size_t *rdsp)
{
    static const void *hsb[NR]; static void *hrb[NR]; static const size_t *sc[NR], *sd[NR], *rc[NR], *rd[NR]; static int posted;
    int me = c->rank; hsb[me] = sb; hrb[me] = rb; sc[me] = scnt; sd[me] = sdsp; rc[me] = rcnt; rd[me] = rdsp;
    if (++posted < NR) return;
    posted = 0;
    for (int t = 0; t < NR; t++) for (int r = 0; r < NR; r++) {
        size_t n = sc[r][t];
        if (n != rc[t][r]) { fprintf(stderr, "comm_sim4: alltoallv_host count mismatch (%d -> %d: %zu vs %zu)\n", r, t, n, rc[t][r]); exit(1); }
        if (n) memmove((char *)hrb[t] + rd[t][r], (const char *)hsb[r] + sd[r][t], n);
    }
}
static void s_barrier(comm *c) { (void)c; }
static uint64_t s_modq(comm *c, uint64_t v, uint64_t q, uint64_t w) { (void)c; (void)q; (void)w; return v; }
static size_t s_max(comm *c, size_t v) { (void)c; return v; }
static void s_destroy(comm *c) { free(c); }
/* all-gather: the fourth caller performs every rank's copies (device or host) */
static void s_allgather_any(comm *c, const void *sb, void *rb, size_t bytes, int host)
{
    G.ag_sb[c->rank] = sb; G.ag_rb[c->rank] = rb;
    if (++G.ag_posted < NR) return;
    G.ag_posted = 0;
    for (int me = 0; me < NR; me++) for (int r = 0; r < NR; r++) {
        void *dst = (char *)G.ag_rb[me] + (size_t)r * bytes; const void *src = G.ag_sb[r];
        if (dst == src) continue;
        if (host) memcpy(dst, src, bytes); else HIP_CHECK(hipMemcpy(dst, src, bytes, hipMemcpyDeviceToDevice));
    }
}
static void s_allgather(comm *c, const void *sb, void *rb, size_t bytes) { s_allgather_any(c, sb, rb, bytes, 0); }
static void s_allgather_host(comm *c, const void *sb, void *rb, size_t bytes) { s_allgather_any(c, sb, rb, bytes, 1); }
static const struct comm_ops sim_ops = { s_rank, s_size, s_alltoall, s_wait, s_barrier, s_modq, s_max, s_destroy, 0, 0, s_allgather, s_allgather_host, s_alltoallv, s_alltoallv_host };
comm *comm_sim4_create(int rank)
{
    comm *c = (comm *)calloc(1, sizeof *c);
    c->ops = &sim_ops; c->rank = rank; c->size = NR; c->inflight = 0;
    return c;
}
