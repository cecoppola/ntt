# MEMORY_IO — Memory, output I/O, health, stress

Scope: the memory model and guards, the output writer, node health, the map benchmark, the pool race, the stress switches. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## MEMORY — device and host memory

Scope: device and host memory: accounting, the arena and its sizing, the reserved tail, init floor, early free, the memory ceiling. | Owner of the items below (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Each item section is a condensed summary of the former `results/<ID>.md`; full originals are in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`. Cite items as `results/MEMORY_IO.md §<ID>`.

### Topic summary

What this is: how ecalc maps and sizes device (HBM, 4 APUs per MI300A node) and host memory, and what the per-node ceiling is. Numbers are measured on aac6 (one node, 502 GB total) unless labelled modeled; the 576-node figures are modeled.

Current state (cross-checked with `ecalc/README.md`; where an item below says "off by default" the README row governs):
- **Layout.** One arena per APU holds both bs parities plus the dm extra, laid out by `region_need` / `dm_layout` / `tree_need_dev` (§A-mem, §M11); the block pool keeps a reserved tail for the reciprocal's largest block, so there is **zero in-phase hipMalloc** at every size run (§M11 v4: 10¹¹ on one node, device 431.2 GB, node peak 445 of 502 GB). `ECALC_DM_POOL` was deleted (§I item 5). The arena is a VMM range in whole 2 GiB chunks (`DB_POOL_VMM`, default on); `DM_TIGHT` defaults on with it (§L114, §T114 context); `BS_ARENA_ROOM` default 0.14 (0.16 until Q99, 2026-10-25) and `BS_SEED_FILL` default 128 (§AS15; user decisions of 2026-09-27/28); `BS_SEED_PULL` default 1 since 2026-10-12 (§K8b, README) although the K8b ABBA never completed in this file.
- **Host.** X never lives on the host and the seeds stream straight into the regions: host HWM at 4 × 10¹⁰ fell 48.8 → 11.7 GB (§H); at 10¹¹ about 14 GB.
- **Init floor.** Init time is 0.06–0.075 s/GB × bytes mapped, a kernel page-clearing cost on one lock that no allocation form, thread count or reordering beats (§I, §K8, §K8b, §P3, §MAP15). Seeds overlapped with the mapping (§I item 3); 3·2ᵏ planes on below 5 × 10¹⁰ digits at one node (§I item 4; the 3·2ᵏ caps were later removed by SIMP, see `results/CODE_TOOLS.md §SIMP` and the README row of `ECALC_PLANE_CAP`).
- **Plane cap `run` (2026-10-25, new switch; the default since 2026-10-25, user decision: unset = `run`, `ECALC_PLANE_CAP=2^31` forces the old cap, `POOL_LOG` or `DIST_LOGN_TEST` given keeps the knobs as given, `ECALC_PE_PER_GPU=1` with `XSHMEM_PES_PER_GPU` > 1 keeps `fit`; the verbose line reads `run (default):` when unset; the models `mem_model.py` / `mn_model.py` still assume 2^31 unless cap is passed).** Q122 (aac7, m): `ECALC_PLANE_CAP=2^30` cuts the fixed start-up of the two plane pools (25.8 GB per APU at 2^31) by 2.64 s [-2.98, -2.30] at 8e9 on one node and 2.05 s at 1e9, compute unchanged, digits identical (hash 3e78c1b2a0276963 at 8e9); about -2.8 s at 2 nodes with 8e9 per node. `ECALC_PLANE_CAP=run` takes 2^30 when the run's largest single product, the division's `2k + 8` limbs (k = limbs of 10^d + 4; `binsplit_div_product_limbs`), fits one mn plane of the 2^30 cap (`mn_logn_cap(g)` at pool_log 30: 2^30 at one node, 2^31 at two), else 2^31, and prints one `ECALC_PLANE_CAP=run:` line. That is 9.6e9 digits per node (assumed bound: above it the products are gridded and no 2^30 run was timed there; the model code found no abort at 2^30 below it, since the batch tier's largest product stays under the pool's 3·2^28 limbs to about 1.3e10 per node, modeled). `ECALC_PE_PER_GPU` is not modeled: `run` gives 2^31. Gate checks: unset unchanged; 8e9 one node picks 2^30 with the same hash; 1.2e10 one node picks 2^31; 2 nodes at 8e9 per node pick 2^30 (not yet run: coordinator's gate).
- **Ceilings (one node).** Three primes (default `ECALC_NP=3`): real edge 1.44 / 1.30 / 1.14 × 10¹¹ at caps 2³⁰ / 2³¹ / 3·2³⁰ (device + host HWM ≲ 524 GB runs, ≈ 529 GB is OOM-killed; §P13b). `mem_model.py` (exact to the byte against the C layout, §M13, §AS15, §DL15) is the tool; its 502/480 GB budget is conservative.
- **576-node ceiling (modeled).** 4.0–5.3 × 10¹³ digits depending on switches (§M13 headline), 5.396 × 10¹³ with the room kept and 5.532 × 10¹³ at most (§DL15); the room-0 ceilings of earlier items are one chunk too high. §M11's older 1.1–1.25 × 10¹³ (spill buffers growing with g) is superseded by §M13/§DL15.
- **Bug fixes in sizing:** §R16 `BS_POOL_RULE` (default 1) fixed the 10¹¹ on 4 nodes abort; §PS15 pool 1 = 4 q at four primes (`RNS_POOL1_4Q`, since removed from the code) fixed the rc 6 at 9.17 × 10¹⁰ with `ECALC_NP=4`.
- **Dropped / stopped:** `DB_POOL_VMM_STREAM` (§MAP15: no gain, two node reboots, ACK-guarded; do not adopt), `RNS_HOOK_CONCUR` and `ECALC_VMM_FILL` (§K8: slower), `DM_TAIL_DEAD=2` / E5 spill (§L114: ≈ 0 over `DM_TIGHT`; `DM_TAIL_DEAD` removed 2026-10-20), host-side allocation forms (§I).
- **Superseded within this file:** §M B4/A3 (C3 `ECALC_DM_POOL`) by §M11 tail, then §I item 5; §A-mem and §M open issues by §M B5/B6 and §M11; §M11's tail v1–v3 by v4; §M11 g > 1 estimate by §M13/§DL15; §L114 first and second forms by the third.
- Live open items: spill buffers and top-level plane growth at g = 576 (§M11, §M13), host term at size > 1 on the target (§M13 issue 1), the mn division peak at 576 counted not measured (§DL15), intermittent leaf fault seen once (§T114), the §L114 batch 6 gate and the §K8b re-armed ABBA not recorded here.

### §AS15 — Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps)

**Recommendation (user's decision):** adopt `BS_ARENA_ROOM=0.16` with `BS_SEED_FILL=128`; the whole-chunk rounding alone is free and removes the reciprocal's remaps. Open: f chosen on 17 traced sizes, placement greedy so an untested size can still remap (one traced run answers any size); `mem_model.py` node peak with the room is pessimistic (+16.3 GB at 10¹¹, one-node ceiling 1.24 vs 1.34 × 10¹¹ modeled) while the run cannot be refused (budget rule); effect at the 576 target unmeasured. 576-node estimate unchanged: **4.25 × 10¹³ digits in 256.0 s (4.27 min) without the write, 285.7 s with the packed write** (modeled, RESULTS §87 / DOC15), node memory 416.0 GB of 480 on defaults, 429.0 with the room (modeled).

### §MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). **STOPPED after a second node failure; nothing here is adopted except the print-only timeline.**
**Conclusions.** (a) The stream removes the seeds' idle wait exactly as modeled (seeds end 28.3 / 29.2 s instead of 34.3–35.5, m) but level 1 does not move: **the mapping itself is the floor**, ≈ 0.2 s per 2 GiB chunk whoever calls it (one worker 0.216; old pregrow threads 0.16–0.19; old background mappers 0.20–0.23 under seed load; 0.131 only with the CPU idle; `hipMemCreate` ≈ 70 %, `hipMemSetAccess` ≈ 30 %). The runtime serialises the calls, four workers map slower and starve launches. Letting bs levels wait only for their own chunks would overlap mapping with compute, the regime preceding both failures: not built. (b) Seed threads at 10¹¹ on the old path (m): 192 threads (default) level 1 at 35.07 / 35.40 s; 96: 36.46 (spans 16.6 → 19.9 s); 48: 47.63 (spans 35.7 s): **keep all threads**. (c) Timeline facts: `rns_init` staging + contexts 1.5–2.5 s, plane pools 4.2–5.6 s (≈ 0.045 s/GB); seed pinned buffers (2 × 8 GiB) cost 1.4–1.6 s at start. **The remaining lever is the mapping rate outside the VMM path:** hipMalloc maps at 0.036–0.05 s/GB against hipMemCreate's 0.077–0.1 (R114 §6b); R114 measured the VMM arena at +8 s of init at 10¹¹ vs the hipMalloc arena. With `BS_ARENA_ROOM=0.16` the division at 10¹¹ remaps 0 times (§AS15), so `DB_POOL_VMM=0` where memory allows may be worth one paired series (untested; no code needed).
Tests: timeline, stream off, 10¹¹ × 7 (job 21659): 7 × VERIFY OK, totals 187.1–199.5 s; r0c (room, stream off) 179.07 s VERIFY OK; e9 stream W = 4 VERIFY OK 12.41 s. **Gates owed before anything merges** (not run after the node failures): `t_dbig 0`, `mnaccept --stress --only unit,e9,mn,stress` at b94072c, stream off. Open: two nodes (s24-26, s24-30) needed the admin; if ever retried, a unit test that maps chunks on one APU while kernels run on all four first, on a node that can be spared. **Merge only `ECALC_INIT_TL` (5162a8f, print only); do not adopt `DB_POOL_VMM_STREAM`.** 576-node estimate unchanged: 4.25 × 10¹³ digits in ≈ 256 s without the write, ≈ 286 s with it (modeled, RESULTS §87 / DOC15); node memory 416 of 480 GB.

### §DL15 — the arena sized to DKM's division, and the room check on the model's node (Phase 15 Batch 3, agent DL)

**Built (`binsplit.c`, `mem_model.py`).** (1) `dm_layout` follows `NEWTON_DKM` (DKM layout at size > 1 always, at size 1 only in the device flow n_Q ≥ 9/8 · 2^`BS_MDEV_LOGL`): reciprocal's jl at h, DKM's division set, the hole is DKM's largest block (size 1: step 1's `X_hi·Q`, n_Q + ⌈k/2⌉ + 8 limbs, which `mul_grid` reserves whole; size > 1: t, k + 8 limbs; the model had used k + 8 at size 1 too, smaller than that block). With `NEWTON_DKM=0` the layout lines are byte-identical to ab8daa2's. (2) `as_room_fits` counts the model's node (planes by the `auto` rule, arena with the room, 6.0 GB of VMM bs growth, host HWM); `mem_model.as_room_fits` / `room_host` is the twin; `BS_LAYOUT_ONLY` prints a `room:` line, the layout line gains ` dkm 1`. (3) `--check-c` exact on 16 layout files (0 terms not exact, room terms included). (4) Outside: `mn_model.py` passes `dkm=DKM`; `--check-c`'s `MN_P24` default 2; `DEFAULTS15D` the new `mem_per_node` base; README rows `NEWTON_DKM`, `BS_ARENA_ROOM`, `BS_LAYOUT_ONLY`.
**Tests (aac6, m).** Batch A (job 21801, s24-16): 10¹¹ ×4 alternating new / base (ab8daa2), all `NEWTON_DKM=1`: identical to `e_1e11.out`; arena **283.5 vs 289.9 GB (−6.4 GB)**, device at init 387.2 vs 393.6; 0 remaps, 0 growths, 0 hipMalloc; division window peak 54.07 GB per APU in both (76.3 % of the new pool); `dm` 72.49 / 74.24 s new vs 72.93 / 74.63 base: **neutral** (new_2's bs outlier 105.1 s vs 78.8–82.9 s is noise). Stage D (timing series) not run. Batch B (job 21805, s24-30): 1.3 × 10¹¹ new: room kept, arena 335.0 GB, 0 remaps, dm 114.25 s, VERIFY OK, budget peak 464.1 GB; base (old check dropped the room): 2 remaps (0.92 s), dm 117.70 s; 4 × 10¹⁰ identical; 10¹⁰ at sizes 2/3/4 identical (window peak 5.52 / 2.96 / 2.76 GB per APU, 43–49 % of the pool). Batch C (job 21808): `mnaccept --stress --only unit,e9,mn,corr,stress` **20 passed 0 failed**.
Open issues: (1) the target's mn division peak is counted, not measured (DKM mn path peaks at 5.5 u + product scratch; reservation 55.54 GB per APU vs 54.11 counted at 5.1 × 10¹³: margin 1.4 GB, modeled; today's path 6.0 u = 59.0 GB vs 56.83 reserved, already over, M6's finding; sizes 2–4 on one node are 43–49 % of pool so do not test it; a 2-node `ECALC_VERBOSE=2` run at a dm-bound size would); (2) `mem_model`'s node should round VMM arenas to chunks at `BS_ARENA_ROOM=0` (not done); (3) size-1 room host uses log10 N! (< 0.02 GB difference).

### §K8 — init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12)

Task K8 (OPPORTUNITIES B.3), branch `k8` off main a448d73, not merged. aac6, one node, one run per arm, `ECALC_INIT_TL=1 ECALC_VERBOSE=2`, all VERIFY OK. **Result: diagnosis only, no switch proposed; code unchanged** (the two patches, `RNS_HOOK_CONCUR` in `rns_mul.c` and `ECALC_VMM_FILL` in `dbig.c`, are in `archive:results_merged_2026-10-19/`'s original of this item).
Differences 2.0–11.5 s vs default spread 1.7 s: verdicts hold without ABBA. What could still work (not built): (1) seed DMAs that need no HIP runtime call (worth ≈ 4–5 s at 6.441e10, modeled; led to §K8b); (2) fewer bytes mapped before level 1, lazy parity-1 mapping (interacts with `vmm_bg_map`, do after G1). K8 as a reordering lever is closed.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/MEMORY.md`.)

