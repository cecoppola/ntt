/* mn.c - the multi-node layer: environment, one mesh per APU thread (SHMEM PE sets with COMM_TRANSPORT=shmem);
 * the start-up self-test */
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
#include "vpe.h"                                        /* S1a: virtual PEs (ECALC_VPE) */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define NA 4
static int g_rank, g_size = 1; static comm *g_cm[NA];   /* off path: this node and the nodes; under ECALC_VPE (S1a) g_rank / g_size stay the NODE values -- the PE view is mn_rank() / mn_size() */
/* S1a (ECALC_VPE): lane-indexed state, one entry per vPE (vpe.h): the flat communicator over all PEs, the cached sub-communicators, the tree's groups */
typedef struct { int g0, g, slot, id; comm *c; } subent;
typedef struct { subent e[64]; int n; } sublist;
typedef struct { struct mn_group *grp[16], *sch[16]; } grplist;
VPE_STATE(comm *, flat); VPE_STATE(sublist, subl); VPE_STATE(grplist, gl);
static const char *mn_who(void) { return vpe_on() ? "pe" : "node"; }
static comm *cm0(void);
static int g_shmem;                                   /* COMM_TRANSPORT=shmem or xshmem -- the meshes are strided PE sets over SHMEM (comm_shmem.c) or over xshmem (comm_xshmem.c) */
static int g_xshmem;                                  /* COMM_TRANSPORT=xshmem (make XSHMEM=ofi; the target-shaped engine, loopback PEs): the meshes come from comm_xshmem_create_at */
#ifdef XSHMEM_ENGINE
int comm_xshmem_available(void); int comm_xshmem_init(void); int comm_xshmem_rank(void); comm *comm_xshmem_create_at(int pe_start, int pe_stride, int n, int id);
void comm_xshmem_finalize(void); void comm_xshmem_counters(double *t, long *n, double *bytes);
#endif
static comm *mesh_create(int pe_start, int pe_stride, int n, int id)   /* every mesh of this file: the transport's communicator over the PE set */
{
#ifdef XSHMEM_ENGINE
    if (g_xshmem) return comm_xshmem_create_at(pe_start, pe_stride, n, id);
#endif
    return comm_shmem_create_at(pe_start, pe_stride, n, id);
}
int mn_rank(void) { return vpe_on() ? vpe_pe() : g_rank; }   /* the PE rank under ECALC_VPE, else this node */
/* MN_COMM_MARK=1 (default 0: off) -- at the end of each tree level, of the reciprocal and of the division, node 0 prints
 * this PE's all-to-all traffic since the previous mark: exchanges, GB received, post-to-completion s summed over the APU threads,
 * GB/s per APU thread, and the wall since the mark -- the per-phase fabric rate (E's step-below run: the step's cost vs the
 * fabric's run-to-run variance).  Reads counters only: no collective, no change to the computation. */
void mn_comm_mark(const char *what)
{
    static int on = -1; static double t0, b0, w0; static long n0;
    if (on < 0) { const char *e = getenv("MN_COMM_MARK"); on = e && atoi(e) > 0; w0 = mem_now(); }
    if (!on || !mn_transport_shmem() || (vpe_on() && vpe_lpp() > 1 && vpe_idx())) return;   /* (one set of counters per process: lane 0 of a vPE process; S2: every process is its own) */
    double t, b, w = mem_now(); long n;
#ifdef XSHMEM_ENGINE
    if (g_xshmem) comm_xshmem_counters(&t, &n, &b); else
#endif
    comm_shmem_counters(&t, &n, &b);
    if (mn_rank() == 0) printf("comm-mark %s: %ld exchanges, %.2f GB received, %.2f s post to completion (summed over the APU threads) = %.2f GB/s per APU thread; wall %.2f s\n",
                            what, n - n0, (b - b0) / 1e9, t - t0, t > t0 ? (b - b0) / 1e9 / (t - t0) : 0, w - w0);
    t0 = t; b0 = b; n0 = n; w0 = w;
}
/* D3: MN_WAIT_STATS=1 (default off; comm.h) -- the time this node blocks in the barrier and in comm_wait, per phase: bs, dm,
 * recip (the reciprocal inside dm) and other.  One line per node, summed over the APU threads; then node 0's max and min
 * over the nodes (a collective on mesh 0, run by every node when on).  Reads counters only: the computation is unchanged. */
void mn_wait_stats_print(void)
{
    if (!comm_wst_enabled()) return;
    static const int ord[4] = { WST_BS, WST_DM, WST_RECIP, WST_OTHER };
    static const char *nm[4] = { "bs", "dm", "recip", "other" };
    uint64_t ns[WST_NP][3], n[WST_NP][3]; comm_wst_totals(ns, n);
    printf("wait-stats node %d:", mn_rank());
    for (int i = 0; i < 4; i++) { int p = ord[i];
        printf("%s %s barrier %.3f s (%lu) wait %.3f s (%lu) ready %.3f s (%lu)", i ? " |" : "", nm[i], ns[p][0] * 1e-9, (unsigned long)n[p][0], ns[p][1] * 1e-9, (unsigned long)n[p][1], ns[p][2] * 1e-9, (unsigned long)n[p][2]); }
    printf("\n");
    if (mn_size() > 1) {
        const uint64_t C = 1ULL << 50; uint64_t mx[12], mn[12]; comm *c = cm0();   /* values in microseconds; min = C - max(C - v) */
        for (int i = 0; i < 4; i++) for (int k = 0; k < 3; k++) {
            uint64_t u = ns[ord[i]][k] / 1000; mx[3 * i + k] = comm_allreduce_max(c, u); mn[3 * i + k] = C - comm_allreduce_max(c, C - u); }
        if (mn_rank() == 0) {
            printf("wait-stats max/min over %d nodes:", mn_size());
            for (int i = 0; i < 4; i++) printf("%s %s barrier %.3f/%.3f s wait %.3f/%.3f s ready %.3f/%.3f s", i ? " |" : "", nm[i], mx[3 * i] * 1e-6, mn[3 * i] * 1e-6, mx[3 * i + 1] * 1e-6, mn[3 * i + 1] * 1e-6, mx[3 * i + 2] * 1e-6, mn[3 * i + 2] * 1e-6);
            printf("\n");
        }
    }
}
int mn_size(void) { return vpe_on() ? vpe_npe() : g_size; }   /* the PE count under ECALC_VPE, else the nodes */
comm *mn_comm(int apu) { return vpe_on() ? (mn_size() > 1 ? VPE(flat) : 0) : g_size > 1 ? g_cm[apu] : 0; }   /* vPE: the flat PE communicator of the calling vPE (apu ignored: the lane is the thread's) */
static comm *cm0(void) { return vpe_on() ? VPE(flat) : g_cm[0]; }
/* the PE communicators (vpe.h).  The flat one is made by mn_init; sub-communicators are made on first use by every member PE (local creation, no message) and
 * cached per lane.  The id is a function of (g0, g, slot) alone, so the members agree without an exchange: an aligned power-of-two run (g0 % g == 0)
 * 1 + log2 g + 16 slot (slot < 16, g < 2^15: ids < 256); any other run 256 + 64 slot + g (slot < 4, g < 64: ids 259..511 < MAXID_VPE).  The map is
 * injective over that domain, so two runs share an id only if they have the same (g, slot) -- and then sharing a PE is the fatal "id used twice"
 * (use another slot).  Outside the domain: fatal.  Creation is local, so no PE order is needed; DESTRUCTION is collective (x_destroy barriers) and
 * sub_destroy_all therefore goes by descending id, a total order the members of every communicator agree on, never by creation order. */
