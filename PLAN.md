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
- What is known about the target (user-supplied 2026-10-12, a; re-assessed in `results/TARGET.md §TGT`):

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

## 2. Current status (2026-10-25)

**What works.** ecalc computes ⌊10^d·e⌋ with decimal limbs end to end (binary splitting, Newton reciprocal, two-half
division, packed output) and verifies itself on every run (T1 residues, T2 windows; `ALGORITHM.md` §10). It runs at 1-6
nodes on aac7 (the only test cluster; aac6 is set aside), digits identical to GMP reference digits up to 10¹¹ and
identical between the two supported multi-node paths (sha 578f5efb0ff2b9af at 2 n 1e11 on main after Q96, m).

**Two supported multi-node paths (Q85).** Build and run lines are in `README.md`. Path 1: libfabric
(`COMM_TRANSPORT=xshmem` over `xshmem_ofi.c`, layout D, the default, Q73). Path 2: Cray SHMEM (`XSHMEM=cray`,
`xshmem_cray.h`, one process per GPU, `XSHMEM_LANES=2`; the target shape). Path 2 is +7.5 % against path 1 at 3 nodes,
10¹¹ digits (CI +3.4 to +11.5 %, 11 interleaved pairs), the whole gap is start-up (init +8 s, aac7-specific). The
exchange worker is the default in every layout (Q96: init -6.5 to -7.5 s, host memory halved, bs and dm neutral). The
plain SHMEM transport (`COMM_TRANSPORT=shmem`) and `COMM_TRACE` were removed 2026-10-25 (Q103; the proxy mode and the
virtual-PE thread mode earlier, Q78, Q86); the transport is xshmem on both paths. `XSHMEM_PEER` is on by default under one process per GPU (Q100). aac7 run times are bimodal (about half the runs 5-8 % slower, both
backends), so every A/B comparison is paired (`results/NODE_SLOWMODE.md §BM`).

Timings (all m, current engine, after the 2026-10-21 default change Q73).

