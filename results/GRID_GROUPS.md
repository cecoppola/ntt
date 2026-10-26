# GRID_GROUPS — the grid of piece products over shares and node groups

Scope: the grid of piece products over shares and node groups: layout at any group size, MN_GROUPS schedules, v-slot release, the 576-node layout checks. Owner of the items below; each section condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`. Labels: **m** measured, **mod** modelled, **a** assumed. Times Eastern.

Sections moved to `archive:results_merged_2026-10-24/GRID_GROUPS.md` (2026-10-24 cleanup): A-grid; M3; L; G; G12; C09; c09_groups; C215.

## Topic summary

What this topic is: how a product of two numbers sharded over a group of g nodes is computed. The pieces are `rns_mul_dist_mn` (one plane per node pool, cap 2^(min(31, POOL_LOG) + log2 g) points), the grid of piece products above the cap (`split_grid_cap`, views, `mdb_add_shifted`), the block-cyclic map that lets every node of a group of any size transform (L), the tree on the `MN_GROUPS` schedule with k-way combines (G12), and the v-exchange slot memory of the general map (B7ACCT, S36, C06, S45).

Current state (main; cross-checked against `ecalc/README.md`):
- The grid over shares, the views, the general map for any g and the tree on the `mn_groups_parse` schedule are the code. Default schedule (README `MN_GROUPS`, G12 / Q's decision): powers of two that divide the size, then the odd part's prime factors ascending (576 -> 2, 4, ..., 64, 192, 576). The L schedule (powers of two, then the size) is `MN_GROUPS=2,4,...,512,576`.
- `COMM_LAYER_VSLOT_SHARE` (S36) and `COMM_LAYER_VSLOT_RELEASE` (C06) are built-in defaults since 2026-10-12 (README). One pair of v-slots per APU, shared by all layered communicators, and released per product under X2. The §B7ACCT / §S36 sums of v-slots per level are superseded: the resident term is the max, not the sum.
- `RNS_DIST_CACHE_MN` (transform cache over shares, G section) is built in with 2 slots. At size 1 the cache stays off (`RNS_DIST_CACHE=0`). The 3·2^30 plane (C5 in A-grid) was measured only; the radix-3 lengths now live under `RNS_R3` (README, default 1 when the prime set has radix-3 roots). The old `DIST_R3` switch is not in the README.
- `DIST_CHUNKS` is 8 (README); the §B7ACCT, §S36 and §S52 numbers use K = 8 (B7ACCT's table assumed K = 4 for the slot arithmetic of option rows).
- `DIST_COALESCE` (message bundling, §S52) was not built (user decision 2026-10-11) unless the target's per-message cost is shown >= 8.5 us.

Live conclusions:
- Memory is the limit at 576 nodes, not time: the real v-slot term (B7ACCT, 6.707 GB per APU before sharing) broke the device bar; SHARE (-5.79 GB per node, mod, S36/S45) and RELEASE (the transport pool 4352 -> 2304 MiB per APU (then comm_ofi's), C06) bring it back. Largest safe size at 576 is 3.82e13 (§S52, mod); above it the plan adds pieces (+20 s at 3.83e13).
- The cut-group pool-offset asymmetry (G5) does not bite (§C06); uneven last groups work (digits identical, §B06) but give no speed gain (§B06, §O1: `[6]` vs `[2,6]` at 6 nodes inconclusive, raw +2.4 s, CI [-3.9, +8.7]).
- The §C09 tables are modelled; the model-best uneven tops are unmeasured extrapolations.
- A bug found by forcing tree grids (`mdb_add_shifted` windows of the wrong nodes for g0 > 0, §G12 2b) was fixed before any run could reach it.
- §C215: the piece-cost grid (`RNS_AUTO_PIECE_COST`, C2) gains -31 s (-13 %) of `total` at 1e11 on one node; neutral at the 576 target (0.0 s, mod).

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

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

## S45 — CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e)

Verdict: **SHARE=1 is effective and safe on the target launch line** (COMM_OFI=1, no X2). Plan and layout change only the v-slot term; a many-rank host test (up to 600 ranks, non-power-of-two groups, a cut last group) found no wrong data, no leak and symmetric pool offsets. It is a TCP / host emulation, not the real SHMEM / OFI transports.

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

## S52 — more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9)

CPU only (model scripts and the aac7 login node's plan/layout printers); no engine code changed. Labels m / mod / a. Target launch block of `archive:results_raw_2026-10-19/S45/launch_block_main13c244e.sh`.

#### Verdict
1. **Largest safe size: 3.82e13 digits** (+3.0 % over 3.71e13; 3.80e13 has the identical layout and margin). Device peak 357.9 GB per node (mod, `estimate.py`, SHARE=1), 360.0 GB for rank 0 (+2.1 GB allowance) against the 373.44 GB edge: **margin 13.4 GB (3.6 %)**, above M1's 10 GB rule; node 402 of 480 GB (mod); 185.9 s without the write vs 180.3 s at 3.71e13 (mod).
2. **Do not go above 3.82e13**: at 3.83e13 the C plan adds pieces (largest tree group 42 -> 54 pieces, 109 -> 121 in all) and the model's wall jumps +20 s (185.9 -> 206.1 s, +11 %) for +0.3 % digits; from 3.88e13 the device peak steps to 366.5 GB (margin 4.8 GB with rank 0); 4.08e13 is over the edge (375.1 GB, -3.8 GB).
3. **Messages:** 2.65 M per APU at 576 (mod): reciprocal 1.098 M, division 0.927 M, tree 0.624 M; 93-95 % are transform-chunk messages (K = 8 chunks x 575 peers). Memory-neutral coalescing saves 0.48-0.66 M messages (18-25 %) = **1.0-1.4 s at 2 us, 2.4-4.5 s at 8.5 us, 3.7-8.4 s at 20 us** (mod). Larger savings need memory or are lost.

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

## B06 — uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001

Question: does a run with an UNEVEN last group (the "exclude k bad nodes" case of archive:results_merged_2026-10-24/NODE_SLOWMODE.md §B04, branch b04) work and give correct digits?

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
