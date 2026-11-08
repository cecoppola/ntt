# target/ — remote testing and evaluation on the target system

> **Workflow.** The copy-and-paste route (`remote/`, browser agent, no agent on the target) was removed 2026-11-08 by user decision; it is in git
> history (commit cfe9d3a1 and earlier, `git show cfe9d3a1:remote/FACTS.md`) and on the frozen branch remote-v12. The Claude Code workflow below (run order
> step 0 and `RESTART.md` §0, §2) is background only.

Scope: everything for the target session (576 nodes × 4 MI300A, 8 Slingshot NICs per node, Cray SHMEM 12.1.0): what we
bring (scripts, the ecalc build, rules) and what we collect (raw results, committed as measured). An agent on the target
starts with `RESTART.md`; conventions are in `FORMATS.md`.

## Run order (each step is ordered by priority, so the session can end at any point)

| step | what | where | time |
|---|---|---|---|
| 0 | (Claude-Code workflow only, not used in the paste session) install the rules and memory | login node | 5 min |
| 1 | system facts: `probe/probe.sh` on a login node, then inside a one-node job; `probe/gen_conf.py` → `config/target.conf` | login + 1 node | ≤ 10 min |
| 2 | build path 1 and path 2 (bench items `bld01`, `bld02`) | login node | ~5 min |
| 3 | bench: workers loop `bench/run.sh` in parallel allocations; answers in each item's `summary.txt` | 1-64 nodes each | minutes per item |
| 4 | ladder: `ladder/ladder.sh --level L0` (a few runs, adjust per `ladder/ADJUST.md`), then L1, L2, L3 | 1 / 8 / 64 / 576 nodes | per level |

## Folder map

See `FORMATS.md` §1.

## Target site facts (measured 2026-10-26, target, m; condensed from the removed `remote/FACTS.md`)

Hand-run by the user, one default partition of 576 nodes; every node started ecalc at least once and no slow or faulty node was seen.

| item | value |
|---|---|
| scheduler | Slurm 25.11, whole nodes, no time limit, no GPU GRES (`Gres=(null)`: `--gpus*` options are refused; `MNRUN_GRES=auto`, `ecalc/gres.sh`), `MpiDefault=cray_shasta` |
| topology | nine 64-node leaf switches = the dragonfly groups, consecutive node numbers (1-64, 65-128, ...) |
| node | 4 APUs, 96 Zen 4 cores = 192 threads, 4 NUMA nodes (numa0 CPUs 0-23,96-119; numa1 24-47,120-143; numa2 48-71,144-167; numa3 72-95,168-191); MemTotal 501.3 GiB, MemAvailable about 490-515 GB idle, Slurm RealMemory 509,000 MB |
| kernel | THP `always`; hugepages about 0; `numa_balancing=0`; memlock unlimited; `max_map_count` 65530; Rocky 9.6 |
| NIC to NUMA | cxi0,1 -> numa0; cxi2,3 -> numa1; cxi4,5 -> numa2; cxi6,7 -> numa3; 400 Gb/s; `pe_geo` picks these pairs with `COMM_OFI_NICS` unset |
| modules | `rocm/7.14.0`, `cray-shmem/12.1.0`, `cray-pmi/6.1.17`, `libfabric/2.3.1`; the PE default `cray-openshmemx/11.8.0` conflicts with `cray-shmem/12.1.0`; `gmp/6.3.0` sets `GMP_BASE`, not `GMP_HOME` |
| userfaultfd | `vm.unprivileged_userfaultfd=0` yet `FI_MR_CACHE_MONITOR=userfaultfd` works; keep it (`disabled` = silent wrong digits, aac7) |
| file system | Lustre, writer (O_DIRECT, 8 threads) about 3.4-5 GB/s per part |
| harmless warnings | `cxip_domain_enable ... unrestricted service ID` (once per NIC per node); path 2: `ofi_monitor_subscribe ... ret = -14`, `DATA heap base ... is not page aligned` |

