# CACHE — the multi-node transform cache

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: **(m)** measured, **(mod)** modelled, **(a)** assumed, **(f)** fitted. Times Eastern.

| item | title |
|---|---|
| CX15 | what the multi-node transform cache is worth (hit model fit, aac6 end-to-end, 5.1e13 estimate, proposals) |
| TC15 | the cache's allocation (what the code takes, the memory it does not count, `RNS_DIST_CACHE_FIT`) |
| PC15 | a partial transform cache inside 480 GB (`RNS_DIST_CACHE_PARTIAL`) |

## Topic summary

The multi-node (mn) tier can keep an operand piece's forward transform in a "slot" across the pieces of a grid product, so a later piece that reuses that operand skips its gather and forward transform. This topic is about what that is worth and how it may use memory.

- **Per hit** (all measured on aac6 loopback, node s24-16): a hit saves one of the three transforms per prime (`ntt` 0.859 -> 0.574 s at 4 node-processes) and about 45 % of the operand's redistribution: 24-33 % of the piece (m) against 31-32 % modelled; fitted `CACHE_HIT_F = 0.96`. A partial hit (k of np primes in the slot) saves 7 % (1 prime) / 14 % (2 primes) of the piece, a full one 31 % (m); `CACHE_PART_F = 1.08` (f). A miss into a slot costs nothing measurable. Digits are identical in every run of every test.
- **The old default harmed**: `RNS_DIST_CACHE_MN=2` took 2 slots sized 2^(dist_logn_max-2) per prime whatever `POOL_LOG` (16 GiB per APU at four primes), allocated inside the first grid product (8.9-16.8 s) and invisible to the memory report, `ECALC_BUDGET_CHECK` and `mem_model.py`; at 10^10 on 2 processes it made the run +37.5 % slower (memory pressure slows the host-staged exchanges 12-19x), at 4 processes it ran out of memory, and at the target it would take 137.4 GB per node that 480 GB does not have (node 592.8 GB mod). The standing 5.1e13 time (346.6 s mod) credited that cache (+87.5 s) with no memory behind it.
- **Current state (ecalc/README.md)**: `RNS_DIST_CACHE_MN` slots are sized to the cap and budget-bounded (the `FIT` rule of §TC15, built in since 2026-10-12; at the target the budget room gives 0 hipMalloc'd slots); `RNS_DIST_CACHE_PARTIAL` is the **built-in default 1** since 2026-10-12 (was on both launch lines): per grid product the slots are taken from the block pool (inside the arena, no byte added to the node), as many primes as the pool's free extents allow, P24 products cached too, and the loop along the longer axis. Its modelled gain at the target is -14.1 s (about 5 %: 288.2 -> 274.0 s without the write at 5.1e13 with `MN_P24=2 NEWTON_DKM=1`), only about -6 s if `BS_ARENA_ROOM=0`.
- **End to end (m, aac6, 10^10 with target-like grids)**: cap-sized slots -14.4 % (1 slot) / -20.4 % (2 slots) at 2 processes, -10.8 % / -17.7 % at 4; the pool-rule partial cache -21.8 % (18-digit) and -16.3 % (P24 + DKM) at 2 processes. These are full slots on a small run; at the target the pool holds 1-2 primes.
- **Caveat that stays open**: the fits are on aac6's loopback (pieces about 90-99 % exchange); at the target the saving is mostly the local forward transform (about 80 % mod, single-node measurement `archive:results_merged_2026-10-24/GRID_GROUPS.md §G`: 1.11 -> 0.72 s at 2^31). The target rooms (tree 36.5 GB free in the arena = 2 primes, division 1 prime) are modelled, not measured.
- **Superseded**: CX15's full-slot and k-prime proposals (nothing but the budget-fitting ones was buildable; PC15 built the pool form); TC15's recommendation to assume no mn cache (434.1 s) is superseded by the PC15 pool rule (the estimate then is the 274-289 s class with P24 + DKM); the old estimates 346.6 / 353.3 s.

## CX15 — what the multi-node transform cache is worth

Agent CX; branch `p15-CX` from `int15f` (TC's `RNS_DIST_CACHE_FIT` on main B1 + Batch 2). aac6 node s24-16 only; raw logs `archive:results-raw/CX15/`.

**Summary of results.**
- Per hit the model is about right (above): cx_grid on the target's 9 grid shapes and ecalc 10^10 end to end give measured / modelled **1.043, 0.976** (cx_grid, 2 / 4 procs), **0.965, 0.840** (ecalc, 2 / 4 procs, 4 runs each), mean 0.956 -> `CACHE_HIT_F = 0.96` (f).
- Hits the code takes at the target's grids (the new `mn_model.cache_pieces` simulates the code exactly on all 9 shapes at 2 and 4 procs): **162 at 2 slots** (the old term credited 168: low-cut grids lose A's piece 0) and **107 at 1 slot** (old term: none).
- At 5.1e13 (mod): 2 slots -80.8 s (old term -87.5), 1 slot -53.8 s (old 0), but neither fits 480 GB (one full slot 68.7 GB per node; rooms 44.5 GB in the tree levels including the arena's 20.0 GB slack, 34.2 GB in the division). **With 0 slots (what the budget allows and FIT gives): 434.1 s (7.23 min) without the write, 453.3 s with it at 0.6 GB/s at `ECALC_NP=4`; 406.8 / 426.0 s at `ECALC_NP=auto`** (mod).
- What fits (mod): slots holding only some of a piece's primes. 1-prime slot (17.2 GB per node) fits everywhere: -13.0 s, -16.4 s with the loop along the longer axis; a 2-prime slot in the tree (arena slack + room, 34.4 of 44.5 GB) with a 1-prime slot in the division: -20.6 s, **-26.7 s with the longer axis** (407.4 s). None built by CX (PC15 built the pool form).

**TC's contradiction resolved.** TC's "wall" included the digit comparison reading the 10 GB reference over NFS (about 107 MB/s; 93 s cold, 13 s warm); the processes' own walls were 115 s (off) and 140 s (on). The cache's +25.8 s came from allocating the default oversized slots (2 x 12 GiB per APU per process, 4x what POOL_LOG 29 needs; 8.9 s and 16.8 s on the two processes) inside the first grid product, plus memory pressure afterwards (shifts 60-96 s instead of 5 with default 16 GiB slots at 2 procs). FIT's cap-sized slots make the same run 20 % faster.

**cx_grid, the target's 9 shapes at cap 2^27 (m, job 21701).** Shares of C identical over slot counts and repetitions. Product totals, node 0 (sum of all 9; mean of 2): 2 procs 231.3 / 186.8 / 166.5 s at 0 / 1 / 2 slots (**-19.2 / -28.0 %**, 54 / 73 hits); 4 procs 90.9 / 74.3 / 67.8 s (**-18.3 / -25.4 %**). Per shape the 2-slot saving ranges -15 % (recip 1 x 2 low) to -32 % (A_h mu 5 x 6); recip 1 x 2 low with 1 slot gains nothing (a hit needs >= 2 slots: nA = min(ka, nf-1) = 0). Hits equal `cache_pieces` on every shape. Per hit piece: wall 2.64 -> 1.78 s at 2 procs (ntt 2.42 -> 1.61), 1.03 -> 0.72 s at 4 procs; cached miss = no change.
DIST_STATS pass (m, job 21733): all 9 products 89.4 -> 67.3 s (-24.8 %); both exchange and transform drop by a third, but on aac6 the transforms are about 99 % exchange so the local kernels' third is 3 ms per piece: the target's local share rests on the `T_PIECE_31_BHIT` single-node measurement.

**ecalc 10^10 end to end (m).** 2 procs, POOL_LOG 27 (job 21703): c0 (no cache) 276.6 s mean; f1 (FIT, 1 slot) 236.7 s (**-14.4 %**, 34 hits); f2 (2 slots) 220.1 s (**-20.4 %**, 45 hits); c2 (default 16 GiB slots) 380.3 s (**+37.5 %**; shifts 60.0 / 95.7 s against 5 s; allocation 10.4-16.9 s). 4 procs, POOL_LOG 26 (job 21725): c0 146.5 s; f1 130.8 s (**-10.8 %**, 38 hits); f2 120.6 s (**-17.7 %**, 51 hits); c2 not run (4 x 128 GiB exceeds the node). All 14 runs digit-identical. Hit pieces against themselves: 29.2-32.9 % saved vs 32.2 % modelled at 2 procs, 24.1-28.6 % vs 31.8 % at 4 procs.

**Model changes.** `MN_MODEL_CACHE=code` is now the default: hits from `cache_pieces` at `CACHE_MN_SLOTS` slots, each saving `CACHE_HIT_F` of piece_cost's fwd 2 - fwd 1 (at the target a hit saves 0.40-0.58 s of a 1.25-1.81 s piece: about 80 % local forward transform, 20 % exchanges, 3 % redistribution). Proposal knobs `CACHE_PRIMES`, `CACHE_LOOP='long'`, `CACHE_SLOTS_PHASE`; `mn_model.py --cache-proposals`.

**Estimate table (5.1e13, mod, `mn_model.py estimate --target`).**

| cache | NP | without the write | with the write @0.6 | distributed levels / recip / div |
|---|---|---|---|---|
| old term, 2 slots (the standing number) | 4 | 346.6 s | 377.8 s | 150.9 / 44.0 / 50.3 |
| code hits x 0.96, 2 slots (137.4 GB: does not fit) | 4 | 353.3 s | 382.7 s | 153.1 / 45.5 / 53.3 |
| code hits x 0.96, 1 slot (68.7 GB: does not fit) | 4 | 380.3 s | 407.9 s | 174.4 / 48.1 / 56.4 |
| **0 slots** | 4 | **434.1 s** | **453.3 s** | 205.6 / 56.0 / 71.1 |
| **0 slots** | auto | **406.8 s** | **426.0 s** | 188.6 / 55.7 / 71.1 |

Proposals (mod, NP=4, 0 slots = 434.1 s without / 453.3 with the write): 1 slot of 3 primes -39.1 s (does not fit); 2 primes both phases -26.1 s (division short by 0.2 GB); 1 prime -13.0 s (-16.4 s longer axis); tree 2 primes + division 1 prime -20.6 s (-26.7 s longer axis, **407.4 s without the write, fits**); tree 2 slots of 1 prime + division 1 -18.2 s. Allocation cost about 1.6 s once for 17.2 GB per node (mod, 0.055 s/GB). Needs: k-prime slots (hit skips those primes' gathers and transforms, no redistribution saved), the loop along the longer axis (kb > ka at every target tree grid: ka (kb - 1) instead of kb (ka - 1) reuses; 2 x 6 with 1 slot: 10 hits not 6; digits independent of order), a slot from the arena's slack (tree only).

**Gates (all at 75cdb2f).** mnaccept `unit,e9,mn` 15 PASS, 0 FAIL (1307 s, job 21733); 4e10 at size 1 identical, `total` 61.69 s (m).

**Open issues.** Fits are on aac6's loopback; `mn_model.py estimate --target` still priced 2 slots by default (353.3 s) while the budget allows 0 (434.1 s); the default cache harmful wherever slots are oversized or the node shared (fixed by FIT; adoption was the user's); proposals modelled only.

## TC15 — the multi-node transform cache's allocation (internal target task list T7)

Agent TC; branch `p15-TC` from `int15e` 6990778; aac6 s24-16; jobs 21669 (m1) and 21687 (m2), 2026-09-28.

**What the code allocated (read from `rns_dist.c`).** At the first product wanting the cache, once per process, n slots of one `hipMalloc` per APU of `rns_pool0_np()` x 2^(`dist_logn_max()`-2) x 8 B (16 GiB at four primes, 12 GiB at three) whatever `POOL_LOG`; n = `RNS_DIST_CACHE_MN` (2) cut to what `hipMemGetInfo` free minus `RNS_DIST_CACHE_MARGIN_GB` (24) allows and to the group's minimum. Requested only for grid products (`nc > cap`); oversized by 2^(31-pool_log) when `POOL_LOG` < 31 (64x at 25), exact at the target. Freed only by `rns_dist_cache_release()` (single-node dm, and the mn division's end): an allocation at tree level 1 lives through the reciprocal and division. Not counted anywhere: memory report, `ECALC_BUDGET_CHECK`, `BS_LAYOUT_ONLY` / `mem_model.py`; `mn_model.py` counted it only as time (`CACHE_MN_SLOTS = 2`).

**At the target (mod, `MN_PLAN_ONLY`).** Every tree level from 1 (group 2) to 8 (576) forms grid products at the cap, so the cache is requested at tree level 1 and held to the division's end; slot 4 primes x 2^29 x 8 = 16 GiB per APU (17.18 GB); 2 slots = 137.4 GB per node. Against the 455.4 GB node figure (which excludes the cache): 592.8 GB with 2 slots, 524.1 GB with 1 (4.74e13: 576.6 / 507.9). What the cache is worth (mod, without the write): 5.1e13 346.6 s (2 slots as modelled) against **434.1 s** with none (+87.5 s, +25 %; @0.6 GB/s 377.8 / 453.3 s); 4.74e13 322.0 vs 401.2 s; 4.25e13 257.4 vs 313.8 s. So the standing 5.1e13 time assumed 137.4 GB per node that the node figure does not hold.

**Measured (m, aac6, three primes; the default POOL_LOG=31 forms no mn grid product at 10^9 / 10^10 so pools were lowered).**

| run (procs, POOL_LOG) | cache | slots per APU (alloc time) | `total` s | dm s | hits / lookups |
|---|---|---|---|---|---|
| 10^10, 2, 29 | off | - | 113.78 | 83.56 | - |
| 10^10, 2, 29 | default | 2 x 12.0 GiB (8.89 s; 16.84 s on the second process) | 139.58 | 109.81 | 3 / 12 |
| 10^10, 2, 29 | FIT | 2 x 3.0 GiB (2.04 s) | 111.67 | 82.29 | 3 / 12 |
| 10^10, 4, 28 | off | - | 82.41 | 48.95 | - |
| 10^10, 4, 28 | default | 2 x 12.0 GiB (12.03 s) | rc 6: HIP out of memory at `rns_dist.c:255` | - | - |
| 10^10, 4, 28 | FIT | 1 x 1.5 GiB (room 18.8 - 8.0 GB -> 1) | 76.37 | 48.31 | 0 / 6 |

Use `total`, not the script wall (the wall included the NFS reference read). Default cache at 2 procs = +25.8 s (+23 %): allocation inside the first division product (A mu + shift 48.86 s vs 21.08 s), division products slower (58.0 vs 37.7 s; shifts 7.79 vs 4.58 s); at 4 procs 96 GiB per APU on top of the pools ran out of memory (G5's failure reproduced). Sized (FIT), 3 hits in 12 lookups saved 2.1 s of `total` (division products 37.7 -> 32.8 s); at 4 procs 1 slot gave 0 hits (a hit needs >= 2 slots). Not the cache: rc 6 at POOL_LOG 24-25 (plane pool would grow, pre-existing limit, `RNS_POOL_GROW=1` allows), rc 137 at 4 procs POOL_LOG 31 (4 x 195.8 GB layout cannot fit a node). `hipMemGetInfo` is node-wide per APU (137.4 GB total) and **does not see the arena's VMM region bytes** (driver figure = planes + pool + tables: at 1e11, 68.9 GB against 108.1 in use); at the 9.17e10 rehearsal layout (device 379.7 GB) it shows 56-71 GB used per APU, leaving 42-57 GB after the 24 GB margin = room for two 17.2 GB slots, so the default would take both and the node would reach about 590 GB (inferred; the target's region bytes at tree level 1 were not measured).

**The fix `RNS_DIST_CACHE_FIT` (built).** Memory source: the budget room is 480 - 455.4 = 24.6 GB per node, below one slot (68.7 GB); arena slack in the tree 284.1 - 264.2 = 19.9 GB, none in the division: no source inside 480 GB holds even one slot (4.74e13: room 40.8 GB). The switch: **slot sized to the cap** (c = min(`dist_logn_max`, `POOL_LOG`); a product whose q exceeds the slot is not cached; unchanged at the target, 64x smaller at POOL_LOG 25); **bounded by the budget** (`budget_check` gives each rank its room; slots <= (room - `RNS_DIST_CACHE_FIT_RESERVE_GB` / ranks) / (4 x slot); reserve 32 GB per node covers the init peak's omissions; **0 slots at 5.1e13 and 4.74e13**); **shared free memory** divided by the node's processes; **reported** (memory report `other`, `transform cache:` line, `plan cache` line in `MN_PLAN_ONLY` / `BS_LAYOUT_ONLY`; `mem_model.py --cache`: 5.1e13 node 455.4 / 592.9 (2 slots) / 524.2 (1 slot) / FIT 455.4). Gates (m, job 21687): FIT `mnaccept unit,e9,mn` 15 PASS, 0 FAIL (1303 s; `t_mn_grid` at 2 procs 200 checks); 10^10 at 2 and 4 procs identical; 4e10 at size 1 identical, `total` 61.65 s (switch off: reporting only). Not run: `t_mn_grid` at 3, 4, 6 with FIT.

**Reconciliation and recommendation.** The +87.5 s "no cache" penalty at the target is unmeasured (aac6 has only 1 x 2 grids at 2^30; there a sized cache gains about 5 s on the division products and 2 s on `total`; the direction agrees with the model). The data does settle that the target cannot hold both the cache and its 455.4 GB node. Recommendations: (1) assume no mn cache (434.1 s / 453.3 s at 0.6 GB/s; 401.2 s at 4.74e13) and label the 87.5 s as an unmeasured model credit; (2) put `RNS_DIST_CACHE_FIT=1` (or `RNS_DIST_CACHE_MN=0`) on the target launch line (done: built in 2026-10-12); (3) a per-prime cache (a quarter of a slot fits the 24.6 GB room) is a design question (answered by PC15). Open: the target's region bytes at tree level 1.


Sections moved to `archive:results_merged_2026-10-24/CACHE.md` (2026-10-24 cleanup): PC15.