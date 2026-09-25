/* mn.c - the multi-node layer: environment, one mesh per APU thread (TCP, or SHMEM PE sets with COMM_TRANSPORT=shmem --
 * Phase 11 S), the start-up self-test (PLAN.md 17, M1) */
#include <stdio.h>
#include "fatal.h"
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "mn.h"
#include "ntt.h"
#include "ntt_dist.h"
#include "modarith.h"
#include "mem.h"
#include "memsample.h"                                /* Phase 14 S1 (E1) */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define NA 4
static int g_rank, g_size = 1; static comm *g_cm[NA];
static const char *g_hosts; static int g_port = 27000;
static int g_shmem, g_topo;                           /* g_topo: MN_TOPO_GROUP (below). Phase 11 S: COMM_TRANSPORT=shmem -- the meshes are strided PE sets over SHMEM (comm_shmem.c), else TCP */
int mn_rank(void) { return g_rank; }
int mn_size(void) { return g_size; }
comm *mn_comm(int apu) { return g_size > 1 ? g_cm[apu] : 0; }
int mn_transport_shmem(void) { return g_shmem; }
int mn_init(void)
{
    const char *er = getenv("COMM_RANK"), *es = getenv("COMM_SIZE"), *eh = getenv("COMM_HOSTS"), *ep = getenv("COMM_PORT"), *et = getenv("COMM_TRANSPORT");
    g_shmem = et && !strcmp(et, "shmem") && es && atoi(es) > 1;   /* (a single process -- no launcher, COMM_SIZE unset by mnrun.sh -- stays size 1 with the variable exported, as TCP does) */
    g_topo = getenv("MN_TOPO_GROUP") ? atoi(getenv("MN_TOPO_GROUP")) : 0;
    if (g_shmem) {                                       /* rank and size from the SHMEM runtime (srun --mpi=pmix / oshrun); COMM_RANK is not needed */
        if (!comm_shmem_available()) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=shmem but built without SHMEM (make SHMEM=1)\n"); }
        double t0 = mem_now();
        g_size = comm_shmem_init(); g_rank = comm_shmem_rank();
        if (g_size <= 1) { g_size = 1; g_rank = 0; comm_shmem_finalize(); return 1; }
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); g_cm[d] = comm_shmem_create_at(0, 1, g_size, d); }
        HIP_CHECK(hipSetDevice(0));
        printf("mn: node %d of %d, four meshes of %d PEs over SHMEM: created in %.2f s\n", g_rank, g_size, g_size, mem_now() - t0);
        return g_size;
    }
    g_size = es ? atoi(es) : 1; g_rank = er ? atoi(er) : 0;
    if (g_size <= 1) { g_size = 1; g_rank = 0; return 1; }
    if (!eh) { ec_fatal(EC_RC_FATAL, "mn: COMM_SIZE %d needs COMM_HOSTS\n", g_size); }
    int base = ep ? atoi(ep) : 27000;
    g_hosts = strdup(eh); g_port = base;
    /* mesh d joins APU thread d of every node: rank = node, size = nodes */
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
    for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); g_cm[d] = comm_tcp_create_at(g_rank, g_size, eh, base + 64 * d); }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: node %d of %d, four meshes of %d ranks on %s (port base %d): connected in %.2f s\n", g_rank, g_size, g_size, eh, base, mem_now() - t0);
    return g_size;
}
void mn_barrier(void) { if (g_size > 1) comm_barrier(g_cm[0]); }
static void groups_finalize(void);
extern "C" void bs_ckpt_tree_remove_below(int level);   /* binsplit.h (included below, with the tree checkpoint code) */
static int g_ckpend = 0;                              /* C6: the tree level whose set is written but whose predecessors are not yet removed */
void mn_finalize(void)
{
    if (g_size > 1 && g_ckpend) { bs_ckpt_tree_remove_below(g_ckpend); g_ckpend = 0; }   /* C6: after the driver's final barrier every node has this set */
    if (g_size > 1) { groups_finalize(); for (int d = 0; d < NA; d++) if (g_cm[d]) { comm_destroy(g_cm[d]); g_cm[d] = 0; } }
    if (g_size > 1 && g_shmem) comm_shmem_finalize();
}
/* self-test: on every APU thread, prime d, a random cyclic convolution of 2^(logR+logC) points from a seed all
 * nodes share; the distributed fwd/pw/inv over mesh d's `size` ranks must equal the one-rank engine on this
 * rank's block-cyclic rows (rank r holds rows [r R/nr, (r+1) R/nr); row i, column j <-> point i + R j) */
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
int mn_selftest(int logR, int logC, int verbose)
{
    if (g_size <= 1) return 1;
    if (g_size & (g_size - 1)) { printf("mn: self-test over the plain meshes skipped (size %d is not a power of two; the layered self-test covers the transform nodes)\n", g_size); return 1; }
    int ok = 1, nr = g_size, logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    if (rr == 0) { fprintf(stderr, "mn_selftest: R < ranks\n"); return 0; }
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        int prime = d, r = g_rank; uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;
        uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *tmp = (uint64_t *)malloc(rows * 8);
        for (size_t i = 0; i < n; i++) { hx[i] = xs(&seed) % p; hy[i] = xs(&seed) % p; }
        ntt_ctx *ctx = ntt_ctx_create(prime); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx, dx, logn, 1, s); ntt_fwd(ctx, dy, logn, 1, s); ntt_pw(ctx, dx, dy, n, s); ntt_inv(ctx, dx, logn, 1, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
        dist_plan pl; dist_plan_create(&pl, g_cm[d], ctx, prime, logR, logC);
        uint64_t *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
        dist_fwd(&pl, rx, s); dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s); dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) if (tmp[il * C + j] != ref[(r * rr + il) + R * j]) bad++;
        if (bad || verbose) printf("mn: node %d mesh %d 2^%d points: %zu of %zu differ\n", r, d, logn, bad, rows);
        if (bad) ok = 0;
        dist_plan_free(&pl); HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); HIP_CHECK(hipStreamDestroy(s)); ntt_ctx_free(ctx);
        free(hx); free(hy); free(ref); free(tmp);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: self-test, four meshes over %d nodes at 2^%d points: %s (%.2f s)\n", nr, logn, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}

