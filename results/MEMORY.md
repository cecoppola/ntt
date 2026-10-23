# MEMORY — device and host memory

Scope: device and host memory: accounting, the arena and its sizing, the reserved tail, init floor, early free, the memory ceiling. | Owner of the items below (old IDs resolve through `results/INDEX.md`). Each item section is a condensed summary of the former `results/<ID>.md`; full originals are in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`. Cite items as `results/MEMORY.md §<ID>`.

## Topic summary

What this is: how ecalc maps and sizes device (HBM, 4 APUs per MI300A node) and host memory, and what the per-node ceiling is. Numbers are measured on aac6 (one node, 502 GB total) unless labelled modelled; the 576-node figures are modelled.

Current state (cross-checked with `ecalc/README.md`; where an item below says "off by default" the README row governs):
- **Layout.** One arena per APU holds both bs parities plus the dm extra, laid out by `region_need` / `dm_layout` / `tree_need_dev` (§A-mem, §M11); the block pool keeps a reserved tail for the reciprocal's largest block, so there is **zero in-phase hipMalloc** at every size run (§M11 v4: 10¹¹ on one node, device 431.2 GB, node peak 445 of 502 GB). `ECALC_DM_POOL` was deleted (§I item 5). The arena is a VMM range in whole 2 GiB chunks (`DB_POOL_VMM`, default on); `DM_TIGHT` defaults on with it (§L114, §T114 context); `BS_ARENA_ROOM` default 0.16 and `BS_SEED_FILL` default 128 (§AS15; user decisions of 2026-09-27/28); `BS_SEED_PULL` default 1 since 2026-10-12 (§K8b, README) although the K8b ABBA never completed in this file.
- **Host.** X never lives on the host and the seeds stream straight into the regions: host HWM at 4 × 10¹⁰ fell 48.8 → 11.7 GB (§H); at 10¹¹ about 14 GB.
- **Init floor.** Init time is 0.06–0.075 s/GB × bytes mapped, a kernel page-clearing cost on one lock that no allocation form, thread count or reordering beats (§I, §K8, §K8b, §P3, §MAP15). Seeds overlapped with the mapping (§I item 3); 3·2ᵏ planes on below 5 × 10¹⁰ digits at one node (§I item 4; the 3·2ᵏ caps were later removed by SIMP, see `results/CODE_REDUCTION.md §SIMP` and the README row of `ECALC_PLANE_CAP`).
- **Ceilings (one node).** Three primes (default `ECALC_NP=3`): real edge 1.44 / 1.30 / 1.14 × 10¹¹ at caps 2³⁰ / 2³¹ / 3·2³⁰ (device + host HWM ≲ 524 GB runs, ≈ 529 GB is OOM-killed; §P13b). `mem_model.py` (exact to the byte against the C layout, §M13, §AS15, §DL15) is the tool; its 502/480 GB budget is conservative.
- **576-node ceiling (modelled).** 4.0–5.3 × 10¹³ digits depending on switches (§M13 headline), 5.396 × 10¹³ with the room kept and 5.532 × 10¹³ at most (§DL15); the room-0 ceilings of earlier items are one chunk too high. §M11's older 1.1–1.25 × 10¹³ (spill buffers growing with g) is superseded by §M13/§DL15.
- **Bug fixes in sizing:** §R16 `BS_POOL_RULE` (default 1) fixed the 10¹¹ on 4 nodes abort; §PS15 pool 1 = 4 q at four primes (`RNS_POOL1_4Q`, since removed from the code) fixed the rc 6 at 9.17 × 10¹⁰ with `ECALC_NP=4`.
- **Dropped / stopped:** `DB_POOL_VMM_STREAM` (§MAP15: no gain, two node reboots, ACK-guarded; do not adopt), `RNS_HOOK_CONCUR` and `ECALC_VMM_FILL` (§K8: slower), `DM_TAIL_DEAD=2` / E5 spill (§L114: ≈ 0 over `DM_TIGHT`; `DM_TAIL_DEAD` removed 2026-10-20), host-side allocation forms (§I).
- **Superseded within this file:** §M B4/A3 (C3 `ECALC_DM_POOL`) by §M11 tail, then §I item 5; §A-mem and §M open issues by §M B5/B6 and §M11; §M11's tail v1–v3 by v4; §M11 g > 1 estimate by §M13/§DL15; §L114 first and second forms by the third.
- Live open items: spill buffers and top-level plane growth at g = 576 (§M11, §M13), host term at size > 1 on the target (§M13 issue 1), the mn division peak at 576 counted not measured (§DL15), intermittent leaf fault seen once (§T114), the §L114 batch 6 gate and the §K8b re-armed ABBA not recorded here.

| item | title |
|---|---|
| M | device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10) |
| H | host memory: X never on the host (B1), the seeds streamed to the regions (B2) |
| A-mem | M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9) |
| M11 | the arena's reserved tail, the tree's slabs in the arena, the per-node model, 10¹¹ on one node |
| M13 | memory truth: one ceiling, the two O(share) terms bounded (Phase 13a) |
| P13b | sizing follows the prime count; the plane cap switch; the one-node ceiling at three primes |
| R16 | the region-pool sizing abort at 10¹¹ digits on 4 nodes |
| AS15 | arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps) |
| MAP15 | the arena's mapping, the seeds and level 1 (stopped) |
| PS15 | plane pool 1 at four primes (the rc 6 at 9.17 × 10¹⁰) |
| DL15 | the arena sized to DKM's division, and the room check on the model's node |
| I | the initialisation floor: every allocation form measured |
| K8 | init and the seed wait at one node (diagnosis) |
| K8b | seed transfers without the HIP runtime inside init's mapping window |
| P3 | shorten the start-up mapping phase (stopped, no change) |
| T114 | E10a, the early free in the multi-node tree (`MN_TREE_EARLY_FREE`) |
| L114 | E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`) |

## §M — device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10, PLAN §21)

Branch `m10` (from main @ 4aca721); continues §A-mem (its open issues 2 and 3 are B5 and B6). Files: `mem.c/.h`, the layout and pool functions of `binsplit.c`, the block pool in `dbig.c`, pool lines of `ecalc.c`.

**B5 — non-zero ranks' region arenas.** `rns_shutdown_hook = binsplit_release_arenas` is now set where the arenas are allocated (`arena_get`), so non-zero ranks of the `MN_DM=host` / M2 paths release them (before: 22 GB per process stayed mapped until exit). Every rank's `[released]` row shows `regions 0.0`.

**B6 — at size > 1 the leaf's regions go to the block pool before the tree.** When the leaf tree ends on the batch tier, P_r and Q_r used to be copied to the host and back (`region_copy`, `db_from_bi`) while the regions sat idle and `mn_tree` `hipMalloc`ed 11–44 GB per process. Now `binsplit_e` donates the idle parity and copies P_r, Q_r into blocks (`db_copy` into `bs_Pd`, `bs_Qd`); size 1 is unchanged. Added `mem_report("tree")` in the distributed-dm path.

| run (one node) | digits | tree row: donated (borrowed) / hipMalloc per process (m) | before |
|---|---|---|---|
| 10⁸ size 2, PL 27 | identical | 0.2 / 0.1 GB | 11 GB hipMalloc |
| 10⁹ size 2, PL 29 | identical | 2.2 / 0.5 | |
| 10¹⁰ size 4, PL 29 | identical (146 s) | 11.3 / 4.9–5.4 | 11–16.5 |

At 10¹⁰ / 4 the tree still maps 5 GB per process (peak live 11.0 GB exceeds the donated 11.3 GB of which P_r, Q_r hold 2.2): the genuine excess of the slabs. All VERIFY OK, parts `cmp`-identical.

**B4 / A3 — the dm phase's block pool, and what the in-phase growth is.** New accounting: every in-phase pool `hipMalloc` is logged (`DB_POOL_VERBOSE=1` / `RNS_VERBOSE`), a `division` row in `mem_report`, and `mem_oom` prints the request plus the full accounting on a failed device allocation. Finding (m): the growth is not fragmentation into many blocks but exactly one block per APU, t1 (the product scratch of `newton_db_recip`, `max(n_Q+k, 2k)+8` limbs; a quarter = 8.9 GB at 4 × 10¹⁰, 15.6 GB at 7 × 10¹⁰), which needs contiguous room next to Q, S, r, r2 and finds the free bytes only in 2–3 holes.

| (m) | 4 × 10¹⁰ | 7 × 10¹⁰ |
|---|---|---|
| regions donated | 95.7 GB | 169.6 GB |
| peak live in dm | 121.5 GB (6.8 n_Q) | 202.2 GB (6.5 n_Q) |
| mapped inside the phase | 45.3 GB | 62.2 GB (all in the reciprocal) |

Three ways to place t1 at 7 × 10¹⁰ (job 20773, s24-26, reference evicted; m, seconds): main 163.8 wall; default (`ECALC_DM_POOL=0`) 159.7; `ECALC_DM_POOL_K=6.6` (arena sized for dm at init, 205.3 GB mapped) 158.6, t1 still did not fit the holes; **`ECALC_DM_POOL=1`** (C3, pool grown before the reciprocal, chunk must hold the largest block via `db_pool_largest_free`) **157.2** (recip 34.2, dm 71.3). All VERIFY OK. `DM_POOL=1` saves 1.7 s of the reciprocal; the same 62 GB are mapped either way (0.06 s/GB); the reciprocal's 34 s are its 93 distributed products (ntt 25 s), not memory, so A3's expected −10…−15 s is not there. At 4 × 10¹⁰: 85.9 vs 85.3 s (noise). `ECALC_DM_POOL` stayed off by default here (later superseded: see §I item 5, deleted). "Second parity's regions earlier" and "planes shrunk after bs": nothing to gain (the 7 × 10¹⁰ top levels fit the donated regions; dm products use all of pool 0 and 3 q + 16 of pool 1).

4 × 10¹⁰ gate (size 1, m): batch 1/2/3 builds with DM_POOL 0/1 all in 85.3–90.4 s wall, digits identical to `results/e_4e10.out`; all within 86.4 ± 1.3 s (RESULTS §74); host HWM 48.8 GB. Accounting at 4 × 10¹⁰ (m, GB): init device 216.6 (planes 120.3, regions 95.7); recip 261.9; pool peak 121.5; host HWM 48.8. At 7 × 10¹⁰: device peaks 352.7 (recip) + host 6.6 ≈ 359 GB; bs 290.5 + 73.2 = 364 GB; node has 502 GB.

