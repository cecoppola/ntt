/* mn.h - the multi-node layer (Phase 8, PLAN.md 17).  A process is one node driving its four APUs as the
 * single-node pipeline does; `size` node-processes are connected by four inter-node meshes, one per APU
 * thread d (its members: thread d of every node; rank = node).  A transform over the whole machine is the
 * node's four-APU exchange over xGMI composed with the mesh exchange (M3/M7).  On aac6 the meshes are TCP
 * (correctness only; several node-processes may share one node); on the target, RDMA.
 *   COMM_RANK   this node-process (0 .. size-1)        COMM_SIZE   node-processes (unset or 1: single node)
 *   COMM_HOSTS  comma list of the node of each process COMM_PORT   port base (27000); mesh d uses base + 64 d */
#ifndef EC_MN_H
#define EC_MN_H
#include "comm.h"
#include "mdb.h"
#ifdef __cplusplus
extern "C" {
#endif
int   mn_init(void);              /* reads the environment, opens the meshes; returns size (1 = not multi-node) */
int   mn_rank(void);
int   mn_size(void);
comm *mn_comm(int apu);           /* mesh apu: this node among the nodes (rank = node); 0 when size 1 */
int   mn_transport_shmem(void);   /* Phase 11 S: 1 when the meshes are SHMEM PE sets (COMM_TRANSPORT=shmem) */
int   mn_selftest(int logR, int logC, int verbose);   /* a distributed convolution over each mesh against the one-rank engine; 1 = ok */
void  mn_barrier(void);
/* M3 (PLAN.md 17): node groups per tree level, the layered communicator's self-test, the distributed top levels */
mn_group *mn_group_at(int level);                            /* this node's group of 2^level nodes (meshes created collectively on first use) */
mn_group *mn_group_span(int l, int g0, int g);               /* Phase 12 G: the group [g0, g0+g) of schedule level l (MN_GROUPS) -- mn_group_at's when it is a binary group, its own meshes otherwise */
void  mn_allgather(comm *c, const uint64_t *v, int k, uint64_t *out);   /* k u64 per rank -> out[rank k + i] */
int   mn_selftest_layered(int logR, int logC, int verbose);  /* the layered comm over 4 x (largest power of two <= size) ranks; 1 = ok */
void  mn_tree(mdb *P, mdb *Q, struct dbig_s *Pleaf, struct dbig_s *Qleaf);   /* the leaves (taken over) -> shares of P, Q over all nodes */
int   mn_ckpt_tree_level(unsigned long N);                    /* M6: the tree level every node can restart from (0: none; N = 0 after the first call) */
void  mn_gather_host(bigint *out, const mdb *X);             /* node 0 assembles the number on the host; the others send their share */
void  mn_finalize(void);
/* Phase 13 N (TASKS 1.4, 4.1): the top tree set in the background -- mode 0 = synchronous (as before), 1 = background and complete,
 * 2 = background and budgeted (dropped when the measured disk rate would stall the critical path more than mn_ckpt_slack s);
 * mn_ckpt_top_finish (every node, after the output stage, before the device memory goes): Q released, the writer joined */
extern int mn_ckpt_bg_mode; extern double mn_ckpt_slack;
void  mn_ckpt_top_finish(void);
#ifdef __cplusplus
}
#endif
#endif