__attribute__((unused)) static int sub_id(int g, int g0, int slot)
{
    int lg = 0; while ((1 << lg) < g) lg++;
    if ((1 << lg) == g && g0 % g == 0) return 1 + lg + 16 * slot;
    if (g >= 64 || slot >= 4) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): a run that is not an aligned power of two supports g < 64 and slot < 4\n", g0, g, slot);
    return 256 + 64 * slot + g;
}
comm *vpe_comm(void) { return VPE(flat); }
comm *vpe_sub_slot(int g0, int g, int slot)
{
    if (!vpe_on()) ec_fatal(EC_RC_FATAL, "vpe_sub without ECALC_VPE\n");
    int npe = vpe_npe(), me = vpe_pe();
    if (g0 < 0 || g < 1 || g0 + g > npe || slot < 0 || slot > 15) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): outside the %d PEs\n", g0, g, slot, npe);
    if (me < g0 || me >= g0 + g) return 0;
    if (g0 == 0 && g == npe && slot == 0) return VPE(flat);
    sublist *sl = &VPE(subl);
    for (int i = 0; i < sl->n; i++) if (sl->e[i].g0 == g0 && sl->e[i].g == g && sl->e[i].slot == slot) return sl->e[i].c;
    if (sl->n >= 64) ec_fatal(EC_RC_FATAL, "vpe_sub: more than 64 sub-communicators on one vPE\n");
#ifdef XSHMEM_ENGINE
    { int dev = -1; HIP_CHECK(hipGetDevice(&dev)); if (dev != vpe_device()) ec_fatal(EC_RC_FATAL, "vpe_sub: device %d is not this vPE's (%d)\n", dev, vpe_device()); }
    int id = sub_id(g, g0, slot);
    int ph_ = ec_ph(EC_PH_MESH_CREATE); comm *c = comm_xshmem_create_flat(g0, g, id); ec_ph(ph_);
    sl->e[sl->n].g0 = g0; sl->e[sl->n].g = g; sl->e[sl->n].slot = slot; sl->e[sl->n].c = c; sl->e[sl->n].id = id; sl->n++;
    return c;
#else
    return 0;
#endif
}
comm *vpe_sub(int g0, int g) { return vpe_sub_slot(g0, g, 0); }
int mn_transport_shmem(void) { return g_shmem; }
static int mn_init_i(void);
int mn_init(void) { int ph_ = ec_ph(EC_PH_MN_INIT); int r = mn_init_i(); ec_ph(ph_); return r; }   /* (comm_phase.h) */
/* S1a (ECALC_VPE): the PE view.  xshmem with K = ECALC_VPE_LANES loopback PEs per node-process (XSHMEM_LOOPBACK_PES, make XSHMEM=ofi); the flat communicator
 * over all PEs (rank = PE = K node + lane) is created for every lane here, by this thread with that lane's device current (creation sends nothing); the vPE
 * threads (vpe_run) then use VPE(flat).  Single node (COMM_SIZE unset): the 4 PEs of the process still form a communicator (npe = K). */
/* S2a (ECALC_PE_PER_GPU=1): one process per PE (srun, ppn tasks per node, block distribution); xshmem with K = 1.  COMM_RANK / COMM_SIZE stay the node index / count (mnrun.sh), g_rank / g_size
 * the same; the PE is the engine's PE.  Identity cross-check (fatal): engine PE and PE count against the launch (SLURM_LOCALID, SLURM_NTASKS_PER_NODE, COMM_RANK, COMM_SIZE), so a wrong
 * --distribution cannot silently cross the GPU and NIC maps.  The device, NUMA node and cpus are pe_geo's (engine topology); only that device is touched. */