**The 8 × 10¹⁰ attempt.** Model (a): init ≈ 395 GB, recip ≈ 390 + 7, division + X ≈ 435 GB < 502, so memory was not the stop.
1. First attempt (`ECALC_DM_POOL_K=6.6`): abort `bs: seed region larger than the staging buffer` — the seed staging was capped at 2^pool_log limbs (16 GiB per APU) but region 0's seed stage is 19.5 GB. One condition removed in the staging line of `ecalc.c`.
2. Second attempt (job 20790, s24-30, `ECALC_DM_POOL=1`): **8 × 10¹⁰ digits in 210.4 s (m), VERIFY OK (T1, T2), peak host 90.0 GB, peak device 386.6 GB**; init 25.1, bs 86.1, dm 99.1 (recip 49.6, division 49.5); one fallback of 17.8 GB on APU3 (arena 0.8 GB smaller); the 80 GB file written in 169 s in the hidden writer thread. No reference file at this size (first 70 × 10⁹ digits are §74's). Node peak 386.6 + 6.7 = 393 GB of 502, i.e. ≈ 110 GB spare; per-APU driver used bytes agree to 0.3 GB. 2.63 ns/digit (7 × 10¹⁰: 2.25; 4 × 10¹⁰: 2.13).
Scaling per 10⁹ digits (m): regions 2.2 GB, dm pool 2.9 GB, staging 1.0 GB, X 0.44 GB; next memory line ≈ 10¹¹ (init ≈ 443 GB, recip ≈ 414 GB, no slack for holes) (a). What stopped 8 × 10¹⁰ before was a staging cap.

Open issues (as left; later items resolve 2–4): (1) `ECALC_DM_POOL` default is the user's call; (2) t1's quarter as contiguous arena tail removes the 62 GB but maps the same bytes at init, memory-only gain (done in §M11); (3) the size-4 tree still maps ≈ 5 GB per process (≈ 0.3 s; §M11 item 2); (4) seed staging 4 × 19.5 GB at 8 × 10¹⁰ (§H B2 brings it to ≈ 20 GB). Tests: 10⁸–10¹⁰ at sizes 1/2/4 identical to references, 4 × 10¹⁰ ×6, 7 × 10¹⁰ ×3, 8 × 10¹⁰ ×1 (jobs 20764, 20769, 20773, 20790).

## §H — host memory: X never on the host (B1), the seeds streamed to the regions (B2) (Phase 10, PLAN §21)

Branch `h10` (from main @ 38ed61b). Owned: `mn_out.c/.h`, output tail of `ecalc.c`, seed code of `binsplit.c`, staging size in `rns_init`'s request.

**B1 — X never on the host.** Size 1: `newton_db_divmod_shifted` gains `newton_db_x_dev` (X stays a device number; ±1 corrections applied in place by `db_share_add_val`; saves the 17.8 GB `db_to_bi` copy, ≈ 0.5 s at 4 × 10¹⁰). The background output thread computes X's residues by the device kernel (`db_mod_qs`, 0.55 s vs 2.8 s of host Horner at 4 × 10¹⁰) and the chunked writer reads `src->dev` (256 MB chunks by `hipMemcpyAsync` on a non-blocking stream per APU). The block pool is released after the output stage. A correction (0 in every run so far) makes the main thread recompute residues and redo the digit string. `ECALC_OVERLAP=0` keeps the host flow. Size > 1: `out_stage` reads this node's share of A-div's `mdb` X in place; the residues of P, Q, R are every node's own (`mdb_mod_qs`), the broadcast is dropped; `db_mod_qs` takes a mutex (shared static scratch).

**B2 — seeds streamed into the regions** (`seeds_stream`, `binsplit.c`). Before: region-sized pinned staging per APU (4 × 10.7 GB at 4 × 10¹⁰, the bs host peak). Now `binsplit_seeds_begin` (the `rns_after_staging_hook`) starts the seed thread and then allocates the region pools (`binsplit_pregrow`, once per run); the thread computes spans in `BS_SEED_CHUNK_MB` (2048) chunks with all threads, into two 2 GiB pinned buffers until the regions exist, then **stores straight into the region** (`BS_SEED_DIRECT=1`, default; CPU stores to device memory ≈ 8 GB/s vs ≈ 5 GB/s of seeds, RESULTS §56). Buffered chunks go by `hipMemcpyAsync` (`mem_dev_copy_async`). `BS_SEED_DIRECT=0` (every chunk by DMA) is slower (a HIP call from the seed thread blocks behind the main thread's `hipMalloc`, 0.4 s per call). `binsplit_e` joins the thread and checks the pools. `rns_init`'s staging is 1 GiB per APU in the decimal device flow (`ECALC_STAGING=2` restores seed-sized, `=0` the paper's 64 GiB).

Tests (aac6, one node, jobs 20763, 20768, 20770, 20781, 20787): all VERIFY OK and `cmp`-identical for 10⁶–10⁹ size 1, `ECALC_OVERLAP=0`, small chunk sizes, `BS_DEVICE_POOLS=0`, `LIMB_BASE=2`, sizes 2/3/4, `MN_DM=host`, `MN_COMBINE=host`, checkpoint + restart, 4 × 10¹⁰ (m).

4 × 10¹⁰, size 1 (m):

| | merged main (RESULTS §74, 5 runs) | h10 final (2 runs) |
|---|---:|---:|
| init | 16.1 ± 0.9 | 14.3 / 14.4 |
| bs | 37.3 | 37.0 / 37.1 |
| dm (recip / division) | 32.9 (16.2 / 16.7) | 31.3 (15.7 / 15.6) |
| **wall** | **86.4 ± 1.3 s** | **82.75 / 82.72 s** |
| **peak host (VmHWM)** | **48.8 GB** | **11.7 GB** |

Host at init 45.7 → 11.5 GB (staging 42.9 → 8.6), bs 47.4 → 9.0, dm 70.8 → 7.5 (X 17.8 → 0, digits 40 → 0 by A-out). `BS_SEED_DIRECT=0` 89.0 s / 12.6 GB; the first B2 form (DMA thread, null stream) 89.2 / 88.4 s. What remains on the host: ROCm runtime + program ≈ 7 GB, checkpoint staging 4.3 GB, writer chunks. Device side unchanged.

Open issues: pinned staging of `rns_init` could go to 0 in the decimal device flow (another 4.3 GB; T owns the checkpoint code); the stand-in scatter and residue broadcast remain for the host flows (PLAN §22 E1); a ±1 correction at size 1 redoes the whole digit string (never observed); with the overlap on, the driver's "init: region pools" line reports 0.

## §A-mem — M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9, PLAN §19)

Branch `amem` (from main @ a75474d).

**Cost of mapping device memory (job 20708, s24-16, m).** `hipMalloc` costs 0.054–0.075 s/GB with no per-call overhead worth chasing (2 × 14 GB 2.11 s, 1 × 28 GB 2.12 s; four APUs in parallel 28 GB each 7.0 s wall: the driver serialises); `hipFree` 0.004 s/GB; no flavour is cheaper (uncached 1.26 s, fine-grained 1.21 s, managed 2.23 s, host 2.73 s per 16 GB). **Bytes mapped over the run is the only lever.** `hipMemGetInfo` works (137.4 GB per APU).

**M9 — accounting (`mem_report`).** `mem.c` keeps a table per phase; modules register providers (`mem_acct_register`): plane pools and transform contexts (`rns_mul.c`), block pool by kind (donated / borrowed / own hipMalloc; live, peak, free) (`dbig.c`), registry regions (`mem.c`), host staging, X, digit string, other = VmRSS − known. The driver calls `mem_report(phase)` after init, bs, (tree), recip, dm, T1, end and prints `mem_report_summary()`; `MEM_REPORT_DEVS=1` adds per-APU used/total; size > 1 prefixes `mem[rank]`.

**C4 — init sizing.** Plane pool 1 = 3 q + 16 limbs (12 GiB + 2 MiB at 2^31, `rns_pool1_default_bytes`) instead of 2^pool_log limbs in the all-device flow (`rns_pool1_bytes_req` / `RNS_POOL1_GB` override). Region pools of both parities come from one allocation per device (the arena) of exactly what `region_need` lays out (per-region max over levels, tier switch at 0.85 × bound, `pool_get` adds 1/8; margin ≈ 20 %; replaces `total0/4 (1 + 1/4)`). Arena halves are joined for the block pool (`db_donate_adjacent`) so dm's largest quarters find contiguous room; released by `rns_shutdown_hook`. `ECALC_ARENA_GB` enlarges the arena; `BS_REGION_FLAT=1` restores flat sizing. Seed-sized pinned staging now applies at any size (four node-processes no longer pin 4 × 64 GiB).

**C2 — the five-node level.** `place_node`: levels with more than `bs_balance_n` (16) nodes keep the subtree rule; smaller levels put each output in the least-loaded region. At 4 × 10¹⁰ the five-output level goes from shares 2/5, 1/5, 1/5, 1/5 to 1/4 each and no pool grows inside bs (`bs_st.n_grow`).

**4 × 10¹⁰ size 1 gate (m, reference evicted, same node/allocation as main a75474d):**

| | main | A-mem arenas = bs need (final) | arenas dm-sized (`ECALC_DM_POOL_K=8`) |
|---|---:|---:|---:|
| device mapped at init | 249 GB | **216 GB** | 263 GB |
| init | 17.7 / 16.1 | 15.5 / 16.9 | 19.9 / 19.6 |
| bs | 43.6 / 44.3 (19.1 GB grown) | 42.6 / 41.5 (none) | 41.0 / 41.7 |
| wall | 95.6 / 96.3 | **92.5 / 93.0** | 93.6 / 94.3 |

Net: init −1…−2 s, bs −2 s, wall −2.6…−3 s, digits identical. The dm-sized arena removes every in-run `hipMalloc` but pays at init for the same wall; it stays a knob. Run maps 216 + 10 + 45 = 271 GB vs 295 for main. Regions per parity from the layouts 12.33 / 10.02 / 10.17 / 10.00 GB (flat rule: 12.49 each, and region 0 still grew to 19.1). Accounting summary (m): device 216.6 → 261.9 (recip) → 120.9; pool peak 121.5; host HWM 70.8 GB (X 17.8, digits 40.0, other 13).

**Sizes 2 and 4 on one node.** First attempts were OOM-killed on node 0 in dm: finished non-zero ranks sat at the final barrier holding 150–200 GB each. Now non-zero ranks release device memory before the barrier. main cannot run these configurations on one node (4 × 64 GiB pinned staging; with `POOL_LOG=27` the 2^30 batch tile overflows the pools: segfault). Measured: 2 × 10¹⁰ size 2 PL 30 VERIFY OK 119 s (host HWM 83.5 GB, node-0 device 269 GB at recip); 10¹⁰ size 4 PL 29 identical, peak 305 GB (4 × 76 in the tree), host HWM 46.3 GB. Capacity (a/m mix): size 1 ceiling unchanged at 7 × 10¹⁰ (17 GB less planes, ≈ 16 GB less regions); size 2 ≈ 3 × 10¹⁰ per node; size 4 ≈ 1.5 × 10¹⁰ per node until the division is distributed.

Open issues: (1) **Resolved:** 10¹⁰ size 4 with `POOL_LOG=29` failed T1 (one leaf P differed by a limb) because the striped batch tier grew pool 1 inside the phase; with pool 1 at the full pool for `POOL_LOG` ≤ 30 (job 20751) it is identical (68–70 s). Growing a plane pool while the batch tier runs is unsafe (stale plane pointer, not chased, N-kernel's code); the `rns_dpool` calls stay as a guard. (2), (3) are §M B5 and B6. (4) `ECALC_DM_POOL_K` is the knob for A-div.

## §M11 — memory: the arena's reserved tail (decision 5), the tree's slabs in the arena, the per-node model, 10¹¹ on one node (Phase 11, PLAN §26 row M)

Branch `m11` (from main @ 72aa2e9). Files: `dbig.c/.h` (block pool reserved tail), `binsplit.c/.h` (layout: `dm_layout`, `tree_need_dev`, `arena_get`, `donate_one`, `binsplit_pregrow`), new `ecalc/mem_model.py`. Continues §M (its open issues 2 and 3 are items 1 and 2 here).

**1. The arena laid out with t1's quarter as its contiguous tail.** `dm_layout(N, size)` computes from N alone n_Q, k_mu, t1 = max(n_Q+k, 2k)+8, the hole (t1's quarter + 1/64) and the dm need per device (Q + S, r + r2, t1, the grid piece temporary, min(1 GiB, 1/8)). The arena per region is `max(2 × bs half, dm need, tree need)`; its last `hole` bytes are the reserved tail, a policy over bytes the arena has anyway. `db_pool_set_tail`: a request ≥ 5/8 of the hole is carved from the back of the extent ending at the tail's end (t1 lands contiguous), smaller requests best-fit with the tail excluded; both preferences soft (fallback `hipMalloc` only if nothing fits). `ECALC_TAIL=0` restores the Phase 10 layout. C3 (`ECALC_DM_POOL=1`) becomes a no-op with the tail. Layout matched the model to the byte (APU0 36.78 GB, APUs 1–3 33.07 GB at 4 × 10¹⁰).

Results (m):
- 4 × 10¹⁰ v1 (job 20804, s24-26, identical to `results/e_4e10.out`): arena 136.0 GB (95.7 + 4.2 + 36.1, 5.3 s), **zero pool hipMalloc in bs/recip/division**, device at dm peak 256.9 (was 261.9), wall 79.8 / 79.4 / 82.3 s = 80.5 ± 1.5 vs Phase 10 closing 83.0 ± 1.2 (recip 15.2 → 13.2 s; init pays ≈ 2.4 s more mapping).
- v2 (hole not added to the arena; job 20814, s24-30): arena 132.3 GB, device 253.1, wall 83.2 / 86.8 / 84.7 s; phases 66.7–67.2 = the closing's 66.7 ± 0.3, only init +2.2 s. s24-26 runs phases ≈ 3 s faster than s24-30 (RESULTS §68), so series are not comparable; phases are unchanged.
- 10¹⁰ size 4 v2 (PL 29): identical to `results/e_1e10.out`, **116.5 s** (Phase 10: 145.7), arena 20.3 GB per process, tree and sharded dm hipMalloc 0 (was 4.9–5.4 GB), host 29.3 GB HWM.
- 8 × 10¹⁰ v1: **VERIFY OK 190.9 s** (Phase 10: 210.4), zero hipMalloc, device 369.9 (386.6), host 12.8, node peak 383 (393); reciprocal 37.9 s vs 49.6.
- 10¹¹ v1: **VERIFY OK 416.6 s** (init 45.6, bs 161.2, dm 209.7), device 454.0 GB, host 13.9, node peak ≈ 468 of 502; pool peak live 283.3 vs model 298.4. 4.17 ns/digit (8 × 10¹⁰: 2.63); the reciprocal's 129 `dist` calls spend 107 s of 170 s in `load` (piece loads, not memory).

Versions: v2 was smaller than v1 on APU0/1 at 10¹¹ so two APUs fell back (22.2 GB each; 672 s on s24-30, whose tiers run 50–70 % slower, not chased). **v3** adds the top levels' live P, Q beside the tail to the dm need (78 GB per APU at 10¹¹); one APU still fell back (434.7 s). **v4 (final)**: a small block that must enter the tail is carved from the tail's very end, so the free bytes stay one extent. **10¹¹ v4: VERIFY OK 262.9 s, zero hipMalloc on every APU, device 431.2 GB (= model), host 14.0, node peak 445 of 502 GB.** 8 × 10¹⁰ v3/v4: zero hipMalloc, device 369.1, host 12.8, 195.5 s (s24-16; 290.9 s on slow s24-30). 4 × 10¹⁰ v3 (s24-16) 85.3 s; v4 ×3 (s24-30) 83.2 / 87.6 / 83.8 s, zero hipMalloc, device 253.1. 10⁹ and 10⁸ sizes 2/4 v4 identical, 0 fallbacks.

**2. The tree's slabs at size > 1 sized into the arena.** `tree_need_dev(n_leaf, size)` lays out every tree level as `rns_mul_dist_mn` / `mn_core` size their scratch (slabs sb, rbA, rbB, cx, tmp, spill buffers g × C × 4 limbs, window temporary T) beside the level's live shares; the top level's scratch is also added to the sharded division's need. Arena at size > 1 = `max(2 × bs half, dm need / size + top scratch, tree need)`. Measured effect: see the 10¹⁰ size 4 row above.

**Model beyond calibration (size 1, tail v2, modelled, GB):** 4 × 10¹⁰ device init/dm peak 216.6 / 253.1 (**measured 253.1**), node peak 265; 8 × 10¹⁰ 297.7 / 369.1, 381; 10¹¹ 363.7 / 431.2, 443; 1.2 × 10¹¹ 392.1 / 493.2, 505. **Per-node ceilings at size 1 (v3, modelled): 502 GB → 1.19 × 10¹¹ with the tail (1.16 × 10¹¹ without); 480 GB budget → 1.04 × 10¹¹.** Measured 10¹¹ at 445 GB node peak.

**Size g > 1 (the 576-node question, modelled).** Per node, the leaf's bs regions and the dm shares are D's; the tree's products add the slabs (≈ 3 × share/4), cx + tmp (2 q), T, and **two spill buffers of g × C × 4 limbs each, growing with g** (C ≈ 2^20 at the 576-node top level: 2 × 576 × 2^20 × 32 B = 38.6 GB per APU = 155 GB per node; `DIST_LOGR_DELTA=3` makes it 8 × smaller). The plane pools grow with the top slice (n_top/(4g) > 2^29: `rns_dpool` re-sizes to 214 GB per node above D ≈ 2 × 10¹⁰ at g = 576). Before L's `alltoallv` (B7) the sharded division's `mdb_shift` scratch makes g = 576 impossible at any D; after it the exchange is 17 GB per node at D = 1.9 × 10¹⁰.

| 502 GB node, alltoallv in | g = 4 | g = 64 | g = 576, delta 0 | g = 576, delta 3 |
|---|---:|---:|---:|---:|
| max D per node | 3.9 × 10¹⁰ | 3.9 × 10¹⁰ | 1.9 × 10¹⁰ | 2.2 × 10¹⁰ |
| digits over g nodes | 1.5 × 10¹¹ | 2.5 × 10¹² | **1.1 × 10¹³** | **1.25 × 10¹³** |

(g = 4/64 ceilings sit at 3.9 × 10¹⁰ because the top product needs a 2^33 / 2^37 transform whose slice exceeds the 2^31 plane pool.) The ceiling at 576 is set by the top-level transform scratch (plane pools, cx + tmp, spills), not the exchange; the bs regions and dm shares alone would allow ≈ 8 × 10¹⁰ per node. **Standing rule (PLAN §26), modelled:** with the code as then written (one transform over the whole top product) **1.1–1.25 × 10¹³ digits**; with the top-level product piece-gridded over the 2^31 planes (`split_grid`, not written then) **≈ 4.4–4.6 × 10¹³** (7.7–8 × 10¹⁰ per node); compute ≈ 1 min per node (extrapolated from ≈ 2.0 ns/digit), the fabric part modelled by X2. (Superseded for the current target figure by §M13 headline and §DL15.)

**3. The per-node memory model `ecalc/mem_model.py`** (`mem_per_node(D, g, opts) -> dict`): the code's own sizing formulas ported (not fitted): `e_terms`, `seed_limbs`, `region_need`, `dm_layout`, `tree_need_dev`, plane pools (grown on demand), sharded exchange scratch (`opts['alltoallv']`), host constants (runtime 7.0, staging 4 × 1 GiB, seed buffers 4 GiB at init, writer 0.65 GB, + 6 GB per process at size > 1). `opts`: `pool_log`, `tail`, `alltoallv`, `logr_delta`, `margin`. Agent X's `mn_model.py` imports it. Calibration (m vs model, tail off): device at dm peak 4 × 10¹⁰ 261.9 vs 263.7 (+0.7 %), 7 × 10¹⁰ 352.7 vs 353.4 (+0.2 %), 8 × 10¹⁰ 386.6 vs 383.3 (−0.8 %); tail v1/v2 at 4 × 10¹⁰ 256.9 measured vs 256.8 / 253.1.

Open issues (as left): (1) spill buffers of `rns_mul_dist_mn` scale with g (155 GB per node at 576 with delta 0): first memory item on the target (X's / L's `rns_dist.c`); (2) plane-pool growth at the top tree level (+93…+120 GB per node): `POOL_LOG=32` or a piece grid at the top level; (3) sharded-division temporaries modelled as 1/size of single-node, a `mem_report` row per level would pin them; (4) `ECALC_DM_POOL=1` is a no-op with the tail, deletable (done in §I item 5); (5) `db_pool_tail_stats` (under `DB_POOL_VERBOSE=1`) reports spills; (6) 10¹¹ at 4.17 ns/digit is a `load` time issue, not memory: memory no longer limits one node below ≈ 1.2 × 10¹¹.

## §M13 — memory truth: one ceiling, the two O(share) terms bounded (Phase 13a, PLAN §30 row M; TASKS 1.1, 1.2, 1.3)

Branch `p13-M` (from main @ 203733b). Files: `mem_model.py`; `binsplit.c` (`binsplit_layout_only`, `BS_LAYOUT_ONLY`); `newton_db.c` (`mdb_shift_g`); `rns_dist.c` (`mn_core` exchange, `mdb_add_shifted`, scratch window term); three README switch rows. **Defaults unchanged**: new paths behind off switches.

**Headline (modelled; model = measured device to ±0.05 % at 1e10, 4e10, 8e10, 1e11; `python3 ecalc/mem_model.py --ceiling`):**

| configuration (502 GB node; 480 GB budget in brackets) | ceiling per node | digits in all |
|---|---|---|
| size 1, defaults | **1.18 × 10¹¹** (1.11 × 10¹¹) | 1.18 × 10¹¹ |
| 576 nodes as coded (SHMEM, pool-resident slabs, 8 GiB pool) | **6.95 × 10¹⁰** (6.50) | **4.00 × 10¹³** |
| 576, `MDB_SHIFT_CHUNK_MB=1024` | 8.30 × 10¹⁰ | 4.78 × 10¹³ |
| 576, `MN_T_CHUNK_MB=1024` | 7.55 × 10¹⁰ | 4.35 × 10¹³ |
| 576, both at 1024 MB | **9.24 × 10¹⁰** (8.58) | **5.32 × 10¹³** |

**The "6.7 vs 7.1 × 10¹⁰" disagreement:** both are `mem_model.py` versions. Top-scratch term 74.5 GB (G's line-by-line port of the merged code) vs 86.4 (Q's estimate of the gridded form) = 0.21 × 10¹⁰; the SHMEM symmetric pool 0 (`transport='tcp'`) vs 8.6 GB = 0.17 × 10¹⁰. The merged model with the SHMEM pool gives **6.95 × 10¹⁰**; the 6.7 in RESULTS §77 is stale.

**The C request against the model** (`BS_LAYOUT_ONLY`, `mem_model.py --check-c`): after rounding the dm extra to 2 MiB the model's arena equals the C request **to the byte at all 35 points** (size 1 at 1e9…1e11, size 4, 64, 576 at 1e9…8 × 10¹⁰ per node, with/without `MN_T_CHUNK_MB`). Model corrections made: since Phase 12 I the 3·2ᵏ planes are the default below 5 × 10¹⁰ digits (`planes_3q30`, 180.4 GB; +60 GB at size 1 below 5 × 10¹⁰, no effect at 576); the arena (bs + dm extra) is mapped at init, so the init peak binds at size 1 (1.19 → 1.18 × 10¹¹). Not in the model: transform cache over shares (`RNS_DIST_CACHE_MN`, opportunistic, takes slots only while free − 24 GB per APU allows), `v->spill` (≤ 34 MB per APU). The sharded-division exchange is added on top of the dm need (conservative; at 1e10/4 it did not stack: +2.5 %).

**Model against measurement:** 1e10 size 1 device 234.4 vs 234.36 (−0.02 %); **4e10 313.3 = 180.4 + 132.3 + 0.61 vs 313.26**; 8e10 369.1 vs 369.10; **1e11 431.2 vs 431.16**; 1e10 size 4 per process 56.6 vs 58.04 (+2.5 %, conservative; host −9 %, see open issue 1). Stated error: device within 0.05 % at size 1; node total within ±1.5 % at size 1.

**1.2 `mdb_shift` in rounds (`MDB_SHIFT_CHUNK_MB`).** The pieces are exchanged in K = ⌈SL / chunk⌉ rounds, one `alltoallv` per round; sb and rb hold ≈ m MB per APU instead of share/4 (at 576 × 8 × 10¹⁰: 71 GB per node → 8.6 GB at m = 1024). Bit-identical by construction and measured. Off (0) by default.

**1.3 The window temporary T in rounds (`MN_T_CHUNK_MB`).** `mn_core` passes the window of a piece product through T in rounds of W = 4m MB of limbs per node; T and rbO become O(W) instead of O(the share of C). Exactness: per round T += spills + previous carry (carries out at most once, checked); C's share receives chunks by fixed-length adds (carries out of the share at most once, checked) then a node scan; a window ending at its share's top gives a unit at the next node's bottom (second scan). `mdb_add_shifted` gets the same rounds. `rns_mul_dist_mn_scratch` and `mn_scratch` take min(window, W) (the `--check-c` comparison including the chunked request is exact).

**Measured (jobs 21008, 21015; s24-30 / s24-26):**
- Correctness: `t_mn_grid` at 2 processes with both at 0.01 MB: 2 × VERIFY OK; 10⁸ at sizes 2/3/4 and 10⁹ at sizes 2/4 with forced grids and small chunks: identical; 10¹⁰/4 four variants identical (113.9 s none; 118.1 `MDB_SHIFT` 64; 101.4 `MN_T` 64; 125.4 both); 10¹⁰ size 1 identical 36.2 s (device 234.4); 4 × 10¹⁰ size 1 identical 82.6 s (device 313.3, host HWM 12.1). `mnaccept.sh --only unit,e9,mn`: 15 PASS / 0 FAIL; with both switches at 1 MB: 5 PASS / 0 FAIL.
- Batch 2 (3601181, T allocated after the operands; m): **mdb_shift slabs fall 5.56 → 0.62 GB and T + rbO fall 4.45 → 0.54 GB per node-process at 10¹⁰/2 (9×, 8×)**; tree peak live 11.3 → 10.1 GB, arena 39.7 → 38.2 GB (as the formula says); at 10¹⁰/4 slabs 2.78 → 0.556, T + rbO 2.22 → 0.537, arena 19.9 → 19.3 (the model's 19.90 → 19.27). Whole-run peak does not move at these sizes (23.2, 14.3: the division's product sets it). Wall: 10⁹/2 37.6 → 36.7 s; 10¹⁰/2 205.8 → 208.0 s (+1 %); 10¹⁰/4 111.3 → 125.6 s (+13 %, TCP, four processes on one node, ±10 % spread, 64 MB rounds). On the target rounds should be ≈ 1 GB (≈ 10 per call); cost not measured. At 576 the plane is capped (3.8 GB) while T alone is 7.7 GB per APU, which is why the two terms decide the ceiling there.

Open issues: (1) host term at size > 1: 24.8 GB HWM per process at 10¹⁰/4 vs 21.6 modelled (TCP host copies; the SHMEM target has none, ≈ 0.15 × 10¹⁰ of ceiling; measure on the target); (2) whether the exchange stacks on the dm need at 576 (moot with `MDB_SHIFT_CHUNK_MB`, 8.30 vs 8.41 × 10¹⁰); (3) the dm need is an upper bound (arena 19.9 vs peak live 14.3); (4) rounds' time cost on the target unmeasured. Stale: the RESULTS §77 figure; `estimate.py` still quotes 4e10 = 253 GB (now 313.3) (Q's files).

## §P13b — sizing follows the prime count; the plane cap as one switch; the one-node ceiling at three primes (Phase 13b, PLAN §31 row P)

Branch `p13b-P` (from `p13b-step0` @ c988218). Defaults unchanged: new paths behind off switches (step 0's three primes and `NTT_MODMUL=1` are the base). "Modelled" = the C layout report `BS_LAYOUT_ONLY` (same formula as `mem_model.py` to the byte, §M13).

**`ECALC_PLANE_CAP` (off by default; unset: `POOL_LOG` and the 3·2ᵏ size rule as on step 0).** Each value sets three knobs and refuses a conflicting env (exit 2): `2^30` = `POOL_LOG=30 RNS_PLANES_3Q30=0 DIST_LOGN_TEST=30` (planes 60.1 GB at P = 3); `3*2^29` = `30/1/30` (77.3 GB); `2^31` = `31/0/31` (103.1 GB); `3*2^30` = `31/1/31` (154.6 GB); `fit [ECALC_NODE_GB=480]` = the largest cap whose modelled node total fits. Aliases `30`, `3x29`, `31`, `3x30`. Unset, the cap is 3·2³⁰ below 5 × 10¹⁰ digits and 2³¹ from there (Phase 12 §I size rule). Also: `BS_LAYOUT_ONLY=<D>[:g],...` prints modelled node bytes at all four caps without a device (`binsplit_node_bytes`); `ECALC_INIT_ONLY=1` maps pools + arena and stops (~30 s).

**Code.** Plane pools from one formula (`rns_mul.c rns_plane_pool_bytes(pool_log, b3, np, &p0, &p1)`): q = 2^(pool_log−2) or 3·2^(pool_log−3); pool 0 = np·q limbs; pool 1 = 3q + 16 limbs (independent of np), never below 2^min(pool_log,30) limbs, 2 MiB-aligned; same bytes as step 0. For agent D: mem_model's pool-0 term `4 * q` becomes `np * q`.

**Sizing follows the prime count (job 21051, s24-16, 4 × 10¹⁰, m):** `ECALC_NP=4` device 313.3 GB, 79.90 s; step-0 default (P = 3) **287.5 GB, 69.44 s** (−25.8 GB, all in pool 0); C report equals measurement to 0.05 GB at both. Host HWM 12.1.

**Cap switch at all four values (jobs 21046 s24-30, 21051 s24-16; three primes; one run per cell; m).** All byte-identical at 10⁹ and 4 × 10¹⁰ (cmp or sha1):

| cap | 4 × 10¹⁰ total (bs, dm), init | device peak measured / C layout |
|---|---|---|
| 2³⁰ | 84.20 s (33.9, 36.5), 13.7 | 184.9 / 184.86 GB |
| 3·2²⁹ | 76.79 s (31.1, 30.4), 15.2 | 202.1 / 202.05 |
| 2³¹ | 70.73 s (28.1, 24.3), 18.2 (s24-30); 67.89 (s24-16) | 236.0 / 235.95 |
| 3·2³⁰ | 65.09 s (25.1, 21.2), 18.7 | 287.5 / 287.49 |

Each step down in cap saves 17–52 GB of device and costs 6–9 s of phases at 4 × 10¹⁰.

**One-node ceiling at three primes.** Modelled (502 / 480 GB budgets): 2³⁰ 1.36 / 1.30 × 10¹¹; 3·2²⁹ 1.30 / 1.24; 2³¹ 1.23 / 1.16; 3·2³⁰ 1.06 / 1.02. Measured real runs (VERIFY OK unless noted; D, wall, device peak, host HWM):
- 3·2³⁰: 1.00 × 10¹¹ 206.9 s, 465.5 GB (job 21051); **1.14 × 10¹¹ OK** 228.7 s, 509.0 + 14.8 host = 523.8; 1.16 × 10¹¹ **OOM-killed** in bs (515.2 at init).
- 2³⁰: 1.42 × 10¹¹ OK 677.2 s (dm alone 400 s), 501.4; **1.44 × 10¹¹ OK** 1085.8 s (s24-26), 507.6 + 16.5 = 524.1; 1.46 × 10¹¹ OOM-killed (513.8 at init, ≈ 529.6 total).
- 2³¹: **1.30 × 10¹¹ OK** 353.1 s, 507.1 + 15.7 = 522.8.
Allocation probes (`ECALC_INIT_ONLY`): hipMalloc refused at 518.5–521.4 GB modelled device (2³⁰ 1.48, 3·2²⁹ 1.42, 2³¹ 1.34, 3·2³⁰ 1.18 × 10¹¹); maps up to 515.2. **What binds (m): device allocation at init fails between 515.2 and 518.5 GB (128.8–129.6 GB per APU); a real run needs host beside it: device + host HWM ≲ 524 GB ran, ≈ 529.6 OOM-killed. So the real one-node ceiling is 1.44 / 1.30 / 1.14 × 10¹¹ at 2³⁰ / 2³¹ / 3·2³⁰; mem_model's 502 GB budget is safe but conservative by 0.06–0.08 × 10¹¹; a 524 GB budget (label measured, s24-30) reproduces the edges (1.44 / 1.30 / 1.13). Keep 502/480 for the target until measured there.** (Four primes at 10¹¹, 2³¹: 263 s, 431.2 GB, M11.)

Regression (m): `mnaccept --only unit` (job 21066) 8 passed 0 failed (1165 s); `--only e9` (21069) 2 passed (LIMB_BASE 10: 16.67 s; 2: 25.33 s).

Open issues: `ECALC_PLANE_CAP=fit` tested on the login node only (picks 2³¹ at 1.1 × 10¹¹); the 5 × 10¹⁰ threshold for 3·2ᵏ planes dates from four primes; at three primes 3·2³⁰ is the fastest cap at 4 × 10¹⁰ (65.1 s vs 67.9–70.7 at 2³¹): whether the default becomes "the largest cap that fits" is the user's decision; at the leanest cap large sizes are slow (1.44 × 10¹¹ took 1085.8 s on s24-26 vs 677 s on s24-30 at 1.42 × 10¹¹; node vs size not separated) and the modelled 576-node wall should use that cap's own times.

## §R16 — the region-pool sizing abort of ecalc at 10¹¹ digits on 4 nodes (2.5 × 10¹⁰ per node)

Branch `p16-R` from main e7dfcae. Files: `binsplit.c` (`region_need`, `level_q_lower`, `bs_pool_rule`), `mem_model.py`, README row.

**Symptom (B16, job 12178, aac7).** `ecalc 10¹¹` on 4 aac7 nodes aborts on rank 0 at the top batch-tier level: `bs: region pool 0 of parity 1 would grow inside bs, 10.87 -> 16.15 GB` (rc 6, `RNS_POOL_GROW=0`). Memory was not the limit (258 of 502 GB).

**Cause.** `region_need` (Phase 9 C4, made an abort by Phase 12 R) lays out levels with the seed bound `per` based on the whole series' N, and treats a level as mdev-tier when `2 × ⌊0.9 × bound⌋ + 1 > 2^BS_MDEV_LOGL`. The 0.9 is the size-1 ratio of real sizes to the bound (0.90–0.945); at size > 1 rank 0 holds terms [1, N/g) so the real sizes are 0.88 of the bound. Level 23's rule said mdev (1.087e9 > 2³⁰), the run's own decision from real sizes (1.062e9 ≤ 2³⁰) took the batch tier, whose outputs (16.15 GB) did not fit the pool sized without that level. Other sizes pass because the boundary level falls elsewhere (10¹¹ on one node ratio 0.92, 10¹⁰ on 4, 9.16 × 10¹⁰ on 12 and 576). The pool that grew is a bs region, not plane pools.

**Fix `BS_POOL_RULE` (default 1; 0 = old rule).** `region_need` predicts the tier from a lower bound on the level's largest Q (the last full node's product over its terms; lgamma-based as `bs_seed_terms_for`), 0.97–0.99 of the real size; a level is left out only when even that bound is over 2^BS_MDEV_LOGL. Sizing only, digits unchanged. `mem_model.py` follows (`--check-c` exact).

Layout before → after (`BS_LAYOUT_ONLY`, login node, measured): 2.5 × 10¹⁰ : 4 (10¹¹ on 4) bs regions 71.65 → **93.39 GB**, arena 154.62 → **161.06 GB**, node at 2³¹ 279.91 → **286.35 GB**; 10¹⁰ : 4 +0.44 GB; 10¹¹ : 1, 9.16 × 10¹⁰ : 12 and the 576-node target (9.16 × 10¹⁰ : 576) **identical** (so RESULTS §94 node totals stand). `MN_PLAN_ONLY` shows the mn plan was not the under-sized pool.

Tests: `mem_model.py --check-c` exact on 5 points; **10¹¹ on 4 nodes (job 12189, aac7, shmem): rc 0, all 4 nodes VERIFY OK, `digcmp.sh` against `results/e_1e11.out` identical, no `would grow` line, total 517.67 s** (m; B's GROW retry run took 323 s on other nodes: one node's level-24 mdev took 35 s vs 1.5–1.7 s on the others, a node/NFS effect, not sizing); **4 × 10¹⁰ on 4 nodes identical, 195.82 s** (m). `mnaccept --only unit,e9,mn` ran only `mn e9` sizes 2 and 4 (PASS); unit and e9 size 1 failed on S16's `MNRUN_CPUS_PER_TASK=auto` bug on multi-node jobs (fixed in `p16-S`, cherry-picked, `head -1`), `mn e8` DIFFERS was a reference-file artifact (clone's `ref/`). `mem_model.py --p15` output byte-identical with rule 0 and 1.

Open: rerun unit / e9 size 1 on a one-node job; the 517.7 vs 323 s difference for the Phase E owner; **the user decides whether `BS_POOL_RULE` stays default 1 (recommended: without it 10¹¹ on 4 nodes aborts).**

## §AS15 — Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps)

Agent AS, branch `p15-AS` from main f184d51 (B1). Labels: measured, replayed (an exact port of the pool driven by a measured trace), modelled.

**Approach.** Record every block-pool event (`DB_POOL_TRACE=1`, print only), replay the pool offline (`archive:scripts_2026-10-23/as_pool_replay.py`, exact port of `dbig.c` placement) and try arena layouts before node time.

**Trace at 10¹¹ (B1 defaults, job 21610, s24-16, m):** total 207.41 s, dm 105.50 s; **15 remaps summing 9.49 s** (APUs 1–3: 11.11 and 4.17 GB in the reciprocal, 22.22 GB for `t`, a growth of 2 chunks costing 0.22–3.80 s, 22.22 GB for `xq`). Replay: 116 blocks, 0 mismatches, same 15 remaps. Causes: (1) the arena (63.82 GB) is not a whole number of 2 GiB chunks; the last chunk's remainder lies after the tail's end, so the "spill to the tail's very end" rule finds no extent ending there and carves the bs top level's second output from the front of the tail; (2) with whole chunks, the division's `t` (22.22 GB) meets 31.09 GB free in two extents (11.11 + 19.98) because the freed top-level children leave 8.07 GB holes (dm_layout v3 counts bytes, not holes).

Replayed layouts (APUs 1–3): B1 15 remaps; whole chunks 9; + 1 chunk 6; **+ 2 chunks (68.72 GB per APU) 0**.

**More sizes (jobs 21610, 21615; every replay exact).** Remaps on B1 (m): 2 × 10¹⁰ 0; 4 × 10¹⁰ 2 (0.44 s); 6 × 10¹⁰ 4; 8 × 10¹⁰ 3 (1.14 s); 9 × 10¹⁰ 9 (3.35 s); 10¹¹ 15; 1.16 / 1.2 / 1.3 × 10¹¹ 4 (2.33 / 2.17 / 3.01 s). `dm` at 10¹¹ fill 0: 91.15 s vs fill 128: 105.50 s (one run each, the cost the fill brought). The extra room that removes remaps is not monotone (greedy placement); the tail's size and the "large" threshold do not help at a fixed arena. **Rule: room = f × hole (the division's largest block per APU) in the dm need, arenas in whole chunks. Every f in [0.13, 0.23] leaves 0 remaps over ten traces; 0.25 leaves 4; f = 0.16 chosen** (least memory).

