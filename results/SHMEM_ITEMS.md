# SHMEM_ITEMS — the SHMEM fallback transport: pool sizing and strided PE sets

Scope: per-item records of the OpenSHMEM transport `COMM_TRANSPORT=shmem` (`ecalc/comm_shmem.c`): the design and portability contract (§S, §S12), the symmetric pool and its sizing law (§P214, §V114, §SHM). The proxy-mode records (§N7, §MEAS-N7, §N7B-§N7G) were archived 2026-10-23 (`archive:results_merged_2026-10-23/SHMEM_ITEMS_full.md`); the summary of that work is `results/SHMEM.md`. Condensed from the former per-item files (old IDs resolve through `archive:results_merged_2026-10-24/INDEX.md`); raw data and drivers in `archive:results_raw_2026-10-19/`. Labels: m = measured, mod = modelled, a = assumed.

Sections moved to `archive:results_merged_2026-10-24/SHMEM_ITEMS.md` (2026-10-24 cleanup): §S; §S12; §SHM.

## Topic summary

- **Live conclusions.** (1) Pool = 4 APU threads x (send + receive) of the largest staged exchange; the law is exact (§P214) and rounds cut the second copy (§V114: 1e10 on 2 nodes pool 8194 -> 2050 MiB, total not slower m). (2) Under `COMM_OFI=0` the v-slots must count in the pool (§SHM). (3) The third layer (`MN_TOPO_GROUP`) costs 16-20 % on one node and stays off (§S12); `DIST_MN_SYM_SLABS=1` measured and kept off (§P214). Three Phase 11 OSHMEM traps (`setarch -L`, quiet not fence, serial calls) remain launcher or runtime switches only.
- **Current state.** The default multi-node transport is xshmem (`results/XSHMEM.md`); this transport is the fallback and the reference (`COMM_SHMEM_ROUND_MB=128` on the aac7 launch line, Q60). The proxy mode and its `PX_*` switches, `COMM_SHMEM_PEER_ORDER` and `COMM_SHMEM_LAYV_ANY` are removed.

| item | title |
|---|---|
| S | the SHMEM transport (M8-s), strided PE sets per tree level, the dragonfly third layer (2026-09-20) |
| S12 | the SHMEM transport's target forms, tested for real on Sandia OpenSHMEM (2026-09-21) |
| P214 | the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0) |
| V114 | the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1) |
| SHM | the SHMEM pool under `COMM_OFI=0` is too small at 6 nodes (bug A of NIGHT) |

## §P214 — the SHMEM pool: measured, modelled, sized (PLAN §33 A2, TASKS 2.4, TARGET_TASKS T0)

Phase 14 agent P2, branch `p14-P2` from `main` @ a839418. SOS build on aac6, two real nodes over 1 GbE (one PE per node,
`COMM_SHMEM_DEVHEAP=1`) and four-process one-node runs; every digit check `cat parts | cmp - ref`. Final commit 7d377c4; regression
ran on 8b6955e. Open at return: the user's decision on `MN_T_CHUNK_MB=1024` for the target; staging in rounds would remove the
second copy (done in §V114).

**Measurements (m).** Pool peak = 4 APU threads x (send + receive) of the single largest staged exchange (8 blocks at every
peak); only staging fills it (control 2 MiB at 2 PEs, 6 at 4; mailbox 0.02 MiB). Every exchange of `ecalc` was staged.

| run | digits | pool peak MiB | largest staged exchange per APU thread (send + recv MiB) | wall s |
|---|---|---|---|---|
| 2 nodes SOS | 1e8 identical | 130 | 10.6 + 10.6 (result exchange) | 25.15 |
| 2 nodes SOS | 1e9 identical | 849 | 106.0 + 106.0 (`mn_core` result exchange) | 171.95 |
| 2 nodes SOS | 1e10 identical | 8479 | 2 x 1059.6 | 2070.7 |
| 4 proc one node | 1e9 identical | 429 | 53.0 + 53.0 | 30.63 |
| 4 proc one node | 1e10 identical | 4244 | 529.8 + 529.8 | 283.26 |
| 2 nodes, `DIST_LOGN_TEST=25` (2 x 2 pieces) | 1e9 identical | PE 0 531, PE 1 637 | PE 1: 53.0 + 106.0 | 197.01 |
| 2 nodes, `COMM_SHMEM_POOL_AUTO=1` | 1e10 identical | 8479 | 1059.6 + 1059.7 (96 exchanges) | 2035.97 |
| 2 nodes, `MN_T_CHUNK_MB=64` | 1e9 identical | PE 1 637 | `mdb_shift` 106.0 + 53.0; result exchange 64 + 64 | 170.77 |

