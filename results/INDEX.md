# INDEX — old report ID to topic file

Scope: lookup of the former `results/<ID>.md` reports (merged 2026-10-19) into the topic files that now hold them; cite as `results/<TOPIC>.md §<ID>`. | Maintained: add a row when a new `results/<ID>.md` is merged into a topic.

Reports older than the 2026-10-12 clean-up are in `archive:results/`; the raw data, logs and one-off drivers of all items are in
`archive:results_raw_2026-10-19/`, the original per-item files in `archive:results_merged_2026-10-19/`. `A11.md` (switch
inventory, read by tools), `DESIGN_TABLE.md` (generated), `SHMEM.md` and `SHMEM_STALLS.md` keep their own files.

| ID | what | topic file |
|---|---|---|
| A-ckpt | M6, per-node checkpoints of the binary-splitting phase (PLAN.md §19) | CHECKPOINT.md |
| A-comm | M7: all-gather in every transport, the layered exchange two-deep, slab pipelining (2026-09-19) | EXCHANGE.md |
| A-div | M4: the distributed division, with B2/B3/C3 (Phase 9, PLAN.md §19) | DIVISION.md |
| A-grid | the grid of piece products over shares (A3) and the 3·2³⁰ plane variant measured (C5) (2026-09-19) | GRID_GROUPS.md |
| A-mem | M9 memory accounting, C4 init sizing, C2 the region imbalance (Phase 9, PLAN §19) | MEMORY.md |
| A-out | per-node output streamed in chunks, rank-local T1/T2 (Phase 9, PLAN.md §19: M5 + C1) | OUTPUT_IO.md |
| A01 | the t_mn_grid 2-node "hang" on main (task N1) | CRASHES_HANGS.md |
| A04 | mnaccept.sh fixes (branch a04, 2026-10-11) | TOOLS_PROFILES.md |
| A05 | (opportunity G6): TASKS 1.6 `DIST_LOGN_TEST` "65 corrections" and 1.7 checkpoint/MN_GROUPS | CRASHES_HANGS.md |
| A06 | (N6): target_kit.sh hand-off items | TOOLS_PROFILES.md |
| A07 | (N7 / opportunity C6): the result-exchange rank gradient in the dm / bs products | EXCHANGE.md |
| A114 | A1 (Phase 14, W3): the intermittent leaf / seed-stage defect — root cause, fix, soak | CRASHES_HANGS.md |
| A13 | 4-node slow mode on a quiet aac7 (part A) and the DIST_LOGN_TEST 4-node "65 corrections" (part B) | NODE_SLOWMODE.md |
| A14 | what picks the "slow mode" NIC group at launch? (aac7, 2026-10-11) | NODE_SLOWMODE.md |
| A16 | Phase 16 A: ecalc on aac7 (Cray OpenSHMEMX 11.8.0, ROCm 7.0.3, Slingshot-11) | AAC7_PORT.md |
| A37CMP | apumult a37v1 spec against ecalc, item by item | APUMULT.md |
| ADOPT2 | `P24_CRT_SCAN=1` (D24) and `NEWTON_CHAIN_GUARD=1` (D25) as code defaults; two helper merges | LAUNCH_DEFAULTS.md |
| apumult_F1 | F1 — apumult §0, Category A (SSD spill/stream), Category D (trim/free/tight-size), §7 (ranked ports), §8 (architecture)  | APUMULT.md |
| apumult_F2 | apumult vs ntt/ — analysis F2: categories B (madvise), C (in-place/view/steal), E (instrumentation/safety), F (kernel/al | APUMULT.md |
| apumult_M | apumult spill study: disk and APU memory measurements (agent M) | APUMULT.md |
| apumult_R | spilling to SSD and releasing memory on MI300A (research findings) | APUMULT.md |
| apumult_summary |  | APUMULT.md |
| AS15 | Phase 15 Batch 2, row 2: arena sizing for `BS_SEED_FILL=128` (the division's VMM remaps) | MEMORY.md |
| AUD2 | removal / consolidation audit of C/HIP sources, tests and Makefile (read-only, 2026-10-12) | CODE_REDUCTION.md |
| AUD3 | where ecalc's algorithms and code structure can be simpler and smaller (read-only audit, 2026-10-12) | CODE_REDUCTION.md |
| B01 | launch-line decisions of 2026-10-11 (branch b01 from main bc80001) | LAUNCH_DEFAULTS.md |
| B04 | fast node screen before the long run (2026-10-11, Wave B) | NODE_SLOWMODE.md |
| B05 | a faster calibration process (design; CPU + reading only) | TOOLS_PROFILES.md |
| B06 | uneven node-group schedules (MN_GROUPS with a non-dividing last group), 2026-10-11, aac7, main bc80001 | GRID_GROUPS.md |
| B07 | `ecalc/calib_table.py` (step 1 of the calibration plan in results/TOOLS_PROFILES.md §B05) | TOOLS_PROFILES.md |
| B08 | how fast can the unit tests be? (2026-10-11, branch b08) | SINGLE_NODE.md |
| B13b | Phase 13b, agent B: strategy B in the library, and B4 (PLAN §31, axis S) | PRIMES_STRATEGY.md |
| B16 | Phase 16 B: ecalc bring-up on aac7 at 4 → 8 → 12 nodes (10¹⁰ digits per node) | AAC7_PORT.md |
| B7ACCT | counting the general map's v-exchange slots in the layout (branch `b7-vslot`, 2026-10-06) | GRID_GROUPS.md |
| C | B7 `alltoallv` in every transport, C4 the two-plane pipeline, C5 `DIST_STATS` under pipelining (2026-09-20) | EXCHANGE.md |
| C01 | machine profile templates (step one of the profile plan) | TOOLS_PROFILES.md |
| C02 | (opportunity C8): the gap between the layered exchange rate and the NIC rate -- where it goes at 4 / 6 nodes, three peer | OFI_FABRIC.md |
| C04 | environment probe (`tools/envprobe.sh`) and the probe covariate in `ecalc/calib_table.py` | TOOLS_PROFILES.md |
| C05 | M5, the 60-bit packed digit output (`ECALC_OUT_PACKED=2`) | OUTPUT_IO.md |
| C06 | G5 (X2 cut-group pool-offset asymmetry) and M3 (release the v-slots; S-1 remainder), 2026-10-11 (branch c06 = main bc800 | GRID_GROUPS.md |
| C09 | best MN_GROUPS for every usable node count (N = 576 .. 544), 3.71e13 and 3.79e13 digits | GRID_GROUPS.md |
| c09_groups |  | GRID_GROUPS.md |
| C16 | Phase 16 C: the model's inputs measured on aac7 (Slingshot-11, Cray OpenSHMEMX 11.8.0) and `mn_model.py` recalibrated | TIME_MODEL.md |
| C215 | the paired series for D1 (`RNS_AUTO_PIECE_COST`, C2), with `DIST_TWREC`, other sizes, and the 576 model (Phase 15, row 1 | GRID_GROUPS.md |
| C314 | C3 (Phase 14): A3 clean failure from worker threads; E12 the collective budget check | CRASHES_HANGS.md |
| CAND | the three off-by-default speed switches combined (interaction test) | SINGLE_NODE.md |
| CAP17 | the device-memory edge at the target (373 GB/node) against the layout (2026-10-06) | TARGET.md |
| CKPT | the recheck could not read the top-level set of the 6-node 3.8646 × 10¹¹ run | CHECKPOINT.md |
| CX | correctness sweep of the 2026-10-11/12 branches on aac6's CPX nodes | TESTING_VERIF.md |
| CX15 | Phase 15 agent CX: what the multi-node transform cache is worth | CACHE.md |
| D16 | Phase 16 D: the A/Bs on aac7, and the Phase E scripts (the 12-node scaled headline) | AAC7_PORT.md |
| D213d | D2 13d — the model: steps for G, the 13c recalibration, the 576 piece counts, the estimate (Phase 13d, PLAN.md §32 row D | TIME_MODEL.md |
| DAY | the 2026-10-13 day plan: large run, correctness sweep, SHMEM sweep, 6-node repeat | NIGHT_RUNS.md |
| DEF | P24 as the multi-node default (D18) and DM_MN_WRAP in the memory model + 6-node A/B (D19 b) | LAUNCH_DEFAULTS.md |
| DKM15 | the division in two quotient halves with a half-length reciprocal (Phase 15 Batch 3, agent DKM) | DIVISION.md |
| DL15 | the arena sized to DKM's division, and the room check on the model's node (Phase 15 Batch 3, agent DL) | MEMORY.md |
| DOC15 | the documentation and the models on the user's decisions of 2026-09-27 (Phase 15, agent DOC) | TARGET.md |
| DOC215 | the documentation and the models on the user's decisions of 2026-09-28 (Phase 15, agent DOC2) | TARGET.md |
| EW15 | the writer starts on X_hi's digits after DKM's step 1 (Phase 15 Batch 3, agent EW) | DIVISION.md |
| F16 | the 576-node figures re-estimated from the inputs measured on aac7 (Phase 16 C), with bands | TIME_MODEL.md |
| FIT | the memory fit before every launch, and DM_MN_WRAP automatic | LAUNCH_DEFAULTS.md |
| FIX117 | FIX1 17 — t_edge vmm, mn_selftest power-of-two>=128, MN_TOPO_GROUP under SHMEM | TESTING_VERIF.md |
| G | grid/dm: A5 the division's cuts over shares, A1 the transform cache, A6 the dist tier's fused pointwise (Phase 10, PLAN. | GRID_GROUPS.md |
| G12 | the top product at scale: the tree's levels as piece grids for any g, the exact spills, the k-way schedule, the per-node | GRID_GROUPS.md |
| G13d | the steps on real hardware (Phase 13d, PLAN §32 row G) | TARGET.md |
| G2 | hang bound for the `DBIG_QSEL=1` start-up hang (aac6 CPX soak) | CRASHES_HANGS.md |
| G515 | Phase 15 Batch 2, item 5 (PLAN §37 row 6): the general map's twiddle packs | KERNELS.md |
| G6 | (TASKS 1.6 / 1.7): checkpoint restart under another MN_GROUPS / node count | CHECKPOINT.md |
| H | host memory: X never on the host (B1), the seeds streamed to the regions (B2) (Phase 10, PLAN.md §21) | MEMORY.md |
| H1 | PLAN.md 28.4 measurement, single-node part (decimal vs binary limbs, no code changed) | CODE_REDUCTION.md |
| H2 | PLAN 28.4, multi-node part: t_mn_grid in decimal vs binary limbs on aac7 (2026-10-11) | CODE_REDUCTION.md |
| H4 | PLAN 28.5 implemented on branch `h4`: the binary limb pipeline archived, decimal limbs only | CODE_REDUCTION.md |
| H5 | decimal (LIMB_BASE=10) vs binary (LIMB_BASE=2) limb pipeline, paired wall clock | CODE_REDUCTION.md |
| HANG | t_mn_grid "hang" inside mnaccept --only unit,e9 (2026-10-13) | CRASHES_HANGS.md |
| I | the initialisation floor: every allocation form measured, the seeds against the mapping, the 3·2³⁰ planes on the number  | MEMORY.md |
| IO15 | Phase 15 agent IO: output I/O on a network file system, packed digits, write scheduling, the memory guard | OUTPUT_IO.md |
| IO215 | Phase 15 agent IO, second task: per-part conversion of the packed digit files | OUTPUT_IO.md |
| K13 | Phase 13a agent K: kernel constants (PLAN §29 H2, H3, H6, H7) | KERNELS.md |
| K13b | Phase 13b agent K: the b1 pass and the stride penalty (PLAN §31, row K) | KERNELS.md |
| K15 | a correction without a rewrite (PLAN §36, row 1d) | DIVISION.md |
| K1b | raw-once gather (S39 candidate A, O7b lever 3) -- IMPLEMENTED behind DIST_GATHER_RAW (off), probe + gates + ABBA ARMED o | EXCHANGE.md |
| K4b | `k_crt_batch`: the per-chunk carry chain as a parallel carry-lookahead | KERNELS.md |
| K4c | `k_crt24`: the P24 CRT's per-chunk carry chain as a parallel carry-lookahead | KERNELS.md |
| K6 | single-node C-tier all-to-all — overlap across primes (RNS_DIST_OVERLAP=1, off by default) | EXCHANGE.md |
| K8 | init and the seed wait at one node: where the time goes, and why reordering does not help (2026-10-12) | MEMORY.md |
| K8b | seed transfers without the HIP runtime inside init's mapping window (2026-10-12) | MEMORY.md |
| KP15 | the correction patch on packed output (a defect on main, B2 44c1642) | OUTPUT_IO.md |
| L | the layout at 576: a block-cyclic map for a group of any size (C3-576), the four `alltoallv` consumers (B7), `MN_GROUPS` | GRID_GROUPS.md |
| L1 | COMM_LAYER_VSLOT_RELEASE=1 on the launch lines (user, 2026-10-11) | LAUNCH_DEFAULTS.md |
| L114 | L1 — Phase 14 (PLAN §34), window W1: E2 tight reservation (`DM_TIGHT`) and E5's layout half (`DM_TAIL_DEAD`) | MEMORY.md |
| L13d | the plan printer (PLAN §32, row L) | TARGET.md |
| L2 | validation of the code reduction (B02) at full size and for speed, aac6 (2026-10-11) | CODE_REDUCTION.md |
| L4 | refit of the time model to all of 2026-10-11's multi-node aac7 runs; what it got wrong and why (wave L, agent L4) | TIME_MODEL.md |
| L5 | parameter system step P1-1: switch registry + print-only effective configuration line | TOOLS_PROFILES.md |
| M | device memory: B5, B6, B4 / A3, the 8 × 10¹⁰ attempt (Phase 10, PLAN §21) | MEMORY.md |
| M11 | memory: the arena's reserved tail (decision 5), the tree's slabs in the arena, the per-node model, 10¹¹ on one node (Pha | MEMORY.md |
| M13 | memory truth: one ceiling, the two O(share) terms bounded (Phase 13a, PLAN §30 row M; TASKS 1.1, 1.2, 1.3) | MEMORY.md |
| M2a | time-model refit, scaling study, 1- and 2-node points (aac7 queue items 77 / 78) | TIME_MODEL.md |
| M3 | the top levels of the binary-splitting tree as distributed products over node groups (2026-09-18) | GRID_GROUPS.md |
| M615 | Phase 15 Batch 2, agent M6: item 6 (E11, band-sized products and the chunked division window) with the Q/S margin trim | DIVISION.md |
| MAP15 | Phase 15 Batch 2, row 5 (N2): the arena's mapping, the seeds and level 1 | MEMORY.md |
| MEAS-N7 | SHMEM data path (`COMM_OFI=0`) against libfabric (`COMM_OFI=1`) at 2 nodes | SHMEM_ITEMS.md |
| MEAS-P24 | four-prime product form (P24) for all multi-node products against the default, 2 nodes | PRIMES_STRATEGY.md |
| MPB15 | MPB (Phase 15 Batch 3): the `ECALC_NP=auto` switch-over on min(pa, pb) (`ECALC_NP_AUTO_MIN=1`) | PRIMES_STRATEGY.md |
| MS15 | the node memory at 5.1–5.7 × 10¹³ on 576 nodes, and the largest target that fits 480 GB (Phase 15, agent MS) | TARGET.md |
| N13 | Phase 13a agent N — checkpoint hardening (TASKS 1.7, 4.1, 1.4) | CHECKPOINT.md |
| N3 | host gaps in the batch tier (`BS_BATCH_PIPE`) | KERNELS.md |
| N414 | `t_mn_grid` on real nodes over SOS, `mnrun.sh`'s SHMEM detection, the dist tier's load speed (Phase 14, PLAN §33 A4, A5) | TESTING_VERIF.md |
| N7 | SHMEM data path (`COMM_OFI=0`) against `comm_ofi`: sweep, diagnosis, peer-order fix, confirmation | SHMEM_ITEMS.md |
| N7B | one SHMEM proxy PE per APU (`COMM_SHMEM_PE_PER_APU=1`): bandwidth, gates, 6-node ABBA | SHMEM_ITEMS.md |
| N7C | SHMEM proxy mode: why it is slow, and `COMM_SHMEM_PX_NUMA=1` + `HSA_ENABLE_SDMA=0` (config C) | SHMEM_ITEMS.md |
| N7D | SHMEM proxy mode toward comm_ofi speed at 6 nodes (copy kernel, parallel init, put-with-signal sends) | SHMEM_ITEMS.md |
| N7E | the N7D proxy-mode switches at 6 nodes: paired repeat (PX_SEND=2, PX_INIT=1) against comm_ofi | SHMEM_ITEMS.md |
| N7E_V | N7E-V — COMM_SHMEM_PE_PER_APU=2 (two helper PEs per APU): correctness record | SHMEM_ITEMS.md |
| N7G | COMM_SHMEM_PX_THP and COMM_SHMEM_PX_PIN on the proxy mode: pairs and verdict | SHMEM_ITEMS.md |
| NIC | node screen catches nodes with fewer NICs than the launch asks for | NODE_SLOWMODE.md |
| NIC16_experiments | NIC16 — one process driving all Slingshot NICs of a node (aac7) | OFI_FABRIC.md |
| NIGHT | overnight runs 2026-10-13 (five aac7 items, two local analyses) | NIGHT_RUNS.md |
| NIGHT2 | overnight regression on main 1aa2b80 (2026-10-14) | NIGHT_RUNS.md |
| NP15 | NP (Phase 15 Batch 2, PLAN §37 row 1) — four primes only where needed (`ECALC_NP=auto`) | PRIMES_STRATEGY.md |
| NTT3P | can ecalc's large transforms run in 3 memory passes instead of 4? (design study, no code changed) | KERNELS.md |
| O1 | overnight 6-node paired ABBA, MN_GROUPS `2,6` (default) vs single level `6` (TASKS 42) | GRID_GROUPS.md |
| O2 | the reciprocal-gather stall: why `db_to_bi` of 0.2 MB takes 2.2 s (2026-10-11/12) | DIVISION.md |
| O3 | P1 step 2: profile loading and validation (branch o3, not merged) | TOOLS_PROFILES.md |
| O4 | packed output (`ECALC_OUT_PACKED=2`) adoption path, write rate and round trip | OUTPUT_IO.md |
| O5 | K7-a GPU-3 rebalance -- the B5 form (B4 with APU 3 given a share of the inverse) | DIVISION.md |
| O8 | TASKS backlog clean-up (local only, no cluster use; 2026-10-11, branch o8 off main 0298fe2) | CODE_REDUCTION.md |
| OFI17 | `comm_ofi`: a multi-NIC bulk path for ecalc (Phase 17, branch `p17-ofi`) | OFI_FABRIC.md |
| P | single-node speed: B3 planes at init, A2 level-22 pairing, DM_POOL default, I11 init, A4 tile budget (Phase 11, PLAN §26 | SINGLE_NODE.md |
| P1 | the distributed division multiply: the low product X·Q as a low part + one cyclic plane (`DM_MN_WRAP`) | DIVISION.md |
| P13b | sizing follows the prime count; the plane cap as one switch; the one-node ceiling at three primes (Phase 13b, PLAN §31 r | MEMORY.md |
| P15 | P (Phase 15, PLAN §36, Batch 1 from L8's findings) — roots above 2³³, the prime bound at the 576 target | PRIMES_STRATEGY.md |
| P16 | Phase 16 P: the fabric limit as an algorithm-design problem on aac7 — four PEs per node | OFI_FABRIC.md |
| P2 | speed up k_gather -- STOPPED after a micro-probe: the kernel is bound by the remote read, and the best possible kernel-l | EXCHANGE.md |
| P214 | the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0) | SHMEM_ITEMS.md |
| P2415 | four primes at 24 digits per transform point in the mn tier (Phase 15 Batch 3, agent P24) | PRIMES_STRATEGY.md |
| P3 | shorten the start-up mapping phase: evaluated by reading code and A09, no change made (STOPPED) | MEMORY.md |
| PC15 | Phase 15 Batch 3 agent PC: a partial transform cache inside 480 GB (`RNS_DIST_CACHE_PARTIAL`) | CACHE.md |
| PS15 | Phase 15 agent PS: plane pool 1 at four primes (the rc 6 at 9.17 × 10¹⁰, `ECALC_NP=4`) | MEMORY.md |
| Q | the target plan: the models completed for 576 nodes, `estimate(g, D)`, the runbook (Phase 12, PLAN.md §27 row Q; 2026-09 | TIME_MODEL.md |
| R | the race at the batch tier's level transition (Phase 12, PLAN.md §27 row R; D5) | CRASHES_HANGS.md |
| R114 | R1 (Phase 14, W1): E7 the reciprocal's products cut to the band read; E9 Karatsuba over the grid (model); E10 the 576 tr | DIVISION.md |
| R16 | the region-pool sizing abort of ecalc at 10¹¹ digits on 4 nodes (2.5 × 10¹⁰ per node) | MEMORY.md |
| R415 | R4 (Phase 15, row 1b): the reciprocal's Q_t·r as a middle product (V3 item 4 = PLAN §33 B2) | DIVISION.md |
| RECIP | the Newton reciprocal's error: the t_newton bound or the reciprocal? | DIVISION.md |
| ROCM | ROCm 7.14.0 against 7.2.4 at 1 node (OPPORTUNITIES M7d) | AAC7_PORT.md |
| S | the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20) | SHMEM_ITEMS.md |
| S114 | S1 (Phase 14, W1): spill and I/O — E1, E3, E4, E12 of internal apumult study | OUTPUT_IO.md |
| S115 | Phase 15 agent S1: the decimal `mul_1` and the fused seed span (`BI_MUL1_FAST`) | KERNELS.md |
| S12 | the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21) | SHMEM_ITEMS.md |
| S13 | Phase 13a, agent S — E0 (the A/B/C strategy boundary) and E2 (plane cap against grid cost) | PRIMES_STRATEGY.md |
| S13d | SHMEM across real nodes (TASKS 1.5), Phase 13d agent S (2026-09-23) | TARGET.md |
| S16 | the intermittent startup segfault of ecalc under Cray OpenSHMEMX on aac7 (Phase 16 S) | CRASHES_HANGS.md |
| S18KIT | `ecalc/target_kit.sh`: one script for the next target session (branch `s18-kit`) | TOOLS_PROFILES.md |
| S18KITFIX | three bugs in `ecalc/target_kit.sh`, found in the aac7 rehearsal (`<cluster work dir>/s18ab3`), fixed and verified for real | TOOLS_PROFILES.md |
| S18M | AAC7 profile calibration against the 2026-10-06/07 10-node comm_ofi runs (branch `s18-m`) | TOOLS_PROFILES.md |
| S18W | `tools/rundriver.sh` and `ECALC_SEGV_TRACE` (branch `s18-w` off `main` ef22f22) | CRASHES_HANGS.md |
| S19B | profile analysis of S19A (2 → 10 nodes) and the S19C switch A/B, with a 576-node projection | TIME_MODEL.md |
| S21 | single-node A37 measurements (R2 seed microbench, Q1 init timeline, R4 dc split, R6 small-j doublings, Q4 NTT structure) | SINGLE_NODE.md |
| S22 | / S23 / S24: T2048 sequential A/B (D2), 10-node scaling sweep with wait + ready stats (E1), Q1 seed sensitivity, NTT swe | SESSIONS.md |
| S27 | / S25 / S26: COMM_XSTATS decomposition, X1 rot ABBA, share ladder, A37-Q9, soak counts | SESSIONS.md |
| S30 | the reciprocal's late node, and today's two segfaults (2026-10-08; aac7 hold 12331; analysis only, nothing launched) | CRASHES_HANGS.md |
| S31 | / S32 / S33 -- X2 second try, host-vs-rank, VMM_SAFE soak: the comm pool failure of S28 and its fix (2026-10-08) | SESSIONS.md |
| S34 | where the rc 139 segfault is (2026-10-09) | CRASHES_HANGS.md |
| S36 | idea S-1: release (share) the v-slots between the tree levels (2026-10-10) | GRID_GROUPS.md |
| S38 | the next single-node speed lever. Found: the dbig add/sub kernel (k_addsub) ran at 36-84 GB/s; DBIG_ADDSUB2=1 (coalesced | KERNELS.md |
| S39 | kernel survey of the 6.441e10 single-node profile (read-only; no runs, no code edits). Found: the next levers are k_gath | KERNELS.md |
| S40 | DBIG_MAXIDX_TOP (top-down k_maxidx) measured -3.2 s per run (95 % CI -5.6 .. -0.75, 3 ABBA rounds); micro-test refutes " | KERNELS.md |
| S41 | DBIG_QSEL (scalar-select quarter pointers), gates and ABBA | KERNELS.md |
| S43 | multi-node A/B of the three dbig kernel defaults (aac7, 2 and 4 nodes) | KERNELS.md |
| S44 | aac6 SH5_MI300A nodes -- regression and ABBA of DBIG_MAXIDX_TOP, DBIG_QSEL, DBIG_ADDSUB2 | KERNELS.md |
| S45 | CPU-only validation of COMM_LAYER_VSLOT_SHARE=1 at the 576-node shape (2026-10-10, branch s45 = s36 + main 13c244e) | GRID_GROUPS.md |
| S46 | crash + hang track (batch 2026-10-11, track T1) | CRASHES_HANGS.md |
| S47 | exchange settings re-confirm at 4 nodes (aac7, batch 2026-10-11, track T2) | LAUNCH_DEFAULTS.md |
| S48 | scaling study (1/2/4/6 nodes at 6.441e10 digits per node) and refit of the aac7 model profile | TIME_MODEL.md |
| S49 | slow-host policy: per-host `nodechk` rehearsal (aac7 all hosts, aac6 PPAC) | NODE_SLOWMODE.md |
| S50 | k_gather coalesced write (DIST_GATHER_TILE, off by default) -- IMPLEMENTED, gated on CPX (test), timing queued (aac7 Job | EXCHANGE.md |
| S51 | kernel round 2, part b (K2 k_addsub3, K3 k_modq_m, K4 k_crt_batch_m) -- FINISHED: K2+K3 propose default (1-node signific | KERNELS.md |
| S52 | more digits at 576 nodes (layout checks) and message aggregation (design) (2026-10-10/11, branch s52 from main daeb0f9) | GRID_GROUPS.md |
| SC15 | scouting Batch 3 for the target 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent SC; desk work) | TARGET.md |
| SCALE35 | scaling points at 3 and 5 nodes | TIME_MODEL.md |
| SH3 | shrink pass 3: rns_dist.c dead items, duplicated helpers, the host product tier (2026-10-12) | CODE_REDUCTION.md |
| SHM | the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT) | SHMEM_ITEMS.md |
| SHR2 | shrink pass 2: top-level checkpoints only, the unreachable stand-in, SOS/OSHMEM builds, AUD2 re-scan (2026-10-12) | CODE_REDUCTION.md |
| SIMP | AUD3 items 1–4: one single-node division flow, DKM only, no 3·2^k distributed planes, one grid planner | CODE_REDUCTION.md |
| STAT | static-analysis triage (L1, results/NIGHT_RUNS.md §NIGHT section 6) and SH4 leftovers | NIGHT_RUNS.md |
| STD17 | OFI memory accounting (OFIMEM) and the standard 10-node run with all NICs (Phase 17, branch `p17-ofimem`) | OFI_FABRIC.md |
| T | tests, checkpoints and docs (Phase 10, PLAN.md §21: D3, D2, D4, D5, C6, E3) | TESTING_VERIF.md |
| T05 | why 4-node run totals are bimodal (fast ~181 s / slow ~208 s) | NODE_SLOWMODE.md |
| T06 | layeq / layv rate in the time model, and the 576-node estimate redone | TIME_MODEL.md |
| T08 | K2 + K3 default on (user adopted 2026-10-11) | LAUNCH_DEFAULTS.md |
| T09 | the bimodal 4-node totals (2026-10-11, aac7, session task t09) | NODE_SLOWMODE.md |
| T10 | cold-path node-check probe vs the known slow host aac7-n01 (2026-10-11) | NODE_SLOWMODE.md |
| T11 | aac6 SH5 CPX paired ABBA of K2 (DBIG_ADDSUB3) + K3 (DBIG_MODQ_MONT) at 4e9 digits | KERNELS.md |
| T114 | Phase 14 agent T1: E10a, the early free in the multi-node tree (`MN_TREE_EARLY_FREE`) | MEMORY.md |
| T12 | 4-node uffd soak (FI_MR_CACHE_MONITOR=userfaultfd) on aac7 | OFI_FABRIC.md |
| T13 | is the t_mn_grid 2-node hang on main the S46 lock-order deadlock? | CRASHES_HANGS.md |
| T215 | Phase 15, row 1c: `BS_SEED_TERMS` chosen per run so the batch tier fills its transform lengths | SINGLE_NODE.md |
| T23 | the `DIST_LOGN_TEST` 4-node ">64 corrections" defect | CRASHES_HANGS.md |
| T27 | parameter system: computed size values and review dates for settings | TOOLS_PROFILES.md |
| TARGET_HW_REVIEW | the target's microbenchmark report against the model (desk review, 2026-10-03, 22:52 EDT) | TIME_MODEL.md |
| TC15 | Phase 15 agent TC: the multi-node transform cache's allocation (internal target task list T7) | CACHE.md |
| TGT | target facts (user-supplied 2026-10-12) and what they change | TARGET.md |
| TGT15 | the target moved to 5.1 × 10¹³ digits on 576 nodes (Phase 15, agent TGT, 2026-09-28) | TARGET.md |
| TGT17 | the 4.08 × 10¹³ target: set to fit the device-memory edge comfortably (2026-10-06) | TARGET.md |
| TGTBENCH2 | the user's second set of target tests, assessed against the ecalc design (2026-10-06, branch `tgtbench2`) | TARGET.md |
| TREF | a test reference product, and the host product tier removed | CODE_REDUCTION.md |
| TUNE17 | comm_ofi tuning: chunk size × writes in flight, and two NICs per device (Phase 17, aac7) | OFI_FABRIC.md |
| V | verification and cleanup (Phase 11, PLAN.md §26 row V: D5, ECALC_RECHECK, E1 (c), E2) | TESTING_VERIF.md |
| V114 | the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1) | SHMEM_ITEMS.md |
| V16 | Phase 16 V — fastest stack on aac7 (ROCm 7.2.4 default), survey, brief A/Bs | AAC7_PORT.md |
| V214 | the remaining one-node options on the Phase 14 defaults: auto's per-piece cost, DIST_TWREC, checkpoints with O_DIRECT (P | SINGLE_NODE.md |
| V314 | Phase 14 agent V3: the remaining development items (PLAN §33.2 B1–B7, APUMULT E11), with numbers | SINGLE_NODE.md |
| W | verification and housekeeping (Phase 12, PLAN.md §27 row W; DECISIONS2 §8) | TESTING_VERIF.md |
| W2A | wave 2, lane A of the user-approved settings removal (2026-10-12) | CODE_REDUCTION.md |
| W2B | wave 2, lane B of the settings removal (2026-10-12) | CODE_REDUCTION.md |
| WM15 | the weak-memory audit (Phase 15, task 0b) | TESTING_VERIF.md |
| WP6 | inter-node correctness on aac6 (2026-09-17/18) | TESTING_VERIF.md |
| WP7 | checkpoint and restart of the binary-splitting phase | CHECKPOINT.md |
| X | cross-fabric traffic and the 576-node model (Phase 11, PLAN.md §26 row X) | TIME_MODEL.md |
| X13 | xGMI and the fabric at the same time (PLAN.md §29 E9 part 1, H4), Phase 13a | EXCHANGE.md |
| X13b | the uneven exchange two deep, one-kernel transposes, faster packs (PLAN.md §31 row X, axis A) | EXCHANGE.md |
| XEFF | why ecalc's exchanges underuse the transport, and what to change | OFI_FABRIC.md |

Topic files:

| file | subject |
|---|---|
| SHMEM_ITEMS.md | the SHMEM transport and proxy mode: per-item records behind results/SHMEM.md and SHMEM_STALLS.md (N7 series, pool sizing, strided PE sets, target forms) |
| OFI_FABRIC.md | comm_ofi (libfabric multi-NIC path), NIC use, the fabric limit and the exchange rate versus the NIC rate |
| NODE_SLOWMODE.md | the bimodal 4-node totals (slow mode), what picks it, and the node screens (nodechk, NIC count, slow hosts) |
| CRASHES_HANGS.md | crashes, hangs and intermittent defects: t_mn_grid hang, segfaults, races, DIST_LOGN_TEST corrections, hang bounds |
| CHECKPOINT.md | checkpoint and restart of the binary-splitting phase: per-node checkpoints, hardening, restart under another MN_GROUPS |
| TOOLS_PROFILES.md | tools and the parameter system: profiles, the switch registry, calibration, the target kit, mnaccept, environment probe |
| LAUNCH_DEFAULTS.md | launch-line decisions and defaults adopted: slot release, K2+K3, P24 default, the pre-launch memory fit, exchange re-confirm |
| NIGHT_RUNS.md | overnight and day-plan runs of 2026-10-13/14 and the static-analysis triage |
| GRID_GROUPS.md | the grid of piece products over shares and node groups: layout at any group size, MN_GROUPS schedules, v-slot release, the 576-node layout checks |
| EXCHANGE.md | the all-to-all exchange in every transport: all-gather, layered exchange two-deep, slab pipelining, the rank gradient, gather kernels |
| TIME_MODEL.md | the time model and the 576-node estimates: refits to measured runs, scaling studies, the model's inputs measured on aac7 |
| TARGET.md | the target (digits and 576 nodes): facts, plans, device edge, scouting, documentation on the user's decisions, steps on real hardware |
| MEMORY.md | device and host memory: accounting, the arena and its sizing, the reserved tail, init floor, early free, the memory ceiling |
| DIVISION.md | the distributed division and the reciprocal: Newton error, DKM halves, middle product, band-sized products, the low product, rebalance |
| KERNELS.md | single-node kernels: dbig add/sub, maxidx, quarter select, CRT carry lookahead, twiddle packs, kernel constants, survey and NTT pass study |
| SINGLE_NODE.md | single-node speed and options: init, level pairing, remaining options on the defaults, development items, seed terms, combined switches |
| PRIMES_STRATEGY.md | prime count and strategy: four primes only where needed, 24-digit points, auto switch-over, strategy boundary and plane cap, roots above 2^33 |
| CACHE.md | the multi-node transform cache: what it is worth, its allocation, the partial cache inside 480 GB |
| OUTPUT_IO.md | output: streamed chunks, 60-bit packed digits, write scheduling on a network file system, the correction patch, spill and I/O |
| CODE_REDUCTION.md | shrinking the code: audits, shrink passes, settings removal waves, validation of the reduction, the limb-format study and the host tier removal |
| TESTING_VERIF.md | verification, correctness sweeps and test design: per-phase verification, inter-node correctness, weak-memory audit, t_mn_grid over SOS |
| AAC7_PORT.md | the aac7 port (Phase 16): bring-up at 4 to 12 nodes, the A/Bs and the 12-node scaled headline, the fastest software stack |
| APUMULT.md | the apumult study (internal reference multiplier) against ecalc: spec comparison and the apumult measurements |
| SESSIONS.md | aac7 measurement sessions of September/October: profile analysis 2 to 10 nodes, T2048 A/B, XSTATS decomposition, X2 and VMM_SAFE soak |