**Switch `BS_ARENA_ROOM=<f>` (off by default; needs the VMM pool).** `binsplit.c`: `bs_arena_room()`, `as_arena()` (arena in whole `DB_POOL_VMM_CHUNK_GB` chunks), `dm_layout` gets `room = f × hole`; `dbig.c`: `db_pool_vmm_chunk()`, `DB_POOL_TRACE=1`; `mem_model.py` follows (`dm_layout(room=)`, `arena_of(chunk=)`); README rows `BS_ARENA_ROOM`, `DB_POOL_TRACE`; `archive:scripts_2026-10-23/as_pool_replay.py`, `tests/as_sweep.sh`. Off = byte-identical layout to B1. `--check-c` exact at f = 0 and 0.16 (10 points each). Mapped arena change (modelled from the port): 2 × 10¹⁰ +4.3 GB, 4 × 10¹⁰ 0, 10¹¹ +12.9 (exactly the 2 chunks × 3 APUs B1 maps inside the division), 1.3 × 10¹¹ +17.2, target share (7.38 × 10¹⁰ : 576, NP=4) +8.6 GB per node.

**Node runs.** First series (job 21619) found a bug: `binsplit_pregrow` rounded from the unrounded cap and `arena_get` re-rounded to 2 MiB, so the arena ended a few MiB into a 33rd chunk (the §2 defect again); fixed in 1667150; check run (job 21626): arenas 39/32/32/32 chunks, 0 remaps, 0 growths, dm 88.48 s, VERIFY OK. **Paired series at 10¹¹ (job 21626 + 21634, s24-26, interleaved, all VERIFY OK, m):**

