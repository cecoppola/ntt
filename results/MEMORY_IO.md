# MEMORY_IO — Memory, output I/O, health, stress

Scope: the memory model and guards, the output writer, node health, the map benchmark, the pool race, the stress switches. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed). Superseded or purely historical detail was moved 2026-11-10 to `archive:MEMORY_IO_ARCHIVE_2026-11-10.md`; each section keeps its `§ID` heading (cited by other files) with a status line and a pointer. Earlier originals: `archive:repo_cl6_2026-10-25/results/MEMORY.md`, `OUTPUT_IO.md`, `HEALTH.md`, `STRESS.md`, `POOLRACE.md`, `MAPBENCH.md`; older text `archive:results_merged_2026-10-19/`.

## MEMORY — device and host memory

Scope: device and host memory: accounting, the arena and its sizing, the reserved tail, init floor, early free, the memory ceiling. Cite items as `results/MEMORY_IO.md §<ID>`.

### Topic summary

What this is: how ecalc maps and sizes device (HBM, 4 APUs per MI300A node) and host memory, and what the per-node ceiling is. Numbers are measured on aac6 (one node, 502 GB total) unless labeled; the 576-node figures are modeled. `ecalc/README.md` governs any switch default.

- **Layout.** One arena per APU holds both bs parities plus the dm extra (`region_need` / `dm_layout` / `tree_need_dev` in `binsplit.c`; twin `mem_model.py`, exact to the byte); the block pool keeps a reserved tail for the reciprocal's largest block, so there is **zero in-phase hipMalloc** at every size run (10¹¹ on one node: device 431.2 GB, node peak 445 of 502 GB). The arena is a VMM range in whole 2 GiB chunks (`DB_POOL_VMM`); the tight reservation of the division (`DM_TIGHT`) is on with it and has no switch; `BS_ARENA_ROOM` is 0.14 (0.16 until Q99, 2026-10-25); `BS_SEED_FILL` is 128 by default; the seed pull kernels have no switch; `ECALC_DM_POOL`, `DM_TAIL_DEAD`, `RNS_POOL1_4Q` and `BS_POOL_RULE` (default 1, fixed the 10¹¹ on 4 nodes abort) are gone or built in.
- **Host.** X never lives on the host and the seeds stream straight into the regions: host HWM at 4 × 10¹⁰ fell 48.8 → 11.7 GB; at 10¹¹ about 14 GB.
- **Init floor.** Init time is 0.06-0.075 s/GB × bytes mapped, a kernel page-clearing cost on one lock that no allocation form, thread count or reordering beat (§K8, §P3, §MAP15, all stopped); helper processes that create the allocations break the serialization (§MAPBENCH, `ECALC_MAPD`).
- **Plane cap `run` (default since 2026-10-25, user decision).** Unset = `run`: 2^30 when the run's largest single product (the division's `2k + 8` limbs) fits one mn plane of that cap (about 9.6e9 digits per node and below, assumed bound), else 2^31; `ECALC_PLANE_CAP=2^31` forces the old cap; `ECALC_PE_PER_GPU=1` with `XSHMEM_PES_PER_GPU` > 1 keeps `fit`; the verbose line reads `run (default):`. Q122 (aac7, m): 2^30 cuts the fixed start-up of the two plane pools (25.8 GB per APU at 2^31) by 2.64 s [-2.98, -2.30] at 8e9 on one node and 2.05 s at 1e9, compute unchanged, digits identical (hash 3e78c1b2a0276963 at 8e9); about -2.8 s at 2 nodes with 8e9 per node. The models `mem_model.py` / `mn_model.py` still assume 2^31 unless a cap is passed.
- **Ceilings (one node).** Three primes (default `ECALC_NP=3`): real edge 1.44 / 1.30 / 1.14 × 10¹¹ at caps 2³⁰ / 2³¹ / 3·2³⁰ (device + host HWM ≲ 524 GB runs, ≈ 529 GB is OOM-killed). The 2026-11-10 M4 rerun passed 1.15e11 on one node (§M4). Modeled 576-node ceilings (4.0-5.5 × 10¹³ depending on switches) are in `results/MODEL.md` §PIECES and §FIT.
- **Dropped / stopped:** `DB_POOL_VMM_STREAM` (§MAP15: no gain, two node reboots; removed), `RNS_HOOK_CONCUR` and `ECALC_VMM_FILL` (§K8: slower), `DM_TAIL_DEAD=2` / E5 spill (≈ 0 over `DM_TIGHT`), host-side allocation forms.
- Live open items: spill buffers and top-level plane growth at g = 576, the host term at size > 1 on the target, the mn division peak at 576 (counted, not measured), an intermittent leaf fault seen once in Phase 14 (fixed, `results/CORRECTNESS.md` §A114).

### §AS15 — Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps)

Status: current (`BS_ARENA_ROOM` later changed 0.16 -> 0.14, Q99). User decision: adopt `BS_ARENA_ROOM` with `BS_SEED_FILL=128`; the whole-chunk rounding alone is free and removes the reciprocal's remaps. Open: f was chosen on 17 traced sizes and placement is greedy, so an untested size can still remap (one traced run answers any size); `mem_model.py` node peak with the room is pessimistic (+16.3 GB at 10¹¹, one-node ceiling 1.24 vs 1.34 × 10¹¹ modeled) while the run cannot be refused (budget rule).

