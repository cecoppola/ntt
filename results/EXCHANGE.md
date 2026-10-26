# EXCHANGE — the all-to-all exchange in every transport

Scope: the all-to-all exchange in every transport: all-gather, layered exchange two-deep, slab pipelining, the rank gradient, gather kernels. Condensed 2026-10-19 from the per-item reports (originals: `archive:results_merged_2026-10-19/`; raw data and drivers: `archive:results_raw_2026-10-19/`). Old item IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: m measured, mod modelled, a assumed.

Sections moved to `archive:results_merged_2026-10-24/EXCHANGE.md` (2026-10-24 cleanup): A-comm; C; X13b.

## Topic summary

What this is: how data moves between ranks (APUs over xGMI, nodes over the fabric, the layered combination) and how that movement is hidden under compute, plus the gather kernels that feed the single-node product.

Current state (cross-checked with `ecalc/README.md` and `RESULTS.md`):
- Built in, no switch: all-gather and `alltoallv` in every transport (§A-comm, §C); the K-chunk slab pipeline (`DIST_CHUNKS`, now default **8**, was 4 when §A-comm was written; README row owns it); the push kernel with 64-bit remote stores and 76 blocks per peer (§C: 2^31 conv 0.1737 -> 0.1577 s, -9 %, m); the layered v-exchange two deep, `COMM_ALLTOALLV_DEPTH=2` (§X13b; RESULTS lists it as built in; xGMI link time hidden on the general map 1.4 -> 74 %, m, two real nodes included).
- Adopted by the user: `DIST_GATHER_RAW=2` (default since 2026-10-12): -3.8 s of ~73 s at 1 node (m), ~-1.0 s of leaf mdev at 2 nodes (m, below the noise of the whole run) (§K1b).
- Measured, not adopted, switch or code removed: two-plane pipeline `dist_fwd2/inv2` (3-6 % slower, §C C4); `DIST_GATHER_TILE` (-0.08 +- 0.88 s, §S50); `RNS_DIST_OVERLAP` (-0.11 +- 0.15 s of 73 s, §K6); rebalancing the result-exchange gradient (NO-GO, ~2 % mod, §A07). §P2 closes the k_gather line: the floor is the remote read (~24 ms of a 35 ms call), <= 0.6 s per run headroom.
- `DIST_TWREC` (twiddle-pack by row recurrence, §X13b): bit-identical, kernels x1.4-1.65, no end-to-end effect at size 1; later adopted (KERNELS.md owns it, see its `DIST_TWREC_G` item). `COMM_LAYER_TKERNEL` (one-kernel transposes): 662x at g = 576 for small blocks (m, kernel alone), needed at scale.
- Live conclusions: (1) one push kernel drives all three xGMI links at once, 244 GB/s per APU (m) vs 273 assumed in E9; (2) the host-visible xGMI stage is 40-100x the link time, so overlap must be counted at link level; (3) the pow2 path hides 3/4 of the xGMI link time (structural ceiling with K = 4), depth 2 gives the general map the same; (4) one inter exchange in flight does not bind while the fabric is the bottleneck (fabric idle while held <= 2 %); (5) XGMI-pipeline gains at 2^31 are small (2.5-5 %) because the exchange is HBM/CU bound against the row pass; (6) kernel-level tweaks of the gather are below what ABBA can resolve; the real lever is layout (avoid the 3/4 remote pull).
- Superseded: A-comm's "default 4 chunks" (now 8); C's push defaults of 228 blocks / 16 B (now 76 / 64-bit); X13 §5 part 2 ("pipeline the v-exchange") is done in X13b.

## X13 — xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a

Branch `p13-X` from `main` 203733b. Files: `comm_layered.c`, `comm_xgmi.c`, `t_dist.c`. Defaults unchanged: everything is behind `COMM_LAYER_STATS`, `COMM_XGMI_STATS`, `DIST_H4`, `DIST_LBENCH` (off). The 3-node slot was never available (admin CI array, then a node down).