### §P3 — shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED)

Nothing built or run. Starting point (m, archive:results/A09.md, 6.441e10 per node): GPUs idle until level 1 at 24.4 / 28.3 / 29.4 / 35.1 s on 1 / 2 / 4 / 6 nodes (32 / 20 / 16 / 12 % of the run); at 1 node the seed thread binds (ends 24.0 s, last background chunk 23.0 s), at 2 and 4 nodes the mapping binds by 1.4–2.5 s; any single fix is worth ≤ ≈ 1.5 s, both ≈ 3 s (modeled). By code reading, overlapping mapping with CPU binary splitting, lazy mapping per pool, bigger chunks (`DB_POOL_VMM_CHUNK_GB` exists; per-GB cost flat), planes overlapped with arenas (unsafe: late planes get small pages, N414) and GPU work on already-mapped pools are already done, ~0, unsafe or not small. The only candidate (publish parity-0 chunks one by one so seed DMAs start early, A09 6.3; switch `ECALC_VMM_PUBLISH`, not started) saves ≈ 1.0–1.5 s of a 76 s run at 1 node (modeled), ~0 at 2/4/6 nodes, adds concurrent mapping and launch traffic where the hipMemMap crash and the lock-order deadlock lived, and at the target the mapping is expected 7× faster (assumed, S22 3.1) so the seed thread does not wait. **Stopped; nothing pending.**

### §L114 — L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`)

**Verdict.** Merged as an opt-in (`BS_ARENA_TRIM_GB`, default 0): the memory saving is large at 2 n and time is neutral to slightly better; the measured row is in `RESULTS.md`. No adoption decision; no 4 n / 6 n data.

### XSHMEM_ROUND_MB 128 / 1024 / 0 at the target sizes: staging fit (B5, 2026-10-27; modeled, planner not run)

