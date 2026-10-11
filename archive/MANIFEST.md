# archive/ manifest

Moved out of the working tree on 2026-10-10 (repo cleanup, audit AUDIT_REPO). Nothing was deleted except the two
byte-identical copies listed under s43. History is kept by `git mv`; use `git log --follow <new path>`.
Moved scripts still carry their original relative `source`/`$HD` paths and will not run from here without
restoring that layout (copy them back next to `ecalc/`, `tools/rundriver.sh`). They are records, not live tooling.

## drivers/ecalc/ (from `ecalc/`, 52 files)
Per-session batch drivers and their libraries; none is called by the Makefile, `target_kit.sh`, `mnrun.sh`,
`mnaccept.sh`, `accept.sh`, `e16_headline.sh`, `aac7env.sh`, `digcmp.sh` or `tools/rundriver.sh`.
| files | session / phase | note |
|---|---|---|
| `s20_batch` .. `s23_batch`, `s29_m2`, `s32_batch`, `s33_batch`, `s34_batch` | S20-S23, S29, S32-S34 | A/B batches, results/S20..S34 notes |
| `s27/s28/s30/s31_lib.sh`, `s27/s28/s30/s31_batch.sh`, `s27/s28/s30/s31_xagg.awk`, `pairstats.awk`, `s32_pairsoak`, `s34_pairsoak`, `s34_stats.awk` | S27-S34 | source each other; moved as one set |
| `c314_batch`, `c314_batch2`, `variance.py` | phase 14 | |
| `dkm15_batch`, `dl15_batch`, `ew15_batch`, `k15_batch`, `int315_job`, `int315_plans`, `mpb15_job`, `np15_job`, `p15_job`, `p24_job`, `wm15_job`, `ladder1n`, `a14_soak`, `s14_batch`, `t10_d5`, `t10_d5b`, `t10_test`, `v11_d5`, `v11_recheck`, `g13d_chain`, `g13d_hang`, `g13d_run`, `n13_ckpt_test`, `ackpt_test`, `tgtb2_est` | phases 10-15 | |
Kept live in `ecalc/`: `target_kit.sh mnrun.sh mnaccept.sh accept.sh e16_headline.sh aac7env.sh digcmp.sh`
and, because docs or code still cite them, `closing.sh`, `variance.sh`, `wp6run.sh`.

## drivers/tools/ (from `tools/`, 7 files)
`s38_abba.sh s40_abba.sh s42_abba2.sh s42_hang.sh s44_abba.sh s44_node.sh s44_reg.sh` (S38, S40, S42, S44 A/B and hang
drivers; `s44_reg` calls `s44_node`, `s40_abba` calls `s38_abba`). `tools/rundriver.sh` and the rest stay.

## drivers/s43/ (from top-level `s43/`)
`s43_batch.sh s43_job.sbatch s43_stats.py`. Removed as byte-identical duplicates: `s43/rundriver.sh` (= `tools/rundriver.sh`)
and `s43/s31_lib.sh` (= `ecalc/s31_lib.sh`, now `drivers/ecalc/s31_lib.sh`); `s43_batch.sh` sources both from its own
directory, so to rerun copy them back next to it.

## drivers/kp15_batch.sh (from `results/`)

## docs/ (from the top level)
`CODE_REDUCTION.md DECISIONS.md DECISIONS2.md DECISIONS3.md DESIGN.md ALGORITHM.md`: banner HISTORICAL, superseded by
`docs/code/05_DECISION_REGISTER.md` and `docs/code/0*`. References in docs, PLAN, TASKS, RESULTS and results notes now
carry the `archive/docs/` prefix.

## results-raw/ (from `results/`)
Raw logs/data (tracked) of: `INT315 NP15 MPB15 CX15 T215 AS15 MS15 SC15 DL15 DOC215 N3x15 S115`; the report
`results/<X>.md` of each stays in `results/` and now points here. `loose/`: `D13b_calibrate.txt`, `D2_13d_plan1_C.txt`,
`D2_13d_steps1.txt`, `S13_e2.txt`, `e40b-report.html`.
Kept in `results/` because code or live docs read them: `L815` (tests/l8_*.py), `PC15 P2415 V314 P15`, the
`D13b_strategy_e0 D2_13d_plan576 L13d_plan576 S13_e0 mrun_13b` text files and the other small directories.

## docs/ (documentation consolidation, 2026-10-10)
`TASKS_HISTORY.md`: the old front of `TASKS.md` (Phase 12-13 log, sections 1-7); `ECALC_README_PREAMBLE_2026-10-05.md`: the
history of the defaults and of the target size that headed `ecalc/README.md`.

## Left in place on purpose
`diff run suite isa.py envcheck.sh nodecheck.sh` (top level): the microbenchmark suite is live (Makefile `make isa`,
`ecalc/README.md` uses `./run`, `suite` calls `envcheck.sh`, `bench/README.md` cites them). `PLAN.md` (now bannered as wholly historical),
`TASKS.md` (now the live list), `RESULTS.md` and `docs/` are not moved (user instruction).

## Archived experiment branches (tags, 2026-10-10)

Annotated tags `archive/<branch>` on GitHub cecoppola/ntt (branches deleted locally and on GitHub; recover with
`git checkout -b <branch> archive/<branch>`): archive/p14-L1, archive/p14-R1, archive/p15-M6, archive/p15-MAP,
archive/p15-RL, archive/p15-SX, archive/rl-fill, archive/s24 (NTT3P bench, shelved), archive/s25 (10-node drivers),
archive/s37. Branches fully merged into main (29, b7-vslot ... s45, tgtbench2) were deleted without tags.
Kept as branches: `s35` (crash fix, deferred), `claude/confident-mayer-yx23pt`.
