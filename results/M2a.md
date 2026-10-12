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

## 4- and 6-node points (later)
Same line, tooling and per-node size, 4 nodes (2.5764e11 digits) and 6 nodes (3.8646e11), with COMM_XSTATS and MN_WAIT_STATS, alternating orders, 3+ runs each, taken in the next 4/6-node window (the soak ends 08:30; existing runs in `results/L4.md` 148 rows give the default schedule at 4 and 6 for reuse). Use a queue item with NODES 4 / 6 built from item 77 (change `SEQ` and `NODES`; the MN_GROUPS default is used). Refit on all four points together.

## Open issues
- 1-node at 6.441e10 digits: memory fit not yet confirmed on this build (4e10 ran at 1 node); a failed run shows as rc != 0 in the summary.
- Slow host exclusion is done by the daemon (`-x`).

## RESUME
Items armed; wait for `~/m2a/DONE` (about 04:35) and `~/m2a/DONE_b`; collect, fill the numbers here, then plan the 4/6-node items.
