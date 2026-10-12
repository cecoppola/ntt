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
- What is known about the target (user-supplied 2026-10-12, a; re-assessed in `results/TGT.md`):

  | item | target | aac7, for comparison |
  |---|---|---|
  | nodes × APUs | 576 × 4 MI300A = 2304 APUs | 6 nodes per user × 4 MI300A |
  | NICs | 8 Slingshot NICs per node at 400 Gb/s: 2 per APU, 100 GB/s injection peak per APU, 400 GB/s per node | 1 per APU |
  | OS, scheduler | Rocky Linux 9.6, Slurm 25.11.8 | Slurm 25.11.4 |
  | ROCm | 7.14.0 (may be upgraded later); builds ecalc on aac7 (m), speed against 7.2.4 not measured | 7.2.4 on the line |
  | SHMEM, compiler, libfabric | cray-shmem 12.1.0, CCE 21.0.0, libfabric 2.3.1 | OpenSHMEMX 11.8.0, CCE 21.0.0, libfabric 2.3.1 |
  | file system | ≈ 1 TB/s cumulative write into one file at 32 processes per node | — |

  Still unknown: the measured injection rate per APU (kit A3), the per-message cost, the dragonfly group size, the
  write rate of ecalc's own writer (two part files per node from 8 threads, not the file system's measured mode;
  OPPORTUNITIES N8), the host memory per node and the site's module names.
- The digits must be right without a reference: every run checks itself (`ALGORITHM.md` §10).
- Where the project stands against this goal, with the standing estimate and the memory margin: section 2.

## 2. Current status (2026-10-12)

**What works.** ecalc computes ⌊10^d·e⌋ with decimal limbs end to end (binary splitting, Newton reciprocal, two-half
division, packed output) and verifies itself on every run (T1 residues, T2 windows; `ALGORITHM.md` §10). It runs at 1-6
nodes on the multi-node test system and at one node on the second test system, with digits identical to GMP reference
digits up to 10¹¹.

| scale | measured time | source |
|---|---|---|
| 1 node (aac6), 6.441 × 10¹⁰ digits, with the three speed defaults adopted 2026-10-12 (`DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL`) | 66.59 s against 73.74 s without them (paired -7.16 s [-8.08, -6.23]) (m, 2026-10-12) | `results/CAND.md` |
| 2 nodes (aac7), 6.441 × 10¹⁰ digits per node, same three defaults | 134.51 s against 140.10 s without them (pooled 12 pairs, -5.58 s [-10.54, -0.63]) (m, 2026-10-12); one gate run after the wave 2 removals: 134.0 s (m) | `results/CAND.md`, `results/W2B.md` |
| 1 node, 6.441 × 10¹⁰ digits, before the 2026-10-12 defaults | 76-78 s (m) | `RESULTS.md`, `results/S48.md` |
| 1 node, 10¹¹ digits | 129.3 s (m, before the 2026-10-12 defaults) | `results/L2.md` |
| 1 / 2 / 4 / 6 nodes at 6.441 × 10¹⁰ digits per node (scaling, aac7) | 74.4 / 138.9 / 188.2 / 246.4 s; efficiency vs 1 node 0.54 / 0.40 / 0.30 (m, 2026-10-12 overnight, before the 2026-10-12 defaults; the 6-node runs had not settled: 224.7-271.4 s) | `results/M2a.md` |

The 60-bit packed output (`ECALC_OUT_PACKED=2`) is the default since 2026-10-12 (batch D5, `results/W2A.md`).
The wave 2 settings removals changed no timing at the defaults (`results/W2A.md`, `results/W2B.md`).

**Not yet done.** ecalc has not run on the target machine. The 576-node layout fits with 12.9 GB of device margin
(mod); the standing estimate for 3.82 × 10¹³ digits is 185.5 s without the write (mod, profile `target_l4`, `results/TGT.md`; at the
user-supplied 100 GB/s injection peak per APU (a for the achieved rate)); the write adds 12.8 s at ecalc's assumed 1.0 GB/s per node
and 1.0 s at the file system's 1 TB/s (mod, `results/TGT.md`); the target has run only the memory and exchange benchmarks (`results/TGTBENCH2.md`). The
largest multi-node cost today is the distributed division multiply (+94 s at 6 nodes against 1, m).

**Road to the target.** Code reduction (settings removal) and documentation are done; next the correctness defect TASKS 23, then
performance work on the test system, then the target session (section 4).

