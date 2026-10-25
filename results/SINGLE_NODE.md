# SINGLE_NODE — single-node speed and options

Scope: single-node speed and options: init, level pairing, remaining options on the defaults, development items, seed terms, combined switches. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; unabridged text: `archive:results_merged_2026-10-19/`. Numbers are measured (m) unless marked modelled (mod) or assumed (a); times Eastern.

Sections moved to `archive:results_merged_2026-10-24/SINGLE_NODE.md` (2026-10-24 cleanup): §P; §V214; §V314; §B08.

| item | title |
|---|---|
| §P | single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11) |
| §V214 | remaining one-node options on the Phase 14 defaults: auto's per-piece cost, DIST_TWREC, checkpoints with O_DIRECT |
| §V314 | the remaining development items (PLAN §33.2 B1-B7, APUMULT E11), with numbers |
| §P1N | one process per GPU at one node: which phase loses (D vs P, ofi and Cray) |
| §T215 | `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths |
| §CAND | the three off-by-default speed switches combined |
| §S21 | single-node A37 measurements (seed microbench, init timeline, dc split, small-j doublings, NTT structure) |
| §B08 | how fast can the unit tests be? |

## Topic summary

- **What it is.** Single-node (one MI300A node, 4 APUs) speed work from Phase 11 to the Oct-2026 switches: what happens in init, the seed thread, the batch and mdev tiers, the reciprocal and the division, and which switches were adopted. Switch rows and defaults are in `ecalc/README.md`; the adopted list is in `RESULTS.md`.
- **Adopted / default now.** `RNS_STRIPED_PAIR=1` (§P A2, level 22 paired, -0.55 s at 4 x 10^10); `ECALC_DM_POOL` on from 5 x 10^10 (§P: 7 x 10^10 153.5 s); `RNS_AUTO_PIECE_COST` (built in since 2026-10-12; §V214: -12.2 % / -10.1 % / -6.4 % of `total` at 1e11 / 1.16e11 / 1.3e11); **`BS_SEED_FILL=128` (default since 2026-09-27, user decision; §T215: bs -12...-16 s, wall -6...-16 s at 1e11, -6.6 s (-10 %) at 4 x 10^10)**; I11's init table uploads (bit-identical). Not adopted: `RNS_PLANES_3Q30` (default 0: planes save 3.6 s of phases but cost 4.7-6.3 s of mapping at init), `RNS_BATCH_TILE_GB` stays 15.
- **Reference walls.** 4 x 10^10 on one node about 81.5 +- 1.4 s (phases 66.0 +- 0.4 s, init 15.5 +- 1.1 s; Phase 11 final build); 10^11 about 215-217 s on Phase 14 defaults (init 17-18, bs 96, dm 101, recip 43), 198.8-203 s with fill 128. aac7 mapping is slower than aac6 (0.070-0.084 s/GB vs 0.047 s/GB alone); init is the max of the seed thread (about 15 s) and the pool mapping.
- **Live conclusions.** (1) The seed thread and the arena mapping finish together on aac7; GPU seeds (A37-R1) are a no-go for now (0-6 s, mod). (2) A seed span that makes the first batch products fill 2^k lengths (fill 128) beats the default; larger fills lose to the seed cost. (3) Every S change shifts the block pool's VMM remaps (serialized under the global pool lock): dm +1-4 s at 1e11, a per-device remap lock (RL, `dbig.c`) is the cure. (4) `ECALC_BUDGET_CHECK` stops 1e11 on one node on the V314-era defaults (rc 8, counts the borrowed pool twice); V314's 1e11 runs used `ECALC_BUDGET_CHECK=0`. (5) The correction count after the division hook is data dependent: a correction after the writer started forces a redo of the digits (§V214 finding; later removed by the K patch, see `results/OUTPUT_IO.md §KP15`).
- **V314 ranking (modelled, at 1e11 / 576 nodes).** (1) decimal `mul_1` + fused seed span -9 s / -9...-11 s; (2) per-run seed terms -6.6 s (this became §T215's `BS_SEED_FILL`); (3) radix-3 pass fused into the first 2^k pass -6...-8 s / -3...-4 s; (4) reciprocal middle product -3.4 s / -1.0 s; B4 (5.2^k / 7.2^k lengths) needs a new prime set: not recommended.
- **Combined switches.** `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1` together: -7.16 s [-8.08, -6.23] at 1 node (aac6, 6.441e10), -5.58 s [-10.54, -0.63] pooled at 2 nodes (aac7); digits identical, no memory cost; adoption is the user's decision (§CAND).
- **Open.** `RNS_AUTO_PIECE_COST` is built in; `DIST_TWREC=1` at 1e11 and checkpoints with O_DIRECT in §V214 were never measured there (O_DIRECT checkpoints were measured in `results/OUTPUT_IO.md §IO15` and §S114); the 4-node cand round (item 7g) is not recorded here; §B08's `t_ntt` fix is a test-suite change.

## §T215 — `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths (Phase 15, row 1c)

