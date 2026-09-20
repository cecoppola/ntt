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

Job 20709 (one node, ppac-pl1-s24-16, 45 min; the node-processes share it), clone `~/ntt-agrid` built
from the branch bundle; the batch scripts are `~/agrid/agrid_b1..4.sh`, the logs `~/agrid/out/`.

### `tests/t_mn_grid` (the grid over shares against the host product)

`SLURM_JOB_ID=$J ./mnrun.sh <procs> ./tests/t_mn_grid 1 28` (scale 1, POOL_LOG 28; `DIST_LOGN_TEST=24` set by
the test: cap 2^(24 + log₂ gt)). Every node checks its own share limb by limb, the length, and the zeros
above the length. Per shape and generator (uniform, all-ones): the product, the product + X, three low
products (w = na + 2, na/2 + 1, n − 1), a product of two views (A[na/3, +na/2) × B[7, ..)) and its low product,
and `mdb_add_shifted` at k = 0, 7, na/2, na on the product's shares; 120 checks per node.

| node-processes | gt | cap | shapes (limbs) → grids | result |
|---|---|---|---|---|
| 2 | 2 | 2^25 | 1.0e7 × 8.4e6 (one plane), 2.0e7 × 1.7e7 (1 × 2), 3.5e7 × 3.5e7 (2 × 3, the 4 × 10¹⁰ shape), 5.0e7 × 1.3e7 (3 × 1), 3.0e7 × 3.0e7 with A on node 0 and B, X on node 1 (the tree's layout, 1 × 2) | VERIFY OK (120) on both |
| 3 | 2 | 2^25 | the same (node 2 redistributes only; A on node 0, B and X on nodes 1–2) | VERIFY OK (120) on all three |
| 4 | 4 | 2^26 | the same shapes doubled (2.0e7 × 1.7e7 … 1.0e8 × 2.7e7); A on nodes 0–1, B, X on 2–3 | VERIFY OK (120) on all four |
| 2, decimal (`LIMB_BASE=10`) | 2 | 2^25 | the same | VERIFY OK (120) on both |

Times (TCP over loopback, meaningless as performance): the 2 × 3 product of 3.5e7 × 3.5e7 limbs 3.2 s at
2 processes; the doubled one 2.6 s at 4.

The first version failed the low products: a share that had nothing to add reported propagate = 1 (M3's
convention for an empty share) although its limbs were not all B − 1, so a real carry into it was passed on
to the next node and also added locally ("carry out of the top share"). Fixed as described above (propagate = 0
for a share that did not add, another scan round if its + 1 carries out).

### ecalc

| digits | size | env | result | wall |
|---|---|---|---|---|
| 10⁹ | 1 | (default) | VERIFY OK, `cmp` identical to `ref/e_1000000000.txt` | 9.0 s |
| 10⁹ | 2 | `POOL_LOG=29 BS_MDEV_LOGL=25` | identical (tree level 1: P, Q 5.6e7 limbs, one plane) | 12 s |
| 10⁹ | 4 | `POOL_LOG=29 BS_MDEV_LOGL=25` | identical (levels 1–2) | 13 s |
| 4 × 10¹⁰ | 2 (one node) | `POOL_LOG=30` | **out of device memory** at the first tree product: `HIP out of memory at dbig.c:73` in both node-processes, right after `dist_mn node 0: 1074769838 x 1147452389 limbs over 2 x 4 ranks (cap 2^31): 1 x 2 pieces` — the grid split itself works (the pieces were chosen), the node cannot hold two processes' leaf trees (112 GB of device pools each after init, the leaf P, Q of 1.1 × 10⁹ limbs each per process, the planes 2 × 8 GiB × 4 APUs per process, the slabs) | — |
| 2 × 10¹⁰ | 2 (one node, job 20733) | `POOL_LOG=30` | VERIFY OK; `cmp` identical to the size-1 run (sha256 5e30894f…8cf05); the top level's products (5.4e8 × 5.7e8 limbs) fit one 2^31 plane over 8 ranks: M3's path, 19–24 s per product over loopback TCP | 125 s (size 1: 50.7 s) |
| 2 × 10¹⁰ | 2 (one node, job 20741) | `POOL_LOG=30 DIST_LOGN_TEST=29` (the mn cap lowered to 2^30 so the top level is a grid) | VERIFY OK; **identical digits** (same sha256): both top-level products ran as **1 × 2 pieces over the shares** (536799527 + 287155795 limbs at 0, then at 287155795 "accumulated" into the windows [0, 268399763) of node 0's share and [268399763, 823955321) of node 1's), P_B added by `mdb_add_shifted` (3 rounds of 2^26; 1.7 s); node 0's division used the single-node grid at the 2^29 cap (4 × 5 pieces) | 118 s |
| 2 × 10¹⁰ | 1 | (default) | VERIFY OK (the reference for the two above) | 50.7 s |
| 2 × 10¹⁰ | 2 | `POOL_LOG=29` | fails in the **leaf** tree (node 1's batch tier: `HIP an illegal memory access at ntt.c:383` at a 2^29-point batch — the single-node pipeline at a pool too small for 10¹⁰-digit leaves; not the mn code) | — |

(The `META pool_log=31` in every log is printed before `rns_init`; the cap line "cap 2^31" at size 2 with
`POOL_LOG=30` confirms the pool size reached the process.)

### C5 measurements (4 × 10¹⁰, size 1, `ECALC_VERBOSE=2 RNS_VERBOSE=1`, the reference file evicted)

`DIST_R3=0` (the baseline, this branch): bs 43.5 s, dm 34.1 s, phases 77.7 s, wall 98.8 s, VERIFY OK,
digits identical to `results/e_4e10.out`. `DIST_R3=1`: bs 56.5 s, then the job's time limit ended the run in
the division (no digits). Per plane (load / ntt / crt / total):

| plane | limbs | load | ntt | crt | total |
|---|---|---|---|---|---|
| 2^30 | 1.06e9 | 0.16 | 0.37 | 0.05 | 0.55 s |
| 3·2^29 | 1.13e9 | 0.27 | 0.54 | 0.07 | 0.88 s (first call 2.94 s: tables, block-pool growth) |
| 2^31 | 1.13e9–1.85e9 | 0.32–0.35 | 0.67 | 0.08–0.10 | 1.08–1.17 s |
| 3·2^30 | 2.19e9–2.96e9 | 0.54–0.57 | 1.06 | 0.13–0.14 | 1.75–1.82 s (first call 14.6 s: pool 0 grown to 32 GiB, the 18 GiB block from the dbig pool, tables) |

The 3·2^30 plane costs 1.57× the 2^31 plane for 1.5× the points (the radix-3 layer and the non-power-of-two
column count cost ≈ 5 % per point). The top levels with it (steady state, the first-call costs excluded):

| product | 2^31 planes (baseline) | 3·2^k planes | change |
|---|---|---|---|
| level 23 (two products 1.06e9 × 1.13e9 ... the pairs of 5.7e8) | 2 × 2^30 + 2 × 2^31 = 3.3 s (level 4.10 s) | 2 × 2^30 + 2 × 3·2^29 = 2.9 s | −0.4 s |
| level 24 (two products 1.06e9 × 1.13e9: 1 × 2 pieces) | 4 × 2^31 = 4.5 s (level 5.36 s) | 2 × 3·2^30 = 3.5 s | −1.0 s |
| level 25 (2.19e9 × 2.7e7: 5 × 1 pieces of 2^29) | 10 × 0.28 = 2.8 s (level 3.54 s) | 2 × 3·2^30 = 3.5 s (1 × 1: the cost model prefers one big plane; the 5 × 1 grid would be as before) | +0.7 s (0 with the cost model fixed) |
| dm: Q_t r 2.22e9 × 1.11e9 (3 × 1 → 1 × 2) | 3 × 1.13 = 3.4 s | 2 × 1.8 = 3.6 s | +0.2 s |
| dm: A μ 2.22e9 × 2.22e9 (2 × 3 → 1 × 3) | 6 × 1.13 = 6.8 s | 3 × 1.8 = 5.4 s | −1.4 s |
| dm: the low product X Q (2 × 3 with pieces skipped → 1 × 3) | 5 × 1.13 = 5.7 s | 3 × 1.8 = 5.4 s (no piece skipped in a 1 × 3 grid: all three start below w) | −0.3 s |

Net ≈ −2.5 s of the 77.7 s of phases in steady state — PLAN §16's I10 estimate (−3 s) — at +34 GiB of device
memory per APU (pool 0 32 GiB instead of 16, the 18 GiB block for xb and the slabs), and a one-time 15 s
unless the pools are sized for it at init. The first-time cost aside, the measured levels were 23: 5.89 s
(+1.8, of which 2.1 first call), 24: 16.65 s (+11.3, of which 12.9 first call), 25: 3.57 s (+0.0). Not
adopted: `DIST_R3` stays 0; the code is a switch in `rns_dist.c` only. Open: the 4 × 10¹⁰ digits with
`DIST_R3=1` were not verified end to end (the run was cut by the job limit after bs; `t_dbig 24 big` with
`DIST_R3=1` verified the 2^30 × 2^30 product on the 3·2^30 plane against the host product before the test was
killed at the next, 3.2e9-limb product — host memory, the reference product of 2^31 limbs).

## Open issues

- 4 × 10¹⁰ at size 2 needs two real nodes (or a node with the leaf tree of one process only): on one node the
  two node-processes run out of device memory at the first tree product (the grid split itself was chosen:
  1 × 2 pieces at the 2^31 cap). The grid over shares at scale is exercised by 2 × 10¹⁰ at size 2 with the cap
  lowered to 2^30 (1 × 2 pieces + `mdb_add_shifted` at the top level, identical digits) and by the t_mn_grid
  shapes (2 × 3, 3 × 1, low products, views, subgroups). Two real nodes were not free during this session.
- The stray `~/agrid-batch1.sh` on the aac6 login node was A-comm's batch script (the shared scratchpad's
  `batch1.sh` was overwritten between agents during the first ship); it was looping on an invalid job id and
  was killed by PID. My scripts and logs live under `~/agrid/`.
- `mdb_add_shifted` pads every slab to the round size and every node takes part in every round (g × 2^26 × 8 B
  per APU and buffer); a point-to-point exchange (comm_send/recv are host-buffer only today) would avoid the
  padding. Used only for X in the grid case (the tree's P_B: one add per level with a grid).
- The piece temporaries: a piece's window T on a node is up to the share's length (2.2 × 10⁹ limbs at size 2,
  4 × 10¹⁰: 17.6 GB per node spread over the four APUs) — allocated from the dbig block pool per piece.
- A node group of one node (g = 1) is not a valid argument of the products (the tree never forms one); A-div
  should call the single-node `rns_mul_dist_db` at size 1.
- The carry scan runs at most g rounds when a + 1 carries out of a share that had nothing to add; that share
  is then all B − 1 up to the carry — never seen; each extra round is one byte all-gather and one allreduce.
