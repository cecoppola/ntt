/* mdb.h - a big number sharded over a node group (Phase 8 M3, PLAN.md 17).
 *
 * The number has n limbs (normalised: limb n-1 != 0 or n == 0) and is held by the nodes [g0, g0+g) of the
 * machine: node g0 + k holds the contiguous share [N k/g, N (k+1)/g) in its dbig `sh` (sh.n = the share's
 * length, never normalised), where N >= n is the sharding basis (the allocated length of the product that
 * made it; the basis does not move when the number is normalised).  Nodes outside the group hold nothing.
 * Every node of a larger group can describe the number from (n, N, g0, g) alone, so the redistribution
 * of the multi-node product needs no boundary exchange.
 *
 * The product C = A B (+ X) over a group G (mn_group): the four-step transform over 4 gt ranks (the group's
 * first gt nodes, gt the largest power of two <= g, each node's four APUs), rank rho = gt d + r holding
 * the block-cyclic rows [rho R/nr, (rho+1) R/nr) (ntt_dist.h).  Each operand is redistributed once: node
 * k packs, for every rank (r, d), the limbs of its share that lie on that rank's rows -- one contiguous
 * segment of the rank's (column, row)-ordered row sequence, since a share is a contiguous limb range --
 * and one all-to-all over mesh d (slabs padded to a common size) delivers them; the rank concatenates the
 * segments (they arrive in node order = sequence order) and transposes into the row layout.  The result's
 * rows go back the same way (one all-to-all per product) into the nodes' contiguous shares of C, sharded
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
 * group (rank = node - g0) for APU thread d, tr[d] the mesh over the first gt nodes (the transform's), both 0
 * when the group is one node; lay[d] the layered communicator over tr[d] (built by the product, cached) */
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
 * skipped and the result is truncated to w limbs ((size_t)-1: the full product) */
void rns_mul_dist_mn_v(mdb *C, const mdbv *A, const mdbv *B, const mdb *X, mn_group *G, size_t w);
/* the low w limbs of A B (the division's X Q): the grid with the pieces above w skipped */
void rns_mul_low_mn(mdb *C, const mdb *A, const mdb *B, mn_group *G, size_t w);
/* C += X << k in place on C's shares (C's basis N must hold the sum: an overflow aborts); X sharded over any
 * subgroup of G; a chunked exchange over the four meshes, then one fixed-length add per share and the carry scan */
void mdb_add_shifted(mdb *C, const mdb *X, size_t k, mn_group *G);
/* C->n = 1 + the highest nonzero limb below `below` (min(N, below)) over the group (one allreduce_max) */
void mdb_norm(mdb *C, mn_group *G, size_t below);
#ifdef __cplusplus
}
#endif
#endif
