# RESULTS — what is in force, and the evidence for it

Scope: an index of what is in force: current measured figures, adopted changes, launch-line settings, open measurement
questions, each with the evidence file. | Maintained: updated in the same merge as any change to a default, a launch-line
setting or an adopted result.

Reports in `results/` are dated records, frozen when written; where one differs from the code or the central docs, the
code and the central docs win. Reports are grouped by topic (`results/<TOPIC>.md §<ID>`); a new report starts as
`results/<ID>.md` and is merged into its topic file later. Ideas measured and not adopted are in `OPPORTUNITIES.md`.
History is outside the repository: the old measurement logs (`archive:RESULTS_ARCHIVE_1-114.md`,
`archive:RESULTS_HISTORY_2026-10-12.md`, "RESULTS §NN" citations), the old-ID lookup, merged and superseded report sections
(`archive:results_merged_2026-10-19/`, `archive:results_merged_2026-10-23/`, `archive:results_merged_2026-10-24/`),
raw data and drivers (`archive:results_raw_2026-10-19/`).

Labels: **m** measured, **mod** modelled, **a** assumed. Times are wall-clock seconds. Test machine: aac7 (MI300A nodes,
Slingshot; the only cluster in use).

## Current figures

Run times at the current engine are in `PLAN.md` §2; this table holds the figures behind the defaults.

| what | figure | label | evidence |
|---|---|---|---|
| path 1 (libfabric, layout D) vs path 2 (Cray SHMEM, one process per GPU, 2 lanes), 3 n 1e11, same hold, 11 interleaved pairs | 83.4 vs 89.6 s, +6.2 s, +7.5 % (CI +3.4 to +11.5 %); measured before Q96; 2 n after Q96 (same layout P, 4 ABBA pairs): 1e10 +0.32 s [-0.51, 1.15], 1e11 -9.69 s [-27.9, 8.5], parity (results/XSHMEM.md, 9zz7); compute phases at parity, the gap is init (+8 s, aac7-specific: libsma heap NIC registration 6.2 s) | m | results/XSHMEM.md craylane |
| exchange worker default in every layout (Q96) | init -6.5 to -7.5 s in 5 of 5 runs at 2 n 1e11 (DATA heap 9408 -> 960 MiB per PE, host memory 16.6 -> 8.0 GB); 2 n 1e10 -8.2 s [-9.4, -7.0] (path 2); bs and dm neutral; at 1e11 the paired total is not resolved (+4.1 s poll minus worker, CI [-14.7, +22.9], n 3) | m | results/XSHMEM.md §XWLAT |
| digit identity at production size, main after Q96 | 2 n 1e11 on both paths: sha 578f5efb0ff2b9af (same as the xwconf3 reference) | m | results/XSHMEM.md §XWLAT |
| day-one checks T1-T8 on aac7, diagnostic arms | flush and `XSHMEM_LOCAL`/`XSHMEM_IPC` neutral; lanes 1 vs 2 neutral (aac7 has one NIC per PE); xshmem 11 % faster than `comm_ofi` at 2 n 1e10; Cray a2a bimodal 11 / 17 GB/s unexplained; T1, T2, T5, T6 (NIC map), T8 on a device heap need the target | m | results/XSHMEM.md §XDIAG |
| aac7 bimodal run times | about half of the 3 n 1e11 runs are 5-8 % slower in every compute phase, both backends; absent at 1 node without network (271 runs); needs the fabric; pair every A/B run | m | results/NODE_SLOWMODE.md §BM |
| start-up hang E1 (7.7e10, one sighting in 31 on old code) | 0 hangs in 174 runs at 7.7e10 and 0 in 1127 at 1e9 on current code (95 % bound 1.7 % and 0.27 %); the old 3.2 % rate is excluded; watch item only | m | results/CRASHES_HANGS.md §A114 |
| budget check with two PEs per GPU (plq2) | `budget_check` counted the transport's comm_ofi pools, already allocated at init, as still to come: the 2 n 1e10 Q2 run was refused (rc 8) though it fits; fixed (merged 3d39601), gate 9zx5 PASS; no unmodelled per-process host term (VmRSS 2.6-5.5 GB) | m | TASKS.md (item row); `ecalc.c` budget_check |
| 4-node run-to-run modes (libfabric era, `comm_ofi`) | fast ≈ 181-185 s, slow ≈ 208-214 s: the inter-node all-to-all rate is set per launch | m | results/NODE_SLOWMODE.md §T05, §T09 |
| 4-node calibration (libfabric era) | total ≈ 80.8 s + 1388 / r (r = all-to-all GB/s per APU thread), residual sd 1.6 s | m/mod | results/TOOLS_PROFILES.md §B07 |
| target: 576 nodes, 3.82 × 10¹³ digits | ≈ 182-185.5 s without the write (profile `target_l4`; fabric rate assumed; 182 s in results/TIME_MODEL.md §L4, `PLAN.md` §2 quotes 185.5 s; the two differ by the model version, not re-reconciled here) | mod | results/TIME_MODEL.md §L4, `PLAN.md` §2 |
| target device memory, rank 0, 3.82 × 10¹³ | margin +14.48 GB under the 373.44 GB/node edge (device 358.96 GB per node; `MN_T_CHUNK_MB=2048`, X2, slot release; the planner figure of 2026-10-19 that replaced the 12.9 GB of L1; not re-run since `BS_ARENA_ROOM` 0.14; re-check 2026-10-25 (m, planner, `plan_values.py --fit` with `ECALC_NP=auto MN_T_CHUNK_MB=2048`): device 407.41 GB per node, margin -33.97 GB, refused; identical on main 0d052a9 and after cleanup round 4; cause open (TASKS)) | mod | results/LAUNCH_DEFAULTS.md §FIT |
| target device edge | 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread) | m (target) | results/TARGET.md §TGTBENCH2 |
| `BS_ARENA_TRIM_GB=8` (opt-in, default 0), 2 n 1.2882e11 | arena -34.4 GB per node (266.3 -> 231.9); 115.2 s (7 runs) vs 111.4 s (5 runs); 3 n 1.9323e11: 149.4 vs 149.6 s; 0 remaps | m | results/MEMORY.md §ATRIM |
| memory fit before every launch (D26): the planner refuses a run that does not fit | target 3.82e13:576: 358.96 GB device vs 373.44 (`DM_MN_WRAP` off; with it 384.73, refused); aac7 6 nodes 374.22 vs 444 | mod/m | results/LAUNCH_DEFAULTS.md §FIT |
| node screen NIC check `tools/nodescreen.sh --want-nics N` | flags nodes whose runtime sees fewer cxi NICs; 4-node check: 4 seen, all flagged at want 8 (positive control) | m | results/NODE_SLOWMODE.md §NIC |
| checkpoint recheck at scale | 6 n 3.8646e11: write 1473 s, VERIFY OK, RECHECK OK on all nodes; recheck 7380 s (model ≈ 1 h); "short read" was NFS EIO under 512 MiB reads, reads now ≤ 64 MiB with retries | m/mod | results/CHECKPOINT.md §CKPT |

