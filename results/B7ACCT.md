# B7ACCT — counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06)

Times are Eastern (aac7 logs are Pacific, so add 3 h). Numbers are labelled **(m)** measured, **(mod)** modelled or **(a)** assumed. A
C binary's own `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` print, made on the aac7 login node with no HIP calls, is a *measured* layout figure:
it is the code's sizing, not a memory reading.
Login-node work only (aac7 `uan1`, `~/b7acct`: `ntt` = this branch, `base` = main 9e26898; outputs in `~/b7acct/out_*`,
`sweep/`, `below/`). No compute-node work, no jobs.

## RESUME (2026-10-06 18:10 EDT): complete

- Branch `b7-vslot`: c7664df (the v-slot accounting), aa86cac (task 2 + README), then this file. It is pushed to origin and not
  merged.
- Nothing is armed and nothing is running. To resume, the next step is the hardware check in §8, item 1. It needs compute nodes.

## 1. Which exchanges take v-slots, and how long they live

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

## 2. The code

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

## 3. Tests (aac7 login node; `make SHMEM_CRAY=1 -s -j16` builds clean, including the tests, rc 0)

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

## 4. Predicted layouts at the standard config (8.1e10 per node; measured C layout figures)

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

## 5. The 4.08e13 / 576 target with the real v-slot term (TGT17's launch line, `DM_MN_LEAN=1`)

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

## 6. Task 2: stale sizes

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

## 7. Labels

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

## 8. Open items

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