/* ---- Phase 8 M3: node groups, the layered self-test, the distributed top levels of the tree (PLAN.md 17) ---- */
#include "mdb.h"
#include "dbig.h"
#define MN_MAXL 16
static mn_group *g_groups[MN_MAXL];
/* the comma list of the hosts of nodes [g0, g0+g) */
static char *hosts_of(int g0, int g)
{
    char *all = strdup(g_hosts), *out = (char *)malloc(strlen(g_hosts) + 2), *o = out, *sp; int i = 0; *o = 0;
    for (char *t = strtok_r(all, ",", &sp); t; t = strtok_r(NULL, ",", &sp), i++) if (i >= g0 && i < g0 + g) { if (o != out) *o++ = ','; strcpy(o, t); o += strlen(t); }   /* (four threads at once: strtok_r) */
    free(all); return out;
}
/* mesh d over the nodes [g0, g0+g): rank = node - g0; port slot s (0 = the M1 meshes): member r listens on
 * base + 512 s + 64 d + (g0 + r), unique per (slot, d, node) even when node-processes share a host */
static comm *sub_mesh(int g0, int g, int slot, int d)
{
    if (g0 == 0 && g == g_size) return g_cm[d];
    if (g_shmem) return comm_shmem_create_at(g0, 1, g, NA + NA * slot + d);   /* S: the PE set [g0, g0+g); id unique per (slot, d) -- the base meshes hold ids 0..3 */
    char *h = hosts_of(g0, g); comm *c = comm_tcp_create_at(g_rank - g0, g, h, g_port + 512 * slot + 64 * d + g0); free(h); return c;
}
/* S (PLAN.md 25): the dragonfly's third layer.  MN_TOPO_GROUP=T (nodes per dragonfly group, contiguous in the node
 * numbering): the transform nodes [g0, g0+gt) of a group, when T divides gt and gt > T, exchange through an inner
 * layered communicator in the intra-minor order (node = T a + b): intra = the T nodes of dragonfly group a (one switch
 * hop), inter = the nodes with in-group index b across the gt/T groups (the global links: one aggregated message per
 * peer group).  Its rank = node - g0, as the plain mesh's, so the callers see no difference.  The two meshes: SHMEM
 * strided PE sets, or TCP meshes on the group's slot (in-group: the same ports the plain mesh would use; cross-group:
 * the d + 4 port lanes of the slot, q b + a < 64). */
static comm *g_topo_mesh[MN_MAXL][NA][2];                 /* the in-group and cross-group meshes under each level's tr[d] */
static comm *sub_mesh_strided(int start, int stride, int n, int slot, int lane, int off)
{
    if (g_shmem) return comm_shmem_create_at(start, stride, n, NA + NA * slot + lane);
    char *all = strdup(g_hosts), *out = (char *)malloc(strlen(g_hosts) + 2), *o = out, *sp; int i = 0, me = -1; *o = 0;
    for (char *t = strtok_r(all, ",", &sp); t; t = strtok_r(NULL, ",", &sp), i++) if (i >= start && (i - start) % stride == 0 && (i - start) / stride < n) { if (o != out) *o++ = ','; strcpy(o, t); o += strlen(t); if (i == g_rank) me = (i - start) / stride; }
    free(all);
    comm *c = comm_tcp_create_at(me, n, out, g_port + 512 * slot + 64 * lane + off); free(out); return c;
}
static comm *topo_tr(mn_group *G, int level, int d)
{
    int T = g_topo, gt = G->gt, me = G->me, a = me / T, b = me % T, q = gt / T;
    int tr = getenv("MN_TOPO_TRACE") != 0;
    if (tr) fprintf(stderr, "mn: node %d thread %d: in-group mesh [%d +%d) ...\n", g_rank, d, G->g0 + a * T, T);
    comm *in = sub_mesh_strided(G->g0 + a * T, 1, T, 2 * level, d, a * T);          /* the in-group mesh: node b of group a */
    if (tr) fprintf(stderr, "mn: node %d thread %d: in-group mesh done; cross mesh {%d + %d k, k < %d} ...\n", g_rank, d, G->g0 + b, T, q);
    comm *cross = sub_mesh_strided(G->g0 + b, T, q, 2 * level, NA + d, q * b);      /* the cross-group mesh: group a of in-group index b */
    if (tr) fprintf(stderr, "mn: node %d thread %d: cross mesh done\n", g_rank, d);
    g_topo_mesh[level][d][0] = in; g_topo_mesh[level][d][1] = cross;
    return comm_layered_create_minor(in, cross, d);
}
static int pow2_floor(int g) { int p = 1; while (2 * p <= g) p *= 2; return p; }
/* this node's group at level l >= 1: the nodes [k 2^l, min((k+1) 2^l, size)), k = rank >> l; its meshes are
 * created on first use by all its members together (a singleton group has none) */
