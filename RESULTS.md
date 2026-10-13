# RESULTS — what is in force, and the evidence for it

Scope: an index of the evidence for what is in force: adopted changes, launch-line settings, changes measured and not adopted,
open measurement questions. | Maintained: updated in the same merge as any change to a default, a launch-line setting or an
adopted result.

Reports in `results/` are dated records, frozen when written; where one differs from the code or the central docs, the code
and the central docs win.

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
| headline timings (1 node at 6.441 × 10¹⁰ and 10¹¹ digits; 2 / 4 / 6 nodes) | kept in `PLAN.md` §2 (Current status), with their evidence files | m | PLAN.md §2 |
| 4-node run-to-run modes | fast ≈ 181-185 s, slow ≈ 208-214 s: the inter-node all-to-all rate is set per launch | m | results/T05.md, T09.md, A13.md |
| 4-node calibration | total ≈ 80.8 s + 1388 / r (r = all-to-all GB/s per APU thread), residual sd 1.6 s | m/mod | results/B07.md |
| target: 576 nodes, 3.82 × 10¹³ digits | ≈ 182 s without the write (profile `target_l4`) | mod (fabric a) | results/L4.md, O8.md |
| target device memory, rank 0, 3.82 × 10¹³ | margin 12.9 GB under the 373.44 GB/node edge (launch line with `MN_T_CHUNK_MB=2048`, X2, slot release) | mod | results/L1.md, B01.md, S52.md |
| target device edge | 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread) | m (target) | results/TGTBENCH2.md |
| `comm_ofi` vs SHMEM, 10 nodes, 8.1 × 10¹¹ | 1669.98 s vs 3532.05 s (× 2.1) | m | results/OFI17.md, STD17.md |

## Adopted changes in force (code defaults)

Every change below is digit-identical (VERIFY OK and identical to the reference in its gates). Where a switch still exists,
setting it to 0 restores the old path; a setting named "built in" no longer exists in the code (settings removal, 2026-10-12: `results/W2A.md`, `results/W2B.md`).

