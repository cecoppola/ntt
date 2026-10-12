# PLAN — objectives, status and the plan to the target run

Scope: the goal, the current status, defaults and launch lines, and the road to the target run. | Maintained: updated in the
same merge as any change to the status, the goal or the launch lines.

The design (what is computed and how) is in `ALGORITHM.md`; how to check a change is in `TESTING.md`; every environment
switch with its default and evidence is in `ecalc/README.md`; open work is in `TASKS.md`; what is in force and its
evidence is in `RESULTS.md`; agent reports are in `results/`. The earlier phase-by-phase plan and status log is kept
outside the repository as `archive:PLAN_HISTORY_2026-10-12.md` ("PLAN §n" citations in older notes point there).

Labels: **m** measured, **mod** modelled, **a** assumed.

## 1. Goal

- Compute ⌊10^d · e⌋ in decimal, for d = **3.82 × 10¹³** digits, on **576 nodes** of four AMD MI300A APUs each (user
  decision 2026-10-12). The target machine is never accessed from this project; the user runs the test kit there.
- Constraints per node: device memory ≤ **373.44 GB** (the measured `hipMalloc` edge on the target, 93.36 GB per APU, m),
  host total ≤ **480 GB** (the budget check's default). Each node has 8 NICs (two per APU) on the target and one per APU on aac7.
- The digits must be right without a reference: every run checks itself (`ALGORITHM.md` §10).
- Where the project stands against this goal, with the standing estimate and the memory margin: section 2.

## 2. Current status (2026-10-12)

**What works.** ecalc computes ⌊10^d·e⌋ with decimal limbs end to end (binary splitting, Newton reciprocal, two-half
division, packed output) and verifies itself on every run (T1 residues, T2 windows; `ALGORITHM.md` §10). It runs at 1-6
nodes on the multi-node test system and at one node on the second test system, with digits identical to GMP reference
digits up to 10¹¹.

| scale | measured time | source |
|---|---|---|
| 1 node, 6.441 × 10¹⁰ digits | 76-78 s (m, before the 2026-10-12 defaults) | `RESULTS.md`, `results/S48.md` |
| 1 node, 10¹¹ digits | 129.3 s (m) | `results/L2.md` |
| 2 / 4 / 6 nodes at 6.441 × 10¹⁰ digits per node | 139.0 / 181.8 / 293.5 s (m) | `results/S48.md` |

**Not yet done.** ecalc has not run on the target machine. The 576-node layout fits with 12.9 GB of device margin
(mod); the standing estimate for 3.82 × 10¹³ digits is about 182 s without the write (mod, profile `target_l4`, at an
assumed fabric rate (a)); the target has run only the memory and exchange benchmarks (`results/TGTBENCH2.md`). The
largest multi-node cost today is the distributed division multiply (+94 s at 6 nodes against 1, m).

**Road to the target.** Code reduction and documentation (done or in progress), then correctness defect TASKS 23, then
performance work on the test system, then the target session (section 4).

## 3. Defaults and launch lines

The code defaults are the tested configuration; nothing has to be set for a one-node run. The full list, with the date
and evidence of each, is the switch table in `ecalc/README.md`.

The multi-node launch line sets, in addition: the transport (`COMM_TRANSPORT=shmem`, Cray contexts per thread, the
symmetric heap in device memory, SHMEM rounds of 1 GiB), `ECALC_NP=auto`, the transform-cache fit and partial cache,
the early write after division step 1, the lean division, `MN_T_CHUNK_MB=2048`, the X2 exchange (`COMM_LAYER_INTER2`,
`COMM_LAYER_VSLOT_POOL`) with slot sharing and release, the memory guard, the crash-trace handler and the verbose memory
lines. The two lines are kept in:

| system | line | source |
|---|---|---|
| aac7 | the `LINE` of `ecalc/e16_headline.sh`; profile `profiles/aac7.conf` | `tools/gate.sh --line aac7` parses it |
| target | `profiles/target.conf.template` (site values `__FILL__`); the filled copy stays out of git | `ecalc/target_kit.sh` |

`mnrun.sh` also sets, when unset, `FI_UNIVERSE_SIZE` (≥ 4 × tasks), `FI_LOG_LEVEL=warn` and
`FI_MR_CACHE_MONITOR=userfaultfd`; it refuses `FI_MR_CACHE_MONITOR=disabled` (silent wrong exchanges, m).

Profiles (`profiles/*.conf`, loaded by `tools/profile_load.py`, precedence: explicit environment > profile > code default)
hold what is machine-specific. Settings adopted on both lines become code defaults (TASKS 27).

## 4. Plan to the target run

**Remaining work in this repository** is in `TASKS.md`: code reduction (wave 2), the forced-grid correction defect at 4
nodes (TASKS 23), performance on aac7 (largest item: the distributed division multiply), and the documentation pass.

**Target session** (the user runs it; the steps are in `ecalc/target_kit.sh -h`):
1. Fill `profiles/target.conf` from the template (modules, ROCm, NICs, partition, memory); `tools/profile_check.sh`.
2. Kit preflight: environment record, build (`-DCOMM_SHMEM`), libfabric memory monitor (uffd), plan check
   (`MN_PLAN_ONLY` must end with `plan check ... OK`), the memory edge, a node screen (`tools/nodescreen.sh`).
3. Measurements: A3 (`comm_ofi` injection per NIC, one vs two NICs per APU) and A4 (all-to-all scaling with node count).
   These replace the assumed fabric rate in the model; re-run `estimate.py` and recheck the size against the grid steps.
4. A/B on the target of `MN_T_CHUNK_MB` 1024 vs 2048 and X2 on vs off (aac7 evidence only so far).
5. Growing runs (a few nodes, then larger groups), each VERIFY OK; then the 576-node run with the write, RECHECK and
   `unpack_digits` off the clock, and a prefix compare against a 10¹¹ reference.

**Not rehearsed on aac7** (results/A06.md): the 2-NIC A3 stage, an 8-node A4, the s2chk stage, the `--spares` path and
the plan-REFUSED path.

**Risks.**

| risk | handling |
|---|---|
| fabric rate at 576 nodes below the assumption | the kit measures it first; the size can drop to the next grid step |
| a slow or faulty node | node screen and `initchk.py` before the run; exclusion only (`MNRUN_EXCLUDE_HOSTS`), no spares; any 572-576 nodes pass the plan check |
| device memory near the edge | budget check (rc 8) before compute; guard (rc 9) during; 12.9 GB margin (mod) |
| hangs in the ROCr / libfabric memory registration | `FI_MR_CACHE_MONITOR=userfaultfd` (0/104 hangs against 22/105, m) |
| crash without a trace | `ECALC_SEGV_TRACE=1` on the launch line |
| wrong digits | T1/T2 in the run, RECHECK and `unpack_digits` after it |