**The law (mod; measured to 0.3 MiB at all eight points).** `pool = 4 x 8 x max over mn_core products and the division shift of
staged send + receive per APU thread + control (4 x (g + lower sizes) x (RING_KB + 64 B)) + mailbox (1024 x g x 8) + 256 MiB
margin` (`mem_model.shmem_pool`; in C `binsplit_shmem_pool_need` + `rns_mul_dist_mn_stage`). Per product: result exchange =
RT(nc_piece) + min(share_c, nc_piece)/4 (each <= m with `MN_T_CHUNK_MB=m`); redistribution = RT(pa) + min(share_a, pa)/4 + 2g
rows; transform = 2q/4; division `mdb_shift` = (2 n_Q/g/4 + n_Q/g/4)/K. Below the cap, `A_h mu` sets it: **pool ~ 32 n_Q/g
bytes ~ 1.78 bytes per digit per node** (x10 per decade; halved at 4 processes). Target 4.25e13 on 576 nodes
(`MN_GROUPS=2,4,8,16,32,64,192,576`, GB per node, mod):

| configuration | pool | node total vs 480 GB |
|---|---|---|
| defaults (TARGET.md §4) | 81.6 (`COMM_SHMEM_POOL_MB=77824`) | 524.7, exceeds |
| + `DM_TIGHT=1` | 81.6 | 501.2, exceeds |
| + `MN_T_CHUNK_MB=1024` | 45.0 | 459.6, fits |
| + `MN_T_CHUNK_MB=1024 DM_TIGHT=1` | 45.0 | 434.3, fits |
| + `DIST_MN_SYM_SLABS=1` | 133.1 | 576.2, exceeds |

`estimate.py` (default `--staging code`): defaults pool 82 / node 525 GB "no"; `--chunk both` pool 45, node 460, wall 244.9 s
(4.1 min) vs 234.0. Largest 576 run at 480 GB: 3.73e13 digits (defaults), 4.59e13 (`MN_T_CHUNK_MB=1024`) (mod).
**Answer to T0:** the staged buffers were counted in the device total already, and the pool holds a *second copy* of the largest
exchange's pair, so both the pool setting and the node total were wrong; 452 GB does not hold with defaults. The README row's
"`MN_T_CHUNK_MB` 1024 since Phase 13c" was wrong: `mn_t_chunk_limbs` defaults to 0 (only `MDB_SHIFT_CHUNK_MB` is 1024). Designs to
remove the second copy: (a) staging in rounds (done in §V114), (b) rbO/sb/rb from the symmetric pool (not done).

**TASKS 2.4 (`DIST_MN_SYM_SLABS=1`, off; m, 1e9, 2 nodes).** Digits identical; pool peak 849 -> **2385 MiB** (staging 847.7 +
symmetric 1536), the transform's 3456 staged exchanges vanish, block pool peak 2.5 -> 2.0 GB, total 172.48 vs 171.23 s. The
transform's staging never sets the peak (32 vs 106 MiB), so the slabs only add 3q*8 B per APU of resident memory (51.5 GB per node
at the 576 target). Kept off; closed as measured.

**Pool rule.** `binsplit_shmem_pool_rule(N, COMM_SIZE)` after `rns_init`: `COMM_SHMEM_POOL_AUTO=1` raises the pool to the need
(256 MiB steps; 256 -> 512 m, identical); without it a too-small pool gets one warning; a pool that runs out stops with an error
naming the use by kind, the size needed and the model's need (`COMM_SHMEM_POOL_MB=96`: rc 3 in 3 s). `MN_PLAN_ONLY` prints
`plan pool`. A host heap needs heap >= pool + 512 MiB.

**Gates (m).** SOS 2 real nodes identical at every size (1e8 x4, 1e9 x7, incl. sym slabs, chunk 64, `DIST_LOGN_TEST=25`); 4
processes 1e9/1e10 identical; TCP 2 nodes identical (1e8 23.02 s, 1e9 170.03 s); **1e10 on 2 real nodes with the final SOS
build and the rule: identical, 2035.97 s (bs 397.2, dm 1620.4, init 14.1)**, pool 8192 -> 8960, peak 8479; regression (8b6955e):
**17 passed, 0 failed**, 1907 s (4e10 identical 61.67 s, RECHECK OK); TCP 1e10 4 processes identical 81.31 s.

**Open then.** Target does not fit 480 GB with defaults (525 GB mod); `MN_T_CHUNK_MB=1024` fits (460) at +0.2 min mod (user's
decision). The law's multi-piece terms are validated at one small shape; the 576 figure rests on them. The Makefile's test rules
lack object dependencies (`make -j16` can link a test before `rns_dist.o` is rebuilt). The device model counts the division's
products with the top tree level's scratch (`A_h mu` rbO at 576 is 16.4 GB per APU, twice the tree top's 8.2).

## §V114 — the SHMEM pool by default, and the exchanges in rounds (Phase 14 agent V1)

Branch `p14-V1` from `p14-defaults` @ 4855842; background §P214. Final code 8f3b136; b6 (2-node re-check on 8f3b136) was never
submitted.

