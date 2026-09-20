/* mn.c - the multi-node layer: environment, one TCP mesh per APU thread, the start-up self-test (PLAN.md 17, M1) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>
#include <hip/hip_runtime.h>
#include "mn.h"
#include "ntt.h"
#include "ntt_dist.h"
#include "modarith.h"
#include "mem.h"
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { fprintf(stderr, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); exit(1); } } while (0)
#define NA 4
static int g_rank, g_size = 1; static comm *g_cm[NA];
static const char *g_hosts; static int g_port = 27000;
int mn_rank(void) { return g_rank; }
int mn_size(void) { return g_size; }
comm *mn_comm(int apu) { return g_size > 1 ? g_cm[apu] : 0; }
int mn_init(void)
{
    const char *er = getenv("COMM_RANK"), *es = getenv("COMM_SIZE"), *eh = getenv("COMM_HOSTS"), *ep = getenv("COMM_PORT");
    g_size = es ? atoi(es) : 1; g_rank = er ? atoi(er) : 0;
    if (g_size <= 1) { g_size = 1; g_rank = 0; return 1; }
    if (!eh) { fprintf(stderr, "mn: COMM_SIZE %d needs COMM_HOSTS\n", g_size); exit(1); }
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
    char *h = hosts_of(g0, g); comm *c = comm_tcp_create_at(g_rank - g0, g, h, g_port + 512 * slot + 64 * d + g0); free(h); return c;
}
static int pow2_floor(int g) { int p = 1; while (2 * p <= g) p *= 2; return p; }
/* this node's group at level l >= 1: the nodes [k 2^l, min((k+1) 2^l, size)), k = rank >> l; its meshes are
 * created on first use by all its members together (a singleton group has none) */