Input for the target A/B (`XSHMEM_ROUND_MB` 128 / 1024 / 0). Python only: `mn_model.memory` (-> `mem_model.mem_per_node`) with the target's launch design, `MN_T_CHUNK_MB=2048`, `MN_NONPOW2_CAP=0` at 576 nodes; the C planner (`tools/plan_values.py --fit`) needs a current build and was not available locally (the aac7 binary predates the code), so the C planner's own lines on the target are the check. Calibration: at 3.82e13 / 576 nodes (round 128, cap 0) the model's device peak is 349.9 GB against the C planner's 358.96 GB (9 GB low, 2.5 %) and its node peak 394.0 GB against the planner's 395.04 GB (planner figures: remote/FACTS.md); the round size does not enter the device figure.

### §B4 — APU balance of the top bs levels: cause, design, zero-code test (2026-10-27; `BS_TOP_SPREAD` dropped by the user 2026-10-27, option 0 adopted)

**Outcome (2026-10-27).** Option 0 measured (aac7, 1 node, 1e11 digits, m): `BS_MDEV_LOGL=29` with `ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup XSHMEM_COLL=tree` against the defaults: APU 0 driver peak 109.9 -> 90.5 GB, all four 90.5 GB; planner device 386.72 -> 367.40 GB; ABBA -7.2 s (-5 %, not resolved); digits identical. The user dropped option 1 (`BS_TOP_SPREAD`, never built) and adopted option 0 for target runs from 1e13 digits (`remote/ROUTE.md` S6). The design below is kept for reference.

**Problem (measured aac7, 1 node).** Device 0 peaks at 109.9 GB at 1e11 against 86-91 GB on devices 1-3 (9e10: 105.6 against 86.2). `ECALC_FIT_PER_APU=1` matches it (planner 111.18 / 106.88 GB, TASKS), so the excess is in the layout the planner already models.

**Cause (modeled, `mem_model.region_need` ported from `binsplit.c region_need`; read of the code, not a new measurement).** Each region's arena is `max(2 cap_r, want)` (`layout_arena`: `want` = the dm need, the same on every APU), and `cap_r` comes from the largest batch level that region holds. Levels with at most `BS_BALANCE_N` (16) nodes place node `i` in region `i % 4` (`place_node`), so the remainder of a level whose node count is not a multiple of 4 lands on regions 0, 1, ... first. At 1e11 the last batch level (level 21, before the mdev tier takes over at level 22) has 10 nodes of 10.13 GB: regions 0 and 1 hold 3 nodes (30.4 and 27.5 GB of limbs after the carry's smaller last node), regions 2 and 3 hold 2 (25.3 and 24.7); the level before it (20 nodes, subtree rule) holds 5 per region (25.3 GB). Modeled bs arena per APU (GB): 1e11 68.4 / 62.0 / 57.0 / 55.5; 9e10 63.5 / 51.3 / 57.0 / 50.2; 8e9 17.7 / 7.5 / 5.1 / 4.4. A rotation of the round-robin start per level does not help (modeled: identical maxima): some region must hold `ceil(n / 4)` whole nodes of the biggest level.

### §S4 — the "B gathered per prime" fallback at 2 nodes, 6e10 digits (2026-11-02; negative result, no code kept)

**Open.** Whether the fallback costs time (the per-prime B gather versus the raw plane) is unmeasured; the cheapest test is a larger `ECALC_DEVICE_GB` pool share for the dist_db phase, not a change of the cache. The 576-node in-phase remaps (54 x 3.9 GB, 10-29 data) were not reproduced at 2 nodes up to 6e10 (0 remaps, 0 in-phase hipMalloc, measured) and remain unexplained.

### §M4 — planner against measured memory, 1 node to the ceiling and 2 nodes (aac7 campaign, 2026-11-01 22:33 to 2026-11-02 13:47 ET; main d7c7f28; measured unless marked modeled)

| nodes | digits | VERIFY | ecalc total s |
|---|---|---|---|
| 1 | 1e10 / 2e10 / 3.5e10 / 5e10 / 6.6e10 | OK x5 | 21.1 / 28.5 / 42.6 / 58.2 / 75.7 |
| 1 | 8e10 / 1e11 | OK x2 | 127.6 / 166.3 |
| 1 | 1.15e11 | none: stopped by the memory guard (rc 9) at 45.0 s, in the start-up phase | |
| 2 | 2e10 / 6.441e10 / 1e11 / 1.5e11 | OK x4 | 35.3 / 62.6 / 92.4 / 155.2 |

**Retained evidence.** The harness keeps the full log only for a failed run, and the memory files (`mem [phase]` lines, `live:` lines, planner lines) are overwritten by the next size of the same item. So the comparison below exists for three points only: 1 node 6.6e10 and 2 nodes 1.5e11 (largest size of each passing item), and 1 node 1.15e11 (the guard stop). The sampler's `device_hwm_apu_gb` is in the log of the stopped run only; for the 11 passing sizes the high-water is not retained. Rerunning with a per-size copy of the memory files would fill the table (a one-line change in the campaign driver, not made).

### §ATRIM — BS_ARENA_TRIM_GB (trim the dm arena need; merged 2026-10-23)

Switch `BS_ARENA_TRIM_GB`, default 0 (off): subtracted at the end of `dm_layout`, clamped at `max(v2, v3)`; twin `mem_model.py arena_trim`. aac7, `=8` against off: 2 nodes at 1.2882e11 digits, arena per node 266.3 -> 231.9 GB (m), time 115.2 -> 111.4 s (m), 0 remaps; 3 nodes at 1.9323e11 digits 149.4 vs 149.6 s (m), 0 remaps.

## OUTPUT_IO — output

Scope: output: streamed chunks, 60-bit packed digits, write scheduling on a network file system, the correction patch, spill and I/O. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; unabridged text: `archive:results_merged_2026-10-19/`. Times Eastern; numbers are measured unless marked modeled (mod) or assumed (a).

### Topic summary

- **What it is.** How a run writes and verifies its digits: the digit string never exists whole; each node formats its share in chunks (`MN_OUT_CHUNK_MB`, 256 MB), a writer thread `pwrite`s `<outfile>.part<k>` (k = size-1-rank, so `cat <outfile>.part*` is the exact file), and T1 (digit residues, term recurrence over each node's range) and T2 (windows) run rank-locally in the output stage (§A-out). Switch rows and defaults: `ecalc/README.md` (authoritative); format and checks: `ALGORITHM.md` §9.
- **Current defaults (main).** **`ECALC_OUT_PACKED=2`**, the 60-bit packed part file, is the default since 2026-10-12 (user decision; §C05, §O4): 7.5 B per 18 digits (-6.25 % vs packed v1 = 8 B, 0.4167 B/digit vs 0.4444 vs 1.0 ASCII), digits identical, `tools/unpack_digits` converts (any consecutive subset of parts, §IO215), RECHECK reads v1 and v2. `MN_OUT_DKM_HI` is on by default; O_DIRECT is the writer default (best on both aac6 paths, §IO15 W1); `ECALC_CKPT_TOP` (the top set for RECHECK) is off by default and `=2` is the budgeted form; `ECALC_CORR_PATCH=2` patches corrections in the file (packed form fixed in §KP15, no new switch). `MN_OUT_STRIPE` has no default (unmeasured on Lustre).
- **Measured conclusions.** On a bandwidth-limited file system the bytes are the lever: packed output cuts the with-write wall 110.8 -> 65.3 s at 10^10 on aac6's NFS (-41 %), -25.7 s without the write and -57 s with it at 10^11 on local NVMe; aac7 4 nodes at 10^10: write time 40.6 s (ASCII) -> 10.5 s (packed v1) / 9.0 s (v2) per run (single runs; the file system varied 1.5-2x between runs). O_DIRECT halves the elapsed time with the top set on (154 -> 82 s at 4 x 10^10, local disk) and leaves nothing in the page cache. The write is file-system bound (0.06-0.12 GB/s per node on aac7 NFS; 0.116 GB/s on aac6's 1 GbE), not format bound.
- **`total` and the write.** `total` is printed before the writer is joined; on NFS the write rarely hides (`dc` shows "waiting for the writer"); `MN_OUT_EARLY=1` starts the part file in the division (at size > 1) so `dc` -> 0, with a net wall gain only where storage does not share the fabric link.
- **Fixed defects.** §KP15: the K patch aimed ASCII byte offsets at the packed file (VERIFY FAILED with the uncorrected digits on any correction after the writer started); fixed by a packed-form patch plus `t2_defer` in the packed writer; `mnaccept` has a `corr` step and `t_patch` in `unit`.
- **Unmeasured / open.** Everything about Lustre (O_DIRECT rate, aggregate at 64 / 576 nodes, stripes, waves; TARGET §6 item 5); more than 2-4 nodes for the packed multi-node writer beyond aac7's 4-node 10^10 check; corrections spanning several limbs end to end (only unit tests); a mode with forced correction under `ECALC_OUT_MODE`.
- **Superseded.** §IO15's packed v1 default (`=1`) is superseded by v2; §O4's "pending" state is closed by its section 4 (and 4.1).

