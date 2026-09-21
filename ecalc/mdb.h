/* mdb.h - a big number sharded over a node group (Phase 8 M3, PLAN.md 17).
 *
 * The number has n limbs (normalised: limb n-1 != 0 or n == 0) and is held by the nodes [g0, g0+g) of the
 * machine: node g0 + k holds the contiguous share [N k/g, N (k+1)/g) in its dbig `sh` (sh.n = the share's
 * length, never normalised), where N >= n is the sharding basis (the allocated length of the product that
 * made it; the basis does not move when the number is normalised).  Nodes outside the group hold nothing.
 * Every node of a larger group can describe the number from (n, N, g0, g) alone, so the redistribution
 * of the multi-node product needs no boundary exchange.
 *
 * The product C = A B (+ X) over a group G (mn_group): the four-step transform over nr = 4 g ranks (every
 * node of the group, each node's four APUs; Phase 11 L), rank rho = g d + r holding the block-cyclic rows
 * [R rho / nr, R (rho+1) / nr) -- floor or ceil of R / nr each, ntt_dist's equal map when g is a power of two,
 * the general map (rns_dist.c gen_fwd / gen_inv_pw, alltoallv exchanges of per-pair slabs) otherwise, so a
 * group of 3, 5, 6, 9 or 576 nodes balances.  Each operand is redistributed once: node k packs, for every
 * rank (r, d), the limbs of its share that lie on that rank's rows -- one contiguous segment of the rank's
 * (column, row)-ordered row sequence, since a share is a contiguous limb range -- and one alltoallv over
 * mesh d (exact segments, B7) delivers them; they arrive in node order = sequence order, so the received
 * buffer is the rank's sequence, transposed into the row layout.  The result's rows go back the same way
 * (one alltoallv per product, straight from the CRT output) into the nodes' contiguous shares of C, sharded
 * evenly over G; the CRT run spills are all-gathered (4 C limbs per rank) and added to the shares by a
 * fixed-length chunked-carry add, the carries across the node boundaries resolved by a scan over the
 * nodes' (carry, propagate) flags and a second pass on the nodes that receive one.  X (the tree add
 * P = P_A Q_B + P_B) rides as the CRT's added operand, redistributed like A and B. */
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
 * all[d] (built by the product, cached).  gt (the largest power of two <= g) and tr[d] (the mesh over the first
 * gt nodes) are no longer used by the product (Phase 11 L: every node transforms); kept for mn.c / t_dist */
typedef struct mn_group { int g0, g, gt, me; comm *all[4], *tr[4], *lay[4]; } mn_group;
/* C = A B + X (X may be 0) over the group; C's share on this node is a new dbig (C->sh freed first if set).
 * Products beyond one plane per node pool (na + nb > 2^(min(31, pool_log) + log2 gt) points) run as a grid of piece
 * products over the shares (Phase 9 A3): piece views of the operands, the first piece straight into C, the others
 * into a temporary window of this node's share and a shifted fixed-length add with the cross-node carry scan;
 * X then by mdb_add_shifted.  DIST_LOGN_TEST lowers the plane cap (tests). */
void rns_mul_dist_mn(mdb *C, const mdb *A, const mdb *B, const mdb *X, mn_group *G);
/* a view of a sharded number: limbs [off, off + len) of m, len the normalised length within the window; every node of
 * G computes the same view (one allreduce_max over the group), the pack kernel honours the global offset */
typedef struct { const mdb *m; size_t off, len; } mdbv;
mdbv mdb_view(const mdb *m, size_t off, size_t len, mn_group *G);
/* C = A B (+ X) over the group with views; w = the low window: pieces of the grid starting at limb w or above are
 * skipped and the result is truncated to w limbs ((size_t)-1: the full product).  Phase 10 A5: a truncated result is
 * delivered in basis w (C->N = w), the carry out of the top dropped (mod B^w) */
void rns_mul_dist_mn_v(mdb *C, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w);
/* Phase 10 A5 (results/A-div.md, the division's cuts): C = A B with the grid's pieces skipped by two cuts -- pieces whose
 * limbs end at or below lowcut (oa + ob + len_a + len_b <= lowcut; 0: none -- the A_h mu product, of which only t >> cut is
 * used: X is low by at most the number of skipped pieces + 1, absorbed by the up-corrections) and pieces starting at or
 * above highcut (oa + ob >= highcut; (size_t)-1: none -- the low product X Q mod B^w, delivered in basis highcut) */
void rns_mul_dist_mn_cut(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t lowcut, size_t highcut);
/* the low w limbs of A B (the division's X Q): the grid with the pieces above w skipped, in basis w */
void rns_mul_low_mn(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t w);
void rns_mul_dist_mn_shape(size_t na, size_t nb, mn_group *G, int *ka, int *kb);   /* the grid the product forms (tests: the cut references) */
int  rns_mul_dist_mn_logcap(mn_group *G);                     /* log2 of the plane cap over G (one plane per node pool; tests) */
/* Phase 12 G: the block-pool bytes per device at the peak of C = A B (+ X) of na x nb limbs over g nodes (the grid's largest piece
 * at the group's cap; the plane pools stay at their init size), for shares of share_a, share_b, share_c limbs; *pieces = the grid */
size_t rns_mul_dist_mn_scratch(size_t na, size_t nb, int has_x, int g, size_t share_a, size_t share_b, size_t share_c, int *pieces);
/* Phase 11 L: the level -> group-size schedule of the distributed tree from MN_GROUPS (default, Phase 12 G: the powers of two
 * dividing the size, then the odd part's prime factors ascending -- 576 -> 2, 4, ..., 64, 192, 576; a power of two: the binary
 * tree; L's "powers of two then size" is MN_GROUPS=2,4,...): out[l-1] = the group size of level l, increasing, each a multiple of the previous or the size
 * itself; returns the level count (0 at size 1), aborts on an invalid list.  A pure function of (size, MN_GROUPS); mn.c calls it */
int  mn_groups_parse(int size, int *out, int max);
/* C += X << k in place on C's shares (C's basis N must hold the sum: an overflow aborts); X sharded over any
 * subgroup of G; a chunked exchange over the four meshes, then one fixed-length add per share and the carry scan */
void mdb_add_shifted(mdb *C, const mdb *X, size_t k, mn_group *G);
/* C->n = 1 + the highest nonzero limb below `below` (min(N, below)) over the group (one allreduce_max) */
void mdb_norm(mdb *C, mn_group *G, size_t below);
#ifdef __cplusplus
}
#endif
#endif