#ifdef XSHMEM_ENGINE
static int mn_init_ppg(void)
{
    double tx = mem_now();
    int np = comm_xshmem_init();
    if (comm_xshmem_loopback_pes() != 1) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: XSHMEM_LOOPBACK_PES=%d (one process per PE: 1)\n", comm_xshmem_loopback_pes());
    int v[8]; char cpus[512] = "";
    if (comm_xshmem_geo(v, cpus, sizeof cpus)) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: the engine reports no topology\n");
    int ppn = vpe_lanes(), local = vpe_idx(), pe = v[0], npe = v[1];
    if (v[3] != ppn || v[2] != local || pe % ppn != local || npe % ppn || np != npe || comm_xshmem_rank() != pe)
        ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: launch layout mismatch: engine pe %d of %d, local %d, %d per node; launch SLURM_LOCALID %d, %d tasks per node (block distribution, one task per GPU)\n", pe, npe, v[2], v[3], local, ppn);
    int node = pe / ppn, nn = npe / ppn; const char *cr = getenv("COMM_RANK"), *cs = getenv("COMM_SIZE");
    if ((cr && *cr && atoi(cr) != node) || (cs && *cs && atoi(cs) != nn))
        ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: COMM_RANK=%s COMM_SIZE=%s (node units) but the engine places pe %d of %d at node %d of %d\n", cr ? cr : "-", cs ? cs : "-", pe, npe, node, nn);
    if (vpe_early_device() >= 0 && vpe_early_device() != v[4]) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: pe %d started on device %d but the engine's topology gives device %d\n", pe, vpe_early_device(), v[4]);
    g_rank = node; g_size = nn; vpe_set_nodes(node, nn); vpe_set_device(v[4]); vpe_set_thread_hook(comm_xshmem_select);
    int ndev = mem_device_count();
    if (ndev < 1 || v[4] >= ndev) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: pe %d uses device %d, %d visible\n", pe, v[4], ndev);
    HIP_CHECK(hipSetDevice(v[4]));
    vpe_pe_affinity(cpus);
    (void)mem_numa_node_of_device(v[4]);
    int ph_ = ec_ph(EC_PH_MESH_CREATE); VPE(flat) = comm_xshmem_create_flat(0, npe, 0); ec_ph(ph_);
    printf("mn: pe %d of %d (node %d of %d, local %d of %d): device %d of %d visible, numa %d, cxi%d (%d NIC%s), one flat communicator over %d PEs over xshmem (one process per PE): created in %.2f s\n",
           pe, npe, node, nn, local, ppn, v[4], ndev, v[5], v[7], v[6], v[6] == 1 ? "" : "s", npe, mem_now() - tx);
    mn_comm_mark("start (the meshes created)");
    return npe;
}
#endif
static int mn_init_vpe(void)
{
    const char *et = getenv("COMM_TRANSPORT");
    vpe_stages_check();
#ifndef XSHMEM_ENGINE
    ec_fatal(EC_RC_FATAL, "mn: ECALC_VPE=1 needs the xshmem engine (make XSHMEM=ofi, COMM_TRANSPORT=xshmem, XSHMEM_LOOPBACK_PES=%d)\n", vpe_lanes());
    return 1;
#else
    if (!et || strcmp(et, "xshmem")) ec_fatal(EC_RC_FATAL, "mn: ECALC_VPE=1 needs COMM_TRANSPORT=xshmem (XSHMEM_LOOPBACK_PES=%d; make XSHMEM=ofi)\n", vpe_lanes());
    g_xshmem = g_shmem = 1;
    if (!comm_xshmem_available()) ec_fatal(EC_RC_FATAL, "mn: ECALC_VPE=1: the xshmem backend is not available (make XSHMEM=ofi)\n");
    if (vpe_ppg()) return mn_init_ppg();
    double tx = mem_now();
    g_size = comm_xshmem_init(); g_rank = comm_xshmem_rank();
    int L = vpe_lanes();
    if (comm_xshmem_loopback_pes() != L) ec_fatal(EC_RC_FATAL, "mn: ECALC_VPE=1: ECALC_VPE_LANES=%d but XSHMEM_LOOPBACK_PES=%d (they must be equal: vPE = loopback PE)\n", L, comm_xshmem_loopback_pes());
    if (mem_device_count() < L) ec_fatal(EC_RC_FATAL, "mn: ECALC_VPE=1: %d vPEs per process need %d devices, %d visible\n", L, L, mem_device_count());
    vpe_set_nodes(g_rank, g_size); vpe_set_thread_hook(comm_xshmem_select);
    int npe = vpe_npe();
    for (int d = 0; d < L; d++) { HIP_CHECK(hipSetDevice(d)); comm_xshmem_select(d); int ph_ = ec_ph(EC_PH_MESH_CREATE); vpe_t_idx = d; flat_v[d] = comm_xshmem_create_flat(0, npe, 0); ec_ph(ph_); }
    vpe_t_idx = 0; comm_xshmem_select(0); HIP_CHECK(hipSetDevice(0));
    for (int d = 0; d < L; d++) (void)mem_numa_node_of_device(d);   /* prime the NUMA lookup cache while single-threaded (mem.c) */
    printf("mn: node %d of %d: %d vPEs per node, one flat communicator over %d PEs over xshmem: created in %.2f s\n", g_rank, g_size, L, npe, mem_now() - tx);
    mn_comm_mark("start (the meshes created)");
    return npe;
#endif
}
static int mn_init_i(void)
{
    if (vpe_on()) return mn_init_vpe();
    const char *es = getenv("COMM_SIZE"), *et = getenv("COMM_TRANSPORT");
    g_xshmem = et && !strcmp(et, "xshmem") && es && atoi(es) > 1;
#ifndef XSHMEM_ENGINE
    if (g_xshmem) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=xshmem but built without the engine (make XSHMEM=ofi)\n"); }
#endif
    g_shmem = g_xshmem || (et && !strcmp(et, "shmem") && es && atoi(es) > 1);   /* (a single process -- no launcher, COMM_SIZE unset by mnrun.sh -- stays size 1 with the variable exported) */
    if (g_shmem) {                                       /* rank and size from the SHMEM runtime (srun --mpi=pmix / oshrun); COMM_RANK is not needed */
#ifdef XSHMEM_ENGINE
        if (g_xshmem) {
            if (!comm_xshmem_available()) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=xshmem but the xshmem backend is not available (make XSHMEM=ofi)\n"); }
            double tx = mem_now();
            g_size = comm_xshmem_init(); g_rank = comm_xshmem_rank();
            if (g_size <= 1) { g_size = 1; g_rank = 0; comm_xshmem_finalize(); g_xshmem = g_shmem = 0; return 1; }
#pragma omp parallel for num_threads(NA) schedule(static)
            for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); int ph_ = ec_ph(EC_PH_MESH_CREATE); g_cm[d] = mesh_create(0, 1, g_size, d); ec_ph(ph_); }
            HIP_CHECK(hipSetDevice(0));
            printf("mn: node %d of %d, four meshes of %d PEs over xshmem: created in %.2f s\n", g_rank, g_size, g_size, mem_now() - tx);
            mn_comm_mark("start (the meshes created)");
            return g_size;
        }
