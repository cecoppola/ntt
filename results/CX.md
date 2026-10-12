# CX: correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes

Armed 2026-10-12 (Eastern, early morning). Driver `tools/cx_aac6.sh` (copied to `~/cx/driver.sh` on aac6). Collected; see Results.

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
Collected 2026-10-12 from `~/cx/summary.txt` and `driver.txt` (all measured). Driver ran 02:37-03:40 ET (three slots, one 45-min hold each, reused or renewed; no leftover steps). 7 of 7 builds PASS. "off/on" = the switch off/on; o3 and g6 have no switch (std). Times are the ecalc total of the run.

| branch | switch (on) | mnaccept e9 (1e9, LIMB_BASE 10 and 2) off / on | ecalc 4e9 off / on | 4e9 digit files cmp off vs on | unit sweep off / on |
|---|---|---|---|---|---|
| o5 | RNS_STRATEGY_FORM=B5 | PASS (2/2) / PASS (2/2) | PASS 20.43 s / PASS 19.73 s | PASS (identical) | t_ntt 24 FAIL (CPX speed check), 7 others PASS / same |
| k6 | RNS_DIST_OVERLAP=1 (RNS_STRATEGY=C) | PASS / PASS | PASS 21.15 / PASS 22.43 s | PASS | same as o5 |
| k1b | DIST_GATHER_RAW=1 (RNS_STRATEGY=C) | PASS / PASS | PASS 21.01 / PASS 20.63 s | PASS | same as o5 |
| o2 | DBIG_SMALL_BOUNCE=1 | PASS / PASS | PASS 18.98 / PASS 18.45 s | PASS | same as o5 |
| k8b | BS_SEED_PULL=1 | PASS / **FAIL (2/2)** | PASS 19.56 s / **FAIL** | FAIL (on run produced no digits) | off: same as o5; on: t_ntt 24 FAIL, t_patch rc 143 (see below), others PASS |
| o3 | none (std) | PASS (2/2) | PASS 19.11 s | n/a | same as o5 |
| g6 | none (std) | PASS (2/2) | PASS 19.00 s | n/a | same as o5; `--only sched` 3 of 3 FAIL (environment, below) |

o5 extras (arm on): `t_strategy 24 1 V` and `t_strategy 26 1 V` (LIMB_BASE=10 ECALC_NP=3) PASS; ecalc with RNS_STRATEGY_CHECK=1 at 1e9 (10.93 s) and 4e9 (30.02 s) VERIFY OK, 4 check lines each, so the B form was chosen and exercised at both sizes (no digit change).

Failures, classified:
- **t_ntt 24 (every branch, both arms, 12 of 12 unit runs): CPX limit, not a code defect.** "VERIFY FAILED (4 of 2360 checks)"; the same failure is on the `off` arms, and S44 shows it is the speed check (a CPX XCD has 1/6 of the APU bandwidth). All other 7 unit tests (t_mul 20 185 checks, t_bs, t_dbig 0 555, t_newton 20 620, t_verify 334, t_out, t_patch) PASS on every branch and arm except k8b/on t_patch.
- **k8b with BS_SEED_PULL=1: FAIL, a possible defect (or a CPX-specific hang), needs a look.** Both mnaccept e9 runs and ecalc 4e9 exit rc 3 after init with `ecalc: FATAL ... after init, rc 3: db_pull_wait: APU0's pull kernel did not finish copy 1 in 600 s` (the pull kernel's own 600 s watchdog), so each failing run cost 600 s (the e9 step took 1211 s, 4e9 607 s). The off arm of the same build is clean, so the switch is the cause. Not known whether it is CPX-only: the pull kernel waits on the XCD devices of one APU; run it on a 4-APU node before judging. In the unit sweep with the switch on, t_patch was killed (rc 143, SIGTERM, after printing "VERIFY OK (1 checks)"; cause not determined).
- **g6 `--only sched` (MNACCEPT_SCHED_PROCS=4 on one CPX node): 3 of 3 FAIL, environment limit.** "sets written by 0 nodes", "0 refusal lines", restart step "Exited with exit code" from the launcher: 4 node-processes need 4 nodes; not a G6 defect (expected in the plan above).

Summary: digits identical on o5, k6, k1b, o2 (switch on vs off, 4e9 files cmp, 1e9 mnaccept) and o3, g6 (std); the only switch failure is k8b BS_SEED_PULL=1. Speed on CPX (4e9, off to on): o5 -0.70 s, k1b -0.38 s, o2 -0.53 s, k6 +1.28 s: single runs, not timing claims.


## RESUME
COLLECTED 2026-10-12 (Results). Open: k8b BS_SEED_PULL=1 pull-kernel 600 s timeout on CPX (re-run on a 4-APU node; ask the k8b owner). Remove the `~/cx/<branch>` clones. Re-run one branch: `CX_BRANCHES=<b> CX_SLOTS=1 bash ~/cx/driver.sh`.
