# GRID_GROUPS — the grid of piece products over shares and node groups

Scope: the grid of piece products over shares and node groups: layout at any group size, MN_GROUPS schedules, v-slot release, the 576-node layout checks. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| A-grid | the grid of piece products over shares (A3) and the 3·2³⁰ plane variant measured (C5) (2026-09-19) |
| M3 | the top levels of the binary-splitting tree as distributed products over node groups (2026-09-18) |
| L | the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` |
| G | grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10, PLAN. |
| G12 | the top product at scale: the tree's levels as piece grids for any g, the exact spills, the k-way schedule, the per-node |
| B7ACCT | counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06) |
| C06 | G5 (X2 cut-group pool-offset asymmetry) and M3 (release the v-slots; S-1 remainder), 2026-10-11 (branch c06 = main bc800 |
| S36 | idea S-1: release (share) the v-slots between the tree levels (2026-10-10) |
| S45 | CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e) |
| S52 | more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9) |
| B06 | uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001 |
| O1 | overnight 6-node paired ABBA, MN_GROUPS `2,6` (default) vs single level `6` (TASKS 42) |
| C09 | best MN_GROUPS for every usable node count (N = 576 .. 544), 3.71e13 and 3.79e13 digits |
| c09_groups |  |
| C215 | the paired series for D1 (`RNS_AUTO_PIECE_COST`, C2), with `DIST_TWREC`, other sizes, and the 576 model (Phase 15, row 1 |

## A-grid — the grid of piece products over shares (A3) and the 3·2³⁰ plane variant measured (C5) (2026-09-19)

(former file: A-grid.md)

Branch `agrid` (from `main` @ a75474d). PLAN.md §19 (agent A-grid), results/GRID_GROUPS.md §M3 "Open issues" (first item).
Files owned: `ecalc/rns_dist.c` (the `_mn` product tier and the single-node dist tier), `ecalc/mdb.h`; new
`ecalc/tests/t_mn_grid.c` (+ its Makefile rule). Touched outside: `ecalc/dbig.c/.h` — one exported
function, `db_share_add_shifted` (the fixed-length shifted add reporting carry-out and propagate; two lines,
commented "Phase 9 A3").

#### A3 — the grid over shares

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

#### C5 — the 3·2³⁰-point plane for the single-node top levels (measured only, not adopted)

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

#### Tests

Job 20709 (one node, aac6-spx-a, 45 min; the node-processes share it), clone `~/ntt-agrid` built
from the branch bundle; the batch scripts are `~/agrid/agrid_b1..4.sh`, the logs `~/agrid/out/`.

##### `tests/t_mn_grid` (the grid over shares against the host product)

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

##### ecalc

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

##### C5 measurements (4 × 10¹⁰, size 1, `ECALC_VERBOSE=2 RNS_VERBOSE=1`, the reference file evicted)

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

#### Open issues

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

## M3 — the top levels of the binary-splitting tree as distributed products over node groups (2026-09-18)

(former file: M3.md)

Branch `m3-dist-top-levels` (from `main` at 62d81bb / c4b6d12). PLAN.md §17, step M3.
After M2 every node-process r ends the leaf tree with P_r, Q_r for its term range
[N r/size, N (r+1)/size); node 0 then received all P_r, Q_r over the thread-0 mesh
and combined them with its own host tier. M3 replaces that with the top
log2(size) levels of the tree computed as distributed products, every operand and
result sharded over the nodes of the level's group, so that the work per node
stays constant when `size` grows. The M2 combine stays reachable with
`MN_COMBINE=host`. At the end (M4 will distribute the division) node 0 gathers
the shares of the final P, Q and runs the single-node division and output, so
the whole run still produces the digits and is checked against the reference.

#### Files

| file | what |
|---|---|
| `ecalc/comm_layered.c` (+ `comm.h`) | the layered communicator: (4 APUs over xGMI) × (g nodes over mesh d) as one `comm` of 4 g ranks |
| `ecalc/mdb.h` | `mdb`: a number sharded over a node group; `mn_group`: a level's node group with its meshes |
| `ecalc/rns_dist.c` | `rns_mul_dist_mn`: C = A B (+ X) over a group — redistribution kernels, the four-step transform over the layered comm, the CRT, the reverse redistribution, the spill add with the cross-node carry scan |
| `ecalc/dbig.c/.h` | `db_pool_alloc/free` (per-APU scratch from the block pool), `db_zero_fill`, `db_share_add_spills`, `db_share_add_one` (fixed-length adds reporting carry-out and propagate), the generalised spill `sparse` (4 gt ranks, share offset) |
| `ecalc/mn.c/.h` | `mn_group_at` (per-level group meshes), `mn_allgather`, `mn_selftest_layered`, `mn_tree` (the top levels), `mn_gather_host` |
| `ecalc/ecalc.c` | `MN_COMBINE` switch (default `dist`; `host` = M2), the leaf → tree → gather flow |
| `ecalc/tests/t_dist.c` | `DIST_LAYERED=1`: the layered communicator checked as in `mn_selftest` (run with `mnrun.sh`) |
| `ecalc/comm_tcp.c` | connect retry window 60 → 600 s (a level's group meshes open when its members arrive) |

#### Design

##### Rank mapping

A node-process drives its four APUs with four threads; APU thread d of every
node is joined by TCP mesh d (M1). For a product over a group of g nodes the
transform's rank space is 4 g ranks: **APU d of the group's node r is global rank
ρ = g d + r** (APU-major). With this order the g slabs a sender holds for the APUs
d' of all nodes are contiguous in its slab buffer, so the intra-node stage of the
all-to-all is the existing xGMI push with slabs of g × bytes, and no re-packing is
needed before it. Rank ρ holds the block-cyclic rows [ρ R/nr, (ρ+1) R/nr) of the
R × C plane with row i, column j ↔ point i + R j — exactly `ntt_dist.c`'s
convention, unchanged.

##### The layered communicator (`comm_layered.c`)

`comm_layered_create(intra = comm_xgmi(d), inter = mesh d over the group, d)`:
rank g d + node, size 4 g. `alltoall(sb, rb, bytes)` is
1. intra: `comm_xgmi` all-to-all with slabs of g × bytes — APU d sends block d'
   (= slabs {g d' + r'}) to APU d'; the receive is laid out [d source][r' dest];
2. a block transpose (4 g device copies of one slab) to [r'][d]; inter: the TCP
   mesh all-to-all with slabs of 4 × bytes — node r sends the four slabs
   (r, 0..3) → (r', d) to node r'; the receive is [r][d];
3. (in `wait`) a block transpose to [d][r] = global source rank order.

The scratch for the transposes is one slab buffer (the caller's, from the dbig
block pool, or hipMalloc'd). `barrier` and `allreduce_max` compose intra and
inter. This is the hierarchical all-to-all of M7 from the start; the 1 GbE TCP
stage dominates on aac6 (correctness only).

##### Sharded numbers (`mdb`)

An `mdb` is (n, N, g0, g, sh): n limbs (normalised length), held by nodes
[g0, g0 + g) with node g0 + k owning the contiguous share [N k/g, N (k+1)/g) in
its own dbig `sh` (its four quarters; sh.n = the share's length, never
normalised). N ≥ n is the sharding basis = the allocated length of the product
that made the number, so normalising (finding the top nonzero limb, one
`allreduce_max`) moves no data. A leaf is (n, n, r, 1). Every node of a larger
group describes any operand from these four numbers, so the redistribution needs
no boundary exchange — only the sibling group's descriptors, all-gathered once
per level (8 u64 per node).

##### The product over a group (`rns_mul_dist_mn`)

Group G = nodes [g0, g0 + g); the transform runs on the first gt = 2^⌊log2 g⌋
nodes (the plane's row count must divide by nr = 4 gt); the other nodes take
part in the redistributions only (they hold shares of the operands and of the
result). n = 2^logn ≥ na + nb points, cap 2^(31 + log2 gt) (one plane per node
pool; no grid split over the shares yet), rows = R/nr ≥ 32.

Rank ρ's rows form the *row sequence* t = j rows + il ↔ limb m = R j + ρ rows + il
(column-major over its rows), which is increasing in t. A node's contiguous share
[lo, hi) therefore meets the sequence in **one segment** [seq_start(lo),
seq_start(hi)), where seq_start(m) is the first t with m(t) ≥ m. So the
redistribution of an operand is one all-to-all-sized exchange:

- every node packs, for each rank (r, d) with r < gt, the limbs of its share on
  that rank's rows in sequence order (`k_pack_mn`, coalesced reads of the runs);
  thread d's slabs are the segments for the ranks (0..gt−1, d);
- one all-to-all over mesh d of the group (slabs padded to S = ((maxshare − 1)/R + 2) rows
  limbs, the bound on any segment; the padding is at most two rows per pair);
- the receiver's g segments arrive in node order = sequence order; `k_gather_mn`
  looks up the segment of each t by a binary search over the g (t0, t1) pairs
  (computed on the host from the descriptors, contiguous by construction),
  canonicalises per prime and writes the transform's row layout x[il C + j].

A and B are redistributed once per product (not per prime) and gathered per
prime from the packed slabs; the added operand X of the tree (P = P_A Q_B + P_B)
is redistributed the same way and compacted (no transpose) into the CRT's `x`
array, so the tree add costs one more redistribution and no add pass — the CRT's
`c = a b + x` path of the batch tier, with the final carry into limb nc.

The transform is `dist_core`'s: per prime, forward A and B, pointwise, inverse
over the layered comm (three layered all-to-alls per prime), then the local
transpose and the striped CRT (one stripe per column j of the rank's rows,
q = n/nr limbs of output in sequence order, a 4-limb spill per stripe).

The reverse: the CRT output is already the rank's result sequence; `k_pack_out_mn`
cuts it into the segments of every node's share of C (sharded evenly over G with
basis N = na + nb (+1 with X)), one all-to-all over mesh d, `k_scatter_mn` places
the ranks' segments into the node's share (zero-filled first; the limb at
position n = R C, if N > n, gets nothing but the spill). The spills of all 4 gt
ranks (4 C limbs each) are all-gathered over the meshes (a replicated all-to-all)
and added to every share by `db_share_add_spills`: the dbig chunked-carry add with
a generalised sparse operand (rank ρ's spill of column j at global limb
R j + (ρ+1) rows, offset by the share's lo), fixed length, reporting the share's
carry-out c and propagate p (all chunks propagate). The carries across nodes are
resolved as across chunks: the (c, p) bytes of the group are all-gathered over
mesh 0 and every node computes cin_r = c_{r−1} | (p_{r−1} & cin_{r−1}); a node
with cin = 1 adds one at its limb 0 (`db_share_add_one`). Finally the highest
nonzero limb over the group by `allreduce_max` gives n.

Per product: 3 (2 without X) operand exchanges + 12 layered all-to-alls + 1
result exchange + 1 spill all-gather + 2 tiny exchanges.

##### The tree (`mn_tree`)

Level ℓ = 1 … ⌈log2 size⌉: node r's group is [k 2^ℓ, min((k+1) 2^ℓ, size)),
k = r >> ℓ; if the group has no second half (a leftover at a non-power-of-two
size) the numbers are carried up unchanged. Otherwise A = the lower half
(2^(ℓ−1) nodes, lower terms), B = the rest: P = P_A Q_B + P_B, Q = Q_A Q_B, the
operands sharded over A's or B's nodes (their shares from the previous level;
the other side's nodes hold empty shares), the results over the joined group. No
operand is moved before the product: the redistribution reads each node's share
where it is. Group meshes (all g nodes, and the first gt when g is not a power
of two) are TCP meshes opened by the members at the level's start, on port
slots base + 512 s + 64 d + node (unique per level/kind/APU/node, so several
node-processes may share a host); at the top level the M1 meshes are reused.

##### M3's end

`mn_gather_host`: node 0 receives every node's share (host copy, mesh 0) into
the host bigint at its offset; the others exit after the barrier. Node 0 then
runs the M2 flow (host P, Q → device reciprocal and division → digits, T1, T2).
`size` 1 goes through none of this (`mn_init` returns 1 and `mn_dist` is 0).

#### Tests

Jobs 20700 (one node, s24-30; the several node-processes share it) and 20706
(three real nodes s24-16/26/30, one or two node-processes per node). Build in
`~/ntt-m3` from the branch bundle. Every ecalc run: `VERIFY OK` and the output
file byte-identical (`cmp`) to `ref/e_<digits>.txt`. Times are wall clock of
the whole run (correctness runs on a 1 GbE fabric; meaningless as performance).

##### The layered communicator (`tests/t_dist`, `DIST_LAYERED=1`, job 20700)

`mnrun.sh <procs> env DIST_LAYERED=1 ./tests/t_dist <logmax>`: per (logR, logC)
one plane of one prime over 4 gt ranks, a tagged pattern check of the all-to-all
itself (slab σ of rank ρ must arrive as slab ρ of rank σ), then the distributed
convolution against the one-rank engine on every rank's block-cyclic rows.

| node-processes | ranks | sizes | result |
|---|---|---|---|
| 2 | 2 × 4 | 2^20 … 2^24 (all logR, logC in 10..12), 2^26 | VERIFY OK (10 checks) on both |
| 4 | 4 × 4 | 2^20 … 2^24 | VERIFY OK (9 checks) on all four |
| 3 | 2 × 4 (gt = 2; node 2 idle) | 2^20 … 2^24 | VERIFY OK on all three |

(A first version of this self-test failed on every rank: it gave each APU thread
its own prime as `mn_selftest` does, but the layered ranks of one node hold rows
of the *same* plane — the fix is in the test, `comm_layered` was right.)

##### ecalc, the digits (`m3test.sh`; `MN_COMBINE` default = the distributed tree)

| digits | POOL_LOG | node-processes (placement) | extra env | result | wall | tree levels |
|---|---|---|---|---|---|---|
| 10⁶ | 27 | 2 (one node) | | identical | 4.3 s | 1.4 s |
| 10⁶ | 27 | 4 (one node) | | identical | 4.9 s | 1.6 s |
| 10⁶ | 27 | 3 (one node; gt = 2 at the top) | | identical | 4.9 s | 1.5 s |
| 10⁸ | 27 | 1 | | identical (the single-process path, unchanged) | 4.1 s | — |
| 10⁸ | 27 | 2 (one node) | | identical | 5.6 s | 1.9 s |
| 10⁸ | 27 | 4 (one node) | | identical | 5.6 s | 2.0 s |
| 10⁸ | 27 | 2 (one node) | `BS_MDEV_LOGL=21` (leaf P, Q left on device by the leaf's device top level) | identical | 5.8 s | 0.5 s |
| 10⁸ | 27 | 4 (one node) | `BS_MDEV_LOGL=21` | identical | 5.5 s | 2.1 s |
| 10⁸ | 27 | 2 (two real nodes, one each) | | identical | 11.5 s | 6.3 s |
| 10⁸ | 27 | 3 (three real nodes, one each) | | identical | 12.9 s | 8.7 s |
| 10⁹ | 29 | 1 | | identical | 8.9 s | — |
| 10⁹ | 29 | 2 (one node) | | identical | 11.7 s | 3.5 s |
| 10⁹ | 29 | 4 (one node) | | identical | 12.8 s | 4.0 s |
| 10⁹ | 29 | 1 | `BS_MDEV_LOGL=25` (mdev 0.4 s in the single-process bs) | identical | 7.8 s | — |
| 10⁹ | 29 | 2 (two real nodes, one each) | `BS_MDEV_LOGL=25` | identical | 48.2 s | 36.1 s |
| 10⁹ | 29 | 4 (two real nodes, two each) | `BS_MDEV_LOGL=25` | identical | 52.4 s | 39.3 s |

The size-1 runs at 10⁸ and 10⁹ (with and without `BS_MDEV_LOGL`) are the
unchanged single-process pipeline (`mn_init` returns 1 before any of the M3 code):
identical digits, VERIFY OK. Both leaf paths are exercised: leaf P_r, Q_r as host
numbers copied in (the leaf tree ends in the batch tier) and as device numbers
kept by the leaf's dev_mdev top level (10⁸, `BS_MDEV_LOGL=21`, "leaf … (device)"
in the log). Group sizes exercised: 2 (level 1), 4 (level 2, size 4), 3 with
gt = 2 (a redistribution-only node), across real nodes and sharing a node.
The tree's runs at 10⁹ cover products of 2^27 points over 8 and 16 ranks
(P ≈ 5.6 × 10⁷ limbs); the largest single exchange was ≈ 0.4 GB per node.

##### Fixed on the way

- `hosts_of` / `comm_tcp_create_at` used `strtok` while four threads open four
  meshes at once (size 4: "COMM_HOSTS lists 1 hosts", a segfault) → `strtok_r`.
- `mnrun.sh` computed `COMM_HOSTS` from `--ntasks-per-node`, which Slurm ignores
  when the process count is not a multiple of the node count (4 on 3 nodes):
  it now uses the largest node count dividing the process count, an explicit
  node list and block distribution; and a random port base per run, so a
  straggler of a failed run cannot catch a later run's connections.
- The plain-mesh self-test (`mn_selftest`) is skipped at non-power-of-two sizes
  (the rows do not divide); the layered self-test covers the transform nodes.
- The node carry scan first used `allreduce_modq` with weights 4^rank, which
  overflows beyond 30 nodes; it is a byte all-gather now.

#### Open issues

- The product over a group is one plane per node pool: products beyond
  2^(31 + log2 gt) points need the grid split over shares (views of shares with
  a global offset in the pack kernel, and a shifted distributed add) — needed
  at 4 × 10¹⁰ for `size` < 8 (2.2 × 10⁹-limb operands: 2^33 points).
- The segment lookup is a binary search over g entries per point and the host
  builds g-entry tables per operand: fine to thousands of nodes, but the
  all-gathers of descriptors and flags are point-to-point loops (O(g) sends per
  node); an `allgather` op in `comm` would replace them (M8).
- Group meshes are created per level (TCP connections); an RDMA communicator
  would carve sub-communicators instead.
- Nodes beyond the largest power of two in a group only redistribute; the load
  balance at non-power-of-two sizes is not addressed (the target sizes are
  powers of two).
- Memory per node: the packed slabs and the layered scratch come from the dbig
  block pool (≈ 4 q per APU on top of the 7 q of planes and slabs, q = n/nr);
  M9 accounts for it.

## L — the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` (Phase 11, PLAN.md §26 row L; 2026-09-20)

(former file: L.md)

Branch `l11` (from `main` @ 72aa2e9). Files owned: `ecalc/rns_dist.c`, `ecalc/mdb.h`, `ecalc/tests/t_mn_grid.c`.
Touched outside, minimal and commented "Phase 11 L (agent L)": `ecalc/newton_db.c` — `mdb_shift` and
`mdb_to_host_all` only (the two consumers named in the task; `recip_mn` / `newton_mn_divmod` untouched);
`ecalc/dbig.c` — three lines in `sparse_get` (the spill decode for unequal row parts) and its comment.
Clone `~/ntt-l` on aac6 (built from the bundle); batches `~/l11/l_batch{1,2}.sh`, logs `~/l11/out{1,2}/`.

#### C3-576 — the map

**Before.** A group of g nodes ran the transform on its first gt = 2^⌊log₂ g⌋ nodes (rank ρ = gt d + r, rows = R / 4 gt
each); the other nodes redistributed and idled in the product. At 576 = 9 · 2⁶ the top level would have idled 64 of
576 nodes (and a 3-way step 1 of 3).

**Now.** Every node of the group transforms: nr = 4 g ranks, rank ρ = g d + r (APU d of the group's node r, the layered
communicator over `G->all[d]` — the mesh over the whole group) holds

* the rows [R ρ / nr, R (ρ+1) / nr) of the R × C plane — ⌊R/nr⌋ or ⌈R/nr⌉ rows (`part0` / `partn` in rns_dist.c) — and
* the columns [C ρ / nr, C (ρ+1) / nr) of the column layout.

