# LAUNCH_DEFAULTS — launch-line decisions and defaults adopted

Owner of items B01, L1, T08, ADOPT2, DEF, FIT, S47, AAC7OPT (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals: `archive:results_merged_2026-10-19/`. Labels: m measured, mod modelled, a assumed. Times Eastern.

## Topic summary

What is current (see `ecalc/README.md` Switches table, which wins on any difference):
- Target launch line (B01, L1): `MN_T_CHUNK_MB=2048`, `COMM_LAYER_VSLOT_POOL=1` (X2's `COMM_LAYER_INTER2=1` left the line with Q65, 2026-10-21; the switch was removed 2026-10-23), `COMM_LAYER_VSLOT_RELEASE=1`, `ECALC_SEGV_TRACE=1`, `RNS_DIST_CACHE_FIT=1`. `COMM_LAYER_VSLOT_RELEASE` is now a built-in default (since 2026-10-12; follows `COMM_LAYER_VSLOT_POOL`), not only a launch-line setting.
- K2 + K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`) default 1 (T08); `=0` is the fallback. K4 `RNS_CRT_SUB` stays off.
- `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) are code defaults (ADOPT2); the guard costs +0.25 s [-1.00, +1.50] (n.s.) at 1 node.
- P24 (four primes, 24 digits per point) is the default for every multi-node product (D18, DEF); `ECALC_NP_MN3=1` kept the three-prime path as a fallback (removed 2026-10-19, D56). P24 costs +17.18 GB planes per node at 6.441 × 10¹⁰ digits per node (m).
- `DM_MN_WRAP` is now AUTOMATIC (D26, FIT): unset = 1 only when the device peak with the wrap is at most `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value wins. DEF's "DM_MN_WRAP default stays 0" and B01/L1's rank-0 figures are superseded by FIT. (D22 had made 1 the default in between; the automatic rule replaced it.)
- Memory fit before every launch (FIT): planner/runs print `plan fit` / `memory fit:`; a run that does not fit is refused (`plan REFUSED`, exit 8; `mnrun.sh` exit 4; `gate.sh` FAIL). Current target figure: 3.82 × 10¹³ digits on 576 nodes, device 358.96 GB vs the 373.44 GB edge, margin +14.48 GB (wrap off; with wrap 384.73, refused) (planner, mod). `ECALC_DEVICE_GB` default 373.44; `profiles/aac7.conf` 444.0 (m).
- Superseded numbers: B01's 3.79 × 10¹³ vs 3.82 × 10¹³ question (3.82 × 10¹³ margin 4.3 GB, below the 10 GB rule) is closed by L1 (margin 12.9 GB with slot release) and then FIT's planner figures; mem_model's own `dev_max` omits the comm_ofi pools (the C `binsplit_fit` is the source of truth).
- S47 (4-node re-confirm of T2048 and X2) was inconclusive; both stay on the aac7 line on their 10-node evidence (`results/SESSIONS.md §S22`, S31).
- Open: P24 at non-power-of-two groups below 32 nodes not gated; DEF's 6-node `total` difference for the wrap not significant (n.s.); FIT figures are layout values, not a target measurement.

## B01 -- launch-line decisions of 2026-10-11 (branch b01 from main bc80001)

Changes (no code default changed):
- Target line: `MN_T_CHUNK_MB=2048`, X2, `ECALC_SEGV_TRACE=1`, `RNS_DIST_CACHE_FIT=1` (no `RNS_DIST_CACHE_MN`; `cache_guard` accepts FIT=1), carried in `target_kit.sh` `S2_LINE_DEFAULT`. aac7 line `e16_headline.sh` already had T2048 + X2 + FIT (only stale comments changed); `tools/gate.sh` always sets SEGV_TRACE.
- `ECALC_SEGV_TRACE` (`segv_trace.c`): off = one getenv; on = installs SIGSEGV/SIGBUS/SIGABRT handlers that print and re-raise. Nothing else.
- `mn_model.py` `aac7` is now an alias of `aac7_s48` (previous profile `aac7_c16`); 4 nodes, 1.6e11: aac7 107.5 s = aac7_s48, aac7_c16 232.3 s (mod). `estimate.py` default is still `target`. `--spares N` kept, documented "not used".

Memory at 576 nodes (mod unless noted; `mn_model.py estimate --fabric target-m --g 576`, `COMM_LAYER_VSLOT_SHARE=1`, only `MN_T_CHUNK_MB=2048`, `MDB_SHIFT_CHUNK_MB=1024`; plain `--chunk-mb 2048` overstates by ~4 GB). X2 = +0.5 GB (user's figure), rank 0 = +2.1 GB (S52). GB:

| digits | T1024 device | T2048 device | + X2 + rank 0 | margin to 373.44 | node total |
|---|---|---|---|---|---|
| 3.71e13 | 351.7 | 357.9 (+6.2) | 360.5 | 12.9 (fits ≥ 10) | 402 of 480 |
| 3.82e13 | 357.9 | 366.5 (+8.6) | 369.1 | 4.3 (below 10) | 411 of 480 |
| 3.79e13 | - | 360.3 | 362.9 | 10.5 | - |
| 3.80e13 | - | 366.5 | 369.1 | 4.3 | - |

- Largest size with ≥ 10 GB rank-0 margin: about 3.79e13. The T2048 step is +6.2 GB in the model (rounds to the 4.3 GB arena chunk); the C layout (m, aac7 login node, `BS_LAYOUT_ONLY`) shows exactly +4.30 GB `device_with` (334.70 → 339.00 at 3.71e13; 339.00 → 343.29 at 3.82e13). `MN_PLAN_ONLY=<D>:576` with the full line: plan check OK at 3.71e13 and 3.82e13, 1236 products, SHMEM pool 17664 MB (m).
- Decision: T2048 + X2 on the line (done); size is the user's later call.
- Gates (aac7, m, 1e9 digits): `gate.sh --build --nodes 1` PASS, 12.50 s; `--nodes 2` PASS, 19.55 s; kit plan at 2 nodes OK. Branch b01 pushed, not merged at report time; 576-node memory with T2048+X2 modelled only.

## L1 -- COMM_LAYER_VSLOT_RELEASE=1 on the launch lines (user, 2026-10-11)

Code default unchanged at the time (0; later built in, see summary).
- Changes: `COMM_LAYER_VSLOT_RELEASE=1` after `COMM_LAYER_VSLOT_POOL=1` in `e16_headline.sh` LINE, `target_kit.sh` `S2_LINE_DEFAULT`, `profiles/aac7.conf`, `profiles/target.conf.template`, `tools/c04_queue_item.sh`, README launch table. `tools/profile_check.sh profiles/aac7.conf -q`: 34 keys, MATCH 24, MISMATCH 0, UNKNOWN 0 (m).
- Memory at 576 nodes, full target line (T2048 + X2 + RELEASE), C code on the aac7 login node (`MN_PLAN_ONLY`, `BS_LAYOUT_ONLY`, `COMM_OFI_PLAN_CXI=1`): plan check OK at three sizes, 1236 products; comm_ofi pool 4352 → 2304 MiB per APU (m). Rank-0 estimate = B01's figure minus the measured layout delta:

| digits | layout device_with RELEASE 0 → 1 (m) | delta | rank-0 before (B01, mod) | rank-0 with RELEASE (mod) | margin to 373.44 (mod) |
|---|---|---|---|---|---|
| 3.71e13 | 357.25 → 348.66 | -8.59 GB | 360.5 | 351.9 | 21.5 GB |
| 3.79e13 | 357.25 → 348.66 | -8.59 GB | 362.9 | 354.3 | 19.1 GB |
| 3.82e13 | 361.55 → 352.96 | -8.59 GB | 369.1 | 360.5 | 12.9 GB (above the 10 GB rule again) |

- Gate item `91_l1_gate.sh` (2 nodes) was queued, not finished at report time; merge needed gate PASS and kit plan check OK. Branch l1 pushed. Verdict: RELEASE restores the 3.82e13 margin (12.9 GB, mod).

## DEF — P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b)

Question: D18 makes P24 the default for every multi-node product (three-prime path stays selectable); D19 (b): count `DM_MN_WRAP`'s extra block-pool memory in `dm_layout`/`mem_model.py` and time `DM_MN_WRAP=0` vs `=1` at 6 nodes with the new default. Branch `def` (from main d1d19bf).

Change:
- `crt.c ec_np_planes`: under `ECALC_NP=auto`, pool 0 holds four planes at every g > 1 (`mn_p24_of()` true for every multi-node product). New `ECALC_NP_MN3=1` (off by default; removed 2026-10-19, D56) kept the rule before D18 (four planes only from 32 node-processes). `ecalc.c`: `ECALC_NP` unset becomes `auto` at size > 1 (still 3 at size 1). `tools/gate.sh --mn-primes p24|3` replaces `--p24`; `mem_model.py np_planes` follows.
- D19 (i): `binsplit.c dm_wrap_bytes` / `mem_model.py wrap_bytes`: with the wrap used, the mn division's low-product moment gains 2 quarters of P + 4 limbs per APU; the `layout:` line prints ` wrap <bytes>`.
- `DM_MN_WRAP` default unchanged (0) in this item (changed later: D22, then D26 automatic, see FIT).

#### 1. Planner and memory model (login node, m)
- `MN_PLAN_ONLY=3.82e13:576`: plan check OK, 1236 products; tree 42/42, recip 49/49, div 18/18 pieces at four primes. Identical with `ECALC_NP_MN3=1` (at 576 the old rule already gave four planes). With `DM_MN_WRAP=1`: layout node 380.84 → 406.61 GB (+25.77), with v-slots 388.51 → 414.28.
- `BS_LAYOUT_ONLY` at 6.441e10 × {1, 2, 4, 6} and 3.82e13/576 × 576, four variants: `--check-c` **0 terms not exact, largest arena difference 0.0000 %** (20 plane figures each).
- Pool 0 at 6.441e10 per node, cap 2^31: g 2, 4, 6 planes 120.88 GB per node (P24) vs 103.70 with `ECALC_NP_MN3=1` (+17.18 GB per node, +4.3 GB per APU); g 576: 120.88 both.
- Wrap term per APU (mod): 8.59 GB at 2 and 4 nodes, 5.73 at 6, 7.64 at 3.82e13/576; arena 214.75 → 240.52 GB, room node 386.84 → 412.61 GB: fits 480. At 2 nodes the model's 8.6 GB per APU is an upper bound against the 4.3 GB pool growth measured in P1 (3-prime form).

#### 2. Gates (aac7 queue, 1e9 digits, m)
All PASS (rc 0, VERIFY OK, digits identical): P24 at 2 nodes 18.79 s and 20.96 s (rebased), 1 node 12.27 s (three planes, unchanged at size 1), 4 nodes 20.30 s; `--mn-primes 3` (option removed 2026-10-19, D56) at 2 nodes 17.09 s and 17.03 s. Log checks `plane pool 0 for 4 primes` (P24) or `3 primes`. Device per APU max 33.1 of 137.4 GB (P24, 2 and 4 nodes; 4 nodes: planes 120.3 GB per node + block pool 8.6), 28.8 GB with three primes. At 6.441e10 per node the model gives the 4-node node peak 423.3 GB with P24 (mod), inside 480; MEAS-P24 measured 88.1 GB per APU in use at init at 2 nodes (`results/PRIMES_STRATEGY.md §MEAS-P24`).

#### 3. 6-node A/B of DM_MN_WRAP with P24 (m)
Item `87_def_abba6`, 2026-10-12 20:55-21:50, main f232085; 6 × 6.441e10 = 3.8646e11 digits, no digit output, aac7 line + `COMM_XSTATS=1 MN_WAIT_STATS=1`; A = `DM_MN_WRAP=0`, B = `=1`; rounds A B B A, cap 4 rounds. All 16 runs rc 0, VERIFY OK, four-prime plane pool; runs 181-229 s.

| quantity | A mean (8) | B mean (8) | B − A per round | B − A mean, 95 % CI, 4 rounds |
|---|---|---|---|---|
| `total` (s) | 199.40 | 196.54 | −4.13, −2.62, −6.89, +2.19 | −2.86 [−8.92, +3.19], p 0.23 (cap reached) |
| dm (s) | 84.88 | 78.63 | −3.46, −8.92, −9.26, −3.36 | −6.25 [−11.47, −1.03], p 0.032 |

Device memory per APU (m): in use at init 81.6 → 88.1 GB (+6.5); block pool at the division 51.54 → 57.98 (+6.44; 60.13 on some APUs, +8.59); division window peak 46.88 → 52.25 (+5.37; model wrap term 5.73, mod); driver-reported peak 84.4 → 92.9 (+8.5 of 137.4).

Verdict: D18 (P24 default) merged. `DM_MN_WRAP=1` saves 6.25 s of the division (p 0.032, 4 rounds) but `total` is n.s.; costs +5.4 GB per APU at the division's peak. Decision went to the user (became D22, then automatic D26). Open: `BS_LAYOUT_ONLY` reads `ECALC_NP` once per process (lay out multi-node points with `ECALC_NP=auto`); P24 at non-power-of-two groups below 32 nodes not gated; 4 more rounds could settle `total`; the wrap term was checked against one peak (5.37 vs 5.73 GB, 6 nodes) and P1's 2-node growth (4.3 vs 8.59 modelled). Raw: aac7 `<cluster work dir>/def/`.

## FIT — the memory fit before every launch, and DM_MN_WRAP automatic

Question (user decision D26, 2026-10-13): make the wrap automatic, and know before launch how well the memory fits. The planner (`MN_PLAN_ONLY`) only checked the 480 GB node budget: at 3.82e13 on 576 nodes with `DM_MN_WRAP=1` the model gave 375.1 GB device vs the 373.44 GB edge and the plan still said OK (`archive:results_merged_2026-10-23/NIGHT_RUNS.md` §NIGHT, section 7; NIGHT's 375.1/345.0 were `mem_model.dev_max`, which omits the 9.66 GB comm_ofi pools). Branch `fit`; digits unchanged.

Change:
- `binsplit.c binsplit_fit` (source of truth): device peak per node = max(dm phase: planes + arena + comm_ofi pools + 4 v-slots; bs phase: same + VMM bs growth 6.0 GB + tree-top v-slots), per APU = /4, vs `ECALC_DEVICE_GB`; node = planes + arena + bs growth + host + v-slots vs `ECALC_NODE_GB`. Printed by `MN_PLAN_ONLY` (`plan fit`; refusal `plan REFUSED ...: the memory does not fit`, exit 8), every run (`memory fit:` on rank 0), `BS_LAYOUT_ONLY` (`fit:`). comm_ofi pools count as device memory (`COMM_OFI_POOL=host` moves them out).
- Switches: `ECALC_DEVICE_GB` default 373.44 (target edge, 93.36 GB per APU, m, `results/TARGET.md §TGTBENCH2`); `ECALC_DEVICE_MARGIN_GB` default 5.0 (the tighter of CAP17's two bars). `profiles/aac7.conf`: `ECALC_DEVICE_GB=444.0` (m: aac7 SPX t_edge, VMM 444.0 GB committed, hipMalloc about 448.0; `archive:RESULTS_ARCHIVE_1-114.md` section 107; the lower, graceful one taken); `profiles/target.conf.template`: 373.44.
- `DM_MN_WRAP` automatic: unset, the wrap is taken only when device-with-wrap ≤ `ECALC_DEVICE_GB` − `ECALC_DEVICE_MARGIN_GB`; a set value (0/1/k) wins with a warning if it does not fit; all ranks decide alike. Config line `dm_mn_wrap:<v>(fit|set|default)`; size 1 does not use the wrap.
- Launchers: `tools/plan_values.py --fit <digits> <nodes>` (exit 1 = does not fit; assumes `COMM_OFI_PLAN_CXI=1` if unset); `ecalc/mnrun.sh` (refuses with exit 4 before srun; `MNRUN_FIT=0` skips); `tools/gate.sh` (`fit.txt`, FAIL if not fit; `GATE_FIT=0` skips); `target_kit.sh` plan stage. `mem_model.py --check-c` ports the decision. Planner with fit 0.73 s vs 0.58 s before (m, MN_PLAN_ONLY 3.82e13:576, login node).

Results (planner on the aac7 login node; figures from the C layout, mod). GB:

| run | profile | device per node (per APU) / limit | margin | node vs 480 | node margin | DM_MN_WRAP |
|---|---|---|---|---|---|---|
| 3.82e13 on 576 | target | 358.96 (89.74) / 373.44 | +14.48 | 395.85 | +84.15 | 0 (fit): with it 384.73 > 368.44 |
| same, `DM_MN_WRAP=1` set | target | 384.73 / 373.44 | −11.29 | 421.62 | +58.38 | 1 (set): WARNING, REFUSED |
| 6.441e10 on 1 | aac7 | 290.09 / 444.00 | +153.91 | 316.44 | +163.56 | not used at size 1 |
| 1.2882e11 on 2 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit): without 368.47 |
| 2.5764e11 on 4 | aac7 | 402.83 / 444.00 | +41.17 | 437.84 | +42.16 | 1 (fit) |
| 3.8646e11 on 6 | aac7 | 374.22 / 444.00 | +69.78 | 409.24 | +70.76 | 1 (fit): without 348.45 |

- Target terms at 3.82e13 on 576 (peak is the bs phase; dm phase 352.96): planes 120.88 + arena 214.75 + comm_ofi pools 9.66 + bs growth 6.00 + v-slots 7.67 GB; per APU arena 53.69 (dm need 51.73: reciprocal set 38.53, division 37.45; tree 50.93), plane pools 30.22; wrap term 7.64 GB per APU.
- `--check-c` on six `BS_LAYOUT_ONLY` points with the launch-line settings (`COMM_SHMEM_ROUND_MB=1024 MN_T_CHUNK_MB=2048`, `ECALC_DEVICE_GB=444`): 0 terms inexact, wrap decision equal everywhere (m); also exact after rebasing on main (rebuilt with `make -B SHMEM_CRAY=1`, gates not rerun after the rebase).
- Gates (aac7, commit 54f62a1, m): `gate.sh --build --nodes 2` PASS (VERIFY OK, digits identical, P24; fit 137.61/444.00 GB; `dm_mn_wrap:0(fit)` at 1e9); `--nodes 1` PASS (fit 120.44/444.00); 6 nodes 3.8646e11 via `mnrun.sh` (no digit output): rc 0, VERIFY OK on all 7 lines, `dm_mn_wrap:1(fit)`, ecalc total 238.5 s (one run, not a timing claim). The first attempt failed at doccheck because an untracked stand-in `profiles/target.conf` was in the worktree.

Verdict: done as decided (D26); the fit is computed before every launch, a run that does not fit is refused, the wrap is automatic. The target at 3.82e13 on 576 fits with the wrap off (+14.48 GB, planner). Merged.
Open: the device figure is the layout's, not a target measurement (bs growth 6.0 GB measured at size 1 on aac7; comm_ofi pools depend on `COMM_OFI_PLAN_CXI`); `mem_model.py --check-c` without `COMM_SHMEM_ROUND_MB=1024` differs in the comm_ofi pools at 6 and 576 nodes (-11.6 % / -19.5 %; exact with the launch lines' 1024); `mem_model.py dev_max` omits the comm_ofi pools (345.0 vs C's 358.96 GB); aac7 without its profile gets the 373.44 GB edge (stricter, never looser).

## S47 - exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2)

Finished 2026-10-11 08:56; **verdict INCONCLUSIVE at 4 nodes**.

Question: on the aac7 line (`e16_headline.sh`, 4 nodes, 6.441e10 digits per node = 2.5764e11 total, no digit output): (i) `MN_T_CHUNK_MB` 1024 (A) vs 2048 (B; aac7 line since 2026-10-08, S22 D2: 24 rounds, −42.3 s at 10 nodes); (ii) X2 off (A) vs on (B; since 2026-10-09, S31: −31.7 s at 10 nodes). `COMM_LAYER_VSLOT_SHARE=1` kept in both arms.

Design: build via `gate.sh --build` at main `daeb0f9` (SHMEM; 1-node 1e9 gate PASS; 4-node gates with each non-default arm PASS, m); hold job 12471 (4 nodes exclusive); driver `archive:drivers/s47/s47_driver.sh` with watchdog; ABBA interleaved by round (i then ii; odd rounds A B, even B A); page cache evicted and MemAvailable ≥ 428 GB before each run; stop rule per comparison: ≥ 6 complete pairs and (95 % CI excludes 0 or half-width ≤ 3 s), or 10 pairs (cap); budget 2.5 h; a pair counts only if both runs are rc 0 + VERIFY OK. Phase 2 ran the batch queue (13 items, all rc 0).

Results (m; paired difference B − A, positive = B slower; 95 % CI):

| comparison | pairs | ecalc total | driver wall | B faster in |
|---|---|---|---|---|
| (i) T2048 vs T1024 | 10 | +6.98 s [-10.32, +24.28] | +8.00 [-8.86, +24.86] | 5/10 |
| (ii) X2 on vs off | 9 | -2.68 s [-12.30, +6.93] | +0.00 [-7.70, +7.70] | 5/9 |

Both CIs include 0; no difference shown; neither setting confirmed or refuted. All 77 completed runs VERIFY OK.
- Bimodal totals: runs cluster at about 181-185 s (low) and 207-214 s (high), a 25-27 s gap (same size as the known slow-host stall, S32/S34; no host identified); round 1 slower still (225, 281 s). Counts low/high (threshold 195 s): (i) A 7/3, B 6/5; (ii) A 5/4, B 5/5. Both modes occur in every arm, so the mode is noise (cause unknown; slow-host or placement is a hypothesis). Pairs across modes show ±26 s, so the half-width stayed ~17 s (i) and ~10 s (ii) and the cap ended phase 1.
- Failures: two runs crashed with a segfault at the end of init (42 s, rc 1, VERIFY FAILED), both arm A (ii r2 on aac7-n11; i r7 on aac7-n10); pairs dropped; independent of the setting, look like the known intermittent init crash; not investigated.
- Queue bug fixed in the driver: `OUT` and `HOLD` were exported and inherited by queue items; `run_item` now uses `env -u OUT -u HOLD`.
- Verdict: inconclusive; point estimates (+7.0 s, −2.7 s) well inside the noise (±17, ±10 s). Both settings stay on the aac7 line (evidence: the 10-node S22/S31 runs). To resolve at 4 nodes: remove the cause of the bimodality (per-host stage times, nodechk, fixed node set via `-w`); or about 30+ pairs for (i) (about 3.5 h per comparison, mod); or a paired design with A and B back to back on the same nodes using stage-level times. Rule (user): adopt/keep only on a CI that excludes 0 with identical digits. Raw: aac7 `<cluster work dir>/s47/run/`.


## AAC7OPT — the fastest settings on aac7 (main 815b7a0 = 208ae18 code, 2026-10-25 13:53-14:31 ET)

Question (user 2026-10-25): the optimal ecalc settings on aac7. Items b5g_opt1n, b5h_opt1n2 (4 NIC-free nodes, 1 n), b5o_opt and
b5p_tchunk (the two networked nodes); one build each of path 1 (`XSHMEM=ofi`) and path 2 (`XSHMEM=cray`); base env = the aac7 profile line
(`ECALC_NODE_GB=420 ECALC_DEVICE_GB=444.0 MN_T_CHUNK_MB=2048 ECALC_MEM_GUARD_GB=6`); arms in Latin / ABBA order, paired by round;
`ecalc total` of PE 0; no digit file; every run VERIFY OK except the `POOL_LOG=30` arm (rc 6 at start). Labels: all m.

| question | size | pairs | result (arm - base, 95 % CI) |
|---|---|---|---|
| D (default layout) vs P (`ECALC_PE_PER_GPU=1`, path 1) | 1 n 6.6e10 | 4 | P +9.62 s [+4.98, +14.26] (D 74.2, P 83.8 s; dm 16.9 vs 28.4 s) |
| `ECALC_NP=3` / `=4` vs auto | 1 n 6.6e10, D | 8 | -0.19 [-1.23, +0.85] / +7.51 [+4.89, +10.13] (auto = 3 primes here) |
| `BS_SEED_CHUNK_MB=2048` | 1 n 6.6e10, D | 8 | +0.25 [-0.66, +1.17] |
| `ECALC_BG_THREADS=96` / `=24` (default 48) | 1 n 6.6e10, D | 8 | +1.12 [-2.42, +4.65] / -0.39 [-2.16, +1.37] |
| `POOL_LOG=30` | 1 n 6.6e10, D | - | fails at start (rc 6) |
| P vs D | 2 n 5e10 | 3 | P -14.71 s [-21.13, -8.30] (D 54.4, P 39.7 s, -27 %) |
| path 2 (`XSHMEM=cray`, P, `XSHMEM_LANES=2`) vs P (path 1) | 2 n 5e10 | 3 | +13.85 s [+10.76, +16.94] (53.5 s; vs D -0.86 [-8.78, +7.06]) |
| `MN_T_CHUNK_MB=1024` vs 2048 under P | 2 n 5e10 | 3 + 6 | +0.62 s over the 9 pairs (b5o -2.36 [-10.1, +5.4], b5p +2.11 [-1.03, +5.25]) |
| `XSHMEM_ROUND_MB=512` vs 128 under P | 2 n 5e10 | 3 | -0.65 [-11.15, +9.85] |

Capacity (planner, `plan_values.py --fit`, same base env): P fits 2 n up to 5e10 (node 412.5 GB of 420; 5.5e10 refused at 425.8 GB, the
host terms of four processes per node + Q110's staging); D fits 2 n to about 1.2e11 (1.32e11 refused by 1.7 GB); 1 n 6.6e10 fits both.

Verdict. On aac7: 1 node at the target's per-node share (6.6e10) runs fastest with the defaults (layout D, three primes); P wins at 1 n only
at smaller sizes (3.3e10: -5.5 s, §PEER of `results/SINGLE_NODE.md`). On 2 nodes P on the default build is fastest (-27 %) wherever it
fits (to 5e10); above that, D. Path 2 is the target's shape, not aac7's fastest. No other setting tested moves the time beyond noise: the
profile stays as it is (`MN_T_CHUNK_MB=2048` kept, no evidence either way under P). aac7's slow mode (one run in three, dm +4 s) is the
noise floor of these pairs (`results/NODE_SLOWMODE.md §BM`).

Sections moved to `archive:results_merged_2026-10-24/LAUNCH_DEFAULTS.md` (2026-10-24 cleanup): T08; ADOPT2.