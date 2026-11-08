# RESULTS — what is in force, and the evidence for it

Scope: an index of what is in force: pointers to the current figures, adopted changes and launch-line settings, each with the evidence file. | Maintained: updated in the same merge as any change to a default, a launch-line
setting or an adopted result.

Reports in `results/` are dated records, frozen when written; where one differs from the code or the central docs, the
code and the central docs win. Reports are grouped by topic (`results/<TOPIC>.md §<ID>`); a new report starts as
`results/<ID>.md` and is merged into its topic file later. Ideas measured and not adopted are in `OPPORTUNITIES.md`.
History is outside the repository: the old measurement logs (`archive:RESULTS_ARCHIVE_1-114.md`,
`archive:RESULTS_HISTORY_2026-10-12.md`, "RESULTS §NN" citations), the old-ID lookup, merged and superseded report sections
(`archive:results_merged_2026-10-19/`, `archive:results_merged_2026-10-23/`, `archive:results_merged_2026-10-24/`),
raw data and drivers (`archive:results_raw_2026-10-19/`); per-item sections of SESSIONS, CACHE, AAC7_PORT, OFI_FABRIC, SHMEM (`archive:repo_cl6_2026-10-25/results/`).

Labels: **m** measured, **mod** modeled, **a** assumed. Times are wall-clock seconds. Test machine: aac7 (MI300A nodes,
Slingshot; the only cluster in use).

## Current figures

Measured figures live in the topic files, not here: run times by scale and path in `results/TRANSPORT.md` (§craylane, §XWLAT, §BM)
and `results/MODEL.md` (§AAC7OPT, §M2a, §SCALE35); the target model, memory fit and piece boundaries in `results/MODEL.md` (§TGT, §FIT,
§PIECES, §P2JUMP, §L4); the aac7 campaigns M1-M6 (stress, soak, planner against memory, round cost, init growth, paired timings) in
`results/MEMORY_IO.md`, `results/TRANSPORT.md` and `results/CORRECTNESS.md`; the block-pool audit in `results/MEMORY_IO.md` §POOLRACE; the
recheck at scale in `results/CORRECTNESS.md` §CKPT. The target's own runs are in `remote/FACTS.md`.

## 2026-11 cleanup: size before and after (measured 2026-11-07, Eastern; counts by `git ls-files` and `wc -l`)

| item | before | after | label |
|---|---|---|---|
| tracked files | 348 | 204 | m |
| folders with tracked files | 29 | 20 | m |
| settings registry rows (`ecalc/switches.tsv`) | 209 | 101 | m |
| C sources, headers and tests (`ecalc/`, lines) | 29,112 | 27,942 | m |
| test files (`.c`/`.h` under `tests/` and `ecalc/tests/`) | 33 | 27 | m |
| Python lines | 8,092 | 7,695 | m |
| shell lines | 7,437 | 5,418 | m |
| Markdown | 7,570 lines, 80 files | 4,714 lines, 24 files | m |

Removed: the unknown-name guard, `ECALC_TEST` folding (the former `DIST_*`
and other test variables are keys of `ECALC_TEST`), one removed settings class, the archived `target/` material (now outside the repository) and
consolidated `results/` (topic files). Digits unchanged.

Speed check, pre-cleanup tag against the new main, 2 nodes, 6.441e10 digits, 8 alternating pairs: new minus old -1.55 s, 95% interval [-5.13, +2.02] s,
which includes 0, so no change is detectable (m). 2 nodes, 1e11 digits: identical to the stored reference, 262.75 s total (m).

## Multi-node transport in force (Q73, Q85, Q96, Q100, Q103)

