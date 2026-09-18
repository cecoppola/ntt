/* mn.h - the multi-node layer (Phase 8, PLAN.md 17).  A process is one node driving its four APUs as the
 * single-node pipeline does; `size` node-processes are connected by an inter-node communicator, one mesh
 * per APU thread at global rank 4 node + d (the distributed tier's rank space is 4 size).  On aac6 the
 * meshes are TCP (correctness only; several node-processes may share one node); on the target, RDMA.
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
comm *mn_comm(int apu);           /* the APU thread's inter-node communicator (rank 4 node + apu of 4 size); 0 when size 1 */
int   mn_selftest(int logR, int logC, int verbose);   /* a distributed convolution over all 4 size ranks against the one-rank engine; 1 = ok */
void  mn_barrier(void);
void  mn_finalize(void);
#ifdef __cplusplus
}
#endif
#endif
