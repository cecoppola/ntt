# COMPUTE — Compute: kernels, single node, primes, APU multiply, division, grid groups

Scope: NTT and big-number kernels, the single-node levers, the prime-set strategy, the APU multiply, the Newton division, the grid groups. Current conclusions and live numbers (labeled m measured / mod modeled / a assumed) condensed from the former per-topic files; the removed text of each is unchanged in `archive:cleanup_1103/results/<OLD>.md`. Every former section keeps its `§ID` heading.

## KERNELS — single-node kernels

Scope: single-node kernels (NTT constants and passes, dbig add/sub, maxidx, quarter select, CRT carry lookahead, twiddle packs, decimal seed arithmetic, batch-tier host gaps) and the design study of a 3-pass NTT. Condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data and drivers in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: (m) measured, (mod) modeled, (a) assumed. Times Eastern.

### Topic summary

Single-node GPU and host kernels of the e-digit run, from the NTT constants (Phase 13) to the dbig add/sub family (October). Current state (checked against `ecalc/README.md`, RESULTS.md, and the code):

- **Adopted and now built in (their switches are gone from the code or README)**: K13 `NTT_MODMUL=1` (reduced-correction Barrett, exact, +5-12 % per transform); K13b `NTT_B1R=3` register-blocked b1 pass and `NTT_PLAN=1` (auto plan that moves the passes off the slow strides, +4-13 % per transform, -4.0 % compute at 4e10); S38/S40/S41 `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL` (user rule, 2026-10-10: -12.3 s, -1.6..-3.2 s, -0.9..-2.1 s at 1 node; -50 s at 4 nodes); S51 K2 `DBIG_ADDSUB3` and K3 `DBIG_MODQ_MONT` (built in; 1 node -2.03 s +- 1.38 on aac7); G515 `DIST_TWREC_G` (default since 2026-09-28, built in 2026-10-12); S115 `BI_MUL1_FAST` (decimal `mul_1` and fused span step, built in).
- **Defaults on by decision, switch still a fallback** (`ecalc/README.md`): `RNS_CRT_SCAN` (K4b, default 1 since 2026-10-12, D21), `P24_CRT_SCAN` (K4c, default 1 since 2026-10-13, D24), `BS_BATCH_PIPE` (N3, default 1 since 2026-10-12, D20).
- **Dropped (measured)**: K13 H2 MALL-chunked passes (`NTT_MALL`), H7 non-temporal stores, H6 occupancy variants (`NTT_B16_VAR`), `NTT_MODMUL=2` Shoup (loses 4-13 %), `NTT_B1R=4` (dominated by 3); S51 K4 `RNS_CRT_SUB` (no gain); S39 candidate A "raw-once k_gather" (S40 refuted the remote-bound assumption); NTT3P 3-pass 2^31 NTT (design study; its microbenchmark E0 failed the gate, `results/CODE_TOOLS.md / S23 / S24`).
- **Live conclusions**: (1) the NTT passes are issue/latency-bound at 1.2-1.6 TB/s, not bandwidth-bound; there is no MALL cliff; (2) a stride penalty at s_lo 17 and 24 (about 12 % of a 2^31 transform) is an address-pattern effect that padding or rotation does not fix, so the plan avoids those strides; (3) by-value kernel argument structs with a runtime quarter index cost 112 B/lane of scratch and about 30 % of k_gather: use scalar-select (QSEL); (4) multi-node gains of the dbig kernels exceed the 1-node gains because the same kernels run inside the multi-node division; (5) gains below about 1 s per run are inside run-to-run noise (sd 1-2 s at 1 node); trust kernel-level stats (`DBIG_S51_STATS`) for them.
- **Open**: the k_gather strided write (next floor after QSEL), the k_addsub2 residue (about 1-1.5 s, weak model), k_modq beyond Montgomery, the s_lo 17 mechanism (HBM address hashing suspected), the four-prime `ec_words_to_dec4` (dominates k_crt24 and the four-prime k_crt_batch), K3 never exercised at 4e9 on CPX (modq counter 0), the level-1 host gap of N3 (about 2.3 s).
- **Superseded**: S38's "armed" ABBA (replaced by the 4-node confirmation inside S38); S39's raw-once gather plan (S40); S44's CPX figures are coverage only (CPX XCD devices share one APU's HBM).

### K4b — `k_crt_batch`: the per-chunk carry chain as a parallel carry-lookahead

**Results (m).** `t_crtk 29`/`27` equivalence: 48 cases (3 and 4 primes), all identical. Kernel, C form 2^29 / 65536 stripes: three primes **30.18 -> 13.65 ms (2.21x, 569 -> 1259 GB/s)**; four primes 49.85 -> 34.11 ms (1.46x); 2^27 / 16384: 8.02 -> 3.95 ms (2.03x) / 12.54 -> 8.56 ms (1.47x). Gates (2 n and 1 n, off and on; before the rebase and after two rebases on main, the last with P24 for every multi-node product): all PASS, VERIFY OK, digits identical, segv 0; `t_mul 20` 169 checks and `t_strategy 24 2 V` 12 checks OK both arms. **ABBA 1 node, 6.441e10, 4 rounds (16 runs, all VERIFY OK): B - A = -2.52 s, 95 % CI [-4.19, -0.85]** (per round -3.6, -1.4, -3.2, -1.9 s; p < 0.05); arm means A 71.47 s, B 68.95 s (-3.5 %). Modeled 2.4-2.6 s of kernel time per APU (4.5-4.8 s at 2.21x).

**Verdict.** Adopt candidate, above the 2.0 s assumed in OPPORTUNITIES; merged switch off, then adopted as default 1 (D21, 2026-10-12, `ecalc/README.md`). **Open**: the four-prime form gains less (1.46x) because it is bound by `ec_words_to_dec4`'s sixteen divisions, not by the chain (a cheaper four-word-to-decimal conversion is the next step); multi-node timing not measured here (2-node gates show digits only); `k_crt24` has the same chain (done in K4c). Driver `archive:drivers/k4b/`.

### K4c — `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead

**Results (m).** `t_crt24k`: 279 checks (9 shapes x 6 coefficient kinds x up to 3 ranks) all identical; `t_p24` against the GMP model 78594 checks OK both switches. Kernel at 2^30 points per rank (rows 4096 / 8192 / 16384): **157.96 / 158.39 / 158.76 -> 101.95 / 101.95 / 101.26 ms, 1.55-1.57x (290 -> 450 GB/s)**. Gates on two commits (2 n and 1 n off/on): PASS x4 each, P24 in use at 2 n. Profiling round: node 0 `crt` sum per run A 4.13 / 4.25 s -> B 2.95 / 2.94 s: **-1.24 s per run** (m, 1 round). **ABBA 2 nodes, 6.441e10/node, 4 rounds (16 runs, VERIFY OK): total B - A = -1.89 s, 95 % CI [-7.51, +3.73]** (per round -2.8, +2.0, -6.3, -0.5; not significant); A 126.28 s, B 124.39 s; dm -0.55 s [-3.00, +1.90], bs -0.43 s [-1.67, +0.82]. Modeled kernel saving -1.49 s per run (4.19 s CRT time at 1.55x).

**Verdict.** The kernel gain is clear, the wall claim unresolved (the A arm had two slow runs, 131.0 and 131.4 s, against B 122.7-125.9 s; resolving -1.2 s at a 3.5 s per-round spread needs about 40 rounds (mod)). Merged switch off, then adopted as default 1 on the kernel evidence (D24, 2026-10-13). **Open**: `ec_words_to_dec4` now dominates the kernel (K4d); the CRT share at larger node counts is not measured (per-node CRT time stays about the same, mod). Drivers `archive:drivers/k4c/`.

### S38: the next single-node speed lever. Found: the dbig add/sub kernel (k_addsub) ran at 36-84 GB/s; DBIG_ADDSUB2=1 (coalesced) cuts 10.4 -> 2.0 s of GPU time per APU

**Phase table, one node, defaults** (6.441e10 standard share, total 94.2 s; larger 8.5e10 share, total 140.7 s, peak 371.7 of 480 GB): init 12.6 s (13.4 %) / 16.2 s; bs wait for the seed thread 11.3 s (12.0 %) / 14.7; bs batch tier levels 1-23 21.8 s (23.1 %) / 31.7; bs mdev tier levels 24-25 13.7 s (14.5 %) / 22.2; recip 8.05 s (inside dm); dm total 34.4 s (36.5 %) / 55.3. Tier split: C form 29 products 24.4 s, B form 63 products 4.4 s. Init timeline: plane pools 5.2 s, seed thread 5.3 -> 24.0 s (waited 2.6 s for region pools), background mapping of the parity-1 halves 12.6 -> 26.8 s, level 1 starts at 25.6 s; at 8.5e10 mapping binds by 5.3 s (seeds joined 31.2 s, level 1 at 36.4 s). GPU view (rocprofv3, 7000 dispatches): busy per APU 56.3 / 56.3 / 55.4 / 43.6 s in an 81 s window; busiest kernels per APU (APU0): **k_addsub 10.35 s (APU3 4.5)**, k_gather 7.7, k_crt_batch 4.5, k_push3 4.4, k_b1r 4.4, k_maxidx 2.4, k_modq 1.6, k_bload 1.2.

**Ceilings of the other candidates** (mod): S-5 push-form B broadcast at most 1.0 s (below the 1 s bar); S-4 E2 fixed-slot allocator 1.3 s at 1e11 (0.5 %), the layout does not fragment at these shares; the 11.3 s seed wait is co-bound by seeds and mapping (mapping alone binds by 1.55 s here, 5.3 s at 8.5e10; belongs to `ECALC_VMM_BG`/`VMM_SAFE`); APU 3 idle in the tiers 12.7 s, perfect rebalancing 3.2 s at best (large design change); the C-tier all-to-all 10.0 s of 24.4 s exposed host wait (41 %) but the push kernel already runs at 221-253 GB/s and the transform is chunk-pipelined (bandwidth lever closed); k_maxidx 2.4 s (follow-up, S40).

### S44: aac6 SH5_MI300A nodes -- regression and ABBA of DBIG_MAXIDX_TOP, DBIG_QSEL, DBIG_ADDSUB2

**Regression (s12-09)**: `t_ntt 24` fails only the speed check (forward rate 0.25-0.34 TB/s vs 0.9; 5 of 3215 checks, a CPX XCD has 1/6 of the bandwidth); `t_verify` 334, `t_out` 1, `t_patch` both forms PASS; with the knob `t_mul` 189, `t_dbig` 555, `t_newton` 620, `t_bs` 10 checks PASS; `t_mn_grid` not run. e9 gates (1e9, POOL_LOG=29): defaults, TOP=1, QSEL=1, TOP+QSEL, ADDSUB2=0, LIMB_BASE=2 (and with TOP + QSEL): all VERIFY OK, identical (6.79-14.21 s).

(Details moved to `archive:repo_cl6_2026-10-25/results/KERNELS.md`, 2026-10-25.)

### S51: kernel round 2, part b (K2 k_addsub3, K3 k_modq_m, K4 k_crt_batch_m) -- FINISHED: K2+K3 propose default (1-node significant on aac7 and aac6, 4-node ns), K4 no gain

**Decision stage (m, identical on aac7, aac6 PPAC and aac6 CPX)**: probe checksums equal; time ratios K2 0.745 (aac7) / 0.736 (PPAC) / 0.661 (CPX), K3 0.070 / 0.058 / 0.043 (`db_mod_qs` 23.0 -> 1.0 ms on the CPX smoke test); K4 kept on its gate. All 12 gates (each switch alone, 1e9, three node types): PASS, VERIFY OK, identical, segv 0.

