# TARGET — the target (digits and 576 nodes)

Owner of items TGT, TGT17, TGTBENCH2, CAP17, SC15, MS15, DOC15, L13d, G13d, S13d (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`). Raw data and drivers: `archive:results_raw_2026-10-19/`; originals of this file: `archive:results_merged_2026-10-19/`. Labels: m measured, mod modelled, a assumed (u = user-supplied). Times Eastern.

## Topic summary

- **The target now:** 3.82 × 10¹³ digits on 576 nodes (4 MI300A each, 2304 APUs), `PLAN.md` §1; standing estimate about 182-185.5 s without the write (mod, profile `target_l4`, fabric assumed: `results/TIME_MODEL.md §L4`). The target moved over time and most sections here are history at the older sizes: 4.25 × 10¹³ (Phase 14), 5.1 × 10¹³ (2026-09-27; section removed 2026-10-25, `archive:results_merged_2026-10-25/TARGET_removed.md`), 5.276 × 10¹³ (earlier headline), 4.08 × 10¹³ (TGT17, 2026-10-06), 3.76 / 3.71 × 10¹³ proposed (TGTBENCH2), then 3.82 × 10¹³ (user, current). Figures at the older sizes are superseded; keep them only as method and as the step structure.
- **Device-memory edge (live):** `hipMalloc` stops at 93.36 GB per APU = 373.44 GB per node (A6, m on the target, n = 5, 0 % spread); VMM shares the edge (aac7 444.0 vs hipMalloc about 448.0 GB, `results/LAUNCH_DEFAULTS.md §FIT`). This is `ECALC_DEVICE_GB` (default 373.44). The binding bound is the device layout (planes + arena + comm_ofi pools + v-slots), not the 480 GB node budget. Current fit at 3.82 × 10¹³: device 358.96 GB with the wrap off, margin +14.48 GB (planner, mod; `results/LAUNCH_DEFAULTS.md §FIT`); with slot release the rank-0 margin was 12.9 GB (`§L1` there).
- **Layout structure (live):** device and piece counts are tiered, not smooth: the arena steps by one 4.29 GB chunk per node at grid steps and the critical path steps by pieces. Always size by `BS_LAYOUT_ONLY` / `MN_PLAN_ONLY` (login node, no HIP) and `mem_model.py --check-c` (exact, 0.0000 % at every point tested here); `mem_model.dev_max` omits the comm_ofi pools (C `binsplit_fit` is the source of truth). Trap: without `COMM_TRANSPORT=shmem` in the environment the `room:` line silently omits the SHMEM and comm_ofi pools (9.66 GB short, still prints `fits 1`).
- **Launch-environment facts (TGT, TGTBENCH2):** user-supplied hardware: 8 Slingshot NICs per node at 400 Gb/s (2 per APU, 100 GB/s injection peak per APU), Rocky 9.6, Slurm 25.11.8, ROCm 7.14.0, cray-shmem 12.1.0, CCE 21.0.0, libfabric 2.3.1, file system about 1 TB/s into one file at 32 writers per node. comm_ofi stripes over the NUMA-local cxi devices when `COMM_OFI_NICS` is unset (two NICs per APU without a setting). ROCm 7.14.0 builds with `-DCOMM_SHMEM` (not run on a compute node then). Proposed (not adopted then): `FI_UNIVERSE_SIZE=4096`, `FI_LOG_LEVEL=warn`, `SHMEM_SYMMETRIC_SIZE` matching our heap.
- **Rejected after review (TGTBENCH2):** hipMallocManaged host spill (host-backed memory 21 GB/s vs HBM 3.2-3.8 TB/s, 0.114 s/GB to map; the 421.5 GB layout it rested on was stale); the harness's 251.5 s estimate (contradicts the 18.6 TB per node of NIC volume: wire time alone 186 s at 100 GB/s per node); microbenchmark kernel rates as model inputs; SDMA for xGMI (our push 909-1014 GB/s per node). Adopted constants behind `--fabric target-m`: `TARGET_DEVICE_EDGE_GB = 373.44`, MAP_RATE 0.010 s/GB (no wall change: the seeds bind), host pin 256 GB per APU (info).
- **Biggest open uncertainty:** the fabric on the target (A3 injection with comm_ofi on 8 NICs, A4 all-to-all scaling to 64+ nodes) is unmeasured; every 576-node wall is modelled with assumed fabric terms (fabric 50-200 GB/s per APU: about +70 / -35 s at 5.1 × 10¹³).
- **Batch-3 ideas of SC15/MS15 that became current:** P24 (four primes at 24 digits per point, `results/LAUNCH_DEFAULTS.md §DEF`, `results/PRIMES_STRATEGY.md`) and DKM division halves were the top-ranked items; `BS_ARENA_ROOM`, `DIST_TWREC_G`, `MN_T_CHUNK_MB=2048` followed. The "no transform cache" baseline and the slot memory discussion are history.
- **Older-hardware items (L13d, G13d, S13d):** the plan printer `MN_PLAN_ONLY` (`ecalc/mn_plan.c`) is live and validated 9 of 9 against real runs; G13d measured the grid steps on one aac6 node (division/reciprocal steps real, model's tree-top re-grids not); S13d showed Sandia OpenSHMEM works across real nodes and OpenMPI OSHMEM 4.1 hangs. aac6 is set aside; these are method and history.

## TGT — target facts (user-supplied 2026-10-12) and what they change

Question: where do the user's target facts go, which backlog rows change, what does the digit write cost against the file system rate, does any launch value change. Branch `tgt`, merged; no code or default changed.
- Change: `mn_model.py` constants `TARGET_NICS_PER_APU=2`, `TARGET_NIC_GBS=50` (so `TARGET_BW_APU` stays 100 GB/s), `TARGET_FS_SINGLE_FILE_GBS=1000` at 32 writers per node (`TARGET_WRITE_BW_FS` 1.736 GB/s per node, now printed beside the write rates); `TARGET_WRITE_BW` (1.0 GB/s per node, ecalc's writer) unchanged. `profiles/target.conf.template` keys filled; `COMM_OFI_NICS` no longer set there.
- ROCm 7.14.0 (m, aac7 login node): `ecalc` and `make all` build with `-DCOMM_SHMEM`; the `ld.lld` relocation failure of `results/AAC7_PORT.md §V16` does not reproduce. Not run on a compute node.
- ecalc's writer: one process per node, 8 pwrite threads per 256 MB chunk (`MN_OUT_THREADS=8`, `MN_OUT_CHUNK_MB=256`), O_DIRECT on Lustre, two part files per node (1152 files in all): 8 writers per node into per-node files, not 32 into one.
- Write cost at 3.82 × 10¹³ (15.92 TB packed, 27.63 GB per node; no-write wall 185.5 s, mod):

| write rate per node | raw write | wall with write | exposed |
|---|---|---|---|
| 1.736 GB/s (the file system's 1 TB/s, u) | 15.9 s | 186.5 s | 1.0 s |
| 1.0 GB/s (model prior, a) | 27.6 s | 198.3 s | 12.8 s |
| 0.6 GB/s (old single-stream prior, a) | 46.1 s | 216.7 s | 31.2 s |
| 0.434 GB/s (8 of 32 writers scaling, a) | 63.7 s | 234.3 s | 48.8 s |

- Fabric: 100 GB/s per APU is now the user-supplied peak, so the estimate is unchanged; with one effective NIC per APU (50 GB/s) 222.7 s, +37.2 s (mod).
- Plan values (m, planner): `COMM_SHMEM_POOL_MB` 2304, heap 2816 MiB, `FI_UNIVERSE_SIZE` 4096, `MN_GROUPS` 2,4,8,16,32,64,192,576, pool_log 31, plan check OK, 1236 products; with `COMM_OFI=0`: pool 10240 MiB, heap 10752 MiB, OK.
- Verdict: no default changes. Three items for target work: verify two NICs per APU (M7c, +37 s at stake), benchmark ecalc's writer shape against the file system (N8, writer design gap 11.8 s vs the 1.0 GB/s prior and up to 47.8 s vs the writer-scaled case, mod), measure the SHMEM path and 12.1.0 (N7); and ROCm 7.14.0 vs 7.2.4 at 1 node (M7d). Open: the module name behind "cray-shmem 12.1.0" is assumed `cray-openshmemx/12.1.0`. Fixed (FIX1): loader accepts `TARGET_PARTITION`, `TARGET_NODES`, `ECALC_OUT_DIR` as meta keys; `mn_model.py estimate --target` / `mem_model.TARGET_DIGITS` use 3.82 × 10¹³.

## TGTBENCH2 — the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`)