#### 1. What was built
- `COMM_LAYER_STATS=1|2` (`comm_layered.c`; removed 2026-10-19, D56): per exchange host stamps for the xGMI stage, ready-for-fabric, fabric stage, complete. Mode 1 passive (upper bound of fabric completion), mode 2 exact (watcher thread; perturbs slightly: use 1 for timings). Report at exit: span, xGMI-busy, fabric-busy, both-busy, held (ready exchange waiting for the previous inter stage), fabric idle while held, NIC balance, node view. With `COMM_XGMI_STATS=1` also the link-active interval of each push kernel, giving **both-busy at link level**.
- `COMM_XGMI_STATS=1` (`comm_xgmi.c`; removed 2026-10-19, D56, the `comm_xgmi_h4()` bench stays): `k_push3_ts` stamps each block; per sender-peer bytes and link-active time. `comm_xgmi_h4()` benchmark with nine modes.
- Drivers: `DIST_H4=1 [DIST_H4_MB=256] ./tests/t_dist`; `DIST_LAYERED=1 DIST_LBENCH=24,26,28 [DIST_LREPS] mnrun.sh <g> ./tests/t_dist`.

#### 2. H4 — does the push saturate all three K4 links? (m, one node)
256 MiB per (sender, peer), 10 reps:

| form | per APU (wall) | per link (stamped) |
|---|---|---|
| one link 0->1 | 86.8 GB/s | 89.4 |
| one link both directions | 84.6 each (169.2 node) | 87.5 / 87.8 |
| fan-out APU 0 -> 3 peers | **253.5** | 86-91 |
| **all-to-all push (default 76 blocks/peer)** | **244.0 (node 976.2)** | 82.7-89.3 |
| contiguous thirds of the grid per peer | 249.9 (+2.4 %) | 85.0-85.7 |
| 3 kernels on 3 streams | 237.9 (-2.5 %) | |
| `hipMemcpyAsync` on 3 streams (copy engines) | 134.8 (-45 %) | |
| 3 rounds, one link at a time | 86.7 (= 1 link) | |

Blocks per peer 19 / 38 / 76 / 152 / 304: 230.8 / 242.2 / 244.0 / 243.7 / 242.0 GB/s (flat from 38). At 32 MiB per pair 212 GB/s per APU.
**Verdict H4: yes.** One kernel drives all three links concurrently at 93-97 % of the single-link rate, sending and receiving at once; nothing beats it by more than 2.4 %. Each APU pair has two xGMI ports (128 GB/s raw per direction per pair); the push reaches 70 % of raw. **E9's assumed 273 GB/s per APU is 244 measured (-11 %).**
In the 4 x 10^10 pipeline (m, one node): 962 GB per APU in 3252 pushes, 4.2-4.3 s of push kernel = **221-230 GB/s per APU, 74-80 GB/s on each link at once** (84-91 % of the one-link rate); stamps cost nothing measurable (81.64 s with, 81.57 s without, one run each, digits identical). In small-exchange multi-process runs the links run below bandwidth (pushes of 0.6-7.9 MB: 15-58 GB/s per link); at the target's top levels pushes are hundreds of MB.

#### 3. E9 part 1 — the present overlap
**3.1 The stage is not the link (m, loopback).** The host-seen xGMI stage (intra post to intra wait return) is **40-100x** the link time (2^24-2^26 lbench: 0.037-0.17 s per APU vs 0.0006-0.0017 s of push kernel, 1-2.4 %): it includes the wait for the producer pack, the four-thread barrier, launch and sync. Overlap must therefore be counted at link level.

**3.2 Two node-processes on one node (loopback TCP, m).**