mn_group *mn_group_at(int level)
{
    if (level < 1 || level >= MN_MAXL) { ec_fatal(EC_RC_FATAL, "mn_group_at: level %d\n", level); }
    if (g_groups[level]) return g_groups[level];
    mn_group *G = (mn_group *)calloc(1, sizeof *G);
    int k = g_rank >> level; G->g0 = k << level; G->g = (1 << level) < g_size - G->g0 ? (1 << level) : g_size - G->g0;
    G->gt = pow2_floor(G->g); G->me = g_rank - G->g0;
    if (G->g > 1) {
        double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) {
            HIP_CHECK(hipSetDevice(d));
            G->all[d] = sub_mesh(G->g0, G->g, 2 * level - 1, d);
            if (g_topo > 1 && G->gt > g_topo && G->gt % g_topo == 0) G->tr[d] = G->me < G->gt ? topo_tr(G, level, d) : 0;
            else G->tr[d] = G->gt == G->g ? G->all[d] : (G->me < G->gt ? sub_mesh(G->g0, G->gt, 2 * level, d) : 0);
        }
        HIP_CHECK(hipSetDevice(0));
        printf("mn: node %d: level %d group [%d, %d) (transform nodes %d%s): meshes connected in %.2f s\n", g_rank, level, G->g0, G->g0 + G->g, G->gt, g_topo_mesh[level][0][0] ? ", three-layer exchange" : "", mem_now() - t0);
    }
    g_groups[level] = G;
    return G;
}
/* Phase 12 G (agent G; the tree's schedule, PLAN 27 row G): the group [g0, g0+g) of schedule level l (mn_groups_parse: MN_GROUPS).
 * A group that is one of the binary levels' -- G_l a power of two, or the whole machine -- IS mn_group_at's (the same meshes,
 * the same slots: the default schedule runs exactly as before); any other size (3, 6, 192 ...) gets its own meshes on the
 * slot 2 MN_MAXL + l (every node of the group takes part in the transform: no tr, no topo layer -- L's general map) */
static mn_group *g_sched[MN_MAXL];
mn_group *mn_group_span(int l, int g0, int g)
{
    if (l < 1 || l >= MN_MAXL) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d\n", l); }
    int lv = 0; while ((1 << lv) < g) lv++;
    if (((1 << lv) == g && (g0 & (g - 1)) == 0) || (g0 == 0 && g == g_size)) {
        if (g0 == 0 && g == g_size) { lv = 0; while ((1 << lv) < g_size) lv++; }
        mn_group *B = mn_group_at(lv);
        if (B->g0 != g0 || B->g != g) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d [%d, %d) is not the binary group [%d, %d)\n", l, g0, g0 + g, B->g0, B->g0 + B->g); }
        return B;
    }
    if (g_sched[l]) { if (g_sched[l]->g0 != g0 || g_sched[l]->g != g) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d group changed\n", l); } return g_sched[l]; }
    mn_group *G = (mn_group *)calloc(1, sizeof *G);
    G->g0 = g0; G->g = g; G->gt = pow2_floor(g); G->me = g_rank - g0;
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d)); G->tr[d] = 0;
        /* SHMEM: a PE-set id of its own (NA + NA (2 MN_MAXL + l) + d); TCP: the port lanes NA + d of the level's odd slot 2 l - 1 (the
         * binary group of the level listens on lanes 0..3 of it, the topo meshes on the even slots), so the ports stay below the
         * ephemeral range on aac6 (slot 2 MN_MAXL + l put them above 32768: "cannot connect", batch 2 at size 9) */
        if (g_shmem) G->all[d] = comm_shmem_create_at(g0, 1, g, NA + NA * (2 * MN_MAXL + l) + d);
        else { char *h = hosts_of(g0, g); G->all[d] = comm_tcp_create_at(g_rank - g0, g, h, g_port + 512 * (2 * l - 1) + 64 * (NA + d) + g0); free(h); }
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: node %d: schedule level %d group [%d, %d) (%d nodes, the general map): meshes connected in %.2f s\n", g_rank, l, g0, g0 + g, g, mem_now() - t0);
    g_sched[l] = G;
    return G;
}
/* all-gather of k u64 per node over a mesh: the transport's host all-gather (M7, A-comm; was a point-to-point loop) */
void mn_allgather(comm *c, const uint64_t *v, int k, uint64_t *out) { comm_allgather_host(c, v, out, (size_t)k * 8); }
/* the layered communicator's self-test (the mn_selftest pattern over 4 gt ranks, gt = the largest power of two <= size,
 * rank rho = gt d + node): one plane of one prime spread over all 4 gt ranks (the four APUs of a node hold rows of the
 * same plane, as in the product tier), each rank's distributed convolution against the one-rank engine on its rows */
