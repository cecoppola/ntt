# NIGHT_RUNS — overnight and day-plan runs of 2026-10-13/14 and the static-analysis triage

Scope: overnight and day-plan runs of 2026-10-13/14 and the static-analysis triage. | Owner of the items below; each section is the former `results/<ID>.md`, text unchanged except heading levels and file references (old IDs resolve through `results/INDEX.md`). Raw data and drivers of these items are in `archive:results_raw_2026-10-19/`.

| item | title |
|---|---|
| DAY | the 2026-10-13 day plan: large run, correctness sweep, SHMEM sweep, 6-node repeat |
| NIGHT | overnight runs 2026-10-13 (five aac7 items, two local analyses) |
| NIGHT2 | overnight regression on main 1aa2b80 (2026-10-14) |
| STAT | static-analysis triage (L1, results/NIGHT_RUNS.md §NIGHT section 6) and SH4 leftovers |

## DAY - the 2026-10-13 day plan: large run, correctness sweep, SHMEM sweep, 6-node repeat

(former file: DAY.md)

**Scope.** Four queued items on the test cluster, run 2026-10-13 between 08:54 and 16:40 ET, on commit 209712f
(`origin/main`, SHMEM build with `-DCOMM_SHMEM`; the large run used commit b1e4d567). Question: do digits stay identical at a large size, over a random sweep of 1 to 6 nodes, with the
SHMEM path (`COMM_OFI=0`), and what are the 6-node times with the current defaults. Collection commands and the item plan
live in the private day plan. Times are Eastern. All numbers are measured unless labelled otherwise.

#### 1. Large run (6 nodes, 3.8646 × 10¹¹ digits, writes)

| item | result |
|---|---|
| write run | rc 0, `VERIFY=OK`, total 1793.37 s (wall 1805 s, 09:37 ET); bs 90.1 s, recip 21.7 s, dm 697.2 s, init 41.5 s |
| digits vs reference | first 1 × 10¹¹ digits identical (cmp, 738 s); output 322 GB, deleted after the compare |
| `ECALC_RECHECK=1` on the written files | **not performed**: the recheck read a path that does not exist (`large_recheck.out.t1`), "no usable sidecar" on all six nodes, rc 1 after 8 s |

- The recheck failure is in the item's harness (the file prefix did not match the written files), not in the digits: the
  compare against the reference passed. The recheck itself is still open (follow-up 1).

#### 2. Correctness sweep (1 to 6 nodes, 48 runs, digits 1 × 10⁹ to 1 × 10¹¹)

| runs | ok | bad | node counts (runs each) | total time, ok runs (measured) |
|---|---|---|---|---|
| 48 | 48 | 0 | 1, 2, 3, 4, 5, 6 (8 each) | mean 63.3 s, sd 57.4, min 12.4, max 240.4 |

- Every run: rc 0, `VERIFY=OK`, digits identical to the reference, no segfault. Sizes are random and not comparable as times.

#### 3. SHMEM path sweep (`COMM_OFI=0`, 24 runs, 1 × 10⁹ to 2 × 10¹⁰ digits)

| runs | ok | bad | node counts | total time, ok runs (measured) |
|---|---|---|---|---|
| 24 | 21 | 3 | 1, 2, 3, 4, 5, 6 (4 each) | mean 33.2 s, sd 19.8, min 11.2, max 77.5 |

- The 3 failures are the first three runs (12:55 ET, at 3, 4 and 6 nodes): rc 141 within 8 to 18 s, `segv=3`. The SHMEM
  library saw one network interface on one node while four were requested. This is a node fault, not a code fault; the other
  21 runs at 1 to 6 nodes were digit-identical. Follow-up 2 (node-screen check of network interfaces).

#### 4. Final: gates and 6-node repeat (defaults, 3.8646 × 10¹¹ digits, no writes)

