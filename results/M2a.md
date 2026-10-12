# M2a - time-model refit, scaling study, 1- and 2-node points (aac7 queue items 77 / 78)

Status: armed 2026-10-12 03:34 ET (item 77 started at once on the 2 free nodes; item 78 queued behind it). Labels: measured / modelled / assumed.

## Goal
Opportunity M2: refit the time model (`ecalc/mn_model.py`, `calib_table.py` inputs; current fit target_l4, `results/L4.md`) to current code with a 1/2/4/6-node scaling study taken with `COMM_XSTATS` and `MN_WAIT_STATS`. This item takes the 1- and 2-node points while 2 nodes are otherwise idle (4 busy until about 04:40, then a 4-node soak to 08:30).

## Design
- Two queue items, `77_m2a_scale12.sh` and `78_m2a_scale12b.sh` (same file; the file name selects tag `a` / `b`), each NODES 2, EST_MIN 60, pre-approved.
- Binary: main's SHMEM build in the existing aac7 worktree `main` baseline (commit 0298fe2, not modified; the binary is copied per item and checked for SHMEM).
- Line: the `LINE=` of `e16_headline.sh` plus `COMM_XSTATS=1 MN_WAIT_STATS=1 ECALC_INIT_TL=1 ECALC_SEGV_TRACE=1`; no digit writes. Size 6.441e10 digits per node (1 node 6.441e10, 2 nodes 1.2882e11).
- Order 1 2 2 1 1 2 2 1 ... (ABBA-style alternation of node count), up to 12 runs, no new run after 48 min. Page cache of the references evicted before each run; one network program per node; every step with `--overlap --mem=0`, no `-w`; `mnrun.sh` with `MNRUN_NODES=n` runs the 1-node points on the first node of the 2-node hold.
- Per run recorded: rc, wall, total, bs / recip / dm stage times, init, VERIFY, xstats GB and `t_wait` of PE 0, NIC MCU group (counter sets `oxe_mcu_meas_18..25` = A vs `28..33` = B on every cxi device of the used nodes, snapshot before and after, same method as C02). Full logs (xstats and wait-stats lines) are kept.

## Arm / outputs
Arm: file moved into `~/aac7q/queue/` (daemon already running). Outputs `~/m2a/a/` (item 77) and `~/m2a/b/` (item 78): `log/<run>.log|.plain|.wait`, `nic/`, `summary.tsv`, `summary.txt`. Done-markers `~/m2a/DONE` (77) and `~/m2a/DONE_b` (78); the marker holds `SUCCESS|FAILED ok=<n> bad=<n> <epoch>`. Item `.rc` and `.log` sit next to the item in the queue dir.
Dry run (measured, stub): both items through the daemon in STUB mode (no Slurm): syntax, binary SHMEM check, line extraction OK, rc 0 each. The real measurement path follows the 51_h4_abba2 / 61_c03_time items; it was not run before arming.

## Collection (cheap; no large agent)
`cat ~/m2a/DONE ~/m2a/DONE_b; cat ~/m2a/a/summary.txt ~/m2a/b/summary.txt`. Then feed the `.log` files to `python3 -I ecalc/calib_table.py <logs> --tsv m2a.tsv` (calib_table reads xstats and stage times) and compare the 1- and 2-node rows with `ecalc/mn_model.py` / target_l4 predictions; raw data to `archive/results-raw/`.

## Results, 1- and 2-node points (collected 2026-10-12; all numbers measured)
Setup: main's SHMEM build, e16 aac7 launch line, COMM_XSTATS and MN_WAIT_STATS, no digit writes, 6.441e10 digits per node (1 node 6.441e10, 2 nodes 1.2882e11), order 1 2 2 1 1 2 2 1 1 2 2 1. Item 78 (`~/m2a/b/`) collected from its summary.tsv. Item 77's summary files in `~/m2a/a/` were overwritten by a later item (7a, same output dir); item 77 was rebuilt from its saved raw logs (`~/m2a/a77_saved/log/*.plain`, all 12 runs) with the item script's own parsing (`^total`, `^bs`, `^recip`, `^dm`, `init` in the total line, xstats GB / t_wait of PE 0). 24 of 24 runs rc 0 and VERIFY OK. Item 7a will write to `~/m2a/a/` again (marker `~/m2a/DONE`).