| change | since | effect | label | evidence |
|---|---|---|---|---|
| `COMM_TRANSPORT=xshmem`, `XSHMEM_ROUND_MB=128` is the only multi-node transport (`XSHMEM_ORDER` fixed to `rot` 2026-10-25) (default build `XSHMEM=ofi`); the plain SHMEM transport (`COMM_TRANSPORT=shmem`, `comm_shmem.c`) and `COMM_TRACE` were removed 2026-10-25 (Q103) | 2026-10-21 | 3 n 1.9323e11: xshmem 149.3 s (sd 1.2, n 8) vs libfabric reference 157.3 s (sd 6.8, n 9), paired -8.5 s [-14.0, -3.0], 8/8 pairs; default 153.2 vs plain shmem 161.3 s (10 n 8.1e11: libfabric 1669.98 vs shmem 3532.05 s, `results/TRANSPORT.md`); gates 1/2/3 n PASS, mnaccept mn 5/5, unit 9/9; whole-exchange staging fails the memory budget at full size, hence the rounds | m | results/TRANSPORT.md §10, §7 |
| Cray SHMEM backend `xshmem_cray.h` (`XSHMEM=cray`), zero engine changes | 2026-10-21 | 1e9 identical at 1, 2, 3 n; `t_xshmem` PASS; first-run failure (unregistered heap tail page) fixed | m | results/TRANSPORT.md §8 |
| one process per GPU `ECALC_PE_PER_GPU=1` (path 2 layout; default layout stays D), `XSHMEM_LANES`, `RNS_BATCH_SPLIT` (automatic under `ECALC_PE_PER_GPU=1`, Q110, 2026-10-25) | 2026-10-23 | digits identical to D at 1, 2, 3 n on both backends; P faster at small sizes, equal at 3 n 1e11 (path 1) | m | results/TRANSPORT.md §PPG, §BSPLIT |
| NUMA placement for Cray builds under P (`XSHMEM_CRAY_NUMA=1`, `MNRUN_PPG_CPUBIND=ldom`, set by `mnrun.sh`) | 2026-10-23 | cured the Cray stalls (40-107 s runs -> 35-38 s at 3 n 1e10); THP disable did not | m | results/TRANSPORT.md craylane |
| exchange worker thread in every layout (`ECALC_XEX_THREAD` removed 2026-10-25, always on) | 2026-10-24 | init -6.5 to -7.5 s at 2 n 1e11 (host memory halved) | m | results/TRANSPORT.md §XWLAT |
| `XSHMEM_PEER` default on with one PE per process (Q100); `=0` restores the NIC path; layout D and the Cray backend unaffected | 2026-10-25 | same-node PEs exchange by hipIpc device copies: P at 1 n 3.3e10 51.6 -> 34.9 s, 2 n 6e10 69.7 -> 49.0 s; per-PE DATA heap peak 256 of 768 MiB | m | results/COMPUTE.md §PEER |
| `COMM_LAYER_INTER2` removed: one exchange in flight is faster on both transports | 2026-10-21 | SHMEM 143.2 vs 149.9 s, libfabric 137.2 vs 140.2 s (3 n 1.9323e11) | m | results/TRANSPORT.md |
| Cray SHMEM 11.8 tuning phase closed (2026-10-21): the 3 n gap of the plain SHMEM transport was the layered variable-count exchange (SHMEM 157.0 vs libfabric 143.0 s, 6 pairs); 21 environment settings, DEVHEAP2, DEVSRC, zero-copy and NUMA flush gave nothing | 2026-10-21 | proxy mode measured, then removed (Q78); the transport itself went in Q103 | m | results/TRANSPORT.md |

## Adopted changes in force (code defaults)

Every change is digit-identical in its gates. Settings named "built in" no longer exist in the code (removals:
`results/CODE_TOOLS.md §W2A`, `§W2B`); for the D18-D25 and D20 defaults the `=0` fallback switch was removed 2026-10-23 (Q87), the
names identify the change only.

