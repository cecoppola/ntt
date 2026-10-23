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
Agent reports are merged into topic files in `results/` (lookup by old ID: `results/INDEX.md`; a new report starts as `results/<ID>.md` and is merged into its topic file later); raw data and drivers are in `archive:results_raw_2026-10-19/`; reports that no longer support the current code are in `archive:results/`.

Labels: **m** measured, **mod** modelled, **a** assumed. Times are wall-clock seconds; "digits/node" is the size divided by
the node count. Test machines: aac7 (MI300A nodes with Slingshot, the main test system) and aac6 (single-node coverage).

## Current figures

| what | figure | label | evidence |
|---|---|---|---|
| headline timings (1 node at 6.441 × 10¹⁰ and 10¹¹ digits; 2 / 4 / 6 nodes) | kept in `PLAN.md` §2 (Current status), with their evidence files | m | PLAN.md §2 |
| 4-node run-to-run modes | fast ≈ 181-185 s, slow ≈ 208-214 s: the inter-node all-to-all rate is set per launch | m | results/NODE_SLOWMODE.md §T05, T09.md, A13.md |
| 4-node calibration | total ≈ 80.8 s + 1388 / r (r = all-to-all GB/s per APU thread), residual sd 1.6 s | m/mod | results/TOOLS_PROFILES.md §B07 |
| target: 576 nodes, 3.82 × 10¹³ digits | ≈ 182 s without the write (profile `target_l4`) | mod (fabric a) | results/TIME_MODEL.md §L4, O8.md |
| target device memory, rank 0, 3.82 × 10¹³ | margin 12.9 GB under the 373.44 GB/node edge (launch line with `MN_T_CHUNK_MB=2048`, X2, slot release) | mod | results/LAUNCH_DEFAULTS.md §L1, B01.md, S52.md |
| target device edge | 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread) | m (target) | results/TARGET.md §TGTBENCH2 |
| `BS_ARENA_TRIM_GB=8` (opt-in, default 0), 2 n 1.2882e11 | arena -34.4 GB per node (266.3 -> 231.9); 115.2 s (7 runs) vs 111.4 s (5 runs); 3 n 1.9323e11: 149.4 vs 149.6 s; 0 remaps | m | results/MEMORY.md §ATRIM |
| `comm_ofi` vs SHMEM, 10 nodes, 8.1 × 10¹¹ | 1669.98 s vs 3532.05 s (× 2.1) | m | results/OFI_FABRIC.md §OFI17, STD17.md |
| `COMM_TRANSPORT=xshmem` (then opt-in; the default since Q73, row below; `make XSHMEM=ofi`; target-shaped Cray SHMEM 12.1 style communication emulated over libfabric), 3 n, 1.9323e11, ABBA 17/17 VERIFY OK | libfabric reference 157.3 s (sd 6.8, n 9) vs xshmem (`XSHMEM_ROUND_MB=128`, rot order) 149.3 s (sd 1.2, n 8); paired -8.5 s, CI [-14.0, -3.0], 8/8 pairs; gates 1 n PASS, 2 n and 3 n digits identical with and without rounds, mnaccept `mn` 5/5; whole-exchange staging at 3 n 1.9323e11 fails the memory budget | m | results/XSHMEM.md |
| P5 swap proof: `xshmem_cray.h`, the Cray SHMEM backend of `xshmem.h` (`make XSHMEM=cray`, aac7 libsma 11.8), zero engine changes; 1e9 digits identical at 1, 2 and 3 n, VERIFY OK, `t_xshmem` PASS at 1/2/3 n; first run failed on an unregistered heap tail page, fixed (0dd3627) | m | results/XSHMEM.md |
| Q73 (a) default: `COMM_TRANSPORT=xshmem` with `XSHMEM_ROUND_MB=128` is the default multi-node transport; default build `XSHMEM=ofi` where libfabric and PMI2 are found; gates 1/2/3 n PASS, mnaccept mn 5/5 and unit 9/9; 3 n 1.9323e11 VERIFY OK, default 153.2 s vs explicit shmem 161.3 s (mean of 2 each, paired -8.1 s) | m | results/XSHMEM.md §10 |
| overnight and day-plan runs 2026-10-13/14 (commits 4ae95ec1, 209712f, 1aa2b80; libfabric era): sweeps 60/60, 48/48 and 24/24 digit-identical, gates 1/2/4/6 nodes PASS, 6-node 3.8646e11 repeats mean 190.7 to 202.1 s over 112 runs, 0 hangs; large run 1793.37 s VERIFY OK; fixed: `COMM_OFI=0` pool at 6 nodes (next row), recheck short read (CKPT row) | m (no speed claim) | `archive:results_merged_2026-10-23/NIGHT_RUNS.md` |
| `COMM_OFI=0` pool at 6 nodes (NIGHT bug A): the v-slots now count in the SHMEM pool; 1e10 6-node run rc 3 -> VERIFY OK, digits identical; 4 and 2 nodes OK (pool 2816 -> 3328 MiB at 6 nodes) | 40.28 / 39.20 / 47.66 s total (single runs) | m | results/SHMEM_ITEMS.md §SHM |
| Pre-launch memory fit (D26): the planner reports device/node memory vs the machine limits before every launch and refuses a run that does not fit; `DM_MN_WRAP` automatic (on only with ≥5 GB device margin). Target 3.82e13:576: 358.96 GB device vs 373.44 (wrap off; with wrap 384.73, refused); aac7 6 nodes 374.22 vs 444 (wrap on) | planner values | mod/m | results/LAUNCH_DEFAULTS.md §FIT |
| Node screen NIC check: `tools/nodescreen.sh --want-nics N` (default `$SHMEM_OFI_NUM_NICS`) excludes nodes whose runtime sees fewer cxi NICs; 4-node check: 4 seen, ok at want 4, all flagged at want 8 (positive control) | 4 NICs/node seen | m | results/NODE_SLOWMODE.md §NIC |
| CKPT: the 6-node 3.8646e11 recheck's "short read" on the top set = EIO from the soft-mounted NFS under 6 × 4 reads of 512 MiB (files complete, sizes = headers); reads now ≤ 64 MiB with retries; 2-node write + recheck at 1e10 / 2e10 RECHECK OK, gates 1 / 2 nodes PASS; proof at scale 2026-10-14 (commit 34175cf): 6 nodes 3.8646e11, write 1473.26 s, VERIFY OK, RECHECK OK on all nodes, 0 retries, recheck 7380 s (2 h 3 min, model ≈ 1 h), timeout ≥ 3 h | 12.5 MB/s per node part files, 45 MB/s top set (2 nodes); 6-node recheck 7380 s | m/mod | results/CHECKPOINT.md §CKPT |

