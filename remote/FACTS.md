# remote/FACTS.md — reference sheet for judging ecalc runs on the target

Reader: the browser agent (cannot run anything; reads this static copy and the user's pasted output). Labels on every number:
**m** measured (where), **mod** modelled (planner `tools/plan_values.py`, `ecalc/mn_model.py`), **a** assumed, **u** user-supplied.
Nothing here is a measurement of the target unless it says "target, m". Times are wall seconds of `ecalc`'s own `total` line.
Sections are cited by name: Machine, Target site measured 2026-10-26, Measured runs on the target, Paths and launch settings, Memory limits and the planner, Time model, Reading ecalc
output, Known problems and fixes, Settings worth testing, Decisions in force, Where to read more.

## Machine

| item | value | label |
|---|---|---|
| nodes | 576 (Cray EX), Slurm 25.11.8, Rocky Linux 9.6 | u |
| per node | 4 AMD MI300A APUs, each one GPU with 128 GB unified memory; 8 Slingshot-400 cxi NICs = 2 per APU | u |
| fabric peak | 400 Gb/s per NIC, 100 GB/s injection per APU, 400 GB/s per node | u (measured value unknown: first thing to measure) |
| software | ROCm 7.14.0, cray-shmem (OpenSHMEMX) 12.1.0, libfabric 2.3.1, CCE 21.0.0 | u |
| device edge | `hipMalloc` edge 373.44 GB per node = 93.36 GB per APU (the planner's default `ECALC_DEVICE_GB`; harness figure 10-06, superseded by P1's measurement `REMOTE_DEVICE_GB`, ROUTE) | target, m (5 runs) |
| host budget | `ECALC_NODE_GB=487` from MemAvailable 515.7 GB on an idle node (code default 480; the old 414 was an assumption) | target, m 2026-10-26 |
| file system | about 1 TB/s cumulative into ONE file with 32 writers per node; ecalc's own writer rate unmeasured | u |
| physical size | 4 / 32 / 256 / 2304 APUs = 1 node / 1 chassis / 1 rack / all (ladder levels L0-L3 = 1 / 8 / 64 / 576 nodes) | u |
| dev cluster (aac7) | same node type, ROCm 7.2.4, Cray SHMEM 11.8, **1 NIC per APU**, 6 nodes per user; all aac7 numbers below are from it | m |

Differences that make aac7 numbers only a guide: 2 NICs per APU (aac7: 1), ROCm 7.14 (aac7: 7.2.4; ROCm 7.0.3 was +22 % slower
than 7.2.4 at 1e11, m aac7), Cray SHMEM 12.1 (aac7: 11.8; path 2 never ran on 12.1), 576 nodes (aac7 tested to 10).
What ecalc is: floor(10^d * e) by binary splitting of sum 1/k!, Newton reciprocal, two-half division, NTT products mod
3-4 primes over 4 APUs and node groups; every run self-verifies (T1 residues, T2 windows, `VERIFY OK`).

## Target site, measured 2026-10-26

Hand-run by the user (no agent on the system); every row target, m, 2026-10-26. Nodes sampled in depth matched on every field;
ecalc ran on 1, 2, 8, 64 and 576 nodes and every node started it at least once (no slow or faulty node seen).

| item | value |
|---|---|
| scheduler | Slurm 25.11: one default partition holding all 576 nodes; whole nodes only; no time or size limit; no GPU GRES on the compute nodes (`Gres=(null)`); `MpiDefault=cray_shasta`; allocations of 576 nodes start within seconds when the partition is idle |
| login environment | sets `SLURM_HINT=nomultithread` and `SLURM_DISTRIBUTION=block:block` (see Known problems) |
| topology | `topology/tree`: nine 64-node leaf switches = the nine dragonfly groups, consecutive node numbers (1-64, 65-128, ...); no xnames, so cabinet/chassis/blade are unknown |
| node | 4 APUs, 96 Zen 4 cores = 192 threads, 4 NUMA nodes (one per APU): numa0 CPUs 0-23,96-119; numa1 24-47,120-143; numa2 48-71,144-167; numa3 72-95,168-191; distance 10 local / 32 remote |
| memory | MemTotal 501.3 GiB; MemAvailable 515.7 GB idle (393.5 GB during the division step of a 64-node run); Slurm RealMemory 509,000 MB; each APU reports 128 GiB of VRAM from the shared HBM |
| kernel settings | THP `always`; hugepages about 0 (8 total, 0 free); `numa_balancing=0`; memlock unlimited; open files 16,384; `max_map_count` 65530; Rocky 9.6, kernel 5.14 el9_6 |
| NIC to NUMA | cxi0,1 -> numa0; cxi2,3 -> numa1; cxi4,5 -> numa2; cxi6,7 -> numa3; 400 Gb/s links up, MTU 2112, PCIe Gen5 x16. ecalc's `pe_geo` map picks exactly these pairs for APU 0-3 with `COMM_OFI_NICS` unset (1, 2, 64, 576 nodes) |
| modules | `rocm/7.14.0` (default; 7.1.1 and 7.2.4 also), `cray-shmem/12.1.0`, `cray-dsmml/0.5.0`, `cray-pmi/6.1.17`, `libfabric/2.3.1`, CCE 21.0.0. The PE default `cray-openshmemx/11.8.0` conflicts with `cray-shmem/12.1.0`. `gmp/6.3.0` sets `GMP_BASE`, not `GMP_HOME` |
| userfaultfd | `vm.unprivileged_userfaultfd=0` yet `FI_MR_CACHE_MONITOR=userfaultfd` works (user-mode only): the launcher default stays |
| SHMEM 12.1 | probe built; rc 0 for 1, 2, 4 PEs and for 2, 4, 8 NICs (one PE accepts `SHMEM_OFI_NUM_NICS=2`) |
| build | both paths build and run after the three site fixes (PMI 6.1.17 library dir, SHMEM 12.1.0 prefix, no `--gpus-per-node`), now automatic in the Makefile and `ecalc/gres.sh` (Known problems) |
| file system | Lustre over the fabric, no quota limit for the user; ecalc's writer (O_DIRECT, 8 threads): 0.42 GB in 0.08-0.09 s at 1 node (about 5 GB/s), 1.04 GB parts in 0.21-0.31 s at 2 nodes (3.4-5.0 GB/s per part); no multi-node aggregate rate measured |
| harmless warnings | `cxip_domain_enable ... unrestricted service ID` once per NIC per node (4,608 lines at 576 nodes); path 2: `ofi_monitor_subscribe ... ret = -14` and `DATA heap base ... is not page aligned` |

Measured behaviour (m; ecalc's own counters, not a fabric benchmark):

| observation | value |
|---|---|
| node uniformity | init 139.50-139.74 s (old seed default) and dc residue 13.08-13.34 s across 64 nodes; 577 `VERIFY OK` at 576 nodes |
| exchange rate | 3.61 GB/s per APU thread at 2 nodes, 1e10 (11,192 exchanges, 172.64 GB; path 2: 3.60); 4.51 GB/s at 64 nodes, 5.12e11 (13,623 exchanges, 971.92 GB); against the 100 GB/s NIC peak per APU. DATA heap peak 257 of 512 MiB |
| tree levels, 64 nodes | levels 1-6 mean 1.94, 2.42, 2.98, 4.14, 4.55, 5.50 s; node spread <= 0.5 s per level |
| transport open | slowest rank 2.29 s (576 nodes, init only), 3.04 s (576 nodes, 2e13) |
| self-tests | four-mesh test 88-93 s at 2 nodes, 119-125 s at 64 (88 % of init) under the old seed default (CPU contention, a symptom); with explicit seed threads init is 13-16 s. At 576 nodes the plain-mesh test is skipped (not a power of two) and the layered one takes 3.07-3.45 s |
| device memory | 64 nodes: 120.3 GB per node, 27.9-36.5 GB per APU; 1 node 8e9: 105.8 GB, 23.8-34.5 per APU. Per-APU peak is 2.7-6.5 GB above the planner's per-APU figure (APU 0 holds the largest arena) while the node total is 6-9 GB below plan |
| host memory | RSS 8.8 GB, HWM 13.0 GB per node at 64 nodes; VmHWM 21.7 GB (576 nodes, 144 seed threads), 25.5 GB (old default); about 288-290 GB host in use at 2e13 init |

Failures at 576 nodes (not attributed to hardware by the analysis; the cause is open):

| run | symptom | what to collect next time |
|---|---|---|
| 2e13 | `comm_xshmem: pe N: HANG (> 900 s) in the allgather handshake`, printed by two lpe-0 PEs | the first non-HANG error lines with their times (`die(`, `boot_abort`, `srun: error`, `Killed`, oom); which PEs printed the last tree-level line (the PEs that did not are the missing group); elapsed time from the previous level to the HANG; node state of the missing group |
| 3e13 | `P24 CRT flag 1` on several nodes (flag 1 = fifth limb nonzero: wrong residues, not a legitimate overflow) | the full P24 lines (node, plane, prime); whether the same nodes fail again; any allocation failure before it; the `MEM_REPORT_DEVS` per-APU lines and the planner fit line at that size; whether 2e13 passes P24 |

Inference for the 3e13 failure (modelled / assumed: no env.txt of that run exists; the planner figures are the planner at 576 nodes with the target's limits):
- Setting (assumed): `MN_NONPOW2_CAP=0`. The code default `ECALC_DEVICE_GB` is 373.44 and `mnrun.sh` refuses a plan that does not fit (exit 4 unless `MNRUN_FIT=0`); 3e13 with the cap on auto is refused (device 385.93 GB, 96.48 per APU), with cap 0 it fits (350.37 GB, 87.59 per APU) (modelled).
- Piece census (modelled; `MN_PLAN_ONLY`, 576 nodes, cap 0): at 4.608e12 and 2e13 levels 1-6 have 2 pieces in every group (level 7, groups of 192: 4, and 4..6 at 2e13, two groups at 6, the first [192,384)). At 3e13 levels 1-6 are MIXED: groups from node 158/160 on have 4 pieces (a 1 x 2 piece grid per product), the groups below 160 have 2 (level 1: 209 of 288 groups at 4, first [158,160); level 5: 13 of 18, first [160,192); level 6: 6 of 9, first [192,256)); level 7: 2 of 3 groups at 10 pieces, first [192,384). At 3.82e13 every level is uniform again (4 everywhere, 12 at level 7). 3e13 is the one size of the four where products cross the per-piece cap inside the node range: one level runs single-piece and multi-piece grids in different groups at once.
- The CRT-flag nodes the target reported (283, 310, 332, 356, 376, indices about 282-375) all lie in [192,384), the level-7 group with 10 pieces and inside the multi-piece range of levels 1-6: five random nodes would all fall in one third with probability (1/3)^5 = 0.4 % (modelled).
- Lead: multi-piece P24 grids in the tree at mixed piece counts (shifted piece adds, piece windows at the cap, the transform cache across pieces, state carried between products of different grid shapes). The code audit (results/PRIMES_STRATEGY.md §P24 audit) found the bound holds per piece kind, so the tests look for what it could not see: `t_mn_grid` mixed-shape rounds and the aac7 item s9a (ROUTE step 7c).
- Where the shape appears (planner, aac7 2026-10-27, target limits, m): at a per-node share of 2.9e13/576 to 3.6e13/576 digits on 64, 128, 256 and 576 nodes (none at 2.2e13-2.8e13 or 3.82e13 on 576 n); 64 nodes at 3.333e12 digits form it and fit with `MN_NONPOW2_CAP` auto (88.09 GB per APU): the cheap target check of ROUTE step 7c.

Switches for the next try (off by default): `COMM_HANG_DETAIL=1` (the HANG message lists the peers that have not arrived), `XSHMEM_HANG_S=3600` (the HANG timeout, default 900),
`COMM_SEQ_CHECK=1` (call-sequence divergence is fatal naming both calls), `XSHMEM_HS_BATCH=k` (paced, rotated flat handshake), `P24_CHECK=1|2` (input limb checks; a CRT flag prints the product, piece, node and first bad position; the CRT flag itself covers every shift class by default since Q130, `P24_CRT_ANY=0` = the old s = 12 + spill flag),
`ECALC_FIT_PER_APU=1` (the planner also checks the busiest APU against the device limit; 3e13 plans 373 of 373.44 GB per node, mod); `MEM_REPORT_DEVS=1`.
Code reading (mod, results/XSHMEM.md T2): the two 2e13 PEs are both in node range 512-575 (the last level-6 group), so a straggler past the fixed 900 s is the most likely cause, then call-order divergence, then a flat burst.
`BS_SEED_THREADS=auto` is now the default. Switch rows: `ecalc/README.md`.

## Measured runs on the target (2026-10-26; target, m; all VERIFY OK, cold starts, path 1, layout D, `ECALC_PLANE_CAP=run`)

Seconds of `ecalc`'s own lines; "seed default" = the old default (OpenMP team latched from the 1-2 CPUs the hand launch gave each task); no output file except the 1e9 and 1e10 smoke runs.

| run | nodes | digits (per node) | settings | total | init | bs | dm | dc |
|---|---|---|---|---|---|---|---|---|
| smoke | 1 | 1e9 | path 1 / path 2, output | 28.25 / 26.49 | 7.1 / 5.2 | 20.2 / 20.3 | 0.49 / 0.50 | 0.11 |
| 2 n | 2 | 1e10 (5e9) | path 1 / path 2, output | 115.20 / 115.77 | 103.6 / 102.5 | 3.3 / 3.6 | 7.87 / 9.63 | 0.40 / 0.04 |
| L0 | 1 | 8e9 | seed default, path 1 / path 2 | 157.11 / 158.60 | 8.7 / 10.9 | 146.4 / 145.6 | 1.45 | 0.38 |
| n8 | 8 | 6.4e10 (8e9) | seed default | 188 | 150 | 10 | 14 | 14 |
| n64 | 64 | 5.12e11 (8e9) | seed default | 209.49 | 139.7 | 24.4 | 31.0 | 14.3 |
| n576 | 576 | 4.608e12 (8e9) | seed default | 282.24 | 124.1 | 61.3 | 73.3 | 14.5 |
| sweep | 2 | 1.6e10 (8e9) | seed threads default / 48 / 96 / 144 / 192 | 182.67 / 42.51 / 40.57 / 39.09 / 40.60 | 155.7 / - / - / 12.9 / - | 4.5-5.0 | 7.6-7.8 | 14.1-14.3 |
| n64 st144 | 64 | 5.12e11 (8e9) | seed 144 | 84.62 / 84.07 | 15.8 / 15.6 | 23.7 | 30.8 | 14.3 |
| n576 st144 | 576 | 4.608e12 (8e9) | seed 144 (best run) | 255.30 | 111.0 | 56.5 | 73.3 | 14.5 |

Weak scaling at 8e9 per node, `BS_SEED_THREADS=144` (m): 2 nodes init 12.9, bs 4.6, dm 7.6, dc 14.1, total 39.1; 64 nodes 15.8 / 23.7 / 30.8 / 14.3 / 84.6; 576 nodes 111.0 (trend about 18) / 56.5 / 73.3 / 14.5 / 255.3 (about 162 at the trend init). Best run: 4.608e12 digits on 576 nodes in 255.3 s. Model gap (`target_l4` profile, mod): 18.1 / 23.4 / 28.9 s at 2 / 64 / 576 nodes, i.e. 2.2x / 3.6x / 8.8x; the fabric moves 2.9 % of the 400 GB/s per node peak at 576 nodes (11.5 GB/s per node effective; 21 and 18 at 2 and 64). The `target_fit` profile (fitted to the six bs / dm points, per-round sync term) is in `results/TIME_MODEL.md` §target_fit. At 576 nodes: reciprocal 41.0 s, division 25.2 s of products plus shifts 15.1 s (124 calls of 122 ms; model 0.49 s), VmHWM 21.7 GB per node; the dc 14.5 s is the digit residue (13.15 s).

Per-APU calibration of the planner (aac7, m): the planner's per-APU maximum is 106.88 GB against 105.6 GB measured on APU 0 at 9e10 digits per node, and 111.18 against 109.9 GB at 1e11 (1.3 GB above measured both times; APUs 1-3 hold 86-91 GB). On the target at 64 nodes the measured per-APU peaks are 2.7-6.5 GB above the planner's per-APU figure while the node total is 6-9 GB below plan (APU 0 holds region 0's arena). `ECALC_FIT_PER_APU=1` makes the planner check this busiest APU.

## Paths and launch settings

Two multi-node paths; digits are identical between them (user rule Q85, both must keep working).

| | path 1: libfabric (default build) | path 2: Cray SHMEM (the target's shape) |
|---|---|---|
| build (GMP not needed for ecalc, tools and the s2 tests; `GMP_HOME` only for the unit tests and ref/gen_e) | `make -C ecalc` (XSHMEM=ofi; needs libfabric + PMI2 header; log must show `-DXSHMEM_OFI`) | `make -B -C ecalc XSHMEM=cray` with the cray-dsmml and cray-openshmemx modules (`-B` when switching) |
| launch | `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ./ecalc <digits>` : layout D = one process per node, 4 APU threads | `SLURM_JOB_ID=<J> ecalc/mnrun.sh <nodes> env ECALC_PE_PER_GPU=1 XSHMEM_LANES=2 ./ecalc <digits>` : one process per GPU, 2 lanes (lane k = NIC k) |
| third shape "P" | path 1 with `ECALC_PE_PER_GPU=1` (one process per GPU): fastest on aac7 at 2 nodes where it fits, slower at 1 node | - |
| `<nodes>` | the node count; `mnrun.sh` uses the first N nodes of the allocation (`MNRUN_NODES`, `MNRUN_NODELIST`) | same |

Launch settings of the target (`target/config/target.conf.template`; `s1_facts.sh` (probe + `target/probe/gen_conf.py`) creates `target/config/target.conf` from it and fills the `__FILL__` keys):

| setting | value | note |
|---|---|---|
| `ECALC_NP` | `auto` | 3 primes at 1 node, 4 (P24) for multi-node products; `=4` at 1 node +7.5 s (m aac7) |
| `MN_T_CHUNK_MB` | 2048 | -42.3 s at 10 n vs 1024 (m aac7); 1024 vs 2048 under P: noise (m aac7) |
| `MN_NONPOW2_CAP` | 0 | Q119: 3.82e13 on 576 fits only with it off; auto default is off below 4.5e10 digits per node anyway |
| `FI_MR_CACHE_MONITOR` | `userfaultfd` | memhooks hangs 22/105, uffd 0/104 (m aac7); `disabled` gives silently WRONG exchanges, refused by `mnrun.sh` |
| `FI_LOG_LEVEL` | `warn` | `FI_UNIVERSE_SIZE` = max(4096, 4 x tasks), computed by `mnrun.sh` |
| `ECALC_DEVICE_GB` / `ECALC_NODE_GB` | 373.44 / 487 (m) | plan refuses a run that exceeds them |
| `ECALC_MEM_GUARD_GB` | 6 | stops with rc 9 before the OOM killer |
| `ECALC_SEGV_TRACE`, `ECALC_VERBOSE`, `MEM_REPORT_DEVS` | 1, 2, 1 | trace on crash; phase lines; per-APU memory rows. `ECALC_VERBOSE=2` costs at most a few s (+2.0 s over 4 pairs, not resolved, m aac7) |
| `ECALC_PLANE_CAP` | unset = `run` | 2^30 plane pools up to about 9.6e9 digits per node, 2^31 above; not a launch setting (Q122) |
| `MNRUN_CPUS_PER_TASK` | unset = `auto` (since 2026-10-27) | CPUs / tasks per node as `srun -c`; else srun confines a task to 1-2 CPUs (m aac7 and the target: a hand launch ran the seed team and the digit residue 15-30x slow); an explicitly empty value passes no `-c` (old line) |
| `MNRUN_MODULES`, `MNRUN_UNLOAD` | site values | path 2 needs `cray-dsmml cray-openshmemx <rocm>`; unload a conflicting login `rocm` |
| computed, never typed | pool `COMM_SHMEM_POOL_MB`, heap `SHMEM_SYMMETRIC_SIZE`=`XT_SYMMETRIC_HEAP_SIZE`, `MN_GROUPS`, `POOL_LOG` | by the planner / `mnrun.sh`; `tools/plan_values.py --exports <digits> <nodes>` prints them |
| bad nodes | `MNRUN_EXCLUDE_HOSTS=h1,h2`, `MNRUN_EXCLUDE_MODE=drop` | no spares (decision 2026-10-11); `drop` shifts ranks, so ask for N + excluded nodes |

Useful `mnrun.sh` options: `MNRUN_PRINT_ENV=1` (print the computed variables, launch nothing, no allocation needed), `MNRUN_FIT=0`
(skip the fit check), `MNRUN_NODES=n`, `MNRUN_PPG_GPUBIND=none|task`, `MNRUN_PPG_CPUBIND=none|ldom`. One network program per node:
never two comm processes of different jobs on one node (CXI command-queue errors). No digit-file write in multi-node timing runs
(slow file system looks like a hang). Under P/path 2, `mnrun.sh` adds GPU/CPU binding (`--gpus-per-node=4 --gpu-bind=none` where the nodes list a gpu GRES: `MNRUN_GRES`, Known problems; NUMA placement) and `srun --kill-on-bad-exit=1`.

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
| 1.74e10 | 576 | path 2 | 371.64 (wrap off by the auto rule; 380.23 with it), node 467.17 vs 487 | fits by +1.80 (per APU +0.45; refused against the old 414 GB node budget); `DIST_CHUNKS=2` -> 318.56, `=4` -> 288.22, `=8` -> 273.06 (mod) |
| 6.632e10 (3.82e13 on 576) | 576 | path 1 D, `MN_NONPOW2_CAP=0` | 358.96 (+14.48), node 395.04 | fits |
| 6.632e10 | 576 | D, cap auto (on) | 407.41 | REFUSED (-33.97) |
| 6.632e10 | 8-576 | P / path 2 | 430 (8 n) to 671 (576 n) | refused from 8 nodes on |

- Largest sizes on 576 nodes (mod): about 4.19e13 digits with `MN_NONPOW2_CAP=0`, about 2.67e13 with it on; path 2 / P about 5.2e12-7.5e12 (1.3e10 per node with a 480-487 GB node budget, about 9.1e9 with the old 414 assumption).
- Why path 2 jumps at 576 nodes: 576 nodes = 2304 PEs is not a power of two, so every product uses the general exchange map whose v-slots (about 61 GB per node at 1.74e10) are zero at power-of-two node counts. `DIST_CHUNKS=K` slices the exchange rows (code default: 1 under `ECALC_PE_PER_GPU=1`, i.e. path 2; 8 otherwise): v-slots 30.6 / 15.3 / 7.7 GB for K = 2 / 4 / 8 (mod). Digit safety of DIST_CHUNKS: identical for 4 vs 8 on layout D (m aac7); path 2 at non-power-of-two counts untested (TQ84).
- Per-APU record edge (planner, mod, 2026-10-28; `results/LAUNCH_DEFAULTS.md` §A2EDGE and §RECORD1, 576 n, `MN_NONPOW2_CAP=0`, edge 93.36 GB, bar 6.5 GB): the staircase table of ROUTE.md carries the settings per step (that table is the one place to change them). Busiest APU, GB (margin): 1e13 `BS_MDEV_LOGL=29` 63.50 (+29.86); 2e13 29: 74.71 (+18.65); 2.8e13 `BS_MDEV_LOGL=28 DIST_CHUNKS=16` 81.69 (+11.67); 3.82e13 29: 89.74 (+3.62), 28: 87.59 (+5.77), 28 + `DIST_CHUNKS=16`: 85.98 (+7.38). Path 2 fits 8e9 per node (+11.19 per APU) and 1e13 only barely (+0.45; `DIST_CHUNKS=2` +13.72), never 3e13.
- Round size `XSHMEM_ROUND_MB` (planner and time model, mod, 2026-10-28; 576 n, path 1; target limits `ECALC_FIT_PER_APU=1 ECALC_DEVICE_GB=373.44 ECALC_NODE_GB=487 MN_NONPOW2_CAP=0`, 3.82e13 and 3.7e13 with `BS_MDEV_LOGL=28 DIST_CHUNKS=16`; same figures with `COMM_TRANSPORT=xshmem`). The setting only grows the device comm pools: +3.22 GB per node (+0.81 per APU) at 1024 against 128, +5.37 GB per node more at 0 (whole exchange). Busiest APU against the 93.36 GB edge (margin) and node GB against 487:

| digits | 128 (default) | 1024 | 0 |
|---|---|---|---|
| 4.608e12 | 40.33 (+53.03), node 160.54 | 41.13 (+52.23), node 163.76 | 41.67 (+51.69), node 165.91 |
| 3.7e13 | 83.83 (+9.53), node 371.42 | 84.64 (+8.72), node 374.64 | 93.77 REFUSED (-0.41), node 411.15 |
| 3.82e13 | 85.98 (+7.38), node 380.01 | 86.79 (+6.57), node 383.23 | 96.18 REFUSED (-2.82), node 420.81 |

  512 (planner, mod, 2026-10-28, aac7 login run of the same planner): identical to 128 at 3.7e13 (+9.53, node 371.42) and 3.82e13 (+7.38, node 380.01): the pools are sized in 128 MB granules, so 512 is free in memory.
  So 1024 still fits both record sizes and costs 0.8 GB of per-APU margin (3.82e13 +6.57 against the staircase bar 6.5 before any measurement, and against measured excess + 2 GB afterwards: see ROUTE "The record staircase"); 0 is refused at both. Time model (`ecalc/mn_model.py estimate --g 576 --fabric target_fit --round-mb R`, mod): 4.608e12 171.8 s at 128 vs 157.2 s at 1024; 8e12 240.9 vs 200.2; 3.82e13 1072 vs 704 s (R=0 695 s at +31 GB host per node; 1024 about +8 GB host); at 64 nodes 81.0 vs 79.0 s. The fit attributes most exposed fabric time to per-round synchronisation; that mechanism is assumed (the fit cannot tell it from latency or bandwidth), hence the 128 / 1024 pair in P2 (ROUTE; last session measured 255.3 s at 128 for 4.608e12 on 576). Measured on aac7 at 1 node (m, no exchange): 14.9 s at 128 vs 15.5 s at 1024 (noise, digits identical).
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
(8e9 per node at 2 nodes) end in `...29680671395813714711` (m, aac7 2026-10-25 path 1 at 2 nodes, identical with and without
the 2^30 plane cap, and 2026-10-26 path 2 at 1 node; the tail after `...` of the `digits:` line that has one). Measured
tails at 1 node: 8e9 `...33902373363486293584`, 6.4e10 `...97846921235350161654`, 1e11 `...37703810896884928977` (m, aac7,
both paths where run).

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
| a task gets 1-2 CPUs | `mnrun.sh` now defaults to `MNRUN_CPUS_PER_TASK=auto` (m aac7, m target); a hand `srun` needs `-c <CPUs per task>`; `MNRUN_CPUS_PER_TASK=` (empty) restores the old line |
| link/load errors after `module load rocm` | versioned ROCm conflicts with the login default: `MNRUN_UNLOAD` |
| APUs invisible in a step | `--gpus-per-node=4` in the allocation, `--gpu-bind=none` on the step (`mnrun.sh` adds it); not on a site without GRES (next row) |
| `Invalid generic resource (gres) specification` from sbatch/srun (m target: `scontrol show node` -> `Gres=(null)` on the compute nodes) | the site lists no GPU GRES, so every `--gpus*` / `--gpu-bind` option is refused although the GPUs are there. Automatic: `MNRUN_GRES=auto` (default, `ecalc/gres.sh`) leaves the options out unless `sinfo`/`scontrol` list a gpu GRES (aac7 does: unchanged); the answer is cached in `MNRUN_GRES_RESOLVED`; s1 prints `gres_gpu=`. Manual: `MNRUN_GRES=off` (or `on` to force the options) in the environment of every stage; for a hand-written launch drop `--gpus*` and `--gpu-bind` |
| `ld.lld: unable to find library -lpmi2` / `-lpmi` (m target: `/opt/cray/pe/lib64` has only `libpmi2.so.0`, the unversioned libs are in the cray-pmi directory `/opt/cray/pe/pmi/<ver>/lib`) | Automatic: `ecalc/Makefile` picks `PMI_LIBDIR` (for `-L` and rpath, both paths): `/opt/cray/pe/lib64` if it has `libpmi2.so` (aac7: unchanged), else `$CRAY_PMI_PREFIX/lib`, `PMI_HOME/lib`, `/opt/cray/pe/pmi/default/lib`, the newest `/opt/cray/pe/pmi/*/lib`. Manual: `module load cray-pmi` (s2 tries it with cray-shmem) or `make PMI_LIBDIR=/opt/cray/pe/pmi/<ver>/lib` or `export LIBRARY_PATH=<that dir>:$LIBRARY_PATH` |
| path 2 builds against the old 11.8 SHMEM (`cannot find shmem.h`, or links the stale one); `cray-openshmemx/11.8.0` is the PE default module and conflicts with `cray-shmem/12.1.0` (m target) | Automatic: `ecalc/Makefile` takes `CRAY_SMA` from `CRAY_OPENSHMEMX_PREFIX`, else the cray-shmem module's `CRAY_SHMEM_PREFIX` / `CRAY_SHMEM_DIR` / `CRAY_SMA_PREFIX` / `SMA_HOME` (each must hold `include/shmem.h`), else a non-stale `pkg-config --variable=prefix sma`, else the 11.8 default, and only if that is absent the newest `/opt/cray/shmem/sma/*/ofi`; `CRAY_DSMML` likewise (`CRAY_DSMML_PREFIX`, the 0.3.1 default, else the newest `/opt/cray/shmem/dsmml/*`). `_build.sh` and the probe pick the newest of cray-shmem / cray-openshmemx, unload a loaded cray-openshmemx first (a module swap) and add cray-pmi. Manual: `module unload cray-openshmemx; module load cray-shmem/12.1.0 cray-dsmml/0.5.0 cray-pmi`, or `export CRAY_OPENSHMEMX_PREFIX=/opt/cray/shmem/sma/12.1.0/ofi` (and `CRAY_DSMML_PREFIX=/opt/cray/shmem/dsmml/0.5.0`) |
| GMP not found although the gmp module is loaded (m target: the module sets `GMP_BASE`, not `GMP_HOME`; libs in `lib64`) | Automatic: s2 uses `GMP_BASE` as `GMP_HOME` when it holds `include/gmp.h`; the Makefile also links `$GMP_HOME/lib64`. GMP is needed only by the unit tests and `ref/gen_e`. Manual: `export GMP_HOME=$GMP_BASE` |
| probe/`gen_conf.py` proposes `ECALC_OUT_DIR` under a file system root the user cannot write | the probe records `fs.writable_mounts`; `gen_conf.py` takes only a writable shared mount (or `$SCRATCH` below it), else a directory under `$HOME` (its quota is advised). Manual: edit `ECALC_OUT_DIR` / `MNACCEPT_TMP` in `target/config/target.conf` (created by `s1_facts.sh`) |
| `srun`/`sbatch` inherit `SLURM_HINT=nomultithread` and `SLURM_DISTRIBUTION=block:block` from the login environment (m target). With the hint each task starts pinned to its first cores and OpenMP latches 1-2 threads: the default seed threads made init 150 s instead of 13-16 s on the target and an 8e9 run 156 s instead of 15.4 s on aac7 (m) | `mnrun.sh` drops the hint for its srun (`MNRUN_KEEP_HINT=1` keeps it) and passes `--distribution=block` itself; `BS_SEED_THREADS=auto` (3/4 of the task's CPUs; the default) protects hand launches; in a hand-written srun add `--hint=multithread` or `-c 48` |
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
| 10 | write rate: `tools/wbench` (built from `tools/wbench.c` by the io bench items, `make wbench` in `tools/`) 8 vs 32 threads, per-node vs one shared file; `MN_OUT_THREADS` (TQ54-TQ56) | 1 then 64 nodes | the part file is 0.417 B per digit (15.9 TB at 3.82e13); 1 GB/s per node assumed; matters only for runs that write | 10-15 min |
| 11 | ROCm 7.14 vs an older module (TQ30) | rebuild + 1-node ABBA | aac7: 7.0.3 +22 % vs 7.2.4 | 25 min |
| 12 | `XSHMEM_ROUND_MB` 128 vs 1024 is in P2 at 576 n (ROUTE); `DM_MN_WRAP` 0/1, `ECALC_NP` 3 vs 4, `BS_SEED_CHUNK_MB`, `ECALC_BG_THREADS` 24/48/96 | pairs | aac7: all within noise except NP=4 (+7.5 s); try only if the phase table points at bs or dm | 4-8 runs each |
| 13 | init cost: VMM map rate, path 2 heap THP (`XSHMEM_CRAY_HEAP_THP=1`, `ECALC_INIT_TL=1`) (TQ35-TQ37) | `ECALC_INIT_TL=1` timeline | init is 24 s of 77 s at 1 node on aac7; at 8e9 per node init is a large share of the wall | 2-4 runs |
| 14 | digit residue and host thread team: `ECALC_RESIDUE=shoup ECALC_OMP_TEAM=mask` vs the defaults (bit-identical by design) | P2: four 64-node arms A B B A at 8e9 per node (ROUTE) | the residue is local per node (target m: dc 13.08-13.34 s per node across 64 nodes at the default team); aac7 1 node (m): dc 0.2 s with the pair, 0.3-0.4 s default, digits identical (33902373363486293584 at 8e9); the target's 13 s may already be gone under `MNRUN_CPUS_PER_TASK=auto`, which the test shows | 4 runs of 64 n, about 6 min |

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

Open facts only the target can supply (not in the repository; module names, partition limits, `MemAvailable` and userfaultfd are answered in Target site): the real injection rate with 1 and 2 lanes, all-to-all scaling and
latency, start-up time for path 2, ROCm 7.14 speed against 7.2.4, the file-system rate of ecalc's own writer,
slow-launch behaviour, and whether the 2^30 plane cap still saves start-up above 2 nodes.