| run | path | xGMI link time hidden under fabric | stage both/xGMI | fabric busy/span | held (fabric idle while held) |
|---|---|---|---|---|---|
| lbench 2^24 | pow2 (pipelined) | **74.9-75.5 %** | 60-66 % | 98.9 % | 0.077-0.083 s (0.4 % of fabric) |
| lbench 2^26 | pow2 | **73.9-76.0 %** | 63-66 % | 99.3 % | 0.24-0.27 s (0.15 %) |
| ecalc 10^8 size 2 (identical) | pow2 | **75.1 %** (node view 70.5) | 60 % | 93.5 % | 0.94 s (1.1 %) |
| same, `DIST_GEN=1` | **general map** (alltoallv) | **1.1 %** (node view 1.2) | 3 % | 91.2 % | 0.009 s |

- **Structural ceiling of the pow2 path: 3/4.** With K = 4 and two in flight, chunk 0's predecessor is the previous transform's last chunk, already waited for by `dist_*_post`; so 3 of 4 pushes can hide and 74-76 % is measured.
- **The general map collected nothing**: `y_alltoallv` completed the previous v-exchange before its own intra stage. The general map is what every non-power-of-two group runs (3, 6, 192, **576** nodes: at the target the two top tree levels, the reciprocal's and division's full-machine products).
- **One-at-a-time does not bind** while the fabric is the bottleneck: held time is ~45 % of fabric time but the fabric was still busy with the predecessor in all but 0.15-1.1 % of it (node view fabric busy 93-99 %); a second inter exchange would only recover those gaps.

**3.2b Three node-processes on one node (m, ecalc 10^8 size 3, identical).** Level 1 pairs nodes 0-1 (pow2, g = 2); top level and division run over all three (general map). Nodes 0 / 1: 22.6 % / 22.2 % hidden passive (18.3 / 18.7 exact), fabric busy 90-91 %; **node 2 (g = 3 only): 0.0 %**. Whole-run fraction ~20 % (the pairwise 75 % diluted by 0 %). NIC balance: the four APU threads send identical bytes (max/min 1.0000, m); fabric time differs 1-3 %.

**3.3 Two and three real nodes: modelled only** (no allocation possible). 2 real nodes, pow2 path over 1 GbE (0.117 GB/s per node, m link speed): both/xGMI-link ~75 % (mod), fabric busy/span >= 95 %. 3 real nodes: general-map levels ~0 % (mod; 1.1 % measured for the same path at size 2), level-1 pair ~75 %; by bytes the g = 3 products dominate, so well under a quarter of the run's xGMI link time hides (mod). General map imbalance: parts are floor/ceil of R/nr rows, <= 3 % on small shapes, ~0 at the target (mod).

#### 4. The overlap ceiling against the §29 E9 estimate (mod)
E9 assumed T_intra/T_inter = 0.37 at 576 (ceiling 27 % of the sequential exchange time). With 244 GB/s and an APU sending 3/4 of its slabs over xGMI: ratio = 0.75 x 100/244 x g/(g-1) = **0.31** at 576 (0.41 at g = 4) -> **ceiling 23.5 % at 576** (29 % at g = 4). The pow2 path already collects 3/4 (measured): present saving **17.8 %** of the sequential exchange time, **5.9 %** left (the chunk-0 push). The general map collected 0 (m 1 %): all 23.5 % remained there.

#### 5. Verdict — are E9 parts 2 and 3 worth writing?
- **Part 2: yes, but the general map's v-exchange, which never overlapped** (not the one-at-a-time inter stage, not the pow2 granularity): give the v-exchange the equal one's two-slot pipeline; up to 23.5 % of exchange time on those levels (mod). Done in §X13b.
- The pow2 path's remaining 1/K (5.9 % at K = 4) can be cut by `DIST_CHUNKS=8` before any code (default is now 8).
- More than one inter exchange in flight: not worth it (<= 1 %).
- **Part 3 (xGMI as relief valve for NIC imbalance): not worth writing** (imbalance <= one row in R/nr, ~0 at target; not measured on real nodes).

Gate: `mnaccept.sh --only unit,e9,mn` on `p13-X` 59ebca79: 15 passed, 0 failed, 1310 s (m). Open at the time: real 2- and 3-node measurements (done for 2 nodes in §X13b 2.1a); block transposes are g x 4 `hipMemcpyAsync` per direction (0.1-0.27 ms per exchange at g = 2-3, m; 2304 launches at g = 576, mod: one-kernel transpose needed, done in §X13b).