## Multi-node transport in force (Q73, Q85, Q96, Q100, Q103)

| change | since | effect | label | evidence |
|---|---|---|---|---|
| `COMM_TRANSPORT=xshmem`, `XSHMEM_ROUND_MB=128` is the only multi-node transport (`XSHMEM_ORDER` fixed to `rot` 2026-10-25) (default build `XSHMEM=ofi`); the plain SHMEM transport (`COMM_TRANSPORT=shmem`, `comm_shmem.c`) and `COMM_TRACE` were removed 2026-10-25 (Q103) | 2026-10-21 | 3 n 1.9323e11: xshmem 149.3 s (sd 1.2, n 8) vs libfabric reference 157.3 s (sd 6.8, n 9), paired -8.5 s [-14.0, -3.0], 8/8 pairs; default 153.2 vs plain shmem 161.3 s (10 n 8.1e11: libfabric 1669.98 vs shmem 3532.05 s, `results/OFI_FABRIC.md §OFI17`); gates 1/2/3 n PASS, mnaccept mn 5/5, unit 9/9; whole-exchange staging fails the memory budget at full size, hence the rounds | m | results/XSHMEM.md §10, §7 |
| Cray SHMEM backend `xshmem_cray.h` (`XSHMEM=cray`), zero engine changes | 2026-10-21 | 1e9 identical at 1, 2, 3 n; `t_xshmem` PASS; first-run failure (unregistered heap tail page) fixed | m | results/XSHMEM.md §8 |
| one process per GPU `ECALC_PE_PER_GPU=1` (path 2 layout; default layout stays D), `XSHMEM_LANES`, `RNS_BATCH_SPLIT` (automatic under `ECALC_PE_PER_GPU=1`, Q110, 2026-10-25) | 2026-10-23 | digits identical to D at 1, 2, 3 n on both backends; P faster at small sizes, equal at 3 n 1e11 (path 1) | m | results/XSHMEM.md §PPG, §BSPLIT |
| NUMA placement for Cray builds under P (`XSHMEM_CRAY_NUMA=1`, `MNRUN_PPG_CPUBIND=ldom`, set by `mnrun.sh`) | 2026-10-23 | cured the Cray stalls (40-107 s runs -> 35-38 s at 3 n 1e10); THP disable did not | m | results/XSHMEM.md craylane |
| exchange worker thread in every layout (`ECALC_XEX_THREAD` removed 2026-10-25, always on) | 2026-10-24 | see Current figures | m | results/XSHMEM.md §XWLAT |
| `XSHMEM_PEER` default on with one PE per process (Q100); `=0` restores the NIC path; layout D and the Cray backend unaffected | 2026-10-25 | same-node PEs exchange by hipIpc device copies: P at 1 n 3.3e10 51.6 -> 34.9 s, 2 n 6e10 69.7 -> 49.0 s; per-PE DATA heap peak 256 of 768 MiB | m | results/SINGLE_NODE.md §PEER |
| `COMM_LAYER_INTER2` removed: one exchange in flight is faster on both transports | 2026-10-21 | SHMEM 143.2 vs 149.9 s, libfabric 137.2 vs 140.2 s (3 n 1.9323e11) | m | results/SHMEM.md |
| Cray SHMEM 11.8 tuning phase closed (2026-10-21): the 3 n gap of the plain SHMEM transport was the layered variable-count exchange (SHMEM 157.0 vs libfabric 143.0 s, 6 pairs); 21 environment settings, DEVHEAP2, DEVSRC, zero-copy and NUMA flush gave nothing | 2026-10-21 | proxy mode measured, then removed (Q78); the transport itself went in Q103 | m | results/SHMEM.md |