| scale | measured time | source |
|---|---|---|
| 3 n, 1e11, path 1 | 81.2 to 83 s (D and P equal within noise) | `results/XSHMEM.md` craylane |
| 3 n, 1e11, path 2 | 85 to 93 s (fast mode 85-87, slow 90-94); layout D on Cray 194.6 s | `results/XSHMEM.md` craylane |
| 2 n, 1e11, path 2 (no digit file) | worker 137-158 s, poll-only 145-154 s (3 ABBA pairs, not resolved) | `results/XSHMEM.md §XWLAT` |
| 2 n, 1e10 | path 2 worker 32.0 s, poll-only 40.2 s; path 1 D 39.2 s (4 pairs each) | `results/XSHMEM.md §XWLAT` |
| 1 n, 3.3e10 | D 39.3 s, P 34.9 s with `XSHMEM_PEER` (51.5 s without) | `results/SINGLE_NODE.md §PEER` |
| 1 n, 6.6e10 (the target's share per node) | D 74.2 s, P 83.8 s (4 pairs): D is the fastest here | `results/LAUNCH_DEFAULTS.md §AAC7OPT` |
| 2 n, 5e10 | P (path 1) 39.7 s, D 54.4 s, path 2 53.5 s (3 rounds); P fits 2 n only to 5e10 (node budget) | `results/LAUNCH_DEFAULTS.md §AAC7OPT` |

Older figures (comm_ofi era, 1 n 1e11 129.3 s before the 2026-10-12 defaults, the 1 / 2 / 4 / 6 n scaling at 6.441e10 per node 67.6 / 119.4 / 172.2 / 204.6 s, 6 n 3.8646e11 190.7 to 202.1 s) are in `results/TIME_MODEL.md §M2a`, `§SCALE35` and `archive:results_merged_2026-10-23/NIGHT_RUNS.md`; the 4-6 node figures have not been re-measured on the current engine.

**Not yet done.** ecalc has not run on the target machine. The 576-node layout is refused by the planner with the defaults: device 407.41 GB per node, margin -33.97 GB
(m, planner, 2026-10-25, `plan_values.py --fit` with `ECALC_NP=auto MN_T_CHUNK_MB=2048`; section 3); it fitted with 14.48 GB of margin on 2026-10-19 before `MN_NONPOW2_CAP` auto (Q58) enlarged the planes, and fits again with `MN_NONPOW2_CAP=0`; the standing estimate for 3.82 × 10¹³ digits is 185.5 s without the write (mod, profile `target_l4`,
`results/TARGET.md §TGT`; at the user-supplied 100 GB/s injection peak per APU, a for the achieved rate); the write adds
12.8 s at ecalc's assumed 1.0 GB/s per node and 1.0 s at the file system's 1 TB/s (mod); the target has run only the
memory and exchange benchmarks (`results/TARGET.md §TGTBENCH2`).

## 3. Defaults and launch lines

The code defaults are the tested configuration; nothing has to be set for a one-node run. Every setting, with its default
and evidence, is in the switch table of `ecalc/README.md`; what is in force with its evidence is in `RESULTS.md`.

The multi-node launch line adds the transport settings of the chosen path (`README.md`), `ECALC_NP=auto`,
`MN_T_CHUNK_MB=2048`, the memory guard and the crash-trace handler; `mnrun.sh` sets the libfabric variables (`ecalc/README.md`).
The two lines are kept in:

| system | line | source |
|---|---|---|
| aac7 | `LAUNCH_LINE_KEYS` of `profiles/aac7.conf` (`tools/profile_load.py aac7 --line`) | `tools/gate.sh --line aac7` runs it; one source (D15) |
| target | `target/config/target.conf.template` (site values `__FILL__`); the filled copy stays out of git | `target/kit/target_kit.sh` |

Profiles (`profiles/*.conf`, loaded by `tools/profile_load.py`; precedence: explicit environment > profile > code default)
hold what is machine-specific. Size-dependent values are computed, never typed: the SHMEM pool and library heap (planner
`MN_PLAN_ONLY`), `FI_UNIVERSE_SIZE` (`mnrun.sh`) and the tree schedule `MN_GROUPS`. `tools/plan_values.py [--profile P]
[--line FILE] [--exports] <digits> <nodes>` prints them without launching and prints `export` lines for a launch without
`mnrun.sh` (D9: the target launch block takes pool and heap from `--exports`). At 3.82 × 10¹³ digits on 576 nodes the plan
asks for a pool of 2304 MiB and a heap of 2816 MiB (m, planner).

**Memory fit before every launch (FIT, D26).** The planner (`MN_PLAN_ONLY`, the source of truth) reports for (digits,
nodes, profile) the device peak per node and per APU against `ECALC_DEVICE_GB` (target 373.44 GB per node, m; aac7 444.0
in `profiles/aac7.conf`), the node against `ECALC_NODE_GB`, the margins and the largest consumers;
`tools/plan_values.py --fit`, `mnrun.sh`, `tools/gate.sh` and the kit's plan stage print it and refuse a run that does not
fit (`results/LAUNCH_DEFAULTS.md §FIT`). At 3.82 × 10¹³ digits on 576 nodes (target profile): device 407.41 GB per node,
margin -33.97 GB, refused (m, planner, 2026-10-25, `ECALC_NP=auto MN_T_CHUNK_MB=2048`; node 443.49 GB, margin +36.51 GB; `DM_MN_WRAP` off, with it 433.18 GB); cause: `MN_NONPOW2_CAP` auto (Q58, 2026-10-20) takes 43/32 larger planes at 6.6e10 digits per node (planes +41.3, v-slots +3.9, comm pools +3.2 GB); with `MN_NONPOW2_CAP=0` the figure is the 2026-10-19 one, device 358.96 GB, margin +14.48 GB, node 395.04 GB (the 12.9 GB of earlier notes is the superseded L1 estimate, `results/LAUNCH_DEFAULTS.md §FIT`). Every setting that is not a plain
default has an owner and a review date (`tools/switch_registry.py --check`).

## 4. Plan to the target run

Open work in this repository is in `TASKS.md`.

**Target session** (the user runs it; the steps are in `target/kit/target_kit.sh -h`):
1. Fill `profiles/target.conf` from the template (modules, ROCm, NICs, partition, memory); `tools/profile_load.py target --check`.
2. Kit preflight: environment record, build (path 2: `make XSHMEM=cray`), libfabric memory monitor (uffd), plan check
   (`MN_PLAN_ONLY` must end with `plan check ... OK`), the memory edge, a node screen (`tools/nodescreen.sh`).
3. Measurements: A3 (kit stage `a3`: injection per NIC, one vs two NICs per APU; disabled since 2026-10-25, its `t_comm` test was removed with Q103 -- a `tests/t_xshmem bw` replacement is on TASKS) and A4 (all-to-all scaling with node count).
   These replace the assumed fabric rate in the model; re-run `mn_model.py estimate` and recheck the size against the grid steps.
4. A/B on the target of `MN_T_CHUNK_MB` 1024 vs 2048 (aac7 evidence only so far; X2, the pooled exchange slots, is built in with no switch).
5. Growing runs (a few nodes, then larger groups), each VERIFY OK; then the 576-node run with the write, RECHECK and
   `unpack_digits` off the clock, and a prefix compare against a 10¹¹ reference.

**Risks.**

| risk | handling |
|---|---|
| fabric rate at 576 nodes below the assumption | the kit measures it first; the size can drop to the next grid step |
| a slow or faulty node | node screen and `initchk.py` before the run; exclusion only (`MNRUN_EXCLUDE_HOSTS`), no spares; any 572-576 nodes pass the plan check |
| device memory near the edge | budget check (rc 8) before compute; guard (rc 9) during; -33.97 GB margin with `MN_NONPOW2_CAP` auto, +14.48 GB with `MN_NONPOW2_CAP=0` (m, planner, 2026-10-25; `results/LAUNCH_DEFAULTS.md §FIT`) |
| hangs in the ROCr / libfabric memory registration | `FI_MR_CACHE_MONITOR=userfaultfd` (0/104 hangs against 22/105, m) |
| crash without a trace | `ECALC_SEGV_TRACE=1` on the launch line |
| wrong digits | T1/T2 in the run, RECHECK and `unpack_digits` after it |
| the write short of the file system's 1 TB/s (one file, 32 writers per node; ecalc: 2 files per node, 8 threads) | `tools/wbench` at scale in the kit, `MN_OUT_THREADS=32`; a shared-file writer if needed (OPPORTUNITIES N8): 12.8 s exposed at 1.0 GB/s per node (mod) |
| Cray SHMEM 12.1 (`XSHMEM=cray`, path 2) untested on the target; the init gap (+8 s on aac7) may differ; the target has hugetlb pages, aac7 does not | path 2 runs on aac7 against Cray SHMEM 11.8 (`results/XSHMEM.md`, +7.5 % against path 1, digits identical); the kit's build, plan and t_xshmem checks and the day-one list T1-T8 (`results/XSHMEM.md`, `TASKS.md`) on the target; path 1 stays as the fallback (Q85) |