| check | result |
|---|---|
| gates at 1, 2, 4, 6 nodes on 209712f | PASS, digits identical |
| 6-node repeats | 34 runs, 34 OK, 0 hangs, rc 0 throughout |
| time (measured) | mean 197.1 s, sd 12.0, min 181.1, max 224.8 |
| previous night (results/NIGHT_RUNS.md §NIGHT §5) | mean 202.1 s, sd 8.6, 47 runs |

- The two means are not a paired comparison (different commits and dates, different run counts). No speed claim is made from
  them; a claim needs paired ABBA rounds.
- The test cluster sat idle from 13:20 to 14:39 ET while the final item was held for its start time (coordinator hold logic).

#### 5. Decision

- D27 (a), keep the summed SHMEM pool (target 17664 MiB, about +7.25 GiB per node, modelled), stands. No change to the
  defaults in this record.

#### Follow-ups

1. Re-run the `ECALC_RECHECK=1` check on the 6-node written files with the correct prefix (open; the write run and the
   compare passed).
2. Node-screen network-interface check before SHMEM runs (task NIC; the cause of the 3 failures is a node fault, not measured
   as fixed).
3. Idle time of about 80 minutes on the test cluster between the sweep and the final item: scheduling, not code.

#### RESUME

Branch `daydoc` from `origin/main` 209712f; this file, the RESULTS.md index row and the PLAN.md §2 repeat line are the only
changes. Nothing to resume unless follow-up 1 is run.

## NIGHT - overnight runs 2026-10-13 (five aac7 items, two local analyses)

(former file: NIGHT.md)

**Question.** Does the code at `origin/main` 4ae95ec1 (ADOPT2 defaults in force) give correct digits over a random
size sweep, node counts 1-6, and each switch fallback; what happens at the largest size that fits; what are the
6-node times with today's defaults, and how stable are they; what do the static analysis and the memory model say.

**Change / branch.** No code change. Commit 4ae95ec1 (`origin/main`), SHMEM build with `-DCOMM_SHMEM` checked per item.
Defaults in force: `P24_CRT_SCAN=1` (D24), `NEWTON_CHAIN_GUARD=1` (D25), `DM_MN_WRAP=1` (D22), packed output
`ECALC_OUT_PACKED=2`. Each item ran in a fresh worktree of `origin/main` on aac7, removed at the end.

**Method.** Runs on aac7, six-node holds, one item after another (queue names `01_`..`05_`). Launch line (identical for all
items): `COMM_TRANSPORT=shmem COMM_SHMEM_SERIAL=0 COMM_SHMEM_DEVHEAP=1 ECALC_NP=auto MN_T_CHUNK_MB=2048
COMM_SHMEM_ROUND_MB=1024 ECALC_MEM_GUARD_GB=6 ECALC_VERBOSE=2 MEM_REPORT_DEVS=1 ECALC_LOG_CLOCKS=1`. Digit checks compare
the written digits with the prefix of the GMP reference `e_1e11` through `unpack_digits` and `cmp`; `VERIFY` is the
program's own self-check (T1 residues, T2 windows). Times are wall-clock seconds from the run's own stage lines, labelled
measured. Collection: `docs/NIGHT_2026-10-13.md` (private protocol, collection commands only).

##### 1. Sweep (item night1_sweep): random sizes, node counts 1-6

| runs | ok | bad | node counts (runs each) | size range (total digits) | total time, ok runs |
|---|---|---|---|---|---|
| 60 | 60 | 0 | 1, 2, 3, 4, 5, 6 (10 each) | 1.34 × 10⁸ to 6.87 × 10¹⁰ | mean 38.7 s, sd 37.4 s, min 8.0 s, max 154.1 s (measured) |

- Every run: rc 0, `VERIFY=OK`, digits identical to the reference prefix, no segfault.
- The sweep was drawn for sizes up to 10¹¹; the largest size drawn was 6.87 × 10¹⁰ digits (no run at 10¹¹).
- Times are not comparable across sizes; the sweep is a correctness test.

