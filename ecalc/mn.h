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
#ifdef __cplusplus
extern "C" {
#endif
int   mn_init(void);              /* reads the environment, opens the meshes; returns size (1 = not multi-node) */
int   mn_rank(void);
int   mn_size(void);
comm *mn_comm(int apu);           /* mesh apu: this node among the nodes (rank = node); 0 when size 1 */
int   mn_selftest(int logR, int logC, int verbose);   /* a distributed convolution over each mesh against the one-rank engine; 1 = ok */
void  mn_barrier(void);
void  mn_finalize(void);
#ifdef __cplusplus
}
#endif
#endif