### §O4 — packed output adoption path, write rate and round trip

Overnight 2026-10-11/12, branch `o4` (from main 0298fe2). Question: what is missing to adopt `ECALC_OUT_PACKED=2`? No ecalc code change needed. Gaps found: no real multi-node `-o` text + `cmp` run, no RECHECK against the checkpoint set on 4 nodes, no write rate side by side per format. Tools added (behind existing switches): `tools/o4_rate.sh` (inside a hold: formats 0 / 1 / 2 in rotated order; VERIFY count, bytes, writer-thread time, rate per node, digcmp, `unpack_digits -o` + `cat` + `cmp`, `ECALC_RECHECK=1`), `tools/o4_aac6.sh`, `tools/o4_aac7_item.sh` (aac7 queue item 80, 4 nodes).

**Collected results (aac7 4-node item 80 and aac6 CPX; measured; format 0 = ASCII, 1 = packed, 2 = 60-bit packed; rate = GB per node / writer-thread time; the file system was shared).**

| where | digits | fmt | GB (all nodes) | write (s) | GB/s per node | total (s) | n |
|---|---|---|---|---|---|---|---|
| aac7 4 nodes | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.40 | 3.23 / 1.14 / 1.41 | 0.083 / 0.103 / 0.092 | 26.0 / 21.3 / 23.0 | 2 |
| aac7 4 nodes (ckpt step) | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.40 | 2.52 / 1.03 / 0.90 | 0.101 / 0.107 / 0.111 | 23.3 / 19.8 / 19.2 | 1 |
| aac7 4 nodes | 1e10 | 0 / 1 / 2 | 10.00 / 4.44 / 4.16 | 40.62 / 10.50 / 8.98 | 0.062 / 0.107 / 0.116 | 73.4 / 36.8 / 39.4 | 1 |
| aac6 CPX 1 node | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.42 | 8.88 / 3.88 / 3.69 | 0.113 / 0.114 / 0.114 | 8.5 / 8.2 / 8.8 | 2 |

### §IO15 — Phase 15 agent IO: output I/O on a network file system, packed digits, write scheduling, the memory guard

PLAN §36.4 row 1f, branch `p15-IO` from int15 0b80ca3. Every switch off by default (digits unchanged), measured on aac6.
**0. Summary of verdicts.**
| item | switch | verdict | evidence |
|---|---|---|---|
| W1 write mode on a network FS | `ECALC_OUT_MODE`, `MN_OUT_THREADS` | **keep O_DIRECT**: fastest on aac6's NFS from a compute node (0.116 GB/s = the 1 GbE link) and the login node (0.64-0.96 vs 0.36-0.48 GB/s buffered + fsync, 10 GbE); buffered forms hold up to 10.7 GB of page cache | §3 |
| W2 packed part file | `ECALC_OUT_PACKED=1` (later 2, §C05) + `tools/unpack_digits` | 10^11 identical after conversion (sha1), RECHECK OK; 2.25x fewer bytes; -25.7 s without / -57 s with the write at 10^11 local; NFS with-write wall 110.8 -> 65.3 s at 10^10 | §2.2-2.3, §3.2 |
| W5d part file during the division | `MN_OUT_EARLY=1` | identical at 10^9 (2, 4 procs, 2 real nodes), 3 x 10^9 (2 nodes), 10^10 (2 procs); `dc` -> 0 everywhere; net gain depends on whether storage shares the fabric link (aac6 1 GbE: -2.9 s of 557; 2 procs at 10^10: -7 s of 116); recommended on for the target, confirm at step 3 | §2.1, 2.5, 2.6 |
| W5b waves | `MN_OUT_WAVES=<n>` | works at size 2-4; at 2 nodes it only serializes (+14.4 s); a target tool if the aggregate saturates | function + cost measured; benefit unmeasured |
| W5a stripe | `MN_OUT_STRIPE` | command path tested with a fake `lfs`; unmeasured on Lustre | |
| W7 O_DIRECT by FS type | `ECALC_ODIRECT=auto` | local and NFS detected, both O_DIRECT; Lustre -> O_DIRECT is assumed | |
| W6 top set at the target | - | run without it (default) or `=2`; RECHECK keeps its residue form | analysis |
| N3 memory guard | `ECALC_MEM_GUARD_GB` | works: rc 9, one line naming rank, host, phase; the silent sampler runs every 1 s (fixed) | §2.1 |
**The 576-node estimate (mod, `mn_model.py --D 7.38e10 --g 576`):** 4.25 x 10^13 digits; without the write about 238 s; with the ASCII part file at an assumed 2 GB/s 260 s, at 0.6-0.8 GB/s 316-347 s; **packed at 0.6-0.8 GB/s 264-278 s**; each node assumed to get its single-stream rate at once (aggregate unmeasured, TARGET §6 item 5(b)). Without `MN_OUT_EARLY` the whole write is exposed: +123 s ASCII / +55 s packed at 0.6 GB/s.
**1. Design.**
**6. Open issues.** Everything about Lustre is unmeasured (TARGET §6 item 5 (a)-(d); `tools/wbench` measures (a) and (b) without a GPU). Corrections rewrite the file (size 1 always; size > 1 with `MN_OUT_EARLY`; doubled the write at 10^11 on NFS; replaced by §KP15's patch). `MN_OUT_EARLY` holds about 0.9 GB of host buffers (about 0.2 % of 480 GB; `MN_OUT_CHUNK_MB=64` quarters it). (`MN_OUT_WAVES` was removed 2026-10-20.) The packed file has no per-chunk checksum beyond the part residues (a flipped bit is found, not located). Minimal edits outside the IO files: `verify.c`, `newton_db.c` / `newton.h`, `ecalc.c`.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/OUTPUT_IO.md`.)

### §KP15 — the correction patch on packed output (a defect on main, B2 44c1642)

Agent KP, branch `p15-KP` from main 44c1642; aac6.

**1. The defect (reproduced).** `ECALC_CORR_PATCH=2` (K) patched the file tail at **ASCII byte offsets**, while `ECALC_OUT_PACKED=1` writes a 4096-byte header plus 8-byte limbs. When a correction comes after the writer started (size 1: always; size > 1: with `MN_OUT_EARLY=1`) the patch aimed past the file's end, `patch_bytes` refused it, and the run ended **VERIFY FAILED** with the file holding the uncorrected digits. A second part: the packed writer ignored `t2_defer` (the ASCII branch passes `d_out + 1 - t2_defer`), so deferred-zone windows were checked on uncorrected digits and again by the patch (`t_patch`: "T2 window at 9 (file) BAD"). Missed because 10^11 on defaults needed no correction reaching the file, forced-correction runs (`ECALC_TEST_CORR=7`) changed only digits past d_out, and `t_patch` was not in `mnaccept`'s unit step. **Reproduction with the base binary (job 21771):** 511461828 size 1 `TEST_CORR=14`: rc 1, VERIFY FAILED, digits DIFFERS ("file holds 227320472 bytes, the patch ends at 511461830"); same with `ECALC_OUT_PACKED=0`: OK (11 bytes patched); 820719000 `CORR_PATCH=1 TEST_CORR=-44` FAILED; 108388422 at size 2 and 4 (`TEST_CORR=36`) all nodes FAILED; `t_patch` packed 2258 of 5165 checks failed (ASCII OK). Matches DKM's controls (job 21768).

**2. The fix: a packed-form patch (built; not the guard).** A correction by dx changes exactly the global limbs [0, m) its carry reaches; a part's limbs are one contiguous byte range. In `ecalc/mn_out.c`: **`packed_patch`** (new) validates the part's header (magic, version, `complete = 1`, d, d_out, k0, k1, lo, hi, nq), patches the changed limbs at bytes `ECP_HDR_BYTES + 8 (hi - 1 - i)` through `patch_bytes` (checked against the old limbs, rewritten in whole 4 KiB blocks, O_DIRECT when the writer used it, fsynced, cut back, read back), then the header's `dres` residues adjusted by sum (new_i - old_i) B^(i - lo) mod q and read back; all changed limbs are patched, including those holding digits past d_out. `mn_out_tail_core` keeps the corrected low limbs and calls it when packed; `patch_bytes` now decides O_DIRECT by `out_mode(name) == OUT_DIRECT` (the writer's own rule; same on defaults); the packed branch of `out_run_one` uses `d_out + 1 - min(t2_defer, d_out + 1)`. The sidecar gets corrected residues and tail; `recheck_*` unchanged. Cost: one limb block plus one header block per affected part (patch time <= 0.003 s). `mnaccept.sh` gets a `corr` step and `t_patch` in `unit`; `t_patch.c`, `test_unpack.py` extended; no new switch.

