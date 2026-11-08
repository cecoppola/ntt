/* mn.c - the multi-node layer: environment, one mesh per APU thread (xshmem PE sets, COMM_TRANSPORT=xshmem);
 * the start-up self-test */
#include <stdio.h>
#include "test_inject.h"
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
#include "memsample.h"                                
#include "vpe.h"                                        /* PE identity (ECALC_PE_PER_GPU) */
#define HIP_CHECK(x) do { hipError_t e_ = (x); if (e_ != hipSuccess) { ec_fatal(e_ == hipErrorOutOfMemory ? EC_RC_OOM : EC_RC_FATAL, "HIP %s at %s:%d\n", hipGetErrorString(e_), __FILE__, __LINE__); } } while (0)
#define NA 4
static int g_rank, g_size = 1; static comm *g_cm[NA];   /* off path: this node and the nodes; under ECALC_PE_PER_GPU g_rank / g_size stay the NODE values -- the PE view is mn_rank / mn_size */
/* this PE's state (one PE per process): the flat communicator over all PEs, the cached sub-communicators, the tree's groups */
typedef struct { int g0, g, slot, id; comm *c; } subent;
typedef struct { subent e[64]; int n; } sublist;
typedef struct { struct mn_group *grp[16], *sch[16]; } grplist;
static comm * flat; static sublist subl; static grplist gl;
static const char *mn_who(void) { return vpe_on() ? "pe" : "node"; }
static comm *cm0(void);
static int g_xshmem;                                  /* COMM_TRANSPORT=xshmem (make XSHMEM=ofi|cray): the meshes are strided PE sets over xshmem (comm_xshmem_create_at) */
#ifdef XSHMEM_ENGINE
int comm_xshmem_available(void); int comm_xshmem_init(void); int comm_xshmem_rank(void); comm *comm_xshmem_create_at(int pe_start, int pe_stride, int n, int id);
void comm_xshmem_finalize(void); void comm_xshmem_counters(double *t, long *n, double *bytes);
#endif
static comm *mesh_create(int pe_start, int pe_stride, int n, int id)   /* every mesh of this file: the transport's communicator over the PE set */
{
#ifdef XSHMEM_ENGINE
    return comm_xshmem_create_at(pe_start, pe_stride, n, id);
#else
    (void)pe_start; (void)pe_stride; (void)n; (void)id; return 0;
#endif
}
int mn_rank(void) { return vpe_on() ? vpe_pe() : g_rank; }   /* the PE rank in process mode, else this node */
/* MN_WAIT_STATS=1 (default off; comm.h) -- the time this node blocks in the barrier and in comm_wait, per phase: bs, dm,
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
int mn_size(void) { return vpe_on() ? vpe_npe() : g_size; }   /* the PE count in process mode, else the nodes */
comm *mn_comm(int apu) { return vpe_on() ? (mn_size() > 1 ? flat : 0) : g_size > 1 ? g_cm[apu] : 0; }   /* PE layout: the flat PE communicator of this PE (apu ignored: one device per PE) */
static comm *cm0(void) { return vpe_on() ? flat : g_cm[0]; }
/* the PE communicators (vpe.h).  The flat one is made by mn_init; sub-communicators are made on first use by every member PE
 * (local creation, no message) and cached per lane.  The id is a function of (g0, g, slot) alone, so the members agree without an
 * exchange: an aligned power-of-two run (g0 % g == 0) 1 + log2 g + 16 slot (slot < 16, g < 2^15: ids < 256); any other run with
 * g < 64 256 + 64 slot + g (slot < 4: ids 259..511); any other run with g >= 64 512 + slot (slot < 31: ids 512..542; only when
 * vpe_big_ids, which sizes the control block 544; callers pass the schedule level so two sizes on one PE differ: mn_group_at
 * level l: slot l (< 16), mn_group_span level l: 15 + l (16..30); id 543 is the t_vpe hs burst block).  The map is injective
 * within a class; two runs may share an id only if they have the same (g, slot) class, and then sharing a PE is a fatal naming
 * both runs (vpe_sub_slot checks its own table first; the engine's "id used twice" is the backstop; use another slot).
 * Outside the domain: fatal.  tools/comm_id_audit.py enumerates every engine partition.  Creation is local, so no PE order is
 * needed; sub_destroy_all goes by descending id, a total order every member agrees on, never by creation order. */