#endif
        if (!comm_shmem_available()) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=shmem but built without SHMEM (make SHMEM=1)\n"); }
        double t0 = mem_now();
        g_size = comm_shmem_init(); g_rank = comm_shmem_rank();
        if (g_size <= 1) { g_size = 1; g_rank = 0; comm_shmem_finalize(); return 1; }
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); int ph_ = ec_ph(EC_PH_MESH_CREATE); g_cm[d] = mesh_create(0, 1, g_size, d); ec_ph(ph_); }
        HIP_CHECK(hipSetDevice(0));
        printf("mn: node %d of %d, four meshes of %d PEs over SHMEM: created in %.2f s\n", g_rank, g_size, g_size, mem_now() - t0);
        mn_comm_mark("start (the meshes created)");   /* the marks' origin */
        return g_size;
    }
    g_size = es ? atoi(es) : 1; g_rank = 0;
    if (g_size <= 1) { g_size = 1; return 1; }
    ec_fatal(EC_RC_FATAL, "mn: COMM_SIZE %d needs COMM_TRANSPORT=shmem in a SHMEM build (the TCP transport was removed 2026-10-12; mnrun.sh sets it)\n", g_size);
    return g_size;
}
void mn_barrier(void) { if (mn_size() > 1) comm_barrier(cm0()); }   /* vPE: collective over all PEs (every vPE thread calls it) */
static void groups_finalize(void);
static void sub_destroy_all(void)       /* vPE: the cached sub-communicators of this lane, descending id (the members agree on that order), then the flat one */
{
    sublist *sl = &VPE(subl);
    /* x_destroy is collective (a barrier of its members): destroy in DESCENDING ID order, the same total order on every PE, whatever the creation order was */
    for (int done = 0; done < sl->n; done++) {
        int b = -1; for (int i = 0; i < sl->n; i++) if (sl->e[i].c && (b < 0 || sl->e[i].id > sl->e[b].id)) b = i;
        comm_destroy(sl->e[b].c); sl->e[b].c = 0;
    }
    sl->n = 0;
}
static void finalize_vpe_lane(void *unused)
{
    (void)unused;
    groups_finalize();
    if (vpe_ppg()) vpe_node_barrier();                   /* S2: the node's processes agree before the destruction (the node group is one of the cached ones) */
    sub_destroy_all();
    if (VPE(flat)) { comm_destroy(VPE(flat)); VPE(flat) = 0; }
    if (!vpe_ppg()) vpe_node_barrier();
#ifdef XSHMEM_ENGINE
    if (vpe_ppg() || vpe_idx() == 0) comm_xshmem_finalize();   /* S2: every process owns its engine */
#endif
}
void mn_finalize(void)
{
    if (vpe_on()) { if (vpe_in_run()) finalize_vpe_lane(0); else vpe_run(finalize_vpe_lane, 0); return; }   /* collective: every vPE destroys its communicators */
    if (g_size > 1) { groups_finalize(); for (int d = 0; d < NA; d++) if (g_cm[d]) { comm_destroy(g_cm[d]); g_cm[d] = 0; } }
#ifdef XSHMEM_ENGINE
    if (g_size > 1 && g_xshmem) { comm_xshmem_finalize(); return; }
#endif
    if (g_size > 1 && g_shmem) comm_shmem_finalize();
}
/* S1a (ECALC_VPE): the self-tests over PE communicators -- every vPE thread is one rank of one communicator (the flat one, or the first 2^k PEs for the
 * layered test, k the largest with 2^k <= #PE), one plane of one prime, its rows against the one-rank engine; no omp.  The 12-PE case (not a power of two)
 * is covered by the 8 PEs of vpe_sub(0, 8); the R / ranks >= 32 growth of the loops below is kept as it is.  Called from a vPE thread (S1e's flow) or from
 * the main thread (then it runs the vPE threads itself). */
