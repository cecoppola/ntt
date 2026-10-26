# NODE_SLOWMODE — the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts)

Scope: the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts). Condensed from the former per-item files (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`); original text, including the per-run table of all 93 four-node runs, in `archive:results_merged_2026-10-19/NODE_SLOWMODE.md`; raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modelled, a = assumed.

Sections moved to `archive:results_merged_2026-10-24/NODE_SLOWMODE.md` (2026-10-24 cleanup): §B04; §T10; §S49; §L1; §RAMDISK.

## Topic summary

- **What this is.** Two linked questions on aac7: (1) why 4-node runs at the same size come out either fast (~181-185 s) or slow (~208-214 s) (§T05, §T09, §A13, §A14), and (2) how to screen nodes before a long run (§B04, §T10, §S49, §NIC), including the known slow host aac7-n01.
- **Slow mode, current state (matches RESULTS.md).** A per-launch effect on the inter-node all-to-all: -21 % rate (10.9 vs 13.8 GB/s per APU thread, m), every network-bound dm product 25-30 % longer, compute and init unchanged, ~29-52 % of launches slow depending on the sample, memoryless between runs, independent of arm, node, rank order, hold, time of day, MemAvailable and other jobs (a quiet cluster still gave 2 of 6, §A13). The NIC traffic lands in one of two `oxe_mcu_meas` groups (A 18-25 slow, B 28-33 fast), drawn per launch; at 4 nodes the split is clean (A 10.62 vs B 12.02 GB/s per thread, 31 launches, §A14) and at 2 nodes the groups do not differ. **The cause that picks the group is not established**: no tested setting (traffic class, NUM_NICS, NIC policy, SEP, startup connect) pins it, and detect-and-relaunch loses (mod). Nothing adopted; for timing, run ABBA and record each run's mode.
- **Separate effects.** A reciprocal-chain spike (3-89 s instead of ~2.3 s, cold host state, 29 of 93 runs) and slow init in the first runs of a fresh hold explain the outliers above 213 s; they are not the bimodality (§T05).
- **Node screens.** `tools/nodescreen.sh` (+ `nodescreen_tab.py`, `initchk.py`) is the fast pre-run screen (44-58 s at 2 nodes, lean 25 s); it excludes nodes that fail, run >3x the median init or first-upload time (and >= 2 s above it), or see fewer cxi NICs than `--want-nics` (§NIC). The 1 GB upload probe and the cold-path probe do not catch the aac7-n01 stall because the host was healthy when tested (no positive control, §S49, §T10); the adopted slow-host policy is the 3x init/upload check of `initchk.py`. Not caught by any quick screen: slow mode, minutes-in stalls, intermittent `shmem_init` segfaults.
- **Superseded or open.** §T05's "mechanism unidentified" is narrowed by §A13/§A14 to a launch-time NIC state. The "65 corrections" report (§A13 part B) is not reproducible on main (settle with an aac6 TCP 4-process rerun). Uneven `MN_GROUPS` for excluded nodes (§B04) is modelled only.

## §T05 - why 4-node run totals are bimodal (fast ~181 s / slow ~208 s)

Analysis of existing logs only (no cluster jobs), session 2026-10-11: 93 four-node runs at 6.441e10 digits per node on aac7 (same 4 hosts in every run; sources: 40 runs of hold
12471, 2 of S48, 16 of S50, 12 + 23 of S51; the full per-run table is in the archive original). All numbers m.
**Answer.** The slow mode is a per-run communication slowdown: the inter-node all-to-all runs at 10.9 instead of 13.8 GB/s per APU thread (-21 %) on all four PEs at once, and every
network-bound dm product takes 25-30 % longer (constant ratio from first to last stage). GPU compute and init are unchanged. It is a two-state switch redrawn independently per run (~29 %
of runs slow): independent of arm, node, rank, ABBA position, preceding run, time of day, hold or MemAvailable.

(Condensed 2026-10-25: the detail, tables and per-run data are in `archive:repo_cl6_2026-10-25/results/NODE_SLOWMODE.md`.)

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

## §NIC - node screen catches nodes with fewer NICs than the launch asks for

Done 2026-10-13, branch `nicchk`, merged to main. **Problem (m).** 2026-10-13 12:55 three SHMEM runs (`COMM_OFI=0`, `SHMEM_OFI_NUM_NICS=4`) aborted within seconds (rc 141) on one node: "invalid number of nics 4 ... There is only 1 NIC available"; later runs on the same allocation passed (21 of 24); the screen checked only times.
**Change.** `tools/nodescreen.sh` local stage prints `NS nics have=<n> want=<w>` per node (`have` = distinct `fi_info -p cxi` domains the runtime sees after the SHMEM launch's module load; `want` = `--want-nics N`, default `$SHMEM_OFI_NUM_NICS`, 0 = report only); `nodescreen_tab.py` flags `have < want` as a failed step so the node lands in EXCLUDE. An option of the tool, no new setting; README tool row updated.
**Verification (m, 4 nodes, one queue item, local stage).** want 4: `have` 4, 4, 4, 4 (`/sys/class/cxi` also 4), EXCLUDE empty, rc 0; want 8 (positive control): all four flagged `NICs 4 < 8 wanted`, rc 3. Not reproduced on a real 1-NIC node; the login node has no cxi device (count 0), so the check is meaningful only on compute nodes. Use `tools/nodescreen.sh --hold JOB --want-nics 4` before SHMEM runs that set 4 NICs.

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