### §K8 — init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12)

Status: **closed, diagnosis only** (aac6, one node, 2026-10-12; code unchanged). The init is the mapping floor; reordering the seeds and the mapping does not help (differences 2.0-11.5 s against a default spread of 1.7 s). Not built: seed DMAs that need no HIP runtime call (≈ 4-5 s at 6.441e10, modeled; became the always-on seed pull kernels) and lazy parity-1 mapping. Patches and per-arm data: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §K8`.


### §P3 — shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED)

Status: **stopped, nothing built** (read of the code and A09 data, 6.441e10 per node). GPUs idle until level 1 at 24.4 / 28.3 / 29.4 / 35.1 s on 1 / 2 / 4 / 6 nodes (32 / 20 / 16 / 12 % of the run, m); any single fix is worth ≤ ≈ 1.5 s, both ≈ 3 s (mod). The only candidate (publish parity-0 chunks one by one, `ECALC_VMM_PUBLISH`, never started) saves ≈ 1.0-1.5 s at 1 node and ~0 at 2/4/6 nodes. Reading detail: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §P3`.

### §L114 — L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`)

Status: current in code. The tight reservation (`DM_TIGHT`) is on by default with the VMM pool and has no switch; `DM_TAIL_DEAD` was removed 2026-10-20; `BS_ARENA_TRIM_GB` (opt-in, default 0) is in §ATRIM below.

### §B4 — APU balance of the top bs levels: cause, design, zero-code test (2026-10-27; `BS_TOP_SPREAD` dropped by the user 2026-10-27, option 0 adopted)

**Outcome (2026-10-27).** Option 0 measured (aac7, 1 node, 1e11 digits, m): `BS_MDEV_LOGL=29` with `ECALC_OMP_TEAM=mask ECALC_RESIDUE=shoup XSHMEM_COLL=tree` against the defaults: APU 0 driver peak 109.9 -> 90.5 GB, all four 90.5 GB; planner device 386.72 -> 367.40 GB; ABBA -7.2 s (-5 %, not resolved); digits identical. The user dropped option 1 (`BS_TOP_SPREAD`, never built) and adopted option 0 for target runs from 1e13 digits (S6 of the removed `remote/ROUTE.md`, git history cfe9d3a1). The later cost of `BS_MDEV_LOGL=28`: `results/MODEL.md` §PIECES. The design below is kept for reference.

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

**Retained evidence.** The 2026-11-02 harness kept the full log only for a failed run and overwrote the memory files per size, so the comparison below exists for three points only (1 node 6.6e10, 2 nodes 1.5e11, 1 node 1.15e11). The 1-node gap is closed by the rerun below; the 2-node sizes are still not retained.

| point | planner per APU (device) | measured per APU | pool peak of the busiest phase |
|---|---|---|---|
| 1 node, 6.6e10 | 72.52 GB (290.09 per node: planes 25.92 + arena 45.10 + bs growth 1.50) | 71.0 GB in use at every phase on all 4 APUs (planes 25.77, pool 45.10, tables 0.15; 284.1 per node); driver view 71.2 of 137.4 GB | live peak 41.57 GB at the division, 92.2 % of the 45.10 GB pool; the dm need was predicted 44.33 GB |
| 2 nodes, 1.5e11 | 105.27 GB (421.08 per node; budget line: rank 0 device peak 412.9 GB) | not retained (no `mem [phase]` lines kept) | live peak 67.18 GB in the division window, 92.0 % of the 73.01 GB pool (dm need predicted 71.59 GB); node MemAvailable 59.5 GB, Cached 62.6 GB |
| 1 node, 1.15e11 | 105.81 GB (423.23 per node: planes 103.70 + arena 313.53 + bs growth 6.00) | high-water 113.73 GB (APU 1) after 45 s: per APU 111.83 / 113.73 / 111.29 / 109.02; a floor, since the run stopped in start-up | none (stopped) |