int mn_selftest_layered(int logR, int logC, int verbose)
{
    if (g_size <= 1) return 1;
    int L = 0; while ((1 << L) < g_size) L++;
    mn_group *G = mn_group_at(L); int gt = G->gt;
    int dbg_local = getenv("MN_LAYERED_LOCAL") != 0;         /* debug: the layered comm over a size-1 inter comm (4 ranks, intra only) */
    if (dbg_local) gt = 1;
    if (g_rank >= gt) return 1;
    int ok = 1, nr = NA * gt, logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    if (rr < 32) { fprintf(stderr, "mn_selftest_layered: R / ranks < 32\n"); return 0; }
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        int prime = (logR + logC) & 3, r = gt * d + g_rank; uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;   /* one plane (one prime) over all 4 gt ranks, as the product tier runs it */
        uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *tmp = (uint64_t *)malloc(rows * 8);
        for (size_t i = 0; i < n; i++) { hx[i] = xs(&seed) % p; hy[i] = xs(&seed) % p; }
        ntt_ctx *ctx = ntt_ctx_create(prime); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx, dx, logn, 1, s); ntt_fwd(ctx, dy, logn, 1, s); ntt_pw(ctx, dx, dy, n, s); ntt_inv(ctx, dx, logn, 1, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
        comm *xg = comm_xgmi_create(d), *cm = getenv("MN_LAYERED_RAW") ? xg : comm_layered_create(xg, dbg_local ? comm_local_create() : G->tr[d], d);
        if (comm_rank(cm) != r || comm_size(cm) != nr) { ec_fatal(EC_RC_FATAL, "mn_selftest_layered: rank %d/%d, expected %d/%d\n", comm_rank(cm), comm_size(cm), r, nr); }
        dist_plan pl; dist_plan_create(&pl, cm, ctx, prime, logR, logC);
        uint64_t *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
        {   /* the all-to-all alone: slab sigma of rank rho tagged (rho, sigma, i) must arrive as slab rho of rank sigma */
            size_t sl = rows / nr; uint64_t *pat = (uint64_t *)malloc(rows * 8);
            for (int sg = 0; sg < nr; sg++) for (size_t i = 0; i < sl; i++) pat[sg * sl + i] = ((uint64_t)r << 40) | ((uint64_t)sg << 32) | i;
            HIP_CHECK(hipMemcpy(rx, pat, rows * 8, hipMemcpyHostToDevice));
            comm_alltoall(cm, rx, ry, sl * 8, s); comm_wait(cm);
            HIP_CHECK(hipMemcpy(pat, ry, rows * 8, hipMemcpyDeviceToHost));
            size_t bad = 0;
            for (int src = 0; src < nr; src++) for (size_t i = 0; i < sl; i++) { uint64_t want = ((uint64_t)src << 40) | ((uint64_t)r << 32) | i; if (pat[src * sl + i] != want) { if (!bad) printf("mn: node %d rank %d: slab %d word %zu = (rank %llu, slab %llu, %llu), want (%d, %d, %zu)\n", g_rank, r, src, i, (unsigned long long)(pat[src * sl + i] >> 40), (unsigned long long)((pat[src * sl + i] >> 32) & 255), (unsigned long long)(pat[src * sl + i] & 0xffffffff), src, r, i); bad++; } }
            if (bad) { printf("mn: node %d rank %d: layered all-to-all pattern: %zu of %zu words wrong\n", g_rank, r, bad, rows); ok = 0; }
            free(pat);
        }
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
        dist_fwd(&pl, rx, s); dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s); dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) if (tmp[il * C + j] != ref[(r * rr + il) + R * j]) bad++;
        if (bad || verbose) printf("mn: node %d layered rank %d of %d, 2^%d points: %zu of %zu differ\n", g_rank, r, nr, logn, bad, rows);
        if (bad && getenv("MN_DEBUG")) {                      /* where do my values come from? search the reference for got[0], got[1] */
            for (int k = 0; k < 3; k++) { size_t il = k, j = 0; uint64_t got = tmp[il * C + j]; size_t where = n;
                for (size_t i = 0; i < n; i++) if (ref[i] == got) { where = i; break; }
                printf("mn: node %d rank %d: row %zu col 0 (point %zu): got %llu, ref %llu; got is ref[%zu] (row %zu col %zu)\n", g_rank, r, r * rr + il, r * rr + il, (unsigned long long)got, (unsigned long long)ref[r * rr + il], where, where < n ? where % R : 0, where < n ? where / R : 0); }
        }
        if (bad) ok = 0;
        dist_plan_free(&pl); if (cm != xg) comm_destroy(cm); comm_destroy(xg);
        HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); HIP_CHECK(hipStreamDestroy(s)); ntt_ctx_free(ctx);
        free(hx); free(hy); free(ref); free(tmp);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: layered self-test, %d x 4 ranks at 2^%d points: %s (%.2f s)\n", gt, logn, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
/* the top log2(size) levels of the binary-splitting tree as distributed products: level l pairs the groups A (lower
 * terms) and B (higher terms) of level l-1, P = P_A Q_B + P_B (P_B as the CRT's added operand), Q = Q_A Q_B, each
 * operand sharded over its own group's nodes and the results over the joined group.  The other side's
 * descriptors (n, N, g0, g) come from an all-gather over the group; nothing else is exchanged outside the
 * products.  In: this node's leaf P_r, Q_r (device numbers, taken over); out: shares of P, Q over all nodes. */
/* M6 (PLAN.md 19, results/A-ckpt.md): the tree level every node restarts from -- the lowest, over the nodes, of each
 * node's highest complete tree set (a node writes level l, then all nodes meet at a barrier, then the superseded
 * set goes: so the lowest level present is present everywhere); computed once, the sets above it discarded */
#include "binsplit.h"
#include "newton.h"                                   /* Phase 13 N: newton_mn_pq_hook */
#include <unistd.h>
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half);
static void tree_level_k(mdb *P, mdb *Q, int l, int g0, int g, int gp, int nch);
/* Phase 12 G (a V-style probe, ECALC_RES_LOG=1): after a level, the sharded P, Q of the group [g0, g0+g) against the recurrence
 * over the group's terms [1 + N g0 / size, 1 + N (g0+g) / size) mod the T1 primes -- every node computes its share's residues,
 * an all-gather over the group, the sum of share_r B^lo_r mod q on every node.  Names the level whose product is wrong. */