## HEALTH — node health gate (C4) and per-APU memory guard / high-water (C5)
**Results (all measured).**
| check | result |
|---|---|
| gate 1 node, sampler on | PASS, 1e9 digits identical, VERIFY OK, ecalc total 7.55 s; `device_hwm_apu_gb=22.14` (apu 0; per APU 22.14, 18.57, 18.61, 21.22) |
| review fixes (queue item 59, measured 2026-11-01): gate 1 node, sampler on | PASS, 1e9 digits identical, VERIFY OK, ecalc total 7.92 s |
| layout P, 1 node, 4 processes (`ECALC_PE_PER_GPU=1`), margin 1000 GB, 1e8 | rc 8; each process names its own APU (APUs 0, 1, 2, 3), e.g. `APU 1 has 129.63 GB free (MemFree 128.54 + file cache 1.08) but the plan needs 1029.57 GB (planner's busiest-APU peak 29.57 + margin 1000.00): short by 899.94 GB` |
| layout P, 1 node, default margin, 1e8 | rc 0, VERIFY OK; four `fit apu check: ok` lines, one per own APU (free 126.5-129.8 GB vs 34.57 GB) |
| layout D, 2 nodes, margin 1000 GB, 1e8 | the launcher's step ended after 3 s (rc 143 = the launcher killed the other ranks after the refusing rank exited 8); `FIT APU CHECK FAILED … APU 0 has 126.49 GB free … short by 895.03 GB` on rank 0; no hang |
| `t_nodemem` | 5 checks, 0 failed |
| unit tests (`NodeHealth`), registry, doccheck | OK, 0 problems |
| 1e10, 1 node: `device_hwm_apu_gb` | 41.48 (apu 3); per APU 41.09, 40.96, 38.19, 41.48; VERIFY OK, total 24.1 s |
| same run, planner | 41.38 GB per APU (modeled, `memory fit:`); block-pool peak-live 3.89-5.0 GB per APU (measured; the block pool is only a small part of the APU's demand, so the 61.4 GB of DIAG is not comparable with it) |
| guard mock tree, MemFree 5.12 GB, guard 10 | rc 9, "APU 0 (NUMA node 0) MemFree 5.12 GB is below it" |
| guard mock tree, guard 1 | rc 0, VERIFY OK |
| guard real tree, guard 1000 | rc 9, names APU 0 |
| node screen, 1 node | prints ue 0, ce 0, boot 2026-11-01T07:47:54Z |
| mock ue=1 | no option: rc 0, `WARN EDAC ue=1` and a `health:` line; `--refuse-ue`: rc 3, node in EXCLUDE |
| nodesnap pair around 1e9 (1 node) | `NIC_TOP=... fec=78 pause=0 stall=0 drop=8` (cxi0 fec 32, cxi1 20, cxi2 12, ...), `HEALTH=1 node(s): ue>0 on 0, rebooted during the run: 0` |
**Verdict.** Adopt for the node screen (all off or read-only; digits unchanged). Caveat on `device_hwm_apu_gb`: MemFree counts reclaimable page cache as used, and APU 0 starts about 65 GB lower (41.8 GB free against 107-111); in the 1e10 run APU 0 fell to 0.66 GB free, i.e. the kernel was reclaiming cache there, so its 41.09 GB is a floor-limited figure, not exact demand. APUs 1-3 (40.96, 38.19, 41.48 against the plan 41.38) are the usable numbers. A `MemFree + file cache` variant would fix this; not done (needs the user's choice).
**Open issues.** (Review fixes 2026-11-01.) The per-APU figure and guard now watch only the APUs the process drives: layout D (one process per node) the four NUMA nodes 0-3, layout P (`ECALC_PE_PER_GPU`) the NUMA node of the process's own device (`mem_own_device` and `mem_numa_node_of_device`); if layout D finds a node count other than 4, or the own device's node has no MemFree file, a `DISABLED` line is printed and the per-APU guard is off. The `device_hwm_apu_gb` line is printed before the guard stops the run. rc 9 from the sampler thread: the bring-up script launches through `ecalc/mnrun.sh`, which adds `srun --kill-on-bad-exit=1` for more than one node and for layout P (also on one node), so a PE stopped by a guard ends the step; a one-node layout-D run is a single process and needs none (read from the code, not tested with a forced multi-node guard stop). Re-check after the fixes (1 node, aac7, measured): gate PASS, digits identical, `device_hwm_apu_gb=22.52` at 1e9; guard mock-low rc 9 with the hwm line printed before the stop; mock-ok rc 0; a 3-node mock tree prints DISABLED and the run passes. Layout P (own-device node) was not run live. The per-APU guard sits in the sampler only; the start-up `budget_check` in `ecalc.c` (sum of ranks' planned peaks against MemAvailable) was not given a per-APU form (it would need a per-APU demand model). NIC deltas were taken on 1 node (little traffic); a 2-node pair was not run. The bring-up script's stair excess rule was changed by reading, not run end to end on a stair (bash -n only). Sampler HWM is per process: with one process per GPU each PE prints the same physical node figures.

### MEMFIT — per-APU free memory with the file cache, and a start-up per-APU check (user decisions Q140 items 1 and 2)
**Results (all measured unless marked).**
| check | result |
|---|---|
| gate 1 node, sampler on | PASS, 1e9 digits identical, VERIFY OK, ecalc total 8.07 s; `device_hwm_apu_gb=20.72` (apu 3), `device_hwm_apu_memfree_gb=20.74` |
| 1e10, 1 node, per APU `device_hwm_apu_gb` (new: MemFree + file cache) | 53.43 (apu 0); per APU 53.43, 40.49, 38.63, 40.92; VERIFY OK, ecalc total 19.84 s |
| same run, `device_hwm_apu_memfree_gb` (old: MemFree only) | 53.43 (apu 0); per APU 53.43, 40.49, 38.63, 40.35 |
| start free per APU (new / MemFree only), GB | 112.15, 113.62, 113.66, 113.08 / 96.50, 112.80, 111.71, 41.01 (APU 3 held 70 GB of file cache this time) |
| lowest free per APU (new / MemFree only), GB | 58.72, 73.14, 75.03, 72.16 / 43.07, 72.31, 73.08, 0.67 (MemFree alone showed APU 3 nearly empty; with the file cache it never fell below 72 GB) |
| planner, same run (modeled) | mean 41.38 GB per APU (`memory fit:`); busiest APU (`dev_apu_max`) 51.05 GB; the measured 53.43 GB on APU 0 is 2.4 GB above the busiest-APU figure |
| GPU memory as file pages | no: over the run MemFree fell by 52.2, 39.5, 37.7, 39.4 GB (nodes 0-3) while Active(file) + Inactive(file) did not rise (0.00 GB on every node; node 3's 70 GB file cache fell by 1.0 GB); Unevictable 0.00 GB and Mapped <= 0.1 GB throughout; Active + Inactive (anon) rose only 3.7, 3.7, 1.8, 4.1 GB. So the HBM allocations (hipMalloc, VMM) are on no LRU list; the file cache is only what files left behind |
| start-up check on, 1e10 | `fit apu check: ok: free memory (MemFree + file cache) APU 0 112.2, APU 1 113.6, APU 2 113.7, APU 3 113.1 GB vs the plan's busiest-APU peak 51.05 + margin 5.00 = 56.05 GB`; run proceeds, VERIFY OK |
| start-up check on, margin 1000 GB, 1e8 | rc 8, before any init line: `ecalc: FIT APU CHECK FAILED (ECALC_FIT_APU_CHECK=1) on rank 0: APU 0 has 112.69 GB free (MemFree 97.04 + file cache 15.65) but the plan needs 1018.83 GB (planner's busiest-APU peak 18.83 + margin 1000.00): short by 906.14 GB; stopping before any GPU work, rc 8`; 0 init / VERIFY lines |
| switch off (default), 1e8 | rc 0, VERIFY OK, no `fit apu check` line |
| `t_nodemem` | 9 checks, 0 failed (file pages summed, absent file lines = 0, MemFree-only reader unchanged) |
| unit tests, `switch_registry --check`, doccheck | 57 tests OK, 0 problems, 0 problems |
**Verdict.** Adopt MemFree + file cache as the per-APU free memory: it is the memory a run can really get (the kernel reclaims file cache on demand; the old figure called a node with 70 GB of reclaimable cache "full") and GPU allocations do not hide in it. The high-water drop is nearly the same either way (differences <= 0.6 GB here) because the run itself takes anon-like HBM; the difference is in the free level, which is what the guard and the start check compare. The start-up check is correct and refuses before GPU work; it stays off until the user adopts it. Caveat: the measured high-water on APU 0 (53.43 GB) exceeded the planner's busiest-APU peak (51.05 GB), so the check's margin should cover at least that gap (the 5 GB default does, 2.4 GB measured); the host-side NUMA-local memory (pinned staging, runtime) is taken from the same HBM and is not in `dev_apu_max`.
**Open issues.** The check uses rank 0's layout figure for every rank; with `XSHMEM_PES_PER_GPU` > 1 several PEs share one APU and the figure is not multiplied (not tested). Margin is the larger of the guard and the device margin. A layout-P process whose device is unknown prints a stderr warning and is not checked.

## MAPBENCH — S1/E1: helper processes that create the GPU allocations, against one process with four threads (2026-11-01)

Program: `ecalc/tests/t_mapbench.c` (`make tests/t_mapbench`; standalone, not linked into ecalc). One aac7 node, 4 APUs, queue item
run 2026-11-01 about 11:07-11:25 ET (ROCm 7.2.4). Per chunk: `hipMemCreate`, `hipMemMap`, `hipMemSetAccess` (4 APUs), then a kernel
writing one word per 4 KiB page (first touch). Size as ecalc: 20 chunks of 2 GiB per APU (40 GiB per APU, 160 GiB per node), plus a
quick size (4 x 0.5 GiB). Wall = from the go signal to the last first-touch kernel, HIP initialized and address ranges reserved
before (helper start-up is separate, `helper_init`, 0.4 s, and overlaps the main process's own start-up). Three repetitions per
arm and load, run interleaved, one process per repetition.

Arms: **A** one process, 4 threads, global lock around a chunk's create+map+access+touch (today's background mapper); **B** as A
without the lock; **C** as B, threads pinned to their APU's NUMA node CPUs; **D** 4 helper processes (one per APU, pinned to its
NUMA node) create with `requestedHandleType = PosixFileDescriptor`, `hipMemExportToShareableHandle`, send the fd over a Unix socket
(SCM_RIGHTS); the main process's 4 threads (unpinned) `hipMemImportFromShareableHandle`, map, set access, touch; **E** as D with
the main threads pinned. "Load" = 48 threads copying 128 MiB buffers in a loop (memory bandwidth, started 0.7 s before the timing).

### Results (measured; full size, median of 3, min-max in brackets, seconds)

| arm | wall, no load | wall, load | speedup vs A (no load / load) |
|---|---|---|---|
| A lock | 15.87 (15.16-15.88) | 15.77 (14.42-16.83) | 1 |
| B no lock | 15.65 (15.19-15.89) | 15.46 (15.23-15.61) | 1.01 / 1.02 |
| C no lock, pinned | 12.29 (12.25-12.37) | 12.41 (12.38-12.42) | 1.29 / 1.27 |
| D helpers | 3.82 (3.81-3.83) | 3.87 (3.82-3.89) | **4.2 / 4.1** |
| E helpers, main pinned | 3.82 (3.76-3.84) | 3.87 (3.82-3.88) | 4.2 / 4.1 |

### Answers

- **Pass criterion (D at least 2.5x faster than A, create through first touch): met, 4.2x measured** (4.1x under load). A to B shows the
  in-process serialization (four unlocked threads are no faster than one locked sequence: each phase of B takes about twice A's);
  pinning (C) recovers only 22 %; separate processes recover 74 %.
- **Where the page clearing happens (U1): at `hipMemCreate`, in the creating process.** The helper's create is 0.108 s per 2 GiB chunk
  (about 19 GiB/s); the importer's import is 1 ms per chunk and the first-touch kernel 10 ms per chunk (A: create 0.15 s, touch 0.00 s).
  So the gain exists: creation, the dominant cost, moves to the helpers and runs in parallel across processes.
- **What remains in the main process** is map + access: 3.2 s per APU for 20 chunks = 0.16 s per chunk, which is above the design's
  secondary target (import+map+access of 0.05 s per chunk) and is now the critical path (helper 2.2 s, main 3.8 s). Map and access were
  not compared with a local handle here (A's map 0.11 s is not comparable: its lock keeps the other threads out; B's 3.65 s is
  four threads contending). Not measured: whether the main-side map+access can be cut (the runtime's per-process cost).
- **Checks (all passed, measured):** data written by APU d is read correctly by APU d+1 through the main process's mapping (peer access
  after import works); unmap + remap of an imported chunk at another slot and back works (APU d and d+1 read the same data); after
  all helpers exited (waitpid) the imported memory reads back correctly and `hipMemGetInfo` used memory is unchanged (40.1 GiB per
  APU): **helpers need not stay alive** once the main process has imported the handle (the fd may be closed after import); releasing
  all handles returns the memory (free 87.8 -> 125.8 GiB on APU 0). Same results for D and E at the quick size.
- Shareable handles: ROCm 7.2.4 on aac7 accepts `hipMemHandleTypePosixFileDescriptor` for pinned device allocations on MI300A. No error.

### Caveats

- Standalone, not ecalc: a process that has done only HIP init; in ecalc the same helpers would start at program start and create
  while the plan fit and `mn_init` run, so their 0.4 s start-up is hidden, but the main process's other work competes for the
  runtime lock. The pass is for the creation phase; the gain in ecalc's init also depends on what binds (seed wait, rns_init tail).
- Memory: helper RSS (4 HIP runtimes) not measured here. The main process maps what the helpers created, one extra copy of nothing.
- 3 repetitions; the spread is under 4 % for D, up to 14 % for A under load.

### Integration, phase 1 (arenas only): `ECALC_MAPD=1` (2026-11-01)

Code: `ecalc/mapd.c`, `mapd.h`; hooks in `dbig.c` (`vmm_map_run` takes the next chunk from the APU's helper, `db_vmm_arena_alloc` sends the
request, the mapper lock and the background mapper are unchanged) and `ecalc.c` (`mapd_early()` at main start, helper entry on
`--mapd-helper`). The helper is this executable; one per APU, NUMA-pinned, socket pair with fd passing; a helper exits after its last chunk.
Failure (spawn, helper init, create or export, 60 s without a chunk, import) kills that helper, prints one `ECALC_MAPD:` line, and the rest of
that APU's chunks are created in-process. Plane pools stay on `hipMalloc`; not used with `ECALC_PE_PER_GPU`. Test knob `ECALC_MAPD_TEST_DIE=<dev>:<k>`.

### Integration, phase 2 (plane pools) and review fixes (2026-11-01)

Code: `mapd.c` serves a FIFO of (kind, count, size, final) requests: the plane pools' chunks first (`rns_init` sends the requests after its device-memory
measurement, in today's creation order: pool 0, pool 1, then the arena), then the arena. `mapd_pool_alloc` builds one VA range over the imported chunks
(`hipMemAddressReserve`, map, access for all APUs); `dpool.vmm` (set by `rns_init`) makes `dpool_malloc` in `mem.c` call it, through two function pointers
(`mem_vmm_pool_alloc`, `mem_vmm_pool_free`) that `rns_init` sets. Any failure kills that helper, logs one line, and the rest is created in-process; a pool
that cannot be built falls back to `hipMalloc`. Test knob `ECALC_MAPD_TEST_DIE=<dev>:<k>[:p]` (`p`: die while serving pool chunks). A log line
`ECALC_MAPD: plane pool of X GB on APU d built from N chunks of the helper` proves the path (it also appears when the chunks were made in-process after a helper failure).

### Known limits

- With the switch on, the helper creates the whole arena (both halves) at the `rns_init` request, whereas switch 0 maps the second half only after `db_vmm_bg_release`, so the seeds see less free memory early. Not tested at the edge size; `cache_avail` slot counts may change (digits unaffected).
- The helper keeps its own handle until it exits, so on a pool regrow while the helper lives, the memory returns only at helper exit.
- An arena requested after `rns_init` returns gets no helper (it is created in-process).
- Growth/restore under `g_pool_mx` can wait up to 60 s on a slow helper.

## POOLRACE — free or remap of a dbig pool block while GPU work still uses it (C1POOL)

### 1. Audit (code reading, main 260d2c18; every line number is of `ecalc/` at that commit, not of the branch, where the switch code shifted them)

Facts the verdicts rest on (all read in the code):

### 2. Method, results, verdict

**Results (measured, aac7, single 1e9 runs, not a timing result).**
- Audit: 21 rows, 0 UNSAFE, 1 UNCLEAR (the `vmm_make_room` unmap), the rest SAFE.
- 2 nodes: `DB_FREE_SYNC=0` PASS 14.62 s; 1 PASS 12.56 s; 2 PASS 12.48 s. 1 node: 0 PASS 8.00 s; 1 PASS 7.66 s. VERIFY OK, digits identical, no segv.
- `mnaccept.sh --only unit,e9` with `DB_FREE_SYNC=1`: rc 0; t_seed, t_dist, t_mn_grid VERIFY OK; e9 identical.
- `gate.sh --build` fails at "python unit tests" (`tests/test_site.py`), also on main: environmental, pre-existing. The build, registry check and doccheck were run by hand: rc 0.
- Pressure: 0 VMM remaps in 24 runs (4e9 and 1.6e10 digits; `ECALC_DEVICE_GB` 250/170, `DM_TIGHT=0`, `BS_ARENA_ROOM=0`, `BS_ARENA_TRIM_GB` 2..12; the trim is clamped to about 0.5 GB). All VERIFY OK. No pressure pair exists.
- ABBA cost of `DB_FREE_SYNC=1` (2 nodes, 1.6e10, item 45_c1pool_abba2, 2026-11-01 04:24 EST, 4 ABBA rounds, totals 22.75-29.17 s; measured): B - A = -0.30 s, 95 % CI [-1.96, +1.37] s (t, 3 df): not resolved, any cost is below about 1.4 s (6 %) at this size. Round 1 holds the slow first run (29.17 s). Its two extra pressure settings again gave 0 VMM remaps (one stopped at the memory guard, rc 9).

**Verdict.** Inconclusive. No caller proven UNSAFE and no race reproduced; the switch is harmless in the gates (no slowdown seen at 1e9, one run each). The hypothesis is neither confirmed nor excluded at sizes where remaps occur. Recommendation: keep the switch (default 0), do not enable it.

**Open issues.** (1) Remaps were never exercised on aac7; a test that shrinks the arena directly is needed. (2) ABBA cost unmeasured. (3) UNCLEAR unmap row relies on the driver ordering of `hipMemUnmap`.

## STRESS — forced VMM remaps of the dbig block pool, with the CRT/VERIFY checks on (C2)

**Hazard.** The knob can itself create the race it tests for: with `DB_FREE_SYNC=0` it unmaps chunks of a block that was just freed while a stream may still use them; with `DB_FREE_SYNC=1` the sync precedes the free, so work enqueued later is unprotected (the same holds for RDMA-registered memory). A failure with the knob on and `DB_FREE_SYNC=0` is therefore not evidence of a product bug until it is rerun with `DB_FREE_SYNC=1`. The knob is never on by default and never in the node screen. Consequence for the clean s0 arms: had any caller freed a block early in the exercised paths, the knob would have turned that into a fault; 0 failures in the s0 arms with the knob on is evidence that no early free happened there (exercised paths and sizes only). Driver `tools/stress/stress.sh`, summary `tools/stress/summarize.py`.

**Results (all measured, aac7, 2026-11-01).**

Knob proof (1 node): 1e9 digits, n=1: 28 remaps per APU (about 1000-1800 chunks moved per APU), VERIFY OK, digits identical; 4e9 digits: 112 remaps per run, identical. Remaps per run are about 30 per APU at 1 node whatever the size (the number of pool allocations and frees of the division phase); at 2 nodes with the diagnostics and 16 MiB chunks a 2e10 run does about 11,000 and a 6e10 run about 12,300 (all APUs and processes).

Gates for the knob: 1 node, 1e9 digits, knob off and on (three builds, the last on the tree merged with main 1250160c): PASS, VERIFY OK, digits identical. 2 nodes, 1e9 digits: off PASS (12.1 s ecalc), on (n=1, 16 MiB chunks) PASS (118 s: the remaps cost 21-45 s per APU there), P24 in use. `switch_registry --check` rc 0, doccheck rc 0 (see RESUME). Unit tests with the knob on, 1 node: `t_dbig 0` VERIFY OK (555 checks), `t_newton 20` VERIFY OK (2752 checks).

Arms (runs / flags / VERIFY failures / digit differences / remaps):

| item | size | nodes | runs done (planned) | clean | CRT flags | P24 flags | VERIFY failures | digit differences | remaps (all forced) |
|---|---|---|---|---|---|---|---|---|---|
| 1-node, DB_FREE_SYNC=0, 8 arms | 2e10 | 1 | 14 (24) | 14 | 0 | 0 | 0 | 0 of 14 compared | 744 |
| 1-node, DB_FREE_SYNC=1, 8 arms | 2e10 | 1 | 15 (24) | 15 | 0 | 0 | 0 | 0 of 15 | 868 |
| 2-node, 16 arms | 2e10 | 2 | 14 (32) | 14 | 0 | 0 | 0 | 0 of 14 | 66,096 |
| 2-node, 16 arms | 6e10 | 2 | 16 (16) | 16 | 0 | 0 | 0 | not compared (no digit file) | 98,624 |
| total | | | 59 | 59 | 0 | 0 | 0 | 0 | 166,332 |

Why fewer runs than planned: every item had a start-time cap (60 min at 1 node, 85 min at 2 nodes, so each item stayed within about 2 h) and no run was stopped by an anomaly. Runs with the knob on are slow, so the cap was reached: 1 node about 105 s (knob off) vs 120 s (on); 2 nodes at 2e10 about 75 s (off) vs 410-420 s (on, 5.5 times: the forced remaps of the 16 MiB chunks cost 21-45 s per APU per run); at 6e10 about 75 s vs 470-520 s. The 2-node 2e10 item therefore did one repetition of 14 arms (arms k1s1d1r0 and k1s1d1r1 and the whole second repetition did not start); the 1-node items did 1-2 of 3 repetitions in the arms' round order (the last arms of the final round are missing). The d and r letters change nothing at 1 node (no P24, no exchange rounds there).

NIC delta (Q140 item 3; `rl_nicdiff` of `remote/run/lib.sh` on `nodesnap.sh` snapshots before and after the first 2-node run, arm k0s0d0r0, 2e10 digits, 2 nodes, 8 NICs listed per node; measured, bytes estimated from the packet-size buckets, mean per node, spans the snapshot interval and the run): tx 517.8 GB and rx 517.8 GB per node; error-like counts 334 per node (mean), paused counts 911,126 per node. Top-5 by FEC + drops: node A `fec=752 pause=10665 stall=6.97e9 drop=8`, node B `fec=554 pause=10960 stall=6.71e9 drop=7` (the bring-up script's columns: FEC codewords/lane errors, pause events, link-layer-retry stalls, drops). Per NIC (cxi0 of node A): 82.58e6 packets sent, 82.58e6 received, error-like 190, FEC 376, drops 2. So the deltas show real traffic and small FEC/drop counts on all NICs; no run was affected (digits identical). The full per-NIC table is in the run's `nic.txt` on the cluster.

**Verdict.** Flags not reproduced; a software cause is not shown at this scale. "0 flags" covers the exercised paths and sizes only and is a lower bound on what could exist; the knob's own hazard (above) is part of the reading: a clean knob-on s0 arm is the stronger result, a failure would first need the `DB_FREE_SYNC=1` rerun. The harness made the remap path run 166,332 times in 59 runs (about 30 forced remaps per APU per 1-node run, about 1,500 per APU per 2-node run), at 1 node 2e10 and 2 nodes 2e10 / 6e10 digits, with `DB_FREE_SYNC` 0/1, the P24/sequence/round diagnostics and tiny exchange rounds, with 0 CRT flags, 0 P24_CHECK flags, 0 VERIFY failures and 0 digit differences in the 43 runs whose digits were compared (the 16 runs at 6e10 are VERIFY-checked only). Limits: the target's sizes (3.7e13 digits, 576 nodes, 2304 PEs, 233 KB pairs on a real fabric) are 600 times larger and the pool's remaps there are in the real memory-pressure regime, which no aac7 setting reaches; the forced remaps move only free chunks and the chunks are 16 MiB, not 2 GiB; one repetition per 2-node arm is too few to see a rare event (a 0.1-per-run failure rate would have a 90 % chance to show in about 22 runs of the same kind; assumed, if the cause were reproduced by this stress). The remap path itself (hipMemUnmap / hipMemMap / hipMemSetAccess, 166,332 times) ran without an error.

**Open issues.** (1) The hardware lead (two target nodes with uncorrectable HBM errors) is untouched by this work. (2) The knob raises the run time 5-6 times at 2 nodes; a cheaper variant (fewer chunks moved per event) would allow more repetitions. (3) No repetition 2 at 2 nodes; no 6-node arm (aac7 has 2 usable NIC nodes).

### M1 — corruption soak at the record per-node load (aac7 campaign, 2026-11-01 22:00 to 2026-11-02 20:08 ET; main d7c7f28; all counts measured)

| group | items (nodes) | arms | runs | clean | forced remaps (all remaps) |
|---|---|---|---|---|---|
| 1 node, 6.6e10, layout D (one node-process: DR inactive) | 61_m1_b1, 61_m1_b2, 71_m1_b3 to b6 (lane-B nodes s4b0, s3b1, s6b0, s4b1) | k0/k1 x s0/s1 x d0/d1 (b1, b2 four arms, b3-b6 three) | 20 | 20 | 1,584 (1,632) |
| 1 node, 6.6e10, layout P (`ECALC_PE_PER_GPU=1`, four node-processes: DR active) | 71_m1_p1 (s1b0), 71_m1_p3 (s5b0) | k0s0d0r0 (3 + 3 runs), k1s0d1r0 (2 + 2) | 10 | 10 | 11,836 (12,048) |
| 2 nodes, 1e11, layout D (two node-processes: DR active) | 62_m1_a1, 72_m1_a2 (s1b0 + s5b0) | k1s0d1r0, k1s1d1r1, k0s0d1r1 | 6 | 6 | 19,424 (19,424) |
| 1 node, layout P, failed start | 71_m1_p2 (s5b0) | k0s0d0r0, k1s0d1r0 | 2 | 0 (rc 255) | 0 |
| total | | | 38 | 36 | 32,844 (33,104) |

- **Result.** 36 of 36 runs that started the computation were clean: rc 0, VERIFY OK, 0 CRT flags, 0 P24 flags, 0 digit differences against the reference hash (36 comparisons, all identical). The soak found no corruption and no fault of the pool-remap path: 32,844 forced remaps (measured, summed from `runs.tsv`; 33,104 including the 260 remaps caused by memory pressure without forcing: 16 per unforced layout-P run, 29 per forced layout-P run, 4 per forced layout-D 1-node run).
- **The two failed runs (71_m1_p2, flagged).** Both arms stopped at start-up on s5b0 within 13 s: the libfabric provider call returned -61 on all four node-processes (no data available), before any computation, rc 255. The identical item rerun later on the same node (71_m1_p3) passed all 5 runs. Likely a transient node or NIC fault (inference: the same node passed on rerun; cause not established). The two logs are the only ones kept (the harness keeps full logs only for anomalies).
- **Cost of the forced remaps (measured, wall seconds per run).** 1 node layout D: arms without forcing 310-328 s, with forcing 331-390 s (`DB_FREE_SYNC=1` and diagnostic words not separable in these single runs); layout P: 309-313 s without, 413-485 s with (mean 445 and 449 s, two runs each); 2 nodes: 298 s (k0s0d1r1, one run) against 584-701 s with forcing. Stress wall times include writing the digits for the hash comparison and are not comparable to the plain-run times of M4.
- **DR false positives.** Not measurable from the retained data. A false detection that caused a retry would show only in the exit line `DR: <checks> checks, <injected> injected, <failures> failures, <recovered> recovered ..., <fatal> fatal` of each process, and the harness deletes the logs of clean runs. What is established: no run ended fatal or with rc != 0 from DR (0 of 36 clean-start runs), and no digit difference. DR was active in 16 of the 36 runs (10 layout P, 6 at 2 nodes) and inactive in the 20 layout-D 1-node runs. The count of silent recoveries without injection (failures with 0 injected) stays unknown; to get it the harness must save the `DR:` lines of every run (a three-line change in `tools/stress/stress.sh`, not made here).

### M5 — switches combined (aac7 campaign, 2026-11-02 05:41 to 11:52 ET; main d7c7f28; measured)

`ECALC_DR=3` and `XSHMEM_ROUND_ACTIVE=1` together, gated against the reference hashes at the three sizes the switch-combination rule uses (`tools/stress/stress.sh`, one run per arm; arms as in M1):

| item | size | arms | rc | VERIFY | CRT / P24 flags | digits vs reference | wall s (ecalc total s) |
|---|---|---|---|---|---|---|---|
| 64_m5_a2r | 2 nodes, 1e11 | k0s0d0r0, k0s0d1r0 | 0, 0 | OK, OK | 0 / 0 | identical (e19c27c362478b18...) | 442 (428.9), 330 (318.8) |
| 77_m5_a3 | 3 nodes, 1e10 | k0s0d0r0, k0s0d1r0, k1s1d1r1 | 0, 0, 0 | OK x3 | 0 / 0 | identical (25ead685ef76d83c...) | 34 (30.6), 34 (29.5), 192 (187.1; 4,980 forced remaps) |
| 77_m5_b1 | 1 node, 6.6e10 (lane B, s3b1) | k0s0d0r0, k1s1d1r0 | 0, 0 | OK, OK | 0 / 0 | identical (c356733c88cae1a0...) | 330 (203.7), 375 (250.0; 132 forced remaps) |

7 of 7 runs pass: 0 digit differences, 0 flags. The planned fourth item (the variant with `ECALC_MAPD=1`, once the S4 fix is merged) did not run; re-run the three sizes after every merge. Notes: the two 2-node runs differ by 111 s (442 vs 330 s) for the same size; the faster one is the arm with the diagnostic words on, so they do not explain it; the difference may be the slow-mode variance described in results/TRANSPORT.md; unverified. At 1 node the DR checks are inactive, so that run gates the round switch only.
