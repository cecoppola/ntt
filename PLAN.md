# PLAN — objectives, status and the plan to the target run

Scope: the goal, the current status, defaults and launch lines, and the road to the target run. | Maintained: updated in the
same merge as any change to the status, the goal or the launch lines.

The design (what is computed and how) is in `ALGORITHM.md`; how to check a change is in `TESTING.md`; every environment
switch with its default and evidence is in `ecalc/README.md`; open work is in `TASKS.md`; what is in force and its
evidence is in `RESULTS.md`; agent reports are in `results/`. The earlier phase-by-phase plan and status log is kept
outside the repository as `archive:PLAN_HISTORY_2026-10-12.md` ("PLAN §n" citations in older notes point there).

Labels: **m** measured, **mod** modeled, **a** assumed.

## 1. Goal

- Compute ⌊10^d · e⌋ in decimal, for d = **3.82 × 10¹³** digits, on **576 nodes** of four AMD MI300A APUs each (user
  decision 2026-10-12). The target machine is never accessed from this project; the user runs the bench there.
- The first target campaign (user decision Q120, 2026-10-25) ran 8 × 10⁹ digits per node (4.608 × 10¹² on 576 nodes), where
  layout D, P and path 2 all fit at 1, 8, 64 and 576 nodes (mod, planner). Its 576-node run succeeded on 2026-10-26 (section 2).
  No further target run of ours follows (user 2026-10-31): the code goes to the target one way and an agent there tests it (section 4).
- Constraints per node: device memory ≤ **373.44 GB** (the measured `hipMalloc` edge on the target, 93.36 GB per APU, m),
  host total ≤ **480 GB** (the budget check's default). Each node has 8 NICs (two per APU) on the target and one per APU on aac7.
- What is known about the target (user-supplied 2026-10-12, a; re-assessed in `results/MODEL.md §TGT`):

  | item | target | aac7, for comparison |
  |---|---|---|
  | nodes × APUs | 576 × 4 MI300A = 2304 APUs | 6 nodes per user × 4 MI300A |
  | NICs | 8 Slingshot NICs per node at 400 Gb/s: 2 per APU, 100 GB/s injection peak per APU, 400 GB/s per node | 1 per APU |
  | OS, scheduler | Rocky Linux 9.6, Slurm 25.11.8 | Slurm 25.11.4 |
  | ROCm | 7.14.0 (may be upgraded later); builds ecalc on aac7 (m), speed against 7.2.4 not measured | 7.2.4 on the line |
  | SHMEM, compiler, libfabric | cray-shmem 12.1.0, CCE 21.0.0, libfabric 2.3.1 | OpenSHMEMX 11.8.0, CCE 21.0.0, libfabric 2.3.1 |
  | file system | ≈ 1 TB/s cumulative write into one file at 32 processes per node | — |

  Still unknown: the measured injection rate per APU (bench A3), the per-message cost, the bandwidth at the group boundaries (the dragonfly group is 64 consecutive nodes, `target/README.md` (Target site facts) Target site), the
  write rate of ecalc's own writer with all 576 nodes writing at once (one node measured 3.4-5.0 GB/s with O_DIRECT and 8 threads; two part files per node, not the file system's 32-writer mode;
  OPPORTUNITIES N8). The host memory per node is about 490 GB available (`target/README.md` (Target site facts)).
- The digits must be right without a reference: every run checks itself (`ALGORITHM.md` §10).
- Where the project stands against this goal, with the standing estimate and the memory margin: section 2.

## 2. Current status

**What works.** ecalc computes ⌊10^d·e⌋ with decimal limbs end to end (binary splitting, Newton reciprocal, two-half
division, packed output) and verifies itself on every run (T1 residues, T2 windows; `ALGORITHM.md` §10). It runs at 1-6
nodes on aac7 (the multi-node test cluster; aac6, back in use since 2026-10-27, has no cxi NICs and runs single-node work only), digits identical to GMP reference digits up to 10¹¹ and
identical between the two supported multi-node paths (sha 578f5efb0ff2b9af at 2 n 1e11 on main after Q96, m).

**Two supported multi-node paths (Q85).** Build and run lines are in section 3. Path 1: libfabric
(`COMM_TRANSPORT=xshmem` over `xshmem_ofi.c`, layout D, the default, Q73). Path 2: Cray SHMEM (`XSHMEM=cray`,
`xshmem.h`, one process per GPU, `XSHMEM_LANES=2`; the target shape). Path 2 is +7.5 % against path 1 at 3 nodes,
10¹¹ digits (CI +3.4 to +11.5 %, 11 interleaved pairs), the whole gap is start-up (init +8 s, aac7-specific). The
exchange worker is the default in every layout (Q96: init -6.5 to -7.5 s, host memory halved, bs and dm neutral). The
plain SHMEM transport, `COMM_TRACE`, the proxy mode and the virtual-PE thread mode are removed (Q103, Q78, Q86; list in `RESULTS.md`). `XSHMEM_PEER` is on by default under one process per GPU (Q100). aac7 run times are bimodal (about half the runs 5-8 % slower, both
backends), so every A/B comparison is paired (`results/TRANSPORT.md §BM`).