Branch `p15-T2` from int15 0b80ca3. **Adopted: `BS_SEED_FILL=128` is the default (user decision 2026-09-27).**

**1. What decides the fill.** `binsplit.c` combines spans of S terms pairwise; a batch level uses one length for all products, `pick_len(maxnc)` = 2^k, or 3.2^(k-2) when k - 2 >= 10 and maxnc <= 3.2^(k-2); with m1 = the last span's limbs, maxnc(l) <= 2^l.m1 and m1 about S.log10(k_max)/18. If m1 <= 2^j every product from 2^10 up fills a 2^k length (fill >= 0.99, mod) and the batch tier's top level ends exactly at the mdev threshold, so **one more level runs in the batch tier** (3-4x cheaper per output limb than mdev: V3's S = 168 level 23 in batch 2.85 s vs about 10 s in mdev). Otherwise padding is 1.34-1.5x: **S = 256 at 1e11 has m1 = 143: fill 0.74 from level 4 on; at 4e10 m1 = 138: 0.71** (mod; the model's `max_nl` matches the log's at all 26 levels). Cost terms fitted to V3's logs (mod): batch 3.39e-10 s per per-APU point + 1.15e-7 s per span (+-0.5 s); mdev 1.23e-18 s per limb^2 + 0.89 s per level (+-3 s). **Rule: S = the largest S whose last span has <= 128 limbs**: S = 239 at 4e10, **229 at 1e11**, 235 on the 576-node plan's node 0, about 184 on its last node (k about 3.5e12); no single `BS_SEED_TERMS` fits every size or node.

**2. The switch.** `BS_SEED_FILL=<limbs>` (`binsplit.c bs_seed_terms_for(bend)`, lgammal; overrides `BS_SEED_TERMS`; 0 = off, byte-identical by construction: the span changes only the tree's shape, P and Q are unique); one line each in `seed_limbs` and `binsplit_e`; `mn_plan.c plan_leaf` (one line) so `MN_PLAN_ONLY` plans the same tree (576-node node 0: S 256 -> 235, mdev levels 3 -> 2, leaf dist_db pieces 16 -> 12, mod). `tests/t2_sweep.sh` and `archive:scripts_2026-10-23/t2_seed_model.py` are the tools.

**3. The +3.4 s in `dm` at S = 168 is real: block-pool VMM remaps (job 21461, 594ba4c, 1e11, no digit file; all VERIFY OK).**

| run | S | total | init | bs (seeds / batch / mdev) | recip | dm | VMM remaps in recip + dm |
|---|---|---|---|---|---|---|---|
| d1 / d2 / dv | 256 | 210.75 / 211.53 / 210.73 | 17.6 / 17.0 / 17.1 | 93.3 / 94.2 / 93.5 (19.5-20.5 / 35.4-35.9 / 37.5-37.9) | 41.2-41.6 | 98.84 / 99.42 / 99.19 | 3, 1.48 s |
| s1 / s2 / sv | 168 | 200.64 / 204.92 / 202.39 | 15.5 / 18.4 / 15.7 | 83.1 / 84.3 / 84.1 (14.9-15.3 / 34.7-35.7 / 33.2-33.4) | 41.2-41.6 | 100.91 / 101.14 / 101.57 | 8, 3.74 s |
| **f128 (S = 229)** | 229 | **198.79** | 18.4 | **78.57 (19.7 / 29.5 / 29.2)** | 42.10 | 100.80 | 12, 3.99 s |

dm 101.21 s (mean of 3) at S = 168 vs 99.15 s at 256: **+2.06 s**, in "A mu + shift" (25.50 -> 27.47 s). The division's two 22.22 GB blocks per APU (t, then xq) need a contiguous extent: at 256 t goes into the arena's reserved tail and only xq remaps (APUs 1-3); at 168 the tail is gone, so t and xq both remap on all four APUs (8 remaps, 0.45-0.49 s each); `vmm_make_room` runs under the global pool lock so the four are serialized (4 x 0.46 = 1.84 s). Suggestion (outside this branch, `ecalc/dbig.c`): remap per device in parallel, or map APUs 1-3's arenas one chunk larger (mod: -1.0 s at the default, -2.8 s at S = 168, -2.5 s at S = 229). **Rule's first run: f128 198.79 s vs 211.00 s (mean of 3): -12.2 s (-5.8 %)**; bs 78.57 vs 93.64 (batch -6.1, mdev -8.5, seeds +-0), dm +1.65 s; S = 168: 202.65 s mean of 3 (-8.3 s). The model predicted -8.4 s for bs - seeds, measured -14.7 s (it under-counts the mdev gain).

**4. Step 2 — the sweep on int15-seed (4e0edd4; jobs 21519, 21529, 21541, gates 21545; 2.6 node-hours).** 40 runs, all VERIFY OK; 16 written runs byte-identical to the reference (sha1 on the node: 4e10 `43c84e49...`, 1e11 `578f5efb...`). One run per cell; noise at 1e11 about +-3 s. Each config run with `BI_MUL1_FAST=0` then 1 (FAST).

4 x 10^10 (wall without the write, FAST 0 / 1, s):

| config | S | no-write wall | with write | bs (seeds / batch / mdev, FAST 1) | dm | remaps |
|---|---|---|---|---|---|---|
| default | 256 | 67.40 / 64.74 | 66.34 / 65.85 | 30.7 (7.3 / 15.5 / 7.8) | 22.67 / 22.65 | 4, 0.86 s |
| fill 96 | 179 | 59.26 / 59.40 | - | 25.4 (6.5 / 14.9 / 4.0) | 21.96 / 22.08 | 0 |
| **fill 128** | 239 | 60.53 / **58.19** | 61.69 / **59.12** | **23.8** (6.6 / 12.5 / 4.6) | 22.51 / 22.39 | 2, 0.44 s |
| fill 192 | 358 | 60.68 / 58.87 | 61.49 / 61.68 | 25.6 (9.2 / 12.5 / 3.9) | 22.14 / 22.03 | 0 |
| fill 256 | 478 | 62.47 / 61.78 | 62.78 / 61.33 | 27.3 (11.9 / 10.8 / 4.5) | 22.08 / 22.25 | 0 |
| control S = 242 (fill 128 + 3 terms) | 242 | 65.14 / 64.70 | - | 30.3 (7.7 / 16.2 / 6.3) | 22.64 / 22.48 | 2, 0.42 s |

10^11 (nw jobs 21519 / 21529; w jobs 21541):

| config | S | no-write wall FAST 0 / 1 | with write FAST 0 / 1 | bs (seeds / batch / mdev, FAST 1) | dm | remaps |
|---|---|---|---|---|---|---|
| default | 256 | 213.13 / 208.91 | 236.64 / 232.33 | 92.3 (17.3 / 37.2 / 37.7) | 100.42 / 99.27 | 3, 1.4 s |
| fill 96 | 172 | 205.71 / **200.75** | - | 81.5 (13.0 / 35.0 / 33.3) | 101.66 / 101.23 | 8, 3.7 s |
| **fill 128** | 229 | 213.41 (outlier) / 203.31 | 224.30 / **216.62** | **79.8** (15.9 / 33.5 / 30.1) | 103.16 / 103.67 | 12, 3.9 s |
| fill 192 | 344 | 207.37 / 205.17 | 231.99 / 226.15 | 85.7 (22.6 / 29.5 / 33.5) | 101.11 / 101.50 | 8, 3.7 s |
| fill 256 | 459 | 208.87 / 203.97 | 228.84 / 228.57 | 83.6 (28.6 / 25.7 / 29.3) | 101.55 / 101.34 | 9, 3.4 s |
| control S = 232 | 232 | 211.20 / 209.19 | - | 89.2 (16.7 / 38.9 / 33.5) | 100.96 / 101.46 | 12, 3.9 s |

(The fill-128 FAST-0 outlier: level 1 took 9.46 s and the seed wait 23.7 s because the arena's background VMM mapping ran 15-22 s.)

**What the sweep shows.** The fill mechanism is confirmed: the controls (3 terms above fill 128) lose almost the whole gain (4e10 64.7-65.1 vs 58.2-60.5 s; 1e11 bs 89.2 vs 79.8 s). **Fill 128 is best or within noise of the best at both sizes:** 4e10 FAST 1 -6.6 s without / -6.7 s with the write (-10 %); 1e11 FAST 1 bs -12.5 s (nw) / -15.8 s (w), wall -5.6 s without and -15.7 s with the write (the with-write difference includes T1's wait for the writer 20.8 vs 26.0 s). Fill 96 is as good without the write. **Larger fills lose even with S1's faster span:** seed wait 22.6 s at fill 192, 28.6 s at 256, vs 15.9 s at 128; S1 shortens the wait by 3-6 s at 1e11 and 0.5-1.5 s at 4e10, not enough: the rule stays 128 with or without `BI_MUL1_FAST`. FAST 1 vs 0: 1e11 bs -3.3 s at the default, -10.3 s at fill 128 (inflated by the outlier; -7.5 s in the w pair); walls -4.2 / -4.3 s at the default. The dm penalty: every fill has 8-12 remaps (3.4-4.0 s) vs 3 (1.4 s) at the default, dm +1-4 s (fill 128 +3.5-4.4 s, about a quarter of the bs gain); at 4e10 remaps <= 4 (<= 0.9 s), dm flat; RL's per-device lock should return about 2-3 s at 1e11 (mod). Level 1's time (4.4-9.5 s) follows the arena's background VMM mapping (3-22 s per APU): single runs at 1e11 differ by about +-3 s.

**Gates (job 21545):** `mnaccept unit,e9` 10/10 PASS; with `BS_SEED_FILL=128`, `--only e9,mn` 7/7 PASS (each node takes its own S from its last term); fills 128 / 192 / 256 x FAST 0 / 1 identical at both sizes (12 runs). Gap: mnaccept logs do not print the span, so they do not prove the environment reached every process.

**5. Recommendation and 576-node estimate.** Adopt `BS_SEED_FILL=128` after a five-run paired series at 1e11 (default vs fill 128, both `BI_MUL1_FAST=1`, ideally after RL's remap lock); single runs so far: bs -12...-16 s, walls -6...-16 s at 1e11, -6.6 s (-10 %) at 4e10. 576 nodes (mod): baseline stays 4.25 x 10^13 digits in about 4.1 min (PLAN §36.10; 5.3-5.8 min at the measured Lustre rate); with fill 128 the leaf loses one mdev level on node 0 (3 -> 2) and 4 of 16 leaf pieces; PLAN's -3...-7 s at the target (a) stands. **Open:** the dm remap penalty belongs to `ecalc/dbig.c` (RL); `mn_model.py` still assumes S = 256; the seed model's mdev term is crude (+-3 s).

## §CAND — the three off-by-default speed switches combined (interaction test)

Branch `cand` (off main a448d73): squashed from `o2` (`DBIG_SMALL_BOUNCE`, results/DIVISION.md §O2), `k1b` (`DIST_GATHER_RAW`, results/EXCHANGE.md §K1b), `k8b` at 99eee11 (`BS_SEED_PULL`, archive:results_merged_2026-10-24/MEMORY.md §K8b); no textual conflicts; switch registry regenerated (269 rows, 0 problems); all switches off by default. Arms: A = main binary (85d9d73f), switches unset; B = cand binary (7e7cd7da) with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`; 6.441e10 digits per node, aac7 line, no digit writes; paired AB/BA; every run VERIFY OK (aac7 24/24, aac6 8/8); gates PASS (aac7 2 nodes all on, 1e9 identical; aac6 1 node).

**aac7, 2 nodes (item 7e round 1, rerun as round 2), paired B - A:**

| round | pairs | paired B - A total (95 % CI, p) | paired B - A chain (95 % CI, p) |
|---|---|---|---|
| 1 | 6 (pair 1 cold: A 170.0, B 145.5) | -4.95 s [-15.45, +5.55], p 0.280 | -4.51 s [-8.39, -0.64], p 0.030 |
| 2 | 6 | -6.22 s [-12.20, -0.24], p 0.044 | -4.62 s [-7.61, -1.64], p 0.011 |
| **pooled (12 per-run totals)** | 12 | A 140.10 s, B 134.51 s: **-5.58 s [-10.54, -0.63], p 0.031** | **-4.57 s [-6.57, -2.57], p 0.0004** |
| pooled without cold pair 1 | 11 | A 137.38, B 133.52: -3.86 s [-7.39, -0.33], p 0.035 | - |

B was faster in 11 of 12 pairs. "Chain" is the `db_to_bi` conversion chain: B 0.05-0.06 s in every run; A 2.0-2.4 s normally but 6.1-10.8 s in 5 of 12 A runs (late PE up to +6.8 s on one node): those A spikes explain the spread; a single-switch effect cannot be separated (all three on). NIC launch groups occur in both arms (no group pattern visible, an effect cannot be excluded). Memory, rank 0: identical in A and B (device in use 317.8 GB, pool peak live 201.80 GB): no memory cost.

**aac6, 1 node (4 pairs, stop rule met):** totals A 74.10, 73.47, 72.86, 74.54 (mean 73.74 s); B 66.08, 66.55, 66.10, 67.61 (mean 66.59 s); **paired B - A -7.16 s [-8.08, -6.23], p 0.0001**, all four pairs negative. **Profile of cand all-on (6.441e10, 1 node, indicative):** total 66.45 s profiled / 66.91 s unprofiled; init 13.74, bs 36.56 (seeds 8.2, batch 21.1, mdev 7.1), dm about 16.6 s; vs main (O7b, 1 node, one run) total 73.32 -> 66.91 s, init 15.5 -> 13.7, bs 38.2 -> 36.6, dm 19.5 -> about 16.6. GPU busy per APU 47.8 / 49.0 / 48.7 / 41.9 s (main 43.7 / 44.7 / 44.4 / 38.1): `k_pull` 8.6 s per APU (18.5 %), `k_gather_raw<1>` 2.0 s (4.3 %).

**Verdict.** All three on together: faster at 1 node (-7.16 s, aac6) and 2 nodes (-5.58 s pooled, aac7), digits identical, no memory change; per-switch shares not separated; interaction vs the sum of the single results not evaluated here. Adoption is the user's decision. **Pending:** item 7g (4 nodes) was running; its result is not recorded in this file.

## §S21 — single-node A37 measurements (R2 seed microbench, Q1 init timeline, R4 dc split, R6 small-j doublings, Q4 NTT structure)

aac7 runs 2026-10-07 21:02-21:13 on hold 12377 (and a CPX node for R2); all rc 0, VERIFY OK; gates passed (digits identical with every switch off and on against the 1e9 reference; `t_ntt 24` OK).

**1. A37-R2 seed microbench `b_seed64` (CPX node, 192 threads).** dec18 (ecalc's decimal seed) vs b64kK (base-2^64 K-way); 451 leaves x 4 schemes, P and Q identical in every case. **Single thread: no gain** (b64k32 vs dec18 ratio 0.86-1.07: 36.3 k vs 34.1 k ns/leaf at 10 nodes rank 0, 30.5 k vs 35.3 k at 576 nodes rank 288). All-core wall scaled to nspan: dec18 9-15 s (agrees with production single-node seed spans 10.7 s), b64k32 6-8 s (25-45 % less, noisy, not measured in the engine; about -3...-5 s of seed-thread time, mod). Gate for A37-R1: **open** (a CPU rewrite does not close the gap by itself), but the exposed wait is small (§2).

**2. A37-Q1 init timeline at 6.441e10 on one node (`ECALC_INIT_TL=1`, aac7-n50, 2 runs; mapping 0.070 s/GB; total 96.4 / 93.4 s; init 14.3 / 14.4 s; VmHWM 26.9 GB).** Plane pools done 6.55 / 6.82 s; region pools there 14.11 / 14.20 s (seed waited 3.28 / 3.10 s); bs joins the seed thread 14.45 / 14.57 s; last background mapping done 27.84 / 23.97 s; spans done 25.09 / 24.64 s (10.74 / 10.75 s of span arithmetic); seed thread ends 25.50 / 24.89 s (thread wall 18.90 / 18.00 s); bs level 1 starts 27.11 / 25.39 s. **The seed and the mapping finish together** (the seed ends 2.3 s before the last mapping in run 1, 0.9 s after in run 2); bs waits about 11 s at the join. Seed cost without the pool wait about 15 s (A37CMP's modelled 22 s is too high for 6.44e10 by about 7 s). Target (mod, low confidence): mapping 7x faster -> seed thread ends at about 16-17 s next to the 16.5 s init floor; a GPU seed would save about 0-6 s, not 7-12 s. **A37-R1 (GPU seed): NO-GO now (confidence medium)**; the cheap decisive step is one `ECALC_INIT_TL=1` run on the target's share.

**3. A37-R4 split of `dc` (DC_STATS, 6.441e10, 2 runs).** The output loop is 4.10-4.11 s over 240 chunks: D2H fetch 1.03-1.06 s (26 %, 27-28 GB/s), limb reversal 0.48 s (12 %), **T1 digit residues 2.26-2.27 s (55 %)**, T2 windows 0.32 s (8 %), writer wait 0.00. The loop is streamed with the low product so on-the-clock `dc` is only 0.02-0.03 s at one node (S19B: 2.8-4.1 s at 10 nodes). **No ASCII formatting is in the run** (packed output; formatting is off the clock in `tools/unpack_digits`): the fmt18 two-digit table is not worth building for the clock; the T1 residue loop could be overlapped or moved to the GPU for at most about -2 s at 10 nodes (mod); backlog only.

**4. A37-R6 reciprocal doublings (`NEWTON_DOUBLING_TS`, one node, 3 578 333 337 limbs).** j 2 to 34126 (about 15 doublings): 0.001-0.003 s each, **total about 0.02 s**; j up to 1.7M cumulative 0.07 s; last four doublings (j 2.2e8 to 1.8e9) 0.38 / 0.31, 1.16 / 0.61, 1.29 / 1.24, 3.82 / 3.86 s; whole chain 11.24 / 6.85 s including a start-up of **4.14 s vs 0.43 s** before the first doubling. **The small-j floor on one node is 0.02 s**, nothing to remove (S19B's 2.28 s floor at 10 nodes is distributed latency, 8.5 us per put, F16); a one-APU small-j path is not worth it on this evidence. New: the start-up varied 3.7 s between runs (run 1 total 106.3 vs 93.9 s), cause unknown (first touch / allocation / cold path).

**5. A37-Q4 forward NTT at 2^31, real vs NOP-modmul (`t_ntt` vs `t_ntt_nop`, 2 runs each, median of 5).** Whole L31 forward 90.39 / 90.56 ms real vs 76.65 / 76.87 NOP: **modmul 13.7-13.8 ms (15 %)**, structure + HBM 76.7 ms; passes (real - NOP): pass 0 [26..30] 6.5 ms, pass 1 1.9, pass 2 2.3, pass 3 [0..11] 1.6-2.3 (24 ms vs 17 for the others, NOP at 1.4-1.46 TB/s vs 1.8-2.0 for passes 0-2); inverse 92.8 vs 76.5-77.0 (16); L28 10.3 vs 9.1-9.2 ms; L24 0.58 vs 0.555 ms. Production plan at 2^31 is 4 passes; today's build 90.4 ms (K13b earlier 99-100 ms). Split (mod, taking a37v1's HBM floor 51 ms (a)): 51 HBM + 17 extra pass + 9 structure + 14 modmul = 90 ms (a37v1: 144 = 51 + 50 + 43). Headroom: the extra 4th memory pass (about 17-20 ms, 20 %); a 3-pass plan at 2^31 is the only large lever left; WbPowTab-style twiddle seeding gains at most pass 0's 6.5 ms (mod); real - NOP is an upper bound on the modmul share (NOP may change scheduling).

**6. Decisions for the user.** A37-R1 GPU seed: (a) no-go now and measure `ECALC_INIT_TL=1` on the target first (minutes of target time), (b) build it (2-4 days for 0-6 s, mod). A37-R4 fmt18: drop (frees an S item; T1 residues remain the only 2 s). 3-pass NTT at 2^31: (a) investigate (up to about -17...-20 ms of 90 per forward, mod; M-L effort), (b) leave.

## §P1N - one process per GPU (P) at one node: which phase loses (a0c2_p1n1, a0c3_p1n33; main 9ec6d50, aac7 2026-10-25; m)

Question (TASKS, from 9zh7: 1e10, D 36.5 s vs P 52.8 s). Method: 1 node, `mnrun.sh 1`, `ECALC_VERBOSE=2`, arms Do = layout D (ofi build), Po = `ECALC_PE_PER_GPU=1` (same build), Pc = P on the Cray build
(`XSHMEM=cray SHMEM_CRAY=1`, `XSHMEM_LANES=2`); rounds in alternating order (Do Po Pc / Pc Po Do); 3 rounds at 1e10, 2 at 3.3e10; every run VERIFY OK. Means, seconds:

| digits | arm | total | init | bs | dm | (recip in dm) |
|---|---|---|---|---|---|---|
| 1e10 (n 3) | Do | 18.69 | 10.77 | 5.46 | 1.89 | 0.93 |
| 1e10 (n 3) | Po | 19.51 | 5.03 | 6.69 | 7.12 | 2.56 |
| 1e10 (n 3) | Pc | 22.07 | 6.23 | 6.81 | 8.31 | 2.92 |
| 3.3e10 (n 2) | Do | 39.33 | 18.15 | 14.73 | 6.42 | 1.96 |
| 3.3e10 (n 2) | Po | 51.45 | 10.55 | 16.46 | 23.05 | 7.66 |
| 3.3e10 (n 2) | Pc | 57.73 | 12.10 | 17.34 | 26.73 | 8.83 |

**Finding.** Main has moved since 9zh7: at 1e10 P is now +0.8 s (+4 %) behind D (init -5.7 s, bs +1.2, dm +5.2); at 3.3e10 +12.1 s (+31 %): P wins init by 7.6 s and loses bs +1.7 and dm +16.6 s (dm is 3.6 x D's).
Cray P is slower than ofi P by 2.6 s and 6.3 s (dm +1.2 / +3.7 s, init +1.2 s). **Cause (code reading plus the xshmem line of the 1e10 Po log):** under P the node's four PEs are four processes; every product above the leaf
and every sharded reciprocal / division step is a flat 4-PE all-to-all that goes through the NIC engine (loopback over the node's NICs), not the in-process xGMI mesh of layout D (`rns_dist.c`: `vpe_on()` has no
`comm_xgmi`). 1e10 Po log: 537 exchanges, 148 GB received, 10.2 s post-to-completion summed over a PE's thread (14.5 GB/s per PE) in a 19.4 s run. The volume is inherent (it is the 4-way split of the same products), the
engine is the loss: dm (4 sharded products of 2.5e8-5.6e8 limbs) and the top two tree levels (4.1 s of bs 6.7 s). Not a plain bug: no code path differs by mistake (the S2fix batch tier is local, striping is off by design).
**Fix (not small, TASKS):** an intra-node peer path inside the xshmem engine (hipIpc / device-mapped peer copies for same-node PEs instead of the NIC; at multi-node the same-node PEs are also exchange partners, so it would speed 2 n and 3 n too).
Until then D stays the 1 n layout (default). Per-PE DATA heap peak: 256 MiB of 768 at 1 n 1e10 (one PE, m); 2 n / 3 n peaks not measured.
**NIC launch-group probe (`MNRUN_NICGRP=1`, first aac7 use):** all 10 P runs (Po and Pc) report group A on the node's NICs; the D runs, which use no NIC, report none; the probe works, 1 node says nothing about the multi-node group split.

## §PEER - `XSHMEM_PEER=1`: same-node PEs exchange by device copy instead of the NIC (branch peer 78015d4; aac7 2026-10-25 02:10-03:05 ET; m)

**What.** Under P (`ECALC_PE_PER_GPU=1`) the 4 PEs of a node used the NIC loopback (§P1N: 14.5 GB/s per PE). With `XSHMEM_PEER=1` (default 0) the libfabric backend exports its DATA heap with `hipIpcGetMemHandle` (in the heap blob,
all-gathered at `xshmem_heap_alloc`), opens the node-local peers' heaps with `hipIpcOpenMemHandle` (lazy peer access) and `xshmem_ptr(p, pe)` returns the peer's byte at p's offset; `comm_xshmem.c` `lane_put` then issues
`hipMemcpyAsync` into that address on one stream per peer, and `lane_quiet` syncs those streams before any signal (a put that went by copy returns the mask bit `PEERBIT`). Signals and control words stay on the NIC; a PE of another
node, a failed export or open, `XSHMEM_LOOPBACK_PES>1` (layout D: notice, off) or the Cray backend (inert, notice) use the old path unchanged. Code: `xshmem_ofi.c` (`xshmem_ptr`, blob, open/close), `comm_xshmem.c`, `xshmem.h` (`cfg.peer`).
Also new: every PE prints its own `DATA heap peak` line at finalize (PE 0's summary line was the only one).

**Correctness (a1a_peerchk, a1g_peerg; digits identical to the GMP reference).** gate.sh 1e9 under P with `XSHMEM_PEER=1`: 1 n PASS (13.11 GB put by copy, 3 peer heaps mapped), 2 n PASS (5.06 GB); layout D with the switch PASS (notice, nothing mapped). Wave gate on 78015d4 (`gen_wave3.sh`): path 1 D 1 n + 2 n PASS, unit set 9/9 PASS, path 2 (`XSHMEM=cray`, P, lanes 2) 1 n + 2 n PASS (2026-10-25 02:52 ET). Every timing run below is VERIFY OK.

**Timing (seconds, PE 0; same binary, the switch is the only difference Po vs Pp; ABBA-rotated).** Po = P without, Pp = P with the switch, Do = layout D (default).

| size | pairs | Do | Po | Pp | Pp - Po (95 % CI) | Pp - Do (95 % CI) |
|---|---|---|---|---|---|---|
| 1 n 3.3e10 | Po/Pp 3 (a1b), Do/Pp 4+3 | 38.59 (n 3) / 39.71 (n 4) | 51.64 (n 3) | 34.87 (n 3) / 34.18 (n 4) | -16.77 [-21.7, -11.9] (t, df 2) | -5.53 [-6.9, -4.1] (4 pairs, a1h) |
| 1 n 1e10 | Do/Pp 4 (a1h) | 19.20 | - | 13.56 | - | -5.64 [-6.9, -4.4] |
| 2 n 2e10 (8 PEs) | 4 (a1c) | - | 26.56 | 19.40 | -7.16 [-9.9, -4.4] | - |
| 2 n 6e10 (8 PEs) | 2 (a1h) | - | 69.74 | 49.03 | -20.7 (pairs -16.9, -24.5) | - |

Phases at 1 n 3.3e10 (means, a1b): Po dm 23.01 / bs 16.53 / recip 7.64 / init 10.73; Pp dm 9.88 / bs 12.03 / recip 3.15 / init 11.67; Do dm 6.41 / bs 14.93 / recip 1.97 / init 17.13. The engine's exchange line (PE 0, 611 exchanges): 31.97 s post to
completion (13.6 GB/s per PE) without the switch, 14.1 s (30.8 GB/s) with it; 427 GB of the payload went by copy. 2 n 2e10: 17.8 -> 8.5 s (10.5 -> 22.0 GB/s), 97 GB by copy per PE 0 process. Pp is still 3.5 s (dm +3.5) behind D in dm and
wins on init (-6 to -7 s: P has no in-process 4-thread start-up) and bs (-3.2 s at 3.3e10), so at 1 n P+peer is now FASTER than D from 1e10 up (the -5.6 s of both sizes is mostly init, which the switch does not touch: it is the P-vs-D init gap of §P1N);
the rest of the dm gap (3.5 s at 3.3e10) is the signals still on the NIC and the three-peer copy serialisation.

**Per-PE DATA heap peak (second part of the TASKS row).** 256 MiB of 768 on every PE in every run: 1 n 1e10 and 3.3e10 (4 PEs), 2 n 2e10 and 6e10 (8 PEs), with and without the switch. The peak does not move with size or node count
(the exchange rounds bound the staging), so the `RNS_BATCH_SPLIT` largest-node heap model is not exceeded: the heap has 3 x headroom at 2 n. (3 n needs a third NIC node: not measured.)

**Verdict.** Digits identical, no memory cost, no regression at 1-2 nodes; Pp - Po -16.8 s at 1 n 3.3e10 and -7.2 s (2 n 2e10) / -20.7 s (2 n 6e10). Adoption (switch default on for P, or tie it to `ECALC_PE_PER_GPU=1`) is the user's decision. **Not done:** 3 n; signals / control words by direct writes; a copy kernel using several SDMA engines; Path 2 (Cray 12.1 `shmem_ptr` with xpmem) is untested (the switch is inert there).
