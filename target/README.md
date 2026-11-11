# target/ — remote testing and evaluation on the target system

> The copy-and-paste route (`remote/`) was removed 2026-11-08 by user decision (git history, commit cfe9d3a1; `git show cfe9d3a1:remote/FACTS.md`). The package below is the Claude Code route; it has not yet been run on the target. Earlier text of this file: `archive:target_README_2026-11-10.md`.

Scope: everything for the target session (576 nodes × 4 MI300A, 8 Slingshot NICs per node, Cray SHMEM 12.1.0): what we
bring (scripts, the ecalc build, rules) and what we collect (raw results, committed as measured). An agent on the target
starts with `RESTART.md`; conventions are in `FORMATS.md` (folder map: §1).

## Run order (each step is ordered by priority, so the session can end at any point)

| step | what | where | time |
|---|---|---|---|
| 1 | system facts: `probe/probe.sh` on a login node, then inside a one-node job; `probe/gen_conf.py` → `config/target.conf` | login + 1 node | ≤ 10 min |
| 2 | build path 1 and path 2 (bench items `bld01`, `bld02`) | login node | ~5 min |
| 3 | bench: workers loop `bench/run.sh` in parallel allocations; answers in each item's `summary.txt` | 1-64 nodes each | minutes per item |
| 4 | ladder: `ladder/ladder.sh --level L0` (a few runs, adjust per `ladder/ADJUST.md`), then L1, L2, L3 | 1 / 8 / 64 / 576 nodes | per level |

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

Failures at 576 nodes (cause open; analysis in `PLAN.md` and `results/COMPUTE.md`): 2e13 `comm_xshmem: pe N: HANG (> 900 s) in the allgather handshake` (two PEs in node range 512-575) and 3e13 `P24 CRT flag 1` on nodes 283-376, both at a mixed piece layout at tree level 7; worked around by `MN_NONPOW2_CAP=0` (and `MN_LEVEL_UNIFORM=1` in the frozen remote-v12 record steps; open item in `TASKS.md`). Switches for the next try (off by default): `COMM_HANG_DETAIL=1`, `XSHMEM_HANG_S`, `COMM_SEQ_CHECK=1`, `XSHMEM_HS_BATCH`, `P24_CHECK=1|2`, `ECALC_FIT_PER_APU=1`, `MEM_REPORT_DEVS=1` (rows in `ecalc/README.md`).

Memory limits and planner results (target limits; m = measured on the target, mod = planner):
- Device edge `hipMalloc` 373.44 GB per node = 93.36 GB per APU (m, 5 runs; the planner default `ECALC_DEVICE_GB`). Host budget `ECALC_NODE_GB=487` from MemAvailable 515.7 GB on a freshly booted idle node (m; code default 480); after a heavy job an idle node showed about 491 GB, so plan node budgets from about 490 GB; the Slurm job memory is about 497 GB per node (m).
- Planner (mod, per node, device GB against 373.44): 8e9 digits per node at 576 nodes path 2: 328.69 (+44.75), node 406.32; layout D 155 at 1 node, 178-181 at 8-576; 1.74e10 (1e13 on 576) path 1 D 238.97 (+134.47), path 2 371.64 with the automatic `DM_MN_WRAP` off (380.23 with it; node 467.17 vs 487; fits by +1.80); 6.632e10 (3.82e13 on 576) path 1 D with `MN_NONPOW2_CAP=0` 358.96 (+14.48), node 395.04: fits; with the cap on auto 407.41: REFUSED (-33.97); path 2 / P refused from 8 nodes on. Largest on 576 nodes: about 4.19e13 digits with `MN_NONPOW2_CAP=0`, about 2.67e13 with it on. Busiest APU against 93.36 GB (cap 0): 1e13 `BS_MDEV_LOGL=29` 63.50 (+29.86); 2e13 74.71 (+18.65); 3.82e13 89.74 (+3.62), `BS_MDEV_LOGL=28` 87.59, with `DIST_CHUNKS=16` 85.98 (+7.38).
- `DM_MN_WRAP` auto resolved to 1 at 1 node and 0 at 2-576 nodes (m, target analysis). Mesh creation 1.6-1.9 s at 64 nodes, layered self-test 3.07-3.45 s at 576, transport open 2.29 s slowest rank (m). VMM map 0.045-0.055 s per GB (arena 45.1 GB in 2.02-2.50 s; m, target analysis; the target profile says 0.010, aac7 0.070). Digits: 1e9 byte-identical for both paths, 8e9 identical, path 1 = path 2 at 2 nodes 1e10 (m).

