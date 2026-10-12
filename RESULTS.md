# RESULTS — what is in force, and the evidence for it

This file is a short index: the current measured figures, every adopted change with the file that holds its evidence,
and the changes that were measured and not adopted. The full measurement log is kept outside the repository:
sections §1-§114 in `archive:RESULTS_ARCHIVE_1-114.md` and §115-§145 (to 2026-10-12) in
`archive:RESULTS_HISTORY_2026-10-12.md`. "RESULTS §NN" citations in other notes refer to those section numbers.
Agent reports are in `results/<ID>.md`; reports that no longer support the current code are in `archive:results/`.

Labels: **m** measured, **mod** modelled, **a** assumed. Times are wall-clock seconds; "digits/node" is the size divided by
the node count. Test machines: aac7 (MI300A nodes with Slingshot, the main test system) and aac6 (single-node coverage).

## Current figures

| what | figure | label | evidence |
|---|---|---|---|
| 1 node, 6.441 × 10¹⁰ digits | ≈ 76-78 s (S48: 76.3 s mean, before the 2026-10-12 defaults) | m | results/S48.md, S43.md |
| 1 node, 10¹¹ digits | 129.3 s, VERIFY OK, digits identical to the reference (aac6) | m | results/L2.md |
| 2 / 4 / 6 nodes, 6.441 × 10¹⁰ digits/node | 139.0 / 181.8 / 293.5 s (mean totals) | m | results/S48.md |
| 4-node run-to-run modes | fast ≈ 181-185 s, slow ≈ 208-214 s: the inter-node all-to-all rate is set per launch | m | results/T05.md, T09.md, A13.md |
| 4-node calibration | total ≈ 80.8 s + 1388 / r (r = all-to-all GB/s per APU thread), residual sd 1.6 s | m/mod | results/B07.md |
| target: 576 nodes, 3.82 × 10¹³ digits | ≈ 182 s without the write (profile `target_l4`) | mod (fabric a) | results/L4.md, O8.md |
| target device memory, rank 0, 3.82 × 10¹³ | margin 12.9 GB under the 373.44 GB/node edge (launch line with `MN_T_CHUNK_MB=2048`, X2, slot release) | mod | results/L1.md, B01.md, S52.md |
| target device edge | 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread) | m (target) | results/TGTBENCH2.md |
| `comm_ofi` vs SHMEM, 10 nodes, 8.1 × 10¹¹ | 1669.98 s vs 3532.05 s (× 2.1) | m | results/OFI17.md, STD17.md |

## Adopted changes in force (code defaults)

Every change below is digit-identical (VERIFY OK and identical to the reference in its gates). Where a switch still exists,
setting it to 0 restores the old path; switches marked "removed" are built in.

