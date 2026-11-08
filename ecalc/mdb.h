/* mdb.h - a big number sharded over a node group.
 *
 * n limbs (normalized: limb n-1 != 0 or n == 0) held by the nodes [g0, g0+g): node g0 + k holds the contiguous share
 * [N k/g, N (k+1)/g) in its dbig `sh` (sh.n = the share's length, never normalized), N >= n the sharding basis (the allocated
 * length of the product that made it; it does not move when the number is normalized).  Nodes outside the group hold nothing.
 * Every node of a larger group can describe the number from (n, N, g0, g) alone, so redistribution needs no boundary exchange.
 *
 * C = A B (+ X) over a group G: the four-step transform over nr = 4 g ranks, rank rho = g d + r holding the block-cyclic rows
 * [R rho / nr, R (rho+1) / nr) -- ntt_dist's equal map when g is a power of two, else the general map (rns_dist.c gen_fwd /
 * gen_inv_pw, alltoallv of per-pair slabs).  Each operand is redistributed once: node k packs, per rank (r, d), the limbs of
 * its share on that rank's rows (one contiguous segment of the rank's row sequence), one alltoallv over mesh d delivers them in
 * node order = sequence order.  The result's rows go back the same way into the nodes' contiguous shares of C; the CRT run
 * spills are all-gathered (4 C limbs per rank) and added by a fixed-length chunked-carry add, carries across node boundaries
 * resolved by a scan over the nodes' (carry, propagate) flags plus a second pass.  X (the tree add P = P_A Q_B + P_B) rides as
 * the CRT's added operand, redistributed like A and B. */
#ifndef EC_MDB_H
#define EC_MDB_H
#include "dbig.h"
#include "comm.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct mdb_s { dbig sh; size_t n, N; int g0, g; } mdb;   /* (the tag: newton.h's forward declaration, A-div) */
/* the share of global node r: [lo, hi) (empty outside the group) */
static inline void mdb_share(const mdb *x, int r, size_t *lo, size_t *hi)
{
    if (r < x->g0) { *lo = *hi = 0; return; }
    if (r >= x->g0 + x->g) { *lo = *hi = x->N; return; }
    *lo = x->N * (size_t)(r - x->g0) / x->g; *hi = x->N * (size_t)(r - x->g0 + 1) / x->g;
}
/* a node group for one level of the tree: nodes [g0, g0+g), this node me = rank - g0; all[d] the mesh over the
 * group (rank = node - g0) for APU thread d, 0 when the group is one node; lay[d] the layered communicator over
 * all[d] (built by the product, cached); tr[d] the mesh over the first 2^k <= g nodes (the layered self-test only) */
typedef struct mn_group { int g0, g, me; comm *all[4], *tr[4], *lay[4]; int lvl, sched; comm *pe; } mn_group;   /* lvl / sched: the group's level, and whether it is a schedule group (their ids) */
/* G->pe is the group's flat PE communicator (rank = PE - g0); under vPE every size in this header counts PEs, g = PEs
 * of the group, nr = vpe_lanes() g ranks, one rank per PE; the all[d] arrays are unused (all[0] == pe). Off: pe is
 * NULL. */
/* C = A B + X (X may be 0) over the group; C's share on this node is a new dbig (C->sh freed first if set).  Products beyond one
 * plane per node pool (na + nb > 2^(min(31, pool_log) + log2 gt) points) run as a grid of piece products over the shares: piece
 * views of the operands, the first piece straight into C, the others into a temporary window of this node's share and a shifted
 * fixed-length add with the cross-node carry scan; X then by mdb_add_shifted.  DIST_LOGN_TEST lowers the plane cap (tests). */
void rns_mul_dist_mn(mdb *C, const mdb *A, const mdb *B, const mdb *X, mn_group *G);
/* C3 step 3 (ECALC_DR >= 2, dr.h): checked products.  mdb_trk = a tracked residue r = (the whole number) mod DR_Q, valid on every node of the group, and, where this node holds
 * a share (held), the per-APU parts of that share's residue as recorded when it was produced.  Callers keep it in an explicit variable beside the mdb (never inside: in-place operations would
 * leave it stale).  rns_mul_dist_mn_chk(C, A, B, X, G, ta, tb, tx, tc): C = A B + X checked against R(A) R(B) + R(X) (tx 0 without X); a mismatch repeats the product once (an operand whose
 * stored share changed since production = fatal naming node and APU, a second failure = fatal); tc (optional) = R(C) and this node's parts of C's share.  mdb_trk_make: the residue and parts of
 * X over G (collective; G 0: X on this node alone); mdb_dr_store_inject: ECALC_TEST dr=store (every node calls; returns 1 where it corrupted a limb). */
typedef struct { uint64_t r; uint64_t part[DB_NQ]; int held; } mdb_trk;
void rns_mul_dist_mn_chk(mdb *C, const mdb *A, const mdb *B, const mdb *X, mn_group *G, const mdb_trk *ta, const mdb_trk *tb, const mdb_trk *tx, mdb_trk *tc);
void mdb_trk_make(const mdb *X, mn_group *G, mdb_trk *t);
int mdb_dr_store_inject(mdb *X, int node);
/* C3 step 4 (ECALC_DR >= 3): mdb_dr_inject = ECALC_TEST dr=<kind> (xql, cyc) on one limb of X's share on `node`; rns_dr_event = a failure of the division checks, counted with the pieces' and products' (returns the
 * run's count, agreed by the group); rns_dr_cache_drop = the transform cache's slots emptied before a repeat */
