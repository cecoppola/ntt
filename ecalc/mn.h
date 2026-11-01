/* mn.h - the multi-node layer .  A process is one node driving its four APUs as the
 * single-node pipeline does; `size` node-processes are connected by four inter-node meshes, one per APU
 * thread d (its members: thread d of every node; rank = node).  A transform over the whole machine is the
 * node's four-APU exchange over xGMI composed with the mesh exchange.  The meshes are xshmem PE sets
 * (COMM_TRANSPORT=xshmem; several node-processes may share one node).
 *   COMM_RANK   this node-process (0 .. size-1)        COMM_SIZE   node-processes (unset or 1: single node) */
#ifndef EC_MN_H
#define EC_MN_H
#include "comm.h"
#include "mdb.h"
#ifdef __cplusplus
extern "C" {
#endif
int   mn_init(void);              /* reads the environment, opens the meshes; returns size (1 = not multi-node) */
int   mn_rank(void);
void  mn_wait_stats_print(void);   /* D3: MN_WAIT_STATS=1 -- per node, the barrier and comm_wait time per phase, and the max / min over the nodes (collective) */
void  mn_comm_mark(const char *what);   /* MN_COMM_MARK=1 -- node 0 prints the all-to-all traffic since the previous mark (default off) */
int   mn_size(void);
comm *mn_comm(int apu);           /* mesh apu: this node among the nodes (rank = node); 0 when size 1 */
int   mn_transport_shmem(void);   /* 1 when the meshes are xshmem PE sets (COMM_TRANSPORT=xshmem) */
int   mn_selftest(int logR, int logC, int verbose);   /* a distributed convolution over each mesh against the one-rank engine; 1 = ok */
void  mn_barrier(void);
/* M3 (archive:PLAN §17): node groups per tree level, the layered communicator's self-test, the distributed top levels */
mn_group *mn_group_at(int level);                            /* this node's group of 2^level nodes (meshes created collectively on first use) */
mn_group *mn_group_span(int l, int g0, int g);               /* the group [g0, g0+g) of schedule level l (MN_GROUPS) -- mn_group_at's when it is a binary group, its own meshes otherwise */
void  mn_allgather(comm *c, const uint64_t *v, int k, uint64_t *out);   /* k u64 per rank -> out[rank k + i] */
int   mn_selftest_layered(int logR, int logC, int verbose);  /* the layered comm over 4 x (largest power of two <= size) ranks; 1 = ok */
void  mn_tree(mdb *P, mdb *Q, struct dbig_s *Pleaf, struct dbig_s *Qleaf);   /* the leaves (taken over) -> shares of P, Q over all nodes */
/* C3 step 3 (ECALC_DR >= 2, dr.h): the leaf check -- poll(arg, &p, &q) = 1 with this node's recurrence residues (T1's, modulo t1_q[0]) once finished, 0 while running, -1 if it never started;
 * mn_dr_division_check (newton_db.c mn_divmod_dkm, with the residues of P, Q it computes): the leaf check if still open, then the tree's tracked values against them */
void  mn_dr_leaf_ref(int (*poll)(void *arg, uint64_t *p, uint64_t *q), void *arg);
void  mn_dr_leaf_final(void);   /* ecalc.c out_stage, after the recurrence thread is joined: the leaf check if the division's start found the recurrence still running (counted as deferred), or logs/counts it skipped if the recurrence never started */
void  mn_dr_division_check(const uint64_t *qs, const uint64_t *pres, const uint64_t *qres);
void  mn_finalize(void);
/* the top tree set in the background (ECALC_CKPT_TOP) -- mode 0 = none, 1 = background and complete,
 * 2 = background and budgeted (dropped when the measured disk rate would stall the critical path more than mn_ckpt_slack s);
 * mn_ckpt_top_finish (every node, after the output stage, before the device memory goes): Q released, the writer joined */
extern int mn_ckpt_bg_mode; extern double mn_ckpt_slack;
void  mn_ckpt_top_finish(void);
#ifdef __cplusplus
}
#endif
#endif
