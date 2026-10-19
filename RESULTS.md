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
| overnight 2026-10-13 (commit 4ae95ec1): sweep 60/60 and fallbacks 17/18 digit-identical; 6-node repeat 202.1 s mean; open bugs: `COMM_OFI=0` at 6 nodes (pool), large run timed out on the slow file system | m | results/NIGHT.md |
| `COMM_OFI=0` pool at 6 nodes (NIGHT bug A): the v-slots now count in the SHMEM pool; 1e10 6-node run rc 3 -> VERIFY OK, digits identical; 4 and 2 nodes OK (pool 2816 -> 3328 MiB at 6 nodes) | 40.28 / 39.20 / 47.66 s total (single runs) | m | results/SHM.md |
| Pre-launch memory fit (D26): the planner reports device/node memory vs the machine limits before every launch and refuses a run that does not fit; `DM_MN_WRAP` automatic (on only with ≥5 GB device margin). Target 3.82e13:576: 358.96 GB device vs 373.44 (wrap off; with wrap 384.73, refused); aac7 6 nodes 374.22 vs 444 (wrap on) | planner values | mod/m | results/FIT.md |
| day plan 2026-10-13 (commit 209712f): large run 3.8646e11 digits, 6 nodes, VERIFY OK, first 1e11 digits identical (recheck not run, follow-up); sweep 48/48 and SHMEM sweep 21/24 digit-identical (3 node faults); gates 1/2/4/6 nodes PASS; 6-node repeat 197.1 s mean over 34 runs (m, no speed claim); D27 (a) | 1793.37 s large run; 197.06 s repeat mean (m) | m | results/DAY.md |
| Node screen NIC check: `tools/nodescreen.sh --want-nics N` (default `$SHMEM_OFI_NUM_NICS`) excludes nodes whose runtime sees fewer cxi NICs; 4-node check: 4 seen, ok at want 4, all flagged at want 8 (positive control) | 4 NICs/node seen | m | results/NIC.md |
| CKPT: the 6-node 3.8646e11 recheck's "short read" on the top set = EIO from the soft-mounted NFS under 6 × 4 reads of 512 MiB (files complete, sizes = headers); reads now ≤ 64 MiB with retries; 2-node write + recheck at 1e10 / 2e10 RECHECK OK, gates 1 / 2 nodes PASS; proof at scale 2026-10-14 (commit 34175cf): 6 nodes 3.8646e11, write 1473.26 s, VERIFY OK, RECHECK OK on all nodes, 0 retries, recheck 7380 s (2 h 3 min, model ≈ 1 h), timeout ≥ 3 h | 12.5 MB/s per node part files, 45 MB/s top set (2 nodes); 6-node recheck 7380 s | m/mod | results/CKPT.md |

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
| exchange slots in the node budget (built in; the `ECALC_VSLOT_BUDGET` switch is gone) | 2026-10-07 | no measured cost | m | results/B7ACCT.md, S18M.md |
| `COMM_OFI=1` where a cxi NIC is present | 2026-10-06 | × 2.1 over SHMEM at 10 nodes | m | results/OFI17.md, STD17.md |
| `FI_UNIVERSE_SIZE`, `FI_LOG_LEVEL=warn` set by `mnrun.sh` when unset | 2026-10-06 | fixes multi-process `fi_enable(-28)`; log volume / 1000 | m (target) | results/TGTBENCH2.md |
| transport opened before the arenas, self-test growth (`COMM_INIT_EARLY=1`, `MN_SELFTEST_GROW=1`; built in) | 2026-10-05 | transport opened before the arenas (init segfault 8/78 before); ≥ 32 processes start | m | results/S16.md, P16.md |
| division in two quotient halves (built in; the `NEWTON_DKM` switch is gone) | Phase 15 | division -25.7 s at 10¹¹ | m | results/DKM15.md |
| 24 digits per transform point in multi-node products (built in; the `MN_P24` switch is gone) | Phase 15 | -38 % at 10¹⁰ on 2 processes | m | results/P2415.md |
| `ECALC_CORR_PATCH=2`, `MN_OUT_EARLY=1`: write overlapped with the division, corrections patched | Phase 15 | write hidden under the division | m | results/IO15.md, IO215.md |
| `ECALC_OUT_PACKED=2`: packed output, 60-bit, 0.4167 B/digit (=1, 0.444 B/digit, until 2026-10-12) | Phase 15; 2 since 2026-10-12 | ASCII made off the clock by `tools/unpack_digits` | m | results/W.md, results/O4.md |
| `BS_BATCH_PIPE=1`: the batch tier's host side in parallel, tiles back to back (user decision D20; `=0` fallback) | 2026-10-12 | 1 n −0.99 s [−1.18, −0.80], 4 ABBA rounds, digits identical | m | results/N3.md |
| `RNS_CRT_SCAN=1`: `k_crt_batch` carry-lookahead (user decision D21; `=0` fallback) | 2026-10-12 | 1 n 6.441e10 −2.52 s [−4.19, −0.85], 4 ABBA rounds, digits identical | m | results/K4b.md |
| `P24_CRT_SCAN=1`: `k_crt24` (the P24 CRT) carry-lookahead (user decision D24; `=0` fallback) | 2026-10-13 | kernel 1.55x; 2 n 6.441e10 per node total -1.89 s [-7.51, +3.73] n.s., 4 ABBA rounds, digits identical | m | results/K4c.md |
| `NEWTON_CHAIN_GUARD=1`: the reciprocal's chain keeps a guard limb per doubling (user decision D25; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-13 | removes a rare fatal (>64 corrections after runs of exact doublings); SIMP Q error -22 -> 0 units; digits identical | m | results/RECIP.md |
| `DM_MN_WRAP=1`: the division's low product X·Q as a low part + one cyclic plane (user decision D22; `=0` fallback; counted by `dm_layout` and `mem_model.py`) | 2026-10-12 | 6 n (P24) dm −6.25 s [−11.47, −1.03], total −2.86 s n.s.; +5.4 GB per APU | m | results/P1.md, DEF.md |
| `BS_SEED_FILL=128` | Phase 15 | the batch tier's products fill their lengths | m | results/T215.md |
| transform cache sized and counted (built in; the `RNS_DIST_CACHE_FIT` switch is gone) | Phase 15 | required at the target | m | results/TC15.md |
| `DB_POOL_VMM=1`: arenas as HIP virtual memory | Phase 14 | no in-phase allocation | m | results/R114.md |
| `MN_T_CHUNK_MB=1024` code default | Phase 14 | pool 81.6 → 45.0 GB per node | mod | results/P214.md |
| `ECALC_BUDGET_CHECK=1`, `ECALC_MEM_GUARD_GB` | Phase 14 | named stop (rc 8 / rc 9) instead of the OOM killer | m | results/L114.md |
| `RNS_STRATEGY=auto`, `ECALC_PLANE_CAP=2^31`, three primes at size 1 | Phase 13 | three primes: -17 % wall, -25.8 GB | m | results/B13b.md, X13b.md, archive RESULTS §78-80 |
| two-deep v-exchange (built in; the `COMM_ALLTOALLV_DEPTH` switch is gone) | Phase 13 | xGMI stage hidden under the fabric (1.4 → 74 %) | m | results/X13.md |
| T1 moduli: the first eight primes above 2⁶² | Phase 11 | earlier moduli were composite | m | results/V.md |

