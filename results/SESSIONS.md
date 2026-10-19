# SESSIONS — aac7 measurement sessions of September/October

Scope: aac7 measurement sessions of September/October: profile analysis 2 to 10 nodes, T2048 A/B, XSTATS decomposition, X2 and VMM_SAFE soak. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| S22 | / S23 / S24: T2048 sequential A/B (D2), 10-node scaling sweep with wait + ready stats (E1), Q1 seed sensitivity, NTT swe |
| S27 | / S25 / S26: COMM_XSTATS decomposition, X1 rot ABBA, share ladder, A37-Q9, soak counts |
| S31 | / S32 / S33 -- X2 second try, host-vs-rank, VMM_SAFE soak: the comm pool failure of S28 and its fix (2026-10-08) |

## S22 / S23 / S24: T2048 sequential A/B (D2), 10-node scaling sweep with wait + ready stats (E1), Q1 seed sensitivity, NTT sweep and NTT_SIZE_STATS, 13/9/9 prototype (E0)

(former file: S22.md)

Source: aac7 `~/s22`, `~/s23`, `~/s24` (each `summary.txt`, `results.txt`, `res/`, `log/`; S22 also `d2.tsv`, `d2_stats.txt`). Drivers: branch s22 commit 13ca2c4 (`archive:drivers/ecalc/s22_batch.sh`, `s23_batch.sh`), branch s24
commit c0f3710 (`ecalc/s24_batch.sh`; prototype kernels in `ecalc/ntt.c` / `tests/t_ntt.c` e0 at efed002). Builds: S22/S23 worktree 83a124bb, S24 efed0026c8 (t_ntt built 2026-10-07 12:25 EDT).
Read-only analysis; nothing was launched on aac7. Times Eastern. Labels: (m) measured, (mod) modelled, (a) assumed.

| batch | when (EDT) | where | runs | outcome |
|---|---|---|---|---|
| S22 | 2026-10-07 22:09 - 2026-10-08 03:03 | hold 12331, 10 nodes aac7-s0..s4 (b0/b1), 6.441e10 digits/node, no write | gate + 10 scaling + 32 D2 = 42 runs | SUCCESS, 0 bad (all VERIFY OK, digits identical); D2 stop rule met after 16 new rounds |
| S23 | 2026-10-07 22:09 - 22:32 | hold 12377 nodes s5b0 (chain A), s5b1 (B), s6b0 (C) one chain each, plus a CPX node of 12294 | 4 chains | SUCCESS; gates passed (1e9 digits identical with every new switch off and on; `t_ntt 24` OK) |
| S24 | 2026-10-07 22:25 - 22:31 | hold 12377 node aac7-n51, one APU | 12 runs (probe, b13, plan real, plan NOP x 3) | SUCCESS (all VERIFY OK) |

#### 0. Verdicts in one table

| # | decision | verdict | confidence |
|---|---|---|---|
| 1 | `MN_T_CHUNK_MB=2048` (D2) | **Faster on aac7: B - A = -42.3 s mean / -35.1 s median over 24 pooled rounds (22 of 24 negative); S22 alone 16 of 16 negative, -40.6 s +- 11.1 s (95 % CI).** Adopt on aac7 profile lines; keep the target line at the pool law until a target A/B | high for aac7, low for the target |
| 2 | S-2 division overlap | **Revised from GO to NO-GO for the target**: ceiling at 576 nodes about 25-34 s (mod) and a realistic hidden share of 6-17 s, below the 30 s threshold; at 10 nodes on aac7 the ceiling is 43 s (mod), realistic 11-22 s. The ready counter does not separate skew from transfer | medium (model-based at 576) |
| 3 | A37-R1 GPU seed | **NO-GO** (n = 6 timelines + sensitivity): seed and mapping co-bind on aac7 (seed ends 0.55 s after the last mapping, mean); on the target the seed would end at about 16 s against a 16.5 s init floor, so a faster seed gains about 0-1 s (mod) | medium-high |
| 4 | 3-pass 2^31 NTT | **NO-GO: the E0 gate failed.** 13/9/9 forward = 90.6 ms (m, mean of 3) vs today 89.9 ms, i.e. no gain; the gate was <= 80 ms and the model's central value 75 ms. The new b1r<13> swizzle is correct (468 checks, 0 mismatches) | high |

#### 1. Decision 1: D2, MN_T_CHUNK_MB=2048 (paired ABBA, 10 nodes)

