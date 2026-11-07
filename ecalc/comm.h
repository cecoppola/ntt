/* comm.h - the rank abstraction. A rank is one APU slot p (prime p) of one node; the communicator groups the ranks
 * holding the same prime across nodes, and every distributed operation acts within one prime's plane.
 * Implementations: comm_xshmem (multi-node transport), comm_layered (node-process layers over it), tests/t_dist.c
 * (synthetic ranks, test-only). Distributed numbers are limb arrays sharded by rank: rank r holds limbs [r n/size,
 * (r+1) n/size). The four-step transform uses the slab all-to-all: rank r sends slab s of its rows to rank s and
 * receives slab r from every rank. */
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
    /* all-to-all of equal slabs: sendbuf holds size slabs of `bytes` (slab s for rank s), recvbuf receives size
     * slabs; both device memory; may return before completion, comm_wait() completes it */
    void (*alltoall)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes, hipStream_t s);
    void (*wait)(comm *c);
    void (*barrier)(comm *c);
    /* reductions used by the tree partition and the verifier: sum of u64 mod q, max of size_t */
    uint64_t (*allreduce_modq)(comm *c, uint64_t v, uint64_t q, uint64_t weight_pow);
    size_t   (*allreduce_max)(comm *c, size_t v);
    void (*destroy)(comm *c);
    /* point-to-point of host buffers (gather of the node-processes' P_r, Q_r); 0 where not implemented */
    void (*send)(comm *c, int to, const void *buf, size_t bytes);
    void (*recv)(comm *c, int from, void *buf, size_t bytes);
    /* all-gather: every rank's block of `bytes` (device memory) into recvbuf[size][bytes] in rank order, complete on
     * return (the synthetic communicator: when its fourth rank has called); sendbuf may be recvbuf + rank * bytes.
     * 0 = not implemented: comm_allgather falls back to an all-to-all of `size` copies */
    void (*allgather)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* the same for host buffers (descriptors, carry flags, residues) */
    void (*allgather_host)(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);
    /* all-to-all of unequal slabs: rank r receives scnt[r] bytes from sendbuf + sdsp[r] of every rank, landing at
     * recvbuf + rdsp[src], with rcnt[src] = the sender's scnt[me] (both sides derive sizes from the same descriptors;
     * every transport aborts on a mismatch). Device memory; same completion rule as alltoall; a v-exchange is never
     * in flight with an equal-slab one. Counts/offsets are bytes (8-byte multiples are fastest). */
    void (*alltoallv)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp, hipStream_t s);
    /* the same for host buffers, complete on return */
    void (*alltoallv_host)(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp);
    /* symmetric memory for the callers' slabs: a transport that puts straight from/into its own pool hands out such
     * buffers (no staging); 0 means the caller allocates (hipMalloc) and the transport stages. The layered
     * communicator forwards to its inter transport. */
    void *(*sym_alloc)(comm *c, size_t bytes);
    void (*sym_free)(comm *c, void *p);
};
/* inflight: how many all-to-alls may be posted before a wait: 1 real transports, 2 layered (xGMI stage of the next
 * exchange overlaps the inter-node stage of the previous), 0 synthetic (every rank must post before any waits).
 * comm_wait completes the oldest pending exchange (a transport may complete more, never less). */
struct comm { const struct comm_ops *ops; void *priv; int rank, size; int inflight; };

static inline int  comm_rank(comm *c) { return c->rank; }
static inline int  comm_size(comm *c) { return c->size; }
static inline void comm_alltoall(comm *c, const void *sb, void *rb, size_t bytes, hipStream_t s) { c->ops->alltoall(c, sb, rb, bytes, s); }
/* MN_WAIT_STATS=1 (default off; comm_util.c): time each call blocks, per phase: barrier (peer skew) and comm_wait
 * (transfer plus skew). comm_wst_on: -1 unread, 0 off, 1 on. */
enum { WST_OTHER, WST_BS, WST_DM, WST_RECIP, WST_NP };
extern int comm_wst_on;
void comm_wst_wait(comm *c); void comm_wst_barrier(comm *c);
int comm_wst_set_phase(int p);                       /* returns the previous phase */
int comm_wst_enabled(void);
void comm_wst_totals(uint64_t ns[WST_NP][3], uint64_t n[WST_NP][3]);   /* [phase][0 barrier, 1 wait, 2 ready]: ns summed over the threads */
/* kind of the exchange a caller is about to post, thread-local, captured by the transport at post time; callers
 * restore the previous value after comm_alltoall[v]. Default XK_OTHER; layered inter stage is XK_LAYEQ / XK_LAYV. */