- **Agreement.** At 6.6e10 the device figure is within the planner: 71.0 used against 72.52 predicted (-1.5 GB per APU; (modeled reading) the bs growth reserve of 1.50 GB per APU not yet touched at the sampled points), and the pool peak stays 7.8 % below the pool (3.5 GB). At 2 nodes 1.5e11 the pool peak is 8.0 % (5.8 GB) below the pool. Both busiest phases sit at 92 % of the pool: the planner's pool size is tight by design (`DM_TIGHT`) and was not exceeded.
- **The 1.15e11 point is above the planner's device figure** (113.7 against 105.8 GB per APU, +7.9 GB, 7.5 %). Modeled reading: the sampler counts the whole NUMA node (device arena plus the process's host memory), and the host share is about 6 GB per APU (run RSS 25.1 GB at the stop, planner host 27.41 GB, /4 = 6.3-6.9 GB); 105.8 + 6.3 = 112.1 GB against the 111.3 GB mean measured. The two figures are then compatible, and the planner's device number is not the sampler's number. The earlier APU 0 overshoot at 1e11 (109.9 GB, §B4) is the same quantity.
- **The ceiling (measured, 2026-11-02; 1-node part superseded by the rerun below).** On the aac7 node at `ECALC_MEM_GUARD_GB=6` the 1-node run passed 1e11 (VERIFY OK) and stopped at 1.15e11 (guard at 45.0 s: MemAvailable 5.48 GB, MemFree 6.26 GB, Cached 82.95 GB, Dirty 0, RSS 25.11 GB, pool live 0) although the planner said FITS (+20.77 GB device, +29.36 GB node). The 2026-11-10 rerun passed 1.15e11, so the stop was caused by an external file that held about 230 GB of host memory then (not by the planner or the engine); the 1-node edge above 1.15e11 is unmeasured.
- **Cached against available.** 83 GB of "Cached" coexisted with 5.5 GB of MemAvailable, so that cache was evidently not reclaimable (inferred). Evidence: the sampler counts Active(file) and Inactive(file) as free and still saw a NUMA-node minimum of 0.04 GB (APU 1) and 0.67 GB (APU 0); at 6.6e10, 1 node, Cached was 13 GB with 233 GB available, and at 2 nodes 1.5e11 62.6 GB against 59.5 GB available. Modeled reading (not verified, no Shmem breakdown in the log): the large Cached figure is shared or mapped memory attributed to the process (arena mappings), growing with the pool, so Cached is not a safe margin and the guard rightly follows MemAvailable.
- **For the 6.04 GB per-APU margin (phase 1 C5).** Measured 6.6e10 (record per-node load) leaves 71.0 of 137.4 GB driver memory per APU; the busiest-phase pool peak keeps 3.5 GB per APU of pool unused. The margin is not tested at the edge: no passing point lies between 1e11 and the guard stop.

#### M4 rerun, 1 node (aac7, 2026-11-10; harness-gap fix, one memory file per size)

15 runs with VERIFY, 15 PASS, 0 failures, 0 infrastructure faults. Device high-water is the largest per-APU value of the run (measured); planner need is the pre-run estimate for the whole node (modeled). Wall is the harness time of the run, 1 node.

| Digits | Rep | Wall s | Planner need GB | Measured device_hwm GB (max APU) |
|---|---|---|---|---|
| 1e10 | 1 / 2 | 23 / 23 | 191.4 | 54.1 / 51.6 |
| 2e10 | 1 / 2 | 34 / 32 | 238.6 | 75.6 / 75.1 |
| 3.5e10 | 1 / 2 | 44 / 45 | 268.7 | 77.1 / 77.1 |
| 5e10 | 1 / 2 | 63 / 61 | 294.7 | 81.7 / 79.9 |
| 6.6e10 | 1 / 2 | 80 / 78 | 316.5 | 82.6 / 82.0 |
| 8e10 | 1 / 2 | 100 / 102 | 351.1 | 91.4 / 91.9 |
| 1e11 | 1 / 2 | 134 / 135 | 413.8 | 118.1 / 118.2 |
| 1.15e11 | 1 / 2 | 154 / not run | 450.6 | 119.7 / not run |

The need is the whole-node total; the measured value is the single most loaded APU of four. The planner is therefore conservative against the per-APU peak. Rep 2 at 1.15e11 was skipped by the memory pre-check (439.7 GB available, 450.6 GB needed plus 10 GB margin, after 120 s wait). Rep 1 at 1.15e11 passed, so the expected 1-node guard stop there did not occur: the external file that held about 230 GB of host memory in the earlier campaign is gone. The `DR:` exit lines are not expected on 1 node (one process, DR inactive).

### §MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). **STOPPED after a second node failure; nothing here is adopted except the print-only timeline.**
**Status: stopped after a second node failure; only the print-only timeline (`ECALC_INIT_TL`) was merged; `DB_POOL_VMM_STREAM` is not adopted.** (a) The stream removed the seeds' idle wait as modeled (seeds end 28.3 / 29.2 s instead of 34.3-35.5, m) but level 1 did not move: **the mapping itself is the floor**, ≈ 0.2 s per 2 GiB chunk whoever calls it (`hipMemCreate` ≈ 70 %, `hipMemSetAccess` ≈ 30 %); the runtime serializes the calls. (b) Seed threads at 10¹¹ (m): 192 (default) level 1 at 35.07 / 35.40 s; 96: 36.46; 48: 47.63: keep all threads. (c) Timeline: `rns_init` staging + contexts 1.5-2.5 s, plane pools 4.2-5.6 s (≈ 0.045 s/GB); seed pinned buffers cost 1.4-1.6 s. The remaining lever, the mapping rate outside the VMM path (hipMalloc 0.036-0.05 s/GB vs hipMemCreate 0.077-0.1), was addressed by the helper processes of §MAPBENCH. Gates, 10¹¹ x 7 runs and failure notes: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §MAP15`.


#### §ATRIM — BS_ARENA_TRIM_GB (trim the dm arena need; merged 2026-10-23 from ATRIM.md)

**Results.**