| change | since | effect | label | evidence |
|---|---|---|---|---|
| decimal limbs 10¹⁸ only; binary pipeline removed | 2026-10-12 | binary was 1.9-2.2 × slower at 2 nodes (+32.47 s at 10¹⁰) | m | results/H4.md, H5.md, H1.md, H2.md |
| `DBIG_SMALL_BOUNCE=1`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL=1` | 2026-10-12 | -7.16 s [-8.08, -6.23] 1 node (aac6); -5.58 s [-10.54, -0.63] 2 nodes (aac7); reciprocal chain 4.57 → 0.06 s | m | results/CAND.md, O2.md, K1b.md, K8b.md |
| kernels K2, K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`; switches removed, built in) | 2026-10-11 | 1 node -2.03 s ± 1.38 (aac7) | m | results/S51.md, T08.md |
| `FI_MR_CACHE_MONITOR=userfaultfd` set by `mnrun.sh`; `disabled` refused | 2026-10-11 | reproducer hangs 22/105 → 0/104; 46 four-node runs, 0 segfaults, 0 hangs | m | results/S46.md, T12.md |
| kernels `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL` (switches removed, built in) | 2026-10-10 | together -50.0 s at 4 nodes [-71.7, -28.3] | m | results/S38.md, S40.md, S41.md, S43.md |
| `DIST_CHUNKS=8` (was 4) | 2026-10-07 | device -13.4 GB at 576 nodes for +2.4 s | mod | results/S45.md |
| `ECALC_VSLOT_BUDGET=1` (exchange slots in the node budget) | 2026-10-07 | no measured cost | m | results/B7ACCT.md, S18M.md |
| `COMM_OFI=1` where a cxi NIC is present | 2026-10-06 | × 2.1 over SHMEM at 10 nodes | m | results/OFI17.md, STD17.md |
| `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` set by `mnrun.sh` when unset | 2026-10-06 | fixes multi-process `fi_enable(-28)`; log volume / 1000 | m (target) | results/TGTBENCH2.md |
| `COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1` | 2026-10-05 | transport opened before the arenas (init segfault 8/78 before); ≥ 32 processes start | m | results/S16.md, P16.md |
| `NEWTON_DKM=1`: division in two quotient halves | Phase 15 | division -25.7 s at 10¹¹ | m | results/DKM15.md |
| `MN_P24=2`: 24 digits per transform point in multi-node products | Phase 15 | -38 % at 10¹⁰ on 2 processes | m | results/P2415.md |
| `ECALC_CORR_PATCH=2`, `MN_OUT_EARLY=1`: write overlapped with the division, corrections patched | Phase 15 | write hidden under the division | m | results/IO15.md, IO215.md |
| `ECALC_OUT_PACKED=1`: packed output, 0.444 B/digit | Phase 15 | ASCII made off the clock by `tools/unpack_digits` | m | results/W.md |
| `BS_SEED_FILL=128` | Phase 15 | the batch tier's products fill their lengths | m | results/T215.md |
| `RNS_DIST_CACHE_FIT=1` (transform cache sized and counted) | Phase 15 | required at the target | m | results/TC15.md |
| `DB_POOL_VMM=1`: arenas as HIP virtual memory | Phase 14 | no in-phase allocation | m | results/R114.md |
| `MN_T_CHUNK_MB=1024` code default | Phase 14 | pool 81.6 → 45.0 GB per node | mod | results/P214.md |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/L114.md |
| `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, three primes at size 1 | Phase 13 | three primes: -17 % wall, -25.8 GB | m | results/B13b.md, X13b.md, archive RESULTS §78-80 |
| `COMM_ALLTOALLV_DEPTH=2` | Phase 13 | xGMI stage hidden under the fabric (1.4 → 74 %) | m | results/X13.md |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/V.md |

## Launch-line settings in force (not code defaults)

On both launch lines (`ecalc/e16_headline.sh` for aac7, `profiles/target.conf.template` for the target). Six of them become
code defaults in the current code reduction (TASKS 27).

| setting | effect | label | evidence |
|---|---|---|---|
| `ECALC_NP=auto` (4 primes above the three-prime bound) | needed at the target size | mod | results/NP15.md, P15.md |
| `MN_OUT_DKM_HI=1`, `RNS_DIST_CACHE_PARTIAL=1` | write starts after division step 1; cache from free pool bytes, -12.8 s at 5.1 × 10¹³ | mod | results/TC15.md, IO215.md |
| `DM_MN_LEAN=1` | same digits, less division memory | m | archive RESULTS §104 |
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]) | m | results/S22.md, S47.md |
| X2: `COMM_LAYER_INTER2=1`, `COMM_LAYER_VSLOT_POOL=1` | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/S31.md |
| `COMM_LAYER_VSLOT_SHARE=1` | -8.4 GB per node at 4 nodes; -5.79 GB at 576 | m / mod | results/S36.md, S45.md |
| `COMM_LAYER_VSLOT_RELEASE=1` | device peak -8.60 GB at 4 nodes; -8.59 GB at 576 | m / mod | results/C06.md, L1.md |
| `COMM_SHMEM_ROUND_MB=1024` | SHMEM pool 45.0 → 9.8 GB per node | mod | results/V114.md |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/B01.md |

Decided, implementation in progress: packed 60-bit output `ECALC_OUT_PACKED=2` as the default (-6.25 % bytes, m;
digits identical, round trip byte-identical; results/C05.md, O4.md).

## Measured and not adopted

| idea | result | label | evidence |
|---|---|---|---|
| `MN_GROUPS=6` instead of `2,6` at 6 nodes | +2.4 s [-3.9, +8.7], p 0.35 | m | results/O1.md |
| uneven node groups | correct, not faster (5 nodes, one run each) | m | results/B06.md |
| B5 product form | +0.08 s [-0.75, +0.91] | m | results/O5.md |
| `RNS_DIST_OVERLAP` (K6) | -0.11 ± 0.15 s of 73.4 s | m | results/K6.md |
| `COMM_SHMEM_RPIPE` (removed), `MN_REDIST_FUSE` | +1.82 s [-14.35, +17.99], -0.78 s [-40.89, +39.34] | m | results/O2.md |
| init reorderings (K8) | each +2.0 .. +11.5 s | m | results/K8.md |
| `DIST_GATHER_TILE` | -0.08 s ± 0.88 at 1 node | m | results/S50.md |
| result-exchange rebalancing | about -7 s at 10 nodes, below noise | mod | results/A07.md |
| message coalescing (`DIST_COALESCE`) | 1.0-4.5 s at 576 nodes; idea saved | mod | results/S52.md |
| `ECALC_VMM_BG=2` | 9/9 reproducer runs failed | m | results/S46.md |
| 4 processes per node (one per APU) | 3.7 × slower at 8 nodes | m | archive RESULTS §101 |
| `hipMallocManaged` host spill at the device edge | host memory 21 GB/s vs HBM 3.2-3.8 TB/s | m | results/TGTBENCH2.md |

## Open measurement questions

- Fabric injection rate per NIC on the target and its fall-off at 576 nodes (A3, A4): every target time above uses an
  assumed rate. The target kit `ecalc/target_kit.sh` measures both (results/S18KIT.md, A06.md).
- `MN_T_CHUNK_MB` 1024 vs 2048 and X2 on the target: not yet measured there.
- The cause of the 4-node slow mode (per-launch NIC group) is not established (results/A14.md).

## New entries

Add one short section per merged experiment below this line: date, what changed, the numbers with labels, and the
`results/<ID>.md` file.