## Adopted changes in force (code defaults)

Every change below is digit-identical (VERIFY OK and identical to the reference in its gates). Where a switch still exists,
setting it to 0 restores the old path; a setting named "built in" no longer exists in the code (settings removal, 2026-10-12: `results/CODE_REDUCTION.md §W2A`, `results/CODE_REDUCTION.md §W2B`).

| change | since | effect | label | evidence |
|---|---|---|---|---|
| one single-node division flow, DKM the only division, no 3*2^k planes in the distributed tier, one grid planner | 2026-10-12 | digits identical at all sizes tested; 1e10 at 1 node -8.31 s [-9.68, -6.93] (4 rounds) | m | results/CODE_REDUCTION.md §SIMP |
| shrink pass 3: gen_pack_bench + plain pack kernels, rns_mul_dist_hd, pow_table, part_owner, k_store duplicate, cx_grid (-261 lines net); host tier kept (design in the file) | 2026-10-12 | gates 2 n / 1 n and mnaccept unit,e9,mn all PASS, digits identical | `results/CODE_REDUCTION.md §SH3` |
| decimal limbs 10¹⁸ only; binary pipeline removed | 2026-10-12 | binary was 1.9-2.2 × slower at 2 nodes (+32.47 s at 10¹⁰) | m | results/CODE_REDUCTION.md §H4, H5.md, H1.md, H2.md |
| `DBIG_SMALL_BOUNCE=1`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL=1` | 2026-10-12 | -7.16 s [-8.08, -6.23] 1 node (aac6); -5.58 s [-10.54, -0.63] 2 nodes (aac7); reciprocal chain 4.57 → 0.06 s | m | results/SINGLE_NODE.md §CAND, O2.md, K1b.md, K8b.md |
| kernels K2, K3 (`DBIG_ADDSUB3`, `DBIG_MODQ_MONT`; built in) | 2026-10-11 | 1 node -2.03 s ± 1.38 (aac7) | m | results/KERNELS.md §S51, T08.md |
| `FI_MR_CACHE_MONITOR=userfaultfd` set by `mnrun.sh`; `disabled` refused | 2026-10-11 | reproducer hangs 22/105 → 0/104; 46 four-node runs, 0 segfaults, 0 hangs | m | results/CRASHES_HANGS.md §S46, T12.md |
| kernels `DBIG_ADDSUB2`, `DBIG_MAXIDX_TOP`, `DBIG_QSEL` (built in) | 2026-10-10 | together -50.0 s at 4 nodes [-71.7, -28.3] | m | results/KERNELS.md §S38, S40.md, S41.md, S43.md |
| `DIST_CHUNKS=8` (was 4) | 2026-10-07 | device -13.4 GB at 576 nodes for +2.4 s | mod | results/GRID_GROUPS.md §S45 |
| exchange slots in the node budget (built in; the `ECALC_VSLOT_BUDGET` switch is gone) | 2026-10-07 | no measured cost | m | results/GRID_GROUPS.md §B7ACCT, S18M.md |
| `COMM_OFI=1` where a cxi NIC is present (data plane of the `COMM_TRANSPORT=shmem` fallback only; the default path is xshmem, Q73) | 2026-10-06 | × 2.1 over SHMEM at 10 nodes | m | results/OFI_FABRIC.md §OFI17, STD17.md |
| `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` set by `mnrun.sh` when unset | 2026-10-06 | fixes multi-process `fi_enable(-28)`; log volume / 1000 | m (target) | results/TARGET.md §TGTBENCH2 |
| transport opened before the arenas, self-test growth (`COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1`; built in) | 2026-10-05 | transport opened before the arenas (init segfault 8/78 before); ≥ 32 processes start | m | results/CRASHES_HANGS.md §S16, P16.md |
| division in two quotient halves (built in; the `NEWTON_DKM` switch is gone) | Phase 15 | division -25.7 s at 10¹¹ | m | results/DIVISION.md §DKM15 |
| 24 digits per transform point in multi-node products (built in; the `MN_P24` switch is gone) | Phase 15 | -38 % at 10¹⁰ on 2 processes | m | results/PRIMES_STRATEGY.md §P2415 |
| `ECALC_CORR_PATCH=2`, `MN_OUT_EARLY=1`: write overlapped with the division, corrections patched | Phase 15 | write hidden under the division | m | results/OUTPUT_IO.md §IO15, IO215.md |
| `ECALC_OUT_PACKED=2`: packed output, 60-bit, 0.4167 B/digit (=1, 0.444 B/digit, until 2026-10-12) | Phase 15; 2 since 2026-10-12 | ASCII made off the clock by `tools/unpack_digits` | m | results/TESTING_VERIF.md §W, results/OUTPUT_IO.md §O4 |
| `BS_BATCH_PIPE=1`: the batch tier's host side in parallel, tiles back to back (user decision D20; `=0` fallback) | 2026-10-12 | 1 n −0.99 s [−1.18, −0.80], 4 ABBA rounds, digits identical | m | results/KERNELS.md §N3 |
| `RNS_CRT_SCAN=1`: `k_crt_batch` carry-lookahead (user decision D21; `=0` fallback) | 2026-10-12 | 1 n 6.441e10 −2.52 s [−4.19, −0.85], 4 ABBA rounds, digits identical | m | results/KERNELS.md §K4b |
| `P24_CRT_SCAN=1`: `k_crt24` (the P24 CRT) carry-lookahead (user decision D24; `=0` fallback) | 2026-10-13 | kernel 1.55x; 2 n 6.441e10 per node total -1.89 s [-7.51, +3.73] n.s., 4 ABBA rounds, digits identical | m | results/KERNELS.md §K4c |
| `NEWTON_CHAIN_GUARD=1`: the reciprocal's chain keeps a guard limb per doubling (user decision D25; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-13 | removes a rare fatal (>64 corrections after runs of exact doublings); SIMP Q error -22 -> 0 units; digits identical | m | results/DIVISION.md §RECIP |
| `DM_MN_WRAP=1`: the division's low product X·Q as a low part + one cyclic plane (user decision D22; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-12 | 6 n (P24) dm −6.25 s [−11.47, −1.03], total −2.86 s n.s.; +5.4 GB per APU | m | results/DIVISION.md §P1, DEF.md |
| `BS_SEED_FILL=128` | Phase 15 | the batch tier's products fill their lengths | m | results/SINGLE_NODE.md §T215 |
| transform cache sized and counted (built in; the `RNS_DIST_CACHE_FIT` switch is gone) | Phase 15 | required at the target | m | results/CACHE.md §TC15 |
| `DB_POOL_VMM=1`: arenas as HIP virtual memory | Phase 14 | no in-phase allocation | m | results/DIVISION.md §R114 |
| `MN_T_CHUNK_MB=1024` code default | Phase 14 | pool 81.6 → 45.0 GB per node | mod | results/SHMEM_ITEMS.md §P214 |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/MEMORY.md §L114 |
| `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, three primes at size 1 | Phase 13 | three primes: -17 % wall, -25.8 GB | m | results/PRIMES_STRATEGY.md §B13b, X13b.md, archive RESULTS §78-80 |
| two-deep v-exchange (built in; the `COMM_ALLTOALLV_DEPTH` switch is gone) | Phase 13 | xGMI stage hidden under the fabric (1.4 → 74 %) | m | results/EXCHANGE.md §X13 |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/TESTING_VERIF.md §V |