| (means) | switch off | switch on | delta |
|---|---|---|---|
| dm, no digit file (3 + 3) | 100.87 s | 88.48 s | **−12.4 s** |
| total, no digit file | 191.88 s | 179.67 s | **−12.2 s (−6.4 %)** |
| dm, digits written (2 + 2) | 103.90 s | 89.36 s | −14.5 s |
| total, digits written | 194.97 s | 179.61 s | **−15.4 s (−7.9 %)** |
| remaps | 15 (5.6–9.4 s, 3 growths) | **0** | |

Digit files identical (sha1 ×2, `digcmp.sh` ×1). The gain exceeds the remap seconds (the events stall all four APUs). 4 × 10¹⁰: dm −0.6 s, total −0.2 s. **Memory: node peak at 10¹¹ unchanged (421 GB measured in both arms; device 393.6, host 27.3–27.6)**: the 12.9 GB are mapped at init instead of by the growth.

**1.3 × 10¹¹ and the budget fix (3fa6f30).** With the room, 1.3 × 10¹¹ was refused at init (budget check rc 8, node peak 480.8 > 480; the check counts measured VmRSS 24.9 GB where the layout counts 15.6), and B1 remaps only 4 times there (2.45 s). So the room is dropped when the node with it would exceed `ECALC_NODE_GB` (`as_room_fits`; `mem_model.py` decides alike; arenas stay in whole chunks): the one-node ceiling is unchanged. Remaining runs (jobs 21651, 21655, b0db7ac; m): 1.3 × 10¹¹ off 271.36 s / on (room dropped) 267.79 s (dm 141.5 / 140.6); sizes 3, 5, 7 × 10¹⁰ total 43.52 / 70.60 / 107.37 s, 1.05 / 1.1 / 1.25 × 10¹¹ 191.89 / 203.97 / 260.31 s, all 0 remaps; 10¹¹ digits written identical by sha1 and `digcmp.sh` against `results/e_1e11.out`. All seven new traces replay exactly. Gate (job 21650, `BS_ARENA_ROOM=0.16 mnaccept --stress --only unit,e9,mn,stress`): **16 PASS 0 FAIL** (t_dbig 0 555 checks; stress 10/10 identical).