#include "verify.h"
static uint64_t mulmod_u64(uint64_t a, uint64_t b, uint64_t q) { return (uint64_t)((unsigned __int128)a * b % q); }
static uint64_t powmod_u64(uint64_t b, uint64_t e, uint64_t q) { uint64_t r = 1 % q; b %= q; while (e) { if (e & 1) r = mulmod_u64(r, b, q); b = mulmod_u64(b, b, q); e >>= 1; } return r; }
static void tree_check(const mdb *P, const mdb *Q, int l, int g0, int g, mn_group *G)
{
    uint64_t v[2 + 2 * T1_NQ], *all = (uint64_t *)malloc((size_t)g * sizeof v);
    size_t lo, hi; mdb_share(P, g_rank, &lo, &hi); v[0] = lo; mdb_share(Q, g_rank, &lo, &hi); v[1] = lo;
    if (P->sh.n) db_mod_qs(&P->sh, t1_q, T1_NQ, v + 2); else memset(v + 2, 0, 8 * T1_NQ);
    if (Q->sh.n) db_mod_qs(&Q->sh, t1_q, T1_NQ, v + 2 + T1_NQ); else memset(v + 2 + T1_NQ, 0, 8 * T1_NQ);
    mn_allgather(G->all[0], v, 2 + 2 * T1_NQ, all);
    uint64_t B = bi_decimal ? 1000000000000000000ull : 0;   /* the limb base (2^64 = 0 mod q handled as (2^64 - 1) + 1) */
    unsigned __int128 nn = bs_N; unsigned long a0 = 1 + (unsigned long)(nn * g0 / g_size), b1 = 1 + (unsigned long)(nn * (g0 + g) / g_size);
    int bad = 0; char line[512]; int o = snprintf(line, sizeof line, "RES node %d tree level %d [%d, %d) P/Q vs recurrence [%lu, %lu):", g_rank, l, g0, g0 + g, a0, b1);
    for (int i = 0; i < T1_NQ; i++) {
        uint64_t q = t1_q[i], pr, qr, ps = 0, qs = 0; vf_pq_range_mod(a0, b1, q, &pr, &qr);
        uint64_t Bq = B ? B % q : (uint64_t)(((unsigned __int128)1 << 64) % q);
        for (int r = 0; r < g; r++) { const uint64_t *w = all + (size_t)r * (2 + 2 * T1_NQ);
            ps = (ps + mulmod_u64(w[2 + i], powmod_u64(Bq, w[0], q), q)) % q; qs = (qs + mulmod_u64(w[2 + T1_NQ + i], powmod_u64(Bq, w[1], q), q)) % q; }
        int ok = ps == pr && qs == qr; if (!ok) bad++;
        o += snprintf(line + o, sizeof line - o, " q%d %s", i, ok ? "ok" : (ps == pr ? "Q BAD" : qs == qr ? "P BAD" : "P,Q BAD"));
    }
    printf("%s%s\n", line, bad ? "  TREE LEVEL MISMATCH" : "  (agrees)");
    free(all);
}
static int g_cktree = -1;
int mn_ckpt_tree_level(unsigned long N)
{
    if (g_cktree >= 0) return g_cktree;
    if (N) bs_N = N;
    if (g_size <= 1 || !bs_restart || !bs_ckpt_dir) { g_cktree = 0; return 0; }
    /* Phase 13 N (TASKS 1.7): every node scans its sets without aborting (a foreign set, or a tree set written under another
     * schedule -- the sets are indexed by the schedule's level), then all nodes see every verdict and fail together, loudly */
    int err = 0; char msg[2048];
    uint64_t v[2] = { (uint64_t)bs_ckpt_restart_scan(bs_N, &err, msg, sizeof msg), 0 }, *all = (uint64_t *)malloc((size_t)g_size * 16), mn = v[0];
    v[1] = (uint64_t)err;
    if (msg[0]) fprintf(stderr, "mn: node %d: %s\n", g_rank, msg);
    mn_allgather(g_cm[0], v, 2, all);
    int nbad = 0, first = -1, kind = 0;
    for (int r = 0; r < g_size; r++) { if (all[2 * r] < mn) mn = all[2 * r]; if (all[2 * r + 1]) { nbad++; if (first < 0) { first = r; kind = (int)all[2 * r + 1]; } } }
    free(all);
    if (nbad) {
        if (g_rank == 0) fprintf(stderr, "mn: BS_RESTART refused: %d of %d nodes hold checkpoint sets in %s %s (node %d first; its message above).  %s\n",
                                 nbad, g_size, bs_ckpt_dir, kind == 2 ? "written under another schedule" : "of another run", first,
                                 kind == 2 ? "Restart with the MN_GROUPS and node count of the run that wrote them, or start afresh (no BS_RESTART, or another BS_CKPT_DIR)."
                                           : "Point BS_CKPT_DIR at this run's sets, or start afresh (no BS_RESTART).");
        fflush(stderr); fflush(stdout);
        exit(kind == 2 ? 5 : 4);
    }
    g_cktree = (int)mn;
    bs_ckpt_tree_clear(g_cktree);
    printf("mn: node %d: tree checkpoint sets: mine up to level %d, all nodes have level %d\n", g_rank, (int)v[0], g_cktree);
    return g_cktree;
}
/* Phase 13 N (TASKS 1.4, 4.1): the top tree set in the background (mn_ckpt_bg_mode, set by ecalc.c: 1 = complete, the division
 * waits for P's part before S = P + Q if it must; 2 = budgeted, dropped when the measured disk rate cannot finish a part within
 * mn_ckpt_slack seconds of its release).  The writer reads the shares while the reciprocal and the division run; the hook in
 * newton_mn_divmod lets go of P before S = P + Q and takes Q's share, when the writer still needs it, until mn_ckpt_top_finish
 * (after the output stage).  mn_ckpt_top_finish joins the writer and, once every node has the set, lets mn_finalize remove the
 * sets it supersedes (the C6 rule; a node that dropped or failed its set keeps every node's lower sets). */