| size | arm | arena per node (GB) | time (s) | runs | remaps |
|---|---|---|---|---|---|
| 2 n 1.2882e11 | S | 266.3 (m) | 115.2 (m) | 7 | 0 |
| 2 n 1.2882e11 | TR8 | 231.9 (m), -34.4 | 111.4 (m) | 5 | 0 |
| 3 n 1.9323e11 | S | - | 149.4 (m) | - | 0 |
| 3 n 1.9323e11 | TR8 | - | 149.6 (m) | - | 0 |

**Verdict.** Opt-in, default 0: the memory saving is large at 2 n and time is neutral to slightly better. No adoption decision; no 4 n / 6 n data.

## OUTPUT_IO — output

Scope: output: streamed chunks, 60-bit packed digits, write scheduling on a network file system, the correction patch. Numbers are measured unless marked modeled (mod) or assumed (a). Unabridged older text: `archive:repo_cl6_2026-10-25/results/OUTPUT_IO.md`.

### Topic summary

- **What it is.** How a run writes and verifies its digits: the digit string never exists whole; each node formats its share in chunks (`MN_OUT_CHUNK_MB`, 256 MB), a writer thread `pwrite`s `<outfile>.part<k>` (k = size-1-rank, so `cat <outfile>.part*` is the exact file), and T1 (digit residues, term recurrence over each node's range) and T2 (windows) run rank-locally in the output stage (§A-out). Switch rows and defaults: `ecalc/README.md` (authoritative); format and checks: `ALGORITHM.md` §9.
- **Current defaults (main; `ecalc/README.md` governs).** **`ECALC_OUT_PACKED=2`**, the 60-bit packed part file, is the default since 2026-10-12 (user decision; §O4): 7.5 B per 18 digits (-6.25 % vs packed v1 = 8 B, 0.4167 B/digit vs 0.4444 vs 1.0 ASCII), digits identical, `tools/unpack_digits` converts (any consecutive subset of parts), RECHECK reads v1 and v2. `MN_OUT_EARLY` is on by default, `MN_OUT_DKM_HI` is built in, O_DIRECT is the writer default (`ECALC_ODIRECT=1`; best on both aac6 paths); `ECALC_CKPT_TOP` (the top set for RECHECK) is off by default and `=2` is the budgeted form; the correction patch of the packed file is built in (§KP15). `MN_OUT_STRIPE` defaults to 1 (unmeasured on Lustre).
- **Measured conclusions.** On a bandwidth-limited file system the bytes are the lever: packed output cuts the with-write wall 110.8 -> 65.3 s at 10^10 on aac6's NFS (-41 %), -25.7 s without the write and -57 s with it at 10^11 on local NVMe; aac7 4 nodes at 10^10: write time 40.6 s (ASCII) -> 10.5 s (packed v1) / 9.0 s (v2) per run (single runs; the file system varied 1.5-2x between runs). O_DIRECT halves the elapsed time with the top set on (154 -> 82 s at 4 x 10^10, local disk) and leaves nothing in the page cache. The write is file-system bound (0.06-0.12 GB/s per node on aac7 NFS; 0.116 GB/s on aac6's 1 GbE), not format bound.
- **`total` and the write.** `total` is printed before the writer is joined; on NFS the write rarely hides (`dc` shows "waiting for the writer"); `MN_OUT_EARLY=1` starts the part file in the division (at size > 1) so `dc` -> 0, with a net wall gain only where storage does not share the fabric link.
- **Fixed defects.** §KP15: the K patch aimed ASCII byte offsets at the packed file (VERIFY FAILED with the uncorrected digits on any correction after the writer started); fixed by a packed-form patch plus `t2_defer` in the packed writer; `mnaccept` has a `corr` step and `t_patch` in `unit`.
- **Unmeasured / open.** Everything about Lustre (O_DIRECT rate, aggregate at 64 / 576 nodes, stripes, waves; TARGET §6 item 5); more than 2-4 nodes for the packed multi-node writer beyond aac7's 4-node 10^10 check; corrections spanning several limbs end to end (only unit tests); a mode with forced correction under `ECALC_OUT_MODE`.
- **Superseded.** §IO15's packed v1 default (`=1`) is superseded by v2.

### §O4 — packed output adoption path, write rate and round trip

Overnight 2026-10-11/12 (branch `o4`, main 0298fe2). Question: what is missing to adopt `ECALC_OUT_PACKED=2`? Gaps found: no real multi-node `-o` text + `cmp` run, no RECHECK against the checkpoint set on 4 nodes, no write rate side by side per format; closed by the item below and adopted 2026-10-12. (The `tools/o4_*.sh` scripts were archived.)

**Collected results (aac7 4-node item 80 and aac6 CPX; measured; format 0 = ASCII, 1 = packed, 2 = 60-bit packed; rate = GB per node / writer-thread time; the file system was shared).**