**Recommendation (user's decision):** adopt `BS_ARENA_ROOM=0.16` with `BS_SEED_FILL=128`; the whole-chunk rounding alone is free and removes the reciprocal's remaps. Open: f chosen on 17 traced sizes, placement greedy so an untested size can still remap (one traced run answers any size); `mem_model.py` node peak with the room is pessimistic (+16.3 GB at 10¹¹, one-node ceiling 1.24 vs 1.34 × 10¹¹ modelled) while the run cannot be refused (budget rule); effect at the 576 target unmeasured. 576-node estimate unchanged: **4.25 × 10¹³ digits in 256.0 s (4.27 min) without the write, 285.7 s with the packed write** (modelled, RESULTS §87 / DOC15), node memory 416.0 GB of 480 on defaults, 429.0 with the room (modelled).

## §MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). **STOPPED after a second node failure; nothing here is adopted except the print-only timeline.**

**Feasibility timeline at 10¹¹ (job 21659, s24-30, 2add00f = int15d + `ECALC_INIT_TL=1`, m).** Arena chunks are 2 GiB; p0 = parity-0 half (seeds' target, mapped inside `binsplit_pregrow` by four threads), p1 = parity-1 half (level 1's outputs, mapped by four background threads under the mapper lock), rest = dm extra. Defaults: planes done 6.88 s, p0 mapped 7.25 → 19.13 s (0.186 s/chunk), seed thread idle **6.92 s** waiting for the pools, p1 + rest mapped by 33.62 s (0.215 s/chunk), seeds end 35.49, **level 1 starts 35.96, done 40.74**. With `BS_ARENA_ROOM=0.16`: level 1 starts 35.07; room + 96 seed threads 36.46; room + `ECALC_SEED_ORDER=after` 37.90. Per chunk the mapper holds its lock 0.15–0.24 s (`hipMemCreate` 0.12–0.16, `hipMemSetAccess` ≈ 0.058, `hipMemMap` + zeroing ≈ 0); the lock is not fair. **What binds:** level 1 needs p0 and p1 of every APU, 124 chunks (266 GB); seeds end 34.6–36.0 s and the p1 mapping 31.3–34.7 s, both on the critical path. Modelled expected gain of overlapping (seeds need only the chunks they write, one mapping stream in need order): level 1 from 35–36.5 s to ≈ 27–30 s, −5…−8 s at 10¹¹ (modelled).

**The stream `DB_POOL_VMM_STREAM=<W>`** (`dbig.c`: under the switch `db_vmm_arena_alloc` only reserves VA; W workers map units of `DB_POOL_VMM_STREAM_BATCH` (2) chunks in need order after the plane pools; `binsplit.c`: every seed DMA waits for its own chunks via `seed_wait_map` / `db_vmm_wait_range`; the rest is released at the first device top-tier level, `db_vmm_stream_rest`). First runs (job 21661, s24-26, m): W = 4: VERIFY OK but 216.91 s (one seed DMA launch blocked 32.9 s behind the workers; 0.27 s/chunk) so W = 1 is the form; W = 1: seeds end **28.33 s** (vs 34.3–35.5) but level 1's chunks are mapped at 34.93 s (0.216 s/chunk), so level 1 does not move; **the node rebooted** during the run (Slurm NODE_FAIL). Repeat on s24-30 (job 21662, rest deferred to the device top tier): seeds end 29.23 s, level 1 at 35.90 s, then the mdev tier took 39.6 s (≈ 27 normally), the reciprocal 59.7 s (≈ 38), and **the node rebooted again in dm**. Every other run (15, stream off or W = 4) was VERIFY OK. Common factor of the failures (not proven; earlier unrelated NODE_FAILs exist, jobs 20629, 20945): VMM chunks created and mapped while GPU kernels run bs products. `DB_POOL_VMM_STREAM` now refuses to run without `DB_POOL_VMM_STREAM_ACK=1` (b94072c). **No further node work with the switch.**

**Conclusions.** (a) The stream removes the seeds' idle wait exactly as modelled (seeds end 28.3 / 29.2 s instead of 34.3–35.5, m) but level 1 does not move: **the mapping itself is the floor**, ≈ 0.2 s per 2 GiB chunk whoever calls it (one worker 0.216; old pregrow threads 0.16–0.19; old background mappers 0.20–0.23 under seed load; 0.131 only with the CPU idle; `hipMemCreate` ≈ 70 %, `hipMemSetAccess` ≈ 30 %). The runtime serialises the calls, four workers map slower and starve launches. Letting bs levels wait only for their own chunks would overlap mapping with compute, the regime preceding both failures: not built. (b) Seed threads at 10¹¹ on the old path (m): 192 threads (default) level 1 at 35.07 / 35.40 s; 96: 36.46 (spans 16.6 → 19.9 s); 48: 47.63 (spans 35.7 s): **keep all threads**. (c) Timeline facts: `rns_init` staging + contexts 1.5–2.5 s, plane pools 4.2–5.6 s (≈ 0.045 s/GB); seed pinned buffers (2 × 8 GiB) cost 1.4–1.6 s at start. **The remaining lever is the mapping rate outside the VMM path:** hipMalloc maps at 0.036–0.05 s/GB against hipMemCreate's 0.077–0.1 (R114 §6b); R114 measured the VMM arena at +8 s of init at 10¹¹ vs the hipMalloc arena. With `BS_ARENA_ROOM=0.16` the division at 10¹¹ remaps 0 times (§AS15), so `DB_POOL_VMM=0` where memory allows may be worth one paired series (untested; no code needed).

Tests: timeline, stream off, 10¹¹ × 7 (job 21659): 7 × VERIFY OK, totals 187.1–199.5 s; r0c (room, stream off) 179.07 s VERIFY OK; e9 stream W = 4 VERIFY OK 12.41 s. **Gates owed before anything merges** (not run after the node failures): `t_dbig 0`, `mnaccept --stress --only unit,e9,mn,stress` at b94072c, stream off. Open: two nodes (s24-26, s24-30) needed the admin; if ever retried, a unit test that maps chunks on one APU while kernels run on all four first, on a node that can be spared. **Merge only `ECALC_INIT_TL` (5162a8f, print only); do not adopt `DB_POOL_VMM_STREAM`.** 576-node estimate unchanged: 4.25 × 10¹³ digits in ≈ 256 s without the write, ≈ 286 s with it (modelled, RESULTS §87 / DOC15); node memory 416 of 480 GB.

## §PS15 — Phase 15 agent PS: plane pool 1 at four primes (the rc 6 at 9.17 × 10¹⁰, `ECALC_NP=4`)

Branch `p15-PS` from `int15f` 2788e1d.

**Cause.** `ECALC_NP=4 ./ecalc 91694091804` (job 21693, s24-16) stopped after bs level 21: `rns_dpool: plane pool 1 on APU 0 would grow inside a phase, 12.89 -> 17.18 GB` (rc 6). Pool 1 was 3 q + 16 limbs at every prime count (q = 2^29 at 2^31: 12.89 GB); the batch-local tier (`rns_mul_batch_local`) holds a tile's B planes in pool 1: up to np q, i.e. 4 q = 17.18 GB at four primes with L = q and Mmax = 1 (the 20-product level at 9.17 × 10¹⁰). Three primes (3 q < 3 q + 16) never reach it, so no default run hit it after Phase 13a made three primes the default.

**Fix.** `rns_plane_pool_bytes`: with four one-node primes pool 1 = max(3 q + 16, 2^min(pool_log,30) tile, **4 q**) (**+17.18 GB per node at 2^31**); three primes and `ECALC_NP=auto` unchanged byte for byte. **`RNS_POOL1_4Q`** (default 1; 0 keeps 3 q + 16 and fits the batch-local tile to pool 1, falling back to the striped tier when the B planes cannot fit; skipped under `RNS_POOL_GROW=1`). `mem_model.py` takes `pool1_np`.

**Sweep (login node, no device, 27 files per setting):** unset/3/auto and `ECALC_NP=4 RNS_POOL1_4Q=0` identical to base; `ECALC_NP=4` changes `planes:` lines by +17.18 GB (2^31), +12.89 (3·2^29), +25.77 (3·2^30), none at 2^30. Modelled node at 9.169 × 10¹⁰ cap 2^31: 412.88 GB (was 395.70); `ECALC_PLANE_CAP=fit` at `ECALC_NP=4` now picks 2^31 instead of 3·2^30. Side effect: with pool 1 at 4 q the dist tier's `RNS_STRATEGY=auto` takes form B for 2^31-point products at four primes (75 pieces instead of 84 at 4 × 10¹⁰). `--check-c` exact (0 terms off, including the `planes:` figures). **Target (modelled, `mem_model.py --p15`):** `ECALC_NP=4` launch line 5.1 × 10¹³ / 576 = **472.6 GB per node (was 455.4)**; the 480 GB ceiling on the launch line drops from 5.57 to 5.24 × 10¹³; `RNS_POOL1_4Q=0` keeps the old figures.

**Node tests (s24-16, m, jobs 21702, 21706, 21732):**
- The failing run now passes: `ECALC_NP=4` 9.169 × 10¹⁰ **VERIFY OK, 172.36 s** (A) / 173.96 s (C), no growth; node **429.4 GB measured** (device 402.2 + host HWM 27.2) vs 480; the budget model 422.4. Planes 137.4 GB (was 120.3: +17.1 as modelled).
- `ECALC_NP=4` 4 × 10¹⁰ 61.05 s and 10¹¹ 212.13 s, both `digcmp.sh`-identical; `RNS_POOL1_4Q=0` at 9.17 × 10¹⁰ VERIFY OK (level 22 went to the striped tier, no growth), 4 × 10¹⁰ identical.
- mnaccept at `ECALC_NP=4`: unit, e9, mn **15 PASS 0 FAIL**; stress PASS (10/10, 16 growths logged); default e9 2 PASS; default 4 × 10¹⁰ 59.45 s identical.
- Timings at 9.169 × 10¹⁰ (job C, single runs, no `RNS_VERBOSE`): `ECALC_NP=4` **173.96 s** (bs 82.6, dm 71.8); `RNS_POOL1_4Q=0` 188.97; `RNS_STRATEGY=C` 203.86; `ECALC_NP=3` 158.45. The fallback itself costs 0.6 s; the rest of the ≈ 15 s between the fix and `RNS_POOL1_4Q=0` is form B at 2^31 which the larger pool enables. So the 17.2 GB buy ≈ 15 s at 9.17 × 10¹⁰; four primes cost +15.5 s over three at this size.

Open issues (user's choices): the target's memory with `ECALC_NP=4` (+17.18 GB per node, 7.4 GB under 480; `RNS_POOL1_4Q=0` keeps 455.4 at ≈ 15 s per one-node-tier run; at 576 the one-node tiers are only part of the wall, not modelled); form B at 2^31 is new at four primes in the default flow (passed all tests); `BS_ARENA_ROOM=0.16` at `ECALC_NP=4` drops the room at ≥ 1.16 × 10¹¹; `RNS_POOL1_4Q=0` at 10¹¹ and repeated timings not run.

## §DL15 — the arena sized to DKM's division, and the room check on the model's node (Phase 15 Batch 3, agent DL)

Branch `p15-DL` from `int15i` ab8daa2.

**New 576-node ceilings (modelled; arena by `BS_LAYOUT_ONLY`; `tests/dl15_ceilings.py`; launch line `ECALC_NP=auto RNS_DIST_CACHE_FIT=1 COMM_SHMEM_ROUND_MB=1024 COMM_TRANSPORT=shmem`):**

| | largest total digits fitting 480 GB with the room kept | room turns off from | largest size at all (room dropped, whole chunks) |
|---|---|---|---|
| built (`NEWTON_DKM=1`, default) | **5.396 × 10¹³** (471.93 GB) | 5.398 × 10¹³ | **5.532 × 10¹³** |
| `NEWTON_DKM=0` (new check) | 5.166 × 10¹³ | 5.168 × 10¹³ | 5.422 × 10¹³ |
| one node, built | 1.320 × 10¹¹ | 1.322 × 10¹¹ | 1.382 × 10¹¹ |

Node at 5.1 × 10¹³: 471.93 → **463.34 GB** (arena 300.65 → 292.06). **Finding: the room-0 figures overstate the ceiling by one chunk**: the VMM pool maps every arena in whole 2 GiB chunks even with `BS_ARENA_ROOM=0`, while `mem_model`'s room-0 node counts it unrounded; so MS's room-0 ceilings (5.571 / 5.696 × 10¹³ with DKM) are really **5.422 / 5.532 × 10¹³**, and 5.55 × 10¹³ does not fit (480.52 GB).

**Built (`binsplit.c`, `mem_model.py`).** (1) `dm_layout` follows `NEWTON_DKM` (DKM layout at size > 1 always, at size 1 only in the device flow n_Q ≥ 9/8 · 2^`BS_MDEV_LOGL`): reciprocal's jl at h, DKM's division set, the hole is DKM's largest block (size 1: step 1's `X_hi·Q`, n_Q + ⌈k/2⌉ + 8 limbs, which `mul_grid` reserves whole; size > 1: t, k + 8 limbs; the model had used k + 8 at size 1 too, smaller than that block). With `NEWTON_DKM=0` the layout lines are byte-identical to ab8daa2's. (2) `as_room_fits` counts the model's node (planes by the `auto` rule, arena with the room, 6.0 GB of VMM bs growth, host HWM); `mem_model.as_room_fits` / `room_host` is the twin; `BS_LAYOUT_ONLY` prints a `room:` line, the layout line gains ` dkm 1`. (3) `--check-c` exact on 16 layout files (0 terms not exact, room terms included). (4) Outside: `mn_model.py` passes `dkm=DKM`; `--check-c`'s `MN_P24` default 2; `DEFAULTS15D` the new `mem_per_node` base; README rows `NEWTON_DKM`, `BS_ARENA_ROOM`, `BS_LAYOUT_ONLY`.

**Tests (aac6, m).** Batch A (job 21801, s24-16): 10¹¹ ×4 alternating new / base (ab8daa2), all `NEWTON_DKM=1`: identical to `e_1e11.out`; arena **283.5 vs 289.9 GB (−6.4 GB)**, device at init 387.2 vs 393.6; 0 remaps, 0 growths, 0 hipMalloc; division window peak 54.07 GB per APU in both (76.3 % of the new pool); `dm` 72.49 / 74.24 s new vs 72.93 / 74.63 base: **neutral** (new_2's bs outlier 105.1 s vs 78.8–82.9 s is noise). Stage D (timing series) not run. Batch B (job 21805, s24-30): 1.3 × 10¹¹ new: room kept, arena 335.0 GB, 0 remaps, dm 114.25 s, VERIFY OK, budget peak 464.1 GB; base (old check dropped the room): 2 remaps (0.92 s), dm 117.70 s; 4 × 10¹⁰ identical; 10¹⁰ at sizes 2/3/4 identical (window peak 5.52 / 2.96 / 2.76 GB per APU, 43–49 % of the pool). Batch C (job 21808): `mnaccept --stress --only unit,e9,mn,corr,stress` **20 passed 0 failed**.

Open issues: (1) the target's mn division peak is counted, not measured (DKM mn path peaks at 5.5 u + product scratch; reservation 55.54 GB per APU vs 54.11 counted at 5.1 × 10¹³: margin 1.4 GB, modelled; today's path 6.0 u = 59.0 GB vs 56.83 reserved, already over, M6's finding; sizes 2–4 on one node are 43–49 % of pool so do not test it; a 2-node `ECALC_LIVE=1` run at a dm-bound size would); (2) `mem_model`'s node should round VMM arenas to chunks at `BS_ARENA_ROOM=0` (not done); (3) size-1 room host uses log10 N! (< 0.02 GB difference).

## §I — the initialisation floor: every allocation form measured, the seeds against the mapping, the 3·2³⁰ planes on the number (Phase 12, PLAN §27 row I)

Branch `i12` (from main @ 7aded87). Files: `mem.c/.h` (allocation form), plane-switch default in `rns_mul.c`, init lines of `ecalc.c` (C3 block deleted), `binsplit_seeds_begin` (seed order), new `ecalc/tests/t_alloc.c`. 4 × 10¹⁰ runs with the reference evicted, digits `cmp`'d against `results/e_4e10.out`; jobs 20873 (s24-30), 20894, 20903 (s24-16), 20929, 20934, 20936 (s24-26). All m.

| item | result | adopted |
|---|---|---|
| 1. every allocation form (`t_alloc`) | none maps cheaper than `hipMalloc` in a fresh process: 0.057–0.072 s/GB, kernel page work on the calling thread under one lock; host forms cost more and route `hipMemcpy` through SDMA (21 GB/s) | `MEM_ALLOC=hipmalloc\|fine\|uncached\|managed\|host\|mmap`, default `hipmalloc` |
| 2. allocator behind pools, regions, block pool | one pair `mem_dev_malloc` / `mem_dev_release` under every `hipMalloc` of mem.c | yes (bit-identical in every form) |
| 3. seeds vs the mapping window (`ECALC_SEED_ORDER=overlap\|first\|after`) | wall 81.5 / 88.5 / 88.3 s: overlap wins by 7 s | overlap stays default |
| 4. 3·2³⁰ planes on the floor | phases 58.5–59.3 vs 63.1–64.8, init +4 s, **wall 80.8 ± 1.3 vs 82.0 ± 0.3 s** | **size rule is the default** (on below 5 × 10¹⁰ at 2³¹ pools) |
| 5. `ECALC_DM_POOL` / the C3 block | deleted (`ECALC_TAIL=0` remains the fallback; the in-phase-growth report line stays) | yes |

**The floor.** Init at 4 × 10¹⁰ is the bytes the run needs mapped at 0.05–0.075 s/GB, with the seed thread's 14 s of CPU work hidden inside: 252 GB (arenas 132.3 GB in 5.5–7.9 s, planes 120 GB in 8–9 s) = 16–19 s planes off, 312 GB = 20–23.5 s planes on. The cost is **CPU time in the kernel on the allocating thread**: a fresh process's first 28 GB `hipMalloc` on one APU takes 2.01 s wall = 2.01 s system time (0.072 s/GB; on MI300A "VRAM" is system memory of the APU's NUMA node handed out by the amdgpu/TTM allocator in zeroed pages). Four threads on four APUs take 7.9 s wall for 1.5–2.4 s system time each: **serialised by one lock** (0.071 s/GB aggregate). The only cheaper mapping seen is re-allocating memory the process freed: 0.035 s/GB (the driver keeps the freed pages; A-mem's 0.047 and an earlier 0.035 were that). A persistent pool would help only within one process (zero in-phase `hipMalloc` since M11); across runs only a daemon holding memory would, not a design for 576 nodes. `hipMallocAsync` from a mempool: cold allocation same cost (11.7 s for 200 GB), warm re-allocation after `hipFreeAsync` faulted on this ROCm: not a candidate.

**1. `t_alloc` (job 20873, 50 GB per APU on four APUs at once).** Per form, alloc wall / s per GB: `hipMalloc` 14.38 s / 0.072; mempool async 11.74 / 0.059 (cold); `hipMallocManaged` 22.90 / 0.114 (copies on SDMA 21 GB/s); `hipExtMallocWithFlags` uncached 8.60 / 0.043 and fine-grained 7.03 / 0.035 (both warm re-allocation; 0.057–0.066 in a fresh process); `hipHostMalloc` (any flavour) 18.1–18.5 / 0.091–0.092 (copies 21 GB/s); mmap + `MADV_HUGEPAGE` + `hipHostRegister` 16.08 / 0.080 (registration is the cost); mmap without huge pages 136.2 / 0.681 (GPU at 60 %, TLB); `MAP_HUGETLB` unavailable (no reserved huge pages). **Every form runs kernels at HBM speed** (bw 3.0 TB/s, 2²⁸ transform 49 ms) except 4 KiB-paged host memory; host-side forms let the CPU store 12 × faster (30 vs 2.6 GB/s) but `hipMemcpy` between such buffers goes to SDMA at 21 GB/s vs 1.4–1.6 TB/s blit kernels on device memory (checkpoints, tree slab copies, `mem_dev_copy` would pay), and map no cheaper. `MEM_ALLOC` default went back from `fine` to `hipmalloc`: 4 × 10¹⁰ with `fine` init 18.5 / 19.2 s, wall 84.9 / 85.1 vs `hipmalloc` init 16.0–18.1, wall 81.5–82.3 on the same node. Probe (`t_alloc 28 none`, job 20903): mapping is CPU work in the kernel serialised across callers by a lock; more threads or another API do not change it. Seed-like team: 200 GB `hipMalloc` takes 11.25 s alone (0.056 s/GB), 15.7 s with all 192 cpus computing (0.079), 44 s with cpus computing and storing into device memory at the team's full 200 GB/s rate (0.221); the real seeds store ≈ 6 GB/s, and in the run the plane pools map at 0.067–0.075 s/GB under the seeds vs 0.064 alone.

**2. `MEM_ALLOC`.** The pair is used by `mem_dev_alloc`, `dpool_get`, `dpool_get_exact` and their frees; M's tail layout and `mem_report` see the same pointers. Host forms were not run through ecalc (the 21 GB/s copies rule them out first).

**3. Seeds vs the mapping window (job 20894, s24-16, 4 × 10¹⁰):**

| order | init | seeds (where) | bs | dm | phases | **wall** |
|---|---:|---|---:|---:|---:|---:|
| overlap (default) | 17.7 | 16.4 s in init (spans 8.5, waited 5.5 for regions, 1.7 issuing DMAs) | 35.2 | 28.5 | 63.8 | **81.5** |
| seeds first, then pools | 24.2 | 14.4 s in init | 35.6 | 28.7 | 64.4 | 88.5 |
| mapping first, seeds after | 16.2 | 9.1 s in bs | 43.9 | 28.2 | 72.1 | 88.3 |

Plane pools map only 0.7–1.1 s slower under the seeds (8.8 vs 7.7–8.1 s for 120 GB), while moving the seeds out of the window puts 7–9 s on the wall. `BS_SEED_THREADS=96`: spans 13.9 s, wall 83.7; 176: 82.0: no gain. Digits identical.

**4. 3·2³⁰ planes (job 20903, s24-16):** planes on (24 + 18 GiB per APU, 180 GB): init 22.5, bs 32.7, recip 12.5, dm 26.4, phases **59.2**, wall **81.7**; planes off (120 GB): phases 64.2 / 64.8, wall 82.3 / 82.1. The extra 60 GB map at the same 0.074 s/GB (+4.4–5.2 s init); phases gain 5.0–5.6 s (P's 3.6 s in top levels and reciprocal plus 2.5 s in the division that M11's tail layout uncovered). Net −0.4…−0.6 s of wall, within one run's noise on the wall but not on the phases; `rns_planes_3q30_default` is on at 2³¹ pools below 5 × 10¹⁰ digits (`RNS_PLANES_3Q30=0/1` overrides; 7–8 × 10¹⁰ do not fit with them). Fastest phase time of this code (59.2 s; P's 62.4 on the old layout).

**5. `ECALC_DM_POOL` deleted.** With the tail layout t1's quarter lands in the reserved tail and the pool never grows (zero hipMalloc in every phase at 4, 8 × 10¹⁰); `ECALC_DM_POOL_K` untouched.

**6. Gate (m).** 10⁹ decimal and `LIMB_BASE=2` identical in every build; 4 × 10¹⁰ planes off ×5: 82.3 / 81.5 / 82.0 / 82.3 / 82.1 s = **82.0 ± 0.3 s, init 17.5 ± 0.9**; planes on (final default) ×6: 81.73 / 80.44 / 78.81 / 82.19 / 81.71 / 79.77 s = **wall 80.8 ± 1.3 s, phases 58.8 ± 0.3, init 22.1 ± 1.3** vs Phase 11 close 81.5 ± 1.4, phases 66.0 ± 0.4, init 15.5 ± 1.1 (phase gain = M11's layout −2 s + 3·2ᵏ planes −5 s; init cost = the 60 GB they map); 8 × 10¹⁰ ×2 (planes off by the size rule): **VERIFY OK, 192.0 and 189.3 s**, device 369.1 GB throughout, zero hipMalloc, node peak ≈ 382 GB; `t_ntt 24` VERIFY OK (565 checks); 10⁸ at sizes 2, 4 identical. Init before/after at 4 × 10¹⁰: staging 1.0 → 0.9 s; arenas 4.8–5.3 s (95.7 GB) → 7.3 s (132.3 GB; the tail layout's 36.6 GB of dm extra bought zero in-phase growth); plane pools 8–9 s (120 GB) → 13.4 s (180 GB). **Floor = 0.06–0.075 s/GB × bytes mapped at init** (252 GB → 15–16 s, 312 GB → 21 s); the seeds fit inside it. The bytes are M's, the rate the driver's.

Open issues: the mapping rate (kernel page clearing on one core under one lock) is not ours; a driver clearing pages on SDMA engines or handing out pre-cleared pages would fix it; with planes on, init is 22.5 s of which the seed thread is 21 s, so further mapping cuts expose the seeds (14 s CPU, 8.5 s spans); the seeds' two buffered chunks wait 1.4–1.8 s behind the runtime lock (harmless in init); a host-memory region would make seed stores free but copies would run at 21 GB/s (not tried).

## §K8 — init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12)

Task K8 (OPPORTUNITIES B.3), branch `k8` off main a448d73, not merged. aac6, one node, one run per arm, `ECALC_INIT_TL=1 ECALC_VERBOSE=2`, all VERIFY OK. **Result: diagnosis only, no switch proposed; code unchanged** (the two patches, `RNS_HOOK_CONCUR` in `rns_mul.c` and `ECALC_VMM_FILL` in `dbig.c`, are in `archive:results_merged_2026-10-19/`'s original of this item).

**Where the time goes (m, init timeline; 6.441e10 default, two runs):** staging + contexts 1.1–2.6 s; plane pools (103 GB, hipMalloc + memset, 4 APUs in parallel) 4.1–6.0 s; region arenas parity-0 half (VMM, 39 chunks, 78 GiB) 7.0 s; peer access 0.1–0.3 s; init 14.1–14.5 s; background mapping of the parity-1 halves (45 chunks, one at a time under `g_vmm_map_mx`) 9.4–11.5 s with 4–8 s queued per APU; seed thread 18.7 s (buffers 1.5, spans 11.8, DMA issue 3.1); bs waits for the seeds 10.2–11.5 s; level 1 starts 24.8–26.5 s; total 73.0–74.7 s. At 1e10: init 8.3–10.4, level 1 11.3–13.8 s, total 23.2–25.6 s.
What binds: (1) **seeds and background mapping finish together** (6.441e10: last mapper 25.9 s, seed thread 26.2 s); level 1 needs both. (2) The seeds' DMA issue time is contention: `hipMemcpyAsync` / the copy kernel block while another thread is in a HIP memory call: 3.1 s with the mappers, 0.24 s with `ECALC_VMM_BG=0`; with `RNS_PLANES_FIRST=0` 10.1 s (one issue 8.3 s behind the plane pools' hipMalloc/hipMemset). (3) The mapping rate is driver-bound and does not scale with threads: VMM arenas ≈ 11–13 GiB/s with four APUs in parallel, ≈ 8 GiB/s for the one-at-a-time background mappers; hipMalloc for planes 18–25 GB/s; the window maps ≈ 159 GB (1e10) or ≈ 280 GB (6.441e10). Modelled floor at 6.441e10: ≈ 20–21 s before level 1 vs 24.8–26.5 s measured (4–5 s window, only if the seeds finish without the HIP runtime).

**Levers tried (switches off by default; 6.441e10 level-1 start vs default 26.46 / 24.78 s, m):**

| lever | level 1 | total | verdict |
|---|---|---|---|
| `RNS_HOOK_CONCUR=1` (seed hook in its own thread concurrent with plane pools) | 37.98 s (+11.5; at 1e10 −0.6, noise) | 96.3 s (+21.6) | rejected: hipMemCreate waits behind the planes' hipMalloc (arena creates 5–8 s), seeds stall 6.2 s then DMA issue 9.7 s |
| `RNS_PLANES_FIRST=0` | 28.96 (+2.5) | 77.5 | worse (planes 14.7 s under seed DMAs) |
| `ECALC_VMM_FILL=1` (finished APU maps more chunks while another still maps) | 28.13 / 29.04 (+3.4 / +4.3) | 76.2 / 77.3 | rejected: driver does not map faster in parallel, parity-0 phase 7 → 12 s |
| `DB_POOL_VMM_CHUNK_GB=8` | 28.43 (+2.0) | 77.2 | worse |
| `ECALC_VMM_BG=0` | 30.06 (+3.6) | 77.5 | worse (seeds wait 7.75 s for regions, DMA issue 0.24 s) |
| `DB_POOL_VMM=0` | 30.56 (+4.1) | 79.3 | worse (synchronous hipMalloc arenas ≈ 20 GB/s, seeds wait 8.8 s) |

Differences 2.0–11.5 s vs default spread 1.7 s: verdicts hold without ABBA. What could still work (not built): (1) seed DMAs that need no HIP runtime call (worth ≈ 4–5 s at 6.441e10, modelled; led to §K8b); (2) fewer bytes mapped before level 1, lazy parity-1 mapping (interacts with `vmm_bg_map`, do after G1). K8 as a reordering lever is closed.

## §K8b — seed transfers without the HIP runtime inside init's mapping window (2026-10-12)

Task K8b, branch `k8b` off main a448d73, not merged. aac6, ROCm 7.2.4.

**Probe (`tools/k8b_probe.cpp`, standalone: four mapper threads, 48 chunks of 2 GiB under a mutex, plus a main thread playing the seed thread with 8 × 8 GiB transfers; m, hold 22990):** (1) with the mappers running, `k_copy_h2d` launches block 0.69 s in all (first launch per APU 0.13–0.21 s, later 0) vs 0.007 s without; `hipMemcpyAsync` 0.3 s issue (1.2 s wait, ≈ 55 GB/s vs ≈ 110 GB/s for kernels), `hipMemcpy` 1.4 s; CPU stores into hipMalloc memory 0.37 GB/s (not a path); (2) **the mapping rate is 9.3–10.0 GiB/s whatever the seed thread does**, even with four mappers in parallel (9.0 GiB/s), so the 90 GiB parity-1 half is a floor of ≈ 9.5–11 s after its release at 14.5 s and level 1 cannot start before ≈ 24–25.5 s at 6.441e10 (modelled; K8 default 24.8–26.5 s); (3) a persistent pull kernel (`pull`) makes transfers free of runtime calls (issue 0.000 s, content verified; its four launches cost 0.38 s even under the mappers); (4) registering two 8 GiB buffers costs 1.25 s with or without mappers (not contention); without the mutex every launch blocks (2.64 s), which is why `g_vmm_map_mx` exists.

**Switch `BS_SEED_PULL=1` (off by default).** `dbig.c` `k_pull`, `db_pull_*`; `binsplit.c seeds_stream` uses them on VMM arenas; kernels launched right after `seed_stream_pools`. Same bytes to the same addresses. Modelled gain at 6.441e10: seed thread ends ≈ 3–3.5 s earlier but level 1 then waits for APU2's parity-1 half (25.38 s), so level 1 moves ≈ 0.5–1 s, not 4–5 s.

**Gate and ABBA.** Gate (1e9, switch on) PASS (VERIFY OK, identical); the first ABBA never ran (driver bug: `grep "^PASS"` vs line `gate PASS`). **K8c, second ABBA (hold 23008, commit 228aa93, m): the switch-on arm hung 2 of 2** (killed at 600 s; the off arm passed: level 1 27.65 / 29.29 s, total 76.62 / 78.69 s). Cause: one descriptor per APU; two buffered seed chunks go to the same APU back to back (`dev = r % nd`), the second `db_pull_copy_async` overwrote the descriptor and `seq` while blocks were still copying, so `done` stayed below 2 × 456 and `db_pull_wait` spun forever (neither probe nor 1e9 gate had two in flight per APU). **Fix b1ff573:** `db_pull_copy_async` first calls `db_pull_wait(dev)`; off path unchanged. A re-armed run (hold 23023, 5 switch-on runs at 6.441e10 then ABBA) was armed on 2026-10-12; **no result is recorded in this file, so `BS_SEED_PULL` has no timing result and stays unadopted** (expected ≤ 1 s, modelled). The only lever left in the window is the mapping itself (lazy parity-1 mapping or an earlier background start); parallel mappers do not help.

## §P3 — shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED)

Nothing built or run. Starting point (m, archive:results/A09.md, 6.441e10 per node): GPUs idle until level 1 at 24.4 / 28.3 / 29.4 / 35.1 s on 1 / 2 / 4 / 6 nodes (32 / 20 / 16 / 12 % of the run); at 1 node the seed thread binds (ends 24.0 s, last background chunk 23.0 s), at 2 and 4 nodes the mapping binds by 1.4–2.5 s; any single fix is worth ≤ ≈ 1.5 s, both ≈ 3 s (modelled). By code reading, overlapping mapping with CPU binary splitting, lazy mapping per pool, bigger chunks (`DB_POOL_VMM_CHUNK_GB` exists; per-GB cost flat), planes overlapped with arenas (unsafe: late planes get small pages, N414) and GPU work on already-mapped pools are already done, ~0, unsafe or not small. The only candidate (publish parity-0 chunks one by one so seed DMAs start early, A09 6.3; switch `ECALC_VMM_PUBLISH`, not started) saves ≈ 1.0–1.5 s of a 76 s run at 1 node (modelled), ~0 at 2/4/6 nodes, adds concurrent mapping and launch traffic where the hipMemMap crash and the lock-order deadlock lived, and at the target the mapping is expected 7× faster (assumed, S22 3.1) so the seed thread does not wait. **Stopped; nothing pending.**

## §T114 — Phase 14 agent T1: E10a, the early free in the multi-node tree (`MN_TREE_EARLY_FREE`)

Branch `p14-T1` (on `p14-R1v` 5dbab07 = main + DM_TIGHT + DB_POOL_VMM). Free the dead P_i / P_run shares between a tree level's two products (APUMULT_STUDY §4 E10); `tree_need_dev` counts max(2c + 2r + n, c + r + 2n) instead of 2c + 2r + 2n; behind `MN_TREE_EARLY_FREE=1` (off by default).

**Design.** `mn.c tree_level_k` (k-way, Horner from the top child) and `tree_level` (binary, r = 0): after the first product P_n = P_i Q_run + P_run and before Q_n = Q_i Q_run, free my child's P share and the owned running P (the second product reads only Q_i, Q_run). `binsplit.c tree_need_dev` uses the smaller live set with the switch (`mn_tree_early_free` read in `dm_switches()`); `BS_LAYOUT_ONLY` prints `early_free 0|1`; `mem_model.py` takes `early_free` (`--e10a` prints the 576 ceilings); one README row.

**Modelled 576 per-node ceilings (`mem_model.py --e10a`, target switches: SHMEM resident staging, `MDB_SHIFT_CHUNK_MB=1024`, `COMM_ALLTOALLV_DEPTH=2`; digits = 576 × D):**

| variant | early free | 480 GB per node (total) | 502 GB per node (total) |
|---|---|---|---|
| V0 | off / on | 8.09e10 (4.66e13), dm binds either way | 8.61e10 (4.96e13) |
| `DM_TIGHT=1` | off | 8.73e10 (5.03e13), tree binds | 9.34e10 (5.38e13) |
| **`DM_TIGHT=1`** | **on** | **9.39e10 (5.41e13)**, dm binds | **1.000e11 (5.76e13)** |
| `DM_TIGHT=1 MN_T_CHUNK_MB=1024` | off / on | 9.86e10 / 1.064e11 (5.68e13 / 6.13e13) | 1.054e11 / 1.133e11 |

With DM_TIGHT the early free is worth +7.6 % (480) / +7.1 % (502) per node; without DM_TIGHT the dm need binds and it buys nothing.

**Tests.** Layout check (login node, C request vs model): **0 terms not exact** for all four `DM_TIGHT` × `MN_TREE_EARLY_FREE` combinations (24 points × 11 terms; a `--check-c` regex bug fixed). Tree need per device falls 13–15 % (7.38e10: 73.58 → 63.78 GB at 576); the arena moves only where the tree binds, at 576 under DM_TIGHT: 294.3 → 273.3 GB (7.38e10), 336.8 → 309.4 GB (8.5e10). Correctness (job 21218, s24-16, m): 1e10 at size 4 (binary and `MN_GROUPS=4`), 1e9 at sizes 2, 3 and `MN_GROUPS=3`, all with the switch: **identical**, every node VERIFY OK; `MN_TREE_EARLY_FREE=1 mnaccept --only unit,mn` **13 passed 0 failed**. Per-product windows (job 21222, binary at 33b9b04, m): every MUL2 window drops by the freed P shares (0.14–0.47 GB per APU at 1e10 / size 4, e.g. L1 node 3 2.27 → 1.98 GB, L2 node 3 2.23 → 1.95; live after MUL1 0.85 → 0.57); MUL1 windows unchanged to 10 MB (nothing dead yet); the level peak stays in MUL1 at this size, so the whole-level peak does not move (the formula's bound stays an upper bound, MUL2 falling to c + r + 2n and the bound becoming MUL1's 2c + 2r + n: 5.33 → 4.33 shares at 576's 3-way levels). `mnaccept --only unit,mn` without the switch (job 21222): 12 passed, 1 failed (`mn e8 size 4`): node 3's bs leaf came out with a garbage size (`leaf P 18385101070989787659 limbs`) before any tree code ran, not the switch's; the same test passed with the switch on and in the integrator's regressions: the open intermittent leaf-level fault (Phase 10/11 notes), not reproduced.

**Verdict:** gates met (11 mnrun runs identical, mnaccept 13/13 with the switch, layout exact); measured MUL2 drop equals the drop the formula takes; the adoption of `MN_TREE_EARLY_FREE` (off by default) is open, worth +7 % per node at 576 only together with DM_TIGHT (modelled). Open: the intermittent leaf fault of the 12/13 run (not T1's).

## §L114 — L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`)

Branch `p14-L1` from main @ ae1d872. Files: `binsplit.c` (dm_layout, device-tier top levels, layout report), `binsplit.h`, `newton_db.c` (`recip_db2` reservations only), `dbig.c/.h` (`db_pool_retarget_tail`), `mem_model.py`, README (two switch rows). Switches off by default. Labels: measured / modelled (`mem_model.py` = `dm_layout` to the byte) / assumed.

**`DM_TIGHT=1` (E2).** Reciprocal (`recip_db2`): r, r2, t1 reserved once after the seed at the last doubling's sizes (jl = ⌈k/2⌉ anchored: r at jl + 4, r2 at 2 jl + 4, t1 at take + jl + 2 + 8), blocks never move (r' copied from r2 into r's block at every doubling but the last); values identical, only capacities change. Last doubling live set **5.2 n_Q + piece vs 6.2 n_Q today** (measured today at 1.058 × 10¹¹: peak live 294.7 GB = 6.27 n_Q). The first form (t1 freed and re-reserved per doubling) churned and fell back to hipMalloc at 4e10. Block pool (`ext_take`, `db_pool_pack_large`): under DM_TIGHT large requests go by a packing rule (bs: highest free end outside the tail; dm: tail's back first, then highest free end), blocks may be carved from an extent's middle (`carve_at`). Device top levels free each pair's P1, P2 after P = P1·Q2 + P2 and Q1, Q2 after Q = Q1·Q2 (top level then 3.3 n_Q instead of 4.4 n_Q; skipped under `ECALC_RES_LOG`). `dm_layout`: v2 = 2Q + r(jl+4) + r2(2jl+4) + hole + piece; new division term div = 2Q + max(µ(k+1) + t(2k+8), X(k) + xq(n_Q+k+8)) + piece = 5.2 n_Q + piece (never binds by default, floor once the reciprocal is tight); v3 = max(4 in + out, 2 in + 2 out) · 9/8 + hole.

**`DM_TAIL_DEAD` (E5's layout half).** `=1`: v3 without the hole; after the top level the pool's tail is retargeted to the back of each APU's largest free extent (`db_pool_retarget_tail`; `SHORT` falls back to in-phase hipMalloc); safe on its own. `=2`: also v2 without P (P on disk): needs a spill, `binsplit_pregrow` refuses unless `dm_p_spill_wired` or `ECALC_INIT_ONLY=1`.

**Layout check.** `mem_model.py --check-c` vs `BS_LAYOUT_ONLY`, six variants × six points (36 `layout:` lines): **every term exact, 0 terms not exact**; default arena byte-identical to the pre-change layout (1e11: 310.29 GB; 576 share 309.21 GB). At 1e11 DM_TIGHT: v2 63.41 G per device, division 62.07 G, arena 258.63 GB node (V0 310.29).

**Modelled savings (`--savings`, node peak GB):** V0 → `DM_TIGHT=1`: 4e10 299.6 → 281.7 (−17.9); 1e11 427.9 → **376.3 (−51.7)**; 1.3e11 522.0 → 441.9; 1.68e11 641.0 → 531.3; 7.38e10 × 576 451.7 → **428.2 (−23.5)** (the tree's 294 GB then binds). `DM_TAIL_DEAD=1` alone: 1e11 −11.9. **Ceilings (modelled):** one node 502 / 524 GB: V0 1.235e11 / 1.305e11 (measured 1.30, §P13b) → **`DM_TIGHT=1` 1.554e11 / 1.648e11 (+26 %)**; 576 per node 480 / 502 GB: 8.09e10 / 8.61e10 → 8.73e10 / 9.34e10 (+8 %), total 5.03e13 / 5.38e13.

**Finding: E5 (P spilled) gives ≈ 0 over E2 until E6.** With the reciprocal tight the division's own set (5.2 n_Q + piece) is the floor: `DM_TAIL_DEAD=2` moves the 1e11 node by −0.8 GB (+0.5 % ceiling) over DM_TIGHT alone; the whole spill programme (E5 + E6, two exposed disk reads) would buy 4–9 GB (1e11 node 372.2 vs 376.3) because the arena is then the bs regions' own size (≈ 5.5 n_Q); the study's 1.30 → 1.68 × 10¹¹ is mostly E2's gain, available without disk. The next lever would be the batch tier's ring layout (F2's `BATCH_PAIR_INMADV` row). For the 576 share none of the dm variants matters beyond the first 23.5 GB.

**Node tests (aac6, m; every run digit-identical / VERIFY OK):**
- Batch 1 (job 21131, df36cef): `t_newton 20` ±tight OK; e9 decimal / binary and forced device-tier top levels OK; **4e10 tight 62.16 s** (baseline 63.5 ± 1.5, RESULTS §80: no time cost), arena 114.4 GB (132.3 before), but one in-phase hipMalloc (4.44 GB, r2 at the last doubling found no extent: placement, not bytes); init-only 1e11 tight device 362.3 GB = modelled to 0.01 GB (arenas 258.6 GB in 10.96 s). Fix 47ced5f (packing rule).
- Batches 2, 3 (jobs 21132, 21134, 47ced5f, 9f04815): 4e10 tight 63.36 / **61.17 s**, `DM_TAIL_DEAD=1` 62.86 / 63.47; 1e11 tight 308.56 s (22 GB in-phase hipMalloc) / **253.99 s (dm 139.1)**; remaining fallbacks came from the division (t = 2 n_Q contiguous; window moment S + Q + X + xq + Aw = 6.2 n_Q), motivating the third form (tail = t's block holding r + t1; Aw before the low product, S freed there).
- Batch 4 (job 21151, 540a20d): 4e10 default 63.01 s / tight 63.51 s (arena 114.4 vs 132.3 GB); 1e11 default 235.83 s (arena 310.3) / tight 237.15 s (arena 259.9, −50.4 GB; **reciprocal 45.63 vs 62.05 s**, 22.22 GB fallback in the division on 2 APUs); 1.4e11 tight 2³¹: reciprocal clean (peak live 326.7 GB = 5.25 n_Q), but t (31.1 GB) found the tail 2.6 GB short and `mem_oom` exited (TASKS A3). Cause of the remaining reciprocal fallback: the last doubling's shift `r >>= 1` went into the small r2 block; fixed in 23a7f43.
- Batch 5 (job 21154, 23a7f43): 4e10 default 63.54 s, tight 63.62 s (recip 9.61, reciprocal clean); one fallback left: division's t (8.89 GB) found the tail's free part 8.78 of 9.03 GB because 7 small blocks spilled into the tail; fix 916fbae (r reserved inside the tail below t1, `db_pool_pin_tail`). Batch 6 (`l114_node6.sh`) was the gate run for it; **its results are not recorded in this file**, so the E2 gate (hipMalloc 0 in the phase at 4e10 / 1e11 / 1.4e11) is not shown as met here.

Open issues: the tight schedule assumes the last doubling converges at once (every run to 1.25 × 10¹¹ shows `corrections 0/0`); per-doubling fragmentation to be read from `DB_POOL_VERBOSE` at 4e10 and 1.4–1.6 × 10¹¹; the size > 1 formula keeps the size-1 shape over the shares (sharded reciprocal's live set not re-derived).

### §ATRIM — BS_ARENA_TRIM_GB (trim the dm arena need; merged 2026-10-23 from ATRIM.md)

**Question.** Does trimming the dm layout's arena need by a fixed number of GB per APU save node memory without costing time or remaps?
**Change / branch.** Branch `atrim`; switch `BS_ARENA_TRIM_GB`, default 0 (off). Subtracted at the end of `dm_layout`, clamped at `max(v2, v3)`; rank 0 prints the decision; twin `mem_model.py arena_trim`.
**Method.** aac7, `BS_ARENA_TRIM_GB=8` (TR8) against the switch off (S); 2 n at 1.2882e11 digits, 3 n at 1.9323e11 digits; `DB_POOL_VERBOSE=1` for remaps.
**Results.**

| size | arm | arena per node (GB) | time (s) | runs | remaps |
|---|---|---|---|---|---|
| 2 n 1.2882e11 | S | 266.3 (m) | 115.2 (m) | 7 | 0 |
| 2 n 1.2882e11 | TR8 | 231.9 (m), -34.4 | 111.4 (m) | 5 | 0 |
| 3 n 1.9323e11 | S | - | 149.4 (m) | - | 0 |
| 3 n 1.9323e11 | TR8 | - | 149.6 (m) | - | 0 |

**Verdict.** Needs more data: memory saving is large at 2 n and time is neutral (3 n) to slightly better (2 n); the digits gate (item 9lu_atrim_gate: 1 n and 2 n, SHMEM and libfabric) decides whether it stays opt-in.
**Open issues.** No adoption decision; gate result pending; no 4 n / 6 n data.
**RESUME.** Branch `atrim` merged with main eb0c5f0; next: collect 9lu_atrim_gate.
