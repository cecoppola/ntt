/* comm.h - the rank abstraction of design A (PLAN.md 15, WP5).
 *
 * A "rank" is one APU slot p (prime p) of one node.  The communicator groups
 * the ranks that hold the same prime across nodes: every distributed
 * operation below acts within one prime's plane.  Implementations:
 *   comm_local   one rank (size 1): every call is a no-op or a memcpy — the
 *                single-node pipeline runs unchanged through it
 *   comm_sim4    a synthetic four-rank layout inside one APU's HBM, for
 *                testing the distributed transform without a network (WP5)
 *   comm_tcp     sockets between nodes, correctness only (WP6, aac6)
 *   comm_fabric  the target system's transport (GPU-direct RDMA), later
 *
 * Distributed numbers are limb arrays sharded by rank: rank r holds limbs
 * [r n/size, (r+1) n/size).  A distributed transform (four-step) uses the
 * slab all-to-all: rank r sends slab s of its rows to rank s and receives
 * slab r from every rank.
 */
#ifndef EC_COMM_H
#define EC_COMM_H
#include <stddef.h>
#include <stdint.h>
#ifdef COMM_HOST_ONLY
typedef void *hipStream_t;              /* host-only build of the TCP communicator (no GPU) */
#else
#include <hip/hip_runtime.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct comm comm;
struct comm_ops {
    int  (*rank)(comm *c);
    int  (*size)(comm *c);
    /* all-to-all of equal slabs: sendbuf holds size slabs of `bytes` each (slab s for rank s),
     * recvbuf receives size slabs (slab r from rank r); both device memory of this rank's APU;
     * may return before completion — comm_wait() completes it */
    void (*alltoall)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes, hipStream_t s);
    void (*wait)(comm *c);
    void (*barrier)(comm *c);
    /* reductions used by the tree partition and the verifier: sum of u64 mod q, max of size_t */
    uint64_t (*allreduce_modq)(comm *c, uint64_t v, uint64_t q, uint64_t weight_pow);   /* sum_r v_r * weight^r... per-rank weights supplied by caller */
    size_t   (*allreduce_max)(comm *c, size_t v);
    void (*destroy)(comm *c);
};
struct comm { const struct comm_ops *ops; void *priv; int rank, size; };

static inline int  comm_rank(comm *c) { return c->rank; }
static inline int  comm_size(comm *c) { return c->size; }
static inline void comm_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s) { c->ops->alltoall(c, sb, rb, bytes, s); }
static inline void comm_wait(comm *c) { c->ops->wait(c); }
static inline void comm_barrier(comm *c) { c->ops->barrier(c); }
static inline size_t comm_allreduce_max(comm *c, size_t v) { return c->ops->allreduce_max(c, v); }
static inline void comm_destroy(comm *c) { c->ops->destroy(c); }

comm *comm_local_create(void);                 /* size 1 */
comm *comm_sim4_create(int rank_of_this_apu);  /* WP5: four synthetic ranks sharing one APU (created four times, one per simulated rank) */
comm *comm_tcp_create(void);                   /* WP6: TCP sockets, one process per rank; COMM_RANK/SIZE/HOSTS/PORT */

/* the shard of an n-limb number held by rank r of size ranks: [lo, hi) */
static inline void comm_shard(size_t n, int r, int size, size_t *lo, size_t *hi) { *lo = n * (size_t)r / size; *hi = n * (size_t)(r + 1) / size; }

#ifdef __cplusplus
}
#endif
#endif