##### 2. Fallback matrix (item night2_fallback): 1e10 digits, 2 and 6 nodes, writes and compare

| config (switch set to its fallback) | 2 nodes: result, total s | 6 nodes: result, total s |
|---|---|---|
| default | PASS, 48.64 | PASS, 50.09 |
| `ECALC_NP_MN3=1` (three-prime path) | PASS, 43.03 | PASS, 34.36 |
| `DM_MN_WRAP=0` | PASS, 46.25 | PASS, 35.75 |
| `P24_CRT_SCAN=0` | PASS, 47.61 | PASS, 38.90 |
| `RNS_CRT_SCAN=0` | PASS, 48.92 | PASS, 33.92 |
| `BS_BATCH_PIPE=0` | PASS, 45.07 | PASS, 32.38 |
| `NEWTON_CHAIN_GUARD=0` | PASS, 49.04 | PASS, 36.53 |
| `COMM_OFI=0` | PASS, 52.76 | **FAIL**, rc 3, no time (see Known failure A) |
| default again (drift check) | PASS, 46.61 | PASS, 30.25 |

- 17 of 18 runs passed. Every fallback that ran gave identical digits.
- One run per cell (measured). The 6-node default was 50.09 s at the start and 30.25 s at the end, so single 6-node
  runs of this size vary by about 40 %; no fallback timing difference is claimed from this table.

**Known failure A - `COMM_OFI=0` at 6 nodes, 1e10 digits (open bug).** rc 3 on all six nodes, before compute, with:
`comm_shmem: the symmetric pool (2816 MiB, COMM_SHMEM_POOL_MB) cannot hold 87 MiB more (in use 372 MiB)`; the
ranks report a needed pool of 460 MiB and, on two nodes, 539 MiB. The SHMEM path with `COMM_OFI=0` passes at 2 nodes.
The symmetric pool is too small at 6 nodes on this path; the fix is not yet made (open bug, the SHMEM path).

##### 3. Large run (item night3_large): 6 × 6.441 × 10¹⁰ = 3.8646 × 10¹¹ digits, packed writes, `ECALC_CKPT_TOP=1`