## Launch-line settings in force (not code defaults)

On both launch lines (`profiles/aac7.conf` (`LAUNCH_LINE_KEYS`) for aac7, `profiles/target.conf.template` for the target). Rows marked
"code default" were moved off the lines into the code on 2026-10-12 (TASKS 27, `results/CODE_REDUCTION.md §W2B`; each keeps a `=0` fallback).

| setting | effect | label | evidence |
|---|---|---|---|
| `ECALC_NP=auto` (4 primes above the three-prime bound) | needed at the target size | mod | results/PRIMES_STRATEGY.md §NP15, P15.md |
| `MN_OUT_DKM_HI=1`, `RNS_DIST_CACHE_PARTIAL=1` (code default) | write starts after division step 1; cache from free pool bytes, -12.8 s at 5.1 × 10¹³ | mod | results/CACHE.md §TC15, IO215.md |
| lean division (built in; the `DM_MN_LEAN` switch is gone) | same digits, less division memory | m | archive RESULTS §104 |
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]) | m | results/SESSIONS.md §S22, S47.md |
| X2: `COMM_LAYER_INTER2=1`, `COMM_LAYER_VSLOT_POOL=1` (code default) | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/SESSIONS.md §S31 |
| aac7 Cray SHMEM 11.8 tuning phase closed (2026-10-21): the 3 n gap was waiting in the layered variable-count exchange (SHMEM 157.0 vs libfabric 143.0 s, 6 pairs); 21 environment settings, DEVHEAP2, DEVSRC, zero-copy and NUMA flush gave nothing | m | results/SHMEM.md |
| `COMM_LAYER_VSLOT_SHARE=1` (always on; switch removed 2026-10-19, task 4) | -8.4 GB per node at 4 nodes; -5.79 GB at 576 | m / mod | results/GRID_GROUPS.md §S36, S45.md |
| `COMM_LAYER_VSLOT_RELEASE=1` (always on with the pool; switch removed 2026-10-19, task 4) | device peak -8.60 GB at 4 nodes; -8.59 GB at 576 | m / mod | results/GRID_GROUPS.md §C06, L1.md |
| `COMM_SHMEM_ROUND_MB=128` (fallback transport only; was 1024, Q60; `XSHMEM_ROUND_MB=128` is the default engine value) | SHMEM pool 45.0 → 9.8 GB per node at 1024; host staging 10.2 → 3.1 GB per node at 2 n at 128 | mod / m | results/SHMEM_ITEMS.md §V114, profiles/aac7.conf |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/LAUNCH_DEFAULTS.md §B01 |