Input: the user's tests on the target (576-node MI300A, 8 Cassini NICs per node) with a generic benchmark harness, not ecalc (`archive:results/TGTBENCH2_raw.md`). Base `origin/b7-vslot` c058964; login-node work only. Proposals only; no runtime change.

Summary:
1. Device edge confirmed exactly: 93.36 GB per APU = 373.44 GB per node (n = 5, 0 % spread, m target). With B7 v-slots (6.707 GB per APU = 26.83 GB per node) the then-current 4.08e13 has C-layout device 372.12 GB (m), 1.32 GB under the edge, before about 0.6 GB per APU B7ACCT could not explain.
2. The harness's "421.5 GB, 26 % oversubscribed, hipMallocManaged spill" is stale and wrong: it is the old 5.276e13 layout (before `DM_MN_LEAN`); 421.5 × 576 = 242.8 TB (not 265.4), real oversubscription 12.9 %; host-backed memory 21 GB/s for `hipMemcpy` against 3.2-3.8 TB/s HBM and 0.114 s/GB to map (ME10, m aac6). Spill rejected; cheaper levers exist (free each level's v-slots, `DIST_CHUNKS=8`: 11.5-19.5 GB, mod; `ECALC_PLANE_CAP=2^30`: -60 GB, about +80 % wall, mod).
3. The harness's "251.5 s, communication 1.0 s at 25 GB/s" contradicts the volumes: the model moves 18.6 TB per node through the NICs at 4.08e13 (2.3 TB per NIC; aac7 measured 2.47-2.63 TB per NIC at 10 nodes, m, cxi counters), so wire time at 25 GB/s per APU is 186 s; the model's wall at 25 GB/s is 369.6 s (199.6 s exposed). Their compute 250 s comes from a 64 K-point NTT microbenchmark. A3 (fabric injection) and A4 (64-node scaling) remain unmeasured: the top uncertainty (`--fall-off 0.3` costs ×1.9 at bw 47).
4. MAP_RATE 0.010 s/GB (m, target) is now profile `target-m`; init falls 1.0 s but the seed wait grows 1.0 s (CPU seeds end at 7.589 + 0.2681 × D_leaf/10⁹ ≈ 27.3 s), so the wall is unchanged (0.0 s; modelled device under `target-m` 370.5 GB).
5. Launch-line findings (Slurm-native `srun --ntasks` fixes the `fi_enable(-28)` issue for on-node multi-PE launches; our line is already Slurm-native, one PE per node; no multi-node launch reported yet).

