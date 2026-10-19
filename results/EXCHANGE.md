# EXCHANGE — the all-to-all exchange in every transport

Scope: the all-to-all exchange in every transport: all-gather, layered exchange two-deep, slab pipelining, the rank gradient, gather kernels. Condensed 2026-10-19 from the per-item reports (originals: `archive:results_merged_2026-10-19/`; raw data and drivers: `archive:results_raw_2026-10-19/`). Old item IDs resolve through `results/INDEX.md`. Labels: m measured, mod modelled, a assumed.

| item | title |
|---|---|
| A-comm | M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19) |
| C | B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20) |
| X13 | xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a |
| X13b | the uneven exchange two deep, one-kernel transposes, faster packs (PLAN.md §31 row X, axis A) |
| A07 | (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products |
| K1b | raw-once gather (S39 candidate A, O7b lever 3), `DIST_GATHER_RAW` |
| S50 | k_gather coalesced write (`DIST_GATHER_TILE`) |
| P2 | speed up k_gather: stopped after a micro-probe |
| K6 | single-node C-tier all-to-all: overlap across primes (`RNS_DIST_OVERLAP`) |

## Topic summary

What this is: how data moves between ranks (APUs over xGMI, nodes over the fabric, the layered combination) and how that movement is hidden under compute, plus the gather kernels that feed the single-node product.

Current state (cross-checked with `ecalc/README.md` and `RESULTS.md`):
- Built in, no switch: all-gather and `alltoallv` in every transport (§A-comm, §C); the K-chunk slab pipeline (`DIST_CHUNKS`, now default **8**, was 4 when §A-comm was written; README row owns it); the push kernel with 64-bit remote stores and 76 blocks per peer (§C: 2^31 conv 0.1737 -> 0.1577 s, -9 %, m); the layered v-exchange two deep, `COMM_ALLTOALLV_DEPTH=2` (§X13b; RESULTS lists it as built in; xGMI link time hidden on the general map 1.4 -> 74 %, m, two real nodes included).
- Adopted by the user: `DIST_GATHER_RAW=2` (default since 2026-10-12): -3.8 s of ~73 s at 1 node (m), ~-1.0 s of leaf mdev at 2 nodes (m, below the noise of the whole run) (§K1b).
- Measured, not adopted, switch or code removed: two-plane pipeline `dist_fwd2/inv2` (3-6 % slower, §C C4); `DIST_GATHER_TILE` (-0.08 +- 0.88 s, §S50); `RNS_DIST_OVERLAP` (-0.11 +- 0.15 s of 73 s, §K6); rebalancing the result-exchange gradient (NO-GO, ~2 % mod, §A07). §P2 closes the k_gather line: the floor is the remote read (~24 ms of a 35 ms call), <= 0.6 s per run headroom.
- `DIST_TWREC` (twiddle-pack by row recurrence, §X13b): bit-identical, kernels x1.4-1.65, no end-to-end effect at size 1; later adopted (KERNELS.md owns it, see its `DIST_TWREC_G` item). `COMM_LAYER_TKERNEL` (one-kernel transposes): 662x at g = 576 for small blocks (m, kernel alone), needed at scale.
- Live conclusions: (1) one push kernel drives all three xGMI links at once, 244 GB/s per APU (m) vs 273 assumed in E9; (2) the host-visible xGMI stage is 40-100x the link time, so overlap must be counted at link level; (3) the pow2 path hides 3/4 of the xGMI link time (structural ceiling with K = 4), depth 2 gives the general map the same; (4) one inter exchange in flight does not bind while the fabric is the bottleneck (fabric idle while held <= 2 %); (5) XGMI-pipeline gains at 2^31 are small (2.5-5 %) because the exchange is HBM/CU bound against the row pass; (6) kernel-level tweaks of the gather are below what ABBA can resolve; the real lever is layout (avoid the 3/4 remote pull).
- Superseded: A-comm's "default 4 chunks" (now 8); C's push defaults of 228 blocks / 16 B (now 76 / 64-bit); X13 §5 part 2 ("pipeline the v-exchange") is done in X13b.

## A-comm — M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19)

Branch from `main` a75474d; PLAN.md §19; follows `results/GRID_GROUPS.md §M3` ("Open issues": the O(g) point-to-point all-gathers).

Change. `comm.h` gains `allgather_host`, `comm.inflight` (exchanges a caller may post before a wait) and `comm_allgather(_host)`; implemented per transport:
- local: memcpy; sim4: the fourth caller performs every rank's copies; xGMI: each rank pushes its block into the three peers' slots with the push kernel, barrier before and after; host variant memcpys between two barriers; TCP: receive side in a thread, device blocks staged through pinned host, host blocks < 4 KiB write-all/read-all without threads, else a sender thread per peer + receiver.
- layered (rho = g d + r): `inflight` 2 (two exchanges on the wire: the xGMI stage and transposes of k+1 run while k is on TCP; two scratch slots of 4 g x bytes). Allgather = inter first (mesh d gathers my block over the nodes into slot d), then the xGMI all-gather of the g-block slots, no transpose: 4 (g-1) bytes per node on the fabric vs 16 (g-1) intra-first.
- `mn_allgather` (mn.c) is now `comm_allgather_host`. The two O(g) loops in `rns_dist.c` (`node_carry_in`, spill all-gather) were left for A-grid's rewrite (one-line replacements given).
- Slab pipeline (`ntt_dist.c`): the rank's rows cut into K chunks (K = min(`DIST_CHUNKS`, rows/32), 1 over sim4), each chunk's exchange an ordinary all-to-all on a per-plan transfer stream, events ordering pack -> post -> wait -> unpack. Forward: row pass + pack of chunk k+1 under the exchange of k, unpacks under the last exchange. Inverse: all packs first (the column layout is overwritten otherwise), then wait(k), unpack + row inverse of k under the exchange of k+1. `DIST_STATS` in chunked mode: `t_a2a` is the exposed exchange time. Numerics untouched (packs are permutations).

Tests (m; one node, job-level details in archive): `t_comm` 2/3/4/8 ranks, `t_dist` synthetic / xGMI (K = 1, 4, 8) / transposed inverse / TCP 2 and 4 ranks / layered 2-4 node-processes all VERIFY OK; ecalc 10^8 at sizes 2 and 4 and 10^9 at size 1 identical (5.7 / 6.2 / 8.27 s wall). One failure found and fixed on the way: a transfer stream no longer ordered behind other ranks' packs on the null stream (sim4), fixed by a device sync in `s_wait`. Real 2-3 node runs not made (nodes taken); layered cross-host paths were verified by M3; what is new (TCP receiver thread, second exchange in flight) is host-side logic.

Pipeline timing, xGMI, four APUs, one conv = fwd + fwd + pw + inv (rank 0 wall, m):

| DIST_CHUNKS | 2^30 fwd | 2^30 conv | 2^31 fwd | 2^31 conv |
|---|---|---|---|---|
| 1 | 0.0296 s | 0.0883 s | 0.0597 s | 0.1792 s |
| 2 | 0.0290 | 0.0865 | | |
| 4 | 0.0257 (-13 %) | 0.0811 (-8 %) | 0.0578 | 0.1737 (-2.5 %) |
| 8 | 0.0249 (-16 %) | 0.0784 (-11 %) | 0.0567 | 0.1704 (-4 %) |

At 2^30 the exchange is 0.0264 s of the 0.0883 s conv (30 %); the pipeline hides 27-37 % of it (K = 4 / 8), 15-25 % at 2^31. Reason for the ceiling: the push kernel and the NTT kernels share the CUs (push overlaps compute at ~74 %, RESULTS §11), and at 2^31 the exchange (0.054 s for 3 x 4 GB per rank, ~220 GB/s) is memory-bound against the row pass on the same HBM. At 2^26 K <= 4 costs nothing, K = 8 costs 20 % (launch overhead on 1-ms transforms) - hence the default 4 then (since raised to 8 for the target shapes, see README). Whole `t_dist 31` runs (host-reference bound): 65.0 s vs main's 63.4-64.7 s, same. Batch nodes differ by ~5 %: compare within a batch.

Open issues (as of the report): the two O(g) loops in `rns_dist.c`; `DIST_STATS` rows/pack are 0 in chunked mode unless K = 1 (fixed by §C C5); forward cannot overlap unpack of chunk k with row pass of k+1 (both in x; a ring of two extra chunk planes would lift it, not done: memory); sim4 all-gather completes at the fourth call; `t_dist` COMM_RANK mode needs a power-of-two rank count.

## C — B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20)

Branch from `main` 4aca721; PLAN.md §21; continues §A-comm. Files: comm*.c/h, `ntt_dist.c/.h`, `t_dist.c`, `t_comm.c`.

#### B7 — the unequal all-to-all
`comm_alltoallv(c, sb, scnt, sdsp, rb, rcnt, rdsp, stream)` (+ `_host`, + `comm_prefix` for back-to-back offsets): rank r receives `scnt[r]` bytes from `sb + sdsp[r]` of every rank, landing at `rb + rdsp[src]`. Both sides compute sizes from the same descriptors and **every transport aborts on a count mismatch**. Completion as `alltoall` (`comm_wait`); a v-exchange and an equal-slab exchange are never in flight together. Per transport: local = one memcpy; sim4 = copies at wait; xGMI = tables published through the shared table + barrier, push kernel `k_push3<T>` with per-peer lengths; TCP = 8-byte count header per slab, sender threads + receiver thread, per-peer staging; layered = send counts all-gathered over the four APU threads, intra xGMI alltoallv, transpose, inter alltoallv over mesh d with per-node counts, reorder at wait (own scratch, 2 x intra total + receive total).

Planned consumers (not changed here; G's files `rns_dist.c`, `newton_db.c`), all already know both sides' sizes: (1) `mdb_shift`: padded slabs of `SL = maxshare/4 + 1` limbs per pair -> piece-sized buffers; saves `2 (g-1) x maxshare/4 x 8` B per APU, at 4 x 10^10 over 2 nodes 4.4 GB per APU = 17.6 GB per node per call (~15 calls per division) (mod); at 2048 nodes the padded form is impossible; (2) `mdb_add_shifted`: rounds of S = 2^26 limbs per pair (g x 512 MB per APU each for sb, rb) -> real cuts, rb 512 MB, sb ~1 GB independent of g (0.5 GB per APU at g = 2, ~1 TB at g = 2048, mod); (3) `redistribute` slab is already tight (<= 2 rows padding per pair), v form only simplifies `k_gather_mn`'s binary search (optional); (4) `mdb_to_host_all` is really an all-gather.

Push kernel: the switches `COMM_PUSH64` (64-bit stores; RESULTS §11 had 909 vs 699 GB/s) and `COMM_PUSH_BLOCKS` came with it. 2^31 conv over four APUs (m, K = 4 unless noted):

| setting | 2^31 conv | 2^30 conv |
|---|---|---|
| 16 B, 228 blocks (old default) | 0.1737 s | 0.0889 s |
| 64-bit, 76 blocks (**new default**) | **0.1577 s (-9 %)** | 0.0801 s (-10 %) |
| 64-bit, 228 / 152 / 38 / 19 blocks | 0.158 / 0.164 / 0.154-0.157 / 0.1545 | |
| 16 B, 76 blocks | 0.1663 | 0.0862 |
| K = 1 (raw exchange): 228 vs 76 blocks, 64-bit | 0.1685 vs 0.1688 | |

64-bit stores -5 %; fewer blocks another -3..-4 % at K = 4 (76 -> 19 within noise) because push blocks stop crowding the row pass off the CUs; at K = 1 the link bounds a lone push. 76 chosen as the conservative end of the flat range (a lone exchange keeps its bandwidth). The kernel also serves the all-gathers and the layered intra stage.

#### C4 — the two-plane pipeline (`dist_fwd2`, `dist_inv2`)
Question: with a second plane per rank (q limbs = 4 GiB per APU at 2^31) the pipeline can run the generic produce/post/wait/consume schedule in both directions (forward unpack of chunk k under exchange k+1; inverse chunk 0 exchange starts after chunk 0's pack). Measured (xGMI, `DIST_XGMI=1 DIST_PLANE2=1 ./tests/t_dist 31`, conv at 2^31, same node and batch): one plane 0.1570-0.1666 s, two planes 0.1650-0.1719 s, i.e. **3-6 % slower** in four batches (2^30: +3 %; 2^26: 0.0060 vs 0.0062). `DIST_STATS` (2^31, three transforms per rank): one plane exposed exchange 0.0089 s (84 % hidden), total 0.1527 s; two planes exposed 0.0168 s (73 % hidden), total 0.1608 s, exchange itself slower (0.057 -> 0.062 s). Cause: moving unpacks earlier puts them on the CUs together with the push kernel and the next row pass. Remaining exposed part with one plane (6 %) is the first chunk's row pass + pack and last chunk's unpack, which no plane count removes.
Verdict: **not adopted**; `dist_fwd2/inv2` stay as tested, unused code (delete or keep as evidence with the E1 set); the q x 8 B per APU stays free.

#### C5 — `DIST_STATS` under pipelining
Parts are now timed by events on the stream they run on; `st_flush` sums the pairs. `dist_stats` gained `t_xfer` (device time of exchanges) and `t_total`; `t_a2a` stays the host time blocked in post + wait = exposed exchange, so `1 - t_a2a/t_xfer` is the hidden fraction. `t_dist`'s line now prints `rows cols pack exchange (exposed: NN % hidden) transform total`; `dist_core`'s "ntt parts" line gets real rows/pack at every K (its "all-to-all" column remains the host-blocked time, an upper bound of the exposed exchange). The global accumulation is under a mutex (four rank threads raced on `+=`).

Tests (m, three batches, one node each): `t_comm` (incl. alltoallv: 6 rounds, zero pairs, reversed receive order), `t_dist` synthetic/local/xGMI (31 runs at 2^31 with `DIST_CHUNKS`, `COMM_PUSH64`, `COMM_PUSH_BLOCKS`, `DIST_PLANE2`, `DIST_STATS`), TCP 2 and 4 ranks, layered 2-4 node-processes all VERIFY OK; ecalc 10^8 (sizes 2, 4) and 10^9 (size 1) identical in every batch. Not run: two real nodes (nodes taken). Gate: xGMI 2^31 not slower (9 % faster, fwd 0.0573 -> 0.0520 s).

Open: the v-uses in `rns_dist.c` / `newton_db.c` are G's (`mdb_shift` is the one that matters at 4 x 10^10); the layered alltoallv has no consumer yet and uses its own `hipMalloc` scratch; the block count was not measured under the batch tier's concurrent transforms (`rns_mul.c` does not use `comm_xgmi`).

## X13 — xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a

Branch `p13-X` from `main` 203733b. Files: `comm_layered.c`, `comm_xgmi.c`, `t_dist.c`. Defaults unchanged: everything is behind `COMM_LAYER_STATS`, `COMM_XGMI_STATS`, `DIST_H4`, `DIST_LBENCH` (off). The 3-node slot was never available (admin CI array, then a node down).

#### 1. What was built
- `COMM_LAYER_STATS=1|2` (`comm_layered.c`): per exchange host stamps for the xGMI stage, ready-for-fabric, fabric stage, complete. Mode 1 passive (upper bound of fabric completion), mode 2 exact (watcher thread; perturbs slightly: use 1 for timings). Report at exit: span, xGMI-busy, fabric-busy, both-busy, held (ready exchange waiting for the previous inter stage), fabric idle while held, NIC balance, node view. With `COMM_XGMI_STATS=1` also the link-active interval of each push kernel, giving **both-busy at link level**.
- `COMM_XGMI_STATS=1` (`comm_xgmi.c`): `k_push3_ts` stamps each block; per sender-peer bytes and link-active time. `comm_xgmi_h4()` benchmark with nine modes.
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

## X13b — the uneven exchange two deep, one-kernel transposes, faster packs (PLAN.md §31 row X, axis A)

Branch `p13b-X` from `p13b-step0` c988218. Files: `comm_layered.c`, `ntt_dist.c` (pack/transpose kernels, bench function), `t_dist.c` (drivers `DIST_PBENCH`, `DIST_TBENCH`, a pipelined alltoallv check). Every addition behind a switch, off by default at the time; depth 2 is now built in (RESULTS).

M-run note: at 4 node-processes every transform group is a power of two, so the equal-slab pipeline runs and the v-exchange depth never acts. To measure axis A there, add `DIST_GEN=1` to both rows (A = 1 / 2: `COMM_ALLTOALLV_DEPTH=1/2`) or run 3 node-processes. At size 1 axis A does nothing. Other options (independent, bit-identical): `COMM_LAYER_TKERNEL=1` (layered exchange, sizes >= 2), `DIST_TWREC=1|2` (`ntt_dist` forward pack and inverse unpack, every size), `DIST_TPACK=1` (`dist_inv_t` only, tests).

#### 1. What was built
- **`COMM_ALLTOALLV_DEPTH=2` (`y_alltoallv2`)**: depth 1 completed the previous v-exchange at the top of the next post (no xGMI/fabric overlap, X13: 1.1 %). Depth 2 does the count tables, intra stage and reorder of exchange k first (under k-1's inter stage), then completes k-1, then posts k's inter stage. "Posting k completes k-1" still holds on return (`gen_inv_pw` stays correct). Inter send x2 and receive x3 alternate between two slots; intra receive x1 lives in the slot's x3 area. Scratch per APU thread: depth 1 `x0 + 2A + B`, depth 2 `x0 + 2(A + max(A,B))`, i.e. **3 S -> 4 S** on the general map; measured (stats scratch line, ecalc 10^8) size 2 `DIST_GEN=1` 12.6 -> 16.8 MB (+33 %), size 3 8.5 -> 11.2 MB (m); at the target one more quarter-plane per APU on general-map levels (mod).
- **`COMM_LAYER_TKERNEL=1`**: `block_transpose` (4 g blocks) becomes one kernel `k_btrans` instead of 4 g `hipMemcpyAsync`; the v-exchange's three reorder copy loops become batched-copy kernels `k_bcopy`. Falls back to copies for non-device pointers or lengths not multiple of 8/16 B.
- **Packs**: the plain `k_pack` (K13: 360-770 GB/s) is on no production path; production forward uses `k_twpack` (LDS-tiled + twiddle). `k_pack_t_tiled` behind `DIST_TPACK=1`. Production `k_twpack` / `k_unpacktw` run at half the copy rate (two table gathers per point); `DIST_TWREC=1` (`k_twpack_r`/`k_unpacktw_r`: a thread takes w^(ij) once for four rows and steps by w^(8j)); `DIST_TWREC=2` eight rows per thread. `ec_mm` is canonical so results are exact (bit-identical, checked).

#### 2. Measurements (m)
**2.1 Both-busy on the general map (loopback, ecalc 10^8, POOL_LOG 27).** Link-level hidden fraction of xGMI time:

| run | depth 1 | depth 2 |
|---|---|---|
| size 2 `DIST_GEN=1` passive | 1.5 % (span 1.931 s, total 8.09 s) | **75.1 %** (span 1.665 s, -14 %; total 7.44 s) |
| same, exact | 1.5 % | 70.1 % |
| size 3 node 2 (g = 3 only) | 0.0 % | **72.6 %** |
| size 3 node 0 (g = 2 and 3) | 22.9 % | **73.1 %** |

The general map now hides 70-75 %, the pow2 path's structural ceiling; size 3 as a whole goes ~23 % -> ~73 %. Held time rises 0.01 -> 0.78 s per APU (the one-at-a-time inter stage doing its job), fabric idle in only 2 % of it. Totals are single loopback runs (noise ~0.3 s); the span is the cleaner number. Ceiling of the whole-time saving on this shape is small (min/(x+f) = 6-13 %); at 576 nodes it is X13 §4's 23.5 % ceiling (mod; 17.8 % collected, 5.9 % left).

**2.1a Two real nodes (job on two nodes, TCP over 1 GbE, ecalc 10^8, `DIST_GEN=1`, all identical, VERIFY OK).** First real multi-node measurement of the layered exchange (fabric 0.1 GB/s per node):

| run | link time hidden (node 0 / node view) | fabric busy/span | held (fabric idle) | phases / total |
|---|---|---|---|---|
| general map, depth 1 | **1.4 % / 1.7 %** | 99.1 % | 0.15 s | 25.3 / 30.73 s |
| general map, **depth 2** | **74.3 % / 73.0 %** | 99.5 % | 7.69 s (0.020) | 19.8 / 24.77 s |
| pow2 path, depth 1 (reference) | 74.9 % / 72.7 % | 97.3 % | 9.30 s | 19.7 / 24.93 s |
| no stats: depth 1 / depth 2 | | | | 20.3 / 25.36 s vs 19.6 / 24.54 s (-3.4 %, one run each) |

Depth 2 takes the general map from 1.4 % to 74 % hidden, the same as pow2 and as X13 modelled (~75 %); the fabric was idle in 0.3 % of held time, so one inter exchange at a time does not bind on real nodes either. With 1 GbE the fabric is 99 % of the span, so the wall saving is small (ceiling 1.2-1.4 %).

**2.1b Depth 1 vs 2 without instrumentation (loopback, 10^9, two runs each, identical).** Phases: size 3 15.6, 15.6 -> 14.5, 14.4 s (**-7.4 %**); size 4 `DIST_GEN=1` 12.6, 12.7 -> 11.5, 11.3 s (**-9.9 %**). With a slower real fabric the xGMI share that depth 2 hides is smaller, the both-busy fraction itself holds.

**2.2 Transposes.** In the exchange (size 2 pow2) copies 0.076-0.082 ms vs kernel 0.040-0.042 ms; size 3 0.092-0.123 vs 0.047 ms. Alone (one APU, median of 7):

| g (4 g blocks) | 4 KiB: copies / kernel | 64 KiB | 1 MiB |
|---|---|---|---|
| 2 | 0.022 / 0.010 ms | 0.022 / 0.009 | 0.025 / 0.012 |
| 16 | 0.240 / 0.010 | 0.249 / 0.011 | 0.259 / 0.045 |
| 192 | 2.902 / 0.010 | 2.807 / 0.035 | 2.676 / 0.545 |
| **576** | **8.160 / 0.012 (662x)** | 7.918 / 0.107 (74x) | 7.013 / 1.522 (4.6x) |

Copies cost 3.0-4.4 us each (launch-bound until ~1 MiB); the kernel runs 2.8-3.2 TB/s on large blocks. All outputs identical.

**2.3 Packs (`DIST_PBENCH`, one APU, GB/s = 16 B x points / t, median of 5; 2^24 .. 2^32 ranges).** Device copy ~2900-3280; `k_twpack` (production fwd pack) 1201-1533; `k_unpacktw` (production inverse unpack) 1195-1501; `k_pack` (plain, unused) 300-566 and `k_pack_t` 301-591; their tiled forms `k_pack_tiled` 2640-3056 and `k_pack_t_tiled` 2614-3139 (x5-9, bit-identical); production `k_unpack` 2170-2863, `k_pack_cols` 2280-2778, `k_unpack_t` 2366-2868. `DIST_TWREC` kernels (2^28 / 2^30, bit-identical): `k_twpack_r` 2322-2335 / 2223-2236 GB/s (x1.65 over 1347-1350), `k_unpacktw_r` 1995-2006 / 2087-2097 (x1.43 over 1461-1463); `DIST_TWREC=2` 2009 / 2083 and 1939 / 1860. **End to end it does not show at size 1**: 4 x 10^10 one node one run each, identical: `TWREC=0` 46.5 s of phases (bs 25.4, dm 21.1; total 66.8 s) vs `TWREC=2` 46.7 s (bs 25.5, dm 21.1); the multi-device products spend little in these kernels. Its weight grows with the distributed share (multi-node), not measured there at the time.

#### 3. Correctness (m)
Every ecalc run `mn: all nodes VERIFY OK` and digits `cmp`-identical: 10^8 sizes 2, 3, 4 with depth 1/2, passive/exact, transposes copies/kernel, all switches; 10^9 sizes 2 (`DIST_GEN=1`) and 3 (19.3 s, 22.7 s); 4 x 10^10 at `TWREC` 0 and 2 identical to the stored reference. `t_dist` layered with depth 2 + one-kernel transposes (incl. a pipelined alltoallv check: three v-exchanges posted back to back, then three waits) and `TWREC=1` VERIFY OK; `DIST_TBENCH` 21/21 identical; `DIST_PBENCH` tiled packs bit-identical. Gate `unit,e9,mn` green at the defaults (unit: t_mn_grid 200 checks, t_mul 20 189, t_bs 10, t_dbig 0 555, t_newton 20 620, t_verify 334, t_out 1, t_ntt 24 1519 checks; e9/mn 7 passed 0 failed, 130 s); with depth 2 + TKERNEL + TWREC=1 on: 7 passed 0 failed, 138 s.

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

Branch s50 from main daeb0f9 (2026-10-10/11). 

#### 1. Design
`k_gather` writes `x[il*C + j]` with a store strided by C (S40's next floor after the select-of-scalars change, already default as `DBIG_QSEL=1`). `k_gather_tile<QS,T>` (T = 16/32/64): a 256-thread block owns a T x T tile; read phase lane = il (contiguous source run), tile in LDS, write phase lane = j (contiguous destination row). Same arithmetic, digits cannot change; shapes with rows % T or C % T non-zero, or `DBIG_QSEL=0`, fall back. Switch `DIST_GATHER_TILE` = 0 (default) / 16 / 32 / 64 (1 = 32); two call sites in the single-node product; multi-node tier untouched. Probe `tests/t_gather_probe.c` with three tile variants.

#### 4. Results (m; aac7 hold, aac6 nodes)
Probe (1 node, remote source, best of reps), rows 8192 x C 65536: 4-way select (default) 35.24 ms; tile 16 31.27 (0.89x); **tile 32 30.23 (0.86x)**; tile 64 35.72 (1.01x); 0 mismatches. Second shape rows 1024 x C 524288: select 26.30 ms, tile 32 32.80 (1.25x slower), tile 64 35.07. The tile saves ~5 ms of one 35 ms launch; the product calls it twice.
Gates (1 node, 1e9): `=0` and `=32` PASS, digits identical (aac6 CPX gates also PASS).
ABBA (A = off, B = tile 32; mean B-A, positive = tile slower):

| where | scale | rounds | mean B-A | CI half-width | result |
|---|---|---|---|---|---|
| aac7 1 node | 6.441e10 | 3 | **-0.08 s** | 0.88 s | ns |
| aac7 4 nodes | 6.441e10 per node | 4 (cap) | **-0.07 s** | 30.6 s | ns (bimodal +-26 s, S47) |
| aac6 CPX | 4e9 | 5 | -0.046 s | 0.655 s | ns |
| aac6 PPAC slot 1 / slot 2 | 6.441e10 | 5 / 5 | +0.507 / -0.293 s | 0.591 / 1.029 s | ns |

The micro-gain (5 ms x 2 launches, ~0.01 s) is two orders below run noise.
Engine gates (`mnaccept --only unit,e9,full`, aac7 4-node, tile exported): 7 passed, 4 failed: `t_bs` (2 of 10 checks), `t_mn_grid` (timeout), two `e9 size 1` (missing reference file on aac7: environment). t_bs / t_mn_grid were not compared against a no-tile baseline (doubt whether pre-existing; neither is known to use the single-node k_gather).

#### 5. Verdict
**Do not adopt**: no measurable gain (0.0 +- 0.9 s at 1 node) and shape-dependent (tile 32 slower at rows 1024 x C 524288). Disposition: the code stays off main (archive tag `archive/s50-gather-tile`); results and drivers (`archive:drivers/s50/`) kept. A gain would need to be >= 0.5 s at 1 node to show in 3 rounds.

## P2: speed up k_gather -- stopped after a micro-probe; no switch added

2026-10-12. Question: is there kernel-level headroom in the gather?

- Share of run time (m from reports; mod for canon passes): 1 node 6.441e10 digits, default `DIST_GATHER_RAW=2`: `k_gather_raw<1>` 2.0 s per APU (4.3 % of 47 s GPU time, `results/SINGLE_NODE.md §CAND`); 58 raw gathers ~34 ms each plus 232 canon passes ~3 ms each (~0.7 s mod, `results/EXCHANGE.md §K1b`); total ~2.7 s (mod) of ~70-80 s wall. At 2+ nodes the leaf's mdev uses the same path (saving ~1.4 s at 3.8e13); tree gathers (`k_gather_mn24_np`) have no kernel breakdown in any report.
- Probe `t_gather_p2_probe.c` (aac7, 1 node, best of 8, all variants identical; rows 8192 x C 65536 / 1024 x C 524288, ms, m):

| variant | shape 1 | shape 2 |
|---|---|---|
| raw gather, strided write, remote source (today) | 35.4 | 21.2 |
| same, local source | 35.2 | 22.1 |
| tiled write 32x32, remote source | 30.4 | 32.0 |
| tiled write 32x32, local source | **3.8** | 3.9 |
| other tile shapes, remote | 33.5-36.6 | 33.5-36.6 |
| read-only of the same limbs, remote (coalesced) | **24.3** | 24.4 |
| kernel copy / DMA of 3 remote quarters (4x the bytes) | 142 / 220 | 142 / 220 |

- Reading: the strided store bounds only a local source (35 -> 3.8 ms). With the striped (3/4 remote) source the floor is the remote read, ~24 ms (~130 GB/s of pull); the best tile reaches 30 ms and is slower at shape 2. Staging remote quarters by copy is slower.
- Verdict: remaining headroom 35 -> ~25 ms x 58 calls = **~0.6 s per run** (mod, upper bound; S50's tile gets ~0.3 s and measured -0.08 +- 0.88 s, `results/EXCHANGE.md §S50`); four ABBA rounds resolve ~+-1 s, so no change can be shown. No switch added; no gates (no product code changed). The real lever is layout (produce the operand already in the rank's rows or fold the pull into the multiplications' all-to-all): a design change (ALGORITHM.md), proposed for the user's priority.

## K6: single-node C-tier all-to-all — overlap across primes (`RNS_DIST_OVERLAP=1`) -- measured, not adopted, setting removed

Branch k6 from main a448d73 (2026-10-12). Source: OPPORTUNITIES §B.3 K6 <- `results/KERNELS.md §S38 §2` ("10.0 s of the 24.4 s C time is exposed host wait at 1 node; only deeper overlap across primes remains").

#### 1. What the 10.0 s is
`DIST_STATS` `t_a2a` is the host time blocked in `chunk_post` + `chunk_wait`; with K = 8 and depth 1 the host waits for chunk k-1's push, which itself waits for k-1's row pass, so the wait covers GPU compute on the critical path and is not GPU idle. The push (3 GB per plane per APU at 240 GB/s, ~13 ms per transform at 2^31, m) overlaps the next row pass (mod 1.4 ms per chunk vs 1.6 ms of push); S38's "bandwidth lever is closed" stands. What is serialised: the **operand gathers**: per prime `k_gather(A)`, `k_gather(B)`, sync, three transforms, sync. S13 (m, aac6, 2^31 points, C form): load 0.35 s of 1.05 s per product (33 %); S38 rocprof `k_gather` 7.7 s per APU over the run; ~8 s of gathers over the 29 C products' 24.4 s at 6.441e10 (mod).

#### 2. Design
`RNS_DIST_OVERLAP=1` (`rns_dist.c` `dist_core`, single-node C form only): a second stream `g` per rank runs the gathers of prime p+1 under the transforms of prime p (A into the untouched `xa[p+1]`; B via an alternating spare plane when pool 0 has one, else via a hook `dist_plan.after_cols` fired after prime p's column inverse, the last reader of `xb`); one `hipStreamSynchronize` per product instead of per prime. Same kernels on the same data, only stream placement changes; hazards checked. Expected gain (mod): up to ~6 s at 6.441e10 minus xGMI contention between gather reads and pushes.

#### 4-5. Results (m)
- 2-node aac7 gate (1e9, switch on): PASS (VERIFY OK, digits identical, ecalc 19.05 s; correctness only).
- aac6: `cmp4e9` PASS (1 777 782 789 bytes identical, `RNS_STRATEGY=C`, off vs on); gate 1e9 on PASS (11.30 s), strategy C PASS (12.36 s); `t_strategy` rc 0.
- ABBA (A = off, B = on), total seconds:

| size | rounds | mean B-A | 95 % CI half | significant | deltas (s) | arm A mean |
|---|---|---|---|---|---|---|
| 1e10 | 8 (cap) | -0.32 | 0.55 | no | -1.0, 0.8, 0.3, -0.5, -0.5, 0.2, -0.9, -0.9 | 24.35 s |
| 6.441e10 | 3 | -0.11 | 0.15 | no | -0.1, -0.1, -0.2 | 73.41 s |

C-tier seconds at 6.441e10 (29 C products, arm A 20.68 s): mean diff +0.242 s, CI half 0.658 s, not significant; at 1e10 no C products.
- Verdict: the switch is correct (digits identical) but there is no measurable gain: -0.11 +- 0.15 s of 73 s (0.15 %), C-tier time not reduced. **Do not adopt**; the setting was removed (RESULTS, OPPORTUNITIES §K6).