Packed 60-bit output `ECALC_OUT_PACKED=2` is the default since 2026-10-12 (TASKS 40; results/OUTPUT_IO.md §C05, O4.md, W2A.md).

## Measured and not adopted

| idea | result | label | evidence |
|---|---|---|---|
| `MN_GROUPS=6` instead of `2,6` at 6 nodes | +2.4 s [-3.9, +8.7], p 0.35 | m | results/GRID_GROUPS.md §O1 |
| the SHMEM proxy mode (N7-N7G: shifted peer order, `PE_PER_APU` 1 and 2, `PX_THP`/`PX_PIN`, `PX_NUMA`, `PX_KCOPY`, `PX_INIT`, `PX_SEND` 1-4 with PX3, `PX_SCHED`), measured 2026-10-13 to 19, removed in P7 (Q78) | best 1.43× comm_ofi at 6 n (3 pairs), 1.68× over 28 pairs; one proxy PE per APU 2.9× slower (656.7 vs 224.0 s); SHMEM 286.0 s mean over 24 runs against libfabric 184.5 s before the heap fix; no switch closed the gap | m | results/SHMEM.md |
| SHMEM compaction stalls (3 n, 4.8e10, items 97a-97t, 2026-10-18): proactive compaction on the 4 KiB node-process heap evicted the GPU queues; `COMM_SHMEM_NP_HEAP_THP=1` (default, D48): 0 stalls in 16 runs (plus 6 at 2 n) vs 9 in 24 without; SHMEM about 6 % behind libfabric (49.6 vs 46.6 s); full account in results/SHMEM.md (the report is archived) | stalls 0/16 vs 9/24 | m | results/SHMEM.md |
| SHMEM `COMM_LAYER_INTER2` default (item 9m5, 3 n 1.9323e11, 14 runs VERIFY OK): SHMEM two in flight 149.9 s (n 5, sd 2.1) vs one in flight 143.2 s (n 3, sd 5.5); libfabric 140.2 (n 3), +INTER2=0 137.2 (n 3); layv wait 264-266 s vs 183 s (m); user adopted unset = 0 on both transports (2026-10-21); libfabric with one in flight is the reference speed | no 6 n measurement yet | `results/SHMEM.md` |
| SHMEM against libfabric (aac7, proxy era, 2026-10-18/19): 4 n 6.4e10 +2.2 % (54.34 vs 53.18 s); by off-node peers 2 n +0.8 %, 4 n about +7 %, 6 n about +9 % with chunked puts; the excess is per byte and mostly waiting; 2 n warm init 23.65 s | gap grows with the peers | m | results/SHMEM.md |
| uneven node groups | correct, not faster (5 nodes, one run each) | m | results/GRID_GROUPS.md §B06 |
| B5 product form | +0.08 s [-0.75, +0.91] | m | results/DIVISION.md §O5 |
| `RNS_DIST_OVERLAP` (K6; setting removed) | -0.11 ± 0.15 s of 73.4 s | m | results/EXCHANGE.md §K6 |
| `COMM_SHMEM_RPIPE`, `MN_REDIST_FUSE` (both removed) | +1.82 s [-14.35, +17.99], -0.78 s [-40.89, +39.34] | m | results/DIVISION.md §O2 |
| init reorderings (K8) | each +2.0 .. +11.5 s | m | results/MEMORY.md §K8 |
| `DIST_GATHER_TILE` (removed) | -0.08 s ± 0.88 at 1 node | m | results/EXCHANGE.md §S50 |
| result-exchange rebalancing | about -7 s at 10 nodes, below noise | mod | results/EXCHANGE.md §A07 |
| message coalescing (`DIST_COALESCE`) | 1.0-4.5 s at 576 nodes; idea saved | mod | results/GRID_GROUPS.md §S52 |
| `ECALC_VMM_BG=2` (removed) | 9/9 reproducer runs failed | m | results/CRASHES_HANGS.md §S46 |
| 4 processes per node (one per APU) | 3.7 × slower at 8 nodes | m | archive RESULTS §101 |
| `hipMallocManaged` host spill at the device edge | host memory 21 GB/s vs HBM 3.2-3.8 TB/s | m | results/TARGET.md §TGTBENCH2 |