## Launch-line settings in force (not code defaults)

On both launch lines (`profiles/aac7.conf` (`LAUNCH_LINE_KEYS`) for aac7, `profiles/target.conf.template` for the target). Rows marked
"code default" were moved off the lines into the code on 2026-10-12 (TASKS 27, `results/W2B.md`; each keeps a `=0` fallback).

| setting | effect | label | evidence |
|---|---|---|---|
| `ECALC_NP=auto` (4 primes above the three-prime bound) | needed at the target size | mod | results/NP15.md, P15.md |
| `MN_OUT_DKM_HI=1`, `RNS_DIST_CACHE_PARTIAL=1` (code default) | write starts after division step 1; cache from free pool bytes, -12.8 s at 5.1 × 10¹³ | mod | results/TC15.md, IO215.md |
| lean division (built in; the `DM_MN_LEAN` switch is gone) | same digits, less division memory | m | archive RESULTS §104 |
| `MN_T_CHUNK_MB=2048` | aac7: 24 paired rounds, mean -42.3 s, p ≈ 0.04; 4-node recheck inconclusive (+6.98 s [-10.3, +24.3]) | m | results/S22.md, S47.md |
| X2: `COMM_LAYER_INTER2=1`, `COMM_LAYER_VSLOT_POOL=1` (code default) | -31.7 s at 10 nodes (CI -55.7 .. -7.8) | m | results/S31.md |
| `COMM_LAYER_VSLOT_SHARE=1` (always on; switch removed 2026-10-19, task 4) | -8.4 GB per node at 4 nodes; -5.79 GB at 576 | m / mod | results/S36.md, S45.md |
| `COMM_LAYER_VSLOT_RELEASE=1` (always on with the pool; switch removed 2026-10-19, task 4) | device peak -8.60 GB at 4 nodes; -8.59 GB at 576 | m / mod | results/C06.md, L1.md |
| `COMM_SHMEM_ROUND_MB=1024` | SHMEM pool 45.0 → 9.8 GB per node | mod | results/V114.md |
| `ECALC_SEGV_TRACE=1` | crash-trace handler only | - | results/B01.md |