**Config check.** S22 uses the same A and B as S20: A = the S20 LINE + `DM_MN_LEAN=1` (`DIST_CHUNKS` default 8), B = A + `MN_T_CHUNK_MB=2048`; the only change is that **both arms also set `ECALC_INIT_TL=1`** (S22 driver line 54 adds it to
the stats string for the D2 runs; no effect on the engine's work). The binary is newer (S20: b67c5c28; S22: 83a124bb, instrumentation off by default, digits identical). S22 rounds 9-24 continue S20's rounds 1-8 in strict ABBA
(odd rounds A first, even rounds B first). Hold: S20 rounds 2-8 and all of S22 ran on hold 12331 (same node set); S20 round 1 ran on 12287. So the 24 rounds are poolable with the caveats "newer build" and "INIT_TL on" (assumed neutral).
No run spanned a hold switch. All 32 S22 runs VERIFY OK, rc 0, peak 355.9-357.0 GB (m, identical to S20: memory unchanged).

##### 1.1 S22's 16 new rounds (m; B - A total s, rounds 9..24)

-27.6, -36.5, -30.2, -32.4, -108.0, -54.8, -35.5, -32.8, -25.5, -26.0, -63.7, -34.5, -32.7, -45.4, -26.2, -38.1

##### 1.2 Statistics (m; computed from `d2.tsv` and the S20 totals in archive:results/S20.md)

| set | n rounds | mean B - A | median | sd | paired t (df) | p (two-sided) | negative | sign test p | Wilcoxon p (normal approx.) |
|---|---|---|---|---|---|---|---|---|---|
| S20 (rounds 1-8) | 8 | -45.7 s | -67.4 s | 170.2 | -0.76 (7) | about 0.47 | 6 of 8 | 0.29 | 0.23 |
| **S22 alone (rounds 9-24)** | 16 | **-40.6 s** (95 % CI -51.7 .. -29.5, half-width 11.1) | **-33.7 s** | 20.8 | -7.80 (15) | about 2e-6 | **16 of 16** | 3e-5 | 0.0005 |
| **pooled (24 rounds, the driver's "ALL")** | 24 | **-42.3 s** (CI -82.6 .. -2.0) | **-35.1 s** | 95.4 | -2.17 (23) | about 0.04 | 22 of 24 | 4e-5 | 0.0013 |
| pooled without the 2 S20 rounds that had a dm blow-up (r3, r6) | 22 | -43.8 s (CI -57.7 .. -29.9) | -35.1 s | 31.4 | -6.56 (21) | < 0.001 | 21 of 22 | | 0.0003 |
| pooled, 3 largest at each end trimmed | 18 | -43.2 s | | | | | | | |
| pooled, sign-flip permutation test on the mean | 24 | | | | | 0.027 | | | |

The driver's own rule fired "STOP significant" at block 4 (24 rounds, t = -2.17 against the critical 2.069): that is the marginal pooled test, dragged by S20's two blow-up rounds. The S22-only and the blow-up-free sets are far
beyond any threshold. The conclusion does not depend on the pooling.

##### 1.3 Arms, phases, variance (m)

| | A (LEAN + DC8) mean / median (min-max), S22 16 runs | B (A + T2048) mean / median (min-max) | paired B - A mean / median |
|---|---|---|---|
| total s | 436.3 / 436.8 (411.0 - 515.5), sd 26.7 | 395.6 / 396.2 (377.2 - 419.7), sd 14.6 | -40.6 / -33.7 |
| bs s | 173.6 / 176.7 | 158.2 / 157.9 | -15.5 / -14.2 |
| dm s (recip + division) | 232.6 / 230.1 (214.5 - 309.1), sd 23.2 | 207.0 / 206.7 (193.3 - 225.4), sd 10.3 | -25.6 / -20.2 |
| init s | 25.3 | 25.9 | +0.6 |

- B is faster in **both** bs (about -15 s) and dm (about -20...-26 s), as in S20 (-23 / -41 medians). Init is unchanged, so it is the compute/pool-chunk work of the tree levels and the division, not start-up.
- **B is also tighter**: dm sd 10.3 vs 23.2 s; A's dm is clustered (215 s x5, 230-246 s x8, plus one 309 s), B's is 193-225 s. Position in the pair does not matter (slot 1 mean 417.5 s, slot 2 414.4 s).
- **Fabric blow-ups**: S20 had 2 of 16 runs with dm 500-536 s (one per arm). S22 had **0 of 32** above the 325 s threshold, one mid event (r13 A, dm 309 s, total 515.5 s, +80 s above A's typical). Over the 48 runs so far: 2 blow-ups (4 %), 1 mid event, no arm
  asymmetry. The blow-ups are fabric events, not a switch effect.
- Scale: S22's A mean (436 s) is 21 s below S20's seven normal A runs (457.6 s); S22's B (395.6) is 8 s above S20's six normal B runs (387.4 s). Different days and a node set shared only partly; the paired deltas are the evidence, not the levels.
- Mechanism: still none (the model does not explain it). B at 2048 MB halves the number of pool chunks in the T pipeline; the saving is local, not in the exchange wait (S20 D3: B's wait was not lower). (a)

##### 1.4 Recommendation

**Adopt `MN_T_CHUNK_MB=2048` in the aac7 profile lines (arm B becomes the aac7 default line).** Memory is unchanged, the effect is 22 of 24 rounds in the same direction, the tail is not worse (A owns the 309 s event), and the variance is lower.
**Do not touch the target line**: the target needs `MN_T_CHUNK_MB=1024` by the pool law (RESULTS §84) and runs `comm_ofi`, where nothing here applies directly. The honest statement for the target is "the effect is plausibly
local (compute/pool), so it may carry over, but only a target A/B shows it" (a). Cost of that A/B: 2 runs on the target's share, about 15 min of node time.

| option | cost | benefit | risk |
|---|---|---|---|
| (a) adopt on aac7 profile lines, target unchanged **(recommended)** | none | about -35...-40 s (8-9 %) per aac7 10-node run (m); cleaner timing for every later aac7 A/B (lower variance) | none to the target |
| (b) (a) plus a 2-run A/B on the target share | about 15 min of target node time, one launch line | the only evidence that informs the target line; could save up to the same 8-9 % there (a) | needs target access |
| (c) keep off | none | none | forgoes a large measured gain on aac7 |
| (d) more rounds on aac7 | 2.5 h per 8 rounds | adds nothing: the question is answered for aac7 | wasted hold |

#### 2. Decision 2: S-2 division overlap / peer wait (E1): scaling sweep n = 2..10

Runs: `scale_n{2,4,6,8,10}_r{1,2}` (config A + `MN_WAIT_STATS=1 ECALC_INIT_TL=1 NTT_SIZE_STATS=1 MN_COMM_MARK=1`), 6.441e10 digits per node, no write, hold 12331 (the first n nodes), all VERIFY OK. The stats cost something: the n = 10 runs took
456.3 / 450.2 s against the D2 A runs' 436.3 s mean (m, n = 2 vs 16, sd 27): about +3-4 % (a: from the extra instruments); **compare shapes across n, do not read the absolute wait levels as the stat-free values.**

##### 2.1 What the counters mean (from the S22 source)

- `wait` = time blocked in `comm_wait` (exchange completion = transfer + peer skew), summed over the APU threads of the node; `barrier` is 0 everywhere. "Per thread" below divides by 4 (a).
- `ready` (new) = every `wait_ge` / `wait_ne` in `comm_shmem.c`: the receiver-offset flag the sender waits for (peer not there yet = skew) **and the data-arrival signal the receiver waits for (`arrive`, round signals = transfer)**.
  So `ready` is "skew + most of the transfer", **not** skew. It also counts the post-side waits (outside `comm_wait`), so it is not strictly a subset of `wait`. Result: the ready counter by itself cannot split skew from transfer;
  I use the node spread (max - min over nodes of `wait` and of `ready`) as the skew bound, as in S20 (the least-waiting node is the last to arrive).
- Phases: bs = tree levels (comm-mark "tree level 1..2"); dm = reciprocal + division (n = 10 r1: recip wall 78.7 + division wall 170.8 = 249.5 = the `dm` line, m).

##### 2.2 Data (m, mean of 2 reps; seconds per APU thread (a: /4); "wait" = the middle of max and min over the nodes, node 0 is the max)

| n | total s | bs wall | recip wall | division wall | wait bs | wait recip | wait division | wait total / wall | in-flight tree / recip / division (post to completion per thread) | spread (max-min) wait: bs / recip / division | spread of ready: bs / recip / division |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | 200 | 66 | 41 | 63 | 17 | 10 | 32 | 59 / 200 = 30 % | 17 / 11 / 37 | 2 / 1 / 8 | 0 / 8 / 9 |
| 4 | 285 | 106 | 49 | 93 | 42 | 14 | 55 | 111 / 285 = 39 % | 48 / 17 / 72 | 10 / 1 / 20 | 10 / 5 / 22 |
| 6 | 385 | 146 | 56 | 152 | 66 | 17 | 101 | 184 / 385 = 48 % | 73 / 20 / 121 | 19 / 3 / 32 | 17 / 7 / 32 |
| 8 | 370 | 157 | 59 | 114 | 72 | 22 | 73 | 167 / 370 = 45 % | 90 / 27 / 92 | 20 / 3 / 21 | 23 / 12 / 28 |
| 10 | 453 | 179 | 76 | 169 | 84 | 42 | 113 | 239 / 453 = 53 % | 93 / 61 / 141 | 14 / 26 / 28 | 16 / 28 / 32 |

Notes: n = 6 and n = 10 are non-power-of-two groups (division 1.4-1.5x the n = 8 one, as in S19B). Node 0 (the maximum) at n = 10: wait bs 84 / division 127 / recip 55 s per thread. Counts are stable across reps (bs 21 072 waits, division 20 252, recip 24 376).
Both reps agree to about 2-8 % on every wait figure except n = 8 (rep 2 had a longer division, 398 s vs 342 s total, as in the earlier S19B fluctuation).

##### 2.3 Sizing the exposed wait against the transfer, per phase (10 nodes, aac7 fabric; mod from the data above)

| phase (n = 10) | wall | wait | local work = wall - wait | in-flight transfer (floor if transfers stay serial) | ceiling A = min(wait, local) | ceiling B = wall - max(local, in-flight) | skew bound (spread) |
|---|---|---|---|---|---|---|---|
| division | 169 | 113 | 56 | 141 | 56 | **28** | 28 |
| reciprocal | 76 | 42 | 34 | 61 | 34 | **15** | 26 |
| bs (tree) | 179 | 84 | 95 | 93 | 84 | 0 (transfer-bound) | 14 |
| division + reciprocal | 245 | 155 | 90 | 202 | 90 | **43** | 54 |

- Ceiling B is the tighter bound: overlap can hide transfers behind local work only until the wall equals the transfer itself, since the fabric is already busy about 84 % of the division (in-flight 141 of 169 s). So the **upper bound for a perfect
  division + reciprocal overlap at 10 nodes is about 43 s of 453 (9.5 %)** (mod); a realistic 25-50 % gives **11-22 s** (a). The earlier "30-60 s" (S20) used wait minus a crude local estimate and ignored the transfer floor; it is revised down.
- Against the threshold ("exposed wait > about 30 s"): the literal exposed wait is 155 s per thread (far above), but most of it is the transfer itself, which no overlap hides. The recoverable part is bounded by the 43 s above, and a 25-50 % realisation lands below 30 s.
- Skew: the spread is 28 s (division) and 26 s (reciprocal) per thread at n = 10, 0-10 s up to n = 4. **The reciprocal's spread jumped from 1-3 s (n <= 8) to 26 s at n = 10**, unlike S20 (spread about 3 s per thread, wait 96 s node sum there vs about 170 s here). The S22 stats
  carried three extra instruments (see above), so the recip wait level may be inflated (a); the n = 10 step may also be the 5-way group. Unresolved; the skew part of the reciprocal is the piece an asymmetric chain wait would give, not something overlap with local work fixes.
- Growth with n: the wait share of the wall rises 30 % -> 53 % (n = 2 -> 10) and the transfer rate per APU thread falls (n = 2: 10.1-11.8 GB/s; n = 10: 4.2 reciprocal, 4.9 division, 6.5 tree level 2, 10.8 tree level 1, comm-mark lines, m).

##### 2.4 Projection to 576 nodes (mod)

`ecalc/estimate.py --fabric target-m --bw 47 --g 576 --D 6.441e10 --verbose` (target-class fabric, the standing estimate: 219.0 s no-write) and `--fabric aac7_s18 --bw 6` (aac7-class: 747.6 s):

| 576 nodes (mod) | target-m: wall / exposed / local = wall - exposed | aac7-class: wall / exposed / local | ceiling = min(exposed, local) target-m / aac7-class |
|---|---|---|---|
| division (16 pieces) | 41.6 / 23.9 / 17.8 | 184.7 / 164.8 / 19.9 | 17.8 / 19.9 |
| reciprocal (49 pieces) | 17.9 / 10.3 / 7.6 | 75.9 / 67.5 / 8.4 | 7.6 / 8.4 |
| **division + reciprocal** | 59.5 / 34.2 / 25.4 | 260.6 / 232.3 / 28.3 | **25.4 / 28.3** |
| all exposed communication | 82.1 s of 219 s (35 %) | 593.9 of 748 (63 %) | |

- The model's local part is 3x lower than what S22 measures at 10 nodes (model local 7.4 + 22.4 = 30 s vs measured 34 + 56 = 90 s), so the model's ceiling is conservative; with the measured local work (a) the target ceiling becomes the whole modelled exposed 34 s.
  **Range for the target: ceiling 25-34 s (mod), realistic hidden share 25-50 % = 6-17 s; even a 70 % share (a) is 24 s.** All below the 30 s threshold, and the model's exposed figure already assumes the general map's overlap (GEN_HIDE).
- On an aac7-class fabric at 576 the ceiling is also bounded by local work (28-34 s), the large exposed wait there (232 s) being almost all transfer.
- The model reproduces the n = 10 division within 6 % (177.96 s modelled vs 169 s measured) but the reciprocal not (54.2 s vs 76 s measured); the 576 reciprocal term may be low.

##### 2.5 Verdict and options

**Do not build S-2 now.** It flips S20's "GO on a prototype": with the transfer floor the ceiling at 576 on the target fabric is 25-34 s (mod) and the realistic gain 6-17 s (a), below the threshold; the gain on aac7 (11-22 s of 453) does not move the 576 target. The ready counter
neither supports nor refutes this (it contains transfer). The only way the answer could change is a **measured** target-fabric wait (a stats run on the target) showing a large skew-only share.

| option | cost | benefit | risk |
|---|---|---|---|
| (a) no build; keep S-2 on the list gated on target wait stats **(recommended)** | none | avoids an M-effort build for 6-17 s (a) at the target (about 3-8 % of 219 s) | gain forgone if the target's wait is mostly skew |
| (b) one stats run on the target share (`MN_WAIT_STATS=1`, a few minutes) | one short target slot | measures target wait / skew directly; decides S-2 with data | needs target access |
| (c) prototype for the reciprocal only (transfer-bound, 49 pieces) | M-effort | at most 7.6-10 s at 576 (mod, target-m), 15 s at 10 nodes | the reciprocal skew at n = 10 is unexplained (26 s per thread) |
| (d) full division + reciprocal overlap | M-L effort | 6-17 s realistic, 25-34 s ceiling at the target (mod) | the gain may not clear the 30 s bar; digits-identity ordering work |

#### 3. Decision 3: A37-R1 GPU seed (S23 Q1 x 6, seed sensitivity, CPX b_seed64)

##### 3.1 Init timeline, 6 repetitions on one node (m; `ECALC_INIT_TL=1`, 6.441e10, aac7 mapping, node aac7-n50; total 90.7-94.5 s, init 12.6-14.1 s, VmHWM 26.8-26.9 GB)

| event (s since `rns_init`) | mean | sd | min - max |
|---|---|---|---|
| plane pools done | 5.88 | 0.58 | 5.13 - 6.51 |
| seed thread starts | 5.95 | 0.57 | 5.20 - 6.57 |
| bs joins the seed thread | 13.65 | | 12.8 - 14.4 |
| last background mapping done | 23.33 | 0.71 | 22.38 - 24.30 |
| **seed thread ends** | **23.88** | 0.97 | 22.74 - 25.38 |
| bs level 1 starts | 24.38 | 0.92 | 23.25 - 25.79 |
| seed thread wall / minus the region-pool wait (2.7-3.3 s) | 17.93 / **14.77** | 0.53 / 0.56 | 17.4 - 18.8 / 14.2 - 15.8 |
| span arithmetic (seed spans) | 10.6 | 0.1 | 10.53 - 10.77 |
| bs waits at the join (level 1 start - join) | 10.7 | 0.5 | 10.2 - 11.6 |
| seed end - last mapping | **+0.55** | 0.44 | -0.06 ... +1.08 |
| level 1 start - max(seed end, last mapping) | 0.48 | 0.11 | 0.41 - 0.71 |

- The seed ends after the last mapping in 5 of 6 runs, by 0.55 s on average: **they co-bind** (S21's two runs: -2.3 and +0.9 s). Total run time 92.4 s, sd 1.4.
- **Sensitivity (m, n = 2 each):** `BS_SEED_THREADS=96` (half the seed threads): spans 12.4-12.8 s (+2 s), seed ends 25.0-25.2 s (+1.2 s), level 1 starts 25.4-25.6 (+1.1 s), **total 92.97 / 91.88 s = 92.4 (no change within noise sd 1.4)**.
  So a 2 s slower seed delays level 1 by about 1 s and the run by about 0: the coupling between seed time and the run is at most 0.5 s per second even on aac7, where the seed is the later one. A faster seed gains less, not more.
  `BS_SEED_FILL=0`: bs +8.2 s, total 100.4 / 99.6 s (+7.6 s): the fill logic (default 128) is worth about 8 s and is not a seed-thread effect (spans 11.6 s).
- **Target (mod, low confidence as in S21):** mapping 7x faster (MAP_RATE 0.010 s/GB) puts the plane pools at about 1 s and removes the pool wait, so the seed thread ends at about 1 + 14.8 = **15.8 s** against the fixed 16.5 s reference init (A37CMP): the seed does not bind. A GPU seed at 10 s (mod,
  a37v1 N13 scaled) would end at about 11 s, still below 16.5 s: **gain 0-1 s** (mod). A37CMP's original 7-12 s relied on a modelled 22 s seed; measured it is 14.8 s without the wait.

##### 3.2 CPU base-2^64 seed (CPX node, MI300A CPU, 192 threads, 3 reps; digits identical in 18 of 18 comparisons) (m)

| case | dec18 1-thread ns/leaf (3 reps) | b64k32 1-thread ns/leaf | dec18 all-core wall x nspan (s, 3 reps) | b64k32 all-core wall x nspan (s) | ratio all-core |
|---|---|---|---|---|---|
| 10 nodes, rank 0 | 36 292 / 36 298 / 36 309 | 34 034 / 34 019 / 34 036 | 17.6 / 23.3 / 14.9 | 7.1 / 7.5 / 7.3 | 2.0-3.1x |
| 10 nodes, rank 5 | 35 089 / 35 560 / 35 140 | 35 379 / 35 357 / 35 372 | 10.3 / 10.3 / 10.6 | 8.0 / 8.0 / 8.0 | 1.3x |
| 576 nodes, rank 0 | 36 371 / 36 382 / 36 357 | 33 840 / 33 835 / 33 821 | 12.5 / 11.7 / 12.8 | 6.1 / 6.1 / 6.1 | 2.0x |
| 576 nodes, rank 288 | 30 488 / 30 485 / 30 470 | 35 222 / 35 256 / 35 261 | 9.0 / 9.3 / 9.0 | 7.9 / 7.9 / 7.9 | 1.15x |

- One-thread numbers repeat to < 1.5 %; b64k32 ranges from 7 % faster to 16 % slower than dec18 single-thread (no CPU gain, as S21). The all-core b64k32 is steadier (sd < 0.3 s) and 1.15-3x shorter than dec18's noisy all-core wall.
  But production's in-engine seed spans take 10.6 s with all 192 threads (m, section 3.1) and the CPX dec18 projection says 9-12.5 s at the same shares, so the b64k32 / dec18 all-core ratio of 0.6-0.88 applied to 10.6 s gives about 6.4-9.3 s (mod): **about 1-4 s less seed-thread time**, worth 0-2 s on aac7 (seed co-binds) and **0 s at the target** (seed not binding).
- The R1 gate ("a CPU rewrite does not close the seed gap itself") stays open in S21's sense, but the measured bound on what any faster seed can give (section 3.1 sensitivity, target model) is now 0-1 s at the target.

##### 3.3 Verdict and options

**A37-R1: NO-GO** (confidence medium-high; one decisive datum remains on the target).

| option | cost | benefit | risk |
|---|---|---|---|
| (a) no-go; run `ECALC_INIT_TL=1` once on the target share **(recommended)** | a few minutes of target time, no code | confirms "seed thread ends < 16.5 s" (the trigger: build only if it ends later than about 18 s) | none |
| (b) build the GPU seed (2-4 days) | 2-4 days, ordering risk with the region arenas | 0-1 s at the target (mod); 0-2 s on aac7 | digits ordering; effort for ~0.4 % of 219 s |
| (c) CPU b64k32 seed instead | M-effort, in-engine integration | 1-4 s of seed-thread time, 0 s at the target (mod) | wasted if the target mapping is fast as modelled |

#### 4. Decision 4: 3-pass 2^31 NTT (S23 Q4 sweep, NTT_SIZE_STATS; S24 E0)

##### 4.1 S23: forward and inverse NTT at logL = 24..31, real vs NOP-modmul (`bench 31 q4sweep`, one APU, median of 5, 3 reps each; ms) (m)

| logL | passes | real fwd | NOP fwd | modmul (real - NOP) | share | real inv | NOP inv | real - NOP inv | real fwd spread over 3 reps |
|---|---|---|---|---|---|---|---|---|---|
| 24 | 3 | 0.663 | 0.559 | 0.10 | 16 % | 0.655 | 0.554 | 0.10 | 0.631 - 0.702 |
| 25 | 3 | 1.049 | 0.881 | 0.17 | 16 % | 1.077 | 0.876 | 0.20 | 1.035 - 1.062 |
| 26 | 3 | 2.200 | 1.942 | 0.26 | 12 % | 2.209 | 1.919 | 0.29 | 2.193 - 2.212 |
| 27 | 4 | 5.132 | 4.977 | 0.16 | 3 % | 5.295 | 5.015 | 0.28 | 5.120 - 5.158 |
| 28 | 4 | 10.434 | 9.146 | 1.29 | 12 % | 10.795 | 9.276 | 1.52 | 10.365 - 10.491 |
| 29 | 4 | 21.489 | 18.064 | 3.43 | 16 % | 22.149 | 18.449 | 3.70 | 21.354 - 21.601 |
| 30 | 4 | 42.791 | 36.596 | 6.20 | 14 % | 44.350 | 37.512 | 6.84 | 42.610 - 42.831 |
| 31 | 4 | **89.972** | 77.163 | 12.8 | 14 % | 92.678 | 76.487 | 16.2 | 89.958 - 90.052 |

Pass detail at 2^31 (median of 3, ms; real / NOP): pass 0 [26..30] 23.89 / 17.33; pass 1 [19..25] 20.80 / 18.95; pass 2 [12..18] 19.43 / 17.17; pass 3 (b1, [0..11]) 25.75 / 24.16. (S21 had 24.0/20.8/19.4/25.8 real: reproduced to 1 %.)
Time per pass is nearly size-independent in the 4-pass range (L28: 2.6 / 2.5 / 2.6 / 2.65 ms; L30: 10.4 / 10.1 / 11.6 / 10.4 ms), i.e. a memory pass costs about the same whatever its stage count.
Plan changes between L26 and L27 (3 -> 4 passes): the step from 2.20 to 5.13 ms is 2.3x for 2x points, a 17 % pass-count premium (m).

##### 4.2 NTT_SIZE_STATS (m; node 0, 4 APUs add up, passes only)

| run | total NTT GPU-s | share of 4 APU-walls | whole transforms >= 2^27 (fwd + inv, 2^k and 3*2^k) | whole 2^20-22 | largest whole transform | dist rows / cols |
|---|---|---|---|---|---|---|
| 1 node (S23, 2 reps, 6.441e10) | 78.0 / 77.8 | 21.4 % | 15.2 GPU-s (4.2 % of 4 APU-walls) | 5.3 GPU-s | **2^30 and 3*2^29** (no 2^31 whole transform at this size) | rows 2^16 and cols 2^15 of logical 2^31: 32.6 GPU-s (13.5 + 6.3 + 8.0 + 4.8) |
| 2 nodes | 82.7 | 10.2 % | 10.8 GPU-s | 7.4 | 2^30 | rows 2^16 of logical 2^31 / 2^32: 39.8 GPU-s |
| 10 nodes | 87.7 | 4.8 % | 10.1 GPU-s (2.5 s per APU, 0.6 % of the 453 s wall) | 7.0 | 2^30 | rows 2^16 of logical 2^31 / 2^32 and cols 2^15, all 2-pass: 15.7 GPU-s |

- At 10 nodes the whole >= 2^27 transforms are 10.1 GPU-s = **2.5 s per APU** of 453 s (0.6 %). A 17 % saving on them (the model's central value) would be **0.43 s**; NTT3P estimated 1.4 s (6.5 + 2 = 8.5 s of NTT time assumed): the assumed NTT time was 3.4x too high (m vs mod).
- 20-22 whole transforms: 7.0 GPU-s = 1.75 s per APU; 3 -> 2 passes would save at most 0.3-0.4 s (mod).
- Distributed rows reach 2^16-2^17 at n <= 10 (already 2 passes): nothing for a 3-pass plan to gain at 10 nodes. The 576-node term (rows 2^20-2^21, 3 -> 2 passes, NTT3P's 3.8 s) **cannot be measured here**; it rests on E0's result below.
- radix-3 side result: `r3bench` shows `NTT_R3_FUSE=1` is 12-21 % faster per 3*2^k transform (fwd 3*2^22: 31.6 -> 26.0 ms, 3*2^29: 77.5 -> 67.9 ms) and the fused 3*2^k product is 0.65-0.84 of the 4*2^k alternative; the 3*2^k transforms are 5.1 GPU-s per run
  (1.3 s per APU), so the gain is about 0.2 s per run: not worth a decision.

##### 4.3 S24 E0: the 13/9/9 prototype at 2^31, forward (one APU, prime 0, median of 5, three repetitions: r1 / r2 / r3) (m)

Gate from NTT3P.md section 7: build only if the fat pass is within 10 % of the model; section 7 step 2: go if the real 3-pass total <= 80 ms. Model (NTT3P section 4): 9-stage pass 20.0 / 22.5 / 25 ms (optimistic / central / pessimistic), b1-13 26.5 / 30 / 33 ms, total 66.5 / 75 / 83 ms.

| item (ms) | r1 | r2 | r3 | mean | model central | measured vs central |
|---|---|---|---|---|---|---|
| 9-stage pass 0 [22..30] real | 32.34 | 33.23 | 31.61 | **32.4** | 22.5 | **+44 %** |
| 9-stage pass 1 [13..21] real | 28.56 | 28.42 | 28.63 | **28.5** | 22.5 | **+27 %** |
| b1 2^13 pass 2 real (best b1r, `<13,3>`) | 29.47 | 30.01 | 29.64 | **29.7** | 30 | -1 % (as modelled) |
| **3-pass total real** | 90.37 | 91.67 | 89.88 | **90.6** | 75 | **+21 %**; vs pessimistic 83: +9 % |
| today's 4-pass sum / whole fwd | 89.77 / 89.90 | 90.01 / 90.30 | 89.93 / 90.14 | 89.9 / 90.1 | | |
| saving vs today's sum | -0.60 | -1.66 | +0.05 | **-0.7 (none)** | +15 | |
| NOP 9-stage pass 0 / pass 1 / b1-13 | 27.98 / 24.38 / 25.18 | 29.02 / 24.48 / 25.15 | 27.98 / 24.56 / 25.10 | 28.3 / 24.5 / 25.1 | | |
| NOP total vs today's NOP sum | 77.54 vs 77.12 | 78.64 vs 77.57 | 77.64 vs 77.13 | 77.9 vs 77.3 | | |

- Gate results (the driver's own lines): **9-stage 16-column tile at s_lo 13 / 22: 2.8-2.9 / 2.2-2.4 TB/s read+write, threshold 1.6 TB/s: GO** (the memory pattern is fine). **3-pass real total <= 80 ms: 90.4 / 91.7 / 89.9 ms, NO-GO** (all three reps).
- Reading: the tile copy alone costs 11.9 ms (s_lo 13) and 15.5 ms (s_lo 22) per pass (probe), so the 9-stage body costs 17-20 ms beyond memory (real; 12-16 ms in the NOP build): the pass is compute / LDS-exchange / latency-bound at 1 block per CU, not HBM-bound.
  Per stage: 3.6 and 3.2 ms (real) for the 9-stage passes against 2.2-2.3 ms for the b1 passes and 2.8-3.0 for today's 7-stage passes. NTT3P's assumption "about 2.15 ms per stage whatever the split" (31 stages = 67 ms) was wrong for the strided fat pass:
  the 31 stages cost 2.9 ms each here, as today. The saved 17 ms memory pass is eaten by the more expensive stages. The 2^13 b1 itself behaves as modelled (1.10-1.13x of the 2^12 b1 forward; 1.17-1.18x inverse; model 1.16x).
- 10-stage 64 B-segment tiles are poor (probe: 1.3-1.4 TB/s at s_lo 12-13, 1.45-1.5 TB/s at s_lo 17-21), so the 12/10/9 and 10/10/11 alternatives of NTT3P section 3 are also out. s_lo 17 and 24 stay slow (1.7-1.9 and 1.6 TB/s for the 7-stage control), unchanged from K13b.
- Other lengths (mod from the measured pass costs): the 2-pass plans for logn 20-22 (13 + 7..9 stages) would cost about 29.7 + 28.5 = 58 ms per 2^31 points against today's 3-pass 60 ms (24 + 19.4 + 17), i.e. at most 3 %: the 576 piece term of NTT3P (about 2.5 s) disappears too. The E0 prototype was not timed at 2^20-2^22.

##### 4.4 Correctness of the new swizzle and kernels (m)

- `t_ntt e0 b13`: **VERIFY OK (468 checks)** in all 3 reps: the new `k_b1r<13, 3>` / `<13, 4>` (`b1r_swz` extended by one index bit) forward, inverse, fused inverse and the radix-3 pair against the paper's default (random canonical input), 0 mismatches.
- `t_ntt e0 plan` (real modmul): **VERIFY OK (2 checks); the 13/9/9 forward equals the production plan at 2^22 and 2^31 with 0 mismatches** in all 3 reps. The prototype is forward only; the NOP runs skip the check by design (outputs wrong by design).
- Caveat: one APU, one prime, two sizes; the inverse of the 9-stage passes and the `MM`, scale and R3 forms were not built. Not needed given the performance result.

##### 4.5 Verdict and options

**NO-GO on building the 3-pass NTT** (confidence high: three consistent reps; the gate was set before the run). Both of NTT3P's gates fail: the fat pass is +27...+44 % over the model (gate 10 %) and the 3-pass total is 90.6 ms (gate 80 ms, central model 75 ms).

| option | cost | benefit | risk |
|---|---|---|---|
| (a) stop; record E0 as a negative result **(recommended)** | none | frees the M-L item (2-3 weeks); the measured stage cost shows the lever is not the memory pass | none |
| (b) one tuning try on the 9-stage body (2 blocks per CU with a 32 KB tile / 8 stages, fewer LDS exchanges), 2-3 days | 2-3 days on the existing E0 harness | would need the fat pass < 22.5 ms to matter; each 1 ms per pass is 1 ms of 90 | probably < 20 ms saving at best (mod: floor 66 ms is unreachable, 3 x 15.5 ms memory + modmul 14 = 61 ms) |
| (c) build 13/9/9 anyway | 2-3 weeks | 0 ms measured | a regression risk at logn 24-26 where the plan stays at 3 passes |

#### 5. New or anomalous

- **Newton chain start-up (S21: 4.14 s vs 0.43 s): not reproduced.** S23 dm-variance probe, 6 runs on node aac7-n60 (`NEWTON_DOUBLING_TS=1`): chain start 0.58 / 0.44 / 0.59 / 0.61 / 0.44 / 0.61 s (two values near 0.44, four near 0.60), whole chain 6.81-7.02 s, **dm 33.4-34.3 s (median 34.05, spread 0.9 s, no blow-up)**, total 91.3-93.7 s. The 4.14 s start-up and the 12 s run-to-run spread of S21 did not recur in 6 of 6 runs; cause still unknown (a first-touch / cold-path event
  of the first run on a node?). The first doubling j 2 -> 4 costs 2.2-2.8 ms, the last four doublings 0.31-0.35 / 0.61 / 1.26 / 3.8 s (m): the chain is 6.8-7.0 s of which the last doubling is 55 %.
- **dm variance at 10 nodes:** A's dm is bimodal/clustered (215 s x5, 230-246 s x8, 309 s x1; sd 23.2) while B's is 193-225 s (sd 10.3). Blow-ups: 2 of 16 D2 runs (S20) and 0 of 32 (S22). The fabric events look like rare, independent of the arm.
- **Wait-stat levels differ between S20 and S22** (reciprocal wait 96 s node-sum in S20, up to 227 s on node 0 in S22, with a spread 111-227 s). The S22 runs carried `MN_COMM_MARK`, `NTT_SIZE_STATS` and `ECALC_INIT_TL` as well, and cost about 3-4 % in wall (n = 2 runs). Unresolved; affects only the absolute level of section 2.2, not the verdicts.
- The `ready` counter counts data-arrival signals as well as peer-ready flags; anyone using it as "skew" will overestimate skew. A skew-only counter would split `W_ROFF` waits (receiver offset) from `W_SIG` waits (data arrival) in `push_peers` and `arrive`; not built.
- The sweep has no 2^31 whole transform at 6.441e10 per node (largest: 2^30 and 3*2^29). Q4's 2^31 numbers apply to the distributed pieces' logical size and to larger shares only.

#### 6. Decisions for the user (all recommendations; adoption is the user's)

| # | decision | recommended option | why now |
|---|---|---|---|
| 1 | T2048 default | adopt on aac7 profile lines; target line unchanged; optional 2-run target A/B | evidence is complete for aac7 (24 rounds); each later aac7 A/B runs about 8 % faster and steadier |
| 2 | S-2 division overlap | no build; (b) one target wait-stats run decides | the S20 GO is superseded by the transfer floor |
| 3 | A37-R1 GPU seed | no-go; one `ECALC_INIT_TL=1` run on the target | removes a 2-4 day item; gain <= 1 s at the target (mod) |
| 4 | 3-pass 2^31 NTT | stop; E0 negative | removes a 2-3 week item; the prototype's 90.6 ms equals today's 89.9 ms |

Further steps are none required on aac7 for these four. If the user wants only one target action: the single `ECALC_INIT_TL=1` + `MN_WAIT_STATS=1` run on the target share (minutes) answers decisions 2 and 3 at once.

#### RESUME

State: analysis complete and committed (results/SESSIONS.md §S22, RESULTS §122, TASKS status lines). Open: user's four decisions (section 6); nothing running on aac7.
Reproduce: S22 `~/s22/d2.tsv`, `~/s22/res/scale_n*_r*.txt`; S23 `~/s23/results.txt` (q1 / q4sweep / r3bench / sizestats / dmvar / r2 sections); S24 `~/s24/results.txt`. Paired statistics were computed by script from `d2.tsv` plus the S20 totals (archive:results/S20.md).

## S27 / S25 / S26: COMM_XSTATS decomposition, X1 rot ABBA, share ladder, A37-Q9, soak counts

(former file: S27.md)

Analysis 2026-10-08, read-only on aac7 (`~/s27`, `~/s25`, `~/s26`; hold 12331 for S25/S27, hold 12377 for S26). Labels: **(m)** measured, **(d)** derived by
arithmetic from measured numbers, **(mod)** modelled, **(a)** assumed. All runs VERIFY OK, rc 0, digits identical. Base line = `DM_MN_LEAN=1 MN_T_CHUNK_MB=2048`,
10 nodes x 6.441e10 digits. "Per thread" = per APU thread, mean over the 10 nodes; at 10 nodes seconds per thread are about wall seconds (exchanges are exposed).

#### 1. X1 verdict: `COMM_SHMEM_PEER_ORDER=rot` is null on wall (m)

- ABBA, 4 rounds, 8 runs (A = base order, B = rot): A 380 / 378 / 379 / 378 s (mean 378.6), B 384 / 384 / 378 / 380 (mean 381.7).
  Mean B-A **+3.1 s**, sd 2.6, 95 % CI **-1.0 .. +7.2** (t = +2.39, df 3); B slower in 3 of 4. Driver rule: STOP, null (CI half 4.1 s <= 15 s). dm blow-ups: 0 of 8.
- Digits identical and VERIFY OK in all runs (gates `gate_base/rot/xs` 1e9-class also pass).
- **But the transport did get faster (m, one run each, with COMM_XSTATS=1):** layered v-exchange (layv) time per thread 109.7 -> 91.2 s (base `stats_r1`
  -> `xsrot_r1`), rate 11.4 -> 13.7 GB/s (+20 %); exposed wait in layv 93.6 -> 75.8 s (-17.8 s). `xsrot_r1` total 358.4 s versus 375.9 / 399.3 (stats_r1/r2).
  The ABBA arms (no xstats) do not show that: 381.7 vs 378.6.
- Reading: the layered transfer got faster but the wall did not follow in the ABBA, so the time saved is absorbed as waiting for the straggler elsewhere
  (skew, section 2). Open anomaly (unresolved): `xsrot_r1` (358 s) is 8 sd below every ABBA run (377.9 .. 384.1). One run each, and `xsrot` has XSTATS on
  while the ABBA arms have it off. Suggested check: 2 x base+XSTATS and 2 x rot+XSTATS, all without MN_WAIT_STATS (about 30 min). It does not change the verdict.

#### 2. The xstats decomposition (answers XEFF section 3)

Source: `~/s27/xagg/stats_r1.txt` (base order, COMM_XSTATS=1 MN_WAIT_STATS=1, 375.9 s) and `stats_r2.txt` (399.3 s); `xsrot_r1.txt` (rot). Seconds per thread.
`t_wait` = blocked in wait; `roff` = blocked on offset words (a peer has not arrived = **skew**); `sig` = blocked on signal words (data in flight / tail);
`other` = t_wait - roff - sig = staging, posting, flush (delivery-complete), copy-out. `t_call` = post to the caller's wait = caller's overlapped work.

##### 2.1 Direct (non-layered) exchanges, stats_r1 (r2 within 3 %)

| kind | calls/node | GB/node | t_wait s | roff (skew) s | sig s | other s | rate GB/s |
|---|---|---|---|---|---|---|---|
| result (dm 30.9, bs 18.2, recip 2.7) | 926 | 605 | 51.8 | **35.3** | 1.6 | 14.9 | 2.3 / 3.5 / 6.3 |
| addsh (recip 21.3, bs 2.7, dm 2.2) | 604 | 201 | 26.1 | **21.3** | 2.2 | 2.6 | 0.8 / 5.1 / 9.3 |
| redist (dm 11.0, bs 8.8, recip 2.0) | 576 | 498 | 21.7 | 3.2 | **12.2** | 6.3 | 5.3 / 5.7 / 8.0 |
| spill (all phases) | 926 | 0.7 | 1.6 | 1.6 | 0.0 | 0.0 | ~0.1 |
| **all direct** | | 1305 | **101.2** | **61.4 (61 %)** | 16.0 (16 %) | 23.8 (23 %) | 3.2 avg |

- The direct exchanges cost 101 s per thread (m) = 27 % of the 376 s wall, not the 136 s derived in XEFF section 0 (that figure subtracted a model).
- **Skew is 61 % of them (61 s, m; r2 62.7 s)**, concentrated in two places:
  1. **Reciprocal's first addsh: 19.2 s on nine nodes and 1.2 s on node 1 (m, r1 / r2 / rot all the same, 21-25 s).** Node 1 reaches the reciprocal's first
     exchange about 20 s after the others; the other nine nodes idle for it. This is the "single-node chain" of S19B (floor 2.28 s, typical 6.8-22 s). It is
     structural (same node, three runs), and rot makes it no better (22.2 s).
  2. **Result exchanges after each product round: dm 22.3 s, bs 12.0 s.** Per-rank roff of dm result (s, r1): 33 32 25 22 16 16 14 21 19 24 for ranks 0..9
     (r2 and rot the same shape): ranks 0-1 wait twice as long as ranks 4-6; bs result: ranks 8-9 wait most (15 vs 8 s). A persistent rank-dependent lead of up to
     about 19 s (m), matching S20's "same ranks wait most".
- Only **sig 16 s + other 24 s = 40 s** is transport-shaped, and 12.2 s of the sig is in the multi-round redistributions (dm redist 4.5 rounds per call, bs redist 4.3,
  dm addsh 5.4, bs addsh 9; single-round kinds have sig < 1 s).
- Roughly: of the 101 s, about 60 s cannot be removed by any transport change (it is waiting for a slower node and would reappear as other waiting if the transport
  were faster, which is exactly what X1 showed).

##### 2.2 Layered exchanges (layv, layeq), stats_r1

- layv: 109.7 s per thread for 1252 GB = **11.4 GB/s**; t_wait 93.6 s (85 % exposed), t_call 16.1 s (overlapped caller work), roff 8.3 s, sig 4.4 s. The remaining
  ~80 s of t_wait is the data push and flush itself (wire-bound; NIC raw 23.3 GB/s, NIC16). By phase: dm 53.1 s (11.6 GB/s), bs 36.7 (11.3), recip 19.9 (11.2).
- layeq (intra stage): 9.4 s, 13.4 GB/s, 126 GB.

##### 2.3 Per-APU rates and the slow mesh (m)

- Per APU (mean over nodes, layv): 11.40 / 11.43 / 11.41 / 11.45 GB/s (r1); r2 11.15 / 10.62 / 9.87 / 10.54. Node 0 (r1): 11.53 / 11.56 / 11.53 / 11.58. **No
  slow mesh in r1 (spread 0.4 %)**; r2 has a 12 % spread but it is a slow run (399 s) and not the same APU as node 0. XEFF section 1.6's "max/min 1.10-1.18" does not appear
  under per-exchange timing; it came from fabric time that included waiting. **The NIC-binding / mesh-0 hypothesis (item 4's second claim) is not supported.**
- Rot lifts all four APUs equally (13.6-13.8 GB/s).

##### 2.4 What this means for the remaining candidates (10 nodes, per thread seconds ~ wall; labels in the table)

| rank | candidate | measured ceiling (m) | expected wall gain at 10 nodes | 576 nodes | effort | note |
|---|---|---|---|---|---|---|
| **1 (new)** | **Reciprocal chain: node 1 is 20 s late into the reciprocal (find what it does, then replicate the chain on every node or overlap it)** | 19.2 s of idle on nine nodes; floor 2.28 s | **-8 .. -17 s (a), ceiling -19 s** | chain length does not depend on node count (a), so the same 10-20 s of about 219 s (5-9 %) | S diagnosis (`ECALC_LOG_CLOCKS`, 1 run), then S-M | only candidate whose size is measured and direct; needs one diagnostic run before code |
| 2 | Result-exchange skew: rank gradient in dm/bs result (rebalance the split, or let the early ranks start the next round) | 35.3 s total roff; the removable part is the gradient (mean - min = 8 s dm, 6 bs) | -5 .. -12 s (a) | unknown | M | needs the per-rank work (which ranks get more limbs/rows) read first |
| 3 | X2 `INTER2 + VSLOT_POOL` (being A/B-tested in S28) | layv exposed wait 93.6 s, but 84 % wire-busy; rot cut exposed wait by 17.8 s and gave 0 on wall | -3 .. -12 s (a); XEFF's central 18 s is lowered because X1 showed layered gains turn into waiting | lockstep rounds at g = 576: the only candidate acting there | M (built) | wait for S28 |
| 4 | Pipelined rounds two deep (`RPIPE`) | sig 12.2 s in multi-round kinds | -3 .. -6 s (a) | -1 .. -4 | M | small |
| 5 | Fuse the three operand redistributions | redist t_wait 21.7 s; the fusable fixed part (roff 3.2 + other 6.3) 9.5 s | -2 .. -4 s (a) | -1 .. -3 | M | smallest |
| dead | rot (X1), OFI-native signal (X3), mesh/NIC rebinding | | 0 | | | |

- Ranks 4 and 5 are inside the noise of one run (sd of a single run about 8-10 s, mod from S22) and cannot be measured at 10 nodes by ABBA with less than about 8 rounds.
- Rank 1 is also visible in `MN_WAIT_STATS` (recip ready 24..117 s across nodes in stats_r1: node 1 is the 24 s one).

#### 3. S25 share ladder (10 nodes, A37-Q9 batch + ladder, base line; m)

| share per node | total s | bs | dm | init | peak rank 0 / others GB | layout + vslot GB | s per 1e10 | peak GB per 1e10 |
|---|---|---|---|---|---|---|---|---|
| 6.441e10 | 410.6 | 163.1 | 217.2 | 26.0 | 356.5 / 347.5 | 369.1 + 6.9 = 376.0 | 63.8 | 55.4 |
| 7.00e10 | 464.4 | 184.0 | 251.3 | 24.7 | 373.6 / 364.7 | 386.3 + 6.9 = 393.2 | 66.3 | 53.4 |
| 7.64e10 | 528.5 | 213.9 | 282.1 | 25.8 | 390.5 / 381.7 | 403.5 + 6.9 = 410.4 | 69.2 | 51.1 |
| 8.10e10 | skipped | | | | | 427.6 needed | | |

- **Time is not linear in digits.** Slopes: 96.2 s per 1e10 from 6.441 to 7.0 and 98.4 s per 1e10 from 7.0 to 7.64, against an average of 64-69 s per 1e10. Exponent from
  the endpoints: 528.5/410.6 over 7.64/6.441 gives share^1.48 (bs ^1.59, dm ^1.53, init flat 25-26 s). The 6.441 run was slow within its own noise (the same share ran 378-405 s
  in six other runs, mean about 385 s); against 385 s the exponent is 1.9. Either way the growth is above the n log n expectation (~1.05) and not explained by this data
  (single runs, noise +-15 s from the dm bimodality). The 7.64e10 target share costs about 128 s (+31 %) more than 6.441e10 for +18.6 % digits.
- **Memory is linear with a large fixed part**: peak rank 0 grows by **28.4 GB per 1e10 digits per node** (others 28.4 as well), fixed part about 173 GB (rank 0). The planned
  layout grows 30.7 GB per 1e10 (fixed 171 GB). Rank 0 carries about +9 GB over the others (bs phase peak).
- **What stops 8.1e10:** the driver's vslot rule, node needs 427.6 GB > 0.95 x MemAvailable 440.7 GB = 418.7 GB. The 440.7 GB is the **minimum over the 10 nodes**; idle
  nodes show 512-515 GB (s26 A/C), so one node has about 74 GB less available (page cache or leftovers, cause not looked up). With 515 GB the rule would allow about 9.4e10. At
  440.7 GB the ceiling is about 7.9e10 (by extrapolation, mod). The target share 7.64e10 passes with 8.3 GB (2 % of node memory) to spare under the rule.

#### 4. A37-Q9: the batch tier by phase, and A37-R5

10 nodes (`~/s25/phases/q9_r1`, `q9_r2`, `b_r1/b_r2` rehearsal, 6.441e10; bs 161.3 / 155.4 / 154.5 s):

| tier (10 n) | wall s | scatter | ntt | crt | merge | unaccounted |
|---|---|---|---|---|---|---|
| batch, 23 levels (q9_r1 / r2) | 21.3 / 22.0 | 2.29 / 2.44 | 10.89 / 10.84 | 3.51 / 3.65 | 0.30 / 0.50 | 3.6 / 3.8 |
| mdev, levels 24+25 | 12.6 | | | | | |

- Levels: level 1 5.05 s (batch 2.94 + scatter 0.46 + ntt 0.93 ...), level 2 1.57, level 3 0.82: **levels 1-3 are 7.4 s (35 % of the batch tier)**, levels 4-20 are
  0.44-0.62 s each (about 9 s), 21-23 are 1.0 / 1.7 / 1.8 s. mdev: level 24 3.6 s, level 25 **9.0 s** (the single top pair) = 12.6 s = 3.3 % of wall.
- 1 node (`~/s26/C/phases` q9_r1-3, 6.85e9 N, bs 44.3-45.2 s): batch 20.56 s (scatter 2.05, ntt 11.19, crt 3.43, merge 0.19), mdev 13.9 s (4.0 + 9.9). The batch tier is
  communication-free, so 1 node and 10 nodes agree within 4 % (20.6 vs 21.3 s); level 1 is 4.0 vs 5.05 s (10-node layout has more pairs/slower first level).
- Tier is 13 % of bs at 10 nodes (21.3 of 161) and 46 % of bs at 1 node.
- **A37-R5 (batch pipeline): not worth it.** Overlappable work = scatter + crt + merge = 6.1 s (m), plus the 3.6 s unaccounted gaps; NTT (10.9 s) stays serial. A perfect
  overlap would give at most about -6 s, realistic -1.5 .. -3 s (mod), i.e. 0.4-0.8 % of the 10-node wall, for 1-2 days. TASKS' <= -2 s per node is confirmed as the upper
  bound; drop it behind the items in section 2.4. Byproduct: the 9.0 s top mdev level (level 25) is a larger single block than anything R5 could pipeline.

#### 5. S26 soak counts (1 node each, all rc 0, VERIFY OK, no diff, no hang)

| role | what | segments | ok / total (from soak_counts.txt) | wall per run |
|---|---|---|---|---|
| A | 1e9 digits, 4 APUs, repeated | `A_run1` 92 (to 11:14) + `A` 378 (to 13:15) | 470 / 470 | 12-16 s |
| C | 1e9 digits (also did Q9 first) | `C_run1` 63 + `C` 350 | 413 / 413 | 12-16 s |
| B | per-node share class (VmHWM 26.8 GB), 1 node | `B` 92 | 92 / 92 | 91-96 s |
| total | | | **975 / 975, 0 bad, 0 diff, 0 hang** | |

- soak.tsv rows exceed counts by 2 (A: 380 vs 378; C: 352 vs 350), all `ok 0`; probably rows counted after the counter was written (a). `hang/` is empty on all five dirs.
- A and C were still running at 13:15 (counts keep growing); B finished 92 runs. Together with the earlier 140/140 (RESULTS §84) these add 975 runs at 0 failures; the
  race fix (A114) remains consistent with 0 hangs, which is not a proof (the 2026-09-23 hang was never reproduced).
- The 10-node full-share runs of S25 (ladder 3 + rehearsal 2 + Q9 2 + S27 9 = 16 runs of 6.4-7.64e10) are also all VERIFY OK.

#### 6. Top recommendation for the next code item (10-node hold)

**Do not build more transport variants first. Diagnose and remove node 1's 20 s lateness into the reciprocal** (section 2.1 item 1): one 10-node run with
`ECALC_LOG_CLOCKS=1 COMM_XSTATS=1` (about 8 min) to see what node 1 does between the end of the preceding phase and the first reciprocal exchange; then the fix is probably
computing the single-node chain on every node (no broadcast, deterministic, digits identical) or starting it earlier/overlapped. Measured ceiling -19 s (5 %), the same
absolute size at 576 nodes. Then X2 (S28 decides), then the dm/bs result rank gradient.

Decisions: (1) approve the diagnostic run and the recip-chain switch (off by default); (2) optional 30 min check of the XSTATS+rot anomaly; (3) A37-R5: drop.

#### RESUME

State: analysis only; nothing launched on aac7. Open: S28 (X2) result; recip-chain diagnostic; XSTATS+rot anomaly check; why the share ladder exponent exceeds 1.4.

## S31 / S32 / S33 -- X2 second try, host-vs-rank, VMM_SAFE soak: the comm pool failure of S28 and its fix (2026-10-08)

(former file: S31.md)

#### What failed in S28 (measured, ~/s28 logs)
- Gates at 3 nodes (base, VSLOT_POOL, both), 6 nodes (both), 2 nodes (VSLOT_POOL, both) and the INTER2-alone negative gate: all passed, digits identical to ~/ref/e_1000000000.txt.
- Every 10-node X2 run failed (xs_r1, x2_r1s2_B, x2_r2s1_B; the base A run x2_r1s1_A passed: 384.5 s, peak 356.6 GB), at the first products:
  `comm_ofi: the comm pool of device 0 (4096 MiB) cannot hold 1023 MiB more (in use 2664 MiB): COMM_OFI_POOL_MB >= 3690 needed`.
- The message is misleading: 4096 - 2664 = 1432 MiB are free in total. It is fragmentation, not a shortage: the first-fit pool had no 1023 MiB hole.
  Cause: with COMM_LAYER_VSLOT_POOL the two v-slots of a lay comm regrow by free-then-allocate with every larger product, between the staging blocks (up to 1 GiB, ROUND_MB=1024) that the same pool serves.
- Reproduced host-only (tests/t_ofi_alloc, 4096 MiB pool, slots 400+400 -> 822+822, scratch 1023, staging 1023): in use 2667 MiB, free 1429, largest hole 800 -- the S28 numbers to 3 MiB.
- The pool size itself is right: plan 4096 MiB/device = 2048 staging + 1640 v-slots (2 x 820) + 256 margin. A base run puts the 1.72 GB of slots in hipMalloc outside the pool (comm pool 2.42 GB), X2 puts them in (4.29 GB): the total is the same.

#### Fix (all behind COMM_LAYER_VSLOT_POOL=1; digits cannot change: only where a buffer lives)
1. `COMM_LAYER_VSLOT_MB_<g>`: the pool rule (binsplit.c vslot_prealloc_env) sets, per group size g, the model's per-slot maximum; need_vslot allocates each slot once at that size.
2. `COMM_OFI_SYM_TOP` (on with VSLOT_POOL): symmetric buffers top-down, staging bottom-up.
3. Diagnostics: comm_ofi's fatal now dumps free total / largest hole / used blocks; `COMM_OFI_VERBOSE=2` traces allocations >= 64 MiB.
4. s31_batch.sh: the 10-node probe (xs_r1) ends the batch at once on failure (S28 spent its ABBA slots).

#### Memory (modelled, mem_model.py with COMM_OFI_PLAN_CXI=1, ROUND_MB=1024)
| nodes, digits/node | comm pools X2 - base | hipMalloc v-slots removed | net layout per node |
|---|---|---|---|
| 10, 6.441e10 | +7.5 GB | -6.9 GB | +0.6 GB |
| 576, 3.71e13/576 (MN_GROUPS 2,4,8,16,32,64,192,576) | +14.0 GB | -13.5 GB | +0.5 GB |
(576: the model's absolute node totals are above the 373.44 GB measured device edge because mem_model's defaults differ from the measured target line; the X2 - base difference is what matters.)

#### RESUME
See the end of this file (unit results and the arming record are appended by the author).

#### Unit tests (hold 12332, CPX, 1 node; measured, 2026-10-08)
- tests/t_ofi_alloc (host-only allocator): regrow pattern reproduces S28 (in use 2667 MiB, largest hole 800 < 1023) and shows sym-top alone does not cure it; preallocated-slot pattern fits with sym-top 0 and 1 (3690 MiB peak of 4096).
- t_mn_grid 0.25 28, 3 processes: switches off, X2 on: 3/3 VERIFY OK each (second mesh used, 3 lines). t_dist 24 DIST_LAYERED=1: off 3 procs OK; X2 on 3 procs and 2 procs OK; INTER2 alone stops at init as designed; X2 with SYM_TOP=0 + PREALLOC=0 OK.
- ecalc 6e7 digits, 3 processes, POOL_LOG=26 DIST_LOGN_TEST=24, X2 on: 4 VERIFY OK; `plan vslot-prealloc COMM_LAYER_VSLOT_MB_3=5 MiB per slot` printed. (6 processes on one node run out of device memory with the base too; the 6-node gate is on the 10-node hold.)
- Not tested at scale: the 10-node pool behaviour; the batch's probe (xs_r1) is that test.

#### S31 results: X2 at 10 nodes (aac7, hold 12331, 6.441e10 digits/node; m = measured, i = inferred, a = assumed)
- **Gates and probe (m):** the pool fix works: all gates passed, the 10-node probe xs_r1 (COMM_XSTATS=1 MN_WAIT_STATS=1) ran VERIFY OK, total 357.6 s (dm 180.9). 33 runs, 2 crashes, 16 ABBA rounds, all other runs VERIFY OK.
- **ABBA (A = base, B = base + COMM_LAYER_INTER2=1 + COMM_LAYER_VSLOT_POOL=1), 14 complete rounds (2 rounds lost to segfaults, below):**

| set | n rounds | mean A | mean B | B - A | 95 % CI | B faster |
|---|---|---|---|---|---|---|
| all rounds | 14 | 412.9 s | 378.8 s | **-34.1 s** | -69.7 .. +1.5 | 11 of 14 |
| without dm blow-up rounds | 12 | 401.3 s | 369.6 s | **-31.7 s** | -55.7 .. -7.8 | 10 of 12 |

  The CI of the full set touches zero (t = -2.07, crit 2.16); the clean-set CI excludes zero. The batch's own rule said CONTINUE (CI half 35.6 s > 15 s).
- **Spread by arm (m, 15 non-crash runs each):** wall mean A 431.2 s sd 49.7, B 394.7 s sd 29.6 (A is wider because of its blow-ups and a cold r1). Typical clean B wall 380-400 s, A 400-420 s (walls are 20 s-quantised by the driver poll).
- **dm blow-ups (dm > 293 s; median dm 195 s):** strict count 1 per arm: r1 A (dm 369.9 s), r10 B (298.2 s). Near-blow-ups (dm 290.2 / 290.9 s) are both in A (r5, r16). Median dm: A 199 s, B 188 s (-11 s). **X2 does not clearly reduce blow-ups (1 vs 1, n too small); it lowers the typical dm by ~11 s and the exposed wait (below).** Quote the clean -32 s.
- **Memory (m, rank-0 device peak, 15 runs each):** A mean 356.60 GB (max 357.00), B 357.41 GB (max 357.80): **+0.81 GB**, as modelled (+0.6 GB per node). No headroom problem at 10 nodes; 576-node model: +0.5 GB per node (modelled, above).
- **Layered rates (m, xs_r1, X2) versus S27 base (stats_r1):** exposed wait in layv: dm 33.7 + bs 22.9 + recip 12.0 = 68.6 s versus 93.6 s in the S27 base (**-25 s, i**). The per-thread rate column is not comparable: it reads 8.2-8.3 GB/s (t_post 151 s) versus 11.4 GB/s in S27, because with INTER2 the caller's posting time t_call (83 s versus 16.1 s) is now inside t_post; the wall (357.6 s versus 375.9 s S27 stats_r1) and the wait are the honest comparison. Skew is unchanged: bs result roff 12.0 s, dm result 23.1 s, recip addsh 18.6 s (node 1's stall, below).
- **576-node effect (a, not modelled with ecalc/mn_model.py):** no run at scale exists. The only support is a 25 s wait saving, 6-8 % of wall, at 10 nodes; whether it grows with node count depends on the layered path count, which S31 did not measure. Treat -6..-8 % as the 10-node figure only. Memory cost scales as modelled (+0.5 GB per node).

#### S32 results: host versus rank, eviction, parallel 2-node crash soak (hold 12331; b/c finished, soak still running at 08:20 EDT 2026-10-09)
- **Host vs rank (m, b_base1/2, b_perm1/2; nodelist permuted so rank 7 = aac7-n01):** unchanged runs: the stall is rank 1 = host **aac7-n01**, 21.1 s and 25.2 s in db_from_bi (upload) (db_init and first_pool_get 0.0 s; median node 1.0 s). Permuted runs: the stall moved to rank 7, the same host, 20.9 s and 20.7 s. **Verdict: it follows the host (2 of 2 permuted runs), the rank 0 of 2.** Cost ~20-25 s per run, ~5 % of 390 s.
- **Stage (c) eviction (m):** dd-nocache on the host changed nothing (MemFree 409.4 to 409.2 GB, Cached 113.73 GB unchanged), c_evict stall still 21.3 s. **Inconclusive: the dd did not drop the cache, so eviction itself is untested.** Context (m, meminfo.txt): that host has MemAvailable ~440.5 GB like its 440 GB peers but **Cached 113.7 GB**, the largest cache of the ten (peers 82-112 GB; 520 GB nodes 47-60 GB). Hypothesis (i): cache pressure or slow reclaim/non-local allocation in the upload; not proven.
- **Parallel crash soak, 5 simultaneous 2-node pairs, A = base, B = ECALC_VMM_SAFE=2 (m, 2026-10-08 19:30 to 2026-10-09 08:20, 590 runs per arm):** A 0 crashes, mean wall 185.4 s; B **3 crashes** (p4_r30 aac7-n40, p2_r166 aac7-n21, p0_r214 aac7-n00), mean wall 185.9 s: **VMM_SAFE=2 costs +0.5 s (0.3 %) and did not remove crashes**. The soak_stats "rc139" column reads 0 because srun reports the segfault as rc 1; the crashes are the 'Segmentation fault' lines (wall 42 s, same moment as at 10 nodes). 3 of 590 vs 0 of 590 is not significant (Fisher p ~ 0.12): no evidence of benefit.
- **SEGV traces (ECALC_SEGV_TRACE=1, m):** the traces read (p4_r30, p0_r214, and S33's) show the same faulting-thread stack: ecalc (0xa69b3b, 0xa6a292) -> libamdhip64 +0x36bb8e, +0x8d21d, +0x8cf57, +0x3cbd39, +0x403b0d, +0x3f8624 -> SIGSEGV; the fault address varies. It is inside the HIP runtime, called from an ecalc pthread: consistent with the VMM background mapper racing with the main thread's HIP calls (i). addr2line of the ecalc frames not done.

#### S33 results: 2-node base vs VMM_SAFE=2, plus 1-node control (holds 12377, 12390; digest 08:16 EDT, still running)

| arm | runs | segfaults | other | mean wall |
|---|---|---|---|---|
| 1 node, base (control) | 515 | 0 | 0 | 100.1 s |
| 2 nodes A (base) | 142 | **1** (p0_r167, aac7-n51, 02:26 EDT) | 0 | 182.8 s |
| 2 nodes B (VMM_SAFE=2) | 141 | 0 | 0 | 182.3 s |

(The digest's "other failures 1" in 2n/A is that segfault.) Pooled 2-node S32 + S33 (m): base 1 of 732 runs, VMM_SAFE=2 3 of 731. **The crash occurs at 2 nodes too (0.14 % per run base), not only at ten; VMM_SAFE=2 neither fixes nor clearly worsens it.** Same stack.

#### Ten-node crash record, 2026-10-08 (m)
3 segfaults (rc 139, wall 42 s, 0.3-0.5 s before binsplit level 1, at the end of the VMM background mapping) in ~85 ten-node runs (3.5 %), 0 in the ~59 before today:

| run | host of the faulting task |
|---|---|
| s30 d30_r4s2_A | aac7-n21 (task 5) |
| s31 x2_r3s2_B | aac7-n30 (task 6) |
| s31 x2_r11s1_A | not isolated: the ALERT lists tasks 1-8 killed afterwards; the "SEGV [rank ..]" line is in ~/s31/log/x2_r11s1_A.log (not found by grep) |

Two of the three are base arms: **X2 does not cause the crash and does not cure it.** Three different hosts, none is the stalling host aac7-n01.

#### Decisions for the user

| # | decision | options | recommendation |
|---|---|---|---|
| 1 | adopt X2 (INTER2 + VSLOT_POOL) on the aac7 line | adopt: -32 s (-8 %) clean, -34 s all rounds (CI to +1.5), +0.8 GB, digits identical; or wait for one more ABBA block (clean CI half 24 s) | **Adopt** as an option: memory-neutral, 10 of 12 rounds faster. Target (576 nodes): keep it as an option; the scale gain is not measured, test it at the first >= 64-node run |
| 2 | VMM_SAFE=2 | adopt (0.3 % cost, but 3 of 731 crashes vs 1 of 732 base: no benefit); or diagnose next | **Do not adopt.** Next: addr2line the traces; soak with synchronous mapping (background mapper off) versus on at 2 nodes, 600 runs per arm, 5 pairs ~6 h; the stack points into libamdhip64 from the mapper thread |
| 3 | host aac7-n01 | exclude it from timed 10-node holds (saves 20-25 s, ~5 %); or keep and report to the cluster admins | **Exclude for timed runs and report it** (stall in 4 of 4 runs, Cached 113.7 GB) |
| 4 | free-memory / page-cache check in the target kit | add a per-node pre-run check of MemFree, Cached and a timed 1-GB upload; warn or refuse on outliers | **Yes**: in a lockstep job one slow node costs ~20 s, and at 576 nodes the chance that one is slow is large |

#### RESUME
- Nothing running from this analysis. S32 and S33 soaks were still going at 08:20 EDT; final digests land in ~/s32 and ~/s33 results.txt (count 'Segmentation fault' lines in pairs.tsv: rc 1, wall ~42 s).
- Open: addr2line of the S32 traces; read the x2_r11s1_A faulting host; synchronous-mapping soak; exclusion of aac7-n01.