Runs (path 1, layout D, all VERIFY OK, cold starts; weak scaling at 8e9 digits per node, `BS_SEED_THREADS=144`): 2 nodes total 39.1 s (init 12.9, bs 4.6, dm 7.6, dc 14.1); 64 nodes 84.6 s (15.8 / 23.7 / 30.8 / 14.3); 576 nodes 255.3 s (4.608e12 digits; 111.0 / 56.5 / 73.3 / 14.5; engine older than v12, see `results/MODEL.md` §v12). With the old seed default init took 103-150 s and the 576-node total was 282 s. Exchange rate 3.6 GB/s per APU thread at 2 nodes, 4.5 at 64 (NIC peak 100). Device memory per node 120.3 GB at 64 nodes (APU peaks 2.7-6.5 GB above the planner's per-APU figure, node total 6-9 GB below plan).

Failures at 576 nodes (cause open; analysis in `PLAN.md` and `results/COMPUTE.md`): 2e13 `comm_xshmem: pe N: HANG (> 900 s) in the allgather handshake` (two PEs in node range 512-575) and 3e13 `P24 CRT flag 1` on nodes 283-376, both at a mixed piece layout at tree level 7; worked around by `MN_NONPOW2_CAP=0`. Switches for the next try (off by default): `COMM_HANG_DETAIL=1`, `XSHMEM_HANG_S`, `COMM_SEQ_CHECK=1`, `XSHMEM_HS_BATCH`, `P24_CHECK=1|2`, `ECALC_FIT_PER_APU=1`, `MEM_REPORT_DEVS=1` (rows in `ecalc/README.md`).

Known problems and fixes (target, m unless noted):

| symptom | cause and fix |
|---|---|
| hang in memory registration (ROCr <-> libfabric) | keep `FI_MR_CACHE_MONITOR=userfaultfd` |
| rc 4 / rc 8 at start; rc 9 | plan does not fit: lower digits to the printed maximum; rc 9 memory guard: lower `ECALC_NODE_GB` by the printed overshoot |
| `fi_enable(-28)` with several PEs per node | launch Slurm-native (`srun --ntasks`), as `mnrun.sh` does |
| a task gets 1-2 CPUs | `MNRUN_CPUS_PER_TASK=auto` (default); a hand `srun` needs `-c <CPUs per task>` |
| `srun` inherits `SLURM_HINT=nomultithread` and `SLURM_DISTRIBUTION=block:block` from the login environment | OpenMP latches 1-2 threads: init 150 s instead of 13-16 s (target), 8e9 run 156 s instead of 15.4 s (aac7). `mnrun.sh` drops the hint (`MNRUN_KEEP_HINT=1` keeps it) and passes `--distribution=block`; `BS_SEED_THREADS=auto` (3/4 of the task CPUs, default) protects hand launches; a hand `srun` adds `--hint=multithread` or `-c 48`. `alloc.sh`-style launches and the target scripts do not inherit the hint |
| `ld.lld: unable to find library -lpmi2`; path 2 links the old 11.8 SHMEM; GMP not found | automatic in `ecalc/Makefile` (`PMI_LIBDIR`, `CRAY_SMA`, `GMP_BASE` as `GMP_HOME`) |
| random multi-second stalls of all 4 APU threads of a node | kernel compaction of a 4 KiB pinned heap (SHMEM heap; path 1 is not affected): hugetlb/THP heap, checklist in `results/TRANSPORT.md` |
| `ECALC_NP=3` memory remedy | three primes cut plane memory (154.6 GB planes, `mem_model`), at most `ec_np3_max_terms` terms per product; aac7 1 node 3 primes -0.19 s vs auto, 4 primes +7.5 s |
| slow node or missing NIC | `tools/nodescreen.sh` (NIC count 8 per node); exclude the host |
| first run on a fresh allocation slow (2x on aac7) | cold start: time from run 2 |
| `ECALC_VERBOSE=2` very long at 576 nodes | use `ECALC_VERBOSE=1` |
| a run seems hung but is a slow file system | no digit output in timing runs; wait for `total` |