## A07 (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products

CPU only, existing logs, no cluster job, no engine change; 2026-10-11.

Question: why do ranks wait unequally in the result exchange of the dm / bs products, and is a rebalancing fix worth building?

Answer (m correlation + code read; the causal chain is inferred, not isolated):
- **The gradient is still there in today's code** at every size (2, 4, 6 nodes), reproducible to +-0.15 s (9 runs, 4 nodes).
- **Cause:** equal work per rank but unequal received result volume. Each rank receives the part of the product falling in its own equal share of accumulator C (`mdb_share`); the pieces of the grid (`mn_grid`, ka x kb at shift oa+ob) overlap most in the middle of C (triangular coverage 1,3,5,7,7,5,3,1 for k = 4, peak/mean 1.75), so middle ranks receive up to 1.3-1.55x the mean and end ranks ~0.6x (dm products are truncated, which moves the peak). Across 8 rank-points per size the result-exchange wait is **inversely proportional to received volume** (Pearson r = -0.96 .. -1.00 at 4 and 6 nodes): light receivers finish their scatter/add earlier and wait for heavy ones at the next exchange (`roff`).
- Not the cause: work split, exchange order (S27 rotated peer order gave the same shape at 10 nodes), rank-0 duties.

Summary (m; wait per APU thread = thread-sum / 4):

| nodes | phase | max/mean recv | rank-0 wait / min wait | mean wait | corr(wait, GB) |
|---|---|---|---|---|---|
| 2 | dm | 1.31 | 1.32 | 2.6 s | -1.00 (2 points) |
| 4 | dm | 1.55 | 1.45 | 5.8 s | -1.00 |
| 4 | bs | 1.31 | 1.18 | 4.2 s | -0.99 |
| 6 | dm | 1.51 | 1.27 | 15.6 s | -0.99 |
| 6 | bs | 1.31 | 1.17 | 8.6 s | -0.96 |
| 10 (S27, old code) | dm / bs | n/a | 2.4 / ~1.9 | 22.3 / 12.0 s | n/a |

Example 4 nodes dm: GB received per node 43 / 200 / 272 / 186, wait 7.1 / 5.7 / 4.9 / 5.6 s (9 runs: +-0.12-0.15 s). Walls: 2 n 144 s, 4 n 182 s, 6 n 283-304 s (m). The gradient ratio grows with n (1.27 at 6 nodes vs "twice" at 10 nodes in S27).

Modelled payoff (upper bound; the heaviest rank is the critical one, cost per GB = (wait - roff)/GB there): 4 n 2.1 s of 182 s (1.2 %), 6 n 5.0 s of ~293 s (1.7 %), each 21 % of the result-exchange wait; **10 n: ~-7 s (range -5..-10), ~2 % of wall** (mod), consistent with S27's -5..-12 (a); 576 n: 2-3 % of wall if the exchange share reaches 10-15 % and the ratio stays 1.3-1.5 (a). Caveat from S27: time saved by reducing transfer time elsewhere was absorbed as straggler waiting (0 on wall).

**Recommendation: NO-GO now** (gain ~2 % mod, below single-run noise sd 8-10 s and below what an ABBA resolves in < 8 rounds at 10 nodes; any fix touches the layout of C used by carries, node scans and digit conversion). Revisit if a 20+ node XSTATS run shows max/mean recv above 1.6 or the result exchange above 15 % of wall.
If wanted later: (1) weighted shares in `mdb_share` (needs a re-layout exchange between products, probably cancels the gain; not recommended); (2) better, shrink the volume: where pa = pb, sum the transform-domain products of one anti-diagonal before the inverse transform and CRT (needs a CRT bound check, `P24` limit 10^60; larger project, unevaluated idea); (3) cheap probe first: a per-round arrival timestamp in `COMM_XSTATS` for `XK_RESULT` plus one 10-node run.

