/* vpe.h - virtual PEs (stage S1a of the TARGET1 plan; ECALC_VPE=1, off by default).  FROZEN API: S1b..S1e are written against it.
 *
 * With ECALC_VPE=1 the process runs L (= ECALC_VPE_LANES, default 4) virtual PEs, one pthread each: vPE = lane d of node n is PE p = L n + d,
 * it owns device d, its own xshmem loopback PE (COMM_TRANSPORT=xshmem, XSHMEM_LOOPBACK_PES = L, make XSHMEM=ofi) and one flat communicator
 * over all PEs (rank = PE).  Switch off (unset or 0): vpe_on() = 0, every function below returns the node view (lanes 1, PE = node, idx 0),
 * vpe_run calls fn once on the caller; nothing else in the program changes.
 *
 * Changes against the S1 brief's API (additions only; nothing renamed or removed):
 *   + vpe_set_forced(mask)  unit tests: switches the vPE mode on without the environment and without the "stages not merged" abort
 *   + vpe_in_run()          1 inside a vpe_run thread
 *   + vpe_set_nodes(n, nn)  mn_init / tests tell vpe.c the node identity (COMM_RANK / COMM_SIZE otherwise)
 *   + vpe_set_thread_hook   the engine registers "select my loopback PE" (vpe.c does not link the xshmem engine)
 *   + vpe_stages_check()    the abort of ECALC_VPE=1 while a stage is missing; VPE_STAGES_MERGED, the stage bits
 *   + vpe_root(), vpe_node_leader(), ec_log_prefix()   log helpers (global lines: PE 0; node lines: lane 0)
 *   + vpe_sub_slot(g0, g, slot)   vpe_sub with an explicit id slot (two communicators over the same PE run)
 *   vpe_comm / vpe_sub / vpe_sub_slot are implemented in mn.c (it owns the transport meshes); the rest in vpe.c.
 *
 * Rules for the other stages: a thread that is not a vPE thread (main before vpe_run, helper threads) has vpe_idx() == 0; helper threads
 * spawned BY a vPE thread must be told their index (vpe_adopt) before they use VPE(...) state or a communicator. */