### NTT3P: can ecalc's large transforms run in 3 memory passes instead of 4? (design study, no code changed)

Labels (m), (mod), (a). Sources: S21 A37-Q4 NTT sweep, K13/K13b, `ecalc/ntt.c` (`make_plan`, `plan_auto`, `k_b16`, `k_b16r`, `k_b1r`), `estimate.py` / `mn_model.py` (`target-m --bw 47`, `aac7_s18 --bw 6`).

**Summary.** Best split at 2^31: **13 / 9 / 9** (b1 of 2^13 points, two strided 9-stage passes, all with 16-column 128 B segments) or **12 / 10 / 9** (keeps today's 2^12 b1; one 10-stage pass with 64 B segments); both fit the 64 KB LDS with no register staging. **Modeled saving per 2^31 forward: about 15 ms of 90.4 (17 %), range 5-24 ms**; the floor of any 3-pass plan is about 66 ms (3 x 17.3 ms NOP pass + 14 ms modmul). Per run (mod): 10 nodes about 1-2 s of 435 s (0.3 %); 576 nodes about 4 s (1.5-7) of 219-266 s (1.5-2 %), mostly NOT the 2^31 transform but the four-step pieces' row/column transforms of length 2^20-2^22, which run 3 passes today and would run 2. Effort M-L (2-3 weeks), correctness risk low (canonical outputs are unique), performance risk medium (1 block/CU, 64 B segments). No prior attempt at more than 7 strided stages or a 2^13 b1 existed. Recommendation: a 2-3 day microbench first (E0/E1), build only if the fat pass is within 10 % of the model. **Outcome: E0 was run in S24 and failed both gates (13/9/9 forward 90.6 ms against the gate <= 80 ms and today's 89.9; `results/CODE_TOOLS.md / S23 / S24`); NTT_SIZE_STATS measured the whole transforms >= 2^27 at only 2.5 s per APU of 453 s at 10 nodes. Dropped.**

**Constraints.** `k_b16`/`k_b16r`: block 2^7 rows x 16 adjacent columns = 2048 points (128 B per row), 256 threads, `sh[128][17]` = 18.4 KB, 3 blocks/CU (LDS-limited), VGPR 46-66; `NTT_B16_STG` clamped to 3..7, so no strided pass is longer than 7 stages. `k_b1r`: 2^10-2^12 points per block (2^12 in production at 2^31, plan 121, 32 KB, 2 blocks/CU). LDS 64 KB per workgroup = exactly 8192 points; HBM 2.0 TB/s in NOP passes, copy kernels 2.2-2.4 TB/s (m). Pass logic is private to `ntt.c`.

**Pass counts now vs with 13/9/9 (computed).** logn 17-19: 2 / 2; **20-22: 3 / 2** (13+7, 13+8, 13+9); 23: 3 / 3 (13 + 9 = 22 stages); 24-26: 3 / 3; **27-30: 4 / 3**; **31 (plan 121, 5/7/7 + b1 12): 4 / 3**; 32-33: 4 / 3 (with C = 8 or 12+10+10). Two-pass coverage logn <= 22; three-pass coverage logn <= 31 (<= 33 with 64 B / 32 B segments). Splits enumerated at logn 31: 9/9/13 (recommended, s_lo 22, 13, 0, 1 block/CU), 9/10/12 (alternative, b1 untouched, 64 B segments), 10/10/11 (more segment risk), 11/10/10 with 32 B segments (not recommended: half-sector reads), 8/8/8/7 (4 passes: no gain, shows the pass count is the cost). All avoid s_lo 17 and 24.

**Cost model at 2^31** (forward, ms; 34.4 GB per pass; calibration S21 (m): NOP passes 17.3 / 19.0 / 17.1, b1-12 NOP 24.2, real 24.0 / 20.8 / 19.4 / 25.8; b1-12 costs 25.8/12 = 2.15 ms per stage all-in). Strided 9-stage 128 B pass 20.0 / 22.5 / 25 (optimistic / central +10 % for 1 block/CU / pessimistic +25 %); strided 10-stage 64 B 21.5 / 24.5 / 29; b1 13 26.5 / 30 / 33. Plan totals optimistic / central / pessimistic: 9/9/13 66.5 / 75.0 / 83 (saving **24 / 15 / 7 ms**); 9/10/12 67.3 / 72.8 / 79.8 (23 / 18 / 11); 10/10/11 66 / 74 / 85 (24 / 16 / 5). Once the passes are not HBM-bound the total is about 31 stages x 2.15 = 67 ms whatever the split, so choose by risk; the memory-only floor (3 x 34.4 GB at 2.2-2.4 TB/s) is 43-47 ms. Other lengths (per 2^31 points, mod): logn 20-22, 3 -> 2 passes: 60 -> 50-52 (-15 %, -22 % at best); logn 27-30 about -15 ms-equivalent. (E0's measured per-stage cost of 3.2-3.6 ms for the fat pass is why the model failed.)

**Prior attempts (relevance).** RESULTS §33 (D5, 2026-09-14): 2^11/2^12 b1 with the old kernels lost to occupancy (2^11 at 24 KB 126-129 ms, 2^12 at 48 KB 153 ms vs 117 for 2^10), superseded by the register-blocked b1r; RESULTS §48: LDS bandwidth 35.8 TB/s at 3 blocks/CU, 19.0-19.3 at 1 block/CU (the 1-block price: LDS traffic of a 9-stage body about 3.6 ms at 19 TB/s (mod), not binding); K13 H6 occupancy +-2 %; K13 H2 no MALL cliff; K13b: "no 3-pass plan avoids s_lo 17 and 24 with b1 2^10" (the 3 x 7 limit came from the 7-stage clamp); the b1 2^11/2^12 lever that removes a pass "worked every time it was tried".

**How many large transforms a run does (mod; NTT_SIZE_STATS replaced this).** Node-local B form / mdev / batch: logn 27-31 (bs top levels 16.2 s at 10 nodes, 14.4 s at 576 in the estimator; part of the batch tier 19.8 / 17.6 s). Four-step C-form pieces: at 10 nodes rows 2^17-2^18 are already 2 passes (no gain); **at 576 nodes (plane 2^20 x 2^20 over 2304 ranks) rows are 2^20-2^21 = 3 passes today, 2 after**: about 37 s of local piece time at rows >= 2^20, NTT share 27 % (a). Totals: 10 nodes 8.5 s of node-local NTT time x 17 % = **1.4 s** (0.5-2.5) and 0 for the pieces; 576 nodes 7.6 s x 17 % = 1.3 s plus pieces 37 s x 27 % = 10 s x 22-30 % = **2.5 s** (0.9-3.6, 0 under the pessimistic fat-pass cost): **about 3.8 s (1.5-6)**, 1.5-1.7 % of 219-266 s, 0.5 % of the aac7-class 748 s. Four-prime pieces scale the 576 term by 1.33 (a); exposed fabric hides nothing of this (upper bound if the chunk pipeline hides some passes).

**Effort, risk, test plan, decision.** Effort M-L: E0 microbench (1 day: tile copy rates at s_lo 12, 13, 21, 22 and the 2^13 b1), a 9-10-stage register-blocked strided body `k_b16w` (4-6 days), plan integration (S), validation (M). Risks: correctness low; occupancy/latency medium (1 block/CU); memory pattern medium (64 B segments unmeasured, E0 settles it); dist integration low; possible regression at logn 24-26 (keep `plan_auto` per logn). Test plan: E0 go/no-go (9-stage 16-column tiles at s_lo 13/22 >= 1.6 TB/s), `t_ntt bench 31 pass` (go if real 3-pass total <= 80 ms), whole and batched 2^20-2^22 shapes, `t_ntt 24`, `t_mul 20`, `ecalc 1e9` identical, 6.44e10 single-node. Options: (a) E0/E1 only (1-2 days, one node-hour), (b) build (2-3 weeks, -15 ms per 2^31 transform, about -1.4 s at 10 nodes, about -3.8 s at 576 (mod)), (c) leave. Recommendation was (a), then (b) only if the target's NTT_SIZE_STATS shows rows >= 2^20: E0 then failed.

### N3 — host gaps in the batch tier (`BS_BATCH_PIPE`)

**Verdict.** Measured -0.99 s per 1-node 6.441e10 run (CI -1.18..-0.80, digits identical on all four gates), the low end of the 1-3 s model; the batch phase alone gains about 1.8 s. Adopted as default 1 (D20, 2026-10-12; `=0` keeps the old host side). At 576 nodes the per-node leaf tree is about the same size (6.6e10 digits per node), so a gain of the same order per node is expected (mod). **Open**: B's init averaged +0.55 s in all four rounds although the switch is first read after init (init varies +-1.2 s; a position effect within ABBA is possible; without it the total gain would be about -1.5 s (mod)); level 1 is still host-bound in B (1.22 s layout = first touch of the 820 MB node array and the 1.9 GB product list, about 1.1 s host time in the batch call; gap about 2.3 s: 3.62 s wall vs 1.27 s GPU), next steps keep node arrays across levels and pre-fault buffers during the seed wait; tested only on VMM pools (the host-pool spill-merge path was not timed). Raw data `archive:results-raw/N3/`.

## SINGLE_NODE — single-node speed and options

Scope: single-node speed and options: init, level pairing, remaining options on the defaults, development items, seed terms, combined switches. Each section below condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; unabridged text: `archive:results_merged_2026-10-19/`. Numbers are measured (m) unless marked modeled (mod) or assumed (a); times Eastern.

### Topic summary

- **What it is.** Single-node (one MI300A node, 4 APUs) speed work from Phase 11 to the Oct-2026 switches: what happens in init, the seed thread, the batch and mdev tiers, the reciprocal and the division, and which switches were adopted. Switch rows and defaults are in `ecalc/README.md`; the adopted list is in `RESULTS.md`.
- **Adopted / default now.** `RNS_STRIPED_PAIR=1` (§P A2, level 22 paired, -0.55 s at 4 x 10^10); `ECALC_DM_POOL` on from 5 x 10^10 (§P: 7 x 10^10 153.5 s); `RNS_AUTO_PIECE_COST` (built in since 2026-10-12; §V214: -12.2 % / -10.1 % / -6.4 % of `total` at 1e11 / 1.16e11 / 1.3e11); **`BS_SEED_FILL=128` (default since 2026-09-27, user decision; §T215: bs -12...-16 s, wall -6...-16 s at 1e11, -6.6 s (-10 %) at 4 x 10^10)**; I11's init table uploads (bit-identical). Not adopted: `RNS_PLANES_3Q30` (default 0: planes save 3.6 s of phases but cost 4.7-6.3 s of mapping at init), `RNS_BATCH_TILE_GB` stays 15.
- **Reference walls.** 4 x 10^10 on one node about 81.5 +- 1.4 s (phases 66.0 +- 0.4 s, init 15.5 +- 1.1 s; Phase 11 final build); 10^11 about 215-217 s on Phase 14 defaults (init 17-18, bs 96, dm 101, recip 43), 198.8-203 s with fill 128. aac7 mapping is slower than aac6 (0.070-0.084 s/GB vs 0.047 s/GB alone); init is the max of the seed thread (about 15 s) and the pool mapping.
- **Live conclusions.** (1) The seed thread and the arena mapping finish together on aac7; GPU seeds (A37-R1) are a no-go for now (0-6 s, mod). (2) A seed span that makes the first batch products fill 2^k lengths (fill 128) beats the default; larger fills lose to the seed cost. (3) Every S change shifts the block pool's VMM remaps (serialized under the global pool lock): dm +1-4 s at 1e11, a per-device remap lock (RL, `dbig.c`) is the cure. (4) `ECALC_BUDGET_CHECK` stops 1e11 on one node on the V314-era defaults (rc 8, counts the borrowed pool twice); V314's 1e11 runs used `ECALC_BUDGET_CHECK=0`. (5) The correction count after the division hook is data dependent: a correction after the writer started forces a redo of the digits (§V214 finding; later removed by the K patch, see `results/MEMORY_IO.md §KP15`).
- **V314 ranking (modeled, at 1e11 / 576 nodes).** (1) decimal `mul_1` + fused seed span -9 s / -9...-11 s; (2) per-run seed terms -6.6 s (this became §T215's `BS_SEED_FILL`); (3) radix-3 pass fused into the first 2^k pass -6...-8 s / -3...-4 s; (4) reciprocal middle product -3.4 s / -1.0 s; B4 (5.2^k / 7.2^k lengths) needs a new prime set: not recommended.
- **Combined switches.** `DBIG_SMALL_BOUNCE=1 DIST_GATHER_RAW=2 BS_SEED_PULL=1` together: -7.16 s [-8.08, -6.23] at 1 node (aac6, 6.441e10), -5.58 s [-10.54, -0.63] pooled at 2 nodes (aac7); digits identical, no memory cost; adoption is the user's decision (§CAND).
- **Open.** `RNS_AUTO_PIECE_COST` is built in; `DIST_TWREC=1` at 1e11 and checkpoints with O_DIRECT in §V214 were never measured there (O_DIRECT checkpoints were measured in `results/MEMORY_IO.md §IO15` and §S114); the 4-node cand round (item 7g) is not recorded here; §B08's `t_ntt` fix is a test-suite change.

### §T215 — `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths (Phase 15, row 1c)

**1. What decides the fill.** `binsplit.c` combines spans of S terms pairwise; a batch level uses one length for all products, `pick_len(maxnc)` = 2^k, or 3.2^(k-2) when k - 2 >= 10 and maxnc <= 3.2^(k-2); with m1 = the last span's limbs, maxnc(l) <= 2^l.m1 and m1 about S.log10(k_max)/18. If m1 <= 2^j every product from 2^10 up fills a 2^k length (fill >= 0.99, mod) and the batch tier's top level ends exactly at the mdev threshold, so **one more level runs in the batch tier** (3-4x cheaper per output limb than mdev: V3's S = 168 level 23 in batch 2.85 s vs about 10 s in mdev). Otherwise padding is 1.34-1.5x: **S = 256 at 1e11 has m1 = 143: fill 0.74 from level 4 on; at 4e10 m1 = 138: 0.71** (mod; the model's `max_nl` matches the log's at all 26 levels). Cost terms fitted to V3's logs (mod): batch 3.39e-10 s per per-APU point + 1.15e-7 s per span (+-0.5 s); mdev 1.23e-18 s per limb^2 + 0.89 s per level (+-3 s). **Rule: S = the largest S whose last span has <= 128 limbs**: S = 239 at 4e10, **229 at 1e11**, 235 on the 576-node plan's node 0, about 184 on its last node (k about 3.5e12); no single `BS_SEED_TERMS` fits every size or node.

**2. The switch.** `BS_SEED_FILL=<limbs>` (`binsplit.c bs_seed_terms_for(bend)`, lgammal; overrides `BS_SEED_TERMS`; 0 = off, byte-identical by construction: the span changes only the tree's shape, P and Q are unique); one line each in `seed_limbs` and `binsplit_e`; `mn_plan.c plan_leaf` (one line) so `MN_PLAN_ONLY` plans the same tree (576-node node 0: S 256 -> 235, mdev levels 3 -> 2, leaf dist_db pieces 16 -> 12, mod). `tests/t2_sweep.sh` and `archive:scripts_2026-10-23/t2_seed_model.py` are the tools.

### §CAND — the three off-by-default speed switches combined (interaction test)

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

### §S21 — single-node A37 measurements (R2 seed microbench, Q1 init timeline, R4 dc split, R6 small-j doublings, Q4 NTT structure)

aac7 runs 2026-10-07 21:02-21:13 on hold 12377 (and a CPX node for R2); all rc 0, VERIFY OK; gates passed (digits identical with every switch off and on against the 1e9 reference; `t_ntt 24` OK).

**1. A37-R2 seed microbench `b_seed64` (CPX node, 192 threads).** dec18 (ecalc's decimal seed) vs b64kK (base-2^64 K-way); 451 leaves x 4 schemes, P and Q identical in every case. **Single thread: no gain** (b64k32 vs dec18 ratio 0.86-1.07: 36.3 k vs 34.1 k ns/leaf at 10 nodes rank 0, 30.5 k vs 35.3 k at 576 nodes rank 288). All-core wall scaled to nspan: dec18 9-15 s (agrees with production single-node seed spans 10.7 s), b64k32 6-8 s (25-45 % less, noisy, not measured in the engine; about -3...-5 s of seed-thread time, mod). Gate for A37-R1: **open** (a CPU rewrite does not close the gap by itself), but the exposed wait is small (§2).

(Details moved to `archive:repo_cl6_2026-10-25/results/SINGLE_NODE.md`, 2026-10-25.)

### §PEER - `XSHMEM_PEER=1`: same-node PEs exchange by device copy instead of the NIC (branch peer 78015d4; aac7 2026-10-25 02:10-03:05 ET; m)

**Verdict.** Digits identical, no memory cost, no regression at 1-2 nodes; Pp - Po -16.8 s at 1 n 3.3e10 and -7.2 s (2 n 2e10) / -20.7 s (2 n 6e10). Adoption (switch default on for P, or tie it to `ECALC_PE_PER_GPU=1`) is the user's decision. **Not done:** 3 n; signals / control words by direct writes; a copy kernel using several SDMA engines; Path 2 (Cray 12.1 `shmem_ptr` with xpmem) is untested (the switch is inert there).

**Verdict.** Measured: communication proper (worker span, 6.1-9.0 s per PE) is separate from the GPU work waited for in the post (4.9-7.7 s per PE, 14-22 % of the 34 s run); the latter is the PE's own queued GPU compute, not exchange cost. Compute imbalance between PEs is small (0.3-0.7 s above the others on one device, 1-2 % of the run) and does not account for the ROFF poll of 0.8-3.7 s per PE; that idle is the slack between four PEs whose GPU work ends at different times in every exchange. Hypothesis (not shown): the per-exchange spread of GPU completion times is larger than the run totals suggest. Next measurement: a per-exchange series (GPU time of the work before each post, per PE) at a few hundred exchanges, with kernel names.

#### Layout D (path 1, one process per node) at the record per-node share, 6.632e10 digits, 1 node (aac7 2026-10-28; m)
The engine does not apply at one node in layout D: with `COMM_TRANSPORT=xshmem` the init finds one node, switches the engine off (no `XSHMEM_ROUND_STATS` output, no posts), and the 4 APUs of the process exchange in process (`k_push3`). So the posts, sync wait and pre-pass window of the P tables do not exist on one node in layout D; they need two nodes (the 2-node hold is not available outside the e8b/e31n gate slots).
- Times, digits tail 79932777543051118449 (the reference), VERIFY OK in all four runs: plain 76.31 s; `COMM_TRANSPORT=xshmem` (engine off at 1 node) 77.58 s; `XSHMEM_ROUND_STATS=2` 75.59 s; rocprofv3 trace 75.14 s (no perturbation; one process, 4 agents, one 17 MB csv).
- Kernels per APU (kernel sum 35-42 s, union-busy 31-38 s of a 62-64 s kernel span): `k_push3` 4.9 s (2376 calls, the in-process exchange), `k_b1r<10,3,0>` 4.1-4.8 s, `k_b16<6,0>` 2.6-2.8 s, `k_crt_batch` 2.7-2.8 s, `k_b16r<0>` 2.4-2.5 s, `k_gather_raw` 2.2-2.3 s (3 % of the run; there is no `k_gather_mn`), `k_b1r<10,3,2>` 1.4-1.8 s. APU 7 is the least busy (31 s).
- GPU idle: 24.5, 25.2, 26.4, 32.4 s per APU (39-51 % of the span). By gap size: under 1 ms 0.35-0.45 s, 1-10 ms 0.5-0.6 s, 10 ms and longer 23.6-31.5 s (72-126 gaps): 95 % or more of the idle is long waits (CPU phases, GMP, sync), 1.2-1.5 % of the run is short gaps.
- Launch-fusion lever: does not apply to layout D at one node. Upper bound (all sub-10-ms gaps removed, which fusion cannot do) 0.9-1.1 s of 75-76 s (1.2-1.5 %, measured as a bound; realistic gain a fraction of it). On layout P the same bound is 1.9-2.8 s of 34 s (6-8 %, m as a bound). Whether the record's multi-node layout D has busy posts and a pre-pass wait needs a 2-node run (`XSHMEM_ROUND_STATS=2`, trace with the wrapper used here); not measured.

## PRIMES_STRATEGY — prime count and strategy

Condensed 2026-10-19 (task 8b) from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`, raw data in
`archive:results_raw_2026-10-19/` (and `archive:results-raw/`). Old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`. Labels: **m** measured,
**mod** modeled, **a** assumed, **plan** = the C code's own `MN_PLAN_ONLY` decision (exact for the code at predicted sizes).
Times Eastern.

### Topic summary

- **Why more than three primes.** Three primes (p0 p1 p2) hold a product piece of at most 58 424 467 928 terms (pa + pb, 18-digit
  limbs). The 576-node target forms pieces up to 1.08e12 limbs, so three primes cannot run it (`MN_PLAN_ONLY=4.25e13:576` refused: 84
  of 1244 products over the bound, the first at tree level 5, 1.01x over; even a min(pa, pb) test leaves 46). §P15.
- **Current state (see ecalc/README.md; D18, 2026-10-12, results/MODEL.md).** At size 1 `ECALC_NP` is 3; unset at size > 1
  it becomes `auto`, plane pool 0 holds four planes in every multi-node run, and **every multi-node product runs four primes in the
  24-digit P24 form** (`mn_p24_of`; the former `MN_P24=1/2` switch is built in). The pre-D18 three-prime multi-node path (`ECALC_NP_MN3`) was removed 2026-10-19 (D56). `ECALC_NP_AUTO_MIN` (min(pa, pb) switch-over, §MPB15) was removed 2026-10-20 (Q53); `ECALC_NP_AUTO_TERMS` (test knob) remains; the
  `auto` per-product switch-over of §NP15 now matters only at one node-process (the `ECALC_NP_MN3=1` path is removed, D56).
- **Digits.** Every variant is exact by construction and was byte-identical in every test (mnaccept, 1e8/1e9, 4e10, 1e11, 2^34
  transforms). Roots above 2^33 were broken (w = 1) and fixed with no switch (§P15).
- **P24 measured value.** 10^10 at 2 processes 158.1 -> 97.0 s (m, -38 %, §P2415); 2 x 6.441e10 digits on 2 nodes on the aac7 line
  -8.87 s [-13.48, -4.26], p 0.009 (m, §MEAS-P24); at +8.6 GB per APU (+34.4 GB per node) of pool memory. Per-point surcharge x1.025
  (m, n = 2). Modeled target gain at cache 0 -88 to -113 s (mod).
- **Four vs three primes per product** costs x1.29-1.31 (m, §NP15); the grids and plan steps do not depend on the prime count (the cap
  is in points).
- **Strategy (Phase 13).** For one dependent product prime-per-APU (B) is the fastest at 2^20..2^32 points; the boundary is memory not
  time (§S13). `RNS_STRATEGY=auto` (B where its planes fit the pools, else C; the grid prefers B-fitting pieces) is the default since
  Phase 13c; B4 was slower and was removed 2026-10-12 (§B13b). The plane cap default is `run` (`results/MEMORY_IO.md`).
- **Superseded:** the planning tables in §P2415 §1.4/§4.2 and §NP15 §4 (modeled targets of 2026-09-28; later target figures are in
  results/MODEL.md); §P15's option (a)/(b) discussion (resolved: `auto`, then D18); the 2^35-cap option (c).

### §P2415 — four primes at 24 digits per transform point in the mn tier (Phase 15 Batch 3, agent P24)

**Open issues at close.** Cache off for P24 products (later cache work: results/TRANSPORT.md); mem_model had no P24 term; the layout did not
see mode 2 (harmless, <= +0.04 GB); surcharge measured at one configuration; B's gathers run per prime; 1e11 size-1 and ckpt/recheck with
P24 on not run then. **Decision/origin:** adoption later by D18 (2026-10-12).

### §P15 — roots above 2^33 and the prime bound at the 576 target (Phase 15, Batch 1 from L8's findings)

**Open.** The run's own check `ec_np_check(pa + pb)` is conservative up to 2x (tightening would not rescue three primes at the target);
a prime set with 15 | p - 1 would change `ec_G`/tables only.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/PRIMES_STRATEGY.md`.)

### §MEAS-P24 — four-prime product form (P24) for all multi-node products against the default, 2 nodes

**Results (m).** A totals mean 133.7 s (131.68-138.71); B mean 124.8 s (122.81-127.26); round differences -12.47, -7.89, -5.58, -9.55;
**mean B - A = -8.87 s, 95 % CI [-13.48, -4.26], 4 pairs, p 0.009** (stop rule met). Stages (A / B, s): binary splitting 43.1 / 40.3
(-2.7), dm 52.9 / 44.4 (-8.5), init about 34 / 36.5. Device memory pool 0 at `[init]` per node (per APU): in use 317.8 (79.5) -> 352.2 (88.1)
GB = **+34.4 (+8.6)**: planes +17.2 (+4.3), block pool +17.2 (+4.3); host RSS unchanged (HWM 28.9 GB); the peak was not extracted.

**Verdict.** P24 is faster at 2 nodes (-6.6 %), at twice the earlier modeled +4 GiB per APU. The target uses P24 from 32 nodes; whether
the target is memory-bound decides the trade. D18 later made P24 the multi-node default and kept `ECALC_NP_MN3=1` as the fallback (removed 2026-10-19, D56); the
gate compares digits at 10^9 (results/CODE_TOOLS.md §AUD3).

### §B13b — Phase 13b, agent B: strategy B in the library, and B4 (PLAN §31, axis S)

**Open at close.** Size > 1: the mn tier's products stay C (a prime-per-APU form there would need a cross-node operand broadcast at 25-50 GB/s
per APU vs 51 over xGMI; not built; node-local products at size > 1 take the strategy); the grid cost may prefer 2^31 pieces under auto
(S13's E2); B4's transfer as an overlapped push (modeled no gain).
Sections moved to `archive:results_merged_2026-10-24/PRIMES_STRATEGY.md` (2026-10-24 cleanup): §MPB15; §S13.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/PRIMES_STRATEGY.md`.)

### §P24 audit — the P24 CRT bound end to end, and the P24_CHECK diagnostic (lane L2, 2026-10-27)

**Verdict (code reading; nothing run).** The bound holds for every product kind **if every input limb is below 10^18**. A fired flag therefore cannot be a marginal bound violation.
- Bound: a coefficient sums at most min(Pa, Pb) products of two points below 10^24, so c < min(Pa, Pb) (10^24 - 1)^2. Plane cap **at most 2^40** points (`P24_CAP_LOG`; the actual cap is `mn_logn_cap(g)` = min(`dist_logn_max`, pool_log) + floor(log2 g) - 2 under VPE, so 31 + 9 = 40 at 576 nodes in layout D, 38 under VPE, lower with pool_log 30; so the bound only gets better with a smaller cap; 576 nodes: 40 with or without `MN_NONPOW2_CAP`: the next power of two, 2^41, does not fit q at 576 nodes) gives Pa + Pb <= 2^40, so min <= 2^39 = 5.50e11 and **c < 5.50e59 (code: 1.82x below 10^60); c 10^12 < 5.50e71 < 10^72 (same margin)**. The primes' product M = 1.511e62 (code), 276x above the worst c, so Garner is exact. `P24_MIN_MAX` (10^12) and `chk24` in the planner are looser than the cap (1.82x) and never fire; the cap is the real limit.
- By product kind: full / low product (pieces skipped, shift only moves the output window) / low cut (whole piece still formed; the cut skips pieces) / `mdb_view` operands (limbs clipped to the view; limbs past the view read as 0 in `k_gather_mn24`, `tend`) / grid pieces (each piece its own plane, `pa = ceil(na / ka)`, accumulated across pieces only after its CRT: the sum is never inside a coefficient) / NONPOW2-capped grids (cap and plane shape from `mn_shape`, same 2^40 limit; rows 455 / 456 per PE at 576 nodes, p24_run periodic in j mod 3 for any R, a): the inputs are the stored limbs, so the premise is only "limbs normalized". Nothing in the P24 path shifts or truncates an operand. At 3e13: operands about 1.67e12 limbs = 1.25e12 points each (code), so a grid of pieces with Pa + Pb <= 2^40 (census: 72 P24 pieces at 2e13 - 3.82e13); 3.82e13: 2.12e12 limbs = 1.59e12 points each, same cap, same bound. The bound is independent of the digits count.
- The flag's own semantics (found, code; the all-shift flag below became the default 2026-10-27, Q130, switch `P24_CRT_ANY`, =0 = this old flag): the old always-on flag is bit 1 = the carry out of c 10^s past four limbs, which at s = 0 and 6 needs c >= 10^72 / 10^66, above M: only s = 12 points (a third of them) flag at c >= 10^60, and the spill carry is bit 2. A **valid-range** error (stale or wrong limbs below 10^18) gives a correct-looking product with wrong digits and raises no flag. A flag needs a residue set that is no convolution of in-bound inputs: then c is a random value below M, which is >= 10^60 with probability 99.3 %, so an s = 12 point flags with near certainty. So the flag means (1) an input limb >= 10^18, (2) a corrupted or misplaced plane in the transform (a wrong exchange, twiddle or layout, in any of the four primes), (3) a stale or mismatched transform-cache plane, or (4) an index or kernel defect in a range no completed run reached.
- Cache (grid products only; P24 caches only inside `cache_pc_take`'s partial slots, keys reset at every grid product and at `cache_pc_give`): key = (mdb, offset, length, q, np, P24). `q` fixes the plane shape (`mn_shape` is a function of logn and q differs between logn), so an equal key means the same limbs and the same layout; no stale reuse found by reading.
- Over-commit: pool allocations come from the pre-mapped arena; growth inside a phase is `hipMalloc` and a failure is `mem_oom` (fatal, clean). An over-committed APU is documented in this tree to slow down (8x on aac7 once) or lose a process, not to return wrong data; nothing in the code path turns an allocation failure into a silent wrong value. Not testable on aac7; `P24_CHECK` does not see it.
- No defect found (T1d: nothing to fix). Open: (4) can only be seen at the target scale.

## APUMULT — the apumult study (internal reference multiplier) against ecalc

Owner of the items below (condensed 2026-10-19; full originals in `archive:results_merged_2026-10-19/`, raw data in `archive:results_raw_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Labels: **(m)** measured, **(mod)** modeled, **(a)** assumed.

### Topic summary

apumult is a host-resident reference multiplier/e pipeline (spec "a37v1") that reached 2.35e11 digits per node (475 GB, SSD spill, madvise) but is 3.2-3.7x slower per digit than ecalc at 4e10 (234.8 s against 63.5-73.1 s (m)). Its doc claimed that porting its spill/madvise stack would lift ecalc's 576-node target from 4.0e13 to about 1.3e14 digits. The study checked every apumult technique against ecalc `main` of that date and measured the disk and memory behavior of the APU. Conclusions that still hold:

- **ecalc already has (or beats) nearly every a37v1 technique**: same four RNS primes, batch tier with all primes on the owning APU, fused pointwise/L^-1/Garner+carry, no bit reversal, correction-form anchored Newton, band-cut products, view-based arena. ecalc's 2^31 forward NTT is 99-100 ms (m, `archive:results_merged_2026-10-24/KERNELS.md §K13b`) against 144 ms; one-node 4e10 is 63.5 +- 1.5 s (m) against 80.6 s. Of 139 rows about 80 are the same or equivalent and 14 ecalc is ahead.
- **The "3.4x digits" headline does not transfer.** Memory is device-resident with one arena per APU mapped at init and sized by `dm_layout`; `db_free` returns bytes only to the pool, there is no madvise on hipMalloc memory (EINVAL on a PFNMAP VMA), and the node peak is a mapped capacity. A buffer lowers the ceiling only by shrinking `dm_layout`'s formula. Modeled one-node ceilings: 1.30e11 now, 1.68e11 with a P spill plus tail policy (V2), 1.70e11 with V3 (+Q spill, late r2) at the 2^31 cap (2^30: 1.44 -> 1.79 / 1.88e11): +30 %, not 3.4x; apumult's 2.02 B/digit is below ecalc's V3 floor of about 2.3 B/digit. At 576 nodes spilling buys only +5 % (the tree top level, 294 GB, binds next); the built chunking switch (`MN_T_CHUNK_MB=1024`, off) is cheaper (5.32e13 mod). The apumult doc's claims about ntt/ were mostly stale or wrong (list under apumult_F1).
- **What was adopted from the study (current code, see `ecalc/README.md`)**: `DM_TIGHT` (tight reciprocal reservation, per-pair release in the device tier's top levels; was F2 item 1, "E2"), `DM_TAIL_DEAD` (tail re-laid over dead inputs, F1's tail policy), `DB_POOL_VMM` (HIP VMM arena; also removes the contiguity/fragmentation failure), `BS_SEED_FILL`, `BS_BATCH_PIPE` (default 1 since D20; was A37CMP R5, `results/COMPUTE.md §N3`). Not adopted: any SSD spill of P/Q/mu (only the checkpoint writer `ECALC_CKPT_TOP`, off by default), the OOM-killing watchdog, madvise, Karatsuba over the device grid (idea only, a time-for-memory trade), FWD_FUSE, "P20" 3-prime pack, base 10^19.
- **Rejected on principle**: skipping the remainder check (T1 residue verification is the only verifier at 3.7e13 digits), Stirling-balanced leaves (a node's term range spans < 3 %), global base 10^19, XNACK=1 (2x slower, RESULTS §55), CPU school tier, arenas on two dies.
- **Disk/APU facts (apumult_M, apumult_R)** that any future spill needs: the page cache lives in the same HBM; use O_DIRECT through a pinned hipHostMalloc bounce ring (O_DIRECT from a hipMalloc pointer fails with EFAULT); `hipMemGetInfo` is useless as a free gauge (use /proc/meminfo); overcommitting hipMalloc OOM-kills the process; madvise(DONTNEED) on host memory frees HBM for hipMalloc immediately; true out-of-core multiplication is hopeless (SSD/HBM bandwidth 1/2000-1/7000 against y-cruncher's needed 1/4).
- **Superseded/stale items**: the apumult summary's table of ecalc numbers (7e10 d_max, 73.1 s, 6.3 B/digit, 121.5 GB pool) is superseded by the F1 section 7 check; F2's per-doubling "used-at-peak" fractions were assumed and are covered by the later `DM_TIGHT` measurements (results/COMPUTE.md); A37CMP R1 (GPU seed) is an open candidate, not adopted (check `archive:results_merged_2026-10-24/INDEX.md` for the seed items before starting it).

### A37CMP: apumult a37v1 spec against ecalc, item by item

**Open questions (measurement needed).** Q1 does the seed (about 22 s (mod) at 6.44e10) bind init at 576 nodes (only data: SC rehearsal at 9.17e10, seed ends 31.75 s vs mapping 30.62 s, aac6; settle with a single-node `ECALC_INIT_TL=1` run on the target); Q2 seed stream handoff stalls; Q3 XNACK retest (only if pinning cost shows); Q4 pass-level NTT breakdown (HBM floor 51 ms (a37v1) against ecalc 99 ms); Q5 FMA contraction in `k_b16r` ISA; Q6 SHMEM pool pre-touch; Q7 what `dc` is; Q8 reciprocal chain floor and wait; Q9 fresh by-phase batch breakdown at 6.44e10 (last full one at 9.5e10: 25.1 s) before trusting R5/R7/R8 (all scaled from 4e10 data).

### AP34 — the apumult A1-A34 optimization list against ecalc (2026-10-23)

Read-only review of a later apumult list (34 items, "optimizations not present in ntt") against `main` 4b5bb14 and the
verdicts above. No code run; numbers are quoted with their labels. Verdicts: **new** (a TASKS "Proposed" row),
**have** (ecalc already does it), **closed** (measured or rejected before), **n/a** (does not apply to ecalc's design).

| item | apumult technique | verdict | why |
|---|---|---|---|
| A1 | GPU seed kernel (512 products per leaf) | new (re-gate) | A37-R1 closed NO-GO because mapping bound init under the node-process layout (S22, +0.55 s); one process per GPU maps about 7x faster, so the seed may bind now: TASKS row |
| A2 | Stirling-balanced leaf boundaries | closed | rejected in A37CMP (#20); `BS_SEED_FILL` already sizes spans from `lgammal` per run |
| A3, A7, A10, A32 | M-2 extent recurrence, d-scaled margin, per-level extent probes, single-pass helper | n/a | regions are laid out by an exact model (`mem_model.py --check-c`, within 0.6 GB measured) on VMM arenas that remap; the open margin lever is TASKS `BS_ARENA_ROOM` |
| A4 | ping-pong dual arena | have | two level pools of alternating parity (ALGORITHM §3, §8) |
| A5 | Newton truncation 2j+3 | n/a | ecalc's correction form takes 2j+2 limbs with the guarded anchored chain (error ≤ 8 units, results/COMPUTE.md §RECIP); wider is not faster |
| A6 | Barrett tail high product with skipped sub-products | have | high products skip pieces below the cut (`rns_mul_high_db`, `grid_piece_skipped`), the reciprocal's products are band/high cuts |
| A8 | budget-gated background checkpoint | n/a | the only checkpoint (`ECALC_CKPT_TOP`) is off by default; the output writer is already hidden under the division |
| A9, A18, A19, A20, A33 | adversarial kernel harness, gate ladder with fault grep, soak, T2 windows, gate-removal audit | n/a (method) | not performance items; ecalc has TESTING.md gates, T1 residues on 8 primes plus T2 windows, the switch registry audit; the soak gap is TASKS "G2 soak" |
| A11 | P and Q arenas split over dies | closed | rejected (#24: local HBM 3.8 TB/s against 93 GB/s remote, m); ecalc already spreads numbers over the four APUs (`dbig` quarters, regions) |
| A12, A13 | zero-copy root P, Q and A as arena views | have | P, Q pass to the block pool without copy; A = S·B^dl is never formed |
| A14 | asynchronous pinned-staging pre-grow | closed | init reorderings K8 each lost 2.0-11.5 s (m); remaining start-up items are TASKS "Default start-up time" and "Start-up shortening" |
| A15 | three-tier dispatch thresholds | new (probe) | school tier measured and removed (loss 42-59 s, m); the batch/device boundary (`BS_MDEV_LOGL`) was never timed: TASKS row |
| A16 | shared operand's forward transform reused by the second product | new | the batch tier pairs products that share an operand, but the device top levels and the tree levels transform Q_B twice (the transform cache empties after each product): TASKS row |
| A17 | two-term reciprocal modmul, one correction | have | `mm_lazy` (ALGORITHM §4.2); apumult's g3r3 is the slower form |
| A21-A24 | per-node leaf ranges, log2 P reduction tree, β-transpose and c_top·log2 P models | have | node ranges and `MN_GROUPS` tree (ALGORITHM §3, §6.5); `estimate.py` / `mn_model.py`; ecalc's measured multi-node cost is the exchange, not c_top |
| A25, A26, A27 | fused CRT + carry, added operand fused into the CRT, CRT writes the result in place | have | `k_crt_batch` / `k_crt24` with LDS carry-lookahead, P₂ as the added operand, results written into the destination `dbig` |
| A28, A30, A31 | `HSA_XNACK=1` hipMalloc arena; SDMA and GPU-write hazards under XNACK | closed / n/a | XNACK=1 was 2x slower (1e10 80 -> 159 s, m); with XNACK off the two hazards do not arise (GPU writes go to device memory or registered pinned pools) |
| A29 | 101 GB usable per die | have | measured device edge 93.36 GB per APU, `ECALC_DEVICE_GB` budget and the memory fit (ALGORITHM §8) |
| A34 | ω derived from an order-2^44 root | have | `ec_root` derives any 2^logn up to v2(p - 1) = 44 from the generator; `tests/t_roots` checks order and the root chain |

Side finding: ALGORITHM §4.5 still said the transform cache served the reciprocal's last doubling and the division's X·Q
(stale since the held slots were removed 2026-10-11); corrected in the same merge.

**Measured follow-ups (2026-10-23, aac7 item 9zu4, main 53778c7, 1 node, 1e11 digits, every run VERIFY OK).**
- A1, start-up timeline (`ECALC_INIT_TL=1`), layout D, 2 runs: plane pools done 7.5 / 6.2 s, last background mapping 37.4 / 37.9 s,
  seed thread ends 51.5 / 52.0 s (seed − mapping +14.1 s both), bs level 1 starts 52.0 / 52.4 s; totals 142.3 / 139.2 s (init 38.5 / 38.9,
  bs 61.6 / 56.5, dm 42.2 / 43.7). Layout P (`ECALC_PE_PER_GPU=1 RNS_BATCH_SPLIT=1`): seed threads end 21.7 / 19.9 s, mappings 24.0 / 13.5 s,
  but both runs stopped at the host budget check (rc 8) before bs, so P has no full run. Without `RNS_BATCH_SPLIT=1` the planner refuses
  P at 1 n at both 1e11 and 5e10 (item 9zu3).
- A15, `BS_MDEV_LOGL` 28 / 29 / 30 / 30 (layout D, one run each): totals 135.07 / 127.45 / 136.89 / 131.62 s; bs 61.3 / 57.0 / 57.1 / 56.5 s.
  The batch tier is faster than the device tier at the levels that move (level 22: batch 1.80 s vs device 3.87 s; level 23: 2.86 vs 3.83 s),
  so 28 loses about 4 s of bs and 29 equals 30 within noise; the default stays (OPPORTUNITIES, rejected).
- A1 under P (Q93 rerun, aac7 item 9zu5, main 1e2ea1b, 1 node, 7e10 digits, P = `ECALC_PE_PER_GPU=1 RNS_BATCH_SPLIT=1`; 3 P runs and
  2 D runs, alternating, the third D run cut by the item's time cap; every run VERIFY OK, same sha). Seed thread ends: D 33.7 / 30.9 s,
  P 19.3 / 21.7 / 19.3 s; last background mapping: D 25.5 / 21.6 s, P 28.8 / 13.9 / 17.0 s; seed − mapping: D +8.2 / +9.3 s,
  P −9.5 / +7.8 / +2.2 s; bs level 1 starts: D 34.0 / 31.2 s, P 29.2 / 22.2 / 19.8 s. So P's per-APU seed threads start bs about
  10 s earlier, and in P the mapping, not the seed, binds in one run of three. Totals: D 207.1 / 198.0 s, P 444.5 / 373.2 / 369.9 s
  (bs 38.7 / 37.8 vs 98.0 / 58.0 / 58.4; dm 27.8 / 20.0 vs 107.1 / 63.7 / 63.4; recip 5.4 / 4.4 vs 25.8 / 16.1 / 16.0), so at one node P is
  about 1.8x slower overall and the start-up gain does not matter there; P is the target-shaped layout, not the 1-node one.
- BS_SEED_NUMA (Q92: the layout-D seed by four per-APU workers pinned to their NUMA node, as P seeds; aac7 items 9zu6, 9zuf-9zuj,
  1 n): 1e9 digits identical; at 1e11, alternating 1/0/0/1, seed ends 56.7 / 41.7 / 49.2 / 40.1 s, bs level 1 starts 57.1 / 42.4 / 49.9 /
  40.4 s, totals 145.4 / 133.4 / 137.9 / 130.8 s (init varies 26-43 s between runs). In every run the seed ends 13.8-14.4 s after the
  region pools exist (the pull kernels' launch): the seed waits for the pools, not for CPU speed or NUMA placement. Dropped (Q94 (b)).


Sections moved to `archive:results_merged_2026-10-24/APUMULT.md` (2026-10-24 cleanup): apumult_summary; apumult_F1; apumult_F2; apumult_M; apumult_R.

### AP34-reuse — RNS_PAIR_REUSE: Q_B's forward planes kept for a pair's second product (A16; branch `reuse` babb434, 2026-10-25)

Switch `RNS_PAIR_REUSE=1` (default 0). `binsplit.c` arms Q_B (`rns_pair_arm`) before P = P_A Q_B + P_B and disarms after Q = Q_A Q_B; in `rns_dist.c` the first B-form product of the pair takes Q_B as its Y operand (`ntt_inv_pw` only reads Y) and records Y's forward planes in the plane pools; the second product skips Y's load and forward transform when the transform length, primes and operand match (else it runs as before). No plane memory is added (Y's plane is the pool's own). Any other product (C form, mn tier, other operand) drops the kept planes. Scope: the one-node dist tier in the B form (path 1, layout D). Not done: the C form, grid products whose B pieces differ, the vPE tier (path 2, `ECALC_PE_PER_GPU=1`: the switch is a no-op there), and the multi-node tree levels (`mn.c tree_level` / `tree_level_k`: their planes live in `mn_core`'s own pool slots, a separate change).
- Digits (m): VERIFY OK in every run with the switch on, 1e9 (BS_MDEV_LOGL 20 and 22), 1e10 (24, 26), 4e10, 7.7e10; with `RNS_STRATEGY=C` the switch does nothing (0 kept). Counts (m): every pair's second product hit (1e9, LOGL 20: 56 of 56; 1e10, LOGL 24: 34 of 34; 7.7e10: 5 of 5 per run). Gate a2g_reuseg on babb434: PASS (1 n + 2 n both paths, unit 9/9, plus 1 n switch-on gates on both paths; coordinator, 02:26 ET).
- Time (m), 1 n 7.7e10 layout D (1e11 is refused by the 1-node plan: device 386.7 > 373.4 GB), item a2b1_reuseab, 4 NIC-free nodes, 3 on/off pairs per node in ABBA / BAAB order, on - off: bs -0.54 s (95 % CI [-0.87, -0.20], off 41.34 s); dm -0.12 s ([-1.44, 1.20]); total -0.33 s ([-4.04, 3.39], off 91.5 s, nodes differ by up to 9 s). The bs gain is real but small; the modeled 2-4 s at 1e11 (assumed) was not reached because only the one-plane levels reuse (about 5 pairs per run) and the top grid level does not.
- Next: reuse inside the grid (B piece j for the P and Q grids), the vPE tier, the tree levels; the division step extension.

## DIVISION — the distributed division and the reciprocal

Scope: the distributed division and the reciprocal: Newton error, DKM halves, middle product, band-sized products, the low product, rebalance. | Owner of the items below (condensed 2026-10-19 from the merged per-item reports; originals in `archive:results_merged_2026-10-19/`; old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`.

### Topic summary

- **What this is.** How the quotient e = P/Q-style division at the end of the binary-splitting phase is computed (`newton_db.c`: `newton_mn_divmod` over shares at size > 1, `newton_db_divmod_shifted` at size 1): Newton reciprocal by an anchored doubling chain, a high product A_h·μ, a low product X·Q mod B^w for the remainder, and a correction loop (≤ 64 steps, fatal beyond; digits are exact whenever a run completes, the corrections make X exact). dm is the largest multi-node cost (+94 s at 6 nodes vs 1, m; §P1).
- **Current state on main (ecalc/README.md, RESULTS.md, `switches.tsv`).** Built in and no longer switches: DKM is the only division (§DKM15: dm −25.7 s at 10¹¹ one node, m), the reciprocal's cut and middle product (§R114 E7, §R415; −3.4 s of the reciprocal, −3.2 s dm at 10¹¹, m). Defaults: `MN_OUT_DKM_HI` 1 (§EW15, writer starts on X_hi after step 1), `ECALC_CORR_PATCH` 2 (§K15), `NEWTON_CHAIN_GUARD` 1 (user decision D25, 2026-10-13; §RECIP), `DM_MN_WRAP` automatic by the memory fit (D26, 2026-10-13; §P1), `DBIG_SMALL_BOUNCE` 1 (2026-10-12; §O2), `DB_POOL_VMM` and `DM_TIGHT` derived by the planner (§R114). Dropped / not built: B5 (§O5, no gain, removed from the code), E9 Karatsuba and E10b disk spill (§R114, rejected by the model), E11 `DM_BAND` and the band-sized stores (§M615, stopped), B2 transform cache (§A-div, not implemented).
- **Live conclusions.** (1) The products of the division are exchange-bound; fewer transforms for the same information is the lever (cyclic low product §P1: −7.7 s of dm at 2 nodes, m; DKM: one doubling fewer). (2) The reciprocal's error is O(M) units with M = B^n_Q/Q ∈ [1, B]; the `t_newton` bound is `recip_bound`, not ±8; runs of exact doublings (k = 2^m) square the error and can make DKM step 1 fatal, which the guard limb removes (§RECIP). (3) The exactness of every cut rests on the correction loop or on a reduction mod B^w; read the argument in the item before changing a cut. (4) The band product (`rns_mul_band_db`) is bit-identical to the cut path because the dropped pieces are multiples of B^w (§R415). (5) Memory: at the 576 target the dm need binds (244.7 GB arena, tree floor 224.8, at most 19.9 GB per node saveable, mod, §M615); DKM saves node memory only with a DKM-aware `dm_layout` (−5.1 GB, mod, §DKM15). (6) The first-copy pinning stall (8 GiB of pinned bounce buffers) cost 2.2 s per node and up to 10 s on the slow rank of the reciprocal's gather (§O2).
- **Superseded / historical.** A-div's flow and tests are the base; the host-A `newton_db_divmod` is not production; EW15's and K15's 10¹¹ timing series are one-node results on aac6; §A-div's "X gathered to node 0" and "low product over shares is the full X·Q" were replaced by A-out and the grid over shares. The §4.3 packed tail-patch defect found in §DKM15 was a bug of main fixed later (see `results/MEMORY_IO.md`).
- **Open (per item):** 2-node gate and ABBA for `NEWTON_CHAIN_GUARD` were closed by its adoption; the 6-node ABBA of `DM_MN_WRAP`; the 2-node `ECALC_VERBOSE=2` measurement of the mn dm peak (§M615); the part-file count 2·size under `MN_OUT_DKM_HI` for target scripts (§EW15).

### P1 — the distributed division multiply: the low product X·Q as a low part + one cyclic plane (`DM_MN_WRAP`)

**Where the time goes** (6.441 × 10¹⁰ digits per node; S48, O1 r12, one instrumented 2-node run; m):

| nodes | dm | reciprocal (its products) | division products | X_lo·Q | shifts (count / s) |
|---|---|---|---|---|---|
| 1 | 22.5 | 6.0 | — | — | — |
| 2 (S48 r1 / P1 i1) | 61.3 / 55.4 | 18.4 (13.4) / 13.6 (11.8) | 40.5 / 39.8 | 15.8 / 17.9 | 85 / 3.4–3.5 |
| 4 | 76.9 | 22.0 (16.4) | 51.8 | 17.6 | 91 / 4.9 |
| 6 | 117.3 | 25.9 (21.0) | 88.7 | 31.1 | 94 / 5.1 |

The division's products are 72–76 % of dm at 2–6 nodes and the two low products X·Q are the largest part (≈ 10.8 + 11.7 s of 39.8 s at 2 nodes; ≈ 26 + 31 s of 88.7 s at 6). Grids: X·Q is 2 × 3 pieces of 2³² at 2 nodes (6 formed), 2 × 7 of 2³³ at 6 (13 formed); A·μ 2 × 2 (3 formed) and 2 × 4 (7 formed). A 2³²-point piece takes 1.9–2.2 s (1.5–1.7 s with the outer transform cached), the all-to-alls being 0.77–1.14 s of it: **exchange-bound**. The partial transform cache gets fewer primes in the second low product (1 of 3 at 2 nodes, 2 of 3 at 6; ≈ 1–5 s per division, mod). The grid shapes are already best (skip-aware search at 2, 4, 6, 576 nodes, mod); the lever left is fewer transforms.

**Correctness** (m; `tools/gate.sh --build` 10⁹, aac7 items `72_p1_gates2`, `77_p1_gates3` after the rebase on f08a4ea): all PASS (VERIFY OK, digits identical): off, `DM_MN_WRAP=24` (h > P, multi-chunk folds), `=21`, `=22 DIST_GEN=1`, `=24 NEWTON_DKM_TEST_HI=±3` (up-corrections, R < 0 branch), `=24 ECALC_TEST_CORR=2`, `=1` (no wrap at 10⁹), 1-node variants; after the rebase the wrap ran with P24 (cyclic plane in 18-digit points).

**Timing** (paired ABBA, 2 nodes, 1.2882 × 10¹¹ digits, no digit output, aac7 launch line, A = default, B = `DM_MN_WRAP=1`, branch 13aa201, 3 rounds ABBA, p < 0.05 stop rule fired after round 3, all 12 runs VERIFY OK; m):

| quantity (s) | A mean (6) | B mean (6) | B − A, 95 % CI (3 rounds) |
|---|---|---|---|
| `total` | 140.4 | 129.4 | **−11.1 [−19.1, −3.1], p 0.027** |
| dm | 54.5 | 46.8 | −7.7 [−10.3, −5.1] |
| division products | 38.5 | 28.1 | −10.4 (means) |
| shifts (count / s) | 85 / 3.45 | 97 / 5.90 | +2.45 |
| step 2's X_lo·Q + corrections | 15.5 | 11.3 | −4.2 |

`total` moves more than dm because bs varies (43–50 s). Memory (m): block pool at the step-2 low product 57.98 GB per APU under B vs 53.69 under A (+4.3 GB per APU), VmHWM unchanged (28.9 GB). At 6 nodes (mod): ≈ −8…−11 s of dm (−15.6 s of products less ≈ +5…+8 s of shifts); 576 nodes −39 % of X·Q transform volume (mod, seconds not modeled).

**Verdict.** At 2 nodes saves 11.1 s of `total` (CI 3.1–19.1, m), 7.7 s of dm, digits identical in every gate; costs +4.3 GB per APU of pool at 2 nodes, not in the memory model (`dm_layout`, `mem_model.py` must count Q″ and the fold accumulators, basis P + 4, before target use), not timed at 6 nodes or with P24. Switch stays off; merged to main; next (user's call) a 6-node ABBA (≤ 4 rounds).

### RECIP — the Newton reciprocal's error: the t_newton bound or the reciprocal?

**Answer.**
1. **The −22 is the bound's fault.** The ±8 assumes a normalized Q; with decimal limbs one unit of μ is relative to M = B^n_Q/Q ∈ [1, B]. SIMP's Q has top limb 2.349·10¹⁶ (M = 42.6) and its last doubling 32769 → 65538 is exact (no limb cut afterwards), which keeps its rounding error, at most 2M + 3 units; −22 = −0.52·M. A host GMP model of `recip_db2` with exact products reproduces −22 bit for bit and SIMP's three rng5 errors (+1, +0, +1).
2. **A real weakness the test never reached.** Several exact doublings in a row (⌈k/2^i⌉ even for consecutive i, e.g. k = 2^m) square the error left by the previous one (ε → −ε² + rounding): modeled errors up to 10¹²–10¹⁹ units (one limb) for some Q with M ≈ 1, 10³⁵ (two limbs) at h = 2048. The division's corrections absorb ~64·B units; beyond that DKM step 1 dies with `DKM step 1: N corrections` (fatal; never wrong digits). Modeled: for a 4095-limb quotient (h = k₁ = 2048) 3 of 900 step-1 estimates are off by > 64 (up to 1.8·10⁸). Production exposure (mod, order of magnitude): top run of ≥ 8 exact doublings for ~0.4 % of k; fatal needs a long run, h = k₁ and an unlucky Q, ≈ 10⁻⁵ per run size; at 10⁹…10¹² digits h is odd (top run 0).
3. **Fix: one guard limb per level** (`NEWTON_CHAIN_GUARD=1`): targets k, ⌈k/2⌉ + 1, … so every doubling has jn ≤ 2j − 1 and is cut by ≥ 1 limb. Modeled error 0 in every sweep; ≤ 2 extra tiny doublings at the bottom (≤ 19 instead of 17 up to k = 2·10⁵), negligible. Digits cannot change (the division corrects to the exact quotient either way).

**Results** (mod unless marked): SIMP's Q: error −22 off, 0 on; sweep n_Q 1027, k 514 (one exact doubling) −174…+1 (exact products) / −479…+2 (cuts), max |err|/M 2.47, guard on 0; k = 512 (9 exact doublings) up to 3.2·10¹² (exact) / 7.3·10¹⁸ (cuts), guard 0; k = 1024 / 2048 up to 4.5·10¹³ / 1.1·10¹² (exact), ~10¹⁸ (cuts); odd k (513, 1027) 0; derived bound −(2⌈M⌉ + 6)…8 with no two exact doublings in a row: 0 violations in 2 × 3 × 300 Q; DKM step 1 (n_Q 8200, h = k₁ = 2048, 300 Q × 3 A): |X₁ − X₀| up to 1.8·10⁸, 3 of 900 beyond 64, guard 0 of 900; quotient 2047 / 8191 3 / 9 of 900 beyond 64; top runs 1…7: μ error 84…4.5·10⁷ units, X₀ within 1.

**Verdict.** (a) The −22 is a bound problem; the bound now scales with M (`recip_bound`: −(2⌈M⌉ + 6)…8 when the last doubling is exact, ±8 otherwise, none with two exact doublings in a row). (b) The reciprocal has a real, rare, fatal-never-wrong failure mode at long runs of exact doublings; `NEWTON_CHAIN_GUARD=1` removes it at no measurable cost (mod; 1-node gate passes, m). Changes no digits. Adoption is the user's. **Open:** the 2-node gate with the switch on (sharded chain `recip_mn_body`, `newton_mn_chain_start`) was not run, needed before a default flip; merge rebased onto SH3 + K4c (dfac293) not re-gated; `mem_model.py` does not mirror the +1 limb of `dm_layout`'s jl; the fatal case was not provoked through the division on the GPU. Merged to main (switch off); to adopt: 2-node gate `--env NEWTON_CHAIN_GUARD=1`, `mnaccept.sh --only unit,e9,mn`, a 1-node 10¹⁰ ABBA.

### R114 — R1 (Phase 14, W1): E7 the reciprocal's products cut to the band read; E9 Karatsuba over the grid (model); E10 the 576 tree-level spill (model)

**Open:** one 8-s wait of the seed thread's first launch behind a plane-pool hipMalloc (≈ −2 s more); host HWM with 2 × 8 GB seed buffers (27 GB at 10¹¹; re-check at 1.4 × 10¹¹); binary E2 and CPU schoolbook tiers unsupported; `T_ALLOC_VMM_SERIAL=0` untested; reserved VA 3 × arena (a run remapping > 2 × arena would fall back to hipMalloc, counted; not seen).

### DKM15: the division in two quotient halves with a half-length reciprocal (Phase 15 Batch 3, agent DKM)

**Open issues** (as of the item): the §4.3 defect needed a fix on main before adoption (then DKM's long-chain cases re-run at sizes 1, 2, 4); the plan printer (`mn_plan.c`) did not follow the switch; `dm_layout` did not know DKM (−5.1 GB); the X_hi writer (→ §EW15); size > 1 tested only on one node (loopback, sizes 2–4, 10⁸–10⁹), fabric terms assumed; a clean paired 10¹¹ series without the on_3 outlier would firm up `total` (the dm figure is firm).

### O2 - the reciprocal-gather stall: why `db_to_bi` of 0.2 MB takes 2.2 s (2026-10-11/12)

**Results** (aac7, 4-node hold item 70, 2026-10-11; m): gates 2 nodes 1e9 PASS off (total 20.50 s) and on (21.83 s), `t_dbig` on VERIFY OK (555); BOUNCE unit (27 302 limbs) `first_to_bi` 7.96–9.95 s off vs 0.35–0.65 s on, steady 0.0004–0.0009 vs 0.00006 s. 4-node paired AB/BA (A = default, B = on), 4 pairs, all VERIFY OK: A chain 9.1–12.5 s vs B 0.06–0.07 s, A `db_to_bi` max 9.1–12.4 s vs 0.016–0.018; **chain −10.98 s [−13.43, −8.54], p 0.0007; total −30.08 s [−38.59, −21.58], p 0.0015** (n = 4); the late rank in A is a different node each pair (+6.9…+10.2 s), pin time summed over ranks A 18.6–25.7 s vs B 0.06 s; the total gain exceeds the chain gain (dm 87–95 s vs 109–115 s) and is not attributed (assumed same pinning elsewhere; totals also drift down pair to pair in both arms). No memory shortage in either arm. aac6 (1 node): gate PASS off 12.57 s / on 11.99 s; `t_bounce` first `to_bi` 1.37 → 0.16 s (8.4 ×), pinning 0.25–0.38 s → 0.02–0.03 s. **6-node repeat** (item `o2b`, rev 2f8de94c, 4 pairs): chain **−6.22 s [−12.02, −0.42], p 0.042**; total −4.59 s [−26.49, +17.31], p 0.55 (not resolved; totals 247–268 s, in pairs 3–4 the A stall was small, 2.3 / 4.1 s); the 4-node total gain should not be quoted as general. **C4 / C5 re-time with the fix on** (item 90, 4 nodes, 3 rounds A / C4 = `COMM_SHMEM_RPIPE=1` / C5 = `MN_REDIST_FUSE=1`): chain 0.06–0.09 s in every run; total C4 +1.82 s [−14.35, +17.99], p 0.68, C5 −0.78 s [−40.89, +39.34], p 0.94; totals drift −35 s from round 1 to 3 in every arm; no measurable effect, neither supported (earlier C03 result stands).

**Verdict.** The switch removes the 8 GiB first-copy pinning stall (chain −6 to −11 s at 4–6 nodes, node dependent; total saving shown at 4 nodes only), digits identical on the gates. Open: the late-node mechanism inferred; big buffers still lazy on the first large copy (a later option: size by copy or pre-pin at init); a 2-node item `7c_o2c_two` was armed at the time of writing.

### O5: K7-a GPU-3 rebalance -- the B5 form (B4 with APU 3 given a share of the inverse)

**Verdict.** B5 is correct but gives no wall-clock gain (the inverse form itself 0.027 s slower, 0.03 s of an 8.9 s run). Not adopted. Open (historical): a lower balance than 2.25 n would need the forward split too; `k_b5_merge` could be fused into the last inverse pass.

### NPCAP — `MN_NONPOW2_CAP`: non-power-of-two groups on the next power-of-two plane (default auto since 2026-10-20, user Q58; items 9bf, 9bg, 9bj, 9bi, 9bo, 9bp, 9jf, 9ji, 9jd; m)

- **What it does.** A group of 3 x 2^k nodes takes the next power-of-two plane (rank plane 43/32 larger), so the division
  has 6 pieces instead of 22 at 1.85e11 digits (plan). Cost: plane memory per node 120.3 -> 161.6 GB (+41.3 GB, m) and about
  +25 GB of arena and v-slots (plan); the node fit refuses 3 n above 1.85e11 and 6 n above 3.70e11 with the wrap on
  (`DM_MN_WRAP=0` fits both).
- **Gates (item 9bf):** 6/6 PASS (1 n and 2 n default path; 3 n default against switch on at a lowered cap, libfabric and
  SHMEM, and at the default cap); digits identical.
- **Timing, libfabric.** 3 n 1.7e11 (item 9bg, 3 pairs): NPC 124.6 against L 132.2 s (-7.6 s; dm 40.1 against 51.0 s;
  init +4.6 s). 3 n 1.2e11 with the wrap forced (item 9bj): NPC 92.4 against 89.2 s (+3.2 s; dm -0.9 s; init +4.1 s).
  6 n 3.70e11 (item 9bi): NPC 175.0 / 174.7 / 169.3 s against L 202.0 (first run) / 186.3 / 185.6 s; means 173.0 against
  191.3 s (-18.3 s, -12.9 s against warm L); pair differences -27.0 / -11.5 / -16.2 s; dm 62.2 against 81.8 s; init +3.5 s.
  All VERIFY OK at 2^34-point pieces.
- **Init cost** is the larger plane pools mapped in rns_init: fixed, independent of the digits. Crossover about 4.5e10
  digits per node (mod, linear in the pieces saved).
- **6 n gates (item 9bo):** 4/4 PASS, digits identical (6 n default against switch on at a lowered cap, libfabric and
  SHMEM; switch on at the default cap).
- **Engine acceptance (item 9bp):** `mnaccept.sh --only unit,grid,e9,mn,ckpt,recheck,corr` with the switch on: 23 passed, 0 failed.
  Merged default-off after it.
- **Default (user Q58, 2026-10-20).** Unset or `auto`: on for a non-power-of-two node count when the digits per
  node-process reach `MN_NONPOW2_MIN` (default 4.5e10, the crossover); `=1` forces on, `=0` keeps the plain plane.
  `rns_np2cap`, the planner and `tools/mem_model.py` share the rule. Below the threshold it stays off: 3 n 1.2e11
  (4e10 per node, item 9jf) on 95.7 against off 93.2 s; an unconditional default cost +8.6 s at 3 n 6e10.
- **Node budget:** with the switch on, the standard 6 n size 3.8646e11 fits for libfabric (planner, mod: node margin
  +46.25 GB) and for SHMEM only with `COMM_SHMEM_ROUND_MB` 128 (+3.84 GB; -3.68 GB at 1024), the aac7 profile value since
  2026-10-20 (user Q60); see `archive:results_merged_2026-10-23/SHMEM_full.md`, PLAN16 section.
- **Defaults confirmed (items 9ji, 9jd, 2026-10-20; m).** Gates 1/2/3 n (auto off below the threshold, `=1` on) and 6 n
  (switch on, SHMEM and libfabric) PASS, digits identical. 6 n 3.8646e11 with the switch on runs for both transports:
  libfabric 174.2, 173.5, 174.7 s (mean 174.1); SHMEM 193.7 (first), 182.3, 184.0 s (186.7). 3 n 6e10 (libfabric): auto
  (off) 59.2, 51.8 s against `=0` 54.7, 53.6 s.

### LAZY — the reciprocal's Q_t as a window view (`NEWTON_MN_LAZY`, branch lazy, 2026-11-02; aac7 1-3 nodes, measured)

**Open (review 2026-11-02).** The window lies in the top shares of Q, so at the early steps (small `take`) only the highest nodes send in the operand exchange, where the old copy was spread over the group; 2-3 nodes cannot show whether this costs time or send-buffer room at 576 nodes. The planners still charge the shift (conservative).

**Open.** Time at the target size; memory planner unchanged (conservative). F-out stays parked behind the target measurement.

## GRID_GROUPS — the grid of piece products over shares and node groups

Scope: the grid of piece products over shares and node groups: layout at any group size, MN_GROUPS schedules, v-slot release, the 576-node layout checks. Owner of the items below; each section condenses the former `results/<ID>.md` (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers are in `archive:results_raw_2026-10-19/`. Labels: **m** measured, **mod** modeled, **a** assumed. Times Eastern.

### Topic summary

What this topic is: how a product of two numbers sharded over a group of g nodes is computed. The pieces are `rns_mul_dist_mn` (one plane per node pool, cap 2^(min(31, POOL_LOG) + log2 g) points), the grid of piece products above the cap (`split_grid_cap`, views, `mdb_add_shifted`), the block-cyclic map that lets every node of a group of any size transform (L), the tree on the `MN_GROUPS` schedule with k-way combines (G12), and the v-exchange slot memory of the general map (B7ACCT, S36, C06, S45).

Current state (main; cross-checked against `ecalc/README.md`):
- The grid over shares, the views, the general map for any g and the tree on the `mn_groups_parse` schedule are the code. Default schedule (README `MN_GROUPS`, G12 / Q's decision): powers of two that divide the size, then the odd part's prime factors ascending (576 -> 2, 4, ..., 64, 192, 576). The L schedule (powers of two, then the size) is `MN_GROUPS=2,4,...,512,576`.
- `COMM_LAYER_VSLOT_SHARE` (S36) and `COMM_LAYER_VSLOT_RELEASE` (C06) are built-in defaults since 2026-10-12 (README). One pair of v-slots per APU, shared by all layered communicators, and released per product under X2. The §B7ACCT / §S36 sums of v-slots per level are superseded: the resident term is the max, not the sum.
- `RNS_DIST_CACHE_MN` (transform cache over shares, G section) is built in with 2 slots. At size 1 the cache stays off (`RNS_DIST_CACHE=0`). The 3·2^30 plane (C5 in A-grid) was measured only; the radix-3 lengths now live under `RNS_R3` (README, default 1 when the prime set has radix-3 roots). The old `DIST_R3` switch is not in the README.
- `DIST_CHUNKS` is 8 (README); the §B7ACCT, §S36 and §S52 numbers use K = 8 (B7ACCT's table assumed K = 4 for the slot arithmetic of option rows).
- `DIST_COALESCE` (message bundling, §S52) was not built (user decision 2026-10-11) unless the target's per-message cost is shown >= 8.5 us.

Live conclusions:
- Memory is the limit at 576 nodes, not time: the real v-slot term (B7ACCT, 6.707 GB per APU before sharing) broke the device bar; SHARE (-5.79 GB per node, mod, S36/S45) and RELEASE (the transport pool 4352 -> 2304 MiB per APU (then comm_ofi's), C06) bring it back. Largest safe size at 576 is 3.82e13 (§S52, mod); above it the plan adds pieces (+20 s at 3.83e13).
- The cut-group pool-offset asymmetry (G5) does not bite (§C06); uneven last groups work (digits identical, §B06) but give no speed gain (§B06, §O1: `[6]` vs `[2,6]` at 6 nodes inconclusive, raw +2.4 s, CI [-3.9, +8.7]).
- The §C09 tables are modeled; the model-best uneven tops are unmeasured extrapolations.
- A bug found by forcing tree grids (`mdb_add_shifted` windows of the wrong nodes for g0 > 0, §G12 2b) was fixed before any run could reach it.
- §C215: the piece-cost grid (`RNS_AUTO_PIECE_COST`, C2) gains -31 s (-13 %) of `total` at 1e11 on one node; neutral at the 576 target (0.0 s, mod).

### B7ACCT — counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06)

Login-node work only (aac7, `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY`; C-binary prints are measured layout figures, the code's sizing, not memory readings). Branch `b7-vslot` pushed, not merged. Question: STD17's 10-node run showed about 3.3-3.5 GB per APU of memory the layout did not count; are they the v-slots of the layered communicator?

##### 1. Which exchanges take v-slots

- The only v-slot site is `comm_layered.c` `y_alltoallv` / `y_alltoallv2`, reached through the layered communicator `G->lay[d]` by the general four-step (`gen_fwd`, `gen_inv_pw`), run when `gen = !is_pow2(g) || DIST_GEN`. Other `comm_alltoallv` calls (redistribute, result, spills, `mdb_shift`, `mdb_add_shifted`, `newton_db.c`) use the plain mesh and stage in the comm_ofi pools (already counted by OFIMEM). Power-of-two groups' layered comm uses the caller's `tmp` (block pool).
- Slot formula (`need_vslot`): `COMM_ALLTOALLV_DEPTH=2` (default) two slots of A + max(A, B) + 4 bytes; depth 1 one area of 2A + B + 4. With A about B about q/K two slots hold 4q/K points = **q x 8 bytes at K = 4** (one full plane per APU). At 10 nodes at the cap q = 3277 x 131072 points: **3.444 GB per APU (mod, exact formula)**.
- Slots never shrink (`need_vslot` only grows) and the communicator is destroyed only in `groups_finalize`, so each general-map group keeps its largest slot from its first general product to the run's end. One group per node holds slots at each level; the whole-machine group is shared by the tree top, reciprocal and division. At 576 with `...,64,192,576` the general-map groups are 192 and 576.
- Resident per APU: 0 for power-of-two tree levels; a general level l = sum over general levels <= l of that level's slot; reciprocal and division = sum of lower levels + max(top, A_h mu, Q_t r) (superseded by SHARE/RELEASE: max not sum).

### C06 — G5 (X2 cut-group pool-offset asymmetry) and M3 (release the v-slots; S-1 remainder), 2026-10-11 (branch c06 = main bc80001)

##### Verdict
- **G5: it does not bite, on the target or the 576-k schedules; no code fix, no guard.** (1) The target schedule has no cut group. (2) The 576-k schedules with explicit `MN_GROUPS=2,..,64,192,<g>` do have cut groups, but the receiver publishes where each sender's slab lands (`comm_shmem.c` `publish()` -> W_ROFF; the sender puts at `UNOFF(roff)`; OFI the same), so members' pool offsets need not agree. (3) Host emulation: 0 wrong words in every run; offsets equal only for OFI pool + SHARE=1 (the target). The real-transport confirmation (offsets forced to differ with `COMM_LAYER_TEST_SKEW_MB`) was queued (item 70). Caveat: the claim rests on code reading + emulation; a cut group + non-SHARE + X2 can still fragment the pool (a clean "pool cannot hold" fatal, not wrong digits).
- **M3: implemented as `COMM_LAYER_VSLOT_RELEASE=1` (needs X2's `COMM_LAYER_VSLOT_POOL=1`); built-in default since 2026-10-12 (README).** Plan (m, C code, aac7 login node, 576 nodes, 3.71e13): comm_ofi pool **4352 -> 2304 MiB per APU = -8.59 GB per node device** (layout node 386.48 -> 377.89 GB; with `--check-c` exact both ways, node + vslots 400.14 -> 391.55 GB). At 3.82e13 with T2048 + X2 the rank-0 margin 4.3 GB (B01, mod) becomes about 12.9 GB. 4 nodes x 6.441e10 with `DIST_GEN=1`: pool 4608 -> 2560 MiB per APU. Saving = min(staging, slots) per APU wherever the general map runs under X2.

##### G5 detail
Schedules (exact arithmetic of `mn_groups_parse`, `archive:results_raw_2026-10-19/C06/cut_groups.py`): 576 (`...,64,192,576`) has no cut group; default schedules of 560..575 are exact too; explicit `2,4,8,16,32,64,192,<g>` for g = 560..575 has 2..6 layered cut groups (power-of-two cut groups use the plain mesh). The pool rule sets `COMM_LAYER_VSLOT_MB_<size>` for every layered size incl. cut sizes, all equal under SHARE (915..967 MiB), so under SHARE every group allocates the pair once at the same size. Host emulation (`ecalc/tests/lay_host`, aac6 MI250, 3 nodes, 7 shapes): equal pool offsets in every member of every level only for {OFI pool emulation, X2, SHARE=1} with cut groups (shapes 23, 47 (= the 575 shape), 35, 34) and the exact-group control (36); unequal for OFI SHARE=0 and the SHMEM-only pool. 0 wrong words, 0 live bytes after destroy. Without X2 no pool is involved.

##### M3 detail
Under X2 the v-slot pair (1.917 GB per APU at 576) is carved from the comm_ofi pool, sized as staging + slots (2048 + 1830 + 256 -> 4352 MiB). Staging (redistribution, result, shifted add) and slots are never live together within a product (redistribute (staged) -> v-exchanges (unstaged) -> CRT -> result exchange (staged)); only the fixed pool holds both, so per-level release gains nothing (dm is the peak) while release **per product** lets the pool be sized for the larger of the two. Code: `comm_layered_release_vslots(cl)` (frees the SHARE or own pair after completing anything pending; no-op without the switch) called once in `rns_mul_dist_mn` after the prime loop (`if (gen)`); the next product's first v-exchange allocates again at the pool rule's size. Pool law: `binsplit_shmem_pool_need` (OFI: `max(staging, slots)`; SHMEM-only: `max(4 staging, 4 slots)`) and `mem_model.py shmem_pool`. Digits cannot change (scratch, written before read). Fatal if RELEASE without POOL. Risks: every layered product must call the hook (only `rns_mul_dist_mn` uses `lay_get`: yes); a violation is a clean pool-full fatal; the alloc/free bookkeeping per product is cheap; time effect not measured at the time. Host emulation with release (shapes 23/47/35/36, OFI + SHARE=1): VERIFY OK, 0 pool blocks after each release.

### S36 — idea S-1: release (share) the v-slots between the tree levels (2026-10-10)

Finding that shaped the design: every general-map group owns a layered communicator per APU thread whose two v-slots grow to that group's largest product and stay to the run's end, so levels' slots add up (`binsplit_vslot_bytes`: `lower + max(top, dm)`). The tree runs level after level and a node is in one group per level, so a level's slots are dead when the next level starts; only the whole-machine group is reused. With X2 the slots live in the comm pool; S31 showed that regrowing slots there (free, alloc, staging blocks in between) fragments the first-fit pool (S28's 10-node failure), so S36 does not free per level.

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

### S45 — CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e)

Verdict: **SHARE=1 is effective and safe on the target launch line** (COMM_OFI=1, no X2). Plan and layout change only the v-slot term; a many-rank host test (up to 600 ranks, non-power-of-two groups, a cut last group) found no wrong data, no leak and symmetric pool offsets. It is a TCP / host emulation, not the real SHMEM / OFI transports.

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

### S52 — more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9)

CPU only (model scripts and the aac7 login node's plan/layout printers); no engine code changed. Labels m / mod / a. Target launch block of `archive:results_raw_2026-10-19/S45/launch_block_main13c244e.sh`.

##### Verdict
1. **Largest safe size: 3.82e13 digits** (+3.0 % over 3.71e13; 3.80e13 has the identical layout and margin). Device peak 357.9 GB per node (mod, `estimate.py`, SHARE=1), 360.0 GB for rank 0 (+2.1 GB allowance) against the 373.44 GB edge: **margin 13.4 GB (3.6 %)**, above M1's 10 GB rule; node 402 of 480 GB (mod); 185.9 s without the write vs 180.3 s at 3.71e13 (mod).
2. **Do not go above 3.82e13**: at 3.83e13 the C plan adds pieces (largest tree group 42 -> 54 pieces, 109 -> 121 in all) and the model's wall jumps +20 s (185.9 -> 206.1 s, +11 %) for +0.3 % digits; from 3.88e13 the device peak steps to 366.5 GB (margin 4.8 GB with rank 0); 4.08e13 is over the edge (375.1 GB, -3.8 GB).
3. **Messages:** 2.65 M per APU at 576 (mod): reciprocal 1.098 M, division 0.927 M, tree 0.624 M; 93-95 % are transform-chunk messages (K = 8 chunks x 575 peers). Memory-neutral coalescing saves 0.48-0.66 M messages (18-25 %) = **1.0-1.4 s at 2 us, 2.4-4.5 s at 8.5 us, 3.7-8.4 s at 20 us** (mod). Larger savings need memory or are lost.

(Details moved to `archive:repo_cl6_2026-10-25/results/GRID_GROUPS.md`, 2026-10-25.)

### B06 — uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001

**Verdict:** correctness of uneven groups at 3 and 5 nodes is shown (VERIFY OK; digits identical at 1e9 / 1e10). A speed gain is NOT shown and the modeled 8-17 % gain is not reproduced at one run each: do not use explicit uneven groups for speed on this evidence; for "exclude a node" they cost at most about 9 % (m, single run, partly rate noise). A paired ABBA (default vs `2,5`, 3 rounds, about 25 min per round) would be needed for any claim either way. Fallback policy if never measured: round down to a friendly count (576 stays; a bad node -> 575 is the poor case, default modeled +34 %), or use explicit groups and accept the modeled estimate. Not shown: speed at real size beyond the above, and a 5-node case with two uneven levels.

### O1 — overnight 6-node paired ABBA, MN_GROUPS `2,6` (default) vs single level `6` (TASKS 42)

**Verdict: inconclusive, no measurable difference in the raw totals; the earlier "17-24 s faster" is not reproduced.** None of the six rows meets the pass bar (the rate-adjusted no-cold row has p 0.042 but CI reaching -0.3 s and point estimate -7.8 s, just short of 8 s). The rate-adjusted estimates are not independent evidence (r is partly a result of the arm: B has the lower mean rate, as L4 found); one p = 0.042 among six rows with a CI touching 0 is not a result. Warm group-B runs of both arms sit at 244-246 s; the default `2,6` is not slower at 6 nodes, and its larger spread in L4 came mostly from the slow NIC group and the cold start.
