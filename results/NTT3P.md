# NTT3P: can ecalc's large transforms run in 3 memory passes instead of 4? (design study, no code changed)

Tasked by the main session. Read-only on code. Labels: (m) measured, (mod) modelled, (a) assumed. Sources: results/S21.md §5 (A37-Q4), K13.md, K13b.md,
RESULTS.md §33 (D5) and §48 (lds/occupancy), N3x15.md, V314.md, internal code notes, `ecalc/ntt.c` (make_plan, plan_auto, k_b16, k_b16r, k_b1r),
`ecalc/mn_model.py` / `estimate.py --fabric target-m --bw 47` and `--fabric aac7_s18 --bw 6` (run on the login node, pure python).

## 0. Summary

- **Best split at 2^31: 13 / 9 / 9** (bottom contiguous b1 of 2^13 points, two strided 9-stage passes, all with 16-column, 128 B segments), or **12 / 10 / 9**
  (keeps today's 2^12 b1 unchanged; one 10-stage pass with 64 B segments). Both fit in the 64 KB LDS with no registers staging.
- **Modelled saving per 2^31 forward: about 15 ms of 90.4 (17 %), range 5...24 ms.** The floor of any 3-pass plan is about 66 ms (3 x 17.3 ms NOP pass + 14 ms modmul), so -24 ms is the ceiling.
- **Per run (mod): 10 nodes about 1-2 s of 435 s (0.3 %); 576 nodes about 4 s (range 1.5-7) of 219-266 s (1.5-2 %).** The 576 gain is mostly NOT the 2^31 transform: it is the
  4-step pieces' row/column transforms of length 2^20...2^22, which run 3 passes today and would run 2.
- Effort M-L (2-3 weeks incl. validation), correctness risk low (canonical outputs are unique, t_ntt hashes), performance risk medium (1 block/CU, 64 B segments).
- No prior attempt at more than 7 strided stages or a 2^13 b1 exists. Recommendation: **a 2-3 day microbench first (E0/E1, section 6); build only if it shows the fat pass within 10 % of the model.**

## 1. Constraints (from the code)

- `k_b16`/`k_b16r`: block = 2^7 rows x 16 adjacent columns = 2048 points (128 B contiguous per row), 256 threads, `sh[128][17]` = 17.4 KB (+ 1 KB twiddles) = 18.4 KB, **3 blocks/CU, LDS-limited**;
  VGPRs 46-66 (m, K13). `NTT_B16_STG` is clamped to 3..7 (`ntt.c:788`), so **no strided pass is longer than 7 stages today**.
- `k_b1r` (contiguous b1): LGL 10...12 points per block, 2^(LGL-LGV) threads, LGV 3 or 4 points per thread in registers, one LDS exchange between groups, XOR-swizzled (`b1r_swz` covers index bits up to 11 only),
  LDS 2^LGL x 8 B (8/16/32 KB), VGPR 18-22 (m, K13 for the old k_b1). 2^12 is already in production at 2^31 (plan 121: stages [0..11], 512 threads, 32 KB, 2 blocks/CU).
- Hardware: LDS 64 KB per workgroup (exactly 8192 points, no room for a padded tile or a twiddle table in LDS), 512 KB VGPR per CU (8 B point = 2 VGPR: 64 K points per CU if all registers held data, in practice about 16 K), HBM 2.0 TB/s in the NOP passes (1.8-2.0), copy kernels 2.2-2.4 TB/s (m).
- All pass logic is private to `ntt.c` (`make_plan`, `plan_auto`, `launch_b16`, `launch_b1`); `ntt_npass` / `ntt_pass_bounds` are used only by `tests/t_ntt.c`. `NTT_MAXPASS` = 8.
  The distributed 4-step packs (`k_twpack_g` etc.) are separate kernels, not part of the passes.

## 2. Today's plans and the pass counts (computed from `make_plan`/`plan_auto`)

| logn | stage split now (b16 passes, then b1 bits) | passes now | passes with 13/9/9 |
|---|---|---|---|
| 17-19 | 7, b1 10/11/12 | 2 | 2 |
| **20, 21, 22** | 7+3 / 7+4 / 7+5, b1 10 | **3** | **2** (13+7, 13+8, 13+9) |
| 23 | 7+6, b1 10 | 3 | 3 (13+10 needs a 10-stage pass: 2 passes only with C=8) |
| 24, 25, 26 | 7+7, b1 10 / 11 / 12 | 3 | 3 (or 2 only with a 10-stage 2nd pass, not planned) |
| **27, 28, 29, 30** | 7+7+(3..6), b1 10 | **4** | **3** (13+7+7 ... 13+9+8) |
| **31** (plan 121) | 5+7+7, b1 12 | **4** | **3** (13+9+9) |
| **32, 33** | 7+7+7, b1 11 / 12 | **4** | **3** (13+10+9 with C=8, or 12+10+10); 33 only with C=8 or 4 |

13 + 9 = 22 stages, so 2^23 stays at 3 passes. Two-pass coverage is logn <= 22, three-pass coverage is logn <= 31 (<= 33 with 64 B / 32 B segments).
3·2^k lengths add the separate radix-3 pass (`NTT_R3_FUSE` off by default, PH13), so they go from 5 to 4 passes at k = 27...31 and from 4 to 3 at k = 20...22.

## 3. Enumerated splits for logn 31 (forward, strided passes listed top first, b1 last)

LDS tile = rows x columns x 8 B, unpadded with XOR swizzle (the 64 KB case cannot be padded). "Seg" = contiguous bytes per row read from HBM.
s_lo = lowest stage of the pass = log2 of the row stride in points (K13b slow strides: s_lo 17 and 24 only).

| # | split (stages) | block points / LDS | threads (a) | seg | s_lo of each pass | blocks/CU (LDS) | status |
|---|---|---|---|---|---|---|---|
| 0 | 5 / 7 / 7 / 12 (today, plan 121) | 2048 (18.4 KB) x3 ; 4096 (32 KB) | 256 ; 512 | 128 B ; contiguous | 26, 19, 12, 0 | 3 ; 2 | measured: 90.4 ms |
| 1 | **9 / 9 / 13** | 8192 (64 KB) x3 | 512 (16 pts/thread, 3 groups of 3 stages for the 9s ; LGV 4, 4 groups for the 13) | 128 B ; contiguous | 22, 13, 0 | 1 | recommended |
| 2 | **9 / 10 / 12** | 8192 (64 KB) ; 8192 (64 KB, 1024 rows x 8) ; 4096 (32 KB, existing) | 512 ; 512 ; 512 | 128 B ; **64 B** ; contiguous | 22, 12, 0 | 1 ; 1 ; 2 | recommended alternative (b1 untouched) |
| 3 | 10 / 10 / 11 | 8192 (1024 rows x 8) x2 ; 2048 (16 KB, existing LGL 11) | | 64 B ; 64 B ; contiguous | 21, 11, 0 | 1 ; 1 ; 4 | feasible, more segment risk |
| 4 | 11 / 10 / 10 | 2048 rows x 4 cols = 8192 (32 B segs) | | **32 B** | | 1 | not recommended: half-sector reads |
| 5 | 11 / 10 / 10 with 16-column tiles (128 KB tile: registers + LDS) | 16384 pts, 1024 threads x 16 pts, two-phase LDS exchange | | 128 B | | 1 (and the whole VGPR file) | L effort, risk high; only if 1-4 are blocked by segment width |
| 6 | 8 / 8 / 8 / 7 (4 passes) | | | | | | no gain, listed to show the pass count is the cost |

Why 13/9/9 over 12/10/9: all strided passes keep 128 B segments (the width K13b measured, 2.1-2.8 TB/s in the stride sweep outside s_lo 17/24); the only new
unknown is the 1 block/CU latency hiding. 12/10/9 reuses the measured 2^12 b1 (25.8 ms real) but adds the 64 B-segment unknown. Both avoid s_lo 17 and 24.

## 4. Per-pass cost model at 2^31 (forward, ms; 34.4 GB moved per pass)

Calibration from S21 (m): NOP passes 17.3 / 19.0 / 17.1 (HBM-bound, 1.8-2.0 TB/s), b1-12 NOP 24.2 (1.4 TB/s, compute/exchange-bound), real 24.0 / 20.8 / 19.4 / 25.8.
Modmul exposure: about 0.3 ms per stage in the 7-stage passes (2.3 / 7), b1: 1.6-2.3 / 12; pass 0 is the outlier (6.5 ms, twiddle seeding, 5 stages).
b1-12 costs 25.8 / 12 = 2.15 ms per stage (all-in): that is what a pass costs once it is no longer HBM-bound. Model: pass = max(HBM floor 17.3 + 0.3 x stages, 2.15 x stages) x (1 + occupancy penalty).

| pass | stages | optimistic | central (+10 % for 1 block/CU) | pessimistic (+25 %) |
|---|---|---|---|---|
| strided 9, 128 B | 9 | 20.0 | 22.5 | 25 |
| strided 10, 64 B | 10 | 21.5 | 24.5 | 29 (plus an unknown segment penalty) |
| b1 13 (new) | 13 | 26.5 | 30 | 33 |
| b1 12 (today) | 12 | 25.8 (m) | 25.8 (m) | 25.8 (m) |

| plan | optimistic | central | pessimistic | saving vs 90.4 (opt / central / pess) |
|---|---|---|---|---|
| 0 today | 90.4 (m) | 90.4 (m) | 90.4 (m) | |
| 1 = 9/9/13 | 66.5 | 75.0 | 83 | **24 / 15 / 7** |
| 2 = 9/10/12 | 67.3 | 72.8 | 79.8 | **23 / 18 / 11** |
| 3 = 10/10/11 | 66 | 74 | 85 | 24 / 16 / 5 |

Reading: once the passes are not HBM-bound the total is about 31 stages x 2.15 = 67 ms whatever the split, so the split should be chosen for risk, not for the last millisecond.
The central saving is **15-18 ms (17-20 %)**, at the low end of S21's "17-20 ms (mod)" estimate; the pessimistic case is 5-11 ms. A reported central value of 15 ms is used below.
The memory-only floor (3 x 34.4 GB at 2.2-2.4 TB/s) is 43-47 ms, so even the optimistic row is compute/structure-bound, not memory-bound; this is the same finding as K13 H2 ("issue/latency-bound at 1.2-1.6 TB/s").

Other lengths (per 2^31 points, mod): logn 20-22, 3 -> 2 passes: today [b1-10 24 + 7-stage 19.4 + 3..5-stage 17] = 60; new [b1-13 30 + 7..9-stage 20-22] = 50-52: **about -9 ms-equivalent (15 %), -22 % at best**. logn 27-30, 4 -> 3 passes: about -15 ms-equivalent.

## 5. Prior attempts and why they do not decide this

| item | where | what it found | relevance |
|---|---|---|---|
| tile 256 rows, b1 2^11 / 2^12, "occupancy costs more than the extra in-LDS stages save" | RESULTS §33 (D5), 2026-09-14 | with the OLD kernels (one LDS round trip per stage); 2^11 at 24 KB (2 blocks) 126-129 ms, 2^12 at 48 KB (1 block) 153 ms vs 117 for 2^10 | **superseded for b1** by the register-blocked b1r (K13b): 2^12 is now the best b1 in production (plan 121). Not re-tested at 2^13 or for strided 8-10 stage tiles |
| LDS bandwidth by occupancy | RESULTS §48 | 35.8 TB/s at 3 blocks/CU, 19.0-19.3 at 1 block/CU (34-64 KB tiles) | the 1 block/CU price: LDS traffic of a 9-stage body with 2 exchanges is 32 B per point = 69 GB per 2^31 transform = 3.6 ms at 19 TB/s (mod), not binding |
| occupancy H6, `NTT_B16_VAR` | K13 §H6, PH6 | 3 -> 4 blocks/CU is within +-2 % | occupancy is not the limiter for 7-stage passes (HBM / issue-bound) |
| MALL-chunked passes `NTT_MALL` | K13 H2, PH6 | no gain, "no MALL cliff, kernels are issue/latency-bound" | rules out cache-resident multi-pass as an alternative to a fatter pass |
| 3-pass plan | K13b §2 | "with a b1 of 2^10, 2^31 has 21 stages above b1 = 3 x 7, so no 3-pass plan avoids s_lo 17 and 24"; with b1 2^12 and bottom-up: 5/7/7 + b1 = 4 passes, s_lo 26/19/12 | the 3x7 limit came from the 7-stage clamp, not from a measured failure |
| 2-pass is the whole transform for logn <= 19 | `plan_auto` | b1 2^11/2^12 saves a pass at logn 25, 26 (1.25-1.40x) | **same lever applied at a different size: it worked every time it was tried** (PH5) |
| DPP / ds_swizzle exchange, radix-4 grouping | PH4, N-kernel | slower | not needed here |
| decision register | internal code notes | no entry for a >7-stage strided pass or a 2^13 b1 | not tried, not rejected |

## 6. How many large transforms a run does (mod; NTT_SIZE_STATS will replace this)

Two families, with different sizes (internal code notes §4.3-4.4):
1. **Node-local B form / mdev / batch** (prime per APU, whole planes up to 2^31 points at size 1; 3·2^29 in the mdev pool): logn 27-31 are the big ones. The bs "top levels" (16.2 s at 10 nodes,
   14.4 s at 576 in the estimator) and part of the batch tier (19.8 / 17.6 s) live here. 4 -> 3 passes.
2. **4-step C form pieces** (`dist_core`, the mn tier): rows of 2^(logn/2) points. At 10 nodes logn 35-36, rows 2^17-2^18 = 2 passes already (no gain). **At 576 nodes
   (plane 2^20 x 2^20 over 2304 ranks) rows are 2^20-2^21 = 3 passes today, 2 after.** This is the larger term.

Estimator (`estimate.py --fabric target-m --bw 47`, g = 576; local = level wall - exposed): level 192 (12 pieces) 10.0 s, level 576 (6 pieces) 6.7 s, division (16 pieces) 17.8 s,
last reciprocal steps (groups 576, 3-5 pieces) about 3 s = **about 37 s of local piece time at rows >= 2^20**. The levels up to group 64 (rows <= 2^19 under plan 120) are already 2-pass.
NTT share of a piece (mod): 2^31 points per piece over 4 APUs x 3 primes = 18 sweeps of 2^29 points per APU, each 3 passes of about 5 ms = 270 ms of the piece's 0.83 x 1.22 = 1.0 s (`T_PIECE_31_NP[3]` x pipeline factor) = **27 %**
(a: depends on the unmeasured exact pass cost at 2^20 rows).

| | 10 nodes (435 s modelled wall, 462 m) | 576 nodes (219 s target estimate, 266 s derated, 748 s aac7-class fabric) |
|---|---|---|
| node-local logn 27-31 NTT time (assumed 40 % of top levels 16.2 / 14.4 s + a quarter of the batch tier at 19.8 / 17.6 s, NTT share 40 %) | 6.5 + 2 = 8.5 s | 5.8 + 1.8 = 7.6 s |
| saving at 17 % (central 15 ms of 90) | **1.4 s** (0.5-2.5) | **1.3 s** (0.5-2.2) |
| 4-step pieces at rows >= 2^20 | 0 | 37 s local x 27 % = 10 s of NTT; saving 22-30 % -> **2.5 s** (0.9-3.6; pessimistic fat-pass cost makes it 0) |
| **total per run** | **about 1.4 s (0.5-2.5), 0.3 %** | **about 3.8 s (1.5-6), 1.5-1.7 % of 219-266 s; 0.5 % of the aac7-class 748 s** |

Exposed fabric (50 % at 10 nodes, 63 % at 576 on aac7-class) hides nothing of this: the local pieces are on the critical path only where not overlapped (mn_model assumes they add), so the table is an upper bound if
the chunk pipeline hides some local passes. Four-prime pieces (`ECALC_NP=auto`) scale the 576 term by 1.33 (a).

**Measurement that confirms it: `NTT_SIZE_STATS` (branch s22).** Needed output: per logn, per direction (fwd / inv / inv_pw), per layout (single, batched rows), count and device milliseconds. Then
saving = sum over logn in {20-22, 27-33} of (count x ms x fraction of passes removed x per-pass share) with the measured per-pass times from t_ntt. It also decides whether the 576 piece term exists at all
(the piece rows at the target are modelled, never run: the biggest real run is 10 nodes with rows 2^18).

## 7. Effort, risk, test plan

**Effort: M-L.** (1) Microbench E0, S (1 day): HBM read/write of a 512- or 1024-row x 16/8-column tile at s_lo 12, 13, 21, 22 and the 2^13 b1 (instantiate `k_b1r<13,4>` after extending `b1r_swz` by one index bit
and the 8192-entry twiddle table `B1R_TW`; 1 block/CU, 512 threads): gives the actual stride / segment / occupancy penalty without the butterflies. (2) 9-10 stage register-blocked strided body `k_b16w` (generalise `k_b16r`: 3-4 groups of 3 stages,
two LDS exchanges, unpadded swizzled 64 KB tile, twiddles from the global table as in K13 VAR 1, T_H by squaring over 9-10 stages), forward and inverse, `MM` 0/1, scale and R3 forms: M (4-6 days). (3) plan integration in `make_plan`
(`NTT_STG` up to 10, `NTT_PLAN` codes 13x, `plan_auto` per logn): S. (4) validation: M.

**Risks.**
- Correctness (low): outputs are canonical and unique, so any exact plan equals the old one bit for bit. Gates: `t_ntt 24` (593 checks) plus new plan cases in the STG 3-7 hash loop (`t_ntt.c:720`), `t_mul 20`, `t_mul 0 big`, e9 identical in both bases, mn e8 sizes 2-4. Inverse with fused pointwise (`inv_pw` in the b1 pass) and the R3-fused forms must be re-checked for 13 stages; `NTT_R3_FUSE` stays off.
- Occupancy / latency (medium): 1 block/CU (2 waves/SIMD) for the 9-10-stage and 2^13 kernels. Evidence for: 2^12 b1r at 2 blocks/CU runs; occupancy 3 -> 4 changed the 7-stage passes by +-2 % (K13). Evidence against: RESULTS §33's 1-block loss (old kernel) and LDS 19 vs 35.8 TB/s. Register pressure is not the limit (VGPR 46-66 of 256 at 2 waves).
- Memory pattern (medium): 64 B segments (plan 2/3) and the new strides are unmeasured; E0 settles it before any kernel work. s_lo 17 and 24 are avoided by all listed plans.
- Dist integration (low): none of the twiddle+pack kernels depends on the pass boundaries.
- Possible regression at logn 24-26 where the plan stays at 3 passes: keep `plan_auto` per logn (use the new plan only where it removes a pass).

**Test plan on one aac7 node (`tests/t_ntt`, one APU, median of 5; build main + patch, NOP build = `t_ntt_nop` as in S21).**
1. E0 microbench: tile copy rates by (rows, columns, s_lo, blocks/CU); go/no-go if 9-stage 16-column tiles at s_lo 13/22 < 1.6 TB/s.
2. `t_ntt bench 31 pass` for the new plan: real and NOP, per pass, forward and inverse; compare to section 4 (go if real 3-pass total <= 80 ms).
3. `t_ntt bench 31 whole` and layouts 2^14 x 2^17, 2^20 x 2^11, and **batched 2^20 / 2^21 / 2^22 rows** (the 576 piece shape, e.g. 455 rows x 2^20 per APU) against the default.
4. `t_ntt 24` VERIFY OK, `t_mul 20`, then `ecalc 1e9` identical and a 6.44e10 single-node run (total, mdev and bs top levels against the S21 baseline 93-96 s).
5. Pair-run ABBA at 10 nodes only if step 3 shows >= 10 % on the 2^31 transform (expected effect 1-2 s is below the 10-node noise, sd 18-49 s): not worth node time; judge by `t_ntt` and the target's NTT_SIZE_STATS.

## 8. Decision for the user

| option | cost | benefit |
|---|---|---|
| (a) Do E0/E1 only (1-2 days, one aac7 node-hour) | none beyond the day | settles whether 64 KB / 1 block/CU tiles and 64 B segments run near the model; decides (b) |
| (b) Build 13/9/9 or 12/10/9 (M-L, 2-3 weeks) | effort, small occupancy risk | -15 ms (range -5...-24) per 2^31 transform; -1.4 s at 10 nodes, about -3.8 s at 576 (mod, 1.5-1.7 % of the target run); also 3 -> 2 passes for every 576 piece |
| (c) Leave | none | 0 |

Recommendation: (a), then (b) only if the target's NTT_SIZE_STATS shows rows >= 2^20 (the larger term). On its own, the 2^31 case (1.4 s at 10 nodes) does not justify the effort; the 576 pieces might.