## K1b: raw-once gather (S39 candidate A, O7b lever 3) -- `DIST_GATHER_RAW`

Branch k1b from main a448d73. Status now: **adopted, default 2** since 2026-10-12 (README row owns the semantics; `results/SINGLE_NODE.md §CAND`).

#### 1. Question
O7b (`archive:results/O7b.md`): `k_gather` is the largest kernel at 6.441e10 on one node (6.34 s APU0, 174 calls x 36.5 ms per APU, 15 % of GPU time, m) plus `k_bload` 1.2 s. S39 candidate A: gather each operand once without the canon, then canonicalise per prime with a local elementwise pass; O7b modelled 2.5-3.6 s. S40 (aac7) found the gather is not bound by the remote pull (remote 36.1 ms vs local 35.9 ms with the select kernel, m), which only removes S39's reason; raw-once saves whenever the gather costs much more than a coalesced canon pass, whatever bounds it. S50's tile kernel bought only 5 ms of 35 ms, so the store alone is not the bound either.

#### 2-3. Probe and implementation
Micro-test `tests/t_gather_raw_probe.c`: V0 gather + canon (product), V1 raw gather, V2/V3 canon pass plane->plane / in place, V4 pow2 index, V5 local source; shapes rows 8192 x C 65536 and 1024 x C 524288; all variants bit-identical. Stop rule: continue only if modelled saving = 174 x 36.5 ms x (1 - RATIO4) >= 1.5 s.
Implementation (`rns_dist.c`, `dist_core` only: the single-node C-form product; the multi-node tier's `k_gather_mn24_np` already reads once for all planes): `k_gather_raw<QS>`, `k_canon_pass`, `gather_raw_all()`. `=1`: A through its own plane xa[np-1], B through its cache plane when it has one (no extra memory). `=2`: additionally B without a cache plane through one extra q-limb plane (4.29 GB per APU at 2^29 points, mod) taken only from the block pool's free extents (no growth, no hipMalloc) and only if `DIST_GATHER_RAW_MARGIN_GB` (default 2) stays free; else B per prime. Digits identical by construction. `k_bload` unchanged.

#### 5. Results (aac6, one node, m)
- Probe: RATIO4 0.3388 / 0.3408; V1 raw gather 34.26 ms vs V0 34.45 ms (RAWREL 0.9944); canon pass alone 3.01 ms; modelled saving 4.20 s per run at 6.441e10.
- Gates (1 node, 1e9, `RNS_STRATEGY=C`): `=2`, `=1`, `=0` all PASS (digits identical).
- ABBA at 6.441e10 (A = 0, B = 2), 3 rounds, all 12 runs VERIFY OK: deltas B-A -3.36, -4.39, -3.66 s; mean **-3.805 s**, 95 % CI half-width 1.314 s (CI -5.12 .. -2.49), p < 0.05, n = 3 pairs; ~5.2 % of ~73 s (A 72.4-74.8 s, B 68.5-70.1 s), close to the modelled 4.20 s.
- Memory (node): in use max 284.1 GB both arms; block-pool peak live 145.5 GB (A) vs 162.7 GB (B) (+17.2 GB = 4 x 4.29 GB); pool hipMalloc 0.0 GB; no "does not fit" notes.
- Verdict: significant gain, digits identical; adoption was the user's call (taken).

#### 6. K1c follow-up (2026-10-12): memory margin, multi-node relevance, 2-node run
- **6.1 `=2` does not raise the device total the margin rule measures** (m at 1 node, mod at target): device 284.1 GB in use at every phase in both arms, pool 180.4 GB all borrowed from init regions; only the pool's peak live moves (bs 100.2 -> 117.4, division 145.5 -> 162.7 GB per node, +17.2 GB). The margin rule uses the layout's device total (`device_with`), not pool peak live. 2-node aac7 run at 6.441e10 per node (m): pool 53.69 GB per APU, leaf mdev window peak 25.1-25.8, tree level 1 window peak 43.4-46.4 GB per APU; the raw plane fits the leaf's free extents (~28 GB free per APU, 6.3 GB needed with the guard) and lifts the leaf peak to ~30 GB, still below the tree's 46 GB (mod). Rank-0 margin at 576 nodes (mod, L1/L4, T2048 + X2 + RELEASE): 3.71e13: 21.5 GB with `=0`, `=1`, `=2`; 3.82e13: 12.9 GB (L1; L4 13.0) with all three. Condition: no pool growth by hipMalloc.
- **6.2 Multi-node relevance:** dist_core runs at > 1 node only in the per-node leaf's `mdev` levels (and the reciprocal chain up to `NEWTON_MN_SPLIT`, 2^16 limbs, negligible); tree levels and the division use `mn_grid`/`mn_core` (unchanged). 1-node phase split of the ABBA (6 runs per arm, m): leaf mdev 8.70 -> 7.35 s (**-1.35 s**), batch 19.67 -> 19.52 s (noise), dm 19.52 -> 17.07 s (-2.45 s: the single-node division, not used multi-node). Expected at 576 nodes (mod): **~-1.4 s of 176.3 s (3.71e13) and 181.7 s (3.82e13), 0.7-0.8 %**.
- **6.4 aac7 2-node ABBA (m, 1.2882e11 digits on 2 nodes, 3 rounds, 0 FAIL, all VERIFY OK; gate PASS with SHMEM build):** total deltas -0.52, +6.46, -4.48 s: mean +0.49 s, CI half-width 13.76 s, no effect resolved (wall noise from bs 44-49 s and dm 54-68 s). Leaf mdev max over nodes: 7.20 -> 6.10, 7.20 -> 6.25, 7.15 -> 6.25 s: mean **-0.983 s, CI half-width 0.259 s** (t -16.4), -13.7 %, matching the 6.2 model. Memory identical in both arms: device in use 317.8 GB, pool peak live 201.8 GB, **pool hipMalloc 0.0 GB**, largest APU driver-used 82.2 GB, 0 not-fit notes.
- Verdict: `=2` saves ~1.0 s in the multi-node leaf at no memory cost; below the noise of the 2-node total, no whole-run claim.

## S50: k_gather coalesced write (`DIST_GATHER_TILE`) -- measured, not adopted, removed

**Do not adopt**: no measurable gain (0.0 +- 0.9 s at 1 node) and shape-dependent (tile 32 slower at rows 1024 x C 524288). Disposition: the code stays off main (archive tag `archive/s50-gather-tile`); results and drivers (`archive:drivers/s50/`) kept. A gain would need to be >= 0.5 s at 1 node to show in 3 rounds.

(Details moved to `archive:repo_cl6_2026-10-25/results/EXCHANGE.md`, 2026-10-25.)

## P2: speed up k_gather -- stopped after a micro-probe; no switch added

- Verdict: remaining headroom 35 -> ~25 ms x 58 calls = **~0.6 s per run** (mod, upper bound; S50's tile gets ~0.3 s and measured -0.08 +- 0.88 s, `results/EXCHANGE.md §S50`); four ABBA rounds resolve ~+-1 s, so no change can be shown. No switch added; no gates (no product code changed). The real lever is layout (produce the operand already in the rank's rows or fold the pull into the multiplications' all-to-all): a design change (ALGORITHM.md), proposed for the user's priority.

(Details moved to `archive:repo_cl6_2026-10-25/results/EXCHANGE.md`, 2026-10-25.)

## K6: single-node C-tier all-to-all — overlap across primes (`RNS_DIST_OVERLAP=1`) -- measured, not adopted, setting removed

- Verdict: the switch is correct (digits identical) but there is no measurable gain: -0.11 +- 0.15 s of 73 s (0.15 %), C-tier time not reduced. **Do not adopt**; the setting was removed (RESULTS, OPPORTUNITIES §K6).

(Details moved to `archive:repo_cl6_2026-10-25/results/EXCHANGE.md`, 2026-10-25.)