## Adopted changes in force (code defaults)

Every change is digit-identical in its gates. Settings named "built in" no longer exist in the code (removals:
`results/CODE_REDUCTION.md §W2A`, `§W2B`); for the D18-D25 and D20 defaults the `=0` fallback switch was removed 2026-10-23 (Q87), the
names identify the change only.

| change | since | effect | label | evidence |
|---|---|---|---|---|
| one single-node division flow (DKM only), no 3·2^k planes in the distributed tier, one grid planner; shrink pass 3 | 2026-10-12 | digits identical at all sizes tested; 1e10 at 1 node -8.31 s [-9.68, -6.93] | m | results/CODE_REDUCTION.md §SIMP, §SH3 |
| decimal limbs 10¹⁸ only | 2026-10-12 | binary pipeline was 1.9-2.2 × slower at 2 nodes (+32.47 s at 10¹⁰) | m | results/CODE_REDUCTION.md §H4 |
| `DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL` | 2026-10-12 | -7.16 s [-8.08, -6.23] 1 node; -5.58 s [-10.54, -0.63] 2 nodes; reciprocal chain 4.57 -> 0.06 s | m | results/SINGLE_NODE.md §CAND |
| kernels `DBIG_ADDSUB2/3`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL`, `DBIG_MODQ_MONT` | 2026-10-10/11 | together -50.0 s at 4 nodes [-71.7, -28.3]; K2/K3 1 node -2.03 s | m | results/KERNELS.md §S38, §S51 |
| carry-lookahead CRT kernels (`k_crt_batch` D21, `k_crt24` D24) and the batch tier's host pipelining (D20) | 2026-10-12/13 | 1 n -2.52 s [-4.19, -0.85]; -0.99 s [-1.18, -0.80]; k_crt24 kernel 1.55x (2 n total -1.89 s, n.s.) | m | results/KERNELS.md §K4b, §K4c, §N3 |
| `NEWTON_CHAIN_GUARD` (D25), `DM_MN_WRAP` (D22, automatic with ≥ 5 GB device margin, D26) | 2026-10-12/13 | guard removes a rare fatal (error -22 -> 0 units); wrap: 6 n dm -6.25 s [-11.47, -1.03], +5.4 GB per APU | m | results/DIVISION.md §RECIP, §P1 |
| division in two quotient halves (DKM) | Phase 15 | -25.7 s at 10¹¹ | m | results/DIVISION.md §DKM15 |
| P24: 24 digits per point, four primes in every multi-node product (D18) | 2026-10-12 | -38 % at 10¹⁰ on 2 processes; 2 n -8.9 s at 1.2882e11 | m | results/PRIMES_STRATEGY.md §P2415, §MEAS-P24 |
| `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, three primes at size 1, `ECALC_NP=auto` above the bound | Phase 13/15 | three primes: -17 % wall, -25.8 GB | m | results/PRIMES_STRATEGY.md §B13b, §NP15 |
| write overlapped with the division: `ECALC_CORR_PATCH=2`, `MN_OUT_EARLY=1`, `MN_OUT_DKM_HI=1`; packed 60-bit output `ECALC_OUT_PACKED=2` | Phase 15; 2026-10-12 | write hidden under the division; 0.4167 B/digit, 10¹⁰ write 18.6 vs 27.9 s ASCII | m | results/OUTPUT_IO.md §IO15, §O4 |
| `FI_MR_CACHE_MONITOR=userfaultfd` set by `mnrun.sh` (`disabled` refused); `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` | 2026-10-06/11 | reproducer hangs 22/105 -> 0/104; 46 four-node runs, 0 segfaults | m | results/CRASHES_HANGS.md §S46, results/TARGET.md §TGTBENCH2 |
| transport opened before the arenas (`COMM_INIT_EARLY`, built in) | 2026-10-05 | init segfault 8/78 before; ≥ 32 processes start | m | results/CRASHES_HANGS.md §S16 |
| `DIST_CHUNKS=8` (was 4) | 2026-10-07 | device -13.4 GB at 576 nodes for +2.4 s | mod | results/GRID_GROUPS.md §S45 |
| exchange slots in the node budget; `COMM_LAYER_VSLOT_SHARE` and `_RELEASE` always on; transform cache sized and counted | 2026-10-07..19 | -8.4 GB per node (share) and -8.60 GB device peak (release) at 4 n; cache required at the target | m/mod | results/GRID_GROUPS.md §S36, §C06, §B7ACCT, results/CACHE.md §TC15 |
| `BS_SEED_FILL=128`, `DB_POOL_VMM=1`, `MN_T_CHUNK_MB=1024` code default | Phase 14/15 | seeds fill their lengths; no in-phase allocation; pool 81.6 -> 45.0 GB per node | m/mod | results/SINGLE_NODE.md §T215, results/DIVISION.md §R114, results/SHMEM.md §P214 |
| `MN_NONPOW2_CAP` auto from 4.5e10 digits per node-process (Q58) | 2026-10-20 | 6 n 3.70e11 -18.3 s (libfabric), +41.3 GB plane memory per node; 3 n 6e10 +8.9 s with it on | m | results/DIVISION.md §NPCAP |
| `BS_ARENA_ROOM` 0.14 (was 0.16; Q99) | 2026-10-25 | -8.6 GB device memory per node at the 2 n bs-phase peak, 0 remaps, time neutral (4 ABBA pairs); `BS_ARENA_ROOM=0.16` restores | m | results/MEMORY.md §AS15 |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/MEMORY.md §L114 |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/CHECKPOINT.md §V |
| two-deep v-exchange (built in) | Phase 13 | xGMI stage hidden under the fabric (1.4 -> 74 %) | m | results/EXCHANGE.md §X13 |

