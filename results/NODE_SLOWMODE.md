# NODE_SLOWMODE — the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts)

Scope: the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts). Condensed from the former per-item files (old IDs resolve through `results/INDEX.md`); original text, including the per-run table of all 93 four-node runs, in `archive:results_merged_2026-10-19/NODE_SLOWMODE.md`; raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modelled, a = assumed.

## Topic summary

- **What this is.** Two linked questions on aac7: (1) why 4-node runs at the same size come out either fast (~181-185 s) or slow (~208-214 s) (§T05, §T09, §A13, §A14), and (2) how to screen nodes before a long run (§B04, §T10, §S49, §NIC), including the known slow host aac7-n01.
- **Slow mode, current state (matches RESULTS.md).** A per-launch effect on the inter-node all-to-all: -21 % rate (10.9 vs 13.8 GB/s per APU thread, m), every network-bound dm product 25-30 % longer, compute and init unchanged, ~29-52 % of launches slow depending on the sample, memoryless between runs, independent of arm, node, rank order, hold, time of day, MemAvailable and other jobs (a quiet cluster still gave 2 of 6, §A13). The NIC traffic lands in one of two `oxe_mcu_meas` groups (A 18-25 slow, B 28-33 fast), drawn per launch; at 4 nodes the split is clean (A 10.62 vs B 12.02 GB/s per thread, 31 launches, §A14) and at 2 nodes the groups do not differ. **The cause that picks the group is not established**: no tested setting (traffic class, NUM_NICS, NIC policy, SEP, startup connect) pins it, and detect-and-relaunch loses (mod). Nothing adopted; for timing, run ABBA and record each run's mode.
- **Separate effects.** A reciprocal-chain spike (3-89 s instead of ~2.3 s, cold host state, 29 of 93 runs) and slow init in the first runs of a fresh hold explain the outliers above 213 s; they are not the bimodality (§T05).
- **Node screens.** `tools/nodescreen.sh` (+ `nodescreen_tab.py`, `initchk.py`) is the fast pre-run screen (44-58 s at 2 nodes, lean 25 s); it excludes nodes that fail, run >3x the median init or first-upload time (and >= 2 s above it), or see fewer cxi NICs than `--want-nics` (§NIC). The 1 GB upload probe and the cold-path probe do not catch the aac7-n01 stall because the host was healthy when tested (no positive control, §S49, §T10); the adopted slow-host policy is the 3x init/upload check of `initchk.py`. Not caught by any quick screen: slow mode, minutes-in stalls, intermittent `shmem_init` segfaults.
- **Superseded or open.** §T05's "mechanism unidentified" is narrowed by §A13/§A14 to a launch-time NIC state. The "65 corrections" report (§A13 part B) is not reproducible on main (settle with an aac6 TCP 4-process rerun). Uneven `MN_GROUPS` for excluded nodes (§B04) is modelled only.

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
| L1 | the NIC bank group is a fair per-launch coin (items 9cg, 9ch, 2026-10-19) |
| RAMDISK | node RAM-disk leftovers (`Shmem` in /proc/meminfo; items 9cj-9cm, 2026-10-19) |

## §T05 - why 4-node run totals are bimodal (fast ~181 s / slow ~208 s)

