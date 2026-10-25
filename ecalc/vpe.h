/* vpe.h - PE identity and the PE communicators for the one-process-per-GPU layout (ECALC_PE_PER_GPU=1: one process per PE, 4 per node).
 * The thread mode (ECALC_VPE: one process driving the PEs as threads) was removed 2026-10-23 (Q86); its code is on branch vpe-int.
 * Off (ECALC_PE_PER_GPU unset or 0): vpe_on() = 0, every function below returns the node view (lanes 1, PE = node, idx 0), vpe_run calls fn once on the caller; nothing else changes.
 * vpe_comm / vpe_sub / vpe_sub_slot are implemented in mn.c (it owns the transport meshes); the rest in vpe.c.
 * A helper thread of the PE's process is told the PE's index (vpe_adopt) before it uses a communicator. */
#ifndef EC_VPE_H
#define EC_VPE_H
#include "comm.h"
#ifdef __cplusplus
extern "C" {
#endif

#define VPE_MAX 8                                       /* the most vPEs of one process (xshmem MAXLPE) */

int  vpe_on(void);                                      /* process mode (ECALC_PE_PER_GPU=1) or vpe_set_forced */
int  vpe_lanes(void);                                   /* PEs per node (1 when off): SLURM tasks per node; ECALC_VPE_LANES only where there is no launch (planners, forced test views; default 4) */
extern int vpe_g_idx;                                   /* the process's PE index on its node (>= 0 with ECALC_PE_PER_GPU: every thread of the process is that PE), else -1 */
static inline int vpe_idx(void) { return vpe_g_idx >= 0 ? vpe_g_idx : 0; }   /* 0..L-1: the process's PE index on its node (process-global) */
int  vpe_lane(void);                                    /* the lane of this vPE (= vpe_idx) */
int  vpe_device(void);                                  /* the HIP device ordinal of this vPE: pe_geo's device (the lane with --gpu-bind=none, 0 with one visible device) */
/* ECALC_PE_PER_GPU=1 (off by default): ONE PROCESS PER PE, 4 per node (srun --ntasks-per-node = vpe_lanes(), block distribution, local id =
 * SLURM_LOCALID), xshmem K = 1.  COMM_RANK / COMM_SIZE stay the NODE index / count (mnrun.sh sets them as for PPN).  vpe_lanes() = PEs per node (ppn: SLURM_NTASKS_PER_NODE,
 * ECALC_VPE_LANES must agree if set); vpe_run = vpe_adopt + fn on the caller, no threads. */
int  vpe_ppg(void);                                     /* ECALC_PE_PER_GPU=1 (or vpe_set_ppg) */
int  vpe_proc_leader(void);                             /* the PE that acts for the PROCESS's shared resources: every PE (its own process); off: 1 */
void vpe_ppg_init(void);                                /* re-reads ECALC_PE_PER_GPU / SLURM_LOCALID (done at process start; tests call it after setenv) */
void vpe_set_ppg(int local);                            /* tests: process mode with this local id (>= 0), or -1 = back to the environment */
int  vpe_early_device(void);                            /* the device made current at process start (-1 if none); mn_init_ppg checks it against pe_geo's */
void vpe_set_device(int dev);                           /* mn.c, after the engine start: pe_geo's device of this PE */
void vpe_pe_affinity(const char *cpulist);              /* restrict this process to the cpus of its NUMA node (cpulist AND the launch mask; unchanged if empty; prints the mask once) */
#define VPE_NODE_SLOT 15                                /* the id slot of the node group (the PEs of one node: vpe_sub_slot(ppn node, ppn, VPE_NODE_SLOT)); non-power-of-two ppn: slot 3 */
int  vpe_pe(void);                                      /* L * node + idx (off: the node) */
int  vpe_npe(void);                                     /* L * nodes (off: the nodes) */
int  vpe_node(void);                                    /* this process's node rank; vpe_nnodes: the node count */
int  vpe_nnodes(void);
void vpe_set_nodes(int node, int nnodes);
void vpe_set_forced(int mask);                          /* tests: mask != 0 = the PE view on without a launch, 0 = back to the environment */
int  vpe_in_run(void);
void vpe_adopt(int idx);                                /* a helper thread of this process's PE: sets its vpe_idx, device and engine PE (process mode only; off: nothing) */
void vpe_set_thread_hook(void (*fn)(int idx));          /* fn(idx) runs in vpe_adopt (the engine registers "select my PE") */

/* Process mode: fn(arg) on the caller with vpe_in_run() = 1 (this process is the one PE: no threads).  Off: fn(arg) once on the caller. */
void vpe_run(void (*fn)(void *), void *arg);
void vpe_node_barrier(void);                            /* barrier over the PEs of this node: the node group over the NIC (collective over the node's 4 processes); off: nothing */

/* the PE communicators (mn.c; collective over their members, creation is local: no message).  vpe_comm: all PEs, rank = PE, created by mn_init.
 * vpe_sub(g0, g): the PEs [g0, g0 + g) (PE units), NULL for a PE outside; cached per process; the whole run returns vpe_comm().  Aligned power-of-two runs
 * (g0 % g == 0) and any contiguous run (3, 6, 12 ...) work.  The id of a run follows from (g0, g): one id per size per PE, so two different runs of
 * the same size that share a PE need vpe_sub_slot (slot 1..15 = another id), or the creation is a fatal "id used twice".  Runs that are not an aligned
 * power of two support g < 64 and slot < 4 (fatal otherwise; ids are collision-free over the domain).  Destruction (mn_finalize) is collective and goes by
 * descending id.
 */
comm *vpe_comm(void);
comm *vpe_sub(int g0, int g);
comm *vpe_sub_slot(int g0, int g, int slot);

/* logging: ec_log_prefix() = "" when off, else "pe N (node n lane d): " -- put it in front of every line a PE prints.
 * Global lines come from vpe_root() (off: node 0, on: PE 0), node-level lines from vpe_node_leader() (off: always 1, on: lane 0). */
const char *ec_log_prefix(void);
int  vpe_root(void);
int  vpe_node_leader(void);

#ifdef __cplusplus
}
#endif
#endif