| where | digits | fmt | GB (all nodes) | write (s) | GB/s per node | total (s) | n |
|---|---|---|---|---|---|---|---|
| aac7 4 nodes | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.40 | 3.23 / 1.14 / 1.41 | 0.083 / 0.103 / 0.092 | 26.0 / 21.3 / 23.0 | 2 |
| aac7 4 nodes (ckpt step) | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.40 | 2.52 / 1.03 / 0.90 | 0.101 / 0.107 / 0.111 | 23.3 / 19.8 / 19.2 | 1 |
| aac7 4 nodes | 1e10 | 0 / 1 / 2 | 10.00 / 4.44 / 4.16 | 40.62 / 10.50 / 8.98 | 0.062 / 0.107 / 0.116 | 73.4 / 36.8 / 39.4 | 1 |
| aac6 CPX 1 node | 1e9 | 0 / 1 / 2 | 1.00 / 0.44 / 0.42 | 8.88 / 3.88 / 3.69 | 0.113 / 0.114 / 0.114 | 8.5 / 8.2 / 8.8 | 2 |

### §IO15 — Phase 15 agent IO: output I/O on a network file system, packed digits, write scheduling, the memory guard

Phase 15 agent IO (aac6, every switch off by default at the time). **Verdicts (m unless marked):**
- W1 write mode on a network FS: **keep O_DIRECT**, fastest on aac6's NFS from a compute node (0.116 GB/s = the 1 GbE link) and the login node (0.64-0.96 vs 0.36-0.48 GB/s buffered + fsync); buffered forms hold up to 10.7 GB of page cache.
- W2 packed part file (`ECALC_OUT_PACKED`, later 2, §C05/§O4): 10^11 identical after conversion (sha1), RECHECK OK; 2.25x fewer bytes; -25.7 s without / -57 s with the write at 10^11 local; NFS with-write wall 110.8 -> 65.3 s at 10^10.
- W5d part file during the division (`MN_OUT_EARLY`, now default 1): identical at 10^9 (2, 4 procs, 2 real nodes), 3 x 10^9 (2 nodes), 10^10 (2 procs); `dc` -> 0; net gain depends on whether storage shares the fabric link (aac6 1 GbE: -2.9 s of 557; 2 procs at 10^10: -7 s of 116).
- W5b waves (`MN_OUT_WAVES`): serialized at 2 nodes (+14.4 s); removed 2026-10-20. W5a stripe (`MN_OUT_STRIPE`): command path tested with a fake `lfs`, unmeasured on Lustre. W7 O_DIRECT by FS type: local and NFS detected, Lustre assumed O_DIRECT. N3 memory guard (`ECALC_MEM_GUARD_GB`): rc 9 with one line naming rank, host, phase.
- Open: everything about Lustre is unmeasured (O_DIRECT rate, aggregate at 64 / 576 nodes, stripes; `tools/wbench` measures it without a GPU); the packed file has no per-chunk checksum beyond the part residues; `MN_OUT_EARLY` holds about 0.9 GB of host buffers (`MN_OUT_CHUNK_MB=64` quarters it).
- Old 576-node estimate at the then-target 4.25 x 10^13 (mod): about 238 s without the write, packed at 0.6-0.8 GB/s 264-278 s; superseded by the TGT write table in `results/MODEL.md`.

### §KP15 — the correction patch on packed output (a defect on main, B2 44c1642)

**The defect (reproduced on main 44c1642, fixed).** `ECALC_CORR_PATCH=2` patched the file tail at **ASCII byte offsets** while `ECALC_OUT_PACKED=1` writes a 4096-byte header plus 8-byte limbs. When a correction came after the writer started (size 1: always; size > 1: with `MN_OUT_EARLY`) the patch aimed past the file's end and the run ended **VERIFY FAILED** with the uncorrected digits; the packed writer also ignored `t2_defer`. Missed because 10^11 on defaults needed no correction reaching the file and `t_patch` was not in `mnaccept`'s unit step. Reproduction (m, job 21771): 511461828 size 1 `TEST_CORR=14` rc 1; 108388422 at size 2 and 4 FAILED; `t_patch` packed 2258 of 5165 checks failed (ASCII OK).
**The fix.** A correction by dx changes exactly the global limbs [0, m) its carry reaches; a part's limbs are one contiguous byte range. `ecalc/mn_out.c` `packed_patch` validates the part's header, patches the changed limbs at bytes `ECP_HDR_BYTES + 8 (hi - 1 - i)` through `patch_bytes` (checked against the old limbs, whole 4 KiB blocks, O_DIRECT as the writer, fsynced, read back), then adjusts the header's `dres` residues; the packed branch of `out_run_one` uses `d_out + 1 - min(t2_defer, d_out + 1)`. Cost: one limb block plus one header block per affected part (<= 0.003 s). `mnaccept.sh` has a `corr` step and `t_patch` in `unit`; no new switch. Status: current.
Original: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §KP15`.

## HEALTH — node health gate (C4) and per-APU memory guard / high-water (C5)
Status: current; node screen and per-APU guard are off or read-only, digits unchanged. **Results (all m, aac7, 2026-11-01):**
- Gate 1 node with the sampler on: PASS, 1e9 digits identical, VERIFY OK, ecalc total 7.55 s (7.92 s after the review fixes); `device_hwm_apu_gb=22.14` (per APU 22.14, 18.57, 18.61, 21.22). 1e10, 1 node: `device_hwm_apu_gb` 41.48 (per APU 41.09, 40.96, 38.19, 41.48), VERIFY OK, total 24.1 s; the planner said 41.38 GB per APU (mod).
- Refusals: layout P, 1 node, margin 1000 GB: rc 8, each process names its own APU (e.g. `APU 1 has 129.63 GB free ... short by 899.94 GB`); layout D, 2 nodes, margin 1000 GB: rc 143 after rank 0 exited 8 (the launcher killed the other ranks), no hang. Guard (`ECALC_MEM_GUARD_GB`) on a mock tree: rc 9 naming APU 0 below the floor, rc 0 otherwise. `t_nodemem` 5 checks, 0 failed.
- Node screen, 1 node: prints ue 0, ce 0 and the boot time; mock ue=1: rc 0 with `WARN EDAC ue=1`, `--refuse-ue` rc 3; `nodesnap` pair around 1e9 prints the NIC deltas (FEC, pause, stall, drop).
**Caveat.** MemFree counts reclaimable page cache as used, so APU 0 (which starts about 65 GB lower) read 0.66 GB free at 1e10 and its 41.09 GB is a floor-limited figure; fixed by the MemFree + file cache variant (§MEMFIT).
Full table: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §HEALTH`.
**Open issues.** The per-APU figure and guard watch only the APUs the process drives: layout D the NUMA nodes 0-3, layout P the NUMA node of the process's own device; if layout D finds a node count other than 4, or the own device's node has no MemFree file, a `DISABLED` line is printed and the per-APU guard is off. The `device_hwm_apu_gb` line is printed before the guard stops the run (rc 9 from the sampler thread; `mnrun.sh` adds `srun --kill-on-bad-exit=1` for more than one node and for layout P). Re-check after the fixes (1 node, aac7, m): gate PASS, `device_hwm_apu_gb=22.52` at 1e9; mock-low rc 9; a 3-node mock tree prints DISABLED and the run passes. Not run: layout P live, a forced multi-node guard stop, a 2-node NIC delta pair. The start-up `budget_check` in `ecalc.c` has no per-APU form. Sampler HWM is per process: with one process per GPU each PE prints the same physical node figures.