__attribute__((unused)) static int sub_id(int g, int g0, int slot)
{
    int lg = 0; while ((1 << lg) < g) lg++;
    if ((1 << lg) == g && g0 % g == 0) { if (slot >= 16) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): an aligned power-of-two run supports slot < 16\n", g0, g, slot); return 1 + lg + 16 * slot; }
    if (g >= 64) {
        if (slot >= 31) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): a run of >= 64 PEs that is not an aligned power of two supports slot < 31\n", g0, g, slot);
        if (!vpe_big_ids()) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): a run of >= 64 PEs that is not an aligned power of two needs the larger control block (vpe_big_ids: more than 64 PEs and a non-power-of-two PE count or MN_GROUPS set)\n", g0, g, slot);
        return 512 + slot;
    }
    if (slot >= 4) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): a run of < 64 PEs that is not an aligned power of two supports slot < 4\n", g0, g, slot);
    if (slot == 3 && g == 63) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, 63, slot 3): id 511 is the t_vpe hs burst block\n", g0);
    return 256 + 64 * slot + g;
}
comm *vpe_comm(void) { return flat; }
comm *vpe_sub_slot(int g0, int g, int slot)
{
    if (!vpe_on()) ec_fatal(EC_RC_FATAL, "vpe_sub without ECALC_PE_PER_GPU\n");
    int npe = vpe_npe(), me = vpe_pe();
    if (g0 < 0 || g < 1 || g0 + g > npe || slot < 0 || slot > 30) ec_fatal(EC_RC_FATAL, "vpe_sub(%d, %d, slot %d): outside the %d PEs\n", g0, g, slot, npe);
    if (me < g0 || me >= g0 + g) return 0;
    if (g0 == 0 && g == npe && slot == 0) return flat;
    sublist *sl = &subl;
    for (int i = 0; i < sl->n; i++) if (sl->e[i].g0 == g0 && sl->e[i].g == g && sl->e[i].slot == slot) return sl->e[i].c;
    if (sl->n >= 64) ec_fatal(EC_RC_FATAL, "vpe_sub: more than 64 sub-communicators on one vPE\n");
#ifdef XSHMEM_ENGINE
    { int dev = -1; HIP_CHECK(hipGetDevice(&dev)); if (dev != vpe_device()) ec_fatal(EC_RC_FATAL, "vpe_sub: device %d is not this vPE's (%d)\n", dev, vpe_device()); }
    int id = sub_id(g, g0, slot);
    for (int i = 0; i < sl->n; i++) if (sl->e[i].id == id) ec_fatal(EC_RC_FATAL, "vpe_sub: runs [%d, %d) slot %d and [%d, %d) slot %d share the communicator id %d and the PE %d (use another slot)\n", sl->e[i].g0, sl->e[i].g0 + sl->e[i].g, sl->e[i].slot, g0, g0 + g, slot, id, me);
    comm *c = comm_xshmem_create_flat(g0, g, id);
    sl->e[sl->n].g0 = g0; sl->e[sl->n].g = g; sl->e[sl->n].slot = slot; sl->e[sl->n].c = c; sl->e[sl->n].id = id; sl->n++;
    return c;
#else
    return 0;
#endif
}
comm *vpe_sub(int g0, int g) { return vpe_sub_slot(g0, g, 0); }
/* the id slot of an engine group: 0 as always, except a run of >= 64 PEs that is not an aligned power of two (and not the whole machine = the flat communicator), which takes `base` */
static int big_slot(int g0, int g, int base) { return g >= 64 && (((g & (g - 1)) | (g0 % g)) != 0) && !(g0 == 0 && g == vpe_npe()) ? base : 0; }
/* ECALC_PE_PER_GPU=1: one process per PE (srun, ppn tasks per node, block distribution); xshmem with K = 1.  COMM_RANK / COMM_SIZE stay the node index / count (mnrun.sh), g_rank / g_size
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
    flat = comm_xshmem_create_flat(0, npe, 0);
    printf("mn: pe %d of %d (node %d of %d, local %d of %d): device %d of %d visible, numa %d, cxi%d (%d NIC%s), one flat communicator over %d PEs over xshmem (one process per PE): created in %.2f s\n",
           pe, npe, node, nn, local, ppn, v[4], ndev, v[5], v[7], v[6], v[6] == 1 ? "" : "s", npe, mem_now() - tx);
    { const char *tl = getenv("ECALC_INIT_TL"); if (tl && atoi(tl) > 0 && pe == 0) printf("tl          mn_init: transport init + identity checks + the flat communicator %.2f s (pe 0; the engine's own lines are `tl ... xshmem:`)\n", mem_now() - tx); }   /* ECALC_INIT_TL=1 (print only) */
    return npe;
}
#endif
static int mn_init_vpe(void)                              /* ECALC_PE_PER_GPU=1 (vpe_on without it is a unit-test view: no launch, no engine) */
{
    const char *et = getenv("COMM_TRANSPORT");
#ifndef XSHMEM_ENGINE
    ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1 needs the xshmem engine (make XSHMEM=ofi, COMM_TRANSPORT=xshmem)\n");
    return 1;
#else
    if (!et || strcmp(et, "xshmem")) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1 needs COMM_TRANSPORT=xshmem (make XSHMEM=ofi)\n");
    g_xshmem = 1;
    if (!comm_xshmem_available()) ec_fatal(EC_RC_FATAL, "mn: ECALC_PE_PER_GPU=1: the xshmem backend is not available (make XSHMEM=ofi)\n");
    if (!vpe_ppg()) ec_fatal(EC_RC_FATAL, "mn: the PE view needs ECALC_PE_PER_GPU=1 (the thread mode ECALC_VPE was removed)\n");
    return mn_init_ppg();
#endif
}
int mn_init(void)
{
    if (vpe_on()) return mn_init_vpe();
    const char *es = getenv("COMM_SIZE"), *et = getenv("COMM_TRANSPORT");
    g_xshmem = et && !strcmp(et, "xshmem") && es && atoi(es) > 1;
#ifndef XSHMEM_ENGINE
    if (g_xshmem) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=xshmem but built without the engine (make XSHMEM=ofi)\n"); }
#endif
#ifdef XSHMEM_ENGINE
    if (g_xshmem) {
        if (!comm_xshmem_available()) { ec_fatal(EC_RC_FATAL, "mn: COMM_TRANSPORT=xshmem but the xshmem backend is not available (make XSHMEM=ofi)\n"); }
        double tx = mem_now();
        g_size = comm_xshmem_init(); g_rank = comm_xshmem_rank(); double t_eng = mem_now() - tx;
        if (g_size <= 1) { g_size = 1; g_rank = 0; comm_xshmem_finalize(); g_xshmem = 0; return 1; }
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) { HIP_CHECK(hipSetDevice(d)); g_cm[d] = mesh_create(0, 1, g_size, d); }
        HIP_CHECK(hipSetDevice(0));
        printf("mn: node %d of %d, four meshes of %d PEs over xshmem: created in %.2f s\n", g_rank, g_size, g_size, mem_now() - tx);
        { const char *tl = getenv("ECALC_INIT_TL"); if (tl && atoi(tl) > 0 && g_rank == 0) printf("tl          mn_init: transport init %.2f s, four meshes created %.2f s (node 0; the engine's own lines are `tl ... xshmem:`)\n", t_eng, mem_now() - tx - t_eng); }   /* ECALC_INIT_TL=1 (print only) */
        return g_size;
    }