Item verdicts (compact):
- Constants: DEVICE_MEM adopted (373.44); VMM_MAP_RATE adopted as profile; HOST 256 GB per APU recorded; PROCESS_OVERHEAD 28 GB vs our 86 GB modelled per engine process not comparable (question to user); NTT 655 GB/s (n = 65536, L2-resident) and transpose 2602 GB/s rejected (ecalc planes are 4-17 GB; 2³¹ transform 117 ms, modmul 1.31-1.34 Top/s per APU, m); INJECTION 25 GB/s is an assumption.
- C1 baseline (μ = 80.801 s at 1e10, CV 5 %) is not ecalc (ecalc 25.98-27.71 s at 7.0.3, 26.18-26.27 s at 7.2.4, m aac7); the "1.22 validates the factor" claim contradicts records (1.22 is the 7.0.3 / 7.2.4 ratio at 1e11: 205.9 vs 168.5 s; at 1e10 they match). C2 thermal: 300 s, +1 °C, no throttling, supports a 4-5 min run (partial).
- B2 device-heap vs host-staged: comm_ofi writes 4 MiB chunks from a fine-grained device pool; `COMM_OFI_POOL=host` exists (NIC16: -6 % off-NUMA on aac7); settle with `t_comm --bw` fine vs host at 2 nodes. Barriers, collectives, atomics: low relevance (one PE per node). SHMEM intra-node and SDMA: ecalc never sends intra-node bytes through SHMEM; our push 909-1014 GB/s per node (244 per APU, 93-97 % of links, m), SDMA at 50-58 GB/s per pair gives at most 174 GB/s per APU; xGMI stage already 72-76 % hidden.
- libfabric: comm_ofi sets CQ size 8192 explicitly and FI_AV_TABLE count 0; `FI_CXI_DEFAULT_CQ_SIZE` does not reach it; 100 ms HIP-init stagger does not apply (one PE per node).