Analysis of existing logs only (no cluster jobs), session 2026-10-11: 93 four-node runs at 6.441e10 digits per node on aac7 (same 4 hosts in every run; sources: 40 runs of hold
12471, 2 of S48, 16 of S50, 12 + 23 of S51; the full per-run table is in the archive original). All numbers m.
**Answer.** The slow mode is a per-run communication slowdown: the inter-node all-to-all runs at 10.9 instead of 13.8 GB/s per APU thread (-21 %) on all four PEs at once, and every
network-bound dm product takes 25-30 % longer (constant ratio from first to last stage). GPU compute and init are unchanged. It is a two-state switch redrawn independently per run (~29 %
of runs slow): independent of arm, node, rank, ABBA position, preceding run, time of day, hold or MemAvailable.
**Classification** (mean of the 4 PEs' all-to-all rate; clean gap: slow 8.5-11.7 GB/s in 27 runs, fast 12.3-14.0 in 66). Clean runs only (chain <= 2.6 s, init < 36 s; 16 slow, 47 fast):

| quantity | slow | fast | slow - fast |
|---|---|---|---|
| total s | 208.3 (sd 1.7) | 181.4 (sd 1.4) | +26.8 (1.15x) |
| bs / dm s | 77.3 / 96.7 | 70.7 / 76.8 | +6.6 / +20.0 (1.09x / 1.26x) |
| reciprocal products / dm step 1 / step 2 / X_lo*Q s | 21.29 / 33.32 / 13.30 / 22.73 | 16.39 / 26.12 / 10.64 / 17.63 | +4.9 / +7.2 / +2.7 / +5.1 (1.25-1.30x) |
| init / dc s | 30.3 / 2.98 | 29.8 / 2.96 | unchanged |
| all-to-all GB/s per APU thread | 10.89 | 13.80 | -2.9 (0.79x; ratio 0.78-0.79 on every PE) |

Per-level tree times are +16 % on every node; per-device compute counters (S51) are identical in slow and fast runs.
**Tested and not separating the modes:** arm (14 slow of 45 vs 13 of 46); one bad node or rank (all PEs slow together; PE0 is slowest in 26 of 27 slow but also 64 of 66 fast runs, structural);
sub-stage (all network products, no compute); ABBA position (2, 5, 5, 4 slow of 13, 13, 13, 12); alternation (P(slow after fast) 0.32, after slow 0.26, runs test z = 0.5: memoryless);
preceding run; gap since previous run (driver artefact); MemAvailable after init (120.7 vs 126.0 GB, Welch t = -1.45); hold (12471 and 12496 both show both modes); time of day; 2-node link route
(S48 2-node in-chassis pair: 144.1 s slow vs 133.9 fast, dm +7-14 %: the mode occurs on a single in-chassis pair, smaller effect with less traffic).
**Best-supported explanation (moderate evidence, mechanism unidentified).** The network throughput seen by the whole job is in one of two states per run (~0.79x when slow). Logs cannot say whether
the state lives in the fabric/NICs or is created at process start (route, endpoint layout, registered-buffer placement); external traffic would fit only if it flipped on minute timescales and stayed
constant within a run. (Resolved toward launch-time NIC state in §A13 and §A14.)
**Second, separate effect (not the bimodality).** The single-node reciprocal chain (normally 2.2-2.4 s) took 3-89 s in 29 of 93 runs (S47 12/40, S51b 16/23, S51 1/12, S50 0/16), independent of the comm
mode; it explains every total above 213 s (231-285 s) and the 185-202 s "fast" runs. Init 46-53 s instead of 28-32 s in the first one or two runs of a fresh start. Hypothesis (low evidence, untested):
cold host-side state (page cache, first touch, HIP module/JIT cache).
**Proposed confirming test (not run as such).** 24 real 4-node runs with a stand-alone 4-node all-to-all probe before and after each, `COMM_XSTATS=1 MN_WAIT_STATS=1`, NIC counters sampled; environmental state
predicts probe rate tracks the run's mode (correlation > 0.8), process-start state predicts it does not (this is what §A13 then did). **Open.** Three "partial" runs (12.3-12.8 GB/s) in cold-start or
chain-spike runs sit between the modes.

## §T09 - the bimodal 4-node totals (2026-10-11, aac7, session task t09)

Question: why do 4-node runs at 6.441e10 digits per node (2.5764e11 digits) come out at 180-186 s in one run and 207-213 s in the next, on the same hosts? Code origin/main ceb94c1 (`make -B SHMEM_CRAY=1`),
no code change; hold 12507 (4 nodes, 11:30-12:18 EDT), aac7 launch line + `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1`, no digit output, reference files evicted before each run. 10 runs VERIFY OK, 2 aborted by
the memory guard. Logs of runs 1-2 lost (label bug, fixed in the driver); totals kept.

| runs (m) | total s | dm rate GB/s per PE | note |
|---|---|---|---|
| 1-2 | 247.6, 257.7 | - | base, first runs of the hold |
| 3, 4 | 233.8, 224.2 | 8.8-10.5, 9.3-11.2 | slow; run 4 plain `srun` block order |
| 5-9, 11 | 181.1-185.1 | 13.0-15.9 | fast (stable to +-2 s); incl. run 8 rank order rotated by 2, run 9 after 60 s pause |
| 10 | 209.0 | 9.9-12.0 | slow, after an aborted fill run |

**Findings (m).** Bytes moved are identical in both modes; the exchange rate of every PE drops by the same factor (dm 0.70-0.72, bs 0.81-0.84); PE ordering unchanged: a uniform slowdown, not one node, rank or link.
Run 4 vs 5: dm +22.8 s, bs +13.5, recip +3.7, init +4; waits rise on all four nodes together (dm wait 172-184 vs 146-160 s); barrier time 0. Not tied to node set, rank order, launch form, 60 s pause or eviction. Pre-run
meminfo/buddyinfo do not separate the modes (run 10 vs 11 identical); Cached fell 160 -> 88-97 -> 81-83 GB over the first runs, so slow runs 1-3 coincide with cached memory still being reclaimed. The page-cache fill test
(80 GB to /tmp) could not be done: MemAvailable fell to the 6 GB guard (rc 9/8).
**Explanation.** Established: a ~25-30 % lower node-to-node exchange rate on all four nodes at once for the same bytes, lasting several consecutive runs (runs 1-4 = 17 min after the hold began; run 10 alone). Not established:
the cause (candidates: concurrent fabric traffic from other jobs of the same session, unread and unproven; host-memory state after allocation start, not shown). Practical rule: the first runs of a fresh 4-node hold
(~15-17 min, 4 runs here) are slow; timing A/B pairs should discard or randomise over them (ABBA does) and record each run's mode (dm >= 13 GB/s on PE 0 = fast, <= 10 = slow).
**Open then.** A second hold while the session is quiet (done in §A13); a controlled reclaimable memory fill.

## §A13 - 4-node slow mode on a quiet aac7 (part A) and the DIST_LOGN_TEST 4-node "65 corrections" (part B)

2026-10-11, aac7, main 6d80ef1, production defaults, no code change; gate `--nodes 1` PASS. One 4-node hold (job 12531, 55 min). Drivers `archive:drivers/a13/`; raw NIC snapshots stayed on aac7.

**Part A: is the slow mode fabric state or set at process start? (m)** 6 back-to-back real runs (4 x 6.441e10), `FI_MR_CACHE_MONITOR=userfaultfd COMM_XSTATS=1 MN_WAIT_STATS=1`, a stand-alone
`t_comm --bw 4 250 256` probe launched like a real run between runs, NIC telemetry counters (`oxe_*`, ~1900 per NIC) and `ethtool -S` read before and after everything. Nothing else of anyone's ran.

| run | total s | init | bs | dm | all-to-all GB/s per thread | mode | OXE MCU group |
|---|---|---|---|---|---|---|---|
| r001 | 220.6 | 47.6 | 78.2 | 91.6 | 12.00 | mid | B |
| r002 | 197.6 | 35.7 | 71.0 | 86.1 | 13.10 | fast | B |
| r003 | 196.8 | 37.7 | 70.7 | 84.6 | 13.07 | fast | B |
| r004 | 220.0 | 34.9 | 76.0 | 104.0 | 10.99 | slow | A |
| r005 | 200.2 | 33.9 | 75.5 | 87.5 | 11.60 | slow | A |
| r006 | 182.0 | 32.1 | 69.4 | 77.0 | 13.68 | fast | B |

All rc 0, VERIFY OK; 2 of 6 slow (33 %, earlier 29 %) on an otherwise empty cluster, so other jobs are not the cause.
1. **Probe rate does not predict the run**: the probes before the runs read 10.44, 10.37, 10.10, 10.48, 10.60, 10.33 GB/s for runs mid, fast, fast, slow, slow, fast (the two slow runs had the two highest "before" probes); no 0.79x drop ever seen in the probe.
2. **NIC counters separate the slow runs and point at the launch.** Fine-grained flow control (`oxe_stall_fgfc_start`) ~2x in slow runs (2.97e8 / 3.74e8 vs 1.43-1.47e8), `oxe_stall_pbuf_bc_0` 1.25e11 / 0.96e11 vs 0.85-0.86e11, no uncorrected codewords:
   more end-to-end back-pressure. **`oxe_mcu_meas_18..25` (group A) vs `28..33` (group B)**: every run puts all byte traffic (3.7e10 per counter) in exactly one group, the same on every NIC of every node; B in r001-r003 and r006, A in the two slow runs (6 of 6 agree; Fisher
   one-sided p = 1/15 = 0.067, not significant). What the groups are in firmware is unknown (assumed: which MCU set an endpoint's traffic hashes to).
3. **The group is drawn per launch, independent of the previous process**: the probes (separate launches) also fall into A (p002, p005, p006) or B, the probe's group does not predict the next run's, and A-probes are slower (64 MiB slabs 10.2 vs 12.1 GB/s; 256 MiB 10.09 vs 10.47). A different tiny program reproduces the slow mode, so it is state set when the
   process's NIC endpoints are created, not a time-varying fabric or host state.
**Answer.** Indicative (6 runs), not proven. **Suggested next step (done in §A14):** find what picks the group at launch with many cheap probe launches; a relaunch-at-init would recover ~20 s in ~30 % of runs.

**Part B: DIST_LOGN_TEST 4-node "65 corrections": not reproduced (main 6d80ef1, m).** 4 nodes x 4 APUs, SHMEM, VERIFY OK on all nodes and 0 corrections in every run: 1e9 with `DIST_LOGN_TEST=21/22/23/24` (caps 2^22-2^26, totals 13.6-28.1 s), 1e10
with `=26/27` (caps 2^27/2^28, X 5.6e8 limbs; 17.9 / 19.3 s), also with `NEWTON_DKM=0` (the older division G12 ran; 14.2 / 12.4 s) and without the aac7 line (13.5 / 15.4 s; no `dist_mn` lines, so the forced grid is not shown). A TCP 1 GbE run was killed after ~9 min
in the known-slow TCP reciprocal (inconclusive). The G12 failures were seen on aac6 (4 processes on loopback TCP, before DKM division was default and before the X1/X2/layered exchange changes); the 1-node A05 check is consistent.
**Hypothesis (assumed):** the defect lived in the TCP/aac6 path or in the pre-DKM division at a forced small cap and was removed later; the guard `++nc > 64` in `mn_dkm_corr` / `newton_mn_divmod` fires on a wrong product or reciprocal. To settle: an aac6 4-process TCP rerun of G12's batch 8
(1e9, `POOL_LOG=29 DIST_LOGN_TEST=23`); if it passes, TASKS 1.6 closes as "not reproducible on main"; if not, `ECALC_RES_LOG=1` localises the level.

## §A14 - what picks the "slow mode" NIC group at launch? (aac7, 2026-10-11)

**Result.** At 2 nodes the MCU group (A = `oxe_mcu_meas` 18-25, B = 28-33) is drawn per launch with p(A) ~0.5 (44 A of 86 launches, m) but does not change the 2-node probe rate (64 MiB slabs: A 6.59 +- 0.42, B 6.59 +- 0.26 GB/s per thread, n = 44 / 42).
**No setting pins the group (m, `t_comm --bw 4 100 64`, ~13 s per launch):** none (22 launches: 9 A, 13 B; 6.53 / 6.65), `SHMEM_OFI_DEFAULT_TCLASS=TC_BULK_DATA` (8 A / 4 B), `TC_LOW_LATENCY` (8 / 4), `SHMEM_OFI_STARTUP_CONNECT=1` (3 / 5), `NUM_NICS=1` (4 / 4), `NIC_POLICY=ROUND-ROBIN` (4 / 4), `USE_SEP=1` (5 / 3);
`CXI_PID_BASE=64` failed to launch. Traffic-class runs A 16/24 vs 9/22 without (Fisher ~0.1, n.s.). Source reading: ecalc (Cray OpenSHMEMX 11.8, libfabric 2.3.1 CXI) sets no tclass/VNI/service; no documented knob selects the MCU; assumed: the group follows which NIC command-queue/pid resources the driver's free list hands out.
**4-node probe (queue item 40_a14_probe4, 31 launches, m):** group A 16 launches mean rate64 10.62 (8.81-11.79), group B 15 launches 12.02 (11.86-12.29); the split is clean (one A at 11.79 above the B minimum 11.86); A is 1.4 GB/s (~12 %) slower, ~52 % of launches are A. **At 4 nodes the group sets the all-to-all speed (A also has 2x fgfc stalls); at 2 nodes it does not.**
**P5 (item 80_f1_a14cfg4, 32 launches, shuffled rounds of n1 = no env, n2 = identical no env, tcll = TC_LOW_LATENCY, nn1 = NUM_NICS=1; m):** n1 5 A (10.78) / 3 B (11.97), n2 4 A (10.48) / 4 B (11.97), tcll 5 / 3 (10.10 / 12.02), nn1 4 / 4 (10.94 / 12.01); identical settings land in both groups and can differ within a round (18 A, 14 B in total); the P4 pattern was chance. Group B 11.97-12.02, group A 10.1-10.9 GB/s: A costs 1.1-1.9 GB/s (10-15 %) per thread.
**Detect-and-relaunch (mod from A13):** init 32-48 s (mean ~37) vs ~20 s lost per slow run (2 of 6), so a relaunch after init loses; a 15-20 s pre-probe on every launch saves 20 s on ~40 %: break-even at best. **Verdict:** no tested setting pins or avoids the slow group; nothing to adopt; relaunch not worthwhile.

## §B04 - fast node screen before the long run (2026-10-11, Wave B)

Prototype `tools/nodescreen.sh` (+ `nodescreen_tab.py`, `initchk.py`), tested on aac7 (4 pairs, 12 screens) and aac6 (1 node); scale figures are a. User's input: "no spare nodes, exclude very bad nodes only".
**Design.** One process per node, two stages in sequence (never two network programs per node): **local** (`t_nodechk 1 3 <cold-gb>`: meminfo, 1 GB upload per APU x3, optional cold path; then a single-node `ecalc` of `--local-digits`, default 1e8, with `ECALC_INIT_TL=1`: `loc_init`, `loc_post`, `loc_wall`) and
**mn** (one SHMEM job over the kept nodes, N x `--mn-digits-per-node`, default 1e8: `mn_init`, `mn_up` = host-attributed `db_from_bi` upload of the reciprocal stage where aac7-n01 stalled). Rule: flag a metric > 3x the reference (median) AND >= 2 s above it; any failed, timed-out or missing step flags the node; exit 0 / 3 (excluded hosts) / 2 (screen failed).
**What it can and cannot catch (m, 2 nodes).** Caught: dead GPU, upload or hipMalloc errors, lost or timed-out node script, gross slow upload or cold path, slow init (`loc_init` 9.6-14.7 s and `mn_init` 7.2-8.9 s healthy), slow short compute (`loc_post` 7.6-9.6 s healthy), low MemAvailable only with `--min-avail-gb` (off: low MemAvailable alone was not a stall, S49).
Unproven: the aac7-n01 first-upload stall (`mn_up` 0.00-0.01 s healthy; a 2e8-digit run uploads little). Not caught: NIC/link bandwidth per node (a libfabric ring `archive:removed_2026-10-12/ecalc/tests/t_nicbw.c` could be a third stage), the fabric-wide slow mode (a state of the allocation; the screen prints the mn wall for comparison with a same-size baseline), minutes-in stalls, thermal throttling, leaks, intermittent `shmem_init` segfault (13 % of launches, A16; shows as mn rc != 0 without excluding a node).
**Cost (m, aac7, 2 nodes).** nodechk step 4.1-7.7 s; local ecalc 1e8 18.7-23.7 s (init 10.8-14.7 + 7.6-9.6); local stage 24.5-32.5 s; mn stage 18.7-25.3 s; full screen 44.1-57.9 s (5 screens); lean `--local-digits 0` 25.3 s; `--no-mn` aac6 1 node 20.9 s. At 576 nodes (a): 1-2 min full, 0.5-1 min lean; worst-case hang `2 x timeout-local + 30` + `timeout-mn` (270 + 240 s default).
**Tests (m).** aac7-n00 + aac7-n01 (job 12535): 2 screens rc 0, nobody flagged, n01 indistinguishable (nc_up 0.02, cold 0.53, loc_init 11.2, mn_init 7.2-8.1, mn_up 0.01 s, avail 439-440 GB); failure path (job 12538, `--timeout-local 8 --no-mn`): both flagged `FAILED: ecalc rc=124`, rc 3; other pairs rc 0 (55.5 and 57.9 s); lean 25.3 s; aac6 1 node rc 0 20.9 s; synthetic 8-node log (slow init 3.7x, rc 1, truncated and missing logs flagged). **aac7-n01 does not stand out today, so the screen has not been shown to catch the stall (no positive control).**
**Findings outside the files.** `mnrun.sh` bug: `MNRUN_LABEL=1` is ignored when `MNRUN_CPUS_PER_TASK` is set (assigns `copt=(-c ...)` instead of appending, dropping `--label`); `nodescreen.sh` works around it with `SLURM_LABELIO=1`; one-line fix `copt+=(-c ...)`. `ecalc 1e8` init is 11-15 s (plane pools 5.6 s, staging 3.5 s).
**If k nodes are excluded (mod, 3.71e13 digits, `MN_PLAN_ONLY` / `BS_LAYOUT_ONLY`, `estimate.py`, `ECALC_NODE_GB=480 BS_ARENA_ROOM=0.16`).** Any count 572-576 passes `plan check ... OK` (1236 / 1234 / 1232 / 1230 / 1228 products; digits per node +0.17 % per excluded node), but the default group schedule (powers of two dividing g, then the odd part's prime factors) makes a bad count expensive; an explicit `MN_GROUPS=2,4,8,16,32,64,192,<g>` fixes it:

| g | default schedule, modelled wall | explicit `MN_GROUPS=...,<g>`, modelled wall | model node GB |
|---|---|---|---|
| 576 | 189.1 s | 189.1 s | 399 |
| 575 (5x5x23) | 252.8 s (+34 %) | 216.6 s (+15 %) | 432 |
| 574 (2x7x41) | 311.6 s (+65 %) | 212.1 s (+12 %) | 428 |
| 573 (3x191) | 887 s (+369 %) | 199.1 s (+5 %) | 430 |
| 572 (4x11x13) | 226.2 s (+20 %) | 203.0 s (+7 %) | 424 |

Layout node 384.33 GB at 576, 396.68 GB at 572-575; SHMEM pool 17.2 vs 17.7 GB per process. Other counts, default schedule (mod): 571 (prime) 2562 s, 570 239, 568 417, 567 215, 564 334, 560 201, 552 270, 540 233, 528 242, 512 223 s. Caveat: uneven schedules are modelled and plan-checked only (e16 used g = 12); open decision: a small test of an uneven `MN_GROUPS` (3-7 nodes) before relying on it.
**Kit wiring (target_kit.sh owned by b01, not edited).** `tools/nodescreen.sh --hold <job> --out <dir> --ecalc-dir ecalc --flagged-file <file>` (needs `make` and `make tests/t_nodechk`; `rc 3` = excluded hosts, `rc 2` = screen failed; on the target set `NS_MODULES` and `NS_UNLOAD` from `kit_modules.env`); run alone on the allocation before the plan check and again if the allocation is older than ~1 h. **Next then:** positive control (inject `--cold-gb 32`), a `t_nicbw` third stage, uneven `MN_GROUPS` test.

## §T10 - cold-path node-check probe vs the known slow host aac7-n01 (2026-10-11)

The stall (read from S34, not re-measured): `db_from_bi` upload 20.7-25.2 s against ~1 s elsewhere, follows the host (2 of 2 permuted runs), also 19-25 s in the reciprocal chain; Cached 113.7 GB then (S34) and 160.9 GB at S49.
**Probe.** `ecalc/tests/t_nodechk.c <gb> <reps> <cold_gb>` (kit option `--nodechk-cold-gb`, default 0 = off): mirrors ecalc's init (per APU `hipMemAddressReserve`, 2 GiB `hipMemCreate` + `hipMemMap`, one `hipMemSetAccess`, `hipMemset`, first touch of a fresh pageable buffer, one cold `hipMemcpy`); 12 s at 17 GB per APU, 21 s at 32 GB.
**Test (m, job 12511, aac7-n01 + aac7-n00 at once, 3 repeats each).** Old probe 0.017 s on all APUs on both hosts; cold 17.2 GB total per APU 1.17-1.19 / 1.52-1.54 / 1.93-1.95 / 2.06-2.07 s (n01) vs 1.18-1.25 / 1.53-1.56 / 1.93-1.96 / 2.06-2.09 s (n00); cold 32.2 GB 2.18-2.22 / 2.83-2.87 / 3.59-3.63 / 3.84-3.89 s vs 2.69-3.89 (apu0 noisy) / 2.84-2.87 / 3.57-3.61 / 3.84-3.88 s; touch of the host buffer 2.9-3.0 / 5.4-5.5 s vs 2.9-3.6 / 5.4 s. Total per APU grows with APU index (`hipMemCreate` serialises inside a process) and is equal on both hosts within 1-2 %. The only difference: n01 now had 14-21 GB Cached and 507 GB MemAvailable (was 113.7 and 160.9 GB Cached earlier), so the stall was not reproduced and the node state changed.
**Conclusion.** Not shown to catch the stall (negative control only: 0 false alarms; one 2x apu0 access step 1.0-1.6 s on the healthy host). Untested candidates: the stall needs a large page cache (not alone: n00 with 125-130 GB cached did not stall) or it is transient. Positive control needs a moment when ecalc itself stalls there (a real 1-node ecalc 1e9 with `ECALC_INIT_TL=1` plus the probe in the same job).
**Provisional policy line (thresholds not data-justified).** Optional `target_kit.sh --only build,nodechk --nodechk-cold-gb 16` (12 s per node); flag a node whose slowest-APU `apu<d>_total_s` or `touch_s` is > 2x the allocation median (healthy: spread <= 2 %); extra check only; keep §S49's per-node `rns_init` > 3x median check. Healthy references (m): 17.2 GB total 1.2 / 1.5 / 1.9 / 2.1 s for APU 0..3, touch 2.9 s; 32.2 GB 2.2 / 2.8 / 3.6 / 3.9 s, touch 5.4 s.
**Operational note.** `srun --overlap -c 192 --gpus-per-node=4` in a 2-node hold hung ("step creation temporarily disabled", ~13 min lost); `--overlap --mem=0 -c1 --gpus-per-node=4` works.

## §S49 - slow-host policy: per-host `nodechk` rehearsal (aac7 all hosts, aac6 PPAC)

Finished 2026-10-11 09:10 EDT: 15 hosts measured (13 aac7 + 2 aac6, m), none flagged, **including aac7-n01. Verdict: the `nodechk` 1 GB upload probe does NOT catch the S32/S34 stall.**
**Design.** Kit stage `nodechk` (`ecalc/target_kit.sh`, `tests/t_nodechk`): per node MemAvailable, Cached, huge pages and a 1 GB host-to-device upload per APU (median of 3); kit flags: slowest-APU upload > 3x median, MemAvailable < 90 % of median. Kit code needed no change (`--only build` on the aac7 login node, `--only nodechk --jobid`); on aac6 `kit_modules.env` is hand-written and `t_nodechk` built with plain `make`. Coverage by a queue item on the Job B hold and a login-node ad-hoc driver (one 1-node job per idle host, rules: our running nodes <= 5, never near a protected window, aac7-n01 first); an aac6 timer submitted one job on each of two nodes. The first ad-hoc attempts produced no data because the sbatch asked for no GPUs (fixed with `sbatch --exclusive --gpus-per-node=4`; post-deadline driver `archive:drivers/s49/s49_adhoc2_driver.sh`). Collection: `archive:drivers/s49/s49_collect.sh`, `s49_table.py`. Coverage limit: with two holds (6 nodes) the ad-hoc rule blocks all ad-hoc jobs, so only gaps and the queue item gave data; aac7-n61 is no longer listed by Slurm; aac7-n30 and n31 were measured by the Job A kit (PASS, upload 0.017 s, MemAvailable 439.7 / 439.9 GB).
**Results (m).** Upload 0.017 s on all four APUs on every host (aac7 13 hosts; median MemAvailable 439.8 GB, range 435.3-440.3, 1.1 %; Cached 81-166 GB; aac7-n01: 439.8 GB, Cached 160.9 GB; aac6-spx-a and -b 516.9 / 515.5 GB, Cached 0.6 GB, median 516.2 GB). Healthy spread in the upload is zero, so 3x trips above 0.051 s, far from any value seen. The probe is a warm 1 GB copy at 60 GB/s (a few ms), while the stall was seen in first-touch and registration of the large pools at init and in large uploads (hypothesis, untested); the host was also fine at 05:58 EDT (transient?).
**Proposals (untested, need a positive control).** (a) cold-path probe at real pool size (done as §T10), (b) a 1-node real run (1e9 gate, ~10 s) on each node comparing `rns_init`, (c) per-node init time of the first timed run, exclude > 3x median.
**Policy text proposed for the kit.** Request N + S nodes, S = 2 spares (~0.3 % at 576; measured flag rate 0 of 13, so S is set by stall rate seen in runs); run `target_kit.sh --only build,nodechk` (1-3 min); exclude every flagged node (`MNRUN_EXCLUDE_HOSTS` or Slurm `-x`: upload > 3x median, or MemAvailable < 90 % of median AND short of the run's per-node need, peak 412-415 GB per node at 6.441e10 digits per node m; healthy aac7 435-440 GB, aac6 515-517); more than S flagged: stop and ask for replacements; re-run if the allocation is older than ~1 h; not sufficient alone (add the cold probe or the init-time rule). Thresholds 3x, 90 %, S = 2 kept (no data argued for a change).
**Slow-host policy adopted by the user 2026-10-11 (check by A03).** `tools/initchk.py` flags a node whose first-run `rns_init` (`ECALC_INIT_TL=1`) or first device upload (`db_from_bi`, `NEWTON_MN_RECIP_TS=2`) is > 3x the allocation median (exit 3; `--threshold`, `--min-excess 2`); the cold probe stays off by default until a positive control exists. Measured (`archive:results/A03.md`): on the two real 10-node logs where aac7-n01 stalled (s32) the upload stall is 20.8x and 25.3x the median and is flagged, but its init is only 1.11x and 1.15x, so an init-only rule would not catch it; healthy runs stay at 0.8-1.15x (`rns_init`) over 8 real runs. Use `MNRUN_LABEL=1` so every metric is attributed to a host.

## §NIC - node screen catches nodes with fewer NICs than the launch asks for

Done 2026-10-13, branch `nicchk`, merged to main. **Problem (m).** 2026-10-13 12:55 three SHMEM runs (`COMM_OFI=0`, `SHMEM_OFI_NUM_NICS=4`) aborted within seconds (rc 141) on one node: "invalid number of nics 4 ... There is only 1 NIC available"; later runs on the same allocation passed (21 of 24); the screen checked only times.
**Change.** `tools/nodescreen.sh` local stage prints `NS nics have=<n> want=<w>` per node (`have` = distinct `fi_info -p cxi` domains the runtime sees after the SHMEM launch's module load; `want` = `--want-nics N`, default `$SHMEM_OFI_NUM_NICS`, 0 = report only); `nodescreen_tab.py` flags `have < want` as a failed step so the node lands in EXCLUDE. An option of the tool, no new setting; README tool row updated.
**Verification (m, 4 nodes, one queue item, local stage).** want 4: `have` 4, 4, 4, 4 (`/sys/class/cxi` also 4), EXCLUDE empty, rc 0; want 8 (positive control): all four flagged `NICs 4 < 8 wanted`, rc 3. Not reproduced on a real 1-NIC node; the login node has no cxi device (count 0), so the check is meaningful only on compute nodes. Use `tools/nodescreen.sh --hold JOB --want-nics 4` before SHMEM runs that set 4 NICs.

## §L1 - the NIC bank group is a fair per-launch coin (items 9cg 2 n, 9ch 4 n, 2026-10-19; m)

- **What picks the group: nothing we can set.** Bank A (`oxe_mcu_meas` 18-25) or B (26-33), chi-square against burn, idle gap
  (0 / 15 s), step id mod 2-4, VNI mod 2-4, child pid, previous launch's group: all below the critical value at 4 n (28
  libfabric launches, 14 A / 14 B; runs test 15 against 15.0 expected, sd 2.6), so a fair, memoryless draw. 2 n (32
  launches, 16 / 16) shows one weak alternation (p about 0.05 among 13 tests), not seen at 4 n. The CXI service is the same in
  every launch.
- **Job-wide.** The bank is the same on all 4 NICs of all 4 nodes of a launch (32 of 32 launches).
- **Speed at 4 n.** Bank B was fast in 14 of 14 launches (11.85-12.82 GB/s per thread). Bank A: 6 launches 9.0-9.6, 2 at
  10.45-10.48, 6 at 11.15-11.70; hard-slow (< 9.7) is 6 of 28 = 21 %. At 2 n the rate is equal in both groups.
- **SHMEM** uses a fixed layout, not a draw: NIC 0 and 3 on bank B, NIC 1 and 2 on bank A in every launch (4 n, 4 launches;
  2 n, 6 launches), at 9.65-9.83 GB/s (byte split 57 / 43 %). The same transport reaches libfabric's fast dm with whole puts
  to one peer (`archive:results_merged_2026-10-23/SHMEM_full.md`, send schedule), so the layout does not cap it (assumed for that run: its counters were not recorded).
- **Verdict.** No launch tweak is supported (earlier tclass, NUM_NICS, NIC policy, SEP, startup-connect and CXI_PID_BASE
  tests did not pin it either, §A14); detect-and-relaunch loses (relaunch costs init about 25 s plus a probe against 10-14 s
  saved in half the launches, mod). The pinning line is closed; compare arms interleaved and report the mode.

## §RAMDISK - node RAM-disk leftovers (items 9cj-9cm, 2026-10-19; m)

- Fresh-boot nodes start with 0.3 GB of `Shmem` (the root RAM-disk) and stay flat over our launches (SHMEM and libfabric, 1e9 and
  1.6e10 digits; 9cj, 9cl, 9cm). Between boots, leftovers of 9-80 GB build up (start levels seen: 0.3, 9, 23-27, 52, 79.8 GB);
  they persist across allocations and are freed only by a reboot.
- The writer is unidentified and not ours: no ecalc_px files, /dev/shm 20 kB, no deleted-open or memfd handles, our own files
  in the RAM-disk are two 476-byte Slurm spool files. A growth of 1.6 GB per launch on one dirty node (9ck) and +5 to +10 GB
  per allocation (9cd, 9ce) did not recur on clean nodes or on the 80 GB node.
- Cost: up to 80 GB (17 % of 480 GB) of node RAM lost on dirty nodes, relevant only when the plan's host need is near the
  budget. Mitigation: read MemFree at start (the PRE line records `memfree_min`) before a production-size run. No code change.

## §BM - the 3 n bimodal run times are not a node-local compute mode (items 9zs19-9zs29 data; 9zv3_bm2d, 2026-10-24; m)

**Existing data (59 runs at 3 n 1e11, 9zs19, 9zs21-9zs25, 9zs29).** 26 of 59 runs slow (ofi 7/21, Cray 19/38); slow runs +5.3 % total on ofi
(+6.5 % bs, +5.4 % dm) and +7.6 % on Cray (+9.1 % bs, +10.6 % dm); init equal. Not time-correlated: P(slow | previous slow) = P(slow | previous
fast) = 0.46 (Fisher p = 1.0), runs test z = -0.29. All 59 runs used the same three nodes, so a node property could not be tested.

**Node-local test (9zv3_bm2d).** Four nodes without cxi NICs, one stream each, layout D 1-node runs of 3.3e10 digits (the per-node work of
the 3 n 1e11 run, no network), 271 runs, all VERIFY OK. Compute time (total - init) per node: median 21.7 / 21.9 / 21.8 / 22.5 s, sd 0.36-0.49 s,
max 23.1-23.2 s; the distribution is continuous and unimodal (19 of 271 runs more than 4 % above the pooled median, most of them on the
node that is 3 % slower in every run). **No 5-8 % slow mode without the fabric**: the 3 n effect needs the network (the per-launch NIC state
of §L1, or fabric load) or more than one node. Next test when three nodes with working NICs exist: the 3 n ABBA with per-launch NIC
counters (plan item 9zv1_bm1); until then pair every A/B run (already the rule).