| item | result |
|---|---|
| free disk at start | 1392 GB (needed 600 GB; check passed) |
| run | **timed out**: rc 124 at 1815 s (driver limit 1800 s); no `VERIFY` reached; no compare; no recheck |
| stage reached | recip 22.67 s (measured) before the write |
| output | 322 GB in 11 part files (the summary's count; 12 would be 6 nodes × 2 parts) |

Cause (measured, from the item's timings): aac7's file system wrote each node's 13.4 GB part in 763-888 s,
about 15-18 MB/s per node. The 1800 s driver limit was reached during the write. This is a timeout on a slow file
system, not a compute failure: the compute had reached the write, and no digit was judged wrong. The large run is
not yet done; it needs a longer limit (or a write-free run) on this file system.

##### 4. Scaling (item night4_scaling): 6.441 × 10¹⁰ digits per node, defaults, no writes, 2 runs per count

Order 1 2 4 6 6 4 2 1. Stage times in seconds, mean of 2 runs (measured):

| nodes | total | bs | recip | dm | init | runs (total) | M2a total (before today's defaults) | change | weak-scaling efficiency T1/Tn (measured; M2a) |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 67.63 | 34.8 | 4.37 | 16.65 | 15.95 | 69.70, 65.57 | 74.4 | -9.1 % | 1.00 (1.00) |
| 2 | 119.39 | 36.1 | 13.50 | 37.80 | 41.45 | 117.81, 120.97 | 138.9 | -14.1 % | 0.57 (0.54) |
| 4 | 172.18 | 60.0 | 21.92 | 62.85 | 43.25 | 182.61, 161.74 | 188.2 | -8.5 % | 0.39 (0.40) |
| 6 | 204.64 | 76.15 | 22.65 | 85.70 | 39.20 | 209.68, 199.60 | 246.4 | -16.9 % | 0.33 (0.30) |

- All 8 runs passed (`VERIFY=OK`). The two 1-node runs were 69.7 s and 65.6 s.
- Efficiency is weak scaling (work grows with the node count): T1 / Tn, the same definition as `results/M2a.md`.
- Tonight's 6-node mean (204.6 s) agrees with the repeat mean (202.1 s, section 5).
- Only two runs per point; the differences are measured, not tested for significance.

##### 5. Repeat (item night5_repeat): 6 nodes, 3.8646 × 10¹¹ digits, defaults, no writes

| n | ok | mean | sd | min | max | hangs | stack dumps |
|---|---|---|---|---|---|---|---|
| 47 | 47 | 202.12 s | 8.60 s | 192.03 s | 230.92 s | 0 | 0 |

- 47 runs in 2 h 45 min (05:31-08:16 ET); stopped by the time limit (08:20 ET), not by a fault.
- Total times are measured; the run-to-run sd of about 4 % is the figure to use for 6-node timing claims at this size.
- Stage means over the 47 runs (measured): bs 78.80 s, recip 22.74 s, dm 87.06 s, init 31.95 s.

##### 6. Local static analysis (L1), commit 4ae95ec1

- 12 files analysed, 21 skipped (they need the GPU runtime, SHMEM or GMP headers). No analyzer timeouts.
- Warning classes (gcc 13, measured): 148 `-Wsign-conversion`, 96 `-Wconversion` (244 conversion warnings), 8 `-Wmisleading-indentation`,
  4 `-Wanalyzer-use-of-uninitialized-value`, 2 `-Wunused-variable`, 2 `-Wanalyzer-possible-null-dereference`, 1 `-Wunused-parameter`,
  1 `-Wanalyzer-null-dereference`.
- Files with most warnings: `ecalc/binsplit.c` 127, `ecalc/verify.c` 49, `ecalc/memsample.c` 41, `tools/unpack_digits.c` 22.
- The 7 analyzer warnings are 5 distinct locations (`memsample.c:73` and `:78` are each reported twice):

| file:line | kind |
|---|---|
| `ecalc/bigint.c:100` | `-Wanalyzer-null-dereference` (NULL `r`) |
| `ecalc/memsample.c:73` (x2) | `-Wanalyzer-use-of-uninitialized-value` (`live[3]`) |
| `ecalc/memsample.c:78` (x2) | `-Wanalyzer-use-of-uninitialized-value` (`live[3]`) |
| `tools/test_pack60.c:16` | `-Wanalyzer-possible-null-dereference` |
| `tools/unpack_digits.c:149` | `-Wanalyzer-possible-null-dereference` |

These are analyzer reports, not run failures. The `memsample.c` reads are the first to check; the others are in tools.

##### 7. Memory model (L2), modelled

- 108 combinations: 576 - k nodes (k = 0..8) × 12 sizes, today's defaults through `estimate.py`'s design. All values modelled.
- Target 3.82 × 10¹³ digits, k = 0 (576 nodes): device 375.1 GB with `DM_MN_WRAP=1` against the measured device edge
  373.44 GB, so **1.6 GB over** (the target does not fit at 576 nodes with this default). With `DM_MN_WRAP=0`, device
  345.0 GB, which fits.
- Also over the edge for k = 1, 4, 6 at 3.82 × 10¹³ (and at sizes just above it on 569-570 nodes). 3.71 × 10¹³ and
  3.60 × 10¹³ fit on 576 nodes (device margins +7.0 and +13.7 GB, modelled).
- The host and 480 GB node budget fit in every row (margins 49-77 GB, modelled).
- The choice of the `DM_MN_WRAP` default is decision **D26** (pending in `docs/DECISIONS.md`, private).

#### Known failures and open items

- **A. `COMM_OFI=0` at 6 nodes (open bug, SHMEM path).** Pool too small for the symmetric buffers (section 2). Passes at 2 nodes.
- **B. Large run timed out (not a compute failure).** The write ran at 15-18 MB/s per node on aac7's file system;
  the 1800 s driver limit ended the run during the write (section 3). The 3.8646 × 10¹¹ run with writes is not yet verified.
- **C. Target memory edge (decision D26).** Device 375.1 GB against 373.44 GB at 3.82 × 10¹³ digits, k = 0 (section 7).

#### Verdict

- Correctness: 132 of 134 runs passed (sweep 60/60, fallback 17/18, scaling 8/8, repeat 47/47, large 0/1). The two
  non-passing runs are known failures A (`COMM_OFI=0` at 6 nodes, rc 3, no compute) and B (large run, timeout during the write).
- Timing: 6 nodes, 3.8646 × 10¹¹ digits: 202.1 s mean, 8.6 s sd over 47 runs (measured).
- Decision needed: D26 (the `DM_MN_WRAP` default against the device edge at 3.82 × 10¹³).

#### Open issues

- Bug A (section 2) is open; `COMM_OFI=0` at 6 nodes is not usable until the pool is sized for it.
- The 3.8646 × 10¹¹ large run with writes and its recheck has not completed (section 3).
- The sweep did not reach 10¹¹ digits (largest 6.87 × 10¹⁰).
- The static analysis findings in `memsample.c` are not triaged.

#### RESUME

- Commit: 4ae95ec1 (`origin/main`). Items on aac7: `~/night/night{1_sweep,2_fallback,3_large,4_scaling,5_repeat}/`
  (summary.txt, DONE markers). Nothing on aac7 was deleted.
- Next: decide D26; fix bug A; re-run the large item with a longer driver limit or on a faster file system.

## NIGHT2 — overnight regression on main 1aa2b80 (2026-10-14)

(former file: NIGHT2.md)

Scope: a dated record of the overnight regression run on the test cluster on 2026-10-14, commit 1aa2b80 (after the CKPT, N7 and N7b
merges). Digits only, no writes on the timing runs, no speed claim. Times are Eastern. Dated record; the code and the central docs win
where they differ.

**Gates (measured, 1e9 digits, default settings and the SHMEM launch line):** 1, 2, 4 and 6 nodes PASS, digits identical, segv 0,
rc 0 (06:09–06:11 ET).

**6-node standard repeat (measured, 3.8646 × 10¹¹ digits, defaults, no writes):** 31 runs started, 0 hangs, all rc 0 and VERIFY OK.
Total time mean 190.70 s, sd 12.03 s, min 175.38 s, max 217.78 s (06:15–07:55 ET). These are repeat numbers only; no paired comparison.

**SHMEM-path sweep (measured, `COMM_OFI=0`, seeded list of 24 cases):** 24 of 24 digits identical at 1, 2, 3, 4, 5 and 6 nodes
(4 cases per node count, 05:12–05:49 ET). Totals 14.8–81.6 s (mean 32.0 s, sd 16.6 s), which mix sizes and are not a benchmark.

#### Observation: nodes drained after our jobs ended (reported from the cluster's node records; cause unknown, site-side)

- The 6 nodes used by a job went to drain with `needs_reboot: reboot issued: reboot timed out` right after three of our jobs ended:
  - 2026-10-13 21:07 ET, after a SHMEM run killed at its timeout;
  - 2026-10-14 02:05 ET, after a run killed at its timeout;
  - 2026-10-14 09:06 ET, after a job that ended normally.
- Other jobs that ended normally were not followed by a drain. The first two drains followed killed runs; the third followed a
  normal end, so a killed run does not account for all of them.
- Cause unknown; the reboot timeout is reported by the site, so this needs a look on the site side. No action taken on our side.

#### RESUME

- Source summaries are on the test cluster in our day-plan directory (not in the repo). The record is complete; nothing to resume.

## STAT - static-analysis triage (L1, results/NIGHT_RUNS.md §NIGHT section 6) and SH4 leftovers

(former file: STAT.md)

Date 2026-10-13. Base: main b1e4d56. Gates ran on aac7 (SHMEM build, 2 nodes then 1 node, one queue item).

#### 1. Analyzer findings (7 reports, 5 locations) and misleading-indentation (8)

| location | verdict | action |
|---|---|---|
| `ecalc/bigint.c:100` NULL `r` in `limb_mul_school` | false positive: the path needs `bi_mul_school` to reach `bi_reserve` with `a->n + b->n == 0`, but both are non-zero there (early return); the analyzer loses that across the recursive call | none in the code (still reported by gcc 13 `-O2 -fanalyzer`). Real nearby defect fixed: the `malloc` of the column scratch (`w1`) was unchecked; now `ec_fatal(EC_RC_OOM, ...)` |
| `ecalc/memsample.c:73`, `:78` `live[3]` uninitialised | false positive: the loop `d < DB_NQ` (4) fills all four | `live[DB_NQ] = {0}` (silences it; no behaviour change) |
| `tools/test_pack60.c:16` possibly-NULL `l` | real but harmless (test tool, unchecked `malloc`) | allocation checks in both loops |
| `tools/unpack_digits.c:149` possibly-NULL `p` | real but harmless (unchecked `calloc`) | check, message, exit 1 |
| `bigint.c:10` unused `oldn` | harmless (signature kept) | `(void)oldn` |
| `binsplit.c` unused `total0` (795), `tl0` (1406) | dead locals | removed |
| `binsplit.c` 8 misleading-indentation (234, 351, 438, 717, 1228, 1251, 1272, 1328) | all false positives: two statements on one line, each meant to be independent (for 1251/1272 the `t_wait_dma +=` after the loop is intended) | split onto separate lines, no semantic change |

Re-run (gcc 13.3, `-O2 -fanalyzer -Wall -Wextra`, and `-Wshadow -Wconversion` on binsplit.c): memsample.c, unpack_digits.c, test_pack60.c,
binsplit.c, and bigint.c except the one false positive above have no remaining analyzer, indentation or unused warnings (measured).

Conversion warnings (244: 148 sign-conversion, 96 conversion): none narrows an integer. 148 are `int` to `size_t`/`unsigned` sign changes
(indices, node/device/prime counts, never negative), 90+ are `size_t` to `double` (exact below 2^53; the largest counts at 3.82e13 digits,
about 2^41 limbs and 2^44 bytes, are far below). Noise; nothing to fix.

#### 2. SH4 shrink

- `ecalc/rns_dist.c`: the `flat` member of `struct acc` and its dead branches (the four accessors, `cache_find`, `b_choose`, `dist_core` including the
  host-side spill merge) removed: `flat` was never set (`acc_db` is the only constructor). 17 lines net.
- `ecalc/rns_mul.h`: `rns_stats` loses `t_repack, t_h2d, t_fwd, t_inv, t_d2h, t_crt, n_mdev, n_split, n_school, points_mdev` (always 0 since TREF).
- Printed summary lines changed (no script or test parses them; grep of tools/, tests/, ecalc/*.sh found none): `recip` line loses "`, N mdev`",
  `10dP` line loses "`(N mdev, N splits)`", `dm` line loses "`; N mdev`"; `t_newton` divmod line loses "`N mdev, N splits`".
  (The task named three prints; `t_newton.c` had a fourth.)

#### 3. Gates (aac7, measured)

| step | result |
|---|---|
| `tools/gate.sh --build --nodes 2` (SHMEM build, `-DCOMM_SHMEM`, no compiler warnings) | PASS, VERIFY OK, digits identical (1e9), P24 in use |
| `tools/gate.sh --nodes 1` | PASS, VERIFY OK, digits identical |
| `t_dbig 0` / `t_bs` / `t_newton 20` / `t_dist` | VERIFY OK (555 / 10 / 2756 / 55 checks) |
| `test_pack60` | PASS |

#### RESUME
Done; merged to main with this file. Open: bigint.c:100 stays as a documented analyzer false positive.