| change | since | effect | label | evidence |
|---|---|---|---|---|
| one single-node division flow, DKM the only division, no 3*2^k planes in the distributed tier, one grid planner | 2026-10-12 | digits identical at all sizes tested; 1e10 at 1 node -8.31 s [-9.68, -6.93] (4 rounds) | m | results/SIMP.md |
| shrink pass 3: gen_pack_bench + plain pack kernels, rns_mul_dist_hd, pow_table, part_owner, k_store duplicate, cx_grid (-261 lines net); host tier kept (design in the file) | 2026-10-12 | gates 2 n / 1 n and mnaccept unit,e9,mn all PASS, digits identical | `results/SH3.md` |
| decimal limbs 10¹⁸ only; binary pipeline removed | 2026-10-12 | binary was 1.9-2.2 × slower at 2 nodes (+32.47 s at 10¹⁰) | m | results/H4.md, H5.md, H1.md, H2.md |
| `DBIG_SMALL_BOUNCE=1`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL=1` | 2026-10-12 | -7.16 s [-8.08, -6.23] 1 node (aac6); -5.58 s [-10.54, -0.63] 2 nodes (aac7); reciprocal chain 4.57 → 0.06 s | m | results/CAND.md, O2.md, K1b.md, K8b.md |
| kernels K2, K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`; built in) | 2026-10-11 | 1 node -2.03 s ± 1.38 (aac7) | m | results/S51.md, T08.md |
| `FI_MR_CACHE_MONITOR=userfaultfd` set by `mnrun.sh`; `disabled` refused | 2026-10-11 | reproducer hangs 22/105 → 0/104; 46 four-node runs, 0 segfaults, 0 hangs | m | results/S46.md, T12.md |
| kernels `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL` (built in) | 2026-10-10 | together -50.0 s at 4 nodes [-71.7, -28.3] | m | results/S38.md, S40.md, S41.md, S43.md |
| `DIST_CHUNKS=8` (was 4) | 2026-10-07 | device -13.4 GB at 576 nodes for +2.4 s | mod | results/S45.md |
| exchange slots in the node budget (`ECALC_VSLOT_BUDGET=1`; built in) | 2026-10-07 | no measured cost | m | results/B7ACCT.md, S18M.md |
| `COMM_OFI=1` where a cxi NIC is present | 2026-10-06 | × 2.1 over SHMEM at 10 nodes | m | results/OFI17.md, STD17.md |
| `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` set by `mnrun.sh` when unset | 2026-10-06 | fixes multi-process `fi_enable(-28)`; log volume / 1000 | m (target) | results/TGTBENCH2.md |
| transport opened before the arenas, self-test growth (`COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1`; built in) | 2026-10-05 | transport opened before the arenas (init segfault 8/78 before); ≥ 32 processes start | m | results/S16.md, P16.md |
| division in two quotient halves (`NEWTON_DKM=1`; built in) | Phase 15 | division -25.7 s at 10¹¹ | m | results/DKM15.md |
| 24 digits per transform point in multi-node products (`MN_P24=2`; built in) | Phase 15 | -38 % at 10¹⁰ on 2 processes | m | results/P2415.md |
| `ECALC_CORR_PATCH=2`, `MN_OUT_EARLY=1`: write overlapped with the division, corrections patched | Phase 15 | write hidden under the division | m | results/IO15.md, IO215.md |
| `ECALC_OUT_PACKED=2`: packed output, 60-bit, 0.4167 B/digit (=1, 0.444 B/digit, until 2026-10-12) | Phase 15; 2 since 2026-10-12 | ASCII made off the clock by `tools/unpack_digits` | m | results/W.md, results/O4.md |
| `BS_BATCH_PIPE=1`: the batch tier's host side in parallel, tiles back to back (user decision D20; `=0` fallback) | 2026-10-12 | 1 n −0.99 s [−1.18, −0.80], 4 ABBA rounds, digits identical | m | results/N3.md |
| `RNS_CRT_SCAN=1`: `k_crt_batch` carry-lookahead (user decision D21; `=0` fallback) | 2026-10-12 | 1 n 6.441e10 −2.52 s [−4.19, −0.85], 4 ABBA rounds, digits identical | m | results/K4b.md |
| `P24_CRT_SCAN=1`: `k_crt24` (the P24 CRT) carry-lookahead (user decision D24; `=0` fallback) | 2026-10-13 | kernel 1.55x; 2 n 6.441e10 per node total -1.89 s [-7.51, +3.73] n.s., 4 ABBA rounds, digits identical | m | results/K4c.md |
| `NEWTON_CHAIN_GUARD=1`: the reciprocal's chain keeps a guard limb per doubling (user decision D25; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-13 | removes a rare fatal (>64 corrections after runs of exact doublings); SIMP Q error -22 -> 0 units; digits identical | m | results/RECIP.md |
| `DM_MN_WRAP=1`: the division's low product X·Q as a low part + one cyclic plane (user decision D22; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-12 | 6 n (P24) dm −6.25 s [−11.47, −1.03], total −2.86 s n.s.; +5.4 GB per APU | m | results/P1.md, DEF.md |
| `BS_SEED_FILL=128` | Phase 15 | the batch tier's products fill their lengths | m | results/T215.md |
| transform cache sized and counted (`RNS_DIST_CACHE_FIT=1`; built in) | Phase 15 | required at the target | m | results/TC15.md |
| `DB_POOL_VMM=1`: arenas as HIP virtual memory | Phase 14 | no in-phase allocation | m | results/R114.md |
| `MN_T_CHUNK_MB=1024` code default | Phase 14 | pool 81.6 → 45.0 GB per node | mod | results/P214.md |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/L114.md |
| `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, three primes at size 1 | Phase 13 | three primes: -17 % wall, -25.8 GB | m | results/B13b.md, X13b.md, archive RESULTS §78-80 |
| two-deep v-exchange (`COMM_ALLTOALLV_DEPTH=2`; built in) | Phase 13 | xGMI stage hidden under the fabric (1.4 → 74 %) | m | results/X13.md |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/V.md |

## Launch-line settings in force (not code defaults)

On both launch lines (`profiles/aac7.conf` (`LAUNCH_LINE_KEYS`) for aac7, `profiles/target.conf.template` for the target). Rows marked
"code default" were moved off the lines into the code on 2026-10-12 (TASKS 27, `results/W2B.md`; each keeps a `=0` fallback).

| setting | effect | label | evidence |
|---|---|---|---|
| `ECALC_NP=auto` (4 primes above the three-prime bound) | needed at the target size | mod | results/NP15.md, P15.md |
| `MN_OUT_DKM_HI=1`, `RNS_DIST_CACHE_PARTIAL=1` (code default) | write starts after division step 1; cache from free pool bytes, -12.8 s at 5.1 × 10¹³ | mod | results/TC15.md, IO215.md |
| lean division (`DM_MN_LEAN=1`; built in) | same digits, less division memory | m | archive RESULTS §104 |
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]) | m | results/S22.md, S47.md |
| X2: `COMM_LAYER_INTER2=1`, `COMM_LAYER_VSLOT_POOL=1` (code default) | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/S31.md |
| `COMM_LAYER_VSLOT_SHARE=1` (code default) | -8.4 GB per node at 4 nodes; -5.79 GB at 576 | m / mod | results/S36.md, S45.md |
| `COMM_LAYER_VSLOT_RELEASE=1` (code default) | device peak -8.60 GB at 4 nodes; -8.59 GB at 576 | m / mod | results/C06.md, L1.md |
| `COMM_SHMEM_ROUND_MB=1024` | SHMEM pool 45.0 → 9.8 GB per node | mod | results/V114.md |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/B01.md |

Packed 60-bit output `ECALC_OUT_PACKED=2` is the default since 2026-10-12 (TASKS 40; results/C05.md, O4.md, W2A.md).

## Measured and not adopted

| idea | result | label | evidence |
|---|---|---|---|
| `MN_GROUPS=6` instead of `2,6` at 6 nodes | +2.4 s [-3.9, +8.7], p 0.35 | m | results/O1.md |
| uneven node groups | correct, not faster (5 nodes, one run each) | m | results/B06.md |
| B5 product form | +0.08 s [-0.75, +0.91] | m | results/O5.md |
| `RNS_DIST_OVERLAP` (K6; setting removed) | -0.11 ± 0.15 s of 73.4 s | m | results/K6.md |
| `COMM_SHMEM_RPIPE`, `MN_REDIST_FUSE` (both removed) | +1.82 s [-14.35, +17.99], -0.78 s [-40.89, +39.34] | m | results/O2.md |
| init reorderings (K8) | each +2.0 .. +11.5 s | m | results/K8.md |
| `DIST_GATHER_TILE` (removed) | -0.08 s ± 0.88 at 1 node | m | results/S50.md |
| result-exchange rebalancing | about -7 s at 10 nodes, below noise | mod | results/A07.md |
| message coalescing (`DIST_COALESCE`) | 1.0-4.5 s at 576 nodes; idea saved | mod | results/S52.md |
| `ECALC_VMM_BG=2` (removed) | 9/9 reproducer runs failed | m | results/S46.md |
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

### 2026-10-12

- W2A, settings removal wave 2 lane A (A1, A3, A4, A2 merged or gating; D5 packed-output default gating): no timing change at the defaults; `results/W2A.md`.
- W2B, settings removal wave 2 lane B (transport, NTT kernels, distributed products, TCP transport; six launch-line settings built in): digits identical, gates PASS; `results/W2B.md`.
- Three speed settings adopted as defaults (`DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL`): measured at 1 and 2 nodes in `results/CAND.md` (current times in `PLAN.md` §2).
- Binary-limb pipeline removal (decimal limbs only): evidence in the "Adopted changes" row above (`results/H4.md`, `H5.md`); the code is in `archive:binary_pipeline/`.
- P3, init mapping evaluated and not pursued (no plausible saving of 1 s at 1 node): `results/P3.md`.
