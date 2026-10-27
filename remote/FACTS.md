# remote/FACTS.md — reference sheet for judging ecalc runs on the target

Reader: the browser agent (cannot run anything; reads this static copy and the user's pasted output). Labels on every number:
**m** measured (where), **mod** modelled (planner `tools/plan_values.py`, `ecalc/mn_model.py`), **a** assumed, **u** user-supplied.
Nothing here is a measurement of the target unless it says "target, m". Times are wall seconds of `ecalc`'s own `total` line.
Sections are cited by name: Machine, Paths and launch settings, Memory limits and the planner, Time model, Reading ecalc
output, Known problems and fixes, Settings worth testing, Decisions in force, Where to read more.

## Machine

| item | value | label |
|---|---|---|
| nodes | 576 (Cray EX), Slurm 25.11.8, Rocky Linux 9.6 | u |
| per node | 4 AMD MI300A APUs, each one GPU with 128 GB unified memory; 8 Slingshot-400 cxi NICs = 2 per APU | u |
| fabric peak | 400 Gb/s per NIC, 100 GB/s injection per APU, 400 GB/s per node | u (measured value unknown: first thing to measure) |
| software | ROCm 7.14.0, cray-shmem (OpenSHMEMX) 12.1.0, libfabric 2.3.1, CCE 21.0.0 | u |
| device edge | `hipMalloc` edge 373.44 GB per node = 93.36 GB per APU (the planner's `ECALC_DEVICE_GB`) | target, m (5 runs) |
| host budget | `ECALC_NODE_GB` 414-480 (code default 480; set from the probe) | a |
| file system | about 1 TB/s cumulative into ONE file with 32 writers per node; ecalc's own writer rate unmeasured | u |
| physical size | 4 / 32 / 256 / 2304 APUs = 1 node / 1 chassis / 1 rack / all (ladder levels L0-L3 = 1 / 8 / 64 / 576 nodes) | u |
| dev cluster (aac7) | same node type, ROCm 7.2.4, Cray SHMEM 11.8, **1 NIC per APU**, 6 nodes per user; all aac7 numbers below are from it | m |

Differences that make aac7 numbers only a guide: 2 NICs per APU (aac7: 1), ROCm 7.14 (aac7: 7.2.4; ROCm 7.0.3 was +22 % slower
than 7.2.4 at 1e11, m aac7), Cray SHMEM 12.1 (aac7: 11.8; path 2 never ran on 12.1), 576 nodes (aac7 tested to 10).
What ecalc is: floor(10^d * e) by binary splitting of sum 1/k!, Newton reciprocal, two-half division, NTT products mod
3-4 primes over 4 APUs and node groups; every run self-verifies (T1 residues, T2 windows, `VERIFY OK`).

## Paths and launch settings

Two multi-node paths; digits are identical between them (user rule Q85, both must keep working).

| | path 1: libfabric (default build) | path 2: Cray SHMEM (the target's shape) |
|---|---|---|
| build (GMP not needed for ecalc, tools and the s2 tests; `GMP_HOME` only for the unit tests and ref/gen_e) | `make -C ecalc` (XSHMEM=ofi; needs libfabric + PMI2 header; log must show `-DXSHMEM_OFI`) | `make -B -C ecalc XSHMEM=cray` with the cray-dsmml and cray-openshmemx modules (`-B` when switching) |
| launch | `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ./ecalc <digits>` : layout D = one process per node, 4 APU threads | `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>` : one process per GPU, 2 lanes (lane k = NIC k) |
| third shape "P" | path 1 with `ECALC_PE_PER_GPU=1` (one process per GPU): fastest on aac7 at 2 nodes where it fits, slower at 1 node | - |
| `<nodes>` | the node count; `mnrun.sh` uses the first N nodes of the allocation (`MNRUN_NODES`, `MNRUN_NODELIST`) | same |

Launch settings of the target (`target/config/target.conf.template`; the probe fills the `__FILL__` keys):

| setting | value | note |
|---|---|---|
| `ECALC_NP` | `auto` | 3 primes at 1 node, 4 (P24) for multi-node products; `=4` at 1 node +7.5 s (m aac7) |
| `MN_T_CHUNK_MB` | 2048 | -42.3 s at 10 n vs 1024 (m aac7); 1024 vs 2048 under P: noise (m aac7) |
| `MN_NONPOW2_CAP` | 0 | Q119: 3.82e13 on 576 fits only with it off; auto default is off below 4.5e10 digits per node anyway |
| `FI_MR_CACHE_MONITOR` | `userfaultfd` | memhooks hangs 22/105, uffd 0/104 (m aac7); `disabled` gives silently WRONG exchanges, refused by `mnrun.sh` |
| `FI_LOG_LEVEL` | `warn` | `FI_UNIVERSE_SIZE` = max(4096, 4 x tasks), computed by `mnrun.sh` |
| `ECALC_DEVICE_GB` / `ECALC_NODE_GB` | 373.44 / probe value (414-480) | plan refuses a run that exceeds them |
| `ECALC_MEM_GUARD_GB` | 6 | stops with rc 9 before the OOM killer |
| `ECALC_SEGV_TRACE`, `ECALC_VERBOSE`, `MEM_REPORT_DEVS` | 1, 2, 1 | trace on crash; phase lines; per-APU memory rows. `ECALC_VERBOSE=2` costs at most a few s (+2.0 s over 4 pairs, not resolved, m aac7) |
| `ECALC_PLANE_CAP` | unset = `run` | 2^30 plane pools up to about 9.6e9 digits per node, 2^31 above; not a launch setting (Q122) |
| `MNRUN_CPUS_PER_TASK` | `auto` | else srun confines a task to 2 CPUs (m aac7) |
| `MNRUN_MODULES`, `MNRUN_UNLOAD` | site values | path 2 needs `cray-dsmml cray-openshmemx <rocm>`; unload a conflicting login `rocm` |
| computed, never typed | pool `COMM_SHMEM_POOL_MB`, heap `SHMEM_SYMMETRIC_SIZE`=`XT_SYMMETRIC_HEAP_SIZE`, `MN_GROUPS`, `POOL_LOG` | by the planner / `mnrun.sh`; `tools/plan_values.py --exports <digits> <nodes>` prints them |
| bad nodes | `MNRUN_EXCLUDE_HOSTS=h1,h2`, `MNRUN_EXCLUDE_MODE=drop` | no spares (decision 2026-10-11); `drop` shifts ranks, so ask for N + excluded nodes |

Useful `mnrun.sh` options: `MNRUN_PRINT_ENV=1` (print the computed variables, launch nothing, no allocation needed), `MNRUN_FIT=0`
(skip the fit check), `MNRUN_NODES=n`, `MNRUN_PPG_GPUBIND=none|task`, `MNRUN_PPG_CPUBIND=none|ldom`. One network program per node:
never two comm processes of different jobs on one node (CXI command-queue errors). No digit-file write in multi-node timing runs
(slow file system looks like a hang). Under P/path 2, `mnrun.sh` adds GPU/CPU binding (`--gpus-per-node=4 --gpu-bind=none`,
NUMA placement) and `srun --kill-on-bad-exit=1`.

## Memory limits and the planner

The planner decides before any launch: `python3 -I tools/plan_values.py --fit --ecalc <REMOTE_ECALC1 (path 1) or REMOTE_ECALC2 (path 2) from site.env> <digits> <nodes>` (path 2: add `--env "ECALC_PE_PER_GPU=1 XSHMEM_LANES=2"`; exit 0 fits, 1 refused) or
`MN_PLAN_ONLY=<digits>:<nodes> ./ecalc`; `mnrun.sh` runs it first and refuses with exit 4. It needs the xshmem build of the run
(a login node is enough). Device peak per node = max(division phase: planes + arena + comm pools + v-slots; leaf phase: same + 6 GB
VMM growth) against 373.44 GB, with a 5 GB margin rule for the automatic `DM_MN_WRAP`.

| size (digits per node) | nodes | shape | device GB per node (limit 373.44) | verdict (mod, planner) |
|---|---|---|---|---|
| 8e9 | 576 (4.608e12) | path 2 | 328.69 (+44.75), node 406.32 | fits; worst of the first campaign |
| 8e9 | 1 / 8-64 | path 2 | 144 / 216, node 191 / 293 | fits |
| 8e9 | 1 / 8-576 | layout D | 155 / 178-181 | fits |
| 1.74e10 (1e13 on 576) | 576 | path 1 D | 238.97 (+134.47) | fits |
| 1.74e10 | 576 | path 2 | 380.23 with wrap, 371.64 without (node 467 vs 414) | REFUSED by 6.8 GB; `DIST_CHUNKS=2` -> 318.56, `=4` -> 288.22, `=8` -> 273.06 (mod) |
| 6.632e10 (3.82e13 on 576) | 576 | path 1 D, `MN_NONPOW2_CAP=0` | 358.96 (+14.48), node 395.04 | fits |
| 6.632e10 | 576 | D, cap auto (on) | 407.41 | REFUSED (-33.97) |
| 6.632e10 | 8-576 | P / path 2 | 430 (8 n) to 671 (576 n) | refused from 8 nodes on |

- Largest sizes on 576 nodes (mod): about 4.19e13 digits with `MN_NONPOW2_CAP=0`, about 2.67e13 with it on; path 2 / P about 5.2e12-7.5e12 (1.3e10 per node with a 480 GB node budget, about 9.1e9 with 414).
- Why path 2 jumps at 576 nodes: 576 nodes = 2304 PEs is not a power of two, so every product uses the general exchange map whose v-slots (about 61 GB per node at 1.74e10) are zero at power-of-two node counts. `DIST_CHUNKS=K` slices the exchange rows (code default: 1 under `ECALC_PE_PER_GPU=1`, i.e. path 2; 8 otherwise): v-slots 30.6 / 15.3 / 7.7 GB for K = 2 / 4 / 8 (mod). Digit safety of DIST_CHUNKS: identical for 4 vs 8 on layout D (m aac7); path 2 at non-power-of-two counts untested (TQ84).
- `ECALC_PLANE_CAP=run` prints `ECALC_PLANE_CAP=run (default): plane cap 2^30 ...` (or 2^31). Cap 2^30 saves 2.64 s [-2.98, -2.30] at 8e9 on 1 node, about 2.8 s at 2 n, digits identical (hash 3e78c1b2a0276963 at 8e9, m aac7); above 2 nodes unmeasured.
- Measured on aac7 (m): P fits 2 n to 5e10 (5.5e10 refused, 425.8 GB host vs 420); D fits 2 n to about 1.2e11; one node edge 1.44 / 1.30e11 at cap 2^30 / 2^31 (three primes). Init floor is 0.06-0.075 s per GB mapped (kernel page clearing, one lock): 0.070 s/GB on aac7 (target reportedly 0.010 s/GB with a generic harness, u, unconfirmed).
- rc 8 / `plan REFUSED`: lower the digits (refusal prints the largest that fits), or `MN_T_CHUNK_MB=1024`, `ECALC_NP=3`, `MN_NONPOW2_CAP=0`, or `DIST_CHUNKS`. rc 9: lower `ECALC_NODE_GB` by the overshoot it prints.

## Time model

`python3 ecalc/mn_model.py estimate --g 1 8 64 576 --D 8e9 --fabric target` (run on any machine with python3). Assumptions (a): 100 GB/s per APU,
2 us per message, dragonfly group 64, file write 1 GB/s per node. Compute is calibrated on aac7/aac6 at larger sizes: **at small
sizes start-up (init, launch, mapping) dominates and the model is too low; trust growth between levels, not the absolute value.**
Same model for both paths; path 2 adds about +8 s init (libsma heap registration 6.2 s, host register 1.5, barrier 1.4; m aac7) and +7.5 % at 3 n.

| digits per node | nodes | digits total | wall no write | wall with write | exposed fabric | device (D) |
|---|---|---|---|---|---|---|
| 1e9 | 1 | 1e9 | 9.4 | 9.8 | 0 | 120 GB |
| 1e10 | 1 | 1e10 | 19.1 | 22.8 | 0 | 166 |
| 1e10 | 2 | 2e10 | 23.7 | 25.9 | 1.2 | 189 |
| 8e9 | 1 | 8e9 | 16.8 | 19.9 | 0 | 155 |
| 8e9 | 2 | 1.6e10 | 20.5 | 22.5 | 0.7 | 178 |
| 8e9 | 8 | 6.4e10 | 22.6 | 24.6 | 1.5 | 178 |
| 8e9 | 64 | 5.12e11 | 25.8 | 27.6 | 2.5 | 178 |
| 8e9 | 576 | 4.608e12 | 32.0 | 33.2 | 6.5 | 181 |
| 1e10 | 8 / 64 / 576 | 8e10 / 6.4e11 / 5.76e12 | 26.2 / 29.7 / 36.1 | 28.2 / 31.6 / 37.8 | 2.4 / 3.8 / 8.3 | 189 / 189 / 192 |
| 1.736e10 | 576 | 1e13 | 52.3 | 55.3 | 12.5 | 229 |
| 6.632e10 | 1 / 8 / 64 | 6.6e10 / 5.3e11 / 4.2e12 | 102.7 / 118.2 / 149.6 | 116.9 / 129.6 / 160.4 | 0 / 17.8 / 28.4 | 290 / 359 / 359 |
| 6.632e10 | 576 | 3.82e13 | **187.1** | **191.2** | 51.6 | 390 (planner 358.96 with cap 0) |

All wall figures: mod, seconds. Per-level growth at 8e9 (mod): 1->8 n +35 %, 8->64 +14 %, 64->576 +24 %; 576 n: 4.6e12 -> 1e13 +63 %,
1e13 -> 3.82e13 3.6x. Older headline estimate 182-185.5 s (mod, 100 GB/s assumed); the write adds 1.0 s at 1 TB/s or 12.8 s at 1 GB/s per node.

aac7 measured references (m; `total`, VERIFY OK, no digit file):

| run | result |
|---|---|
| 1 node 6.6e10, D / P | 74.2 s / 83.8 s (P +9.62 [+4.98, +14.26]); 1-node runs of the 6.4e10 class 74-76 s, init 13.5 s |
| 2 nodes 5e10 | P (path 1) 39.7 s, D 54.4 s, path 2 53.5 s (path 2 vs P +13.85 [+10.76, +16.94]) |
| 3 nodes 1e11 | path 1 81-83 s, path 2 85-93 s (+7.5 %; the gap is start-up) |
| weak scaling 6.441e10 per node | 1 n 75, 2 n 139, 4 n 182-188 (fast) / 208-211 (slow mode), 6 n 246-293 s |
| 1 node, cold vs warm | first run on a fresh allocation 154.9 s (init 47.4), later runs 76.9-85.5 s (init 23.6-24.2) |
| 1 node 8e9 | cap 2^30 saves 2.64 s; 2 n 8e9 per node about 23.9 s with cap 2^30 against 26-27.4 s with 2^31 (path 1) |

How to read measured / model (rule of thumb, a): at L0-L1 (8e9 per node) expect 1.3-2.5x the model (start-up share); a ratio that is
roughly constant from level to level is fine; the ratio should fall as size grows (compute-bound above 3e10 per node, expect 0.9-1.2).
Ratio above 1.5x with no phase explaining it, or growth per level well above the figures above = fabric, launch or placement problem
(first suspect: a slow node, then fabric rate below 100 GB/s per APU). Run-to-run spread above 5 % at L0-L1 (8 % at L2) or one slow
node = exclude it. Do not time run 1 of a fresh allocation (cold start): time from run 2.

## Reading ecalc output

A multi-node run prints per-node lines prefixed by the rank (`N: ...`) and rank 0 prints the totals. The lines to read:

| line | meaning and judgement |
|---|---|
| `total  T s   (bs a + 10dP b + dm c + T1 d + dc e + T2 f = P; init i; other o); VmHWM x GB` | rank 0 only; **the wall figure**. bs = binary-splitting leaf phase, 10dP = power of ten, dm = reciprocal + division (fabric-heavy), T1 = residue check, dc = decimal conversion, formatting and digit check (also the write if any), init = mapping and seeds. `other` should be a few seconds; large `other` = unexplained wait. |
| `mn: all N nodes: VERIFY OK` (or `N PEs`) | rank 0, all ranks agree: the result passed T1 and T2 everywhere. Anything else (`VERIFY FAILED`, missing line) = wrong or dead run: the run does not count |
| `N: VERIFY OK` | per node/PE; expect one per node (per PE under P / path 2) |
| `N: T2 0.00 s windows ok (K checked ...), digits == X mod q ok` | the second check; `FAILED` = bug |
| `N: digits: 27182818...<last digits> (this node's range)` | first and last digits of that node's share; node 0's line starts `2718281828459045235360287471352662497757247093699959574966967...` (first digits of e, a correct head); the last node's last digits are the tail of the whole result |
| `N: dc  t s  digits [k0, k1) of d formatted ...` | per-node phase; compare across nodes: a node far above the median = slow node |
| `wrote <file>.partNNNN (G GB; write w s ..., s after the checks)` | only with a digit file; `s after the checks` is the exposed write time |
| `ECALC_PLANE_CAP=run (default): plane cap 2^30` | cap picked (see Memory limits); `fit` or a forced value reads differently |
| `plan check ... OK` / `plan REFUSED ...: the memory does not fit (device X GB vs ECALC_DEVICE_GB Y ...)` | planner verdict; FITS in the sense of exit 0 |
| `plan fit` / `memory fit:` lines | device GB per node and per APU against limit, margin, largest consumers, `dm_mn_wrap:<v>(fit/set/default)` |
| `config: ...` line | the effective settings (ECALC_NP, MN_GROUPS, caps): check it matches what the script said it launched |

Digit checks without a digit file: compare the `digits:` lines with a reference run (same digits, same node count) or the head with
the e prefix above. The ladder tool (`target/ladder/parse_run.py`) hashes the sorted `digits:` lines (first 16 hex of sha256 of the
joined lines; per-node ranges differ between layouts, so compare hashes only between runs of the same layout and node count; across layouts compare the
`digits_tail` key = the last digits, which is layout independent) and reports `e_prefix_check PASS|FAIL`. With a digit file: `tools/unpack_digits -q <parts...> | sha1sum | cut -c1-16`.

Reference digit hashes (first 16 hex of sha1 of the unpacked ASCII digits, m aac7, `ecalc_ref_hash` in `target/bench/items/_ecalc.sh`):
1e9 `bf9f4b08758b40d9`; 1e10 `3fc025473586ed22`; 1e11 `578f5efb0ff2b9af`; 8e9 `3e78c1b2a0276963`. Larger sizes have no stored reference on
the development side: judge by VERIFY OK on all ranks and equal digits between paths and replicas. Measured reference: 1.6e10 digits
(8e9 per node at 2 nodes) end in `...862935846376248170970671395813714711` (m, aac7 2026-10-25, path 1, identical with and without
the 2^30 plane cap; the last node's `mn: node N: digits:` line).

Return codes (ecalc exit codes from `ecalc/fatal.h`, `memsample.h`; `mnrun.sh` header):

| rc | meaning |
|---|---|
| 0 | run finished, VERIFY OK (a failed verification returns nonzero) |
| 1 | `VERIFY FAILED` (or `mnrun.sh` refusing `FI_MR_CACHE_MONITOR=disabled`) |
| 2 | usage / bad setting (e.g. `ECALC_PLANE_CAP` value) / planner could not run |
| 3 | internal check failed (HIP call, transport, a `plan` refusal inside the run); log has the first fatal line |
| 4 | **`mnrun.sh` pre-launch refusal**: the plan does not fit (nothing launched); `ladder.sh` also uses 4 |
| 6 | allocation failed or a pool would grow inside a phase (e.g. `POOL_LOG=30` fails at 6.6e10 on aac7) |
| 8 | `plan REFUSED` / `ECALC_BUDGET_CHECK` stop: modelled peak over budget (the RESTART notes call this "rc 8 at start") |
| 9 | memory guard stopped the run before the OOM killer (`ECALC_MEM_GUARD_GB`) |
| 124 | `timeout` of the wrapper/allocation: too slow or hung; look at the last phase line |
| 134 | `abort()`; with `ECALC_SEGV_TRACE=1` the trace is in the log |
| 137 / 143 | killed (OOM killer / scancel or time limit) |

## Known problems and fixes

| symptom | cause and fix (evidence) |
|---|---|
| hang in memory registration (ROCr <-> libfabric) | keep `FI_MR_CACHE_MONITOR=userfaultfd`; if the kernel refuses uffd (probe `uffd.*`), stop and tell the user before any multi-node run. `disabled` = silent wrong digits (S46, m aac7) |
| rc 4 / rc 8 at start | plan does not fit: lower digits to the printed maximum, or the memory settings above |
| rc 9 | memory guard: lower `ECALC_NODE_GB` by the printed overshoot |
| `fi_enable(-28)` with several PEs per node | launch Slurm-native (`srun --ntasks`), as `mnrun.sh` does (m target, TGTBENCH2) |
| a task gets 2 CPUs | `MNRUN_CPUS_PER_TASK=auto` (m aac7) |
| link/load errors after `module load rocm` | versioned ROCm conflicts with the login default: `MNRUN_UNLOAD` |
| APUs invisible in a step | `--gpus-per-node=4` in the allocation, `--gpu-bind=none` on the step (`mnrun.sh` adds it) |
| path 2 start-up slow or failing on SHMEM 12.1 | path 1 is complete alone; run with one path and report the first error. Known aac7 start-up costs of path 2 are heap NIC registration; `XSHMEM_CRAY_HEAP_THP=1` (huge pages) is untested (TQ37) |
| slow node (replica 5-10 % behind, one `dc`/`init` far above median) or missing NIC | `tools/nodescreen.sh` (screens upload stall, init time, `fi_info -p cxi` NIC count 8 per node); exclude the host; no spares |
| slow launch mode (aac7: about 30-50 % of 4-node launches have -21 % all-to-all rate, dm +25-30 %) | per-launch NIC state, memoryless; unknown on the target (TQ50; 2 NICs per APU may be immune). Compare arms at the same time on different nodes; never infer from one run |
| first run on a fresh allocation slow (2x on aac7) | cold start: time from run 2 |
| NICs lost after jobs end (aac7) | check NIC count at the start of each allocation; use one long allocation and run many items inside it |
| one start-up hang in 31 at 7.7e10 (E1), 0 in 301 since | kill only that job, rerun once, record it |
| about 1 % of 2-node runs segfaulted in `hipMemMap` of the background mapper (4/422 on aac7; not seen since uffd) | rerun; report with `ECALC_SEGV_TRACE` log if it recurs |
| random multi-second stalls of all 4 APU threads of a node | kernel compaction of a 4 KiB pinned heap (path 1 with libfabric is not affected, SHMEM heap was): hugetlb/THP heap; checklist in `results/SHMEM.md` |
| `ECALC_VERBOSE=2` output very long at 576 nodes | use `ECALC_VERBOSE=1` for L2/L3-size runs (the ladder does) |
| a run seems hung but is a slow file system | do not write digits in timing runs; wait for `total` before judging |

## Settings worth testing

The session is hours, not days: each test should be one ABBA block or one pair at the same time on different node groups. Source:
`target/bench/QUESTIONS.md` (rows TQ##, band 100-199 = optimization). Expected effects are from aac7 (a for the target). Ordered by value:

| # | question (TQ) | test | expected effect | size of test |
|---|---|---|---|---|
| 1 | does path 2 run on SHMEM 12.1 at all, and which path is faster at 8-64 nodes (TQ11, TQ40, TQ20) | same digits, both paths, 2 then 8 nodes | aac7: path 2 equals D but +13.9 s vs P at 2 n, +7.5 % at 3 n; decides the path of every later run | 2 x 1-2 runs at 2 and 8 n |
| 2 | do both NICs of an APU carry traffic: `XSHMEM_LANES=1` vs 2 (TQ13, TQ38) | path 2, 2 nodes, ABBA x2 | aac7 +0.45 s (1 NIC); target gain unknown, up to 2x injection if only 1 lane works; M7c: +37 s at 576 n if one NIC idles | 4 runs of 1e10 at 2 n |
| 3 | fabric rate and latency per APU (TQ13-TQ16): all-to-all 2/4/8 nodes | `t_xshmem` tests in `tests/` (stream, a2a, lat) | model assumes 100 GB/s per APU, 2 us; aac7 measured 10-14 GB/s per PE, 8.5 us. If the rate is far below 100 GB/s the whole table above grows (feeds `--bw`, `--lat`) | minutes |
| 4 | D vs P vs path 2 at 1 node (TQ18-TQ19) | 3 arms at 8e9 and 1e10 | aac7: D 74.2 < P 83.8 s at 6.6e10; P wins below 3.3e10 (-5.5 s) | 3-6 runs |
| 5 | `ECALC_PLANE_CAP=2^30` vs `2^31` at 1 and 8 nodes (TQ82) | ABBAAB at 8e9 per node | -2.6 s per run at 1 n, -2.8 s at 2 n (m aac7); small relative to 576-n walls | 6 runs |
| 6 | `MN_NONPOW2_CAP` 0 vs 1 at a non-power-of-two count, e.g. 2 x 12 nodes (TQ81) | same digits, run both at once | time cost of cap 0 unknown; decides whether 3.82e13 stays (cap 0) or falls back to about 2.67e13 | 2 x 12 n, 25 min |
| 7 | `DIST_CHUNKS=2` or 4 on path 2 at 3 nodes (digits gate) then 576 n (TQ84) | digit hash equal to path 1, VERIFY OK, 12 PEs | lets path 2 run 1e13 on 576 n (fits 318.56 GB) | 3 runs of 1e10 |
| 8 | `MN_T_CHUNK_MB` 1024 vs 2048 at 8 nodes (TQ44) | ABBA x2 | aac7 noise; 2048 is the default | 4 runs |
| 9 | `MN_GROUPS` top-level schedule at 64 nodes (TQ53) | 3 schedules | modelled -4.7 s at 576 for a 9-way top; the default is within 0.2 s of the best at 576. Any node exclusion at other N needs an explicit `MN_GROUPS` (default schedule is 8-2000 s worse at N != 576, mod) | 40 min |
| 10 | write rate: `tools/wbench` 8 vs 32 threads, per-node vs one shared file; `MN_OUT_THREADS` (TQ54-TQ56) | 1 then 64 nodes | the part file is 0.417 B per digit (15.9 TB at 3.82e13); 1 GB/s per node assumed; matters only for runs that write | 10-15 min |
| 11 | ROCm 7.14 vs an older module (TQ30) | rebuild + 1-node ABBA | aac7: 7.0.3 +22 % vs 7.2.4 | 25 min |
| 12 | `XSHMEM_ROUND_MB` 128 vs 512, `DM_MN_WRAP` 0/1, `ECALC_NP` 3 vs 4, `BS_SEED_CHUNK_MB`, `ECALC_BG_THREADS` 24/48/96 | pairs | aac7: all within noise except NP=4 (+7.5 s); try only if the phase table points at bs or dm | 4-8 runs each |
| 13 | init cost: VMM map rate, path 2 heap THP (`XSHMEM_CRAY_HEAP_THP=1`, `ECALC_INIT_TL=1`) (TQ35-TQ37) | `ECALC_INIT_TL=1` timeline | init is 24 s of 77 s at 1 node on aac7; at 8e9 per node init is a large share of the wall | 2-4 runs |

A/B rules: run both arms at the same time on disjoint nodes of one allocation, or alternate A B B A; always a A/A control first (aac7 spread
was bimodal, one run in three 5-8 % slower); digits must be identical in every arm; one change per test.

## Decisions in force

| id | decision (user) |
|---|---|
| Q85 | both paths stay working; digits identical; SHMEM changes must not regress libfabric |
| Q112 | raw target data is committed as measured; Q116 never post passwords, keys or tokens |
| Q114 | ladder sizes 1, 8, 64, 576 nodes (4 / 32 / 256 / 2304 APUs), each group on physically adjacent nodes |
| Q119 | `MN_NONPOW2_CAP=0` on the target line (3.82e13 on 576 fits only with it off) |
| Q120 | first make every code path work on 576 nodes at 8e9 digits per node (4.608e12); raise digits only after those runs succeed |
| Q121 | 576-node staircase, each step after the previous passed: 4.608e12 (both paths) -> 1e13 (path 1; path 2 refused) -> 3.82e13 (path 1) |
| Q122 | plane pools: no launch setting; code default `ECALC_PLANE_CAP=run` |
| 2026-10-11 | no spare nodes: bad nodes are excluded, not replaced |
| standing | correctness first: a result counts only with VERIFY OK on every rank; digits identical between paths; new behaviour behind a switch, off by default; every number labelled m / mod / a; never use `FI_MR_CACHE_MONITOR=disabled` |
| session goal (user, 2026-10-26) | the most digits of e in the shortest time; the route may deviate from the old plan if it serves that goal; runs should be optimized ecalc runs, not long experiments |

Gate to move up a level (from the ladder rules, a): every run `VERIFY OK`, digit hash equal between replicas and paths, spread below 5 %
(L0, L1) / 8 % (L2), mean `total` within 1.5x the model of the same level, device peak at least 10 GB under 373.44, no rc 8/9, no
slow host left. Staircase s1 (4.608e12) -> s2 (1e13) -> s3 (3.82e13) follows it step by step; a failing step stops the climb.

## Where to read more

All paths are in this repository; read them on the static copy.

| need | file |
|---|---|
| doc map, build/run quick start | `README.md`; algorithm `ALGORITHM.md`; gates and ABBA method `TESTING.md` |
| every switch with default and evidence | `ecalc/README.md` (table "Switches"; machine-readable `ecalc/switches.tsv`) |
| launch lines, plan | `PLAN.md` section 3; defaults, fit, path-2 jump, aac7 optimum `results/LAUNCH_DEFAULTS.md` (sections FIT, P2JUMP, AAC7OPT) |
| time model, fits | `results/TIME_MODEL.md`; `ecalc/mn_model.py` (`estimate --help`) |
| memory | `results/MEMORY.md`; planner `tools/plan_values.py --help` |
| paths, craylane, SHMEM facts | `results/XSHMEM.md`, `results/SHMEM.md` (stall checklist), `results/OFI_FABRIC.md` |
| crashes, hangs | `results/CRASHES_HANGS.md`; slow nodes and screens `results/NODE_SLOWMODE.md` |
| target runbook, ladder, bench | `target/RESTART.md` (sections 3, 8, 9), `target/ladder/ADJUST.md` (model walls, rules 1-12), `target/ladder/levels/L0.conf`-`L3.conf`, `target/bench/QUESTIONS.md` (TQ01-TQ84), `target/config/target.conf.template` |
| launcher | header of `ecalc/mnrun.sh` (MNRUN_* options) |

Extract one section of a file on the target (no editor needed):
`sed -n '/^## Memory limits/,/^## Time model/p' remote/FACTS.md | head -60`, or one markdown section of any file:
`awk '/^## FIT/{f=1;print;next} /^## /{f=0} f' results/LAUNCH_DEFAULTS.md | cut -c1-400 | head -60`;
a bounded look at a run: `remote/run/show.sh <run dir|stage|latest> [errors|tail N|grep PATTERN|phases|env|files]`.

Open facts only the target can supply (not in the repository): module names and versions as installed, partition and QOS limits,
`MemAvailable` and the usable node budget, whether userfaultfd works, the real injection rate with 1 and 2 lanes, all-to-all scaling and
latency, Cray SHMEM 12.1 behaviour and start-up time for path 2, ROCm 7.14 speed against 7.2.4, the file-system rate of ecalc's own writer,
node-to-node variation and slow-launch behaviour, the dragonfly group size, and whether the 2^30 plane cap still saves start-up above 2 nodes.