int mdb_dr_inject(mdb *X, int node, int kind, size_t occ, int attempt);
size_t rns_dr_event(mn_group *G);
void rns_dr_cache_drop(void);
/* a view of a sharded number: limbs [off, off + len) of m, len the normalised length within the window; every node of
 * G computes the same view (one allreduce_max over the group), the pack kernel honours the global offset */
typedef struct { const mdb *m; size_t off, len; } mdbv;
mdbv mdb_view(const mdb *m, size_t off, size_t len, mn_group *G);
/* C = A B (+ X) over the group with views; w = the low window: pieces of the grid starting at limb w or above are
 * skipped and the result is truncated to w limbs ((size_t)-1: the full product).  a truncated result is
 * delivered in basis w (C->N = w), the carry out of the top dropped (mod B^w) */
void rns_mul_dist_mn_v(mdb *C, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w);
/* C = A B with the grid's pieces skipped by two cuts -- pieces whose limbs end at
 * or below lowcut (oa + ob + len_a + len_b <= lowcut; 0: none -- the A_h mu product, of which only t >> cut is used:
 * X is low by at most the number of skipped pieces + 1, absorbed by the up-corrections) and pieces starting at or
 * above highcut (oa + ob >= highcut; (size_t)-1: none -- the low product X Q mod B^w, delivered in basis highcut) */
void rns_mul_dist_mn_cut(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t lowcut, size_t highcut);
void rns_mul_dist_mn_cut_v(mdb *C, const mdbv *A, const mdbv *B, mn_group *G, size_t lowcut, size_t highcut);   /* int15k (lean division): the same on views */
/* the low w limbs of A B (the division's X Q): the grid with the pieces above w skipped, in basis w */
void rns_mul_low_mn(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t w);
void rns_mul_dist_mn_shape(size_t na, size_t nb, mn_group *G, int *ka, int *kb);   /* the grid the product forms (tests: the cut references) */
int rns_dist_p24_crt_all(void);                                   /* 1 = the P24 CRT flag covers every shift class (P24_CRT_ANY, default 1, or P24_CHECK) */
size_t rns_dist_p24_products(void);                              /* P24 products formed in this process (rns_dist.c; tests print it) */
int  rns_mul_dist_mn_logcap(mn_group *G);                     /* log2 of the plane cap over G (one plane per node pool; tests) */
/* P1 (DM_MN_WRAP): the division's low product in two parts -- the pieces at or above wskip skipped, C in a given
 * basis; and one cyclic plane of P points (C == A B mod (B^P - 1), C < B^(P + 4), basis >= P + 4); the group's
 * smallest plane (log2) */
void rns_mul_dist_mn_low_basis_v(mdb *C, const mdbv *A, const mdbv *B, mn_group *G, size_t wskip, size_t basis);
void rns_mul_dist_mn_cyc_v(mdb *C, const mdbv *A, const mdbv *B, mn_group *G, size_t P, size_t basis);
int  rns_mul_dist_mn_logmin(mn_group *G);
int  rns_mul_dist_mn_logcap_g(int g), rns_mul_dist_mn_logmin_g(int g);   /* the same by the group size (binsplit.c dm_layout: DM_MN_WRAP's plane) */
/* the block-pool bytes per device at the peak of C = A B (+ X) of na x nb limbs over g nodes (the grid's largest piece
 * at the group's cap; the plane pools stay at their init size), for shares of share_a, share_b, share_c limbs; *pieces = the grid */
size_t rns_mul_dist_mn_scratch(size_t na, size_t nb, int has_x, int g, size_t share_a, size_t share_b, size_t share_c, int *pieces);
size_t rns_mul_dist_mn_stage(size_t na, size_t nb, int g, size_t share_a, size_t share_b, size_t share_c, size_t *qmax);   /* the xshmem staging per APU (limbs) */
size_t rns_mul_dist_mn_vslot(size_t na, size_t nb, int g);   /* the layered comm's v-exchange slots per APU thread (bytes, hipMalloc'd outside the layout) */
/* the level -> group-size schedule of the distributed tree from MN_GROUPS (default, the powers of two
 * dividing the size, then the odd part's prime factors ascending -- 576 -> 2, 4, ..., 64, 192, 576; a power of two: the binary
 * tree; L's "powers of two then size" is MN_GROUPS=2,4,...): out[l-1] = the group size of level l, increasing, each a multiple of the previous or the size
 * itself; returns the level count (0 at size 1), aborts on an invalid list.  A pure function of (size, MN_GROUPS); mn.c calls it */
int  mn_groups_parse(int size, int *out, int max);
void rns_dist_cap_test(int logn);                             /* force the mn tier's plane cap 2^(logn + floor(log2 g)) from here on (0: the default); mn_tree's ECALC_TEST tree_logn */
/* C += X << k in place on C's shares (C's basis N must hold the sum: an overflow aborts); X sharded over any
 * subgroup of G; a chunked exchange over the four meshes, then one fixed-length add per share and the carry scan */
void mdb_add_shifted(mdb *C, const mdb *X, size_t k, mn_group *G);
/* C->n = 1 + the highest nonzero limb below `below` (min(N, below)) over the group (one allreduce_max) */
void mdb_norm(mdb *C, mn_group *G, size_t below);
#ifdef __cplusplus
}
#endif
#endif