Mean (sd) in seconds, per item. "excl r1" drops run 1 of the 1-node arm: in item 77 it ran 87.44 s (bs 48.8, init 17.5), in item 78 80.68 s (bs 43.4, init 16.5), a cold-start run (page cache / first launch after the allocation); the other five 1-node runs agree to under 1 s.

| item | nodes | n | total | bs | recip | dm | init |
|---|---|---|---|---|---|---|---|
| 77 | 1 (all) | 6 | 76.57 (5.36) | 41.45 (3.60) | 5.12 (0.11) | 20.79 (0.12) | 14.17 (1.72) |
| 77 | 1 (excl r1) | 5 | 74.39 (0.64) | 39.98 (0.11) | 5.09 (0.09) | 20.75 (0.09) | 13.50 (0.60) |
| 77 | 2 | 6 | 136.41 (2.69) | 48.31 (0.97) | 16.90 (2.75) | 55.69 (2.92) | 28.70 (1.68) |
| 78 | 1 (all) | 6 | 75.30 (2.72) | 40.62 (1.36) | 5.04 (0.09) | 20.66 (0.12) | 13.85 (1.38) |
| 78 | 1 (excl r1) | 5 | 74.23 (0.75) | 40.07 (0.26) | 5.03 (0.10) | 20.66 (0.14) | 13.32 (0.54) |
| 78 | 2 | 6 | 137.97 (4.18) | 48.04 (1.49) | 17.27 (1.97) | 57.30 (3.68) | 28.87 (4.51) |

- Pooled over both items (excl r1): 1 node 74.31 s (n 10), 2 nodes 137.19 s (n 12). The two items agree within 1.6 s at 2 nodes and 0.2 s at 1 node (excl r1): reproducible.
- Per-node efficiency (weak scaling, same digits per node): E = T1 / T2 = 74.31 / 137.19 = **0.54** (77: 0.545, 78: 0.538, excl r1), i.e. at the same digits per node the 2-node run takes 1.85 x the 1-node time, so each node delivers 54 % of its 1-node digit rate. Part is the digit count itself (2 x the digits cost slightly more than 2 x the work, n log n), most is communication: the 1 -> 2 node step adds bs +8.1 s (+20 %), recip +12.0 s (3.4 x), dm +35.8 s (2.7 x), init +15.3 s (2.1 x).
- Comm: XSTATS GB of PE 0 is 0 at 1 node and 2739.3 at 2 nodes (identical in all 12 runs); summed t_wait of PE 0 is 139.8 s (77) / 142.3 s (78) at 2 nodes (sd 5.4 / 4.8). The NIC counters show the traffic on one MCU group per run (item 78: A in 9 of 12 runs, B in 2, 1 unknown; B runs 135.7 and 135.0 s vs 2-node A runs 132.7-143.6 s: no separation, n too small).
- Run-to-run spread at 2 nodes is dominated by dm and recip (item 77 run 11: recip 22.5, dm 61.6 s; item 78 run 7: recip 20.0, dm 63.3 s), bs is stable (sd 1.0-1.5 s).

How this feeds the model refit (not refitted here): (1) the 1-node anchor (74.3 s at 6.441e10) fixes the single-node stage terms bs 40.0, recip 5.0, dm 20.7; the cold first run must be excluded or flagged. (2) The 2-node point is the first multi-node anchor at the target per-node size: it supplies the per-stage communication step (bs +8.1, recip +12.0, dm +35.8 s) that the refit of `ecalc/mn_model.py` / target_l4 needs to separate fixed latency from per-GB volume (XSTATS 2739 GB, t_wait about 141 s). (3) Compare these three stage steps with the model's 2-node predictions before using the 4- and 6-node points; the 2-to-4 and 4-to-6 steps are still needed to fit the node-count dependence (do not extrapolate to 576 nodes from one step). Feed the `.log` files to `ecalc/calib_table.py` as planned above.

