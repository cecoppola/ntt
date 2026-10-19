# SINGLE_NODE — single-node speed and options

Scope: single-node speed and options: init, level pairing, remaining options on the defaults, development items, seed terms, combined switches. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `results/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; unabridged text: `archive:results_merged_2026-10-19/`. Numbers are measured (m) unless marked modelled (mod) or assumed (a); times Eastern.

| item | title |
|---|---|
| §P | single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11) |
| §V214 | remaining one-node options on the Phase 14 defaults: auto's per-piece cost, DIST_TWREC, checkpoints with O_DIRECT |
| §V314 | the remaining development items (PLAN §33.2 B1-B7, APUMULT E11), with numbers |
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

## §P — single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11, PLAN §26)

Branch `p11` (from main 72aa2e9). Files: `rns_mul.c` (pools, striped batch path), `rns_mul.h`, init lines of `ecalc.c`, `ntt.c` (context upload); minimal edits in `rns_dist.c` (`dist_r3()` follows the plane switch, `dist_core` takes `xb | sbuf | rbuf` from pool 1 when it holds them, the grid cost model counts a 3.2^k plane at 21/20 per point). All runs aac6, 4 x 10^10, reference evicted before each timed run, digits `cmp`'d.

| item | switch (default) | measured at 4 x 10^10 | adopted |
|---|---|---|---|
| B3: 3.2^k planes sized at init (pool 0 = 3.2^30 limbs = 24 GiB, pool 1 = 3q + 16 = 18 GiB per APU) | `RNS_PLANES_3Q30=0/1/auto` (**0**) | phases -3.6 s (top levels 13.2 -> 10.4, reciprocal -0.6, level 21 -0.2; A-grid's 15 s first-use cost gone), but 60 GB more pools cost **+4.7 / +6.3 s at init** (mapping 0.08-0.12 s/GB while the seeds stream): wall +1.0 / +2.6 s | no (code kept; `auto` = on below 5 x 10^10) |
| A2: level-22 products (8 at 2^30 points, 4 pairs) paired in the striped batch path on 3.2^28 planes | `RNS_STRIPED_PAIR=0/1` (**1**) | level 22 2.16 -> 1.55-1.63 s (**-0.55 s**; scatter 0.350 -> 0.293, ntt 1.475 -> 1.022, crt 0.367 -> 0.342); at 7 x 10^10 the level of 14 products 2.7 s | yes |
| §23-3: `ECALC_DM_POOL` on from 5 x 10^10 | `ECALC_DM_POOL=0/1` (**on at d >= 5 x 10^10**) | 7 x 10^10: **153.5 s** VERIFY OK (init 18.4, bs 67.3, dm 67.7: recip 32.3, division 35.4); RESULTS §75 159.7 s default, 157.2 s with the pool; 4 x 10^10 unchanged | yes |
| I11 init: context tables and pass twiddles in one `hipMalloc` + `hipMemcpy` each; engine-2 context only for engine 2 | - | contexts 0.3-0.8 s of 1.0 s staging; init = regions 4.8 s + pools 12.4-15.7 s with the seed thread (15 s) alongside; nothing left to share across APUs (tables 8 KB per context) | yes (bit-identical, `t_ntt` 565 checks) |
| A4 tile budget in pair mode | `RNS_BATCH_TILE_GB` (**15**) | 25.8 GB: batch tier 22.6 -> 22.5 s, transforms -0.9 s of 15.2 s, wall same (83.0 vs 81.7: init noise) | no (knob kept) |

**B3 pairs (same node, same allocation, A2 on in both).**

| 4 x 10^10 | s24-30 off / on | s24-26 off / on |
|---|---|---|
| init (pools mapped) | 13.9 (12.4) / 20.2 (18.6) | 15.6 (14.0) / 20.3 (18.6) |
| bs | 37.4 / 34.6 | 35.9 / 32.8 |
| dm | 31.9 / 31.0 | 30.1 / 29.6 |
| phases | 69.3 / 65.6 | 66.0 / 62.4 |
| **wall** | **83.3** / 85.9 | **81.7** / 82.7 |

Memory with planes on: init 276.7 GB (planes 180.4, regions 95.7) vs 216.6; recip peak 322 vs 262 GB. The extra 14 GiB per APU is needed exactly at the arena donation and reciprocal peaks, so no idle memory covers it. What would make B3 pay: cheaper mapping (0.047 s/GB alone), seeds off the init path, or M's tail layout for t1 (removes 35.6 GB of in-reciprocal mapping: then +2 s init against -3.6 s phases, net about -1.5 s, a). `RNS_PLANES_3Q30=1` gives the fastest phase time measured (62.4 s). Related: grid steps (level 25 stays 3 x 1 pieces of 3.2^28, level 24 becomes one 3.2^30 plane 1.85 s, division products become 1 x 3 pieces of 3.2^30).

**I11 init.** At 4 x 10^10: staging + contexts 1.0-1.1 s, regions 4.8-5.3 s (95.7 GB), pools 12.4-15.7 s (120 GB under the seed stream), peer access 0.3 s; init on the final build 15.6-17.3 s (RESULTS §75: 16.4 +- 1.1). Not done: sharing tables across APUs (xGMI reads would cost more than the one-time upload) and pool clears by kernels (none left; `hipMemset` of a fresh 22 GB is 0.01 s, the mapping is in `hipMalloc`).

**Tests.** `t_mul 20` 189 checks, `t_mul 0 batch` 72, `t_ntt 24` 565 VERIFY OK; 10^9 identical in every build; 10^8 sizes 2, 4 identical; 4 x 10^10 identical in 9 runs; 7 x 10^10 VERIFY OK.

**Gate (job 20828, five runs of 4 x 10^10, final build: planes off, A2 on, tile 15 GB): wall 81.99, 82.18, 82.74, 79.13, 81.26 s = 81.5 +- 1.4 s, phases 66.0 +- 0.4 s** (RESULTS §75: 83.0 +- 1.2, phases 66.7 +- 0.3), init 15.5 +- 1.1; all identical. The target of <= 79 s is not met: A2 is the only adopted phase gain; the wall spread is the init mapping (13.6-16.4 s for the same bytes).

**Open.** B3's bytes vs the mapping rate under the seed stream; the transform cache at size 1 (G, not adopted) could share B3's planes; `t_dbig 24 big` with planes cannot finish on a node (host reference product); the striped pair mode is taken only by level 22 at 4 x 10^10.

## §V214 — remaining one-node options on the Phase 14 defaults (Phase 14, agent V2)

Branch `p14-V2` from `p14-defaults` 4855842 (all Phase 14 defaults on); aac6.

**1. C2 — `RNS_AUTO_PIECE_COST=1` (off by default at the time; built in since 2026-10-12).** `rns_dist.c split_grid`: a grid of more than one piece also pays, per piece, the pipeline law's per-piece terms (results/TIME_MODEL.md §D213d): +0.087 x B points + 0.092 x nc in split_grid's units (mod); the one-plane choice is in the same loop. The mn tier and the C strategy are unchanged; digits do not depend on the grid.
- *`MN_PLAN_ONLY` at size 1 (mod):* total pieces 4e10 84 -> 76, 1e11 156 -> 127 (div A_h mu / X Q 7 x 7 B 3.2^29 -> 5 x 6 C 2^31), 1.16e11 206 -> 152, 1.3e11 204 -> 163; unchanged at 1e10 and 7.64e10.
- *Measured (s24-16, job 21443, same node back to back, sha1 by `dd iflag=direct`):* **1e11 off 234.55 s (bs 96.7, dm 99.9, recip 41.7, T1 20.9, corrections 0/2) vs on 206.02 s (bs 95.7, dm 94.6, recip 38.8, T1 0.0, 0/0)**, sha1 578f5efb... identical; 4e10 on 64.22 s identical (default history 63.5 +- 1.5 s). Above 1e11 (s24-26, job 21447; pair sha1s equal, VERIFY OK, no reference): **1.16e11 287.74 -> 258.80 s (-10.1 %; dm 149.3 -> 130.0)**; **1.3e11 325.17 -> 304.25 s (-6.4 %; dm 163.7 -> 147.2)**; dc 3-7 s longer with the switch (after `total`).
- *Verdict C2: adopt* (one node, >= 4 x 10^10: -12.2 % / -10.1 % / -6.4 % of `total` at 1e11 / 1.16e11 / 1.3e11). The mn tier (576 target) is untouched.
- *Where the 28.5 s at 1e11 come from:* dm -5.3 s and T1 -20.9 s. The T1 part is the correction count: the default 7 x 7 grid skips 21 pieces below the low cut and took 2 up-corrections; the writer starts at the hook, so a correction makes T1 wait 20.9 s and then redo the digits and rewrite the file (dc 57.0 vs 22.5 s; srun wall 294 vs 231 s). The 5 x 6 grid skips 10 pieces: 0 corrections; data-dependent, not guaranteed. **Finding:** the defaults pay about 21 s of `total` and 35 s more of dc for two corrections; a correction test before the hook (or fewer low-cut skips) would remove it.

**2. DIST_TWREC=1 at 1e11 — not measured** (stopped by the integrator). Modelled: on the defaults it only reaches the C-form products (14, 11.97 s of 234.5 s), saving well under 1 % of `total`; with `RNS_AUTO_PIECE_COST=1` the C-form share is 90 products / 75.8 s, so if TWREC is ever adopted it is together with C2. Verdict: no evidence to adopt; open.

**3. Checkpoints with O_DIRECT — not measured here** (batch never started; later measured in `results/OUTPUT_IO.md §IO15` 2.4 and §S114).

**Other findings.** `mnaccept unit,e9`: 9 passed, 1 failed (`t_bs` segfault rc 139): **`t_bs` segfaults on the base 4855842 too and passes with `DB_POOL_VMM=0`**: the Phase 14 default `DB_POOL_VMM=1` (t_bs writes pool memory from the host), not this branch. 1e11 runs first stopped at the budget check (rc 8, 513 GB counted), fixed by the integrator's 2085d1e. The e_1e11 reference sha1 (578f5efb...) took 943 s to read from the shared FS and is kept. Open: items 2 and 3, t_bs vs the VMM default, the correction-after-hook cost.

## §V314 — the remaining development items (PLAN §33.2 B1-B7, APUMULT E11), with numbers (Phase 14, agent V3)

Branch `p14-V3` from `p14-defaults` 4855842. **No pipeline code changed**; prototypes and models only (`tests/v3_*`), logs in `results/V314/`.

**0. Answer in one table.** Reference: **1e11 on one node, current defaults, 215.9 / 217.1 s** (s24-30, job 21436: init 17.2 / 18.5, bs 96.3 / 96.6, dm 101.6 / 101.2, recip 43.0 / 42.7). 576 nodes at 4.25 x 10^13: 244.9 s (mod, fabric assumed).

| rank | item | 1e11 one node | 576 target | effort / risk |
|---|---|---|---|---|
| 1 | B1 / TASKS 2.2: decimal `mul_1` + fused span (`bigint.c mul1_serial`, `binsplit.c span`) | **-9 s (-4 %)** to the mapping floor (mod from measured parts) | -9...-11 s (mod; the model lacks this term) | 1/2-1 d, low (prototype 0 mismatches) |
| 2 | `BS_SEED_TERMS` chosen per run (rule in `seed_limbs`) | **-6.6 s (-3 %)** m, one run (S = 168); 4e10: **-5.5 s (-8 %)** (S = 236) | -3...-7 s (a) | 1/2 d + series, low; **became §T215** |
| 3 | B5: radix-3 pass fused into the first 2^k pass (`ntt3.c`, `ntt.c`) + `pick_len` preferring 2^(k+2) at k <= 12 | -6...-8 s (mod from measured pass share) | -3...-4 s (leaf only) | 2-3 d, medium |
| 4 | B2: reciprocal Q_t.r as a middle product (`recip_db2 / recip_mn`: lowcut and highcut) | -3.4 s (mod) | -1.0 s (mod) | 1 d, low |
| 5 | B5: general-map twiddle packs (`k_twpack_g`) | 0 | -2...-3 s (a) | 1-2 d, medium, multi-node only |
| 6 | E11: band-sized t1 + chunked division window | 0 s; one-node ceiling 1.41 -> 1.52 x 10^11 at 480 GB (mod) | 0 s; 424 -> 407 GB per node, 576 ceiling 9.2 -> 9.8 x 10^10 per node (mod) | 3-4 d, medium; memory only |
| 7 | B5: the 2^31 plan's 5-stage top pass | <= -1 s (mod) | <= -0.3 s | 1-2 d |
| 8 | B4: lengths 5.2^k, 7.2^k | -3 s (a; item 2 removes the same padding free) | -0.3...-0.8 s (mod); does not soften the target's steps | **>= 2 weeks (new prime set: no current prime has 7 | p-1; only p0 has 5)**, high; not recommended |
| - | seeds before planes (2.1), seeds on GPU (6.4) | ~0 after item 1 | ~0 | mapping floor binds |
| - | B2 piece loading (2.3) | closed: recip 45.6-45.9 -> 43.0 / 42.7 s m | - | done |
| - | B6 tier balance, B7 CPX / SDMA | <= 0.3 s / ~0 | 0 | not recommended |

Recommended order 1 -> 2 (five-run series) -> 3 -> 4 -> 5; items 1-4 together about -25 s at 1e11 (-12 %, not exactly additive), -15...-20 s at the target (-6...-8 %).

**1. B1 — initialization and the seeds.** The seed thread (`binsplit_seeds_begin`) computes level-0 spans (256 terms, Horner) on all cores and, under `DB_POOL_VMM`, stages them through two 8 GB pinned buffers into the region arenas; since `RNS_PLANES_FIRST=1` it starts after the plane pools. **At 1e11 on the defaults (2 runs): init 17.2 / 18.5 s; the seed thread 30.9 / 31.1 s (spans 22.0 / 21.9 s); bs waits for the seeds 19.7 / 20.5 s after init, 9 % of the wall** (older defaults 13.1-13.8 s at 1.03e11). **Prototype `v3_span_bench` (96 threads, 400 000 spans, 0 mismatches):** pipeline `span()` 3.36 / 4.38 / 5.68 ns per term at k = 4e9 / 1e10 / 3.5e12; fused P += Q; Q *= k with a double-estimate quotient: **x1.69 / x2.11 / x2.19**; widened Barrett x1.10 / x1.65 / x1.45. `mul1_serial` takes its Barrett step only for m < 2^33, and at the target every node but the first two has k > 2^33, so one-node runs understate the target's seeds by about 29 %. Benefit: the seeds cannot end before the second half of the arena is mapped (floor about 28 s, mod); the fused spans take about 12.6 s so the seeds end at about 27.5 s: -9 s at 1e11; at the target's top node -9...-11 s (mod). The model's init/bs laws are fitted on k < 2^33 runs, so item 1 mostly secures the modelled 245 s.

**2. The seed span as a padding knob (`BS_SEED_TERMS`).** The batch tier's first products have nc = 2 x span limbs and take the next 2^k (3.2^k from 3.2^10): at 1e11, 286 limbs -> 512 points (1.79x), then 1.34x from level 4. One run each (VERIFY OK):

| run | total | init | bs (seeds wait / batch / mdev) | dm | batch ntt |
|---|---|---|---|---|---|
| 1e11 S = 256 (default), 2 runs | 215.9 / 217.1 | 17.2 / 18.5 | 96.3 / 96.6 (19.7-20.5 / 37.3 / 38.7-39.0) | 101.6 / 101.2 | 25.3 |
| 1e11 **S = 168** | **209.3** | 16.8 | **86.7** (14.8 / 37.2 / 34.5) | 105.0 | 21.2 |
| 4e10 S = 256 | 66.7 | 11.1 | 32.0 (8.2 / 15.7 / 8.1) | 23.5 | 10.6 |
| 4e10 **S = 236** | **61.1** | 11.6 | **26.3** (8.3 / 13.1 / 4.8) | 23.1 | 7.5 |
| 4e10 S = 176 | 62.2 | 12.8 | 26.2 (6.3 / 15.7 / 4.1) | 23.1 | 8.8 |

Gains come from fewer seed CPU (proportional to S), less batch padding and a different batch/mdev split (mdev 8.1 -> 4.1-4.8 s at 4e10). dm at 1e11 S = 168 is +3.4 s (one run; shown real in §T215).

**3. B5 kernels.** Radix-3 pass (APU0, 3.2^29 points per call, median of 5): of a 3.2^k forward transform the r3 pass is 49.8 % at k = 10, 39.3 % (12), 32.7 % (17), 23.8 % (22), 19.9 % (28), 19.8 % (29); inverse 45.3 -> 18.6 %. The pass moves 25.8 GB in about 16.3 ms (1.58 TB/s): memory-bound, so a reduced-correction modmul gains about 0; fusing it into the first 2^k pass removes most of it. At k = 10 a 3.2^k transform costs **1.23x a 2^(k+2) transform** of the same product (1.06x at k = 12; wins from k about 17), so `pick_len` should take 2^(k+2) for k <= 12 (bit-identical). Benefit (mod): batch ntt 25.3 s at 1e11, about 30 % of it the r3 pass -> -4.5 s at 70 % recovery, plus -3 s in the dm / mdev B pieces: **-6...-8 s at 1e11, -3...-4 s at 576**. The 2^31 plan's top pass (K13b 26.1 ms -> 21.5 ms: -4 ms per 2^31 transform): <= 1 s at 1e11. s_lo 17/24 penalty remains at 2^24 only (<= 0.2 s). General-map twiddle packs (576 = 2^6.9 levels, about 94 s of the modelled wall): -2...-3 s at 576 (a), 0 at one node.

**4. B2 — the reciprocal.** u = t1 >> (take - j) is fixed by its low j + 2 limbs (|u - B^2j| < B^(j+1)), so Q_t.r is needed only on [v, v + j + 2): mul_grid's `lowcut` = v - 2 and `highcut` = v + j + 4 (both exist); one-plane products become a cyclic convolution of length >= 2j + 3 instead of 3j + 3. Modelled: at 1e11 the top doubling's 9 x 3 grid forms 20 instead of 24 pieces; reciprocal 52.8 -> 48.6 s (mod), about **-3.4 s** at the measured scale (-1.6 % of the wall); 7.64e10 -1.6 s, 1.3e11 -5.6 s; 576: -1.0 s (-0.4 %). TASKS 6.3's -25...-35 % does not hold (the cut already took the easy part; r.d cannot shrink).

**5. B4 — lengths 5.2^k / 7.2^k.** The primes are c.2^44 + 1 with c = 240, 216, 207 (`modarith.h`): only p0 has 5 | p-1, none has 7; every prime of a product must support the length, so a **new three-prime set** with 105 | c and CRT margin (today 6.8 at 2^33 points) is needed: >= 2 weeks, high risk. Benefit (mod): target 244.9 -> 244.1 s (the target's grid steps are counts of cap-sized pieces and more lengths do not soften them); one node about -3 s at 1e11, which item 2 gets without code. Not recommended.

**6. B6 / B7.** B6: the alternative that gives APU 3 residues was 3-7 % slower (B13b), striped batch idle <= 0.3 s at 4e10 (P3), B at size > 1 stays C: about 0. B7: CPX makes each APU six devices (against the dist tier) and needs an administrator; SDMA: the division's X fetch is 0.87 s off the critical path: about 0.

**7. E11 — band-sized reciprocal products and chunked window (mod).** `dm_layout` reserves t1 at Q_t r's full 3j size and the window at nq + k limbs. One-node ceiling 480 / 502 GB: as built 1.41 / 1.50 x 10^11; band-sized t1 1.46 / 1.56; chunked window alone no gain (top bs levels bind); **both 1.52 / 1.64 x 10^11**; node at 1e11 378 -> 364 GB; at 576 424 -> **407 GB**, ceiling 9.2 -> 9.8 x 10^10 per node. Not the 2.1-2.3 x 10^11 of APUMULT_STUDY (that assumed P and Q spilled, rejected in L114; 2e11 needs 590 GB without them). Time effect: item 4's, 0 for the window.

**8. Where the target's wall goes (mod, 576 nodes, 4.25 x 10^13, fabric assumed):** 244.9 s = init 23.6 + batch 28.9 + top 24.0 + distributed levels 86.6 (29.3 exposed) + reciprocal 30.2 (12.2 exposed) + division 29.1 (12.1 exposed) + part file 22.3; per-node compute about 169 s (69 %). Model lacks `NEWTON_RECIP_CUT` at 576 (about -1.9 s) and the seed term of §1 (about +10 s): net about 8 s optimistic (derived).

**9. For the integrator.** The budget check stops 1e11 on one node on the defaults (`rc 8`, peak 513.2 GB: regions 259.9 + block pool 124.6 (borrowed) + planes 103.1; measured runs with `ECALC_BUDGET_CHECK=0` completed with host HWM 27 GB, MemAvailable >= 152 GB; the borrowed pool appears counted on top of its regions). The target's part file (22.3 s exposed at an assumed 2 GB/s) is larger than any B item: measure the target SSD early. **Tests:** node batch job 21436 (s24-30): bench.log, e11_def, e11_def2, e11_s168, e4_def, e4_s236, e4_s176, all VERIFY OK (S runs digit-exact by construction, not compared byte for byte).

## §T215 — `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths (Phase 15, row 1c)