#ifndef EC_VPE_H
#define EC_VPE_H
#include "comm.h"
#ifdef __cplusplus
extern "C" {
#endif

#define VPE_MAX 8                                       /* the most vPEs of one process (xshmem MAXLPE) */
/* the stages of S1; a stage ORs its bit into VPE_STAGES_MERGED when it lands (one line each, merge order a b c d e) */
#define VPE_S_A 0x01                                    /* vpe, comm layer, NUMA, log prefix */
#define VPE_S_B 0x02                                    /* dbig single quarter, local product, PE-granular mdb / mn_core */
#define VPE_S_C 0x04                                    /* newton_db */
#define VPE_S_D 0x08                                    /* binary splitting, checkpoint, planner */
#define VPE_S_E 0x10                                    /* flow, writer threads, parts */
#define VPE_S_ALL 0x1f
#define VPE_STAGES_MERGED VPE_S_ALL
/* S2 (ECALC_PE_PER_GPU=1, one process per PE): its stages; the switch aborts with "stages ... not merged" until all are in (unit tests use vpe_set_forced) */
#define VPE_P_A 0x01                                    /* identity, launch, bootstrap (vpe.c, mn.c, comm_xshmem.c, mnrun.sh) */
#define VPE_P_B 0x02                                    /* node-level state in the flow (leader audit, bg threads, budget) */
#define VPE_P_C 0x04                                    /* planner, host budget, pools */
#define VPE_P_D 0x08                                    /* gate, measurements */
#define VPE_P_ALL 0x0f
#define VPE_PSTAGES_MERGED VPE_P_A

int  vpe_on(void);                                      /* ECALC_VPE=1 (or vpe_set_forced) */
int  vpe_lanes(void);                                   /* L: vPEs of this process (1 when off); ECALC_VPE_LANES, never a literal 4 */
extern __thread int vpe_t_idx;
extern int vpe_g_idx;                                   /* S2: the process's PE index on its node (>= 0 with ECALC_PE_PER_GPU: every thread of the process is that PE), else -1 */
static inline int vpe_idx(void) { return vpe_g_idx >= 0 ? vpe_g_idx : vpe_t_idx; }   /* 0..L-1: thread-local in S1 (0 outside vpe_run threads), process-global in S2 */
int  vpe_lane(void);                                    /* the lane of this vPE (= vpe_idx) */
int  vpe_device(void);                                  /* the HIP device ordinal of this vPE: S1 the lane; S2 pe_geo's device (the lane with --gpu-bind=none, 0 with one visible device) */
/* S2 (ECALC_PE_PER_GPU=1, off by default; implies the vPE view): ONE PROCESS PER PE, 4 per node (srun --ntasks-per-node = vpe_lanes(), block distribution, local id =
 * SLURM_LOCALID), xshmem K = 1.  COMM_RANK / COMM_SIZE stay the NODE index / count (mnrun.sh sets them as for PPN).  vpe_lanes() = PEs per node (ppn: SLURM_NTASKS_PER_NODE,
 * ECALC_VPE_LANES must agree if set); vpe_lpp() = lanes per process (S1: ppn, S2: 1).  vpe_run = vpe_adopt + fn on the caller, no threads. */
int  vpe_ppg(void);                                     /* ECALC_PE_PER_GPU=1 (or vpe_set_ppg) */
int  vpe_lpp(void);                                     /* lanes per process: 1 in S2, else vpe_lanes() */
int  vpe_proc_leader(void);                             /* the PE that acts for the PROCESS's shared resources: S1 lane 0, S2 every PE (its own process); off: 1 */
void vpe_ppg_init(void);                                /* re-reads ECALC_PE_PER_GPU / SLURM_LOCALID (done at process start; tests call it after setenv) */
void vpe_set_ppg(int local);                            /* tests: process mode with this local id (>= 0), or -1 = back to the environment */
void vpe_set_device(int dev);                           /* mn.c, after the engine start: pe_geo's device of this PE */
void vpe_pe_affinity(const char *cpulist);              /* S2: restrict this process to the cpus of its NUMA node (cpulist ∩ the launch mask; unchanged if empty; prints the mask once) */
#define VPE_NODE_SLOT 15                                /* the id slot of the node group (the PEs of one node: vpe_sub_slot(ppn node, ppn, VPE_NODE_SLOT)); non-power-of-two ppn: slot 3 */
int  vpe_pe(void);                                      /* L * node + idx (off: the node) */
int  vpe_npe(void);                                     /* L * nodes (off: the nodes) */
int  vpe_node(void);                                    /* this process's node rank; vpe_nnodes: the node count */
int  vpe_nnodes(void);
void vpe_set_nodes(int node, int nnodes);
void vpe_set_forced(int mask);                          /* tests: mask != 0 = vPE mode on (bits = stages the test exercises), 0 = back to the environment */
int  vpe_in_run(void);
void vpe_adopt(int idx);                                /* a helper thread of vPE idx: sets its vpe_idx, device and engine PE */
void vpe_set_thread_hook(void (*fn)(int idx));          /* fn(idx) runs in every vPE thread at start (and in vpe_adopt) */
void vpe_stages_check(void);                            /* fatal "ECALC_VPE: stages ... not merged" when on, not forced and a stage is missing (S2: "ECALC_PE_PER_GPU: stages ...") */

/* L pthreads, vPE i = thread i: hipSetDevice(i), thread hook (current PE), vpe_idx = i; joined before return.  Off: fn(arg) once on the caller.
 * fn may call collective operations; a fatal error in any thread ends the process (ec_fatal). */
void vpe_run(void (*fn)(void *), void *arg);
void vpe_node_barrier(void);                            /* barrier over the PEs of this node: S1 the vPE threads (inside vpe_run), S2 the node group over the NIC (collective over the node's 4 processes); off: nothing */

/* lane-indexed file-scope state: VPE_STATE(int, foo); ... VPE(foo) = 1;  In S2 (one lane per process) vpe_idx() is the constant 0. */
#define VPE_STATE(T, name) static T name##_v[VPE_MAX]
#define VPE(name) (name##_v[vpe_idx()])

/* the PE communicators (mn.c; collective over their members, creation is local: no message).  vpe_comm: all PEs, rank = PE, created by mn_init.
 * vpe_sub(g0, g): the PEs [g0, g0 + g) (PE units), NULL for a PE outside; cached per vPE; the whole run returns vpe_comm().  Aligned power-of-two runs
 * (g0 % g == 0) and any contiguous run (3, 6, 12 ...) work.  The id of a run follows from (g0, g): one id per size per PE, so two different runs of
 * the same size that share a PE need vpe_sub_slot (slot 1..15 = another id), or the creation is a fatal "id used twice".  Runs that are not an aligned
 * power of two support g < 64 and slot < 4 (fatal otherwise; ids are collision-free over the domain).  Destruction (mn_finalize) is collective and goes by
 * descending id.
 * CAVEAT for S1b..S1e: under ECALC_VPE, mn_comm(apu) IGNORES apu and returns the calling vPE's flat comm; OpenMP workers inside a vPE thread are NOT
 * adopted (vpe_idx() = 0 there).  Do not call mn_comm(d) from omp num_threads(NA) regions: take the communicator in the vPE thread and pass it in. */
comm *vpe_comm(void);
comm *vpe_sub(int g0, int g);
comm *vpe_sub_slot(int g0, int g, int slot);

/* logging: ec_log_prefix() = "" when off, else "pe N (node n lane d): " (a vPE thread) -- put it in front of every line a vPE prints.
 * Global lines come from vpe_root() (off: node 0, on: PE 0), node-level lines from vpe_node_leader() (off: always 1, on: lane 0). */
const char *ec_log_prefix(void);
int  vpe_root(void);
int  vpe_node_leader(void);

#ifdef __cplusplus
}
#endif
#endif