## Open measurement questions

- Fabric injection rate per NIC on the target and its fall-off at 576 nodes (A3, A4): every target time above uses an
  assumed rate. The target kit `ecalc/target_kit.sh` measures both (results/TOOLS_PROFILES.md §S18KIT, A06.md).
- `MN_T_CHUNK_MB` 1024 vs 2048 and X2 on the target: not yet measured there.
- The cause of the 4-node slow mode (per-launch NIC group) is not established (results/NODE_SLOWMODE.md §A14).
- Cray SHMEM path against libfabric: +7.5 % at 3 n, 1e11 (CI +3.4 to +11.5 %), the whole gap is init (+8 s, aac7-specific); to be re-measured on the target with Cray SHMEM 12.1 (results/XSHMEM.md craylane; TASKS Proposed).

## New entries

Add one short section per merged experiment below this line: date, what changed, the numbers with labels, and the
`results/<ID>.md` file.

### 2026-10-12

- W2A, settings removal wave 2 lane A (A1, A3, A4, A2 merged or gating; D5 packed-output default gating): no timing change at the defaults; `results/CODE_REDUCTION.md §W2A`.
- W2B, settings removal wave 2 lane B (transport, NTT kernels, distributed products, TCP transport; six launch-line settings built in): digits identical, gates PASS; `results/CODE_REDUCTION.md §W2B`.
- Three speed settings adopted as defaults (`DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL`): measured at 1 and 2 nodes in `results/SINGLE_NODE.md §CAND` (current times in `PLAN.md` §2).
- Binary-limb pipeline removal (decimal limbs only): evidence in the "Adopted changes" row above (`results/CODE_REDUCTION.md §H4`, `H5.md`); the code is in `archive:binary_pipeline/`.
- P3, init mapping evaluated and not pursued (no plausible saving of 1 s at 1 node): `results/MEMORY.md §P3`.