int mn_ckpt_bg_mode = 0; double mn_ckpt_slack = 1.0;
static bs_ckpt_bg *g_topbg = 0; static int g_topbg_on = 0, g_toplevel = 0; static dbig g_heldQ;
static void mn_pq_hook(int stage, mdb *x)
{
    if (!g_topbg) return;
    if (stage == 0) { bs_ckpt_bg_release(g_topbg, 0); return; }
    if (bs_ckpt_bg_done(g_topbg, 1)) return;               /* Q written (or the set dropped): freed as before */
    g_heldQ = x->sh; memset(&x->sh, 0, sizeof x->sh);         /* held until after the output stage (mfree(Q) frees nothing now) */
}
void mn_ckpt_top_finish(void)
{
    if (!g_topbg_on) return;
    g_topbg_on = 0; newton_mn_pq_hook = 0;
    double tw = 0, tq = 0; size_t bytes = 0;
    if (g_topbg) {
        bs_ckpt_bg_release(g_topbg, 1);
        db_free(&g_heldQ);
        char who[64]; snprintf(who, sizeof who, "mn: node %d: ", g_rank);
        bytes = bs_ckpt_bg_join(g_topbg, who, bs_ckpt_dir, &tw, &tq); g_topbg = 0;
    }
    if (bytes) { bs_st.n_ckpt++; bs_st.ckpt_bytes += bytes; }
    uint64_t v = bytes ? 1 : 0, *all = (uint64_t *)malloc((size_t)g_size * 8); int every = 1;
    mn_allgather(g_cm[0], &v, 1, all);
    for (int r = 0; r < g_size; r++) every = every && all[r];
    free(all);
    if (every) g_ckpend = g_toplevel;                         /* the lower sets go at mn_finalize, after the final barrier */
    else if (bytes) printf("mn: node %d: the top tree set (level %d) is not on every node: the lower sets are kept\n", g_rank, g_toplevel);
    if (g_rank == 0) printf("RESULT ecalc ckpt_top s %.6g\nRESULT ecalc ckpt_top_wait s %.6g\n", tw, tq);
}
void mn_tree(mdb *P, mdb *Q, dbig *Pleaf, dbig *Qleaf)
{
    memset(P, 0, sizeof *P); memset(Q, 0, sizeof *Q);
    P->sh = *Pleaf; P->n = P->N = Pleaf->n; P->g0 = g_rank; P->g = 1; memset(Pleaf, 0, sizeof *Pleaf);
    Q->sh = *Qleaf; Q->n = Q->N = Qleaf->n; Q->g0 = g_rank; Q->g = 1; memset(Qleaf, 0, sizeof *Qleaf);
    dm_switches(); if (mn_tree_early_free > 0 && g_rank == 0) printf("mn: MN_TREE_EARLY_FREE: the tree levels free P_i, P_run between their two products\n");   /* Phase 14 T1 (E10a) */
    int gs[MN_MAXL]; int L = mn_groups_parse(g_size, gs, MN_MAXL - 1);   /* Phase 12 G: the level -> group-size schedule (MN_GROUPS; default 2, 4, ..., size: the binary tree as before) */
    int lr = mn_ckpt_tree_level(0), ck = bs_ckpt_dir && (getenv("BS_CKPT_TREE") ? atoi(getenv("BS_CKPT_TREE")) : 1);   /* M6: resume above level lr; BS_CKPT_TREE=0: no tree sets */
    if (lr > 0) {                                            /* the shares of P, Q after tree level lr, from this node's set */
        double t0 = mem_now(); uint64_t d[10]; dbig ps, qs;
        if (!bs_ckpt_tree_read(lr, bs_N, d, &ps, &qs)) { ec_fatal(EC_RC_FATAL, "mn: node %d: restart from tree level %d failed\n", g_rank, lr); }
        db_free(&P->sh); db_free(&Q->sh);
        P->sh = ps; P->n = d[0]; P->N = d[1]; P->g0 = (int)d[2]; P->g = (int)d[3]; Q->sh = qs; Q->n = d[5]; Q->N = d[6]; Q->g0 = (int)d[7]; Q->g = (int)d[8];
        bs_st.t_restart += mem_now() - t0;
        printf("mn: node %d: restart from tree level %d: P %zu limbs (share %zu), Q %zu limbs (share %zu), loaded in %.2f s\n", g_rank, lr, P->n, P->sh.n, Q->n, Q->sh.n, mem_now() - t0);
    }
    int every = getenv("BS_CKPT_TREE_EVERY") ? atoi(getenv("BS_CKPT_TREE_EVERY")) : 1; if (every < 1) every = 1;   /* C6: a set every this many tree levels (the top level always) */
    if (ck && lr == 0 && !bs_restart) bs_ckpt_tree_clear(0);   /* Phase 13 N (1.7): a fresh run: this node's tree sets of an earlier run go (levels another schedule wrote would outlive this run's) */
    int captest = getenv("MN_TREE_LOGN_TEST") ? atoi(getenv("MN_TREE_LOGN_TEST")) : 0;   /* Phase 12 G (tests): the tree's levels at a lowered plane cap (grids at 10^10 on one node), the division at its own */
    if (captest) rns_dist_cap_test(captest);
    for (int l = lr + 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1;       /* this level's group size and the children's (the previous level's) */
        int k = g_rank / Gl, g0 = k * Gl, g = Gl < g_size - g0 ? Gl : g_size - g0, nch = (g + Gp - 1) / Gp;
        if (nch == 2) tree_level(P, Q, l, g0, g, Gp);        /* two children: the pair of products as before (the default schedule: bit for bit, the same groups) */
        else if (nch > 2) tree_level_k(P, Q, l, g0, g, Gp, nch);   /* a k-way level (nch children of Gp nodes): k - 1 combines over the level's group */
        /* (one child: no sibling group, carried up unchanged) */
        if (nch > 1 && db_res_log_on()) tree_check(P, Q, l, g0, g, mn_group_span(l, g0, g));
        if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d", g_rank, l); mem_live_line(w); }   /* Phase 14 S1 (E1): the pool's live bytes per level */
        if (ck && (l % every == 0 || l == L)) {              /* M6: this node's shares after level l */
            /* C6: the sets below the previous set (g_ckpend) go here, not right after its write: every node wrote
             * g_ckpend before entering the next level, so this barrier waits for the nodes' compute, never for the
             * slowest node's write.  The restart rule (the lowest "highest complete level" over the nodes exists on
             * every node) holds: a node removes below g_ckpend only after every node has it.  The last set's
             * predecessors go at mn_finalize, after the driver's final barrier. */
            if (g_ckpend) { double tb = mem_now(); mn_barrier(); bs_ckpt_tree_remove_below(g_ckpend); g_ckpend = 0; bs_st.t_ckpt += mem_now() - tb; }
            double tc = mem_now(); uint64_t d[10] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, P->sh.n, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g, Q->sh.n };
            if (l == L && mn_ckpt_bg_mode) {                 /* Phase 13 N (1.4): the top set in the background */
                g_topbg = bs_ckpt_bg_start(l, bs_N, d, &P->sh, &Q->sh, mn_ckpt_bg_mode == 2, mn_ckpt_slack); g_topbg_on = 1; g_toplevel = l;
                newton_mn_pq_hook = mn_pq_hook;
                printf("mn: node %d: checkpoint tree level %d -> %s: %.3f GB in the background%s (started in %.2f s)\n", g_rank, l, bs_ckpt_dir, (P->sh.n + Q->sh.n) * 8e-9,
                       mn_ckpt_bg_mode == 2 ? ", budgeted" : "", mem_now() - tc);
                if (getenv("BS_CKPT_ABORT_TREE") && atoi(getenv("BS_CKPT_ABORT_TREE")) == l && (!getenv("BS_CKPT_ABORT_NODE") || atoi(getenv("BS_CKPT_ABORT_NODE")) == g_rank)) {
                    char who[64]; snprintf(who, sizeof who, "mn: node %d: ", g_rank);
                    bs_ckpt_bg_release(g_topbg, 1); size_t by = bs_ckpt_bg_join(g_topbg, who, bs_ckpt_dir, 0, 0); g_topbg = 0;
                    printf("mn: node %d: BS_CKPT_ABORT_TREE: exiting after the tree level %d checkpoint%s\n", g_rank, l, by ? "" : " (the set FAILED)"); fflush(stdout); _exit(3);
                }
                continue;
            }
            size_t bytes = bs_ckpt_tree_write(l, bs_N, d, &P->sh, &Q->sh); double dtc = mem_now() - tc;
            if (bytes) { bs_st.n_ckpt++; bs_st.ckpt_bytes += bytes; bs_st.t_ckpt += dtc; g_ckpend = l; }
            printf("mn: node %d: checkpoint tree level %d -> %s: %.3f GB in %.2f s (%.2f GB/s)%s\n", g_rank, l, bs_ckpt_dir, bytes * 1e-9, dtc, bytes * 1e-9 / (dtc > 0 ? dtc : 1), bytes ? "" : "  FAILED, continuing");
            if (bytes && getenv("BS_CKPT_ABORT_TREE") && atoi(getenv("BS_CKPT_ABORT_TREE")) == l && (!getenv("BS_CKPT_ABORT_NODE") || atoi(getenv("BS_CKPT_ABORT_NODE")) == g_rank)) {   /* test hook: die right after the write (BS_CKPT_ABORT_NODE: this node only) */
                printf("mn: node %d: BS_CKPT_ABORT_TREE: exiting after the tree level %d checkpoint\n", g_rank, l); fflush(stdout); _exit(3);
            }
        }
    }
    if (captest) rns_dist_cap_test(0);
}
/* one tree level: the product over the group [g0, g0+g) whose halves A = [g0, g0+half), B = the rest hold the operands */
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half)
{
    double t0 = mem_now();
    mn_group *G = mn_group_span(l, g0, g);                   /* (Phase 12 G: = mn_group_at(l) for the binary schedule) */
    uint64_t v[8] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g }, *all = (uint64_t *)malloc((size_t)g * 8 * 8);
    mn_allgather(G->all[0], v, 8, all);
    int inA = G->me < half; const uint64_t *da = all, *dbb = all + (size_t)half * 8;   /* member 0 describes A, member `half` describes B */
    mdb PA, QA, PB, QB; memset(&PA, 0, sizeof PA); memset(&QA, 0, sizeof QA); memset(&PB, 0, sizeof PB); memset(&QB, 0, sizeof QB);
    PA.n = da[0]; PA.N = da[1]; PA.g0 = (int)da[2]; PA.g = (int)da[3]; QA.n = da[4]; QA.N = da[5]; QA.g0 = (int)da[6]; QA.g = (int)da[7];
    PB.n = dbb[0]; PB.N = dbb[1]; PB.g0 = (int)dbb[2]; PB.g = (int)dbb[3]; QB.n = dbb[4]; QB.N = dbb[5]; QB.g0 = (int)dbb[6]; QB.g = (int)dbb[7];
    if (inA) { PA.sh = P->sh; QA.sh = Q->sh; } else { PB.sh = P->sh; QB.sh = Q->sh; }
    free(all);
    mdb Pn, Qn; memset(&Pn, 0, sizeof Pn); memset(&Qn, 0, sizeof Qn);
    rns_mul_dist_mn(&Pn, &PA, &QB, &PB, G);
    if (mn_tree_early_free > 0) db_free(&P->sh);             /* Phase 14 T1 (E10a): my child's P (PA or PB) is dead once P_n is formed */
    if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d mul1", g_rank, l); mem_live_line(w); }   /* Phase 14 T1: the first product's window; the level's line then covers the second */
    rns_mul_dist_mn(&Qn, &QA, &QB, 0, G);
    db_free(&P->sh); db_free(&Q->sh); *P = Pn; *Q = Qn;
    size_t lo, hi; mdb_share(P, g_rank, &lo, &hi);
    printf("mn: node %d level %d [%d, %d): P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", g_rank, l, g0, g0 + g, P->n, Q->n, lo, hi, mem_now() - t0);
}
/* Phase 12 G: a k-way level (MN_GROUPS: the 9-way top step at 576, 3-way steps ...): the group [g0, g0+g) has nch children of gp
 * nodes each (the last one cut), child i = the members [i gp, min((i+1) gp, g)) holding P_i, Q_i sharded over them.  The
 * combine is Horner's from the top child down (agent X's model prices it so: a 3-way level = 4 products):
 *   (P, Q) = (P_{k-1}, Q_{k-1});  for i = k-2 .. 0:  P = P_i Q + P,  Q = Q_i Q
 * -- k - 1 combines of two products each, every product balanced over all g nodes by the block-cyclic map (the operands
 * sharded over the children's subgroups or over the group), the running P, Q sharded over the group; a child's shares are
 * freed right after its combine, so a node holds at most its child's P, Q, the running pair and the new pair: O(share).
 * For nch = 2 this is tree_level's pair of products with the same operands in the same order. */
