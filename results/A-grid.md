# A-grid — the grid of piece products over shares (A3) and the 3·2³⁰ plane variant measured (C5) (2026-09-19)

Branch `agrid` (from `main` @ a75474d). PLAN.md §19 (agent A-grid), results/M3.md "Open issues" (first item).
Files owned: `ecalc/rns_dist.c` (the `_mn` product tier and the single-node dist tier), `ecalc/mdb.h`; new
`ecalc/tests/t_mn_grid.c` (+ its Makefile rule). Touched outside: `ecalc/dbig.c/.h` — one exported
function, `db_share_add_shifted` (the fixed-length shifted add reporting carry-out and propagate; two lines,
commented "Phase 9 A3").

## A3 — the grid over shares

M3's `rns_mul_dist_mn` ran one plane per node pool: a product needed na + nb ≤ 2^(31 + log₂ gt) points,
which the 4 × 10¹⁰ top level (2.2 × 10⁹ × 2.2 × 10⁹ limbs) and the division's products exceed at `size` < 8.
Now:

**Cap.** One plane per node pool = 2^(min(31, POOL_LOG) + log₂ gt) points (`mn_logn_cap`); `DIST_LOGN_TEST`
lowers it for tests. Above it the product is a grid of piece products, `split_grid_cap`: piece counts (ka, kb)
with ⌈na/ka⌉ + ⌈nb/kb⌉ ≤ cap chosen for the fewest plane points in total (the single-node `split_grid` is now a
wrapper of it, unchanged in behaviour).

**Views (`mdbv`, `mdb_view`).** A view is (m, off, len): the limbs [off, off + len) of a sharded number, len
its normalised length within the window, computed by every node of the group identically (each node scans its
share ∩ window with `db_norm` on a `db_view`; one `allreduce_max`). The redistribution packs a node's part of a
view — its share cut to the window — with the pack kernel reading the share at the window's offset
(`view_share`, `redistribute`); the segment tables are built from the views' shares, so nothing else changes:
the operand pieces are never copied.

**Piece delivery (`mn_core`).** A piece product at limb offset `shift` with basis Np covers the window
[max(clo, shift), min(chi, shift + Np)) − shift of every node's share [clo, chi) of C (`piece_window`). The
reverse redistribution's segments are built from these windows; the first piece (shift 0) is scattered straight
into C's zero-filled shares and its spills added there (M3's path, bit for bit — the one-plane case is exactly
M3's code). Every other piece is scattered into a temporary T of this node's window (in the piece's
coordinates), the spills are added to T (`db_share_add_spills`, fixed length, the node carry scan), then
C's share += T << (window offset) by `db_share_add_shifted` (fixed length cn, carry-out and propagate) and a
second node carry scan. No extra traffic: the piece's rows go to exactly the nodes whose shares they hit.

**The node carry scan (`share_carry_fix`).** As M3's: the (carry, propagate) bytes all-gathered over mesh 0,
carry into node r = c_{r−1} | (p_{r−1} & carry into r−1), + 1 on the shares that receive one. A node that had
nothing to add (an empty window of a piece, or nothing of X) has not looked at its limbs, so it reports
propagate = 0 (M3 reported 1 for an empty share, which is right only for a share of zero limbs); if such a
node's + 1 carries out, that is a new carry and the scan runs another round (a group-wide max decides; never in
practice, at most g rounds). A carry out of the top node aborts ("carry out of the top share").

**X (the tree add P = P_A Q_B + P_B).** Rides in the CRT as before when one plane suffices; with a grid it is
added afterwards by `mdb_add_shifted(C, X, 0, G)`.

**`mdb_add_shifted(C, X, k, G)`: C += X << k on the shares.** Node r's share needs X's limbs
[clo − k, chi − k) ∩ [0, nX): its window. APU thread d serves quarter d of every node's window in rounds of
2^26 limbs: per round one all-to-all over mesh d with slabs of the round size (a pair's slab = the sender's
part of X cut to the receiver's quarter chunk, contiguous; both sides compute the cut from the descriptors),
packed by `k_pack_rng`, unpacked by `k_unpack_rng` into a temporary of this node's window; then one
`db_share_add_shifted` per share and the carry scan. Buffers per APU: 2 × g × 2^26 × 8 B (1 GB at g = 2).

**Low products (`rns_mul_low_mn`, `rns_mul_dist_mn_v` with w).** As the single-node `rns_mul_low_db`: views
A[0, w), B[0, w), the pieces whose limbs start at or above w skipped, the result truncated to w and normalised
(`mdb_norm(C, G, w)`), the share limbs above w zeroed (`share_zero_from`) so the result stays a valid operand of
the adds (which read every limb of a share).

**Interface (mdb.h).** `rns_mul_dist_mn` unchanged. Added: `mdbv`, `mdb_view`, `rns_mul_dist_mn_v(C, A, B, X,
G, w)`, `rns_mul_low_mn(C, A, B, G, w)`, `mdb_add_shifted(C, X, k, G)`, `mdb_norm(C, G, below)`. Invariant kept
by every result: share limbs at or above C->n are zero. A one-node group (g = 1) is not supported by the
products (the meshes are null); `mdb_view`/`mdb_norm` handle it.

## C5 — the 3·2³⁰-point plane for the single-node top levels (measured only, not adopted)

`DIST_R3=1` (rns_dist.c only, no change to ntt_dist.c): in `dist_core` a product of logn ≥ 30 points whose
nc fits 3·2^(logn−2) runs on a plane of that length (3·2²⁸, 3·2²⁹, 3·2³⁰ — the last above the 2³¹ cap, so
`dist_cap` = 3·2³⁰ and the grid's cost model counts 3·2^k planes). The four-step with rows of C = 3·2^logk
through the radix-3 layer (`ntt_fwd3`/`ntt_inv3`; position t of third r holds X[3 brev(t) + r]) and columns
of R = 2^logR; twiddle w_n^(i j) = twr[e / C] · twc[e mod C] with w_n the 3·2^(logR+logk)-th root
(`ec_root3`); own copies of the pack/unpack kernels (generic in C) and the xGMI all-to-all; the CRT unchanged
(C stripes). Memory: the planes xa[4] take 4q = 3·2³⁰ limbs of pool 0 (grown to 32 GiB per APU by
`dpool_get`'s power-of-two rounding); xb and the slabs (3q + 16 limbs = 18 GiB) come from the dbig block pool
(pool 1's tail is donated to that pool at the 2³¹ layout's 3q, so pool 1 cannot hold them), i.e. +18 + 16 = +34 GiB
per APU over the 2³¹ layout (+136 GB per node).

## Tests

(filled in below from the aac6 batches; job 20709 on ppac-pl1-s24-16, clone `~/ntt-agrid`)