### 2026-10-14

- ROCM, ROCm 7.14.0 against 7.2.4 at 1 node, 1e10 digits: digits identical, B - A +4.74 s [-17.02, +26.49] (4 pairs; -2.01 s [-8.62, +4.59] without the cold first run) (m); first 7.14.0 launch is slow (cold start); `results/AAC7_PORT.md §ROCM`.
- SCALE35, scaling at 3 and 5 nodes (6.441e10 digits per node): 147.7 s and 182.1 s (mean of 3, VERIFY OK) (m); `results/TIME_MODEL.md §SCALE35`.

### 2026-10-21

- XSHMEM, target-shaped communication (plan TARGET1): `xshmem.h` with a libfabric backend (aac7) and a Cray SHMEM forwarding header (target), the engine `COMM_TRANSPORT=xshmem` (opt-in then; the default since Q73): digits identical at 1, 2 and 3 n; 3 n 1.9323e11 149.3 s against the reference 157.3 s, paired -8.5 s [-14.0, -3.0] (m); adoption and the open items are the user's; `results/XSHMEM.md`.
- PPG, one process per GPU (plan TARGET1 stage S2, `ECALC_PE_PER_GPU`, off by default; xshmem K=1): gate 9v8 on aac7 (3 nodes): component tests and off path PASS, digits identical off vs on; single runs off / on: 1 n 1e9 9.73 / 8.94 s, 3 n 1e9 14.27 / 5.42 s, 3 n 1e10 32.83 / 24.68 s (m, one run each, not a speed claim; S2d measured 2026-10-22, see the PPG section of results/XSHMEM.md); the vPE stages S1b-S1e are carried, thread-mode full runs abort (`stages e not merged`); `results/XSHMEM.md §PPG`.
- BSPLIT, own-device split of oversized batch products under P (`RNS_BATCH_SPLIT`, off by default): unit test and planner checks PASS (measured), PG 3 n 1.9323e11 cap 2^30 plans as FITS with the switch; digit gates 1 n 3e10, 2 n 1e11, 3 n 1.9323e11 identical to D (measured): results/XSHMEM.md §BSPLIT

### 2026-10-22 and 2026-10-23

- Q73 xshmem default (2026-10-21), P7 proxy-mode removal (Q78, 2026-10-22), NUMA placement fixing the Cray stalls and the final Cray-vs-libfabric figure: path 2 is +7.5 % against path 1 at 3 n 1e11 (11 interleaved pairs, CI +3.4 to +11.5 %), the gap is init (+8 s) (m); both paths are supported (Q85); `README.md`, `results/XSHMEM.md` (craylane).
