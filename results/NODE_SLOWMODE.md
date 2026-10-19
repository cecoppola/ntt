# NODE_SLOWMODE — the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts)

Scope: the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts). | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| T05 | why 4-node run totals are bimodal (fast ~181 s / slow ~208 s) |
| T09 | the bimodal 4-node totals (2026-10-11, aac7, session task t09) |
| A13 | 4-node slow mode on a quiet aac7 (part A) and the DIST_LOGN_TEST 4-node "65 corrections" (part B) |
| A14 | what picks the "slow mode" NIC group at launch? (aac7, 2026-10-11) |
| B04 | fast node screen before the long run (2026-10-11, Wave B) |
| T10 | cold-path node-check probe vs the known slow host aac7-n01 (2026-10-11) |
| S49 | slow-host policy: per-host `nodechk` rehearsal (aac7 all hosts, aac6 PPAC) |
| NIC | node screen catches nodes with fewer NICs than the launch asks for |

## T05 - why 4-node run totals are bimodal (fast ~181 s / slow ~208 s)

(former file: T05.md)

Task t05 (session 2026-10-11). Analysis of EXISTING logs only (no cluster jobs). Sources, all aac7, 4 nodes x 6.441e10 digits,
hosts aac7-n10 (rank 0), s1b1n0, s2b0n0, s2b1n0 in every run: `~/s47/run/log` (40 runs, hold 12471), `~/s48/log/n4_r*` (2),
`~/batch1011/T5/s50/abba4_0024` (16, hold 12471), `~/batch1011/T5/s51/abba4` (12, hold 12471), `~/batch1011/T5/s51/abba4b` (23, hold 12496).
93 four-node runs in total; times in Eastern. Every number below is **measured** (parsed from the logs) unless marked otherwise.
Parser and table were built in a session scratch dir (not tracked); the full table is at the end.

#### Answer in one paragraph

The slow mode is a **per-run communication slowdown**: in a slow run the inter-node all-to-all (the SHMEM transport) runs at
10.9 GB/s per APU thread instead of 13.8 GB/s (-21%), on all four PEs at the same time, and every network-bound product in dm
takes 25-30% longer (constant ratio from the first to the last stage). GPU compute is unchanged, init is unchanged. It is a
two-state switch that is re-drawn independently for every run (about 29% of runs are slow); it does not depend on the arm, the
node, the rank, the position in the ABBA block, the preceding run, the time of day, the hold, or MemAvailable. A second,
separate effect (the single-node reciprocal chain spiking from 2.3 s to up to 89 s, plus init 46-53 s) hits cold starts and one
new hold and explains the outliers above 213 s; it is not part of the bimodality.

#### Classification

Mode = mean over the 4 PEs of the `comm_shmem: ... all-to-all ... GB/s per APU thread` line. The distribution has a clean gap:
slow runs 8.5-11.7 GB/s (27 runs), fast runs 12.3-14.0 GB/s (66 runs). Using only "clean" runs (chain <= 2.6 s, init < 36 s;
16 slow, 47 fast), the two modes are tight (sd about 1-2 s on the total):

| quantity (clean runs) | slow | fast | slow - fast | ratio |
|---|---|---|---|---|
| total (s) | 208.3 (sd 1.7) | 181.4 (sd 1.4) | +26.8 | 1.15 |
| bs (s) | 77.3 | 70.7 | +6.6 | 1.09 |
| dm (s) | 96.7 | 76.8 | +20.0 | 1.26 |
| - reciprocal products (s) | 21.29 | 16.39 | +4.9 | 1.30 |
| - dm step 1, A*mu and X_hi*Q (s) | 33.32 | 26.12 | +7.2 | 1.28 |
| - dm step 2, A*mu + assembly (s) | 13.30 | 10.64 | +2.7 | 1.25 |
| - X_lo*Q (s) | 22.73 | 17.63 | +5.1 | 1.29 |
| init (s) | 30.3 | 29.8 | +0.5 | unchanged |
| dc (output formatting, s) | 2.98 | 2.96 | 0 | unchanged |
| all-to-all rate, mean of PEs (GB/s per APU thread) | 10.89 | 13.80 | -2.9 | 0.79 |

All-to-all rate per PE, slow vs fast: PE0 10.10 vs 12.86, PE1 10.92 vs 13.94, PE2 11.33 vs 14.45, PE3 10.48 vs 13.37 (ratio 0.78-0.79
for each PE; all runs, 27 slow / 66 fast). The +20 s in dm is the sum of the four network-bound product groups (4.9 + 7.2 + 2.7 + 5.1).
bs gets +6.6 s (the coordinator's "+3-9 s"). Per-level tree times (`mn: node N: tree levels`) are +16% on every node (node 0 49.7 vs
42.6 s, nodes 1-3 46.4-46.9 vs 39.5-40.3 s), consistent with comm being about half of tree time.
The per-device compute counters of S51 (`s51stat.tsv`, crt/modq/addsub timings) are identical in slow and fast runs of abba4b
(e.g. 21.5 s / 6.80 s / 0.46 s in arm A in both modes), so the slow mode is not GPU compute.

#### What does NOT separate the modes (tested)

| candidate | evidence | verdict |
|---|---|---|
| arm / switch | A 14 slow of 45, B 13 of 46; S47, S50 and S51 all contain both arms identically | no |
| one bad node or rank | all 4 PEs slow together by the same factor (0.78-0.79); the slowest PE and slowest tree node are PE0/node 0 in 26 of 27 slow runs, but also in 64 of 66 fast runs (structural: PE0 receives the least data); rank 0 is always on aac7-n10 so rank-0 host cannot discriminate | no node identifies it (but see "localisation" below) |
| sub-stage | every network product is slowed by 25-30% and none of the compute stages are | uniform, not one stage |
| position in ABBA block | positions 1-4 of the S50/S51 rounds: 2, 5, 5, 4 slow of 13, 13, 13, 12 | no |
| alternation / persistence | sequence of slow (S) and fast (.) in time order: S47 `.SS..S..S..S.....S..S...SS...S....S.....`, S50 `.S.......S...SS.`, S51 `..SS..S.SS..`, S51b `.S.S........S.SS..SS...`. P(slow after fast) = 20/62 = 0.32, P(slow after slow) = 7/27 = 0.26; Wald-Wolfowitz runs test 41 runs vs 39.0 expected (z = 0.5) | memoryless: each run is an independent draw, no slow periods, no alternation |
| preceding run's state | the preceding run's total/mode does not predict it (above) | no |
| time since previous run | ABBA gaps are 1-23 s everywhere; slow runs show a shorter derived gap (10.7 vs 16.0 s, t = -2.7) but this is a driver artefact: ab.tsv walls are quantised to 20 s polls (200 s for a 181-184 s run, 220 s for a 205-210 s run), so a long run leaves less slack | not a cause |
| MemAvailable | prep lines before each run: 450-452 GB on all 4 nodes in every S47 run (no information). After init (`budget:` lines), minimum MemAvailable is 120.7 GB slow vs 126.0 GB fast (Welch t = -1.45, not significant) | no |
| hold / allocation / reallocation | holds 12471 and 12496 both show both modes (12496: 8 slow of 23 runs); same 4 hosts throughout | no |
| time of day | slow share by EDT hour 00h 5/16, 01h 5/17, 02h 0/7, 03h 2/11, 04h 4/9, 05h 3/8, 07h 2/9, 08h 5/14 | no pattern visible |
| 2-node link route | S48 2-node (hosts s1b0n0 + s1b1n0, same chassis): 144.1 s slow vs 133.9 s fast; dm 61.3 vs 55.9 s, every dm stage +7-14% | the mode also occurs on a single in-chassis pair, so it is not about a path across chassis; smaller effect at 2 nodes because less all-to-all traffic |

#### Best-supported explanation (evidence level: moderate, mechanism not yet identified)

The throughput of the inter-node network path seen by the whole 4-node job is in one of two states for the duration of a run
(about 0.79 x the fast rate when slow), chosen independently per run. Evidence for "per-run, whole-job, network side":
(1) the all-to-all rate falls by an identical factor on every PE; (2) the penalty is a constant ratio of 1.25-1.30 across all
network-bound dm stages over 60+ s, so it is not a burst of congestion hitting one phase; (3) GPU compute and init are unchanged;
(4) the state is independent between consecutive runs (3.5 min apart), the arm, the hold and MemAvailable; (5) it also exists at
2 nodes on one in-chassis host pair. What the logs cannot say is whether the state lives in the fabric/NICs of these hosts
(for example Slingshot routing or congestion state, NIC state) or is created at process start (for example which route or
address-vector/endpoint layout libfabric/CXI sets up, or host-memory/page placement of the registered buffers). External
traffic from other users sharing the fabric would also fit (1)-(4) only if it flipped on timescales of minutes and stayed
constant within a run, which looks less likely than a state fixed per launch. These two groups are not separable from existing
logs: the old runs never printed per-peer statistics in a slow run (`COMM_XSTATS`, `MN_WAIT_STATS` exist only in the two S48 4-node
runs, both fast).

#### Second, separate effect: the reciprocal chain spike and cold start (not the bimodality)

The single-node part of the reciprocal (`recip(mn): the single-node chain to 54602 limbs (N s)`) normally takes 2.2-2.4 s. In 29 of
93 runs it took 3-89 s (S47 12 of 40, S51b 16 of 23, S51 1 of 12, S50 0 of 16), independent of the comm mode (11 of the 29 were
also slow-mode). Its spikes explain every total above 213 s (231, 281, 285, 245, 275, 243 s) and the 185-202 s "fast-mode"
runs: e.g. S51b r2_3A chain 88.8 s, r5_2B 67.5 s, S47 i_r1_B 61 s of recip. Init is also longer (46-53 s instead of 28-32 s) in the first
one or two runs of a fresh start (S47 first two runs, S51b first two runs), which matches the coordinator's "first rounds slower
still". Hypothesis (low evidence, not tested): cold host-side state (page cache, first-touch, HIP module/JIT cache) for the CPU/
single-GPU chain stage; it is only seen at the start of S47 and throughout S51b (new hold 12496 after a 2 h idle), only once (S51 04:5x) in the 28 consecutive S50/S51
runs on hold 12471 after 03:24.

#### The exact test that would confirm or kill the explanation (for t09 or later)

Goal: decide whether the slow state belongs to the environment (fabric/NICs/hosts) or is set when the process starts.
Cost: one 4-node hold, about 24 real runs (about 80 min) plus short probes, no code change.

1. Before every real 4-node run and again immediately after it, run a 30 s stand-alone 4-node all-to-all bandwidth probe on the same
   four hosts (use the app's own all-to-all, e.g. a `t_a2a`-style test with 1 GiB per pair, or `osu_alltoall`-like over the same SHMEM
   path; never concurrently with a real run: one network program per node). Log the probe GB/s per PE with the run id.