Run times by scale (3 n 1e11 on both paths, 1 n 3.3e10 to 6.6e10, 2 n 1e10 to 1e11, 2 n 5e10 per layout) are measured and owned by `results/TRANSPORT.md` (§craylane, §XWLAT), `results/COMPUTE.md` §PEER and `results/MODEL.md` §AAC7OPT; older figures are in `results/MODEL.md` §M2a and §SCALE35.

**On the target (2026-10-26, m; `target/README.md` (Target site facts)).** Path 1, layout D, ran 2 to 576 nodes at
8 × 10⁹ digits per node, every run VERIFY OK; the best 576-node run, 4.608 × 10¹² digits, took 255.3 s, of which 111.0 s was
init. Two larger runs failed: 2 × 10¹³ in a communicator handshake hang and 3 × 10¹³ with the P24 CRT flag on nodes 283-376; both
had a mixed piece layout at tree level 7. The workaround, `MN_LEVEL_UNIFORM=1` on every record step (in main since the 2026-10-31 merge, cand3 0c2da14; 2-node gate
`results/TRANSPORT.md` §G2N), is a launch-line setting; the root cause is open (`TASKS.md` Proposed).

**Not yet done.** No run above 4.608 × 10¹² digits has completed on the target. The 576-node layout is refused by the planner with the defaults (margin -33.97 GB; with `MN_NONPOW2_CAP=0` +14.48 GB; m, planner, 2026-10-25, `results/MODEL.md §FIT`); the standing estimate for 3.82 × 10¹³ digits is 185.5 s without the write (mod, profile `target_l4`,
`results/MODEL.md §TGT`; at the user-supplied 100 GB/s injection peak per APU, assumed for the achieved rate); the write adds
2.4 s of file write at ecalc's modeled 4.0 GB/s per node (1.9-2.8 s over the 5.0-3.4 GB/s one node measured; the rate with 576 nodes writing at once is assumed) and 1.0 s at the file system's 1 TB/s (mod).

## 3. Defaults and launch lines

The code defaults are the tested configuration; nothing has to be set for a one-node run (the defaults, with their
evidence, are in the switch table of `ecalc/README.md` and `RESULTS.md`; `ECALC_PLANE_CAP` defaults to `run` since 2026-10-25).

**The two supported paths (Q85; the one owner of their build and launch lines).** Digits are identical between them.

| | path 1: libfabric | path 2: Cray SHMEM (the target shape) |
|---|---|---|
| build | `make -C ecalc GMP_HOME=<gmp>` (default `XSHMEM=ofi`; libfabric and the PMI2 header must be found; `tools/gate.sh` requires `-DXSHMEM_OFI` in the log of a multi-node build) | `make -B -C ecalc XSHMEM=cray GMP_HOME=<gmp>` (aac7: `module load cray-dsmml cray-openshmemx`; `-B` when switching `XSHMEM`) |
| launch | `./mnrun.sh <nodes> env ./ecalc <digits>`: layout D, one process per node, four APU threads, `COMM_TRANSPORT=xshmem` | `./mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>`: one process per GPU, two lanes |
| added by `mnrun.sh` | the communicator and libfabric variables | the same, and under `ECALC_PE_PER_GPU=1` the NUMA placement (`MNRUN_PPG_CPUBIND=ldom`, `XSHMEM_CRAY_NUMA=1`), the automatic libsma heap and the plan |

The exchange worker thread runs in every layout; the per-device allocator helpers (`ECALC_MAPD`) are on above one process (user 2026-11-08); `RNS_BATCH_SPLIT` is automatic under `ECALC_PE_PER_GPU=1`; the transport
is xshmem on both paths (the plain SHMEM transport is gone, Q103). Both lines add `MN_T_CHUNK_MB=2048`, the memory guard and the crash-trace handler (`ECALC_NP` is auto above one process by default). The full lines
(`mnrun.sh` is called with them) are kept in:

| system | line | source |
|---|---|---|
| aac7 | `LAUNCH_LINE_KEYS` of `profiles/aac7.conf` (`tools/profile_load.py aac7 --line`) | `tools/gate.sh --line aac7` runs it; one source (D15) |
| target | `target/config/target.conf.template` (site values `__FILL__`, `MN_NONPOW2_CAP=0` by Q119); filled on the target by `target/probe/gen_conf.py` and committed there (Q112) | `target/RESTART.md` |

Profiles (`profiles/*.conf`, loaded by `tools/profile_load.py`; precedence: explicit environment > profile > code default)
hold what is machine-specific. Size-dependent values are computed, never typed: the SHMEM pool and library heap (planner
`MN_PLAN_ONLY`), `FI_UNIVERSE_SIZE` (`mnrun.sh`) and the tree schedule `MN_GROUPS`. `tools/plan_values.py [--profile P]
[--line FILE] [--exports] <digits> <nodes>` prints them without launching and prints `export` lines for a launch without
`mnrun.sh` (D9: the target launch block takes pool and heap from `--exports`). At 3.82 × 10¹³ digits on 576 nodes the plan
asks for a pool of 2304 MiB and a heap of 2816 MiB (m, planner).

**Memory fit before every launch (FIT, D26).** The planner (`MN_PLAN_ONLY`, the source of truth) reports for (digits,
nodes, profile) the device peak per node and per APU against `ECALC_DEVICE_GB` (target 373.44 GB per node, m; aac7 444.0
in `profiles/aac7.conf`), the node against `ECALC_NODE_GB`, the margins and the largest consumers;
`tools/plan_values.py --fit`, `mnrun.sh`, `tools/gate.sh` and the bench's plan stage print it and refuse a run that does not
fit (`results/MODEL.md §FIT`, which owns the figures for the target and aac7 sizes; the 576-node result today is in section 2). Every setting read by the code has a row in
`ecalc/switches.tsv` (`tools/switch_registry.py --check`).

## 4. Plan to the target run

**Delivery.** No further target session of ours follows (user 2026-10-31); the code goes to the target one way and an agent there
reads and tests it (`target/README.md`, `target/RESTART.md`, `target/bench/QUESTIONS.md`). The removed copy-and-paste route (`remote/`, user
2026-11-08) is in git history (`git show cfe9d3a1:remote/ROUTE.md`). Open work: `TASKS.md`.

**Risks.**

| risk | handling |
|---|---|
| fabric rate at 576 nodes below the assumption | the bench measures it first; the size can drop to the next grid step |
| a slow or faulty node | node screen and `initchk.py` before the run; exclusion only (`MNRUN_EXCLUDE_HOSTS`), no spares; any 572-576 nodes pass the plan check |
| device memory near the edge | budget check (rc 8) before compute; guard (rc 9) during; margins per `MN_NONPOW2_CAP` setting in `results/MODEL.md §FIT` |
| hangs in the ROCr / libfabric memory registration | `FI_MR_CACHE_MONITOR=userfaultfd` (0/104 hangs against 22/105, m) |
| no usable hugetlb pages on the target (8 pages total, 0 free; THP `always`, `max_map_count` 65530, m target 2026-10-26; the aac7 libsma heap also sits on 4 KiB pages, no hugetlb, m `results/TRANSPORT.md`): the path 2 heap (`XSHMEM_CRAY_HEAP_THP=1`) and any pinned 4 KiB heap cannot be put on reserved huge pages, so the init gap and rare multi-second compaction stalls of a node stay possible | path 1 (libfabric) is not affected and stays the fallback; the stall checklist is in `results/TRANSPORT.md`; the heap-THP test is TQ37 and the init timeline `ECALC_INIT_TL=1` (day-one list); the user alone decides any request for reserved pages |
| crash without a trace | `ECALC_SEGV_TRACE=1` on the launch line |
| wrong digits | T1/T2 in the run, RECHECK and `unpack_digits` after it |
| the write short of the file system's 1 TB/s (one file, 32 writers per node; ecalc: 2 files per node, 8 threads) | `tools/wbench` at scale in the bench, `MN_OUT_THREADS=32`; a shared-file writer if needed (OPPORTUNITIES N8): 2.4 s file write at 4.0 GB/s per node, hidden under the division in the model (mod; the old 1.0 GB/s prior gave 12.8 s) |
| Cray SHMEM 12.1 (`XSHMEM=cray`, path 2) untested on the target; the init gap (+8 s on aac7) may differ and is expected to persist (no hugetlb pages, next row) | path 2 runs on aac7 against Cray SHMEM 11.8 (`results/TRANSPORT.md`, +7.5 % against path 1, digits identical); the package's build, plan and t_xshmem checks and the day-one list T1-T8 (`results/TRANSPORT.md` §XDIAG) on the target; path 1 stays as the fallback (Q85) |