## Third batch: item 7a (collected 2026-10-12, aac7; all numbers measured)
Item 7a: same line, binary (main 0298fe2), order 1 2 2 1 1 2 2 1 1 2 2 1, 6.441e10 digits per node, marker `SUCCESS ok=12 bad=0`, 05:45-06:09 ET. It wrote `~/m2a/a/summary.txt` and `summary.tsv` (item 77's data are the saved logs in `~/m2a/a77_saved`, section above). 12 of 12 runs rc 0 and VERIFY OK.

Mean (sd) in seconds. "excl r1" drops 1-node run 1 (81.42 s, bs 46.2, init 14.5: cold first run, as in 77 and 78); "excl first 2" drops 2-node run 2 (167.85 s, init 42.6, t_wait 226.5 s: the first 2-node run, the slowest of all 30 runs of the three items).

| nodes | n | total | bs | recip | dm | init |
|---|---|---|---|---|---|---|
| 1 (all) | 6 | 75.70 (2.86) | 41.25 (2.48) | 5.11 (0.06) | 20.59 (0.07) | 13.70 (0.49) |
| 1 (excl r1) | 5 | 74.56 (0.67) | 40.25 (0.48) | 5.13 (0.05) | 20.60 (0.08) | 13.54 (0.33) |
| 2 (all) | 6 | 143.69 (12.64) | 49.29 (2.41) | 18.20 (3.18) | 58.70 (5.26) | 31.67 (5.50) |
| 2 (excl first) | 5 | 138.85 (4.95) | 48.55 (1.77) | 17.20 (2.27) | 57.07 (3.82) | 29.48 (1.38) |

- Agrees with items 77 and 78: 1 node 74.56 s (77: 74.39, 78: 74.23, excl r1), 2 nodes 138.85 s excl first (77: 136.41, 78: 137.97; 143.69 s with all runs). Pooled 1 node (15 runs excl r1): 74.4 s; 2 nodes, the 18 runs of the three items: 139.4 s (item means 136.4 / 138.0 / 143.7).
- Step 1 to 2 nodes, 7a excl first/r1: total +64.3 s, bs +8.3, recip +12.1, dm +36.5, init +15.9 (77/78: bs +8.1, recip +12.0, dm +35.8, init +15.3): the same stage steps. E = T1/T2 = 74.56 / 138.85 = 0.54.
- Comm: XSTATS GB of PE 0 2739.3 at 2 nodes in all 6 runs (as before); summed t_wait of PE 0 is 133-226 s, median 137 s (mean 158 s with the first run, 138 s without); the five later runs are 133.1-144.2 s, 77/78 had 139.8 / 142.3 s mean. NIC counters: A group in 4 of the 6 two-node runs and B in 2 (runs 2 and 11); the B runs were 167.85 s (first run) and 135.07 s, so no separation.
- Reading: a first run after an allocation is slow at both node counts (1 node +7 s, 2 nodes +29 s over the later runs); keep it out of the fit or flag it. The three batches give 16 steady-state-quality points for the model refit; the 4- and 6-node points are still open.

## 4- and 6-node points (later)
Same line, tooling and per-node size, 4 nodes (2.5764e11 digits) and 6 nodes (3.8646e11), with COMM_XSTATS and MN_WAIT_STATS, alternating orders, 3+ runs each, taken in the next 4/6-node window (the soak ends 08:30; existing runs in `results/L4.md` 148 rows give the default schedule at 4 and 6 for reuse). Use a queue item with NODES 4 / 6 built from item 77 (change `SEQ` and `NODES`; the MN_GROUPS default is used). Refit on all four points together.

## Open issues
- 1-node at 6.441e10 digits: memory fit not yet confirmed on this build (4e10 ran at 1 node); a failed run shows as rc != 0 in the summary.
- Slow host exclusion is done by the daemon (`-x`).

## RESUME
Items 77, 78 and 7a COLLECTED (Results and Third batch); nothing runs. Next: the 4- and 6-node points and the refit. Was: wait for `~/m2a/DONE` (about 04:35) and `~/m2a/DONE_b`; collect, fill the numbers here, then plan the 4/6-node items.
