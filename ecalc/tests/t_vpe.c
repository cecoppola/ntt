/* t_vpe.c - the vPE foundation (stage S1a of the TARGET1 plan; vpe.h, mn.c, comm_xshmem.c).
 *
 *   t_vpe [all|probe|host|stages]
 *     host    no GPU, no network: identity arithmetic (vpe_set_nodes / lanes), the log prefix, VPE_STATE, the "stages not merged" text is NOT run here
 *     stages  ECALC_VPE=1 t_vpe stages: vpe_stages_check() must abort with "ECALC_VPE: stages ... not merged" (rc 3) while stages are missing
 *     probe   the sub-communicator probe (D1): on every PE, the aligned groups of 2 and 4 PEs of a node and the contiguous runs of 3, 6, 12 PEs: barrier,
 *             allreduce_max, allgather_host, alltoallv of random counts on each; prints T_VPE_PROBE PASS|FAIL per process
 *     all     host + probe + the flat communicator (alltoall, alltoallv of random counts incl. zeros, allreduce_max, allgather_host, barrier) + the
 *             tree's group schedule for the PE count against mn_groups_parse; prints T_VPE_RESULT PASS|FAIL per process
 *   One process per node, L = ECALC_VPE_LANES (4) vPE threads each, COMM_TRANSPORT=xshmem XSHMEM_LOOPBACK_PES=L (make XSHMEM=ofi), launched by srun on
 *   1, 2, 3 nodes (4, 8, 12 PEs).  Without COMM_TRANSPORT=xshmem the communicator parts are skipped (SKIP line). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <hip/hip_runtime.h>
#include "../vpe.h"
#include "../mn.h"
#include "../comm.h"
#include "../mem.h"

int mn_groups_parse(int size, int *out, int max);
static int g_fail, g_checks, g_skip_comm;
#define CHK(c, ...) do { __atomic_fetch_add(&g_checks, 1, __ATOMIC_RELAXED); if (!(c)) { __atomic_fetch_add(&g_fail, 1, __ATOMIC_RELAXED); \
    printf("  FAILED %s:%d  %s  pe %d: ", __FILE__, __LINE__, #c, vpe_pe()); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define HIPC(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { printf("HIP %s at %d\n", hipGetErrorString(e_), __LINE__); exit(3); } } while (0)

/* ---- host part ---- */
VPE_STATE(int, cnt); static int g_barrier_sum;
static void state_body(void *unused)
{
    (void)unused;
    VPE(cnt) = 100 + vpe_idx();                          /* each vPE its own cell */
    vpe_node_barrier();
    for (int i = 0; i < vpe_lanes(); i++) CHK(cnt_v[i] == 100 + i, "cell %d = %d", i, cnt_v[i]);
    __atomic_fetch_add(&g_barrier_sum, vpe_idx() + 1, __ATOMIC_RELAXED);
    vpe_node_barrier();
    int L = vpe_lanes(); CHK(g_barrier_sum == L * (L + 1) / 2, "sum %d", g_barrier_sum);   /* nobody passed the barrier early */
    CHK(vpe_lane() == vpe_idx() && vpe_in_run(), "lane/in_run");
    CHK(vpe_pe() == L * vpe_node() + vpe_idx() && vpe_npe() == L * vpe_nnodes(), "identity pe %d npe %d", vpe_pe(), vpe_npe());
    const char *p = ec_log_prefix(); char want[64]; snprintf(want, sizeof want, "pe %d (node %d lane %d): ", vpe_pe(), vpe_node(), vpe_idx());
    CHK(!strcmp(p, want), "prefix '%s' want '%s'", p, want);
    CHK(vpe_node_leader() == (vpe_idx() == 0) && vpe_root() == (vpe_pe() == 0), "leader/root");
}
static void host_part(void)
{
    CHK(vpe_on(), "forced on");
    int L = vpe_lanes(); CHK(L >= 1 && L <= VPE_MAX, "lanes %d", L);
    vpe_set_nodes(2, 3);
    CHK(vpe_node() == 2 && vpe_nnodes() == 3 && vpe_npe() == 3 * L && vpe_pe() == 2 * L, "pe %d npe %d", vpe_pe(), vpe_npe());
    CHK(!strcmp(ec_log_prefix(), ""), "no prefix outside vpe_run");
    CHK(vpe_root() == 0 && vpe_node_leader() == 1, "root/leader on the main thread of node 2");
    vpe_set_nodes(0, 1);
    printf("t_vpe: host part: %d lanes, identity, prefix: done\n", L);
}