Branch `p15-T2` from int15 0b80ca3. **Adopted: `BS_SEED_FILL=128` is the default (user decision 2026-09-27).**

**1. What decides the fill.** `binsplit.c` combines spans of S terms pairwise; a batch level uses one length for all products, `pick_len(maxnc)` = 2^k, or 3.2^(k-2) when k - 2 >= 10 and maxnc <= 3.2^(k-2); with m1 = the last span's limbs, maxnc(l) <= 2^l.m1 and m1 about S.log10(k_max)/18. If m1 <= 2^j every product from 2^10 up fills a 2^k length (fill >= 0.99, mod) and the batch tier's top level ends exactly at the mdev threshold, so **one more level runs in the batch tier** (3-4x cheaper per output limb than mdev: V3's S = 168 level 23 in batch 2.85 s vs about 10 s in mdev). Otherwise padding is 1.34-1.5x: **S = 256 at 1e11 has m1 = 143: fill 0.74 from level 4 on; at 4e10 m1 = 138: 0.71** (mod; the model's `max_nl` matches the log's at all 26 levels). Cost terms fitted to V3's logs (mod): batch 3.39e-10 s per per-APU point + 1.15e-7 s per span (+-0.5 s); mdev 1.23e-18 s per limb^2 + 0.89 s per level (+-3 s). **Rule: S = the largest S whose last span has <= 128 limbs**: S = 239 at 4e10, **229 at 1e11**, 235 on the 576-node plan's node 0, about 184 on its last node (k about 3.5e12); no single `BS_SEED_TERMS` fits every size or node.

**2. The switch.** `BS_SEED_FILL=<limbs>` (`binsplit.c bs_seed_terms_for(bend)`, lgammal; overrides `BS_SEED_TERMS`; 0 = off, byte-identical by construction: the span changes only the tree's shape, P and Q are unique); one line each in `seed_limbs` and `binsplit_e`; `mn_plan.c plan_leaf` (one line) so `MN_PLAN_ONLY` plans the same tree (576-node node 0: S 256 -> 235, mdev levels 3 -> 2, leaf dist_db pieces 16 -> 12, mod). `tests/t2_sweep.sh` and `tests/t2_seed_model.py` are the tools.

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

Branch `cand` (off main a448d73): squashed from `o2` (`DBIG_SMALL_BOUNCE`, results/DIVISION.md §O2), `k1b` (`DIST_GATHER_RAW`, results/EXCHANGE.md §K1b), `k8b` at 99eee11 (`BS_SEED_PULL`, results/MEMORY.md §K8b); no textual conflicts; switch registry regenerated (269 rows, 0 problems); all switches off by default. Arms: A = main binary (85d9d73f), switches unset; B = cand binary (7e7cd7da) with `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1`; 6.441e10 digits per node, aac7 line, no digit writes; paired AB/BA; every run VERIFY OK (aac7 24/24, aac6 8/8); gates PASS (aac7 2 nodes all on, 1e9 identical; aac6 1 node).

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

## §B08 — how fast can the unit tests be? (2026-10-11, branch b08)

**1. Tasks per step.** On aac7 (1-node hold 12545) and aac6 s24-16 (hold 22908) `srun -N1 --mem=0 --gpus=4 [-n1] --overlap bash -lc ...` starts **1 task** with and without `-n1`; B05 §9's "4 copies" was not reproduced (likely B05's hold had a larger task count; a 2-node hold on aac7 not tested). Kept as a harmless guard: `R()` and `N()` use `-n1`.

**2. `t_ntt 24`, one task.** aac7 `-c 192` OMP 192: 285.9 s (section 3 convolution 266 s); `-c` unset: 276.2 s; aac6 s24-16: timeout at 420 s stuck in section 3 (with or without `-c 48`). One `t_ntt` thread at 80-95 % CPU, all four GPUs at 0 %: **a single-thread host wait, `-c` / `OMP_NUM_THREADS` do not matter.** Cause (`tests/t_ntt.c` section 3b): `for (i = n; i-- > 0;) { mpz_mul_2exp(C, C, 16); mpz_add_ui(C, C, hz[i]); }` is O(n^2) (n up to 2^20). **Fix:** carry-propagate into 16-bit limbs and one `mpz_import` (O(n)); same integer, same checks.

**3. After the fix (aac6 s24-16, `-n1`):** 8.6 s (`-c` unset) / 8.3 s (`-c 48`, OMP 48), VERIFY OK (3215 checks); before > 420 s (aac6), 276-286 s (aac7): about 30x on aac7 (mod), > 49x on aac6.

**4. Whole suite `mnaccept.sh --skip-grid --only unit,e9` (aac7 1 node, before the fix):** 755 s, 8 passed, 2 failed: `t_bs` (`ref/e_1000000.sha256` missing, `ECALC_REF` not set) and `t_ntt 24` (rc 137 at 137 s into section 3, a SIGKILL not sent by this run, cause unknown, doubt); a partial figure. Modelled with the fix: about 630 s (not run). e9 base 10 13.9 s, base 2 29.0 s, identical.

**5. Recommendation.** Adopt the `t_ntt.c` section-3b fix (saves about 4.5 min of every unit run on aac7, removes the > 7 min hang on aac6); leave `MNRUN_CPUS_PER_TASK` as is (no difference unset / 48 / 192); keep `-n1` in `R()` / `N()`; set `ECALC_REF` in unit runs so `t_bs` passes; one final full-suite timing after merge (about 630 s, mod). Open: the `t_ntt` rc 137 cause; 2-node hold task count on aac7 untested.