### MEMFIT — per-APU free memory with the file cache, and a start-up per-APU check (user decisions Q140 items 1 and 2)
**Results (all measured unless marked).**
**Results (all m, aac7).** Gate 1 node, sampler on: PASS, 1e9 identical, VERIFY OK, total 8.07 s. 1e10, 1 node (new = MemFree + file cache, old = MemFree only): `device_hwm_apu_gb` 53.43 (APU 0; per APU 53.43, 40.49, 38.63, 40.92), total 19.84 s; start free per APU 112.15, 113.62, 113.66, 113.08 GB (old: 96.50, 112.80, 111.71, 41.01, because APU 3 held 70 GB of file cache); lowest free 58.72-75.03 GB (old: 0.67 on APU 3). Planner (mod): mean 41.38 GB per APU, busiest APU 51.05 GB; measured 53.43 on APU 0 is 2.4 GB above it. GPU memory is not file pages: MemFree fell by 52.2, 39.5, 37.7, 39.4 GB while Active(file) + Inactive(file) did not rise; HBM allocations are on no LRU list. Start-up check on (`ECALC_FIT_APU_CHECK=1`): `fit apu check: ok` (free 112.2-113.7 GB vs 51.05 + margin 5.00 = 56.05 GB), run proceeds; margin 1000 GB, 1e8: rc 8 before any init line (`APU 0 has 112.69 GB free ... short by 906.14 GB`); switch off: no line. `t_nodemem` 9 checks, 0 failed; 57 unit tests, registry and doccheck OK.
Full table: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §MEMFIT`.
**Verdict.** Adopt MemFree + file cache as the per-APU free memory: it is the memory a run can really get (the kernel reclaims file cache on demand; the old figure called a node with 70 GB of reclaimable cache "full") and GPU allocations do not hide in it. The high-water drop is nearly the same either way (differences <= 0.6 GB here) because the run itself takes anon-like HBM; the difference is in the free level, which is what the guard and the start check compare. The start-up check is correct and refuses before GPU work; it stays off until the user adopts it. Caveat: the measured high-water on APU 0 (53.43 GB) exceeded the planner's busiest-APU peak (51.05 GB), so the check's margin should cover at least that gap (the 5 GB default does, 2.4 GB measured); the host-side NUMA-local memory (pinned staging, runtime) is taken from the same HBM and is not in `dev_apu_max`.
**Open issues.** The check uses rank 0's layout figure for every rank; with `XSHMEM_PES_PER_GPU` > 1 several PEs share one APU and the figure is not multiplied (not tested). Margin is the larger of the guard and the device margin. A layout-P process whose device is unknown prints a stderr warning and is not checked.

## MAPBENCH — S1/E1: helper processes that create the GPU allocations, against one process with four threads (2026-11-01)

Program `ecalc/tests/t_mapbench.c` (`make tests/t_mapbench`; standalone). One aac7 node, 4 APUs, ROCm 7.2.4, 2026-11-01. Per chunk: `hipMemCreate`, `hipMemMap`, `hipMemSetAccess` (4 APUs), then a first-touch kernel; size as ecalc: 20 chunks of 2 GiB per APU (160 GiB per node). Wall = go signal to the last first-touch kernel; 3 repetitions per arm and load, interleaved. Arms: **A** one process, 4 threads, global lock (the background mapper of that time); **B** as A without the lock; **C** as B, threads pinned to their APU's NUMA CPUs; **D** 4 helper processes (one per APU, pinned) create with `PosixFileDescriptor` handles and send the fd over a Unix socket, the main process's 4 threads import, map, set access, touch; **E** as D with the main threads pinned. "Load" = 48 threads copying 128 MiB buffers.
Status: **adopted** as `ECALC_MAPD` (helper per APU; on by default at COMM_SIZE > 1 since 2026-11-08, `ecalc/mapd.c`, TASKS row 12).

### Results (measured; full size, median of 3, min-max in brackets, seconds)

| arm | wall, no load | wall, load | speedup vs A (no load / load) |
|---|---|---|---|
| A lock | 15.87 (15.16-15.88) | 15.77 (14.42-16.83) | 1 |
| B no lock | 15.65 (15.19-15.89) | 15.46 (15.23-15.61) | 1.01 / 1.02 |
| C no lock, pinned | 12.29 (12.25-12.37) | 12.41 (12.38-12.42) | 1.29 / 1.27 |
| D helpers | 3.82 (3.81-3.83) | 3.87 (3.82-3.89) | **4.2 / 4.1** |
| E helpers, main pinned | 3.82 (3.76-3.84) | 3.87 (3.82-3.88) | 4.2 / 4.1 |

Quick size (4 x 0.5 GiB, no load): A 1.20, B 1.11, C 0.98, D 0.49, E 0.54 s (A/D = 2.4; the helper's 0.4 s start-up weighs more). Phases (mean over the APUs, full size, no load): A create 3.01, map 0.11, access 0.85; D main import 0.02, map 1.96, access 1.26, touch 0.19; D helper create 2.17 for 20 chunks. The paired hunt of 2026-11-09 (`results/CORRECTNESS.md`) measured the helper on a whole run: default 53.9 s vs 65.6 s with `ECALC_MAPD=0` (2 nodes, 6.441e10 digits, m).
Phase tables: `archive:MEMORY_IO_ARCHIVE_2026-11-10.md §MAPBENCH`.

Load changes nothing beyond the spread (D 3.82 -> 3.87; A 15.87 -> 15.77).

## POOLRACE — free or remap of a dbig pool block while GPU work still uses it (C1POOL)

### 2. Method, results, verdict

Status: **inconclusive, switch kept off.** `DB_FREE_SYNC` (0 default, 1 = the freeing device, 2 = all pool devices) syncs the device before a pool block is freed or remapped.

**Results (measured, aac7, single 1e9 runs, not a timing result).**
- Audit: 21 rows, 0 UNSAFE, 1 UNCLEAR (the `vmm_make_room` unmap), the rest SAFE.
- 2 nodes: `DB_FREE_SYNC=0` PASS 14.62 s; 1 PASS 12.56 s; 2 PASS 12.48 s. 1 node: 0 PASS 8.00 s; 1 PASS 7.66 s. VERIFY OK, digits identical, no segv.
- `mnaccept.sh --only unit,e9` with `DB_FREE_SYNC=1`: rc 0; t_seed, t_dist, t_mn_grid VERIFY OK; e9 identical.
- `gate.sh --build` fails at "python unit tests" (`tests/test_site.py`), also on main: environmental, pre-existing. The build, registry check and doccheck were run by hand: rc 0.
- Pressure: 0 VMM remaps in 24 runs (4e9 and 1.6e10 digits; `ECALC_DEVICE_GB` 250/170, `DM_TIGHT=0`, `BS_ARENA_ROOM=0`, `BS_ARENA_TRIM_GB` 2..12; the trim is clamped to about 0.5 GB). All VERIFY OK. No pressure pair exists.
- ABBA cost of `DB_FREE_SYNC=1` (2 nodes, 1.6e10, item 45_c1pool_abba2, 2026-11-01 04:24 EST, 4 ABBA rounds, totals 22.75-29.17 s; measured): B - A = -0.30 s, 95 % CI [-1.96, +1.37] s (t, 3 df): not resolved, any cost is below about 1.4 s (6 %) at this size. Round 1 holds the slow first run (29.17 s). Its two extra pressure settings again gave 0 VMM remaps (one stopped at the memory guard, rc 9).

Re-gate after review fixes (aac7, commit 7a4ab8fe, 1e9 digits, single runs, not timing): 1 node `DB_FREE_SYNC=0/1/2` PASS (9.18 / 7.86 / 7.96 s); 2 nodes PASS (15.13 / 11.74 / 12.15 s); digits identical, segv 0. The first pressure runs (24 runs, 4e9 and 1.6e10 digits, shrunken `ECALC_DEVICE_GB`, `DM_TIGHT=0`, `BS_ARENA_ROOM=0`, `BS_ARENA_TRIM_GB` 2..12) saw no VMM remap, so the `vmm_make_room` remap path was not exercised then; §STRESS forced it 166,332 times later without an error.

**Verdict.** Inconclusive. No caller proven UNSAFE and no race reproduced; the switch is harmless in the gates (no slowdown seen at 1e9, one run each). The hypothesis is neither confirmed nor excluded at sizes where remaps occur. Recommendation: keep the switch (default 0), do not enable it.

**Open issues.** (1) A natural (unforced) remap under pressure was never seen on aac7. (2) The cost of `DB_FREE_SYNC=1` above 1.4 s (6 %) at 1.6e10 is unresolved. (3) The UNCLEAR unmap row relies on the driver ordering of `hipMemUnmap`.

## STRESS — forced VMM remaps of the dbig block pool, with the CRT/VERIFY checks on (C2)

**Hazard.** The knob can itself create the race it tests for: with `DB_FREE_SYNC=0` it unmaps chunks of a block that was just freed while a stream may still use them; with `DB_FREE_SYNC=1` the sync precedes the free, so work enqueued later is unprotected (the same holds for RDMA-registered memory). A failure with the knob on and `DB_FREE_SYNC=0` is therefore not evidence of a product bug until it is rerun with `DB_FREE_SYNC=1`. The knob is never on by default and never in the bring-up script. Consequence for the clean s0 arms: had any caller freed a block early in the exercised paths, the knob would have turned that into a fault; 0 failures in the s0 arms with the knob on is evidence that no early free happened there (exercised paths and sizes only). Driver `tools/stress/stress.sh`, summary the `summarize` function in `tools/stress/stress.sh`.

**Results (all measured, aac7, 2026-11-01).**

Knob proof (1 node): 1e9 digits, n=1: 28 remaps per APU (about 1000-1800 chunks moved per APU), VERIFY OK, digits identical; 4e9 digits: 112 remaps per run, identical. Remaps per run are about 30 per APU at 1 node whatever the size (the number of pool allocations and frees of the division phase); at 2 nodes with the diagnostics and 16 MiB chunks a 2e10 run does about 11,000 and a 6e10 run about 12,300 (all APUs and processes).

Gates for the knob: 1 node, 1e9 digits, knob off and on: PASS, VERIFY OK, digits identical. 2 nodes, 1e9 digits: off PASS (12.1 s ecalc), on (n=1, 16 MiB chunks) PASS (118 s: the remaps cost 21-45 s per APU there), P24 in use. Unit tests with the knob on, 1 node: `t_dbig 0` VERIFY OK (555 checks), `t_newton 20` VERIFY OK (2752 checks).

Arms (runs / flags / VERIFY failures / digit differences / remaps):

| item | size | nodes | runs done (planned) | clean | CRT flags | P24 flags | VERIFY failures | digit differences | remaps (all forced) |
|---|---|---|---|---|---|---|---|---|---|
| 1-node, DB_FREE_SYNC=0, 8 arms | 2e10 | 1 | 14 (24) | 14 | 0 | 0 | 0 | 0 of 14 compared | 744 |
| 1-node, DB_FREE_SYNC=1, 8 arms | 2e10 | 1 | 15 (24) | 15 | 0 | 0 | 0 | 0 of 15 | 868 |
| 2-node, 16 arms | 2e10 | 2 | 14 (32) | 14 | 0 | 0 | 0 | 0 of 14 | 66,096 |
| 2-node, 16 arms | 6e10 | 2 | 16 (16) | 16 | 0 | 0 | 0 | not compared (no digit file) | 98,624 |
| total | | | 59 | 59 | 0 | 0 | 0 | 0 | 166,332 |

Why fewer runs than planned: every item had a start-time cap (60 min at 1 node, 85 min at 2 nodes) and runs with the knob on are slow (1 node about 105 s off vs 120 s on; 2 nodes at 2e10 about 75 s vs 410-420 s, 5.5 times; at 6e10 about 75 s vs 470-520 s). No run was stopped by an anomaly.

NIC delta (2 nodes, 2e10, arm k0s0d0r0, `tools/stress/nicdiff.sh` on `tools/nodesnap.sh` snapshots; m): tx and rx 517.8 GB per node each, error-like counts 334 per node, paused counts 911,126 per node; top FEC + drop node `fec=752 pause=10665 stall=6.97e9 drop=8`. Real traffic with small FEC/drop counts; no run was affected.

**Verdict.** Flags not reproduced; a software cause is not shown at this scale. "0 flags" covers the exercised paths and sizes only: the harness made the remap path run 166,332 times in 59 runs (about 30 forced remaps per APU per 1-node run, about 1,500 per APU per 2-node run) with `DB_FREE_SYNC` 0/1, the P24/sequence/round diagnostics and tiny exchange rounds, with 0 CRT flags, 0 P24_CHECK flags, 0 VERIFY failures and 0 digit differences in the 43 runs whose digits were compared (the 16 runs at 6e10 are VERIFY-checked only). A clean knob-on `DB_FREE_SYNC=0` arm is the stronger result; a failure there would first need the `DB_FREE_SYNC=1` rerun (the knob can create the race it tests). Limits: the target's sizes (3.7e13 digits, 576 nodes) are 600 times larger and the pool's remaps there are in the real memory-pressure regime; the forced remaps move only free chunks and the chunks are 16 MiB, not 2 GiB; one repetition per 2-node arm is too few to see a rare event (a 0.1-per-run failure rate would show in about 22 runs with 90 % chance; assumed).

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