Measured runs (target, m; path 1 layout D unless noted; seconds; seed default = old default):

| run | nodes | digits | total | init | bs | dm | dc |
|---|---|---|---|---|---|---|---|
| smoke, path 1 / 2 | 1 | 1e9 | 28.25 / 26.49 | 7.1 / 5.2 | 20.2 / 20.3 | 0.49 / 0.50 | 0.11 |
| 2 n, path 1 / 2 | 2 | 1e10 | 115.20 / 115.77 | 103.6 / 102.5 | 3.3 / 3.6 | 7.87 / 9.63 | 0.40 / 0.04 |
| L0, path 1 / 2 | 1 | 8e9 | 157.11 / 158.60 | 8.7 / 10.9 | 146.4 / 145.6 | 1.45 | 0.38 |
| n8 | 8 | 6.4e10 | 188 | 150 | 10 | 14 | 14 |
| n64 | 64 | 5.12e11 | 209.49 | 139.7 | 24.4 | 31.0 | 14.3 |
| n576 | 576 | 4.608e12 | 282.24 | 124.1 | 61.3 | 73.3 | 14.5 |
| seed sweep (default / 48 / 96 / 144 / 192) | 2 | 1.6e10 | 182.67 / 42.51 / 40.57 / 39.09 / 40.60 | 155.7 (default), 12.9 (144) | 4.5-5.0 | 7.6-7.8 | 14.1-14.3 |
| n64, seed 144 | 64 | 5.12e11 | 84.62 / 84.07 | 15.8 | 23.7 | 30.8 | 14.3 |
| n576, seed 144 | 576 | 4.608e12 | 255.30 | 111.0 | 56.5 | 73.3 | 14.5 |

Host memory: VmHWM 12.0 GB (1 node), 13.0 (64), 21.7 (576, 144 seed threads), 25.5 (old default). 8e9 per node: format 0.42 s, digit residue 13.15 s = 1.79 s per 1e9 digits (m).

P24 / 3e13 failure inference (mod / assumed; no env.txt of the run): `MN_NONPOW2_CAP=0` assumed; 3e13 with the cap on auto is refused (device 385.93 GB, 96.48 per APU), with cap 0 it fits (350.37, 87.59 per APU). Piece census (`MN_PLAN_ONLY`, 576 nodes, cap 0): at 4.608e12 and 2e13 levels 1-6 have 2 pieces in every group; at 3e13 levels 1-6 are MIXED (groups from node 158/160 on have 4 pieces, those below 2) and level 7 has 10 pieces in the group [192,384); at 3.82e13 every level is uniform again. The CRT-flag nodes (283, 310, 332, 356, 376) all lie in [192,384). Lead: multi-piece P24 grids at mixed piece counts (shifted piece adds, piece windows at the cap, the transform cache across pieces); the audit in `results/COMPUTE.md` §P24 audit found nothing. The shape appears at 2.9e13/576 to 3.6e13/576 digits per node on 64-576 nodes (planner, aac7); 64 nodes at 3.333e12 digits form it. Collect next time: the first non-HANG error lines with times, the full P24 lines (node, plane, prime), and `MEM_REPORT_DEVS=1` per-APU lines. 2e13 code reading (mod, `results/TRANSPORT.md` T2): a straggler past the fixed 900 s is the most likely cause, then call-order divergence, then a flat burst.

Known problems and fixes: `RESTART.md` §8.