## Launch-line settings in force (not code defaults)

On both launch lines (`profiles/aac7.conf` `LAUNCH_LINE_KEYS`; `target/config/target.conf.template`).

| setting | effect | label | evidence |
|---|---|---|---|
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]); 2 n 5e10 under P: 1024 vs 2048 +0.62 s over 9 pairs (2026-10-25, §AAC7OPT) | m | results/SESSIONS.md §S22, results/LAUNCH_DEFAULTS.md §S47, §AAC7OPT |
| X2: the pooled exchange slots (`COMM_LAYER_VSLOT_POOL`, now built in, no switch) | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/SESSIONS.md §S31 |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/LAUNCH_DEFAULTS.md §B01 |
| aac7 fastest settings (2026-10-25): layout by size -- 1 n 6.6e10 the defaults (P +9.6 s), 2 n up to 5e10 `ECALC_PE_PER_GPU=1` on the default build (-14.7 s, -27 %), above that D; path 2 +13.9 s vs P; `ECALC_NP`, `BS_SEED_CHUNK_MB`, `ECALC_BG_THREADS`, `MN_T_CHUNK_MB`, `XSHMEM_ROUND_MB` within noise; `ECALC_NP=4` +7.5 s | m | results/LAUNCH_DEFAULTS.md §AAC7OPT |

## Open measurement questions

- Fabric injection rate per NIC on the target and its fall-off at 576 nodes (A3, A4): every target time above uses an
  assumed rate. The target kit `target/kit/target_kit.sh` measures both (results/TOOLS_PROFILES.md §A06).
- `MN_T_CHUNK_MB` 1024 vs 2048 on the target: not yet measured there (X2 is built in, no switch).
- Path 2 on the target (Cray SHMEM 12.1): the init gap (+8 s on aac7), the day-one checks T1-T8 on a device heap (results/XSHMEM.md §XDIAG), two real NICs per PE.
- The cause of the aac7 slow modes (per-launch NIC group at 4 n; bimodal 3 n run times) is not established (results/NODE_SLOWMODE.md §A14, §BM).
