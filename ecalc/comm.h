/* comm.h - the rank abstraction of design A (archive:PLAN §15, WP5).
 *
 * A "rank" is one APU slot p (prime p) of one node.  The communicator groups
 * the ranks that hold the same prime across nodes: every distributed
 * operation below acts within one prime's plane.  Implementations:
 *   comm_xshmem  the multi-node transport (comm_xshmem.c, on xshmem.h)
 *   comm_layered the node-process layers over it (comm_layered.c)
 *   tests/comm_local.c, tests/comm_sim4.c: one rank / four synthetic ranks in one APU, test-only (t_dist)
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
#include <hip/hip_runtime.h>
#define HIP_SYNC(s) HIP_CHECK(hipStreamSynchronize(s))   /* where HIP_CHECK exists */

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
     * 0 = not implemented: comm_allgather falls back to an all-to-all of `size` copies */
    void (*allgather)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* the same for host buffers (M7: the descriptors, carry flags and residues the tree gathers -- k u64 per node) */
    void (*allgather_host)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* B7: all-to-all of unequal slabs.  Rank r receives scnt[r] bytes from sendbuf + sdsp[r] of every
     * rank; they land at recvbuf + rdsp[src] of the receiver, which supplies rcnt[src] (= the sender's scnt[me]:
     * both sides compute the sizes from the same descriptors; every transport aborts on a mismatch).  Device
     * memory; the same completion rule as alltoall (may return before completion, comm_wait completes it; the
     * pipelining depth is the transport's inflight, and a v-exchange is never in flight with an equal-slab one).
     * Counts and offsets are bytes, any values (a zero count is allowed; 8-byte multiples are fastest). */
    void (*alltoallv)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp, hipStream_t s);
    /* the same for host buffers, complete on return */
    void (*alltoallv_host)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp);
    /* symmetric memory for the callers' slabs.  A transport that puts straight from and into buffers of its own
     * pool (xshmem: the DATA heap, device-accessible) hands out such buffers here; an exchange whose send / receive
     * buffer lies in the pool is not staged.  0 (the field or the result): the caller allocates the buffer itself
     * (hipMalloc) and the transport stages it.  The layered communicator forwards to its inter transport. */
    void *(*sym_alloc)(comm *c, size_t bytes);
    void (*sym_free)(comm *c, void *p);
};
/* inflight: how many all-to-alls may be posted before a wait (M7's slab pipelining, ntt_dist.c): 1 for the real
 * transports, 2 for the layered one (its xGMI stage of the next exchange runs under the inter-node stage of the
 * previous), 0 for the synthetic one (every rank must post before any waits -- no pipelining).  comm_wait completes
 * the oldest pending exchange; a transport may complete more than that (never less). */
struct comm { const struct comm_ops *ops; void *priv; int rank, size; int inflight; };

static inline int  comm_rank(comm *c) { return c->rank; }
static inline int  comm_size(comm *c) { return c->size; }
static inline void comm_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s) { c->ops->alltoall(c, sb, rb, bytes, s); }
/* D3 (MN_WAIT_STATS=1, default off; comm_util.c): the time each call blocks -- the barrier (the peers' skew) and comm_wait
 * (the exchanges' completion, transfer plus skew), per phase.  comm_wst_on: -1 unread, 0 off, 1 on: off costs one flag test. */
enum { WST_OTHER, WST_BS, WST_DM, WST_RECIP, WST_NP };
extern int comm_wst_on;
void comm_wst_wait(comm *c); void comm_wst_barrier(comm *c);
int comm_wst_set_phase(int p);                       /* returns the previous phase */
int comm_wst_enabled(void);
void comm_wst_totals(uint64_t ns[WST_NP][3], uint64_t n[WST_NP][3]);   /* [phase][0 barrier, 1 wait, 2 ready]: ns summed over the threads */
/* X1: the kind of the exchange a caller is about to post, thread-local, captured by the transport at post time (comm_xshmem.c: the lane trace).  Callers set it before comm_alltoall[v] and restore the previous value after;
 * the default is XK_OTHER.  Layered exchanges (comm_layered.c's inter stage) are XK_LAYEQ / XK_LAYV; everything else is direct. */
enum { XK_OTHER, XK_REDIST, XK_RESULT, XK_SPILL, XK_ADDSH, XK_LAYEQ, XK_LAYV, XK_NK };
extern __thread int comm_xtag;
static inline int comm_xtag_set(int k) { int o = comm_xtag; comm_xtag = k; return o; }
double comm_wst_t0(void); void comm_wst_ready_end(double t0);   /* S22: timing of the peer-ready waits */
static inline void comm_wait(comm *c) { if (comm_wst_on != 0) comm_wst_wait(c); else c->ops->wait(c); }
static inline void comm_barrier(comm *c) { if (comm_wst_on != 0) comm_wst_barrier(c); else c->ops->barrier(c); }
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
static inline void *comm_sym_alloc(comm *c, size_t bytes) { return c->ops->sym_alloc ? c->ops->sym_alloc(c, bytes) : 0; }   /* S12: a device-accessible buffer of the transport's symmetric pool, or 0 */
static inline void comm_sym_free(comm *c, void *p) { if (p && c->ops->sym_free) c->ops->sym_free(c, p); }

hipError_t comm_copy_async(void *d, const void *s, size_t n, hipStream_t st);   /* N7D: copy kernel (comm_copy.c) for copies that touch host-registered pools */
comm *comm_xgmi_create(int rank);              /* WP5: four real ranks = four APUs of one node, driven by four host threads; safe to create concurrently */
/* M3: the layered communicator of a node group: intra (the four APUs, xGMI, size 4) x inter (mesh d over the
 * g nodes, rank = node); this APU thread d is global rank g d + node, size 4 g (comm_layered.c) */
comm *comm_layered_create(comm *intra, comm *inter, int d);
#ifdef XSHMEM_ENGINE
/* the xshmem transport (comm_xshmem.c; COMM_TRANSPORT=xshmem, make XSHMEM=ofi): the target-shaped engine over xshmem.h, one PE per APU (loopback PEs in a node-process).
 * The PE set of create_at is NODE indices (the process rank).  comm_xshmem_counters: the all-to-all totals so far (post-to-completion s summed over the APU threads, count, bytes received). */
int   comm_xshmem_available(void);
int   comm_xshmem_init(void);                  /* returns the node-process count */
int   comm_xshmem_rank(void);
comm *comm_xshmem_create_at(int pe_start, int pe_stride, int n, int id);
comm *comm_xshmem_create_flat(int pe_start, int n, int id);   /* S1a (PE layout): the contiguous PE run [pe_start, pe_start + n) in PE units (PE = K node + loopback PE); local, no message */
void  comm_xshmem_select(int lpe);             /* S1a: this thread's current loopback PE (the vpe.h thread hook) */
int   comm_xshmem_loopback_pes(void);          /* K: loopback PEs per process */
int   comm_xshmem_geo(int *v, char *cpus, int ncpus);   /* S2a: v = { pe, npes, local, pes per node, gpu, numa, nics, nic[0] }; cpus = the NUMA node cpu list; 0 = ok */
void  comm_xshmem_finalize(void);
void  comm_xshmem_counters(double *t, long *n, double *bytes);
#endif
void  comm_layered_scratch(comm *c, void *p, size_t bytes);
void  comm_layered_release_vslots(comm *c);   /* M3: free the pool v-slots after a product's v-exchanges  */
size_t comm_layered_vbytes(int dev);   /* B7ACCT: the v-exchange scratch hipMalloc'd now on device dev (all layered communicators) */

#ifdef __cplusplus
}
#endif
#endif