mn_group *mn_group_at(int level)
{
    if (level < 1 || level >= MN_MAXL) { fprintf(stderr, "mn_group_at: level %d\n", level); exit(1); }
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
            G->tr[d] = G->gt == G->g ? G->all[d] : (G->me < G->gt ? sub_mesh(G->g0, G->gt, 2 * level, d) : 0);
        }
        HIP_CHECK(hipSetDevice(0));
        printf("mn: node %d: level %d group [%d, %d) (transform nodes %d): meshes connected in %.2f s\n", g_rank, level, G->g0, G->g0 + G->g, G->gt, mem_now() - t0);
    }
    g_groups[level] = G;
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
        if (comm_rank(cm) != r || comm_size(cm) != nr) { fprintf(stderr, "mn_selftest_layered: rank %d/%d, expected %d/%d\n", comm_rank(cm), comm_size(cm), r, nr); exit(1); }
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
#include <unistd.h>
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half);
static int g_cktree = -1;
int mn_ckpt_tree_level(unsigned long N)
{
    if (g_cktree >= 0) return g_cktree;
    if (N) bs_N = N;
    if (g_size <= 1 || !bs_restart || !bs_ckpt_dir) { g_cktree = 0; return 0; }
    uint64_t my = (uint64_t)bs_ckpt_tree_find(bs_N), *all = (uint64_t *)malloc((size_t)g_size * 8), mn = my;
    mn_allgather(g_cm[0], &my, 1, all);
    for (int r = 0; r < g_size; r++) if (all[r] < mn) mn = all[r];
    free(all); g_cktree = (int)mn;
    bs_ckpt_tree_clear(g_cktree);
    printf("mn: node %d: tree checkpoint sets: mine up to level %d, all nodes have level %d\n", g_rank, (int)my, g_cktree);
    return g_cktree;
}
void mn_tree(mdb *P, mdb *Q, dbig *Pleaf, dbig *Qleaf)
{
    memset(P, 0, sizeof *P); memset(Q, 0, sizeof *Q);
    P->sh = *Pleaf; P->n = P->N = Pleaf->n; P->g0 = g_rank; P->g = 1; memset(Pleaf, 0, sizeof *Pleaf);
    Q->sh = *Qleaf; Q->n = Q->N = Qleaf->n; Q->g0 = g_rank; Q->g = 1; memset(Qleaf, 0, sizeof *Qleaf);
    int L = 0; while ((1 << L) < g_size) L++;
    int lr = mn_ckpt_tree_level(0), ck = bs_ckpt_dir && (getenv("BS_CKPT_TREE") ? atoi(getenv("BS_CKPT_TREE")) : 1);   /* M6: resume above level lr; BS_CKPT_TREE=0: no tree sets */
    if (lr > 0) {                                            /* the shares of P, Q after tree level lr, from this node's set */
        double t0 = mem_now(); uint64_t d[10]; dbig ps, qs;
        if (!bs_ckpt_tree_read(lr, bs_N, d, &ps, &qs)) { fprintf(stderr, "mn: node %d: restart from tree level %d failed\n", g_rank, lr); exit(1); }
        db_free(&P->sh); db_free(&Q->sh);
        P->sh = ps; P->n = d[0]; P->N = d[1]; P->g0 = (int)d[2]; P->g = (int)d[3]; Q->sh = qs; Q->n = d[5]; Q->N = d[6]; Q->g0 = (int)d[7]; Q->g = (int)d[8];
        bs_st.t_restart += mem_now() - t0;
        printf("mn: node %d: restart from tree level %d: P %zu limbs (share %zu), Q %zu limbs (share %zu), loaded in %.2f s\n", g_rank, lr, P->n, P->sh.n, Q->n, Q->sh.n, mem_now() - t0);
    }
    int every = getenv("BS_CKPT_TREE_EVERY") ? atoi(getenv("BS_CKPT_TREE_EVERY")) : 1; if (every < 1) every = 1;   /* C6: a set every this many tree levels (the top level always) */
    for (int l = lr + 1; l <= L; l++) {
        int k = g_rank >> l, g0 = k << l, g = (1 << l) < g_size - g0 ? (1 << l) : g_size - g0, half = 1 << (l - 1);
        if (g > half) tree_level(P, Q, l, g0, g, half);      /* (no sibling group: carried up unchanged) */
        if (ck && (l % every == 0 || l == L)) {              /* M6: this node's shares after level l */
            /* C6: the sets below the previous set (g_ckpend) go here, not right after its write: every node wrote
             * g_ckpend before entering the next level, so this barrier waits for the nodes' compute, never for the
             * slowest node's write.  The restart rule (the lowest "highest complete level" over the nodes exists on
             * every node) holds: a node removes below g_ckpend only after every node has it.  The last set's
             * predecessors go at mn_finalize, after the driver's final barrier. */
            if (g_ckpend) { double tb = mem_now(); mn_barrier(); bs_ckpt_tree_remove_below(g_ckpend); g_ckpend = 0; bs_st.t_ckpt += mem_now() - tb; }
            double tc = mem_now(); uint64_t d[10] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, P->sh.n, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g, Q->sh.n };
            size_t bytes = bs_ckpt_tree_write(l, bs_N, d, &P->sh, &Q->sh); double dtc = mem_now() - tc;
            if (bytes) { bs_st.n_ckpt++; bs_st.ckpt_bytes += bytes; bs_st.t_ckpt += dtc; g_ckpend = l; }
            printf("mn: node %d: checkpoint tree level %d -> %s: %.3f GB in %.2f s (%.2f GB/s)%s\n", g_rank, l, bs_ckpt_dir, bytes * 1e-9, dtc, bytes * 1e-9 / (dtc > 0 ? dtc : 1), bytes ? "" : "  FAILED, continuing");
            if (bytes && getenv("BS_CKPT_ABORT_TREE") && atoi(getenv("BS_CKPT_ABORT_TREE")) == l && (!getenv("BS_CKPT_ABORT_NODE") || atoi(getenv("BS_CKPT_ABORT_NODE")) == g_rank)) {   /* test hook: die right after the write (BS_CKPT_ABORT_NODE: this node only) */
                printf("mn: node %d: BS_CKPT_ABORT_TREE: exiting after the tree level %d checkpoint\n", g_rank, l); fflush(stdout); _exit(3);
            }
        }
    }
}
/* one tree level: the product over the group [g0, g0+g) whose halves A = [g0, g0+half), B = the rest hold the operands */
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half)
{
    double t0 = mem_now();
    mn_group *G = mn_group_at(l);
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
    rns_mul_dist_mn(&Qn, &QA, &QB, 0, G);
    db_free(&P->sh); db_free(&Q->sh); *P = Pn; *Q = Qn;
    size_t lo, hi; mdb_share(P, g_rank, &lo, &hi);
    printf("mn: node %d level %d [%d, %d): P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", g_rank, l, g0, g0 + g, P->n, Q->n, lo, hi, mem_now() - t0);
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
            if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]);
        }
        free(G); g_groups[l] = 0;
    }
}