**Task 1: `COMM_SHMEM_POOL_AUTO=1` by default (done).** `mnrun.sh` (when `COMM_SHMEM_POOL_MB` is unset) finds the SHMEM-linked
executable followed by a digit count, runs it with `MN_PLAN_ONLY=<digits>:<procs>` (no device touched, 0.04 s at 4.25e13:576) and
takes the pool from the `plan pool` line; heap = pool + 512 MiB (`MNRUN_PLAN_POOL=0` skips; other commands keep 8192).
`binsplit_shmem_pool_rule` defaults to auto; with the pool in the library heap and a launch heap variable set, a heap below
pool + 512 MiB stops every rank before `shmem_init` with rc 8 (`EC_RC_BUDGET`) naming the variable and sizes (verified: heap 768M
vs need 1280M, rc 8 in 8.8 s). Measured on SOS 2 real nodes: 1e9 identical (host and device heap) and 1e10 identical with the
pool from the plan. 1e10 on 4 processes **failed** on 0b07ebc (pool 4608 enough in bytes but its one first-fit area fragmented
with four APU threads allocating out of step) and passed after the fix (8f3b136, `COMM_SHMEM_STAGE_SLOTS`, default 1): staging in
four regions at the pool's end, one per APU, each the per-APU need + 1 MiB (`COMM_SHMEM_STAGE_SLOT_MB`), fallback to the general
area. Memory placement only; sizes and law unchanged.

**Task 2 (C1): `COMM_SHMEM_ROUND_MB=m` (off): all-to-alls in rounds; bit-identical at every size tested; removes the second copy.**
Chosen over pool-resident caller buffers (b) because it is local to `comm_shmem.c` (TCP and SHMEM keep one layout; arena and
callers unchanged). Design: a pair (s->r) of b bytes goes in K = ceil(b/c) rounds, c = m/(n-1) (256-aligned, >= 4 KiB); both
ends derive K from b; control-word sequences are **per pair** (`oseq[r]`, `iseq[r]`), so rounded and unrounded members
interoperate; rounds run in a helper thread on the communicator's HIP stream (call returns at once, `wait` joins). Staging per
round <= m each way: pool staging <= 4 x 2m. Under the switch every all-to-all takes the rounds' path (0b07ebc), which also
stops staging the self slab (half the staging at 2 PEs). Not covered: `alltoallv_host`, all-gathers (small).

**Measured (m, SOS, no pool/heap set by hand).**

| run | pool MiB plan -> peak | host HWM GB | node MemAvailable min GB | total s |
|---|---|---|---|---|
| 1e9, 2 nodes, default | 1280 -> 849 | 19.8 | 396.3 | 177.35 |
| 1e9, 2 nodes, `ROUND_MB=64` | 768 -> 425 | 19.3 | 396.8 | 177.39 |
| 1e10, 2 nodes, default | 8704 -> 8194 | 27.8 | 351.2 | 2089.83 (bs 403.1, dm 1628.5) |
| 1e10, 2 nodes, `ROUND_MB=256` | 2560 -> **2050** | 21.3 | 357.8 | **2072.91** (396.9 / 1620.3) |
| 1e10, 4 proc, default (8f3b136) | 4608 -> 4244 | 23.3 | 205.1 | 381.16 |
| 1e10, 4 proc, `ROUND_MB=256` | 2560 -> **2054** | 21.2 | 213.5 | 357.90 |

All identical, VERIFY OK; device total unchanged (186.7 GB at 2 nodes). Summed all-to-all time 7990.3 -> 7677.8 s (-4 %) at 2
nodes, 1026.5 -> 936.8 s (-9 %) at 4 processes: rounds are not slower (one run each, within noise). `t_comm` with `ROUND_MB`
1 / 0.001 (4 KiB chunks) / quiet / serial VERIFY OK. Law matches every point to 0.01 %.
Target (mod, `mem_model.py --pool`, GB per node): `COMM_SHMEM_ROUND_MB=1024` alone pool 9.8, node 452.9 (P214's figure); with
`DM_TIGHT` 429.4; with `MN_T_CHUNK_MB=1024` 424.4; with both 399.1 (the Phase 14 defaults plus the switch; defaults without the
switch 434.3). Plan `MN_PLAN_ONLY` 4.25e13:576: 43008 -> 9472 MiB. **Recommended for the target: 1024; adoption is the user's.**

**Open.** (1) 2-node re-check on 8f3b136 (b1-b3 used 6e92c74 / 0b07ebc without regions; b5 checked regions at 4 processes).
(2) `t_bs` segfault (rc 139, "P, Q vs the GMP-free reference recursion") in `mnaccept --only unit,e9,mn` on b4: same on the
integrator's `ntt-def14` and V2's branch, so it predates V1 (Phase 14 defaults). (3) The rounds' law is an upper bound (side =
(g-1)/g of the whole, bounded by m), exact at measured points. (4) The 1e11 reference did not exist; 1e9/1e10 references used.