static void tree_level_k(mdb *P, mdb *Q, int l, int g0, int g, int gp, int nch)
{
    double t0 = mem_now();
    mn_group *G = mn_group_span(l, g0, g);
    uint64_t v[8] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g }, *all = (uint64_t *)malloc((size_t)g * 8 * 8);
    mn_allgather(G->all[0], v, 8, all);
    int ci = G->me / gp;                                     /* my child */
    mdb Pr, Qr; memset(&Pr, 0, sizeof Pr); memset(&Qr, 0, sizeof Qr); int own = 0;   /* the running pair: child nch-1's (own = 0: the shares are P->sh, Q->sh on its members) */
    { const uint64_t *dd = all + (size_t)(nch - 1) * gp * 8;
      Pr.n = dd[0]; Pr.N = dd[1]; Pr.g0 = (int)dd[2]; Pr.g = (int)dd[3]; Qr.n = dd[4]; Qr.N = dd[5]; Qr.g0 = (int)dd[6]; Qr.g = (int)dd[7];
      if (ci == nch - 1) { Pr.sh = P->sh; Qr.sh = Q->sh; } }
    for (int i = nch - 2; i >= 0; i--) {
        const uint64_t *da = all + (size_t)i * gp * 8;
        mdb PA, QA; memset(&PA, 0, sizeof PA); memset(&QA, 0, sizeof QA);
        PA.n = da[0]; PA.N = da[1]; PA.g0 = (int)da[2]; PA.g = (int)da[3]; QA.n = da[4]; QA.N = da[5]; QA.g0 = (int)da[6]; QA.g = (int)da[7];
        if (ci == i) { PA.sh = P->sh; QA.sh = Q->sh; }
        mdb Pn, Qn; memset(&Pn, 0, sizeof Pn); memset(&Qn, 0, sizeof Qn);
        rns_mul_dist_mn(&Pn, &PA, &Qr, &Pr, G);
        if (mn_tree_early_free > 0) {                        /* Phase 14 T1 (E10a): P_i and P_run are dead once P_n = P_i Q_run + P_run is formed */
            if (ci == i || (!own && ci == nch - 1)) db_free(&P->sh);   /* my child's P_i, or the top child's P (= P_run, not owned) */
            if (own) db_free(&Pr.sh); else memset(&Pr.sh, 0, sizeof Pr.sh);
            memset(&PA.sh, 0, sizeof PA.sh);
        }
        if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d c%d mul1", g_rank, l, nch - 1 - i); mem_live_line(w); }   /* Phase 14 T1: the first product's window */
        rns_mul_dist_mn(&Qn, &QA, &Qr, 0, G);
        if (mem_live_on() && i > 0) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d c%d mul2", g_rank, l, nch - 1 - i); mem_live_line(w); }   /* (the last combine's: the level's line) */
        if (ci == i || (!own && ci == nch - 1)) { db_free(&P->sh); db_free(&Q->sh); memset(&P->sh, 0, sizeof P->sh); memset(&Q->sh, 0, sizeof Q->sh); db_init(&P->sh); db_init(&Q->sh); }   /* my child's shares are used up */
        if (own) { db_free(&Pr.sh); db_free(&Qr.sh); }
        Pr = Pn; Qr = Qn; own = 1;
        if (getenv("ECALC_VERBOSE")) printf("mn: node %d level %d [%d, %d): combine %d of %d: P %zu limbs, Q %zu limbs (%.2f s)\n", g_rank, l, g0, g0 + g, nch - 1 - i, nch - 1, Pr.n, Qr.n, mem_now() - t0);
    }
    free(all);
    if (P->sh.cap) db_free(&P->sh); if (Q->sh.cap) db_free(&Q->sh);
    *P = Pr; *Q = Qr;
    size_t lo, hi; mdb_share(P, g_rank, &lo, &hi);
    printf("mn: node %d level %d [%d, %d) (%d children of %d): P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", g_rank, l, g0, g0 + g, nch, gp, P->n, Q->n, lo, hi, mem_now() - t0);
}
/* M3's end: node 0 assembles the whole number on the host from the shares (over mesh 0); the others send theirs */
void mn_gather_host(bigint *out, const mdb *X)
{
    comm *c = g_cm[0]; size_t lo, hi; mdb_share(X, g_rank, &lo, &hi);
    bigint h; bi_init(&h);
    if (hi > lo) db_to_bi(&h, &X->sh);                       /* sh.n = hi - lo limbs */
    if (g_rank) { if (hi > lo) comm_send(c, 0, h.l, (hi - lo) * 8); bi_free(&h); return; }
    bi_reserve(out, X->N + 1);
    if (hi > lo) memcpy(out->l + lo, h.l, (hi - lo) * 8);
    for (int r = 1; r < g_size; r++) { mdb_share(X, r, &lo, &hi); if (hi > lo) comm_recv(c, r, out->l + lo, (hi - lo) * 8); }
    out->n = X->n; bi_free(&h);
}
static void groups_finalize(void)
{
    for (int l = 0; l < MN_MAXL; l++) if (g_groups[l]) {
        mn_group *G = g_groups[l];
        for (int d = 0; d < NA; d++) {
            if (G->lay[d]) comm_destroy(G->lay[d]);
            if (G->tr[d] && G->tr[d] != G->all[d]) comm_destroy(G->tr[d]);
            for (int k = 0; k < 2; k++) if (g_topo_mesh[l][d][k]) { comm_destroy(g_topo_mesh[l][d][k]); g_topo_mesh[l][d][k] = 0; }
            if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]);
        }
        free(G); g_groups[l] = 0;
    }
    for (int l = 0; l < MN_MAXL; l++) if (g_sched[l]) {   /* Phase 12 G: the schedule's own groups */
        mn_group *G = g_sched[l];
        for (int d = 0; d < NA; d++) { if (G->lay[d]) comm_destroy(G->lay[d]); if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]); }
        free(G); g_sched[l] = 0;
    }
}
