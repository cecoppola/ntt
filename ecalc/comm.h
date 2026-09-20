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
    /* point-to-point of host buffers (M2: the gather of the node-processes' P_r, Q_r); 0 where not implemented */
    void (*send)(comm *c, int to, const void *buf, size_t bytes);
    void (*recv)(comm *c, int from, void *buf, size_t bytes);
    /* all-gather: every rank's block of `bytes` (device memory) into recvbuf[size][bytes] in rank order, complete on
     * return (the synthetic communicator: when its fourth rank has called); sendbuf may be recvbuf + rank * bytes.
     * 0 = not implemented: comm_allgather falls back to an all-to-all of `size` copies (Phase 9 day 0) */
    void (*allgather)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* the same for host buffers (M7: the descriptors, carry flags and residues the tree gathers -- k u64 per node) */
    void (*allgather_host)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* B7 (Phase 10): all-to-all of unequal slabs.  Rank r receives scnt[r] bytes from sendbuf + sdsp[r] of every
     * rank; they land at recvbuf + rdsp[src] of the receiver, which supplies rcnt[src] (= the sender's scnt[me]:
     * both sides compute the sizes from the same descriptors; every transport aborts on a mismatch).  Device
     * memory; the same completion rule as alltoall (may return before completion, comm_wait completes it; the
     * pipelining depth is the transport's inflight, and a v-exchange is never in flight with an equal-slab one).
     * Counts and offsets are bytes, any values (a zero count is allowed; 8-byte multiples are fastest). */
    void (*alltoallv)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp, hipStream_t s);
    /* the same for host buffers, complete on return */
    void (*alltoallv_host)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp);
};
/* inflight: how many all-to-alls may be posted before a wait (M7's slab pipelining, ntt_dist.c): 1 for the real
 * transports, 2 for the layered one (its xGMI stage of the next exchange runs under the inter-node stage of the
 * previous), 0 for the synthetic one (every rank must post before any waits -- no pipelining).  comm_wait completes
 * the oldest pending exchange; a transport may complete more than that (never less). */
struct comm { const struct comm_ops *ops; void *priv; int rank, size; int inflight; };

static inline int  comm_rank(comm *c) { return c->rank; }
static inline int  comm_size(comm *c) { return c->size; }
static inline void comm_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s) { c->ops->alltoall(c, sb, rb, bytes, s); }
static inline void comm_wait(comm *c) { c->ops->wait(c); }
static inline void comm_barrier(comm *c) { c->ops->barrier(c); }
static inline size_t comm_allreduce_max(comm *c, size_t v) { return c->ops->allreduce_max(c, v); }
static inline void comm_destroy(comm *c) { c->ops->destroy(c); }
void comm_allgather(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);   /* comm_util.c: the transport's op or the all-to-all fallback */
void comm_allgather_host(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);   /* host buffers: the transport's op or the device op through temporaries */
/* B7: the unequal all-to-all (comm_util.c: the transport's op; the host variant through device temporaries where a
 * transport lacks it).  The typical use: slabs packed back to back in rank order -- comm_prefix() builds the offsets. */
void comm_alltoallv(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp, hipStream_t s);
void comm_alltoallv_host(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp);
static inline size_t comm_prefix(const size_t *cnt, size_t *dsp, int n) { size_t o = 0; for (int r = 0; r < n; r++) { dsp[r] = o; o += cnt[r]; } return o; }   /* offsets of back-to-back slabs; returns the total */
static inline void comm_send(comm *c, int to, const void *b, size_t n) { c->ops->send(c, to, b, n); }
static inline void comm_recv(comm *c, int from, void *b, size_t n) { c->ops->recv(c, from, b, n); }

comm *comm_local_create(void);                 /* size 1 */
comm *comm_sim4_create(int rank_of_this_apu);  /* WP5: four synthetic ranks sharing one APU (created four times, one per simulated rank) */
comm *comm_xgmi_create(int rank);              /* WP5: four real ranks = four APUs of one node, driven by four host threads; safe to create concurrently */
comm *comm_tcp_create(void);                   /* WP6: TCP sockets, one process per rank; COMM_RANK/SIZE/HOSTS/PORT */
comm *comm_tcp_create_at(int me, int n, const char *hosts_csv, int port_base);   /* the same for an explicit rank/size/hosts/port (M1) */
/* M3: the layered communicator of a node group: intra (the four APUs, xGMI, size 4) x inter (mesh d over the
 * g nodes, rank = node); this APU thread d is global rank g d + node, size 4 g (comm_layered.c) */
comm *comm_layered_create(comm *intra, comm *inter, int d);
/* Phase 11 S (PLAN.md 25-26): the SHMEM transport (comm_shmem.c; COMM_TRANSPORT=shmem, launched by oshrun / srun).  One PE
 * per node-process; a communicator is a strided PE set {pe_start + pe_stride r : r < n} -- the shim for OpenSHMEM 1.5
 * teams -- created by all its members with a run-unique id (mn.c: per level slot and APU thread).  comm_shmem_init
 * (shmem_init_thread, the symmetric pool) is called once per process, by mn_init or the first create. */
int   comm_shmem_available(void);              /* 1 when built with SHMEM (make SHMEM=1) */
int   comm_shmem_init(void);                   /* returns the PE count */
int   comm_shmem_rank(void);
int   comm_shmem_size(void);
comm *comm_shmem_create_at(int pe_start, int pe_stride, int n, int id);
void  comm_shmem_finalize(void);
void  comm_layered_scratch(comm *c, void *p, size_t bytes);   /* a device scratch of one slab buffer for the block transposes (else hipMalloc'd) */

/* the shard of an n-limb number held by rank r of size ranks: [lo, hi) */
static inline void comm_shard(size_t n, int r, int size, size_t *lo, size_t *hi) { *lo = n * (size_t)r / size; *hi = n * (size_t)(r + 1) / size; }

#ifdef __cplusplus
}
#endif
#endif