enum { XK_OTHER, XK_REDIST, XK_RESULT, XK_SPILL, XK_ADDSH, XK_LAYEQ, XK_LAYV, XK_NK };
extern __thread int comm_xtag;
static inline int comm_xtag_set(int k) { int o = comm_xtag; comm_xtag = k; return o; }
double comm_wst_t0(void); void comm_wst_ready_end(double t0);  /* timing of the peer-ready waits */
static inline void comm_wait(comm *c) { if (comm_wst_on != 0) comm_wst_wait(c); else c->ops->wait(c); }
static inline void comm_barrier(comm *c) { if (comm_wst_on != 0) comm_wst_barrier(c); else c->ops->barrier(c); }
static inline size_t comm_allreduce_max(comm *c, size_t v) { return c->ops->allreduce_max(c, v); }
static inline void comm_destroy(comm *c) { c->ops->destroy(c); }
void comm_allgather(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);   /* comm_util.c: the transport's op or the all-to-all fallback */
void comm_allgather_host(comm *c, const void *sendbuf, void *recvbuf, size_t bytes);   /* host buffers: the transport's op or the device op through temporaries */
/* the unequal all-to-all (comm_util.c: the transport's op; host variant through device temporaries). Typical use:
 * slabs packed back to back in rank order, comm_prefix() builds the offsets. */
void comm_alltoallv(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp, hipStream_t s);
void comm_alltoallv_host(comm *c, const void *sendbuf, const size_t *scnt, const size_t *sdsp, void *recvbuf, const size_t *rcnt, const size_t *rdsp);
static inline size_t comm_prefix(const size_t *cnt, size_t *dsp, int n) { size_t o = 0; for (int r = 0; r < n; r++) { dsp[r] = o; o += cnt[r]; } return o; }   /* offsets of back-to-back slabs; returns the total */
static inline void comm_send(comm *c, int to, const void *b, size_t n) { c->ops->send(c, to, b, n); }
static inline void comm_recv(comm *c, int from, void *b, size_t n) { c->ops->recv(c, from, b, n); }
static inline void *comm_sym_alloc(comm *c, size_t bytes) { return c->ops->sym_alloc ? c->ops->sym_alloc(c, bytes) : 0; }  /* device-accessible buffer of the transport's symmetric pool, or 0 */
static inline void comm_sym_free(comm *c, void *p) { if (p && c->ops->sym_free) c->ops->sym_free(c, p); }

hipError_t comm_copy_async(void *d, const void *s, size_t n, hipStream_t st);  /* copy kernel (comm_util.c) for copies that touch host-registered pools */
comm *comm_xgmi_create(int rank);  /* four real ranks = four APUs of one node, driven by four host threads; safe to create concurrently */
/* layered communicator of a node group: intra (4 APUs, xGMI) x inter (mesh over g nodes, rank = node); APU thread d
 * is global rank g d + node, size 4 g */
comm *comm_layered_create(comm *intra, comm *inter, int d);
#ifdef XSHMEM_ENGINE
/* xshmem transport (COMM_TRANSPORT=xshmem): one PE per APU (loopback PEs in a node-process). The PE set of create_at
 * is NODE indices. comm_xshmem_counters: all-to-all totals so far (post-to-completion s summed over APU threads,
 * count, bytes received). */
int   comm_xshmem_available(void);
int   comm_xshmem_init(void);                  /* returns the node-process count */
int   comm_xshmem_rank(void);
comm *comm_xshmem_create_at(int pe_start, int pe_stride, int n, int id);
comm *comm_xshmem_create_flat(int pe_start, int n, int id);  /* contiguous PE run [pe_start, pe_start + n) in PE units (PE = K node + loopback PE); local, no message */
void  comm_xshmem_select(int lpe);  /* this thread's current loopback PE (vpe.h thread hook) */
int   comm_xshmem_loopback_pes(void);          /* K: loopback PEs per process */
int   comm_xshmem_geo(int *v, char *cpus, int ncpus);  /* v = { pe, npes, local, pes per node, gpu, numa, nics, nic[0] }; cpus = the NUMA node cpu list; 0 = ok */
void  comm_xshmem_finalize(void);
void  comm_xshmem_counters(double *t, long *n, double *bytes);
#endif
void  comm_layered_scratch(comm *c, void *p, size_t bytes);
void  comm_layered_release_vslots(comm *c);  /* free the pool v-slots after a product's v-exchanges */
size_t comm_layered_vbytes(int dev);  /* v-exchange scratch hipMalloc'd now on device dev (all layered communicators) */

#ifdef __cplusplus
}
#endif
#endif