| change | since | effect | label | evidence |
|---|---|---|---|---|
| one single-node division flow (DKM only), no 3·2^k planes in the distributed tier, one grid planner; shrink pass 3 | 2026-10-12 | digits identical at all sizes tested; 1e10 at 1 node -8.31 s [-9.68, -6.93] | m | results/CODE_TOOLS.md §SIMP, §SH3 |
| decimal limbs 10¹⁸ only | 2026-10-12 | binary pipeline was 1.9-2.2 × slower at 2 nodes (+32.47 s at 10¹⁰) | m | results/CODE_TOOLS.md §H4 |
| `DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL` | 2026-10-12 | -7.16 s [-8.08, -6.23] 1 node; -5.58 s [-10.54, -0.63] 2 nodes; reciprocal chain 4.57 -> 0.06 s | m | results/COMPUTE.md §CAND |
| kernels `DBIG_ADDSUB2/3`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL`, `DBIG_MODQ_MONT` | 2026-10-10/11 | together -50.0 s at 4 nodes [-71.7, -28.3]; K2/K3 1 node -2.03 s | m | results/COMPUTE.md §S38, §S51 |
| carry-lookahead CRT kernels (`k_crt_batch` D21, `k_crt24` D24) and the batch tier's host pipelining (D20) | 2026-10-12/13 | 1 n -2.52 s [-4.19, -0.85]; -0.99 s [-1.18, -0.80]; k_crt24 kernel 1.55x (2 n total -1.89 s, n.s.) | m | results/COMPUTE.md §K4b, §K4c, §N3 |
| `NEWTON_CHAIN_GUARD` (D25), `DM_MN_WRAP` (D22, automatic with ≥ 5 GB device margin, D26) | 2026-10-12/13 | guard removes a rare fatal (error -22 -> 0 units); wrap: 6 n dm -6.25 s [-11.47, -1.03], +5.4 GB per APU | m | results/COMPUTE.md §RECIP, §P1 |
| division in two quotient halves (DKM) | Phase 15 | -25.7 s at 10¹¹ | m | results/COMPUTE.md §DKM15 |
| P24: 24 digits per point, four primes in every multi-node product (D18) | 2026-10-12 | -38 % at 10¹⁰ on 2 processes; 2 n -8.9 s at 1.2882e11 | m | results/COMPUTE.md §P2415, §MEAS-P24 |
| `RNS_STRATEGY=auto`, three primes at size 1, `ECALC_NP=auto` above the bound | Phase 13/15 | three primes: -17 % wall, -25.8 GB | m | results/COMPUTE.md §B13b, §NP15 |
| write overlapped with the division: deferred corrections (built in), `MN_OUT_EARLY=1`, `MN_OUT_DKM_HI=1`; packed 60-bit output `ECALC_OUT_PACKED=2` | Phase 15; 2026-10-12 | write hidden under the division; 0.4167 B/digit, 10¹⁰ write 18.6 vs 27.9 s ASCII | m | results/MEMORY_IO.md §IO15, §O4 |
| `FI_MR_CACHE_MONITOR=userfaultfd` set by `mnrun.sh` (`disabled` refused); `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` | 2026-10-06/11 | reproducer hangs 22/105 -> 0/104; 46 four-node runs, 0 segfaults | m | results/CORRECTNESS.md §S46, results/MODEL.md §TGTBENCH2 |
| transport opened before the arenas (`COMM_INIT_EARLY`, built in) | 2026-10-05 | init segfault 8/78 before; ≥ 32 processes start | m | results/CORRECTNESS.md §S16 |
| `DIST_CHUNKS=8` (was 4) | 2026-10-07 | device -13.4 GB at 576 nodes for +2.4 s | mod | results/COMPUTE.md §S45 |
| exchange slots in the node budget; `COMM_LAYER_VSLOT_SHARE` and `_RELEASE` always on; transform cache sized and counted | 2026-10-07..19 | -8.4 GB per node (share) and -8.60 GB device peak (release) at 4 n; cache required at the target | m/mod | results/COMPUTE.md §S36, §C06, §B7ACCT, results/TRANSPORT.md |
| `BS_SEED_FILL=128`, `DB_POOL_VMM=1`, `MN_T_CHUNK_MB=1024` code default | Phase 14/15 | seeds fill their lengths; no in-phase allocation; pool 81.6 -> 45.0 GB per node | m/mod | results/COMPUTE.md §T215, results/COMPUTE.md §R114, results/TRANSPORT.md |
| `MN_NONPOW2_CAP` auto from 4.5e10 digits per node-process (Q58) | 2026-10-20 | 6 n 3.70e11 -18.3 s (libfabric), +41.3 GB plane memory per node; 3 n 6e10 +8.9 s with it on | m | results/COMPUTE.md §NPCAP |
| `BS_ARENA_ROOM` 0.14 (was 0.16; Q99) | 2026-10-25 | -8.6 GB device memory per node at the 2 n bs-phase peak, 0 remaps, time neutral (4 ABBA pairs); `BS_ARENA_ROOM=0.16` restores | m | results/MEMORY_IO.md §AS15 |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/MEMORY_IO.md §L114 |
| P24 CRT flag on every shift class (`P24_CRT_ANY=1`; `=0` is the old s = 12 flag) (Q130) | 2026-10-31 (merge of cand3) | closes the silent wrong-coefficient hole at s = 0 / 6; digits identical; fault injection at `P24_CHECK=0` fatal at 1 and 2 nodes | m | results/COMPUTE.md §P24 audit, results/TRANSPORT.md §G2N |
| `mnrun.sh` passes `srun -c <CPUs / tasks per node>` (`MNRUN_CPUS_PER_TASK` unset = auto; empty = no `-c`) | 2026-10-31 (merge of cand3) | without it srun confines a task to 1-2 CPUs; `-c 192` for one task per node, `-c 48` for four | m | results/CORRECTNESS.md, remote/FACTS.md |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/CORRECTNESS.md §V |
| two-deep v-exchange (built in) | Phase 13 | xGMI stage hidden under the fabric (1.4 -> 74 %) | m | results/TRANSPORT.md §X13 |

## Launch-line settings in force (not code defaults)

On both launch lines (`profiles/aac7.conf` `LAUNCH_LINE_KEYS`; `target/config/target.conf.template`).

| setting | effect | label | evidence |
|---|---|---|---|
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]); 2 n 5e10 under P: 1024 vs 2048 +0.62 s over 9 pairs (2026-10-25, §AAC7OPT) | m | results/CODE_TOOLS.md, results/MODEL.md §S47, §AAC7OPT |
| X2: the pooled exchange slots (`COMM_LAYER_VSLOT_POOL`, now built in, no switch) | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/CODE_TOOLS.md |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/MODEL.md §B01 |
| aac7 fastest settings (2026-10-25) | layout by size -- 1 n 6.6e10 the defaults (P +9.6 s), 2 n up to 5e10 `ECALC_PE_PER_GPU=1` on the default build (-14.7 s, -27 %), above that D; path 2 +13.9 s vs P; `ECALC_NP`, `BS_SEED_CHUNK_MB`, `ECALC_BG_THREADS`, `MN_T_CHUNK_MB`, `XSHMEM_ROUND_MB` within noise; `ECALC_NP=4` +7.5 s | m | results/MODEL.md §AAC7OPT |
