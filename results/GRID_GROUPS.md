# GRID_GROUPS — the grid of piece products over shares and node groups

Scope: the grid of piece products over shares and node groups: layout at any group size, MN_GROUPS schedules, v-slot release, the 576-node layout checks. Owner of the items below; each section condenses the former `results/<ID>.md` (old IDs resolve through `results/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`. Labels: **m** measured, **mod** modelled, **a** assumed. Times Eastern.

## Topic summary

What this topic is: how a product of two numbers sharded over a group of g nodes is computed. The pieces are `rns_mul_dist_mn` (one plane per node pool, cap 2^(min(31, POOL_LOG) + log2 g) points), the grid of piece products above the cap (`split_grid_cap`, views, `mdb_add_shifted`), the block-cyclic map that lets every node of a group of any size transform (L), the tree on the `MN_GROUPS` schedule with k-way combines (G12), and the v-exchange slot memory of the general map (B7ACCT, S36, C06, S45).

Current state (main; cross-checked against `ecalc/README.md`):
- The grid over shares, the views, the general map for any g and the tree on the `mn_groups_parse` schedule are the code. Default schedule (README `MN_GROUPS`, G12 / Q's decision): powers of two that divide the size, then the odd part's prime factors ascending (576 -> 2, 4, ..., 64, 192, 576). The L schedule (powers of two, then the size) is `MN_GROUPS=2,4,...,512,576`.
- `COMM_LAYER_VSLOT_SHARE` (S36) and `COMM_LAYER_VSLOT_RELEASE` (C06) are built-in defaults since 2026-10-12 (README). One pair of v-slots per APU, shared by all layered communicators, and released per product under X2. The §B7ACCT / §S36 sums of v-slots per level are superseded: the resident term is the max, not the sum.
- `RNS_DIST_CACHE_MN` (transform cache over shares, G section) is built in with 2 slots. At size 1 the cache stays off (`RNS_DIST_CACHE=0`). The 3·2^30 plane (C5 in A-grid) was measured only; the radix-3 lengths now live under `RNS_R3` (README, default 1 when the prime set has radix-3 roots). The old `DIST_R3` switch is not in the README.
- `DIST_CHUNKS` is 8 (README); the §B7ACCT, §S36 and §S52 numbers use K = 8 (B7ACCT's table assumed K = 4 for the slot arithmetic of option rows).
- `DIST_COALESCE` (message bundling, §S52) was not built (user decision 2026-10-11) unless the target's per-message cost is shown >= 8.5 us.

Live conclusions:
- Memory is the limit at 576 nodes, not time: the real v-slot term (B7ACCT, 6.707 GB per APU before sharing) broke the device bar; SHARE (-5.79 GB per node, mod, S36/S45) and RELEASE (comm_ofi pool 4352 -> 2304 MiB per APU, C06) bring it back. Largest safe size at 576 is 3.82e13 (§S52, mod); above it the plan adds pieces (+20 s at 3.83e13).
- The cut-group pool-offset asymmetry (G5) does not bite (§C06); uneven last groups work (digits identical, §B06) but give no speed gain (§B06, §O1: `[6]` vs `[2,6]` at 6 nodes inconclusive, raw +2.4 s, CI [-3.9, +8.7]).
- The §C09 tables are modelled; the model-best uneven tops are unmeasured extrapolations.
- A bug found by forcing tree grids (`mdb_add_shifted` windows of the wrong nodes for g0 > 0, §G12 2b) was fixed before any run could reach it.
- §C215: the piece-cost grid (`RNS_AUTO_PIECE_COST`, C2) gains -31 s (-13 %) of `total` at 1e11 on one node; neutral at the 576 target (0.0 s, mod).

| item | title |
|---|---|
| A-grid | the grid of piece products over shares (A3) and the 3·2^30 plane variant measured (C5) (2026-09-19) |
| M3 | the top levels of the binary-splitting tree as distributed products over node groups (2026-09-18) |
| L | the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` |
| G | grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10) |
| G12 | the top product at scale: tree levels as piece grids for any g, exact spills, k-way schedule, per-node profile independent of g |
| B7ACCT | counting the general map's v-exchange slots in the layout (2026-10-06) |
| C06 | G5 (cut-group pool-offset asymmetry) and M3 (release the v-slots) (2026-10-11) |
| S36 | idea S-1: share the v-slots between the tree levels (2026-10-10) |
| S45 | CPU-only validation of `COMM_LAYER_VSLOT_SHARE=1` at the 576-node shape (2026-10-10) |
| S52 | more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11) |
| B06 | uneven node-group schedules (`MN_GROUPS` with a non-dividing last group), 2026-10-11 |
| O1 | overnight 6-node paired ABBA, `MN_GROUPS` `2,6` vs `6` (TASKS 42) |
| C09 | best `MN_GROUPS` for every usable node count (N = 576 .. 544) |
| c09_groups | the full C09 table as generated (no title in the original); kept as a pointer |
| C215 | the paired series for D1 (`RNS_AUTO_PIECE_COST`, C2), with `DIST_TWREC`, other sizes, and the 576 model (Phase 15) |

## A-grid — the grid of piece products over shares (A3) and the 3·2³⁰ plane variant measured (C5) (2026-09-19)

Branch `agrid` (from `main` @ a75474d; PLAN.md §19). Files: `ecalc/rns_dist.c`, `ecalc/mdb.h`, new `ecalc/tests/t_mn_grid.c`; `ecalc/dbig.c/.h` one exported function `db_share_add_shifted`.

#### A3 — the grid over shares

Question: M3's `rns_mul_dist_mn` ran one plane per node pool, so na + nb <= 2^(31 + log2 gt) points; the 4e10 top level (2.2e9 x 2.2e9 limbs) and the division's products exceed that at size < 8.

- **Cap.** One plane per pool = 2^(min(31, POOL_LOG) + log2 gt) points (`mn_logn_cap`; `DIST_LOGN_TEST` lowers it). Above it `split_grid_cap` chooses piece counts (ka, kb) with ceil(na/ka) + ceil(nb/kb) <= cap for the fewest plane points (single-node `split_grid` is now a wrapper, same behaviour).
- **Views (`mdbv`, `mdb_view`).** A view is (m, off, len): limbs [off, off + len) of a sharded number, len normalised within the window, computed identically on every node (`db_norm` on the share cut to the window, one `allreduce_max`). The redistribution packs a node's part of a view with the pack kernel reading at the window's offset; operand pieces are never copied.
- **Piece delivery (`mn_core`).** A piece at limb offset `shift`, basis Np, covers the window [max(clo, shift), min(chi, shift + Np)) - shift of every node's share (`piece_window`). The first piece (shift 0) is scattered straight into C's zero-filled shares with its spills (M3's path bit for bit). Other pieces go to a temporary T of the node's window, spills added to T, then C += T << window offset by `db_share_add_shifted` and a second node carry scan. No extra traffic.
- **Node carry scan (`share_carry_fix`).** As M3's: (carry, propagate) bytes all-gathered, carry into node r = c(r-1) | (p(r-1) & carry into r-1). A node with nothing to add reports propagate = 0 (M3 reported 1 for an empty share, right only for a zero-length one); if its +1 carries out, the scan runs another round (never seen, at most g rounds). Carry out of the top node aborts.
- **X (tree add P = P_A Q_B + P_B).** Rides in the CRT when one plane suffices; with a grid `mdb_add_shifted(C, X, 0, G)` adds it. `mdb_add_shifted(C, X, k, G)`: C += X << k; APU thread d serves quarter d of every node's window in rounds of 2^26 limbs (one all-to-all over mesh d per round), buffers 2 x g x 2^26 x 8 B per APU (1 GB at g = 2).
- **Low products (`rns_mul_low_mn`, `rns_mul_dist_mn_v` with w).** Views A[0, w), B[0, w), pieces starting at or above w skipped, result truncated to w (`mdb_norm`), share limbs above w zeroed (`share_zero_from`).
- Interface (mdb.h): added `mdbv`, `mdb_view`, `rns_mul_dist_mn_v`, `rns_mul_low_mn`, `mdb_add_shifted`, `mdb_norm`. Invariant: share limbs at or above C->n are zero. A one-node group (g = 1) is not a valid product argument.

Tests (m; aac6, one node, node-processes sharing it over loopback TCP, times meaningless):
- `t_mn_grid` at 2, 3, 4 node-processes and 2 with `LIMB_BASE=10`: VERIFY OK, 120 checks per node. Shapes: one plane, 1 x 2, 2 x 3 (the 4e10 shape), 3 x 1, subgroup layout (A on node 0, B and X on node 1); per shape and generator (uniform, all-ones): product, + X, three low products, two views and their low product, `mdb_add_shifted` at four shifts. A first version failed the low products (empty share reported propagate = 1); fixed as above.
- ecalc, all digits `cmp`-identical: 1e9 at sizes 1 (9.0 s), 2 (12 s), 4 (13 s); 2e10 at size 2 (125 s, size 1: 50.7 s); 2e10 at size 2 with `DIST_LOGN_TEST=29` so both top-level products ran as 1 x 2 pieces with `mdb_add_shifted` (3 rounds, 1.7 s): identical digits, 118 s.
- 4e10 at size 2 on one node: out of device memory at the first tree product (two processes' leaf trees do not fit one node; the grid split itself, 1 x 2 pieces at cap 2^31, was chosen). Needs two real nodes. 2e10 at size 2 with `POOL_LOG=29` failed in the leaf tree (single-node pipeline at a too-small pool), not the mn code.

#### C5 — the 3·2³⁰-point plane for the single-node top levels (measured, not adopted)

`DIST_R3=1`: in `dist_core` a product of logn >= 30 whose nc fits 3·2^(logn-2) runs on a plane of 3·2^28 / 3·2^29 / 3·2^30 (the last above the 2^31 cap); four-step with rows of C = 3·2^logk through the radix-3 layer and columns of 2^logR. Memory: +34 GiB per APU over the 2^31 layout (+136 GB per node; pool 0 grown to 32 GiB, xb and slabs 18 GiB from the block pool).

Measured at 4e10, size 1: baseline bs 43.5 s, dm 34.1 s, phases 77.7 s, wall 98.8 s, digits identical to `results/e_4e10.out`. With `DIST_R3=1` bs 56.5 s, then the job limit ended the run (no digits).

| plane | limbs | load | ntt | crt | total |
|---|---|---|---|---|---|
| 2^30 | 1.06e9 | 0.16 | 0.37 | 0.05 | 0.55 s |
| 3·2^29 | 1.13e9 | 0.27 | 0.54 | 0.07 | 0.88 s (first call 2.94 s) |
| 2^31 | 1.13e9-1.85e9 | 0.32-0.35 | 0.67 | 0.08-0.10 | 1.08-1.17 s |
| 3·2^30 | 2.19e9-2.96e9 | 0.54-0.57 | 1.06 | 0.13-0.14 | 1.75-1.82 s (first call 14.6 s) |

The 3·2^30 plane costs 1.57x the 2^31 plane for 1.5x the points (radix-3 layer and non-power-of-two columns, about 5 % per point). Steady-state per product: level 23 -0.4 s, level 24 -1.0 s, level 25 +0.7 s (0 with a fixed cost model), dm Q_t r +0.2 s, A mu -1.4 s, X Q -0.3 s: net about -2.5 s of the 77.7 s of phases (PLAN I10 estimate -3 s), at +34 GiB per APU and a one-time 15 s unless pools are sized at init. Verdict: **not adopted**. 4e10 with `DIST_R3=1` was not verified end to end (cut by the job limit).

#### Open issues (A-grid)
- 4e10 at size 2 needs two real nodes; the grid over shares at scale is covered by 2e10 with cap 2^30 and by `t_mn_grid`.
- `mdb_add_shifted` pads every slab to the round size (g x 2^26 x 8 B per APU and buffer); fixed later by L's B7 consumers (below).
- A piece's window T is up to the share length (2.2e9 limbs at size 2, 4e10: 17.6 GB per node) from the block pool. Later bounded in G12 (§G12 section 6 still lists T).

## M3 — the top levels of the binary-splitting tree as distributed products over node groups (2026-09-18)

Branch `m3-dist-top-levels` (from `main` @ 62d81bb; PLAN.md §17). Question: after M2 node 0 combined all leaf (P_r, Q_r) on its host; M3 computes the top log2(size) levels as distributed products, every operand and result sharded over the level's group, so work per node stays constant as size grows. `MN_COMBINE=host` keeps the M2 combine. At the end node 0 gathers the final shares and runs the single-node division and output (M4 later distributed the division).

Files: `ecalc/comm_layered.c` (layered communicator: 4 APUs over xGMI x g nodes over mesh d as one comm of 4 g ranks), `mdb.h` (`mdb`, `mn_group`), `rns_dist.c` (`rns_mul_dist_mn`), `dbig.c/.h` (`db_pool_alloc/free`, `db_zero_fill`, `db_share_add_spills`, `db_share_add_one`, generalised sparse spill), `mn.c/.h` (`mn_group_at`, `mn_allgather`, `mn_selftest_layered`, `mn_tree`, `mn_gather_host`), `ecalc.c` (`MN_COMBINE`), `tests/t_dist.c` (`DIST_LAYERED=1`), `comm_tcp.c` (connect retry 60 -> 600 s).

Design:
- **Rank map.** APU d of the group's node r is rank rho = g d + r (APU-major): the g slabs a sender holds for APU d' of all nodes are contiguous, so the intra-node stage is the existing xGMI push with slabs of g x bytes. Rank rho holds the block-cyclic rows [rho R/nr, (rho+1) R/nr) of the R x C plane, row i column j <-> point i + R j (`ntt_dist.c`'s convention).
- **Layered all-to-all.** (1) xGMI all-to-all with slabs of g x bytes, (2) block transpose to [r'][d], inter-node mesh all-to-all with slabs of 4 x bytes, (3) block transpose to global rank order in `wait`. Scratch is one slab buffer (block pool or hipMalloc).
- **Sharded numbers.** `mdb` = (n, N, g0, g, sh): n normalised limbs, node g0 + k owns the contiguous share [N k/g, N (k+1)/g) of the sharding basis N >= n; normalising moves no data (one `allreduce_max`). Every node describes any operand from these numbers; only the sibling group's descriptors are all-gathered per level.
- **Product over a group.** The transform ran on the first gt = 2^floor(log2 g) nodes (rows must divide by 4 gt), others took part only in redistributions (superseded by L). Rank rho's row sequence t = j rows + il <-> limb R j + rho rows + il is increasing, so a node's contiguous share meets it in one segment [seq_start(lo), seq_start(hi)); the redistribution is one all-to-all (slabs padded to S = ((maxshare - 1)/R + 2) rows; later replaced by exact `alltoallv` in L). A and B are redistributed once per product, X (the tree's P_B) compacted into the CRT's `x` array so the tree add costs one redistribution and no add pass. Per prime: forward A and B, pointwise, inverse over the layered comm (3 layered all-to-alls), local transpose, striped CRT (stripe per column, 4-limb spill each). Reverse: `k_pack_out_mn` / `k_scatter_mn`, spills of all ranks all-gathered and added by `db_share_add_spills`, cross-node carries by the (c, p) scan. Per product: 3 (2 without X) operand exchanges + 12 layered all-to-alls + 1 result exchange + 1 spill all-gather + 2 tiny exchanges.
- **Tree (`mn_tree`).** Level l = 1..ceil(log2 size): group [k 2^l, min((k+1) 2^l, size)); A = lower half, B = rest; P = P_A Q_B + P_B, Q = Q_A Q_B over the joined group; no operand is moved before the product. Group meshes are TCP meshes opened at the level's start (port slots base + 512 s + 64 d + node); the top level reuses the M1 meshes.
- End: `mn_gather_host` on node 0, then the M2 flow. Size 1 goes through none of it.

Tests (m; jobs on one node and on three real nodes, 1 GbE: times are correctness only). Every ecalc run VERIFY OK and `cmp`-identical to `ref/e_<digits>.txt`:
- Layered communicator (`t_dist` `DIST_LAYERED=1`): 2, 3 (gt = 2, node 2 idle) and 4 node-processes, 2^20..2^24 (and 2^26 at 2): VERIFY OK. A first self-test failed because it gave each APU its own prime (test bug, `comm_layered` was right).
- ecalc 1e6 (sizes 2, 3, 4), 1e8 (sizes 1, 2, 4; with `BS_MDEV_LOGL=21` device leaf P, Q; two and three real nodes: 11.5 / 12.9 s, tree 6.3 / 8.7 s), 1e9 (sizes 1, 2, 4; two real nodes with `BS_MDEV_LOGL=25`: 48.2 s, tree 36.1 s; 4 processes on two nodes 52.4 s, tree 39.3 s): all identical. The tree's 1e9 runs cover products of 2^27 points over 8 and 16 ranks (largest exchange about 0.4 GB per node).
- Fixed on the way: `strtok` -> `strtok_r` (four threads open meshes at once), `mnrun.sh` host list and random port base, plain-mesh self-test skipped at non-power-of-two sizes, carry scan as a byte all-gather (`allreduce_modq` overflowed beyond 30 nodes).

Open issues at the time (all later addressed): one plane per pool (-> A-grid), per-point binary search over g (-> L), idle nodes beyond the largest power of two (-> L), memory of slabs and layered scratch (-> M9, G12 section 3).

## L — the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` (Phase 11, PLAN.md §26 row L; 2026-09-20)

Branch `l11` (from `main` @ 72aa2e9). Files: `ecalc/rns_dist.c`, `mdb.h`, `tests/t_mn_grid.c`; minimal edits in `newton_db.c` (`mdb_shift`, `mdb_to_host_all`) and `dbig.c` (three lines in `sparse_get` for unequal row parts).

#### C3-576 — the map

Before: a group of g nodes transformed on its first gt = 2^floor(log2 g) nodes; the others idled in the product. At 576 = 9 x 2^6 the top level would have idled 64 of 576 nodes.

Now every node transforms: nr = 4 g ranks, rank rho = g d + r holds the rows [R rho/nr, R (rho+1)/nr) (floor or ceil of R/nr rows; `part0` / `partn`) and the columns [C rho/nr, C (rho+1)/nr). Row sequence and one-segment property are unchanged (`seq_start(m, R, nr, rho)`). For g a power of two the transform is `ntt_dist.c`'s pipelined one (sizes 1, 2, 4: same code as before, bit for bit). For any other g it is the general four-step in rns_dist.c (`gen_fwd` / `gen_inv_pw`, kernels `k_twpack_g`, `k_unpack_g`, `k_pack_cols_g`, `k_unpacktw_g`): per chunk k of the rank's rows (K = `DIST_CHUNKS`), row pass, twiddle and pack into slabs (back to back in rank order), one `comm_alltoallv` over the layered communicator with exact per-pair counts, wire time of chunk k under the row pass of chunk k+1; then unpack tables (`gplan_build`) and the column pass; the inverse mirrors it with the pointwise product fused into the column inverse (`ntt_inv_pw_y`). Values are the same integers mod p (exact NTT), so digits are identical. `DIST_GEN=1` forces the general map at a power-of-two g (tested at 2 and 4: same digits).

Also generalised: redistribution kernels take (R, nr, rho); CRT stripes are the rank's rows x C; spills of rank rho land at limb R j + R (rho+1)/nr (`sparse_get` decodes this when rows x nr != R); the cap `mn_logn_cap` = 2^(min(31, POOL_LOG) + floor(log2 g)) lowered by one only where rounding would not fit the pools (never for g <= 2304: at 576 nodes and 2^40 points R = C = 2^20, rows 455/456, largest plane 478 M limbs of 2^29); minimum 32 rows per rank. `mn_group.gt` / `tr[d]` are no longer read by the product. A group of one node is still not a product argument.

#### The schedule: `mn_groups_parse` (`mdb.h`)

`int mn_groups_parse(int size, int *out, int max)`: pure function of (size, `MN_GROUPS`); `out[l-1]` = group size of tree level l (nodes [k G_l, min((k+1) G_l, size))); returns the level count. The list must be increasing, each size a multiple of the previous or the size itself (the top group is cut by the size); anything >= size ends the list at size; an invalid list aborts. L's default: powers of two up to the largest <= size, then size (576 -> 2, ..., 512, 576). A level with ratio k > 2 is a k-way step (k - 1 products over the level's group; the combine loop is `tree_level_k` in G12). G12 replaced the default (see §G12 section 2).

#### B7 — the four consumers on `comm_alltoallv` / `comm_allgather`

| consumer | before | now |
|---|---|---|
| operand redistribution (`redistribute`) | one all-to-all of g padded slabs; `k_gather_mn` binary search over g | `seg_tables`: exact send/receive counts, one `alltoallv`; the receive buffer is my sequence; `k_gather_mn` reads `rb[t]` |
| result exchange | `k_pack_out_mn` into g padded slabs, `k_scatter_mn` at r x S | CRT output `xb` is the send buffer (sdsp = t0 x 8), receive at prefix offsets |
| spills | all-to-all of g copies of my 4 C limbs | `comm_allgather(G->all[d], spill, spill_rb, 4 C x 8)` |
| `mdb_add_shifted` | rounds of 2^26 limbs, sb, rb = g x 2^26 x 8 B per APU | exact cuts per round, one `alltoallv` per round; sb = largest round's send total, rb = one chunk: independent of g |
| `mdb_shift` (newton_db.c) | sb, rb = g x SL x 8 B per APU | `struct piece` gained `off`; one `alltoallv`; sb, rb = my pieces' limbs (<= share/4 per APU) |
| `mdb_to_host_all` | all-to-all of g copies (2 g x ms x 8 B on APU 0) | `comm_allgather` of one copy (g + 1 blocks) |

Gain: the redistribution padding was only two rows per pair, so its gain is layout; the memory is in `mdb_shift` (-2 (g-1) x share/4 x 8 B per APU per call: 17.6 GB per node at 4e10 over 2 nodes (arithmetic, mod); at 576 the padded form would have been impossible) and `mdb_add_shifted` (-(2 g - 3) x 512 MB per APU).

Scratch at size 2 (m; `mem_report`, 1e9 digits, POOL_LOG=29, one node, block pool peak live per process; `main`'s binary vs this branch): after the tree pl:peak 1.9 -> **1.5** GB (hipMalloc fallback 0.5 -> 0.4); after bs + dm 3.0 -> **2.5** GB (1.6 -> 1.2). Digits identical; wall 27.9 -> 26.1 s (dm 18.8 -> 16.9) on loopback TCP.

#### Tests (m; aac6, one node per batch, loopback TCP, correctness only)

Every run VERIFY OK on every node, digits `cmp`-identical to `ref/e_<digits>.txt` (or `results/e_1e10.out`):
- `t_mn_grid` at 2, 3, 5, 6, 9 processes (5 shapes x 2 generators incl. subgroups; 200 checks on every node; sizes 3, 5, 6, 9 on "the general map"), and with `DIST_GEN=1` at 2 and 4: VERIFY OK.
- ecalc 1e8 at sizes 2, 3, 4, 6, 9 (8.0 / 9.4 / 8.3 / 25.8 / 62.2 s; size 9 = four levels 2, 4, 8, 9, last a 9-node group on the general map); 1e9 at sizes 1, 2, 3, 4 (14.5 / 26.1 / 24.5 / 18.9 s), size 3 with `DIST_LOGN_TEST=25` (grids forced, 26.5 s), size 6 (`POOL_LOG=28`, 55.8 s): all identical. 
- **1e10 at size 4 (`POOL_LOG=29`): identical to `results/e_1e10.out`, 4 nodes VERIFY OK, 99.3 s (bs 28.9, dm 61.8).**
- 1e9 at size 9 not run (TCP mesh port collision on the shared node; no computation started; 1e8 at size 9 covers the 9-node group).

#### Open issues (L)
- The general map's exchange is not pipelined as deeply as the power-of-two one (the layered communicator keeps one v-exchange pending), and its v-scratch is the communicator's own hipMalloc (about 3 x the chunk's slab volume per APU: 2.9 GB at 576 nodes, 2^40 points, K = 4). Both led to B7ACCT / S36 / C06 below.
- On aac6 the general map costs more wall only because loopback TCP serialises; no fabric number exists here.
- `mn.c` still created `tr[d]` (unused by the product); `mn_selftest_layered` and `t_dist` still test the gt-node layered communicator. The 4e10-over-2-nodes figure of 17.6 GB per node is arithmetic from buffer sizes (not run).
- The k-way combine and the 9-way vs 3·3 choice were left to G12 / X2's model.

## G — grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10, PLAN.md §21)

Branch `g10` (from `main` @ 4aca721). Files: `ecalc/rns_dist.c`, `mdb.h`, `newton_db.c`; minimal edits in `ntt_dist.c` (`dist_inv_pw`, 8 lines), `ntt_dist.h`, `rns_mul.h`, `tests/t_mn_grid.c` (new cases).

#### A5 — the cuts of the grid over shares (`rns_mul_dist_mn_cut`)

`rns_mul_dist_mn_cut(C, A, B, G, lowcut, highcut)`:
- **lowcut:** a piece whose limbs end at or below the cut is skipped (the A_h mu product of which only `t >> (k+1)` is used; each skipped piece < B^cut, X low by at most their number + 1, absorbed by the up-corrections).
- **highcut:** pieces starting at or above it are skipped, and the result is **delivered in basis `highcut`** (`C->N = w`): windows clip to the shares and the carry out of the top node is dropped (mod B^w, `g_top_ok`) instead of aborting. Before, the truncated product came out in basis na + nb and the sharded division re-sharded it with an `mdb_shift` of the whole low half; that shift is gone (51 shifts per run instead of 52 at 1e9). `rns_mul_low_mn` and `rns_mul_dist_mn_v(w)` inherit this.
- `newton_mn_divmod` uses `mn_prod_cut(&t, &Ah, &mu, G, k+1, -1)` and `mn_prod_cut(&xql, &Xn, Q, G, 0, w)`; `NEWTON_HIGHPROD=0` / `NEWTON_LOWPROD=0` keep the full products. `rns_mul_dist_mn_shape` exposes the grid shape (the test's reference).
- `t_mn_grid`: low cut at (na+nb)/2, nb/2+1, na+nb, each with and without a high cut; 200 checks per node at 2 processes (was 120).
- m (aac6, one node per batch, all identical, every node VERIFY OK): 1e8 at sizes 2, 3, 4 (also with `BS_MDEV_LOGL=21`, and `DIST_LOGN_TEST=22`); 1e9 at sizes 1, 2, 3, 4 (also `DIST_LOGN_TEST=25`: A_h mu 2 x 2 with one piece skipped, X Q 2 x 2 in basis w); `t_mn_grid 0.5 28` at 2, 3, 4: 200 checks.

#### A1 — the transform cache (`rns_dist.c`; PLAN §20 I4, A-div's "B2")

Idea: within one grid product every piece transform is recomputed per piece product (2 ka kb forward transforms for a ka x kb grid) although the loop reuses B piece j across all i and A piece i across all j. A cache of N slots brings it to about ka + kb transforms. A slot = the forward transform of one operand view (key: dbig quarters, offset, length; over shares the mdb, offset, length) on EC_NP planes of q limbs per rank from the block pool; a hit skips gather, forward transform (and over shares the all-to-all redistribution); `mul_grid` / `mn_grid` plan the slots and return the planes at the end of every product, except pinned slots (`rns_dist_cache_hold(1)`, `RNS_DIST_CACHE_HOLD=1`, off: the in-product policy already transforms each piece of Q once inside X Q). `RNS_DIST_CACHE` = slots (default 3 in the code at the time; 0 = off), 16 GiB per slot per APU at 2^31 points. `mul_high_db` became `mul_grid`'s `lowcut`, so the cache serves it too (bit for bit). `recip_mn` uses Q itself at the top step (one all-to-all less). Expected at 4e10 (mod): about -15 transforms, about -5 s, if the pool holds the planes without hipMalloc fallback (0.057 s/GB, serialised).

m, 4e10 size 1 (`RNS_VERBOSE=1 ECALC_VERBOSE=2`, digits identical in every run):

| run | cache | planes allocation | bs | recip | division | dm | wall |
|---|---|---|---|---|---|---|---|
| base (fuse off) | off | - | 38.3 | 16.9 | 17.7 | 34.8 | 86.0 |
| c0 (batch 4) | off | - | 36.7 | 16.1 | 16.3 | 32.6 | **85.6** |
| c1 | 1 slot | 3.6 s | 40.2 | 15.1 | 15.2 | 30.4 | 86.6 |
| c2 | 2 slots | 7.4 s | 43.1 | 15.6 | 14.2 | 30.0 | 89.6 |
| c2b (repeat) | 2 slots | 9.3 s | 45.1 | 15.1 | 14.1 | 29.3 | 89.8 |
| c3 from the block pool (first version) | 3 | pool fallback 3 x 64 GB | 135.3 | 26.2 | 27.2 | 57.4 | 208.1 |

Per product: -4.1 s of products at 2 slots (a piece product with a B hit 1.11 -> 0.72 s; 20 hits of 43 transforms in dm), but the two 16 GiB planes per APU cost **7.4-9.3 s to map** (hipMalloc 0.055 s/GB; the pool has no room at the products' peaks: peak live 121.5 of 141 GB), so the wall loses 1-4 s. Mapping at init would move, not remove, the cost (RESULTS §70). **Not adopted at size 1: `RNS_DIST_CACHE` defaults to 0.** `RNS_DIST_CACHE_HOLD=1` adds nothing over the in-product policy (4e10, N = 4: 4+8+4 transforms held vs 4+5+5 unheld).

Over shares (m, 1e9 over 2 node-processes, grids forced by `DIST_LOGN_TEST=25`): dm 21.4 s (cache 0) -> 19.1 s (2 slots) -> 18.4 s (4 slots); the division's products 9.7 -> 7.9 -> 6.8 s (a hit skips the all-to-all redistribution too). Planes over gt nodes are 1/gt the size (8 GiB per APU per slot at 4e10 over 2 nodes, 2 s to map). **Adopted over shares: `RNS_DIST_CACHE_MN` defaults to 2** (the group agrees on the minimum slot count that every node's free memory allows). 4e10 over two real nodes was not run.

Size-1 correctness of the cache: 1e9 with `DIST_LOGN_TEST=25` at 1, 2, 4 slots (28 hits), the hold, `LIMB_BASE=2`, `t_dbig 24 big`: all identical.

#### A6 — the distributed tier's local transforms

- The pointwise product is fused into the column inverse's first pass (`dist_inv_pw` -> `ntt_inv_pw_y`; bit-identical; `DIST_PW_FUSE=0` restores the two passes): a 2^31 piece product's ntt 0.73 -> 0.67 s, dm 34.8 -> 33.8 s (m, within run-to-run spread). `t_dist 24`, `DIST_TINV=1`, `DIST_XGMI=1 t_dist 31`: VERIFY OK (67 checks); xGMI 2^31 fwd 0.0573 s, fwd+fwd+pw+inv 0.1750 s.
- Body 1 (register-blocked kernel) needs 7-stage passes (local lengths >= 2^17); the dist tier's rows/columns are 2^15/2^16 at a 2^31 plane, so it never runs. `DIST_LOGR_DELTA=-1` (rows 2^17, columns 2^14): ntt 0.65 but load 0.54 s; total 1.33 vs 1.11 s per plane: worse, not adopted (the split stays logn/2). `DIST_R3` keeps the separate pointwise.

Final tree with defaults (m, batch 5, shared node): `t_dbig 24 big` VERIFY OK (609 checks); `t_mn_grid` 200 checks at 2 and 4; size 1 1e9 decimal 8.2 s and `LIMB_BASE=2` 13.8 s identical; **4e10 size 1: 84.8 s, dm 32.6 (recip 16.0 + division 16.5), bs 36.8, init 15.3, digits identical** (RESULTS §74: 86.4 +- 1.3, dm 32.9). `RNS_DIST_CACHE=2 t_dbig 24 big` was killed on that shared node at its 2^31 x 2^30 product: the free-memory guard (`hipMemGetInfo` minus margin) does not see the host's share on the APU.

#### Open issues (G)
- The cache at size 1 pays only if its planes are already mapped; a plane pool sized at init (M's `rns_init`) would cost the same mapping. If B3 (3·2^30 planes, +34 GiB per APU) is ever adopted the cache could share that memory.
- The cache over shares was measured at 1e9 over node-processes; the expected win at 4e10 over real nodes (8 GiB per APU per slot, 2 s to map vs skipped redistributions) is unmeasured.
- The `hipMemGetInfo` guard does not see host allocations on the APU.
- A b16 kernel for 6-stage passes is N-kernel's territory.

## G12 — the top product at scale: the tree's levels as piece grids for any g, the exact spills, the k-way schedule, the per-node profile independent of g (Phase 12, PLAN.md §27 row G; DECISIONS2 §2)

Branch `g12` (from `main` @ 7aded87). Files: `ecalc/rns_dist.c`, `mdb.h`, `mn.c` (`mn_tree`, `tree_level`, new `tree_level_k`, `mn_group_span`), `mn.h`, `mem_model.py` (tree/spill g-terms); minimal edits in `dbig.c/.h` (spill decode of the compact form, `db_share_add_spills_x`) and `binsplit.c` (`tree_need_dev`).

#### 0. What was actually the case at 7aded87

`rns_mul_dist_mn` already gridded over the shares at the group's cap, so plane pools never grew with g. M11's "214-856 GB of planes at 576" was the model's reading of `tree_need_dev`, which sized the arena request at init as ONE uncapped transform (q = n/4g) plus two g x C x 4-limb spill buffers: about **700 GB per node at 576 x 4e10** (Q's finding 1; it would fail at init). In the code itself one item scaled with g: the spill all-gather in `mn_core` (`spill_rb` = g x 4 C limbs per APU per piece: 77 GB per node at 576 nodes, C = 2^20). And `MN_GROUPS` was parsed but `mn_tree` walked binary levels (finding 2).

#### 1. The exact spill exchange (`mn_core`, `sparse_get`)

For a destination node with window [lo, hi) of the piece, the columns whose block meets the window form one contiguous range [j0, j1) (`spill_cols`): `comm_alltoallv` over mesh d straight from `v->spill` (counts 4 (j1 - j0) x 8 B), receive per source rank only the blocks meeting my window plus a (j0, j1, offset) table, about 4 C + 4 g limbs per APU **whatever g** (32 MB at C = 2^20). `db_share_add_spills_x` decodes through the table (the old layout stays for `tab = 0`). Both sides compute the ranges from the same windows (`piece_window`). Digits unchanged.

#### 2. The tree on the `MN_GROUPS` schedule with k-way combines (`mn.c`)

`mn_tree` walks `mn_groups_parse`'s levels (children = the previous level's groups, G_0 = 1). Two children -> `tree_level`; k > 2 -> `tree_level_k`: Horner from the top child, (P, Q) <- (P_i Q + P, Q_i Q) for i = k-2..0, k - 1 combines of two products over the level's group, each balanced over all nodes; a child's shares are freed right after its combine (O(share) memory). `mn_group_span(l, g0, g)` returns `mn_group_at`'s group for a binary-level group (same meshes and slots as before at power-of-two sizes) and otherwise creates four meshes (SHMEM PE sets with their own ids; TCP meshes on the level's odd slot 2l - 1 at port lanes 4..7 (a first version used slot 2 MN_MAXL + l, whose ports fell into the ephemeral range: "cannot connect" at size 9)). No `tr` / topo layer; destroyed in `groups_finalize`.

**Default schedule (Q's decision, results/TIME_MODEL.md §Q §1: the 3·3 top):** powers of two that divide the size, then the odd part's prime factors ascending: 576 -> 2, 4, ..., 64, **192, 576**; 9 -> 3, 9; 6 -> 2, 6; 12 -> 2, 4, 12; a power of two -> the binary tree. Every group is exact and every level a k-way step by one prime factor. L's schedule is `MN_GROUPS=2,4,8,16,32,64,128,256,512,576`; the 9-way top is `...,64,576`. The size-1 path, sizes 2 and 4, and the reciprocal's / division's groups are untouched.

##### 2b. The bug the forced tree grids found (pre-existing since Phase 9 A3 / Phase 11 B7, fixed in commit 7d2b2c9)

With the tree's levels gridded, 1e9 at size 4 gave a wrong P on the level-1 group [2, 4), on `main`'s binary too (`DIST_LOGN_TEST=23`). A new probe found the level (`ECALC_RES_LOG=1`: after every tree level each node's shares of P, Q reduced mod the 8 T1 primes, all-gathered, compared with `vf_pq_range_mod`; `tree_check` in mn.c, kept), and a new `t_mn_grid` pass over every node's level-1 group reproduced it in seconds: **`mdb_add_shifted` sized its rounds by the windows of nodes 0..g-1 instead of g0..g0+g-1**, so on a group not starting at node 0 every window was empty and X was never added. Could it have affected an earlier result? No: the one caller is `mn_grid`'s `if (X) mdb_add_shifted(&Cn, X, 0, G)`, and before this branch the tree's levels never gridded at any tested size (the M3 path carried X in the CRT), and the other products over shares (reciprocal, division) pass X = 0. It could only show on a gridded product with X over a group not starting at node 0, i.e. the 576-node tree below the top level, and it aborts loudly there.

#### 3. The decision rule and the arena request

A level whose product fits one plane per prime at the group's cap runs the M3 path (X in the CRT); above it the grid. `DIST_LOGN_TEST=<log n>` lowers the cap; new `MN_TREE_LOGN_TEST` forces the cap for the tree's levels only (the division under `DIST_LOGN_TEST=27` at 1e10/4 fails with "65 corrections" on `main`'s binary as well: pre-existing, X's / A-div's code). **Arena request** (`binsplit.c` `tree_need_dev`, commit 79a2b48): per schedule level the largest Horner product P_0 x Q_run costed by `rns_mul_dist_mn_scratch(na, nb, has_x, g, share_a, share_b, share_c, &pieces)`: the grid's largest piece at the cap and `mn_core`'s buffers (rbA, rbB, rbX, sb, cx, tmp, rbO, spills 4 C + 4 g limbs, T <= share_c) = O(share) + O(q), q <= 2^(min(31, POOL_LOG) - 2): no g-term. `ECALC_VERBOSE=2` prints one `bs: tree layout:` line per level.

#### 4. The memory model (`mem_model.py`)

Three changes on Q's file: `tree_need_dev(form='grid')` is the Python port of `rns_mul_dist_mn_scratch` (same formula line by line); default `form='grid'` (`'flat'` = 7aded87, kept for before/after); default schedule = section 2's. Q's size-1 calibration unchanged (4 / 7 / 8e10 within 1 %).

Modelled per node at g = 576 (GB, `transport='tcp'`):

| D per node | form | planes | arena | top scratch | exchange | host | node peak |
|---|---|---:|---:|---:|---:|---:|---:|
| 4e10 | flat (7aded87) | 428 | 709 | 556 | 36 | 18 | **1170** |
| 4e10 | grid, default 3·3 | 121 | 180 | 47 | 36 | 18 | **354** |
| 4e10 | grid, `...,512,576` | 121 | 196 | 63 | 36 | 18 | 370 |
| 8e10 | flat | 856 | 1417 | 1112 | 71 | 18 | **2305** |
| 8e10 | grid, default | 121 | 333 | 85 | 71 | 18 | **543** |
| 8e10 | grid, `...,512,576` | 121 | 375 | 127 | 71 | 18 | 585 |

Ceiling at 502 GB per node (grid, tcp): **7.1e10 per node at g = 576** (6.8e10 at g = 4 and 64), against 1.9e10 flat, i.e. 576 x 7.1e10 about 4.1e13 digits on the profile alone (mod). Against `mem_report` (1e10 at size 4, POOL_LOG=29, per process, m): block pool borrowed **19.9 GB** (the arena, zero hipMalloc), planes 34.4, tree peak live **9.2-10.7 GB** (M11: 11.0 with the g x 4 C spills), device 56.6-56.7 GB; modelled tree need 14.6 GB (an upper bound, 40-100 % above peak live because sb is freed before rbO exists), arena 19.9 GB (exact to 0.1 GB), planes 36.5.

#### 5. Tests (m; aac6, one node, node-processes over loopback TCP; every ecalc run VERIFY OK on every node, part files `cmp`-identical to `ref/e_<digits>.txt`, probe `ECALC_RES_LOG=1` on)

Gate (binary 7d2b2c9 / f65449e):
- `t_mn_grid 0.5 27` at 2, 3, 4, 6 and `0.25 27` at 9: VERIFY OK, **200 / 200 / 224 / 224 / 224 checks** (24 new at sizes >= 4: every node's level-1 group [2k, 2k+2)).
- 1e8 size 3 / 6 / 9 (tree and division gridded; schedules 3 / 2,6 / 3,9): identical, 0 level mismatches (12.4 / 62.0 / 60.2 s). 1e9 sizes 2, 3, 4 (43.4 / 39.4 / 27.0 s) and size 9 with `MN_TREE_LOGN_TEST=22` (124.6 s; tree device 21.9 GB per process, hipMalloc 0, against model tree need 0.6 and arena 0.8 GB): identical, 0 mismatches.
- **1e10 size 4 with the grid forced (`MN_TREE_LOGN_TEST=27`: level 2 1 x 2 pieces at the 2^29 cap, accumulating piece, shifted add, exact spills): identical, 123.3 s (bs 50.1, dm 61.9); not forced (M3 path): identical, 124.3 s (bs 54.8, dm 59.5).** 1e9 size 1 identical, 15.9 s.
- Tree's device peak at 1e10/4: 56.7 GB = planes 34.4 + block pool 19.9 + tables 2.5; peak live 7.0-8.2 GB grid forced, 9.2-10.7 not forced.
- Earlier batches: all identical; 1e8 at 3, 4, 6, 9 with `MN_GROUPS=3` / `4` / `3,6` / `9` / `3,9` / `6` (a 9-way step = 8 combines over 9 nodes, 79.9 s; the 3·3 schedule 56.1 s); 1e10 size 4 not forced 92.6 s (L: 99.3, M11: 116.5, X: 100-123). Two failures were not the tree: `DIST_LOGN_TEST` at 1e10/4 and 1e9/4 fails in the sharded division with "65 corrections" on `main`'s binary too; the schedule groups' first port slot fell in the ephemeral range (fixed).

#### 6. Open issues (G12)
- The window temporary T of an accumulating piece is up to the node's share of C (35 GB per node at 8e10); a rounds form would bound it. It is the largest O(share) term after the packed part sb.
- The transform cache over shares allocates 16 GiB per slot per APU by hipMalloc (constant in g).
- The division's `mdb_shift` exchange scratch (2 x share/4 per APU, 71 GB per node at 8e10) is the remaining term above the single-node profile at 576 (X's / L's file).
- A k-way level is Horner (2 (k-1) products); Q notes a pairwise combine would cost about 25 % fewer plane points at a 9-way step (no step above 3-way in the default).
- Tree checkpoint sets (M6) are indexed by the schedule's level number: a restart must use the same `MN_GROUPS`.
- The reciprocal over 6 nodes at a lowered cap takes 365-454 s (99 % one wait; `MN_MODEL_TCP=1` or `NEWTON_MN_GROUPS=0` give 34-46 s): X1's group choice on loopback TCP, not the tree.
- `DIST_LOGN_TEST` below the division's own need is pre-existing on `main` (works at 24, not at 23 or below over 4 nodes).

## B7ACCT — counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06)

Login-node work only (aac7, `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY`; C-binary prints are measured layout figures, the code's sizing, not memory readings). Branch `b7-vslot` pushed, not merged. Question: STD17's 10-node run showed about 3.3-3.5 GB per APU of memory the layout did not count; are they the v-slots of the layered communicator?

#### 1. Which exchanges take v-slots

- The only v-slot site is `comm_layered.c` `y_alltoallv` / `y_alltoallv2`, reached through the layered communicator `G->lay[d]` by the general four-step (`gen_fwd`, `gen_inv_pw`), run when `gen = !is_pow2(g) || DIST_GEN`. Other `comm_alltoallv` calls (redistribute, result, spills, `mdb_shift`, `mdb_add_shifted`, `newton_db.c`) use the plain mesh and stage in the comm_ofi pools (already counted by OFIMEM). Power-of-two groups' layered comm uses the caller's `tmp` (block pool).
- Slot formula (`need_vslot`): `COMM_ALLTOALLV_DEPTH=2` (default) two slots of A + max(A, B) + 4 bytes; depth 1 one area of 2A + B + 4. With A about B about q/K two slots hold 4q/K points = **q x 8 bytes at K = 4** (one full plane per APU). At 10 nodes at the cap q = 3277 x 131072 points: **3.444 GB per APU (mod, exact formula)**.
- Slots never shrink (`need_vslot` only grows) and the communicator is destroyed only in `groups_finalize`, so each general-map group keeps its largest slot from its first general product to the run's end. One group per node holds slots at each level; the whole-machine group is shared by the tree top, reciprocal and division. At 576 with `...,64,192,576` the general-map groups are 192 and 576.
- Resident per APU: 0 for power-of-two tree levels; a general level l = sum over general levels <= l of that level's slot; reciprocal and division = sum of lower levels + max(top, A_h mu, Q_t r) (superseded by SHARE/RELEASE: max not sum).

#### 2. The code

`rns_dist.c` `vslot_shape` / `rns_mul_dist_mn_vslot(na, nb, g)` (one product's v-slots per APU, 2 MiB rounding, conservative; 0 for power-of-two g without `DIST_GEN`); `binsplit.c` `binsplit_vslot_bytes` (resident sum from §1, uses `mn_groups_parse`, `tree_need_dev`'s products, `dm_nq_of`); switch **`ECALC_VSLOT_BUDGET`** (default 0): adds 4 x v-slots to `binsplit_node_bytes` (the budget check's modelled peak, `RNS_DIST_CACHE_FIT` room, `ECALC_PLANE_CAP=fit`, `as_room_fits`); arena request and every allocation unchanged. Always-on reporting: `BS_LAYOUT_ONLY` prints a `vslot:` line, `MN_PLAN_ONLY` a `plan vslot` line (every other line byte-identical to main), `MEM_REPORT_DEVS=1` adds "outside the layout" fields from `comm_layered_vbytes(dev)` (a relaxed atomic counter; built, not run on a GPU node at the time). `mem_model.py` mirrors it (`mem_per_node` adds 4 x tree_top to `dev_bs`, 4 x per_apu to `dev_dm`; default `depth` is 2); `--check-c` compares vslot, tree top, device + vslots, node + vslots. A flip test found a bug in the check's room loop (used 480 instead of `ECALC_NODE_GB`), fixed.

#### 3. Tests (m, aac7 login node, no GPU)

`mem_model.py --check-c ... 31 1024 128 0.16 auto` is **exact everywhere** (0 terms not exact, largest arena difference 0.0000 %): 4e10 and 1e11 (g 1), 9.1597e10 on 576 and 2, 5e10 on 2, the target share 7.0833e10 on 576, 8.1e10 on 3, 6, 10; vslot rows match to the byte (3,443,523,584 at 10 nodes, 6,706,692,096 at 576); also under `ECALC_VSLOT_BUDGET=1` (flip at 9.1597e10:576 with `ECALC_NODE_GB=460`: room fits without, dropped with it). Gates not run (compute nodes forbidden for the task); by default the runtime change is an atomic counter plus printing, so digits are unaffected by construction.

#### 4. Predicted layouts at the standard config (8.1e10 per node; GB per node)

| nodes, digits | general-map groups | v-slots per APU | device before -> after | node before -> after |
|---|---|---|---|---|
| 3, 2.43e11 | 3 | 2.869 | 362.47 -> **373.94** | 403.48 -> **414.96** |
| 6, 4.86e11 | 2, 6 | 2.869 | 379.65 -> **391.12** | 420.66 -> **432.14** |
| 10, 8.1e11 | 2, 10 | 3.444 | 379.65 -> **393.42** | 420.66 -> **434.44** |

STD17 10-node: measured node 4 x 100.9 + 19.9 = **423.5 GB (m)**; old layout 420.66 (mod, 2.9 GB under, only because the host term's 17.2 GB seed buffers are freed after init); new layout **434.44 (mod)**, 10.9 GB over. Per APU at [bs]: layout 96.41 + v-slots 3.44 + runtime about 0.4-0.5 (assumed from g = 2) = about 100.3 GB against `driver used` **100.9 GB (m)**: the v-slots account for B7's 3.3-3.5 GB per APU, about 0.6 GB left unexplained.

#### 5. The 4.08e13 / 576 target with the real v-slot term (TGT17's line, `DM_MN_LEAN=1`)

v-slots **6.707 GB per APU = 26.83 GB per node (mod)**: 2.873 from the 192-group + 3.834 from the 576-group, resident from tree level 8 to the end (below TGT17's assumed 30 GB).

| bar | before (TGT17) | after | margin |
|---|---|---|---|
| (a) device <= 373 - 20 = 353 GB | 345.29 (fits) | **372.12** | **-19.12 GB: FAILS**; +0.88 GB under the raw 373 GB edge (A6, m) |
| (b) node <= 480 GB | 387.37 + 30 (a) | **387.37 + 26.83 = 414.20** | +65.80 GB: fits |

The v-slots are hipMalloc'd device memory, so they count against the device edge; CAP17/TGT17 had counted only planes + arena + ofi pools. Options (mod, device per node at 4.08e13; per-APU sizes exact, performance of each unmeasured): today 6.707 -> 372.12; free each level's slots at its end 3.834 -> 360.62; `DIST_CHUNKS=8` 3.364 -> 358.74; `DIST_CHUNKS=8` + per-level free 1.917 -> **352.96**; depth 1 + `DIST_CHUNKS=8` 2.521 -> 355.37; `DIST_CHUNKS=16` 1.837 -> **352.64**; lower tier arena 210.45 from 3.85e13 6.707 -> 367.82. These led to S36 (share) and C06 (release).

#### 6. Stale sizes (task 2)

`e16_headline.sh`: per-node share 7.0834e10 (ceil(2,266,666,666,671 / 576) = 3,935,185,186 limbs). Re-measured with the plan/layout printers (m): g = 10 -> `E16_DIGITS` **708,340,000,000**, 96 products, 101 pieces, node 386.30 -> 400.08 GB with v-slots, device 345.29 -> 359.06, `E16_BELOW` 687,000,000,000 (step at 6.87 -> 6.88e11: 97 -> 101 pieces), modelled walls `--bw 25` 212.0 s no write / 500.7 s write (`--write-bw 0.1`), `--bw 3.7` 719.5 / 926.7 s; g = 12 -> **850,008,000,000**, 100 products, 130 pieces, node 369.12 -> 380.60, device 328.11 -> 339.59, `E16_BELOW` 840,000,000,000, walls 234.4 / 523.9 s (`--bw 25`), 821.7 / 1032.4 s (`--bw 3.7`).

`mem_model.py` `TARGET_BELOW` is now **3.99e13** (was 4.74e13). Sweep at 576 (TGT17's line): 4.0-4.08e13: 117-119 / 133 pieces, arena 214.75 GB; 3.96-3.997e13: **115 / 131**; 3.9-3.95e13: 111 / 127; <= 3.85e13: 109 / <= 123 with arena **210.45 GB**. The nearest lower tier on the critical path is the step at 3.997 -> 4.0e13. `mn_model.py estimate --target` (mod): 4.08e13 pieces 133, 223.8 s without the write; 3.99e13 pieces 131, 218.2 s.

Labels: measured = the layout/plan prints, STD17's readings, A6's 373 GB edge; modelled = the v-slot term (exact exchange arithmetic, not yet read back from a run), the per-APU comparison, the §5 options, estimate.py walls; assumed = runtime 0.4-0.5 GB per APU, 2 MiB granularity, the fabric rates in the e16 walls.

#### 8. Open items
1. Hardware check needed: a standard 10-node run with `MEM_REPORT_DEVS=1` should show `v-slots hipMalloc'd now` = 3.44 GB per APU after the tree top and `driver - that` about 0.4-0.6 GB; run the GPU gates before merging.
2. The target failed bar (a) with the real term (372.12 vs 353): a decision among §5's options.
3. `ECALC_VSLOT_BUDGET` stayed off by default (no layout decision changes at 3, 6, 10, 576 here).
4. Unchecked: the cut-group branch (e.g. 512 -> 576, or a binary clip at size 14); the 2 x max-slot model is at most one chunk row x C x 8 B high per slot.
5. `mem_per_node` now includes the v-slots (`--p15`'s target row 411.8 -> 438.6 GB, dev_bs 367.4 -> 394.2); depth default 1 -> 2; older notes quoting `mem_per_node` figures are stale by about 4 x v-slots on general-map runs.

## C06 — G5 (X2 cut-group pool-offset asymmetry) and M3 (release the v-slots; S-1 remainder), 2026-10-11 (branch c06 = main bc80001)

#### Verdict
- **G5: it does not bite, on the target or the 576-k schedules; no code fix, no guard.** (1) The target schedule has no cut group. (2) The 576-k schedules with explicit `MN_GROUPS=2,..,64,192,<g>` do have cut groups, but the receiver publishes where each sender's slab lands (`comm_shmem.c` `publish()` -> W_ROFF; the sender puts at `UNOFF(roff)`; OFI the same), so members' pool offsets need not agree. (3) Host emulation: 0 wrong words in every run; offsets equal only for OFI pool + SHARE=1 (the target). The real-transport confirmation (offsets forced to differ with `COMM_LAYER_TEST_SKEW_MB`) was queued (item 70). Caveat: the claim rests on code reading + emulation; a cut group + non-SHARE + X2 can still fragment the pool (a clean "pool cannot hold" fatal, not wrong digits).
- **M3: implemented as `COMM_LAYER_VSLOT_RELEASE=1` (needs X2's `COMM_LAYER_VSLOT_POOL=1`); built-in default since 2026-10-12 (README).** Plan (m, C code, aac7 login node, 576 nodes, 3.71e13): comm_ofi pool **4352 -> 2304 MiB per APU = -8.59 GB per node device** (layout node 386.48 -> 377.89 GB; with `--check-c` exact both ways, node + vslots 400.14 -> 391.55 GB). At 3.82e13 with T2048 + X2 the rank-0 margin 4.3 GB (B01, mod) becomes about 12.9 GB. 4 nodes x 6.441e10 with `DIST_GEN=1`: pool 4608 -> 2560 MiB per APU. Saving = min(staging, slots) per APU wherever the general map runs under X2.

#### G5 detail
Schedules (exact arithmetic of `mn_groups_parse`, `archive:results_raw_2026-10-19/C06/cut_groups.py`): 576 (`...,64,192,576`) has no cut group; default schedules of 560..575 are exact too; explicit `2,4,8,16,32,64,192,<g>` for g = 560..575 has 2..6 layered cut groups (power-of-two cut groups use the plain mesh). The pool rule sets `COMM_LAYER_VSLOT_MB_<size>` for every layered size incl. cut sizes, all equal under SHARE (915..967 MiB), so under SHARE every group allocates the pair once at the same size. Host emulation (`ecalc/tests/lay_host`, aac6 MI250, 3 nodes, 7 shapes): equal pool offsets in every member of every level only for {OFI pool emulation, X2, SHARE=1} with cut groups (shapes 23, 47 (= the 575 shape), 35, 34) and the exact-group control (36); unequal for OFI SHARE=0 and the SHMEM-only pool. 0 wrong words, 0 live bytes after destroy. Without X2 no pool is involved.

#### M3 detail
Under X2 the v-slot pair (1.917 GB per APU at 576) is carved from the comm_ofi pool, sized as staging + slots (2048 + 1830 + 256 -> 4352 MiB). Staging (redistribution, result, shifted add) and slots are never live together within a product (redistribute (staged) -> v-exchanges (unstaged) -> CRT -> result exchange (staged)); only the fixed pool holds both, so per-level release gains nothing (dm is the peak) while release **per product** lets the pool be sized for the larger of the two. Code: `comm_layered_release_vslots(cl)` (frees the SHARE or own pair after completing anything pending; no-op without the switch) called once in `rns_mul_dist_mn` after the prime loop (`if (gen)`); the next product's first v-exchange allocates again at the pool rule's size. Pool law: `binsplit_shmem_pool_need` (OFI: `max(staging, slots)`; SHMEM-only: `max(4 staging, 4 slots)`) and `mem_model.py shmem_pool`. Digits cannot change (scratch, written before read). Fatal if RELEASE without POOL. Risks: every layered product must call the hook (only `rns_mul_dist_mn` uses `lay_get`: yes); a violation is a clean pool-full fatal; the alloc/free bookkeeping per product is cheap; time effect not measured at the time. Host emulation with release (shapes 23/47/35/36, OFI + SHARE=1): VERIFY OK, 0 pool blocks after each release.

Queue gates (aac7 items 70 and 71, dry-run OK; sources in `archive:results_raw_2026-10-19/C06/queue/`): item 70 five 2-node gates at 1e9 with `DIST_GEN=1` (RELEASE, RELEASE + SKEW, control, SKEW alone, RELEASE without SHARE; VERIFY OK and digits identical expected); item 71 4-node memory A/B at 4 x 6.441e10 with `MEM_REPORT_DEVS=1` (expected B -8.6 GB per node device, wall unchanged, mod). Results were collected later and fed the adoption of RELEASE and SHARE as defaults (README, TASKS 27).

## S36 — idea S-1: release (share) the v-slots between the tree levels (2026-10-10)

Branch `s36`, commit db9cb7f. Status at writing: code built on aac7, gates and A/B were measurement pending (job 12455); adopted later as the built-in default `COMM_LAYER_VSLOT_SHARE` (README, 2026-10-12).

Finding that shaped the design: every general-map group owns a layered communicator per APU thread whose two v-slots grow to that group's largest product and stay to the run's end, so levels' slots add up (`binsplit_vslot_bytes`: `lower + max(top, dm)`). The tree runs level after level and a node is in one group per level, so a level's slots are dead when the next level starts; only the whole-machine group is reused. With X2 the slots live in the comm pool; S31 showed that regrowing slots there (free, alloc, staging blocks in between) fragments the first-fit pool (S28's 10-node failure), so S36 does not free per level.

Design `COMM_LAYER_VSLOT_SHARE=1`: one pair of slots per device (`g_vsh[dev]` in `comm_layered.c`) shared by every layered communicator of that APU thread; `need_vslot_shared` completes the previous owner's pending v-exchange on hand-over (`complete_v`), grows only if a level needs more, frees with the last communicator (refcount); works with the pool (X2) and with hipMalloc. Pool rule (`vslot_prealloc_env`): under SHARE every `COMM_LAYER_VSLOT_MB_<g>` is set to the overall per-slot maximum (allocated once, no regrow). Model: `binsplit_vslot_bytes` returns `max(largest lower level, top, dm)` instead of the sum; `mem_model.py vslot_resident` likewise. Digits cannot change (only where buffers live and when reused).

Modelled saving (mod, `DIST_CHUNKS` 8 = the code default):

| case | v-slots per APU, sum -> max | saving per node |
|---|---|---|
| 576 nodes, 3.71e13, `...,64,192,576` (default) | 3.364 -> 1.917 GB | **5.79 GB** |
| 576 nodes, `...,64,576` | 1.917 -> 1.917 | 0 (one general level) |
| 10 nodes x 6.441e10 | 1.724 -> 1.724 | **0** (one general level) |
| 4 nodes x 5e10, `DIST_GEN=1` (the A/B) | 4.31 -> 2.16 | 8.6 GB (test case only) |

The idea table's -11.5 GB (S-1) was for `DIST_CHUNKS` 4; DC8 halves it (5.8 GB). Saving is device memory at the 576 target only (smaller comm pool under X2; smaller hipMalloc budget otherwise).

Test plan (as armed): gates at 2 and 4 nodes with `DIST_GEN=1` (needed because at 4 nodes every group is a power of two): 1e9 digits identical; A/B ABBA at 4 nodes x 5e10 digits, A = base + X2 + `DIST_GEN=1`, B = A + SHARE, expectation (mod) B-A peak device about -8.6 GB per node, wall about 0 (the test case's 5e10 per node: modelled A peak 333 GB). Not tested at scale at writing: 576-node behaviour (cut last group, pool symmetry across group sizes), later covered by S45 and C06.

## S45 — CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e)

Verdict: **SHARE=1 is effective and safe on the target launch line** (COMM_OFI=1, no X2). Plan and layout change only the v-slot term; a many-rank host test (up to 600 ranks, non-power-of-two groups, a cut last group) found no wrong data, no leak and symmetric pool offsets. It is a TCP / host emulation, not the real SHMEM / OFI transports.

1. Login-node plan / layout (m, C code; `archive:results_raw_2026-10-19/S45/`; target launch block of main 13c244e, `MN_PLAN_ONLY=37100000000000:576`, `BS_LAYOUT_ONLY=64409722236:576`): levels 1-6 are exact power-of-two groups; **level 7: 3 groups of 192; level 8: the whole 576**; only levels 7, 8 and dm own v-slots; the target has **no cut last group**. v-slot per APU: level 7 1.447 GB, level 8 / dm 1.917 GB; SHARE=0 sum 3.364 = 13.46 GB per node, SHARE=1 max 1.917 = 7.67 GB per node: **saving 5.79 GB per node** (S36's model). Plan check OK both ways (1236 products, 103 / 107 pieces unchanged), only `vslot` and budget lines change. With OFI pools counted (`mn_model.py estimate --target`, mod): **357.5 -> 351.7 GB per node vs the 373.44 GB edge (margin 15.9 -> 21.7 GB)**, node 400.8 -> 395.0 GB, times unchanged. X2 (not on the launch line): the pool rule sets MB_192 = MB_576 = 915 MiB under SHARE (691 / 915 without), SHMEM pool 23040 -> 17664 MiB, node 391.7 -> 386.0. `vslot_resident` (mod): one general level or 512 / 256 as the last lower level: 0 saving; 3 general levels (`...,96,192,576`): 4.811 -> 1.917 GB = 11.58 GB per node.
2. Many-rank host simulation (`ecalc/tests/lay_host/`, new; no product code changed; compiles a rewritten copy of `comm_layered.c` against a host shim, one process per APU, TCP meshes, per level batches of unequal v-exchanges with data checked word for word, owner hand-over with the previous owner's exchange pending, symmetric-pool emulations `ofi` (top-down last fit, the launch line) and `shmem` (first fit, control blocks in the pool)). Shapes: 36 nodes `12,36` (144 ranks) and `6,12,36` (peak live 4.81 -> 3.83 MB per process with SHARE); 72 nodes `24,72` (2.70 -> 2.38 MB); **144 nodes `48,144` (576 ranks, the 192/576 shape): 6/6 VERIFY OK, peak 2.686 -> 2.368 MB per process (-11.8 %, m), symmetric on both pool emulations**; cut shapes 39 / 150 nodes (156 / 600 ranks) and 15 nodes `6,15`: no-X2 OK, X2 OFI OK with SHARE=1 (symmetric), SHARE=0 asymmetric (288 mismatches at 15 nodes: a hazard of the existing X2 path, not of SHARE), SHMEM single pool asymmetric for SHARE 0 and 1 (control blocks differ by group size, assumed sizes; one run lost to a TCP timeout). Every run: 0 wrong words, 0 live bytes and 0 live symmetric blocks after destroy. Neither asymmetry occurs on the target (no cut group, no X2); C06 shows offsets need not agree anyway.
3. Without X2 the v-slots are hipMalloc'd, so SHARE acts identically (-5.79 GB per node, mod; counted in the node budget with `ECALC_VSLOT_BUDGET=1`).

Doubts: kernels, one-sided puts and the real pool code are not exercised; sizes small; the target numbers are modelled except the layout lines. Rerun: `ecalc/tests/lay_host/build.sh`, `s45_all.sh <t_lay> <outdir>`.

## S52 — more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9)

CPU only (model scripts and the aac7 login node's plan/layout printers); no engine code changed. Labels m / mod / a. Target launch block of `archive:results_raw_2026-10-19/S45/launch_block_main13c244e.sh`.

#### Verdict
1. **Largest safe size: 3.82e13 digits** (+3.0 % over 3.71e13; 3.80e13 has the identical layout and margin). Device peak 357.9 GB per node (mod, `estimate.py`, SHARE=1), 360.0 GB for rank 0 (+2.1 GB allowance) against the 373.44 GB edge: **margin 13.4 GB (3.6 %)**, above M1's 10 GB rule; node 402 of 480 GB (mod); 185.9 s without the write vs 180.3 s at 3.71e13 (mod).
2. **Do not go above 3.82e13**: at 3.83e13 the C plan adds pieces (largest tree group 42 -> 54 pieces, 109 -> 121 in all) and the model's wall jumps +20 s (185.9 -> 206.1 s, +11 %) for +0.3 % digits; from 3.88e13 the device peak steps to 366.5 GB (margin 4.8 GB with rank 0); 4.08e13 is over the edge (375.1 GB, -3.8 GB).
3. **Messages:** 2.65 M per APU at 576 (mod): reciprocal 1.098 M, division 0.927 M, tree 0.624 M; 93-95 % are transform-chunk messages (K = 8 chunks x 575 peers). Memory-neutral coalescing saves 0.48-0.66 M messages (18-25 %) = **1.0-1.4 s at 2 us, 2.4-4.5 s at 8.5 us, 3.7-8.4 s at 20 us** (mod). Larger savings need memory or are lost.

#### 2. Layout and plan at 576 nodes (all plans `plan check OK`, `ECALC_NP=auto`, 1236-1238 products)

| digits (e13) | pieces total / tree largest (m) | C layout device_with GB (m) | est. device GB (mod) | rank 0 (+2.1) | margin to 373.44, rank 0 | est. node GB of 480 | wall no write s (mod) |
|---|---|---|---|---|---|---|---|
| 3.71 | 103 / 107 | 344.37 | 351.7 | 353.8 | 19.6 | 394 | 180.3 |
| 3.80 | 109 / 109 | 348.66 | 357.9 | 360.0 | 13.4 | 402 | 185.2 |
| **3.82** | 109 / 109 | 348.66 | 357.9 | 360.0 | **13.4** | 402 | **185.9** |
| 3.83 | 109 / 121 | 348.66 | 357.9 | 360.0 | 13.4 | 402 | 206.1 |
| 3.86 | 109 / 125 | 348.66 | 359.7 | 361.8 | 11.6 | 402 | 211.7 |
| 3.88 | 109 / 125 | 348.66 | 366.5 | 368.6 | 4.8 | 411 | 211.9 |
| 3.90 | 111 / 127 | 352.96 | 366.5 | 368.6 | 4.8 | 411 | 215.8 |
| 4.08 | 119 / 133 | 352.96 | 375.1 | 377.2 | **-3.8** | 419 | 226.7 |

The C layout steps by one 4.3 GB arena chunk between 3.88 and 3.90e13; the model steps slightly earlier and is the conservative basis (on the exact C-layout basis the rank-0 margin at 3.82e13 is 22.7 GB). The estimate's device figure carries +7.3 GB over the C layout (bs phase and other terms). The rank-0 allowance is applied in the unfavourable direction. The 373.44 GB edge was measured on the target (n = 5, TGTBENCH2). Part-file volume 0.444 B/digit = 17.0 TB (mod). Recommendation: request **38200000000000** (6.632e10 per node); 3.80e13 has the same layout and 0.7 s less. M1's earlier "+6.5 % digits" ignored the plan step at 3.83e13 and the arena step at 3.88e13.

#### 3-4. Messages and schemes (mod; 47 GB/s per APU, 6.441e10 digits per node, base wall 219.0 s no write)

Per APU: tree levels g = 2..64: 48 120 msgs; g = 192: 229 582; g = 576: 346 150; reciprocal (49 pieces) **1 098 275**; division (16 pieces) **927 475**; total **2 649 602**, 2 475 035 (93 %) transform-chunk messages. Messages already go one per node (four slabs aggregated per destination node); what multiplies the count is K chunks x n_tr exchanges per piece x 575 peers. The reciprocal's messages are small (0.016-1.05 MB; 0.6 M are <= 0.5 MB) because its operands are 4-64x below the cap while K stays 8; the division's and the g = 576 level's slabs (478-537 MB) are already at the cap.

| scheme | memory | msgs per APU | wall @2 us (saving) | @8.5 us | @20 us |
|---|---|---|---|---|---|
| base (K = 8) | - | 2 649 602 | 219.0 | 237.5 | 268.4 |
| S1 coalesce to the slab bound (general-map groups only) | none | 2 173 502 | 218.0 (-1.0) | 235.1 (-2.4) | 264.6 (-3.7) |
| S1 on all groups | none | 1 990 999 | 217.5 (-1.4) | 233.0 (-4.5) | 259.9 (-8.4) |
| S2 bundle the piece's n_tr exchanges | none | 1 954 320 | 217.6 (-1.4) | 233.4 (-4.1) | 260.7 (-7.6) |
| S3 uniform K = 4 | +13.4 GB per node (the whole margin) | 1 460 970 | 216.5 (-2.5) | 227.9 (-9.6) | 247.1 (-21.2) |
| S3 K = 2 | more (a) | 842 203 | 215.3 (-3.7) | 223.0 (-14.5) | 235.7 (-32.6) |
| S4 3-layer relay (G = 64) | not modelled | 864 083 | 290.9 (+72.0) | 298.4 (+60.9) | 311.0 (+42.7) |

S1/S2 are bounded by the 1.2 M messages already at the slab bound; the total latency bill is 2.65 M x lat = 5.3 / 22.5 / 53.0 s. S4 loses (NIC bytes double 13.7 -> 27.9 TB per node, mod): rejected. S3 trades against digits (13.4 GB about 2.3e12 digits, +6 %, about +13 s): pays only above about 12 us per message. Per-message costs: 2 us (a, the design value), 8.5 us (m, aac7 at 4-10 nodes), 20 us (pessimistic).

#### 5. Design (not implemented): `DIST_COALESCE=1`
Replace the fixed K by K' = min(K, 2^ceil(log2(ceil(slab_total / S_max)))) per transform exchange (S_max = the run's largest chunk slab, 536.9 MB at the cap 2^31) on the general map only (groups 192 and 576, the reciprocal's g = 576 steps), bundle a piece's n_tr exchanges when their sum fits S_max, group the single-chunk redistribution exchanges A and B; memory-neutral by construction, digits unchanged; gates: `tools/gate.sh --build` 1 and 2 nodes, e9, 4e10, a forced 4-node general-map run with `COMM_XSTATS=1`. The saving cannot be measured below the target (message count grows with g: 30k at 10 nodes, 2.65 M at 576). Expected 0.5-2.8 % of the wall (mod).

#### Decision 2026-10-11: DIST_COALESCE skipped
The user decided not to build `DIST_COALESCE` unless the target's per-message cost is shown to be >= 8.5 us (assumed 2 us: saving 1.0-1.4 s, 0.5-0.7 %). No code, no switch row. Reopen only with a measured target per-message cost of 8.5 us or more (then the section 5 gates apply).

Doubts: the rank-0 sign convention is read two ways in the sources (both give a margin above 10 GB at 3.82e13); the model's time knee at 3.82 -> 3.83e13 is plan step (m) + model pricing (mod) and should be rechecked after the S48 refit; the estimate's +7.3 GB means the true margin may be 2-5 GB larger; overlap loss from a smaller K is not modelled (a). Next: the user decides 3.82e13 vs 3.80e13 (TGT, results/TARGET.md).

## B06 — uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001

Question: does a run with an UNEVEN last group (the "exclude k bad nodes" case of results/NODE_SLOWMODE.md §B04, branch b04) work and give correct digits?

Code (read): `mn_groups_parse` accepts an explicit increasing list where each entry is a multiple of the previous one EXCEPT that the first entry >= size is replaced by `size` with no multiple check, and `size` is appended if missing. So `MN_GROUPS=2,3` at 3 nodes, `2,5` / `4,5` at 5 nodes, `...,192,573` at 573 nodes are accepted. Plan checks (m, login node, 6.441e10 digits per node) all pass: 3 nodes default 98 pieces and `2,3` 98; 5 nodes default 85, `2,5` 91, `4,5` 85; 6 nodes default 108.

Correctness (m, 3 nodes, aac7 line, digits compared with `ecalc/digcmp.sh`): 1e9 default and `2,3`: VERIFY OK, identical; 1e10 `2,3`: VERIFY OK (44.1 s), identical. (First 1e10 attempts said DIFFERS because the prefix reference was still being written; rerun identical.) 5 nodes (queue item 47, all rc 0, every run `all 5 nodes: VERIFY OK`): 1e10 with `MN_GROUPS=2,5` with digit write: VERIFY OK, 41.59 s, `digcmp: identical`. 9 and 10 nodes were already VERIFY OK with `3,9` / `4,10` (RESULTS Phase 13 N).

Timing (m, 3.2205e11 digits = 6.441e10 per node, no digit write, one run per schedule, NOT paired ABBA so no CI) vs model (`estimate.py`, aac7_s48 fabric):

| nodes | schedule | measured `total` | modelled |
|---|---|---|---|
| 3 | default | 210.2 s | 237.2 |
| 5 | default | 213.15 | 297.9 |
| 5 | `2,5` | 226.23 (+13.1 s, +6.1 %) | 273.0 |
| 5 | `4,5` | 232.18 (+19.0 s, +8.9 %) | 247.8 |

The model overpredicts the absolute time by 40 % (default) and gets the ordering wrong (it says uneven is 8 % / 17 % faster; measured: uneven slower by 6-9 %). Rate check: all three 5-node runs were in the NIC slow mode (mean all-to-all rate per APU thread 10.25 / 9.61 / 9.57 GB/s, slow < 11.95) and carry the single-node-chain flag, so no clean fit row; if the total scaled as 1/rate, the gaps (13, 19 s) are within what the rate difference alone could explain.

**Verdict:** correctness of uneven groups at 3 and 5 nodes is shown (VERIFY OK; digits identical at 1e9 / 1e10). A speed gain is NOT shown and the modelled 8-17 % gain is not reproduced at one run each: do not use explicit uneven groups for speed on this evidence; for "exclude a node" they cost at most about 9 % (m, single run, partly rate noise). A paired ABBA (default vs `2,5`, 3 rounds, about 25 min per round) would be needed for any claim either way. Fallback policy if never measured: round down to a friendly count (576 stays; a bad node -> 575 is the poor case, default modelled +34 %), or use explicit groups and accept the modelled estimate. Not shown: speed at real size beyond the above, and a 5-node case with two uneven levels.

## O1 — overnight 6-node paired ABBA, MN_GROUPS `2,6` (default) vs single level `6` (TASKS 42)

DONE 2026-10-12 01:40 ET (aac7 queue item `60_o1_abba_groups`, main 0298fe2, marker SUCCESS). Question: is the single 6-way level faster than the default `2,6` at 6 nodes? Earlier evidence (§C09 f3 pairs, 5+5 runs): `[6]` faster by 17-24 s, SE 12-14 s (m); the model says the opposite (-9 s, mod).

Design: arms A = default (`2,6`), B = `MN_GROUPS=6`; 6 nodes, 6.441e10 digits per node (3.8646e11), no digit write, f3's production line (shmem, T2048, FIT, X2, VSLOT pool/share, DM_MN_LEAN, ROUND_MB 1024, `COMM_XSTATS=1`). One round = a quartet, ABBA then BAAB alternating; covariates per run: NIC MCU counter group (A slow / B fast, drawn per launch, not controllable), flow-control stalls, mean all-to-all rate r, init time (cold start if init > 36 s). Estimators (`tools/o1_stats.py`, t, 95 % CI): raw (mean of round differences), nic-adjusted OLS, rate-adjusted OLS (`total = c + d[B] + b/r`), each also without cold starts. Pass reading: `[6]` faster if CI excludes 0 with d <= -8 s; the model's order reproduced if d >= +8 s; else inconclusive.

Results (m): 24 runs launched 00:00-01:40 ET (about 4 min each), 23 valid (run 2, B round 1, failed; run 1 was a cold start: init 41.7 s, total 338.85 s); stop rule did not fire (p 0.350, half-width 6.3 s); the time cap ended the loop. B - A (negative = single level faster):

| estimator | runs | B-A s | 95% CI s | p |
|---|---|---|---|---|
| raw (5 round differences) | 23 | +2.4 | [-3.9, +8.7] | 0.350 |
| NIC-group adjusted | 23 | -5.8 | [-22.9, +11.3] | 0.488 |
| rate adjusted (1/r) | 23 | -22.0 | [-36.8, -7.2] | 0.006 |
| raw, no cold start | 22 | +2.4 | [-3.9, +8.7] | 0.350 |
| NIC adjusted, no cold start | 22 | +2.4 | [-5.7, +10.4] | 0.546 |
| rate adjusted, no cold start | 22 | -7.8 | [-15.3, -0.3] | 0.042 |

Arm statistics: A `2,6` n = 12, mean 255.3 s, median 246.2, sd 26.9 (without the cold start n = 11, mean 247.7, sd 6.0), mean rate 8.24 GB/s per thread; B `6` n = 11, mean 252.0, median 246.4, sd 13.0, rate 7.65. Per-round B-A (s): +9.9, +0.4, +2.8, +3.0, -4.1. NIC mix: group B (fast) runs A 245.6 (n = 7, sd 5.8) vs B 244.0 (n = 5, sd 5.5); group A (slow) runs A 268.9 (n = 5, includes the cold start) vs B 258.6 (n = 6, sd 14.1); fitted group effect +19.1 s (all) / +10.4 s (no cold start).

**Verdict: inconclusive, no measurable difference in the raw totals; the earlier "17-24 s faster" is not reproduced.** None of the six rows meets the pass bar (the rate-adjusted no-cold row has p 0.042 but CI reaching -0.3 s and point estimate -7.8 s, just short of 8 s). The rate-adjusted estimates are not independent evidence (r is partly a result of the arm: B has the lower mean rate, as L4 found); one p = 0.042 among six rows with a CI touching 0 is not a result. Warm group-B runs of both arms sit at 244-246 s; the default `2,6` is not slower at 6 nodes, and its larger spread in L4 came mostly from the slow NIC group and the cold start.

Recommendation (for the user, not adopted): keep the default `2,6` at 6 nodes (no change, no evidence for a gain); `MN_GROUPS=6` has no measured gain (only the rate-adjusted sensitivity and lower spread); more rounds would need about 12 more warm rounds for a half-width near 3 s. At 576 nodes (mod, L4) the standing `2,4,...,64,192,576` is 176.3 s at 3.71e13 and no explicit schedule is better by more than 0.2 s; a single-level form is not applicable there, so the 6-node result neither supports nor refutes the model's tree-level terms. Files: `tools/o1_item.sh`, `tools/o1_stats.py`.

## C09 — best MN_GROUPS for every usable node count (N = 576 .. 544), 3.71e13 and 3.79e13 digits

Wave C, agent c09, 2026-10-11. All times and GB **modelled** (standing model, `ecalc/estimate.py` / `mn_model.py`); "plan OK" is **measured** (C planner, aac6 MI250 CPU node). No GPU, no aac7 (except the final aac7 check below).

#### Method
Tool `tools/groups_table.py` (`search`, `plancheck`, `maxdigits`, `table`). Model line = the target line (profile `target`, layv factor 0.655, `MN_T_CHUNK_MB=2048`, X2, SHARE, FIT, `ECALC_NP=auto`, `BS_ARENA_ROOM=0.16`, 2^31 cap, round 1024); total digits fixed (3.71e13 or 3.79e13). Check: reproduces §B01 memory (576 nodes, 3.71e13: device 357.9 GB; margin 373.44 - (357.9 + 0.5 + 2.1) = 13.0 GB) of results/LAUNCH_DEFAULTS.md; wall no-write **T2048** (183.7 s at 576 default; B04's 189.1 s is the T1024 figure). Candidates per N (about 440 plus the default): power-of-two prefix 2..2^a, then one further level (ratio <= 12) or two (ratio <= 8), then N as an uneven last group; rule: margin >= 10 GB and node total <= 480 GB. **All 224 top candidates passed `plan check OK`** (m, 4 jobs).

#### Findings
1. **Memory, not time, limits how many nodes can be excluded.** At fixed 3.71e13 the 10 GB margin holds down to N = 554 (N = 553..544 fail: margin <= 4.0 GB, arena step). At 3.79e13 only N = 576..569 and 558 pass. The size-vs-N limit is a staircase (arena step), not monotone.
2. **The default schedule is bad at almost every N != 576** (modelled 195-2500 s). Two best explicit forms: the *standing form* `2,4,8,16,32,64,192,N` (B04's): 184-211 s at 3.71e13 for N = 576..566; the *model-best* uneven top, e.g. `2,4,8,16,80,560,N`: **138-155 s at N = 576..570, 127 s at 569**, 13-21 % below the standing form, even at 576 (146.5 s vs 183.7 s).
3. **Doubt (important): the model-best forms are extrapolations.** No measurement exists of 5-way or 7-way levels, 80/560-node groups, or a top whose children differ 35x; the model rewards fewer levels and unbalanced tops (fewer pieces). The planner accepts them (m) but no run exists. Balanced multi-way forms (`5,25,175,576`: 147.3 s at 576) are also below the standing form because of fewer levels. **Until measurements are in, the safe choice is the standing form (pow2 prefix, 192, N); the model-best is the upside.**
4. **Round down?** The modelled wall is not monotone in N even for the standing form (3.71e13: 183.7 s at 576, 211.3 at 575, 194.2 at 573, 184.4 at 570, 177.3 at 569, 166.7 at 561, 175.5 at 554); with the model-best form 569 (127.3 s) beats 576 (146.5 s) by 19 s. Part is real structure (fewer pieces where group sizes fit the cap), part the extrapolation of finding 3; act only after measurement. Going down costs digits per node (margin falls 0.3 GB per 6 nodes).
5. N = 575 (b04): explicit `2,4,8,16,32,64,192,575` 211.3 s (T2048; B04: 216.6 s T1024), default 245.4 s (B04: 252.8).

Condensed table (mod; 3.71e13, "std form" = `2,4,8,16,32,64,192,N`; full tables for 3.71e13 and 3.79e13 incl. next-best, balanced-top and advice columns are in `archive:results_raw_2026-10-19/C09_groups_table.csv`):

| N | best `MN_GROUPS` | wall s | margin GB | std form s | default s |
|---|---|---|---|---|---|
| 576 | `2,4,8,16,80,560,576` | 146.5 | 12.7 | 183.7 | 183.7 |
| 575 | `2,4,8,16,80,560,575` | 155.0 | 12.7 | 211.3 | 245.4 |
| 573 | `2,4,8,16,80,560,573` | 140.6 | 12.7 | 194.2 | 854.4 |
| 570 | `2,4,8,16,80,560,570` | 132.8 | 12.7 | 184.4 | 232.3 |
| 569 | `2,4,8,16,80,560,569` | 127.3 | 12.3 | 177.3 | 2482.5 |
| 566 | `2,4,8,16,80,560,566` | 137.1 | 11.2 | 183.9 | 1229.1 |
| 561 | `2,4,8,16,112,336,561` | 141.6 | 10.3 | 166.7 | 241.0 |
| 560 | `3,18,144,560` | 170.0 | 11.2 | 184.3 | 195.1 |
| 557 | `2,4,24,144,557` | 177.2 | 10.1 | 179.2 | 2452.1 |
| 554 | `7,14,112,554` | 181.8 | 10.2 | 175.5 | 1227.0 |
| 553..544 | infeasible (margin < 10 GB; e.g. best ignoring memory at 553: 141.2 s, margin -4.6 GB) | | | | |

At 3.79e13: 576 `2,4,8,16,80,560,576` 153.0 s (std 188.5); 575 `7,21,168,575` 162.6 s; 571 `5,30,120,571` 162.7 s; 569 `3,21,147,569` 172.6 s; N = 568..559 and 557..544 infeasible; 558 default only (292.1 s).

Reproduce: `python3 tools/groups_table.py search --nmin 544 --nmax 576 ...`, then `plancheck`, `maxdigits`, `table` (about 90 s per quarter on a 32-core MI250 node).

Open: model-best uneven forms unmeasured (suggested b06 check: paired ABBA at 6-8 nodes); the search space is bounded (<= 3 levels beyond the prefix); memory margins use the model (+0.5 GB X2, +2.1 GB rank 0), the C layout of the picked schedules was not run; T1024 walls are a few seconds higher.

#### aac7 test of the structural claim (item `48_c09_sched6`; 6 nodes, 6.441e10 per node, no write, production line)
Model predictions written before the run (mod, T2048): A `2,6` 75 pieces **292.1 s** (profile aac7_s48; target profile 124.6 s), B `6` 77 pieces 308.4 s (+16.3), C `3,6` 79 pieces 308.1 s (+16.0): at 6 nodes the model does NOT reward fewer levels. Pass rule: model reproduced if A < B, C by >= 8 s after rate adjustment; refuted if B or C faster by >= 8 s; else inconclusive.

#### Single-level grouping, paired confirmation (f5 collected 2026-10-11 21:05 ET; runs by f3, queue items 80-83)
Setup (m): 6.441e10 digits per node, no write, production line; arm A default, arm B single level (`MN_GROUPS=6` at 6 nodes, `=4` at 4 nodes); two ABBA/BAAB sets per size; the first run of each 6-node set was a cold start (init 56.5 and 44.4 s vs 27-29 s) and is left out of "clean" means.

| size | A (default) clean runs, s | B (single level) clean runs, s | mean A | mean B | B - A (SE) |
|---|---|---|---|---|---|
| 6 nodes | 223.6, 271.5, 258.0, 271.5 (304.4 cold) | 231.3, 234.3, 238.2, 252.8 (254.1 cold) | 256.2 | 239.2 | **-17.0 s** (all 5+5 runs: -23.7, SE 13.9) |
| 4 nodes | 210.1, 183.0, 193.7, 207.7, 211.6 | 209.8, 202.4, 181.3, 233.7, 205.7, 208.8 | 201.2 | 206.9 | **+5.7 s** (SE 8.8) |

Rate (m): 6-node r 9.4-10.5 GB/s, mean 10.1 (A) vs 9.6 (B). 1e10 correctness with `MN_GROUPS=6` (6 nodes, with digits): VERIFY OK and `digcmp` identical in both sets. At 4 nodes the NIC-group split was uneven (B arm 4 of 6 runs in the slow group), worth about 20 s alone, so +5.7 s is confounded. **Verdict then: 6 nodes - the model's ranking is not reproduced, `[6]` faster by 17-24 s (>= the 8 s bar, A sd 29 s, B sd 11 s; same sign in 9 of 10 runs); 4 nodes inconclusive.** This agrees with L4 (results/TIME_MODEL.md §L4: -22.7 s, SE 11.8, from 148 pooled rows incl. these runs) but is the same evidence seen twice, about 1.7-2 SE from zero. **Superseded by §O1** (the overnight paired ABBA: raw +2.4 s, inconclusive).

## c09_groups — the generated C09 table

(former file: c09_raw/groups_table.md; the original had no title.) The full tables (3.71e13 and 3.79e13 digits, columns best `MN_GROUPS`, wall, margin, plan, next-best, balanced-top best, default, std form, advice) duplicated §C09 and are kept as `archive:results_raw_2026-10-19/C09_groups_table.csv`. Advice column in one line: at 3.71e13 the model-best table recommends rounding down to 569 (127.3 s, -19.2 s vs 576) and N <= 553 has no schedule with the 10 GB margin ("use 569 instead"); at 3.79e13 rounding down to 571 or 569 only helps by 3.9-11.1 s and N = 568..559 and 557..544 are infeasible ("use 576 instead", 153.0 s). All modelled; the model-best forms are unmeasured (§C09 finding 3).

## C215 — the paired series for D1 (`RNS_AUTO_PIECE_COST`, C2), with `DIST_TWREC`, other sizes, and the 576 model (Phase 15, row 1e, agent C2)

Branch `p15-C2` from int15 0b80ca3 (= B0), no pipeline code changed. Labels: measured (aac6 node, one node pinned for the whole series), modelled (`MN_PLAN_ONLY`, `mn_model.py` / `archive:results_raw_2026-10-19/C215/c2_grid.py`). Scripts and logs in `archive:results_raw_2026-10-19/C215/`.

Method: 23 runs on one aac6 node (every job checked `git rev-parse HEAD:ecalc` and binary newer than source), `env RNS_AUTO_PIECE_COST=<0|1> [DIST_TWREC=1] ECALC_VERBOSE=2 RNS_VERBOSE=1 ./ecalc <D> [file]`. Wall with the write = process wall incl. the part file and the top set; wall without = no outfile. Digits hashed on the node (`dd iflag=direct | sha1sum`) against the 1e11 and 4e10 references (and the pair's own hashes at 1.3e11). Order: cycles of C2 off, on, on + `DIST_TWREC=1` (x3) then off/on (x2): 5 off, 5 on, 3 TWREC with the file; then 3 off/on pairs without the file; one 4e10 pair; one 1.3e11 pair.

#### 4. The 576 target (done first: login node)
C2 lives only in `rns_dist.c` `split_grid` (auto's grid for a node-local product). The mn tier uses `split_grid_cap`, which C2 does not change. At 576 nodes only each node's leaf mdev levels (above 2^30 limbs) and the reciprocal's single-node start chain use `split_grid`. `MN_PLAN_ONLY=42500000000000:576` (with `MN_GROUPS=2,4,8,16,32,64,192,576 MN_TOPO_GROUP=0`), off and on: **logs byte-identical** (mod, node 0's leaf only). Every node's leaf (`c2_grid.py`, a Python port of `split_grid` matched to V2's 1e11 plan table and to node 0 at the target): 311 of 576 nodes' leaf grids change (levels 24/25), per-node leaf big-product time -0.38..+0.40 s, but **the slowest leaf (node 575, the top node) is 17.97 s with and without C2**, so the critical path does not change: `mn_model.run(TARGET, 4.25e13/576, 576)` **233.9 s -> 233.9 s (mod; 0.0 s)**. On B0's model (main 7367e25, incl. the Lustre write rate): 4.25e13 / 576 wall 349.7 s both (compute part 226.7 -> 226.7; the slowest leaf 18.31 s both); size 1 (mod): 4e10 126.4 -> 125.9 s, 1e11 348.6 -> 343.2 (compute 216.6 -> 209.3), 1.16e11 433.8 -> 417.5, 1.3e11 493.4 -> 481.0. **Verdict: C2 is neutral at the 576 target (0.0 s, mod); its gain is a one-node gain (>= 4e10 per node on one node).**

#### 1. The paired series at 1e11 (m, aac6, 23 runs on one node, all VERIFY OK, rc 0; all 13 digit files identical to `e_1e11.out`, sha1 578f5efb...)

Every C2-off run took the same **2 up-corrections** ("redo": T1 24-25 s wait, 0/2) and every C2-on run took none: the count is a deterministic property of the digits and the grid (A_h mu 7 x 7 with 21 pieces skipped under the low cut vs 5 x 6 with 10 skipped), not run-to-run noise.

| quantity | C2 off (n 5) | C2 on (n 5) | on - off |
|---|---|---|---|
| `total` (file runs) | 234.62 +- 1.62 | 203.46 +- 1.20 | **-31.15 +- 0.90 s (-13.3 %)** |
| wall with the file | 296.28 +- 1.78 | 230.99 +- 0.70 | **-65.29 +- 0.86 s (-22.0 %)** |
| bs | 93.36 +- 0.35 | 92.78 +- 0.75 | -0.59 +- 0.37 |
| dm | 99.35 +- 0.49 | 93.33 +- 0.22 | **-6.02 +- 0.24** |
| of which recip | 41.26 +- 0.18 | 38.46 +- 0.11 | -2.80 +- 0.10 |
| T1 | 24.44 +- 0.63 | 0.00 | -24.44 +- 0.28 |
| dc (after `total`) | 59.89 +- 0.35 | 25.75 +- 0.87 | -34.14 +- 0.42 |
| no file: `total` (n 3) | 210.93 +- 0.65 | 202.80 +- 1.41 | **-8.14 +- 0.89 s (-3.9 %)** |
| no file: wall (n 3) | 217.91 +- 0.57 | 204.47 +- 1.41 | **-13.43 +- 0.88 s (-6.2 %)** |
| no file: dm (n 3) | 99.50 +- 0.25 | 93.05 +- 0.19 | -6.45 +- 0.18 |

(mean +- sigma; difference +- standard error; init 15.5-19.1 s, bs 92-94 s per run.) Grid gain vs correction effect: the grid itself gives dm -6.0 s (recip -2.8, division -3.2) and bs -0.6 s, **about -6.6 s of `total` (-2.8 %)**; the no-file series agrees (`total` -8.1 s, of which dm -6.45, bs -0.7, T1 -0.64); the model predicted -7.3 s. The rest is the two corrections C2's grid avoids at this size (T1's writer wait -24.4 s in `total`; dc's redo and file rewrite -34.1 s after `total`; together about -58.7 of the -65.3 s wall with the file; without a file the redo costs only 5.3 s). Row 1d (K, a correction without a rewrite) would remove most of this part on the defaults too. C2 does not guarantee zero corrections at every size. **For D1 (m): C2 on is faster in every paired comparison at 1e11 (5/5 file pairs, 3/3 no-file pairs), digits identical in every run.** (The 240.3 s standard of §36.1 was measured on a different node; here C2 off = 234.6 s.)

#### 2. DIST_TWREC=1 together with C2 (m, interleaved in the series, n = 3)
`total` 201.58 / 200.84 / 199.49 s, all identical to `e_1e11.out`; vs C2 alone (n 5): 200.64 +- 1.06 vs 203.46 +- 1.20, **-2.83 +- 0.81 s (-1.4 %)**; dm -1.96 +- 0.14 s (recip -0.76 +- 0.08), bs -0.37 +- 0.37; wall with the file -1.54 +- 0.80 s because dc was +1.29 +- 0.51 s. V2 expected < 1 % on the defaults, more with C2. **Verdict: a candidate switch together with C2 (-1.4 % of `total`), for the user; not required for D1.**

#### 3. Other sizes (m, one pair each)

| digits | switch | total | bs | dm (recip) | corrections | wall with file | digits |
|---|---|---|---|---|---|---|---|
| 4e10 | off | 65.25 | 31.2 | 22.8 (9.6) | 0/0 | 80.5 | identical to `e_4e10.out` (43c84e49...) |
| 4e10 | on | 65.53 (+0.4 %) | 30.8 | 22.2 (9.4) | 0/0 | 81.7 | identical |
| 1.3e11 | off | 325.09 | 142.3 | 163.9 (59.5) | 0/0 | 347.4 | b1946e71..., VERIFY OK |
| 1.3e11 | on | **302.69 (-6.9 %)** | 136.0 | **147.8** (59.6) | 0/0 | **332.0 (-4.4 %)** | same hash |

4e10: neutral (dm -0.6, total +0.3 s, within the +-1.5 s spread; one pair). 1.3e11: -22.4 s of `total` (V2: -20.9 s, -6.4 %), no corrections on either side, so grid gain alone (dm -16.1 s, bs -6.3 s); dc +6.9 s with C2 (V2: +3..7 s), wall with file -15.4 s; model predicted -24.9 s for the compute part.

#### Conclusions
1. **D1: confirmed.** 1e11: `total` 234.62 +- 1.62 -> 203.46 +- 1.20 s (**-31.2 +- 0.9 s, -13.3 %**); wall with the digit file 296.3 -> 231.0 s (**-65.3 +- 0.9 s**); wall without 217.9 -> 204.5 s (**-13.4 +- 0.9 s**); all 13 file runs identical to the reference. About 6.6 s of `total` is the grid; the rest is the two corrections C2's grid avoids at this size.
2. `DIST_TWREC=1` with C2: -2.8 +- 0.8 s more, identical digits (separate optional adoption).
3. Sizes: neutral at 4e10 (one pair); -6.9 % at 1.3e11 (grid only).
4. 576 target: unchanged (0.0 s modelled; plan-only identical); adoption is safe for the target and does not move the target estimate.

Open issues: (a) the correction share of the 1e11 gain is data-dependent (row 1d); (b) at 1.3e11 dc is +6.9 s with C2 (after `total`, part of the wall with the file; not investigated); (c) `DIST_TWREC=1` with C2 is a separate adoption decision for the user; (d) at size 1 B0's `mn_model` hides the file write under half the division (a model rule, not measured here). No job is running; `RNS_AUTO_PIECE_COST` and `ECALC_VSLOT_BUDGET` no longer have README rows on main (switches removed or built in); the grid they selected is the current code path.