Packed 60-bit output `ECALC_OUT_PACKED=2` is the default since 2026-10-12 (TASKS 40; results/C05.md, O4.md, W2A.md).

## Measured and not adopted

| idea | result | label | evidence |
|---|---|---|---|
| `MN_GROUPS=6` instead of `2,6` at 6 nodes | +2.4 s [-3.9, +8.7], p 0.35 | m | results/O1.md |
| `COMM_SHMEM_PEER_ORDER=shift` (N7 Fix A; SHMEM path, `COMM_OFI=0`; default stays `rank`): shifted-ring peer order | t_comm at 6 n: 12.21 vs 10.08 GB/s per node, +21 % (n = 2 per arm); full run at 6 n: B − A +130 s, 95 % CI [−976, +1237] over 3 pairs, pairs 2–3 −115 and −138 s; not adopted, needs more pairs | m | results/N7.md |
| `COMM_SHMEM_PE_PER_APU=1` (N7b; one SHMEM proxy PE per APU, `COMM_OFI=0`; default stays 0): bandwidth, digits, 6-node full run | t_comm at 6 n: 28.8-29.8 GB/s per node (SHMEM today 8.4, comm_ofi 30.8-39.7); digits identical at 1, 2 and 6 nodes; full run at 6 n, 3.8646e11 digits: B − A +432.7 s, 95 % CI [+174.3, +691.1] over 4 pairs (224.0 vs 656.7 s, 2.9 × slower than comm_ofi); not adopted | m | results/N7B.md |
| N7E paired repeat on the proxy mode with `COMM_SHMEM_PX_SEND=2` and `COMM_SHMEM_PX_INIT=1` (defaults unchanged): 6 n, 3.8646e11 digits, 83/83 runs ok; B − A +129.3 s, 95 % CI [+92.9, +165.7] over 28 pairs (348.3 vs 191.0 s, 1.68 × comm_ofi); the N7D gap closure (1.43 ×) is not reproduced; PX_SEND 2 vs 1 +69.9 s [−217.8, +357.6] (4 pairs); PX_INIT 1 vs 0 +107.2 s [−50.5, +264.9] (3 pairs), init −1.3 s [−2.7, +0.1] | m | results/N7E.md |
| `COMM_SHMEM_PE_PER_APU=2` (N7E-V; two helper PEs per APU, `COMM_OFI=0`; default stays 1 under `COMM_OFI=0`): gates, 1e10 digits | gates 1 and 2 n PASS, 1e10 at 2 n identical, no `/dev/shm` files left (m); the first 2-node gate failed on an unaligned pool offset, fixed with `shmem_align`; no speed claim (one NIC per APU on aac7) | results/N7E_V.md |
| `COMM_SHMEM_PX_THP=1` + `COMM_SHMEM_PX_PIN=1` (N7G; proxy mode, default 0): 3 interleaved pairs at 6 n, 3.8646e11 digits: B − A −124.9 s, 95 % CI [−460.6, +210.8], n = 3 (A 432.1 s, B 307.2 s, m); not significant, defaults stay 0; helper pools are 4 KB pages, shared-memory huge pages unavailable, `MADV_COLLAPSE` 0.29 s/GiB (m); item 37, THP only, 4 interleaved pairs at 3.8646e11 digits: A 576.5 s (sd 155.5), B 288.8 s (sd 63.0), B − A −287.8 s, 95 % CI [−507, −68], B faster in 4/4 (m), gates PASS, default stays 0 pending D36 | 3 pairs not significant; item 37 4 pairs significant (THP only), decision D36 open | `results/N7G.md` |
| N7G item 38: default flip (D36 a), gates PASS at 1 and 2 nodes, 6-node 1e10 compare identical; 2 pairs at 3.8646e11: default A 237.4 s and 228.8 s, THP=0 B 337.3 s and 544.3 s, B - A +207.7 s (n 2, not significant alone; direction as item 37) (m) | default THP=1 in proxy mode, merged | `results/N7G.md` |
| SHMEM proxy mode vs libfabric at 6 n, 3.8646e11 (items 45-55): libfabric 184.5 s (n 4, sd 6.4); SHMEM default 286.0 s over 24 runs (sd 50.1, 212-398), best 4 220.5 s: steady gap about 36 s plus about 65 s run-to-run variance on average (m); slow runs lose their time in node-local steps; no switch closes either part (huge-page default D36 kept; LAYV_ANY and PHASE_TS merged default 0) | open: phase B (variance) and C (steady gap) | `results/SHMEM.md` |
| SHMEM compaction stalls (3 n, 4.8e10, items 97a-97t, 2026-10-18): proactive compaction on the 4 KiB node-process heap evicted the GPU queues; `COMM_SHMEM_NP_HEAP_THP=1` (default, D48): 0 stalls in 16 runs (plus 6 at 2 n) vs 9 in 24 without; SHMEM about 6 % behind libfabric (49.6 vs 46.6 s); full account in results/SHMEM_STALLS.md | stalls 0/16 vs 9/24 | m | results/SHMEM.md |
| `COMM_SHMEM_PX_NUMA=1` (N7C; proxy heaps on the NIC's NUMA node, default 0) with `HSA_ENABLE_SDMA=0` (environment, not on any launch line) on the proxy mode: digits, gates, 6-node full run | gates 1 and 2 nodes PASS (config C and defaults), 1e10 identical at 2 and 6 nodes; 2 n: exchanges at comm_ofi speed (57.1 vs 55.1 s); 6 n, 3.8646e11 digits: B − A +242.4 s, 95 % CI [+127.1, +357.7] over 4 pairs (216.3 vs 458.7 s, 2.1 × slower than comm_ofi; was +432.7 s); not adopted | m | results/N7C.md |
| N7D on the proxy mode: `COMM_SHMEM_PX_KCOPY=1` (copy kernel instead of `HSA_ENABLE_SDMA=0`), `COMM_SHMEM_PX_INIT=1` (parallel pre-fault and registration), `COMM_SHMEM_PX_SEND=2` (put-with-signal chunks, a context per peer); all default 0 | gates 1/2/6 nodes PASS, 1e10 identical at 2 and 6 n; 2 n: KCOPY − SDMA-off −0.66 s [−16.6, +15.3] (2 pairs); 6 n, 3.8646e11, all three + PX_NUMA (+ v-slots off the pool): B − A +83.2 s, 95 % CI [+37.7, +128.7] over 3 pairs (191.6 vs 274.8 s, 1.43 × comm_ofi; was +242.4 s); not adopted as the default transport; D31: under `COMM_OFI=0` the proxy mode with PX_NUMA and PX_KCOPY is the default (gates 1/2 n PASS, 1e10 identical) | m | results/N7D.md |
| overnight 2026-10-14 (commit 1aa2b80): gates 1/2/4/6 nodes PASS; 6-node repeat 31 runs, 0 hangs, mean 190.70 s (sd 12.03, min 175.38, max 217.78); SHMEM-path sweep 24/24 digits identical at 1–6 nodes; nodes drained after our jobs ended (3 cases, cause unknown, site-side) | m (no speed claim) | results/NIGHT2.md |
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
- SHMEM-only path at 6 nodes: 3.0 × `comm_ofi` (698.5 s against 230.5 s, m) with the best SHMEM settings (4 NICs, 256 MiB
  rounds). One PE per node uses one of four NICs (vendor limit); the one-PE-per-APU design (N7b, D29) reaches 28.8-29.8 GB/s per node in t_comm but its 6-node full run is 2.9 × slower than `comm_ofi` (656.7 vs 224.0 s, m; results/N7B.md).

## New entries

Add one short section per merged experiment below this line: date, what changed, the numbers with labels, and the
`results/<ID>.md` file.

### 2026-10-12

- W2A, settings removal wave 2 lane A (A1, A3, A4, A2 merged or gating; D5 packed-output default gating): no timing change at the defaults; `results/W2A.md`.
- W2B, settings removal wave 2 lane B (transport, NTT kernels, distributed products, TCP transport; six launch-line settings built in): digits identical, gates PASS; `results/W2B.md`.
- Three speed settings adopted as defaults (`DBIG_SMALL_BOUNCE`, `DIST_GATHER_RAW=2`, `BS_SEED_PULL`): measured at 1 and 2 nodes in `results/CAND.md` (current times in `PLAN.md` §2).
- Binary-limb pipeline removal (decimal limbs only): evidence in the "Adopted changes" row above (`results/H4.md`, `H5.md`); the code is in `archive:binary_pipeline/`.
- P3, init mapping evaluated and not pursued (no plausible saving of 1 s at 1 node): `results/P3.md`.

### 2026-10-14

- ROCM, ROCm 7.14.0 against 7.2.4 at 1 node, 1e10 digits: digits identical, B - A +4.74 s [-17.02, +26.49] (4 pairs; -2.01 s [-8.62, +4.59] without the cold first run) (m); first 7.14.0 launch is slow (cold start); `results/ROCM.md`.
- SCALE35, scaling at 3 and 5 nodes (6.441e10 digits per node): 147.7 s and 182.1 s (mean of 3, VERIFY OK) (m); `results/SCALE35.md`.