#endif
    g_size = es ? atoi(es) : 1; g_rank = 0;
    if (g_size <= 1) { g_size = 1; return 1; }
    ec_fatal(EC_RC_FATAL, "mn: COMM_SIZE %d needs COMM_TRANSPORT=xshmem (mnrun.sh sets it)\n", g_size);
    return g_size;
}
void mn_barrier(void) { if (mn_size() > 1) comm_barrier(cm0()); }   /* collective over all PEs (every PE calls it) */
static void groups_finalize(void);
static void sub_destroy_all(void)       /* PE layout: the cached sub-communicators of this PE, descending id (the members agree on that order), then the flat one */
{
    sublist *sl = &subl;
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
    vpe_node_barrier();                                  /* the node's processes agree before the destruction (the node group is one of the cached ones) */
    sub_destroy_all();
    if (flat) { comm_destroy(flat); flat = 0; }
#ifdef XSHMEM_ENGINE
    if (vpe_ppg()) comm_xshmem_finalize();               /* every process owns its engine */
#endif
}
void mn_finalize(void)
{
    if (vpe_on()) { finalize_vpe_lane(0); return; }      /* collective: every PE destroys its communicators */
    if (g_size > 1) { groups_finalize(); for (int d = 0; d < NA; d++) if (g_cm[d]) { comm_destroy(g_cm[d]); g_cm[d] = 0; } }
#ifdef XSHMEM_ENGINE
    if (g_size > 1 && g_xshmem) { comm_xshmem_finalize(); return; }
#endif
}
/* Self-tests: a random cyclic convolution of 2^(logR+logC) points through the distributed fwd/pw/inv of communicator cm (one plane of one
 * prime; rank r holds rows [r R/nr, (r+1) R/nr), row i, column j <-> point i + R j) against the one-rank engine on the same rows.  a2a also
 * checks the layered all-to-all pattern; want_rank >= 0 checks the communicator's rank and size.  Runs on the calling thread's device. */