/* ---- communicator part ---- */
static size_t cntw(int s, int t) { return (size_t)((s * 5 + t * 3 + (s ^ t)) % 7) * 37; }   /* words from PE s to PE t: 0..222, zeros included */
static uint64_t wv(int s, int t, size_t j) { return ((uint64_t)s << 40) | ((uint64_t)t << 20) | j; }
/* the five collectives on communicator c, whose rank 0 is PE g0 */
static void exercise(comm *c, int g0, const char *tag)
{
    int n = comm_size(c), r = comm_rank(c), pe = vpe_pe();
    CHK(r == pe - g0 && n >= 1, "%s: rank %d size %d (pe %d g0 %d)", tag, r, n, pe, g0);
    comm_barrier(c);
    size_t mx = comm_allreduce_max(c, (size_t)pe + 1); CHK(mx == (size_t)(g0 + n), "%s: allreduce_max %zu want %d", tag, mx, g0 + n);
    uint64_t mine[2] = { (uint64_t)pe * 7 + 3, (uint64_t)pe ^ 0x55 }, *all = (uint64_t *)calloc((size_t)n * 2, 8);
    comm_allgather_host(c, mine, all, 16);
    for (int i = 0; i < n; i++) CHK(all[2 * i] == (uint64_t)(g0 + i) * 7 + 3 && all[2 * i + 1] == (uint64_t)((g0 + i) ^ 0x55), "%s: allgather member %d", tag, i);
    free(all);
    size_t *sc = (size_t *)calloc((size_t)n * 4, sizeof(size_t)), *sd = sc + n, *rc = sc + 2 * n, *rd = sc + 3 * n;
    for (int t = 0; t < n; t++) { sc[t] = 8 * cntw(pe, g0 + t); rc[t] = 8 * cntw(g0 + t, pe); }
    size_t ts = comm_prefix(sc, sd, n), tr = comm_prefix(rc, rd, n);
    uint64_t *hs = (uint64_t *)calloc(ts / 8 + 1, 8), *hr = (uint64_t *)calloc(tr / 8 + 1, 8), *ds, *dr;
    for (int t = 0; t < n; t++) for (size_t j = 0; j < sc[t] / 8; j++) hs[sd[t] / 8 + j] = wv(pe, g0 + t, j);
    HIPC(hipMalloc(&ds, ts + 8)); HIPC(hipMalloc(&dr, tr + 8)); HIPC(hipMemcpy(ds, hs, ts, hipMemcpyHostToDevice));
    hipStream_t st; HIPC(hipStreamCreate(&st));
    comm_alltoallv(c, ds, sc, sd, dr, rc, rd, st); comm_wait(c);
    HIPC(hipMemcpy(hr, dr, tr, hipMemcpyDeviceToHost));
    size_t bad = 0; for (int s = 0; s < n; s++) for (size_t j = 0; j < rc[s] / 8; j++) if (hr[rd[s] / 8 + j] != wv(g0 + s, pe, j)) bad++;
    CHK(bad == 0, "%s: alltoallv %zu words wrong (of %zu)", tag, bad, tr / 8);
    /* equal slabs: 64 words per pair */
    size_t sl = 64; uint64_t *es = (uint64_t *)calloc(sl * n, 8), *er = (uint64_t *)calloc(sl * n, 8), *des, *der;
    for (int t = 0; t < n; t++) for (size_t j = 0; j < sl; j++) es[t * sl + j] = wv(pe, g0 + t, j);
    HIPC(hipMalloc(&des, sl * n * 8)); HIPC(hipMalloc(&der, sl * n * 8)); HIPC(hipMemcpy(des, es, sl * n * 8, hipMemcpyHostToDevice));
    comm_alltoall(c, des, der, sl * 8, st); comm_wait(c);
    HIPC(hipMemcpy(er, der, sl * n * 8, hipMemcpyDeviceToHost));
    bad = 0; for (int s = 0; s < n; s++) for (size_t j = 0; j < sl; j++) if (er[s * sl + j] != wv(g0 + s, pe, j)) bad++;
    CHK(bad == 0, "%s: alltoall %zu words wrong", tag, bad);
    comm_barrier(c);
    HIPC(hipFree(ds)); HIPC(hipFree(dr)); HIPC(hipFree(des)); HIPC(hipFree(der)); HIPC(hipStreamDestroy(st));
    free(sc); free(hs); free(hr); free(es); free(er);
}
static int g_mode_probe, g_mode_all;
/* the partition of the PEs into runs of g (the last one cut): this PE's run, exercised */
static void partition(int g, const char *what)
{
    int npe = vpe_npe(), pe = vpe_pe(), g0 = pe / g * g, gg = g < npe - g0 ? g : npe - g0;
    comm *c = vpe_sub(g0, gg);
    CHK(c != 0, "%s: no communicator for [%d, %d)", what, g0, g0 + gg);
    if (!c) return;
    char tag[64]; snprintf(tag, sizeof tag, "%s [%d,%d)", what, g0, g0 + gg);
    exercise(c, g0, tag);
    if (vpe_node_leader() && g0 == 0) printf("t_vpe: partition by %d (%s): this PE's run [%d, %d) exercised\n", g, what, g0, g0 + gg);
}
static void comm_body(void *unused)
{
    (void)unused;
    int npe = vpe_npe(), L = vpe_lanes();
    comm *flat = vpe_comm();
    CHK(flat && comm_rank(flat) == vpe_pe() && comm_size(flat) == npe, "flat comm rank %d size %d", flat ? comm_rank(flat) : -1, flat ? comm_size(flat) : -1);
    if (!flat) return;
    if (g_mode_all) { exercise(flat, 0, "flat"); if (vpe_root()) printf("t_vpe: flat communicator over %d PEs exercised\n", npe); }
    /* the sub-communicator probe: 2 and 4 PEs of one node (aligned), 3, 6, 12 contiguous runs */
    static const int sizes[] = { 2, 4, 3, 6, 8, 12 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        int g = sizes[i]; if (g > npe) continue;
        if (g <= L && (L % g) == 0) partition(g, "node subgroup"); else partition(g, "run");
    }
    /* the tree's group schedule for this PE count (mn_groups_parse in PE units), every level's group built and exercised */
    if (g_mode_all) {
        int gs[32]; int nl = mn_groups_parse(npe, gs, 31);
        static const int e4[] = { 2, 4 }, e8[] = { 2, 4, 8 }, e12[] = { 2, 4, 12 };
        const int *want = npe == 4 ? e4 : npe == 8 ? e8 : npe == 12 ? e12 : 0; int nw = npe == 4 ? 2 : 3;
        if (want) { CHK(nl == nw, "schedule levels %d want %d", nl, nw); for (int l = 0; l < nl && l < nw; l++) CHK(gs[l] == want[l], "level %d: %d want %d", l + 1, gs[l], want[l]); }
        for (int l = 0; l < nl; l++) {
            int Gl = gs[l], pe = vpe_pe(), g0 = pe / Gl * Gl, gg = Gl < npe - g0 ? Gl : npe - g0;
            comm *c = vpe_sub(g0, gg); CHK(c != 0, "level %d group [%d,%d)", l + 1, g0, g0 + gg);
            if (c) { char tag[48]; snprintf(tag, sizeof tag, "schedule level %d", l + 1); exercise(c, g0, tag); }
        }
        if (vpe_root()) { printf("t_vpe: group schedule for %d PEs:", npe); for (int l = 0; l < nl; l++) printf(" %d", gs[l]); printf("\n"); }
    }
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "all";
    g_mode_probe = !strcmp(mode, "probe"); g_mode_all = !strcmp(mode, "all");
    if (!strcmp(mode, "stages")) {
        if (!vpe_on()) { printf("t_vpe stages: run with ECALC_VPE=1\n"); return 2; }
        vpe_stages_check(); printf("t_vpe stages: no abort (all stages merged)\n"); return 0;
    }
    if (!g_mode_all && !g_mode_probe && strcmp(mode, "host")) { printf("usage: t_vpe [all|probe|host|stages]\n"); return 2; }
    vpe_set_forced(VPE_S_A);
    host_part();
    if (!strcmp(mode, "host")) { printf("\nt_vpe host: %s (%d checks)\n", g_fail ? "FAILED" : "OK", g_checks); return g_fail != 0; }
    vpe_run(state_body, 0);
    const char *tr = getenv("COMM_TRANSPORT");
    g_skip_comm = !tr || strcmp(tr, "xshmem");
    int npe = 0;
    if (g_skip_comm) printf("t_vpe: SKIP the communicator parts (COMM_TRANSPORT=xshmem, XSHMEM_LOOPBACK_PES=%d and make XSHMEM=ofi needed)\n", vpe_lanes());
    else {
        npe = mn_init();
        printf("t_vpe: node %d of %d: mn_init -> %d PEs (%d vPE threads)\n", vpe_node(), vpe_nnodes(), npe, vpe_lanes());
        CHK(npe == vpe_npe() && mn_size() == npe, "npe %d vs %d", npe, vpe_npe());
        vpe_run(comm_body, 0);
        mn_finalize();
    }
    const char *tag = g_mode_probe ? "T_VPE_PROBE" : "T_VPE_RESULT";
    printf("%s %s node %d of %d npe %d checks %d failures %d%s\n", tag, g_fail ? "FAIL" : "PASS", vpe_node(), vpe_nnodes(), npe, g_checks, g_fail, g_skip_comm ? " (comm parts skipped)" : "");
    return g_fail != 0;
}