static uint64_t xs(uint64_t *s);
static int selftest_pe_core(comm *cm, int prime, int logR, int logC, int verbose)
{
    int r = comm_rank(cm), nr = comm_size(cm), ok = 1;
    while (((size_t)1 << logR) / (size_t)nr < 32) logR++;
    int logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;
    uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *tmp = (uint64_t *)malloc(rows * 8);
    for (size_t i = 0; i < n; i++) { hx[i] = xs(&seed) % p; hy[i] = xs(&seed) % p; }
    ntt_ctx *ctx = ntt_ctx_create(prime); hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
    uint64_t *dx, *dy; HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
    HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
    ntt_fwd(ctx, dx, logn, 1, s); ntt_fwd(ctx, dy, logn, 1, s); ntt_pw(ctx, dx, dy, n, s); ntt_inv(ctx, dx, logn, 1, s);
    HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
    dist_plan pl; dist_plan_create(&pl, cm, ctx, prime, logR, logC);
    uint64_t *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
    for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
    HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
    for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
    HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
    dist_fwd(&pl, rx, s); dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s); dist_inv(&pl, rx, s);
    HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
    size_t bad = 0;
    for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) if (tmp[il * C + j] != ref[(r * rr + il) + R * j]) bad++;
    if (bad || verbose) printf("%smn: PE rank %d of %d, 2^%d points: %zu of %zu differ\n", ec_log_prefix(), r, nr, logn, bad, rows);
    if (bad) ok = 0;
    dist_plan_free(&pl); HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); HIP_CHECK(hipStreamDestroy(s)); ntt_ctx_free(ctx);
    free(hx); free(hy); free(ref); free(tmp);
    return ok;
}
static int selftest_pe(int logR, int logC, int verbose, int layered)
{
    int npe = vpe_npe(); comm *cm; double t0 = mem_now();
    if (!layered) {
        if (npe & (npe - 1)) { if (vpe_root()) printf("mn: self-test over the plain meshes skipped (%d PEs is not a power of two; the layered self-test covers the transform PEs)\n", npe); return 1; }
        cm = VPE(flat);
    } else {
        int gt = 1; while (2 * gt <= npe) gt *= 2;
        cm = vpe_sub(0, gt); if (!cm) return 1;
    }
    int ok = selftest_pe_core(cm, (logR + logC) & 3, logR, logC, verbose);
    if (vpe_root()) printf("mn: %sself-test over %d PEs at 2^%d+ points: %s (%.2f s)\n", layered ? "layered " : "", comm_size(cm), logR + logC, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
struct stctx { int logR, logC, verbose, layered, ok; };
static void selftest_thunk(void *a) { struct stctx *c = (struct stctx *)a; if (!selftest_pe(c->logR, c->logC, c->verbose, c->layered)) __atomic_store_n(&c->ok, 0, __ATOMIC_RELAXED); }
static int selftest_pe_entry(int logR, int logC, int verbose, int layered)
{
    if (vpe_in_run()) return selftest_pe(logR, logC, verbose, layered);
    struct stctx c = { logR, logC, verbose, layered, 1 }; vpe_run(selftest_thunk, &c); return c.ok;
}
/* self-test: on every APU thread, prime d, a random cyclic convolution of 2^(logR+logC) points from a seed all
 * nodes share; the distributed fwd/pw/inv over mesh d's `size` ranks must equal the one-rank engine on this
 * rank's block-cyclic rows (rank r holds rows [r R/nr, (r+1) R/nr); row i, column j <-> point i + R j) */
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
int mn_selftest(int logR, int logC, int verbose)
{
    if (mn_size() <= 1) return 1;
    if (vpe_on()) return selftest_pe_entry(logR, logC, verbose, 0);
    if (mn_size() & (mn_size() - 1)) { printf("mn: self-test over the plain meshes skipped (size %d is not a power of two; the layered self-test covers the transform nodes)\n", mn_size()); return 1; }
    int nr = mn_size();
    if (getenv("MN_SELFTEST_LOGR")) logR = atoi(getenv("MN_SELFTEST_LOGR"));   /* debug only: lower the starting logR to force R / ranks < 32 at a small rank count -- see the note below on why this cannot
                                                                                 * reach NTT_LOGN_MIN at only 2 ranks; past that point growth (below) still has to engage, so the knob is exercised, not skipped */
    if (logR < NTT_LOGN_MIN) logR = NTT_LOGN_MIN;   /* the column pass is ntt_fwd(..., p->logR, ...) (ntt_dist.c:327): logR itself must be a valid transform size regardless of the rows-per-rank rule below.
                                                      * Only the debug override above can drive it under NTT_LOGN_MIN (10) -- the production default 11 and everything mn_selftest_layered's growth produces
                                                      * are already >=.  Consequence for testing at 2 ranks: R >= 2^NTT_LOGN_MIN = 1024 always, so R / 2 >= 512 -- the
                                                      * small-rows condition (R / ranks < 32) needs ranks > R_min / 32 = 32, i.e. >= 33 node-processes at this floor (>= 65 at the production logR = 11).
                                                      * Neither is reachable on the available hardware (aac7: 13 nodes, <= 4 node-processes/node by default), so the real bug cannot be reproduced end to
                                                      * end here; the growth formula was instead checked by direct arithmetic (matches mn_selftest_layered's identical, already-shipped pattern) and this
                                                      * clamp keeps the debug knob itself from crashing on an invalid override instead of silently exercising the wrong thing. */
    /* (results/OFI_FABRIC.md §P16's growth pattern) the fixed 2^11 rows give R/ranks < 32 from 128
     * ranks on (k_twpack_r's grid is rk/32 blocks, 0 at R/ranks = 16: a silent no-op, not a HIP error).  Grow logR until
     * R/ranks >= 32, as the layered test does; below 128 ranks R/ranks is already >= 32 (2048/64
     * = 32) so the loop never fires and behavior is unchanged. */
    while (((size_t)1 << logR) / (size_t)nr < 32) logR++;
    int ok = 1, logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    if (rr < 32) { fprintf(stderr, "mn_selftest: R / ranks < 32\n"); return 0; }
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        int prime = d, r = mn_rank(); uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;
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

/* ---- node groups, the layered self-test, the distributed top levels of the tree (archive:PLAN §17) ---- */
#include "mdb.h"
#include "dbig.h"
#define MN_MAXL 16
#define g_groups (VPE(gl).grp)                       /* (per vPE under ECALC_VPE; the one array off) */
/* mesh d over the nodes [g0, g0+g): rank = node - g0; slot s (0 = the M1 meshes) gives the PE set its id */
static comm *sub_mesh(int g0, int g, int slot, int d)
{
    if (g0 == 0 && g == mn_size()) return g_cm[d];
    int ph_ = ec_ph(EC_PH_MESH_CREATE); comm *r = mesh_create(g0, 1, g, NA + NA * slot + d); ec_ph(ph_); return r;   /* S: the PE set [g0, g0+g); id unique per (slot, d) -- the base meshes hold ids 0..3 */
}
static int pow2_floor(int g) { int p = 1; while (2 * p <= g) p *= 2; return p; }
/* this node's group at level l >= 1: the nodes [k 2^l, min((k+1) 2^l, size)), k = rank >> l; its meshes are
 * created on first use by all its members together (a singleton group has none) */
mn_group *mn_group_at(int level)
{
    if (level < 1 || level >= MN_MAXL) { ec_fatal(EC_RC_FATAL, "mn_group_at: level %d\n", level); }
    if (g_groups[level]) return g_groups[level];
    mn_group *G = (mn_group *)calloc(1, sizeof *G);
    int k = mn_rank() >> level; G->g0 = k << level; G->g = (1 << level) < mn_size() - G->g0 ? (1 << level) : mn_size() - G->g0;
    G->gt = pow2_floor(G->g); G->me = mn_rank() - G->g0; G->lvl = level;
    if (vpe_on()) {                                      /* S1a: the group of 2^level PEs; all[0] = its PE communicator (the other fields stay unused; S1b's mn_group.pe) */
        if (G->g > 1) G->all[0] = vpe_sub(G->g0, G->g);
        if (vpe_on()) G->pe = G->all[0];   /* S1b: the PE communicator (all[0] == pe; NULL for one PE) */
    } else
    if (G->g > 1) {
        double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) {
            HIP_CHECK(hipSetDevice(d));
            G->all[d] = sub_mesh(G->g0, G->g, 2 * level - 1, d);
            G->tr[d] = G->gt == G->g ? G->all[d] : (G->me < G->gt ? sub_mesh(G->g0, G->gt, 2 * level, d) : 0);
        }
        HIP_CHECK(hipSetDevice(0));
        printf("%smn: %s %d: level %d group [%d, %d) (transform nodes %d): meshes connected in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), level, G->g0, G->g0 + G->g, G->gt, mem_now() - t0);
    }
    g_groups[level] = G;
    return G;
}
/* the tree's schedule: the group [g0, g0+g) of schedule level l (mn_groups_parse: MN_GROUPS).
 * A group that is one of the binary levels' -- G_l a power of two, or the whole machine -- IS mn_group_at's (the same meshes,
 * the same slots: the default schedule runs exactly as before); any other size (3, 6, 192 ...) gets its own meshes on the
 * slot 2 MN_MAXL + l (every node of the group takes part in the transform: no tr, no topo layer -- L's general map) */
#define g_sched (VPE(gl).sch)
mn_group *mn_group_span(int l, int g0, int g)
{
    if (l < 1 || l >= MN_MAXL) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d\n", l); }
    int lv = 0; while ((1 << lv) < g) lv++;
    if (((1 << lv) == g && (g0 & (g - 1)) == 0) || (g0 == 0 && g == mn_size())) {
        if (g0 == 0 && g == mn_size()) { lv = 0; while ((1 << lv) < mn_size()) lv++; }
        mn_group *B = mn_group_at(lv);
        if (B->g0 != g0 || B->g != g) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d [%d, %d) is not the binary group [%d, %d)\n", l, g0, g0 + g, B->g0, B->g0 + B->g); }
        return B;
    }
    if (g_sched[l]) { if (g_sched[l]->g0 != g0 || g_sched[l]->g != g) { ec_fatal(EC_RC_FATAL, "mn_group_span: level %d group changed\n", l); } return g_sched[l]; }
    mn_group *G = (mn_group *)calloc(1, sizeof *G);
    G->g0 = g0; G->g = g; G->gt = pow2_floor(g); G->me = mn_rank() - g0; G->lvl = l; G->sched = 1;
    if (vpe_on()) { G->all[0] = G->pe = vpe_sub(g0, g); g_sched[l] = G; return G; }   /* S1a: the general map over PEs; all[0] = the PE run's communicator */
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d)); G->tr[d] = 0;
        /* a PE-set id of its own (NA + NA (2 MN_MAXL + l) + d) */
        G->all[d] = mesh_create(g0, 1, g, NA + NA * (2 * MN_MAXL + l) + d);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("%smn: %s %d: schedule level %d group [%d, %d) (%d nodes, the general map): meshes connected in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, g, mem_now() - t0);
    g_sched[l] = G;
    return G;
}
/* X2 (COMM_LAYER_INTER2, results/OFI_FABRIC.md §XEFF item 2): the second mesh of the group for APU thread d, over the same PE set as all[d] (sub = 0) or as the
 * first gt nodes' tr[d] (sub = 1), under an id of its own: NA + NA (3 MN_MAXL + idx) + d, idx = 2 MN_MAXL sched + MN_MAXL sub + level (below MAXID 1024).
 * Created on first use, collectively by the members (SHMEM only); destroyed by groups_finalize */
comm *mn_mesh2(mn_group *G, int d, int sub)
{
    comm **slot = sub ? &G->tr2[d] : &G->in2[d];
    if (*slot) return *slot;
    if (vpe_on()) ec_fatal(EC_RC_FATAL, "COMM_LAYER_INTER2 is not available with ECALC_VPE (flat PE communicators)\n");
    if (!g_shmem) ec_fatal(EC_RC_FATAL, "COMM_LAYER_INTER2=1 needs COMM_TRANSPORT=shmem or xshmem\n");
    int idx = (G->sched ? 2 * MN_MAXL : 0) + (sub ? MN_MAXL : 0) + G->lvl;
    *slot = mesh_create(G->g0, 1, sub ? G->gt : G->g, NA + NA * (3 * MN_MAXL + idx) + d);
    if (d == 0) printf("%smn: %s %d: COMM_LAYER_INTER2: second mesh (%s) of level %d%s group [%d, %d), %d nodes\n", ec_log_prefix(), mn_who(), mn_rank(), sub ? "transform nodes" : "all nodes", G->lvl, G->sched ? " schedule" : "", G->g0, G->g0 + G->g, sub ? G->gt : G->g);
    return *slot;
}
/* all-gather of k u64 per node over a mesh: the transport's host all-gather (M7, A-comm; was a point-to-point loop) */
void mn_allgather(comm *c, const uint64_t *v, int k, uint64_t *out) { comm_allgather_host(c, v, out, (size_t)k * 8); }
/* the layered communicator's self-test (the mn_selftest pattern over 4 gt ranks, gt = the largest power of two <= size,
 * rank rho = gt d + node): one plane of one prime spread over all 4 gt ranks (the four APUs of a node hold rows of the
 * same plane, as in the product tier), each rank's distributed convolution against the one-rank engine on its rows */
int mn_selftest_layered(int logR, int logC, int verbose)
{
    if (mn_size() <= 1) return 1;
    if (vpe_on()) return selftest_pe_entry(logR, logC, verbose, 1);
    int L = 0; while ((1 << L) < mn_size()) L++;
    mn_group *G = mn_group_at(L); int gt = G->gt;
    if (mn_rank() >= gt) return 1;
    /* logR grows until R / ranks >= 32 -- the fixed 2^11 rows refuse from 32 PEs on
     * (4 x 32 = 128 ranks, R / ranks = 16), so the rows grow with the rank count */
    while (((size_t)1 << logR) / ((size_t)NA * gt) < 32) logR++;
    int ok = 1, nr = NA * gt, logn = logR + logC; size_t n = (size_t)1 << logn, R = (size_t)1 << logR, C = (size_t)1 << logC, rr = R / nr, rows = n / nr;
    if (rr < 32) { fprintf(stderr, "mn_selftest_layered: R / ranks < 32\n"); return 0; }
    int quick = getenv("MN_SELFTEST") && atoi(getenv("MN_SELFTEST")) == 2;   /* MN_SELFTEST=2: the layered all-to-all pattern alone (no one-rank reference, no plan, no convolution) */
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        int prime = (logR + logC) & 3, r = gt * d + mn_rank(); uint64_t p = ec_P[prime], seed = 0x9E3779B97F4A7C15ull + prime;   /* one plane (one prime) over all 4 gt ranks, as the product tier runs it */
        uint64_t *hx = (uint64_t *)malloc(n * 8), *hy = (uint64_t *)malloc(n * 8), *ref = (uint64_t *)malloc(n * 8), *tmp = (uint64_t *)malloc(rows * 8);
        ntt_ctx *ctx = NULL; hipStream_t s; HIP_CHECK(hipStreamCreate(&s));
        uint64_t *dx = NULL, *dy = NULL;
        if (!quick) {
        for (size_t i = 0; i < n; i++) { hx[i] = xs(&seed) % p; hy[i] = xs(&seed) % p; }
        ctx = ntt_ctx_create(prime);
        HIP_CHECK(hipMalloc(&dx, n * 8)); HIP_CHECK(hipMalloc(&dy, n * 8));
        HIP_CHECK(hipMemcpy(dx, hx, n * 8, hipMemcpyHostToDevice)); HIP_CHECK(hipMemcpy(dy, hy, n * 8, hipMemcpyHostToDevice));
        ntt_fwd(ctx, dx, logn, 1, s); ntt_fwd(ctx, dy, logn, 1, s); ntt_pw(ctx, dx, dy, n, s); ntt_inv(ctx, dx, logn, 1, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(ref, dx, n * 8, hipMemcpyDeviceToHost));
        }
        comm *xg = comm_xgmi_create(d), *cm = comm_layered_create(xg, G->tr[d], d);
        if (comm_layered_inter2_on()) comm_layered_set_inter2(cm, mn_mesh2(G, d, G->tr[d] != G->all[d]));   /* X2 */
        if (comm_rank(cm) != r || comm_size(cm) != nr) { ec_fatal(EC_RC_FATAL, "mn_selftest_layered: rank %d/%d, expected %d/%d\n", comm_rank(cm), comm_size(cm), r, nr); }
        dist_plan pl; if (!quick) dist_plan_create(&pl, cm, ctx, prime, logR, logC);
        uint64_t *rx, *ry; HIP_CHECK(hipMalloc(&rx, rows * 8)); HIP_CHECK(hipMalloc(&ry, rows * 8));
        {   /* the all-to-all alone: slab sigma of rank rho tagged (rho, sigma, i) must arrive as slab rho of rank sigma */
            size_t sl = rows / nr; uint64_t *pat = (uint64_t *)malloc(rows * 8);
            for (int sg = 0; sg < nr; sg++) for (size_t i = 0; i < sl; i++) pat[sg * sl + i] = ((uint64_t)r << 40) | ((uint64_t)sg << 32) | i;
            HIP_CHECK(hipMemcpy(rx, pat, rows * 8, hipMemcpyHostToDevice));
            comm_alltoall(cm, rx, ry, sl * 8, s); comm_wait(cm);
            HIP_CHECK(hipMemcpy(pat, ry, rows * 8, hipMemcpyDeviceToHost));
            size_t bad = 0;
            for (int src = 0; src < nr; src++) for (size_t i = 0; i < sl; i++) { uint64_t want = ((uint64_t)src << 40) | ((uint64_t)r << 32) | i; if (pat[src * sl + i] != want) { if (!bad) printf("%smn: %s %d rank %d: slab %d word %zu = (rank %llu, slab %llu, %llu), want (%d, %d, %zu)\n", ec_log_prefix(), mn_who(), mn_rank(), r, src, i, (unsigned long long)(pat[src * sl + i] >> 40), (unsigned long long)((pat[src * sl + i] >> 32) & 255), (unsigned long long)(pat[src * sl + i] & 0xffffffff), src, r, i); bad++; } }
            if (bad) { printf("%smn: %s %d rank %d: layered all-to-all pattern: %zu of %zu words wrong\n", ec_log_prefix(), mn_who(), mn_rank(), r, bad, rows); ok = 0; }
            free(pat);
        }
        if (!quick) {
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hx[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(rx, tmp, rows * 8, hipMemcpyHostToDevice));
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) tmp[il * C + j] = hy[(r * rr + il) + R * j];
        HIP_CHECK(hipMemcpy(ry, tmp, rows * 8, hipMemcpyHostToDevice));
        dist_fwd(&pl, rx, s); dist_fwd(&pl, ry, s); dist_pw(&pl, rx, ry, s); dist_inv(&pl, rx, s);
        HIP_CHECK(hipStreamSynchronize(s)); HIP_CHECK(hipMemcpy(tmp, rx, rows * 8, hipMemcpyDeviceToHost));
        size_t bad = 0;
        for (size_t il = 0; il < rr; il++) for (size_t j = 0; j < C; j++) if (tmp[il * C + j] != ref[(r * rr + il) + R * j]) bad++;
        if (bad || verbose) printf("%smn: %s %d layered rank %d of %d, 2^%d points: %zu of %zu differ\n", ec_log_prefix(), mn_who(), mn_rank(), r, nr, logn, bad, rows);
        if (bad) ok = 0;
        dist_plan_free(&pl);
        }
        comm_destroy(cm); comm_destroy(xg);
        HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); if (!quick) { HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); } HIP_CHECK(hipStreamDestroy(s)); if (!quick) ntt_ctx_free(ctx);
        free(hx); free(hy); free(ref); free(tmp);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: layered self-test%s, %d x 4 ranks at 2^%d points: %s (%.2f s)\n", quick ? " (quick: all-to-all pattern only)" : "", gt, logn, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