static int pow2_floor(int g) { int p = 1; while (2 * p <= g) p *= 2; return p; }
static uint64_t xs(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }
static int selftest_core(comm *cm, int prime, int want_rank, int want_size, int logR, int logC, int verbose, int a2a)
{
    int r = comm_rank(cm), nr = comm_size(cm), ok = 1;
    if (logR < NTT_LOGN_MIN) logR = NTT_LOGN_MIN;
    while (((size_t)1 << logR) / (size_t)nr < 32) logR++;        /* R / ranks >= 32 (k_twpack_r's grid is rk/32 blocks) */
    if (want_rank >= 0 && (r != want_rank || nr != want_size)) ec_fatal(EC_RC_FATAL, "mn selftest: rank %d/%d, expected %d/%d\n", r, nr, want_rank, want_size);
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
    if (a2a) {   /* slab sigma of rank rho tagged (rho, sigma, i) must arrive as slab rho of rank sigma */
        size_t sl = rows / nr, bad = 0; uint64_t *pat = (uint64_t *)malloc(rows * 8);
        for (int sg = 0; sg < nr; sg++) for (size_t i = 0; i < sl; i++) pat[sg * sl + i] = ((uint64_t)r << 40) | ((uint64_t)sg << 32) | i;
        HIP_CHECK(hipMemcpy(rx, pat, rows * 8, hipMemcpyHostToDevice));
        comm_alltoall(cm, rx, ry, sl * 8, s); comm_wait(cm);
        HIP_CHECK(hipMemcpy(pat, ry, rows * 8, hipMemcpyDeviceToHost));
        for (int src = 0; src < nr; src++) for (size_t i = 0; i < sl; i++) { uint64_t want = ((uint64_t)src << 40) | ((uint64_t)r << 32) | i; if (pat[src * sl + i] != want) { if (!bad) printf("%smn: %s %d rank %d: slab %d word %zu = (rank %llu, slab %llu, %llu), want (%d, %d, %zu)\n", ec_log_prefix(), mn_who(), mn_rank(), r, src, i, (unsigned long long)(pat[src * sl + i] >> 40), (unsigned long long)((pat[src * sl + i] >> 32) & 255), (unsigned long long)(pat[src * sl + i] & 0xffffffff), src, r, i); bad++; } }
        if (bad) { printf("%smn: %s %d rank %d: layered all-to-all pattern: %zu of %zu words wrong\n", ec_log_prefix(), mn_who(), mn_rank(), r, bad, rows); ok = 0; }
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
    if (bad || verbose) printf("%smn: %s %d prime %d rank %d of %d, 2^%d points: %zu of %zu differ\n", ec_log_prefix(), mn_who(), mn_rank(), prime, r, nr, logn, bad, rows);
    if (bad) ok = 0;
    dist_plan_free(&pl); HIP_CHECK(hipFree(rx)); HIP_CHECK(hipFree(ry)); HIP_CHECK(hipFree(dx)); HIP_CHECK(hipFree(dy)); HIP_CHECK(hipStreamDestroy(s)); ntt_ctx_free(ctx);
    free(hx); free(hy); free(ref); free(tmp);
    return ok;
}
/* PE layout: every PE is one rank of the flat communicator, or of the first 2^k PEs for the layered test (2^k <= #PE) */
static int selftest_pe(int logR, int logC, int verbose, int layered)
{
    int npe = vpe_npe(); comm *cm; double t0 = mem_now();
    if (!layered) {
        if (npe & (npe - 1)) { if (vpe_root()) printf("mn: self-test over the plain meshes skipped (%d PEs is not a power of two; the layered self-test covers the transform PEs)\n", npe); return 1; }
        cm = flat;
    } else {
        cm = vpe_sub(0, pow2_floor(npe)); if (!cm) return 1;
    }
    int ok = selftest_core(cm, (logR + logC) & 3, -1, 0, logR, logC, verbose, 0);
    if (vpe_root()) printf("mn: %sself-test over %d PEs at 2^%d+ points: %s (%.2f s)\n", layered ? "layered " : "", comm_size(cm), logR + logC, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
/* node layout, on every APU thread d: prime d over mesh d's `size` ranks */
int mn_selftest(int logR, int logC, int verbose)
{
    if (mn_size() <= 1) return 1;
    if (vpe_on()) return selftest_pe(logR, logC, verbose, 0);
    if (mn_size() & (mn_size() - 1)) { printf("mn: self-test over the plain meshes skipped (size %d is not a power of two; the layered self-test covers the transform nodes)\n", mn_size()); return 1; }
    int ok = 1; double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        ok = selftest_core(g_cm[d], d, -1, 0, logR, logC, verbose, 0) && ok;
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: self-test, four meshes over %d nodes at 2^%d+ points: %s (%.2f s)\n", mn_size(), logR + logC, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}

/* ---- node groups, the layered self-test, the distributed top levels of the tree ---- */
#include "mdb.h"
#include "dbig.h"
#define MN_MAXL 16
#define g_groups (gl.grp)                    
/* mesh d over the nodes [g0, g0+g): rank = node - g0; slot s (0 = the M1 meshes) gives the PE set its id */
static comm *sub_mesh(int g0, int g, int slot, int d)
{
    if (g0 == 0 && g == mn_size()) return g_cm[d];
    comm *r = mesh_create(g0, 1, g, NA + NA * slot + d); return r;   /* S: the PE set [g0, g0+g); id unique per (slot, d) -- the base meshes hold ids 0..3 */
}
/* this node's group at level l >= 1: the nodes [k 2^l, min((k+1) 2^l, size)), k = rank >> l; its meshes are
 * created on first use by all its members together (a singleton group has none) */
mn_group *mn_group_at(int level)
{
    if (level < 1 || level >= MN_MAXL) { ec_fatal(EC_RC_FATAL, "mn_group_at: level %d\n", level); }
    if (g_groups[level]) return g_groups[level];
    mn_group *G = (mn_group *)calloc(1, sizeof *G);
    int k = mn_rank() >> level; G->g0 = k << level; G->g = (1 << level) < mn_size() - G->g0 ? (1 << level) : mn_size() - G->g0;
    G->me = mn_rank() - G->g0; G->lvl = level;
    if (vpe_on()) {                                      /* the group of 2^level PEs; all[0] = its PE communicator (the other fields stay unused) */
        if (G->g > 1) G->all[0] = vpe_sub_slot(G->g0, G->g, big_slot(G->g0, G->g, level));
        G->pe = G->all[0];   /* (NULL for one PE) */
    } else
    if (G->g > 1) {
        double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
        for (int d = 0; d < NA; d++) {
            HIP_CHECK(hipSetDevice(d));
            G->all[d] = sub_mesh(G->g0, G->g, 2 * level - 1, d);
            G->tr[d] = pow2_floor(G->g) == G->g ? G->all[d] : (G->me < pow2_floor(G->g) ? sub_mesh(G->g0, pow2_floor(G->g), 2 * level, d) : 0);
        }
        HIP_CHECK(hipSetDevice(0));
        printf("%smn: %s %d: level %d group [%d, %d) (transform nodes %d): meshes connected in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), level, G->g0, G->g0 + G->g, pow2_floor(G->g), mem_now() - t0);
    }
    g_groups[level] = G;
    return G;
}
/* the tree's schedule: the group [g0, g0+g) of schedule level l (mn_groups_parse: MN_GROUPS).
 * A group that is one of the binary levels' -- G_l a power of two, or the whole machine -- IS mn_group_at's (the same meshes,
 * the same slots: the default schedule runs exactly as before); any other size (3, 6, 192 ...) gets its own meshes on the
 * slot 2 MN_MAXL + l (every node of the group takes part in the transform: no tr, no topo layer -- L's general map) */
#define g_sched (gl.sch)
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
    G->g0 = g0; G->g = g; G->me = mn_rank() - g0; G->lvl = l; G->sched = 1;
    if (vpe_on()) { G->all[0] = G->pe = vpe_sub_slot(g0, g, big_slot(g0, g, 15 + l)); g_sched[l] = G; return G; }   /* the general map over PEs; all[0] = the PE run's communicator */
    double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        /* a PE-set id of its own (NA + NA (2 MN_MAXL + l) + d) */
        G->all[d] = mesh_create(g0, 1, g, NA + NA * (2 * MN_MAXL + l) + d);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("%smn: %s %d: schedule level %d group [%d, %d) (%d nodes, the general map): meshes connected in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, g, mem_now() - t0);
    g_sched[l] = G;
    return G;
}
/* all-gather of k u64 per node over a mesh: the transport's host all-gather (M7, A-comm; was a point-to-point loop) */
void mn_allgather(comm *c, const uint64_t *v, int k, uint64_t *out) { comm_allgather_host(c, v, out, (size_t)k * 8); }
/* the layered communicator over 4 x gt ranks (gt = the largest power of two <= size, rank rho = gt d + node): one plane of one prime
 * spread over all 4 gt ranks, as in the product tier */
int mn_selftest_layered(int logR, int logC, int verbose)
{
    if (mn_size() <= 1) return 1;
    if (vpe_on()) return selftest_pe(logR, logC, verbose, 1);
    int L = 0; while ((1 << L) < mn_size()) L++;
    mn_group *G = mn_group_at(L); int gt = pow2_floor(G->g);
    if (mn_rank() >= gt) return 1;
    int ok = 1; double t0 = mem_now();
#pragma omp parallel for num_threads(NA) schedule(static) reduction(&&:ok)
    for (int d = 0; d < NA; d++) {
        HIP_CHECK(hipSetDevice(d));
        comm *xg = comm_xgmi_create(d), *cm = comm_layered_create(xg, G->tr[d], d);
        ok = selftest_core(cm, (logR + logC) & 3, gt * d + mn_rank(), NA * gt, logR, logC, verbose, 1) && ok;
        comm_destroy(cm); comm_destroy(xg);
    }
    HIP_CHECK(hipSetDevice(0));
    printf("mn: layered self-test, %d x 4 ranks at 2^%d+ points: %s (%.2f s)\n", gt, logR + logC, ok ? "ok" : "FAILED", mem_now() - t0);
    return ok;
}
/* the top log2(size) levels of the binary-splitting tree as distributed products: level l pairs the groups A (lower terms) and B
 * (higher terms) of level l-1, P = P_A Q_B + P_B (P_B as the CRT's added operand), Q = Q_A Q_B, each operand sharded over its own
 * group's nodes and the results over the joined group.  The other side's descriptors (n, N, g0, g) come from an all-gather over the
 * group; nothing else is exchanged outside the products.  In: this node's leaf P_r, Q_r (device numbers, taken over); out: shares
 * of P, Q over all nodes. */
#include "binsplit.h"
#include "newton.h"                                   /* newton_mn_pq_hook */
#include "mn_plan.h"                                  /* MN_LEVEL_UNIFORM */
#include "dr.h"                                       /* the tracked residues of the tree, the leaf check */
#include "verify.h"                                   /* t1_q */
#include <unistd.h>
/* ---- the tree's tracked residues (ECALC_DR >= 2) (B) and the leaf check (C) ----
 * tp / tq: R(P), R(Q) (mod DR_Q = t1_q[0]) of the node's current P, Q (the common value of its child group, held in an explicit
 * variable here, never in the mdb) and this node's share parts.  Seeded from the leaf at the start of the tree (db_mod_qs: the only
 * extra pass), carried through the levels by the descriptor all-gather (11 words) and the checked products.  C: the seeds against
 * the node's recurrence (pq_bg_run, ecalc.c; the T1 values of this node's term range): polled after every level, finally at the
 * division. */
static struct { int on, checked, deferred; uint64_t seed_p, seed_q; int top_valid; uint64_t top_p, top_q; } g_dr;
static int (*g_leaf_poll)(void *arg, uint64_t *p, uint64_t *q); static void *g_leaf_arg;
void mn_dr_leaf_ref(int (*poll)(void *arg, uint64_t *p, uint64_t *q), void *arg) { g_leaf_poll = poll; g_leaf_arg = arg; }
static void dr_leaf_check(int mode)   /* mode 0: after a level (non-blocking); 1: the division's start (non-blocking: a recurrence still running defers the check, so it keeps overlapping the division); 2: the join point (ecalc.c out_stage, the recurrence is joined) */
{
    if (!g_dr.on || g_dr.checked || !g_leaf_poll) return;
    uint64_t p = 0, q = 0; int st = g_leaf_poll(g_leaf_arg, &p, &q);   /* poll == 1: finished (p, q valid), 0: running, -1: never started */
    if (st < 0) { if (mode == 2) { g_dr.checked = 1; dr_count(DR_C_LEAF_SKIP, 1); printf("mn: %s %d: DR: leaf check skipped: the recurrence never started (the T1 values are computed directly)\n", mn_who(), mn_rank()); fflush(stdout); } return; }
    if (st == 0) { if (mode == 1 && !g_dr.deferred) { g_dr.deferred = 1; dr_count(DR_C_LEAF_DEFER, 1); } return; }
    g_dr.checked = 1; dr_count(DR_C_LEAF, 1);
    if (p != g_dr.seed_p || q != g_dr.seed_q) {
        dr_count(DR_C_FATAL, 1); dr_summary();
        ec_fatal(EC_RC_FATAL, "mn: DR: leaf check failed on %s %d: the residues of the node's leaf P_r, Q_r (%llu, %llu) differ from its recurrence's (%llu, %llu): the leaf is corrupted (kind 2, a wrong in-range limb); the run stops\n",
                 mn_who(), mn_rank(), (unsigned long long)g_dr.seed_p, (unsigned long long)g_dr.seed_q, (unsigned long long)p, (unsigned long long)q);
    }
}
void mn_dr_leaf_final(void) { dr_leaf_check(2); }
static int g_dr_div_seen;
int mn_dr_division_seen(void) { return __atomic_load_n(&g_dr_div_seen, __ATOMIC_ACQUIRE); }
/* newton_db.c mn_divmod_dkm, with the residues of the sharded P, Q it computes for T1: the leaf check if not yet done, then the tree's tracked values (a mismatch = corruption at rest between the tree and here) */
void mn_dr_division_check(const uint64_t *qs, const uint64_t *pres, const uint64_t *qres)
{
    __atomic_store_n(&g_dr_div_seen, 1, __ATOMIC_RELEASE);   /* also when tracking is off, so leafslow does not wait for it */
    if (!g_dr.on) return;
    dr_leaf_check(1);                                 /* only if the recurrence is done; else deferred to mn_dr_leaf_final */
    if (!g_dr.top_valid) return;
    g_dr.top_valid = 0;
    if (qs[0] != DR_Q) return;
    dr_count(DR_C_LEAF, 1);
    if (pres[0] != g_dr.top_p || qres[0] != g_dr.top_q) {
        dr_count(DR_C_FATAL, 1); dr_summary();
        ec_fatal(EC_RC_FATAL, "mn: DR: at the division's start the residues of the tree's P, Q (%llu, %llu) differ from the tracked values (%llu, %llu) on %s %d: a stored share changed between the tree's last product and here (corruption at rest); the run stops\n",
                 (unsigned long long)pres[0], (unsigned long long)qres[0], (unsigned long long)g_dr.top_p, (unsigned long long)g_dr.top_q, mn_who(), mn_rank());
    }
}
static mdb_trk trk_from(uint64_t r, const mdb_trk *mine, int held) { mdb_trk t; memset(&t, 0, sizeof t); t.r = r; if (held && mine) { t.held = mine->held; memcpy(t.part, mine->part, sizeof t.part); } return t; }
static int g_lv_tab[4 * 64], g_lv_on;                  /* MN_LEVEL_UNIFORM: this level's largest grid per product (mn_plan.c mn_level_grids), the same on every PE */
static void lv_force(int pi) { if (g_lv_on) rns_dist_mn_force(g_lv_tab[2 * pi], g_lv_tab[2 * pi + 1]); }
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int gp, int nch, mdb_trk *tp, mdb_trk *tq);
/* the top tree set in the background (mn_ckpt_bg_mode, set by ecalc.c: 0 = none, 1 = complete, the division waits for P's part
 * before S = P + Q if it must; 2 = budgeted, dropped when the measured disk rate cannot finish a part within mn_ckpt_slack seconds
 * of its release).  The writer reads the shares while the reciprocal and the division run; the hook in newton_mn_divmod lets go of
 * P before S = P + Q and takes Q's share, when the writer still needs it, until mn_ckpt_top_finish (after the output stage),
 * which joins the writer. */
int mn_ckpt_bg_mode = 0; double mn_ckpt_slack = 1.0;
static bs_ckpt_bg *g_topbg; static int g_topbg_on; static dbig g_heldQ;
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
    dm_switches(); if (mn_rank() == 0) printf("mn: MN_TREE_EARLY_FREE: the tree levels free P_i, P_run between their two products\n");   
    int gs[MN_MAXL]; int L = mn_groups_parse(mn_size(), gs, MN_MAXL - 1);   /* the level -> group-size schedule (MN_GROUPS; default 2, 4, ..., size: the binary tree as before) */
    int ck = bs_ckpt_dir && mn_ckpt_bg_mode > 0;            /* the top-level set (ECALC_CKPT_TOP) after the top level */
    if (ck) bs_ckpt_tree_clear(0);                           /* this node's tree sets of an earlier run in the directory go (a set at another level would outlive this run's) */
    mdb_trk tp, tq; memset(&tp, 0, sizeof tp); memset(&tq, 0, sizeof tq);
    memset(&g_dr, 0, sizeof g_dr);
    if (dr_level() >= 2 && mn_size() > 1) {                 /* C3 step 3: seed the tracked residues from the leaf (after the leaf injection) */
        if (t1_q[0] != DR_Q) ec_fatal(EC_RC_FATAL, "mn_tree: DR: t1_q[0] = %llu is not DR_Q\n", (unsigned long long)t1_q[0]);
        if (P->sh.n && dr_inject(DR_LEAF, 1, mn_rank(), 0, 0)) { uint64_t v = db_limb(&P->sh, P->sh.n / 2); db_set_limb(&P->sh, P->sh.n / 2, v ? v - 1 : 1); }   /* ECALC_TEST dr=leaf: one limb of P_r wrong */
        mdb_trk_make(P, 0, &tp); mdb_trk_make(Q, 0, &tq);
        g_dr.on = 1; g_dr.seed_p = tp.r; g_dr.seed_q = tq.r; dr_arm();
        dr_leaf_check(0);
    }
    int captest = (int)ec_test_long("tree_logn", 0);   /* the tree's levels at a lowered plane cap (grids at 10^10 on one node), the division at its own */
    if (captest) rns_dist_cap_test(captest);
    for (int l = 1; l <= L; l++) {
        int Gl = gs[l - 1], Gp = l > 1 ? gs[l - 2] : 1;       /* this level's group size and the children's (the previous level's) */
        int k = mn_rank() / Gl, g0 = k * Gl, g = Gl < mn_size() - g0 ? Gl : mn_size() - g0, nch = (g + Gp - 1) / Gp;
        g_lv_on = mn_level_uniform() && nch >= 2 && mn_level_grids(bs_N, mn_size(), Gl, Gp, g_lv_tab, 64) > 0;   /* every group of the level at the level's largest grid (a floor: no communication) */
        if (nch >= 2) tree_level(P, Q, l, g0, g, Gp, nch, &tp, &tq);   /* nch children of Gp nodes: nch - 1 combines over the level's group (one child: carried up unchanged) */
        if (g_lv_on) { g_lv_on = 0; rns_dist_mn_force(0, 0); }
        dr_leaf_check(0);                                    /* C: the recurrence may be done by now (non-blocking) */
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
    if (g_dr.on) { g_dr.top_valid = 1; g_dr.top_p = tp.r; g_dr.top_q = tq.r; }
}
/* one tree level (MN_GROUPS): the group [g0, g0+g) has nch children of gp nodes each (the last one cut), child i = the members
 * [i gp, min((i+1) gp, g)) holding P_i, Q_i sharded over them.  The combine is Horner's from the top child down (a 3-way level =
 * 4 products):  (P, Q) = (P_{k-1}, Q_{k-1});  for i = k-2 .. 0:  P = P_i Q + P,  Q = Q_i Q
 * -- k - 1 combines of two products each, every product balanced over all g nodes by the block-cyclic map; a child's shares are
 * freed right after its combine, so a node holds at most its child's P, Q, the running pair and the new pair: O(share). */
static void tree_level(mdb *P, mdb *Q, int l, int g0, int g, int gp, int nch, mdb_trk *tp, mdb_trk *tq)
{
    double t0 = mem_now();
    mn_group *G = mn_group_span(l, g0, g);
    const int chk = g_dr.on, nw = chk ? 11 : 8;              /* ECALC_DR >= 2: R(P), R(Q) and the tracking flag ride along */
    uint64_t v[11] = { P->n, P->N, (uint64_t)P->g0, (uint64_t)P->g, Q->n, Q->N, (uint64_t)Q->g0, (uint64_t)Q->g, chk ? tp->r : 0, chk ? tq->r : 0, (uint64_t)chk }, *all = (uint64_t *)malloc((size_t)g * nw * 8);
    mn_allgather(G->all[0], v, nw, all);
    int use = chk; for (int r = 0; r < g; r++) if (chk && !all[(size_t)r * nw + 10]) use = 0;
    mdb_trk tPr, tQr, tPA, tQA, tPn, tQn; memset(&tPr, 0, sizeof tPr); memset(&tQr, 0, sizeof tQr);   /* the running pair's and the child's tracked residues */
    int ci = G->me / gp;                                     /* my child */
    mdb Pr, Qr; memset(&Pr, 0, sizeof Pr); memset(&Qr, 0, sizeof Qr); int own = 0;   /* the running pair: child nch-1's (own = 0: the shares are P->sh, Q->sh on its members) */
    { const uint64_t *dd = all + (size_t)(nch - 1) * gp * nw;
      Pr.n = dd[0]; Pr.N = dd[1]; Pr.g0 = (int)dd[2]; Pr.g = (int)dd[3]; Qr.n = dd[4]; Qr.N = dd[5]; Qr.g0 = (int)dd[6]; Qr.g = (int)dd[7];
      if (ci == nch - 1) { Pr.sh = P->sh; Qr.sh = Q->sh; }
      if (use) { tPr = trk_from(dd[8], tp, ci == nch - 1); tQr = trk_from(dd[9], tq, ci == nch - 1); } }
    for (int i = nch - 2; i >= 0; i--) {
        const uint64_t *da = all + (size_t)i * gp * nw;
        mdb PA, QA; memset(&PA, 0, sizeof PA); memset(&QA, 0, sizeof QA);
        PA.n = da[0]; PA.N = da[1]; PA.g0 = (int)da[2]; PA.g = (int)da[3]; QA.n = da[4]; QA.N = da[5]; QA.g0 = (int)da[6]; QA.g = (int)da[7];
        if (ci == i) { PA.sh = P->sh; QA.sh = Q->sh; }
        if (use) { tPA = trk_from(da[8], tp, ci == i); tQA = trk_from(da[9], tq, ci == i); }
        mdb Pn, Qn; memset(&Pn, 0, sizeof Pn); memset(&Qn, 0, sizeof Qn);
        lv_force(2 * (nch - 2 - i));
        if (use) rns_mul_dist_mn_chk(&Pn, &PA, &Qr, &Pr, G, &tPA, &tQr, &tPr, &tPn); else rns_mul_dist_mn(&Pn, &PA, &Qr, &Pr, G);
        {                                                    /* P_i and P_run are dead once P_n = P_i Q_run + P_run is formed */
            if (ci == i || (!own && ci == nch - 1)) db_free(&P->sh);   /* my child's P_i, or the top child's P (= P_run, not owned) */
            if (own) db_free(&Pr.sh); else memset(&Pr.sh, 0, sizeof Pr.sh);
            memset(&PA.sh, 0, sizeof PA.sh);
        }
        if (mem_live_on()) { char w[64]; if (nch == 2) snprintf(w, sizeof w, "mn node %d tree level %d mul1", mn_rank(), l); else snprintf(w, sizeof w, "mn node %d tree level %d c%d mul1", mn_rank(), l, nch - 1 - i); mem_live_line(w); }   /* the first product's window */
        if (use) mdb_dr_store_inject(nch == 2 ? Q : &Qr, mn_rank());   /* ECALC_TEST dr=store, every node calls (the occurrence count must agree): at nch = 2 my child's Q, else the running Q, wrong at rest after the first product */
        lv_force(2 * (nch - 2 - i) + 1);
        if (use) rns_mul_dist_mn_chk(&Qn, &QA, &Qr, 0, G, &tQA, &tQr, 0, &tQn); else rns_mul_dist_mn(&Qn, &QA, &Qr, 0, G);
        if (mem_live_on() && i > 0) { char w[64]; snprintf(w, sizeof w, "mn node %d tree level %d c%d mul2", mn_rank(), l, nch - 1 - i); mem_live_line(w); }   /* (the last combine's: the level's line) */
        if (ci == i || (!own && ci == nch - 1)) { db_free(&P->sh); db_free(&Q->sh); memset(&P->sh, 0, sizeof P->sh); memset(&Q->sh, 0, sizeof Q->sh); db_init(&P->sh); db_init(&Q->sh); }   /* my child's shares are used up */
        if (own) { db_free(&Pr.sh); db_free(&Qr.sh); }
        Pr = Pn; Qr = Qn; own = 1;
        if (use) { tPr = tPn; tQr = tQn; }
        if (nch > 2 && getenv("ECALC_VERBOSE")) printf("%smn: %s %d level %d [%d, %d): combine %d of %d: P %zu limbs, Q %zu limbs (%.2f s)\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, nch - 1 - i, nch - 1, Pr.n, Qr.n, mem_now() - t0);
    }
    free(all);
    if (P->sh.cap) db_free(&P->sh); if (Q->sh.cap) db_free(&Q->sh);
    *P = Pr; *Q = Qr;
    if (use) { *tp = tPr; *tq = tQr; } else if (chk) g_dr.on = 0;
    size_t lo, hi; mdb_share(P, mn_rank(), &lo, &hi);
    char kw[48] = ""; if (nch > 2) snprintf(kw, sizeof kw, " (%d children of %d)", nch, gp);
    printf("%smn: %s %d level %d [%d, %d)%s: P %zu limbs, Q %zu limbs (my share of P [%zu, %zu)) in %.2f s\n", ec_log_prefix(), mn_who(), mn_rank(), l, g0, g0 + g, kw, P->n, Q->n, lo, hi, mem_now() - t0);
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
            if (G->tr[d] && G->tr[d] != G->all[d]) comm_destroy(G->tr[d]);
            if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]);
        }
        free(G); g_groups[l] = 0;
    }
    for (int l = 0; l < MN_MAXL; l++) if (g_sched[l]) {   /* the schedule's own groups */
        mn_group *G = g_sched[l];
        for (int d = 0; d < NA; d++) { if (G->lay[d]) comm_destroy(G->lay[d]); if (G->all[d] && G->all[d] != g_cm[d]) comm_destroy(G->all[d]); }
        free(G); g_sched[l] = 0;
    }
}