Target size sweep (m layout, b7-vslot aa86cac, v-slots 26.83 GB per node everywhere; plan check OK, 1236 products below 4.0e13, 1238 from 4.0; `--check-c` exact on 3.76e13):

| T (digits) | arena | device | node / with v-slots | pieces node 0 / critical path |
|---|---|---|---|---|
| 3.0-3.15e13 | 193.27 | 350.64 | 365.90 / 392.73 | 82-86 / 98 |
| 3.2-3.3e13 | 197.57 | 354.94 | 370.20 / 397.02 | 88-90 / 100 |
| 3.35-3.5e13 | 201.86 | 359.23 | 374.49 / 401.32 | 92-94 / 100-102 |
| 3.55-3.7104e13 | 206.16 | 363.53 (9.91 under edge) | 378.79 / 405.61 | 101-103 / 107 |
| 3.7105-3.896e13 | 210.45 | 367.82 (5.62 under edge) | 383.08 / 409.91 | 103-111 / 107 → 109 → 111-127 |
| 3.897-4.0815e13 | 214.75 | 372.12 (1.32 under edge) | 387.37 / 414.20 | 111-119 / 127-133 |

Critical path inside the 367.82 tier: 107 (42+49+16) for 3.7105-3.769e13; 109 (division 16 → 18) for 3.770-3.8209e13; 111 → 127 (tree level-8 group 42 → 60) from 3.8210e13. Modelled walls (`target-m`, ROCm 7.2.4, no write / write, s): 3.71e13 bw 47 221.9 / 225.1; **3.76e13 222.7 / 226.1 (bw 11 462.0 / 458.1; bw 100 183.7 / 192.4; 13.8 TB per node)**; 3.77e13 227.8 / 228.8; 3.82e13 229.2 / 230.6 (bw 11 476.9 / 472.9; bw 100 188.9 / 196.1); 3.89e13 268.5 / 270.2; 4.08e13 282.4 / 282.9.
- Proposal (user decides): `ecalc 37600000000000`: device 367.82 GB (5.62 under the edge; about 3.2 if the 0.6 GB per APU sits on the device), node 383.08 (409.91 with v-slots), pieces 105 / 107, largest piece 1,462,222,222,224 limbs, 222.7 s at bw 47 vs 282.4 s for 4.08e13 (-21 % for -7.8 % digits; NIC volume 13.8 vs 18.6 TB per node). Alternative 3.71e13 (363.53 GB, 9.91 margin, 221.9 s). The current target of 3.82e13 sits in the same tier (367.82 GB in this build) at 109 pieces, 229.2 s at bw 47; later layouts (release, wrap off) are in `results/LAUNCH_DEFAULTS.md §FIT`.
- Questions put to the user: target size; adding `FI_UNIVERSE_SIZE=4096`, `FI_LOG_LEVEL=warn`, `SHMEM_SYMMETRIC_SIZE`; the C1 program; the 28 GB overhead; VMM rate unit; next target session (A3: `t_comm --bw` on 2 nodes with `COMM_OFI_NICS="0,4;1,5;2,6;3,7"` vs 1 NIC per APU; A4: `ecalc` at 1e10 per node with `MN_COMM_MARK=1` on 2, 8, 64 nodes).