/* the top log2(size) levels of the binary-splitting tree as distributed products: level l pairs the groups A (lower
 * terms) and B (higher terms) of level l-1, P = P_A Q_B + P_B (P_B as the CRT's added operand), Q = Q_A Q_B, each
 * operand sharded over its own group's nodes and the results over the joined group.  The other side's
 * descriptors (n, N, g0, g) come from an all-gather over the group; nothing else is exchanged outside the
 * products.  In: this node's leaf P_r, Q_r (device numbers, taken over); out: shares of P, Q over all nodes. */
#include "binsplit.h"
#include "newton.h"                                   /* newton_mn_pq_hook */
#include <unistd.h>
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half);
static void tree_level_k(mdb *P, mdb *Q, int l, int g0, int g, int gp, int nch);
/* the top tree set in the background (mn_ckpt_bg_mode, set by ecalc.c: 0 = none, 1 = complete, the division
 * waits for P's part before S = P + Q if it must; 2 = budgeted, dropped when the measured disk rate cannot finish a part within
 * mn_ckpt_slack seconds of its release).  The writer reads the shares while the reciprocal and the division run; the hook in
 * newton_mn_divmod lets go of P before S = P + Q and takes Q's share, when the writer still needs it, until mn_ckpt_top_finish
 * (after the output stage).  mn_ckpt_top_finish joins the writer. */