Its row sequence is t = j · rows + il ↔ limb m = R j + row0 + il, increasing in t, so a node's contiguous share still
meets it in one segment (`seq_start(m, R, nr, ρ)` takes the rank's actual range). For g a power of two the parts are
exactly R / nr and the transform is `ntt_dist.c`'s pipelined one on the same layered communicator: sizes 1, 2, 4 run
the same code as before, bit for bit. For any other g the transform is the general four-step in rns_dist.c
(`gen_fwd` / `gen_inv_pw`, kernels `k_twpack_g`, `k_unpack_g`, `k_pack_cols_g`, `k_unpacktw_g`; the twiddle tables are
the dist_plan's): per chunk k of the rank's rows (K = `DIST_CHUNKS`, 4), the row pass, twiddle and pack into slabs
(slab σ = my chunk rows × σ's columns, column-major, at offset col0(σ) · rk — back to back in rank order, no table), one
`comm_alltoallv` over the layered communicator with the exact per-pair counts (rows(ρ) chunk × cols(σ)), the wire time of
chunk k under the row pass and pack of chunk k + 1; then the unpacks (a table T[k][ρ] = (offset, first row, rows) of every
rank's chunk on the "my columns" side, `gplan_build`) and the column pass; the inverse mirrors it with the pointwise
product fused into the column inverse (`ntt_inv_pw_y`, as A6). The values are the same integers mod p as the one-rank
engine's by construction (an exact NTT), so the digits are identical to size 1 (measured below). `DIST_GEN=1` forces the
general map at a power-of-two g (tested at 2 and 4: same digits).

**What else had to become general.** The redistribution kernels (`k_pack_mn`, `k_scatter_mn`) take (R, nr, ρ) instead of
(rows, gt); the CRT stripes are the rank's rows × C = its sequence length q_s (the plane allocation is q = max over
ranks of rows × C and cols × R, the transform cache keys on that q — the same on every node); the spills of rank ρ land
at limb R j + R (ρ+1) / nr — `dbig.c`'s `sparse_get` decodes that when rows · nr ≠ R (the only change outside my files
that is not a consumer: three lines, the old formula kept verbatim for the equal case). The cap (`mn_logn_cap`) is
2^(min(31, POOL_LOG) + ⌊log₂ g⌋) as before, lowered by one where the rounding of R / nr would not fit the pools (never for
g ≤ 2304: at 576 nodes and 2⁴⁰ points, R = C = 2²⁰, rows 455/456, the largest rank plane is 456 · 2²⁰ = 478 M limbs of
the 2²⁹ available; at g = 9 and POOL_LOG 27, 911 · 2¹⁵ of 2²⁵). The minimum plane keeps ≥ 32 rows per rank
(logR ≥ 5 + ⌈log₂ nr⌉ — identical to the old 7 + log₂ gt at a power of two).

`mn_group.gt` / `tr[d]` are no longer read by the product (mn.c still creates `tr`; S can drop it with the communicator
rewrite). A group of one node is still not a product argument (unchanged).

#### The schedule: `mn_groups_parse` (`mdb.h`, for S's `mn.c`)

`int mn_groups_parse(int size, int *out, int max)` — a pure function of (size, `MN_GROUPS`): `out[l−1]` = the group size
of tree level l ≥ 1 (the nodes [k G_l, min((k+1) G_l, size)), k = rank / G_l); returns the level count (0 at size 1).
The list must be increasing, each size a multiple of the previous or the size itself (the top group is cut by the size);
anything ≥ size ends the list at size; an invalid list aborts with the reason. Default: the powers of two up to the
largest ≤ size, then size (576 → 2, 4, …, 512, 576; 9 → 2, 4, 8, 9 — what the tree does today). `MN_GROUPS=2,4,8,16,32,64,576`
gives the 9-way top step; `…,64,192,576` a 3-way then a 3-way. A level whose ratio to the previous is k > 2 is a k-way
step: the tree combines its k children in k − 1 products over the level's group (each balanced over all its nodes, the
operands sharded over the children's subgroups — exactly what `rns_mul_dist_mn` accepts today: A over a subgroup, B and
X over another, C over the whole group); that combine loop is S's `tree_level`. The layout itself has no notion of levels.

#### B7 — the four consumers on `comm_alltoallv` / `comm_allgather`

| consumer | before | now |
|---|---|---|
| the operand redistribution (`redistribute`, per operand per APU thread) | one all-to-all of g padded slabs S = (⌈maxshare/R⌉ + 1) rows; `k_gather_mn` found each point's segment by a binary search over g | `seg_tables`: my part's segment in every rank's sequence (send counts), every node's part in mine (receive counts), offsets back to back; one `alltoallv`; the received buffer **is** my sequence [0, seq_start(len)): `k_gather_mn` reads `rb[t]` (zero beyond) |
| the result exchange | `k_pack_out_mn` cut the CRT output into g padded slabs, all-to-all, `k_scatter_mn` at r · S | the CRT output `xb` is the send buffer itself (the nodes' windows are contiguous segments of it: sdsp = t0 · 8); the receive at prefix offsets, `k_scatter_mn` reads `rb[off[r] + k]` |
| the spills | an all-to-all of g copies of my 4 C limbs (send buffer g × 4 C per APU) | `comm_allgather(G->all[d], spill, spill_rb, 4 C · 8)` |
| `mdb_add_shifted` | per round of 2²⁶ limbs: sb, rb = g × 2²⁶ × 8 B per APU each | the rounds kept; both sides' cuts computed for every round first, one `alltoallv` per round with the exact cuts (`struct rng` gained `off`); sb = the largest round's send total (my part of X cut to the receivers' chunks — about two chunks), rb = one chunk: independent of g |
| `mdb_shift` (newton_db.c) | sb, rb = g × SL × 8 B per APU, SL = ⌈maxshare/4⌉ + 1, whatever the pieces' lengths | `struct piece` gained `off`; both sides' pieces from `piece_of` with prefix offsets, one `alltoallv`; sb, rb = my pieces' limbs (≤ my share / 4 per APU) |
| `mdb_to_host_all` (newton_db.c) | an all-to-all of g copies of the block (2 g × ms × 8 B on APU 0) | `comm_allgather` of one copy (g + 1 blocks) |

The redistribution's padding was only two rows per pair (C.md), so its gain is the layout (no search, no pack of the
result); the memory is in `mdb_shift` (−2 (g − 1) × share/4 × 8 B per APU per call: 17.6 GB per node at 4 × 10¹⁰ over 2
nodes, and at 576 nodes the padded form would have been g × share/4 — impossible) and `mdb_add_shifted`
(−(2 g − 3) × 512 MB per APU).

##### The scratch at size 2 (`mem_report`, block pool peak live per process; 10⁹ digits, POOL_LOG=29, one node)

`mnrun.sh 2 env POOL_LOG=29 ECALC_VERBOSE=1 MEM_REPORT_DEVS=1 ./ecalc 1000000000` with `main`'s binary (`~/ntt/ecalc`, 064da26)
and with this branch, node 0's summary (GB, all four APUs of the process; `pl:peak` = the block pool's peak live bytes,
where every slab, table and temporary of the multi-node product lives; `pl:hip` = the pool's hipMalloc fallback):

| phase boundary | main: pl:peak | main: pl:hip | L: pl:peak | L: pl:hip |
|---|---|---|---|---|
| after the tree (levels over 2 nodes) | 1.9 | 0.5 | **1.5** | 0.4 |
| after bs + dm (the division's shifts and products) | 3.0 | 1.6 | **2.5** | 1.2 |

−0.5 GB of peak scratch per process at 10⁹ over 2 nodes (shares of 2.8 × 10⁷ limbs: the padded g × SL of `mdb_shift`
was 2 × 2 × 7 × 10⁶ × 8 B = 0.22 GB per APU per buffer pair, plus `mdb_add_shifted`'s g × 2²⁶ rounds and the spill
copies). Scaled to 4 × 10¹⁰ over 2 nodes (shares of 1.1 × 10⁹ limbs) the same buffers are 4.4 GB per APU per `mdb_shift`
call = **17.6 GB per node** (C.md's figure; not measurable on one node — two node-processes at 4 × 10¹⁰ do not fit it).
Digits identical in both runs; the wall 27.9 → 26.1 s (dm 18.8 → 16.9: no padded copies, no result pack) on loopback TCP.

#### Tests (aac6, one node per batch, the node-processes sharing it over loopback TCP — correctness only)

Every run: `VERIFY OK` on every node, the digits (`cat` of the part files) `cmp`-identical to `~/ntt/ecalc/ref/e_<digits>.txt`
(or `results/e_1e10.out`).

Batch 1 (job 20808, aac6-spx-b, `~/l11/l_batch1.sh`):

| test | command (from `~/ntt-l/ecalc`, `J` the job) | result |
|---|---|---|
| t_mn_grid at 2, 3, 5, 6, 9 processes (5 shapes × 2 generators: the product, + X, three low products, the low cut with/without a high cut, the held product, views, `mdb_add_shifted` at four shifts; the "subgroups" shape has A over the lower half of the nodes and B, X over the rest) | `SLURM_JOB_ID=$J ./mnrun.sh <p> ./tests/t_mn_grid 0.5 27` (0.25 at 9) | VERIFY OK, 200 checks on every node, at every p (3, 5, 6, 9: "the general map"; the plane cap 2^25 / 2^26 / 2^26 / 2^27) |
| the general map forced at a power of two | `mnrun.sh 2 env DIST_GEN=1 ./tests/t_mn_grid 0.5 27`, `mnrun.sh 4 env DIST_GEN=1 …` | VERIFY OK (200) on every node |
| ecalc 10⁸ at sizes 3, 6, 9 | `mnrun.sh <p> env POOL_LOG=27 ./ecalc 100000000 <out>` | identical; 9.4 / 25.8 / 62.2 s (size 9: four levels 2, 4, 8, 9 — the last a 9-node group on the general map) |
| ecalc 10⁸ at sizes 2, 4 | the same | identical; 8.0 / 8.3 s |
| ecalc 10⁹ at size 3 | `mnrun.sh 3 env POOL_LOG=29 ./ecalc 1000000000 <out>` | identical; 24.5 s |
| ecalc 10⁹ at size 1 | `./ecalc 1000000000 <out>` | identical (VERIFY OK, 14.5 s on the shared node) |

Batch 2 (job 20813, aac6-spx-b, `~/l11/l_batch2.sh`):

| test | command | result |
|---|---|---|
| ecalc 10⁹ at sizes 2, 4 | `mnrun.sh <p> env POOL_LOG=29 ./ecalc 1000000000 <out>` | identical; 26.1 / 18.9 s |
| ecalc 10⁹ at size 2, `main`'s binary (the scratch "before") | the same from `~/ntt/ecalc` | identical; 27.9 s |
| **ecalc 10¹⁰ at size 4** | `mnrun.sh 4 env POOL_LOG=29 ./ecalc 10000000000 <out>` | **identical to `~/ntt/ecalc/results/e_1e10.out`**, all 4 nodes VERIFY OK; 99.3 s (bs 28.9, dm 61.8) |
| ecalc 10⁹ at size 3 with the grids forced (`DIST_LOGN_TEST=25`: the reciprocal's and division's products as 2 × 1, 2 × 2 grids with the low cut and the low product, over the general map, the transform cache over shares hitting) | `mnrun.sh 3 env POOL_LOG=29 DIST_LOGN_TEST=25 ./ecalc 1000000000 <out>` | identical; 26.5 s |
| ecalc 10⁹ at size 6 (extra) | `mnrun.sh 6 env POOL_LOG=28 ./ecalc 1000000000 <out>` | identical; 55.8 s |
| ecalc 10⁹ at size 9 (extra) | `mnrun.sh 9 env POOL_LOG=28 …` | **not run**: the TCP meshes failed to open (`bind: Address already in use` — `mnrun.sh`'s random port base collided with another process on the shared node; no computation started). 10⁸ at size 9 (batch 1) covers the 9-node group; not retried within the node budget |

#### Open issues

* The general map's exchange is not pipelined as deeply as the power-of-two one: the layered communicator keeps one
  v-exchange pending (`comm_layered.c`: posting the next completes the previous), so a chunk's wire time overlaps only
  the next chunk's row pass and pack, not two chunks as with `inflight 2`; and its v-scratch is the communicator's own
  `hipMalloc` (≈ 3 × the chunk's slab volume per APU: at 576 nodes and 2⁴⁰ points ≈ 3 × 478 M / 4 × 8 B ≈ 2.9 GB per APU
  at K = 4), not the caller's pool memory as the equal-slab scratch is — both are S's / C's file (`comm_layered.c` /
  the SHMEM transport) to fix when the v-exchange gets a consumer on the real fabric. The pool `tmp` slab of the
  equal path (q × 8 B per APU) is not allocated on the general path.
* On aac6 the general map costs more wall than the power-of-two one only because the loopback TCP transport
  serialises: 10⁸ at size 9 takes 62 s (36 processes' threads on one node) — no fabric number exists here; the
  per-rank work is 1/g of the plane at every g, which is the point.
* `mn.c` still creates the `tr[d]` sub-meshes for the first gt nodes (unused by the product now); `mn_selftest_layered`
  and `t_dist`'s layered check still test the gt-node layered communicator. Both go with S's communicator rewrite.
* The tree's k-way combine (`tree_level` for a ratio > 2) is S's; the parser and the layout are ready for it. The
  default schedule at 576 is 2, 4, …, 512, 576 (the task's stated default); `MN_GROUPS=2,4,8,16,32,64,576` selects
  the 9-way top step. Which is cheaper is X2's model's call (the 9-way step is one level of 8 products over 576 nodes
  vs three levels of 2 products over 128–576 nodes).
* The 4 × 10¹⁰ over two real nodes was not run (the nodes were never idle together); the 17.6 GB per node figure for
  `mdb_shift` is arithmetic from the buffer sizes (C.md), the measured numbers are the size-2 10⁹ ones above.

## G — grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10, PLAN.md §21)

(former file: G.md)

Branch `g10` (from `main` @ 4aca721). Files owned: `ecalc/rns_dist.c`, `ecalc/mdb.h`, `ecalc/newton_db.c`, the dm flow of
`ecalc/ecalc.c` (unchanged in the end: the flow's calls did not move). Touched outside, minimal and commented "Phase 10 A6 (agent
G)" / "(G)": `ecalc/ntt_dist.c` (one static helper and `dist_inv_pw`, 8 lines; C's file), `ecalc/ntt_dist.h` (one declaration),
`ecalc/rns_mul.h` (three declarations next to the existing `rns_mul_dist_db` ones), `ecalc/tests/t_mn_grid.c` (new cases; T's
directory, A-grid's test).

#### A5 — the cuts of the grid over shares (`rns_mul_dist_mn_cut`)

`rns_mul_dist_mn_cut(C, A, B, G, lowcut, highcut)` in `rns_dist.c` / `mdb.h`, exactly A-div's interface note:

* **lowcut** — a piece whose limbs end at or below the cut (`oa + ob + len_a + len_b <= lowcut`, the normalised view lengths as
  `mul_high_db` used on one node) is skipped: the A_h μ product of which only `t >> (k + 1)` is used (each skipped piece is
  < B^cut, X low by at most their number + 1, absorbed by the up-corrections). If (0,0) is skipped the first formed piece takes
  the accumulating path into the zero-filled shares (nothing else changes: `direct` was only a shortcut).
* **highcut** — pieces starting at or above it are skipped (A-grid's `w`), and the result is now **delivered in basis
  `highcut`** (`C->N = w`): the piece windows clip to the shares, and the carry out of the top node is dropped (mod B^w) instead of
  aborting (`g_top_ok` around `node_carry_in`). Before, the truncated product came out in basis na + nb with zeros above w, and
  the sharded division re-sharded it into basis w with an `mdb_shift` (an all-to-all of the whole low half); that shift is gone.
  `rns_mul_low_mn` and `rns_mul_dist_mn_v(w)` inherit the basis-w delivery (t_mn_grid's checks read the basis from C).
* `newton_mn_divmod`: `mn_prod_cut(&t, &Ah, &mu, G, k + 1, -1)` and `mn_prod_cut(&xql, &Xn, Q, G, 0, w)` at A-div's two marked
  places; `NEWTON_HIGHPROD=0` / `NEWTON_LOWPROD=0` keep the full products (the same switches as on one node).
* `rns_mul_dist_mn_shape(na, nb, G, &ka, &kb)` exposes the grid the product forms (the test's reference).

**t_mn_grid** (A-grid's test, extended): per shape and generator, the low cut at (na + nb)/2, nb/2 + 1 and na + nb (everything
skipped), each with and without a high cut at r.n − 1 — the reference is the host product minus exactly the pieces the grid
skips (from `rns_mul_dist_mn_shape` and the normalised piece lengths), then truncated; the low products check `C.N <= w`;
and (A1) a held product followed by the low product over the same B pieces. 180 checks per node at 2 processes (was 120).

#### A1 — the transform cache (`rns_dist.c`; PLAN §20 I4, A-div's "B2")

A-div's design was a cache of Q's piece transforms kept from the reciprocal's last doubling to the division's X Q (48 GiB per
APU at 4e10). Building the mechanism generally showed a better use of the same planes: **within one grid product**, every
piece transform is currently recomputed per piece product (a ka × kb grid does 2 ka kb forward transforms), while the loop (j
outer over B's pieces, i inner over A's) reuses B piece j across all i and A piece i across all j. With a cache of N slots the
grid costs ≈ ka + kb transforms: A's pieces get min(ka, N − 1) slots, B's piece of the current j the rest.

Mechanism (single node and over shares, the same slot table):

* a slot = the forward transform of one operand view (key: its dbig's quarters, offset and length — over shares the view's mdb,
  offset and length, the same decision on every node of the group) on EC_NP planes of q limbs per rank, allocated from the block
  pool at first use (`db_pool_alloc`, so they survive the pool's frees and go back to it);
* `dist_core` / `mn_core` take the two designated slots; a hit anywhere skips the operand's gather and forward transform (over
  shares also its all-to-all redistribution) — B's cached planes are read in place by the pointwise product, A's are copied into
  the product plane (one device-to-device copy per prime); a miss forwards into the designated slot;
* `mul_grid` / `mn_grid` plan the slots from the grid and return the planes to the pool at the end of every product (the keys
  are addresses: no entry outlives its operands), except pinned slots: `rns_dist_cache_hold(1)` makes the next product pin its
  B pieces until `hold(0)` — A-div's B2 exactly (the reciprocal's top step uses Q as the B operand, the division's X Q hits).
  Holding is **off** unless `RNS_DIST_CACHE_HOLD=1`: the in-product policy already transforms each of Q's pieces once inside X Q,
  so the hold only moves those transforms into the reciprocal and takes the pinned slots away from the A_h μ product in between
  (at 4e10 and N = 4: 4 + 8 + 4 transforms held vs 4 + 5 + 5 unheld for the top step, A_h μ and X Q).
* `RNS_DIST_CACHE` = slots (default 3; 0 = off), 16 GiB per slot per APU at 2^31 points (4 GiB over 2 nodes).
* `mul_high_db` (A-div's B3, in newton_db.c) became `mul_grid`'s `lowcut` (`rns_mul_high_db`), so the cache serves it too; the
  same split and piece order, bit for bit.
* `recip_mn` uses Q itself at the top step (Q_t = Q: no shift copy — one all-to-all less per top step).

Expected at 4e10 (2^31 planes, one forward transform + gather ≈ 0.36 s): the reciprocal's top step 6 → 4, r d 4 → 3, A_h μ
10 → 5, X Q 10 → 5, the top tree levels (5 × 1 and 1 × 2 grids) −5: ≈ −15 transforms ≈ −5 s, if the block pool holds the planes
without falling back to `hipMalloc` (0.057 s/GB, serialised under the pool's mutex).

#### A6 — the distributed tier's local transforms

* The pointwise product is fused into the column inverse's first pass: `dist_inv_pw(p, x, y, s)` in `ntt_dist.c` (=
  `dist_pw` + `dist_inv` through `ntt_inv_pw_y(FULL)`, the same modmul on the same canonical operands: bit-identical), used by both
  tiers in `rns_dist.c` (`DIST_PW_FUSE=0` restores the two passes). One plane read + write per prime per product less.
* Body 1 (N-kernel's register-blocked kernel) runs only for 7-stage passes, i.e. local lengths ≥ 2^17 (b1 takes 10 stages, a
  pass up to 7): the dist tier's rows/columns are 2^15/2^16 at a 2^31 plane, so it never reaches them. `DIST_LOGR_DELTA` moves the
  four-step split (logR = logn/2 + delta: +1 makes the rows 2^16 × columns 2^15 at 2^31 — still no 7-stage pass; the split that
  gives one, 2^14 × 2^17, needs delta −1, measured below). The radix-3 (`DIST_R3`) path keeps the separate pointwise (off by
  default).

#### Tests

Clone `~/ntt-g` on aac6 (and `~/ntt-g-a1` for the size-1 batches), built from the branch bundle; scripts `~/g_b1.sh` .. `~/g_b5.sh`,
outputs and logs under `~/g10/b1..b5/`. Every run: `VERIFY OK` on every node and the digits (`cmp` of the file, or of `cat` of
the part files) byte-identical to `~/ntt/ecalc/ref/e_<digits>.txt` / `results/e_4e10.out`. One node per batch, the
node-processes sharing it (TCP over loopback: multi-process walls are correctness only).

##### A5 (batch 1 / 3 / 5: jobs 20762, 20771, and the final one)

`SLURM_JOB_ID=$J ./mnrun.sh <s> env POOL_LOG=27 [BS_MDEV_LOGL=21] ./ecalc 100000000 out`, `... POOL_LOG=29 BS_MDEV_LOGL=25 ./ecalc 1000000000 out`:

| digits | sizes | extra | result |
|---|---|---|---|
| 10⁸ | 2, 3, 4 | (and size 2 with `BS_MDEV_LOGL=21`: the device top levels) | identical, all nodes VERIFY OK |
| 10⁸ | 2 | `DIST_LOGN_TEST=22` (the mn cap lowered: the division's products as 1 × 2 grids with the low cut and the low product in basis w) | identical |
| 10⁹ | 2, 4 | | identical |
| 10⁹ | 2, 3, 4 | `DIST_LOGN_TEST=25`: the A_h μ product 2 × 2 with one piece skipped by the low cut, X Q 2 × 2 in basis w | identical |
| 10⁹ | 1 | (the single-node flow, untouched by A5) | identical |

`t_mn_grid 0.5 28` at 2, 3 and 4 node-processes: VERIFY OK, 200 checks per node (was 120: the cut cases and the held product).
The sharded division's low product no longer re-shards X Q into basis w (one `mdb_shift` of the whole low half less: 51 shifts
per run instead of 52 at 10⁹).

##### A1 (batches 2 and 4: jobs 20766, 20774 on s24-30 / s24-16; 4 × 10¹⁰ size 1, `RNS_VERBOSE=1 ECALC_VERBOSE=2`, the reference evicted first)

The transform cache at size 1, the planes allocated once at the first grid product (the top tree level) and freed after the
division; digits identical in every run:

| run | cache | planes' allocation | bs (mdev top levels) | recip | division | dm | wall |
|---|---|---|---|---|---|---|---|
| base (batch 2, fused pointwise off) | off | — | 38.3 (13.9) | 16.9 | 17.7 | 34.8 | 86.0 |
| fuse (batch 2) | off | — | 38.1 (13.8) | 16.6 | 17.1 | 33.8 | 87.7 (init 15.7) |
| c0 (batch 4) | off | — | 36.7 (13.2) | 16.1 | 16.3 | 32.6 | **85.6** |
| c1 | 1 slot | 3.6 s | 40.2 (16.4) | 15.1 | 15.2 | 30.4 | 86.6 |
| c2 | 2 slots | 7.4 s | 43.1 (19.6) | 15.6 | 14.2 | 30.0 | 89.6 |
| c2b (repeat) | 2 slots | 9.3 s | 45.1 (21.4) | 15.1 | 14.1 | 29.3 | 89.8 |
| dm1 (2 slots, `DIST_LOGR_DELTA=-1`) | 2 | 8.8 s | 45.4 (22.0) | 16.8 | 15.3 | 32.2 | 94.4 |
| c3 from the block pool (batch 2, the first version) | 3 | pool fallback 3 × 64 GB | 135.3 | 26.2 | 27.2 | 57.4 | 208.1 |

Per product (the `dist_db` lines): the reciprocal's top step (3 × 1 pieces) 3.95 → 3.18 s, r d (1 × 2) 2.51 → 2.14, A_h μ
(2 × 3, 5 formed) 6.89 → 5.75, X Q (2 × 3, 5 formed) 7.23 → 5.70, the two level-24 products 2.51/3.00 → 2.12/2.66, level 25's
5 × 1 grids 1.70/1.65 → 1.43/1.39: **−4.1 s of products at 2 slots** (a piece product with a B hit: 1.11 → 0.72 s, load 0.33 →
0.17, ntt 0.67 → 0.44), 20 hits of 43 operand transforms in dm. But the two 16 GiB planes per APU cost **7.4–9.3 s to map**
(hipMalloc on the four APUs in parallel, 0.055 s/GB; one slot 3.6 s) — the same cost the block pool's fallback pays, and the
pool has no room for them at the products' peaks (peak live 121.5 of 141 GB; the first version's 3 × 64 GB from the pool
oversubscribed the node: 208 s) — so the wall loses by 1–4 s. Mapping them at init would move, not remove, the cost (RESULTS
§70: the mapping stalls running kernels too). **Not adopted at size 1: `RNS_DIST_CACHE` defaults to 0** (the code is the
measured form of A-div's B2 / PLAN I4; the hold of Q's pieces across the two products, `RNS_DIST_CACHE_HOLD=1`, adds nothing
over the in-product policy, see the design note). At 10⁹ (one plane per product) the cache is not entered.

Over shares (batch 3, 10⁹ over 2 node-processes with the grids forced by `DIST_LOGN_TEST=25`, `RNS_DIST_CACHE_MN`): dm 21.4 s
(cache 0) → 19.1 s (2 slots) → 18.4 s (4 slots); the division's products 9.7 → 7.9 → 6.8 s (a hit skips the operand's
all-to-all redistribution as well). The planes over gt nodes are 1/gt the size (8 GiB per APU per slot at 4 × 10¹⁰ over two
nodes, 2 s to map). **Adopted over shares: `RNS_DIST_CACHE_MN` defaults to 2**; the group agrees on the slot count (the minimum of
what every node's free memory allows). 4 × 10¹⁰ over two real nodes was not run (the 1 GbE fabric: hours).

Size-1 correctness of the cache (batch 2/4, all identical): 10⁹ with `DIST_LOGN_TEST=25` (grids of 2 × 1 .. 3 × 4 pieces, 28 hits)
at 1, 2 and 4 slots; the hold (`RNS_DIST_CACHE_HOLD=1`); `LIMB_BASE=2`; `t_dbig 24 big` with and without the cache (batch 5).

##### A6 (batch 2: `t_dist`; the 4 × 10¹⁰ runs above)

`./tests/t_dist 24`, `DIST_TINV=1 ./tests/t_dist 24`, `DIST_XGMI=1 ./tests/t_dist 31` (+ `DIST_STATS=1`): VERIFY OK (67 checks);
xGMI 2³¹: fwd 0.0573 s, fwd+fwd+pw+inv 0.1750 s (t_dist itself calls `dist_pw` + `dist_inv`: unchanged by construction).
The fused inverse in the products: a 2³¹ piece product's ntt 0.73 → 0.67 s (base → fuse, both cache off), dm 34.8 → 33.8 s
(−1.0 s; wall within the run-to-run spread). `DIST_LOGR_DELTA=-1` (rows 2¹⁷ on body 1, columns 2¹⁴): ntt 0.65 but load 0.54
(the gather's column runs are shorter) — total 1.33 vs 1.11 s per plane: worse, not adopted (the split stays logn/2).

##### The final tree with its defaults (batch 5, job 20783 on s24-16, shared with another user's job)

`t_dbig 24 big` VERIFY OK (609 checks); `t_mn_grid 0.5 28` at 2 and 4 processes 200 checks each; size 1 10⁹ decimal (8.2 s) and
`LIMB_BASE=2` (13.8 s) identical; 10⁸ at sizes 2, 3, 4, 10⁹ at sizes 2, 4 and size 2 with the grids forced (cache 2 over
shares: 5 hits) identical, every node VERIFY OK; **4 × 10¹⁰ size 1: 84.8 s, dm 32.6 (recip 16.0 + division 16.5), bs 36.8, init
15.3, VERIFY OK, digits identical** (RESULTS §74: 86.4 ± 1.3 with dm 32.9). `RNS_DIST_CACHE=2 ./tests/t_dbig 24 big` was killed
on that shared node at its 2³¹ × 2³⁰ product (the two 16 GiB planes per APU on top of the test's 100 GB of host and device
operands and the other job): the guard is `hipMemGetInfo`, which on the APU does not see the host's share of the memory.

#### Open issues

* The cache at size 1 pays only if its planes are already mapped: 32 GiB per APU of mapped, idle device memory during the
  top levels and dm does not exist today (the plane pools are in use by the product, the block pool is at its peak); a plane
  pool sized for it at init (M's `rns_init`) would cost the same mapping there. If B3 (the 3·2³⁰ planes, +34 GiB per APU)
  is ever adopted, the cache could share that memory when the product is not radix-3.
* The cache over shares was measured at 10⁹ over node-processes on one node (grids forced); at 4 × 10¹⁰ over two real nodes
  the 8 GiB per APU per slot (2 s to map) against the skipped redistributions of 2^28-limb pieces is the expected win.
* `t_dbig 0 big` with the single-node cache on (2 × 16 GiB per APU) is killed at its 2³¹ × 2³⁰ product on a node shared with
  another job; the free-memory guard (`hipMemGetInfo` minus `RNS_DIST_CACHE_MARGIN_GB`) does not see host allocations on the
  APU. Off by default at size 1, so only `RNS_DIST_CACHE=n` runs are exposed.
* Body 1 never reaches the dist tier's local passes (2¹⁵/2¹⁶ points: no 7-stage pass); a split with a 2¹⁷ side loses more in
  the gather than it gains. A b16 kernel variant for 6-stage passes is N-kernel's territory.

## G12 — the top product at scale: the tree's levels as piece grids for any g, the exact spills, the k-way schedule, the per-node profile independent of g (Phase 12, PLAN.md §27 row G; DECISIONS2 §2)

(former file: G12.md)

Branch `g12` (from `main` @ 7aded87). Files owned and touched: `ecalc/rns_dist.c`, `ecalc/mdb.h`, `ecalc/mn.c` (`mn_tree` and its
helpers: `tree_level`, new `tree_level_k`, `mn_group_span`; the M1 mesh creation, `mn_group_at`, `sub_mesh`, the topo layer
untouched), `ecalc/mn.h` (one declaration), `ecalc/mem_model.py` (the tree/spill g-terms; see §4 for how it sits on Q's file).
Touched outside, minimal and commented "Phase 12 G": `ecalc/dbig.c` (the sparse spill decode reads the compact form: a
`tab[4]` field, 2 lines in `sparse_get`, one exported function `db_share_add_spills_x`), `ecalc/dbig.h` (its declaration),
`ecalc/binsplit.c` (the body of `tree_need_dev` — the arena request for the tree at size > 1 — now the schedule's levels with
`rns_mul_dist_mn_scratch`; two forward declarations). Clone `~/ntt-g12` on aac6; batches `~/g_b1.sh`, `~/g_b2.sh`; logs
`~/g12/out1/`, `~/g12/out2/`, summaries `~/g12/b1.out`, `~/g12/b2.out`.

#### 0. What was actually the case at 7aded87 (the accounting vs the code)

`rns_mul_dist_mn` already grids over the shares at the group's cap (`mn_grid`, `mn_logn_cap` = 2^(min(31, POOL_LOG) +
⌊log₂ g⌋) points: one plane per node pool, q ≤ 2^29 limbs per rank whatever g and n — A-grid/L). So the plane pools never
grew in the code; M11's "214–856 GB of planes at 576" was the *model's* reading of `binsplit.c`'s `tree_need_dev`, which
sized the arena request at init as ONE uncapped transform of the level (q = n / 4g) plus **two g × C × 4-limb spill
buffers** — and that request is real memory: at 576 × 4 × 10¹⁰ it asks ≈ 700 GB per node (Q's finding 1), so the run
would fail at init. What did scale with g in the code itself was one item: the spill all-gather in `mn_core` (`spill_rb`
= g × 4 C limbs per APU per piece: 19 GB per APU, 77 GB per node at 576 nodes and C = 2^20). And `MN_GROUPS` was parsed
but `mn_tree` walked binary levels (Q's finding 2). Those three are what this branch changes.

#### 1. The exact spill exchange (`rns_dist.c` `mn_core`, `dbig.c` `sparse_get`)

Rank ρ's CRT stripe j spills 4 limbs into the piece's limbs [R j + a_ρ, +4), a_ρ = the first row of rank ρ + 1 (the
block-cyclic map; the last rank's into R (j + 1)). For a destination node r with window [lo, hi) of the piece (its share
of C cut to the piece), the columns whose block meets the window form one contiguous range [j0, j1) (`spill_cols`), so:

* send: `comm_alltoallv` over mesh d straight from `v->spill` (the [j][4] array of my rank), counts 4 (j1 − j0) × 8 B
  and displacements 4 j0 × 8 B per node — adjacent windows share at most one block (sent to both; each side adds only the
  limbs inside its own window, the carry between them goes through the node scan as before);
* receive: per source rank (r, d) the blocks of the columns that meet MY window, back to back in node order, plus a
  table (j0, j1, block offset) per source node; total ≈ 4 C + 4 g limbs per APU **whatever g** (32 MB at C = 2^20);
* the add: `db_share_add_spills_x` — `sparse_get` decodes global limb m → (ρ, j, t) as before and looks the block up
  through the table (the old [r][j][4] layout stays for `tab = 0`).

Both sides compute the ranges from the same windows (`piece_window`), so the counts match by construction (every
transport aborts on a mismatch). The direct (one-plane, M3) path and the accumulating pieces both use it; the digits are
identical in every test below (the spill values and their positions are unchanged, only the transport is).

#### 2. The tree on the `MN_GROUPS` schedule with k-way combines (`mn.c`)

`mn_tree` now walks `mn_groups_parse`'s levels: level l has groups [k G_l, min((k + 1) G_l, size)) whose children are the
previous level's groups (G_0 = 1). Two children → `tree_level` (the pair of products as before). k > 2 children →
`tree_level_k`: Horner from the top child, (P, Q) ← (P_i Q + P, Q_i Q) for i = k − 2 … 0 — k − 1 combines of two
products over the level's group, every product balanced over all its nodes by L's map, the operands sharded over the
children's subgroups or the group (what `rns_mul_dist_mn` accepts), P added as the CRT's X on one plane or by
`mdb_add_shifted` on a grid; a child's shares are freed right after its combine, so a node holds at most its child's
pair, the running pair and the new pair (O(share)). For k = 2 the products and their order are `tree_level`'s.

The groups: `mn_group_span(l, g0, g)` returns `mn_group_at`'s group when [g0, g0 + g) is a binary-level group (g a power
of two and aligned, or the whole machine) — the default at power-of-two sizes runs exactly as before, same meshes, same
slots — and otherwise creates the group's four meshes: SHMEM PE sets with ids of their own (NA + NA (2 MN_MAXL + l) + d),
TCP meshes on the level's odd slot 2l − 1 at the port lanes 4..7 (the binary group of the level uses lanes 0..3, the topo
layer the even slots; a first version used the slot 2 MN_MAXL + l, whose ports fell into the ephemeral range on aac6:
"cannot connect" at size 9 in batch 2). No tr / topo layer: every node transforms on the general map. Destroyed in
`groups_finalize`.

**The default schedule (Q's decision, results/TIME_MODEL.md §Q §1: the 3·3 top):** the powers of two that divide the size, then the
odd part's prime factors ascending — 576 → 2, 4, …, 64, **192, 576**; 9 → 3, 9; 6 → 2, 6; 12 → 2, 4, 12; a power of two →
the binary tree as before. Every group is exact (no cut top group) and every level is a k-way step by one prime factor.
L's "powers of two then size" is `MN_GROUPS=2,4,8,16,32,64,128,256,512,576`; the 9-way top is `…,64,576`. The size-1 path,
sizes 2 and 4, and the reciprocal's / division's groups (`mn_group_at(L)` = [0, size), the X1 prefix groups) are untouched.

##### 2b. The bug the forced tree grids found (pre-existing since Phase 9 A3 / Phase 11 B7, fixed here)

With the tree's levels gridded, 10⁹ at size 4 gave a wrong P (Q right) on the level-1 group [2, 4) — on `main`'s binary too
(`DIST_LOGN_TEST=23`, batch 8). A new probe found the level (`ECALC_RES_LOG=1`: after every tree level each node's shares of
P, Q are reduced mod the 8 T1 primes, all-gathered over the group, combined as Σ share_r B^lo_r and compared with
`vf_pq_range_mod` over the group's terms — `RES node r tree level l [g0, g0+g) P/Q vs recurrence [a, b): q0 ok … (agrees)` or
`TREE LEVEL MISMATCH`; `tree_check` in mn.c, kept), and a new `t_mn_grid` pass over every node's level-1 group (a group with
g0 > 0: the product, + X, `mdb_add_shifted`) reproduced it in seconds: **`mdb_add_shifted` sized its rounds by the windows of
nodes 0 … g − 1 instead of g0 … g0 + g − 1** (`add_window(C, r, …)` for `G->g0 + r`), so on a group that does not start at
node 0 every window was empty, 0 rounds ran and X was never added. It could only show on a gridded product with X on a
lower level's group (the division's groups start at 0; the tree's P at a lower level was never gridded in any test
before), i.e. exactly the 576-node top levels this branch is for. Fixed (commit 7d2b2c9); the gate below runs the tree's
levels gridded at every size with the probe on.

**Could it have affected an earlier result?** No. `mdb_add_shifted` has exactly one caller in the tree:
`mn_grid`'s `if (X) mdb_add_shifted(&Cn, X, 0, G)` — the added operand of a product that ran as a **grid**. Before this
session (a) the tree's levels never gridded at any tested size (the M3 path carried X in the CRT), and (b) the only
other products over shares are the reciprocal's and the division's, which pass X = 0. The groups with g₀ > 0 that
existed — the tree's lower levels and X1's prefix groups (`mn_group_at(L)` on node 0 is [0, 2^L): X1's target group is
always named [0, 2^L), results/TIME_MODEL.md §X) — were therefore never reached by this line with a non-empty X. The bug was
reachable only by the path this branch adds (a gridded tree level over a group that does not start at node 0, i.e.
the 576-node tree below the top level), and it aborts loudly there (a wrong P, caught by every digit check).

#### 3. The decision rule and the arena request

A level whose product fits one plane per prime at the group's cap runs the M3 path (X in the CRT, the rows straight
into the zero-filled shares) — bit for bit as before; above it the grid (pieces at the cap, the window temporary, the
shifted add, X by `mdb_add_shifted`). `DIST_LOGN_TEST=<log n>` lowers the cap (2^(log n + ⌊log₂ g⌋)) so the grid runs
at 10⁸–10¹⁰ on one node — the gate's runs below; `MN_TREE_LOGN_TEST=<log n>` (new) forces the cap for the tree's levels
only (`rns_dist_cap_test`, the transform cache released at the change), because the division under `DIST_LOGN_TEST=27`
at 10¹⁰/4 fails on main's binary too (§5, batch 3: "65 corrections" — the sharded division's grid at a 2^29 cap over 4
nodes, X's/A-div's code, pre-existing). The plane pools stay at their init size (POOL_LOG) in every run: the
`mem_report` "planes" line is the same at the tree as at init (34.4 GB per process at POOL_LOG=29, table §5).

**The arena request** (`binsplit.c` `tree_need_dev`, the one change outside my files that is not a consumer — this is Q's
finding 1: at 7aded87 the function sized the tree's arena as one uncapped transform, q = n / 4g, with two g × C × 4-limb
spill buffers, ≈ 700 GB per node at 576 × 4 × 10¹⁰; its body is replaced, commit 79a2b48, the signature kept): per
schedule level, the largest Horner product P_0 × Q_run (a leaf share × (k − 1) leaf shares of the children's size) costed
by the new `rns_mul_dist_mn_scratch(na, nb, has_x, g, share_a, share_b, share_c, &pieces)` in `rns_dist.c` — the grid's
largest piece at the group's cap and `mn_core`'s buffers: the received sequences rbA, rbB (and rbX on one plane) ≤ my
rows × C, the packed part sb (≈ a quarter of my part of the piece), cx and tmp (q; tmp only on the equal-part path), then
rbO (my window's part), the spills (4 C + 4 g limbs) and T (the window's quarter, ≤ share_c) — O(share) + O(q), q ≤
2^(min(31, POOL_LOG) − 2): no g-term. The live shares: the child's pair, the running pair (k > 2), the new pair, + 1/8.
`ECALC_VERBOSE=2` prints one `bs: tree layout:` line per level (product, pieces, scratch and live per device).

#### 4. The memory model (`mem_model.py`) — the g-terms

My `mem_model.py` is agent Q's file (their worktree at 2026-09-21 18:57: forms, `level_children`, SHMEM staging,
`estimate` hooks) with three changes, all in the tree/spill terms: (i) `tree_need_dev(form='grid')`'s body replaced by
the exact Python port of `rns_mul_dist_mn_scratch` (`mn_scratch`, with `split_grid_cap`) over `level_children`, the Horner
product and the live shares as in §3 — the C function and the Python one are the same formula line by line; (ii) the
default `form` is `'grid'` (the code after this branch; `'flat'` = the request at 7aded87, kept for the before/after);
(iii) `mn_groups`' default schedule = §2's. Q's `MEASURED` table and size-1 calibration are unchanged (`python3
mem_model.py`: 4 / 7 / 8 × 10¹⁰ within 1 %). The integrator should take Q's file and these three functions.

**Modelled per node at g = 576 (GB; `transport='tcp'` — the SHMEM staging term is Q's/S's item, not the tree's):**

| D per node | form | planes | arena (dm need, tree need) | top scratch | exchange | host | node peak |
|---|---|---:|---:|---:|---:|---:|---:|
| 4 × 10¹⁰ | flat (7aded87) | 428 | 709 (688, 709) | 556 | 36 | 18 | **1170** |
| 4 × 10¹⁰ | grid, default 3·3 | 121 | 180 (180, 164) | 47 | 36 | 18 | **354** |
| 4 × 10¹⁰ | grid, `…,512,576` | 121 | 196 (196, 185) | 63 | 36 | 18 | 370 |
| 8 × 10¹⁰ | flat | 856 | 1417 (1360, 1417) | 1112 | 71 | 18 | **2305** |
| 8 × 10¹⁰ | grid, default | 121 | 333 (333, 317) | 85 | 71 | 18 | **543** |
| 8 × 10¹⁰ | grid, `…,512,576` | 121 | 375 (375, 371) | 127 | 71 | 18 | 585 |

The ceiling per node (502 GB, grid, tcp): **7.1 × 10¹⁰ at g = 576** (6.8 × 10¹⁰ at g = 4 and 64: the cap at a power of
two makes the top piece one plane per prime over the group, 15 GB more of scratch), against 1.9 × 10¹⁰ flat — i.e.
**576 × 7.1 × 10¹⁰ ≈ 4.1 × 10¹³ digits** on the profile alone (X's/M11's "shares' line" was 7.7–8 × 10¹⁰: what remains
above the single-node profile is the division's exchange scratch (71 GB, L's `mdb_shift` buffers) and the window
temporary T of the accumulating pieces (≤ a share), see the open issues). With Q's SHMEM staging term the tables in
`python3 mem_model.py` are lower; that term is S's form to settle.

**Against `mem_report` (the gate's check):** 10¹⁰ at size 4 (POOL_LOG=29, per process): modelled tree need 14.6 GB
(top scratch 8.7), arena = max(dm need, tree) 19.9 GB, planes 36.5 — measured: block pool borrowed **19.9 GB** (the arena,
zero hipMalloc), planes 34.4, the tree's **peak live 9.2–10.7 GB** per process (was 11.0 in M11 with the g × 4 C spills:
−0.3…−1.8 GB at g = 4 — at g = 576 the same term is −77 GB per node), device 56.6–56.7 GB at the tree (M11: 56.8 at the dm
peak). The tree need is an upper bound (+40 % over the peak live: the buffers of one piece are not all alive at once —
sb is freed before rbO exists; the bound is what the arena must hold). 10⁹ at size 9: see §5 (batch 2).

#### 5. Tests (aac6, one node per batch, the node-processes sharing it over loopback TCP — correctness only)

Every run: `VERIFY OK` on every node, the part files concatenated and `cmp`-identical to `~/ntt/ecalc/ref/e_<digits>.txt`
/ `~/ntt/ecalc/results/e_1e10.out`. `SLURM_JOB_ID=$J ./mnrun.sh <p> env POOL_LOG=<pl> ECALC_VERBOSE=2 RNS_VERBOSE=1
ECALC_RES_LOG=1 <env> ./ecalc <d> <out>` (the per-level probe on in every ecalc run of the gate).

**The gate (jobs 20915 and 20933, aac6-spx-c / s24-26; scripts `~/g_gate.sh`, `~/g_gate2.sh`, logs `~/g12/gate*/`,
summaries `~/g12/gate.out`, `~/g12/gate2.out`; binary 7d2b2c9 / f65449e):**

| test | env | result | wall |
|---|---|---|---|
| `t_mn_grid 0.5 27` at 2, 3, 4, 6 processes; `0.25 27` at 9 (the grid forced at 2^24 by the test; 5 shapes × 2 generators: products, + X, low products, the two cuts, the held product, views, `mdb_add_shifted`; **plus the g₀ > 0 pass**: every node's level-1 group [2k, 2k+2), 3 shapes × 2 generators of product / + X / two shifted adds) | | VERIFY OK on every node: **200 / 200 / 224 / 224 / 224 checks** (the 24 new ones at sizes ≥ 4) | 1.5 / 1.5 / 2.0 / 8.5 / 9.1 min |
| 10⁸ size 3 | `POOL_LOG=27 MN_TREE_LOGN_TEST=20 DIST_LOGN_TEST=20` (tree and division gridded; schedule 3) | identical, 3 nodes VERIFY OK, 0 level mismatches; tree grids 2 × 2, 2 × 4 | 12.4 s |
| 10⁸ size 6 | the same + `MN_MODEL_TCP=1` (schedule 2, 6: a 3-way top level) | identical, 0 mismatches | 62.0 |
| 10⁸ size 9 | the same (schedule 3, 9: two 3-way levels) | identical, 0 mismatches | 60.2 |
| 10⁹ size 2 | `POOL_LOG=29 MN_TREE_LOGN_TEST=23 DIST_LOGN_TEST=24` | identical, 0 mismatches; tree grids 3 × 4 | 43.4 |
| 10⁹ size 3 | the same (schedule 3) | identical, 0 mismatches; 2 × 3, 2 × 5 | 39.4 |
| 10⁹ size 4 | the same (schedule 2, 4) | identical, 0 mismatches; 1 × 4, 2 × 2 | 27.0 |
| 10⁹ size 9 | `POOL_LOG=28 MN_TREE_LOGN_TEST=22 MEM_REPORT_DEVS=1` (schedule 3, 9) | identical, 10 × VERIFY OK, 0 mismatches; the tree's device 21.9 GB per process (planes 17.2, pool 0.8 borrowed, **hipMalloc 0**, peak live 0.3) against the model's tree need 0.6 GB and arena 0.8 GB | 124.6 |
| **10¹⁰ size 4** | `POOL_LOG=29 MEM_REPORT_DEVS=1 MN_TREE_LOGN_TEST=27` — **the grid forced**: level 1 one 2^28 plane, level 2 **1 × 2 pieces** at the 2^29 cap (the accumulating piece, the shifted add, the exact spills) | **identical**, 4 × VERIFY OK, 0 level mismatches | **123.3 s** (bs 50.1, dm 61.9) |
| **10¹⁰ size 4** | the same, not forced (level 1 one 2^30 plane, level 2 one 2^31: the M3 path) | **identical**, 4 × VERIFY OK | **124.3 s** (bs 54.8, dm 59.5) |
| 10⁹ size 1 | (the untouched path) | identical, VERIFY OK | 15.9 |

**The tree's device peak per process at 10¹⁰ / 4** (`mem_report` at the `tree` boundary, four APUs of one process):
device **56.7 GB** in both runs = planes 34.4 + block pool 19.9 (all borrowed from the arena, **hipMalloc 0**) + tables
2.5; the pool's **peak live 7.0–8.2 GB** with the grid forced and **9.2–10.7 GB** without (M11's figure with the
g × 4 C spill all-gather: 11.0). The model: `mem_per_node(2.5e9, 4, form='grid', pool_log=29)` gives tree need 14.6 GB
and arena 19.9 GB per process, planes 36.5, device at the dm peak 58.0 — the arena to the 0.1 GB, the tree need a
bound 40–100 % above the peak live (the buffers of one piece are not all alive at once: sb is freed before rbO
exists), which is what the arena must hold.

**Earlier batches (jobs 20878, 20890, 20905; `~/g12/out{1,...,11}/`)** — every ecalc run identical, all nodes VERIFY OK
unless noted: 10⁸ at sizes 3, 6, 9 with `DIST_LOGN_TEST=20`; 10⁸ at 3, 4, 6, 9 with `MN_GROUPS=3` / `4` / `3,6` / `9` /
`3,9` / `6` (the k-way combines: a 9-way step is 8 combines over 9 nodes, 79.9 s; the 3·3 schedule 56.1 s); 10⁹ at 2, 3,
4 with `DIST_LOGN_TEST=24`; 10¹⁰ at size 4 not forced, 92.6 s on s24-16 (L: 99.3, M11: 116.5, X: 100–123) and 97.4 s
with `DIST_LOGN_TEST=28`; 10⁹ size 1 identical in every batch. Two failures in those batches were not the tree:
(i) `DIST_LOGN_TEST` at 10¹⁰/4 and 10⁹/4 (cap 2^27 / 2^23) fails in the **sharded division** with "65 corrections" on
`main`'s binary as well (batch 3 and 8: pre-existing, X's / A-div's file — the reason `MN_TREE_LOGN_TEST` exists);
(ii) the schedule groups' first port slot collided with the ephemeral range (fixed, §2).

#### 6. Open issues

* **The window temporary T** of an accumulating piece is up to this node's share of C (`mn_core`: the piece's rows land
  in T, then C += T << off): at 8 × 10¹⁰ per node it is 35 GB per node beside the pair being formed. A rounds form (the
  result exchange in chunks of the window, as `mdb_add_shifted` does, with the spills of the chunk) would bound it to
  a constant; it is the largest O(share) term of the tree's scratch after the packed part sb (share / 4 per APU).
* The transform cache over shares (`RNS_DIST_CACHE_MN`, 2 slots) allocates 16 GiB per slot per APU by `hipMalloc` (the
  largest plane) when `hipMemGetInfo` allows, not from the block pool and not sized to the group's q: "where the block
  pool has room" is not what the code does; it is constant in g. Left as is (G.md's measured form).
* The exchange scratch of the division's shifts (`mdb_shift`, L's B7: 2 × share/4 per APU) is the remaining term above
  the single-node profile at 576 (71 GB per node at 8 × 10¹⁰); X's/L's file.
* A k-way level is Horner (2 (k − 1) products, the running operand growing); Q notes a pairwise combine inside the
  level would cost ≈ 25 % fewer plane points at a 9-way step — the default 3·3 schedule has no step above 3-way.
* The tree checkpoint sets (M6) are indexed by the schedule's level number: a restart must use the same `MN_GROUPS`.
* The reciprocal over 6 nodes at a lowered cap takes 365–454 s of which ≈ 99 % is one wait (every node reports the same
  figure; `MN_MODEL_TCP=1` or `NEWTON_MN_GROUPS=0` bring it to 34–46 s): X1's group choice with the target's constants
  on a 6-node group over loopback TCP. Not the tree (the same sizes with the schedule's own groups run in 33–62 s);
  X's file.
* `DIST_LOGN_TEST` below the division's own need ("65 corrections") is pre-existing on `main` — the sharded division's
  grid at a forced small cap. The tree's gate uses `MN_TREE_LOGN_TEST` instead; the division's forced-grid path at
  10⁹–10¹⁰ still works at `DIST_LOGN_TEST=24` (the gate's 10⁹ runs) but not at 23 or below over 4 nodes.

## B7ACCT — counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06)

(former file: B7ACCT.md)

Times are Eastern (aac7 logs are Pacific, so add 3 h). Numbers are labelled **(m)** measured, **(mod)** modelled or **(a)** assumed. A
C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, made on the aac7 login node with no HIP calls, is a *measured* layout figure:
it is the code's sizing, not a memory reading.
Login-node work only (aac7 `aac7-login`, `~/b7acct`: `ntt` = this branch, `base` = main 9e26898; outputs in `~/b7acct/out_*`,
`sweep/`, `below/`). No compute-node work, no jobs.

#### RESUME (2026-10-06 18:10 EDT): complete

- Branch `b7-vslot`: c7664df (the v-slot accounting), aa86cac (task 2 + README), then this file. It is pushed to origin and not
  merged.
- Nothing is armed and nothing is running. To resume, the next step is the hardware check in §8, item 1. It needs compute nodes.

#### 1. Which exchanges take v-slots, and how long they live

- **The only v-slot site is `comm_layered.c` `y_alltoallv` / `y_alltoallv2`.** It is reached only through the layered communicator
  `G->lay[d]` (`rns_dist.c:1156` `lay_get`). The only callers of `comm_alltoallv` on `cl` are the general four-step:
  - `gen_fwd` (`rns_dist.c:1427`): one post per chunk k < K.
  - `gen_inv_pw` (`rns_dist.c:1440`): the same.
  - Both run only when `gen = !is_pow2(g) || DIST_GEN` (`rns_dist.c:1590`).
- **The other `comm_alltoallv` calls use the plain mesh `G->all[d]`, not the layered comm.** They take no v-slot. Their staging is
  in the comm_ofi pools, which OFIMEM already counts. The calls are:
  - `redistribute` (`rns_dist.c:1202`);
  - the result exchange (`:1751`) and the spills (`:1770`);
  - `mdb_shift` / `mdb_add_shifted` (`:2290`, `:2357`);
  - `newton_db.c:635`.
- **Power-of-two groups also get a layered comm, but only for the equal-slab path.** That path's scratch is the caller's `tmp`,
  which comes from the block pool.
- **The exchange sizes, per APU thread** (layered rank ρ = g·d + r, nr = 4g, rk(ρ,k) = rank ρ's rows of chunk k, cols(ρ) = its
  columns, with `partn` for both):
  - forward: A = Σ_dd rk(g·dd + r, k) · Σ_r' cols(g·d + r'), and B = cols(ρ) · Σ_σ rk(σ, k);
  - inverse: A = Σ_dd cols(g·dd + r) · Σ_r' rk(g·d + r', k), and B = rk(ρ, k) · C;
  - all in points × 8 B.
  - A is the intra-stage receive, `vtab_build`'s Σ rI. B is the inter-stage receive, Σ rcnt.
  - The send slabs are back to back in rank order (`fsd`/`frd`), so `contig = 1` and x0n = 0. `need_vtmp` is never taken at depth 2.
- **The slot formula:**
  - `COMM_ALLTOALLV_DEPTH=2` (the default): two slots of A + max(A, B) + 4 bytes (`need_vslot`, `comm_layered.c:467-474`).
  - Depth 1: one area of 2A + B + 4 (`need_vtmp`).
  - With A ≈ B ≈ q/K, two slots hold 4q/K points, which is **q × 8 bytes at K = 4**. That is one full plane per APU.
- **The plane is the grid's largest piece**, as in `rns_mul_dist_mn_scratch` (P24's branch included). At 10 nodes, at the cap, the
  plane is 2¹⁷ × 2¹⁷ over 40 ranks: q = 3277 × 131072 points, so **3.444 GB per APU (mod, exact formula)**.
- **The slots never shrink.** `need_vslot` only grows (hipFree, then hipMalloc). The comm is destroyed only in `groups_finalize`
  (`mn.c:556-573`), at the end of the run. So each general-map group keeps the largest slot of all its products from its first
  general product to the end.
- **One group per node can hold v-slots at each level** (`mn_group_span` / `mn_group_at`). The whole machine's group is the tree's
  top level, and the reciprocal's sharded steps and the division run on the same object, so they share one communicator.
  - At 576 nodes with `MN_GROUPS=…,64,192,576`, the general-map groups are 192 (`g_sched`, level 7) and 576 (`mn_group_at(10)`).
  - The reciprocal's early doublings run on `mn_group_at(L)`, which is a power of two (`x1_group`, `newton_db.c:837`). They take no
    v-slots.
- **Resident v-slot bytes per APU over the run:**

| point of the run | resident v-slots per APU |
|---|---|
| tree levels with power-of-two groups | 0 |
| a general-map tree level l | Σ over the general-map levels ≤ l of slot(that level's largest product, P₀ × Q_run) |
| the tree's top level (the whole machine) | Σ of the lower levels + slot(top level) |
| reciprocal and division, to the end | Σ of the lower levels + max(slot(top level), slot(A_h·mu, n_Q × n_Q), slot(Q_t·r, n_Q × n_Q/2)) |

#### 2. The code

- **`rns_dist.c:2148` `vslot_shape` and `:2173` `rns_mul_dist_mn_vslot(na, nb, g)`:** one product's v-slots per APU thread.
  - It returns the max over the group's APU threads, with each area rounded up to whole 2 MiB, which is conservative.
  - It returns 0 for a power-of-two g without `DIST_GEN`.
  - It reads `COMM_ALLTOALLV_DEPTH` and `DIST_CHUNKS` the way the runtime does.
- **`binsplit.c:419` `binsplit_vslot_bytes(N, size, &tree_top, by, len)`:** the resident sum from §1.
  - It uses `mn_groups_parse` and `tree_need_dev`'s products.
  - A level whose last group is cut by the size counts the larger of the two shapes.
  - n_Q comes from `dm_nq_of`, which is dm_layout's formula without the room decision. That avoids recursion.
- **`binsplit_vslot_budget_on` / `_node`:** the switch **`ECALC_VSLOT_BUDGET`**, default 0.
- **Reporting (always on; no effect on the run):**
  - `BS_LAYOUT_ONLY` prints a new `vslot:` line after `planes:` (`binsplit.c:807`). It gives per_apu, tree_top, node_vslot (×4),
    device and device_with, node and node_with, and the per-level terms. device is the room's planes + arena_with_room + ofi_pool
    (CAP17/TGT17's definition); node is the room's node.
  - `MN_PLAN_ONLY` prints a new `plan vslot` line (`mn_plan.c:363`).
  - Every other layout and plan line is **byte-identical to main**. Checked: `diff` of the base and new outputs for 3, 6, 10 and 576
    nodes and the check set shows only the `META` date line differing.
- **The budget (only under `ECALC_VSLOT_BUDGET=1`):**
  - 4 × the v-slots are added to `binsplit_node_bytes` (`:743`). That feeds `ECALC_BUDGET_CHECK`'s modelled peak (`ecalc.c:367`
    also adds them to the device peak), the `RNS_DIST_CACHE_FIT` room and `ECALC_PLANE_CAP=fit`.
  - They are also added to `as_room_fits` (`:512`), the `BS_ARENA_ROOM` decision.
  - These can change the run's decisions (room, cache slots, plane cap, the budget stop), which is why the switch is off by default.
  - The arena request, the plane sizing (`rns_dist.c:394`), the piece plans and every allocation are unchanged either way. The slots
    are hipMalloc'd outside the arena, so putting them in `tree_need_dev` would have grown the arena and counted them twice.
- **`MEM_REPORT_DEVS=1` at size > 1:**
  - `ecalc.c:534` hands `mem_report_outside(layout v-slots per APU, COMM_OFI_POOL_MB, comm_layered_vbytes)` to `mem.c`.
  - Each per-APU line then adds: `| outside the layout: v-slots layout X GB, hipMalloc'd now Y; comm pool Z GB -> in use + v-slots now
    + comm pool W GB, driver - that (the runtime) R GB`.
  - `comm_layered_vbytes(dev)` (`comm_layered.c:365`) is a relaxed atomic counter. `need_vtmp` / `need_vslot` / `y_destroy` update it
    and nothing else reads it.
  - **Built only.** The new fields have not been run on a GPU node; see §8.
- **`mem_model.py`:**
  - `vslot_shape` / `mn_vslot` / `vslot_resident` / `vslot_budget_on` mirror the C.
  - `mem_per_node` now adds 4 × tree_top to `dev_bs` and 4 × per_apu to `dev_dm`. It returns `vslot`, `vslot_tree` and
    `vslot_levels`.
  - `ECALC_VSLOT_BUDGET` is mirrored in the room decision and in `layout_node`.
  - The old depth-2 term in `xchg` is removed. It put one quarter-plane per APU into the block pool; the v-slot term above replaces it.
  - The default `depth` is now 2, the code's default.
- **`--check-c` changes:**
  - It parses the `vslot:` line, searching up to the next point because `ECALC_VERBOSE=2` interleaves the tree lines.
  - It compares four rows: vslot per APU, tree top, device + vslots and node + vslots.
  - Fix: the check's room loop now passes `ECALC_NODE_GB` to `as_room_fits`, as the C does. It had used 480 always. Found by the
    budget-switch flip test below.
- README rows: `ECALC_VSLOT_BUDGET` (new) and `MEM_REPORT_DEVS` (extended).

#### 3. Tests (aac7 login node; `make SHMEM_CRAY=1 -s -j16` builds clean, including the tests, rc 0)

- **Commands.** All runs use the standard env: LINE10 without the logging switches, plus `COMM_OFI_PLAN_CXI=1`; target runs add
  TGT17's line.
  - Layouts: `BS_LAYOUT_ONLY=81000000000:{3,6,10} ./ecalc {243,486,810}000000000 /dev/null`.
  - Plans: `MN_PLAN_ONLY=…:{3,6,10}`.
  - Target: `BS_LAYOUT_ONLY=70833333333.33:576 ./ecalc 40800000000000`.
  - Check set: `BS_LAYOUT_ONLY=4e10,1e11,9.1597e10:576,9.1597e10:2,5e10:2`.
- **`mem_model.py --check-c <file> 31 1024 128 0.16 auto` is exact everywhere** (**0 terms not exact, largest arena difference
  0.0000 %**, 20 + 4 plane figures exact). That covers:
  - 4e10 and 1e11 (g 1); 9.1597e10 on 576 and on 2; 5e10 on 2;
  - the target share 7.0833e10 on 576 (4.08e13);
  - 8.1e10 on 3, 6 and 10.
  - The vslot rows match to the byte: e.g. 3,443,523,584 (10 nodes) and 6,706,692,096 (576).
- **The same with `ECALC_VSLOT_BUDGET=1`:** exact. A flip test at 9.1597e10:576 with `ECALC_NODE_GB=460`: the room fits without the
  switch (node 447.5) and is dropped with it (447.5 + 26.8 > 460). C and model agree in both cases.
- **The `plan cache` line under the switch:** at 10 nodes the node goes 414.66 → 428.44 GB; at the target, 381.37 → 408.20 GB. The
  cache decision itself does not change. Under the launch line's `RNS_DIST_CACHE_PARTIAL=1`, the slots come from the block pool's
  free bytes, not from the node room (the line says "nothing added").
- **Gates not run:** `t_newton`, `t_mul`, e9, 4e10 and `mnaccept` need compute nodes, which this task forbids. By default the runtime
  change is limited to (i) an atomic byte counter in comm_layered.c and (ii) extra printing under `MEM_REPORT_DEVS`. No buffer, size,
  plan or decision changes unless `ECALC_VSLOT_BUDGET=1`, so the digits are unaffected by construction. A GPU gate should still run
  before merging (§8).

#### 4. Predicted layouts at the standard config (8.1e10 per node; measured C layout figures)

The config is the launch line minus `MN_GROUPS`, with the default schedule, comm_ofi on and `DM_MN_LEAN` off, as LINE10. GB per node.
"Before" is main 9e26898, whose output is byte-identical to this branch's minus the new lines. "After" adds the v-slots.

| nodes, digits | schedule (general-map groups) | v-slots per APU | device before → after | node before → after |
|---|---|---|---|---|
| 3, 2.43e11 | 3 (top only) | 2.869 | 362.47 → **373.94** | 403.48 → **414.96** |
| 6, 4.86e11 | 2, 6 (top only) | 2.869 | 379.65 → **391.12** | 420.66 → **432.14** |
| 10, 8.1e11 | 2, 10 (top only) | 3.444 | 379.65 → **393.42** | 420.66 → **434.44** |

Notes on the table:
- device = planes 103.70 + arena_with_room (249.11 at 3 nodes, 266.29 at 6 and 10) + 4 comm_ofi pools 9.66.
- node = the room's node, which adds bs_grow 6.0 and host 44.68 to the device terms.
- The v-slot term is the same during the tree's top level and the dm phase at these sizes, because every product is at the cap.
- Plan checks: 80, 88 and 96 products, all OK.
- **Old vs new, STD17 10-node:**
  - measured node 4 × 100.9 + 19.9 = **423.5 GB (m)**;
  - old layout **420.66 GB (mod)**, 2.9 GB under the measurement, and only because the host term's 17.2 GB of seed buffers is freed
    after init;
  - new layout **434.44 GB (mod)**, 10.9 GB over the measurement, so conservative again.
- **Per APU, STD17 at [bs]:**
  - layout device + the bs growth = 94.91 + 1.50 = 96.41 GB;
  - plus the v-slots 3.44 → 99.86 GB;
  - plus the runtime (≈ 0.4–0.5 GB, measured at g = 2) → ≈ 100.3 GB;
  - against `driver used` **100.9 GB (m)**.
  - So the v-slots account for the 3.3–3.5 GB of B7 per APU, leaving ≈ 0.6 GB unexplained per APU (mod vs m).
  - The 3.444 GB/APU prediction matches B7's inferred 6.35 − 2.42 (pool) − ≈ 0.45 (runtime) ≈ 3.5 GB/APU.

#### 5. The 4.08e13 / 576 target with the real v-slot term (TGT17's launch line, `DM_MN_LEAN=1`)

- **v-slots: 6.707 GB per APU = 26.83 GB per node (mod, the C layout).** It is 2.873 from the 192-group level plus 3.834 from the
  576-group, where the tree top and the dm phase are the same at the cap. Both are resident from tree level 8 to the end.
  - That is below TGT17's assumed 30 GB.

| bar | before (TGT17) | after (B7ACCT) | margin |
|---|---|---|---|
| (a) device ≤ 373 − 20 = 353 GB | 345.29 (fits, +7.71) | **372.12** | **−19.12 GB: FAILS**; only **+0.88 GB** under the raw 373 GB `hipMalloc` edge (A6, m) |
| (b) node ≤ 480 GB | 387.37 + 30 (a) = 417.37 (+62.63) | **387.37 + 26.83 = 414.20** | **+65.80 GB: fits** |

**The real v-slot term breaks bar (a).** CAP17/TGT17 counted only planes + arena + ofi pools against the device edge. The v-slots are
`hipMalloc`'d device memory too, so they count against that edge. Options, all modelled from `vslot_resident` (device per node at
4.08e13; the slots' per-APU sizes are exact, but the performance of each option is unmeasured):

| option | v-slots per APU | device |
|---|---|---|
| today (depth 2, K = 4) | 6.707 | 372.12 |
| free each level's slots at the level's end (a code change: destroy or shrink `G->lay` after level 7) | 3.834 | 360.62 |
| `DIST_CHUNKS=8` (smaller chunk exchanges) | 3.364 | 358.74 |
| `DIST_CHUNKS=8` + freeing per level | 1.917 | **352.96** (just inside 353) |
| `COMM_ALLTOALLV_DEPTH=1 DIST_CHUNKS=8` | 2.521 | 355.37 |
| `DIST_CHUNKS=16` | 1.837 | **352.64** |
| a lower target tier: arena 210.45 from 3.85e13 (§6) | 6.707 | 367.82 |

These options go to the user as a decision. None is applied here.

#### 6. Task 2: stale sizes

**`e16_headline.sh`.** The per-node share is 7.0834e10: the target's largest node share is ceil(2,266,666,666,671 / 576) =
3,935,185,186 limbs = 70,833,333,348 digits, rounded up to 1e6. Re-measured with `MN_PLAN_ONLY` / `BS_LAYOUT_ONLY` on the login node,
using the script's LINE + `COMM_OFI_PLAN_CXI=1`. Pieces are given as node 0 / critical path.

| g | `E16_DIGITS` | products, pieces, levels | SHMEM pool | layout node → with v-slots | device → with v-slots | `E16_BELOW` | modelled walls, no write / write (`--write-bw 0.1`) |
|---|---|---|---|---|---|---|---|
| 10 | **708,340,000,000** | 96 products, 101 / 101 pieces, levels 8/8 20/20 | 512 MiB (+ 4 × 2304 comm pools) | 386.30 → 400.08 GB | 345.29 → 359.06 GB | **687,000,000,000**: step at 6.87 → 6.88e11 (97 → 101); node 377.71 | `--bw 25`: 212.0 / 500.7 s; `--bw 3.7`: 719.5 / 926.7 s |
| 12 | **850,008,000,000** | 100 products, 130 / 130 pieces, levels 8/8 8/8 24/24 | 512 MiB (+ 4 × 2304 comm pools) | 369.12 → 380.60 GB | 328.11 → 339.59 GB | **840,000,000,000** (128 / 128; the pieces are not monotone at 129–131 across 8.412–8.496e11, 133 at 8.52e11); same layout | `--bw 25`: 234.4 / 523.9 s; `--bw 3.7`: 821.7 / 1032.4 s |

The script's comments now carry these figures. The old values are kept as history in one line, and the timeout comment and the disk
estimate are rescaled. `bash -n` passes.

**`mem_model.py` `TARGET_BELOW` is now 3.99e13 (was 4.74e13).** Sweep on 576 with TGT17's line (MN_PLAN_ONLY + BS_LAYOUT_ONLY):

| total digits | pieces (node 0 / critical path) | products | arena with room |
|---|---|---|---|
| 4.05–4.08e13 | 119 / 133 | 1238 | 214.75 GB |
| 4.0–4.04e13 | 117 / 133 | 1238 | 214.75 GB |
| **3.96–3.997e13** | **115 / 131** | 1238 | 214.75 GB |
| 3.9–3.95e13 | 111 / 127 | 1236 | 214.75 GB |
| ≤ 3.85e13 | 109 / ≤ 123 | 1236 | **210.45 GB** |

- The nearest lower tier on the critical path is the step at 3.997 → 4.0e13. That puts TARGET_BELOW at **3.99e13**, with the same
  arena.
- `estimate.py --target` runs. Its rows:
  - 4.08e13: pieces 62 + 51 + 20 = 133, and 223.8 s without the write (mod, the default fabric);
  - 3.99e13: 60 + 51 + 20 = 131, and 218.2 s (mod);
  - 4.74e13 is kept as a history row.
- The relabelled row is in `estimate.py`, and `mn_model.py`'s comment is updated.

#### 7. Labels

- **measured:**
  - every `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` figure in §3–§6 (the code's own sizing);
  - STD17's driver, host and pool readings;
  - A6's 373 GB edge.
- **modelled:**
  - the v-slot term itself. It is the code's exact exchange arithmetic, not yet read back from a run.
  - the per-APU comparison in §4;
  - the §5 options;
  - the estimate.py walls.
- **assumed:**
  - the runtime ≈ 0.4–0.5 GB per APU (from STD17's g = 2 run);
  - 2 MiB `hipMalloc` granularity;
  - the 200 Gb/s and 3.7 GB/s fabric rates in the e16 walls.

#### 8. Open items

1. **Hardware check (needs compute nodes).**
   - Run the next standard 10-node run with `MEM_REPORT_DEVS=1` on this branch.
   - Expect `v-slots hipMalloc'd now` = 3.44 GB per APU after the tree's top level, and `driver - that` ≈ 0.4–0.6 GB.
   - Also run the GPU gates (t_newton, t_mul, e9, 4e10 identical) before merging.
2. **The target fails bar (a) with the real term:** 372.12 GB against 353, and 0.88 GB under the raw 373 GB edge. A decision is needed
   from §5's options: free the slots per level, `DIST_CHUNKS`, a lower target tier, or change the bar.
3. **`ECALC_VSLOT_BUDGET` stays off by default.** Turning it on changes no layout decision at the 3, 6, 10 or 576 sizes here: the room
   fits, the nodes stay under 480, and the `plan cache` (PARTIAL) form is unchanged. At runtime it adds 4 × v-slots to the budget
   check's modelled peak, and so to the `RNS_DIST_CACHE_FIT` room that check hands to `rns_dist_cache_budget`. Adopting it is the
   user's decision.
4. **Unchecked assumptions:**
   - The cut-group branch (MN_GROUPS steps such as 512 → 576 at size 576, or a binary clip at size 14) is not checked against a run.
   - The conservative 2 × max slot model is at most one chunk row × C × 8 B high per slot.
5. **`mem_model.py` changes affect the modelled node figures:**
   - `mem_per_node` now includes the v-slots. Example: `--p15`'s target row moved 411.8 → 438.6 GB, with dev_bs 367.4 → 394.2.
   - The default `depth` changed from 1 to 2.
   - Other notes that quote the old `mem_per_node` node figures are stale by about 4 × v-slots on the general-map runs.

## C06 -- G5 (X2 cut-group pool-offset asymmetry) and M3 (release the v-slots; S-1 remainder), 2026-10-11 (branch c06 = main bc80001)

(former file: C06.md)

#### Verdict
- **G5: it does not bite, on the target or on the 576-k schedules; no code fix, no guard.** (1) The target schedule has no cut group at all. (2) The 576-k schedules with an explicit `MN_GROUPS=2,..,64,192,<g>` do have cut groups, but the pool-offset asymmetry is not a correctness hazard: the receiver publishes where each sender's slab lands (comm_shmem.c `publish()` -> W_ROFF; the sender puts at `UNOFF(roff)`; the OFI path uses the same offsets), so the members' pool offsets need not agree. (3) Host emulation (more shapes than S45): data correct in every run (0 wrong words); offsets equal only for OFI pool + SHARE=1 (the target); the S45 "asym" flag measures a property the transport does not rely on. The real-transport confirmation (offsets forced to differ) is queued (item 70, gates g2 / g4).
- **M3: implemented, `COMM_LAYER_VSLOT_RELEASE=1` (default 0, needs X2's `COMM_LAYER_VSLOT_POOL=1`).** Plan (C code, aac7 login node, 576 nodes, 3.71e13): comm_ofi pool **4352 -> 2304 MiB per APU = -8.59 GB per node device** (layout node 386.48 -> 377.89 GB). At 3.82e13 with T2048+X2 the rank-0 margin 4.3 GB (B01, modelled) becomes about 12.9 GB. Host emulation correct; aac7 gates and a 4-node memory measurement are queued (not run yet: the aac7 queue was busy).

#### G5 detail
1. Schedules (`archive:results_raw_2026-10-19/C06/cut_groups.py`, exact arithmetic of `mn_groups_parse`): 576 (2,4,..,64,192,576): groups exact at every level, no cut group. Default schedules of 560..575 (prime factors) are exact too. B04's explicit `2,4,8,16,32,64,192,<g>` for g = 575..560: a cut group at every level where g mod level != 0 (575: 4:3 8:7 16:15 32:31 64:63 192:191, all non-power-of-two = layered); power-of-two cut groups (e.g. 2 at level 4 for 574) use the plain mesh. Every 576-k count has 2..6 layered cut groups.
2. Pool rule coverage (`c06_cov.sh`, MN_PLAN_ONLY on the aac7 login node, X2+SHARE+T2048, g = 560..575, `cov_check.py`): `COMM_LAYER_VSLOT_MB_<size>` is set for every layered size incl. every cut size, all equal under SHARE (915..967 MiB). No size is missing, so under SHARE every group allocates the pair once at the same size.
3. Why offsets need not agree: `publish(p, base, ..)` puts the receiver's own offset of its buffer into every sender's control block per exchange; `push_peers` waits for it and `put_signalled(.., UNOFF(x), ..)`. A sender never computes the receiver's offset. The slot is allocated before the receiver posts the exchange, so a peer cannot write earlier.
4. Host emulation (`ecalc/tests/lay_host`, now `--real 1`: only non-power-of-two group sizes run the layered communicator, as in the product; realistic SHMEM control size n (64 + 256 KiB) -- S45's 4 KiB + 64 B was hidden by the 256 B alignment; per-level symmetry check; hand-over fixed for nodes without a lower layered level; host placement blocks fixed). aac6 MI250, 3 nodes, 7 shapes x variants; `archive:results_raw_2026-10-19/C06/lay_host_c06_summary.txt`, `lay_host_runs_c06.txt`. All runs: 0 wrong words, 0 live bytes after destroy. Equal pool offsets in every member of every level: only {OFI pool emulation, X2, SHARE=1} with cut groups (23: 2,4,8,23; 47: 2,4,8,16,47 = the 575 shape; 35: 2,4,8,24,35; 34: 2,4,8,24,34) and the exact-group control (36). Unequal: OFI SHARE=0 (cut-group nodes hold a smaller slot), and the SHMEM-only pool (control blocks differ by group size) for SHARE 0 and 1. Without X2 (hipMalloc slots) no pool is involved.
5. `COMM_LAYER_TEST_SKEW_MB=n` (test only, default 0): the odd ranks take an extra pool block before the slots, so offsets differ between the members on a real run. Queue item 70: g4 (skew alone, baseline code) and g2 (skew + RELEASE), 2 nodes, 1e9, DIST_GEN=1, VERIFY OK + digits identical expected.
- Caveat: the "does not matter" claim rests on code reading + emulation until item 70 has run. A cut group + non-SHARE + X2 can still fragment the pool (a clean "pool cannot hold" fatal, not wrong digits); SHARE=1 is on the target line.

#### M3 detail
- What is left after VSLOT_SHARE: under X2 the pair (1.917 GB per APU at 576) is carved from the comm_ofi pool, which the pool rule sized as staging + slots (`2048 + 1830 + 256 -> 4352 MiB`). The staging (redistribution, result, shifted add; capped by ROUND_MB=1024 each side) and the slots are never live together: a product is redistribute (staged) -> v-exchanges (unstaged, from/into the pool) -> CRT -> result exchange (staged). Only the pool, fixed at init, holds both, so per-level release alone gains nothing (the dm phase is the peak and uses the slots); release **per product** lets the pool be sized for the larger of the two.
- Code: `comm_layered.c comm_layered_release_vslots(cl)` (frees the pair: SHARE pair or own pair; completes anything pending first; no-op without the switch), called from `rns_dist.c` once, after the prime loop of `rns_mul_dist_mn` (`if (gen)`), before the CRT and the result exchange; the next product's first v-exchange allocates the slots again at the pool rule's size (same size, top-down last fit). Pool law: `binsplit.c binsplit_shmem_pool_need` (OFI: `max(staging, slots)`; SHMEM-only: `max(4 staging, 4 slots)`) and `mem_model.py shmem_pool` (`vslot_release_on`). `comm.h` +1 declaration, README +2 rows, TASKS S-1 note. Digits cannot change (slots are scratch, written before read). Fatal if RELEASE without POOL.
- Measured by the C code (MN_PLAN_ONLY, aac7 login, target line + X2 + T2048, COMM_OFI_PLAN_CXI=1): 576 nodes: `comm_ofi pools` 4352 -> 2304 MiB; plan check OK both ways (1236 products). BS_LAYOUT_ONLY + `mem_model.py --check-c ... 31 2048`: exact both ways, node + vslots 400.14 -> 391.55 GB (model = C, +0.0000 %). 4 nodes x 6.441e10 with DIST_GEN=1: pool 4608 -> 2560 MiB per APU. Saving = min(staging, slots) per APU, so it exists wherever the general map runs under X2.
- Risks: every layered product must call the hook (only `rns_mul_dist_mn` uses `lay_get`: yes); at slot allocation nothing staged may be live beyond pool - slots (true in the code path: all staged exchanges are waited before the transforms; a violation is a clean pool-full fatal); alloc/free per product is host bookkeeping (cheap, ~1236 x few products). Time effect not measured.
- Host emulation with the release (`COMM_LAYER_VSLOT_RELEASE=1`, shapes 23/47/35/36): OFI + SHARE=1: VERIFY OK, 0 pool blocks live after each release. (SHARE=0 rows show leftover blocks only because the harness releases lay[L] before using lay[A] again.)

#### Queue items on aac7 (dry-run STUB=1 OK; in ~/aac7q/queue/; sources in archive:results_raw_2026-10-19/C06/queue/)
- `70_c06_gate.sh` (2 nodes, EST 27): five gates, 1e9 digits, aac7 line + DIST_GEN=1: g1 RELEASE, g2 RELEASE+SKEW, g0 control, g4 SKEW alone, g3 RELEASE without SHARE. Output ~/sess1011c/c06/G/summary.txt.
- `71_c06_mem4.sh` (4 nodes, EST 28): A = line + DIST_GEN=1, B = + RELEASE, 4 x 6.441e10 digits, MEM_REPORT_DEVS=1, COMM_OFI_VERBOSE=1 (pool peaks); `s36_devpeak.py` per-node device peak; fallback to 5e10 per node if A fails. Expectation (modelled): B -8.6 GB per node device, wall unchanged. Output ~/sess1011c/c06/M/summary.txt.
- Build: ~/ntt-wt/c06 (branch c06 c3c03b7+, make SHMEM_CRAY=1 on the login node).

#### Files
ecalc/comm_layered.c, comm.h, binsplit.c (pool law, 6 lines), rns_dist.c (1 line, a08's file), mem_model.py, README.md, TASKS.md; ecalc/tests/lay_host/{t_lay.cc,run_matrix.sh,ana.py,c06_all.sh}; archive:results_raw_2026-10-19/C06/*.

#### RESUME
1. When the aac7 queue has run items 70 / 71: `cat ~/sess1011c/c06/G/summary.txt ~/sess1011c/c06/M/summary.txt`. Expect 5 gates PASS; B devmax about 8.6 GB below A.
2. All pass -> the user decides adoption of `COMM_LAYER_VSLOT_RELEASE=1` for the target line (needs X2); then the launch line, kit and `plan pool` text (the `plan vslot` line still says "hipMalloc'd") get the switch.
3. Merge: tiny conflicts possible with b02 (deletions) in rns_dist.c / binsplit.c.

## S36 -- idea S-1: release (share) the v-slots between the tree levels (2026-10-10)

(former file: S36.md)

Status: code written and built on aac7 (build clean, make -B, rc 0); gates and the A/B are **measurement pending** (job 12455, below).

#### Finding that shapes the design (read from the code)
- Every general-map group (group size g not a power of two, or DIST_GEN=1) owns a layered communicator per APU thread (`rns_dist.c lay_get`); its two v-exchange slots (`comm_layered.c need_vslot`) grow to that group's largest product and **stay to the run's end** (`mn.c groups_finalize`). The levels' slots therefore add up (`binsplit_vslot_bytes`: `lower + max(top, dm)`).
- The tree runs level after level and a node is in one group per level, so a level's slots are dead when the next level's first exchange starts. Only the whole-machine group is used again (reciprocal / division), and only the non-power-of-two groups (`g_sched`, or `mn_group_at(L)` at a non-power-of-two size) own slots at all.
- With X2 (`COMM_LAYER_VSLOT_POOL=1`) the slots live in the comm pool, whose size the pool rule takes from `binsplit_vslot_bytes`; S31 showed that regrowing slots there (free then alloc, with staging blocks in between) fragments the first-fit pool (S28's 10-node failure). A free-and-regrow per level would bring that failure mode back, so S36 does not free per level.

#### Design: `COMM_LAYER_VSLOT_SHARE=1` (default 0)
- One pair of slots per device (`g_vsh[dev]` in `comm_layered.c`), shared by every layered communicator of that APU thread. `need_vslot_shared`: when the pair changes owner, the previous owner's pending v-exchange is completed first (`complete_v`), then the new owner uses the pair; it grows (free + alloc, as before) only if a level needs more than it holds. The pair is freed with the last communicator (`y_destroy`, reference count). Works with the pool (X2) and with hipMalloc (no X2); `vlive_add` accounting kept.
- Pool rule (`binsplit.c vslot_prealloc_env`): under SHARE every `COMM_LAYER_VSLOT_MB_<g>` is set to the overall per-slot maximum, so the pair is allocated once (no regrow, no fragmentation).
- Model: `binsplit_vslot_bytes` returns `max(largest lower level, top, dm)` instead of the sum (the pool rule, `ECALC_VSLOT_BUDGET`, `MEM_REPORT_DEVS` and `mn_plan` follow); `mem_model.py vslot_resident` the same (`vslot_share_on`, `summed=` keeps the old sum). README row added.
- Digits cannot change: only where the buffers live and when they are reused; the exchanges, their order and data are the same.

#### Modelled saving (mem_model.py, `COMM_LAYER_VSLOT_SHARE=1` versus 0; modelled, not measured; DIST_CHUNKS 8 = the code default)
| case | v-slots per APU, sum -> max | saving per node (4 APUs) |
|---|---|---|
| 576 nodes, 3.71e13 digits, MN_GROUPS 2,4,8,16,32,64,192,576 (the code's default at 576 too) | 3.364 -> 1.917 GB (levels 192: 1.447, 576: 1.917) | **5.79 GB** |
| 576 nodes, MN_GROUPS 2,..,64,576 | 1.917 -> 1.917 | 0 (one general-map level) |
| 10 nodes x 6.441e10 | 1.724 -> 1.724 | **0** (one general-map level, g = 10) |
| 4 nodes x 5e10, DIST_GEN=1 (the A/B) | 4.31 -> 2.16 GB | 8.6 GB (the test case only) |
- The idea table's -11.5 GB (S-1) was for DIST_CHUNKS 4; DC8 (now default) halves the slot: 11.5 / 2 = 5.8 GB, matching the 5.79 GB above. At 10 nodes the tree has a single general-map level, so there is nothing to share (0 GB, as the table suspected).
- Saving is in device memory at the 576 target only; X2 users see it as a smaller comm pool (the pool rule counts the max), non-X2 users as a smaller hipMalloc'd v-slot budget.

#### Test plan (job 12455, ~/s36/s36_batch.sh; 4 nodes, after S35's job 12452; no digit writes except the gates')
- Why DIST_GEN=1: at 4 nodes every group size is a power of two, so there are no slots to share. DIST_GEN=1 forces the general map at g = 2 and g = 4 (two levels; also reuses the lower binary group in the reciprocal).
- Gates (1e9 digits, `VERIFY OK` + digits identical to ~/ref/e_1000000000.txt): g1 2n X2+SHARE; g2 2n X2+SHARE+DIST_GEN; g3 2n SHARE+DIST_GEN (no X2); g4 2n SHARE (no X2); g5 4n X2+DIST_GEN (control, SHARE off); g6 4n X2+DIST_GEN+SHARE; g7 4n DIST_GEN+SHARE (no X2). Any failure ends the job (marker FAILED).
- A/B: 4 nodes, 5e10 digits per node (2e11 total): model says A peak device 333 GB (modelled; the 10-node measured peak runs ~17 GB above the model, so ~350 of 373 GB, assumed margin 20+ GB). ABBA (round odd A,B; even B,A), A = base + X2 + DIST_GEN=1, B = A + SHARE=1; stop rule after >= 4 complete rounds: wall CI excludes 0, half-width <= 15 s, or 8 rounds (max 12 attempts, 3 consecutive bad runs stop). Per run: wall, ecalc total, per-node peak device memory (sum of the 4 APUs' `driver: X of Y GB used`, max over phases; `s36_devpeak.py`). One 2-node pair (6.441e10 per node, A then B).
- Statistics: paired B-A with 95 % CI (t, `s36_stats.py`): wall, ecalc total, peak device memory (max and mean over nodes), comm pool.
- Expectation (modelled): peak device memory B - A about -8.6 GB per node; wall about 0.

#### Arming record
- Branch s36, commit db9cb7f (code, model, README); built on aac7 in ~/ntt-wt/s36 (`make -B`, log ~/s36-build.log). Driver files in ~/s36 (s36_batch.sh, s36_job.sbatch, s36_stats.py, s36_devpeak.py, s31_lib.sh); copies in this repo under archive:results_raw_2026-10-19/S36_driver/.
- Dry run of the driver logic (DRY=1, stub runs, ~/s36dry): gates, ABBA order, stop rule (stopped after 4 rounds on half-width), 2-node pair and digest all worked; one bug found and fixed in the dry run (loop variable clobbering the round counter).
- sbatch job **12455**: 4 nodes, `--dependency=afterany:12452`, `-x aac7-n01,aac7-n61`, -t 5:00:00, runs the driver directly and ends when it exits. Marker ~/s36/S36_DONE, ~/s36/ALERT, ~/s36/STOP (touch to stop), summary ~/s36/summary.txt, digest ~/s36/results.txt, stats ~/s36/stats.txt.
- Collect: `sshpass -p <cluster-password> ssh aac7 'cat ~/s36/S36_DONE; cat ~/s36/ALERT 2>/dev/null | head; head -70 ~/s36/results.txt' < /dev/null`

#### RESUME
1. Wait for ~/s36/S36_DONE. SUCCESS -> read results.txt; append measured numbers and the verdict to this file; the user decides adoption (switch stays default 0).
2. FAILED at a gate: read ~/s36/gate_g*.txt and log/gate_*.log. If only g5 (SHARE off control) failed, DIST_GEN=1 at 4 nodes is the problem, not SHARE: change the A/B to a 3-node run with MN_GROUPS=1.. or use 6 nodes later. If g6/g7 failed with g5 passing, the bug is in `need_vslot_shared` (owner hand-over, symmetric offsets in the pool, refcount free).
3. Not tested at scale: 576-node behaviour (cut last group, pool symmetry across groups of different size; all slots are allocated at the same size on every node under the pool rule, which is what keeps the pool offsets symmetric).

## S45 -- CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e)

(former file: S45.md)

Verdict: **SHARE=1 is effective and safe on the target launch line** (COMM_OFI=1, no X2). The plan and layout change only the v-slot term; a many-rank host test
(up to 600 ranks, non-power-of-two groups, a cut last group) found no wrong data, no leak, and symmetric pool offsets. The test is a TCP / host emulation, not the
real SHMEM / OFI transports, which no CPU node can run (no cxi, no libfabric on aac6).

#### 1. Login-node plan / layout (aac6-login, no GPU; measured by the C code; files archive:results_raw_2026-10-19/S45/plan_*.txt, launch_block_main13c244e.sh)
Exact internal target notes export block of main 13c244e (SHARE=1, COMM_OFI=1, COMM_SHMEM_POOL_MB=1536 ...), `MN_PLAN_ONLY=37100000000000:576`, `BS_LAYOUT_ONLY=64409722236:576`.
- Groups at 576: levels 1-6 are exact power-of-two groups (2..64, 576 = 9 x 64: no cut group); **level 7: 3 groups of 192; level 8: the whole 576**. Only levels 7 and 8 (and the dm phase) own v-slots. The target has **no cut last group** among its general-map levels (the "cut group" case is a risk for other sizes / schedules only).
- v-slot per APU (measured, layout): level 7 (g 192) 1.447 GB, level 8 / dm (g 576) 1.917 GB. SHARE=0: sum 3.364 GB per APU = 13.46 GB per node; SHARE=1: max 1.917 GB = 7.67 GB per node. **Saving 5.79 GB per node** (matches S36's model).
- Plan check OK both ways (1236 products, 103 / 107 pieces unchanged); `planes`, `arena`, `room`, `node` lines identical; only the `vslot` and the `plan vslot` / `plan cache` budget lines change. So SHARE changes only the v-slot placement.
- Device total, v-slots counted: `BS_LAYOUT_ONLY` here (no OFI pools: this build has no libfabric, `ofi_pool 0`, plan pool 9472 MiB is the SHMEM-only law, not the launch line's) 340.49 -> 334.70 GB. With the OFI pools counted (estimate.py --target, COMM_OFI=1, modelled): **357.5 -> 351.7 GB per node vs the 373.44 GB edge (margin 15.9 -> 21.7 GB)**; node 400.8 -> 395.0 GB (480 budget); times unchanged (180.2 / 181.8 s ...).
- X2 (COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1; not on the launch line), plan without OFI pools: the pool rule sets MB_192 = MB_576 = 915 MiB under SHARE (691 / 915 without), SHMEM pool 23040 -> 17664 MiB (-5.25 GiB); node 391.7 -> 386.0 GB.
- mem_model.py `vslot_resident` (modelled): default schedule 3.364 -> 1.917 GB per APU (5.79 GB per node); one general level (2..64,576), or 512 / 256 as the last lower level: 0 saving (that level is a power of two: no slots); 3 general levels (..96,192,576 or ..96,384,576, the latter with a cut 192 group): 4.811 -> 1.917 GB = 11.58 GB per node. archive:results_raw_2026-10-19/S45/vslot_model_cases.py.

#### 2. Many-rank host simulation (ecalc/tests/lay_host/, new; no product code changed)
- comm_layered.c has no host build. Harness: `build.sh` compiles a perl-rewritten copy of comm_layered.c (the 3 kernel launches) with g++ against `hip_host_shim.h` (malloc for device memory, memcpy for copies; the kernel paths are not exercised, the memcpy fallbacks are); `t_lay.cc`: one process per APU (node, d), NA = 4 intra TCP meshes, one inter TCP mesh per APU per group, levels as in the real tree (lower group size(s), then the whole), per level batches of unequal v-exchanges (depth 2, data checked word for word), the owner hand-over with the previous owner's exchange pending, and the return to the lower level. Checks: data, live "device" bytes, freed at destroy, and (X2) the live symmetric blocks equal across the members of the top group. The symmetric pool is emulated: `ofi` = comm_ofi.c's top-down last-fit COMM_OFI_SYM_TOP (what the launch line has), `shmem` = comm_shmem.c's first-fit with control blocks in the same pool (control size emulated, assumed).
- Shapes (processes = 4 x nodes), run with SHARE 0/1, X2 off/on (X2: both pool emulations), 3 aac6 MI250 nodes over TCP, ubase small payloads (archive:results_raw_2026-10-19/S45/lay_host_runs.txt):
| config | ranks | result |
|---|---|---|
| 36 nodes, 12,36 (3 equal groups, the 192/576 shape) | 144 | 6/6 VERIFY OK (earlier run, same code; its files were overwritten by the 3-level run) |
| 36 nodes, 6,12,36 (three owners) | 144 | 6/6 OK; peak live 4.81 -> 3.83 MB per process with SHARE (measured) |
| 39 nodes, 12,39 (groups 12,12,12,3: cut) | 156 | no-X2 OK; X2 ofi OK both; X2 shmem asym (both SHARE 0 and 1) |
| 72 nodes, 24,72 | 288 | 6/6 OK; 2.70 -> 2.38 MB |
| **144 nodes, 48,144 (3 equal groups, 4 x 144 = 576 ranks, 192/576 shape)** | **576** | **6/6 VERIFY OK; peak 2.686 -> 2.368 MB per process (-11.8 %, measured); symmetric on both pool emulations** |
| 150 nodes, 48,150 (groups 48,48,48,6: cut) | 600 | no-X2 OK, X2 ofi OK both SHARE; X2 shmem SHARE=0 run lost to a TCP "Connection timed out" (infrastructure), SHARE=1 asym |
| 15 nodes, 6,15 (6,6,3 cut; local, 60 ranks) | 60 | no-X2 OK; X2 ofi: **SHARE=0 asym (288 mismatches), SHARE=1 symmetric**; X2 shmem asym both |
- Every run: 0 wrong words, 0 live bytes after destroy (the last communicator frees the shared pair), 0 live symmetric blocks.
- Pool offset symmetry with a cut last group: (a) OFI emulation, SHARE=1: symmetric in every run (the pool rule gives every group size the same MB, so cut-group nodes do not regrow); SHARE=0 can be asymmetric (15-node case: cut-group nodes hold a smaller lower slot, so the top slot lands elsewhere): a hazard of the existing X2 path, not caused by SHARE; (b) SHMEM single-pool emulation: asymmetric with a cut group for SHARE 0 and 1 alike, because the control blocks differ by group size (assumed sizes). Neither occurs on the target: no cut group, and X2 is not on the launch line.

#### Under COMM_OFI=1
- Without X2 the v-slots are hipMalloc'd (`need_vslot_shared` non-pool branch): the OFI transport is not involved, so SHARE takes effect identically (-5.79 GB per node modelled; the plan counts it in the node budget, ECALC_VSLOT_BUDGET=1). Not a no-op, not unsafe there. Only the libfabric transport itself (cxi) could not be run here.

#### Doubts
- Host emulation: kernels, SHMEM / OFI one-sided puts, and the real pool code are not exercised; sizes are small. The target numbers are modelled (estimate.py, mem_model.py) except the layout lines (C code, no OFI pools). 

#### RESUME
Nothing running. Re-run: `ecalc/tests/lay_host/build.sh <out>`; in an allocation `s45_all.sh <t_lay> <outdir>` (ports 9000-18000, below the ephemeral range).

## S52 -- more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9)

(former file: S52.md)

CPU only: this workstation (model scripts) and the aac7 login node (the C code's plan / layout printers; no Slurm job, no GPU, no
target access). No engine code changed. Every number is labelled **m** (measured: printed by the C code, or recorded elsewhere), **mod**
(modelled: `ecalc/estimate.py` / `mn_model.py`) or **ass** (assumed).

#### Verdict

1. **Largest safe size: 3.82 x 10^13 digits** (+3.0 % digits over 3.71 x 10^13; 3.80 x 10^13 has the identical layout and margin).
   Device peak 357.9 GB per node (mod, `estimate.py`, SHARE=1), 360.0 GB for rank 0 with the 2.1 GB rank-0 allowance, against the 373.44 GB
   edge: **margin 13.4 GB (3.6 %)**, above the 10 GB rule of M1. Node total 402 GB of 480 (mod). Time 185.9 s without the write
   against 180.3 s at 3.71 x 10^13 (mod): the extra digits cost the same per digit.
2. **Do not go above 3.82 x 10^13.** At 3.83 x 10^13 the C plan adds pieces (the largest tree group goes from 42 to 54 pieces, 109 to 121
   in all) and the model's wall jumps +20 s (185.9 -> 206.1 s, +11 %) for +0.3 % digits; from 3.88 x 10^13 the device peak steps up to
   366.5 GB (margin 4.8 GB with rank 0), and 4.08 x 10^13 is over the edge (375.1 GB, margin -3.8 GB).
3. **Messages:** 2.65 M per APU at 576 (mod): reciprocal 1.098 M, division 0.927 M, tree levels 0.624 M. 95 % are the transform
   exchanges' chunk messages (K = 8 chunks x 575 peers). Memory-neutral coalescing saves 0.48-0.66 M messages (18-25 %) = **1.0-1.4 s at 2 us,
   2.4-4.5 s at 8.5 us, 3.7-8.4 s at 20 us** (mod). Larger savings need memory (K = 4: 9.6 s at 8.5 us, 21.2 s at 20 us, for +13.4 GB per node
   = the whole margin) or are lost (the 3-layer relay: +61 s at 8.5 us). Design in section 5; no code change.

#### 1. Method and commands (part a)

Build (aac7 login node, worktree of main daeb0f9, branch s52a, since removed): `cd ecalc && source aac7env.sh && module load cray-dsmml
cray-openshmemx && make -B -j8 SHMEM_CRAY=1 GMP_HOME=~/gmp` (the build has libfabric, so the layout counts the four OFI pools: 9.66 GB).
Environment: the target launch block of `archive:results_raw_2026-10-19/S45/launch_block_main13c244e.sh` (`ECALC_NP=auto`, `RNS_DIST_CACHE_FIT=1`,
`RNS_DIST_CACHE_PARTIAL=1`, `MN_OUT_DKM_HI=1`, `MN_T_CHUNK_MB=1024`, `DM_MN_LEAN=1`, `COMM_LAYER_VSLOT_SHARE=1`, `COMM_OFI=1`,
`COMM_SHMEM_ROUND_MB=1024`, `COMM_SHMEM_POOL_MB=1536`, `MN_GROUPS=2,4,8,16,32,64,192,576`, `MN_TOPO_GROUP=0`, ...; `DIST_CHUNKS` is 8 by default).
Per size D (total digits), D/576 digits per node:

```
MN_PLAN_ONLY=<D>:576 ./ecalc <D> /dev/null        # plan check, pieces, pool          -> plan_<D>.txt   (m, C code)
BS_LAYOUT_ONLY=<D/576>:576 MEM_REPORT_DEVS=1 ./ecalc <D> /dev/null   # layout, vslot  -> layout_<D>.txt (m, C code)
COMM_LAYER_VSLOT_SHARE=1 python3 ecalc/estimate.py --fabric target-m --g 576 --D <D/576 ...> --groups 2,4,8,16,32,64,192,576
```

The estimate output is in `estimate_target_m_share1.txt` (four sizes) and `estimate_fine_share1.txt` (3.80-3.95 x 10^13 in steps of 0.02).
`estimate_target_m.txt` is the same without `COMM_LAYER_VSLOT_SHARE` (357.5 GB at 3.71 x 10^13, the pre-S45 figure; the launch line has SHARE=1).
The `estimate.py` device figure is the sizing figure (it adds the bs phase's +6 GB and the other modelled terms: 351.7 GB at 3.71 x 10^13, the standing
figure of the target notes); the C layout's `device_with` (344.37 GB) is exact for the layout but leaves those terms out.

#### 2. Layout and plan at 576 nodes

All plans: `plan check ... OK`, ECALC_NP=auto, 1236-1238 products, SHMEM pool 1536 MiB (m).

| digits (x 10^13) | pieces total / tree largest-group (m) | C layout device_with GB (m) | C layout node_with GB (m) | est. device GB (mod) | est. rank 0 = +2.1 GB | margin to 373.44 GB, rank 0 | est. node GB of 480 (mod) | wall no write s (mod) |
|---|---|---|---|---|---|---|---|---|
| 3.71 | 103 / 107 | 344.37 | 386.45 | 351.7 | 353.8 | 19.6 | 394 | 180.3 |
| 3.80 | 109 / 109 | 348.66 | 390.75 | 357.9 | 360.0 | 13.4 | 402 | 185.2 |
| **3.82** | 109 / 109 | 348.66 | 390.75 | 357.9 | 360.0 | **13.4** | 402 | **185.9** |
| 3.83 | 109 / 121 | 348.66 | 390.75 | 357.9 | 360.0 | 13.4 | 402 | 206.1 |
| 3.86 | 109 / 125 | 348.66 | 390.75 | 359.7 | 361.8 | 11.6 | 402 | 211.7 |
| 3.88 | 109 / 125 | 348.66 | 390.75 | 366.5 | 368.6 | 4.8 | 411 | 211.9 |
| 3.90 | 111 / 127 | 352.96 | 395.04 | 366.5 | 368.6 | 4.8 | 411 | 215.8 |
| 3.95 | 111 / 127 | 352.96 | 395.04 | 366.5 | 368.6 | 4.8 | 411 | 216.8 |
| 4.08 | 119 / 133 | 352.96 | 395.04 | 375.1 | 377.2 | **-3.8** | 419 | 226.7 |

Notes.
- The C layout steps by one 4.3 GB arena chunk between 3.88 and 3.90 x 10^13; the model steps slightly earlier (3.86-3.88). The model is the conservative one
  and is used for the recommendation. On the exact C layout basis the rank-0 margin at 3.82 x 10^13 is 373.44 - (348.66 + 2.1) = 22.7 GB.
- Rank 0 allowance: "measured rank-0 peak 1.8-2.1 GB from its layout" (S19B: rank 0 measured 389.1-389.6 GB against a layout node_with of 391.49 GB, i.e. 1.9-2.4 GB
  *below* the layout; M1 states it as a margin of -1.8..-2.1). It is applied here in the unfavourable direction (+2.1 GB on top of the modelled device figure,
  which already carries +7.3 GB over the C layout), so the margins are conservative.
- The 373.44 GB edge was measured on the target (n = 5, TGTBENCH2); the rank-0 margin and the per-node peak of every other node are therefore both above 13 GB at 3.82 x 10^13.
- Node total (host + device) stays 402 GB against 480 GB and the 6 GB MemAvailable guard; not binding. Part-file volume scales linearly: 0.444 B/digit = 17.0 TB (mod).
- Time knee: the model's wall rises gently from 3.71 to 3.82 x 10^13 (180.3, 185.2, 185.9 s at 3.71, 3.80, 3.82) and then rises 20 s between 3.82 and 3.83 x 10^13
  where `mn_plan` raises the largest group's pieces (the plan step is m; its cost is mod). This step is the reason for the cap at 3.82 and not for a memory limit.

Recommendation: request **38200000000000** (3.82 x 10^13; 6.632 x 10^10 digits per node). Safety margin: 13.4 GB (3.6 %) of device edge on the conservative basis, 22.7 GB
on the exact-layout basis. If the user prefers a round number, 3.80 x 10^13 has the same layout, device and margin and 0.7 s less time (mod). Gain over the current target: +3.0 % digits (+2.4 % for 3.80),
+5.6 s (mod). M1's earlier estimate of "+6.5 % digits" (about 4.0 x 10^13) is not available: it ignored the plan step at 3.83 x 10^13 (+20 s) and the arena step at 3.88 x 10^13 (-8.6 GB of margin).
Labels: edge m (target); layout m; est. mod; the 10 GB rule is the project's (M1).

#### 3. Messages per APU per phase at 576 nodes (part b)

Tool: `archive:results_raw_2026-10-19/S52/msg_count.py` (reads `mn_model.py`; no model change: it wraps `Fabric.a2a`, `piece_cost`, `hit_cost`, `Cost.add` to attach a breakdown to the model's own
`Cost` objects; the totals equal the model's `msgs/APU` exactly, 2 649 602). Run: `python3 archive:results_raw_2026-10-19/S52/msg_count.py > archive:results_raw_2026-10-19/S52/msg_count_out.txt`.
Setting: `target-m` fabric, 47 GB/s per APU, 2 us per message, 6.441 x 10^10 digits per node (3.71 x 10^13), `MN_GROUPS` as the launch line, wall 219.0 s without write (mod; the old model's figure, 219.2 s in OPPORTUNITIES).

| phase (mod) | msgs per APU | transform-chunk msgs (K = 8 x 575 x exchanges) | redistribution / spill msgs | shifts, adds, other | mean transform message | exposed s of the phase |
|---|---|---|---|---|---|---|
| tree levels g = 2 ... 64 (6 levels) | 48 120 | 45 600 | 2 400 | 120 | 8-268 MB | 28.2 |
| tree level g = 192 (3-way) | 229 582 | 217 740 | 11 460 | 382 | 1.86 MB | 11.4 |
| tree level g = 576 (3-way) | 346 150 | 327 750 | 17 250 | 1 150 | 0.83 MB | 8.2 |
| reciprocal (49 pieces) | **1 098 275** | 1 009 945 | 53 155 | 35 175 | 0.27 MB (0.016-1.05 MB) | 10.3 |
| division (16 pieces) | **927 475** | 874 000 | 46 000 | 7 475 | 0.83 MB | 23.9 |
| **total** | **2 649 602** | 2 475 035 (93 %) | 130 265 | 44 302 | | 82.1 |

- Every message goes to one of the g - 1 peers in the exchange's group (one per node, aggregated from the node's four slabs already: `comm_layered.c` sends "block r' = the four
  slabs" per destination node over the APU's own mesh). So node-level aggregation exists; what multiplies the count is K chunks x n_tr exchanges per piece
  (n_tr = primes x (operands + 1), up to 12) x 575 peers.
- The reciprocal's 1.01 M transform messages are mostly small: at g = 576 its pieces send 0.016-1.05 MB messages (0.6 M of them are 0.5 MB or smaller), because its operands
  are 4-64x smaller than the cap while K stays 8. The division's and the g = 576 tree level's exchanges are already at the cap (a slab of 478-537 MB per chunk), so
  there is no slack to coalesce there without spending memory.
- Message totals at the other settings are in `msg_count_out.txt` (the log of this run, with the size histogram per phase).

#### 4. Modelled saving of each scheme (mod; wall without write, 6.441 x 10^10 digits per node)

Per-message cost 2 / 8.5 / 20 us (2 us ass = the target's design value; 8.5 us m on aac7; 20 us = the pessimistic case of OPPORTUNITIES C3). The model charges the message
count x latency fully and hides part of it behind the xGMI stage (pow-2 groups 75 %, the general map 1.1 %).

| scheme | memory | msgs per APU | wall s @2 us (saving) | wall s @8.5 us (saving) | wall s @20 us (saving) |
|---|---|---|---|---|---|
| base (K = 8, 2-layer) | - | 2 649 602 | 219.0 | 237.5 | 268.4 |
| **S1 coalesce to the slab bound** (K' = ceil(bytes / 537 MB), power of two, general-map groups 192 and 576 only) | none (the slab bound is unchanged) | 2 173 502 | 218.0 (-1.0) | 235.1 (-2.4) | 264.6 (-3.7) |
| S1 on all groups, any K' | none | 1 990 999 | 217.5 (-1.4) | 233.0 (-4.5) | 259.9 (-8.4) |
| **S2 bundle the piece's n_tr exchanges (primes, operands) into one slab-bound bundle**, power of two, general map | none | 1 954 320 (n_tr = 9 ass) | 217.6 (-1.4) | 233.4 (-4.1) | 260.7 (-7.6) |
| S2, idealised (any K', all groups; bound) | none | 1 740 287 | 217.1 (-1.9) | 231.1 (-6.4) | 255.7 (-12.7) |
| S3 uniform K = 4 | +13.4 GB per node (README, DIST_CHUNKS 8 -> 4) | 1 460 970 | 216.5 (-2.5) | 227.9 (-9.6) | 247.1 (-21.2) |
| S3 uniform K = 2 | more than K = 4 (not modelled, ass) | 842 203 | 215.3 (-3.7) | 223.0 (-14.5) | 235.7 (-32.6) |
| S4 3-layer relay through group leaders (`--layers 3`, G = 64) | scratch not modelled | 864 083 | 290.9 (+72.0) | 298.4 (+60.9) | 311.0 (+42.7) |

- S1 and S2 are bounded by the messages at the cap: 1.2 M of the 1.74 M remaining messages (division 0.87 M, tree level 576 0.33 M) are already one-slab-bound messages and cannot shrink
  without memory. The total latency bill is 2.65 M x lat = 5.3 / 22.5 / 53.0 s, so no scheme can recover more than that (S3 K = 2 recovers 70 %, with memory).
- S4 loses: the relay doubles the NIC bytes (13.7 -> 27.9 TB per node, mod) and the fabric time dominates the saved latency even at 20 us. Rejected.
- S3 is a trade against digits: 13.4 GB is about 2.3 x 10^12 digits at the 5.7 GB per 10^12 slope of section 2 (+6 % digits, about +13 s), so K = 4 pays only if the per-message cost is above about 12 us.
- The model does not charge a loss of xGMI/fabric overlap when K falls; S1/S2 are restricted to the general-map groups (192, 576), where the model's hidden fraction is 1.1 % (the measured X13 constant), so the overlap
  loss there is small (ass). The equal-slab (power-of-two) groups keep K = 8 (75 % hidden xGMI, m on aac6).

#### 5. Design: memory-neutral message coalescing (not implemented)

Proposed switch (off by default, one README row): `DIST_COALESCE=1`.
1. `ntt_dist.c` (K chunks, line ~112; `comm_layered.c` exchange): replace the fixed K by `K' = min(K, 2^ceil(log2(ceil(slab_total / S_max))))` per transform exchange, where
   `slab_total` = the exchange's bytes per APU and `S_max` = the run's largest chunk slab (bytes per APU at the cap, divided by K = 8: 536.9 MB at the cap 2^31). Every exchange then
   moves at most `S_max` per chunk, so the scratch, the two inflight slots and the v-slots (sized from the largest exchange) are unchanged: **memory-neutral by construction**.
   Apply it only when `g` is not a power of two (the general map: the 192 and 576 levels, the reciprocal's g = 576 steps), keep K = 8 on the equal-slab path (its overlap is the point of K).
2. Bundle the n_tr transform exchanges of one piece when their sum fits `S_max` (S2): post the primes' (and both operands') slabs for the same rows as one message per peer; at the reciprocal's
   g = 576 steps (operands 4-64x below the cap) this cuts the exchange count by up to n_tr. Needs the primes' transforms of a row block done before the post; the planes are resident,
   so no new buffer.
3. Group the redistribution exchanges A and B (single-chunk) of a piece into one (about 53k messages, 0.1 / 0.45 / 1.1 s at 2 / 8.5 / 20 us if all exposed, mod bound).
4. Digits are unchanged by construction (the same bytes, different framing); gate: `tools/gate.sh --build` 1 and 2 nodes, e9, 4 x 10^10, and a 4-node run (general map, forced) with
   `COMM_XSTATS=1` to count messages per phase (m) and compare with the table of section 3.
5. Cannot be measured below the target: the message count grows with g (30k at 10 nodes, 2.65 M at 576), and the per-message cost on the target is unknown (T-7 in OPPORTUNITIES).
   A 4-6 node run validates correctness and the per-exchange count (m), not the saving.

Expected value (mod): 1.0-1.4 s at 2 us, 2.4-4.1 s at 8.5 us, 3.7-7.6 s at 20 us (0.5-2.8 % of the wall). It is small against the other levers because 1.2 M of 2.65 M messages are
at the memory-bound slab size; the larger lever is the division's K (S3) if memory allows, which the digit margin competes for.

#### 6. Open issues and doubts

- The rank-0 sign convention (above or below its layout) is read two ways in the sources; both give a margin above the 10 GB rule at 3.82 x 10^13.
- The model's time knee at 3.82 -> 3.83 x 10^13 is from the plan's piece step (m) and the model's pricing (mod); the true wall near the knee should be re-checked with `estimate.py` after the S48 refit.
- The estimate's device figure carries +7.3 GB over the C layout (bs phase + terms): if the real bs growth is the measured 2.1-5.8 GB, the true margin is about 2-5 GB larger than stated.
- Per-message costs on the target are ass (2 us) or m on aac7 (8.5 us at 4-10 nodes); the savings above are linear in them.
- Overlap loss from a smaller K is not in the model (ass: small on the general map).

#### Decision 2026-10-11: DIST_COALESCE skipped
- The user decided not to build `DIST_COALESCE` (message bundling) unless the target's per-message cost is shown to be at least 8.5 us. The target's cost is still
  assumed (2 us); the only measured figure is 8.5 us on aac7 at 4-10 nodes. At 2 us the saving is 1.0-1.4 s of the 576-node wall (mod, 0.5-0.7 %); at 8.5 us, 2.4-4.1 s (mod).
  No code, no switch row. Reopen this only with a measured target per-message cost of 8.5 us or more (then the section 5 gates apply).

#### RESUME

Nothing running; no Slurm job; aac7 worktree `~/ntt-wt/s52` (branch s52a) and the helper files `~/s52_*`, `~/s52_out` removed at the end. To redo: build as in section 1 on the aac7 login node, run the plan/layout loop for the sizes,
`python3 ecalc/estimate.py --fabric target-m ...` with `COMM_LAYER_VSLOT_SHARE=1`, and `python3 archive:results_raw_2026-10-19/S52/msg_count.py`. Next: the user decides the size (3.82 x 10^13 or 3.80 x 10^13); if the message
scheme is wanted, implement `DIST_COALESCE` behind its switch and run the section 5 gates.
- 2026-10-11: decision above (skipped unless the target's per-message cost is at least 8.5 us).

## B06 - uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001

(former file: B06.md)

Question: does a run with an UNEVEN last group (the "exclude k bad nodes" case of results/NODE_SLOWMODE.md §B04 on branch b04) work and give correct digits?
Status: 5-node results added 2026-10-11 (f3); earlier text: PARTIAL. 3-node correctness done; the 5-node correctness and the timing pair were NOT run (aac7 node budget never allowed 5 nodes: wait script
re-checked every 2 min for 45 min, others held 4-6 nodes, then BLOCKED). The coordinator's cut-down request (one correctness run each at 3 and 5 nodes, timing
pair at 5 nodes only) is therefore half done.

#### What "uneven last group" means in code (read)
`mn_groups_parse` (rns_dist.c): an explicit list must be increasing, each entry a multiple of the previous one EXCEPT that the first entry >= size is replaced
by `size` with no multiple check, and `size` is appended if missing. So `MN_GROUPS=2,3` at 3 nodes, `2,5` / `4,5` at 5 nodes, `...,192,573` at 573 nodes are
accepted. Plan (MN_PLAN_ONLY, measured on the login node, 6.441e10 digits/node, aac7 line + ECALC_SEGV_TRACE=1): all pass "plan check OK".
| nodes | schedule | levels (node 0 pieces) | plan total pieces |
|---|---|---|---|
| 3 | default (3) | 3-way, 20 | 98 |
| 3 | 2,3 | 2-way groups of 2 (8), then "3 = 2 children of 2" i.e. 2+1 (12) | 98 |
| 5 | default (5) | 5-way, 18 | 85 |
| 5 | 2,5 | 2,2,(1) then 3 children of 2 (16) | 91 |
| 5 | 4,5 | 4-way (12), then 2 children of 4 = 4+1 (6) | 85 |
| 6 | default 2,6 | 2-way (8), 3-way (20) | 108 |

#### Correctness (measured, 3 nodes, aac7 line incl. COMM_TRANSPORT=shmem, T2048, X2, digits written, compared with `ecalc/digcmp.sh`)
| run | digits | schedule | VERIFY | digcmp |
|---|---|---|---|---|
| c9_def | 1e9 | default | OK | identical (ref ~/ref/e_1000000000.txt) |
| c9_2,3 | 1e9 | 2,3 (uneven) | OK | identical |
| c10b_2,3 | 1e10 | 2,3 (uneven) | OK (44.1 s) | identical (ref = first 1e10+3 bytes of ~/ref/e_1e11.out + newline) |
Note: first 1e10 attempts (2,3 and default) said DIFFERS because the prefix reference file was still being written when they ran (7.9 GB of 10 GB);
VERIFY OK in both. The rerun with the finished reference is identical. Not a schedule issue; the default run at 1e10 was not repeated.

#### Timing (measured / modelled; no digit write)
| nodes | schedule | measured `total` | modelled no-write (estimate.py --fabric aac7_s48) |
|---|---|---|---|
| 3 | default | 210.2 s (6.441e10/node, VERIFY OK) | 237.2 s |
| 3 | 2,3 | not run | 219.2 s |
| 5 | default | 213.15 s (VERIFY OK, 5 nodes) | 297.9 s |
| 5 | 2,5 | 226.23 s (VERIFY OK) | 273.0 s |
| 5 | 4,5 | 232.18 s (VERIFY OK) | 247.8 s |
| 6 | default | not run | 293.5 s |
The model overpredicts the one measured point by 13 %. The model says uneven is faster than the all-in-one top level at 3 and 5 nodes (-7.6 %, -8 %/-17 %),
which is unconfirmed by measurement.

#### Recommendation (provisional)
- The code path works at real structure (uneven last group: 2+1 children) and gives identical digits at 1e9 and 1e10 on 3 nodes; 9 and 10 nodes were already
  VERIFY OK with `3,9` / `4,10` (RESULTS Phase 13 N). So an explicit `MN_GROUPS=2,4,8,16,32,64,192,<g>` for "exclude k nodes" is not blocked by code.
- Not yet shown: speed at real per-node size (only the modelled B04 figures), and a 5-node case with two uneven levels. Before relying on it, run once
  at 5 nodes: `MN_GROUPS=2,5` and default, 3.2205e11 digits, no write (about 2 x 5 min), plus 1e10 digits with write for correctness.
- Fallback policy if never measured: round down to a friendly count (576 stays; a bad node -> 575 is the poor case, default modelled +34 %), or use
  explicit groups and accept the modelled estimate.

#### RESUME
Driver scripts: aac7 `~/b06/drv.sh` (3 nodes), `~/b06/drv5.sh` (5 nodes: `drv5.sh 5 <hold> "2,5" 0`; its `corr` / `run` functions are in `drv.sh`; set
the deadline arg to a future epoch), refs `~/b06/ref_1e10.txt`, logs `~/b06/run3/`, plans `~/b06/plan_*`. Worktree aac7 `~/ntt-wt/b06`, built with
SHMEM_CRAY=1 (binary at bc80001). Hold needs 5 nodes (QOS 6).

##### Update (queued)
The 5-node pair is queued as aac7 queue item `~/aac7q/queue/47_b06_five.sh` (OWNER=b06, NODES=5, EST_MIN=25; dry-run STUB=1 OK). It runs
`~/b06/drv5.sh` (1e10 correctness with digits for `2,5`, then timing default and `2,5` at 6.441e10/node, no write) and, if under 18 min elapsed,
`~/b06/drv5b.sh` (timing `4,5`). The aac7 build `~/ntt-wt/b06` (detached at bc80001, rebuilt with SHMEM_CRAY=1) is needed and was left in place;
remove it after collecting: `git -C ~/ntt-acc worktree remove --force ~/ntt-wt/b06`.
Collect: `cat ~/b06/run5/drv.log` (lines `c10_2,5 ... digcmp: identical|DIFFERS`, `t_def`, `t_2,5`, `t_4,5` with `total=`; DONE / DONE4 markers in ~/b06/run5/),
then fill the timing table above (model: 5 nodes default 297.9 s, 2,5 273.0 s, 4,5 247.8 s).

#### 5-node results (aac7 queue item 47_b06_five, collected by f3, 2026-10-11)
Source: aac7 `~/b06/run5/drv.log` (all rc 0, every run `all 5 nodes: VERIFY OK`). 3.2205e11 digits (6.441e10/node), no digit write, one run per schedule (measured; NOT paired ABBA rounds, so no CI).
| check | result |
|---|---|
| 1e10 correctness, `MN_GROUPS=2,5`, with digit write, 5 nodes | VERIFY OK, 41.59 s, `digcmp: identical` (measured) |
| timing default | 213.15 s (elapsed 222) |
| timing `2,5` | 226.23 s (+13.1 s, +6.1 % vs default) |
| timing `4,5` | 232.18 s (+19.0 s, +8.9 % vs default) |
Model (estimate.py, aac7_s48 fabric): 297.9 / 273.0 / 247.8 s, i.e. uneven 8 % / 17 % FASTER than default. Measured: the model overpredicts the absolute time by 40 % (default) and gets the ordering wrong (uneven slower by 6-9 %).
Rate check (calib_table.py, measured): all three runs were in the NIC slow mode (mean all-to-all rate per APU thread: default 10.25, `2,5` 9.61, `4,5` 9.57 GB/s; slow < 11.95), and all three carry the single-node-chain flag, so none is a clean fit row. If the whole total scaled as 1/rate, default at the `4,5` rate would be 228 s and at the `2,5` rate 227 s: the measured gaps (13 s, 19 s) are within what the rate difference alone could explain.
Verdict: correctness of uneven groups at 5 nodes (`2,5` and `4,5`, uneven last group) is shown (VERIFY OK; digits identical at 1e10 for `2,5`). A speed gain is NOT shown; the modelled 8-17 % gain is NOT reproduced at one run each. Do not use explicit uneven groups for speed on this evidence; for "exclude a node" they cost at most about 9 % (measured, single run, partly rate noise). A paired ABBA (default vs `2,5`, 3 rounds, about 25 min per round) would be needed for any claim either way.

## O1 - overnight 6-node paired ABBA, MN_GROUPS `2,6` (default) vs single level `6` (TASKS 42)

(former file: O1.md)

Status: DONE 2026-10-12 01:40 ET (aac7 queue item `60_o1_abba_groups`; marker SUCCESS, collected read-only 2026-10-12). Branch o1 off main 0298fe2. Labels: measured / modelled / assumed. Times Eastern.
Question: is the single 6-way level faster than the default `2,6` at 6 nodes? Evidence so far (results/GRID_GROUPS.md §C09, L4.md, B06.md section 5): `[6]` faster by 17-24 s, SE 12-14 s, about 1.7-2 SE (f3 pairs, 5+5 runs, measured); the model says the opposite (-9 s, modelled). Purpose: settle sign and size before the default group schedule is decided.

#### Design
- Arms: A = default grouping (`MN_GROUPS` unset, = `2,6` at 6 nodes); B = `MN_GROUPS=6`. Everything else identical.
- Run: 6 nodes (the QOS maximum), 6.441e10 digits per node (3.8646e11), no digit write, f3's production launch line (COMM_TRANSPORT=shmem, T2048, FIT, X2, VSLOT pool/share, DM_MN_LEAN, ROUND_MB 1024, COMM_XSTATS=1, MN_WAIT_STATS=1, ECALC_INIT_TL=1; the line is in the item). Binary: main 0298fe2 SHMEM build in aac7 `~/ntt-wt/o1` (`-DCOMM_SHMEM` in `~/o1_build.log`). Correctness: every run must print `mn: all 6 nodes: VERIFY OK` (no digits written; `MN_GROUPS=6` digits were identical to the reference in f3's 1e10 test, measured; plan check OK in B06/C09).
- Rounds: one round = one quartet, ABBA then BAAB alternately (A,B = arms), so each round holds 2 runs per arm with the arms balanced in time. Round difference d = mean(B) - mean(A) (negative = single level faster).
- Per-run covariates: NIC MCU counter group (A slow / B fast; `tools/envprobe.sh --snap` before and after the run, `--group`, as in f3/A14: group is drawn per launch, not controllable), flow-control stall millions (fgfc), the run's mean all-to-all rate r (GB/s per APU thread, from the `comm_shmem ... per APU thread` lines), init time (cold start flagged if init > 36 s).
- Estimators (`tools/o1_stats.py`, pure Python, t distribution, 95 % CI): **raw** = mean of round differences (df = rounds - 1); **nic** = OLS `total = c + d*[B] + g*[NIC group A]` on all valid runs (df = n - 3); **rate** = OLS `total = c + d*[B] + b/r` (the L4 rate-adjusted form); each also without cold starts (`_clean`). Rounds with a missing or failed run drop out of `raw` only.
- Stop rule (on `raw`, checked after every completed round, minimum 3 rounds): p < 0.05, or CI half-width <= 5 s; otherwise run until the time cap: a round starts only when 4 x 340 s remain of the 7500 s (125 min) driver budget (about 5 rounds = 20 runs expected; round cap 8). Also stops after two rounds with >= 3 failed runs or on a `STOP` file in the output dir. Note: repeated looks at p inflate the false-positive rate; read an early p just under 0.05 with the CI and the adjusted estimates, not alone.
- Pass reading (same bars as C09/L4): `[6]` faster if the CI excludes 0 with d <= -8 s; the model's order reproduced if d >= +8 s; else inconclusive. The `nic` estimate is the one to prefer if the NIC groups are unbalanced between the arms.

#### Queue item
- File: aac7 `~/aac7q/queue/60_o1_abba_groups.sh` (copy `tools/o1_item.sh`); header OWNER=o1, NODES=6, EST_MIN=130, APPROVED=yes (coordinator pre-approved), NOT_AFTER=2026-10-12 07:00 ET. Order in the queue: after 50 (H4), before 70. The daemon holds 6 nodes (`-t 200` min cap, kills the item at 200 min).
- Stub dry run on aac7 (no Slurm, synthetic totals): passed (20 stub runs, summary and marker written, stop rule exercised); `o1_stats.py` checked against hand values (t quantiles 2.776 at df 4, 2.228 at df 10; OLS on a 4-point line).
- Output dir aac7 `~/o1/run/`: `totals.tsv` (seq, arm, round, total, init, rate, nic group, fgfc, ok), `r<seq>_<arm>.log`, `grp<seq>.pre/.post`, `drv.log`, `last_stats.txt`, `summary.txt`, **marker `~/o1/run/O1_DONE`** (one line `SUCCESS|FAILED|BLOCKED <reason> <epoch>`; SUCCESS needs >= 8 valid runs).
- Collect (one command): `sshpass -p <see private protocol> ssh aac7 'cat ~/o1/run/O1_DONE; cat ~/o1/run/summary.txt; cut -f1-7 ~/o1/run/totals.tsv'` (before it exists: `tail -5 ~/o1/run/drv.log`). Early stop: `touch ~/o1/run/STOP` (ends after the current round).
- Expected finish: starts after H2 (to about 00:20) and H4 (about 50 min) -> about 01:15; ends about 03:20 (modelled, assumed ~5.7 min per run).

#### Results (measured; collected 2026-10-12 from the aac7 summary, totals and driver log)
Marker: `SUCCESS time: 798 s left < 4 runs; 6 rounds, 23 valid runs`. 24 runs launched 00:00-01:40 ET (about 4 min each after run 1), 23 valid: run 2 (B, round 1) produced no total (failed; its round drops out of `raw`, so raw has 5 rounds). Run 1 (A) was a cold start (init 41.7 s, total 338.85 s). All valid runs VERIFY OK. The stop rule did not fire (p 0.350, half-width 6.3 s); the time cap ended the loop.

##### Estimates, B - A = `MN_GROUPS=6` minus default `2,6` (negative = single level faster)
| estimator | runs | B-A s | 95% CI s | p |
|---|---|---|---|---|
| raw (mean of 5 round differences) | 23 (5 rounds) | +2.4 | [-3.9, +8.7] | 0.350 |
| NIC-group adjusted | 23 | -5.8 | [-22.9, +11.3] | 0.488 |
| rate adjusted (1/r) | 23 | -22.0 | [-36.8, -7.2] | 0.006 |
| raw, no cold start | 22 (same 5 rounds) | +2.4 | [-3.9, +8.7] | 0.350 |
| NIC-group adjusted, no cold start | 22 | +2.4 | [-5.7, +10.4] | 0.546 |
| rate adjusted, no cold start | 22 | -7.8 | [-15.3, -0.3] | 0.042 |

The pass reading (CI excludes 0 with d <= -8 s for `[6]`; d >= +8 s for the model's order) is met by none of the six rows: the rate-adjusted no-cold row has p 0.042 but its CI reaches -0.3 s and the point estimate is -7.8 s, just short of the 8 s bar. Verdict: **inconclusive, no measurable difference in the raw totals**; the earlier "17-24 s faster" is not reproduced.

##### Arm statistics (measured)
| | runs | mean total s | median | sd | mean rate GB/s per thread |
|---|---|---|---|---|---|
| A `2,6` (all) | 12 | 255.3 | 246.2 | 26.9 | 8.24 |
| A without cold start | 11 | 247.7 | | 6.0 | |
| B `6` | 11 | 252.0 | 246.4 | 13.0 | 7.65 |

Per-round B-A (s): round 1 not paired (run 2 failed; A had the cold start), +9.9, +0.4, +2.8, +3.0, -4.1.

##### NIC group mix (measured; group A slow, B fast)
| arm | group A runs (mean s) | group B runs (mean s) |
|---|---|---|
| A `2,6` | 5 (268.9, sd 39.3; includes the cold start) | 7 (245.6, sd 5.8) |
| B `6` | 6 (258.6, sd 14.1) | 5 (244.0, sd 5.5) |
Groups are drawn per launch: 11 runs in A, 12 in B overall. Group B runs are the same for both arms within noise (245.6 vs 244.0 s, +1.6 s for `[6]` faster); group A runs differ by 10 s but the A arm there is one cold start (without it the group-A mean for A is about 252 s, close to B's 258.6, with `[2,6]` still faster). The fitted group effect is +19.1 s (all) / +10.4 s (no cold start).

##### Reading and caveats
- Raw totals show no difference: warm, group-B runs of both arms sit at 244-246 s. The default `2,6` is not slower at 6 nodes in this test; its larger spread in L4 (sd 22 s) came mostly from the slow NIC group and the cold start here (A sd 6.0 s without the cold start).
- The rate-adjusted estimates are not independent evidence of a speed-up: the run's all-to-all rate r is partly a result of the arm (B has a lower mean rate, 7.65 vs 8.24, as L4 found for larger groups), so dividing it out removes part of the arm's own effect. Treat them as a sensitivity only.
- Repeated looks: the driver checked `raw` after rounds 3, 4, 5, 6 and the six estimators were read side by side; one p of 0.042 among six rows, with a CI touching 0, is not a result. Only 5 informative rounds, 22-23 runs; the CI half-width (6.3 s raw) misses the 5 s target.
- One cold start (run 1) and one failed run (run 2) were handled as designed (cold flagged, round dropped from raw).

##### Recommendation (a decision for the user, not adopted)
- Evidence: at 6 nodes `[6]` is not faster than `[2,6]` (raw +2.4 s, CI [-3.9, +8.7]); it may be up to about 9 s slower or 4 s faster. It is also not slower, and its run-to-run variability is lower (sd 13 vs 27 s with the cold start; similar without).
- Option 1: keep the default `2,6` at 6 nodes (no change, no evidence for a gain). Option 2: adopt `MN_GROUPS=6` at 6 nodes (no measured gain; the only argument is the rate-adjusted sensitivity and lower spread). Option 3: more rounds (about 4 min each): about 12 more warm rounds would be needed to bring the raw half-width near 3 s if the true difference is small; costs aac7 time, which is scarce.
- 576 nodes (modelled, L4, fabric assumed): the L4 model puts the standing `2,4,8,16,32,64,192,576` at 176.3 s (3.71e13) and finds no explicit schedule better by more than 0.2 s, so the 6-node outcome does not change the 576-node default. The L4 model also says `[2,6]` is 9 s faster than `[6]` at 6 nodes; O1 does not confirm the opposite sign either, so the 6-node result neither supports nor refutes the model's tree-level terms. The unmodelled expected-maximum-over-groups term (L4 3.3) is not shown to matter in warm group-B runs. A single-level form is not applicable at 576 nodes (a 576-way level), so this test says little about the large-scale schedule.
- Suggested: Option 1 (keep default), no further aac7 time unless the group question matters for another reason.

#### RESUME
Done: results pasted above (inconclusive). Next (coordinator): add a short RESULTS.md section, update TASKS item 42, user decides the 6-node default. Files: tools/o1_item.sh, tools/o1_stats.py, results/GRID_GROUPS.md §O1; aac7: `~/o1/`, `~/ntt-wt/o1` (detached worktree at 0298fe2; remove with `git -C ~/ntt-acc worktree remove --force ~/ntt-wt/o1` after collection), `~/o1_build.log`, `~/o1_item.sh.new`.

## C09: best MN_GROUPS for every usable node count (N = 576 .. 544), 3.71e13 and 3.79e13 digits

(former file: C09.md)

Wave C, agent c09, 2026-10-11. Labels: all times and GB are **modelled** (standing model, `ecalc/estimate.py` / `mn_model.py`); "plan OK" is **measured** (the C planner, aac6 MI250 CPU node). No GPU, no aac7. Times Eastern.

#### Method
- Tool: `tools/groups_table.py` (`search`, `plancheck`, `maxdigits`, `table`; see its docstring). Model line = the target line: profile `target` (layv factor 0.655), `MN_T_CHUNK_MB=2048` (shift chunk 1024), X2 (`COMM_LAYER_VSLOT_SHARE=1`), FIT, `ECALC_NP=auto`, `BS_ARENA_ROOM=0.16`, 2^31 cap, round 1024. Total digits fixed (3.71e13 or 3.79e13), so digits per node grow as N falls.
- Check: the T2048 model reproduces results/LAUNCH_DEFAULTS.md §B01 memory (576 nodes, 3.71e13: device 357.9 GB; margin 373.44 - (357.9 + 0.5 X2 + 2.1 rank 0) = 13.0 GB). Wall: no-write, **T2048** (183.7 s at 576, default schedule); B04's 189.1 s is the T1024 figure of the same model.
- Candidates per N (about 440, plus the code's default): power-of-two prefix 2..2^a (a = 0..8), then one further level (ratio <= 12) or two (ratio <= 8), then N as an uneven last group. Memory rule: margin >= 10 GB (device + 0.5 + 2.1 vs 373.44) and node total <= 480 GB.
- Plan check (measured): `MN_PLAN_ONLY=<digits>:<N> MN_GROUPS=<g>` with the target line, aac6 MI250 compute node, host binary `ecalc` built there (hipcc on MI250 nodes; the login node has none). **All 224 top candidates passed `plan check ... OK`** (4 jobs, 90 s each; raw files in `archive:results_raw_2026-10-19/c09_raw/`).

#### Findings
1. **Memory, not time, limits how many nodes can be excluded.** At a fixed 3.71e13 digits the 10 GB margin holds down to N = 554 (23 counts, 576..554; N = 553..544 all fail: margin 4.0 GB or less, arena step). At 3.79e13 only N = 576..569 and 558 pass; 568..559 and 557..544 fail. The size-vs-N limit is a staircase (the arena step), so a bigger N does not always have more room; `maxdigits` column gives the largest size (0.01e13 steps) that keeps 10 GB at N with the default schedule (non-monotone: do not read it as a smooth curve).
2. **The default schedule is bad at almost every N != 576** (modelled 195-2500 s). The two best explicit forms:
   - *standing form* `2,4,8,16,32,64,192,N` (B04's): 184-211 s at 3.71e13 for N = 576..566 (column "std form").
   - *model-best*: an uneven top, e.g. `2,4,8,16,80,560,N` (children 560 nodes + N-560; for 576 that is a 560-node group plus a 16-node group): **138-155 s at N = 576..570, 127 s at 569** (3.71e13). It is 13-21 % below the standing form, **even at 576** (146.5 s vs 183.7 s with the default `2,4,8,16,32,64,192,576`).
3. **Doubt (important): the model-best forms are extrapolations.** The model has no measurement of 5-way or 7-way levels, of 80/560-node groups or of a top level whose children differ by 35x; it rewards fewer levels and unbalanced tops (fewer pieces: the plan shows pieces skipped). The planner accepts them (measured) but no run exists (b06 tests uneven groups on aac7). Columns "next-best" and "balanced-top best" (largest/smallest child of the top level <= 4, e.g. `5,25,175,576`: 147.3 s at 576) show the alternatives; even these balanced multi-way forms are below the standing form because of the fewer-level effect. **Until b06's measurements are in, the safe choice is the standing form (std form column), whose structure (pow2 prefix, 192, N) matches B04's tested 'uneven last group' idea; the model-best is the upside.**
4. **Round down?** The modelled wall is not monotone in N, even for the standing form: 3.71e13, std form 183.7 s (576), 211.3 (575), 194.2 (573), 184.4 (570), 177.3 (569), 166.7 (561), 175.5 (554). So with the standing form some smaller N is modelled 6-17 s faster than 576 (569 and 561 notably), and with the model-best form 569 (127.3 s) beats 576 (146.5 s) by 19 s. The CSV/table "advice" column names the fastest feasible smaller N. Part of this is real structure (fewer pieces at N where the group sizes fit the plane cap), part is the extrapolation of finding 3; act on it only after measurement. Going down also costs digits per node (memory margin falls 0.3 GB per 6 nodes at 3.71e13).
5. N = 575 (b04): the explicit `2,4,8,16,32,64,192,575` gives 211.3 s here (T2048; B04: 216.6 s with T1024), default 245.4 s (B04: 252.8).

#### Tables (modelled; "std form" = `2,4,8,16,32,64,192,N`; "default" = the code's schedule; "max size at N" = largest total digits with >= 10 GB margin, default schedule)

##### 3.71e13 digits

| N | best MN_GROUPS | wall s | margin GB | next-best (s) | std form 2,..,64,192,N s | default s | max size at N (default sched) | advice |
|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,80,560,576` | 146.5 | 12.7 | `2,4,8,16,112,560,576` (146.5) | 183.7 | 183.7 | 3.79e+13 | round down to 569 (127.3 s, -19.2 s) |
| 575 | `2,4,8,16,80,560,575` | 155.0 | 12.7 | `2,4,8,16,112,560,575` (155.0) | 211.3 | 245.4 | 3.73e+13 | round down to 569 (127.3 s, -27.7 s) |
| 574 | `2,4,8,16,80,560,574` | 149.6 | 12.7 | `2,4,8,16,112,560,574` (149.6) | 206.7 | 301.2 | 3.85e+13 | round down to 569 (127.3 s, -22.3 s) |
| 573 | `2,4,8,16,80,560,573` | 140.6 | 12.7 | `2,4,8,16,112,560,573` (140.6) | 194.2 | 854.4 | 3.74e+13 | round down to 569 (127.3 s, -13.3 s) |
| 572 | `2,4,8,16,80,560,572` | 143.4 | 12.7 | `2,4,8,16,112,560,572` (143.4) | 197.8 | 219.4 | 3.73e+13 | round down to 569 (127.3 s, -16.1 s) |
| 571 | `2,4,8,16,80,560,571` | 138.1 | 12.7 | `2,4,8,16,112,560,571` (138.1) | 188.9 | 2484.5 | 3.72e+13 | round down to 569 (127.3 s, -10.8 s) |
| 570 | `2,4,8,16,80,560,570` | 132.8 | 12.7 | `2,4,8,16,112,560,570` (132.8) | 184.4 | 232.3 | 3.7e+13 | round down to 569 (127.3 s, -5.5 s) |
| 569 | `2,4,8,16,80,560,569` | 127.3 | 12.3 | `2,4,8,16,112,560,569` (127.3) | 177.3 | 2482.5 | 3.7e+13 | - |
| 568 | `2,4,8,16,80,560,568` | 140.4 | 11.9 | `2,4,8,16,112,560,568` (140.4) | 188.6 | 401.2 | 3.72e+13 | round down to 566 (137.1 s, -3.3 s) |
| 567 | `2,4,8,16,80,560,567` | 143.3 | 11.6 | `2,4,8,16,112,560,567` (143.3) | 192.2 | 208.6 | 3.76e+13 | round down to 566 (137.1 s, -6.2 s) |
| 566 | `2,4,8,16,80,560,566` | 137.1 | 11.2 | `2,4,8,16,112,560,566` (137.1) | 183.9 | 1229.1 | 3.69e+13 | - |
| 565 | `2,4,8,16,80,400,565` | 144.8 | 10.3 | `2,4,8,16,80,160,565` (146.6) | 174.6 | 564.9 | 3.69e+13 | round down to 561 (141.6 s, -3.2 s) |
| 564 | `2,4,8,16,80,160,564` | 153.5 | 11.8 | `2,4,8,16,112,336,564` (153.7) | 179.3 | 322.5 | 3.81e+13 | round down to 561 (141.6 s, -11.9 s) |
| 563 | `2,4,8,16,80,160,563` | 150.3 | 11.5 | `2,4,8,16,112,336,563` (150.4) | 176.6 | 2464.4 | 3.67e+13 | round down to 561 (141.6 s, -8.7 s) |
| 562 | `2,4,8,16,80,160,562` | 147.2 | 11.1 | `2,4,8,16,112,336,562` (147.4) | 174.0 | 1227.9 | 3.66e+13 | round down to 561 (141.6 s, -5.6 s) |
| 561 | `2,4,8,16,112,336,561` | 141.6 | 10.3 | `2,4,8,16,80,160,561` (141.9) | 166.7 | 241.0 | 3.64e+13 | - |
| 560 | `3,18,144,560` | 170.0 | 11.2 | `6,18,144,560` (170.0) | 184.3 | 195.1 | 3.71e+13 | - |
| 559 | `3,18,144,559` | 170.9 | 10.8 | `6,18,144,559` (170.9) | 199.5 | 335.2 | 3.68e+13 | - |
| 558 | `5,25,150,558` | 186.2 | 10.1 | `2,4,24,144,558` (189.6) | 193.4 | 284.4 | 3.79e+13 | round down to 557 (177.2 s, -9.0 s) |
| 557 | `2,4,24,144,557` | 177.2 | 10.1 | `2,4,8,24,144,557` (177.2) | 179.2 | 2452.1 | 3.64e+13 | - |
| 556 | `2,4,8,16,112,556` | 202.1 | 11.2 | `7,14,112,556` (202.8) | 188.3 | 673.9 | 3.63e+13 | round down to 554 (181.8 s, -20.3 s) |
| 555 | `7,14,112,555` | 190.3 | 10.6 | `2,4,8,16,112,555` (190.6) | 176.8 | 310.1 | 3.76e+13 | round down to 554 (181.8 s, -8.5 s) |
| 554 | `7,14,112,554` | 181.8 | 10.2 | `7,28,112,554` (183.9) | 175.5 | 1227.0 | 3.61e+13 | - |
| 553 | infeasible | - | - | - | 164.5 | 454.2 | 3.62e+13 | memory margin < 10 GB |
| 552 | infeasible | - | - | - | 187.6 | 262.0 | 3.66e+13 | memory margin < 10 GB |
| 551 | infeasible | - | - | - | 180.4 | 304.1 | 3.6e+13 | memory margin < 10 GB |
| 550 | infeasible | - | - | - | 177.6 | 224.1 | 3.6e+13 | memory margin < 10 GB |
| 549 | infeasible | - | - | - | 165.3 | 389.8 | 3.6e+13 | memory margin < 10 GB |
| 548 | infeasible | - | - | - | 176.1 | 674.9 | 3.58e+13 | memory margin < 10 GB |
| 547 | infeasible | - | - | - | 169.1 | 2432.4 | 3.64e+13 | memory margin < 10 GB |
| 546 | infeasible | - | - | - | 166.9 | 236.4 | 3.56e+13 | memory margin < 10 GB |
| 545 | infeasible | - | - | - | 156.7 | 569.4 | 3.56e+13 | memory margin < 10 GB |
| 544 | infeasible | - | - | - | 202.0 | 242.2 | 3.54e+13 | memory margin < 10 GB |

##### 3.79e13 digits

| N | best MN_GROUPS | wall s | margin GB | next-best (s) | std form 2,..,64,192,N s | default s | max size at N (default sched) | advice |
|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,80,560,576` | 153.0 | 10.6 | `2,4,8,16,112,560,576` (153.0) | 188.5 | 188.5 | 3.79e+13 | - |
| 575 | `7,21,168,575` | 162.6 | 11.3 | `2,4,8,16,80,560,575` (165.6) | 220.3 | 252.2 | 3.73e+13 | - |
| 574 | `3,21,168,574` | 173.8 | 10.8 | `5,30,120,574` (174.5) | 211.6 | 309.3 | 3.85e+13 | round down to 571 (162.7 s, -11.1 s) |
| 573 | `2,4,8,56,168,573` | 166.6 | 10.4 | `2,4,8,16,80,160,573` (167.5) | 199.5 | 871.3 | 3.74e+13 | round down to 571 (162.7 s, -3.9 s) |
| 572 | `5,30,120,572` | 168.5 | 11.2 | `6,30,120,572` (168.5) | 203.1 | 226.9 | 3.73e+13 | round down to 571 (162.7 s, -5.8 s) |
| 571 | `5,30,120,571` | 162.7 | 10.8 | `6,30,120,571` (162.7) | 200.9 | 2503.7 | 3.72e+13 | - |
| 570 | `2,4,8,40,120,570` | 177.1 | 10.4 | `2,4,8,24,120,570` (183.7) | 195.5 | 257.5 | 3.7e+13 | round down to 569 (172.6 s, -4.5 s) |
| 569 | `3,21,147,569` | 172.6 | 10.2 | `7,21,147,569` (172.6) | 185.1 | 2507.0 | 3.7e+13 | - |
| 568 | infeasible | - | - | - | 206.1 | 420.5 | 3.72e+13 | memory margin < 10 GB |
| 567 | infeasible | - | - | - | 199.8 | 227.5 | 3.76e+13 | memory margin < 10 GB |
| 566 | infeasible | - | - | - | 194.9 | 1249.3 | 3.69e+13 | memory margin < 10 GB |
| 565 | infeasible | - | - | - | 182.9 | 580.6 | 3.69e+13 | memory margin < 10 GB |
| 564 | infeasible | - | - | - | 193.9 | 343.2 | 3.81e+13 | memory margin < 10 GB |
| 563 | infeasible | - | - | - | 184.4 | 2491.6 | 3.67e+13 | memory margin < 10 GB |
| 562 | infeasible | - | - | - | 182.7 | 1250.4 | 3.66e+13 | memory margin < 10 GB |
| 561 | infeasible | - | - | - | 171.8 | 258.4 | 3.64e+13 | memory margin < 10 GB |
| 560 | infeasible | - | - | - | 205.4 | 219.7 | 3.71e+13 | memory margin < 10 GB |
| 559 | infeasible | - | - | - | 204.9 | 343.3 | 3.68e+13 | memory margin < 10 GB |
| 558 | `default` | 292.1 | 11.7 | `` () | 198.8 | 292.1 | 3.79e+13 | - |
| 557 | infeasible | - | - | - | 187.5 | 2476.8 | 3.64e+13 | memory margin < 10 GB |
| 556 | infeasible | - | - | - | 193.7 | 689.2 | 3.63e+13 | memory margin < 10 GB |
| 555 | infeasible | - | - | - | 182.2 | 317.7 | 3.76e+13 | memory margin < 10 GB |
| 554 | infeasible | - | - | - | 180.8 | 1241.8 | 3.61e+13 | memory margin < 10 GB |
| 553 | infeasible | - | - | - | 170.2 | 467.9 | 3.62e+13 | memory margin < 10 GB |
| 552 | infeasible | - | - | - | 193.2 | 269.5 | 3.66e+13 | memory margin < 10 GB |
| 551 | infeasible | - | - | - | 186.3 | 315.8 | 3.6e+13 | memory margin < 10 GB |
| 550 | infeasible | - | - | - | 183.5 | 232.6 | 3.6e+13 | memory margin < 10 GB |
| 549 | infeasible | - | - | - | 171.1 | 400.1 | 3.6e+13 | memory margin < 10 GB |
| 548 | infeasible | - | - | - | 181.7 | 688.2 | 3.58e+13 | memory margin < 10 GB |
| 547 | infeasible | - | - | - | 175.0 | 2461.2 | 3.64e+13 | memory margin < 10 GB |
| 546 | infeasible | - | - | - | 172.8 | 244.4 | 3.56e+13 | memory margin < 10 GB |
| 545 | infeasible | - | - | - | 163.4 | 582.5 | 3.56e+13 | memory margin < 10 GB |
| 544 | infeasible | - | - | - | 207.9 | 249.6 | 3.54e+13 | memory margin < 10 GB |

Full machine-readable table for the kit: `archive:results_raw_2026-10-19/C09_groups_table.csv` (columns: digits, N, best_MN_GROUPS, wall_s, margin_gb, plan_check, next_MN_GROUPS, next_wall_s, balanced_MN_GROUPS, balanced_wall_s, default_wall_s, std_form_wall_s, std_form_margin_gb, round_down_to, round_down_wall_s, advice, max_digits_at_N_default_sched). INFEASIBLE rows have no schedule.

#### Reproduce
```
python3 tools/groups_table.py search --nmin 544 --nmax 576 --jobs 30 --out OUT      # about 90 s per quarter on a 32-core MI250 node; 4 h CPU on a loaded 4-core workstation
python3 tools/groups_table.py plancheck --cmds OUT/plan_cmds.txt --ecalc ecalc/ecalc --jobs 16 --out OUT/plan_results.txt --env ECALC_NP=auto MN_T_CHUNK_MB=2048 RNS_DIST_CACHE_FIT=1 BS_ARENA_ROOM=0.16 ECALC_NODE_GB=480 COMM_LAYER_INTER2=1 COMM_LAYER_VSLOT_POOL=1 COMM_LAYER_VSLOT_SHARE=1 COMM_SHMEM_ROUND_MB=1024
python3 tools/groups_table.py maxdigits --jobs 30 --out OUT/maxdigits.csv
python3 tools/groups_table.py table --out OUT --plan OUT/plan_results.txt
```
(The run used four jobs, N ranges 544-551, 552-559, 560-567, 568-576, then merged into `archive:results_raw_2026-10-19/c09_raw/`.)

#### Open issues
- Model-best uneven forms are unmeasured (finding 3). Suggested aac7 check for b06: 6-8 nodes, `MN_GROUPS` of the standing-form kind versus an unbalanced-top kind at the same size, paired ABBA.
- The search space is bounded (<= 3 levels beyond the pow2 prefix, ratio caps); a wider search may find better. Model memory depends weakly on the schedule (device 357.9-358.2 GB at 576).
- Memory margins use the model (+0.5 GB X2, +2.1 GB rank 0); the C-layout of the picked schedules was not run (the plan check does not report the device figure).
- The time model uses T2048; at T1024 every wall is a few seconds higher (576 default: 189.1 s).

#### aac7 test of the structural claim (approved 2026-10-11; queue item `~/aac7q/queue/48_c09_sched6.sh`)
Question: does "fewer levels / unbalanced top is faster" (finding 2/3) hold on a real run? aac7 can run 6 nodes; an uneven top cannot be tested there (6 nodes: the planner's groups are 2,6 / 6 / 3,6), so this tests only the fewer-levels effect.
Setup: 6 nodes, 6.441e10 digits per node (3.8646e11), no part-file write, production line (COMM_TRANSPORT=shmem, T2048, X2, FIT, COMM_XSTATS=1), `~/ntt-acc` main build (bc80001). A = default `2,6`, B = `6`, C = `3,6`; order ABC, CBA, ABC ... until 27 min are used (about 4-5 runs of ~5.5 min: expect each schedule 1-2 times). `plan check ... OK` for A, B, C (measured, aac7 login node, same line + BS_ARENA_ROOM=0.16; SHMEM pool 38656 MB for A and B, 39936 MB for C).

**Model predictions, written before the run (modelled, no write, T2048):**
| schedule | pieces | profile aac7_s48 (the fit for this cluster) | profile target (assumed fabric) |
|---|---|---|---|
| A `2,6` (default) | 75 | **292.1 s** | 124.6 s |
| B `6` | 77 | 308.4 s (+16.3) | 126.2 s (+1.6) |
| C `3,6` | 79 | 308.1 s (+16.0) | 127.5 s (+2.9) |
So at 6 nodes the model does NOT reward fewer levels: it predicts the default fastest, B and C about 5.5 % slower on aac7 (the layv penalty on the 6-way top level is the same in all three; B and C lose the cheap 2-way level / get larger pieces). The test therefore checks the model's ranking, not the 576-node gain directly.

**Pass rule (applied to the totals in `run6/totals.tsv`; the xstats logs go to calib_table.py to rate-adjust for the NIC slow mode):**
- Model ranking reproduced: mean(A) < mean(B) and mean(A) < mean(C), each gap at least 8 s (half the predicted 16 s) after rate adjustment; with one run per schedule only the sign is read (a single pair is never a result).
- Refuted: B or C is faster than A by 8 s or more (then fewer levels is really cheaper than the model says; the model-best 576 forms gain credibility but still untested for the uneven top).
- Inconclusive: all gaps under 8 s, or runs lost to a fabric slow mode.

#### Collect
`ssh aac7 'cat ~/sess1011c/c09/run6/totals.tsv; tail -5 ~/sess1011c/c09/run6/drv.log; ls ~/sess1011c/c09/run6/DONE'` then rate-adjust with calib_table.py on `r*_*.log` (COMM_XSTATS lines). Item exits by itself; hold released by the daemon.

#### RESUME
Pending: the aac7 item above (queue 48_c09_sched6; outputs `~/sess1011c/c09/run6/`, driver `~/sess1011c/c09/drv6.sh`). On results: add the totals table and pass-rule verdict under "aac7 test", then update the advice in finding 3.
Branch c09 (worktree ../ntt-wt-c09): tools/groups_table.py, results/GRID_GROUPS.md §C09, archive:results_raw_2026-10-19/C09_groups_table.csv, archive:results_raw_2026-10-19/c09_raw/. aac6: `~/ntt-c09` (clone with built `ecalc`), `~/c09_out_*`, `~/c09run.sh`; remove when done.

#### Single-level grouping, paired confirmation (f5 collected 2026-10-11 21:05 ET; runs by f3, queue items 80-83, 16:15-17:51 ET)
Setup (measured): 6.441e10 digits/node, no digit write, production line + COMM_XSTATS; arm A = default grouping, arm B = a single level (`MN_GROUPS=6` at 6 nodes, `MN_GROUPS=4` at 4 nodes); two ABBA/BAAB sets per size (`single6`, `single6b`, `single4`, `single4b` under aac7 `~/sess1011c/f3/`); NIC launch group (fgfc, A or B) recorded per run. The first run of each 6-node set was a cold start (init 56.5 s and 44.4 s, against 27-29 s) and is listed but left out of the "clean" means. The generic fit of `calib_table.py` could not be used (every run carries the chain-spike flag and the sets have one config each), so the rate adjustment is by inspection of the per-run all-to-all rate r (GB/s per APU thread).
Pass rule used (C09 rule, reduced to two arms): the model is reproduced if A beats B by >= 8 s after rate adjustment; refuted if B beats A by >= 8 s; otherwise inconclusive.

| size | arm A (default) clean runs, s | arm B (single level) clean runs, s | mean A | mean B | B - A (SE) |
|---|---|---|---|---|---|
| 6 nodes | 223.6, 271.5, 258.0, 271.5 (304.4 cold) | 231.3, 234.3, 238.2, 252.8 (254.1 cold) | 256.2 | 239.2 | **-17.0 s** (all 5+5 runs: -23.7, SE 13.9) |
| 4 nodes | 210.1, 183.0, 193.7, 207.7, 211.6 | 209.8, 202.4, 181.3, 233.7, 205.7, 208.8 | 201.2 | 206.9 | **+5.7 s** (SE 8.8) |

- Rate (measured): 6-node r is 9.4-10.5 GB/s in every run, mean 10.1 (A) against 9.6 (B); the slightly lower rate of B means the rate adjustment would widen the B advantage a little, not remove it. NIC group: 8 of 10 runs in group B (the fast one at 4 nodes); the only group-A runs are one in each arm (A 271.5, B 238.2).
- 1e10 correctness with `MN_GROUPS=6` (6 nodes, with digits): VERIFY OK and `digcmp` identical to the reference, in both 6-node sets (measured).
- 4 nodes: the NIC group split is uneven (B arm: 4 of 6 runs in the slow group A; A arm: 1 of 5), which alone is worth about 20 s (A13/L4), so the +5.7 s is confounded; group-matched pairs are 4 (A) against 2 (B) in group B (-7.7 s) and 1 against 4 in group A (+6.8 s): nothing can be read from it.
- **Verdict: 6 nodes - REFUTED in the model's direction, i.e. the model's ranking (default `2,6` fastest) is not reproduced; the single level `[6]` is faster by 17-24 s (>= the 8 s bar), with a large spread (A sd 29 s, B sd 11 s).** The sign is the same in 9 of 10 individual runs (every B run is below all but the fastest A run). 4 nodes - INCONCLUSIVE (gap 5.7 s < 8 s, NIC-group confounded).
- Agreement with L4 (`results/TIME_MODEL.md §L4`: 6 n `[6]` faster than `[2,6]` by 22.7 s, SE 11.8, from 148 pooled rows incl. these runs): yes - the f3 pairs alone give -17.0 (clean) / -23.7 s (all) with SE of the order 12-14 s, consistent with L4 and each other. They are not independent of L4 (L4 pooled them), so this is the same evidence seen twice, not a second confirmation. Significance: each estimate is about 1.7-2 SE from zero (p about 0.05-0.1, df small): a strong indication, not a closed case; the standing request is the overnight 2-h ABBA `[2,6]` vs `[6]` (TASKS).

## c09_groups

(former file: c09_raw/groups_table.md)

##### 3.71e+13 digits (modelled, no part-file write)

| N | best MN_GROUPS | wall s | margin GB | plan | next-best (wall s) | balanced-top best (wall s) | default (wall s) | std form 2,..,64,192,N (wall s / margin GB) | advice |
|---|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,80,560,576` | 146.5 | 12.7 | OK | `2,4,8,16,112,560,576` (146.5) | `5,25,175,576` (147.3) | 183.7 | 183.7 / 13.0 | round down to 569 (127.3 s, -19.2 s) |
| 575 | `2,4,8,16,80,560,575` | 155.0 | 12.7 | OK | `2,4,8,16,112,560,575` (155.0) | `7,56,392,575` (155.9) | 245.4 | 211.3 / 13.0 | round down to 569 (127.3 s, -27.7 s) |
| 574 | `2,4,8,16,80,560,574` | 149.6 | 12.7 | OK | `2,4,8,16,112,560,574` (149.6) | `5,30,240,574` (158.2) | 301.2 | 206.7 / 12.8 | round down to 569 (127.3 s, -22.3 s) |
| 573 | `2,4,8,16,80,560,573` | 140.6 | 12.7 | OK | `2,4,8,16,112,560,573` (140.6) | `2,4,8,16,112,448,573` (150.8) | 854.4 | 194.2 / 12.8 | round down to 569 (127.3 s, -13.3 s) |
| 572 | `2,4,8,16,80,560,572` | 143.4 | 12.7 | OK | `2,4,8,16,112,560,572` (143.4) | `5,30,240,572` (151.6) | 219.4 | 197.8 / 12.8 | round down to 569 (127.3 s, -16.1 s) |
| 571 | `2,4,8,16,80,560,571` | 138.1 | 12.7 | OK | `2,4,8,16,112,560,571` (138.1) | `2,4,8,16,112,448,571` (146.3) | 2484.5 | 188.9 / 12.8 | round down to 569 (127.3 s, -10.8 s) |
| 570 | `2,4,8,16,80,560,570` | 132.8 | 12.7 | OK | `2,4,8,16,112,560,570` (132.8) | `2,4,8,16,112,448,570` (141.1) | 232.3 | 184.4 / 12.5 | round down to 569 (127.3 s, -5.5 s) |
| 569 | `2,4,8,16,80,560,569` | 127.3 | 12.3 | OK | `2,4,8,16,112,560,569` (127.3) | `2,4,8,16,112,448,569` (135.6) | 2482.5 | 177.3 / 12.2 | - |
| 568 | `2,4,8,16,80,560,568` | 140.4 | 11.9 | OK | `2,4,8,16,112,560,568` (140.4) | `3,21,168,568` (146.6) | 401.2 | 188.6 / 11.8 | round down to 566 (137.1 s, -3.3 s) |
| 567 | `2,4,8,16,80,560,567` | 143.3 | 11.6 | OK | `2,4,8,16,112,560,567` (143.3) | `2,4,8,16,80,400,567` (160.8) | 208.6 | 192.2 / 11.4 | round down to 566 (137.1 s, -6.2 s) |
| 566 | `2,4,8,16,80,560,566` | 137.1 | 11.2 | OK | `2,4,8,16,112,560,566` (137.1) | `2,4,8,16,80,400,566` (154.7) | 1229.1 | 183.9 / 11.1 | - |
| 565 | `2,4,8,16,80,400,565` | 144.8 | 10.3 | OK | `2,4,8,16,80,160,565` (146.6) | `2,4,8,16,80,400,565` (144.8) | 564.9 | 174.6 / 10.7 | round down to 561 (141.6 s, -3.2 s) |
| 564 | `2,4,8,16,80,160,564` | 153.5 | 11.8 | OK | `2,4,8,16,112,336,564` (153.7) | `2,4,8,16,80,160,564` (153.5) | 322.5 | 179.3 / 10.2 | round down to 561 (141.6 s, -11.9 s) |
| 563 | `2,4,8,16,80,160,563` | 150.3 | 11.5 | OK | `2,4,8,16,112,336,563` (150.4) | `2,4,8,16,80,160,563` (150.3) | 2464.4 | 176.6 / 4.1 | round down to 561 (141.6 s, -8.7 s) |
| 562 | `2,4,8,16,80,160,562` | 147.2 | 11.1 | OK | `2,4,8,16,112,336,562` (147.4) | `2,4,8,16,80,160,562` (147.2) | 1227.9 | 174.0 / 4.1 | round down to 561 (141.6 s, -5.6 s) |
| 561 | `2,4,8,16,112,336,561` | 141.6 | 10.3 | OK | `2,4,8,16,80,160,561` (141.9) | `2,4,8,16,112,336,561` (141.6) | 241.0 | 166.7 / 3.9 | - |
| 560 | `3,18,144,560` | 170.0 | 11.2 | OK | `6,18,144,560` (170.0) | `3,18,144,560` (170.0) | 195.1 | 184.3 / 4.1 | - |
| 559 | `3,18,144,559` | 170.9 | 10.8 | OK | `6,18,144,559` (170.9) | `3,18,144,559` (170.9) | 335.2 | 199.5 / 4.1 | - |
| 558 | `5,25,150,558` | 186.2 | 10.1 | OK | `2,4,24,144,558` (189.6) | `5,25,150,558` (186.2) | 284.4 | 193.4 / 4.1 | round down to 557 (177.2 s, -9.0 s) |
| 557 | `2,4,24,144,557` | 177.2 | 10.1 | OK | `2,4,8,24,144,557` (177.2) | `2,4,24,144,557` (177.2) | 2452.1 | 179.2 / 4.1 | - |
| 556 | `2,4,8,16,112,556` | 202.1 | 11.2 | OK | `7,14,112,556` (202.8) | `2,4,8,16,112,556` (202.1) | 673.9 | 188.3 / 4.1 | round down to 554 (181.8 s, -20.3 s) |
| 555 | `7,14,112,555` | 190.3 | 10.6 | OK | `2,4,8,16,112,555` (190.6) | `7,14,112,555` (190.3) | 310.1 | 176.8 / 4.0 | round down to 554 (181.8 s, -8.5 s) |
| 554 | `7,14,112,554` | 181.8 | 10.2 | OK | `7,28,112,554` (183.9) | `7,14,112,554` (181.8) | 1227.0 | 175.5 / 4.0 | - |
| 553 | infeasible (memory) | - | - | - | - | - | 454.2 | 164.5 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,553 (141.2 s, margin -4.6 GB); largest size with margin >= 10 GB at N: 3.62e+13 (default sched); use 569 instead (127.3 s) |
| 552 | infeasible (memory) | - | - | - | - | - | 262.0 | 187.6 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,25,175,552 (149.5 s, margin -4.6 GB); largest size with margin >= 10 GB at N: 3.66e+13 (default sched); use 569 instead (127.3 s) |
| 551 | infeasible (memory) | - | - | - | - | - | 304.1 | 180.4 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,25,175,551 (140.9 s, margin -4.6 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 569 instead (127.3 s) |
| 550 | infeasible (memory) | - | - | - | - | - | 224.1 | 177.6 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 9,108,550 (149.9 s, margin -4.6 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 569 instead (127.3 s) |
| 549 | infeasible (memory) | - | - | - | - | - | 389.8 | 165.3 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,549 (141.5 s, margin -4.6 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 569 instead (127.3 s) |
| 548 | infeasible (memory) | - | - | - | - | - | 674.9 | 176.1 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 6,30,180,548 (148.5 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.58e+13 (default sched); use 569 instead (127.3 s) |
| 547 | infeasible (memory) | - | - | - | - | - | 2432.4 | 169.1 / 4.0 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 6,42,252,547 (140.4 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.64e+13 (default sched); use 569 instead (127.3 s) |
| 546 | infeasible (memory) | - | - | - | - | - | 236.4 | 166.9 / 3.8 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,30,180,546 (140.4 s, margin -13.3 GB); largest size with margin >= 10 GB at N: 3.56e+13 (default sched); use 569 instead (127.3 s) |
| 545 | infeasible (memory) | - | - | - | - | - | 569.4 | 156.7 / 3.4 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,545 (132.5 s, margin -13.5 GB); largest size with margin >= 10 GB at N: 3.56e+13 (default sched); use 569 instead (127.3 s) |
| 544 | infeasible (memory) | - | - | - | - | - | 242.2 | 202.0 / 3.2 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,30,180,544 (149.5 s, margin -13.3 GB); largest size with margin >= 10 GB at N: 3.54e+13 (default sched); use 569 instead (127.3 s) |

##### 3.79e+13 digits (modelled, no part-file write)

| N | best MN_GROUPS | wall s | margin GB | plan | next-best (wall s) | balanced-top best (wall s) | default (wall s) | std form 2,..,64,192,N (wall s / margin GB) | advice |
|---|---|---|---|---|---|---|---|---|---|
| 576 | `2,4,8,16,80,560,576` | 153.0 | 10.6 | OK | `2,4,8,16,112,560,576` (153.0) | `7,56,168,576` (161.4) | 188.5 | 188.5 / 10.6 | - |
| 575 | `7,21,168,575` | 162.6 | 11.3 | OK | `2,4,8,16,80,560,575` (165.6) | `7,21,168,575` (162.6) | 252.2 | 220.3 / 4.4 | - |
| 574 | `3,21,168,574` | 173.8 | 10.8 | OK | `5,30,120,574` (174.5) | `3,21,168,574` (173.8) | 309.3 | 211.6 / 4.2 | round down to 571 (162.7 s, -11.1 s) |
| 573 | `2,4,8,56,168,573` | 166.6 | 10.4 | OK | `2,4,8,16,80,160,573` (167.5) | `2,4,8,56,168,573` (166.6) | 871.3 | 199.5 / 4.2 | round down to 571 (162.7 s, -3.9 s) |
| 572 | `5,30,120,572` | 168.5 | 11.2 | OK | `6,30,120,572` (168.5) | `5,30,120,572` (168.5) | 226.9 | 203.1 / 4.2 | round down to 571 (162.7 s, -5.8 s) |
| 571 | `5,30,120,571` | 162.7 | 10.8 | OK | `6,30,120,571` (162.7) | `5,30,120,571` (162.7) | 2503.7 | 200.9 / 4.2 | - |
| 570 | `2,4,8,40,120,570` | 177.1 | 10.4 | OK | `2,4,8,24,120,570` (183.7) | `2,4,8,40,120,570` (177.1) | 257.5 | 195.5 / 4.2 | round down to 569 (172.6 s, -4.5 s) |
| 569 | `3,21,147,569` | 172.6 | 10.2 | OK | `7,21,147,569` (172.6) | `3,21,147,569` (172.6) | 2507.0 | 185.1 / 4.2 | - |
| 568 | infeasible (memory) | - | - | - | - | - | 420.5 | 206.1 / 4.2 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 7,35,280,568 (142.2 s, margin -21.7 GB); largest size with margin >= 10 GB at N: 3.72e+13 (default sched); use 576 instead (153.0 s) |
| 567 | infeasible (memory) | - | - | - | - | - | 227.5 | 199.8 / 4.2 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,567 (150.8 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.76e+13 (default sched); use 576 instead (153.0 s) |
| 566 | infeasible (memory) | - | - | - | - | - | 1249.3 | 194.9 / 4.2 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,35,280,566 (143.3 s, margin -21.7 GB); largest size with margin >= 10 GB at N: 3.69e+13 (default sched); use 576 instead (153.0 s) |
| 565 | infeasible (memory) | - | - | - | - | - | 580.6 | 182.9 / 4.2 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,565 (135.3 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.69e+13 (default sched); use 576 instead (153.0 s) |
| 564 | infeasible (memory) | - | - | - | - | - | 343.2 | 193.9 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,564 (145.6 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.81e+13 (default sched); use 576 instead (153.0 s) |
| 563 | infeasible (memory) | - | - | - | - | - | 2491.6 | 184.4 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,563 (138.4 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.67e+13 (default sched); use 576 instead (153.0 s) |
| 562 | infeasible (memory) | - | - | - | - | - | 1250.4 | 182.7 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,562 (135.9 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.66e+13 (default sched); use 576 instead (153.0 s) |
| 561 | infeasible (memory) | - | - | - | - | - | 258.4 | 171.8 / 3.9 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,80,560,561 (127.1 s, margin 4.1 GB); largest size with margin >= 10 GB at N: 3.64e+13 (default sched); use 576 instead (153.0 s) |
| 560 | infeasible (memory) | - | - | - | - | - | 219.7 | 205.4 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 3,18,108,560 (162.4 s, margin -4.5 GB); largest size with margin >= 10 GB at N: 3.71e+13 (default sched); use 576 instead (153.0 s) |
| 559 | infeasible (memory) | - | - | - | - | - | 343.3 | 204.9 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 3,18,108,559 (153.9 s, margin -4.5 GB); largest size with margin >= 10 GB at N: 3.68e+13 (default sched); use 576 instead (153.0 s) |
| 558 | `default` | 292.1 | 11.7 | OK | - | `default` (292.1) | 292.1 | 198.8 / 4.1 | - |
| 557 | infeasible (memory) | - | - | - | - | - | 2476.8 | 187.5 / 4.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,64,512,557 (164.4 s, margin -13.1 GB); largest size with margin >= 10 GB at N: 3.64e+13 (default sched); use 576 instead (153.0 s) |
| 556 | infeasible (memory) | - | - | - | - | - | 689.2 | 193.7 / 3.8 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,30,180,556 (152.0 s, margin -13.1 GB); largest size with margin >= 10 GB at N: 3.63e+13 (default sched); use 576 instead (153.0 s) |
| 555 | infeasible (memory) | - | - | - | - | - | 317.7 | 182.2 / 3.3 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,555 (159.1 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.76e+13 (default sched); use 576 instead (153.0 s) |
| 554 | infeasible (memory) | - | - | - | - | - | 1241.8 | 180.8 / 2.9 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 7,42,252,554 (152.9 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.61e+13 (default sched); use 576 instead (153.0 s) |
| 553 | infeasible (memory) | - | - | - | - | - | 467.9 | 170.2 / 2.5 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,553 (146.9 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.62e+13 (default sched); use 576 instead (153.0 s) |
| 552 | infeasible (memory) | - | - | - | - | - | 269.5 | 193.2 / 2.1 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,25,175,552 (155.1 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.66e+13 (default sched); use 576 instead (153.0 s) |
| 551 | infeasible (memory) | - | - | - | - | - | 315.8 | 186.3 / -4.6 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,25,175,551 (146.7 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 576 instead (153.0 s) |
| 550 | infeasible (memory) | - | - | - | - | - | 232.6 | 183.5 / -4.6 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 9,108,550 (155.8 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 576 instead (153.0 s) |
| 549 | infeasible (memory) | - | - | - | - | - | 400.1 | 171.1 / -4.6 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,549 (147.4 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.6e+13 (default sched); use 576 instead (153.0 s) |
| 548 | infeasible (memory) | - | - | - | - | - | 688.2 | 181.7 / -4.6 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 6,42,252,548 (154.8 s, margin -13.2 GB); largest size with margin >= 10 GB at N: 3.58e+13 (default sched); use 576 instead (153.0 s) |
| 547 | infeasible (memory) | - | - | - | - | - | 2461.2 | 175.0 / -4.6 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 6,42,252,547 (146.0 s, margin -21.8 GB); largest size with margin >= 10 GB at N: 3.64e+13 (default sched); use 576 instead (153.0 s) |
| 546 | infeasible (memory) | - | - | - | - | - | 244.4 | 172.8 / -4.8 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,30,180,546 (147.8 s, margin -13.3 GB); largest size with margin >= 10 GB at N: 3.56e+13 (default sched); use 576 instead (153.0 s) |
| 545 | infeasible (memory) | - | - | - | - | - | 582.5 | 163.4 / -4.9 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 2,4,8,16,32,64,512,545 (138.9 s, margin -13.5 GB); largest size with margin >= 10 GB at N: 3.56e+13 (default sched); use 576 instead (153.0 s) |
| 544 | infeasible (memory) | - | - | - | - | - | 249.6 | 207.9 / -4.8 | no schedule keeps the 10 GB margin at this size; fastest ignoring memory: 5,30,180,544 (155.2 s, margin -21.9 GB); largest size with margin >= 10 GB at N: 3.54e+13 (default sched); use 576 instead (153.0 s) |

## C215: the paired series for D1 (`RNS_AUTO_PIECE_COST`, C2), with `DIST_TWREC`, other sizes, and the 576 model (Phase 15, row 1e, agent C2)

(former file: C215.md)

Branch `p15-C2` from int15 0b80ca3 (= B0), with no pipeline code changed. aac6 clone: `~/ntt-C215` (built at 0b80ca3). Scripts,
queue and logs: `~/C215/` on aac6. Copies of the scripts are in `archive:results_raw_2026-10-19/C215/`. Times are Eastern. Labels: **measured** (aac6,
node named), **modelled** (the code's plan printer `MN_PLAN_ONLY`, `mn_model.py` / `archive:results_raw_2026-10-19/C215/c2_grid.py`), **assumed**.

#### 0. Method

- `archive:results_raw_2026-10-19/C215/job.sh` runs one unattended 1-node job (`-J C2`, ≤ 45 min). It works through `queue.txt` until the next step
  would not fit the job. `chain.sh` waits for Stage 0 (`~/reg15b.done` with 3 lines, polled every 10 min), then runs jobs one
  after another. The first job takes any node, and every later job is pinned to that node with `-w` (`~/C215/node.lock`).
  Each job checks the code first: `git rev-parse HEAD:ecalc` must equal 0b80ca3's, and the binary must be newer than the
  source ("WRONG COMMIT" otherwise).
- Each run goes through `run1.sh`: `env <switches> ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc <D> [/tmp/c2_<tag>.txt]`. The process
  wall is timed around ecalc inside srun (the `C2WALL` line).
  - **Wall with the write**: the process wall of a run with a digit file, including the part file and the top set
    (`ECALC_CKPT_TOP`, on by default above 10¹⁰ when there is an outfile).
  - **Wall without the write**: the process wall of a run without an outfile (the digits are still formatted and checked by
    T2, but nothing is written).
  - `total` is ecalc's own line; at size 1 it ends before the writer is joined.
- **Digits**: after each file run, the file is hashed on the node with `dd iflag=direct bs=64M | sha1sum` and then deleted.
  The hashes are compared with `~/V214/e_1e11.sha1` (578f5efb…), with `e_4e10.out.sha1` (43c84e49…), and at 1.3 × 10¹¹ with the
  pair's own hashes (no reference exists at that size).
- ECALC_ODIRECT=1 (the default) is used, and the reference is never read on the node, so there is nothing to evict. The digits
  go to the node's local `/tmp`.
- **Order** (a single queue on one node): cycles of C2 off, C2 on and C2 on + `DIST_TWREC=1` (×3), then off/on (×2). This gives
  5 off, 5 on and 3 TWREC runs, all with the file. Then 3 off/on pairs without the file, one 4 × 10¹⁰ pair and one 1.3 × 10¹¹
  pair.

#### 4. The 576 target (done first: login node, no GPU)

**How the code decides.** C2 lives only in `rns_dist.c` `split_grid`, which is auto's grid for a node-local (`dist_db`) product.
The mn tier's distributed products use `split_grid_cap` with minpts (`mn_grid`), and strategy C uses `split_grid_cap` directly.
C2 changes neither. At 576 nodes, the only node-local grids are each node's **leaf mdev levels** (binsplit.c's device levels
above 2³⁰ limbs) and the reciprocal's single-node start chain (≤ 828 limbs, so one plane).

**`MN_PLAN_ONLY=42500000000000:576 ./ecalc`** (with `MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0`, off and then on):
**the two logs are byte-identical** (`~/C215/plan/t576_pc{0,1}.log`). This is **modelled**: it is the code's own plan. The
plan printer asks `db_grid_shape` → `split_grid`, so it does honour the switch; at 1 × 10¹¹ on 1 node it reproduces V2's
changes. It prints only node 0's leaf, though.

**Every node's leaf.** `archive:results_raw_2026-10-19/C215/c2_grid.py` is a Python port of `split_grid` that includes the per-piece term, patched into
`mn_model._split_auto` at run time (mn_model itself is unchanged). Two checks show it matches the code:
- at 1 × 10¹¹ on 1 node it reproduces V2's plan-only table exactly: A_h mu 7 × 7 (28) → 5 × 6 (20), X Q 7 × 7 (31) → 5 × 6 (20),
  recip Q_t r 9 × 3 → 5 × 3, leaf 25/26 3 × 3 → 2 × 3 and 8 × 1 → 5 × 1;
- node 0 at the target matches the plan printer (level 24 1 × 3 B 3·2²⁹, level 25 2 × 1 C 2³¹).

Over all 576 nodes' term ranges (`node r = [1 + N r / g, 1 + N (r+1) / g)`, mn_plan.c term0), **modelled**:

| | C2 off | C2 on |
|---|---|---|
| nodes whose leaf grids change | — | 311 of 576 (levels 24/25; e.g. node 288: level 25 3 × 2 B → 2 × 2 C 2³¹) |
| per-node leaf big-product time (pipeline law) | | −0.38 … +0.40 s per node |
| **the slowest leaf (node 575, the top node: level 24 2 × 2 B 3·2²⁹, level 25 2 × 2 C 2³¹)** | **17.97 s** | **17.97 s (unchanged)** |

Every tree level waits for its slowest group, and the leaf waits for the top node's. The top node's grids are the same with
and without C2, and no other node's leaf rises above it, so **the target's critical path does not change**.
`mn_model.run(TARGET, 4.25e13/576, 576)` with the patched grid gives **233.9 s → 233.9 s (modelled; 0.0 s)**. The model's own
top leaf (`leaf_shapes(D·ftop)`) is unchanged as well.

For comparison, the same patched model at size 1 (**modelled**, without corrections): 1 × 10¹¹ 223.8 → 217.1 s (−6.7 s), 1.16
× 10¹¹ 318.6 → 293.7 s, 1.3 × 10¹¹ 373.6 → 351.5 s, 4 × 10¹⁰ 65.4 → 64.9 s.

**On B0's model** (main 7367e25, where Stage 0.4 updated `mn_model.py` / `mem_model.py`; `archive:results_raw_2026-10-19/C215/c2_wall.py` run from an
extracted copy of that tree, design `DEFAULT15` = auto 2³¹ both d2 p15; `TARGET` now includes the Lustre write rate).
Everything is **modelled**:

| size | C2 off: wall (out exposed) | C2 on: wall (out exposed) | compute part (wall − out) |
|---|---|---|---|
| **4.25 × 10¹³ / 576** | **349.7 (123.0)** | **349.7 (123.0)** | **226.7 → 226.7 (0.0 s)** |
| 4 × 10¹⁰ / 1 | 126.4 (58.4) | 125.9 (58.8) | 68.0 → 67.1 |
| 1 × 10¹¹ / 1 | 348.6 (132.0) | 343.2 (133.9) | 216.6 → 209.3 (−7.3) |
| 1.16 × 10¹¹ / 1 | 433.8 (139.7) | 417.5 (149.3) | 294.1 → 268.2 |
| 1.3 × 10¹¹ / 1 | 493.4 (151.3) | 481.0 (163.8) | 342.1 → 317.2 |

On B0's model, 311 of 576 nodes change their leaf grids, by −0.52 … +0.24 s per node. The slowest leaf, node 575's, is
18.31 s → 18.31 s. At size 1 the model's exposed output grows by about as much as the division shrinks. This is the model's
rule, which hides the file write under half the division; on aac6's NVMe the measured walls decide (§1).

**Verdict for the target**: C2 is neutral at 4.25 × 10¹³ on 576 nodes (0 s modelled). Its gain is a one-node gain (≥ 4 × 10¹⁰
per node on one node, where the division and the reciprocal are node-local).

#### 1. The paired series at 10¹¹ (measured, s24-26, jobs 21467 / 21481 / 21493, 2026-09-26 23:34 – 2026-09-27 03:25 EDT)

All 23 runs were on **aac6-spx-b**, the ecalc tree was 0b80ca3's (checked by each job), and every run was VERIFY OK with
rc 0. **All 13 digit files at 10¹¹ are identical to `e_1e11.out`** (sha1 578f5efb…, hashed on the node, 101–103 s each).
Per run (`archive:results_raw_2026-10-19/C215/parse_out.txt`; the logs are in `~/C215/logs/`):

| run | total | init | bs | dm (recip) | corrections (down/up) | T1 | dc (writer wait) | wall with file | wall without file | sha1 |
|---|---|---|---|---|---|---|---|---|---|---|
| off 1 | 235.41 | 17.4 | 93.3 | 99.9 (41.5) | 0/2, redo | 24.49 | 59.5 (51.6) | 296.7 | | identical |
| on 1 | 204.76 | 17.3 | 93.6 | 93.5 (38.6) | 0/0 | 0.00 | 24.6 (40.4) | 231.1 | | identical |
| off 2 | 233.70 | 15.9 | 93.8 | 99.7 (41.3) | 0/2, redo | 23.97 | 59.6 (52.0) | 295.0 | | identical |
| on 2 | 203.70 | 17.8 | 92.2 | 93.3 (38.5) | 0/0 | 0.00 | 25.3 (41.1) | 230.7 | | identical |
| off 3 | 237.11 | 19.1 | 93.7 | 98.8 (41.0) | 0/2, redo | 25.15 | 60.3 (52.7) | 299.2 | | identical |
| on 3 | 201.51 | 15.5 | 92.2 | 93.6 (38.5) | 0/0 | 0.00 | 26.8 (41.9) | 230.1 | | identical |
| off 4 | 233.54 | 16.9 | 93.2 | 99.5 (41.3) | 0/2, redo | 23.64 | 60.0 (52.3) | 295.3 | | identical |
| on 4 | 203.93 | 16.8 | 93.6 | 93.2 (38.4) | 0/0 | 0.00 | 26.3 (41.6) | 232.0 | | identical |
| off 5 | 233.32 | 16.3 | 92.9 | 98.9 (41.2) | 0/2, redo | 24.93 | 60.0 (52.4) | 295.1 | | identical |
| on 5 | 203.41 | 17.6 | 92.3 | 93.1 (38.3) | 0/0 | 0.00 | 25.7 (41.7) | 231.0 | | identical |
| off, no file 1 | 211.65 | 17.9 | 93.2 | 99.6 (41.6) | 0/2, redo | 0.64 | 5.3 | | 218.6 | — |
| on, no file 1 | 201.78 | 16.1 | 92.2 | 93.1 (38.4) | 0/0 | 0.00 | 0.0 | | 203.4 | — |
| off, no file 2 | 210.78 | 16.4 | 93.7 | 99.7 (41.6) | 0/2, redo | 0.64 | 5.2 | | 217.6 | — |
| on, no file 2 | 202.21 | 16.5 | 92.2 | 93.2 (38.6) | 0/0 | 0.00 | 0.0 | | 203.9 | — |
| off, no file 3 | 210.37 | 17.7 | 92.5 | 99.2 (41.3) | 0/2, redo | 0.64 | 5.5 | | 217.5 | — |
| on, no file 3 | 204.40 | 18.5 | 92.8 | 92.8 (38.7) | 0/0 | 0.00 | 0.0 | | 206.1 | — |

(Wall = the ecalc process from start to exit. With the file it includes the 100 GB part file and the top set, both O_DIRECT
to the node's NVMe `/tmp`. "redo" = "X corrected after the formatting started: redoing the digits".)

**Series statistics (measured; mean ± σ; difference ± its standard error):**

| quantity | C2 off (n 5) | C2 on (n 5) | on − off |
|---|---|---|---|
| `total` (file runs) | 234.62 ± 1.62 | 203.46 ± 1.20 | **−31.15 ± 0.90 s (−13.3 %)** |
| wall with the file | 296.28 ± 1.78 | 230.99 ± 0.70 | **−65.29 ± 0.86 s (−22.0 %)** |
| bs | 93.36 ± 0.35 | 92.78 ± 0.75 | −0.59 ± 0.37 |
| dm | 99.35 ± 0.49 | 93.33 ± 0.22 | **−6.02 ± 0.24** |
| of which recip | 41.26 ± 0.18 | 38.46 ± 0.11 | −2.80 ± 0.10 |
| T1 | 24.44 ± 0.63 | 0.00 | −24.44 ± 0.28 |
| dc (after `total`) | 59.89 ± 0.35 | 25.75 ± 0.87 | −34.14 ± 0.42 |
| | **no file, off (n 3)** | **no file, on (n 3)** | |
| `total` | 210.93 ± 0.65 | 202.80 ± 1.41 | **−8.14 ± 0.89 s (−3.9 %)** |
| wall without the file | 217.91 ± 0.57 | 204.47 ± 1.41 | **−13.43 ± 0.88 s (−6.2 %)** |
| dm | 99.50 ± 0.25 | 93.05 ± 0.19 | −6.45 ± 0.18 |

**Grid gain versus correction effect.** Every C2-off run at 10¹¹ took the same **2 up-corrections**. The count is a
deterministic property of the digits and the grid, not run-to-run noise: A_h mu is 7 × 7 with 21 pieces skipped under the low
cut. Every C2-on run took none (5 × 6, 10 skipped). The −31 s therefore splits into two parts:

- **The grid itself** (independent of the corrections): dm −6.0 ± 0.2 s (recip −2.8, division −3.2) and bs −0.6 ± 0.4 s, so
  **≈ −6.6 s of `total` (−2.8 %)**. The no-file series agrees: `total` −8.1 ± 0.9, of which −6.45 dm, −0.7 bs and −0.64 T1 (the
  join in T1 when a correction came after the hook). The model predicted −7.3 s (§4). This part holds wherever C2 changes the
  grids.
- **The corrections**, specific to 10¹¹ and data-dependent:
  - in `total` with the file: T1's wait for the writer, −24.4 s;
  - after `total`: dc's redo of the digits and the file rewrite, −34.1 s.
  - Together, ≈ −58.7 of the −65.3 s in the wall with the file. Without the file, the redo costs only 5.3 s (formatting
    again, nothing to rewrite): wall −13.4 = grid ≈ −7 + corrections ≈ −6.

  Row 1d (agent K, a correction without a rewrite) would remove most of this part on the defaults too. C2 does not guarantee
  zero corrections at every size; it only has fewer low-cut skips.

**For D1 (measured):** C2 on is faster in every paired comparison at 10¹¹ (5/5 file pairs, 3/3 no-file pairs), and the digits
are identical in every run. The 240.3 s standard of §36.1 was measured on a different node (s24-16); here C2 off = 234.6 s on
s24-26.

#### 2. DIST_TWREC=1 together with C2 (measured, s24-26, interleaved within the series above)

| run | total | bs | dm (recip) | corrections | dc | wall with file | sha1 |
|---|---|---|---|---|---|---|---|
| C2 + TWREC 1 | 201.58 | 92.7 | 91.5 (37.8) | 0/0 | 26.7 | 230.1 | identical |
| C2 + TWREC 2 | 200.84 | 92.4 | 91.5 (37.7) | 0/0 | 27.7 | 230.3 | identical |
| C2 + TWREC 3 | 199.49 | 92.1 | 91.2 (37.6) | 0/0 | 26.7 | 228.0 | identical |

Compared with C2 alone (n 5 above), n 3: `total` 200.64 ± 1.06 vs 203.46 ± 1.20, **−2.83 ± 0.81 s (−1.4 %)**. dm is −1.96 ±
0.14 s (recip −0.76 ± 0.08), and bs is −0.37 ± 0.37. The wall with the file is −1.54 ± 0.80 s, because dc was +1.29 ± 0.51 s. All
three runs are identical to `e_1e11.out`. V2's expectation was "< 1 % on the defaults, more with C2" (C2 moves products into the
C form). The measured dm gain of ≈ 2 s is small but clear (≈ 14 σ). **Verdict: a candidate switch together with C2 (−1.4 % of
`total`), for the user; not required for D1.**

#### 3. Other sizes (measured, s24-26, one pair each)

| digits | switch | total | bs | dm (recip) | corrections | dc | wall with file | digits |
|---|---|---|---|---|---|---|---|---|
| 4 × 10¹⁰ | off | 65.25 | 31.2 | 22.8 (9.6) | 0/0 | 14.0 | 80.5 | 43c84e49… = `e_4e10.out`: identical |
| 4 × 10¹⁰ | on | 65.53 (+0.4 %) | 30.8 | 22.2 (9.4) | 0/0 | 15.0 | 81.7 | identical |
| 1.3 × 10¹¹ | off | 325.09 | 142.3 | 163.9 (59.5) | 0/0 | 20.3 | 347.4 | b1946e71…, VERIFY OK |
| 1.3 × 10¹¹ | on | **302.69 (−6.9 %)** | 136.0 | **147.8** (59.6) | 0/0 | 27.2 | **332.0 (−4.4 %)** | b1946e71… (the same as off, and the same as V2's pair) |

V2's trend is confirmed:
- At 4 × 10¹⁰ the switch is neutral. dm −0.6, total +0.3 s, which is within the 4 × 10¹⁰ spread of ±1.5 s; one pair is
  measured, so the difference is not resolved.
- At 1.3 × 10¹¹ it gains −22.4 s of `total` (V2: −20.9 s, −6.4 %) with no corrections on either side, so this is the grid gain
  alone: dm −16.1 s, bs −6.3 s.
- dc (after `total`) is +6.9 s with C2 at 1.3 × 10¹¹, as V2 saw (+3…7 s). The wall with the file therefore gains −15.4 s.
- The model predicted 342.1 → 317.2 (−24.9) for the compute part.

#### Conclusions

1. **D1: confirmed.** The paired five-run series at 10¹¹ on s24-26 gives:
   - `total` 234.62 ± 1.62 → 203.46 ± 1.20 s (**−31.2 ± 0.9 s, −13.3 %**);
   - the wall with the digit file 296.3 → 231.0 s (**−65.3 ± 0.9 s**);
   - the wall without it 217.9 → 204.5 s (**−13.4 ± 0.9 s**).

   All 13 file runs are identical to the reference. About 6.6 s of `total` is the grid (dm −6.0, bs −0.6); the rest is the two
   corrections that C2's grid avoids at this size.
2. **DIST_TWREC=1** with C2: −2.8 ± 0.8 s of `total` more, identical digits (a separate, optional adoption).
3. **Sizes:** neutral at 4 × 10¹⁰ (one pair); −6.9 % at 1.3 × 10¹¹ (grid only).
4. **576 target:** unchanged (0.0 s modelled; plan-only identical; the top node's leaf grids are the same with and without
   C2). Adoption is safe for the target and does not move the target estimate.

#### RESUME

- 2026-09-26 22:25 EDT: branch `p15-C2` @ 0b80ca3. Clone `~/ntt-C215` built on aac6 (bundle 19bf6dd..p15-C2). Item 4 is done
  (above).
- 22:32 EDT: `setsid nohup bash ~/C215/chain.sh any` is running on aac6 (log `~/C215/chain.log`, per-job driver
  `~/C215/driver.log`, run logs `~/C215/logs/<tag>.log|.sha`). It polls `~/reg15b.done` every 10 min, then runs the 23 steps
  of `~/C215/queue.txt`. `~/C215/done.txt` lists the finished tags; `~/C215/node.lock` holds the node. **To resume**: check
  `pgrep -af C215/chain.sh`; if the chain is not running, relaunch it the same way (it continues from done.txt on the locked
  node). `touch ~/C215/stop` stops it after the current job.
- 23:12 EDT: Stage 0 done (B0 = main 7367e25; its ecalc C/HIP tree = 0b80ca3's, only the Python models differ). The first chain found no `done.txt` and ended; relaunched 23:13 EDT, job 21467 pending. §4 redone on B0's model (c2_wall.py).
- Next: `python3 ~/C215/parse.py ~/C215/logs` on aac6 -> the tables of §1–3.
- 2026-09-26 23:34–00:10 EDT: job 21467 on **s24-26** (node locked) ran s_off1, s_on1, s_tw1, s_off2, s_on2 and s_tw2. All six
  were identical (578f5efb…) and VERIFY OK. The off runs had corrections 0/2 each with T1 ≈ 24 s; the on runs had none.
  00:10 EDT: job 21481 is pending on s24-26 behind the nightly CI (cdash-nightly holds all three nodes from ≈ 00:04 EDT).
- 01:38–02:15 EDT job 21481, 02:46–03:25 EDT job 21493, 03:25–03:33 EDT job 21499 (all on s24-26; each ended itself with its
  own `scancel`, and the chain ended at 03:33 EDT with 0 steps left). **All 23 steps are done.** The account's usage limit
  stopped the agent overnight, but not the jobs. 2026-09-27 ≈ 11:00 EDT: the logs were parsed and §1–3 and the conclusions
  written. **Done.** No job is running or pending, and `/tmp/c2_*` on s24-26 was removed by each job.

#### Report (protocol)

- **Branch** `p15-C2` from 0b80ca3. Commits: scripts and model, parse.py, item 4 on B0's model, chain fix, RESUMEs, and the final
  report. No pipeline code was changed, and nothing outside `results/C215*` was touched.
- **What passed** (measured, s24-26):
  - 13 runs at 10¹¹ with the digit file, identical to `e_1e11.out` (sha1 on the node);
  - 4 × 10¹⁰ off and on, identical to `e_4e10.out`;
  - 1.3 × 10¹¹ off and on, VERIFY OK with equal sha1s;
  - 6 runs without the file at 10¹¹, VERIFY OK.

  Commands: `~/C215/queue.txt` via `job.sh` / `run1.sh`, i.e. `env RNS_AUTO_PIECE_COST=<0|1> [DIST_TWREC=1] ECALC_VERBOSE=2
  RNS_VERBOSE=1 ./ecalc <D> [/tmp/c2_<tag>.txt]`. The plan checks used `MN_PLAN_ONLY=42500000000000:576 ./ecalc` (login node).
- **Did not pass / not done**: nothing failed. Only one pair was run at 4 × 10¹⁰ and at 1.3 × 10¹¹, as asked.
- **Open issues**:
  - (a) The correction share of the 10¹¹ gain is data-dependent; row 1d (K) addresses it on the defaults.
  - (b) At 1.3 × 10¹¹, dc is +6.9 s with C2 (V2 saw the same). It is after `total` but part of the wall with the file, and
    was not investigated.
  - (c) `DIST_TWREC=1` with C2, −2.8 ± 0.8 s: a separate adoption decision for the user.
  - (d) At size 1, B0's `mn_model` hides the file write under half the division (the `out` term grows when the division
    shrinks). This is a model rule, not measured here.