2. Classify each real run by its own `comm_shmem: ... GB/s per APU thread` (slow < 11.95, fast > 12.3 GB/s per APU thread).
3. Prediction if the state is environmental: the "before" and "after" probes both read about 0.79 x lower exactly when the run is slow
   (point-biserial correlation of probe rate with the run's mode above 0.8), and the mode may persist for a few minutes. Prediction if it is
   created at process start: the stand-alone probe (a different process, started separately) does not predict the real run, and
   the real runs keep the memoryless pattern. Because the probe is itself a new launch, also launch the probe twice in a row to check that a
   launch-to-launch draw exists on its own.
4. In the same runs keep `COMM_XSTATS=1 MN_WAIT_STATS=1` and compare slow with fast per PE: `t_wait`, `roff_wait` and `sig_wait` rising uniformly on
   all PEs means a slower fabric/remote side; a single PE with high `t_call` and the others with high `t_wait` means a straggler.
   Also sample the NIC counters (`cxi_stat`-style telemetry, retry/stall/congestion counters, all 4 NICs per node) before and after each run.
5. If the probe shows the state, localise with the 6 host pairs (2-node probes), which also tests the in-chassis pair (s1b0n0 + s1b1n0) seen in S48.

A single stratum is enough for a decision: 24 runs give an expected 7 slow ones (p about 0.29); a probe that tracks the mode in at least 6 of those
7 and in none of the fast ones is conclusive.

The t09 runs with stats on already provide step 2 and 4 for free; only the stand-alone probes (steps 1, 3) need to be added.

#### Open points

- Whether the 27 slow runs include a few "partial" ones: three runs with rates 12.3-12.8 GB/s and slightly raised products (S47 i_r1_A,
  S51b r1_1A, r3_1A; product group 18.1-21.9 s) sit between the modes, all in cold-start or chain-spike runs.
- Raw logs stay on aac7 (`~/s47`, `~/s48`, `~/batch1011/T5`).

#### RESUME

State: analysis complete; no cluster jobs, nothing running. If continuing: run the probe test above, or wait for the t09 xstats
and add per-PE slow/fast comparison here.

#### Table of all 93 four-node runs

Columns: start = run start (EDT, from the aac7env line). recip (products) = reciprocal total and its product part; step1/step2/X_lo Q
are the dm divmod stage times; chain = single-node reciprocal chain; a2a = per-APU-thread all-to-all rate (min-max over the 4 PEs);
tree max = longest `tree levels` over the 4 nodes; min MemAvail after init from the `budget:` lines (not the pre-run value, which was
450-452 GB on every node); mode = SLOW if mean a2a < 11.95 GB/s; prev total = total of the previous run in time order on the same
hold; rank 0 was on aac7-n10 and the hosts were aac7-n10, s1b1n0, s2b0n0, s2b1n0 in all runs.

| src | run | start EDT | arm | total | bs | dm | recip (products) | step1 | step2 | X_lo Q | chain | init | a2a GB/s/APU thread (min-max over PEs) | tree max | min MemAvail after init (GB) | mode | prev total |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| s47 | i_r1_A | 10-10 23:54 | A | 224.7 | 82.4 | 92.4 | 31.8 (18.1) | 29.2 | 12.7 | 18.0 | 9.8 | 46.2 | 12.2-14.3 | 53.5 | 105 | fast | 0 |
| s47 | i_r1_B | 10-10 23:58 | B | 280.7 | 86.8 | 142.5 | 61.4 (26.7) | 37.9 | 17.4 | 25.1 | 31.2 | 46.0 | 8.2-9.4 | 59.6 | 91 | SLOW | 225 |
| s47 | ii_r1_A | 10-11 00:03 | A | 231.0 | 81.3 | 106.1 | 47.6 (21.0) | 27.7 | 12.6 | 17.6 | 23.1 | 38.5 | 11.1-12.0 | 54.5 | 91 | SLOW | 281 |
| s47 | ii_r1_B | 10-11 00:07 | B | 213.7 | 74.8 | 104.2 | 47.2 (18.4) | 27.1 | 11.0 | 18.4 | 25.4 | 29.7 | 12.5-13.8 | 45.8 | 133 | fast | 231 |
| s47 | i_r2_B | 10-11 00:11 | B | 207.7 | 72.6 | 102.2 | 47.2 (16.9) | 26.1 | 10.7 | 17.6 | 27.0 | 28.1 | 13.0-14.4 | 42.0 | 136 | fast | 214 |
| s47 | i_r2_A | 10-11 00:15 | A | 231.0 | 78.1 | 115.4 | 44.3 (21.3) | 33.9 | 13.5 | 23.1 | 19.5 | 29.6 | 10.2-11.4 | 48.9 | 131 | SLOW | 208 |
| s47 | ii_r2_B | 10-11 00:19 | B | 201.9 | 73.7 | 94.8 | 39.9 (16.4) | 26.1 | 10.7 | 17.6 | 20.1 | 28.5 | 13.0-14.8 | 44.5 | 129 | fast | 231 |
| s47 | i_r3_A | 10-11 00:23 | A | 193.9 | 70.2 | 87.5 | 31.7 (16.5) | 26.5 | 10.7 | 18.0 | 11.9 | 32.5 | 12.7-14.3 | 43.0 | 122 | fast | 202 |
| s47 | i_r3_B | 10-11 00:27 | B | 213.2 | 79.2 | 101.4 | 31.5 (21.3) | 33.4 | 13.3 | 22.7 | 6.8 | 28.3 | 10.4-11.6 | 49.1 | 137 | SLOW | 194 |
| s47 | ii_r3_A | 10-11 00:30 | A | 181.6 | 71.2 | 77.0 | 22.1 (16.4) | 26.1 | 10.7 | 17.6 | 2.3 | 28.6 | 12.9-14.3 | 42.6 | 120 | fast | 213 |
| s47 | ii_r3_B | 10-11 00:34 | B | 181.4 | 71.1 | 76.7 | 21.9 (16.4) | 26.1 | 10.6 | 17.6 | 2.2 | 28.8 | 13.0-14.4 | 41.5 | 138 | fast | 182 |
| s47 | i_r4_B | 10-11 00:37 | B | 207.4 | 76.0 | 97.9 | 27.1 (21.5) | 33.7 | 13.5 | 23.0 | 2.2 | 29.9 | 10.2-11.6 | 46.6 | 127 | SLOW | 181 |
| s47 | i_r4_A | 10-11 00:41 | A | 184.1 | 71.4 | 77.7 | 22.0 (16.4) | 26.5 | 10.7 | 17.9 | 2.3 | 31.3 | 12.8-14.5 | 44.1 | 127 | fast | 207 |
| s47 | ii_r4_B | 10-11 00:44 | B | 207.6 | 71.5 | 103.3 | 48.4 (16.8) | 26.1 | 10.6 | 17.6 | 28.3 | 28.9 | 13.0-14.4 | 42.4 | 133 | fast | 184 |
| s47 | ii_r4_A | 10-11 00:48 | A | 189.9 | 74.1 | 81.8 | 22.7 (16.4) | 28.2 | 10.6 | 19.8 | 3.0 | 30.0 | 12.3-13.8 | 44.9 | 136 | fast | 208 |
| s47 | i_r5_A | 10-11 00:51 | A | 183.3 | 69.2 | 77.6 | 22.0 (16.5) | 26.4 | 10.7 | 17.9 | 2.2 | 32.7 | 12.7-14.2 | 42.1 | 100 | fast | 190 |
| s47 | i_r5_B | 10-11 00:54 | B | 181.0 | 69.3 | 76.9 | 21.9 (16.4) | 26.1 | 10.7 | 17.6 | 2.2 | 30.6 | 13.0-14.6 | 41.5 | 125 | fast | 183 |
| s47 | ii_r5_A | 10-11 00:58 | A | 208.7 | 80.5 | 97.5 | 27.0 (21.4) | 33.6 | 13.5 | 22.9 | 2.2 | 26.3 | 10.5-11.7 | 48.1 | 146 | SLOW | 181 |
| s47 | ii_r5_B | 10-11 01:01 | B | 209.3 | 74.5 | 100.2 | 44.7 (17.0) | 26.7 | 10.6 | 17.6 | 22.5 | 30.9 | 12.4-14.2 | 46.6 | 120 | fast | 209 |
| s47 | i_r6_B | 10-11 01:05 | B | 185.6 | 69.4 | 80.5 | 25.6 (16.5) | 26.1 | 10.6 | 17.6 | 5.8 | 31.1 | 12.8-14.4 | 41.6 | 122 | fast | 209 |
| s47 | i_r6_A | 10-11 01:08 | A | 209.0 | 76.9 | 97.8 | 26.9 (21.3) | 33.8 | 13.4 | 23.1 | 2.3 | 30.4 | 10.2-11.6 | 48.1 | 135 | SLOW | 186 |
| s47 | ii_r6_B | 10-11 01:12 | B | 182.5 | 72.8 | 77.1 | 22.0 (16.4) | 26.2 | 10.7 | 17.6 | 2.3 | 28.2 | 13.2-14.6 | 41.7 | 134 | fast | 209 |
| s47 | ii_r6_A | 10-11 01:15 | A | 182.1 | 71.0 | 76.7 | 22.0 (16.4) | 26.0 | 10.6 | 17.6 | 2.3 | 29.9 | 12.8-14.4 | 41.9 | 125 | fast | 183 |
| s47 | i_r7_B | 10-11 01:19 | B | 180.7 | 71.7 | 77.0 | 22.0 (16.3) | 26.1 | 10.6 | 17.7 | 2.3 | 28.4 | 13.0-14.8 | 40.6 | 140 | fast | 182 |
| s47 | ii_r7_A | 10-11 01:23 | A | 207.2 | 77.2 | 96.4 | 26.8 (21.1) | 33.2 | 13.2 | 22.6 | 2.3 | 30.2 | 10.5-11.3 | 47.8 | 120 | SLOW | 181 |
| s47 | ii_r7_B | 10-11 01:26 | B | 208.7 | 77.4 | 97.5 | 27.4 (21.6) | 33.5 | 13.4 | 22.7 | 2.2 | 29.5 | 10.3-11.3 | 47.5 | 135 | SLOW | 207 |
| s47 | i_r8_B | 10-11 01:30 | B | 183.0 | 70.6 | 77.6 | 22.4 (16.7) | 26.3 | 10.6 | 17.8 | 2.3 | 30.2 | 12.8-14.7 | 42.6 | 129 | fast | 209 |
| s47 | i_r8_A | 10-11 01:33 | A | 182.3 | 72.3 | 77.9 | 22.1 (16.5) | 26.6 | 10.7 | 17.9 | 2.3 | 27.6 | 12.9-14.8 | 42.4 | 138 | fast | 183 |
| s47 | ii_r8_B | 10-11 01:37 | B | 181.8 | 72.6 | 77.0 | 22.1 (16.4) | 26.1 | 10.7 | 17.6 | 2.3 | 28.5 | 13.1-14.7 | 42.3 | 138 | fast | 182 |
| s47 | ii_r8_A | 10-11 01:40 | A | 208.2 | 78.0 | 96.6 | 26.8 (21.2) | 33.3 | 13.3 | 22.7 | 2.2 | 29.7 | 10.4-11.5 | 46.2 | 129 | SLOW | 182 |
| s47 | i_r9_A | 10-11 01:44 | A | 183.7 | 70.7 | 77.8 | 22.1 (16.5) | 26.5 | 10.7 | 18.0 | 2.2 | 31.1 | 13.0-14.5 | 42.9 | 129 | fast | 208 |
| s47 | i_r9_B | 10-11 01:47 | B | 180.4 | 71.6 | 76.8 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 28.1 | 13.3-14.8 | 39.9 | 136 | fast | 184 |
| s47 | ii_r9_A | 10-11 01:50 | A | 180.9 | 71.4 | 76.8 | 22.0 (16.3) | 26.0 | 10.6 | 17.6 | 2.3 | 28.2 | 13.1-14.7 | 39.6 | 138 | fast | 180 |
| s47 | ii_r9_B | 10-11 01:54 | B | 180.7 | 70.2 | 76.8 | 21.9 (16.3) | 26.1 | 10.6 | 17.6 | 2.2 | 28.1 | 13.3-14.8 | 41.3 | 131 | fast | 181 |
| s47 | i_r10_B | 10-11 01:57 | B | 207.7 | 78.1 | 96.6 | 26.9 (21.2) | 33.2 | 13.2 | 22.7 | 2.3 | 29.5 | 10.4-11.7 | 47.8 | 135 | SLOW | 181 |
| s47 | i_r10_A | 10-11 02:01 | A | 183.3 | 71.9 | 77.9 | 22.1 (16.5) | 26.6 | 10.7 | 18.0 | 2.2 | 29.7 | 12.9-14.7 | 43.4 | 133 | fast | 208 |
| s47 | ii_r10_B | 10-11 02:04 | B | 181.2 | 70.2 | 76.6 | 21.9 (16.3) | 26.0 | 10.6 | 17.5 | 2.3 | 30.0 | 12.8-14.8 | 42.9 | 126 | fast | 183 |
| s47 | ii_r10_A | 10-11 02:08 | A | 181.2 | 70.8 | 76.8 | 21.9 (16.3) | 26.1 | 10.7 | 17.6 | 2.2 | 28.8 | 12.9-14.6 | 40.1 | 136 | fast | 181 |
| s47 | i_r11_A | 10-11 02:11 | A | 183.6 | 72.9 | 77.9 | 22.1 (16.5) | 26.5 | 10.8 | 18.0 | 2.4 | 28.9 | 12.8-14.5 | 43.7 | 132 | fast | 181 |
| s47 | i_r11_B | 10-11 02:14 | B | 182.0 | 70.4 | 77.0 | 22.1 (16.4) | 26.1 | 10.7 | 17.6 | 2.4 | 31.1 | 12.9-14.4 | 41.6 | 118 | fast | 184 |
| s48 | n4_r1 | 10-11 02:26 | - | 182.0 | 70.9 | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 29.9 | 13.0-14.8 | 42.0 | 138 | fast | 182 |
| s48 | n4_r2 | 10-11 02:29 | - | 181.6 | 72.3 | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 28.9 | 12.9-14.6 | 43.0 | 136 | fast | 182 |
| s50 | r1_1A | 10-11 03:24 | A | 182.8 | 69.4 | 76.8 | 22.0 (16.4) | 26.1 | 10.6 | 17.5 | 2.3 | 32.4 | 12.9-14.5 | 42.4 | 112 | fast | 182 |
| s50 | r1_2B | 10-11 03:27 | B | 208.3 | 75.5 | 96.8 | 27.1 (21.4) | 33.1 | 13.2 | 22.8 | 2.3 | 32.0 | 10.2-11.5 | 48.5 | 120 | SLOW | 183 |
| s50 | r1_3B | 10-11 03:31 | B | 181.2 | 71.3 | 76.8 | 22.0 (16.3) | 26.1 | 10.6 | 17.6 | 2.3 | 29.2 | 12.8-14.6 | 43.4 | 138 | fast | 208 |
| s50 | r1_4A | 10-11 03:34 | A | 181.4 | 68.9 | 76.7 | 21.9 (16.3) | 26.0 | 10.6 | 17.6 | 2.3 | 31.5 | 12.9-14.3 | 40.6 | 116 | fast | 181 |
| s50 | r2_1B | 10-11 03:38 | B | 181.1 | 68.6 | 77.2 | 22.2 (16.5) | 26.2 | 10.6 | 17.6 | 2.4 | 32.0 | 13.1-14.5 | 39.5 | 127 | fast | 181 |
| s50 | r2_2A | 10-11 03:41 | A | 182.4 | 69.4 | 76.9 | 22.0 (16.4) | 26.1 | 10.7 | 17.6 | 2.3 | 32.0 | 12.8-14.4 | 41.3 | 92 | fast | 181 |
| s50 | r2_3A | 10-11 03:44 | A | 180.7 | 71.3 | 76.9 | 22.0 (16.4) | 26.1 | 10.7 | 17.6 | 2.2 | 27.9 | 13.1-14.9 | 41.1 | 127 | fast | 182 |
| s50 | r2_4B | 10-11 03:48 | B | 183.6 | 73.7 | 76.8 | 21.9 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 28.8 | 13.0-14.8 | 44.3 | 146 | fast | 181 |
| s50 | r3_1A | 10-11 03:51 | A | 181.3 | 70.1 | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 29.4 | 13.2-14.6 | 40.1 | 135 | fast | 184 |
| s50 | r3_2B | 10-11 03:54 | B | 207.7 | 75.5 | 96.6 | 27.0 (21.3) | 33.2 | 13.2 | 22.6 | 2.4 | 31.3 | 10.2-11.4 | 48.4 | 112 | SLOW | 181 |
| s50 | r3_3B | 10-11 03:58 | B | 181.1 | 69.2 | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 30.8 | 12.8-14.7 | 42.3 | 129 | fast | 208 |
| s50 | r3_4A | 10-11 04:01 | A | 179.9 | 69.0 | 76.9 | 22.1 (16.3) | 26.0 | 10.6 | 17.6 | 2.5 | 29.2 | 13.1-14.6 | 39.0 | 124 | fast | 181 |
| s50 | r4_1B | 10-11 04:05 | B | 181.1 | 70.9 | 76.8 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 28.5 | 13.2-15.0 | 40.5 | 131 | fast | 180 |
| s50 | r4_2A | 10-11 04:08 | A | 208.9 | 79.8 | 96.4 | 26.8 (21.1) | 33.2 | 13.2 | 22.6 | 2.3 | 28.0 | 10.5-11.5 | 48.3 | 146 | SLOW | 181 |
| s50 | r4_3A | 10-11 04:12 | A | 208.2 | 76.5 | 96.8 | 27.1 (21.3) | 33.2 | 13.2 | 22.7 | 2.2 | 30.3 | 10.1-11.7 | 49.8 | 116 | SLOW | 209 |
| s50 | r4_4B | 10-11 04:15 | B | 180.9 | 72.1 | 76.9 | 22.1 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 28.0 | 13.2-14.7 | 42.4 | 135 | fast | 208 |
| s514 | r1_1A | 10-11 04:47 | A | 181.4 | 69.2 | 76.8 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 31.5 | 13.1-14.7 | 40.4 | 124 | fast | 181 |
| s514 | r1_2B | 10-11 04:50 | B | 177.5 | 68.4 | 75.3 | 21.2 (16.3) | 25.9 | 10.6 | 17.4 | 2.4 | 29.6 | 13.1-14.4 | 39.1 | 118 | fast | 181 |
| s514 | r1_3B | 10-11 04:53 | B | 209.8 | 77.2 | 95.9 | 26.2 (21.4) | 33.4 | 13.4 | 22.8 | 2.2 | 33.1 | 10.3-11.4 | 50.3 | 92 | SLOW | 178 |
| s514 | r1_4A | 10-11 04:57 | A | 206.8 | 77.1 | 96.4 | 27.0 (21.2) | 33.1 | 13.2 | 22.6 | 2.2 | 29.7 | 10.2-11.7 | 48.5 | 131 | SLOW | 210 |
| s514 | r2_1B | 10-11 05:01 | B | 178.9 | 68.4 | 75.1 | 21.1 (16.3) | 25.9 | 10.5 | 17.4 | 2.3 | 32.0 | 12.9-14.4 | 40.7 | 107 | fast | 207 |
| s514 | r2_2A | 10-11 05:04 | A | 182.1 | 72.9 | 76.9 | 22.0 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 27.2 | 13.2-14.6 | 42.4 | 137 | fast | 179 |
| s514 | r2_3A | 10-11 05:07 | A | 208.0 | 76.0 | 97.9 | 28.1 (21.5) | 33.2 | 13.3 | 22.8 | 3.1 | 30.0 | 10.2-11.5 | 46.1 | 133 | SLOW | 182 |
| s514 | r2_4B | 10-11 05:11 | B | 178.9 | 70.3 | 75.2 | 21.1 (16.3) | 25.9 | 10.6 | 17.5 | 2.2 | 29.9 | 13.2-14.6 | 40.0 | 133 | fast | 208 |
| s514 | r3_1A | 10-11 05:14 | A | 206.6 | 75.8 | 97.0 | 27.2 (21.4) | 33.2 | 13.3 | 22.7 | 2.3 | 30.3 | 10.1-11.5 | 48.0 | 120 | SLOW | 179 |
| s514 | r3_2B | 10-11 05:18 | B | 205.3 | 76.3 | 94.7 | 25.9 (21.1) | 33.0 | 13.1 | 22.5 | 2.2 | 30.6 | 10.2-11.5 | 49.1 | 116 | SLOW | 207 |
| s514 | r3_3B | 10-11 05:22 | B | 178.9 | 70.6 | 75.4 | 21.2 (16.4) | 25.9 | 10.6 | 17.5 | 2.3 | 29.6 | 13.1-14.4 | 40.5 | 125 | fast | 205 |
| s514 | r3_4A | 10-11 05:25 | A | 180.5 | 70.2 | 76.7 | 22.0 (16.4) | 26.0 | 10.6 | 17.5 | 2.2 | 28.9 | 12.9-14.6 | 41.5 | 129 | fast | 179 |
| s514b | r1_1A | 10-11 07:27 | A | 231.0 | 84.7 | 94.8 | 28.6 (18.2) | 33.2 | 14.7 | 17.7 | 6.8 | 46.2 | 11.7-13.1 | 57.0 | 101 | fast | 181 |
| s514b | r1_2B | 10-11 07:31 | B | 275.2 | 90.4 | 126.2 | 39.4 (22.7) | 44.6 | 15.4 | 26.7 | 14.1 | 53.3 | 7.6-10.1 | 63.6 | 91 | SLOW | 231 |
| s514b | r1_3B | 10-11 07:36 | B | 192.8 | 77.3 | 79.9 | 22.5 (16.5) | 27.5 | 10.6 | 19.1 | 3.5 | 32.1 | 12.6-13.7 | 46.4 | 124 | fast | 275 |
| s514b | r1_4A | 10-11 07:39 | A | 213.6 | 79.7 | 96.9 | 26.9 (21.2) | 33.4 | 13.3 | 22.7 | 2.3 | 33.5 | 10.2-11.3 | 53.0 | 105 | SLOW | 193 |
| s514b | r2_1B | 10-11 07:43 | B | 187.9 | 74.0 | 81.8 | 27.6 (16.5) | 26.0 | 10.6 | 17.5 | 8.5 | 27.9 | 13.2-14.6 | 42.5 | 139 | fast | 214 |
| s514b | r2_2A | 10-11 07:47 | A | 189.2 | 71.6 | 81.4 | 26.5 (16.3) | 26.1 | 10.7 | 17.6 | 6.8 | 31.2 | 12.6-14.8 | 43.6 | 123 | fast | 188 |
| s514b | r2_3A | 10-11 07:50 | A | 285.4 | 74.8 | 171.1 | 110.7 (18.6) | 28.8 | 10.6 | 20.5 | 88.8 | 33.2 | 11.3-13.4 | 48.0 | 107 | fast | 189 |
| s514b | r2_4B | 10-11 07:55 | B | 181.2 | 68.9 | 75.5 | 21.2 (16.3) | 26.0 | 10.6 | 17.6 | 2.4 | 33.6 | 13.1-14.4 | 42.0 | 103 | fast | 285 |
| s514b | r3_1A | 10-11 07:58 | A | 201.8 | 74.0 | 93.4 | 35.6 (21.9) | 27.3 | 10.6 | 19.3 | 10.3 | 28.6 | 11.8-13.5 | 43.0 | 127 | fast | 181 |
| s514b | r3_2B | 10-11 08:02 | B | 179.1 | 72.1 | 75.7 | 21.2 (16.4) | 26.1 | 10.6 | 17.6 | 2.3 | 26.9 | 13.0-14.7 | 41.4 | 133 | fast | 202 |
| s514b | r3_3B | 10-11 08:05 | B | 179.4 | 69.5 | 75.3 | 21.2 (16.3) | 25.9 | 10.6 | 17.5 | 2.3 | 31.0 | 13.0-14.5 | 41.8 | 129 | fast | 179 |
| s514b | r3_4A | 10-11 08:09 | A | 182.5 | 72.6 | 76.9 | 21.9 (16.4) | 26.1 | 10.7 | 17.6 | 2.2 | 29.7 | 12.9-14.6 | 44.9 | 133 | fast | 179 |
| s514b | r4_1B | 10-11 08:12 | B | 214.3 | 76.1 | 103.2 | 33.3 (21.4) | 33.5 | 13.5 | 22.8 | 9.3 | 30.3 | 10.3-11.4 | 49.6 | 122 | SLOW | 182 |
| s514b | r4_2A | 10-11 08:16 | A | 188.7 | 70.0 | 84.1 | 29.4 (16.3) | 26.0 | 10.6 | 17.5 | 9.7 | 30.1 | 13.3-14.4 | 41.5 | 109 | fast | 214 |
| s514b | r4_3A | 10-11 08:19 | A | 213.9 | 76.7 | 103.1 | 33.0 (21.6) | 33.4 | 13.4 | 22.8 | 7.9 | 29.8 | 10.2-11.5 | 48.3 | 133 | SLOW | 189 |
| s514b | r4_4B | 10-11 08:23 | B | 214.9 | 76.6 | 103.4 | 34.3 (21.5) | 33.1 | 13.2 | 22.6 | 10.3 | 30.5 | 10.5-11.4 | 46.8 | 131 | SLOW | 214 |
| s514b | r5_1A | 10-11 08:27 | A | 182.2 | 70.7 | 76.7 | 21.9 (16.3) | 26.1 | 10.6 | 17.6 | 2.3 | 30.7 | 12.9-14.7 | 41.8 | 138 | fast | 215 |
| s514b | r5_2B | 10-11 08:30 | B | 245.4 | 71.5 | 140.8 | 86.9 (16.9) | 25.8 | 10.6 | 17.5 | 67.5 | 29.6 | 12.7-14.5 | 42.6 | 144 | fast | 182 |
| s514b | r5_3B | 10-11 08:35 | B | 208.6 | 73.6 | 95.6 | 39.2 (26.2) | 28.3 | 10.6 | 17.4 | 10.5 | 30.8 | 11.1-12.3 | 47.1 | 122 | SLOW | 245 |
| s514b | r5_4A | 10-11 08:39 | A | 242.6 | 78.7 | 120.8 | 45.5 (30.6) | 36.1 | 13.3 | 25.4 | 11.5 | 35.2 | 8.3-9.7 | 52.2 | 92 | SLOW | 209 |
| s514b | r6_1B | 10-11 08:43 | B | 187.6 | 71.6 | 82.2 | 22.2 (16.4) | 29.5 | 10.6 | 19.8 | 3.3 | 30.1 | 12.1-13.5 | 44.0 | 118 | fast | 243 |
| s514b | r6_2A | 10-11 08:46 | A | 188.0 | 69.5 | 81.3 | 26.3 (16.6) | 26.1 | 10.7 | 17.6 | 6.4 | 33.2 | 13.2-14.4 | 42.3 | 107 | fast | 188 |
| s514b | r6_3A | 10-11 08:50 | A | 182.7 | 68.6 | 76.8 | 22.0 (16.4) | 26.0 | 10.7 | 17.6 | 2.3 | 33.1 | 12.8-14.1 | 41.8 | 92 | fast | 188 |

## T09 - the bimodal 4-node totals (2026-10-11, aac7, session task t09)

(former file: T09.md)

Question: why do 4-node runs at 6.441e10 digits/node (2.5764e11 digits) come out at 180-186 s in one run and 207-213 s (S47/S48) in the next, on the same hosts?

#### Setup
- Code: origin/main ceb94c1, aac7 worktree `~/ntt-wt/t09`, built with `make -B SHMEM_CRAY=1` (links libsma). No code change; this branch holds only this file.
- Hold: job 12507 (`-J s9b4`, 4 nodes aac7-n10, s1b1n0, s2b0n0, s2b1n0, 60 min, `-x aac7-n01,aac7-n61`), 11:30-12:18 EDT. Released; no step or job of ours left.
- Launch: the aac7 line of `ecalc/e16_headline.sh` (parsed) + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit output, reference files evicted (`posix_fadvise DONTNEED`) before each run, runs back to back. 10 runs completed with VERIFY OK, 2 aborted by the memory guard (see below).
- Driver and raw logs: aac7 `~/sess1011b/t09/` (`summary.tsv`, `log/`, `pre/` = per-node meminfo/buddyinfo before each run, `parse.txt`). The logs of runs 1-2 were lost (a labelling bug overwrote them); their totals are in `summary.tsv`.

#### Runs (totals measured; stage times from the `total` line)
| # | start EDT | variable | total s | init | bs | recip | dm | bs rate GB/s per PE (post) | dm rate GB/s per PE |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 11:31 | base (rank order = node order, arbitrary dist.) | 247.6 | - | - | - | - | - | - |
| 2 | 11:35 | base | 257.7 | - | - | - | - | - | - |
| 3 | 11:39 | base | 233.8 | 39.7 | 79.5 | 33.6 | 110.9 | 11.8 12.0 12.2 11.2 | 8.8 9.8 10.5 9.7 |
| 4 | 11:44 | plain launch (srun block order, as S48) | 224.2 | 32.3 | 84.2 | 28.6 | 103.4 | 11.8 12.6 12.5 11.3 | 9.3 10.3 11.2 10.3 |
| 5 | 11:48 | base | 184.4 | 28.3 | 70.7 | 24.9 | 80.6 | 14.2 15.2 14.9 13.6 | 13.0 14.7 15.8 14.6 |
| 6 | 11:51 | plain | 185.1 | 30.4 | 69.7 | 25.4 | 80.6 | 14.2 15.2 15.0 13.7 | 13.0 14.7 15.8 14.6 |
| 7 | 11:54 | base | 181.1 | 29.6 | 70.0 | 22.1 | 77.1 | 14.2 15.2 15.0 13.7 | 13.1 14.8 15.9 14.8 |
| 8 | 11:57 | rank order rotated by 2 | 182.5 | 30.5 | 69.9 | 22.6 | 77.6 | 14.1 15.2 15.0 13.6 | 13.1 14.8 16.0 14.7 |
| 9 | 12:01 | 60 s pause before the run | 183.0 | 29.9 | 70.6 | 22.1 | 77.2 | 14.2 15.2 15.0 13.6 | 13.1 14.8 15.9 14.7 |
| a | 12:05 | 80 GB file written to /tmp on every node | aborted rc 9 | | | | | | |
| 10 | 12:06 | plain, after the aborted run and the file's removal | 209.0 | 30.7 | 76.1 | 27.2 | 97.4 | 11.5 12.4 12.5 11.5 | 9.9 11.3 12.0 11.1 |
| b | 12:10 | same fill again | aborted rc 8 | | | | | | |
| 11 | 12:11 | plain | 182.8 | 29.9 | 72.3 | 22.0 | 76.8 | 14.1 15.1 14.9 13.6 | 13.1 14.8 15.9 14.7 |

The fill (the test of "more page cache") could not be done: a 80 GB file written to /tmp raised Cached to 165 GB and MemAvailable fell to the 6 GB guard in the run itself (the guard stopped it, rc 9/8). The page cache from this file is not reclaimable here, so that variable is untested.

#### What differs in a slow run (measured)
- Slow: runs 1-4 (224-258 s) and 10 (209 s). Fast: runs 5-9, 11 (181-185 s). The fast mode is stable to +-2 s.
- Bytes moved are identical in both modes (xstats GB per PE the same). The exchange rate (GB / t_post) of every PE drops by the same factor: dm 0.70-0.72 (e.g. 13.0 -> 9.3 GB/s on PE 0, 15.8 -> 11.2 on PE 2), bs 0.81-0.84. The PE ordering (PE 2 fastest, PE 3 slowest in bs) is the same in both modes. So the slow mode is a uniform slowdown of all four nodes' exchanges, not one node, one rank or one link.
- Stage deltas, run 4 vs run 5 (slow vs fast): dm +22.8 s, bs +13.5 s, recip +3.7 s, init +4 s (run 3: dm +30, bs +9, recip +9, init +10). Exchange-dominated stages carry the loss; the per-node `wait` times in `wait-stats` rise on all four nodes together (dm wait 172-184 s against 146-160 s); the `barrier` time is 0 everywhere, so no node waits at a barrier for another.
- The mode is not tied to: the node set (same four hosts throughout), the rank-to-node order (run 8 rotated by 2: fast), the launch form (plain srun and arbitrary distribution both slow, then both fast), a 60 s pause (run 9: fast), the eviction (done every run).
- The per-node meminfo/buddyinfo taken before each run does not separate the modes: run 10 (209 s) had the same MemFree/Cached/free-block counts as run 11 (183 s). At the hold's start (before run 1) Cached was 160 GB (MemFree 360 GB); it fell to 88-97 GB before run 3 and 81-83 GB from run 5 on, MemAvailable 440 GB, i.e. the slow runs 1-3 coincide with extra cached memory still being reclaimed.

#### Explanation and evidence level
- Established (measured): the slow mode is a ~25-30% lower rate of the node-to-node exchanges on all four nodes at once, for the same bytes, lasting several consecutive runs (runs 1-4 = 17 min after the hold began; run 10 alone) and then gone. Not a node, rank, rank order, launch form, or pause effect.
- Not established: the cause of the uniform slowdown. Candidates, with what the data says:
  1. Concurrent fabric traffic from other jobs of the same session: sacct shows 2-node jobs t10 (11:32-11:52) and t08 (11:53-11:56), then t01 (from 11:56) on aac7-n00/s0b1n0 (same chassis c1 as our nodes), but their run times inside these windows were not read (other agents' files); the 17 min slow period ends at 11:47, before t10's job end. No other users' jobs appear in `sacct -a` for the period. Possible, unproven.
  2. Host-memory state after the allocation starts (extra 70-80 GB of cached pages freed during runs 1-3, and a similar disturbance before run 10): consistent with the README's note that memory pressure slows the host-staged exchanges 12-19x in extreme cases, but run 10's pre-run snapshot was normal, so this is not shown.
- Practical consequence: the first runs of a fresh 4-node hold (about 15-17 min here, 4 runs) are in the slow mode; timing A/B pairs should discard or randomise over them (ABBA rounds already do) and report the mode of each run (the exchange rate in the `xstats` lines of `bs redist` / `dm` identifies it directly: dm >= 13 GB/s on PE 0 = fast, <= 10 = slow).

#### Open
- Test candidate 1 directly: run a second 4-node hold while nothing else of ours runs on aac7 (needs the session to be quiet), and a hold whose first run is preceded by a controlled memory fill that stays reclaimable (e.g. reading a file under ~60 GB, not a write to /tmp).
- The logs of runs 1-2 are lost; the label bug (`t09_run.sh` reused the label for repeated specs) is fixed in the driver.

#### RESUME
Branch `t09` pushed; the aac7 worktree `~/ntt-wt/t09` and the scratch `~/sess1011b/t09/` are kept for the coordinator; hold 12507 is released.

## A13 - 4-node slow mode on a quiet aac7 (part A) and the DIST_LOGN_TEST 4-node "65 corrections" (part B)

(former file: A13.md)

Date 2026-10-11 (times Eastern), aac7, main 6d80ef1 (worktree `~/ntt-wt/a13`, production defaults), `tools/gate.sh --build --nodes 1`:
PASS (1e9, VERIFY OK, digits identical, `-DCOMM_SHMEM` in build.log). One hold, job 12531 (`sbatch -N4 --exclusive --gpus-per-node=4
-x aac7-n01,aac7-n61`, nodes aac7-n20, s2b1n0, s3b0n0, s3b1n0), 14:35 to 15:30 = 55 min of 4-node time (measured).
No code change. Driver, probe and snapshot scripts: `archive:drivers/a13/` (the driver is `t12_drive.sh` of the t12 branch plus probes and NIC
snapshots; it needs `t12_stats.py` from there). Raw NIC snapshots and logs stay on aac7 in `~/sess1011c/a13/`.

#### Part A - is the slow mode fabric state or set at process start?

Setup (measured): 6 back-to-back real runs, 4 x 6.441e10 digits/node, the e16_headline aac7 line + `FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1
MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit writes, page-cache eviction before each run. The other agent's job (a12) had ended at 14:38, before the
first run; `squeue -a` and `sacct -a` show only our own jobs for the whole hold (no other user's job visible on the cluster at all). Between
runs a stand-alone 4-node probe `tests/t_comm --bw 4 250 256` (symmetric buffers, 4 threads, 250 reps; 256 MiB and 64 MiB slabs; about 43 s
including the launch), launched like a real run (mnrun, `FI_MR_CACHE_MONITOR=userfaultfd`), never concurrent with a run. NIC counters:
`/sys/class/cxi/cxi*/device/telemetry/*` (about 1900 counters per NIC) and `ethtool -S hsn*` read on all 4 nodes (readable without privileges) before
and after every run and after every probe.
Only 6 runs fitted (about 6.5 min per run+probe+prep cycle); the plan said about 8.

| run | start | total s | init | bs | dm | all-to-all GB/s per thread | mode | OXE MCU group during run | probe before (256 MiB / 64 MiB) | probe after |
|---|---|---|---|---|---|---|---|---|---|---|
| r001 | 14:46 | 220.6 | 47.6 | 78.2 | 91.6 | 12.00 | mid | B | 10.44 / 12.18 | 10.37 / 12.21 |
| r002 | 14:57 | 197.6 | 35.7 | 71.0 | 86.1 | 13.10 | fast | B | 10.37 / 12.21 | 10.10 / 10.68 |
| r003 | 15:03 | 196.8 | 37.7 | 70.7 | 84.6 | 13.07 | fast | B | 10.10 / 10.68 | 10.48 / 12.15 |
| r004 | 15:09 | 220.0 | 34.9 | 76.0 | 104.0 | 10.99 | **slow** | **A** | 10.48 / 12.15 | 10.60 / 11.91 |
| r005 | 15:16 | 200.2 | 33.9 | 75.5 | 87.5 | 11.60 | **slow** | **A** | 10.60 / 11.91 | 10.33 / 8.78 |
| r006 | 15:23 | 182.0 | 32.1 | 69.4 | 77.0 | 13.68 | fast | B | 10.33 / 8.78 | 9.84 / 11.06 |

All 6 runs rc 0, VERIFY OK, no segv/hang (measured). 2 of 6 slow (33 %; earlier 29 %) on a cluster with nothing else of ours or anyone's running.
The mean slow-run total is 220 s / 200 s against 182-198 s for the fast ones; r001 (mid, 12.00) behaves like a fast run in the counters.

##### Findings (measured unless stated)
1. **Probe rate does not predict the run.** The probe before each run read 10.44, 10.37, 10.10, 10.48, 10.60, 10.33 GB/s for the runs
   mid, fast, fast, slow, slow, fast: the two slow runs had the two highest "before" probes. The probe after a slow run (10.60, 10.33) is not lower than after
   a fast one (10.37 to 10.48). The 256 MiB probe rate (about 10.1-10.6, a different accounting from the run's 13.8) never shows a 0.79 x drop.
2. **The NIC counters do separate the slow runs, and they point at the launch, not the fabric.** Of about 1200 counters that changed, the ones that
   correlate with the run's all-to-all rate (6 runs, so only indicative) are:
   - Fine-grained flow control (`oxe_stall_fgfc_start/_end/_cntr/_blk`, summed over the 4 NICs x 4 nodes): about 2x in the slow runs
     (`oxe_stall_fgfc_start_0` 2.97e8 and 3.74e8 against 1.43-1.47e8 in fast/mid runs; the fgfc counters are identical across the four `_0.._3` copies);
     `oxe_stall_pbuf_bc_0` 1.25e11 / 0.96e11 against 0.85-0.86e11 (slow vs fast); `hni_tx_stall_llr` and `pct_hni_stalls` 3-6 % lower in slow runs.
     So the slow runs see more end-to-end flow-control back-pressure (the receiving side or the path is the bottleneck), not link-level errors:
     no uncorrected codewords, `corrected_cw` rises only with run length (corr -0.9 is the duration).
   - **`oxe_mcu_meas_18..25` versus `oxe_mcu_meas_28..33`** (the NIC's OXE message-control-unit measurement counters). Every run puts the byte traffic
     (3.7e10 per counter, all 4 NICs on all 4 nodes together) in exactly one of the two groups, the same group on every NIC of every node:
     group B (28-33) in r001, r002, r003, r006 (the fast and mid runs), group A (18-25) in r004 and r005 (the two slow runs). 6 of 6 runs agree with the slow/not-slow split
     (Fisher exact, one-sided: 2 of 2 slow in A, 0 of 4 others, p = 1/15 = 0.067; not significant at 0.05 with 6 runs). I do not know what the groups are in
     the NIC firmware (assumed: which MCU set the endpoint's traffic is hashed to).
3. **The A/B group is drawn per launch, independent of the previous process.** The probes (separate launches) fall into A or B too: p002, p005, p006 in A;
   p000, p001, p003, p004 in B. The probe's group does not predict the next run's group (p001 B to r002 B; p002 A to r003 B; p003 B to r004 A;
   p004 B to r005 A; p005 A to r006 B), and the group changes between consecutive processes with no time gap.
   A-group probes read lower than B-group probes: 64 MiB slabs 10.2 vs 12.1 GB/s (A: 10.68, 8.78, 11.06; B: 12.18, 12.21, 12.15, 11.91), 256 MiB slabs
   10.09 vs 10.47 (A: 10.10, 10.33, 9.84; B: 10.44, 10.37, 10.48, 10.60). So the slow mode is **reproducible by a different, tiny program**, which
   supports "state set when the process (its NIC endpoints/communicator) is created" and argues against a time-varying fabric or host-environment state.

##### Answer
The all-to-all rate of a run is not predicted by the probe launched before it, nor by the state of the fabric around it; it is set per launch
(the traffic lands in one of two NIC MCU groups, and the group-A launches are slow in the probe as well as in the real runs). With only 6 runs and
2 slow ones this is indicative (p = 0.067 for the perfect split), not proven. Quiet cluster: still 2 of 6 slow, so other jobs are not the cause.

##### Suggested next step (not done)
- Find what picks the group at launch: log `oxe_mcu_meas` per NIC in the real runs and in `t_comm --bw` over many cheap launches (a 20 s probe, no
  4-hour run needed: p(A) about 0.4 here, so 30 launches give about 12 A-launches), then test candidates that differ per launch (CXI service/VNI or
  traffic-class and endpoint-to-NIC-queue assignment in comm_ofi / SHMEM init, the order of NIC opens, `FI_CXI_*` settings). If the cause is a
  launch-time choice, a retry-at-init (relaunch when the first all-to-all measures slow) would recover about 20 s in 30 % of runs.

#### Part B - DIST_LOGN_TEST 4-node "65 corrections": **not reproduced** on main 6d80ef1 at 4 nodes

Runs (measured, 4 nodes x 4 APUs, SHMEM, aac7 line unless noted, VERIFY OK on all 4 nodes, 0 corrections reported by the division in every run;
logs `~/sess1011c/a13/B/`):

| digits | env | grid (`dist_mn ... cap`) | total s | result |
|---|---|---|---|---|
| 1e9 | `RNS_INIT_POOL_LOG=29 DIST_LOGN_TEST=23` | cap 2^24/2^25 (grids 1x4, 2x2, 2x3 pieces) | 20.4 | VERIFY OK, corrections 0/0 |
| 1e9 | `POOL_LOG=29 DIST_LOGN_TEST=23` | same | 13.6 | VERIFY OK |
| 1e9 | `RNS_INIT_POOL_LOG=29 DIST_LOGN_TEST=22` / `=21` / `=24` | 2^23 / 2^22 / 2^26 | 21.4 / 28.1 / 17.9 | VERIFY OK |
| 1e10 | `POOL_LOG=29 DIST_LOGN_TEST=27` / `=26` | 2^28 / 2^27 (X 5.6e8 limbs) | 17.9 / 19.3 | VERIFY OK |
| 1e9 | `NEWTON_DKM=0` (the older non-DKM `newton_mn_divmod`, the path G12 ran) `POOL_LOG=29 DIST_LOGN_TEST=23` / `=24` | 2^24 / 2^26 | 14.2 / 12.4 | VERIFY OK |
| 1e9 | no aac7 line (COMM_TRANSPORT=shmem only), `POOL_LOG=29 DIST_LOGN_TEST=23` / `=22` | no `dist_mn` lines printed (grid not shown, so not shown to be forced) | 13.5 / 15.4 | VERIFY OK |
| 1e9 | plain TCP over 1 GbE, `DIST_LOGN_TEST=23` | - | - | killed by me after about 9 min in the reciprocal (known slow TCP reciprocal, G12 open issue); inconclusive |

So at 1e9 caps 2^22-2^26 and 1e10 caps 2^27/2^28 with the current default DKM division and with the old division, 4 nodes do not hit the guard.
The G12 failures ("65 corrections" at 10^9 / 10^10 with DIST_LOGN_TEST 23 / 27) were seen on `main`'s binary of that time on aac6 (4 processes on loopback TCP, before
the DKM division was the default, before X1/X2/layered exchange changes); the 1-node A05 check (rows 22-24 pass, 21 and below stop at the `split_grid`
limit) is consistent. No divergence to localise.

**Hypothesis (assumed, not tested):** the defect lived in the TCP/aac6 path or in the pre-DKM division at a forced small cap and was removed by later changes, or it needs the TCP transport
(the product at the forced grid being wrong only there). The guard `++nc > 64` in `mn_dkm_corr` / `newton_mn_divmod` fires when the quotient is off by more than
64 limbs-units, i.e. a wrong product or reciprocal at a level, not a loop bug. The way to settle it is an aac6 or 4-process-per-node TCP rerun of G12's batch 8 (10^9, `POOL_LOG=29 DIST_LOGN_TEST=23`,
4 processes) on this main; if it passes, TASKS 1.6 can be closed as "not reproducible on main"; if it fails, `ECALC_RES_LOG=1` (per-level P/Q residue checks, `tree_check`) is
the existing debug output to localise the level.

#### RESUME
Nothing running; hold 12531 cancelled, `squeue -u <user>` empty, worktree `~/ntt-wt/a13` removed. Branch a13 (docs and drivers only). Open: (1) launch-to-launch MCU-group experiment above;
(2) aac6 TCP 4-process rerun for part B.

## A14 - what picks the "slow mode" NIC group at launch? (aac7, 2026-10-11)

(former file: A14.md)

#### Result in one paragraph
At 2 nodes the NIC message-control-unit (MCU) counter group (A = oxe_mcu_meas 18-25, B = 28-33) is drawn per launch with p(A) about 0.5 (measured, 44 A of 86 launches),
but the group does NOT change the 2-node probe rate (64 MiB slabs: A 6.59 +- 0.42, B 6.59 +- 0.26 GB/s per thread, measured, n = 44 / 42). So the A13 slow mode
(4 nodes, A: 10.2 vs B: 12.1 GB/s) was not reproduced at 2 nodes; whether group -> rate holds at 4 nodes was NOT retested (a 4-node run was cancelled on the
coordinator's directive: aac7 is scarce; the budget also was full). No env setting pinned the group (all settings below still gave both A and B).

#### Tests (measured; 2 nodes, `t_comm --bw 4 100 64`, one launch about 13 s, snapshot of oxe_mcu_meas_10..40 + fgfc around each launch)
Driver `archive:drivers/a14/` (a14_drive.sh, wrap.sh); raw data aac7 `~/sess1011c/a14/{R,R2}/launches.tsv`.
| config (env added) | launches | A | B | mean rate64 A / B (GB/s per thread) |
|---|---|---|---|---|
| none (2 labels pooled) | 22 | 9 | 13 | 6.53 / 6.65 |
| SHMEM_OFI_DEFAULT_TCLASS=TC_BULK_DATA | 12 | 8 | 4 | 6.59 / 6.54 |
| SHMEM_OFI_DEFAULT_TCLASS=TC_LOW_LATENCY | 12 | 8 | 4 | 6.55 / 6.49 |
| none (second job) | 8 | 3 | 5 | 6.49 / 6.54 |
| SHMEM_OFI_STARTUP_CONNECT=1 | 8 | 3 | 5 | 6.60 / 6.70 |
| SHMEM_OFI_NUM_NICS=1 | 8 | 4 | 4 | 6.95 / 6.45 |
| SHMEM_OFI_NIC_POLICY=ROUND-ROBIN | 8 | 4 | 4 | 6.59 / 6.60 |
| SHMEM_OFI_USE_SEP=1 | 8 | 5 | 3 | 6.55 / 6.63 |
| SHMEM_OFI_CXI_PID_BASE=64 | 8 | - | - | launch failed (no output; value invalid or unsupported) |
Traffic-class runs show A 16/24 vs 9/22 without (Fisher about 0.1, not significant) - no pin, if anything the opposite.

#### Source reading
- ecalc uses Cray OpenSHMEMX 11.8 / libfabric 2.3.1 CXI; `comm_shmem.c` makes one SHMEM context per communicator (per APU thread); no tclass/VNI/service is set by ecalc.
- libfabric CXI knobs present (fi_info -e): FI_CXI_DEFAULT_VNI, FI_CXI_RGET_TC, FI_CXI_CQ_POLICY, FI_CXI_LLRING_MODE, FI_CXI_DEVICE_NAME, FI_CXI_RX_MATCH_MODE, ...; none documents MCU selection.
  SHMEM: SHMEM_OFI_DEFAULT_TCLASS (TC_UNSPEC/BEST_EFFORT/DEDICATED_ACCESS/LOW_LATENCY), NIC_POLICY/NIC_MAPPING/NUM_NICS, USE_SEP, STARTUP_CONNECT, CXI_PID_BASE (undocumented).
- Assumed (not shown): the group follows which NIC hardware resources (command queue / pid) the launch's endpoints receive from the driver's free list, outside app control.

#### Detect-and-relaunch (modelled from A13 numbers)
A13 4-node runs: init 32-48 s (mean about 37), slow runs 200-220 s vs fast 182-197 s: loss of a slow run about 20 s (measured, 2 slow of 6). A relaunch after init costs >= 37 s per A launch,
more than the 20 s it saves, even at p(A)=0.4-0.5: not worthwhile. A separate pre-probe (about 15-20 s) costs 15-20 s on every launch to save 20 s on 40 % of them: break-even at best.

#### Open
4-node probe loop (30+ launches, base only) to test group -> rate there: driver ready (`wait4.sh` logic, not run). Needs one 4-node job of ~8 min.

#### 4-node probe (queue item 40_a14_probe4, ran 16:23-16:32 ET, 31 launches, 16-21 s each; measured)
Raw: aac7 `~/sess1011c/a14/R4/launches.tsv`, `summary.txt`.
| group | launches | mean rate64 (GB/s per thread) | range |
|---|---|---|---|
| A (oxe_mcu_meas 18-25) | 16 | 10.62 | 8.81-11.79 |
| B (28-33) | 15 | 12.02 | 11.86-12.29 |
Every B launch is above every A launch except one A at 11.79 (B min 11.86): the split is clean. A is 1.4 GB/s (about 12 %) slower, ~52 % of launches are A. This confirms A13: at 4 nodes
the group sets the all-to-all speed (A also has 2x fgfc stalls), while at 2 nodes it does not. No setting pins the group (2-node trials above; none tried at 4 nodes), relaunch loses (modelled above).
Welch/CI not computed beyond the table (group means differ by 1.4 with spread 0.8 in A, 0.1 in B: far outside any CI).

#### RESUME
Queued (not yet run when written): `~/aac7q/queue/41_a14_pin4.sh` (copy in `archive:drivers/a14/`; 4 nodes, EST 12 min, STUB=1 dry run passed): 10 interleaved rounds of
{SHMEM_OFI_DEFAULT_TCLASS=TC_LOW_LATENCY, SHMEM_OFI_NUM_NICS=1}, `t_comm --bw 4 100 64`, group + rate per launch, 540 s cap, exits by itself.
Collect: `ssh aac7 'cat ~/sess1011c/a14/P4/summary.txt; wc -l ~/sess1011c/a14/P4/launches.tsv'` (lines "<cfg> <A|B> n= mean64="; raw in P4/launches.tsv, cols as above).
Analysis: a setting PINS if all its launches are group B (or A) over >= 8 launches (chance of 8 same-group at p=0.5 is 0.8 %); it HELPS if all-B and rate64 about 12.0 (B baseline above).
NUM_NICS=1 may lower the rate for its own reasons: judge it by group AND rate against the 4-node baseline (A 10.62 / B 12.02). Append the table here; adopt nothing (user decides).
Done so far: 2-node tests, 4-node baseline (above).

#### P5: do identical settings land in opposite NIC groups? (queue item 80_f1_a14cfg4, 4 nodes, 19:02-19:10 ET; collected by f5; measured)
Raw: aac7 `~/sess1011c/a14/P5/launches.tsv`, `summary.txt`. 32 launches of `t_comm --bw 4 100 64` in shuffled rounds of {n1: no env, n2: no env (identical to n1), tcll: `SHMEM_OFI_DEFAULT_TCLASS=TC_LOW_LATENCY`, nn1: `SHMEM_OFI_NUM_NICS=1`}.
| cfg | group A (launches, mean rate64) | group B (launches, mean rate64) |
|---|---|---|
| n1 (no env) | 5, 10.78 | 3, 11.97 |
| n2 (no env, identical to n1) | 4, 10.48 | 4, 11.97 |
| tcll | 5, 10.10 | 3, 12.02 |
| nn1 | 4, 10.94 | 4, 12.01 |
- **Yes: the two identical no-env settings land in both groups (n1 5 A / 3 B, n2 4 A / 4 B), and inside a round they can differ (round 1: n2 = A, n1 = B; round 3: n2 = B, n1 = A).** Every setting is split about 50/50 (18 A, 14 B in total); the P4 pattern (nn1 = A, tcll = B in rounds 1-7) was chance. The group, not the setting, sets the rate: B 11.97-12.02 GB/s for all four, A 10.1-10.9.
- Conclusion: no tested setting pins or avoids the slow group; group A costs about 1.1-1.9 GB/s (10-15 %) per thread at 4 nodes, as in P4 (10.62 vs 12.02). Detect-and-relaunch stays not worthwhile (section above). Nothing to adopt.

## B04 - fast node screen before the long run (2026-10-11, Wave B)

(former file: B04.md)

Status: prototype `tools/nodescreen.sh` (+ `tools/nodescreen_tab.py`, `tools/initchk.py`) tested on aac7 (4 pairs, 12 screens) and aac6 (1 node). Not run at scale (the
allocation has 2 nodes at a time); every scale figure below is labelled assumed. Decision input: the user chose "no spare nodes, exclude very bad nodes only".

#### 1. Design: what a quick screen can and cannot catch
One process per node, all nodes at once (`srun`, one task per node). Two stages run one after the other, so a node never has two network programs:
- **local** (no network): `tests/t_nodechk 1 3 <cold-gb>` (meminfo, 1 GB upload per APU x3, T10 cold path of `--cold-gb` GB per APU: VMM map + first touch + cold upload),
  then one single-node `ecalc` of `--local-digits` (default 1e8) with `ECALC_INIT_TL=1 ECALC_VERBOSE=2`: `loc_init` = rns_init, `loc_post` = wall - init (compute and first uploads), `loc_wall`.
- **mn** (one SHMEM job over the nodes the local stage kept): `mnrun.sh` ecalc of N x `--mn-digits-per-node` (default 1e8) with `ECALC_INIT_TL=1 NEWTON_MN_RECIP_TS=2`,
  log labelled (`SLURM_LABELIO=1`), read by `initchk.py`'s parser: `mn_init` (rns_init per node) and `mn_up` (host-attributed `db_from_bi` upload of the reciprocal stage, the stage where aac7-n01 stalled). The tiny run does reach the reciprocal stage (the `recip ts2` lines are printed).
Rule (as initchk): flag a time metric > 3x the reference (median; the smaller value if fewer than 3 nodes) AND >= 2 s above it; any failed / timed-out / missing step flags the node. EXCLUDE = flagged hosts; exit 0 / 3 / 2.

| failure mode | probe | cost (measured, 2 nodes) | caught? |
|---|---|---|---|
| dead GPU / hipMalloc or upload ERR / node script lost or timed out | nodechk ERR, step rc, no log | included below | yes (FAILED) |
| host memory / page-cache state | MemAvailable, Cached from meminfo (table column `avail`) | 0 s | shown; excluded only with `--min-avail-gb` (off: low MemAvailable alone was not a stall, S49) |
| slow host->device path on every upload | 1 GB upload per APU x3 (`nc_up`, 0.02 s healthy) | part of 4.1-7.7 s nodechk step | yes if >= 2 s slower, i.e. a gross fault only |
| slow VMM map / first touch / cold upload (init path) | cold 4 GB per APU (`cold` 0.51-0.58 s, `touch` 0.66-0.76 s healthy) | same step (cold 16 GB: +8 s, T10 12 s) | gross faults only; 2 s excess needed |
| node slow in init | `loc_init` 9.6-14.7 s healthy, `mn_init` 7.2-8.9 s | 15-24 s local ecalc | yes (3x and 2 s) |
| node slow in a short real compute / first uploads | `loc_post` 7.6-9.6 s healthy (`loc_wall` 14.9-23.7 s) | same ecalc | yes if >= 3x: a 20 s stall is 2x loc_wall but 3x+ loc_post, hence the `loc_post` column |
| first-upload stall of the known host (db_from_bi) | `mn_up` from the tiny multi-node run (0.00-0.01 s healthy here) | mn stage 18.7-25.3 s | **unproven**: the host did not stall today (below), and a 2e8-digit run uploads little; if the stall needs large buffers or a large page cache it will be missed |
| NIC / link bandwidth per node | none | - | **no**: the tiny run moves too little data to give a per-node rate. `archive:removed_2026-10-12/ecalc/tests/t_nicbw.c` (libfabric ring, 5 s per NIC set) could be run as a third stage after the mn stage (not integrated, needs a libfabric build) |
| fabric-wide slow mode (T05/T09: every node's exchanges slow together) | mn stage wall only | - | **no attribution**: it is a state of the allocation/fabric, not a node. The screen prints the mn wall; compare it with a baseline of the same size on the same system |
| stalls that appear minutes into a run, thermal throttling under sustained load, leaks, intermittent shmem_init segfault (13 % of launches, A16) | - | - | **cannot be caught quickly**; a segfault or hang in the mn stage shows as mn rc != 0 (no node is excluded for it) |

Per-probe and per-screen walls (measured on aac7, 2 nodes; run1-run7 in `~/b04/` on aac7):
| piece | wall |
|---|---|
| nodechk step (1 GB x3 per APU + cold 4 GB per APU), per node | 4.1-7.7 s |
| local ecalc 1e8 (init 10.8-14.7 s + 7.6-9.6 s) | 18.7-23.7 s |
| local stage (srun, both steps, whole allocation) | 24.5-32.5 s |
| mn stage (mnrun incl. plan call, SHMEM init, 2e8 digits) | 18.7-25.3 s |
| full screen, 2 nodes | 44.1-57.9 s (5 screens) |
| lean screen `--local-digits 0` (nodechk + mn) | 25.3 s |
| `--no-mn` (nodechk + local ecalc), aac6 1 node | 20.9 s |
At 576 nodes (assumed, not measured): srun fan-out, module load and 576 log files on NFS add maybe 10-40 s; the mn stage adds SHMEM init and pool setup at 576 PEs (the A16 init segfault rate applies). Expect 1-2 min for the full screen, 0.5-1 min lean; the worst case on a hang is `2 x timeout-local + 30` + `timeout-mn` (default 270 + 240 s).

#### 2. Tests (measured)
| where | nodes | screens | result |
|---|---|---|---|
| aac7 job 12535 (15:39-15:41 EDT) | aac7-n00 + **aac7-n01** (the known slow host) | 2 | rc 0, nobody flagged; aac7-n01: nc_up 0.02, cold 0.53, loc_init 11.2, loc_post 7.6-8.7, mn_init 7.2-8.1, mn_up 0.01 s, avail 439-440 GB: indistinguishable from its partner |
| aac7 job 12538 | the same pair again | 2 (second one with `--timeout-local 8 --no-mn`: failure path) | normal screen rc 0 (44.6 s); the timeout screen flagged both (`FAILED: ecalc rc=124`), EXCLUDE written, rc 3 |
| aac7 job 12536 | aac7-n40 + s4b1n0 | 1 | rc 0, 55.5 s; loc_init 13.6-14.7, mn_init 8.4-8.8 |
| aac7 job 12537 | aac7-n50 + s5b1n0 | 1 | rc 0, 57.9 s; loc_init 13.1-14.3, mn_init 7.5-8.4 |
| aac7 jobs 12543/12544 | s4 pair | 2 lean (`--local-digits 0`) | rc 0, 25.3 s (the first of the two ran with an empty `DIGM` bug, fixed) |
| aac6 job 22907 | aac6-spx-b only | 1 (`--no-mn`, no Cray SHMEM) | rc 0, 20.9 s; loc_init 9.55, loc_post 5.3, avail 516 GB |
| synthetic (CPU) | 8 fake nodes from a real log | 1 | slow init 3.7x flagged, ecalc rc 1 flagged, truncated log flagged, missing log flagged, exit logic as designed |
- **Does aac7-n01 stand out today? No (measured).** Same as T10/S49: the host is normal now (MemAvailable 439 GB, Cached ~160 GB at S49). The screen has therefore NOT been shown to catch the stall (no positive control); its failure path and rule were exercised only with a timeout and with synthetic logs.
- aac6: s24-16 was held by another agent's job (b05cpu), so the requested 2-node test was done on 1 node; the 2-node job stayed pending 2 min and was cancelled by me. aac6 has no SHMEM, so the mn stage cannot run there.

#### 3. Findings outside the files
- **mnrun.sh bug (owner b01)**: `MNRUN_LABEL=1` is ignored when `MNRUN_CPUS_PER_TASK` is set (aac7env.sh exports `auto`): lines 72-73 assign `copt=(-c ...)` instead of appending, dropping `--label`. Then target_kit's initchk stage reads an unlabelled log (anonymous columns, host names only on upload lines). nodescreen.sh works around it with `SLURM_LABELIO=1`; the one-line fix is `copt+=(-c ...)`.
- `ecalc 1e8` init is 11-15 s on aac7 (plane pools 5.6 s, staging 3.5 s): the local ecalc is dominated by init; `--local-digits 0` drops it (lean screen).

#### 4. If k nodes are excluded: can the run proceed on 576 - k nodes? (3.71e13 digits; MN_PLAN_ONLY / BS_LAYOUT_ONLY on the aac7 login node with the aac7 launch line + ECALC_NODE_GB=480, BS_ARENA_ROOM=0.16; estimate.py; modelled)
**Yes, any node count passes `plan check ... OK`** (1236 / 1234 / 1232 / 1230 / 1228 products for 576 / 575 / 574 / 573 / 572; the same P, Q of 2 061 111 111 115 limbs; digits per node 6.441e10 -> +0.17 % per excluded node). **But the default group schedule is "the powers of two dividing g, then the odd part's prime factors", so a bad count is very expensive**; an explicit `MN_GROUPS=2,4,8,16,32,64,192,<g>` (uneven last groups, accepted by the planner) removes that.
| g | default schedule (levels) | pieces total (node 0 / largest group) | modelled wall, no write | explicit `MN_GROUPS=...,<g>`: pieces (largest group) | modelled wall | model node GB |
|---|---|---|---|---|---|---|
| 576 | 2,4,8,16,32,64,192,576 | 103 (107) | 189.1 s | - | 189.1 s | 399 |
| 575 | 5,25,575 (5x5x23) | 173 (175) | 252.8 s (+34 %) | 103 (151) | 216.6 s (+15 %) | 432 |
| 574 | 2,14,574 (2x7x41) | 237 (241) | 311.6 s (+65 %) | 103 (143) | 212.1 s (+12 %) | 428 |
| 573 | prime-ish (3x191) | - | 887 s (+369 %) | 105 (135) | 199.1 s (+5 %) | 430 |
| 572 | 2,4,44,572 (4x11x13) | 143 (143) | 226.2 s (+20 %) | 105 (131) | 203.0 s (+7 %) | 424 |
Memory: the plan's layout node is 384.33 GB at 576 and 396.68 GB at 572-575 (default schedules; the cache slots stay 0); the model's node figure 399 -> 424-432 GB, all under 480 GB (modelled). SHMEM pool 17.2 GB per process vs 17.7 GB at 576 (plan pool). Other counts, default schedule (modelled): 571 (prime) 2562 s, 570 239 s, 568 417 s, 567 215 s, 564 334 s, 560 201 s, 552 270 s, 540 233 s, 528 242 s, 512 223 s.
Caveats: the uneven schedules are modelled and plan-checked only; no run with an uneven last group at a size like this exists in the records (e16 used g = 12, the 3-way top). Open decision: a small test of an uneven `MN_GROUPS` (e.g. 3-7 nodes with a non-dividing last group) before relying on it.

#### 5. Wiring for target_kit.sh (b01 owns it; not edited)
Manual line: `tools/nodescreen.sh --hold $JOBID --out $OUT/nodescreen --ecalc-dir ecalc --flagged-file $OUT/flagged_hosts.txt` (needs `make` and `make tests/t_nodechk` first; `--flagged-file` appends EXCLUDE to the file later kit stages already drop; `rc 3` = excluded hosts exist, `rc 2` = the screen itself failed). On the target set `NS_MODULES="<dsmml> <sma> <rocm>" NS_UNLOAD=<unload>` from kit_modules.env (the script defaults to mnrun.sh's module convention). Kit stage sketch: `stage_optin nodescreen && exec_cmd "$LOGDIR/07_nodescreen.log" 300 env NS_MODULES=... "$SELF_DIR/../tools/nodescreen.sh" --hold "$JOBID" --out "$OUT/nodescreen" --ecalc-dir "$SELF_DIR" --flagged-file "$FLAGGED"`; then re-run the existing `--spares` check on the flagged list. Run it alone on the allocation, before the plan check of the real run, and again if the allocation is older than ~1 h.

#### RESUME
Branch b04 (tools/nodescreen.sh, tools/nodescreen_tab.py, this file). aac7 `~/ntt-wt/b04` (worktree) and `~/b04/` (raw runs, plan files); aac6 `~/ntt-b04`, `~/b04.bundle`, `~/a6run.sh`, `~/b04_run`. Next: positive control when a stalling host appears (inject: `--cold-gb 32`), a t_nicbw third stage, test of an uneven MN_GROUPS on 3-7 nodes.

## T10 - cold-path node-check probe vs the known slow host aac7-n01 (2026-10-11)

(former file: T10.md)

Status: probe implemented (off by default); positive control NOT obtained: the slow host did not stall in this test, so the probe could not be shown to catch it, and no threshold is justified yet. All numbers measured.

#### What the stall is (read from S34 / RESULTS.md, not re-measured)
db_from_bi upload 20.7-25.2 s against ~1 s on the other hosts, follows the host (2 of 2 permuted runs), also seen as 19-25 s in the reciprocal "chain" stage; with the chain broadcast on it remained in db_init / db_from_bi. Probably the first device allocation or a host stall (not proven). Cached was 113.7 GB then (S34) and 160.9 GB at S49.

#### Probe (ecalc/tests/t_nodechk.c, kit option `--nodechk-cold-gb GB`, default 0 = off, output unchanged)
`t_nodechk <gb> <reps> <cold_gb>`. With cold_gb > 0 it prints one extra `NODECHK_COLD` line per node. It mirrors ecalc's init (dbig.c db_vmm_arena_alloc / vmm_map_run): per APU hipMemAddressReserve (3x), 2 GiB chunks hipMemCreate + hipMemMap, one hipMemSetAccess for all APUs, hipMemset, then first-touch of a fresh pageable host buffer (touch_s) and one cold hipMemcpy of it into the VMM range (up_s). Fields: touch_s, apu<d>_total_s / _create_s / _map_s / _access_s / _zero_s / _up_s. Not a tiny ecalc run: no init-only mode exists; the probe takes 12 s (17 GB/APU) or 21 s (32 GB/APU).

#### Test (aac7 job 12511, 2 nodes aac7-n01 + aac7-n00, one process per node, both nodes at once, 3 repeats each of: old probe, cold 17.2 GB, cold 32.2 GB; page cache not droppable, no root)
| quantity | slow host aac7-n01 | normal aac7-n00 |
|---|---|---|
| old probe, 1 GB upload per APU | 0.017 s all APUs, 3/3 | 0.017 s all APUs, 3/3 |
| cold 17.2 GB: total per APU (apu0..3) | 1.17-1.19, 1.52-1.54, 1.93-1.95, 2.06-2.07 s | 1.18-1.25, 1.53-1.56, 1.93-1.96, 2.06-2.09 s |
| cold 32.2 GB: total per APU | 2.18-2.22, 2.83-2.87, 3.59-3.63, 3.84-3.89 s | 2.69-3.89 (apu0 noisy), 2.84-2.87, 3.57-3.61, 3.84-3.88 s |
| touch_s of the host buffer (17 / 32 GB) | 2.9-3.0 / 5.4-5.5 s | 2.9-3.6 / 5.4 s |
| MemAvailable / Cached | 506-509 GB / 14-21 GB | 440-441 GB / 124-130 GB |
Total per APU grows with APU index (hipMemCreate serialises across the 4 APUs inside one process) and is the same on both hosts to within 1-2 %. The slow host is not distinguishable on any field. The only difference seen is the page cache: the slow host now has only 14-21 GB Cached (was 113.7 GB in S34, 160.9 GB in S49) and 507 GB MemAvailable; the stall was therefore not reproduced, and the node state changed since S49 (someone or something dropped its cache, or the cause is transient).

#### Conclusion and what is open
- Not shown: the new probe cannot be said to catch the stall (negative control only: 0 false alarms, spread between hosts <= 2 % except the normal host's apu0 access step 1.0-1.6 s of 3 runs, which is a one-off 2x on a healthy host).
- Two candidates for the stall remain untested: (1) it needs a large page cache (the host had 114-161 GB cached when it stalled; the normal host with 125-130 GB cached did not stall, so this alone is not it); (2) it is transient. A positive control needs a moment when ecalc itself stalls on that host: run a real 1-node ecalc (1e9 digits) with ECALC_INIT_TL=1 on it and compare the probe in the same job.
#### Proposed policy line (provisional, thresholds NOT justified by data)
> Optional: `target_kit.sh --only build,nodechk --nodechk-cold-gb 16` (12 s/node). Flag a node whose slowest-APU `apu<d>_total_s` or `touch_s` is > 2x the allocation median (healthy hosts: spread <= 2 %, one outlier 2x). Treat it as an extra check only; do not rely on it for the S32/S34 stall until a positive control exists. Keep the per-node rns_init time check of S49 (exclude a node whose rns_init > 3x the median in the first run).
Healthy reference values (measured): 17.2 GB: total 1.2/1.5/1.9/2.1 s for APU 0..3, touch 2.9 s; 32.2 GB: 2.2/2.8/3.6/3.9 s, touch 5.4 s.

#### Operational notes
- `srun --overlap -c 192 --gpus-per-node=4` in a 2-node hold hung with "step creation temporarily disabled (Requested nodes are busy)" (first attempt, job 12508, ~13 min lost); `--overlap --mem=0 -c1 --gpus-per-node=4` works. The following 20-min hold expired while another task held the B2 lock; the final run used a hold of 15 min taken after the lock.

#### RESUME
Raw: aac7 `~/sess1011b/t10/out/`. Branch t10 (commit with probe + this file). Next: positive control with a real 1-node ecalc on aac7-n01, or a retry when the host's cache is large again.

## S49 - slow-host policy: per-host `nodechk` rehearsal (aac7 all hosts, aac6 PPAC)

(former file: S49.md)

Status 2026-10-11 09:10 EDT: finished. 15 hosts measured (13 aac7 + 2 aac6); no host flagged, including aac7-n01. Verdict: the nodechk 1 GB upload probe does NOT catch the S32/S34 stall.

#### Design
- Kit stage `nodechk` (ecalc/target_kit.sh, program tests/t_nodechk): per node MemAvailable/Cached/Huge pages and a 1 GB
  host->device upload per APU (median of 3). Flags (kit rules): slowest-APU upload > 3x median, MemAvailable < 90 % of median.
- Kit code needed NO change: `--only build` works on aac7 (login node, ROCm 7.2.4 chosen, writes kit_modules.env) and
  `--only nodechk --jobid <job>` takes the allocation size from `squeue -j`. Not yet executed on a compute node (see risk).
  On aac6 the build stage cannot run (no Cray SHMEM), so `kit_modules.env` is hand-written (`TARGET_ROCM=rocm`, others empty)
  and `tests/t_nodechk` is built with plain `make tests/t_nodechk`; the job body falls back to a raw `srun t_nodechk 1 3`
  if the kit yields no NODECHK line.
- aac7 (13 hosts listed by sinfo; aac7-n61 is no longer listed at all, so it cannot be tested):
  (a) queue item `~/batch1011/jobB/queue/30_nodechk.sh` (OWNER=T4, EST_MIN=6): kit nodechk on all 4 nodes of the Job B hold;
  (b) login-node driver `~/batch1011/T4/adhoc_driver.sh`: per host an `sbatch -N1 -w <host> -t 10` job running
  `nc_job.sh` (kit nodechk, out dir `~/batch1011/T4/out/adhoc_<host>_<epoch>/`). Rules enforced: our running nodes <= 5 before
  submitting, never submit in [W-60, W+75] min nor if the job (<= 20 min incl. pending) would touch it or pass D, only
  when Slurm shows the host `idle`, scancel if pending > 10 min, up to 3 tries per host, hosts already covered (any NODECHK
  line, e.g. from the Job B item) skipped, aac7-n01 first. Stops at D-20 min or on STOP file.
- aac6: timer `~/batch1011/T4/aac6_timer.sh` on the aac6 login node sleeps to 03:30 EDT (epoch 1791703826), then submits one
  `-t 12` job each on aac6-spx-a and -26 (`--deadline` 03:50), scancels leftovers at 03:46. Nothing pending before 03:30.
- Collection: `archive:drivers/s49/s49_collect.sh` (+ `archive:drivers/s49/s49_table.py`) fetches all NODECHK lines from both clusters and prints the table.

#### Known coverage limit (important)
Job A (2 nodes) + Job B (4 nodes) hold 6 nodes, so the ad-hoc rule (our running total <= 5) blocks every ad-hoc job while
both run. Ad-hoc jobs can run only in gaps (e.g. between hold hand-offs). Hosts of Job B are covered by the queue item when the
driver reaches it; Job A's hosts and any host that never idles are reported MISSING in `DONE_adhoc`. A second chance: a
nodechk step inside the A hold by T1 (needs the coordinator's OK), or after D.

#### Commands (aac7; markers under ~/batch1011/T4/)
- Driver: `setsid nohup bash adhoc_driver.sh > driver.log 2>&1 < /dev/null &`; dry run `DRY=1 RUNLIM=9 bash adhoc_driver.sh`
  (verified 2026-10-10 20:35 PDT: lists all 7 free hosts, slow host first).
- Stop: `touch ~/batch1011/T4/STOP` (aac7) / `STOP6` (aac6, before 03:30 only).

#### Proposed slow-host policy text for the kit (thresholds checked against the data below)
> Before a timed multi-node run: (1) request N+S nodes, S = 2 spare nodes (about 0.3 % at 576 nodes; the measured flag rate was 0 of 13 hosts, so S is set by the stall rate seen in runs, not by this probe). (2) Run `target_kit.sh --only build,nodechk` on the whole allocation (1-3 min). (3) Exclude every FLAGGED node (`MNRUN_EXCLUDE_HOSTS` or Slurm `-x`): flag = slowest-APU 1 GB upload > 3x the allocation median (healthy hosts measured 0.017 s on all 13 hosts, so the flag trips above 0.051 s; the threshold has margin and no healthy host is near it) or MemAvailable < 90 % of the median AND short of the run's per-node need (peak 412-415 GB/node at 6.441e10 digits/node, measured; healthy aac7 hosts measured 435-440 GB, aac6 515-517 GB). (4) If more than S nodes are flagged, stop and ask for replacements. (5) Re-run nodechk if the allocation is more than ~1 h old at start time. (6) NOT sufficient alone: nodechk does not detect the known aac7 slow host (see Results); add a cold-path probe (below) or watch the per-node init time of the first run and exclude any node whose `rns_init` is more than 3x the median.
> Thresholds [3x, 90 %, S = 2] are kept; no data argued for a change.

#### Results (measured, 2026-10-11 ~09:10 EDT)
Sources: aac7 Job B item (4 nodes, 05:29 EDT), aac7 ad-hoc 1-node jobs after D (05:58 EDT, hosts of Job A/B skipped), aac6 two PPAC nodes (02:31 EDT). Upload = 1 GB host->device per APU, median of 3, seconds. Collected with `archive:drivers/s49/s49_collect.sh`.

```
== aac7: 13 hosts (11 listed here + c1s3b0n0, c1s3b1n0 added at merge, see Coverage); median slowest-APU upload 0.017 s, median MemAvailable 439.8 GB
host               MemAvail GB Cached GB upload s per APU (apu0..3)         flags
aac7-n00            440.1     130.8 0.017 0.017 0.017 0.017            
aac7-n01            439.8     160.9 0.017 0.017 0.017 0.017            
aac7-n10            440.2      81.3 0.017 0.017 0.017 0.017            
aac7-n11            439.9      97.6 0.017 0.017 0.017 0.017            
aac7-n20            440.3      81.4 0.017 0.017 0.017 0.017            
aac7-n21            440.2      81.6 0.017 0.017 0.017 0.017            
aac7-n40            435.3     161.0 0.017 0.017 0.017 0.017            
aac7-n41            438.0     160.9 0.017 0.017 0.017 0.017            
aac7-n50            439.0     160.8 0.017 0.017 0.017 0.017            
aac7-n51            438.9     160.8 0.017 0.017 0.017 0.017            
aac7-n60            439.6     166.1 0.017 0.017 0.017 0.017            
== aac6: 2 hosts; median slowest-APU upload 0.017 s, median MemAvailable 516.2 GB
host               MemAvail GB Cached GB upload s per APU (apu0..3)         flags
aac6-spx-a          516.9       0.6 0.017 0.017 0.017 0.017            
aac6-spx-b          515.5       0.6 0.017 0.017 0.017 0.017
```
Coverage: 15 hosts. Correction at merge (T7, 2026-10-11): aac7-n30 and aac7-n31 WERE measured (PASS, upload 0.017 s, MemAvailable 439.7 / 439.9 GB; aac7 `~/batch1011/T4/jobA/KIT_SUMMARY.txt`); they are not in the table above, which was collected before that file was read. Only aac7-n61 is absent (no longer listed by Slurm). Earlier ad-hoc attempts (adhoc_*) produced no data because the job had no GPUs requested (fixed 05:35, see Update below).

Findings:
- **aac7-n01 (the known slow host) is NOT flagged**: upload 0.017 s on all four APUs, MemAvailable 439.8 GB (= median), Cached 160.9 GB. No host in the table is flagged (0 SLOW, 0 LOWMEM of 13 in the table; the two c1s3 hosts add 2 more, also unflagged). Healthy spread is zero in the upload (all 0.017 s), 435.3-440.3 GB in MemAvailable on aac7 (1.1 %).
- Therefore the 1 GB upload probe **does not catch the 20-25 s stall** seen in S32/S34. Either the stall is transient (the host was fine at 05:58 EDT), or it sits on a path the probe does not exercise: the probe is a warm 1 GB copy at 60 GB/s, a few ms of work, while the stall was seen in the first-touch/registration of the large pools at init and in the large uploads of a real run (hypothesis, not tested here).
- What would catch it (proposals, untested): (a) a cold-path probe: allocate and first-touch/register the real pool size (e.g. 8-16 GB per APU, fresh process) and time it, flag at > 3x median; (b) a 1-node short real run (1e9 digits gate, about 10 s) on each node and compare `rns_init` times; (c) in a timed run, per-node init time from the first run, exclude outliers. Because the host was healthy in this test, a positive control (a time when the stall is present) is needed before any of these thresholds can be set.
- Thresholds: 3x upload and 90 % MemAvailable stay; with a zero spread of healthy hosts, 3x gives 0.051 s, far from any value seen.

#### RESUME
- Markers: aac7 `~/batch1011/T4/DONE_adhoc` (driver end; lists covered/MISSING hosts), `~/batch1011/jobB/queue/30_nodechk.sh.rc`
  (item result; output in `~/batch1011/T4/out/jobB_*/`), aac6 `~/batch1011/T4/DONE_aac6` (about 03:50 EDT).
- Collect: `bash ~/apucode/ntt-wt-s49/archive:drivers/s49/s49_collect.sh` (prints the table; paste into Results, edit the policy
  thresholds, commit, push `s49`, remove worktree after merge; also remove aac7 `~/ntt-wt/s49` and aac6 `~/ntt-s49`).
- Cleanup after D: `git -C ~/ntt-acc worktree remove ~/ntt-wt/s49` on aac7 after the item/driver are done (the item and
  jobs use its binaries until then).

#### Update 2026-10-11 05:35 EDT (T7): ad-hoc fix and post-deadline driver
- Cause of "1 s, no data": the ad-hoc sbatch asked for no GPUs (job got cpu=1, no gres), so the kit's
  `srun --gpus-per-node=4` failed ("Invalid generic resource (gres) specification"); output went to `kit.stdout`, not `job.<id>.out`.
  Fix: `sbatch --exclusive --gpus-per-node=4` (archive:drivers/s49/s49_adhoc_driver.sh; `sbatch --test-only` accepts it; not run live because the 6-node QOS is full).
- Old driver (PID 3397105) killed. New driver `archive:drivers/s49/s49_adhoc2_driver.sh` (aac7 `~/batch1011/T4/adhoc2_driver.sh`, log `driver2.log`):
  sleeps to D+60 s, waits for <=1 running node of ours (max to D+720 s), 1-node 10-min jobs `-w <host>` on hosts other than the 6 of Job A/B
  (aac7-n01 first), <=5 at once, `--deadline` S, scancels only its own ids. STOP file: `~/batch1011/T4/STOP2`.
- RESUME marker: aac7 `~/batch1011/T4/DONE_adhoc2` (covered/MISSING per host); per-host output `~/batch1011/T4/out/adhoc2_<host>_<epoch>/KIT_SUMMARY.txt`.
  Collect: `bash ~/apucode/ntt-wt-s49/archive:drivers/s49/s49_collect.sh`.

#### Slow-host policy (adopted by the user 2026-10-11; check by A03)
A node whose first-run init time (`rns_init`, from the run's own `ECALC_INIT_TL=1` output) or first device upload (`db_from_bi`, `NEWTON_MN_RECIP_TS=2`) is more than 3x the allocation median is flagged by `tools/initchk.py` (exit 3; `--threshold`, `--min-excess 2`). The cold probe (`t_nodechk` cold, results/NODE_SLOWMODE.md §T10) stays off by default until a positive control exists. Measured (archive:results/A03.md): on the two real 10-node logs where aac7-n01 stalled (s32), the upload stall is 20.8x and 25.3x the median and is flagged, but its init is only 1.11x and 1.15x, so an init-only rule would not catch it; healthy runs stay at 0.8-1.15x (rns_init) over 8 real runs. Use `MNRUN_LABEL=1` so every metric is attributed to a host.

## NIC — node screen catches nodes with fewer NICs than the launch asks for

(former file: NIC.md)

Status: done 2026-10-13 (Eastern). Branch nicchk, merged to main.

#### Problem (measured)
2026-10-13 12:55: three SHMEM-path runs (`COMM_OFI=0`, `SHMEM_OFI_NUM_NICS=4`) aborted within seconds (rc 141) on one node:
`LIBSMA ERROR: error in SHMEM_OFI_NUM_NICS parsing: An invalid number of nics 4 was found in "4". There is only 1 NIC available.`
Later runs on the same allocation passed (21 of 24). The node screen checked only times, not the NIC count.

#### Change
`tools/nodescreen.sh` local stage prints `NS nics have=<n> want=<w>` per node: `have` = distinct `fi_info -p cxi` domains seen
by the runtime (after the same module load as the SHMEM launch); `want` = `--want-nics N`, default `$SHMEM_OFI_NUM_NICS`, 0 = report only.
`tools/nodescreen_tab.py` flags a node with `have < want` as a failed step, so it lands in EXCLUDE like any other bad node.
No new setting (an option of the tool). `ecalc/README.md` tool row updated.

#### Verification (measured, 4 nodes, one queue item, local stage only)
| want | per-node `have` | verdict | rc |
|---|---|---|---|
| 4 | 4, 4, 4, 4 (`/sys/class/cxi` also 4 on each) | all ok, EXCLUDE empty | 0 |
| 8 (positive control) | 4, 4, 4, 4 | all four flagged `NICs 4 < 8 wanted` | 3 |

Not reproduced on a real 1-NIC node (none in the allocation); the flag path is exercised by the want=8 control.
The login node has no cxi device (count 0), so the check is only meaningful on compute nodes.

#### RESUME
Use `tools/nodescreen.sh --hold JOB --want-nics 4` (or export `SHMEM_OFI_NUM_NICS=4`) before SHMEM runs that set 4 NICs.
