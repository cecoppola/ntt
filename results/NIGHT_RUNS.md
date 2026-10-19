# NIGHT_RUNS — overnight and day-plan runs of 2026-10-13/14 and the static-analysis triage

Owner of items DAY, NIGHT, NIGHT2, STAT (old IDs resolve through `results/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals of this file: `archive:results_merged_2026-10-19/`. Times Eastern; numbers measured (m) unless labelled modelled (mod).

## Topic summary

- These are correctness-and-stability runs of `origin/main` at four commits (4ae95ec1, 209712f, 1aa2b80, b1e4d56) on aac7: random-size sweeps at 1-6 nodes, switch-fallback matrix, 6-node repeat at 3.8646 × 10¹¹ digits, one large run with writes, and a local static-analysis pass. No speed claim is made from any of them (repeat means are not paired comparisons).
- Result: digits were identical in every run that computed (sweeps 60/60, 48/48, 24/24, 21/24 with 3 node faults; fallbacks 17/18). Gates at 1/2/4/6 nodes PASS at 209712f and 1aa2b80.
- 6-node timing at 3.8646 × 10¹¹ digits, no writes (m): 202.1 s (sd 8.6, n 47, 4ae95ec1); 197.1 s (sd 12.0, n 34, 209712f); 190.7 s (sd 12.0, n 31, 1aa2b80). The run-to-run sd of about 4-6 % is the figure to use for 6-node timing claims.
- Large run with writes (6 nodes, 3.8646 × 10¹¹ digits): timed out at the 1800 s driver limit in NIGHT (slow file system, 15-18 MB/s per node), then completed in DAY (1793.37 s, VERIFY OK, first 1 × 10¹¹ digits identical). The `ECALC_RECHECK=1` pass on the written files was never done (harness prefix error): open.
- Open from NIGHT: bug A (`COMM_OFI=0` at 6 nodes, symmetric pool too small, rc 3; passes at 2 nodes). The DAY/NIGHT2 SHMEM sweeps at `COMM_OFI=0` passed at 3, 4, 6 nodes at smaller sizes; the 3 DAY failures were a node network-interface fault. Check the current state in `ecalc/README.md` and `RESULTS.md` before relying on A.
- Memory model (NIGHT §7, mod): the 3.82 × 10¹³ target on 576 nodes was 1.6 GB over the device edge with `DM_MN_WRAP=1` at that time; this led to D26 (pre-launch memory fit, `DM_MN_WRAP` automatic): current numbers are in `results/LAUNCH_DEFAULTS.md §FIT`, which supersede §7 here.
- Static analysis (STAT): all analyzer findings triaged; 2 false positives kept, real harmless defects fixed; SH4 dead-code shrink merged.
- Nodes drained (reboot timeout) after three of our jobs (NIGHT2): cause unknown, site-side.
- Superseded: NIGHT §7 and open issues B/C (see above); DAY decision D27 (a) (summed SHMEM pool, target 17664 MiB, about +7.25 GiB per node, mod) stands.

## DAY — the 2026-10-13 day plan: large run, correctness sweep, SHMEM sweep, 6-node repeat

Question: do digits stay identical at a large size, over a random 1-6 node sweep, and on the SHMEM path (`COMM_OFI=0`); what are the 6-node times with the defaults. Run 2026-10-13 08:54-16:40 on 209712f (SHMEM build `-DCOMM_SHMEM`; large run on b1e4d567). Branch `daydoc`.

#### 1. Large run (6 nodes, 3.8646 × 10¹¹ digits, writes)
- rc 0, `VERIFY=OK`, total 1793.37 s (wall 1805 s); bs 90.1, recip 21.7, dm 697.2, init 41.5 s (m).
- First 1 × 10¹¹ digits identical to the reference (cmp, 738 s); output 322 GB, deleted after the compare.
- `ECALC_RECHECK=1` **not performed**: the harness read a nonexistent prefix ("no usable sidecar", rc 1 after 8 s). A harness error, not a digit error; the recheck is still open (follow-up 1).

#### 2. Correctness sweep (48 runs, 1-6 nodes, 8 each, 1 × 10⁹ to 1 × 10¹¹ digits)
- 48/48 ok (rc 0, `VERIFY=OK`, digits identical, no segfault). Total time, ok runs: mean 63.3 s, sd 57.4, min 12.4, max 240.4 (m; sizes random, not comparable).

#### 3. SHMEM path sweep (`COMM_OFI=0`, 24 runs, 1 × 10⁹ to 2 × 10¹⁰ digits, 4 per node count)
- 21 ok, 3 failed. Failures: the first three runs (12:55, at 3, 4, 6 nodes), rc 141 within 8-18 s, `segv=3`: the SHMEM library saw one network interface on one node while four were requested: a node fault, not a code fault. The other 21 runs were digit-identical. Total, ok runs: mean 33.2 s, sd 19.8, min 11.2, max 77.5 (m).

#### 4. Final: gates and 6-node repeat (defaults, 3.8646 × 10¹¹ digits, no writes)
- Gates at 1, 2, 4, 6 nodes on 209712f PASS, digits identical.
- 6-node repeats: 34/34 OK, 0 hangs; mean 197.1 s, sd 12.0, min 181.1, max 224.8 (m). The previous night (NIGHT §5) gave 202.1 s, sd 8.6, n 47; not a paired comparison (different commits, dates, counts), so no speed claim; a claim needs paired ABBA rounds.
- The cluster sat idle 13:20-14:39 while the final item waited for its start time (scheduling, not code).

#### 5. Decision
- D27 (a), keep the summed SHMEM pool (target 17664 MiB, about +7.25 GiB per node, mod), stands; no defaults changed.

#### Follow-ups
1. Re-run `ECALC_RECHECK=1` on the 6-node written files with the correct prefix (open).
2. Node-screen network-interface check before SHMEM runs (task NIC; not measured as fixed).

## NIGHT — overnight runs 2026-10-13 (five aac7 items, two local analyses)

Question: at `origin/main` 4ae95ec1 (ADOPT2 defaults in force: `P24_CRT_SCAN=1` D24, `NEWTON_CHAIN_GUARD=1` D25, `DM_MN_WRAP=1` D22, `ECALC_OUT_PACKED=2`), are digits correct over a random size sweep at 1-6 nodes and under each switch fallback; what happens at the largest size; what are the 6-node times and their spread. No code change. Launch line (all items): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto MN_T_CHUNK_MB=2048 COMM_SHMEM_ROUND_MB=1024 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1`, six-node holds, one item after another (`night1_`..`night5_`). Digit check: written digits vs the GMP reference prefix `e_1e11` (`unpack_digits`, `cmp`); `VERIFY` = the program's self-check. Protocol: `docs/NIGHT_2026-10-13.md` (private).

##### 1. Sweep (night1_sweep)
- 60/60 ok (1-6 nodes, 10 each), sizes 1.34 × 10⁸ to 6.87 × 10¹⁰ digits (drawn up to 10¹¹; none reached it). rc 0, `VERIFY=OK`, digits identical, no segfault. Total time mean 38.7 s, sd 37.4, min 8.0, max 154.1 (m; correctness test only).

##### 2. Fallback matrix (night2_fallback): 1e10 digits, 2 and 6 nodes, writes and compare
Total s, one run per cell (m):

| config | 2 nodes | 6 nodes |
|---|---|---|
| default | PASS 48.64 | PASS 50.09 |
| `ECALC_NP_MN3=1` (removed 2026-10-19, D56) | PASS 43.03 | PASS 34.36 |
| `DM_MN_WRAP=0` | PASS 46.25 | PASS 35.75 |
| `P24_CRT_SCAN=0` | PASS 47.61 | PASS 38.90 |
| `RNS_CRT_SCAN=0` | PASS 48.92 | PASS 33.92 |
| `BS_BATCH_PIPE=0` | PASS 45.07 | PASS 32.38 |
| `NEWTON_CHAIN_GUARD=0` | PASS 49.04 | PASS 36.53 |
| `COMM_OFI=0` | PASS 52.76 | **FAIL** rc 3 (known failure A) |
| default again | PASS 46.61 | PASS 30.25 |

- 17/18 passed; every fallback that ran gave identical digits. The 6-node default went from 50.09 s to 30.25 s (about 40 % single-run variation), so no fallback timing difference is claimed.
- **Known failure A (open bug, SHMEM path):** `COMM_OFI=0` at 6 nodes: rc 3 on all six nodes before compute: `comm_shmem: the symmetric pool (2816 MiB, COMM_SHMEM_POOL_MB) cannot hold 87 MiB more (in use 372 MiB)`; ranks need 460 MiB (539 MiB on two nodes). Passes at 2 nodes. Pool too small at 6 nodes on this path; not fixed.

##### 3. Large run (night3_large): 6 × 6.441 × 10¹⁰ = 3.8646 × 10¹¹ digits, packed writes, `ECALC_CKPT_TOP=1`
- Free disk 1392 GB (600 GB needed). **Timed out**: rc 124 at 1815 s (driver limit 1800 s); no `VERIFY`, compare or recheck. recip 22.67 s reached before the write; output 322 GB in 11 part files (the summary's count; 6 nodes × 2 parts would be 12).
- Cause (m): the file system wrote each node's 13.4 GB part in 763-888 s (15-18 MB/s per node); the limit hit during the write. A timeout on a slow file system, not a compute failure. (Completed later in DAY §1.)

##### 4. Scaling (night4_scaling): 6.441 × 10¹⁰ digits per node, defaults, no writes, 2 runs per count (order 1 2 4 6 6 4 2 1)
Stage times in s, mean of 2 (m); weak-scaling efficiency T1/Tn, same definition as `results/TIME_MODEL.md §M2a`:

| nodes | total | bs | recip | dm | init | runs | M2a total before today's defaults | change | efficiency (M2a) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 67.63 | 34.8 | 4.37 | 16.65 | 15.95 | 69.70, 65.57 | 74.4 | -9.1 % | 1.00 (1.00) |
| 2 | 119.39 | 36.1 | 13.50 | 37.80 | 41.45 | 117.81, 120.97 | 138.9 | -14.1 % | 0.57 (0.54) |
| 4 | 172.18 | 60.0 | 21.92 | 62.85 | 43.25 | 182.61, 161.74 | 188.2 | -8.5 % | 0.39 (0.40) |
| 6 | 204.64 | 76.15 | 22.65 | 85.70 | 39.20 | 209.68, 199.60 | 246.4 | -16.9 % | 0.33 (0.30) |

- All 8 runs `VERIFY=OK`. Two runs per point; differences not tested for significance. The 6-node mean agrees with section 5.

##### 5. Repeat (night5_repeat): 6 nodes, 3.8646 × 10¹¹ digits, defaults, no writes
- n 47, ok 47, mean 202.12 s, sd 8.60, min 192.03, max 230.91; 0 hangs, 0 stack dumps (05:31-08:16; stopped by the 08:20 time limit). Stage means (m): bs 78.80, recip 22.74, dm 87.06, init 31.95 s. Run-to-run sd about 4 %.

##### 6. Local static analysis (L1), 4ae95ec1
- 12 files analysed, 21 skipped (need GPU runtime, SHMEM or GMP headers). gcc 13 warning classes (m): 148 `-Wsign-conversion`, 96 `-Wconversion`, 8 `-Wmisleading-indentation`, 4 `-Wanalyzer-use-of-uninitialized-value`, 2 `-Wunused-variable`, 2 `-Wanalyzer-possible-null-dereference`, 1 `-Wunused-parameter`, 1 `-Wanalyzer-null-dereference`. Most warnings: `binsplit.c` 127, `verify.c` 49, `memsample.c` 41, `unpack_digits.c` 22.
- 7 analyzer reports at 5 locations: `bigint.c:100` (NULL `r`), `memsample.c:73` and `:78` (`live[3]` uninitialised, each x2), `tools/test_pack60.c:16` and `tools/unpack_digits.c:149` (possible NULL). Analyzer reports, not run failures. Triage in STAT.

##### 7. Memory model (L2), modelled (superseded by `results/LAUNCH_DEFAULTS.md §FIT`)
- 108 combinations (576 - k nodes, k = 0..8, × 12 sizes). Target 3.82 × 10¹³, k = 0: device 375.1 GB with `DM_MN_WRAP=1` vs the measured device edge 373.44 GB: **1.6 GB over** (mod); with `DM_MN_WRAP=0` 345.0 GB, fits. Also over at k = 1, 4, 6. 3.71 × 10¹³ and 3.60 × 10¹³ fit at 576 nodes (device margins +7.0 and +13.7 GB, mod). Host and 480 GB node budget fit in every row (margins 49-77 GB, mod). The `DM_MN_WRAP` default question became decision D26.

#### Known failures, verdict, open
- A: `COMM_OFI=0` at 6 nodes (above). B: large run timeout, slow file system (resolved by DAY §1). C: target memory edge, decision D26.
- Verdict: 132 of 134 runs passed (sweep 60/60, fallback 17/18, scaling 8/8, repeat 47/47, large 0/1); the two non-passing are A and B. 6 nodes at 3.8646 × 10¹¹: 202.1 s mean, 8.6 s sd over 47 runs (m).
- Open: bug A; recheck of the written large run; sweep never reached 10¹¹ digits (largest 6.87 × 10¹⁰); `memsample.c` findings (triaged in STAT).
- Items on aac7: `<cluster work dir>/night/night{1_sweep,2_fallback,3_large,4_scaling,5_repeat}/` (summary.txt, DONE markers).

## NIGHT2 — overnight regression on main 1aa2b80 (2026-10-14)

After the CKPT, N7 and N7b merges. Digits only, no speed claim; dated record (code and central docs win).
- Gates (m, 1e9 digits, defaults, SHMEM launch line): 1, 2, 4, 6 nodes PASS, digits identical, segv 0, rc 0 (06:09-06:11).
- 6-node repeat (m, 3.8646 × 10¹¹ digits, defaults, no writes): 31 runs, 0 hangs, all rc 0 and VERIFY OK; mean 190.70 s, sd 12.03, min 175.38, max 217.78 (06:15-07:55). No paired comparison.
- SHMEM-path sweep (m, `COMM_OFI=0`, seeded 24 cases, 4 per node count 1-6): 24/24 digits identical (05:12-05:49); totals 14.8-81.6 s (mean 32.0, sd 16.6; sizes mixed, not a benchmark).
- Observation: the 6 nodes of a job went to drain with `needs_reboot: reboot issued: reboot timed out` right after three of our jobs: 2026-10-13 21:07 (SHMEM run killed at its timeout), 2026-10-14 02:05 (run killed at its timeout), 2026-10-14 09:06 (job that ended normally). Other normal ends were not followed by a drain. Cause unknown, site-side; no action taken.

## STAT — static-analysis triage (L1, NIGHT §6) and SH4 leftovers

Date 2026-10-13, base main b1e4d56; gates on aac7 (SHMEM build, 2 nodes then 1 node).

#### 1. Analyzer findings (7 reports, 5 locations) and misleading-indentation (8)
- `bigint.c:100` NULL `r` in `limb_mul_school`: false positive (the path needs `a->n + b->n == 0`, excluded by an early return; the analyzer loses it across the recursive call). Still reported by gcc 13 `-O2 -fanalyzer`. A real nearby defect fixed: unchecked `malloc` of `w1` now `ec_fatal(EC_RC_OOM, ...)`.
- `memsample.c:73`, `:78` `live[3]`: false positive (loop `d < DB_NQ` fills all four); `live[DB_NQ] = {0}` silences it, no behaviour change.
- `tools/test_pack60.c:16` and `tools/unpack_digits.c:149`: real but harmless (unchecked `malloc`/`calloc`); checks added.
- `bigint.c:10` unused `oldn`: `(void)oldn`. `binsplit.c` dead locals `total0` (795), `tl0` (1406): removed.
- `binsplit.c` 8 misleading-indentation (234, 351, 438, 717, 1228, 1251, 1272, 1328): all false positives (two independent statements on one line); split onto lines, no semantic change.
- Re-run (gcc 13.3, `-O2 -fanalyzer -Wall -Wextra`, plus `-Wshadow -Wconversion` on binsplit.c): no remaining analyzer, indentation or unused warnings except the one `bigint.c` false positive (m).
- 244 conversion warnings (148 sign-conversion, 96 conversion): none narrows an integer; 148 are non-negative `int` to `size_t`/`unsigned` changes, 90+ are `size_t` to `double` (exact below 2^53; the target needs about 2^41 limbs, 2^44 bytes). Noise, nothing to fix.

#### 2. SH4 shrink
- `ecalc/rns_dist.c`: the never-set `flat` member of `struct acc` and its dead branches removed (17 lines net). `ecalc/rns_mul.h`: `rns_stats` loses `t_repack, t_h2d, t_fwd, t_inv, t_d2h, t_crt, n_mdev, n_split, n_school, points_mdev` (always 0 since TREF).
- Printed lines changed (nothing parses them): `recip` loses ", N mdev"; `10dP` loses "(N mdev, N splits)"; `dm` loses "; N mdev"; `t_newton` divmod line loses "N mdev, N splits".

#### 3. Gates (aac7, m)
- `tools/gate.sh --build --nodes 2` (SHMEM, no warnings) PASS, VERIFY OK, digits identical (1e9), P24 in use; `--nodes 1` PASS; `t_dbig 0` / `t_bs` / `t_newton 20` / `t_dist` VERIFY OK (555 / 10 / 2756 / 55 checks); `test_pack60` PASS.
- Done and merged. Open: `bigint.c:100` stays as a documented analyzer false positive.
