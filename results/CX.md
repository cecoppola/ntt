# CX: correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes

Armed 2026-10-12 (Eastern, early morning). Driver `tools/cx_aac6.sh` (copied to `~/cx/driver.sh` on aac6). Nothing here is measured yet:
the tables below are filled at collection from `~/cx/summary.txt`.

## What runs
- One git bundle of the seven branches (`o2 o5 k6 k1b k8b o3 g6`, tips of GitHub after tonight's rewrite) -> clone + `make -j12` per
  branch in `~/cx/<branch>` (module rocm), three builds at a time.
- Three slots, each with one 45-min hold on a CPX node (`SH5_MI300A_CPX,PPAC_MI300A_CPX`; the 4-APU SPX nodes are not used),
  taking branches from a queue (o5 k6 k1b o2 k8b o3 g6); a hold is reused while >= 31 min remain, else cancelled and a new one taken.
  Only its own holds are cancelled (also on exit); `~/cx/STOP` stops after the current branch.
- CPX environment on every run: `RNS_INIT_POOL_LOG=29 ECALC_PLANE_CAP=2^30` (S44, T11, o4); CPX tops out near 4e9 digits.
- Per branch, arm `off` then arm `on` (o3, g6: one arm `std`, no switch):
  1. `mnaccept.sh --only e9 --skip-grid`: 1e9 decimal and LIMB_BASE=2, digits cmp'd against the reference.
  2. `ecalc 4000000000` (VERIFY OK); the digit files of `off` and `on` kept on the node and compared with `cmp` (all sibling part files).
  3. `mnaccept.sh --only unit --skip-grid` (t_ntt 24, t_mul 20, t_bs, t_dbig 0, t_newton 20, t_verify, t_out, t_patch; `t_mn_grid`
     skipped: it needs two node-processes).
- Switches (arm `on`): o2 `DBIG_SMALL_BOUNCE=1`; o5 `RNS_STRATEGY_FORM=B5`; k6 `RNS_DIST_OVERLAP=1` and k1b `DIST_GATHER_RAW=1`
  (both arms with `RNS_STRATEGY=C` so the changed path runs at these sizes); k8b `BS_SEED_PULL=1`.
- Extras: o5 (arm on) `LIMB_BASE=10 ECALC_NP=3 tests/t_strategy {24,26} 1 V` and ecalc at 1e9 and 4e9 with `RNS_STRATEGY_CHECK=1
  RNS_VERBOSE=1` (the number of check lines is reported: 0 means the B form was not chosen, so B5 was not exercised);
  g6 `mnaccept.sh --only sched` with `MNACCEPT_SCHED_PROCS=4` (4 node-processes on one CPX node: may fail for lack of devices, which
  would be an environment limit, not a G6 defect).
- Known CPX baseline: `t_ntt 24` fails only its speed check (S44: a CPX XCD has 1/6 of the APU bandwidth); compare against the `off` arm.

## Outputs
`~/cx/summary.txt` (branch | arm | test | PASS/FAIL | s | note), marker `~/cx/DONE`, `~/cx/driver.txt` (holds, slot timeline),
logs `~/cx/<branch>/out/`, build logs `~/cx/<branch>.build.log`.

## Results
(pending: filled at collection)

## RESUME
Collect: `ssh <cluster> 'ls ~/cx/DONE && cat ~/cx/summary.txt ~/cx/driver.txt'`; copy the table above, mark each FAIL as a code
defect or a CPX limit (compare off vs on), then remove `~/cx/<branch>` clones. Re-run one branch: `CX_BRANCHES=<b> CX_SLOTS=1 bash ~/cx/driver.sh`.