int mn_ckpt_bg_mode = 0; double mn_ckpt_slack = 1.0;
VPE_STATE(bs_ckpt_bg *, topbg); VPE_STATE(int, topbg_on); VPE_STATE(dbig, heldQ);   /* S1a: per vPE */
#define g_topbg VPE(topbg)
#define g_topbg_on VPE(topbg_on)
#define g_heldQ VPE(heldQ)
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
    double tw = 0, tq = 0;
    if (g_topbg) {
        bs_ckpt_bg_release(g_topbg, 1);
        db_free(&g_heldQ);
        char who[64]; snprintf(who, sizeof who, "%smn: %s %d: ", ec_log_prefix(), mn_who(), mn_rank());
        bs_ckpt_bg_join(g_topbg, who, bs_ckpt_dir, &tw, &tq); g_topbg = 0;
    }
    if (mn_rank() == 0) printf("RESULT ecalc ckpt_top s %.6g\nRESULT ecalc ckpt_top_wait s %.6g\n", tw, tq);
}
void mn_tree(mdb *P, mdb *Q, dbig *Pleaf, dbig *Qleaf)
{
    memset(P, 0, sizeof *P); memset(Q, 0, sizeof *Q);
    P->sh = *Pleaf; P->n = P->N = Pleaf->n; P->g0 = mn_rank(); P->g = 1; memset(Pleaf, 0, sizeof *Pleaf);
    Q->sh = *Qleaf; Q->n = Q->N = Qleaf->n; Q->g0 = mn_rank(); Q->g = 1; memset(Qleaf, 0, sizeof *Qleaf);
    dm_switches(); if (mn_rank() == 0) printf("mn: MN_TREE_EARLY_FREE: the tree levels free P_i, P_run between their two products\n");   /* Phase 14 T1 (E10a) */
    int gs[MN_MAXL]; int L = mn_groups_parse(mn_size(), gs, MN_MAXL - 1);   /* the level -> group-size schedule (MN_GROUPS; default 2, 4, ..., size: the binary tree as before) */
    int ck = bs_ckpt_dir && mn_ckpt_bg_mode > 0;            /* the top-level set (ECALC_CKPT_TOP) after the top level */
    if (ck) bs_ckpt_tree_clear(0);                           /* this node's tree sets of an earlier run in the directory go (a set at another level would outlive this run's) */
    int captest = getenv("MN_TREE_LOGN_TEST") ? atoi(getenv("MN_TREE_LOGN_TEST")) : 0;   /* the tree's levels at a lowered plane cap (grids at 10^10 on one node), the division at its own */
    if (captest) rns_dist_cap_test(captest);
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1;       /* this level's group size and the children's (the previous level's) */
        int k = mn_rank() / Gl, g0 = k * Gl, g = Gl < mn_size() - g0 ? Gl : mn_size() - g0, nch = (g + Gp - 1) / Gp;
        if (nch == 2) tree_level(P, Q, l, g0, g, Gp);        /* two children: the pair of products as before (the default schedule: bit for bit, the same groups) */
        else if (nch > 2) tree_level_k(P, Q, l, g0, g, Gp, nch);   /* a k-way level (nch children of Gp nodes): k - 1 combines over the level's group */
        /* (one child: no sibling group, carried up unchanged) */
        if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d", mn_rank(), l); mem_live_line(w); }   /* the pool's live bytes per level */
        if (ck && l == L) {                                  /* this node's shares after the top level, in the background */
            double tc = mem_now(); uint64_t d[10] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, P->sh.n, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g, Q->sh.n };
            g_topbg = bs_ckpt_bg_start(l, bs_N, d, &P->sh, &Q->sh, mn_ckpt_bg_mode == 2, mn_ckpt_slack); g_topbg_on = 1;
            newton_mn_pq_hook = mn_pq_hook;
            printf("%smn: %s %d: checkpoint tree level %d -> %s: %.3f GB in the background%s (started in %.2f s)\n", ec_log_prefix(), mn_who(), mn_rank(), l, bs_ckpt_dir, (P->sh.n + Q->sh.n) * 8e-9,
                   mn_ckpt_bg_mode == 2 ? ", budgeted" : "", mem_now() - tc);
        }
    }
    if (captest) rns_dist_cap_test(0);
}
/* one tree level: the product over the group [g0, g0+g) whose halves A = [g0, g0+half), B = the rest hold the operands */
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int half)
{
    double t0 = mem_now();
    mn_group *G = mn_group_span(l, g0, g);                   /* (= mn_group_at(l) for the binary schedule) */
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
    db_free(&P->sh);                                       /* my child's P (PA or PB) is dead once P_n is formed */
    if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d mul1", mn_rank(), l); mem_live_line(w); }   /* the first product's window; the level's line then covers the second */
    rns_mul_dist_mn(&Qn, &QA, &QB, 0, G);
    db_free(&P->sh); db_free(&Q->sh); *P = Pn; *Q = Qn;
    size_t lo, hi; mdb_share(P, mn_rank(), &lo, &hi);
    printf("%smn: %s %d level %d [%d, %d): P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, P->n, Q->n, lo, hi, mem_now() - t0);
    { char w[48]; snprintf(w, sizeof w, "tree level %d", l); mn_comm_mark(w); }
}
/* a k-way level (MN_GROUPS: the 9-way top step at 576, 3-way steps ...): the group [g0, g0+g) has nch children of gp
 * nodes each (the last one cut), child i = the members [i gp, min((i+1) gp, g)) holding P_i, Q_i sharded over them.  The
 * combine is Horner's from the top child down (a 3-way level = 4 products):
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
        {                                                    /* P_i and P_run are dead once P_n = P_i Q_run + P_run is formed */
            if (ci == i || (!own && ci == nch - 1)) db_free(&P->sh);   /* my child's P_i, or the top child's P (= P_run, not owned) */
            if (own) db_free(&Pr.sh); else memset(&Pr.sh, 0, sizeof Pr.sh);
            memset(&PA.sh, 0, sizeof PA.sh);
        }
        if (mem_live_on()) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d c%d mul1", mn_rank(), l, nch - 1 - i); mem_live_line(w); }   /* the first product's window */
        rns_mul_dist_mn(&Qn, &QA, &Qr, 0, G);
        if (mem_live_on() && i > 0) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d c%d mul2", mn_rank(), l, nch - 1 - i); mem_live_line(w); }   /* (the last combine's: the level's line) */
        if (ci == i || (!own && ci == nch - 1)) { db_free(&P->sh); db_free(&Q->sh); memset(&P->sh, 0, sizeof P->sh); memset(&Q->sh, 0, sizeof Q->sh); db_init(&P->sh); db_init(&Q->sh); }   /* my child's shares are used up */
        if (own) { db_free(&Pr.sh); db_free(&Qr.sh); }
        Pr = Pn; Qr = Qn; own = 1;
        if (getenv("ECALC_VERBOSE")) printf("%smn: %s %d level %d [%d, %d): combine %d of %d: P %zu limbs, Q %zu limbs (%.2f s)\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, nch - 1 - i, nch - 1, Pr.n, Qr.n, mem_now() - t0);
    }
    free(all);
    if (P->sh.cap) db_free(&P->sh); if (Q->sh.cap) db_free(&Q->sh);
    *P = Pr; *Q = Qr;
    size_t lo, hi; mdb_share(P, mn_rank(), &lo, &hi);
    printf("%smn: %s %d level %d [%d, %d) (%d children of %d): P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, nch, gp, P->n, Q->n, lo, hi, mem_now() - t0);
    { char w[48]; snprintf(w, sizeof w, "tree level %d", l); mn_comm_mark(w); }
}
static void groups_finalize(void)
{
    if (vpe_on()) {                                      /* the communicators are the sub cache's (sub_destroy_all) */
        for (int l = 0; l < MN_MAXL; l++) { free(g_groups[l]); g_groups[l] = 0; free(g_sched[l]); g_sched[l] = 0; }
        return;
    }
    for (int l = 0; l < MN_MAXL; l++) if (g_groups[l]) {
        mn_group *G = g_groups[l];
        for (int d = 0; d < NA; d++) {
            if (G->lay[d]) comm_destroy(G->lay[d]);
            if (G->in2[d]) comm_destroy(G->in2[d]);
            if (G->tr2[d]) comm_destroy(G->tr2[d]);
            if (G->tr[d] && G->tr[d] != G->all[d]) comm_destroy(G->tr[d]);
            if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]);
        }
        free(G); g_groups[l] = 0;
    }
    for (int l = 0; l < MN_MAXL; l++) if (g_sched[l]) {   /* the schedule's own groups */
        mn_group *G = g_sched[l];
        for (int d = 0; d < NA; d++) { if (G->lay[d]) comm_destroy(G->lay[d]); if (G->in2[d]) comm_destroy(G->in2[d]); if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]); }
        free(G); g_sched[l] = 0;
    }
}
