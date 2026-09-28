# MAP15 — Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1

Agent MAP, branch `p15-MAP` from `int15d` 3e0a0db (= main B1 f184d51 + NP + AS + N3x, all off by default). aac6 clone
`~/ntt-MAP15`, logs `~/MAP15/<batch>/` (copied to `results/MAP15/`). Times Eastern (aac6 logs are Central: +1 h). Numbers are
labelled **measured** (a run's log), **modelled** or **assumed**.

## 1. Feasibility note

(in progress: the timeline run f1, job 21659, s24-30, started 22:21 EDT)

## RESUME

- 5162a8f: `ECALC_INIT_TL=1` (print only: `dbig.c` VMM mapping split by HIP call, `rns_init` steps, the seed thread, level 1–3),
  README row. 2add00f: `tests/map_sweep.sh` (from `as_sweep.sh`; summary adds the seed wait, the seed thread and the `tl` marks),
  `tests/map_plan_f1.txt`.
- Running: `~/MAP15/f1` (job 21659, s24-30), launched 22:21 EDT by
  `setsid nohup bash ~/ntt-MAP15/tests/map_sweep.sh 2add00f ~/MAP15/f1 ~/ntt-MAP15/tests/map_plan_f1.txt > ~/MAP15/f1.out`.
  The script cancels its own job. Next: read `~/MAP15/f1/summary.txt` and the `tl` lines, write §1.