## 3. Defaults and launch lines

The code defaults are the tested configuration; nothing has to be set for a one-node run. The full list, with the date
and evidence of each, is the switch table in `ecalc/README.md`.

The multi-node launch line sets, in addition: the transport (`COMM_TRANSPORT=shmem`, Cray contexts per thread, the
symmetric heap in device memory, SHMEM rounds of 1 GiB), `ECALC_NP=auto`, `MN_T_CHUNK_MB=2048`, the memory guard, the
crash-trace handler and the verbose memory lines. Every multi-node product runs the four-prime 24-digit form P24 (user decision
D18, 2026-10-12: the code default at every size > 1, as the target always had from 32 nodes; `results/MEAS-P24.md`: −8.9 s
at 2 nodes, +8.6 GB per APU, m; `results/DEF.md`). The three-prime multi-node path stays selectable for A/B runs with
`ECALC_NP_MN3=1` (`tools/gate.sh --mn-primes 3` gates it). The transform-cache fit and partial cache, the early write after
division step 1, the lean division and the X2 exchange with slot sharing and release are code defaults (TASKS 27,
`results/W2B.md`; each has a `=0` fallback). The two lines are kept in:

| system | line | source |
|---|---|---|
| aac7 | `LAUNCH_LINE_KEYS` of `profiles/aac7.conf` (`tools/profile_load.py aac7 --line`) | `tools/gate.sh --line aac7` runs it; one source (D15) |
| target | `profiles/target.conf.template` (site values `__FILL__`); the filled copy stays out of git | `ecalc/target_kit.sh` |

`mnrun.sh` also sets, when unset, `FI_UNIVERSE_SIZE` (≥ 4 × tasks), `FI_LOG_LEVEL=warn` and
`FI_MR_CACHE_MONITOR=userfaultfd`; it refuses `FI_MR_CACHE_MONITOR=disabled` (silent wrong exchanges, m).

Profiles (`profiles/*.conf`, loaded by `tools/profile_load.py`, precedence: explicit environment > profile > code default)
hold what is machine-specific. Settings adopted on both lines are code defaults (TASKS 27).

Size-dependent values are computed, never typed (TASKS 27, `results/T27.md`): the SHMEM pool (`COMM_SHMEM_POOL_MB`, from
the planner `MN_PLAN_ONLY`), the library heap (pool + 512 MiB), `FI_UNIVERSE_SIZE` (`mnrun.sh`) and the tree schedule
`MN_GROUPS` (the code's default). `tools/plan_values.py [--profile P] [--line FILE] [--exports] <digits> <nodes>` prints
them without launching, checks a hand-typed line against them, and prints `export` lines for a launch without `mnrun.sh`.
At 3.82 × 10¹³ digits on 576 nodes the plan asks for a pool of 2304 MiB and a heap of 2816 MiB with the current defaults
(m, planner on the aac7 login node); a typed 1536 / 2048 MiB is right only with `COMM_LAYER_VSLOT_POOL=0`. User decision D9 (a), 2026-10-12: the target launch block does not type pool or heap; it takes them from `tools/plan_values.py --profile target --exports <digits> <nodes>`.

Every setting that is not a plain default (experiments, diagnostics, test-harness switches, fallbacks kept after a default
flipped) has an owner and a review date (`ecalc/switches_review.tsv`; the table in `ecalc/README.md`);
`tools/switch_registry.py --check` warns, without failing, once a date has passed.

## 4. Plan to the target run

**Remaining work in this repository** is in `TASKS.md`: the forced-grid correction defect at 4
nodes (TASKS 23), performance on aac7 (largest item: the distributed division multiply).

**Target session** (the user runs it; the steps are in `ecalc/target_kit.sh -h`):
1. Fill `profiles/target.conf` from the template (modules, ROCm, NICs, partition, memory); `tools/profile_load.py target --check`.
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
| the write short of the file system's 1 TB/s (one file, 32 writers per node; ecalc: 2 files per node, 8 threads) | `tools/wbench` at scale in the kit, `MN_OUT_THREADS=32`; a shared-file writer if needed (OPPORTUNITIES N8): 12.8 s exposed at 1.0 GB/s per node (mod) |
| SHMEM has to carry the device exchanges (cray-shmem 12.1.0 untested) | `COMM_OFI=0` A/B on aac7 first (OPPORTUNITIES N7); pool 10240 MiB then (m, planner) |
